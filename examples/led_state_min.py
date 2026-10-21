# 状态灯最小测试：绿灯亮 1 秒 → 熄灭
# 依赖：pip install bleak
import asyncio
from bleak import BleakClient, BleakScanner

CHAR_LED = "0000ff01-0000-1000-8000-00805f9b34fb"
# 状态码：0=全灭 1=黄 2=绿 3=红 4=蓝


async def main():
    dev = await BleakScanner.find_device_by_name("VibeKey", timeout=8)
    if not dev:
        print("没找到 VibeKey，检查键盘是否已连到别的设备")
        return
    print("已找到:", dev.address)

    async with BleakClient(dev) as c:
        await c.write_gatt_char(CHAR_LED, b"\x02")   # 绿灯
        await asyncio.sleep(1)
        await c.write_gatt_char(CHAR_LED, b"\x00")   # 全灭

    print("完成")


asyncio.run(main())
