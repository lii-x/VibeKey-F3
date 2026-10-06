#include "LightMonitor.h"
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QDebug>
#include "PortDetector.h"

// 蓝牙连接判定去抖阈值：连续这么多次探测为“断开”才真正判定断开。
// BLE HID 设备空闲时无线电链接会瞬时掉线、有活动时重连，ConnectionStatus 会闪；
// 用迟滞避免 UI 反复“连接/断开”，也避免每次断开都 kill 掉常驻 BLE 进程导致灯控失效。
static const int kBleDiscThreshold = 2;

LightMonitor::LightMonitor(QObject *parent)
    : QObject(parent)
    , m_bleTimer(new QTimer(this))
    , m_bleProc(nullptr)
    , m_bleConnected(false)
    , m_portDetector(nullptr)
    , m_channel("none")
    , m_viaUsb(false)
    , m_process(nullptr)
    , m_bleServeProc(nullptr)
    , m_bleServeReady(false)
    , m_pendingServeState(-1)
{
    m_pythonPath = findPython();

    /* ★★ 10-04: 启动时清理上一会话遗留的**孤儿 worker 进程**。
     * 成因：exe 被强制结束（Qt Creator 停止按钮 / 任务管理器 / 崩溃）时，
     *       QProcess 析构不执行 ⇒ 子 python 进程成孤儿，且**持续占用 BLE 连接**；
     *       设备端同一时刻只允许一个 GATT 客户端 ⇒ 新会话的 serve / config worker
     *       一连就 AccessDenied ⇒ 界面"已连接"却"操作失败 / 无响应"。
     *       实测反例：孤儿 PID 12360(START 15:02) 与当前 exe(START 15:07) 并存时，
     *       serve 三次连接全部 `service 0xff00 denied`。
     * 用 PowerShell **异步**清理（startDetached 不阻塞启动）；
     * 只匹配本项目 worker 脚本名，不会误伤用户其它 python 进程。 */
#ifdef Q_OS_WIN
    /* ⚠️ 必须用 ParentProcessId 排除自己的 worker：清理是异步的，
     * 若只用脚本名匹配，可能把本进程刚启动的 serve 一起杀掉（父子关系是
     * 唯一可靠的区分依据 —— 孤儿的特点是它的父进程已经不在了）。 */
    const qint64 myPid = QCoreApplication::applicationPid();
    QProcess::startDetached(QStringLiteral("powershell"), {
        QStringLiteral("-NoProfile"), QStringLiteral("-WindowStyle"), QStringLiteral("Hidden"),
        QStringLiteral("-Command"),
        QStringLiteral("Get-CimInstance Win32_Process -Filter \"name='python.exe'\" | "
                       "Where-Object { $_.CommandLine -match "
                       "'ble_led_worker\\.py|ble_config_worker\\.py|net_worker\\.py|ota_worker\\.py' "
                       "-and $_.ParentProcessId -ne %1 } | "
                       "ForEach-Object { Stop-Process -Id $_.ProcessId -Force "
                       "-ErrorAction SilentlyContinue }").arg(myPid)});
#endif

    // 定时探测 VibeKey 蓝牙设备是否已配对/在线（手动测试时用于通道选择）
    m_bleTimer->setInterval(3000);
    connect(m_bleTimer, &QTimer::timeout, this, &LightMonitor::checkBleConnection);
    m_bleTimer->start();
    checkBleConnection(); // 立即探一次

    // 09-03: serve 延迟恢复(见 resumeBleServe 注释)。singleShot 天然防抖:
    // 若 2s 窗口内又 pause(新一轮配置), 定时器被停掉, 不会与配置 worker 抢连接。
    m_serveResumeTimer.setSingleShot(true);
    m_serveResumeTimer.setInterval(2000);
    connect(&m_serveResumeTimer, &QTimer::timeout, this, [this]() {
        if (!m_bleServeProc && m_bleConnected) {
            qDebug() << "[LightMonitor] BLE serve 延迟恢复: 启动";
            startBleServe();
        }
    });
}

void LightMonitor::setBleDeviceVersion(const QString &v)
{
    if (v != m_bleDeviceVersion) {
        m_bleDeviceVersion = v;
        emit bleDeviceVersionChanged();
    }
}

LightMonitor::~LightMonitor()
{
    m_bleTimer->stop();
    /* 10-04: kill() 只是发起终止，**不保证进程已退出**。直接等一下确认，
     * 避免关程序时留下孤儿 python 进程持续占用 BLE 连接
     * （孤儿会与下次启动的 serve / config worker 抢连接 ⇒ AccessDenied）。 */
    if (m_bleProc && m_bleProc->state() != QProcess::NotRunning) {
        m_bleProc->kill();
        m_bleProc->waitForFinished(1000);
    }
    stopBleServe();
    if (m_process && m_process->state() != QProcess::NotRunning) {
        m_process->kill();
        m_process->waitForFinished(1000);
    }
}

QString LightMonitor::findPython()
{
    // 优先使用程序目录下的 python/（绿色版捆绑的 Python）
    QString appDir = QCoreApplication::applicationDirPath();
    QStringList candidates = {
        appDir + "/python/python.exe",
        appDir + "/python/python3.exe",
        "python",
        "python3",
        "C:\\Python312\\python.exe",
        "C:\\Python311\\python.exe",
        "C:\\Python310\\python.exe",
        "C:\\Users\\" + qEnvironmentVariable("USERNAME") + "\\AppData\\Local\\Programs\\Python\\Python312\\python.exe",
        "C:\\Users\\" + qEnvironmentVariable("USERNAME") + "\\AppData\\Local\\Programs\\Python\\Python311\\python.exe",
        "C:\\Users\\" + qEnvironmentVariable("USERNAME") + "\\AppData\\Local\\Programs\\Python\\Python310\\python.exe"
    };
    for (const QString &path : candidates) {
        QProcess test;
        test.start(path, {"--version"});
        if (test.waitForFinished(1000) && test.exitCode() == 0)
            return path;
    }
    return "python";
}

QString LightMonitor::findWorkerScript()
{
    QString appDir = QCoreApplication::applicationDirPath();
    QString scriptName = "config_worker.py";
    QStringList searchPaths = {
        appDir + "/" + scriptName,
        appDir + "/workers/" + scriptName,
        appDir + "/../" + scriptName,
        appDir + "/../workers/" + scriptName,
        appDir + "/../../" + scriptName,
        appDir + "/../../../" + scriptName,
    };
    for (const QString &p : searchPaths) {
        if (QFile::exists(p)) return p;
    }
    return scriptName;
}

QString LightMonitor::findBleWorkerScript()
{
    QString appDir = QCoreApplication::applicationDirPath();
    QString scriptName = "ble_led_worker.py";
    QStringList searchPaths = {
        appDir + "/" + scriptName,
        appDir + "/workers/" + scriptName,
        appDir + "/../" + scriptName,
        appDir + "/../workers/" + scriptName,
        appDir + "/../../" + scriptName,
        appDir + "/../../../" + scriptName,
    };
    for (const QString &p : searchPaths) {
        if (QFile::exists(p)) return p;
    }
    return scriptName;
}

void LightMonitor::setPortDetector(PortDetector *detector)
{
    m_portDetector = detector;
    if (m_portDetector) {
        // USB 连接状态变化（插拔/握手）时刷新当前通道显示
        connect(m_portDetector, &PortDetector::deviceConnectedChanged,
                this, &LightMonitor::updateChannel);
    }
    updateChannel();
}

void LightMonitor::updateChannel()
{
    QString ch = m_bleConnected
            ? QStringLiteral("ble")
            : (m_portDetector && m_portDetector->deviceConnected()
                   ? QStringLiteral("usb")
                   : QStringLiteral("none"));
    if (ch != m_channel) {
        m_channel = ch;
        emit channelChanged();
    }
    // 蓝牙可用时保持常驻 BLE 连接（手动测试走它，省去重连）；断开即关闭
    if (m_channel == QStringLiteral("ble")) {
        /* 09-05: 键配置独占期间不自动拉起 serve —— serve 与 config worker 的
         * GATT 会话并发会让双方的服务发现都拿到空结果(设备端单客户端限制),
         * 并触发"serve 失败→这里复活→再撞车"的循环。恢复由 resumeBleServe
         * 的 2s 延迟定时器负责。 */
        if (!m_bleServeProc && !m_serveSuppressed) startBleServe();
    } else {
        if (m_bleServeProc) stopBleServe();
    }
}

void LightMonitor::startBleServe()
{
    if (m_bleServeProc) return;
    m_bleServeProc = new QProcess(this);
    m_bleServeReady = false;
    m_pendingServeState = -1;
    m_serveBuf.clear();

    connect(m_bleServeProc, &QProcess::finished, this, &LightMonitor::onBleServeFinished);
    connect(m_bleServeProc, &QProcess::readyReadStandardOutput, this, [this]() {
        if (!m_bleServeProc) return;
        m_serveBuf.append(m_bleServeProc->readAllStandardOutput());
        int nl;
        while ((nl = m_serveBuf.indexOf('\n')) >= 0) {
            QByteArray line = m_serveBuf.left(nl).trimmed();
            m_serveBuf.remove(0, nl + 1);
            if (line == "READY") {
                if (!m_bleServeReady)
                    emit bleServeReadyChanged();   /* 10-04: 通知等待方可以动作了 */
                m_bleServeReady = true;
                if (m_pendingServeState >= 0 && m_bleServeProc
                        && m_bleServeProc->state() == QProcess::Running) {
                    m_bleServeProc->write(QString("led %1\n").arg(m_pendingServeState).toUtf8());
                    m_lastSentState = m_pendingServeState;
                    m_pendingServeState = -1;
                }
            } else if (line == "LOST") {
                /* 常驻进程检测到真实断连：立即改判断开，见 onServeLinkLost()。 */
                onServeLinkLost();
            } else if (line.startsWith("OK") || line.startsWith("ERROR")) {
                qDebug() << "[LightMonitor] BLE serve:" << line;
            }
        }
    });
    connect(m_bleServeProc, &QProcess::readyReadStandardError, this, [this]() {
        QByteArray err = m_bleServeProc->readAllStandardError();
        if (!err.isEmpty())
            qDebug() << "[LightMonitor] BLE serve stderr:" << QString::fromUtf8(err).trimmed();
    });

    QString appDir = QCoreApplication::applicationDirPath();
    m_bleServeProc->setWorkingDirectory(appDir);
    QString worker = findBleWorkerScript();
    qDebug() << "[LightMonitor] 启动 BLE 常驻进程:" << worker;
    m_bleServeProc->start(m_pythonPath, {worker, "serve"});
}

void LightMonitor::stopBleServe()
{
    if (!m_bleServeProc) return;
    m_bleServeReady = false;
    m_pendingServeState = -1;
    // 断开该进程到本对象的信号，避免 finished 清理与下方手动清理重复
    disconnect(m_bleServeProc, nullptr, this, nullptr);
    if (m_bleServeProc->state() == QProcess::Running) {
        m_bleServeProc->write("exit\n");
        /* 09-03: 先等进程正常退出(serve 退出前会 close BLE 设备对象, 让
         * Windows 干净释放连接); 强杀会留半开句柄污染后续 GATT 发现。
         * 09-05: 等待窗口 800ms→3s —— serve 可能正卡在服务发现的重试循环里
         * (每轮最长 ~1s), 短窗口会在它握着 GATT 设备对象时强杀, 实测会
         * 把 Windows 的 GATT 缓存污染成"服务在/特征为空", 紧跟着的键配置
         * worker 就撞上 "cfg chars not found"(0905 实测复现)。
         *
         * ⚠️ 10-04 回退：曾把这里改成 600ms+2400ms 分段"提速"，是**错的** ——
         *   QProcess::waitForFinished(ms) 在进程真正结束时就**立即返回**，
         *   根本不会阻塞满整个 ms，所以分段相比单次 3000ms **没有任何提速**。
         *   而实测（10-04）分段版本反而更容易撞上 AccessDenied：serve 未退干净时
         *   就放行下一个 GATT 客户端，两边同时做服务发现 → 设备端只允许一个
         *   客户端 ⇒ 双方都 AccessDenied（0xFF00 服务 denied / cfg chars not found）。
         *   保留原样，勿再"优化"这里。 */
        if (!m_bleServeProc->waitForFinished(3000)
                && m_bleServeProc->state() != QProcess::NotRunning) {
            m_bleServeProc->kill();    // Windows 下 terminate
            /* 10-04 补漏：kill() 只是【发起】终止，**不保证进程已经真的没了**。
             * 之前 kill 完直接 deleteLater()+置空，QProcess 随即析构 ——
             * 若此刻子进程尚未退出，就没人再管它了 ⇒ 变成**孤儿 serve**，
             * 持续占着 BLE 连接不放 ⇒ 上位机自己的新 serve / config worker
             * 一连上去就 AccessDenied（日志实测：孤儿 START=14:27:21，
             * 而新 exe 14:37 才启动，旧的一直在）。
             * ⇒ kill 后必须再等它真正结束，确认状态为 NotRunning。
             * 1s 窗口足够（terminate 是异步的，实测 <100ms 退出）。 */
            if (!m_bleServeProc->waitForFinished(1000))
                qWarning("[LightMonitor] BLE serve kill 未在 1s 内退出，"
                         "可能残留孤儿进程占用 BLE 连接");
        }
    }
    m_bleServeProc->deleteLater();
    m_bleServeProc = nullptr;
}

/* 09-03 (BLE 改建键配置): 暂停常驻 serve, 让配置 worker 独占 BLE 连接。
 * 复用 stopBleServe —— 只杀进程, 不动 m_bleConnected/channel, 恢复由 resume 完成。
 * 同时停掉未触发的延迟恢复定时器, 避免上一轮恢复与新一轮配置抢连接。 */
void LightMonitor::pauseBleServe()
{
    m_serveResumeTimer.stop();
    m_serveSuppressed = true;   /* 09-05: updateChannel 在独占期间不得复活 serve */
    if (!m_bleServeProc) return;
    qDebug() << "[LightMonitor] 暂停 BLE serve(键配置独占连接)";
    stopBleServe();
}

/* ★ 10-04: serve 是否可直接下发 —— 必须进程在跑 **且** 已收到 READY。
 * 与 sendState 用同一判据（不用 m_bleConnected：空闲掉线瞬间的抖动会让按钮点了没反应）。 */
bool LightMonitor::bleServeUsable() const
{
    return m_bleServeProc && m_bleServeReady
           && m_bleServeProc->state() == QProcess::Running;
}

/* ★ 10-04: 通过常驻 serve 下发 RGB 预览 —— 毫秒级。
 * 协议与 ble_led_worker.py 的 serve reader 对应:
 *     rgb <idx> <en> <r> <g> <b> <bri>
 * 走这条路的**前提**：serve 初次连接时已把 0xFF03 配置特征一并取到
 * （ble_led_worker.py 的 connect_led_char 返回 (led_ch, cfg_ch)）。
 * 好处：不启新进程、不重做 GATT 发现、**也不需要 pauseBleServe 独占连接**。
 * 返回 false 时调用方应回退到一次性 worker 路径（保证兼容性）。 */
bool LightMonitor::sendRgbViaServe(int ledIndex, bool enabled, int r, int g, int b, int brightnessPct)
{
    if (!bleServeUsable()) {
        qDebug() << "[LightMonitor] sendRgbViaServe: serve 不可用，回退一次性 worker";
        return false;
    }
    const QString line = QString("rgb %1 %2 %3 %4 %5 %6\n")
                             .arg(ledIndex).arg(enabled ? 1 : 0)
                             .arg(r).arg(g).arg(b).arg(brightnessPct);
    m_bleServeProc->write(line.toUtf8());
    qDebug() << "[LightMonitor] RGB via serve:" << line.trimmed();
    return true;
}

/* 09-03: 键配置完成后的恢复入口。不立即重启 —— Windows 释放上一进程的 BLE
 * 连接有 1~2s 延迟, 立刻重连会撞上释放窗口(E_INVALIDARG/特征表空, 实测),
 * 统一延迟 2s 再启(见 m_serveResumeTimer)。仅当蓝牙仍判定已连接才安排恢复。 */
void LightMonitor::resumeBleServe()
{
    m_serveSuppressed = false;        /* 09-05: 配置会话结束, 允许自动拉起 */
    if (m_bleServeProc) return;       // 已在运行(无需恢复)
    if (!m_bleConnected) return;      // 配置期间已断开, 不安排
    qDebug() << "[LightMonitor] BLE serve 延迟 2s 恢复";
    m_serveResumeTimer.start();
}

void LightMonitor::onBleServeFinished(int exitCode, QProcess::ExitStatus)
{
    qDebug() << "[LightMonitor] BLE serve 退出 code=" << exitCode;
    if (m_bleServeProc) {
        m_bleServeProc->deleteLater();
        m_bleServeProc = nullptr;
    }
    m_bleServeReady = false;
    m_pendingServeState = -1;
    // 兜底：进程以"断连退出码 2"结束时，即便 LOST 行因缓冲原因没解析到，
    // 也要改判断开（比如 stdout 与进程退出竞争、行未及时送达）。
    if (exitCode == 2)
        onServeLinkLost();
}

/* 常驻 BLE 进程上报真实断连（连续多次 ConnectionStatus 非 CONNECTED）。
 *
 * 背景：本类原先以"常驻 serve 进程 READY"作为连接状态的唯一权威
 * （checkBleConnection / onBleCheckFinished 的 serveAuthoritative 分支），
 * 用意是避开 BLE HID 空闲瞬时掉线导致的 UI 反复横跳。但 serve 进程只会在收到
 * exit 命令或启动失败时退出 —— 设备断电 / 走远 / 休眠都不会让它退出，于是
 * m_bleConnected 长期停在 true，表现为"设备断连了还显示已连接"。
 * 现在由 serve 侧主动盯链路并上报 LOST，这里收到后立即改判断开；
 * 进程随后自行退出，清理由 onBleServeFinished 负责。 */
void LightMonitor::onServeLinkLost()
{
    qDebug() << "[LightMonitor] BLE serve 上报链路断开 (LOST)";
    m_bleServeReady = false;
    // 置到阈值：断开侧立刻生效（无需再等 kBleDiscThreshold 次探测），
    // 而恢复仍走"一次 CONNECTED 即判定已连接"，保持原有的防抖语义不变。
    m_bleDiscStreak = kBleDiscThreshold;

    if (m_bleConnected) {
        m_bleConnected = false;
        m_bleDeviceName.clear();
        m_bleDeviceVersion.clear();
        emit bleConnectedChanged();
        emit bleDeviceNameChanged();
        emit bleDeviceVersionChanged();
        updateChannel();
        qDebug() << "[LightMonitor] BLE DISCONNECTED (link lost)";
    }
}

void LightMonitor::sendState(int state)
{
    m_desiredState = state;   // 始终记住最新想要的状态，供 USB 发送完成后补发

    // 蓝牙通道优先：常驻 BLE 进程已真正就绪（握住设备连接）就立即写 stdin，
    // 不受下方 USB 发送占用的影响。用 serve 就绪作为判据，而非瞬时轮询的
    // m_bleConnected，避免空闲掉线瞬间的“断开”抖动让按钮点了没反应。
    if (m_bleServeProc && m_bleServeReady) {
        // 防 Debug 断言/崩溃：必须确认进程确实还在运行才写 stdin；
        // 否则说明 serve 已异常退出但 finished 信号尚未处理，重置 ready 标志并走兜底。
        if (m_bleServeProc->state() == QProcess::Running) {
            m_bleServeProc->write(QString("led %1\n").arg(state).toUtf8());
            m_viaUsb = false;
            m_lastSentState = state;
            qDebug() << "[LightMonitor] 发送状态 state=" << state << "通道=蓝牙(BLE)";
            return;
        }
        // serve 已不在运行：重置 ready 标志，落到下方 USB 兜底，避免状态被静默丢弃
        qDebug() << "[LightMonitor] BLE serve 已不在运行，重置 ready 标志，改走 USB 兜底，state=" << state;
        m_bleServeReady = false;
    }

    // USB 通道：若上一次发送（config_worker.py 单次约 1~2s）仍在飞行中，
    // 不再直接丢弃，而是标记有更新待补发，等本次发送结束后再下发最新状态。
    // 否则任务完成→idle 等快速连续变化会丢失，导致页面已空闲但灯不变。
    if (m_process && m_process->state() != QProcess::NotRunning) {
        m_usbPending = true;
        qDebug() << "[LightMonitor] USB 发送进行中，缓存最新 state=" << state << "（完成后补发）";
        return;
    }

    // 蓝牙显示已连接但 serve 没启动/没就绪：启动/保持 serve，让后续点击能走蓝牙，
    // 同时仍走下方 USB 兜底立刻发出，保证一定有反应。
    if (m_bleConnected && !m_bleServeProc) {
        startBleServe();
        m_pendingServeState = state;
        qDebug() << "[LightMonitor] BLE serve 未启动，已启动并缓存 state=" << state;
    } else if (m_bleConnected && m_bleServeProc && !m_bleServeReady) {
        // 已经在启动中，只保留最新一次点击
        m_pendingServeState = state;
        qDebug() << "[LightMonitor] BLE serve 启动中，缓存 state=" << state;
    }

    // 兜底：只要 USB 已连接，立刻用 USB 发出去，保证按钮一定有反应。
    // 蓝牙 serve 后续就绪后，再点击的按钮会走蓝牙。
    QString port = m_portDetector ? m_portDetector->devicePort() : QString();
    if (port.isEmpty()) {
        qDebug() << "[LightMonitor] 发送失败：USB 未连接且蓝牙未就绪，state=" << state;
        return;
    }

    // 占用串口，避免探测线程抢端口造成冲突
    if (m_portDetector) m_portDetector->setPortInUse(true);
    m_viaUsb = true;

    m_process = new QProcess(this);
    connect(m_process, &QProcess::finished, this, &LightMonitor::onProcessFinished);
    connect(m_process, &QProcess::readyReadStandardError, m_process, [this, proc = m_process]() {
        if (proc == m_process)
            m_stderrBuf.append(proc->readAllStandardError());
    });
    connect(m_process, &QProcess::readyReadStandardOutput, m_process, [this, proc = m_process]() {
        if (proc == m_process)
            m_stdoutBuf.append(proc->readAllStandardOutput());
    });

    QString appDir = QCoreApplication::applicationDirPath();
    m_process->setWorkingDirectory(appDir);
    QStringList fullArgs;
    fullArgs << findWorkerScript() << port << "led" << QString::number(state);
    qDebug() << "[LightMonitor] 发送状态 state=" << state << "通道=USB(" + port + ")";
    m_lastSentState = state;
    m_process->start(m_pythonPath, fullArgs);
}

void LightMonitor::setPortName(const QString &port)
{
    m_portName = port;
}

void LightMonitor::refreshBle()
{
    checkBleConnection();
}

/* 探测 VibeKey 蓝牙设备是否已配对/在线。通过独立的 QProcess 运行
 * ble_led_worker.py status，避免与 LED 写入进程（m_process）互相干扰。 */
void LightMonitor::checkBleConnection()
{
    // 常驻 BLE 进程已真正握住链路(READY)：它就是连接状态的唯一权威。
    // 此时不再跑 status 探测——既避免每 3s 开/关设备造成的无线电链接抖动
    // （弱适配器上尤其明显，正是“显示断开但实际连着”的根源），也避免 racy 的
    // ConnectionStatus 轮询把 m_bleConnected 误判为断开、进而 kill 掉常驻进程。
    if (m_bleServeProc && m_bleServeReady) {
        if (!m_bleConnected) {
            m_bleConnected = true;
            m_bleDeviceName = QStringLiteral("VibeKey-F3");
            emit bleConnectedChanged();
            emit bleDeviceNameChanged();
            updateChannel();
        }
        return;
    }

    if (m_bleProc && m_bleProc->state() != QProcess::NotRunning)
        return;

    m_bleProc = new QProcess(this);
    connect(m_bleProc, &QProcess::finished, this, &LightMonitor::onBleCheckFinished);

    QString worker = findBleWorkerScript();
    QString appDir = QCoreApplication::applicationDirPath();
    m_bleProc->setWorkingDirectory(appDir);
    qDebug() << "[LightMonitor] BLE status check";
    m_bleProc->start(m_pythonPath, {worker, "status"});
}

void LightMonitor::onBleCheckFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    Q_UNUSED(exitStatus);
    bool connected = false;
    QString name;
    int battery = -1;

    if (m_bleProc) {
        QByteArray out = m_bleProc->readAllStandardOutput();
        QString output = QString::fromUtf8(out).trimmed();
        qDebug() << "[LightMonitor] BLE status exit=" << exitCode << "out=" << output;
        // 新格式：CONNECTED <battery>（battery 为 0-100 或 -1）；旧格式 CONNECTED 也兼容
        if (output.startsWith("CONNECTED", Qt::CaseInsensitive)) {
            connected = true;
            name = "VibeKey-F3";
            QStringList parts = output.split(' ');
            if (parts.size() > 1) {
                int tmp = parts[1].toInt();
                battery = (tmp >= 0 && tmp <= 100) ? tmp : -1;
            }
        }
        m_bleProc->deleteLater();
        m_bleProc = nullptr;
    }

    // 去抖（迟滞）：出现一次 CONNECTED 立即判定为已连接并清零计数；
    // 只有连续 kBleDiscThreshold 次 DISCONNECTED 才真正判定为断开。
    //
    // 关键修正：当常驻 BLE 进程已真正握住链路(READY)时，以它为唯一权威——
    // 忽略 racy 的 status 轮询结果。BLE HID 设备空闲时无线电链接会瞬时掉线，
    // ConnectionStatus 会闪，但 serve 进程持稳的那条 GATT 链接才是“实际连着”
    // 的真实体现。否则轮询的瞬时 DISCONNECTED 会把 m_bleConnected 误判断开，
    // 进而触发 stopBleServe() 杀掉常驻进程、灯控随之失效，并让 UI 反复横跳。
    bool serveAuthoritative = (m_bleServeProc && m_bleServeReady);
    bool effective = false;
    if (serveAuthoritative) {
        // serve 握着真实链路：设备名沿用已设的 "VibeKey-F3"，避免轮询偶发
        // DISCONNECTED(空 name) 把显示名清空。
        effective = true;
        name = QStringLiteral("VibeKey-F3");
        m_bleDiscStreak = 0;
    } else if (connected) {
        m_bleDiscStreak = 0;
        effective = true;
    } else if (m_bleConnected) {
        // 曾判定连接：迟滞去抖 —— 连续 kBleDiscThreshold 次 DISCONNECTED 才真正
        // 断开（BLE HID 空闲瞬时掉线会闪 ConnectionStatus，避免 UI 反复横跳）。
        ++m_bleDiscStreak;
        effective = (m_bleDiscStreak < kBleDiscThreshold);
    } else {
        // 从未连上（或已确认断开）：本次结果即权威，不再攒断开计数。
        // 修复: 启动即无 BLE 时, 首次 DISCONNECTED 被去抖成"仍连接"
        // (streak 0→1 < 阈值2) → 误显示已连接并触发 BLE 自动读/拉起 serve。
        // 去抖只对"曾经连着、可能瞬时抖动"有意义; 从未连接就无从谈抖动。
        effective = false;
    }

    if (effective != m_bleConnected || (effective && name != m_bleDeviceName)) {
        m_bleConnected = effective;
        m_bleDeviceName = effective ? name : QString();
        emit bleConnectedChanged();
        emit bleDeviceNameChanged();
        updateChannel();
        qDebug() << "[LightMonitor] BLE" << (m_bleConnected ? "CONNECTED" : "DISCONNECTED")
                 << "(discStreak=" << m_bleDiscStreak << ")";
    }

    // 电量独立刷新：即使连接状态没变，电量变化也要及时更新显示
    if (m_bleBatteryPercent != battery) {
        m_bleBatteryPercent = battery;
        emit bleBatteryPercentChanged();
    }
}

void LightMonitor::onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    Q_UNUSED(exitStatus);
    // 若上一次发送走的是 USB，结束后释放串口占用，让探测线程恢复
    if (m_viaUsb) {
        if (m_portDetector) m_portDetector->setPortInUse(false);
        m_viaUsb = false;
    }

    QByteArray stdout_data = m_process->readAllStandardOutput();
    QByteArray stderr_data = m_stderrBuf;
    m_stderrBuf.clear();
    m_stdoutBuf.clear();

    QString output = QString::fromUtf8(stdout_data).trimmed();
    QString errOutput = QString::fromUtf8(stderr_data).trimmed();

    qDebug() << "[LightMonitor] exit=" << exitCode << "stdout=" << output << "stderr=" << errOutput;

    if (exitCode != 0 || output.contains("ERROR")) {
        QString msg = output.isEmpty() ? errOutput : output;
        qDebug() << "[LightMonitor] LED failed:" << msg;
    }

    m_process->deleteLater();
    m_process = nullptr;

    // 补发 USB 发送期间被合并的最新状态。例：任务完成→idle 的绿灯下发若落在
    // 上一次（busy 红灯）USB 发送窗口内被缓存，此处统一补发，确保灯最终与状态一致。
    if (m_usbPending) {
        m_usbPending = false;
        if (m_desiredState != m_lastSentState)
            sendState(m_desiredState);
    }
}
