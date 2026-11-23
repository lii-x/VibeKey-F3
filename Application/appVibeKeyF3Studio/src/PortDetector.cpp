#include "PortDetector.h"
#include <QSerialPort>
#include <QSerialPortInfo>
#include <QDebug>
#include <QThread>

/* 等待设备回包的超时(ms)。
 * 旧实现是 `while (t.elapsed() < 800) { sp.waitForReadyRead(300); }` —— 阻塞轮询，
 * 在 GUI 线程里空等最多 800ms；设备不回包时每 1.5s 就冻一次界面，是卡顿头号来源。
 * 这里改成单次定时器兜底，语义等价但完全不占住事件循环。 */
#define PROBE_REPLY_TIMEOUT_MS 800

PortDetector::PortDetector(QObject *parent)
    : QObject(parent)
    , m_pollTimer(nullptr)
    , m_deviceConnected(false)
    , m_busyCount(0)
    , m_probePort(nullptr)
    , m_probeTimeout(nullptr)
    , m_probeIndex(0)
    , m_probeActive(false)
{
    // 定时轮询：Qt 无原生 USB 热插拔事件，靠轮询实现"插上即识别、拔掉即离线"
    m_pollTimer = new QTimer(this);
    m_pollTimer->setInterval(1500);
    connect(m_pollTimer, &QTimer::timeout, this, &PortDetector::probeDevice);

    /* 异步探测链路(08-31)：复用同一个 QSerialPort，靠信号驱动，全程零阻塞。
     * 旧实现每轮探测都栈上新建 QSerialPort，现改成员复用，避免频繁构造/析构。 */
    m_probePort = new QSerialPort(this);
    connect(m_probePort, &QSerialPort::readyRead, this, &PortDetector::onProbeReadyRead);
    connect(m_probePort, &QSerialPort::errorOccurred, this, &PortDetector::onProbeError);

    m_probeTimeout = new QTimer(this);
    m_probeTimeout->setSingleShot(true);
    connect(m_probeTimeout, &QTimer::timeout, this, &PortDetector::onProbeTimeout);

    m_pollTimer->start();

    refreshPorts();   // 初始枚举
    probeDevice();    // 立即探一次设备
}

PortDetector::~PortDetector()
{
    if (m_pollTimer)
        m_pollTimer->stop();
    if (m_probeTimeout)
        m_probeTimeout->stop();
    closeProbePort();
}

/* 枚举所有串口，仅保留 VID == VIBEKEY_VID 的 VibeKey 设备 */
QStringList PortDetector::detectVibeKeyPorts()
{
    QStringList ports;
    const auto infos = QSerialPortInfo::availablePorts();
    for (const QSerialPortInfo &info : infos) {
        if (info.vendorIdentifier() == VIBEKEY_VID) {
            ports.append(info.portName());
        }
    }
    return ports;
}

void PortDetector::refreshPorts()
{
    m_portList = detectVibeKeyPorts();
    emit portListChanged();
    qDebug() << "VibeKey candidate ports:" << m_portList;
}

/* 定时探测入口：只做枚举并启动本轮异步探测，立即返回（不做任何阻塞等待）。
 * 注意：open/握手失败不立即判离线（Windows USB CDC 偶发打不开属正常现象），
 * 只要端口仍存在于系统，就保留在线状态；只有端口彻底消失（拔掉）才判离线。 */
void PortDetector::probeDevice()
{
    // 有其它模块正在占用串口（升级 / 配置读写），暂停探测避免抢端口
    if (m_busyCount > 0)
        return;
    // 上一轮异步探测尚未结束（设备没回包），不堆积新的探测
    if (m_probeActive)
        return;

    QStringList candidates = detectVibeKeyPorts();
    if (m_portList != candidates) {
        m_portList = candidates;
        emit portListChanged();
    }

    // 端口从系统列表消失：设备被拔掉/真正离线
    if (candidates.isEmpty()) {
        if (m_deviceConnected || !m_deviceName.isEmpty() || !m_devicePort.isEmpty() || m_batteryPercent != -1 || m_deviceSlot != -1) {
            m_deviceConnected = false;
            m_deviceName.clear();
            m_deviceVersion.clear();
            m_devicePort.clear();
            m_batteryPercent = -1;
            m_deviceSlot = -1;
            emit deviceConnectedChanged();
            emit deviceNameChanged();
            emit deviceVersionChanged();
            emit batteryPercentChanged();
            emit deviceSlotChanged();
            qDebug() << "Device OFFLINE (VibeKey port gone)";
        }
        return;
    }

    startProbeSequence(candidates);
}

void PortDetector::startProbeSequence(const QStringList &ports)
{
    m_probeCandidates = ports;
    m_probeIndex = 0;
    m_probeActive = true;
    probeNext();
}

/* 打开第 m_probeIndex 个候选端口并发出查询包，随后立即返回。
 * 收包走 onProbeReadyRead，超时走 onProbeTimeout，出错走 onProbeError。 */
void PortDetector::probeNext()
{
    closeProbePort();

    if (m_probeIndex >= m_probeCandidates.size()) {
        /* 所有候选都未握手成功：端口仍在系统里，保留上次状态，不误判离线 */
        m_probeActive = false;
        qDebug() << "probe: 端口在系统中但本轮未握手成功，保留在线状态";
        return;
    }

    const QString portName = m_probeCandidates.at(m_probeIndex);
    m_probeResp.clear();

    m_probePort->setPortName(portName);
    m_probePort->setBaudRate(115200);
    m_probePort->setDataBits(QSerialPort::Data8);
    m_probePort->setParity(QSerialPort::NoParity);
    m_probePort->setStopBits(QSerialPort::OneStop);
    m_probePort->setFlowControl(QSerialPort::NoFlowControl);

    // 偶发打不开时先 close 再重试一次，提升重枚举/挂起恢复率
    if (!m_probePort->open(QIODevice::ReadWrite)) {
        closeProbePort();
        if (!m_probePort->open(QIODevice::ReadWrite)) {
            qDebug() << "probe: cannot open" << portName << "(端口暂不可用，保留在线状态)";
            m_probeIndex++;
            probeNext();
            return;
        }
    }
    m_probePort->clear(QSerialPort::AllDirections);

    if (m_probePort->write(VIBEKEY_QUERY) != VIBEKEY_QUERY.size()) {
        m_probeIndex++;
        probeNext();
        return;
    }

    /* 写请求已排入，等 readyRead 收包；800ms 没收全则由 m_probeTimeout 超时兜底 */
    m_probeTimeout->start(PROBE_REPLY_TIMEOUT_MS);
}

/* ============================================================================
 * USB RGB 预览直发（11-23）
 * ============================================================================
 * 帧格式（与 config_worker.py 的 conf_rgb 完全一致，14 字节）：
 *   [0..3] CONF_MAGIC(0x434F4E46, LE)  [4] cmd=0x06  [5] slot(0)
 *   [6..7] data_len=6 (LE)
 *   [8..13] idx, en, r, g, b, bri
 * 设备回 2 字节(code, eff_bri)，这里只等 120ms 尽量读走，读不到也不影响——
 * 预览是"发出去就行"的语义，不落盘。
 * ========================================================================== */
/* 通用：短连接写一帧 CONF 命令（cmd + payload），耗时 1~5ms。
 * 帧 = magic(4, LE) + cmd(1) + slot(1)=0 + len(2, LE) + payload */
static bool vk_send_conf_frame(const QString &portName, quint8 cmd, const QByteArray &payload)
{
    if (portName.isEmpty())
        return false;

    QSerialPort sp;
    sp.setPortName(portName);
    sp.setBaudRate(115200);
    /* 端口可能正被探测/配置流程短暂占用：重试 2 次，每次让 20ms */
    bool opened = false;
    for (int i = 0; i < 3 && !opened; i++) {
        opened = sp.open(QIODevice::ReadWrite);
        if (!opened) QThread::msleep(20);
    }
    if (!opened) {
        qDebug() << "[LedTest] direct usb preview: open failed on" << portName
                 << sp.errorString() << "-> fallback to worker";
        return false;
    }

    QByteArray frame;
    const quint32 magic = 0x434F4E46u;          /* CONF_MAGIC */
    frame.append(char(magic & 0xFF));
    frame.append(char((magic >> 8) & 0xFF));
    frame.append(char((magic >> 16) & 0xFF));
    frame.append(char((magic >> 24) & 0xFF));
    frame.append(char(cmd));
    frame.append(char(0x00));                    /* slot 0 */
    const int len = payload.size();
    frame.append(char(len & 0xFF));              /* data_len (LE) */
    frame.append(char((len >> 8) & 0xFF));
    frame.append(payload);

    const qint64 n = sp.write(frame);
    const bool ok = (n == frame.size()) && sp.waitForBytesWritten(80);
    if (ok) {
        /* 尽量把设备回的 2 字节读走，避免残留在下次探测的缓冲里 */
        sp.waitForReadyRead(60);
        sp.readAll();
    }
    sp.close();
    if (!ok)
        qDebug() << "[LedTest] direct usb: write failed, n=" << n;
    return ok;
}

bool PortDetector::sendUsbRgbPreview(const QString &portName, int idx, int en,
                                     int r, int g, int b, int bri)
{
    QByteArray pl;
    pl.append(char(idx & 0xFF));
    pl.append(char(en ? 1 : 0));
    pl.append(char(r & 0xFF));
    pl.append(char(g & 0xFF));
    pl.append(char(b & 0xFF));
    pl.append(char(bri & 0xFF));
    return vk_send_conf_frame(portName, 0x06 /*CONF_CMD_RGB_APPLY*/, pl);
}

bool PortDetector::sendUsbLedState(const QString &portName, int state)
{
    QByteArray pl;
    pl.append(char(state & 0xFF));
    /* ⚠️ 命令字必须与固件 dfu_device.c 一致：
     *   READ=0x01 WRITE=0x02 RESET=0x03 **LED=0x04** INFO=0x05 RGB_APPLY=0x06
     * 写成 0x05 会被当成 INFO 查询（设备只回版本号，灯不亮）。 */
    return vk_send_conf_frame(portName, 0x04 /*CONF_CMD_LED*/, pl);
}

void PortDetector::onProbeReadyRead()
{
    if (!m_probeActive)
        return;

    m_probeResp.append(m_probePort->readAll());
    if (!m_probeResp.contains('\n'))
        return;   /* 还没收完整帧，继续等（总时长由 m_probeTimeout 兜底） */

    m_probeTimeout->stop();

    const QString portName = m_probePort->portName();
    const QString text = QString::fromUtf8(m_probeResp).trimmed();
    if (!text.startsWith("VibeKey-F3")) {
        m_probeIndex++;
        probeNext();
        return;
    }

    const QStringList parts = text.split('|');
    const QString name = "VibeKey-F3";
    const QString version = parts.size() > 1 ? parts.at(1) : QString();
    int bat = -1;
    if (parts.size() > 2) {   /* 第三字段为电量：0~100 真实值，其它(含 255 哨兵)按未知 */
        const int tmp = parts.at(2).toInt();
        bat = (tmp >= 0 && tmp <= 100) ? tmp : -1;
    }
    int slot = -1;
    if (parts.size() > 3) {   /* 08-31: 第四字段为当前蓝牙槽位 1~3；旧固件无此字段 */
        const int tmp = parts.at(3).toInt();
        slot = (tmp >= 1 && tmp <= 3) ? tmp : -1;
    }

    /* ★ 11-23 只在"状态变化"时打印 —— 探测每 1.5s 一次，原来每次都打一行，
     * 日志里全是 `probe: device online ...`，把真正的问题淹没掉。 */
    static QString lastKey;
    const QString key = portName + "|" + version + "|" + QString::number(slot);
    if (key != lastKey) {
        lastKey = key;
        qDebug() << "probe: device online on" << portName << name << version
                 << "battery=" << bat << "slot=" << slot;
    }
    applyProbeResult(true, name, version, portName, bat);
    applyProbeSlot(slot);
}

void PortDetector::onProbeTimeout()
{
    if (!m_probeActive)
        return;
    /* 当前端口 800ms 内没回完整帧：关掉，试下一个候选 */
    m_probeIndex++;
    probeNext();
}

void PortDetector::onProbeError(QSerialPort::SerialPortError error)
{
    if (error == QSerialPort::NoError)
        return;
    if (!m_probeActive)
        return;
    m_probeTimeout->stop();
    m_probeIndex++;
    probeNext();
}

void PortDetector::applyProbeResult(bool probed, const QString &name,
                                    const QString &version, const QString &port, int bat)
{
    closeProbePort();
    m_probeActive = false;

    // 仅握手成功才刷新连接详情；端口在但本次未握手成功 -> 保持现状
    if (!probed)
        return;

    bool detailChanged = (!m_deviceConnected || name != m_deviceName
            || version != m_deviceVersion || port != m_devicePort);
    if (detailChanged) {
        m_deviceConnected = true;
        m_deviceName = name;
        m_deviceVersion = version;
        m_devicePort = port;
        emit deviceConnectedChanged();
        emit deviceNameChanged();
        emit deviceVersionChanged();
    }
    /* 电量独立刷新：即使 name/version 不变，电量变化也要及时更新显示 */
    if (bat != m_batteryPercent) {
        m_batteryPercent = bat;
        emit batteryPercentChanged();
    }
}

void PortDetector::applyProbeSlot(int slot)
{
    if (slot < 1 || slot > 3)
        slot = -1;   /* 非法/旧固件无此字段 -> 未知 */
    if (slot != m_deviceSlot) {
        m_deviceSlot = slot;
        emit deviceSlotChanged();
    }
}

void PortDetector::closeProbePort()
{
    /* blockSignals：close() 可能触发 errorOccurred，避免与状态机互相递归 */
    m_probePort->blockSignals(true);
    if (m_probePort->isOpen())
        m_probePort->close();
    m_probePort->blockSignals(false);
}

/* 立刻中止本轮探测并释放串口。
 * OTA / 配置读写会自己打开同一个端口，探测必须马上松手，否则对方 open 失败。 */
void PortDetector::abortProbe()
{
    m_probeTimeout->stop();
    m_probeActive = false;
    m_probeCandidates.clear();
    m_probeIndex = 0;
    m_probeResp.clear();
    closeProbePort();
}

void PortDetector::setPortInUse(bool inUse)
{
    if (inUse) {
        m_busyCount++;
        abortProbe();   /* 让出串口：OTA/配置读写随即要打开它 */
    } else {
        m_busyCount = qMax(0, m_busyCount - 1);
    }
}

void PortDetector::pausePolling()
{
    if (m_pollTimer)
        m_pollTimer->stop();
    abortProbe();   /* 彻底停探测并释放串口 */
    qDebug() << "probe: polling PAUSED (OTA/transfer in progress)";
}

void PortDetector::resumePolling()
{
    if (m_pollTimer)
        m_pollTimer->start();
    qDebug() << "probe: polling RESUMED";
}
