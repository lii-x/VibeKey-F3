import serial
import struct
import sys
import time
import zlib

# ⚠️ 08-27: Windows 下被 QProcess 管道捕获时, Python 默认按 locale(GBK/cp936)编码输出
# stdout → Qt 端 QString::fromUtf8 解析出乱码("????"). 强制 stdout 为 UTF-8 与 Qt 对齐。
try:
    sys.stdout.reconfigure(encoding='utf-8')
    # 10-04: stderr 也要设! C++ 侧用 QString::fromUtf8() 读 stderr，
    # 不设则中文日志输出成 "??" 完全不可读(实测: BLE 服务 denied 的原因说明全变问号)
    sys.stderr.reconfigure(encoding='utf-8')
except AttributeError:
    pass  # Python < 3.7: 保持默认

OTA_MAGIC = 0x4F544100
CONF_MAGIC = 0x434F4E46      # "CONF"
CONF_CMD_INFO = 0x05         # 设备信息查询, 回包格式 "VibeKey-F3|<版本>|<电量>\n"

# OTA 分区上限: 0x12C00000(按键配置区起点) - 0x12680000(DFU 暂存区起点)
OTA_REGION_LIMIT = 0x12C00000 - 0x12680000

# 本设备固件镜像的专属标识串(内嵌于固件 rodata, 由 OTA_MODEL_TAG 写入)。
# 选普通长串以避免与常见字符串(如 "VibeKey-F3")碰撞导致误过。
KNOWN_MODELS = [b"VIBEKEYF3-FWIMG-7F3A9C21"]

def extract_model(fw_data):
    """从固件镜像二进制中提取内嵌的型号标识串; 找不到返回 None。"""
    for m in KNOWN_MODELS:
        if m in fw_data:
            return m.decode("utf-8", errors="ignore")
    return None

def is_valid_fw_image(fw_data):
    """结构校验: VibeKey-F3 基于 SF32LB52(Cortex-M33), XIP 基址 0x12020000,
    RAM 约 0x20000000 起。合法固件开头是 ARMv7-M 向量表:
      word[0]=初始 MSP(应在 RAM 范围); word[1]=Reset 向量(应在 Flash XIP, bit0=1=Thumb)。
    非固件文件(txt/png/zip/python/exe)几乎不可能满足 => 直接拒绝,
    避免"随便选个文件"被误当固件发送导致变砖。"""
    if len(fw_data) < 8:
        return False
    sp, reset = struct.unpack("<II", fw_data[:8])
    # 初始栈指针须在 RAM 区域 (SF32LB52 RAM 起始 0x20000000)
    if not (0x20000000 <= sp <= 0x30000000):
        return False
    # Reset 向量须指向 Flash XIP 且为 Thumb (bit0=1)
    if not (0x12000000 <= (reset & ~1) <= 0x13FFFFFF):
        return False
    return True

def send_info_and_read(ser, timeout=2):
    """发 CONF_CMD_INFO 并读取一行响应, 返回版本字符串或 None。"""
    pkt = struct.pack("<IBBH", CONF_MAGIC, CONF_CMD_INFO, 0, 0)
    try:
        ser.write(pkt)
        ser.flush()
    except Exception:
        return None
    ser.timeout = timeout
    try:
        line = ser.readline()
    except Exception:
        return None
    if not line:
        return None
    try:
        text = line.decode("utf-8", errors="ignore").strip()
    except Exception:
        return None
    # 格式: "VibeKey-F3|<版本>|<电量>"
    parts = text.split("|")
    if len(parts) >= 2 and parts[0] == "VibeKey-F3":
        return (parts[0], parts[1])
    return None

def ota_upgrade(port, firmware_path):
    with open(firmware_path, "rb") as f:
        fw_data = f.read()

    total_size = len(fw_data)
    print(f"SIZE:{total_size}", flush=True)

    # PC 端预检: 固件过大直接拒绝, 不发送, 避免无谓传输后才在设备侧被拒
    # 预留 8(头) + 4(CRC 尾部) 开销
    if total_size + 8 + 4 > OTA_REGION_LIMIT:
        print(f"ERROR:固件过大 ({total_size} 字节) 超过 OTA 分区上限 {OTA_REGION_LIMIT} 字节", flush=True)
        sys.exit(3)

    # OTA 前读取当前设备型号与版本, 用于升级后比对(判断"是否真升级成功")
    old_model = None
    old_version = None
    try:
        pre = serial.Serial(port, 115200, timeout=3)
        time.sleep(0.2)
        info = send_info_and_read(pre, timeout=3)
        if info:
            old_model, old_version = info
        pre.close()
    except Exception as e:
        print(f"STATUS:升级前读取设备信息失败(将跳过比对): {e}", flush=True)

    # PC 端结构校验: 挡掉所有"非固件文件"(txt/png/zip/python/exe 等)。
    # 不依赖任何字符串, 避免"随便选个文件"被误当固件发送导致变砖。
    if not is_valid_fw_image(fw_data):
        print("ERROR:所选文件不是有效的固件镜像(向量表校验失败)，请选择正确的 .bin 固件", flush=True)
        sys.exit(5)

    # PC 端型号预检: 从 .bin 提取内嵌的固件专属型号标识, 确认是本设备固件。
    # 不含本设备专属标识(如选了其他板子的固件)直接拒绝发送。
    file_model = extract_model(fw_data)
    if file_model is None:
        print("ERROR:固件文件不含本设备型号标识，可能不是 VibeKey-F3 固件", flush=True)
        sys.exit(4)

    header = struct.pack("<II", OTA_MAGIC, total_size)

    # HIGH-2: 端到端 CRC32(覆盖固件体, 不含头), 尾部追加 4 字节小端 CRC,
    # 设备侧(dfu_device.c 收齐后)与 bootloader(安装前)均会校验, 坏文件被拦/不装, 防变砖。
    crc = zlib.crc32(fw_data) & 0xFFFFFFFF
    payload = fw_data + struct.pack("<I", crc)

    ser = serial.Serial(port, 115200, timeout=5, write_timeout=5)
    time.sleep(0.1)

    print("STATUS:Sending header...", flush=True)
    ser.write(header)
    time.sleep(0.1)

    chunk_size = 512
    sent = 0
    retries = 0
    while sent < len(payload):
        chunk = payload[sent:sent + chunk_size]
        try:
            # ⚠️ 按实际写入字节数累加(08-27): pyserial 的 write() 在设备端背压/
            # 部分超时下可能【只写入一部分】就返回(不抛异常)。原来 sent += len(chunk)
            # 按整块计, 会把未写出的字节跳过 → 固件流错位 → 设备端 CRC32 校验失败
            # (OTA CRC mismatch, 升级回退旧固件)。按返回值累加, 下轮从正确偏移继续。
            written = ser.write(chunk)
            if written <= 0:
                retries += 1
                if retries > 20:
                    print(f"ERROR:No data written at {sent}/{len(payload)}", flush=True)
                    ser.close()
                    sys.exit(1)
                time.sleep(0.05)
                continue
            sent += written
            retries = 0
        except serial.SerialTimeoutException:
            retries += 1
            if retries > 20:
                print(f"ERROR:Write timeout at {sent}/{len(payload)}", flush=True)
                ser.close()
                sys.exit(1)
            time.sleep(0.05)
            continue
        pct = sent * 100 // len(payload)
        print(f"PROGRESS:{pct}", flush=True)

    print("STATUS:固件已发送(含CRC), 等待设备重启...", flush=True)
    ser.close()
    time.sleep(5)   # 等待 reboot + USB 重新枚举

    # 重握手验证: 设备应已用新固件重新上线; 若超时无响应说明可能变砖
    print("STATUS:验证设备是否重新上线...", flush=True)
    new_version = None
    for attempt in range(1, 16):
        try:
            vser = serial.Serial(port, 115200, timeout=2)
            time.sleep(0.3)
            info = send_info_and_read(vser, timeout=2)
            vser.close()
            if info is not None:
                new_model, new_version = info
                break
        except Exception:
            pass
        time.sleep(1)

    if new_version is None:
        print("VERIFY_FAIL", flush=True)
        sys.exit(2)

    print(f"VERIFY_OK:{new_version}", flush=True)
    if old_version is not None and old_version == new_version:
        # 设备活着但版本未变: 可能固件未真正写入(如 bootloader 不认自定义头)
        print("VERIFY_WARN_SAME_VERSION", flush=True)
    print("DONE", flush=True)

if __name__ == "__main__":
    if len(sys.argv) < 3:
        print("ERROR:Usage: ota_worker.py <COM port> <firmware.bin>", flush=True)
        sys.exit(1)

    ota_upgrade(sys.argv[1], sys.argv[2])
