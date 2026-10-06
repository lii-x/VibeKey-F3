#pragma once

#include <QObject>
#include <QSystemTrayIcon>
#include <QQuickWindow>

class AppUpdater;
class QAction;

// 系统托盘图标管理器：在 Windows 任务栏通知区显示图标，
// 支持双击/右键菜单恢复窗口，并作为程序常驻后台的入口。
class SystemTray : public QObject
{
    Q_OBJECT
public:
    explicit SystemTray(QObject *parent = nullptr);
    ~SystemTray();

    // 关联 QML 主窗口，供显示/隐藏使用
    void setWindow(QQuickWindow *window);
    // 注入自更新管理器(检查更新/安装新版)
    // 注: 托盘只做【上位机自身】的更新提醒; 固件更新提醒在应用内(横幅/弹窗/小红点), 不走托盘。
    void setAppUpdater(AppUpdater *updater);
    // 创建并展示托盘图标（需在 QGuiApplication 创建后调用）
    void init();

public slots:
    // 收进托盘（隐藏窗口）
    void minimizeToTray();
    // 从托盘恢复窗口并置顶
    void showWindow();
    // 切换窗口显隐
    void toggleWindow();

private slots:
    void onActivated(QSystemTrayIcon::ActivationReason reason);

private:
    // 程序化生成品牌图标，避免依赖外部图片资源
    QIcon buildIcon();
    // 自更新结果 → 托盘气泡/菜单
    void onUpdateAvailable(const QString &version, const QString &file, const QString &body);
    void onUpToDate(bool wasSilent);
    void onCheckFailed(const QString &error);
    void onUpdateFailed(const QString &error);
    void onInstallStarted();

    QQuickWindow *m_window = nullptr;
    QSystemTrayIcon *m_tray = nullptr;
    AppUpdater *m_appUpdater = nullptr;
    QAction *m_updateAct = nullptr;   /* 有新版时显示的"立即更新"菜单项 */
};
