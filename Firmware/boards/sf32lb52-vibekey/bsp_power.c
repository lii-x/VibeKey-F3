#include "bsp_board.h"
#include <stdint.h>

#ifdef BSP_USING_RTTHREAD
extern void *rt_flash_get_handle_by_addr(uint32_t addr);
#endif

void BSP_GPIO_Set(int pin, int val, int is_porta)
{
    GPIO_TypeDef *gpio = (is_porta) ? hwp_gpio1 : hwp_gpio2;
    GPIO_InitTypeDef GPIO_InitStruct;

    GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT;
    GPIO_InitStruct.Pin = pin;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(gpio, &GPIO_InitStruct);

    /* 08-30: 同步设 PAD 下拉寄存器 —— SF32LB52 PA 脚上下拉由 PAD 寄存器管, HAL_GPIO_Init
     * 只清 GPIO PUPDR、不会动 PAD, SoC 复位默认上拉残留 → "输出低 + 上拉"漏电对。
     * (现调用方只有 PA00 RGB 数据脚与 PA40 预留脚, 都设下拉无副作用。) */
    HAL_PIN_Set(PAD_PA00 + pin, GPIO_A0 + pin, PIN_PULLDOWN, 1);

    HAL_GPIO_WritePin(gpio, pin, (GPIO_PinState)val);
}

/* 08-30: 预留/未用脚专用 —— 输入下拉, 比"输出低"更低功耗(输出缓冲器关闭,
 * 输入缓冲器空闲, 仅下拉电阻微安级电流, 且输入下拉无任何"上拉+输出低"漏电对风险)。
 * 同步清 PAD 下拉(同 BSP_GPIO_Set 的 PAD 上下拉修复)。 */
void BSP_GPIO_SetInputPD(int pin, int is_porta)
{
    GPIO_TypeDef *gpio = (is_porta) ? hwp_gpio1 : hwp_gpio2;
    GPIO_InitTypeDef GPIO_InitStruct;

    GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
    GPIO_InitStruct.Pin = pin;
    GPIO_InitStruct.Pull = GPIO_PULLDOWN;
    HAL_GPIO_Init(gpio, &GPIO_InitStruct);

    HAL_PIN_Set(PAD_PA00 + pin, GPIO_A0 + pin, PIN_PULLDOWN, 1);
}

/* 08-30: 未用(NC)脚统一设"输入下拉", 根治活跃态悬空脚漏电/振荡。
 * SF32LB52 GPIO 范围 PA00..PA44(45 脚)。used_pa[] 列出固件已配置/必须保留原功能的脚
 * (LED/RGB/马达/IMU-I2C/Flash-MPI/调试UART/按键/编码器/唤醒脚), 一律跳过。
 * ⚠️ 例外 PA40/PA44: 二者**没有功能**，但仍列在 used_pa[] 里 —— 因为它们由
 *    bsp_pinmux.c 开机 init 与 BSP_PowerUpCustom(DEEP 唤醒) **显式**调
 *    BSP_GPIO_SetInputPD() 处理(唤醒后重锁，见 BSP_PowerUpCustom 注释)，
 *    不走这里的批量循环。新增此类"预留脚"请照此办理，不要只删 used_pa[] 条目。
 * 其余 PA00..PA44 中未列出的脚为网表 NC 悬空输入脚:
 *   PA01,03,09,20,21,22,23,28,29,33,35,36,37,39,41,42,43 (共17个)
 * SoC 复位默认是上拉输入 → 活跃态就在漏电(CMOS 缓冲器+上拉), 此处开机 init 就拉成
 * 输入下拉(输入缓冲器电平钳 0V, 无穿透电流), 进 DEEP 前同样调用保持确定态。
 * 这些脚在 PCB 上无外接上/下拉电阻(无外部上拉可灌电流), 拉下拉安全。
 * USB D+/D- 与晶振 XI/XO 在专用非 PA 脚(PA00..PA44 功能表无 USB/OSC), 不在此范围。
 * 调用方: bsp_pinmux.c 开机 init + BSP_PowerDownCustom(进 DEEP 前), 幂等。 */
void BSP_GPIO_InitUnusedPD(void)
{
    static const uint8_t used_pa[] =
    {
        0, 2, 4, 5, 6, 7, 8, 10, 11, 12, 13, 14, 15, 16, 17, 18, 19,
        24, 25, 26, 27, 28, 30, 31, 32, 34, 38, 40, 44
    };
    int used_cnt = (int)(sizeof(used_pa) / sizeof(used_pa[0]));
    for (int i = 0; i <= 44; i++)
    {
        int used = 0;
        for (int k = 0; k < used_cnt; k++)
        {
            if (used_pa[k] == i) { used = 1; break; }
        }
        if (used) continue;
        BSP_GPIO_SetInputPD(i, 1);
    }
}

/* 进深睡前的板级 GPIO 收尾(省漏电), 与唤醒后恢复无直接关系(唤醒后固件重新初始化各脚)。
 * 此处只动 GPIO, 不碰 Flash —— Flash(MPI2/NOR_FLASH2) 的片内 DPD/RELEASE 放在
 * BSP_IO_Power_Down / BSP_Power_Up 里, 严格对齐 52x 官方 ULP 参考板
 * (sf32lb52-lchspi-ulp_base) 的 proven 实现: DPD 在 HCPU 掉电路径、RELEASE 仅在
 * !is_deep_sleep(LIGHT) 路径做, DEEP 唤醒的 Flash 退出 DPD 由 boot ROM 在 XIP 恢复前完成。
 * 这两个函数保持 __WEAK(普通 XIP), 不进保留 RAM —— 参考板已验证: DEEP 唤醒时 XIP 在
 * Flash 被 boot ROM 释放后才恢复, 无需把它们放 RAM, 放 RAM 反而有保留 RAM 段溢出风险。 */
__WEAK void BSP_PowerDownCustom(int coreid, bool is_deep_sleep)
{
    /* 进深睡前把 LED 引脚设为确定低电平输出, 避免悬空/上拉漏电(唤醒后由 led_status 重新控制) */
    HAL_GPIO_WritePin(hwp_gpio1, BSP_LED1_PIN, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(hwp_gpio1, BSP_LED2_PIN, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(hwp_gpio1, BSP_LED3_PIN, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(hwp_gpio1, BSP_LED4_PIN, GPIO_PIN_RESET);

    /* RGB SK6812 数据脚(PA00) 与预留脚 PA40 在深睡期若走 PWM 复用会浮空 →
     * 拉成确定低电平, 避免浮空脚漏电且确保外设深睡期确定关闭。
     * 注意: IMU 为 I2C(PA10/PA11) 带外部上拉, 若拉低会经上拉电阻持续灌电流, 故保持 I2C 功能不碰。
     * (PM_DEEP_ENABLE 策略表仅 {30, DEEP}、无 LIGHT 档, 此处只在 DEEP 入口被调用) */
    BSP_GPIO_Set(0, 0, 1);    /* PA00 = RGB data */
    BSP_GPIO_Set(40, 0, 1);   /* PA40 = reserved, 确定低 */

    /* 未用 GPIO 统一设"输入下拉"(BSP_GPIO_InitUnusedPD, 08-30 抽出复用),
     * 消除悬空输入在深睡期漏电/振荡 —— 与开机 init(bsp_pinmux.c)同一实现。 */
    BSP_GPIO_InitUnusedPD();
}

__WEAK void BSP_PowerUpCustom(bool is_deep_sleep)
{
    /* 08-30: PA40 DEEP 唤醒后 HPSYS 域寄存器复位 → PA40 回到 SoC 默认(输入上拉),
     * 与"输出低"组合成漏电对。BSP_PowerDownCustom 那次配置是进深睡前的,
     * 唤醒后必须在此重新设一次(对称), 锁住"下拉 + 输出低"确定关断态。
     * BSP_GPIO_Set 内部已同时清 PAD 上拉 + GPIO PUPDR, 幂等, 开机时(BSP_Power_Up
     * 启动初始化路径也会调本函数)也安全。
     * 此前注释误判"PA40 唤醒后保持该确定关断态即可" —— 实测 HPSYS 复位后 PA 上下拉
     * 全部回到默认上拉, 不重设就漏电。RGB(PA00) 由 power.c resume 6.5 步恢复
     * GPTIM2 复用与时钟(不再依赖驱动自重配); 17 个 NC 脚保持下拉无副作用。 */
    BSP_GPIO_SetInputPD(40, 1);  /* PA40 = reserved, 唤醒后锁"输入下拉"确定态 */

    /* 10-05: PA44 同上处理。原 Type-C 检测脚，方案B(USB 检测并入充电检测)后软件不再读它，
     * 与 PA40 同为"预留悬空脚" ⇒ DEEP 唤醒后 HPSYS 复位会把 PAD 上下拉打回默认上拉，
     * 必须在唤醒路径重设一次，否则活跃态持续漏电。 */
    BSP_GPIO_SetInputPD(44, 1);  /* PA44 = reserved(原 Type-C 检测), 唤醒后锁"输入下拉" */
}

/* app 层可重写(main.c): 返回当前是否需要给 IMU 供电。
 * 默认(bootloader/lcpu 或未重写)恒返回 1, 保持“唤醒即给 IMU 上电”的原行为;
 * HCPU 主程序重写为随 BLE 连接状态开关, 断连时唤醒不再重开 LDO3 省电。 */
__WEAK int BSP_ImuPowerWanted(void)
{
    return 1;
}

void BSP_Power_Up(bool is_deep_sleep)
{
    /* IMU 3.3V LDO(VOUT2 / LDO3): 仅在需要时唤醒即上电。蓝牙断连(air_mouse_stop 后)
     * BSP_ImuPowerWanted()==0, 此时唤醒不重开 LDO3, 深睡期 IMU 电源保持关断以省静态电流。 */
    if (BSP_ImuPowerWanted())
        HAL_PMU_ConfigPeriLdo(PMU_PERI_LDO3_3V3, true, true);
    BSP_PowerUpCustom(is_deep_sleep);

    /* Flash(MPI2 / NOR_FLASH2, 主固件 XIP 于此) 退出片内 Deep Power Down：
     * ⚠️ 已禁用（2026-08-05 真机确认）：52x DEEP 唤醒 = WFI 返回继续执行（非 boot ROM
     * 冷启动），唤醒后恢复段第一条指令就在 Flash(XIP) 里；若进 DEEP 前把 Flash 置 DPD，
     * 唤醒瞬间取指即卡死（实测：电流≈运行态 1.5mA、无 BSP_PowerUpCustom wake! 日志）。
     * classical 例程 board 无 bsp_power.c（SDK __WEAK 空实现）从不 DPD 故能正常唤醒；
     * ULP 参考板"DEEP 唤醒由 boot ROM 释放 Flash"的假设在本板不成立。为唤醒可靠性，
     * DPD/RELEASE 全部去掉，代价仅是 Flash 静态电流多几 µA（相对 0.2mA 目标可忽略）。 */
#ifdef SOC_BF0_HCPU
    if (!is_deep_sleep)
    {
#ifdef BSP_USING_NOR_FLASH2
        FLASH_HandleTypeDef *flash_handle =
            (FLASH_HandleTypeDef *)rt_flash_get_handle_by_addr(MPI2_MEM_BASE);
        (void)flash_handle;
        /* HAL_FLASH_RELEASE_DPD(flash_handle); */
        /* HAL_Delay_us(80); */   /* tRES1: Flash 退出 DPD 稳定时间 */
#endif /* BSP_USING_NOR_FLASH2 */
    }
#endif /* SOC_BF0_HCPU */
}

void BSP_IO_Power_Down(int coreid, bool is_deep_sleep)
{
    BSP_PowerDownCustom(coreid, is_deep_sleep);

    /* Flash(MPI2 / NOR_FLASH2, 主固件 XIP 于此) 片内 Deep Power Down：
     * ⚠️ 已禁用（2026-08-05 真机确认）——进 DEEP 让 Flash 进 DPD 会导致唤醒即死
     * （WFI 返回取指 Flash 在 DPD 卡死，见上方 BSP_Power_Up 注释）。保留此段仅作
     * 文档留痕，若未来换用"恢复段放 RAM"方案可重新启用。 */
#ifdef SOC_BF0_HCPU
    if (coreid == CORE_ID_HCPU)
    {
#ifdef BSP_USING_NOR_FLASH2
        FLASH_HandleTypeDef *flash_handle =
            (FLASH_HandleTypeDef *)rt_flash_get_handle_by_addr(MPI2_MEM_BASE);
        (void)flash_handle;
        /* HAL_FLASH_DEEP_PWRDOWN(flash_handle); */
        /* HAL_Delay_us(3); */    /* tDP: Flash 进入 DPD 稳定时间 */
#endif /* BSP_USING_NOR_FLASH2 */
    }
#endif /* SOC_BF0_HCPU */
}

void BSP_SDIO_Power_Up(void)
{
}

void BSP_SDIO_Power_Down(void)
{
}
