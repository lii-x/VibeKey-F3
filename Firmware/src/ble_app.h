#ifndef __BLE_APP_H__
#define __BLE_APP_H__

#include <stdint.h>

/* Init */
void app_bt_hid_init(void);
void app_ble_stop_advertising(void);



/* BAS */
void ble_bas_init(void);
void ble_bas_set_level(uint8_t percent);

/* Battery status getters (for low-battery monitor in power.c) */
uint8_t app_get_battery_percent(void);
uint8_t app_battery_valid(void);
/* 09-16: 最近一次成功读到的电池电压(mV)与读数序号(自增) —— 低电关机判定用,
 * 序号变化说明"这次真的重新读到了值", 借此排除陈旧读数导致的误判/漏判。 */
uint32_t app_get_battery_mv(void);
uint32_t app_get_battery_read_seq(void);
/* 强制重采样电量(ADC 读+计算, 内含 300ms 延时): 待机 RTC 唤醒检查用(09-09) */
void ble_app_battery_refresh(void);

extern void ble_app_advertising_start(void);

/* Air Mouse 生命周期：由 BLE 连接/断开事件驱动（阶段1：从主循环轮询改到事件回调）*/
void air_mouse_start(void);
void air_mouse_stop(void);

/* 断连后“按键唤醒再广播”：让设备断连后仍可被重新发现（阶段1低功耗配套）*/
void ble_app_wakeup_advertise(void);

/* 09-13 连接态待机: 主动断开 BLE 链路(并抑制断开事件的自动重广播), 让深睡真正断射频。
 * 不接的话小核会一直维持 BLE 链路, 待机电流 ~0.3mA 而非 0.1mA。由 power.c 进待机前调用。 */
void app_ble_teardown_for_standby(void);

/* 连接态动态连接参数 (空闲优化 C): 活跃收紧(低延迟) / 空闲放宽(省电) */
void ble_app_conn_param_tighten(void);
void ble_app_conn_param_relax(void);
/* 空闲深睡: 强制放松 BLE 连接间隔(绕过 2s 节流), 削 BLE 射频基线; 由
 * main.c 空中鼠标进入空闲态时调用一次, 拿起由 set_active(active) 内部 tighten 恢复 */
void ble_app_conn_param_relax_idle(void);
/* 按当前活动状态恢复参数 (SCO 断开/HFP 恢复时调用): 活跃→tighten, 空闲→relax */
void ble_app_conn_param_by_activity(void);
/* 通话(SCO)期间 BLE 尽量安静：拉长间隔避开与 SCO 争抢 2.4GHz 射频 */
void ble_app_conn_param_sco_quiet(void);
/* 结束语音识别: 发 AT+BVRA=0 走 HFP 链路(不依赖 BLE), C1 语音开关键按下且
 * SCO 活跃时由 main.c 调用, 与键盘报告双通道兜底"结束语音" */
void ble_app_hfp_deactivate_vf(void);

/* PTT(C1 语音开关键)按下时提前退出 HFP sniff，为 SCO 建链铺路(避免语音开头丢) */
void ble_app_exit_sniff_for_call(void);

/* 当前 BLE 连接间隔(×1.25ms), 由 ble_app.c 在连接/参数协商事件维护;
 * main.c 的 mouse_send_thread 据此自适应发送节拍(max(10ms, 间隔ms+2ms)),
 * 语音期 sco_quiet 宽间隔下不超发不丢帧(08-26)。 */
extern volatile uint16_t g_ble_cur_interval;

/* HFP(经典蓝牙免提)链路连接状态查询(power.c RC 重校门控用) */
uint8_t ble_app_hfp_connected(void);

#endif
