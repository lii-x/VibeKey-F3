#ifndef __POWER_H__
#define __POWER_H__

#include <stdint.h>

#define POWER_KEY_PIN       34
#define POWER_LONG_PRESS_MS 3000

/* ===== USB 插入检测并入充电检测（方案B）配套开关 =====
 * 固件的"USB 插入检测"判据已统一为内置充电器 CHG_SR 的 VBUS_RDY 位（与充电指示同源），
 * 不再用 PA44 GPIO 边沿中断。由此产生的副作用是"DEEP 待机中插 USB 不再有 GPIO 唤醒"，
 * 本开关用于（尝试）补上充电器侧的唤醒链路。
 *
 * ★★★ 10-05 真机实测结论：**置 1 会拖垮 DEEP，今起固定为 0** ★★★
 *   现象：置 1 后拔掉 USB 进待机，电流停在 **0.9mA**（应 0.1mA）。日志铁证：
 *     WSR=0x00000008 WER=0x0041cbcb pend=0x00000008
 *     → quiesce TIMEOUT (DEEP will degrade to IDLE!)
 *   机理：本开关使能的 WER bit3 = `HPSYS_AON_WER_PMUC`；
 *         PMUC 侧 `HAL_PMU_EnableChgWakeup()` 已开，且 FORCE_RST 后 CHG_CR4 回默认值带 IE，
 *         PMUC 请求线持续有效 ⇒ **WSR bit3(WSR_PMUC) 被反复锁存，清掉立刻重挂**。
 *         `sifli_suspend` 判据是 `WSR & WER != 0 → EBUSY`，于是 **DEEP 永久降级 IDLE** → 0.9mA。
 *   （这与本文件/代码里反复警告的"电平模式锁存 WSR ⇒ DEEP 降级 IDLE"是同一类坑。）
 *
 *   更深一层：待机省电靠 `power_analog_domain_off()` 对充电器 **FORCE_RST**（省几十~上百 µA），
 *   而复位的充电器模拟前端基本无法再产生 VBUS 事件 ⇒
 *   **"DEEP 中插 USB 唤醒"与"FORCE_RST 省电"本身互斥**，这笔账算不过来。
 *   ⇒ 结论：DEEP 中插 USB 不唤醒（按一下键 / RTC 唤醒后由 usb_resume_if_plugged()
 *     按 VBUS 真实状态补枚举，功能不受影响，只是不即时）。
 *
 *   1 = 尝试启用上述 PMUC 唤醒链路（**已知会降级 DEEP，勿用**，仅留作将来重测用）。
 *   0 = 【当前值】CHG_CR4 中断使能位全清（= 原 EOC 风暴对策原样）+ 不使能 AON PMUC 路由。
 *
 *   万一将来要重试置 1：**必须**先确认"DEEP 中 WSR_PMUC 不会持续锁存"
 *   （即 PMUC 请求线在睡觉前能被真正清干净），否则必然重现 0.9mA。 */
#define USB_WAKE_BY_PMUC    0

void power_init(void);
void power_set_bt_connected(uint8_t connected);
void power_bt_indicator_sleep(void);   /* 进深睡：灭蓝灯 + 停闪烁定时器 + 电池监控降频 */
void power_bt_indicator_active(void);  /* 唤醒广播：恢复蓝灯闪烁 + 电池监控节拍 */

/* 统一待机唤醒入口：置 g_wakeup_requested + release 待机信号量，唤醒 sleep_thread。
 * 调用方：
 *  - DEEP 唤醒：BSP_PowerUpCustom 检测到 WSR 含 PIN/GPIO1 位时调用；
 *  - IDLE/LIGHT 降级兜底：key_config.c 的 key_handler（按键 GPIO 中断路径）。
 * 仅当处于待机态(g_standby_active=1)时生效，活跃态调用直接忽略（无副作用）。 */
void power_standby_wakeup(void);

/* LDO3 域（IMU/RGB/I2C+ENC 上拉）断电/上电，严格三步顺序见 power.c 函数头注释。
 * ⚠️ BSP_ImuPowerWanted() 等任何想给该域上电的逻辑，必须先用 power_ldo3_is_off() 判定：
 *    仅当 LDO3 已断电(g_ldo3_off=1)时才能安全上电，否则引脚仍驱动低电平会反灌。 */
int  power_ldo3_is_off(void);
void power_ldo3_domain_off(void);
void power_ldo3_domain_on(void);

#endif /* __POWER_H__ */
