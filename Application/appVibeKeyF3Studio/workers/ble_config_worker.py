import asyncio
import sys
import json
import time
from pathlib import Path

# ⚠️ 08-27 约定: Windows QProcess 管道下默认 GBK 输出, Qt fromUtf8 乱码; 强制 UTF-8。
try:
    sys.stdout.reconfigure(encoding='utf-8')
    # 10-04: stderr 也要设! C++ 侧用 QString::fromUtf8() 读 stderr，
    # 不设则中文日志输出成 "??" 完全不可读(实测: BLE 服务 denied 的原因说明全变问号)
    sys.stderr.reconfigure(encoding='utf-8')
except AttributeError:
    pass

# 09-03 (BLE 改建键配置): 复用同目录两个 worker 的成熟逻辑 ——
#   ble_led_worker: 多同名设备地址解析 / winrt 连接 / 特征发现框架 / 事件兼容;
#   config_worker : CONF 命令帧组装 / 43B WRITE payload / 52B READ 回包解析(单一事实源)。
# 这样 USB 与 BLE 两条通道的打包/解析永不漂移。
import ble_led_worker as blw
import config_worker as cw

# 固件 0xFF00 服务下的配置特征(16-bit SIG base 展开串, 与 ble_led_worker 的写法一致)
CFG_TX_UUID = "0000ff03-0000-1000-8000-00805f9b34fb"   # 写 CONF 命令帧
CFG_RX_UUID = "0000ff04-0000-1000-8000-00805f9b34fb"   # notify 回包帧

RESP_TIMEOUT = 6.0    # 每命令等回包超时(秒)。BLE 首次连接+服务发现慢, 放宽。

# 单帧承载能力取决于协商后的 ATT MTU。Windows 客户端总是发起 MTU 交换,
# 固件侧 L2CAP ATT MTU 已由 proj.conf(CONFIG_BT_L2CAP_TX_MTU) 提到 340(09-07,
# 原 128 放不下 L 键扩展后的 277B 写帧 / 276B READ 回包):
#   Write Request 载荷上限 = MTU-3 = 337B >= 323B(命令帧, 10-04 EC 按压滚动扩展后),
#   notify 同为 337B >= 324B(READ 回包)。均为单帧。
# 10-05 固件已实现 EC 按压滚动(详见 config_worker.py 头部注释):
#   特征 max_len 同步 320->336, READ 回 324B / 写帧 323B, MTU=340(MTU-3=337) 均满足。
# 若实测 MTU 偏低(旧固件仍 128), 读长回包会失败 —— 需升级固件, 日志会打印 MTU。
MAX_FRAME = 336       # 固件特征 max_len(10-05 随 323B 写帧/324B READ 回包由 320 放宽; 预留)

g_resp_event = None    # asyncio.Event: 收到一帧 notify
g_resp_data = None     # 收到的字节
g_mtu = 0              # 09-06: 最近一次连接的协商 ATT MTU(0=探测失败)


def log(msg):
    print(msg, file=sys.stderr)
    sys.stderr.flush()


# 09-03 提速: 进程内缓存最近一次成功解析的设备地址(30s 内复用)。
# resolve_vibekey_address 每次都要跑 powershell Get-PnpDevice(0.5~1.5s),
# 连续读取时没必要重复枚举。
_cached_addr = None
_cached_addr_ts = 0.0


def cached_resolve():
    global _cached_addr, _cached_addr_ts
    addr = _cached_addr
    if not addr or (time.monotonic() - _cached_addr_ts) > 30.0:
        addr = None
    return addr


async def connect_cfg_chars():
    """连接 VibeKey 并返回 (dev, tx_ch, rx_ch, mtu)。tx=0xFF03(写命令),
    rx=0xFF04(notify 回包)。找不到返回 (None,)*4。"""
    from winrt.windows.devices.bluetooth import BluetoothLEDevice, BluetoothCacheMode
    global _cached_addr, _cached_addr_ts
    addr = cached_resolve()
    if not addr:
        addr = await blw.resolve_vibekey_address()
        _cached_addr = addr
        _cached_addr_ts = time.monotonic()
    if not addr:
        log("ERROR: VibeKey not found in paired devices")
        return None, None, None, 0

    dev = await BluetoothLEDevice.from_bluetooth_address_async(addr)
    if not dev:
        log("ERROR: cannot open device from address %s" % hex(addr))
        return None, None, None, 0
    # ⚠️ 10-04 回退：曾把这里从 1.2s 砍到 0.05s"提速"，是**错的** ——
    #   实测导致 config worker 与 BLE serve 同时做 GATT 服务发现，两个客户端
    #   抢同一条连接 ⇒ 双方 AccessDenied（日志实测：0xFF00 服务 denied +
    #   "cfg chars not found"）。设备端同一时刻只允许一个 GATT 客户端，
    #   这个窗口就是给 serve 干净退出的，**勿再缩短**。
    await asyncio.sleep(1.2)   # GATT 库就绪; 也给被暂停 serve 的退出留出重叠窗口

    # 09-05: 尝试 4→10 次 —— 设备端同一时刻只容一个 GATT 客户端做发现(并发发现
    # 返回 AccessDenied/空)。pauseBleServe 与本 worker 启动存在固有重叠(serve 要
    # 从发现循环里退出), 重试必须能熬过这个窗口; 中途一旦独占即可命中。
    tx = rx = None
    last_status = None   # 09-05: 记录最后一次特征发现状态, 失败时给出可执行提示
    for attempt in range(10):
        try:
            if attempt < 2:
                result = await blw.gatt_services(dev)
            else:
                # UNCACHED 刷新: OS GATT 缓存被污染(服务在/特征空)时的唯一自愈手段。
                # 09-05 起必须走 blw.gatt_services —— winrt 3.x 不再投影
                # get_gatt_services_async(mode) 重载(报 Invalid parameter count)。
                result = await blw.gatt_services(dev, uncached=True)
        except Exception as e:
            log("WARN: get_gatt_services_async failed (%s)" % e)
            await asyncio.sleep(0.5)   # 09-03: 释放窗口已由 0.6s 前导覆盖, 重试不必久等
            continue
        if not result or not result.services:
            await asyncio.sleep(0.5)
            continue
        for svc in result.services:
            svc_uuid = str(svc.uuid).lower().replace("-", "")
            if svc_uuid != blw.LED_SVC_UUID_16.replace("-", ""):
                continue
            chars = None
            for uncached in (False, True):
                try:
                    chars_result = await blw.gatt_characteristics(svc, uncached=uncached)
                except Exception:
                    continue
                if chars_result is not None:
                    last_status = int(chars_result.status)
                if chars_result and chars_result.characteristics:
                    chars = chars_result.characteristics
                    break
            if not chars:
                continue
            for ch in chars:
                u = str(ch.uuid).lower().replace("-", "")
                if u == CFG_TX_UUID.replace("-", ""):
                    tx = ch
                elif u == CFG_RX_UUID.replace("-", ""):
                    rx = ch
            if tx and rx:
                break
        if tx and rx:
            break
        await asyncio.sleep(0.3)
    if not tx or not rx:
        # 09-05: status=3 = GattCommunicationStatus.AccessDenied —— 链路未加密,
        # 设备侧配对/密钥状态失效(服务在、电池服务等普通服务正常, 仅受保护服务
        # 被拒)。重启设备电源可恢复; 无效则需在 Windows 删除设备后重新配对。
        if last_status == 3:
            log("ERROR: cfg chars not found: BLE 链路未加密(AccessDenied), "
                "配对状态可能失效 —— 请先重启 VibeKey 电源重试; 无效则在 Windows "
                "蓝牙设置中删除 VibeKey-F3 后重新配对")
        else:
            log("ERROR: cfg chars not found (tx=%s rx=%s last_status=%s)"
                % (tx is not None, rx is not None, last_status))
        try:
            dev.close()
        except Exception:
            pass
        return None, None, None, 0

    # 实测协商 MTU(方案验证点): GattSession.max_pdu_size。
    # 09-06 修: winrt 3.x 的 from_device_id_async 需要 BluetoothDeviceId 对象,
    # 传字符串(dev.device_id)会抛异常 -> mtu 恒 0, 日志看不到真实协商值。
    global g_mtu
    mtu = 0
    try:
        from winrt.windows.devices.bluetooth.genericattributeprofile import GattSession
        session = await GattSession.from_device_id_async(dev.bluetooth_device_id)
        if session:
            mtu = int(session.max_pdu_size)
            session.close()
    except Exception as e:
        log("WARN: mtu probe failed: %s" % e)
        mtu = 0
    g_mtu = mtu
    log("DEBUG: cfg link up (mtu=%d)" % mtu)
    return dev, tx, rx, mtu


async def subscribe_rx(rx_ch, loop):
    """订阅 0xFF04 CCCD notify; 兼容新旧 winrt 事件 API(参照 ble_led_worker.diag)。"""
    from winrt.windows.devices.bluetooth.genericattributeprofile import (
        GattClientCharacteristicConfigurationDescriptorValue)

    def on_changed(sender, args):
        global g_resp_data
        try:
            from winrt.windows.storage.streams import DataReader
            reader = DataReader.from_buffer(args.characteristic_value)
            try:
                raw = reader.read_bytes(int(args.characteristic_value.length))
            except TypeError:
                n = int(args.characteristic_value.length)
                b = bytearray(n)
                reader.read_bytes(b)
                raw = bytes(b)
            if isinstance(raw, bytes):
                data = raw
            else:
                data = bytes(raw)
            g_resp_data = data
            loop.call_soon_threadsafe(g_resp_event.set)
        except Exception as e:
            log("WARN: notify parse err %s" % e)

    using_old = False
    try:
        rx_ch.value_changed += on_changed
    except AttributeError:
        rx_ch.add_value_changed(on_changed)
        using_old = True
    try:
        await rx_ch.write_client_characteristic_configuration_descriptor_async(
            GattClientCharacteristicConfigurationDescriptorValue.NOTIFY)
    except Exception as e:
        log("WARN: enable notify failed: %s" % e)
    return on_changed, using_old


async def do_cmd(tx_ch, cmd_bytes):
    """写一条 CONF 命令帧并等固件 notify 回包(每命令一帧)。返回回包字节或 None(超时/失败)。"""
    from winrt.windows.devices.bluetooth.genericattributeprofile import (
        GattCommunicationStatus, GattWriteOption)
    from winrt.windows.storage.streams import DataWriter

    global g_resp_data
    g_resp_data = None
    g_resp_event.clear()

    # 写: 优先 1 参形式(自动选类型), 失败依次试显式选项(与 write_led_char 同策略)
    w = DataWriter()
    w.write_bytes(cmd_bytes)
    buf = w.detach_buffer()
    last = None
    for opt in (None, GattWriteOption.WRITE_WITH_RESPONSE, GattWriteOption.WRITE_WITHOUT_RESPONSE):
        try:
            if opt is None:
                res = await tx_ch.write_value_async(buf)
            else:
                res = await tx_ch.write_value_async(buf, opt)
            if res == GattCommunicationStatus.SUCCESS:
                last = "ok"
                break
            last = "status=%s" % res
        except Exception as e:
            last = "exc=%s" % e
    if last != "ok":
        log("ERROR: cmd write failed (%s)" % last)
        return None

    try:
        await asyncio.wait_for(g_resp_event.wait(), timeout=RESP_TIMEOUT)
    except asyncio.TimeoutError:
        log("ERROR: no notify response within %.1fs (mtu=%d)" % (RESP_TIMEOUT, g_mtu))
        return None
    return g_resp_data


async def get_device_info(tx_ch):
    """INFO 命令一次返回固件版本/电量/激活槽(MODEL|FW_VERSION|bat|slot)。
    供取激活槽与透传版本号共用, 避免重复 query。"""
    resp = await do_cmd(tx_ch, cw.build_conf_frame(cw.CONF_CMD_INFO, 0))
    if not resp:
        return None
    try:
        text = resp.decode("utf-8", errors="replace")
    except Exception:
        return None
    return cw.parse_info_response(text)


async def get_active_slot(tx_ch):
    """INFO 命令拿当前激活蓝牙槽(1~3)。失败返回 0(调用方按槽1兜底)。"""
    info = await get_device_info(tx_ch)
    if info and 1 <= info.get("slot", 0) <= 3:
        return info["slot"]
    return 0


async def cmd_read(tx_ch, slot):
    """读指定槽(0~2)的 Flash 配置 —— 与 USB 语义一致, 无需设备切槽。
    配置存在每槽独立 4KB Flash 页, 固件可按任意槽读。"""
    resp = await do_cmd(tx_ch, cw.build_conf_frame(cw.CONF_CMD_READ, slot))
    if not resp:
        # 10-05: READ 回包 = 324B(EC 按压滚动扩展后, sizeof(key_config_storage_t))。
        # 若固件 ATT MTU < 327, 固件发不出这条通知(写命令本身成功, 但回包石沉大海) ——
        # 典型成因是固件工程丢了 proj.conf 的 CONFIG_BT_L2CAP_TX_MTU=340。
        log("ERROR: READ 回包未收到 —— 回包 324B 需固件 ATT MTU>=327 "
            "(实测协商 mtu=%d)。请确认固件 proj.conf 含 CONFIG_BT_L2CAP_TX_MTU=340 "
            "并删除固件 build 目录重新编译烧录" % g_mtu)
        return False
    try:
        cfg = cw.parse_read_response(resp)
    except ValueError as e:
        print("ERROR:%s" % e)
        return False
    print(json.dumps(cfg))
    print("DONE", flush=True)
    return True


async def cmd_write(tx_ch, config_path, slot):
    with open(config_path, "r") as f:
        config = json.load(f)
    try:
        payload = cw.build_write_payload(config)
    except ValueError as e:
        print("ERROR:%s" % e)
        return False
    frame = cw.build_conf_frame(cw.CONF_CMD_WRITE, slot, payload)
    resp = await do_cmd(tx_ch, frame)
    if not resp:
        return False
    if len(resp) >= 1 and resp[0] == 0x00:
        print("DONE", flush=True)
        return True
    print("ERROR:Device rejected config", flush=True)
    return False


async def cmd_reset(tx_ch, slot):
    resp = await do_cmd(tx_ch, cw.build_conf_frame(cw.CONF_CMD_RESET, slot))
    if not resp:
        return False
    if len(resp) >= 1 and resp[0] == 0x00:
        print("DONE", flush=True)
        return True
    print("ERROR:Device rejected reset", flush=True)
    return False


async def cmd_rgb(tx_ch, key_index, enabled, r, g, b, brightness):
    """C1/C2/C3 单键 RGB 实时预览（不落盘）—— 10-04 新增。

    与 USB 侧 config_worker.py 的 conf_rgb() 完全对应（同一 CONF_CMD_RGB_APPLY 帧），
    固件 dfu_device.c 一视同仁处理，不区分 USB/BLE。
    ⚠️ 此前本 worker 只有 read/write/reset（无 rgb）⇒ 蓝牙通道下按"亮/灭"毫无反应，
    而 USB 通道正常。这是"按亮灭固件无反应"的根因（10-04 真机确认通道=蓝牙）。
    """
    import struct
    payload = struct.pack("BBBBBB", key_index & 0xFF, enabled & 0xFF,
                          r & 0xFF, g & 0xFF, b & 0xFF, brightness & 0xFF)
    resp = await do_cmd(tx_ch, cw.build_conf_frame(cw.CONF_CMD_RGB_APPLY, 0, payload))
    if not resp:
        print("ERROR:no response from device (cmd not delivered?)", flush=True)
        return False
    if len(resp) >= 1 and resp[0] == 0x00:
        # 10-04: 固件改为回 2 字节 [0]=0x00 已受理 [1]=实际生效的限幅后亮度。
        # 打印出来便于确认"命令真的到了 + 限幅真的生效"，而不是只看到笼统的 DONE。
        eff = resp[1] if len(resp) >= 2 else -1
        print("DONE eff_bri=%d" % eff, flush=True)
        return True
    print("ERROR:Device rejected RGB command (code=%s len=%d)"
          % (resp[0] if resp else "none", len(resp)), flush=True)
    return False


async def main():
    global g_resp_event
    if len(sys.argv) < 2:
        print("ERROR:Usage: ble_config_worker.py read|write|reset|rgb [args...] [slot]")
        return 1
    cmd = sys.argv[1]
    if cmd == "write":
        # 09-03 修: 之前 < 3 在主入口已 return 1, write 分支单独检查 config_path 即可
        if len(sys.argv) < 3:
            print("ERROR:Usage: ble_config_worker.py write <config.json> [slot]")
            return 1
    elif cmd == "rgb":
        # 10-04 新增: rgb <key_index> <enabled> <r> <g> <b> <brightness>
        if len(sys.argv) < 8:
            print("ERROR:Usage: ble_config_worker.py rgb <key> <on> <r> <g> <b> <bri>")
            return 1
    elif cmd not in ("read", "reset"):
        print("ERROR:Unknown command: %s" % cmd)
        return 1

    # read/reset: 参数2 可带 slot; write: 参数2=json, 参数3=slot
    # rgb(10-04): 参数2~7 = key,on,r,g,b,bri —— **没有 slot**，所以必须跳过，
    #            否则会把 key_index("0") 当成 slot，后面白跑一轮 INFO/slot 解析。
    slot_arg = None
    config_path = None
    if cmd == "write":
        config_path = sys.argv[2]
        if len(sys.argv) >= 4:
            slot_arg = sys.argv[3]
    elif cmd != "rgb":
        if len(sys.argv) >= 3:
            slot_arg = sys.argv[2]

    loop = asyncio.get_event_loop()
    g_resp_event = asyncio.Event()
    # 09-03 提速: serve 干净退出后 Windows 释放连接 ~0.5s, 而连接内部还有
    # resolve(缓存 0.2s) + 0.4s GATT 就绪才做发现, 前导 0.3s 足够; 撞窗口由
    # connect_cfg_chars 多轮重试(0.5s 间隔)兜底。
    await asyncio.sleep(0.3)
    dev, tx_ch, rx_ch, mtu = await connect_cfg_chars()
    if tx_ch is None or rx_ch is None:
        return 1
    try:
        on_changed, using_old = await subscribe_rx(rx_ch, loop)

        # INFO 响应自带固件版本号(格式 "MODEL|FW_VERSION|bat|slot"), 趁连接一次性取出;
        # 版本与槽无关, 自动读/手动读都透传, 供上位机在蓝牙连接状态里展示版本。
        # 10-04 提速：rgb 预览【跳过】——它既不读也不写配置，版本/槽位对它毫无意义，
        # 每多一次 INFO 往返就多等一个 notify 周期(实测是主要延迟来源之一)。
        # ⚠️ info 必须**无条件**先赋值 None 再按需填充 —— 下面的 slot 解析是无条件执行的，
        #    若只在 `if cmd != "rgb"` 里赋值，rgb 路径访问 info 会抛
        #    UnboundLocalError(10-04 实测踩过)。
        info = None
        if cmd != "rgb":
            info = await get_device_info(tx_ch)
            if info and info.get("version"):
                print("VERSION %s" % info["version"], flush=True)

        # 10-04: rgb 与槽无关，**整段跳过** slot 解析（否则既白等 INFO、又多打 "SLOT n"，
        # 而且 slot 解析会访问 info，rgb 路径根本没取过 info ⇒ UnboundLocalError）。
        if cmd == "rgb":
            ok = await cmd_rgb(tx_ch,
                               int(sys.argv[2]), int(sys.argv[3]),
                               int(sys.argv[4]), int(sys.argv[5]),
                               int(sys.argv[6]), int(sys.argv[7]))
        else:
            # 09-03 槽语义(与 USB 完全一致, 配置在各槽独立 Flash 页, 无需设备切槽):
            #   slot 参数 = 0~2 -> 直接读写该槽(手动点槽 tab / 读取按钮, 尊重用户选择,
            #                 不输出 SLOT, 不弹回);
            #   slot 缺失或 0xFF   -> 先 INFO 拿"当前激活槽"再操作(连接自动读用,
            #                 顺带输出 "SLOT n" 供 C++ 同步 UI 到当前生效槽)。
            try:
                slot_val = int(slot_arg) if slot_arg is not None else 0xFF
            except ValueError:
                slot_val = 0xFF
            if 0 <= slot_val <= 2:
                slot = slot_val
            else:
                # 09-05 修: INFO 回包的激活槽是 1~3(1-based), 而帧头 slot 是 0~2
                # (0-based, build_conf_frame 注释: slot 0~2=槽1~3)。原实现把 1~3
                # 直接透传进帧头, 导致自动读(0xFF 路径)永远错位: 激活槽1→读槽2,
                # 激活槽2→读槽3, 激活槽3→帧头3越界被固件回落槽1; INFO 解析失败
                # 兜底 slot=1 同样错读槽2。表现即"蓝牙连上自动读的第一次, 显示的
                # 不是当前激活槽的配置"(手动点槽读走 0~2 分支不受影响)。
                if info and 1 <= info.get("slot", 0) <= 3:
                    slot = info["slot"] - 1
                else:
                    slot = 0     # 兜底槽1
                    log("WARN: INFO slot parse failed, fallback slot=1")
                print("SLOT %d" % (slot + 1), flush=True)   # 输出保持 1~3, 供 C++ 同步 UI

            if cmd == "read":
                ok = await cmd_read(tx_ch, slot)
            elif cmd == "write":
                ok = await cmd_write(tx_ch, config_path, slot)
            else:
                ok = await cmd_reset(tx_ch, slot)

        try:
            if using_old:
                rx_ch.remove_value_changed(on_changed)
            else:
                rx_ch.value_changed -= on_changed
        except Exception:
            pass
        return 0 if ok else 1
    finally:
        try:
            dev.close()
        except Exception:
            pass


if __name__ == "__main__":
    sys.exit(asyncio.run(main()))
