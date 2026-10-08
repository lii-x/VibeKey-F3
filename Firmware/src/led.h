#ifndef __LED_H__
#define __LED_H__

#include "bf0_hal.h"

// #define BSP_LED1_PIN          30
// #define BSP_LED2_PIN          31
// #define BSP_LED3_PIN          32
// #define BSP_LED4_PIN          28

#define BSP_LED1_ACTIVE_HIGH  GPIO_PIN_SET
#define BSP_LED2_ACTIVE_HIGH  GPIO_PIN_SET
#define BSP_LED3_ACTIVE_HIGH  GPIO_PIN_SET
#define BSP_LED4_ACTIVE_HIGH  GPIO_PIN_SET

void led_init(void);
void led_status(uint8_t pin, uint8_t mode);
void led_status_mutex(uint8_t pin, uint8_t status);

#endif /* __LED_H__ */
