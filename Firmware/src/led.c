#include "led.h"
#include "rtconfig.h"
#include "bf0_hal.h"
#include "drv_io.h"


void led_init(void)
{
    GPIO_InitTypeDef GPIO_InitStruct;
    GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Pin  = BSP_LED1_PIN;
    HAL_GPIO_Init(hwp_gpio1, &GPIO_InitStruct);
    GPIO_InitStruct.Pin  = BSP_LED2_PIN;
    HAL_GPIO_Init(hwp_gpio1, &GPIO_InitStruct);
    GPIO_InitStruct.Pin  = BSP_LED3_PIN;
    HAL_GPIO_Init(hwp_gpio1, &GPIO_InitStruct);
    GPIO_InitStruct.Pin  = BSP_LED4_PIN;
    HAL_GPIO_Init(hwp_gpio1, &GPIO_InitStruct);

    HAL_PIN_Set(PAD_PA00 + BSP_LED1_PIN, GPIO_A0 + BSP_LED1_PIN, PIN_NOPULL, 1);
    HAL_PIN_Set(PAD_PA00 + BSP_LED2_PIN, GPIO_A0 + BSP_LED2_PIN, PIN_NOPULL, 1);
    HAL_PIN_Set(PAD_PA00 + BSP_LED3_PIN, GPIO_A0 + BSP_LED3_PIN, PIN_NOPULL, 1);
    HAL_PIN_Set(PAD_PA00 + BSP_LED4_PIN, GPIO_A0 + BSP_LED4_PIN, PIN_NOPULL, 1);

    HAL_GPIO_WritePin(hwp_gpio1, BSP_LED1_PIN, !BSP_LED1_ACTIVE);
    HAL_GPIO_WritePin(hwp_gpio1, BSP_LED2_PIN, !BSP_LED2_ACTIVE);
    HAL_GPIO_WritePin(hwp_gpio1, BSP_LED3_PIN, !BSP_LED3_ACTIVE);
    HAL_GPIO_WritePin(hwp_gpio1, BSP_LED4_PIN, !BSP_LED4_ACTIVE);
}

void led_status(uint8_t pin, uint8_t mode)
{
    HAL_GPIO_WritePin(hwp_gpio1, pin, mode);
}

void led_status_mutex(uint8_t pin, uint8_t status)
{
    HAL_GPIO_WritePin(hwp_gpio1, BSP_LED2_PIN, !BSP_LED2_ACTIVE);
    HAL_GPIO_WritePin(hwp_gpio1, BSP_LED3_PIN, !BSP_LED3_ACTIVE);
    HAL_GPIO_WritePin(hwp_gpio1, BSP_LED4_PIN, !BSP_LED4_ACTIVE);
    if (status) HAL_GPIO_WritePin(hwp_gpio1, pin, status);
}
