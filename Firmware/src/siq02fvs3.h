/**
 ******************************************************************************
 * @file    siq02fvs3.h
 * @brief   SIQ-02FVS3 旋转编码器驱动 (仅 A/B 相)
 *
 * SIQ-02FVS3 (Mitsumi / Kinghelm KH-SIQ-02FVS3)
 *   - 10mm 薄型增量式旋转编码器, 15 档位 / 15 脉冲每转
 *   - DIP-5 封装 (3 脚编码器 + 2 脚按压开关)
 *   - 仅使用 A/B 相 (C 接 GND)
 *
 * 电气参数 (摘自规格书):
 *   - 额定电压: DC 5V
 *   - 输出: 2 相正交信号 (A, B), Code-ON ≤ 1.5V, Code-OFF ≥ 3.5V
 *   - Chattering ≤ 3ms (OFF↔ON 切换抖动)
 *   - Sliding noise ≤ 2ms (ON 状态杂讯)
 *   - 推荐滤波: 10KΩ 上拉 + 0.01μF 电容 (A/B 各自)
 *
 * 引脚连接:
 *   ENC_A → GPIO 输入 (上拉)
 *   ENC_B → GPIO 输入 (上拉)
 *   C     → GND
 *
 * 解码算法: 4x 正交增量计数
 *   轻量去抖 (2ms) + 转移表方向判定 + 子步累加.
 *   每 4 个子步 = 1 个机械档位, 回弹边沿自动抵消, 不丢进度.
 ******************************************************************************
 */

#ifndef __SIQ02FVS3_H__
#define __SIQ02FVS3_H__

#include "rtthread.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*==============================================================================
 * 引脚配置
 *============================================================================*/

#ifndef ENC_A_PIN
#define ENC_A_PIN           2       /* PA02 */
#endif

#ifndef ENC_B_PIN
#define ENC_B_PIN           4       /* PA04 */
#endif

/*==============================================================================
 * 参数 (可在编译选项中覆盖)
 *============================================================================*/

/* 定时器轮询周期 (ms) */
#ifndef ENC_POLL_PERIOD_MS
#define ENC_POLL_PERIOD_MS      1
#endif

/* 消抖采样次数 (连续 N 次 1ms 采样相同才确认) */
#ifndef ENC_DEBOUNCE_SAMPLES
#define ENC_DEBOUNCE_SAMPLES    2
#endif

/*==============================================================================
 * 事件
 *============================================================================*/

typedef enum {
    ENC_EVT_CW  = 0,   /* 顺时针一步 */
    ENC_EVT_CCW = 1,   /* 逆时针一步 */
} enc_event_t;

typedef void (*enc_callback_t)(enc_event_t event, int32_t count);

/*==============================================================================
 * API
 *============================================================================*/

int     enc_init(enc_callback_t cb);
void    enc_set_callback(enc_callback_t cb);
int32_t enc_get_count(void);
void    enc_set_count(int32_t count);
void    enc_start(void);
void    enc_stop(void);

/* 低功耗(空闲优化 B): 空闲停 1ms 轮询、改用引脚边沿唤醒 */
void    enc_enter_idle(void);     /* 停定时器 + 挂引脚双边沿中断作唤醒源 */
void    enc_enter_active(void);   /* 卸引脚中断 + 重启 1ms 轮询 */
void    enc_set_wake_callback(void (*cb)(void));  /* 注册空闲唤醒回调(通知上层回 active) */

#ifdef __cplusplus
}
#endif

#endif /* __SIQ02FVS3_H__ */
