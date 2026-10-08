#include "rtthread.h"
#include "bf0_sibles.h"
#include "bf0_ble_gatt.h"
#include "att.h"
#include "ble_led_service.h"
#include "ws2812b.h"
#include "led.h"
#include "bt_slot.h"            /* LED 归属约定: bt_slot 独占 LED1(充电/鼠标故障), 本文件独占 LED2/3/4(app 状态灯) */
#include <string.h>


#define LOG_TAG "ble_led"
#include "ulog.h"

/* RGB colors */
#define COLOR_OFF     0x000000
#define COLOR_YELLOW  0xFFAA00
#define COLOR_GREEN   0x00FF00
#define COLOR_RED     0xFF0000
#define COLOR_WHITE   0xFFFFFF

/* 16-bit UUIDs for LED service (vendor-specific, SIG base: 0000FF00-0000-1000-8000-00805F9B34FB).
 * 改用 16-bit：空口只占 2 字节（缩短 UUID），客户端代码更简洁；
 * 必须同步改 PC 端 ble_led_worker.py（从 128-bit 字符串改为 16-bit SIG-base 展开串）。 */
#define LED_SVC_UUID_16   0xFF00
#define LED_CHAR_UUID_16  0xFF01
/* 09-03 (BLE 改建键配置): 0xFF03 = 命令特征(PC 写 CONF 命令帧);
 * 0xFF04 = 回包特征(固件 notify 响应帧)。与上位机 ble_config_worker.py 同步。 */
#define CFG_CHAR_UUID_16   0xFF03
#define CFG_RSP_UUID_16    0xFF04

enum {
    LED_SVC_IDX,      // 0 - 服务声明索引
    LED_CHAR_DECL,    // 1 - 特征声明索引
    LED_CHAR_VAL,     // 2 - 特征值索引
    LED_DIAG_DECL,    // 3 - 诊断特征声明(09-01, IMU 诊断工具保留)
    LED_DIAG_VAL,     // 4 - 诊断特征值 0xFF02: 写任意字节 -> notify 一帧 16B 快照
    LED_DIAG_CCCD,    // 5 - 诊断特征 CCCD (0x2902, notify 使能)
    LED_CFG_DECL,     // 6 - 配置命令特征声明(09-03, BLE 改建键配置)
    LED_CFG_VAL,      // 7 - 配置命令特征值 0xFF03: 写 CONF 命令帧
    LED_CFG_RSP_DECL, // 8 - 配置回包特征声明
    LED_CFG_RSP_VAL,  // 9 - 配置回包特征值 0xFF04: notify 响应帧
    LED_CFG_RSP_CCCD, // 10 - 配置回包特征 CCCD (0x2902, notify 使能)
    LED_ATT_NB        // 11 - 属性总数
};

/* 16-bit 服务：用 struct attm_desc（非 _128），uuid 字段为 16-bit 值。
 * 不再需要 BLE_GATT_VALUE_PERM_UUID_128 —— 该标志是 128-bit 专属，16-bit 路径下不能加。
 * 服务声明 0x2800 / 特征声明 0x2803 为 GAP 标准，保持不动。 */

/* 配置通道(0xFF03 写 / 0xFF04 notify)单帧最大长度。
 * ⚠️ 必须与 ble_led_cfg_reply() 的 len 闸门【同一个来源】——
 * 早期两处各写死字面量, 扩 READ 回包时只改了 att_db, 闸门仍卡 320 ->
 * 324B 的 READ 回包被静默丢弃, 上位机表现为"已连接但操作失败"。
 * 10-05: 320→336(READ 回包 304→324 / 写帧 305→323)。 */
#define CFG_FRAME_MAX_LEN   336

static struct attm_desc led_att_db[LED_ATT_NB] = {
    /* Service declaration */
    [LED_SVC_IDX] = {
        ATT_DECL_PRIMARY_SERVICE, PERM(RD, ENABLE), 0, 0
    },
    /* Characteristic declaration */
    [LED_CHAR_DECL] = {
        ATT_DECL_CHARACTERISTIC, PERM(RD, ENABLE), 0, 0
    },
    /* Characteristic value - write only. 16-bit vendor UUID 0xFF01 */
    [LED_CHAR_VAL] = {
        LED_CHAR_UUID_16, PERM(WRITE_REQ, ENABLE) | PERM(WRITE_COMMAND, ENABLE), 0, 64
    },
    /* 09-01 诊断特征(IMU 诊断工具): 0xFF02 可写(触发一帧 notify 快照) + NTF。
     * 上位机先订阅 CCCD, 再写任意字节, 固件回发 16B imu_diag_fill() 快照。 */
    [LED_DIAG_DECL] = {
        ATT_DECL_CHARACTERISTIC, PERM(RD, ENABLE), 0, 0
    },
    [LED_DIAG_VAL] = {
        0xFF02, PERM(WRITE_REQ, ENABLE) | PERM(WRITE_COMMAND, ENABLE) | PERM(NTF, ENABLE),
        PERM(RI, ENABLE), 16
    },
    [LED_DIAG_CCCD] = {
        ATT_DESC_CLIENT_CHAR_CFG, PERM(RD, ENABLE) | PERM(WRITE_REQ, ENABLE),
        PERM(RI, ENABLE), 2
    },
    /* 09-03 配置命令特征(0xFF03): PC 写一条完整 CONF 命令帧 —— 与 USB CDC 的
     * 线上格式完全一致: [4B CONF_MAGIC][1B cmd][1B slot][2B data_len][payload]
     * (10-05: 最大 8 + 315 = 323B —— EC 按压滚动追加后 WRITE payload 297→315,
     * max_len 同步 320→336; 单帧还受 ATT MTU 限制(需 MTU>=326), 见 proj.conf
     * CONFIG_BT_L2CAP_TX_MTU=340)。固件原样投 conf_task 线程处理: 解析/落盘/回包与
     * USB 共用同一套逻辑, 保证双通道行为一致。 */
    [LED_CFG_DECL] = {
        ATT_DECL_CHARACTERISTIC, PERM(RD, ENABLE), 0, 0
    },
    [LED_CFG_VAL] = {
        CFG_CHAR_UUID_16, PERM(WRITE_REQ, ENABLE) | PERM(WRITE_COMMAND, ENABLE), 0, CFG_FRAME_MAX_LEN
    },
    /* 09-03 配置回包特征(0xFF04, notify): conf_task 处理完命令后由
     * ble_led_cfg_reply() 推一帧。帧内容随命令而异:
     *   READ  -> 324B 配置内存镜像(sizeof(key_config_storage_t),
     *            10-05 由 304 增至 324 —— EC 按压滚动尾部追加 2×9B);
     *   WRITE/RESET -> 1B 状态码(0x00=成功);
     *   INFO  -> ASCII 文本 "VibeKey-F3|<ver>|<bat>|<slot>\n"。 */
    [LED_CFG_RSP_DECL] = {
        ATT_DECL_CHARACTERISTIC, PERM(RD, ENABLE), 0, 0
    },
    [LED_CFG_RSP_VAL] = {
        CFG_RSP_UUID_16, PERM(NTF, ENABLE), PERM(RI, ENABLE), CFG_FRAME_MAX_LEN
    },
    [LED_CFG_RSP_CCCD] = {
        ATT_DESC_CLIENT_CHAR_CFG, PERM(RD, ENABLE) | PERM(WRITE_REQ, ENABLE),
        PERM(RI, ENABLE), 2
    },
};

/* State + timer */
static uint8_t  g_led_state = LED_STATE_OFF;
static uint8_t  g_led_current[3] = {0, 0, 0};
static uint32_t g_led_color = COLOR_OFF;
static uint8_t  g_led_blink_on = 0;
static uint32_t g_led_done_tick = 0;
static sibles_hdl g_led_svc_handle;
static struct rt_timer g_led_timer;   /* 声明在前：led_timer_tick 中 rt_timer_stop 需要 */

static void led_apply_color(uint32_t color)
{
    g_led_color = color;
    g_led_current[0] = (color >> 16) & 0xFF;
    g_led_current[1] = (color >> 8) & 0xFF;
    g_led_current[2] = color & 0xFF;
    rgb_led_set_color(color);
}

static void led_timer_tick(void *param)
{
    /* LED 归属划分(08-31 收敛; 08-26/08-27 的"充电让位"判断已整体移除):
     *   LED1(蓝, PA5)            = 充电指示 + 鼠标故障蓝闪  -> bt_slot.c 独占
     *   LED2(黄)/LED3(绿)/LED4(红) = app 经 BLE 推送的状态灯  -> 本文件独占
     * 两者物理上完全不重叠, 且 led_status_mutex() 只操作 LED2/3/4、碰不到 LED1,
     * 故插着充电器时 app 状态灯照常显示, 不再被 bt_multi_charge_active() 拦掉。
     *
     * ⚠️ 历史坑(本次修复的 bug): 08-26 充电指示曾占用 LED2/LED3, 当时在此加了
     *   `if (bt_multi_charge_active() || bt_multi_mouse_error_active()) return;`
     *   让位; 08-29 充电收敛到 LED1 后忘了同步删, 变成"无冲突却仍拦截" ——
     *   插电时 app 推送的 state 被静默吞掉: GATT 回调照常打 [LED SVC] state=N,
     *   但定时器每次提前 return, LED2/3/4 永远不会被点亮。 */
    switch (g_led_state)
    {
    case LED_STATE_BUSY:    //忙碌
        //led_apply_color(COLOR_YELLOW);
        led_status_mutex(BSP_LED2_PIN,1);
        /* 持续亮灯，无需 50ms 刷新。停定时器省功耗，下次 set_state 恢复。*/
        rt_timer_stop(&g_led_timer);
        break;
    case LED_STATE_DONE:    //完成
        //led_apply_color(COLOR_GREEN);
        led_status_mutex(BSP_LED3_PIN,1);
        /* 持续亮灯，无需 50ms 刷新。停定时器省功耗，下次 set_state 恢复。*/
        rt_timer_stop(&g_led_timer);
        break;
    case LED_STATE_ERROR:   //错误
        //led_apply_color(COLOR_RED);
        led_status_mutex(BSP_LED4_PIN,1);
        /* 持续亮灯，无需 50ms 刷新。停定时器省功耗，下次 set_state 恢复。*/
        rt_timer_stop(&g_led_timer);
        break;
    default:
        //led_apply_color(COLOR_OFF);
        led_status_mutex(BSP_LED4_PIN,0);
        /* OFF 态无需周期刷新：停掉自身定时器省 50ms 唤醒。
         * 下次 ble_led_set_state 会 rt_timer_start 恢复。
         * rt_timer_stop 在软定时器回调中调用是安全的（标记 stopped，下轮不触发）。*/
        rt_timer_stop(&g_led_timer);
        break;
    }
}

/* GATT callbacks */
static uint8_t *led_gatts_get_cbk(uint8_t conn_idx, uint8_t idx, uint16_t *len)
{
    *len = 0;
    return NULL;
}

static uint8_t led_gatts_set_cbk(uint8_t conn_idx, sibles_set_cbk_t *para)
{
    if (para->idx == LED_CHAR_VAL && para->len >= 1) {
        uint8_t state = para->value[0];
        // 0/1/2/3 为 LED 状态机（WAITING 已移除，不再接受）
        if (state == LED_STATE_OFF || state == LED_STATE_BUSY
            || state == LED_STATE_DONE || state == LED_STATE_ERROR) {
            ble_led_set_state(state);
            rt_kprintf("[LED SVC] state=%d\n", state);
        }
    }
    /* 09-01 (保留, IMU 诊断工具): 0xFF02 诊断特征 —— 写任意字节触发一帧 notify 快照(16B) */
    else if (para->idx == LED_DIAG_VAL) {
        extern void imu_diag_fill(uint8_t buf[16]);
        static uint8_t diag_buf[16];
        imu_diag_fill(diag_buf);
        sibles_value_t value;
        value.hdl  = g_led_svc_handle;
        value.idx  = LED_DIAG_VAL;
        value.len  = sizeof(diag_buf);
        value.value = diag_buf;
        sibles_write_value(conn_idx, &value);
    }
    /* 09-03 (BLE 改建键配置): 0xFF03 命令特征 —— PC 写一条 CONF 命令帧。
     * GATT 回调线程不能直接做 flash 写(与 USB ISR 同款陷阱: mutex 断言),
     * 这里只拷贝投递 conf_task 线程(USB/BLE 共用), 由它解析并回包。 */
    else if (para->idx == LED_CFG_VAL && para->len >= 8) {
        extern int conf_ble_submit(uint8_t conn_idx, const uint8_t *buf, uint16_t len);
        if (conf_ble_submit(conn_idx, para->value, para->len) != 0)
            rt_kprintf("[LED SVC] cfg submit failed (len=%u)\n", para->len);
    }
    return 0;
}

/* 09-03 (BLE 改建键配置): conf_task 线程处理完命令后的 notify 回包入口。
 * SDK sibles_write_value: acquire_tx_pkts(临界区)+同步 memcpy 到独立消息+
 * sifli_msg_send 投递, 不持有调用者 buffer, 可从任意线程调用 —— 实测结论见
 * bf0_sibles.c:298/1227。len 上限 = CFG_FRAME_MAX_LEN(与 att_db max_len 同源;
 * 09-05 由 64 放宽, 09-07 随 sizeof 276→304, 10-05 随 304→324)。
 * ⚠️ 这里的闸门若小于 att_db 的 max_len, 超长回包会【静默丢弃】且只留一行 kprintf,
 *    上位机侧表现为"已连接但操作失败"。两处务必都用 CFG_FRAME_MAX_LEN。 */
void ble_led_cfg_reply(uint8_t conn_idx, const uint8_t *data, uint16_t len)
{
    if (g_led_svc_handle == 0 || data == NULL || len == 0 || len > CFG_FRAME_MAX_LEN)
        return;
    sibles_value_t value;
    value.hdl   = g_led_svc_handle;
    value.idx   = LED_CFG_RSP_VAL;
    value.len   = len;
    value.value = (uint8_t *)(uintptr_t)data;
    if (sibles_write_value(conn_idx, &value) <= 0)
        rt_kprintf("[LED SVC] cfg reply notify failed (len=%u)\n", len);
}

void ble_led_set_state(uint8_t state)
{
    g_led_state = state;
    g_led_blink_on = 0;

    if (state == LED_STATE_DONE)
        g_led_done_tick = rt_tick_get() + rt_tick_from_millisecond(3000);
    else
        g_led_done_tick = 0;

    /* No direct LED operation here — this may be called from ISR context.
     * The timer thread (50ms period) will apply the actual color.
     * 若此前因断连已停定时器(ble_led_turn_off)，此处无条件重启恢复状态机
     * 周期应用；rt_timer_start 对已运行定时器幂等且 ISR 安全。*/
    rt_timer_start(&g_led_timer);
}

/* BLE 断连时熄灭所有 app 推送的状态灯。
 * 三颗状态灯(LED2/LED3/LED4)由 app 经 BLE(或 USB)实时推送，属“瞬态指示”；
 * 蓝牙一旦断开，这些灯不应继续亮着（否则断连后仍显示绿/红/黄，且浪费电）。
 * 直接复位状态机并立即关灯：led_status_mutex 先全关再按 status 点亮指定灯，
 * 传 status=0 即不点亮任何灯 → 三颗全灭（不等 50ms 定时器，断开即刻生效）。*/
void ble_led_turn_off(void)
{
    /* 08-31: 移除充电/鼠标故障让位判断。LED1(充电常亮/鼠标故障蓝闪)由 bt_slot.c
     * 独占, 而 led_status_mutex() 只关 LED2/3/4、动不到 LED1, 所以断连时直接
     * 全灭三颗 app 状态灯是安全的, 不会掐掉充电指示或故障蓝闪。
     * (旧版拦在这里还有个副作用: 充电中蓝牙断开 → app 状态灯一直亮着灭不掉。) */
    g_led_state = LED_STATE_OFF;
    g_led_blink_on = 0;
    g_led_done_tick = 0;
    led_status_mutex(BSP_LED4_PIN, 0);
    /* 停 50ms 状态灯定时器：断连后 app 不再推送，无需继续周期刷新，
     * 否则该定时器持续唤醒系统、阻止 RT-Thread PM 进深睡。
     * 下次 ble_led_set_state 会重新 rt_timer_start 恢复。*/
    rt_timer_stop(&g_led_timer);
    rt_kprintf("[LED SVC] turned off on disconnect\n");
}

void ble_led_service_init(void)
{
    rt_kprintf("[LED SVC] init\n");

    rt_timer_init(&g_led_timer, "led_tmr", led_timer_tick, RT_NULL,
                   rt_tick_from_millisecond(50), RT_TIMER_FLAG_PERIODIC | RT_TIMER_FLAG_SOFT_TIMER);
    rt_timer_start(&g_led_timer);
}

void ble_led_register_service(void)
{
    sibles_register_svc_t svc;
    svc.att_db    = (struct attm_desc *)&led_att_db;
    svc.num_entry = LED_ATT_NB;
    svc.sec_lvl   = PERM(SVC_AUTH, NO_AUTH);
    svc.uuid      = LED_SVC_UUID_16;
    g_led_svc_handle = sibles_register_svc(&svc);
    if (g_led_svc_handle) {
        sibles_register_cbk(g_led_svc_handle, led_gatts_get_cbk, led_gatts_set_cbk);
        rt_kprintf("[LED SVC] registered (16-bit UUID 0x%04X)\n", LED_SVC_UUID_16);
    } else {
        rt_kprintf("[LED SVC] register FAILED\n");
    }
}
