#include <rtthread.h>
#include "ble_app.h"
#include "bf0_hal.h"
#include "drivers/pm.h"
#include <string.h>

// #include "bf0_sibles.h"
#include "bf0_ble_gap.h"
#include "bf0_sibles_advertising.h"
#include "att.h"

#include "ble_hid.h"
#include "ble_led_service.h"
#define LOG_TAG "ble_hid"
#include "ulog.h"



/* ===== BLE HID GATT ===== */
static const uint8_t hid_info_value[] = {0x11, 0x01, 0x00, 0x02};
static const uint8_t hid_report_map[] = {
    0x05, 0x01, 0x09, 0x06, 0xA1, 0x01, 0x85, 0x01,
    0x75, 0x01, 0x95, 0x08, 0x05, 0x07, 0x19, 0xE0, 0x29, 0xE7,
    0x15, 0x00, 0x25, 0x01, 0x81, 0x02, 0x95, 0x01, 0x75, 0x08, 0x81, 0x01,
    0x95, 0x05, 0x75, 0x01, 0x05, 0x08, 0x19, 0x01, 0x29, 0x05,
    0x91, 0x02, 0x95, 0x01, 0x75, 0x03, 0x91, 0x01,
    0x95, 0x06, 0x75, 0x08, 0x15, 0x00, 0x25, 0x65,
    0x05, 0x07, 0x19, 0x00, 0x29, 0x65, 0x81, 0x00, 0xC0,
    0x05, 0x01, 0x09, 0x02, 0xA1, 0x01, 0x85, 0x02,
    0x09, 0x01, 0xA1, 0x00,
    0x05, 0x09, 0x19, 0x01, 0x29, 0x08, 0x15, 0x00, 0x25, 0x01, 0x95, 0x08, 0x75, 0x01, 0x81, 0x02,
    0x05, 0x01, 0x09, 0x30, 0x09, 0x31, 0x16, 0x01, 0xF8, 0x26, 0xFF, 0x07, 0x75, 0x0C, 0x95, 0x02, 0x81, 0x06,
    0x09, 0x38, 0x15, 0x81, 0x25, 0x7F, 0x75, 0x08, 0x95, 0x01, 0x81, 0x06, 0xC0, 0xC0
};
static const uint8_t kb_report_ref[] = {0x01, 0x01};
static const uint8_t ms_report_ref[] = {0x02, 0x01};

enum {
    HID_IDX_SVC,
    HID_IDX_INFO_CHAR,
    HID_IDX_INFO_VAL,
    HID_IDX_REPORT_MAP_CHAR,
    HID_IDX_REPORT_MAP_VAL,
    HID_IDX_KB_REPORT_CHAR,
    HID_IDX_KB_REPORT_VAL,
    HID_IDX_KB_REPORT_REF,
    HID_IDX_KB_REPORT_NTF_CFG,
    HID_IDX_MS_REPORT_CHAR,
    HID_IDX_MS_REPORT_VAL,
    HID_IDX_MS_REPORT_REF,
    HID_IDX_MS_REPORT_NTF_CFG,
    HID_IDX_CTRL_CHAR,
    HID_IDX_CTRL_VAL,
    HID_ATT_NB
};

static struct attm_desc hid_att_db[] = {
    [HID_IDX_SVC]               = {ATT_DECL_PRIMARY_SERVICE, PERM(RD, ENABLE), 0, 0},
    [HID_IDX_INFO_CHAR]         = {ATT_DECL_CHARACTERISTIC,  PERM(RD, ENABLE), 0, 0},
    [HID_IDX_INFO_VAL]          = {ATT_CHAR_HID_INFO,        PERM(RD, ENABLE), PERM(RI, ENABLE), sizeof(hid_info_value)},
    [HID_IDX_REPORT_MAP_CHAR]   = {ATT_DECL_CHARACTERISTIC,  PERM(RD, ENABLE), 0, 0},
    [HID_IDX_REPORT_MAP_VAL]    = {ATT_CHAR_REPORT_MAP,      PERM(RD, ENABLE), PERM(RI, ENABLE), sizeof(hid_report_map)},
    [HID_IDX_KB_REPORT_CHAR]    = {ATT_DECL_CHARACTERISTIC,  PERM(RD, ENABLE), 0, 0},
    [HID_IDX_KB_REPORT_VAL]     = {ATT_CHAR_REPORT,          PERM(RD, ENABLE) | PERM(NTF, ENABLE), PERM(RI, ENABLE), 8},
    [HID_IDX_KB_REPORT_REF]     = {ATT_DESC_REPORT_REF,      PERM(RD, ENABLE), PERM(RI, ENABLE), sizeof(kb_report_ref)},
    [HID_IDX_KB_REPORT_NTF_CFG] = {ATT_DESC_CLIENT_CHAR_CFG, PERM(RD, ENABLE) | PERM(WRITE_REQ, ENABLE) | PERM(WP, UNAUTH), PERM(RI, ENABLE), 2},
    [HID_IDX_MS_REPORT_CHAR]    = {ATT_DECL_CHARACTERISTIC,  PERM(RD, ENABLE), 0, 0},
    [HID_IDX_MS_REPORT_VAL]     = {ATT_CHAR_REPORT,          PERM(RD, ENABLE) | PERM(NTF, ENABLE), PERM(RI, ENABLE), 5},
    [HID_IDX_MS_REPORT_REF]     = {ATT_DESC_REPORT_REF,      PERM(RD, ENABLE), PERM(RI, ENABLE), sizeof(ms_report_ref)},
    [HID_IDX_MS_REPORT_NTF_CFG] = {ATT_DESC_CLIENT_CHAR_CFG, PERM(RD, ENABLE) | PERM(WRITE_REQ, ENABLE) | PERM(WP, UNAUTH), PERM(RI, ENABLE), 2},
    [HID_IDX_CTRL_CHAR]         = {ATT_DECL_CHARACTERISTIC,  PERM(RD, ENABLE), 0, 0},
    [HID_IDX_CTRL_VAL]          = {ATT_CHAR_HID_CTNL_PT,     PERM(WRITE_REQ, ENABLE), PERM(RI, ENABLE), 1},
};

static uint8_t batt_level = 100;
enum {
    BAS_IDX_SVC,
    BAS_IDX_BATT_LVL_CHAR,
    BAS_IDX_BATT_LVL_VAL,
    BAS_IDX_BATT_LVL_NTF_CFG,
    BAS_ATT_NB
};

static struct attm_desc bas_att_db[] = {
    [BAS_IDX_SVC]              = {ATT_DECL_PRIMARY_SERVICE, PERM(RD, ENABLE), 0, 0},
    [BAS_IDX_BATT_LVL_CHAR]    = {ATT_DECL_CHARACTERISTIC,  PERM(RD, ENABLE), 0, 0},
    [BAS_IDX_BATT_LVL_VAL]     = {ATT_CHAR_BATTERY_LEVEL,   PERM(RD, ENABLE) | PERM(NTF, ENABLE), PERM(RI, ENABLE), 1},
    [BAS_IDX_BATT_LVL_NTF_CFG] = {ATT_DESC_CLIENT_CHAR_CFG, PERM(RD, ENABLE) | PERM(WRITE_REQ, ENABLE) | PERM(WP, UNAUTH), PERM(RI, ENABLE), 2},
};



ble_hid_env_t g_hid_env;

static uint8_t *hid_gatts_get_cbk(uint8_t conn_idx, uint8_t idx, uint16_t *len)
{
    uint8_t *ret = NULL; *len = 0;
    switch (idx) {
    case HID_IDX_INFO_VAL:
        ret = (uint8_t *)hid_info_value;
        *len = sizeof(hid_info_value);
        break;
    case HID_IDX_REPORT_MAP_VAL:
        ret = (uint8_t *)hid_report_map;
        *len = sizeof(hid_report_map);
        break;
    case HID_IDX_KB_REPORT_VAL:
        ret = g_hid_env.last_kbd_report;
        *len = sizeof(g_hid_env.last_kbd_report);
    break;
        case HID_IDX_KB_REPORT_REF:
        ret = (uint8_t *)kb_report_ref;
        *len = sizeof(kb_report_ref);
    break;
        case HID_IDX_MS_REPORT_VAL:
        ret = g_hid_env.last_mouse_report;
        *len = sizeof(g_hid_env.last_mouse_report);
    break;
        case HID_IDX_MS_REPORT_REF:
        ret = (uint8_t *)ms_report_ref;
        *len = sizeof(ms_report_ref);
        break;
    default: break;
    }
    return ret;
}

static uint8_t hid_gatts_set_cbk(uint8_t conn_idx, sibles_set_cbk_t *para)
{
    switch (para->idx) {
    case HID_IDX_KB_REPORT_NTF_CFG:
        g_hid_env.kb_ntf_enabled = *(para->value);
        break;
    case HID_IDX_MS_REPORT_NTF_CFG:
        g_hid_env.ms_ntf_enabled = *(para->value);
        break;
    case HID_IDX_CTRL_VAL:
        rt_kprintf("[BLE HID] Control Point: %d\n", para->value[0]);
        break;
    default: break;
    }
    return 0;
}


static uint8_t *bas_gatts_get_cbk(uint8_t conn_idx, uint8_t idx, uint16_t *len)
{
    uint8_t *ret = NULL; *len = 0;
    if (idx == BAS_IDX_BATT_LVL_VAL) { ret = &batt_level; *len = 1; }
    return ret;
}

static uint8_t bas_gatts_set_cbk(uint8_t conn_idx, sibles_set_cbk_t *para)
{
    if (para->idx == BAS_IDX_BATT_LVL_NTF_CFG) g_hid_env.bas_ntf_enabled = *(para->value);
    return 0;
}


/* ===== TX 池等待 (语音/SCO 期间 HID 偶发失效根因修复, 08-06) =====
 * sibles TX 池仅 8 包(MAX_NUM_OF_TX_PKT=8), 由 LCPU 在 controller 实际发完
 * 一个包后归还(SIBLES_VALUE_IND_RSP)。通话(SCO)中 BLE 被 sco_quiet 拉到
 * 40~80ms 且射频被 SCO 抢占 → 归还慢, 连续按键/键盘补发/按钮操作极易耗尽池
 * → sibles_acquire_tx_pkts() 返回 0 → sibles_write_value 静默丢包
 * (bf0_sibles.c:1429) → 表现为"语音期间 HID 偶发用不了"。
 * 这里发送前等池归还(≤6 次×5ms=30ms), 超时放弃(键盘补发下轮重试,
 * 按钮/移动由发送线程按引脚电平下帧纠正)。必须在线程上下文调用。 */
#define TX_WAIT_MAX_TRIES 6
static void ble_hid_wait_tx_pkt(void)
{
    extern uint8_t sibles_get_tx_pkts(void);
    int tries = 0;
    while (sibles_get_tx_pkts() <= 1 && tries < TX_WAIT_MAX_TRIES) {
        rt_thread_mdelay(5);
        tries++;
    }
}

static void ble_hid_keyboard_send(uint8_t modifier, uint8_t *keys, uint8_t len);
static void ble_hid_kb_arm_retry(void);   /* 前置声明: 被 ble_hid_keyboard_send 调用 */

/* ===== 多键同时按下支持 (08-27 修复互斥) =====
 * 维护"当前按住的 HID 键集合": 每个元素记录该键的 (modifier, keycode) 与
 * 引用计数。按下时加入(已存在则 ref++), 释放时 ref--(归零才移除),
 * 每次变化都从集合重建【完整】6 键报告发送 —— 这样同时按 C2+C3 会发
 * [C2, C3, 0, 0, 0, 0], 而非后按覆盖先按(旧实现每次只发单键 → 互斥假象)。
 * 引用计数处理"同一键被多来源按下"的边界情况; HID 协议最多 6 键, 集合留 8
 * 余量, 超 6 键时按顺序截断(标准 NKRO 行为)。 */
#define HID_HELD_MAX 8
typedef struct { uint8_t modifier; uint8_t keycode; int ref; } hid_held_t;
static hid_held_t g_kb_held[HID_HELD_MAX] = {0};

/* 从集合重建并发送完整键盘报告(所有当前按住的键 + OR 后的 modifier) */
static void ble_hid_kb_send_held(void)
{
    uint8_t modifier = 0;
    uint8_t keys[6] = {0};
    int n = 0;
    /* 遍历全部按住项: modifier 一律 OR 进报告; 仅 keycode≠0 的填入 6 键数组。
     * 这样"纯修饰键"(如 C1 默认 LCtrl+LShift+LWin, keycode=0)也能生效——其
     * modifier 仍被上报, 主机据此识别 Win/Ctrl 等组合(08-27 修复误杀纯修饰键)。 */
    for (int i = 0; i < HID_HELD_MAX; i++) {
        if (g_kb_held[i].ref <= 0) continue;
        modifier |= g_kb_held[i].modifier;
        if (g_kb_held[i].keycode != 0 && n < 6)
            keys[n++] = g_kb_held[i].keycode;
    }
    ble_hid_keyboard_send(modifier, keys, 6);
}

/* 清空按住集合(断连/全释放时调用) */
static void ble_hid_kb_clear_held(void)
{
    for (int i = 0; i < HID_HELD_MAX; i++) {
        g_kb_held[i].ref = 0;
        g_kb_held[i].modifier = 0;
        g_kb_held[i].keycode = 0;
    }
}

static void ble_hid_keyboard_send(uint8_t modifier, uint8_t *keys, uint8_t len)
{
    if (!g_hid_env.is_connected || !g_hid_env.kb_ntf_enabled) return;
    ble_hid_wait_tx_pkt();   /* 通话中 TX 池易满: 等归还再发, 防静默丢包 */
    uint8_t report[8]; report[0] = modifier; report[1] = 0x00;
    for (int i = 0; i < 6; i++) report[i + 2] = (i < len) ? keys[i] : 0x00;
    memcpy(g_hid_env.last_kbd_report, report, sizeof(report));
    sibles_value_t value = { .hdl = g_hid_env.hid_handle, .idx = HID_IDX_KB_REPORT_VAL, .len = sizeof(report), .value = report };
    sibles_write_value(g_hid_env.conn_idx, &value);
    ble_hid_kb_arm_retry();   /* 进入确认窗口: 未收到 RSP 则周期补发(见下) */
}

/* ===== 键盘报告冗余补发 (SCO 期间 BLE 数据可能滞留/丢失) =====
 * 实测: SCO 期间 sibles_write_value 提交后 WRITE_VALUE_RSP 会正常返回
 * result=0 —— 但那只代表 HCI 层"处理完成"(数据进 controller 队列),
 * 不代表空口送达。故"RSP 确认补发"形同虚设(确认太快, 从不触发补发)。
 * 改为固定冗余补发: 每次键盘报告提交后, 无论 RSP 如何, 间隔 500ms 再
 * 补发 KB_TX_RETRY_MAX-1 次, 让数据多次进 controller 队列, 提高 SCO
 * 射频空隙/结束后送达概率。新报告(状态变化)到来即重置计数; 断连停止。
 * 补发的是同一"状态"报告(按下/松开集合), Windows 按状态变化触发热键,
 * 重复提交相同状态不会重复触发, 安全。
 * 窗口设计(08-06 修正): 300ms×3 次 ≈ 900ms 覆盖常规延迟; 通话(SCO)中补发
 * 被跳过(射频被抢占+池不归还, 盲补发只占池), 改为 SCO 断开后由
 * ble_hid_flush_pending() 一次性补发最后状态(射频释放必送达)。 */
#define KB_TX_RETRY_MS 300
#define KB_TX_RETRY_MAX 4
static uint8_t g_kb_retry_left = 0;
static rt_timer_t g_kb_retry_timer = RT_NULL;

static void ble_hid_kb_retry_cb(void *param)
{
    (void)param;
    if (!g_kb_retry_left || !g_hid_env.is_connected) { g_kb_retry_left = 0; return; }
    /* ⚠️ 通话(SCO)中跳过补发：SCO 抢占射频 + sco_quiet 拉长连接事件, 包发不出去
     * 也不归还 TX 池 → 盲补发只会把 8 包池占满 → 之后所有 HID 报告 acquire 失败
     * 静默丢包 = "语音期间 HID 用不了"(08-06 实测复现)。跳过不减计数, 等 SCO
     * 结束后由 ble_hid_flush_pending() 一次性补发最后状态(射频已释放, 必送达)。 */
    extern volatile uint8_t g_sco_active;
    if (g_sco_active)
        return;
    ble_hid_wait_tx_pkt();   /* 非通话场景池归还快, 等 30ms 内归还即发 */
    sibles_value_t value = { .hdl = g_hid_env.hid_handle, .idx = HID_IDX_KB_REPORT_VAL,
                             .len = sizeof(g_hid_env.last_kbd_report),
                             .value = g_hid_env.last_kbd_report };
    sibles_write_value(g_hid_env.conn_idx, &value);
    g_kb_retry_left--;
    rt_kprintf("[KB] re-send left=%d\n", g_kb_retry_left);
    if (g_kb_retry_left) rt_timer_start(g_kb_retry_timer);
}

static void ble_hid_kb_arm_retry(void)
{
    if (!g_kb_retry_timer) {
        /* 必须 SOFT_TIMER: 回调里调 sibles_write_value, 硬定时器回调跑在
         * tick 中断上下文有 rt_malloc 断言风险(08-06 实测 fatal error). */
        g_kb_retry_timer = rt_timer_create("kb_retry", ble_hid_kb_retry_cb, RT_NULL,
                                           rt_tick_from_millisecond(KB_TX_RETRY_MS),
                                           RT_TIMER_FLAG_ONE_SHOT | RT_TIMER_FLAG_SOFT_TIMER);
        if (!g_kb_retry_timer) return;
    }
    g_kb_retry_left = KB_TX_RETRY_MAX - 1;   /* 原始提交已发 1 次, 冗余补发 MAX-1 次 */
    rt_timer_stop(g_kb_retry_timer);
    rt_timer_start(g_kb_retry_timer);
}

/* LE 栈下发结果(带 result). 仅诊断: RSP 不代表空口送达, 不用于停止冗余补发. */
void ble_hid_on_tx_rsp(uint8_t conn_idx, uint8_t result)
{
    (void)conn_idx;
    rt_kprintf("[KB] tx rsp result=%d\n", result);
}

/* 断连: 清冗余计数, 停补发 (由 ble_app.c 在 BLE_GAP_DISCONNECTED_IND 调用) */
void ble_hid_kb_tx_reset(void)
{
    g_kb_retry_left = 0;
    if (g_kb_retry_timer) rt_timer_stop(g_kb_retry_timer);
    ble_hid_kb_clear_held();   /* 08-27: 断连清按住集合, 防重连后残留 */
}

/* SCO(语音)断开后补发最后 HID 状态：通话中射频被 SCO 抢占, 键盘 up/鼠标按钮
 * 报告可能滞留/丢失(补发在通话中也因 TX 池被占而跳过)；挂断瞬间射频释放,
 * 重发 last 状态必然送达, 恢复对端一致性(PC 停止录音/按钮状态对齐)。
 * 同时清掉通话中被跳过的补发计数。由 ble_app.c 在 SCO_DISCONNECTED 调用。 */
void ble_hid_flush_pending(void)
{
    if (!g_hid_env.is_connected) return;
    g_kb_retry_left = 0;
    if (g_kb_retry_timer) rt_timer_stop(g_kb_retry_timer);
    /* 重发最后键盘报告(含 PTT 松开 up → PC 停止录音) */
    if (g_hid_env.kb_ntf_enabled) {
        sibles_value_t v = { .hdl = g_hid_env.hid_handle, .idx = HID_IDX_KB_REPORT_VAL,
                             .len = sizeof(g_hid_env.last_kbd_report),
                             .value = g_hid_env.last_kbd_report };
        sibles_write_value(g_hid_env.conn_idx, &v);
    }
    /* 重发最后鼠标报告(含按钮状态) */
    if (g_hid_env.ms_ntf_enabled) {
        sibles_value_t v = { .hdl = g_hid_env.hid_handle, .idx = HID_IDX_MS_REPORT_VAL,
                             .len = sizeof(g_hid_env.last_mouse_report),
                             .value = g_hid_env.last_mouse_report };
        sibles_write_value(g_hid_env.conn_idx, &v);
    }
}

static void ble_hid_mouse_send(int8_t dx, int8_t dy, uint8_t buttons)
{
    if (!g_hid_env.is_connected) return;
    if (!g_hid_env.ms_ntf_enabled) {
        rt_kprintf("[BLE HID] MS send skipped (ntf=0)\n");
        return;
    }
    ble_hid_wait_tx_pkt();   /* 通话中 TX 池易满: 等归还再发, 防静默丢包 */
    uint8_t report[5]; report[0] = buttons;
    report[1] = (uint8_t)(dx & 0xFF);
    report[2] = ((dy & 0x0F) << 4) | ((dx >> 8) & 0x0F);
    report[3] = (uint8_t)((dy >> 4) & 0xFF);
    report[4] = 0;
    memcpy(g_hid_env.last_mouse_report, report, sizeof(report));
    sibles_value_t value = { .hdl = g_hid_env.hid_handle, .idx = HID_IDX_MS_REPORT_VAL, .len = sizeof(report), .value = report };
    sibles_write_value(g_hid_env.conn_idx, &value);
}

void ble_hid_mouse_move(int16_t dx, int16_t dy, uint8_t button)
{
    if (!g_hid_env.is_connected) return;

    if (dx > 127) dx = 127;
    else if (dx < -127) dx = -127;

    if (dy > 127) dy = 127;
    else if (dy < -127) dy = -127;

    if(button != 0x00 && button !=0x01 && button !=0x02 && button !=0x04) button = 0x00;

    ble_hid_mouse_send((int8_t)dx, (int8_t)dy, button);
}


// buttons:left == 0x01,right == 0x02, middle == 0x04, release == 0x00
void ble_hid_mouse_button(uint8_t button)
{
    if (!g_hid_env.is_connected)return;
    ble_hid_mouse_send(0, 0,button);
}

static void ble_hid_mouse_scroll(int8_t wheel)
{
    if (!g_hid_env.is_connected || !g_hid_env.ms_ntf_enabled) return;
    ble_hid_wait_tx_pkt();   /* 通话中 TX 池易满: 等归还再发, 防静默丢包 */
    uint8_t report[5] = {0, 0, 0, 0, (uint8_t)wheel};
    memcpy(g_hid_env.last_mouse_report, report, sizeof(report));
    sibles_value_t value = { .hdl = g_hid_env.hid_handle, .idx = HID_IDX_MS_REPORT_VAL, .len = sizeof(report), .value = report };
    sibles_write_value(g_hid_env.conn_idx, &value);
}

void ble_hid_mouse_scroll_up(void)
{
    ble_hid_mouse_scroll(1);
}
void ble_hid_mouse_scroll_down(void)
{
    ble_hid_mouse_scroll(-1);
}

void ble_hid_keyboard_press_key(uint8_t modifier, uint8_t keycode)
{
    if (!g_hid_env.is_connected) return;
    /* keycode 0 + modifier 0 = 完全无键, 丢弃; 但 keycode 0 + modifier≠0 是合法
     * "纯修饰键"(如 C1 默认 LCtrl+LShift+LWin), 必须保留, 否则 C1 不发任何报告 */
    if (modifier == 0 && keycode == 0) return;

    /* 已按住则引用计数+1(状态未变, 仍重发一次确保对端一致), 否则找空槽填入 */
    int empty = -1;
    for (int i = 0; i < HID_HELD_MAX; i++) {
        if (g_kb_held[i].ref > 0 && g_kb_held[i].modifier == modifier
            && g_kb_held[i].keycode == keycode) {
            g_kb_held[i].ref++;
            ble_hid_kb_send_held();
            return;
        }
        if (empty < 0 && g_kb_held[i].ref == 0) empty = i;
    }
    if (empty < 0) return;   /* 集合已满(>8), 丢弃本次按下 */
    g_kb_held[empty].modifier = modifier;
    g_kb_held[empty].keycode  = keycode;
    g_kb_held[empty].ref      = 1;
    ble_hid_kb_send_held();
}

/* 释放单个键: 从集合移除(引用计数归零才真正移除)并发送剩余完整报告。
 * 这样 C2+C3 同时按下时松开 C2, 不会误释放仍按着的 C3。 */
void ble_hid_keyboard_release_key(uint8_t modifier, uint8_t keycode)
{
    if (!g_hid_env.is_connected) return;
    for (int i = 0; i < HID_HELD_MAX; i++) {
        if (g_kb_held[i].ref > 0 && g_kb_held[i].modifier == modifier
            && g_kb_held[i].keycode == keycode) {
            g_kb_held[i].ref--;
            if (g_kb_held[i].ref <= 0) {
                g_kb_held[i].ref = 0;
                g_kb_held[i].modifier = 0;
                g_kb_held[i].keycode  = 0;
            }
            break;
        }
    }
    ble_hid_kb_send_held();   /* 发送当前剩余(可能为空)的完整报告 */
}

void ble_hid_keyboard_release(void)
{
    if (!g_hid_env.is_connected) return;
    ble_hid_kb_clear_held();
    uint8_t empty[6] = {0};
    ble_hid_keyboard_send(0, empty, 6);
}

uint8_t ble_hid_is_connected(void)
{
    return g_hid_env.is_connected;
}

/* POWER_ON_IND 由 SDK 在 BLE 栈上电后发出，HID 线程据此注册服务并开广播。
 * 但温启动/唤醒(mode:4, 例如 sftool 烧录后复位)时，BLE 控制器被认为"已上电"，
 * SDK 不再重发该事件 → 服务永不注册、广播永不开启、手机搜不到设备。
 * 引入 BLE_HID_FORCE_INIT 兜底消息：若超时仍未就绪，由 app_bt_hid_init 的
 * 看门狗定时器补发，确保无论冷/温启动广播都能拉起。值刻意取 SDK 枚举之外。 */
#define BLE_HID_FORCE_INIT  0x1000

static uint8_t g_hid_ready = 0;   /* 服务已注册 + 广播已拉起；幂等保护 */

uint8_t ble_hid_is_ready(void)
{
    return g_hid_ready;
}

/* 注册 HID/BAS/LED 服务并启动广播。被 POWER_ON_IND 与 BLE_HID_FORCE_INIT 两条
 * 路径共用；g_hid_ready 保证只执行一次（兜底晚于事件到达时不会重复注册）。*/
static void ble_hid_do_init(void)
{
    if (g_hid_ready)
        return;
    g_hid_ready = 1;   /* 先置位，防止事件与兜底竞态下重复进入 */

    rt_kprintf("[BLE HID] init services + advertising\n");
    sibles_register_svc_t svc;
    svc.att_db = (struct attm_desc *)hid_att_db;
    svc.num_entry = HID_ATT_NB;
    svc.sec_lvl = PERM(SVC_AUTH, NO_AUTH) | PERM(SVC_UUID_LEN, UUID_16);
    svc.uuid = ATT_SVC_HID;
    g_hid_env.hid_handle = sibles_register_svc(&svc);
    if (g_hid_env.hid_handle)
    {
        sibles_register_cbk(g_hid_env.hid_handle, hid_gatts_get_cbk, hid_gatts_set_cbk);
        rt_kprintf("[BLE HID] HID service registered\n");
    }
    svc.att_db = (struct attm_desc *)bas_att_db;
    svc.num_entry = BAS_ATT_NB;
    svc.uuid = ATT_SVC_BATTERY_SERVICE;
    g_hid_env.bas_handle = sibles_register_svc(&svc);
    if (g_hid_env.bas_handle)
    {
        sibles_register_cbk(g_hid_env.bas_handle, bas_gatts_get_cbk, bas_gatts_set_cbk);
        rt_kprintf("[BLE HID] BAS service registered\n");
    }
    ble_led_register_service();

    /* 三设备切换: BLE 栈就绪, 配对模式先删当前槽位配对, 再广播重新配对 */
    extern void bt_multi_apply_mode(void);
    bt_multi_apply_mode();

    /* 诊断: 打印 SDK 从 Flash 恢复的配对设备数(KVDB 是否真的持久化了 bond) */
    {
        extern uint8_t connection_manager_get_bonded_devices(uint8_t *data);
        uint8_t _bond_buf[3 * 7 + 6];
        rt_memset(_bond_buf, 0, sizeof(_bond_buf));
        uint8_t _cnt = connection_manager_get_bonded_devices(_bond_buf);
        rt_kprintf("[BLE HID] boot: SDK bonded devices=%d\n", _cnt);
    }

    rt_kprintf("[BLE HID] starting advertising...\n");
    ble_app_advertising_start();
}

/* 温启动兜底：从任意线程(含定时器线程)调用，向 HID 线程补发一次初始化请求。*/
void ble_hid_ensure_init(void)
{
    if (g_hid_env.mb_handle)
        rt_mb_send(g_hid_env.mb_handle, BLE_HID_FORCE_INIT);
}

static void ble_hid_main_thread(void *param)
{
    rt_uint32_t value;
    while (1) {
        rt_mb_recv(g_hid_env.mb_handle, &value, RT_WAITING_FOREVER);
        if (value == BLE_POWER_ON_IND || value == BLE_HID_FORCE_INIT) {
            /* BLE 电源开启指示 或 温启动兜底：注册服务并启动广播 */
            ble_hid_do_init();
        }
    }
}

void ble_bas_init(void) { rt_kprintf("[BLE HID] BAS init\n"); }
void ble_bas_set_level(uint8_t percent) {
    if (percent > 100) percent = 100;
    batt_level = percent;
    if (g_hid_env.is_connected && g_hid_env.bas_ntf_enabled && g_hid_env.bas_handle) {
        sibles_value_t value = { .hdl = g_hid_env.bas_handle, .idx = BAS_IDX_BATT_LVL_VAL, .len = 1, .value = &batt_level };
        sibles_write_value(g_hid_env.conn_idx, &value);
    }
}

void ble_hid_init(void) {
    rt_kprintf("[BLE HID] init\n");
    g_hid_env.mb_handle = rt_mb_create("ble_hid", 8, RT_IPC_FLAG_FIFO);
    RT_ASSERT(g_hid_env.mb_handle);
    rt_thread_t tid = rt_thread_create("ble_hid", ble_hid_main_thread, RT_NULL, 4096, RT_THREAD_PRIORITY_MIDDLE - 1, 10);
    RT_ASSERT(tid);
    rt_thread_startup(tid);
}