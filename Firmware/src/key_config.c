#include "rtthread.h"
#include "bf0_hal.h"
#include "bf0_hal_aon.h"        /* HAL_HPAON_EnableWakeupSrc / DisableWakeupSrc / AON_PIN_MODE_NEG_EDGE */
#include "drv_io.h"             /* HAL_PIN_Set / PAD_PAxx / GPIO_Axx / PIN_PULLUP */
#include "drv_flash.h"
#include "key_config.h"
#include "bt_slot.h"            /* bt_multi_get_slot: 当前蓝牙槽位(08-27 每槽独立键配置) */
#include "ws2812b.h"
#include <string.h>

rt_mailbox_t g_button_event_mb;

extern volatile uint8_t g_key2_active;

/* 每蓝牙槽位一份独立键配置(08-27): g_key_config[0..2] = 槽1..3 */
static key_config_storage_t g_key_config[KEY_CONFIG_SLOT_NUM];

/* 槽位 flash 地址(0-based): 槽0=旧地址(继承旧配置), 槽1/2 跳过 BT_SLOT(0x12629000) */
static uint32_t key_config_slot_addr(uint8_t slot)
{
    if (slot == 0) return KEY_CONFIG_FLASH_ADDR;
    return KEY_CONFIG_FLASH_ADDR + (slot + 1) * KEY_CONFIG_SLOT_STRIDE;
}

/* 槽2/槽3 默认(08-31)：C1 = LCtrl+LShift+LWin(0x0B) */
static const key_config_storage_t default_config = {
    .magic = KEY_CONFIG_MAGIC,
    .version = KEY_CONFIG_VERSION,
    .num_keys = KEY_CONFIG_NUM_KEYS,
    .air_mouse_mode = AIR_MOUSE_MODE_HOLD,       /* 默认：按住 L1 移动，松开停止 */
    .air_mouse_speed = AIR_MOUSE_SPEED_MEDIUM,   /* 默认：中速 */
    .sleep_min = KEY_CONFIG_SLEEP_MIN_DEFAULT,   /* 默认：45 分钟空闲休眠(08-27) */
    .air_mouse_dir = KEY_CONFIG_AIR_MOUSE_DIR_DEFAULT, /* 默认：正常方向(09-01) */
    /* 09-03 摇一摇: 默认【关闭】(新功能不主动改变既有使用习惯);
     * 快捷键留空(action=KEY_ACTION_NONE) —— 用户在上位机打开开关时必须先捕获一个键,
     * UI 会阻止"开了开关却没设键"的空配置下发。 */
    .shake_enabled = KEY_CONFIG_SHAKE_OFF,
    .shake_sens = KEY_CONFIG_SHAKE_SENS_DEFAULT,
    .shake_key = { KEY_ACTION_NONE, 0x00, 0x00, 0, 0, 0x20, 0xA0, 0xFF, 15 },
    /* 09-05: L2/L3 默认 = 历史硬编码行为(鼠标左/右键, action=MOUSE + 按键位)。
     * rgb 字段存而不用(L2/L3 无 WS2812B)。 */
    .l2_key = { KEY_ACTION_MOUSE, 0x00, 0x01, 0, 0, 0, 0, 0, 0 },
    .l3_key = { KEY_ACTION_MOUSE, 0x00, 0x02, 0, 0, 0, 0, 0, 0 },
    .l1_key = { KEY_ACTION_AIRMOUSE, 0x00, 0x00, 0, 0, 0, 0, 0, 0 },
    /* EC 编码器(09-05): 顺时针=滚轮上(伪键码 0x05) 按下=中键 逆时针=滚轮下(0x06) */
    .ec_cw_key = { KEY_ACTION_MOUSE, 0x00, 0x05, 0, 0, 0, 0, 0, 0 },
    .ec_press_key = { KEY_ACTION_MOUSE, 0x00, 0x04, 0, 0, 0, 0, 0, 0 },
    .ec_ccw_key = { KEY_ACTION_MOUSE, 0x00, 0x06, 0, 0, 0, 0, 0, 0 },
    /* 10-05 出厂默认 = 【手势模式】(mode=1):
     *   单击 = 鼠标中键(与常规模式体验一致, 老用户无感);
     *   双击/长按 = 未设置(NONE), 留给用户自定义。 */
    .ec_press_mode = 1,
    .ec_press_dbl_key = { KEY_ACTION_NONE, 0x00, 0x00, 0, 0, 0, 0, 0, 0 },
    .ec_press_lng_key = { KEY_ACTION_NONE, 0x00, 0x00, 0, 0, 0, 0, 0, 0 },
    .ec_press_tap_key = { KEY_ACTION_MOUSE, 0x00, 0x04, 0, 0, 0, 0, 0, 0 },
    /* 10-05 按压滚动(按住旋钮旋转) 出厂默认 = LCtrl+[ / LCtrl+]:
     *   KEY_ACTION_KEYBOARD, modifier=0x01(LCtrl), keycode=0x2F('[') / 0x30(']')。
     *   键码依据 HID Usage ID(0x2F=[ 0x30=]), 与上位机 Main.qml 的 hidKeyName 表一致
     *   ([47,"["] [48,"]"])、hidModifierName 的 0x01->LCtrl 一致, 显示为 "LCtrl+[ / LCtrl+]"。 */
    .ec_cw_press_key = { KEY_ACTION_KEYBOARD, 0x01, 0x2F, 0, 0, 0, 0, 0, 0 },
    .ec_ccw_press_key = { KEY_ACTION_KEYBOARD, 0x01, 0x30, 0, 0, 0, 0, 0, 0 },
    /* 09-05: C 键双击/长按默认【未设置】(NONE) —— 保持历史"按住发送"行为不变,
     * 用户在上位机给某键配上双击/长按后该键才进入手势模式。 */
    .c1_dbl_key = { KEY_ACTION_NONE, 0x00, 0x00, 0, 0, 0, 0, 0, 0 },
    .c1_lng_key = { KEY_ACTION_NONE, 0x00, 0x00, 0, 0, 0, 0, 0, 0 },
    .c2_dbl_key = { KEY_ACTION_NONE, 0x00, 0x00, 0, 0, 0, 0, 0, 0 },
    .c2_lng_key = { KEY_ACTION_NONE, 0x00, 0x00, 0, 0, 0, 0, 0, 0 },
    .c3_dbl_key = { KEY_ACTION_NONE, 0x00, 0x00, 0, 0, 0, 0, 0, 0 },
    .c3_lng_key = { KEY_ACTION_NONE, 0x00, 0x00, 0, 0, 0, 0, 0, 0 },
    /* 09-07: L 键模式+三手势 —— 默认【常规】(mode=0), 手势块未设置(NONE),
     * L1/L2/L3 行为与升级前完全一致(模式字段 0 由省略初始化保证, 这里显式写出)。 */
    .l1_mode = 0, .l2_mode = 0, .l3_mode = 0,
    .l1_dbl_key = { KEY_ACTION_NONE, 0x00, 0x00, 0, 0, 0, 0, 0, 0 },
    .l1_lng_key = { KEY_ACTION_NONE, 0x00, 0x00, 0, 0, 0, 0, 0, 0 },
    .l2_dbl_key = { KEY_ACTION_NONE, 0x00, 0x00, 0, 0, 0, 0, 0, 0 },
    .l2_lng_key = { KEY_ACTION_NONE, 0x00, 0x00, 0, 0, 0, 0, 0, 0 },
    .l3_dbl_key = { KEY_ACTION_NONE, 0x00, 0x00, 0, 0, 0, 0, 0, 0 },
    .l3_lng_key = { KEY_ACTION_NONE, 0x00, 0x00, 0, 0, 0, 0, 0, 0 },
    .keys = {
        /* C1: 仅修饰键 LCtrl+LShift+LWin (0x0B)，无主键(0x00)；底光清亮蓝 0x20A0FF 亮度15% */
        { KEY_ACTION_KEYBOARD, 0x0B, 0x00, 0, 1, 0x20, 0xA0, 0xFF, 15 },
        /* C2: Enter (0x28)；底光清亮蓝 0x20A0FF 亮度15% */
        { KEY_ACTION_KEYBOARD, 0x00, 0x28, 0, 1, 0x20, 0xA0, 0xFF, 15 },
        /* C3: Backspace (0x2A)；底光清亮蓝 0x20A0FF 亮度15% */
        { KEY_ACTION_KEYBOARD, 0x00, 0x2A, 0, 1, 0x20, 0xA0, 0xFF, 15 },
    },
};

/* 槽1 默认(08-31)：仅 C1 与槽2/3 不同 —— C1 = 右Alt。
 * 其余(C2=Enter、C3=Backspace、空中鼠标、休眠)与 default_config 完全一致。
 *
 * 键码依据：右Alt 在 HID 里是【修饰键】(modifier bit6 = 0x40)，不是主键码。
 * 上位机 Main.qml 的 hidModifierName() 正是按 0x40 解码成 "RAlt"，其 hidKeyName()
 * 键码表里也根本没有 100(0x64) 这一项（会显示成 "?"）。故必须写成
 * modifier=0x40、keycode=0x00，与 C1 原有的"纯修饰键"形式(0x0B/0x00)保持一致。 */
static const key_config_storage_t default_config_slot1 = {
    .magic = KEY_CONFIG_MAGIC,
    .version = KEY_CONFIG_VERSION,
    .num_keys = KEY_CONFIG_NUM_KEYS,
    .air_mouse_mode = AIR_MOUSE_MODE_HOLD,
    .air_mouse_speed = AIR_MOUSE_SPEED_MEDIUM,
    .sleep_min = KEY_CONFIG_SLEEP_MIN_DEFAULT,
    .air_mouse_dir = KEY_CONFIG_AIR_MOUSE_DIR_DEFAULT,
    .shake_enabled = KEY_CONFIG_SHAKE_OFF,
    .shake_sens = KEY_CONFIG_SHAKE_SENS_DEFAULT,
    .shake_key = { KEY_ACTION_NONE, 0x00, 0x00, 0, 0, 0x20, 0xA0, 0xFF, 15 },
    .l2_key = { KEY_ACTION_MOUSE, 0x00, 0x01, 0, 0, 0, 0, 0, 0 },
    .l3_key = { KEY_ACTION_MOUSE, 0x00, 0x02, 0, 0, 0, 0, 0, 0 },
    .l1_key = { KEY_ACTION_AIRMOUSE, 0x00, 0x00, 0, 0, 0, 0, 0, 0 },
    .ec_cw_key = { KEY_ACTION_MOUSE, 0x00, 0x05, 0, 0, 0, 0, 0, 0 },
    .ec_press_key = { KEY_ACTION_MOUSE, 0x00, 0x04, 0, 0, 0, 0, 0, 0 },
    .ec_ccw_key = { KEY_ACTION_MOUSE, 0x00, 0x06, 0, 0, 0, 0, 0, 0 },
    /* 10-05 出厂默认 = 【手势模式】(mode=1):
     *   单击 = 鼠标中键(与常规模式体验一致, 老用户无感);
     *   双击/长按 = 未设置(NONE), 留给用户自定义。 */
    .ec_press_mode = 1,
    .ec_press_dbl_key = { KEY_ACTION_NONE, 0x00, 0x00, 0, 0, 0, 0, 0, 0 },
    .ec_press_lng_key = { KEY_ACTION_NONE, 0x00, 0x00, 0, 0, 0, 0, 0, 0 },
    .ec_press_tap_key = { KEY_ACTION_MOUSE, 0x00, 0x04, 0, 0, 0, 0, 0, 0 },
    /* 10-05 按压滚动(按住旋钮旋转) 出厂默认 = LCtrl+[ / LCtrl+]:
     *   KEY_ACTION_KEYBOARD, modifier=0x01(LCtrl), keycode=0x2F('[') / 0x30(']')。
     *   键码依据 HID Usage ID(0x2F=[ 0x30=]), 与上位机 Main.qml 的 hidKeyName 表一致
     *   ([47,"["] [48,"]"])、hidModifierName 的 0x01->LCtrl 一致, 显示为 "LCtrl+[ / LCtrl+]"。 */
    .ec_cw_press_key = { KEY_ACTION_KEYBOARD, 0x01, 0x2F, 0, 0, 0, 0, 0, 0 },
    .ec_ccw_press_key = { KEY_ACTION_KEYBOARD, 0x01, 0x30, 0, 0, 0, 0, 0, 0 },
    .c1_dbl_key = { KEY_ACTION_NONE, 0x00, 0x00, 0, 0, 0, 0, 0, 0 },
    .c1_lng_key = { KEY_ACTION_NONE, 0x00, 0x00, 0, 0, 0, 0, 0, 0 },
    .c2_dbl_key = { KEY_ACTION_NONE, 0x00, 0x00, 0, 0, 0, 0, 0, 0 },
    .c2_lng_key = { KEY_ACTION_NONE, 0x00, 0x00, 0, 0, 0, 0, 0, 0 },
    .c3_dbl_key = { KEY_ACTION_NONE, 0x00, 0x00, 0, 0, 0, 0, 0, 0 },
    .c3_lng_key = { KEY_ACTION_NONE, 0x00, 0x00, 0, 0, 0, 0, 0, 0 },
    .l1_mode = 0, .l2_mode = 0, .l3_mode = 0,
    .l1_dbl_key = { KEY_ACTION_NONE, 0x00, 0x00, 0, 0, 0, 0, 0, 0 },
    .l1_lng_key = { KEY_ACTION_NONE, 0x00, 0x00, 0, 0, 0, 0, 0, 0 },
    .l2_dbl_key = { KEY_ACTION_NONE, 0x00, 0x00, 0, 0, 0, 0, 0, 0 },
    .l2_lng_key = { KEY_ACTION_NONE, 0x00, 0x00, 0, 0, 0, 0, 0, 0 },
    .l3_dbl_key = { KEY_ACTION_NONE, 0x00, 0x00, 0, 0, 0, 0, 0, 0 },
    .l3_lng_key = { KEY_ACTION_NONE, 0x00, 0x00, 0, 0, 0, 0, 0, 0 },
    .keys = {
        /* C1: 右Alt（修饰键 0x40），无主键 */
        { KEY_ACTION_KEYBOARD, 0x40, 0x00, 0, 1, 0x20, 0xA0, 0xFF, 15 },
        /* C2: Enter (0x28) */
        { KEY_ACTION_KEYBOARD, 0x00, 0x28, 0, 1, 0x20, 0xA0, 0xFF, 15 },
        /* C3: Backspace (0x2A) */
        { KEY_ACTION_KEYBOARD, 0x00, 0x2A, 0, 1, 0x20, 0xA0, 0xFF, 15 },
    },
};

/* 指定槽(0~2)的默认配置：槽1 用 default_config_slot1，槽2/3 用 default_config */
static const key_config_storage_t *key_config_defaults(uint8_t slot)
{
    return (slot == 0) ? &default_config_slot1 : &default_config;
}

/* ---- 编译期布局断言(09-03) ----
 * ⚠️ key_config_storage_t 的布局同时被三处硬编码依赖, 改动任一处都容易静默错位:
 *   ① READ 回包 = 结构体内存布局(含 padding), 上位机按 offset 36/38/39/40/41 解析;
 *   ② WRITE payload = 紧凑布局(无 padding), 偏移与①不同, 见 dfu_device.c;
 *   ③ flash 落盘 = 整个结构体裸写, 旧数据尾部是 0xFF 擦除态(靠兜底迁移)。
 * 这里把 sizeof 与关键 offset 钉死: 以后在结构体中间插字段 / 改对齐 / 换编译器,
 * 会直接在编译期报错, 而不是变成"上位机读到的数字全是错的"这种极难排查的故障。
 * ⚠️ 新增字段时必须同步更新这三处和本断言。 */
#include <stddef.h>
_Static_assert(sizeof(key_config_t) == 9,
               "key_config_t must be 9B (上位机按 9B/键 解析 payload 与回包)");
/* 09-05: 由 52 → 104(尾部追加 L2/L3/L1/EC 三键 各 9B = 45B, 0..49 布局不变)
 *        由 104 → 160(尾部追加 C 键三手势 6×9B = 54B + 显式尾 padding 2B,
 *        0..103 布局不变; 结构体含 uint32_t 需 4B 对齐, 158 会隐式补齐到 160)
 * 09-07: 由 160 → 164(C 键显式模式 3B 追加在 reserved_tail【之后】的 160..162,
 *        0..159 布局不变; 旧固件写的 160B 槽页在 160..163 恒为擦除态 0xFF,
 *        key_config_get_c_mode 据此做旧数据派生兜底, 见结构体注释)
 *        由 164 → 192(手势"单击"独立块 c*_tap_key 3×9B 追加在 164..190,
 *        0..163 布局不变, 尾 pad@191)
 * 09-07: 由 192 → 276(L1/L2/L3 模式 3B@192..194 + 手势 dbl/lng/tap 9×9B
 *        @195..275, 0..191 布局不变 —— 全在旧 192B 写界之外, 旧槽页此区恒 0xFF,
 *        key_config_get_l_mode 据此派生兜底, 见结构体注释)
 * 09-07: 由 276 → 304(EC 按下模式 1B@276 + 手势 dbl/lng/tap 3×9B
 *        @277..303, 0..275 布局不变 —— 全在旧 276B 写界之外, 旧槽页此区恒 0xFF,
 *        key_config_get_ec_press_mode 据此派生兜底, 见结构体注释)
 * 10-05: 由 304 → 324(EC 按压滚动 ec_cw_press@304..312 ec_ccw_press@313..321,
 *        0..303 布局不变 —— 全在旧 304B 写界之外, 旧槽页此区恒 0xFF,
 *        两个 getter 据此回落对应的常规滚动键, 见结构体注释;
 *        322 非 4 的倍数 -> 显式 reserved_ecscroll_pad[2] 补到 324) */
_Static_assert(sizeof(key_config_storage_t) == 324,
               "key_config_storage_t must be 324B (READ 回包长度 = sizeof, 上位机按 324 读)");
_Static_assert(offsetof(key_config_storage_t, sleep_min) == 36,
               "sleep_min offset 36 (uint16 需 2 对齐, keys[3] 后插了 1B padding)");
_Static_assert(offsetof(key_config_storage_t, air_mouse_dir) == 38,
               "air_mouse_dir offset 38");
_Static_assert(offsetof(key_config_storage_t, shake_enabled) == 39,
               "shake_enabled offset 39 (吃掉 air_mouse_dir 后的 1B padding)");
_Static_assert(offsetof(key_config_storage_t, shake_sens) == 40,
               "shake_sens offset 40");
_Static_assert(offsetof(key_config_storage_t, shake_key) == 41,
               "shake_key offset 41 (key_config_t 全 uint8_t, 无对齐要求)");
/* 09-05: L2/L3 尾部追加 —— offset 0..49 不得变动(旧 flash 数据兼容), 新字段紧随其后 */
_Static_assert(offsetof(key_config_storage_t, l2_key) == 50,
               "l2_key offset 50 (紧跟 shake_key 尾部, 旧数据 0..49 兼容)");
_Static_assert(offsetof(key_config_storage_t, l3_key) == 59,
               "l3_key offset 59 (l2_key + 9B)");
_Static_assert(offsetof(key_config_storage_t, l1_key) == 68,
               "l1_key offset 68 (l3_key + 9B)");
_Static_assert(offsetof(key_config_storage_t, ec_cw_key) == 77,
               "ec_cw_key offset 77 (l1_key + 9B)");
_Static_assert(offsetof(key_config_storage_t, ec_press_key) == 86,
               "ec_press_key offset 86 (ec_cw_key + 9B)");
_Static_assert(offsetof(key_config_storage_t, ec_ccw_key) == 95,
               "ec_ccw_key offset 95 (ec_press_key + 9B)");
/* 09-05: C 键三手势尾部追加 —— offset 0..103 不得变动(旧 flash 数据兼容) */
_Static_assert(offsetof(key_config_storage_t, c1_dbl_key) == 104,
               "c1_dbl_key offset 104 (紧跟 ec_ccw_key 尾部, 旧数据 0..103 兼容)");
_Static_assert(offsetof(key_config_storage_t, c1_lng_key) == 113,
               "c1_lng_key offset 113 (c1_dbl_key + 9B)");
_Static_assert(offsetof(key_config_storage_t, c2_dbl_key) == 122,
               "c2_dbl_key offset 122 (c1_lng_key + 9B)");
_Static_assert(offsetof(key_config_storage_t, c2_lng_key) == 131,
               "c2_lng_key offset 131 (c2_dbl_key + 9B)");
_Static_assert(offsetof(key_config_storage_t, c3_dbl_key) == 140,
               "c3_dbl_key offset 140 (c2_lng_key + 9B)");
_Static_assert(offsetof(key_config_storage_t, c3_lng_key) == 149,
               "c3_lng_key offset 149 (c3_dbl_key + 9B)");
/* 09-07: C 键显式模式 —— 追加在 reserved_tail(158..159)【之后】, 0..159 不变 */
_Static_assert(offsetof(key_config_storage_t, c1_mode) == 160,
               "c1_mode offset 160 (旧固件 160B 写界之外, 旧数据必为 0xFF)");
_Static_assert(offsetof(key_config_storage_t, c2_mode) == 161,
               "c2_mode offset 161 (c1_mode + 1B)");
_Static_assert(offsetof(key_config_storage_t, c3_mode) == 162,
               "c3_mode offset 162 (c2_mode + 1B)");
/* 09-07: 手势"单击"独立块 —— 追加在模式字段之后, 0..163 不变 */
_Static_assert(offsetof(key_config_storage_t, c1_tap_key) == 164,
               "c1_tap_key offset 164 (紧跟 reserved_mode_pad@163 之后)");
_Static_assert(offsetof(key_config_storage_t, c2_tap_key) == 173,
               "c2_tap_key offset 173 (c1_tap_key + 9B)");
_Static_assert(offsetof(key_config_storage_t, c3_tap_key) == 182,
               "c3_tap_key offset 182 (c2_tap_key + 9B)");
/* 09-07: L1/L2/L3 模式+三手势 —— 追加在 192B 写界之外, 0..191 不变 */
_Static_assert(offsetof(key_config_storage_t, l1_mode) == 192,
               "l1_mode offset 192 (旧固件 192B 写界之外, 旧数据必为 0xFF)");
_Static_assert(offsetof(key_config_storage_t, l2_mode) == 193,
               "l2_mode offset 193 (l1_mode + 1B)");
_Static_assert(offsetof(key_config_storage_t, l3_mode) == 194,
               "l3_mode offset 194 (l2_mode + 1B)");
_Static_assert(offsetof(key_config_storage_t, l1_dbl_key) == 195,
               "l1_dbl_key offset 195 (紧跟模式字节之后)");
_Static_assert(offsetof(key_config_storage_t, l1_lng_key) == 204,
               "l1_lng_key offset 204 (l1_dbl_key + 9B)");
_Static_assert(offsetof(key_config_storage_t, l2_dbl_key) == 213,
               "l2_dbl_key offset 213 (l1_lng_key + 9B)");
_Static_assert(offsetof(key_config_storage_t, l2_lng_key) == 222,
               "l2_lng_key offset 222 (l2_dbl_key + 9B)");
_Static_assert(offsetof(key_config_storage_t, l3_dbl_key) == 231,
               "l3_dbl_key offset 231 (l2_lng_key + 9B)");
_Static_assert(offsetof(key_config_storage_t, l3_lng_key) == 240,
               "l3_lng_key offset 240 (l3_dbl_key + 9B)");
_Static_assert(offsetof(key_config_storage_t, l1_tap_key) == 249,
               "l1_tap_key offset 249 (紧跟 l3_lng_key 尾部)");
_Static_assert(offsetof(key_config_storage_t, l2_tap_key) == 258,
               "l2_tap_key offset 258 (l1_tap_key + 9B)");
_Static_assert(offsetof(key_config_storage_t, l3_tap_key) == 267,
               "l3_tap_key offset 267 (l2_tap_key + 9B)");
/* 09-07: EC 按下模式+三手势 —— 追加在 276B 写界之外, 0..275 不变 */
_Static_assert(offsetof(key_config_storage_t, ec_press_mode) == 276,
               "ec_press_mode offset 276 (旧固件 276B 写界之外, 旧数据必为 0xFF)");
_Static_assert(offsetof(key_config_storage_t, ec_press_dbl_key) == 277,
               "ec_press_dbl_key offset 277 (紧跟模式字节之后)");
_Static_assert(offsetof(key_config_storage_t, ec_press_lng_key) == 286,
               "ec_press_lng_key offset 286 (ec_press_dbl_key + 9B)");
_Static_assert(offsetof(key_config_storage_t, ec_press_tap_key) == 295,
               "ec_press_tap_key offset 295 (ec_press_lng_key + 9B)");
/* 10-05: EC 按压滚动 —— 追加在 304B 写界之外, 0..303 不变 */
_Static_assert(offsetof(key_config_storage_t, ec_cw_press_key) == 304,
               "ec_cw_press_key offset 304 (旧固件 304B 写界之外, 旧数据必为 0xFF)");
_Static_assert(offsetof(key_config_storage_t, ec_ccw_press_key) == 313,
               "ec_ccw_press_key offset 313 (ec_cw_press_key + 9B)");
_Static_assert(offsetof(key_config_storage_t, reserved_ecscroll_pad) == 322,
               "reserved_ecscroll_pad offset 322 (ec_ccw_press_key + 9B, 补 2B 到 324)");

void key_config_init(void)
{
    for (int s = 0; s < KEY_CONFIG_SLOT_NUM; s++)
    {
        uint32_t addr = key_config_slot_addr((uint8_t)s);
        rt_flash_read(addr, (uint8_t *)&g_key_config[s], sizeof(key_config_storage_t));
        if (g_key_config[s].magic != KEY_CONFIG_MAGIC || g_key_config[s].version != KEY_CONFIG_VERSION)
        {
            rt_kprintf("[KEYCONFIG] slot%d invalid magic, loading defaults\n", s + 1);
            memcpy(&g_key_config[s], key_config_defaults((uint8_t)s), sizeof(g_key_config[s]));
        }
        else
        {
            /* 08-27: 旧固件(无 sleep_min 字段)升级后此处读到 0xFFFF(擦除态),
             * 识别为"未设置" -> 用默认 45 分钟。0 是合法值=永不, 不覆盖。 */
            if (g_key_config[s].sleep_min == 0xFFFF ||
                g_key_config[s].sleep_min > KEY_CONFIG_SLEEP_MIN_MAX)
            {
                g_key_config[s].sleep_min = KEY_CONFIG_SLEEP_MIN_DEFAULT;
            }
            /* 09-01: 旧固件 flash 数据无 air_mouse_dir 字段 -> 读到 0xFF(擦除态)
             * 或残留值, 识别为"未设置" -> 用默认(0° 正常方向)。与 sleep_min 同策略:
             * 不升 KEY_CONFIG_VERSION(避免清掉用户全部自定义配置)。
             * (09-01: 枚举改角度语义, 0~3 数值不变, 兜底判断只认范围不认名字) */
            if (g_key_config[s].air_mouse_dir > AIR_MOUSE_DIR_270)
            {
                g_key_config[s].air_mouse_dir = KEY_CONFIG_AIR_MOUSE_DIR_DEFAULT;
            }
            /* 09-03: 摇一摇三字段 —— 旧 flash 数据无这些字段, 读到 0xFF(擦除态)
             * 或残留值。与 sleep_min / air_mouse_dir 同策略(不升 version, 避免
             * 清掉用户全部自定义): 非法值识别为"未设置" -> 填默认值。
             * shake_key 逐字段兜底: action_type 只认 NONE/KETBOARD/MULTIMEDIA,
             * 其余(含 0xFF)整个键重置为"未设置"(KEY_ACTION_NONE) —— 否则 0xFF
             * 会被当成"鼠标动作"落到 switch 的 default 分支, 表现为摇了没反应。 */
            if (g_key_config[s].shake_enabled != KEY_CONFIG_SHAKE_ON)
            {
                g_key_config[s].shake_enabled = KEY_CONFIG_SHAKE_OFF;
            }
            if (g_key_config[s].shake_sens > SHAKE_SENS_STRONG)
            {
                g_key_config[s].shake_sens = KEY_CONFIG_SHAKE_SENS_DEFAULT;
            }
            {
                key_config_t *sk = &g_key_config[s].shake_key;
                if (sk->action_type != KEY_ACTION_NONE &&
                    sk->action_type != KEY_ACTION_KEYBOARD &&
                    sk->action_type != KEY_ACTION_MULTIMEDIA)
                {
                    sk->action_type   = KEY_ACTION_NONE;
                    sk->modifier      = 0x00;
                    sk->keycode       = 0x00;
                    sk->reserved      = 0;
                    sk->rgb_enabled   = 0;
                    sk->rgb_r         = 0x20;
                    sk->rgb_g         = 0xA0;
                    sk->rgb_b         = 0xFF;
                    sk->rgb_brightness = 15;
                }
            }
            /* 09-05: L2/L3 —— 旧 flash 数据无这两个字段(读到 0xFF 擦除态/残留值),
             * 兜底为"鼠标键默认位"(L2=左 L3=右, 即历史硬编码行为), 升级后行为不变。
             * L1 同策略, 兜底为"空中鼠标开关"。 */
            {
                static const key_config_t l2_def = { KEY_ACTION_MOUSE, 0x00, 0x01, 0, 0, 0, 0, 0, 0 };
                static const key_config_t l3_def = { KEY_ACTION_MOUSE, 0x00, 0x02, 0, 0, 0, 0, 0, 0 };
                static const key_config_t l1_def = { KEY_ACTION_AIRMOUSE, 0x00, 0x00, 0, 0, 0, 0, 0, 0 };
                if (!key_config_lkey_valid(&g_key_config[s].l2_key))
                    memcpy(&g_key_config[s].l2_key, &l2_def, sizeof(l2_def));
                if (!key_config_lkey_valid(&g_key_config[s].l3_key))
                    memcpy(&g_key_config[s].l3_key, &l3_def, sizeof(l3_def));
                if (!key_config_l1key_valid(&g_key_config[s].l1_key))
                    memcpy(&g_key_config[s].l1_key, &l1_def, sizeof(l1_def));
                /* EC 编码器: 兜底为历史硬编码行为(滚轮上/中键/滚轮下) */
                {
                    static const key_config_t ec_cw_def = { KEY_ACTION_MOUSE, 0x00, 0x05, 0, 0, 0, 0, 0, 0 };
                    static const key_config_t ec_pr_def = { KEY_ACTION_MOUSE, 0x00, 0x04, 0, 0, 0, 0, 0, 0 };
                    static const key_config_t ec_cc_def = { KEY_ACTION_MOUSE, 0x00, 0x06, 0, 0, 0, 0, 0, 0 };
                    if (!key_config_eckey_valid(&g_key_config[s].ec_cw_key, 1))
                        memcpy(&g_key_config[s].ec_cw_key, &ec_cw_def, sizeof(ec_cw_def));
                    if (!key_config_eckey_valid(&g_key_config[s].ec_press_key, 0))
                        memcpy(&g_key_config[s].ec_press_key, &ec_pr_def, sizeof(ec_pr_def));
                    if (!key_config_eckey_valid(&g_key_config[s].ec_ccw_key, 1))
                        memcpy(&g_key_config[s].ec_ccw_key, &ec_cc_def, sizeof(ec_cc_def));
                }
                /* 09-05: C 键三手势 —— 旧 flash 数据无这 6 个字段(读到 0xFF 擦除态/
                 * 残留值), 兜底为"未设置"(NONE)。默认就是未设置 -> 不必 memcpy 默认块,
                 * 逐字段清零即可(该键保持历史"按住发送"行为, 升级后行为不变)。
                 * 09-07: 手势"单击"独立块 c*_tap_key 同策略清零(0xFF -> NONE, 运行期
                 * getter 还会按 action==NONE 视为未设置, 双保险)。 */
                {
                    key_config_t *cg[9] = {
                        &g_key_config[s].c1_dbl_key, &g_key_config[s].c1_lng_key,
                        &g_key_config[s].c2_dbl_key, &g_key_config[s].c2_lng_key,
                        &g_key_config[s].c3_dbl_key, &g_key_config[s].c3_lng_key,
                        &g_key_config[s].c1_tap_key, &g_key_config[s].c2_tap_key,
                        &g_key_config[s].c3_tap_key,
                    };
                    for (int i = 0; i < 9; i++)
                    {
                        if (!key_config_ckey_valid(cg[i]))
                            memset(cg[i], 0, sizeof(key_config_t));   /* action_type=0 即 NONE */
                    }
                }
                /* 09-07: L1/L2/L3 手势块 —— 与 C 键同策略清零。L1(idx0) 允许
                 * AIRMOUSE(用户要求手势里可开关空中鼠标), L2/L3 不允许。 */
                {
                    key_config_t *lg[9] = {
                        &g_key_config[s].l1_dbl_key, &g_key_config[s].l1_lng_key,
                        &g_key_config[s].l2_dbl_key, &g_key_config[s].l2_lng_key,
                        &g_key_config[s].l3_dbl_key, &g_key_config[s].l3_lng_key,
                        &g_key_config[s].l1_tap_key, &g_key_config[s].l2_tap_key,
                        &g_key_config[s].l3_tap_key,
                    };
                    const int lair[9] = { 1, 1, 0, 0, 0, 0, 1, 0, 0 };  /* 1=L1(dbl/lng/tap) */
                    for (int i = 0; i < 9; i++)
                    {
                        if (!key_config_lgesture_valid(lg[i], lair[i]))
                            memset(lg[i], 0, sizeof(key_config_t));   /* action_type=0 即 NONE */
                    }
                }
                /* 09-07: EC 按下手势块 —— 与 C/L 同策略清零(0xFF 擦除态/残留 ->
                 * NONE, 双保险: getter 还会按 NONE 视为未设置)。
                 * ⚠️ ec_press_mode【不】在此清零: 0xFF 由 key_config_get_ec_press_mode
                 * 实时按 dbl/lng 真实动作派生(与 c/l 模式同策略 —— 旧数据若已配
                 * 真实手势动作会被正确判成手势模式, 而不是被静默打成常规)。 */
                {
                    key_config_t *eg[3] = {
                        &g_key_config[s].ec_press_dbl_key,
                        &g_key_config[s].ec_press_lng_key,
                        &g_key_config[s].ec_press_tap_key,
                    };
                    for (int i = 0; i < 3; i++)
                    {
                        if (!key_config_ecpressgesture_valid(eg[i]))
                            memset(eg[i], 0, sizeof(key_config_t));   /* action_type=0 即 NONE */
                    }
                }
            }
            rt_kprintf("[KEYCONFIG] slot%d loaded from flash, version=%d sleep_min=%d dir=%d shake=%d/%d/%d\n",
                       s + 1, g_key_config[s].version, g_key_config[s].sleep_min,
                       g_key_config[s].air_mouse_dir, g_key_config[s].shake_enabled,
                       g_key_config[s].shake_sens, g_key_config[s].shake_key.action_type);
        }
    }
    key_init();
}

/* 指定槽 0~2 的配置(CONF 协议读写任意槽用) */
key_config_storage_t *key_config_get_slot(uint8_t slot)
{
    if (slot >= KEY_CONFIG_SLOT_NUM) slot = 0;
    return &g_key_config[slot];
}

/* 当前蓝牙槽位的配置(按键处理/空中鼠标参数随槽切换) */
key_config_storage_t *key_config_get(void)
{
    return key_config_get_slot(bt_multi_get_slot() - 1);
}

/* 当前槽休眠分钟(0=永不) —— power.c 连接态空闲超时用 */
uint16_t key_config_get_sleep_min(void)
{
    key_config_storage_t *cfg = key_config_get();
    if (cfg->sleep_min == 0xFFFF || cfg->sleep_min > KEY_CONFIG_SLEEP_MIN_MAX)
        return KEY_CONFIG_SLEEP_MIN_DEFAULT;   /* 未设置/异常值兜底 */
    return cfg->sleep_min;
}

/* 当前槽空中鼠标方向(0~3) —— main.c 位移映射用(09-01)。
 * 每次调用都校验而不仅在加载时校验: 万一运行中被写入非法值(如旧上位机误写 0xFF)
 * 也能安全兜底为"正常方向", 绝不会算出未定义行为。 */
uint8_t key_config_get_air_mouse_dir(void)
{
    uint8_t dir = key_config_get()->air_mouse_dir;
    if (dir > AIR_MOUSE_DIR_270)
        return KEY_CONFIG_AIR_MOUSE_DIR_DEFAULT;   /* 未设置/异常值兜底 */
    return dir;
}

/* ---- 09-03 摇一摇 getter ----
 * ⚠️ 三个 getter 都【每次调用重新校验】, 不只依赖加载时的兜底:
 * 运行期间 CONF WRITE 可能被旧版上位机写入 0xFF(不认识新字段却把整块回写),
 * 若只在 init 兜底, 一次写入就能让摇一摇发不出键或永不触发。 */

uint8_t key_config_get_shake_enabled(void)
{
    return (key_config_get()->shake_enabled == KEY_CONFIG_SHAKE_ON) ? 1 : 0;
}

uint8_t key_config_get_shake_sens(void)
{
    uint8_t sens = key_config_get()->shake_sens;
    if (sens > SHAKE_SENS_STRONG)
        return KEY_CONFIG_SHAKE_SENS_DEFAULT;
    return sens;
}

/* 返回 NULL = 未配置合法快捷键(此时摇一摇命中也【不发包】, 只打日志) */
const key_config_t *key_config_get_shake_key(void)
{
    const key_config_t *sk = &key_config_get()->shake_key;
    if (sk->action_type != KEY_ACTION_KEYBOARD && sk->action_type != KEY_ACTION_MULTIMEDIA)
        return NULL;
    /* 键盘组合键至少要有修饰键或主键之一, 全 0 等于没设 */
    if (sk->action_type == KEY_ACTION_KEYBOARD && sk->modifier == 0x00 && sk->keycode == 0x00)
        return NULL;
    return sk;
}

/* ---- 09-05: L2/L3 侧键快捷键 ----
 * 合法性: NONE/KEYBOARD/MULTIMEDIA 直接放行; MOUSE 要求 keycode 是有效按键位
 * (1=左 2=右 4=中)。非法返回 0 —— 加载/写入路径据此兜底为默认鼠标键。 */
int key_config_lkey_valid(const key_config_t *k)
{
    switch (k->action_type)
    {
    case KEY_ACTION_NONE:
    case KEY_ACTION_KEYBOARD:
    case KEY_ACTION_MULTIMEDIA:
        return 1;
    case KEY_ACTION_MOUSE:
        /* 09-06: 1/2/4=左/右/中键, 5/6=滚轮上/下(与 EC 编码器统一) */
        return (k->keycode >= 0x01 && k->keycode <= 0x06);
    default:
        return 0;
    }
}

/* 运行期防御: 加载(key_config_init)与 CONF 写入路径已兜底, 正常不会返回 NULL;
 * main.c 据此在 NULL 时按"鼠标键默认位"处理(与历史硬编码一致)。 */
const key_config_t *key_config_get_l2_key(void)
{
    const key_config_t *k = &key_config_get()->l2_key;
    return key_config_lkey_valid(k) ? k : NULL;
}

const key_config_t *key_config_get_l3_key(void)
{
    const key_config_t *k = &key_config_get()->l3_key;
    return key_config_lkey_valid(k) ? k : NULL;
}

/* L1 合法动作 = L2/L3 集合 + KEY_ACTION_AIRMOUSE(默认, 仅 L1 允许) */
int key_config_l1key_valid(const key_config_t *k)
{
    if (k->action_type == KEY_ACTION_AIRMOUSE)
        return 1;
    return key_config_lkey_valid(k);
}

const key_config_t *key_config_get_l1_key(void)
{
    const key_config_t *k = &key_config_get()->l1_key;
    return key_config_l1key_valid(k) ? k : NULL;
}

/* EC 编码器合法性(09-06 放宽): MOUSE 键码 1/2/4=左/右/中键, 5/6=滚轮上/下 ——
 * 三手势统一允许(按下手势映射滚轮=每次按下滚一格, 实测有用); 
 * 另允许 KEYBOARD/MULTIMEDIA/NONE/AIRMOUSE(激活空中鼠标, 行为同 L1)。 */
int key_config_eckey_valid(const key_config_t *k, int rotation)
{
    (void)rotation;
    switch (k->action_type)
    {
    case KEY_ACTION_AIRMOUSE:
    case KEY_ACTION_NONE:
    case KEY_ACTION_KEYBOARD:
    case KEY_ACTION_MULTIMEDIA:
        return 1;
    case KEY_ACTION_MOUSE:
        return (k->keycode >= 0x01 && k->keycode <= 0x06);
    default:
        return 0;
    }
}

static const key_config_t *ec_key_get(const key_config_t *k, int rotation)
{
    return key_config_eckey_valid(k, rotation) ? k : NULL;
}

const key_config_t *key_config_get_ec_cw_key(void)
{
    return ec_key_get(&key_config_get()->ec_cw_key, 1);
}

const key_config_t *key_config_get_ec_press_key(void)
{
    return ec_key_get(&key_config_get()->ec_press_key, 0);
}

const key_config_t *key_config_get_ec_ccw_key(void)
{
    return ec_key_get(&key_config_get()->ec_ccw_key, 1);
}

/* 10-05: EC 按压滚动上/下(手势模式"按住旋钮旋转")。
 * 未写入(旧固件槽页 0xFF -> action_type 非法) / 非法 -> 【回落对应的常规滚动键】:
 *   保证"按住旋钮旋转"仍有动作(默认与常规滚动一致, 滚轮上/下), 不会退化成
 *   "按住旋转反而什么都不做"; 显式 action=NONE 是合法块, 照常返回 —— 尊重
 *   "按压滚动=无动作"的选择(抑制单击/双击/长按的语义不依赖本动作是否为空, 见 main.c)。 */
const key_config_t *key_config_get_ec_cw_press_key(void)
{
    const key_config_t *k = &key_config_get()->ec_cw_press_key;
    return key_config_eckey_valid(k, 1) ? k : key_config_get_ec_cw_key();
}

const key_config_t *key_config_get_ec_ccw_press_key(void)
{
    const key_config_t *k = &key_config_get()->ec_ccw_press_key;
    return key_config_eckey_valid(k, 1) ? k : key_config_get_ec_ccw_key();
}

/* ---- 09-05: C 键三手势(双击/长按) ----
 * 合法性: NONE/KEYBOARD/MULTIMEDIA 放行; MOUSE 要求 keycode 1..6(与 L 键统一,
 * 5/6=滚轮上/下); 不允许 AIRMOUSE(空中鼠标开关归 L 键/EC)。 */
int key_config_ckey_valid(const key_config_t *k)
{
    switch (k->action_type)
    {
    case KEY_ACTION_NONE:
    case KEY_ACTION_KEYBOARD:
    case KEY_ACTION_MULTIMEDIA:
        return 1;
    case KEY_ACTION_MOUSE:
        return (k->keycode >= 0x01 && k->keycode <= 0x06);
    default:
        return 0;
    }
}

static const key_config_t *c_gesture_get(const key_config_t *k)
{
    return key_config_ckey_valid(k) ? k : NULL;
}

const key_config_t *key_config_get_c_dbl_key(uint8_t idx)
{
    key_config_storage_t *cfg = key_config_get();
    const key_config_t *k;
    switch (idx)
    {
    case 0:  k = &cfg->c1_dbl_key; break;
    case 1:  k = &cfg->c2_dbl_key; break;
    case 2:  k = &cfg->c3_dbl_key; break;
    default: return NULL;
    }
    return c_gesture_get(k);
}

const key_config_t *key_config_get_c_lng_key(uint8_t idx)
{
    key_config_storage_t *cfg = key_config_get();
    const key_config_t *k;
    switch (idx)
    {
    case 0:  k = &cfg->c1_lng_key; break;
    case 1:  k = &cfg->c2_lng_key; break;
    case 2:  k = &cfg->c3_lng_key; break;
    default: return NULL;
    }
    return c_gesture_get(k);
}

/* 09-07: C 键显式模式(上位机落盘)。0=常规(直通) 1=手势。
 * 0xFF/非法(旧固件数据 —— 旧固件只写 160B, 本区恒为擦除态) -> 按旧规则派生:
 *   双击/长按任一配了【真实动作】(action != NONE, 键块合法才算) -> 手势;
 *   都未配 -> 常规。注意派生只看真实动作: 旧上位机给每个键默认写"合法 NONE"
 *   的双击/长按块, 若按"块存在"派生会误判成手势模式(按下无反应只能单击,
 *   即 09-07 用户报告的 bug 根因)。 */
uint8_t key_config_get_c_mode(uint8_t idx)
{
    if (idx > 2)
        return 0;
    key_config_storage_t *cfg = key_config_get();
    uint8_t v = 0xFF;
    const key_config_t *dbl = NULL, *lng = NULL;
    switch (idx)
    {
    case 0:  v = cfg->c1_mode; dbl = &cfg->c1_dbl_key; lng = &cfg->c1_lng_key; break;
    case 1:  v = cfg->c2_mode; dbl = &cfg->c2_dbl_key; lng = &cfg->c2_lng_key; break;
    case 2:  v = cfg->c3_mode; dbl = &cfg->c3_dbl_key; lng = &cfg->c3_lng_key; break;
    default: break;
    }
    if (v == 0 || v == 1)
        return v;
    if ((key_config_ckey_valid(dbl) && dbl->action_type != KEY_ACTION_NONE) ||
        (key_config_ckey_valid(lng) && lng->action_type != KEY_ACTION_NONE))
        return 1;   /* 旧数据: 配了真实手势动作 -> 手势模式 */
    return 0;       /* 旧数据: 都未配 -> 常规(直通) */
}

/* 09-07: 手势模式"单击"动作 —— 独立于 keys[](常规键)存储, 避免"改单击把常规键
 * 也改了"。放行规则同 ckey_valid, 且要求【真实动作】(NONE/0xFF/非法均视为未设置
 * -> NULL, 调用方点按不触发)。旧数据(0xFF)读到 NULL: 升级后单击需重新设置。 */
const key_config_t *key_config_get_c_tap_key(uint8_t idx)
{
    key_config_storage_t *cfg = key_config_get();
    const key_config_t *k;
    switch (idx)
    {
    case 0:  k = &cfg->c1_tap_key; break;
    case 1:  k = &cfg->c2_tap_key; break;
    case 2:  k = &cfg->c3_tap_key; break;
    default: return NULL;
    }
    if (!key_config_ckey_valid(k) || k->action_type == KEY_ACTION_NONE)
        return NULL;
    return k;
}

/* ---- 09-07: L1/L2/L3 模式 + 三手势(与 C 键同架构) ----
 * idx: 0=L1 1=L2 2=L3。手势合法动作 = C 键手势集(NONE/键盘/多媒体 + MOUSE
 * 键码 1..6) + 【L1 额外允许 AIRMOUSE】(用户要求; 每触发切换一次, 语义同
 * EC 旋转的空中鼠标)。L2/L3 手势不允许 AIRMOUSE(空中鼠标开关归 L1 常规/EC)。 */
int key_config_lgesture_valid(const key_config_t *k, int allow_airmouse)
{
    if (k->action_type == KEY_ACTION_AIRMOUSE)
        return allow_airmouse ? 1 : 0;
    switch (k->action_type)
    {
    case KEY_ACTION_NONE:
    case KEY_ACTION_KEYBOARD:
    case KEY_ACTION_MULTIMEDIA:
        return 1;
    case KEY_ACTION_MOUSE:
        return (k->keycode >= 0x01 && k->keycode <= 0x06);
    default:
        return 0;
    }
}

/* L1/L2/L3 显式模式。0=常规(保持现行为) 1=手势。
 * 0xFF/非法(旧固件只写 192B, 本区恒为擦除态) -> 按旧规则派生(与 C 键同策略):
 *   双击/长按任一配了【真实动作】(action != NONE 且键块合法, L1 允许 AIRMOUSE)
 *   -> 手势; 都未配 -> 常规。 */
uint8_t key_config_get_l_mode(uint8_t idx)
{
    if (idx > 2)
        return 0;
    key_config_storage_t *cfg = key_config_get();
    uint8_t v = 0xFF;
    const key_config_t *dbl = NULL, *lng = NULL;
    int air = (idx == 0);   /* L1 手势允许空中鼠标 */
    switch (idx)
    {
    case 0:  v = cfg->l1_mode; dbl = &cfg->l1_dbl_key; lng = &cfg->l1_lng_key; break;
    case 1:  v = cfg->l2_mode; dbl = &cfg->l2_dbl_key; lng = &cfg->l2_lng_key; break;
    case 2:  v = cfg->l3_mode; dbl = &cfg->l3_dbl_key; lng = &cfg->l3_lng_key; break;
    default: break;
    }
    if (v == 0 || v == 1)
        return v;
    if ((key_config_lgesture_valid(dbl, air) && dbl->action_type != KEY_ACTION_NONE) ||
        (key_config_lgesture_valid(lng, air) && lng->action_type != KEY_ACTION_NONE))
        return 1;   /* 旧数据: 配了真实手势动作 -> 手势模式 */
    return 0;       /* 旧数据: 都未配 -> 常规(直通) */
}

static const key_config_t *l_gesture_get(const key_config_t *k, int air)
{
    return key_config_lgesture_valid(k, air) ? k : NULL;
}

const key_config_t *key_config_get_l_dbl_key(uint8_t idx)
{
    key_config_storage_t *cfg = key_config_get();
    const key_config_t *k;
    switch (idx)
    {
    case 0:  k = &cfg->l1_dbl_key; break;
    case 1:  k = &cfg->l2_dbl_key; break;
    case 2:  k = &cfg->l3_dbl_key; break;
    default: return NULL;
    }
    return l_gesture_get(k, (idx == 0));
}

const key_config_t *key_config_get_l_lng_key(uint8_t idx)
{
    key_config_storage_t *cfg = key_config_get();
    const key_config_t *k;
    switch (idx)
    {
    case 0:  k = &cfg->l1_lng_key; break;
    case 1:  k = &cfg->l2_lng_key; break;
    case 2:  k = &cfg->l3_lng_key; break;
    default: return NULL;
    }
    return l_gesture_get(k, (idx == 0));
}

/* 手势模式"单击"动作 —— 独立于 l*_key(常规键)存储, 避免"改手势单击把常规键
 * 也改了"。未设置/非法 -> NULL(点按不触发)。 */
const key_config_t *key_config_get_l_tap_key(uint8_t idx)
{
    key_config_storage_t *cfg = key_config_get();
    const key_config_t *k;
    switch (idx)
    {
    case 0:  k = &cfg->l1_tap_key; break;
    case 1:  k = &cfg->l2_tap_key; break;
    case 2:  k = &cfg->l3_tap_key; break;
    default: return NULL;
    }
    if (!key_config_lgesture_valid(k, (idx == 0)) || k->action_type == KEY_ACTION_NONE)
        return NULL;
    return k;
}

/* ---- 09-07: EC 按下 模式 + 三手势(与 C/L 键同架构) ----
 * 手势合法动作 = EC 常规键同集(key_config_eckey_valid: NONE/键盘/多媒体/MOUSE
 * 键码 1..6 + AIRMOUSE —— EC 常规键本就允许空中鼠标, 手势每触发切换一次,
 * 与 EC 旋转/L1 手势语义一致)。 */
int key_config_ecpressgesture_valid(const key_config_t *k)
{
    return key_config_eckey_valid(k, 1);
}

/* EC 按下显式模式。0=常规(直通, 用 ec_press_key 主键) 1=手势。
 * 0xFF/非法(旧固件只写 276B, 本区恒为擦除态) -> 按旧规则派生(与 C/L 同策略):
 *   双击/长按任一配了【真实动作】(action != NONE 且块合法) -> 手势; 都未配 -> 常规。 */
uint8_t key_config_get_ec_press_mode(void)
{
    key_config_storage_t *cfg = key_config_get();
    uint8_t v = cfg->ec_press_mode;
    if (v == 0 || v == 1)
        return v;
    const key_config_t *dbl = &cfg->ec_press_dbl_key;
    const key_config_t *lng = &cfg->ec_press_lng_key;
    if ((key_config_ecpressgesture_valid(dbl) && dbl->action_type != KEY_ACTION_NONE) ||
        (key_config_ecpressgesture_valid(lng) && lng->action_type != KEY_ACTION_NONE))
        return 1;   /* 旧数据: 配了真实手势动作 -> 手势模式 */
    return 0;       /* 旧数据: 都未配 -> 常规(直通) */
}

static const key_config_t *ec_press_gesture_get(const key_config_t *k)
{
    return key_config_ecpressgesture_valid(k) ? k : NULL;
}

/* 双击/长按块(返回块本身, NONE 也合法 —— 调用方(main.c)用它做双击判定窗门控,
 * 与 C/L 键 getter 语义一致) */
const key_config_t *key_config_get_ec_press_dbl_key(void)
{
    return ec_press_gesture_get(&key_config_get()->ec_press_dbl_key);
}

const key_config_t *key_config_get_ec_press_lng_key(void)
{
    return ec_press_gesture_get(&key_config_get()->ec_press_lng_key);
}

/* 手势模式"单击"动作 —— 独立于 ec_press_key(常规键)存储, 避免"改手势单击把
 * 常规键也改了"。未设置(NONE)/非法 -> NULL(点按不触发)。 */
const key_config_t *key_config_get_ec_press_tap_key(void)
{
    const key_config_t *k = &key_config_get()->ec_press_tap_key;
    if (!key_config_ecpressgesture_valid(k) || k->action_type == KEY_ACTION_NONE)
        return NULL;
    return k;
}

void key_config_save_slot(uint8_t slot)
{
    if (slot >= KEY_CONFIG_SLOT_NUM) slot = 0;
    uint32_t addr = key_config_slot_addr(slot);
    key_config_storage_t *cfg = &g_key_config[slot];
    cfg->magic = KEY_CONFIG_MAGIC;
    cfg->version = KEY_CONFIG_VERSION;
    cfg->num_keys = KEY_CONFIG_NUM_KEYS;
    rt_flash_erase(addr, KEY_CONFIG_SLOT_STRIDE);
    rt_flash_write(addr, (const uint8_t *)cfg, sizeof(key_config_storage_t));
    rt_kprintf("[KEYCONFIG] slot%d saved to flash\n", slot + 1);
}

void key_config_save(void)
{
    key_config_save_slot(bt_multi_get_slot() - 1);
}

void key_config_reset_slot(uint8_t slot)
{
    if (slot >= KEY_CONFIG_SLOT_NUM) slot = 0;
    memcpy(&g_key_config[slot], key_config_defaults(slot), sizeof(g_key_config[slot]));
    rt_kprintf("[KEYCONFIG] slot%d reset to defaults\n", slot + 1);
}

void key_config_reset(void)
{
    key_config_reset_slot(bt_multi_get_slot() - 1);
}

void key_config_apply_rgb(void)
{
    /* 底光为“按下点亮、松开熄灭”的瞬时指示：空闲/上电/写配置后均为熄灭态。
       故这里把全部 WS2812B 置为 0（不再常亮），亮灯由按键按下时主动触发。 */
    for (int i = 0; i < KEY_CONFIG_NUM_KEYS; i++)
        rgb_led_show(i, 0x000000);
}

static void button_event_init(void)
{
    g_button_event_mb = rt_mb_create("btn_mb", 8, RT_IPC_FLAG_FIFO);
}

static void key_handler(int32_t pin, button_action_t action)
{
    /* 待机唤醒兜底（IDLE/LIGHT 降级场景）：若系统处于待机态但未能真正进 DEEP
     * （如 WSR 残留导致 sifli_suspend EBUSY → 降级 IDLE WFI），按键 GPIO 中断只能
     * 走到 button 库这里，触发不了 BSP_PowerUpCustom 的 power_standby_wakeup。
     * 此处显式调用：待机态(g_standby_active=1)时置唤醒标志唤醒 sleep_thread，
     * 活跃态调用被内部保护直接忽略，无副作用。 */
    extern void power_standby_wakeup(void);
    power_standby_wakeup();
    rt_mb_send(g_button_event_mb, (pin << 8) | action);
}


/* 按键 pin + IRQ 硬件初始化：逐个按键做 HAL_PIN_Set(PAD 复用/上拉) + button_init
 * + button_enable(即 rt_pin_irq_enable)。深睡唤醒后外设域掉电，除 GPIO1 唤醒脚外，
 * 其余按键 pin 的使能寄存器与 PAD 配置均丢失，需重跑本函数重建。 */
static void key_hw_init(void)
{
    button_cfg_t cfg;
    int32_t key_id;
    cfg.button_handler = key_handler;
#ifdef BSP_KEY_C1_PIN
    #ifdef BSP_KEY_C1_ACTIVE_HIGH
    cfg.active_state = BUTTON_ACTIVE_HIGH;
    cfg.mode = PIN_MODE_INPUT_PULLDOWN;
    #else
    cfg.active_state = BUTTON_ACTIVE_LOW;
    cfg.mode = PIN_MODE_INPUT_PULLUP;
    HAL_PIN_Set(PAD_PA00 + BSP_KEY_C1_PIN, GPIO_A0 + BSP_KEY_C1_PIN, PIN_PULLUP, 1);
    #endif
    cfg.pin = BSP_KEY_C1_PIN;
    key_id = button_init(&cfg);
    button_enable(key_id);
#endif

#ifdef BSP_KEY_C2_PIN
    #ifdef BSP_KEY_C2_ACTIVE_HIGH
    cfg.active_state = BUTTON_ACTIVE_HIGH;
    cfg.mode = PIN_MODE_INPUT_PULLDOWN;
    #else
    cfg.active_state = BUTTON_ACTIVE_LOW;
    cfg.mode = PIN_MODE_INPUT_PULLUP;
    HAL_PIN_Set(PAD_PA00 + BSP_KEY_C2_PIN, GPIO_A0 + BSP_KEY_C2_PIN, PIN_PULLUP, 1);
    #endif
    cfg.pin = BSP_KEY_C2_PIN;
    key_id = button_init(&cfg);
    button_enable(key_id);
#endif

#ifdef BSP_KEY_C3_PIN
    #ifdef BSP_KEY_C3_ACTIVE_HIGH
    cfg.active_state = BUTTON_ACTIVE_HIGH;
    cfg.mode = PIN_MODE_INPUT_PULLDOWN;
    #else
    cfg.active_state = BUTTON_ACTIVE_LOW;
    cfg.mode = PIN_MODE_INPUT_PULLUP;
    HAL_PIN_Set(PAD_PA00 + BSP_KEY_C3_PIN, GPIO_A0 + BSP_KEY_C3_PIN, PIN_PULLUP, 1);
    #endif
    cfg.pin = BSP_KEY_C3_PIN;
    key_id = button_init(&cfg);
    button_enable(key_id);
#endif

#ifdef BSP_KEY_L1_PIN
    #ifdef BSP_KEY_L1_ACTIVE_HIGH
    cfg.active_state = BUTTON_ACTIVE_HIGH;
    cfg.mode = PIN_MODE_INPUT_PULLDOWN;
    #else
    cfg.active_state = BUTTON_ACTIVE_LOW;
    cfg.mode = PIN_MODE_INPUT_PULLUP;
    HAL_PIN_Set(PAD_PA00 + BSP_KEY_L1_PIN, GPIO_A0 + BSP_KEY_L1_PIN, PIN_PULLUP, 1);
    #endif
    cfg.pin = BSP_KEY_L1_PIN;
    key_id = button_init(&cfg);
    button_enable(key_id);
#endif

#ifdef BSP_KEY_L2_PIN
    #ifdef BSP_KEY_L2_ACTIVE_HIGH
    cfg.active_state = BUTTON_ACTIVE_HIGH;
    cfg.mode = PIN_MODE_INPUT_PULLDOWN;
    #else
    cfg.active_state = BUTTON_ACTIVE_LOW;
    cfg.mode = PIN_MODE_INPUT_PULLUP;
    HAL_PIN_Set(PAD_PA00 + BSP_KEY_L2_PIN, GPIO_A0 + BSP_KEY_L2_PIN, PIN_PULLUP, 1);
    #endif
    cfg.pin = BSP_KEY_L2_PIN;
    key_id = button_init(&cfg);
    button_enable(key_id);
#endif

#ifdef BSP_KEY_L3_PIN
    #ifdef BSP_KEY_L3_ACTIVE_HIGH
    cfg.active_state = BUTTON_ACTIVE_HIGH;
    cfg.mode = PIN_MODE_INPUT_PULLDOWN;
    #else
    cfg.active_state = BUTTON_ACTIVE_LOW;
    cfg.mode = PIN_MODE_INPUT_PULLUP;
    HAL_PIN_Set(PAD_PA00 + BSP_KEY_L3_PIN, GPIO_A0 + BSP_KEY_L3_PIN, PIN_PULLUP, 1);
    #endif
    cfg.pin = BSP_KEY_L3_PIN;
    key_id = button_init(&cfg);
    button_enable(key_id);
#endif

#ifdef BSP_EC_KEY_PIN
    #ifdef BSP_EC_KEY_ACTIVE_HIGH
    cfg.active_state = BUTTON_ACTIVE_HIGH;
    cfg.mode = PIN_MODE_INPUT_PULLDOWN;
    #else
    cfg.active_state = BUTTON_ACTIVE_LOW;
    cfg.mode = PIN_MODE_INPUT_PULLUP;
    HAL_PIN_Set(PAD_PA00 + BSP_EC_KEY_PIN, GPIO_A0 + BSP_EC_KEY_PIN, PIN_PULLUP, 1);
    #endif
    cfg.pin = BSP_EC_KEY_PIN;
    key_id = button_init(&cfg);
    button_enable(key_id);
#endif
}

void key_init(void)
{
    key_hw_init();
    button_event_init();   /* 创建按键事件邮箱（仅首次/冷启动调用一次） */
}

/* 深睡唤醒后重建按键硬件（供 power_resume_from_deep() 调用）。
 *
 * ⚠️ 绝不能再走 key_hw_init()/button_init()：SDK button.c 对【已注册的同一 pin】
 * 返回 -SF_EBUSY 且什么都不做，于是 key_hw_init 里 `button_enable(负 id)` 是空操作，
 * 按键库 IRQ 永远不恢复 —— 按键就停在 key_enable_deep_wakeup 设的【仅下降沿】模式，
 * 松开(上升沿)收不到中断 → HID 卡键(只有按下没松开) + WS2812 底光按下常亮不灭(实测)。
 *
 * 正确做法：直接按【双向边沿 RISING_FALLING】重配各键 GPIO EXTI。IRQ 分发表
 * (drv_gpio.c 的 pin_irq_hdr_tab，含 handler 与 mode) 在 boot 时 button_init 已登记、
 * DEEP 期间 RAM 保留，故这里只需重配引脚 PAD/上拉 + EXTI 边沿即可恢复"按下+松开"。
 * 与 key_enable_deep_wakeup 的唯一区别：这里用 RISING_FALLING(双向) 而非 FALLING(仅下降)。 */
void key_reinit_after_deep(void)
{
#ifdef BSP_KEY_C1_PIN
    HAL_PIN_Set(PAD_PA00 + BSP_KEY_C1_PIN, GPIO_A0 + BSP_KEY_C1_PIN, PIN_PULLUP, 1);
    { GPIO_InitTypeDef gi; gi.Mode = GPIO_MODE_IT_RISING_FALLING; gi.Pull = GPIO_PULLUP;
      gi.Pin = BSP_KEY_C1_PIN; HAL_GPIO_Init(hwp_gpio1, &gi); }
#endif
#ifdef BSP_KEY_C2_PIN
    HAL_PIN_Set(PAD_PA00 + BSP_KEY_C2_PIN, GPIO_A0 + BSP_KEY_C2_PIN, PIN_PULLUP, 1);
    { GPIO_InitTypeDef gi; gi.Mode = GPIO_MODE_IT_RISING_FALLING; gi.Pull = GPIO_PULLUP;
      gi.Pin = BSP_KEY_C2_PIN; HAL_GPIO_Init(hwp_gpio1, &gi); }
#endif
#ifdef BSP_KEY_C3_PIN
    HAL_PIN_Set(PAD_PA00 + BSP_KEY_C3_PIN, GPIO_A0 + BSP_KEY_C3_PIN, PIN_PULLUP, 1);
    { GPIO_InitTypeDef gi; gi.Mode = GPIO_MODE_IT_RISING_FALLING; gi.Pull = GPIO_PULLUP;
      gi.Pin = BSP_KEY_C3_PIN; HAL_GPIO_Init(hwp_gpio1, &gi); }
#endif
#ifdef BSP_KEY_L1_PIN
    HAL_PIN_Set(PAD_PA00 + BSP_KEY_L1_PIN, GPIO_A0 + BSP_KEY_L1_PIN, PIN_PULLUP, 1);
    { GPIO_InitTypeDef gi; gi.Mode = GPIO_MODE_IT_RISING_FALLING; gi.Pull = GPIO_PULLUP;
      gi.Pin = BSP_KEY_L1_PIN; HAL_GPIO_Init(hwp_gpio1, &gi); }
#endif
#ifdef BSP_KEY_L2_PIN
    HAL_PIN_Set(PAD_PA00 + BSP_KEY_L2_PIN, GPIO_A0 + BSP_KEY_L2_PIN, PIN_PULLUP, 1);
    { GPIO_InitTypeDef gi; gi.Mode = GPIO_MODE_IT_RISING_FALLING; gi.Pull = GPIO_PULLUP;
      gi.Pin = BSP_KEY_L2_PIN; HAL_GPIO_Init(hwp_gpio1, &gi); }
#endif
#ifdef BSP_KEY_L3_PIN
    HAL_PIN_Set(PAD_PA00 + BSP_KEY_L3_PIN, GPIO_A0 + BSP_KEY_L3_PIN, PIN_PULLUP, 1);
    { GPIO_InitTypeDef gi; gi.Mode = GPIO_MODE_IT_RISING_FALLING; gi.Pull = GPIO_PULLUP;
      gi.Pin = BSP_KEY_L3_PIN; HAL_GPIO_Init(hwp_gpio1, &gi); }
#endif
#ifdef BSP_EC_KEY_PIN
    HAL_PIN_Set(PAD_PA00 + BSP_EC_KEY_PIN, GPIO_A0 + BSP_EC_KEY_PIN, PIN_PULLUP, 1);
    { GPIO_InitTypeDef gi; gi.Mode = GPIO_MODE_IT_RISING_FALLING; gi.Pull = GPIO_PULLUP;
      gi.Pin = BSP_EC_KEY_PIN; HAL_GPIO_Init(hwp_gpio1, &gi); }
#endif
}

/* ============================================================================
 * 52x DEEP 按键唤醒（对齐 SDK example/pm/classical + 实测路径）
 *
 * SF32LB52X 的 AON per-pin 通道为连续 21 通道 PIN0~PIN20 = PA24~PA44：
 *   CR1 管 PIN0-7（PA24-31），CR2 管 PIN8-15（PA32-39），CR3 管 PIN16-20（PA40-44），
 *   WER bit8~28 全部有效（hpsys_aon.h 核实，2026-08-05）。7 键映射：
 *   PA24→0, PA25→1, PA27→3, PA30→6(EC), PA31→7(L1), PA32→8(C3), PA38→14(L3)。
 *   早前"SVD 证据：PA28-33 无通道"（78dca16 的 key_aon_slot_valid）是误读——
 *   CR1 实际含 PIN0-7 全部 8 组 mode 字段。
 *
 * DEEP 下 PAD 被 HAL_HPAON_DISABLE_PAD 禁用（引脚浮空，内部上拉/驱动失效），
 * AON per-pin 检测器本身不看 PAD 电平；实测唤醒路径是（78dca16 验证，2026-08-04）：
 *   GPIO EXTI 检测按键下降沿 → 经 HAL_HPAON_QueryWakeupPin 映射置 WSR PIN 位
 *   → WER 使能 → AON 唤醒（见 bf0_hal_gpio.c HAL_GPIO_IRQHandler）。
 * 故本函数必须同时做三件事，缺一不可：
 *   ① HAL_PIN_Set 重配内部上拉（给边沿检测干净起点，静止=HIGH）；
 *   ② HAL_HPAON_EnableWakeupSrc(NEG_EDGE) 使能 AON per-pin 下降沿（WER PIN 位+CR mode）；
 *   ③ HAL_GPIO_Init(GPIO_MODE_IT_FALLING) 强制使能 GPIO EXTI（IESR=1，ITSR=1，下降沿）。
 *     实测 key_reinit_after_deep 的 button_enable 未生效（进 DEEP 前 IESR/ITSR/IPLSR
 *     全 0），GPIO1 检测器不监控按键 → DEEP 永远唤不醒，必须在此显式 Init。
 *
 * 必须用 NEG_EDGE（下降沿，按键 active-LOW：静止=HIGH 上拉，按下=LOW），
 * 绝不能用 AON_PIN_MODE_LOW（电平）：按下期间持续锁存 WSR PIN 位 → sifli_suspend
 * 见 WSR&WER≠0 返回 EBUSY → DEEP 降级 IDLE（2026-07-30 实测踩坑，根因是电平锁存）。
 * ============================================================================ */
void key_enable_deep_wakeup(void)
{
    int i;
    uint32_t wer;

    /* ① 清掉所有已使能的 AON PIN 唤醒 + 残留 PIN 锁存位（防 legacy/旧固件误使能锁存） */
    wer = HAL_HPAON_GET_WER();
    for (i = 0; i <= 20; i++)
    {
        if (wer & (HPSYS_AON_WSR_PIN0 << i))
            HAL_HPAON_DisableWakeupSrc((HPAON_WakeupSrcTypeDef)(HPAON_WAKEUP_SRC_PIN0 + i));
    }
    HAL_HPAON_CLEAR_WSR(HAL_HPAON_GET_WSR());

    /* ② 逐键：重配上拉 + AON per-pin NEG_EDGE + GPIO EXTI 强制使能（7 键均有通道，全配） */
#ifdef BSP_KEY_C1_PIN
    HAL_PIN_Set(PAD_PA00 + BSP_KEY_C1_PIN, GPIO_A0 + BSP_KEY_C1_PIN, PIN_PULLUP, 1);
    HAL_HPAON_EnableWakeupSrc(HPAON_WAKEUP_SRC_PIN0 + (BSP_KEY_C1_PIN - 24), AON_PIN_MODE_NEG_EDGE);
    { GPIO_InitTypeDef gi; gi.Mode = GPIO_MODE_IT_FALLING; gi.Pull = GPIO_PULLUP;
      gi.Pin = BSP_KEY_C1_PIN; HAL_GPIO_Init(hwp_gpio1, &gi); }
#endif
#ifdef BSP_KEY_C2_PIN
    HAL_PIN_Set(PAD_PA00 + BSP_KEY_C2_PIN, GPIO_A0 + BSP_KEY_C2_PIN, PIN_PULLUP, 1);
    HAL_HPAON_EnableWakeupSrc(HPAON_WAKEUP_SRC_PIN0 + (BSP_KEY_C2_PIN - 24), AON_PIN_MODE_NEG_EDGE);
    { GPIO_InitTypeDef gi; gi.Mode = GPIO_MODE_IT_FALLING; gi.Pull = GPIO_PULLUP;
      gi.Pin = BSP_KEY_C2_PIN; HAL_GPIO_Init(hwp_gpio1, &gi); }
#endif
#ifdef BSP_KEY_C3_PIN
    HAL_PIN_Set(PAD_PA00 + BSP_KEY_C3_PIN, GPIO_A0 + BSP_KEY_C3_PIN, PIN_PULLUP, 1);
    HAL_HPAON_EnableWakeupSrc(HPAON_WAKEUP_SRC_PIN0 + (BSP_KEY_C3_PIN - 24), AON_PIN_MODE_NEG_EDGE);
    { GPIO_InitTypeDef gi; gi.Mode = GPIO_MODE_IT_FALLING; gi.Pull = GPIO_PULLUP;
      gi.Pin = BSP_KEY_C3_PIN; HAL_GPIO_Init(hwp_gpio1, &gi); }
#endif
#ifdef BSP_KEY_L1_PIN
    HAL_PIN_Set(PAD_PA00 + BSP_KEY_L1_PIN, GPIO_A0 + BSP_KEY_L1_PIN, PIN_PULLUP, 1);
    HAL_HPAON_EnableWakeupSrc(HPAON_WAKEUP_SRC_PIN0 + (BSP_KEY_L1_PIN - 24), AON_PIN_MODE_NEG_EDGE);
    { GPIO_InitTypeDef gi; gi.Mode = GPIO_MODE_IT_FALLING; gi.Pull = GPIO_PULLUP;
      gi.Pin = BSP_KEY_L1_PIN; HAL_GPIO_Init(hwp_gpio1, &gi); }
#endif
#ifdef BSP_KEY_L2_PIN
    HAL_PIN_Set(PAD_PA00 + BSP_KEY_L2_PIN, GPIO_A0 + BSP_KEY_L2_PIN, PIN_PULLUP, 1);
    HAL_HPAON_EnableWakeupSrc(HPAON_WAKEUP_SRC_PIN0 + (BSP_KEY_L2_PIN - 24), AON_PIN_MODE_NEG_EDGE);
    { GPIO_InitTypeDef gi; gi.Mode = GPIO_MODE_IT_FALLING; gi.Pull = GPIO_PULLUP;
      gi.Pin = BSP_KEY_L2_PIN; HAL_GPIO_Init(hwp_gpio1, &gi); }
#endif
#ifdef BSP_KEY_L3_PIN
    HAL_PIN_Set(PAD_PA00 + BSP_KEY_L3_PIN, GPIO_A0 + BSP_KEY_L3_PIN, PIN_PULLUP, 1);
    HAL_HPAON_EnableWakeupSrc(HPAON_WAKEUP_SRC_PIN0 + (BSP_KEY_L3_PIN - 24), AON_PIN_MODE_NEG_EDGE);
    { GPIO_InitTypeDef gi; gi.Mode = GPIO_MODE_IT_FALLING; gi.Pull = GPIO_PULLUP;
      gi.Pin = BSP_KEY_L3_PIN; HAL_GPIO_Init(hwp_gpio1, &gi); }
#endif
#ifdef BSP_EC_KEY_PIN
    HAL_PIN_Set(PAD_PA00 + BSP_EC_KEY_PIN, GPIO_A0 + BSP_EC_KEY_PIN, PIN_PULLUP, 1);
    HAL_HPAON_EnableWakeupSrc(HPAON_WAKEUP_SRC_PIN0 + (BSP_EC_KEY_PIN - 24), AON_PIN_MODE_NEG_EDGE);
    { GPIO_InitTypeDef gi; gi.Mode = GPIO_MODE_IT_FALLING; gi.Pull = GPIO_PULLUP;
      gi.Pin = BSP_EC_KEY_PIN; HAL_GPIO_Init(hwp_gpio1, &gi); }
#endif
}

/* 唤醒后禁用各键 AON per-pin 唤醒（交还 HPSYS GPIO 中断），并清 WSR PIN 残留，
 * 避免下次进 DEEP 时误锁存。GPIO EXTI（IESR）保持使能：唤醒后按键照常走 GPIO 中断。 */
void key_disable_deep_wakeup(void)
{
#ifdef BSP_KEY_C1_PIN
    HAL_HPAON_DisableWakeupSrc(HPAON_WAKEUP_SRC_PIN0 + (BSP_KEY_C1_PIN - 24));
#endif
#ifdef BSP_KEY_C2_PIN
    HAL_HPAON_DisableWakeupSrc(HPAON_WAKEUP_SRC_PIN0 + (BSP_KEY_C2_PIN - 24));
#endif
#ifdef BSP_KEY_C3_PIN
    HAL_HPAON_DisableWakeupSrc(HPAON_WAKEUP_SRC_PIN0 + (BSP_KEY_C3_PIN - 24));
#endif
#ifdef BSP_KEY_L1_PIN
    HAL_HPAON_DisableWakeupSrc(HPAON_WAKEUP_SRC_PIN0 + (BSP_KEY_L1_PIN - 24));
#endif
#ifdef BSP_KEY_L2_PIN
    HAL_HPAON_DisableWakeupSrc(HPAON_WAKEUP_SRC_PIN0 + (BSP_KEY_L2_PIN - 24));
#endif
#ifdef BSP_KEY_L3_PIN
    HAL_HPAON_DisableWakeupSrc(HPAON_WAKEUP_SRC_PIN0 + (BSP_KEY_L3_PIN - 24));
#endif
#ifdef BSP_EC_KEY_PIN
    HAL_HPAON_DisableWakeupSrc(HPAON_WAKEUP_SRC_PIN0 + (BSP_EC_KEY_PIN - 24));
#endif
    /* 关掉 AON PIN 唤醒后清掉残留 PIN 状态位（SDK GPIO ISR 也会清，这里兜底一次） */
    HAL_HPAON_CLEAR_WSR(HAL_HPAON_GET_WSR_PIN());
}
