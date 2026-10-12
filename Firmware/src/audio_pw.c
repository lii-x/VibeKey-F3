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
