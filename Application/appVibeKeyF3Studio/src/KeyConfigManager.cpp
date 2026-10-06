#include "KeyConfigManager.h"
#include "PortDetector.h"
#include "LightMonitor.h"   /* 09-03: BLE 通道仲裁(channel()==ble) */
#include <QGuiApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QCoreApplication>
#include <QDateTime>   /* 10-04: serve 就绪等待的计时基准 */
#include <QDebug>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QTimer>
#ifdef Q_OS_WIN
#include <windows.h>
static KeyConfigManager *g_hookInstance = nullptr;
#endif

static const QString KEY_NAMES[] = {"KEY3", "KEY4", "KEY5"};

/* ---- 09-05: L2/L3 侧键辅助(构造器默认值 / worker JSON 兜底), 放文件头供全文使用 ---- */

/* L2/L3 默认 = 历史硬编码行为: 鼠标左/右键(与固件 default_config 一致)。
 * 09-07: 带 L 键三手势默认(模式=常规 0, 单击/双击/长按=未设置 NONE), 与固件
 * default_config 一致 —— 保证"未读取即写入"时 worker 发的是 NONE 块(显式清除),
 * 而不是 0xFF(保持设备旧值), 语义与 C 键默认完全对齐。 */
static QVariantMap defaultLKey(int mouseKeycode)
{
    QVariantMap m;
    m["action"] = "mouse";
    m["modifier"] = 0;
    m["keycode"] = mouseKeycode;
    m["l_mode"] = 0;
    m["dbl_action"] = "none"; m["dbl_modifier"] = 0; m["dbl_keycode"] = 0;
    m["lng_action"] = "none"; m["lng_modifier"] = 0; m["lng_keycode"] = 0;
    m["tap_action"] = "none"; m["tap_modifier"] = 0; m["tap_keycode"] = 0;
    return m;
}

/* 10-05: EC 键的"键盘组合"默认(按压滚动用) —— 字段结构与 defaultEcKey 完全对齐,
 * 只是 action 换成 keyboard 并带 modifier。用于 ec_cw_press_key / ec_ccw_press_key
 * 的出厂默认(LCtrl+[ / LCtrl+]), 必须与固件 key_config.c 的默认值保持一致。 */
static QVariantMap ecKeyFromShortcut(int modifier, int keycode)
{
    QVariantMap m;
    m["action"] = "keyboard";
    m["modifier"] = modifier;
    m["keycode"] = keycode;
    m["ec_mode"] = 0;
    m["dbl_action"] = "none"; m["dbl_modifier"] = 0; m["dbl_keycode"] = 0;
    m["lng_action"] = "none"; m["lng_modifier"] = 0; m["lng_keycode"] = 0;
    m["tap_action"] = "none"; m["tap_modifier"] = 0; m["tap_keycode"] = 0;
    return m;
}

/* L1 默认 = 空中鼠标开关(与固件默认一致)。手势默认同上。 */
static QVariantMap defaultL1Key()
{
    QVariantMap m;
    m["action"] = "airmouse";
    m["modifier"] = 0;
    m["keycode"] = 0;
    m["l_mode"] = 0;
    m["dbl_action"] = "none"; m["dbl_modifier"] = 0; m["dbl_keycode"] = 0;
    m["lng_action"] = "none"; m["lng_modifier"] = 0; m["lng_keycode"] = 0;
    m["tap_action"] = "none"; m["tap_modifier"] = 0; m["tap_keycode"] = 0;
    return m;
}

/* EC 默认(与固件一致): cw=滚轮上(5) press=中键(4) ccw=滚轮下(6)。
 * 09-07: 带 EC 按下模式+三手势默认(模式=常规 0, 单击/双击/长按=未设置 NONE),
 * 与固件 default_config 一致 —— 保证"未读取即写入"时 worker 发的是 NONE 块
 * (显式清除)而不是 0xFF(保持设备旧值)。旋转键(cw/ccw)不读这些字段, 存而不用。 */
static QVariantMap defaultEcKey(int keycode)
{
    QVariantMap m;
    m["action"] = "mouse";
    m["modifier"] = 0;
    m["keycode"] = keycode;
    m["ec_mode"] = 0;
    m["dbl_action"] = "none"; m["dbl_modifier"] = 0; m["dbl_keycode"] = 0;
    m["lng_action"] = "none"; m["lng_modifier"] = 0; m["lng_keycode"] = 0;
    m["tap_action"] = "none"; m["tap_modifier"] = 0; m["tap_keycode"] = 0;
    return m;
}

/* 09-07: 兜底合并 —— 旧 worker/旧固件回包缺 EC 按下模式/手势字段时补默认
 * (模式=常规 0, 三手势 NONE), 保证 QML 拿到的 map 恒含 ec_mode 与 tap/dbl/lng
 * 前缀字段。 */
static QVariantMap withEcPressGestureDefaults(QVariantMap m)
{
    if (!m.contains(QStringLiteral("ec_mode")))
        m[QStringLiteral("ec_mode")] = 0;
    const QStringList prefs = { QStringLiteral("tap_"),
                                QStringLiteral("dbl_"),
                                QStringLiteral("lng_") };
    for (const QString &pfx : prefs) {
        if (!m.contains(pfx + QStringLiteral("action")))
            m[pfx + QStringLiteral("action")] = QStringLiteral("none");
        if (!m.contains(pfx + QStringLiteral("modifier")))
            m[pfx + QStringLiteral("modifier")] = 0;
        if (!m.contains(pfx + QStringLiteral("keycode")))
            m[pfx + QStringLiteral("keycode")] = 0;
    }
    return m;
}

/* EC worker JSON 兜底(非法 -> 手势默认值)。rotation 参数保留(历史签名):
 * EC 三键/按下 MOUSE 键码统一放宽为 1..6(与固件 key_config_eckey_valid 一致,
 * 09-06 起按下手势也允许滚轮 5/6 = 每次按下滚一格)。 */
static QVariantMap sanitizedEcKey(const QVariantMap &in, const QVariantMap &def, bool rotation)
{
    (void)rotation;   /* 09-07: MOUSE 键码统一 1..6, 不再区分旋转/按下 */
    QVariantMap m = in;
    QString action = m.value("action").toString();
    if (action == "airmouse") {
        /* 09-06: EC 三手势允许"空中鼠标"动作(激活行为同 L1) */
        m["modifier"] = 0;
        m["keycode"] = 0;
        return m;
    }
    if (action == "mouse") {
        int kc = m.value("keycode").toInt();
        bool ok = (kc >= 0x01 && kc <= 0x06);
        return ok ? m : def;
    }
    if (action == "keyboard" || action == "multimedia") {
        m["modifier"] = m.value("modifier").toInt() & 0xFF;
        m["keycode"] = m.value("keycode").toInt() & 0xFF;
        return m;
    }
    if (action == "none") {
        m["modifier"] = 0;
        m["keycode"] = 0;
        return m;
    }
    return def;
}

/* worker JSON(旧固件回包缺字段/字段损坏) -> 就地兜底, 保证 QML 拿到的 map 恒有效。
 * def = 该键的默认值(mouse 类键带默认键码; L1 = airmouse)。
 * allowAirmouse: 仅 L1 允许 airmouse 动作, 与固件校验一致。 */
static QVariantMap sanitizedLKey(const QVariantMap &in, const QVariantMap &def,
                                 bool allowAirmouse = false)
{
    QVariantMap m = in;
    QString action = m.value("action").toString();
    if (allowAirmouse && action == "airmouse") {
        m["modifier"] = 0;
        m["keycode"] = 0;
        return m;
    }
    if (action == "mouse") {
        int kc = m.value("keycode").toInt();
        if (kc < 1 || kc > 6)
            return def;
        m["modifier"] = 0;
    } else if (action == "keyboard" || action == "multimedia") {
        m["modifier"] = m.value("modifier").toInt() & 0xFF;
        m["keycode"] = m.value("keycode").toInt() & 0xFF;
    } else if (action == "none") {
        m["modifier"] = 0;
        m["keycode"] = 0;
    } else {
        return def;
    }
    return m;
}

static int qtKeyToHid(int key)
{
    if (key >= 0x41 && key <= 0x5A) return key - 0x41 + 4;
    if (key >= 0x30 && key <= 0x39) return key - 0x30 + 29;
    if (key >= Qt::Key_F1 && key <= Qt::Key_F12) return key - Qt::Key_F1 + 58;
    struct { int qt; int hid; } map[] = {
        {Qt::Key_Return, 40}, {Qt::Key_Enter, 40}, {Qt::Key_Escape, 41},
        {Qt::Key_Backspace, 42}, {Qt::Key_Tab, 43}, {Qt::Key_Space, 44},
        {Qt::Key_Delete, 76}, {Qt::Key_Insert, 73}, {Qt::Key_Home, 74},
        {Qt::Key_End, 77}, {Qt::Key_PageUp, 75}, {Qt::Key_PageDown, 78},
        {Qt::Key_Left, 80}, {Qt::Key_Right, 79}, {Qt::Key_Up, 82},
        {Qt::Key_Down, 81}, {Qt::Key_CapsLock, 57},
        {0, -1}
    };
    for (int i = 0; map[i].qt != 0; i++) {
        if (map[i].qt == key) return map[i].hid;
    }
    return -1;
}

#ifdef Q_OS_WIN
static int vkToHid(int vk)
{
    if (vk >= 'A' && vk <= 'Z') return vk - 'A' + 4;
    if (vk >= '0' && vk <= '9') return vk - '0' + 29;
    if (vk >= VK_F1 && vk <= VK_F12) return vk - VK_F1 + 58;
    struct { int vk; int hid; } map[] = {
        {VK_RETURN, 40}, {VK_ESCAPE, 41}, {VK_BACK, 42}, {VK_TAB, 43},
        {VK_SPACE, 44}, {VK_DELETE, 76}, {VK_INSERT, 73}, {VK_HOME, 74},
        {VK_END, 77}, {VK_PRIOR, 75}, {VK_NEXT, 78},
        {VK_LEFT, 80}, {VK_RIGHT, 79}, {VK_UP, 82}, {VK_DOWN, 81},
        {VK_CAPITAL, 57},
        {VK_OEM_3, 53}, {VK_OEM_MINUS, 45}, {VK_OEM_PLUS, 46},
        {VK_OEM_4, 47}, {VK_OEM_6, 48}, {VK_OEM_5, 49},
        {VK_OEM_1, 51}, {VK_OEM_7, 52},
        {VK_OEM_COMMA, 54}, {VK_OEM_PERIOD, 55}, {VK_OEM_2, 56},
        {0, -1}
    };
    for (int i = 0; map[i].vk != 0; i++) {
        if (map[i].vk == vk) return map[i].hid;
    }
    return -1;
}

LRESULT CALLBACK KeyConfigManager::keyboardHookProc(int nCode, WPARAM wParam, LPARAM lParam)
{
    if (nCode >= 0 && g_hookInstance) {
        KBDLLHOOKSTRUCT *p = reinterpret_cast<KBDLLHOOKSTRUCT *>(lParam);
        int vk = p->vkCode;

        // 手动跟踪修饰键状态（WH_KEYBOARD_LL中GetAsyncKeyState不可靠）
        bool isLeftMod = (vk == VK_LCONTROL || vk == VK_LSHIFT || vk == VK_LMENU || vk == VK_LWIN);
        bool isRightMod = (vk == VK_RCONTROL || vk == VK_RSHIFT || vk == VK_RMENU || vk == VK_RWIN);
        bool isGenericMod = (vk == VK_CONTROL || vk == VK_SHIFT || vk == VK_MENU);
        bool isWinKey = (vk == VK_LWIN || vk == VK_RWIN);

        if (isLeftMod || isRightMod || isGenericMod || isWinKey) {
            int bit = 0;
            // 直接左右 VK 码
            if (vk == VK_LCONTROL) bit = 0x01;
            else if (vk == VK_LSHIFT) bit = 0x02;
            else if (vk == VK_LMENU) bit = 0x04;
            else if (vk == VK_LWIN) bit = 0x08;
            else if (vk == VK_RCONTROL) bit = 0x10;
            else if (vk == VK_RSHIFT) bit = 0x20;
            else if (vk == VK_RMENU) bit = 0x40;
            else if (vk == VK_RWIN) bit = 0x80;
            // 通用 VK 码：用 LLKHF_EXTENDED 标志区分左右
            else if (isGenericMod) {
                bool extended = (p->flags & LLKHF_EXTENDED) != 0;
                if (vk == VK_CONTROL) bit = extended ? 0x10 : 0x01;
                else if (vk == VK_SHIFT) bit = extended ? 0x20 : 0x02;
                else if (vk == VK_MENU) bit = extended ? 0x40 : 0x04;
            }

            qDebug() << "[Hook] mod event: vk=" << vk << "flags=" << p->flags << "bit=" << bit << "isLeft=" << isLeftMod << "isRight=" << isRightMod << "isGeneric=" << isGenericMod;

            if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) {
                g_hookInstance->m_captureMods |= bit;
                // 按下：更新 m_lastMod 并重启定时器
                int mod = g_hookInstance->m_captureMods;
                g_hookInstance->m_lastMod = mod;
                g_hookInstance->m_modifierTimer.start(500);
                qDebug() << "[Hook] modifier pressed: mod=" << mod << "vk=" << vk;
            } else if (wParam == WM_KEYUP || wParam == WM_SYSKEYUP) {
                g_hookInstance->m_captureMods &= ~bit;
                // 释放：只清位，不更新 m_lastMod，不定时器
            }
            return CallNextHookEx(nullptr, nCode, wParam, lParam);
        }

        // 非修饰键按下：捕获模式写入配置；非捕获模式识别物理按键并弹出设置
        if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) {
            int hidCode = vkToHid(vk);
            if (hidCode >= 0) {
                int ki = g_hookInstance->m_captureIndex;
                int mod = g_hookInstance->m_captureMods;
                if (ki != -1) {
                    qDebug() << "[Hook] key pressed: vk=" << vk << "hid=" << hidCode << "mod=" << mod << "target=" << ki;
                    g_hookInstance->finishCapture(mod, hidCode);
                } else {
                    int found = g_hookInstance->findKeyIndexByHid(mod, hidCode);
                    // 只在应用处于前台时触发，避免干扰其它程序
                    if (found >= 0 && QGuiApplication::applicationState() == Qt::ApplicationActive) {
                        qDebug() << "[Hook] physical key detected: keyIndex=" << found << "mod=" << mod << "hid=" << hidCode;
                        emit g_hookInstance->physicalKeyPressed(found);
                    }
                }
            }
        }
    }
    return CallNextHookEx(nullptr, nCode, wParam, lParam);
}

void KeyConfigManager::onModifierTimeout()
{
    qDebug() << "[Hook] modifier timeout: captureIndex=" << m_captureIndex << "lastMod=" << m_lastMod;
    if (m_captureIndex == -1) return;
    int mod = m_lastMod;
    if (mod != 0)
        finishCapture(mod, 0);   /* 纯修饰键(无主键)也是合法快捷键 */
}
#endif


KeyConfigManager::KeyConfigManager(QObject *parent)
    : QObject(parent)
    , m_portDetector(nullptr)
    , m_lightMonitor(nullptr)   /* 09-03: 由 main.cpp 经 setLightMonitor 注入 */
    , m_process(nullptr)
    , m_busy(false)
    , m_lastDone(false)
    , m_bleConfigOp(false)  /* 09-03: 当前进程非 BLE 配置 */
    , m_bleAutoReadPending(false) /* 09-03: 非 BLE 自动读 */
    , m_captureIndex(-1)
    , m_captureMods(0)
    , m_prevMods(0)
    , m_prevKeyState{}
    , m_airMouseMode(1)   /* 默认：按住移动 (hold)，与固件 default_config 一致 */
    , m_airMouseSpeed(1)
    , m_slot(0)           /* 08-27: 默认编辑槽1 */
    , m_sleepMin(45)      /* 08-27: 默认 45 分钟空闲休眠(与固件 default_config 一致) */
    , m_autoReadDone(false) /* 08-31: 尚未自动读取 */
    , m_configRead(false) /* 08-31: 尚未读到有效配置 */
    , m_shakeEnabled(0)     /* 09-03: 摇一摇默认关(与固件一致: 升级后行为不变) */
    , m_shakeSens(1)        /* 09-03: 默认"适中"(与固件 KEY_CONFIG_SHAKE_SENS_DEFAULT 一致) */
    , m_shakeAction("none") /* 09-03: 默认不触发任何键 */
    , m_shakeModifier(0)
    , m_shakeKeycode(0)
    , m_l2Key({})           /* 09-05: L2/L3 默认在下方构造体赋值(鼠标左/右键) */
    , m_l3Key({})
#ifdef Q_OS_WIN
    , m_keyboardHook(nullptr)
    , m_modifierTimer()
    , m_lastMod(0)
#endif
    , m_currentOp(OP_NONE)
    , m_previewProcess(nullptr)
    , m_previewPending(false)
    , m_previewOff(false)
    , m_previewKeyIndex(-1)
{
    m_pythonPath = findPython();
    m_previewTimer.setInterval(40);   // ~25Hz 上限，避免拖拽时刷爆设备
    m_previewTimer.setSingleShot(false);
    connect(&m_previewTimer, &QTimer::timeout, this, &KeyConfigManager::onPreviewTick);
    m_previewTimer.start();

    /* 自动读取延迟器：设备一连上并不立刻读，而是等 150ms 让 QML 的 Connections
     * 完全建立好再接 configReadComplete；否则首帧连接如果发生得太早，QML 还没
     * 准备好，读到的配置不会显示在界面上。 */
    m_autoReadTimer.setSingleShot(true);
    m_autoReadTimer.setInterval(150);
    /* 10-04: 改走 tryAutoRead()（原来是直接 readConfig）——
     * 需要在里面判断"serve 是否就绪"，serve 未就绪就再等等（见 onLightChannelChanged）。 */
    connect(&m_autoReadTimer, &QTimer::timeout, this, &KeyConfigManager::tryAutoRead);

    for (int i = 0; i < 3; i++) {
        QJsonObject obj;
        obj["id"] = i;
        obj["name"] = KEY_NAMES[i];
        obj["action"] = "keyboard";
        obj["modifier"] = 0;
        obj["keycode"] = 0;
        obj["rgb_enabled"] = true;
        obj["rgb_color"] = 0x20A0FF;
        obj["rgb_brightness"] = 15;   /* 默认亮度 15%，与固件 default_config 一致 */
        /* 09-07: 双击/长按默认未设置(固件读到 0xFF/NONE 均不触发) */
        obj["dbl_action"] = "none";
        obj["dbl_modifier"] = 0;
        obj["dbl_keycode"] = 0;
        obj["lng_action"] = "none";
        obj["lng_modifier"] = 0;
        obj["lng_keycode"] = 0;
        /* 09-07: 手势"单击"独立于常规键(keys[] 主键另存 action/modifier/keycode)。
         * 常规(直通)模式编辑主键; 手势模式"单击"编辑 tap_* —— 两者互不干扰。 */
        obj["tap_action"] = "none";
        obj["tap_modifier"] = 0;
        obj["tap_keycode"] = 0;
        obj["c_mode"] = 0;   /* 09-07: 显式模式默认"常规(直通)" —— 按下即发/松开即发, 与历史行为一致 */
        m_keyConfigs.append(obj);
    }

    /* 09-05: L2/L3 默认 = 历史硬编码行为(鼠标左/右键), L1 默认 = 空中鼠标开关 */
    m_l2Key = defaultLKey(1);
    m_l3Key = defaultLKey(2);
    m_l1Key = defaultL1Key();
    m_ecCwKey = defaultEcKey(5);
    m_ecPressKey = defaultEcKey(4);
    /* 10-05 出厂默认 = 【手势模式】(ec_mode=1) —— 与固件 key_config.c 的
     * ec_press_mode=1 / ec_press_tap_key 一致:
     *   单击 = 鼠标中键(与常规模式体验一致, 老用户无感);
     *   双击/长按 = 未设置, 留给用户自定义。 */
    m_ecPressKey["ec_mode"]     = 1;
    m_ecPressKey["tap_action"]  = "mouse";
    m_ecPressKey["tap_modifier"] = 0;
    m_ecPressKey["tap_keycode"] = 4;
    m_ecCcwKey = defaultEcKey(6);
    /* 10-05 按压滚动(按住旋钮旋转) 出厂默认 = LCtrl+[ / LCtrl+]:
     * 与固件 key_config.c 的 ec_cw_press_key/ec_ccw_press_key 默认严格一致
     * (KEY_ACTION_KEYBOARD, modifier=0x01, keycode=0x2F('[') / 0x30(']'))。 */
    m_ecCwPressKey = ecKeyFromShortcut(0x01, 0x2F);
    m_ecCcwPressKey = ecKeyFromShortcut(0x01, 0x30);

#ifdef Q_OS_WIN
    g_hookInstance = this;
    m_keyboardHook = SetWindowsHookExW(WH_KEYBOARD_LL, keyboardHookProc, GetModuleHandleW(nullptr), 0);
#endif
}

KeyConfigManager::~KeyConfigManager()
{
    m_previewTimer.stop();
#ifdef Q_OS_WIN
    if (m_keyboardHook) {
        UnhookWindowsHookEx(m_keyboardHook);
        m_keyboardHook = nullptr;
    }
    g_hookInstance = nullptr;
#endif
    if (m_process && m_process->state() != QProcess::NotRunning) {
        m_process->kill();
        m_process->waitForFinished(2000);
    }
    if (m_previewProcess && m_previewProcess->state() != QProcess::NotRunning) {
        m_previewProcess->kill();
        m_previewProcess->waitForFinished(2000);
    }
}

QString KeyConfigManager::findPython()
{
    // 优先使用程序目录下的 python/（绿色版捆绑的 Python）
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

QString KeyConfigManager::findWorkerScript()
{
    QString appDir = QCoreApplication::applicationDirPath();
    QString scriptName = "config_worker.py";

    QString path1 = appDir + "/" + scriptName;
    if (QFile::exists(path1)) return path1;

    // scripts now live in workers/
    QString path1w = appDir + "/workers/" + scriptName;
    if (QFile::exists(path1w)) return path1w;

    QString path2 = appDir + "/../" + scriptName;
    if (QFile::exists(path2)) return path2;

    QString path2w = appDir + "/../workers/" + scriptName;
    if (QFile::exists(path2w)) return path2w;

    QString path3 = appDir + "/../../" + scriptName;
    if (QFile::exists(path3)) return path3;

    QString path4 = appDir + "/../../../" + scriptName;
    if (QFile::exists(path4)) return path4;

    return scriptName;
}

/* 09-03: BLE 通道配置 worker(一次性进程: 连接 -> 读写配置 -> 断开)。
 * 与 findWorkerScript 同策略搜索(workers/ 或 appDir 平级)。 */
QString KeyConfigManager::findBleConfigWorker()
{
    QString appDir = QCoreApplication::applicationDirPath();
    QString scriptName = "ble_config_worker.py";
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

/* 09-03: 统一 worker 启动(USB/BLE 双通道共用)。
 * 读/写/重置与预览共用一套进程生命周期管理, 消除三处重复代码。 */
void KeyConfigManager::launchWorker(const QStringList &args)
{
    m_process = new QProcess(this);
    connect(m_process, &QProcess::readyReadStandardOutput, this, &KeyConfigManager::onProcessReadyRead);
    connect(m_process, &QProcess::readyReadStandardError, this, [this]() {
        qDebug() << "Config stderr:" << QString::fromUtf8(m_process->readAllStandardError());
    });
    connect(m_process, &QProcess::finished, this, &KeyConfigManager::onProcessFinished);
    connect(m_process, &QProcess::errorOccurred, this, &KeyConfigManager::onProcessError);
    m_process->setWorkingDirectory(QCoreApplication::applicationDirPath());
    m_process->start(m_pythonPath, args);
}

/* 09-03: BLE 配置独占会话入口。Windows 同一 BLE 设备的 GATT 连接一次只容一个
 * 客户端进程 —— 先暂停 LightMonitor 的常驻 serve(它占着连接会让本 worker 的
 * 服务发现失败), worker 结束后由 finishBleConfigIfNeeded 恢复 serve。 */
void KeyConfigManager::launchBleConfig(const QStringList &args)
{
    if (m_lightMonitor)
        m_lightMonitor->pauseBleServe();
    m_bleConfigOp = true;
    launchWorker(args);
}

/* 09-03: BLE 配置 worker 结束(成功/失败/崩溃)统一收尾: 恢复 LED serve。 */
void KeyConfigManager::finishBleConfigIfNeeded()
{
    if (!m_bleConfigOp)
        return;
    m_bleConfigOp = false;
    if (m_lightMonitor)
        m_lightMonitor->resumeBleServe();
}

/* 09-03: 当前是否应走 BLE 通道(蓝牙已连, 且无 USB —— USB 永远优先)。 */
bool KeyConfigManager::bleChannelLive() const
{
    return m_lightMonitor && m_lightMonitor->channel() == QStringLiteral("ble");
}

/* 从共享探测器取当前识别到的 VibeKey 设备端口 */
QString KeyConfigManager::currentPort()
{
    return m_portDetector ? m_portDetector->devicePort() : QString();
}

void KeyConfigManager::setPortDetector(PortDetector *detector)
{
    m_portDetector = detector;
    if (!m_portDetector)
        return;

    /* 08-31: 设备一连上就自动读一次按键配置。
     * 与 setSlot() 的"切槽即读"保持一致：连上后界面立刻显示设备真实配置，
     * 不必手动点"读取"，也避免停留在上一次的陈旧数据。
     * Qt::UniqueConnection 防止重复注入时接多次。 */
    connect(m_portDetector, &PortDetector::deviceConnectedChanged,
            this, &KeyConfigManager::onDeviceConnectedChanged,
            Qt::UniqueConnection);
    /* 08-31: 设备槽位变化（用户在设备端按切槽键）-> 同步更新上位机显示 */
    connect(m_portDetector, &PortDetector::deviceSlotChanged,
            this, &KeyConfigManager::onDeviceSlotChanged,
            Qt::UniqueConnection);

    /* 竞态兜底：探测是异步的，若本注入发生在探测完成之后（设备早已在线），
     * deviceConnectedChanged 不会再触发一次，这里立即补读。 */
    if (m_portDetector->deviceConnected())
        onDeviceConnectedChanged();
}

void KeyConfigManager::setLightMonitor(LightMonitor *monitor)
{
    m_lightMonitor = monitor;
    if (!m_lightMonitor)
        return;
    /* 09-03: BLE 通道变化 -> 未插 USB 时蓝牙连上自动读配置 / 全断复位标记。
     * 插着 USB 的场景自动读由 PortDetector(deviceConnectedChanged) 驱动, 不重复。 */
    connect(m_lightMonitor, &LightMonitor::channelChanged,
            this, &KeyConfigManager::onLightChannelChanged,
            Qt::UniqueConnection);
    /* ★ 10-04: serve READY 信号 —— 启动自动读据此从"定时 150ms 就读"
     * 改为"等 serve 握手完成再读"，否则会杀掉正在连接中的 serve ⇒ AccessDenied。 */
    connect(m_lightMonitor, &LightMonitor::bleServeReadyChanged,
            this, &KeyConfigManager::onBleServeReady,
            Qt::UniqueConnection);
    /* 竞态兜底: 注入发生在 BLE 早已连上的情形(同 setPortDetector 的补读)。 */
    if (m_lightMonitor->channel() == QStringLiteral("ble"))
        onLightChannelChanged();
}

/* ★ 10-04: 自动读的统一入口（m_autoReadTimer 的槽）。
 * serve 尚未 READY 时**继续等**（重新起一个长一点的定时器），
 * 最多等 BLE_AUTO_READ_MAX_WAIT_MS；超时后无论如何都读，避免永远停在"未读"。 */
void KeyConfigManager::tryAutoRead()
{
    if (!m_bleAutoReadWaitingServe) {
        readConfig();          // 非等待态（USB 或已就绪）：直接读
        return;
    }
    if (m_lightMonitor && m_lightMonitor->bleServeUsable()) {
        doBleAutoRead();
        return;
    }
    if (QDateTime::currentMSecsSinceEpoch() - m_bleAutoReadWaitStart < BLE_AUTO_READ_MAX_WAIT_MS) {
        m_autoReadTimer.start(BLE_AUTO_READ_RETRY_MS);   // 还没就绪，继续等
        return;
    }
    qWarning("[KeyConfig] serve 等待超时(未收到 READY)，仍尝试读取配置");
    m_bleAutoReadWaitingServe = false;
    doBleAutoRead();
}

/* ★ 10-04: 真正执行 BLE 自动读（serve 已就绪，或已超时兜底）。 */
void KeyConfigManager::doBleAutoRead()
{
    m_bleAutoReadWaitingServe = false;
    m_autoReadTimer.stop();
    /* 09-03: 自动读用 0xFF=设备当前激活槽(读"生效配置"并同步 UI 槽 tab);
     * 用户手动点槽 tab/读取按钮则走 readConfig() 的 m_slot 分支(任意槽可读)。 */
    m_bleAutoReadPending = true;
    setStatus("BLE 已连接，正在自动读取配置...");
    readConfig();
}

/* ★ 10-04: serve 握手完成信号 —— 若正在等它，立刻读配置。 */
void KeyConfigManager::onBleServeReady()
{
    if (!m_bleAutoReadWaitingServe)
        return;
    if (m_lightMonitor && m_lightMonitor->bleServeUsable()) {
        qDebug() << "[KeyConfig] serve 已就绪，立即执行推迟的自动读";
        doBleAutoRead();
    }
}

void KeyConfigManager::onLightChannelChanged()
{
    if (!m_lightMonitor)
        return;
    QString ch = m_lightMonitor->channel();
    qDebug() << "[KeyConfig] light channel changed:" << ch;

    if (ch == QStringLiteral("ble")) {
        /* BLE-only 场景才由这里驱动自动读; USB 在时走 USB 通道(onDeviceConnectedChanged) */
        if (m_portDetector && m_portDetector->deviceConnected())
            return;
        if (m_autoReadDone)
            return;   /* 本轮连接已读(BLE 连上只触发一次) */
        m_autoReadDone = true;
        m_autoReadTimer.stop();
        /* ★★ 10-04 启动"无响应"的根因修复：
         * channel=="ble" 事件**早于**常驻 serve 完成 GATT 握手(READY)。
         * 原实现在此直接 start(150ms) 就去读配置，而读配置必须
         * pauseBleServe() 独占连接 ⇒ 把"正在连接中"的 serve 杀掉
         * ⇒ serve 与配置 worker 抢同一条连接 ⇒ AccessDenied
         * ⇒ 界面表现为"已连接"但"操作失败 / 无响应"。
         * 现在改为：**等 serve 真正 READY 之后**再读（onBleServeReady 槽）。
         * 若 serve 始终不就绪，由下面的兜底超时照常触发，不至于永远不读。 */
        if (m_lightMonitor && m_lightMonitor->bleServeUsable()) {
            doBleAutoRead();
        } else {
            qDebug() << "[KeyConfig] BLE 已连但 serve 未就绪，自动读推迟到 READY";
            m_bleAutoReadWaitingServe = true;   // 等 onBleServeReady
            m_bleAutoReadWaitStart = QDateTime::currentMSecsSinceEpoch();
            // 兜底：serve 一直没 ready（如设备拒绝连接）时也要能读，
            // 否则界面永远停在"未读"状态。超时后若 serve 仍未就绪，
            // 就走无 serve 独占的老路（可能失败，但好过完全无响应）。
            m_autoReadTimer.start();
        }
    } else if (ch == QStringLiteral("none")) {
        /* 蓝牙与 USB 全断: 复位标记, 取消尚未执行的延迟读取(与 USB 断开分支一致) */
        m_autoReadDone = false;
        m_autoReadTimer.stop();
        if (m_configRead) {
            m_configRead = false;
            emit configReadChanged();
        }
    }
    /* ch == "usb": USB 连接态, 由 PortDetector 事件统一驱动, 这里不处理 */
}

void KeyConfigManager::onDeviceConnectedChanged()
{
    qDebug() << "[KeyConfig] deviceConnectedChanged: connected="
             << (m_portDetector ? m_portDetector->deviceConnected() : false)
             << "autoReadDone=" << m_autoReadDone;

    if (!m_portDetector || !m_portDetector->deviceConnected()) {
        /* 断开：复位标记，取消尚未执行的延迟读取，下次连上时重新自动读一次 */
        m_autoReadDone = false;
        m_autoReadTimer.stop();
        if (m_configRead) {
            m_configRead = false;
            emit configReadChanged();
        }
        return;
    }
    if (m_autoReadDone)
        return;   /* 本轮连接已读过；电量/版本变化也会触发本信号，不重复读 */

    /* 同步固件当前激活槽位到上位机编辑槽位（0~2），这样首次读取的就是设备真实槽 */
    int devSlot = m_portDetector->deviceSlot();
    int newSlot = (devSlot >= 1 && devSlot <= 3) ? (devSlot - 1) : m_slot;
    if (newSlot < 0) newSlot = 0;
    if (newSlot > 2) newSlot = 2;
    if (m_slot != newSlot) {
        m_slot = newSlot;
        emit slotChanged();
    }

    m_autoReadDone = true;
    /* 给界面一点准备时间，再触发 readConfig()；否则 QML 还没接好信号就读完了。 */
    setStatus("设备已连接，正在自动读取配置...");
    m_autoReadTimer.start();
}

void KeyConfigManager::onDeviceSlotChanged()
{
    if (!m_portDetector || !m_portDetector->deviceConnected())
        return;

    int devSlot = m_portDetector->deviceSlot();
    int newSlot = (devSlot >= 1 && devSlot <= 3) ? (devSlot - 1) : m_slot;
    if (newSlot < 0) newSlot = 0;
    if (newSlot > 2) newSlot = 2;
    if (m_slot == newSlot)
        return;

    m_slot = newSlot;
    emit slotChanged();
    /* 已经初始化过配置的前提下，槽位变化才重新读取；首次连接由 onDeviceConnectedChanged 统一读 */
    if (m_configRead)
        readConfig();
}

void KeyConfigManager::setBusy(bool busy)
{
    if (m_busy != busy) {
        m_busy = busy;
        emit busyChanged();
    }
}

void KeyConfigManager::setStatus(const QString &msg)
{
    m_statusMessage = msg;
    emit statusMessageChanged();
}

void KeyConfigManager::setSlot(int slot)
{
    if (slot < 0) slot = 0;
    if (slot > 2) slot = 2;
    if (m_slot == slot)
        return;
    m_slot = slot;
    emit slotChanged();
    /* 切槽即读取该槽配置(丢弃当前未保存的编辑内容) */
    readConfig();
}

void KeyConfigManager::setSleepMin(int minutes)
{
    /* 只允许合法档位: 0=永不, 45~240。其余按最近档位钳制。 */
    int v = minutes;
    if (v < 0) v = 0;
    if (v > 240) v = 240;
    if (v > 0 && v < 45) v = 45;
    if (m_sleepMin == v)
        return;
    m_sleepMin = v;
    emit sleepMinChanged();
    setStatus("休眠时间已更改，写入后生效");
}

void KeyConfigManager::readConfig()
{
    if (m_process && m_process->state() != QProcess::NotRunning) {
        setStatus("正在处理中...");
        return;
    }
    QString port = currentPort();
    bool bleLive = bleChannelLive();
    if (port.isEmpty() && !bleLive) {
        setStatus("未检测到设备，请先连接设备");
        return;
    }

    setBusy(true);
    if (m_portDetector) m_portDetector->setPortInUse(true);
    m_lastDone = false;
    m_currentOp = OP_READ;
    if (!port.isEmpty()) {
        /* USB 优先(插线必走 USB; 与用户定案一致) */
        setStatus("正在通过 USB 读取配置...");
        QString worker = findWorkerScript();
        qDebug() << "Config worker:" << worker << "python:" << m_pythonPath << "port:" << port << "slot:" << m_slot << "via=USB";
        launchWorker({worker, port, "read", QString::number(m_slot)});
    } else {
        /* 无 USB 且蓝牙已连 -> BLE 通道。槽位由 worker 先发 INFO 取"当前激活槽",
         * 入参槽号仅作占位(worker 输出 SLOT n 供 UI 同步)。 */
        setStatus("正在通过 BLE 读取配置...");
        QString worker = findBleConfigWorker();
        /* 09-03: 自动读(BLE 刚连上)用 0xFF=激活槽(读生效配置+SLOT 同步);
         * 手动读取/切槽用当前 UI 槽 —— 各槽配置独立存 Flash, 无需设备切槽即可读任意槽。 */
        QString slotArg = m_bleAutoReadPending
                ? QStringLiteral("0xFF") : QString::number(m_slot);
        m_bleAutoReadPending = false;
        qDebug() << "Config worker:" << worker << "python:" << m_pythonPath << "via=BLE slotArg=" << slotArg;
        launchBleConfig({worker, "read", slotArg});
    }
}

void KeyConfigManager::writeConfig()
{
    if (m_process && m_process->state() != QProcess::NotRunning) {
        setStatus("正在处理中...");
        return;
    }
    QString port = currentPort();
    bool bleLive = bleChannelLive();
    if (port.isEmpty() && !bleLive) {
        setStatus("未检测到设备，请先连接设备");
        return;
    }

    QJsonObject root;
    root["keys"] = configsToJson();
    root["air_mouse_mode"] = (m_airMouseMode == 1) ? "hold" : "toggle";
    root["air_mouse_speed"] = (m_airMouseSpeed == 0) ? "slow" : ((m_airMouseSpeed == 2) ? "fast" : "medium");
    root["sleep_min"] = m_sleepMin;   /* 08-27: 休眠分钟(0=永不) */
    /* 09-03: 摇一摇。⚠️ air_mouse_dir 故意不写入(交 python 用 0xFF 保持现值) */
    root["shake_enabled"] = (m_shakeEnabled == 1) ? 1 : 0;
    root["shake_sens"] = m_shakeSens;
    QJsonObject shakeKeyObj;
    shakeKeyObj["action"] = m_shakeAction;
    shakeKeyObj["modifier"] = m_shakeModifier;
    shakeKeyObj["keycode"] = m_shakeKeycode;
    root["shake_key"] = shakeKeyObj;
    /* 09-05: L2/L3/L1 侧键快捷键(worker 组 payload 尾部 27B; 旧固件忽略, 双向兼容) */
    root["l2_key"] = QJsonObject::fromVariantMap(m_l2Key);
    root["l3_key"] = QJsonObject::fromVariantMap(m_l3Key);
    root["l1_key"] = QJsonObject::fromVariantMap(m_l1Key);
    root["ec_cw_key"] = QJsonObject::fromVariantMap(m_ecCwKey);
    root["ec_press_key"] = QJsonObject::fromVariantMap(m_ecPressKey);
    root["ec_ccw_key"] = QJsonObject::fromVariantMap(m_ecCcwKey);
    /* 10-04: EC 按压滚动上/下(worker 追加 payload 尾部 18B; 旧固件忽略, 双向兼容) */
    root["ec_cw_press_key"] = QJsonObject::fromVariantMap(m_ecCwPressKey);
    root["ec_ccw_press_key"] = QJsonObject::fromVariantMap(m_ecCcwPressKey);
    QJsonDocument doc(root);
    QString tmpPath = QDir::tempPath() + "/vibekey_config.json";
    QFile tmpFile(tmpPath);
    tmpFile.open(QIODevice::WriteOnly);
    tmpFile.write(doc.toJson());
    tmpFile.close();

    setBusy(true);
    if (m_portDetector) m_portDetector->setPortInUse(true);
    m_currentOp = OP_WRITE;
    if (!port.isEmpty()) {
        setStatus("正在通过 USB 写入配置...");
        QString worker = findWorkerScript();
        qDebug() << "Config worker:" << worker << "port:" << port << "slot:" << m_slot << "via=USB";
        launchWorker({worker, port, "write", tmpPath, QString::number(m_slot)});
    } else {
        setStatus("正在通过 BLE 写入配置...");
        QString worker = findBleConfigWorker();
        qDebug() << "Config worker:" << worker << "via=BLE slot=" << m_slot;
        launchBleConfig({worker, "write", tmpPath, QString::number(m_slot)});
    }
}

void KeyConfigManager::resetConfig()
{
    if (m_process && m_process->state() != QProcess::NotRunning) {
        setStatus("正在处理中...");
        return;
    }
    QString port = currentPort();
    bool bleLive = bleChannelLive();
    if (port.isEmpty() && !bleLive) {
        setStatus("未检测到设备，请先连接设备");
        return;
    }

    setBusy(true);
    if (m_portDetector) m_portDetector->setPortInUse(true);
    m_currentOp = OP_RESET;
    if (!port.isEmpty()) {
        setStatus("正在通过 USB 恢复默认配置...");
        QString worker = findWorkerScript();
        launchWorker({worker, port, "reset", QString::number(m_slot)});
    } else {
        setStatus("正在通过 BLE 恢复默认配置...");
        QString worker = findBleConfigWorker();
        launchBleConfig({worker, "reset", QString::number(m_slot)});
    }
}

/* 09-07: C 键字段前缀 —— gesture 1/2 固定 dbl_/lng_; gesture 0 按模式分派:
 * 常规(直通)读主键 keys[](""), 手势模式读独立"单击"块 tap_。
 * 主键与手势单击互不干扰: 常规模式改键不影响手势单击, 反之亦然。 */
static QString cGesturePrefix(int keyIndex, int gesture, const QVariantList &keys)
{
    switch (gesture) {
    case 1:  return QStringLiteral("dbl_");
    case 2:  return QStringLiteral("lng_");
    default:
        if (keyIndex >= 0 && keyIndex < keys.size() &&
            keys[keyIndex].toJsonObject()["c_mode"].toInt(0) == 1)
            return QStringLiteral("tap_");
        return QString();
    }
}

void KeyConfigManager::setKeyAction(int keyIndex, int gesture, const QString &action)
{
    if (keyIndex >= 0 && keyIndex < m_keyConfigs.size()) {
        const QString pfx = cGesturePrefix(keyIndex, gesture, m_keyConfigs);
        QJsonObject obj = m_keyConfigs[keyIndex].toJsonObject();
        /* 09-07 加固: C 键 action 字段存【大类】(mouse/keyboard/multimedia/none),
         * 细分(左/右/中/滚轮)靠 keycode。收到细分字符串(历史调用曾直接传
         * mouse_left/wheel_up..., 会把 action 存成细分值导致显示层派生为
         * none"弹到无动作")时归一为大类+keycode —— 与 setEcKeyAction 同策略。 */
        QString act = action;
        int mouseKc = 0;
        if (action == QStringLiteral("mouse_left"))      { act = QStringLiteral("mouse"); mouseKc = 1; }
        else if (action == QStringLiteral("mouse_right")) { act = QStringLiteral("mouse"); mouseKc = 2; }
        else if (action == QStringLiteral("mouse_middle")){ act = QStringLiteral("mouse"); mouseKc = 4; }
        else if (action == QStringLiteral("wheel_up"))    { act = QStringLiteral("mouse"); mouseKc = 5; }
        else if (action == QStringLiteral("wheel_down"))  { act = QStringLiteral("mouse"); mouseKc = 6; }
        obj[pfx + "action"] = act;
        if (act == "keyboard") {
            obj[pfx + "modifier"] = 0;
            obj[pfx + "keycode"] = 0;
        } else if (act == "mouse") {
            obj[pfx + "keycode"] = mouseKc ? mouseKc : 1;
        } else if (act == "multimedia") {
            obj[pfx + "keycode"] = 0xE9;
        } else {
            obj[pfx + "modifier"] = 0;
            obj[pfx + "keycode"] = 0;
        }
        m_keyConfigs[keyIndex] = obj;
        emit keyConfigsChanged();
    }
}

void KeyConfigManager::setKeyModifier(int keyIndex, int gesture, int modifier)
{
    if (keyIndex >= 0 && keyIndex < m_keyConfigs.size()) {
        const QString pfx = cGesturePrefix(keyIndex, gesture, m_keyConfigs);
        QJsonObject obj = m_keyConfigs[keyIndex].toJsonObject();
        obj[pfx + "modifier"] = modifier;
        m_keyConfigs[keyIndex] = obj;
        emit keyConfigsChanged();
    }
}

void KeyConfigManager::setKeyKeycode(int keyIndex, int gesture, int keycode)
{
    if (keyIndex >= 0 && keyIndex < m_keyConfigs.size()) {
        const QString pfx = cGesturePrefix(keyIndex, gesture, m_keyConfigs);
        QJsonObject obj = m_keyConfigs[keyIndex].toJsonObject();
        obj[pfx + "keycode"] = keycode;
        m_keyConfigs[keyIndex] = obj;
        emit keyConfigsChanged();
    }
}

void KeyConfigManager::setKeyMode(int keyIndex, int mode)
{
    /* 09-07: C 键显式模式。只改模式不碰双击/长按内容 —— 常规模式固件直接无视
     * 手势配置(存而不用), 切回手势模式时旧配置原样恢复。 */
    if (keyIndex < 0 || keyIndex >= m_keyConfigs.size())
        return;
    int v = (mode == 1) ? 1 : 0;
    QJsonObject obj = m_keyConfigs[keyIndex].toJsonObject();
    if (obj["c_mode"].toInt(-1) == v)
        return;
    obj["c_mode"] = v;
    m_keyConfigs[keyIndex] = obj;
    setStatus(QString("KEY C%1 已设为%2模式，写入后生效")
              .arg(keyIndex + 1).arg(v == 0 ? "常规(直通)" : "手势"));
    emit keyConfigsChanged();
}

void KeyConfigManager::setKeyRgbEnabled(int keyIndex, bool enabled)
{
    if (keyIndex >= 0 && keyIndex < m_keyConfigs.size()) {
        QJsonObject obj = m_keyConfigs[keyIndex].toJsonObject();
        obj["rgb_enabled"] = enabled;
        m_keyConfigs[keyIndex] = obj;
        emit keyConfigsChanged();
    }
}

void KeyConfigManager::setKeyRgbColor(int keyIndex, int r, int g, int b)
{
    if (keyIndex >= 0 && keyIndex < m_keyConfigs.size()) {
        QJsonObject obj = m_keyConfigs[keyIndex].toJsonObject();
        obj["rgb_color"] = ((r & 0xFF) << 16) | ((g & 0xFF) << 8) | (b & 0xFF);
        m_keyConfigs[keyIndex] = obj;
        emit keyConfigsChanged();
    }
}

void KeyConfigManager::setKeyRgbBrightness(int keyIndex, int brightness)
{
    if (keyIndex >= 0 && keyIndex < m_keyConfigs.size()) {
        QJsonObject obj = m_keyConfigs[keyIndex].toJsonObject();
        obj["rgb_brightness"] = brightness;
        m_keyConfigs[keyIndex] = obj;
        emit keyConfigsChanged();
    }
}

void KeyConfigManager::previewKeyRgb(int keyIndex)
{
    /* 读/写进行中时跳过，避免抢占同一串口（原逻辑保留） */
    if (m_process && m_process->state() != QProcess::NotRunning)
        return;
    QString port = currentPort();
    /* ★10-04 同 ledTestSet/onPreviewTick：currentPort() 只查 USB 串口，
     *   蓝牙通道下它恒为空 ⇒ 原来这里直接 return，按键设置里的"颜色预览"在
     *   BLE 下**从来没生效过**（用户拖调色器看不到灯变化）。 */
    if (port.isEmpty() && !bleChannelLive())
        return;
    if (keyIndex < 0 || keyIndex >= m_keyConfigs.size())
        return;

    /* 不立即发进程，而是标记“有待发的最新预览”，交给合并定时器以 ~25Hz 节流下发。
       多次高频调用（拖拽）只会刷新 m_previewKeyIndex / pending 标志，最终只发最新一次，
       既不会刷爆串口，也保证松手后 LED 落在最终颜色。 */
    m_ledClearQueue.clear();      /* 10-05: 新预览优先，取消未发完的"全部熄灭"队列 */
    m_previewKeyIndex = keyIndex;
    m_previewOff = false;
    m_previewPending = true;
}

void KeyConfigManager::onPreviewTick()
{
    /* 10-05: 除常规 pending 外，还要处理「全部熄灭」的逐灯补发队列 */
    if (!m_previewPending && m_ledClearQueue.isEmpty())
        return;
    /* 上一次预览进程还没结束（串口仍忙）就先不重复开进程，等下个 tick 重试，
       这样绝不会有两个预览进程并发抢同一个 COM 口 */
    if (m_previewProcess && m_previewProcess->state() != QProcess::NotRunning)
        return;

    /* 读/写占用串口时暂缓下发，保留 pending 等下一个 tick 重试（不丢预览） */
    if (m_process && m_process->state() != QProcess::NotRunning)
        return;

    /* 10-05「全部熄灭」逐灯补发：取队首作为本次下发的 idx，参数覆盖为"发黑色"。
     * ★ 必须在上面"进程忙/串口忙"两个早退**之后**才出队 —— 否则本次不发，
     *   idx 一旦出了队就丢了，那颗灯永远关不掉。 */
    if (!m_ledClearQueue.isEmpty()) {
        m_previewKeyIndex = m_ledClearQueue.takeFirst();
        m_previewOff      = true;
        m_previewLedTest  = true;
        m_previewPending  = true;
    }
    /* ★★ 10-04 根因修复：这里**不能**只判 port 非空！
     * currentPort() 只查 USB 串口，蓝牙通道下它**恒为空** ⇒ 原来这行
     *   if (port.isEmpty()) { m_previewPending = false; return; }
     * 会把 BLE 预览**全部静默丢弃**（连 m_previewOff 的黑灯都发不出去）——
     * 这就是"USB 能亮、BLE 完全不亮"的根因。
     * 正确判据与 ledTestSet() / readConfig() 一致：**USB 有串口 或 BLE 在线**，任一即可。
     * ⚠️ 之前只在 ledTestSet() 放行了 BLE，漏了这里 —— 同一类判据要一次改全。 */
    QString port = currentPort();
    if (port.isEmpty() && !bleChannelLive()) {
        /* 走到这里说明既没 USB 也没 BLE —— 命令发不出去。让用户看到原因，
         * 而不是"点了没反应"（10-04 连续多轮都在这个盲区里排查）。 */
        qDebug() << "[LedTest] preview ABORT: no device (port empty, BLE not live)";
        setStatus("未检测到设备（USB 未插且蓝牙未连）");
        m_previewPending = false;
        m_ledClearQueue.clear();   /* 10-05: 无设备，放弃逐灯补发队列 */
        return;
    }

    int keyIndex = m_previewKeyIndex;
    if (keyIndex < 0 || keyIndex >= m_keyConfigs.size())
        return;

    bool en;
    int r, g, b, bri;
    if (m_previewOff) {
        /* 熄灭覆盖：发黑色（en=1,r=g=b=0）把该键关掉 */
        en = true; r = 0; g = 0; b = 0; bri = 0;
    } else if (m_previewLedTest) {
        /* 10-04 灯效测试态：取临时色/亮度，不碰用户按键配置。
         * 亮度用用户意图值下发（0~100），固件会再压到 15% —— 见 ws2812b.c。 */
        if (keyIndex < 0 || keyIndex > 2) {
            m_previewPending = false;
            return;
        }
        en  = m_ledTestActive[keyIndex];
        int c = m_ledTestColor[keyIndex];
        r = (c >> 16) & 0xFF;
        g = (c >> 8) & 0xFF;
        b =  c        & 0xFF;
        bri = m_ledTestBrightness[keyIndex];
    } else {
        QJsonObject obj = m_keyConfigs[keyIndex].toJsonObject();
        en = obj["rgb_enabled"].toBool();
        int color = obj["rgb_color"].toInt();
        r = (color >> 16) & 0xFF;
        g = (color >> 8) & 0xFF;
        b = color & 0xFF;
        bri = obj["rgb_brightness"].toInt();
    }

    if (!m_previewProcess) {
        m_previewProcess = new QProcess(this);
        connect(m_previewProcess, &QProcess::finished, this, &KeyConfigManager::onPreviewFinished);
        connect(m_previewProcess, &QProcess::errorOccurred, this, &KeyConfigManager::onPreviewFinished);
    }
    m_previewProcess->setWorkingDirectory(QCoreApplication::applicationDirPath());
    /* 10-04 排障 + ★关键修复：预览命令要**按通道选 worker**。
     *
     * 此前这里硬编码 findWorkerScript()（= USB 的 config_worker.py），
     * 于是**蓝牙通道下 RGB 预览完全发不出去**（worker 会因缺 port 参数而失败），
     * 表现就是"按亮/灭固件一点反应都没有"（10-04 真机确认通道=BLE 时复现）。
     *
     * 策略与 readConfig()/writeConfig() 一致（USB 优先，插线必走 USB）：
     *   port 非空 -> USB:  config_worker.py     <port> rgb <k> <on> <r> <g> <b> <bri>
     *   port 为空 -> BLE: ble_config_worker.py  rgb <k> <on> <r> <g> <b> <bri>
     *                          （BLE 无 port 概念，10-04 已给该 worker 补上 rgb 子命令）
     */
    qDebug() << "[LedTest] preview start idx=" << keyIndex
             << "en=" << (en ? 1 : 0)
             << "rgb=" << r << "," << g << "," << b << "bri=" << bri
             << "ledTest=" << (m_previewLedTest ? 1 : 0)
             << "off=" << (m_previewOff ? 1 : 0)
             << "via=" << (port.isEmpty() ? "BLE" : "USB")
             << "py=" << m_pythonPath;
    if (!port.isEmpty()) {
        m_previewProcess->start(m_pythonPath, {findWorkerScript(), port, "rgb",
                                QString::number(keyIndex),
                                QString::number(en ? 1 : 0),
                                QString::number(r), QString::number(g),
                                QString::number(b), QString::number(bri)});
        /* 预览进程占用串口期间暂停 PortDetector 探测，避免探测与预览抢同一独占 COM 口
           导致探测 open() 失败、误报“设备未连接”（与 read/write 行为一致） */
        if (m_portDetector) m_portDetector->setPortInUse(true);
    } else {
        /* ★★ 10-04 快速通道：BLE 下优先**复用常驻 serve 已持有的 GATT 连接**。
         * 0xFF03 配置特征与 0xFF01 LED 特征在同一个 0xFF00 服务里，serve 一次取到，
         * 所以 RGB 预览只需一次 write（**毫秒级**），不必：
         *   ① 启一个 python 一次性进程 ② 重新做 GATT 服务/特征发现
         *   ③ pauseBleServe 独占连接（serve 退出 + Windows 释放 1~2s）
         * 这三点正是原来 1~2s 延迟的来源。
         * serve 不可用（未 ready / 进程已退出）时回退到下面的老路径，保证兼容。 */
        if (m_lightMonitor && m_lightMonitor->sendRgbViaServe(keyIndex, en, r, g, b, bri)) {
            // 已交给 serve，无需起进程；也不要动 m_bleConfigOp / m_previewViaBle
            m_previewPending = false;
            m_previewOff = false;
            m_previewLedTest = false;
            /* 10-05: 逐灯补发队列还有剩 → 下一 tick 继续发下一颗 */
            if (!m_ledClearQueue.isEmpty())
                m_previewPending = true;
            return;
        }
        /* 回退路径：BLE 走一次性 config worker —— 需暂停 serve 独占连接 */
        if (m_lightMonitor)
            m_lightMonitor->pauseBleServe();
        m_bleConfigOp = true;
        m_previewViaBle = true;   // 让 onPreviewFinished 知道要恢复 BLE serve
        m_previewProcess->start(m_pythonPath, {findBleConfigWorker(), "rgb",
                                QString::number(keyIndex),
                                QString::number(en ? 1 : 0),
                                QString::number(r), QString::number(g),
                                QString::number(b), QString::number(bri)});
    }
    m_previewPending = false;
    m_previewOff = false;
    /* 10-04: 本次已按测试态下发，复位标志 —— 否则之后的按键预览会误用
     * m_ledTestColor 而拿到灯效测试的临时色（表现为"改按键配置却没反应"）。 */
    m_previewLedTest = false;

    /* 10-05: 队列还有剩（如「全部熄灭」要发 3 颗）→ 下一 tick 继续补发下一颗。
     * 此时 m_previewProcess 正在跑，下一 tick 会在"进程忙"处早退并保留 pending，
     * 等进程结束后自然发出下一颗。 */
    if (!m_ledClearQueue.isEmpty())
        m_previewPending = true;
}

void KeyConfigManager::onPreviewFinished()
{
    /* 预览进程结束，释放串口占用，探测恢复。期间若又来新请求会在下一 tick 补发。
       进程对象复用，不 delete。errorOccurred 也会进这里（clamp 防止忙计数变负）。 */
    /* 10-04: 读 worker 的 stdout/stderr 落到调试输出 ——
     * worker 会打印 "DONE eff_bri=N"（固件回传的实际限幅值）或 ERROR 原因。
     * 此前预览进程输出**完全没人读**，失败也是静默的，排查只能靠猜。 */
    if (m_previewProcess) {
        const QByteArray pout = m_previewProcess->readAllStandardOutput();
        const QByteArray perr = m_previewProcess->readAllStandardError();
        if (!pout.isEmpty())
            qDebug() << "[LedTest] worker stdout:" << pout.trimmed();
        if (!perr.isEmpty())
            qDebug() << "[LedTest] worker stderr:" << perr.trimmed();
        if (m_previewProcess->exitCode() != 0) {
            qDebug() << "[LedTest] worker FAILED rc=" << m_previewProcess->exitCode();
            setStatus("RGB 下发失败（详见调试输出 [LedTest]）");
        }
    }
    if (m_portDetector) m_portDetector->setPortInUse(false);
    /* 10-04: 走 BLE 的预览要恢复 LED serve（与 launchBleConfig/finishBleConfigIfNeeded
     * 同款收尾），否则 serve 被永久暂停，状态灯会失去联动。
     * ★ 同时必须清 m_bleConfigOp —— 它在预览启动时被置 true（见 onPreviewTick 的 BLE 分支），
     *   而 finishBleConfigIfNeeded() 只挂在 m_process 的回调上，**预览用的是 m_previewProcess**，
     *   不在这里清的话 m_bleConfigOp 会永久卡在 true ⇒ 后续所有 BLE 命令都发不出去。
     *   这正是"USB 能亮、BLE 不亮"的根因（第一个预览命令能发，之后全被占住）。 */
    if (m_previewViaBle) {
        m_previewViaBle = false;
        m_bleConfigOp = false;          // 预览不是"配置会话"，别占着这个标志
        if (m_lightMonitor)
            m_lightMonitor->resumeBleServe();
    }
}

void KeyConfigManager::clearKeyRgb(int keyIndex)
{
    /* 配置完/关弹窗时调用：经合并定时器下发一次黑色，使该键熄灭，
       回到“松开即灭”的空闲态（设备非按压时不应常亮）。 */
    if (keyIndex < 0 || keyIndex >= m_keyConfigs.size())
        return;
    if (m_process && m_process->state() != QProcess::NotRunning)
        return;
    /* ★10-04 同 ledTestSet/onPreviewTick：蓝牙通道下 currentPort() 恒为空，
     *   原来这里会直接 return ⇒ 关弹窗时"熄灯"命令在 BLE 下发不出去（灯会一直亮着）。 */
    if (currentPort().isEmpty() && !bleChannelLive())
        return;
    m_previewKeyIndex = keyIndex;
    m_previewOff = true;
    m_previewPending = true;
}

/* ===================== C1/C2/C3 灯效测试（10-04）==========================
 * 需求：运行状态页要能单独控 C1/C2/C3 三颗 RGB 灯（颜色 + 亮度各自独立），
 *       但亮度**不得超过 15%**，且该限制要在**固件里锁死** —— 硬件主动降到 15%。
 *
 * 分工：
 *   固件 ws2812b.c  s_rgb_cap_brightness()  ← 最终出灯口钳位，软件绕不过（真正的锁）
 *   本文件                                        ← UI 侧收敛 + 测试态管理
 *
 * 与"按键配置"的区别（重要）：
 *   previewKeyRgb() 读的是 m_keyConfigs[] 里的**持久配置**，会改用户配置。
 *   灯效测试是**临时态** —— 不写 m_keyConfigs、不落盘，页面切走即失效。
 *   所以这里用独立的成员保存测试色，经同一个合并定时器下发（复用 ~25Hz 节流，
 *   拖拽调色不会刷爆串口，也不会与按键配置预览抢同一个预览进程）。
 */

/* 固件 ws2812b.c 的 RGB_BRIGHTNESS_MAX_PCT，两边必须一致。
 * ★ 10-04 已恢复限幅：15（产品要求锁死）。排查期间曾临时改成 100 取消限幅，现已改回。
 *    改这里时**必须**同步改固件的 RGB_BRIGHTNESS_MAX_PCT（ws2812b.c）。
 *    固件侧的钳位对象是**亮度参数 bri**（不是已缩放的颜色值）——
 *    否则会出现双重降幅（bri=15 → 255→38→6 = 2.4%，肉眼全黑，10-04 踩过）。
 *    UI 只是提示，真正的限幅在固件。 */
static const int kLedBrightnessHwCapPct = 15;

int KeyConfigManager::ledBrightnessHardwareCap() const
{
    return kLedBrightnessHwCapPct;
}

void KeyConfigManager::ledTestSet(int ledIndex, int r, int g, int b, int brightnessPct)
{
    /* 只认 C1/C2/C3（索引 0/1/2）。越界直接忽略，不做任何下发。
     * ⚠️ 下面的 qDebug 是 10-04 排障用（"按亮灭固件无反应"），
     *    三个静默 return 点各打一条 —— 静默 return 是最难查的一类问题，
     *    宁可吵一点也不要让调用方猜为什么没反应。查完可保留（开销可忽略）。 */
    if (ledIndex < 0 || ledIndex > 2) {
        qDebug() << "[LedTest] set: bad index" << ledIndex;
        return;
    }
    if (m_process && m_process->state() != QProcess::NotRunning) {
        qDebug() << "[LedTest] set: serial busy (read/write in progress)";
        return;
    }
    /* ★10-04 修正：不能用 currentPort().isEmpty() 一刀切拒绝 ——
     * 蓝牙通道下 currentPort() 本就为空（它只查 USB 串口），
     * 那样会导致 BLE 已连时按亮/灭**直接被拦掉**，正是"没反应"的原因之一。
     * 正确判据与 readConfig() 一致：USB 有串口 **或** BLE 通道在线，任一即可。 */
    if (currentPort().isEmpty() && !bleChannelLive()) {
        qDebug() << "[LedTest] set: no device (no USB port and BLE not live)";
        return;
    }
    qDebug() << "[LedTest] set idx=" << ledIndex
             << "rgb=" << QString("%1,%2,%3").arg(r).arg(g).arg(b)
             << "bri=" << brightnessPct << "port=" << currentPort();

    /* UI 侧收敛到 0~100。**注意这不是安全边界** —— 用户可以传 100，
     * 固件仍会压到 15%。这里只是让滑块读数与显示一致。 */
    if (brightnessPct < 0)   brightnessPct = 0;
    if (brightnessPct > 100) brightnessPct = 100;
    r &= 0xFF; g &= 0xFF; b &= 0xFF;

    m_ledTestColor[ledIndex]    = (r << 16) | (g << 8) | b;
    m_ledTestBrightness[ledIndex] = brightnessPct;
    m_ledTestActive[ledIndex]     = true;

    /* 复用按键预览的合并下发通道（同一个串口、同一套节流）。 */
    m_ledClearQueue.clear();      /* 10-05: 用户显式设色 → 取消尚未发完的"全部熄灭"队列 */
    m_previewKeyIndex = ledIndex;
    m_previewOff = false;
    m_previewLedTest = true;      /* 让 onPreviewTick 走测试态数据源 */
    m_previewPending = true;
}

void KeyConfigManager::ledTestClear(int ledIndex)
{
    if (ledIndex < 0 || ledIndex > 2)
        return;
    if (m_process && m_process->state() != QProcess::NotRunning)
        return;
    /* ★10-04 同 ledTestSet：BLE 通道下 currentPort() 为空，不能据此拒绝 */
    if (currentPort().isEmpty() && !bleChannelLive())
        return;
    m_ledTestActive[ledIndex] = false;
    m_previewKeyIndex = ledIndex;
    m_previewOff = true;          /* 发黑色熄灭 */
    m_previewLedTest = true;
    m_previewPending = true;
}

void KeyConfigManager::ledTestClearAll()
{
    for (int i = 0; i < 3; ++i) {
        m_ledTestActive[i] = false;
    }
    /* ★ 10-05 根因修复："全部熄灭"灭不掉。
     * worker/固件一次只认一个 idx（rgb <k> ...），而预览通道是**合并式**的
     * （只有 m_previewKeyIndex/m_previewPending 这几个标量）——
     * 旧实现在 QML 里 for 循环 ledTestClear(0..2)，三次调用互相覆盖，
     * 最终只剩最后一颗（C3）被下发 ⇒ 只有 C3 关掉。
     * 这里改成把 3 个 idx **排队**，交给 onPreviewTick 每 tick 弹一个依次补发。 */
    m_ledClearQueue = QList<int>{0, 1, 2};

    /* 串口忙时 onPreviewTick 会跳过并保留队列（queue 非空即视为待发）；
     * 无设备时它会在那边清空队列放弃。灯效测试是临时态，不额外排队等待。 */
    if (m_process && m_process->state() != QProcess::NotRunning)
        return;
    if (currentPort().isEmpty() && !bleChannelLive())
        return;

    m_previewOff = true;
    m_previewLedTest = true;
    m_previewPending = true;
}

static const QJsonArray keyboardKeyData = QJsonArray::fromVariantList({
    QJsonObject({{"text", "无"}, {"value", 0}}),
    QJsonObject({{"text", "A"}, {"value", 4}}),
    QJsonObject({{"text", "B"}, {"value", 5}}),
    QJsonObject({{"text", "C"}, {"value", 6}}),
    QJsonObject({{"text", "D"}, {"value", 7}}),
    QJsonObject({{"text", "E"}, {"value", 8}}),
    QJsonObject({{"text", "F"}, {"value", 9}}),
    QJsonObject({{"text", "G"}, {"value", 10}}),
    QJsonObject({{"text", "H"}, {"value", 11}}),
    QJsonObject({{"text", "I"}, {"value", 12}}),
    QJsonObject({{"text", "J"}, {"value", 13}}),
    QJsonObject({{"text", "K"}, {"value", 14}}),
    QJsonObject({{"text", "L"}, {"value", 15}}),
    QJsonObject({{"text", "M"}, {"value", 16}}),
    QJsonObject({{"text", "N"}, {"value", 17}}),
    QJsonObject({{"text", "O"}, {"value", 18}}),
    QJsonObject({{"text", "P"}, {"value", 19}}),
    QJsonObject({{"text", "Q"}, {"value", 20}}),
    QJsonObject({{"text", "R"}, {"value", 21}}),
    QJsonObject({{"text", "S"}, {"value", 22}}),
    QJsonObject({{"text", "T"}, {"value", 23}}),
    QJsonObject({{"text", "U"}, {"value", 24}}),
    QJsonObject({{"text", "V"}, {"value", 25}}),
    QJsonObject({{"text", "W"}, {"value", 26}}),
    QJsonObject({{"text", "X"}, {"value", 27}}),
    QJsonObject({{"text", "Y"}, {"value", 28}}),
    QJsonObject({{"text", "Z"}, {"value", 29}}),
    QJsonObject({{"text", "1"}, {"value", 30}}),
    QJsonObject({{"text", "2"}, {"value", 31}}),
    QJsonObject({{"text", "3"}, {"value", 32}}),
    QJsonObject({{"text", "4"}, {"value", 33}}),
    QJsonObject({{"text", "5"}, {"value", 34}}),
    QJsonObject({{"text", "6"}, {"value", 35}}),
    QJsonObject({{"text", "7"}, {"value", 36}}),
    QJsonObject({{"text", "8"}, {"value", 37}}),
    QJsonObject({{"text", "9"}, {"value", 38}}),
    QJsonObject({{"text", "0"}, {"value", 39}}),
    QJsonObject({{"text", "Enter"}, {"value", 40}}),
    QJsonObject({{"text", "Backspace"}, {"value", 42}}),
    QJsonObject({{"text", "Space"}, {"value", 44}}),
    QJsonObject({{"text", "Delete"}, {"value", 76}}),
    QJsonObject({{"text", "Tab"}, {"value", 43}}),
    QJsonObject({{"text", "Esc"}, {"value", 41}}),
    QJsonObject({{"text", "F1"}, {"value", 58}}),
    QJsonObject({{"text", "F2"}, {"value", 59}}),
    QJsonObject({{"text", "F3"}, {"value", 60}}),
    QJsonObject({{"text", "F4"}, {"value", 61}}),
    QJsonObject({{"text", "F5"}, {"value", 62}}),
    QJsonObject({{"text", "F6"}, {"value", 63}}),
    QJsonObject({{"text", "F7"}, {"value", 64}}),
    QJsonObject({{"text", "F8"}, {"value", 65}}),
    QJsonObject({{"text", "F9"}, {"value", 66}}),
    QJsonObject({{"text", "F10"}, {"value", 67}}),
    QJsonObject({{"text", "F11"}, {"value", 68}}),
    QJsonObject({{"text", "F12"}, {"value", 69}})
});

QStringList KeyConfigManager::keyNameList() const
{
    QStringList names;
    for (int i = 0; i < keyboardKeyData.size(); i++) {
        names.append(keyboardKeyData[i].toObject()["text"].toString());
    }
    return names;
}

QVariantList KeyConfigManager::keyValueList() const
{
    QVariantList values;
    for (int i = 0; i < keyboardKeyData.size(); i++) {
        values.append(keyboardKeyData[i].toObject()["value"].toInt());
    }
    return values;
}

int KeyConfigManager::getKeycodeIndex(int keyIndex) const
{
    if (keyIndex < 0 || keyIndex >= m_keyConfigs.size()) return 0;
    int keycode = m_keyConfigs[keyIndex].toJsonObject()["keycode"].toInt();
    for (int i = 0; i < keyboardKeyData.size(); i++) {
        if (keyboardKeyData[i].toObject()["value"].toInt() == keycode)
            return i;
    }
    return 0;
}

QJsonArray KeyConfigManager::configsToJson()
{
    QJsonArray arr;
    for (const QVariant &v : m_keyConfigs) {
        arr.append(v.toJsonObject());
    }
    return arr;
}

void KeyConfigManager::onProcessReadyRead()
{
    // 不在这里处理输出，全部留给 onProcessFinished
}

void KeyConfigManager::onProcessFinished(int exitCode, QProcess::ExitStatus exitStatus)
{
    setBusy(false);
    if (m_portDetector) m_portDetector->setPortInUse(false);

    if (m_process) {
        QByteArray output = m_process->readAllStandardOutput();
        QString text = QString::fromUtf8(output);

        qDebug() << "Config finished, exitCode=" << exitCode << "output=" << text.left(200);

        bool hasDone = text.contains("DONE");
        bool hasJson = false;

        QStringList lines = text.split("\n", Qt::SkipEmptyParts);
        for (const QString &line : lines) {
            QString trimmed = line.trimmed();
            /* 09-03: BLE worker 的 "SLOT n"(1~3=设备当前激活槽) -> 同步 UI 槽 tab。
             * 仅更新高亮不触发重读(该次读写本就按真实槽执行); USB worker 无此行。 */
            if (trimmed.startsWith("SLOT ")) {
                bool ok = false;
                int n = trimmed.mid(5).trimmed().toInt(&ok);
                if (ok && n >= 1 && n <= 3 && m_slot != n - 1) {
                    m_slot = n - 1;
                    emit slotChanged();
                    qDebug() << "[KeyConfig] BLE slot synced:" << n;
                }
            } else if (trimmed.startsWith("VERSION ")) {
                /* 09-03: BLE worker 透传的固件版本号(INFO 响应自带, 与 USB deviceVersion
                 * 同源)。写回 LightMonitor 供连接状态显示拼接 "vX.Y.Z"(蓝牙-only 时也能看到版本)。 */
                QString ver = trimmed.mid(8).trimmed();
                if (m_lightMonitor && !ver.isEmpty())
                    m_lightMonitor->setBleDeviceVersion(ver);
                qDebug() << "[KeyConfig] BLE version:" << ver;
            } else if (trimmed.startsWith("{")) {
                QJsonDocument doc = QJsonDocument::fromJson(trimmed.toUtf8());
                if (!doc.isNull()) {
                    QJsonObject obj = doc.object();
                    QJsonArray keys = obj["keys"].toArray();
                    m_keyConfigs.clear();
                    for (int i = 0; i < keys.size() && i < 3; i++) {
                        m_keyConfigs.append(QVariant(keys[i].toObject()));
                    }
                    QString modeStr = obj["air_mouse_mode"].toString("toggle");
                    QString speedStr = obj["air_mouse_speed"].toString("medium");
                    int newMode = (modeStr == "hold") ? 1 : 0;
                    int newSpeed = (speedStr == "slow") ? 0 : ((speedStr == "fast") ? 2 : 1);
                    int newSleep = obj["sleep_min"].toInt(45);   /* 08-27: 旧固件无此字段->45 */
                    if (m_airMouseMode != newMode) {
                        m_airMouseMode = newMode;
                        emit airMouseModeChanged();
                    }
                    if (m_airMouseSpeed != newSpeed) {
                        m_airMouseSpeed = newSpeed;
                        emit airMouseSpeedChanged();
                    }
                    if (m_sleepMin != newSleep) {
                        m_sleepMin = newSleep;
                        emit sleepMinChanged();
                    }
                    /* 09-03: 摇一摇。旧固件 worker 不回这些字段 -> 保持默认(关/中/无) */
                    int newShakeEn = obj["shake_enabled"].toInt(0);
                    int newShakeSens = obj["shake_sens"].toInt(1);
                    QJsonObject skObj = obj["shake_key"].toObject();
                    QString newShakeAction = skObj["action"].toString("none");
                    int newShakeMod = skObj["modifier"].toInt(0);
                    int newShakeKc = skObj["keycode"].toInt(0);
                    bool shakeDirty = (m_shakeEnabled != newShakeEn)
                            || (m_shakeSens != newShakeSens)
                            || (m_shakeAction != newShakeAction)
                            || (m_shakeModifier != newShakeMod)
                            || (m_shakeKeycode != newShakeKc);
                    if (shakeDirty) {
                        m_shakeEnabled = (newShakeEn == 1) ? 1 : 0;
                        m_shakeSens = (newShakeSens >= 0 && newShakeSens <= 2) ? newShakeSens : 1;
                        m_shakeAction = newShakeAction;
                        m_shakeModifier = newShakeMod;
                        m_shakeKeycode = newShakeKc;
                        emit shakeChanged();
                    }
                    /* 09-05: L2/L3/L1 侧键。旧 worker/旧固件不回这些字段 -> 保持默认 */
                    QVariantMap newL2 = sanitizedLKey(obj["l2_key"].toObject().toVariantMap(), defaultLKey(1));
                    QVariantMap newL3 = sanitizedLKey(obj["l3_key"].toObject().toVariantMap(), defaultLKey(2));
                    QVariantMap newL1 = sanitizedLKey(obj["l1_key"].toObject().toVariantMap(), defaultL1Key(), true);
                    if (m_l2Key != newL2) { m_l2Key = newL2; emit l2KeyChanged(); }
                    if (m_l3Key != newL3) { m_l3Key = newL3; emit l3KeyChanged(); }
                    if (m_l1Key != newL1) { m_l1Key = newL1; emit l1KeyChanged(); }
                    /* 09-05: EC 编码器三手势。旧 worker/旧固件不回 -> 保持默认(滚轮/中键/滚轮) */
                    QVariantMap newEcCw = sanitizedEcKey(obj["ec_cw_key"].toObject().toVariantMap(), defaultEcKey(5), true);
                    QVariantMap newEcPr = withEcPressGestureDefaults(sanitizedEcKey(obj["ec_press_key"].toObject().toVariantMap(), defaultEcKey(4), true));
                    QVariantMap newEcCc = sanitizedEcKey(obj["ec_ccw_key"].toObject().toVariantMap(), defaultEcKey(6), true);
                    /* 10-04: EC 按压滚动上/下。旧 worker/旧固件(回包 304B)不回这两字段
                     * -> 空 map 走 sanitizedEcKey 落到默认(滚轮上/下), 与旋转键一致 */
                    QVariantMap newEcCwPr = sanitizedEcKey(obj["ec_cw_press_key"].toObject().toVariantMap(), defaultEcKey(5), true);
                    QVariantMap newEcCcPr = sanitizedEcKey(obj["ec_ccw_press_key"].toObject().toVariantMap(), defaultEcKey(6), true);
                    if (m_ecCwKey != newEcCw || m_ecPressKey != newEcPr || m_ecCcwKey != newEcCc ||
                        m_ecCwPressKey != newEcCwPr || m_ecCcwPressKey != newEcCcPr) {
                        m_ecCwKey = newEcCw;
                        m_ecPressKey = newEcPr;
                        m_ecCcwKey = newEcCc;
                        m_ecCwPressKey = newEcCwPr;
                        m_ecCcwPressKey = newEcCcPr;
                        emit ecKeysChanged();
                    }
                    qDebug() << "Parsed" << m_keyConfigs.size() << "keys.";
                    for (int k = 0; k < m_keyConfigs.size(); k++) {
                        QJsonObject o = m_keyConfigs[k].toJsonObject();
                        qDebug() << "  key[" << k << "]:" << o["name"].toString() << "action=" << o["action"].toString() << "mod=" << o["modifier"].toInt() << "kc=" << o["keycode"].toInt();
                    }
                    qDebug() << "Air mouse mode=" << m_airMouseMode << "speed=" << m_airMouseSpeed;
                    qDebug() << "Shake en=" << m_shakeEnabled << "sens=" << m_shakeSens
                             << "action=" << m_shakeAction << "mod=" << m_shakeModifier
                             << "kc=" << m_shakeKeycode;
                    emit keyConfigsChanged();
                    hasJson = true;
                }
            }
        }

        qDebug() << "hasJson=" << hasJson << "hasDone=" << hasDone << "keyConfigs size=" << m_keyConfigs.size();
        if (hasDone) {
            setStatus("操作成功");
            switch (m_currentOp) {
            case OP_READ:
                emit configReadComplete();
                if (!m_configRead) {
                    m_configRead = true;
                    emit configReadChanged();
                }
                break;
            case OP_WRITE: emit configWriteComplete(); break;
            case OP_RESET: emit configResetComplete(); break;
            default: emit configReadComplete(); break;
            }
        } else {
            setStatus("操作失败");
            emit configFailed(text.isEmpty() ? "无响应" : text);
        }
        m_process->deleteLater();
        m_process = nullptr;
        finishBleConfigIfNeeded();   /* 09-03: BLE 配置结束恢复 LED serve */
    }
}

void KeyConfigManager::onProcessError(QProcess::ProcessError error)
{
    setBusy(false);
    if (m_portDetector) m_portDetector->setPortInUse(false);
    QString msg;
    switch (error) {
    case QProcess::FailedToStart: msg = "无法启动 (检查Python)"; break;
    case QProcess::Crashed: msg = "进程崩溃"; break;
    case QProcess::Timedout: msg = "超时"; break;
    default: msg = "通信错误"; break;
    }
    setStatus(msg);
    emit configFailed(msg);
    if (m_process) {
        m_process->deleteLater();
        m_process = nullptr;
        finishBleConfigIfNeeded();   /* 09-03: BLE 配置结束恢复 LED serve */
    }
}

void KeyConfigManager::startCapture(int keyIndex, int gesture)
{
    /* 09-07: C 键(0-2)三手势捕获索引 = base + ki*3 + gesture;
     * L 键(3/5/6, 主键快捷键捕获)保持原索引(=卡片索引) —— L 键【三手势】捕获
     * 走 startLKeyGestureCapture(li*3 区)。其余(EC 等)保持原索引。 */
    const int target = (keyIndex >= 0 && keyIndex < 3)
            ? (kCKeyGestureBase + keyIndex * 3 + gesture) : keyIndex;
    qDebug() << "[Capture] startCapture keyIndex=" << keyIndex << "gesture=" << gesture << "target=" << target;
    beginCapture(target);
}

/* 09-07: L 键三手势捕获(which 0=L2 1=L3 2=L1; gesture 0=单击 1=双击 2=长按)。
 * 捕获索引 = kLKeyGestureBase + li*3 + gesture。 */
void KeyConfigManager::startLKeyGestureCapture(int which, int gesture)
{
    if (which < 0 || which > 2 || gesture < 0 || gesture > 2)
        return;
    const int li = lGestureOrdinalFromWhich(which);
    const int target = kLKeyGestureBase + li * 3 + gesture;
    qDebug() << "[Capture] startLKeyGestureCapture which=" << which << "gesture=" << gesture << "target=" << target;
    beginCapture(target);
}

/* 09-07: 捕获公共入口 —— 置索引并复位钩子状态(原 startCapture 主体)。 */
void KeyConfigManager::beginCapture(int target)
{
    // 如果已经在捕获同一个键，不重复启动（防止修饰键触发QML焦点事件重复调用）
    if (m_captureIndex == target) return;
    m_captureIndex = target;
    m_captureMods = 0;
    m_lastMod = 0;
    m_prevKeys.clear();
    memset(m_prevKeyState, 0, sizeof(m_prevKeyState));
#ifdef Q_OS_WIN
    // 连接修饰键超时信号
    if (!m_modifierTimer.isActive()) {
        connect(&m_modifierTimer, &QTimer::timeout, this, &KeyConfigManager::onModifierTimeout, Qt::UniqueConnection);
    }
    m_modifierTimer.stop();
#endif
    emit capturingChanged();
}

void KeyConfigManager::cancelCapture()
{
    m_captureIndex = -1;
    m_captureMods = 0;
    m_prevMods = 0;
    m_lastMod = 0;
    m_prevKeys.clear();
    emit capturingChanged();
}

int KeyConfigManager::findKeyIndexByHid(int modifier, int keycode) const
{
    for (int i = 0; i < m_keyConfigs.size(); ++i) {
        QJsonObject obj = m_keyConfigs[i].toJsonObject();
        if (obj["modifier"].toInt() == modifier && obj["keycode"].toInt() == keycode)
            return i;
    }
    return -1;
}

// pollCapture removed — WH_KEYBOARD_LL hook replaces polling

void KeyConfigManager::setAirMouseMode(int mode)
{
    if (mode < 0) mode = 0;
    if (mode > 1) mode = 1;
    if (m_airMouseMode == mode) return;
    m_airMouseMode = mode;
    emit airMouseModeChanged();
}

void KeyConfigManager::setAirMouseSpeed(int speed)
{
    if (speed < 0) speed = 0;
    if (speed > 2) speed = 2;
    if (m_airMouseSpeed == speed) return;
    m_airMouseSpeed = speed;
    emit airMouseSpeedChanged();
}

/* ===================== 09-03 摇一摇 ===================== */

/* 捕获完成统一收尾: 按目标索引路由到按键或摇一摇快捷键, 复位捕获状态。
 * 键盘钩子(按键按下)与修饰键超时(纯修饰键)两条路径都走这里。 */
void KeyConfigManager::finishCapture(int modifier, int keycode)
{
    int ki = m_captureIndex;
    if (ki == -1)
        return;
    m_captureIndex = -1;
#ifdef Q_OS_WIN
    m_modifierTimer.stop();
#endif
    if (ki >= 0) {
        if (ki == kL1CaptureIndex) {
            setLKeyKey(2, modifier, keycode);       /* 09-05: KEY L1(卡片索引 3) */
        } else if (ki == kEcCwCaptureIndex) {
            setEcKeyKey(0, modifier, keycode);      /* 09-05: EC 顺时针 */
        } else if (ki == kEcPressCaptureIndex) {
            setEcKeyKey(1, modifier, keycode);      /* 09-05: EC 按下 */
        } else if (ki == kEcCcwCaptureIndex) {
            setEcKeyKey(2, modifier, keycode);      /* 09-05: EC 逆时针 */
        } else if (ki == kEcCwPressCaptureIndex) {
            setEcKeyKey(3, modifier, keycode);      /* 10-04: EC 按压滚动上 */
        } else if (ki == kEcCcwPressCaptureIndex) {
            setEcKeyKey(4, modifier, keycode);      /* 10-04: EC 按压滚动下 */
        } else if (ki == kL2CaptureIndex) {
            setLKeyKey(0, modifier, keycode);
        } else if (ki == kL3CaptureIndex) {
            setLKeyKey(1, modifier, keycode);
        } else if (ki >= kCKeyGestureBase && ki < kCKeyGestureBase + 9) {
            /* 09-07: C 键三手势(双击/长按) —— 编码索引反解键位与手势位 */
            const int keyIdx = (ki - kCKeyGestureBase) / 3;
            const int gesture = (ki - kCKeyGestureBase) % 3;
            setKeyAction(keyIdx, gesture, "keyboard");
            setKeyModifier(keyIdx, gesture, modifier);
            setKeyKeycode(keyIdx, gesture, keycode);
        } else if (ki >= kLKeyGestureBase && ki < kLKeyGestureBase + 9) {
            /* 09-07: L 键三手势 —— li 反解 which(0=L1 1=L2 2=L3 序 -> 2/0/1)。
             * 键盘组合键落地到 tap_/dbl_/lng_ 前缀块(独立于主键 l*_key)。 */
            const int li = (ki - kLKeyGestureBase) / 3;
            const int gesture = (ki - kLKeyGestureBase) % 3;
            const int which = (li == 0) ? 2 : (li == 1) ? 0 : 1;   /* L1/L2/L3 */
            QVariantMap m = lKeyMapFor(which);
            const QString pfx = lGesturePrefix(gesture);
            m[pfx + "action"] = "keyboard";
            m[pfx + "modifier"] = modifier & 0xFF;
            m[pfx + "keycode"] = keycode & 0xFF;
            setLKeyMap(which, m);
            setStatus(QString("%1 %2 快捷键已更改，写入后生效")
                      .arg(lKeyName(which))
                      .arg(gesture == 0 ? "单击" : gesture == 1 ? "双击" : "长按"));
        } else if (ki >= kEcPressGestureBase && ki < kEcPressGestureBase + 3) {
            /* 09-07: EC 按下三手势 —— 键盘组合键落地到 tap_/dbl_/lng_ 前缀块
             * (独立于主键 ec_press_key)。gesture: 0=单击 1=双击 2=长按 */
            const int gesture = ki - kEcPressGestureBase;
            QVariantMap m = ecKeyMapFor(1);   /* 1 = press */
            const QString pfx = ecPressGesturePrefix(gesture);
            m[pfx + "action"] = "keyboard";
            m[pfx + "modifier"] = modifier & 0xFF;
            m[pfx + "keycode"] = keycode & 0xFF;
            setEcKeyMap(1, m);
            setStatus(QStringLiteral("EC 按下 %1 快捷键已更改，写入后生效")
                      .arg(gesture == 0 ? "单击" : gesture == 1 ? "双击" : "长按"));
        } else {
            setKeyAction(ki, 0, "keyboard");
            setKeyModifier(ki, 0, modifier);
            setKeyKeycode(ki, 0, keycode);
        }
    } else if (ki == kShakeCaptureIndex) {
        setShakeKey(modifier, keycode);
    }
    emit capturingChanged();
    emit captured(ki, modifier, keycode);
}

void KeyConfigManager::startShakeCapture()
{
    qDebug() << "[Capture] startShakeCapture";
    if (m_captureIndex == kShakeCaptureIndex)
        return;              /* 已在捕获摇一摇, 不重复启动 */
    if (m_captureIndex >= 0)
        cancelCapture();     /* 理论互斥, 双保险 */
    beginCapture(kShakeCaptureIndex);
}

void KeyConfigManager::setShakeEnabled(int on)
{
    int v = on ? 1 : 0;
    if (m_shakeEnabled == v)
        return;
    m_shakeEnabled = v;
    setStatus("摇一摇已更改，写入后生效");
    emit shakeChanged();
}

void KeyConfigManager::setShakeSens(int sens)
{
    if (sens < 0) sens = 0;
    if (sens > 2) sens = 2;
    if (m_shakeSens == sens)
        return;
    m_shakeSens = sens;
    setStatus("摇一摇灵敏度已更改，写入后生效");
    emit shakeChanged();
}

void KeyConfigManager::setShakeKey(int modifier, int keycode)
{
    if (m_shakeAction == "keyboard" && m_shakeModifier == modifier && m_shakeKeycode == keycode)
        return;
    m_shakeAction = "keyboard";
    m_shakeModifier = modifier & 0xFF;
    m_shakeKeycode = keycode & 0xFF;
    setStatus("摇一摇快捷键已更改，写入后生效");
    emit shakeChanged();
}

void KeyConfigManager::clearShakeKey()
{
    if (m_shakeAction == "none")
        return;
    m_shakeAction = "none";
    m_shakeModifier = 0;
    m_shakeKeycode = 0;
    setStatus("摇一摇快捷键已清除，写入后生效");
    emit shakeChanged();
}

/* ===================== 09-05 L2/L3/L1 侧键快捷键 ===================== */
/* (defaultLKey / sanitizedLKey / defaultL1Key 定义在文件头部, 供构造器与 JSON 解析共用) */

QVariantMap KeyConfigManager::lKeyMapFor(int which) const
{
    if (which == 0) return m_l2Key;
    if (which == 1) return m_l3Key;
    return m_l1Key;   /* 2 = L1 */
}

void KeyConfigManager::setLKeyMap(int which, const QVariantMap &m)
{
    if (which == 0)      { m_l2Key = m; emit l2KeyChanged(); }
    else if (which == 1) { m_l3Key = m; emit l3KeyChanged(); }
    else                 { m_l1Key = m; emit l1KeyChanged(); }
}

QString KeyConfigManager::lKeyName(int which)
{
    return which == 0 ? QStringLiteral("KEY L2")
         : which == 1 ? QStringLiteral("KEY L3")
                      : QStringLiteral("KEY L1");
}

void KeyConfigManager::setLKeyAction(int which, const QString &action)
{
    QVariantMap m = lKeyMapFor(which);
    QString cur = m.value("action").toString();
    int curKc = m.value("keycode").toInt();

    if (action == "airmouse" && which == 2) {
        /* 仅 L1: 空中鼠标开关(默认动作) */
        m = defaultL1Key();
    } else if (action == "wheel_up" || action == "wheel_down") {
        /* 09-06: 滚轮动作(与 EC 统一) —— 每次按下滚动一格 */
        m["action"] = "mouse";
        m["modifier"] = 0;
        m["keycode"] = (action == "wheel_up") ? 5 : 6;
    } else if (action == "mouse_left" || action == "mouse_right" || action == "mouse_middle") {
        m["action"] = "mouse";
        m["modifier"] = 0;
        m["keycode"] = (action == "mouse_left") ? 1 : (action == "mouse_right") ? 2 : 4;
    } else if (action == "keyboard" || action == "multimedia") {
        /* 切到键盘/多媒体: 保留已有组合键(如有), 否则置空待捕获 */
        m["action"] = action;
        if (cur != "keyboard" && cur != "multimedia") {
            m["modifier"] = 0;
            m["keycode"] = 0;
        }
    } else if (action == "none") {
        m["action"] = "none";
        m["modifier"] = 0;
        m["keycode"] = 0;
    } else {
        return;
    }
    if (m == lKeyMapFor(which))
        return;
    setLKeyMap(which, m);
    setStatus(QString("%1 已更改，写入后生效").arg(lKeyName(which)));
}

void KeyConfigManager::setLKeyKey(int which, int modifier, int keycode)
{
    QVariantMap m = lKeyMapFor(which);
    m["action"] = "keyboard";
    m["modifier"] = modifier & 0xFF;
    m["keycode"] = keycode & 0xFF;
    setLKeyMap(which, m);
    setStatus(QString("%1 快捷键已更改，写入后生效").arg(lKeyName(which)));
}

/* ---- 09-05: EC 编码器三手势 ---- */

/* gesture: 0=cw 1=press 2=ccw(+ 10-04: 3=cw_press 按压滚动上, 4=ccw_press 按压滚动下) */
QVariantMap KeyConfigManager::ecKeyMapFor(int gesture) const
{
    if (gesture == 0) return m_ecCwKey;
    if (gesture == 1) return m_ecPressKey;
    if (gesture == 3) return m_ecCwPressKey;
    if (gesture == 4) return m_ecCcwPressKey;
    return m_ecCcwKey;   /* 2 = ccw */
}

void KeyConfigManager::setEcKeyMap(int gesture, const QVariantMap &m)
{
    if (gesture == 0) m_ecCwKey = m;
    else if (gesture == 1) m_ecPressKey = m;
    else if (gesture == 3) m_ecCwPressKey = m;
    else if (gesture == 4) m_ecCcwPressKey = m;
    else m_ecCcwKey = m;
    emit ecKeysChanged();
}

QString KeyConfigManager::ecKeyName(int gesture)
{
    if (gesture == 0) return QStringLiteral("编码器 旋转上");
    if (gesture == 1) return QStringLiteral("编码器 按下");
    if (gesture == 3) return QStringLiteral("编码器 按压滚动上");
    if (gesture == 4) return QStringLiteral("编码器 按压滚动下");
    return QStringLiteral("编码器 旋转下");
}

void KeyConfigManager::setEcKeyAction(int gesture, const QString &action)
{
    QVariantMap m = ecKeyMapFor(gesture);

    if (action == "airmouse") {
        /* 09-06: 激活空中鼠标(行为同 L1: 遵循 air_mouse_mode/speed) */
        m["action"] = "airmouse";
        m["modifier"] = 0;
        m["keycode"] = 0;
    } else if (action == "wheel_up" || action == "wheel_down") {
        /* 09-06: 按下手势也允许滚轮动作(每次按下滚一格) */
        m["action"] = "mouse";
        m["modifier"] = 0;
        m["keycode"] = (action == "wheel_up") ? 5 : 6;
    } else if (action == "mouse_left" || action == "mouse_right" || action == "mouse_middle") {
        m["action"] = "mouse";
        m["modifier"] = 0;
        m["keycode"] = (action == "mouse_left") ? 1 : (action == "mouse_right") ? 2 : 4;
    } else if (action == "keyboard" || action == "multimedia") {
        /* 切到键盘/多媒体: 保留已有组合键(如有), 否则置空待捕获 */
        QString prev = m.value("action").toString();
        m["action"] = action;
        if (prev != "keyboard" && prev != "multimedia") {
            m["modifier"] = 0;
            m["keycode"] = 0;
        }
    } else if (action == "none") {
        m["action"] = "none";
        m["modifier"] = 0;
        m["keycode"] = 0;
    } else {
        return;
    }
    if (m == ecKeyMapFor(gesture))
        return;
    setEcKeyMap(gesture, m);
    setStatus(QString("%1 已更改，写入后生效").arg(ecKeyName(gesture)));
}

void KeyConfigManager::setEcKeyKey(int gesture, int modifier, int keycode)
{
    QVariantMap m = ecKeyMapFor(gesture);
    m["action"] = "keyboard";
    m["modifier"] = modifier & 0xFF;
    m["keycode"] = keycode & 0xFF;
    setEcKeyMap(gesture, m);
    setStatus(QString("%1 快捷键已更改，写入后生效").arg(ecKeyName(gesture)));
}

void KeyConfigManager::setEcKeyMultimedia(int gesture, int keycode)
{
    QVariantMap m = ecKeyMapFor(gesture);
    m["action"] = "multimedia";
    m["modifier"] = 0;
    m["keycode"] = keycode & 0xFF;
    setEcKeyMap(gesture, m);
    setStatus(QString("%1 多媒体键已更改，写入后生效").arg(ecKeyName(gesture)));
}

void KeyConfigManager::setLKeyMultimedia(int which, int keycode)
{
    QVariantMap m = lKeyMapFor(which);
    m["action"] = "multimedia";
    m["modifier"] = 0;
    m["keycode"] = keycode & 0xFF;
    setLKeyMap(which, m);
    setStatus(QString("%1 已更改，写入后生效").arg(lKeyName(which)));
}

/* ===================== 09-07: L1/L2/L3 模式 + 三手势 ===================== */

/* 手势存储前缀(与固件 l*_tap/dbl/lng_key 块一致): 0=单击 tap_ 1=双击 dbl_ 2=长按 lng_ */
QString KeyConfigManager::lGesturePrefix(int gesture)
{
    switch (gesture) {
    case 1:  return QStringLiteral("dbl_");
    case 2:  return QStringLiteral("lng_");
    default: return QStringLiteral("tap_");
    }
}

/* which(0=L2 1=L3 2=L1) -> li(0=L1 1=L2 2=L3), 捕获索引 = base + li*3 + gesture */
int KeyConfigManager::lGestureOrdinalFromWhich(int which)
{
    switch (which) {
    case 2:  return 0;   /* L1 第一序 */
    case 0:  return 1;   /* L2 第二序 */
    default: return 2;   /* L3 第三序 */
    }
}

void KeyConfigManager::setLKeyMode(int which, int mode)
{
    if (which < 0 || which > 2)
        return;
    const int v = (mode == 1) ? 1 : 0;
    QVariantMap m = lKeyMapFor(which);
    /* QVariant::toInt 无默认值形参(那是 QJsonValue::toInt 才有的), 缺字段按 -1 处理
     * (=与 0/1 均不等 → 照常落盘, 语义与 C 键 setKeyMode 的 toInt(-1) 一致)。 */
    const int cur = m.contains(QStringLiteral("l_mode"))
            ? m.value(QStringLiteral("l_mode")).toInt() : -1;
    if (cur == v)
        return;   /* 值与现值一致, 不重复落盘 */
    m["l_mode"] = v;
    setLKeyMap(which, m);
    /* 语义同 C 键: 切换不清空手势内容 —— 常规模式手势存而不用, 切回原样恢复 */
    setStatus(QString("%1 已设为%2模式，写入后生效")
              .arg(lKeyName(which)).arg(v == 0 ? "常规(直通)" : "手势"));
}

void KeyConfigManager::setLKeyGestureAction(int which, int gesture, const QString &action)
{
    if (which < 0 || which > 2 || gesture < 0 || gesture > 2)
        return;
    QVariantMap m = lKeyMapFor(which);
    const QString pfx = lGesturePrefix(gesture);
    QString act = action;
    /* 固件 key_config_lgesture_valid: AIRMOUSE 仅 L1(which==2)合法; L2/L3 拒绝。
     * 这里按 none 落盘(而不是忽略), 保持与固件校验结果一致, 避免脏值污染 flash。 */
    if (act == "airmouse" && which != 2)
        act = QStringLiteral("none");
    if (act != "none" && act != "keyboard" && act != "mouse" &&
        act != "multimedia" && act != "airmouse")
        return;   /* 未知动作值忽略 */
    m[pfx + "action"] = act;
    if (act == "mouse") {
        m[pfx + "modifier"] = 0;
        /* 已处鼠标族则保留现键码(1..6), 否则默认左键; 细分由 setLKeyGestureKeycode 覆盖 */
        int cur = m.value(pfx + "keycode").toInt();
        m[pfx + "keycode"] = (cur >= 1 && cur <= 6) ? cur : 1;
    } else if (act == "keyboard") {
        m[pfx + "modifier"] = 0;
        m[pfx + "keycode"] = 0;   /* 待捕获后置组合键 */
    } else if (act == "multimedia") {
        m[pfx + "modifier"] = 0;
        m[pfx + "keycode"] = 0xE9;   /* 默认音量+, 细分下拉可改 */
    } else {
        m[pfx + "modifier"] = 0;
        m[pfx + "keycode"] = 0;      /* none / airmouse */
    }
    setLKeyMap(which, m);
}

void KeyConfigManager::setLKeyGestureKeycode(int which, int gesture, int keycode)
{
    if (which < 0 || which > 2 || gesture < 0 || gesture > 2)
        return;
    QVariantMap m = lKeyMapFor(which);
    m[lGesturePrefix(gesture) + "keycode"] = keycode & 0xFF;
    setLKeyMap(which, m);
}

/* ===================== 09-07: EC 按下 模式 + 三手势 ===================== */

/* 手势存储前缀(与固件 ec_press_tap/dbl/lng_key 块一致): 0=单击 tap_ 1=双击 dbl_ 2=长按 lng_ */
QString KeyConfigManager::ecPressGesturePrefix(int gesture)
{
    switch (gesture) {
    case 1:  return QStringLiteral("dbl_");
    case 2:  return QStringLiteral("lng_");
    default: return QStringLiteral("tap_");
    }
}

/* EC 按下显式模式(0=常规/直通 1=手势)。模式字节 ec_mode 存于 ec_press_key map,
 * 随配置落盘 —— 固件按此判定, 不再由双击/长按是否设置派生。
 * 切换不清空手势内容(常规模式手势存而不用, 切回原样恢复, 与 C/L 键同语义)。 */
void KeyConfigManager::setEcPressKeyMode(int mode)
{
    const int v = (mode == 1) ? 1 : 0;
    QVariantMap m = ecKeyMapFor(1);   /* 1 = press */
    const int cur = m.contains(QStringLiteral("ec_mode"))
            ? m.value(QStringLiteral("ec_mode")).toInt() : -1;
    if (cur == v)
        return;   /* 值与现值一致, 不重复落盘 */
    m["ec_mode"] = v;
    setEcKeyMap(1, m);
    setStatus(QStringLiteral("EC 按下已设为%1模式，写入后生效")
              .arg(v == 0 ? "常规(直通)" : "手势"));
}

/* EC 按下手势动作(gesture 0=单击 1=双击 2=长按)。合法动作(与固件
 * key_config_ecpressgesture_valid = eckey_valid 一致): none/keyboard/mouse/
 * multimedia/airmouse —— EC 常规键本就允许空中鼠标, 手势每触发切换一次。
 * mouse 默认左键(键码 1), 细分由 setEcPressGestureKeycode 覆盖。 */
void KeyConfigManager::setEcPressGestureAction(int gesture, const QString &action)
{
    if (gesture < 0 || gesture > 2)
        return;
    QVariantMap m = ecKeyMapFor(1);
    const QString pfx = ecPressGesturePrefix(gesture);
    QString act = action;
    if (act != "none" && act != "keyboard" && act != "mouse" &&
        act != "multimedia" && act != "airmouse")
        return;   /* 未知动作值忽略 */
    m[pfx + "action"] = act;
    if (act == "mouse") {
        m[pfx + "modifier"] = 0;
        int cur = m.value(pfx + "keycode").toInt();
        m[pfx + "keycode"] = (cur >= 1 && cur <= 6) ? cur : 1;
    } else if (act == "keyboard") {
        m[pfx + "modifier"] = 0;
        m[pfx + "keycode"] = 0;   /* 待捕获后置组合键 */
    } else if (act == "multimedia") {
        m[pfx + "modifier"] = 0;
        m[pfx + "keycode"] = 0xE9;   /* 默认音量+, 细分下拉可改 */
    } else {
        m[pfx + "modifier"] = 0;
        m[pfx + "keycode"] = 0;      /* none / airmouse */
    }
    setEcKeyMap(1, m);
}

void KeyConfigManager::setEcPressGestureKeycode(int gesture, int keycode)
{
    if (gesture < 0 || gesture > 2)
        return;
    QVariantMap m = ecKeyMapFor(1);
    m[ecPressGesturePrefix(gesture) + "keycode"] = keycode & 0xFF;
    setEcKeyMap(1, m);
}

void KeyConfigManager::setEcPressGestureMultimedia(int gesture, int keycode)
{
    if (gesture < 0 || gesture > 2)
        return;
    QVariantMap m = ecKeyMapFor(1);
    const QString pfx = ecPressGesturePrefix(gesture);
    m[pfx + "action"] = QStringLiteral("multimedia");
    m[pfx + "modifier"] = 0;
    m[pfx + "keycode"] = keycode & 0xFF;
    setEcKeyMap(1, m);
}

/* EC 按下三手势捕获(gesture 0=单击 1=双击 2=长按)。
 * 捕获索引 = kEcPressGestureBase + gesture。 */
void KeyConfigManager::startEcPressGestureCapture(int gesture)
{
    if (gesture < 0 || gesture > 2)
        return;
    const int target = kEcPressGestureBase + gesture;
    qDebug() << "[Capture] startEcPressGestureCapture gesture=" << gesture << "target=" << target;
    beginCapture(target);
}

