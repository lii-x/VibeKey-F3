/**
 ******************************************************************************
 * @file    lsm6ds3.h
 * @brief   LSM6DS3TR-C 六轴 IMU 驱动 (I2C, 轻量裸驱动)
 *
 * ST LSM6DS3TR-C
 *   - 3轴加速度计 + 3轴陀螺仪, 内置温度传感器
 *   - I2C 地址: 0x6A (SA0=GND) / 0x6B (SA0=VCC)
 *   - 8kB FIFO, INT1/INT2 可编程中断输出
 *   - 供电: 1.71~3.6V, I2C 最高 400kHz
 *
 * 引脚连接 (本项目):
 *   SDA  → PA10 (I2C1_SDA)
 *   SCL  → PA11 (I2C1_SCL)
 *   INT1 → PA9  (LSM_INT, 数据就绪/FIFO 阈值)
 *   SDO/SA0 → GND/VCC (决定 I2C 地址)
 *
 * 功能:
 *   1. 加速度计 / 陀螺仪 原始数据读取
 *   2. 换算为 mg (加速度) 和 mdps (角速度)
 *   3. FIFO 批量读取 (ODR 模式)
 *   4. INT1 GPIO 中断触发数据就绪回调
 ******************************************************************************
 */

#ifndef __LSM6DS3_H__
#define __LSM6DS3_H__

#include "rtthread.h"
#include "rtdevice.h"
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/*==============================================================================
 * 硬件配置 (可在编译选项中覆盖)
 *============================================================================*/

/* RT-Thread I2C 总线名称 */
#ifndef LSM_I2C_BUS
#define LSM_I2C_BUS         "i2c1"
#endif

/* I2C 7-bit 地址: SA0=GND→0x6A, SA0=VCC→0x6B */
#ifndef LSM_I2C_ADDR
#define LSM_I2C_ADDR        0x6A
#endif

/* INT1 GPIO 引脚号 (RT-Thread pin number: PA26 = 26) */
#ifndef LSM_INT1_PIN
#define LSM_INT1_PIN        26
#endif

/*==============================================================================
 * 寄存器地址
 *============================================================================*/

#define LSM_REG_FUNC_CFG_ACCESS     0x01
#define LSM_REG_FIFO_CTRL1          0x06
#define LSM_REG_FIFO_CTRL2          0x07
#define LSM_REG_FIFO_CTRL3          0x08
#define LSM_REG_FIFO_CTRL4          0x09
#define LSM_REG_FIFO_CTRL5          0x0A
#define LSM_REG_INT1_CTRL           0x0D
#define LSM_REG_INT2_CTRL           0x0E
#define LSM_REG_WHO_AM_I            0x0F  /* 期望值: 0x6A */
#define LSM_REG_CTRL1_XL            0x10  /* 加速度计控制 */
#define LSM_REG_CTRL2_G             0x11  /* 陀螺仪控制 */
#define LSM_REG_CTRL3_C             0x12  /* 通用控制 (软复位, BDU, IF_INC) */
#define LSM_REG_CTRL4_C             0x13
#define LSM_REG_CTRL5_C             0x14
#define LSM_REG_CTRL6_C             0x15
#define LSM_REG_CTRL7_G             0x16
#define LSM_REG_CTRL8_XL            0x17
#define LSM_REG_CTRL9_XL            0x18
#define LSM_REG_CTRL10_C            0x19
#define LSM_REG_STATUS              0x1E  /* 数据就绪标志 */
#define LSM_REG_OUT_TEMP_L          0x20
#define LSM_REG_OUT_TEMP_H          0x21
#define LSM_REG_OUTX_L_G            0x22  /* 陀螺仪 X 低字节 */
#define LSM_REG_OUTX_H_G            0x23
#define LSM_REG_OUTY_L_G            0x24
#define LSM_REG_OUTY_H_G            0x25
#define LSM_REG_OUTZ_L_G            0x26
#define LSM_REG_OUTZ_H_G            0x27
#define LSM_REG_OUTX_L_XL           0x28  /* 加速度计 X 低字节 */
#define LSM_REG_OUTX_H_XL           0x29
#define LSM_REG_OUTY_L_XL           0x2A
#define LSM_REG_OUTY_H_XL           0x2B
#define LSM_REG_OUTZ_L_XL           0x2C
#define LSM_REG_OUTZ_H_XL           0x2D
#define LSM_REG_FIFO_STATUS1        0x3A
#define LSM_REG_FIFO_STATUS2        0x3B
#define LSM_REG_FIFO_STATUS3        0x3C
#define LSM_REG_FIFO_STATUS4        0x3D
#define LSM_REG_FIFO_DATA_OUT_L     0x3E
#define LSM_REG_FIFO_DATA_OUT_H     0x3F

/* WHO_AM_I 期望值 */
#define LSM_WHO_AM_I_VAL            0x6A

/* STATUS 寄存器位 */
#define LSM_STATUS_XLDA             (1u << 0)  /* 加速度计数据就绪 */
#define LSM_STATUS_GDA              (1u << 1)  /* 陀螺仪数据就绪 */
#define LSM_STATUS_TDA              (1u << 2)  /* 温度数据就绪 */

/*==============================================================================
 * 量程 / ODR 枚举
 *============================================================================*/

/** 加速度计量程 */
typedef enum {
    LSM_XL_FS_2G  = 0x00,  /* ±2g,  灵敏度 0.061 mg/LSB */
    LSM_XL_FS_4G  = 0x08,  /* ±4g,  灵敏度 0.122 mg/LSB */
    LSM_XL_FS_8G  = 0x0C,  /* ±8g,  灵敏度 0.244 mg/LSB */
    LSM_XL_FS_16G = 0x04,  /* ±16g, 灵敏度 0.488 mg/LSB */
} lsm_xl_fs_t;

/** 陀螺仪量程 */
typedef enum {
    LSM_G_FS_125DPS  = 0x02,  /* ±125 dps,  灵敏度  4.375 mdps/LSB */
    LSM_G_FS_250DPS  = 0x00,  /* ±250 dps,  灵敏度  8.75  mdps/LSB */
    LSM_G_FS_500DPS  = 0x04,  /* ±500 dps,  灵敏度 17.5   mdps/LSB */
    LSM_G_FS_1000DPS = 0x08,  /* ±1000 dps, 灵敏度 35.0   mdps/LSB */
    LSM_G_FS_2000DPS = 0x0C,  /* ±2000 dps, 灵敏度 70.0   mdps/LSB */
} lsm_g_fs_t;

/** 输出数据率 (ODR) */
typedef enum {
    LSM_ODR_OFF   = 0x00,
    LSM_ODR_12HZ  = 0x10,
    LSM_ODR_26HZ  = 0x20,
    LSM_ODR_52HZ  = 0x30,
    LSM_ODR_104HZ = 0x40,
    LSM_ODR_208HZ = 0x50,
    LSM_ODR_416HZ = 0x60,
    LSM_ODR_833HZ = 0x70,
} lsm_odr_t;

/*==============================================================================
 * 数据结构
 *============================================================================*/

/** 三轴原始数据 */
typedef struct {
    int16_t x;
    int16_t y;
    int16_t z;
} lsm_raw3_t;

/** 三轴物理量 (加速度: mg; 角速度: mdps) */
typedef struct {
    int32_t x;  /* mg 或 mdps, ×1000 精度 */
    int32_t y;
    int32_t z;
} lsm_data3_t;

/** 一帧 IMU 数据 (从输出寄存器或 FIFO 读取) */
typedef struct {
    lsm_raw3_t accel_raw;   /* 加速度计原始值 */
    lsm_raw3_t gyro_raw;    /* 陀螺仪原始值 */
} lsm_frame_t;

/** 驱动配置 */
typedef struct {
    lsm_xl_fs_t xl_fs;      /* 加速度计量程, 默认 ±4g */
    lsm_g_fs_t  g_fs;       /* 陀螺仪量程,   默认 ±500 dps */
    lsm_odr_t   xl_odr;     /* 加速度计 ODR, 默认 104Hz */
    lsm_odr_t   g_odr;      /* 陀螺仪 ODR,   默认 104Hz */
    uint16_t    fifo_thr;   /* FIFO 阈值 (样本数, 0=不使用 FIFO) */
    bool        use_int1;   /* 是否启用 INT1 中断 */
    uint8_t     filter_shift; /* 低通滤波强度: 0=关闭, 2~6 推荐 */
} lsm_config_t;

/**
 * 数据就绪回调 (INT1 或轮询时触发)
 *   @param frame  本次读取的加速度计+陀螺仪原始数据
 */
typedef void (*lsm_drdy_cb_t)(const lsm_frame_t *frame);

/**
 * FIFO 阈值回调
 *   @param buf   FIFO 读取缓冲区 (每帧 lsm_frame_t)
 *   @param count 帧数
 */
typedef void (*lsm_fifo_cb_t)(const lsm_frame_t *buf, uint16_t count);

/*==============================================================================
 * API
 *============================================================================*/

/**
 * @brief 初始化 LSM6DS3TR-C
 * @param cfg       驱动配置 (NULL 使用默认值)
 * @param drdy_cb   数据就绪回调 (NULL 则不注册)
 * @param fifo_cb   FIFO 阈值回调 (NULL 或 fifo_thr=0 则不使用 FIFO)
 * @return 0: 成功, 负值: 错误
 */
int lsm_init(const lsm_config_t *cfg, lsm_drdy_cb_t drdy_cb, lsm_fifo_cb_t fifo_cb);

/** @brief 去初始化, 停止中断和定时器 */
void lsm_deinit(void);

/**
 * @brief 进入低功耗(空闲)模式: 关陀螺仪, 加速度降到 12.5Hz.
 *        连接态空闲优化(A): 大幅降低 IMU 电流, 加速度保留用于唤醒.
 * @return 0 成功, 负值为错误
 */
int lsm_enter_lowpower(void);

/**
 * @brief 恢复活跃(工作)模式: 加速度+陀螺仪均 104Hz.
 * @return 0 成功, 负值为错误
 */
int lsm_enter_active(void);

/** @brief 软复位芯片 */
int lsm_reset(void);

/** @brief 读取一帧加速度计+陀螺仪原始数据 */
int lsm_read_raw(lsm_frame_t *out);

/**
 * @brief 将原始加速度值换算为 mg (×10^-3 g)
 * @param raw   原始三轴数据
 * @param fs    量程配置
 * @param out   输出 mg 值 (×1000, 即 1234 表示 1.234 g)
 */
void lsm_xl_to_mg(const lsm_raw3_t *raw, lsm_xl_fs_t fs, lsm_data3_t *out);

/**
 * @brief 将原始角速度值换算为 mdps (×10^-3 dps)
 * @param raw   原始三轴数据
 * @param fs    量程配置
 * @param out   输出 mdps 值 (×1000, 即 12345 表示 12.345 dps)
 */
void lsm_g_to_mdps(const lsm_raw3_t *raw, lsm_g_fs_t fs, lsm_data3_t *out);

/** @brief 读取内置温度 (单位: 0.01 °C, 即 2530 = 25.30°C) */
int lsm_read_temp(int32_t *temp_cdeg);

/** @brief 手动触发 FIFO 读取并调用 fifo_cb (轮询场景使用) */
int lsm_fifo_read(void);

/** @brief 读取 STATUS 寄存器 */
int lsm_read_status(uint8_t *status);

/** @brief 直接读取寄存器 (调试用) */
int lsm_reg_read(uint8_t reg, uint8_t *val);

/** @brief 直接写寄存器 (调试用) */
int lsm_reg_write(uint8_t reg, uint8_t val);

#ifdef __cplusplus
}
#endif

#endif /* __LSM6DS3_H__ */
