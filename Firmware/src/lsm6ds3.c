/**
 ******************************************************************************
 * @file    lsm6ds3.c
 * @brief   LSM6DS3TR-C 六轴 IMU 驱动 (I2C, 轻量裸驱动)
 *
 * 通信协议: RT-Thread I2C 总线 (rt_i2c_transfer)
 * 中断:     INT1 → PA9, 上升沿触发, 通过 rt_pin_attach_irq
 * FIFO:     ODR 模式, 阈值触发批量读取
 ******************************************************************************
 */

#include "lsm6ds3.h"
#include "rthw.h"
#include <string.h>
#include <stdlib.h>  /* strtol, strtoul */
#include <rtdevice.h>  /* rt_i2c_bus_device_find */

/*----------------------------------------------------------------------------*/
/* 内部常量                                                                      */
/*----------------------------------------------------------------------------*/

/* CTRL3_C 位定义 */
#define CTRL3_C_SW_RESET    (1u << 0)   /* 软复位 */
#define CTRL3_C_IF_INC      (1u << 2)   /* 地址自动递增 (连续读) */
#define CTRL3_C_BDU         (1u << 6)   /* 块数据更新 */
#define CTRL3_C_H_LACTIVE   (1u << 5)   /* 中断引脚电平: 1=低有效(active-low) */

/* 低功耗(空闲)模式参数: 关陀螺仪, 加速度降到 12.5Hz (LSM_ODR_12HZ=0x10).
 * 陀螺仪是 IMU 主要耗电项, 关掉后电流大幅降低; 加速度保留以检测"拿起/移动"
 * 从而唤醒. 配合 Ctrl3 H_LACTIVE(INT1 低有效) + 引脚 FALLING 中断,
 * 加速度 DRDY 的下降沿可被 GPIO1 NEG 边沿唤醒源捕获, 使 HCPU 从 DEEP 唤醒. */
#define LSM_LOWPOWER_XL_ODR LSM_ODR_12HZ

/* INT1_CTRL 位定义 */
#define INT1_CTRL_DRDY_XL   (1u << 0)   /* 加速度计数据就绪 */
#define INT1_CTRL_DRDY_G    (1u << 1)   /* 陀螺仪数据就绪 */
#define INT1_CTRL_FIFO_TH   (1u << 3)   /* FIFO 阈值 */
#define INT1_CTRL_FIFO_FULL (1u << 5)   /* FIFO 满 */

/* FIFO_CTRL5 模式 */
#define FIFO_CTRL5_MODE_ODR (0x06)      /* 连续模式 */

/* FIFO 状态寄存器掩码 */
#define FIFO_STATUS1_DIFF_MASK   0xFF
#define FIFO_STATUS2_DIFF_MASK   0x07   /* bit[2:0] = diff[10:8] */
#define FIFO_STATUS2_OVER_RUN    (1u << 6)
#define FIFO_STATUS2_FIFO_FULL   (1u << 5)
#define FIFO_STATUS2_FIFO_EMPTY  (1u << 4)
#define FIFO_STATUS2_FIFO_TH     (1u << 7)   /* FTH bit */

/* 每帧字数 (G×3 + XL×3 = 6 个 16-bit 字) */
#define FIFO_WORDS_PER_FRAME    6

/* FIFO 最大帧缓冲 (避免栈溢出, 按需调整) */
#ifndef LSM_FIFO_BUF_FRAMES
#define LSM_FIFO_BUF_FRAMES     32
#endif

/*----------------------------------------------------------------------------*/
/* 前向声明                                                                      */
/*----------------------------------------------------------------------------*/

static void lsm_filter_apply(lsm_frame_t *frame);

/*----------------------------------------------------------------------------*/
/* 驱动状态                                                                      */
/*----------------------------------------------------------------------------*/

typedef struct {
    struct rt_i2c_bus_device *i2c;
    lsm_config_t    cfg;
    lsm_drdy_cb_t   drdy_cb;
    lsm_fifo_cb_t   fifo_cb;
    bool            initialized;

    /* INT1 软件线程通知 */
    rt_sem_t        int_sem;
    rt_thread_t     int_thread;

    /* 低通滤波状态 (整数 IIR, 移位实现) */
    uint8_t     filter_shift;   /* 0=关闭, 2~6 推荐 */
    int32_t     accel_filt[3];  /* 滤波后加速度 mg (x,y,z) */
    int32_t     gyro_filt[3];   /* 滤波后角速度 mdps (x,y,z) */
    bool        filter_valid;  /* 是否已初始化 */

    /* FIFO 读取缓冲 */
    lsm_frame_t     fifo_buf[LSM_FIFO_BUF_FRAMES];
} lsm_ctx_t;

static lsm_ctx_t g_lsm;

/* 轮询模式定时器 (不依赖 INT1 中断) */
static rt_timer_t g_lsm_poll_timer  = RT_NULL;
static bool      g_lsm_poll_running = false;

/* ISR/线程调试计数器 */
static volatile uint32_t g_lsm_isr_count   = 0;
static volatile uint32_t g_lsm_thread_count = 0;

/*----------------------------------------------------------------------------*/
/* I2C 底层                                                                      */
/*----------------------------------------------------------------------------*/

static int lsm_i2c_write(uint8_t reg, const uint8_t *data, uint16_t len)
{
    /* 组帧: [reg, data0, data1, ...] */
    uint8_t buf[len + 1];
    buf[0] = reg;
    memcpy(&buf[1], data, len);

    struct rt_i2c_msg msg = {
        .addr  = LSM_I2C_ADDR,
        .flags = RT_I2C_WR,
        .buf   = buf,
        .len   = (uint16_t)(len + 1),
    };

    if (rt_i2c_transfer(g_lsm.i2c, &msg, 1) != 1)
        return -RT_EIO;
    return RT_EOK;
}

static int lsm_i2c_read(uint8_t reg, uint8_t *data, uint16_t len)
{
    struct rt_i2c_msg msgs[2] = {
        {
            .addr  = LSM_I2C_ADDR,
            .flags = RT_I2C_WR,
            .buf   = &reg,
            .len   = 1,
        },
        {
            .addr  = LSM_I2C_ADDR,
            .flags = RT_I2C_RD,
            .buf   = data,
            .len   = (uint16_t)len,
        },
    };

    if (rt_i2c_transfer(g_lsm.i2c, msgs, 2) != 2)
        return -RT_EIO;
    return RT_EOK;
}

/*----------------------------------------------------------------------------*/
/* 寄存器操作                                                                    */
/*----------------------------------------------------------------------------*/

int lsm_reg_write(uint8_t reg, uint8_t val)
{
    return lsm_i2c_write(reg, &val, 1);
}

int lsm_reg_read(uint8_t reg, uint8_t *val)
{
    return lsm_i2c_read(reg, val, 1);
}

static int lsm_reg_modify(uint8_t reg, uint8_t mask, uint8_t bits)
{
    uint8_t val;
    int ret = lsm_reg_read(reg, &val);
    if (ret != RT_EOK) return ret;
    val = (val & ~mask) | (bits & mask);
    return lsm_reg_write(reg, val);
}

/*----------------------------------------------------------------------------*/
/* INT1 中断线程                                                                  */
/*----------------------------------------------------------------------------*/

static void lsm_int1_isr(void *args)
{
    (void)args;
    g_lsm_isr_count++;
    /* ISR 中只释放信号量, 实际处理在线程。
     * ⚠️ 必须判空: lsm_deinit 删除信号量后 / lsm_init 的 memset(g_lsm) 清零瞬间,
     * int_sem 可能是 NULL, 而 IMU 的 104Hz DRDY 边沿此刻仍可能触发本 ISR →
     * rt_sem_release(NULL) 断言挂死(实测 "fatal error on ISR")。 */
    if (g_lsm.int_sem)
        rt_sem_release(g_lsm.int_sem);
}

static void lsm_int_thread_entry(void *param)
{
    (void)param;

    while (1) {
        /* 等待 INT1 信号量 */
        rt_sem_take(g_lsm.int_sem, RT_WAITING_FOREVER);

        if (!g_lsm.initialized)
            break;

        if (g_lsm.cfg.fifo_thr > 0) {
            /* FIFO 模式: 批量读取 */
            lsm_fifo_read();
        } else {
            /* 直接模式: 读一帧并回调 */
            if (g_lsm.drdy_cb) {
                lsm_frame_t frame;
                if (lsm_read_raw(&frame) == RT_EOK) {
                    g_lsm_thread_count++;
                    lsm_filter_apply(&frame);
                    g_lsm.drdy_cb(&frame);
                }
            }
        }
    }
}

/*----------------------------------------------------------------------------*/
/* FIFO                                                                          */
/*----------------------------------------------------------------------------*/

/**
 * @brief 读取 FIFO 中当前可用的字数
 */
static int lsm_fifo_get_count(uint16_t *words)
{
    uint8_t s1, s2;
    int ret;

    ret = lsm_reg_read(LSM_REG_FIFO_STATUS1, &s1);
    if (ret != RT_EOK) return ret;
    ret = lsm_reg_read(LSM_REG_FIFO_STATUS2, &s2);
    if (ret != RT_EOK) return ret;

    *words = s1 | (((uint16_t)(s2 & FIFO_STATUS2_DIFF_MASK)) << 8);
    return RT_EOK;
}

int lsm_fifo_read(void)
{
    uint16_t words;
    int ret = lsm_fifo_get_count(&words);
    if (ret != RT_EOK) return ret;

    if (words == 0) return RT_EOK;

    /* 每帧 6 个字: G×3 + XL×3 */
    uint16_t frames = words / FIFO_WORDS_PER_FRAME;
    if (frames > LSM_FIFO_BUF_FRAMES)
        frames = LSM_FIFO_BUF_FRAMES;

    /*
     * 连续读取 FIFO_DATA_OUT 寄存器.
     * IF_INC 已开启, 但 FIFO 数据都在同一地址, 需按字循环读.
     * 每次读 2 字节 (1 个字), 共 frames×6 个字.
     */
    for (uint16_t f = 0; f < frames; f++) {
        uint8_t raw[12]; /* 6 个字 × 2 字节 = 12 字节 */
        ret = lsm_i2c_read(LSM_REG_FIFO_DATA_OUT_L, raw, sizeof(raw));
        if (ret != RT_EOK) break;

        /* 字序: G_X, G_Y, G_Z, XL_X, XL_Y, XL_Z */
        g_lsm.fifo_buf[f].gyro_raw.x  = (int16_t)(raw[0]  | ((uint16_t)raw[1]  << 8));
        g_lsm.fifo_buf[f].gyro_raw.y  = (int16_t)(raw[2]  | ((uint16_t)raw[3]  << 8));
        g_lsm.fifo_buf[f].gyro_raw.z  = (int16_t)(raw[4]  | ((uint16_t)raw[5]  << 8));
        g_lsm.fifo_buf[f].accel_raw.x = (int16_t)(raw[6]  | ((uint16_t)raw[7]  << 8));
        g_lsm.fifo_buf[f].accel_raw.y = (int16_t)(raw[8]  | ((uint16_t)raw[9]  << 8));
        g_lsm.fifo_buf[f].accel_raw.z = (int16_t)(raw[10] | ((uint16_t)raw[11] << 8));
    }

    if (g_lsm.fifo_cb && frames > 0)
        g_lsm.fifo_cb(g_lsm.fifo_buf, frames);

    return RT_EOK;
}

/*----------------------------------------------------------------------------*/
/* 换算函数                                                                      */
/*----------------------------------------------------------------------------*/

/*
 * 灵敏度表 (单位: μg/LSB, 存储为 ng/LSB×10 避免浮点)
 * 实际换算: mg = raw × sensitivity / 1000
 */
static const uint32_t xl_sensitivity_ug[4] = {
    /* 2G  */ 61,
    /* 4G  */ 122,
    /* 8G  */ 244,
    /* 16G */ 488,
};

/* 根据 lsm_xl_fs_t 枚举值获取索引 */
static uint8_t xl_fs_to_idx(lsm_xl_fs_t fs)
{
    switch (fs) {
    case LSM_XL_FS_2G:  return 0;
    case LSM_XL_FS_4G:  return 1;
    case LSM_XL_FS_8G:  return 2;
    case LSM_XL_FS_16G: return 3;
    default:            return 1;
    }
}

void lsm_xl_to_mg(const lsm_raw3_t *raw, lsm_xl_fs_t fs, lsm_data3_t *out)
{
    uint32_t sens = xl_sensitivity_ug[xl_fs_to_idx(fs)]; /* μg/LSB */
    /* mg = raw × sens / 1000 (sens 单位 μg/LSB → /1000 = mg/LSB) */
    out->x = (int32_t)((int64_t)raw->x * sens / 1000);
    out->y = (int32_t)((int64_t)raw->y * sens / 1000);
    out->z = (int32_t)((int64_t)raw->z * sens / 1000);
}

/*
 * 陀螺仪灵敏度 (单位: μdps/LSB)
 */
static const uint32_t g_sensitivity_udps[5] = {
    /* 125dps  */ 4375,
    /* 250dps  */ 8750,
    /* 500dps  */ 17500,
    /* 1000dps */ 35000,
    /* 2000dps */ 70000,
};

static uint8_t g_fs_to_idx(lsm_g_fs_t fs)
{
    switch (fs) {
    case LSM_G_FS_125DPS:  return 0;
    case LSM_G_FS_250DPS:  return 1;
    case LSM_G_FS_500DPS:  return 2;
    case LSM_G_FS_1000DPS: return 3;
    case LSM_G_FS_2000DPS: return 4;
    default:               return 2;
    }
}

void lsm_g_to_mdps(const lsm_raw3_t *raw, lsm_g_fs_t fs, lsm_data3_t *out)
{
    uint32_t sens = g_sensitivity_udps[g_fs_to_idx(fs)]; /* μdps/LSB */
    /* mdps = raw × sens / 1000 */
    out->x = (int32_t)((int64_t)raw->x * sens / 1000);
    out->y = (int32_t)((int64_t)raw->y * sens / 1000);
    out->z = (int32_t)((int64_t)raw->z * sens / 1000);
}

/*----------------------------------------------------------------------------*/
/* 低通滤波 (整数 IIR, 无浮点)                                                  */
/*----------------------------------------------------------------------------*/

/** @brief 重置滤波状态 */
static void lsm_filter_reset(void)
{
    memset(g_lsm.accel_filt, 0, sizeof(g_lsm.accel_filt));
    memset(g_lsm.gyro_filt,  0, sizeof(g_lsm.gyro_filt));
    g_lsm.filter_valid = false;
}

/**
 * @brief 对一帧原始数据应用 IIR 低通滤波 (就地修改 frame)
 *
 * 算法:  y[n] = y[n-1] + (x[n] - y[n-1]) >> shift
 *   shift=4 → α=1/16, 截止 ≈ ODR/10 (104Hz→~10Hz)
 *   shift=0 → 关闭 (skip)
 *
 * 滤波在原始值上做, 线性换算后效果等价.
 */
static void lsm_filter_apply(lsm_frame_t *frame)
{
    uint8_t s = g_lsm.filter_shift;
    if (s == 0) return;

    int32_t *gx = &g_lsm.gyro_filt[0];
    int32_t *ax = &g_lsm.accel_filt[0];

    /* 陀螺仪 Gx/Gu/Gz */
    int16_t g_raw[3] = {frame->gyro_raw.x,
                         frame->gyro_raw.y,
                         frame->gyro_raw.z};
    /* 加速度 Ax/Au/Az */
    int16_t a_raw[3] = {frame->accel_raw.x,
                         frame->accel_raw.y,
                         frame->accel_raw.z};

    if (!g_lsm.filter_valid) {
        for (int i = 0; i < 3; i++) {
            gx[i] = g_raw[i];
            ax[i] = a_raw[i];
        }
        g_lsm.filter_valid = true;
        return;
    }

    for (int i = 0; i < 3; i++) {
        gx[i] += (g_raw[i] - gx[i]) >> s;
        ax[i] += (a_raw[i] - ax[i]) >> s;
    }

    frame->gyro_raw.x = (int16_t)gx[0];
    frame->gyro_raw.y = (int16_t)gx[1];
    frame->gyro_raw.z = (int16_t)gx[2];
    frame->accel_raw.x = (int16_t)ax[0];
    frame->accel_raw.y = (int16_t)ax[1];
    frame->accel_raw.z = (int16_t)ax[2];
}

/*----------------------------------------------------------------------------*/
/* 公开 API                                                                      */
/*----------------------------------------------------------------------------*/

int lsm_reset(void)
{
    int ret = lsm_reg_write(LSM_REG_CTRL3_C, CTRL3_C_SW_RESET);
    if (ret != RT_EOK) return ret;

    /* 等待复位完成 (最大 50ms) */
    uint8_t val = CTRL3_C_SW_RESET;
    for (int i = 0; i < 50 && (val & CTRL3_C_SW_RESET); i++) {
        rt_thread_mdelay(1);
        if (lsm_reg_read(LSM_REG_CTRL3_C, &val) != RT_EOK)
            return -RT_EIO;
    }
    if (val & CTRL3_C_SW_RESET)
        return -RT_ETIMEOUT;

    return RT_EOK;
}

int lsm_read_raw(lsm_frame_t *out)
{
    uint8_t buf[12];
    /* 从 OUTX_L_G (0x22) 连续读 12 字节: G×6 + XL×6 */
    int ret = lsm_i2c_read(LSM_REG_OUTX_L_G, buf, sizeof(buf));
    if (ret != RT_EOK) return ret;

    out->gyro_raw.x  = (int16_t)(buf[0]  | ((uint16_t)buf[1]  << 8));
    out->gyro_raw.y  = (int16_t)(buf[2]  | ((uint16_t)buf[3]  << 8));
    out->gyro_raw.z  = (int16_t)(buf[4]  | ((uint16_t)buf[5]  << 8));
    out->accel_raw.x = (int16_t)(buf[6]  | ((uint16_t)buf[7]  << 8));
    out->accel_raw.y = (int16_t)(buf[8]  | ((uint16_t)buf[9]  << 8));
    out->accel_raw.z = (int16_t)(buf[10] | ((uint16_t)buf[11] << 8));

    return RT_EOK;
}

int lsm_read_status(uint8_t *status)
{
    return lsm_reg_read(LSM_REG_STATUS, status);
}

int lsm_read_temp(int32_t *temp_cdeg)
{
    uint8_t buf[2];
    int ret = lsm_i2c_read(LSM_REG_OUT_TEMP_L, buf, 2);
    if (ret != RT_EOK) return ret;

    int16_t raw = (int16_t)(buf[0] | ((uint16_t)buf[1] << 8));
    /*
     * 规格书: 0 LSB = 25°C, 灵敏度 256 LSB/°C
     * temp_cdeg = (raw / 256 + 25) × 100
     *           = raw × 100 / 256 + 2500
     */
    *temp_cdeg = (int32_t)((int32_t)raw * 100 / 256) + 2500;
    return RT_EOK;
}

int lsm_init(const lsm_config_t *cfg, lsm_drdy_cb_t drdy_cb, lsm_fifo_cb_t fifo_cb)
{
    int ret;

    rt_kprintf("[LSM] lsm_init entry\n");  /* ★ 第一条调试输出 */

    /* ⚠️ 铁律: 重建前先停掉上一次会话, 再往下 memset(g_lsm)。
     * memset 会把 g_lsm.int_sem 清零为 NULL; 若此时 INT1 中断仍使能, IMU 的 104Hz
     * DRDY 边沿随时触发 lsm_int1_isr → rt_sem_release(NULL) 断言挂死(实测第二次
     * lsm_init 未先 deinit 即 fatal error on ISR)。lsm_deinit 会关/摘 INT1 中断 +
     * 删信号量 + 停传感器输出; 末尾再无脑关一次中断兜底(未挂中断时返回错误, 无副作用)。 */
    if (g_lsm.initialized || g_lsm.int_sem)
        lsm_deinit();
    rt_pin_irq_enable(LSM_INT1_PIN, PIN_IRQ_DISABLE);

    /* 默认配置 */
    static const lsm_config_t default_cfg = {
        .xl_fs    = LSM_XL_FS_4G,
        .g_fs     = LSM_G_FS_500DPS,
        .xl_odr   = LSM_ODR_104HZ,
        .g_odr    = LSM_ODR_104HZ,
        .fifo_thr = 0,
        .use_int1     = true,
        .filter_shift = 0,  /* 关闭: 由应用层统一滤波, 避免双重滤波导致延迟 */
    };

    memset(&g_lsm, 0, sizeof(g_lsm));
    g_lsm.cfg      = cfg ? *cfg : default_cfg;
    g_lsm.drdy_cb  = drdy_cb;
    g_lsm.fifo_cb  = fifo_cb;

    /* 初始化滤波状态 */
    g_lsm.filter_shift = g_lsm.cfg.filter_shift;
    lsm_filter_reset();

    /* 1. 查找 I2C 总线 */
    g_lsm.i2c = rt_i2c_bus_device_find(LSM_I2C_BUS);
    if (!g_lsm.i2c) {
        rt_kprintf("[LSM] I2C bus '%s' not found\n", LSM_I2C_BUS);
        return -RT_ENOSYS;
    }

    /* 2. 验证 WHO_AM_I */
    uint8_t who;
    ret = lsm_reg_read(LSM_REG_WHO_AM_I, &who);
    if (ret != RT_EOK) {
        rt_kprintf("[LSM] WHO_AM_I read failed: %d\n", ret);
        return ret;
    }
    if (who != LSM_WHO_AM_I_VAL) {
        rt_kprintf("[LSM] WHO_AM_I mismatch: got 0x%02X, expect 0x%02X\n",
                   who, LSM_WHO_AM_I_VAL);
        return -RT_EIO;  /* device id mismatch */
    }

    /* 3. 软复位 */
    ret = lsm_reset();
    if (ret != RT_EOK) {
        rt_kprintf("[LSM] reset failed: %d\n", ret);
        return ret;
    }

    /* 4. CTRL3_C: BDU + IF_INC + H_LACTIVE(INT1 低有效).
     * 低有效使 DRDY 变为下降沿, 与板级 GPIO1 NEG 边沿唤醒源一致,
     * 从而 IMU 数据就绪可在 HCPU DEEP 时唤醒(连接态空闲低功耗关键). */
    ret = lsm_reg_write(LSM_REG_CTRL3_C,
                        CTRL3_C_BDU | CTRL3_C_IF_INC | CTRL3_C_H_LACTIVE);
    if (ret != RT_EOK) return ret;

    /* 5. 配置加速度计: ODR + FS */
    ret = lsm_reg_write(LSM_REG_CTRL1_XL,
                        (uint8_t)(g_lsm.cfg.xl_odr | g_lsm.cfg.xl_fs));
    if (ret != RT_EOK) return ret;

    /* 6. 配置陀螺仪: ODR + FS */
    ret = lsm_reg_write(LSM_REG_CTRL2_G,
                        (uint8_t)(g_lsm.cfg.g_odr | g_lsm.cfg.g_fs));
    if (ret != RT_EOK) return ret;

    /* 7. 配置 FIFO (如启用) */
    if (g_lsm.cfg.fifo_thr > 0) {
        uint16_t thr = g_lsm.cfg.fifo_thr;
        ret = lsm_reg_write(LSM_REG_FIFO_CTRL1, (uint8_t)(thr & 0xFF));
        if (ret != RT_EOK) return ret;
        ret = lsm_reg_modify(LSM_REG_FIFO_CTRL2, 0x0F,
                             (uint8_t)((thr >> 8) & 0x07));
        if (ret != RT_EOK) return ret;

        /* FIFO_CTRL3: G 和 XL 都以 ODR/1 存入 FIFO (decimation=1) */
        ret = lsm_reg_write(LSM_REG_FIFO_CTRL3, 0x09); /* G=1, XL=1 */
        if (ret != RT_EOK) return ret;

        /* FIFO_CTRL5: 连续模式, ODR = XL_ODR */
        ret = lsm_reg_write(LSM_REG_FIFO_CTRL5,
                            FIFO_CTRL5_MODE_ODR |
                            (uint8_t)(g_lsm.cfg.xl_odr >> 1));
        if (ret != RT_EOK) return ret;
    }

    /* 8. 配置 INT1 */
    if (g_lsm.cfg.use_int1) {
        uint8_t int1_val = 0;
        if (g_lsm.cfg.fifo_thr > 0)
            int1_val = INT1_CTRL_FIFO_TH;
        else
            int1_val = INT1_CTRL_DRDY_XL | INT1_CTRL_DRDY_G;

        ret = lsm_reg_write(LSM_REG_INT1_CTRL, int1_val);
        if (ret != RT_EOK) return ret;

        /* 创建信号量 */
        g_lsm.int_sem = rt_sem_create("lsm_int", 0, RT_IPC_FLAG_FIFO);
        if (!g_lsm.int_sem) {
            rt_kprintf("[LSM] sem create failed\n");
            return -RT_ENOMEM;
        }

        /* 创建处理线程 (优先级10, 高于普通线程, 避免被BT协议栈饿死) */
        g_lsm.int_thread = rt_thread_create("lsm",
                                             lsm_int_thread_entry, RT_NULL,
                                             4096, 10, 5);
        if (!g_lsm.int_thread) {
            rt_sem_delete(g_lsm.int_sem);
            rt_kprintf("[LSM] thread create failed\n");
            return -RT_ENOMEM;
        }
        rt_thread_startup(g_lsm.int_thread);

        {
            lsm_frame_t dummy;
            lsm_read_raw(&dummy);
            rt_kprintf("[LSM] INT1 cleared (pre-IRQ dummy read)\n");
        }

        /* 绑定 INT1 GPIO 中断. INT1 已配为低有效(H_LACTIVE),
         * DRDY 由高->低(下降沿)触发, 与 GPIO1 NEG 边沿唤醒源匹配. */
        rt_pin_mode(LSM_INT1_PIN, PIN_MODE_INPUT);
        ret = rt_pin_attach_irq(LSM_INT1_PIN, PIN_IRQ_MODE_FALLING,
                                lsm_int1_isr, RT_NULL);
        if (ret != RT_EOK) {
            rt_kprintf("[LSM] pin irq attach failed: %d\n", ret);
            return ret;
        }
        ret = rt_pin_irq_enable(LSM_INT1_PIN, PIN_IRQ_ENABLE);
        if (ret != RT_EOK) {
            rt_kprintf("[LSM] pin irq enable failed: %d\n", ret);
            return ret;
        }
    }

    g_lsm.initialized = true;

    rt_kprintf("[LSM] LSM6DS3TR-C ready  I2C=%s addr=0x%02X "
               "XL=%s/%s G=%s/%s FIFO_thr=%u INT1=%s\n",
               LSM_I2C_BUS, LSM_I2C_ADDR,
               (g_lsm.cfg.xl_odr == LSM_ODR_104HZ) ? "104Hz" : "?",
               (g_lsm.cfg.xl_fs  == LSM_XL_FS_4G)  ? "±4g"  : "?",
               (g_lsm.cfg.g_odr  == LSM_ODR_104HZ) ? "104Hz" : "?",
               (g_lsm.cfg.g_fs   == LSM_G_FS_500DPS) ? "±500dps" : "?",
               g_lsm.cfg.fifo_thr,
               g_lsm.cfg.use_int1 ? "on" : "off");

    return RT_EOK;
}

/*----------------------------------------------------------------------------*/
/* 运行态功耗切换 (连接态空闲优化 A)                                         */
/*----------------------------------------------------------------------------*/

/**
 * @brief 进入低功耗(空闲)模式: 关陀螺仪, 加速度降到 12.5Hz.
 *        陀螺仪是 IMU 主要耗电项; 关掉后电流大幅下降, 加速度保留用于
 *        "拿起/移动"检测以唤醒. INT1 已配低有效, 加速度 DRDY 下降沿可唤醒 DEEP.
 * @return 0 成功, 负值为错误
 */
int lsm_enter_lowpower(void)
{
    if (!g_lsm.initialized)
        return -RT_ENOSYS;
    int ret = lsm_reg_write(LSM_REG_CTRL2_G, LSM_ODR_OFF);   /* 关陀螺仪 */
    if (ret != RT_EOK) return ret;
    /* 加速度降到 12.5Hz (低功耗), 量程保持 */
    ret = lsm_reg_write(LSM_REG_CTRL1_XL,
                        (uint8_t)(LSM_LOWPOWER_XL_ODR | g_lsm.cfg.xl_fs));
    return ret;
}

/**
 * @brief 恢复到活跃(工作)模式: 加速度+陀螺仪均 104Hz.
 * @return 0 成功, 负值为错误
 */
int lsm_enter_active(void)
{
    if (!g_lsm.initialized)
        return -RT_ENOSYS;
    int ret = lsm_reg_write(LSM_REG_CTRL1_XL,
                            (uint8_t)(g_lsm.cfg.xl_odr | g_lsm.cfg.xl_fs));
    if (ret != RT_EOK) return ret;
    ret = lsm_reg_write(LSM_REG_CTRL2_G,
                        (uint8_t)(g_lsm.cfg.g_odr | g_lsm.cfg.g_fs));
    return ret;
}

void lsm_deinit(void)
{
    g_lsm.initialized = false;

    /* 停止并删除轮询定时器 */
    if (g_lsm_poll_running) {
        rt_timer_stop(g_lsm_poll_timer);
        rt_timer_delete(g_lsm_poll_timer);
        g_lsm_poll_timer  = RT_NULL;
        g_lsm_poll_running = false;
    }

    if (g_lsm.cfg.use_int1) {
        rt_pin_irq_enable(LSM_INT1_PIN, PIN_IRQ_DISABLE);
        rt_pin_detach_irq(LSM_INT1_PIN);
    }

    if (g_lsm.int_sem) {
        /* 唤醒线程使其退出 */
        rt_sem_release(g_lsm.int_sem);
        rt_thread_mdelay(10);
        rt_sem_delete(g_lsm.int_sem);
        g_lsm.int_sem = RT_NULL;
    }

    /* 关闭传感器输出 */
    lsm_reg_write(LSM_REG_CTRL1_XL, LSM_ODR_OFF);
    lsm_reg_write(LSM_REG_CTRL2_G,  LSM_ODR_OFF);
}

/*----------------------------------------------------------------------------*/
/* Shell 调试命令                                                              */
/*----------------------------------------------------------------------------*/

static void cmd_lsm_read(int argc, char **argv)
{
    (void)argc; (void)argv;
    if (!g_lsm.initialized) {
        rt_kprintf("[LSM] not initialized\n");
        return;
    }
    lsm_frame_t frame;
    if (lsm_read_raw(&frame) == RT_EOK) {
        lsm_data3_t a, g;
        lsm_xl_to_mg(&frame.accel_raw, g_lsm.cfg.xl_fs, &a);
        lsm_g_to_mdps(&frame.gyro_raw, g_lsm.cfg.g_fs, &g);
        rt_kprintf("[LSM] A=(%+d,%+d,%+d)mg  G=(%+d,%+d,%+d)mdps  (raw read)\n",
                   a.x, a.y, a.z, g.x, g.y, g.z);
    } else {
        rt_kprintf("[LSM] read failed\n");
    }
}

static void cmd_lsm_status(int argc, char **argv)
{
    (void)argc; (void)argv;
    rt_kprintf("[LSM] initialized=%d  isr_count=%lu  thread_count=%lu\n",
               g_lsm.initialized, g_lsm_isr_count, g_lsm_thread_count);

    uint8_t status = 0;
    if (lsm_read_status(&status) == RT_EOK) {
        rt_kprintf("[LSM] STATUS=0x%02X  XLDA=%d  GDA=%d  TDA=%d\n",
                   status,
                   (status & LSM_STATUS_XLDA) ? 1 : 0,
                   (status & LSM_STATUS_GDA)  ? 1 : 0,
                   (status & LSM_STATUS_TDA)  ? 1 : 0);
    }

    /* 读 INT1_CTRL 和 STATUS 寄存器确认中断源 */
    uint8_t int1_ctrl = 0, ctrl1 = 0, ctrl2 = 0;
    lsm_reg_read(LSM_REG_INT1_CTRL, &int1_ctrl);
    lsm_reg_read(LSM_REG_CTRL1_XL,  &ctrl1);
    lsm_reg_read(LSM_REG_CTRL2_G,   &ctrl2);
    rt_kprintf("[LSM] INT1_CTRL=0x%02X  CTRL1_XL=0x%02X  CTRL2_G=0x%02X\n",
               int1_ctrl, ctrl1, ctrl2);
}

static void cmd_lsm_reg(int argc, char **argv)
{
    if (argc < 2) {
        rt_kprintf("Usage: lsm_reg <reg> [val]\n");
        return;
    }
    uint8_t reg = (uint8_t)strtol(argv[1], NULL, 0);
    if (argc >= 3) {
        uint8_t val = (uint8_t)strtol(argv[2], NULL, 0);
        int ret = lsm_reg_write(reg, val);
        rt_kprintf("[LSM] write reg 0x%02X = 0x%02X: %d\n", reg, val, ret);
    } else {
        uint8_t val = 0;
        int ret = lsm_reg_read(reg, &val);
        rt_kprintf("[LSM] read reg 0x%02X = 0x%02X: %d\n", reg, val, ret);
    }
}

MSH_CMD_EXPORT(cmd_lsm_read,   Read one LSM6DS3 frame);
MSH_CMD_EXPORT(cmd_lsm_status,  Show LSM6DS3 status and ISR count);
MSH_CMD_EXPORT(cmd_lsm_reg,    Read/write LSM6DS3 register);

/*----------------------------------------------------------------------------*/
/* 轮询模式 (不依赖 INT1 中断, 用于调试)                                  */
/*----------------------------------------------------------------------------*/

static void lsm_poll_timer_cb(void *param)
{
    (void)param;
    if (!g_lsm.initialized || !g_lsm.drdy_cb)
        return;

    /*
     * ISR 上下文: 不能调用任何可能拿 mutex 的函数
     * (rt_i2c_transfer 内部会 rt_mutex_take)
     * 直接 release semaphore, 由 lsm_int_thread_entry 线程安全读取
     */
    rt_sem_release(g_lsm.int_sem);
}

static void cmd_lsm_poll(int argc, char **argv)
{
    if (g_lsm_poll_running) {
        rt_kprintf("[LSM] poll already running, use 'lsm_poll_stop' to stop\n");
        return;
    }
    if (!g_lsm.initialized) {
        rt_kprintf("[LSM] not initialized\n");
        return;
    }

    uint32_t period_ms = 10;  /* 默认 10ms ≈ 100Hz */
    if (argc >= 2)
        period_ms = (uint32_t)strtoul(argv[1], NULL, 0);

    g_lsm_poll_timer = rt_timer_create("lsm_poll",
                                       lsm_poll_timer_cb,
                                       RT_NULL,
                                       rt_tick_from_millisecond(period_ms),
                                       RT_TIMER_FLAG_PERIODIC);
    if (!g_lsm_poll_timer) {
        rt_kprintf("[LSM] timer create failed\n");
        return;
    }
    rt_timer_start(g_lsm_poll_timer);
    g_lsm_poll_running = true;
    rt_kprintf("[LSM] poll started, period=%lums\n", period_ms);
}

static void cmd_lsm_poll_stop(int argc, char **argv)
{
    (void)argc; (void)argv;
    if (!g_lsm_poll_running) {
        rt_kprintf("[LSM] poll not running\n");
        return;
    }
    rt_timer_stop(g_lsm_poll_timer);
    rt_timer_delete(g_lsm_poll_timer);
    g_lsm_poll_timer  = RT_NULL;
    g_lsm_poll_running = false;
    rt_kprintf("[LSM] poll stopped\n");
}

MSH_CMD_EXPORT(cmd_lsm_poll,      Start LSM6DS3 poll mode: lsm_poll [period_ms]);
MSH_CMD_EXPORT(cmd_lsm_poll_stop, Stop LSM6DS3 poll mode);
