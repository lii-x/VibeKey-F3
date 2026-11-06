#include <rtthread.h>
#include "cls_hid_desc.h"

/*
 * 经典蓝牙 HID（BR/EDR）
 * ---------------------------------------------------------------
 * 背景：BLE HID 在 SCO（语音）期间会被射频调度饿死 —— 报告提交成功
 *       （tx rsp=0）但主机收不到，通话中按键基本不可用。经典 ACL 与 SCO
 *       走同一套时隙调度，不会互相抢，所以把 HID 搬到经典链路上。
 *
 * 本文件只负责【让电脑能发现并识别】这台设备：
 *   1. 覆盖 SDK 的 __WEAK bt_hid_cmpose_hid_descriptor()，注册复合描述符；
 *   2. 在蓝牙栈就绪后调用 bt_hid_open() 使能 HID 设备。
 *
 * ⚠️ SDK 默认只注册了 mouse + consumer，缺键盘 —— 这是必须覆盖的原因。
 * ⚠️ 本文件刻意【不 include SDK 的 bts2 头】：那套头依赖一堆基础类型与配置宏，
 *    直接引会把工程编译单元拖进 SDK 的内部世界。这里只声明用到的少数符号，
 *    它们在 SDK 的 .c 里有定义，链接期能解析。
 */

typedef unsigned char  bts2_u8;
typedef unsigned short bts2_u16;

/* 预置描述符（SDK 内部定义） */
extern const bts2_u8  bts2s_sds_hid_device_svc_record_mid_keyboard[];
extern const bts2_u16 bts2s_sds_hid_device_svc_record_mid_keyboard_len;
/* 鼠标描述符：SDK 只给了数组本身、没有导出长度符号，extern 声明不完整无法 sizeof。
 * 先只注册键盘 + 多媒体（本轮目标是验证"电脑能否识别经典 HID"）；
 * 鼠标留到下一轮：把 SDK 那份描述符拷进工程、带上明确长度即可。 */
// extern const bts2_u8  bts2s_sds_hid_device_svc_record_mid_mouse[];
// extern const bts2_u16 bts2s_sds_hid_device_svc_record_mid_mouse_len;
extern const bts2_u8  bts2s_sds_hid_device_svc_record_mid_consumer[];
extern const bts2_u16 bts2s_sds_hid_device_svc_record_mid_consumer_len;

/* SDK 内部的 app 上下文（bts2_app_hid.c 里就是 bts2_app_data = bts2g_app_p） */
extern void *bts2g_app_p;

/* SDK 接口原型（bts2_app_hid.c） */
void bt_hid_add_descriptor(const bts2_u8 *data, bts2_u8 len);
void bt_hid_open(void);
void bt_hid_send_keyboard(void *bts2_app_data, bts2_u8 conn_idx,
                          bts2_u8 modifier, const bts2_u8 *keys, bts2_u8 key_num);
void bt_hid_send_consumer(void *bts2_app_data, bts2_u8 conn_idx, bts2_u16 bitmap);
/* ⚠️ 刻意【不声明】bt_hid_mouse_drag_page：它用报告 ID 2，而 ID 2 是我们的禁区
 *    （SDK 的连接校准报文占着它，见 ble_app.c 里 bt_classic_hid_send_mouse_raw 的注释）。
 *    鼠标一律走 bt_classic_hid_send_mouse_raw（报告 ID 3）。 */

/* 经典链路是否可用：由 ble_app 在 HFP(经典 ACL) 连上/断开时置位。
 * 经典 HID 与 HFP 共用同一条 ACL，ACL 在就说明经典侧可发。 */
static volatile bts2_u8 s_cls_ready = 0;

/* HID 通道(控制 PSM 0x11 + 中断 PSM 0x13)是否已建立。
 * ★ 2026-10-31 新增：ACL 通 ≠ HID 通。实测"首次配对能用、重启后再也用不了"时，
 *   日志里 ACL/HFP 都正常，却【完全没有】`remote connect HID control channel` ——
 *   主机(Windows)在设备重启后根本不会主动重开 HID，只有首次配对/手动"连接"时才开。
 *   所以必须由设备侧主动请求，与真实蓝牙键盘的行为一致。 */
static volatile bts2_u8 s_hid_linked = 0;

/* 主动请求建立 HID 通道。定义在 ble_app.c(那里有 SDK 头 + 对端地址)，
 * 本文件只保留"要不要发"的判据，避免为了一个函数把 SDK 整套头拖进来。 */
extern void bt_classic_hid_kick_connect(void);

void classic_hid_set_linked(bts2_u8 on)
{
    if (s_hid_linked != (on ? 1 : 0))
        rt_kprintf("[CLS HID] link %s\n", on ? "UP" : "DOWN");
    s_hid_linked = on ? 1 : 0;
}

bts2_u8 classic_hid_linked(void)
{
    return s_hid_linked;
}

void classic_hid_set_ready(bts2_u8 on)
{
    s_cls_ready = on ? 1 : 0;
}

bts2_u8 classic_hid_ready(void)
{
    bts2_u8 ok = (s_cls_ready && bts2g_app_p) ? 1 : 0;

    /* 发送失败时不再静默丢包：限速打一条日志，方便区分三种情况 ——
     *   ① 根本没走到经典（日志里没有 [KB] -> classic）
     *   ② 走了但 ACL 没就绪（下面这条 NOT-READY）
     *   ③ 走了、ACL 也通，但 HID 通道没建起来（下面这条 NO-LINK，会顺手踢一次连接） */
    if (!ok) {
        static rt_tick_t last = 0;
        rt_tick_t now = rt_tick_get();
        if (last == 0 || (now - last) > rt_tick_from_millisecond(2000)) {
            last = now;
            rt_kprintf("[CLS HID] NOT-READY (acl=%d app=%p)\n",
                       (int)s_cls_ready, bts2g_app_p);
        }
    }
    return ok;
}

/* 发送前的统一闸门：ACL 通 就发。
 * HID 通道没建立时【顺手踢一次主动连接】，但**不阻断本次发送** ——
 * 交给 SDK 自己判(hid 未连接时它会拒发并打 "[U-L]hid is not connect")，
 * 这样即使我们的 linked 标志因事件没到而偏旧，也不会把本来能发的键吞掉。 */
static bts2_u8 hid_tx_gate(void)
{
    if (!classic_hid_ready()) return 0;
    if (!s_hid_linked) bt_classic_hid_kick_connect();
    return 1;
}

/* 键盘报告：格式与 BLE 侧完全一致（SDK 内部会拼 [header][report_id][modifier][0][key1..6]） */
void classic_hid_send_keyboard(bts2_u8 modifier, const bts2_u8 *keys, bts2_u8 key_num)
{
    if (!hid_tx_gate()) return;
    bt_hid_send_keyboard(bts2g_app_p, 0, modifier, keys, key_num);
}

/* 鼠标：位移 + 按键 + 滚轮。
 * ★ 10-31 改为自己组包（ble_app.c 的 bt_classic_hid_send_mouse_raw），原因见那边注释：
 *   SDK 的 bt_hid_mouse_drag_page 用报告 ID 2，而 SDK 每次连上 HID 都会用 ID 2 发
 *   "把光标拽到屏幕角"的校准报文；我们的描述符刻意不定义 ID 2，鼠标改走 ID 3。 */
void classic_hid_send_mouse(bts2_u8 buttons, short dx, short dy, signed char wheel)
{
    extern void bt_classic_hid_send_mouse_raw(unsigned char, int, int, int);
    if (!hid_tx_gate()) return;
    bt_classic_hid_send_mouse_raw(buttons, dx, dy, wheel);
}

/* 多媒体键：bitmap 与 HID consumer page 一致（BLE 侧同样语义） */
void classic_hid_send_consumer(bts2_u16 bitmap)
{
    if (!hid_tx_gate()) return;
    bt_hid_send_consumer(bts2g_app_p, 0, bitmap);
}

/* 覆盖 SDK 的弱函数：键盘 + 鼠标 + 多媒体 三合一 */
void bt_hid_cmpose_hid_descriptor(void)
{
    /* ★ 10-31 复合描述符：键盘(Report ID 1) + 鼠标(Report ID 2) 写在【一条】SDP 记录里。
     * 实测多次 bt_hid_add_descriptor 会被主机拒绝，合成单条才能建起中断通道。 */
    bt_hid_add_descriptor(cls_hid_desc, (bts2_u8)CLS_HID_DESC_LEN);
    rt_kprintf("[CLS HID] composite descriptor: keyboard(id1) + mouse(id3), %d bytes\n",
               CLS_HID_DESC_LEN);
}

/* 蓝牙栈就绪后调用：使能 HID 设备（SDK 里只有 finsh 命令会调，产品固件必须自己调） */
void classic_hid_open(void)
{
    if (!bts2g_app_p) {
        rt_kprintf("[CLS HID] open skipped: bts2g_app_p is NULL\n");
        return;
    }
    bt_hid_open();
    rt_kprintf("[CLS HID] bt_hid_open() called, HID device enabled\n");
}
