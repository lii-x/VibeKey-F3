#ifndef OTAMANAGER_H
#define OTAMANAGER_H

#include <QObject>
#include <QProcess>
#include <QTimer>
#include <QByteArray>
#include <QVariantList>
#include <QAbstractItemModel>

class PortDetector;
class ReleasesModel;
class LightMonitor;

class OtaManager : public QObject
{
    Q_OBJECT
    Q_PROPERTY(int progress READ progress NOTIFY progressChanged)
    Q_PROPERTY(bool transferring READ transferring NOTIFY transferringChanged)
    Q_PROPERTY(QString statusMessage READ statusMessage NOTIFY statusMessageChanged)
    Q_PROPERTY(QString firmwareInfo READ firmwareInfo NOTIFY firmwareInfoChanged)

    /* 09-05: 网络获取固件 (Gitee Releases) —— 用 QAbstractItemModel(基类) 而非
     * 具体类/或 QVariantList 暴露给 QML:
     *  1) QVariantList + ListView 会触发 Qt 6.11 qarraydataops.h:45 共享追加断言;
     *  2) 属性声明为基类指针, moc 无需前向声明类完整定义(moc 合并文件可正常编译)。 */
    Q_PROPERTY(QAbstractItemModel* releasesModel READ releasesModel CONSTANT)
    Q_PROPERTY(bool netBusy READ netBusy NOTIFY netBusyChanged)
    Q_PROPERTY(int netProgress READ netProgress NOTIFY netProgressChanged)

    /* 09-13: 固件新版本提醒 —— 在线最新固件(Gitee Releases, tag vX.Y.Z)与【已连接设备】
     * 固件版本比对, 在线更新则 fwUpdateAvailable=true, QML 顶部横幅 + 托盘气泡提示。 */
    Q_PROPERTY(QString latestFwVersion READ latestFwVersion NOTIFY fwUpdateChanged)
    Q_PROPERTY(bool fwUpdateAvailable READ fwUpdateAvailable NOTIFY fwUpdateChanged)
    Q_PROPERTY(int latestFwIndex READ latestFwIndex NOTIFY fwUpdateChanged)

public:
    explicit OtaManager(QObject *parent = nullptr);
    ~OtaManager();

    int progress() const { return m_progress; }
    bool transferring() const { return m_transferring; }
    QString statusMessage() const { return m_statusMessage; }
    QString firmwareInfo() const { return m_firmwareInfo; }

    /* 声明为基类指针: 上行转换需要完整类型, 定义放 OtaManager.cpp (已 include 头文件) */
    QAbstractItemModel* releasesModel() const;
    bool netBusy() const { return m_netBusy; }
    int netProgress() const { return m_netProgress; }   /* 下载中 0..100, 空闲 -1 */

    /* 09-13 固件新版本提醒 */
    QString latestFwVersion() const { return m_latestFwVersion; }
    bool fwUpdateAvailable() const { return m_fwUpdateAvailable; }
    int latestFwIndex() const { return m_latestFwIndex; }   /* releasesModel 中的下标, -1 无 */

    /* 注入共享的设备探测器（由 main.cpp 在构造后设置，定义在 .cpp 以便挂 deviceVersionChanged） */
    void setPortDetector(PortDetector *detector);
    /* 09-14: 注入蓝牙监视器 —— 蓝牙连接时也能拿到固件版本(bleDeviceVersion), 从而
     * 在【只连蓝牙没插 USB】时也能比对出"固件有新版本"并提醒。 */
    void setLightMonitor(LightMonitor *monitor);

    Q_INVOKABLE void selectFirmware(const QString &filePath);
    Q_INVOKABLE void startTransfer();
    Q_INVOKABLE void cancelTransfer();

    /* 09-05: 网络获取固件 */
    Q_INVOKABLE void refreshReleases();
    Q_INVOKABLE void downloadRelease(int index);

    /* 09-13: 检查固件是否有新版本(与已连接设备比对)。silent=true 时不刷 OTA 页状态文案
     * (启动后台自检用)。查到后置 latestFwVersion / fwUpdateAvailable 并发信号。 */
    Q_INVOKABLE void checkFirmwareUpdate(bool silent = false);
    /* 一键下载最新固件(需先 checkFirmwareUpdate/refreshReleases 拿到列表) */
    Q_INVOKABLE void downloadLatestFirmware();

signals:
    void progressChanged();
    void transferringChanged();
    void statusMessageChanged();
    void firmwareInfoChanged();
    void transferComplete();
    void transferFailed(const QString &error);

    /* 09-05: 网络获取固件 */
    void netBusyChanged();
    void netProgressChanged();
    void netFailed(const QString &error);

    /* 09-13: 固件新版本提醒 */
    void fwUpdateChanged();
    void firmwareUpdateAvailable(const QString &version, const QString &current);

private slots:
    void onProcessReadyRead();
    void onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void onProcessError(QProcess::ProcessError error);
    void onNetReadyRead();
    void onNetFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void onNetError(QProcess::ProcessError error);

private:
    void setStatus(const QString &msg);
    QString findPython();
    QString findScript(const QString &scriptName);
    QString findWorkerScript();
    void setNetBusy(bool busy);
    void startNetWorker(const QStringList &args);
    void handleNetLine(const QString &line);
    void parseReleases(const QString &json);

    /* 09-13: 依据 releasesModel + 已连接设备版本, 重算"固件是否有新版"
     * (设备插拔/版本变化、在线列表刷新时都调用) */
    void refreshFwUpdateState();
    static int cmpVersion(const QString &a, const QString &b);   /* >0 表示 a 比 b 新 */

    PortDetector *m_portDetector;
    LightMonitor *m_lightMonitor = nullptr;   /* 09-14: 蓝牙固件版本来源 */
    QProcess *m_otaProcess;
    QProcess *m_netProcess;
    int m_progress;
    bool m_transferring;
    bool m_verifyWarned;   /* 升级后版本未变化警告(08-27): 已按失败提示, 抑制 DONE 的成功提示 */
    QString m_statusMessage;
    QString m_firmwareInfo;
    QString m_firmwarePath;
    QString m_pythonPath;
    ReleasesModel *m_releasesModel;  /* 09-05: Gitee release 列表(QAbstractListModel) */
    bool m_netBusy;
    int m_netProgress;           /* 下载进度 0..100, -1 空闲 */
    QByteArray m_netStdout;      /* net_worker 行缓冲 */

    /* 09-13 固件新版本提醒 */
    QString m_latestFwVersion;   /* 在线最新固件版本(不含前导 v); 空=未知 */
    bool m_fwUpdateAvailable = false;
    int m_latestFwIndex = -1;    /* 最新固件在 releasesModel 中的下标; -1 无 */
    bool m_fwCheckSilent = false;/* 本次 list 是静默自检(不刷状态文案) */
    bool m_fwAutoTried = false;  /* 设备首次在线时已补拉过一次在线列表(防重复联网) */
};

#endif // OTAMANAGER_H
