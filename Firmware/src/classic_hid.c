#include <rtthread.h>
#include "cls_mouse_desc.h"

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
void bt_hid_mouse_drag_page(void *bts2_app_data, bts2_u8 conn_idx, bts2_u8 buttons,
                            short dx, short dy, signed char wheel_offset);

/* 经典链路是否可用：由 ble_app 在 HFP(经典 ACL) 连上/断开时置位。
 * 经典 HID 与 HFP 共用同一条 ACL，ACL 在就说明经典侧可发。 */
static volatile bts2_u8 s_cls_ready = 0;

void classic_hid_set_ready(bts2_u8 on)
{
    s_cls_ready = on ? 1 : 0;
}

bts2_u8 classic_hid_ready(void)
{
    /* ⚠️ 10-31 暂时强制返回 0：实测经典 HID 的【中断通道(PSM 0x13)】没建立
     * （日志 interrupt_cid = 0x0000），SDK 直接拒发 "[U-L]hid is not connect"。
     * 更糟的是控制通道随后也被断开(res=8)。此时若继续走经典，按键会全部丢失，
     * 且因为提前 return 也不回退 BLE —— 两条路都堵死。
     * 10-31 二轮: BLE HID 服务已关闭 ⇒ 不再有回退路径, 重新打开经典路由。 */
    return s_cls_ready && bts2g_app_p;
}

/* 键盘报告：格式与 BLE 侧完全一致（SDK 内部会拼 [header][report_id][modifier][0][key1..6]） */
void classic_hid_send_keyboard(bts2_u8 modifier, const bts2_u8 *keys, bts2_u8 key_num)
{
    if (!classic_hid_ready()) return;
    bt_hid_send_keyboard(bts2g_app_p, 0, modifier, keys, key_num);
}

/* 鼠标：位移 + 按键 + 滚轮 一次调用搞定（SDK 的 drag_page 覆盖三种语义） */
void classic_hid_send_mouse(bts2_u8 buttons, short dx, short dy, signed char wheel)
{
    if (!classic_hid_ready()) return;
    bt_hid_mouse_drag_page(bts2g_app_p, 0, buttons, dx, dy, wheel);
}

/* 多媒体键：bitmap 与 HID consumer page 一致（BLE 侧同样语义） */
void classic_hid_send_consumer(bts2_u16 bitmap)
{
    if (!classic_hid_ready()) return;
    bt_hid_send_consumer(bts2g_app_p, 0, bitmap);
}

/* 覆盖 SDK 的弱函数：键盘 + 鼠标 + 多媒体 三合一 */
void bt_hid_cmpose_hid_descriptor(void)
{
    /* 10-31 三轮: 只注册【键盘】一条描述符, 做最小化验证。
     * 依据: 主机开完 control(0x11) 就放弃, 从不建立 interrupt(0x13) ——
     * 典型症状是 SDP 里的报告描述符不被接受。
     * 先排除"多条描述符组合"这个变量, 只留最标准的 Boot Keyboard。 */
    bt_hid_add_descriptor(bts2s_sds_hid_device_svc_record_mid_keyboard,
                          (bts2_u8)bts2s_sds_hid_device_svc_record_mid_keyboard_len);
    /* ⚠️ 10-31 实测: keyboard + mouse 两条一起注册, 主机同样放弃中断通道
     * (interrupt_cid=0x0000, res=8) —— 与 keyboard + consumer 表现完全一致。
     * ⇒ SDK 这种"多条 SDP 记录"的方式不可用, 只能注册【一条】。
     * 鼠标暂缓: 正确做法是把键盘+鼠标的 report descriptor 合并成【一份】
     * 的复合描述符(不同 report ID 区分), 需要手改 SDK 的 SDP 记录字节。
     * 恢复鼠标时把下面两行取消注释。 */
    // bt_hid_add_descriptor(cls_mouse_desc, (bts2_u8)CLS_MOUSE_DESC_LEN);
    rt_kprintf("[CLS HID] descriptors: keyboard ONLY (mouse pending composite desc)\n");
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
