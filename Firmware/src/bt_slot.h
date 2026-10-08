#ifndef __BT_SLOT_H__
#define __BT_SLOT_H__

#include <stdint.h>

/* ============================================================================
 * 三设备(蓝牙1/2/3)切换存储 + 多设备管理
 *
 * Flash 布局: SDK KVDB_BLE(0x12624000~0x12628000) -> KEY_CONFIG(0x12628000)
 * -> BT_SLOT(0x12629000, 独立 4KB 页)。三槽位各存一台已配对设备的 BLE 地址
 * (经典蓝牙与 BLE 共享同一 BD_ADDR, 删除配对时两栈一起删), 外加"最近一次
 * 被选择的蓝牙槽位"用于上电无按键时恢复。
 * ============================================================================ */

#define BT_SLOT_NUM         3
#define BT_SLOT_MAGIC       0x42534C54   /* "BSLT" */
#define BT_SLOT_VERSION     3             /* v2(08-26): rand_base 随机基址; v3: TRNG 修复后强制重生成 */
#define BT_SLOT_FLASH_ADDR  0x12629000   /* 紧跟 KEY_CONFIG(0x12628000) 之后独立 4KB 页 */

/* 上电按键选择的蓝牙模式 */
#define BT_MODE_CONNECT     0   /* C 键: 连接模式 —— 广播等已配对设备回连(蓝闪) */
#define BT_MODE_PAIR        1   /* L 键: 配对模式 —— 删除该槽位配对, 广播重新配对(黄闪) */

typedef struct {
    uint32_t magic;
    uint8_t  version;
    uint8_t  last_slot;             /* 最近一次选择的蓝牙槽位 1~3 */
    uint8_t  slot_valid[BT_SLOT_NUM]; /* 槽位是否有已配对设备地址 */
    uint8_t  rand_valid;            /* rand_base 随机基址是否已生成(0=未生成) */
    uint8_t  slot_addr[BT_SLOT_NUM][6]; /* 每槽位配对设备 BLE/BR-EDR 地址 */
    uint8_t  rand_base[6];          /* 每设备唯一随机静态地址基址(首次生成后固定,
                                     * 每槽 = rand_base + 槽偏移, 重启不变) */
} bt_slot_storage_t;

/* ---- 存储层 ---- */
void bt_slot_init(void);            /* 上电调用: 读 Flash, 无效则初始化 */
void bt_slot_save(void);
uint8_t bt_slot_get_last(void);     /* 返回 1~3 */
void bt_slot_set_last(uint8_t slot);
uint8_t bt_slot_get_addr(uint8_t slot, uint8_t *out6);  /* 返回 1=有效 0=空 */
void bt_slot_set_addr(uint8_t slot, const uint8_t *addr6);
void bt_slot_clear(uint8_t slot);
uint8_t bt_slot_get_rand_base(uint8_t *out6);   /* 返回 1=已生成 0=未生成 */
void bt_slot_set_rand_base(const uint8_t *base6);

/* ---- 多设备切换状态机 ---- */
/* 上电调用(需按键 pin 已初始化): 读 C1-C3/L1-L3 电平决定初始槽位+模式;
 * 无按键 -> 用最近一次槽位, 连接模式。只做决策, 不碰 BLE/RGB(此时 RGB 未初始化)。 */
void bt_multi_boot_select(void);

/* RGB 已初始化后调用(幂等): 按当前模式启动对应 RGB 闪烁指示
 * (连接=蓝闪 / 配对=黄闪); 当前已连接则直接熄灭。 */
void bt_multi_start_indication(void);

/* BLE 栈就绪后调用(ble_hid_do_init 内, 广播前, 幂等):
 * 配对模式 -> 删除当前槽位 BLE+BR/EDR 配对信息。 */
void bt_multi_apply_mode(void);

/* ---- 事件回调(ble_app.c 调用) ---- */
void bt_multi_on_connected(void);     /* BLE 连接成功: 记录地址到当前槽位 + 停闪熄灭 */
void bt_multi_on_disconnected(void);  /* BLE 断开: 若等待回连则恢复蓝闪指示 */
void bt_multi_on_paired(void);        /* BLE 配对成功(CM_PAIRING_SUCCEED): 记录地址 + 停闪熄灭 */
/* BR/EDR(经典蓝牙)配对+连接成功(HFP connected, 手机等走经典蓝牙的设备):
 * 记录地址到当前槽位 + 结束配对窗口(切连接模式) + 停指示。之后广播按连接模式
 * 带白名单, 已配对设备(蓝牙1/2)的 BLE 抢连被控制器拒, 不再反复骚扰。 */
void bt_multi_on_br_paired(const uint8_t *addr);

uint8_t bt_multi_get_slot(void);      /* 当前槽位 1~3 */
uint8_t bt_multi_get_mode(void);      /* BT_MODE_CONNECT / BT_MODE_PAIR */
void bt_multi_led_stop_all(void);     /* 停止所有闪烁并熄灭 RGB */
/* 充电指示(08-29 收敛到蓝灯 LED1): state 0=无充电 1=充电中(LED1 蓝常亮) 2=充满(LED1 灭)。
 * 由 main.c 的 1s 轮询按内置充电器 detect/full 状态调用; 独立 GPIO LED, 与槽位 RGB 背光共存。
 * LED2(黄)/LED3(绿) 自 08-29 起不再做充电指示, 已交还 ble_led_service 的 app 状态灯。 */
void bt_multi_set_charge(uint8_t state);
/* 充电指示是否激活(0=无充电)。08-31 起 ble_led_service 不再用它让位 —— LED1 与
 * app 状态灯 LED2/3/4 无重叠, 插电时 app 推送的状态灯照常显示。保留供调试/其它判断。 */
uint8_t bt_multi_charge_active(void);
/* 按当前充电状态恢复蓝灯(LED1)充电指示(08-29): 开机电量显示等临时占用 LED1
 * 结束后调用, 避免把充电常亮误灭; 鼠标故障蓝闪优先, 闪烁期间不动。 */
void bt_multi_restore_charge_led(void);
/* HFP 状态通知(ble_app.c 在 HFP connected/disconnected 时调用):
 * hfp_on=1(建立) -> 熄灭模式指示; hfp_on=0(断开, BLE 仍连) -> 青色闪(等 HFP)。
 * 用于 BLE 重连时判断"HFP 是否仍存活"(语音中 BLE 断连重连场景)。 */
void bt_multi_on_hfp_state(uint8_t hfp_on);
/* 鼠标故障蓝色闪烁(08-29 由 LED4 红闪改为 LED1 蓝闪): 空中鼠标数据流停摆(IMU 看门狗
 * 判定)时 LED1(PA5)蓝闪提示; err=0 时熄灭并按 g_charge_state 还原充电常亮。
 * LED1 与 app 状态灯(LED2/3/4)互不干扰。 */
void bt_multi_set_mouse_error(uint8_t err);
/* 鼠标故障蓝闪是否激活。08-31 起 ble_led_service 不再用它让位(同 bt_multi_charge_active)。 */
uint8_t bt_multi_mouse_error_active(void);

/* ---- 连接过滤(白名单): 选蓝牙 n 时只允许蓝牙 n 的设备连接 ---- */
/* 是否需要白名单过滤: 连接模式且当前槽位已有配对地址 -> 1(只允许该设备连);
 * 配对模式或槽位为空 -> 0(接受任何设备, 等配对)。广播启动时据此设置
 * config.white_list_enable。 */
uint8_t bt_multi_white_list_enabled(void);
/* 广播启动前(SIBLES_ADV_EVT_REQUEST_SET_WHITE_LIST 事件)调用:
 * 把当前槽位设备地址设为白名单(从 SDK bonded 列表取完整条目含 addr_type)。 */
void bt_multi_set_white_list(void);

/* ---- 配对窗口 ---- */
/* 是否处于"配对窗口": 仅 L 键配对模式为真(用于 LED 黄闪 / 配对期关 BR/EDR 等)。
 * ⚠️ 方案B(每槽独立 BD_ADDR)已整体移除旧的"配对模式防抢连拒绝列表"
 * (bt_multi_pair_reject_build / bt_multi_pair_reject 已删除): 每槽地址不同,
 * 其他地址下的设备看不见本槽广播、不存在抢配; 拒绝列表反而挡住"同一台电脑
 * 配到多个槽"。当前槽位旧设备由 clear_slot_bond 删 bond 天然防抢配。 */
uint8_t bt_multi_is_pairing_window(void);

#endif /* __BT_SLOT_H__ */
