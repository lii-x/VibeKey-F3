#include <QApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickWindow>
#include <QIcon>
#include <QQuickStyle>
#include <QTimer>
#include "OtaManager.h"
#include "KeyConfigManager.h"
#include "SystemTray.h"
#include "PortDetector.h"
#include "WorkBuddyMonitor.h"
#include "LightMonitor.h"
#include "ReleasesModel.h"
#include "AppUpdater.h"

int main(int argc, char *argv[])
{
    // 使用 QApplication（而非 QGuiApplication），因为系统托盘右键菜单用到了 QWidget（QMenu）
    QApplication app(argc, argv);

    // 强制使用非 native 风格（Fusion）。本工程大量自定义了 Slider/ProgressBar 等控件的
    // background/handle/contentItem，而 Windows 的 native(Fluent) 风格不支持这些自定义，
    // 在 Qt 6.11 + Debug 下重绘时会直接段错误（terminated abnormally）。
    // Fusion 完整支持自定义且跨平台外观一致，可彻底消除此类崩溃与“style does not support
    // customization”警告。若想换 Basic/Material 也可在此修改。
    QQuickStyle::setStyle("Fusion");

    // 尽早设置应用级图标，作为所有窗口的默认图标（任务栏/ALT+TAB 等会优先使用）
    app.setWindowIcon(QIcon(":/appicon.png"));

    PortDetector portDetector;
    OtaManager otaManager;
    KeyConfigManager keyConfigManager;
    SystemTray systemTray;
    WorkBuddyMonitor workbuddyMonitor;
    LightMonitor lightMonitor;
    AppUpdater appUpdater;   /* 09-05: 上位机应用自更新(托盘检查/安装) */

    // 共享的设备探测器注入各业务模块
    otaManager.setPortDetector(&portDetector);
    keyConfigManager.setPortDetector(&portDetector);
    lightMonitor.setPortDetector(&portDetector);
    workbuddyMonitor.setLightMonitor(&lightMonitor);
    /* 09-03: BLE 改建键配置 —— 把 BLE 连接状态源注入键配置管理器:
     * 插 USB 走 USB 通道; 未插 USB 且蓝牙已连时键配置读写走 BLE 通道(OTA 仍仅 USB)。 */
    keyConfigManager.setLightMonitor(&lightMonitor);
    /* 09-14: OTA 也接蓝牙版本源 —— 只连蓝牙(没插 USB)时也能比对"固件有没有新版本"并提醒 */
    otaManager.setLightMonitor(&lightMonitor);

    QQmlApplicationEngine engine;
    /* 09-05: 把 ReleasesModel 注册到 QML 模块, 让 QML 能在基类指针属性上
     * 正确分发到具体类的元对象(否则 count()/at() 这种方法可能解析失败)。 */
    qmlRegisterUncreatableType<ReleasesModel>("VibeKeyF3Studio", 1, 0,
        "ReleasesModel", QStringLiteral("由 OtaManager.releasesModel 提供"));
    engine.rootContext()->setContextProperty("portDetector", &portDetector);
    engine.rootContext()->setContextProperty("otaManager", &otaManager);
    engine.rootContext()->setContextProperty("keyConfig", &keyConfigManager);
    engine.rootContext()->setContextProperty("workbuddyMonitor", &workbuddyMonitor);
    engine.rootContext()->setContextProperty("lightMonitor", &lightMonitor);
    engine.rootContext()->setContextProperty("systemTray", &systemTray);
    engine.rootContext()->setContextProperty("appUpdater", &appUpdater);
    engine.loadFromModule("VibeKeyF3Studio", "Main");

    QObject::connect(
        &engine,
        &QQmlApplicationEngine::objectCreationFailed,
        &app,
        []() { QCoreApplication::exit(-1); },
        Qt::QueuedConnection);

    // 健壮获取主窗口：优先 rootObjects[0]，失败则遍历查找 QQuickWindow
    QQuickWindow *win = qobject_cast<QQuickWindow *>(engine.rootObjects().value(0));
    if (!win) {
        for (QObject *obj : engine.rootObjects()) {
            if ((win = qobject_cast<QQuickWindow *>(obj)))
                break;
            if ((win = obj->findChild<QQuickWindow *>()))
                break;
        }
    }

    // 在窗口显示之前设置图标，确保 Windows 任务栏按钮创建时即捕获正确图标
    if (win) {
        systemTray.setWindow(win);
        win->create();                              // 强制创建原生窗口句柄（HWND），确保后续 setIcon 能写入
        win->setIcon(QIcon(":/appicon.png"));
    }

    // 初始化系统托盘（其内部 m_window->setIcon 作为双保险，不冲突）
    systemTray.setAppUpdater(&appUpdater);   // 注入自更新管理器(先于 init 建菜单连接)
    /* 注: 托盘只提醒【上位机自身】更新; 固件更新提醒在应用内(OtaManager → QML 横幅/弹窗/小红点)。 */
    systemTray.init();

    // 启动 3 秒后后台静默查一次应用更新(静默: 无新版/失败都不弹气泡, 有新版才提示)
    QTimer::singleShot(3000, &app, [&appUpdater]() { appUpdater.checkNow(true); });
    // 启动 5 秒后后台静默查一次固件更新: 拉到在线最新固件并与【已连接设备】版本比对,
    // 在线更新才提醒。未插设备时只缓存最新版; 之后插上 USB 由 deviceVersionChanged 触发比对。
    QTimer::singleShot(5000, &app, [&otaManager]() { otaManager.checkFirmwareUpdate(true); });

    // 现在才显式显示窗口，任务栏捕获带图标的窗口
    if (win)
        win->show();

    return app.exec();
}
