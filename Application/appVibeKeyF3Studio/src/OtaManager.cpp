#include "OtaManager.h"
#include "PortDetector.h"
#include "LightMonitor.h"
#include "ReleasesModel.h"
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QCoreApplication>
#include <QElapsedTimer>
#include <QStandardPaths>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonParseError>
#include <QDebug>

OtaManager::OtaManager(QObject *parent)
    : QObject(parent)
    , m_portDetector(nullptr)
    , m_otaProcess(nullptr)
    , m_netProcess(nullptr)
    , m_progress(0)
    , m_transferring(false)
    , m_verifyWarned(false)
    , m_pythonPath(findPython())
    , m_releasesModel(new ReleasesModel(this))
    , m_netBusy(false)
    , m_netProgress(-1)
{
}

QAbstractItemModel* OtaManager::releasesModel() const
{
    return m_releasesModel;   /* ReleasesModel* → QAbstractItemModel* 上行转换 */
}

/* ---- 09-13 固件新版本提醒 ---- */

void OtaManager::setPortDetector(PortDetector *detector)
{
    m_portDetector = detector;
    if (m_portDetector) {
        /* 设备插拔 / 固件版本变化 → 重算"是否有新固件"(在线列表已缓存, 不必重新联网) */
        connect(m_portDetector, &PortDetector::deviceVersionChanged,
                this, &OtaManager::refreshFwUpdateState);
        connect(m_portDetector, &PortDetector::deviceConnectedChanged,
                this, &OtaManager::refreshFwUpdateState);
    }
}

void OtaManager::setLightMonitor(LightMonitor *monitor)
{
    m_lightMonitor = monitor;
    if (m_lightMonitor) {
        /* 蓝牙连上 / 版本透传到达 → 重算"是否有新固件"(与 USB 同源, 仅连蓝牙也提醒) */
        connect(m_lightMonitor, &LightMonitor::bleDeviceVersionChanged,
                this, &OtaManager::refreshFwUpdateState);
        connect(m_lightMonitor, &LightMonitor::bleConnectedChanged,
                this, &OtaManager::refreshFwUpdateState);
    }
}

int OtaManager::cmpVersion(const QString &a, const QString &b)
{
    const QStringList pa = a.split('.', Qt::SkipEmptyParts);
    const QStringList pb = b.split('.', Qt::SkipEmptyParts);
    const int n = qMax(pa.size(), pb.size());
    for (int i = 0; i < n; ++i) {
        const int va = (i < pa.size()) ? pa[i].toInt() : 0;
        const int vb = (i < pb.size()) ? pb[i].toInt() : 0;
        if (va != vb) return va - vb;
    }
    return 0;   /* 相等 */
}

void OtaManager::checkFirmwareUpdate(bool silent)
{
    if (m_netBusy) return;                  /* 已有网络操作在跑, 跳过本次自检 */
    m_fwCheckSilent = silent;
    m_netProgress = -1;
    emit netProgressChanged();
    setNetBusy(true);
    if (!silent) setStatus("正在检查固件更新...");
    startNetWorker({"list"});               /* list 默认只列 .bin 固件 */
}

void OtaManager::downloadLatestFirmware()
{
    if (m_latestFwIndex < 0) {
        setStatus("暂无可用固件版本，请先刷新在线列表");
        return;
    }
    downloadRelease(m_latestFwIndex);
}

void OtaManager::refreshFwUpdateState()
{
    /* 在线列表里挑 tag=vX.Y.Z 中版本号最大的一条, 作为"最新在线固件" */
    QString best;
    int bestIdx = -1;
    const int n = m_releasesModel ? m_releasesModel->count() : 0;
    for (int i = 0; i < n; ++i) {
        const QVariantMap m = m_releasesModel->at(i);
        QString ver = m.value("tag").toString().trimmed();
        if (ver.startsWith('v') || ver.startsWith('V')) ver = ver.mid(1);
        if (ver.isEmpty()) continue;
        if (best.isEmpty() || cmpVersion(ver, best) > 0) { best = ver; bestIdx = i; }
    }

    const bool listChanged = (best != m_latestFwVersion) || (bestIdx != m_latestFwIndex);
    m_latestFwVersion = best;
    m_latestFwIndex = bestIdx;

    /* 与【已连接设备】固件版本比对: USB 优先; 只连蓝牙时用 BLE 透传的版本(与 USB 同源)。
     * 两者都没有(没连设备)则无从判断, 不提醒。 */
    QString cur;
    if (m_portDetector && !m_portDetector->deviceVersion().isEmpty())
        cur = m_portDetector->deviceVersion();
    else if (m_lightMonitor)
        cur = m_lightMonitor->bleDeviceVersion();
    const bool avail = (!best.isEmpty() && !cur.isEmpty() && cmpVersion(best, cur) > 0);
    const bool availChanged = (avail != m_fwUpdateAvailable);
    m_fwUpdateAvailable = avail;

    if (listChanged || availChanged) emit fwUpdateChanged();
    if (availChanged && avail) emit firmwareUpdateAvailable(best, cur);

    /* 设备刚上线但还没有在线列表(启动自检失败/尚未跑) → 补一次静默拉取, 保证
     * "连上设备(USB 或蓝牙)即能对比出固件有没有新版"。只尝试一次, 防反复联网。 */
    const bool online = (m_portDetector && m_portDetector->deviceConnected())
                        || (m_lightMonitor && m_lightMonitor->bleConnected());
    if (!m_fwAutoTried && n == 0 && !m_netBusy && online) {
        m_fwAutoTried = true;
        checkFirmwareUpdate(true);
    }
}

OtaManager::~OtaManager()
{
    cancelTransfer();
    if (m_netProcess && m_netProcess->state() != QProcess::NotRunning) {
        m_netProcess->kill();
        m_netProcess->waitForFinished(2000);
    }
    if (m_netProcess) {
        m_netProcess->deleteLater();
        m_netProcess = nullptr;
    }
}

QString OtaManager::findPython()
{
    // 优先使用安装包内自带的 Python（位于程序目录下的 python/）
    QString appDir = QCoreApplication::applicationDirPath();
    QStringList candidates = {
        appDir + "/python/python.exe",    // 安装包内自带的 Python（优先）
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
        if (test.waitForFinished(500)) {
            if (test.exitCode() == 0) {
                qDebug() << "Found Python at:" << path;
                return path;
            }
        }
    }

    qWarning() << "Python not found";
    return "python";
}

QString OtaManager::findScript(const QString &scriptName)
{
    // 在可执行文件目录和源码目录中查找 python worker 脚本
    QString appDir = QCoreApplication::applicationDirPath();
    QStringList dirs = {
        appDir,                                   // 可执行文件同目录
        appDir + "/workers",                      // 运行目录下的 workers/ 子目录
        appDir + "/..",                           // 上一级目录 (开发环境)
        appDir + "/../workers",
        appDir + "/../..",                        // 项目源码目录
        appDir + "/../../workers",
        appDir + "/../../..",                     // CMakeLists.txt 所在目录
        appDir + "/../../../workers"
    };
    for (const QString &dir : dirs) {
        QString p = dir + "/" + scriptName;
        if (QFile::exists(p)) return p;
    }
    qWarning() << scriptName << "not found in" << appDir;
    return scriptName;
}

QString OtaManager::findWorkerScript()
{
    return findScript("ota_worker.py");
}

void OtaManager::selectFirmware(const QString &filePath)
{
    QFile file(filePath);
    if (!file.open(QIODevice::ReadOnly)) {
        setStatus("无法读取固件文件");
        return;
    }

    QByteArray data = file.readAll();
    m_firmwarePath = filePath;
    file.close();

    QFileInfo info(filePath);
    m_firmwareInfo = QString("%1 (%2 字节)").arg(info.fileName()).arg(data.size());
    emit firmwareInfoChanged();

    setStatus(QString("已选择固件: %1").arg(info.fileName()));
    qDebug() << "Firmware loaded:" << data.size() << "bytes";
}

void OtaManager::startTransfer()
{
    if (m_transferring) {
        setStatus("正在传输中...");
        return;
    }

    if (m_firmwarePath.isEmpty()) {
        setStatus("请先选择固件文件");
        return;
    }

    // 端口完全由共享探测器决定（已自动识别到 VibeKey 设备）
    QString port = m_portDetector ? m_portDetector->devicePort() : QString();
    if (port.isEmpty()) {
        setStatus("未检测到设备，请先连接设备");
        return;
    }

    QString workerScript = findWorkerScript();
    QString python = m_pythonPath;

    setStatus("正在启动升级...");

    // 通知共享探测器：串口将被本进程占用，暂停探测避免抢端口
    if (m_portDetector) m_portDetector->setPortInUse(true);
    // 彻底停掉轮询定时器，避免 CONF 查询包混入固件流导致设备端 CRC 校验失败
    if (m_portDetector) m_portDetector->pausePolling();

    m_otaProcess = new QProcess(this);
    connect(m_otaProcess, &QProcess::readyReadStandardOutput, this, &OtaManager::onProcessReadyRead);
    connect(m_otaProcess, &QProcess::finished, this, &OtaManager::onProcessFinished);
    connect(m_otaProcess, &QProcess::errorOccurred, this, &OtaManager::onProcessError);

    m_otaProcess->start(python, {workerScript, port, m_firmwarePath});

    m_transferring = true;
    m_verifyWarned = false;   /* 新一次升级重置版本未变化标记 */
    m_progress = 0;
    emit transferringChanged();
    emit progressChanged();
}

void OtaManager::cancelTransfer()
{
    if (m_otaProcess && m_otaProcess->state() != QProcess::NotRunning) {
        m_otaProcess->kill();
        m_otaProcess->waitForFinished(3000);
    }

    if (m_otaProcess) {
        m_otaProcess->deleteLater();
        m_otaProcess = nullptr;
    }

    // 释放串口占用
    if (m_portDetector) m_portDetector->setPortInUse(false);
    if (m_portDetector) m_portDetector->resumePolling();

    if (m_transferring) {
        m_transferring = false;
        m_progress = 0;
        emit transferringChanged();
        emit progressChanged();
        setStatus("传输已取消");
    }
}

void OtaManager::onProcessReadyRead()
{
    if (!m_otaProcess) return;

    QByteArray output = m_otaProcess->readAllStandardOutput();
    QString text = QString::fromUtf8(output);

    QStringList lines = text.split("\n", Qt::SkipEmptyParts);
    for (const QString &line : lines) {
        QString trimmed = line.trimmed();
        if (trimmed.isEmpty()) continue;

        if (trimmed.startsWith("PROGRESS:")) {
            int pct = trimmed.mid(9).toInt();
            m_progress = pct;
            emit progressChanged();
        } else if (trimmed.startsWith("STATUS:")) {
            setStatus(trimmed.mid(7));
        } else if (trimmed.startsWith("ERROR:")) {
            m_transferring = false;
            emit transferringChanged();
            emit transferFailed(trimmed.mid(6));
        } else if (trimmed.startsWith("SIZE:")) {
            // 忽略
        } else if (trimmed.startsWith("VERIFY_OK:")) {
            QString ver = trimmed.mid(9);
            setStatus(QString("设备已重新上线，版本 %1").arg(ver));
        } else if (trimmed == "VERIFY_WARN_SAME_VERSION") {
            /* 设备活着但版本未变：固件未真正生效（典型如设备端 CRC 校验失败回退旧固件）。
             * ⚠️ 08-27 修复: 原来只 setStatus(界面未显示 statusMessage, 警告被吞),
             * 且后续 DONE 仍触发"升级完成" → 用户看到成功提示但版本没变。
             * 现在升级为失败信号(醒目提示)并抑制 DONE 的成功提示。 */
            m_verifyWarned = true;
            m_transferring = false;
            emit transferringChanged();
            emit transferFailed("升级未生效：设备版本未变化（固件校验可能失败），请重试");
        } else if (trimmed == "VERIFY_FAIL") {
            // 重握手超时：设备未重新上线，可能已变砖
            m_transferring = false;
            emit transferringChanged();
            emit transferFailed("升级后设备未重新上线，可能已变砖，请检查设备");
        } else if (trimmed == "DONE") {
            if (m_verifyWarned) return;   /* 已按失败提示处理, 不再发"升级完成" */
            m_progress = 100;
            emit progressChanged();
            m_transferring = false;
            emit transferringChanged();
            emit transferComplete();
        }
    }
}

void OtaManager::onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    qDebug() << "OTA process finished:" << exitCode << exitStatus;

    // 释放串口占用
    if (m_portDetector) m_portDetector->setPortInUse(false);
    if (m_portDetector) m_portDetector->resumePolling();

    if (m_otaProcess) {
        m_otaProcess->deleteLater();
        m_otaProcess = nullptr;
    }

    if (m_transferring) {
        m_transferring = false;
        emit transferringChanged();

        if (exitStatus == QProcess::CrashExit || exitCode != 0) {
            setStatus("升级进程异常退出");
            emit transferFailed("升级失败 (exit code: " + QString::number(exitCode) + ")");
        }
    }
}

void OtaManager::onProcessError(QProcess::ProcessError error)
{
    QString msg;
    switch (error) {
    case QProcess::FailedToStart:
        msg = "无法启动升级程序 (检查Python是否安装)";
        break;
    case QProcess::Crashed:
        msg = "升级程序崩溃";
        break;
    case QProcess::Timedout:
        msg = "升级程序超时";
        break;
    case QProcess::WriteError:
        msg = "写入错误";
        break;
    case QProcess::ReadError:
        msg = "读取错误";
        break;
    default:
        msg = "未知错误";
        break;
    }

    setStatus(msg);
    m_transferring = false;
    emit transferringChanged();
    emit transferFailed(msg);

    // 释放串口占用
    if (m_portDetector) m_portDetector->setPortInUse(false);
    if (m_portDetector) m_portDetector->resumePolling();

    if (m_otaProcess) {
        m_otaProcess->deleteLater();
        m_otaProcess = nullptr;
    }
}

/* ============ 09-05: 网络获取固件 (Gitee Releases) ============ */

void OtaManager::setNetBusy(bool busy)
{
    if (m_netBusy == busy) return;
    m_netBusy = busy;
    emit netBusyChanged();
}

void OtaManager::startNetWorker(const QStringList &args)
{
    if (m_netProcess) {
        m_netProcess->deleteLater();
        m_netProcess = nullptr;
    }
    QString script = findScript("net_worker.py");
    m_netProcess = new QProcess(this);
    connect(m_netProcess, &QProcess::readyReadStandardOutput, this, &OtaManager::onNetReadyRead);
    connect(m_netProcess, &QProcess::finished, this, &OtaManager::onNetFinished);
    connect(m_netProcess, &QProcess::errorOccurred, this, &OtaManager::onNetError);
    qDebug() << "net_worker start:" << script << args;
    m_netProcess->start(m_pythonPath, QStringList() << script << args);
}

void OtaManager::refreshReleases()
{
    if (m_netBusy) {
        setStatus("网络操作进行中...");
        return;
    }
    m_fwCheckSilent = false;   /* 手动刷新: 正常刷状态文案 */
    m_netProgress = -1;
    emit netProgressChanged();
    setNetBusy(true);
    setStatus("正在获取在线版本...");
    /* 10-05: 手动刷新传 force —— 绕过 net_worker 的 30min 缓存 TTL。
     * 本函数是**用户主动点击**触发的, 应当拿到实时结果;
     * 启动时的自动检查(checkFirmwareUpdate)则吃缓存, 以保护 Gitee 匿名配额。 */
    startNetWorker({"list", "bin", "force"});
}

void OtaManager::downloadRelease(int index)
{
    if (m_netBusy) {
        setStatus("网络操作进行中...");
        return;
    }
    if (index < 0 || index >= m_releasesModel->count()) {
        setStatus("请先选择要下载的版本");
        return;
    }
    QVariantMap rel = m_releasesModel->at(index);
    QString url = rel.value("url").toString();
    QString file = rel.value("file").toString();
    if (url.isEmpty()) {
        setStatus("该版本缺少下载地址");
        return;
    }
    if (file.isEmpty()) file = "firmware.bin";
    QString dest = QStandardPaths::writableLocation(QStandardPaths::TempLocation)
                   + "/vibekey_ota_" + file;
    m_netProgress = 0;
    emit netProgressChanged();
    setNetBusy(true);
    setStatus(QString("正在下载 %1 ...").arg(file));
    startNetWorker({"download", url, dest});
}

void OtaManager::onNetReadyRead()
{
    if (!m_netProcess) return;
    m_netStdout += m_netProcess->readAllStandardOutput();
    int idx;
    while ((idx = m_netStdout.indexOf('\n')) >= 0) {
        QByteArray lineB = m_netStdout.left(idx);
        m_netStdout.remove(0, idx + 1);
        QString line = QString::fromUtf8(lineB).trimmed();
        if (!line.isEmpty()) handleNetLine(line);
    }
}

void OtaManager::handleNetLine(const QString &trimmed)
{
    if (trimmed.startsWith("RELEASES_JSON:")) {
        parseReleases(trimmed.mid(14));
    } else if (trimmed.startsWith("DOWNLOAD_OK:")) {
        QString path = trimmed.mid(12);
        selectFirmware(path);                 // 复用本地固件校验/传输链路
        m_netProgress = -1;
        emit netProgressChanged();
        setNetBusy(false);
        setStatus("已下载固件，可点击「开始升级」");
    } else if (trimmed.startsWith("PROGRESS:")) {
        int pct = trimmed.mid(9).toInt();
        if (pct != m_netProgress) {
            m_netProgress = pct;
            emit netProgressChanged();
        }
    } else if (trimmed.startsWith("STATUS:")) {
        setStatus(trimmed.mid(7));
    } else if (trimmed.startsWith("ERROR:")) {
        m_netProgress = -1;
        emit netProgressChanged();
        setNetBusy(false);
        emit netFailed(trimmed.mid(6));
    }
}

void OtaManager::parseReleases(const QString &json)
{
    QJsonParseError perr;
    QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8(), &perr);
    if (perr.error != QJsonParseError::NoError || !doc.isArray()) {
        qWarning() << "parseReleases error:" << perr.errorString();
        setNetBusy(false);
        emit netFailed("在线版本列表解析失败");
        return;
    }
    /* 先构建本地列表, 再原子塞入模型(beginResetModel/endResetModel);
     * 避免在 QML 仍持有旧引用时反复 clear/append 触发 Qt 6.11 共享追加断言 */
    QList<QVariantMap> items;
    const QJsonArray arr = doc.array();
    for (const QJsonValue &v : arr) {
        if (!v.isObject()) continue;
        QJsonObject o = v.toObject();
        QVariantMap map;
        map["tag"]  = o.value("tag").toString();
        map["name"] = o.value("name").toString();
        map["date"] = o.value("date").toString();
        map["body"] = o.value("body").toString();
        map["file"] = o.value("file").toString();
        map["url"]  = o.value("url").toString();
        map["size"] = o.value("size").toDouble();
        items.append(map);
    }
    m_releasesModel->setReleases(items);
    /* 09-13: 刷新后重算"固件是否有新版本"(与已连接设备比对) */
    refreshFwUpdateState();
    if (!m_fwCheckSilent) {
        if (m_releasesModel->count() == 0) {
            setStatus("仓库暂无可用固件发布");
        } else {
            setStatus(QString("获取到 %1 个可用版本，请选择后下载").arg(m_releasesModel->count()));
        }
    }
    m_fwCheckSilent = false;   /* 复位: 后续默认非静默 */
    setNetBusy(false);
}

void OtaManager::onNetFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    if (m_netProcess) {
        m_netProcess->deleteLater();
        m_netProcess = nullptr;
    }
    // 若还处于忙碌态但没收到终止标记，说明进程异常退出
    if (m_netBusy) {
        m_netProgress = -1;
        emit netProgressChanged();
        setNetBusy(false);
        if (exitStatus == QProcess::CrashExit || exitCode != 0)
            emit netFailed(QString("网络操作异常退出 (exit code %1)").arg(exitCode));
    }
}

void OtaManager::onNetError(QProcess::ProcessError error)
{
    QString msg;
    switch (error) {
    case QProcess::FailedToStart: msg = "无法启动网络进程"; break;
    case QProcess::Crashed: msg = "网络进程崩溃"; break;
    default: msg = "网络进程错误"; break;
    }
    if (m_netProcess) {
        m_netProcess->deleteLater();
        m_netProcess = nullptr;
    }
    m_netProgress = -1;
    emit netProgressChanged();
    setNetBusy(false);
    emit netFailed(msg);
}

void OtaManager::setStatus(const QString &msg)
{
    m_statusMessage = msg;
    emit statusMessageChanged();
    qDebug() << "Status:" << msg;
}
