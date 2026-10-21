# LED 控制接口说明

> 适用固件：VibeKey-F3 `1.1.4` 及以上
> 涉及文件：`Firmware/src/ble_led_service.c`、`Firmware/src/dfu_device.c`、`Firmware/src/ws2812b.c`

本文描述**两个相对独立的接口**：

- **接口一：状态灯** —— 控制 4 颗独立 GPIO 指示灯，只下发「状态码」
- **接口二：RGB 底光预览** —— 控制 3 颗 WS2812B 按键底光，才真正下发 R/G/B 数值

两者**没有从属关系**，不共用同一条特征，也不共用同一份数据。

---

## 0. 一分钟速览

| | 接口一 · 状态灯 | 接口二 · RGB 底光 |
|---|---|---|
| 控制对象 | LED1~LED4（4 颗单色 GPIO） | C1/C2/C3 的 WS2812B（3 颗彩灯） |
| BLE 特征 | `0xFF01`（write） | `0xFF03`（write） |
| 数据量 | **1 字节** | **14 字节**（CONF 帧） |
| 是否含颜色数值 | 否，只有状态枚举 | 是，`r/g/b/bri` 全在里面 |
| 回执 | 1 字节状态码 | 2 字节 `[code, 实际生效亮度]` |
| 是否持久化 | 否（瞬态） | 否（预览；落盘需 `cmd=0x02`） |
| USB 通道 | `cmd=0x04` | `cmd=0x06`（同一帧结构） |

**一句话区分**：想让「黄/绿/红/蓝」这几颗状态灯亮，用接口一；想让「按键下面那颗彩色灯」显示任意颜色，用接口二。

---

## 1. 传输层

两个接口都同时支持 **BLE** 和 **USB CDC（虚拟串口）**，二者语义一致。

### 1.1 BLE GATT

服务 `0xFF00`，SIG base 展开后：

| 特征 | UUID | 属性 | 用途 |
|---|---|---|---|
| `0xFF01` | `0000ff01-0000-1000-8000-00805f9b34fb` | Write / Write Without Response | 接口一：状态灯 |
| ~~`0xFF02`~~ | — | — | **已于 2026-10-21 移除**（原为 IMU 诊断快照，生产不再暴露 IMU 原始数据） |
| `0xFF03` | `0000ff03-0000-1000-8000-00805f9b34fb` | Write / Write Without Response | 接口二：配置命令（含 RGB） |
| `0xFF04` | `0000ff04-0000-1000-8000-00805f9b34fb` | Notify | 回执通道（两接口共用） |

> 回执走 `0xFF04` 的 notify，**必须先订阅其 CCCD（`0x2902`）**，否则收不到任何响应。

### 1.2 USB CDC

虚拟串口，波特率对 CDC-ACM 无实际意义（写 115200 即可）。帧格式与 BLE 的 `0xFF03` 完全一致——固件里是同一条解析路径，只按来源区分回包路由。

### 1.3 CONF 帧通用结构

接口二使用完整 CONF 帧；接口一在 BLE 下**不使用帧**（裸 1 字节），在 USB 下才套帧。

```
偏移  长度  字段        说明
 0     4   magic      固定 0x434F4E46（小端写出 → 线上字节为 46 4E 4F 43）
 4     1   cmd        命令字
 5     1   slot       槽位 0~2（对应槽 1~3）；RGB 预览固定传 0
 6     2   data_len   payload 长度，小端 LE16
 8     N   payload    命令参数
```

命令字：

| cmd | 名称 | 说明 |
|---|---|---|
| `0x01` | READ | 读该槽完整配置（回 324 字节内存镜像） |
| `0x02` | WRITE | 写该槽配置并落盘（含每键 `rgb_*`） |
| `0x03` | RESET | 该槽恢复默认 |
| `0x04` | LED | **接口一的 USB 通道**，payload 1 字节状态 |
| `0x05` | INFO | 设备信息，回 `"VibeKey-F3\|<版本>\|<电量>\|<槽位>\n"` |
| `0x06` | RGB_APPLY | **接口二**，payload 6 字节实时预览 |

---

## 2. 接口一：状态灯

### 2.1 定义

写入一个**状态枚举**，固件据此点亮对应的**固定颜色**指示灯。**不含任何 RGB 数值**，颜色是写死的，改不了。

### 2.2 参数

| 值 | 名称 | 视觉效果 | 物理灯 |
|---|---|---|---|
| `0` | OFF | 全灭 | LED2/3/4 全关 |
| `1` | BUSY | 黄色常亮 | LED2 |
| `2` | DONE | 绿色常亮 | LED3 |
| `3` | ERROR | 红色常亮 | LED4 |
| `4` | BLUE | 蓝色常亮 | LED1（借用） |

**取值只有这 5 个。** 其他值会被固件白名单**静默丢弃**——不报错、不回执、不打日志（除非开 debug），排查时容易误判成"命令没发出去"。

### 2.3 回执

- **BLE**：`0xFF04` notify 回 **1 字节**
- **USB**：回 **8 字节**，`[0]` 为状态码（`0x00` 成功）

### 2.4 示例

> 可直接运行的完整版本：`examples/led_state_demo.py`
> （带设备扫描、按名自动查找、回执查看，用法见文件头部注释）

**BLE（Python / bleak）**

```python
import asyncio
from bleak import BleakClient

CHAR_LED = "0000ff01-0000-1000-8000-00805f9b34fb"

async def main(addr):
    async with BleakClient(addr) as c:
        await c.write_gatt_char(CHAR_LED, bytes([2]), response=False)  # 绿灯
        await asyncio.sleep(1)
        await c.write_gatt_char(CHAR_LED, bytes([0]), response=False)  # 全灭

asyncio.run(main("XX:XX:XX:XX:XX:XX"))
```

**USB（Python / pyserial）**

```python
import struct, serial

def led_frame(state: int) -> bytes:
    return struct.pack("<IBBH", 0x434F4E46, 0x04, 0, 1) + bytes([state])

with serial.Serial("COM5", 115200, timeout=2) as sp:
    sp.write(led_frame(2))      # 绿灯
    print("reply:", sp.read(8).hex(" "))
```

**手工验证（串口调试助手，HEX 发送）**

```
46 4E 4F 43 04 00 01 00 02      灯 = 绿
46 4E 4F 43 04 00 01 00 00      灯 = 全灭
```

### 2.5 行为约定

- **异步生效**：真正的 GPIO 操作在 50ms 软定时器里执行，写入后最多 50ms 才看到效果
- **断连即灭**：BLE 断开时固件主动熄灭 LED2/3/4（避免断连后还亮着绿/红灯浪费电）
- **不持久化**：重启后状态为 OFF，需上位机重新推送
- **LED1 有归属冲突**：LED1 本是充电指示 / 鼠标故障闪烁，`BLUE(4)` 是**借用**。退出蓝灯态时，只在"未充电且无故障闪烁"时才熄灭它

---

## 3. 接口二：RGB 底光预览

### 3.1 帧结构（14 字节）

```
46 4E 4F 43 | 06 | 00 | 06 00 | idx en r g b bri
└─ magic ─┘  cmd  slot  len    └── payload 6B ──┘
```

| 偏移 | 长度 | 字段 | 取值 | 说明 |
|---|---|---|---|---|
| 0 | 4 | magic | `0x434F4E46` | 固定 |
| 4 | 1 | cmd | `0x06` | RGB_APPLY |
| 5 | 1 | slot | `0` | 预览不区分槽位，固定 0 |
| 6 | 2 | data_len | `6` | LE16 |
| 8 | 1 | idx | `0~2` | 0=C1, 1=C2, 2=C3 |
| 9 | 1 | en | `0` / `1` | 0 = 熄灭（颜色值会被忽略） |
| 10 | 1 | r | `0~255` | 红分量 |
| 11 | 1 | g | `0~255` | 绿分量 |
| 12 | 1 | b | `0~255` | 蓝分量 |
| 13 | 1 | bri | `1~15` | 亮度百分比，见 3.4 |

### 3.2 回执（2 字节）

`0xFF04` notify 回：

| 字节 | 含义 |
|---|---|
| `[0]` | `0x00` = 已受理；`0x01` = `idx` 越界被拒 |
| `[1]` | **实际生效的亮度**（已被固件限幅后的值） |

> 回第 2 个字节是刻意设计：让上位机能区分"命令到了但亮度被压"和"命令根本没到"。只看 `[0]` 会以为一切正常。

### 3.3 示例

**BLE（Python / bleak）**

```python
import asyncio, struct
from bleak import BleakClient

CHAR_CMD = "0000ff03-0000-1000-8000-00805f9b34fb"
CHAR_RSP = "0000ff04-0000-1000-8000-00805f9b34fb"

def rgb_frame(idx, on, r, g, b, bri):
    payload = struct.pack("BBBBBB", idx, 1 if on else 0, r, g, b, bri)
    return struct.pack("<IBBH", 0x434F4E46, 0x06, 0, len(payload)) + payload

async def main(addr):
    async with BleakClient(addr) as c:
        loop = asyncio.get_running_loop()
        fut = loop.create_future()

        def on_rsp(_, data: bytearray):
            if not fut.done():
                fut.set_result(bytes(data))

        await c.start_notify(CHAR_RSP, on_rsp)          # 必须先订阅回执

        # C2 设为 RGB(32,160,255)，亮度 15
        await c.write_gatt_char(CHAR_CMD, rgb_frame(1, True, 0x20, 0xA0, 0xFF, 15),
                                response=False)
        code, eff = await asyncio.wait_for(fut, 2.0)
        print(f"code={code} 实际亮度={eff}")

        await c.write_gatt_char(CHAR_CMD, rgb_frame(1, False, 0, 0, 0, 0),
                                response=False)          # 熄灭

asyncio.run(main("XX:XX:XX:XX:XX:XX"))
```

**USB（Python / pyserial）**

```python
import struct, serial

def rgb_frame(idx, on, r, g, b, bri):
    payload = struct.pack("BBBBBB", idx, 1 if on else 0, r, g, b, bri)
    return struct.pack("<IBBH", 0x434F4E46, 0x06, 0, len(payload)) + payload

with serial.Serial("COM5", 115200, timeout=2) as sp:
    sp.write(rgb_frame(1, True, 0x20, 0xA0, 0xFF, 15))
    print("reply:", sp.read(2).hex(" "))
```

**Web（Web Serial）** —— 仓库已有实现，可直接参考 `web/vibekey-F3_web_config/js/19-led-rgb.js`

```js
const header = buildHeader(CMD_RGB, 6, 0);   // 0x06
const data = new Uint8Array(header.length + 6);
data.set(header, 0);
data.set([idx, on ? 1 : 0, r, g, b, bri], header.length);
await port.writable.getWriter().write(data);
```

**手工验证（串口调试助手，HEX 发送）**

```
46 4E 4F 43 06 00 06 00 01 01 20 A0 FF 0F    C2 点亮 RGB(32,160,255) 亮度15
46 4E 4F 43 06 00 06 00 01 00 00 00 00 00    C2 熄灭
```

> ⚠️ 前 4 字节必须是 `46 4E 4F 43`。写错的话固件会**静默丢帧**——既不回执也不打日志，表现就像"设备没收到"。

### 3.4 亮度限幅（重要）

固件把 C1/C2/C3 的亮度**硬锁在 15%**（`ws2812b.c: RGB_BRIGHTNESS_MAX_PCT = 15`），这是产品层面的功耗/亮度过高保护，**软件绕不过**。

- 传 `bri = 100` → 实际生效 15，回执 `[1]` 会告诉你 15
- 传 `bri = 0` → 全黑
- **不要在上位机重复缩放颜色值**。固件是把 `bri` 这个**参数**钳到 15 再乘颜色；如果上位机先按 100 缩放一遍、固件再钳一次，就成了双重降幅（15% → 38 → 6 ≈ 2.4%），肉眼全黑。已经踩过这个坑。

### 3.5 行为约定

- **仅供预览，不落盘**。要让配置持久化，走 `cmd=0x02 WRITE` 写整个配置区（`key_config_t` 里的 `rgb_enabled/rgb_r/rgb_g/rgb_b/rgb_brightness`）
- **断开即灭**：这是"看一眼"的临时效果，重新连接后不会恢复
- **与蓝牙槽位指示共用硬件**：那 3 颗 WS2812B 同时被 `bt_slot.c` 用于配对时的槽位闪烁（1~3 槽各一颗灯），两者会互相覆盖
- **按键按下优先**：如果配置里 `rgb_enabled=1`，物理按键按下时固件会按存好的颜色点亮、松开即灭，可能与你正在预览的颜色打架

---

## 4. 完整调用流程

```
上位机                                  固件
  │                                      │
  ├─ 订阅 0xFF04 notify（CCCD）─────────▶│  必须，否则收不到回执
  │                                      │
  ├─ write 0xFF03 : 14B RGB 帧──────────▶│ GATT 回调线程
  │                                      │   └ conf_ble_submit() 拷一份投队列
  │                                      │        ↓ conf_mq
  │                                      │   conf_task 线程解析 cmd=0x06
  │                                      │        ├ 校验 idx < 3
  │                                      │        ├ conf_send_reply([0x00, eff_bri])
  │◀── notify 0xFF04 : 2B [00, eff]─────┤        └ 投 rgb_mq
  │                                      │              ↓
  │                                      │        rgb_apply_task 线程
  │                                      │          bri 限幅 → 乘颜色
  │                                      │          rgb_led_show(idx, 0xRRGGBB)
  │                                      │              ↓
  │                                      │          WS2812B 出灯
```

**为什么绕这么多线程**：GATT 回调跑在协议栈上下文，里面既不能做 flash 写，也不能驱动 WS2812B（驱动内部要取互斥量，在此上下文取会直接断言）。所以一律"拷贝 + 投队列"，真正干活放在普通线程。

---

## 5. 常见坑

| 现象 | 原因 | 排查 |
|---|---|---|
| 写成功但灯不亮 | `bri` 传了 0；或 `en=0` | 看回执第 2 字节是否为 0 |
| 灯很暗几乎看不见 | 上位机自己又缩放了一遍颜色 | 见 3.4，只传原始 r/g/b + bri |
| 完全没反应，无回执 | 没订阅 `0xFF04` 的 CCCD | 先 `start_notify` 再写 |
| 完全没反应，有回执 0x01 | `idx` 越界（≥3） | idx 只能 0~2 |
| 状态灯命令像"没发出去" | 状态码不在 0~4 白名单 | 抓 `[LED SVC] state=` 日志 |
| 灯亮一下就灭 | 预览与按键底光/槽位闪烁抢同一颗灯 | 见 3.5 |
| 断连后灯还亮着 | 状态灯会主动灭；RGB 不会 | 断连前自己发一帧 `en=0` |
| 帧发出去没动静 | magic 写错（应为线上 `46 4E 4F 43`） | 核对前 4 字节 |

**调试钩子**：固件在 RGB 路径上打了成对日志，`grep "\[RGB\]"` 即可定位卡在哪一步——

```
[RGB] APPLY len=6 idx=1 en=1 rgb=32,160,255 bri=15   ← 命令真的到了
[RGB] queued idx=1 eff_bri=15 (mq ok)                ← 已入队
[RGB] show idx=1 en=1 bri=15 -> 0x041826             ← 真正出灯（32,160,255 × 15%）
```

只有 `APPLY` 没有 `show` → 队列消费线程没跑起来。

---

## 6. 写接口文档的建议结构

写这类硬件接口文档，建议固定这六段，**缺任何一段都会导致对接方反复来问**：

1. **一句话定位** —— 它控制什么、**不**控制什么（本文最关键的一句就是"状态灯不含 RGB 数值"）
2. **传输层** —— 走哪个特征/哪条通道、UUID、要不要订阅
3. **数据契约** —— 字节偏移表 + 每个字段的**取值范围**
4. **边界行为** —— 非法值怎么办（报错 / 静默丢弃 / 钳位）—— 这一段最容易漏，也最容易引发联调事故
5. **回执语义** —— 成功怎么判、失败怎么判、异步还是同步
6. **可复制示例** —— 从最短能跑通的一段开始，再加完整流程

另外两个实践：

- **示例要给"手工验证"版本**（HEX 串），这样对方可以用串口助手 30 秒验证硬件通不通，不用先写代码
- **回执里尽量带上"实际生效值"**，而不是只回成功/失败。本文的 `[0x00, eff_bri]` 就是这么设计的——它把"亮度被限幅"这个隐形事实暴露给了上位机
