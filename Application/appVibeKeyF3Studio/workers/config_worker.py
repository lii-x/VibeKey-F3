import serial
import struct
import sys
import time
import json
import os

# ⚠️ 08-27: 同 ota_worker —— Windows QProcess 管道下默认 GBK 输出, Qt fromUtf8 乱码; 强制 UTF-8。
try:
    sys.stdout.reconfigure(encoding='utf-8')
    # 10-04: stderr 也要设! C++ 侧用 QString::fromUtf8() 读 stderr，
    # 不设则中文日志输出成 "??" 完全不可读(实测: BLE 服务 denied 的原因说明全变问号)
    sys.stderr.reconfigure(encoding='utf-8')
except AttributeError:
    pass

CONF_MAGIC = 0x434F4E46
KEYC_MAGIC = 0x4B455943
CONF_CMD_READ = 0x01
CONF_CMD_WRITE = 0x02
CONF_CMD_RESET = 0x03
CONF_CMD_LED   = 0x04
CONF_CMD_INFO  = 0x05   # 设备信息查询(固件 dfu_device.c; BLE 通道靠它取当前激活槽)
CONF_CMD_RGB_APPLY = 0x06   # 单键 RGB 实时预览（不落盘）

KEY_ACTION_NAMES = {0: "none", 1: "keyboard", 2: "mouse", 3: "multimedia", 4: "airmouse"}
KEY_ACTION_IDS = {"none": 0, "keyboard": 1, "mouse": 2, "multimedia": 3, "airmouse": 4}
KEY_NAMES = {0: "KEY3", 1: "KEY4", 2: "KEY5"}

AIR_MOUSE_MODE_NAMES = {0: "toggle", 1: "hold"}
AIR_MOUSE_MODE_IDS = {"toggle": 0, "hold": 1}
AIR_MOUSE_SPEED_NAMES = {0: "slow", 1: "medium", 2: "fast"}
AIR_MOUSE_SPEED_IDS = {"slow": 0, "medium": 1, "fast": 2}

# 摇一摇(09-03): 灵敏度档位(与固件 shake_sens_t 一致)
SHAKE_SENS_NAMES = {0: "light", 1: "medium", 2: "strong"}
SHAKE_SENS_IDS = {"light": 0, "medium": 1, "strong": 2}
SHAKE_SENS_DEFAULT = 1   # medium, 与固件 KEY_CONFIG_SHAKE_SENS_DEFAULT 一致
SHAKE_OFF, SHAKE_ON = 0, 1
# 新固件 READ 回包长度 = sizeof(key_config_storage_t); 旧固件 = 40, 摇一摇版 = 52,
# 09-05 L2/L3 版 = 68, L1 版 = 80, EC 编码器版 = 104(尾部追加 ec_cw@77..85
# ec_press@86..94 ec_ccw@95..103, 结构体内存布局)。
# 新固件 WRITE payload(紧凑布局) = 2+27+2+1+1+1+9+45 = 97B(尾部 45B =
# L2/L3/L1/EC 三键 各 9B; 旧固件忽略多余尾部, 旧上位机短 payload 固件保留现值)。
# 09-07 L 键模式+三手势: payload 184->269; READ 回包 192->276B(帧 277B)。
# 09-07 EC 按下模式+三手势: payload 269->297; READ 回包 276->304B(帧 305B)。
# 10-04 EC 按压滚动(手势模式新增): payload 297->315; READ 回包 304->324B(帧 323B)。
#   结构体尾部追加 ec_cw_press@304..312 ec_ccw_press@313..321(304+18=322, 补 2B
#   对齐 -> sizeof 324)。
#   10-05 【固件已同步实现】: key_config_storage_t 324B + dfu_device.c 尾部解析
#   (阈值 +18)+ main.c 手势模式"按住旋钮旋转"= 按压滚动动作并清零单击/双击/长按。
#   上下位机布局完全一致, 无需再改。
#   兼容性: 旧固件 READ 仍回 304B、WRITE 按阈值 data_len>= 逐段判定, 多出的 18B
#   尾部被忽略(不污染 flash) -> 新字段读回"缺失" -> 回落默认(滚轮上/下);
#   旧上位机短 payload 写入时固件保留按压滚动现值(双向兼容)。
CFG_READ_LEN_NEW = 324   # 10-04: 追加 EC 按压滚动两键, sizeof 304->324
CFG_READ_LEN_OLD = 40
CFG_WRITE_LEN_NEW = 315   # 10-04: payload 297->315(+2x9B EC 按压滚动上/下)

# L2/L3 侧键(09-05): 鼠标动作的合法按键位(1=左 2=右 4=中), 与固件 key_config_lkey_valid 一致
LKEY_MOUSE_KEYCODES = (1, 2, 4, 5, 6)   # 09-06: 5/6=滚轮上/下(L 键与 EC 统一)
# C 键三手势(09-07): 双击/长按的 MOUSE 合法键码(与固件 key_config_ckey_valid 一致)。
# 单击(keys[] 本体)沿用历史语义, 三手势均允许 1..6(左/右/中 + 滚轮上/下)。
CKEY_MOUSE_KEYCODES = (1, 2, 4, 5, 6)
# L 键三手势(09-07): MOUSE 合法键码 1..6(连续区间, 与固件 key_config_lgesture_valid
# 的 `keycode >= 0x01 && keycode <= 0x06` 完全一致; L1 额外允许 AIRMOUSE)。
LGEST_MOUSE_KEYCODES = (1, 2, 3, 4, 5, 6)
# EC 编码器(09-06): MOUSE 伪键码 1/2/4=左/右/中键, 5/6=滚轮上/下 ——
# 三手势统一允许(按下手势=每次按下滚一格, 固件 key_config_eckey_valid 同规则)。
# 10-04: EC 按压滚动上/下(手势模式)沿用旋转键同集(旋转语义 -> 允许滚轮)。
EC_MOUSE_KEYCODES_ROT = (1, 2, 4, 5, 6)
EC_MOUSE_KEYCODES_PRESS = (1, 2, 4, 5, 6)

LOG_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), "config_debug.log")

def debug_log(msg):
    try:
        with open(LOG_FILE, "a") as f:
            f.write(f"{time.strftime('%H:%M:%S')} {msg}\n")
    except Exception:
        pass

def open_port(port):
    debug_log(f"Opening port {port}")
    ser = serial.Serial(port, 115200, timeout=1)
    ser.reset_input_buffer()
    ser.reset_output_buffer()
    time.sleep(0.5)
    while ser.in_waiting > 0:
        ser.read(ser.in_waiting)
        time.sleep(0.1)
    debug_log(f"Port cleaned, in_waiting={ser.in_waiting}")
    return ser

def send_and_receive(ser, data, expect_len=16, timeout=1.0):
    """发送并循环读取, 直到收满 expect_len 字节或超时。
    ⚠️ 08-27: 原实现 ser.read(expect_len) 只读一次 —— 回包(如 40B 的键配置)在
    CDC 下分片到达时只读回一部分(如 35B), 尾部字段(sleep_min)解析缺失回落默认值
    (表现: 写 60 分钟读回 45)。循环补读保证完整。
    09-07: USB FS 批量端点 MPS=64, OUT 传输必须以短包/ZLP 收尾。帧长恰为 64 的
    整数倍(如 192B = 3x64 的键配置 WRITE)时, Windows usbser 不补 ZLP, 设备侧读
    永不完成 -> 无 ACK -> "Device rejected config" 且管道卡死(后续命令收到残留帧)。
    物理补 1 字节垫尾使末包变短包; 设备按帧头 data_len 解析、忽略垫尾字节。"""
    if len(data) > 0 and len(data) % 64 == 0:
        data = data + b"\x00"
        debug_log(f"MPS-64 exact-multiple frame: appended 1 pad byte (now {len(data)})")
    debug_log(f"Sending {len(data)} bytes: {data.hex()}")
    ser.write(data)
    ser.flush()
    deadline = time.monotonic() + timeout
    resp = b""
    while len(resp) < expect_len:
        if time.monotonic() > deadline:
            break
        chunk = ser.read(expect_len - len(resp))
        if chunk:
            resp += chunk
            deadline = time.monotonic() + timeout   # 有数据就续期
        else:
            time.sleep(0.02)
    debug_log(f"Read {len(resp)} bytes: {resp.hex()}")
    return resp

def build_conf_frame(cmd, slot, payload=b""):
    """组一条 CONF 命令帧 —— 与固件线上格式一致(USB CDC 与 BLE 0xFF03 共用):
    [4B magic][1B cmd][1B slot][2B data_len][payload]。slot 0~2=槽1~3。"""
    return struct.pack("<IBBH", CONF_MAGIC, cmd, slot & 0xFF, len(payload)) + payload


def build_write_payload(config):
    """把 config dict 组 315B 紧凑 WRITE payload(校验规则与固件 dfu_device.c 完全一致):
    mode@0 speed@1 keys@2..28 sleep@29..30 dir@31 en@32 sens@33 shake_key@34..42
    l2_key@43..51 l3_key@52..60 l1_key@61..69 ec_cw@70..78 ec_press@79..87 ec_ccw@88..96
    c1_dbl..c3_lng@97..150(6x9B) reserved_tail 占位@151..152 c_mode@153..155
    @163 占位@156 c1_tap..c3_tap@157..183(3x9B) reserved_tap_pad@184 占位
    l_mode@185..187 l1_dbl..l3_lng@188..241(6x9B) l1_tap..l3_tap@242..268(3x9B)
    ec_press_mode@269 ec_press_dbl@270..278 lng@279..287 tap@288..296(3x9B)
    ec_cw_press@297..305 ec_ccw_press@306..314(10-04 EC 按压滚动, 各 9B)。
    ⚠️ 语义(与固件对齐): JSON 缺失/非法字段 -> 0xFF = 设备保持现值(绝不污染 flash);
    只有显式 action=none 才算"清除快捷键"。keys 必须正好 3 个, 否则抛 ValueError。"""
    keys = config.get("keys", [])
    if len(keys) != 3:
        raise ValueError("Config must have exactly 3 keys")

    mode = AIR_MOUSE_MODE_IDS.get(config.get("air_mouse_mode", "toggle"), 0)
    speed = AIR_MOUSE_SPEED_IDS.get(config.get("air_mouse_speed", "medium"), 1)
    payload = struct.pack("BB", mode, speed)

    for k in keys:
        action_id = KEY_ACTION_IDS.get(k.get("action", "none"), 0)
        modifier = k.get("modifier", 0)
        keycode = k.get("keycode", 0)
        rgb_en = int(k.get("rgb_enabled", 1))
        color = int(k.get("rgb_color", 0xFFFFFF))
        r = (color >> 16) & 0xFF
        g = (color >> 8) & 0xFF
        b = color & 0xFF
        bri = int(k.get("rgb_brightness", 100))
        payload += struct.pack("BBBBBBBBB", action_id, modifier, keycode, 0,
                               rgb_en, r, g, b, bri)

    # 08-27: 休眠时间(分钟)追加在 payload 尾部(LE16)。0=永不; 45~240 合法。
    sleep_min = int(config.get("sleep_min", 45))
    if sleep_min <= 0:
        sleep_min = 0
    elif sleep_min < 45:
        sleep_min = 45
    payload += struct.pack("<H", sleep_min)

    # 09-01: 鼠标方向(1B)在 sleep_min 后。0~3 合法; 缺失/非法 -> 0xFF=设备保持现值。
    air_dir = config.get("air_mouse_dir", 0xFF)
    try:
        air_dir = int(air_dir)
    except Exception:
        air_dir = 0xFF
    if air_dir < 0 or air_dir > 3:
        air_dir = 0xFF
    payload += struct.pack("B", air_dir)

    # 09-03: 摇一摇 en(1B) sens(1B) shake_key(9B) 依次在 dir 后。
    en = config.get("shake_enabled", 0xFF)
    try:
        en = int(en)
    except Exception:
        en = 0xFF
    if en not in (SHAKE_OFF, SHAKE_ON):
        en = 0xFF
    payload += struct.pack("B", en)

    sens = config.get("shake_sens", 0xFF)
    try:
        sens = int(sens)
    except Exception:
        sens = 0xFF
    if sens not in SHAKE_SENS_NAMES:
        sens = 0xFF
    payload += struct.pack("B", sens)

    # shake_key: action 只认 none/keyboard/multimedia(摇一摇不支持鼠标动作)。
    sk = config.get("shake_key")
    sk_action = KEY_ACTION_IDS.get(sk.get("action", "none"), 0) if isinstance(sk, dict) else -1
    if sk_action in (KEY_ACTION_IDS["none"], KEY_ACTION_IDS["keyboard"], KEY_ACTION_IDS["multimedia"]):
        sk_mod = int(sk.get("modifier", 0)) & 0xFF
        sk_kc = int(sk.get("keycode", 0)) & 0xFF
        # 键盘组合键至少要有修饰键或主键之一; 空组合=显式清除
        payload += struct.pack("BBBBBBBBB", sk_action, sk_mod, sk_kc,
                               0, 0, 0, 0, 0, 0)
    else:
        payload += struct.pack("BBBBBBBBB", 0xFF, 0xFF, 0xFF,
                               0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF)

    # 09-05: L2/L3 侧键快捷键, 依次追加在 shake_key 后(各 9B)。
    # 与 shake_key 不同: 支持"鼠标键"动作(action=mouse, keycode 1/2/4, 固件按住保持),
    # 这是 L2/L3 的历史默认行为(L2=左键 L3=右键)。缺失/非法 -> 9×0xFF=设备保持现值。
    # 09-05: L1 同样追加(默认动作 airmouse=空中鼠标开关, 仅 L1 允许该动作)。
    for name in ("l2_key", "l3_key", "l1_key"):
        lk = config.get(name)
        lk_action = KEY_ACTION_IDS.get(lk.get("action", "none"), -1) if isinstance(lk, dict) else -1
        valid = False
        mod = kc = 0
        if lk_action == KEY_ACTION_IDS["airmouse"]:
            # 仅 L1: 空中鼠标开关(固件 l1key_valid 校验; L2/L3 的该值会被拒绝)
            valid = (name == "l1_key")
        elif lk_action == KEY_ACTION_IDS["mouse"]:
            try:
                kc = int(lk.get("keycode", 0))
            except Exception:
                kc = 0
            valid = kc in LKEY_MOUSE_KEYCODES
        elif lk_action in (KEY_ACTION_IDS["keyboard"], KEY_ACTION_IDS["multimedia"]):
            try:
                mod = int(lk.get("modifier", 0)) & 0xFF
                kc = int(lk.get("keycode", 0)) & 0xFF
            except Exception:
                mod = kc = 0
            valid = True
        elif lk_action == KEY_ACTION_IDS["none"]:
            valid = True   # 显式禁用该键
        if valid:
            payload += struct.pack("BBBBBBBBB", lk_action, mod, kc, 0, 0, 0, 0, 0, 0)
        else:
            payload += struct.pack("BBBBBBBBB", 0xFF, 0xFF, 0xFF,
                                   0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF)

    # 09-05: EC 编码器三手势(旋转 cw / 按下 press / 旋转 ccw)。MOUSE 动作键码:
    # 旋转键 1/2/4(左/右/中) + 5/6(滚轮上/下); 按下键仅 1/2/4。
    # 默认: cw=滚轮上(5) press=中键(4) ccw=滚轮下(6)。缺失/非法 -> 9×0xFF=保持现值。
    for name, kcs in (("ec_cw_key", EC_MOUSE_KEYCODES_ROT),
                      ("ec_press_key", EC_MOUSE_KEYCODES_PRESS),
                      ("ec_ccw_key", EC_MOUSE_KEYCODES_ROT)):
        lk = config.get(name)
        lk_action = KEY_ACTION_IDS.get(lk.get("action", "none"), -1) if isinstance(lk, dict) else -1
        valid = False
        mod = kc = 0
        if lk_action == KEY_ACTION_IDS["airmouse"]:
            # 09-06: EC 三手势允许"空中鼠标"动作(激活行为同 L1, 遵循其模式/灵敏度)
            valid = True
        elif lk_action == KEY_ACTION_IDS["mouse"]:
            try:
                kc = int(lk.get("keycode", 0))
            except Exception:
                kc = 0
            valid = kc in kcs
        elif lk_action in (KEY_ACTION_IDS["keyboard"], KEY_ACTION_IDS["multimedia"]):
            try:
                mod = int(lk.get("modifier", 0)) & 0xFF
                kc = int(lk.get("keycode", 0)) & 0xFF
            except Exception:
                mod = kc = 0
            valid = True
        elif lk_action == KEY_ACTION_IDS["none"]:
            valid = True
        if valid:
            payload += struct.pack("BBBBBBBBB", lk_action, mod, kc, 0, 0, 0, 0, 0, 0)
        else:
            payload += struct.pack("BBBBBBBBB", 0xFF, 0xFF, 0xFF,
                                   0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF)

    # 09-07: C 键三手势(双击/长按) —— 追加在 EC 之后, 每键两块共 6 块:
    #   payload[97..105]=c1_dbl [106..114]=c1_lng [115..123]=c2_dbl
    #   [124..132]=c2_lng [133..141]=c3_dbl [142..150]=c3_lng (payload 索引+8=buf 索引)
    # 合法动作(与固件 key_config_ckey_valid 一致): NONE/键盘/多媒体 + MOUSE 键码 1..6。
    # 显式 NONE = 清除该手势; 缺失/非法 -> 9x0xFF = 设备保持现值。
    keys_cfg = config.get("keys", [])
    for ki in range(3):
        for gname in ("dbl", "lng"):
            k = keys_cfg[ki] if ki < len(keys_cfg) and isinstance(keys_cfg[ki], dict) else {}
            act_str = k.get(gname + "_action", "none")
            lk_action = KEY_ACTION_IDS.get(act_str, -1)
            valid = False
            mod = kc = 0
            if lk_action == KEY_ACTION_IDS["mouse"]:
                try:
                    kc = int(k.get(gname + "_keycode", 0))
                except Exception:
                    kc = 0
                valid = kc in CKEY_MOUSE_KEYCODES
            elif lk_action in (KEY_ACTION_IDS["keyboard"], KEY_ACTION_IDS["multimedia"]):
                try:
                    mod = int(k.get(gname + "_modifier", 0)) & 0xFF
                    kc = int(k.get(gname + "_keycode", 0)) & 0xFF
                except Exception:
                    mod = kc = 0
                valid = True
            elif lk_action == KEY_ACTION_IDS["none"]:
                valid = True   # 显式清除该手势
            if valid:
                payload += struct.pack("BBBBBBBBB", lk_action, mod, kc, 0, 0, 0, 0, 0, 0)
            else:
                payload += struct.pack("BBBBBBBBB", 0xFF, 0xFF, 0xFF,
                                       0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF)

    # 09-07: C 键显式模式 —— 追加在 C 手势块之后。紧凑 payload 无结构体的
    # reserved_tail@158..159(占位 2B 0xFF, 设备忽略), 模式字节在其后:
    #   payload[153..155] = c1..c3_mode(buf[161..163], 结构体 offset 160..162)。
    # 语义(与固件 key_config_get_c_mode 一致): 0=常规(直通) 1=手势。
    # 缺失/非法 -> 0xFF = 设备保持现值(绝不污染 flash)。
    payload += b"\xff\xff"   # reserved_tail 占位(设备端忽略)
    for ki in range(3):
        k = keys_cfg[ki] if ki < len(keys_cfg) and isinstance(keys_cfg[ki], dict) else {}
        m = k.get("c_mode", 0xFF)
        try:
            m = int(m)
        except Exception:
            m = 0xFF
        if m not in (0, 1):
            m = 0xFF
        payload += struct.pack("B", m)

    # 09-07: 手势"单击"独立块 —— 常规(keys[])之外单独存储, 避免改单击影响常规键。
    # 结构体 reserved_mode_pad@163 在紧凑 payload 无对应字段(占位 1B 0xFF), tap 块
    # 在其后: payload[157..165]=c1_tap [166..174]=c2_tap [175..183]=c3_tap。
    # 合法性同双击/长按(NONE/键盘/多媒体 + MOUSE 键码 1..6); 显式 NONE=清除。
    payload += b"\xff"       # reserved_mode_pad@163 占位(设备端忽略)
    for ki in range(3):
        k = keys_cfg[ki] if ki < len(keys_cfg) and isinstance(keys_cfg[ki], dict) else {}
        act_str = k.get("tap_action", "none")
        lk_action = KEY_ACTION_IDS.get(act_str, -1)
        valid = False
        mod = kc = 0
        if lk_action == KEY_ACTION_IDS["mouse"]:
            try:
                kc = int(k.get("tap_keycode", 0))
            except Exception:
                kc = 0
            valid = kc in CKEY_MOUSE_KEYCODES
        elif lk_action in (KEY_ACTION_IDS["keyboard"], KEY_ACTION_IDS["multimedia"]):
            try:
                mod = int(k.get("tap_modifier", 0)) & 0xFF
                kc = int(k.get("tap_keycode", 0)) & 0xFF
            except Exception:
                mod = kc = 0
            valid = True
        elif lk_action == KEY_ACTION_IDS["none"]:
            valid = True   # 显式清除该手势
        if valid:
            payload += struct.pack("BBBBBBBBB", lk_action, mod, kc, 0, 0, 0, 0, 0, 0)
        else:
            payload += struct.pack("BBBBBBBBB", 0xFF, 0xFF, 0xFF,
                                   0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF)

    # 09-07: L1/L2/L3 显式模式 + 三手势 —— 追加在 C 单击块之后。紧凑 payload 与
    # 结构体【尾区】相差 7B(payload 索引+7=结构体偏移), 故:
    #   payload[184]=reserved_tap_pad@191 占位(设备忽略)
    #   payload[185..187]=l1/l2/l3_mode(buf[193..195], 结构体 @192..194)
    #   payload[188..241]=l1_dbl..l3_lng 六块(buf[196..249], 结构体 @195..248)
    #   payload[242..268]=l1_tap..l3_tap 三块(buf[250..276], 结构体 @249..275)
    # payload 总长 184 -> 269(帧 277B); BLE MTU-3=297 覆盖。旧固件(data_len 不足)
    # 不进入分支, L 模式/手势保持现值(双向兼容, 与 C 尾块策略一致)。
    # 模式语义与 C 键一致(0=常规 1=手势); 缺失/非法 -> 0xFF=设备保持现值。
    payload += b"\xff"   # reserved_tap_pad@191 占位(设备端忽略)
    for name in ("l1_key", "l2_key", "l3_key"):
        lk = config.get(name)
        m = lk.get("l_mode", 0xFF) if isinstance(lk, dict) else 0xFF
        try:
            m = int(m)
        except Exception:
            m = 0xFF
        if m not in (0, 1):
            m = 0xFF
        payload += struct.pack("B", m)

    # L 三手势块(双击/长按), 顺序 l1_dbl,l1_lng,l2_dbl,l2_lng,l3_dbl,l3_lng ——
    # 块布局同 C 键手势(各 9B)。合法性(与固件 key_config_lgesture_valid 一致):
    # NONE/键盘/多媒体 + MOUSE 键码 1..6; AIRMOUSE 仅 L1(li==0, 用户要求)。
    # 显式 NONE = 清除该手势; 缺失/非法 -> 9x0xFF = 设备保持现值。
    for li, name in enumerate(("l1_key", "l2_key", "l3_key")):
        lk = config.get(name)
        allow_am = (li == 0)   # L1 允许空中鼠标
        for gname in ("dbl", "lng"):
            k = lk if isinstance(lk, dict) else {}
            lk_action = KEY_ACTION_IDS.get(k.get(gname + "_action", "none"), -1)
            valid = False
            mod = kc = 0
            if lk_action == KEY_ACTION_IDS["airmouse"]:
                valid = allow_am
            elif lk_action == KEY_ACTION_IDS["mouse"]:
                try:
                    kc = int(k.get(gname + "_keycode", 0))
                except Exception:
                    kc = 0
                valid = kc in LGEST_MOUSE_KEYCODES
            elif lk_action in (KEY_ACTION_IDS["keyboard"], KEY_ACTION_IDS["multimedia"]):
                try:
                    mod = int(k.get(gname + "_modifier", 0)) & 0xFF
                    kc = int(k.get(gname + "_keycode", 0)) & 0xFF
                except Exception:
                    mod = kc = 0
                valid = True
            elif lk_action == KEY_ACTION_IDS["none"]:
                valid = True   # 显式清除该手势
            if valid:
                payload += struct.pack("BBBBBBBBB", lk_action, mod, kc, 0, 0, 0, 0, 0, 0)
            else:
                payload += struct.pack("BBBBBBBBB", 0xFF, 0xFF, 0xFF,
                                       0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF)

    # L 三手势"单击"独立块(每键一块, 独立于 l*_key 常规键, 与 C 键同语义)。
    # 布局/合法性同上; 字段用 tap_ 前缀。
    for li, name in enumerate(("l1_key", "l2_key", "l3_key")):
        lk = config.get(name)
        allow_am = (li == 0)
        k = lk if isinstance(lk, dict) else {}
        lk_action = KEY_ACTION_IDS.get(k.get("tap_action", "none"), -1)
        valid = False
        mod = kc = 0
        if lk_action == KEY_ACTION_IDS["airmouse"]:
            valid = allow_am
        elif lk_action == KEY_ACTION_IDS["mouse"]:
            try:
                kc = int(k.get("tap_keycode", 0))
            except Exception:
                kc = 0
            valid = kc in LGEST_MOUSE_KEYCODES
        elif lk_action in (KEY_ACTION_IDS["keyboard"], KEY_ACTION_IDS["multimedia"]):
            try:
                mod = int(k.get("tap_modifier", 0)) & 0xFF
                kc = int(k.get("tap_keycode", 0)) & 0xFF
            except Exception:
                mod = kc = 0
            valid = True
        elif lk_action == KEY_ACTION_IDS["none"]:
            valid = True   # 显式清除该手势
        if valid:
            payload += struct.pack("BBBBBBBBB", lk_action, mod, kc, 0, 0, 0, 0, 0, 0)
        else:
            payload += struct.pack("BBBBBBBBB", 0xFF, 0xFF, 0xFF,
                                   0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF)

    # 09-07: EC 按下 显式模式 + 三手势 —— 追加在 L 单击块之后(payload 尾部)。
    #   payload[269]=ec_press_mode(buf[277], 结构体 @276)
    #   payload[270..278]=ec_press_dbl [279..287]=ec_press_lng [288..296]=ec_press_tap
    #   (payload 索引 +8=buf 索引, +7=结构体偏移)
    # payload 总长 269 -> 297(帧 305B); BLE MTU-3=337 覆盖(proj.conf MTU=340)。
    # 旧固件(data_len 不足)不进入分支, EC 模式/手势保持现值(双向兼容)。
    # 模式语义与 C/L 键一致(0=常规 1=手势); 缺失/非法 -> 0xFF=设备保持现值。
    ep = config.get("ec_press_key")
    m = ep.get("ec_mode", 0xFF) if isinstance(ep, dict) else 0xFF
    try:
        m = int(m)
    except Exception:
        m = 0xFF
    if m not in (0, 1):
        m = 0xFF
    payload += struct.pack("B", m)

    # EC 按下三手势块 —— 单击/双击/长按顺序 tap/dbl/lng(与 C/L 前缀语义一致,
    # 各 9B)。合法性(与固件 key_config_ecpressgesture_valid = eckey_valid 一致):
    # NONE/键盘/多媒体 + MOUSE 键码 1..6 + AIRMOUSE(EC 常规键本就允许,
    # 空中鼠标每触发切换一次)。显式 NONE=清除; 缺失/非法 -> 9x0xFF=保持现值。
    for gname in ("dbl", "lng", "tap"):
        k = ep if isinstance(ep, dict) else {}
        lk_action = KEY_ACTION_IDS.get(k.get(gname + "_action", "none"), -1)
        valid = False
        mod = kc = 0
        if lk_action == KEY_ACTION_IDS["airmouse"]:
            valid = True
        elif lk_action == KEY_ACTION_IDS["mouse"]:
            try:
                kc = int(k.get(gname + "_keycode", 0))
            except Exception:
                kc = 0
            valid = kc in EC_MOUSE_KEYCODES_ROT
        elif lk_action in (KEY_ACTION_IDS["keyboard"], KEY_ACTION_IDS["multimedia"]):
            try:
                mod = int(k.get(gname + "_modifier", 0)) & 0xFF
                kc = int(k.get(gname + "_keycode", 0)) & 0xFF
            except Exception:
                mod = kc = 0
            valid = True
        elif lk_action == KEY_ACTION_IDS["none"]:
            valid = True   # 显式清除该手势
        if valid:
            payload += struct.pack("BBBBBBBBB", lk_action, mod, kc, 0, 0, 0, 0, 0, 0)
        else:
            payload += struct.pack("BBBBBBBBB", 0xFF, 0xFF, 0xFF,
                                   0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF)

    # 10-04: EC 按压滚动(手势模式新增) —— 上/下各一块(9B), 追加在 EC 按下手势之后。
    #   payload[297..305]=ec_cw_press(结构体 @304) [306..314]=ec_ccw_press(@313)。
    # 合法性: 与 EC 旋转键同集(eckey_valid rotation=1) —— NONE/键盘/多媒体 +
    #   MOUSE 键码 1..6 + AIRMOUSE(按压滚动本质仍是"旋转", 允许滚轮)。
    #   字段【整体缺失】-> 9x0xFF = 设备保持现值(不覆盖); 显式 action=none = 清除。
    # ⚠️ 旧固件按 data_len 阈值逐段判定, 不进入本段 -> 多出的 18B 尾部被忽略,
    #   不污染 flash; 固件补齐同布局后即生效。
    for pname in ("ec_cw_press_key", "ec_ccw_press_key"):
        raw = config.get(pname)
        if not isinstance(raw, dict) or "action" not in raw:
            payload += b"\xff" * 9        # 字段缺失 -> 设备保持现值
            continue
        pk_action = KEY_ACTION_IDS.get(raw.get("action", "none"), -1)
        valid = False
        mod = kc = 0
        if pk_action == KEY_ACTION_IDS["airmouse"]:
            valid = True
        elif pk_action == KEY_ACTION_IDS["mouse"]:
            try:
                kc = int(raw.get("keycode", 0))
            except Exception:
                kc = 0
            valid = kc in EC_MOUSE_KEYCODES_ROT
        elif pk_action in (KEY_ACTION_IDS["keyboard"], KEY_ACTION_IDS["multimedia"]):
            try:
                mod = int(raw.get("modifier", 0)) & 0xFF
                kc = int(raw.get("keycode", 0)) & 0xFF
            except Exception:
                mod = kc = 0
            valid = True
        elif pk_action == KEY_ACTION_IDS["none"]:
            valid = True   # 显式清除按压滚动
        if valid:
            payload += struct.pack("BBBBBBBBB", pk_action, mod, kc, 0, 0, 0, 0, 0, 0)
        else:
            payload += struct.pack("BBBBBBBBB", 0xFF, 0xFF, 0xFF,
                                   0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF)
    return payload


def parse_read_response(resp):
    """解析 READ 回包(52B 新固件内存镜像 / 40B 旧固件兼容) → dict。
    校验失败(magic 错/长度不足)抛 ValueError, 由调用方按各自通道报错。"""
    if len(resp) < 16:
        raise ValueError(f"No response (got {len(resp)} bytes)")

    # 09-09 bugfix: 原变量名 mode/speed 会被后面 C 键模式循环与 L 键模式循环
    # 【同名遮蔽】—— 到函数尾部组返回 dict 时 mode 已变成 L3 的 l_mode(0/1),
    # 导致 air_mouse_mode 永远随 L3 模式漂移(L3=手势 -> UI 恒显"按住移动")。
    # 改名 am_mode/am_speed 与键模式变量彻底隔离。
    magic, ver, num_keys, am_mode, am_speed = struct.unpack("<IBBBB", resp[:8])
    if magic != KEYC_MAGIC:
        raise ValueError("Invalid response magic")

    keys = []
    for i in range(num_keys):
        off = 8 + i * 9
        action, modifier, keycode, reserved, rgb_en, r, g, b, bri = struct.unpack(
            "BBBBBBBBB", resp[off:off + 9])
        keys.append({
            "id": i,
            "name": KEY_NAMES.get(i, f"KEY{i}"),
            "action": KEY_ACTION_NAMES.get(action, "unknown"),
            "modifier": modifier,
            "keycode": keycode,
            "rgb_enabled": rgb_en,
            "rgb_color": (r << 16) | (g << 8) | b,
            "rgb_brightness": bri,
        })

    # 08-27: 休眠时间(分钟)在结构体尾部(LE16)。⚠️ offset 是 36 不是 35:
    # C 结构体 keys[3](27B) 后是奇数 offset 35, uint16 需 2 字节对齐 → 编译器插 1 字节
    # padding, sleep_min 实际在 offset 36。旧固件回包无此字段(len<38) -> 默认 45;
    # 0xFFFF(旧数据/擦除态) -> 默认 45; 0 = 永不。
    sleep_min = 45
    if len(resp) >= 38:
        sleep_min = struct.unpack_from("<H", resp, 36)[0]
        if sleep_min == 0xFFFF:
            sleep_min = 45

    # 09-01: 鼠标方向在 sleep_min 之后(offset 38)。旧固件(40B)也含此字段。
    air_mouse_dir = 0
    if len(resp) >= 39:
        d = resp[38]
        air_mouse_dir = d if d <= 3 else 0

    # 09-03: 摇一摇 —— 仅新固件(52B)有。结构体布局(含 padding):
    #   en@39 sens@40 shake_key@41..49(9B: action,mod,keycode,res,en,r,g,b,bri)
    # 旧固件(len<52)这些字节是 padding/未定义, 一律用默认值。
    shake_enabled = SHAKE_OFF
    shake_sens = SHAKE_SENS_DEFAULT
    shake_key = {"action": "none", "modifier": 0, "keycode": 0}
    if len(resp) >= 52:
        en = resp[39]
        sens = resp[40]
        shake_enabled = 1 if en == SHAKE_ON else SHAKE_OFF
        if sens in SHAKE_SENS_NAMES:
            shake_sens = sens
        a, m, kc = resp[41], resp[42], resp[43]
        act = KEY_ACTION_NAMES.get(a, "none")
        if act in ("keyboard", "multimedia"):
            shake_key = {"action": act, "modifier": m, "keycode": kc}

    # 09-05: L2/L3/L1 侧键 —— 68B 固件有 L2/L3(结构体尾部 l2_key@50..58
    # l3_key@59..67), 80B 固件另有 l1_key@68..76。
    # 10-04: EC 按压滚动上/下 @304/@313 —— 324B 固件(EC 按压滚动版)才有;
    # 旧固件 len<313 -> 保持默认(滚轮上/下), 与旋转键旧固件行为一致。
    # 历史默认: L2=鼠标左键 L3=鼠标右键 L1=空中鼠标(旧固件/旧数据统一按此显示)。
    l_keys = {}
    for name, off, def_kc, def_act, kcs in (
            ("l2_key", 50, 1, "mouse", LKEY_MOUSE_KEYCODES),
            ("l3_key", 59, 2, "mouse", LKEY_MOUSE_KEYCODES),
            ("l1_key", 68, 0, "airmouse", LKEY_MOUSE_KEYCODES),
            ("ec_cw_key", 77, 5, "mouse", EC_MOUSE_KEYCODES_ROT),
            ("ec_press_key", 86, 4, "mouse", EC_MOUSE_KEYCODES_PRESS),
            ("ec_ccw_key", 95, 6, "mouse", EC_MOUSE_KEYCODES_ROT),
            ("ec_cw_press_key", 304, 5, "mouse", EC_MOUSE_KEYCODES_ROT),
            ("ec_ccw_press_key", 313, 6, "mouse", EC_MOUSE_KEYCODES_ROT)):
        action, modifier, keycode = def_act, 0, def_kc
        if len(resp) >= off + 9:
            a, m, kc = resp[off], resp[off + 1], resp[off + 2]
            act = KEY_ACTION_NAMES.get(a, "unknown")
            if act == "mouse":
                # 鼠标动作: keycode 必须是该手势的合法按键位, 非法兜底历史默认
                action = "mouse"
                keycode = kc if kc in kcs else def_kc
            elif act == "airmouse" and (name == "l1_key" or name.startswith("ec_")):
                # 09-06: airmouse 动作 —— L1 与 EC 三手势均允许(键码归零)
                action, modifier, keycode = "airmouse", 0, 0
            elif act in ("keyboard", "multimedia", "none"):
                action, modifier, keycode = act, m, kc
            # unknown(0xFF 等) -> 保持默认
        l_keys[name] = {"action": action, "modifier": modifier, "keycode": keycode}

    # 09-07: 把双击/长按合并进 keys[i](C++ 只透传 keys[], QML 直接读 dbl_*/lng_* 字段)。
    # 旧固件(len<160)时 _parse_c_gestures 内部兜底为 none, 不会越界。
    for i in range(min(3, len(keys))):
        keys[i].update(_parse_c_gestures(resp, 104 + i * 18))

    # 09-07: 手势"单击"独立块(结构体 offset 164+i*9, 仅 len>=173 的新固件有)。
    # 与常规键 keys[] 分开存储 —— 改单击不影响常规。旧固件/旧数据 -> none(未设置)。
    for i in range(min(3, len(keys))):
        keys[i].update(_parse_c_tap(resp, 164 + i * 9))

    # 09-07: 每键显式模式 —— 结构体 offset 160/161/162(仅 len>=163 的新固件有)。
    # 0=常规(直通) 1=手势; 0xFF/缺失(旧固件/旧数据, 模式字节区在旧 160B 写界外,
    # 恒为擦除态) -> 派生兜底(与固件 key_config_get_c_mode 同规则):
    # 双击/长按任一配了真实动作(非 none) -> 手势, 否则常规。写回时转显式落盘。
    for i in range(min(3, len(keys))):
        mode = 0xFF
        if len(resp) >= 163:
            mode = resp[160 + i]
        if mode not in (0, 1):
            kd = keys[i]
            mode = 1 if (kd.get("dbl_action", "none") != "none" or
                         kd.get("lng_action", "none") != "none") else 0
        keys[i]["c_mode"] = mode

    # 09-07: L1/L2/L3 模式 + 三手势 —— 并入 l1_key/l2_key/l3_key 字典(C++ 侧
    # 只透传这三个 map, QML 读 l_mode/dbl_*/lng_*/tap_* 字段)。旧固件(192B 写界
    # 之外)此区恒为 0xFF -> 模式按派生兜底(与固件 key_config_get_l_mode 同规则:
    # 双击/长按任一真实动作且键块合法, L1 允许 AIRMOUSE), 各手势块按"未设置"。
    for li, name in enumerate(("l1_key", "l2_key", "l3_key")):
        lk = l_keys[name]
        allow_am = (li == 0)   # L1 手势允许空中鼠标(用户要求)
        ges = _parse_l_gestures(resp, 195 + li * 18, allow_am)
        tap = _parse_l_tap(resp, 249 + li * 9, allow_am)
        mode = resp[192 + li] if len(resp) >= 193 + li else 0xFF
        if mode not in (0, 1):
            mode = 1 if (ges["dbl_action"] != "none" or
                         ges["lng_action"] != "none") else 0
        lk["l_mode"] = mode
        lk.update(ges)
        lk.update(tap)

    # 09-07: EC 按下 模式 + 三手势 —— 并入 ec_press_key 字典(C++/QML 读
    # ec_mode/dbl_*/lng_*/tap_* 字段)。旧固件(276B 写界之外)此区恒为 0xFF ->
    # 模式按派生兜底(与固件 key_config_get_ec_press_mode 同规则: 双击/长按任一
    # 真实动作即手势), 各手势块按"未设置"。旋转键 ec_cw/ec_ccw 无手势。
    ep = l_keys["ec_press_key"]
    eges = _parse_ec_press_gestures(resp, 277)   # dbl@277 lng@286(结构体偏移)
    etap = _parse_ec_press_tap(resp, 295)        # tap@295
    emode = resp[276] if len(resp) >= 277 else 0xFF
    if emode not in (0, 1):
        emode = 1 if (eges["dbl_action"] != "none" or
                      eges["lng_action"] != "none") else 0
    ep["ec_mode"] = emode
    ep.update(eges)
    ep.update(etap)

    return {
        "keys": keys,
        "air_mouse_mode": AIR_MOUSE_MODE_NAMES.get(am_mode, "toggle"),
        "air_mouse_speed": AIR_MOUSE_SPEED_NAMES.get(am_speed, "medium"),
        "sleep_min": sleep_min,
        "air_mouse_dir": air_mouse_dir,
        "shake_enabled": shake_enabled,
        "shake_sens": shake_sens,
        "shake_key": shake_key,
        "l2_key": l_keys["l2_key"],
        "l3_key": l_keys["l3_key"],
        "l1_key": l_keys["l1_key"],
        "ec_cw_key": l_keys["ec_cw_key"],
        "ec_press_key": l_keys["ec_press_key"],
        "ec_ccw_key": l_keys["ec_ccw_key"],
        "ec_cw_press_key": l_keys["ec_cw_press_key"],
        "ec_ccw_press_key": l_keys["ec_ccw_press_key"],
    }


# 09-07: 解析单个 C 键的双击/长按两块(各 9B) -> {"dbl_*", "lng_*"}。
# 与固件 key_config_ckey_valid 同规则: NONE/键盘/多媒体 + MOUSE 键码 1..6;
# 非法动作一律按"未设置"(none)。
def _parse_c_gestures(resp, off):
    def one(o):
        if len(resp) < o + 9:
            return {"action": "none", "modifier": 0, "keycode": 0}
        a, m, kc = resp[o], resp[o + 1], resp[o + 2]
        act = KEY_ACTION_NAMES.get(a, "none")
        if act == "mouse" and kc not in CKEY_MOUSE_KEYCODES:
            act = "none"
        if act not in ("keyboard", "multimedia", "mouse", "none"):
            act = "none"
        return {"action": act, "modifier": m if act in ("keyboard", "multimedia") else 0,
                "keycode": kc if act in ("keyboard", "multimedia", "mouse") else 0}
    dbl = one(off)
    lng = one(off + 9)
    return {"dbl_action": dbl["action"], "dbl_modifier": dbl["modifier"], "dbl_keycode": dbl["keycode"],
            "lng_action": lng["action"], "lng_modifier": lng["modifier"], "lng_keycode": lng["keycode"]}


# 09-07: 手势"单击"独立块(9B) -> {"tap_*"}。规则同双击/长按。
def _parse_c_tap(resp, off):
    if len(resp) < off + 9:
        return {"tap_action": "none", "tap_modifier": 0, "tap_keycode": 0}
    a, m, kc = resp[off], resp[off + 1], resp[off + 2]
    act = KEY_ACTION_NAMES.get(a, "none")
    if act == "mouse" and kc not in CKEY_MOUSE_KEYCODES:
        act = "none"
    if act not in ("keyboard", "multimedia", "mouse", "none"):
        act = "none"
    return {"tap_action": act,
            "tap_modifier": m if act in ("keyboard", "multimedia") else 0,
            "tap_keycode": kc if act in ("keyboard", "multimedia", "mouse") else 0}


# 09-07: 解析单个 L 键(L1/L2/L3)的双击/长按两块(各 9B) -> {"dbl_*", "lng_*}。
# 规则与固件 key_config_lgesture_valid 一致: NONE/键盘/多媒体 + MOUSE 键码 1..6;
# AIRMOUSE 仅 allow_airmouse=1(L1)时合法, L2/L3 的 AIRMOUSE 一律按"未设置"(none)。
def _parse_l_gestures(resp, off, allow_airmouse):
    def one(o):
        if len(resp) < o + 9:
            return {"action": "none", "modifier": 0, "keycode": 0}
        a, m, kc = resp[o], resp[o + 1], resp[o + 2]
        act = KEY_ACTION_NAMES.get(a, "none")
        if act == "airmouse":
            act = "airmouse" if allow_airmouse else "none"
        elif act == "mouse" and kc not in LGEST_MOUSE_KEYCODES:
            act = "none"
        if act not in ("keyboard", "multimedia", "mouse", "none", "airmouse"):
            act = "none"
        return {"action": act, "modifier": m if act in ("keyboard", "multimedia") else 0,
                "keycode": kc if act in ("keyboard", "multimedia", "mouse") else 0}
    dbl = one(off)
    lng = one(off + 9)
    return {"dbl_action": dbl["action"], "dbl_modifier": dbl["modifier"], "dbl_keycode": dbl["keycode"],
            "lng_action": lng["action"], "lng_modifier": lng["modifier"], "lng_keycode": lng["keycode"]}


# 09-07: L 键手势"单击"独立块(9B) -> {"tap_*"}。规则同 _parse_l_gestures。
def _parse_l_tap(resp, off, allow_airmouse):
    if len(resp) < off + 9:
        return {"tap_action": "none", "tap_modifier": 0, "tap_keycode": 0}
    a, m, kc = resp[off], resp[off + 1], resp[off + 2]
    act = KEY_ACTION_NAMES.get(a, "none")
    if act == "airmouse":
        act = "airmouse" if allow_airmouse else "none"
    elif act == "mouse" and kc not in LGEST_MOUSE_KEYCODES:
        act = "none"
    if act not in ("keyboard", "multimedia", "mouse", "none", "airmouse"):
        act = "none"
    return {"tap_action": act,
            "tap_modifier": m if act in ("keyboard", "multimedia") else 0,
            "tap_keycode": kc if act in ("keyboard", "multimedia", "mouse") else 0}


# 09-07: EC 按下手势 双击/长按 两块(各 9B, 结构体偏移 off=dbl) -> {"dbl_*, lng_*}。
# 规则与固件 key_config_ecpressgesture_valid = eckey_valid 一致: NONE/键盘/多媒体
# + MOUSE 键码 1..6 + AIRMOUSE(EC 常规键本就允许); 非法动作一律按"未设置"(none)。
def _parse_ec_press_gestures(resp, off):
    def one(o):
        if len(resp) < o + 9:
            return {"action": "none", "modifier": 0, "keycode": 0}
        a, m, kc = resp[o], resp[o + 1], resp[o + 2]
        act = KEY_ACTION_NAMES.get(a, "none")
        if act == "airmouse":
            act = "airmouse"
        elif act == "mouse" and kc not in EC_MOUSE_KEYCODES_ROT:
            act = "none"
        if act not in ("keyboard", "multimedia", "mouse", "none", "airmouse"):
            act = "none"
        return {"action": act, "modifier": m if act in ("keyboard", "multimedia") else 0,
                "keycode": kc if act in ("keyboard", "multimedia", "mouse") else 0}
    dbl = one(off)
    lng = one(off + 9)
    return {"dbl_action": dbl["action"], "dbl_modifier": dbl["modifier"], "dbl_keycode": dbl["keycode"],
            "lng_action": lng["action"], "lng_modifier": lng["modifier"], "lng_keycode": lng["keycode"]}


# 09-07: EC 按下手势"单击"独立块(9B, 结构体偏移 off) -> {"tap_*"}。规则同
# _parse_ec_press_gestures; 未设置(NONE)亦返回 none(调用方据此显示"未设置")。
def _parse_ec_press_tap(resp, off):
    if len(resp) < off + 9:
        return {"tap_action": "none", "tap_modifier": 0, "tap_keycode": 0}
    a, m, kc = resp[off], resp[off + 1], resp[off + 2]
    act = KEY_ACTION_NAMES.get(a, "none")
    if act == "airmouse":
        act = "airmouse"
    elif act == "mouse" and kc not in EC_MOUSE_KEYCODES_ROT:
        act = "none"
    if act not in ("keyboard", "multimedia", "mouse", "none", "airmouse"):
        act = "none"
    return {"tap_action": act,
            "tap_modifier": m if act in ("keyboard", "multimedia") else 0,
            "tap_keycode": kc if act in ("keyboard", "multimedia", "mouse") else 0}


def parse_info_response(text):
    """解析 INFO 回包 "VibeKey-F3|<ver>|<bat>|<slot>\n" → dict。
    电量 255=未知; 槽位 1~3。格式不符返回 None(由调用方兜底)。"""
    parts = text.strip().split("|")
    if len(parts) != 4:
        return None
    model, version, battery, slot = parts
    try:
        battery = int(battery)
        slot = int(slot)
    except ValueError:
        return None
    return {"model": model, "version": version, "battery": battery, "slot": slot}


def conf_read(port, slot=0):
    ser = open_port(port)
    # 08-27: buf[5] 带槽位(0~2=槽1~3, 越界设备端按槽1处理); 旧版上位机缺省 slot=0
    header = build_conf_frame(CONF_CMD_READ, slot)
    # 09-03: 新固件回包 52B(含摇一摇字段); 旧固件回包 40B。先收 40B, 若还有
    # 余量(新固件多出的 12B 已随同一 IN 传输到达缓冲)则短超时补读。
    resp = send_and_receive(ser, header, CFG_READ_LEN_OLD)
    if len(resp) >= CFG_READ_LEN_OLD:
        try:
            ser.timeout = 0.25
            expect_tail = CFG_READ_LEN_NEW - CFG_READ_LEN_OLD
            tail = b""
            deadline = time.monotonic() + 1.0
            while len(tail) < expect_tail:
                if time.monotonic() > deadline:
                    break
                chunk = ser.read(expect_tail - len(tail))
                if chunk:
                    tail += chunk
                    deadline = time.monotonic() + 0.25
                else:
                    time.sleep(0.02)
        except Exception:
            tail = b""
        if tail:
            resp += tail
    ser.close()
    debug_log(f"Response length: {len(resp)}")

    try:
        cfg = parse_read_response(resp)
    except ValueError as e:
        print(f"ERROR:{e}")
        sys.exit(1)

    print(json.dumps(cfg))
    print("DONE", flush=True)

def conf_write(port, config_path, slot=0):
    with open(config_path, "r") as f:
        config = json.load(f)
    # 09-03: 组 43B payload 抽到公共函数 build_write_payload(USB/BLE 双通道复用)
    try:
        payload = build_write_payload(config)
    except ValueError as e:
        print(f"ERROR:{e}")
        sys.exit(1)

    ser = open_port(port)
    # 08-27: buf[5] 带槽位
    header = build_conf_frame(CONF_CMD_WRITE, slot, payload)
    resp = send_and_receive(ser, header, 8)
    ser.close()
    if len(resp) >= 1 and resp[0] == 0x00:
        print("DONE", flush=True)
    else:
        print("ERROR:Device rejected config", flush=True)
        sys.exit(1)

def conf_reset(port, slot=0):
    ser = open_port(port)
    # 08-27: buf[5] 带槽位
    header = build_conf_frame(CONF_CMD_RESET, slot)
    resp = send_and_receive(ser, header, 8)
    ser.close()
    if len(resp) >= 1 and resp[0] == 0x00:
        print("DONE", flush=True)
    else:
        print("ERROR:Device rejected reset", flush=True)
        sys.exit(1)

def conf_led(port, state):
    ser = open_port(port)
    header = build_conf_frame(CONF_CMD_LED, 0, bytes([state]))
    resp = send_and_receive(ser, header, 8)
    ser.close()
    if len(resp) >= 1 and resp[0] == 0x00:
        print("DONE", flush=True)
    else:
        print("ERROR:Device rejected LED command", flush=True)
        sys.exit(1)

def conf_rgb(port, key_index, enabled, r, g, b, brightness):
    """单键 RGB 实时预览（不落盘）。payload = idx,enabled,r,g,b,brightness(各1字节)。

    10-04: 固件把回包从 1 字节(0x00) 改成 **2 字节** [0]=0x00 已受理 [1]=实际生效的
    限幅后亮度(%)。等待长度必须同步改，否则 send_and_receive 等不到旧长度而超时，
    导致"明明下发成功却报错"。同时打印 eff_bri 便于确认限幅真的生效。
    """
    ser = open_port(port)
    payload = struct.pack("BBBBBB", key_index & 0xFF, enabled & 0xFF,
                          r & 0xFF, g & 0xFF, b & 0xFF, brightness & 0xFF)
    header = build_conf_frame(CONF_CMD_RGB_APPLY, 0, payload)
    resp = send_and_receive(ser, header, 2)   # 2 字节：code + eff_bri
    ser.close()
    if len(resp) >= 1 and resp[0] == 0x00:
        eff = resp[1] if len(resp) >= 2 else -1
        print("DONE eff_bri=%d" % eff, flush=True)
    elif not resp:
        print("ERROR:no response from device (cmd not delivered?)", flush=True)
        sys.exit(1)
    else:
        print("ERROR:Device rejected RGB command (code=%s len=%d)"
              % (resp[0], len(resp)), flush=True)
        sys.exit(1)

if __name__ == "__main__":
    debug_log(f"args={sys.argv} python={sys.executable} cwd={os.getcwd()}")
    if len(sys.argv) < 3:
        print("ERROR:Usage: config_worker.py <COM port> read|write|reset|led|rgb ...")
        sys.exit(1)
    port = sys.argv[1]
    cmd = sys.argv[2]
    debug_log(f"port={port} cmd={cmd}")
    # 08-27: 槽位参数(0~2=槽1~3, 默认 0=槽1), read/write/reset 支持
    slot = 0
    if cmd in ("read", "reset"):
        if len(sys.argv) >= 4:
            slot = int(sys.argv[3])
    if cmd == "read":
        conf_read(port, slot)
    elif cmd == "write":
        if len(sys.argv) < 4:
            print("ERROR:Usage: config_worker.py <COM port> write <config.json> [slot]")
            sys.exit(1)
        if len(sys.argv) >= 5:
            slot = int(sys.argv[4])
        conf_write(port, sys.argv[3], slot)
    elif cmd == "reset":
        conf_reset(port, slot)
    elif cmd == "led":
        if len(sys.argv) < 4:
            print("ERROR:Usage: config_worker.py <COM port> led <state>")
            sys.exit(1)
        conf_led(port, int(sys.argv[3]))
    elif cmd == "rgb":
        # config_worker.py <COM port> rgb <key_index> <enabled> <r> <g> <b> <brightness>
        if len(sys.argv) < 8:
            print("ERROR:Usage: config_worker.py <COM port> rgb <key> <on> <r> <g> <b> <bri>")
            sys.exit(1)
        conf_rgb(port, int(sys.argv[3]), int(sys.argv[4]),
                 int(sys.argv[5]), int(sys.argv[6]), int(sys.argv[7]), int(sys.argv[8]))
    else:
        print(f"ERROR:Unknown command: {cmd}")
        sys.exit(1)
