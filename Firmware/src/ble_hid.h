#ifndef __BLE_HID_H__
#define __BLE_HID_H__

#include <stdint.h>
#include "bf0_sibles.h"
typedef struct {
    uint8_t is_connected;           // 连接标志，0=未连接，1=已连接
    uint8_t conn_idx;               // 连接索引，用于标识不同的连接实例
    uint8_t peer_addr[6];           // 对端设备MAC地址（6字节蓝牙地址）
    uint8_t kb_ntf_enabled;         // 键盘通知使能标志（CCC descriptor配置）
    uint8_t ms_ntf_enabled;         // 鼠标通知使能标志
    uint8_t bas_ntf_enabled;        // 电池服务通知使能标志
    sibles_hdl hid_handle;          // HID服务句柄
    sibles_hdl bas_handle;          // 电池服务句柄
    uint8_t last_kbd_report[8];     // 最后一次键盘报告
    uint8_t last_mouse_report[5];   // 最后一次鼠标报告
    rt_mailbox_t mb_handle;         // 邮箱句柄
    struct rt_timer *hfp_timer;
} ble_hid_env_t;

/* BLE HID */
void ble_hid_init(void);
uint8_t ble_hid_is_ready(void);     /* 服务已注册+广播已拉起(温启动兜底判定) */
void ble_hid_ensure_init(void);     /* 补发一次初始化(从任意线程安全调用) */
uint8_t ble_hid_is_connected(void);
void ble_hid_mouse_move(int16_t dx, int16_t dy, uint8_t button);
void ble_hid_mouse_button(uint8_t button);
void ble_hid_mouse_scroll_up(void);
void ble_hid_mouse_scroll_down(void);
void ble_hid_keyboard_press_key(uint8_t modifier, uint8_t keycode);
void ble_hid_keyboard_release_key(uint8_t modifier, uint8_t keycode);
void ble_hid_keyboard_release(void);
/* 08-27 多键修复: press/release_key 各自增删"按住集合"中的一个键并发送完整报告,
 * 支持同时按下多个键(如 C2+C3); release() 为兼容接口, 释放全部(断连清理用)。 */
/* 键盘报告 RSP 确认+补发(防 SCO 期间滞留丢失):
 * ble_hid_on_tx_rsp 由 ble_app 在 SIBLES_WRITE_VALUE_RSP 事件调用;
 * ble_hid_kb_tx_reset 由 ble_app 在 BLE 断开时调用清 pending. */
void ble_hid_on_tx_rsp(uint8_t conn_idx, uint8_t result);
void ble_hid_kb_tx_reset(void);
/* SCO(语音)断开后补发最后键盘/鼠标状态(射频释放必送达), 由 ble_app 在
 * SCO_DISCONNECTED 调用; 通话中补发被跳过, 靠本函数恢复对端一致性 */
void ble_hid_flush_pending(void);

#endif
