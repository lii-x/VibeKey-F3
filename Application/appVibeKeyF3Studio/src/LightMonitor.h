#ifndef LIGHTMONITOR_H
#define LIGHTMONITOR_H

#include <QObject>
#include <QProcess>
#include <QTimer>
#include <QElapsedTimer>
#include "PortDetector.h"

class LightMonitor : public QObject
{
    Q_OBJECT
    Q_PROPERTY(bool bleConnected READ bleConnected NOTIFY bleConnectedChanged)
    Q_PROPERTY(QString bleDeviceName READ bleDeviceName NOTIFY bleDeviceNameChanged)
    /* 蓝牙连接时通过 CONF INFO 命令透传的固件版本号(与 USB 的 deviceVersion 同源);
     * 空串表示未知/未取到。供连接状态显示拼接 "vX.Y.Z"。 */
    Q_PROPERTY(QString bleDeviceVersion READ bleDeviceVersion NOTIFY bleDeviceVersionChanged)
    /* 当前发送通道：ble=蓝牙已连走蓝牙；usb=蓝牙未连走 USB；none=两者都无 */
    Q_PROPERTY(QString channel READ channel NOTIFY channelChanged)
    /* 蓝牙连接时通过 BLE Battery Service 读到的电量；<0 表示未知/未读成功 */
    Q_PROPERTY(int bleBatteryPercent READ bleBatteryPercent NOTIFY bleBatteryPercentChanged)
    /* ★ 蓝牙诊断状态 —— 回答"看不到设备到底卡在哪"，用于给出**针对性**引导。
     *   空串     = 未知（还没探测过 / 非 macOS）
     *   "found"  = 设备正在广播但系统没连 -> 多半没配对 / 槽位不匹配
     *   "absent" = 附近没有设备在广播 -> 设备睡了（按键唤醒）或没开机
     *   "paired" = 本机有配对记录但当前未连接 -> 去系统蓝牙点连接
     *   "slot"   = 本机配对的是另一个蓝牙地址（设备停在别的槽位）-> 按 C 键切槽
     *   "busy"   = 系统显示已连接，但配置通道拿不到 -> 配对记录异常，需重新配对
     *   "btoff"  = 这台 Mac 的蓝牙已关闭
     *   "noperm" = 扫描被系统拦截（蓝牙权限未授予）-> 去隐私设置授权
     * 只有 macOS 的 probe 会给出非空值；其余平台恒为空，UI 据空串隐藏提示。 */
    Q_PROPERTY(QString bleDiagnostic READ bleDiagnostic NOTIFY bleDiagnosticChanged)

public:
    explicit LightMonitor(QObject *parent = nullptr);
    ~LightMonitor();

    bool bleConnected() const { return m_bleConnected; }
    QString bleDeviceName() const { return m_bleDeviceName; }
    QString bleDeviceVersion() const { return m_bleDeviceVersion; }
    void setBleDeviceVersion(const QString &v);   /* 09-03: 由 KeyConfigManager 解析 BLE worker 的 VERSION 行设置 */
    QString channel() const { return m_channel; }
    int bleBatteryPercent() const { return m_bleBatteryPercent; }
    QString bleDiagnostic() const { return m_bleDiagnostic; }

    Q_INVOKABLE void sendState(int state);
    Q_INVOKABLE void setPortName(const QString &port);
    Q_INVOKABLE void refreshBle();
    /* ★ macOS 深度诊断：已连接列表查不到时主动扫描，区分
     *   "设备在广播但没配对" 与 "设备压根不在附近"。
     * 低频调用（用户点刷新 / 首次发现连不上），**不要**进 3s 轮询 ——
     * 一次扫描要 8s，塞进轮询会堆积。 */
    Q_INVOKABLE void probeBleDiagnostics();
    /* macOS：打开系统“蓝牙”设置页，引导用户先把设备配对/连接。
     * macOS 的 CoreBluetooth 只暴露系统已连接的外设，App 本身无法发起配对。 */
    Q_INVOKABLE void openBluetoothSettings();
    /* macOS：打开「隐私与安全性 → 蓝牙」权限页。
     * 用途：诊断出 noperm（扫描被 TCC 拦截）时，直接把用户送到授权开关，
     * 而不是让他自己去系统设置里翻。 */
    Q_INVOKABLE void openBluetoothPrivacySettings();
    /* macOS：按**当前**诊断结论把用户送到对应的系统面板。
     * 为什么不放在 QML 里判断：probe 是异步的（8~25s），QML 里
     * `probeBleDiagnostics(); if (bleDiagnostic === ...)` 读到的永远是
     * **上一次**的结论 —— 第一次点"点此授权"会什么都不打开。
     * 这里由 C++ 直接读 m_bleDiagnostic 分派，语义明确、不依赖时序。 */
    Q_INVOKABLE void openDiagnosticSettings();
    /* 注入共享的设备探测器，用于蓝牙未连接时走 USB 发送 */
    void setPortDetector(PortDetector *detector);

    /* 09-03 (BLE 改建键配置): Windows 同一 BLE 设备的 GATT 连接同时只允许一个
     * 客户端进程 —— 常驻 serve 占着连接时, 配置 worker 第二个进程的服务发现会
     * 失败(E_INVALIDARG/空服务表)。故 BLE 配置操作前暂停 serve(独占连接),
     * 操作完成后再恢复。只停/启 serve 进程, 不动 m_bleConnected/channel 状态。 */
    void pauseBleServe();
    void resumeBleServe();

    /* ★ 10-04: 通过**常驻 BLE serve 连接**下发单键 RGB 预览。
     * 复用 serve 已持有的 GATT 会话（与 sendState 走同一条路），因此**毫秒级**完成。
     * 关键收益：不再为每次调色启一个一次性 config worker 进程 + 重新做 GATT
     * 服务/特征发现（原方案 1~2s/次），也**不需要 pauseBleServe 独占连接** ——
     * 0xFF03 配置特征与 0xFF01 LED 特征在同一个 0xFF00 服务里，serve 一次就取到。
     * 返回 true 表示已交给 serve（无需再走 USB/一次性进程兜底）。 */
    bool sendRgbViaServe(int ledIndex, bool enabled, int r, int g, int b, int brightnessPct);
    /* serve 是否已就绪（收到 READY）且进程在跑 —— 决定能否走快速通道 */
    bool bleServeUsable() const;

signals:
    void bleConnectedChanged();
    void bleDeviceNameChanged();
    void bleDeviceVersionChanged();
    void channelChanged();
    void bleBatteryPercentChanged();
    void bleDiagnosticChanged();
    /* ★ 10-04: 常驻 serve 进程【连接就绪】状态变化。
     * 启动时序关键：channel=="ble" 早于 serve READY，若此时就自动读配置，
     * 会把"正在连接中的 serve"杀掉去抢连接 ⇒ AccessDenied ⇒ 表现为
     * "已连接但操作失败/无响应"。KeyConfigManager 据此把自动读推迟到
     * serve 真正握手完成之后。 */
    void bleServeReadyChanged();

private slots:
    void onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void checkBleConnection();
    void onBleCheckFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void onBleServeFinished(int exitCode, QProcess::ExitStatus exitStatus);
    /* 常驻 BLE 进程上报真实断连（连续多次 ConnectionStatus 非 CONNECTED） */
    void onServeLinkLost();
    /* probe 诊断子进程结束：解析 CONNECTED/FOUND/DISCONNECTED 到 m_bleDiagnostic */
    void onBleProbeFinished(int exitCode, QProcess::ExitStatus exitStatus);

private:
    QString findPython();
    QString findWorkerScript();
    QString findBleWorkerScript();
    void updateChannel();
    void startBleServe();
    void stopBleServe();

    QTimer *m_bleTimer;
    QProcess *m_bleProc;
    QTimer *m_bleProcTimeout = nullptr;  // status 探测硬超时(等授权时防止进程堆积)
    bool m_bleConnected;
    QString m_bleDeviceName;
    QString m_bleDeviceVersion;   // 蓝牙连接透传的固件版本号(空=未知)
    int m_bleBatteryPercent = -1;  // 通过 BLE Battery Service 读到的电量，<0 表示未知
    int m_bleDiscStreak = 0;       // 曾连接后连续探测为"断开"的次数(去抖, 见 onBleCheckFinished);
                                   // 从未连接时首次 DISCONNECTED 直接判定断开, 不攒计数
    /* ★ 11-25: 蓝牙权限被拒/未授权。
     * 置位后 checkBleConnection 从 3s 轮询退到 30s —— 未授权时每 3s 新起一个
     * python 进程碰 CoreBluetooth，就是每 3s 一次新的 TCC 弹窗；而旧进程只等 6s
     * 就退出，进程一退弹窗被系统撤掉，用户永远点不到"允许"。
     * （实测 tccd 日志：6 小时 160 次弹窗，一次成功授权都没有。）
     * m_bleNoPermTimer 记录退避窗口起点。 */
    bool m_bleNoPerm = false;
    QElapsedTimer m_bleNoPermTimer;
    /* 蓝牙诊断状态（仅 macOS 的 probe 会写）：
     *   ""/"found"/"absent"，含义见 Q_PROPERTY 注释。 */
    QString m_bleDiagnostic;
    QProcess *m_bleProbeProc = nullptr;   // probe 诊断子进程（同一时刻只允许一个）
    QTimer *m_bleProbeTimeout = nullptr;  // probe 硬超时(等授权时防止进程堆积)

    PortDetector *m_portDetector;   // 蓝牙未连接时走 USB 发送用
    QString m_channel;              // "ble" / "usb" / "none"
    bool m_viaUsb;                  // 上一次发送是否占用 USB（用于结束后释放）

    QString m_pythonPath;
    QString m_portName;
    QProcess *m_process;
    QByteArray m_stderrBuf;
    QByteArray m_stdoutBuf;

    // 蓝牙常驻发送进程：保持一条 BLE 连接，手动测试直接写 stdin，避免每次重连+服务发现
    QProcess *m_bleServeProc;
    bool m_bleServeReady;           // 收到 READY 后为 true
    int m_pendingServeState;        // 未就绪时缓存最后一次 state（-1 = 无）
    QByteArray m_serveBuf;          // 累积 serve 进程 stdout 用于按行解析
    /* 09-03: serve 延迟恢复定时器 —— Windows 释放上一进程的 BLE 连接有 1~2s
     * 延迟, config worker 退出后立刻重连 serve 会撞释放窗口(E_INVALIDARG/
     * 特征空), 统一延迟 ~2s 再启; pause 时停表避免与新一轮配置抢连接。 */
    QTimer m_serveResumeTimer;
    /* 09-05: 配置独占标志 —— pauseBleServe 置位, resumeBleServe 清除。
     * updateChannel 的"蓝牙可用即自动拉起 serve"在此期间必须跳过, 否则 serve
     * 会在 config worker 会话中途复活, 与其抢 GATT(设备端并发发现返回空),
     * 是"配置失败→serve 反复重启→缓存反复污染"螺旋的直接推手。 */
    bool m_serveSuppressed = false;

    // 灯效下发合并缓存：USB 发送在飞时（config_worker.py 单次约 1~2s），记住最新一次
    // 想要的状态，待发送完成后补发，避免任务完成→idle 等快速连续状态变化被顶部守卫丢弃。
    int m_desiredState = -1;        // 最近一次请求的灯效状态（sendState 入口始终刷新）
    int m_lastSentState = -1;       // 最近一次实际已下发（USB/BLE）的状态，用于去重
    bool m_usbPending = false;      // USB 发送期间有更新的状态待补发
};

#endif // LIGHTMONITOR_H
