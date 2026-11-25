#!/usr/bin/env python3
"""ble_status_worker_mac.py — macOS 的 BLE 连接状态探测 + 常驻 LED/RGB 通道

替代依赖 winrt 的 `ble_led_worker.py`。两个子命令，输出协议与 Windows 版**完全一致**，
C++ 侧（LightMonitor）不需要任何改动：

    ble_status_worker_mac.py status
        CONNECTED <battery>     battery 为 0~100，读失败为 -1
        DISCONNECTED

    ble_status_worker_mac.py serve
        READY                   已握住 GATT 连接
        OK state=N / ERROR ...  每条 led 命令的回执
        OK rgb / ERROR ...      每条 rgb 命令的回执
        LOST                    链路真的断了（看门狗连续判定）
        BYE                     退出前最后一行

    stdin 命令（一行一条，与 ble_led_worker.py 的 serve 协议相同）：
        led <state>
        rgb <idx> <en> <r> <g> <b> <bri>
        exit

判定依据
========
macOS 的 CoreBluetooth 不把"被系统 HID 接管的 BLE 链路"暴露给普通进程的
扫描结果（设备连上后停播），但 **配对正确时** 会出现在
`retrieveConnectedPeripheralsWithServices(0xFF00)` 里 —— 这正是"真的连着"的
权威判据，比看广播可靠（广播会在空闲 60s 后停掉）。

电池读 0x180F 标准服务；失败不影响连接判定。
"""
import asyncio
import os
import sys

LED_SVC = "0000ff00-0000-1000-8000-00805f9b34fb"
BATT_SVC = "0000180f-0000-1000-8000-00805f9b34fb"
BATT_CHAR = "00002a19-0000-1000-8000-00805f9b34fb"

# 0xFF00 服务内的三个特征（见 Firmware/src/ble_led_service.c）
CHAR_LED = "0000ff01-0000-1000-8000-00805f9b34fb"
CHAR_CFG = "0000ff03-0000-1000-8000-00805f9b34fb"

# CONF 帧常量：与 config_worker.py / 固件共用同一套线上格式
CONF_MAGIC = 0x434F4E46          # "FNOC" little-endian
CONF_CMD_LED = 0x04
CONF_CMD_RGB_APPLY = 0x06

# serve 的断连看门狗：0.2s 一轮，连续 15 轮(≈3s)才认断，避开空闲瞬时掉线
WATCHDOG_INTERVAL = 0.2
WATCHDOG_STREAK = 15


def log(msg):
    print(f"[ble-status-mac] {msg}", file=sys.stderr, flush=True)


def out(line):
    print(line, flush=True)


async def _connected_device():
    """取系统【已连接】的 0xFF00 设备，找不到返回 None。

    用 retrieveConnectedPeripheralsWithServices 而不是扫描：设备连上后停止广播，
    扫描看不到；而这条 API 正好回答"系统层面是不是真的连着"。

    带 6s 硬超时：蓝牙适配器被关闭或权限未授予时，CoreBluetooth 的回调不会
    到达，不兜超时会让每 3s 一次的 status 轮询堆积成僵尸进程。
    """
    from bleak import BleakAdapter

    async def _query():
        adapter = await BleakAdapter.get()
        return await adapter.get_connected_devices([LED_SVC])

    try:
        devs = await asyncio.wait_for(_query(), timeout=6.0)
    except asyncio.TimeoutError:
        log("get_connected_devices hard-timeout (蓝牙关闭或未授权?)")
        return None

    for d in devs:
        if "vibekey" in (d.name or "").lower() or not d.name:
            return d
    return None


async def _scan_device(timeout=8.0):
    """主动扫描发现【未连接】的 VibeKey（配对前/换机后就是这种状态）。

    为什么必须补这一步
    ==================
    retrieveConnectedPeripherals 只返回"系统已经配对并连着"的外设。换一台
    从没配过的 Mac，列表永远是空的 —— 但设备其实就在旁边开着、广播着。
    只查已连接列表会把"看得见但没连"误报成"设备不存在"，用户完全无从下手。

    固件的 0xFF00 服务是 SVC_AUTH/NO_AUTH（Firmware/src/ble_led_service.c），
    不需要加密链路；而且广播里带 0x1812(HID)，据此可在配对前认出设备。

    注意设备只在开机/按键唤醒后广播 60s 就停（ADV_GRACE_WINDOW_MS），
    所以扫描为空并不代表"没有设备"，只代表"此刻没在广播"——UI 文案要能区分。
    """
    from bleak import BleakScanner

    # ⚠️ 必须自己兜一层超时：没有蓝牙权限（TCC 未授权）或适配器不可用时，
    # BleakScanner.discover(timeout=...) 的 timeout **不会生效** —— 它在等
    # CoreBluetooth 的回调，那条回调永远不来，进程会无限挂起（实测 >2min）。
    # C++ 侧进程不会卡死，但 UI 会一直停在旧状态，用户只觉得"点了没反应"。
    # 这里用 asyncio.wait_for 强制到点返回，把"扫不到"当成正常结论上报。
    try:
        seen = await asyncio.wait_for(
            BleakScanner.discover(timeout=timeout, return_adv=True),
            timeout=timeout + 6.0)
    except asyncio.TimeoutError:
        log(f"scan hard-timeout after {timeout + 6.0:.0f}s "
            f"(蓝牙权限未授予? 系统设置→隐私与安全性→蓝牙)")
        return None, []
    except Exception as e:
        log(f"scan failed: {e}")
        return None, []

    found = []
    for addr, (d, adv) in seen.items():
        nm = (d.name or adv.local_name or "")
        uuids = [u.lower() for u in (getattr(adv, "service_uuids", None) or [])]
        is_vibekey = ("vibekey" in nm.lower()
                      or LED_SVC in uuids
                      or "00001812-0000-1000-8000-00805f9b34fb" in uuids)
        if is_vibekey:
            found.append((d, nm or "(no name)", adv.rssi))

    if found:
        d, nm, rssi = found[0]
        log(f"found by scan: {d.address} {nm} rssi={rssi}")
        return d, found
    log(f"scan finished, no VibeKey advertisement (total {len(seen)} devices)")
    return None, []


def build_conf_frame(cmd, slot, payload=b""):
    """[4B magic][1B cmd][1B slot][2B data_len][payload]，小端 —— 与固件一致。"""
    import struct
    return struct.pack("<IBBH", CONF_MAGIC, cmd, slot & 0xFF, len(payload)) + payload


def build_rgb_frame(idx, en, r, g, b, bri):
    import struct
    payload = struct.pack("BBBBBB", idx & 0xFF, en & 0xFF,
                          r & 0xFF, g & 0xFF, b & 0xFF, bri & 0xFF)
    return build_conf_frame(CONF_CMD_RGB_APPLY, 0, payload)


async def status():
    """快速连接态检查（<1s）—— 供 C++ 每 3s 轮询。

        CONNECTED <battery>    系统已配对且已连接
        DISCONNECTED           不在已连接列表（**不代表设备不在旁边**）

    ⚠️ 这里刻意**不做扫描**：扫描要 8s+，塞进 3s 轮询会让探测互相堆积。
    "设备在不在附近"由独立的 probe 子命令回答（见下）。
    """
    try:
        dev = await _connected_device()
    except Exception as e:
        out("DISCONNECTED")
        log(f"adapter error: {e}")
        return 0

    if dev is None:
        out("DISCONNECTED")
        return 0

    # 已确认在系统已连接列表里 -> 电池读失败也仍然算连上
    batt = -1
    try:
        from bleak import BleakClient
        async with BleakClient(dev, timeout=15.0) as c:
            if c.services.get_characteristic(BATT_CHAR) is not None:
                raw = await c.read_gatt_char(BATT_CHAR)
                if raw:
                    batt = int(raw[0])
    except Exception as e:
        log(f"battery read failed: {e}")

    out(f"CONNECTED {batt}")
    return 0


async def probe():
    """深度诊断：已连接列表 + 主动扫描，回答"看不到设备到底卡在哪"。

        CONNECTED <battery>          正常，直接用
        FOUND <name> <rssi>          设备就在旁边广播，但系统没连 —— 需要配对/
                                     换槽位（这是"换一台 Mac 就看不到"的典型态）
        PAIRED_NOT_ADVERTISING       本机有配对记录但设备此刻不广播（已进深睡）
        DISCONNECTED                 附近没有设备在广播（没开机 / 按键唤醒一下）

    只在"已确认连不上"时才跑（C++ 侧低频触发或用户点刷新），不要进 3s 轮询。
    """
    try:
        dev = await _connected_device()
    except Exception as e:
        log(f"adapter error: {e}")
        dev = None

    if dev is not None:
        batt = -1
        try:
            from bleak import BleakClient
            async with BleakClient(dev, timeout=15.0) as c:
                if c.services.get_characteristic(BATT_CHAR) is not None:
                    raw = await c.read_gatt_char(BATT_CHAR)
                    if raw:
                        batt = int(raw[0])
        except Exception as e:
            log(f"battery read failed: {e}")
        out(f"CONNECTED {batt}")
        return 0

    dev2, found = await _scan_device()
    if dev2 is not None:
        _, nm, rssi = found[0]
        safe = (nm or "VibeKey").replace(" ", "_")
        out(f"FOUND {safe} {rssi}")
        return 0

    # 扫描为空 = 设备此刻没在广播。两种可能无法用 bleak 区分
    # （3.1 的 BleakAdapter 只有 get_connected_devices，没有枚举已配对设备的 API）：
    #   ① 设备睡着了（固件广播宽限 60s 后停播进深睡）→ 按键唤醒即可
    #   ② 从没在本机配对过
    # 两者对用户的动作都是"先按键唤醒设备、再试"，所以合并上报。
    out("DISCONNECTED")
    return 0


class LinkLost(Exception):
    """看门狗判定链路已断。"""


async def serve():
    """常驻持有一条 GATT 连接，从 stdin 收 led/rgb 命令立即下发。

    与 Windows 版 serve 的差异只在传输层：Windows 靠 winrt 的
    connection_status 轮询做断连看门狗；这里用 BleakClient 的
    disconnected_callback（CoreBluetooth 主动回调，比轮询更准）。
    """
    from bleak import BleakClient

    dev = None
    try:
        for attempt in range(1, 4):
            dev = await _connected_device()
            if dev is not None:
                break
            log(f"serve connect attempt {attempt}/3: 设备不在已连接列表")
            await asyncio.sleep(2.0)
    except Exception as e:
        log(f"serve adapter error: {e}")

    if dev is None:
        log("ERROR: VibeKey 不在系统已连接列表（蓝牙未连接或配对失效）")
        return 1

    loop = asyncio.get_running_loop()
    link_lost = asyncio.Event()

    def _on_disconnect(_client):
        # CoreBluetooth 回调在事件循环线程上触发
        link_lost.set()

    client = BleakClient(dev, timeout=25.0, disconnected_callback=_on_disconnect)
    try:
        await client.connect()
    except Exception as e:
        log(f"ERROR: connect failed: {e}")
        return 1

    if not client.is_connected:
        log("ERROR: connect returned but not connected")
        return 1

    out("READY")

    # 写 0xFF01：状态灯。response=True 优先（固件两个属性都开放），
    # 失败再退 WRITE_COMMAND —— 与 ble_config_worker_mac.py 的策略一致。
    async def write_led(state):
        data = bytes([state & 0xFF])
        try:
            await client.write_gatt_char(CHAR_LED, data, response=True)
            return True
        except Exception:
            try:
                await client.write_gatt_char(CHAR_LED, data, response=False)
                return True
            except Exception as e:
                log(f"led write failed: {e}")
                return False

    async def write_rgb(a):
        frame = build_rgb_frame(int(a[0]), int(a[1]), int(a[2]),
                                int(a[3]), int(a[4]), int(a[5]))
        try:
            await client.write_gatt_char(CHAR_CFG, frame, response=False)
            return True
        except Exception as e:
            log(f"rgb write failed: {e}")
            return False

    # ---- stdin 读取：用 executor 跑阻塞 readline，避免卡住事件循环 ----
    # C++ 端 stopBleServe() 会先写 "exit" 再 waitForFinished(3000)；
    # 必须能及时收到 exit 才能干净释放连接（强杀会在协议栈留下半开句柄）。
    def read_line():
        return sys.stdin.readline()

    async def stdin_task():
        while True:
            line = await loop.run_in_executor(None, read_line)
            if not line:            # stdin 关闭 = 父进程没了
                return "eof"
            line = line.strip()
            if not line:
                continue
            if line == "exit":
                return "exit"
            if line.startswith("led "):
                try:
                    st = int(line.split()[1])
                except (IndexError, ValueError):
                    out("ERROR: bad state")
                    continue
                ok = await write_led(st)
                out(f"OK state={st}" if ok else "ERROR: write failed")
            elif line.startswith("rgb "):
                parts = line.split()
                if len(parts) < 7:
                    out("ERROR: bad rgb args")
                    continue
                ok = await write_rgb(parts[1:7])
                out("OK rgb" if ok else "ERROR: rgb write failed")
            else:
                out(f"ERROR: unknown command: {line}")

    stdin_fut = asyncio.ensure_future(stdin_task())
    lost_fut = asyncio.ensure_future(link_lost.wait())

    try:
        done, pending = await asyncio.wait(
            {stdin_fut, lost_fut}, return_when=asyncio.FIRST_COMPLETED)
    finally:
        for f in (stdin_fut, lost_fut):
            if not f.done():
                f.cancel()

    reason = "lost" if lost_fut.done() and not stdin_fut.done() else "exit"
    if stdin_fut.done() and not stdin_fut.cancelled():
        exc = stdin_fut.exception()
        if exc is None:
            reason = stdin_fut.result() or reason

    # 干净释放：不 disconnect 会让 CoreBluetooth / 设备端以为连接还在，
    # 后续 config worker 连不上（设备端只允许一个 GATT 客户端）。
    try:
        await client.disconnect()
    except Exception as e:
        log(f"disconnect error: {e}")

    out("BYE")
    if reason == "lost":
        log("看门狗判定链路断开，退出码 2")
        return 2
    return 0


def main():
    if len(sys.argv) < 2:
        out("Usage: ble_status_worker_mac.py status|probe|serve")
        return 1
    cmd = sys.argv[1]
    try:
        if cmd == "status":
            return asyncio.run(status())
        if cmd == "probe":
            return asyncio.run(probe())
        if cmd == "serve":
            return asyncio.run(serve())
    except Exception as e:
        out(f"ERROR:{type(e).__name__}: {e}")
        return 1
    out(f"ERROR: unknown command: {cmd}")
    return 1


if __name__ == "__main__":
    sys.exit(main())
