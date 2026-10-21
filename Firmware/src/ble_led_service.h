#ifndef __BLE_LED_SERVICE_H__
#define __BLE_LED_SERVICE_H__

#include <stdint.h>

void ble_led_service_init(void);
void ble_led_register_service(void);
void ble_led_set_state(uint8_t state);
void ble_led_turn_off(void);

/* 09-03 (BLE 改建键配置): conf_task 线程处理完 CONF 命令后, 经 0xFF04 特征
 * notify 一帧回包给 PC。SDK sibles_write_value 内部同步拷贝+消息投递, 线程安全,
 * 可从 conf_task 线程直接调用(见 bf0_sibles.c acquire_tx_pkts/send_value)。 */
void ble_led_cfg_reply(uint8_t conn_idx, const uint8_t *data, uint16_t len);

/* LED / 反馈状态 */
#define LED_STATE_OFF     0
#define LED_STATE_BUSY    1
#define LED_STATE_DONE    2
#define LED_STATE_ERROR   3
/* 10-21: 蓝灯(手动测试用)。蓝灯物理上就是 LED1(PA5) —— 按 bt_slot.h 的归属约定
 * LED1 属 bt_slot(充电指示/鼠标故障闪烁), 本服务只占 LED2/3/4, 故此态是【借用】。 */
#define LED_STATE_BLUE    4

#endif
