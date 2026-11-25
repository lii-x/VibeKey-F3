/* ============================================================================
 * audio_pw.c —— 语音(SCO)链路功耗调试命令（09-22 新增）
 *
 * === 背景（用户实测：单向语音输入时整机 18.x mA）===
 * 读 SDK 源码确认了三处"在本机场景下白耗电"的地方：
 *
 *   ① HCPU 被拉到 240MHz 且 WFI 期间也不降频
 *      `pm_scenario_start(PM_SCENARIO_AUDIO)`（`bf0_pm.c:3247`）做三件事：
 *        · `rt_pm_run_enter(PM_RUN_MODE_HIGH_SPEED)`  → HCPU **240MHz**（:3274）
 *        · `HAL_RCC_HCPU_SetDeepWFIDiv(1,0,1)`        → **WFI 期间 HDIV=1，几乎不降频**（:3293）
 *        · `hwp_hpsys_rcc->DBGR |= ..._FORCE_HP`      → 强制开 HP 时钟（:3296）
 *      而 3A + 编码每 7.5ms 才处理一帧，**其余时间都在满速空转**。
 *
 *   ② AEC 在"没有扬声器、没有下行音频"时依然空转
 *      `audio_server.c:474-486`：`tx_enable == 0`（无真实下行数据）时，SDK 仍然
 *        `memset(tx_data_tmp, 0, ...)` 之后主动调 `audio_3a_far_put(静音)`（:479）
 *      并用 `bf0_audprc_device_write()` 继续给 DAC 灌数据（:484）。
 *      ⇒ `is_far_putted` 被置 1 ⇒ **AECM 照跑**（`audio_3a.c:716` 的条件是
 *        `is_far_putted && g_u16_test_aec`）。
 *      本机**没有扬声器**、用户只是单向说话 ⇒ **根本不存在回声路径**，
 *      AEC 的 FFT / 延迟估计 / BufferFarend / Process 全部是纯浪费。
 *
 *   ③ 下行 DAC 通道被一并打开
 *      `audio_open(AUDIO_TYPE_BT_VOICE, AUDIO_TXRX, ...)` 硬编码在
 *      `hfp_audio_api.c:175`，默认设备 `AUDIO_DEVICE_SPEAKER`(=0) ⇒ 无喇叭也占电。
 *      改它要动 SDK，**本命令不涉及**（留待单独授权）。
 *
 * === 用法（按住 C1 说话时逐条敲，每条之间读一次电流表）===
 *   apw / apw dump        只打印现场（不改任何东西）
 *   apw aec  <0|1>        AEC 开关          0 = 关（**本场景推荐，无音质代价**）
 *   apw ns   <0|1>        NS 降噪开关       0 = 关（可能降低语音识别率，谨慎）
 *   apw agc  <0|1>        上行 AGC 开关     默认 1，建议保留
 *   apw 3a   <0|1>        整个 3A bypass    1 = 全关（用于量化 3A 的总开销）
 *   apw freq <0..3>       运行频率 0=240 / 1=144 / 2=48 / 3=24 MHz
 *   apw wfi  <div>        WFI 分频档（与 power.c 的 60 同语义），0 = 不改
 *   apw hp   <0|1>        DBGR.FORCE_HP（**音频外设工作时应保持 1**，见下方警告）
 *
 * === 注意事项 ===
 * · 只改**运行时**开关，不动任何编译默认值；重新上电即全部恢复。
 * · `apw freq 2`（48MHz）是官方低功耗文档给出的"有音频外设工作时的最低档"
 *   （`docs/.../low_power.md`：音频仅可降至 48MHz，其它可至 4MHz）。但 mSBC 帧周期
 *   7.5ms 是**硬实时窗口** ⇒ 降频后**务必听有无卡顿/丢帧**，不行就退回 144MHz。
 * · ⚠️ `apw hp 0` 会清掉 FORCE_HP，而该位的用途正是"避免 AUDPRC 时钟在 DeepWFI
 *   期间被关"（`bf0_pm.c:3295` 原注释）。**降频时请保持 hp=1**，只有在音频已停止
 *   的状态下才适合清它。
 * · `apw freq` / `apw hp` 会被下一次 `pm_scenario_start(AUDIO)` 覆盖
 *   （例如 SCO 链路重建时）⇒ 改完请用 `apw dump` 复看当前值。
 * · 本文件可**整个删除**：`src/SConscript` 用 `Glob('*.c')` 自动收编，
 *   增删文件都不需要改构建脚本。
 * ============================================================================ */

#include "rtthread.h"
#include "rtdevice.h"
#include "bf0_hal.h"
#include "bf0_hal_aon.h"         /* HPSYS_AON_ISSR_* / hwp_hpsys_aon */
#include "bf0_pm.h"              /* pm_scenario_start/stop, PM_SCENARIO_AUDIO */
#include "drivers/pm.h"          /* rt_pm_run_enter / rt_pm_run_mode_get / PM_RUN_MODE_* */
#include "hpsys_rcc.h"           /* HPSYS_RCC_DBGR_FORCE_HP / DWCFGR HDIV 位域 */
#include "audcodec.h"            /* AUDCODEC_CFG_ADC/DAC_ENABLE, AUDCODEC_REFGEN_CFG_EN */
#include "audprc.h"              /* AUDPRC_TypeDef / hwp_audprc / AUDPRC_CFG_ENABLE */
#include "bf0_hal_pmu.h"         /* PMUC_LPSYS_LDO_EN / PMUC_LPSYS_SWR_RDY (hwp_pmuc 来自 register.h) */
#include <stdlib.h>              /* atoi */
#include <math.h>                /* sqrtf: 静音门计算 RMS */
#include <stdbool.h>             /* bool —— 必须与 audio_3a.c 的 g_ans1_disabled 同型 */

/* ---------------------------------------------------------------------------
 * 3A 内部开关 —— 取自 `audio_3a.c` 的**非 static 全局变量**。
 *
 * ⚠️⚠️ 类型必须与 `audio_3a.c:107-110` 的**定义完全逐字一致**，尤其是
 *   `bool`（C99 `_Bool` = **1 字节**）。若这里写错宽度（例如写成 uint8_t 而对面
 *   是 uint16_t），读写会跨到相邻变量上，可能破坏 3A 内部状态。
 *   改动本段前请先回看 `audio_3a.c` 的定义。
 *
 * 这些变量 `audioproc.h` **没有声明**，所以只能自行 extern。
 * 它们在 `audio_3a_open()` 中**不会被重置** ⇒ 运行时设定可持久生效，
 * 唯一的例外是有人调 `audio_3a_set_bypass(0, ...)` 会把它们恢复成全开。
 * --------------------------------------------------------------------------- */
extern volatile uint8_t  g_uplink_agc;      /* audio_3a.c:107  1 = 开上行 AGC */
extern volatile uint16_t g_u16_test_aec;    /* audio_3a.c:108  1 = 开 AEC */
extern volatile uint16_t g_u16_test_agc;    /* audio_3a.c:109  1 = 开另一处 AGC */
extern volatile bool     g_ans1_disabled;   /* audio_3a.c:110  true = 关 NS 降噪 */

/* `audio_3a_set_bypass` 声明在 `audioproc.h:82`。这里不直接 include 那个头，
 * 因为它会拉入 ipc/ringbuffer.h 等一串依赖；自行声明更轻。 */
extern void audio_3a_set_bypass(uint8_t is_bypass, uint8_t mic, uint8_t down);

/* ============================================================================
 * ★ 2026-10-12 追加：语音后 +0.3mA 专项取证（旧 dump 视野之外）
 *
 * === 为什么需要它 ===
 * 实测：修好 DWCFGR.HDIV(语音后残留 12 → 恢复 60) 后电流**毫无变化**
 * ⇒ HDIV 不是根因(已证伪)。两份 dump 逐项对比后，唯一差异是
 * `AUDCODEC_CFG: 0x00000000 → 0x00000018`，即 ADC_EN_DLY_SEL(=3) ——
 * 该位由 `HAL_AUDCODEC_Config_TChanel()` 置上，是**纯配置位、本身不耗电**
 * ⇒ 它只是"语音动过 codec"的标记，真凶在旧 dump 覆盖不到的模拟块里。
 *
 * === 首要怀疑：bandgap ===
 * `HAL_TURN_OFF_PLL()`(bf0_hal_audcodec_m.c:396，audio close 时经
 * drv_audcodec_m.c 调用) 关断时**只清两个位**：
 *     BG_CFG0 &= ~EN;   BG_CFG0 &= ~EN_SMPL;
 * 而 `HAL_TURN_ON_PLL()` 曾把 BG_CFG0 置成
 *     EN | VREF_SEL=0xc | EN_RCFLT | MIC_VREF_SEL=4 | EN_AMP | ...
 * ⇒ **EN_AMP / EN_RCFLT / MIC_VREF_SEL 全会留下**。bandgap 放大器是真模拟块，
 *   若其使能不随主 EN 门控，就是持续的 0.1~0.3mA 泄漏。
 *
 * === 用法（同一 boot 内，voice 前后各一次）===
 *   1) 语音前(电流 1.4-1.5mA): `apw snap`   ← 取基准
 *   2) 说一次语音、回到空闲(电流 1.7-1.8mA)
 *   3) `apw diff`                            ← 只打印发生变化的寄存器
 *   4) `apw bg 0` / `apw bg 1`               ← 直接 A/B 切 bandgap，听电流变化
 * ============================================================================ */
typedef struct { const char *name; volatile uint32_t *addr; } apw_reg_t;
static const apw_reg_t apw_regs[] =
{
    {"CODEC.CFG",      &hwp_audcodec->CFG},
    {"CODEC.DAC_CFG",  &hwp_audcodec->DAC_CFG},
    {"CODEC.ADC_CFG",  &hwp_audcodec->ADC_CFG},
    {"CODEC.COMMON",   &hwp_audcodec->COMMON_CFG},
    {"CODEC.BG0",      &hwp_audcodec->BG_CFG0},
    {"CODEC.BG1",      &hwp_audcodec->BG_CFG1},
    {"CODEC.BG2",      &hwp_audcodec->BG_CFG2},
    {"CODEC.REFGEN",   &hwp_audcodec->REFGEN_CFG},
    {"CODEC.PLL0",     &hwp_audcodec->PLL_CFG0},
    {"CODEC.PLL2",     &hwp_audcodec->PLL_CFG2},
    {"CODEC.PLL3",     &hwp_audcodec->PLL_CFG3},
    {"CODEC.PLL4",     &hwp_audcodec->PLL_CFG4},
    {"CODEC.PLLSTAT",  &hwp_audcodec->PLL_STAT},
    {"CODEC.ADCANA",   &hwp_audcodec->ADC_ANA_CFG},
    {"CODEC.DAC1",     &hwp_audcodec->DAC1_CFG},
    {"CODEC.DAC2",     &hwp_audcodec->DAC2_CFG},
    {"CODEC.ADCH0",    &hwp_audcodec->ADC_CH0_CFG},
    {"CODEC.DACH0",    &hwp_audcodec->DAC_CH0_CFG},
    {"AUDPRC.CFG",     &hwp_audprc->CFG},
    {"AUDPRC.STB",     &hwp_audprc->STB},
    {"AUDPRC.ADCPATH", &hwp_audprc->ADC_PATH_CFG0},
    {"AUDPRC.DACPATH", &hwp_audprc->DAC_PATH_CFG0},
    {"RCC.DWCFGR",     &hwp_hpsys_rcc->DWCFGR},
    {"RCC.DBGR",       &hwp_hpsys_rcc->DBGR},
    {"AON.ISSR",       &hwp_hpsys_aon->ISSR},
    {"PMUC.LPSYS_LDO", &hwp_pmuc->LPSYS_LDO},
    {"PMUC.LPSYS_SWR", &hwp_pmuc->LPSYS_SWR},
};
#define APW_REG_N ((int)(sizeof(apw_regs) / sizeof(apw_regs[0])))
static uint32_t g_apw_snap[APW_REG_N];
static uint8_t  g_apw_snap_valid = 0;

static const char *apw_rm_name(rt_uint8_t m)
{
    switch (m)
    {
    case PM_RUN_MODE_HIGH_SPEED:   return "HIGH 240MHz";
    case PM_RUN_MODE_NORMAL_SPEED: return "NORMAL 144MHz";
    case PM_RUN_MODE_MEDIUM_SPEED: return "MEDIUM 48MHz";
    case PM_RUN_MODE_LOW_SPEED:    return "LOW 24MHz";
    default:                       return "?";
    }
}

/* 一次性打印判读所需全部现场（只读，无副作用） */
static void apw_dump(void)
{
    rt_uint8_t rm   = rt_pm_run_mode_get();
    uint32_t   dw   = hwp_hpsys_rcc->DWCFGR;
    uint32_t   hdiv = (dw & HPSYS_RCC_DWCFGR_HDIV_Msk) >> HPSYS_RCC_DWCFGR_HDIV_Pos;
    uint32_t   ac   = hwp_audcodec->CFG;
    uint32_t   rg   = hwp_audcodec->REFGEN_CFG;
    uint32_t   issr = hwp_hpsys_aon->ISSR;

    rt_kprintf("[APW] 3A : aec=%u ns=%s uplink_agc=%u test_agc=%u\n",
               (unsigned)g_u16_test_aec, g_ans1_disabled ? "OFF" : "ON",
               (unsigned)g_uplink_agc, (unsigned)g_u16_test_agc);

    /* run_mode：SDK 在语音期设 HIGH_SPEED(240MHz)；这里能看到我们的降频是否还在 */
    rt_kprintf("[APW] PM : run_mode=%u(%s)\n", (unsigned)rm, apw_rm_name(rm));

    /* DWCFGR.HDIV = WFI 期间的分频；语音场景 SDK 设成 1（几乎不降频）
     * DBGR.FORCE_HP = 强制开 HP 时钟（保 AUDPRC） */
    rt_kprintf("[APW] RCC: DWCFGR=0x%08x(HDIV=%u) CFGR=0x%08x\n",
               (unsigned)dw, (unsigned)hdiv, (unsigned)hwp_hpsys_rcc->CFGR);
    rt_kprintf("[APW] RCC: DBGR=0x%08x FORCE_HP=%u\n",
               (unsigned)hwp_hpsys_rcc->DBGR,
               !!(hwp_hpsys_rcc->DBGR & HPSYS_RCC_DBGR_FORCE_HP));

    rt_kprintf("[APW] AON: PMR=0x%08x(mode=%u) ISSR=0x%08x LP_ACTIVE=%u HP2LP_REQ=%u\n",
               (unsigned)hwp_hpsys_aon->PMR, (unsigned)(hwp_hpsys_aon->PMR & 0x3u),
               (unsigned)issr,
               !!(issr & HPSYS_AON_ISSR_LP_ACTIVE),
               !!(issr & HPSYS_AON_ISSR_HP2LP_REQ));

    /* REFGEN.EN 是 codec 模拟总闸；ADC/DAC 是收发通道。无喇叭时 DAC_EN 是纯浪费。 */
    rt_kprintf("[APW] CDEC: CFG=0x%08x(ADC_EN=%u DAC_EN=%u) REFGEN=0x%08x(EN=%u)\n",
               (unsigned)ac,
               !!(ac & AUDCODEC_CFG_ADC_ENABLE), !!(ac & AUDCODEC_CFG_DAC_ENABLE),
               (unsigned)rg, !!(rg & AUDCODEC_REFGEN_CFG_EN));

    /* ★ 语音后 +0.3mA 专项(详见文件上方 regs 表注释)。这些块旧 dump 看不到。 */
    {
        uint32_t bg0 = hwp_audcodec->BG_CFG0;
        rt_kprintf("[APW] BG0 : 0x%08x EN=%u EN_AMP=%u EN_RCFLT=%u EN_SMPL=%u MIC_VREF_SEL=%u\n",
                   (unsigned)bg0,
                   !!(bg0 & AUDCODEC_BG_CFG0_EN), !!(bg0 & AUDCODEC_BG_CFG0_EN_AMP),
                   !!(bg0 & AUDCODEC_BG_CFG0_EN_RCFLT), !!(bg0 & AUDCODEC_BG_CFG0_EN_SMPL),
                   (unsigned)((bg0 & AUDCODEC_BG_CFG0_MIC_VREF_SEL_Msk) >> AUDCODEC_BG_CFG0_MIC_VREF_SEL_Pos));
        rt_kprintf("[APW] BG1 : 0x%08x BG2 : 0x%08x COMMON: 0x%08x\n",
                   (unsigned)hwp_audcodec->BG_CFG1, (unsigned)hwp_audcodec->BG_CFG2,
                   (unsigned)hwp_audcodec->COMMON_CFG);
        rt_kprintf("[APW] DACF: 0x%08x ADCF: 0x%08x DAC1: 0x%08x DAC2: 0x%08x\n",
                   (unsigned)hwp_audcodec->DAC_CFG, (unsigned)hwp_audcodec->ADC_CFG,
                   (unsigned)hwp_audcodec->DAC1_CFG, (unsigned)hwp_audcodec->DAC2_CFG);
        rt_kprintf("[APW] PLL0: 0x%08x PLL2: 0x%08x PLLST: 0x%08x ADCANA: 0x%08x\n",
                   (unsigned)hwp_audcodec->PLL_CFG0, (unsigned)hwp_audcodec->PLL_CFG2,
                   (unsigned)hwp_audcodec->PLL_STAT, (unsigned)hwp_audcodec->ADC_ANA_CFG);
        rt_kprintf("[APW] AUDPRC CFG=0x%08x(EN=%u) STB=0x%08x\n",
                   (unsigned)hwp_audprc->CFG, !!(hwp_audprc->CFG & AUDPRC_CFG_ENABLE),
                   (unsigned)hwp_audprc->STB);
        rt_kprintf("[APW] PMUC LPSYS_LDO=0x%08x(EN=%u) LPSYS_SWR=0x%08x(RDY=%u) snap=%d\n",
                   (unsigned)hwp_pmuc->LPSYS_LDO, !!(hwp_pmuc->LPSYS_LDO & PMUC_LPSYS_LDO_EN),
                   (unsigned)hwp_pmuc->LPSYS_SWR, !!(hwp_pmuc->LPSYS_SWR & PMUC_LPSYS_SWR_RDY),
                   (int)g_apw_snap_valid);

        /* ★ 2026-10-12: BLE 连接间隔(×1.25ms) —— 此前完全未观测的变量。
         * 语音中按了按钮 ⇒ air_mouse_is_active() 为真 ⇒ SCO 断开后
         * ble_app_conn_param_by_activity() 把 BLE 收紧到 7.5~15ms(interval 6~12);
         * 而长时间空闲的基线是 relax(20~40ms, interval 16~32)。
         * 射频唤醒频率差 2 倍以上 ⇒ 是 +0.3mA 的候选之一。语音前后各敲一次一比即知。 */
        {
            extern volatile uint16_t g_ble_cur_interval;   /* ble_app.c:39 */
            extern volatile uint16_t g_ble_cur_supvto;      /* 监督超时 ×10ms */
            rt_kprintf("[APW] LINK: ble_interval=%u (=%u.%02ums) sup_to=%u (=%u.%02us)\n",
                       (unsigned)g_ble_cur_interval,
                       (unsigned)(g_ble_cur_interval * 5 / 4),
                       (unsigned)(g_ble_cur_interval * 5 % 4 * 25),
                       (unsigned)g_ble_cur_supvto,
                       (unsigned)(g_ble_cur_supvto / 100),
                       (unsigned)(g_ble_cur_supvto % 100));
        }
    }
}

static void apw_usage(void)
{
    rt_kprintf("apw — audio(SCO) power tuning\n");
    rt_kprintf("  apw                   dump only\n");
    rt_kprintf("  apw aec  <0|1>        AEC on/off       (0 = off, no echo path here)\n");
    rt_kprintf("  apw ns   <0|1>        NS  on/off       (0 = off, may hurt ASR)\n");
    rt_kprintf("  apw agc  <0|1>        uplink AGC       (default 1, keep on)\n");
    rt_kprintf("  apw 3a   <0|1>        whole 3A bypass  (1 = all off, for measuring)\n");
    rt_kprintf("  apw freq <0..3>       run mode 240/144/48/24 MHz\n");
    rt_kprintf("  apw wfi  <div>        SetDeepWFIDiv    (0 = keep unchanged)\n");
    rt_kprintf("  apw hp   <0|1>        DBGR.FORCE_HP   (keep 1 while audio runs)\n");
    rt_kprintf("  apw snap              snapshot regs (take BEFORE voice)\n");
    rt_kprintf("  apw diff              print only regs changed since snapshot\n");
    rt_kprintf("  apw bg   <0|1>        BG_CFG0 EN/EN_AMP/EN_RCFLT (A/B test bandgap)\n");
}

/* MSH 命令实现。每次执行后自动 dump，方便对照读数。 */
static int apw(int argc, char **argv)
{
    if (argc >= 2)
    {
        const char *k = argv[1];
        int v = (argc >= 3) ? atoi(argv[2]) : 0;

        if (rt_strcmp(k, "help") == 0)
        {
            apw_usage();
            return 0;
        }
        else if (rt_strcmp(k, "dump") == 0)
        {
            /* 什么都不做，走下面的 dump */
        }
        else if (rt_strcmp(k, "aec") == 0)
        {
            g_u16_test_aec = (uint16_t)(v ? 1u : 0u);
            rt_kprintf("[APW] AEC -> %s\n", v ? "ON" : "OFF (memcpy bypass)");
        }
        else if (rt_strcmp(k, "ns") == 0)
        {
            g_ans1_disabled = (v ? true : false);
            rt_kprintf("[APW] NS  -> %s\n", v ? "OFF (disabled)" : "ON");
        }
        else if (rt_strcmp(k, "agc") == 0)
        {
            g_uplink_agc = (uint8_t)(v ? 1u : 0u);
            rt_kprintf("[APW] uplink AGC -> %s\n", v ? "ON" : "OFF");
        }
        else if (rt_strcmp(k, "3a") == 0)
        {
            audio_3a_set_bypass((uint8_t)(v ? 1u : 0u), 0, 0);
            rt_kprintf("[APW] 3A bypass -> %s\n", v ? "ON (DC/NS/AEC/AGC all off)" : "OFF (all restored)");
        }
        else if (rt_strcmp(k, "freq") == 0)
        {
            if (v < 0 || v >= (int)PM_RUN_MODE_MAX)
            {
                rt_kprintf("[APW] freq out of range: expect 0..3\n");
            }
            else
            {
                /* 注意：降速请求由 IDLE 线程落地，本行打印后可能还要一会儿才生效；
                 * 下面的 dump 若仍显示旧值，稍等再看一次。 */
                int r = rt_pm_run_enter((uint8_t)v);
                rt_kprintf("[APW] rt_pm_run_enter(%d) -> %d (%s)\n",
                           v, r, apw_rm_name((rt_uint8_t)v));
            }
        }
        else if (rt_strcmp(k, "wfi") == 0)
        {
            if (v <= 0)
                rt_kprintf("[APW] wfi: unchanged\n");
            else
            {
                HAL_RCC_HCPU_SetDeepWFIDiv((int8_t)v, 0, 1);
                rt_kprintf("[APW] WFI HDIV -> %d\n", v);
            }
        }
        else if (rt_strcmp(k, "hp") == 0)
        {
            if (v)
                hwp_hpsys_rcc->DBGR |= HPSYS_RCC_DBGR_FORCE_HP;
            else
                hwp_hpsys_rcc->DBGR &= ~HPSYS_RCC_DBGR_FORCE_HP;
            rt_kprintf("[APW] FORCE_HP -> %d\n", v ? 1 : 0);
        }
        else if (rt_strcmp(k, "snap") == 0)
        {
            int i;
            for (i = 0; i < APW_REG_N; i++)
                g_apw_snap[i] = *apw_regs[i].addr;
            g_apw_snap_valid = 1;
            rt_kprintf("[APW] snapshot taken: %d regs (语音前取基准, 语音后 `apw diff`)\n", APW_REG_N);
        }
        else if (rt_strcmp(k, "diff") == 0)
        {
            if (!g_apw_snap_valid)
            {
                rt_kprintf("[APW] no snapshot yet -> run 'apw snap' first (同一 boot 内)\n");
            }
            else
            {
                int i, n = 0;
                for (i = 0; i < APW_REG_N; i++)
                {
                    uint32_t cur = *apw_regs[i].addr;
                    if (cur != g_apw_snap[i])
                    {
                        rt_kprintf("[APW] DIFF %-14s 0x%08x -> 0x%08x\n",
                                   apw_regs[i].name, (unsigned)g_apw_snap[i], (unsigned)cur);
                        n++;
                    }
                }
                if (!n)
                    rt_kprintf("[APW] diff: no change across %d regs\n", APW_REG_N);
            }
        }
        else if (rt_strcmp(k, "bg") == 0)
        {
            /* A/B 直测: 切 bandgap 的三个使能位(EN/EN_AMP/EN_RCFLT)，配万用表看电流跟随情况。
             * ⚠️ 仅在音频已停(空闲)时用; 通话中勿动。 */
            uint32_t bits = (uint32_t)(AUDCODEC_BG_CFG0_EN_Msk | AUDCODEC_BG_CFG0_EN_AMP_Msk
                                     | AUDCODEC_BG_CFG0_EN_RCFLT_Msk);
            if (v) hwp_audcodec->BG_CFG0 |=  bits;
            else   hwp_audcodec->BG_CFG0 &= ~bits;
            rt_kprintf("[APW] BG_CFG0 EN/EN_AMP/EN_RCFLT -> %d (BG0=0x%08x)\n",
                       v ? 1 : 0, (unsigned)hwp_audcodec->BG_CFG0);
        }
        else
        {
            rt_kprintf("[APW] unknown sub-command: %s\n", k);
            apw_usage();
        }
    }
    else
    {
        apw_usage();
    }

    apw_dump();
    return 0;
}
MSH_CMD_EXPORT(apw, audio(SCO) power tuning: dump|aec|ns|agc|3a|freq|wfi|hp|snap|diff|bg);

/* ============================================================================
 * 麦克风 ADC 增益可调（11-13，排查"滋滋"底噪用）
 * ============================================================================
 * SDK 里 `get_mic_volume()` 是 __weak 默认返回 12dB，本工程此前没有覆盖。
 * 这里提供强符号覆盖 + 运行时命令，用来判断底噪到底来自模拟前端还是后级：
 *   micvol 0   → 重拨一次 → 底噪明显变小 ⇒ 模拟侧(麦克风自噪/前端增益过高)
 *   micvol 0   → 底噪几乎不变 ⇒ ADC 之后(数字域/供电串扰/蓝牙链路)
 * 范围: SF32LB52x 的 ADC 路径音量 -60 ~ +30 dB。
 * ⚠️ 音量在 ADC 打开时配置，改完要**重新建立通话**才生效。
 * ========================================================================== */
/* ⚠️ 不能自己定义 get_mic_volume() —— SDK 的 drv_audprc.c 里它是**强符号**，
 * 再定义一个会 multiple definition 链接失败（已踩过）。
 * 它的真实取值来自该文件里的全局 `g_adc_volume`（默认 12），直接改这个变量即可。 */
extern int8_t g_adc_volume;

int cmd_micvol(int argc, char **argv)
{
    if (argc > 1)
    {
        int v = atoi(argv[1]);
        if (v < -60) v = -60;
        if (v > 30)  v = 30;
        g_adc_volume = (int8_t)v;
    }
    rt_kprintf("[MIC] ADC gain = %d dB (range -60~30), apply on next call\n", g_adc_volume);
    return 0;
}
MSH_CMD_EXPORT_ALIAS(cmd_micvol, micvol, set mic ADC gain dB range -60~30);

/* ============================================================================
 * 麦克风链路默认值（11-13）
 * ============================================================================
 * 器件：WMM7037AT2-4/TR（韦尔 WILLSEMI）
 *   灵敏度 -42 dBV/Pa（偏低）、**SNR 仅 59 dB(A)**、DC 输出 0.76V、200Ω
 * ⇒ 等效输入噪声 = 94 - 59 = **35 dB(A) SPL**，与安静房间背景（30~35 dB SPL）相当，
 *   所以只要通话开着就能听到"沙沙"。这是**器件的物理下限**，软件消不掉。
 *
 * 能做的只有把增益降下来、让底噪的绝对电平变小；响度损失由 AGC 补：
 *   ADC 12dB → **3dB**（底噪降 9dB）
 *   AGC 18dB → **22dB**（补回语音响度）
 * 唯一根治办法是换 SNR ≥ 65dB 的麦克风（且这颗已停产）。
 *
 * 运行时可用 `micvol <dB>` 现场调整，立即生效（下次通话）。
 * ========================================================================== */
extern int8_t  g_adc_volume;
extern int16_t g_ul_agc_compression_gain_db;

void vk_mic_apply_defaults(void)
{
    /* ★ 11-13 回退：之前把 ADC 12→3dB、AGC 18→22dB，实测**电流声反而更大**
     * （AGC 多给 4dB 把底噪一起抬了），且降模拟增益只是让信号更弱、
     * 逼 AGC 用数字增益去补，反而放大 ADC 量化噪声 —— 对 SNR 是负优化。
     * ⇒ 恢复原值 12dB / 18dB。噪声问题不再靠增益解决，见下面的结论。 */
    g_adc_volume = 12;
    g_ul_agc_compression_gain_db = 18;
    rt_kprintf("[MIC] defaults: ADC=%ddB AGC=%ddB\n",
               g_adc_volume, g_ul_agc_compression_gain_db);
}

/* ============================================================================
 * wbs —— 宽带语音(mSBC)开关（11-13，排查"声音一段一段"用）
 * ============================================================================
 * 现象：音质差 + 声音断断续续 + 电流声，且日志出现过
 *       `msbc packet: total 252 rx error 252`（100% 包错误）。
 * 这是**链路误码**的典型表现，不是麦克风问题。
 *
 * mSBC(16kHz) 与 CVSD(8kHz) 在空中都是 64kbps，但 **mSBC 几乎没有冗余、
 * 对丢包极其敏感**；CVSD 用差分编码，丢一点还能"糊"过去。
 * ⇒ 关掉 WBS 退回 CVSD：
 *     断断续续**消失**（只是音质变闷）⇒ 链路余量不足，问题在射频/天线
 *     仍然断断续续                      ⇒ 不是编码问题，继续查射频或干扰
 * 通话中改可能不生效，建议改完重新拨号。
 * ========================================================================== */
int cmd_wbs(int argc, char **argv)
{
    extern int bt_interface_set_wbs_status(unsigned char status);
    static unsigned char wbs_on = 1;
    if (argc > 1) wbs_on = (argv[1][0] != '0');
    bt_interface_set_wbs_status(wbs_on);
    rt_kprintf("[HFP] WBS(mSBC) -> %s  (0=CVSD 更抗丢包但音质闷)\n", wbs_on ? "ON" : "OFF");
    return 0;
}
MSH_CMD_EXPORT_ALIAS(cmd_wbs, wbs, toggle wideband speech 0=CVSD 1=mSBC);

/* ============================================================================
 * nslevel —— 降噪等级（11-17，软件侧压"沙沙"底噪）
 * ============================================================================
 * 麦克风 SNR 只有 59dB，静音底噪明显；硬件不能改，只能加深降噪。
 *   0 = LOW   1 = MODERATE   2 = HIGH   3 = VERY_HIGH（当前默认）
 * 等级越高底噪压得越狠，代价是语音会有轻微"水声/发闷"，可现场对比取平衡点。
 * 立即生效（下一帧），不需要重新拨号。
 * ========================================================================== */
int cmd_nslevel(int argc, char **argv)
{
    extern int   g_vk_ns_level;
    extern void *g_vk_ns_inst;
    extern int   WebRtcNsx_set_policy(void *nsxInst, int mode);

    if (argc > 1) {
        int v = atoi(argv[1]);
        if (v < 0) v = 0;
        if (v > 3) v = 3;
        g_vk_ns_level = v;
        if (g_vk_ns_inst) {
            int r = WebRtcNsx_set_policy(g_vk_ns_inst, v);
            rt_kprintf("[NS] level -> %d (set_policy ret=%d)\n", v, r);
        } else {
            rt_kprintf("[NS] level -> %d (降噪尚未初始化, 下次通话生效)\n", v);
        }
        return 0;
    }
    rt_kprintf("[NS] level = %d  (0=LOW 1=MODERATE 2=HIGH 3=VERY_HIGH)\n", g_vk_ns_level);
    return 0;
}
MSH_CMD_EXPORT_ALIAS(cmd_nslevel, nslevel, set NS level 0~3 VERY_HIGH=3);

/* ============================================================================
 * 上行静音门（11-17，软件侧压"沙沙"底噪，且**不降带宽**）
 * ============================================================================
 * 背景：麦克风 WMM7037AT2 SNR 仅 59dB，静音时等效噪声 ~35dB(A) SPL，与安静房间相当；
 *      降噪等级提到 VERY_HIGH 会带来"水声"副作用（实测用户更偏好 HIGH），
 *      又不能降带宽（会发闷）。剩下最有效且不伤语音的手段 = 静音门。
 *
 * 做法：在 mSBC 编码前的最后一级(PCM, 已经过 3A)，按短时能量决定增益：
 *   · 每帧算 RMS，用"快降慢升"的方式跟踪噪声底 noise_rms
 *     （向下 0.2 系数，向上 0.002 系数 ≈6s，避免把语音算进噪声底）
 *   · RMS < 2.5×噪声底 → 目标增益 0.06(-24dB，听不出沙沙)
 *     RMS > 4.0×噪声底 → 目标增益 1.0
 *     （两个阈值构成迟滞，防止在临界点抖）
 *   · 增益平滑：开启 0.5/帧(≈15ms，不吞字头)，关闭 0.15/帧(≈50ms，自然收尾)
 * 副作用：极轻的"背景变安静"感，属正常；语音本身完全不处理。
 *
 * 现场命令：gate 0 关闭 / gate 1 打开（默认开）
 * ========================================================================== */
static volatile uint8_t g_gate_en   = 0;   /* 默认关: 用户实测底噪伴随语音出现, 不是静音问题 */
static float            g_gate_gain = 1.0f;
static float            g_noise_rms = 400.0f;
static volatile float   g_gate_open_ratio  = 4.0f;   /* 高于 4×噪声底 = 有语音 */
static volatile float   g_gate_close_ratio = 2.5f;   /* 低于 2.5×噪声底 = 只有底噪 */
/* ★ 11-18 "近讲模式": 用**绝对电平**当门槛(dBFS)，低于它的一律当远场/底噪压掉。
 * 0 = 关闭(退回相对噪声底模式)。
 * ⚠️ 必须关掉上行 AGC(apw agc 1) 才有意义 —— AGC 会把远处的小声也拉到目标电平，
 *    远近信息被抹平，绝对门槛就失效了。 */
static volatile float   g_gate_abs_dbfs   = 0.0f;

void vk_uplink_gate(int16_t *pcm, int nsamples)
{
    if (!g_gate_en || nsamples <= 0 || pcm == RT_NULL) return;

    /* 1) 帧 RMS */
    int64_t sum = 0;
    for (int i = 0; i < nsamples; i++) {
        int32_t v = pcm[i];
        sum += (int64_t)v * v;
    }
    float rms = (float)sqrtf((float)sum / (float)nsamples);

    /* 2) 噪声底跟踪：向下快、向上极慢 */
    if (rms < g_noise_rms) g_noise_rms += (rms - g_noise_rms) * 0.2f;
    else                   g_noise_rms += (rms - g_noise_rms) * 0.002f;
    if (g_noise_rms < 50.0f) g_noise_rms = 50.0f;    /* 防止门限掉到 0 变成永不关闭 */

    /* 3) 目标增益（迟滞） */
    float target;
    if (g_gate_abs_dbfs < -1.0f) {
        /* 近讲模式: 绝对电平门槛(带 6dB 迟滞) */
        float lin = 32768.0f * powf(10.0f, g_gate_abs_dbfs / 20.0f);
        if (rms > lin * 2.0f)        target = 1.0f;      /* 高 6dB 以上: 开 */
        else if (rms < lin)          target = 0.06f;     /* 低于门槛: 关 */
        else target = g_gate_gain;                       /* 迟滞区 */
    } else {
        if (rms > g_noise_rms * g_gate_open_ratio)        target = 1.0f;
        else if (rms < g_noise_rms * g_gate_close_ratio)  target = 0.06f;
        else target = g_gate_gain;                        /* 迟滞区: 保持不变 */
    }

    /* 4) 平滑：开启快、关闭慢 */
    float a = (target > g_gate_gain) ? 0.5f : 0.15f;
    g_gate_gain += (target - g_gate_gain) * a;

    /* 5) 应用 */
    if (g_gate_gain < 0.999f) {
        for (int i = 0; i < nsamples; i++)
            pcm[i] = (int16_t)((float)pcm[i] * g_gate_gain);
    }
}

int cmd_gate(int argc, char **argv)
{
    if (argc > 1) {
        if (argv[1][0] == '0') g_gate_en = 0;
        else                    g_gate_en = 1;
        /* 重新打开时把增益复位，避免停留在衰减态 */
        if (g_gate_en) g_gate_gain = 1.0f;
    }
    rt_kprintf("[GATE] %s  (gain=%.2f noise_rms=%.0f 开阈值=%.1fx 关阈值=%.1fx)\n",
               g_gate_en ? "ON" : "OFF", (double)g_gate_gain, (double)g_noise_rms,
               (double)g_gate_open_ratio, (double)g_gate_close_ratio);
    return 0;
}
MSH_CMD_EXPORT_ALIAS(cmd_gate, gate, uplink noise gate 0=off 1=on);

/* near —— 近讲模式（把"收音范围"缩小到嘴边）
 *   near -30   只放行电平高于 -30dBFS 的声音（约等于 5~10cm 内正常说话）
 *   near -36   门槛更低，拾音更远一点
 *   near 0     关闭，回到"相对噪声底"模式
 * ⚠️ 必须先 apw agc 1 关掉上行 AGC，否则远近电平被拉平、门槛失灵。
 * ⚠️ 门槛越低越容易通，越高越"只认近讲"。
 */
int cmd_near(int argc, char **argv)
{
    if (argc > 1) {
        float v = (float)atof(argv[1]);
        if (v > 0.0f)  v = 0.0f;
        if (v < -60.0f) v = -60.0f;
        g_gate_abs_dbfs = v;
        if (v < -1.0f) g_gate_en = 1;     /* 设置门槛即自动开门控 */
        else           g_gate_abs_dbfs = 0.0f;
    }
    rt_kprintf("[NEAR] abs threshold = %.1f dBFS (%s), gate=%d\n",
               (double)g_gate_abs_dbfs,
               g_gate_abs_dbfs < -1.0f ? "近讲模式" : "相对噪声底模式",
               (int)g_gate_en);
    return 0;
}
MSH_CMD_EXPORT_ALIAS(cmd_near, near, near-field gate threshold in dBFS 0=off);

/* ============================================================================
 * agcgain —— 上行 AGC 压缩增益（11-17，对方反馈底噪大时用）
 * ============================================================================
 * 上行 AGC 会把安静段的底噪一起抬起来（压缩增益 18dB 默认）。
 * 对方听到的"嘶嘶"有一部分就是被 AGC 放大后的本底噪声。
 * 调低（如 12~14）能压底噪，代价是说话距离稍远时对方觉得小声。
 * ⚠️ 该值在 AGC 初始化时读取（audio_3a_open），改完需要**重新拨号**生效。
 * ========================================================================== */
int cmd_agcgain(int argc, char **argv)
{
    extern int16_t g_ul_agc_compression_gain_db;
    if (argc > 1) {
        int v = atoi(argv[1]);
        if (v < 0)  v = 0;
        if (v > 30) v = 30;
        g_ul_agc_compression_gain_db = (int16_t)v;
    }
    rt_kprintf("[AGC] uplink compression gain = %d dB (改后需重新拨号生效)\n",
               (int)g_ul_agc_compression_gain_db);
    return 0;
}
MSH_CMD_EXPORT_ALIAS(cmd_agcgain, agcgain, set uplink AGC compression gain dB 0~30);

/* ============================================================================
 * whisper —— 气音/小声输入预设（11-18）
 * ============================================================================
 * 用户需求：靠近嘴边小声说话（气音）也要能输入。
 *
 * 为什么和"降噪/门控"天然冲突：
 *   气音几乎没有基频（unvoiced），能量分布和宽带噪声几乎一样，
 *   降噪算法无法区分"气音"和"底噪" —— 压噪就会连气音一起吃掉；
 *   门控按电平判断，气音电平低，会被直接关掉。
 *
 * 更根本的限制（用这颗麦克风的实测指标算）：
 *   灵敏度 -42 dBV/Pa、自身噪声等效 35 dB(A) SPL
 *     正常说话 10cm ≈ 65 dB SPL → 高于底噪 30dB，随便怎么处理都清楚
 *     气音      ≈ 45 dB SPL → 只高于底噪 **10dB**
 *   ⇒ 气音的信噪比只有 10dB，任何强降噪都会伤到它。
 *
 * 所以这个预设做了取舍：**放弃降噪强度，换取气音可懂**
 *   · 门控全关（否则气音直接被切）
 *   · 降噪回到 HIGH(2)（VERY_HIGH 会吃 unvoiced 成分）
 *   · ADC 增益提到 18dB（气音要够响，只能靠模拟增益抬）
 *   · 上行 AGC 打开、压缩增益回 18（补小声部分的电平）
 * 代价：底噪会比之前明显 —— 这是"能听见气音"的必要条件，不是 bug。
 * ========================================================================== */
int cmd_whisper(int argc, char **argv)
{
    extern int8_t  g_adc_volume;
    extern int16_t g_ul_agc_compression_gain_db;
    extern int     g_vk_ns_level;
    extern void   *g_vk_ns_inst;
    extern int     WebRtcNsx_set_policy(void *nsxInst, int mode);
    extern void    audio_3a_set_bypass(uint8_t is_bypass, uint8_t mic, uint8_t down);

    (void)argc; (void)argv;

    g_gate_en = 0;                 /* 关静音门/近讲门: 否则气音被切 */
    g_gate_abs_dbfs = 0.0f;
    g_gate_gain = 1.0f;
    g_adc_volume = 18;             /* 模拟增益抬高, 气音才够响 */
    g_ul_agc_compression_gain_db = 18;
    g_vk_ns_level = 2;             /* HIGH: 兼顾压噪与 unvoiced 保真 */
    if (g_vk_ns_inst) WebRtcNsx_set_policy(g_vk_ns_inst, 2);
    audio_3a_set_bypass(0, 0, 0);  /* 确保 3A 没被旁路 */

    rt_kprintf("[WHISPER] 气音模式: gate=OFF near=OFF ns=HIGH adc=18dB agc=18dB\n");
    rt_kprintf("[WHISPER] 底噪会比之前明显 —— 这是能听见气音的必要代价\n");
    return 0;
}
MSH_CMD_EXPORT_ALIAS(cmd_whisper, whisper, apply whisper-friendly audio preset);
