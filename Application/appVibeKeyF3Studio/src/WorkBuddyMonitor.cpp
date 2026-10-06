#include "WorkBuddyMonitor.h"
#include "LightMonitor.h"
#include <QCoreApplication>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QJsonParseError>

WorkBuddyMonitor::WorkBuddyMonitor(QObject *parent)
    : QObject(parent)
    , m_timer(new QTimer(this))
    , m_blinkTimer(new QTimer(this))
    , m_blinkOn(false)
    , m_blinkMode(NoBlink)
    , m_proc(nullptr)
    , m_status("idle")
    , m_taskName()
    , m_active(false)
    , m_alert(false)
    , m_deviceLinked(false)
    , m_lightMonitor(nullptr)
    , m_todayRuns(0)
    , m_todaySuccess(0)
    , m_todayFail(0)
{
    /* 08-31: python 路径只在这里解析一次。findPython() 会阻塞并派生 python.exe,
     * 绝不能放进每秒执行的 poll()。 */
    m_pythonPath = findPython();

    connect(m_timer, &QTimer::timeout, this, &WorkBuddyMonitor::poll);
    /* 09-10: await/alert 态设备灯闪烁定时器（350ms 翻转, 与上位机面板 blink 同步）。
     * 仅经 BLE serve 快速写 stdin 才有意义; USB 单条命令 1~2s, 不会真闪但也不出错。 */
    m_blinkTimer->setInterval(350);
    connect(m_blinkTimer, &QTimer::timeout, this, &WorkBuddyMonitor::onBlinkTimeout);
    m_blinkTimer->stop();
    /* 08-31: 1s -> 3s。状态页展示的是 WorkBuddy 运行态, 3s 刷新足够,
     * 且每轮都要起一个 python 子进程, 降频可显著减少进程派生开销。 */
    m_timer->setInterval(3000);
    m_timer->start();

    poll(); // 立即读一次
}

WorkBuddyMonitor::~WorkBuddyMonitor()
{
    // 先断掉对 LightMonitor 的引用，防止 kill() 触发 finished -> parseOutput -> pushToDevice()
    // 时 LightMonitor 已因 main() 栈逆序析构而被释放（RTC#2 栈破坏）。
    m_lightMonitor = nullptr;
    m_timer->stop();
    m_blinkTimer->stop();
    if (m_proc) {
        // 断开 finished 等信号，避免 kill() 过程中同步触发本对象槽函数
        disconnect(m_proc, nullptr, this, nullptr);
        if (m_proc->state() != QProcess::NotRunning)
            m_proc->kill();
    }
}

QString WorkBuddyMonitor::findPython()
{
    // 优先程序目录下的 python/（绿色版捆绑的 Python）
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

QString WorkBuddyMonitor::findScript()
{
    QString appDir = QCoreApplication::applicationDirPath();
    QString scriptName = "workbuddy_status.py";
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

void WorkBuddyMonitor::poll()
{
    if (!m_proc)
        m_proc = new QProcess(this);
    if (m_proc->state() != QProcess::NotRunning)
        return; // 上一轮还没结束，跳过本次
    connect(m_proc, &QProcess::finished, this, &WorkBuddyMonitor::onProcFinished);
    connect(m_proc, &QProcess::readyReadStandardOutput, this, [this]() {
        parseOutput(m_proc->readAllStandardOutput());
    });
    m_proc->setWorkingDirectory(QCoreApplication::applicationDirPath());
    /* 08-31: 用构造函数缓存的 m_pythonPath, 不再每轮调阻塞的 findPython() */
    m_proc->start(m_pythonPath, {findScript()});
}

void WorkBuddyMonitor::onProcFinished(int exitCode, QProcess::ExitStatus)
{
    Q_UNUSED(exitCode)
    if (m_proc) {
        // 收尾可能残留的输出
        parseOutput(m_proc->readAllStandardOutput());
        m_proc->deleteLater();
        m_proc = nullptr;
    }
}

void WorkBuddyMonitor::parseOutput(const QByteArray &data)
{
    m_buf.append(data);
    int nl;
    while ((nl = m_buf.indexOf('\n')) >= 0) {
        QByteArray line = m_buf.left(nl).trimmed();
        m_buf.remove(0, nl + 1);
        if (line.isEmpty()) continue;

        QJsonParseError err;
        QJsonDocument doc = QJsonDocument::fromJson(line, &err);
        if (err.error != QJsonParseError::NoError || !doc.isObject())
            continue;
        QJsonObject o = doc.object();

        QString st = o.value("status").toString();
        QString tn = o.value("task").toString();
        bool act = o.value("active").toBool();
        /* 10-03: alert 独立字段。worker 已在其内部用 1.2s 迟滞做过防抖，
         * 这里直接采信即可；status 仍会同时是 "alert"，两者都传是冗余但无害，
         * 保留 alert 字段是为了 QML 能单独取用而不用字符串比较。 */
        bool alrt = o.value("alert").toBool();

        QVariantList rl;
        QJsonArray rec = o.value("recent").toArray();
        for (const QJsonValue &v : rec) {
            QJsonObject ro = v.toObject();
            QVariantMap m;
            m["name"] = ro.value("name").toString();
            m["success"] = ro.value("success").toBool();
            m["timeText"] = ro.value("timeText").toString();
            rl.append(m);
        }

        int tr = o.value("todayRuns").toInt();
        int ts = o.value("todaySuccess").toInt();
        int tf = o.value("todayFail").toInt();

        if (st != m_status) {
            m_status = st;
            emit statusChanged();
            /* 10-03: 闪烁由 status 单值驱动，alert 优先（红）> await（黄）。
             * 离开两个等待态才停闪并下发新稳态。 */
            if (st == "alert") {
                startBlink(AlertBlink);
            } else if (st == "await") {
                startBlink(AwaitBlink);
            } else {
                stopBlink();
                maybePushToDevice();
            }
        }
        if (alrt != m_alert) { m_alert = alrt; emit alertChanged(); }
        if (tn != m_taskName) { m_taskName = tn; emit taskNameChanged(); }
        if (act != m_active) { m_active = act; emit activeChanged(); maybePushToDevice(); }
        if (rl != m_recentTasks) { m_recentTasks = rl; emit recentTasksChanged(); }
        if (tr != m_todayRuns || ts != m_todaySuccess || tf != m_todayFail) {
            m_todayRuns = tr; m_todaySuccess = ts; m_todayFail = tf;
            emit todayStatsChanged();
        }
    }
}

void WorkBuddyMonitor::setDeviceLinked(bool linked)
{
    if (m_deviceLinked == linked) return;
    m_deviceLinked = linked;
    emit deviceLinkedChanged();
    if (!linked) stopBlink();   // 关联动：先停闪，再下发灭灯
    pushToDevice();   // 开 -> 按当前状态下发；关 -> 0(灭灯)
}

void WorkBuddyMonitor::setLightMonitor(LightMonitor *monitor)
{
    m_lightMonitor = monitor;
    /* 构造期就进入等待态时 lightMonitor 尚未注入，此处补启动。
     * 10-03: alert 也要补，且优先 —— 红灯比黄灯更紧急，不能被黄闪覆盖。 */
    if (!m_lightMonitor || !m_deviceLinked) return;
    if (m_status == "alert")     startBlink(AlertBlink);
    else if (m_status == "await") startBlink(AwaitBlink);
}

int WorkBuddyMonitor::mapStatusToCode() const
{
    if (!m_deviceLinked) return 0;      // 联动关闭 -> 灭灯（唯一灭灯条件）
    if (m_status == "alert") return 3;  // 10-03: 高风险命令审批 = 红灯（闪烁期间由 blinkCode 接管）
    if (m_status == "await") return 1;  // 黄灯（等待审批/输入；09-10 新增，上位机侧闪烁）
    if (m_status == "busy") return 1;   // 黄灯（工作中）
    if (m_status == "error") return 3;  // 红灯（出错）
    return 2;                           // 绿灯（空闲 / 任务已完成 / 未运行 均保持常亮）
}

void WorkBuddyMonitor::pushToDevice()
{
    if (!m_lightMonitor) return;
    if (!m_deviceLinked) {                 // 联动关闭 -> 灭灯（唯一灭灯条件）
        m_lightMonitor->sendState(0);
        return;
    }
    /* 两种等待态都走闪烁而非稳态。10-03: alert(红) 必须排在 await(黄) 前面 ——
     * 若先判 await，alert 会被降级成黄闪，红灯就丢了。 */
    if (m_status == "alert") {
        startBlink(AlertBlink);
        return;
    }
    if (m_status == "await") {
        startBlink(AwaitBlink);
        return;
    }
    m_lightMonitor->sendState(mapStatusToCode());
}

void WorkBuddyMonitor::maybePushToDevice()
{
    if (m_deviceLinked) pushToDevice();
}

/* ---- 闪烁（10-03 重构：统一入口，支持黄/红两色）--------------------------
 * 原设计只有 await 一种闪烁，故 startAwaitBlink/stopAwaitBlink 里颜色写死 1(黄)。
 * 10-03 起有第二种：alert = 高风险命令审批（批量删除等），必须红灯 —— 危险等级
 * 明显高于普通"选一个选项"，红色闪烁才不会被当成"工作中"忽略掉。
 * 两种模式共用 m_blinkTimer：status 是单值字段且 alert 判定优先级最高，
 * 不存在两种同时成立的情况，因此一个 timer 足够，也避免两个 timer 打架。 */
int WorkBuddyMonitor::blinkCode() const
{
    // LED 编码: 0=灭 1=黄 2=绿 3=红
    return (m_blinkMode == AlertBlink) ? 3 : 1;
}

void WorkBuddyMonitor::startBlink(BlinkMode mode)
{
    if (!m_lightMonitor || !m_deviceLinked) return;
    if (m_blinkTimer->isActive() && m_blinkMode == mode) return;  // 已在按同色闪
    m_blinkMode = mode;
    m_blinkOn = true;
    m_lightMonitor->sendState(blinkCode());   // 先点亮对应颜色
    m_blinkTimer->start();
}

void WorkBuddyMonitor::stopBlink()
{
    if (m_blinkTimer->isActive()) {
        m_blinkTimer->stop();
        m_blinkOn = false;
    }
    m_blinkMode = NoBlink;
}

void WorkBuddyMonitor::onBlinkTimeout()
{
    if (!m_lightMonitor || !m_deviceLinked || m_blinkMode == NoBlink) {
        stopBlink();
        return;
    }
    m_blinkOn = !m_blinkOn;
    // 亮/灭交替 = 闪烁；颜色由 blinkMode 决定（黄=await，红=alert）
    m_lightMonitor->sendState(m_blinkOn ? blinkCode() : 0);
}
