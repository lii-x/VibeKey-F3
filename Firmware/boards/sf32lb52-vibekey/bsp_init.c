/*
 * SPDX-FileCopyrightText: 2019-2022 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include "bsp_board.h"
#include "bf0_hal_rtc.h"

#ifndef LXT_LP_CYCLE
    #define LXT_LP_CYCLE 200
#endif

static uint16_t mpi1_div = 1;
static uint16_t mpi2_div = 1;

static uint32_t otp_flash_addr = AUTO_FLASH_MAC_ADDRESS;

#define FUNC_BSP_FLASH_DIV_GET(i) \
uint16_t BSP_GetFlash##i##DIV(void) \
{ \
    return mpi##i##_div; \
}\

#define FUNC_BSP_FLASH_DIV_SET(i) \
void BSP_SetFlash##i##DIV(uint16_t div) \
{ \
    mpi##i##_div = div; \
}\

FUNC_BSP_FLASH_DIV_GET(1);
FUNC_BSP_FLASH_DIV_GET(2);

FUNC_BSP_FLASH_DIV_SET(1)
FUNC_BSP_FLASH_DIV_SET(2)

int rt_psram_init(void);
int rt_hw_flash1_init(uint8_t auto_detect);
int rt_hw_flash2_init(uint8_t auto_detect);
int rt_hw_flash_init(void);

uint32_t BSP_GetOtpBase(void)
{
    return otp_flash_addr;
}

#ifdef SOC_BF0_HCPU
#define HXT_DELAY_EXP_VAL 1000
static void LRC_init(void)
{
    HAL_PMU_RC10Kconfig();

    HAL_RC_CAL_update_reference_cycle_on_48M(LXT_LP_CYCLE);
    uint32_t ref_cnt = HAL_RC_CAL_get_reference_cycle_on_48M();
    uint32_t cycle_t = (uint32_t)ref_cnt / (48 * LXT_LP_CYCLE);

    HAL_PMU_SET_HXT3_RDY_DELAY((HXT_DELAY_EXP_VAL / cycle_t + 1));
}
#endif

void HAL_PreInit(void)
{
#ifdef SOC_BF0_HCPU
    if (RCC_SYSCLK_HRC48 == HAL_RCC_HCPU_GetClockSrc(RCC_CLK_MOD_SYS))
    {
        HAL_HPAON_EnableXT48();
        HAL_RCC_HCPU_ClockSelect(RCC_CLK_MOD_SYS, RCC_SYSCLK_HXT48);
    }

    HAL_RCC_HCPU_ClockSelect(RCC_CLK_MOD_HP_PERI, RCC_CLK_PERI_HXT48);

    if (PM_STANDBY_BOOT != SystemPowerOnModeGet())
    {
        HAL_HPAON_WakeCore(CORE_ID_LCPU);
        HAL_RCC_Reset_and_Halt_LCPU(1);
#ifndef USE_ATE_MODE
        BSP_System_Config();
#endif
        HAL_HPAON_StartGTimer();
        HAL_PMU_EnableRC32K(1);
        HAL_PMU_LpCLockSelect(PMU_LPCLK_RC32);

        HAL_PMU_EnableDLL(1);

#ifndef LXT_DISABLE
        HAL_PMU_EnableXTAL32();
        if (HAL_PMU_LXTReady() != HAL_OK)
            HAL_ASSERT(0);
        HAL_RTC_ENABLE_LXT();
#endif

#ifndef CFG_BOOTLOADER
        HAL_PMU_SetWdt((uint32_t)hwp_wdt2);
#endif

        HAL_RCC_LCPU_ClockSelect(RCC_CLK_MOD_LP_PERI, RCC_CLK_PERI_HXT48);

        HAL_HPAON_CANCEL_LP_ACTIVE_REQUEST();
        if (HAL_LXT_DISABLED())
            LRC_init();
    }

#ifndef USE_ATE_MODE
    HAL_RCC_HCPU_ConfigHCLK(240);
#else
    HAL_RCC_HCPU_SetDiv(1, 1, 6);
    HAL_RCC_HCPU_EnableDLL1(240000000);
    HAL_RCC_HCPU_ClockSelect(RCC_CLK_MOD_SYS, RCC_SYSCLK_DLL1);
#endif

#if defined(BSP_USING_USBD) || defined(BSP_USING_USBH)
    HAL_RCC_HCPU_EnableDLL2(240000000);
    hwp_hpsys_rcc->USBCR = 4;
    hwp_hpsys_rcc->CSR |= HPSYS_RCC_CSR_SEL_USBC;
#else
    HAL_RCC_HCPU_EnableDLL2(288000000);
#endif

    HAL_Delay_us(0);

    mpi1_div = 2;
    mpi2_div = 4;

    HAL_MspInit();
    HAL_RCC_HCPU_ClockSelect(RCC_CLK_MOD_FLASH1, RCC_CLK_FLASH_DLL2);

#if defined (BSP_USING_PSRAM)
    /* Enable 1.8V LDO and initialize PSRAM controller.
     * Without this, bf0_psram_handle[].qspi_handle.Instance stays NULL.
     * When PM later calls rt_psram_wait_idle("psram1") during frequency
     * scaling / deep WFI (after pm_scenario_stop(PM_SCENARIO_AUDIO)),
     * HAL_FLASH_MANUAL_CMD dereferences the NULL Instance and faults
     * (DACCVIOL, MMAR=0x28). This matches the SDK sf32lb52-lcd_base
     * board init sequence. */
    HAL_PMU_ConfigPeriLdo(PMU_PERI_LDO_1V8, true, true);
#ifdef BSP_USING_RTTHREAD
    rt_psram_init();
#else
    board_init_psram();
#endif /* BSP_USING_RTTHREAD */
#endif

    HAL_RCC_HCPU_ClockSelect(RCC_CLK_MOD_FLASH2, RCC_CLK_FLASH_DLL2);
#if defined(BSP_USING_NOR_FLASH1) || defined(BSP_USING_NOR_FLASH2)
#ifdef BSP_USING_NOR_FLASH1
    mpi1_div = 3;
#endif
    if (PM_STANDBY_BOOT == SystemPowerOnModeGet())
    {
        HAL_HPAON_ENABLE_PAD();
#if defined(BSP_USING_NOR_FLASH1)
        BSP_Flash_hw1_init();
#endif
#if defined(BSP_USING_NOR_FLASH2)
        BSP_Flash_hw2_init();
#endif
    }
    else
    {
#ifdef BSP_USING_RTTHREAD
        rt_hw_flash_init();
#else
        BSP_Flash_Init();
#endif
    }
#endif

#elif defined(SOC_BF0_LCPU)
    HAL_LPAON_EnableXT48();
    HAL_RCC_LCPU_ClockSelect(RCC_CLK_MOD_SYS, RCC_SYSCLK_HXT48);
    HAL_RCC_LCPU_ClockSelect(RCC_CLK_MOD_LP_PERI, RCC_CLK_PERI_HXT48);
    HAL_RCC_LCPU_SetDiv(2, 1, 3);
    HAL_MspInit();
#endif
}

extern void BSP_PIN_Init(void);
void BSP_IO_Init(void)
{
    BSP_PIN_Init();
    BSP_Power_Up(true);

    /* Enable I2C1 clock gate (ECR1 bit 27) - not done by HAL_RCC_EnableModule */
    hwp_hpsys_rcc->ECR1 |= HPSYS_RCC_ECR1_I2C1;
}

__WEAK void SystemClock_Config(void)
{
}
