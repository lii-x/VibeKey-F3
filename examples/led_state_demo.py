#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""VibeKey-F3 状态灯接口 —— 最小可运行示例

对应文档: docs/led-rgb-protocol.md 第 2 节「接口一：状态灯」
          （第 2.4 节 BLE 示例的可直接运行版本）

接口语义(一句话):
    往 GATT 特征 0xFF01 写【1 个字节】= 状态码, 固件据此点亮固定颜色的指示灯。
    这里【没有 RGB 数值】—— 想要任意颜色请用接口二(RGB 底光, 特征 0xFF03)。

    ┌──────┬─────────────────┬────────────────────────────┐
    │ 状态码 │ 含义             │ 物理灯                      │
    ├──────┼─────────────────┼────────────────────────────┤
    │  0   │ OFF  全灭        │ LED2/3/4 全关               │
    │  1   │ BUSY 忙碌(黄)    │ LED2                        │
    │  2   │ DONE 完成(绿)    │ LED3                        │
    │  3   │ ERROR 错误(红)   │ LED4                        │
    │  4   │ BLUE 蓝灯        │ LED1(借用, 见下方注意)       │
    └──────┴─────────────────┴────────────────────────────┘

用法
----
    # 1) 先看看电脑上能扫到哪些 BLE 设备(确认名字/地址)
    python led_state_demo.py --scan

    # 2) 自动按名字找设备, 跑一遍默认演示: 绿 -> 停 1s -> 灭
    python led_state_demo.py

    # 3) 指定地址 + 指定状态 + 保持 2 秒
    python led_state_demo.py --addr AA:BB:CC:DD:EE:FF --state 3 --hold 2

    # 4) 指定状态, 但执行完不熄灭(方便肉眼观察)
    python led_state_demo.py --state 1 --keep

    # 5) 顺便把固件回执打出来(需要先订阅 0xFF04)
    python led_state_demo.py --state 2 --verbose

依赖
----
    pip install bleak

    ⚠️ 本示例用 bleak(跨平台、代码最短, 便于教学)。
       工程里实际运行的上位机用的是 Windows 原生 winrt, 见
       Application/appVibeKeyF3Studio/workers/ble_led_worker.py 的 write_led_char()。
       两者对设备而言完全等价 —— 都是"往 0xFF01 写 1 字节"。

注意
----
  * 状态码只接受 0~4。其他值会被固件【静默丢弃】: 不报错、不回执、不打日志,
    看起来就像"命令没发出去"。这是排查时最常见的误判点。
  * 生效有延迟: 固件的 GPIO 操作在 50ms 软定时器里跑, 写完最多 50ms 才看到灯变。
  * BLE 一断开, 固件会主动熄灭 LED2/3/4(避免断连后还亮着浪费电)。
  * 状态码 4(蓝灯)借用的是 LED1 —— 那颗灯原本归充电指示 / 鼠标故障闪烁使用,
    退出蓝灯态时固件只在"未充电且无故障闪烁"时才熄灭它, 属于设计上的共用。
"""

from __future__ import annotations

import argparse
import asyncio
import sys

# ---- UUID: 16-bit 厂商 UUID 展开成 128-bit 标准写法 -------------------------
# 固件里注册的是 16-bit(0xFF00/0xFF01/0xFF04), 空口只占 2 字节;
# 但 Windows/macOS/Linux 的 API 都要求传 128-bit 字符串, 故这里用 SIG base 展开。
SVC_UUID = "0000ff00-0000-1000-8000-00805f9b34fb"
CHAR_LED = "0000ff01-0000-1000-8000-00805f9b34fb"  # 状态灯(只写)
CHAR_RSP = "0000ff04-0000-1000-8000-00805f9b34fb"  # 回执(notify)

STATE_NAMES = {
    0: "OFF   全灭",
    1: "BUSY  黄灯",
    2: "DONE  绿灯",
    3: "ERROR 红灯",
    4: "BLUE  蓝灯",
}

# 默认设备名关键字。固件的广播名是 "VibeKey", 不同批次可能带 -F3 后缀,
# 所以用子串匹配而不是全等 —— 工程里 PowerShell 也是用 "*VibeKey*" 找的。
DEFAULT_NAME_HINT = "VibeKey"


def _require_bleak():
    """延迟导入: 这样 --help 在没有装 bleak 的机器上也能正常显示。"""
    try:
        from bleak import BleakClient, BleakScanner
    except ImportError:
        print("[错误] 缺少依赖 bleak。请先安装：\n\n    pip install bleak\n", file=sys.stderr)
        print("提示：本工程上位机实际用 Windows 原生的 winrt，不依赖 bleak；",
              file=sys.stderr)
        print("      这里用 bleak 只是为了让示例更短、可跨平台。", file=sys.stderr)
        sys.exit(2)
    return BleakClient, BleakScanner


# ============================ 扫 描 ============================

async def cmd_scan(BleakScanner, timeout: float) -> int:
    """列出附近所有 BLE 设备，方便确认设备名和地址。"""
    print(f"[扫描] 正在搜索 {timeout:.0f} 秒内的 BLE 广播 …")
    devices = await BleakScanner.discover(timeout=timeout)

    if not devices:
        print("[扫描] 没扫到任何设备。请确认：")
        print("       · 键盘已开机且未连到手机/其他电脑")
        print("       · 电脑蓝牙已打开，且在有效距离内")
        return 1

    print(f"[扫描] 共 {len(devices)} 个设备：\n")
    print(f"  {'地址':<38} {'信号':>5}  名称")
    print(f"  {'-' * 38} {'-' * 5}  {'-' * 20}")

    hit = []
    for d in sorted(devices, key=lambda x: x.name or ""):
        name = d.name or "(未命名)"
        rssi = getattr(d, "rssi", None)
        rssi_s = f"{rssi}" if rssi is not None else "-"
        print(f"  {d.address:<38} {rssi_s:>5}  {name}")
        if DEFAULT_NAME_HINT.lower() in name.lower():
            hit.append(d)

    print()
    if hit:
        print(f"[扫描] 找到 {len(hit)} 个疑似 VibeKey：")
        for d in hit:
            print(f"       {d.address}   {d.name}")
        print(f"\n       下一步：python {sys.argv[0]} --addr {hit[0].address}")
    else:
        print("[扫描] 没有名称包含 'VibeKey' 的设备。")
        print("       如果设备确实在，用 --addr 指定上面列出的地址即可。")
    return 0


# ============================ 找设备 ============================

async def find_device(BleakScanner, name_hint: str, timeout: float):
    """按名字找设备。优先用 find_device_by_name(新 API)，老版本回退到全量扫描。"""
    print(f"[连接] 按名称匹配 '{name_hint}'，最多等 {timeout:.0f} 秒 …")
    try:
        return await BleakScanner.find_device_by_name(name_hint, timeout=timeout)
    except AttributeError:
        # 部分 bleak 版本没有 find_device_by_name，退回手动过滤
        devices = await BleakScanner.discover(timeout=timeout)
        for d in devices:
            if d.name and name_hint.lower() in d.name.lower():
                return d
        return None


# ============================ 写 入 ============================

async def ensure_services(client) -> None:
    """确保 GATT 服务已发现。

    老版本 bleak(<=0.22) 需要在连接后手动 await client.get_services()；
    新版本连接时已自动发现，且已移除该方法 —— 因此这里做成兼容调用，
    不能直接写死 get_services()，否则在新版上会抛 AttributeError 被当成"连接失败"。
    """
    fn = getattr(client, "get_services", None)
    if fn is not None:
        await fn()


async def write_state(client, state: int, verbose: bool) -> bool:
    """往 0xFF01 写 1 个字节。返回是否成功。

    写类型说明：
      固件给 0xFF01 同时开了 WRITE_REQ(带响应) 和 WRITE_COMMAND(不带响应)，
      所以 response=False 是最省空口的写法；
      个别 Windows 蓝牙栈对 without-response 处理异常，故失败时用 response=True 重试一次。
    """
    payload = bytes([state])

    try:
        await client.write_gatt_char(CHAR_LED, payload, response=False)
    except Exception as e1:
        print(f"       · 不带响应写入失败({e1})，改用带响应重试 …")
        try:
            await client.write_gatt_char(CHAR_LED, payload, response=True)
        except Exception as e2:
            print(f"       ✗ 写入失败: {e2}")
            return False

    print(f"       ✓ 已写入 0x{state:02X}  →  {STATE_NAMES.get(state, '未知状态码')}")

    if verbose:
        # 回执走 0xFF04 的 notify，最多等 1 秒；收不到不算失败
        # （BLE 的 write 成功本身已经说明命令送达）
        try:
            await asyncio.sleep(0.4)
        except Exception:
            pass

    return True


# ============================ 主 流 程 ============================

async def subscribe_reply(client) -> bool:
    """订阅 0xFF04 回执通道，打印固件回的 1 字节状态码。

    回执不是必需的：BLE 的 write 成功本身已说明命令送达。
    但只要想知道"固件到底认没认"，就必须先订阅 notify，否则永远收不到。
    """
    def on_reply(_sender, data: bytearray):
        payload = bytes(data)
        ok = bool(payload) and payload[0] == 0
        print(f"       ← 回执 0x{payload.hex(' ')}  ({'成功' if ok else '非零状态'})")

    try:
        await client.start_notify(CHAR_RSP, on_reply)
        print("[连接] 已订阅回执特征 0xFF04")
        return True
    except Exception as e:
        print(f"[连接] 订阅回执失败({e})，不影响写入")
        return False


async def demo_run(BleakClient, scanner, addr, state, hold, keep, verbose, scan_timeout, name_hint):
    # ---- 1. 找到设备 ----
    if addr:
        target = addr
        print(f"[连接] 使用指定地址 {target}")
    else:
        device = await find_device(scanner, name_hint, scan_timeout)
        if device is None:
            print(f"[连接] ✗ 没找到名字包含 '{name_hint}' 的设备。")
            print("       请先跑一次 --scan 看看实际广播名，或用 --addr 指定地址。")
            return 3
        target = device
        print(f"[连接] 找到 {getattr(device, 'name', '')} @ {device.address}")

    # ---- 2. 连上并写状态 ----
    print("[连接] 正在建立 BLE 连接 …")
    try:
        async with BleakClient(target, timeout=15.0) as client:
            print("[连接] ✓ 已连接")
            await ensure_services(client)

            if verbose:
                await subscribe_reply(client)

            ok = await write_state(client, state, verbose)
            if not ok:
                return 4

            if hold > 0:
                print(f"[等待] 保持 {hold:.1f} 秒 …")
                await asyncio.sleep(hold)

            if not keep and state != 0:
                print("[收尾] 熄灭 …")
                await write_state(client, 0, False)

            if verbose:
                try:
                    await client.stop_notify(CHAR_RSP)
                except Exception:
                    pass

    except Exception as e:
        print(f"[连接] ✗ 连接/通信失败: {e}")
        print("       常见原因：设备已被手机或其他程序占用；或未配对。")
        return 5

    print("[完成]")
    return 0


async def demo_sequence(BleakClient, scanner, addr, verbose, scan_timeout, name_hint):
    """默认演示：绿 -> 停 1 秒 -> 灭，和文档 2.4 的示例完全一致。"""
    if addr:
        target = addr
        print(f"[连接] 使用指定地址 {target}")
    else:
        device = await find_device(scanner, name_hint, scan_timeout)
        if device is None:
            print(f"[连接] ✗ 没找到名字包含 '{name_hint}' 的设备，请先 --scan 或用 --addr 指定。")
            return 3
        target = device
        print(f"[连接] 找到 {getattr(device, 'name', '')} @ {device.address}")

    print("[连接] 正在建立 BLE 连接 …")
    try:
        async with BleakClient(target, timeout=15.0) as client:
            print("[连接] ✓ 已连接（保持连接期间共用一个 GATT 会话）")
            await ensure_services(client)

            if verbose:
                await subscribe_reply(client)

            print("[演示] 1/2 点亮绿灯(DONE)")
            await write_state(client, 2, verbose)
            await asyncio.sleep(1.0)

            print("[演示] 2/2 熄灭")
            await write_state(client, 0, verbose)
    except Exception as e:
        print(f"[连接] ✗ 连接/通信失败: {e}")
        return 5

    print("[完成] 演示结束")
    return 0


# ============================ CLI ============================

def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="led_state_demo.py",
        description="VibeKey-F3 状态灯接口示例（往 GATT 0xFF01 写 1 字节状态码）",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="""示例:
  python led_state_demo.py --scan
  python led_state_demo.py
  python led_state_demo.py --state 3 --hold 2
  python led_state_demo.py --addr AA:BB:CC:DD:EE:FF --state 1 --keep
  python led_state_demo.py --state 2 --verbose
""",
    )
    p.add_argument("--scan", action="store_true",
                   help="只扫描附近 BLE 设备并退出（不连接）")
    p.add_argument("--addr", metavar="ADDR",
                   help="设备地址，如 AA:BB:CC:DD:EE:FF（不填则按名称自动查找）")
    p.add_argument("--name", default=DEFAULT_NAME_HINT, metavar="NAME",
                   help=f"自动查找时的名称关键字，默认 {DEFAULT_NAME_HINT!r}")
    p.add_argument("--state", type=int, choices=[0, 1, 2, 3, 4], default=None,
                   metavar="N",
                   help="状态码 0=灭 1=忙(黄) 2=完成(绿) 3=错误(红) 4=蓝灯；"
                        "不填则跑默认演示（绿 1 秒 → 灭）")
    p.add_argument("--hold", type=float, default=0.0, metavar="SEC",
                   help="写入后保持多少秒再熄灭（默认 0，立即熄灭）")
    p.add_argument("--keep", action="store_true",
                   help="执行完不自动熄灭（方便肉眼观察）")
    p.add_argument("--verbose", "-v", action="store_true",
                   help="订阅 0xFF04 并打印固件回执")
    p.add_argument("--scan-timeout", type=float, default=8.0, metavar="SEC",
                   help="扫描超时秒数，默认 8")
    return p


async def amain(args) -> int:
    BleakClient, BleakScanner = _require_bleak()

    if args.scan:
        return await cmd_scan(BleakScanner, args.scan_timeout)

    if args.state is None:
        return await demo_sequence(BleakClient, BleakScanner, args.addr,
                                   args.verbose, args.scan_timeout, args.name)

    return await demo_run(BleakClient, BleakScanner, args.addr, args.state, args.hold,
                          args.keep, args.verbose, args.scan_timeout, args.name)


def main() -> int:
    args = build_parser().parse_args()
    try:
        return asyncio.run(amain(args))
    except KeyboardInterrupt:
        print("\n[中断] 用户取消")
        return 130


if __name__ == "__main__":
    sys.exit(main())
