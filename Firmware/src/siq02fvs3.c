/**
 ******************************************************************************
 * @file    siq02fvs3.c
 * @brief   SIQ-02FVS3 旋转编码器驱动 (仅 A/B 相)
 *
 * 基于 SIQ-02FVS3 规格书优化的解码算法:
 *
 *   规格书关键参数:
 *     - 15 pulses/360°, 15 detents, 步角 24°±2°
 *     - Chattering t1/t3 ≤ 3ms (OFF↔ON 切换时抖动)
 *     - Sliding noise t2 ≤ 2ms (ON 状态下杂讯)
 *     - 推荐电路: 上拉 10KΩ + 0.01μF 滤波
 *
 *   解码策略: 经典正交转移表 (Gray-code 相邻态) 4x 增量计数
 *     1. 每次 1ms 采样读取 AB 状态, 与上一确认态查表得方向 +1(CW)/-1(CCW)。
 *        合法转移仅 Gray 相邻态 (差 1 位), 每步固定 ±1; 4 子步 = 1 机械档位。
 *        (注: Gray 相邻态在二进制下差 ±1/±2 交替, 故必须用转移表而非"二进制
 *         状态差", 否则相邻 01→11 会被误判跳步, 整圈净步抵消 → 双向全失灵。)
 *     2. 非法跳变 (差 2 位, 多为 1ms 漏采样越过一个相邻态) 按最近方向
 *        (last_dir) 补偿 ±2, 不丢步 —— 这是快转准确的关键。
 *     3. 不再使用"连续 N 次一致"软件去抖: SIQ-02FVS3 硬件已 10k+0.01uF 滤波
 *        (规格书推荐), 软件去抖会在连续快转时因阈值永远达不到而整段丢步
 *        (旧方案 bug: 向下滑常快转 → 滚好多下才反应)。
 *
 *   此方案对慢转/快转/反向均鲁棒, 且方向对称(向上/向下不应有差异);
 *   若改完后仍单向失灵, 则应查硬件(A/B 相上拉/接触不对等, 示波器看波形).
 ******************************************************************************
 */

#include "siq02fvs3.h"
#include "rtdevice.h"
#include "rthw.h"
#include <string.h>

/*----------------------------------------------------------------------------*/
/* 内部常量                                                                      */
/*----------------------------------------------------------------------------*/
#define ENC_STATE(a, b)         (((a) << 1) | (b))

/*
 * 去抖采样次数: 连续 N 次 1ms 定时器读到相同值才确认.
 * 规格书: sliding noise ≤ 2ms → 2 次采样 (2ms) 即可过滤.
 * 3 次 (3ms) 会导致快转时把正常跳变也滤掉.
 */
#ifndef ENC_DEBOUNCE_SAMPLES
#define ENC_DEBOUNCE_SAMPLES    2
#endif

/*
 * 定时器轮询周期 (ms).
 * 1ms 可在 2ms 窗口内采集到 2 个样本.
 */
#ifndef ENC_POLL_PERIOD_MS
#define ENC_POLL_PERIOD_MS      1
#endif

/*
 * 每步子步数: 一个完整正交周期 (00→01→11→10→00) 有 4 个状态跳变.
 * 4 个子步 = 1 个机械档位.
 */
#define ENC_SUBSTEPS_PER_STEP   4

/*
 * 注: "正交转移表"方向判定保留(已恢复), 仅移除其中有害的"连续 N 次一致
 * 软件去抖"(见 enc_quadrature_decode)。2026-08-18 一度误用"二进制状态差
 * 最短路径"替代转移表, 因 Gray 相邻态二进制差 ±1/±2 交替, 导致整圈净步
 * 抵消、双向全失灵, 已回退为转移表方案。
 */

/*----------------------------------------------------------------------------*/
/* 驱动状态                                                                      */
/*----------------------------------------------------------------------------*/
typedef struct {
    rt_timer_t      timer;
    enc_callback_t  callback;
    int32_t         count;          /* 公开: 累计步数 */

    /* ---- GPIO / 去抖 ---- */
    uint8_t         prev_stable;    /* 上次已确认的稳定状态 (0~3) */
    uint8_t         raw_state;      /* 当前原始读数 */
    uint8_t         debounce_cnt;   /* 连续相同读数的次数 */

    /* ---- 4x 子步累加 ---- */
    int8_t          quad_sub;       /* 子步计数器 */
    int8_t          last_dir;       /* 最近一步方向 ±1, 用于跳步方向补偿 */

    /* ---- 低功耗: 空闲停 1ms 轮询, 引脚边沿唤醒 (空闲优化 B) ---- */
    uint8_t         idle;           /* 1=空闲态(定时器停, 引脚中断作为唤醒源) */
    rt_sem_t        wake_sem;       /* 引脚边沿唤醒信号量 */
    rt_thread_t     wake_thread;    /* 唤醒处理线程 */
    void          (*wake_cb)(void); /* 唤醒回调: 通知上层回到 active(LIGHT/紧连接/IMU唤醒) */

} enc_ctx_t;

static enc_ctx_t g_enc;

/*----------------------------------------------------------------------------*/
/* 内部函数                                                                      */
/*----------------------------------------------------------------------------*/
static inline int enc_read_a(void) { return rt_pin_read(ENC_A_PIN); }
static inline int enc_read_b(void) { return rt_pin_read(ENC_B_PIN); }

/**
 * @brief 核心解码函数 — 由 1ms 定时器调用
 *
 * 采用经典正交转移表 (Gray-code 相邻态) 解码, 4x 计数:
 *   - 每次采样读到新状态, 与上一确认态查表得方向 +1(CW)/-1(CCW)。
 *     合法转移仅 Gray 相邻态 (差 1 位), CW/CCW 各 4 条。
 *   - 非法跳变 (差 2 位, 多为 1ms 漏采样越过一个相邻态) 不丢弃,
 *     按最近方向 last_dir 补偿为 ±2, 避免快转丢步。
 *   - 已去除"连续 N 次一致"软件去抖: SIQ-02FVS3 硬件 10k+0.01uF 滤波
 *     (规格书推荐) 已滤掉机械抖动, 软件去抖在连续快转时因阈值永远达不到
 *     而整段丢步 (旧方案 bug: 向下滑常快转 → 滚好多下才反应)。
 *   - 4 个子步 = 1 个机械档位, quad_sub 满 ±4 上报一步。
 */
/*
 * 正交转移方向表 [prev][curr]:
 *   +1 = CW (正向)  -1 = CCW (反向)  0 = 无变化 / 非法跳变(差两位)
 * 状态编码 ENC_STATE = (A<<1)|B:
 *   0=00  1=01  2=10  3=11
 * CW 序列: 0→1→3→2→0   CCW 序列: 0→2→3→1→0
 */
static const int8_t enc_transition[4][4] = {
    /* curr:  00   01   10   11 */
    /* 00 */ { 0,  +1,  -1,   0 },
    /* 01 */ {-1,   0,   0,  +1 },
    /* 10 */ {+1,   0,   0,  -1 },
    /* 11 */ { 0,  -1,  +1,   0 },
};

static void enc_quadrature_decode(void)
{
    uint8_t state = ENC_STATE(enc_read_a(), enc_read_b());

    if (state == g_enc.prev_stable)
        return;   /* 无变化 */

    int8_t dir = enc_transition[g_enc.prev_stable][state];
    g_enc.prev_stable = state;

    if (dir == 0) {
        /* 非法跳变 (差两位): 多半是 1ms 轮询漏掉中间一步,
         * 按最近方向补偿 ±2 以免快转丢步; 无方向史则不补偿(直接忽略)。 */
        dir = g_enc.last_dir ? (int8_t)(g_enc.last_dir * 2) : 0;
        if (dir == 0)
            return;
    }

    g_enc.last_dir = (dir > 0) ? 1 : -1;
    g_enc.quad_sub += dir;

    /* 检测完整步: 4 个子步 = 1 个机械档位. 用 while 兼容一次采样跨多步. */
    rt_base_t level = rt_hw_interrupt_disable();

    while (g_enc.quad_sub >= ENC_SUBSTEPS_PER_STEP) {
        g_enc.quad_sub -= ENC_SUBSTEPS_PER_STEP;
        ++g_enc.count;

        int32_t cur = g_enc.count;
        rt_hw_interrupt_enable(level);

        if (g_enc.callback)
            g_enc.callback(ENC_EVT_CW, cur);

        level = rt_hw_interrupt_disable();
    }

    while (g_enc.quad_sub <= -ENC_SUBSTEPS_PER_STEP) {
        g_enc.quad_sub += ENC_SUBSTEPS_PER_STEP;
        --g_enc.count;

        int32_t cur = g_enc.count;
        rt_hw_interrupt_enable(level);

        if (g_enc.callback)
            g_enc.callback(ENC_EVT_CCW, cur);

        level = rt_hw_interrupt_disable();
    }

    rt_hw_interrupt_enable(level);
}

/**
 * @brief 1ms 软件定时器回调
 */
static void enc_timer_cb(void *parameter)
{
    (void)parameter;
    enc_quadrature_decode();
}

/*----------------------------------------------------------------------------*/
/* 低功耗: 空闲态引脚边沿唤醒 (空闲优化 B)                                     */
/*----------------------------------------------------------------------------*/

/* 引脚边沿 ISR: 仅释放信号量, 实际处理在 wake 线程(避免 ISR 内做重活/加锁) */
static void enc_wake_isr(void *args)
{
    (void)args;
    if (g_enc.wake_sem)
        rt_sem_release(g_enc.wake_sem);
}

/* 唤醒线程: 空闲态下引脚边沿(用户滚动)唤醒后, 重启 1ms 轮询定时器并通知上层
 * 回到 active 模式(HCPU 退出 DEEP, IMU 唤醒, 蓝牙连接参数收紧). */
static void enc_wake_thread_entry(void *param)
{
    (void)param;
    while (1) {
        if (g_enc.wake_sem)
            rt_sem_take(g_enc.wake_sem, RT_WAITING_FOREVER);
        /* 仅在空闲态才需处理(避免活跃态每次边沿空转) */
        if (!g_enc.idle)
            continue;
        g_enc.idle = 0;
        if (g_enc.wake_cb)
            g_enc.wake_cb();   /* 上层完成: 重启定时器 + LIGHT + 紧连接 + IMU active */
    }
}

/**
 * @brief 进入空闲态: 停 1ms 轮询定时器, 改用引脚【双边沿】中断作为唤醒源.
 *        编码引脚(PA02/PA04)位于 GPIO1 银行, 与按键/IMU 共用 NEG 边沿唤醒源,
 *        滚动(接地)产生下降沿即可从 DEEP 唤醒. 解码算法保持 1ms 轮询不变,
 *        唤醒后由 wake_cb 重启定时器.
 */
void enc_enter_idle(void)
{
    if (!g_enc.timer)
        return;
    rt_timer_stop(g_enc.timer);
    g_enc.idle = 1;
    /* 挂载【双边沿】中断作为唤醒源: SIQ-02FVS3 静止相位不定, 只听 FALLING 会
     * 漏掉"首动为上升沿"的方向(实测: 向下滑常无反应)。双边沿确保任意方向/
     * 相位的首动都能唤醒。多余中断在 wake 线程因 idle 已清零而被丢弃, 无副作用。 */
    rt_pin_attach_irq(ENC_A_PIN, PIN_IRQ_MODE_RISING_FALLING, enc_wake_isr, RT_NULL);
    rt_pin_irq_enable(ENC_A_PIN, PIN_IRQ_ENABLE);
    rt_pin_attach_irq(ENC_B_PIN, PIN_IRQ_MODE_RISING_FALLING, enc_wake_isr, RT_NULL);
    rt_pin_irq_enable(ENC_B_PIN, PIN_IRQ_ENABLE);
    rt_kprintf("[ENC] idle: 1ms poll stopped, pin-edge wake armed\n");
}

/**
 * @brief 回到活跃态: 卸引脚中断, 重启 1ms 轮询定时器.
 */
void enc_enter_active(void)
{
    g_enc.idle = 0;
    rt_pin_irq_enable(ENC_A_PIN, PIN_IRQ_DISABLE);
    rt_pin_detach_irq(ENC_A_PIN);
    rt_pin_irq_enable(ENC_B_PIN, PIN_IRQ_DISABLE);
    rt_pin_detach_irq(ENC_B_PIN);
    if (g_enc.timer)
        rt_timer_start(g_enc.timer);
    rt_kprintf("[ENC] active: 1ms poll resumed\n");
}

/** @brief 注册唤醒回调(由 main 注册, 在空闲态引脚边沿时调用) */
void enc_set_wake_callback(void (*cb)(void))
{
    g_enc.wake_cb = cb;
}

/*----------------------------------------------------------------------------*/
/* 公开 API                                                                      */
/*----------------------------------------------------------------------------*/

int enc_init(enc_callback_t cb)
{
    memset(&g_enc, 0, sizeof(g_enc));
    g_enc.callback = cb;

    /* 配置 GPIO 输入上拉 (C 端子接 GND, 开关输出低电平有效) */
    rt_pin_mode(ENC_A_PIN, PIN_MODE_INPUT_PULLUP);
    rt_pin_mode(ENC_B_PIN, PIN_MODE_INPUT_PULLUP);

    /* 读取初始状态并初始化状态机 */
    uint8_t init_state = ENC_STATE(rt_pin_read(ENC_A_PIN), rt_pin_read(ENC_B_PIN));
    g_enc.prev_stable   = init_state;
    g_enc.raw_state     = init_state;
    g_enc.debounce_cnt  = ENC_DEBOUNCE_SAMPLES; /* 保留未用(硬件滤波替代软件去抖) */
    g_enc.quad_sub      = 0;
    g_enc.last_dir      = 0;

    /* 创建 1ms 周期定时器（不在此启动！）
     * 编码器仅在 BLE 连接后由 enc_start()/enc_enter_active() 启用、断连即 enc_stop()，
     * 否则这枚周期软定时器会永远 ≤1ms 到期，直接阻断 HPSYS 进深睡（低功耗命门）。
     * 空闲态(见 enc_enter_idle)会停掉此定时器, 改用引脚边沿中断作为 DEEP 唤醒源。 */
    g_enc.timer = rt_timer_create("enc", enc_timer_cb, RT_NULL,
                                   rt_tick_from_millisecond(ENC_POLL_PERIOD_MS),
                                   RT_TIMER_FLAG_PERIODIC | RT_TIMER_FLAG_SOFT_TIMER);
    if (!g_enc.timer) {
        rt_kprintf("[ENC] timer create failed\n");
        return -1;
    }

    /* 低功耗唤醒通道: 信号量 + 线程, 空闲态引脚边沿唤醒时通知上层回 active */
    g_enc.wake_sem = rt_sem_create("enc_wk", 0, RT_IPC_FLAG_FIFO);
    if (g_enc.wake_sem) {
        g_enc.wake_thread = rt_thread_create("enc_wk", enc_wake_thread_entry,
                                             RT_NULL, 1024, 18, 10);
        if (g_enc.wake_thread)
            rt_thread_startup(g_enc.wake_thread);
    }

    rt_kprintf("[ENC] SIQ-02FVS3 ready (idle, start on BT connect)  PA%02d=A PA%02d=B  poll=%dms debounce=%d sub_per_step=%d\n",
               ENC_A_PIN, ENC_B_PIN, ENC_POLL_PERIOD_MS, ENC_DEBOUNCE_SAMPLES, ENC_SUBSTEPS_PER_STEP);

    return 0;
}

void enc_set_callback(enc_callback_t cb)
{
    g_enc.callback = cb;
}

int32_t enc_get_count(void)
{
    rt_base_t level = rt_hw_interrupt_disable();
    int32_t val = g_enc.count;
    rt_hw_interrupt_enable(level);
    return val;
}

void enc_set_count(int32_t count)
{
    rt_base_t level = rt_hw_interrupt_disable();
    g_enc.count = count;
    g_enc.quad_sub = 0; /* 重置子步, 避免残留 */
    rt_hw_interrupt_enable(level);
}

void enc_start(void)
{
    if (!g_enc.timer)
        return;
    /* 确保不在空闲态(卸掉可能的引脚唤醒中断), 回到活跃轮询 */
    g_enc.idle = 0;
    rt_pin_irq_enable(ENC_A_PIN, PIN_IRQ_DISABLE);
    rt_pin_detach_irq(ENC_A_PIN);
    rt_pin_irq_enable(ENC_B_PIN, PIN_IRQ_DISABLE);
    rt_pin_detach_irq(ENC_B_PIN);
    /* 重新初始化去抖状态机：丢弃断连期间可能累积的旋转，避免重连瞬间误报一步。
     * 不清 count（累计步数跨连接保留，符合滚轮语义）。 */
    uint8_t init_state = ENC_STATE(rt_pin_read(ENC_A_PIN), rt_pin_read(ENC_B_PIN));
    g_enc.prev_stable   = init_state;
    g_enc.raw_state     = init_state;
    g_enc.debounce_cnt  = ENC_DEBOUNCE_SAMPLES;
    g_enc.quad_sub      = 0;
    g_enc.last_dir      = 0;
    rt_timer_start(g_enc.timer);
}

void enc_stop(void)
{
    if (g_enc.timer)
        rt_timer_stop(g_enc.timer);
    /* 卸掉可能残留的引脚唤醒中断, 回到完全停止 */
    g_enc.idle = 0;
    rt_pin_irq_enable(ENC_A_PIN, PIN_IRQ_DISABLE);
    rt_pin_detach_irq(ENC_A_PIN);
    rt_pin_irq_enable(ENC_B_PIN, PIN_IRQ_DISABLE);
    rt_pin_detach_irq(ENC_B_PIN);
}
