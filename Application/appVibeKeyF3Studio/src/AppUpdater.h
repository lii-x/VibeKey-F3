#ifndef APPUPDATER_H
#define APPUPDATER_H

/* AppUpdater —— 上位机应用自更新(09-05 起)
 *
 * 链路: 本机 APP_VERSION(AppVersion.h) ↔ Gitee Releases「setup 资产」
 *   checkNow()   → net_worker.py list setup   → 取版本号最大者
 *                  → 比 currentVersion: 大则 updateAvailable(version,file,body), 否则 upToDate()
 *   installUpdate() → net_worker.py download <url> <tmp/setup.exe>
 *                  → DOWNLOAD_OK 后 QProcess::startDetached(setup.exe, /S) 静默重装
 *                  → qApp 延时退出(installer 覆盖文件前 taskkill 兜底, 装完自启新版)
 *
 * 注: 依赖 net_worker.py(与固件 OTA 共用), 资产按文件名 VibeKey-F3_Studio_Setup_vX.Y.Z.exe
 *     过滤 —— 与固件 .bin 天然隔离。
 */
#include <QObject>
#include <QProcess>
#include <QTimer>
#include <QByteArray>
#include <QVariantMap>

class AppUpdater : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString currentVersion READ currentVersion CONSTANT)
    Q_PROPERTY(QString latestVersion READ latestVersion NOTIFY latestVersionChanged)
    Q_PROPERTY(bool busy READ busy NOTIFY busyChanged)
    Q_PROPERTY(int downloadProgress READ downloadProgress NOTIFY downloadProgressChanged)

public:
    explicit AppUpdater(QObject *parent = nullptr);
    ~AppUpdater();

    QString currentVersion() const { return m_currentVersion; }
    QString latestVersion() const { return m_latestVersion; }
    bool busy() const { return m_busy; }
    int downloadProgress() const { return m_downloadProgress; }   /* 0..100, -1 空闲 */

    /* silent=true: 失败时不发 checkFailed(启动后台自检用, 免得没网一直弹) */
    Q_INVOKABLE void checkNow(bool silent = false);
    /* 有可用更新时调用: 下载并静默重装, 装完自动开新版 */
    Q_INVOKABLE void installUpdate();

signals:
    void latestVersionChanged();
    void busyChanged();
    void downloadProgressChanged();
    void updateAvailable(const QString &version, const QString &file, const QString &body);
    void upToDate(bool wasSilent);      /* wasSilent: 启动自检(无新版时 UI 不弹气泡) */
    void checkFailed(const QString &error);
    void updateFailed(const QString &error);
    void updateInstallStarted();      /* 已把安装器交给系统, 即将退出 */

private slots:
    void onCheckReadyRead();
    void onCheckFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void onCheckError(QProcess::ProcessError error);
    void onDownloadReadyRead();
    void onDownloadFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void onDownloadError(QProcess::ProcessError error);

private:
    void setBusy(bool busy);
    QString findPython();
    QString findScript(const QString &scriptName);
    void handleCheckLine(const QString &line);
    void handleDownloadLine(const QString &line);
    void finishCheckOk();
    static int compareVersions(const QString &a, const QString &b);  /* >0 a 新 */
    static QVariantMap parseReleasesLine(const QString &json, QString *err);  /* 返回版本号最大条目 */

    QString m_currentVersion;
    QString m_latestVersion;      /* 发现的在线最新版, 空=尚未查到 */
    QString m_latestFile;         /* setup 资产文件名 */
    QString m_latestUrl;
    QString m_latestBody;
    bool m_silentCheck = false;
    bool m_busy = false;
    int m_downloadProgress = -1;
    QString m_pythonPath;
    QProcess *m_checkProcess = nullptr;
    QProcess *m_downloadProcess = nullptr;
    QByteArray m_checkBuf;
    QByteArray m_downloadBuf;
};

#endif
