#include "ws2812b.h"
#include "rtthread.h"
#include "bf0_hal.h"
#include "drv_io.h"
#include "stdio.h"
#include "string.h"
#include "drivers/rt_drv_pwm.h"
#include "drv_rgbled.h"

#define RGBLED_NAME    "rgbled"
#define LED_COUNT      3

/* ===================== RGB 亮度硬上限 15%（10-04 产品要求）==================
 * C1/C2/C3 三颗 WS2812 的亮度上限锁死为 15%，超了硬件主动降下来，软件绕不过。
 *
 * ⚠️ 只作用于 C1/C2/C3 三颗按键灯。**状态灯（LED2/3/4，走
 *    ble_led_service → rgb_led_set_color）不在此限幅范围内，保持原样**
 *    （10-04 用户明确要求"状态灯先不动它"）。
 *
 * ★★ 限幅的**唯一开关**就是下面这个宏（钳位函数与调用点都已就位）：
 *     15  = 锁死 15%（产品要求）—— 正常值
 *     100 = 取消限幅            —— 10-04 排查期间临时用过，现已恢复
 *   日志实况：排查期固件跑的是 100，worker 回包 `eff_bri=100`；恢复 15 后应回 `eff_bri=15`
 *   （`dfu_device.c` 的 CONF_CMD_RGB_APPLY 回包第 2 字节就是实际生效的 bri，可直接核对）。
 *
 *   ★★ 教训（保留备查，最容易踩）：钳位对象必须是**亮度参数 bri (0~100)**，
 *   而非已缩放的颜色值。因为上游两条路径都已经先按 bri 缩放过：
 *     · 预览  dfu_device.c  rr = (r * bri) / 100
 *     · 按键  main.c        r  = (rgb_r * bri) / 100
 *   若在出灯口再压一次 15/100 就是**双重降幅**：bri=15 时
 *     255 →(上游 15%) 38 →(本处再 15%) 6   = 2.4% ⇒ 肉眼几乎全黑（10-04 踩过）
 *   正确做法 = 把 bri 钳到 ≤15，让上游**只缩放一次**。
 *   ⇒ 上位机 `KeyConfigManager::kLedBrightnessHwCapPct` 必须与本值一致（两边各一份）。
 */
#define RGB_BRIGHTNESS_MAX_PCT   15   /* C1/C2/C3 亮度上限：产品要求锁死 15% */

/* 把亮度参数 bri 钳到 [0, 上限]。uint8 不会溢出。 */
static inline uint8_t rgb_led_cap_bri(uint8_t bri)
{
    return (bri > RGB_BRIGHTNESS_MAX_PCT) ? (uint8_t)RGB_BRIGHTNESS_MAX_PCT : bri;
}


struct rt_device *rgbled_device;
static uint32_t s_led_buffer[LED_COUNT];

/* C1/C2/C3 上一次被下发的"已限幅亮度%"，供按键路径复用（省一次除法）。
 * 仅记录，不参与出灯；状态灯不读写这里。 */
static uint8_t s_last_capped_bri[LED_COUNT] = {0, 0, 0};

/* 供 dfu_device.c / main.c 调用的限幅入口（非 static，声明见 ws2812b.h） */
uint8_t rgb_led_cap_brightness(uint8_t bri) { return rgb_led_cap_bri(bri); }


struct rt_color
{
    char *color_name;
    uint32_t color;
};

static struct rt_color rgb_color_arry[] = {
    {"black", 0x000000},  {"blue", 0x0000FF}, {"green", 0x00FF00},
    {"cyan", 0x00FFFF},   {"red", 0xFF0000},  {"purple", 0xFF00FF},
    {"yellow", 0xFFFF00}, {"white", 0xFFFFFF}
};

void rgb_led_init(void)
{
    //HAL_PMU_ConfigPeriLdo(PMU_PERI_LDO3_3V3, true, true);
    
    rgbled_device = rt_device_find(RGBLED_NAME);
    if (!rgbled_device)
    {
        RT_ASSERT(0);
    }
    memset(s_led_buffer, 0, sizeof(s_led_buffer));
}

/* ---- 驱动出口：保持原样，**不**在此限幅 ----
 * 10-04 教训：这两个出口收到的 color 都已由调用方按 bri 缩放过了
 *   · rgb_led_set_color()  ← ble_led_service 状态灯（用户 10-04 明确要求不动）
 *   · rgb_led_set_colors() ← rgb_led_show()，值来自 s_led_buffer（已限幅）
 * 在此再压一次 = 双重降幅（bri=15 时 38 → 6，肉眼全黑）。
 * 限幅改在**参数级**（rgb_led_cap_brightness 钳 bri），见文件头。 */
void rgb_led_set_color(uint32_t color)
{
    struct rt_rgbled_configuration configuration;
    configuration.color_rgb = color;
    rt_device_control(rgbled_device, PWM_CMD_SET_COLOR, &configuration);
}

void rgb_led_set_colors(uint32_t *colors, uint16_t count)
{
    struct rt_rgbled_multi_configuration configuration;
    configuration.led_count = count;
    configuration.color_array = (rt_uint32_t *)colors;
    rt_device_control(rgbled_device, RGB_CMD_SET_MULTI_COLOR, &configuration);
}

/* C1/C2/C3 单灯点亮。color 必须是"已按限幅后 bri 缩放好"的值
 * （本函数不再缩放，否则与上游重复）。 */
void rgb_led_show(uint8_t index, uint32_t color)
{
    if (index >= LED_COUNT) return;   /* 顺手修掉原有的越界写（`>` 应为 `>=`） */

    s_led_buffer[index] = color;
    rgb_led_set_colors(s_led_buffer, LED_COUNT);
}

/* 记录某颗灯本次用的限幅后亮度（供按键路径查询，避免重复除法） */
void rgb_led_note_bri(uint8_t index, uint8_t capped_bri)
{
    if (index < LED_COUNT) s_last_capped_bri[index] = capped_bri;
}

uint8_t rgb_led_get_bri(uint8_t index)
{
    return (index < LED_COUNT) ? s_last_capped_bri[index] : 0;
}

