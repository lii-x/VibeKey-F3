#include <rtthread.h>
#include <string.h>
#include "bf0_hal.h"          /* HAL_StatusTypeDef / HAL_OK 等基础 */
#include "bf0_hal_rng.h"      /* RNG_HandleTypeDef / HAL_RNG_Init / HAL_RNG_Generate */
#include "bf0_ble_common.h"   /* ble_request_public_address / bd_addr_t / ble_common_update_type_t */
#include "bf0_ble_gap.h"      /* ble_request_local_irk / ble_gap_sec_key_t / GAP_KEY_LEN */
#include "register.h"         /* TRNG / hwp_trng */
#include "trng.h"             /* TRNG_CTRL_* / TRNG_STAT_* 寄存器位 */
#include "hpsys_rcc.h"        /* hwp_hpsys_rcc / HPSYS_RCC_ENR1_TRNG / HPSYS_RCC_RSTR1_TRNG */
#include "bt_slot.h"          /* bt_multi_get_slot / bt_slot_get_rand_base / BT_SLOT_NUM */

/* ============================================================================
 * 方案B(阶段1.6 随机地址改造): 每槽独立 BD_ADDR, 基址为每设备唯一随机静态地址
 *
 * 需求: 同一台电脑/手机可独立配对到槽1、槽2、槽3, 各槽互不影响。
 * 机制: 每个槽 = 一个不同的本机静态 BD_ADDR (BLE 与 BR/EDR 共用同一地址),
 *       主机眼里是不同的设备 → 可对每个槽独立配对、独立使用。
 *
 * 基址: 首次开机用芯片硬件 RNG(TRNG)生成 6 字节随机基址, 最高 2 位置 11
 *       (合法随机静态地址格式 0xC0 开头), 持久化到 bt_slot 的 flash 存储区;
 *       之后每次开机读回, 地址永不漂移。⚠️ 不用"当前 NVDS 地址"做基址
 *       (NVDS 每次开机被本函数覆写 → 反馈回路地址漂移); 也不依赖工厂 MAC
 *       (FACTORY_CFG_ID_MAC 缺失时仍每设备唯一, 解决多台设备地址冲突)。
 * 槽地址: 基址最低字节 + (槽位-1) → 3 槽地址互异(+0/+1/+2 模 256 必互异)。
 *
 * 接入点: 本函数是 SDK 的 __WEAK 弱符号(bf0_bt_common.c), 此处用强符号覆盖;
 *        BT 栈初始化时 ble_nvds_config_prepare() → ble_generate_public_address()
 *        回调本函数, 返回 BLE_UPDATE_ALWAYS 会把槽地址写入
 *        NVDS_STACK_TAG_BD_ADDRESS, BLE 与 BR/EDR 栈共用该地址。
 * 广播: 固件广播 own_addr_type = GAPM_STATIC_ADDR(ble_app.c), SDK 直接把 NVDS
 *       地址当本机静态地址用 → 随机静态地址格式(0xC0)主机兼容性最佳。
 * ========================================================================== */

/* TRNG 生成 6 字节真随机数(自实现轮询)。
 * ⚠️ 不用 HAL_RNG_Generate: 其 RNG_TIMEOUT_VALUE=2(仅 2ms), TRNG 出数来不及
 * 就超时返回(实测地址走兜底路径, 熵不足)。这里手动: 开时钟→复位→先 seed→
 * 两次 rand, 每步等 STAT 置位, 超时放宽到 50ms。成功返回 1。 */
static uint8_t btid_trng_rand6(uint8_t out[6])
{
    uint32_t r0 = 0, r1 = 0;
    /* 开 TRNG 时钟 + 复位(weak HAL_RNG_MspInit 为空实现, 必须手动) */
    hwp_hpsys_rcc->ENR1  |= HPSYS_RCC_ENR1_TRNG;
    hwp_hpsys_rcc->RSTR1 |= HPSYS_RCC_RSTR1_TRNG;
    hwp_hpsys_rcc->RSTR1 &= ~HPSYS_RCC_RSTR1_TRNG;
    rt_thread_mdelay(2);   /* 等时钟/复位稳定 */

    /* 使能(清 STOP) */
    TRNG->CTRL &= ~(TRNG_CTRL_GEN_SEED_STOP | TRNG_CTRL_GEN_RAND_NUM_STOP);

    /* 先 seed(随机数引擎需要种子) */
    TRNG->CTRL |= TRNG_CTRL_GEN_SEED_START;
    {
        uint32_t t0 = rt_tick_get();
        while (!(TRNG->STAT & TRNG_STAT_SEED_VALID)) {
            if (rt_tick_get() - t0 > rt_tick_from_millisecond(50)) return 0;
        }
    }
    /* 两次随机数取 4+2 字节 */
    for (int k = 0; k < 2; k++) {
        TRNG->CTRL |= TRNG_CTRL_GEN_RAND_NUM_START;
        uint32_t t0 = rt_tick_get();
        while (!(TRNG->STAT & TRNG_STAT_RAND_NUM_VALID)) {
            if (rt_tick_get() - t0 > rt_tick_from_millisecond(50)) return 0;
        }
        if (k == 0) r0 = TRNG->RAND_NUM0;
        else        r1 = TRNG->RAND_NUM0;
    }
    memcpy(out, &r0, 4);
    memcpy(out + 4, &r1, 2);
    return 1;
}

/* 确保每设备唯一随机基址已生成(首次用硬件 RNG, 之后读 flash 固定)。
 * 随机静态地址格式: 最高字节置 0xC0(bit47-46 = 11)。 */
static void btid_ensure_rand_base(uint8_t base[6])
{
    if (bt_slot_get_rand_base(base))
        return;   /* 已生成: 直接复用, 重启地址不变 */

    if (!btid_trng_rand6(base)) {
        /* 兜底: TRNG 异常时混入 tick, 保证每台设备地址不重复 */
        uint32_t r0 = (uint32_t)rt_tick_get() ^ 0xA5A55A5A;
        uint32_t r1 = (uint32_t)rt_tick_get() ^ 0x5A5AA5A5;
        memcpy(base, &r0, 4);
        memcpy(base + 4, &r1, 2);
        rt_kprintf("[BTID] TRNG failed, fallback tick-based base\n");
    }
    base[5] = (uint8_t)((base[5] & 0x3F) | 0xC0);   /* 随机静态地址格式 */
    if (base[0] == 0 && base[1] == 0 && base[2] == 0 &&
        base[3] == 0 && base[4] == 0)
        base[4] = 0x5A;                              /* 防全 0 */
    bt_slot_set_rand_base(base);                     /* 持久化: 此后重启固定 */
    rt_kprintf("[BTID] rand base generated %02x:%02x:%02x:%02x:%02x:%02x\n",
               base[5], base[4], base[3], base[2], base[1], base[0]);
}

ble_common_update_type_t ble_request_public_address(bd_addr_t *addr)
{
    uint8_t base[6] = {0};
    uint8_t slot = bt_multi_get_slot();   /* 1..3, boot 时已定 */

    if (slot < 1 || slot > BT_SLOT_NUM)
        slot = 1;

    btid_ensure_rand_base(base);
    /* 最低字节偏移: +0/+1/+2 模 256 必互异(即使进位回绕也互异) */
    base[0] = (uint8_t)(base[0] + (slot - 1));

    memcpy(addr->addr, base, 6);
    rt_kprintf("[BTID] slot%d public addr %02x:%02x:%02x:%02x:%02x:%02x\n",
               slot, base[5], base[4], base[3], base[2], base[1], base[0]);
    return BLE_UPDATE_ALWAYS;
}

/* ============================================================================
 * 每槽独立本地 IRK(方案B 阶段1.5)
 *
 * 背景: 同一台电脑先配槽1、再配槽2(同对端、不同本机地址)时, 实测 SMP 配对
 * 失败 GAPC_PAIRING_FAILED 120。SDK app 层 bond 表按对端地址匹配
 * (ble_connection_manager.c 只比 BD_ADDR_LEN), 若 SMP 底层按"本机身份+对端"
 * 区分 bond, 共享同一个 IRK 会让两个槽的 bond 互相冲突。给每槽派生独立 IRK,
 * 让 SMP 眼里每个槽是真正不同的本机身份, 避免 bond 冲突/覆盖。
 * 派生: 槽地址(与上面地址一致)+ 固定模式异或, 每槽不同且重启稳定。
 * ========================================================================== */
ble_common_update_type_t ble_request_local_irk(ble_gap_sec_key_t *local_irk)
{
    uint8_t base[6] = {0};
    uint8_t slot = bt_multi_get_slot();
    /* 与 ble_request_public_address 相同的槽地址派生 */
    static const uint8_t pat[16] = {
        0x56, 0x69, 0x62, 0x65, 0x4B, 0x65, 0x79, 0x46,   /* "VibeKeyF" */
        0x33, 0x2D, 0x49, 0x52, 0x4B, 0x2D, 0x53, 0x6C    /* "3-IRK-Sl" */
    };

    if (slot < 1 || slot > BT_SLOT_NUM)
        slot = 1;
    btid_ensure_rand_base(base);
    base[0] = (uint8_t)(base[0] + (slot - 1));

    for (int i = 0; i < GAP_KEY_LEN; i++)
        local_irk->key[i] = (uint8_t)(base[i % 6] ^ pat[i]);
    /* 诊断: 打印每槽 IRK 前 8 字节, 确认各槽 IRK 不同(方案B 同机多槽前提) */
    rt_kprintf("[BTID] slot%d local irk %02x%02x%02x%02x%02x%02x%02x%02x...\n",
               slot, local_irk->key[0], local_irk->key[1], local_irk->key[2],
               local_irk->key[3], local_irk->key[4], local_irk->key[5],
               local_irk->key[6], local_irk->key[7]);
    return BLE_UPDATE_ALWAYS;
}
