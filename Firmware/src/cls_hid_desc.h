#ifndef __CLS_HID_DESC_H__
#define __CLS_HID_DESC_H__

/* 10-31: 经典 HID 的【复合】SDP 记录 —— 键盘(Report ID 1) + 鼠标(Report ID 2)。
 *
 * 为什么必须合成一条：
 *   实测 SDK 的多次 bt_hid_add_descriptor() 方式不可用 —— keyboard+consumer、
 *   keyboard+mouse 两种组合都会让主机放弃建立 interrupt 通道
 *   (interrupt_cid=0x0000, res=8)，只有【单条】SDP 记录能成功。
 *   所以把两份 report descriptor 拼进同一条记录，用 report ID 区分。
 *
 * 结构（与 SDK 原件一致）：
 *   09 02 06 | 35 L1 | 35 L2 | 08 22 25 L3 | <report descriptor>
 *   三层嵌套长度：L3=描述符长度, L2=L3+4, L1=L2+2
 */

#define CLS_HID_DESC_LEN  134

extern const unsigned char cls_hid_desc[CLS_HID_DESC_LEN];

#endif
