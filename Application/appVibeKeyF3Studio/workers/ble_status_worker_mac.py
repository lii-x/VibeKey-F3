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
# 标准 HID 服务。VibeKey 对外是 BLE HID 键盘，macOS 就是按这个服务连上去的，
# 因此 retrieveConnectedPeripheralsWithServices 只有带上它才查得到设备。
HID_SVC = "00001812-0000-1000-8000-00805f9b34fb"
# 除 HID 外再带上标准设备信息服务：某些 macOS 版本对"自定义服务"的已连接
# 外设缓存不完整，多查一个标准服务能提高命中率（同一设备会去重）。
DIS_SVC = "0000180a-0000-1000-8000-00805f9b34fb"
HID_SVC_LIST = [HID_SVC, DIS_SVC]
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


class BtDenied(Exception):
    """macOS 蓝牙隐私权限被拒（TCC 拒绝/受限）。"""


# --- 授权状态静态查询（不触发弹窗）------------------------------------------
# CBManager.authorization() 只读 TCC 记录、不碰适配器，因此**不会弹框**：
#   0 = NotDetermined（还没问过）  1 = Restricted  2 = Denied  3 = AllowedAlways
# 与之相对，任何真正访问 CoreBluetooth 的调用在 NotDetermined 时都会**抬起
# 系统授权框** —— 而弹窗归属于发起请求的那个进程。
AUTH_ALLOWED = 3
AUTH_DENIED_VALUES = (1, 2)      # Restricted / Denied


def _bt_authorization():
    """读 TCC 授权状态；取不到返回 None（当作未知，沿用"先碰再判"的老路径）。"""
    try:
        import CoreBluetooth as CB
        return int(CB.CBManager.authorization())
    except Exception:
        return None


async def _await_authorization(budget=90.0, interval=0.5):
    """把进程留住，等用户在系统授权弹窗里做出选择。

    ★ 这是"弹窗反复出现、关掉马上又弹"的关键修复。
    弹窗是异步的：访问 CoreBluetooth 抬起授权框后，本次调用往往立刻失败返回。
    如果进程随即退出（status 原本只等 6s），弹窗会被系统撤掉 ——
    用户根本没有机会点"允许"，TCC 里也就永远留不下一笔授权。
    3 秒后下一个 status 进程再碰一次，又是新的一轮弹窗。实测 6 小时 160 次。

    留在原地轮询授权状态（轮询本身不弹窗），用户点完立即返回。
    返回 True = 已授权。
    """
    import time
    deadline = time.monotonic() + budget
    logged = False
    while time.monotonic() < deadline:
        auth = _bt_authorization()
        if auth == AUTH_ALLOWED:
            log("用户已授予蓝牙权限")
            return True
        if auth in AUTH_DENIED_VALUES:
            log(f"用户拒绝蓝牙权限 (authorization={auth})")
            return False
        if not logged:
            log("蓝牙授权未决：系统弹窗已出现，等待用户选择 ...")
            logged = True
        await asyncio.sleep(interval)
    log(f"等待蓝牙授权超时 ({budget:.0f}s)")
    return False


def _is_bt_denied(exc):
    """判断异常是否为"蓝牙权限被拒"。

    bleak 3.1 在 CoreBluetooth 权限被拒时抛 BleakBluetoothNotAvailableError，
    reason 是 DENIED_BY_USER / DENIED_BY_SYSTEM / DENIED_BY_UNKNOWN；其余
    (POWERED_OFF / NO_BLUETOOTH / UNKNOWN) 都不属于权限问题，不能混为一谈。
    同时兼容异常跨进程/版本差异：兜底看文案里的 "denied"/"not authorized"。
    """
    reason = getattr(exc, "reason", None)
    name = getattr(reason, "name", "") or ""
    if name.startswith("DENIED_BY"):
        return True
    text = str(exc).lower()
    return "denied by the user" in text or "not authorized" in text \
        or "access is denied" in text or "access is restricted" in text


async def _connected_device(allow_gatt_probe=True):
    """取系统【已连接】的 VibeKey，找不到返回 None。

    用 retrieveConnectedPeripheralsWithServices 而不是扫描：设备连上后停止广播，
    扫描看不到；而这条 API 正好回答"系统层面是不是真的连着"。

    ⚠️ **必须同时查 HID 服务和 0xFF00 服务**（2026-11-25 实测）：
    macOS 只缓存"系统自己发现过的服务"。VibeKey 是 HID 键盘，系统是按
    0x1812 连上去的，`retrieveConnectedPeripheralsWithServices(0xFF00)`
    会返回 0 个 —— 即使设备连着、按键正常、GATT 也确实可连。
    实测同一台机器：0xFF00 → 0 个；0x1812 → 2 个（含本设备）。
    只查 0xFF00 就是"设备和系统都好好的，客户端却永远看不到"的根因。

    带 6s 硬超时：蓝牙适配器被关闭或权限未授予时，CoreBluetooth 的回调不会
    到达，不兜超时会让每 3s 一次的 status 轮询堆积成僵尸进程。
    """
    from bleak import BleakAdapter

    async def _query():
        adapter = await BleakAdapter.get()
        devs = []
        seen = set()
        # HID 优先：VibeKey 就是以 HID 身份连上系统的，这条命中最可靠
        for uuids in (HID_SVC_LIST, [LED_SVC]):
            try:
                got = await adapter.get_connected_devices(uuids)
            except Exception as e:
                if _is_bt_denied(e):
                    raise
                log(f"get_connected_devices({uuids[0][4:8]}) failed: {e}")
                continue
            for d in got:
                key = getattr(d, "address", None) or id(d)
                if key not in seen:
                    seen.add(key)
                    devs.append(d)
        return devs

    try:
        devs = await asyncio.wait_for(_query(), timeout=6.0)
    except asyncio.TimeoutError:
        log("get_connected_devices hard-timeout (蓝牙关闭或未授权?)")
        return None
    except Exception as e:
        # ⚠️ 权限被拒时 bleak 是**抛异常**而不是挂起（CentralManagerDelegate.
        # wait_until_ready -> BleakBluetoothNotAvailableError，reason=
        # DENIED_BY_USER/DENIED_BY_SYSTEM）。吞掉它会让上层把"没权限"当成
        # "设备不在已连接列表"，于是一路落到 PAIRED —— UI 让用户去系统蓝牙
        # 点连接，可设备明明已经连着、按键都在用，用户完全懵。
        if _is_bt_denied(e):
            raise BtDenied(e) from e
        log(f"get_connected_devices failed: {e}")
        return None

    return await _pick_vibekey(devs, allow_gatt_probe=allow_gatt_probe)


async def _pick_vibekey(devs, allow_gatt_probe=True):
    """从已连接候选里挑出真正的 VibeKey。

    为什么不能只看名字：查 HID 服务会把用户的鼠标/别家键盘一并带出来，
    而 macOS 对**未做过服务发现的**外设经常只给空名字 —— 旧逻辑"名字为空
    就当 VibeKey"会把鼠标认成设备，进而连上去读配置、必然失败。

    判据（按成本从低到高）：
      ① 名字含 vibekey      -> 直接认定（最常见，零开销）
      ② 命中缓存            -> 用上次的校验结论（避免每 3s 轮询都连一次）
      ③ 连上去看有没有 0xFF00 服务 -> 唯一可靠的身份验证，结论写回缓存

    allow_gatt_probe=False 时只走 ①②：status 是 3s 一次的快速轮询，
    不能在里面建 GATT 连接（一次 1~3s，会让轮询互相堆积）。
    """
    if not devs:
        return None

    named = [d for d in devs if "vibekey" in (d.name or "").lower()]
    if named:
        return named[0]

    cache = _load_identity_cache()
    unknown = []
    for d in devs:
        addr = (getattr(d, "address", "") or "").upper()
        if cache.get(addr) is True:
            log(f"identity cache hit: {addr} = VibeKey")
            return d
        if addr in cache:          # 已知不是 VibeKey
            continue
        unknown.append(d)

    if not allow_gatt_probe:
        log(f"{len(unknown)} unnamed connected device(s) need identity probe "
            f"(skipped in fast status path)")
        return None

    for d in unknown[:3]:          # 最多验 3 个，别把轮询拖死
        ok = await _probe_has_led_service(d)
        if ok is None:             # 连接失败：结论未知，不能缓存成"不是"
            continue
        addr = (getattr(d, "address", "") or "").upper()
        if addr:
            cache[addr] = ok
            _save_identity_cache(cache)
        if ok:
            log(f"identity verified by GATT: {addr}")
            return d
    return None


def _identity_cache_path():
    import os
    import tempfile
    return os.path.join(tempfile.gettempdir(), "vibekey-ble-identity.json")


def _load_identity_cache():
    import json
    import os
    try:
        path = _identity_cache_path()
        if not os.path.exists(path):
            return {}
        with open(path, "r", encoding="utf-8") as f:
            data = json.load(f)
        return data if isinstance(data, dict) else {}
    except Exception:
        return {}


def _save_identity_cache(cache):
    """把"某地址是不是 VibeKey"的结论落盘。

    必要性：status 每 3s 就是一个新进程，进程内缓存等于没有；没有这份缓存，
    每次轮询都要为每个 HID 候选建一次 GATT 连接，既慢又干扰设备。
    """
    import json
    try:
        with open(_identity_cache_path(), "w", encoding="utf-8") as f:
            json.dump(cache, f)
    except Exception as e:
        log(f"identity cache write failed: {e}")


async def _probe_has_led_service(dev):
    """连上去看设备有没有 0xFF00 服务 —— VibeKey 的唯一可靠身份特征。

    只做服务发现、不读不写，连上即断，对设备无副作用。

    返回 True / False / None：
      None = 连接失败、结论未知。**绝不能当 False 缓存** —— 设备忙/被别的
             进程占用时会连不上，一旦缓存成"不是 VibeKey"，该地址会被永久
             跳过（缓存落盘，重启也不会清），用户从此再也连不上。
    """
    from bleak import BleakClient
    try:
        async with BleakClient(dev, timeout=10.0) as c:
            svcs = [s.uuid.lower() for s in c.services]
            return any(s.startswith(LED_SVC[:8]) for s in svcs)
    except Exception as e:
        log(f"identity probe failed for {getattr(dev, 'address', '?')}: {e}")
        return None


def _system_bt_state():
    """用 system_profiler 读系统蓝牙开关与配对表（**不需要蓝牙 TCC 权限**）。

    为什么必须要这条通道
    ====================
    App 的蓝牙权限没授予时，CoreBluetooth 的扫描不会报错、也不会有回调，
    只会无限期挂起（见 _scan_device 的硬超时）。此时从 App 自己的视角看，
    "没权限"和"设备不在附近"完全一样 —— UI 会错误地让用户去按设备按键，
    用户按了也没用。system_profiler 是独立系统进程，不受本 App 的 TCC
    限制，能回答两个关键问题：
      ① 蓝牙控制器是不是开着（controller_state）；
      ② 这台 Mac 到底有没有 VibeKey 的配对记录、当前连没连。

    返回 {"power": True/False/None, "connected": [名称...], "paired": [名称...],
          "raw_connected": [...], "raw_paired": [...]}。
    后两项是原始条目（含 device_address），用于比对"配对的是哪个蓝牙地址"。
    """
    import json
    import subprocess
    try:
        raw = subprocess.run(
            ["system_profiler", "SPBluetoothDataType", "-json"],
            capture_output=True, timeout=5.0).stdout
        top = json.loads(raw or b"{}").get("SPBluetoothDataType", [{}])[0]
    except Exception as e:
        log(f"system_profiler 失败: {e}")
        return {"power": None, "connected": [], "paired": [],
                "raw_connected": [], "raw_paired": []}

    state = (top.get("controller_properties") or {}).get("controller_state", "")
    power = (state == "attrib_on") if state else None

    def names(section):
        out = []
        for item in top.get(section) or []:
            if isinstance(item, dict):
                out.extend(item.keys())
        return out

    return {"power": power,
            "connected": names("device_connected"),
            "paired": names("device_not_connected"),
            "raw_connected": top.get("device_connected") or [],
            "raw_paired": top.get("device_not_connected") or []}


def _has_vibekey(names):
    return any("vibekey" in (n or "").lower() for n in names)


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

    返回 (device, strong_list, scan_status, weak_list)：
      weak_list 是"只带 HID 服务"的候选（可能是用户的鼠标/别家键盘），
      **调用方必须做 GATT 校验**才能当 VibeKey 用。配对前设备也可能是这种
      状态（此时它还连不上别人），所以 probe 会把它们也验一遍。
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
        return None, [], "timeout", []
    except Exception as e:
        log(f"scan failed: {e}")
        return None, [], "error", []

    found = []
    weak = []
    for addr, (d, adv) in seen.items():
        nm = (d.name or adv.local_name or "")
        uuids = [u.lower() for u in (getattr(adv, "service_uuids", None) or [])]
        # 强判据：名字含 vibekey，或广播里直接带 0xFF00 自定义服务
        if "vibekey" in nm.lower() or LED_SVC in uuids:
            found.append((d, nm or "(no name)", adv.rssi))
        elif HID_SVC in uuids:
            # 弱判据：只带 HID —— 用户自己的鼠标/别家键盘也长这样，
            # 不能直接认定，交给调用方做 GATT 校验（见 probe）。
            weak.append((d, nm or "(no name)", adv.rssi))

    if found:
        d, nm, rssi = found[0]
        log(f"found by scan: {d.address} {nm} rssi={rssi}")
        return d, found, "ok", weak
    log(f"scan finished, no strong VibeKey match "
        f"(total {len(seen)} devices, {len(weak)} HID-only candidates)")
    return None, [], "ok", weak


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
        NOPERM                 蓝牙权限被拒/受限，或授权框等待超时

    ⚠️ 这里刻意**不做扫描**：扫描要 8s+，塞进 3s 轮询会让探测互相堆积。
    "设备在不在附近"由独立的 probe 子命令回答（见下）。

    ⚠️ 授权状态必须先静态查一次：未授权时本函数**每 3 秒被一个新进程调用**，
    每个进程碰 CoreBluetooth 都是一次新的 TCC 询问；而 status 原本只等 6s
    就退出，进程一退弹窗被撤，用户永远点不到"允许" ⇒ 表现为弹窗连环出现。
    见 _await_authorization 的说明。
    """
    auth = _bt_authorization()
    if auth in AUTH_DENIED_VALUES:
        out("NOPERM")
        log(f"蓝牙权限已被拒绝 (authorization={auth})")
        return 0
    pending = (auth != AUTH_ALLOWED)
    if pending:
        log(f"蓝牙授权未决 (authorization={auth})")

    try:
        dev = await _connected_device()
    except BtDenied as e:
        # 权限被拒时不要报 DISCONNECTED —— 设备可能就在系统里连着（用户按键都在用）。
        # 报 NOPERM 让 UI 说清"是本机权限问题"，而不是让用户去折腾设备。
        # pending 说明弹窗刚抬起来：留住进程等用户点，否则弹窗会被撤掉。
        if pending and await _await_authorization():
            try:
                dev = await _connected_device()
            except Exception as e2:
                out("NOPERM")
                log(f"授权后重试失败: {e2}")
                return 0
        else:
            out("NOPERM")
            log(f"蓝牙权限被拒: {e}")
            return 0
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


def _vibekey_entries(section):
    """从 system_profiler 的某一节里取出 VibeKey 条目（名称 -> 属性 dict）。"""
    for item in section or []:
        if not isinstance(item, dict):
            continue
        for name, props in item.items():
            if "vibekey" in (name or "").lower():
                yield name, (props or {})


async def probe():
    """深度诊断：系统蓝牙状态 + 已连接列表 + 主动扫描，回答"卡在哪一步"。

    输出（一行，C++/QML 按前缀解析）：
        CONNECTED <battery>     正常，配置通道可用
        PAIRED_BUSY             系统显示已连接，但本进程拿不到 0xFF00 ——
                                配对记录异常（典型：旧配对残留），需重新配对
        SLOT_MISMATCH <addr>    本机存着 VibeKey 的配对记录，但**扫到的是另一个
                                蓝牙地址** —— 设备当前停在别的槽位，必须关机后
                                按住本机对应的 C 键再开机（见下面详述）
        PAIRED                  本机有配对记录，当前没连上（设备睡了 / 系统没回连）
        FOUND <name> <rssi>     设备在广播但系统没连 —— 需要配对
        BT_OFF                  这台 Mac 的蓝牙是关的
        NOPERM                  扫描被系统拦截（蓝牙权限未授予 / 适配器异常）
        DISCONNECTED            蓝牙正常，但附近没有设备在广播

    只在"已确认连不上"时才跑（C++ 侧低频触发或用户点刷新），不要进 3s 轮询。
    """
    # ① 先看系统级状态：这条不依赖蓝牙权限，是"NOPERM"与"设备不在"的唯一区分依据
    sys_bt = _system_bt_state()
    log(f"system bt: power={sys_bt['power']} "
        f"connected={sys_bt['connected']} paired={sys_bt['paired']}")
    if sys_bt["power"] is False:
        out("BT_OFF")
        return 0

    # 本机配对表里 VibeKey 的地址（每槽独立 BD_ADDR，方案B 见 bt_identity.c）
    paired_addrs = {props.get("device_address", "").upper()
                    for _, props in _vibekey_entries(sys_bt["raw_paired"])}
    paired_addrs.discard("")
    log(f"paired VibeKey addrs: {sorted(paired_addrs) or '(none)'}")

    # ② 权限静态预判：已拒绝就直接给结论，别再碰 CoreBluetooth（每次碰都可能再弹）
    auth = _bt_authorization()
    if auth in AUTH_DENIED_VALUES:
        log(f"蓝牙权限已被拒绝 (authorization={auth})")
        out("NOPERM")
        return 0
    pending = (auth != AUTH_ALLOWED)

    # ③ 系统已连接列表（配置通道真正可用的权威判据）
    try:
        dev = await _connected_device()
    except BtDenied as e:
        # 关键分支：权限被拒时系统层面"设备可能就在连着、按键都在用"，
        # 但本进程一条 GATT 都看不到。必须单独报 NOPERM，不能让用户去折腾设备。
        # pending = 弹窗刚被抬起：留住进程等用户点完，否则弹窗被系统撤掉。
        # ⚠️ probe 是用户点"检查设备"主动触发的，等待预算要短（25s），
        #    不能像后台 status 那样等 90s —— 否则界面像卡死。
        if pending and await _await_authorization(budget=25.0):
            try:
                dev = await _connected_device()
            except Exception as e2:
                log(f"授权后重试失败: {e2}")
                dev = None
        else:
            log(f"蓝牙权限被拒: {e}")
            out("NOPERM")
            return 0
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

    # ③ 系统说"连着"但 CoreBluetooth 不给 0xFF00：配对记录异常（HID 占了链路，
    #    或旧 bond 与当前槽位地址不一致）。macOS 里「忽略设备」再重新配对可解。
    if _has_vibekey(sys_bt["connected"]):
        out("PAIRED_BUSY")
        return 0

    # ④ 主动扫描：换机场景下这一步比"有没有配对记录"信息量大得多。
    #    固件每个蓝牙槽位是**不同的本机地址**（bt_identity.c: rand_base + 槽偏移），
    #    而槽位只在开机瞬间按键决定（bt_slot.c: bt_multi_boot_select）。
    #    所以"本机配过槽1、设备此刻跑在槽2"时，本机配对表里有 VibeKey、却永远连不上，
    #    而扫到的地址和配对表里的对不上 —— 这是必须让用户按 C 键的关键判据。
    dev2, found, scan_status, weak = await _scan_device()
    if dev2 is None and weak:
        # 只带 HID 服务的广播：可能是 VibeKey，也可能是用户自己的鼠标/键盘。
        # probe 是低频诊断，可以负担一次 GATT 校验来定身份。
        cand = await _pick_vibekey([w[0] for w in weak], allow_gatt_probe=True)
        if cand is not None:
            rssi = next((w[2] for w in weak if w[0] is cand), "?")
            log(f"HID-only candidate verified as VibeKey: {cand.address}")
            dev2, found = cand, [(cand, cand.name or "VibeKey", rssi)]

    if dev2 is not None:
        d, nm, rssi = found[0]
        safe = (nm or "VibeKey").replace(" ", "_")
        found_addr = (getattr(d, "address", "") or "").upper()
        log(f"scan addr={found_addr} paired={sorted(paired_addrs)}")
        if paired_addrs and found_addr and found_addr not in paired_addrs:
            # 配对过的是别的地址 => 设备当前在别的槽位
            out(f"SLOT_MISMATCH {found_addr.replace(' ', '_')}")
            return 0
        out(f"FOUND {safe} {rssi}")
        return 0

    if scan_status == "timeout":
        # 蓝牙开着、扫描却硬超时 —— 几乎只可能是 TCC 权限没给。
        # 若这里仍报 DISCONNECTED，UI 会误导用户去按键唤醒设备。
        out("NOPERM")
        return 0

    # ⑤ 扫描正常但没扫到：有配对记录 = 设备睡了或正连着别的电脑；
    #    没有配对记录 = 本机从未配对且设备不在广播范围。
    if paired_addrs:
        out("PAIRED")
        return 0

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
