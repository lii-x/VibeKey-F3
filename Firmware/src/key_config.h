#ifndef __KEY_CONFIG_H__
#define __KEY_CONFIG_H__

#include <stdint.h>
#include "button.h"

#define KEY_CONFIG_MAGIC       0x4B455943   // "KEYC"
#define KEY_CONFIG_VERSION     3            // v3: 增加 KEY L1 空中鼠标配置(模式+灵敏度)
                                            /* ⚠️ 09-03 加摇一摇字段时【刻意未升到 v4】:
                                             * key_config_init 用 version != KEY_CONFIG_VERSION
                                             * 判定, 升版会把用户全部槽的自定义键配置/休眠
                                             * 全部重置为默认。新字段沿用 sleep_min、air_mouse_dir
                                             * 的"尾部追加 + 擦除态兜底"策略平滑迁移。 */
#define KEY_CONFIG_FLASH_ADDR  0x12628000  /* 8MB Flash 重排后: 紧跟 KVDB_BLE(0x12624000~0x12628000) 之后; 原 0x12C00000 与 KVDB_DFU 分区重叠, 已下移修复 */
#define KEY_CONFIG_NUM_KEYS    3
#define KEY_CONFIG_SLOT_NUM    3            /* 每蓝牙槽位一份独立键配置(08-27) */
#define KEY_CONFIG_SLOT_STRIDE 0x1000       /* 每槽独立 4KB 页, 互不干扰 */
/* 槽位 flash 布局(08-27, 与 BT_SLOT 共存):
 *   槽1: 0x12628000  (沿用旧版单份配置地址 -> 升级后槽1 天然继承旧配置)
 *   BT_SLOT: 0x12629000 (不动)
 *   槽2: 0x1262A000  槽3: 0x1262B000
 * 每槽独立一页, 单槽写入只擦自己的页, 不影响其他槽。 */

typedef struct {
    int32_t pin;
    button_action_t action;
} button_event_t;

typedef enum {
    KEY_ACTION_NONE       = 0,
    KEY_ACTION_KEYBOARD   = 1,
    KEY_ACTION_MOUSE      = 2,
    KEY_ACTION_MULTIMEDIA = 3,
    /* 09-05: 仅用于 KEY L1 的默认动作(空中鼠标开关)。L2/L3 的配置不会出现
     * 该值(上位机不发, 固件 key_config_lkey_valid 拒绝)。 */
    KEY_ACTION_AIRMOUSE   = 4,
} key_action_type_t;

typedef struct {
    uint8_t action_type;
    uint8_t modifier;
    uint8_t keycode;
    uint8_t reserved;
    uint8_t rgb_enabled;    /* 0=关, 1=开（每键 WS2812B 底光）*/
    uint8_t rgb_r;          /* 颜色 R 分量 0-255 */
    uint8_t rgb_g;          /* 颜色 G 分量 0-255 */
    uint8_t rgb_b;          /* 颜色 B 分量 0-255 */
    uint8_t rgb_brightness; /* 亮度 0-100 (%) */
} key_config_t;

/* KEY L1 空中鼠标工作模式 */
typedef enum {
    AIR_MOUSE_MODE_TOGGLE = 0,   /* 单击 L1: 开 -> 再单击: 关 */
    AIR_MOUSE_MODE_HOLD   = 1,   /* 按住 L1 移动, 松开停止 */
} air_mouse_mode_t;

/* KEY L1 空中鼠标灵敏度档位 */
typedef enum {
    AIR_MOUSE_SPEED_SLOW   = 0,
    AIR_MOUSE_SPEED_MEDIUM = 1,
    AIR_MOUSE_SPEED_FAST   = 2,
} air_mouse_speed_t;

/* KEY L1 空中鼠标方向(09-01): IMU 位移 → 光标位移的映射。
 * 4 档【旋转】变换(09-01 由"镜像翻转"方案改为角度旋转 —— 用户反馈翻转档位
 * 不直观/混乱)。以用户实测正确的"正常"档(0°)为基准, 其余档相对该姿态旋转:
 *   0 0°(正常)   ( dx,  dy)                    用户默认握姿(设备侧握)
 *   1 旋转 90°   ( dx,  dy)->(-dy,  dx)         09-01 四方向实测反推: 上/下/左 三向命中
 *   2 旋转 180°  ( dx,  dy)->(-dx, -dy)         倒过来拿(唯一确定)
 *   3 旋转 270°  ( dx,  dy)->( dy, -dx)         与 90° 严格反向, 实测选对的一档
 * 解决"手持方向不同 / 握姿不同导致光标往反方向跑"的问题，由上位机写入。
 * ⚠️ 注意这是【旋转】不是镜像: 90°/270° 会交换 X/Y 轴方向(横竖握切换)。 */
typedef enum {
    AIR_MOUSE_DIR_0   = 0,   /* 0° 正常(用户默认握姿基准) */
    AIR_MOUSE_DIR_90  = 1,   /* 90°(顺时针) */
    AIR_MOUSE_DIR_180 = 2,   /* 180° 反向 */
    AIR_MOUSE_DIR_270 = 3,   /* 270°(即逆时针 90°) */
} air_mouse_dir_t;

#define KEY_CONFIG_AIR_MOUSE_DIR_DEFAULT  AIR_MOUSE_DIR_0

/* 摇一摇灵敏度档位(09-03)。三档对应不同的【加速度阈值(mg)】与【触发所需峰值数】:
 *   0 轻摇: 幅度小也要能触发, 但更容易被走动/拿起误触
 *   1 适中: 默认档
 *   2 用力: 必须明显甩动才触发, 最不容易误触
 * 具体阈值见 main.c 的 shake_sens_params[]。 */
typedef enum {
    SHAKE_SENS_LIGHT  = 0,
    SHAKE_SENS_MEDIUM = 1,
    SHAKE_SENS_STRONG = 2,
} shake_sens_t;

#define KEY_CONFIG_SHAKE_SENS_DEFAULT  SHAKE_SENS_MEDIUM
#define KEY_CONFIG_SHAKE_OFF           0
#define KEY_CONFIG_SHAKE_ON            1

typedef struct {
    uint32_t magic;
    uint8_t version;
    uint8_t num_keys;
    uint8_t air_mouse_mode;     /* 0=toggle, 1=hold */
    uint8_t air_mouse_speed;    /* 0=slow, 1=medium, 2=fast */
    key_config_t keys[KEY_CONFIG_NUM_KEYS];
    /* 08-27: 连接态空闲休眠时间(分钟)。0=永不; 45~240=空闲该时长后自动休眠。
     * 0xFFFF(旧固件数据未写此字段/擦除态) 视为未设置 -> 加载默认 45。 */
    uint16_t sleep_min;
    /* 09-01: 空中鼠标方向(见 air_mouse_dir_t)。放在 sleep_min【之后】:
     * 原结构体有效长度 38B + 2B padding = sizeof 40，新增 1B 后为 39B + 1B padding
     * = sizeof 仍为 40 —— READ 回包长度与既有字段偏移【完全不变】, 旧上位机兼容。
     * 旧 flash 数据此处为 0xFF(擦除态)/残留值 -> key_config_init 兜底为"正常"。 */
    uint8_t air_mouse_dir;
    /* ---- 09-03 摇一摇(Shake)配置 ----
     * ⚠️ 布局说明(READ 回包是【结构体内存布局】, 含 padding):
     *   air_mouse_dir @38, shake_enabled @39(吃掉原 padding), shake_sens @40,
     *   shake_key @41..49(9B), 尾部 padding 50..51 → sizeof 从 40 变 52。
     *   READ 回包长度自动跟随 sizeof, 旧上位机读 40B 只是读不到新字段, 安全。
     * ⚠️ WRITE payload 是【紧凑布局, 无 padding】, 偏移与结构体不同:
     *   payload[31]=dir [32]=shake_enabled [33]=shake_sens [34..42]=shake_key
     *   (payload 索引 + 8 = buf 索引), 见 dfu_device.c。
     * ⚠️ 旧 flash 数据此处为 0xFF(擦除态):
     *   shake_enabled=0xFF -> 兜底为"关"(功能默认关闭, 升级后行为不变);
     *   shake_sens=0xFF    -> 兜底为"适中";
     *   shake_key 非 0/1/3 的 action_type -> 兜底为"未设置"(不发键)。 */
    uint8_t shake_enabled;      /* 0=关, 1=开(其余值按关处理) */
    uint8_t shake_sens;         /* 0=轻, 1=适中, 2=用力(其余值按适中处理) */
    key_config_t shake_key;     /* 触发后发出的快捷键(键盘组合键 / 多媒体键) */
    /* ---- 09-05 L2/L3 侧键快捷键(上位机可改) ----
     * ⚠️ 尾部追加(与 sleep_min/air_mouse_dir/摇一摇 同策略): offset 0..49 完全
     * 不变, 旧 flash 数据读入后新字段为 0xFF(擦除态) -> key_config_init 兜底为
     * "鼠标键"(L2=左键 L3=右键, 即历史硬编码行为), 不升 KEY_CONFIG_VERSION。
     * action_type 语义: MOUSE=鼠标键(keycode 1/2/4=左/右/中, 按住保持);
     * KEYBOARD/MULTIMEDIA=快捷键(按下触发, PRESSED/RELEASED); NONE=无动作。
     * ⚠️ READ 回包长度随 sizeof 从 52 变 68(l2_key@50 l3_key@59);
     * WRITE payload(紧凑布局)从 43 变 61: payload[43..51]=L2 [52..60]=L3
     * (payload 索引+8=buf 索引), 见 dfu_device.c。
     * ⚠️ L2/L3 无 RGB 硬件(WS2812B 仅 3 颗对应 C1-C3), rgb_* 字段存而不用。 */
    key_config_t l2_key;        /* KEY L2(默认鼠标左键) */
    key_config_t l3_key;        /* KEY L3(默认鼠标右键) */
    /* ---- 09-05: KEY L1 侧键(默认 KEY_ACTION_AIRMOUSE=空中鼠标开关, 与历史
     * 硬编码行为一致; 也可改键盘/多媒体/无动作)。迁移策略与 L2/L3 相同:
     * 尾部追加, 旧 flash 数据读到 0xFF -> 兜底为空中鼠标。
     * ⚠️ READ 回包 52→68→80(l1_key@68..76, sizeof 对齐后 80);
     * WRITE payload 43→61→70: payload[61..69]=L1(payload 索引+8=buf 索引)。 */
    key_config_t l1_key;        /* KEY L1(默认空中鼠标开关) */
    /* ---- 09-05: EC 编码器三手势(上位机可改) ----
     * ec_cw=顺时针一步(默认鼠标滚轮上) ec_press=按下(默认鼠标中键)
     * ec_ccw=逆时针一步(默认鼠标滚轮下)。
     * ⚠️ MOUSE 动作键码扩展(仅 EC 三键): 1/2/4=左/右/中键之外,
     *   0x05=滚轮上 0x06=滚轮下(配置层伪键码, main.c 映射为滚轮报告);
     *   ec_press 的 MOUSE 仅允许 1/2/4(按下手势映射滚轮无意义, 不允许)。
     * 迁移策略同 L 键: 尾部追加, 旧 flash 数据读到 0xFF -> 兜底为历史行为。
     * ⚠️ READ 回包 80→104(ec_cw@77 ec_press@86 ec_ccw@95, 无新增尾部对齐);
     * WRITE payload 70→97: payload[70..78]/[79..87]/[88..96](索引+8=buf 索引)。 */
    key_config_t ec_cw_key;     /* 编码器顺时针一步 */
    key_config_t ec_press_key;  /* 编码器按下 */
    key_config_t ec_ccw_key;    /* 编码器逆时针一步 */
    /* ---- 09-05: C 键三手势(上位机可改) ----
     * keys[3] 仍是【单击】动作(偏移 0..33 不动); 双击/长按尾部追加, 每键各一份:
     *   c1_dbl@104 c1_lng@113 c2_dbl@122 c2_lng@131 c3_dbl@140 c3_lng@149,
     *   显式 padding @158..159 -> sizeof 104 -> 160。
     * 迁移策略与 L/EC 键相同: 不升 KEY_CONFIG_VERSION, 旧 flash 数据读到
     * 0xFF(擦除态) -> key_config_init 兜底为"未设置"(KEY_ACTION_NONE)。
     * ⚠️ 09-07 模式字段另放 @160..162(旧 160B 之外), 见下。
     * ⚠️ 手势键无 RGB 硬件联动(rgb_* 字段存而不用, 底光仍挂在 keys[] 单击上)。 */
    key_config_t c1_dbl_key;    /* C1 双击 */
    key_config_t c1_lng_key;    /* C1 长按 */
    key_config_t c2_dbl_key;    /* C2 双击 */
    key_config_t c2_lng_key;    /* C2 长按 */
    key_config_t c3_dbl_key;    /* C3 双击 */
    key_config_t c3_lng_key;    /* C3 长按 */
    /* 显式尾 padding: 结构体含 uint32_t(4B 对齐), 104+54=158 非自然对齐 ->
     * 编译器隐式补 2B 到 160; 显式声明使布局确定。 */
    uint8_t reserved_tail[2];
    /* ---- 09-07: C 键显式模式(上位机"模式"下拉落盘) ----
     * 每键 1B: 0=常规/直通, 1=手势。固件按此判定, 【不再】看双击/长按是否设置
     * (旧 c_gesture_mode 的"任一已设置即手势"判定删除 —— 上位机写默认配置时
     * 双击/长按块是"合法 NONE", 曾被误判成手势模式, 导致按下无反应只能单击)。
     * 常规模式语义: 按下即发按下/松开即发松开, 用 keys[] (主键), 无视手势配置;
     * 手势模式语义: 单击=下方 c*_tap_key【独立存储】(不再复用 keys[]!), 
     * 双击/长按 = c*_dbl/lng 按配置分发(双击窗 ~220ms、长按 800ms)。
     * 0xFF(旧固件 flash 只写了 160B, 本区必为擦除态) -> key_config_get_c_mode
     * 按旧规则派生兜底: 双击或长按配了【真实动作】(非 NONE) -> 手势, 否则常规。
     * ⚠️ 放 reserved_tail【之后】(offset 160..162)是有意的: 旧固件写满 160B 的
     * 槽页在 160..163 恒为 0xFF, 派生兜底确定可靠; 若放 158..160 会误读旧
     * reserved_tail 残留(可能为 0) -> 旧手势配置被静默打成直通。 */
    uint8_t c1_mode;    /* C1 模式: 0=常规(直通) 1=手势 */
    uint8_t c2_mode;    /* C2 模式 */
    uint8_t c3_mode;    /* C3 模式 */
    /* 显式 padding @163(结构体 160+3=163 非 4 倍数, 补齐到 164; 紧凑 payload 里
     * 对应 1B 占位, 设备忽略)。 */
    uint8_t reserved_mode_pad;
    /* ---- 09-07: 手势"单击"独立存储(每键 9B) ----
     * keys[](主键, @8..34) = 常规(直通)模式动作; 手势模式的"单击"若复用 keys[]
     * 会导致"改手势单击把常规键也改了"(用户实测问题) —— 这里为单击单独存一块,
     * 手势模式单击只读它。0xFF/NONE = 单击未设置(点按无动作), 与双击/长按独立。
     * 旧数据(0xFF)读到 NONE: 升级后手势模式单击需在上位机重新设置(新语义)。
     * ⚠️ READ 回包 164->192(c1_tap@164 c2_tap@173 c3_tap@182, 尾 pad@191);
     * WRITE payload(紧凑布局) 156->184: payload[156]=@163 占位(设备忽略),
     *   payload[157..165]=c1_tap [166..174]=c2_tap [175..183]=c3_tap
     *   (buf[165..173]/[174..182]/[183..191], 见 dfu_device.c)。 */
    key_config_t c1_tap_key;    /* C1 手势·单击 */
    key_config_t c2_tap_key;    /* C2 手势·单击 */
    key_config_t c3_tap_key;    /* C3 手势·单击 */
    /* 显式尾 padding: 164+27=191 非 4 的倍数 -> 补 1B 到 192(上位机 READ 按 192 读) */
    uint8_t reserved_tap_pad;
    /* ---- 09-07: L1/L2/L3 三键【模式 + 三手势】—— 与 C 键同架构, 尾部追加 ----
     * 背景: 用户要求"L 键那三个键也能像 C 键那样选模式"(常规/手势)。
     *   常规(直通): L1=空中鼠标开关(或配置的键)/鼠标按住保持等 —— 完全保持
     *   09-05 以来的行为, 无视下方手势配置(存而不用);
     *   手势: 单击=独立 l*_tap_key, 双击/长按 = l*_dbl/lng 按配置分发
     *   (双击窗 220ms / 长按 800ms, 与 C 键同一套时机)。
     * 布局(全在旧固件 192B 写界【之外】, 旧槽页此区恒为 0xFF 擦除态 ->
     *   key_config_get_l_mode 派生兜底可靠):
     *   l1_mode@192 l2_mode@193 l3_mode@194(0=常规 1=手势)
     *   l1_dbl@195 l1_lng@204 l2_dbl@213 l2_lng@222 l3_dbl@231 l3_lng@240
     *   l1_tap@249 l2_tap@258 l3_tap@267  -> sizeof 192 -> 276(尾无需补 pad, 276%4=0)
     * 合法动作: 同 C 键手势(NONE/键盘/多媒体 + MOUSE 键码 1..6=左/右/中/滚轮上下),
     *   【L1 额外允许 AIRMOUSE】(用户要求: L1 手势里也要能开关空中鼠标, 每触发
     *   切换一次, 与 EC 旋转语义一致); L2/L3 手势不允许 AIRMOUSE。
     * WRITE payload(紧凑布局, struct=payload+7 同尾区规则):
     *   payload[184]=reserved_tap_pad@191 占位(设备忽略) payload[185..187]=模式
     *   [188..241]=l1_dbl..l3_lng(6x9B) [242..268]=l1_tap..l3_tap(3x9B)
     *   payload 总长 184 -> 269(帧 277B); READ 回包 192 -> 276B。
     *   旧上位机(payload<=184B)不带此块 -> 不进入分支, L 模式/手势保持现值。 */
    uint8_t l1_mode;    /* L1 模式: 0=常规(保持现行为) 1=手势 */
    uint8_t l2_mode;    /* L2 模式 */
    uint8_t l3_mode;    /* L3 模式 */
    key_config_t l1_dbl_key;    /* L1 双击 */
    key_config_t l1_lng_key;    /* L1 长按 */
    key_config_t l2_dbl_key;    /* L2 双击 */
    key_config_t l2_lng_key;    /* L2 长按 */
    key_config_t l3_dbl_key;    /* L3 双击 */
    key_config_t l3_lng_key;    /* L3 长按 */
    key_config_t l1_tap_key;    /* L1 手势·单击(独立于 l1_key 常规键) */
    key_config_t l2_tap_key;    /* L2 手势·单击 */
    key_config_t l3_tap_key;    /* L3 手势·单击 */
    /* ---- 09-07: EC 按下【模式 + 三手势】—— 与 C/L 键同架构, 尾部追加 ----
     * 背景: 上位机 EC 编码器 UI 支持"按下"选模式 —— 常规(直通)用 ec_press_key
     * 主键(历史行为: 中键/键/空中鼠标等); 手势=单击/双击/长按三手势独立配置
     * (与 C/L 键同一套 UI/语义)。旋转(cw/ccw)是离散步进, 无手势概念。
     * 布局(全在旧固件 276B 写界【之外】, 旧槽页此区恒为 0xFF 擦除态 ->
     *   key_config_get_ec_press_mode 派生兜底可靠, 与 C/L 模式同策略):
     *   ec_press_mode@276(0=常规 1=手势)
     *   ec_press_dbl@277..285 ec_press_lng@286..294 ec_press_tap@295..303
     *   -> sizeof 276 -> 304(304%4=0, 无需尾 padding)。
     * 合法动作: 与 EC 常规键同集(NONE/键盘/多媒体/MOUSE 键码 1..6 +
     *   AIRMOUSE —— 空中鼠标每触发切换一次, 语义同 EC 旋转/L1 手势);
     * 手势"单击"用独立 tap 块(与常规键 ec_press_key 分开, 改手势单击不影响常规)。
     * ⚠️ READ 回包 276 -> 304; WRITE payload(紧凑布局) 269 -> 297:
     *   payload[269]=模式 payload[270..296]=dbl/lng/tap(3x9B), 见 dfu_device.c。 */
    uint8_t ec_press_mode;      /* EC 按下模式: 0=常规(直通) 1=手势 */
    key_config_t ec_press_dbl_key;  /* EC 按下·双击 */
    key_config_t ec_press_lng_key;  /* EC 按下·长按 */
    key_config_t ec_press_tap_key;  /* EC 按下·手势单击(独立于 ec_press_key 常规键) */
    /* ---- 10-05: EC 按压滚动(手势模式: 按住旋钮时旋转) ----
     * 背景(用户需求): 手势模式下 EC 编码器要区分【常规滚动】(不按住旋转)与
     *   【按压滚动】(按住旋钮旋转), 且**只要发生按压滚动, 就清零 EC 按下手势状态**
     *   (单击判定窗/定时器) —— 本次按压周期内不再触发单击/双击/长按, 避免"按住
     *   旋钮旋转"松手时被识别成点按。
     * 字段划分:
     *   常规滚动上/下 = ec_cw_key / ec_ccw_key(沿用 09-05 字段, 语义不变);
     *   按压滚动上/下 = ec_cw_press_key / ec_ccw_press_key(本块新增)。
     * 合法动作: 与 EC 旋转键同集(key_config_eckey_valid rotation=1):
     *   NONE/键盘/多媒体/MOUSE 键码 1..6(5/6=滚轮上/下) + AIRMOUSE。
     * 行为实现见 main.c(enc_event_handler + 主循环 EC 分支)。
     * 布局(全在旧固件 304B 写界【之外】, 旧槽页此区恒为 0xFF 擦除态):
     *   ec_cw_press@304..312 ec_ccw_press@313..321 -> 322, 显式补 2B 对齐
     *   -> sizeof 304 -> 324(324%4=0)。
     * 迁移: 未写入/非法(0xFF) 时 getter 【回落对应的常规滚动键】—— 行为与常规滚动
     *   一致, 不会出现"按住旋转反而什么都不做"的退化; 显式 action=NONE 则尊重
     *   用户选择(按压滚动=无动作, 但仍抑制单击/双击/长按)。
     * ⚠️ READ 回包 304 -> 324; WRITE payload(紧凑布局) 297 -> 315:
     *   payload[297..305]=ec_cw_press [306..314]=ec_ccw_press, 见 dfu_device.c。 */
    key_config_t ec_cw_press_key;   /* 按压滚动·上 */
    key_config_t ec_ccw_press_key;  /* 按压滚动·下 */
    /* 显式尾 padding: 304+18=322 非 4 的倍数 -> 补 2B 到 324(上位机 READ 按 324 读) */
    uint8_t reserved_ecscroll_pad[2];
} key_config_storage_t;

/* 休眠时间默认值/档位(08-27) */
#define KEY_CONFIG_SLEEP_MIN_DEFAULT  45    /* 默认(也是最小档): 45 分钟 */
#define KEY_CONFIG_SLEEP_MIN_NEVER    0     /* 永不自动休眠 */
#define KEY_CONFIG_SLEEP_MIN_MAX      240   /* 上限: 4 小时 */

void key_config_init(void);
key_config_storage_t *key_config_get(void);       /* 当前蓝牙槽位的配置 */
void key_config_save(void);                       /* 保存当前槽位 */
void key_config_reset(void);                      /* 重置当前槽位为默认 */
key_config_storage_t *key_config_get_slot(uint8_t slot);  /* 指定槽 0~2 (CONF 协议用) */
void key_config_save_slot(uint8_t slot);
void key_config_reset_slot(uint8_t slot);
uint16_t key_config_get_sleep_min(void);   /* 当前槽休眠分钟(0=永不), power.c 用 */
uint8_t key_config_get_air_mouse_dir(void);/* 当前槽空中鼠标方向 0~3(09-01), main.c 位移映射用; 非法值兜底为"正常" */
/* 09-03 摇一摇: 每次调用都校验(不只加载时), 运行中被旧上位机误写 0xFF 也安全 */
uint8_t key_config_get_shake_enabled(void); /* 当前槽摇一摇开关 0/1 */
uint8_t key_config_get_shake_sens(void);    /* 当前槽灵敏度 0~2, 非法值兜底为"适中" */
const key_config_t *key_config_get_shake_key(void);  /* 当前槽快捷键; action_type 非法时返回 NULL */
/* 09-05 L2/L3 侧键快捷键: 返回当前槽配置; 非法(action_type 越界/MOUSE 但 keycode
 * 非 1/2/4)时返回 NULL, 调用方(main.c)按"鼠标键默认位"(L2=左/L3=右)兜底。
 * 加载/写入路径已兜底, 这里只做运行期防御(与 shake 同策略)。 */
int key_config_lkey_valid(const key_config_t *k);    /* L2/L3 键配置合法性(0/1) */
const key_config_t *key_config_get_l2_key(void);
const key_config_t *key_config_get_l3_key(void);
/* 09-05: KEY L1 —— 合法动作 = L2/L3 的集合 + KEY_ACTION_AIRMOUSE(默认);
 * 非法时返回 NULL, main.c 按"空中鼠标"兜底。 */
int key_config_l1key_valid(const key_config_t *k);   /* L1 键配置合法性(0/1) */
const key_config_t *key_config_get_l1_key(void);
/* 09-05: EC 编码器三手势。MOUSE 伪键码: 普通键 1/2/4; 旋转键另允许
 * 0x05=滚轮上 0x06=滚轮下; 按下键仅 1/2/4。非法返回 NULL, main.c 按历史行为兜底
 * (cw=滚轮上 press=中键 ccw=滚轮下)。 */
int key_config_eckey_valid(const key_config_t *k, int rotation);  /* rotation: 1=旋转键 0=按下键 */
const key_config_t *key_config_get_ec_cw_key(void);
const key_config_t *key_config_get_ec_press_key(void);
const key_config_t *key_config_get_ec_ccw_key(void);
/* 09-05: C 键三手势(双击/长按)。合法动作 = NONE/键盘/多媒体 + MOUSE 键码 1..6
 * (与 L 键一致; 不允许 AIRMOUSE —— 空中鼠标开关归 L 键/EC)。非法返回 NULL,
 * main.c 对 NULL 按"未设置"处理(不触发, 且该手势不参与手势模式判定)。
 * idx: 0=C1 1=C2 2=C3。 */
int key_config_ckey_valid(const key_config_t *k);
const key_config_t *key_config_get_c_dbl_key(uint8_t idx);   /* 双击动作; 未设置/非法 -> NULL */
const key_config_t *key_config_get_c_lng_key(uint8_t idx);   /* 长按动作; 未设置/非法 -> NULL */
const key_config_t *key_config_get_c_tap_key(uint8_t idx);   /* 手势模式"单击"动作(独立于 keys[] 常规键); 未设置/非法 -> NULL */
/* 09-07: C 键显式模式(上位机落盘)。返回 0=常规(直通) 1=手势。
 * 0xFF(旧固件数据未写此字段) -> 按旧规则派生: 双击/长按任一配了真实动作
 * (非 NONE) -> 手势; 都未配 -> 常规。每次调用实时校验, 运行中被旧上位机
 * 误写 0xFF 也安全。 */
uint8_t key_config_get_c_mode(uint8_t idx);   /* idx: 0=C1 1=C2 2=C3 */
/* 09-07: L1/L2/L3 模式 + 三手势 —— 与 C 键同架构(上位机"模式"下拉落盘)。
 * idx: 0=L1 1=L2 2=L3。合法动作 = C 键手势集 + (L1 额外允许 AIRMOUSE);
 * 非法/未设置 -> NULL(调用方不触发), mode 0xFF(旧数据) -> 按 dbl/lng 真实动作派生。 */
int key_config_lgesture_valid(const key_config_t *k, int allow_airmouse); /* 1=L1(手势可含空中鼠标) */
uint8_t key_config_get_l_mode(uint8_t idx);   /* 0=常规 1=手势; 0xFF旧数据→派生 */
const key_config_t *key_config_get_l_dbl_key(uint8_t idx);
const key_config_t *key_config_get_l_lng_key(uint8_t idx);
const key_config_t *key_config_get_l_tap_key(uint8_t idx);   /* 手势模式"单击"动作(独立于 l*_key 常规键) */
/* 09-07: EC 按下【模式 + 三手势】—— 与 C/L 键同架构。手势合法动作 = EC 常规键
 * 同集(NONE/键盘/多媒体/MOUSE 键码 1..6 + AIRMOUSE —— 空中鼠标每触发切换一次)。
 * mode 0xFF(旧固件只写 276B, 本区恒擦除态) -> 按 dbl/lng 真实动作派生(同 C/L)。 */
int key_config_ecpressgesture_valid(const key_config_t *k);   /* EC 按下手势块合法性(0/1) */
uint8_t key_config_get_ec_press_mode(void);                   /* 0=常规(直通) 1=手势; 0xFF 旧数据 -> 派生 */
const key_config_t *key_config_get_ec_press_dbl_key(void);    /* 按下·双击; 未设置/非法 -> NULL */
const key_config_t *key_config_get_ec_press_lng_key(void);    /* 按下·长按; 未设置/非法 -> NULL */
const key_config_t *key_config_get_ec_press_tap_key(void);    /* 按下·手势单击(独立于 ec_press_key); 未设置/非法 -> NULL */
/* 10-05: EC 按压滚动上/下(手势模式"按住旋钮旋转")。合法集同 EC 旋转键
 * (key_config_eckey_valid rotation=1)。⚠️ 未写入(旧数据 0xFF)/非法时
 * 【回落对应的常规滚动键】(ec_cw_key / ec_ccw_key) —— 保证"按住旋转"仍有动作,
 * 不会出现"按住旋转反而什么都不做"的退化; 显式 action=NONE 则尊重用户选择。 */
const key_config_t *key_config_get_ec_cw_press_key(void);     /* 按压滚动·上; 未设置 -> 回落 ec_cw_key */
const key_config_t *key_config_get_ec_ccw_press_key(void);    /* 按压滚动·下; 未设置 -> 回落 ec_ccw_key */
void key_config_apply_rgb(void);   /* 把 3 颗 WS2812B 置为熄灭（底光为瞬时指示，空闲/上电/写配置后全灭）*/

void key_init(void);
void key_reinit_after_deep(void);  /* 深睡唤醒后重建按键 pin+IRQ（供 power_resume_from_deep 调用）*/
void key_enable_deep_wakeup(void);  /* 进 DEEP 前使能按键 AON per-pin 下降沿唤醒（7 键均有通道 PIN0-20=PA24-44）*/
void key_disable_deep_wakeup(void); /* 唤醒后关闭按键 AON per-pin 唤醒（交还 HPSYS GPIO 中断）*/

#endif
