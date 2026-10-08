#include "rtthread.h"
#include "rtdevice.h"
#include <string.h>
#include "bf0_hal.h"
#include "bf0_hal_pmu.h"        /* HAL_PMU_ConfigPeriLdo / PMU_PERI_LDO3_3V3 (IMU 3.3V LDO) */
#include "bf0_pm.h"
#include "ble_app.h"
#include "bsp_board.h"
#include "lsm6ds3.h"
#include "battery_calculator.h"
#include "siq02fvs3.h"
#include "key_config.h"
#include "ble_hid.h"
#include "charge.h"             /* 内置充电器: rt_charge_get_detect/full_status (充电指示) */

#include "led.h"
#include "ws2812b.h"
#include "power.h"
#include "bt_slot.h"
#include "drivers/pm.h"          /* pm_policy_t / PM_SLEEP_MODE_xxx (覆盖 SDK 默认 pm_policy) */
#include "hpsys_rcc.h"          /* HPSYS_RCC_DBGR_FORCE_HP / hwp_hpsys_rcc (WFI 降频管理) */

/* SF32LB52X 的 DEEP 睡眠隔离 PAD，GPIO 无法唤醒；LIGHT 睡眠不隔离 PAD，GPIO 保留供电。
 * 覆盖 sifli_light_handler 后 GPIO1_IRQn 在 LIGHT 中可唤醒。活跃态默认策略只到 LIGHT：
 * - LIGHT 门限 2ms（对齐 RT_PM_TICKLESS_THRESH），使 50ms 周期内 ~96% 空闲进 LIGHT(WFI 4MHz)
 * - 真正的 DEEP(0.2mA) 由 power.c 待机流程在断连 60s 后显式注册 g_standby_deep_policy 触发，
 *   走完整停 BT/USB/PSRAM 低功耗流程；绝不在此放 DEEP 条目，否则连接态会误入“部分 DEEP”
 *   （未停 BT/PSRAM）到不了 0.2mA，还可能扰动链路。 */
const pm_policy_t pm_policy[] =
{
    {2,     PM_SLEEP_MODE_LIGHT},   /* ≥2ms→LIGHT, 按键可唤醒 */
};

extern rt_mailbox_t g_button_event_mb;

/* 电池曲线表 (来自 xiaozhi-sf32-main) */
extern const battery_lookup_point_t discharge_curve_table[];
extern const battery_lookup_point_t charging_curve_table[];
extern const uint32_t discharge_curve_table_size;
extern const uint32_t charging_curve_table_size;

#define BAT_ADC_CHANNEL     7
/* PA44: 板级 Type-C 检测脚。10-05 方案B 后**不再用于软件检测**（判据已改为充电器
 * VBUS_RDY）；这里仅保留常量供 ota_flash_task() 的历史参数使用（该参数当前未使用）。 */
#define CHARGE_DETECT_PIN   44
#define LOW_BATTERY_THRESHOLD 10

static void battery_check(void);

volatile int usb_on;  /* USB 控制器是否已 initialize；插入边沿时若仍为 1，说明上次拔掉未正常停用，将强制重启确保重新枚举 */

/* USB 的 48MHz 时钟源是 DLL2 (板级 bsp_init.c: CSR |= SEL_USBC)。
 * PM 在 PM_RUN_MODE_MEDIUM_SPEED 会调用 HAL_RCC_HCPU_DisableDLL2()，
 * 一旦音频等场景结束、且无其它活动场景，系统掉到 MEDIUM 会直接关掉 DLL2，
 * USB 立即失去时钟而静默挂死(表现为“语音/音频会话后 COM 口打不开”，需重插拔恢复)。
 * 因此在 USB 连接期间持有一个 PM 场景，强制系统停留在 HIGH_SPEED(DLL2 常开)，
 * 阻止 PM 关闭 USB 的时钟源；Type-C 拔出(usb_stop)时释放。 */
static int usb_pm_held;  /* 是否已通过 PM 场景保持 DLL2(USB 时钟源)常开 */

/* 开机一次性：注册 USB CDC 接口/端点 + 启动 OTA 刷写线程。
 * 之后插拔 Type-C 只调用 cdc_acm_enable / cdc_acm_disable 启停控制器，
 * 不再重复注册，避免多次拔插后 USB 描述符错乱、无法识别。*/
static void ota_thread_entry(void *param)
{
    extern void ota_flash_task(int typec_pin);
    ota_flash_task(CHARGE_DETECT_PIN);
}

static void usb_init_once(void)
{
    extern void cdc_acm_init(uint8_t busid, uintptr_t base);
    cdc_acm_init(0, (uintptr_t)USBC_BASE);

    rt_thread_t ota_tid = rt_thread_create("ota", ota_thread_entry, RT_NULL,
                                           8192, 20, 10);
    if (ota_tid)
        rt_thread_startup(ota_tid);
}

static void usb_start(void)
{
    rt_kprintf("[USB] Type-C detected, (re)starting USB...\r\n");
    extern void cdc_acm_disable(void);
    extern void cdc_acm_enable(void);
    if (usb_on) {
        /* 之前未正常停用（如拔掉时检测漏触发），强制先停再启，
           避免“插上后控制器未重新 initialize / 重复 initialize 导致状态混乱”，
           从而解决“拔了再插上读取不到串口”。 */
        cdc_acm_disable();
        rt_thread_mdelay(20);
    }
    HAL_RCC_EnableModule(RCC_MOD_USBC);
    cdc_acm_enable();
    usb_on = 1;

    /* 保持 DLL2(USB 时钟源)常开：持有 UI 场景使 PM 不降到 MEDIUM_SPEED，
     * 从而不会 DisableDLL2 让 USB 失钟。仅在第一次插入时持有，避免重复调用。 */
    if (!usb_pm_held) {
        pm_scenario_start(PM_SCENARIO_UI);
        rt_pm_request(PM_SLEEP_MODE_IDLE);  /* USB 插入期间钉 IDLE：HCPU 时钟常开，
                                               HUSB 控制器才能维持 CDC 通信，深睡会让 COM 口失联 */
        usb_pm_held = 1;
        /* USB 在线期间 WFI 不宜降到 4MHz（CDC 时延敏感），保持 SDK 默认 ~20MHz(div 12) */
        HAL_RCC_HCPU_SetDeepWFIDiv(12, 0, 1);
    }
}

void usb_stop(void)
{
    if (!usb_on) return;
    usb_on = 0;
    rt_kprintf("[USB] Type-C removed, stopping USB...\r\n");
    extern void cdc_acm_disable(void);
    cdc_acm_disable();
    HAL_RCC_DisableModule(RCC_MOD_USBC);
    /* 释放 UI 场景，允许 PM 在无其它活动时正常掉速省电(此时 USB 已停用，DLL2 关掉无碍) */
    if (usb_pm_held) {
        pm_scenario_stop(PM_SCENARIO_UI);
        rt_pm_release(PM_SLEEP_MODE_IDLE);  /* 释放 IDLE 锁，USB 已停用可正常深睡省电 */
        usb_pm_held = 0;
        /* USB 已拔：恢复空闲 WFI 降到 4MHz(div 60) 省电，并清 FORCE_HP(仅音频场景用) */
        HAL_RCC_HCPU_SetDeepWFIDiv(60, 0, 1);
        hwp_hpsys_rcc->DBGR &= ~HPSYS_RCC_DBGR_FORCE_HP;
    }
    rt_kprintf("[USB] USB deinitialized + clock off\r\n");
}

/*==============================================================================
 * USB 插入检测：并入充电检测（方案B）—— 判据统一为内置充电器 VBUS_RDY
 *
 * 原实现：PA44 GPIO 双沿中断 + 20ms 去抖线程检测 Type-C 插拔。
 * 现实现：
 *   判据 = sifli_charge 直读 PMUC->CHG_SR 的 VBUS_RDY_OUT 位（与"充电指示"同源；
 *          该位在 AON 域常供电，充电器被 FORCE_RST 复位后依然有效 —— power.c
 *          的待机 RTC 电量检查已实证）；
 *   节拍 = 复用充电轮询的 1s 软定时器：VBUS 状态变化时 release usb_sem，
 *          由 usb_vbus_thread 执行 usb_start/usb_stop（重活不放定时器回调里，
 *          避免占住系统 timer 线程）；
 *   代价 = 插入/拔出后最多 ~1s 才启停 USB（原 PA44 约 20ms，日常无感）。
 *
 * ⚠️ 深睡唤醒：删掉 PA44 中断后，DEEP 中插 USB 只能靠充电器唤醒源
 *    （PMUC.WER_CHG 由驱动使能 + power.c 侧 HPAON_WAKEUP_SRC_PMUC 路由）。
 *    该链路受 power.h 的 USB_WAKE_BY_PMUC 开关控制，**必须真机实测**。
 *============================================================================*/
static rt_sem_t usb_sem = RT_NULL;     /* 由 charge_poll_cb 在 VBUS 状态变化时 release */
static uint8_t  g_usb_vbus_prev = 0;   /* 上一次 VBUS 状态（供 charge_poll_cb 判边沿）；
                                        * 由 usb_vbus_thread / usb_resume_if_plugged 更新 */

/* 读内置充电器 VBUS 状态：1=USB 已插入(供电有效) / 0=未插入 / -1=读取失败 */
static int usb_vbus_read(void)
{
    uint8_t detect = 0;
    if (rt_charge_get_detect_status(&detect) != RT_CHARGE_EOK)
        return -1;
    return detect ? 1 : 0;
}

/* 深睡唤醒后按真实 VBUS 状态补发 USB 启停：
 * DEEP 期间 USB 控制器断电(usb_on 仍为 1)，唤醒后若仍插着 USB 却无插拔沿，
 * CDC 不会自发重启 → 永久失联。这里按真实 VBUS 补发 usb_start/usb_stop，
 * 并同步 g_usb_vbus_prev，避免唤醒后轮询立刻再补一次启停。
 * 由 power.c::power_resume_from_deep 调用。 */
void usb_resume_if_plugged(void)
{
    int vbus = usb_vbus_read();
    if (vbus < 0)
        return;                     /* 读取失败：不动作，保持现状最安全 */
    if (vbus) {
        if (!usb_on)
            usb_start();
    } else {
        if (usb_on)
            usb_stop();
    }
    g_usb_vbus_prev = (uint8_t)vbus;
}

/* USB 启停线程：等 charge_poll_cb 报"VBUS 状态变了"→ 读硬件权威值 → 幂等启停。
 * 1s 超时兜底（漏通知/定时器异常时也能自愈）。 */
static void usb_vbus_thread(void *param)
{
    (void)param;

    int v0 = usb_vbus_read();
    g_usb_vbus_prev = (v0 > 0) ? 1 : 0;
    if (v0 > 0)
        usb_start();                /* 插着开机也可用 CDC */
    rt_kprintf("[USB] vbus thread started, vbus=%d\n", v0);

    while (1) {
        /* 阻塞等"VBUS 状态变化"通知（来自 1s 充电轮询）；1s 超时兜底 = 自带兜底轮询，
         * 即使通知链路失效也能在 1s 内把 USB 状态收敛到真实 VBUS。 */
        rt_sem_take(usb_sem, rt_tick_from_millisecond(1000));

        int vbus = usb_vbus_read();
        if (vbus < 0)
            continue;               /* 读失败：不当作"拔出"，保持现状 */

        /* 按"目标态 vs 实际态(usb_on)"收敛，而非仅看边沿 ——
         * 这样即使 usb_on 被别处改动(如待机流程 usb_stop)，下一拍也能自愈。 */
        if (vbus && !usb_on) {
            rt_kprintf("[USB] vbus inserted -> usb_start\r\n");
            usb_start();
        } else if (!vbus && usb_on) {
            rt_kprintf("[USB] vbus removed -> usb_stop\r\n");
            usb_stop();
        }
        g_usb_vbus_prev = (uint8_t)vbus;
    }
}

/* ===== 充电状态轮询 (08-26): 内置充电器状态 → LED + USB 启停 =====
 * sifli_charge 的 VBUS_RDY/EOC 事件回调跑在 PMUC 中断上下文, 且事件不连续;
 * 这里 1s 轮询 detect(插入)/full(充满), 简单可靠。
 * 状态: 无充电 -> LED1 灭; 充电中 -> LED1(PA5) 蓝灯常亮; 充满 -> LED1 灭
 *       (08-29 起充电指示收敛到蓝灯 LED1, 旧"黄慢闪/绿常亮"注释已作废)。
 * 10-05 方案B: 同一拍还负责把 VBUS 状态变化通知给 usb_vbus_thread（USB CDC 启停）。
 * 充电电流(08-27): 400mAh 电池按 0.5C 设 200mA(覆盖 SDK 默认 65mA)。每次轮询
 * 到"充电中"都重新应用 —— 深睡时充电器被 FORCE_RST 复位(CHG_CR1 配置丢失),
 * 唤醒恢复后插入充电也能保证电流正确; 纯寄存器写, 1s 一次开销可忽略。 */
#define CHG_CC_CURRENT_MA   200
static rt_timer_t g_charge_poll_timer = RT_NULL;

/* 前向声明: 充电器中断自检/复位(定义在下方, 充电轮询里要调用) */
static void charge_irq_guard(void);

static void charge_poll_cb(void *param)
{
    (void)param;
    uint8_t state = 0, detect = 0, full = 0;
    /* 08-31: 先复查充电器中断有没有被意外重新使能(OTA 重启后 EOC 中断风暴),
     * 必须在碰串口/充电状态之前做, 否则风暴期间本回调根本排不上队。 */
    charge_irq_guard();
    rt_err_t dr = rt_charge_get_detect_status(&detect);
    if (dr == RT_CHARGE_EOK && detect) {
        state = 1;   /* Type-C 已插入 */
        if (rt_charge_get_full_status(&full) == RT_CHARGE_EOK && full)
            state = 2;   /* 已充满 */
        /* 充电中: 确保 CC 电流 200mA(深睡 FORCE_RST 复位后配置丢失, 每次重应用) */
        rt_charge_set_cc_current(CHG_CC_CURRENT_MA);
    }
    /* 诊断打印(08-26 加的"未插电误报黄闪"排查)已移除(08-27): 问题定位为配置未切
     * SIFLI, 已修复; 每秒一条刷屏无意义。 */
    bt_multi_set_charge(state);

    /* 方案B: USB CDC 启停随 VBUS 状态(与充电指示同源)。
     * 只在读取成功时判边沿 —— 读失败不当作"拔出", 避免误停 CDC;
     * 真正的启停由 usb_vbus_thread 执行(重活不放在本定时器回调里)。 */
    if (dr == RT_CHARGE_EOK && usb_sem) {
        uint8_t vbus = detect ? 1 : 0;
        if (vbus != g_usb_vbus_prev)
            rt_sem_release(usb_sem);
    }
}

/* 在 charge 设备注册(INIT_DEVICE_EXPORT 的 charge_init 找 "charge" 设备)之后运行;
 * 首次查询推迟到 1s 后第一次 tick —— 确保 power_init() 里的 led_init() 已配置 LED GPIO
 * (power_init 由 app_main 手动调用, 晚于本 INIT_APP_EXPORT; 插着开机 1s 内显示即可)。 */
/* 08-31: 彻底关断充电器中断的统一实现（关 NVIC 向量 + 清 CHG_CR4 全部 IE 位 + 清挂起）。
 *
 * 为什么要清"全部"IE 位而不只是 IE_EOC？
 *   驱动 sifli_config_charge_irq() 使能了 VBUS_RDY(双边沿)/VBAT_HIGH(上升沿)/
 *   EOC(上升沿)/CV 等多个源。只清 IE_EOC，其它源仍会拉 PMUC 中断线，ISR 里
 *   `status` 寄存器照样带着 EOC 状态位，仍会走到 LOG_I("eoc: %d") 那条分支。
 *
 * 为什么要清 NVIC pending？
 *   HAL_NVIC_DisableIRQ() 只写 ICER（屏蔽），不写 ICPR（清挂起）。若在关断前
 *   中断已挂起，一旦后续任何路径重新使能 PMUC_IRQn，挂起的中断会立刻触发。
 *
 * 为什么必须连 CHG_CR4 一起清？
 *   CHG_CR4 是充电器模块内部寄存器。深睡时 power_analog_domain_off() 对充电器
 *   做 FORCE_RST，CHG_CR4 会回到复位默认值（很可能带中断使能）。若唤醒后没有
 *   恢复/重清，EOC 中断会在充满复充的临界点重新炸开。power.c 已补 CHG_CR4 的
 *   保存/恢复，这里再兜一层。 */
#define CHG_CR4_IE_ALL_Msk   (PMUC_CHG_CR4_IE_VBUS_RDY_Msk  | \
                              PMUC_CHG_CR4_IE_VBAT_HIGH_Msk | \
                              PMUC_CHG_CR4_IE_ABOVE_REP_Msk | \
                              PMUC_CHG_CR4_IE_ABOVE_CC_Msk  | \
                              PMUC_CHG_CR4_IE_CC_MODE_Msk   | \
                              PMUC_CHG_CR4_IE_CV_MODE_Msk   | \
                              PMUC_CHG_CR4_IE_EOC_Msk)
#define PMUC_NVIC_BIT        (1UL << (PMUC_IRQn & 0x1FUL))

/* 方案B(USB 插入检测并入充电检测)配套：是否保留 VBUS_RDY 中断使能。
 * VBUS_RDY 同时是充电器唤醒源(PMUC.WER_CHG)的事件源 —— 要让它能把 DEEP 唤醒，
 * 这里的 IE 位就不能清。只保留它一个，EOC 等其余源照旧全清，
 * 且 NVIC 侧 PMUC_IRQn 始终关闭 ⇒ 不会重新引入 EOC 中断风暴。 */
#if USB_WAKE_BY_PMUC
#define CHG_CR4_IE_KEEP_Msk  (PMUC_CHG_CR4_IE_VBUS_RDY_Msk)
#else
#define CHG_CR4_IE_KEEP_Msk  (0)
#endif
/* 实际要关掉的位 = 全部 IE 去掉"保留位" */
#define CHG_CR4_IE_KILL_Msk  (CHG_CR4_IE_ALL_Msk & ~CHG_CR4_IE_KEEP_Msk)

static void charge_irq_disable_all(void)
{
    HAL_NVIC_DisableIRQ(PMUC_IRQn);
    hwp_pmuc->CHG_CR4 &= ~CHG_CR4_IE_KILL_Msk;
    /* 清可能已挂起的 PMUC 中断（PMUC_IRQn=48 -> ICPR[1] bit16） */
    NVIC->ICPR[PMUC_IRQn >> 5] = PMUC_NVIC_BIT;
    __DSB();
    __ISB();
}

/* 08-31: 自检 + 条件复位。供 1s 充电轮询调用，自愈"被意外重新使能"的情况
 * (OTA 升级重启后实测 EOC 中断风暴)。先读后判：没被重新使能就一个寄存器都不写，
 * 每秒一次几乎零开销；一旦发现 IE 位或 NVIC 使能位又亮了，立刻重新关断。 */
static void charge_irq_guard(void)
{
    if ((hwp_pmuc->CHG_CR4 & CHG_CR4_IE_KILL_Msk) ||
        (NVIC->ISER[PMUC_IRQn >> 5] & PMUC_NVIC_BIT))
    {
        charge_irq_disable_all();
        rt_kprintf("[CHG] PMUC IRQ re-enabled unexpectedly, disabled again\n");
    }
}

/* 08-27: 尽早关闭充电器 PMUC 中断(INIT_DEVICE_EXPORT, 紧跟驱动 INIT_PREV_EXPORT 之后)。
 * 驱动 sifli_charge_device_init 在 INIT_PREV_EXPORT 使能 PMUC_IRQn + EOC 中断; 若此时已
 * 插电且电池在 EOC 临界, 中断风暴从启动早期就开始刷屏/拖慢系统。本函数紧随其后关掉,
 * 窗口最短。charge_led_poll_init 里保留同款关闭作双保险(深睡唤醒后异常重使能兜底)。 */
static int charge_irq_disable_early(void)
{
    charge_irq_disable_all();
    return RT_EOK;
}
INIT_DEVICE_EXPORT(charge_irq_disable_early);

static int charge_led_poll_init(void)
{
    /* 08-27: 关闭充电器 PMUC 中断(彻底) —— EOC(充满)中断在充满后复充循环里反复触发
     * (实测 4~5ms 一次刷屏 "sifli_charge ISR: eoc", 中断风暴把系统拖死)。本固件充电
     * 状态全靠 1s 轮询读 CHG_SR(detect/full), 不依赖任何充电中断, 故直接关掉整个
     * PMUC_IRQn 中断向量(比只清 IE_EOC 更彻底: 电平/边沿/触发模式异常都能兜住)。
     * 影响评估: ①VBUS_RDY/VBAT_HIGH 中断也一并关——本固件均不依赖(插拔检测走 PA44
     * GPIO, 充电状态走轮询); ②深睡插充电器唤醒走 WER_CHG 唤醒源, 不经 NVIC 中断,
     * 不受影响; ③深睡 FORCE_RST 复位充电器后 CHG_CR4 复位值默认关中断, 唤醒后
     * NVIC 复位也默认 disabled, 无需重复处理。
     * 08-31 更正: 上面第③条的"复位值默认关中断"并不成立 —— CHG_CR4 是充电器内部
     * 寄存器, FORCE_RST 后的默认值不受控; 且驱动只在 INIT_PREV 配过一次。故改为
     * power.c 里保存/恢复 CHG_CR4(见 power_analog_domain_off/on), 这里再全量清一次。 */
    charge_irq_disable_all();

    g_charge_poll_timer = rt_timer_create("chg_led", charge_poll_cb, RT_NULL,
        rt_tick_from_millisecond(1000),
        RT_TIMER_FLAG_PERIODIC | RT_TIMER_FLAG_SOFT_TIMER);
    if (g_charge_poll_timer) {
        rt_timer_start(g_charge_poll_timer);
    }
    return RT_EOK;
}
INIT_APP_EXPORT(charge_led_poll_init);

static const char *action_name(button_action_t action)
{
    switch (action)
    {
    case BUTTON_PRESSED:      return "BUTTON_PRESSED";
    case BUTTON_RELEASED:     return "BUTTON_RELEASED";
    case BUTTON_LONG_PRESSED: return "BUTTON_LONG_PRESSED";
    case BUTTON_CLICKED:      return "BUTTON_CLICKED";
    default:                  return "unknown";
    }
}

/*==============================================================================
 * 空中鼠标 (Air Mouse) — 陀螺仪积分相对位移方案
 *
 * 数据流: IMU(104Hz) → 去偏置 → 静止检测/bias跟踪 → 迟滞死区
 *         → 单级EMA → 累积 → 节流发送(8ms) → BLE HID
 *
 * 设计要点 (解决旧实现"乱飘"/"不跟手"):
 *   1. 去掉所有高频 rt_kprintf (旧代码每帧打印, 占用 ~10% CPU)
 *   2. 单级EMA滤波, 驱动层IIR已关闭 (旧双重滤波延迟>50ms)
 *   3. 双阈值迟滞死区 (旧硬死区边界抖动)
 *   4. 静止检测+滑动平均自适应零漂 (旧强制±1漂移导致bias随机游走)
 *   5. 发送由独立线程按固定节拍(10ms)刷新累积位移, 匹配BLE连接间隔, 避免超发丢帧
 *============================================================================*/

/* --- 灵敏度与位移 --- */
#define MOUSE_MAX_MOVE           100      /* 单次上报最大位移 (HID支持±127) */

/* 三档灵敏度系数：由 KEY L1 设置里的 air_mouse_speed 选择。
 * 中档沿用原有 0.0003 手感，慢档/快档等比缩放。 */
static const float MOUSE_SENS_TABLE[3] = { 0.00015f, 0.0003f, 0.0006f };
static float mouse_sens(void)
{
    uint8_t speed = key_config_get()->air_mouse_speed;
    if (speed >= 3) speed = AIR_MOUSE_SPEED_MEDIUM;
    return MOUSE_SENS_TABLE[speed];
}

/* --- 连接时校准 (成熟产品三层校准之第2层) ---
 * BLE 连接后不立即采样, 而是先等设备静止。
 * 静止检测用帧间差分 (相邻帧差值), 不受零漂影响 —
 * 即使设备零漂很大 (如2000mdps), 只要不动, 差分就接近0。
 */
#define MOUSE_CALIB_WAIT_STILL  20       /* 等待静止的连续帧数 (~0.19s) */
#define MOUSE_CALIB_FRAMES      60       /* 校准采样帧数 (~0.58s) */
#define MOUSE_CALIB_DIFF_THRESH 800      /* 帧间差分阈值 (mdps), 静止时差分<此值 */
#define MOUSE_CALIB_TIMEOUT_FRAMES 520   /* 等待静止超时(~5s@104Hz): 一直不静止也强制放行,
                                            由运行时自适应(Step2)后台修正 bias, 绝不让鼠标
                                            因校准卡死(08-26 断连重连体验修复) */

/* --- 静止检测与自适应零漂 (第3层, 运行时无感修正) --- */
/* 静止判定用【三重证据】：①平滑帧间差分小 ②去偏后速率小 ③加速度净转角小。
 * ②是①的盲区补丁: 匀速 yaw 旋转(空中鼠标左右移动)时差分≈0 且 acc 门控看不到
 * (绕重力轴旋转时重力方向不变) → 只看差分会把真实运动吸收进 bias → 停下后
 * rate 反向 → "向右移动停下后往左飘"。速率判据挡掉匀速移动(>600 冻结)。
 * ③挡非 yaw 旋转(俯仰/滚转, 重力方向偏转)。 */
#define MOUSE_STILL_DIFF_THRESH   800      /* ① 真静止平滑差分阈值 (mdps), 对齐校准层 */
#define MOUSE_SDIFF_EMA_NUM       2        /* 平滑差分EMA: (旧*3+新*2)/5, 新值权重0.4 */
#define MOUSE_BIAS_RATE_LIMIT     250      /* ② 常规收敛去偏速率上限 (mdps): 只吸收残差量级,
                                             慢速有意移动(300~600, 减速瞄准) >250 → 冻结,
                                             避免吸收运动 → 停下后反向回飘 */
#define MOUSE_BIAS_TRACK_ALPHA_STILL 0.03f /* 常规收敛系数 (三重证据齐, 96ms 确认后) */
#define MOUSE_BIAS_TRACK_LONG    0.01f     /* 长时绝对静止强制收敛 (残差>250 时 rate 判据会挡
                                             常规收敛, 此路径保证残差仍能回退) */
#define MOUSE_STILL_LONG_DIFF    200       /* 长时静止判定差分上限 (mdps): 接近绝对静止
                                             (放下设备差分<50~100); 流畅慢速移动差分 200+
                                             不会误触发, 避免把慢速运动当静止吸收 */
/* 加速度方向净转角检测(旋转 vs 静止/平移, bias 更新门控):
 * 静止时重力方向在传感器坐标中稳定; 非 yaw 旋转时持续偏转。用窗口内【净转角】
 * 分辨: 慢速旋转(≥~13°/s)8帧净转角超 1°, 手持微颤是高频来回(净转角≈0);
 * 挥动时线性加速度方向剧变同样判"旋转"→ 冻结 bias, 正确(运动时本就不该更新)。
 * 注意: 绕重力轴(yaw)旋转不改变重力方向 → 本门控盲区, 由 ② 速率判据补。 */
#define MOUSE_ACC_ROT_WIN        8        /* 净转角窗口(帧), 8帧≈77ms@104Hz */
#define MOUSE_ACC_ROT_SINSQ    0.00030f  /* 窗口净转角阈值 sin²(1°): 超过=设备在旋转 */
#define MOUSE_STILL_THRESH       1000     /* 空闲判定角速度阈值 (mdps, 去偏置后, ~1.0dps):
                                            匀速移动时 rate 大不会误判静止 */
#define MOUSE_STILL_FRAMES       10       /* 静止确认帧数 (~96ms) */
#define MOUSE_STILL_LONG_FRAMES  50       /* 长时绝对静止帧数 (~0.5s): 人手匀速移动必有抖动
                                            打断差分, 差分持续<800 达 0.5s 只有放下/真静止
                                            能做到 → 触发强制收敛(残差大也能回退) */

/* --- 双阈值迟滞死区 --- */
/* 09-01 手感微调: 1200/600 → 900/450 —— 上挥是慢速动作, 原 1.2dps 阈值
 * 把小幅度上挥吞掉(用户反馈"上有点难上"), 适度降低让慢速挥动更易激活。 */
#define MOUSE_DZ_ENTER           900      /* 进入移动阈值 (mdps, ~0.9dps) */
#define MOUSE_DZ_EXIT            450      /* 退出移动阈值 (mdps, ~0.45dps) */

/* --- EMA平滑 --- */
/* 09-01 手感微调: 0.5 → 0.45 —— 上挥幅度波动大(实测 sy -16547↔-6030), 略增平滑减跳变 */
#define MOUSE_EMA_ALPHA          0.45f    /* 新值权重, 越大越跟手但越抖 */

/* --- 轴对齐旋转角 (补偿传感器安装偏角) ---
 * IMU 物理安装轴与屏幕 X/Y 轴有夹角, 纯 dx=rate_x/dy=rate_y 会让每个方向走斜线。
 * 用 2x2 旋转矩阵把传感器角速度转到屏幕坐标系:
 *   sx = a*rate_x + b*rate_y   -> 屏幕X(左右)
 *   sy = c*rate_x + d*rate_y   -> 屏幕Y(上下)
 * 矩阵整行一起旋转 (保持上下/左右正交), 故调角度不会破坏已对准的上下方向。
 *
 * 偏角用 MOUSE_ROT_DEG 设定(度):
 *   - 上下已准说明接近 45°, 左右带对角就在此附近微调
 *   - 左右漂移【向下】 -> 试着【增大】角度 (50 / 55 / 60)
 *   - 左右漂移【向上】 -> 试着【减小】角度 (40 / 35 / 30)
 * 系数查表给出 (无需浮点库, 编译期固定), 表覆盖 30~60° 步进5°。
 * 注: 旋转后单轴幅度≈原来2倍, 若觉得太快把 MOUSE_SENS 减半。
 */
#define MOUSE_ROT_DEG   45      /* 传感器安装偏角(度), 可调范围 30~60 */

/* 旋转矩阵系数表 [a b; c d] = S*[-cosθ -sinθ; -sinθ +cosθ], S=√2≈1.4142
 * 索引 = (deg-30)/5, 顺序: 30,35,40,45,50,55,60。b 与 c 恒等 (对称)。 */
static const float g_rot_tbl[7][4] = {
    /*30*/ {-1.2247f, -0.7071f, -0.7071f,  1.2247f},
    /*35*/ {-1.1589f, -0.8111f, -0.8111f,  1.1589f},
    /*40*/ {-1.0835f, -0.9091f, -0.9091f,  1.0835f},
    /*45*/ {-1.0000f, -1.0000f, -1.0000f,  1.0000f},
    /*50*/ {-0.9091f, -1.0835f, -1.0835f,  0.9091f},
    /*55*/ {-0.8111f, -1.1589f, -1.1589f,  0.8111f},
    /*60*/ {-0.7071f, -1.2247f, -1.2247f,  0.7071f},
};
#define MOUSE_ROT_IDX  (((MOUSE_ROT_DEG) - 30) / 5)
#if (MOUSE_ROT_IDX < 0) || (MOUSE_ROT_IDX > 6)
#error "MOUSE_ROT_DEG must be in range 30..60"
#endif
/* 编译期从表取4个系数 (零运行时开销) */
static const float ROT_A = g_rot_tbl[MOUSE_ROT_IDX][0];  /* a: rate_x -> sx */
static const float ROT_B = g_rot_tbl[MOUSE_ROT_IDX][1];  /* b: rate_y -> sx */
static const float ROT_C = g_rot_tbl[MOUSE_ROT_IDX][2];  /* c: rate_x -> sy */
static const float ROT_D = g_rot_tbl[MOUSE_ROT_IDX][3];  /* d: rate_y -> sy */

/* --- 加速度计动态滚转补偿 (成熟空中鼠标的做法, 解决"握持滚转导致左右串上下") ---
 * 固定 MOUSE_ROT_DEG 只能补偿一个【固定安装角】; 但手握小设备时【滚转角每次不同】,
 * 同样的"右往左"会因为设备歪了而斜过来, 分出向下分量 -> 左右走不动+往下漂, 且时好时坏。
 *
 * 原理: 加速度计静态下测到的是重力向量。取传感器 X/Y 平面内的重力方向 (gx,gy)(单位向量),
 *   它就代表当前的"屏幕向下"方向。把陀螺角速度按此方向分解:
 *     yaw(水平) = rate_x*gx + rate_y*gy   (绕重力轴 = 屏幕左右)
 *     pitch(垂直)= -rate_x*gy + rate_y*gx (绕水平轴 = 屏幕上下)
 *   等价于"每帧用实时滚转角做旋转", 自动跟随握持姿态, 无需再调 MOUSE_ROT_DEG。
 *
 * 重力方向靠对加速度做【慢EMA】提取 (滤掉挥动时的线性加速度), 滚转本身变化很慢。
 * 若某方向反了, 翻对应符号即可 (1行改动):
 */
#define MOUSE_ROLL_COMP_EN     1        /* 1=加速度计动态滚转补偿(推荐); 0=退回固定角矩阵 */
#define MOUSE_ACC_EMA_ALPHA    0.05f    /* 加速度EMA(慢), 只取重力方向, 滤掉挥动加速度 */
#define MOUSE_ROLL_MIN_NORM2   0.25f    /* g3 XY 投影平方阈值: |g_xy|>0.5(设备倾斜>30°)才用
                                           滚转补偿; 否则(接近水平, 用户实际主姿态)用固定映射
                                           —— 水平时投影小且方向不稳定, 动态分解反而乱(09-01
                                           实测 g3z≈0.99, 投影 0.01~0.3, 方向乱跳) */
#define MOUSE_ROLL_GAIN        1.4142f  /* 恢复旧45°矩阵的√2幅度, 沿用现有死区/灵敏度手感 */
#define MOUSE_ROLL_SGN_X      (+1)      /* 左右方向: 若左右反了改成 (-1)。
                                           09-01 实测: 统一公式后左右反 → 改 +1 */
#define MOUSE_ROLL_SGN_Y      (+1)      /* 上下方向: 若上下反了改成 (-1)。
                                           09-01 pitch 改用 rate_y(实测上挥主绕Y轴)后符号重调 */
/* 光束轴指向: p = MOUSE_BEAM_SGN · (+X), 即设备 X 轴的哪一端指向屏幕。
 * 由【主姿态(平放)时 DIAG 打印的 g3 的 Z 分量符号】唯一确定:
 *     平放 g3z > 0  → -1      (本器件实测平放 g3z≈+0.9x, 取 -1)
 *     平放 g3z < 0  → +1
 * ⚠️ 取值错误只会让【上下整体反】, 左右不受影响; 若改完后 0° 主姿态上下反了,
 *    把它取反即可(1 行)。±90° 的对称性由公式本身保证, 与取值无关。 */
#define MOUSE_BEAM_SGN        (-1)
/* 注: 早期"水平固定映射"独立符号 MOUSE_FLAT_SGN_Z/X 已废弃 —— 统一公式
 * (yaw=ω·g3, pitch=rate_x) 下水平/立着符号物理一致, 不再需要第二套宏。 */

/* --- 手动空中鼠标方向档(09-01 新增后因与滚转补偿冲突被禁用) ---
 * ⚠️ MOUSE_ROLL_COMP_EN=1 的 yaw/pitch 分解具有【绕垂直轴转动不变性】:
 * 设备怎么握(侧握/绕垂直轴转 90°/倒拿) 重力投影 (gx,gy) 与角速度同步旋转,
 * 点积/叉积结果不变 → 光标自动跟手, 已完整覆盖"方向"需求。手动旋转叠加其上
 * = 自动对齐后再手动转一次(双重旋转) → 实测全乱。故默认 AIR_MOUSE_DIR_EN=0,
 * 方向完全交给滚转补偿; 若个别姿态(屏幕朝上平放, 重力 X/Y 投影 <300mg 复用
 * 上次向量)需要手动补偿, 置 1 恢复。 */
#define AIR_MOUSE_DIR_EN        0   /* 09-01 05:20: 暂禁用 —— 旋转叠加在自动姿态补偿
                                        之后=双重变换必乱(用户实测全不对), 需重新设计
                                        (补偿应作用于输入侧或与 u 公式整合, 而非输出叠加) */

/* --- 发送 --- */
/* 刷新节拍: 见 mouse_send_thread. 与 BLE 连接间隔匹配, 避免超发被协议栈合并/丢弃(掉帧感).
 * 节拍 = max(MOUSE_FLUSH_MS, 当前连接间隔+2ms): 语音期 sco_quiet 把间隔拉到 40~80ms,
 * 若按 10ms 出包会耗尽 8 包 TX 池(每连接事件才归还) → 丢帧卡顿; 自适应间隔后任何
 * 参数状态都不超发不丢帧 (08-26 根治, 取代此前的事件驱动慢节拍). */
#define MOUSE_FLUSH_MS           10       /* 正常发送刷新间隔(ms), 连接间隔≤10ms 时使用 */

/* ===========================================================================
 * 陀螺上电/唤醒过渡期 (修复"刚连蓝牙/空闲→活跃时光标跳变"的核心)
 *
 * MEMS 陀螺芯片(LSM6DS3)在使能后存在"上电零偏暂态": 头几百毫秒内读数不稳、
 * 偶有尖峰, 并非真实角速度。若直接送进积分器, 光标会瞬间跳变; 且此时
 * 帧间差分很小, 静止检测会误判"已静止"而提前完成校准 → bias 测错 →
 * 残留零偏要靠运行时极慢自适应(α=0.01)花 1~2s 才吸收, 表现为"过一两秒才正常"。
 *
 * 修复: 陀螺使能后, 丢弃随后若干过渡帧的"光标相关处理"(去偏/死区/EMA/累积/输出
 * 与校准), 仅让重力EMA继续收敛(滚转补偿需要); 待陀螺稳定后再正常积分。
 *   - 冷启动(BT刚连): 重力EMA也需从零收敛, 窗口较长 (GYRO_SETTLE_COLD)
 *   - 温唤醒(空闲/外部唤醒): 重力EMA早已收敛, 只需覆盖尖峰, 窗口较短 (GYRO_SETTLE_WARM)
 * 均为编译期常量, 可按实测暂态长度微调。
 * =========================================================================== */
#define GYRO_SETTLE_WARM   24   /* 温唤醒: ~0.23s@104Hz, 仅丢弃陀螺上电尖峰 */
#define GYRO_SETTLE_COLD   64   /* 冷启动: ~0.6s@104Hz, 另覆盖重力EMA收敛 */

/* 校准状态机 */
typedef enum {
    CALIB_IDLE = 0,      /* 未校准, bias=0, 不输出 (BLE未连接) */
    CALIB_WAIT_STILL,    /* BLE已连接, 等待设备静止才开始采样 */
    CALIB_SAMPLING,      /* 静止确认, 正在采样 (中途运动则回WAIT_STILL) */
    CALIB_DONE           /* 校准完成, 正常工作 + 运行时自适应 */
} calib_state_t;

volatile uint8_t g_key2_active = 0;        /* 空中鼠标使能: 0=关, 1=开 */

static uint8_t mouse_button = 0x00;

/* 鼠标按键状态 = 引脚电平直读（上拉输入: 按下=低电平=1, 释放=高电平=0）。
 * 不引入任何"单击/拖拽"阈值门控: 按下即 down、释放即 up, 纯 1:1 状态映射。
 * 发送线程每帧据引脚电平反推按键位; 按下但无移动时由按键事件回调(L2 分支)
 * 主动发一次 button-only 报告, 保证静止态也能即时上报。 */

/* --- 算法状态 --- */
static int32_t g_bias_x = 0, g_bias_y = 0, g_bias_z = 0; /* 零漂偏置 (mdps, z 轴 09-01 补: 水平固定映射用) */
static uint8_t g_bias_valid = 0;   /* 上次校准的 bias 是否有效(跨连接保留, 08-26):
                                      有效则 BLE 重连后直接进入工作态, 不再等待静止
                                      重新校准 —— 解决"断连重连后拿在手上鼠标用不了"
                                      (校准状态机卡在等静止, 而手在动永远不静止) */
static int32_t g_sdiff_x = 0, g_sdiff_y = 0;           /* 帧间差分 EMA 平滑值 (mdps, 抗手抖/毛刺) */
static int16_t g_acc_ring[MOUSE_ACC_ROT_WIN][3];       /* 加速度环形缓冲(原始mg), 算净转角 */
static uint8_t g_acc_ring_idx = 0;                     /* 当前写入槽 */
static bool    g_acc_ring_full = false;                /* 缓冲已填满(可算净转角) */
static float   g_ema_x = 0.0f, g_ema_y = 0.0f;         /* EMA平滑后位移 */
static float   g_pending_dx = 0.0f, g_pending_dy = 0.0f;/* 累积位移 (含小数) */
static bool    g_active_x = false, g_active_y = false;  /* 迟滞死区状态 */
static uint8_t g_still_cnt = 0;                         /* 连续静止计数 */

/* --- 校准采样缓冲 (中位数+均值, 抑制抖动异常值) --- */
static calib_state_t g_calib_state = CALIB_IDLE;
static uint16_t      g_calib_idx = 0;                      /* 采样索引 */
static uint16_t      g_wait_still_cnt = 0;                 /* 等待静止的连续帧计数 */
static uint16_t      g_wait_still_timeout = 0;             /* 等待静止总超时计数(见 MOUSE_CALIB_TIMEOUT_FRAMES) */
static int32_t       g_calib_buf_x[MOUSE_CALIB_FRAMES];
static int32_t       g_calib_buf_y[MOUSE_CALIB_FRAMES];
static int32_t       g_last_gyro_x = 0, g_last_gyro_y = 0; /* 上一帧原始值 (差分用) */
static bool          g_last_gyro_valid = false;            /* 差分是否有上一帧 */
static bool          g_imu_started = false;             /* IMU是否已启动 */
static uint16_t      g_gyro_settle = 0;                 /* 陀螺过渡期剩余丢弃帧(0=已稳定) */

/* --- 滚转补偿状态 (加速度计提取重力方向) --- */
static float         g_acc_x = 0.0f, g_acc_y = 0.0f;   /* 加速度EMA (mg), 提取重力 */
static bool          g_acc_valid = false;              /* 加速度EMA是否已初始化 */
static float         g_last_gx = 0.7071f, g_last_gy = 0.7071f; /* 上次可信重力单位向量 */

/* --- 3D 重力向量追踪 (09-01: 互补滤波, 根治"换角度方向乱") ---
 * g3 = 设备坐标系下的单位重力向量(指向地心)。每帧两步:
 *   ① 陀螺预测: dg/dt = -ω×g (小角近似), 设备任何旋转方向都跟随
 *   ② 近似静止(非旋转)时向实测重力 -acc 拉拢, 消除陀螺零漂累积/收敛初值
 * 屏幕平面内重力投影方向 = g3 的 XY 分量(单位化), 因此【任何姿态】都持续
 * 正确 —— 含设备接近水平(2D 投影幅度→0)的情况。取代旧 2D 阈值方案
 * (投影 <300mg 即复用上次旧值 → 用户换角度必乱)。 */
static float g_g3x = 0.0f, g_g3y = 0.0f, g_g3z = -1.0f; /* 初始假设平放(屏幕朝上) */
static bool  g_g3_valid = false;
#define MOUSE_G3_ACC_ALPHA  0.08f   /* 静止时向加速度测量拉拢的权重 */
#define MOUSE_G3_DT         0.0096f /* IMU 活跃帧间隔 104Hz ≈ 9.6ms */

static float   g_flat_mix = 0.0f;   /* 09-01: 已废弃(mix 切换移除), 保留声明防误引用 */

/* --- BLE 诊断快照 (09-01, 用户要求保留 IMU 诊断工具): 供 0xFF02 特征 notify 读取 --- */
static int32_t g_diag_gyro[3] = {0, 0, 0};   /* 原始角速度 (mdps) */
static float   g_diag_sx = 0.0f, g_diag_sy = 0.0f; /* 滚转补偿输出 (mdps 级) */

/* ===========================================================================
 * 连接态空闲低功耗状态机 (A/C/B 三步优化)
 *
 *  - g_imu_lp: 0=活跃(陀螺在线 104Hz), 1=空闲(陀螺关, 加速度 12.5Hz)
 *  - 活跃->空闲: 连续静止 ~1s 后关陀螺 + 释放 LIGHT(允许 DEEP) + 放宽 BLE 连接参数
 *                 + 编码器停 1ms 轮询(改引脚边沿唤醒)
 *  - 空闲->活跃: 加速度偏离静止基线(拿起/移动) 且空中鼠标开启时, 恢复陀螺 +
 *                请求 LIGHT + 收紧 BLE + 编码器恢复轮询
 *  - LIGHT 锁由本状态机独占持有(ble_app.c 连接/断开不再直接 request/release),
 *    以避免重复计数导致 DEEP 无法进入.
 * =========================================================================== */
static uint8_t g_imu_lp = 1;          /* 初始逻辑为空闲(未连时) */
static uint8_t g_light_held = 0;      /* LIGHT 锁当前是否由本状态机持有 */
static uint16_t g_idle_cnt = 0;       /* 活跃态连续静止帧计数 */
static rt_tick_t g_idle_since_tick = 0; /* 进入连接态空闲的时刻(08-27, 休眠超时计时用);
                                         * 活跃/未进入空闲时=0 */
static int32_t  g_idle_acc_base_x = 0;/* 进入空闲时的加速度基线(重力方向), 用于唤醒检测 */
static int32_t  g_idle_acc_base_y = 0;
static rt_sem_t g_mouse_sem = RT_NULL;       /* 发送线程阻塞信号量(取代 2ms 轮询) */
/* IMU 帧心跳(08-27 看门狗用): imu_drdy_cb 每帧更新(空闲 12.5Hz / 活跃 104Hz 都更新),
 * 深睡唤醒后 IMU 配置丢失不再产帧 → 心跳过期 → 发送线程判定停摆并自动重启 IMU */
static volatile rt_tick_t g_imu_last_frame_tick = 0;
/* 鼠标故障红闪状态(08-27): 看门狗判停置位 -> 按键背光红闪; IMU 帧流恢复且
 * 距判停 ≥2s(保证红闪至少可见 ~2s)后清除, 交还模式指示。断连也清除。 */
static volatile uint8_t g_mouse_err = 0;
static volatile rt_tick_t g_mouse_err_since = 0;

#define IDLE_ENTER_FRAMES     100     /* 活跃态连续静止帧数(~0.96s@104Hz)后进入空闲 */
#define IDLE_ENTER_FRAMES_ACTIVE 500  /* 空中鼠标【已激活】(g_key2_active=1)时: 连续静止
                                          ~4.8s 才自动空闲 —— 激活=用户有意使用, 短暂停顿
                                          (1s级)不误关陀螺; 确认空闲后取消激活(会话结束) */
#define IDLE_ACC_MOTION_MG    200     /* 空闲态加速度偏离基线阈值(mg), 超过视为"拿起/移动" */

/* 比较函数供 qsort 使用 */
static int cmp_int32(const void *a, const void *b)
{
    int32_t va = *(const int32_t *)a;
    int32_t vb = *(const int32_t *)b;
    return (va > vb) - (va < vb);
}

/* 快速反平方根 (Quake trick, 2次牛顿迭代). 工程未链 -lm, 用此避免 sqrtf 依赖. */
static float inv_sqrt(float x)
{
    float xhalf = 0.5f * x;
    union { float f; uint32_t i; } u;
    u.f = x;
    u.i = 0x5f3759df - (u.i >> 1);
    u.f = u.f * (1.5f - xhalf * u.f * u.f);
    u.f = u.f * (1.5f - xhalf * u.f * u.f);
    return u.f;
}

/* 进入活跃(陀螺使能)时调用: 丢弃随后若干过渡帧, 重置平滑/累积/死区/差分状态,
 * 避免陀螺上电/唤醒暂态尖峰被直接积分成光标跳变。重力EMA不在此重置(需持续收敛)。
 *   cold=1 冷启动(BT刚连, 需等重力EMA收敛); cold=0 温唤醒(重力已收敛, 仅覆盖尖峰)。 */
static void gyro_start_settle(int cold)
{
    g_gyro_settle = (uint16_t)(cold ? GYRO_SETTLE_COLD : GYRO_SETTLE_WARM);
    g_ema_x = 0.0f; g_ema_y = 0.0f;
    g_pending_dx = 0.0f; g_pending_dy = 0.0f;
    g_active_x = false; g_active_y = false;
    g_still_cnt = 0;
    g_last_gyro_valid = false;  /* 重新积累帧间差分, 防上一活跃期残留值误判静止 */
    g_sdiff_x = 0; g_sdiff_y = 0;   /* 平滑差分基准同样重置 */
    g_acc_ring_idx = 0; g_acc_ring_full = false;   /* 加速度净转角窗口重置(重新积累) */
}

/* 发送线程: 按固定节拍刷新"累积位移".
 * 关键改进(解决掉帧感):
 *  1) 累积值在 IMU 高优先级回调里只加不取; 这里用临界区原子取出,
 *     消除原"共享变量被覆盖 / 信号量堆积"导致的运动丢失或重复.
 *  2) 发送节拍由本线程自持(信号量唤醒后补眠到 10ms 边界)——104Hz 回调配
 *     10ms 门控时释放间隔恒 ~19.2ms 且抖动, 主机端帧间隔不均 = 丢帧感;
 *     线程自持后快速移动严格 10ms 一帧, 空闲无数据时阻塞在信号量(可深睡).
 *  3) 亚像素余数跨帧保留, 小幅运动也能累计输出, 不会因截断而"丢步". */
static void mouse_send_thread(void *param)
{
    extern ble_hid_env_t g_hid_env;
    float rem_x = 0.0f, rem_y = 0.0f;   /* 亚像素余数, 防丢步 */
    rt_tick_t last_send = 0;            /* 上次实际发送时刻(节拍基准) */

    while (1)
    {
        /* 未连接时 IMU 已断电(air_mouse_stop), 低频轮询允许系统进深睡 */
        if (!g_hid_env.is_connected) {
            rt_thread_mdelay(200);
            continue;
        }
        /* SCO(通话)中抑制 BLE HID 发送, 把射频让给 SCO 上行 —— 根治
         * "msbc uplink full" 刷屏(08-06 实测: sco_quiet 的 interval 放大
         * 要等手机协商 11s 才生效, 期间 BLE 15ms 高频+活跃鼠标持续发包
         * 饿死 SCO 上行 → 麦克风无声). 通话中不发任何 HID 包, 即使 BLE
         * 保持高频, 空事件占用射频极短, SCO 不受影响.
         * 【仅抑制"移动"位移】HID 键盘(C1/C2/C3 按键报告)与鼠标按钮
         * (L2/L3/EC)走独立通道不在此列 —— PTT 松开时靠键盘报告让 PC
         * 结束语音, 该通道必须保持可用(用户 08-06 明确要求).
         * 注意: ①必须在取数据之前判断 —— 数据取走后再 continue 会丢位移;
         * ②pending 位移作废(通话中移动无意义), 否则挂断瞬间一次性灌出
         * 导致光标飞移; ③IMU 104Hz 持续释放信号量使 sem_take 立即返回,
         * 此处 10ms 睡眠等待通话结束, 不会空转饿死其他任务.
         * 通话结束 g_sco_active=0 后立即恢复发包(陀螺全程未关, 零延迟跟手). */
        extern volatile uint8_t g_sco_active;
        if (g_sco_active) {
            rt_enter_critical();
            g_pending_dx = 0.0f;
            g_pending_dy = 0.0f;
            rt_exit_critical();
            rt_thread_mdelay(10);
            continue;
        }
        /* 阻塞直到有累积位移(IMU 回调有位移即释放)或断开(air_mouse_stop 释放),
         * 取代原 2ms 轮询 —— 这是连接态空闲能让 HCPU 进 LIGHT/DEEP 的关键.
         * 08-27 看门狗: 改 2s 超时(原永久阻塞), 超时即检查 IMU 心跳(见下). */
        if (g_mouse_sem)
            rt_sem_take(g_mouse_sem, rt_tick_from_millisecond(2000));
        if (!g_hid_env.is_connected)
            continue;

        /* ---- IMU 看门狗 (08-27 修复"鼠标随机失灵, 仅重连恢复") ----
         * 根因: 深睡断 HPSYS 域后 IMU(LSM6DS3)寄存器配置/INT1 中断丢失, 唤醒路径
         * (power_resume_from_deep)只重建了按键/USB/BLE, 未重建 IMU → 不再产帧 →
         * g_pending 恒 0 → g_mouse_sem 永不释放 → 本线程(原 RT_WAITING_FOREVER)
         * 永久阻塞 → 移动死, 重连(air_mouse_start→lsm_init)才恢复。I2C 总线持续
         * 卡死(长临界区/掉电瞬态)同样症状。
         * 判据: 心跳 g_imu_last_frame_tick 超 2s 未更新(空闲 12.5Hz 也更新, 不会
         * 把静止/空闲误判为停摆) → lsm_deinit + air_mouse_start 全流程重启 IMU,
         * 免重连自愈。语音期(SCO)跳过, 不扰动射频。air_mouse_start 会清
         * g_key2_active, 用户需重按 L1(与重连后行为一致)。 */
        if (!g_sco_active) {
            if (!g_imu_started) {
                /* 已连接但 IMU 未在跑(启动失败/异常): 补启动 */
                if (!g_mouse_err) { g_mouse_err = 1; g_mouse_err_since = rt_tick_get(); bt_multi_set_mouse_error(1); }
                rt_kprintf("[AirMouse] WATCHDOG: IMU not running while connected, (re)start\n");
                air_mouse_start();
                continue;
            }
            if ((rt_tick_get() - g_imu_last_frame_tick) > rt_tick_from_millisecond(2000)) {
                if (!g_mouse_err) { g_mouse_err = 1; g_mouse_err_since = rt_tick_get(); bt_multi_set_mouse_error(1); }
                rt_kprintf("[AirMouse] WATCHDOG: IMU stall (no frame >2s), restarting...\n");
                lsm_deinit();
                g_imu_started = false;
                air_mouse_start();
                continue;
            }
        }

        /* 发送节拍: 距上次发送不足节拍则补眠, 保证 BLE 侧帧间隔均匀.
         * 用 tick 差值而非取 now 为基准, 回调/调度抖动不会造成提前连发.
         * ⚠️ 节拍自适应当前 BLE 连接间隔 (08-26 根治): 语音期 sco_quiet 把间隔
         * 拉到 40~80ms, 若按固定 10ms 出包 → 8 包 TX 池(每连接事件才归还一次)
         * 80ms 即耗尽 → 每帧等池(≤30ms)超时丢帧 → "语音结束后一卡一卡"。
         * 改为 max(10ms, 间隔ms+2ms): 任何参数状态(语音/过渡/收紧/空闲)都不
         * 超发不丢帧, 不依赖参数协商事件/标志, 自动正确。 */
        int flush_ms = MOUSE_FLUSH_MS;
        {
            extern volatile uint16_t g_ble_cur_interval;   /* 当前连接间隔×1.25ms, ble_app.c 维护 */
            int intv_ms = (g_ble_cur_interval * 5 + 3) / 4;  /* ×1.25ms → ms */
            int need = intv_ms + 2;                            /* 裕量: 每连接事件约 1 帧 */
            if (need > MOUSE_FLUSH_MS)
                flush_ms = need;
        }
        if (last_send != 0) {
            rt_tick_t gap = rt_tick_get() - last_send;
            if (gap < rt_tick_from_millisecond(flush_ms))
                rt_thread_mdelay(rt_tick_from_millisecond(flush_ms) - gap);
        }
        last_send = rt_tick_get();

        /* 原子取出累积位移: IMU 回调优先级更高, 临界区保护避免取到一半 */
        rt_enter_critical();
        float dx = g_pending_dx;
        float dy = g_pending_dy;
        g_pending_dx = 0.0f;
        g_pending_dy = 0.0f;
        rt_exit_critical();

        if (dx == 0.0f && dy == 0.0f)
            continue;   /* 信号量堆积(回调释放快于消费), 数据已被前次取走 */

        /* TX 池余量自适应 (防掉帧): BLE 发送池仅 8 包, 由连接事件归还.
         * 连接间隔若 > 发送节拍(如手机协商成 15ms / SCO 期 100~200ms), 归还
         * 跟不上消耗 → 池耗尽 → sibles_write_value 返回 0 静默丢帧(HEAD 版
         * ble_hid_mouse_send 无保护). 这里在取走数据后、发送前等归还(最多
         * ~20ms)——【绝不能 continue】: 数据已从 pending 取出, continue 会
         * 丢位移; 且信号量被 IMU 104Hz 堆积时 sem_take 立即返回 → 空转循环,
         * 移动永久失灵(实测: 语音后移动几次就卡死, 按键却正常). */
        {
            extern uint8_t sibles_get_tx_pkts(void);
            uint8_t tries = 0;
            while (sibles_get_tx_pkts() <= 1 && tries < 4) {
                rt_thread_mdelay(5);      /* 等一个连接事件归还(5ms 级) */
                tries++;
            }
            /* 仍不足: 继续发送(池满时 sibles_write_value 返回 0 丢一帧),
             * 但线程不卡死, 下帧继续 — 优于"永久停发" */
        }

        /* 加入上次余数 -> 取整发送 -> 余数留待下帧 */
        dx += rem_x;
        dy += rem_y;

        /* 09-01: 手动空中鼠标方向档。⚠️ 与 MOUSE_ROLL_COMP_EN 动态滚转补偿冲突
         * (自动对齐 + 手动再旋转 = 双重旋转, 实测乱), 已由 AIR_MOUSE_DIR_EN=0 禁用。
         * 保留代码便于将来按需恢复。 */
#if AIR_MOUSE_DIR_EN
        {
            uint8_t dir = key_config_get_air_mouse_dir();
            if (dir != AIR_MOUSE_DIR_0) {
                float ndx = dx, ndy = dy;
                switch (dir) {
                case AIR_MOUSE_DIR_90:   /* 90°: (dx,dy)->(-dy,dx) */
                    ndx = -dy; ndy =  dx; break;
                case AIR_MOUSE_DIR_180:  /* 180°: (dx,dy)->(-dx,-dy), 唯一确定 */
                    ndx = -dx; ndy = -dy; break;
                case AIR_MOUSE_DIR_270:  /* 270°: (dx,dy)->(dy,-dx), 与 90° 严格反向 */
                    ndx =  dy; ndy = -dx; break;
                default: break;
                }
                dx = ndx; dy = ndy;
            }
        }
#endif

        int16_t mx = (int16_t)dx;
        int16_t my = (int16_t)dy;
        rem_x = dx - (float)mx;
        rem_y = dy - (float)my;

        if (mx >  MOUSE_MAX_MOVE) mx =  MOUSE_MAX_MOVE;
        if (mx < -MOUSE_MAX_MOVE) mx = -MOUSE_MAX_MOVE;
        if (my >  MOUSE_MAX_MOVE) my =  MOUSE_MAX_MOVE;
        if (my < -MOUSE_MAX_MOVE) my = -MOUSE_MAX_MOVE;

        if (mx != 0 || my != 0) {
            /* 每帧直接用引脚电平反推按键位(按下=低电平=1): 不信任 mouse_button 全局变量,
             * 避免按键事件在激活/I2C 阻塞期间丢失导致"左键永久卡住"。按下即 down、释放即 up,
             * 无拖拽阈值门控。静止态的 down/up 由按键事件回调主动发 button-only 报告。
             * 09-05: L2/L3 已改为可配置 —— 仅当该键仍是鼠标动作(或历史默认)时才把
             * 引脚电平计入鼠标按键位; 重映射为键盘/多媒体/无动作后, 按下不再拖拽。 */
            uint8_t btn = 0;
            {
                const key_config_t *lk = key_config_get_l2_key();
                /* 09-07: L 键手势模式不参与按住拖拽(手势=点按语义, 无按住保持)。
                 * key_config_get_l_mode(1)=L2: 仅常规(直通)模式才按引脚电平保持。 */
                if (key_config_get_l_mode(1) == 0 &&
                    (!lk || lk->action_type == KEY_ACTION_MOUSE) &&
                    rt_pin_read(BSP_KEY_L2_PIN) == 0)
                    btn |= 0x01;   /* 左键: 按下即 down */
            }
#ifdef BSP_KEY_L3_PIN
            {
                const key_config_t *lk = key_config_get_l3_key();
                if (key_config_get_l_mode(2) == 0 &&
                    (!lk || lk->action_type == KEY_ACTION_MOUSE) &&
                    rt_pin_read(BSP_KEY_L3_PIN) == 0)
                    btn |= 0x02;   /* 右键 */
            }
#endif
            {
                /* 09-05: 编码器按下可配置 —— 仅当仍是鼠标动作(默认中键)时才把
                 * 引脚电平计入鼠标按键位。 */
                const key_config_t *ek = key_config_get_ec_press_key();
                if ((!ek || ek->action_type == KEY_ACTION_MOUSE) &&
                    rt_pin_read(BSP_EC_KEY_PIN) == 0)
                    btn |= 0x04;   /* 中键 */
            }
            mouse_button = btn;
            ble_hid_mouse_move(mx, my, mouse_button);
        }
    }
}

/* ===========================================================================
 * 空闲/活跃模式切换 (连接态空闲低功耗 A/C/B 核心)
 * 仅在线程上下文调用(IMU 回调线程 / 编码器唤醒线程 / 主循环), 内部含 I2C 写,
 * 不可在 ISR 中调用.
 * =========================================================================== */
static void air_mouse_set_active(int active, int cold)
{
    if (active) {
        if (!g_imu_lp)
            return;   /* 已是活跃, 无需重复(避免重复 I2C/加锁) */
        lsm_enter_active();                 /* A: 恢复加速度+陀螺仪 104Hz */
        gyro_start_settle(cold);            /* 丢弃随后过渡帧, 抑制上电/唤醒尖峰跳变 */
        g_imu_lp = 0;
        g_idle_cnt = 0;                     /* 重置空闲计数, 避免唤醒后误触空闲 */
        g_idle_since_tick = 0;              /* 活跃: 清零休眠计时(08-27) */
        if (!g_light_held) {                /* B: 持有 LIGHT 锁, 禁止 DEEP(使用中需响应) */
            rt_pm_request(PM_SLEEP_MODE_LIGHT);
            g_light_held = 1;
        }
        ble_app_conn_param_tighten();       /* C: 收紧连接参数(低延迟) */
        enc_enter_active();                 /* 编码器恢复 1ms 轮询 */
        rt_kprintf("[AirMouse] -> ACTIVE (gyro on, LIGHT, tight BLE, enc poll)\n");
    } else {
        if (g_imu_lp)
            return;   /* 已是空闲 */
        lsm_enter_lowpower();               /* A: 关陀螺, 加速度降到 12.5Hz */
        g_imu_lp = 1;
        g_idle_since_tick = rt_tick_get();  /* 进入空闲: 记录休眠计时起点(08-27) */
        if (g_light_held) {                 /* B: 释放 LIGHT, 允许 HCPU 进 DEEP */
            rt_pm_release(PM_SLEEP_MODE_LIGHT);
            g_light_held = 0;
        }
        /* C: 进入空闲 → 放松 BLE 连接间隔(7.5~15ms → 20~40ms)削 BLE 射频基线,
         * 见 ble_app_conn_param_relax_idle(). 原 08-07 定案顾虑"拿起即用需协商
         * 1~2s 才收紧 → 移动一卡", 现已明确接受该延迟(用户确认要压连接态功耗),
         * 且拿起由本函数 active 分支立刻 tighten 触发协商, 1~2s 内恢复跟手.
         * IMU 降频(陀螺关+12.5Hz)+释放 LIGHT 锁 + BLE 放松 共同承担空闲省电. */
        ble_app_conn_param_relax_idle();
        enc_enter_idle();                   /* 编码器停轮询, 改引脚边沿唤醒 */
        /* 记录当前重力方向作为唤醒检测基线 */
        g_idle_acc_base_x = (int32_t)g_acc_x;
        g_idle_acc_base_y = (int32_t)g_acc_y;
        rt_kprintf("[AirMouse] -> IDLE (gyro off, DEEP ok, enc wake)\n");
    }
}

/* 编码器空闲态滚动唤醒回调: 回到活跃(IMU/PM/BLE 补全; 编码器自身已重启动定时器) */
static void air_mouse_on_external_activity(void)
{
    air_mouse_set_active(1, 0);
}

/* 空中鼠标活动状态查询 (ble_app.c 用: SCO 断开/HFP 恢复时按当前活动状态
 * 决定 BLE 连接参数, 避免活跃态被无条件放宽成 20~40ms 导致掉帧) */
int air_mouse_is_active(void)
{
    return (g_imu_lp == 0);
}

/* 连接态连续空闲毫秒数(08-27, power.c 空闲超时休眠用):
 * 活跃(g_imu_lp=0)或尚未进入空闲返回 0; 空闲中返回距进入空闲的时刻。 */
uint32_t air_mouse_idle_ms(void)
{
    if (g_imu_lp != 1 || g_idle_since_tick == 0)
        return 0;
    return (uint32_t)((rt_tick_get() - g_idle_since_tick) * 1000 / RT_TICK_PER_SECOND);
}

/* ===========================================================================
 * 摇一摇 (Shake) —— 09-03
 * 上位机可开关 / 可选三档灵敏度 / 可自定义触发的快捷键(键盘组合键或多媒体键)。
 * 触发前提: BLE 连接 + 【空中鼠标未激活】(g_key2_active=0) + 非语音(SCO 期)。
 *
 * ⚠️ 设计要点(改动前务必读完, 每一条都踩过或推演过的坑):
 *  演进史(踩坑记录, 勿退回旧法):
 *  - v1 "|a|−1g + 回落峰计数": 方向耦合(同向 dev≈a / 垂直 dev≈a²/2g, 差 3.6 倍)
 *    → 拿起放下(沿重力冲击)被放大误触; 且峰命中依赖单帧采到峰值, 12.5Hz 欠采样
 *    下"摇几下才触发"随机(用户实测: 时灵时不灵)。
 *  - v2 "手势内累计超阈时长": 高通去重力消掉方向耦合✓, 但累计判据不需要振荡
 *    特征, 一次猛冲击也凑得够 → 误触更重(用户实测)。
 *  - v3 "方向翻转计数": 显式数来回次数、抗误触最好, 但状态机(方向基准跟随/
 *    need_quiet/窗口超时)过度设计 → 偶发不灵; 且触发后 need_quiet 要等出现
 *    完整停顿才解除, 叠加 1.2s 冷却 → 用户实测"第一次能触发, 之后很难再唤醒"。
 *  - v4 "连续超阈累计满阈值时长": 强度门槛单帧即够, 但空闲 12.5Hz 采样 dt≈80ms≈
 *    触发时长 → 实际是"单帧超阈即触发", 全靠采样点碰巧落在高加速度相位 —— 快速
 *    小幅摇动的动态加速度峰值常 <1g, 采样命中率低 → 用户实测"要大力挥才触发"。
 *  - v5 "窗口内 ≥2 次超阈脉冲": 无需大力✓, 但判据不含振荡方向特征 —— 走路
 *    (每步一次冲击)/连续敲击等【同向】重复脉冲在窗口内也能凑满 2 次 → 太易误触
 *    (用户实测)。
 *  - v6(首版, 已否)= 方向交替 + pulses=3: 治"走路/敲击同向误触"✓, 但需摇满
 *    1.5 次完整往复, 每次超阈还得命中空闲 12.5Hz 采样相位 -> 触发率过低, 用户
 *    实测"摇不出来"(与 v4"要大力"同源的采样命中问题)。
 *  - v6.1= 方向交替判定保留(抗误触内核), 脉冲数/阈值/窗口回 v5 实测面:
 *    2 脉冲 = 摇一个完整往复即触发, 阈值 400/650/950, 窗口 600ms。走路/敲击等
 *    【同向】重复仍只计 1 次、永不凑满 -> 误触不回潮。去重力/更新时机/抑制等
 *    设计(①③④⑤⑥)不变, 仅判据②的"有效脉冲数/阈值/窗口"调整。
 *  - v7(现行, 因"效果不理想/容易误触"重做)= 用户实测 v6.1 在【拿起/放下设备】
 *    时仍会误触。根因: v6.1 判据只验【方向相反】+【数量够】, 它数的是"某一帧
 *    碰巧超阈"的上升沿, 拿到的是【采样那一刻的瞬时值】—— 而"拿起/放下"物理上
 *    就是一个半正弦(抬起时正向加速→手停住时反向减速), 天然自带一对反向冲击,
 *    于是"一次拿起"就够 2 脉冲 ⇒ 触发。
 *    ⇒ 改为【真峰值检测 + 四重校验】: ①取【整段摆动的真实峰值幅度】(而非某帧瞬时值)
 *    ②方向相反 ③【幅度一致】(来回幅度相当; 落地/放桌是衰减、突袭冲击是突增)
 *    ④【间隔合理】(相邻峰 ≥90ms, 滤掉同一冲击的 ringing/回弹)。
 *    ★ 脉冲数【仍保持 2】(仿真标定, 别想当然改成 3): 改成 3 虽让拿起/放下更
 *      不可能触发, 但真摇触发率从 55~70% 掉到 37~48% ⇒ 用户反馈"摇不出来"。
 *      抗误触靠上面三条校验, 不是靠加脉冲数。窗口 600→900ms 配合真峰值判定。
 *    关键区别: v6.1 "两个反向瞬时超阈"就触发(一次拿起+回弹就够了);
 *    v7 要求【方向相反 + 幅度相当 + 间隔合理】的一对峰 —— 半正弦型的一次性
 *    拿起/放下降不合规, 而真正的来回振荡合规 ⇒ 这才是"真的摇一摇"。
 *  ① 为什么必须去重力(高通): 直接判含重力的 |a|−1g 时, 甩动加速度与重力同向/
 *     垂直的响应差 3.6 倍(v1 教训)。先每轴一阶低通(α=dt/τ, τ=SHAKE_LP_TAU_MS)
 *     估重力分量并减掉, 得纯动态加速度 |lin| → 阈值语义=真实晃动强度, 与姿态
 *     无关; 拿起放下等慢动作被低通吸收, 贡献不到 |lin|。
 *  ② 判据(v7)= 真正的【摇一摇手势】= 一段规律往复振荡。实现:
 *     (a) 峰值检测: 超阈后进入"摆动中", 持续记录 mag 极大值与该时刻的 lin 矢量;
 *         当 mag 回落到峰值的 SHAKE_PEAK_REL(30%) 以下, 或该次摆动持续超
 *         SHAKE_PEAK_MAX_MS(300ms, 兜住采样稀疏/持续推压), 才【提交一个峰】——
 *         拿到真实峰值幅度, 而非"某一帧超阈"(v6.1 老办法对采样相位敏感)。
 *     ★ 回落比 30% 是"一次冲击"与"真实往复"的分界: 拿起/放下的半正弦幅度一路
 *       衰减到底, 提峰时 mag 已远低于峰值 ⇒ 只出 1 峰; 而摇一摇来回振荡, 每次
 *       到反向顶点前 mag 会明显回落 ⇒ 稳定出多峰。
 *     (b) 峰序列校验: ①首峰直接记; ②后续峰须与上一有效峰【方向相反】(点积<0);
 *         ③【幅度一致】: 峰值比 r 须落在 [1/AMP_LO, AMP_HI](0.4~2.5) —— 真摇动
 *         来回幅度相当, 而"落地/放桌回弹"衰减(r≪)、"突然冲击"突增(r≫), 皆排除;
 *         ④【间隔合理】: 相邻峰间隔 ≥ MIN_GAP_MS(90ms), 滤 ringing/回弹抖动。
 *     (c) 窗口: 首峰起 SHAKE_WINDOW_MS(900ms) 内凑满 SHAKE_PULSES(2) 个
 *         【连续合规】峰即触发。任一峰不合规 → 【整段作废】(严格): 真实摇动的峰序
 *         干净连续, 中间插一个不合律的峰就说明这不是摇动。
 *     (d) 仿真基线(适中档/加噪 25mg/12.5Hz 采样, 各 300 次): 真摇 1200mg
 *         触发 55%、1500mg 触发 70%; 拿起/放下 0%; 静置纯噪声 0/300。
 *         同一仿真下 v6.1 的"放下+回弹"误触 10.5%。
 *  ③ 检测必须放在 imu_drdy_cb 的【g_gyro_settle 判断之前】。settle 期间
 *     回调直接 return, 而空闲态被唤醒时正好要丢弃若干过渡帧 —— 若检测放在其后,
 *     用户摇一摇唤起到活跃的这段时间恰好全部漏检, 表现为"要摇很久才触发"。
 *  ④ 【不主动把系统唤醒到活跃态】。v6.1 判据约需一次完整往复(空闲 12.5Hz 下
 *     2~3 帧即可攒满 2 脉冲), 唤醒只是白开陀螺仪耗电。代价: 系统已进 DEEP
 *     (IMU 断电)时摇一摇无效 —— 设备休眠中, 属可接受行为(LSM6DS3 的 wake-up
 *     中断可解, 未做)。
 *  ⑤ 【空中鼠标激活期间不触发】(用户定案)。用空中鼠标时晃动/挥动设备是常态
 *     操作(瞄准、快速转向、换姿势), 若不抑制会误触发快捷键。因此 g_key2_active=1
 *     时检测短路(不累计不触发), 与语音(SCO)抑制同级; 进入激活瞬间清残留累计。
 *  ⑥ 重力低通在【任何阶段都更新】(含冷却/抑制期): 一旦恢复判定, 低通必须已
 *     收敛到当前重力 —— 否则重力方向一变化(如拿起翻转)会被低通滞后误当"摇动"。
 *     首帧以原始值播种(valid=0 → 播种后 return, 不判定)。
 * =========================================================================== */

/* 冷却: 触发后这段时间内不再检测(也不累计), 防一次晃动连发多次。
 * 无 need_quiet(同 v4/v5/v6.1) —— 冷却一过随时可再触发, 修复 v3"第二次难唤醒"。 */
#define SHAKE_COOLDOWN_MS   1000
/* 快捷键按下保持时长: 太短主机可能采不到, 太长影响连续触发手感 */
#define SHAKE_KEY_HOLD_MS   40
/* v7 判据: 首峰起 SHAKE_WINDOW_MS 内需凑满 SHAKE_PULSES 个【连续合规】峰。
 * v6.1 是 600ms/2 脉冲; v7 放宽到 900ms 并配合"真峰值"判定, 让慢摇也能凑满
 * (峰之间留足回落时间, 900ms 足够覆盖一个完整往复)。 */
#define SHAKE_WINDOW_MS     900
/* 重力低通时间常数(ms): 高通截止≈1/(2πτ)≈0.8Hz。滤掉重力与"拿起/放下/走路"
 * 等慢动作, 只留 3~10Hz 的摇动振荡。12.5Hz 与 104Hz 经 α=dt/τ 自动归一。 */
#define SHAKE_LP_TAU_MS     200
/* ---- v7 峰值检测参数(把"某一帧超阈"升级为"真实峰值") ---- */
/* 峰值回落比: 摆动中 mag 回落到【峰值×此值】以下才算一个峰提交。
 * ★ 取 0.30(仿真标定值): 这是"一次冲击"与"真实往复"的分界 —— 拿起/放下是
 *   单个半正弦(正→负), 幅度一路衰减到底, 提峰时 mag 已远低于峰值 ⇒ 只出 1 峰;
 *   而摇一摇是来回振荡, 每次到反向顶点前 mag 会明显回落 ⇒ 稳定出多峰。
 *   取太高(如 0.55)会把真摇的半个周期也吞掉 → 漏触发(实测触发率掉到 37%)。 */
#define SHAKE_PEAK_REL      0.30f
/* 单次摆动最长持续(ms): 兜住"采样稀疏/持续单向推压"导致 mag 一直不回落、
 * 永远不提交峰的死锁。超过此时长就强制提交当前峰。 */
#define SHAKE_PEAK_MAX_MS   300
/* 相邻有效峰最小间隔(ms): 真实摇动半周期 ≥ ~90ms(2~5Hz); 小于此的多半是同一
 * 冲击的 ringing/传感器抖动(方向与幅度都"假"), 不计(避免一次冲击凑出多峰)。
 * ★ 取 90ms 是关键: 拿起/放下时的回弹与主冲击间隔往往 < 90ms ⇒ 被此门槛滤掉。 */
#define SHAKE_MIN_GAP_MS    90
/* 相邻峰【幅度一致性】窗口(倍数): r = 本次峰值/上一有效峰值, 须落在
 * [1/AMP_LO, AMP_HI] 内才算合规。真摇动来回幅度相当, 而"落地/放桌回弹"是
 * 衰减(r≪)、"突然猛击"是突增(r≫), 皆判不合规。取 2.5 是仿真标定值:
 * 2.2 对真摇偏严(1200mg 触发率仅 46%), 2.5 能把拿起/放下压到 0%。 */
#define SHAKE_AMP_LO        2.5f    /* r ≥ 1/2.5 = 0.4 */
#define SHAKE_AMP_HI        2.5f    /* r ≤ 2.5 */

typedef struct {
    int32_t  hi;       /* 触发阈值(mg): 动态加速度 |lin| > hi 进入"摆动中"。
                       * 语义=真实晃动强度, 与姿态无关(v1 教训)。 */
    uint8_t  pulses;   /* 窗口内【连续合规】峰数 ≥ 此值触发。v7: 2 = 一个完整往复 */
} shake_params_t;

/* 三档灵敏度(v7 = 真峰值 + 四重校验)。⚠️ 调参只需改这张表 + 上面几个宏。
 * 【脉冲数仍为 2】—— 这是仿真标定的结果, 别想当然改成 3:
 *   ★ 改成 3 会让"拿起/放下"几乎不可能触发(本就是目标), 但代价是真摇触发率
 *     从 55%~70% 掉到 37%~48% ⇒ 用户反馈"摇不出来"。误触要靠【峰值回落比
 *     0.30 + 最小间隔 90ms + 幅度一致性】这三条压制, 而不是靠加脉冲数。
 *   仍易误触        -> 收窄 SHAKE_AMP 容差, 或把 pulses 提到 3(牺牲触发率)。
 *   摇不出(要大力)  -> 调小 hi —— 普通小幅快摇动态峰值 ~0.5~1g, 且空闲 12.5Hz
 *                      采样要吃相位余量, 别按理论峰值卡。
 *   慢摇(峰间隔大)  -> 调大 SHAKE_WINDOW_MS。
 *   拿起/放下仍误触  -> 调大 SHAKE_MIN_GAP_MS(把回弹与主冲击隔开)或调大
 *                      SHAKE_PEAK_MAX_MS(让单次冲击更容易因超时被截断)。
 * 仿真基线(适中档/加噪25mg/12.5Hz): 真摇 1200mg 触发 55%、1500mg 触发 70%;
 * 拿起/放下 0%; 静置纯噪声 0/300。v6.1 基线则是"放下+回弹"误触 10.5%。 */
static const shake_params_t shake_sens_params[3] = {
    /* 轻摇 SHAKE_SENS_LIGHT */  {  400, 2 },
    /* 适中 SHAKE_SENS_MEDIUM */ {  650, 2 },
    /* 用力 SHAKE_SENS_STRONG */ {  950, 2 },
};

static rt_sem_t  g_shake_sem = RT_NULL;
static float     g_sh_lp_x, g_sh_lp_y, g_sh_lp_z; /* 重力低通(每轴, mg) */
static uint8_t   g_sh_lp_valid;     /* 低通已播种(首帧以原始值初始化) */
static rt_tick_t g_sh_last_tick;    /* 上一帧 tick(实测帧间隔, 折算 dt) */
/* ---- v7 峰值检测状态 ---- */
static uint8_t   g_sh_swing;        /* 是否处于"摆动中"(已超阈, 正在追踪峰值) */
static rt_tick_t g_sh_swing_tick;   /* 本次摆动开始时刻(超 PEAK_MAX_MS 强制提交) */
static float     g_sh_pk_mag;       /* 本次摆动内的 mag 极大值(真实峰值幅度) */
static float     g_sh_pk_x, g_sh_pk_y, g_sh_pk_z; /* 达峰时刻的 lin 矢量(方向鉴别用) */
static uint8_t   g_sh_pulse_cnt;    /* 窗口内已计【有效(合规)】峰数 */
static rt_tick_t g_sh_pulse_first;  /* 窗口内首个峰时刻(超 SHAKE_WINDOW_MS 清零重计) */
static float     g_sh_dir_x, g_sh_dir_y, g_sh_dir_z; /* 上一有效峰的 lin 矢量
                                     * (反向判定: 与本次峰矢量点积 <0 = 方向相反) */
static float     g_sh_pk_prev;      /* 上一有效峰的峰值幅度(幅度一致性比对) */
static rt_tick_t g_sh_peak_last;    /* 上一有效峰时刻(最小间隔判定) */
static rt_tick_t g_sh_last_trig;    /* 上次触发时刻(冷却计时) */
static uint8_t   g_sh_suppress;     /* 上帧是否处于抑制(空中鼠标激活): 抑制期清零
                                     * 峰计数与摆动状态, 防激活前攒的脉冲在鼠标一关
                                     * 就"补一发"触发 */

/* 每帧调用一次(含 settle 期间、含空闲态)。acc: 当前帧加速度(mg) */
static void shake_detect(const lsm_data3_t *acc)
{
    if (!key_config_get_shake_enabled())
        return;

    rt_tick_t now = rt_tick_get();

    /* 实测帧间隔(ms), clamp [5,120]: 空闲 12.5Hz≈80ms / 活跃 104Hz≈9.6ms。
     * dt 只用于低通 α 折算, 两档采样率共用同一套参数表。 */
    uint32_t dt = 80;
    if (g_sh_last_tick != 0) {
        uint32_t el = (uint32_t)((now - g_sh_last_tick) * 1000u / RT_TICK_PER_SECOND);
        if (el < 5)        dt = 5;
        else if (el > 120) dt = 120;
        else               dt = el;
    }
    g_sh_last_tick = now;

    /* 重力低通(每轴): LP += α·(raw−LP)。任何阶段都更新(含冷却/抑制期),
     * 保证恢复判定时低通已收敛到当前重力(设计要点⑥)。 */
    float x = (float)acc->x, y = (float)acc->y, z = (float)acc->z;
    if (!g_sh_lp_valid) {
        g_sh_lp_x = x; g_sh_lp_y = y; g_sh_lp_z = z;  /* 首帧播种, 不判 */
        g_sh_lp_valid = 1;
        return;
    }
    float alpha = (float)dt / (float)SHAKE_LP_TAU_MS;
    if (alpha > 0.45f) alpha = 0.45f;
    g_sh_lp_x += (x - g_sh_lp_x) * alpha;
    g_sh_lp_y += (y - g_sh_lp_y) * alpha;
    g_sh_lp_z += (z - g_sh_lp_z) * alpha;

    /* ⚠️ 空中鼠标【激活】期间不检测(用户定案, 设计要点⑤)。与语音(SCO)门控
     * 同级: 抑制期不累计不触发; 进入激活瞬间清残留脉冲状态, 防激活前攒的
     * 脉冲在鼠标一关就"补一发"触发。 */
    if (g_key2_active) {
        if (!g_sh_suppress) {
            g_sh_pulse_cnt = 0;
            g_sh_pulse_first = 0;
            g_sh_swing = 0;       /* 摆动状态一并清, 防残留半次摆动被误当成峰 */
            g_sh_suppress = 1;
        }
        return;
    }
    g_sh_suppress = 0;

    /* 冷却期内不检测(也不累计, 避免冷却一结束立刻被存量触发) */
    if (g_sh_last_trig != 0 &&
        (now - g_sh_last_trig) < rt_tick_from_millisecond(SHAKE_COOLDOWN_MS)) {
        g_sh_pulse_cnt = 0;      /* 冷却中的新晃动不预攒, 冷却结束须重新攒脉冲 */
        g_sh_pulse_first = 0;
        g_sh_swing = 0;
        return;
    }

    /* 语音(SCO)期间不触发: 把射频留给音频上行, 且通话中手部动作频繁易误触 */
    extern volatile uint8_t g_sco_active;
    if (g_sco_active)
        return;

    /* 动态加速度(高通)模长 |lin|, mg。重力已被低通减掉 → 物理意义=纯甩动
     * 加速度, 与姿态无关(设计要点①)。静止时≈0。 */
    float lx = x - g_sh_lp_x;
    float ly = y - g_sh_lp_y;
    float lz = z - g_sh_lp_z;
    float l2 = lx * lx + ly * ly + lz * lz;
    if (l2 < 1.0f) {
        g_sh_swing = 0;          /* 已回落到噪声级 → 摆动结束, 未达回落比即无峰 */
        return;                  /* 数值保护: 防 inv_sqrt(0) 出 NaN */
    }
    float fmag = l2 * inv_sqrt(l2);

    const shake_params_t *p = &shake_sens_params[key_config_get_shake_sens()];

    /* 窗口超时检查(先做, 与弱/强帧无关): 距首峰超过 SHAKE_WINDOW_MS 仍未凑满
     * → 清零重计。放在最前可让"超时"优先于"本帧恰好超阈"被处理, 语义更清晰。 */
    if (g_sh_pulse_first != 0 &&
        (now - g_sh_pulse_first) >= rt_tick_from_millisecond(SHAKE_WINDOW_MS)) {
        g_sh_pulse_cnt = 0;
        g_sh_pulse_first = 0;
    }

    /* ---- (a) 峰值检测: 把"某一帧超阈"升级为"追踪一次摆动的真实峰值" ----
     * v6.1 是"弱→强上升沿 = 1 个脉冲", 判据只能反映"采样那一刻碰巧多大", 对
     * 采样相位敏感; v7 进入"摆动中"后持续更新峰值, 直到回落到 55% 以下(或超时
     * 强制提交)才提交 —— 拿到的是这次摆动的真实强度。 */
    if (fmag > (float)p->hi) {
        /* 超阈: 进入/保持在"摆动中", 持续追踪最大值与达峰时刻的 lin 矢量 */
        if (!g_sh_swing) {
            g_sh_swing = 1;
            g_sh_swing_tick = now;
            g_sh_pk_mag = fmag;
            g_sh_pk_x = lx; g_sh_pk_y = ly; g_sh_pk_z = lz;
        } else if (fmag > g_sh_pk_mag) {
            g_sh_pk_mag = fmag;
            g_sh_pk_x = lx; g_sh_pk_y = ly; g_sh_pk_z = lz;
        }
        return;                   /* 还在摆动中, 未到回落比 → 本帧不提交峰 */
    }

    /* ---- 提交本次摆动的峰 ----
     * 到达这里说明 mag 已回落到 hi 以下。若回落幅度不足峰值×SHAKE_PEAK_REL
     * (或该次摆动持续超 PEAK_MAX_MS), 说明 mag 只是短暂回落/被采样稀疏拖住,
     * 这不是一次干净的峰 → 不计, 继续摆动(避免"一次冲击被拆成多峰")。 */
    if (g_sh_swing) {
        g_sh_swing = 0;
        int32_t forced = (now - g_sh_swing_tick) >=
                         rt_tick_from_millisecond(SHAKE_PEAK_MAX_MS);
        if (fmag > g_sh_pk_mag * SHAKE_PEAK_REL && !forced) {
            return;               /* 回落不够深: 只是一次摆动内的起伏, 不算峰 */
        }

        /* ---- (b) 峰序列校验: 首峰直记; 后续峰须 ①反向 ②幅度一致 ③间隔合理。
         * 任一不合规 → 整段作废(严格): 真实摇动的峰序干净连续, 中间插一个不合律
         * 的峰就说明这不是摇一摇。这正是 v7 抗"拿起放下/回弹/敲击"的关键。 */
        uint8_t ok = 1;
        if (g_sh_pulse_cnt > 0) {
            /* ① 方向相反: 摇一摇往复的两半周期 lin 矢量反向(点积 <0) */
            float d = g_sh_pk_x * g_sh_dir_x + g_sh_pk_y * g_sh_dir_y + g_sh_pk_z * g_sh_dir_z;
            /* ② 幅度一致: 峰值比 r 落在 [1/AMP_LO, AMP_HI]。真摇动来回幅度相当;
             *    "落地/放桌回弹"是衰减(r≪)、"突然猛击"是突增(r≫) → 都判不合规。 */
            float r = g_sh_pk_mag / (g_sh_pk_prev > 1.0f ? g_sh_pk_prev : 1.0f);
            /* ③ 间隔合理: 相邻峰 ≥ MIN_GAP_MS, 滤掉同一冲击的 ringing/采样抖动 */
            uint32_t gap = (uint32_t)((now - g_sh_peak_last) * 1000u / RT_TICK_PER_SECOND);
            if (d >= 0.0f)             ok = 0;
            else if (r < (1.0f / SHAKE_AMP_LO) || r > SHAKE_AMP_HI) ok = 0;
            else if (gap < SHAKE_MIN_GAP_MS) ok = 0;
        }

        if (!ok) {
            /* 不合规 → 整段作废(不保留本次峰, 严格) */
            g_sh_pulse_cnt = 0;
            g_sh_pulse_first = 0;
            return;
        }

        if (g_sh_pulse_cnt == 0) {
            g_sh_pulse_first = now;   /* 首峰开窗 */
            g_sh_pulse_cnt = 1;
        } else {
            g_sh_pulse_cnt++;
        }
        g_sh_dir_x = g_sh_pk_x; g_sh_dir_y = g_sh_pk_y; g_sh_dir_z = g_sh_pk_z;
        g_sh_pk_prev = g_sh_pk_mag;
        g_sh_peak_last = now;

        if (g_sh_pulse_cnt >= p->pulses) {
            g_sh_pulse_cnt = 0;
            g_sh_pulse_first = 0;
            g_sh_last_trig = now;   /* 冷却从本次触发起算 */
            rt_kprintf("[Shake] triggered (sens=%d)\n", key_config_get_shake_sens());
            if (g_shake_sem)
                rt_sem_release(g_shake_sem);   /* 按键在独立线程里发, 回调不阻塞 */
        }
    }
}

/* 摇一摇按键发送线程: 阻塞在信号量, 平时零开销(可随系统深睡) */
static void shake_thread(void *param)
{
    while (1)
    {
        rt_sem_take(g_shake_sem, RT_WAITING_FOREVER);

        const key_config_t *sk = key_config_get_shake_key();
        if (!sk) {
            rt_kprintf("[Shake] no key configured, ignored\n");
            continue;
        }
        if (!ble_hid_is_connected())
            continue;

        /* 与 C1/C2/C3 按键处理保持一致: 多媒体键固定 modifier=0x00 */
        uint8_t mod = (sk->action_type == KEY_ACTION_MULTIMEDIA) ? 0x00 : sk->modifier;
        ble_hid_keyboard_press_key(mod, sk->keycode);
        rt_thread_mdelay(SHAKE_KEY_HOLD_MS);
        ble_hid_keyboard_release_key(mod, sk->keycode);
    }
}

/* 连接/重连时重置摇一摇检测状态(在 air_mouse_start 里调用) */
static void shake_reset(void)
{
    g_sh_lp_valid = 0;      /* 低通重新播种(重连后设备姿态可能已变) */
    g_sh_last_tick = 0;
    g_sh_swing = 0;         /* v7: 清摆动状态(替代旧 g_sh_over) */
    g_sh_pulse_cnt = 0;
    g_sh_pulse_first = 0;
    g_sh_pk_prev = 0;       /* v7: 上一有效峰幅度/时刻一并清, 防跨连接比对 */
    g_sh_peak_last = 0;
    /* ⚠️ 冷却时刻【不清零】: 重连后立刻允许触发。清不清都行, 但保留上次时刻
     * 更安全 —— 连接瞬间的插拔抖动本就可能被判成摇晃, 沿用冷却可滤掉它。 */
}

static void imu_drdy_cb(const lsm_frame_t *frame)
{
    g_imu_last_frame_tick = rt_tick_get();   /* IMU 帧心跳: 每帧更新(08-27 看门狗用) */
    /* 鼠标故障红闪恢复(08-27): 帧流恢复(看门狗重启成功)且距判停 ≥2s 才清除,
     * 保证红闪至少可见 ~2s; 判停后若帧流未恢复, 本函数不执行, 红闪持续。 */
    if (g_mouse_err && (rt_tick_get() - g_mouse_err_since) >= rt_tick_from_millisecond(2000)) {
        g_mouse_err = 0;
        bt_multi_set_mouse_error(0);
    }
    //rt_kprintf("imu_drdy_cb\n");
    lsm_data3_t gyro_mdps;
    lsm_g_to_mdps(&frame->gyro_raw, LSM_G_FS_500DPS, &gyro_mdps);

#if MOUSE_ROLL_COMP_EN
    /* 加速度 -> mg, 慢EMA提取重力方向 (滤掉挥动时的线性加速度).
     * 每帧都更新, 不论校准状态, 保证进入工作态时重力估计已收敛. */
    {
        lsm_data3_t acc_mg;
        lsm_xl_to_mg(&frame->accel_raw, LSM_XL_FS_4G, &acc_mg);
        if (!g_acc_valid) {
            g_acc_x = (float)acc_mg.x;
            g_acc_y = (float)acc_mg.y;
            g_acc_valid = true;
        } else {
            g_acc_x = g_acc_x * (1.0f - MOUSE_ACC_EMA_ALPHA) + acc_mg.x * MOUSE_ACC_EMA_ALPHA;
            g_acc_y = g_acc_y * (1.0f - MOUSE_ACC_EMA_ALPHA) + acc_mg.y * MOUSE_ACC_EMA_ALPHA;
        }
    }
#endif

    /* ---- 加速度 3D 净转角检测 (旋转 vs 静止/平移, bias 更新门控) ----
     * 陀螺差分判据有盲区: 慢速匀速旋转时单帧差分≈0, 与静止不可分 → bias 会被
     * 真实运动污染 → 光标越用越偏(游戏场景"玩久了位置慢慢偏移")。而旋转时重力
     * 方向在传感器坐标中持续偏转, 静止/平移时稳定 → 用 MOUSE_ACC_ROT_WIN 帧
     * 窗口的【净转角】分辨(叉积近似 sinθ): 旋转净转角累计超阈值, 手持微颤是
     * 高频来回(净转角≈0), 挥动线性加速度方向剧变同样判旋转(运动时本就不该更新)。
     * 每帧都更新(含 settle 期间), 保证恢复输出时门控立即可用。 */
    lsm_data3_t acc_mg3;
    lsm_xl_to_mg(&frame->accel_raw, LSM_XL_FS_4G, &acc_mg3);
    g_acc_ring[g_acc_ring_idx][0] = (int16_t)acc_mg3.x;
    g_acc_ring[g_acc_ring_idx][1] = (int16_t)acc_mg3.y;
    g_acc_ring[g_acc_ring_idx][2] = (int16_t)acc_mg3.z;
    bool acc_rotating = false;
    if (g_acc_ring_full) {
        uint8_t prev_idx = (uint8_t)((g_acc_ring_idx + 1) % MOUSE_ACC_ROT_WIN);
        float ax = (float)g_acc_ring[prev_idx][0];
        float ay = (float)g_acc_ring[prev_idx][1];
        float az = (float)g_acc_ring[prev_idx][2];
        float bx = (float)acc_mg3.x;
        float by = (float)acc_mg3.y;
        float bz = (float)acc_mg3.z;
        float cx = ay * bz - az * by;
        float cy = az * bx - ax * bz;
        float cz = ax * by - ay * bx;
        float cross2 = cx * cx + cy * cy + cz * cz;
        float dot2   = ax * bx + ay * by + az * bz;
        /* sin²θ = |a×b|²/(|a|²|b|²) = cross2/(dot²+cross2), 无需开方 */
        float sinsq  = cross2 / (dot2 * dot2 + cross2);
        acc_rotating = (sinsq > MOUSE_ACC_ROT_SINSQ);
    }
    g_acc_ring_idx = (uint8_t)((g_acc_ring_idx + 1) % MOUSE_ACC_ROT_WIN);
    if (g_acc_ring_idx == 0) g_acc_ring_full = true;

    /* ---- 3D 重力向量追踪 (09-01: 互补滤波, 根治"换角度方向乱") ----
     * ① 陀螺预测: dg/dt = -ω×g (小角近似), 设备怎么转重力方向都跟随 ——
     *    即使屏幕平面内投影幅度≈0(设备接近水平), 方向仍被持续跟踪;
     * ② 近似静止(acc_rotating 为假)时向实测重力 -acc 拉拢, 消除陀螺零漂
     *    累积与初始误差。acc_rotating(旋转/强挥动)时只预测不校正(加速度
     *    含线性分量不可信)。每帧都执行, 不受 settle/校准状态影响。 */
    {
        float a2 = (float)acc_mg3.x * acc_mg3.x + acc_mg3.y * acc_mg3.y + acc_mg3.z * acc_mg3.z;
        if (!g_g3_valid) {
            /* 首帧直接以加速度初始化 (重力 = -acc) */
            float inv = inv_sqrt(a2);
            g_g3x = -(float)acc_mg3.x * inv;
            g_g3y = -(float)acc_mg3.y * inv;
            g_g3z = -(float)acc_mg3.z * inv;
            g_g3_valid = true;
        } else {
            /* ① 陀螺预测 (mdps -> rad/s: ×1.74532925e-5)
             * dg/dt = -ω×g (重力在世界系固定, 设备系旋转 → 表观变化率 -ω×g)。
             * ⚠️ 09-01 符号修正: 原实现写成了 +ω×g, 设备一旋转 g3 就朝错误方向
             * 漂移(直到静止被加速度拉回) → 用户换角度后滚转补偿方向错 → 乱。 */
            float wx = (float)gyro_mdps.x * 1.74532925e-5f;
            float wy = (float)gyro_mdps.y * 1.74532925e-5f;
            float wz = (float)gyro_mdps.z * 1.74532925e-5f;
            float t  = MOUSE_G3_DT;
            g_g3x += (wz * g_g3y - wy * g_g3z) * t;
            g_g3y += (wx * g_g3z - wz * g_g3x) * t;
            g_g3z += (wy * g_g3x - wx * g_g3y) * t;
            /* ② 加速度校正 (近似静止时) */
            if (!acc_rotating) {
                float inv = inv_sqrt(a2);
                float al = MOUSE_G3_ACC_ALPHA;
                g_g3x = g_g3x * (1.0f - al) - (float)acc_mg3.x * inv * al;
                g_g3y = g_g3y * (1.0f - al) - (float)acc_mg3.y * inv * al;
                g_g3z = g_g3z * (1.0f - al) - (float)acc_mg3.z * inv * al;
            }
            /* 归一化 (防积分/舍入漂移长度) */
            float n3 = inv_sqrt(g_g3x*g_g3x + g_g3y*g_g3y + g_g3z*g_g3z);
            g_g3x *= n3; g_g3y *= n3; g_g3z *= n3;
        }
    }

    /* ---- 摇一摇检测(09-03) ----
     * ⚠️ 必须放在下方 g_gyro_settle 判断【之前】: settle 期间本函数直接 return,
     * 而空闲态被唤醒时恰好要丢弃若干过渡帧, 若检测放在其后则唤醒过程中的摇晃
     * 全部漏检(表现: 要摇很久才触发)。详见 shake_detect 上方的设计要点 ③。 */
    shake_detect(&acc_mg3);

    /* ---- 陀螺过渡期: 上电/唤醒后 MEMS 零偏暂态, 丢弃光标相关处理直至稳定 ----
     * 上方重力EMA已在每帧更新(保证滚转补偿在恢复输出时已收敛);
     * 此处起的所有处理(去偏/静止检测/自适应/校准/死区/EMA/累积/发送)全部跳过,
     * 避免暂态尖峰被积分成光标跳变, 也避免暂态被误判"静止"而提前完成校准。
     * 倒计时归零后, 陀螺已稳定, 后续逻辑才真正开始工作。 */
    if (g_gyro_settle > 0) {
        g_gyro_settle--;
        return;
    }

    /* ---- 帧间差分 + EMA 平滑（校准层与运行时共用） ----
     * 单帧差分对手持微颤敏感(腕抖单帧差分常>800mdps), 用约2.5帧窗口EMA平滑:
     * 手抖高频来回分量大幅衰减, 真·慢速单向移动仍保留 → 静止判定对手持鲁棒。
     * settle 期间跳过(差分基准由 gyro_start_settle 重置, 恢复后重新积累)。 */
    int32_t diff_x = 0, diff_y = 0;
    if (g_last_gyro_valid) {
        diff_x = gyro_mdps.x - g_last_gyro_x;
        diff_y = gyro_mdps.y - g_last_gyro_y;
        if (diff_x < 0) diff_x = -diff_x;
        if (diff_y < 0) diff_y = -diff_y;
    }
    g_last_gyro_x = gyro_mdps.x;
    g_last_gyro_y = gyro_mdps.y;
    g_last_gyro_valid = true;
    g_sdiff_x = (int32_t)((g_sdiff_x * (5 - MOUSE_SDIFF_EMA_NUM) + diff_x * MOUSE_SDIFF_EMA_NUM) / 5);
    g_sdiff_y = (int32_t)((g_sdiff_y * (5 - MOUSE_SDIFF_EMA_NUM) + diff_y * MOUSE_SDIFF_EMA_NUM) / 5);

    /* ---- 校准状态机 (三层校准之第2层: 连接时静止检测再采样) ----
     * CALIB_IDLE:       BLE未连接, 不输出
     * CALIB_WAIT_STILL: BLE已连接, 等设备静止 (平滑差分<阈值持续N帧)
     * CALIB_SAMPLING:   静止确认后采样, 中途运动则丢弃重来
     * CALIB_DONE:       完成, 输出 + 运行时自适应(第3层)
     *
     * 静止检测用【平滑帧间差分】(g_sdiff): |gyro[n] - gyro[n-1]| EMA 平滑
     * 不受零漂影响 — 即使零漂2000mdps, 静止时相邻帧差分≈0; EMA 平滑使手持微颤
     * (单帧差分常>800) 也能通过静止判定 → 手持时校准也能完成, 避免 bias=0 吃
     * 出厂零偏(±2000+ mdps)导致恒定慢漂。另叠加加速度净转角门控(!acc_rotating):
     * 慢速旋转时陀螺差分≈0 会被误判静止, 旋转中采样会把真实运动写进 bias。 */
    if (g_calib_state != CALIB_DONE) {
        bool raw_still = g_last_gyro_valid && !acc_rotating &&
                         (g_sdiff_x < MOUSE_CALIB_DIFF_THRESH) &&
                         (g_sdiff_y < MOUSE_CALIB_DIFF_THRESH);

        if (g_calib_state == CALIB_WAIT_STILL) {
            if (raw_still) {
                g_wait_still_cnt++;
                if (g_wait_still_cnt >= MOUSE_CALIB_WAIT_STILL) {
                    /* 连续静止确认, 开始采样 */
                    g_calib_state = CALIB_SAMPLING;
                    g_calib_idx = 0;
                }
            } else {
                g_wait_still_cnt = 0;   /* 检测到运动, 重新等 */
                /* ⚠️ 等待静止总超时兜底(08-26): 用户拿在手上(一直在动)时永远等不到
                 * 静止 → 校准永远不完成 → 鼠标永久不可用。超时后强制放行进工作态:
                 * 有上次有效 bias 则沿用(断连温漂增量小, 足够用), 无则用 0(首次,
                 * 短暂慢漂可接受) —— 反正运行时自适应(Step2)会在用户静止的间隙
                 * 后台修正, 不再需要"连接时先静止"这层阻塞校准。 */
                if (++g_wait_still_timeout >= MOUSE_CALIB_TIMEOUT_FRAMES) {
                    g_calib_state = CALIB_DONE;
                    if (g_bias_valid) {
                        rt_kprintf("[AirMouse] calib TIMEOUT, keep old bias (%d,%d,%d) mdps\n",
                                   (int)g_bias_x, (int)g_bias_y, (int)g_bias_z);
                    } else {
                        g_bias_x = 0; g_bias_y = 0; g_bias_z = 0;
                        rt_kprintf("[AirMouse] calib TIMEOUT, bias=0 (runtime adapt will fix)\n");
                    }
                }
            }
            return;
        }

        if (g_calib_state == CALIB_SAMPLING) {
            /* 采样中途检测到运动 → 丢弃, 回等待状态 */
            if (!raw_still) {
                g_calib_state = CALIB_WAIT_STILL;
                g_wait_still_cnt = 0;
                g_calib_idx = 0;
                return;
            }
            if (g_calib_idx < MOUSE_CALIB_FRAMES) {
                g_calib_buf_x[g_calib_idx] = gyro_mdps.x;
                g_calib_buf_y[g_calib_idx] = gyro_mdps.y;
                g_calib_idx++;
                if (g_calib_idx == MOUSE_CALIB_FRAMES) {
                    /* 排序后取中间50%做均值, 抑制残余抖动 */
                    qsort(g_calib_buf_x, MOUSE_CALIB_FRAMES, sizeof(int32_t), cmp_int32);
                    qsort(g_calib_buf_y, MOUSE_CALIB_FRAMES, sizeof(int32_t), cmp_int32);
                    int32_t sum_x = 0, sum_y = 0;
                    int start = MOUSE_CALIB_FRAMES / 4;
                    int end   = MOUSE_CALIB_FRAMES * 3 / 4;
                    for (int i = start; i < end; i++) {
                        sum_x += g_calib_buf_x[i];
                        sum_y += g_calib_buf_y[i];
                    }
                    int cnt = end - start;
                    g_bias_x = sum_x / cnt;
                    g_bias_y = sum_y / cnt;
                    g_bias_valid = 1;   /* 校准完成: bias 有效, 下次重连直接沿用 */
                    g_calib_state = CALIB_DONE;
                    rt_kprintf("[AirMouse] calib done bias=(%d,%d) mdps  grav=(%d,%d) mg\n",
                               (int)g_bias_x, (int)g_bias_y,
                               (int)g_acc_x, (int)g_acc_y);
                }
            }
            return;   /* 校准期间不输出 */
        }

        /* CALIB_IDLE: BLE未连接, 不输出 */
        return;
    }

    /* ---- Step 1: 去偏置 ---- */
    int32_t rate_x = gyro_mdps.x - g_bias_x;   /* mdps */
    int32_t rate_y = gyro_mdps.y - g_bias_y;

    /* ---- Step 2: 静止检测 + 自适应零漂校准 ----
     * 不论空中鼠标开关, 只要确认静止就缓慢跟踪 bias, 补偿温漂.
     * 运动期间不更新 bias, 避免吃掉真实运动.
     *
     * 静止判定【三重证据】:
     *  ① 平滑帧间差分小 (g_sdiff < 800, 上方统一计算) —— 真静止 = 读数稳定;
     *  ② 去偏后速率小 (|rate| < MOUSE_BIAS_RATE_LIMIT=250, 残差量级);
     *  ③ 加速度净转角小 (!acc_rotating, 非 yaw 旋转检测)。
     * ②是①的盲区补丁: 匀速 yaw 旋转(左右移动)时差分≈0 且 acc 门控看不到
     * (重力方向不变) → 只看差分会把真实运动吸收进 bias → 停下后 rate 反向,
     * "向右移动停下后往左飘"。② 把常规收敛限制在残差量级(<250), 慢速有意移动
     * (300~600, 减速瞄准)也冻结, 不吸收运动。
     *
     * 分档收敛:
     *  - 短确认(96ms 差分稳定) + 速率小(<250) → 常规收敛 alpha 0.03;
     *  - 长时【接近绝对静止】(差分<200 持续 0.5s —— 只有放下设备能做到, 流畅
     *    慢速移动差分 200+ 不误触发) → 强制收敛 alpha 0.01, 残差>250 也能回退;
     *  - 旋转中 / 运动 / 慢速移动 → 冻结。
     */
    bool is_still = (g_sdiff_x < MOUSE_STILL_DIFF_THRESH) &&
                    (g_sdiff_y < MOUSE_STILL_DIFF_THRESH);
    bool rate_small = (rate_x > -MOUSE_BIAS_RATE_LIMIT && rate_x < MOUSE_BIAS_RATE_LIMIT) &&
                      (rate_y > -MOUSE_BIAS_RATE_LIMIT && rate_y < MOUSE_BIAS_RATE_LIMIT);
    /* 09-01 修复: 仅【活跃态】(g_imu_lp==0, 陀螺在线)才做 bias 校准。
     * 空闲态陀螺已关闭, gyro_mdps 是无效残留值(实测恒定 -12390 等), 若被
     * is_still(残留差分≈0)误判为静止 → bias 被无效值污染 → 恢复活跃后手停住
     * 时 rate 不为 0 → 光标持续漂("向上到顶停住后往下走")。 */
    if (g_imu_lp == 0) {
        if (acc_rotating) {
            /* 设备在旋转(含慢速, 差分盲区由速率判据兜底): 冻结 bias, 保护真实运动 */
            g_still_cnt = 0;
        } else if (is_still) {
            if (g_still_cnt < 255) g_still_cnt++;
            if (g_still_cnt >= MOUSE_STILL_FRAMES) {
                if (rate_small) {
                    /* 常规收敛: 三重证据齐 (差分稳定 + 速率小 + 未旋转) */
                    g_bias_x = (int32_t)(g_bias_x * (1.0f - MOUSE_BIAS_TRACK_ALPHA_STILL)
                                         + gyro_mdps.x * MOUSE_BIAS_TRACK_ALPHA_STILL);
                    g_bias_y = (int32_t)(g_bias_y * (1.0f - MOUSE_BIAS_TRACK_ALPHA_STILL)
                                         + gyro_mdps.y * MOUSE_BIAS_TRACK_ALPHA_STILL);
                    g_bias_z = (int32_t)(g_bias_z * (1.0f - MOUSE_BIAS_TRACK_ALPHA_STILL)
                                         + gyro_mdps.z * MOUSE_BIAS_TRACK_ALPHA_STILL);
                } else if (g_still_cnt >= MOUSE_STILL_LONG_FRAMES &&
                           g_sdiff_x < MOUSE_STILL_LONG_DIFF &&
                           g_sdiff_y < MOUSE_STILL_LONG_DIFF) {
                    /* 长时【接近绝对静止】(差分<200 持续 0.5s: 只有放下设备能做到,
                     * 流畅慢速移动差分 200+ 不会误触发) → 强制收敛, 让残差>250 也能
                     * 回退到死区以下。alpha 0.01, 放下 1~2s 残差基本归零 */
                    g_bias_x = (int32_t)(g_bias_x * (1.0f - MOUSE_BIAS_TRACK_LONG)
                                         + gyro_mdps.x * MOUSE_BIAS_TRACK_LONG);
                    g_bias_y = (int32_t)(g_bias_y * (1.0f - MOUSE_BIAS_TRACK_LONG)
                                         + gyro_mdps.y * MOUSE_BIAS_TRACK_LONG);
                    g_bias_z = (int32_t)(g_bias_z * (1.0f - MOUSE_BIAS_TRACK_LONG)
                                         + gyro_mdps.z * MOUSE_BIAS_TRACK_LONG);
                }
            }
        } else {
            g_still_cnt = 0;   /* 活跃态运动中: 清零静止计数 */
        }
    } else {
        g_still_cnt = 0;       /* 空闲态(陀螺关): 不校准, 清零 */
    }

    /* ---- 空闲/活跃状态机 (连接态空闲低功耗 A/C/B) ----
     * 校准完成(CALIB_DONE)后才允许进入空闲; 校准期间强制活跃(陀螺仪需在线). */
    if (g_calib_state == CALIB_DONE) {
        if (g_imu_lp) {
            /* 空闲态: 加速度偏离静止基线(拿起/移动)则唤醒到活跃.
             * 仅当空中鼠标开启时才值得唤醒陀螺仪; 关闭时保持空闲省电. */
            if (g_key2_active) {
                int32_t dx = (int32_t)g_acc_x - g_idle_acc_base_x;
                int32_t dy = (int32_t)g_acc_y - g_idle_acc_base_y;
                if (dx * dx + dy * dy > IDLE_ACC_MOTION_MG * IDLE_ACC_MOTION_MG)
                    air_mouse_set_active(1, 0);
            }
        } else {
            /* 活跃态: 连续静止累计到阈值则进入空闲。
             * ⚠️ 静止判定用【去偏后角速度】(idle_still)，不能用帧间差分(is_still)：
             * 匀速移动时差分≈0 会被误判"静止"→ 移动中 ~1s 误入空闲、陀螺关闭，
             * 而空闲唤醒只对姿态变化(加速度偏离基线)敏感，匀速平移加速度不变
             * 唤不醒 → "移动着移动着就不能移动了"。rate 判定下匀速移动 rate 大，
             * 不会误判；bias 残差大的静止暂时不省电，残差被 Step2 吸收(约1s)后
             * 自然可进空闲。 */
            bool idle_still = (rate_x > -MOUSE_STILL_THRESH && rate_x < MOUSE_STILL_THRESH) &&
                              (rate_y > -MOUSE_STILL_THRESH && rate_y < MOUSE_STILL_THRESH);
            /* ⚠️ SCO 语音通话中禁止进空闲: 说话时手必然静止, 若按常规 0.96s
             * 空闲判定 → 关陀螺(鼠标移动停) + 释放 LIGHT 锁(HCPU 可进 DEEP,
             * 打断 SCO 实时音频 → 麦克风无声)。通话中保持活跃, 由 SCO 断开
             * 事件后的正常流程恢复。 */
            extern volatile uint8_t g_sco_active;
            if (g_sco_active) {
                g_idle_cnt = 0;   /* 通话中不累计静止, 永不进空闲 */
            } else if (idle_still) {
                if (g_idle_cnt < 0xFFFF) g_idle_cnt++;
            } else {
                g_idle_cnt = 0;
            }
            /* 空闲判定阈值按激活状态分档: 激活态(用户显式开启空中鼠标)放大到 ~4.8s,
             * 短暂停顿不误关陀螺; 未激活保持 ~0.96s 快速省电。
             * 确认空闲后若处于激活态则取消激活标志 —— 自动空闲 = 本次会话结束,
             * 需重新按 L1 激活才再发鼠标(空闲态唤醒也受 g_key2_active 门控)。 */
            uint16_t idle_enter_frames = g_key2_active ? IDLE_ENTER_FRAMES_ACTIVE : IDLE_ENTER_FRAMES;
            if (g_idle_cnt >= idle_enter_frames) {
                if (g_key2_active) {
                    g_key2_active = 0;
                    rt_kprintf("[AirMouse] auto deactivated on idle\n");
                }
                air_mouse_set_active(0, 0);
            }
        }
    } else {
        /* 校准期间: 确保活跃(陀螺仪在线) */
        if (g_imu_lp)
            air_mouse_set_active(1, 0);
    }

    /* ---- 空中鼠标关闭: 清状态, 不发送 ---- */
    if (!g_key2_active) {
        g_ema_x = 0.0f; g_ema_y = 0.0f;
        g_pending_dx = 0.0f; g_pending_dy = 0.0f;
        g_active_x = false; g_active_y = false;
        return;
    }

    /* ---- Step 3: 轴对齐旋转 + 双阈值迟滞死区 ----
     * 把传感器角速度(rate_x,rate_y)旋转到屏幕坐标系 (sx=左右, sy=上下):
     *   纯左右移 -> 只有 sx 非零, 纯上下移 -> 只有 sy 非零, 互不串扰。
     * 死区/迟滞在旋转后的屏幕分量上判定, 避免偏角导致两轴同时超阈值。
     *
     * MOUSE_ROLL_COMP_EN=1: 用加速度计实时算滚转角(动态跟随握持姿态);
     * =0: 退回固定 MOUSE_ROT_DEG 矩阵。
     */
    float sx, sy;
#if MOUSE_ROLL_COMP_EN
    {
        /* 09-01 全姿态映射【严格推导版】—— 修复 ±90° 上下反。
         *
         * 模型: 设备 X 轴为"指向屏幕的光束"轴 p = σ·(+X), σ = MOUSE_BEAM_SGN;
         *       g3 = 单位重力向量(设备系, 指向地心), 世界向上 U = -g3;
         *       光束角速度 ṗ = ω × p; 屏幕右 = normalize(p×U), 屏幕上 = U。
         *   ① 左右 sx ∝  ṗ·(p×U)/|p×U| =  (rate_y·gy3 + rate_z·gz3)/den
         *   ② 上下 sy ∝ -ṗ·U      /|p×U| = σ·(rate_z·gy3 - rate_y·gz3)/den
         *      (sy 正 = 下 故取负号; den = |p×U| = √(gy3²+gz3²) = √(1-gx3²))
         *
         * ⚠️ 两式都【不含 rate_x】: 绕光束轴(p 所在轴)的自转不改变指向, 本就不该
         *    推动光标。旧式 pitch = ω·u (u=(gy3,(1-|g_xy|²)²)) 里的 rate_x·gy3 项
         *    正是 ±90° 上下反的根因 —— 设备滚转 ±90° 时 gy3 = ±1 变号, 该项随之
         *    反号, 而它驱动的偏偏是"不该动光标"的滚转速率 → 一侧碰巧对、另一侧
         *    必反(用户报的正是"90° 正常、-90° 上下反")。
         * ⚠️ 系数矩阵 [[gy3, gz3], [-σ·gz3, σ·gy3]]/den 是纯旋转(行列式 = σ;
         *    屏幕系 (右,下) 相对世界系 (右,上) 本就翻转了一次, σ=-1 才是对的),
         *    全姿态连续、±90° 严格镜像对称、无阈值切换、无抛物线。
         * ⚠️ 除以 den 归一化: 各姿态灵敏度恒为 1(平放 den≈1, 与旧式数值完全一致);
         *    den→0(光束指向天顶/地面, 鼠标本就不可用)时钳位, 防增益爆炸。 */
        float gy3 = g_g3y, gz3 = g_g3z;      /* 单位重力向量(指向地心) Y/Z 分量 */
        float rate_z = (float)(gyro_mdps.z - g_bias_z);
        float den2 = gy3 * gy3 + gz3 * gz3;  /* = 1-gx3² = |p×U|² */
        float inv  = (den2 > 0.04f) ? inv_sqrt(den2) : 5.0f;   /* den<0.2 钳位 */
        float yaw   = (rate_y * gy3 + rate_z * gz3) * inv;                          /* 屏幕X: +右 */
        float pitch = (float)MOUSE_BEAM_SGN * (rate_z * gy3 - rate_y * gz3) * inv;  /* 屏幕Y: +下 */
        sx = (float)MOUSE_ROLL_SGN_X * MOUSE_ROLL_GAIN * yaw;
        sy = (float)MOUSE_ROLL_SGN_Y * MOUSE_ROLL_GAIN * pitch;
    }
#else
    sx = ROT_A * rate_x + ROT_B * rate_y;  /* 屏幕X原始分量(左右) */
    sy = ROT_C * rate_x + ROT_D * rate_y;  /* 屏幕Y原始分量(上下) */
#endif

#if AIR_MOUSE_DIR_EN
    /* 09-01 手动角度档(仅 0/90 两档): 屏幕朝上【水平面内绕垂直轴转90°】时重力
     * 垂直屏幕、g3 不变 → 重力方案检测不到该旋转(物理极限, 无磁力计), 由用户
     * 手动选档补偿。对最终输出 (sx,sy) 旋转 90°。
     * 09-01 实测迭代: (-sy,sx) 左右反 → (sy,-sx) 左右对但上下反 → 当前 (sy,sx);
     * 仍不对则试 (-sy,-sx)。 */
    if (key_config_get_air_mouse_dir() == AIR_MOUSE_DIR_90) {
        float t = sx;
        sx = sy;
        sy = t;
    }
#endif

    /* 09-01 BLE 诊断快照: 每帧更新, 供 0xFF02 特征 notify 读取 */
    g_diag_gyro[0] = gyro_mdps.x;
    g_diag_gyro[1] = gyro_mdps.y;
    g_diag_gyro[2] = gyro_mdps.z;
    g_diag_sx = sx;
    g_diag_sy = sy;

    if (!g_active_x) {
        if (sx >  MOUSE_DZ_ENTER || sx < -MOUSE_DZ_ENTER) g_active_x = true;
    } else {
        if (sx <  MOUSE_DZ_EXIT && sx > -MOUSE_DZ_EXIT) g_active_x = false;
    }
    if (!g_active_y) {
        if (sy >  MOUSE_DZ_ENTER || sy < -MOUSE_DZ_ENTER) g_active_y = true;
    } else {
        if (sy <  MOUSE_DZ_EXIT && sy > -MOUSE_DZ_EXIT) g_active_y = false;
    }

    float dx, dy;
    if (g_active_x) {
        float sx_eff = (sx >= 0) ? (sx - MOUSE_DZ_EXIT) : (sx + MOUSE_DZ_EXIT);
        dx = sx_eff * mouse_sens();   /* 屏幕X (左右, +右 -左) */
    } else {
        dx = 0.0f;
    }
    if (g_active_y) {
        float sy_eff = (sy >= 0) ? (sy - MOUSE_DZ_EXIT) : (sy + MOUSE_DZ_EXIT);
        dy = sy_eff * mouse_sens();   /* 屏幕Y (上下, +下 -上) */
    } else {
        dy = 0.0f;
    }

    /* ---- Step 4: 单级 EMA 平滑 (含停位拖尾清零) ----
     * EMA alpha=0.5 在停止瞬间有存量(≈停止前的 dx): 停止后 dx=0, ema 以 1/2^n
     * 每帧衰减但每帧仍注入 pending → 几何级数把存量全部吐出 = "停下后偷偷缓慢
     * 移动一小点"(快速移动后存量可达十几像素)。当 sx/sy 均低于退出死区(输出域
     * 已判停, dx/dy 必为 0)时直接清零 EMA 存量消除拖尾——停止前的真实位移已在
     * pending 中照常发出, 总位移不受影响; 不用 is_still 判据, 避免低速移动时
     * 误清 EMA 吞掉真实运动。 */
    if (sx > -MOUSE_DZ_EXIT && sx < MOUSE_DZ_EXIT &&
        sy > -MOUSE_DZ_EXIT && sy < MOUSE_DZ_EXIT) {
        g_ema_x = 0.0f;
        g_ema_y = 0.0f;
    } else {
        g_ema_x = g_ema_x * (1.0f - MOUSE_EMA_ALPHA) + dx * MOUSE_EMA_ALPHA;
        g_ema_y = g_ema_y * (1.0f - MOUSE_EMA_ALPHA) + dy * MOUSE_EMA_ALPHA;
    }

    /* ---- Step 5: 累积位移 ----
     * 只负责累积; 发送节拍由 mouse_send_thread 统一控制(见上方).
     * 这样高优先级的 IMU 回调不会被 BLE 发送阻塞, 且累积值在发送线程中
     * 原子取出, 不会出现"共享变量被覆盖导致丢帧"的问题. */
    g_pending_dx += g_ema_x;
    g_pending_dy += g_ema_y;

    /* 通知发送线程: 有累积位移待发. 直接释放信号量(节拍由发送线程自持 10ms),
     * 取代原时间门控——104Hz 回调配 10ms 门控时释放间隔恒 ~19.2ms(每2帧1次)
     * 且受调度抖动 → 主机端帧间隔不均 = 丢帧感. 空闲态无位移 → 线程持续阻塞 → 可深睡. */
    if (g_key2_active && (g_pending_dx != 0.0f || g_pending_dy != 0.0f)
        && g_mouse_sem) {
        rt_sem_release(g_mouse_sem);
    }
}

/* 09-01 (保留, IMU 诊断工具): BLE 0xFF02 诊断特征快照。16 字节 LE:
 *   [0..5]  g3 三轴 ×1000 (int16)   3D 重力方向(设备坐标, 指向地心)
 *   [6..11] 原始角速度三轴 (int16 mdps)
 *   [12..13] sx 滚转补偿输出 (int16)
 *   [14..15] sy 滚转补偿输出 (int16)
 * 供上位机 IMU 诊断工具实时读取。 */
void imu_diag_fill(uint8_t buf[16])
{
    int16_t v[8];
    v[0] = (int16_t)(g_g3x * 1000.0f);
    v[1] = (int16_t)(g_g3y * 1000.0f);
    v[2] = (int16_t)(g_g3z * 1000.0f);
    v[3] = (int16_t)g_diag_gyro[0];
    v[4] = (int16_t)g_diag_gyro[1];
    v[5] = (int16_t)g_diag_gyro[2];
    v[6] = (int16_t)g_diag_sx;
    v[7] = (int16_t)g_diag_sy;
    memcpy(buf, v, sizeof(v));
}


/*==============================================================================
 * 空中鼠标生命周期: BLE 连接时启动 IMU, 断开时关闭 IMU
 * 避免 BT 协议栈启动阶段与 IMU I2C 读取竞争资源; 且未连接时省电.
 *============================================================================*/

/* IMU 3.3V 供电(LDO3/VOUT2)是否需要保持:
 *  - 连接态(含空闲): 需要, 空闲态加速度仍 12.5Hz + INT1 唤醒 DEEP, LDO3 不能关;
 *  - 断连态: air_mouse_stop 已 deinit IMU 且不作唤醒源, 可关 LDO3 省深睡静态电流。
 * board 层 BSP_Power_Up 每次唤醒会查询本谓词, 断连时唤醒不再重开 LDO3。 */
static volatile int g_imu_power_wanted = 1;

/* 重写 bsp_power.c 的 __WEAK 默认(默认恒返回 1), 让 IMU LDO 随连接状态开关 */
int BSP_ImuPowerWanted(void)
{
    return g_imu_power_wanted;
}

void air_mouse_start(void)
{
    if (g_imu_started) return;

    /* 先给 IMU 上电: 断连期间 LDO3 可能已被 air_mouse_stop 关闭, 且唤醒路径因
     * g_imu_power_wanted=0 不会重开; 这里显式打开并等待 IMU 上电稳定后再 I2C 初始化。 */
    g_imu_power_wanted = 1;
    HAL_PMU_ConfigPeriLdo(PMU_PERI_LDO3_3V3, true, true);
    rt_thread_mdelay(20);   /* LSM6DS3 上电启动时间(boot ~15ms), 留裕量确保 I2C 可访问 */

    /* 重置算法状态. 保留上次校准的 bias(断连期间温漂增量小): 若清零而本次校准
     * 又因手持微颤未完成, bias=0 会直接吃出厂零偏(±2000+ mdps) → 恒定慢漂.
     * 保留旧值即使校准失败也能维持上次的合理零偏.
     *
     * ⚠️ 断连重连体验修复(08-26): 有上次有效 bias 时【直接进入工作态】(CALIB_DONE),
     * 不再强制等待静止重新校准 —— 旧实现重连后进 CALIB_WAIT_STILL, 用户正拿在手上
     * 移动 → 永远等不到静止 → 校准卡死 → 鼠标用不了。bias 修正改由运行时自适应
     * (Step2 静止检测+慢速跟踪)在后台无感完成; 首次连接(无 bias)仍走完整校准。 */
    g_ema_x = 0.0f; g_ema_y = 0.0f;
    g_pending_dx = 0.0f; g_pending_dy = 0.0f;
    g_active_x = false; g_active_y = false;
    g_still_cnt = 0;
    g_calib_state = g_bias_valid ? CALIB_DONE : CALIB_WAIT_STILL;
    g_wait_still_cnt = 0;
    g_wait_still_timeout = 0;
    g_calib_idx = 0;
    g_last_gyro_valid = false;   /* 差分需要重新积累第一帧 */
    g_acc_valid = false;         /* 加速度EMA重新初始化 */
    g_acc_ring_idx = 0; g_acc_ring_full = false;   /* 加速度净转角窗口重置 */
    g_key2_active = 0;
    shake_reset();   /* 09-03: 摇一摇检测状态(重力低通/脉冲计数)清零, 防上一段连接残留 */

    if (lsm_init(NULL, imu_drdy_cb, NULL) != 0) {
        rt_kprintf("[AirMouse] lsm_init failed!\n");
        return;
    }
    g_imu_started = true;
    g_idle_cnt = 0;
    air_mouse_set_active(1, 1);   /* 校准期间强制活跃(冷启动: 陀螺在线 + 重力EMA收敛 + 持有 LIGHT + 编码器轮询) */
    rt_kprintf("[AirMouse] IMU started, waiting for still...\n");
    rt_kprintf("[AirMouse] IMU started, waiting for still...\n");
}

void air_mouse_stop(void)
{
    if (!g_imu_started) return;
    lsm_deinit();
    g_imu_started = false;
    g_key2_active = 0;
    /* 鼠标故障红闪: 断连即清除(未连时鼠标不工作属正常, 交还模式指示) */
    if (g_mouse_err) {
        g_mouse_err = 0;
        bt_multi_set_mouse_error(0);
    }
    mouse_button = 0;            /* 断连时清零鼠标按键, 重连后不再残留"按住" */
    g_calib_state = CALIB_IDLE;
    g_last_gyro_valid = false;
    /* 释放可能持有的 LIGHT 锁(空闲时可能已释放, 双重保护) */
    if (g_light_held) {
        rt_pm_release(PM_SLEEP_MODE_LIGHT);
        g_light_held = 0;
    }
    g_imu_lp = 1;   /* 逻辑回到空闲 */
    if (g_mouse_sem)
        rt_sem_release(g_mouse_sem);   /* 唤醒发送线程使其退出连接分支 */

    /* 断连：停传感器，LDO3 保持供电——实测关 LDO3 后引脚浮空反而更耗电 */
    g_imu_power_wanted = 0;
    rt_kprintf("[AirMouse] IMU stopped (BLE disconnected), LDO3 kept on\n");

    rt_kprintf("[AirMouse] IMU stopped (BLE disconnected), IMU LDO off\n");
}

/* 09-05: EC 编码器旋转的虚拟引脚号(仅走按键 mailbox 到主循环分发, 避免在
 * 编码器回调所在的系统定时器线程里做 press/delay/release)。不与真实按键
 * 引脚(24/25/27/30/31/32/38)冲突。 */
#define EC_VPIN_CW   0x70
#define EC_VPIN_CCW  0x71

/* 10-05: EC "按压滚动"(手势模式: 按住旋钮时旋转) —— 独立伪引脚 + 伪事件。
 * 与 EC_VPIN_CW/CCW 分开: 主循环据此走"按压滚动动作 + 清零 EC 按下手势状态"。
 * 0x74/0x75 与真实 pin(24~44)/C 伪(0x70..0x72)/L 伪(0x78..0x7A)/EC 单击伪(0x90)
 * 均不冲突; 伪事件 0x7E 与 C_SINGLE_EVT(0x7F) 区分。 */
#define EC_VPIN_CW_PRESS   0x74
#define EC_VPIN_CCW_PRESS  0x75
#define EC_SCROLL_EVT      0x7E

/* 10-05: 按压滚动抑制标志 —— 1 = 本次按压周期内已发生按压滚动, 单击/双击/长按
 * 一律不触发(用户需求: "只要是按压滚动了, 就把这些标志清零")。
 * 编码器定时器线程置位; 主循环读取, 并在 BSP_EC_KEY_PIN 的 PRESSED/RELEASED 复位。 */
static volatile uint8_t g_ec_scroll_hold = 0;

/* EC 编码器鼠标伪键码(与 key_config.h 的 ec_*_key 注释一致) */
#define EC_KC_WHEEL_UP    0x05
#define EC_KC_WHEEL_DOWN  0x06

/* 09-06: 按键触发空中鼠标的公共入口 —— L1 与 EC 编码器(可配置动作)复用。
 * toggle: 单击切换(含"校准未完成不允许开启"拦截); hold: 按下开/松开关。
 * 边界统一清鼠标按键位, 防残留拖拽。 */
static void air_mouse_key_toggle(void)
{
    g_key2_active = !g_key2_active;
    mouse_button = 0;
    if (g_key2_active && g_calib_state != CALIB_DONE) {
        g_key2_active = 0;   /* 校准未完成, 不允许开启 */
        ble_hid_mouse_button(0);
    } else {
        if (g_key2_active)
            air_mouse_set_active(1, 0);
        ble_hid_mouse_button(0);
    }
    rt_kprintf("[AirMouse] %s\n", g_key2_active ? "ON" : "OFF");
}

static void air_mouse_key_hold(int down)
{
    if (down) {
        if (g_calib_state == CALIB_DONE) {
            g_key2_active = 1;
            air_mouse_set_active(1, 0);
            /* 激活起点清理残留鼠标按键, 防"一直按住左键拖拽" */
            mouse_button = 0;
            ble_hid_mouse_button(0);
            rt_kprintf("[AirMouse] HOLD ON\n");
        } else {
            rt_kprintf("[AirMouse] HOLD ignored (still calibrating)\n");
        }
    } else {
        g_key2_active = 0;
        /* 关闭时强制释放鼠标按键(补发一次释放报告) */
        mouse_button = 0;
        ble_hid_mouse_button(0);
        rt_kprintf("[AirMouse] HOLD OFF\n");
    }
}

static void enc_event_handler(enc_event_t event, int32_t count)
{
    /* 10-05: 手势模式 + 按住旋钮旋转 = 【按压滚动】—— 与常规滚动分开处理:
     * ① 走独立伪引脚/伪事件, 由主循环分发按压滚动动作(ec_cw_press/ec_ccw_press);
     * ② 置抑制标志 —— 本次按压周期内不再触发单击/双击/长按(用户需求)。
     * 常规模式(ec_press_mode==0)完全不变: 旋转照旧走下方常规滚动(ec_cw/ec_ccw)。
     * ⚠️ 这里只做"读引脚 + 置标志 + 投邮箱"(定时器线程内安全); 状态清零与动作分发
     *    都交给主循环 —— 与 EC 既有约定一致(不在定时器线程里动 rt_timer/做延时)。 */
    if (key_config_get_ec_press_mode() && rt_pin_read(BSP_EC_KEY_PIN) == 0)
    {
        g_ec_scroll_hold = 1;
        rt_mb_send(g_button_event_mb,
                   ((event == ENC_EVT_CW ? EC_VPIN_CW_PRESS : EC_VPIN_CCW_PRESS) << 8)
                       | EC_SCROLL_EVT);
        return;
    }

    /* 09-05: 三手势可配置(上位机写入)。默认 MOUSE 伪键码 0x05/0x06=滚轮上/下
     * (历史行为, 直发零延迟); MOUSE 普通键位与键盘/多媒体动作经 mailbox 转
     * 主循环处理(单击需要按下/释放或保持延时, 不能在定时器线程里做)。 */
    const key_config_t *k = (event == ENC_EVT_CW) ? key_config_get_ec_cw_key()
                                                  : key_config_get_ec_ccw_key();
    uint8_t def_kc = (event == ENC_EVT_CW) ? EC_KC_WHEEL_UP : EC_KC_WHEEL_DOWN;
    uint8_t vpin   = (event == ENC_EVT_CW) ? EC_VPIN_CW : EC_VPIN_CCW;
    uint8_t act = (k && k->action_type <= KEY_ACTION_AIRMOUSE)
                  ? k->action_type : KEY_ACTION_MOUSE;
    uint8_t kc = (k && k->action_type == KEY_ACTION_MOUSE)
                 ? ((k->keycode == 0x01 || k->keycode == 0x02 || k->keycode == 0x04 ||
                     k->keycode == EC_KC_WHEEL_UP || k->keycode == EC_KC_WHEEL_DOWN)
                    ? k->keycode : def_kc)
                 : def_kc;

    if (act == KEY_ACTION_NONE)
        return;
    if (act == KEY_ACTION_MOUSE && kc >= EC_KC_WHEEL_UP) {
        /* 滚轮(默认): 保持原直发路径 —— 定时器线程内已验证安全, 且旋转无延迟 */
        if (kc == EC_KC_WHEEL_UP)
            ble_hid_mouse_scroll_up();
        else
            ble_hid_mouse_scroll_down();
        return;
    }
    /* 其余(鼠标键位/键盘/多媒体/空中鼠标): 转主循环单击。
     * 鼠标键位发 PRESSED+RELEASED 成对事件; 键盘/多媒体发 CLICKED(主循环内
     * 按住 15ms 再释放, 保证 host 识别); 空中鼠标发 CLICKED(主循环内切换,
     * 含校准检查与 L1 完全一致)。邮箱满(狂转)时丢弃 —— 与断连时
     * 滚轮丢弃同级, 不阻塞编码器线程。 */
    if (act == KEY_ACTION_MOUSE) {
        rt_mb_send(g_button_event_mb, (vpin << 8) | BUTTON_PRESSED);
        rt_mb_send(g_button_event_mb, (vpin << 8) | BUTTON_RELEASED);
    } else {
        rt_mb_send(g_button_event_mb, (vpin << 8) | BUTTON_CLICKED);
    }
}

/* ============================================================================
 * 09-05: C 键三手势(单击/双击/长按)
 *
 * 事件源(SDK button 库, 不可改): 短按 PRESSED -> CLICKED -> RELEASED;
 *   按住超时( BUTTON_ADV_ACTION_CHECK_DELAY, proj.conf 调为 800ms )发
 *   LONG_PRESSED(按住期间触发, 松开只发 RELEASED 不再发 CLICKED)。
 * 双击无原生事件: 用"单击判定窗口"状态机 —— 第 1 次 CLICKED 起等
 *   C_GESTURE_DBL_WINDOW_MS(220ms), 窗口内再来一次 CLICKED = 双击; 超时 = 单击。
 *   09-06: 判定窗按需启用 —— 手势模式下仅配置了"双击"动作的键进窗等待;
 *   未配双击的键 CLICKED 到达即分发单击(零延迟)。
 *
 * 模式判定(09-07 重构): 常规/手势由上位机"模式"下拉【显式落盘】,
 *   c_gesture_mode() 只读 key_config_get_c_mode(旧数据按真实手势派生兜底)。
 *   常规(直通): 按下即发按下/松开即发松开(零延迟, 与旧固件一致), 用 keys[] 主键,
 *   无视双击/长按/单击配置 —— 不存在"单击点按"概念。
 *   手势: 单击=独立 c*_tap_key 块(09-07, 不再复用 keys[] —— 常规键与手势单击
 *   互不干扰), 双击/长按 = c*_dbl/lng —— 单击变"松开后 ~220ms 点按触发"
 *   (不再支持按住保持), 长按在按住 800ms 时立即触发(不等松开)。
 * 单击判定超时经 mailbox 伪事件(C_SINGLE_VPIN_BASE+idx)回主循环分发,
 *   soft timer 回调里不做 HID 发送(与 EC 编码器同一线程约定)。
 * ⚠️ 本块依赖 mouse_button(上方定义), 勿上移到它之前。 */
#define C_GESTURE_DBL_WINDOW_MS   220   /* 双击两击最大间隔(单击判定窗口, 09-06 由 300 缩短: 单击更快, 慢双击间隔需<220ms) */
#define C_SINGLE_VPIN_BASE        0x70  /* 伪引脚基址: 单击超时事件(真实 pin 24~44 无冲突) */
#define C_SINGLE_EVT              0x7F  /* 伪事件类型(仅与伪引脚搭配, 无真实含义) */

typedef struct {
    uint8_t    pending;      /* 1=单击判定窗口中(等第二次 CLICKED) */
    rt_tick_t  click_tick;   /* 上一次 CLICKED 时刻 */
    rt_timer_t timer;        /* 单击延迟判定(one-shot, 每键独立) */
} c_gesture_t;
static c_gesture_t g_c_gesture[3];

/* 某键是否处于手势模式 —— 09-07: 改读上位机【显式落盘】模式(0=常规直通 1=手势),
 * 不再由"双击/长按是否设置"派生(旧派生把合法 NONE 块也当成手势 -> 按下无反应)。
 * 旧固件数据(模式字节 0xFF)由 key_config_get_c_mode 按真实手势动作派生兜底。 */
static int c_gesture_mode(int idx)
{
    if (idx < 0 || idx > 2) return 0;
    return key_config_get_c_mode((uint8_t)idx);
}

/* 某键手势模式"单击"动作 —— 09-07: 独立 c*_tap_key 块(与 keys[] 常规键分开),
 * 避免"改手势单击把常规键也改了"。未设置(NULL) -> 点按不触发。 */
static const key_config_t *c_single_gesture_key(int idx)
{
    if (idx < 0 || idx > 2)
        return NULL;
    return key_config_get_c_tap_key((uint8_t)idx);
}

/* 手势动作分发(单击/双击/长按 —— 均为一次触发): 键盘/多媒体按 15ms 点按
 * (host 可靠识别, 同 EC 键盘路径); 鼠标键位按 15ms 点按; NONE 不发。 */
static void c_dispatch_gesture(int idx, const key_config_t *k)
{
    (void)idx;
    if (!k)
        return;
    uint8_t act = k->action_type;
    if (act == KEY_ACTION_NONE)
        return;
    if (act == KEY_ACTION_MOUSE)
    {
        if (k->keycode == EC_KC_WHEEL_UP)
        {
            ble_hid_mouse_scroll_up();
        }
        else if (k->keycode == EC_KC_WHEEL_DOWN)
        {
            ble_hid_mouse_scroll_down();
        }
        else
        {
        uint8_t bit = (k->keycode >= 0x01 && k->keycode <= 0x04) ? k->keycode : 0x01;
        mouse_button |= bit;
        ble_hid_mouse_button(mouse_button);
        rt_thread_mdelay(15);
        mouse_button &= (uint8_t)~bit;
        ble_hid_mouse_button(mouse_button);
        }
    }
    else if (act == KEY_ACTION_KEYBOARD || act == KEY_ACTION_MULTIMEDIA)
    {
        uint8_t send_mod = (act == KEY_ACTION_MULTIMEDIA) ? 0x00 : k->modifier;
        ble_hid_keyboard_press_key(send_mod, k->keycode);
        rt_thread_mdelay(15);
        ble_hid_keyboard_release_key(send_mod, k->keycode);
    }
}

/* 单击判定超时(soft timer 线程): 只投伪事件, 分发在主循环做 */
static void c_single_timeout_cb(void *param)
{
    int idx = (int)(intptr_t)param;
    rt_mb_send(g_button_event_mb, ((C_SINGLE_VPIN_BASE + idx) << 8) | C_SINGLE_EVT);
}

/* main() 里创建 3 个单击判定定时器(key_config_init 之后任意时机) */
static void c_gesture_timers_init(void)
{
    for (int i = 0; i < 3; i++)
    {
        g_c_gesture[i].pending = 0;
        g_c_gesture[i].click_tick = 0;
        g_c_gesture[i].timer = rt_timer_create("csng", c_single_timeout_cb,
                                               (void *)(intptr_t)i,
                                               rt_tick_from_millisecond(C_GESTURE_DBL_WINDOW_MS),
                                               RT_TIMER_FLAG_ONE_SHOT | RT_TIMER_FLAG_SOFT_TIMER);
    }
}

/* ============================================================================
 * 09-07: L1/L2/L3 三键【模式 + 三手势】—— 与 C 键同架构(用户需求)。
 * 模式判定: key_config_get_l_mode(idx) —— 上位机显式落盘(0=常规 1=手势),
 *   0xFF 旧数据按真实手势动作派生(见 key_config.c)。
 * 常规: 完全保持既有行为(L1=空中鼠标开关/键/鼠标按住, L2/L3=按住/滚轮等),
 *   手势配置存而不用 —— 下方所有 L 手势逻辑不参与。
 * 手势: 单击=独立 l*_tap_key 块, 双击/长按 = l*_dbl/lng。时机与 C 键相同
 *   (双击窗 220ms、长按 800ms); AIRMOUSE 仅 L1 手势可用(=每触发切换一次,
 *   与 EC 旋转语义一致), 鼠标=15ms 点按, 滚轮=每触发滚一格, 键盘/多媒体=点按
 *   (单击/双击/长按均为一次触发动作, 长按在按住 800ms 时即时分发一次)。
 * 事件流: 手势模式下 PRESSED 被忽略(无"按下即发"按住保持), CLICKED 进双击
 *   判定窗(未配双击的键即时分发单击), LONG_PRESSED 即时分发一次 lng 动作
 *   (15ms 点按, 不经单击判定窗; RELEASED 无按住语义, 忽略)。
 * 单击判定超时经 mailbox 伪事件(L_SINGLE_VPIN_BASE+idx)回主循环分发。
 * ⚠️ 与 C 键引擎复用同一 c_gesture_t 状态结构与 C_GESTURE_DBL_WINDOW_MS 窗宽。 */
#define L_SINGLE_VPIN_BASE        0x78  /* 伪引脚基址: L 键单击超时事件(真实 pin 24~44 / C 伪 0x70 起无冲突) */
static c_gesture_t g_l_gesture[3];

static int l_gesture_mode(int idx)
{
    if (idx < 0 || idx > 2) return 0;
    return key_config_get_l_mode((uint8_t)idx);
}

static const key_config_t *l_single_gesture_key(int idx)
{
    if (idx < 0 || idx > 2)
        return NULL;
    return key_config_get_l_tap_key((uint8_t)idx);
}

/* L 键手势分发: 与 C 键一致(键盘/多媒体/鼠标 15ms 点按, 滚轮每触发一格),
 * 另支持 L1(idx==0)的 AIRMOUSE = 空中鼠标切换一次(与 EC 旋转语义一致)。 */
static void l_dispatch_gesture(int idx, const key_config_t *k)
{
    if (!k)
        return;
    uint8_t act = k->action_type;
    if (act == KEY_ACTION_NONE)
        return;
    if (act == KEY_ACTION_AIRMOUSE)
    {
        if (idx == 0)                     /* 仅 L1 手势允许空中鼠标 */
            air_mouse_key_toggle();
        return;
    }
    if (act == KEY_ACTION_MOUSE)
    {
        if (k->keycode == EC_KC_WHEEL_UP)
        {
            ble_hid_mouse_scroll_up();
        }
        else if (k->keycode == EC_KC_WHEEL_DOWN)
        {
            ble_hid_mouse_scroll_down();
        }
        else
        {
            uint8_t bit = (k->keycode >= 0x01 && k->keycode <= 0x04) ? k->keycode : 0x01;
            mouse_button |= bit;
            ble_hid_mouse_button(mouse_button);
            rt_thread_mdelay(15);
            mouse_button &= (uint8_t)~bit;
            ble_hid_mouse_button(mouse_button);
        }
    }
    else if (act == KEY_ACTION_KEYBOARD || act == KEY_ACTION_MULTIMEDIA)
    {
        uint8_t send_mod = (act == KEY_ACTION_MULTIMEDIA) ? 0x00 : k->modifier;
        ble_hid_keyboard_press_key(send_mod, k->keycode);
        rt_thread_mdelay(15);
        ble_hid_keyboard_release_key(send_mod, k->keycode);
    }
}

static void l_single_timeout_cb(void *param)
{
    int idx = (int)(intptr_t)param;
    rt_mb_send(g_button_event_mb, ((L_SINGLE_VPIN_BASE + idx) << 8) | C_SINGLE_EVT);
}

/* main() 里创建 3 个 L 键单击判定定时器(与 C 键定时器同窗宽) */
static void l_gesture_timers_init(void)
{
    for (int i = 0; i < 3; i++)
    {
        g_l_gesture[i].pending = 0;
        g_l_gesture[i].click_tick = 0;
        g_l_gesture[i].timer = rt_timer_create("lsng", l_single_timeout_cb,
                                               (void *)(intptr_t)i,
                                               rt_tick_from_millisecond(C_GESTURE_DBL_WINDOW_MS),
                                               RT_TIMER_FLAG_ONE_SHOT | RT_TIMER_FLAG_SOFT_TIMER);
    }
}

/* ============================================================================
 * 09-07: EC 按下【模式 + 三手势】—— 与 C/L 键同架构(用户需求)。
 * 模式判定: key_config_get_ec_press_mode() —— 上位机显式落盘(0=常规 1=手势),
 *   0xFF 旧数据按 dbl/lng 真实手势动作派生(见 key_config.c)。
 * 常规(直通): 完全保持既有行为(中键/键/空中鼠标等, 见下方 BSP_EC_KEY_PIN 分支),
 *   手势配置存而不用。
 * 手势: 单击=独立 ec_press_tap_key 块(不与常规键 ec_press_key 复用), 双击/长按 =
 *   ec_press_dbl/lng。时机与 C/L 相同(双击窗 220ms、长按 800ms); AIRMOUSE = 每触发
 *   切换一次(语义同 EC 旋转/L1 手势), 鼠标=15ms 点按, 滚轮=每触发滚一格,
 *   键盘/多媒体=点按(单击/双击/长按均为一次触发动作, 长按在按住 800ms 时
 *   即时分发一次)。
 * 事件流: 手势模式下 PRESSED 被忽略(无"按下即发"按住保持), CLICKED 进双击
 *   判定窗(未配双击则即时分发单击), LONG_PRESSED 即时分发一次 lng 动作
 *   (15ms 点按, 不经单击判定窗; RELEASED 无按住语义, 忽略)。
 * 单击判定超时经 mailbox 伪事件(EC_SINGLE_VPIN_BASE)回主循环分发。
 * ⚠️ 与 C/L 引擎复用同一 c_gesture_t 状态结构(EC 按下只有 1 个实例, 无 idx)。
 * ⚠️ 伪引脚须避开真实 pin(24~44)与 C(0x70..0x72)/L(0x78..0x7A)/EC_VPIN(0x70/0x71),
 *   选 0x90 无冲突。 */
#define EC_SINGLE_VPIN_BASE   0x90
static c_gesture_t g_ec_gesture;

static int ec_press_gesture_mode(void)
{
    return key_config_get_ec_press_mode();
}

static const key_config_t *ec_press_single_gesture_key(void)
{
    return key_config_get_ec_press_tap_key();
}

/* EC 按下手势分发: 与 L 键一致(键盘/多媒体/鼠标 15ms 点按, 滚轮每触发滚一格),
 * 另支持 AIRMOUSE = 空中鼠标切换一次(与 EC 旋转/L1 手势语义一致)。 */
static void ec_dispatch_press_gesture(const key_config_t *k)
{
    if (!k)
        return;
    uint8_t act = k->action_type;
    if (act == KEY_ACTION_NONE)
        return;
    if (act == KEY_ACTION_AIRMOUSE)
    {
        air_mouse_key_toggle();
        return;
    }
    if (act == KEY_ACTION_MOUSE)
    {
        if (k->keycode == EC_KC_WHEEL_UP)
        {
            ble_hid_mouse_scroll_up();
        }
        else if (k->keycode == EC_KC_WHEEL_DOWN)
        {
            ble_hid_mouse_scroll_down();
        }
        else
        {
            uint8_t bit = (k->keycode >= 0x01 && k->keycode <= 0x04) ? k->keycode : 0x01;
            mouse_button |= bit;
            ble_hid_mouse_button(mouse_button);
            rt_thread_mdelay(15);
            mouse_button &= (uint8_t)~bit;
            ble_hid_mouse_button(mouse_button);
        }
    }
    else if (act == KEY_ACTION_KEYBOARD || act == KEY_ACTION_MULTIMEDIA)
    {
        uint8_t send_mod = (act == KEY_ACTION_MULTIMEDIA) ? 0x00 : k->modifier;
        ble_hid_keyboard_press_key(send_mod, k->keycode);
        rt_thread_mdelay(15);
        ble_hid_keyboard_release_key(send_mod, k->keycode);
    }
}

static void ec_single_timeout_cb(void *param)
{
    (void)param;
    rt_mb_send(g_button_event_mb, (EC_SINGLE_VPIN_BASE << 8) | C_SINGLE_EVT);
}

/* main() 里创建 EC 按下单击判定定时器(与 C/L 定时器同窗宽, 仅 1 个实例) */
static void ec_gesture_timers_init(void)
{
    g_ec_gesture.pending = 0;
    g_ec_gesture.click_tick = 0;
    g_ec_gesture.timer = rt_timer_create("ecsng", ec_single_timeout_cb, RT_NULL,
                                         rt_tick_from_millisecond(C_GESTURE_DBL_WINDOW_MS),
                                         RT_TIMER_FLAG_ONE_SHOT | RT_TIMER_FLAG_SOFT_TIMER);
}

void HAL_MspInit(void)
{
    BSP_IO_Init();
}

int main(void)
{
    //rt_kprintf("[VibeKey-F3] Version 1.2v!\r\n");
    /* boot ROM(USB下载检测)在 USBCR 遗留 USB_EN/DP_EN/DM_PD，PHY 白耗 ~3.6mA。
     * 在所有 init 之前立即关断，复制 usb_dc_low_level_deinit 序列。 */
    hwp_hpsys_cfg->USBCR &= ~(HPSYS_CFG_USBCR_DM_PD | HPSYS_CFG_USBCR_DP_EN | HPSYS_CFG_USBCR_USB_EN);
    hwp_hpsys_rcc->RSTR2 |= HPSYS_RCC_RSTR2_USBC;
    HAL_Delay_us(100);
    hwp_hpsys_rcc->RSTR2 &= ~HPSYS_RCC_RSTR2_USBC;
    HAL_RCC_DisableModule(RCC_MOD_USBC);

    /* 诊断探针(排查上电随机高功耗)：清完 PHY 后回读 USBCR，确认 USB_EN/DP_EN/DM_PD 真正清零。
     * 若 leak=1 却无 Type-C 插入(PA44=0)，说明 boot-ROM 残留未清掉 → PHY 白耗 ~3.6mA → 上电态 ~5.1mA。 */
    {
        uint32_t _usbcr = hwp_hpsys_cfg->USBCR;
        int _leak = (_usbcr & (HPSYS_CFG_USBCR_USB_EN | HPSYS_CFG_USBCR_DP_EN | HPSYS_CFG_USBCR_DM_PD)) ? 1 : 0;
        rt_kprintf("[USB] PHY cleanup done: USBCR=0x%08X leak=%d (1=残余耗电~3.6mA)\n", _usbcr, _leak);
    }

    power_init();
    key_config_init();
    /* 09-05: C 键三手势单击判定定时器(依赖 key_config_init 后的 mailbox/定时器框架)
     * 09-07: L1/L2/L3 三键同样架构 —— 各建 3 个单击判定定时器
     * 09-07: EC 按下同样架构 —— 建 1 个单击判定定时器 */
    c_gesture_timers_init();
    l_gesture_timers_init();
    ec_gesture_timers_init();
    /* 三设备切换: 读 Flash 槽位存储 + 上电按键选择蓝牙槽位/模式(C1-C3=连接, L1-L3=配对) */
    bt_slot_init();
    bt_multi_boot_select();
    app_bt_hid_init();

    /* RGB LED init */
    extern void rgb_led_init(void);
    rgb_led_init();
    /* 应用按键底光配置（需先 rgb_led_init 再 key_config_init 之后调用）*/
    extern void key_config_apply_rgb(void);
    key_config_apply_rgb();
    /* 三设备切换: RGB 就绪后启动当前槽位指示(连接=蓝闪 / 配对=黄闪) */
    bt_multi_start_indication();
    extern void ble_led_service_init(void);
    ble_led_service_init();

    /* USB CDC 一次性注册 + OTA 线程（接口/端点只注册一次，插拔只启停控制器）*/
    usb_init_once();

    /* 编码器 */
    {
        extern int enc_init(enc_callback_t cb);
        enc_init(enc_event_handler);
        /* 注册空闲态滚动唤醒回调: 编码器在空闲(停轮询)时由引脚边沿唤醒,
         * 回调令本状态机回到活跃(IMU 唤醒 + LIGHT + 紧连接). */
        enc_set_wake_callback(air_mouse_on_external_activity);
    }

    // extern void rgb_led_set_pixel(uint8_t index, uint32_t color);
    // extern void rgb_led_show(void);
    // extern void rgb_led_set_colors(uint32_t *colors, uint16_t count);
    // extern void rgb_led_set_color(uint32_t color);

    rt_thread_mdelay(100);

    /* 发送线程阻塞信号量(取代 2ms 轮询, 连接态空闲低功耗 B 关键) */
    g_mouse_sem = rt_sem_create("mse", 0, RT_IPC_FLAG_FIFO);

    {
        rt_thread_t tid = rt_thread_create("mse_tx", mouse_send_thread, RT_NULL, 3072, 16, 10);
        if (tid)
            rt_thread_startup(tid);
    }

    /* 09-03 摇一摇: 信号量 + 发送线程。栈只用 512B(只发几个 HID 包), 优先级与
     * mse_tx 相同 —— 高于它也没意义(HID 发送本身受 BLE 连接事件节拍限制)。 */
    g_shake_sem = rt_sem_create("shake", 0, RT_IPC_FLAG_FIFO);

    {
        rt_thread_t tid = rt_thread_create("shake", shake_thread, RT_NULL, 1024, 16, 10);
        if (tid)
            rt_thread_startup(tid);
    }

    /* USB 插入检测（方案B）：并入充电检测 —— 判据 = 充电器 CHG_SR 的 VBUS_RDY 位，
     * 由 charge_poll_cb(1s) 在状态变化时唤醒 usb_vbus_thread 执行启停。
     * 不再使用 PA44 中断/去抖线程（见文件上方说明）。
     * 注：cdc_acm 接口/端点已由 usb_init_once() 注册，此处只起"启停决策"线程。
     * IMU 启动/关闭由 BLE 连接/断开事件回调直接驱动（见 ble_app.c 的
     * ble_hid_event_handler 的 CONNECTED/DISCONNECTED case 调 air_mouse_start/stop）。 */
    usb_sem = rt_sem_create("usbvbus", 0, RT_IPC_FLAG_FIFO);

    {
        rt_thread_t tid = rt_thread_create("usbvbus", usb_vbus_thread, RT_NULL, 1024, 16, 10);
        if (tid)
            rt_thread_startup(tid);
    }

    extern ble_hid_env_t g_hid_env;
    rt_uint32_t key_value;

    /* 开局 CPU 降到 48MHz（广播/空闲无需 240MHz）。
     * HFP 音频激活时 SDK 自动 pm_scenario_start(AUDIO)→切回 240MHz，
     * 音频停止后回到 48MHz。 */
    rt_pm_run_enter(PM_RUN_MODE_MEDIUM_SPEED);
    rt_kprintf("[POWER] CPU set to 48MHz (MEDIUM_SPEED), HFP bumps to 240MHz on demand\n");

    /* 音频省电：关 EQ（免提通话不需要音效处理）、降麦克风增益已在 power.c 覆写为 0dB */
    extern void bf0_audprc_eq_enable_offline(uint8_t is_enable);
    bf0_audprc_eq_enable_offline(0);
    rt_kprintf("[POWER] audio EQ disabled for power saving\n");
    while (1)
    {
        /* 事件驱动：无按键时主线程阻塞在 mailbox，idle 线程得以运行、
         * 系统进入深睡。这是阶段1消除 50ms 轮询、让设备“能睡”的关键。 */
        if (rt_mb_recv(g_button_event_mb, &key_value, RT_WAITING_FOREVER) == RT_EOK)
        {
            /* 断连后按键唤醒再广播：让设备重新可被手机发现（阶段1低功耗配套）*/
            ble_app_wakeup_advertise();

            uint8_t pin = (key_value >> 8) & 0xFF;
            button_action_t action = key_value & 0xFF;

            /* 09-05: 单击判定超时伪事件(手势模式专用, 见 c_gesture 注释) */
            if (pin >= C_SINGLE_VPIN_BASE && pin <= C_SINGLE_VPIN_BASE + 2)
            {
                int idx = pin - C_SINGLE_VPIN_BASE;
                if (g_c_gesture[idx].pending)
                {
                    g_c_gesture[idx].pending = 0;
                    c_dispatch_gesture(idx, c_single_gesture_key(idx));   /* 手势模式单击(tap 块) */
                }
                continue;
            }
            /* 09-07: L 键单击判定超时伪事件(手势模式, 见 l_gesture 注释) */
            if (pin >= L_SINGLE_VPIN_BASE && pin <= L_SINGLE_VPIN_BASE + 2)
            {
                int idx = pin - L_SINGLE_VPIN_BASE;
                if (g_l_gesture[idx].pending)
                {
                    g_l_gesture[idx].pending = 0;
                    l_dispatch_gesture(idx, l_single_gesture_key(idx));   /* L 手势模式单击(l*_tap 块) */
                }
                continue;
            }
            /* 10-05: EC 按压滚动(手势模式: 按住旋钮旋转) —— 先【清零】EC 按下手势
             * 状态(单击判定窗 + 定时器), 再分发按压滚动动作。这样"按住旋钮旋转"
             * 松手时不会被识别成单击/双击/长按。抑制标志已由编码器回调置位。 */
            if (pin == EC_VPIN_CW_PRESS || pin == EC_VPIN_CCW_PRESS)
            {
                g_ec_gesture.pending = 0;
                rt_timer_stop(g_ec_gesture.timer);
                ec_dispatch_press_gesture(pin == EC_VPIN_CW_PRESS
                                              ? key_config_get_ec_cw_press_key()
                                              : key_config_get_ec_ccw_press_key());
                continue;
            }
            /* 09-07: EC 按下单击判定超时伪事件(手势模式, 见 ec_gesture 注释) */
            if (pin == EC_SINGLE_VPIN_BASE)
            {
                if (g_ec_gesture.pending)
                {
                    g_ec_gesture.pending = 0;
                    /* 10-05: 本按压周期已发生按压滚动 -> 兜底不再触发单击
                     * (防"按压滚动事件因邮箱满被丢弃"时误触发) */
                    if (!g_ec_scroll_hold)
                        ec_dispatch_press_gesture(ec_press_single_gesture_key());   /* EC 按下手势·单击(tap 块) */
                }
                continue;
            }

            rt_kprintf("[BTN] pin=%d event=%s\r\n", pin, action_name(action));

            int key_idx = -1;
            /* ⚠️ 引脚映射(实际宏定义, 见 boards/sf32lb52-vibekey/bsp_pinmux.c):
             * C1=25, C2=27, C3=32 (旧注释 27/32/25 是早期引脚, 勿再混淆) */
            if (pin == BSP_KEY_C1_PIN) key_idx = 0;   //0 //25
            else if (pin == BSP_KEY_C2_PIN) key_idx = 1;  //1 27
            else if (pin == BSP_KEY_C3_PIN) key_idx = 2; //2  32

            /* 09-07: L1/L2/L3 手势模式提前截获 —— 手势模式下 L 键的 PRESSED 无
             * "按下即发"按住语义(忽略), CLICKED 进双击判定窗(未配双击则即时分发
             * 单击), LONG_PRESSED 即时分发一次 lng 动作(15ms 点按);
             * 常规模式走下方既有分支(完全不变)。 */
            {
                int li = -1;
                if (pin == BSP_KEY_L1_PIN) li = 0;
                else if (pin == BSP_KEY_L2_PIN) li = 1;
#ifdef BSP_KEY_L3_PIN
                else if (pin == BSP_KEY_L3_PIN) li = 2;
#endif
                if (li >= 0 && l_gesture_mode(li))
                {
                    if (action == BUTTON_CLICKED)
                    {
                        const key_config_t *dbl = key_config_get_l_dbl_key((uint8_t)li);
                        c_gesture_t *g = &g_l_gesture[li];
                        if (!dbl)
                        {
                            l_dispatch_gesture(li, l_single_gesture_key(li));
                        }
                        else
                        {
                            rt_tick_t now = rt_tick_get();
                            if (g->pending &&
                                (now - g->click_tick) <= rt_tick_from_millisecond(C_GESTURE_DBL_WINDOW_MS))
                            {
                                g->pending = 0;
                                rt_timer_stop(g->timer);
                                l_dispatch_gesture(li, dbl);    /* 双击 */
                            }
                            else
                            {
                                g->pending = 1;
                                g->click_tick = now;
                                rt_timer_start(g->timer);
                            }
                        }
                    }
                    else if (action == BUTTON_LONG_PRESSED)
                    {
                        /* 长按: 按住 800ms 触发一次 —— 按 lng 块 15ms 点按
                         * (AIRMOUSE 仅 L1 可用=切换一次, 见 l_dispatch_gesture)。 */
                        l_dispatch_gesture(li, key_config_get_l_lng_key((uint8_t)li));
                    }
                    /* PRESSED / RELEASED: 手势模式无按住语义, 忽略 */
                    continue;
                }
            }

            /* 09-07: EC 按下手势模式提前截获 —— 与 L 键同策略: 手势模式下 EC 按下键
             * 的 PRESSED 无"按下即发"按住语义(忽略), CLICKED 进双击判定窗(未配双击则
             * 即时分发单击), LONG_PRESSED 即时分发一次 lng 动作(15ms 点按);
             * 常规模式走下方 BSP_EC_KEY_PIN 既有分支(完全不变, 中键直通等)。 */
            if (pin == BSP_EC_KEY_PIN && ec_press_gesture_mode())
            {
                /* 10-05: 本次按压周期内发生过"按压滚动" -> 单击/双击/长按一律不触发
                 * (用户需求: 只要是按压滚动了, 就把这些标志清零)。
                 * PRESSED/RELEASED 复位抑制标志(周期开始/结束)。 */
                if (action == BUTTON_PRESSED || action == BUTTON_RELEASED)
                {
                    g_ec_scroll_hold = 0;
                }
                else if (g_ec_scroll_hold)
                {
                    g_ec_gesture.pending = 0;          /* 吞掉本次单击/双击/长按 */
                    rt_timer_stop(g_ec_gesture.timer);
                }
                else if (action == BUTTON_CLICKED)
                {
                    const key_config_t *dbl = key_config_get_ec_press_dbl_key();
                    if (!dbl)
                    {
                        ec_dispatch_press_gesture(ec_press_single_gesture_key());
                    }
                    else
                    {
                        rt_tick_t now = rt_tick_get();
                        if (g_ec_gesture.pending &&
                            (now - g_ec_gesture.click_tick) <= rt_tick_from_millisecond(C_GESTURE_DBL_WINDOW_MS))
                        {
                            g_ec_gesture.pending = 0;
                            rt_timer_stop(g_ec_gesture.timer);
                            ec_dispatch_press_gesture(dbl);    /* 双击 */
                        }
                        else
                        {
                            g_ec_gesture.pending = 1;
                            g_ec_gesture.click_tick = now;
                            rt_timer_start(g_ec_gesture.timer);
                        }
                    }
                }
                else if (action == BUTTON_LONG_PRESSED)
                {
                    /* 长按: 按住 800ms 触发一次 —— 按 lng 块 15ms 点按
                     * (AIRMOUSE=切换一次, 见 ec_dispatch_press_gesture)。 */
                    ec_dispatch_press_gesture(key_config_get_ec_press_lng_key());
                }
                /* PRESSED / RELEASED: 手势模式无按住语义, 忽略 */
                continue;
            }

            /* KEY2 空中鼠标开关: 在 mailbox 中通过 BUTTON_CLICKED 切换.
             * 主循环已改 RT_WAITING_FOREVER 阻塞, 按键即时唤醒, 开关响应不受影响. */

            if (key_idx >= 0)
            {
                key_config_t *cfg = &key_config_get()->keys[key_idx];

                if (action == BUTTON_PRESSED)
                {
                    /* C1 语音开关键: 按下=开始语音(PTT)。
                     * ① 提前退出 HFP sniff(长时间空闲链路已进 ~500ms sniff, 不退则
                     *    SCO 建链慢/语音开头丢), 为 SCO 铺路(见 ble_app_exit_sniff_for_call);
                     * ② 当前语音(SCO)进行中按下 = 用户想结束语音. 键盘报告走 BLE,
                     *    SCO 期间射频被饿可能延迟/丢失(实测 11s 才送达) → 并行发
                     *    AT+BVRA=0 走 HFP 链路立即请求手机停 VF, 双通道兜底,
                     *    谁先到达谁生效. 开启语音时(C1 第一次按) SCO 未建立
                     *    g_sco_active=0, 不会误发. */
                    if (key_idx == 0) {
                        ble_app_exit_sniff_for_call();
                        /* ⚠️ 08-26 定案: C1 按下【不再预切 sco_quiet】。曾加"语音开头
                         * 预切"(按下即把 BLE 拉到 40~80ms 给 SCO 让射频), 但按下时
                         * 无法预知是否真要开语音 —— 误按/非语音按下(HFP 已连场景)
                         * 同样拉宽 BLE → "按 C1 鼠标一卡一卡, 放下一会才正常"。
                         * 现改回由 SCO_CONNECTED 事件里切 sco_quiet(语音真正建立
                         * 后才拉宽): 按下 C1 完全无 BLE 副作用; 语音开头 BLE 高频
                         * 空事件(无数据流, HID 已被 g_sco_active 抑制)与 SCO 建链
                         * 共存影响小。 */
                        extern volatile uint8_t g_sco_active;
                        if (g_sco_active)
                            ble_app_hfp_deactivate_vf();
                    }
                    if(g_key2_active) g_key2_active = 0;
                    /* 按下即亮：启用则显示配置色（按亮度缩放）；关闭则不亮 */
                    uint32_t press_color = 0x000000;
                    if (cfg->rgb_enabled)
                    {
                        /* ★ C1/C2/C3 亮度硬上限 15%（10-04 产品要求，硬件主动降低）。
                         * 钳亮度参数而非结果色：本行下面就是"按亮度缩放"，若在 rgb_led_show()
                         * 里再压一次就成了双重降幅（15% → 38 → 6 ≈ 2.4%，肉眼全黑）。
                         * 即使上位机存了 100，也在此被压到 15。 */
                        uint8_t bri = rgb_led_cap_brightness(cfg->rgb_brightness);
                        uint32_t r = ((uint32_t)cfg->rgb_r * bri) / 100;
                        uint32_t g = ((uint32_t)cfg->rgb_g * bri) / 100;
                        uint32_t b = ((uint32_t)cfg->rgb_b * bri) / 100;
                        press_color = (r << 16) | (g << 8) | b;
                    }
                    rgb_led_show(key_idx, press_color);

                    /* 09-05: 手势模式下按住即发改为手势分发(单击有 ~220ms 判定窗),
                     * 09-07: 常规/手势由上位机显式落盘模式判定(c_gesture_mode 读
                     * key_config_get_c_mode), 常规(直通)模式走"按下即发/松开即发"
                     * —— 无单击概念, 且【无视】双击/长按配置(存而不用):
                     * 键盘/多媒体=按住保持(down/up), 鼠标键位=按下
                     * 置位按住保持(拖拽), 滚轮=每按滚动一格(语义与 L1/L2/L3 直通一致)。
                     * 08-27 多键修复: 按下/释放用同一 modifier, 保证 held 集合
                     * 增删匹配。多媒体键按下时固定 modifier=0x00。 */
                    if (!c_gesture_mode(key_idx))
                    {
                    if (cfg->action_type == KEY_ACTION_MOUSE)
                    {
                        if (cfg->keycode == EC_KC_WHEEL_UP)
                            ble_hid_mouse_scroll_up();
                        else if (cfg->keycode == EC_KC_WHEEL_DOWN)
                            ble_hid_mouse_scroll_down();
                        else
                        {
                            uint8_t bit = (cfg->keycode == 0x01 || cfg->keycode == 0x02 || cfg->keycode == 0x04)
                                          ? cfg->keycode : 0x01;
                            mouse_button |= bit;
                            ble_hid_mouse_button(mouse_button);
                        }
                    }
                    else
                    {
                    uint8_t send_mod = (cfg->action_type == KEY_ACTION_MULTIMEDIA) ? 0x00 : cfg->modifier;
                    switch (cfg->action_type)
                    {
                    case KEY_ACTION_KEYBOARD:
                        ble_hid_keyboard_press_key(send_mod, cfg->keycode);
                        break;
                    case KEY_ACTION_MULTIMEDIA:
                        ble_hid_keyboard_press_key(send_mod, cfg->keycode);
                        break;
                    default:
                        break;
                    }
                    }
                    }
                }
                else if (action == BUTTON_RELEASED)
                {
                    /* 松开即灭（底光为瞬时指示，空闲态全灭） */
                    rgb_led_show(key_idx, 0x000000);
                    if (key_idx == 0) {
                        /* C1 松开: 仅当 BLE 参数确实处于宽间隔(被 sco_quiet 拉宽,
                         * 即语音中松开=结束语音)才发起收紧(绕过节流), 提前为
                         * SCO 断开铺路。
                         * ⚠️ 短按(非语音)场景: 参数本就在高频(7.5~15ms)——
                         * 【不发无谓的 conn param update 请求】: 协商过程(LL
                         * 控制流程 1~2s)本身会扰动链路, 撞上按完键继续移动的
                         * 鼠标 → "短按 C1 鼠标就一卡一卡"(08-26 实测定案)。 */
                        extern volatile uint16_t g_ble_cur_interval;
                        if (g_ble_cur_interval > 20)   /* >25ms 才需要收紧 */
                            ble_app_conn_param_tighten();
                    }
                    /* 08-27 多键修复: 释放单键(用与按下相同的 modifier),
                     * 不再全释放, 避免 C2+C3 同时按下时松开一个误放另一个。
                     * 松开即发"松开" —— 键盘/多媒体释放, 鼠标键位清位
                     * (按住拖拽松开才停); 滚轮无按住态无需释放。
                     * 手势模式 RELEASED 无按住语义(单击/双击/长按均为 15ms
                     * 点按, 已自含释放), 落到此处仅做常规键释放, 无副作用。 */
                    if (cfg->action_type == KEY_ACTION_MOUSE)
                    {
                        if (cfg->keycode != EC_KC_WHEEL_UP && cfg->keycode != EC_KC_WHEEL_DOWN)
                        {
                            uint8_t bit = (cfg->keycode == 0x01 || cfg->keycode == 0x02 || cfg->keycode == 0x04)
                                          ? cfg->keycode : 0x01;
                            mouse_button &= (uint8_t)~bit;
                            ble_hid_mouse_button(mouse_button);
                        }
                    }
                    else if (cfg->action_type == KEY_ACTION_KEYBOARD ||
                             cfg->action_type == KEY_ACTION_MULTIMEDIA)
                    {
                        uint8_t send_mod = (cfg->action_type == KEY_ACTION_MULTIMEDIA) ? 0x00 : cfg->modifier;
                        ble_hid_keyboard_release_key(send_mod, cfg->keycode);
                    }
                }
                else if (action == BUTTON_CLICKED)
                {
                    /* 09-05: 手势模式单击判定 —— SDK 对短按发 CLICKED(松开沿)。
                     * 窗口内第二次 CLICKED = 双击(取消单击定时器并分发双击);
                     * 否则记 pending + 重启 one-shot 定时器, 超时伪事件分发单击。
                     * 直通模式 CLICKED 到达时已按 PRESSED/RELEASED 处理完, 忽略。 */
                    if (c_gesture_mode(key_idx))
                    {
                        const key_config_t *dbl = key_config_get_c_dbl_key((uint8_t)key_idx);
                        c_gesture_t *g = &g_c_gesture[key_idx];
                        /* 09-06: 判定窗按需启用 —— 仅配置了"双击"动作的键才有 300ms 级
                         * 等待(区分双击的物理必要); 只配长按/单击的键, 单击即时分发。
                         * 09-07: 单击动作读独立 tap 块(与常规键 keys[] 分开)。 */
                        if (!dbl)
                        {
                            c_dispatch_gesture(key_idx, c_single_gesture_key(key_idx));
                        }
                        else
                        {
                            rt_tick_t now = rt_tick_get();
                            if (g->pending &&
                                (now - g->click_tick) <= rt_tick_from_millisecond(C_GESTURE_DBL_WINDOW_MS))
                            {
                                g->pending = 0;
                                rt_timer_stop(g->timer);
                                c_dispatch_gesture(key_idx, dbl);    /* 双击 */
                            }
                            else
                            {
                                g->pending = 1;
                                g->click_tick = now;
                                rt_timer_start(g->timer);
                            }
                        }
                    }
                }
                else if (action == BUTTON_LONG_PRESSED)
                {
                    /* 09-05: 手势模式长按 —— 按住 BUTTON_ADV_ACTION_CHECK_DELAY
                     * (proj.conf=800ms)时 SDK 发 LONG_PRESSED(按住期间, 每按一次)。
                     * 手势模式: 按 lng 块 15ms 点按一次(不经单击判定窗);
                     * C 键 lng 块无 AIRMOUSE(手势校验已拒)。
                     * 常规(直通)模式忽略(按住发送已由 PRESSED/RELEASED 管理)。 */
                    if (c_gesture_mode(key_idx))
                    {
                        c_dispatch_gesture(key_idx, key_config_get_c_lng_key((uint8_t)key_idx));
                    }
                }
            }
            else if (pin == BSP_KEY_L1_PIN)
            {
                /* 09-05: L1 动作可配置(默认 AIRMOUSE=历史行为: 空中鼠标开关)。
                 * 重映射为键盘/多媒体时按住发送(同 C 键), MOUSE 时鼠标键位保持,
                 * NONE 无动作 —— 注意此时空中鼠标将无法用 L1 开启(用户显式选择)。 */
                const key_config_t *lk = key_config_get_l1_key();
                uint8_t act = (lk && lk->action_type <= KEY_ACTION_AIRMOUSE)
                              ? lk->action_type : KEY_ACTION_AIRMOUSE;
                if (act == KEY_ACTION_KEYBOARD || act == KEY_ACTION_MULTIMEDIA)
                {
                    uint8_t send_mod = (act == KEY_ACTION_MULTIMEDIA) ? 0x00 : lk->modifier;
                    if (action == BUTTON_PRESSED)
                        ble_hid_keyboard_press_key(send_mod, lk->keycode);
                    else if (action == BUTTON_RELEASED || action == BUTTON_CLICKED)
                        ble_hid_keyboard_release_key(send_mod, lk->keycode);
                }
                else if (act == KEY_ACTION_MOUSE)
                {
                    uint8_t bit = (lk->keycode == 0x01 || lk->keycode == 0x02 || lk->keycode == 0x04)
                                  ? lk->keycode : 0x01;
                    if (action == BUTTON_PRESSED) {
                        mouse_button |= bit;
                    } else if (action == BUTTON_RELEASED || action == BUTTON_CLICKED) {
                        mouse_button &= (uint8_t)~bit;
                    }
                    ble_hid_mouse_button(mouse_button);
                }
                else if (act != KEY_ACTION_NONE)
                {
                uint8_t mode = key_config_get()->air_mouse_mode;
                if (mode == AIR_MOUSE_MODE_HOLD) {
                    /* 按住模式: 按下开, 松开关(09-06: 逻辑抽到 air_mouse_key_hold,
                     * 与 EC 编码器的 AIRMOUSE 动作共用同一实现) */
                    if (action == BUTTON_PRESSED) {
                        air_mouse_key_hold(1);
                    } else if (action == BUTTON_RELEASED) {
                        air_mouse_key_hold(0);
                    }
                } else {
                    /* 单击切换模式 (默认): 单击 toggle(与 EC 编码器共用助手) */
                    if (action == BUTTON_CLICKED) {
                        air_mouse_key_toggle();
                    }
                }
                }   /* 09-05: act != NONE(空中鼠标默认路径) 包裹块收尾 */
            }
            else if (pin == BSP_KEY_L2_PIN)
            {
                /* 09-05: L2 快捷键可配置(上位机写入 l2_key)。
                 * - KEYBOARD/MULTIMEDIA: 按住发送(与 C1-C3 相同的 press/release 语义);
                 * - MOUSE(或历史默认): 鼠标按键位(默认左键), 按住保持;
                 * - NONE: 无动作。
                 * 位域维护(历史行为): 仅本键置位/清位, 互不影响; CLICKED(完整单击)
                 * 也清位, 避免"只发 CLICKED 没发 RELEASED"的边界下鼠标键永久卡住。
                 * 移动中按键位由发送线程按引脚电平直读反推(见 mouse 发送线程,
                 * 仅当 L2 仍是鼠标动作时才纳入)。 */
                const key_config_t *lk = key_config_get_l2_key();
                uint8_t act = (lk && lk->action_type <= KEY_ACTION_MULTIMEDIA)
                              ? lk->action_type : KEY_ACTION_MOUSE;
                if (act == KEY_ACTION_KEYBOARD || act == KEY_ACTION_MULTIMEDIA)
                {
                    uint8_t send_mod = (act == KEY_ACTION_MULTIMEDIA) ? 0x00 : lk->modifier;
                    if (action == BUTTON_PRESSED)
                        ble_hid_keyboard_press_key(send_mod, lk->keycode);
                    else if (action == BUTTON_RELEASED || action == BUTTON_CLICKED)
                        ble_hid_keyboard_release_key(send_mod, lk->keycode);
                }
                else if (act == KEY_ACTION_MOUSE)
                {
                    /* 09-06: 滚轮动作(键码 5/6) —— 每次按下滚动一格 */
                    if (lk->keycode == EC_KC_WHEEL_UP || lk->keycode == EC_KC_WHEEL_DOWN) {
                        if (action == BUTTON_PRESSED) {
                            if (lk->keycode == EC_KC_WHEEL_UP) ble_hid_mouse_scroll_up();
                            else ble_hid_mouse_scroll_down();
                        }
                    } else {
                        uint8_t bit = (lk->keycode == 0x02 || lk->keycode == 0x04) ? lk->keycode : 0x01;
                        if (action == BUTTON_PRESSED) {
                            mouse_button |= bit;
                        } else if (action == BUTTON_RELEASED
                                 || action == BUTTON_CLICKED) {
                            mouse_button &= (uint8_t)~bit;
                        }
                        ble_hid_mouse_button(mouse_button);
                    }
                }
            }
#ifdef BSP_KEY_L3_PIN
            else if (pin == BSP_KEY_L3_PIN) //mouse right
            {
                /* 09-05: L3 快捷键可配置 —— 分支结构与 L2 完全对称, 默认鼠标右键。 */
                const key_config_t *lk = key_config_get_l3_key();
                uint8_t act = (lk && lk->action_type <= KEY_ACTION_MULTIMEDIA)
                              ? lk->action_type : KEY_ACTION_MOUSE;
                if (act == KEY_ACTION_KEYBOARD || act == KEY_ACTION_MULTIMEDIA)
                {
                    uint8_t send_mod = (act == KEY_ACTION_MULTIMEDIA) ? 0x00 : lk->modifier;
                    if (action == BUTTON_PRESSED)
                        ble_hid_keyboard_press_key(send_mod, lk->keycode);
                    else if (action == BUTTON_RELEASED || action == BUTTON_CLICKED)
                        ble_hid_keyboard_release_key(send_mod, lk->keycode);
                }
                else if (act == KEY_ACTION_MOUSE)
                {
                    /* 09-06: 滚轮动作(键码 5/6) —— 每次按下滚动一格 */
                    if (lk->keycode == EC_KC_WHEEL_UP || lk->keycode == EC_KC_WHEEL_DOWN) {
                        if (action == BUTTON_PRESSED) {
                            if (lk->keycode == EC_KC_WHEEL_UP) ble_hid_mouse_scroll_up();
                            else ble_hid_mouse_scroll_down();
                        }
                    } else {
                        uint8_t bit = (lk->keycode == 0x01 || lk->keycode == 0x04) ? lk->keycode : 0x02;
                        if (action == BUTTON_PRESSED) {
                            mouse_button |= bit;
                        } else if (action == BUTTON_RELEASED || action == BUTTON_CLICKED) {
                            mouse_button &= (uint8_t)~bit;
                        }
                        ble_hid_mouse_button(mouse_button);
                    }
                }
                }   /* L3 分支收尾(09-06 行替换时误吞, 补回) */
#endif
            else if (pin == BSP_EC_KEY_PIN) //mouse middle
            {
                /* 09-05: 编码器按下可配置 —— 默认鼠标中键(历史行为);
                 * KEYBOARD/MULTIMEDIA 按住发送; MOUSE 仅键位 1/2/4(有效性已由
                 * key_config_eckey_valid 保证); NONE 无动作。 */
                const key_config_t *k = key_config_get_ec_press_key();
                uint8_t act = (k && k->action_type <= KEY_ACTION_AIRMOUSE)
                              ? k->action_type : KEY_ACTION_MOUSE;
                if (act == KEY_ACTION_AIRMOUSE)
                {
                    /* 09-06: 激活空中鼠标 —— 语义与 L1 完全一致(遵循 air_mouse_mode) */
                    if (key_config_get()->air_mouse_mode == AIR_MOUSE_MODE_HOLD) {
                        if (action == BUTTON_PRESSED) air_mouse_key_hold(1);
                        else if (action == BUTTON_RELEASED || action == BUTTON_CLICKED) air_mouse_key_hold(0);
                    } else if (action == BUTTON_CLICKED) {
                        air_mouse_key_toggle();
                    }
                }
                else if (act == KEY_ACTION_KEYBOARD || act == KEY_ACTION_MULTIMEDIA)
                {
                    uint8_t send_mod = (act == KEY_ACTION_MULTIMEDIA) ? 0x00 : k->modifier;
                    if (action == BUTTON_PRESSED)
                        ble_hid_keyboard_press_key(send_mod, k->keycode);
                    else if (action == BUTTON_RELEASED || action == BUTTON_CLICKED)
                        ble_hid_keyboard_release_key(send_mod, k->keycode);
                }
                else if (act == KEY_ACTION_MOUSE)
                {
                    /* 09-06: 按下手势也支持滚轮动作 —— 每次按下滚动一格 */
                    if (k->keycode == EC_KC_WHEEL_UP || k->keycode == EC_KC_WHEEL_DOWN) {
                        if (action == BUTTON_PRESSED) {
                            if (k->keycode == EC_KC_WHEEL_UP) ble_hid_mouse_scroll_up();
                            else ble_hid_mouse_scroll_down();
                        }
                    } else {
                        uint8_t bit = (k->keycode == 0x01 || k->keycode == 0x02) ? k->keycode : 0x04;
                        if (action == BUTTON_PRESSED) {
                            mouse_button |= bit;
                        } else if (action == BUTTON_RELEASED
                                 || action == BUTTON_CLICKED) {
                            mouse_button &= (uint8_t)~bit;
                        }
                        ble_hid_mouse_button(mouse_button);
                    }
                }
            }
            else if (pin == EC_VPIN_CW || pin == EC_VPIN_CCW)
            {
                /* 09-05: 编码器旋转(虚拟引脚, 来自 enc_event_handler 的 mailbox 转发)。
                 * 鼠标键位: PRESSED/RELEASED 成对到达, 置位/清位发单击;
                 * 键盘/多媒体: CLICKED 到达, 按住 15ms 再释放(timer 线程不能延时,
                 * 借主循环上下文完成; 15ms 足够 host 识别一次按键)。 */
                const key_config_t *k = (pin == EC_VPIN_CW) ? key_config_get_ec_cw_key()
                                                            : key_config_get_ec_ccw_key();
                uint8_t act = (k && k->action_type <= KEY_ACTION_AIRMOUSE)
                              ? k->action_type : KEY_ACTION_MOUSE;
                if (act == KEY_ACTION_AIRMOUSE)
                {
                    /* 09-06: 旋转一步 = 切换空中鼠标(旋转无按下/松开, 每档切换一次) */
                    if (action == BUTTON_CLICKED)
                        air_mouse_key_toggle();
                }
                else if (act == KEY_ACTION_MOUSE)
                {
                    uint8_t bit = (k->keycode >= 0x01 && k->keycode <= 0x04) ? k->keycode : 0x01;
                    if (action == BUTTON_PRESSED) {
                        mouse_button |= bit;
                    } else if (action == BUTTON_RELEASED
                             || action == BUTTON_CLICKED) {
                        mouse_button &= (uint8_t)~bit;
                    }
                    ble_hid_mouse_button(mouse_button);
                }
                else if (action == BUTTON_CLICKED)
                {
                    uint8_t send_mod = (act == KEY_ACTION_MULTIMEDIA) ? 0x00 : k->modifier;
                    ble_hid_keyboard_press_key(send_mod, k->keycode);
                    rt_thread_mdelay(15);
                    ble_hid_keyboard_release_key(send_mod, k->keycode);
                }
            }
        }
    }
}
