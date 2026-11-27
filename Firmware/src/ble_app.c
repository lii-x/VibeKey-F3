
#include <rtthread.h>
#include "ble_app.h"
#include "key_config.h"   /* 11-24: 每槽蓝牙开关/HID 通道 */
#include "bf0_hal.h"
#include "drivers/pm.h"
#include <string.h>

#include "bf0_sibles.h"
#include "bf0_ble_gap.h"
#include "bf0_sibles_advertising.h"
#include "att.h"
#include "ble_led_service.h"
#include "ble_connection_manager.h"
#include "bt_connection_manager.h"   /* bt_cm_get_bonded_dev_by_addr() */
#include "power.h"
#include "bts2_app_inc.h"
#include "bts2_app_interface.h"
#include "bts2_app_generic.h"
#include "hci_api.h"

#include "ble_hid.h"
#include "bt_slot.h"
#define LOG_TAG "ble_app"
#include "ulog.h"

extern bts2_app_stru *bts2g_app_p;

extern ble_hid_env_t g_hid_env;

/* SCO(通话)进行中标志：供 power.c 的 15s RC 周期重校定时器跳过重校，
 * 避免 HAL_RC_CAL_update_reference_cycle_on_48M 扰动 LCPU(BT 控制器)→损坏 SCO 链路。
 * 仅作标志位，绝不在此手动切频或调 RC 校准（切频由 BT 栈 PM_SCENARIO_AUDIO 自管）。 */
volatile uint8_t g_sco_active = 0;

/* 当前 BLE 连接间隔(×1.25ms): 由 CONNECTED(重置为保守 40)与
 * UPDATE_CONN_PARAM_IND(读协商结果)维护。main.c 的 mouse_send_thread 据此
 * 自适应发送节拍(max(10ms, 间隔ms+2ms))—— 语音期 sco_quiet 的 40~80ms 间隔下
 * 固定 10ms 出包会耗尽 TX 池丢帧("语音结束后一卡一卡"), 自适应后任何参数状态
 * 都不超发不丢帧(08-26 根治, 取代事件驱动的慢节拍标志)。 */
volatile uint16_t g_ble_cur_interval = 40;

/* 当前 BLE 监督超时(×10ms)。2026-10-12 新增: 这是 `reason=8(conn timeout)` 断连的
 * 直接变量 —— `sco_quiet` 把它放宽到 3200(32s, 规范上限)以容忍 SCO 抢射频;
 * 若该协商未被手机确认, 链路会一直用旧的短超时, 被 SCO 饿过 32s 就断。
 * 此前只跟踪 con_interval、超时完全没观测 ⇒ 断连根因不可见。 */
volatile uint16_t g_ble_cur_supvto = 0;

/* ★ 2026-10-12: 本次语音是否已经切过 sco_quiet(给 SCO 让路)。
 * 两个触发点(VF active 提前切 + SCO_CONNECTED 兜底)应只协商一次 ——
 * 重复发起 LL 参数协商本身会扰动链路。在"语音结束 / BLE 断连"时清零。 */
static uint8_t g_sco_quiet_on = 0;

/* VF(语音识别)会话进行中: 由 BT_NOTIFY_HF_VOICE_RECOG_STATUS_CHANGE 事件维护.
 * 用于区分"语音助手会话"与"正常通话" —— 只有 VF 会话才允许强制拆 SCO
 * 兜底结束语音(否则会把正常通话拆断). */
static uint8_t g_vf_active = 0;

/* BVRA=0 复查 + 拆 SCO 兜底(结束语音): 见 ble_app_hfp_deactivate_vf */
#define VF_BVRA_RETRY_MS   2000
#define ESCO_IDLE_TEARDOWN_MS 2500   /* HFP 连上无通话, 延迟拆空闲 eSCO 让 BR 稳 Sniff */
static uint8_t g_vf_retry_left = 0;   /* >0: 下一轮再试 BVRA=0; ==0: 再下一轮拆 SCO */
static rt_timer_t g_vf_retry_timer = RT_NULL;
static rt_timer_t g_esco_idle_timer = RT_NULL;   /* HFP 连上无通话, 延迟拆空闲 eSCO */
static void esco_idle_teardown_cb(void *param);   /* 前向声明(定义见文件后部) */
static void esco_idle_teardown_schedule(void);    /* 前向声明(定义见文件后部) */
static void ble_app_voice_end_pm_restore(void);   /* 前向声明(定义见文件后部) */

/* 2026-10-12: 旧的"语音中 BLE 断开 → 延迟广播 + 30s 兜底定时器"机制已【整体移除】。
 * 两处原因:
 *   ① 语义错: 语音开关是 C1 → BLE HID 发 ALT 键 → 电脑输入法。BLE 一掉 ALT 就发不出去,
 *      用户【被彻底锁死】(停不了语音、SCO 干挂、Active ~3mA), 本地再按 C1 也没用
 *      (HFP 的 AT+BVRA=0 停的是手机端 VF, 与电脑输入法无关)。⇒ 必须立即恢复广播。
 *   ② 实现错: 该兜底挂在【软定时器】上, 而"BLE RF 活动会饿死 soft timer 线程"
 *      (本文件 ble_boot_timeout_thread 的注释早写明)。实测 30s 兜底从未触发,
 *      广播拖到 191s 后才恢复 ⇒ 一次掉线被放大成 ~3 分钟高功耗。
 * 现在统一走 BLE_GAP_DISCONNECTED_IND 里的【立即】ble_app_wakeup_advertise()。 */

/* 编码器启停（连接时启、断连即停，避免 1ms 轮询定时器常驻阻断 HPSYS 深睡） */
extern void enc_start(void);
extern void enc_stop(void);

#ifdef BT_DEVICE_NAME
    static const char *local_name = BT_DEVICE_NAME;
#else
    static const char *local_name = "VibeKey-F3";
#endif

/* 经典蓝牙(BR/EDR)名 —— ★ 11-18 按要求与 BLE 名保持一致。
 * 直接指向 local_name(而不是各写一份字符串), 以后改名只改一处, 不会漂移。
 * 历史: 10-21 曾是单个空格; 11-17 改为 "VibeKey-F3-BR/EDR"; 11-18 改回同名。
 * ⚠️ 已经配对过的主机会缓存旧名字, 需在主机侧"删除设备"后重新配对才会更新。 */
static const char *bt_classic_name = RT_NULL;   /* 运行时取 local_name, 见使用处 */

/* 设备类别(COD, Class of Device)：
 * 用户要求恢复默认图标 → 改回基线 0x200000(Object Transfer)。
 * ⚠️ 代价(08-07 实测): 安卓不当音频设备 → 不自动连 BR/EDR HFP, 对设备
 * page 消极响应(100s+) → 设备重启后 HFP 连接慢(~200s)。设备主动连
 * 兜底逻辑保留(6s 主动连 + 重试收敛), 仅手机响应慢。
 * 历史: 0x200414(音响图标)→0x200408(耳机图标)均实测改善手机响应
 * (24s), 若后续想恢复"手机识别为音频设备"的收益可改回。 */
/* ★★ 11-24 COD 修正 —— macOS 配对后不建 HID 通道的直接原因。
 *
 * COD(24bit) 字段布局：bit23-13 主要服务类 / bit12-8 主要设备类 / bit7-2 次要设备类
 *   0x200000 ⇒ 服务=Audio(bit21) ✅，**主要设备类=0x00(Miscellaneous)** ❌
 *   0x200414 / 0x200408(历史值) ⇒ 主要设备类=0x04(Audio/Video)，同样**不是外设**
 * 也就是说设备一直没对外宣称自己是"外设"，Windows 无所谓（照样查 SDP 读 HID 记录），
 * 但 **macOS 更严格：COD 不是 Peripheral 就不去建立 HID 控制/中断通道**
 * ⇒ 表现正是"能扫到设备、连上后没反应"。
 *
 * 正确值 0x2005C0：
 *   服务类 bit21 = Audio（保留，手机仍认它是音频设备）
 *   主要设备类   = 0x05 Peripheral
 *   次要设备类   = 0xC0 Keyboard + Pointing（我们同时暴露键盘/鼠标/多媒体）
 *
 * ⚠️ 与下方历史注释的取舍：曾经为了"手机更快连上 HFP"把 COD 调成音响/耳机图标
 *    (0x200414/0x200408)。现在必须让主要设备类回到 Peripheral —— 音频服务位仍在，
 *    手机的 HFP 行为应基本不变；若安卓 HFP 变慢，靠已有的"设备主动连 + 重试"兜底。
 */
uint32_t bt_get_class_of_device(void) { return 0x2005C0; }

typedef struct { uint16_t type; uint16_t event_id; uint16_t data_len; uint8_t *data; } bt_app_notify_data_t;

static rt_mq_t g_bt_hid_service_queue;
static struct rt_thread g_bt_hid_service_thread;
static uint8_t bt_hid_service_thread_stack[3072];

/* ===== Battery ===== */
#define BAT_ADC_CHANNEL 7
static uint8_t g_battery_percent = 0;
static volatile uint8_t g_battery_valid = 0;   /* 首次真实 ADC 读数后置 1，防开机初值 0% 误关机 */
static uint32_t g_battery_mv = 0;              /* 09-16: 最近一次成功读到的电池电压, 供低电关机用。
                                                * ★★★ 09-18 单位定案: **单位是 0.1mV，不是 mV**
                                                * （名字有误导性，勿按 mV 理解）。证据: drv_adc.c:477
                                                * `fval = HAL_ADC_RegToVoltageFloat(...)*10; // mv to 0.1mv based`
                                                * 再乘 vbat_factor(vbat 通道 1:2 分压)。实测
                                                * `ch[7]voltage=37830;18942.146484` ⇒ 37830=18942.146×2
                                                * ⇒ 表示 3783.0mV=3.783V。battery_table.c 的曲线表
                                                * 也是同一单位(4.18V=100%/3.50V=0%)，二者自洽。
                                                * ⚠️ 故 power.c 的 BAT_SHUTDOWN_MV 必须用 37000 表示
                                                * 3.70V（旧值 3700 少了十倍，条件恒假）。 */
static volatile uint32_t g_battery_read_seq = 0; /* 09-16: 每次成功读数自增 —— 供"本次读数是否新鲜"判定 */
#include "battery_calculator.h"
extern const battery_lookup_point_t discharge_curve_table[];
extern const battery_lookup_point_t charging_curve_table[];
extern const uint32_t discharge_curve_table_size;
extern const uint32_t charging_curve_table_size;

static void hid_report_battery(void)
{
    rt_device_t bat_dev = rt_device_find("bat1");
    if (!bat_dev) return;
    rt_adc_cmd_read_arg_t read_arg;
    read_arg.channel = BAT_ADC_CHANNEL;
    if (rt_adc_enable((rt_adc_device_t)bat_dev, read_arg.channel) != RT_EOK) return;
    rt_thread_mdelay(300);
    rt_uint32_t adc_val = rt_adc_read((rt_adc_device_t)bat_dev, read_arg.channel);
    rt_adc_disable((rt_adc_device_t)bat_dev, read_arg.channel);
    battery_calculator_t calc;
    battery_calculator_config_t cfg = {
        .charging_table = charging_curve_table, .charging_table_size = charging_curve_table_size,
        .discharging_table = discharge_curve_table, .discharging_table_size = discharge_curve_table_size,
        .charge_filter_threshold = 50, .discharge_filter_threshold = 30, .filter_count = 3,
        .secondary_filter_enabled = true, .secondary_filter_weight_pre = 90, .secondary_filter_weight_cur = 10,
    };
    battery_calculator_init(&calc, &cfg);
    g_battery_percent = battery_calculator_get_percent(&calc, adc_val);
    g_battery_mv = adc_val;
    g_battery_valid = 1;
    g_battery_read_seq++;   /* 标记"本次读数有效且新鲜" */
    rt_kprintf("[HID] battery: %d%% (%umV)\n", g_battery_percent, (unsigned)g_battery_mv);
    ble_bas_set_level(g_battery_percent);
}

uint8_t app_get_battery_percent(void) { return g_battery_percent; }
uint8_t app_battery_valid(void) { return g_battery_valid; }
/* 09-16: 最近一次成功读到的电压(mV) / 读数序号(比较两次调用即可判断"有没有刷新")。
 * 低电关机判定同时看百分比与电压, 并只在"读数确实刷新过"时才动作, 避免读到陈旧值。 */
uint32_t app_get_battery_mv(void) { return g_battery_mv; }
uint32_t app_get_battery_read_seq(void) { return g_battery_read_seq; }

/* 09-09: 待机 RTC 定时唤醒路径的电量刷新入口(hid_report_battery 为静态)。
 * 待机期 BLE 未连接, ble_bas_set_level 仅写 RAM 变量(ble_hid.c 有 is_connected
 * 保护), 调用安全。内含 300ms ADC 稳定延时, 调用方需持 IDLE 锁防框架进 DEEP。 */
void ble_app_battery_refresh(void)
{
    hid_report_battery();
}

static rt_thread_t g_bat_rpt_thread = RT_NULL;   /* 供待机 delete/recreate 使用 */

static void hid_battery_report_thread(void *param)
{
    (void)param;
    /* 开机后尽快首读：500ms 后读(ADC 内部再等 300ms 稳定, 实际 ~0.8s 出结果)。
     * 08-29: 3000ms->500ms —— 开机电量阶梯显示(bat_mon 首读)要尽早亮灯,
     * 避免比槽位 RGB 慢太多; 低电量指示/关机也不必等满 120s。 */
    rt_thread_mdelay(500);
    hid_report_battery();
    while (1) { rt_thread_mdelay(120000); hid_report_battery(); }  /* 120s 周期，省电 */
}
static void hid_battery_report_init(void)
{
    rt_thread_t tid = rt_thread_create("bat_rpt", hid_battery_report_thread, RT_NULL, 2048, RT_THREAD_PRIORITY_LOW - 1, 10);
    if (tid) { g_bat_rpt_thread = tid; rt_thread_startup(tid); }
}

/* 待机省电：power.c 在待机时 rt_thread_delete(g_bat_rpt_thread) 以摘除其 120s 定时器，
 * 唤醒后调本函数重建电池上报线程。 */
rt_thread_t ble_app_get_bat_rpt_thread(void) { return g_bat_rpt_thread; }
void ble_app_init_battery_report(void) { hid_battery_report_init(); }



static int bt_app_interface_event_handle(uint16_t type, uint16_t event_id, uint8_t *data, uint16_t data_len)
{
    bt_app_notify_data_t *msg = rt_malloc(sizeof(bt_app_notify_data_t) + data_len);
    RT_ASSERT(msg);
    msg->type = type; msg->event_id = event_id; msg->data_len = data_len;
    msg->data = (uint8_t *)((uint8_t *)msg + sizeof(bt_app_notify_data_t));
    if (data && data_len) rt_memcpy(msg->data, data, data_len);
    return rt_mq_send(g_bt_hid_service_queue, &msg, sizeof(msg));
}

// static void sco_restart_adv_cb(void *param)
// {
//     //ble_app_advertising_start();
// }

/* ===== HFP(经典蓝牙免提/麦克风) 自动连接与重连 =====
 * 旧实现 hfp_connect_timer 是一次性闩锁、从不复位 -> HFP 在一开机只尝试连接一次；
 * 一旦首次 BLE 断连(手机熄屏/距离/空闲都会触发)或首次 HFP 连接请求被拒，此后永远不再重连，
 * 表现为"经典蓝牙/麦克风有时候连不上(连上过一次后彻底失效，需重启)"。
 * 修复：断连 / HFP 断开 / 连接请求被拒 时复位闩锁与重试计数；BLE 重连或 HFP 掉线(仍连着 BLE)
 * 时自动重连，直到成功或达到重试上限。 */
#define HFP_CONNECT_RETRY_MAX 15
/* 连接尝试失败上限(08-07): 区别于总重试上限。总重试上限只在 start_connecting
 * 同步返回非 0 时累计; 而异步失败(CONN_CFM res≠0 → PROFILE_DISCONNECTED,
 * 实测每 ~25s 一次)走本上限——达上限即判定"手机已删配对/stale bond", 清配对
 * 转 page scan 等手机重新配对, 防止每 25s 骚扰式死循环重连。
 * ⚠️ 08-26 整改: 不再清配对/linkkey(用户要求, 避免失败即重新配对), 达上限
 * 仅提示 + 转 page scan 等主机侧处理。 */
#define HFP_CONNECT_FAIL_MAX  5
/* ⚠️ HFP 失败自动重派生经典 key(2026-08-26, 用户要求"失败不要删 linkkey"):
 * 经典侧 linkkey 表按对端单键存储(LINK_KEYBD_ADDR 无本机地址字段, 闭源栈),
 * 同机双槽下两槽的经典 key 会互相覆盖; 且认证失败时闭源经典栈会自行删除
 * (日志 rmv ... lk, cnt 2->1)。key 不匹配 → 认证失败 → 删 key → 下次再失败,
 * 表面像"每次都要重新配对"。修复: HFP 连接被拒后, 从 BLE LTK 重新派生经典
 * key 写回(connection_manager_convert_ltk_to_ilk, H6 异步, CONFIG_BT_FINSH
 * 已开 → sc_ble_bt_link_key_ind 写回经典栈), 延迟后重试 —— key 自愈, 无需
 * 用户重新配对。每 BLE 会话限次, 防无限循环。 */
#define HFP_REDERIVE_MAX       3
#define HFP_REDERIVE_DELAY_MS  600   /* 等 H6 异步派生写回经典栈 */
#define HFP_BLE_STABLE_MS     3000
#define HFP_CONNECT_DELAY_MS   1200   /* BLE 连上(panel scan 窗口超时后)延迟发起 HFP 兜底：
                                          * 已配对回连时 CTKD 早已派生, 1200ms 足矣(原 3000 偏长致"连得久") */
#define HFP_RETRY_INTERVAL_MS  2000   /* 重试间隔：给手机 page 建立经典蓝牙链路留时间 */
static uint8_t hfp_connect_timer;   /* 1=本次 BLE 连接期间已发起过 HFP 连接(防重复发起) */
static uint8_t hfp_connect_retry;
static uint8_t g_hfp_rederive_cnt;  /* 本次 BLE 会话已重派生经典 key 次数(HFP_REDERIVE_MAX 上限) */
static uint8_t g_hfp_connected;     /* HFP 链路已建立(麦克风可用) */

/* ★ 10-31 新增：宿主是否已连上 —— BLE 或 经典蓝牙(HFP/HID) 任一。
 * 起因：手机只走经典蓝牙(Android 不会连我们的 BLE)，而全工程大量逻辑用
 *       `g_hid_env.is_connected`(BLE) 当作"已连接"判据 ⇒ 在手机上永远为假：
 *       ① 槽位 RGB 每次按键唤醒都被重开成"等回连蓝闪"(用户反馈"按按键槽位灯闪")；
 *       ② 空中鼠标/旋钮只在 BLE 连上时启动 ⇒ 手机端鼠标完全不可用。
 * 以后凡是要判断"有没有宿主连着"，一律用本函数，不要再用 g_hid_env.is_connected。 */
int bt_host_linked(void)
{
    extern unsigned char classic_hid_linked(void);
    if (g_hid_env.is_connected) return 1;   /* BLE(PC/上位机)已连 */
    if (g_hfp_connected) return 1;          /* 经典 ACL + HFP 已连(手机主路径) */
    return classic_hid_linked() ? 1 : 0;    /* 经典 HID 通道已建 */
}

/* 经典链路对端地址（6 字节，小端）。由 BT_NOTIFY_HF_PROFILE_CONNECTED 事件填充。
 * 手机只走经典、BLE 从不连 ⇒ g_hid_env.peer_addr 是全 0，而发 HID 报文必须给真实地址。 */
static uint8_t g_cls_peer_addr[6];

/* 发经典 HID 报文要用哪个对端地址：
 *   PC   —— BLE 已连，用 g_hid_env.peer_addr（BLE 与经典是同一台设备，实测可用）
 *   手机 —— BLE 未连，用 HFP 事件里记下的经典地址
 * ⚠️ 不要改成去读 bts2g_app_p->hid_inst.conn[i].rmt_bd：实测这样取到的地址和
 *    SDK 库内部那份连接表对不上（结构布局/初始化时机差异），报文会被判"未连接"丢弃。 */
const uint8_t *bt_cls_peer_addr(void)
{
    if (g_hid_env.is_connected &&
        (g_hid_env.peer_addr[0] | g_hid_env.peer_addr[1] | g_hid_env.peer_addr[2] |
         g_hid_env.peer_addr[3] | g_hid_env.peer_addr[4] | g_hid_env.peer_addr[5])) {
        return g_hid_env.peer_addr;
    }
    if (g_cls_peer_addr[0] | g_cls_peer_addr[1] | g_cls_peer_addr[2] |
        g_cls_peer_addr[3] | g_cls_peer_addr[4] | g_cls_peer_addr[5]) {
        return g_cls_peer_addr;
    }
    return RT_NULL;
}

/* ★ 10-31: 空中鼠标活跃期禁止经典链路进 Sniff，空闲时恢复。
 * 为什么必须这么做：HFP 空闲时栈会把 ACL 打进 Sniff，间隔 500ms 左右
 * （日志 `Sniff mode st: 0, inv: 498.75` / `[L-U]hid mode change to 2`）。
 * Sniff 下鼠标报文只能等下一个锚点才发得出去 ⇒ 表现就是"一帧一帧、跟手很差"。
 * 原来只在 PTT(语音键) 按下时退出 sniff，鼠标没有对应处理。
 * 这里：活跃 → 先退出 sniff，并关掉 sniff 使能(防止刚退出又被拉回去)；
 *       空闲 → 重新允许 sniff，把功耗还原。 */
static void hfp_request_sniff_delayed(uint32_t delay_ms);   /* 定义在下方 */

void ble_app_mouse_sniff_ctl(int active)
{
    const uint8_t *peer = bt_cls_peer_addr();
    BTS2S_BD_ADDR bd;

    bt_interface_set_sniff_enable(active ? 0 : 1);

    /* ★ 11-06: 回到空闲时**补发一次 sniff 请求**。
     * 只把使能标志置 1 是不够的：`hfp_request_sniff_delayed()` 是**一次性**定时器，
     * 若它触发的那个 2s 窗口正好落在"鼠标活跃"期间（此时 sniff 使能=0、请求被丢），
     * 之后再也没人重发 ⇒ 这一次连接就永远进不了 sniff（"有时候进不去"的来源之一）。
     * 这里在空闲时补一发，把这条路径闭合。 */
    if (!active && peer && g_hfp_connected && !g_sco_active)
        hfp_request_sniff_delayed(1200);

    if (!active || !peer) return;
    bt_addr_convert_to_bts((bd_addr_t *)peer, &bd);
    hcia_exit_sniff_mode(&bd, NULL);
    rt_kprintf("[HID] mouse ACTIVE -> exit sniff, sniff disabled\n");
}

/* ★★★ 11-15 移动期反复踢出 sniff（解决手机端"晃完了才开始动"）
 * ============================================================================
 * 现象：同一设备在 PC 上正常，连手机时鼠标明显滞后——手已经停下，光标才开始移动。
 * 原因：`hcia_exit_sniff_mode()` 只在激活那一刻发**一次**；手机作为主设备过一会儿
 *       会自己把 ACL 重新拉进 sniff（sniff interval 常见 100~500ms）。
 *       链路一旦在 sniff，HID 中断报文必须等下一个嗅探锚点才能发出去
 *       ⇒ 报文被攒在本地，表现为"动作延迟 + 一卡一卡/追赶"。
 *       PC 端通常维持 active（或 sniff 极短），所以看不出来。
 * 对策：移动报文真正要发的时候，如果距上次踢出已超过 400ms，就再踢一次。
 *       手机收到 exit sniff 会回到 active，报文立刻能走。
 *       限流 400ms，避免自己造成"指令风暴"（原注释担心的点）。
 * ========================================================================== */
#define MOUSE_SNIFF_KICK_MS  150

/* ============================================================================
 * 配对完成后修正角色（11-23）
 * ============================================================================
 * 现象（手机）：**首次配对**连上后，空中鼠标明显"拖慢"；重启设备后恢复正常。
 * 根因：两次连接的角色不同 ——
 *   手机发起(dir=1) → 设备是 **slave**，只能等主机轮询才能发 HID 报文，
 *                     手机轮询间隔长 ⇒ 报文排队 ⇒ 手感拖慢；
 *   设备发起(dir=0) → 设备是 **master**，发包时序自己掌控 ⇒ 顺畅。
 *   （日志实证：dir=1 时 `role: slave`，重启后 dir=0 时 `role: master`）
 * 手机拒绝设备直接 role switch 回 master（res=14 安全理由），
 * 所以改用等效于"重启"的做法：配对保存完之后**主动断一次**，让 auto-connect
 * 以设备发起的方式重连 ⇒ 变 master。只在配对完成时触发一次，不影响日常回连。
 * ========================================================================== */
void ble_app_post_pair_role_fix(void)
{
    const uint8_t *peer = bt_cls_peer_addr();
    if (!peer) return;
    rt_kprintf("[HID] post-pair role fix: drop ACL, will reconnect as master (dir=0)\n");
    bt_interface_disconnect_req((unsigned char *)peer);
    /* 快速回连：auto-connect 默认 8s 一拍，这里插一次 1.5s 的提前量 */
    { extern void bt_cls_autoconn_kick(uint32_t delay_ms); bt_cls_autoconn_kick(1500u); }
}

void ble_app_mouse_sniff_kick(void)
{
    static rt_tick_t last = 0;
    rt_tick_t now = rt_tick_get();

    if (last != 0 && (now - last) < rt_tick_from_millisecond(MOUSE_SNIFF_KICK_MS))
        return;

    const uint8_t *peer = bt_cls_peer_addr();
    if (!peer) return;

    BTS2S_BD_ADDR bd;
    bt_addr_convert_to_bts((bd_addr_t *)peer, &bd);
    hcia_exit_sniff_mode(&bd, NULL);
    last = now;
}

/* ⚠️ 自愈重配已整体移除(2026-08-25): 设备侧无 bond 时【不再主动 create_bond】。
 * 原因: ① Windows 拒绝设备主动重配(实测 op10 GAPC_BOND ret66 → 断连19);
 * ② 延迟定时器会在 Windows 正常发起的配对流程进行中补发 create_bond →
 * 两次配对冲突 → 配对失败 → "重配-失败-重连"死循环(槽2 反复断连实测)。
 * 设备侧无 bond = 主机侧密钥 stale/缺失, 只能由主机侧处理(删旧设备后重新配对)。 */
static uint8_t g_boot_first_connect = 0;  /* 上电后是否已发生过 BLE 连接。
                                            * 用于区分"设备重启后首次连接"(手机不会主动
                                            * 发起经典蓝牙 ACL, 日志确认 dir=0) 与"运行中
                                            * 重连"(手机可能主动来连, 断开重连场景 dir=1)。 */

/* HFP 连上后延迟请求 sniff 的定时器 + 回调。
 * HFP 连上后还有 AT 命令交互，立即请求 sniff 会被打断。延迟 2s 等 AT 完成后再请求。
 * 三步策略：① 写 link policy 为 sniff+role switch ② 设置 DM 策略 KEEP_SNIFF +
 * 主动请求 sniff ③ 重试 BLE 参数放宽。
 * SDK Connection Manager 在 ACL 建立时设了 KEEP_SNIFF_INTERVAL 但用 NO_CHANGE 不改 link policy，
 * 若 link policy 没 sniff bit 则 sniff 永不进入。此处显式补上。
 * 注意：有 SCO 连接时 sniff 可能进不去（controller 限制），无 SCO 时可生效。 */
static BTS2S_BD_ADDR g_sniff_bd;   /* 静态生存期：定时器回调时仍有效 */
static rt_timer_t g_sniff_timer = RT_NULL;
static void hfp_sniff_timer_cb(void *param)
{
    (void)param;
    if (!g_hfp_connected) return;   /* HFP 已断开，不请求 */
    /* 11-06 诊断：把"请求发出时"的关键状态打出来，一眼分清是哪类原因 ——
     *   sco=1  ⇒ 通话中，controller 不允许 sniff（正常现象，通完会重发）
     *   idle 为 0 相关的判断不在本函数，但可对照 air_mouse 日志看当时是否活跃
     * 若这里一切正常却始终进不去，就是**对端(手机/PC)拒绝了 HCI Sniff_Mode**。 */
    LOG_I("[HID] sniff req: hfp=%d sco=%d", g_hfp_connected, g_sco_active);
    /* ① 写 link policy：出向连接禁用 role switch(0x0004=sniff only)，保持 master。
     * sniff bit 确保链路可进 sniff（若 SCO 未活动时 sniff 可生效）。 */
    bt_wr_link_policy(&g_sniff_bd, 0x0004);
    /* ② 设置 DM 策略 KEEP_SNIFF_INTERVAL：1s 空闲即进 sniff（SDK 默认 5s，太慢）。
     * interval=798(~499ms), attempt=4, timeout=1 */
    hcia_wr_lp_settings_keep_sniff_interval(&g_sniff_bd, 0x0004,
        1, 798, 798, 4, 1, NULL);
    /* ③ 主动请求 sniff（立即进入，不等 1s 空闲）。
     * 注意：有 SCO 连接时 sniff 可能进不去（controller 限制），但无 SCO 时可生效。 */
    bt_etner_sniff_mode(&g_sniff_bd, 798, 4);
    /* ④ 重试 BLE 连接参数恢复: 按当前活动状态收紧/放宽.
     * 不能无条件 relax——活跃态鼠标被降到 25Hz 会掉帧. */
    ble_app_conn_param_by_activity();
    rt_kprintf("[HID] sniff + link policy + DM keep_sniff + BLE param restore\n");
}
static void hfp_request_sniff_delayed(uint32_t delay_ms)
{
    /* ★★★ 11-10 修复：这里原来用 g_hid_env.peer_addr —— 那是 **BLE** 地址，
     * 而"手机只连经典蓝牙"时它恒为全 0（本文件 260 行的注释早就写明这一点，
     * 发 HID 报文时也早已改用 bt_cls_peer_addr()，唯独 sniff 这两处漏了）。
     *
     * 后果（用户日志逐行实证）：每次 `[HID] sniff req:` 后面紧跟
     *     write link policy error,try perform error handle
     *     svc_release gap_wr_lp_settings_compl2c_hdl ...
     * 因为地址是 0-0-0，HCI 命令直接被拒。⇒ 我们的 link policy / keep_sniff /
     * Enter Sniff **三条全部无效**，实际进 sniff 只能靠 SDK 自己的 keep_sniff
     * 计时器，日志里 1032s 才出现 `>> Sniff mode`（与注释记录的 268~331s 吻合）。
     * ⇒ 改用 bt_cls_peer_addr()（BLE 已连时取 BLE 地址，否则取经典对端地址）。 */
    const uint8_t *m = bt_cls_peer_addr();
    if (!m) return;                        /* 还没有对端地址，这次不排 */
    bt_addr_convert_to_bts((bd_addr_t *)m, &g_sniff_bd);
    if (g_sniff_timer) {
        rt_timer_stop(g_sniff_timer);
        rt_timer_delete(g_sniff_timer);
    }
    g_sniff_timer = rt_timer_create("sniff_req", hfp_sniff_timer_cb, RT_NULL,
        rt_tick_from_millisecond(delay_ms),
        RT_TIMER_FLAG_ONE_SHOT | RT_TIMER_FLAG_SOFT_TIMER);
    if (g_sniff_timer) rt_timer_start(g_sniff_timer);
}

/* PTT(语音开关键 C1)按下时提前退出 sniff：
 * HFP 空闲会自动进 sniff(间隔 798≈500ms)，突然要讲话时 SCO 建立需先退出
 * sniff(协商慢，语音开头丢/建链延迟)。提前退出为 SCO 建链铺路。
 * 由 main.c 在 C1 按下(开始语音)时调用；无 HFP 连接时无操作。 */
void ble_app_exit_sniff_for_call(void)
{
    if (!g_hfp_connected || !g_hid_env.is_connected) return;
    /* 同 hfp_request_sniff_delayed：必须用经典对端地址，g_hid_env.peer_addr 在
     * "只连经典"的场景下是全 0，发出去的 HCI 命令会被直接拒掉。 */
    {
        const uint8_t *m = bt_cls_peer_addr();
        if (!m) return;
        bt_addr_convert_to_bts((bd_addr_t *)m, &g_sniff_bd);
    }
    bt_exit_sniff_mode(&g_sniff_bd);
    LOG_I("[HID] exit sniff for call (PTT start)");
}

/* ⚠️ 覆盖 SDK __WEAK bt_sc_io_capability_rsp: 经典蓝牙 SSP 配对固定回复
 * NoInputNoOutput(无输入无输出) → 手机选 Just Works 自动配对, 无需确认。
 * 08-07 实测: 若走 Numeric Comparison(需显示并确认 6 位数字), 无屏幕设备
 * 无法确认 → 卡 274s 超时 pair fail(日志 Numeric_Value + auth_comp st22)。
 * 双保险: 事件路径(IO_CAPABILITY_IND/USER_CONFIRM_IND)兜底见事件处理。 */
void bt_sc_io_capability_rsp(BTS2S_BD_ADDR *bd)
{
    bt_io_capability_rsp(bd, IO_CAPABILITY_NO_INPUT_NO_OUTPUT, 0, 1);
}

/* HFP(经典蓝牙免提)链路连接状态查询。
 * power.c 的 RC 重校门控使用：任何蓝牙链路在线(BLE/HFP/SCO)时不做 RC 重校，
 * 避免切 LCPU 时钟+劫持 BLE MAC 忙等与随机操作撞车（08-06 偶发根因定案）。 */
uint8_t ble_app_hfp_connected(void)
{
    return g_hfp_connected;
}

/* 查询 peer_addr 是否已在已配对设备列表中（区分首次配对 vs 重连/重启）。
 * 已配对 → 设备主动发起 HFP（重连场景，手机不会主动来连 HFP）；
 * 未配对 → 开 page scan 等手机配对时主动建立经典蓝牙链路（首次配对场景）。*/
/* ★★ 11-27 诊断：打印 SDK bond 表全貌。
 * 目的：验证"同一台电脑在不同槽位配对时会互相覆盖"这一判断 ——
 *       bond 表只按对端地址索引（结构体里没有本地地址维度），
 *       所以同一 PC 配第二个槽时，旧槽的密钥应当被顶掉。
 * 用法：刷入后分别用槽1、槽2 配同一台电脑，对比两次日志的 [BOND] 段。
 * 看完可整段删除（无副作用，只读）。 */
void bt_dump_bonds(const char *tag)
{
    extern conn_manager_pair_dev_t g_bond_info;
    rt_kprintf("[BOND] ==== %s ====\n", tag ? tag : "");
    int shown = 0;
    for (int i = 0; i < MAX_PAIR_DEV; i++) {
        if (!g_bond_info.priority[i]) continue;
        const uint8_t *a = g_bond_info.peer_addr[i].addr.addr;
        rt_kprintf("[BOND] #%d pri=%d type=%d addr=%02x:%02x:%02x:%02x:%02x:%02x "
                   "ltk=%d auth=%d\n",
                   i, g_bond_info.priority[i], g_bond_info.peer_addr[i].addr_type,
                   a[5], a[4], a[3], a[2], a[1], a[0],
                   (int)g_bond_info.ltk_present[i], (int)g_bond_info.auth[i]);
        shown++;
    }
    if (!shown) rt_kprintf("[BOND] (empty)\n");
    rt_kprintf("[BOND] ==== end (%d entries, MAX=%d) ====\n", shown, MAX_PAIR_DEV);
}

/* ★★ 11-27 按槽恢复经典密钥（同一电脑多槽场景的关键一步）。
 *
 * 背景：经典 link key 在控制器里**只按对端地址存一份**，同一台电脑配了槽1、
 * 槽2 后，控制器里只剩最后配对那一槽的 key —— 换槽回连必然认证失败
 * （实测：classic_bond=YES 但无 ENCRYPTION_IND，30 秒后被电脑断开，再弹配对）。
 *
 * BLE 侧不受影响：bond 表每条带 local_irk，协议栈能按本地身份区分，多条并存。
 *
 * 解法：经典 key 本来就是**从 BLE LTK 经 CTKD 派生**的（日志 `ilk to lk`）。
 * 只要在开机选定槽位后，找到"本槽对应的那条 bond"，重新派生一次写回控制器，
 * 经典密钥就自动变成这一槽的正确值 —— 不需要读出/保存明文密钥。
 *
 * 怎么精确找到本槽的 bond：`ble_get_local_irk()` 拿到当前本地 IRK，
 * 与 bond 表里每条 `local_irk[i]` 比对（同一台电脑的多条记录，IRK 各不相同），
 * 再加对端地址匹配，双重确认。
 */
static uint8_t g_cls_key_synced = 0;   /* 本次上电是否已同步过（避免重复派生） */

void ble_app_sync_classic_key(const char *why)
{
    extern uint8_t bt_multi_get_slot(void);
    extern uint8_t bt_slot_get_addr(uint8_t slot, uint8_t *out6);
    extern conn_manager_pair_dev_t g_bond_info;
    extern uint8_t key_config_bt_flags(void);

    if (!(key_config_bt_flags() & KEY_CONFIG_BT_CLASSIC_EN)) return;  /* 经典关着, 不用管 */
    if (g_cls_key_synced) return;

    uint8_t slot = bt_multi_get_slot();
    uint8_t peer[6];
    if (!bt_slot_get_addr(slot, peer)) {
        rt_kprintf("[CLS KEY] slot%d 无记录, 跳过经典密钥同步 (%s)\n", slot, why);
        return;
    }

    ble_gap_sec_key_t my_irk;
    rt_memset(&my_irk, 0, sizeof(my_irk));
    if (ble_get_local_irk(&my_irk) != 0) {
        rt_kprintf("[CLS KEY] 取本机 IRK 失败, 跳过 (%s)\n", why);
        return;
    }

    for (int i = 0; i < MAX_PAIR_DEV; i++) {
        if (!g_bond_info.priority[i]) continue;
        if (memcmp(peer, g_bond_info.peer_addr[i].addr.addr, BD_ADDR_LEN) != 0) continue;
        if (memcmp(my_irk.key, g_bond_info.local_irk[i].key, GAP_KEY_LEN) != 0) continue;

        /* 命中：这条就是"本槽 + 本机地址"对应的 bond */
        connection_manager_convert_ltk_to_ilk(i);   /* LTK->ILK, H6 回调链式写回经典栈 */
        g_cls_key_synced = 1;
        rt_kprintf("[CLS KEY] slot%d 命中 bond#%d (peer %02x:%02x:%02x:%02x:%02x:%02x, IRK 匹配), "
                   "re-derive classic key (%s)\n",
                   slot, i, peer[5], peer[4], peer[3], peer[2], peer[1], peer[0], why);
        return;
    }
    rt_kprintf("[CLS KEY] slot%d 没有与本机 IRK 匹配的 bond (共 %d 条), 等配对后再说 (%s)\n",
               slot, MAX_PAIR_DEV, why);
}

static uint8_t hfp_peer_is_bonded(const uint8_t *addr)
{
    conn_manager_get_bonded_dev_t bonded;
    uint8_t count = connection_manager_get_bonded_devices((uint8_t *)&bonded);
    for (uint8_t i = 0; i < count; i++) {
        if (memcmp(addr, bonded.peer_addr[i].addr.addr, BD_ADDR_LEN) == 0)
            return 1;
    }
    return 0;
}

/* 返回 addr 在 SDK bond 表(g_bond_info)中的【原始索引】, 找不到返回 -1。
 * ⚠️ 不能用 connection_manager_get_bonded_devices 的返回序(它压缩了空位,
 * 索引错位), CTKD 重派生 API 需要 g_bond_info 原始索引。g_bond_info 是
 * ble_connection_manager.c 的全局变量(非 static), 可直接 extern。 */
static int hfp_peer_bond_index(const uint8_t *addr)
{
    extern conn_manager_pair_dev_t g_bond_info;
    for (int i = 0; i < MAX_PAIR_DEV; i++) {
        if (g_bond_info.priority[i] != 0 &&
            memcmp(addr, g_bond_info.peer_addr[i].addr.addr, BD_ADDR_LEN) == 0)
            return i;
    }
    return -1;
}

/* 从 BLE LTK 重新派生经典 linkkey 并写回经典栈(CTKD: LTK->ILK->LK, H6 异步)。
 * 解决经典 key 被覆盖/被闭源栈删除导致的 HFP 认证失败 —— key 自愈, 无需重配对。
 * 异步完成, 调用方需延迟 HFP_REDERIVE_DELAY_MS 再重试连接。 */
static void hfp_rederive_classic_key(void)
{
    int i = hfp_peer_bond_index(g_hid_env.peer_addr);
    if (i < 0) {
        rt_kprintf("[BLE HID] HFP key re-derive: no BLE bond for peer, skip\n");
        return;
    }
    connection_manager_convert_ltk_to_ilk(i);   /* LTK->ILK; H6 回调自动链 ILK->LK 并写回 */
    rt_kprintf("[BLE HID] HFP key re-derive from BLE LTK (bond idx=%d)\n", i);
}

/* HFP 主动连管理线程状态(声明在 hfp_bt_key_valid/hfp_mgr_thread_entry 之前供其使用) */
static uint32_t g_wait_phone_ms = 0;  /* 已配对: 先等手机主动来连的累计时长(超时后设备主动连) */
static uint8_t g_ble_param_updated = 0;  /* 手机 BLE 连接参数协商完成(手机 BLE 初始化稳定) */
static uint8_t g_hfp_failed_once = 0;  /* 本 BLE 会话内 HFP 连接尝试失败过(先等手机, 30s 后才主动连) */
static uint8_t g_stale_bond_cleared = 0;  /* 认证类失败(手机删配对/重配): 暂缓主动连, 等 CTKD 派生新 key 后恢复 */
static uint8_t g_auth_fail_cnt = 0;    /* 认证类失败连续计数(≥3 才真删 key, 防 key 永久失效死循环) */
static rt_sem_t g_hfp_mgr_sem = RT_NULL;  /* 唤醒 hfp_mgr 线程做"等 key 就绪→主动连"(事件线程不阻塞) */
static uint8_t g_phone_acl_up = 0;     /* 手机已主动来连且 ACL 建立(dir=1): 设备绝不主动连(双连接冲突) */
/* 经典蓝牙(BR/EDR)协议栈就绪标志(BT_STACK_READY 置位): 配对完成后恢复
 * page scan 前判断用 —— 栈未就绪时 set_scan_mode 无效, 由 STACK_READY 兜底。 */
static uint8_t g_br_ready = 0;
/* 广播活动标志：供"断连后按键唤醒再广播"判断是否已在广播，避免重复 start */
static uint8_t g_adv_active = 0;
static rt_tick_t g_ble_conn_tick = 0;   /* BLE 连上时刻(tick): 用于实测 BLE→HFP 连接耗时 */
static uint8_t  g_hfp_connect_dir = 0;  /* 本次 HFP 由谁建立: 0=设备抢发, 1=手机来连(ACL dir=1 改写) */

/* 设备侧是否有该设备的 BR/EDR link key(经典蓝牙 bonded)。
 * ⚠️ 与 hfp_peer_is_bonded(BLE LTK) 完全不同: BR/EDR 主动连能否认证通过
 * 取决于这个 key——手机删配对重新配对时, BLE LTK 可能还在(误判 bonded)
 * 但 BR/EDR key 是旧值 → 主动连认证失败(st22)。CTKD 派生后把【新 key】
 * 写入设备侧(wr lk), 此后主动连才有效。08-07 实测日志 13 定案。 */
static uint8_t hfp_bt_key_valid(void)
{
    /* 类型由 bt_connection_manager.h 提供（原来这里写的是 void*，与头文件冲突） */
    return bt_cm_get_bonded_dev_by_addr(g_hid_env.peer_addr) != NULL;
}

/* ============ 仅经典链路(手机)的主动回连 ============
 * 背景：整套 HFP 主动连逻辑(hfp_mgr_thread / pscan 链 / hfp_retry)全部挂在
 *       `g_hid_env.is_connected`(BLE) 上。手机只走经典蓝牙、BLE 从不连
 *       ⇒ 设备永远不会主动发起 ⇒ 每次都要在手机上点一下"连接"。
 * 做法：独立一条只依赖经典链路的回连定时器，完全不看 BLE 状态：
 *   - 目标地址：本次会话记下的经典对端 → 没有则用当前槽位里存的已配对地址
 *   - 只在"确实没有任何宿主连着"时发起；手机正在来连(g_phone_acl_up)或
 *     已判定要等主机重配(g_stale_bond_cleared)时不插手(避免双连接认证冲突)
 *   - 失败按 3s 间隔重试，最多 5 次即收敛(避免无脑 page 打扰手机 + 费电)
 *   - 设备侧没有 BR/EDR link key 时直接不发起(发起也必然认证失败)
 * 触发点：蓝牙栈就绪后 3s、HFP 断开后 3s、按键唤醒时 300ms。 */
static rt_timer_t g_cls_conn_timer = RT_NULL;
static uint8_t g_cls_conn_tries = 0;

/* ★★★ 11-15 断连锁 —— 解决"没有按键也会自己唤醒"
 * 现象：PC 蓝牙关掉后，设备每隔十几秒自己从 DEEP 醒一次，日志 `[pm]WSR:0x80`。
 *   0x80 = HPSYS_AON_WSR_LP2HP_IRQ（低功耗核/蓝牙子系统把主核叫醒），不是按键/RTC。
 * 根因：断连后 `bt_cls_autoconn_kick()` 会重置重试计数并重新排 5 次重连（间隔 3s），
 *   每次都拉起蓝牙协议栈 → 射频活动 → 又把主核唤醒 → 醒了又重试 ……
 *   日志里 `[CLS] auto connect #1..#5` 反复从 #1 开始，就是"计数被重置 + 重新来一轮"。
 * 对策：连续失败按"轮"退避，60s → 120s → 240s …… 上限 10 分钟，且**不再被唤醒事件重置**；
 *   只有真正连上（bt_host_linked）才清零。这样放着不动时唤醒频率会迅速降到分钟级。
 */
/* 用户需求：断连后**只给 60 秒**回连窗口，超时立即放弃，
 * 之后只有【按键】能重新触发（不再自己醒过来反复试）。 */
/* ★★★ 11-17 第三版：**改用"总尝试次数"硬上限**，不再依赖时间窗口。
 * 前两版失败的原因：每次"建链失败→断开"都会触发一次 kick，而 kick 里会
 * 重置各种计数/时间戳；只要有一个路径漏掉，60 秒就永远走不完
 * （实测循环了 15 分钟、auto connect 从 #1 数到 #10 又回 #1）。
 * 次数上限是**单调递增、只有两种事件能清零**（真正连上 / 用户按键），
 * 不依赖任何可能被重置的时间戳，最坏情况也一定会停。
 * 20 次 × 3 秒 ≈ 60 秒，正好符合"60 秒连不上就休眠"。 */
#define CLS_CONN_MAX_TOTAL   20
static uint16_t  g_cls_attempts = 0;      /* 本次断电周期内累计尝试次数(只增) */
static uint8_t   g_cls_giveup = 0;        /* 1=已放弃, 只等按键 */
/* ★★★ 11-17 关键：**只有"真的连上过"之后的断开，才重新给 60 秒**。
 * 之前的 bug：每次 ACL 建链失败也会收到 `[APP] HFP disconnected`，
 * 那段代码无条件重置窗口 ⇒ 60 秒永远走不完，日志里 auto connect 从 #1
 * 循环了几十次、持续九百多秒，一直在唤醒主核。 */
static rt_tick_t g_cls_link_up_tick = 0;  /* ACL 建立时刻(用于判断是否真的连上过) */

#define CLS_CONN_TRIES_MAX   5
/* ★ 11-21: 3s → 8s。经典寻呼一次超时就是 5.12s，3s 的间隔必然让第二条请求
 * 落在第一条的窗口内 —— 实测崩溃日志里正是"auto connect #1 未结束就发 #2"，
 * 随后 ACL 建起来后协议栈在 L2CAP 通道建立阶段跑飞(hard fault)。
 * 8s > 5.12s，保证任一时刻最多一个连接请求在途。 */
#define CLS_CONN_RETRY_MS    8000

/* 该主动连谁：优先本次会话记下的经典对端地址，否则用当前槽位的已配对地址 */
static const uint8_t *cls_target_addr(void)
{
    static uint8_t s_slot_addr[6];
    const uint8_t *p = bt_cls_peer_addr();
    if (p) return p;
    if (bt_slot_get_addr(bt_multi_get_slot(), s_slot_addr)) return s_slot_addr;
    return RT_NULL;
}

static void cls_autoconn_schedule(uint32_t ms);
static void cls_autoconn_cb(void *param);
void bt_classic_hid_kick_connect(void);   /* 定义在下方：排一次 HID 通道主动连接 */

static int cls_enabled(void);   /* 前向声明：判定定义在 cls_autoconn_cb 之前 */
/* 供其它路径（HFP 握手等）判断"经典是否被配置关闭" */
int ble_app_classic_enabled(void) { return cls_enabled(); }

/* ★ 11-25 HFP 连接统一入口：经典被关闭时**不发请求**，直接返回 0。
 * 返回 0 的语义是"已提交"，调用方的重试/告警逻辑因此不会启动
 * （用负值会让它进 8×2s 重试循环，反而一直骚扰射频）。
 * 之所以做成函数而不是在每处加 if：`bt_err_t r = if(...)` 不是合法 C，
 * 逐个包裹容易改坏语法，替换函数名则处处安全。 */
/* ★ 11-25 page/inquiry scan 统一入口：经典被关闭时**强制全关**。
 * 之前逐处加判断总会漏 —— BLE 连上后 "boot-first, page scan wait 800ms" 这条路
 * 又把 page scan 打开了（日志 target_scan_mode:2），主机照样能寻呼到设备。
 * 收敛到这一层后，任何调用点都不可能违背"经典关闭"的意图。 */
static uint8_t vk_set_scan_mode(uint8_t inquiry, uint8_t page_scan)
{
    if (!cls_enabled()) { inquiry = 0; page_scan = 0; }
    return bt_interface_set_scan_mode(inquiry, page_scan);
}

static bt_err_t vk_hfp_connect_req(const uint8_t *addr)
{
    if (!cls_enabled()) return 0;
    return bt_interface_hfp_hf_start_connecting((uint8_t *)addr);
}

static void cls_autoconn_schedule(uint32_t ms)
{
    if (!cls_enabled()) return;                               /* 经典已关闭，不排回连 */

    if (!g_cls_conn_timer) {
        g_cls_conn_timer = rt_timer_create("cls_conn", cls_autoconn_cb, RT_NULL,
            rt_tick_from_millisecond(ms),
            RT_TIMER_FLAG_ONE_SHOT | RT_TIMER_FLAG_SOFT_TIMER);
        if (!g_cls_conn_timer) return;
    } else {
        rt_timer_control(g_cls_conn_timer, RT_TIMER_CTRL_SET_TIME,
                         &(rt_tick_t){rt_tick_from_millisecond(ms)});
    }
    rt_timer_start(g_cls_conn_timer);
}

/* 按键唤醒/断连时调用：只有"没连上任何宿主"才排一次主动回连。
 * 按键是明确的用户意图 ⇒ 重置重试计数，让"按一下就想连上"能真的重试。 */
void bt_cls_autoconn_kick(uint32_t delay_ms);

/* 用户按键 = 明确意图：清掉"已放弃"和累计次数，重新允许 20 次回连 */
void bt_cls_autoconn_user_intent(void)
{
    g_cls_giveup = 0;
    g_cls_attempts = 0;
    g_cls_conn_tries = 0;
}   /* 前向声明(下面按键入口要用) */

/* 按键专用：用户明确要用 ⇒ 清掉"已放弃"并重新开一个 60 秒窗口 */
void bt_cls_autoconn_kick_user(uint32_t delay_ms)
{
    g_cls_giveup = 0;
    g_cls_attempts = 0;      /* 按键 = 用户明确要用: 次数清零，重新给 20 次 */
    g_cls_conn_tries = 0;
    bt_cls_autoconn_kick(delay_ms);
}

void bt_cls_autoconn_kick(uint32_t delay_ms)
{
    /* ★ 11-06: 关机流程中绝不能再拉起蓝牙。
     * 实测日志: "shutdown: enter hibernate" 之后仍出现 `[CLS] auto connect #1 -> ...`
     * 并真的发起了 HFP/ACL 连接 —— 关机倒计时里把协议栈又激活了一次，
     * 直接妨碍 PMU 进关断。g_pm_shutting_down 由 power.c 的 pm_shutdown() 置位。 */
    extern volatile int g_pm_shutting_down;
    if (g_pm_shutting_down) return;
    if (bt_host_linked()) { g_cls_conn_tries = 0; g_cls_giveup = 0; return; }

    /* 已放弃 ⇒ 任何事件都不再触发，只有按键能唤醒重连 */
    if (g_cls_giveup) return;

    /* 次数用完 ⇒ 放弃（不再依赖任何时间戳，绝对会停） */
    if (g_cls_attempts >= CLS_CONN_MAX_TOTAL) {
        g_cls_giveup = 1;
        g_cls_conn_tries = 0;
        rt_kprintf("[CLS] gave up after %d attempts, sleep until key\n", (int)g_cls_attempts);
        return;
    }

    g_cls_conn_tries = 0;
    cls_autoconn_schedule(delay_ms);
}

/* ★ 11-24 当前槽位是否允许经典蓝牙。关闭时所有"主动建链"路径都必须让路，
 * 否则设备自己 page 主机 → ACL 建起来 → HID 通道被踢开，"关闭"形同虚设。
 * （实测: 只跳过 classic_hid_open() 时，auto connect 仍把 PC 连上了。） */
static int cls_enabled(void)
{
    extern uint8_t key_config_bt_flags(void);
    return (key_config_bt_flags() & KEY_CONFIG_BT_CLASSIC_EN) != 0;
}

static void cls_autoconn_cb(void *param)
{
    if (!cls_enabled()) return;                               /* 经典已关闭 */
    extern volatile int g_pm_shutting_down;

    if (g_pm_shutting_down) return;                           /* 关机中: 不许再拉蓝牙 */
    if (bt_host_linked()) { g_cls_conn_tries = 0; g_cls_giveup = 0; return; }
    if (g_phone_acl_up) return;                               /* 手机正在来连，别抢 */
    if (g_stale_bond_cleared) return;                         /* 等主机重新配对，别骚扰 */

    const uint8_t *a = cls_target_addr();
    if (!a) return;
    if (!bt_cm_get_bonded_dev_by_addr((uint8_t *)a)) {
        rt_kprintf("[CLS] auto connect skipped: no BR/EDR link key\n");
        return;                                               /* 没 key：连了也认证不过 */
    }
    if (g_cls_giveup) return;
    if (g_cls_attempts >= CLS_CONN_MAX_TOTAL) {
        g_cls_giveup = 1;
        g_cls_conn_tries = 0;
        rt_kprintf("[CLS] gave up after %d attempts, sleep until key\n", (int)g_cls_attempts);
        return;
    }

    g_cls_conn_tries++;
    g_cls_attempts++;
    rt_kprintf("[CLS] auto connect #%d -> %02x:%02x:%02x:%02x:%02x:%02x (%d/%d)\n",
               g_cls_conn_tries, a[5], a[4], a[3], a[2], a[1], a[0],
               (int)g_cls_attempts, CLS_CONN_MAX_TOTAL);
    vk_hfp_connect_req((uint8_t *)a);
    /* 顺手把 HID 通道也排上：ACL 建起来之后（不管是 HFP 建的还是只建了 ACL）
     * 都能把 HID 控制/中断通道拉起来。即使对端不接受 HFP，键鼠还有机会工作。 */
    bt_classic_hid_kick_connect();

    cls_autoconn_schedule(CLS_CONN_RETRY_MS);   /* 排下一次重试；成功后会因 host_linked 提前收敛 */
}

/* ===== HFP 主动连管理线程 (08-07 定案: 事件线程绝不做长等待) =====
 * ⚠️ 教训: 上一版把"等 BR/EDR key 就绪"写成 BLE 连接事件(KE_EVT2 线程)里
 * mdelay 轮询 → 事件线程阻塞 → 整个蓝牙事件分发瘫痪(BLE 配对/MTU/服务发现
 * 全部卡死, 实测日志 15"连接卡住")。修复: 等待逻辑放【独立专用线程】——
 * 事件线程收到 BLE 连接事件只发信号量立即返回, 本线程被唤醒后做 key 轮询
 * 与主动连, 线程内 mdelay 不阻塞任何事件分发。
 * 与 pscan 窗口链兜底互斥: hfp_connect_timer 闩锁, 谁先发起谁赢。 */
static uint8_t hfp_connect_latch_take(void);   /* 前向声明: 连接闩锁原子获取(定义见下方) */

static void hfp_mgr_thread_entry(void *param)
{
    (void)param;
    while (1) {
        rt_sem_take(g_hfp_mgr_sem, RT_WAITING_FOREVER);
        /* 被唤醒(BLE 连接事件): 条件不满足直接回等 */
        if (!g_hid_env.is_connected || g_hfp_connected) continue;
        if (g_hfp_failed_once || g_stale_bond_cleared) continue;
        if (hfp_connect_timer) continue;   /* pscan 链已发起: 交它 */
        if (g_phone_acl_up) continue;      /* 手机已来连(ACL 已建): 设备绝不主动连(双连接冲突) */
        /* 轮询等设备侧 BR/EDR link key 就绪(CTKD 派生写入): 正常重启 key 在
         * → 立即; 重新配对场景等新 key(~20s) → 用新 key 连(认证必过)。
         * 期间手机来连(Incoming:1)则 g_hfp_connected=1 跳出。 */
        uint32_t waited = 0;
        while (waited < 40000) {
            if (!g_hid_env.is_connected || g_hfp_connected) break;
            if (hfp_bt_key_valid()) break;
            rt_thread_mdelay(2000);
            waited += 2000;
        }
        if (!g_hid_env.is_connected || g_hfp_connected || hfp_connect_timer)
            continue;
        if (!hfp_bt_key_valid())
            continue;   /* key 始终未就绪(手机重配中): 交 pscan 链等手机 */
        /* ⚠️ 双模主机兼容: 发起 HFP 前先等 BLE 稳定 —— conn 参数协商完成
         * (g_ble_param_updated, 手机场景一般 <2s)或最长 HFP_BLE_STABLE_MS。
         * 立即(8ms)page 双模主机(Windows)会干扰其 BLE 服务发现/加密,
         * 导致二次加密后断连(reason 19); 等 BLE 稳定后再 page 则无扰。
         * 期间主机若主动来连 BR/EDR 则 g_hfp_connected=1 提前退出。 */
        {
            uint32_t b_wait = 0;
            while (b_wait < HFP_BLE_STABLE_MS) {
                if (!g_hid_env.is_connected || g_hfp_connected) break;
                if (g_ble_param_updated) break;
                rt_thread_mdelay(200);
                b_wait += 200;
            }
        }
        if (!g_hid_env.is_connected || g_hfp_connected || hfp_connect_timer)
            continue;
        /* ⚠️ 抢发策略(08-22 整改): 设备永远先发、抢在手机前面把 HFP 建好。
         * 已配对回连时 BR/EDR key 在 BLE 连上时即就绪 → 立即发一次主动连,
         * 把"BLE 连上→HFP 连上"压到 ~1-2s; 手机随后 BR/EDR 请求发现链路已
         * 存在即无害重复。手机若抢先建 ACL(dir=1, g_phone_acl_up=1)则
         * hfp_connect_latch_take 返回 0, 设备不插手——双连接冲突由此杜绝。
         * 旧版 8s 等待已删除: BLE 连上即代表手机 BLE 已稳定, 无需再等。 */
        if (g_hid_env.is_connected && !g_hfp_connected && hfp_bt_key_valid()) {
            /* 原子获取闩锁(同时校验 g_phone_acl_up): 杜绝 mgr 线程与定时器回调
             * 并发各发一次连接 → 双连接认证混乱(日志 17)。 */
            if (hfp_connect_latch_take()) {
                g_hfp_connect_dir = 0;   /* 设备抢发 */
                rt_tick_t now = rt_tick_get();
                rt_kprintf("[HID] direct active HFP connect (bt key ready, mgr), BLE->HFP %lums\n",
                           (unsigned long)((now - g_ble_conn_tick) * 1000UL / RT_TICK_PER_SECOND));
                bt_err_t r = vk_hfp_connect_req(g_hid_env.peer_addr);
                if (r != 0) {
                    /* ⚠️ BLE 回连可能早于 BR/EDR 栈就绪(HFP 未使能): 首次请求返回
                     * BT_ERROR_STATE(0x10000005)被拒。若直接放弃, 依赖 pscan 链兜底
                     * 大概率也错过窗口 → HFP 永不连(实测"切蓝牙2 HFP 不主动连")。
                     * 延迟重试等栈就绪: 每 2s 一次, 最多 8 次(覆盖栈初始化 ~6s),
                     * 期间 pscan 链/手机来连成功则 g_hfp_connected=1 提前退出。 */
                    hfp_connect_timer = 0;   /* 释放闩锁, 允许重试 */
                    for (int i = 0; i < 8; i++) {
                        rt_thread_mdelay(2000);
                        if (!g_hid_env.is_connected || g_hfp_connected) break;
                        if (!hfp_connect_latch_take()) break;   /* 已被 pscan 链抢发 */
                        r = vk_hfp_connect_req(g_hid_env.peer_addr);
                        if (r == 0) {
                            rt_kprintf("[HID] HFP connect retry %d OK (BR/EDR ready)\n", i + 1);
                            break;
                        }
                        hfp_connect_timer = 0;   /* 又被拒: 继续等 */
                    }
                }
            }
        }
    }
}

/* ===== 配对/回连期 page scan 高活动度 (08-07 定案: "经典蓝牙很难连上"根因) =====
 * SDK 默认 page scan interval=0x800(1280ms)/window=0x12(11.25ms) → 监听窗口仅占
 * 周期 0.9% → 手机寻呼(page 超时通常 5~10s)大概率撞不上监听窗口 → 首次配对/
 * 重连 HFP 极难连上。等手机来连的窗口期切高活动度(interval 320ms/window 50ms,
 * 窗口占比 ~15.6%, 命中率提升 ~17x), 连接成功/窗口关闭后恢复默认低活动度省电。
 * hcia_wr_pagescan_activity 走 HCI 写命令(pmsg=NULL 即发送), 线程上下文安全。 */
#define PAGE_SCAN_DF_INTVL   0x800   /* 1280ms  SDK 默认 */
#define PAGE_SCAN_DF_WINDOW  0x12    /* 11.25ms */
#define PAGE_SCAN_HI_INTVL   0x200   /* 320ms  配对/回连窗口 */
#define PAGE_SCAN_HI_WINDOW  0x50    /* 50ms   (window 必须 <= interval) */
static void set_page_scan_activity(uint8_t high)
{
    /* ⚠️ 去重：HCI 命令为同步等待(发到 LCPU 等处理)。timeout_cb 在软定时器线程
     * 反复调 window_start → 每次 set_page_scan_activity 都发 HCI → LCPU 忙(BLE
     * 配对/ACL 断链清理)时同步阻塞 timer 线程 → 所有软定时器延迟 50s+(08-07
     * 实测"首次配对偶尔很久"：断开后重连 attempt 间隔本应 2s 实际 51s)。
     * 状态未变直接跳过，只在首次设置时发一次 HCI。 */
    static uint8_t last = 0xFF;
    if (high == last) return;
    last = high;
    if (high)
        hcia_wr_pagescan_activity(PAGE_SCAN_HI_INTVL, PAGE_SCAN_HI_WINDOW, NULL);
    else
        hcia_wr_pagescan_activity(PAGE_SCAN_DF_INTVL, PAGE_SCAN_DF_WINDOW, NULL);
}

/* page scan 等待窗口：BLE 连上后开 page scan 等手机主动来连（首次配对场景）。
 * 超时仍未连上 HFP → 已配对设备由设备主动发起 HFP 作为兜底。
 * ⚠️ 08-07 三次实测定案：首次配对/重新配对时手机 CTKD 派生密钥后会自动来连
 * 经典蓝牙(Incoming:1 全成功, 用已有 key 直接加密, 无认证/配对环节), 此时
 * 设备抢连会与手机冲突(0x0D/0x0C/双 ACL/Numeric 卡死) → 未配对只等手机。
 * 但【设备重启】场景手机不一定自动重连 BR/EDR(安卓重启后只静默回连 BLE,
 * HFP 常驻不恢复) → 已配对且等手机超时后设备主动连兜底(有 key 直连, 二次
 * 日志 attempt1→state3 成功实证; 失败由 RETRY_MAX/认证失败清 bond 收敛)。 */
static rt_timer_t g_page_scan_timer = RT_NULL;
static void hfp_connect_timer_cb(void *parameter);  /* 前向声明 */
static void page_scan_window_start(uint32_t ms);    /* 前向声明(timeout_cb 内重开窗口) */
static void hfp_retry_schedule(void);               /* 前向声明(已配对兜底主动连) */

/* ===== 连接闩锁原子获取 + ACL up 看门狗 (修复双连接竞态 / acl_up 卡死) =====
 * 三个上下文(mgr 线程 / 软定时器线程 / bt 事件线程)都会读改写 hfp_connect_timer
 * 与 g_phone_acl_up。原代码"检查-置位"非原子 → mgr 线程 8s 等待期间定时器回调
 * 也可能置位并各发起一次连接 → 双连接并发认证混乱(granted 0 → 182s 断开, 日志 17)。
 * 统一用关调度临界区做"读-判-置"原子操作, 保证同一时刻只有一个连接在途。 */
static uint8_t hfp_connect_latch_take(void)
{
    uint8_t got = 0;
    rt_enter_critical();
    if (!hfp_connect_timer && !g_phone_acl_up) {
        hfp_connect_timer = 1;
        got = 1;
    }
    rt_exit_critical();
    return got;
}

/* ACL up 看门狗: 手机建 BR/EDR ACL(dir=1)却迟迟不建 HFP 时, 超时清零
 * g_phone_acl_up → 允许设备主动连兜底, 避免主动路径被永久短路(缺陷5)。 */
#define ACL_UP_WATCHDOG_MS   20000
static rt_timer_t g_acl_up_timer = RT_NULL;
static void acl_up_watchdog_cb(void *param)
{
    (void)param;
    if (g_hfp_connected || !g_phone_acl_up) return;   /* 已连HFP或ACL已断: 无需动作 */
    g_phone_acl_up = 0;   /* 手机只建 ACL 不建 HFP: 释放保护, 交设备主动连兜底 */
    rt_kprintf("[HID] ACL up but no HFP in %ums, release acl_up guard, device fallback\n",
               ACL_UP_WATCHDOG_MS);
    if (g_hid_env.is_connected && !g_hfp_connected)
        hfp_retry_schedule();   /* 闩锁由 hfp_connect_latch_take 原子保护, 不会双连 */
}
static void acl_up_watchdog_start(void)
{
    if (g_acl_up_timer) { rt_timer_stop(g_acl_up_timer); rt_timer_delete(g_acl_up_timer); }
    g_acl_up_timer = rt_timer_create("acl_wd", acl_up_watchdog_cb, RT_NULL,
        rt_tick_from_millisecond(ACL_UP_WATCHDOG_MS),
        RT_TIMER_FLAG_ONE_SHOT | RT_TIMER_FLAG_SOFT_TIMER);
    if (g_acl_up_timer) rt_timer_start(g_acl_up_timer);
}
static void acl_up_watchdog_stop(void)
{
    if (g_acl_up_timer) { rt_timer_stop(g_acl_up_timer); rt_timer_delete(g_acl_up_timer); g_acl_up_timer = RT_NULL; }
}
static void page_scan_timeout_cb(void *param)
{
    if (g_hfp_connected) return;   /* HFP 已连上，无需再等 */
    if (!g_hid_env.is_connected) return;  /* BLE 已断连，等 BLE 重连后再开窗口 */
    /* 未配对设备(首次配对/已清 stale bond)：经典蓝牙连接须由手机主动发起——
     * 设备无链路密钥, 主动连会强制重新配对 → 手机端弹 Numeric 确认无人点 →
     * 卡 270s pair fail(08-07 三次日志实证)。仅保持 page scan 听候手机来连。
     * ⚠️ g_stale_bond_cleared: 清 bond 只删经典蓝牙 link key, BLE LTK 还在
     * → hfp_peer_is_bonded 仍返回 1 → 必须用此标志强制未配对路径, 否则
     * 无 key 主动连 → Numeric 卡死(实测日志 11 回归)。 */
    /* ★★ 11-26 配对模式：**只等对端主动连**，设备绝不主动发起。
     * 为什么单列一条：配对模式下 bond 可能只清了经典侧（跨槽保护会跳过删除），
     * hfp_peer_is_bonded() 仍返回 1 ⇒ 躲过下面的"未配对"判据 ⇒ 设备带着
     * 半残 bond 主动连 → 对端不接受 → 弹 Numeric 确认 → 无人点 → 卡 270s。
     * 实测就是"每次重连都要配对、手机 3 分钟后断开"的循环。 */
    if (bt_multi_is_pairing_window()) {
        page_scan_window_start(10000);  /* 只开监听窗口, 等电脑主动 page 我们 */
        return;
    }
    if (!hfp_peer_is_bonded(g_hid_env.peer_addr)) {
        page_scan_window_start(5000);   /* 未配对: 等手机来连 */
        return;
    }
    /* ⚠️ 认证类失败后(g_stale_bond_cleared): 本 BLE 会话内【只保持 page scan 等主机】。
     *
     * 09-16 修正(用户实测): 原实现"只要 hfp_bt_key_valid() 为真就恢复主动连"是错的 ——
     * 该函数只判断"设备侧 bond 表里有这个 peer"(bond 一直在, 恒为真), **不代表对端接受这个
     * key**。于是上面刚判定的"keep bond, wait host"被本回调立刻推翻: 设备自己从 BLE LTK
     * 重派生写回的 key 对端不认 → ACL 建起即被拆 → 每 ~31s 一轮
     * (1/3)(2/3)(3/3) → keep bond → pscan(bt key renewed) → 再主动连 …… **永远连不上、
     * 白耗电还骚扰主机**(日志实测)。
     * 现策略: 判定"认证类失败/等主机"后, 本会话不再自动主动连, 只开 page scan 听候主机。
     * 恢复时机: ①主机重新配对(经典链路由主机发起, 会经 ACL up 路径重建 HFP);
     *           ②BLE 重连 —— 新 BLE 会话在 CONNECTED 处理里会清 g_stale_bond_cleared。*/
    if (g_stale_bond_cleared) {
        page_scan_window_start(10000);
        return;
    }
    /* 已配对设备：先给手机 ~6s 主动来连机会(手机 BLE 回连后可能顺带重连
     * BR/EDR, 高活动度窗口提高寻呼命中率), 超时设备主动连兜底——设备重启
     * 场景安卓只回连 BLE、HFP 不自动恢复, 必须设备主动发起; 等久了手机也
     * 不会来(安卓对 HID 设备 BR/EDR 消极, 08-07 多次实测全程无 Incoming:1)。 */
    if (g_ble_param_updated) {
        g_wait_phone_ms = 0;
        rt_kprintf("[HID] pscan (BLE stable), fallback active HFP connect\n");
        hfp_retry_schedule();
        return;
    }
    /* 本会话已失败过一次: 重新配对场景手机 BLE 配对可能要几十秒(实测 55s),
     * 失败后立即重试=骚扰风暴(每 25-35s 一次, 全失败, 5 次清 bond → 无 key
     * 主动连 → Numeric 卡死)。改为: 失败后【先等手机 30s】(手机配对完成/
     * CTKD 后会来连 Incoming:1, 零认证直连), 30s 后仍未连上才主动连一次
     * (此时 key 可能已更新, 有 key 直连成功率稳定)。 */
    if (g_hfp_failed_once) {
        g_wait_phone_ms += 10000;
        if (g_wait_phone_ms < 30000) {
            page_scan_window_start(10000);
            return;
        }
        g_wait_phone_ms = 0;
        rt_kprintf("[HID] pscan (prev fail, waited 30s), retry active HFP connect\n");
        hfp_retry_schedule();
        return;
    }
    g_wait_phone_ms += 3000;
    if (g_wait_phone_ms < 6000) {
        page_scan_window_start(3000);
        return;
    }
    g_wait_phone_ms = 0;
    rt_kprintf("[HID] pscan wait (bonded) expired, fallback active HFP connect\n");
    hfp_retry_schedule();
}
static void page_scan_window_start(uint32_t ms)
{
    set_page_scan_activity(1);   /* 等手机来连的窗口期: page scan 高活动度, 提高寻呼命中率 */
    if (g_page_scan_timer) {
        rt_timer_stop(g_page_scan_timer);
        rt_timer_delete(g_page_scan_timer);
    }
    g_page_scan_timer = rt_timer_create("pscan", page_scan_timeout_cb, RT_NULL,
        rt_tick_from_millisecond(ms),
        RT_TIMER_FLAG_ONE_SHOT | RT_TIMER_FLAG_SOFT_TIMER);
    if (g_page_scan_timer) rt_timer_start(g_page_scan_timer);
}
static void page_scan_window_stop(void)
{
    set_page_scan_activity(0);   /* 连接成功/窗口关闭: 恢复默认低活动度省电 */
    if (g_page_scan_timer) {
        rt_timer_stop(g_page_scan_timer);
        rt_timer_delete(g_page_scan_timer);
        g_page_scan_timer = RT_NULL;
    }
    vk_set_scan_mode(0, 0);
}

/* 延迟 ms 后触发一次 HFP 连接尝试(hfp_connect_timer_cb 执行)。
 * 若 hfp_timer 不存在则创建（防御：HFP 断开重连时可能尚未创建）。 */
static void hfp_retry_schedule_delayed(uint32_t ms)
{
    if (!g_hid_env.hfp_timer)
        g_hid_env.hfp_timer = rt_timer_create("hfp_conn", hfp_connect_timer_cb, RT_NULL,
            rt_tick_from_millisecond(ms),
            RT_TIMER_FLAG_ONE_SHOT | RT_TIMER_FLAG_SOFT_TIMER);
    if (g_hid_env.hfp_timer) {
        rt_timer_stop(g_hid_env.hfp_timer);
        rt_timer_control(g_hid_env.hfp_timer, RT_TIMER_CTRL_SET_TIME,
                         &(rt_tick_t){rt_tick_from_millisecond(ms)});
        rt_timer_start(g_hid_env.hfp_timer);
    }
}
/* 默认间隔重试(连接失败/断开后的重试路径) */
static void hfp_retry_schedule(void)
{
    hfp_retry_schedule_delayed(HFP_RETRY_INTERVAL_MS);
}

/* ===================== 经典 HID 通道主动连接 =====================
 * 背景(2026-10-31)：用户报"只有首次配对能用，设备重启后再也用不了"。
 * 日志证据 —— 重启后 ACL/HFP 全部正常建立，但【从未出现】
 * `[L-U]remote connect HID control channel success`，随后按键全是
 * `[U-L]hid is not connect,can't send data`。
 * 原因：BR/EDR HID 里通道是【主机】开的，Windows 只在"首次配对 / 用户手动点连接"
 *      时开；设备自己为了 HFP 去 page 建立 ACL 后，Windows 不会顺带重开 HID。
 * 解法：设备侧主动 hid_conn_req(真实蓝牙键盘就是这个行为)。
 * 节奏：ACL 上来后 600ms 踢第一次，之后每 1.5s 重试，最多 4 次即收敛
 *      (主机不认就等下次 ACL/按键再踢，不无限骚扰射频与功耗)。
 * 线程：SDK 调用统一放到软定时器线程里做，避免发送线程(按键/鼠标)直接进协议栈。 */
static rt_timer_t g_hid_kick_timer = RT_NULL;
static uint8_t g_hid_kick_tries = 0;
static volatile uint8_t g_hid_kick_pending = 0;   /* 已排上队/在途，防高频重复调度 */

#define HID_KICK_TRIES_MAX   6
#define HID_KICK_FIRST_MS    600
#define HID_KICK_RETRY_MS    1500
/* ★ 11-10: 4 次(约 5s)放弃后**再也不会重试** —— 实测"断连一次再自动回连后
 * HID 发不上去"就是踩的这个坑：主机(Windows)重连后往往要好几秒才肯重开 HID
 * 通道，等它反应过来我们已经放弃了，而 g_hid_kick_tries 只在 ACL 连接/
 * PROFILE_CONNECTED 时清零 ⇒ 除非再断一次重连，这一辈子都不会再踢。
 * 改为：放弃后进入 10s 退避，退避期一过重新武装再给一轮。 */
#define HID_KICK_REARM_MS    10000

static rt_tick_t g_hid_kick_last_tick = 0;   /* 最后一次尝试/放弃的时刻 */
static uint8_t   g_hid_kick_warned = 0;      /* "已放弃"日志只打一次，重新武装时复位 */

static void hid_kick_fire(void *parameter);

/* 由 classic_hid.c 的发送闸门调用（可高频调用，已在途则直接返回） */
void bt_classic_hid_kick_connect(void)
{
    extern unsigned char classic_hid_linked(void);
    if (!cls_enabled()) return;                               /* 经典已关闭，不建 HID 通道 */

    /* ★ 11-17: 60 秒窗口用完进入"放弃"状态后，**任何来源都不许再踢**，
     * 否则 kick 状态机每 10s 自动重新武装，会一直把射频拉起来、反复唤醒主核。
     * 只有按键（bt_cls_autoconn_kick_user）会清掉这个标志。 */
    /* ★ 11-23 静默返回改成可观测 —— 之前这里一旦被挡住，日志里连一行提示都没有，
     * 只能看到"HFP 连上了但 HID 没反应"，完全无法判断是没踢还是踢了没用。 */
    if (g_cls_giveup) {
        rt_kprintf("[CLS HID] kick SKIP: giveup flag set (wait user key)\n");
        return;
    }

    if (g_hid_kick_pending || classic_hid_linked()) {
        rt_kprintf("[CLS HID] kick SKIP: pending=%d linked=%d\n",
                   g_hid_kick_pending, classic_hid_linked() ? 1 : 0);
        return;
    }

    /* ★ 11-10 重新武装：用满次数后不是永久放弃，退避期满就再来一轮，
     * 否则"主机重连后反应慢"会变成一次性的死局。 */
    if (g_hid_kick_tries >= HID_KICK_TRIES_MAX) {
        if (g_hid_kick_last_tick != 0 &&
            (rt_tick_get() - g_hid_kick_last_tick) < rt_tick_from_millisecond(HID_KICK_REARM_MS))
            return;                       /* 退避期内：不骚扰射频 */
        rt_kprintf("[CLS HID] kick re-armed after %d ms idle\n", HID_KICK_REARM_MS);
        g_hid_kick_tries = 0;
        g_hid_kick_warned = 0;
    }

    g_hid_kick_pending = 1;
    if (!g_hid_kick_timer) {
        g_hid_kick_timer = rt_timer_create("hidkick", hid_kick_fire, RT_NULL,
            rt_tick_from_millisecond(HID_KICK_FIRST_MS),
            RT_TIMER_FLAG_ONE_SHOT | RT_TIMER_FLAG_SOFT_TIMER);
        if (!g_hid_kick_timer) { g_hid_kick_pending = 0; return; }
    } else {
        rt_timer_control(g_hid_kick_timer, RT_TIMER_CTRL_SET_TIME,
                         &(rt_tick_t){rt_tick_from_millisecond(HID_KICK_FIRST_MS)});
    }
    rt_timer_start(g_hid_kick_timer);
}

/* ★★ 11-23 清掉 SDK HID 连接表里的陈旧槽位。
 * SDK 的 bt_hid_connect_requset() 开头用 bt_hid_check_connection_by_addr() 判重：
 *     for(i) if (bd_eq(&conn[i].rmt_bd, bd)) { 打印 "already connected or connecting"; return; }
 * 它**只比地址**，而且 bt_hid_close() 并不会清这张表。
 * 于是深睡唤醒后（我们 close+open 过），每次踢都会被判成"已连接"直接返回，
 * HID 通道永远建不起来 —— 日志实证：kick connect #1..#6 后面全是
 * "This address already connected or connecting"，最后次数耗尽放弃。
 * 对策：发起连接前把该地址对应的槽位清空，让 SDK 的判重放行。 */
static void hid_clear_stale_conn_slot(BTS2S_BD_ADDR *bd)
{
    bts2_app_stru *app = (bts2_app_stru *)bts2g_app_p;
    if (!app || !bd) return;

    for (int i = 0; i < CFG_MAX_HID_CONN_NUM; i++) {
        if (bd_eq(&app->hid_inst.conn[i].rmt_bd, bd)) {
            rt_kprintf("[CLS HID] clear stale hid conn slot #%d (%02x:%02x:%02x:%02x:%02x:%02x)\n",
                       i, bd->lap & 0xFF, (bd->lap >> 8) & 0xFF, (bd->lap >> 16) & 0xFF,
                       (bd->lap >> 24) & 0xFF, bd->uap, bd->nap & 0xFF);
            /* ★★ 11-26 必须清**整条记录**，不能只清 rmt_bd。
             * 只清地址会留下"地址全 0 + 旧 CID/状态"的半残项：SDK 的判重放行、
             * 把新连接塞进同一槽，而旧的 L2CAP 通道句柄还在 →
             * 后续 l2cb_send_hci_data_pkt 用到已失效的缓冲区指针 →
             * mem manage fault(DACCVIOL, MMAR=0x200020B0) 实测崩溃。
             * 这里整条清零，让槽位回到"真正空闲"的语义。 */
            memset(&app->hid_inst.conn[i], 0, sizeof(app->hid_inst.conn[i]));
        }
    }
}

static void hid_kick_fire(void *parameter)
{
    extern unsigned char classic_hid_linked(void);
    extern void bt_hid_connect_requset(BTS2S_BD_ADDR *bd);

    g_hid_kick_pending = 0;
    if (!bts2g_app_p) return;
    if (classic_hid_linked()) {          /* 主机自己开了 ⇒ 收工 */
        g_hid_kick_tries = 0;
        return;
    }
    if (g_hid_kick_tries >= HID_KICK_TRIES_MAX) {
        g_hid_kick_last_tick = rt_tick_get();     /* 记录放弃时刻，供重新武装计时 */
        if (!g_hid_kick_warned) {
            g_hid_kick_warned = 1;
            rt_kprintf("[CLS HID] kick gave up after %d tries, will retry after %dms\n",
                       HID_KICK_TRIES_MAX, HID_KICK_REARM_MS);
        }
        return;
    }

    g_hid_kick_tries++;
    {
        BTS2S_BD_ADDR bd;
        const uint8_t *m = bt_cls_peer_addr();
        if (!m) return;                       /* 还没有任何对端地址，等下次再踢 */
        bt_addr_convert_to_bts((bd_addr_t *)m, &bd);
        rt_kprintf("[CLS HID] kick connect #%d -> %02x:%02x:%02x:%02x:%02x:%02x\n",
                   g_hid_kick_tries, m[5], m[4], m[3], m[2], m[1], m[0]);
        hid_clear_stale_conn_slot(&bd);   /* 放行 SDK 的地址判重，见函数上方说明 */
        bt_hid_connect_requset(&bd);
    }

    /* 排下一次重试 */
    g_hid_kick_pending = 1;
    rt_timer_control(g_hid_kick_timer, RT_TIMER_CTRL_SET_TIME,
                     &(rt_tick_t){rt_tick_from_millisecond(HID_KICK_RETRY_MS)});
    rt_timer_start(g_hid_kick_timer);
}

/* ============================================================================
 * HID 链路标志自愈（11-10）
 * ============================================================================
 * 故障现场（用户日志）：
 *     [KB] -> classic mod=0x00 n=6 keys=00 ...
 *     [U-L]hid is not connect,can't send data
 *     [hid_debug] bd 0-0-0, control_cid=0x0000, interrupt_cid=0x0000
 * `[KB] -> classic` 说明**我们自己的 s_hid_linked 还是 1**（否则会走 BLE），
 * 但 SDK 内部根本没有这条连接 —— 标志卡住了，于是 hid_tx_gate() 认为"已连"、
 * 永远不去重踢通道，所有报文都被 SDK 丢掉。
 *
 * 为什么会卡：HID 通道的断开事件并不是每次都送到（链路突然消失、主机单方面
 * 拆除、协议栈内部清理等情况），只靠事件维护标志必然会有残留。
 *
 * 对策：用 SDK 的真值定期校正（bt_hid_check_connection_by_addr 是公开 API，
 * 同时覆盖"已连接"和"连接中"两种状态），不一致就纠正并重踢通道。
 * 周期 2s：足够快，又不至于骚扰射频。
 */
int bt_classic_hid_sdk_connected(void)
{
    const uint8_t *m = bt_cls_peer_addr();
    BTS2S_BD_ADDR bd;
    U8 idx = 0;

    if (!m) return 0;
    bt_addr_convert_to_bts((bd_addr_t *)m, &bd);
    return bt_hid_check_connection_by_addr(&bd, &idx) ? 1 : 0;
}

static rt_timer_t g_hid_sync_timer = RT_NULL;

static void hid_link_sync_cb(void *parameter)
{
    extern unsigned char classic_hid_linked(void);
    extern unsigned char classic_hid_ready(void);
    extern void classic_hid_set_linked(unsigned char);

    if (!classic_hid_ready()) return;          /* ACL 不在，没什么可校正的 */

    int sdk = bt_classic_hid_sdk_connected();
    int ours = classic_hid_linked() ? 1 : 0;

    if (sdk == ours) return;

    if (sdk) {
        /* ★★ 11-23 关键修正：**不能**凭 sdk 恢复"已连接"标志。
         * `bt_classic_hid_sdk_connected()` 底层是 bt_hid_check_connection_by_addr()，
         * 而它的实现只是遍历 hid_inst.conn[] **比较一下对端地址**
         *   for(i) if (bd_eq(&conn[i].rmt_bd, bd)) return TRUE;
         * —— 它表示"这个地址还登记在 HID 槽位里"，**完全不反映 L2CAP 通道是否打开**。
         * 深睡唤醒后我们刚 close+open 过，旧槽位地址仍在 ⇒ 它照样返回真，
         * 于是这里把标志恢复成 1，报文被送进一个 PC 根本没建立的通道 → 静默丢失。
         * 正确做法：只把"地址已登记但通道未知"当作**该去建通道**的信号，
         * 踢一次 HID 连接，等 BT_NOTIFY_HID_PROFILE_CONNECTED 这个唯一可靠事件来置位。 */
        bt_classic_hid_kick_connect();
    } else {
        classic_hid_set_linked(0);
        rt_kprintf("[CLS HID] sync: SDK says NOT connected -> flag cleared, re-kick\n");
        g_hid_kick_tries = 0;
        g_hid_kick_warned = 0;
        g_hid_kick_last_tick = 0;
        bt_classic_hid_kick_connect();
    }
}

static void hid_link_sync_start(void)
{
    if (!g_hid_sync_timer) {
        g_hid_sync_timer = rt_timer_create("hidsync", hid_link_sync_cb, RT_NULL,
            rt_tick_from_millisecond(2000),
            RT_TIMER_FLAG_PERIODIC | RT_TIMER_FLAG_SOFT_TIMER);
        if (!g_hid_sync_timer) return;
    }
    rt_timer_start(g_hid_sync_timer);
}

/* ===================== 经典 HID 鼠标报文（自己组包） =====================
 * 为什么不再用 SDK 的 bt_hid_mouse_drag_page：
 *   SDK 的鼠标报告 ID 是 2，而 SDK 在【每次 HID 连接成功】时都会调
 *   bt_hid_mouse_reset_at_middle()：先 (-2047,-2047) 把光标拽到屏幕角，再 (+180,+300)
 *   挪回来 —— 那是空中鼠标遥控器的绝对定位校准，副作用就是"每次重连光标飞到角落"。
 *   这两条报文固定用报告 ID 2，无法从应用层拦。
 * ⇒ 对策：我们的描述符里【不定义报告 ID 2】，鼠标改用 ID 3。主机按 HID 规范
 *   丢弃未知报告 ID 的报文，跳变从根上消失（不需要改 SDK）。
 * 报文：报告 ID 3，输入 4 字节 = [按键][X int8][Y int8][滚轮 int8]
 * 缓冲区首字节是协议头（DATA|INPUT），栈会剥掉它并把 report_id 作为报文首字节。 */
void bt_classic_hid_send_mouse_raw(unsigned char buttons, int dx, int dy, int wheel)
{
    extern unsigned char classic_hid_linked(void);

    if (!bts2g_app_p || !classic_hid_linked()) return;

    if (dx > 127) dx = 127; else if (dx < -127) dx = -127;
    if (dy > 127) dy = 127; else if (dy < -127) dy = -127;
    if (wheel > 127) wheel = 127; else if (wheel < -127) wheel = -127;

    /* ⚠️ 目标地址取 bt_cls_peer_addr()：手机只用经典链路，BLE 地址恒为 0，
     * 拿 g_hid_env.peer_addr 会让 SDK 查不到连接并丢弃报文（详见该函数注释）。 */
    const uint8_t *peer = bt_cls_peer_addr();
    if (!peer) return;
    BTS2S_BD_ADDR bd;
    bt_addr_convert_to_bts((bd_addr_t *)peer, &bd);

    /* ★ 10-31 只在"手势报文"(按键/滚轮)时退出 sniff —— 这类报文低频、单发，
     * 退出 sniff 的代价可以接受；移动报文每 10ms 一帧，若每帧都发一条
     * HCI Exit Sniff 指令会形成指令风暴，反而把链路拖慢(加重掉帧)。
     * 移动期的 sniff 由 air_mouse 活跃态统一处理(ble_app_mouse_sniff_ctl)。 */
    if (buttons || wheel)
        hcia_exit_sniff_mode(&bd, NULL);

    unsigned char msg[6];
    msg[0] = 0xA1;                    /* (HID_MSG_TYPE_DATA<<4) | HID_REPORT_TYPE_INPUT */
    /* ★ 11-06: 报告 ID 改为 4。ID 3 已让给多媒体 —— SDK 的 bt_hid_send_consumer()
     * 固定用 HID_CONSUMER_REPORT_ID(=0x03)，而早期版本把 ID 3 给了鼠标，
     * 导致多媒体报文被主机当成鼠标报文解析 ⇒ 多媒体键完全没反应。 */
    msg[1] = 0x04;                    /* 报告 ID 4（见 cls_hid_desc.c） */
    msg[2] = buttons;
    msg[3] = (unsigned char)(signed char)dx;
    msg[4] = (unsigned char)(signed char)dy;
    msg[5] = (unsigned char)(signed char)wheel;
    /* 诊断(10-31): 只打印"带按键/滚轮"的报文（移动报文太多不打印），
     * 用来确认按键报文到底发出去没有、内容对不对、发给谁。 */
    if (buttons || wheel) {
        static rt_tick_t last = 0;
        rt_tick_t now = rt_tick_get();
        if (last == 0 || (now - last) > rt_tick_from_millisecond(50)) {
            last = now;
            rt_kprintf("[MS] TX b=0x%02x x=%d y=%d w=%d -> %02x:%02x:%02x:%02x:%02x:%02x\n",
                       buttons, dx, dy, wheel,
                       peer[5], peer[4], peer[3], peer[2], peer[1], peer[0]);
        }
    }

    /* ★ 10-31: 用【不带对端地址】的发送接口。
     * 带地址的 hid_send_report_req_ext 会按 bd 去内部连接表里查，查不到就打
     * "[U-L]hid is not connect,can't send data" 直接丢弃 —— 实测 PC/手机两端都会踩到
     * （表里明明有连接）。本机只有一路经典 HID，用单连接接口即可，彻底绕开地址匹配。 */
    hid_send_report_req(bts2g_app_p->phdl, sizeof(msg), msg, TRUE);
}

static void hfp_connect_timer_cb(void *parameter)
{
    if (!g_hid_env.is_connected || g_hfp_connected)
        return;
    /* ⚠️ 手机已主动来连(ACL 已建立): 设备绝不主动连——双连接并发会让手机
     * 来连的认证流程失败(实测日志 17: granted 0 → 182s 后断开)。 */
    if (g_phone_acl_up)
        return;
    /* 09-16: 已判定"认证类失败 / 等主机"(g_stale_bond_cleared) → 本 BLE 会话内不再主动连。
     * 集中在此拦截: 任何路径(pscan 链/mgr 线程/acl_up 看门狗)调度出的主动连都会落到本回调,
     * 在此挡住即可杜绝"每 ~31s 一轮"的空转骚扰(对端不认设备侧 key, 重试必然再失败)。
     * 恢复: 主机重新配对(经典链路由主机发起) 或 BLE 重连(CONNECTED 处理会清该标志)。 */
    if (g_stale_bond_cleared)
        return;
    /* ⚠️ 原子获取闩锁(含 g_phone_acl_up 校验): 已有连接在途或手机已来连 → 跳过,
     * 杜绝 mgr 线程与定时器回调并发各发一次连接 → 双连接认证混乱(日志 17)。
     * 连接失败/断开由 PROFILE_DISCONNECTED 清零, 成功保持(已连不重复)。 */
    if (!hfp_connect_latch_take())
        return;
    /* 已达重试上限：不无脑死循环重试(避免刷屏/耗电), 但也不永久放弃——
     * 复位计数并重新开 page scan 窗口, 等主机来连或下一轮兜底主动连(自愈)。
     * ⚠️ 08-26 整改: 【不再删除 bonded/linkkey】—— 删 key 会让主机以为配对
     * 丢失, 触发重新配对(用户明确不要)。仅复位计数 + 开 page scan 等主机侧。
     * 若主机确实删了配对, 由主机发起配对时 SDK 会正常走首次配对流程。 */
    if (hfp_connect_retry >= HFP_CONNECT_RETRY_MAX) {
        rt_kprintf("[BLE HID] HFP retry %d exhausted, keep bond, re-arm page scan\n",
                   HFP_CONNECT_RETRY_MAX);
        hfp_connect_retry = 0;
        hfp_connect_timer = 0;
        g_stale_bond_cleared = 1;   /* 强制未配对路径: 只等主机重新配对, 不再主动连 */
        g_hfp_failed_once = 0;
        page_scan_window_start(800);   /* 按未配对处理等主机来连 */
        return;
    }

    hfp_connect_timer = 1;
    vk_set_scan_mode(0, 1);   // 不可被搜索(关 inquiry)，保持可连接(page 开)
    rt_kprintf("[BLE HID] HFP connect attempt %d -> %02x:%02x:%02x:%02x:%02x:%02x\n",
        hfp_connect_retry,
        g_hid_env.peer_addr[5], g_hid_env.peer_addr[4], g_hid_env.peer_addr[3],
        g_hid_env.peer_addr[2], g_hid_env.peer_addr[1], g_hid_env.peer_addr[0]);
    bt_err_t ret = vk_hfp_connect_req(g_hid_env.peer_addr);
    if (ret != 0) {
        /* MAC 非法 / 请求被拒：释放闩锁并计数，800ms 后由本定时器重试 */
        hfp_connect_timer = 0;
        hfp_connect_retry++;
        if (hfp_connect_retry < HFP_CONNECT_RETRY_MAX)
            hfp_retry_schedule();
        rt_kprintf("[BLE HID] HFP connect rejected (ret=%d), retry %d/%d\n",
                   ret, hfp_connect_retry, HFP_CONNECT_RETRY_MAX);
    }
    /* ret==0：请求已提交，等待 BT_NOTIFY_HF_PROFILE_CONNECTED；
     * 若手机迟迟不响应，由 HFP 断开事件或下次 BLE 断连兜底重新发起。 */
}

static int bt_hid_service_notify_event_handle(bt_app_notify_data_t *msg)
{
    if (msg->type == BT_NOTIFY_COMMON)
    {
        switch (msg->event_id) {
        case BT_NOTIFY_COMMON_BT_STACK_READY:   // bt蓝牙协议栈准备就绪事件
            g_br_ready = 1;
            /* 经典蓝牙名: 默认与 BLE 名相同(bt_classic_name 为 NULL 时回落到 local_name) */
            const char *cls_name = bt_classic_name ? bt_classic_name : local_name;
            bt_interface_set_local_name(strlen(cls_name), (void *)cls_name);
            bt_dump_bonds("BT stack ready");
            ble_app_sync_classic_key("bt stack ready");
            LOG_I("[HID] BT stack ready, classic_name=\"%s\" ble_name=%s",
                  cls_name, local_name);
            /* ★ 11-24 按槽位开关决定是否拉起经典蓝牙。
             * 关掉经典 ⇒ 不注册 HID、不开 page scan，设备在 BR/EDR 上完全沉默
             * （省电 / 避免与 BLE 通道互相干扰；例如只用 BLE 的场景）。 */
            { extern uint8_t key_config_bt_flags(void);
              if (key_config_bt_flags() & KEY_CONFIG_BT_CLASSIC_EN) {
                  extern void classic_hid_open(void);
                  classic_hid_open();
              } else {
                  rt_kprintf("[BT] classic DISABLED by config (slot level)\n");
                  /* SDK 在栈初始化时就 hid_enb_req 把 HID 服务打开了（日志
                   * "btsapp_hid: >> HID enabled" 早于我们的判断），所以只跳过
                   * classic_hid_open() 没用 —— 必须显式关掉，主机才找不到 HID。 */
                  { extern void bt_hid_close(void); bt_hid_close(); }
                  rt_kprintf("[BT] classic HID service closed\n");
              } }
            /* 链路策略：入向 lp_in=0x0005(role switch+sniff) 保留——PC 主动连时
             * 设备可 role switch 成 master 省功耗；出向 lp_out=0x0004 禁用 role switch，
             * 设备主动发起 ACL 时拒绝手机的 role switch 请求、保持 master，
             * 拉平"设备主动连高 0.3mA"的差异(active 模式 slave 比 master 多耗)。 */
            bt_interface_set_linkpolicy(0x0005, 0x0004);  /* lp_in:RS+sniff  lp_out:sniff only */
            bt_interface_set_sniff_enable(1);
            /* 经典蓝牙 page scan 开机即开(connectable, 不可被搜索): 设备从启动起就
             * 可被手机经典蓝牙寻呼, 不依赖 BLE 先连上 —— 否则手机在 BLE 连上前发起
             * BR/EDR page 会失败 → "连不上/连得慢"。深睡由 power.c 调 set_scan_mode(0,0)
             * 关闭以保持 0.2mA 深睡功耗。需被新设备搜索配对时显式 set_scan_mode(1,1)。
             * ⚠️ 三设备切换: 配对窗口(空槽C键/L键)期间 BR/EDR 保持 page scan 开放 ——
             * 手机等安卓设备对键盘走经典蓝牙(BR/EDR)配对, 不主动连 BLE; 若关闭 BR/EDR
             * 手机将彻底无法配对(实测日志: 手机 BLE 连接从未发生)。配对窗口内已配对
             * 设备(蓝牙1/2)的 BR/EDR 骚扰由"仅手机主动 page 键盘"天然规避(电脑不会主动
             * page), BLE 侧仍由拒绝列表拦截。手机 BR/EDR 配对成功(HFP connected)时
             * bt_multi_on_br_paired 结束配对窗口, 广播转白名单停掉蓝牙1/2 骚扰。 */
            /* ★★ 11-24 配对窗口内必须**可被发现**(inquiry scan 打开)。
             * 原来这里恒为 (0,1) = 只开 page scan：page scan 只允许【已知地址】
             * 的旧主机来寻呼，新主机必须先把设备"扫"出来才能发起配对。
             * Windows 靠 BLE 广播能看见设备，所以一直没暴露；**macOS 的配对流程
             * 是"蓝牙设置 → 搜索设备"（经典 inquiry）**，设备不可被发现 ⇒ 列表里
             * 根本没有它 ⇒ 客户反馈"Mac 上蓝牙配不上"。
             * 配对模式(BT_MODE_PAIR=1)下改成 (1,1)：可被发现 + 可被寻呼；
             * 配对完成由 bt_slot.c 的两个出口收回成 (0,1)，不长期增加功耗。 */
            { extern uint8_t key_config_bt_flags(void);
              if (!(key_config_bt_flags() & KEY_CONFIG_BT_CLASSIC_EN)) {
                  vk_set_scan_mode(0, 0);      /* 经典关闭: 不可连也不可发现 */
                  rt_kprintf("[BT] page/inquiry scan OFF (classic disabled)\n");
              } else {
              extern uint8_t bt_multi_get_mode(void);
              uint8_t m = bt_multi_get_mode();
              if (m == 1 /*BT_MODE_PAIR*/) {
                  vk_set_scan_mode(1, 1);
                  rt_kprintf("[CLS] PAIR mode -> discoverable (inquiry+page scan ON)\n");
              } else {
                  vk_set_scan_mode(0, 1);
              } } }
            /* 开机常开 page scan 用默认低活动度(省电); 配对/回连窗口由
             * page_scan_window_start 切高活动度提高寻呼命中率(08-07)。 */
            set_page_scan_activity(0);
            /* 启用 WBS(mSBC 宽带 16kHz)，禁用窄带 CVSD：
             * ① 关键修复"有时没声音"：CVSD 无 FEC，本机实测 SCO 包 ~100% packet_status 错误
             *    (rx errcnt 全满) 时直接交付静音；MSBC 带 FEC/CRC 可容忍同等错误
             *    (正常通话日志 rx errcnt 256/256 但 decode errcnt:0→可听)。手机在 CVSD/MSBC
             *    间摇摆时，协商到 CVSD 就静音→这正是"有时没声音"的根因。
             * ② 之前为规避"msbc uplink full 麦克风丢帧"曾关 WBS(旧 A 方案)，但当前固件 MSBC
             *    上行已无 uplink full(正常通话日志可见)，该取舍已翻转——CVSD 静音远比偶发
             *    丢帧严重，故改回启用 WBS。若上行丢帧重现，再单独优化 3A/uplink 通路。
             * ③ 配合 SCO_CONNECTED 的 ble_app_conn_param_sco_quiet() 降低 BLE 射频占空比，
             *    进一步减少 SCO/BLE 共存错误。须早于通话建链，故放 stack ready。
             * SDK 接口语义：bt_interface_set_wbs_status(1)=启用 msbc / (0)=禁用(强制 CVSD)。 */
            bt_interface_set_wbs_status(1);
            /* ★ 10-31: 手机只走经典蓝牙，BLE 不会来连 ⇒ 那套挂在 BLE 上的主动连
             * 永远不会触发。这里排一次"仅经典链路"的主动回连（3s 后），
             * 已配对的手机会被设备主动 page 起来，不用再在手机上点"连接"。 */
            g_cls_conn_tries = 0;
            bt_cls_autoconn_kick(3000);
            //hid_report_battery();
            break;
        case BT_NOTIFY_COMMON_ACL_CONNECTED:    // ACL 链路建立
            {
                bt_notify_device_acl_conn_info_t *acl = (bt_notify_device_acl_conn_info_t *)msg->data;
                /* ★★★ 11-17 修正：这个回调**失败也会进来**（res!=0 / handle=0xFFFF）。
                 * 典型：PC 蓝牙关掉后设备一直 page，每次 5.12s 超时，
                 * 回调带着 res=4(Page Timeout)、hdl=65535 到达。
                 * 原来无条件打印 "ACL connected"，把"寻呼超时"伪装成"已连接"，
                 * 还会把链路建立时刻记下来 ⇒ 后续判断全乱。
                 * 只有 res==0 且 handle 有效才算真的连上。 */
                if (acl->res != 0 || acl->handle == 0xFFFF || acl->handle == 0) {
                    rt_kprintf("[HID] ACL connect FAILED: dir=%d res=%d handle=0x%04X (4=page timeout)\n",
                               acl->acl_dir, acl->res, acl->handle);
                    break;      /* 失败: 不计链路时刻、不当作已连接 */
                }
                /* ★ 11-27 诊断：经典链路建起来时，协议栈的**经典配对表**里还有没有这台设备。
                 * 用例：同一台电脑配了槽1/槽2 后回槽1，若这里返回 NULL，
                 * 说明经典侧记录已被覆盖/清除 —— 与"BLE 有多条 bond、经典只有一条"吻合。 */
                {
                    bt_notify_device_base_info_t *d0 = (bt_notify_device_base_info_t *)msg->data;
                    extern uint8_t bt_multi_get_slot(void);
                    void *bd = d0 ? bt_cm_get_bonded_dev_by_addr(d0->mac.addr) : NULL;
                    rt_kprintf("[CLS BOND-DIAG] slot%d peer=%02x:%02x:%02x:%02x:%02x:%02x "
                               "classic_bond=%s\n",
                               bt_multi_get_slot(),
                               d0 ? d0->mac.addr[5] : 0, d0 ? d0->mac.addr[4] : 0,
                               d0 ? d0->mac.addr[3] : 0, d0 ? d0->mac.addr[2] : 0,
                               d0 ? d0->mac.addr[1] : 0, d0 ? d0->mac.addr[0] : 0,
                               bd ? "YES" : "NO(key lost)");
                }
                rt_kprintf("[HID] ACL connected: dir=%d (0=device-init, 1=phone-init)\n",
                    acl->acl_dir);
                /* ⚠️ 只记录"链路建立时刻"，**不能直接当作连上过**：
                 * 日志实证 `[HID] ACL connected` 每次都会出现，但 HFP/HID 紧接着失败
                 * （res:c / confirmation connect failed），几百毫秒就断。
                 * 若在这里标记 had_link，断连时就会重置 60 秒窗口 → 永远走不完。
                 * 改为记录时刻，断开时用"存活时长"判断是不是真的连上用过。 */
                g_cls_link_up_tick = rt_tick_get();
                /* 日志确认：设备发起 ACL(dir=0) 时手机会 role switch 让设备变 slave，
                 * 且手机拒绝设备 role switch 回 master 的请求(res=14 安全理由)。
                 * 设备作为 slave 在 active 模式下多耗 ~0.3mA（硬件特性）。
                 * 解决方案：避免设备发起 ACL——在 page scan 窗口等手机主动来连。
                 * ⚠️ 手机主动来连(dir=1) = 手机 BLE 初始化完成后的自发重连:
                 * 置 g_phone_acl_up → 设备所有主动连路径(mgr 线程/pscan 链)立即
                 * 短路, 防双连接并发导致认证混乱(实测日志 17: 手机来连 ACL 建立
                 * + RFCOMM 推进中, 设备 pscan 链又主动连 → granted 0 认证失败
                 * → 182s 后断开)。 */
                if (acl->acl_dir == 1) {
                    g_phone_acl_up = 1;
                    g_hfp_connect_dir = 1;   /* 手机抢先来连, HFP 将由手机建立 */
                    acl_up_watchdog_start();   /* 启动看门狗: ACL up 超时未建 HFP 则释放保护 */
                }
            }
            break;
        case BT_NOTIFY_COMMON_ACL_DISCONNECTED:  // ACL 链路断开
            {
                bt_notify_device_base_info_t *dev = (bt_notify_device_base_info_t *)msg->data;
                /* ⚠️ 认证类断开 = 手机侧密钥已变(重新配对/stale bond):
                 * 08-07 实测日志 e0-4c-23-99-87 重新配对时 auth_comp st22 →
                 * disc reason 0x22(LMP PDU Not Allowed)。此时手机正在重新配对,
                 * CTKD 派生会写入新 key(一次性事件) → 【绝不能立即删 key】——
                 * 删了设备侧永久没 key, 而 0x200000 下手机不来连 → HFP 永久
                 * 死锁(实测日志 13)。正确: 软标记 → pscan 链等 CTKD 派生新
                 * key(hfp_bt_key_valid)后自动恢复主动连。
                 * 连续 ≥3 次认证失败(手机 key 永久失效且不重配)才真删 key,
                 * 转等手机重新配对(用户重配 → CTKD 重新派生 → 恢复)。 */
                if (dev && (dev->res == 0x05 || dev->res == 0x06 || dev->res == 0x22)) {
                    g_auth_fail_cnt++;
                    if (g_auth_fail_cnt >= 3) {
                        extern void bt_cm_delete_bonded_devs_and_linkkey(uint8_t *addr);
                        bt_cm_delete_bonded_devs_and_linkkey(dev->mac.addr);
                        g_stale_bond_cleared = 1;
                        g_hfp_failed_once = 0;
                        g_auth_fail_cnt = 0;
                        LOG_I("[HID] ACL auth-fail x3 reason=0x%02x, dropped bt key, wait phone re-pair", dev->res);
                    } else {
                        g_stale_bond_cleared = 1;   /* 等 CTKD 派生新 key 后自动恢复 */
                        g_hfp_failed_once = 1;
                        LOG_I("[HID] ACL auth-fail disc reason=0x%02x (keep bt key, wait CTKD renew)", dev->res);
                    }
                }
                /* 11-27 诊断：任何 reason 都打一行，便于对照
                 * 0x19(25)=远端终止(常伴随认证不通过) / 0x05,0x06,0x22=认证类 */
                if (dev)
                    rt_kprintf("[CLS BOND-DIAG] ACL down res=0x%02x(%u), auth_fail_cnt=%d\n",
                               dev->res, dev->res, g_auth_fail_cnt);
                g_phone_acl_up = 0;   /* 手机来连的 ACL 已断开(或设备主动连的断): 恢复主动连候选 */
                acl_up_watchdog_stop();   /* ACL 断开, 看门狗无意义 */
            }
            break;
        case BT_NOTIFY_COMMON_IO_CAPABILITY_IND:  // SSP IO capability 请求(仅配对确认宏开启时派发)
            {
                /* 无屏幕设备: 立即回复 NoInputNoOutput → 手机选 Just Works 自动配对,
                 * 避免 Numeric Comparison(需确认 6 位数字)卡死(08-07 实测 274s 超时)。 */
                uint8_t *mac = (uint8_t *)msg->data;   /* 6 字节 BD_ADDR */
                BTS2S_BD_ADDR bd;
                bt_addr_convert_to_bts((bd_addr_t *)mac, &bd);
                bt_io_capability_rsp(&bd, IO_CAPABILITY_NO_INPUT_NO_OUTPUT, 0, 1);
                LOG_I("[HID] SSP io-cap reply NoIO (Just Works pairing)");
            }
            break;
        case BT_NOTIFY_COMMON_USER_CONFIRM_IND:  // Numeric Comparison 数字确认
            {
                /* 兜底: 若仍走到数字确认(手机强制/时序), 无输入能力设备自动接受。
                 * Just Works 场景不应出现, 此为最后防线。 */
                bt_notify_pair_confirm_t *info = (bt_notify_pair_confirm_t *)msg->data;
                BTS2S_BD_ADDR bd;
                bt_addr_convert_to_bts((bd_addr_t *)info->mac.addr, &bd);
                extern void sc_user_cfm_rsp(BTS2S_BD_ADDR *bd, U8 confirm);
                sc_user_cfm_rsp(&bd, 1);
                LOG_I("[HID] auto-accept numeric pairing val=%u", (unsigned)info->num_val);
            }
            break;
        case BT_NOTIFY_COMMON_SCO_CONNECTED:    // SCO连接完成事件
            {
                LOG_I("[APP] HFP HF audio_connected");
                bt_notify_device_sco_info_t *sco_info = (bt_notify_device_sco_info_t *)msg->data;
                const char *codec = (sco_info->para.air_mode == 2) ? "CVSD(8k)" :
                                    (sco_info->para.air_mode == 3) ? "mSBC/WBS(Transparent)" : "other";
                LOG_I("[HFP][DIAG] SCO_CONNECTED -> g_sco_active=1, air_mode=%d (%s), ACL forced Active ~3.8mA",
                      sco_info->para.air_mode, codec);
            g_sco_active = 1;   /* 标记通话中：RC 周期重校定时器跳过，避免扰动 LCPU/SCO */
            /* SCO 通话期间强制空中鼠标活跃: 通话前设备可能已空闲(陀螺关+LIGHT
             * 已释放), 若不恢复活跃, 通话中说话静止会被空闲状态机维持空闲 →
             * 关陀螺(鼠标移动停)+可进 DEEP(破坏 SCO 音频→麦克风无声).
             * air_mouse_start 内部 air_mouse_set_active(1), 活跃态保持 LIGHT
             * 锁, 通话中永不进空闲(见 main.c idle 判定 g_sco_active 门控).
             * 注意: 通话中 mouse_send_thread 已被 g_sco_active 抑制 HID 发包
             * (main.c 08-06 修改) —— 这里保持活跃仅为 ①防 DEEP ②陀螺热着,
             * 通话结束立即恢复跟手, 不产生 BLE 数据流抢占 SCO 射频. */
            air_mouse_start();
            /* 退出 sniff。频率完全交给 BT 协议栈的 PM_SCENARIO_AUDIO 管理——
             * 绝不在 SCO 事件里手动切频或重校准 RC（会打断 SDK 频率序列）。
             *
             * ⚠️ 通话中把 BLE 拉长到 40~80ms（sco_quiet，给 SCO 让射频）：
             * 早期实测 BLE 15ms 高频+活跃鼠标持续发包会与 SCO 上行时隙争抢
             * 射频 → SCO 发送被饿 → 上行 mSBC 缓冲满("msbc uplink full" 刷屏)
             * → 麦克风没声音。现在通话中已抑制鼠标发包(只剩空事件), 40~80ms
             * 间隔足够让射频且不饿 SCO; 语音结束后 BLE 参数协商收紧(1~5s)前
             * 鼠标仍有 12~25Hz, 无明显延迟感。SCO 断开后由 by_activity 恢复
             * 高频全速跟手。 */
            bt_addr_convert_to_bts((bd_addr_t *)g_hid_env.peer_addr, &g_sniff_bd);
            bt_exit_sniff_mode(&g_sniff_bd);
            ble_app_conn_param_sco_quiet();
            }
            break;
        case BT_NOTIFY_COMMON_SCO_DISCONNECTED: // SCO断开完成事件
            LOG_I("[APP] HFP HF audio_disconnected");
            LOG_I("[HFP][DIAG] SCO_DISCONNECTED -> g_sco_active=0 (SCO down: ACL free to Sniff, expect ~1.5mA)");
            /* ★ 先取旧值再清零(2026-10-12 死循环教训): 本事件有两个来源, 必须区分:
             *   ① 真实语音会话结束 —— g_sco_active 原为 1, 需要重新武装 eSCO 拆除;
             *   ② 我们自己拆"挂空 eSCO"引发的断开 —— 此时 g_sco_active 已是 0。
             * 曾无条件在此武装 ⇒ 拆 eSCO → << Audio disconnect → 本事件 → 再武装,
             * 自我喂养成死循环(实测每 ~22s 一轮、共 11 轮, 链路被反复顶成 Active)。 */
            uint8_t sco_was_active = g_sco_active;
            g_sco_active = 0;   /* 通话结束：允许 RC 周期重校定时器恢复 */
            g_sco_quiet_on = 0; /* 语音结束: 解除"sco_quiet 已切", 下次语音重新协商 */
            g_vf_active = 0;    /* SCO 断开, VF 会话必然结束 */
            g_vf_retry_left = 0;             /* 语音已结束, 停 BVRA=0 复查/拆 SCO 兜底 */
            if (g_vf_retry_timer) rt_timer_stop(g_vf_retry_timer);
            /* ⚠️ SCO 结束射频释放: 通话中 BLE 包被射频抢占(键盘 up/按钮可能
             * 滞留丢失, 补发也在通话中被跳过), 立即重发最后 HID 状态, 恢复
             * 对端一致性(PC 停止录音/按钮对齐)。抢在射频最空闲时刻执行。 */
            ble_hid_flush_pending();
            bt_addr_convert_to_bts((bd_addr_t *)g_hid_env.peer_addr, &g_sniff_bd);
            hfp_request_sniff_delayed(100);
            /* ★ 语音结束后重新武装一次"挂空 eSCO"拆除(2026-10-12 实测定案):
             * 语音结束时栈把 eSCO 从 0x7(音频中)退回 0x5
             * (日志 `HF device_sco_state new_sate 0x 5, old_state 0x 7`), 重新生成
             * 那条"挂空 eSCO"(栈在 HFP 建立时自动 open 的) —— 它占经典蓝牙时隙,
             * 使 BR/EDR 反复弹 Active/Sniff(电流 1.4↔3.0 横跳), 且 ACL 语音后要
             * **268~331 秒**才回得到 sniff(三版日志均复现)。
             * ⚠️ 必须用 sco_was_active 门控(见上方取值处): 拆除动作自身会引发一次
             * SCO_DISCONNECTED, 无条件武装会死循环 —— 已实测踩过。
             * 安全: 若 2.5s 内又开语音, 回调触发时 g_sco_active==1 会 SKIP, 不误拆通话 eSCO。 */
            if (sco_was_active)
                esco_idle_teardown_schedule();
            /* ★ 语音结束后恢复空闲 WFI 分频(2026-10-12 定案, 待机 +0.3mA 的根因):
             * `pm_scenario_start(AUDIO)` 把 DWCFGR.HDIV 设 1；而 `pm_scenario_stop(AUDIO)`
             * 在 SF32LB52X 上【固定设 12】(bf0_pm.c:3323)，并不恢复空闲值 60。
             * 二者写的是同一个寄存器字段(`bf0_hal_rcc.c:2509 SetDeepWFIDiv` → DWCFGR.HDIV),
             * 也正是 `apw dump` 打印的 HDIV ⇒ 语法/寄存器双向闭环。
             * 换算(据 main.c 自身注释): div 12 ≈ 20MHz、div 60 = 4MHz(base 240MHz)。
             * main.c 顶部原文"使 50ms 周期内 ~96% 空闲进 LIGHT(WFI 4MHz)" ⇒ 4MHz 才是设计空闲态。
             * ⇒ 语音后 HCPU 在 LIGHT/WFI 期间跑 20MHz 而非 4MHz(5 倍) ⇒ 实测待机 1.4-1.5 → 1.7-1.8mA。
             * 实测取证: 语音前 DWCFGR=0x0000903c(HDIV=60) → 语音后 0x0000900c(HDIV=12), 且复现。
             * ⚠️ `apw dump` 其余各项(3A/run_mode/FORCE_HP/codec/REFGEN/LPSYS_LDO)全干净,
             *    极易误判成 BT 侧问题(本工程已在这上面走过弯路)。
             * USB 在线时 main.c:104 有意持 12(CDC 时延敏感)，不可恢复；仅离线时恢复。
             * (拔线路径 main.c:122 已设 60，本条补的是"语音结束且 USB 一直离线"这条没人管的路径。) */
            ble_app_voice_end_pm_restore();
            /* 按当前活动状态恢复 BLE 参数: 活跃→tighten(7.5~15ms 跟手),
             * 空闲→relax(20~40ms 省电). 不能无条件 relax——活跃态鼠标被降
             * 到 25Hz 会掉帧(SDK 更新解决 SCO 共存后此问题凸显). */
            ble_app_conn_param_by_activity();
            /* 语音结束: 若 BLE 在语音中断开, 电脑刚结束语音最有空回连 → 恢复广播.
             * (2026-10-12: BLE 断开时已改为【立即】广播, 此处只是幂等补一次;
             *  原先的 sco_adv_defer_cancel 随延迟广播机制一并移除。)
             * BLE 未断时 wakeup_advertise 内部有 is_connected 保护, 无副作用. */
            if (!g_hid_env.is_connected) {
                LOG_I("[BLE] SCO ended, restore advertise for re-connect");
                ble_app_wakeup_advertise();
            }
            break;
        default: break;
        }
    }
    else if (msg->type == BT_NOTIFY_HID)
    {
        /* ★ 10-31 新增：主机真正把 HID 控制(PSM 0x11)+中断(0x13)通道开起来才会来这条。
         * SDK 只在【中断通道】建立/失败时才派发 PROFILE_CONNECTED/DISCONNECTED，
         * 所以这里置位就等于"键鼠真的能发了"，是判断链路的唯一可靠依据。 */
        switch (msg->event_id) {
        case BT_NOTIFY_HID_PROFILE_CONNECTED:
            {
                extern void classic_hid_set_linked(unsigned char);
                classic_hid_set_linked(1);
                g_hid_kick_tries = 0;
                g_hid_kick_pending = 0;
                g_hid_kick_warned = 0;
                g_hid_kick_last_tick = 0;
                /* ★ 10-31：手机只走经典蓝牙 ⇒ BLE 永远不连，而 IMU/编码器原先只在
                 * BLE CONNECTED 分支启动 ⇒ 手机端空中鼠标完全不可用（日志里连
                 * `[LSM] lsm_init` 都没有，按键只报 "HOLD ignored (still calibrating)"）。
                 * 这里补一条：BLE 没连、但经典 HID 通道已建 ⇒ 把鼠标链路拉起来。
                 * PC 路径(BLE 已连)完全不变，不会重复启动(air_mouse_start 内部有幂等判断)。 */
                if (!g_hid_env.is_connected) {
                    air_mouse_start();
                    enc_start();
                }
            }
            LOG_I("[CLS HID] host opened HID channels, keyboard/mouse ready");
            break;
        case BT_NOTIFY_HID_PROFILE_DISCONNECTED:
            {
                extern void classic_hid_set_linked(unsigned char);
                classic_hid_set_linked(0);
                /* 经典链路断了且 BLE 也没连 ⇒ 宿主彻底没了，关 IMU/编码器省电。
                 * BLE 还连着时不动：那条链路的生命周期由 BLE 断开事件管。 */
                if (!g_hid_env.is_connected) {
                    air_mouse_stop();
                    enc_stop();
                }
            }
            LOG_I("[CLS HID] HID channels closed");
            break;
        default: break;   /* OPEN_COMPLETE / CLOSE_COMPLETE：HID 功能开关，与链路无关 */
        }
    }
    else if (msg->type == BT_NOTIFY_HFP_HF)
    {
        switch (msg->event_id) {
        case BT_NOTIFY_HF_PROFILE_CONNECTED:    // HFP免提配置文件连接完成事件
            LOG_I("[APP] HFP connected");
            /* ★ 10-31: 记下【经典链路对端地址】。手机只走经典蓝牙、BLE 从不连
             * （g_hid_env.peer_addr 恒为全 0），发鼠标报文时需要一个真实地址，
             * 否则 SDK 查不到连接、每帧回 "[U-L]hid is not connect"。
             * 这里的事件数据里就带着对端 MAC，是唯一可靠来源。 */
            {
                bt_notify_profile_state_info_t *pi = (bt_notify_profile_state_info_t *)msg->data;
                if (pi) {
                    memcpy(g_cls_peer_addr, pi->mac.addr, 6);
                    rt_kprintf("[CLS HID] peer = %02x:%02x:%02x:%02x:%02x:%02x\n",
                               g_cls_peer_addr[5], g_cls_peer_addr[4], g_cls_peer_addr[3],
                               g_cls_peer_addr[2], g_cls_peer_addr[1], g_cls_peer_addr[0]);
                }
            }
            { extern void classic_hid_set_ready(unsigned char); classic_hid_set_ready(1); }   /* 10-31: 经典 ACL 就绪 */
            /* ★ 10-31: ACL/HFP 通了不等于 HID 通了 —— Windows 重启后不会自己
             * 重开 HID 通道(详见 hid_kick_fire 上方注释)。这里主动踢一次，
             * 主机 600ms 内没反应就按 1.5s 间隔重试，最多 4 次。 */
            g_hid_kick_tries = 0;
            g_hid_kick_warned = 0;
            g_hid_kick_last_tick = 0;
            /* ★ 11-23: ACL 已经建起来 = 主机就在那儿，之前"60 秒窗口用尽、放弃重连"
             * 的判据已经不成立。必须在这里解除 giveup，否则 kick 会被永久挡住
             * （只有按键能清），表现就是"链路连着但 HID 通道永远不重建"。 */
            if (g_cls_giveup) {
                g_cls_giveup = 0;
                rt_kprintf("[CLS HID] giveup cleared (ACL is up, host reachable)\n");
            }
            bt_classic_hid_kick_connect();
            hid_link_sync_start();     /* 11-10: 起 HID 标志自愈校正 */
            g_hfp_connected = 1;
            /* 三设备切换指示: HFP 建立成功, 熄灭模式指示(青闪结束) */
            extern void bt_multi_on_hfp_state(uint8_t hfp_on);
            bt_multi_on_hfp_state(1);
            /* ⚠️ 三设备切换: 配对窗口内 BR/EDR 连接成功(HFP) = 手机等走经典蓝牙的
             * 新设备配对完成 —— 记录地址、结束配对窗口, 广播转白名单模式,
             * 停止蓝牙1/2 的 BLE 反复抢连刷屏(它们被白名单控制器级拒绝)。 */
            if (bt_multi_is_pairing_window()) {
                bt_notify_profile_state_info_t *pinfo = (bt_notify_profile_state_info_t *)msg->data;
                if (pinfo) {
                    bt_multi_on_br_paired(pinfo->mac.addr);
                    if (g_adv_active) {   /* 配对窗口结束, 刷新广播启用白名单 */
                        app_ble_stop_advertising();
                        ble_app_advertising_start();
                    }
                }
            }
            {
                rt_tick_t now = rt_tick_get();
                LOG_I("[HID] HFP connected, total BLE->HFP %lums, winner=%s",
                      (unsigned long)((now - g_ble_conn_tick) * 1000UL / RT_TICK_PER_SECOND),
                      g_hfp_connect_dir ? "phone" : "device");
            }
            g_phone_acl_up = 0;   /* 已连上, 不再需要保护(后续断开按正常路径) */
            /* 连接成功: 复位 stale-bond 等待与失败标记(缺陷4)。若曾因认证失败置位,
             * 连接既已建立说明密钥已就绪, 无需再等手机重配。 */
            g_stale_bond_cleared = 0;
            g_hfp_failed_once = 0;
            acl_up_watchdog_stop();   /* HFP 已连, 停 ACL up 看门狗 */
            hfp_connect_retry = 0;   /* 连接成功，重置重试计数，下次断连从头来 */
            page_scan_window_stop();  /* HFP 已连上，不再需要被 page：关 page scan 省 ~0.4mA */
            /* 延迟 2s 请求 sniff：HFP 连上后还有 AT 命令交互（BRSF/CIND/CIEV 等），
             * 立即请求会被 AT 通信打断。等 AT 交互完成后再请求 sniff，
             * 确保链路进入低占空比模式（若有 SCO 则 sniff 可能不生效）。 */
            hfp_request_sniff_delayed(2000);
            /* 防御: HFP 连上无通话, 2.5s 后拆掉空闲 eSCO(见 esco_idle_teardown_cb),
             * 让 BR/EDR 稳定进 Sniff(消除 Active/Sniff 横跳). 排在 sniff 请求(2s)
             * 之后, 确保先请求 sniff 再拆 eSCO, 避免 eSCO 残留阻挡 sniff 生效. */
            esco_idle_teardown_schedule();
            break;
        case BT_NOTIFY_HF_PROFILE_DISCONNECTED: // HFP免提配置文件断开完成事件
            LOG_I("[APP] HFP disconnected");
            /* ★ 10-31: 经典链路断 ⇒ 排一次"仅经典"的主动回连（3s 后）。
             * 手机场景 BLE 不会连，原来没有任何重连触发点 ⇒ 只能人手点。 */
            g_cls_conn_tries = 0;
            {
                /* 只有链路**稳定存活超过 3 秒**才算"真的连上用过"⇒ 次数清零重新给 20 次。
                 * 建链失败那种 100~500ms 就断的，**不动计数**，让它自然用满。 */
                rt_tick_t up = (g_cls_link_up_tick && rt_tick_get() > g_cls_link_up_tick)
                             ? (rt_tick_get() - g_cls_link_up_tick) : 0;
                if (up > rt_tick_from_millisecond(3000)) {
                    g_cls_attempts = 0;
                    g_cls_giveup = 0;
                    rt_kprintf("[CLS] real link lost (%ums), attempt counter reset\n", (unsigned)up);
                }
                g_cls_link_up_tick = 0;
            }
            bt_cls_autoconn_kick(3000);
            { extern void classic_hid_set_ready(unsigned char); classic_hid_set_ready(0); }   /* 10-31: 经典 ACL 断开 */
            if (g_hid_sync_timer) rt_timer_stop(g_hid_sync_timer);   /* 11-10: 停 HID 标志校正 */
            /* ACL 断了 HID 通道必然也没了；复位踢连接的状态机，下次连上重新来 */
            {
                extern void classic_hid_set_linked(unsigned char);
                classic_hid_set_linked(0);
                g_hid_kick_tries = 0;
                g_hid_kick_pending = 0;
                if (g_hid_kick_timer) rt_timer_stop(g_hid_kick_timer);
            }
            /* ⚠️ 区分【连接尝试失败】与【已连接后断开】(08-07 死循环定案)：
             * 连接失败时 SDK 同样发本事件, 但 g_hfp_connected==0(从未成功)。
             * 实测日志: 设备主动连 → 手机拒(res 0x0C Limited Resources 或
             * 认证类) → CONN_CFM res≠0 → 本事件; 而 start_connecting 同步返回
             * 0 使 hfp_connect_timer_cb 的 retry 永不累计, 原代码此处又每次
             * 归零 → 每 ~25s 死循环主动连(attempt 恒 0, 骚扰手机+白耗电)。
             * 改为: 连接失败要累计, 达上限清 stale bonded 转 page scan 等
             * 手机重新配对, 停止骚扰性重连。 */
            {
                uint8_t was_connected = g_hfp_connected;
                uint8_t sco_was_active = g_sco_active;   /* 语音中 HFP 断 = 语音结束(08-26) */
                g_hfp_connected = 0;
                g_vf_active = 0;   /* HFP 断开, VF 会话必然结束 */
                /* ⚠️ SCO 依附经典蓝牙 ACL：HFP 断开则通话必已结束。SCO_DISCONNECTED
                 * 事件在 ACL 直接断开时可能不单独派发（栈实现/时序竞争），漏清会让
                 * g_sco_active 永久卡 1 → 鼠标发送被永久抑制(光标不动) + C1 结束语音
                 * 无效 —— "偶发鼠标失灵/通话结束不了"的根因之一(08-06 定案)。
                 * 必须在此兜底清理，并停掉 BVRA=0 复查/拆 SCO 定时器。 */
                g_sco_active = 0;
                g_vf_retry_left = 0;
                if (g_vf_retry_timer) rt_timer_stop(g_vf_retry_timer);
                if (g_esco_idle_timer) rt_timer_stop(g_esco_idle_timer);
                /* 语音中 HFP 断开(无 SCO_DISCONNECTED 派发的兜底路径): 必须恢复
                 * BLE 参数(by_activity→tighten) —— SCO_DISCONNECTED 未派发时若
                 * 不恢复, BLE 会一直停在 sco_quiet 的 40~80ms, 鼠标丢帧卡顿,
                 * 直到空闲 relax/拿起 tighten 才间接修复("语音结束后一卡一卡,
                 * 静放一会才正常"的根因, 08-26)。发送节拍由 main.c 按
                 * g_ble_cur_interval 自适应, 此处无需另行通知。 */
                if (sco_was_active) {
                    /* 与 SCO_DISCONNECTED 路径同一收尾: 语音结束恢复空闲 WFI 分频。
                     * 通话中 HFP 直接掉线时 SCO_DISCONNECTED 可能不派发, 需在此兜底,
                     * 否则 pm_scenario_stop 留在 HDIV=12 无人恢复。 */
                    ble_app_voice_end_pm_restore();
                    ble_app_conn_param_by_activity();
                }
                /* HFP/SCO 断开兜底(08-26): SCO 依附经典 ACL, HFP 断开则通话必
                 * 已结束 —— 若语音中 BLE 断开, 现在恢复(电脑有空回连).
                 * 与 SCO_DISCONNECTED 路径二选一, 谁先到谁恢复. */
                if (!g_hid_env.is_connected) {
                    LOG_I("[BLE] HFP down, restore advertise for re-connect");
                    ble_app_wakeup_advertise();
                }
                if (!was_connected && hfp_connect_timer) {
                    /* 本机发起的连接尝试被拒(从没成功过): 累计失败 */
                    hfp_connect_retry++;
                    g_hfp_failed_once = 1;   /* 失败标记: 下次窗口立即重试, 不等手机 */
                    /* ⚠️ 09-16 整改(用户实测: 待机唤醒后经典蓝牙"难连", 但【没动主机、只重启
                     * 设备就好了】)。原实现**首次失败就立刻从 BLE LTK 重派生经典 key 并写回**,
                     * 这是破坏性的: 双模主机(PC/Windows)的经典 key 常与 BLE LTK【不同源】(经典与
                     * BLE 分开配对), 那个派生值对端必然不认, 却把设备侧【原本可用/可用于回退的
                     * 原 key 覆盖成错的】→ 从此一直连不上, 只有重启设备(从 flash 重载原 key)才恢复。
                     * 这正是"重启就好"的原因。
                     * 现策略(不破坏设备侧 key):
                     *   ① 设备侧【没有】经典 key → 才从 BLE LTK 派生补齐(保留"key 自愈"初衷);
                     *   ② 有 key 却连不上       → 只做普通有界重试, **绝不覆盖 key**;
                     *   ③ 普通重试也用尽       → 转 g_stale_bond_cleared 等主机(pscan 听候),
                     *      设备侧 key 保持不动 —— 主机重配 / BLE 重连 / 重启设备 都能恢复。 */
                    if (!hfp_bt_key_valid()) {
                        if (g_hfp_rederive_cnt < HFP_REDERIVE_MAX) {
                            g_hfp_rederive_cnt++;
                            hfp_rederive_classic_key();
                            hfp_retry_schedule_delayed(HFP_REDERIVE_DELAY_MS);
                            rt_kprintf("[BLE HID] HFP failed & no bt key, re-derive from LTK (%d/%d), retry in %dms\n",
                                       g_hfp_rederive_cnt, HFP_REDERIVE_MAX, HFP_REDERIVE_DELAY_MS);
                        } else {
                            hfp_connect_retry = 0;
                            g_stale_bond_cleared = 1;
                            g_hfp_failed_once = 0;
                            rt_kprintf("[BLE HID] HFP failed x%d, still no bt key, keep bond, wait host\n",
                                       HFP_REDERIVE_MAX);
                            if (g_hid_env.is_connected) {
                                vk_set_scan_mode(0, 1);
                                uint8_t bonded = hfp_peer_is_bonded(g_hid_env.peer_addr);
                                page_scan_window_start(bonded ? 1500 : 800);
                            }
                        }
                    } else if (hfp_connect_retry < HFP_CONNECT_RETRY_MAX) {
                        /* 有 key: 普通有界重试, 期间【绝不重派生/覆盖经典 key】 */
                        hfp_retry_schedule();
                        rt_kprintf("[BLE HID] HFP conn failed (bt key present, kept), retry %d/%d\n",
                                   hfp_connect_retry, HFP_CONNECT_RETRY_MAX);
                    } else {
                        /* 有 key 但多次连不上: 转"等主机重配", 不再自动主动连。
                         * 关键: 设备侧 key 保持原值不被破坏 —— 重启设备/主机重配后即可恢复。 */
                        hfp_connect_retry = 0;
                        g_stale_bond_cleared = 1;
                        g_hfp_failed_once = 0;
                        rt_kprintf("[BLE HID] HFP failed x%d (bt key kept, not overwritten), wait host re-pair\n",
                                   HFP_CONNECT_RETRY_MAX);
                        if (g_hid_env.is_connected) {
                            vk_set_scan_mode(0, 1);
                            uint8_t bonded = hfp_peer_is_bonded(g_hid_env.peer_addr);
                            page_scan_window_start(bonded ? 1500 : 800);
                        }
                    }
                } else {
                    hfp_connect_retry = 0;   /* 已连接后正常断开: 下次回连从头计 */
                }
                hfp_connect_timer = 0;
                /* 三设备切换指示: HFP 断开但 BLE 仍在 → 恢复青色闪烁(等 HFP 重建);
                 * BLE 也断开时由 on_disconnected → start_indication 恢复蓝闪。 */
                extern void bt_multi_on_hfp_state(uint8_t hfp_on);
                bt_multi_on_hfp_state(0);
                if (g_hid_env.is_connected && !g_hfp_rederive_cnt) {
                    vk_set_scan_mode(0, 1);
                    uint8_t bonded = hfp_peer_is_bonded(g_hid_env.peer_addr);
                    /* HFP 掉线但 BLE 仍在：短窗口等 PC 重连，超时设备主动发起兜底。
                     * 原 10s/3s 过长，改为 1.5s/0.8s（同 BLE 连接策略）。
                     * ⚠️ 重派生重试路径(g_hfp_rederive_cnt>0)不开 page scan,
                     * 避免干扰延迟重试的主动连。 */
                    page_scan_window_start(bonded ? 1500 : 800);
                }
            }
            break;
        case BT_NOTIFY_HF_VOICE_RECOG_STATUS_CHANGE: {  /* 手机(AG)通知 VF 状态(AT+BVRA 指示) */
            bt_notify_ag_at_arg_t *info = (bt_notify_ag_at_arg_t *)msg->data;
            if (info && info->payload_len >= 1) {
                g_vf_active = (info->payload[0] != 0);
                LOG_I("[HFP] VF %s", g_vf_active ? "active" : "inactive");
                /* ★ 2026-10-12: VF 变 active = 语音真的开始了 ⇒ 【立即】给 SCO 让路。
                 * 实测: sco_quiet 原先只在 SCO_CONNECTED 才请求, 而"VF active → SCO 建好"
                 * 隔了 8.5s、参数协商生效又 4.6s ⇒ 语音开头 ~13s BLE 一直跑 37.5ms 抢射频
                 * ⇒ SCO 上行被饿 ⇒ `msbc uplink full` 刷屏 ⇒ **麦克风没声音**(实测症状)。
                 * 用 VF active(而不是"按下 C1")作触发点: 前者只在真正进入语音时才来, 不会
                 * 像 08-26 那版"C1 按下预切"那样误伤鼠标(main.c:2612 记载已撤)。 */
                if (g_vf_active)
                    ble_app_conn_param_sco_quiet();
                else
                    g_sco_quiet_on = 0;   /* 本次语音结束, 允许下次重新协商 */
            }
            break;
        }
        default: break;
        }
    }
    return 0;
}

static void bt_hid_service_thread_entry(void *param) {
    bt_app_notify_data_t *msg;
    while (1) {
        rt_err_t ret = rt_mq_recv(g_bt_hid_service_queue, &msg, sizeof(msg), RT_WAITING_FOREVER);
        RT_ASSERT(RT_EOK == ret);
        bt_hid_service_notify_event_handle(msg);
        if (msg) rt_free(msg);
    }
}

static rt_err_t hid_service_therad_init(void) {
    g_bt_hid_service_queue = rt_mq_create("bt_hid_q", sizeof(void *), 30, RT_IPC_FLAG_FIFO);
    RT_ASSERT(g_bt_hid_service_queue);
    rt_err_t err = rt_thread_init(&g_bt_hid_service_thread, "bt_hid_svc", bt_hid_service_thread_entry, RT_NULL, bt_hid_service_thread_stack, sizeof(bt_hid_service_thread_stack), RT_THREAD_PRIORITY_MIDDLE, 4);
    RT_ASSERT(RT_EOK == err);
    rt_thread_startup(&g_bt_hid_service_thread);
    return RT_EOK;
}

/* 覆盖 SDK weak 函数: sibles_advertising_init() 会用本函数返回值强制覆盖
 * para->adv_data.disc_mode(bf0_sibles_advertising.c:208-209), 而 compose 只在
 * disc_mode == CUSTOMIZE 时才把 flags 拼进广播 PDU。
 * 必须返回 CUSTOMIZE(而非 GEN_DISC), 否则 flags=0x02|0x08 被丢弃,
 * Windows 收不到 0x08(LE/BR-EDR 同地址) → BLE 被当独立设备 → 扫到2个同名设备。
 * 可发现性由 flags 里的 0x02(General Discoverable) 表达, 不受影响。 */
uint8_t sibles_advertising_disc_mode_get(void) { return GAPM_ADV_MODE_CUSTOMIZE; }

SIBLES_ADVERTISING_CONTEXT_DECLAR(g_app_advertising_context);

/* 广播期间持有的 PM LIGHT 锁：DEEP 下 BLE/GPIO1 不可靠唤醒，故广播(可被手机发现/
 * 连接)期间保持在 LIGHT(而非 DEEP)，确保可连接 + 按键经 GPIO1 生效；停广播才释放
 * 进 DEEP。与 air_mouse 连接态持有的 LIGHT 互不冲突(PM 引用计数)。 */
static uint8_t g_adv_pm_held = 0;

void app_ble_stop_advertising(void) {
    sibles_advertising_stop(g_app_advertising_context);
    g_adv_active = 0;
    if (g_adv_pm_held) {                 /* 停广播即释放 LIGHT，允许进 DEEP 真省电 */
        rt_pm_release(PM_SLEEP_MODE_LIGHT);
        g_adv_pm_held = 0;
    }
    rt_kprintf("[HID] BLE advertising stopped\n");
}

/* ===== 断连后广播宽限窗口 =====
 * 断连后保持广播 N 秒作为"回连窗口"：BLE 外设的回连由手机(主设备)发起，
 * 只要设备仍在广播且手机已配对(bonded)，手机会静默自动回连、无需用户操作。
 * 窗口内回连成功 → CONNECTED 事件停计时器；
 * 窗口超时仍未回连 → 停广播 → 进深睡(真省电)；
 * 任意按键 → 重启广播并重开窗口(强制可被发现的兜底路径)。 */
#define ADV_GRACE_WINDOW_MS 60000
#define ADV_BOOT_GRACE_WINDOW_MS 60000   /* 上电首次广播宽限窗口：60s（与 power.c 深睡等待对齐）*/
static rt_timer_t g_adv_grace_timer = RT_NULL;

static void adv_grace_timeout_cb(void *param)
{
    if (g_hid_env.is_connected) return;   /* 已回连，无需停 */
    app_ble_stop_advertising();
    /* 三设备切换: 进深睡前熄灭槽位指示, 避免深睡下 LED 残留点亮 */
    extern void bt_multi_led_stop_all(void);
    bt_multi_led_stop_all();
    power_bt_indicator_sleep();   /* 灭蓝灯 + 停闪烁 + 电池监控降频，释放唤醒源进深睡 */
    rt_kprintf("[HID] adv grace window expired -> stop adv, deep sleep\n");
}

/* 开/重开广播宽限窗口。window_ticks 为窗口时长（tick）：
 * 断连/按键唤醒用 ADV_GRACE_WINDOW_MS(60s)，上电首次用 ADV_BOOT_GRACE_WINDOW_MS(60s)。
 * 重建定时器以设置新周期（RT-Thread 定时器周期在创建时固定，故先删除再建）。*/
static void adv_grace_start(rt_tick_t window_ticks)
{
    if (g_adv_grace_timer) {
        rt_timer_stop(g_adv_grace_timer);
        rt_timer_delete(g_adv_grace_timer);   /* 释放旧定时器，避免不同周期叠加 */
        g_adv_grace_timer = RT_NULL;
    }
    g_adv_grace_timer = rt_timer_create("adv_grace", adv_grace_timeout_cb, RT_NULL,
        window_ticks,
        RT_TIMER_FLAG_ONE_SHOT | RT_TIMER_FLAG_SOFT_TIMER);
    if (g_adv_grace_timer) {
        rt_timer_start(g_adv_grace_timer);
    }
}

static void adv_grace_stop(void)
{
    if (g_adv_grace_timer) rt_timer_stop(g_adv_grace_timer);
}

static uint8_t ble_app_advertising_event(uint8_t event, void *context, void *data)
{
    switch (event) {
    case SIBLES_ADV_EVT_ADV_STARTED:    // 广播启动完成事件
        {
            sibles_adv_evt_startted_t *evt = (sibles_adv_evt_startted_t *)data; 
            LOG_I("[HID] ADV started, result=%d, mode=%d", evt->status, evt->adv_mode);
            break;
        }
    case SIBLES_ADV_EVT_ADV_STOPPED:    // 广播停止事件
        {
            sibles_adv_evt_stopped_t *evt = (sibles_adv_evt_stopped_t *)data;
            g_adv_active = 0;
            /* 修复 LIGHT 锁泄漏：BLE 连接时 SDK 自动停广播走此回调，
             * 不经过 app_ble_stop_advertising()，g_adv_pm_held 不会被释放。
             * 若此时已连接（非主动停广播），释放泄漏的 LIGHT 锁。 */
            if (g_hid_env.is_connected && g_adv_pm_held) {
                rt_pm_release(PM_SLEEP_MODE_LIGHT);
                g_adv_pm_held = 0;
                rt_kprintf("[HID] ADV stopped by conn, released leaked LIGHT lock\n");
            }
            LOG_I("[HID] ADV stopped, reason=%d, mode=%d", evt->reason, evt->adv_mode);
            break;
        }
    case SIBLES_ADV_EVT_REQUEST_SET_WHITE_LIST:  // 广播启动前: 填充白名单(连接过滤)
        bt_multi_set_white_list();
        break;
    default: break;
    }
    return 0;
}

static uint16_t *g_adv_appearance = NULL;
static sibles_adv_type_name_t *g_adv_rsp_name = NULL;
static sibles_adv_type_srv_uuid_t *g_adv_uuid = NULL;
/* 当前广播参数里已配置的白名单开关(与 bt_multi_white_list_enabled() 比较,
 * 决定断连唤醒时是快速重启广播还是重新 init —— 重复 init 会反复创建新的
 * 广播活动(adv set), 多次后控制器状态冲突导致广播启动失败(0x43 实测)。 */
static uint8_t g_adv_wl_configured = 0;

void ble_app_advertising_start(void)
{
    /* ★★ 11-26 生命周期修复（配合 OTA 后 bus fault on KE_EVT2 排查）
     * 症状：OTA 后开机广播时 hard fault，pc 在 memcpy，调用者
     *       gapm_adv_send_hci_le_set_ext_adv_data_cmd，写向 0x4009AEFC
     *       （BT_MAC 外设段）——典型的"指针被野掉"。
     * 成因：下面两个对象都在**函数栈/堆**上，而 SDK 的 init 是异步消费
     *       （真正的数据下发发生在 KE_EVT2 任务里，函数早已返回）：
     *   ① para 是栈变量：若 SDK 保存了 &para，返回后栈被复用，
     *      里面的指针字段就变成上一次占用者的残留(常常是外设寄存器地址)
     *   ② dev_name 用完即 free：若 SDK 只存指针不拷贝，立刻悬空
     * 处理：para 改静态并每次清零；设备名缓冲改静态、不再 free。
     *       代价是几十字节 BSS，换掉一整类"偶发野指针"故障。 */
    static sibles_advertising_para_t para;
    static ble_gap_dev_name_t *dev_name = RT_NULL;
    rt_memset(&para, 0, sizeof(para));

    if (!dev_name) {
        dev_name = malloc(sizeof(ble_gap_dev_name_t) + strlen(local_name) + 1);
        if (!dev_name) { LOG_E("[HID] malloc dev_name failed"); return; }
    }
    dev_name->len = strlen(local_name);
    memcpy(dev_name->name, local_name, dev_name->len);
    ble_gap_set_dev_name(dev_name);
    /* 不 free：SDK 可能异步引用，见上方注释 */
    para.own_addr_type = GAPM_STATIC_ADDR;
    para.config.adv_mode = SIBLES_ADV_CONNECT_MODE;
    para.config.mode_config.conn_config.duration = 0x0;
    para.config.mode_config.conn_config.interval = 0xA0;  /* 100ms (0xA0×0.625ms)，省电 */
    para.config.max_tx_pwr = 0x7F;
    /* 阶段1低功耗：关闭 SDK 广播自动重启。原本 =1 时 SDK 自己订阅
     * BLE_GAP_DISCONNECTED_IND 会抢先重启广播，导致 app 层 stop 无效、
     * 断连后广播永不停、无法深睡。改 =0 后广播启停完全由 app 控制：
     * 断连保持广播 ADV_GRACE_WINDOW_MS 供已配对手机静默回连，超时才停。*/
    para.config.is_auto_restart = 0;
    /* 三设备切换: 连接模式且当前槽位已配对 -> 开启白名单过滤,
     * 只允许该槽位设备连接(选蓝牙2不会被蓝牙1的电脑连上)。
     * 开启后广播启动前会收到 SIBLES_ADV_EVT_REQUEST_SET_WHITE_LIST
     * 事件, 由 ble_app_advertising_event 里调 bt_multi_set_white_list 填充。 */
    para.config.white_list_enable = bt_multi_white_list_enabled();
    /* 必须用 CUSTOMIZE 模式: SDK 的 sibles_advertising_data_compose() 只在
     * disc_mode == GAPM_ADV_MODE_CUSTOMIZE 时才把 flags 拼进广播 PDU;
     * GEN_DISC 分支只记 disc_mode、直接丢弃 flags(实测 Windows 收不到 0x08,
     * 把 BLE 当独立设备 → 与经典蓝牙各列一条 = 扫到2个同名设备)。 */
    para.adv_data.disc_mode = GAPM_ADV_MODE_CUSTOMIZE;
    /* 双模(LE + BR/EDR)设备: flags 不能置 0x04(BR/EDR Not Supported), 否则 Windows
     * 会把 BLE 当作独立 LE 设备, 与经典蓝牙(HFP)各列一条同名记录(即"扫到2个VibeKey-F3")。
     * 正确写法: 0x02(LE General Discoverable) + 0x08(Controller 同时支持 LE/BR/EDR,
     * 即 LE 与经典蓝牙共享同一 BD_ADDR), 让 Windows 把两者合并为单个设备。 */
    para.adv_data.flags = (uint8_t)(0x02 | 0x08);
    if (!g_adv_appearance) g_adv_appearance = rt_malloc(sizeof(uint16_t));
    para.adv_data.appearance = g_adv_appearance;
    if (para.adv_data.appearance) { uint16_t a = 0x03C0; memcpy(para.adv_data.appearance, &a, 2); }
    if (!g_adv_uuid) g_adv_uuid = rt_malloc(sizeof(sibles_adv_type_srv_uuid_t) + 2);
    para.adv_data.completed_uuid = g_adv_uuid;
    if (para.adv_data.completed_uuid) {
        para.adv_data.completed_uuid->count = 1;
        para.adv_data.completed_uuid->uuid_list[0].uuid_len = 2;
        uint16_t uuid_hids = ATT_SVC_HID;
        memcpy(para.adv_data.completed_uuid->uuid_list[0].uuid.uuid_16, &uuid_hids, 2);
    }
    /* 扫描响应携带完整设备名: ble_gap_set_dev_name() 只写 GAP 设备名(供连接后
     * GATT 查询), 不会自动进广播/扫描响应; 不设这里 BLE 广播侧就没有名字字段,
     * Windows 显示的名字只能靠 BR/EDR 侧或 MAC 兜底。 */
    if (!g_adv_rsp_name) g_adv_rsp_name = rt_malloc(sizeof(sibles_adv_type_name_t) + rt_strlen(local_name));
    para.rsp_data.completed_name = g_adv_rsp_name;
    if (para.rsp_data.completed_name) {
        para.rsp_data.completed_name->name_len = rt_strlen(local_name);
        rt_memcpy(para.rsp_data.completed_name->name, local_name, para.rsp_data.completed_name->name_len);
    }
    para.evt_handler = ble_app_advertising_event;
    uint8_t ret = sibles_advertising_init(g_app_advertising_context, &para);
    if (ret == SIBLES_ADV_NO_ERR) {
        /* 记录本次 init 配置的白名单开关, 供断连唤醒时判断是否需要重新 init */
        g_adv_wl_configured = para.config.white_list_enable;
        sibles_advertising_start(g_app_advertising_context);
        g_adv_active = 1;
        if (!g_adv_pm_held) {            /* 广播期间持 LIGHT，保持在可连接态(防 DEEP 失联) */
            rt_pm_request(PM_SLEEP_MODE_LIGHT);
            g_adv_pm_held = 1;
        }
        LOG_I("[HID] BLE Advertising started, name=%s", local_name);
        /* 上电首次广播开 60s 宽限窗口：长期不连则停广播进深睡（避免冷启动
         * 一直广播耗电）；期间连上由 CONNECTED 事件 adv_grace_stop 取消计时。*/
        adv_grace_start(rt_tick_from_millisecond(ADV_BOOT_GRACE_WINDOW_MS));
    } else {
        LOG_E("[HID] Advertising init failed, ret=%d", ret);
    }
}
/* 断连后按键唤醒再广播（阶段1低功耗配套）
 * 仅当未连接且当前未在广播时，重新启动广播让设备可被手机发现。
 * SDK 文档明确：stop 之后无需重新 init，直接 sibles_advertising_start 即可；
 * 若返回非成功（极少见，如状态机不一致），兜底完整重新初始化。 */
/* 09-13 连接态待机: 主动断 BLE 时抑制断开事件里的"自动重广播"。
 * 深睡待机必须真正断掉 BLE 链路, 否则小核(LCPU)会一直按连接间隔维持链路
 * (射频活动 + 周期唤醒) → 待机电流 ~0.3mA 而非 0.1mA(实测)。断开事件默认会
 * 重启广播给主机回连窗口, 但进待机不能广播, 故用本标志屏蔽之。 */
static volatile uint8_t g_standby_teardown = 0;


/* 供 power.c 在进连接态待机前调用: 标记"待机拆除中" + 主动断开当前 BLE 链路。
 * 断开事件(will 走 BLE_GAP_DISCONNECTED_IND)负责 air_mouse_stop/enc_stop/
 * 清补发/清 g_bt_connected 等收尾, 但不会重启广播(被 g_standby_teardown 抑制)。 */
void app_ble_teardown_for_standby(void)
{
    g_standby_teardown = 1;
    if (!g_hid_env.is_connected)
        return;
    ble_gap_disconnect_t d;
    d.conn_idx = g_hid_env.conn_idx;
    d.reason   = 0x16;   /* Connection Terminated By Local Host */
    ble_gap_disconnect(&d);
}

void ble_app_wakeup_advertise(void)
{
    g_standby_teardown = 0;   /* 退出待机/正常唤醒: 恢复正常广播策略 */
    /* ★ 11-24 该槽位关掉了 BLE ⇒ 不广播。
     * 所有广播入口(POWER_ON / 唤醒 / 断连重播)都收敛到本函数，所以一处判断即可。
     * ⚠️ 关 BLE 后配置通道只剩 USB —— UI 必须提示用户。 */
    { extern uint8_t key_config_bt_flags(void);
      if (!(key_config_bt_flags() & KEY_CONFIG_BT_BLE_EN)) {
          static uint8_t warned = 0;
          if (!warned) { warned = 1; rt_kprintf("[BT] BLE advertising DISABLED by config\n"); }
          return;
      } }
    if (g_hid_env.is_connected) return;   /* 已连接，无需广播 */
    /* ⚠️ BLE 栈未就绪(上电早期, 短按松开即触发按键事件)时绝不能碰广播:
     * sibles_advertising_* 在栈未初始化时调用会取到 NULL 信号量
     * → rt_sem_take 断言崩溃(实测"看到灯松开就崩")。广播统一由
     * ble_hid_do_init(POWER_ON_IND / 温启动兜底)在就绪后启动。 */
    if (!ble_hid_is_ready()) return;
    if (!g_adv_active) {                   /* 广播已停(深睡后)：重启 */
        /* 三设备切换: 仅当白名单需求与当前广播参数不一致时才重新 init
         * (例如配对完成后切连接模式), 否则走 SDK 快速重启 —— 避免反复
         * init 创建多个广播活动导致控制器状态冲突、广播启动失败(0x43)。 */
        if (bt_multi_white_list_enabled() != g_adv_wl_configured) {
            ble_app_advertising_start();
        } else {
            uint8_t ret = sibles_advertising_start(g_app_advertising_context);
            if (ret == SIBLES_ADV_NO_ERR) {
                g_adv_active = 1;
                if (!g_adv_pm_held) {        /* 重新广播同样持 LIGHT，恢复到可连接态 */
                    rt_pm_request(PM_SLEEP_MODE_LIGHT);
                    g_adv_pm_held = 1;
                }
                LOG_I("[HID] advertising restarted");
            } else {
                LOG_W("[HID] adv restart ret=%d, fallback full init", ret);
                ble_app_advertising_start();
            }
        }
    }
    /* 唤醒/广播期：恢复蓝色指示闪烁 + 电池监控节拍（对应 sleep 的反向操作）。*/
    power_bt_indicator_active();
    /* 三设备切换: 唤醒重开广播时恢复当前槽位指示(等待回连蓝闪) */
    extern void bt_multi_start_indication(void);
    bt_multi_start_indication();
    /* 无论广播原本是否已开，都(重)开宽限窗口：
     * 用户在按键说明在用，多给一段回连时间；超时未回连再停广播深睡。
     * 断连/按键走 60s 窗口（ADV_GRACE_WINDOW_MS）。*/
    adv_grace_start(rt_tick_from_millisecond(ADV_GRACE_WINDOW_MS));
    /* ★ 10-31: 按键唤醒 = 用户现在就要用 ⇒ 若连经典链路也没连，300ms 后主动 page
     * 已配对的手机/电脑（手机场景没有 BLE 回连，不主动发起就只能人手点"连接"）。
     * bt_cls_autoconn_kick 内部会判 bt_host_linked()，已连上则什么都不做。 */
    /* ⚠️ 这里**不能**用 _user 版本：`ble_app_wakeup_advertise()` 在每次 BLE 断开
     * 都会被调用（日志里 "[BLE] HFP down, restore advertise for re-connect"），
     * 用 user 版本会把重连计数清零 ⇒ 20 次永远用不完（实测就是这样循环的）。
     * 真正的"用户按键"由 main.c 的按键分发处调 bt_cls_autoconn_user_intent()。 */
    bt_cls_autoconn_kick(300);
}

/* (2026-10-12) 原"语音中 BLE 断开延迟广播 + 30s 软定时器兜底"实现已在此删除,
 * 改为 BLE_GAP_DISCONNECTED_IND 里立即 ble_app_wakeup_advertise()。理由见文件前部注释。 */

#ifdef FINSH_USING_MSH
#include <finsh.h>
/* 测试用：手动重启广播（断连后调用以验证按键唤醒路径）*/
static int adv_start(void)
{
    ble_app_wakeup_advertise();
    return 0;
}
MSH_CMD_EXPORT(adv_start, restart BLE advertising (for testing))

/* ★ 2026-10-12: 手动拆一次"挂空 eSCO"，用于语音后 A/B 测电流（一步定生死）。
 *
 * 背景：语音后 +0.3mA 排查到目前——HDIV 修好无效、bandgap(apw bg)无效、
 * `apw diff` 证明 HCPU 侧所有**使能位**语音后全是关的(10 个变化项全是分频/音量/
 * 延时选择等纯配置位，无害) ⇒ **问题不在 HCPU 可见的音频寄存器里，在 LCPU/控制器侧**。
 *
 * 首要假设(最早的判断，中途被一个弱证据误否，现重新启用)：
 *   语音结束时栈把 eSCO 从 0x7(音频中)退回 0x5
 *   (日志 `HF device_sco_state new_sate 0x 5, old_state 0x 7`)，重新生成一条
 *   "挂空 eSCO"(栈在 HFP 建立时自动 open 的那条) —— 它占经典蓝牙时隙，
 *   既让 ACL 语音后 **268s** 才回到 sniff(两次日志均实测到)，也让小核持续醒着。
 *   与 +0.3mA 的关联：`power.c:1581` 判据 "lcpu_sleep=0 ⇒ 电流必在 mA 级"，
 *   而 `PMUC.LPSYS_LDO/SWR.RDY` 语音前稳定 0、语音后偏 1(=LP 域在供电)，
 *   正是 `power.c:1447` 记录的"语音残留"签名。
 *
 * 用法：语音后电流 1.7-1.8mA 时敲 `hfp_teardown`，看电流是否掉回 1.4-1.5mA。
 *   · 掉了 ⇒ 实锤。接着把 re-arm 接回 SCO_DISCONNECTED，**必须带 sco_was_active 门控**
 *          （否则拆除动作自身引发的 SCO_DISCONNECTED 会再武装 → 自反馈死循环，已踩过）。
 *   · 没掉 ⇒ eSCO 排除，转向 LCPU 侧(用 lpstat 连续观察 lcpu_sleep)。 */
static int hfp_teardown(void)
{
    int ret;
    rt_kprintf("[HFP] manual teardown: g_hfp_connected=%d g_sco_active=%d g_vf_active=%d\n",
               g_hfp_connected, g_sco_active, g_vf_active);
    ret = bt_interface_audio_switch(1);   /* 1 = disconnect audio(eSCO) */
    rt_kprintf("[HFP] bt_interface_audio_switch(1) ret=%d (0=submitted)\n", ret);
    return 0;
}
MSH_CMD_EXPORT(hfp_teardown, tear down HFP audio(eSCO) once - A/B current test);
#endif

/* HFP timer（定义见文件前部：hfp_connect_timer_cb / hfp_*_schedule） */


/* ===== 连接态动态连接参数 (空闲优化 C) =====
 * 活跃(使用中): 低延迟 7.5~15ms, 保证空中鼠标跟手.
 * 空闲(静止): 放宽间隔 + slave latency, 大幅减少射频活动省电.
 * 由 main.c 的空闲/活跃状态机调用 (g_hid_env.conn_idx 为当前连接句柄).
 *
 * ⚠️ 节流(08-06 排查"蓝牙有害行为"): conn param update 需手机协商 1~2s 才生效,
 * 而空闲/活跃状态机切换最快 ~0.96s 一次(快速拿起放下设备) → 无节流时协商风暴、
 * 参数永远滞后。tighten/relax 共享 2s 节流窗; sco_quiet 不走节流(SCO 事件频率
 * 低且必须立即生效, 见下)。 */
#define CONN_PARAM_THROTTLE_MS 2000
static rt_tick_t g_last_conn_param_tick = 0;
static void conn_param_throttle_enter(void)
{
    g_last_conn_param_tick = rt_tick_get();
}
static int conn_param_throttled(void)
{
    return (rt_tick_get() - g_last_conn_param_tick)
           < rt_tick_from_millisecond(CONN_PARAM_THROTTLE_MS);
}

void ble_app_conn_param_tighten(void)
{
    if (!g_hid_env.is_connected) return;
    if (conn_param_throttled()) return;   /* 2s 内刚更新过, 跳过(协商风暴防护) */
    conn_param_throttle_enter();
    ble_gap_update_conn_param(BLE_GAP_CREATE_UPDATE_CONN_PARA(
        g_hid_env.conn_idx, 6, 12, 0, 500));
    LOG_I("[BLE] conn param tightened (active: 7.5~15ms)");
}

void ble_app_conn_param_relax(void)
{
    if (!g_hid_env.is_connected) return;
    if (conn_param_throttled()) return;   /* 2s 内刚更新过, 跳过(协商风暴防护) */
    conn_param_throttle_enter();
    /* 空闲态连接参数: interval **40~64(50~80ms)**, slave_latency 0, timeout 600(6s).
     *
     * ★★★ 11-06 调整: 原为 16~32(20~40ms)。原因是实测"进 Sniff 后电流在
     * 1.4 / 2.7 / 3.1mA 之间反复横跳"—— 1.4mA 是经典蓝牙 sniff 的底噪，
     * 而 **latency=0 ⇒ 手机每个连接事件从机都必须应答**，每 20~40ms 就有一次
     * 射频唤醒（≈1~2ms、数 mA），叠加在底噪上就成了这种脉冲式横跳。
     * 把间隔拉到 50~80ms，唤醒次数直接减半以上，平均电流显著下降。
     *
     * ⚠️ slave_latency 仍然必须为 0（不能靠它省电）：
     *   旧实现用 latency 4（可跳过 4 个连接事件），最坏每 5×100ms=500ms 才送达一次
     *   HID 报告。空中鼠标每 10ms 累积一帧位移，连接事件到来时控制器把 500ms 内
     *   攒的多帧一次性灌出 → 光标"跳变"；且 tighten 要等手机协商 1~2s 才生效，
     *   这期间持续跳变 → 用户感知"空闲→活跃卡顿跳变，过一两秒才正常"。
     *   ⇒ 用"拉长间隔"省电，不用 latency —— 前者只影响首帧延迟(50~80ms, 基本无感)，
     *     后者会造成可见的跳变。
     *
     * 数值换算: interval 单位 1.25ms ⇒ 40 = 50ms, 64 = 80ms。
     * timeout 600 = 6s，远大于 (1+latency)×interval×2 = 160ms，满足规范约束。 */
    ble_gap_update_conn_param(BLE_GAP_CREATE_UPDATE_CONN_PARA(
        g_hid_env.conn_idx, 40, 64, 0, 600));
    LOG_I("[BLE] conn param relaxed (idle: 50~80ms, latency 0)");
}

/* 空闲深睡: 强制放松 BLE 连接间隔(绕过 2s 节流), 削 BLE 射频基线功耗.
 * 由 main.c 空中鼠标进入空闲态(air_mouse_set_active idle 分支)调用一次.
 * 拿起/移动时由 air_mouse_set_active(active) 内部 ble_app_conn_param_tighten
 * 恢复跟手(协商 1~2s 延迟, 可接受). 与 SCO 期间 sco_quiet 不同, 此处仅在
 * 【无通话空闲】生效, latency 保持 0 防光标跳变. */
void ble_app_conn_param_relax_idle(void)
{
    if (!g_hid_env.is_connected) return;
    /* 绕过 2s 节流: 空闲意图应立刻生效. 否则会被进入空闲前不久那次
     * tighten(拿起/连接时的低延迟请求)挡在 2s 节流窗外, 导致 relax 不执行,
     * 空闲态 BLE 仍钉在 7.5~15ms. 进入空闲 = 已停止移动 ≥0.96s, 不存在
     * "刚 tighten 又立刻 relax 的协商风暴"风险. */
    g_last_conn_param_tick = 0;
    ble_app_conn_param_relax();
}

/* 按当前活动状态恢复 BLE 连接参数 (SCO 断开/HFP 恢复时调用):
 * 空中鼠标活跃 → tighten(7.5~15ms 跟手); 空闲 →【不 relax】保持当前参数。
 * 背景: SCO 断开后若无条件 relax, 活跃态鼠标被降到 25Hz 报告 → 快速移动
 * 掉帧。参数更新需手机协商 1~2s, 因此必须一开始就按活跃态收紧。
 * 空闲分支不 relax 的原因见 main.c air_mouse_set_active(08-07 定案):
 * 空闲 relax(20~40ms) 后"拿起即用"需协商 1~2s 才收紧, 期间帧率 25~50Hz
 * → 移动一卡一卡(掉帧); 空闲省电由 IMU 降频 + LIGHT 锁释放承担。 */
extern int air_mouse_is_active(void);
void ble_app_conn_param_by_activity(void)
{
    /* 紧急恢复路径(SCO 断开/语音结束): 绕过 2s 节流立即恢复跟手参数,
     * 避免短通话(<2s)后鼠标仍停留在 sco_quiet 的 40~80ms。 */
    g_last_conn_param_tick = 0;
    if (air_mouse_is_active())
        ble_app_conn_param_tighten();
}

/* 通话(SCO)期间 BLE 参数：尽量安静以避开与 SCO 争抢同一 2.4GHz 射频。
 * BLE 与经典蓝牙 SCO 语音流共用片上射频；SCO 是实时流、对丢包极敏感。
 * 通话中鼠标发包已被抑制(mouse_send_thread g_sco_active 门控) → BLE 只剩
 * 空事件, 占射频极短 → 间隔不必像早期(有数据时)那样拉到 100~200ms。
 * 40~80ms 折中: ① 空事件频率低, 不饿 SCO 上行(uplink full 根因规避);
 * ② 语音结束瞬间 BLE 参数协商收紧(1~5s)前, 鼠标报告仍有 12~25Hz,
 *    跟手性远好于 100~200ms(5~10Hz) → 消除"语音结束后鼠标有点延迟"。
 * 监督超时 3200(32s) 保持: 低频期容忍整段通话射频饥饿防断连。 */
void ble_app_conn_param_sco_quiet(void)
{
    if (!g_hid_env.is_connected) return;
    /* ★ 2026-10-12: 一次语音只协商一次(见 g_sco_quiet_on 注释)。 */
    if (g_sco_quiet_on) return;
    g_sco_quiet_on = 1;
    /* 11-04: 按用户要求恢复为 40~80ms(32~64)，给 SCO 让更多射频余量。
     * 历史: 10-30 曾改成与空闲一致的 20~40ms(16~32)，理由是实测主机不按
     * 32~64 下发(applied 一直停在 30=37.5ms)，那版意图从未真正生效。
     * ⚠️ 若这次观察到 applied 仍停在 30 左右，说明主机(手机/PC)自身限制，
     *    不是固件没请求 —— 以日志 `[BLE] conn param applied:` 为准。
     * timeout 仍保留 3200(32s): 它才是真正生效、且防 reason 8 断连的那一项。 */
    ble_gap_update_conn_param(BLE_GAP_CREATE_UPDATE_CONN_PARA(
        g_hid_env.conn_idx, 32, 64, 0, 3200));
    LOG_I("[BLE] conn param SCO-quiet (40~80ms, to 32s)");
}

static int ble_hid_event_handler(uint16_t event_id, uint8_t *data, uint16_t len, uint32_t context)
{
    switch (event_id) {
    case BLE_POWER_ON_IND:  // 电源开启指示事件
        if (g_hid_env.mb_handle) rt_mb_send(g_hid_env.mb_handle, BLE_POWER_ON_IND);
        break;
    case BLE_GAP_CONNECTED_IND: {   // BLE GAP（通用访问配置文件）连接事件
        ble_gap_connect_ind_t *ind = (ble_gap_connect_ind_t *)data;
        g_hid_env.conn_idx = ind->conn_idx;
        g_hid_env.is_connected = 1;
        g_ble_cur_interval = 40;   /* 新连接参数未知: 置保守 50ms, 首次参数协商事件修正 */
        g_hid_env.kb_ntf_enabled = 1;
        g_hid_env.ms_ntf_enabled = 1;
        g_hid_env.bas_ntf_enabled = 1;
        memcpy(g_hid_env.peer_addr, ind->peer_addr.addr, 6);
        rt_kprintf("[BLE HID] connected idx=%d\n", ind->conn_idx);
        bt_dump_bonds("BLE connected");
        ble_app_sync_classic_key("ble connected");

        /* ★ 11-26 最终定案（用户选择）：配对模式对老设备**不打断**。
         * 曾经试过两版"拒绝/清密钥"方案，都会让老设备被踢一次再重配，用户不接受。
         * 现在配对模式只做两件事：① 广播关白名单（任何设备可连）② 可被发现。
         * 已配对设备连上来照常使用；要重置配对请在主机端删除设备，或换一个槽位。 */
        /* 三设备切换(方案B): 不再有"配对模式拒绝已配对设备"逻辑 —— 每槽独立
         * BD_ADDR, 其他地址下的设备看不见本槽广播, 不存在抢配; 任何设备都可
         * 连接(已配对→直接连/回连, 未配对→走 create_bond/等配对)。 */
        /* 三设备切换: 记录当前槽位地址 + 熄灭槽位指示 LED */
        bt_multi_on_connected();
        /* 设备侧无 bond(设备 KVDB 被清/首次连接/主机密钥 stale): 已配对主机回连
         * 会因加密找不到 LTK 被断开(reason 19)。⚠️ 不主动 create_bond —— Windows
         * 拒绝设备主动重配(op10 GAPC_BOND ret66), 且延迟定时器会干扰 Windows
         * 正常发起的配对流程(两次配对冲突 → 槽2 反复断连实测)。
         * 正确做法: 交主机侧处理(主机删除旧设备后重新配对, 由主机发起)。 */
        if (!hfp_peer_is_bonded(g_hid_env.peer_addr)) {
            rt_kprintf("[BLE HID] not bonded (device key missing): "
                       "re-pair from host (delete device on host & pair again)\n");
        }
        power_set_bt_connected(1);
        adv_grace_stop();    /* 已(回)连成功，取消宽限窗口计时，避免 60s 后误停广播 */
        air_mouse_start();   /* 阶段1：连接即启动 IMU；内部 air_mouse_set_active(1) 负责
                                持有 LIGHT 锁 + 收紧连接参数(空闲优化 A/C/B 的活跃态) */
        enc_start();         /* 连接即启动编码器轮询（断连时 enc_stop 已停），解封此前睡眠阻断 */
        g_ble_conn_tick = rt_tick_get();   /* 计时起点: BLE 连上 */
        g_hfp_connect_dir = 0;             /* 默认设备抢发; 手机若先来连(ACL dir=1)会被改写 */
        /* HFP 连接策略(08-22 整改): 设备永远【抢先】主动发起, 抢在手机前面
         * 把 HFP 建好(详见 hfp_mgr 线程)。代价: 设备发起 ACL(dir=0) → 被切
         * slave → 比 master 多耗 ~0.3mA(硬件特性); 为换取 1-2s 快连, 接受此
         * 额外功耗。手机若抢先来连(dir=1)由 g_phone_acl_up 守卫, 设备不插手。
         * 首次配对(未 bonded)设备无 key, mgr 不唤醒, 只等手机 CTKD 派生后来连。 */
        /* 三设备切换: 配对窗口内(新设备 BLE 已连、配对尚未完成)经典蓝牙保持关闭,
         * 防已配对设备趁 page scan 从 BR/EDR 抢连 HFP; 配对成功后恢复(PAIRING_SUCCEED)。 */
        /* ★ 11-26 配对模式保持**可被发现 + 可被寻呼**(1,1)：
         * 既然改成"只等对端主动连"，就必须让对端既搜得到也连得上。
         * 原实现这里写死 (0,0)，等于把门关上了 —— 设备只能自己往外连，
         * 正是"反复主动连 → 弹配对确认 → 卡死"循环的一部分。
         * 方案B 下每槽独立 BD_ADDR，别的槽设备 page 不到本地址，
         * 因此不必再用"关扫描"来防抢连。 */
        if (bt_multi_is_pairing_window())
            vk_set_scan_mode(1, 1);     /* 配对: 可发现 + 可连接 */
        else
            vk_set_scan_mode(0, 1);     /* 正常: 仅可连接 */
        {
            uint8_t bonded = hfp_peer_is_bonded(g_hid_env.peer_addr);
            uint8_t boot_first = !g_boot_first_connect;
            /* page scan 等待窗口: 仍开高活动度 page scan 给手机一个快速主动连
             * 的机会(若手机抢先则 g_phone_acl_up/g_hfp_connected 互斥); 超时由
             * pscan 链主动连兜底。实际 HFP 主动连主要由 hfp_mgr 线程在 BLE 连上
             * 即抢发(见上方策略), 本窗口仅作兜底/功耗友好的监听。*/
            uint32_t wait_ms = boot_first ? 800 : (bonded ? 1500 : 800);
            g_boot_first_connect = 1;
            g_wait_phone_ms = 0;   /* 新连接重新计时(已配对: 先等手机再主动连) */
            g_ble_param_updated = 0;  /* 等手机 BLE 参数协商完成信号 */
            g_hfp_failed_once = 0;  /* 新会话重置失败标记 */
            g_stale_bond_cleared = 0;  /* 新会话重置(已重新配对成功/未清过) */
            g_auth_fail_cnt = 0;    /* 新会话重置认证失败计数 */
            /* ⚠️ 已配对设备【主动连交给 hfp_mgr 线程】(08-07 定案):
             * 安卓对 HID 设备 BR/EDR 多数不主动来连 → 需设备主动连兜底。
             * 但【绝不能在事件线程里等】(实测日志 15: 事件线程 mdelay 轮询
             * → 蓝牙事件分发瘫痪, BLE 配对/MTU/服务发现全卡死)。
             * 这里只发信号量唤醒 hfp_mgr 线程, 立即返回——该线程负责等
             * BR/EDR key 就绪(CTKD 派生)后主动连(正常重启 key 在 → 秒连;
             * 重配场景等新 key → 用新 key 认证必过), 线程内 mdelay 不阻塞
             * 事件分发。与 pscan 链兜底互斥: hfp_connect_timer 闩锁。 */
            if (bonded && !g_hfp_failed_once && !g_stale_bond_cleared
                && !bt_multi_is_pairing_window()) {
                if (g_hfp_mgr_sem)
                    rt_sem_release(g_hfp_mgr_sem);   /* 唤醒 mgr 线程(非阻塞) */
            }
            rt_kprintf("[HID] BLE connected, %s, %s, page scan wait %ums%s\n",
                bonded ? "bonded" : "not bonded",
                boot_first ? "boot-first" : "reconnect",
                wait_ms,
                bonded ? " (wait bt key then active-conn)" : "");
            page_scan_window_start(wait_ms);
        }
        /* 注: LIGHT 锁与连接参数由 main.c 的空闲/活跃状态机统一持有/切换,
         * 此处不再直接 request/release, 避免重复计数导致 DEEP 无法进入. */
        break;
    }
    case BLE_GAP_UPDATE_CONN_PARAM_IND: {  // 手机 BLE 连接参数协商完成
        /* 跟踪当前连接间隔(×1.25ms): main.c 发送线程据此自适应节拍, 防止
         * 语音期/宽间隔下 10ms 出包耗尽 TX 池丢帧(08-26)。无条件记录协商
         * 结果(无论收紧还是放宽), 发送线程实时自适应。 */
        {
            ble_gap_update_conn_param_ind_t *pind = (ble_gap_update_conn_param_ind_t *)data;
            if (pind && pind->con_interval)
            {
                g_ble_cur_interval = pind->con_interval;
                g_ble_cur_supvto = pind->sup_to;
                /* 打出来。判读要点: 语音中 `[BLE] conn param SCO-quiet` 之后若**没有**
                 * 本行, 说明 40~80ms/32s 那组参数没被手机确认 ⇒ 链路仍在用旧短超时
                 * ⇒ 长语音(>32s)必被 SCO 饿断(reason=8)。实测第一段 52s 语音正是如此。 */
                LOG_I("[BLE] conn param applied: interval=%u(%u.%02ums) latency=%u sup_to=%u(%u.%02us)",
                      pind->con_interval,
                      (unsigned)(pind->con_interval * 5 / 4), (unsigned)(pind->con_interval * 5 % 4 * 25),
                      pind->con_latency,
                      pind->sup_to,
                      (unsigned)(pind->sup_to / 100), (unsigned)(pind->sup_to % 100));
            }
        }
        /* 设备重启后手机回连 BLE: 连接→加密→MTU→服务发现→参数协商(~13s)
         * 期间手机无暇响应 BR/EDR page(实测设备主动连撞上 → ACL 建立被拖
         * 67s)。参数协商完成 = 手机 BLE 初始化稳定 → 立即触发 HFP 主动连,
         * 将重启连接耗时从 ~80s 压到 ~15s(08-07 实测优化)。
         * 仅边沿触发一次(标志 0→1); 未配对仍走 page_scan_timeout_cb 等手机。 */
        if (!g_ble_param_updated) {
            g_ble_param_updated = 1;
            /* 08-22 整改: HFP 主动连统一由 hfp_mgr 线程在 BLE 连上即抢发,
             * 不再在此独立触发(避免与 mgr 双路径竞发/重复计时)。本事件通常
             * 晚于 mgr 抢发, 仅作状态标记; 若 mgr 因极端竞态未发, page_scan
             * 窗口超时后仍会主动连兜底。 */
            if (g_hid_env.is_connected && !g_hfp_connected)
                rt_kprintf("[HID] BLE param updated (phone stable), mgr owns HFP connect\n");
        }
        break;
    }
    case BLE_GAP_DISCONNECTED_IND: {  // BLE GAP 连接断开事件
        g_sco_quiet_on = 0;   /* ★ 断连后该标志失效: 重连是新链路, 需重新为语音让路 */
        /* 诊断(08-26): 打印断开 reason 区分"本地超时"vs"主机主动断".
         * 语音(SCO)期间 reason 8(Connection Timeout) = 32s 内主机侧未发任何
         * BLE 包(电脑接收器 SCO 期共存调度断供); reason 19(Remote User
         * Terminated) = 电脑接收器主动断开. 两者均指向接收器侧, 设备只能
         * 断得快/恢复快/语音不中断. */
        ble_gap_disconnected_ind_t *dind = (ble_gap_disconnected_ind_t *)data;
        uint8_t ble_disc_reason = dind ? dind->reason : 0xFF;


        const char *rs = "?";
        switch (ble_disc_reason) {
        case 0x08: rs = "conn timeout"; break;
        case 0x13: rs = "remote user term"; break;
        case 0x16: rs = "local host term"; break;
        case 0x0B: rs = "remote dev term"; break;
        default: break;
        }
        rt_kprintf("[BLE HID] disconnected reason=%u(%s), sco_active=%d\n",
                   ble_disc_reason, rs, g_sco_active);
        g_hid_env.is_connected = 0;
        g_phone_acl_up = 0;   /* BLE 断开: 清除手机来连保护(下次 BLE 重连重新评估) */
        acl_up_watchdog_stop();   /* BLE 断开, 停 ACL up 看门狗 */
        ble_hid_kb_tx_reset();   /* 清键盘报告补发 pending, 停定时器 */
        rt_kprintf("[BLE HID] disconnected\n");
        /* 三设备切换: 断连后恢复槽位指示(等待回连蓝闪) */
        bt_multi_on_disconnected();
        power_set_bt_connected(0);
        /* ★ 10-31: 判"经典链路是否还在"再关 —— 鼠标/旋钮现在走经典 HID，
         * BLE 掉了但手机经典链路还活着时，不能把 IMU 停掉（否则鼠标立刻失灵）。 */
        {
            extern unsigned char classic_hid_linked(void);
            if (!classic_hid_linked()) {
                air_mouse_stop();   /* 阶段1：断开即关闭 IMU；内部释放 LIGHT 锁 + 逻辑回到空闲 */
                enc_stop();         /* 断连即停编码器轮询，HPSYS 不再被 1ms 定时器唤醒，可进深睡 */
            }
        }
        ble_led_turn_off(); /* 断连即熄灭 app 经 BLE 推送的状态灯(LED2/3/4)，避免断连后仍亮 */
        /* 断连即复位 HFP 自动连接闩锁: 下次 BLE 重连会重新发起 HFP(否则只连一次、永不重连)。
         * 仅停 page scan 窗口定时器(取消 BLE 连接时的兜底主动连), 但【保持 page scan 开】——
         * 设备仍需被手机经典蓝牙寻呼, 手机可不依赖 BLE 重连直接连 HFP; 深睡由 power.c 关。
         * 同时停 hfp_timer + sniff 定时器。 */
        if (g_page_scan_timer) {
            rt_timer_stop(g_page_scan_timer);
            rt_timer_delete(g_page_scan_timer);
            g_page_scan_timer = RT_NULL;
        }
        if (g_hid_env.hfp_timer) rt_timer_stop(g_hid_env.hfp_timer);
        if (g_sniff_timer) rt_timer_stop(g_sniff_timer);
        hfp_connect_timer = 0;
        hfp_connect_retry = 0;
        g_hfp_rederive_cnt = 0;   /* 新 BLE 会话: 重派生次数重新计 */
        /* ⚠️ 不再清 g_hfp_connected! HFP/SCO 是经典蓝牙独立链路, BLE 断开时它可能
         * 仍存活(语音中 BLE 断连实测: msbc rx errcnt 持续增长 = 电脑还在发语音)。
         * 若在 BLE 断开时清零, BLE 重连后 mgr 会重复主动连 HFP → 与存量链路冲突
         * (实测 BTS2MU_HF_CONN_CFM res:1d) → SCO 被拆、语音断。HFP 真断开由
         * BT_NOTIFY_HF_PROFILE_DISCONNECTED 事件清零(见其处理)。 */
        /* g_hfp_connected = 0; */
        /* 断连后保持广播 ADV_GRACE_WINDOW_MS 供已配对手机静默回连；
         * is_auto_restart=0，SDK 不再自动重启，故此处手动重启广播并开窗口，
         * 窗口超时未回连则由 adv_grace_timeout_cb 停广播进深睡。
         * ⚠️ 2026-10-12 变更: 语音(SCO)活跃时**也立即广播**(原来会延迟到 SCO 结束)。
         * 原因见下面分支内注释 —— 语音开关走 BLE HID 的 ALT 键, 掉线即用户被锁死。 */
        if (g_standby_teardown) {
            LOG_I("[BLE] disconnected for standby, skip advertise");
        } else {
            /* ★ 2026-10-12: 语音(SCO)中掉线也必须【立即】广播, 不再延迟。
             * 语音开关是 C1 → BLE HID 的 ALT 键 → 电脑输入法: BLE 一掉 ALT 就发不出去,
             * 用户被彻底锁死(停不了语音 / SCO 干挂 / Active ~3mA), 本地按 C1 也无效
             * (HFP 的 AT+BVRA=0 停的是手机端 VF, 与电脑输入法无关)。
             * ⇒ 唯一出路是尽快重连, 因此广播优先级高于"避免与 SCO 抢射频"。
             * 实测教训: 旧方案的 30s 软定时器兜底从未触发(软定时器被 BLE RF 活动饿死),
             * 广播拖了 191s 才回来, 把一次掉线放大成 ~3 分钟高功耗。 */
            if (g_sco_active)
                LOG_I("[BLE] SCO active, advertise immediately (voice stop needs BLE HID ALT)");
            ble_app_wakeup_advertise();
        }
        break;
    }
    case SIBLES_WRITE_VALUE_RSP: {   /* LE 栈真实下发结果(键盘报告确认, 见 ble_hid.c) */
        sibles_write_value_rsp_t *rsp = (sibles_write_value_rsp_t *)data;
        ble_hid_on_tx_rsp(rsp->conn_idx, rsp->result);
        break;
    }
    case CONNECTION_MANAGER_PAIRING_SUCCEED: {   /* BLE 配对成功(新设备绑定) */
        rt_kprintf("[BLE HID] pairing succeeded (slot%d)\n", bt_multi_get_slot());
        bt_dump_bonds("after pairing");
        /* 三设备切换: 把新配对设备地址记录到当前槽位 + 熄灭指示 */
        bt_multi_on_paired();
        /* 配对完成 = 配对窗口结束: 恢复经典蓝牙 page scan(可被新设备 BR/EDR
         * 寻呼建 HFP)。BR/EDR 栈未就绪时此调用被 SDK 忽略, 由 BT_STACK_READY
         * 时的状态判断兜底(届时已非配对窗口 → 正常开启)。 */
        if (g_br_ready) {
            /* ★ 11-25 日志如实反映实际行为：经典被配置关闭时 vk_set_scan_mode
             * 会强制归零，原来那句 "page scan restored" 会误导排查
             * （上层说恢复了，底层其实没开）。 */
            vk_set_scan_mode(0, 1);
            if (ble_app_classic_enabled())
                rt_kprintf("[HID] PAIR done: classic BT page scan restored\n");
            else
                rt_kprintf("[HID] PAIR done: page scan kept OFF (classic disabled)\n");
        }
        break;
    }
    default: break;
    }
    return 0;
}
BLE_EVENT_REGISTER(ble_hid_event_handler, NULL);

/* 结束语音识别: 发 AT+BVRA=0 请求 AG 停 VF(走 HFP 链路, 不依赖 BLE).
 * SCO 期间 BLE 键盘报告可能延迟/丢失(实测 11s 才送达), BVRA=0 是"结束
 * 语音"的可靠兜底 —— 由 main.c 在 C1 语音开关键按下且 SCO 活跃时调用,
 * 与键盘报告双通道并行, 谁先到达谁生效.
 * 复查重试: 实测某些手机不响应单次 BVRA=0(旧 SDK 铁证), 故 2s 后复查
 * SCO 仍活跃则重发, 最多共 VF_BVRA_RETRY_MAX 次; SCO 断开即停. */
static void vf_retry_timer_cb(void *param)
{
    (void)param;
    if (!g_hfp_connected || !g_sco_active) { g_vf_retry_left = 0; return; }
    /* 非 VF 会话(正常电话等): 不重试也不拆 SCO, 避免误拆正常通话 */
    if (!g_vf_active) { g_vf_retry_left = 0; return; }
    if (g_vf_retry_left > 0) {
        /* 第一级: 再试一次温和的 AT+BVRA=0 */
        bt_interface_voice_recog(0);
        g_vf_retry_left--;
        LOG_I("[HFP] BVRA=0 retry left=%d", g_vf_retry_left);
        rt_timer_start(g_vf_retry_timer);
    } else {
        /* 第二级: 温和手段无效(实测手机确认 AT 命令但 VF 不停), 强制拆 SCO ——
         * 手机收到音频流断开必然停止语音识别, 历史实测唯一可靠路径 */
        bt_interface_audio_switch(1);
        LOG_I("[HFP] force SCO disconnect (VF not responding)");
    }
}

/* 语音(SCO)结束后的 HCPU 侧 PM 收尾: 恢复空闲 WFI 分频档位。
 * 完整根因说明见调用处(BT_NOTIFY_COMMON_SCO_DISCONNECTED)的注释。 */
static void ble_app_voice_end_pm_restore(void)
{
    extern int usb_on;   /* main.c:50 全局量; power.c 亦以此判 USB 在线 */
    if (usb_on)
        return;          /* USB 在线: main.c:104 有意持 div 12(CDC 时延敏感), 不可动 */
    HAL_RCC_HCPU_SetDeepWFIDiv(60, 0, 1);   /* 与 power_init / usb_stop 一致的空闲值(4MHz 档) */
}

/* 武装空闲 eSCO 拆除定时器(创建/重启 2.5s 一次性软定时器)。
 * 调用点: ① HFP 首次连接(BT_NOTIFY_HF_PROFILE_CONNECTED)
 *         ② 每次【真实】语音结束(BT_NOTIFY_COMMON_SCO_DISCONNECTED, sco_was_active 门控)。 */
static void esco_idle_teardown_schedule(void)
{
    if (!g_esco_idle_timer) {
        g_esco_idle_timer = rt_timer_create("esco_idle",
            esco_idle_teardown_cb, RT_NULL,
            rt_tick_from_millisecond(ESCO_IDLE_TEARDOWN_MS),
            RT_TIMER_FLAG_ONE_SHOT | RT_TIMER_FLAG_SOFT_TIMER);
    }
    if (g_esco_idle_timer) {
        rt_timer_stop(g_esco_idle_timer);
        rt_timer_start(g_esco_idle_timer);
    }
}

/* HFP 连上后【无通话】防御性拆"空闲 eSCO": 栈在 HFP 建立时自动 open 一条 eSCO
 * (日志 HF device_sco_state→0x5) 但不派发 SCO_CONNECTED(g_sco_active=0, 无音频流),
 * 这条"挂空"的 eSCO 占经典蓝牙时隙 → BR/EDR 反复弹 Active/Sniff(电流 1.7~2.5 横跳).
 * 拆掉后 BR 稳 Sniff, 电流回到稳定 ~1.5.
 * 门控: 仅当【已连 HFP 且当前无通话/无语音】(g_sco_active==0 && !g_vf_active) 才拆;
 * 通话中绝不拆(由 SCO_DISCONNECTED/VF 流程负责). 真有来电时栈会重建 eSCO 并派发
 * SCO_CONNECTED(g_sco_active=1), 不影响通话建立. */
static void esco_idle_teardown_cb(void *param)
{
    (void)param;
    /* [诊断] 确认本回调是否真的被触发, 以及触发时的门控状态.
     * 关键判据:
     *  - 若触发时 g_sco_active==1 → 确有"挂空 eSCO", audio_switch(1) 应引发
     *    SCO_DISCONNECTED(g_sco_active 1->0), BR/EDR 才能稳定进 Sniff(回到 ~1.5mA).
     *  - 若触发时 g_sco_active 已是 0 → app 层根本无 SCO 实体; "挂空 eSCO" 假设
     *    不成立, 3.8mA 由别的原因(ACL 被周期流量顶在 Active / SDK 内部 eSCO 未
     *    派发 SCO_CONNECTED)导致, 需另查(看下面 SCO_DISCONNECTED 是否真来). */
    LOG_I("[HFP][DIAG] esco_idle_teardown_cb ENTER: g_hfp_connected=%d g_sco_active=%d g_vf_active=%d",
          g_hfp_connected, g_sco_active, g_vf_active);

    if (!g_hfp_connected) {
        LOG_I("[HFP][DIAG] SKIP: HFP not connected");
        return;
    }
    if (g_sco_active || g_vf_active) {
        LOG_I("[HFP][DIAG] SKIP: in call/voice (g_sco_active=%d g_vf_active=%d) -> ACL cannot Sniff, do NOT tear down",
              g_sco_active, g_vf_active);
        return;
    }
    /* 拆之前记录 SCO 态(正常应为 0, 即无 app 层 eSCO), 用于对比后续
     * BT_NOTIFY_COMMON_SCO_DISCONNECTED 是否真来(异步确认 eSCO 被拆). */
    int sco_before = g_sco_active;
    int ret = bt_interface_audio_switch(1);   /* 0 = 提交成功; 仅表示请求已发出, 拆完靠 SCO_DISCONNECTED */
    LOG_I("[HFP][DIAG] idle eSCO teardown REQUESTED: audio_switch ret=%d (0=submit OK), sco_before=%d",
          ret, sco_before);
    LOG_I("[HFP][DIAG] -> expect BT_NOTIFY_COMMON_SCO_DISCONNECTED (g_sco_active 1->0) then BR/EDR stable Sniff (~1.5mA)");
}

/* 结束语音识别: 发 AT+BVRA=0 请求 AG 停 VF(走 HFP 链路, 不依赖 BLE).
 * SCO 期间 BLE 键盘报告可能延迟/丢失(实测 11s 才送达), BVRA=0 是"结束
 * 语音"的兜底 —— 由 main.c 在 C1 语音开关键按下且 SCO 活跃时调用.
 * 两级兜底(软定时器, 回调在 timer 线程可安全调 SDK):
 *   t+2s   SCO 仍活跃且 VF 会话 → 再发一次 BVRA=0
 *   t+4s   SCO 仍活跃且 VF 会话 → 强制拆 SCO(手机必停录音, 唯一可靠路径)
 * 仅 VF 会话(g_vf_active)才走到拆 SCO; 正常通话不拆. SCO 断开即停. */
void ble_app_hfp_deactivate_vf(void)
{
    if (!g_hfp_connected) return;
    bt_interface_voice_recog(0);
    LOG_I("[HFP] send BVRA=0 (deactivate voice recognition)");
    /* ★ 2026-10-12: BLE 已断时不再走 BVRA 重试(要 2s+18s 才有结果), 直接强拆 SCO 止损。
     * 理由: 语音开关在【电脑输入法】侧(C1 发 ALT 走 BLE), BLE 断 = 用户已被锁死,
     * 手机端 VF 停不停都救不回用户控制权; 此刻唯一该做的是别再让 ACL 停在 Active
     * 白烧 ~3mA(实测语音中 BLE 掉线后 Active 烧了 ~3 分钟)。 */
    if (!g_hid_env.is_connected) {
        LOG_I("[HFP] BLE down -> skip BVRA retry, force SCO disconnect now");
        g_vf_retry_left = 0;
        bt_interface_audio_switch(1);
        return;
    }
    g_vf_retry_left = 1;   /* 1 次 BVRA=0 重试, 之后拆 SCO */
    if (!g_vf_retry_timer) {
        /* 必须 SOFT_TIMER: 回调里调 bt_interface_voice_recog → SDK 内部
         * rt_malloc, 硬定时器回调跑在 tick 中断上下文会断言崩溃
         * (08-06 实测 fatal error on ISR). 软定时器跑在 timer 线程可安全调用. */
        g_vf_retry_timer = rt_timer_create("vf_retry", vf_retry_timer_cb, RT_NULL,
                                           rt_tick_from_millisecond(VF_BVRA_RETRY_MS),
                                           RT_TIMER_FLAG_ONE_SHOT | RT_TIMER_FLAG_SOFT_TIMER);
        if (!g_vf_retry_timer) return;
    }
    rt_timer_stop(g_vf_retry_timer);
    rt_timer_start(g_vf_retry_timer);
}



/* BLE 上电看门狗：20s 后若仍未收到 POWER_ON_IND，补发强制初始化。
 * 用线程而非 soft timer（BLE RF cal 约 10s 会饿死 soft timer 线程）。 */
static void ble_boot_timeout_thread(void *p)
{
    rt_thread_mdelay(20000);
    if (!ble_hid_is_ready()) {
        rt_kprintf("[BLE HID] POWER_ON_IND timeout, forcing init\n");
        ble_hid_ensure_init();
    }
}

void app_bt_hid_init(void)
{
    ble_hid_init();
    bt_interface_register_bt_event_notify_callback(bt_app_interface_event_handle);
    hid_service_therad_init();
    hid_battery_report_init();
    sifli_ble_enable();

    static rt_thread_t ble_boot_tid = RT_NULL;
    ble_boot_tid = rt_thread_create("bleboot", ble_boot_timeout_thread, RT_NULL,
                                     2048, 10, 10);
    if (ble_boot_tid)
        rt_thread_startup(ble_boot_tid);

    /* HFP 主动连管理线程: 等 BR/EDR key 就绪→主动连。独立线程避免阻塞
     * 蓝牙事件分发(08-07 日志 15 教训: 事件线程内等待 = 全系统蓝牙卡死)。
     * 优先级 12(低于事件分发线程), 事件线程只发信号量唤醒。 */
    g_hfp_mgr_sem = rt_sem_create("hfp_mgr", 0, RT_IPC_FLAG_FIFO);
    static rt_thread_t hfp_mgr_tid = RT_NULL;
    hfp_mgr_tid = rt_thread_create("hfp_mgr", hfp_mgr_thread_entry, RT_NULL,
                                   2048, 12, 10);
    if (hfp_mgr_tid)
        rt_thread_startup(hfp_mgr_tid);
}
