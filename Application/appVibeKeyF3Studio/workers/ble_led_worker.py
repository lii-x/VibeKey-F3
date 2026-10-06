import asyncio
import sys
import json
import subprocess
import time
from pathlib import Path

# ⚠️ 08-27: Windows QProcess 管道下默认 GBK 输出, Qt fromUtf8 乱码; 强制 UTF-8。
try:
    sys.stdout.reconfigure(encoding='utf-8')
    # 10-04: stderr 也要设! C++ 侧用 QString::fromUtf8() 读 stderr，
    # 不设则中文日志输出成 "??" 完全不可读(实测: BLE 服务 denied 的原因说明全变问号)
    sys.stderr.reconfigure(encoding='utf-8')
except AttributeError:
    pass

# 16-bit vendor UUID（SIG base: 0000FF00-0000-1000-8000-00805F9B34FB）
# 固件侧已改为 struct attm_desc + sibles_register_svc(0xFF00)，空口缩短为 2 字节。
# winrt 的 GattDeviceService.Uuid 对 16-bit 服务返回完整 128-bit SIG-base 展开串。
LED_SVC_UUID_16 = "0000ff00-0000-1000-8000-00805f9b34fb"
LED_CHAR_UUID_16 = "0000ff01-0000-1000-8000-00805f9b34fb"
# 10-04: 配置命令特征(0xFF03) —— 与 LED 特征(0xFF01) 在**同一个 0xFF00 服务**里。
# 初次连接时一次性把两个可写特征都取到，之后 RGB 预览直接复用这条连接，
# 不再为每次调色启一个 python 进程 + 重新做 GATT 服务/特征发现（那才是延迟主因）。
CFG_CHAR_UUID_16 = "0000ff03-0000-1000-8000-00805f9b34fb"
# 09-01 (保留, IMU 诊断工具): 固件 0xFF02 特征, 写任意字节 -> notify 一帧 16B 快照
DIAG_CHAR_UUID_16 = "0000ff02-0000-1000-8000-00805f9b34fb"
# 标准 BLE Battery Service / Battery Level characteristic
BATT_SVC_UUID = "0000180f-0000-1000-8000-00805f9b34fb"
BATT_CHAR_UUID = "00002a19-0000-1000-8000-00805f9b34fb"
STATE_MAP = {"idle": 0, "busy": 1, "waiting": 2, "done": 3, "error": 4}
STATE_FILE = Path(__file__).parent / "state.json"

# 当前握住的 BluetoothLEDevice: serve() 用它做【断连检测】。
# 读已持有对象的 connection_status 不会新开链路(不像 from_bluetooth_address_async),
# 因此可安全高频查询, 不会造成射频抖动 —— 见 serve() 里的断连看门狗(08-29)。
g_ble_dev = None


# ---- 09-05: winrt 3.x 兼容服务/特征发现 ----
# winrt-runtime 3.2.1 起【不再】把带 BluetoothCacheMode 参数的重载投影到原方法名
# (get_gatt_services_async(mode) / get_characteristics_async(mode) 一调用就抛
# "TypeError: Invalid parameter count"), 改投影为独立名 *_with_cache_mode_async;
# 旧 winsdk/2.x 只有原方法名。下方 helper 用 getattr 探测分派, 两个包版本都可用。
# ⚠️ 所有需要 UNCACHED 刷新的调用必须走这里 —— UNCACHED 是"OS GATT 缓存被污染成
# 服务在/特征空"时的唯一用户态自愈手段, 此前它在该机器上静默全灭, 表现为
# "service has no characteristics" 永远失败。
async def gatt_services(dev, uncached=False):
    """服务发现(兼容 winrt 2.x/3.x)。uncached=True 强制刷新 OS GATT 缓存。"""
    if uncached:
        fn = getattr(dev, "get_gatt_services_with_cache_mode_async", None)
        if fn is not None:
            from winrt.windows.devices.bluetooth import BluetoothCacheMode
            return await fn(BluetoothCacheMode.UNCACHED)
        return await dev.get_gatt_services_async(BluetoothCacheMode.UNCACHED)
    return await dev.get_gatt_services_async()


async def gatt_characteristics(svc, uncached=False):
    """特征发现(兼容 winrt 2.x/3.x), 语义同 gatt_services。"""
    if uncached:
        fn = getattr(svc, "get_characteristics_with_cache_mode_async", None)
        if fn is not None:
            from winrt.windows.devices.bluetooth import BluetoothCacheMode
            return await fn(BluetoothCacheMode.UNCACHED)
        from winrt.windows.devices.bluetooth import BluetoothCacheMode
        return await svc.get_characteristics_async(BluetoothCacheMode.UNCACHED)
    return await svc.get_characteristics_async()


def find_vibekey_addresses():
    """Return ALL VibeKey BLE MAC addresses (int) found in Windows PnP devices.

    ⚠️ 08-28 多同名设备修复: 设备【每蓝牙槽位独立 BD_ADDR】(方案B), 电脑上可能
    同时存在多个同名 "VibeKey" 条目(槽1/2/3 各一条, 外加历史配对残留)。
    旧实现 return 第一个解析到的地址 —— 很可能挑到【当前并未连接】的那条,
    于是 GATT 发现拿到陈旧缓存(服务在、特征为空)且写入报 E_INVALIDARG,
    正是"电脑有多个同名设备时 LED 失效"的根因。这里改为返回全部候选地址,
    由 resolve_vibekey_address() 挑真正在线的那个。

    该查找通过 powershell 子进程(Get-PnpDevice)完成，冷启动有开销，在较慢的
    机器上可能偶发超时——而一次返回空就会被上层判为 DISCONNECTED，正是
    “换电脑后蓝牙显示断开但实际连着、且概率性反复横跳”的主要成因之一。
    这里加重试兜底：单次慢/空响应不再直接翻状态。
    """
    ps_cmd = ('Get-PnpDevice -FriendlyName "*VibeKey*" -ErrorAction SilentlyContinue'
              ' | Select-Object -ExpandProperty InstanceId')
    addrs = []
    last_err = None
    for attempt in range(3):
        try:
            result = subprocess.run(
                ['powershell', '-NoProfile', '-Command', ps_cmd],
                capture_output=True, text=True, timeout=10
            )
        except subprocess.TimeoutExpired as e:
            last_err = e
            print("WARN: find_vibekey_addresses powershell timed out (attempt %d)"
                  % (attempt + 1), file=sys.stderr)
            continue
        except Exception as e:
            last_err = e
            continue
        for line in result.stdout.strip().split('\n'):
            line = line.strip()
            if 'BTHLE\\DEV_' in line:
                # BTHLE\DEV_CDAB78563412\...
                try:
                    hex_str = line.split('DEV_')[1].split('\\')[0]
                    addr = int(hex_str, 16)
                    if addr not in addrs:
                        addrs.append(addr)
                except (IndexError, ValueError):
                    continue
        if addrs:
            return addrs
        # powershell 正常返回但本尝试没解析到 BTHLE 设备行，稍后重试
        if attempt < 2:
            time.sleep(0.2)
    if last_err is not None:
        print("WARN: find_vibekey_addresses failed after retries: %s" % last_err,
              file=sys.stderr)
    return addrs


# 09-03 提速: 进程间地址缓存文件(serve/status/config 三个进程共用)。
# powershell Get-PnpDevice 冷启动 0.5~1.5s, 而 status 每 3s 探测、serve/config
# 每次连接都重复枚举 —— 缓存命中 + 轻量连接态验证即可免掉 powershell。
CACHE_FILE = Path(__file__).parent / ".vibekey_ble_addr.json"
CACHE_TTL = 30.0


def _cache_load():
    try:
        import json as _json
        d = _json.loads(CACHE_FILE.read_text("utf-8"))
        addr = int(d.get("addr", 0))
        ts = float(d.get("ts", 0))
        import time as _time
        if addr and (_time.monotonic() - ts) < CACHE_TTL:
            return addr
    except Exception:
        pass
    return None


def _cache_save(addr):
    try:
        import json as _json
        import time as _time
        CACHE_FILE.write_text(_json.dumps({"addr": addr, "ts": _time.monotonic()}), "utf-8")
    except Exception:
        pass


async def resolve_vibekey_address():
    """Pick the VibeKey entry that is *actually connected* right now.

    多个同名设备时逐个开 BluetoothLEDevice 查 ConnectionStatus, 返回真正在线的
    地址; 没有在线条目时退回首个候选(与旧行为一致, 不至于直接判失败)。
    """
    # 缓存命中且验证仍在连接 -> 直接复用(不再跑 powershell)。
    cached = _cache_load()
    if cached:
        try:
            from winrt.windows.devices.bluetooth import BluetoothLEDevice, BluetoothConnectionStatus
            dev = await BluetoothLEDevice.from_bluetooth_address_async(cached)
            ok = False
            if dev is not None:
                try:
                    ok = (dev.connection_status == BluetoothConnectionStatus.CONNECTED)
                finally:
                    try:
                        dev.close()
                    except Exception:
                        pass
            if ok:
                return cached
        except Exception:
            pass

    addrs = find_vibekey_addresses()
    if not addrs:
        return None
    if len(addrs) == 1:
        _cache_save(addrs[0])
        return addrs[0]
    try:
        from winrt.windows.devices.bluetooth import BluetoothLEDevice, BluetoothConnectionStatus
    except Exception:
        return addrs[0]
    for addr in addrs:
        dev = None
        try:
            dev = await BluetoothLEDevice.from_bluetooth_address_async(addr)
            if dev is not None and dev.connection_status == BluetoothConnectionStatus.CONNECTED:
                print("DEBUG: %d VibeKey entries, selected CONNECTED %s"
                      % (len(addrs), hex(addr)), file=sys.stderr)
                _cache_save(addr)
                return addr
        except Exception:
            pass
        finally:
            if dev is not None:
                try:
                    dev.close()
                except Exception:
                    pass
    print("WARN: %d VibeKey entries but none reports CONNECTED, falling back to first"
          % len(addrs), file=sys.stderr)
    return addrs[0]


def _is_vibekey_connected_pnp():
    """Fallback: read the PnP 'device connected' property (DEVPKEY 15).

    NOTE: unreliable for BLE HID devices — the Bluetooth-class container device
    often reports Disconnected while the HID profile link is actually up — so this
    is only used as a fallback when the UWP Bluetooth API is unavailable.
    """
    # NOTE: do NOT set a flag inside ForEach-Object — its script block runs in a
    # child scope, so the assignment would not propagate and the result is wrong.
    # Instead emit each device's connected value into the pipeline and test with
    # -contains, which is scope-safe.
    ps = (
        "$k='{83DA6326-97A6-4088-9453-A1923F573B29} 15';"
        "$vals = Get-PnpDevice -FriendlyName '*VibeKey*' -ErrorAction SilentlyContinue | ForEach-Object {"
        "  (Get-PnpDeviceProperty -InstanceId $_.InstanceId -KeyName $k -ErrorAction SilentlyContinue).Data"
        "};"
        "if ($vals -contains $true) { 'CONNECTED' } else { 'DISCONNECTED' }"
    )
    try:
        result = subprocess.run(
            ['powershell', '-NoProfile', '-Command', ps],
            capture_output=True, text=True, timeout=8
        )
        # NOTE: exact match — 'CONNECTED' is a substring of 'DISCONNECTED'!
        return result.stdout.strip().upper() == 'CONNECTED'
    except Exception:
        return False


async def is_vibekey_connected():
    """Return True only if a VibeKey BLE device is *actually linked* right now.

    PnP connection-status properties are unreliable for BLE HID devices: the
    'Bluetooth' class container device often reports Disconnected even while the
    HID profile link is up. We instead open the device via the UWP Bluetooth API
    and read BluetoothLEDevice.ConnectionStatus, which reflects the real radio link.
    """
    addr = await resolve_vibekey_address()
    if not addr:
        return False
    try:
        from winrt.windows.devices.bluetooth import BluetoothLEDevice, BluetoothConnectionStatus
        dev = await BluetoothLEDevice.from_bluetooth_address_async(addr)
        if dev is None:
            return False
        try:
            return dev.connection_status == BluetoothConnectionStatus.CONNECTED
        finally:
            dev.close()
    except Exception:
        # winrt unavailable/failed — fall back to PnP property detection
        return _is_vibekey_connected_pnp()


async def connect_led_char(stop=None):
    """Connect once and return the writable characteristics. Caller keeps them
    alive so subsequent writes skip reconnect + full GATT discovery.

    ★ 10-04 返回值改为 **(led_ch, cfg_ch)** 二元组：
      led_ch = 0xFF01 状态灯特征（原有行为）
      cfg_ch = 0xFF03 配置命令特征（RGB 预览用；固件 ble_led_service.c 定义的
                "PC 写一条 CONF 命令帧"特征，与 LED 特征同在 0xFF00 服务内）
      拿不到 cfg_ch 时返回 None —— 调用方须容忍（老设备/权限不足时），退回
      原来的一次性进程路径。

    发现策略：已配对设备优先用 Windows 的 GATT 缓存（配对时即建好、稳定可靠），
    仅当缓存里找不到 LED 服务/特征时才用 BluetoothCacheMode.UNCACHED 刷新一次。
    切勿每次都强制 UNCACHED——反复刷新会逐步污染 Windows 的 GATT 缓存，
    导致后续连接偶发“service has no characteristics”。

    09-05: stop 为 threading.Event —— 置位后尽快放弃返回 None(供 serve 的
    "exit" 打断初始连接; 此前 exit 要等整个 3×4 轮发现循环跑完 ≈10s+, 期间
    serve 一直占着 GATT 会话, 挡住紧随其后的键配置 worker, 表现为 AccessDenied)。
    """
    from winrt.windows.devices.bluetooth import BluetoothLEDevice, BluetoothCacheMode
    from winrt.windows.devices.bluetooth.genericattributeprofile import GattCharacteristicProperties
    addr = await resolve_vibekey_address()
    if not addr:
        print("ERROR: VibeKey not found in paired devices", file=sys.stderr)
        return None, None
    dev = await BluetoothLEDevice.from_bluetooth_address_async(addr)
    if not dev:
        print("ERROR: cannot open device from address %s" % hex(addr), file=sys.stderr)
        return None, None
    # 记住设备对象供 serve() 做断连检测(08-29)
    global g_ble_dev
    g_ble_dev = dev

    # 连上后稍等 GATT 库就绪：刚建立链路时 Windows 偶发把特征发现返回空，
    # 短暂延时能显著降低“service has no characteristics”的瞬时失败。
    await asyncio.sleep(0.4)

    # 多次尝试：Windows BLE 缓存偶发漏服务 / 把特征属性误报为 0，重试可兜住。
    for attempt in range(4):
        if stop is not None and stop.is_set():
            return None, None
        # 服务发现：前两轮用缓存（已配对设备的缓存稳定且省时）；
        # 之后强制 UNCACHED 刷新一次 —— 多同名设备/历史配对残留会让 Windows 缓存
        # 留着别台设备的旧服务表，此时不刷新就永远找不到 LED 服务。
        try:
            if attempt < 2:
                result = await gatt_services(dev)
            else:
                result = await gatt_services(dev, uncached=True)
        except Exception as e:
            try:
                result = await gatt_services(dev, uncached=True)
            except Exception as e2:
                print("WARN: get_gatt_services_async failed, retry (%s / %s)" % (e, e2),
                      file=sys.stderr)
                await asyncio.sleep(0.3)
                continue
        if not result or not result.services:
            print("WARN: no GATT services returned (attempt %d)" % (attempt + 1), file=sys.stderr)
            await asyncio.sleep(0.3)
            continue

        print("DEBUG: discovered %d service(s)" % len(result.services), file=sys.stderr)
        for svc in result.services:
            svc_uuid = str(svc.uuid).lower().replace("-", "")
            print("DEBUG: service uuid=%s" % svc_uuid, file=sys.stderr)
            if svc_uuid != LED_SVC_UUID_16.replace("-", ""):
                continue

            # 特征发现：缓存优先；但【缓存返回空】必须退到 UNCACHED 刷新 ——
            # 旧实现只在抛异常时才刷新，于是"服务在缓存里、特征列表为空"（多设备
            # 残留缓存/陈旧 GATT DB 的典型表现）会一路空到放弃，正是日志里
            # "service ... has no characteristics (after retries)" 的成因。
            chars = None
            last_status = None   # 09-05: 区分 AccessDenied(链路未加密)与普通缓存空
            for char_try in range(3):
                for uncached in (False, True):
                    try:
                        chars_result = await gatt_characteristics(svc, uncached=uncached)
                    except Exception:
                        continue
                    if chars_result is not None:
                        last_status = int(chars_result.status)
                    if chars_result and chars_result.characteristics:
                        chars = chars_result.characteristics
                        break
                if chars:
                    break
                await asyncio.sleep(0.2)
            if not chars:
                if last_status == 3:
                    print("WARN: service %s denied (BLE 链路未加密, 配对状态可能"
                          "失效 —— 重启设备电源, 无效则删除设备重新配对)" % svc_uuid,
                          file=sys.stderr)
                else:
                    print("WARN: service %s has no characteristics (after retries)" % svc_uuid,
                          file=sys.stderr)
                continue

            chars = chars_result.characteristics
            # 10-04: 一次遍历收集【两个】可写特征 —— LED(0xFF01) 与配置(0xFF03)
            # 都在本服务内。同时返回 (led_ch, cfg_ch)，调用者持有它们即可复用连接。
            led_ch = None
            cfg_ch = None
            fallback = None
            for ch in chars:
                ch_uuid = str(ch.uuid).lower().replace("-", "")
                props = ch.characteristic_properties
                print("DEBUG: char uuid=%s props=%s" % (ch_uuid, int(props)), file=sys.stderr)
                if ch_uuid == LED_CHAR_UUID_16.replace("-", ""):
                    led_ch = ch
                    continue
                if ch_uuid == CFG_CHAR_UUID_16.replace("-", ""):
                    cfg_ch = ch
                    continue
                if led_ch is None and (int(props) & GattCharacteristicProperties.WRITE or
                                       int(props) & GattCharacteristicProperties.WRITE_WITHOUT_RESPONSE):
                    # 兜底：UUID 对不上时把首个可写特征当 LED 用（旧行为）
                    if fallback is None:
                        fallback = ch
            if led_ch is None:
                led_ch = fallback
            if led_ch is not None:
                print("DEBUG: chars ready: led=%s cfg=%s"
                      % (led_ch is not None, cfg_ch is not None), file=sys.stderr)
                return led_ch, cfg_ch
            # 兜底：Windows 偶发把缓存特征属性误报为 0，降级用首个特征避免进程退出。
            if chars:
                print("WARN: no writable char matched, falling back to first char (props=%s)"
                      % int(chars[0].characteristic_properties), file=sys.stderr)
                return chars[0], None
        # 本轮没找到可写特征：重试一次（避开瞬时缓存漏服务）
        if attempt < 2:
            await asyncio.sleep(0.3)
    print("ERROR: LED service not found", file=sys.stderr)
    return None, None


async def write_led_char(ch, state):
    from winrt.windows.devices.bluetooth.genericattributeprofile import (
        GattCommunicationStatus, GattWriteOption, GattCharacteristicProperties)
    from winrt.windows.storage.streams import DataWriter
    # winrt 3.x 下显式传 GattWriteOption 偶发触发 E_INVALIDARG("Invalid parameter count")，
    # 因此优先用 1 参形式（让 Windows 按特征属性自动选写类型），失败再依次尝试两种显式选项。
    options = [None]  # None => 1-arg form
    props = 0
    try:
        props = int(ch.characteristic_properties)
    except Exception:
        props = 0
    if props:
        if props & GattCharacteristicProperties.WRITE_WITHOUT_RESPONSE:
            options.append(GattWriteOption.WRITE_WITHOUT_RESPONSE)
        if props & GattCharacteristicProperties.WRITE:
            options.append(GattWriteOption.WRITE_WITH_RESPONSE)
    else:
        # ⚠️ 08-28: Windows 缓存异常时会把特征属性误报为 0，此时只剩 1 参形式，
        # 一旦它抛 "Invalid parameter count" 就再无尝试机会（陈旧特征对象的典型
        # 表现）。属性为 0 时把两种显式写类型都排上，确保还有退路。
        options.append(GattWriteOption.WRITE_WITHOUT_RESPONSE)
        options.append(GattWriteOption.WRITE_WITH_RESPONSE)
    last = None
    for opt in options:
        try:
            w = DataWriter()
            w.write_byte(state)
            buf = w.detach_buffer()
            if opt is None:
                res = await ch.write_value_async(buf)
            else:
                res = await ch.write_value_async(buf, opt)
            if res == GattCommunicationStatus.SUCCESS:
                return True
            last = "status=%s" % res
        except Exception as e:
            last = "exc=%s" % e
    print("ERROR: write failed (%s)" % last, file=sys.stderr)
    return False


async def write_cfg_bytes(ch, payload):
    """向配置特征(0xFF03)写一段字节。10-04 新增：供 serve 复用连接下发 RGB 预览。

    与 write_led_char 的写类型回退策略一致（1 参形式优先，再试两种显式选项），
    因为 winrt 3.x 显式传 GattWriteOption 偶发 E_INVALIDARG("Invalid parameter count")。
    """
    from winrt.windows.devices.bluetooth.genericattributeprofile import (
        GattCommunicationStatus, GattWriteOption, GattCharacteristicProperties)
    from winrt.windows.storage.streams import DataWriter
    options = [None]          # None => 1-arg form
    props = 0
    try:
        props = int(ch.characteristic_properties)
    except Exception:
        props = 0
    if props:
        if props & GattCharacteristicProperties.WRITE_WITHOUT_RESPONSE:
            options.append(GattWriteOption.WRITE_WITHOUT_RESPONSE)
        if props & GattCharacteristicProperties.WRITE:
            options.append(GattWriteOption.WRITE_WITH_RESPONSE)
    else:
        # 属性被 Windows 误报为 0 —— 两种显式写类型都排上，确保还有退路
        options.append(GattWriteOption.WRITE_WITHOUT_RESPONSE)
        options.append(GattWriteOption.WRITE_WITH_RESPONSE)
    last = None
    for opt in options:
        try:
            w = DataWriter()
            w.write_bytes(payload)
            buf = w.detach_buffer()
            if opt is None:
                res = await ch.write_value_async(buf)
            else:
                res = await ch.write_value_async(buf, opt)
            if res == GattCommunicationStatus.SUCCESS:
                return True
            last = "status=%s" % res
        except Exception as e:
            last = "exc=%s" % e
    print("ERROR: cfg write failed (%s)" % last, file=sys.stderr)
    return False


def build_rgb_cmd_frame(key_index, enabled, r, g, b, brightness):
    """组一条 CONF_CMD_RGB_APPLY 帧 —— 必须与 config_worker.build_conf_frame 完全一致:
    [4B magic][1B cmd=0x06][1B slot=0][2B data_len=6][idx,en,r,g,b,bri]"""
    import struct
    CONF_MAGIC = 0x434F4E46
    payload = struct.pack("BBBBBB", key_index & 0xFF, enabled & 0xFF,
                          r & 0xFF, g & 0xFF, b & 0xFF, brightness & 0xFF)
    return struct.pack("<IBBH", CONF_MAGIC, 0x06, 0, len(payload)) + payload


async def write_led(state):
    ch, _cfg = await connect_led_char()
    if ch is None:
        return False
    ok = await write_led_char(ch, state)
    print(f"OK state={state}" if ok else "ERROR: write failed")
    return ok


async def serve():
    """Keep ONE BLE connection alive. Read 'led <state>' lines from stdin and write
    immediately (no per-command reconnect / full GATT service rediscovery).

    Protocol (stdout): 'READY' once connected, then 'OK state=N' / 'ERROR ...' per command.
    Send 'exit' (or close stdin) to quit.
    """
    import threading
    # 09-03: 本函数尾部会给 g_ble_dev 赋值(退出前 close 置 None) —— 不加 global
    # 声明 Python 会把整个函数里的 g_ble_dev 视为局部变量, watchdog 里读取即
    # UnboundLocalError(实测 serve 恢复必崩)。
    global g_ble_dev

    loop = asyncio.get_event_loop()
    stop = threading.Event()

    def run(coro):
        return asyncio.run_coroutine_threadsafe(coro, loop).result()

    async def _wake():
        pass

    ch = None
    cfg_ch = None      # 10-04: 配置特征(0xFF03)，RGB 预览复用同一连接

    def handle(state):
        nonlocal ch
        ok = run(write_led_char(ch, state))
        if not ok:
            # link may have dropped; try reconnect once
            ch2, _c2 = run(connect_led_char())
            if ch2 is not None:
                ch = ch2
                ok = run(write_led_char(ch, state))
        print(f"OK state={state}" if ok else "ERROR: write failed")
        sys.stdout.flush()

    def handle_rgb(idx, en, r, g, b, bri):
        """10-04: RGB 预览走**常驻连接**（与 led 同一进程、同一 GATT 会话）。
        这是把延迟从"启进程+服务发现(~1s)"降到"一次 write(毫秒级)"的关键。
        cfg 特征缺失时回退到 led 特征（固件不支持时至少不会崩）。"""
        nonlocal ch, cfg_ch
        target = cfg_ch if cfg_ch is not None else ch
        if target is None:
            print("ERROR: no cfg char for rgb", flush=True)
            return
        frame = build_rgb_cmd_frame(idx, en, r, g, b, bri)
        ok = run(write_cfg_bytes(target, frame))
        if not ok and cfg_ch is not None:
            # 配置特征写失败 → 重连一次再试（连接可能已断）
            ch2, c2 = run(connect_led_char())
            if c2 is not None:
                ch, cfg_ch = ch2, c2
                ok = run(write_cfg_bytes(cfg_ch, build_rgb_cmd_frame(idx, en, r, g, b, bri)))
        print("OK rgb" if ok else "ERROR: rgb write failed", flush=True)

    def reader_thread():
        while not stop.is_set():
            line = sys.stdin.readline()
            if not line:
                break
            line = line.strip()
            if line == "exit":
                stop.set()
                asyncio.run_coroutine_threadsafe(_wake(), loop)
                break
            if line.startswith("rgb "):
                # 协议: rgb <idx> <en> <r> <g> <b> <bri>（与 config_worker.py rgb 子命令一致）
                try:
                    a = line.split()
                    handle_rgb(int(a[1]), int(a[2]), int(a[3]),
                               int(a[4]), int(a[5]), int(a[6]))
                except (IndexError, ValueError):
                    print("ERROR: bad rgb args", flush=True)
                continue
            if line.startswith("led "):
                try:
                    state = int(line.split()[1])
                except (IndexError, ValueError):
                    print("ERROR: bad state")
                    sys.stdout.flush()
                    continue
                handle(state)

    # 09-05: stdin 线程必须先于初始连接启动 —— 此前要等初始连接(3×4 轮发现,
    # 最坏 ≈10s+)结束才读得到 "exit", pauseBleServe 等不到只能强杀, 留下半开
    # GATT 句柄污染缓存并挡住紧随其后的键配置 worker(实测 AccessDenied)。
    # 现在收到 exit 置 stop, connect_led_char 在轮间检查点尽快放弃。
    t = threading.Thread(target=reader_thread, daemon=True)
    t.start()

    for attempt in range(1, 4):
        if stop.is_set():
            break
        print("DEBUG: serve connect attempt %d/3" % attempt, file=sys.stderr)
        ch, cfg_ch = await connect_led_char(stop=stop)
        if ch is not None:
            break
        # 09-03: 失败间隔拉长到 2s —— serve 被杀/配置独占后 Windows 释放连接
        # 需 1~2s, 快速连轰会撞释放窗口并反复 UNCACHED 污染 GATT 缓存
        await asyncio.sleep(2.0)

    def _close_and_exit(code):
        """干净释放 BLE 设备再退出 —— 强杀进程会留半开句柄污染后续发现(09-03)。"""
        dev = g_ble_dev
        if dev is not None:
            try:
                dev.close()
            except Exception:
                pass
            g_ble_dev = None
        sys.stdout.flush()
        sys.exit(code)

    if ch is None:
        if stop.is_set():
            # 09-05: 连接过程中被要求退出 = 配置独占的正常路径, 按 0 退出
            _close_and_exit(0)
        print("ERROR: LED service connect failed", file=sys.stderr)
        _close_and_exit(1)
        return
    if stop.is_set():
        # 连接刚成功就收到 exit(罕见竞态): 释放后退出
        _close_and_exit(0)
    print("READY")
    sys.stdout.flush()

    # ---- 断连看门狗 (08-29 修复"设备断连了还显示已连接") ----
    # 背景：上位机 C++ 端把"常驻 serve 进程 READY"当作连接状态的唯一权威，
    # 而本进程只会在收到 exit / 启动失败时退出 —— 设备断电、走远、休眠都不会
    # 让它退出，于是 UI 长期停在"已连接"。这里主动盯链路：手持 device 对象上读
    # connection_status 取自本地缓存、不新开链路(不像 from_bluetooth_address_async)，
    # 可安全高频查询。连续 N 次非 CONNECTED 才上报 LOST, 避开空闲瞬时掉线抖动
    # (BLE HID 空闲时无线电会瞬断, 单次结果不能当真断连)。
    disc_streak = 0
    link_lost = False
    while not stop.is_set():
        await asyncio.sleep(0.2)
        dev = g_ble_dev
        if dev is None:
            continue
        try:
            from winrt.windows.devices.bluetooth import BluetoothConnectionStatus
            lost = (dev.connection_status != BluetoothConnectionStatus.CONNECTED)
        except Exception:
            lost = False
        if lost:
            disc_streak += 1
            if disc_streak >= 15:          # 0.2s × 15 ≈ 3s 去抖
                print("LOST")
                sys.stdout.flush()
                link_lost = True
                stop.set()
                break
        else:
            disc_streak = 0

    # 09-03: 干净释放 BLE 连接再退出。Windows 强杀进程(不 close)会留下半开
    # GATT 句柄, 污染后续连接的服务发现 —— "配置/灯效要好多次才成功"的根因。
    # 收到 exit(或 LOST)后先把设备对象 close 掉, Windows 才能真正释放链路。
    dev = g_ble_dev
    if dev is not None:
        try:
            dev.close()
        except Exception:
            pass
        g_ble_dev = None

    print("BYE")
    sys.stdout.flush()
    if link_lost:
        sys.exit(2)   # 明确退出码, 便于 C++ 端区分"断连退出"与"正常退出"


async def monitor():
    last = None
    while True:
        try:
            if STATE_FILE.exists():
                with open(STATE_FILE) as f:
                    s = json.load(f).get("state", "idle")
                code = STATE_MAP.get(s, 0)
                if code != last:
                    await write_led(code)
                    last = code
        except:
            pass
        await asyncio.sleep(1.0)


async def _read_battery_from_device(dev):
    """Read battery level (0-100) from an already-open BLE device.
    Returns -1 if the Battery service/characteristic is unavailable or read fails."""
    from winrt.windows.devices.bluetooth import BluetoothCacheMode
    from winrt.windows.devices.bluetooth.genericattributeprofile import GattCommunicationStatus
    from winrt.windows.storage.streams import DataReader

    batt_svc_uuid = BATT_SVC_UUID.replace("-", "")
    batt_char_uuid = BATT_CHAR_UUID.replace("-", "")
    services = None
    # 服务发现：缓存优先，缓存返回空也退到 UNCACHED（多同名设备残留缓存会返回空表）
    for uncached in (False, True):
        try:
            result = await gatt_services(dev, uncached=uncached)
        except Exception:
            continue
        if result and result.services:
            services = result.services
            break

    if not services:
        return -1

    for svc in services:
        svc_uuid = str(svc.uuid).lower().replace("-", "")
        if svc_uuid != batt_svc_uuid:
            continue
        chars = None
        for uncached in (False, True):
            try:
                chars_result = await gatt_characteristics(svc, uncached=uncached)
            except Exception:
                continue
            if chars_result and chars_result.characteristics:
                chars = chars_result.characteristics
                break
        if not chars:
            continue
        for ch in chars:
            ch_uuid = str(ch.uuid).lower().replace("-", "")
            if ch_uuid != batt_char_uuid:
                continue
            try:
                read_result = await ch.read_value_async()
                if read_result and read_result.status == GattCommunicationStatus.SUCCESS:
                    reader = DataReader.from_buffer(read_result.value)
                    val = reader.read_byte()
                    if 0 <= val <= 100:
                        return val
            except Exception:
                pass
            break
        break
    return -1


async def status():
    """Report whether a VibeKey BLE device is currently connected (not just paired),
    and if connected, also report battery level read from the standard BLE Battery Service.

    Output formats:
      DISCONNECTED
      CONNECTED <battery>   # <battery> is 0-100, or -1 if battery read failed
    """
    addr = await resolve_vibekey_address()
    if not addr:
        print("DISCONNECTED")
        return

    try:
        from winrt.windows.devices.bluetooth import BluetoothLEDevice, BluetoothConnectionStatus
        dev = await BluetoothLEDevice.from_bluetooth_address_async(addr)
        if dev is None:
            print("DISCONNECTED")
            return
        try:
            if dev.connection_status != BluetoothConnectionStatus.CONNECTED:
                print("DISCONNECTED")
                return
            batt = await _read_battery_from_device(dev)
            print("CONNECTED %d" % batt)
        finally:
            dev.close()
    except Exception:
        # winrt unavailable/failed — fall back to PnP property detection without battery
        print("CONNECTED -1" if _is_vibekey_connected_pnp() else "DISCONNECTED")


async def diag(count=20, interval=0.3):
    """IMU 诊断 (09-01, 保留): 订阅固件 0xFF02 特征 notify, 循环写触发采样, 打印每帧快照。

    帧格式(16B LE): g3 3×int16(×1000, 设备坐标重力方向) + gyro 3×int16(mdps) + sx/sy 2×int16
    输出: DIAG g3=(+0.312,-0.707,+0.634) gyro=(+123,+456,+789) sx=+1414 sy=-707
    ⚠️ 运行时勿同时开 LED serve 进程(两者争抢同一 BLE 连接)。"""
    import struct
    from winrt.windows.devices.bluetooth import BluetoothLEDevice, BluetoothCacheMode
    from winrt.windows.devices.bluetooth.genericattributeprofile import (
        GattClientCharacteristicConfigurationDescriptorValue)
    from winrt.windows.storage.streams import DataWriter, DataReader

    addr = await resolve_vibekey_address()
    if not addr:
        print("ERROR: VibeKey not found", file=sys.stderr)
        return
    dev = await BluetoothLEDevice.from_bluetooth_address_async(addr)
    if not dev:
        print("ERROR: cannot open device", file=sys.stderr)
        return
    await asyncio.sleep(0.4)

    ch = None
    diag_uuid = DIAG_CHAR_UUID_16.replace("-", "")
    for attempt in range(4):
        try:
            if attempt < 2:
                result = await gatt_services(dev)
            else:
                result = await gatt_services(dev, uncached=True)
        except Exception:
            continue
        if not result or not result.services:
            await asyncio.sleep(0.3)
            continue
        for svc in result.services:
            if str(svc.uuid).lower().replace("-", "") != LED_SVC_UUID_16.replace("-", ""):
                continue
            chars_result = await gatt_characteristics(svc)
            chars = chars_result.characteristics if chars_result else []
            if not chars:
                chars_result = await gatt_characteristics(svc, uncached=True)
                chars = chars_result.characteristics if chars_result else []
            for c in chars:
                if str(c.uuid).lower().replace("-", "") == diag_uuid:
                    ch = c
                    break
            if ch:
                break
        if ch:
            break
        await asyncio.sleep(0.3)
    if not ch:
        print("ERROR: 0xFF02 diag char not found", file=sys.stderr)
        dev.close()
        return

    def on_changed(sender, args):
        try:
            reader = DataReader.from_buffer(args.characteristic_value)
            try:
                raw = reader.read_bytes(16)          # PyWinRT 3.x: count -> bytes/list
            except TypeError:
                b = bytearray(16)
                reader.read_bytes(b)                  # PyWinRT 2.x: 输出到 bytearray
                raw = b
            data = bytes(raw)
            g3x, g3y, g3z, gx, gy, gz, sx, sy = struct.unpack("<8h", data)
            print("DIAG g3=(%+.3f,%+.3f,%+.3f) gyro=(%+d,%+d,%+d) sx=%+d sy=%+d"
                  % (g3x / 1000.0, g3y / 1000.0, g3z / 1000.0, gx, gy, gz, sx, sy))
            sys.stdout.flush()
        except Exception as e:
            print("WARN: parse err %s" % e, file=sys.stderr)

    # ⚠️ winrt 新旧版事件 API 不同: 新版支持 Python `+=`, 旧版(如绿色版捆绑
    # PyWinRT 2.x)只有 add_value_changed()。统一做兼容。
    using_old_events = False
    try:
        ch.value_changed += on_changed
    except AttributeError:
        ch.add_value_changed(on_changed)
        using_old_events = True
    try:
        await ch.write_client_characteristic_configuration_descriptor_async(
            GattClientCharacteristicConfigurationDescriptorValue.NOTIFY)
        print("READY", flush=True)
    except Exception as e:
        print("WARN: enable notify failed: %s" % e, file=sys.stderr)

    for i in range(count):
        ok = await write_led_char(ch, 0)   # 复用写兼容逻辑, 触发固件回发一帧快照
        if not ok:
            print("ERROR: trigger write failed", file=sys.stderr)
            break
        await asyncio.sleep(interval)
    try:
        if using_old_events:
            ch.remove_value_changed(on_changed)
        else:
            ch.value_changed -= on_changed
    except Exception:
        pass
    print("DONE", flush=True)
    dev.close()


async def main():
    if len(sys.argv) < 2:
        print("Usage: ble_led_worker.py led|monitor|status [state]")
        return
    cmd = sys.argv[1]
    if cmd == "led" and len(sys.argv) >= 3:
        await write_led(int(sys.argv[2]))
    elif cmd == "monitor":
        await monitor()
    elif cmd == "status":
        await status()
    elif cmd == "serve":
        await serve()
    elif cmd == "diag":
        count = int(sys.argv[2]) if len(sys.argv) >= 3 else 20
        interval = float(sys.argv[3]) if len(sys.argv) >= 4 else 0.3
        await diag(count, interval)

if __name__ == "__main__":
    asyncio.run(main())
