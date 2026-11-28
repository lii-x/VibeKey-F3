#include <rtthread.h>
#include <string.h>
#include "drv_flash.h"
#include "bt_slot.h"
#include "ws2812b.h"
#include "ble_connection_manager.h"
#include "ble_hid.h"
#include "key_config.h"
#include "led.h"                /* 充电指示用独立 GPIO LED: LED2(黄)/LED3(绿), 非槽位背光 */

/* ============================================================================
 * 三设备(蓝牙1/2/3)切换: 存储 + 状态机 + RGB 指示
 *
 * 上电按键选择:
 *   C1/C2/C3 按下 -> 蓝牙 1/2/3 连接模式: 删除无关, 广播等已配对设备回连,
 *                   对应 RGB1/2/3 蓝色闪烁, 连上(或配对完成)熄灭;
 *   L1/L2/L3 按下 -> 蓝牙 1/2/3 配对模式: 删除该槽位 BLE+BR/EDR 配对信息,
 *                   重新配对, 对应 RGB 黄色闪烁, 配对成功熄灭;
 *   无按键        -> 使用最近一次被选择的蓝牙槽位(连接模式)。
 *
 * 配对信息归属: BLE 侧由 SDK Connection Manager 管理(MAX_PAIR_DEV=3, KVDB
 * 自动持久化); 本模块只负责记录"每个槽位对应哪台设备"的地址映射, 以及
 * 上电/事件驱动的 LED 指示与配对删除。经典蓝牙(HFP)与 BLE 共享同一 BD_ADDR,
 * 删除配对时两栈一起删。
 * ============================================================================ */

extern ble_hid_env_t g_hid_env;

static bt_slot_storage_t g_slot;

/* 待删除经典 linkkey 的对端地址（延迟 5s 执行，避开 SDP 初始化） */
static uint8_t g_cls_key_del_addr[6];
static volatile uint8_t g_cls_key_del_pending = 0;
static rt_timer_t g_cls_key_del_timer = RT_NULL;

static void cls_key_del_cb(void *param)
{
    (void)param;
    if (!g_cls_key_del_pending) return;
    g_cls_key_del_pending = 0;
    extern void bt_cm_delete_bonded_devs_and_linkkey(uint8_t *addr);
    bt_cm_delete_bonded_devs_and_linkkey(g_cls_key_del_addr);
    rt_kprintf("[BTSLOT] 已删除经典 linkkey(%02x:%02x:%02x:%02x:%02x:%02x), 等主机重新配对\n",
               g_cls_key_del_addr[5], g_cls_key_del_addr[4], g_cls_key_del_addr[3],
               g_cls_key_del_addr[2], g_cls_key_del_addr[1], g_cls_key_del_addr[0]);
}

/* 配对模式开机时暂存的"本槽原设备地址"（供栈就绪后精确删 bond 用） */
static uint8_t g_pair_prev_peer[6];
static uint8_t g_pair_prev_valid = 0;

/* ---- 当前多设备状态 ---- */
static uint8_t g_cur_slot  = 1;
static uint8_t g_cur_mode  = BT_MODE_CONNECT;
static uint8_t g_mode_applied = 0;   /* 配对删除仅执行一次(等 BLE 栈就绪) */

/* ---- RGB 闪烁指示 (槽位键背光, ws2812b) ---- */
#define BT_BLINK_MS        300
#define RGB_COLOR_BLUE     0x0000FF   /* 连接模式: 蓝闪(等回连/等配对) */
#define RGB_COLOR_YELLOW   0xFFFF00   /* 配对模式(L 键): 黄闪(等新设备配对) */
#define RGB_COLOR_CYAN     0x00FFFF   /* BLE 已连/配对完成, HFP 未建立: 青闪(等 HFP) */
#define BT_RGB_BRIGHTNESS_PCT  10     /* 模式指示亮度系数(%): 10% 调暗, 100=全亮 */
static rt_timer_t g_blink_timer = RT_NULL;
static uint8_t g_blink_slot  = 0;   /* 1~3, 0=未在闪烁 */
static uint32_t g_blink_color = 0;
static uint8_t g_blink_on = 0;

/* ---- 充电指示 (08-26, 08-29 收敛到蓝灯 LED1) ----
 * 硬件: LED1=蓝(PA5)、LED2=黄(PA6)、LED3=绿(PA7)、LED4=红(PA8)。
 * 08-29: LED2(黄)/LED3(绿)不再做充电指示, 交还 ble_led_service 状态灯;
 * 充电指示只由蓝灯 LED1 承担: 充电中亮、充满/拔出灭。
 * 鼠标故障蓝闪也走 LED1(优先): 故障闪烁期间不动 LED1, 恢复时按充电状态恢复常亮。 */
static uint8_t g_charge_state = 0;   /* 0=无充电 1=充电中 2=充满 */

/* 充电指示是否激活。08-31 起 ble_led_service 不再调用它让位(LED1 与 LED2/3/4 无重叠) */
uint8_t bt_multi_charge_active(void)
{
    return (g_charge_state != 0);
}

/* 24bit RGB 每通道按系数缩放(四舍五入), 用于把模式指示灯调暗 */
static uint32_t bt_rgb_dim(uint32_t c)
{
    uint32_t r = (c >> 16) & 0xFF;
    uint32_t g = (c >> 8)  & 0xFF;
    uint32_t b =  c        & 0xFF;
    r = (r * BT_RGB_BRIGHTNESS_PCT + 50) / 100;
    g = (g * BT_RGB_BRIGHTNESS_PCT + 50) / 100;
    b = (b * BT_RGB_BRIGHTNESS_PCT + 50) / 100;
    return (r << 16) | (g << 8) | b;
}

/* ============================ 存储层 ============================ */

void bt_slot_init(void)
{
    rt_flash_read(BT_SLOT_FLASH_ADDR, (uint8_t *)&g_slot, sizeof(g_slot));
    /* 迁移到 v5：地址布局改为"每槽独占字节位"。
     * v3(只有 rand_base) / v4(每槽地址但共用倒数第二字节) 都统一重排一次：
     *   [5..2] 沿用设备前缀，[1] = 0x10 + 槽号，[0] = 换址计数器（从 0 起）
     * ⚠️ 这会改变本机地址 —— 升级后需要在主机端重新配对一次。
     * 但这是必要的：旧布局下三槽的换址序列共用同一段数字，早晚撞车。 */
    if (g_slot.magic == BT_SLOT_MAGIC &&
        (g_slot.version == 3 || g_slot.version == 4)) {
        uint8_t tpl[6] = {0};
        uint8_t oldver = g_slot.version;                 /* 打印用（下面会把 version 改成 5） */
        if (g_slot.version == 4 && g_slot.self_valid) {
            memcpy(tpl, g_slot.self_addr[0], 6);      /* 用槽1 现有前缀 */
        } else {
            memcpy(tpl, g_slot.rand_base, 6);
        }
        { extern uint8_t btid_rand8(void);
          for (int i = 0; i < BT_SLOT_NUM; i++) {
            memcpy(&g_slot.self_addr[i][0], tpl, 6);
            g_slot.self_addr[i][1] = (uint8_t)(0x10 + (i + 1));  /* 0x11/0x12/0x13 独占 */
            /* 计数器起点用真随机：固定从 0 起会让设备间唯一性只剩 4 字节随机
             * （30 位有效熵），万级出货撞车约 4.7%；随机起点补到 38 位。 */
            g_slot.self_addr[i][0] = btid_rand8();
          } }
        g_slot.self_valid = 1;
        g_slot.version = BT_SLOT_VERSION;
        bt_slot_save();
        rt_kprintf("[BTSLOT] migrated v%d->v5: 每槽独占字节位 "
                   "(slot1 %02x:%02x:%02x:%02x:%02x:%02x / slot2 %02x:%02x:%02x:%02x:%02x:%02x / "
                   "slot3 %02x:%02x:%02x:%02x:%02x:%02x)\n",
                   oldver,
                   g_slot.self_addr[0][5], g_slot.self_addr[0][4], g_slot.self_addr[0][3],
                   g_slot.self_addr[0][2], g_slot.self_addr[0][1], g_slot.self_addr[0][0],
                   g_slot.self_addr[1][5], g_slot.self_addr[1][4], g_slot.self_addr[1][3],
                   g_slot.self_addr[1][2], g_slot.self_addr[1][1], g_slot.self_addr[1][0],
                   g_slot.self_addr[2][5], g_slot.self_addr[2][4], g_slot.self_addr[2][3],
                   g_slot.self_addr[2][2], g_slot.self_addr[2][1], g_slot.self_addr[2][0]);
    }
    if (g_slot.magic != BT_SLOT_MAGIC || g_slot.version != BT_SLOT_VERSION) {
        rt_kprintf("[BTSLOT] invalid magic, init defaults\n");
        memset(&g_slot, 0, sizeof(g_slot));
        g_slot.magic = BT_SLOT_MAGIC;
        g_slot.version = BT_SLOT_VERSION;
        g_slot.last_slot = 1;
        bt_slot_save();
    } else {
        if (g_slot.last_slot < 1 || g_slot.last_slot > BT_SLOT_NUM)
            g_slot.last_slot = 1;
        rt_kprintf("[BTSLOT] loaded: last_slot=%d valid=[%d %d %d]\n",
                   g_slot.last_slot,
                   g_slot.slot_valid[0], g_slot.slot_valid[1], g_slot.slot_valid[2]);
    }
}

void bt_slot_save(void)
{
    g_slot.magic = BT_SLOT_MAGIC;
    g_slot.version = BT_SLOT_VERSION;
    rt_flash_erase(BT_SLOT_FLASH_ADDR, 4096);
    rt_flash_write(BT_SLOT_FLASH_ADDR, (const uint8_t *)&g_slot, sizeof(g_slot));
    rt_kprintf("[BTSLOT] saved to flash\n");
}

uint8_t bt_slot_get_last(void)
{
    return g_slot.last_slot;
}

void bt_slot_set_last(uint8_t slot)
{
    if (slot < 1 || slot > BT_SLOT_NUM) return;
    if (g_slot.last_slot == slot) return;
    g_slot.last_slot = slot;
    bt_slot_save();
}

uint8_t bt_slot_get_addr(uint8_t slot, uint8_t *out6)
{
    if (slot < 1 || slot > BT_SLOT_NUM) return 0;
    if (!g_slot.slot_valid[slot - 1]) return 0;
    if (out6) memcpy(out6, g_slot.slot_addr[slot - 1], 6);
    return 1;
}

/* ---- v4: 每槽本地地址（本机自己的 BT 地址，不是对端） ---- */
uint8_t bt_slot_get_self_addr(uint8_t slot, uint8_t *out6)
{
    if (slot < 1 || slot > BT_SLOT_NUM) return 0;
    if (!g_slot.self_valid) return 0;
    if (out6) memcpy(out6, g_slot.self_addr[slot - 1], 6);
    return 1;
}

void bt_slot_set_self_addr(uint8_t slot, const uint8_t *in6)
{
    if (slot < 1 || slot > BT_SLOT_NUM || !in6) return;
    memcpy(g_slot.self_addr[slot - 1], in6, 6);
    g_slot.self_valid = 1;
    bt_slot_save();
}

/* 换址：最低字节 +1。模 256 回绕也不怕 —— 三个槽同时回绕的概率可忽略，
 * 这里仍做一次碰撞检查，撞上就再 +1。 */
void bt_slot_rotate_self_addr(uint8_t slot)
{
    if (slot < 1 || slot > BT_SLOT_NUM || !g_slot.self_valid) return;
    uint8_t *a = g_slot.self_addr[slot - 1];
    uint8_t old = a[0];

    /* ★ 11-28 v5：只有最低字节是"换址计数器"，倒数第二字节固定为本槽专属
     * （0x11/0x12/0x13）。三槽的序列各自在自己的字节位上走，永远不会相交 ——
     * 旧实现三槽共用倒数第二字节，靠"撞槽检查"临时避让，历史序列仍会交叉。
     * 这里顺手把 [1] 钉正，兼容从旧布局升级过来、尚未重排的残留数据。 */
    a[1] = (uint8_t)(0x10 + slot);
    a[0] = (uint8_t)(a[0] + 1);

    rt_kprintf("[BTSLOT] slot%d 换址 0x%02x->0x%02x, 新本地地址 %02x:%02x:%02x:%02x:%02x:%02x\n",
               slot, old, a[0], a[5], a[4], a[3], a[2], a[1], a[0]);
    bt_slot_save();
}

void bt_slot_set_addr(uint8_t slot, const uint8_t *addr6)
{
    if (slot < 1 || slot > BT_SLOT_NUM || !addr6) return;
    if (g_slot.slot_valid[slot - 1] &&
        memcmp(g_slot.slot_addr[slot - 1], addr6, 6) == 0)
        return;   /* 无变化, 不写 Flash */
    g_slot.slot_valid[slot - 1] = 1;
    memcpy(g_slot.slot_addr[slot - 1], addr6, 6);
    bt_slot_save();
}

void bt_slot_clear(uint8_t slot)
{
    if (slot < 1 || slot > BT_SLOT_NUM) return;
    if (!g_slot.slot_valid[slot - 1]) return;
    g_slot.slot_valid[slot - 1] = 0;
    memset(g_slot.slot_addr[slot - 1], 0, 6);
    bt_slot_save();
}

uint8_t bt_slot_get_rand_base(uint8_t *out6)
{
    if (!g_slot.rand_valid) return 0;
    if (out6) memcpy(out6, g_slot.rand_base, 6);
    return 1;
}

void bt_slot_set_rand_base(const uint8_t *base6)
{
    if (!base6) return;
    if (g_slot.rand_valid &&
        memcmp(g_slot.rand_base, base6, 6) == 0)
        return;   /* 无变化, 不写 Flash */
    g_slot.rand_valid = 1;
    memcpy(g_slot.rand_base, base6, 6);
    bt_slot_save();
}

/* ============================ RGB 指示 ============================ */

static void blink_cb(void *param)
{
    (void)param;
    if (!g_blink_slot) return;
    g_blink_on = !g_blink_on;
    rgb_led_show(g_blink_slot - 1, g_blink_on ? g_blink_color : 0x000000);
}

static void bt_multi_led_start(uint8_t slot, uint32_t color)
{
    if (slot < 1 || slot > BT_SLOT_NUM) return;
    color = bt_rgb_dim(color);   /* 统一压暗到 BT_RGB_BRIGHTNESS_PCT(闪烁+初始点亮都生效) */
    g_blink_slot = slot;
    g_blink_color = color;
    g_blink_on = 1;
    if (!g_blink_timer) {
        g_blink_timer = rt_timer_create("btslot_blk", blink_cb, RT_NULL,
            rt_tick_from_millisecond(BT_BLINK_MS),
            RT_TIMER_FLAG_PERIODIC | RT_TIMER_FLAG_SOFT_TIMER);
    }
    if (g_blink_timer) {
        rt_timer_stop(g_blink_timer);
        rt_timer_start(g_blink_timer);
    }
    rgb_led_show(slot - 1, color);
}

static void bt_multi_led_stop(void)
{
    if (g_blink_timer) rt_timer_stop(g_blink_timer);
    if (g_blink_slot) rgb_led_show(g_blink_slot - 1, 0x000000);
    g_blink_slot = 0;
}

void bt_multi_led_stop_all(void)
{
    bt_multi_led_stop();
}

/* 09-16: 抑制槽位指示闪烁。进待机前由 power.c 置 1 —— 连接态待机会【主动断 BLE】,
 * 断开事件随即调 bt_multi_on_disconnected() → bt_multi_start_indication() 起一个 PERIODIC
 * 闪烁软定时器; 只要该定时器还有周期, PM 框架就永远到不了 tick==MAX → DEEP 进不去
 * (降级 IDLE) → 整晚高电流把电池耗光(用户实测: 47% 睡一晚耗干)。置 1 后既立即停闪,
 * 也不允许再起定时器; 唤醒时由 power_resume_from_deep 置 0 恢复指示。 */
static uint8_t g_ind_suppress = 0;

void bt_multi_suppress_indication(uint8_t on)
{
    g_ind_suppress = on ? 1 : 0;
    if (g_ind_suppress) bt_multi_led_stop();   /* 若已起, 立即停 */
}

/* ---- 鼠标故障蓝闪(08-29): 蓝灯 LED1(PA5), 非按键 RGB ----
 * 用户指定: 鼠标移动出问题时用 LED1(PA5)蓝色闪烁提示(08-27 原为 LED4 红灯)。
 * 蓝灯同时承担充电指示(bt_multi_set_charge): 常亮=充电中, 蓝闪=鼠标故障,
 * 故障闪烁优先(bt_multi_set_charge 会检查 g_mouse_err_led); 故障恢复时按
 * g_charge_state 还原充电常亮/熄灭。BLE 状态灯(LED2/3/4)不受影响。 */
#define MOUSE_ERR_BLINK_MS  300
static uint8_t g_mouse_err_led = 0;      /* 1=鼠标故障蓝闪激活 */
static uint8_t g_mouse_err_blink_on = 0;
static rt_timer_t g_mouse_err_timer = RT_NULL;

static void mouse_err_blink_cb(void *param)
{
    (void)param;
    if (!g_mouse_err_led) return;
    g_mouse_err_blink_on = !g_mouse_err_blink_on;
    led_status(BSP_LED1_PIN, g_mouse_err_blink_on ? BSP_LED1_ACTIVE : !BSP_LED1_ACTIVE);
}

/* 鼠标故障蓝闪(08-29): main.c 的 IMU 看门狗判停后 LED1(PA5)蓝闪提示"移动通道异常",
 * 恢复后熄灭并还原充电常亮(若正在充电)。 */
void bt_multi_set_mouse_error(uint8_t err)
{
    if (err) {
        if (g_mouse_err_led) return;
        g_mouse_err_led = 1;
        g_mouse_err_blink_on = 1;
        if (!g_mouse_err_timer) {
            g_mouse_err_timer = rt_timer_create("mse_err", mouse_err_blink_cb, RT_NULL,
                rt_tick_from_millisecond(MOUSE_ERR_BLINK_MS),
                RT_TIMER_FLAG_PERIODIC | RT_TIMER_FLAG_SOFT_TIMER);
        }
        if (g_mouse_err_timer) rt_timer_start(g_mouse_err_timer);
        led_status(BSP_LED1_PIN, BSP_LED1_ACTIVE);
    } else {
        if (!g_mouse_err_led) return;
        g_mouse_err_led = 0;
        if (g_mouse_err_timer) rt_timer_stop(g_mouse_err_timer);
        /* 熄灭蓝灯; 充电中则还原常亮 */
        led_status(BSP_LED1_PIN, (g_charge_state == 1) ? BSP_LED1_ACTIVE : !BSP_LED1_ACTIVE);
    }
}

/* 鼠标故障蓝闪是否激活(ble_led_service 让位用, 同 bt_multi_charge_active 模式) */
uint8_t bt_multi_mouse_error_active(void)
{
    return g_mouse_err_led;
}

/* 充电指示(08-29 收敛到蓝灯 LED1): 0=无充电 1=充电中 2=充满。
 * 充电中 -> 蓝灯(LED1)亮; 充满/拔出 -> 蓝灯灭。
 * 由 main.c 的 1s 轮询按内置充电器 detect/full 状态调用。
 * 槽位 RGB(蓝牙模式灯)与 ble_led_service 状态灯(LED2/3/4)均不受影响;
 * 鼠标故障蓝闪(bt_multi_set_mouse_error)优先 —— 故障闪烁期间不动 LED1,
 * 故障恢复时按 g_charge_state 还原充电常亮。 */
void bt_multi_set_charge(uint8_t state)
{
    uint8_t old = g_charge_state;
    if (state > 2) state = 2;
    g_charge_state = state;
    if (old == state) return;

    /* 08-29: LED2(黄)/LED3(绿)不再做充电指示, 充电指示只由蓝灯 LED1 承担 */
    if (!g_mouse_err_led)
        led_status(BSP_LED1_PIN, (state == 1) ? BSP_LED1_ACTIVE : !BSP_LED1_ACTIVE);
}

/* 按当前充电状态恢复蓝灯(LED1)充电指示(08-29): 开机电量显示等临时占用 LED1
 * 结束后调用, 避免把充电常亮误灭。鼠标故障蓝闪优先, 闪烁期间不动。 */
void bt_multi_restore_charge_led(void)
{
    if (g_mouse_err_led) return;
    led_status(BSP_LED1_PIN, (g_charge_state == 1) ? BSP_LED1_ACTIVE : !BSP_LED1_ACTIVE);
}

static uint8_t g_hfp_live = 0;   /* HFP(经典语音)是否已建立; ble_app.c 事件通知更新 */

/* HFP 状态通知(ble_app.c 在 HFP connected/disconnected 时调用):
 * hfp_on=1 -> 熄灭模式指示(HFP 就绪, 全链路 OK)
 * hfp_on=0 -> BLE 仍连则青色闪(等 HFP 重建); BLE 未连则由蓝/黄闪管 */
void bt_multi_on_hfp_state(uint8_t hfp_on)
{
    extern int bt_host_linked(void);
    g_hfp_live = hfp_on;
    /* 10-31: 原来用 g_hid_env.is_connected(BLE) 判据 —— 手机只走经典蓝牙，
     * BLE 永远不连 ⇒ 这里直接 return，HFP 就绪后槽位灯**永远不熄灭**。改判"任一宿主已连"。 */
    if (!bt_host_linked()) return;
    /* ★ 11-25 经典关闭时，HFP 状态变化不应再点亮"等 HFP"的青色 */
    { extern uint8_t key_config_bt_flags(void);
      if (!(key_config_bt_flags() & KEY_CONFIG_BT_CLASSIC_EN)) {
          bt_multi_led_stop();
          return;
      } }
    if (hfp_on)
        bt_multi_led_stop();
    else
        bt_multi_led_start(g_cur_slot, RGB_COLOR_CYAN);
}

/* ============================ 配对窗口 ============================ */
/* ⚠️ 方案B(每槽独立 BD_ADDR)整改: 旧的"配对模式防抢连拒绝列表"(收集其他槽位
 * + SDK 全部 bond 地址, 配对模式下一律断开)已整体移除。
 * 原因: 方案B 下每槽地址不同 —— 绑定在别的地址下的设备看不见本槽广播, 不存在
 * 抢配风险; 拒绝列表反而把用户想配进本槽的"已在别的槽配过的设备"挡在门外
 * (实测: 槽1 配完电脑, L2 想把它配进槽2 → PAIR window reject 被拒 / SMP 120)。
 * 当前槽位旧设备的防抢配由 bt_multi_clear_slot_bond() 删 bond 天然覆盖
 * (bond 已删, 旧设备回连无法认证)。 */

uint8_t bt_multi_is_pairing_window(void)
{
    /* 只有显式 L 键配对模式才算"配对窗口"。空槽 C 键连接模式 = 连接模式,
     * 接受任何设备(已配对→记录/连接, 新设备→正常配对)。 */
    return (g_cur_mode == BT_MODE_PAIR);
}

/* ============================ 配对删除 ============================ */

/* 删除当前槽位记录的配对信息: BLE 侧(Connection Manager) + 经典蓝牙侧(linkkey)。
 * 需在 BLE 栈就绪后调用(connection_manager_get_bonded_devices 才有效)。
 * ⚠️ 11-28 重新启用：配对模式再次调用它清本槽 bond（见 bt_multi_apply_mode）。
 *    只清"本槽记录的那台设备"，不清其它槽的记录。 */
static void bt_multi_clear_bond_by_addr(uint8_t *addr);

static void bt_multi_clear_slot_bond(void)
{
    uint8_t addr[6];
    /* 优先用"配对开机时暂存的原设备地址"——记录已在 boot_select 清掉，
     * 但 bond 还在，必须拿旧地址去删，否则永远删不到。 */
    if (g_pair_prev_valid) {
        memcpy(addr, g_pair_prev_peer, 6);
        g_pair_prev_valid = 0;
        rt_kprintf("[BTSLOT] slot%d 用暂存地址删 bond: %02x:%02x:%02x:%02x:%02x:%02x\n",
                   g_cur_slot, addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);
        bt_multi_clear_bond_by_addr(addr);
        return;
    }
    if (!bt_slot_get_addr(g_cur_slot, addr)) {
        rt_kprintf("[BTSLOT] slot%d no bond to clear, wait for pairing\n", g_cur_slot);
        return;
    }
    bt_multi_clear_bond_by_addr(addr);
}

/* 真正的删除动作（原 bt_multi_clear_slot_bond 的主体，改成按地址入参） */
static void bt_multi_clear_bond_by_addr(uint8_t *addr)
{
    /* ★★ 11-28 验证实验：去掉"地址也在别的槽就跳过删除"的保护。
     * 目标：确认 connection_manager_delete_bond() 到底是
     *   ① 按 (对端 + 本机身份) 精确删本槽那一条   ← 期望
     *   ② 把该对端的所有条目一起删掉
     * 之前这段保护让删除永远不执行，所以这个问题一直没答案。
     * 现在放行，靠删除前后的 [BOND] 快照判断是 ① 还是 ②。
     * 若结果是 ②，再改成"仅当表满时才删"或换其它策略。 */
    for (int i = 0; i < BT_SLOT_NUM; i++) {
        if (i != g_cur_slot - 1 && g_slot.slot_valid[i] &&
            memcmp(addr, g_slot.slot_addr[i], 6) == 0) {
            rt_kprintf("[BTSLOT] 注意: 该地址也在 slot%d 记录中, 仍按实验放行删除\n", i + 1);
            break;
        }
    }
    /* BLE 侧: 在 bonded 列表中按地址匹配删除(带真实 addr_type) */
    {
        conn_manager_get_bonded_dev_t bonded;
        uint8_t count = connection_manager_get_bonded_devices((uint8_t *)&bonded);
        uint8_t found = 0;
        for (int i = 0; i < count; i++) {
            if (memcmp(addr, bonded.peer_addr[i].addr.addr, 6) == 0) {
                uint8_t ret = connection_manager_delete_bond(bonded.peer_addr[i]);
                rt_kprintf("[BTSLOT] BLE bond deleted (slot%d, ret=%d)\n", g_cur_slot, ret);
                found = 1;
            }
        }
        if (!found)
            rt_kprintf("[BTSLOT] slot%d addr not in BLE bonded list (%d devs)\n",
                       g_cur_slot, count);
    }
    /* ★★★ 11-28 关键修正：**不再在初始化阶段删经典 linkkey**。
     * 现象：恢复"删本槽 bond"后，每次开机都固定崩在
     *   `app addr nvds get 1` 之后 → spp_srv_client_sdp_req 空指针(DACCVIOL 0x68)。
     * 原因：bt_cm_delete_bonded_devs_and_linkkey() 会动经典的 bond/SDP 记录，
     *   而本函数在 BLE 初始化后紧接着执行，正好和 SPP 的 SDP 请求撞车，
     *   把 SDP 客户端句柄清成 NULL。
     * 处理：这里**只删 BLE 侧 bond**；经典 linkkey 留着 —— 它由
     *   ble_app_sync_classic_key() 在开机时从 BLE LTK 重新派生并写回，
     *   效果等价且不碰 SDP 状态。
     * 代价：经典侧会短暂保留旧 key（用错 key 认证失败→重配），可接受。 */
    /* ★★★ 11-28 延迟删除经典 linkkey：
     * 直接在这里删会在 BLE 初始化刚结束时撞上 SPP 的 SDP 请求 → 开机必崩
     * （spp_srv_client_sdp_req 空指针）。改成**栈完全就绪 5 秒后**删除。
     * 为什么仍要删：换址后电脑侧没有新地址对应的经典密钥，旧 key 只会让
     * page 被拒（实测 ACL 建不起来、键盘报文 NOT-READY 全丢）。
     * 删掉后由主机重新配对经典，双方重新协商 linkkey。 */
    memcpy(g_cls_key_del_addr, addr, 6);
    g_cls_key_del_pending = 1;
    rt_kprintf("[BTSLOT] 经典 linkkey 将在 5s 后删除(避开 SDP 初始化)\n");

    bt_slot_clear(g_cur_slot);
}

/* ============================ 状态机 ============================ */

/* 上电读按键电平(active-low, 按下=0): 检测到按下即锁定生效。
 * ⚠️ 不做长时间复读确认 —— 用户"看到 RGB 亮即可松手", 短按(几十 ms)也必须识别;
 * 若用 mdelay 复读, 短按会在复读窗口内已松开 → 判定"无按键" → 走默认槽位,
 * 表现为"松开太快就异常"。上电到此时按键 pin 已配置上拉且电平稳定, 单次读取可靠。 */
static int key_pressed_at_boot(uint32_t pin)
{
    return (rt_pin_read(pin) == 0) ? 1 : 0;
}

void bt_multi_boot_select(void)
{
    uint8_t slot = 0;
    uint8_t mode = BT_MODE_CONNECT;

#ifdef BSP_KEY_C1_PIN
    if (key_pressed_at_boot(BSP_KEY_C1_PIN)) { slot = 1; mode = BT_MODE_CONNECT; }
#endif
#ifdef BSP_KEY_C2_PIN
    if (!slot && key_pressed_at_boot(BSP_KEY_C2_PIN)) { slot = 2; mode = BT_MODE_CONNECT; }
#endif
#ifdef BSP_KEY_C3_PIN
    if (!slot && key_pressed_at_boot(BSP_KEY_C3_PIN)) { slot = 3; mode = BT_MODE_CONNECT; }
#endif
#ifdef BSP_KEY_L1_PIN
    if (!slot && key_pressed_at_boot(BSP_KEY_L1_PIN)) { slot = 1; mode = BT_MODE_PAIR; }
#endif
#ifdef BSP_KEY_L2_PIN
    if (!slot && key_pressed_at_boot(BSP_KEY_L2_PIN)) { slot = 2; mode = BT_MODE_PAIR; }
#endif
#ifdef BSP_KEY_L3_PIN
    if (!slot && key_pressed_at_boot(BSP_KEY_L3_PIN)) { slot = 3; mode = BT_MODE_PAIR; }
#endif

    if (!slot) {
        slot = bt_slot_get_last();
        mode = BT_MODE_CONNECT;
        rt_kprintf("[BTSLOT] boot: no key -> last slot%d (connect)\n", slot);
    } else {
        rt_kprintf("[BTSLOT] boot: slot%d mode=%s\n", slot, mode ? "PAIR" : "CONNECT");
    }

    /* 连接模式但槽位无配对记录: 空槽连接模式 = 待配对(黄闪, 见 start_indication),
     * 接受任何设备(已配对→记录/连接, 新设备→正常配对), 连接成功由
     * on_connected/on_paired 记录到槽位, 自动补齐槽位地址。 */
    {
        uint8_t _a[6];
        if (mode == BT_MODE_CONNECT && !bt_slot_get_addr(slot, _a))
            rt_kprintf("[BTSLOT] slot%d empty: CONNECT accepts any device (connect), will record on connect\n", slot);
    }

    g_cur_slot = slot;
    g_cur_mode = mode;
    g_mode_applied = 0;

    /* ★★ 11-28 配对模式换址（对标多槽键盘的做法）：
     * 老主机手里是"旧地址 + 旧密钥"，地址一换它就根本找不到设备，
     * 不可能再静默抢占槽位；新设备则以新地址完成配对。
     * 时机必须在这里（BLE 身份回调 ble_request_public_address 之前）。
     * 同时清掉本槽的设备记录 —— 旧记录绑的是旧地址，已作废。 */
    if (mode == BT_MODE_PAIR) {
        /* ★★ 11-28 顺序修正：清记录之前先把旧设备地址**暂存**下来。
         * 清 bond 要等 BLE 栈就绪（apply_mode）才能做，那时记录已被清空 ⇒
         * 原来永远走到 "no bond to clear"（实测两次都是这句）。
         * 现在存一份到 g_pair_prev_peer，供 apply_mode 精确删除。 */
        if (g_slot.slot_valid[slot - 1]) {
            memcpy(g_pair_prev_peer, g_slot.slot_addr[slot - 1], 6);
            g_pair_prev_valid = 1;
        }
        /* ★★★ 11-28 定案：配对模式**必须换址**（用户明确要求）。
         * 理由：不换址的话，电脑拿着旧记录会直接连上来，配对模式形同虚设。
         * 换址的代价是"同槽重配会在 bond 表里新增一条"（同对端、新本机身份）；
         * 由 apply_mode 里的"删本槽旧 bond"来抵消 —— 见 bt_multi_clear_slot_bond()。 */
        bt_slot_rotate_self_addr(slot);
        if (g_slot.slot_valid[slot - 1]) {
            memset(g_slot.slot_addr[slot - 1], 0, 6);
            g_slot.slot_valid[slot - 1] = 0;
            bt_slot_save();
            rt_kprintf("[BTSLOT] slot%d 旧设备记录已清除(地址已变, 旧配对作废)\n", slot);
        }
    }

    bt_slot_set_last(slot);
}

uint8_t bt_multi_get_slot(void) { return g_cur_slot; }
uint8_t bt_multi_get_mode(void) { return g_cur_mode; }

/* RGB 已初始化后调用(幂等): 未连接 -> 启动闪烁; 已连接 -> 保持熄灭。
 * 黄闪 = 待配对: 显式 L 键配对模式 + 空槽连接模式(C 键/开机, 无配对信息)。
 * 蓝闪 = 等回连: 连接模式且槽位已有配对设备(只等它回连)。
 * 配对成功后 on_paired 记录地址到槽位, 指示灯转青闪/灭。 */
void bt_multi_start_indication(void)
{
    extern int bt_host_linked(void);

    if (g_ind_suppress) return;   /* 09-16: 待机拆卸中 —— 不要起闪烁定时器(否则 DEEP 进不去) */
    /* ★ 10-31 修"按按键槽位灯就闪"：原判据是 g_hid_env.is_connected(BLE)。
     * 手机只走经典蓝牙，BLE 从不连 ⇒ 每次按键从深睡唤醒都会走
     * ble_app_wakeup_advertise() → bt_multi_start_indication() → 重开"等回连蓝闪"，
     * 用户看到的正是"按一下键槽位灯闪一下"。
     * 改成"任一宿主(BLE/经典)已连就保持熄灭"。 */
    if (bt_host_linked()) {
        bt_multi_led_stop();
        return;
    }
    if (g_blink_slot) return;   /* 已在闪烁 */
    uint8_t addr[6];
    /* 08-29: 空槽连接模式(C 键/开机) = 待配对, 黄闪提示"可配对";
     * 槽位已有配对设备 = 等回连, 蓝闪。 */
    uint8_t pair_like = (g_cur_mode == BT_MODE_PAIR) || !bt_slot_get_addr(g_cur_slot, addr);
    bt_multi_led_start(g_cur_slot, pair_like ? RGB_COLOR_YELLOW : RGB_COLOR_BLUE);
}

/* BLE 栈就绪后调用(ble_hid_do_init 内, 广播前): L 键配对模式 -> 删除该槽位配对 */
void bt_multi_apply_mode(void)
{
    if (g_mode_applied) return;
    g_mode_applied = 1;
    /* 仅显式 L 键配对模式: 清当前槽位 bond(旧设备回连无法认证, 天然防抢配)。
     * C 键连接模式(含空槽)不做任何处理: 空槽也接受已配对设备回连并记录到槽位。 */
    if (g_cur_mode == BT_MODE_PAIR) {
        /* ★★ 11-28 恢复"进配对模式清本槽 bond"（用户要求，且必要）：
         * 换址后旧条目会变成死条目占表位；表只有 3 条，反复重配同一个槽
         * 会把其它槽的条目挤掉（SDK 按 priority 淘汰最旧）。
         * 这里主动清掉**本槽记录的那台设备**的 bond，而不是等淘汰。
         * 用的是官方 API（connection_manager_delete_bond + bt_cm_delete_bonded_devs_and_linkkey），
         * 不碰协议栈内部结构 —— 之前用 memset 直接写 g_bond_info 那版会让开机必崩。 */
        /* ★ 11-28：IRK 改为按槽固定后，同槽重新配对会**原地更新**那条 bond，
         * 不再新增条目 ⇒ 不需要（也不应该）删 bond：
         * 删除按对端地址做，同一台电脑配了别的槽时会把别人的条目一起删掉。
         * 这里只打印表快照，便于观察条目是否稳定。 */
        { extern void bt_dump_bonds(const char *); bt_dump_bonds("PAIR mode entry"); }
        bt_multi_clear_slot_bond();                    /* 删本槽那台设备的 bond */
        /* 经典 linkkey 延迟 5s 删（避开 SDP）；只在本槽确有记录时才需要 */
        if (g_cls_key_del_pending) {
            if (!g_cls_key_del_timer) {
                g_cls_key_del_timer = rt_timer_create("clskdel", cls_key_del_cb, RT_NULL,
                    rt_tick_from_millisecond(5000),
                    RT_TIMER_FLAG_ONE_SHOT | RT_TIMER_FLAG_SOFT_TIMER);
            }
            if (g_cls_key_del_timer) {
                rt_timer_control(g_cls_key_del_timer, RT_TIMER_CTRL_SET_TIME,
                                 &(rt_tick_t){rt_tick_from_millisecond(5000)});
                rt_timer_start(g_cls_key_del_timer);
            }
        }
        { extern void bt_dump_bonds(const char *); bt_dump_bonds("after slot clear"); }
        /* ★ 自动救援：若表被"同一台设备的多条历史记录"占满（早期版本换址新增条目
         * 留下的），新配对会直接 120 失败、设备连不上。这种状态一定是坏的，
         * 直接整体清空，让本次配对能从干净状态开始。 */
        {
            extern conn_manager_pair_dev_t g_bond_info;
            extern void bt_bond_clear_all(void);
            int n = 0, same = 1;
            uint8_t first[6]; memset(first, 0, 6);
            for (int i = 0; i < MAX_PAIR_DEV; i++) {
                if (!g_bond_info.priority[i]) continue;
                const uint8_t *a = g_bond_info.peer_addr[i].addr.addr;
                if (n == 0) memcpy(first, a, 6);
                else if (memcmp(first, a, 6) != 0) same = 0;
                n++;
            }
            if (n >= MAX_PAIR_DEV && same) {
                rt_kprintf("[BOND] 表被同一设备占满(%d 条), 自动清空以恢复\n", n);
                bt_bond_clear_all();
            }
        }
    }
}

/* ---- 事件回调 ---- */

/* 在 SDK bonded 列表中找与 addr 匹配的 identity 地址(比连接事件里的地址更稳)。
 * 找到则复制到 out6 并返回 1, 否则返回 0(调用方回退用连接事件里的地址)。 */
static uint8_t find_identity_addr(const uint8_t *addr, uint8_t *out6)
{
    conn_manager_get_bonded_dev_t bonded;
    uint8_t count = connection_manager_get_bonded_devices((uint8_t *)&bonded);
    for (int i = 0; i < count; i++) {
        if (memcmp(addr, bonded.peer_addr[i].addr.addr, 6) == 0) {
            memcpy(out6, bonded.peer_addr[i].addr.addr, 6);
            return 1;
        }
    }
    return 0;
}

void bt_multi_on_connected(void)
{
    uint8_t addr[6];
    if (!find_identity_addr(g_hid_env.peer_addr, addr))
        memcpy(addr, g_hid_env.peer_addr, 6);
    /* ★★ 11-27 修正：跨槽判断改为"本槽优先"。
     * 背景：需求允许**同一台电脑配多个槽**（电脑端看到多个设备，各自一套配置）。
     * 原逻辑先查"该地址是否在其它槽"，命中就不记录 —— 于是同一台电脑配了槽1、
     * 槽2 之后，回槽1 连接时永远被判定 other_slot，日志打出
     * "(other slot, not recorded)" 这种自相矛盾的提示（槽1 明明有记录，
     * 白名单也正是拿它填的），而且"槽位记录"这条链路再也刷新不了。
     * 新逻辑：
     *   ① 本槽已记录该地址 → 正常，什么都不用做
     *   ② 本槽为空 / 记录的是别的地址 → 再判断是否属于其它槽：
     *        属于 → 不记录（防止"按C2却被蓝牙1设备抢连"污染别的槽）
     *        不属于 → 记录到本槽
     * 注意：on_paired() 仍然**无条件**写入本槽 —— 配对是用户的明确意图，
     * 允许同一台设备进入多个槽；只有"被动连接"才需要这道防污染检查。 */
    uint8_t cur_addr[6];
    int cur_has = (bt_slot_get_addr(g_cur_slot, cur_addr) &&
                   memcmp(cur_addr, addr, 6) == 0);
    uint8_t other_slot = 0;
    const char *note = "";
    if (cur_has) {
        note = " (already recorded)";
    } else {
        for (int i = 0; i < BT_SLOT_NUM; i++) {
            if (i != g_cur_slot - 1 && g_slot.slot_valid[i] &&
                memcmp(addr, g_slot.slot_addr[i], 6) == 0) {
                other_slot = 1;
                break;
            }
        }
        if (other_slot) note = " (belongs to another slot, not recorded)";
        else { bt_slot_set_addr(g_cur_slot, addr); note = " (recorded)"; }
    }
    rt_kprintf("[BTSLOT] connected -> slot%d addr %02x:%02x:%02x:%02x:%02x:%02x%s, LED %s\n",
               g_cur_slot, addr[5], addr[4], addr[3], addr[2], addr[1], addr[0],
               note,
               g_hfp_live ? "off (HFP alive)" : "cyan (wait HFP)");
    /* ★ 11-25 经典被配置关闭时**不要闪青**。
     * 青闪的语义是"已连上、等 HFP 建好"；经典关了 HFP 永远不会来，
     * 这盏灯就会一直闪到关机（用户实测："蓝牙关了但青灯还在闪"）。
     * 直接停灯 = 已连接且无需等待。 */
    { extern uint8_t key_config_bt_flags(void);
      if (!(key_config_bt_flags() & KEY_CONFIG_BT_CLASSIC_EN)) {
          bt_multi_led_stop();
          rt_kprintf("[BTSLOT] classic disabled -> LED off (no HFP to wait)\n");
          return;
      } }
    if (g_hfp_live)
        bt_multi_led_stop();   /* HFP 仍存活(语音中 BLE 断连重连): 保持关灯, 不闪青 */
    else
        bt_multi_led_start(g_cur_slot, RGB_COLOR_CYAN);   /* HFP 未建立: 青闪 */
}

void bt_multi_on_paired(void)
{
    uint8_t addr[6];
    if (!find_identity_addr(g_hid_env.peer_addr, addr))
        memcpy(addr, g_hid_env.peer_addr, 6);
    bt_slot_set_addr(g_cur_slot, addr);
    rt_kprintf("[BTSLOT] paired -> slot%d addr %02x:%02x:%02x:%02x:%02x:%02x, LED %s\n",
               g_cur_slot, addr[5], addr[4], addr[3], addr[2], addr[1], addr[0],
               g_hfp_live ? "off (HFP alive)" : "cyan (wait HFP)");
    /* ★ 11-25 与 bt_multi_on_connected 保持一致：经典被配置关闭时不要闪青。
     * 青闪的语义是"配对完成、等 HFP 建好"；经典关了 HFP 永远不会来，
     * 这盏灯会一直闪到关机（用户实测：配对模式下青灯常闪不停）。 */
    { extern uint8_t key_config_bt_flags(void);
      if (!(key_config_bt_flags() & KEY_CONFIG_BT_CLASSIC_EN)) {
          bt_multi_led_stop();
          rt_kprintf("[BTSLOT] classic disabled -> LED off after pairing\n");
          goto paired_led_done;
      } }
    if (g_hfp_live)
        bt_multi_led_stop();
    else
        bt_multi_led_start(g_cur_slot, RGB_COLOR_CYAN);   /* 配对完成, HFP 未建立: 青闪 */
paired_led_done:
    /* 配对完成 = 该槽位已就绪, 之后按连接模式对待:
     * 断连回连窗口只允许刚配对的新设备(白名单), 防其它已配对设备抢连 */
    g_cur_mode = BT_MODE_CONNECT;
    /* ★ 11-24 配对窗口关闭 ⇒ 收回"可被发现"，回到只允许寻呼的省电状态。
     * （配对期间是开着的，见 ble_app.c 里 stack-ready 时的判断） */
    { extern uint8_t bt_interface_set_scan_mode(uint8_t, uint8_t);
      bt_interface_set_scan_mode(0, 1); }
}

/* BR/EDR(经典蓝牙)配对+连接成功(HFP connected): 手机等设备走 BR/EDR 配对,
 * 不主动连 BLE —— 若仍按 BLE 配对窗口等待, 窗口永不结束, 已配对设备(蓝牙1/2)
 * 的 BLE 抢连会一直刷屏。这里记录地址并结束配对窗口, 让广播转白名单模式。 */
void bt_multi_on_br_paired(const uint8_t *addr)
{
    if (!bt_multi_is_pairing_window()) return;

    /* ★★ 11-27 与 bt_multi_on_connected 同一原则：**本槽已记录时先认账**。
     * 需求允许同一台电脑配多个槽；此时"该地址也出现在别的槽"是正常状态，
     * 不能因此把本次连接判成"别人的设备回连"而拒绝记录/不结束窗口。 */
    {
        uint8_t cur[6];
        if (bt_slot_get_addr(g_cur_slot, cur) && memcmp(cur, addr, 6) == 0) {
            rt_kprintf("[BTSLOT] BR/EDR conn from slot%d's own dev (already recorded), "
                       "pair window end\n", g_cur_slot);
            g_cur_mode = BT_MODE_CONNECT;
            bt_slot_save();
            bt_multi_led_stop();
            { extern uint8_t bt_interface_set_scan_mode(uint8_t, uint8_t);
              bt_interface_set_scan_mode(0, 1); }
            return;
        }
    }

    /* 防跨槽污染: 来连设备若已属于其他槽位(已配对设备自动回连, 非新设备),
     * 不记录、不结束窗口 —— 否则手机(配过 BR/EDR 自动回连)会顶替当前空槽,
     * 导致该槽位对应的电脑 HFP 异常(实测: C1/C2 连接不对, 即手机地址被误记)。 */
    for (int i = 0; i < BT_SLOT_NUM; i++) {
        if (i != g_cur_slot - 1 && g_slot.slot_valid[i] &&
            memcmp(addr, g_slot.slot_addr[i], 6) == 0) {
            rt_kprintf("[BTSLOT] BR/EDR conn from slot%d paired dev (not new), "
                       "slot%d NOT recorded\n", i + 1, g_cur_slot);
            return;
        }
    }
    /* 拒绝 BLE 已配对设备(bond 列表)回连 —— 双模同地址, 覆盖手机等已配对设备;
     * L 键重新配对场景: clear_slot_bond 已删该设备 bond, 校验自然放行。 */
    {
        conn_manager_get_bonded_dev_t bonded;
        uint8_t count = connection_manager_get_bonded_devices((uint8_t *)&bonded);
        for (int i = 0; i < count; i++) {
            if (memcmp(addr, bonded.peer_addr[i].addr.addr, 6) == 0) {
                rt_kprintf("[BTSLOT] BR/EDR conn from BLE-bonded dev (not new), "
                           "slot%d NOT recorded\n", g_cur_slot);
                return;
            }
        }
    }
    bt_slot_set_addr(g_cur_slot, addr);
    g_cur_mode = BT_MODE_CONNECT;
    bt_slot_save();
    rt_kprintf("[BTSLOT] BR/EDR paired -> slot%d addr %02x:%02x:%02x:%02x:%02x:%02x, PAIR window end\n",
               g_cur_slot, addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);
    bt_multi_led_stop();
    /* ★ 11-24 经典配对完成 ⇒ 同样收回"可被发现" */
    g_cur_mode = BT_MODE_CONNECT;
    { extern uint8_t bt_interface_set_scan_mode(uint8_t, uint8_t);
      bt_interface_set_scan_mode(0, 1); }
    /* ★ 11-23: 首次配对是手机发起(dir=1) ⇒ 设备为 slave ⇒ 空中鼠标拖慢。
     * 主动断一次让 auto-connect 以 master 身份重连，与"重启设备"等效。 */
    { extern void ble_app_post_pair_role_fix(void); ble_app_post_pair_role_fix(); }
}

/* ---- 连接过滤(白名单) ---- */

uint8_t bt_multi_white_list_enabled(void)
{
    uint8_t addr[6];
    if (g_cur_mode == BT_MODE_PAIR) return 0;      /* 配对模式: 任何设备都能连 */
    return bt_slot_get_addr(g_cur_slot, addr);     /* 连接模式且槽位有配对设备: 只允许它 */
}

void bt_multi_set_white_list(void)
{
    uint8_t addr[6];
    if (!bt_slot_get_addr(g_cur_slot, addr)) {
        rt_kprintf("[BTSLOT] whitelist: slot%d empty, no filter\n", g_cur_slot);
        return;
    }
    /* 优先从 SDK bonded 列表取完整条目(含 addr_type, RPA 设备也能正确匹配) */
    ble_gap_addr_t target;
    uint8_t found = 0;
    {
        conn_manager_get_bonded_dev_t bonded;
        uint8_t count = connection_manager_get_bonded_devices((uint8_t *)&bonded);
        for (int i = 0; i < count; i++) {
            if (memcmp(addr, bonded.peer_addr[i].addr.addr, 6) == 0) {
                target = bonded.peer_addr[i];
                found = 1;
                break;
            }
        }
    }
    if (!found) {
        memcpy(target.addr.addr, addr, 6);
        target.addr_type = 0;
    }
    ble_gap_white_list_t *wl = rt_malloc(sizeof(ble_gap_white_list_t) + sizeof(ble_gap_addr_t));
    if (!wl) return;
    wl->size = 1;
    wl->addr[0] = target;
    uint8_t ret = ble_gap_set_white_list(wl);
    rt_free(wl);
    rt_kprintf("[BTSLOT] whitelist set: slot%d %02x:%02x:%02x:%02x:%02x:%02x (ret=%d)\n",
               g_cur_slot, addr[5], addr[4], addr[3], addr[2], addr[1], addr[0], ret);
}



void bt_multi_on_disconnected(void)
{
    extern int bt_host_linked(void);
    /* 断连后设备会重新广播等待回连/配对 -> 恢复槽位指示
     * (连接模式蓝闪 / 配对模式黄闪)。
     * 10-31: 判据改为"任一宿主是否还连着" —— BLE 断了但经典(手机)还在时不该闪。 */
    if (!bt_host_linked()) {
        bt_multi_start_indication();
    } else {
        bt_multi_led_stop();
    }
}
