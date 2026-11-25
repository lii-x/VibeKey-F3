#!/usr/bin/env python3
"""ble_config_worker_mac.py — macOS 的 BLE 配置 worker（替代依赖 winrt 的 ble_config_worker.py）

背景
====
`ble_config_worker.py` 基于 Windows UWP 的 `winrt.windows.devices.bluetooth`，
在 macOS 上不可用。但 macOS 的 CoreBluetooth 可以访问设备的 0xFF00 服务
（实测：配对正确后 `retrieveConnectedPeripheralsWithServices(0xFF00)` 能拿到设备，
GATT 三特征齐全，连接耗时约 0.1s）：

    0xFF01 (write)   LED 状态灯
    0xFF03 (write)   配置命令 —— 与 USB CDC 共用同一套 CONF 帧格式
    0xFF04 (notify)  命令回包

因此本 worker 只替换传输层：帧的组装/解析完全复用 `config_worker.py`
（`build_conf_frame` / `build_write_payload` / `parse_read_response`），
保证 BLE 与 USB 两条通道语义严格一致 —— 不要在这里另写一套编解码。

用法（与 ble_config_worker.py 的命令行保持一致，C++ 侧无需区分）
    ble_config_worker_mac.py read [slot]        slot: 0~2 或 0xFF(当前激活槽)
    ble_config_worker_mac.py write <json> [slot]
    ble_config_worker_mac.py reset [slot]
    ble_config_worker_mac.py led <state>
    ble_config_worker_mac.py rgb <k> <on> <r> <g> <b> <bri>
    ble_config_worker_mac.py info

依赖: pip install bleak   (macOS 上走 CoreBluetooth，无需额外权限)
"""
import asyncio
import json
import os
import sys

# 复用传输无关的编解码/常量（同目录）。
# C++ 侧把工作目录设为 Contents/MacOS/（worker 与 config_worker.py 同处），
# 正常情况 sys.path[0] 就是脚本目录；这里显式补一次，避免从别处调用时失败。
_HERE = os.path.dirname(os.path.abspath(__file__))
if _HERE not in sys.path:
    sys.path.insert(0, _HERE)
import config_worker as cw

LED_SVC = "0000ff00-0000-1000-8000-00805f9b34fb"
# 标准 HID / 设备信息服务（2026-11-25 实测必要）：
# macOS 的 retrieveConnectedPeripheralsWithServices 只认系统自己发现过的服务。
# VibeKey 是以 BLE HID(0x1812) 身份连上 Mac 的，用 0xFF00 去查会返回 0 个 ——
# 即使设备连着、按键正常。同一台机器实测：0xFF00 → 0 个，0x1812 → 2 个。
# 必须带上 HID 才找得到已连接设备，否则只会在"扫描"里空转到超时。
HID_SVC = "00001812-0000-1000-8000-00805f9b34fb"
DIS_SVC = "0000180a-0000-1000-8000-00805f9b34fb"
CHAR_LED = "0000ff01-0000-1000-8000-00805f9b34fb"
CHAR_CFG = "0000ff03-0000-1000-8000-00805f9b34fb"
CHAR_RSP = "0000ff04-0000-1000-8000-00805f9b34fb"

# 回包超时(秒)。BLE 比 USB 慢一个量级：READ 回包 324B 需要多次 notify 拼包，
# 实测 1~2s 完成；给足 6s 兜底弱信号场景。
REPLY_TIMEOUT = 6.0
# INFO 回包是 ASCII 一行 "VibeKey-F3|<ver>|<bat>|<slot>\n"
INFO_TIMEOUT = 4.0


def log(msg):
    print(f"[ble-mac] {msg}", file=sys.stderr, flush=True)


async def _find_device():
    """优先取系统【已连接】的设备（免扫描、最快），
    退化为主动扫描（设备可能因空闲停播，故扫描时间给足）。

    ⚠️ 已连接列表里不要只认名字含 "vibekey"：macOS 有时先给出空名字，
    设备连上后又停止广播，一旦被名字过滤掉就再也找不到。

    ⚠️ **必须查 HID 服务，不能只查 0xFF00**：系统只缓存自己发现过的服务。
    VibeKey 作为 BLE HID 键盘被系统连上，0xFF00（自定义服务）不会进
    已连接缓存 —— 实测同一台 Mac：0xFF00 → 0 个设备，0x1812 → 2 个。
    只查 0xFF00 就会直接掉到扫描分支，而设备连着时根本不广播 ⇒ 永远失败。
    这正是"设备连接正常、按键能用，客户端却读不到配置"的根因。"""
    from bleak import BleakAdapter, BleakScanner

    try:
        adapter = await BleakAdapter.get()
        # HID/设备信息服务优先（命中最可靠），再补一次 0xFF00（旧配对可能只缓存它）
        candidates = []
        seen_addrs = set()
        for uuids in ([HID_SVC, DIS_SVC], [LED_SVC]):
            try:
                devs = await adapter.get_connected_devices(uuids)
            except Exception as e:
                log(f"get_connected_devices({uuids[0][4:8]}) failed: {e}")
                continue
            for d in devs:
                key = getattr(d, "address", None) or id(d)
                if key in seen_addrs:
                    continue
                seen_addrs.add(key)
                log(f"connected candidate: {d.address} {d.name or '(no name)'} "
                    f"via={uuids[0][4:8]}")
                candidates.append(d)

        # 名字命中直接认；否则必须验证 —— 查 HID 会把用户的鼠标/别家键盘
        # 一起带出来，而 macOS 对未做过服务发现的外设常只给空名字。认错设备
        # 的代价很大：会对着鼠标去读配置，然后把错误计入"设备无响应"。
        for d in candidates:
            if "vibekey" in (d.name or "").lower():
                log(f"found connected device by name: {d.address} {d.name}")
                return d
        for d in candidates:
            if await _has_led_service(d):
                log(f"found connected device by GATT: {d.address} "
                    f"{d.name or '(no name)'}")
                return d
    except Exception as e:
        log(f"get_connected_devices failed: {e}")

    log("not in connected list, scanning ...")
    # ⚠️ 与 ble_status_worker_mac.py 同一坑：蓝牙权限未授予时
    # BleakScanner.discover 的 timeout 不生效，会无限挂起（实测 >2min）。
    # C++ 侧只等有限时间，弹一次"读取失败"总比进程永远挂着好。
    try:
        seen = await asyncio.wait_for(
            BleakScanner.discover(timeout=12.0, return_adv=True), timeout=18.0)
    except asyncio.TimeoutError:
        log("scan hard-timeout (蓝牙权限未授予? 系统设置→隐私与安全性→蓝牙)")
        return None
    except Exception as e:
        log(f"scan failed: {e}")
        return None

    strong = []
    weak = []
    for addr, (d, adv) in seen.items():
        nm = d.name or adv.local_name or ""
        uuids = [u.lower() for u in (getattr(adv, "service_uuids", None) or [])]
        if "vibekey" in nm.lower() or LED_SVC in uuids:
            strong.append(d)
        elif HID_SVC in uuids:
            # 只带 HID 的很大概率是用户自己的外设，必须验证后再用
            weak.append(d)

    if strong:
        log(f"found by scan: {strong[0].address}")
        return strong[0]
    for d in weak:
        if await _has_led_service(d):
            log(f"found by scan+GATT: {d.address}")
            return d
    return None


async def _has_led_service(dev):
    """连上去确认有没有 0xFF00 服务 —— VibeKey 的可靠身份特征。

    只做服务发现，不读不写。连不上时返回 False（这里不需要区分
    "未知"与"不是"，因为调用方还有扫描兜底路径）。
    """
    from bleak import BleakClient
    try:
        async with BleakClient(dev, timeout=10.0) as c:
            return any(s.uuid.lower().startswith(LED_SVC[:8]) for s in c.services)
    except Exception as e:
        log(f"identity check failed for {getattr(dev, 'address', '?')}: {e}")
        return False


class Session:
    """一次 BLE 会话: 连接 -> (可选)发命令等回包 -> 断开。

    设计成"每次命令一个新连接"与 USB 的 worker 语义一致 ——
    C++ 侧本来就是一次性子进程，不值得在进程间维持常连接。
    """

    def __init__(self):
        self.client = None
        self.buf = bytearray()
        self.loop = None

    async def __aenter__(self):
        from bleak import BleakClient
        dev = await _find_device()
        if dev is None:
            raise RuntimeError("VibeKey BLE device not found (已配对且已连接?)")
        self.client = BleakClient(dev, timeout=25.0)
        await self.client.connect()
        log(f"connected: {self.client.is_connected}")
        await self.client.start_notify(CHAR_RSP, self._on_notify)
        return self

    async def __aexit__(self, *exc):
        try:
            await self.client.stop_notify(CHAR_RSP)
        except Exception:
            pass
        try:
            await self.client.disconnect()
        except Exception:
            pass

    def _on_notify(self, _ch, data: bytearray):
        self.buf.extend(data)

    async def send(self, cmd, slot=0, payload=b"", expect_len=1,
                   timeout=REPLY_TIMEOUT, as_text=False):
        """发一条 CONF 帧并等待回包 >= expect_len 字节。"""
        self.buf.clear()
        frame = cw.build_conf_frame(cmd, slot, payload)
        log(f"-> cmd=0x{cmd:02x} slot={slot} frame={len(frame)}B")
        await self.client.write_gatt_char(CHAR_CFG, frame, response=False)

        step = 0.05
        waited = 0.0
        while waited < timeout:
            if len(self.buf) >= expect_len:
                # READ 的回包会在后续 notify 里继续补齐，多等一小会儿
                if cmd == cw.CONF_CMD_READ and len(self.buf) < cw.CFG_READ_LEN_NEW:
                    await asyncio.sleep(0.12)
                break
            await asyncio.sleep(step)
            waited += step
        data = bytes(self.buf)
        log(f"<- {len(data)}B (waited {waited:.2f}s)")
        return data.decode("utf-8", errors="replace") if as_text else data


async def _read_info_text(s):
    """在已有会话里发 INFO，返回原始文本（用于解析激活槽/版本）。"""
    return await s.send(cw.CONF_CMD_INFO, 0, expect_len=8,
                        timeout=INFO_TIMEOUT, as_text=True)


def _print_info_version(info_text):
    """把 INFO 里的固件版本透传给 C++（KeyConfigManager 解析 `VERSION <ver>`）。

    Windows 版 ble_config_worker.py 早就在 read/write/reset 前打印这一行；
    macOS 版最初只输出了 SLOT/JSON，导致 BLE-only 时 LightMonitor 的
    bleDeviceVersion 永远为空 —— 界面看不到版本，OtaManager 也无法判断
    "设备当前固件 vs 在线最新固件"，固件更新提醒因此失效。
    """
    parsed = cw.parse_info_response(info_text) if info_text else None
    if parsed and parsed.get("version"):
        print(f"VERSION {parsed['version']}", flush=True)


def _resolve_slot(slot_arg, info_text):
    """槽语义（与 Windows 版 ble_config_worker.py 完全一致）：
      slot 0~2            -> 直接操作该槽（手动点槽 tab / 读取按钮）
      slot 缺失或 0xFF    -> 先 INFO 取"当前激活槽"，再输出 `SLOT n` 供 C++ 同步 UI

    ⚠️ 必须转换：INFO 回包激活槽是 1~3(1-based)，而帧头 slot 是 0~2(0-based)。
    直接透传会错位（激活槽1 → 读槽2），这是 Windows 版 09-05 已修过的坑。
    """
    try:
        slot_val = int(slot_arg) if slot_arg is not None else 0xFF
    except (TypeError, ValueError):
        slot_val = 0xFF

    if 0 <= slot_val <= 2:
        return slot_val, None   # 不需要输出 SLOT

    parsed = cw.parse_info_response(info_text) if info_text else None
    if parsed and 1 <= (parsed.get("slot") or 0) <= 3:
        return parsed["slot"] - 1, parsed["slot"]
    log("WARN: INFO slot parse failed, fallback slot=1")
    return 0, 1


async def cmd_read(slot_arg):
    async with Session() as s:
        info_text = await _read_info_text(s)
        _print_info_version(info_text)
        slot, noted = _resolve_slot(slot_arg, info_text)
        if noted is not None:
            print(f"SLOT {noted}", flush=True)
        resp = await s.send(cw.CONF_CMD_READ, slot, expect_len=cw.CFG_READ_LEN_OLD)
    if len(resp) < cw.CFG_READ_LEN_OLD:
        print(f"ERROR:设备无响应 (收到 {len(resp)}B)", flush=True)
        return 1
    try:
        cfg = cw.parse_read_response(resp)
    except ValueError as e:
        print(f"ERROR:{e}", flush=True)
        return 1
    log(f"parsed, resp={len(resp)}B")
    print(json.dumps(cfg))
    print("DONE", flush=True)
    return 0


async def cmd_write(config_path, slot_arg):
    with open(config_path, "r") as f:
        config = json.load(f)
    try:
        payload = cw.build_write_payload(config)
    except ValueError as e:
        print(f"ERROR:{e}", flush=True)
        return 1
    async with Session() as s:
        info_text = await _read_info_text(s)
        _print_info_version(info_text)
        slot, noted = _resolve_slot(slot_arg, info_text)
        if noted is not None:
            print(f"SLOT {noted}", flush=True)
        resp = await s.send(cw.CONF_CMD_WRITE, slot, payload, expect_len=1)
    if resp[:1] == b"\x00":
        print("DONE", flush=True)
        return 0
    print(f"ERROR:Device rejected config (resp={resp.hex()})", flush=True)
    return 1


async def cmd_reset(slot_arg):
    async with Session() as s:
        info_text = await _read_info_text(s)
        _print_info_version(info_text)
        slot, noted = _resolve_slot(slot_arg, info_text)
        if noted is not None:
            print(f"SLOT {noted}", flush=True)
        resp = await s.send(cw.CONF_CMD_RESET, slot, expect_len=1)
    if resp[:1] == b"\x00":
        print("DONE", flush=True)
        return 0
    print(f"ERROR:Device rejected reset (resp={resp.hex()})", flush=True)
    return 1


async def cmd_led(state):
    async with Session() as s:
        resp = await s.send(cw.CONF_CMD_LED, 0, bytes([state & 0xFF]), expect_len=1)
    if resp[:1] == b"\x00":
        print("DONE", flush=True)
        return 0
    print(f"ERROR:Device rejected LED (resp={resp.hex()})", flush=True)
    return 1


async def cmd_rgb(k, on, r, g, b, bri):
    import struct
    payload = struct.pack("BBBBBB", k & 0xFF, on & 0xFF, r & 0xFF,
                          g & 0xFF, b & 0xFF, bri & 0xFF)
    async with Session() as s:
        resp = await s.send(cw.CONF_CMD_RGB_APPLY, 0, payload, expect_len=2)
    if resp[:1] == b"\x00":
        eff = resp[1] if len(resp) >= 2 else -1
        print(f"DONE eff_bri={eff}", flush=True)
        return 0
    print(f"ERROR:Device rejected RGB (resp={resp.hex()})", flush=True)
    return 1


async def cmd_info():
    async with Session() as s:
        text = await s.send(cw.CONF_CMD_INFO, 0, expect_len=8,
                            timeout=INFO_TIMEOUT, as_text=True)
    if text:
        print(f"INFO:{text.strip()}", flush=True)
        return 0
    print("ERROR:no info reply", flush=True)
    return 1


async def main():
    if len(sys.argv) < 2:
        print("Usage: ble_config_worker_mac.py read|write|reset|led|rgb|info ...",
              flush=True)
        return 1
    cmd = sys.argv[1]
    try:
        # slot 原样透传字符串：0~2 直接操作该槽，0xFF/缺失走"当前激活槽"分支，
        # 解析统一在 _resolve_slot 里做（避免这里 int() 直接炸）。
        if cmd == "read":
            slot_arg = sys.argv[2] if len(sys.argv) >= 3 else None
            return await cmd_read(slot_arg)
        if cmd == "write":
            if len(sys.argv) < 3:
                print("ERROR:Usage: ... write <config.json> [slot]", flush=True)
                return 1
            slot_arg = sys.argv[3] if len(sys.argv) >= 4 else None
            return await cmd_write(sys.argv[2], slot_arg)
        if cmd == "reset":
            slot_arg = sys.argv[2] if len(sys.argv) >= 3 else None
            return await cmd_reset(slot_arg)
        if cmd == "led":
            return await cmd_led(int(sys.argv[2]))
        if cmd == "rgb":
            return await cmd_rgb(*[int(x) for x in sys.argv[2:8]])
        if cmd == "info":
            return await cmd_info()
    except Exception as e:
        print(f"ERROR:{type(e).__name__}: {e}", flush=True)
        return 1
    print(f"ERROR:Unknown command: {cmd}", flush=True)
    return 1


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
