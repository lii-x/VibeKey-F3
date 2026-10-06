#ifndef PORTDETECTOR_H
#define PORTDETECTOR_H

#include <QObject>
#include <QTimer>
#include <QStringList>
#include <QByteArray>
#include <QSerialPort>
#include "VibeKeyCommon.h"

/* 共享的 VibeKey 设备探测器：整个应用只有一个实例，
 * 负责从所有 COM 口中按 VID 找出 VibeKey 设备、定时握手确认在线，
 * 并向各业务模块（OTA / 按键配置）提供统一的端口与连接状态。 */
class PortDetector : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QStringList portList READ portList NOTIFY portListChanged)
    Q_PROPERTY(bool deviceConnected READ deviceConnected NOTIFY deviceConnectedChanged)
    Q_PROPERTY(QString deviceName READ deviceName NOTIFY deviceNameChanged)
    Q_PROPERTY(QString deviceVersion READ deviceVersion NOTIFY deviceVersionChanged)
    Q_PROPERTY(int batteryPercent READ batteryPercent NOTIFY batteryPercentChanged)
    Q_PROPERTY(int deviceSlot READ deviceSlot NOTIFY deviceSlotChanged)   /* 08-31: 当前激活蓝牙槽位 1~3；<=0 未知 */

public:
    explicit PortDetector(QObject *parent = nullptr);
    ~PortDetector();

    QStringList portList() const { return m_portList; }
    bool deviceConnected() const { return m_deviceConnected; }
    QString deviceName() const { return m_deviceName; }
    QString deviceVersion() const { return m_deviceVersion; }
    int batteryPercent() const { return m_batteryPercent; }   /* 0~100 真实百分比；-1 未知 */
    int deviceSlot() const { return m_deviceSlot; }           /* 08-31: 1~3 当前槽位；-1 未知 */

    /* 当前识别到的 VibeKey 设备端口（如 "COM3"），未连接时返回空 */
    QString devicePort() const { return m_deviceConnected ? m_devicePort : QString(); }

    Q_INVOKABLE void refreshPorts();

    /* 通知探测器某处正在占用串口，暂停探测以免抢端口。
     * 采用引用计数：可多次 setPortInUse(true)，需要同样次数的
     * setPortInUse(false) 才会真正恢复探测。 */
    Q_INVOKABLE void setPortInUse(bool inUse);

    /* OTA / 大文件传输期间彻底停掉轮询定时器。
     * 比 setPortInUse 引用计数更彻底：定时器 stop 后 probeDevice 不会在任何时刻触发，
     * 可绝对避免 CONF 查询包混入固件流导致设备端 CRC 校验失败、升级回退旧固件。 */
    Q_INVOKABLE void pausePolling();
    Q_INVOKABLE void resumePolling();

signals:
    void portListChanged();
    void deviceConnectedChanged();
    void deviceNameChanged();
    void deviceVersionChanged();
    void batteryPercentChanged();
    void deviceSlotChanged();

private slots:
    void probeDevice();
    void onProbeReadyRead();
    void onProbeTimeout();
    void onProbeError(QSerialPort::SerialPortError error);

private:
    QStringList detectVibeKeyPorts();

    /* ---- 异步探测状态机(08-31) ----
     * 旧实现在 GUI 线程里 waitForBytesWritten(500) + while(800ms){waitForReadyRead(300)},
     * 设备不回包时每 1.5s 冻结界面近 1s, 是卡顿头号来源。现改为信号驱动:
     *   probeDevice() 只负责枚举 + 启动本轮探测, 立刻返回;
     *   readyRead 收包, m_probeTimeout(单次 800ms) 兜底超时;
     *   收全一帧 -> 解析并更新状态; 超时/出错 -> 关端口, 试下一个候选端口。
     * 全程零阻塞调用, 事件循环不被卡住。 */
    void startProbeSequence(const QStringList &ports);
    void probeNext();
    void abortProbe();
    void closeProbePort();
    void applyProbeResult(bool probed, const QString &name,
                          const QString &version, const QString &port, int bat);
    void applyProbeSlot(int slot);

    QTimer *m_pollTimer;
    QStringList m_portList;
    bool m_deviceConnected;
    QString m_deviceName;
    QString m_deviceVersion;
    QString m_devicePort;
    int m_batteryPercent = -1;   /* 电池百分比，<0 表示未知 */
    int m_deviceSlot = -1;       /* 08-31: 当前蓝牙槽位 1~3；<0 未知 */
    int m_busyCount;

    QSerialPort *m_probePort;      /* 复用的探测串口(仅在探测期间 open) */
    QTimer *m_probeTimeout;        /* 单次: 等待设备回包超时 */
    QStringList m_probeCandidates; /* 本轮待试端口 */
    int m_probeIndex;              /* 当前试到第几个 */
    QByteArray m_probeResp;        /* 累积回包 */
    bool m_probeActive;            /* 本轮探测进行中(防重入堆积) */
};

#endif // PORTDETECTOR_H
