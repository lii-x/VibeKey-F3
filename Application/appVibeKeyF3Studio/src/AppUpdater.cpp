#include "AppUpdater.h"
#include "AppVersion.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonDocument>
#include <QJsonArray>
#include <QJsonObject>
#include <QJsonParseError>
#include <QStandardPaths>
#include <QVariantList>
#include <QVariantMap>
#include <QDebug>

AppUpdater::AppUpdater(QObject *parent)
    : QObject(parent)
    , m_currentVersion(QLatin1String(VIBEKEY_STUDIO_VERSION))
    , m_pythonPath(findPython())
{
}

AppUpdater::~AppUpdater()
{
    if (m_checkProcess) {
        m_checkProcess->disconnect(this);
        m_checkProcess->kill();
        m_checkProcess->waitForFinished(1000);
        m_checkProcess->deleteLater();
    }
    if (m_downloadProcess) {
        m_downloadProcess->disconnect(this);
        m_downloadProcess->kill();
        m_downloadProcess->waitForFinished(1000);
        m_downloadProcess->deleteLater();
    }
}

QString AppUpdater::findPython()
{
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
    };
    for (const QString &path : candidates) {
        QProcess test;
        test.start(path, {"--version"});
        if (test.waitForFinished(500) && test.exitCode() == 0) {
            qDebug() << "AppUpdater: Python at" << path;
            return path;
        }
    }
    qWarning() << "AppUpdater: Python not found";
    return "python";
}

QString AppUpdater::findScript(const QString &scriptName)
{
    QString appDir = QCoreApplication::applicationDirPath();
    QStringList dirs = {
        appDir,
        appDir + "/workers",
        appDir + "/..",
        appDir + "/../workers",
        appDir + "/../..",
        appDir + "/../../workers",
        appDir + "/../../..",
        appDir + "/../../../workers",
    };
    for (const QString &dir : dirs) {
        QString p = dir + "/" + scriptName;
        if (QFile::exists(p))
            return p;
    }
    qWarning() << scriptName << "not found near" << appDir;
    return scriptName;
}

void AppUpdater::setBusy(bool busy)
{
    if (m_busy == busy)
        return;
    m_busy = busy;
    emit busyChanged();
}

void AppUpdater::checkNow(bool silent)
{
    if (m_busy) {
        qDebug() << "AppUpdater: already busy";
        return;
    }
    m_silentCheck = silent;
    m_checkBuf.clear();
    setBusy(true);
    if (!m_latestVersion.isEmpty()) {
        m_latestVersion.clear();
        emit latestVersionChanged();
    }

    QString script = findScript("net_worker.py");
    m_checkProcess = new QProcess(this);
    connect(m_checkProcess, &QProcess::readyReadStandardOutput, this, &AppUpdater::onCheckReadyRead);
    connect(m_checkProcess, &QProcess::finished, this, &AppUpdater::onCheckFinished);
    connect(m_checkProcess, &QProcess::errorOccurred, this, &AppUpdater::onCheckError);
    qDebug() << "AppUpdater: check start" << script;
    /* 10-05: silent(启动自动检查) 走 net_worker 的缓存 TTL, 不每次都打 Gitee;
     *        非 silent(托盘菜单手动"检查更新") 传 force 绕过缓存拿实时结果。 */
    if (silent)
        m_checkProcess->start(m_pythonPath, {script, "list", "setup"});
    else
        m_checkProcess->start(m_pythonPath, {script, "list", "setup", "force"});
}

void AppUpdater::installUpdate()
{
    if (m_busy)
        return;
    if (m_latestUrl.isEmpty() || m_latestFile.isEmpty()) {
        emit updateFailed("没有可用的更新(请先点\"检查更新\")");
        return;
    }
    QString dest = QStandardPaths::writableLocation(QStandardPaths::TempLocation)
                   + "/" + m_latestFile;
    m_downloadProgress = 0;
    emit downloadProgressChanged();
    m_downloadBuf.clear();
    setBusy(true);

    QString script = findScript("net_worker.py");
    m_downloadProcess = new QProcess(this);
    connect(m_downloadProcess, &QProcess::readyReadStandardOutput, this, &AppUpdater::onDownloadReadyRead);
    connect(m_downloadProcess, &QProcess::finished, this, &AppUpdater::onDownloadFinished);
    connect(m_downloadProcess, &QProcess::errorOccurred, this, &AppUpdater::onDownloadError);
    m_downloadProcess->start(m_pythonPath, {script, "download", m_latestUrl, dest});
}

void AppUpdater::onCheckReadyRead()
{
    if (!m_checkProcess)
        return;
    m_checkBuf += m_checkProcess->readAllStandardOutput();
    int idx;
    while ((idx = m_checkBuf.indexOf('\n')) >= 0) {
        QByteArray lineB = m_checkBuf.left(idx).trimmed();
        m_checkBuf.remove(0, idx + 1);
        if (!lineB.isEmpty())
            handleCheckLine(QString::fromUtf8(lineB));
    }
}

void AppUpdater::handleCheckLine(const QString &line)
{
    if (line.startsWith("RELEASES_JSON:")) {
        QString err;
        QVariantMap best = parseReleasesLine(line.mid(14), &err);
        if (!err.isEmpty()) {
            if (!m_silentCheck)
                emit checkFailed(err);
            setBusy(false);            // 列表拉到了但解析失败: 复位忙碌即可, 别误报"已最新"
            return;
        }
        QString ver = best.value("ver").toString();
        if (!ver.isEmpty() && compareVersions(ver, m_currentVersion) > 0) {
            m_latestVersion = ver;
            m_latestFile = best.value("file").toString();
            m_latestUrl = best.value("url").toString();
            m_latestBody = best.value("body").toString();
            emit latestVersionChanged();
        } else {
            m_latestVersion.clear();
        }
    }
}

QVariantMap AppUpdater::parseReleasesLine(const QString &json, QString *err)
{
    QVariantMap empty;
    QJsonParseError perr;
    QJsonDocument doc = QJsonDocument::fromJson(json.toUtf8(), &perr);
    if (perr.error != QJsonParseError::NoError || !doc.isArray()) {
        *err = "在线版本列表解析失败";
        return empty;
    }
    const QJsonArray arr = doc.array();
    QVariantMap best;
    QString bestVer;
    for (const QJsonValue &v : arr) {
        if (!v.isObject())
            continue;
        QVariantMap m = v.toObject().toVariantMap();
        QString ver = m.value("ver").toString();
        if (ver.isEmpty())
            continue;
        if (bestVer.isEmpty() || compareVersions(ver, bestVer) > 0) {
            bestVer = ver;
            best = m;
        }
    }
    return best;
}

int AppUpdater::compareVersions(const QString &a, const QString &b)
{
    const QStringList pa = a.split('.');
    const QStringList pb = b.split('.');
    const int n = qMax(pa.size(), pb.size());
    for (int i = 0; i < n; ++i) {
        int x = (i < pa.size()) ? pa.at(i).toInt() : 0;
        int y = (i < pb.size()) ? pb.at(i).toInt() : 0;
        if (x != y)
            return x > y ? 1 : -1;
    }
    return 0;
}

void AppUpdater::onCheckError(QProcess::ProcessError error)
{
    Q_UNUSED(error);
    if (!m_silentCheck)
        emit checkFailed("无法启动版本检查进程");
    setBusy(false);
}

void AppUpdater::onCheckFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    Q_UNUSED(exitStatus);
    // 收尾残留在缓冲区里的行
    m_checkBuf += m_checkProcess ? m_checkProcess->readAllStandardOutput() : QByteArray();
    int idx;
    while ((idx = m_checkBuf.indexOf('\n')) >= 0) {
        QByteArray lineB = m_checkBuf.left(idx).trimmed();
        m_checkBuf.remove(0, idx + 1);
        if (!lineB.isEmpty())
            handleCheckLine(QString::fromUtf8(lineB));
    }
    if (m_checkProcess) {
        m_checkProcess->deleteLater();
        m_checkProcess = nullptr;
    }
    if (!m_busy)
        return;                       // 解析失败分支已复位, 不再重复收尾
    if (exitCode != 0) {
        if (!m_silentCheck)
            emit checkFailed(QString("检查更新失败 (exit %1)").arg(exitCode));
        setBusy(false);
        return;
    }
    finishCheckOk();
}

void AppUpdater::finishCheckOk()
{
    if (!m_busy)
        return;
    setBusy(false);
    if (!m_latestVersion.isEmpty()) {
        emit updateAvailable(m_latestVersion, m_latestFile, m_latestBody);
    } else {
        emit upToDate(m_silentCheck);
    }
}

void AppUpdater::onDownloadReadyRead()
{
    if (!m_downloadProcess)
        return;
    m_downloadBuf += m_downloadProcess->readAllStandardOutput();
    int idx;
    while ((idx = m_downloadBuf.indexOf('\n')) >= 0) {
        QByteArray lineB = m_downloadBuf.left(idx).trimmed();
        m_downloadBuf.remove(0, idx + 1);
        if (!lineB.isEmpty())
            handleDownloadLine(QString::fromUtf8(lineB));
    }
}

void AppUpdater::handleDownloadLine(const QString &line)
{
    if (line.startsWith("PROGRESS:")) {
        int pct = line.mid(9).toInt();
        if (pct != m_downloadProgress) {
            m_downloadProgress = pct;
            emit downloadProgressChanged();
        }
    } else if (line.startsWith("DOWNLOAD_OK:")) {
        QString setupPath = line.mid(12);
        // 交给系统静默安装(管理员清单会弹一次 UAC), 随后退出本进程让安装器覆盖文件
        QStringList args{"/S"};
        bool ok = QProcess::startDetached(setupPath, args);
        qDebug() << "AppUpdater: launch installer" << setupPath << ok;
        if (!ok) {
            setBusy(false);
            m_downloadProgress = -1;
            emit downloadProgressChanged();
            emit updateFailed("无法启动安装程序，请手动打开 " + setupPath);
            return;
        }
        emit updateInstallStarted();
        QTimer::singleShot(1200, qApp, &QCoreApplication::quit);
    }
}

void AppUpdater::onDownloadError(QProcess::ProcessError error)
{
    Q_UNUSED(error);
    setBusy(false);
    m_downloadProgress = -1;
    emit downloadProgressChanged();
    emit updateFailed("无法启动下载进程");
}

void AppUpdater::onDownloadFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    Q_UNUSED(exitStatus);
    m_downloadBuf += m_downloadProcess ? m_downloadProcess->readAllStandardOutput() : QByteArray();
    int idx;
    while ((idx = m_downloadBuf.indexOf('\n')) >= 0) {
        QByteArray lineB = m_downloadBuf.left(idx).trimmed();
        m_downloadBuf.remove(0, idx + 1);
        if (!lineB.isEmpty())
            handleDownloadLine(QString::fromUtf8(lineB));
    }
    if (m_downloadProcess) {
        m_downloadProcess->deleteLater();
        m_downloadProcess = nullptr;
    }
    if (exitCode != 0) {
        setBusy(false);
        m_downloadProgress = -1;
        emit downloadProgressChanged();
        emit updateFailed(QString("下载更新失败 (exit %1)").arg(exitCode));
    }
}
