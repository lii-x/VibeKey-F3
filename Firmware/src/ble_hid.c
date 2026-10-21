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

static void ble_hid_kb_send_held(void);   /* 前向声明: 供 CCCD 订阅回调做重同步 */

static uint8_t hid_gatts_set_cbk(uint8_t conn_idx, sibles_set_cbk_t *para)
{
    switch (para->idx) {
    case HID_IDX_KB_REPORT_NTF_CFG:
        g_hid_env.kb_ntf_enabled = *(para->value);
        /* ★ 2026-10-12: 主机订阅键盘通知(重连/首次)后【按当前按住集合重同步】一次。
         * 场景: 长按式语音(按住=ALT down, 松开=ALT up) —— 若 BLE 在按住期间掉线,
         * "松开"发不出去, 电脑会一直以为 ALT 按着 ⇒ 语音输入掐不掉 + ALT 卡住。
         * 断连期间 press/release 仍维护按住集合(见下), 故此处重建的即是【当前真实状态】:
         *   · 已松开 ⇒ 报告为空 ⇒ 电脑收到 ALT up ⇒ 语音停止 ✓
         *   · 仍按着 ⇒ 报告含 ALT ⇒ 语音继续 ✓
         * ⚠️ 必须用 send_held 重建, 不能重发 last_kbd_report —— 后者在断连期间
         *    (keyboard_send 早退)并未更新, 是过期状态。 */
        if (g_hid_env.kb_ntf_enabled)
            ble_hid_kb_send_held();
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

/* ===== 多键同时按下支持 (08-27 初版; 2026-10-12 改为"按物理键源") =====
 * 维护"当前按住的 HID 键集合", 每次变化都从集合重建【完整】6 键报告发送 ——
 * 这样同时按 C2+C3 会发 [C2, C3, 0, 0, 0, 0], 而非后按覆盖先按。
 *
 * ★ 2026-10-12 重构: 原实现按 (modifier,keycode) 记【引用计数】, 依赖"按下/松开
 *   严格配对"。但按键事件邮箱在狂按时会丢事件(main.c:2003 注释自述"邮箱满时丢弃"),
 *   丢一次"松开"就永久多算一次 ⇒ 该项 ref 永远 >0 ⇒ 报告里永远含该键 ⇒ 主机认为
 *   它一直按着、状态不再变化 ⇒ **"电脑收不到数据"**(实测 k=0x0f 卡在 r=5), 只能重启。
 *   改为按【物理按键源】记"是否按下"(布尔, 幂等):
 *     · 幂等 —— 同一源重复按下只是重发报告, 不存在累加;
 *     · 自愈 —— 任何一次丢事件都会在下一次按/松时被纠正, 不会永久失配;
 *     · 多键同码仍正确 —— 不同源各占一项, 松开一个不影响另一个(08-27 的诉求);
 *     · 代价 —— 同码需【不同源】才区分; 旧接口(按码自动选槽)在"两个不同物理键
 *       映射到同一码"时会共用一项(见 ble_hid_keyboard_press_key 注释)。
 *   src 由调用方指定(一般用按键索引), 见 ble_hid.h。 */
#define KB_SRC_MAX 12
typedef struct { uint8_t modifier; uint8_t keycode; uint8_t down; } kb_src_t;
static kb_src_t g_kb_src[KB_SRC_MAX];

/* 从集合重建并发送完整键盘报告(所有当前按下的键 + OR 后的 modifier) */
static void ble_hid_kb_send_held(void)
{
    uint8_t modifier = 0;
    uint8_t keys[6] = {0};
    int n = 0;
    /* 遍历全部按下项: modifier 一律 OR 进报告; 仅 keycode≠0 的填入 6 键数组。
     * 这样"纯修饰键"(如 C1 默认 LCtrl+LShift+LWin, keycode=0)也能生效——其
     * modifier 仍被上报, 主机据此识别 Win/Ctrl 等组合(08-27 修复误杀纯修饰键)。 */
    for (int i = 0; i < KB_SRC_MAX; i++) {
        if (!g_kb_src[i].down) continue;
        modifier |= g_kb_src[i].modifier;
        if (g_kb_src[i].keycode != 0 && n < 6)
            keys[n++] = g_kb_src[i].keycode;
    }
    ble_hid_keyboard_send(modifier, keys, 6);
}

/* 清空按住集合(断连/全释放时调用) */
static void ble_hid_kb_clear_held(void)
{
    for (int i = 0; i < KB_SRC_MAX; i++) {
        g_kb_src[i].modifier = 0;
        g_kb_src[i].keycode = 0;
        g_kb_src[i].down = 0;
    }
}

/* ⚠️ kb_held_dump() 的定义必须放在 KB_TX_LOG 宏【之后】(见下方 keyboard_send 前),
 * 否则 #if KB_TX_LOG 看不到宏 -> 整段被静默编译掉(这个坑本文件已踩两次)。 */

/* ⚠️ 2026-10-12 临时诊断开关: 打印每一次键盘报告的【实际内容】与【被丢弃的原因】。
 * 起因: 用户"单击 C1 一次, 语音却自己断了"，而从 `[KB] tx rsp` 数不出真实发送条数
 * (该日志只在写成功时出现, 发失败/被跳过不留痕) ⇒ 无法判断电脑到底收到几次 ALT。
 *   1 = 开启(每次按键打 1~2 行; 调试完请改回 0, 否则打字会刷屏)
 *   0 = 关闭
 * ⚠️ 必须定义在 ble_hid_keyboard_send() 定义之前, 否则 #if 看不到宏会静默编译掉。 */
#define KB_TX_LOG 1

#if KB_TX_LOG
/* ★ 2026-10-12 临时诊断: 打印"当前按住集合"快照(按物理键源)。
 * 用途: 用户报"多按几次后电脑收不到数据" —— 实测为集合里某项永久停留
 * (旧引用计数模型下 k=0x0f 卡在 r=5) ⇒ 报告恒含该键 ⇒ 主机认为它一直
 * 按着、状态不再变化 ⇒ 无按键输出。
 * 判读: `total=` 应随按/松回到 0; 若非 0 则说明有键被卡住, 方括号里的
 * `[src]m=.. k=..` 直接指出是哪个源、哪个键码。
 * 另两个关键标记: press-FULL = 集合满导致按下被丢弃; rel-MISS = 松开没找到对应项。 */
static void kb_held_dump(const char *tag)
{
    int n = 0;
    rt_kprintf("[KB] held@%s:", tag);
    for (int i = 0; i < KB_SRC_MAX; i++) {
        if (g_kb_src[i].down) {
            rt_kprintf(" [%d]m=%02x k=%02x", i, g_kb_src[i].modifier, g_kb_src[i].keycode);
            n++;
        }
    }
    rt_kprintf(" total=%d/%d\n", n, KB_SRC_MAX);
}
#else
#define kb_held_dump(tag) ((void)0)
#endif

static void ble_hid_keyboard_send(uint8_t modifier, uint8_t *keys, uint8_t len)
{
    if (!g_hid_env.is_connected || !g_hid_env.kb_ntf_enabled) {
#if KB_TX_LOG
        /* 被丢弃也要留痕: 否则"没发出去"和"发了没回执"在日志上完全一样 */
        rt_kprintf("[KB] TX-SKIP mod=0x%02x (conn=%d ntf=%d)\n",
                   modifier, g_hid_env.is_connected, g_hid_env.kb_ntf_enabled);
#endif
        return;
    }
    ble_hid_wait_tx_pkt();   /* 通话中 TX 池易满: 等归还再发, 防静默丢包 */
    uint8_t report[8]; report[0] = modifier; report[1] = 0x00;
    for (int i = 0; i < 6; i++) report[i + 2] = (i < len) ? keys[i] : 0x00;
    memcpy(g_hid_env.last_kbd_report, report, sizeof(report));
#if KB_TX_LOG
    rt_kprintf("[KB] TX mod=0x%02x keys=%02x %02x %02x %02x %02x %02x\n",
               report[0], report[2], report[3], report[4], report[5], report[6], report[7]);
#endif
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
/* ⚠️⚠️ 2026-10-12 临时测试开关（用户要求："先不补发，正常按几次就发几次"）:
 *   1 = 关闭键盘报告冗余补发 —— 每次按键/松开只发 1 条报告, 不再补 KB_TX_RETRY_MAX-1 次。
 *   0 = 恢复原行为（原始 1 次 + 每 300ms 补发, 共 4 次）。
 * 测试完把这里改回 0 即可, 其余代码无需改动。
 * 代价提醒: 关掉后单条报告若在空口丢失 = "这次按键没生效"(无重试兜底)。
 * 注意 SCO(语音)期间本来就跳过补发, 所以本开关主要影响【非 SCO 时点】——
 * 而"按 C1 启动语音"那一下恰在最需要送达的时刻, 测试时请留意是否偶发不触发。 */
#define KB_TX_RETRY_DISABLED 1

/* 临时诊断开关 KB_TX_LOG 定义在 ble_hid_keyboard_send() 之前(约文件上方),
 * 必须早于使用点, 否则 #if 看不到宏会整段编译掉。 */
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
    if (KB_TX_RETRY_DISABLED) {
        g_kb_retry_left = 0;   /* 测试开关: 不补发, 一次按键 = 一条报告 */
        return;
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
    /* ★ 2026-10-12: 【不再】清按住集合!
     * 原实现 "08-27: 断连清按住集合, 防重连后残留" 会引入一个更糟的后果:
     * 设备把自己也"忘了" —— 长按式按键(如 C1=ALT)在断连期间用户松开时,
     * release_key 因集合已被清空而找不到项, 重连后也无从补发 "ALT up",
     * 电脑端修饰键永久卡住、语音输入掐不掉(实测场景)。
     * 改为: 断连保留集合(press/release 在断连期间仍维护它, 见其实现),
     * 重连/订阅后由 hid_gatts_set_cbk 按当前集合重建下发, 天然自愈。 */
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

/* ===== 键盘按下/释放 =====
 * 两条接口:
 *   press_src/release_src —— 按【物理键源】索引(推荐): 幂等、可自愈、同码不同键
 *                            互不影响。src 由调用方给(一般用按键索引)。
 *   press_key/release_key —— 兼容旧调用(只给 modifier/keycode): 自动选槽(同码且
 *                            已按下则复用该槽, 否则取空闲槽)。同样幂等 ⇒ 丢事件
 *                            也能自愈; ⚠️ 但"两个不同物理键映射到同一
 *                            (modifier,keycode)"时会共用一项, 松开其一会把另一个
 *                            也放掉 —— 这类配置请改用 press_src/release_src。 */
void ble_hid_keyboard_press_src(uint8_t src, uint8_t modifier, uint8_t keycode)
{
    if (src >= KB_SRC_MAX) return;
    if (modifier == 0 && keycode == 0) return;
    /* 幂等: 已按下也只是重发报告, 不累加 —— 丢事件不会永久失配 */
    g_kb_src[src].modifier = modifier;
    g_kb_src[src].keycode  = keycode;
    g_kb_src[src].down     = 1;
    ble_hid_kb_send_held();
    kb_held_dump("press");
}

void ble_hid_keyboard_release_src(uint8_t src)
{
    if (src >= KB_SRC_MAX) return;
    int was = g_kb_src[src].down;
    g_kb_src[src].down = 0;   /* 幂等: 本来没按也无妨 */
    ble_hid_kb_send_held();
    kb_held_dump(was ? "rel" : "rel-MISS");
}

void ble_hid_keyboard_press_key(uint8_t modifier, uint8_t keycode)
{
    /* ★ 2026-10-12: 【不再】因未连接而直接 return —— 断连期间仍须维护按住集合,
     * 否则重连后无法判断该补发哪些键(长按式按键会永久卡在主机侧)。
     * 真正的发送由 ble_hid_keyboard_send() 内部按 is_connected/kb_ntf_enabled 保护。 */
    if (modifier == 0 && keycode == 0) return;

    int slot = -1;
    for (int i = 0; i < KB_SRC_MAX; i++) {
        if (!g_kb_src[i].down) continue;
        if (g_kb_src[i].modifier == modifier && g_kb_src[i].keycode == keycode) { slot = i; break; }
    }
    if (slot < 0)
        for (int i = 0; i < KB_SRC_MAX; i++)
            if (!g_kb_src[i].down) { slot = i; break; }
    if (slot < 0) {
        /* 全部槽都被占(KB_SRC_MAX 个键同时按住): 丢弃本次按下 */
        kb_held_dump("press-FULL");
        return;
    }
    g_kb_src[slot].modifier = modifier;
    g_kb_src[slot].keycode  = keycode;
    g_kb_src[slot].down     = 1;   /* 幂等, 不再引用计数 */
    ble_hid_kb_send_held();
    kb_held_dump("press");
}

/* 释放单个键: 从集合移除(引用计数归零才真正移除)并发送剩余完整报告。
 * 这样 C2+C3 同时按下时松开 C2, 不会误释放仍按着的 C3。 */
void ble_hid_keyboard_release_key(uint8_t modifier, uint8_t keycode)
{
    /* ★ 2026-10-12: 同 press_key —— 断连期间也要把"松开"记进集合(不发送),
     * 这样重连后重同步才能正确地把 ALT up 补发给电脑。 */
    int hit = 0;
    for (int i = 0; i < KB_SRC_MAX; i++) {
        if (g_kb_src[i].down && g_kb_src[i].modifier == modifier
            && g_kb_src[i].keycode == keycode) {
            g_kb_src[i].down = 0;
            hit = 1;
            break;
        }
    }
    ble_hid_kb_send_held();   /* 发送当前剩余(可能为空)的完整报告 */
    /* "rel-MISS" = 松开时没找到对应项。★ 在【幂等】模型下这已不再致命:
     * 下一次按下/松开会把状态纠正过来(旧引用计数模型下它会永久失配, 那正是
     * "电脑收不到数据"的成因)。保留该标记只为观察事件是否成对。 */
    kb_held_dump(hit ? "rel" : "rel-MISS");
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