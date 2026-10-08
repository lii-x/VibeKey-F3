/*
 * SPDX-FileCopyrightText: 2019-2025 SiFli Technologies(Nanjing) Co., Ltd
 *
 * SPDX-License-Identifier: Apache-2.0
 */
#include "usbd_core.h"
#include "usbd_cdc_acm.h"
#include "rtthread.h"
#include "board.h"
#include "drv_flash.h"
#include "key_config.h"
#include "ws2812b.h"
#include "ble_led_service.h"
#include "ble_app.h"   /* app_get_battery_percent / app_battery_valid（电量经 CDC INFO 上报） */
#include "bt_slot.h"   /* bt_multi_get_slot: 当前蓝牙槽位 1~3（CDC INFO 上报） */

#define CDC_IN_EP  0x85
#define CDC_OUT_EP 0x02
#define CDC_INT_EP 0x86
#define USBD_VID   0x38f4
#define USBD_PID   0xF3A0  /* VibeKey-F3 专用 PID（开发占位值，量产请向 SiFli 申请正式分配的 PID）*/
#define USBD_MAX_POWER 100
#define USB_CONFIG_SIZE (9 + CDC_ACM_DESCRIPTOR_LEN)
#ifdef CONFIG_USB_HS
    #define CDC_MAX_MPS 512
#else
    #define CDC_MAX_MPS 64
#endif

#define OTA_MAGIC   0x4F544100
#define CONF_MAGIC  0x434F4E46
#define OTA_TARGET_ADDR 0x12320000  /* = DFU_DOWNLOAD_REGION 起点(ptab: 0x12000000 + 0x320000)，8MB Flash 重排后地址 */
#define OTA_REGION_END  0x12620000  /* = DFU_DOWNLOAD_REGION 终点(ptab: 0x12320000 + 0x300000); OTA 镜像(含 8 字节头)不得越过, 否则擦写会抹掉 KVDB 区 */
#define OTA_BODY_MAX_SIZE (2 * 1024 * 1024)  /* 与 bootloader(main.c:325) 对 fw_size 的硬上限对齐; 超过则 bootloader 直接忽略不安装; DFU_DOWNLOAD 区 3MB 可容纳 */

#define CONF_CMD_READ  0x01
#define CONF_CMD_WRITE 0x02
#define CONF_CMD_RESET 0x03
#define CONF_CMD_LED   0x04
#define CONF_CMD_INFO  0x05   /* 设备信息查询：返回 "VibeKey-F3|<版本>|<电量>|<槽位>\n" */
#define CONF_CMD_RGB_APPLY 0x06 /* 单键 RGB 实时预览：payload = idx,enabled,r,g,b,brightness */

/* 设备身份信息（App 端握手据此确认是 VibeKey-F3）。版本号随发布 bump。 */
#define VIBEKEY_MODEL      "VibeKey-F3"
#define VIBEKEY_FW_VERSION "1.1.3"

#ifdef CONFIG_USBDEV_ADVANCE_DESC

static const uint8_t device_descriptor[] =
{
    USB_DEVICE_DESCRIPTOR_INIT(USB_2_0, 0xEF, 0x02, 0x01, USBD_VID, USBD_PID, 0x0100, 0x01)
};
static const uint8_t config_descriptor[] =
{
    USB_CONFIG_DESCRIPTOR_INIT(USB_CONFIG_SIZE, 0x02, 0x01, USB_CONFIG_BUS_POWERED, USBD_MAX_POWER),
    CDC_ACM_DESCRIPTOR_INIT(0x00, CDC_INT_EP, CDC_OUT_EP, CDC_IN_EP, CDC_MAX_MPS, 0x02)
};
static const uint8_t device_quality_descriptor[] =
{
    0x0a, USB_DESCRIPTOR_TYPE_DEVICE_QUALIFIER,
    0x00, 0x02, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00,
};
static const char *string_descriptors[] =
{
    (const char[]){ 0x09, 0x04 },
    "SiFli", "VibeKey-F3", "2022123456",
};
static const uint8_t *device_descriptor_callback(uint8_t s) { return device_descriptor; }
static const uint8_t *config_descriptor_callback(uint8_t s) { return config_descriptor; }
static const uint8_t *device_quality_descriptor_callback(uint8_t s) { return device_quality_descriptor; }
static const char *string_descriptor_callback(uint8_t s, uint8_t i)
{
    if (i >= (sizeof(string_descriptors) / sizeof(char *))) return NULL;
    return string_descriptors[i];
}
const struct usb_descriptor cdc_descriptor =
{
    .device_descriptor_callback = device_descriptor_callback,
    .config_descriptor_callback = config_descriptor_callback,
    .device_quality_descriptor_callback = device_quality_descriptor_callback,
    .string_descriptor_callback = string_descriptor_callback
};
#endif

USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX uint8_t read_buffer[2048];
volatile bool ep_tx_busy_flag = false;

/* USB 总线/寄存器句柄（cdc_acm_enable/disable 与 conf 回包共用），提前声明使全文件可见 */
static uint8_t g_busid;
static uintptr_t g_reg_base;

static uint8_t flash_queue[131072];
static volatile uint32_t q_head, q_tail, q_count;
static volatile uint32_t ota_total_size, ota_received;
static volatile uint8_t ota_started, ota_complete;
static rt_sem_t q_sem;
static uint8_t ota_header[8];
static volatile uint8_t ota_erase_done;
static volatile uint8_t ep_read_paused;
/* 型号校验：OTA 收尾激活前，从新固件体流式匹配本机型号标识串。
 * 不匹配（选错固件文件）则拒绝写 magic 激活，直接 reboot 回旧固件，避免变砖。
 * 采用流式状态机，跨数据块边界也能正确匹配，无需回读 Flash。 */
/* 固件专属标识: 仅本设备固件镜像的 rodata 含此串; 普通文件/其他板子固件不含,
 * 用于 OTA 收尾校验, 防止选错文件变砖。切勿改成常见字符串(如 "VibeKey-F3")。 */
#define OTA_MODEL_TAG       "VIBEKEYF3-FWIMG-7F3A9C21"
#define OTA_MODEL_TAG_LEN   24
static volatile int    ota_model_state;
static volatile uint8_t ota_model_found;
static uint32_t q_count_peak;   /* 诊断: 接收队列峰值, 用于判断是否溢出覆盖了未读数据 */
static volatile int    ota_model_best;        /* 诊断: 流式扫描中达到的最长部分匹配长度 */
static volatile uint32_t ota_model_best_off;  /* 诊断: 该最长匹配在固件流中的起始偏移 */
static uint8_t ep_busid;
static volatile uint8_t ota_stop;
static volatile uint8_t conf_busy;

/* OTA 端到端 CRC32(IEEE 802.3, zlib 兼容): 覆盖固件体, 尾部 4 字节为发送方附带的 CRC。
 * 设备端收齐后校验, bootloader 安装前也校验, 防止坏文件被装成砖。 */
static volatile uint32_t ota_crc_running;
static uint8_t ota_crc_trailer[4];

/* CRC-32 (poly 0xEDB88320, init 0xFFFFFFFF, final XOR 0xFFFFFFFF), 与 Python zlib.crc32 完全一致 */
static uint32_t ota_crc32_update(uint32_t crc, const uint8_t *buf, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++)
    {
        crc ^= buf[i];
        for (int k = 0; k < 8; k++)
            crc = (crc & 1) ? (0xEDB88320U ^ (crc >> 1)) : (crc >> 1);
    }
    return crc;
}

/* RGB 实时预览：usbd_cdc_acm_bulk_out 跑在 ISR 上下文，而 WS2812B 驱动
 * (rgb_led_show -> rt_device_control) 内部会取互斥量 rgb_drv，ISR 里取互斥量
 * 会触发 rt_mutex_take 断言并 fatal error。故所有 RGB 下发必须转到线程上下文。
 * 这里用消息队列把请求从 ISR 投到专用线程 rgbapp 处理。 */
#define RGB_REQ_MQ_SIZE 8
static struct rt_messagequeue *rgb_mq;

typedef struct {
    uint8_t type;   /* 0 = 单键实时预览; 1 = 按当前配置全部应用 */
    uint8_t idx;
    uint8_t en;
    uint8_t r;
    uint8_t g;
    uint8_t b;
    uint8_t bri;
} rgb_req_t;

/* ---- CONF 命令处理线程 ----
 * 原实现在 USB ISR(bulk_out) 内同步处理全部 CONF 命令，其中 WRITE/RESET 调用
 * key_config_save()->rt_flash_erase/write（内部取互斥量），在 ISR 中调用会触发
 * rt_mutex_take 断言/fatal error 并破坏内核状态；且 ISR 内直接 usbd_ep_start_write
 * 回包对 MUSB 不安全，源 buffer(g_key_config/栈数组)在 cacheable RAM 时 DMA 还会
 * 读错或触发总线错误，表现为“读取/写入后 USB 整个用不了”。
 * 现改为：ISR 仅把 OUT 包拷贝投递到 conf_mq，由本线程统一处理（含落盘与回包）。
 * 回包统一走 nocache 缓冲 conf_tx_buf，保证 HS 下 cache 一致性且 DMA 生命周期安全。
 * 09-03 (BLE 改建键配置)：BLE GATT 0xFF03 回调收到的命令帧【同样】投进本消息
 * 队列 —— USB 与 BLE 共用 conf_task 的解析/落盘逻辑, 回包按 src 路由
 * (USB→CDC IN 端点; BLE→0xFF04 notify), 保证双通道行为一致。*/
#define CONF_PKT_MAX   336   /* 10-05: 320→336 —— EC 按压滚动追加后 WRITE 帧
                               * = 8 + 315B payload = 323B, 320 会截帧(按压滚动块落垃圾)。
                               * 09-07 的 320 = 8 + 297B = 305B(EC 按下模式+三手势);
                               * 288 时代 = L1/L2/L3 模式+手势 (8+269B=277B)。 */
/* 命令消息来源: USB CDC bulk_out(ISR) / BLE GATT 0xFF03 回调(协议栈线程) */
#define CONF_SRC_USB    0
#define CONF_SRC_BLE    1
typedef struct {
    uint32_t len;
    uint8_t  src;          /* CONF_SRC_USB / CONF_SRC_BLE */
    uint8_t  conn_idx;     /* BLE 连接索引(src=BLE 时有效, 回包 notify 目标) */
    uint8_t  buf[CONF_PKT_MAX];
} conf_pkt_t;

static struct rt_messagequeue *conf_mq;
/* 09-05: 64→128 —— READ 回包 = sizeof(key_config_storage_t) 由 52 增至 68
 * (L2/L3 尾部追加), 原 64B 缓冲会溢出; 余量留给后续字段扩展。
 * 09-05: 128→192 —— READ 回包 104→158(C 键三手势尾部追加 6×9B)。
 * 09-07: 192→288 —— READ 回包 192→276(L1/L2/L3 模式+三手势尾部追加 9×9B+3B)。
 * 09-07: 288→320 —— READ 回包 276→304(EC 按下模式+三手势尾部追加 3×9B+1B)。
 * 10-05: 320→336 —— READ 回包 304→324(EC 按压滚动尾部追加 2×9B), 320 会溢出。 */
USB_NOCACHE_RAM_SECTION USB_MEM_ALIGNX static uint8_t conf_tx_buf[336];

/* 等待上一次 IN 传输结束，避免覆盖正在 DMA 的缓冲或端点状态机错乱 */
static void conf_in_write(const uint8_t *data, uint32_t len)
{
    while (ep_tx_busy_flag)
        rt_thread_mdelay(1);
    usbd_ep_start_write(g_busid, CDC_IN_EP, data, len);
}

/* 回包路由(09-03): USB → CDC IN 端点; BLE → 0xFF04 notify。
 * conf_tx_buf 由 conf_task 单线程串行使用, 不存在并发写。 */
static void conf_send_reply(const conf_pkt_t *pkt, const uint8_t *data, uint16_t len)
{
    if (pkt->src == CONF_SRC_BLE)
        ble_led_cfg_reply(pkt->conn_idx, data, len);
    else
        conf_in_write(data, len);
}

/* 状态回包: USB 保持历史 8B 帧协议(首字节=code); BLE 只发 1B, 干净且省空口。 */
static void conf_status_reply(const conf_pkt_t *pkt, uint8_t code)
{
    conf_tx_buf[0] = code;
    if (pkt->src == CONF_SRC_BLE)
        conf_send_reply(pkt, conf_tx_buf, 1);
    else
        conf_send_reply(pkt, conf_tx_buf, 8);
}

static void conf_task_entry(void *param)
{
    conf_pkt_t pkt;
    while (1)
    {
        if (rt_mq_recv(conf_mq, &pkt, sizeof(pkt), RT_WAITING_FOREVER) == RT_EOK)
        {
            uint32_t magic = *(uint32_t *)&pkt.buf[0];
            if (magic != CONF_MAGIC)
                continue;
            uint8_t cmd = pkt.buf[4];
            uint16_t data_len = *(uint16_t *)&pkt.buf[6];
            /* 08-27: 每槽独立键配置。slot 复用 buf[5](原恒 0): 0~2=槽1~3,
             * 越界按槽1处理。READ/WRITE/RESET 均按指定槽操作, 上位机可改任意
             * 槽(运行时无需切槽/重启)。旧上位机(buf[5]=0)默认操作槽1, 兼容。 */
            uint8_t slot = pkt.buf[5];

            if (cmd == CONF_CMD_READ)
            {
                /* 09-03: 回包长度 = sizeof(key_config_storage_t), 加摇一摇字段后
                 * 由 40B 自动变 52B。旧上位机按 40B 读 —— 只是读不到新字段, 安全;
                 * 多出的 12B 残留在串口缓冲, 由上位机 open_port() 的下次清理丢弃。
                 * ⚠️ 改结构体布局时必须同步 key_config.c 里的 _Static_assert。 */
                key_config_storage_t *cfg = key_config_get_slot(slot);
                memcpy(conf_tx_buf, cfg, sizeof(key_config_storage_t));
                conf_send_reply(&pkt, conf_tx_buf, sizeof(key_config_storage_t));
            }
            else if (cmd == CONF_CMD_WRITE && data_len >= (2 + KEY_CONFIG_NUM_KEYS * sizeof(key_config_t)))
            {
                key_config_storage_t *cfg = key_config_get_slot(slot);
                cfg->air_mouse_mode  = pkt.buf[8];
                cfg->air_mouse_speed = pkt.buf[9];
                memcpy(cfg->keys, pkt.buf + 10, KEY_CONFIG_NUM_KEYS * sizeof(key_config_t));
                /* 08-27: payload 尾部 2 字节 sleep_min(LE16)。旧上位机(payload=29B,
                 * 无此字段)写入时不覆盖当前值, 保持兼容。新格式 payload=31B。
                 * ⚠️ 用 memcpy 读(不能用 *(uint16_t*)&pkt.buf[37]: buf 在结构体 offset 4,
                 * +37=奇数地址, Cortex-M 非对齐访问可能读错值)。 */
                if (data_len >= (2 + KEY_CONFIG_NUM_KEYS * sizeof(key_config_t) + 2))
                {
                    uint16_t sm;
                    memcpy(&sm, pkt.buf + 10 + KEY_CONFIG_NUM_KEYS * sizeof(key_config_t), sizeof(sm));
                    if (sm == 0 || (sm >= KEY_CONFIG_SLEEP_MIN_DEFAULT && sm <= KEY_CONFIG_SLEEP_MIN_MAX))
                        cfg->sleep_min = sm;   /* 0=永不; 45~240 合法; 其余非法值忽略 */
                    else if (sm == 0xFFFF)
                        cfg->sleep_min = KEY_CONFIG_SLEEP_MIN_DEFAULT;
                }
                /* 09-01: 鼠标方向(1 字节)紧跟 sleep_min 之后, 即 buf[39]。
                 * payload 布局: [8]=mode [9]=speed [10..36]=keys(27B)
                 *               [37..38]=sleep_min(LE16) [39]=dir
                 * 旧上位机(payload=31B, 无 dir)写入时不覆盖当前值, 保持兼容。
                 * 新格式 payload=32B。dir 语义(09-01): 0=0° 1=90° 2=180° 3=270° 旋转。
                 * 非法值(>3)忽略, 不改配置。 */
                if (data_len >= (2 + KEY_CONFIG_NUM_KEYS * sizeof(key_config_t) + 3))
                {
                    uint8_t dir = pkt.buf[10 + KEY_CONFIG_NUM_KEYS * sizeof(key_config_t) + 2];
                    if (dir <= AIR_MOUSE_DIR_270)
                        cfg->air_mouse_dir = dir;   /* 0~3 合法; 其余忽略 */
                }
                /* 09-03: 摇一摇三字段, 紧跟 dir 之后。
                 * ⚠️ payload 是【紧凑布局、无 padding】, 偏移与结构体内存布局【不同】:
                 *    结构体(READ 回包): dir@38 en@39 sens@40 key@41..49 (keys[3]后有1B padding)
                 *    payload (WRITE)  : dir@31 en@32 sens@33 key@34..42 (紧凑, 无 padding)
                 *    buf 索引 = payload 索引 + 8 → buf[39]=dir buf[40]=en buf[41]=sens
                 *                                  buf[42..50]=key
                 *    新格式 payload 长度 = 43。旧上位机(payload=32B, 无摇一摇字段)
                 *    写入时不覆盖当前值, 保持兼容。
                 * ⚠️ 三个字段都做合法性校验, 非法值一律【忽略】(保留现有值)而不是
                 *    写入后靠 getter 兜底 —— 上位机改造期间可能短暂发来 0xFF,
                 *    写入即污染 flash, 而 getter 兜底只在内存里兜, 重启后仍读到脏值。 */
                if (data_len >= (2 + KEY_CONFIG_NUM_KEYS * sizeof(key_config_t) + 3 + 11))
                {
                    uint8_t  en   = pkt.buf[10 + KEY_CONFIG_NUM_KEYS * sizeof(key_config_t) + 3];
                    uint8_t  sens = pkt.buf[10 + KEY_CONFIG_NUM_KEYS * sizeof(key_config_t) + 4];
                    uint8_t *kp   = &pkt.buf[10 + KEY_CONFIG_NUM_KEYS * sizeof(key_config_t) + 5];
                    if (en == KEY_CONFIG_SHAKE_OFF || en == KEY_CONFIG_SHAKE_ON)
                        cfg->shake_enabled = en;
                    if (sens <= SHAKE_SENS_STRONG)
                        cfg->shake_sens = sens;
                    /* 快捷键: action_type 只认 NONE/键盘/多媒体。鼠标动作本功能
                     * 不支持(摇一摇发单击意义不大), 0xFF 等脏值整块忽略。 */
                    if (kp[0] == KEY_ACTION_NONE || kp[0] == KEY_ACTION_KEYBOARD ||
                        kp[0] == KEY_ACTION_MULTIMEDIA)
                    {
                        memcpy(&cfg->shake_key, kp, sizeof(key_config_t));
                    }
                }
                /* 09-05: L2/L3 侧键快捷键 —— 尾部追加块(payload 紧凑布局):
                 *   buf[51..59]=L2  buf[60..68]=L3 (payload[43..51]/[52..60], 索引+8)
                 * 偏移 = 10 + 27(keys) + 2(sleep) + 1(dir) + 1(en) + 1(sens) + 9(shake_key)。
                 * 新 payload 长度 = 61。旧上位机(43B)不带此块 -> 不进入分支, L2/L3
                 * 保持现值(与摇一摇字段的兼容策略一致, 双向安全)。
                 * 校验(逐键独立, 与 shake_key 同策略): action_type 合法且 MOUSE 时
                 * keycode ∈{1,2,4} 才落盘; 非法值整键忽略(保留现值, 绝不污染 flash)。 */
                if (data_len >= (2 + KEY_CONFIG_NUM_KEYS * sizeof(key_config_t) + 3 + 11 + 18))
                {
                    uint8_t *l2 = &pkt.buf[10 + KEY_CONFIG_NUM_KEYS * sizeof(key_config_t) + 3 + 11];
                    uint8_t *l3 = l2 + sizeof(key_config_t);
                    key_config_t lk;
                    memcpy(&lk, l2, sizeof(lk));
                    if (key_config_lkey_valid(&lk))
                        memcpy(&cfg->l2_key, &lk, sizeof(lk));
                    memcpy(&lk, l3, sizeof(lk));
                    if (key_config_lkey_valid(&lk))
                        memcpy(&cfg->l3_key, &lk, sizeof(lk));
                }
                /* 09-05: KEY L1 —— 追加在 L2/L3 之后: buf[69..77] (payload[61..69])。
                 * 新 payload 长度 = 70。合法动作比 L2/L3 多一个 AIRMOUSE(默认)。 */
                if (data_len >= (2 + KEY_CONFIG_NUM_KEYS * sizeof(key_config_t) + 3 + 11 + 27))
                {
                    uint8_t *l1 = &pkt.buf[10 + KEY_CONFIG_NUM_KEYS * sizeof(key_config_t) + 3 + 11 + 18];
                    key_config_t lk;
                    memcpy(&lk, l1, sizeof(lk));
                    if (key_config_l1key_valid(&lk))
                        memcpy(&cfg->l1_key, &lk, sizeof(lk));
                }
                /* 09-05: EC 编码器三手势 —— 追加在 L1 之后:
                 *   buf[78..86]=ec_cw  buf[87..95]=ec_press  buf[96..104]=ec_ccw
                 * 新 payload 长度 = 97。MOUSE 伪键码: 旋转键 0x05/0x06=滚轮上/下,
                 * 按下键仅 1/2/4(滚轮无意义, key_config_eckey_valid 拒绝)。 */
                if (data_len >= (2 + KEY_CONFIG_NUM_KEYS * sizeof(key_config_t) + 3 + 11 + 27 + 27))
                {
                    uint8_t *ec = &pkt.buf[10 + KEY_CONFIG_NUM_KEYS * sizeof(key_config_t) + 3 + 11 + 27];
                    key_config_t lk;
                    memcpy(&lk, ec, sizeof(lk));
                    if (key_config_eckey_valid(&lk, 1))
                        memcpy(&cfg->ec_cw_key, &lk, sizeof(lk));
                    memcpy(&lk, ec + sizeof(key_config_t), sizeof(lk));
                    if (key_config_eckey_valid(&lk, 0))
                        memcpy(&cfg->ec_press_key, &lk, sizeof(lk));
                    memcpy(&lk, ec + 2 * sizeof(key_config_t), sizeof(lk));
                    if (key_config_eckey_valid(&lk, 1))
                        memcpy(&cfg->ec_ccw_key, &lk, sizeof(lk));
                }
                /* 09-05: C 键三手势(双击/长按) —— 追加在 EC 之后, 每键两块共 6 块:
                 *   buf[105..113]=c1_dbl  buf[114..122]=c1_lng
                 *   buf[123..131]=c2_dbl  buf[132..140]=c2_lng
                 *   buf[141..149]=c3_dbl  buf[150..158]=c3_lng
                 * 偏移基准 = 10+27(keys)+2(sleep)+1(dir)+1(en)+1(sens)+9(shake_key)
                 *            +9(L2)+9(L3)+9(L1)+27(EC) = buf[105]。
                 * 新 payload 长度 = 151。旧上位机(97B)不带此块 -> 不进入分支,
                 * 双击/长按保持现值(双向兼容, 与 L/EC 块策略一致)。
                 * 校验(逐键独立): key_config_ckey_valid(NONE/键盘/多媒体 + MOUSE
                 * 键码 1..6); 非法值整键忽略(保留现值, 绝不污染 flash)。
                 * 09-07: C 键显式模式 + 手势"单击"独立块追加其后 —— 紧凑 payload
                 * 与结构体相差 7B(结构体 keys[3] 后有 1B padding), 故:
                 *   payload[151..152]=reserved_tail@158..159 占位(设备忽略)
                 *   payload[153..155]=c1..c3_mode(buf[161..163], 结构体 @160..162)
                 *   payload[156]=reserved_mode_pad@163 占位(设备忽略)
                 *   payload[157..165]=c1_tap(buf[165..173], 结构体 @164..172)
                 *   payload[166..174]=c2_tap  payload[175..183]=c3_tap
                 * 新 payload 长度 = 184。旧上位机(151B/156B)不带后续块 -> 不进入
                 * 对应分支, 手势/模式/单击块保持现值(双向兼容)。 */
                if (data_len >= (2 + KEY_CONFIG_NUM_KEYS * sizeof(key_config_t) + 3 + 11 + 27 + 27 + 54))
                {
                    uint8_t *cg = &pkt.buf[10 + KEY_CONFIG_NUM_KEYS * sizeof(key_config_t) + 3 + 11 + 27 + 27];
                    key_config_t lk;
                    memcpy(&lk, cg, sizeof(lk));
                    if (key_config_ckey_valid(&lk))
                        memcpy(&cfg->c1_dbl_key, &lk, sizeof(lk));
                    memcpy(&lk, cg + 1 * sizeof(key_config_t), sizeof(lk));
                    if (key_config_ckey_valid(&lk))
                        memcpy(&cfg->c1_lng_key, &lk, sizeof(lk));
                    memcpy(&lk, cg + 2 * sizeof(key_config_t), sizeof(lk));
                    if (key_config_ckey_valid(&lk))
                        memcpy(&cfg->c2_dbl_key, &lk, sizeof(lk));
                    memcpy(&lk, cg + 3 * sizeof(key_config_t), sizeof(lk));
                    if (key_config_ckey_valid(&lk))
                        memcpy(&cfg->c2_lng_key, &lk, sizeof(lk));
                    memcpy(&lk, cg + 4 * sizeof(key_config_t), sizeof(lk));
                    if (key_config_ckey_valid(&lk))
                        memcpy(&cfg->c3_dbl_key, &lk, sizeof(lk));
                    memcpy(&lk, cg + 5 * sizeof(key_config_t), sizeof(lk));
                    if (key_config_ckey_valid(&lk))
                        memcpy(&cfg->c3_lng_key, &lk, sizeof(lk));
                    /* 09-07: 显式模式 —— buf[161..163](cg 后 2B 占位 reserved_tail) */
                    if (data_len >= (2 + KEY_CONFIG_NUM_KEYS * sizeof(key_config_t) + 3 + 11 + 27 + 27 + 54 + 5))
                    {
                        uint8_t *cm = &pkt.buf[10 + KEY_CONFIG_NUM_KEYS * sizeof(key_config_t)
                                               + 3 + 11 + 27 + 27 + 54 + 2];
                        uint8_t v = cm[0];
                        if (v == 0 || v == 1)
                            cfg->c1_mode = v;
                        v = cm[1];
                        if (v == 0 || v == 1)
                            cfg->c2_mode = v;
                        v = cm[2];
                        if (v == 0 || v == 1)
                            cfg->c3_mode = v;
                    }
                    /* 09-07: 手势"单击"独立块 —— buf[165..191](cg 后 2B reserved_tail
                     * + 3B 模式 + 1B padding @164 占位, 即 buf[164])。 */
                    if (data_len >= (2 + KEY_CONFIG_NUM_KEYS * sizeof(key_config_t) + 3 + 11 + 27 + 27 + 54 + 5 + 1 + 27))
                    {
                        uint8_t *ct = &pkt.buf[10 + KEY_CONFIG_NUM_KEYS * sizeof(key_config_t)
                                               + 3 + 11 + 27 + 27 + 54 + 2 + 3 + 1];
                        memcpy(&lk, ct, sizeof(lk));
                        if (key_config_ckey_valid(&lk))
                            memcpy(&cfg->c1_tap_key, &lk, sizeof(lk));
                        memcpy(&lk, ct + 1 * sizeof(key_config_t), sizeof(lk));
                        if (key_config_ckey_valid(&lk))
                            memcpy(&cfg->c2_tap_key, &lk, sizeof(lk));
                        memcpy(&lk, ct + 2 * sizeof(key_config_t), sizeof(lk));
                        if (key_config_ckey_valid(&lk))
                            memcpy(&cfg->c3_tap_key, &lk, sizeof(lk));
                    }
                    /* 09-07: L1/L2/L3 模式 + 三手势 —— 全在旧 192B 写界之外
                     * (结构体 @192..275, 见 key_config.h; buf = 结构体偏移 + 1):
                     *   buf[193..195]=l1..l3_mode  buf[196..249]=l1_dbl..l3_lng(6x9B,
                     *   排列 l1_dbl@196 l1_lng@205 l2_dbl@214 l2_lng@223
                     *          l3_dbl@232 l3_lng@241)  buf[250..276]=l1_tap..l3_tap
                     *   (l1_tap@250 l2_tap@259 l3_tap@268)
                     * 紧凑 payload(设备按 data_len 阈值判定, 见 worker):
                     *   payload[184]=@191 pad 占位(忽略) [185..187]=模式
                     *   [188..241]=dbl/lng [242..268]=tap —— payload 总长 269。
                     * 校验(逐块独立): L1(idx0)手势允许 AIRMOUSE, L2/L3 不允许;
                     * 非法整块忽略(保留现值, 绝不污染 flash)。 */
                    if (data_len >= (2 + KEY_CONFIG_NUM_KEYS * sizeof(key_config_t) + 3 + 11 + 27 + 27 + 54 + 5 + 1 + 27 + 4))
                    {
                        uint8_t *lm = &pkt.buf[10 + KEY_CONFIG_NUM_KEYS * sizeof(key_config_t)
                                               + 3 + 11 + 27 + 27 + 54 + 2 + 3 + 1 + 27 + 1];
                        uint8_t v = lm[0];
                        if (v == 0 || v == 1)
                            cfg->l1_mode = v;
                        v = lm[1];
                        if (v == 0 || v == 1)
                            cfg->l2_mode = v;
                        v = lm[2];
                        if (v == 0 || v == 1)
                            cfg->l3_mode = v;
                        if (data_len >= (2 + KEY_CONFIG_NUM_KEYS * sizeof(key_config_t) + 3 + 11 + 27 + 27 + 54 + 5 + 1 + 27 + 4 + 54))
                        {
                            uint8_t *lb = lm + 3;   /* buf[196]: l1_dbl..l3_lng 六块 */
                            key_config_t *dst[6] = {
                                &cfg->l1_dbl_key, &cfg->l1_lng_key,
                                &cfg->l2_dbl_key, &cfg->l2_lng_key,
                                &cfg->l3_dbl_key, &cfg->l3_lng_key,
                            };
                            for (int i = 0; i < 6; i++)
                            {
                                memcpy(&lk, lb + i * sizeof(key_config_t), sizeof(lk));
                                if (key_config_lgesture_valid(&lk, (i / 2) == 0))   /* i0/1 = L1 */
                                    memcpy(dst[i], &lk, sizeof(lk));
                            }
                            if (data_len >= (2 + KEY_CONFIG_NUM_KEYS * sizeof(key_config_t) + 3 + 11 + 27 + 27 + 54 + 5 + 1 + 27 + 4 + 54 + 27))
                            {
                                uint8_t *lt = lb + 6 * sizeof(key_config_t);   /* buf[250]: tap 三块 */
                                memcpy(&lk, lt, sizeof(lk));
                                if (key_config_lgesture_valid(&lk, 1))
                                    memcpy(&cfg->l1_tap_key, &lk, sizeof(lk));
                                memcpy(&lk, lt + 1 * sizeof(key_config_t), sizeof(lk));
                                if (key_config_lgesture_valid(&lk, 0))
                                    memcpy(&cfg->l2_tap_key, &lk, sizeof(lk));
                                memcpy(&lk, lt + 2 * sizeof(key_config_t), sizeof(lk));
                                if (key_config_lgesture_valid(&lk, 0))
                                    memcpy(&cfg->l3_tap_key, &lk, sizeof(lk));
                            }
                        }
                        /* 09-07: EC 按下【模式 + 三手势】—— 全在旧 276B 写界之外
                         * (结构体 @276..303, 见 key_config.h; buf = 结构体偏移 + 1):
                         *   buf[277]=ec_press_mode  buf[278..286]=ec_press_dbl
                         *   buf[287..295]=ec_press_lng  buf[296..304]=ec_press_tap
                         * 紧凑 payload(设备按 data_len 阈值判定, 见 worker):
                         *   payload[269]=模式 payload[270..296]=dbl/lng/tap(3x9B)
                         *   —— payload 总长 269 -> 297(帧 305B)。
                         * 校验(逐块独立): 同 EC 常规键(key_config_ecpressgesture_valid
                         *   = eckey_valid: NONE/键盘/多媒体/MOUSE 键码 1..6 + AIRMOUSE);
                         * 非法整块忽略(保留现值, 绝不污染 flash)。 */
                        if (data_len >= (2 + KEY_CONFIG_NUM_KEYS * sizeof(key_config_t) + 3 + 11 + 27 + 27 + 54 + 5 + 1 + 27 + 4 + 54 + 27 + 1))
                        {
                            uint8_t *em = cg + 172;   /* cg=buf[105](结构体@104) -> em=buf[277](结构体@276) */
                            uint8_t v = em[0];
                            if (v == 0 || v == 1)
                                cfg->ec_press_mode = v;
                            if (data_len >= (2 + KEY_CONFIG_NUM_KEYS * sizeof(key_config_t) + 3 + 11 + 27 + 27 + 54 + 5 + 1 + 27 + 4 + 54 + 27 + 1 + 27))
                            {
                                uint8_t *eb = em + 1;   /* buf[278]: dbl/lng/tap 三块 */
                                memcpy(&lk, eb, sizeof(lk));
                                if (key_config_ecpressgesture_valid(&lk))
                                    memcpy(&cfg->ec_press_dbl_key, &lk, sizeof(lk));
                                memcpy(&lk, eb + 1 * sizeof(key_config_t), sizeof(lk));
                                if (key_config_ecpressgesture_valid(&lk))
                                    memcpy(&cfg->ec_press_lng_key, &lk, sizeof(lk));
                                memcpy(&lk, eb + 2 * sizeof(key_config_t), sizeof(lk));
                                if (key_config_ecpressgesture_valid(&lk))
                                    memcpy(&cfg->ec_press_tap_key, &lk, sizeof(lk));
                            }
                            /* 10-05: EC 按压滚动上/下 —— 全在旧 304B 写界之外
                             * (结构体 @304..321, 见 key_config.h; buf = 结构体偏移 + 1):
                             *   buf[305..313]=ec_cw_press  buf[314..322]=ec_ccw_press
                             * 紧凑 payload(设备按 data_len 阈值判定, 见 worker):
                             *   payload[297..305]=ec_cw_press [306..314]=ec_ccw_press
                             *   —— payload 总长 297 -> 315(帧 8+315=323B)。
                             * 校验(逐块独立): 同 EC 旋转键(key_config_eckey_valid
                             *   rotation=1: NONE/键盘/多媒体/MOUSE 键码 1..6 + AIRMOUSE);
                             * 非法整块忽略(保留现值, 绝不污染 flash)。
                             * ⚠️ 旧上位机(payload<=297B)不带此块 -> 不进入分支,
                             *   按压滚动保持现值(双向兼容)。 */
                            if (data_len >= (2 + KEY_CONFIG_NUM_KEYS * sizeof(key_config_t) + 3 + 11 + 27 + 27 + 54 + 5 + 1 + 27 + 4 + 54 + 27 + 1 + 27 + 18))
                            {
                                /* ec_press_dbl/lng/tap 之后紧接两块:
                                 * em=buf[277](结构体@276) -> +1(模式) +3*9(手势) = buf[305] */
                                uint8_t *es = em + 1 + 3 * sizeof(key_config_t);   /* buf[305] = 结构体 @304 */
                                memcpy(&lk, es, sizeof(lk));
                                if (key_config_eckey_valid(&lk, 1))
                                    memcpy(&cfg->ec_cw_press_key, &lk, sizeof(lk));
                                memcpy(&lk, es + sizeof(key_config_t), sizeof(lk));
                                if (key_config_eckey_valid(&lk, 1))
                                    memcpy(&cfg->ec_ccw_press_key, &lk, sizeof(lk));
                            }
                        }
                    }
                }
                key_config_save_slot(slot);
                rgb_req_t req = { .type = 1 };
                rt_mq_send(rgb_mq, &req, sizeof(req));
                conf_status_reply(&pkt, 0x00);
            }
            else if (cmd == CONF_CMD_RESET)
            {
                key_config_reset_slot(slot);
                key_config_save_slot(slot);
                rgb_req_t req = { .type = 1 };
                rt_mq_send(rgb_mq, &req, sizeof(req));
                conf_status_reply(&pkt, 0x00);
            }
            else if (cmd == CONF_CMD_RGB_APPLY && data_len >= 6)
            {
                uint8_t idx = pkt.buf[8];
                uint8_t en  = pkt.buf[9];
                uint8_t r   = pkt.buf[10];
                uint8_t g   = pkt.buf[11];
                uint8_t b   = pkt.buf[12];
                uint8_t bri = pkt.buf[13];
                /* 10-04 排障日志：先确认命令**真的到达了固件**。
                 * ⚠️ 必须拆两行：rt_kprintf 单次输出受 RT_CONSOLEBUF_SIZE(128) 限制。
                 * tag 用 [RGB] 便于 grep。 */
                rt_kprintf("[RGB] APPLY len=%d idx=%d en=%d rgb=%d,%d,%d bri=%d\n",
                           (int)data_len, (int)idx, (int)en,
                           (int)r, (int)g, (int)b, (int)bri);
                /* 10-04 排障：固件回包从 1 字节改 **2 字节** [0]=0x00 已受理
                 * [1]=实际生效的 bri。让上位机能区分"命令被接受"与"被丢弃"，
                 * 否则永远显示成功、问题完全不可见。 */
                if (idx < KEY_CONFIG_NUM_KEYS)
                {
                    uint8_t eff_bri = rgb_led_cap_brightness(bri);
                    rgb_req_t req = { .type = 0, .idx = idx, .en = en,
                                      .r = r, .g = g, .b = b, .bri = bri };
                    rt_mq_send(rgb_mq, &req, sizeof(req));
                    rt_kprintf("[RGB] queued idx=%d eff_bri=%d (mq ok)\n",
                               (int)idx, (int)eff_bri);
                    conf_tx_buf[0] = 0x00;
                    conf_tx_buf[1] = eff_bri;
                    conf_send_reply(&pkt, conf_tx_buf, 2);
                }
                else
                {
                    rt_kprintf("[RGB] REJECT idx=%d >= KEY_CONFIG_NUM_KEYS=%d\n",
                               (int)idx, (int)KEY_CONFIG_NUM_KEYS);
                    conf_status_reply(&pkt, 0x01);   /* idx 非法 -> 明确报错 */
                }
            }
            else if (cmd == CONF_CMD_LED && data_len >= 1)
            {
                ble_led_set_state(pkt.buf[8]);
                conf_status_reply(&pkt, 0x00);
            }
            else if (cmd == CONF_CMD_INFO)
            {
                /* 回包格式: "VibeKey-F3|<版本>|<电量>|<槽位>\n"
                 * 电量: 0~100 为真实百分比；未就绪(开机首读前)返回 255 哨兵，上位机按"未知"处理。
                 * 槽位: 1~3 为当前激活的蓝牙槽位；方便上位机首次连接时自动同步显示。
                 * (09-03 BLE 通道也靠它拿真实槽位 —— ble_config_worker 先 INFO 再按槽读写) */
                int bat = app_battery_valid() ? (int)app_get_battery_percent() : 255;
                int slot = (int)bt_multi_get_slot();   /* 1~3 */
                int info_len = rt_snprintf((char *)conf_tx_buf, sizeof(conf_tx_buf),
                                           "%s|%s|%d|%d\n", VIBEKEY_MODEL, VIBEKEY_FW_VERSION, bat, slot);
                conf_send_reply(&pkt, conf_tx_buf, (uint16_t)info_len);
            }
        }
    }
}

/* 09-03 (BLE 改建键配置): BLE GATT 0xFF03 回调(协议栈线程)收到命令帧后调用,
 * 拷贝投进 conf_mq —— 与 USB ISR 投递同构, 保证 flash 写/回包都发生在
 * conf_task 线程上下文。命令格式与 USB 线上完全一致(magic+cmd+slot+len+payload),
 * conf_task 无需区分来源即可解析; 仅回包按 src 路由。 */
int conf_ble_submit(uint8_t conn_idx, const uint8_t *buf, uint16_t len)
{
    if (buf == NULL || len < 8 || len > CONF_PKT_MAX)
        return -1;
    if (conf_mq == NULL)   /* USB 模块未初始化(理论上开机即建, 防御) */
        return -1;
    conf_pkt_t pkt;
    pkt.len = len;
    pkt.src = CONF_SRC_BLE;
    pkt.conn_idx = conn_idx;
    memcpy(pkt.buf, buf, len);
    /* rt_mq_send 满时阻塞 —— 队列深 4、命令低频(BLE 改建键配置为人手操作),
     * 且调用方是线程上下文(非 ISR), 阻塞可接受; 极端情况由上层超时兜底。 */
    if (rt_mq_send(conf_mq, &pkt, sizeof(pkt)) != RT_EOK)
    {
        rt_kprintf("[CONF] BLE submit failed (len=%u)\n", len);
        return -1;
    }
    return 0;
}

static void rgb_apply_task(void *param)
{
    rgb_req_t req;
    while (1)
    {
        if (rt_mq_recv(rgb_mq, &req, sizeof(req), RT_WAITING_FOREVER) == RT_EOK)
        {
            if (req.type == 0)
            {
                if (req.idx < KEY_CONFIG_NUM_KEYS)
                {
                    /* ★ C1/C2/C3 亮度硬上限 15%（10-04 产品要求，硬件主动降低）。
                     * 钳的是**亮度参数 bri**，不是下面的颜色值 ——
                     * 因为 rgb_led_show() 收到的是"已缩放"的输出，若在那儿再压一次
                     * 就成双重降幅（bri=15 → 38 → 6 ≈ 2.4%，肉眼全黑，10-04 实测踩过）。
                     * 上位机即使传 100，也会在此被压到 15，软件层绕不过。
                     * ⚠️ 只作用于 C1/C2/C3；状态灯走 ble_led_service→rgb_led_set_color，不经此处。 */
                    uint8_t bri = rgb_led_cap_brightness(req.bri);
                    uint32_t rr = ((uint32_t)req.r * bri) / 100;
                    uint32_t gg = ((uint32_t)req.g * bri) / 100;
                    uint32_t bb = ((uint32_t)req.b * bri) / 100;
                    /* 10-04 排障：真正出灯前再打一条 —— 与上面的 "APPLY/queued" 配对。
                     * 若只有 APPLY 没有 APPLY-ON，说明消息队列的消费线程没跑起来。 */
                    rt_kprintf("[RGB] show idx=%d en=%d bri=%d -> 0x%06X\n",
                               (int)req.idx, (int)req.en, (int)bri,
                               (unsigned)((req.en ? ((rr << 16) | (gg << 8) | bb) : 0)));
                    rgb_led_show(req.idx, req.en ? ((rr << 16) | (gg << 8) | bb) : 0);
                }
            }
            else
            {
                key_config_apply_rgb();
            }
        }
    }
}

uint8_t ota_check_complete(void) { return ota_complete; }

static void q_write(const uint8_t *data, uint32_t len)
{
    /* 溢出保护: 环形队列满时不再覆盖未读数据(q_tail 之后), 多余字节直接丢弃。
     * 丢弃会导致固件流损坏(CRC/型号校验失败、安全 ABORT 防变砖), 但绝不发生
     * "静默覆盖" 使旧字节被新字节替换却总长不变 —— 那种错位最难排查。
     * 正常路径靠下方临界区 + bulk_out 流控 + USB 背压, 这里只是最后一道闸。 */
    rt_enter_critical();   /* 与消费线程 q_read 互斥: ISR 与线程并发改 q_head/q_tail/q_count
                             会造成 q_count 丢更新→流控失效→环形队列真实溢出→固件流错位。 */
    uint32_t room = sizeof(flash_queue) - q_count;
    if (len > room)
        len = room;
    for (uint32_t i = 0; i < len; i++)
    {
        flash_queue[q_head] = data[i];
        q_head = (q_head + 1) % sizeof(flash_queue);
    }
    q_count += len;
    if (q_count > q_count_peak)
        q_count_peak = q_count;
    rt_exit_critical();
    rt_sem_release(q_sem);
}

static uint32_t q_read(uint8_t *dst, uint32_t max_len)
{
    rt_enter_critical();   /* 与 ISR q_write 互斥(见 q_write 注释): 保证 q_tail/q_head/q_count 原子更新 */
    uint32_t to_read = (q_count < max_len) ? q_count : max_len;
    for (uint32_t i = 0; i < to_read; i++)
    {
        dst[i] = flash_queue[q_tail];
        q_tail = (q_tail + 1) % sizeof(flash_queue);
    }
    q_count -= to_read;
    rt_exit_critical();
    return to_read;
}

static void usbd_event_handler(uint8_t busid, uint8_t event)
{
    if (event == USBD_EVENT_CONFIGURED)
    {
        ep_tx_busy_flag = false;
        usbd_ep_start_read(busid, CDC_OUT_EP, read_buffer, 2048);
    }
    else if (event == USBD_EVENT_RESET)
    {
        /* 总线复位：清发送忙标志，OUT 接收会在随后的 CONFIGURED 事件中重新武装，
         * 避免 reset 后设备收不到数据 / 串口打不开。*/
        ep_tx_busy_flag = false;
    }
}

void usbd_cdc_acm_bulk_out(uint8_t busid, uint8_t ep, uint32_t nbytes)
{
    uint32_t magic = (nbytes >= 8) ? *(uint32_t *)&read_buffer[0] : 0;

    if (!ota_started && !conf_busy && nbytes >= 8)
    {
        if (magic == OTA_MAGIC)
        {
            uint32_t ts = *(uint32_t *)&read_buffer[4];
            /* 上界检查: OTA 镜像 = 8 字节头 + ts, 扇区对齐后不得越过 OTA_REGION_END,
             * 否则后续 rt_flash_erase 会抹掉 DFU 区外的 Flash(FS/配置区)。
             * 越界时直接拒绝: 不进入 OTA 态、不擦写、不重启, 设备维持正常使 PC 端可感知。 */
            /* HIGH-2: 固件体 = ts, 另含 PC 端追加的尾部 4 字节 CRC, 越界检查按 ts+4 计 */
            uint32_t erase_size = (ts + 8 + 4 + 4095) & ~4095;
            if (ts > OTA_BODY_MAX_SIZE || erase_size > (OTA_REGION_END - OTA_TARGET_ADDR))
            {
                rt_kprintf("[OTA] size overflow, ABORT. body=%u > %u or erase=%u > region=%u\r\n",
                           ts, OTA_BODY_MAX_SIZE, erase_size, OTA_REGION_END - OTA_TARGET_ADDR);
                usbd_ep_start_read(busid, CDC_OUT_EP, read_buffer, 2048);
                return;
            }
            /* HIGH-2: 固件体大小 = ts, 另含尾部 4 字节 CRC(由 PC 端追加), 故需多收 4 字节 */
            ota_total_size = ts + 4;
            ota_received = 0;
            ota_started = 1;
            ota_erase_done = 0;
            ota_model_state = 0;
            ota_model_found = 0;
            ota_model_best = 0;
            ota_model_best_off = 0;
            ota_crc_running = 0xFFFFFFFF;   /* CRC32 初值 */
            memcpy(ota_header, read_buffer, 8);
            rt_kprintf("OTA: start, size=%d\r\n", ota_total_size);
            q_write(read_buffer + 8, nbytes - 8);
            usbd_ep_start_read(busid, CDC_OUT_EP, read_buffer, 2048);
            return;
        }
        else if (magic == CONF_MAGIC)
        {
            /* ISR 只拷贝并投递：真正处理（含 flash 写/回包）由 conf_task 线程完成，
               避免在中断上下文同步调用 key_config_save()（取互斥量->fatal）与
               usbd_ep_start_write（MUSB 回包在 ISR 内不安全）。*/
            conf_pkt_t pkt;
            uint32_t cp = (nbytes > CONF_PKT_MAX) ? CONF_PKT_MAX : nbytes;
            pkt.len = cp;
            pkt.src = CONF_SRC_USB;      /* 09-03: 回包路由来源标记 */
            pkt.conn_idx = 0;
            memcpy(pkt.buf, read_buffer, cp);
            rt_mq_send(conf_mq, &pkt, sizeof(pkt));
            usbd_ep_start_read(busid, CDC_OUT_EP, read_buffer, 2048);
            return;
        }
    }

    if (ota_started && nbytes > 0)
    {
        /* 流控前置: 队列余量不足时立即暂停 USB 读取, 避免下面 q_write 覆盖尚未被
         * 消费线程读出的数据(环形队列 q_head 越过 q_tail 会静默覆盖, 使固件流错位、
         * 型号串/CRC 被破坏却总长不变, 极难排查)。原逻辑在 q_write 之后才查, 高速
         * USB 下会越界。这里用更紧的余量(4KB)提前暂停, 靠 USB 背压(NCK)让 PC 端写阻塞。 */
        if (q_count >= sizeof(flash_queue) - 4096)
        {
            ep_busid = busid;
            ep_read_paused = 1;
            return;
        }
        /* 防御：OTA 进行中若收到控制包（典型为 PC 端 CONF 查询），
         * 绝不能当固件字节入队，否则污染固件流使 CRC 校验失败、升级回退旧固件。
         * - 若整包恰好是一个完整的 CONF 命令(长度=8+data_len)，丢弃该包并照常处理它；
         * - 若长度不符（与被固件字节粘连/分片），无法安全剥离 → 拒绝激活、保留旧固件防变砖。 */
        if (magic == CONF_MAGIC)
        {
            uint16_t dlen = *(uint16_t *)&read_buffer[6];
            if (nbytes == (uint32_t)8 + dlen)
            {
                conf_pkt_t pkt;
                uint32_t cp = (nbytes > CONF_PKT_MAX) ? CONF_PKT_MAX : nbytes;
                pkt.len = cp;
                pkt.src = CONF_SRC_USB;      /* 09-03: 回包路由来源标记 */
                pkt.conn_idx = 0;
                memcpy(pkt.buf, read_buffer, cp);
                rt_mq_send(conf_mq, &pkt, sizeof(pkt));
                rt_kprintf("[OTA] dropped stray CONF packet (len=%u), firmware stream intact\r\n", nbytes);
                usbd_ep_start_read(busid, CDC_OUT_EP, read_buffer, 2048);
                return;
            }
            else
            {
                rt_kprintf("[OTA] CONF packet len=%u != 8+data_len, cannot isolate, ABORT (keep old fw)\r\n", nbytes);
                rt_thread_mdelay(200);
                NVIC_SystemReset();
            }
        }
        q_write(read_buffer, nbytes);
    }

    if (ota_started && q_count >= sizeof(flash_queue) - 16384)
    {
        ep_busid = busid;
        ep_read_paused = 1;
        return;
    }
    usbd_ep_start_read(busid, CDC_OUT_EP, read_buffer, 2048);
}

void usbd_cdc_acm_bulk_in(uint8_t busid, uint8_t ep, uint32_t nbytes)
{
    rt_kprintf("CDC IN: nbytes=%d\r\n", nbytes);
    if ((nbytes % usbd_get_ep_mps(busid, ep)) == 0 && nbytes)
        usbd_ep_start_write(busid, CDC_IN_EP, NULL, 0);
    else
        ep_tx_busy_flag = false;
}

struct usbd_endpoint cdc_out_ep = { .ep_addr = CDC_OUT_EP, .ep_cb = usbd_cdc_acm_bulk_out };
struct usbd_endpoint cdc_in_ep  = { .ep_addr = CDC_IN_EP,  .ep_cb = usbd_cdc_acm_bulk_in };
static struct usbd_interface intf0, intf1;

/* 保存 busid/reg_base，供 usb_start/usb_stop 只在控制器层 enable/disable，
 * 避免每次插拔都重新注册接口/端点（重复注册会让 USB 描述符错乱、枚举失败）。*/

/* 一次性注册接口/端点 + 创建 RGB 线程。开机调用一次，插拔 USB 时不再重跑。*/
void cdc_acm_init(uint8_t busid, uintptr_t reg_base)
{
    g_busid = busid;
    g_reg_base = reg_base;
#ifdef CONFIG_USBDEV_ADVANCE_DESC
    usbd_desc_register(busid, &cdc_descriptor);
#else
    usbd_desc_register(busid, cdc_descriptor);
#endif
    usbd_add_interface(busid, usbd_cdc_acm_init_intf(busid, &intf0));
    usbd_add_interface(busid, usbd_cdc_acm_init_intf(busid, &intf1));
    usbd_add_endpoint(busid, &cdc_out_ep);
    usbd_add_endpoint(busid, &cdc_in_ep);

    /* RGB 实时预览线程（ISR 不能直接调 WS2812B 驱动，经消息队列转发）*/
    rgb_mq = rt_mq_create("rgbmq", sizeof(rgb_req_t), RGB_REQ_MQ_SIZE, RT_IPC_FLAG_FIFO);
    rt_thread_t rgb_tid = rt_thread_create("rgbapp", rgb_apply_task, RT_NULL, 1024, 15, 10);
    if (rgb_tid)
        rt_thread_startup(rgb_tid);

    /* CONF 命令处理线程（ISR 只投递，避免 ISR 内 flash 写/回包导致 USB 挂死）*/
    conf_mq = rt_mq_create("confmq", sizeof(conf_pkt_t), 4, RT_IPC_FLAG_FIFO);
    rt_thread_t conf_tid = rt_thread_create("conftask", conf_task_entry, RT_NULL, 2048, 14, 10);
    if (conf_tid)
        rt_thread_startup(conf_tid);
}

/* 插上 Type-C：只初始化 USB 控制器（接口/端点已一次性注册）。*/
void cdc_acm_enable(void)
{
    ep_tx_busy_flag = false;
    ota_started = 0;
    usbd_initialize(g_busid, g_reg_base, usbd_event_handler);
}

/* 拔掉 Type-C：只停用控制器并关时钟。*/
void cdc_acm_disable(void)
{
    usbd_deinitialize(g_busid);
}

volatile uint8_t dtr_enable = 0;
void usbd_cdc_acm_set_dtr(uint8_t busid, uint8_t intf, bool dtr) { dtr_enable = dtr ? 1 : 0; }

static uint8_t wbuf[8192];

void ep_resume_read(void)
{
    if (ep_read_paused && q_count < sizeof(flash_queue) - 16384)
    {
        ep_read_paused = 0;
        usbd_ep_start_read(ep_busid, CDC_OUT_EP, read_buffer, 2048);
    }
}

void ota_flash_task_stop(void)
{
    ota_stop = 1;
    if (q_sem)
        rt_sem_release(q_sem);
}

void ota_flash_task(int typec_pin)
{
    ota_stop = 0;
    q_head = q_tail = q_count = 0;
    ota_started = 0;
    ota_erase_done = 0;
    ota_received = 0;
    ep_read_paused = 0;
    q_sem = rt_sem_create("ota", 0, RT_IPC_FLAG_FIFO);

    rt_kprintf("OTA: flash task ready, target=0x%08x\r\n", OTA_TARGET_ADDR);

    uint32_t write_off = 0;

    while (!ota_stop)
    {
        /* 永久等待，不用 500ms 超时轮询：
         * - 数据路径：q_write()（USB ISR）每次入队都 rt_sem_release，来数据必醒；
         * - 退出路径：ota_flash_task_stop() 置 ota_stop 后也 release，必醒；
         * - ep_resume_read() 在每写完一块后调用，无需超时兜底。
         * ⚠️ 带超时的 sem_take = 常驻 500ms OS 定时器 = DEEP 待机期每 0.5s 被
         * LPTIM1 拽醒一次（2026-07-30 实测 135 次伪唤醒的唯一来源），故必须 FOREVER。 */
        rt_sem_take(q_sem, RT_WAITING_FOREVER);

        while (q_count > 0)
        {
            if (!ota_erase_done && ota_started)
            {
                /* 仅擦除 OTA 区并定位写指针; 此刻【不】写 magic。
                 * magic(ota_header) 延后到全部固件体收齐、reboot 之前才写入,
                 * 以确保"传输中途断电"时 DFU 区没有合法 magic,
                 * bootloader 不会把半截固件装进运行区 → 避免变砖、可安全重试。 */
                uint32_t erase_size = (ota_total_size + 8 + 4095) & ~4095;
                rt_flash_erase(OTA_TARGET_ADDR, erase_size);
                ota_erase_done = 1;
                write_off = 8;
            }

            uint32_t chunk = (q_count > 4096) ? 4096 : q_count;
            q_read(wbuf, chunk);
            /* 流式处理: 型号匹配(原逻辑) + CRC32(仅固件体) + 捕获尾部 4 字节 CRC */
            {
                uint32_t base = ota_received;   /* 本块首个字节的全局偏移 */
                for (uint32_t i = 0; i < chunk; i++)
                {
                    uint8_t c = wbuf[i];
                    /* 型号匹配(命中后本块剩余字节仍需走 CRC, 故不 break) */
                    if (!ota_model_found) {
                        if (c == (uint8_t)OTA_MODEL_TAG[ota_model_state]) {
                            uint32_t match_start = base + i - ota_model_state; /* ota_model_state 为匹配前长度 */
                            ota_model_state++;
                            if (ota_model_state > ota_model_best) {
                                ota_model_best = ota_model_state;
                                ota_model_best_off = match_start;
                            }
                            if (ota_model_state >= OTA_MODEL_TAG_LEN)
                                ota_model_found = 1;
                        } else {
                            if (c == (uint8_t)OTA_MODEL_TAG[0])
                                ota_model_state = 1;
                            else
                                ota_model_state = 0;
                        }
                    }
                    /* CRC32: 仅固件体字节(偏移 < ota_total_size-4)参与; 尾部 4 字节捕获 */
                    uint32_t off = base + i;
                    if (off < ota_total_size - 4) {
                        ota_crc_running = ota_crc32_update(ota_crc_running, &c, 1);
                    } else {
                        ota_crc_trailer[off - (ota_total_size - 4)] = c;
                    }
                }
            }
            rt_flash_write(OTA_TARGET_ADDR + write_off, wbuf, chunk);
            write_off += chunk;
            ota_received += chunk;

            ep_resume_read();

            if (ota_received >= ota_total_size)
            {
                if (!ota_model_found)
                {
                    /* 型号不匹配：极可能是选错了固件文件。
                     * 拒绝激活(不写 magic)，直接 reboot 回到旧固件，避免变砖。 */
                    rt_kprintf("[OTA] model mismatch, ABORT. expect '%s' recv=%u total=%u q_peak=%u state=%u best=%d best_off=%u\r\n",
                               OTA_MODEL_TAG, ota_received, ota_total_size, q_count_peak, ota_model_state, ota_model_best, ota_model_best_off);
                    rt_thread_mdelay(200);
                    NVIC_SystemReset();
                }
                /* HIGH-2: 校验 CRC32(固件体) 与发送方追加的尾部 4 字节是否一致;
                 * 不一致说明传输损坏/文件错误, 拒绝激活(不写 magic), 避免坏文件变砖。 */
                uint32_t crc_calc = ota_crc_running ^ 0xFFFFFFFF;
                uint32_t crc_stored = ((uint32_t)ota_crc_trailer[0])
                                    | ((uint32_t)ota_crc_trailer[1] << 8)
                                    | ((uint32_t)ota_crc_trailer[2] << 16)
                                    | ((uint32_t)ota_crc_trailer[3] << 24);
                if (crc_calc != crc_stored)
                {
                    /* 诊断(08-27): q_peak 接近队列容量说明曾接近背压极限;
                     * recv/total 不一致说明流不完整; trailer 首字节用于对比错位程度 */
                    rt_kprintf("[OTA] CRC mismatch, ABORT (keep old fw). calc=0x%08x stored=0x%08x\r\n",
                               crc_calc, crc_stored);
                    rt_kprintf("[OTA] diag: q_peak=%u/%u recv=%u total=%u trailer=%02x%02x%02x%02x\r\n",
                               q_count_peak, (unsigned)sizeof(flash_queue),
                               ota_received, ota_total_size,
                               ota_crc_trailer[0], ota_crc_trailer[1],
                               ota_crc_trailer[2], ota_crc_trailer[3]);
                    rt_thread_mdelay(200);
                    NVIC_SystemReset();
                }
                /* 收齐且校验通过后才写 magic 激活: 断电安全的最后一道闸。
                 * 此时固件体已完整落盘, 即使之后立即断电, 重启也会装好新固件。 */
                rt_flash_write(OTA_TARGET_ADDR, ota_header, 8);
                rt_kprintf("OTA: complete, %d bytes, CRC OK, magic set, rebooting...\r\n", ota_received);
                rt_thread_mdelay(200);
                NVIC_SystemReset();
            }
        }
    }

    rt_kprintf("[USB] OTA task stopped\r\n");
    if (q_sem)
    {
        rt_sem_delete(q_sem);
        q_sem = RT_NULL;
    }
}
