
#include <rtthread.h>
#include "ble_app.h"
#include "bf0_hal.h"
#include "drivers/pm.h"
#include <string.h>

#include "bf0_sibles.h"
#include "bf0_ble_gap.h"
#include "bf0_sibles_advertising.h"
#include "att.h"
#include "ble_led_service.h"
#include "ble_connection_manager.h"
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

/* 语音(SCO)活跃期间 BLE 断开: 延迟重启广播, 避免广播与 SCO 抢同一射频.
 * 见文件后部定义(ble_app_wakeup_advertise 之后). */
static void sco_adv_defer_start(void);
static void sco_adv_defer_cancel(void);

/* 编码器启停（连接时启、断连即停，避免 1ms 轮询定时器常驻阻断 HPSYS 深睡） */
extern void enc_start(void);
extern void enc_stop(void);

#ifdef BT_DEVICE_NAME
    static const char *local_name = BT_DEVICE_NAME;
#else
    static const char *local_name = "VibeKey-F3";
#endif

/* 设备类别(COD, Class of Device)：
 * 用户要求恢复默认图标 → 改回基线 0x200000(Object Transfer)。
 * ⚠️ 代价(08-07 实测): 安卓不当音频设备 → 不自动连 BR/EDR HFP, 对设备
 * page 消极响应(100s+) → 设备重启后 HFP 连接慢(~200s)。设备主动连
 * 兜底逻辑保留(6s 主动连 + 重试收敛), 仅手机响应慢。
 * 历史: 0x200414(音响图标)→0x200408(耳机图标)均实测改善手机响应
 * (24s), 若后续想恢复"手机识别为音频设备"的收益可改回。 */
uint32_t bt_get_class_of_device(void) { return 0x200000; }

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
    bt_addr_convert_to_bts((bd_addr_t *)g_hid_env.peer_addr, &g_sniff_bd);
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
    bt_addr_convert_to_bts((bd_addr_t *)g_hid_env.peer_addr, &g_sniff_bd);
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
    extern void *bt_cm_get_bonded_dev_by_addr(uint8_t *addr);
    return bt_cm_get_bonded_dev_by_addr(g_hid_env.peer_addr) != NULL;
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
                bt_err_t r = bt_interface_hfp_hf_start_connecting(g_hid_env.peer_addr);
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
                        r = bt_interface_hfp_hf_start_connecting(g_hid_env.peer_addr);
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
    bt_interface_set_scan_mode(0, 0);
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
    bt_interface_set_scan_mode(0, 1);   // 不可被搜索(关 inquiry)，保持可连接(page 开)
    rt_kprintf("[BLE HID] HFP connect attempt %d -> %02x:%02x:%02x:%02x:%02x:%02x\n",
        hfp_connect_retry,
        g_hid_env.peer_addr[5], g_hid_env.peer_addr[4], g_hid_env.peer_addr[3],
        g_hid_env.peer_addr[2], g_hid_env.peer_addr[1], g_hid_env.peer_addr[0]);
    bt_err_t ret = bt_interface_hfp_hf_start_connecting(g_hid_env.peer_addr);
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
            bt_interface_set_local_name(strlen(local_name), (void *)local_name);
            LOG_I("[HID] BT stack ready, name=%s", local_name);
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
            bt_interface_set_scan_mode(0, 1);
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
            //hid_report_battery();
            break;
        case BT_NOTIFY_COMMON_ACL_CONNECTED:    // ACL 链路建立
            {
                bt_notify_device_acl_conn_info_t *acl = (bt_notify_device_acl_conn_info_t *)msg->data;
                rt_kprintf("[HID] ACL connected: dir=%d (0=device-init, 1=phone-init)\n",
                    acl->acl_dir);
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
            g_sco_active = 0;   /* 通话结束：允许 RC 周期重校定时器恢复 */
            g_vf_active = 0;    /* SCO 断开, VF 会话必然结束 */
            g_vf_retry_left = 0;             /* 语音已结束, 停 BVRA=0 复查/拆 SCO 兜底 */
            if (g_vf_retry_timer) rt_timer_stop(g_vf_retry_timer);
            /* ⚠️ SCO 结束射频释放: 通话中 BLE 包被射频抢占(键盘 up/按钮可能
             * 滞留丢失, 补发也在通话中被跳过), 立即重发最后 HID 状态, 恢复
             * 对端一致性(PC 停止录音/按钮对齐)。抢在射频最空闲时刻执行。 */
            ble_hid_flush_pending();
            bt_addr_convert_to_bts((bd_addr_t *)g_hid_env.peer_addr, &g_sniff_bd);
            hfp_request_sniff_delayed(100);
            /* 按当前活动状态恢复 BLE 参数: 活跃→tighten(7.5~15ms 跟手),
             * 空闲→relax(20~40ms 省电). 不能无条件 relax——活跃态鼠标被降
             * 到 25Hz 会掉帧(SDK 更新解决 SCO 共存后此问题凸显). */
            ble_app_conn_param_by_activity();
            /* 语音结束: 若 BLE 在语音中断开(处于延迟广播), 电脑刚结束语音最
             * 有空回连 → 立即恢复广播(取消 30s 兜底定时器). BLE 未断时
             * wakeup_advertise 内部有 is_connected 保护, 无副作用. */
            sco_adv_defer_cancel();
            if (!g_hid_env.is_connected) {
                LOG_I("[BLE] SCO ended, restore advertise for re-connect");
                ble_app_wakeup_advertise();
            }
            break;
        default: break;
        }
    }
    else if (msg->type == BT_NOTIFY_HFP_HF)
    {
        switch (msg->event_id) {
        case BT_NOTIFY_HF_PROFILE_CONNECTED:    // HFP免提配置文件连接完成事件
            LOG_I("[APP] HFP connected");
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
            break;
        case BT_NOTIFY_HF_PROFILE_DISCONNECTED: // HFP免提配置文件断开完成事件
            LOG_I("[APP] HFP disconnected");
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
                    ble_app_conn_param_by_activity();
                }
                /* HFP/SCO 断开兜底(08-26): SCO 依附经典 ACL, HFP 断开则通话必
                 * 已结束 —— 若语音中 BLE 断开处于延迟广播, 现在恢复(电脑有空
                 * 回连). 与 SCO_DISCONNECTED 路径二选一, 谁先到谁恢复. */
                sco_adv_defer_cancel();
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
                                bt_interface_set_scan_mode(0, 1);
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
                            bt_interface_set_scan_mode(0, 1);
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
                    bt_interface_set_scan_mode(0, 1);
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
    sibles_advertising_para_t para = {0};
    
    ble_gap_dev_name_t *dev_name = malloc(sizeof(ble_gap_dev_name_t) + strlen(local_name));
    if (!dev_name) { LOG_E("[HID] malloc dev_name failed"); return; }
    dev_name->len = strlen(local_name);
    memcpy(dev_name->name, local_name, dev_name->len);
    ble_gap_set_dev_name(dev_name);
    free(dev_name);
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
}

/* 语音(SCO)活跃期间 BLE 断开的广播策略 (08-26):
 * SCO 是周期实时流, 广播/扫描类射频活动会与它争抢 2.4GHz 时隙 → 加剧
 * "msbc rx errcnt 100%" 收错; 且电脑接收器 SCO 期间本就没空回连 BLE。
 * 因此语音中 BLE 断开【不立即广播】, 等 SCO 结束(电脑刚空出来, 最有空
 * 回连)由 SCO_DISCONNECTED/HFP_DISCONNECTED 立即广播; 本定时器仅作兜底
 * (30s): 防 SCO 断开事件丢失导致广播永不重启 —— 语音一般远短于 30s,
 * 若超时语音仍在, 恢复广播的代价(短暂共存)远小于永久静默。 */
#define SCO_ADV_DEFER_MS 30000
static rt_timer_t g_sco_adv_defer_timer = RT_NULL;
static void sco_adv_defer_cb(void *param)
{
    g_sco_adv_defer_timer = RT_NULL;
    if (!g_hid_env.is_connected) {
        LOG_I("[BLE] SCO-adv defer timeout(%dms), restoring advertise", SCO_ADV_DEFER_MS);
        ble_app_wakeup_advertise();
    }
}
static void sco_adv_defer_start(void)
{
    if (!g_sco_adv_defer_timer) {
        g_sco_adv_defer_timer = rt_timer_create("sco_adv", sco_adv_defer_cb, RT_NULL,
            rt_tick_from_millisecond(SCO_ADV_DEFER_MS),
            RT_TIMER_FLAG_ONE_SHOT | RT_TIMER_FLAG_SOFT_TIMER);
    }
    if (g_sco_adv_defer_timer) rt_timer_start(g_sco_adv_defer_timer);
}
static void sco_adv_defer_cancel(void)
{
    if (g_sco_adv_defer_timer) {
        rt_timer_stop(g_sco_adv_defer_timer);
        rt_timer_delete(g_sco_adv_defer_timer);
        g_sco_adv_defer_timer = RT_NULL;
    }
}

#ifdef FINSH_USING_MSH
#include <finsh.h>
/* 测试用：手动重启广播（断连后调用以验证按键唤醒路径）*/
static int adv_start(void)
{
    ble_app_wakeup_advertise();
    return 0;
}
MSH_CMD_EXPORT(adv_start, restart BLE advertising (for testing))
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
    /* 空闲态连接参数: interval 16~32(20~40ms), slave_latency 0, timeout 600(6s).
     *
     * 注意: 此处 slave_latency 必须为 0! 旧实现用 latency 4 (可跳过 4 个连接事件),
     * 最坏每 5×100ms=500ms 才送达一次 HID 报告。空中鼠标每 10ms 累积一帧位移,
     * 连接事件到来时控制器把 500ms 内攒的多帧一次性灌出 → 光标"跳变";
     * 且 tighten 请求要等手机协商 1~2s 才生效, 这期间持续跳变 → 用户感知的
     * "空闲→活跃卡顿跳变, 过一两秒才正常"。latency=0 让每帧及时送达, 消除跳变。
     * interval 比活跃态(7.5~15ms)宽, 仍保留一定省电(射频活动更少)。 */
    ble_gap_update_conn_param(BLE_GAP_CREATE_UPDATE_CONN_PARA(
        g_hid_env.conn_idx, 16, 32, 0, 600));
    LOG_I("[BLE] conn param relaxed (idle: 20~40ms, latency 0)");
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
    /* 通话中 BLE 拉长到 40~80ms(空事件, 占射频极短)给 SCO 让路; latency 0
     * 保证挂断后鼠标恢复跟手; timeout 3200(32s 规范上限)容忍低频期被 SCO
     * 挤占的事件缺口, 防 reason 8 断连, 通话结束参数恢复后自动回正常。 */
    ble_gap_update_conn_param(BLE_GAP_CREATE_UPDATE_CONN_PARA(
        g_hid_env.conn_idx, 32, 64, 0, 3200));
    LOG_I("[BLE] conn param SCO-quiet (40~80ms, yield to voice, to 32s)");
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
        if (bt_multi_is_pairing_window())
            bt_interface_set_scan_mode(0, 0);
        else
            bt_interface_set_scan_mode(0, 1);
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
            if (bonded && !g_hfp_failed_once && !g_stale_bond_cleared) {
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
                g_ble_cur_interval = pind->con_interval;
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
        air_mouse_stop();   /* 阶段1：断开即关闭 IMU；内部释放 LIGHT 锁 + 逻辑回到空闲 */
        enc_stop();         /* 断连即停编码器轮询，HPSYS 不再被 1ms 定时器唤醒，可进深睡 */
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
         * ⚠️ 语音(SCO)活跃时【不立即广播】: 广播会与 SCO 抢同一射频(加剧
         * msbc rx errcnt), 且电脑 SCO 期间无暇回连 —— 由 sco_adv_defer
         * 兜底, 等 SCO_DISCONNECTED/HFP_DISCONNECTED 立即广播(电脑刚结束
         * 语音最有空回连). */
        if (g_standby_teardown) {
            LOG_I("[BLE] disconnected for standby, skip advertise");
        } else if (g_sco_active) {
            LOG_I("[BLE] SCO active, defer advertise until SCO ends (30s fallback)");
            sco_adv_defer_start();
        } else {
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
        /* 三设备切换: 把新配对设备地址记录到当前槽位 + 熄灭指示 */
        bt_multi_on_paired();
        /* 配对完成 = 配对窗口结束: 恢复经典蓝牙 page scan(可被新设备 BR/EDR
         * 寻呼建 HFP)。BR/EDR 栈未就绪时此调用被 SDK 忽略, 由 BT_STACK_READY
         * 时的状态判断兜底(届时已非配对窗口 → 正常开启)。 */
        if (g_br_ready) {
            bt_interface_set_scan_mode(0, 1);
            rt_kprintf("[HID] PAIR done: classic BT page scan restored\n");
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
