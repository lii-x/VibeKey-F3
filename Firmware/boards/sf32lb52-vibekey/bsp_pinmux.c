#include "bsp_board.h"


#define BSP_KEY_C1_PIN 25
#define BSP_KEY_C2_PIN 27
#define BSP_KEY_C3_PIN 32
#define BSP_KEY_L1_PIN 31
#define BSP_KEY_L2_PIN 24
#define BSP_KEY_L3_PIN 38
#define BSP_EC_KEY_PIN 30




static void BSP_PIN_Common(void)
{
#ifdef SOC_BF0_HCPU
    // UART1 - debug
    HAL_PIN_Set(PAD_PA18, USART1_RXD, PIN_PULLUP, 1);
    HAL_PIN_Set(PAD_PA19, USART1_TXD, PIN_PULLUP, 1);
    // Key
    HAL_PIN_Set(PAD_PA00 + 34, GPIO_A0 + 34, PIN_PULLDOWN, 1);

    // LSM6DS3 IMU (I2C1 + INT1)
    HAL_PIN_Set(PAD_PA10, I2C1_SDA, PIN_PULLUP, 1);
    HAL_PIN_Set(PAD_PA11, I2C1_SCL, PIN_PULLUP, 1);
    HAL_PIN_Set(PAD_PA26, GPIO_A26, PIN_PULLUP, 1);   /* LSM INT1 */

    // PA44: 原 Type-C 检测脚。10-05「USB 检测并入充电检测(方案B)」后软件不再读它，
    // 板级已无用途 ⇒ 按"预留悬空脚"处理(同 PA40)：锁"输入下拉"防漏电。
    // (SoC 复位默认上拉输入，悬空脚在活跃态就漏电/振荡；改前是 GPIO 输入 + PIN_NOPULL。)
    BSP_GPIO_SetInputPD(44, 1);

    //RGB PWM
    HAL_PIN_Set(PAD_PA00, GPTIM2_CH1, PIN_NOPULL, 1);

    // SIQ-02FVS3 旋转编码器
    HAL_PIN_Set(PAD_PA02, GPIO_A2, PIN_PULLUP, 1);   /* ENC_A */
    HAL_PIN_Set(PAD_PA04, GPIO_A4, PIN_PULLUP, 1);   /* ENC_B */

    // PA40: 预留脚, 不用 → 开机即配"输入下拉"(最低功耗, 输出缓冲器关闭)
    BSP_GPIO_SetInputPD(40, 1);

    // 未用(NC)脚批量"输入下拉"(08-30): SoC 复位默认上拉输入, 活跃态就漏电,
    // 开机 init 即根治; 与 BSP_PowerDownCustom(进 DEEP 前)共用同一实现。
    BSP_GPIO_InitUnusedPD();

    // LEDs
    // HAL_PIN_Set(PAD_PA00 + BSP_LED1_PIN, GPIO_A0 + BSP_LED1_PIN, PIN_NOPULL, 1);
    // HAL_PIN_Set(PAD_PA00 + BSP_LED2_PIN, GPIO_A0 + BSP_LED2_PIN, PIN_NOPULL, 1);
    // HAL_PIN_Set(PAD_PA00 + BSP_LED3_PIN, GPIO_A0 + BSP_LED3_PIN, PIN_NOPULL, 1);
    // HAL_PIN_Set(PAD_PA00 + BSP_LED4_PIN, GPIO_A0 + BSP_LED4_PIN, PIN_NOPULL, 1);
#endif
}

void BSP_PIN_Init(void)
{
    BSP_PIN_Common();
}
