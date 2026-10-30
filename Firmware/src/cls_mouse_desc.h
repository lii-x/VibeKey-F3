#ifndef __CLS_MOUSE_DESC_H__
#define __CLS_MOUSE_DESC_H__

/* 10-31: SDK 的鼠标 SDP 记录数组，原样拷入工程。
 * 原因: SDK 只导出数组本身、没有 ..._mouse_len，extern 声明不完整无法 sizeof；
 *       拷进来才能拿到明确长度注册给 bt_hid_add_descriptor。
 * 源: SiFli-SDK/.../bt_finsh/bts2_app_hid.c */

#define CLS_MOUSE_DESC_LEN  80

extern const unsigned char cls_mouse_desc[CLS_MOUSE_DESC_LEN];

#endif
