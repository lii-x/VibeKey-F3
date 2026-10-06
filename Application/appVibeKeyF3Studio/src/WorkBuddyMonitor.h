#ifndef WORKBUDDYMONITOR_H
#define WORKBUDDYMONITOR_H

#include <QObject>
#include <QPointer>
#include <QTimer>
#include <QProcess>
#include <QStringList>
#include <QVariantList>

class LightMonitor;   // 前向声明，避免循环 include

/* 监控 WorkBuddy 自身运行状态（运行状态页）。
 * 周期运行 workbuddy_status.py 读取 ~/.workbuddy/workbuddy.db，
 * 解析出 status(busy/idle/error) 并暴露给 QML 用于状态页展示。
 * 支持通过 deviceLinked 开关把状态同步到设备 LED（由 LightMonitor 下发）。 */
class WorkBuddyMonitor : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString status READ status NOTIFY statusChanged)
    Q_PROPERTY(QString taskName READ taskName NOTIFY taskNameChanged)
    Q_PROPERTY(bool active READ active NOTIFY activeChanged)
    /* 10-03: 高风险命令审批（批量删除等）等待确认 —— 上位机据此红灯闪烁 */
    Q_PROPERTY(bool alert READ alert NOTIFY alertChanged)
    Q_PROPERTY(bool deviceLinked READ deviceLinked NOTIFY deviceLinkedChanged)
    Q_PROPERTY(QVariantList recentTasks READ recentTasks NOTIFY recentTasksChanged)
    Q_PROPERTY(int todayRuns READ todayRuns NOTIFY todayStatsChanged)
    Q_PROPERTY(int todaySuccess READ todaySuccess NOTIFY todayStatsChanged)
    Q_PROPERTY(int todayFail READ todayFail NOTIFY todayStatsChanged)

public:
    explicit WorkBuddyMonitor(QObject *parent = nullptr);
    ~WorkBuddyMonitor();

    QString status() const { return m_status; }
    QString taskName() const { return m_taskName; }
    bool active() const { return m_active; }
    bool alert() const { return m_alert; }
    bool deviceLinked() const { return m_deviceLinked; }
    QVariantList recentTasks() const { return m_recentTasks; }
    int todayRuns() const { return m_todayRuns; }
    int todaySuccess() const { return m_todaySuccess; }
    int todayFail() const { return m_todayFail; }

    Q_INVOKABLE void setDeviceLinked(bool linked);
    Q_INVOKABLE void pushToDevice();
    /* 注入 LightMonitor，用于 deviceLinked 开启时自动下发 LED 状态 */
    void setLightMonitor(LightMonitor *monitor);

signals:
    void statusChanged();
    void taskNameChanged();
    void activeChanged();
    void alertChanged();
    void deviceLinkedChanged();
    void recentTasksChanged();
    void todayStatsChanged();

private slots:
    void poll();
    void onProcFinished(int exitCode, QProcess::ExitStatus exitStatus);
    void onBlinkTimeout();

private:
    QString findPython();
    QString findScript();
    void parseOutput(const QByteArray &data);
    int mapStatusToCode() const;
    void maybePushToDevice();
    /* 10-03: 闪烁统一入口。mode = AwaitBlink（黄，审批/可选项）
     * 或 AlertBlink（红，高风险命令审批）。两种模式互斥，切换时先停再起，
     * 避免残留的 timer 带着旧颜色继续闪。 */
    enum BlinkMode { NoBlink = 0, AwaitBlink, AlertBlink };
    void startBlink(BlinkMode mode);
    void stopBlink();
    int  blinkCode() const;

    QTimer *m_timer;
    QTimer *m_blinkTimer;          // await/alert 态时驱动设备灯闪烁
    bool m_blinkOn;
    /* 10-03: 当前闪烁模式。alert(红) 优先于 await(黄) —— 高风险审批更紧急。
     * 没有单独的 m_alertBlinkTimer，两种模式共用 m_blinkTimer，
     * 因为同一时刻只可能有一种（status 字段是单值，alert 判定优先级最高）。 */
    BlinkMode m_blinkMode;
    QProcess *m_proc;
    QByteArray m_buf;

    /* 08-31: 缓存 python 路径。findPython() 内部是
     * `test.start(); test.waitForFinished(1000)` —— 阻塞且真起一个 python.exe 进程。
     * 原来每轮 poll() 都调它(1s 一次, 且在主线程), 是卡顿的主要来源之一。
     * 绿色版里 appDir/python/python.exe 存在 -> 每次都真的派生进程;
     * 而开发版该路径不存在 -> start() 秒失败, 反而没开销(故"打包版比开发版更卡")。
     * 与 LightMonitor / KeyConfigManager 的做法对齐: 构造函数里解析一次并缓存。 */
    QString m_pythonPath;

    QString m_status;
    QString m_taskName;
    bool m_active;
    bool m_alert;              // 10-03: 高风险命令审批等待确认
    bool m_deviceLinked;
    QPointer<LightMonitor> m_lightMonitor;
    QVariantList m_recentTasks;
    int m_todayRuns;
    int m_todaySuccess;
    int m_todayFail;
};

#endif // WORKBUDDYMONITOR_H
