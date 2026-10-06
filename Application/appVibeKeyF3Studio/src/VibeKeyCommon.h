#ifndef VIBEKEYCOMMON_H
#define VIBEKEYCOMMON_H

#include <QByteArray>

/* VibeKey 设备的 USB Vendor ID（SiFli 0x38f4），用于从所有 COM 口中认出我们的设备 */
static const quint16 VIBEKEY_VID = 0x38f4;

/* 握手查询指令：CONF_MAGIC(4) + cmd(1)=0x05 + 保留(1) + data_len(2)=0，小端发送 */
static const QByteArray VIBEKEY_QUERY = QByteArray::fromRawData(
    "\x46\x4e\x4f\x43\x05\x00\x00\x00", 8);

#endif // VIBEKEYCOMMON_H
