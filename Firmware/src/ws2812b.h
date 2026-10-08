#ifndef __WS2812B_H__
#define __WS2812B_H__

#include <stdint.h>

void rgb_led_init(void);
void rgb_led_set_color(uint32_t color);
void rgb_led_set_colors(uint32_t *colors, uint16_t count);
void rgb_led_show(uint8_t index, uint32_t color);

/* C1/C2/C3 亮度硬上限 15%（10-04 产品要求，硬件主动降低，软件绕不过）。
 * 用法：先把【亮度参数】过一遍这个函数，再用限幅后的值去缩放颜色：
 *     uint8_t bri = rgb_led_cap_brightness(cfg->rgb_brightness);
 *     r = (cfg->rgb_r * bri) / 100;  ... 再 rgb_led_show(idx, color);
 * ⚠️ 不要在 rgb_led_show() 里再压一次颜色 —— 上游已缩放，会成双重降幅
 *    （15% → 38 → 6 ≈ 2.4%，肉眼全黑，10-04 实测踩过）。
 * ⚠️ 只作用于 C1/C2/C3 三颗按键灯；状态灯（LED2/3/4，走 rgb_led_set_color）不受此限。 */
uint8_t rgb_led_cap_brightness(uint8_t bri);

/* 记录/查询某颗灯上次用的限幅后亮度（仅记录，便于诊断/复用） */
void rgb_led_note_bri(uint8_t index, uint8_t capped_bri);
uint8_t rgb_led_get_bri(uint8_t index);

#endif
