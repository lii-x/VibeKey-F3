#include "SystemTray.h"
#include "AppUpdater.h"
#include "AppVersion.h"

#include <QGuiApplication>
#include <QMenu>
#include <QAction>
#include <QPainter>
#include <QFont>

SystemTray::SystemTray(QObject *parent)
    : QObject(parent)
{
}

SystemTray::~SystemTray()
{
    delete m_tray;
}

void SystemTray::setAppUpdater(AppUpdater *updater)
{
    m_appUpdater = updater;
    if (m_appUpdater) {
        connect(m_appUpdater, &AppUpdater::updateAvailable, this, &SystemTray::onUpdateAvailable);
        connect(m_appUpdater, &AppUpdater::upToDate, this, &SystemTray::onUpToDate);
        connect(m_appUpdater, &AppUpdater::checkFailed, this, &SystemTray::onCheckFailed);
        connect(m_appUpdater, &AppUpdater::updateFailed, this, &SystemTray::onUpdateFailed);
        connect(m_appUpdater, &AppUpdater::updateInstallStarted, this, &SystemTray::onInstallStarted);
    }
}

QIcon SystemTray::buildIcon()
{
    // 优先使用工程内置图片资源（resources/appicon.qrc -> :/appicon.png）
    QIcon icon(":/appicon.png");
    if (!icon.isNull())
        return icon;

    // 资源缺失时的兜底：程序化生成绿色圆角 + 白 "V" 图标
    const int size = 64;
    QPixmap pm(size, size);
    pm.fill(Qt::transparent);

    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);

    p.setPen(Qt::NoPen);
    p.setBrush(QColor("#07c160"));
    p.drawRoundedRect(4, 4, size - 8, size - 8, 14, 14);

    p.setPen(Qt::white);
    p.setBrush(Qt::white);
    QFont f;
    f.setBold(true);
    f.setPixelSize(40);
    p.setFont(f);
    p.drawText(pm.rect(), Qt::AlignCenter, "V");

    return QIcon(pm);
}

void SystemTray::setWindow(QQuickWindow *window)
{
    m_window = window;
}

void SystemTray::init()
{
    QIcon icon = buildIcon();

    // 同时设置应用程序/窗口图标，修复任务栏按钮无图标的问题
    qApp->setWindowIcon(icon);
    if (m_window)
        m_window->setIcon(icon);   // 任务栏窗口按钮图标（QWindow::setIcon，不依赖 QML 属性）

    m_tray = new QSystemTrayIcon(this);
    m_tray->setIcon(icon);
    m_tray->setToolTip(QStringLiteral("VibeKey-F3 Studio 工作台 v%1").arg(VIBEKEY_STUDIO_VERSION));

    auto *menu = new QMenu();
    auto *verAct = menu->addAction(QStringLiteral("VibeKey-F3 Studio v%1").arg(VIBEKEY_STUDIO_VERSION));
    verAct->setEnabled(false);                       // 灰色信息项
    menu->addSeparator();
    auto *checkAct = menu->addAction("检查更新…");
    // 有新版时可见的"立即更新"入口(触发即下载→静默重装→自动开新版)
    m_updateAct = menu->addAction("立即更新");
    m_updateAct->setVisible(false);
    menu->addSeparator();
    auto *showAct = menu->addAction("显示主窗口");
    menu->addSeparator();
    auto *quitAct = menu->addAction("退出");
    m_tray->setContextMenu(menu);

    connect(showAct, &QAction::triggered, this, &SystemTray::showWindow);
    connect(quitAct, &QAction::triggered, qApp, &QGuiApplication::quit);
    connect(checkAct, &QAction::triggered, this, [this]() {
        if (m_appUpdater)
            m_appUpdater->checkNow(false);
    });
    connect(m_updateAct, &QAction::triggered, this, [this]() {
        if (m_appUpdater && !m_appUpdater->busy())
            m_appUpdater->installUpdate();
    });
    connect(m_tray, &QSystemTrayIcon::activated, this, &SystemTray::onActivated);
    connect(m_tray, &QSystemTrayIcon::messageClicked, this, [this]() {
        // 点气泡 = 立即更新(若确有新版且未在下载)
        if (m_appUpdater && m_updateAct->isVisible() && !m_appUpdater->busy())
            m_appUpdater->installUpdate();
    });

    m_tray->show();
}

void SystemTray::onActivated(QSystemTrayIcon::ActivationReason reason)
{
    // 单击/双击均恢复窗口
    if (reason == QSystemTrayIcon::DoubleClick || reason == QSystemTrayIcon::Trigger)
        showWindow();
}

void SystemTray::minimizeToTray()
{
    if (m_window)
        m_window->hide();
}

void SystemTray::showWindow()
{
    if (m_window) {
        m_window->show();
        m_window->raise();
        m_window->requestActivate();
    }
}

void SystemTray::toggleWindow()
{
    if (!m_window)
        return;
    if (m_window->isVisible())
        minimizeToTray();
    else
        showWindow();
}

/* ---- 自更新结果 → 气泡 / 菜单 ---- */

void SystemTray::onUpdateAvailable(const QString &version, const QString &file, const QString &body)
{
    Q_UNUSED(file);
    Q_UNUSED(body);
    m_updateAct->setText(QStringLiteral("立即更新到 v%1").arg(version));
    m_updateAct->setVisible(true);
    if (m_tray) {
        m_tray->showMessage(QStringLiteral("发现新版本 v%1").arg(version),
                            QStringLiteral("点击此处立即下载并自动重装（期间会退出一次）"),
                            QSystemTrayIcon::Information, 10000);
    }
}

void SystemTray::onUpToDate(bool wasSilent)
{
    m_updateAct->setVisible(false);
    if (!wasSilent && m_tray) {
        m_tray->showMessage(QStringLiteral("已是最新版本"),
                            QStringLiteral("VibeKey-F3 Studio v%1 已是最新").arg(VIBEKEY_STUDIO_VERSION),
                            QSystemTrayIcon::Information, 4000);
    }
}

void SystemTray::onCheckFailed(const QString &error)
{
    if (m_tray)
        m_tray->showMessage(QStringLiteral("检查更新失败"), error, QSystemTrayIcon::Warning, 6000);
}

void SystemTray::onUpdateFailed(const QString &error)
{
    m_updateAct->setVisible(true);   // 保留"立即更新"入口, 允许重试
    if (m_tray)
        m_tray->showMessage(QStringLiteral("更新失败"), error, QSystemTrayIcon::Critical, 8000);
}

void SystemTray::onInstallStarted()
{
    if (m_tray)
        m_tray->showMessage(QStringLiteral("正在更新"),
                            QStringLiteral("已下载新版，正在静默安装，本程序即将退出并自动重开…"),
                            QSystemTrayIcon::Information, 3000);
}
