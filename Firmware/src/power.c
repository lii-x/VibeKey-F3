#include "rtthread.h"
#include "rtdevice.h"            /* rt_pin_mode / rt_pin_write（LDO3 域引脚收尾） */
#include "rthw.h"                /* rt_hw_interrupt_disable (本 build 的 rtthread.h 不含) */
#include "bf0_hal.h"
#include "bf0_hal_pmu.h"
#include "bf0_hal_aon.h"         /* HAL_HPAON_QueryWakeupPin / AON_PIN_MODE_LOW */
#include "bf0_hal_lrc_cal.h"     /* HAL_RC_CAL_update_reference_cycle_on_48M (LXT 关时周期重校 LP/RC) */
#include "bf0_pm.h"
#include "drv_io.h"              /* HAL_PIN_Set / PAD_PAxx / GPIO_Axx / PIN_PULLUP */
#include "drivers/pm.h"
#include "hpsys_rcc.h"          /* HPSYS_RCC_DBGR_FORCE_HP（清掉以允许 WFI 深度降频） */
#include "audcodec.h"            /* AUDCODEC_CFG_ADC/DAC_ENABLE */
#include "audprc.h"              /* AUDPRC_CFG_ENABLE */
#include "bf0_hal_mpi_ex.h"     /* HAL_MPI_PSRAM_ENT_LOWP / HAL_MPI_EXIT_LOWP (psram_dbg) */
#include "power.h"
#include "led.h"
#include "key_config.h"          /* key_enable_deep_wakeup：按键 AON per-pin 唤醒（power_init 常驻使能）*/
#include "ble_app.h"
#include "ble_led_service.h"   /* ble_led_turn_off：待机熄灭 app 状态灯并停其 50ms 软定时器 */
#include "bt_slot.h"           /* bt_multi_restore_charge_led：开机显示后还原蓝灯充电指示 */
#include "charge.h"            /* rt_charge_get_detect_status：直读 CHG_SR VBUS 位(待机 RTC 电量检查) */

/* SCO(通话)进行中标志，由 ble_app.c 维护。通话中 RC 重校会扰动 LCPU→损坏 SCO 链路，
 * 故 15s 周期重校定时器需据此跳过。*/
extern volatile uint8_t g_sco_active;

/* USB 控制器是否在线(main.c 维护)：USB 插入期间禁止 DEEP 待机(USB 在线 = 用户
 * 需要保持连接)，sleep_thread_entry 据此跳过待机分支。 */
extern int usb_on;

/* LXT_LP_CYCLE：LXT 关闭时 LP/RC 校准的参考周期数，板级 rtconfig.h 已定义为 200；
 * 此处加 #ifndef 兜底，避免极端构建未包含 rtconfig 时编译失败（与 bsp_init.c 一致）。 */
#ifndef LXT_LP_CYCLE
    #define LXT_LP_CYCLE 200
#endif

/* ===== 低电量监控 ===== */
#define BAT_LOW_BLINK_PCT     20   /* <20% 红灯(BSP_LED4_PIN) 闪烁(09-09: 由 15% 提高) */
#define BAT_LOW_BLINK_DIV     4    /* 闪烁分频: 每 4 个 500ms 节拍翻转一次 = 2s亮/2s灭(09-09 放慢) */
/* 09-16: 关机阈值 5% → 10%。用户实测本芯片低于 3.6V 就不能正常工作(欠压→掉电/行为异常),
 * 而放电表 5% = 3681mV(3.681V)离 3.6V 太近(且实际带载时电压更低, 会先掉到 3.6V 以下)。
 * 10% ≈ 3719mV, 留出安全余量。
 * 09-16 晚: 曾临时改 20% 实测关机链路(用户手上 17%), 实测通过 ⇒ 已改回 10%。
 * 实测结果: 活跃态 `battery critically low (17%, ...) → pm_shutdown → hibernate` ✓
 * (并顺带修掉"关机后 RGB 常亮 + 0.6mA"—— 见 pm_shutdown 里断 LDO3 的说明)。
 * ★09-18: 曾临时改 20%（用户电源不可调, 只能抬高阈值来触发关机以验证链路）。
 *   ✅ 验证已完成，**已复位为 10**。两条路径都读本宏:
 *     ① 活跃态 battery_monitor(power.c:2043): 连续 2 次确认 → pm_shutdown
 *     ② 待机态 standby_rtc_battery_check(power.c:992): 每次 RTC 唤醒检查 */
#define BAT_SHUTDOWN_PCT      10   /* ≤X% 关机(09-09: 1%→5%; 09-16: 5%→10%) */
#define BAT_SHUTDOWN_MV       37000 /* 电压地板: 低于 3.70V 直接关机(兜底, 略低于 10% 点 3.719V)。
                                     *
                                     * ★★★ 09-18 单位定案（此前写成 3700，是**漏了一位的死代码**）:
                                     *   `app_get_battery_mv()` 的单位是 **0.1mV**，不是 mV！
                                     *   证据链: drv_adc.c:477 `fval = HAL_ADC_RegToVoltageFloat(...)*10;
                                     *            // mv to 0.1mv based`，随后
                                     *            `*value = fval * vbat_factor`（vbat 通道 1:2 分压）。
                                     *   实测日志 `ch[7]voltage=37830;18942.146484` ⇒ 37830 = 18942.146×2
                                     *   ⇒ 37830 表示 3783.0mV = 3.783V，与曲线表 {18, 37826} 完全自洽。
                                     *   ⇒ 3.70V 应为 **37000**；旧值 3700 = 370.0mV，
                                     *     `mv <= 3700` **永远为假 ⇒ 该兜底分支从未生效过**。
                                     * ⚠️ 连带更正: 曲线表(battery_table.c)单位同样是 0.1mV，且**标定正确**
                                     *   (4.18V=100% / 3.90V=50% / 3.72V=10% / 3.50V=0%，标准 18650 曲线)，
                                     *   **不存在"偏高 200mV、会误关机"的问题**（那个判断是我的错误，已撤回）。 */
#define BAT_SHUTDOWN_CONFIRM  2    /* 连续 N 次确认再关机，防抖动误触。
                                    * ⚠️ **仅活跃态使用**（battery_monitor，500ms 节拍 ⇒ 代价 ~1s）。
                                    * 待机路径**不用**它：那里 1 次/周期，要求连续 N 次会让真低电多等
                                    * N 个周期（产量 1h ⇒ 2h），用户已明确否决（09-18 选 A 回退）。 */
#define BAT_MON_PERIOD_MS       500   /* 监控节拍（活跃态） */
#define BAT_MON_SLEEP_PERIOD_MS 10000 /* 监控节拍（深睡待机态）：10s 仍兼容 PM 深睡 */
/* 待机 RTC 定时唤醒查电量周期 = 1 小时（正式值）。
 * 6h 间隔太粗: 电池从 ~3.8V 掉到 3.2V 常在几小时内完成, 每次恰好采样到 >5% 就会整晚不关机
 * (用户实测"睡一晚到 3.2V 都没自动关机")。1h 的唤醒开销可忽略(每次约 0.4s 活动 / 3600s)。
 * 09-16: 曾临时改 60s 排查"RTC 到期唤不醒"(根因=RTC 从未初始化, 已修) —— 已改回 1h。
 * 09-18: 又临时改 60s 验证"分频链修正 + 武装自检"是否真能让 WUT 到点唤醒。
 *        ✅ 验证已完成，**已复位为 (1 * 3600)**。
 * ⚠️ 实际时长由 `rtc_calibrate_once()` 实测的 ms/count 换算(1h ≈ 1949 counts, 远小于 65535 上限),
 *    故这里写的秒数是**真实秒数**。 */
#define STANDBY_BAT_CHECK_PERIOD_S (1 * 3600)

/* ============================================================
 * POWER_DEBUG_DIAG ★ 电源深度排查诊断总开关 —— **正式值 = 0（关闭）**。
 *
 * 09-18 收尾：此前为定位「待机 mA 级异常 / 关机随机失败」而加的一大批诊断打印
 * 与探针，验证完成后**统一收进本开关**，默认全部不编译，避免：
 *   ① 刷屏拖慢待机时序（尤其 `mdelay` 轮询与关中断前的大段打印）；
 *   ② 探针里含**真实写操作**（`ISSR |= HP2LP_REQ`），不该出现在量产流程里。
 *
 * 置 1 时会启用（全部只影响日志/探针，不改变正常逻辑）：
 *   · `power_dump_standby_state()` 整函数 + 其 5 个调用点
 *     （rtc-woke-light / pre-rearm-2nd / pre-deep-2nd / pre-deep-1st / just-woke）
 *   · `[POWER] standby stats: deep_wakes=...`
 *   · `pm_shutdown()` 的面包屑 step1~5
 *   · `pm_shutdown()` 的 LCPU 唤醒探针（**含 ISSR.HP2LP_REQ 写操作**）
 *
 * ⚠️ 保留不随开关变化的打印（都是**低频、有诊断价值且无害**的）：
 *   · 电量/关机判定、`LDO3 domain OFF/ON`、`analog domain ON/OFF`、
 *     `RTC standby wake: battery=...`、`shutdown: calling HAL_PMU_EnterShutdown()`
 *   · 与根因修复绑定的**告警行**（`stale AUDIO scenario ... -> clearing`）
 *     —— 它们正常应"永不出现"，一旦出现就是 bug 复现的信号，必须能看到。
 *
 * 排查电源问题时把本行改为 1，重新编译即可（固件由用户用 Qt Creator / scons 构建）。 */
#define POWER_DEBUG_DIAG   0

/* ============================================================
 * DEBUG_KEEP_AWAKE — 调试用临时开关，调试结束后整行删掉，勿提交。
 * 打开后：sleep_thread 不再走“60s 无连接 -> 释放 PM 锁 -> 进深睡待机”
 * 流程，改为永久持有 PM_SLEEP_MODE_IDLE，设备常醒、所有外设不掉电、
 * BLE 栈保持运行，便于看日志 / 抓交互。其它逻辑（低电量、广播宽限等）不变。
 * ============================================================ */
// #define DEBUG_KEEP_AWAKE

static uint8_t g_is_powered_on = 1;
static uint8_t g_bt_connected = 0;
static uint8_t g_bat_mon_sleep = 0;   /* 1=深睡待机态：电池监控降频，允许 PM 进深睡 */
static volatile uint8_t g_standby_rtc_wake = 0; /* 09-09: 本次 DEEP 唤醒=RTC 电量检查(BSP_PowerUpCustom 置位) */

/* DEEP 睡眠处理器（bf0_pm.c, __WEAK）：保存 Flash 状态→掉 HPSYS 域→__WFI()→唤醒后恢复。
 * 本固件**不直接裸调**本函数（裸调会被 LPAON 闸门挡下且永远返回 -1，无法判睡/没睡），
 * 而是走 PM 框架：待机窗口注册 DEEP policy（见 sleep_thread_entry）→ idle 线程选 DEEP →
 * 框架调 sifli_sleep→sifli_deep_handler 进入。
 * ⚠️ 真正的拦路虎在更早的设备挂起阶段：sifli_suspend() 检查 WSR&WER 与 IPC 队列，
 * 运行态 WSR 无人清（AON IRQ 平时关闭），必须待机入口主动清 + 等 LCPU 安静，
 * 见 power_wait_lcpu_quiesce()。 */
extern int32_t sifli_deep_handler(void);

/* 覆盖 SDK 的 __WEAK sifli_light_handler。
 * SDK 版调用 clear_interrupt_setting() 后只开了 AON_IRQn，GPIO1_IRQn(按键)未开，
 * 导致 LIGHT 下按键无法唤醒。此版多开 GPIO1_IRQn。
 *
 * ⚠️ LIGHT 不做 Flash DPD：BSP_IO_Power_Down 会把 NOR_FLASH2 进 DPD，但 __WFI() 后的
 * 恢复代码(BSP_Power_Up)在 Flash 上执行(XIP)，Flash DPD 期间取指失败 → 死锁/HardFault。
 * LIGHT 不隔离 PAD，Flash 保持供电无需 DPD；仅 DEEP 路径(BSP_PowerDownCustom is_deep_sleep=true)
 * 才做 DPD（DEEP 唤醒由 boot ROM 释放 DPD）。
 *
 * ⚠️ LIGHT 不调 BSP_IO_Power_Down 的另一个原因：它会重新配 PA28(L3键)为下拉，
 * 破坏 button 库的按键中断配置。LIGHT 下外设不掉电，不需要 Power_Down。 */
extern uint32_t g_lscr;
extern uint32_t iser_bak[16];
void sifli_light_handler(void)    /* 强符号覆盖 SDK 的 __WEAK 版 */
{
    uint32_t dll1_freq, dll2_freq;
    int clk_src, i;

    /* 备份并禁用所有 NVIC 中断（SDK 原版行为，避免 WFI 被无关中断打醒） */
    for (i = 0; i < 16; i++) { iser_bak[i] = NVIC->ISER[i]; NVIC->ICER[i] = 0xFFFFFFFF; }
    __DSB(); __ISB();

    /* 仅使能唤醒源：AON(LPTIM/RTC) + GPIO1(按键)。LIGHT 不隔离 PAD，GPIO1 有电。 */
    NVIC_EnableIRQ(AON_IRQn);
    NVIC_EnableIRQ(GPIO1_IRQn);

    /* 降频：切 HRC48、关 DLL1/DLL2，降低 LIGHT 下静态功耗 */
    clk_src = HAL_RCC_HCPU_GetClockSrc(RCC_CLK_MOD_SYS);
    HAL_RCC_HCPU_ClockSelect(RCC_CLK_MOD_SYS, RCC_SYSCLK_HRC48);
    dll1_freq = HAL_RCC_HCPU_GetDLL1Freq();
    dll2_freq = HAL_RCC_HCPU_GetDLL2Freq();
    HAL_RCC_HCPU_DisableDLL1(); HAL_RCC_HCPU_DisableDLL2();

    HAL_HPAON_CLEAR_HP_ACTIVE();
    HAL_HPAON_EnterLightSleep(g_lscr);
    __WFI();
    __NOP(); __NOP(); __NOP(); __NOP(); __NOP();
    __NOP(); __NOP(); __NOP(); __NOP(); __NOP();

    /* 唤醒恢复：先恢复时钟（HRC48 已在跑，可安全切回 DLL），再恢复 NVIC */
    HAL_HPAON_SET_HP_ACTIVE(); HAL_HPAON_CLEAR_POWER_MODE();
    HAL_RCC_HCPU_EnableDLL1(dll1_freq);
    HAL_RCC_HCPU_ClockSelect(RCC_CLK_MOD_SYS, clk_src);
    HAL_RCC_HCPU_EnableDLL2(dll2_freq);
    for (i = 0; i < 16; i++) { __COMPILER_BARRIER(); NVIC->ISER[i] = iser_bak[i]; __COMPILER_BARRIER(); }
}

/* 注：曾尝试覆盖 sifli_deep_handler 去掉 PA_ISO（HAL_HPAON_DISABLE_PAD）以修复按键唤醒，
 * 实测功耗上升且唤醒仍失败 → PA_ISO 非根因，已撤销覆盖，恢复 SDK 原版 DEEP 流程。 */
static rt_thread_t g_sleep_thread = RT_NULL;
int g_in_deep_sleep = 0;                   /* 待机标志：当前是否处于待机（供 key_config.c 唤醒判断） */
int g_wakeup_requested = 0;                 /* 待机唤醒请求：按键唤醒后置位 */
int g_standby_active = 0;                  /* 待机激活：置位后各周期线程循环顶部自行 suspend，消除其定时器以达 tick==RT_TICK_MAX */
/* ★★★ 09-18 新增：待机"准备期"标志（power_enter_standby 从进入到真正阻塞之间）。
 *
 * 为什么需要它（本日实测 BUG 的真根因）：
 *   `power_enter_standby()` 的准备期很长——首轮要跑 RTC 校准/自检（日志实测 ~20s）。
 *   而 `g_standby_active` 是在**校准之后**才置位的（power.c L1227，在
 *   `standby_rtc_arm_timer()` L1211 之后）。于是整个校准窗口内 `g_standby_active == 0`，
 *   而 `power_standby_wakeup()` 的第一行守卫 `if (!g_standby_active) return;` 会把
 *   这段时间到达的**真按键**误判为"活跃态按键"直接丢弃 ⇒ `g_wakeup_requested` 永远
 *   置不上 ⇒ 用户在准备期按键无法中止待机，设备仍被推进 DEEP。
 *   实测后果：BLE 已重连(live link)/小核醒着时仍 `[pm]S:3`，随后 IMU/I2C 被待机清场
 *   电源循环 ⇒ `i2c bus err` / `WHO_AM_I read failed: -8` / `lsm_init failed!`。
 *
 * 语义：仅把"准备期"也认作待机流程（只影响 power_standby_wakeup 的守卫），
 *       **不动 `g_standby_active`**（它还管周期线程 suspend，语义不能扩）。
 * 时序：`power_enter_standby()` 入口置 1；函数正常出口与"中止"出口都要清 0。 */
static volatile uint8_t g_standby_prepare = 0;

/* DEEP 唤醒源诊断计数（由 BSP_PowerUpCustom 更新，resume 时打印）。
 * spurious = 非 GPIO1（LPTIM1 等内部定时器）唤醒次数，正常待机期应随看门狗周期缓慢增长。 */
volatile uint32_t g_last_deep_wsr = 0;
volatile uint32_t g_deep_wake_cnt = 0;
volatile uint32_t g_deep_spur_cnt = 0;

static void power_resume_from_deep(void);   /* 唤醒后外设重建（定义见文件后部） */
static void led4_set(int on);               /* LED4 红灯直控（定义见文件后部）：
                                             * standby_rtc_battery_check 的关机指示(H4) 要用，
                                             * 而该函数在文件前部 ⇒ 需前置声明（否则 implicit
                                             * declaration 与后面的 static 定义冲突）。 */

extern uint32_t air_mouse_idle_ms(void);   /* main.c: 连接态连续空闲毫秒(08-27 空闲超时休眠判定用) */

/* 待机唤醒信号量：sleep_thread 待机时阻塞于此（无定时器，不干扰 DEEP 的 tick==MAX 判定）；
 * 唤醒源(DEEP: BSP_PowerUpCustom / LIGHT: key_handler) 调 power_standby_wakeup() release 之。 */
static rt_sem_t g_standby_sem = RT_NULL;

/* 待机期需挂起的周期线程句柄注册表（供唤醒时统一 rt_thread_resume）。
 * 这些线程在 g_standby_active 置位后于循环顶部自行 rt_thread_suspend 自身。 */
#define MAX_SUSPEND_THREADS 8
static rt_thread_t g_suspend_threads[MAX_SUSPEND_THREADS];
static int g_suspend_thread_cnt = 0;
void power_register_suspend_thread(rt_thread_t t)
{
    if (t && g_suspend_thread_cnt < MAX_SUSPEND_THREADS)
        g_suspend_threads[g_suspend_thread_cnt++] = t;
}

/* 统一唤醒入口：置唤醒标志 + release 待机信号量。供 DEEP(LIGHT) 唤醒路径调用。
 * ⚠️ 守卫范围 = `g_standby_active`（已进入阻塞/待机）**或** `g_standby_prepare`
 *   （power_enter_standby 的准备期，含首轮 ~20s RTC 校准）。
 *   09-18 修：原先只看 `g_standby_active`，而该标志在校准**之后**才置位 ⇒ 准备期内的
 *   真按键被当成"活跃态按键"丢弃，用户按了键也拦不住进 DEEP。见 g_standby_prepare 注释。
 *   活跃态（两者皆 0）仍然忽略——这条保护必须保留：活跃态按键也走 key_handler→本函数，
 *   无保护会积累 sem 计数/误置唤醒标志。 */
void power_standby_wakeup(void)
{
    if (!g_standby_active && !g_standby_prepare) return;   /* 既非待机也非准备期：忽略 */
    g_wakeup_requested = 1;
    if (g_standby_sem) rt_sem_release(g_standby_sem);
}

/* 待机 DEEP policy：任意待定定时器 >=2ms 即选 DEEP（tickless：唤醒后由下一定时器再唤醒）。
 * 仅在待机窗口注册；唤醒后恢复为 LIGHT-only policy，避免活跃态误入 DEEP。 */
static const pm_policy_t g_standby_deep_policy[] = { {2, PM_SLEEP_MODE_DEEP} };
/* 唤醒后恢复为 IDLE 策略（不引用被禁用的 LIGHT 模式）。
 * 活跃态靠 rt_pm_request(IDLE) 锁短路，框架永不误选 DEEP；仅待机窗口显式切 g_standby_deep_policy。
 * 注：SF32LB52 vendor 默认选 DEEP 为最深模式（PM_LIGHT_ENABLE 与 PM_DEEP_ENABLE 互斥，已取 DEEP），
 * 故活跃态正常只到 IDLE(WFI)，LIGHT 不参与。 */
static const pm_policy_t g_normal_policy[]       = { {2, PM_SLEEP_MODE_IDLE} };

/* LXT 关闭时 LP/RC32K 时钟随温度/电压漂移；周期重校保证 DEEP/RTC 唤醒计时精度。
 * 参考 SDK example/get-started/dualcore（其 LCPU 每 15s 重校）。本固件无 LCPU 应用源码，
 * 故在 HCPU 侧周期重校；启动的一次性校准已由 boards/sf32lb52-vibekey/bsp_init.c::LRC_init() 完成。 */
static rt_timer_t g_rc_cal_timer = RT_NULL;

/* 09-16: RW BLE 主机栈的 "BLEHost" 软定时器句柄(待机时停、唤醒后放开) —— 见待机入口注释 */
static rt_timer_t g_ble_host_timer = RT_NULL;

/* ===== DEEP 前置：WSR/IPC 静默（"看不到 [pm]S:3" 的真正根因） =====
 * HCPU 版 sifli_suspend()（bf0_pm.c:2648）在设备挂起阶段就检查两个条件，任一不满足
 * 即返回 EBUSY，框架据此把 DEEP 降级为 IDLE（pm.c:454），于是永远打不出 [pm]S:3：
 *   1) ipc_queue_check_idle()==false —— HCPU<->LCPU 邮箱仍有未处理数据；
 *   2) HAL_HPAON_GET_WSR() & HAL_HPAON_GET_WER() != 0 —— 已使能唤醒源有事件挂起。
 * 而 HCPU 运行态唯一清 WSR 的地方是 AON_IRQHandler（bf0_pm.c:2699），该中断平时被
 * 自禁用、仅在真正进入睡眠流程时才重新使能 —— 运行态 LCPU 每发一次 IPC（BLE 事件、
 * close_bt 应答等）就把 LP2HP_REQ/IRQ 位挂进 WSR 且无人清。位一旦挂上，DEEP 永久
 * 不可达。故待机入口最后必须：清残留位 + 等 LCPU 停发 IPC（连续多次干净才放行）。 */
#include "ipc_queue.h"   /* bool ipc_queue_check_idle(void) —— HCPU<->LCPU 邮箱空闲检查 */

/* 小核(LCPU)是否已睡：HPSYS_AON->ISSR.LP_ACTIVE（0x500c0000，大核 AON 域常供电，可安全读）。
 * LCPU 各 sleep handler 进睡前 CLEAR、醒来 SET。1=运行中，0=已睡。
 * ⚠️ 铁律：绝不能读 hwp_lpsys_aon（0x4004xxxx，LP 域），小核睡下去后总线不应答 -> HardFault。 */
int power_lcpu_is_sleeping(void)
{
    return (hwp_hpsys_aon->ISSR & HPSYS_AON_ISSR_LP_ACTIVE) ? 0 : 1;
}

static void power_dump_wsr(const char *tag)
{
    uint32_t wsr  = HAL_HPAON_GET_WSR();
    uint32_t wer  = HAL_HPAON_GET_WER();
    uint32_t pend = wsr & wer;
    /* ⚠️ 必须拆成两行：rt_kprintf 单次输出受 RT_CONSOLEBUF_SIZE(128) 限制，
     * 合成一行会被硬截断（历史现象：日志停在 "ipc_" 处）。 */
    rt_kprintf("[POWER] %s WSR=0x%08x WER=0x%08x pend=0x%08x\n",
               tag, wsr, wer, pend);
    rt_kprintf("[POWER] %s RTC=%d GPIO1=%d LPTIM1=%d PMUC=%d LP2HP=%d/%d PIN=0x%08x ipc_idle=%d lcpu_sleep=%d\n",
               tag,
               !!(pend & HPSYS_AON_WSR_RTC),    !!(pend & HPSYS_AON_WSR_GPIO1),
               !!(pend & HPSYS_AON_WSR_LPTIM1), !!(pend & HPSYS_AON_WSR_PMUC),
               !!(pend & HPSYS_AON_WSR_LP2HP_REQ), !!(pend & HPSYS_AON_WSR_LP2HP_IRQ),
               (unsigned)HAL_HPAON_GET_WSR_PIN(),
               (int)ipc_queue_check_idle(), power_lcpu_is_sleeping());
}

/*==============================================================================
 * LDO3(PMU_PERI_LDO3_3V3 / VOUT2) 3.3V 外设域整域断电
 *============================================================================*/
/* 该域负载：LSM6DS3TR-C(IMU)、WS2812(RGB)、I2C1 外部上拉、编码器 A/B 外部上拉。
 * 待机期整域断电可省掉上拉电阻分压电流 + WS2812 静态电流（实测待机 630µA 的主要来源）。
 *
 * ⚠️ 关断顺序不可颠倒（历史教训：曾直接 ConfigPeriLdo(LDO3,false) 一关了事，电流
 *    不降反升，于是放弃并在 main.c air_mouse_stop 留下 "LDO3 kept on" 的注释）：
 *
 *   【反灌 backfeed 原理】MCU 引脚的内部上拉接的是主域 VDD33(LDO2/VOUT1)，而外部
 *   上拉电阻的另一端接 LDO3。LDO3 一断电就形成通路：
 *       VDD33 → 内部上拉 → 引脚 → 外部上拉 → LDO3 轨 → LSM/WS2812 的 ESD 二极管
 *   把已断电的 LDO3 轨"寄生供电"抬到 0.7~2V。器件处于欠压态，既不工作也不休眠，
 *   内部逻辑乱翻转，漏电比正常供电时还大。
 *   【浮空直通电流】无外部上拉的脚(INT1/DIN)若仅置 NOPULL 会真浮空，输入缓冲器
 *   停在 Vil~Vih 之间产生 P/N 管直通电流，每脚可达几十~几百 µA。
 *
 * 故必须严格三步：
 *   1) 全域引脚切 GPIO 输入 + NOPULL —— 去掉内部上拉，切断反灌通路；
 *   2) 关 LDO3，等轨电容放电到 0V；
 *   3) 再把引脚驱动为输出低 —— 与 0V 轨同电位，既不灌电流也不浮空。
 * 恢复时反着来：先撤输出低回到输入，再上电、等 IMU boot，最后恢复原复用/上拉。 */

/* LDO3 域上的 MCU 引脚（与 bsp_pinmux.c 一致）：
 *   PA02=ENC_A  PA04=ENC_B (外部上拉到 LDO3)
 *   PA10=I2C1_SDA PA11=I2C1_SCL (外部上拉到 LDO3)
 *   PA26=LSM INT1 (无外部上拉)   PA00=WS2812 DIN (无外部上拉) */
static const uint8_t g_ldo3_pins[] = { 2, 4, 10, 11, 26, 0 };
#define LDO3_PIN_CNT   ((int)(sizeof(g_ldo3_pins) / sizeof(g_ldo3_pins[0])))

static volatile uint8_t g_ldo3_off = 0;

int power_ldo3_is_off(void)
{
    return g_ldo3_off;
}

void power_ldo3_domain_off(void)
{
    int i;
    if (g_ldo3_off) return;

    /* 1) 去内部上拉 + 变输入：切断 VDD33 经内/外上拉向 LDO3 轨的反灌通路 */
    for (i = 0; i < LDO3_PIN_CNT; i++)
    {
        uint8_t p = g_ldo3_pins[i];
        HAL_PIN_Set(PAD_PA00 + p, GPIO_A0 + p, PIN_NOPULL, 1);
        rt_pin_mode(p, PIN_MODE_INPUT);
    }

    /* 2) 关 LDO3，忙等 2ms 让轨电容放电。
     *    用 HAL_Delay_us 而非 rt_thread_mdelay：待机路径上不要再武装线程定时器。 */
    HAL_PMU_ConfigPeriLdo(PMU_PERI_LDO3_3V3, false, false);
    HAL_Delay_us(2000);

    /* 3) 驱动为确定低电平：此时轨已 0V，不灌电流，同时消除浮空输入直通电流 */
    for (i = 0; i < LDO3_PIN_CNT; i++)
    {
        rt_pin_mode(g_ldo3_pins[i], PIN_MODE_OUTPUT);
        rt_pin_write(g_ldo3_pins[i], PIN_LOW);
    }

    g_ldo3_off = 1;
    rt_kprintf("[POWER] LDO3 domain OFF (IMU/RGB/I2C+ENC pullups unpowered)\n");
}

void power_ldo3_domain_on(void)
{
    int i;
    if (!g_ldo3_off) return;
    g_ldo3_off = 0;   /* 先清标志：BSP_ImuPowerWanted 据此放行后续上电 */

    /* 先撤掉输出低、回到输入无上拉：避免 LDO3 上电瞬间 MCU 仍在灌低电平，
     * 与外部上拉及器件输出对冲（瞬时几 mA 且可能让 IMU boot 失败）。 */
    for (i = 0; i < LDO3_PIN_CNT; i++)
    {
        uint8_t p = g_ldo3_pins[i];
        HAL_PIN_Set(PAD_PA00 + p, GPIO_A0 + p, PIN_NOPULL, 1);
        rt_pin_mode(p, PIN_MODE_INPUT);
    }

    HAL_PMU_ConfigPeriLdo(PMU_PERI_LDO3_3V3, true, true);
    rt_thread_mdelay(20);   /* LSM6DS3 上电 boot ~15ms，留裕量后 I2C 才可访问 */

    /* 恢复各脚原复用/上拉（与 bsp_pinmux.c 一致）。
     * PA00(WS2812 DIN) 不恢复 PWM 复用：ws2812b 每次使用时自重配，
     * 保持 GPIO 输出低就是它的正确静默态。 */
    HAL_PIN_Set(PAD_PA02, GPIO_A2,  PIN_PULLUP, 1);   /* ENC_A */
    HAL_PIN_Set(PAD_PA04, GPIO_A4,  PIN_PULLUP, 1);   /* ENC_B */
    HAL_PIN_Set(PAD_PA10, I2C1_SDA, PIN_PULLUP, 1);
    HAL_PIN_Set(PAD_PA11, I2C1_SCL, PIN_PULLUP, 1);
    HAL_PIN_Set(PAD_PA26, GPIO_A26, PIN_PULLUP, 1);   /* LSM INT1 */
    rt_pin_mode(2,  PIN_MODE_INPUT_PULLUP);
    rt_pin_mode(4,  PIN_MODE_INPUT_PULLUP);
    rt_pin_mode(26, PIN_MODE_INPUT_PULLUP);
    rt_pin_mode(0,  PIN_MODE_OUTPUT);
    rt_pin_write(0, PIN_LOW);

    rt_kprintf("[POWER] LDO3 domain ON\n");
}

/* ============================================================================
 * 模拟域断电：片内充电器 + 音频 codec REFGEN + GPADC（第十三根因, 2026-07-31）
 *
 * 待机 230µA 中大部分来自芯片内部模拟模块在 DEEP 期间仍保持偏置：
 *  - PMU 充电器(CHG_CR3)：sifli_charge 驱动初始化后即使没插 USB(VBUS=0)，
 *    内部基准/比较器仍偏置 → 几十~上百 µA。FORCE_RST 把整个 charger 模块
 *    拉进复位态，彻底断模拟偏置。
 *  - AUDCODEC REFGEN(AUDCODEC_REFGEN_CFG.EN)：音频 codec 的 VREF/偏置发生器，
 *    CONFIG_AUDIO=y + BSP_ENABLE_AUDCODEC=1 下初始化后一直开着 → 上百 µA。
 *    关掉 EN 即切断所有 ADC/DAC/HP/LP 通道的模拟偏置电流。
 *  - GPADC(ADC_CFG_REG1)：CONFIG_BSP_USING_ADC1=1 下初始化后模拟前端仍偏置。
 *
 * 保存/恢复策略：off 时保存寄存器原值再清零使能位；on 时写回原值。
 * 唤醒后音频子系统会在下次播放/录音时重配 codec 细节，但 REFGEN 恢复
 * 可保证立刻可用，无需等完整 reinit。 */
static struct {
    uint32_t chg_cr3;
    uint32_t chg_cr4;   /* 08-31: 充电器中断使能位(IE_*)所在寄存器, FORCE_RST 会清掉 */
    uint32_t audcodec_cfg;
    uint32_t audcodec_refgen;
    uint32_t gpadc_cfg1;
    uint8_t  active;
} g_analog_save;

void power_analog_domain_off(void)
{
    if (g_analog_save.active) return;   /* 已断，不重复 */

    /* 1. 充电器 FORCE_RST（bit30）→ 模块复位，断基准/比较器偏置
     * 08-31: 连 CHG_CR4 一起保存。CHG_CR4 是充电器内部寄存器(IE_* 中断使能位)，
     * FORCE_RST 会把它打回复位默认值(不受控, 很可能重新打开 EOC 中断)。此前只
     * 保存 CR3，导致深睡唤醒后 EOC 中断重新炸开、ISR 里 LOG_I 刷屏拖死系统。 */
    g_analog_save.chg_cr3 = hwp_pmuc->CHG_CR3;
    g_analog_save.chg_cr4 = hwp_pmuc->CHG_CR4;
    hwp_pmuc->CHG_CR3 = g_analog_save.chg_cr3 | PMUC_CHG_CR3_FORCE_RST;

    /* 2. AUDCODEC：先关 ADC/DAC 通道，再关 REFGEN（VREF/偏置发生器）
     *    REFGEN.EN 是总闸，关掉后所有模拟通道的偏置电流归零。 */
    g_analog_save.audcodec_cfg   = hwp_audcodec->CFG;
    g_analog_save.audcodec_refgen = hwp_audcodec->REFGEN_CFG;
    hwp_audcodec->CFG       = g_analog_save.audcodec_cfg
                            & ~(AUDCODEC_CFG_ADC_ENABLE | AUDCODEC_CFG_DAC_ENABLE);
    hwp_audcodec->REFGEN_CFG = g_analog_save.audcodec_refgen
                            & ~AUDCODEC_REFGEN_CFG_EN;

    /* 3. GPADC：清零模拟使能位（CMREF_FAST_EN / P_INT_EN / EN_V18 等） */
    g_analog_save.gpadc_cfg1 = hwp_gpadc->ADC_CFG_REG1;
    hwp_gpadc->ADC_CFG_REG1  = 0;

    g_analog_save.active = 1;
    rt_kprintf("[POWER] analog domain OFF (charger+codec+gpadc)\n");
}

void power_analog_domain_on(void)
{
    if (!g_analog_save.active) return;
    g_analog_save.active = 0;

    /* 恢复顺序与断电相反：先 GPADC，再 codec REFGEN+ADC/DAC，最后充电器 */
    hwp_gpadc->ADC_CFG_REG1   = g_analog_save.gpadc_cfg1;
    hwp_audcodec->REFGEN_CFG  = g_analog_save.audcodec_refgen;
    hwp_audcodec->CFG          = g_analog_save.audcodec_cfg;

    /* 充电器：先撤 FORCE_RST 让模块退出复位，趁模块活着再把 CR4 写回去。
     * 顺序反了的话 CR4 写入会被复位态吞掉。 */
    hwp_pmuc->CHG_CR3          = g_analog_save.chg_cr3;
    hwp_pmuc->CHG_CR4          = g_analog_save.chg_cr4;

    /* 双保险：即便 CR4 恢复异常(或复位默认值带 IE)，这里再彻底关一次充电器中断。
     * main.c 的 charge_irq_disable_all() 为静态函数，这里直接操作同一组寄存器。
     * ⚠️ 掩码必须与 main.c 的策略保持一致：USB_WAKE_BY_PMUC=1 时 VBUS_RDY 是
     *    PMUC 唤醒源的事件源，**保留**它；置 0 时才一并清掉。 */
    HAL_NVIC_DisableIRQ(PMUC_IRQn);
    hwp_pmuc->CHG_CR4 &= ~(PMUC_CHG_CR4_IE_VBAT_HIGH_Msk |
                           PMUC_CHG_CR4_IE_ABOVE_REP_Msk |
                           PMUC_CHG_CR4_IE_ABOVE_CC_Msk  |
                           PMUC_CHG_CR4_IE_CC_MODE_Msk   |
                           PMUC_CHG_CR4_IE_CV_MODE_Msk   |
                           PMUC_CHG_CR4_IE_EOC_Msk
#if !USB_WAKE_BY_PMUC
                           | PMUC_CHG_CR4_IE_VBUS_RDY_Msk
#endif
                          );
    NVIC->ICPR[PMUC_IRQn >> 5] = (1UL << (PMUC_IRQn & 0x1FUL));
    __DSB();
    __ISB();

    rt_kprintf("[POWER] analog domain ON (charger+codec+gpadc)\n");
}

/* 等 LCPU/BLE 真正安静：清残留 WSR，连续 stable_need 次(pend==0 且 IPC 空)才算成功。
 * 返回 1=安静可进 DEEP；0=超时（DEEP 将被 sifli_suspend 降级为 IDLE）。 */
static int power_wait_lcpu_quiesce(int stable_need, int max_ms)
{
    int stable = 0, waited = 0;
    while (waited < max_ms)
    {
        uint32_t pend = HAL_HPAON_GET_WSR() & HAL_HPAON_GET_WER();
        if (pend)
            HAL_HPAON_CLEAR_WSR(HAL_HPAON_GET_WSR());  /* 事件已由各外设 ISR 处理过，清除安全 */
        if (!pend && ipc_queue_check_idle())
            stable++;
        else
            stable = 0;
        if (stable >= stable_need)
            return 1;
        rt_thread_mdelay(100);
        waited += 100;
    }
    return 0;
}

static void led_all_off(void)
{
    // HAL_GPIO_WritePin(hwp_gpio1, BSP_LED1_PIN, GPIO_PIN_RESET);
    // HAL_GPIO_WritePin(hwp_gpio1, BSP_LED2_PIN, GPIO_PIN_RESET);
    // HAL_GPIO_WritePin(hwp_gpio1, BSP_LED3_PIN, GPIO_PIN_RESET);
    // HAL_GPIO_WritePin(hwp_gpio1, BSP_LED4_PIN, GPIO_PIN_RESET);
}

static void led_all_on(void)
{
    // HAL_GPIO_WritePin(hwp_gpio1, BSP_LED1_PIN, GPIO_PIN_SET);
    // HAL_GPIO_WritePin(hwp_gpio1, BSP_LED2_PIN, GPIO_PIN_SET);
    // HAL_GPIO_WritePin(hwp_gpio1, BSP_LED3_PIN, GPIO_PIN_SET);
    // HAL_GPIO_WritePin(hwp_gpio1, BSP_LED4_PIN, GPIO_PIN_SET);
}

/* 08-29: LED1(PA5) 不再做 BLE 连接指示 —— 连接/配对/HFP 状态已由槽位 RGB
 * (bt_slot.c 的 WS2812B 蓝/青/黄闪)覆盖。LED1 改为充电指示, 由 bt_slot.c 的
 * bt_multi_set_charge() 驱动(充电中亮、充满灭)。原闪烁定时器整套已移除。 */

/* LXT 关闭时周期重校 LP/RC 参考周期（15s 触发）。若 LXT 启用则停止定时器（无需重校）。
 * 任何蓝牙链路在线(BLE 连接/HFP/SCO)都跳过：HAL_RC_CAL_update_reference_cycle_on_48M
 * 会切 LCPU(BT 协议栈小核)系统时钟到 HXT48 + 劫持 BLE MAC 忙等(bf0_hal_lrc_cal.c)，
 * 与随机操作(SCO 建链/HID 上报/AT 命令)撞车 = 偶发丢包/失灵(08-06 定案偶发根因)。
 * 校准仅服务于深睡计时精度，而深睡时 tickless 冻结本软定时器本就不触发 →
 * 蓝牙在线期间做校准纯有害无益，直接跳过。 */
static void rc_cal_timer_cb(void *param)
{
    if (g_sco_active || g_bt_connected || ble_app_hfp_connected())
        return;     /* 通话/BLE/HFP 任一在线：跳过本次，周期定时器下轮继续 */
    if (HAL_LXT_DISABLED())
    {
        HAL_RC_CAL_update_reference_cycle_on_48M(LXT_LP_CYCLE);
    }
    else if (g_rc_cal_timer)
    {
        rt_timer_stop(g_rc_cal_timer);
    }
}

/* ============================================================================
 * 待机 RTC 定时电量检查（09-09 方案 B）
 *
 * 解决的盲区：待机期 bat_mon 线程自挂起（g_standby_active），设备可以一睡
 * 很多天，电量耗尽也不会自动关机。方案 B：进待机前用 RTC WakeUpTimer 设一个
 * 6h 的一次性闹钟，到点硬件把系统从 DEEP 里拽出来 → 轻量查一次电量 →
 * ≤5% 且未插充电器就进 Hibernate，否则重新武装定时器继续睡。
 *
 * 轻量路径设计（不走 power_resume_from_deep 全量重建）：
 *   - 不重开 BLE/经典蓝牙/USB/LDO3，只恢复 GPADC 所在的模拟域采样一次电压；
 *   - 检查期间持 IDLE 锁（待机期 LPTIM1 唤醒源已关、tickless 下 mdelay 计不了
 *     时，且不持锁框架会反复短暂进出 DEEP）；
 *   - 充电判断不读 bt_multi_charge_active()（RAM 标志在待机期是陈旧值），改读
 *     rt_charge_get_detect_status() —— sifli_charge 直读 PMUC->CHG_SR 的
 *     VBUS_RDY_OUT 活硬件位，AON 域常供电，即使充电器被 FORCE_RST 也能反映
 *     VBUS 是否插着；
 *   - 查完立刻 power_analog_domain_off()，不打扰后续待机电流。
 *
 * SDK 机制（bf0_hal_rtc.c / drv_rtc.c / bf0_hal_hpaon.c 核实）：
 *   - HAL_RTC_SetWakeUpTimer(&RTC_Handler, n, RTC_WAKEUP_SEC)：秒级纯递减 16 位
 *     计数器，最长 65535s≈18.2h，内部使能 RTC_IRQn + WUT 中断；
 *   - WUTF 由 drv_rtc 注册的 RTC_IRQHandler→HAL_RTC_IRQHandler 自动清标志，且
 *     drv_rtc_callback 收到 RTC_CBK_WAKEUP 会 DeactivateWakeUpTimer —— 一次性
 *     闹钟，每次唤醒后必须重新武装；
 *   - 唤醒源路由：HAL_HPAON_EnableWakeupSrc(HPAON_WAKEUP_SRC_RTC,..)=WER bit0，
 *     到点置 WSR_RTC(bit0) 把系统从 DEEP 唤醒；BSP_PowerUpCustom 据此分流。
 * ============================================================================ */
/* ============================================================================
 * 进 DEEP 前的"清场"(清 WSR 残留 + 等 LCPU/IPC 安静)。
 * ⚠️ 首次进待机【和每次 RTC 轻量唤醒回来之后】都必须执行:
 *   sifli_suspend 会检查 WSR&WER 与 IPC 队列, 任一不为空即 EBUSY → 框架把 DEEP
 *   **静默降级成 IDLE**。RTC 唤醒本身会重新搅动 AON/小核, 若回睡前不重新清一次, 就可能
 *   "被 RTC 唤醒后再也回不去 DEEP" → 整晚 IDLE 高电流(把电池耗干)。
 * quiesce 期间持 IDLE 锁: 轮询里的 mdelay(100) 会让框架反复进出 DEEP, 既耗电又刷屏。 */
static void standby_prepare_deep_gate(const char *tag, int max_ms)
{
    rt_pm_request(PM_SLEEP_MODE_IDLE);
    power_dump_wsr(tag);
    if (power_wait_lcpu_quiesce(5, max_ms))
        rt_kprintf("[POWER] %s: DEEP gate clear\n", tag);
    else
        power_dump_wsr("quiesce TIMEOUT (DEEP will degrade to IDLE!)");
    rt_pm_release(PM_SLEEP_MODE_IDLE);
}

extern RTC_HandleTypeDef RTC_Handler;   /* drv_rtc.c 全局句柄(BSP_USING_ONCHIP_RTC) */
/* drv_common.c: 读 RTC 硬件日历时间戳（内部正确走 HAL_RTC_GetTime/GetDate 的影子寄存器同步
 * 时序）。仅自检用，返回值当作 32 位无符号秒数打印/求差即可。 */
extern uint32_t drv_get_timestamp(void);

/* 09-16 诊断: 打印 RTC 唤醒定时器关键寄存器 —— 排查"待机 RTC 电量检查从不触发"
 * (实测: 待机 92 分钟、RTC 已 arm 3600s, 却一次都没被 RTC 唤醒, 无任何相关日志)。
 *   CR  : WUTE(使能)/WUTWF/WUCKSEL(时钟选择, SEC 应为 1Hz)
 *   WUTR: 载入的计数值(应为 3600)
 *   ISR : WUTF(到点标志)
 * 进睡前打一次、每次(重)武装后再打一次, 对比即可判断"没装上"还是"计数没在走"。 */
static void rtc_wut_dump(const char *tag)
{
    /* PSCLR 位域(52x): DIVB=bits[9:0], DIVA_FRAC=bits[23:10], DIVA_INT=bits[31:24]。
     * ⚠️ 09-16 已推翻"PSCLR 停在复位默认 128 ⇒ 0.27Hz"的推断：`DIVA=128` 正是
     * **32.768kHz 源**的正确分频(LXT_FREQ/DIVB)，`DIVA=34` 才是 RC10K(8867Hz) 的；
     * 驱动按 `HAL_RTC_LXT_ENABLED()` 在两者间取舍，128 很可能是对的。 */
    uint32_t p = (uint32_t)RTC_Handler.Instance->PSCLR;
    /* ⚠️ TR(日历时间)是**影子寄存器**(CR.BYPSHAD=0 时需 RSF 同步)，裸读恒 0 **不能**
     * 作为"RTC 停摆"的证据 —— 判断日历是否在走请用 drv_get_timestamp()(自检里已加)。 */
    rt_kprintf("[POWER] RTC(%s) CR=0x%08x WUTR=0x%08x ISR=0x%08x TR=0x%08x PSCLR=0x%08x(DIVA=%u FRAC=%u DIVB=%u)\n",
               tag,
               (unsigned)RTC_Handler.Instance->CR,
               (unsigned)RTC_Handler.Instance->WUTR,
               (unsigned)RTC_Handler.Instance->ISR,
               (unsigned)RTC_Handler.Instance->TR,
               (unsigned)p,
               (unsigned)((p >> 24) & 0xFF), (unsigned)((p >> 10) & 0x3FFF), (unsigned)(p & 0x3FF));
}

/* ============================================================================
 * 09-18 新增: 待机电流异常诊断（只读打印，不改变任何行为）。
 *
 * 背景（用户实测，逐行核对日志定案）：
 *   第一次 DEEP 正常（µA 级）；**RTC 低电量唤醒后重新入睡的第二次 DEEP 期间
 *   整机电流高达 26.5mA**，按键唤醒后才恢复正常。
 *   两条路径的「进 DEEP 前清场动作」差异极大：
 *     - 第一次 power_enter_standby(): 关 BT / 停 3 个软定时器 / 删 bat_rpt 线程 /
 *       熄 LED / 断 LDO3 / 断 analog / key_enable_deep_wakeup / 10s quiesce（全套）
 *     - 第二次 standby_rtc_battery_check(): **只有 analog on→off + 重武装 RTC +
 *       3s quiesce**，其余 13 项全省，且阻塞循环走 `continue` 直接回睡。
 *   但"少做清场"理论上不该让 µA 变 26.5mA（LDO3/BT/定时器在第一次已关且未重开）
 *   ⇒ 存在尚未定位的机制。本函数用于一次性抓齐判定所需的全部现场。
 *
 * 重点抓三样：
 *   ① 小核/时钟/PMR：DEEP 到底进没进、进了多深（HCPU 有没有真停表）。
 *   ② 各外设域：LDO2(VDD33)/LDO3/LDO1V8/PSRAM/Flash 的实际供电与低功耗态。
 *   ③ PM 锁引用计数现场：残留 IDLE 锁会让框架只能待在 IDLE（mA 级）。
 *
 * 用法：在"进 DEEP 之前"与"从 DEEP 醒来之后"各打一次，两两对比即知哪一项在
 * 第二次路径里与第一次不同。全部只读，无副作用（不写任何寄存器）。
 *
 * ★09-18 收尾：整函数收进 `POWER_DEBUG_DIAG`（默认 0 = 不编译）。
 *   关掉时保留一个空实现占位，这样 5 个调用点无需逐个包 `#if`（调用本身零开销）。 */
static void power_dump_standby_state(const char *tag)
{
#if POWER_DEBUG_DIAG
    /* ① AON/HPSYS：PMR 电源模式(0/1=? 2=DEEP 3=STANDBY)、ISSR 小核活跃位、
     *    WSR/WER 唤醒源现场（WSR&WER 非 0 ⇒ sifli_suspend 会 EBUSY → DEEP 降级 IDLE） */
    uint32_t pmr  = hwp_hpsys_aon->PMR;
    uint32_t issr = hwp_hpsys_aon->ISSR;
    uint32_t wsr  = HAL_HPAON_GET_WSR();
    uint32_t wer  = HAL_HPAON_GET_WER();

    rt_kprintf("[STBYDBG] %s PMR=0x%08x(mode=%lu) ISSR=0x%08x LP_ACTIVE=%d(lcpu_sleep=%d)\n",
               tag, (unsigned)pmr, (unsigned long)(pmr & 0x3u), (unsigned)issr,
               !!(issr & HPSYS_AON_ISSR_LP_ACTIVE), power_lcpu_is_sleeping());
    rt_kprintf("[STBYDBG] %s WSR=0x%08x WER=0x%08x pend=0x%08x ipc_idle=%d\n",
               tag, (unsigned)wsr, (unsigned)wer, (unsigned)(wsr & wer),
               (int)ipc_queue_check_idle());

    /* ② PMUC 外设域：PERI_LDO 各域使能位 + LPSYS 供电（0x500ca000，AON 常供电可读；
     *    ⚠️ 绝不能读 hwp_lpsys_aon(0x4004xxxx)，小核睡下后总线不应答 → HardFault） */
    rt_kprintf("[STBYDBG] %s PERI_LDO=0x%08x  LPSYS_LDO=0x%08x(EN=%d) LPSYS_SWR=0x%08x\n",
               tag, (unsigned)hwp_pmuc->PERI_LDO,
               (unsigned)hwp_pmuc->LPSYS_LDO,
               !!(hwp_pmuc->LPSYS_LDO & PMUC_LPSYS_LDO_EN),
               (unsigned)hwp_pmuc->LPSYS_SWR);

    /* ★★ 09-18 定案关键：LDO3 的**硬件**状态 vs 软件标志。
     * PERI_LDO 位定义(pmuc.h)：
     *   bit16 = EN_VDD33_LDO3   （1=上电 / 0=关断）
     *   bit17-20 = LDO3 VOUT 档位
     *   bit21 = VDD33_LDO3_PD   （1=断电/关 / 0=工作）
     * ⚠️ power_ldo3_is_off() 返回的 g_ldo3_off 只是**软件标志**，SDK 的
     *    BSP_Power_Up() 会绕过它直接 HAL_PMU_ConfigPeriLdo() 重开硬件 —— 故二者
     *    可能不一致，必须以寄存器位为准。同时打印 g_imu_power_wanted
     *    （BSP_ImuPowerWanted() 的返回值，决定 BSP_Power_Up 是否重开 LDO3）。 */
    {
        extern int BSP_ImuPowerWanted(void);
        int ldo3_en = !!(hwp_pmuc->PERI_LDO & PMUC_PERI_LDO_EN_VDD33_LDO3);
        int ldo3_pd = !!(hwp_pmuc->PERI_LDO & PMUC_PERI_LDO_VDD33_LDO3_PD);
        rt_kprintf("[STBYDBG] %s LDO3 HW: EN=%d PD=%d  => %s | sw_off=%d imu_want=%d\n",
                   tag, ldo3_en, ldo3_pd,
                   (ldo3_en && !ldo3_pd) ? "*** POWERED ON ***" : "off",
                   power_ldo3_is_off(), BSP_ImuPowerWanted());
    }

    /* ③ 模拟域现场（充电器/codec/GPADC）。用**寄存器实际值**而非软件标志判断，
     *    因为标志可能被别处改动（g_analog_save 是本文件 static，只在 off/on 里动）：
     *      - CHG_CR3.FORCE_RST=1  ⇒ 充电器模拟前端已被强制复位（我们的省电手段）
     *      - AUDCODEC.REFGEN_CFG  ⇒ codec 参考电流源，**开着就是 mA 级负载**
     *      - GPADC.ADC_CFG_REG1   ⇒ ADC 模拟前端
     *    三项任一"回到未关断态"都足以解释 mA 级电流。 */
    rt_kprintf("[STBYDBG] %s ldo3_off=%d CHG_CR3=0x%08x(FORCE_RST=%d) CHG_SR=0x%08x\n",
               tag, power_ldo3_is_off(),
               (unsigned)hwp_pmuc->CHG_CR3,
               !!(hwp_pmuc->CHG_CR3 & PMUC_CHG_CR3_FORCE_RST),
               (unsigned)hwp_pmuc->CHG_SR);
    rt_kprintf("[STBYDBG] %s AUDCODEC: CFG=0x%08x REFGEN=0x%08x  GPADC_CFG1=0x%08x  CHG_CR4=0x%08x\n",
               tag, (unsigned)hwp_audcodec->CFG, (unsigned)hwp_audcodec->REFGEN_CFG,
               (unsigned)hwp_gpadc->ADC_CFG_REG1, (unsigned)hwp_pmuc->CHG_CR4);

    /* ④ 时钟/降频现场 ★关键：DEEP 下 DLL1/DLL2 必须已关（HCPU 降到 HRC48），
     *    若 DLL 还在跑，光 DLL + 数字域全速就值 10~25mA —— 这正是 26.5mA 的量级！
     *    DLLxCR 的 EN 位 = DLL 使能；CFGR 含 HCLK 分频。 */
    rt_kprintf("[STBYDBG] %s RCC: DLL1CR=0x%08x(EN=%d) DLL2CR=0x%08x(EN=%d) CFGR=0x%08x\n",
               tag, (unsigned)hwp_hpsys_rcc->DLL1CR,
               !!(hwp_hpsys_rcc->DLL1CR & 0x1u),
               (unsigned)hwp_hpsys_rcc->DLL2CR,
               !!(hwp_hpsys_rcc->DLL2CR & 0x1u),
               (unsigned)hwp_hpsys_rcc->CFGR);

    /* ⑤ 外设时钟门控：待机期这些应全关。ENR1/ENR2 是 52x 仅有的两个使能寄存器，
     *    任一非零位都是明确的漏电源头（可直接定位到具体外设，比猜电流快得多）。 */
    rt_kprintf("[STBYDBG] %s RCC ENR1=0x%08x ENR2=0x%08x ESR1=0x%08x ESR2=0x%08x\n",
               tag, (unsigned)hwp_hpsys_rcc->ENR1, (unsigned)hwp_hpsys_rcc->ENR2,
               (unsigned)hwp_hpsys_rcc->ESR1, (unsigned)hwp_hpsys_rcc->ESR2);

    /* ★★★ 09-18(4) 新增：HP 时钟强制位 + LCPU 唤醒请求现场。这是 1.3mA/16.3mA 的
     * 头号嫌疑，此前从未打印过（`power_dump_standby_state` 只看 DLL/ENR/ESR，漏了 DBGR）。
     *
     * === 机制（全部读 SDK 源码确认）===
     * `pm_scenario_start(PM_SCENARIO_AUDIO)`（bf0_pm.c:3290）会置：
     *     hwp_hpsys_rcc->DBGR |= HPSYS_RCC_DBGR_FORCE_HP;      // DBGR bit4
     * 语义 = **强制开 HP 时钟**（DeepWFI 期间也不停），并顺手把 HCPU 降到 48MHz。
     * 只有 `pm_scenario_stop(PM_SCENARIO_AUDIO)`（bf0_pm.c:3318）才清它。
     * ⚠️ **它一旦残留，HCPU 即便真进了 [pm]S:3(DEEP) 也仍被强制供给时钟 ⇒ 恒定 mA 级**，
     *    且 SoC 其它可观测状态全部"干净"（正是我们几轮排查的困境）。
     * 本工程 main.c::usb_stop() 已知道这个位（注释"清 FORCE_HP(仅音频场景用)"），
     * 但**只处理了 USB 那条路**，待机/DEEP 路径从未核对。
     *
     * === LCPU 侧 ===
     * `HAL_HPAON_WakeCore(CORE_ID_LCPU)` 置 ISSR.HP2LP_REQ 并自带引用计数
     * （52x 启用 `AON_LCPU_ACTIVE_REQUEST_REF_COUNT_SUPPORT`，上限 20）。
     * 只有 `HAL_HPAON_CANCEL_LP_ACTIVE_REQUEST()` 把计数减到 0 时才清 HP2LP_REQ。
     * 语音路径（audio_server.c::audio_client_stop，`AUDIO_TYPE_BT_VOICE`）才负责：
     *     HAL_HPAON_CANCEL_LP_ACTIVE_REQUEST();       // 释放小核
     *     pm_scenario_stop(PM_SCENARIO_AUDIO);        // 清 FORCE_HP
     * ⇒ 若语音会话未走"正常 audio stop"（例如 SCO 由 ACL 掉线收尾），这两句都不执行
     *   ⇒ LCPU 永不休眠（LP_ACTIVE=1 + LPSYS_LDO/SWR RDY=1）**且** HP 时钟被强制
     *   ⇒ 与用户实测的三处异常完全吻合。
     *
     * 判读：
     *   - `DBGR.FORCE_HP=1` ⇒ **确认** 音频场景残留（先清它，再看电流）。
     *   - `LP_ACTIVE=1` 且 `HP2LP_REQ=1` ⇒ 是 HCPU 侧仍在请求（引用计数没归 0）。
     *   - `LP_ACTIVE=1` 但 `HP2LP_REQ=0` ⇒ 小核自因（BT 栈还有活干，如 sniff/SCO 在跑）。 */
    rt_kprintf("[STBYDBG] %s RCC DBGR=0x%08x FORCE_HP=%d  ISSR HP2LP_REQ=%d LP_ACTIVE=%d\n",
               tag, (unsigned)hwp_hpsys_rcc->DBGR,
               !!(hwp_hpsys_rcc->DBGR & HPSYS_RCC_DBGR_FORCE_HP),
               !!(issr & HPSYS_AON_ISSR_HP2LP_REQ),
               !!(issr & HPSYS_AON_ISSR_LP_ACTIVE));
#else
    (void)tag;   /* POWER_DEBUG_DIAG=0：整段不编译，仅保留空函数占位 */
#endif
}

/* ============================================================================
 * 09-16 ★★根因已定案（自检实证）: 本固件的 RTC **从未被真正初始化**
 *
 * 用户实测自检日志:
 *   `RTC selftest: calendar 946684800 -> 946684800 (delta=0)`   ⇒ 日历**停摆**
 *   `A:before-reinit: SEC 3counts -> TIMEOUT(未到点)`            ⇒ WUT 计数链根本没走
 *   `RTC selftest: reinit(RTC_INIT_REINIT) ok`
 *   `B:after-reinit: SEC 3counts -> WUTF SET (5542ms)`           ⇒ 重初始化后立刻会到点 ✓
 *   `RTC standby wake: battery=17% (37815mV, fresh=1, valid=1)`  ⇒ 待机周期唤醒/电量检查/自动关机链路**通了** ✓
 *
 * 机制: `drv_rtc.c:280` 只在"冷启动 且 `RTC_BACKUP_INITIALIZED==0`"才走 `RTC_INIT_NORMAL`
 * (真正写硬件), 其余一律 `RTC_INIT_SKIP` —— 而 SKIP 分支**完全不碰硬件**
 * (`bf0_hal_rtc.c:80-90`)。本机备份域标记早已是 1(或非冷启动) ⇒ 从第二次开机起 RTC 一直
 * 停在"未初始化"态: 分频没配、**计数本体没启动** ⇒ 日历恒 0、WUT 永不到点。
 * ⇒ 这解释了最开始的怪象: **寄存器每一项都"对"(WUTE/WUTIE/WUTR/PSCLR) 却永不触发** ——
 *   因为那些只是寄存器值, 计数本体压根没跑; 也解释了为什么"重写 PSCLR 到 34"没用。
 *   (另: `TR` 是影子寄存器, 裸读恒 0 当时误导了我, 真读数要 `drv_get_timestamp()`。)
 *
 * 修法(见 rtc_ensure_hw_init): 每次进待机前幂等补一次 `HAL_RTC_Init(RTC_INIT_REINIT)`,
 * 即 SDK 自己在 `drv_rtc.c::rtc_reconfig()` 用的"重初始化但不影响 Alarm/WUT"路径。
 *
 * 另: `rtc_selftest_wut()` 保留为"醒着量 WUT 到点时间"的测量原语, 供下面的频率校准用。
 * ============================================================================ */
static int rtc_selftest_wut(const char *tag, uint32_t counts, uint32_t timeout_ms)
{
    RTC_TypeDef *r = RTC_Handler.Instance;
    uint32_t t0;
    int      ms = -1;

    if (HAL_OK != HAL_RTC_SetWakeUpTimer(&RTC_Handler, counts, RTC_WAKEUP_SEC))
    {
        rt_kprintf("[POWER] RTC selftest(%s): arm FAILED\n", tag);
        return -1;
    }
    /* 关掉 WUT 中断：SetWakeUpTimer 会重新打开 WUTIE，而 drv_rtc 注册的 RTC_IRQHandler
     * 会在中断里清 WUTF —— 那我们就永远轮询不到标志。本自检只看标志位，不需要中断。 */
    r->CR &= ~RTC_CR_WUTIE;
    __HAL_RTC_WAKEUPTIMER_CLEAR_FLAG(&RTC_Handler, RTC_ISR_WUTF);

    t0 = HAL_GetTick();
    while ((HAL_GetTick() - t0) < timeout_ms)
    {
        if (r->ISR & RTC_ISR_WUTF)
        {
            ms = (int)(HAL_GetTick() - t0);
            break;
        }
        rt_thread_mdelay(10);
    }
    rt_kprintf("[POWER] RTC selftest(%s): SEC %ucounts -> %s (%dms, 1Hz 应约 %ums)\n",
               tag, (unsigned)counts, (ms >= 0) ? "WUTF SET" : "TIMEOUT(未到点)",
               ms, (unsigned)(counts * 1000u));

    __HAL_RTC_WAKEUPTIMER_CLEAR_FLAG(&RTC_Handler, RTC_ISR_WUTF);
    HAL_RTC_DeactivateWakeUpTimer(&RTC_Handler);
    __HAL_RTC_WAKEUPTIMER_CLEAR_FLAG(&RTC_Handler, RTC_ISR_WUTF);
    return ms;
}

/* 09-16 ★真根因修复: 补齐 RTC 硬件初始化（幂等，每次进待机前调用）。
 * 依据与机制见上方大段注释：drv_rtc 在非冷启动时走 RTC_INIT_SKIP(完全不碰硬件)
 * ⇒ 计数链没启动 ⇒ 日历停摆 + WUT 永不到点。
 * ⚠️ HAL_RTC_Init 会把 hrtc->callback 清成 NULL ⇒ 必须补回 drv_rtc_callback，
 *    否则 RT-Thread RTC 设备的中断回调/时间同步会失效。 */
static void rtc_ensure_hw_init(void)
{
    extern void drv_rtc_callback(int reason);

    RTC_Handler.State = HAL_RTC_STATE_READY;    /* 防 State 残留导致 Init 里 MspInit 误判 */
    if (HAL_OK != HAL_RTC_Init(&RTC_Handler, RTC_INIT_REINIT))
    {
        rt_kprintf("[POWER] RTC reinit FAILED (wake timer may not work)\n");
        return;
    }
    HAL_RTC_RegCallback(&RTC_Handler, drv_rtc_callback);
}

/* 09-16/09-17: SEC 计数速率实测/校准。
 * 自检实测 3 counts 用 5538ms、6 counts 用 8864ms ⇒ 两点斜率 = (8864-5538)/3 = 1108.7ms/count，
 * 截距 ≈2212ms（分频链启动/同步的固定延迟，与周期无关）。
 * 用**两点斜率**而非单点，正是为了剔除这个固定偏移：slope = (t(6)-t(3))/3。
 *
 * ★09-17 定案：1108ms/count 不是"RTC 时钟神秘变慢 1.85×"，而是**真实 RC10K 频率
 * 只有 8002Hz，而代码以为 8866Hz**（AVE 备份值 1082761 对应的就是 8866Hz）。
 * 两条独立证据互相印证：
 *   ① 本自检实测 SEC 周期 1108.7ms（设计值 1000ms）→ 偏慢 10.87%；
 *   ② step 2.9 的 GTIMER 实测（f_real=8004Hz）→ 偏慢 10.6%（隐含 AVE=1199400）。
 *   48000000*200/8002 = 1199700 ≈ 1199400 ⇒ 两法一致。
 * 所以真正要修的是 **AVE 备份值（→ 分频链 PSCLR）**，而不是"RTC 时钟"本身。
 * 一旦 PSCLR 按真实频率重算，SEC 就回到 1.000s/count，本变量自然回到 1000。
 *
 * 每 BOOT 只跑一次（首次进待机时，约 20s）；测量失败则退回理论值 1000ms/count。 */
static uint32_t g_rtc_ms_per_count = 1000;

static void rtc_calibrate_once(void)
{
    static uint8_t done = 0;

    if (done)
        return;
    done = 1;

    /* 持 IDLE 锁：校准期间绝不能掉进 DEEP（否则 tick 冻结、测量失去意义） */
    rt_pm_request(PM_SLEEP_MODE_IDLE);

    /* 1) 日历时间是否在走 —— 验证 rtc_ensure_hw_init 是否真的把 RTC 启起来了 */
    {
        uint32_t t1 = (uint32_t)drv_get_timestamp();
        rt_thread_mdelay(2000);
        uint32_t t2 = (uint32_t)drv_get_timestamp();
        rt_kprintf("[POWER] RTC cal: calendar %u -> %u (delta=%d, 正常≈2)\n",
                   (unsigned)t1, (unsigned)t2, (int)((int32_t)t2 - (int32_t)t1));
    }

    /* 2) 两点测速率（N=3 与 N=6），取斜率 —— 斜率即【当前 PSCLR 下】SEC 的真实周期 */
    {
        int t3 = rtc_selftest_wut("N=3", 3, 20000);
        int t6 = rtc_selftest_wut("N=6", 6, 30000);

        if (t3 > 0 && t6 > t3)
        {
            g_rtc_ms_per_count = (uint32_t)(t6 - t3) / 3u;
            if (g_rtc_ms_per_count == 0)
                g_rtc_ms_per_count = 1;
        }
        rt_kprintf("[POWER] RTC cal: ms/count=%u (设计 1000ms, 偏差 %s%d.%02u%%)\n",
                   (unsigned)g_rtc_ms_per_count,
                   (g_rtc_ms_per_count >= 1000u) ? "+" : "-",
                   (int)((g_rtc_ms_per_count >= 1000u ? g_rtc_ms_per_count - 1000u
                                                      : 1000u - g_rtc_ms_per_count)) / 10,
                   (unsigned)((g_rtc_ms_per_count >= 1000u ? g_rtc_ms_per_count - 1000u
                                                           : 1000u - g_rtc_ms_per_count) % 10u));
    }

    /* 2b) ★09-17 主修: 让 SEC 回到 1.000s/count。
     * 现象: 实测 SEC=1108ms/count（偏慢 10.8%）⇒ 设计分频链是按"错误的高频"算的:
     *   RTC 分频链的输入频率 = 48000000*LXT_LP_CYCLE / AVE（AVE 越低 ⇒ 认为时钟越快）。
     *   本机 AVE=1082761 ⇒ 认为 8866Hz；真实只有 8002Hz ⇒ SEC 被拉长到 1108ms。
     * 修法: 用实测斜率反算真实输入频率，反推正确的 AVE，写回备份 + 重算 PSCLR。
     *   f_real = f_assumed * (1000 / ms_per_count)
     *   AVE_correct = 48000000 * LXT_LP_CYCLE / f_real
     *              = AVE_assumed * (ms_per_count / 1000)
     * ⚠️ 必须用 HAL_RTC_Init(RTC_INIT_REINIT) 让新 PSCLR 落到硬件（rtc_ensure_hw_init
     *    已做这件事；这里只改 AVE 备份值 + 更新 hdl->Init.DivA*，再触发一次 reinit）。
     * ⚠️ 偏差 <2% 时不折腾：PSCLR 重写会重置分频计数器，把已武装的 WUT 计时起点打乱。 */
    if (g_rtc_ms_per_count > 0)
    {
        uint32_t ave_assumed = HAL_Get_backup(RTC_BACKUP_LPCYCLE_AVE);
        if (ave_assumed == 0)
            ave_assumed = 1082788u;     /* drv_rtc DEFAULT_CYCLE(1200000) 的常见落地值兜底 */
        uint32_t ave_correct =
            (uint32_t)((uint64_t)ave_assumed * g_rtc_ms_per_count / 1000u);
        /* 偏差千分比（|ms-1000| / 1000 * 1000）。
         * ⚠️ 09-18 修正: 上一版误写成 `diff/1000*1000` —— 整数除法先截断(105/1000=0)
         * ⇒ dev1000 恒为 0 ⇒ **永远进不了修正分支**(实测日志: "偏差 +10.05%" 却
         * "divider deviation <2%, PSCLR kept")。正确顺序是先乘后除。 */
        uint32_t diff = (g_rtc_ms_per_count > 1000u)
                        ? (g_rtc_ms_per_count - 1000u) : (1000u - g_rtc_ms_per_count);
        uint32_t dev1000 = (uint32_t)((uint64_t)diff * 1000u / 1000u);

        if (dev1000 >= 20u && ave_correct > 0)      /* ≥2% 才修 */
        {
            extern void rtc_rc10_calculate_div(RTC_HandleTypeDef *hdl, uint32_t value);
            HAL_Set_backup(RTC_BACKUP_LPCYCLE_AVE, ave_correct);
            HAL_RC_CAL_update_ave_cycle(ave_correct);
            rtc_rc10_calculate_div(&RTC_Handler, ave_correct);
            RTC_Handler.State = HAL_RTC_STATE_READY;
            if (HAL_OK == HAL_RTC_Init(&RTC_Handler, RTC_INIT_REINIT))
            {
                extern void drv_rtc_callback(int reason);
                HAL_RTC_RegCallback(&RTC_Handler, drv_rtc_callback);
                rt_kprintf("[POWER] RTC cal: DIVIDER FIXED AVE %u -> %u, PSCLR=0x%08x "
                           "(DIVA=%u FRAC=%u DIVB=%u)\n",
                           (unsigned)ave_assumed, (unsigned)ave_correct,
                           (unsigned)RTC_Handler.Instance->PSCLR,
                           (unsigned)((RTC_Handler.Instance->PSCLR >> 24) & 0xFF),
                           (unsigned)((RTC_Handler.Instance->PSCLR >> 10) & 0x3FFF),
                           (unsigned)(RTC_Handler.Instance->PSCLR & 0x3FF));
                /* 分频链已按真实频率重配 ⇒ SEC 回到 1.000s/count，把换算基准复位，
                 * 避免用"旧 PSCLR 下测出的 1108ms"去算新 PSCLR 的 counts（双重补偿）。 */
                g_rtc_ms_per_count = 1000;
            }
            else
            {
                rt_kprintf("[POWER] RTC cal: divider reinit FAILED, keep old PSCLR\n");
                HAL_Set_backup(RTC_BACKUP_LPCYCLE_AVE, ave_assumed);  /* 回滚备份值 */
            }
        }
        else
        {
            rt_kprintf("[POWER] RTC cal: divider deviation <2%%, PSCLR kept\n");
        }
    }

    /* 3) 时钟源现场（SEL_LPCLK: 0=RC10K, 1=RC32/XT32; RTC_CR.LPCKSEL 为 RTC 侧选择） */
    rt_kprintf("[POWER] RTC cal: PMUC CR=0x%08x(SEL_LPCLK=%u) LRC10_CR=0x%08x LRC32_CR=0x%08x RTC_CR=0x%08x(LPCKSEL=%u)\n",
               (unsigned)hwp_pmuc->CR, (unsigned)(hwp_pmuc->CR & PMUC_CR_SEL_LPCLK),
               (unsigned)hwp_pmuc->LRC10_CR, (unsigned)hwp_pmuc->LRC32_CR,
               (unsigned)RTC_Handler.Instance->CR,
               (unsigned)(RTC_Handler.Instance->CR & RTC_CR_LPCKSEL));

    rt_pm_release(PM_SLEEP_MODE_IDLE);
}

/* 武装待机电量检查闹钟：一次性 + 使能 RTC 唤醒源（幂等，重复调用安全）。
 *
 * 09-16 根因①: **RTC 从未被硬件初始化**（drv_rtc 非冷启动时走 RTC_INIT_SKIP，完全不碰硬件）
 *   ⇒ 计数链没启动 ⇒ 日历停摆、WUT 永不到点（"寄存器全对却不响"的谜底）。
 * 09-17 根因②: **分频链按错误的 AVE 计算** ⇒ SEC 实际 1108ms/count（设计 1000ms），
 *   于是"3600s"实际是 3600*1.108 = 3989s；叠加"用 1108 换算 counts"，
 *   两者互相抵消一部分，但 PSCLR 与 counts 基准不一致会让周期不可预测。
 *   ⇒ 现在由 rtc_calibrate_once() 的 step 2b 直接把 PSCLR 按真实频率修好，
 *     之后 g_rtc_ms_per_count 复位为 1000，counts = 秒数（1:1 干净换算）。
 *
 * 本函数顺序：① 幂等补一次 RTC 初始化 → ② 首次进待机时校准/修正分频链
 *   → ③ 用（修正后的）SEC 节拍把"秒"换算成 counts 并武装。
 * - SEC("From Prescalar B") 名义 1Hz；修正后即真实 1.000s/count。 */
static void standby_rtc_arm_timer(void)
{
    /* ★09-18: 全程持 IDLE 锁 —— 本函数含两次自检(N=3/N=6)+一次 arm-verify，累计可达 ~15s。
     * 若期间掉进 DEEP，RTC 计数链会被 sifli_suspend/resume 打乱 ⇒ 自检假失败、WUT 被清。
     * (rtc_calibrate_once 内部也持锁；RT-Thread PM 的 request/release 是引用计数，嵌套安全。) */
    rt_pm_request(PM_SLEEP_MODE_IDLE);

    /* ① ★根因修复: 确保 RTC 硬件已初始化（幂等；HAL_RTC_Init(REINIT) 不碰 Alarm/WUT） */
    rtc_ensure_hw_init();

    /* ② 一次性校准（每 BOOT 首次进待机时，约 20s；之后各次直接复用测量结果） */
    rtc_calibrate_once();

    /* ③ 按实测速率换算 counts 并武装 */
    {
        uint32_t cnt = (uint32_t)((uint64_t)STANDBY_BAT_CHECK_PERIOD_S * 1000u
                                  / (g_rtc_ms_per_count ? g_rtc_ms_per_count : 1000u));

        if (cnt == 0)
            cnt = 1;
        if (cnt > 0x3FFFFu)
            cnt = 0x3FFFFu;     /* RTC_WUTR_WUT 是 18 位 (0x3FFFF)，非 16 位 */

        if (HAL_OK != HAL_RTC_SetWakeUpTimer(&RTC_Handler, cnt, RTC_WAKEUP_SEC))
        {
            rt_kprintf("[POWER] RTC wake timer arm FAILED (battery check disabled)\n");
            rt_pm_release(PM_SLEEP_MODE_IDLE);   /* ★勿漏：早退也要放锁 */
            return;
        }
        /* PMU 侧 RTC 唤醒使能（WER bit0）。幂等；显式写一次以排除"被别处关掉"。 */
        HAL_PMU_EnableRtcWakeup();
        HAL_HPAON_EnableWakeupSrc(HPAON_WAKEUP_SRC_RTC, AON_PIN_MODE_HIGH);
        rt_kprintf("[POWER] RTC standby battery-check armed (%ds -> %u counts, %ums/count, "
                   "real=%.1fs)\n",
                   STANDBY_BAT_CHECK_PERIOD_S, (unsigned)cnt, (unsigned)g_rtc_ms_per_count,
                   (double)((uint64_t)cnt * g_rtc_ms_per_count) / 1000.0);
        rtc_wut_dump("armed");

        /* ★09-17 新增自检（每 BOOT 首次武装后跑一次，约 3s）：
         * 用极短 counts(3) 实测"当前 PSCLR + 当前换算基准"下 WUT 是否真能到点。
         * 这是唯一能在**进长睡之前**发现"寄存器对但计时链不跑"的手段 ——
         * 上一版就是 armed 打印漂亮、实际 10 小时一次没醒，事后无从判断。
         * 若这里 TIMEOUT，直接把周期降级为 1 个 count 也仍有风险，故只告警不改行为。 */
        {
            static uint8_t s_arm_verify_done = 0;
            if (!s_arm_verify_done)
            {
                s_arm_verify_done = 1;
                int probe = rtc_selftest_wut("arm-verify", 3, 15000);
                if (probe < 0)
                {
                    rt_kprintf("[POWER] RTC ARM VERIFY FAILED: WUT did not fire in 15s @3counts "
                               "-> standby battery check will NOT wake (check WER/PSCLR/CR)\n");
                }
                else
                {
                    rt_kprintf("[POWER] RTC ARM VERIFY OK: 3counts fired in %dms (%.0fms/count)\n",
                               probe, (double)probe / 3.0);
                }
                /* 自检会把 WUT 停掉 → 必须重新武装正式周期 */
                if (HAL_OK != HAL_RTC_SetWakeUpTimer(&RTC_Handler, cnt, RTC_WAKEUP_SEC))
                    rt_kprintf("[POWER] RTC re-arm after verify FAILED\n");
                else
                {
                    HAL_PMU_EnableRtcWakeup();
                    HAL_HPAON_EnableWakeupSrc(HPAON_WAKEUP_SRC_RTC, AON_PIN_MODE_HIGH);
                    rt_kprintf("[POWER] RTC re-armed after verify (%u counts)\n", (unsigned)cnt);
                }
            }
        }
    }

    rt_pm_release(PM_SLEEP_MODE_IDLE);   /* ★配平 772 行的 request */
}

/* RTC 唤醒后的电量检查决策：≤5% 且未插充电器 → Hibernate 关机；否则继续睡。
 * 在 sleep_thread 待机阻塞循环内调用（g_standby_active 仍为 1）。 */
static void standby_rtc_battery_check(void)
{
    uint8_t vbus = 0;

    g_in_deep_sleep = 0;
    g_standby_rtc_wake = 0;

    /* ★09-18 诊断：RTC 轻量唤醒刚回来、**动任何东西之前**的现场。
     * 这是整个问题的核心观测点 —— 若此处 ldo3_off 已变 0（或 CHG 未 FORCE_RST），
     * 就证明 DEEP 唤醒路径（SDK 的 BSP_Power_Up）把域重开了，而轻量路径从不关回去
     * ⇒ 第二次 DEEP 期间 LDO3/analog 一直上电 ⇒ 26.5mA。
     * 注意：轻量路径不经过 power_resume_from_deep()，故 just-woke 那次不会打印。 */
    power_dump_standby_state("rtc-woke-light");

    /* DEEP 期 NVIC RTC_IRQn 使能随 HPSYS 掉电丢失 → drv_rtc_callback 没机会跑，
     * WUTF/WUTE 可能残留。显式去激活 + 清 WUTF，防残留事件把 WSR_RTC 重新挂上
     * （WSR&WER≠0 → sifli_suspend EBUSY → DEEP 永久降级 IDLE 的老坑）。 */
    HAL_RTC_DeactivateWakeUpTimer(&RTC_Handler);
    __HAL_RTC_WAKEUPTIMER_CLEAR_FLAG(&RTC_Handler, RTC_ISR_WUTF);
    NVIC->ICPR[RTC_IRQn >> 5] = (1UL << (RTC_IRQn & 0x1FUL));

    /* 持 IDLE 锁：ADC 采样有 300ms mdelay，待机期 LPTIM1 已关（tickless 无节拍），
     * 不持锁的话 mdelay 计不了时且框架会反复短进 DEEP。 */
    rt_pm_request(PM_SLEEP_MODE_IDLE);

    /* 09-16: 记下读数序号 —— 只有当本次 refresh 真的刷新了序号, 才认为读到的是当前值。
     * 防 ADC 使能失败/读数失败时沿用陈旧的高电量值 → 整晚不关机。 */
    uint32_t seq_before = app_get_battery_read_seq();

    power_analog_domain_on();       /* 恢复 GPADC 模拟前端才能采样电池电压 */
    /* ★ 09-18 验证结论：BLE 与本次 26.5mA **无关**（置 0 屏蔽 refresh 后电流仍 26.5mA）。
     * 真凶已定案 = LDO3 被 SDK 的 BSP_Power_Up() 重开（见下方"根因修复"段）。
     * 故此处恢复常态：1 = 每次轻量唤醒都刷新电量。 */
#define STANDBY_BAT_REFRESH_EN  1
#if STANDBY_BAT_REFRESH_EN
    ble_app_battery_refresh();      /* ADC 读 + 电量计算（内含 300ms 稳定延时） */
#else
    rt_kprintf("[POWER] STANDBY-BAT-REFRESH DISABLED (26.5mA experiment)\n");
#endif
    (void)rt_charge_get_detect_status(&vbus);   /* 直读 CHG_SR VBUS 位 */
    power_analog_domain_off();      /* 立刻重新断模拟域，恢复待机低电流 */

    uint8_t  pct   = app_get_battery_percent();
    uint32_t mv    = app_get_battery_mv();
    int      fresh = (app_get_battery_read_seq() != seq_before);   /* 本次读数成功刷新? */
    rt_kprintf("[POWER] RTC standby wake: battery=%d%% (%umV, fresh=%d, valid=%d) vbus=%d\n",
               pct, (unsigned)mv, fresh, app_battery_valid(), vbus);

    /* ★★★ 09-18 探针3：定位"**待机 60s 窗口内**电流在 0.1↔0.2mA 之间横跳"
     *（用户实测：不是唤醒占空比造成的平均值，而是深睡期间就在跳）。
     *
     * 深睡期电流抬高并在两个值间跳，最可能是 **DEEP 被周期性打断**（每次短醒把平均值抬一点）。
     * 这里统计**本窗口内 DEEP 被唤醒的次数**并打印**最后一次 DEEP 的唤醒源**：
     *   · `g_deep_wake_cnt` 由 `BSP_PowerUpCustom()` 在**每次** DEEP 唤醒时自增；
     *   · `g_last_deep_wsr` 记着那次的 WSR（bit0=RTC / bit1=GPIO1 / bit2=LPTIM1 /
     *     bit6=LP2HP_REQ / bit7=LP2HP_IRQ / bit8+=PIN0-20）；`g_deep_spur_cnt` 是判为
     *     伪唤醒（纯 LPTIM1/RTC）的累计数。
     *
     * 判读：
     *   · 期望 **每 60s 窗口恰好 +1** 且 `last_wsr` 含 bit0(RTC) ⇒ 深睡是干净的，
     *     那 0.1↔0.2 就是"稳态电流真的比原来高 ~0.1mA"，要换方向查（谁在深睡期常驻耗电）；
     *   · 若 **+N（N>1）** ⇒ DEEP 被周期性打断，`last_wsr` 会直接指出元凶
     *     （例如含 bit2=LPTIM1 ⇒ 待机看门狗 10s 超时在反复醒；含 PIN/bit1 ⇒ 某按键脚被触发）。
     * 定位完成后本段可删。 */
#if POWER_DEBUG_DIAG
    {
        static uint32_t s_prev_deep_wakes = 0;
        uint32_t now_wakes = g_deep_wake_cnt;
        rt_kprintf("[POWER] standby stats: deep_wakes=%u (+%u this window) last_wsr=0x%08x "
                   "spur=%u lcpu_sleep=%d\n",
                   (unsigned)now_wakes, (unsigned)(now_wakes - s_prev_deep_wakes),
                   (unsigned)g_last_deep_wsr, (unsigned)g_deep_spur_cnt,
                   power_lcpu_is_sleeping());
        s_prev_deep_wakes = now_wakes;
    }
#endif

    rt_pm_release(PM_SLEEP_MODE_IDLE);

    /* 充电中(硬件 VBUS 直读)不关机; 且要求本次读数新鲜(排除陈旧值)。
     * ★09-18 回退说明（按用户决定）：曾短暂加过"static 计数 + 连续
     * BAT_SHUTDOWN_CONFIRM 个唤醒周期都低才关"，但实测（用户反馈"第一次低电唤醒没关、
     * 按键后才关"）显示：
     *   ① 真低电时要**多等一整个周期**（产量 1h ⇒ 2h）—— 用户明确不要这个延迟；
     *   ② 该保护本身**冗余** —— adc 采样已很干净（见下），确认机制收益极小。
     * ⇒ **回退为单次判定**。活跃态那边保留 confirm：那里 500ms 一次，代价可忽略。
     *
     * 为什么单次读数已足够可靠：
     *   · `rt_adc_read`（drv_adc.c）内部：8× 过采样 → 排序 → 去掉最大最小 → 求均值；
     *   · `hid_report_battery()` 在 `rt_adc_enable` 后 `mdelay(300)` 才读
     *     ⇒ 模拟域建立时间已留裕量。
     * 另注：读数在**带载时刻**取得（CPU 48MHz + analog 域开），测得值天然略**低**于静止
     * 电压 ⇒ 方向上是"偏早关机"，安全侧。 */
    /* ★★★ 09-18 临时测试开关（与活跃态那个对称，便于独立控制两条路径）。
     * 本次验证目标是【待机态】路径，故此开关保持 1（启用）。
     * 用法：置 0 = 屏蔽待机态关机；置 1 = 正常。⚠️ 量产必须为 1。 */
#define STANDBY_SHUTDOWN_EN  1
#if STANDBY_SHUTDOWN_EN
    if (fresh && app_battery_valid() && !vbus
        && (pct <= BAT_SHUTDOWN_PCT || (mv > 0 && mv <= BAT_SHUTDOWN_MV)))
    {
        rt_kprintf("[POWER] battery critically low in standby (%d%%, %u.%umV), shutting down\n",
                   pct, (unsigned)(mv / 10u), (unsigned)(mv % 10u));
        /* ★09-18 补(H4)：待机路径原先没有红灯指示，却仍要等 pm_shutdown 里那 3s
         * 延时 ⇒ 用户看不到"正在关机"，像卡死。此处与活跃态一致点亮红 LED4
         * （LED4 挂 VDD33 域，pm_shutdown 末尾才断，故此刻能亮）。 */
        led4_set(1);
        pm_shutdown();              /* 不返回 */
    }
#else
    (void)fresh; (void)vbus;
    rt_kprintf("[POWER] STANDBY shutdown DISABLED (test switch)\n");
#endif

    /* 电量尚可：重新武装闹钟，继续睡（外设/LDO3/BLE 全程未动）。
     * ⚠️ 回睡前必须重新"清场"：RTC 唤醒会搅动 AON/小核, WSR 残留或 IPC 未清会让
     * sifli_suspend EBUSY → DEEP 静默降级成 IDLE → 之后就再也回不到 DEEP(整晚高电流)。 */
    /* ★★★ 09-18 根因修复：轻量路径重进 DEEP 前，把 LDO3 强制关回去。
     *
     * === 根因链（寄存器实证，非推断）===
     *   PERI_LDO 位定义(pmuc.h): bit16=EN_VDD33_LDO3  bit21=VDD33_LDO3_PD
     *     pre-deep-1st    0x002c2d19 → LDO3 EN=0 PD=1  【关】 → 这次 DEEP 0.1mA ✓
     *     rtc-woke-light  0x000d2d19 → LDO3 EN=1 PD=0  【开】 ★
     *     pre-deep-2nd    0x000d2d19 → LDO3 EN=1 PD=0  【开】 → 这次 DEEP 26.5mA ✗
     *   ⇒ SDK 的 `BSP_Power_Up(false)`（DEEP 唤醒路径，bf0_pm.c:1137）无条件执行
     *     `if (BSP_ImuPowerWanted()) HAL_PMU_ConfigPeriLdo(LDO3,true,true);`
     *     而 `main.c:1665 BSP_ImuPowerWanted()` = `return g_imu_power_wanted;`
     *     —— **没有 power_ldo3_is_off() 判断**（原注释声称有，是错的），
     *     且 `g_imu_power_wanted` 初值=1（main.c:1662）⇒ 唤醒即重开 LDO3。
     *
     * === 为什么第一次不出问题、第二次才炸 ===
     *   第一次路径 `power_enter_standby()` 会调 `power_ldo3_domain_off()` 把 LDO3
     *   关掉并置 `g_ldo3_off=1`，所以进 DEEP 时 LDO3=关 → 0.1mA。
     *   但唤醒后 SDK 又把 LDO3 硬开，而 `g_ldo3_off` 仍停在 1（**标志与硬件不一致**），
     *   轻量路径 `standby_rtc_battery_check()` 又从不碰 LDO3 ⇒ 第二次进 DEEP 时
     *   LDO3 一直开着 → IMU/编码器/I2C 上拉/RGB 全部带电 → 26.5mA。
     *
     * === 修法（以**寄存器位**为准，不用软件标志做判据）===
     *   ① 硬件确实带电 → 清 `g_ldo3_off` 破除幂等保护，再调 power_ldo3_domain_off()
     *      真正关断（含去上拉 + 驱动低，防反灌）。
     *   ② 硬件本来就是关的（标志=1，第一次进 DEEP 前的常见态）→ 什么都不做。
     *   ⚠️ 绝不能用 power_ldo3_domain_on() 来"复位标志"——那会真的把 LDO3 上电，
     *      形成无谓的上电/下电循环，且 on() 内有 20ms mdelay 与引脚重配，待机路径
     *      上既危险又可能反灌。故这里直接操作 g_ldo3_off（本文件 static，可见）。 */
    {
        int ldo3_powered = !!(hwp_pmuc->PERI_LDO & PMUC_PERI_LDO_EN_VDD33_LDO3)
                        && !(hwp_pmuc->PERI_LDO & PMUC_PERI_LDO_VDD33_LDO3_PD);
        if (ldo3_powered) {
            rt_kprintf("[POWER] LDO3 still powered before 2nd DEEP; forcing OFF\n");
            g_ldo3_off = 0;             /* 破除幂等保护（标志本已失真） */
            power_ldo3_domain_off();    /* 真正关断 + 去上拉 + 驱动低 */
        }
    }

    /* ★★★ 09-18(4) 新增修复（与 power_enter_standby 里那句对称）：
     * RTC 轻量唤醒会经 SDK `BSP_Power_Up(false)` 恢复一堆东西，其中**可能**把
     * 音频场景的 `DBGR.FORCE_HP` 或 WFI 分频重新弄回去；重进 DEEP 之前必须再核一遍。
     * 详见 power_enter_standby() 里那段完整的机制说明（`pm_scenario_start(AUDIO)`
     * 置 FORCE_HP；只有 audio_client_stop 的 BT_VOICE 分支才清，语音异常收尾会残留）。
     * ⚠️ 位置必须在 power_dump_standby_state("pre-rearm-2nd") 之前，才能保留"清之前"现场。 */
    if (hwp_hpsys_rcc->DBGR & HPSYS_RCC_DBGR_FORCE_HP)
    {
        rt_kprintf("[POWER] 2nd-DEEP: stale AUDIO scenario DBGR.FORCE_HP=1 -> clearing\n");
        hwp_hpsys_rcc->DBGR &= ~HPSYS_RCC_DBGR_FORCE_HP;
    }
    HAL_RCC_HCPU_SetDeepWFIDiv(60, 0, 1);

    /* ★09-18 诊断（用户实测：本次轻量路径重进 DEEP 期间整机电流 26.5mA）：
     * 抓"重进 DEEP 之前"的现场，与唤醒归来后的 post-rtc-check 现场对比。 */
    power_dump_standby_state("pre-rearm-2nd");
    standby_rtc_arm_timer();
    standby_prepare_deep_gate("post-rtc-check", 3000);
    /* ★09-18 诊断：quiesce 之后再抓一次（此时才是真正即将进 DEEP 的瞬间状态） */
    power_dump_standby_state("pre-deep-2nd");
    g_in_deep_sleep = 1;
}

/* ============================================================================
 * 09-16: 按名字在内核定时器对象里查一个 rt_timer（SDK 未提供 rt_timer_find）。
 * 只用于处理 RW BLE 主机栈的 "BLEHost" 软定时器 —— 见下方待机入口/唤醒路径说明。
 * 原理: rt_object_get_information(RT_Object_Class_Timer) 取出该类对象链表,
 *       按 name 命中后把 rt_object 指针转回 rt_timer(其第一个成员就是 parent)。
 * ============================================================================ */
static rt_timer_t power_find_timer_by_name(const char *name)
{
    struct rt_object_information *info = rt_object_get_information(RT_Object_Class_Timer);
    struct rt_list_node *node;

    if (info == RT_NULL)
        return RT_NULL;
    for (node = info->object_list.next; node != &info->object_list; node = node->next)
    {
        struct rt_object *obj = rt_list_entry(node, struct rt_object, list);
        if (rt_strcmp(obj->name, name) == 0)
            return (rt_timer_t)obj;
    }
    return RT_NULL;
}

/* ============================================================================
 * 待机主体(08-27 提取为函数): 未连接 60s 超时 与 连接态空闲超时 共用。
 * 关扫描/经典栈/BLE 广播 -> 断 USB CDC -> 释放 PM 锁 -> 关周期源 -> 外设域
 * 断电 -> 按键唤醒 -> DEEP 阻塞 -> 唤醒后重建外设。
 * 调用时机: sleep_thread_entry 判定无 USB、无通话(SCO) 且满足休眠条件后。
 * ============================================================================ */
static void power_enter_standby(void)
{
    /* ★★★ 09-18：入口立刻置"准备期"标志 + 建立干净基线。
     * 必须在**任何耗时步骤之前**（BT 关闭 / ~20s RTC 校准都在后面）：它让
     * `power_standby_wakeup()` 在准备窗口内也能记下按键，从而可被中止检查捕获。
     *
     * ⚠️ 同时必须清零 `g_wakeup_requested`：阻塞循环是**因为它=1 才退出**的，
     *    正常唤醒路径从未清它 ⇒ 若不在入口清，下一次待机的"准备期检查"会看到
     *    上一轮的陈旧值 ⇒ **误中止**（永远进不了 DEEP）。
     *    入口清零后，"准备期检查为真"才精确等价于"本次准备期内新到达了按键"。
     *    入口清零是安全的：活跃态下 `power_standby_wakeup()` 被守卫挡住，不会置位，
     *    故入口处不可能有"合法的待处理唤醒"。
     * ⚠️ 本标志的所有置/清都必须成对（正常出口 + 中止出口）。 */
    g_standby_prepare = 1;
    g_wakeup_requested = 0;

    rt_kprintf("[POWER] stopping BT for sleep\n");
    led_all_off();

    /* Stop classic BT stack to release PM locks */
    /* 先强制关断经典蓝牙扫描（page/inquiry scan 空转耗电 ~0.5mA）*/
    extern void bt_interface_set_scan_mode(uint8_t scan_mode, uint8_t enable);
    bt_interface_set_scan_mode(0, 0);
    rt_thread_mdelay(100);

    extern void bt_interface_close_bt(void);
    bt_interface_close_bt();
    rt_thread_mdelay(500);

    /* 09-13: 主动断开 BLE 链路 —— 否则小核(LCPU)会在整个"待机"期间持续维持 BLE
     * 链路(射频活动 + 连接间隔唤醒), 待机电流只有 ~0.3mA 而非 0.1mA(实测)。
     * 断开事件负责 air_mouse_stop/enc_stop/清键盘补发/清 g_bt_connected 等收尾,
     * 但被 app_ble_teardown_for_standby() 抑制了"自动重广播"(进待机不能广播)。
     * 等到断开事件落地(g_bt_connected 归 0)再继续, 避免带着半断链路进 DEEP。 */
    {
        extern void app_ble_teardown_for_standby(void);
        extern void bt_multi_suppress_indication(uint8_t on);
        /* 先抑制槽位指示闪烁: 断开事件会调 bt_multi_on_disconnected→start_indication,
         * 起一个 PERIODIC 闪烁定时器 → PM 到不了 tick==MAX → DEEP 进不去 → 耗光电池。
         * 必须在 app_ble_teardown_for_standby() 之前置位(断开事件是异步的)。 */
        bt_multi_suppress_indication(1);
        app_ble_teardown_for_standby();
        int w;
        for (w = 0; w < 20 && g_bt_connected; w++)
            rt_thread_mdelay(50);   /* 最多等 1s */
    }

    /* close_bt 直接断经典蓝牙 ACL，SCO_DISCONNECTED 事件可能不单独派发 →
     * 强制清通话标志，防唤醒后 g_sco_active 卡 1(鼠标永禁/结束语音无效)。
     * vf_retry 定时器由 ble_app 在 !g_sco_active 时自清，无需此处处理。 */
    g_sco_active = 0;

    /* Stop BLE advertising */
    app_ble_stop_advertising();
    rt_thread_mdelay(500);

    rt_kprintf("[POWER] releasing PM locks\n");

    /* 待机即断 CDC：无论是否插 USB，待机期间强制 usb_stop()，
     * 释放 USB 持有的 PM IDLE 锁（也便于唤醒后依据 usb_on 重新枚举）。 */
    extern int usb_on;
    extern void usb_stop(void);
    usb_stop();
    rt_kprintf("[POWER] usb stopped for standby (usb_on=%d)\n", usb_on);

    /* ⚠️ 先注册 DEEP policy 再释放 IDLE 锁！
     * 若先释放 IDLE 锁再注册 policy，存在时间窗口：IDLE 锁已释放但
     * policy 仍是默认的 {2,LIGHT} → idle 线程选 LIGHT → sifli_light_handler
     * → WFI 后 Flash DPD 恢复代码跑不到 → 死锁（[pm]S:2 后无日志）。 */
    rt_pm_policy_register(1, g_standby_deep_policy);

    /* 待机期关闭 LPTIM1 唤醒源。⚠️ 实测（08-04）：保持 LPTIM1 使能会让
     * DEEP 频繁被 LPTIM1 唤醒/tick 补算打断，甚至 WSR bit2 残留触发
     * sifli_suspend EBUSY → DEEP 降级 IDLE → 待机 1.1mA（应 0.3mA）。
     * 本键盘待机只靠按键(GPIO1)唤醒，不需要 LPTIM1 周期唤醒。
     * 注意：关 LPTIM1 后 DEEP 是 tickless，OS tick 冻结，rt_sem_take 超时
     * 不触发 → 看门狗失效（调试期需靠按键唤醒/BSP_PowerUpCustom 日志）。 */
    HAL_HPAON_DisableWakeupSrc(HPAON_WAKEUP_SRC_LPTIM1);

    /* ★09-18 时序修正：把 RTC 校准+武装**移到释放 PM 锁之前**。
     * 真故障（09-18 实测日志，逐行核对）：
     *     [POWER] RTC selftest(N=3): WUTF SET (5548ms)      ← 校准自检①成功
     *     [POWER] RTC selftest(N=6): WUTF SET (8865ms)      ← 校准自检②成功
     *     [POWER] RTC(armed) CR=0x1200 WUTR=0x36 ...        ← 已武装 60s
     *     [pm]S:3,1016595                                   ← ★武装后立刻进 DEEP
     *     [pm]W:2689834  [pm]WSR:0x202                       ← 3.1min 后被按键(GPIO1+PIN1/PA25)唤醒
     *     [POWER] RTC selftest(arm-verify): TIMEOUT         ← ★在"进出过 DEEP"之后才跑 → 假失败
     * 两个问题同源：`standby_rtc_arm_timer()` 原来放在下面 "PM locks released" 之后，
     * 无锁 ⇒ 框架在武装完成的一瞬间就把系统丢进 DEEP，而**那段 arm-verify 自检只能等
     * 唤醒回来后才执行**，此时 RTC 刚经历一次 DEEP 进出（sifli_suspend/resume 会重配
     * AON/HPSYS、清 NVIC），计数链状态已变 ⇒ 15s 内等不到 WUTF，报假 TIMEOUT。
     * 同时那次待机（60s 周期）睡了 3.1min 也没触发 RTC 电量检查 ⇒ 该 DEEP 周期里
     * RTC 唤醒同样没走通 —— 都与"自检/武装与进 DEEP 抢时序"相关。
     * 现在全程持 IDLE 锁（见 standby_rtc_arm_timer 内部），且**先武装再释放 PM 锁** ⇒
     * 自检必然在"活跃且不睡"的干净状态下跑完，进 DEEP 前 WUT 已装好。
     * ⚠️ 顺序：本调用必须留在 HAL_HPAON_DisableWakeupSrc(LPTIM1) 之后（WSR 判定才干净）。 */
    standby_rtc_arm_timer();

    /* 释放所有孤儿 PM 锁，避免唤醒后框架误判仍被钉住。
     * 释放浅锁 + 上面已注册 DEEP 策略，idle 线程在无锁且 tick>=2ms 时即选 DEEP。 */
    int i;
    for (i = 0; i < 50; i++)
    {
        rt_pm_release(PM_SLEEP_MODE_IDLE);
        rt_pm_release(PM_SLEEP_MODE_LIGHT);
        rt_pm_release(PM_SLEEP_MODE_DEEP);
    }
    rt_kprintf("[POWER] PM locks released\n");

    /* ===== 走 PM 框架进 DEEP（µA 级）===== */
    g_in_deep_sleep = 1;
    /* ★★★ 09-18 删除：这里原本有 `g_wakeup_requested = 0;` —— 它紧跟在
     * `standby_rtc_arm_timer()`（首轮含 ~20s RTC 校准自检）之后，会把**校准期间
     * 用户按下按键所记下的唤醒请求当场抹掉**，是"准备期按键拦不住待机"的第二个
     * 吞噬点（第一个是 power_standby_wakeup 的 g_standby_active 守卫，已由
     * g_standby_prepare 修复）。
     * 基线清零已上移到函数入口（与本标志一起），阻塞前的排空清零也仍在
     * （见下面的 drain），故此处**不得**再清 —— 否则中止检查永远看不到唤醒。
     * ⚠️ 切勿"顺手加回来"。 */
    g_standby_active = 1;   /* 周期线程(typec/mouse/bat_mon)下次循环顶部自行 suspend */

    /* 删掉 120s 电池上报线程：其 mdelay 定时器是常驻最大周期源；rt_thread_delete
     * 会 detach 其 thread_timer，立即消除该 120s 挂起定时器（否则需等满 120s）。
     * 重建在 power_resume_from_deep。 */
    {
        extern rt_thread_t ble_app_get_bat_rpt_thread(void);
        rt_thread_t bat_rpt = ble_app_get_bat_rpt_thread();
        if (bat_rpt) rt_thread_delete(bat_rpt);
    }

    /* 停掉周期软定时器：rc-cal(15s) 虽在 DEEP tickless 下不触发，
     * 但其"下一个超时"会让框架把 LPTIM 设为 15s 唤醒。待机期间停掉，
     * 唤醒后由 power_resume_from_deep 重启。
     * 08-29: 蓝灯闪烁定时器已整体移除(见前注释)。 */
    if (g_rc_cal_timer) rt_timer_stop(g_rc_cal_timer);

    /* 09-16: 停掉 RW BLE 主机栈的 "BLEHost" 软定时器(端口层 rw_rtt.c 创建, 周期取自 rwip_ticks)。
     * ⚠️ 必须停 —— 实测崩溃根因: 它是 SOFT 定时器(跑在 rt 的 timer 线程), 进 DEEP 时其 timeout
     * 只距当时 ~13min; DEEP 期间 tick 冻结但时间照走, 唤醒瞬间 tick 一次跳一大截 → 它立刻在
     * timer 线程回调 timeout_isr → 取 rwip 时间基(长睡后未同步) → `co_time_get:408 ASSERT`
     * → `fatal error on thread: timer`。定时器转储实证: 只有 BLEHost 仍 activated。
     * 待机期 BLE 已断连/未广播, 停它安全; 唤醒后由 power_resume_from_deep 重新放开。 */
    g_ble_host_timer = power_find_timer_by_name("BLEHost");
    if (g_ble_host_timer) rt_timer_stop(g_ble_host_timer);

    /* 09-16: 连同"广播宽限窗口"定时器(adv_grac, ble_app.c 的 g_adv_grace_timer)一起停。
     * 同类隐患: 它在 DEEP 期间还挂着(转储实测 activated), 唤醒 tick 跳变后立刻回调
     * `adv_grace_timeout_cb` → app_ble_stop_advertising() **直接访问 BLE 广播状态** ——
     * 与 BLEHost 一样会踩到"长睡后 BLE 栈时间基陈旧"。本路径不需要它(待机本来就要停广播),
     * 唤醒后由 ble_app_wakeup_advertise() 重新 arm, 故此处置停、无需恢复。 */
    {
        rt_timer_t adv_grac = power_find_timer_by_name("adv_grac");
        if (adv_grac) rt_timer_stop(adv_grac);
    }
    /* 停 BLE app 状态灯 50ms 软定时器：否则其所在的软定时器线程硬定时器
     * 仍活跃，会作为"最近硬定时器"让框架把 LPTIM1 设成 50ms 周期唤醒。
     * 同时熄灭 app 推送的三颗状态灯（待机应全灭）。*/
    ble_led_turn_off();

    /* 09-16: 停掉"槽位指示"WS2812 闪烁定时器(bt_slot 的 g_blink_timer, PERIODIC 软定时器)。
     * ⚠️ 必须停: 连接态待机现在会【主动断 BLE】→ 触发断开事件 → bt_multi_on_disconnected()
     * → bt_multi_start_indication() 起了这个周期定时器。只要它还有周期, PM 框架就永远到不了
     * tick==MAX → DEEP 进不去(降级 IDLE) → 整晚高电流把电池耗光(用户实测: 47% 睡一晚耗干)。
     * 断连态那条路靠 adv_grace_timeout_cb 停它, 但本路径不经过那里, 必须显式停。 */
    {
        extern void bt_multi_led_stop_all(void);
        bt_multi_led_stop_all();
    }

    /* 恢复 LDO3 + analog 域关断（隔离实验已完成，真因=内部上拉在 DEEP 丢失，
     * 与域关断无关。关断省 ~0.4mA）。
     * ⚠️ 09-18：同 standby_rtc_battery_check 的修复 —— `g_ldo3_off` 可能与硬件
     * 不一致（SDK BSP_Power_Up 会绕过标志重开 LDO3），故先以**寄存器位**判定，
     * 确实带电时清标志再关，避免被幂等保护误挡。 */
    {
        int ldo3_powered = !!(hwp_pmuc->PERI_LDO & PMUC_PERI_LDO_EN_VDD33_LDO3)
                        && !(hwp_pmuc->PERI_LDO & PMUC_PERI_LDO_VDD33_LDO3_PD);
        if (ldo3_powered && power_ldo3_is_off()) {
            rt_kprintf("[POWER] LDO3 flag/hardware mismatch (flag=off, hw=on); fixing\n");
            g_ldo3_off = 0;
        }
    }
    power_ldo3_domain_off();
    power_analog_domain_off();

    /* ★★★ 09-18(4) 新增修复：清掉"语音(AUDIO)场景残留"——DBGR.FORCE_HP。
     *
     * === 现象 ===
     * 用户实测：**语音过一次后待机，电流 1.3mA**（且 `pre-deep-1st` 显示
     * `ISSR=0x30 LP_ACTIVE=1(lcpu_sleep=0)`、`LPSYS_LDO=0x0001041d(RDY=1)`、
     * `LPSYS_SWR=0x80000036(RDY=1)`），与早前 16.3mA 那次三处异常**完全相同**。
     *
     * === 机制（逐句读 SDK 源码定案，非推断）===
     * 1) `pm_scenario_start(PM_SCENARIO_AUDIO)`（`bf0_pm.c:3290`）执行：
     *        hwp_hpsys_rcc->DBGR |= HPSYS_RCC_DBGR_FORCE_HP;   // DBGR bit4
     *    语义 = **强制开 HP 时钟**（DeepWFI 期间也不停）+ HCPU 降到 48MHz。
     *    只有 `pm_scenario_stop(PM_SCENARIO_AUDIO)`（`bf0_pm.c:3318`）才清。
     *    ⚠️ 该位一旦残留 ⇒ **HCPU 就算真进了 `[pm]S:3`(DEEP) 也仍被强制供给时钟**
     *    ⇒ 恒定 mA 级，而 SoC 其余可观测状态全部"干净"（我们前几轮排查的困境正是它）。
     *    本工程 `main.c::usb_stop()` 已经知道这个位（注释"清 FORCE_HP(仅音频场景用)"），
     *    但**只处理了 USB 那条路径**，待机/DEEP 路径从未核对过。
     * 2) `HAL_HPAON_WakeCore(CORE_ID_LCPU)` 置 `ISSR.HP2LP_REQ`，并在 52x 上启用
     *    引用计数（`AON_LCPU_ACTIVE_REQUEST_REF_COUNT_SUPPORT`，`bf0_hal_aon_sf32lb52x.h:19`）。
     *    只有 `HAL_HPAON_CANCEL_LP_ACTIVE_REQUEST()` 把计数减到 0 才清 `HP2LP_REQ`。
     * 3) **语音路径**（`audio_server.c::audio_client_stop`，`AUDIO_TYPE_BT_VOICE`）是唯一
     *    成对做这两件事的地方：
     *        HAL_HPAON_CANCEL_LP_ACTIVE_REQUEST();   // 释放小核
     *        pm_scenario_stop(PM_SCENARIO_AUDIO);    // 清 FORCE_HP
     *    ⇒ 若语音会话**没有走"正常 audio stop"**（例如 SCO 由 ACL 掉线收尾、
     *      客户端引用计数没归零），这两句都不执行 ⇒ 小核永不休眠 + HP 时钟被强开
     *      ⇒ 与用户实测完全吻合（`LP_ACTIVE=1` + `LPSYS_LDO/SWR RDY=1` + mA 级）。
     *    另：`bluetooth_config.c:1568` 在 `uart_ipc_path_change()`（BT 初始化时）也调用
     *    `pm_scenario_start(PM_SCENARIO_AUDIO)`，而**整个蓝牙中间件里没有对应的 stop**。
     *
     * === 修法 ===
     * 待机是"确定要极低功耗"的状态，音频场景不应存在 ⇒ 在此无条件清 FORCE_HP。
     * ⚠️ 只清 DBGR 位（power.c 的职责边界内），**不碰 audio 客户端引用计数**
     *    （那属音频/蓝牙栈内部状态，误减会让后续语音异常）。清位后若下次真要用音频，
     *    `pm_scenario_start` 会重新置上，无副作用。
     * ⚠️ 必须在 `power_dump_standby_state("pre-deep-1st")` 之后执行，才能保留"清之前"
     *    的现场（否则诊断看不到残留值，反而掩盖问题）。 */
    if (hwp_hpsys_rcc->DBGR & HPSYS_RCC_DBGR_FORCE_HP)
    {
        rt_kprintf("[POWER] stale AUDIO scenario: DBGR.FORCE_HP=1 (voice residue) -> clearing\n");
        hwp_hpsys_rcc->DBGR &= ~HPSYS_RCC_DBGR_FORCE_HP;
    }
    /* 空闲 WFI 应降到最低速档（div 60）。USB 在线时 main.c 会设回 div 12，
     * 拔线时也会恢复；若因异常路径残留在 div 1/12 ⇒ WFI 期间仍跑高速 ⇒ 耗电。 */
    HAL_RCC_HCPU_SetDeepWFIDiv(60, 0, 1);

    /* 进 DEEP 前处理按键唤醒：key_enable_deep_wakeup 逐键使能 AON per-pin
     * 【下降沿】唤醒(AON_PIN_MODE_NEG_EDGE)——SDK pm example/classical 给唤醒键用的
     * 标准范式。边沿模式静止态(pin=HIGH)不锁存 → DEEP 可进(0.2mA)；按下=LOW 下降沿
     * → 唤醒。绝不能用 AON_PIN_MODE_LOW(电平)：按下期间会持续锁存 WSR PIN 位 →
     * DEEP 降级 IDLE（实测 2026-07-30，根因是电平锁存与通道无关）。 */
    {
        /* key_enable_deep_wakeup 内部已用 HAL_GPIO_Init(GPIO_MODE_IT_FALLING)
         * 强制使能 7 键 EXTI（IESR/ITSR/IPLSR），不再需要 key_reinit_after_deep
         * （实测它的 button_enable 未生效，IESR 全 0）。*/
        extern void key_enable_deep_wakeup(void);
        key_enable_deep_wakeup();
    }

    /* DEEP policy 已在释放 IDLE 锁之前注册（避免时间窗口内误选 LIGHT）。 */

    {
        extern rt_tick_t rt_timer_next_timeout_tick(void);
        rt_tick_t nxt = rt_timer_next_timeout_tick();
        rt_kprintf("[POWER] entering DEEP standby; next_timeout_tick=%lu (MAX=%lu)\n",
                   (unsigned long)nxt, (unsigned long)RT_TICK_MAX);
    }

    /* ===== 最后一步：等 LCPU/BLE 真正安静并清 WSR 残留 =====
     * 必须放在所有 quiesce 之后、阻塞之前：bt_interface_close_bt() 是异步的，
     * LCPU 的应答 IPC 会在 close 之后继续把 LP2HP 位挂进 WSR。
     * 连续 5 次(500ms)干净才认为安静；最多等 10s。 */
    /* quiesce 期间临时持 IDLE 锁：其 rt_thread_mdelay(100) 轮询会让框架每 100ms
     * 进/出一次 DEEP（日志表现为十几组 [pm]S:3 / [pm]W / WSR:0x4），既无谓耗电
     * 又干扰观察。持锁把这段钉在 IDLE，轮询结束后再释放。 */
    standby_prepare_deep_gate("pre-quiesce", 10000);
    /* ★09-18 诊断：第一次进 DEEP 的现场（作为 baseline，与第二次的 pre-deep-2nd 对比） */
    power_dump_standby_state("pre-deep-1st");

    /* ★★★ 09-18 修复 BUG：准备窗口内的唤醒不能被吞掉 —— 必须中止待机。
     *
     * === 实测（用户日志，待机首轮 = 含 ~20s RTC 校准）===
     *   用户在校准期按了按键 ⇒ 蓝牙已重连(**live link**)、小核醒着（`LP_ACTIVE=1
     *   lcpu_sleep=0`）、`imu_want=1`，但设备**仍然强行 `[pm]S:3` 进了 DEEP**
     *   （`pre-deep-1st` 那几行就是现场）。后果在下次 RTC 唤醒时显现：
     *   `i2c bus err` / `[LSM] WHO_AM_I read failed: -8` / `lsm_init failed!`
     *   —— 待机清场把 IMU/I2C 在"逻辑上仍连接"的状态下电源循环了。
     *
     * === 根因 ===
     *   本函数在"释放 PM 锁 → RTC 校准/武装(首轮 ~20s) → quiesce"这段长准备期内，
     *   `g_standby_active` 已置 1、DEEP policy 已注册、PM 锁已释放
     *   ⇒ 系统能睡，按键也能经 `BSP_PowerUpCustom()→power_standby_wakeup()` 置
     *   `g_wakeup_requested`。而下面这段"排空 + 清零"会把这个唤醒请求**无条件丢掉**
     *   ⇒ 醒来后又被打包推进 DEEP。用户感受："按了按键应该打断休眠重新开始，但没有"。
     *
     * === 修法 ===
     *   清零之前先记下"准备期是否已被真唤醒"（`g_wakeup_requested`）；是则跳过 DEEP，
     *   直接走函数尾部同款的 `power_resume_from_deep()` 恢复路径 —— 与真唤醒完全同一条
     *   路，因为外设确实已被本函数关过（BT/LDO3/analog/软定时器/PM 锁）。
     *   排空与清零**两条路径都要做**：否则残留标志会让下一次待机一进门就被打断。 */
    {
        int woken_during_prepare = (g_wakeup_requested != 0);

        /* 排空 + 清零（保持原语义；顺序不变） */
        g_wakeup_requested = 0;
        while (rt_sem_take(g_standby_sem, 0) == RT_EOK)
            ;   /* drain */
        g_standby_rtc_wake = 0;   /* 09-09: 清可能残留的 RTC 电量检查标志（排空语义的一部分） */

        if (woken_during_prepare)
        {
            rt_kprintf("[POWER] standby ABORTED: wake request arrived during prepare "
                       "(skip DEEP, restoring peripherals)\n");
            g_standby_prepare = 0;      /* ★ 与入口成对清位 */
            g_in_deep_sleep = 0;
            power_resume_from_deep();
            rt_kprintf("[POWER] resumed from standby (aborted before DEEP)\n");
            return;
        }
    }

    /* ===== 定时器现场取证（默认关，排查周期性假唤醒时改 1）===== */
/* 09-16 临时置 1: 排查 `rwip_assert / co_time_get:408 (ASSERT)` + `fatal error on thread:
 * timer`(长待机唤醒时崩, 且崩在 power_resume_from_deep 之前 = SDK 恢复阶段/软定时器回调)。
 * 打印进 DEEP 前所有挂起定时器 → 定位是哪个软定时器在唤醒瞬间回调里取 rwip 时间基断言。
 * 09-17 排查结束(`co_time_init(0)` 已定案修复) → 改回 0(每次待机都会刷一大段定时器列表)。
 * 需要时把下面的值改回 1 即可重新取证。 */
#ifndef POWER_DUMP_TIMERS_BEFORE_SLEEP
#define POWER_DUMP_TIMERS_BEFORE_SLEEP 0
#endif
#if POWER_DUMP_TIMERS_BEFORE_SLEEP
    {
        extern long list_timer(void);
        rt_kprintf("[POWER] === timer dump before blocking (tick=%u) ===\n",
                   (unsigned)rt_tick_get());
        list_timer();
    }
#endif
    /* 睡前最后一眼：此刻 close_bt 已完成，小核应已自主进睡。
     * lcpu_sleep=1 + 大核 [pm]S:3 才可能是 µA 级；lcpu_sleep=0 则电流必在 mA 级。 */
    /* 🔴 LOW 电平模式：AON per-pin 检测器在 pin=HIGH 时仍异常置 WSR PIN 位→
     * sifli_suspend EBUSY→DEEP 降级 IDLE。quiesce 清了又被 AON 检测器重置。
     * 解法：blocking 前清 WSR + 暂时关掉 AON PIN 的 WER 位（不让 PIN 参与
     * EBUSY 判定），只留 GPIO1(bit1) 做 DEEP 唤醒。唤醒后恢复 WER PIN 位。 */
    /* edge 模式不锁存 WSR PIN，不需要屏蔽。直接进 DEEP。 */
    HAL_HPAON_CLEAR_WSR(HAL_HPAON_GET_WSR());

    rt_kprintf("[POWER] DEEP standby (key wake: GPIO1 + AON PIN0-20), lcpu_sleep=%d, blocking\n",
               power_lcpu_is_sleeping());

    /* 看门狗式阻塞：30s 超时轮转。若 LCPU 事后再发 IPC 把 WSR 挂上
     * （运行态无人清 -> DEEP 永久降级 IDLE），每 30s 醒来清一次恢复
     * DEEP 通路。30s LPTIM 唤醒对 DEEP 平均电流影响可忽略(µs 级醒)。
     * ⚠️ 超时【不算唤醒】：LPTIM1 唤醒时 BSP_PowerUpCustom 的 WSR 过滤不会置
     * g_wakeup_requested，故 while 条件仍为真 → 清完 WSR 继续阻塞、框架自动再入
     * DEEP。只有 WSR 含 GPIO1（真按键）才会退出。 */
    while (!g_wakeup_requested)
    {
        if (rt_sem_take(g_standby_sem, rt_tick_from_millisecond(10000)) == RT_EOK)
        {
            /* 09-09 方案 B：RTC 电量检查唤醒 → 轻量路径（不置 g_wakeup_requested、
             * 不重建外设/蓝牙，查完电量重新武装闹钟后 continue 继续睡）。
             * 只有真按键唤醒（BSP_PowerUpCustom 走 power_standby_wakeup 置标志）才 break。 */
            if (g_standby_rtc_wake)
            {
                standby_rtc_battery_check();
                continue;
            }
            break;   /* 按键唤醒 */
        }
        /* 超时看门狗：无条件打印睡眠状态，定位"唤不醒"（PMR mode: 0/1=?, 2=DEEP, 3=STANDBY）
         *  - PMR mode==2 且 WSR 无按键位 → 在 DEEP，AON 检测未触发（PAD/电平问题）；
         *  - PMR mode!=2 → DEEP 未进/已降级 IDLE（此时按键应走 key_handler 兜底唤醒）；
         *  - WSR 含按键 PIN/GPIO1 位 → 检测到了但唤醒路由断了。 */
        {
            uint32_t pmr = hwp_hpsys_aon->PMR;
            uint32_t wsr = HAL_HPAON_GET_WSR();
            uint32_t pend = wsr & HAL_HPAON_GET_WER();
            rt_kprintf("[POWER] watchdog: PMR=0x%08x(mode=%lu) WSR=0x%08x pend=0x%08x "
                       "PIN=0x%08x lcpu_sleep=%d\n",
                       (unsigned)pmr, (unsigned long)(pmr & 0x3), (unsigned)wsr,
                       (unsigned)pend, (unsigned)HAL_HPAON_GET_WSR_PIN(),
                       power_lcpu_is_sleeping());
            if (pend)
                HAL_HPAON_CLEAR_WSR(wsr);
        }
    }
    g_standby_prepare = 0;      /* ★ 与入口成对清位（正常真唤醒路径） */
    g_in_deep_sleep = 0;

    power_resume_from_deep();
    rt_kprintf("[POWER] resumed from standby\n");
}

static void sleep_thread_entry(void *param)
{
#ifdef DEBUG_KEEP_AWAKE
    /* 调试态：不进待机，永久持有 IDLE 锁，设备常醒、外设不掉电、BLE 常运行。
     * 仅打印一次提示，随后空转。DEBUG_KEEP_AWAKE 关闭后恢复正式待机逻辑。 */
    rt_pm_request(PM_SLEEP_MODE_IDLE);
    rt_kprintf("[POWER] DEBUG_KEEP_AWAKE: IDLE locked, deep sleep disabled\n");
    while (1)
    {
        rt_thread_mdelay(1000);
    }
#else
    while (1)
    {
        rt_thread_mdelay(500);
        /* ⚠️ USB 插入期间禁止进 DEEP 待机：待机流程会强制 usb_stop() 把 USB 控制器
         * 和时钟全关 → 电脑端 USB 总线级断开、COM 口消失(08-06 定案"插着电脑
         * 也掉线"根因)。USB 在线 = 用户需要保持连接，跳过待机分支，拔掉才深睡。
         * (08-27 用户确认: 插 USB 时不要进休眠 —— 无论蓝牙连没连都不休眠。)
         * ⚠️ 通话(SCO)中同样禁止进待机：通话中 BLE 断开(信号波动/主机省电)是
         * 常态，若无 USB 且 60s 未回连 → 待机流程 bt_interface_close_bt() 会掐断
         * SCO → 通话被强制中断(08-06 排查"蓝牙有害行为"定案)。SCO 依赖经典
         * 蓝牙链路, 与 BLE 连接状态无关, 通话中必须保持系统活跃。 */
        if (usb_on || g_sco_active)
            continue;

        if (!g_bt_connected)
        {
            /* 未连接：60s 等待回连, 仍未连则进待机(原逻辑) */
            rt_kprintf("[POWER] BT disconnected, waiting 60s\n");
            uint32_t elapsed = 0;
            while (elapsed < 60000 && !g_bt_connected)
            {
                rt_thread_mdelay(1000);
                elapsed += 1000;
            }
            if (!g_bt_connected)
                power_enter_standby();
        }
        else
        {
            /* 08-27: 连接态空闲超时休眠 —— 用户可配(sleep_min, 最小档 45 分钟)。
             * 空闲判定: main.c 连接态空闲状态机(静止~1s 进空闲, 按键/鼠标/编码器
             * 活动立即回活跃并清零计时)。达到阈值 -> 主动断 BT 进 DEEP 待机,
             * 按键唤醒后广播等主机回连。 */
            /* ★★★ 09-18 临时测试开关：把"连接态空闲待机"阈值改短，便于快速验证。
             * 背景：sleep_min 单位是**分钟**且合法档位 45~240 (key_config.h:258-260)，
             *       无法表达"30 秒"，故不能走配置项，只能在此临时短路。
             * 用法：设为 >0 的秒数即启用（无视 sleep_min 配置）；设 0 = 关闭本开关，
             *       完全恢复原有 sleep_min 分钟逻辑。
             * ✅ 验证已完成，**已复位为 0** —— 恢复按 sleep_min(45~240 分钟) 正常待机。 */
#define IDLE_SLEEP_TEST_SEC  0

#if IDLE_SLEEP_TEST_SEC > 0
            {
                uint32_t idle_ms = air_mouse_idle_ms();
                if (idle_ms >= (uint32_t)IDLE_SLEEP_TEST_SEC * 1000)
                {
                    rt_kprintf("[POWER] BT connected, idle %lu ms >= %d s (TEST), entering standby\n",
                               (unsigned long)idle_ms, (int)IDLE_SLEEP_TEST_SEC);
                    power_enter_standby();
                }
            }
#else
            uint16_t sleep_min = key_config_get_sleep_min();
            if (sleep_min > 0 && sleep_min <= KEY_CONFIG_SLEEP_MIN_MAX)
            {
                uint32_t idle_ms = air_mouse_idle_ms();
                if (idle_ms >= (uint32_t)sleep_min * 60 * 1000)
                {
                    rt_kprintf("[POWER] BT connected, idle %lu ms >= %u min, entering standby\n",
                               (unsigned long)idle_ms, (unsigned)sleep_min);
                    power_enter_standby();
                }
            }
#endif
        }
    }
#endif
}

void power_set_bt_connected(uint8_t connected)
{
    g_bt_connected = connected;
    if (connected)
    {
        rt_kprintf("[POWER] BT connected\n");
        g_bat_mon_sleep = 0;   /* 连接态：电池监控恢复正常节拍 */
        /* 08-29: LED1 不再做连接指示(槽位 RGB 已覆盖), 由 bt_multi_set_charge 管充电指示 */
    }
    else
    {
        rt_kprintf("[POWER] BT disconnected\n");
    }
}

/* 进深睡待机：电池监控进入降频态（g_bat_mon_sleep=1），释放周期唤醒源。
 * 08-29: LED1 不再做连接指示(槽位 RGB 已覆盖, 充电指示由 bt_multi_set_charge 管);
 * 充电/USB 插入期间本函数不会触发(不休眠)。由广播宽限窗口超时调用。 */
void power_bt_indicator_sleep(void)
{
    g_bat_mon_sleep = 1;
    rt_kprintf("[POWER] bt indicator sleep (bat mon slow)\n");
}

/* 唤醒/广播期：电池监控恢复活跃节拍。由按键唤醒/断连后重开广播调用。 */
void power_bt_indicator_active(void)
{
    g_bat_mon_sleep = 0;
    rt_kprintf("[POWER] bt indicator active (bat mon normal)\n");
}

/* DEEP 唤醒后硬件寄存器丢失（USB/UART/音频/GPIO 配置等），但 RAM 状态保持。
 * 此函数在 sleep_thread 待机循环被唤醒后调用，重建必要外设。
 * 调用时机：sifli_deep_handler 返回、BSP_Power_Up(含 BSP_PowerUpCustom) 已完成之后，
 * 即系统已退出 DEEP、节拍定时器恢复、sleep_thread 的 mdelay 返回之时。
 * 注意：被唤醒后才调用，避免在 DEEP 唤醒早期硬件未稳时操作外设。 */
static void power_resume_from_deep(void)
{
    g_standby_active = 0;
    /* 恢复进 DEEP 前屏蔽的 AON PIN WER 位（key_enable_deep_wakeup 重新使能即可）。
     * key_disable_deep_wakeup 会先关掉，然后 key_reinit_after_deep 重建。 */
    key_enable_deep_wakeup();   /* 恢复 AON per-pin WER 位（进 DEEP 前屏蔽过） */
    /* 恢复 LPTIM1 唤醒源：活跃态 tickless 需要它做周期唤醒；待机期被关掉以杜绝
     * 周期把系统拽出 DEEP。必须在重建外设前恢复，否则后续活跃态 PM 退不出深睡。*/
    HAL_HPAON_EnableWakeupSrc(HPAON_WAKEUP_SRC_LPTIM1, AON_PIN_MODE_HIGH);
    /* 唤醒其余被 suspend 的周期线程（其挂起定时器已超时摘除，tick 已可趋 MAX） */
    for (int i = 0; i < g_suspend_thread_cnt; i++)
        if (g_suspend_threads[i]) rt_thread_resume(g_suspend_threads[i]);
    /* 重建 120s 电池上报线程（待机时 delete 过其定时器） */
    {
        extern void ble_app_init_battery_report(void);
        ble_app_init_battery_report();
    }
    /* 重启本模块周期定时器（rc-cal 15s） */
    if (g_rc_cal_timer) rt_timer_start(g_rc_cal_timer);
    /* 恢复 IDLE-only policy（活跃态靠 IDLE 锁短路，不进 LIGHT/DEEP） */
    rt_pm_policy_register(1, g_normal_policy);
    rt_kprintf("[POWER] power_resume_from_deep: rebuilding peripherals "
               "(wake WSR=0x%08x, deep_wakes=%u spurious=%u, lcpu_sleep=%d)\n",
               (unsigned)g_last_deep_wsr, (unsigned)g_deep_wake_cnt,
               (unsigned)g_deep_spur_cnt, power_lcpu_is_sleeping());
    rtc_wut_dump("post-deep");   /* 09-16 诊断: 对比武装时的 TR/CR, 判断 RTC 时钟是否跨 DEEP 停摆 */
    /* ★09-18 诊断：DEEP 唤醒刚回来、**尚未重开 LDO3/analog 之前**的现场。
     * 这里若看到 ldo3_off=0 或 CHG_CR3.FORCE_RST=0，即证明 SDK 的 BSP_Power_Up()
     * 在唤醒早期把域重开了（它不看 is_deep_sleep，只看 BSP_ImuPowerWanted()）。 */
    power_dump_standby_state("just-woke");

    /* 1. 重新打开按键中断。DEEP 断 HPSYS 域后不仅 NVIC 状态丢失，GPIO1 的 pin 中断
     *    使能寄存器与 PAD 复用/上拉配置也一并丢失，故除了 NVIC 还要重做每个按键 pin
     *    的 HAL_PIN_Set + rt_pin_irq_enable（见 key_config.c::key_reinit_after_deep）。 */
    HAL_NVIC_SetPriority(GPIO1_IRQn, 5, 0);
    HAL_NVIC_EnableIRQ(GPIO1_IRQn);
    {
        extern void key_reinit_after_deep(void);
        key_reinit_after_deep();
        extern void key_disable_deep_wakeup(void);
        key_disable_deep_wakeup();   /* 唤醒后关 AON per-pin 唤醒，交还 HPSYS GPIO 中断 */
    }

    /* 1.5 恢复 LDO3 3.3V 外设域（编码器/I2C 上拉、IMU、RGB 都靠它）。
     *     放在按键重建之后、USB/BLE 之前：按键响应最紧急，而本函数内含 20ms
     *     IMU boot 等待。
     * ⚠️⚠️ 09-18 更正：原注释称"BSP_Power_Up 早期不会抢先开 LDO3 —— BSP_ImuPowerWanted
     *     已用 power_ldo3_is_off() 挡住"是**错误的**：`main.c:1665` 的实现
     *     `return g_imu_power_wanted;` **没有任何 power_ldo3_is_off() 判断**，
     *     而 `g_imu_power_wanted` 初值=1（main.c:1662），仅在 air_mouse_stop()
     *     时置 0。⇒ DEEP 唤醒时 SDK 会**直接在 power_ldo3_domain_on() 之前**把 LDO3
     *     硬开。这正是"第二次 DEEP 期间 26.5mA"的根因（见 standby_rtc_battery_check）。 */
    power_ldo3_domain_on();
    power_analog_domain_on();   /* 恢复片内模拟域（充电器/codec REFGEN/GPADC，对应待机断电）*/

    /* 2. 深睡唤醒后重建 USB：以充电器 VBUS_RDY 真实状态为准（待机已 usb_stop 把
     *    usb_on 清 0；10-05 方案B 后不再有 PA44 边沿/typec 线程，唤醒时不会自发重启
     *    → CDC 永久失联）。
     *    插着 USB 唤醒即补发 usb_start 触发 CDC 重枚举；PC 端虚拟串口短暂消失后
     *    由上位机 PortDetector 的 CONF_CMD_INFO 握手自动恢复。 */
    extern void usb_resume_if_plugged(void);
    extern void ble_app_wakeup_advertise(void);
    usb_resume_if_plugged();

    /* 2.4 09-17 ★★修复 `co_time_get:408 ASSERT`（实测定案后的正式修法）。
     *
     * === 真因（实测日志实证，非推断）===
     * 唤醒后诊断打印:
     *     [POWER] TB rt_tick=6699488 last_clock=2705778 delta=3993710(s32)
     *             d*1000=0xee0b2db0 bit31=1 -> co_time_get WOULD ASSERT!
     *     [POWER] TB diagnose: FORWARD overflow, slept 3993710 ms (66.56 min)
     *             => matches case A (35.8~71.6min window)
     * ⇒ `delta = rt_tick - co_time_env->last_clock = 66.56min`，**正向**落在断言窗口
     *   (35.8~71.6min) 内 ⇒ 随后 BLE 回连加密完成、LCPU arm 定时器走 `co_timer_prog`
     *   → `co_time_get` 读到该 delta → `1000*delta` 溢出 int32 → 断言。**完全对得上**。
     *
     * === 为什么 delta 会累积到 66min（关键）===
     * `co_time_env->last_clock` 只在 **`co_time_get` 被调用时**才更新。
     * 而待机期间 BLE 是关的(close_bt + 停广播) ⇒ **整个待机期没有任何 `co_time_get` 调用**
     * ⇒ `last_clock` 冻结在"待机前最后一次 BLE 活动"的时刻。
     * 更要命的是**跨次累积**：第一次连接态待机(45min 空闲)记下 last_clock=2705778，
     * 之后 **RTC 轻量唤醒(`standby_rtc_battery_check`)不调本函数、不碰 BLE**
     * ⇒ last_clock 仍是 2705778；再睡第二次、直到按键真唤醒才走本函数。
     * ⇒ 两次 DEEP 的全部时长都算进 delta。**单次 DEEP 只有 ~16min，两次加起来才够 66min。**
     * 这也解释了为什么"睡 12min 也崩"(上一轮)—— 不是单次时长，是**跨次累积**。
     *
     * === 修法：唤醒后立刻 `co_time_init(0)` ===
     * 反汇编 `co_time_init`(lib_bt_gcc.a 的 co_time.o，最终 elf `T @0x12073e50`):
     *     cbnz r0, 22           ; arg!=0 → 跳过下面整段(只保留旧值, 不是我们要的)
     *     str  r0,[env,#12]     ; offset32 = 0
     *     strb r0,[env,#16]     ; offset8  = 0
     *     strh r0,[env,#8]      ; frac     = 0
     *     bl   rwip_time_get
     *     str  r3,[env,#4]      ; ★last_clock = 现在      ← 正是需要的"重设时间基"
     *     _ke_event_callback_set_handler(6, co_timer_handler)  ; 重设回调(幂等)
     *  22: co_time_get()         ; 立刻对齐一次
     *      co_time_sys_timer_update()  ; 重算系统定时器(rwip_timer_co_set)
     * ⇒ **`co_time_init(0)` 就是"BLE 协议栈时间基复位"的官方入口**。
     * 佐证：全库扫描它的调用者**只有 `_rwip_init`(x2) 与 `_rwip_reset`(x1)**
     * —— 即协议栈初始化/复位，语义与我们的需求完全一致。
     *
     * === 位置要求（重要）===
     * 必须在**任何会 arm BLE 定时器的动作之前**：
     *   - `bt_interface_open_bt()`(2.5, 经典栈 open，会走 BTS2 定时器)
     *   - `ble_db_init()`(2.7)、`ble_app_wakeup_advertise()`(步骤3, 一广播 LCPU 就工作)
     * 故放在 2.5 之前。此刻 BLE 尚未 open/广播/连接，重置时间基最安全。
     * ⚠️ 绝不能放在广播/连接之后 —— 那时 LCPU 可能已读到旧时间基。
     *
     * === 安全性 ===
     * - `co_time_init` 是 `T` 全局符号(最终 elf 0x12073e50)，可链接(已 nm 核实)；
     * - 幂等：清 offset/frac 归零、last_clock 重取当前、回调重设(与 _rwip_init 同款)；
     * - 不碰 flash / 不注册 rt 对象 / 无内存分配。
     * - 注：`co_time_init` 无头文件声明 ⇒ 用 `extern` 局部声明(SDK 自身示例同样写法)。
     *
     * === 连带效果 ===
     * 本调用同时把 offset32/offset8/frac 清零 —— 那是补偿"BLE 睡眠期间 rwip 时钟停摆"的
     * 累加量；跨 DEEP 后本就该归零重来，故一并清掉是正确的。 */
    {
        extern void co_time_init(uint8_t arg);
        co_time_init(0);   /* arg=0 → 真正执行"重置 last_clock 为当前时刻"的分支 */
        rt_kprintf("[POWER] BLE timebase reset: co_time_init(0) done (last_clock re-based)\n");
    }

    /* 2.5 09-16: 补齐与待机前 bt_interface_close_bt() 对称的"重新打开经典蓝牙栈"。
     * 原因（实测日志定案）: close_bt_request 会 close-all(日志 hdlffff) 并丢掉 BR/EDR 安全
     * 上下文(link key)，而全固件从不调用 open_bt ⇒ 唤醒后经典蓝牙认证必失败
     * (日志里"ACL 建起来但整段没有 rd lk/granted")，语音/HFP 连不上，
     * **只有重启设备**(重新 init 栈、从 flash 载入 key)才恢复 —— 用户实测"重启就好"。
     * 实测有效: 加上后唤醒重连出现 `sec_state:1 → granted 1 → HF Conneted success`
     * (`[APP] HFP connected`, BLE->HFP 7833ms) ✓
     * 功耗: 不影响待机(待机前 scan 已关 + 栈已 close)；open 自带 inquiry scan 会让设备
     * 短暂"可被搜索"，紧接 set_scan_mode(0,1) 立刻恢复"不可被搜索/仅可连接"全线策略。 */
    {
        extern void bt_interface_open_bt(void);
        extern void bt_interface_set_scan_mode(uint8_t scan_mode, uint8_t enable);
        bt_interface_open_bt();
        bt_interface_set_scan_mode(0, 1);
    }

    /* 2.6 09-16: 放开待机前停掉的 RW BLE 主机栈软定时器 "BLEHost"（见待机入口注释 ——
     * 不停它会在 DEEP 唤醒瞬间于 timer 线程回调里断言 co_time_get 崩溃）。 */
    if (g_ble_host_timer) rt_timer_start(g_ble_host_timer);

    /* 2.7 09-16: 深睡唤醒后重建 BLE 栈数据库/状态。
     * SDK 的 `ble_pm_resume(device, mode)`(bf0_bt_nvds.c:1398) 只在 mode==STANDBY/DEEP 时
     * 才执行下面这句 `ble_db_init()`。本固件待机走的就是框架 DEEP(日志 [pm]S:3), 但该钩子
     * 很可能因 mode 取值判定被**短路** ⇒ BLE 栈状态在长睡后陈旧 ⇒ 唤醒后任何取 rwip 时间的
     * 地方都断言(`co_time_get:408 ASSERT`: 先在 timer 线程, 停掉 BLEHost 后转到 KE_EVT2)。
     * ⚠️ 不能直接调 ble_pm_resume: 它整个包在 `#if defined(BSP_USING_PM) &&
     *    !defined(BSP_USING_PSRAM)` 里, 而本板用 PSRAM ⇒ 该符号根本不存在(实测 ld
     *    undefined reference)。故直接调它内部那句 —— `ble_db_init()` 在 #if 之外,
     *    无条件编译, 语义相同。
     * 放在 BLE 广播重启(步骤3)之前: 此刻 BLE 尚未连接/未广播, 重建库安全。 */
    {
        extern void ble_db_init(void);
        ble_db_init();
    }

    /* 2.8 09-17: 刷新 LCPU 侧 NVDS 共享内存(修 `co_time_get:408 ASSERT` 真根因)。
     * 背景(反汇编+ELF 实证, 见 09-16/09-17 记忆):
     *   - 该断言条件 = `1000*(curr_clock-last_clock)` 溢出 int32, 即 rwip 时间基一次
     *     跳变落在 ~35.8min..71.6min 时**必炸**。45min 待机正中窗口 ⇒ 连接态待机回连时崩。
     *   - 停 BLEHost/adv_grac 软定时器只把崩溃点从 timer 线程**搬**到 KE_EVT2 线程
     *     (LCPU 侧 ble_svc_change_enable 自己 arm 定时器 → co_timer_prog → co_time_get)。
     *     ⇒ 真因不是某个定时器, 而是 BLE 栈时间基跨 DEEP 从未重同步。
     *   - LCPU 读的是**共享内存** `NVDS_BUFF_START`(52x=0x2040FE00), 由
     *     `bt_stack_nvds_init()`(bf0_bt_common.c:327) 从 flash 重读 STACK buffer 再 memcpy
     *     —— 而它只在开机 `ble_power_on()` 里调**一次**, 唤醒路径**从无刷新** ⇒
     *     LCPU 一直用开机时的 EXT_WAKEUP_TIME(tag 0x0D, 用于补偿唤醒延迟/对齐时间基)。
     *
     * ⚠️ 为什么**不能**调 `sifli_nvds_init()`(原方案, 已否决):
     *   它第一步 `rt_mutex_init(&ble_flash_mutex,...)`, 而 rt_object_init(object.c:278-286)
     *   有 `RT_ASSERT(obj != object)` 去重断言 —— 该 static mutex 开机已注册,
     *   二次调用必断言/重复插链表损坏对象容器。
     *
     * 本调用(bt_stack_nvds_init)的安全性:
     *   - 不做任何 rt_object_init, 不碰对象链表;
     *   - 内部 `HAL_HPAON_WakeCore(CORE_ID_LCPU)` 自带 ref-count(上限 20)+LP_ACTIVE 自旋,
     *     且成对 `HAL_HPAON_CANCEL_LP_ACTIVE_REQUEST()`;
     *   - 只读 flash → 写共享内存, 幂等。
     * 位置: 必须在 BLE 广播/连接之前(BLE 一起来 LCPU 就会读该 buffer),
     *       且与 2.7 一样放在步骤 3 之前。
     * 注: 本步只"刷新既有值"(本板 rc10k 默认 6500), **不改数值** ——
     *     目的是先验证"共享内存陈旧"这一假设; 若仍崩, 再评估是否需一并
     *     `ble_nvds_update_wakeup_time()`(它会把值改成 4500, 与 SDK 默认表 6500 打架, 有取舍)。 */
    {
        extern void bt_stack_nvds_init(void);
        bt_stack_nvds_init();
        /* 读回共享内存头部做实证: pattern 应为 0x4E564453("NVDS"), used_mem 应 >0。
         * 这样下一轮日志能直接判断"刷新真的落到了 LCPU 读的那块内存", 而不是靠推测。
         * 结构 = {u32 pattern; u16 used_mem; u16 writting;} 紧跟数据(见 bf0_bt_common.c:319)。 */
        {
            volatile uint32_t *p = (volatile uint32_t *)0x2040FE00u;
            rt_kprintf("[POWER] nvds share refreshed: pattern=0x%08x used=%u writing=%u\n",
                       (unsigned)p[0], (unsigned)((p[1]) & 0xFFFFu), (unsigned)((p[1]) >> 16));
        }
    }

    /* 2.9 09-17: ★GTIMER 时基校准（`gtime_freq` 偏慢 10.6% 的正式修法）。
     *
     * 背景（全部代码实证，非推测）:
     *   本板 `proj.conf: CONFIG_LXT_DISABLE=y` ⇒ **没有 32.768kHz 晶振**，
     *   低频域只能用内部 RC。`bsp_init.c:81` 选 `PMU_LPCLK_RC32`（PMUC_CR.SEL_LPCLK=1），
     *   而 `#ifndef LXT_DISABLE` 那段（EnableXTAL32 / RTC_ENABLE_LXT）整段不编译
     *   ⇒ `RTC_CR.LPCKSEL=0`（实测吻合）⇒ RTC 走 RC10K 域。
     *
     *   `HAL_LPTIM_GetFreq()`(drv_common.c:398, Flash 非 ROM) 返回
     *      48000000 / HAL_Get_backup(RTC_BACKUP_LPCYCLE_AVE) * HAL_RC_CAL_GetLPCycle()
     *   实测 `LRC_CAL_LPCYCE=200`、返回值 8866 ⇒ 反推 AVE = 48000000*200/8866 = 1082788。
     *   这个 8866 就是 PM 用来把 GTIMR 增量换算成 tick 的 `gtime_freq`
     *   (bf0_pm.c:2591/2596、drv_common.c:238)，而 **GTIMR 是 LP 域自由计数器**。
     *
     *   实测该 LP 域真实频率 ≈ 8017Hz（由 `RTC cal: ms/count=1106` 反推：
     *   RTC 分频固定 8867 = DIVA*DIVB，实测 1106ms/count ⇒ f = 8867*1000/1106 ≈ 8017）。
     *   ⇒ `gtime_freq` 偏大 10.6% ⇒ 每次 DEEP 唤醒补的 tick **少 10.6%**
     *     （例：睡 1 小时少补 ~345 秒）⇒ `rt_tick` 系统性落后真实时间。
     *
     *   为什么以前记的是"偏慢 1.85×"：那是首次校准时把**固定启动延迟**算进去了
     *   （`SEC 3counts -> 5542ms` 含 ~2s 启振延迟）；两点斜率法（N=3 与 N=6 求差）
     *   已把该延迟剔除，稳态值就是 ms/count≈1106 ⇒ 10.6%。旧结论作废。
     *
     * 修法：在**已知的 ms 时间窗**内量 GTIMR 增量，反推真实频率 f_real，
     *   再令 `48000000 / (AVE/200) = f_real` ⇒ `AVE = 48000000*200/f_real`，
     *   写回 `RTC_BACKUP_LPCYCLE_AVE`。此后 SDK 自己算出的 `HAL_LPTIM_GetFreq()`
     *   就是正确值 —— **不 patch SDK、不覆盖符号、复用 SDK 现有的校准写入接口**
     *   (`HAL_RC_CAL_update_ave_cycle()`，lrc_cal.c:71，就是干这个的)。
     *
     *   ⚠️ 参考时基用 `HAL_GetTick()`：它走 SysTick，而本板 `BSP_PM_FREQ_SCALING`
     *      从未定义 ⇒ drv_common.c:155 用 **HCLK** 作 SysTick 源 ⇒ ms 是准的，
     *      与 RC 低频域无关，正好适合当标尺。
     *
     *   ⚠️ 本步只在唤醒路径且**已过步骤 2.4~2.8**(BLE 栈已重建)之后执行；
     *      测量本身持 IDLE 锁防止期间误入 DEEP（否则 GTIMR 照走但 SysTick 停，
     *      标尺失效）。测完即释放。
     *
     *   自校准策略：**每次 BOOT 只测一次**（RC 频率不随时间快变，备份域会保存结果），
     *   后续唤醒直接复用 —— 避免每次唤醒都白等 500ms 拖慢响应（按键唤醒后要立刻能用）。
     *   实测值若离谱（<4000 或 >30000）判定为测量异常，丢弃本次结果。 */
    {
        static uint8_t s_gtcal_done = 0;   /* 每 BOOT 一次；备份域已持久化 */

        if (!s_gtcal_done)
        {
            s_gtcal_done = 1;

            rt_pm_request(PM_SLEEP_MODE_IDLE);

            {
                uint32_t t0     = HAL_GetTick();
                uint32_t gt0    = HAL_GTIMER_READ();
                uint32_t gt1, t1, dms, dgt;

                /* 累计到至少 ~500ms 的窗口（SysTick ms 计时，精度足够） */
                do
                {
                    rt_thread_mdelay(10);
                    t1 = HAL_GetTick();
                } while ((uint32_t)(t1 - t0) < 500u);

                gt1 = HAL_GTIMER_READ();
                dms = (uint32_t)(t1 - t0);
                dgt = (uint32_t)(gt1 - gt0);

                if (dms >= 400u && dgt >= 800u)
                {
                    /* f_real ≈ dgt * 1000 / dms  (Hz，整数运算避免 %f) */
                    uint32_t f_real = (uint32_t)((uint64_t)dgt * 1000u / dms);
                    uint8_t  lpc    = HAL_RC_CAL_GetLPCycle();

                    if (lpc == 0)
                        lpc = 200;

                    if (f_real >= 4000u && f_real <= 30000u)
                    {
                        /* AVE_correct = 48000000 * lpc / f_real，用 u64 防溢出 */
                        uint32_t ave_new = (uint32_t)((uint64_t)48000000u * lpc / f_real);

                        /* 读回旧值（低 32 位）作对比；未知时按 0 处理 */
                        uint32_t ave_old = HAL_Get_backup(RTC_BACKUP_LPCYCLE_AVE);

                        if (ave_old == 0)
                            ave_old = 1082788u;    /* 出厂/兜底名义值(=8866Hz) */

                        /* 相对偏差（万分比），仅在 >1% 时写回，避免无谓改写备份域。
                         * ⚠️ 必须用 u64 做中间量：AVE≈1.08e6，×10000 = 1.08e10 已超
                         *    u32 上限(4.29e9)，用 u32 会静默溢出使偏差判断失效。 */
                        uint32_t diff = (uint32_t)((uint64_t)((ave_new > ave_old) ? ave_old : ave_new)
                                                   * 10000u
                                                   / ((ave_new > ave_old) ? ave_new : ave_old));
                        int need_write = (diff < 9900u);

                        rt_kprintf("[POWER] GT cal: dms=%u dgt=%u f_real=%u Hz (was 8866), "
                                   "AVE %u -> %u (lpc=%u, dev=%u.%02u%%) %s\n",
                                   (unsigned)dms, (unsigned)dgt, (unsigned)f_real,
                                   (unsigned)ave_old, (unsigned)ave_new, (unsigned)lpc,
                                   (unsigned)((10000u - diff) / 100u),
                                   (unsigned)((10000u - diff) % 100u),
                                   need_write ? "-> writeback" : "-> keep");

                        if (need_write)
                        {
                            /* 用 SDK 官方写入接口（同时更新 g_ave_cycle 与备份域） */
                            HAL_RC_CAL_update_ave_cycle(ave_new);
                            HAL_Set_backup(RTC_BACKUP_LPCYCLE_AVE, ave_new);
                            rt_kprintf("[POWER] GT cal: HAL_LPTIM_GetFreq now = %u Hz\n",
                                       (unsigned)HAL_LPTIM_GetFreq());
                        }
                    }
                    else
                    {
                        rt_kprintf("[POWER] GT cal: f_real=%u out of range, discarded\n",
                                   (unsigned)f_real);
                    }
                }
                else
                {
                    rt_kprintf("[POWER] GT cal: window too short (dms=%u dgt=%u), skipped\n",
                               (unsigned)dms, (unsigned)dgt);
                }
            }

            rt_pm_release(PM_SLEEP_MODE_IDLE);
        }
    }

    /* 3. 重新开启 BLE 广播（待机时 app_ble_stop_advertising 已停） */
    ble_app_wakeup_advertise();

    /* 4. 重新钉住活跃态基础 IDLE 锁（对应 power_init 开头那把，待机时被 release 掉了）。
     *    无论是否插 USB 都要拿回：否则唤醒后活跃态无锁 + policy 虽已切回 IDLE-only，
     *    仍会让 idle 线程反复进 WFI-IDLE（可接受）；但若后续任何路径再切 DEEP policy
     *    就会重演"无锁裸睡"。统一由这把锁兜底，只在待机分支显式释放。
     *    注：usb_start() 内部另有自己的 IDLE 锁（usb_pm_held 保护，不会重复）。 */
    rt_pm_request(PM_SLEEP_MODE_IDLE);

    /* 5. 恢复电池监控活跃节拍 + 蓝牙指示 */
    g_bat_mon_sleep = 0;
    power_bt_indicator_active();

    /* 6.5 重建 WS2812B 输出通路（DEEP 断 HPSYS 域后全部丢失, 08-30 实测修复）：
     *  - GPTIM2 时钟: HAL_GPT_Base_MspInit(bf0_hal_tim.c:111) 在开机初始化时
     *    HAL_RCC_EnableModule(RCC_MOD_GPTIM2) 只开一次, DEEP 后 RCC ENR1 复位
     *    默认关 → 必须重开, 否则 PWM/DMA 写寄存器全部无效。
     *  - PA00 复用: bsp_pinmux 开机配成 GPTIM2_CH1 仅一次; BSP_PowerDownCustom
     *    进深睡前把 PA00 拉成 GPIO 输出低, 唤醒后 BSP_PowerUpCustom 为空实现
     *    (其注释假设"ws2812b 每次使用时自重配"——但 drv_rgbled_send_data 只写
     *    TIM 寄存器、从不重配引脚, 该假设不成立) → 唤醒后 PA00 停留在 GPIO 低,
     *    PWM 信号到不了灯。此处必须恢复 GPTIM2_CH1 复用。
     *  两者都幂等, 重复调用无害。必须在 power_ldo3_domain_on(299-300 把 PA00
     *  设回 GPIO 低)之后、首次 rgb_led_show 之前执行。 */
    HAL_RCC_EnableModule(RCC_MOD_GPTIM2);
    HAL_PIN_Set(PAD_PA00, GPTIM2_CH1, PIN_NOPULL, 1);
    rt_kprintf("[POWER] rgb path rebuilt (PA00->GPTIM2_CH1, GPTIM2 clk on)\n");

    /* 6. 深睡唤醒后强制重刷槽位 RGB 指示(08-30) —— 唤醒早期按键事件线程可能抢在
     *    本函数 LDO3 上电之前就调了 bt_multi_start_indication(WS2812B 无供电,
     *    写了不亮), 且其 g_blink_slot 已被置位, 后续调用被 if(g_blink_slot) 挡掉 →
     *    "休眠唤醒后蓝牙未连接时槽位 RGB 不闪"。这里在 LDO3/模拟域确认恢复后
     *    显式 stop+start 一次, 保证蓝/黄闪(等待回连/配对)真正可见。
     *    已连接场景 bt_multi_start_indication 内部会直接停闪, 无副作用。 */
    extern void bt_multi_led_stop_all(void);
    extern void bt_multi_start_indication(void);
    extern void bt_multi_suppress_indication(uint8_t on);
    bt_multi_suppress_indication(0);   /* 退出待机: 解除抑制, 恢复槽位指示 */
    bt_multi_led_stop_all();
    bt_multi_start_indication();

    /* 7. ★★★ 09-18(4) 新增：唤醒后同步"低功耗时钟档位"，与 power_init() 保持一致。
     *
     * === 为什么必须在这里补（这是 1.3mA / 16.3mA 的机制性根因）===
     * 此前**只有 `power_init()`（开机一次性）**做过这两句（见 power_init 尾部注释
     * "清 FORCE_HP(仅音频场景用)"）。`power_resume_from_deep()` 从来不做 ⇒ 一旦
     * 音频场景把 `DBGR.FORCE_HP` 置上而没有被 `audio_client_stop` 的 BT_VOICE 分支清掉
     * （语音非正常收尾，如 SCO 随 ACL 掉线一起消失），**这个位就会一直留着**：
     *   · 它强制开 HP 时钟 ⇒ HCPU 即便真进 `[pm]S:3`(DEEP) 也仍被供给时钟 ⇒ 恒定 mA；
     *   · 它还让 HCPU 停在 48MHz（`pm_scenario_start` 会顺手降频到 48M）。
     * 新增的两处"进 DEEP 前清"（power_enter_standby / standby_rtc_battery_check）
     * 是**兜底**——真正的对称修复是这里：唤醒时把状态复位到与开机一致，
     * 这样无论后面哪条待机路径进入都能从干净状态出发。两处叠加，互为保险。
     * ⚠️ `SetDeepWFIDiv(60,0,1)`（=4MHz）与 `power_init()` 一致；USB 在线时
     *    `main.c::usb_start()` 会再改回 div 12，不冲突。 */
    HAL_RCC_HCPU_SetDeepWFIDiv(60, 0, 1);
    hwp_hpsys_rcc->DBGR &= ~HPSYS_RCC_DBGR_FORCE_HP;
}

/* ⚠️ 52x 唤醒范式（官方 low_power.md "SF32LB52X" 分支原话）：
 *   "52系列deepsleep模式下的休眠不需要额外配置唤醒pin，所有pin都可以唤醒，唤醒源走WSR_GPIO1"
 * 即 DEEP 下按键唤醒直接走 GPIO1 中断唤醒源（默认 WER 已含 GPIO1 位）。
 *
 * SF32LB52X 的 AON per-pin 硬件实为【连续 21 通道】PIN0~PIN20 = PA24~PA44
 * （HAL_HPAON_QueryWakeupPin 直接返回 pin-24；WER bit8~28 全部有效，CR1 管 PIN0-7、
 * CR2 管 PIN8-15、CR3 管 PIN16-20）。故 PA28-33（EC/L1/C3 等）同样有 per-pin 通道，
 * 早前"PA28-33 无 AON 通道、必须靠 GPIO1 bank"的结论是误读（2026-08-05 对照
 * example/pm/classical + hpsys_aon.h 核实）。按键唤醒仍双保险：per-pin + GPIO1 bank。
 *
 * 历史教训（2026-07-30 定位）：早前对 7 键调 pm_enable_pin_wakeup(slot, AON_PIN_MODE_LOW)，
 * 电平模式在按键按下期间持续锁存 WSR PIN 位（6 键位清后 100ms 内必回挂）→ sifli_suspend
 * 见 WSR&WER≠0 永远 EBUSY → DEEP 被降级 IDLE，[pm]S:3 永不出现。
 * 根因是【电平模式锁存】而非通道缺失——现 key_enable_deep_wakeup 已改 NEG_EDGE 边沿模式规避。
 *
 * 且 AON 域寄存器（WER/CR1~CR3）跨软复位保留：旧固件写入的使能位换新固件也仍在。
 * 故此处必须【主动禁用】全部 AON PIN 唤醒（含 PIN17-20 遗留位）并清残留状态。 */
static void configure_aon_wakeup_pins(void)
{
    int slot;
    for (slot = 0; slot <= 20; slot++)          /* AON PIN0-20 (PA24-PA44) 全部禁用 */
        pm_disable_pin_wakeup((uint8_t)slot);
    HAL_HPAON_CLEAR_WSR(HAL_HPAON_GET_WSR());   /* 清掉遗留的 PIN 锁存位 */
}

/* DEEP 唤醒后由框架 sifli_deep_handler → BSP_Power_Up → BSP_PowerUpCustom 自动调用。
 * 52x DEEP 下按键经 GPIO1 唤醒源唤醒（官方 low_power.md：所有 pin 走 WSR_GPIO1）。
 *
 * ⚠️ 必须按唤醒源过滤，不能无条件置唤醒标志！
 * 待机期间系统会因【非按键】原因多次进出 DEEP：
 *   - quiesce 轮询的 rt_thread_mdelay(100)、待机看门狗的 30s sem 超时 → 均由 LPTIM1(WSR bit2) 唤醒；
 *   - 这些唤醒同样会走到本函数。若无条件 power_standby_wakeup()，待机会被自己的定时器立刻打断
 *     （实测现象：blocking 打印后 41ms 就 resume，用户没碰任何按键）。
 * 故需按 WSR 唤醒源过滤（保守策略见下方判定处：仅纯 LPTIM1/RTC 判为伪唤醒）。
 * WSR 读取时序安全：SDK 在 sifli_deep_handler 中 BSP_Power_Up(bf0_pm.c:1137) 之前只清了
 * POWER_MODE，未动 WSR；真正的 CLEAR_WSR 在其后的 sifli_resume/AON_IRQHandler(2699) 才执行。 */
void BSP_PowerUpCustom(bool is_deep_sleep)
{
    uint32_t wsr;

    (void)is_deep_sleep;

    wsr = HAL_HPAON_GET_WSR();
    g_last_deep_wsr = wsr;
    g_deep_wake_cnt++;

    /* 无论何种唤醒源都要恢复 GPIO1 中断（DEEP 断 HPSYS 后 NVIC 使能会丢） */
    HAL_NVIC_SetPriority(GPIO1_IRQn, 5, 0);
    HAL_NVIC_EnableIRQ(GPIO1_IRQn);

    /* 只清非 PIN 位，对齐 SDK sifli_resume 行为（PIN 位留给 GPIO handler 分发） */
    HAL_HPAON_CLEAR_WSR(wsr & ~HPSYS_AON_WSR_PIN_ALL);

    /* 判定策略：只有【纯内部定时器源】(LPTIM1/RTC) 才算伪唤醒；含 GPIO1、或读到 0/未知源，
     * 一律按真唤醒处理 —— 宁可偶尔多醒一次，也不能因为过滤过严导致按键永远唤不醒。 */
    if (g_standby_active && (wsr & HPSYS_AON_WSR_RTC) &&
        !(wsr & (uint32_t)(HPSYS_AON_WSR_GPIO1 | HPSYS_AON_WSR_PIN_ALL)))
    {
        /* 09-09 方案 B：待机 RTC 电量检查唤醒（RTC 位必须【独占】——若同时有
         * 按键位则落到底部真唤醒分支，全量恢复 + bat_mon 接管监控，不漏检查）。
         * 置标志 + 放信号量让 sleep_thread 走 standby_rtc_battery_check 轻量路径
         * （不置 g_wakeup_requested，不触发 power_resume_from_deep 全量重建，
         * 查完继续睡）。 */
        g_standby_rtc_wake = 1;
        if (g_standby_sem) rt_sem_release(g_standby_sem);
    }
    else if (wsr != 0 && (wsr & ~(uint32_t)(HPSYS_AON_WSR_LPTIM1 | HPSYS_AON_WSR_RTC)) == 0)
    {
        g_deep_spur_cnt++;        /* 伪唤醒：不置标志，sleep_thread 继续阻塞，框架自动再入 DEEP */
    }
    else
    {
        power_standby_wakeup();   /* 真唤醒：置 g_wakeup_requested + release 信号量 */
    }
}

/* 红灯(BSP_LED4_PIN, 高电平点亮)开关 */
static void led4_set(int on)
{
    led_status(BSP_LED4_PIN, on ? BSP_LED4_ACTIVE : !BSP_LED4_ACTIVE);
}

/* 极低电量关机：进 Hibernate。不返回。
 * 09-09: 不再用 L2 键作开机唤醒 —— 电源是拨动开关, 关机后用户拨开关
 * 断电→上电即冷启动开机(hibernate 状态随断电自然清掉)。
 * ★09-18 更正: 原文写"保留 RTC 唤醒使能兜底(项目未设 RTC 闹钟, 无副作用)" ——
 *   该前提早已不成立（待机检查会武装 WUT），**必须改为关掉 RTC 唤醒源**，
 *   否则关机后被 RTC 闹钟唤醒→开机→再关机，无限重启。详见函数体内注释。
 * ★09-16 修: 进 hibernate 前必须"熄灯 + 断 LDO3 外设域"（见下方注释）。 */
void pm_shutdown(void)
{
    rt_kprintf("[POWER] shutdown: enter hibernate (boot back via slide switch)\n");

    /* ★★★ 09-18 修复：整个关机流程必须"钉住 PM、不许框架睡下去"。
     *
     * === 实测故障（待机路径，用户日志逐行实证）===
     *     [POWER] battery critically low in standby (17%, 37822mV), shutting down
     *     [POWER] shutdown: enter hibernate (boot back via slide switch)
     *     [pm]S:3,1049012          ← ★ 3s 红指示延时还没跑完就进了 DEEP
     *     （且**没有** `[POWER] LDO3 domain OFF` —— 证明延时被打断，后面的
     *      关灯/断 LDO3/EnterHibernate 全都没执行）
     *     之后一直卡在 DEEP，**要等用户按键**(`[pm]W:2850420  [pm]WSR:0x202`
     *     = GPIO1+PIN1) 唤醒，代码才从 HAL_Delay_us 返回并继续跑到真 hibernate。
     *     用户感受："没有真关机，需要我按下按键才关机"。
     *
     * === 根因：PM 锁状态在两条路径上不同 ===
     *   · 待机路径 `standby_rtc_battery_check()` 由 sleep_thread 调用，而进待机前
     *     sleep_thread 已 `rt_pm_release` **全部 150 个 PM 锁**（就是为了让框架能进
     *     DEEP）；本函数里的 `HAL_Delay_us(3000000)` 是**忙等且中断未关** ⇒
     *     SysTick → idle 线程 → PM 选 DEEP ⇒ 关机流程被打断。
     *   · 活跃态路径（`battery_monitor`）不出问题，是因为那时 `power_init()` 的
     *     IDLE 锁仍被持有（生命周期：power_init → sleep_thread 待机分支才释放）
     *     ⇒ 3s 延期内框架进不去 DEEP。这解释了"同一函数两条路径表现不同"。
     *
     * === 修法 ===
     *   进函数立刻 request IDLE 锁（与 `standby_rtc_arm_timer()` 同款手段），
     *   把框架钉在 IDLE，保证 3s 延时 + 关灯 + 断域 + hibernate 一气呵成。
     *   ⚠️ 成功路径本函数不返回，无需 release；异常也只会停在下方 while(1)。
     *   ⚠️ 这是**必须**的，不是保险 —— 去掉它待机路径就又会被拽进 DEEP。 */
    rt_pm_request(PM_SLEEP_MODE_IDLE);

#if defined(SF32LB52X) || defined(SOC_SF32LB52X)
    /* ★★★ 09-18 关键修复：进 hibernate 前必须"停 RTC 闹钟 + 关 RTC 唤醒源"，
     * 否则陷入【关机 → RTC 闹钟到点把芯片唤醒 → 重启开机 → 又关机】的死循环。
     *
     * === 原代码的错误前提（用户实测翻案）===
     * 原为 `HAL_PMU_EnableRtcWakeup();` 且注释"项目未设 RTC 闹钟, 无副作用"。
     * **该前提现已不成立** —— 待机低电量检查 `standby_rtc_arm_timer()` 会武装
     * 60s/1h 的 WUT 并调用 `HAL_PMU_EnableRtcWakeup()`；而 **RTC 属于常供电域,
     * WUT 跨 hibernate 存活**。完整链条（全部有代码/日志实证）：
     *   ① pm_shutdown 调 `HAL_PMU_EnableRtcWakeup()` ⇒ PMUC.WER 的 RTC 位置 1
     *      （把 RTC 显式设成 hibernate 的唤醒源）
     *   ② `HAL_PMU_EnterHibernate()` ⇒ 芯片掉电
     *   ③ 残留 WUT 到点 ⇒ PMU 唤醒芯片 ⇒ **整机上电重启**
     *      （日志: `boot mode=2` = PM_HIBERNATE_BOOT, 且 `Reason:00000001`）
     *   ④ 开机时 `HAL_RTC_Init(hrtc, RTC_INIT_SKIP)` **只在 PM_COLD_BOOT 才清
     *      WUT/Alarm**（bf0_hal_rtc.c:83-88）；HIBERNATE_BOOT 分支**不清** ⇒
     *      WUT 依然武装 ⇒ 回到 ①，无限循环。
     *   用户实测现象：`battery critically low (17%) → shutting down` 之后不断
     *   SFBL 重启（每轮 = 一次完整开机 ~31s + hibernate 等待）。
     *
     * === 修法 ===
     *   ① 停 WUT（等价于 standby_rtc_arm_timer 的逆操作）+ 清 WUTF 残留标志；
     *   ② 关 PMU 侧 RTC 唤醒使能（HAL_PMU_DisableRtcWakeup，与 Enable 对称）；
     *   ③ 顺手关 HPAON 侧 RTC 唤醒源（DEEP 用，hibernate 后无意义，一并清干净）。
     *   此后 hibernate 只能靠"拨动开关断电→上电"恢复，符合产品定义。 */
    (void)HAL_RTC_DeactivateWakeUpTimer(&RTC_Handler);
    __HAL_RTC_WAKEUPTIMER_CLEAR_FLAG(&RTC_Handler, RTC_ISR_WUTF);
    (void)HAL_PMU_DisableRtcWakeup();
    HAL_HPAON_DisableWakeupSrc(HPAON_WAKEUP_SRC_RTC);

    /* ★09-18 分步面包屑（定位"`shutdown: enter hibernate` 之后全无日志"）。
     * 每步一条打印 —— **最后出现的那条**就说明卡在它后面那一句。
     * ★09-18 收尾：整组收进 `POWER_DEBUG_DIAG`（默认 0 = 不编译）。排查时置 1。 */
#if POWER_DEBUG_DIAG
    rt_kprintf("[POWER] shutdown step1: RTC src cleared, PMR=0x%08x(mode=%lu)\n",
               (unsigned)hwp_hpsys_aon->PMR, (unsigned long)(hwp_hpsys_aon->PMR & 0x3u));
#endif

    /* 先让"低电关机"红指示可见 ~3s（原逻辑保留：调用方刚点亮 LED4） */
    HAL_Delay_us(3000000);

    /* ★ 面包屑 step2：**这条不出现 ⇒ 卡在这 3 秒忙等里**。
     * 怀疑点：待机路径下 PM 锁已全释放，`rt_pm_request(IDLE)` 可能没兜住 ⇒ 框架把系统
     * 拽进 DEEP；而本函数**刚把 RTC 唤醒源关掉** ⇒ 误入的 DEEP 无任何唤醒源。
     * 同时打 PMR（mode：0=run/2=DEEP）以佐证当时是否真进了 DEEP。 */
#if POWER_DEBUG_DIAG
    rt_kprintf("[POWER] shutdown step2: 3s delay done, PMR=0x%08x(mode=%lu)\n",
               (unsigned)hwp_hpsys_aon->PMR, (unsigned long)(hwp_hpsys_aon->PMR & 0x3u));
#endif

    /* ★09-16 用户实测修复: 关机后 **RGB 常亮 + 静态电流 ~0.6mA 永不消失**。
     *
     * 根因: 本函数此前只关了 VDD33(LDO2)/LDO1V8, **从未关 LDO3 外设域** —— 而 RGB
     * (SK6812MINI-HS, rtconfig: RGB_USING_SK6812MINI_HS) 正挂在该域上。
     *   - `power_ldo3_domain_off()` 的注释早就写明: "WS2812 静态电流（实测待机 630µA
     *     的主要来源）" —— 与用户实测 0.6mA 完全吻合；
     *   - SK6812/WS2812 是**锁存型**智能灯: 一旦被点亮(本机 RGB 兼作按键底光 + 槽位指示,
     *     `key_config_apply_rgb`/`bt_multi_start_indication`), 只要 VDD 还在就永远保持
     *     最后一个颜色 —— MCU 都停了它还亮着 ⇒ 关机后常亮 + 持续 ~0.6mA 耗电。
     *
     * 修法（顺序即可，两步都幂等）:
     *   ① 显式把 RGB 整链置黑 + 熄掉红 LED4（关机指示已亮了 3s，不必再亮）；
     *   ② 整域断 LDO3（内部三步法已处理引脚反灌/浮空，顺序不可颠倒，勿自行简化）。
     * 注: 本文件的 `led_all_off()` 是**空实现(函数体全被注释掉)**，不能再指望它；
     *     其余 3 颗 GPIO LED 由随后关掉的 VDD33 域一并熄灭(用户实测: 只有 RGB 卡住,
     *     说明 VDD33 断电确实管用)。放在关中断之前: 只用 pin/PMU/PWM 寄存器操作。 */
    {
        extern void rgb_led_set_color(uint32_t color);   /* ws2812b.c */
        extern struct rt_device *rgbled_device;          /* ws2812b.c 全局(rt_device_find 结果) */
        if (rgbled_device)                               /* 防"开机极低电, rgb_led_init 还没跑" */
            rgb_led_set_color(0x000000u);                /* 整链黑色 */
    }
    /* ★ 面包屑 step3：不出现 ⇒ 卡在 `rgb_led_set_color()`。
     * 该函数是 09-16 为修"关机后 RGB 常亮"加的，而**待机态关机这条路径是最近才走通的**
     * —— 此时 LDO3 域与 PA00/GPTIM2 早已断电，调 WS2812 驱动属于**从未验证过的组合**。 */
#if POWER_DEBUG_DIAG
    rt_kprintf("[POWER] shutdown step3: rgb cleared\n");
#endif

    led4_set(0);

    /* ★★★ 09-18 关键修复（**43.2mA 的根因**）：`power_ldo3_domain_off()` 有幂等守卫
     * `if (g_ldo3_off) return;`，而 SDK 的 `BSP_Power_Up()` 会**绕过该标志**直接把 LDO3
     * 硬件重开（只看 `BSP_ImuPowerWanted()`）；此时 `g_ldo3_off` 仍停在 1 ⇒ 本句会被
     * **静默跳过、什么都不做** ⇒ 进 hibernate 时 LDO3 域（IMU/ENC 编码器/I2C 上拉/RGB）
     * 全部带电。
     *
     * === 实测证据（用户日志，两次不同场景）===
     *   · 正常关机：`probe: ... PERI_LDO=0x002c2d19`（LDO3 PD=1 已断电）⇒ 电流正常；
     *   · **异常关机**：`rtc-woke-light LDO3 HW: EN=1 PD=0 => *** POWERED ON ***`
     *     （`imu_want=1`，因为**本次没连过蓝牙**，`air_mouse_stop()` 没被调用 ⇒
     *      SDK 唤醒即重开 LDO3），随后 `probe: ... PERI_LDO=0x000d2d19`（LDO3 仍开）
     *     ⇒ **hibernate 后实测 43.2mA**。
     *   注：待机路径里我也加过同类纠正，但**低电关机在它之前就 `pm_shutdown()` 了、
     *   根本走不到** ⇒ 必须修在这里，才能覆盖所有关机入口。
     *
     * === 修法 ===
     *   以**寄存器位**为准（bit16=`EN_VDD33_LDO3`、bit21=`VDD33_LDO3_PD`）：
     *   硬件确实带电而标志说"已关"⇒ 先清标志破除幂等，再真正关断
     *   （含去上拉 + 驱动低，防反灌；见 `power_ldo3_domain_off()` 内部三步法）。 */
    {
        int ldo3_hw_on = !!(hwp_pmuc->PERI_LDO & PMUC_PERI_LDO_EN_VDD33_LDO3)
                      && !(hwp_pmuc->PERI_LDO & PMUC_PERI_LDO_VDD33_LDO3_PD);
        if (ldo3_hw_on && power_ldo3_is_off())
        {
            rt_kprintf("[POWER] shutdown: LDO3 flag/hardware mismatch "
                       "(flag=off, hw=on) -> clearing flag then forcing OFF\n");
            g_ldo3_off = 0;
        }
    }
    power_ldo3_domain_off();

    /* ★09-18 补：同时断模拟域（充电器 FORCE_RST + codec REFGEN + GPADC）。
     * 原函数只关了 VDD33_LDO2 / LDO_1V8，**从未断 analog 域**：
     *   · 待机路径关机时该域已由 power_enter_standby() 关过 ⇒ 无影响（幂等直接 return）；
     *   · **活跃态路径关机时（如开机即低电直接关机）该域仍是开的** ⇒ 进 hibernate 前
     *     残留偏置电流。DEEP 下该域 OFF 可省 ~0.4mA（见本文件第十三根因注释），
     *     hibernate 前没有理由留着。
     * 纯寄存器写、无延时、幂等 ⇒ 放在关中断之前安全。 */
    power_analog_domain_off();
    /* ★ 面包屑 step4：不出现 ⇒ 卡在 `led4_set()` / `power_ldo3_domain_off()` /
     * `power_analog_domain_off()` 三者之一（三者都是纯寄存器写，正常不会卡）。 */
#if POWER_DEBUG_DIAG
    rt_kprintf("[POWER] shutdown step4: led4/ldo3/analog off\n");
#endif

    /* ★★★ 09-18 重要教训：**所有诊断打印必须放在 `rt_hw_interrupt_disable()` 之前**。
     * 面包屑实测（用户日志）：关中断后**只有一条短打印能出去**（`step5`，旧固件的
     * `hibernate entering now` 同理）；一旦再打印一条**长**行（本探针原第一条 ~130 字符），
     * console 的 TX 就推不动了 ⇒ `rt_kprintf` 自己卡死 ⇒ **诊断反而制造了新的卡死点、
     * 把真正的故障完全挡住**（症状：探针一行都没出现）。
     * ⇒ 本段整体前移；关中断后**只保留唯一一条已验证可出的短打印**。
     *
     * === 探针要回答的问题 ===
     * `HAL_PMU_EnterHibernate()`（bf0_hal_pmu.c:213）在置 `PMUC_CR_HIBER_EN` 之前，
     * 唯一的**无超时死等**是第 2 步 `HAL_HPAON_WakeCore(CORE_ID_LCPU)` 内部
     * （bf0_hal_hpaon.c:162/165）：`while (!(ISSR & HPSYS_AON_ISSR_LP_ACTIVE));`
     * 若 LCPU 不应答就永久自旋 ⇒ 芯片永不断电 ⇒ 恒定 mA 级（与实测 16.5mA 吻合）。
     * 这里用**同款手法但加 20ms 超时**：
     *   · 能醒 → SDK 随后必然立刻通过 ⇒ 不影响结果（不掩盖问题）；
     *   · 唤不醒 → 报 TIMEOUT，而 SDK 那处无超时必然死等 ⇒ 就是根因。
     * 定位完成后本段可整块删除。
     *
     * ★09-18 收尾：整块收进 `POWER_DEBUG_DIAG`（默认 0 = 不编译）。
     *   ⚠️ 关掉是**必须**的，不只是省日志 —— 本块含 `ISSR |= HP2LP_REQ` 的真实写操作，
     *   它会让 LCPU 被唤醒一次；量产流程里不应存在这种"诊断副作用"。 */
#if POWER_DEBUG_DIAG
    {
        uint32_t issr0 = hwp_hpsys_aon->ISSR;
        int lp_woke = 0;
        int k;

        hwp_hpsys_aon->ISSR |= HPSYS_AON_ISSR_HP2LP_REQ;   /* 与 HAL_HPAON_WakeCore 第一步一致 */
        for (k = 0; k < 200; k++)                          /* 200 × 100µs = 20ms 上限 */
        {
            if (hwp_hpsys_aon->ISSR & HPSYS_AON_ISSR_LP_ACTIVE)
            {
                lp_woke = 1;
                break;
            }
            HAL_Delay_us(100);
        }
        /* 只打一行（关中断前，长一点也安全）：CR/WSR/ISSR 前后值 + PERI_LDO + 唤醒结果 */
        rt_kprintf("[POWER] probe: CR=0x%08x(HIBER_EN=%d) WSR=0x%08x PERI_LDO=0x%08x | "
                   "ISSR 0x%08x->0x%08x LP_ACT=%d->%d | LCPU wake=%s(%dms)\n",
                   (unsigned)hwp_pmuc->CR, !!(hwp_pmuc->CR & PMUC_CR_HIBER_EN),
                   (unsigned)hwp_pmuc->WSR, (unsigned)hwp_pmuc->PERI_LDO,
                   (unsigned)issr0, (unsigned)hwp_hpsys_aon->ISSR,
                   !!(issr0 & HPSYS_AON_ISSR_LP_ACTIVE),
                   !!(hwp_hpsys_aon->ISSR & HPSYS_AON_ISSR_LP_ACTIVE),
                   lp_woke ? "OK" : "*** TIMEOUT => WakeCore 必死等 => 不断电 ***", k / 10);
    }
#endif

    /* ⚠️ 09-18 已删除"探针2/探针3"（曾用来排查 `HAL_PMU_EnterHibernate()` 是否卡死）。
     * 两者都返回 OK、把"死等"假设全部证伪，但 probe2 有**副作用**必须注意：
     * `HAL_RCC_Reset_and_Halt_LCPU(1)` 会置 `PMR.CPUWAIT`，使**随后 SDK 内部那道
     * LCPU 复位/停机被守卫跳过** ⇒ LCPU 反被留在运行态。**切勿再加回来。**
     */

    /* ★ 面包屑 step5：本行是关中断前**最后一条**打印。
     * 若它是最后出现的日志，说明卡在 `rt_hw_interrupt_disable()` 或其后两句 LDO 配置。
     *
     * ★09-18 收尾：关闭（`POWER_DEBUG_DIAG=0`）时**保留**后续那条短打印
     *   `shutdown: calling HAL_PMU_EnterShutdown() (RC10K path) ...` 作为关机标志行
     *   —— 它是关中断前唯一一条已验证可出的短行，普通使用中也想让用户看到"正在断电"。 */
#if POWER_DEBUG_DIAG
    rt_kprintf("[POWER] shutdown step5: prep done, irq disable + LDO2/1V8 off next\n");
#endif

    rt_hw_interrupt_disable();

    /* 关外设 LDO 省电 */
    HAL_PMU_ConfigPeriLdo(PMUC_PERI_LDO_EN_VDD33_LDO2_Pos, false, false);
    HAL_PMU_ConfigPeriLdo(PMU_PERI_LDO_1V8, false, false);

    /* ★09-18 诊断：本行出现即证明整个关机流程**未被 PM 拽进 DEEP**、一气呵成。
     * 判读：若 "shutdown: enter hibernate" 之后**没有**本行（而是直接 [pm]S:3），
     *       说明 3s 延期内框架又睡了 —— 即 rt_pm_request(IDLE) 没生效或被提前释放。
     * 定位完成后本行可删。 */
    rt_kprintf("[POWER] shutdown: calling HAL_PMU_EnterShutdown() (RC10K path) ...\n");

    /* ============================================================================
     * ★★★★ 09-18:改用 `HAL_PMU_EnterShutdown()`（**不是** `HAL_PMU_EnterHibernate()`）
     *
     * === 为什么换（官方文档 + SDK 源码对照）===
     * 官方低功耗文档写明两种系统级关机：
     *   · **Hibernate**：所有子系统掉电，**系统切到 32K 晶体**，可被 PIN/RTC 唤醒（RTC 准时）
     *     接口 `HAL_PMU_EnterHibernate()`
     *   · **Shutdown**：所有子系统掉电，**系统切到 RC10K**，可被 PIN/RTC 唤醒（RTC 不准时）
     *     接口 `HAL_PMU_EnterShutdown()`
     * 而 **本板 `LXT_DISABLE`（没有 32.768kHz 晶振）** ⇒ hibernate 要切的那条"32K 晶体"路径
     * 在本板并不存在 ⇒ 表现为**时好时坏**（用户实测：同一固件第一次能关机、第二次 16.9mA）。
     * 源码对照（`bf0_hal_pmu.c:213` vs `:261`）：**Shutdown 比 Hibernate 多出三步**——
     *     HAL_PMU_LpCLockSelect(PMU_LPCLK_RC10);   // 低功耗时钟切到 RC10K
     *     HAL_Delay_us(100);                        // 等切换完成
     *     HAL_PMU_DisableXTAL32();                  // 关掉 32K 晶振
     * 这正是本板需要的；hibernate 完全没有这几步。
     *
     * === 代价（对本产品无影响）===
     * 文档标注 Shutdown 的"RTC 唤醒时间不准确"——但本函数**在关机前已显式禁用 RTC 唤醒**
     * （见上方 `HAL_PMU_DisableRtcWakeup()` / 停 WUT），产品定义就是"只能拨开关恢复"
     * ⇒ 该代价完全不涉及本产品。
     *
     * === 历史（为何不再自己手写尾部）===
     * 曾自己手写尾部（清 WSR → 清 HIBER_EN → 延时 → 置 HIBER_EN）：第一次成功、第二次失败，
     * 与 SDK 版一样"时好时坏" ⇒ 证明问题不在"哪句卡住"，而在 hibernate 那条时钟路径本身
     * （探针1/2/3 已逐一证伪所有"死等"假设：LCPU 能唤醒、Reset_and_Halt 能返回、
     *  HAL_Delay 关中断下可用；SoC 侧 PERI_LDO / WSR / HIBER_EN / DLL / analog 全部干净）。
     * ⚠️ **切勿把 probe2 那道 `HAL_RCC_Reset_and_Halt_LCPU(1)` 加回来**：它会置 `PMR.CPUWAIT`，
     * 使 Shutdown 内部的 LCPU 复位/停机被守卫跳过，反而不干净。
     * ⚠️ 关键前置仍需本函数上方完成：VDD33_LDO2 / LDO_1V8 关闭、LDO3 与 analog 断开、
     * 以及 LDO3 标志/硬件一致性纠正。 */
    HAL_PMU_EnterShutdown();                /* 关机；不返回 */
    while (1) { }                           /* 兜底 */
#endif

    while (1) { }   /* 兜底，正常不应到达 */
}

/* 开机电量显示(08-29): 开机首次读到有效电量后, 按电量点亮对应数量的 GPIO LED
 * 约 2 秒再熄灭, 交还各 LED 正常职责。4 段阶梯电量条(4 颗灯全用上):
 *   <20%   红 (LED4/PA8) —— 只有红灯(与低电量红闪阈值对齐, 09-09: 15→20)
 *   ≥20%  +绿 (LED3/PA7)
 *   ≥50%  +黄 (LED2/PA6)
 *   ≥80%  +蓝 (LED1/PA5) —— 接近满电才亮最高段
 * 例: 76% -> 红绿黄(3 段); 88% -> 红绿黄蓝(4 段)。
 * 蓝灯平时承担充电指示, 显示结束后由 bt_multi_restore_charge_led 还原。
 * 由 bat_mon 线程首读调用; 仅开机一次(深睡唤醒不重复)。 */
static uint8_t g_boot_bat_shown = 0;

static void boot_battery_display(uint8_t pct)
{
    /* 阶梯: 红(有电即亮) → 绿(≥20%) → 黄(≥50%) → 蓝(≥80%) */
    led_status(BSP_LED4_PIN, BSP_LED4_ACTIVE);
    if (pct >= BAT_LOW_BLINK_PCT) led_status(BSP_LED3_PIN, BSP_LED3_ACTIVE);  /* 绿 */
    if (pct >= 50) led_status(BSP_LED2_PIN, BSP_LED2_ACTIVE);                  /* 黄 */
    if (pct >= 80) led_status(BSP_LED1_PIN, BSP_LED1_ACTIVE);                  /* 蓝 */
    rt_kprintf("[POWER] boot battery display %d%% -> %d LED(s)\n", pct,
               pct >= 80 ? 4 : pct >= 50 ? 3 : pct >= BAT_LOW_BLINK_PCT ? 2 : 1);
    rt_thread_mdelay(2000);   /* 点亮 ~2s */
    /* 熄灭红黄绿; 蓝灯(LED1)交还充电指示(充电中应常亮) */
    led_status(BSP_LED4_PIN, !BSP_LED4_ACTIVE);
    led_status(BSP_LED2_PIN, !BSP_LED2_ACTIVE);
    led_status(BSP_LED3_PIN, !BSP_LED3_ACTIVE);
    bt_multi_restore_charge_led();
}

/* 低电量监控线程：<20% 红灯闪烁(2s亮/2s灭, 09-09 放慢)；≤5% 关机（充电中豁免，09-09） */
static void battery_monitor_entry(void *param)
{
    uint8_t low_shown = 0;      /* 当前是否处于低电量闪烁态 */
    uint8_t led_on = 0;
    uint8_t shutdown_cnt = 0;
    uint8_t blink_div = 0;      /* 闪烁分频计数(500ms 节拍 × BAT_LOW_BLINK_DIV) */

    while (1)
    {
        if (g_standby_active)
        {
            /* 自挂起必须紧跟 rt_schedule()（RT-Thread 范式）：suspend 只是摘出就绪队列，
             * 不调度的话本线程会继续执行到下面的 mdelay，把周期定时器重新武装 →
             * 待机期每 500ms 被 LPTIM1 拽出 DEEP 一次（实测第八根因）。 */
            rt_thread_suspend(rt_thread_self());
            rt_schedule();
        }
        /* 深睡待机态下降频到 10s，允许 RT-Thread PM 在两次检测间进深睡；
         * 活跃态维持 500ms 以保证低电量及时响应。两类态下均仍检测低电量，仅节拍不同。*/
        rt_thread_mdelay(g_bat_mon_sleep ? BAT_MON_SLEEP_PERIOD_MS : BAT_MON_PERIOD_MS);

        if (!app_battery_valid())   /* 首读前不动作，防开机初值 0% 误关机/误闪 */
            continue;

        uint8_t pct = app_get_battery_percent();

        /* 开机电量显示(08-29): 首读有效后按颜色点亮 ~2s, 仅开机一次 */
        if (!g_boot_bat_shown)
        {
            g_boot_bat_shown = 1;
            boot_battery_display(pct);
        }

        /* ≤BAT_SHUTDOWN_PCT(10%) 或 电压低于硬地板 -> 连续确认后关机。
         * 09-16: 充电豁免改为【直读硬件 VBUS 位】(rt_charge_get_detect_status), 不再用
         * bt_multi_charge_active() —— 后者是 RAM 标志("充满"状态可能残留不清), 一旦卡住
         * 就会永远豁免关机(用户实测: 电量掉到 3.2V 仍不关机)。硬件位反映"此刻是否插充电器"。
         * 09-09 豁免的初衷仍保留: 深放电插充电器后电压未回升的头几秒, 防边充边误关。 */
        {
            uint8_t  vbus = 0;
            uint32_t mv   = app_get_battery_mv();
            (void)rt_charge_get_detect_status(&vbus);
            /* ★★★ 09-18 临时测试开关：曾用于屏蔽【活跃态】关机判据，以单独验证
             * 【待机态】RTC 低电量关机路径（standby_rtc_battery_check → pm_shutdown）。
             * ✅ 验证已完成，**已复位为 1**（两条路径都恢复正常关机）。
             * 用法：置 0 = 屏蔽活跃态关机（仅测待机路径）；置 1 = 正常。
             * ⚠️ 置 0 时电池耗尽也不会自动关机，会过放到掉电 —— 量产必须为 1。
             * 日志区别：待机路径打 "battery critically low in standby (...)"
             *          活跃路径打 "battery critically low (...)"。 */
#define ACTIVE_SHUTDOWN_EN  1
#if ACTIVE_SHUTDOWN_EN
            int low = (pct <= BAT_SHUTDOWN_PCT) || (mv > 0 && mv <= BAT_SHUTDOWN_MV);
#else
            int low = 0;    /* 屏蔽：活跃态永不触发关机（仅调试用） */
            {
                static uint8_t s_act_off_announced = 0;   /* 只播报一次，避免 500ms 刷屏 */
                if (!s_act_off_announced)
                {
                    s_act_off_announced = 1;
                    rt_kprintf("[POWER] ACTIVE shutdown DISABLED (standby-path test)\n");
                }
            }
#endif
            if (low && !vbus)
            {
                if (++shutdown_cnt >= BAT_SHUTDOWN_CONFIRM)
                {
                    rt_kprintf("[POWER] battery critically low (%d%%, %umV), shutting down\n",
                               pct, (unsigned)mv);
                    led4_set(1);
                    rt_thread_mdelay(300);
                    pm_shutdown();      /* 不返回 */
                }
            }
            else
            {
                shutdown_cnt = 0;
            }
        }

        if (pct < BAT_LOW_BLINK_PCT)   /* <20% 红灯闪烁(09-09 放慢: 分频后 2s 翻转) */
        {
            if (++blink_div >= BAT_LOW_BLINK_DIV)
            {
                blink_div = 0;
                led_on = !led_on;
                led4_set(led_on);
            }
            low_shown = 1;
        }
        else if (low_shown)            /* 从低电量恢复正常，清一次灯 */
        {
            led_on = 0;
            led4_set(0);
            low_shown = 0;
            blink_div = 0;
        }
    }
}

void power_init(void)
{
    /* ⚠️⚠️ 必须是本函数第一件事：立刻钉住 IDLE 锁，禁止框架在开机期误进 DEEP。
     *
     * 根因（2026-07-30 定位）：SDK 的 low_power_init() 是 INIT_COMPONENT_EXPORT，
     * 在 main() 之前就已执行并注册了 main.c 的 pm_policy[]={{2,DEEP}}。而 main()
     * 里 power_init→key_config_init→app_bt_hid_init→...→sleep_thread 启动这一整段
     * 初始化期间，若无任何 rt_pm_request，idle 线程一旦得到调度（tick>=2ms）就会
     * 直接选 DEEP → 开机数百 ms 内即 [pm]S:3。
     *
     * 而此时 key_config_init() 尚未执行（在 power_init 之后），按键 GPIO1 中断还没
     * 配置 → DEEP 下没有任何可用唤醒源 → 表现为"上电就死机、按键无反应"。
     *
     * 这把锁的生命周期：power_init 持有 → 直到 sleep_thread 的待机分支显式释放
     * （见 sleep_thread_entry 中 rt_pm_release 循环）；唤醒后由
     * power_resume_from_deep 重新 request 回来。 */
    rt_pm_request(PM_SLEEP_MODE_IDLE);

    pm_power_on_mode_t boot_mode = SystemPowerOnModeGet();
    rt_kprintf("[POWER] boot mode=%d\n", boot_mode);
    /* 唤醒源诊断：DEEP 从任意 GPIO1 脚(6键/PA44)唤醒时 WSR 应含 bit1(0x2=WSR_GPIO1)。
     * WER=使能掩码, WSR=状态。仅在 BSP_PM_DEBUG 打开时打印，便于验证 DEEP 唤醒来源。 */
#ifdef BSP_PM_DEBUG
    rt_kprintf("[POWER] wake src WSR=0x%08x WER=0x%08x (bit1=GPIO1)\n",
               (unsigned)HAL_HPAON_GET_WSR(), (unsigned)HAL_HPAON_GET_WER());
#endif
    led_init();

    if (boot_mode == PM_HIBERNATE_BOOT || boot_mode == PM_SHUTDOWN_BOOT)
    {
        rt_kprintf("[POWER] woke from deep sleep\n");
        //led_status(BSP_LED1_PIN,BSP_LED1_ACTIVE);
        rt_thread_mdelay(500);

        /* ★09-18 诊断：**是谁把芯片从 hibernate 唤醒的**。
         * pm_get_pwron_wakeup_src() = HAL_PMU_CheckBootMode() 存下的 PMUC.WSR 快照
         * （pmuc.h 位定义：bit0=RTC / bit1=WDT1 / bit2=WDT2 / bit3-4=PIN0-1 /
         *   bit5=IWDT / bit6=PWRKEY / bit7=LOWBAT / bit8=CHG）。
         * 用途：验证"关机后自杀式重启"是否真由 RTC 闹钟引起（bit0=1 即证实）；
         *       若为其他位（如 WDT/IWDT）则说明是看门狗把死循环的 hibernate 打断，
         *       需另找路由。定位完成后本段可删。 */
        {
            extern uint32_t pm_get_pwron_wakeup_src(void);
            uint32_t pwsr = pm_get_pwron_wakeup_src();
            if (pwsr)
            {
                rt_kprintf("[POWER] hibernate wake src PMUC.WSR=0x%08x%s%s%s%s%s\n",
                           (unsigned)pwsr,
                           (pwsr & PMUC_WSR_RTC)    ? " RTC"    : "",
                           (pwsr & PMUC_WSR_PIN_ALL)? " PIN"    : "",
                           (pwsr & PMUC_WSR_WDT1)   ? " WDT1"   : "",
                           (pwsr & PMUC_WSR_IWDT)   ? " IWDT"   : "",
                           (pwsr & PMUC_WSR_LOWBAT) ? " LOWBAT" : "");
            }
        }
    }

    /* ===== 52x 按键唤醒：用 AON per-pin【下降沿】模式（AON_PIN_MODE_NEG_EDGE）=====
     * 官方 low_power.md "DEEP 下所有 pin 都能唤醒" 在 SDK 里的实际落地是：GPIO 边沿→经
     * HAL_HPAON_QueryWakeupPin 映射置 WSR PIN 位→WER 使能→唤醒（bf0_hal_gpio.c）。
     * 故进 DEEP 前由 key_enable_deep_wakeup 逐键使能 AON per-pin NEG_EDGE（下降沿，
     * 按键 active-LOW：静止=HIGH 上拉，按下=LOW；对齐 SDK example/pm/classical 的边沿范式，
     * 其 active-high 键用 POS_EDGE，我们取 NEG_EDGE）；唤醒后 key_disable_deep_wakeup
     * 关掉交还 HPSYS GPIO 中断。
     * 这里 boot 时先 disable 全部 AON PIN0-20 + 清残留锁存（configure_aon_wakeup_pins，
     * 兜底 legacy/旧固件误使能），避免进 DEEP 前已有电平锁存。
     * 注意：绝不能用 AON_PIN_MODE_LOW(电平)——电平模式下按键按下期间会持续锁存 WSR PIN 位
     * → sifli_suspend EBUSY → DEEP 降级 IDLE → 0.2mA 丢。NEG_EDGE 边沿模式不锁存，规避此坑。 */
    configure_aon_wakeup_pins();

    /* ===== GPIO1 bank 唤醒（52x 官方方案：DEEP 下所有 PA 引脚走 WSR_GPIO1 唤醒）=====
     * SF32LB52X 的 AON per-pin 是连续 21 通道（PIN0-20=PA24-44，见 configure_aon_wakeup_pins
     * 上方注释，2026-08-05 对照 hpsys_aon.h 核实，PA28-33 有通道 PIN4-9）；此处 GPIO1 bank
     * 作双保险兜底：任意 PA 脚 GPIO 中断都能经 WSR_GPIO1 唤醒 DEEP（旧版 705a1c7 的 7 键
     * 唤醒即靠本行）。mode 参数对 bank 级源被忽略（EnableWakeupSrc 非 PIN 分支只置 WER bit1）。 */
    HAL_HPAON_EnableWakeupSrc(HPAON_WAKEUP_SRC_GPIO1, AON_PIN_MODE_LOW);

    /* ===== 充电器 VBUS 唤醒源（10-05 方案B 配套，见 power.h::USB_WAKE_BY_PMUC）=====
     * 背景：USB 插入检测的判据已改为充电器 CHG_SR 的 VBUS_RDY 位，PA44 双沿中断被
     * 移除 ⇒ 原本由它承担的"DEEP 中插 USB 唤醒"必须改由充电器承担。
     * 组件（缺一不可）：
     *   ① 驱动 sifli_charge_init() 已调 HAL_PMU_EnableChgWakeup()（PMUC.WER_CHG）—— 事件源；
     *   ② 本行 HPSYS AON 侧的 PMUC 唤醒源路由 —— 把 PMUC 事件接到 AON 唤醒控制器
     *      （SDK 里这行是注释掉的：sifli_charge.c:334）；
     *   ③ main.c 侧保留 CHG_CR4 的 IE_VBUS_RDY（受同一开关控制）。
     * mode 参数对非 PIN 源被忽略（EnableWakeupSrc 非 PIN 分支只置 WER 位）。
     * ⚠️ 该链路**未经真机验证**：若出现"待机电流升高 / 无故唤醒 / 插 USB 仍不唤醒"，
     *    把 power.h 的 USB_WAKE_BY_PMUC 置 0 退回（退回后 DEEP 中插 USB 不唤醒，
     *    但按键/RTC 唤醒后会由 usb_resume_if_plugged() 按 VBUS 补枚举，功能不受影响）。 */
#if USB_WAKE_BY_PMUC
    HAL_HPAON_EnableWakeupSrc(HPAON_WAKEUP_SRC_PMUC, AON_PIN_MODE_DOUBLE_EDGE);
#endif

    /* ===== 按键唤醒统一走 AON per-pin（常驻使能）=====
     * 在 power_init 里就把 6 键+编码器配成 AON per-pin 下降沿唤醒（key_enable_deep_wakeup），
     * 而非只在进 DEEP 前临时使能 —— 这样 LIGHT/IDLE/DEEP 任何睡眠态按键都能经 AON 唤醒，
     * 不依赖 sleep_thread 的调用时序。活跃态按键按下时 GPIO ISR 会清 WSR PIN 位，
     * 不会残留锁存；即便残留，进 DEEP 前 key_enable_deep_wakeup 也会再清一次（幂等）。 */
    key_enable_deep_wakeup();

    /* ===== WFI 期间 HCPU 深度降频（低功耗设计参考 SiFli 文档 4.3） =====
     * 进入 IDLE(WFI) 但不满足睡眠条件时，CPU 仍可降频降低 WFI 电流。
     * SDK 的 pm 框架在不活跃时默认把 WFI 降到 ~20MHz(div 12)；这里进一步降到 4MHz(div 60,
     * 240/60)，VibeKey 无屏/无 EPIC/EZIP/LCDC 等高速外设，空闲期降频安全。
     * 仅影响 WFI 空闲时钟，不影响活跃态运行频率（仍由 run-mode/DLL2 决定）。
     * USB 插入期间(main.c usb_start)会改回 div 12 以保证 CDC 时延；USB 拔出再恢复 4MHz。
     * 清 FORCE_HP 确保不被强制保持高速（音频场景才会置位，本机不用）。 */
    HAL_RCC_HCPU_SetDeepWFIDiv(60, 0, 1);
    hwp_hpsys_rcc->DBGR &= ~HPSYS_RCC_DBGR_FORCE_HP;

    /* 08-29: 蓝灯闪烁定时器已移除 —— LED1(PA5) 改为充电指示, 由 bt_slot.c 的
     * bt_multi_set_charge() 驱动, 不在此处创建定时器(避免 2s 周期唤醒浪费)。 */

    /* LXT 关闭时启动 LP/RC 周期重校（~15s），保持 DEEP/RTC 唤醒计时精度。
     * 启动时的单次校准由 bsp_init.c::LRC_init() 完成，此处补运行时周期重校。 */
    if (HAL_LXT_DISABLED())
    {
        g_rc_cal_timer = rt_timer_create("rccal", rc_cal_timer_cb, RT_NULL,
                                         rt_tick_from_millisecond(15000),
                                         RT_TIMER_FLAG_PERIODIC | RT_TIMER_FLAG_SOFT_TIMER);
        if (g_rc_cal_timer) rt_timer_start(g_rc_cal_timer);
    }

    /* 低电量监控：<20% 红灯闪烁(2s亮/2s灭), ≤5% 关机(充电中豁免) */
    {
        rt_thread_t bat_mon = rt_thread_create("bat_mon", battery_monitor_entry, RT_NULL,
                                               2048, RT_THREAD_PRIORITY_LOW, 10);
        if (bat_mon) { rt_thread_startup(bat_mon); power_register_suspend_thread(bat_mon); }
    }

    /* 待机唤醒信号量（sleep_thread 待机时阻塞于此，无定时器，不干扰 DEEP 的 tick==MAX 判定） */
    g_standby_sem = rt_sem_create("stby", 0, RT_IPC_FLAG_FIFO);

    g_sleep_thread = rt_thread_create("sleep_w", sleep_thread_entry, RT_NULL,
                                       2048, RT_THREAD_PRIORITY_LOW, 10);
    if (g_sleep_thread)
        rt_thread_startup(g_sleep_thread);
}

/* ===== 调试命令：查看小核(LCPU)休眠状态 =====
 *
 * ⚠️ 铁律：HCPU 绝不能直接读 hwp_lpsys_aon（LPSYS_AON_BASE=0x40040000）。
 * 该寄存器块在 LP 电源域内，小核睡下去/掉电后总线无应答，HCPU 一读就
 * PRECISERR 硬件错误（实测 SCB->BFAR=0x40040000，tshell 线程 hard fault）。
 * 同理 hwp_lpsys_rcc / hwp_lpsys_cfg 等 0x4004xxxx 一律禁读。
 * 只能读 HCPU 自己域内的镜像/握手寄存器（0x500cxxxx，AON 常供电）：
 *
 *  1) HPSYS_AON->ISSR.LP_ACTIVE（0x500c0000）：LCPU 进睡眠前 CLEAR、醒来 SET。
 *     0 = 小核已睡，1 = 小核运行中。这是 HCPU 侧唯一直接的软件判据。
 *  2) HPSYS_AON->ISSR.LP2HP_REQ/HP2LP_REQ：跨核唤醒请求握手位。
 *  3) PMUC->LPSYS_LDO/LPSYS_SWR（0x500ca000，AON 域可读）：LP 域供电是否还开着。
 *  4) WSR 的 LP2HP 位 + ipc_queue_check_idle()：小核是否还在发 IPC。
 *  硬判据仍是整机电流：HCPU 已 [pm]S:3 但仍 mA 级 ⇒ 小核没睡。 */
static int lpstat(int argc, char **argv)
{
    uint32_t issr    = hwp_hpsys_aon->ISSR;
    uint32_t lp_ldo  = hwp_pmuc->LPSYS_LDO;
    uint32_t lp_swr  = hwp_pmuc->LPSYS_SWR;

    rt_kprintf("[LPSTAT] LP_ACTIVE=%d (%s)  ISSR=0x%08x (LP2HP_REQ=%d HP2LP_REQ=%d)\n",
               !!(issr & HPSYS_AON_ISSR_LP_ACTIVE),
               (issr & HPSYS_AON_ISSR_LP_ACTIVE) ? "LCPU RUNNING" : "LCPU SLEEPING",
               (unsigned)issr,
               !!(issr & HPSYS_AON_ISSR_LP2HP_REQ),
               !!(issr & HPSYS_AON_ISSR_HP2LP_REQ));
    rt_kprintf("[LPSTAT] PMUC LPSYS_LDO=0x%08x (EN=%d)  LPSYS_SWR=0x%08x\n",
               (unsigned)lp_ldo, !!(lp_ldo & PMUC_LPSYS_LDO_EN), (unsigned)lp_swr);
    power_dump_wsr("lpstat");
    return 0;
}
MSH_CMD_EXPORT(lpstat, show LCPU sleep status);

/* ===== 调试命令：LDO3 3.3V 外设域开关 / 电流分解 =====
 *
 * 用途：待机 630µA 需要定量分解到各负载。接好万用表后在活跃态逐条敲，
 * 每条之间读一次电流，差值即该负载的贡献：
 *
 *   ldo3 off        整域断电（IMU + WS2812 + I2C 上拉 + 编码器上拉一起消失）
 *   ldo3 on         恢复
 *   ldo3            只打印当前状态与各脚电平
 *
 * 判读提示：
 *  - 若 `ldo3 off` 后电流掉 ~600µA → 确认大头在本域，待机自动断电即可解决；
 *  - 若只掉一两百 µA → 剩余在主域（Flash DPD / LCPU / PMU 静态），另查；
 *  - 若电流反而上升 → 说明仍有反灌通路，检查是否还有别的脚连到本域却没进
 *    g_ldo3_pins[]（漏一个脚就足以毁掉整个断电收益）。
 *  - 编码器停在不同档位电流不同（A/B 触点接地时上拉电阻直接分压，
 *    10k 上拉 = 330µA/条）：断电前请转动编码器多测几个位置取最坏值。 */
static int ldo3(int argc, char **argv)
{
    if (argc >= 2 && rt_strcmp(argv[1], "off") == 0)
    {
        power_ldo3_domain_off();
    }
    else if (argc >= 2 && rt_strcmp(argv[1], "on") == 0)
    {
        power_ldo3_domain_on();
    }
    else
    {
        rt_kprintf("usage: ldo3 [on|off]\n");
    }

    rt_kprintf("[LDO3] state=%s  PERI_LDO=0x%08x\n",
               g_ldo3_off ? "OFF" : "ON", (unsigned)hwp_pmuc->PERI_LDO);
    rt_kprintf("[LDO3] ENC_A(PA2)=%d ENC_B(PA4)=%d SDA(PA10)=%d SCL(PA11)=%d INT1(PA26)=%d\n",
               rt_pin_read(2), rt_pin_read(4), rt_pin_read(10),
               rt_pin_read(11), rt_pin_read(26));
    return 0;
}
MSH_CMD_EXPORT(ldo3, LDO3 3V3 peripheral domain on / off / status);

/* ===== 调试命令：片内模拟域开关 / 电流分解 =====
 *
 * 用途：待机残余电流定位。接好万用表后逐条敲，差值即该模块贡献：
 *
 *   analog off          充电器(CHG_CR3 FORCE_RST) + codec REFGEN + GPADC 全断
 *   analog on           恢复
 *   analog              只打印当前状态与寄存器值
 *
 * 若 off 后电流掉 ~100~200µA → 确认大头在片内模拟偏置，待机自动断电即可。
 * 可逐个排查（先 charger，再 codec，再 gpadc）定位精确贡献。 */
static int analog(int argc, char **argv)
{
    if (argc >= 2 && rt_strcmp(argv[1], "off") == 0)
        power_analog_domain_off();
    else if (argc >= 2 && rt_strcmp(argv[1], "on") == 0)
        power_analog_domain_on();
    else
    {
        rt_kprintf("usage: analog [on|off]\n");
    }
    rt_kprintf("[ANALOG] state=%s\n", g_analog_save.active ? "OFF" : "ON");
    rt_kprintf("[ANALOG] CHG_CR3=0x%08x (FORCE_RST=%d)\n",
               (unsigned)hwp_pmuc->CHG_CR3,
               !!(hwp_pmuc->CHG_CR3 & PMUC_CHG_CR3_FORCE_RST));
    rt_kprintf("[ANALOG] AUDCODEC CFG=0x%08x REFGEN=0x%08x\n",
               (unsigned)hwp_audcodec->CFG, (unsigned)hwp_audcodec->REFGEN_CFG);
    rt_kprintf("[ANALOG] GPADC CFG_REG1=0x%08x\n",
               (unsigned)hwp_gpadc->ADC_CFG_REG1);
    return 0;
}
MSH_CMD_EXPORT(analog, analog domain (charger+codec+gpadc) on / off / status);

/* ===== 调试命令：PSRAM handle 诊断 =====
 * 查 rt_flash_get_handle_by_addr(MPI1_MEM_BASE) 返回的 handle 信息，
 * 用于判断 PSRAM1 的类型和 handle 是否有效。
 *
 *   psram           打印 handle 地址、Instance、isNand(SpiMode)
 *   psram sleep     手动调 HAL_MPI_PSRAM_ENT_LOWP（活跃态测试，接万用表看电流变化）
 *   psram wake      手动调 HAL_MPI_EXIT_LOWP */
static int psram_dbg(int argc, char **argv)
{
    extern void *rt_flash_get_handle_by_addr(uint32_t addr);

    FLASH_HandleTypeDef *h =
        (FLASH_HandleTypeDef *)rt_flash_get_handle_by_addr(MPI1_MEM_BASE);

    rt_kprintf("[PSRAM] MPI1 handle=%p\n", h);
    if (!h)
    {
        rt_kprintf("[PSRAM] handle is NULL! rt_flash_get_handle_by_addr didn't find PSRAM1.\n");
        return 0;
    }
    rt_kprintf("[PSRAM] Instance=%p  isNand=%u (0=QSPI 1=Legacy 3=OPI 4=Hyper)\n",
               h->Instance, (unsigned)h->isNand);

    if (argc >= 2)
    {
        if (rt_strcmp(argv[1], "sleep") == 0)
        {
            HAL_MPI_PSRAM_ENT_LOWP(h, h->isNand);
            rt_kprintf("[PSRAM] HAL_MPI_PSRAM_ENT_LOWP called (isNand=%u)\n", (unsigned)h->isNand);
        }
        else if (rt_strcmp(argv[1], "wake") == 0)
        {
            HAL_MPI_EXIT_LOWP(h, h->isNand);
            rt_kprintf("[PSRAM] HAL_MPI_EXIT_LOWP called\n");
        }
    }
    return 0;
}
MSH_CMD_EXPORT(psram_dbg, PSRAM handle diagnostics);
