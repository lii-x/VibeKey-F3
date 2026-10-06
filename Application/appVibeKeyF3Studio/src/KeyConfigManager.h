#ifndef KEYCONFIGMANAGER_H
#define KEYCONFIGMANAGER_H

#include <QObject>
#include <QProcess>
#include <QJsonArray>
#include <QKeyEvent>
#include <QTimer>
#include <QElapsedTimer>
#include <QList>
#ifdef Q_OS_WIN
#include <windows.h>
#endif

class PortDetector;
class LightMonitor;   /* 09-03: BLE 通道状态源(通道仲裁: USB 优先, 无 USB 走 BLE) */

class KeyConfigManager : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString statusMessage READ statusMessage NOTIFY statusMessageChanged)
    Q_PROPERTY(QVariantList keyConfigs READ keyConfigs NOTIFY keyConfigsChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(QStringList keyNames READ keyNameList CONSTANT)
    Q_PROPERTY(QVariantList keyValues READ keyValueList CONSTANT)
    Q_PROPERTY(bool capturing READ capturing NOTIFY capturingChanged)
    Q_PROPERTY(int captureIndex READ captureIndex NOTIFY capturingChanged)
    /* KEY L1 空中鼠标设置 */
    Q_PROPERTY(int airMouseMode READ airMouseMode NOTIFY airMouseModeChanged)
    Q_PROPERTY(int airMouseSpeed READ airMouseSpeed NOTIFY airMouseSpeedChanged)
    /* 08-27: 每槽独立键配置 —— 当前编辑/读写的槽位 0~2(槽1~3) */
    Q_PROPERTY(int slot READ slot NOTIFY slotChanged)
    /* 08-27: 连接态空闲休眠时间(分钟)。0=永不; 档位 45/60/90/120/180 */
    Q_PROPERTY(int sleepMin READ sleepMin NOTIFY sleepMinChanged)
    /* 08-31: 配置是否已读取（QML 绑定用，避免只依赖 configReadComplete 信号时机） */
    Q_PROPERTY(bool configRead READ configRead NOTIFY configReadChanged)
    /* 09-03: 摇一摇(晃动设备触发快捷键)。shakeEnabled 0=关/1=开; shakeSens
     * 0=轻/1=中/2=强; shakeAction none|keyboard|multimedia(UI 只写 keyboard,
     * multimedia 为兼容读回); shakeModifier/shakeKeycode 为键盘组合键。 */
    Q_PROPERTY(int shakeEnabled READ shakeEnabled NOTIFY shakeChanged)
    Q_PROPERTY(int shakeSens READ shakeSens NOTIFY shakeChanged)
    Q_PROPERTY(QString shakeAction READ shakeAction NOTIFY shakeChanged)
    Q_PROPERTY(int shakeModifier READ shakeModifier NOTIFY shakeChanged)
    Q_PROPERTY(int shakeKeycode READ shakeKeycode NOTIFY shakeChanged)
    /* 09-05: L2/L3 侧键快捷键。map 字段 action/modifier/keycode —— action:
     * mouse(鼠标键, keycode 1/2/4=左/右/中, 按住保持)|keyboard|multimedia|none。
     * 与 C1-C3 的 m_keyConfigs 分开存放(QML 可视化索引: 0-2=C1-C3, 3=L1,
     * 4=摇一摇, 5/6=L2/L3, 避免与卡片索引空间冲突)。 */
    Q_PROPERTY(QVariantMap l2Key READ l2Key NOTIFY l2KeyChanged)
    Q_PROPERTY(QVariantMap l3Key READ l3Key NOTIFY l3KeyChanged)
    /* 09-05: KEY L1 —— 默认 airmouse(空中鼠标开关, 保留原模式/灵敏度子设置),
     * 可改 keyboard/multimedia/none(及 mouse)。 */
    Q_PROPERTY(QVariantMap l1Key READ l1Key NOTIFY l1KeyChanged)
    /* 09-05: EC 编码器三手势。gesture 0=顺时针(默认滚轮上) 1=按下(默认中键)
     * 2=逆时针(默认滚轮下)。MOUSE 键码扩展 5/6=滚轮上/下。共享一个 NOTIFY。 */
    Q_PROPERTY(QVariantMap ecCwKey READ ecCwKey NOTIFY ecKeysChanged)
    Q_PROPERTY(QVariantMap ecPressKey READ ecPressKey NOTIFY ecKeysChanged)
    Q_PROPERTY(QVariantMap ecCcwKey READ ecCcwKey NOTIFY ecKeysChanged)
    /* 10-04: EC 按压滚动(手势模式新增) —— 按住旋钮时转动, 上/下各一份。
     * 与旋转键 ecCwKey/ecCcwKey 同构(默认 滚轮上 5 / 滚轮下 6); 常规模式不显示
     * (UI 仅手势模式可见), 数据层照常存写, 与"常规模式不改动原有 UI"一致。
     * gesture 编码沿用 ecKeyMapFor: 3=按压滚动上 4=按压滚动下。 */
    Q_PROPERTY(QVariantMap ecCwPressKey READ ecCwPressKey NOTIFY ecKeysChanged)
    Q_PROPERTY(QVariantMap ecCcwPressKey READ ecCcwPressKey NOTIFY ecKeysChanged)

public:
    explicit KeyConfigManager(QObject *parent = nullptr);
    ~KeyConfigManager();

    QString statusMessage() const { return m_statusMessage; }
    QVariantList keyConfigs() const { return m_keyConfigs; }
    bool busy() const { return m_busy; }
    bool capturing() const { return m_captureIndex != -1; }
    int captureIndex() const { return m_captureIndex; }
    int airMouseMode() const { return m_airMouseMode; }
    int airMouseSpeed() const { return m_airMouseSpeed; }
    int slot() const { return m_slot; }
    int sleepMin() const { return m_sleepMin; }
    bool configRead() const { return m_configRead; }
    int shakeEnabled() const { return m_shakeEnabled; }
    int shakeSens() const { return m_shakeSens; }
    QString shakeAction() const { return m_shakeAction; }
    int shakeModifier() const { return m_shakeModifier; }
    int shakeKeycode() const { return m_shakeKeycode; }
    QVariantMap l2Key() const { return m_l2Key; }
    QVariantMap l3Key() const { return m_l3Key; }
    QVariantMap l1Key() const { return m_l1Key; }
    QVariantMap ecCwKey() const { return m_ecCwKey; }
    QVariantMap ecPressKey() const { return m_ecPressKey; }
    QVariantMap ecCcwKey() const { return m_ecCcwKey; }
    QVariantMap ecCwPressKey() const { return m_ecCwPressKey; }
    QVariantMap ecCcwPressKey() const { return m_ecCcwPressKey; }

    /* 注入共享的设备探测器（由 main.cpp 在构造后设置）。
     * 08-31: 由内联改到 .cpp —— 注入时顺带接上 deviceConnectedChanged，
     * 实现"设备一连上就自动读一次按键配置"（与 setSlot 切槽即读保持一致）。 */
    void setPortDetector(PortDetector *detector);

    /* 09-03: 注入 BLE 连接状态源(LightMonitor)。未插 USB 但蓝牙已连时,
     * 键配置读写走 BLE 通道(ble_config_worker.py); 插上 USB 仍走 USB 优先。 */
    void setLightMonitor(LightMonitor *monitor);

    Q_INVOKABLE void readConfig();
    Q_INVOKABLE void writeConfig();
    Q_INVOKABLE void resetConfig();
    /* 08-27: 切换编辑槽位 0~2(槽1~3)。丢弃当前未保存修改并从设备读取该槽配置 */
    Q_INVOKABLE void setSlot(int slot);
    /* 08-27: 设置休眠时间(分钟)。0=永不; 档位 45/60/90/120/180 */
    Q_INVOKABLE void setSleepMin(int minutes);
    /* 09-07: C 键三手势 —— gesture 0=单击(tap) 1=双击(dbl) 2=长按(lng),
     * 分别读写 action/dbl_action/lng_action 及 modifier/keycode 字段 */
    Q_INVOKABLE void setKeyAction(int keyIndex, int gesture, const QString &action);
    Q_INVOKABLE void setKeyModifier(int keyIndex, int gesture, int modifier);
    Q_INVOKABLE void setKeyKeycode(int keyIndex, int gesture, int keycode);
    /* 09-07: C 键显式模式(0=常规/直通 1=手势)。随配置落盘, 固件按此判定,
     * 不再由双击/长按是否设置派生 —— 常规模式无视手势配置, 按下即发/松开即发 */
    Q_INVOKABLE void setKeyMode(int keyIndex, int mode);

    /* 根据当前 modifier + keycode 反查 keyIndex，用于物理按键识别 */
    int findKeyIndexByHid(int modifier, int keycode) const;
    /* 每键 RGB 底光：开关 / 颜色(拆分 R,G,B) / 亮度(0-100) */
    Q_INVOKABLE void setKeyRgbEnabled(int keyIndex, bool enabled);
    Q_INVOKABLE void setKeyRgbColor(int keyIndex, int r, int g, int b);
    Q_INVOKABLE void setKeyRgbBrightness(int keyIndex, int brightness);
    /* 实时预览：即时下发到设备灯光，不落盘；读/写进行中时跳过避免串口冲突 */
    Q_INVOKABLE void previewKeyRgb(int keyIndex);
    /* 熄灭某键底光（配置完/关弹窗时调用，使设备回到"松开即灭"的熄灭态） */
    Q_INVOKABLE void clearKeyRgb(int keyIndex);

    /* ============ C1/C2/C3 灯效测试（10-04）============
     * 独立于"按键配置"的**临时测试**通道：只改灯色，不写配置、不落盘，
     * 刷新页面/切走即失效，避免把测试值存成用户的按键配置。
     * brightnessPct 为 0~100 的用户意图值；固件侧另有 15% 硬上限兜底
     * （ws2812b.c 的 s_rgb_cap_brightness），这里只做 UI 侧的范围收敛。 */
    Q_INVOKABLE void ledTestSet(int ledIndex, int r, int g, int b, int brightnessPct);
    Q_INVOKABLE void ledTestClear(int ledIndex);
    Q_INVOKABLE void ledTestClearAll();
    /* 固件锁死的亮度上限（%），UI 用来显示"已受硬件限幅"的提示 */
    Q_INVOKABLE int ledBrightnessHardwareCap() const;

    Q_INVOKABLE int getKeycodeIndex(int keyIndex) const;
    Q_INVOKABLE QStringList keyNameList() const;
    Q_INVOKABLE QVariantList keyValueList() const;
    Q_INVOKABLE void startCapture(int keyIndex, int gesture = 0);   /* gesture: C 键(0-2)手势位; L/EC 键忽略 */
    Q_INVOKABLE void cancelCapture();
    /* 09-03 摇一摇 */
    Q_INVOKABLE void setShakeEnabled(int on);       /* 0=关 1=开 */
    Q_INVOKABLE void setShakeSens(int sens);        /* 0=轻 1=中 2=强 */
    Q_INVOKABLE void setShakeKey(int modifier, int keycode); /* 动作=键盘组合键 */
    Q_INVOKABLE void clearShakeKey();               /* 动作=无(不触发任何键) */
    Q_INVOKABLE void startShakeCapture();           /* 捕获摇一摇快捷键(索引=-2) */
    /* KEY L1 空中鼠标：模式 0=toggle(单击切换), 1=hold(按住移动/松开停止) */
    Q_INVOKABLE void setAirMouseMode(int mode);
    /* KEY L1 空中鼠标：灵敏度 0=slow, 1=medium, 2=fast */
    Q_INVOKABLE void setAirMouseSpeed(int speed);
    /* 09-05 L2/L3 侧键: which 0=L2 1=L3 2=L1。action 为组合框语义值:
     * mouse_left / mouse_right / mouse_middle / keyboard / multimedia / none,
     * L1 另支持 airmouse(空中鼠标开关, 默认)。 */
    Q_INVOKABLE void setLKeyAction(int which, const QString &action);
    /* 09-05 L2/L3: 键盘捕获完成后落地(动作置为 keyboard) */
    Q_INVOKABLE void setLKeyKey(int which, int modifier, int keycode);
    /* 09-05 EC 编码器: gesture 0=cw 1=press 2=ccw。action 为组合框语义值:
     * wheel_up / wheel_down / mouse_left / mouse_right / mouse_middle /
     * keyboard / multimedia / none(wheel_* 与 mouse_* 仅对应手势可用)。
     * 10-04 手势模式新增: 3=cw_press(按压滚动上) 4=ccw_press(按压滚动下),
     * 与旋转键同集(默认滚轮上 5 / 滚轮下 6)。 */
    Q_INVOKABLE void setEcKeyAction(int gesture, const QString &action);
    Q_INVOKABLE void setEcKeyKey(int gesture, int modifier, int keycode);
    Q_INVOKABLE void setEcKeyMultimedia(int gesture, int keycode);
    /* 09-06: L 键多媒体键码落地(动作置为 multimedia) */
    Q_INVOKABLE void setLKeyMultimedia(int which, int keycode);
    /* ===================== 09-07: L1/L2/L3 三键模式 + 三手势 =====================
     * 与 C 键同架构: 每键可选"常规/手势"。which: 0=L2 1=L3 2=L1。
     *   常规(直通): 用 l*_key 主键(历史行为: L1=空中鼠标开关/鼠标按住保持等),
     *               无视下方手势配置(存而不用);
     *   手势:       单击=独立 tap 块(l*_tap_key), 双击/长按 = dbl/lng 块。
     * 手势 pfx(gesture): 0=tap_ 1=dbl_ 2=lng_。固件 key_config_lgesture_valid:
     * MOUSE 键码 1..6; AIRMOUSE 仅 L1(which==2)允许(用户要求 L1 手势可切空中鼠标)。
     * 模式字节 l_mode 随配置【显式落盘】(固件 09-07 起不再由手势是否设置派生)。 */
    Q_INVOKABLE void setLKeyMode(int which, int mode);
    /* 动作: none/keyboard/mouse/multimedia/airmouse(airmouse 仅 L1 合法, 其余键按
     * none 落盘)。mouse 默认左键(键码 1), 之后用 setLKeyGestureKeycode 改细分。 */
    Q_INVOKABLE void setLKeyGestureAction(int which, int gesture, const QString &action);
    Q_INVOKABLE void setLKeyGestureKeycode(int which, int gesture, int keycode);
    Q_INVOKABLE void startLKeyGestureCapture(int which, int gesture);
    /* ===================== 09-07: EC 按下 模式 + 三手势 =====================
     * 与 C/L 键同架构: EC 按下可选"常规/手势"(旋转 cw/ccw 是离散步进, 无手势)。
     *   常规(直通): 用 ec_press_key 主键(历史行为: 中键/键/空中鼠标等),
     *               无视下方手势配置(存而不用);
     *   手势:       单击=独立 tap 块(固件 ec_press_tap_key), 双击/长按 = dbl/lng。
     * 手势 pfx(gesture): 0=tap_ 1=dbl_ 2=lng_(与 C/L 前缀语义一致)。
     * 固件 key_config_ecpressgesture_valid = eckey_valid: MOUSE 键码 1..6 +
     * AIRMOUSE(EC 常规键本就允许空中鼠标, 手势每触发切换一次)。
     * 模式字节 ec_mode 随配置【显式落盘】(固件 09-07 起不再由手势是否设置派生)。 */
    Q_INVOKABLE void setEcPressKeyMode(int mode);
    Q_INVOKABLE void setEcPressGestureAction(int gesture, const QString &action);
    Q_INVOKABLE void setEcPressGestureKeycode(int gesture, int keycode);
    Q_INVOKABLE void setEcPressGestureMultimedia(int gesture, int keycode);
    Q_INVOKABLE void startEcPressGestureCapture(int gesture);

signals:
    void statusMessageChanged();
    void keyConfigsChanged();
    void busyChanged();
    void configReadComplete();
    void configWriteComplete();
    void configResetComplete();
    void configFailed(const QString &error);
    void configReadChanged();
    void capturingChanged();
    void captured(int keyIndex, int modifier, int keycode);
    /* 非捕获模式下，按到设备上已配置的按键时触发（用于 UI 自动弹出对应设置） */
    void physicalKeyPressed(int keyIndex);
    void airMouseModeChanged();
    void airMouseSpeedChanged();
    void slotChanged();
    void sleepMinChanged();
    /* 09-03: 摇一摇任意一项变化(开关/灵敏度/快捷键), QML 只读绑定此信号刷新 */
    void shakeChanged();
    /* 09-05: L2/L3 侧键快捷键变化 */
    void l2KeyChanged();
    void l3KeyChanged();
    void l1KeyChanged();
    void ecKeysChanged();

private slots:
    void onProcessReadyRead();
    void onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void onProcessError(QProcess::ProcessError error);
    void onModifierTimeout();
    /* 08-31: 设备连接状态变化 -> 新连上时自动读一次按键配置 */
    void onDeviceConnectedChanged();
    /* 08-31: 设备端切换蓝牙槽位 -> 同步更新上位机当前编辑槽位并重新读取 */
    void onDeviceSlotChanged();
    /* 09-03: BLE 通道状态变化 -> BLE-only(无 USB)连上时自动读一次配置;
     * 全断(none)时复位自动读标记(与 USB 断开分支语义一致) */
    void onLightChannelChanged();
    /* ★ 10-04: 常驻 serve 握手完成 —— 启动时自动读配置被推迟到此之后，
     * 否则会把"正在连接中"的 serve 杀掉抢连接 ⇒ AccessDenied ⇒ 启动即"无响应"。 */
    void onBleServeReady();
    void tryAutoRead();          // m_autoReadTimer 的槽：serve 未就绪则继续等
    void doBleAutoRead();        // 真正执行 BLE 自动读

    /* 实时预览合并：拖拽时高频调用 previewKeyRgb 会瞬间刷爆串口/固件队列，
       这里用定时器把多次请求合并成 ~25Hz 的单一进程下发，且始终发送最新值 */
    void onPreviewTick();
    void onPreviewFinished();
#if defined(Q_OS_WIN) && !defined(Q_MOC_RUN)
    /* ⚠ Q_MOC_RUN 排除: 这是 Win32 低级键盘钩子回调(SetWindowsHookExW 用),
     * 不经元对象调用。moc 经 -I 解析 <QObject> 会连带拿到 Q_OS_WIN, 从而把本行
     * 纳入元对象; 但它带 Windows 调用约定宏 CALLBACK(=__stdcall), moc 会把
     * "CALLBACK" 原样当成返回类型写进 moc_*.cpp ->
     * QtMocHelpers::SlotData<__stdcall(int, WPARAM, LPARAM)> 语法错误, 构建失败。 */
    static LRESULT CALLBACK keyboardHookProc(int nCode, WPARAM wParam, LPARAM lParam);
#endif

private:
    void setStatus(const QString &msg);
    void setBusy(bool busy);
    QString findPython();
    QString findWorkerScript();
    QString findBleConfigWorker();   // 09-03: BLE 通道配置 worker 脚本路径
    void launchWorker(const QStringList &args);  // 09-03: 统一 QProcess 启动(USB/BLE)
    /* 09-03: BLE 配置独占会话 —— Windows 同设备 GATT 一次只容一个客户端进程,
     * 启动配置 worker 前暂停 LED serve, 结束时恢复。 */
    void launchBleConfig(const QStringList &args);
    void finishBleConfigIfNeeded();
    QJsonArray configsToJson();
    QString currentPort();   // 从共享探测器取当前设备端口
    bool bleChannelLive() const;     // 09-03: 当前 BLE 已连(LightMonitor channel==ble)
    /* 09-05: L2/L3/L1 键配置统一存取(0=L2 1=L3 2=L1) */
    QVariantMap lKeyMapFor(int which) const;
    void setLKeyMap(int which, const QVariantMap &m);
    static QString lKeyName(int which);
    /* 09-07: L 键三手势。lGesturePrefix: 0=tap_ 1=dbl_ 2=lng_(与固件存储块一致);
     * lGestureOrdinalFromWhich: which(0=L2 1=L3 2=L1) -> li(0=L1 1=L2 2=L3),
     * 供捕获索引(li*3)编解码。 */
    static QString lGesturePrefix(int gesture);
    static int lGestureOrdinalFromWhich(int which);
    /* 09-07: EC 按下三手势(gesture 0=单击 tap_ 1=双击 dbl_ 2=长按 lng_, 存于
     * ec_press_key map —— 与常规主键 action/modifier/keycode 分开, 互不干扰) */
    static QString ecPressGesturePrefix(int gesture);
    /* 09-05: EC 编码器三手势统一存取(0=cw 1=press 2=ccw) */
    QVariantMap ecKeyMapFor(int gesture) const;
    void setEcKeyMap(int gesture, const QVariantMap &m);
    static QString ecKeyName(int gesture);

    PortDetector *m_portDetector;
    LightMonitor *m_lightMonitor;    // 09-03: 通道仲裁用(USB 优先, 无 USB 走 BLE)
    QProcess *m_process;
    QString m_statusMessage;
    QVariantList m_keyConfigs;
    bool m_busy;
    QString m_pythonPath;
    bool m_lastDone;
    bool m_bleConfigOp;   /* 09-03: 当前 QProcess 是否 BLE 配置(结束需恢复 LED serve) */
    bool m_bleAutoReadPending;  /* 09-03: 下次 readConfig 为 BLE 连接自动读(槽=0xFF 激活槽) */
    int m_captureIndex;
    int m_airMouseMode;
    int m_airMouseSpeed;
    int m_slot;                 /* 08-27: 当前编辑槽位 0~2 */
    int m_sleepMin;             /* 08-27: 休眠分钟(0=永不), 默认 45 */
    bool m_autoReadDone;        /* 08-31: 本轮连接是否已自动读过一次配置(断开时复位) */
    /* ★ 10-04: 启动"无响应"修复 —— 自动读推迟到常驻 serve READY 之后。
     * 见 onLightChannelChanged() 的注释。等待期间靠 onBleServeReady() 唤醒，
     * 并有 MAX_WAIT 超时兜底（设备拒绝连接时也不至于永远不读）。 */
    bool m_bleAutoReadWaitingServe = false;
    qint64 m_bleAutoReadWaitStart = 0;
    static const int BLE_AUTO_READ_MAX_WAIT_MS = 8000;   // 最长等 serve 8s
    static const int BLE_AUTO_READ_RETRY_MS  = 400;     // 未就绪时每 400ms 再试
    QTimer m_autoReadTimer;     /* 08-31: 自动读取延迟触发器，等 QML 接好信号再读 */
    bool m_configRead;          /* 08-31: 最近一次连接是否已成功读过配置 */
    int m_captureMods;
    int m_prevMods;
    QList<int> m_prevKeys;
    bool m_prevKeyState[70];
#ifdef Q_OS_WIN
    HHOOK m_keyboardHook;
    QTimer m_modifierTimer;
    int m_lastMod;
#endif
    enum OpType { OP_NONE, OP_READ, OP_WRITE, OP_RESET } m_currentOp;
    /* 09-03: 摇一摇捕获目标索引。m_captureIndex == -2 表示当前在捕获摇一摇
     * 快捷键; >=0 为 C1/C2/C3 按键; -1 = 未在捕获。 */
    static const int kShakeCaptureIndex = -2;
    /* 09-05: L2/L3 捕获目标索引(与 QML 可视化卡片索引一致: 3=L1 4=摇一摇, 5/6=L2/L3) */
    static const int kL2CaptureIndex = 5;
    static const int kL3CaptureIndex = 6;
    static const int kL1CaptureIndex = 3;   /* L1 卡片索引即 3 */
    /* 09-05: EC 编码器三手势捕获目标(0=cw 1=press 2=ccw; 卡片索引 7) */
    static const int kEcCwCaptureIndex = 7;
    static const int kEcPressCaptureIndex = 8;
    static const int kEcCcwCaptureIndex = 9;
    /* 10-04: EC 按压滚动上/下 捕获目标(紧接 EC 按下手势区 28..30 之后, 31/32 无冲突)。
     * 与 QML 弹窗行模型的 cap 字段一致。 */
    static const int kEcCwPressCaptureIndex = 31;
    static const int kEcCcwPressCaptureIndex = 32;
    /* 09-07: C 键三手势捕获基址(避开 0-2 C 键/3 L1/5-6 L2L3/7-9 EC)。
     * 捕获索引 = kCKeyGestureBase + keyIndex*3 + gesture(0=tap 1=dbl 2=lng) */
    static const int kCKeyGestureBase = 10;
    /* 09-07: L 键三手势捕获基址(紧接 C 手势区之后, 19..27 无冲突)。
     * 捕获索引 = kLKeyGestureBase + li*3 + gesture, li = L 键序号(0=L1 1=L2 2=L3,
     * 对应卡片索引 3/5/6)。完成解码: li→which(2/0/1)。 */
    static const int kLKeyGestureBase = 19;
    /* 09-07: EC 按下三手势捕获基址(紧接 L 手势区之后, 28..30 无冲突)。
     * 捕获索引 = kEcPressGestureBase + gesture(0=单击 1=双击 2=长按)。 */
    static const int kEcPressGestureBase = 28;
    void finishCapture(int modifier, int keycode);  /* 捕获完成统一收尾(按目标路由) */
    /* 09-07: 捕获公共入口(置捕获索引 + 复位状态)。startCapture /
     * startShakeCapture / startLKeyGestureCapture 共用。 */
    void beginCapture(int target);
    int m_shakeEnabled;        /* 0=关 1=开 */
    int m_shakeSens;           /* 0=轻 1=中 2=强 */
    QString m_shakeAction;     /* none / keyboard / multimedia */
    int m_shakeModifier;       /* 键盘组合键修饰位 */
    int m_shakeKeycode;        /* 键盘主键码(HID usage) */

    /* 09-05: L2/L3 侧键快捷键。map 字段 action/modifier/keycode(与 worker JSON
     * "l2_key"/"l3_key" 同构); 默认鼠标左/右键, 与固件默认一致。
     * m_l1Key 同构, 默认 airmouse(空中鼠标开关)。 */
    QVariantMap m_l2Key;
    QVariantMap m_l3Key;
    QVariantMap m_l1Key;
    QVariantMap m_ecCwKey;      /* 09-05: EC 顺时针(默认滚轮上) */
    QVariantMap m_ecPressKey;   /* 09-05: EC 按下(默认中键) */
    QVariantMap m_ecCcwKey;     /* 09-05: EC 逆时针(默认滚轮下) */
    QVariantMap m_ecCwPressKey;   /* 10-04: EC 按压滚动上(默认滚轮上) */
    QVariantMap m_ecCcwPressKey;  /* 10-04: EC 按压滚动下(默认滚轮下) */

    /* 预览合并定时器（常驻运行，空闲时基本零开销） */
    QTimer m_previewTimer;
    QProcess *m_previewProcess;   // 复用的单预览进程，避免并发抢串口
    bool m_previewPending;
    bool m_previewOff;            // 覆盖：本次下发黑色（熄灭）而非配置色
    int m_previewKeyIndex;
    /* 10-04 灯效测试：本次预览的数据源是测试态而非 m_keyConfigs。
     * 不设独立进程/定时器 —— 串口是独占资源，必须与按键预览共用同一条下发通道。 */
    bool m_previewLedTest = false;
    /* 10-04: 本次预览走的是 BLE 通道（而非 USB 串口）。
     * 用于 onPreviewFinished 里决定要不要 resumeBleServe()。
     * 背景：预览此前硬编码走 USB worker，蓝牙通道下 RGB 完全发不出去。 */
    bool m_previewViaBle = false;

    /* C1/C2/C3 灯效测试的临时态（不写入 m_keyConfigs、不落盘）。
     * active=false 表示该灯当前不点亮。 */
    int  m_ledTestColor[3]     = {0, 0, 0};
    int  m_ledTestBrightness[3] = {0, 0, 0};
    bool m_ledTestActive[3]    = {false, false, false};

    /* 10-05「全部熄灭」逐灯补发队列。
     * 背景：worker/固件一次只认一个 idx（rgb <k> ...），而预览通道是**合并式**的
     * （只有 m_previewKeyIndex / m_previewPending 三个标量）⇒ 在 QML 里循环调用
     * ledTestClear(0..2) 会被互相覆盖，最终只发出最后一颗（C3），
     * 表现就是"点全部熄灭灭不掉"。这里把要补发的 idx 排队，onPreviewTick 每 tick 弹一个。 */
    QList<int> m_ledClearQueue;
};

#endif // KEYCONFIGMANAGER_H
