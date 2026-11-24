#include "PythonLocator.h"

#include <QCoreApplication>
#include <QDebug>
#include <QDir>
#include <QHash>
#include <QProcess>
#include <QStandardPaths>
#include <QSysInfo>

/* 各平台常见解释器位置。顺序 = 优先级。
 *
 * macOS 注释说明（与 PythonLocator.h 的根因说明配套）：
 *   - 捆绑的 python/ 目录永远第一（绿色版 / 未来若打包解释器）；
 *   - 之后是各发行版的绝对路径：GUI 启动时 PATH 只有 /usr/bin:/bin:...，
 *     Homebrew（Apple Silicon /usr/local）和 conda 都不在 PATH 里，必须显式列出；
 *   - "python3" / "python" 放最后：只在终端启动（PATH 完整）时才可能是对的那个。
 *   - 刻意把 /usr/bin/python3 排在这些之后 —— 它有，但通常没依赖。 */
static QStringList pythonCandidates()
{
    QStringList candidates;
    const QString appDir = QCoreApplication::applicationDirPath();

#ifdef Q_OS_WIN
    candidates << appDir + "/python/python.exe"
               << appDir + "/python/python3.exe"
               << "python" << "python3"
               << "C:\\Python312\\python.exe"
               << "C:\\Python311\\python.exe"
               << "C:\\Python310\\python.exe";
    const QString user = qEnvironmentVariable("USERNAME");
    if (!user.isEmpty()) {
        candidates << "C:\\Users\\" + user + "\\AppData\\Local\\Programs\\Python\\Python312\\python.exe"
                   << "C:\\Users\\" + user + "\\AppData\\Local\\Programs\\Python\\Python311\\python.exe"
                   << "C:\\Users\\" + user + "\\AppData\\Local\\Programs\\Python\\Python310\\python.exe";
    }
#else
    /* macOS：优先使用 app 内置的独立 Python 运行时。
     * 官方 universal2 包里 arm64 / x86_64 各放一套（python-build-standalone，
     * 已装 pyserial + bleak），按当前进程架构选目录；用户无需自己装 Python。
     * 目录布局：Contents/Resources/python/<arch>/bin/python3。 */
#ifdef Q_OS_MACOS
    const QString arch = QSysInfo::currentCpuArchitecture();
    if (!arch.isEmpty()) {
        candidates << QDir::cleanPath(appDir + "/../Resources/python/" + arch + "/bin/python3")
                   << QDir::cleanPath(appDir + "/../Resources/python/" + arch + "/python3");
    }
#endif
    candidates << appDir + "/python/bin/python3"      // 未来若捆绑解释器
               << appDir + "/python/python3"
               << "/opt/homebrew/bin/python3"         // Apple Silicon Homebrew
               << "/opt/homebrew/opt/python@3.12/bin/python3"
               << "/opt/homebrew/opt/python@3.11/bin/python3"
               << "/opt/anaconda3/bin/python3"
               << "/opt/miniconda3/bin/python3"
               << "/opt/miniforge3/bin/python3"
               << "/usr/local/bin/python3"            // Intel Homebrew / python.org 安装
               << "/usr/local/opt/python@3.12/bin/python3"
               << qEnvironmentVariable("HOME") + "/anaconda3/bin/python3"
               << qEnvironmentVariable("HOME") + "/miniconda3/bin/python3"
               << qEnvironmentVariable("HOME") + "/miniforge3/bin/python3";
    /* PATH 里的条目（终端启动时完整，GUI 启动时基本只剩系统目录）。
     * QStandardPaths::findExecutable 会按 PATH 展开，避免重复判断裸名字。 */
    const QString fromPath = QStandardPaths::findExecutable("python3");
    if (!fromPath.isEmpty())
        candidates << fromPath;
    candidates << "python3" << "python"
               << "/usr/bin/python3";
#endif

    candidates.removeDuplicates();
    return candidates;
}

/* requiredModules 支持两种写法：
 *   "serial"              -> import serial
 *   "bleak:BleakAdapter"  -> from bleak import BleakAdapter
 * 第二种用于校验"模块存在但版本太老"的情况（旧版 bleak 没有 BleakAdapter，
 * 只 import bleak 会误判为可用，worker 启动后才 ImportError）。 */
static QString probeCode(const QStringList &requiredModules)
{
    if (requiredModules.isEmpty())
        return QStringLiteral("import sys");
    QStringList statements;
    for (const QString &m : requiredModules) {
        const int colon = m.indexOf(':');
        if (colon > 0) {
            statements << QStringLiteral("from %1 import %2")
                              .arg(m.left(colon), m.mid(colon + 1));
        } else {
            statements << QStringLiteral("import %1").arg(m);
        }
    }
    return statements.join(QStringLiteral("; "));
}

/* 用 requiredModules 实际 import 一次作为可用性判据。
 * 探测脚本走 -c 而不是临时文件：不落盘、不受 cwd 影响。 */
static bool probe(QProcess &proc, const QString &path, const QStringList &requiredModules)
{
    proc.start(path, {"-c", probeCode(requiredModules)});
    if (!proc.waitForFinished(2500))
        return false;
    return proc.exitCode() == 0;
}

QString findPythonInterpreter(const QStringList &requiredModules)
{
    /* 按模块集合缓存：KeyConfigManager / LightMonitor / OtaManager / AppUpdater /
     * WorkBuddyMonitor 各自在构造时问一次，实际只探测一轮。 */
    static QHash<QString, QString> cache;
    QStringList sorted = requiredModules;
    sorted.sort();
    const QString key = sorted.join(',');
    if (cache.contains(key))
        return cache.value(key);

    const QStringList candidates = pythonCandidates();
    QString startableFallback;

    QProcess proc;
    for (const QString &path : candidates) {
        if (!probe(proc, path, requiredModules))
            continue;
        qDebug() << "PythonLocator: 采用" << path
                 << (requiredModules.isEmpty() ? QStringLiteral("(无依赖要求)")
                                               : QStringLiteral("(已验证 %1)").arg(requiredModules.join(",")));
        cache.insert(key, path);
        return path;
    }

    /* 没有候选满足依赖：退一步找"至少能启动"的，保证 worker 仍会跑起来并
     * 在 stderr 里报模块缺失 —— 比静默选中一个跑不动的解释器更容易排查。 */
    for (const QString &path : candidates) {
        if (probe(proc, path, {})) {
            startableFallback = path;
            break;
        }
    }
    if (startableFallback.isEmpty())
        startableFallback = QStringLiteral("python3");

    qWarning() << "PythonLocator: 没有找到安装了" << requiredModules
               << "的解释器，回退到" << startableFallback
               << "（相关功能会失败：pip3 install" << requiredModules.join(" ") << "）";
    cache.insert(key, startableFallback);
    return startableFallback;
}
