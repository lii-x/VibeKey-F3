import QtQuick
import QtQuick.Controls
import QtQuick.Layouts
import QtQuick.Dialogs

ApplicationWindow {
    id: root
    visible: false
    /* ★ 10-05: 由 660×600 放大 —— 对齐网页端的设计尺度(web_config 的
     * --design-w: 752px, --app-max-w: 1180px)。原尺寸下三列卡片明显偏挤,
     * 用户反馈"主页面显得小"。
     * ★ 10-05 二调: 780×700 宽高比 ≈1.11 偏"方", 用户反馈"比例不协调、要宽一点"
     *   ⇒ 加宽到 960 (宽高比 ≈1.37), 横向更舒展, 三列卡片有呼吸感。
     * 只改这两行即可(布局是 Layout 体系, 会自动伸缩)。 */
    width: 960
    height: 700
    title: "VibeKey-F3 Studio 工作台"
    color: "transparent"
    // 透明背景已由 color: "transparent" 实现；
    // 切勿在此加 Qt.WA_TranslucentBackground —— 其值 0x08000000 等于
    // Qt::BypassWindowManagerHint，会导致 Windows 不创建任务栏条目（图标缺失）。
    flags: Qt.FramelessWindowHint | Qt.Window

    property string cfgStatusMsg: ""    // 状态栏消息
    property bool cfgStatusError: false // 状态栏消息是否为错误(红色显示, 08-27: OTA 失败醒目提示)
    property bool isMaximized: false    // 记录窗口是否处于最大化状态
    property real savedX: 100
    property real savedY: 100
    property real savedW: root.width
    property real savedH: root.height

    function toggleMaximize() {
        if (isMaximized) {
            root.x = savedX; root.y = savedY
            root.width = savedW; root.height = savedH
            isMaximized = false
        } else {
            savedX = root.x; savedY = root.y
            savedW = root.width; savedH = root.height
            root.x = 0; root.y = 0
            root.width = screen.width; root.height = screen.height
            isMaximized = true
        }
    }
    property int refreshKeyTick: 0
    property int currentTab: 1
    property int otaSrcMode: 0       // 0=本地文件 1=网络获取
    /* 09-13: 固件更新提醒 —— 已点"稍后"的固件版本(同一版本不再弹窗打扰; 换新版会再提醒) */
    property string fwNoticeDismissed: ""

    /* 09-14: 固件版本/在线判定 —— USB 优先, 只连蓝牙时用 BLE 透传的版本(与 USB 同源) */
    function curFwVersion() {
        if (portDetector.deviceVersion) return portDetector.deviceVersion
        if (lightMonitor.bleDeviceVersion) return lightMonitor.bleDeviceVersion
        return ""
    }
    function deviceOnline() {
        return portDetector.deviceConnected || lightMonitor.bleConnected
    }
    /* 09-07: EC 编码器"按下"模式与当前手势 —— 根作用域共享(按键配置卡 + 弹窗都用同一份) */
    property int ecPressMode: 0       // 0=常规 1=手势(弹窗编辑器状态; 打开弹窗/读取完成时从数据同步)
    property int ecPressGesture: 0    // 0=单击 1=双击 2=长按
    /* 09-07: EC 主卡模式显示【数据派生】—— 对齐 C/L 键徽标策略(直接绑数据源,
     * 随 ecKeysChanged 通知自动刷新), 不再依赖 configReadComplete 一次性事件:
     * 首次自动读取若早于 QML Connections 就绪而丢事件, 旧逻辑(仅 4 个手工赋值点
     * 同步 ecPressMode)会让主卡永远卡在默认"常规模式"。 */
    property bool ecPressModeGesture: {
        var _m = keyConfig.ecPressKey
        if (!_m) return false
        var md = _m.ec_mode
        if (md === 0 || md === 1) return md === 1
        /* 0xFF/字段缺失(旧 worker, 理论已被 C++ withDefaults 补 0): 按双击/长按派生 */
        return (_m.dbl_action !== undefined && _m.dbl_action !== "none") ||
               (_m.lng_action !== undefined && _m.lng_action !== "none")
    }
    function fmtSizeBytes(b) {
        if (!b || b < 0) return ""
        if (b >= 1048576) return (b / 1048576).toFixed(1) + " MB"
        if (b >= 1024) return (b / 1024).toFixed(0) + " KB"
        return b + " B"
    }
    function fmtOtaDate(d) {
        // Gitee ISO8601 -> "MM-DD HH:MM"
        if (!d) return ""
        var m = d.match(/^(\d{4})-(\d{2})-(\d{2})[T ](\d{2}):(\d{2})/)
        if (m) return m[2] + "-" + m[3] + " " + m[4] + ":" + m[5]
        return d
    }
    /* 有效电量：USB 已连时优先用 USB 电量；否则尝试用 BLE Battery Service 电量 */
    property int effectiveBatteryPercent: portDetector.batteryPercent >= 0
                                          ? portDetector.batteryPercent
                                          : lightMonitor.bleBatteryPercent

    /* 电池电量图标：连续填充(100% 填满)，图标小且靠近设备名；未知时显示"未知" */
    Component {
        id: batteryIndicator
        RowLayout {
            id: batteryRow
            spacing: 4
            Layout.alignment: Qt.AlignVCenter
            property color levelColor: root.effectiveBatteryPercent >= 0
                                        ? (root.effectiveBatteryPercent <= 20 ? "#ff4d4f"
                                        : (root.effectiveBatteryPercent <= 45 ? "#fa9d3b" : "#07c160"))
                                        : "#999"
            Item {
                width: 24; height: 12
                Rectangle {
                    x: 0; y: 0; width: 22; height: 12; radius: 3
                    color: "white"; border.color: "#cccccc"; border.width: 1
                }
                Rectangle {
                    x: 22; y: 4; width: 2; height: 4; radius: 1
                    color: "#cccccc"
                }
                Rectangle {
                    x: 2; y: 2
                    width: Math.max(0, Math.min(18, (root.effectiveBatteryPercent >= 0 ? root.effectiveBatteryPercent : 0) * 18 / 100))
                    height: 8; radius: 2
                    color: batteryRow.levelColor
                }
            }
            Text {
                text: root.effectiveBatteryPercent >= 0 ? (root.effectiveBatteryPercent + "%") : "未知"
                font.pointSize: 11
                color: batteryRow.levelColor
            }
        }
    }

    function hidModifierName(mod) {
        var modKeys = []
        if (mod & 0x01) modKeys.push("LCtrl")
        if (mod & 0x02) modKeys.push("LShift")
        if (mod & 0x04) modKeys.push("LAlt")
        if (mod & 0x08) modKeys.push("LWin")
        if (mod & 0x10) modKeys.push("RCtrl")
        if (mod & 0x20) modKeys.push("RShift")
        if (mod & 0x40) modKeys.push("RAlt")
        if (mod & 0x80) modKeys.push("RWin")
        return modKeys.join("+")
    }

    function hidKeyName(code) {
        var pairs = [
            [4,"A"],[5,"B"],[6,"C"],[7,"D"],[8,"E"],[9,"F"],[10,"G"],[11,"H"],[12,"I"],[13,"J"],
            [14,"K"],[15,"L"],[16,"M"],[17,"N"],[18,"O"],[19,"P"],[20,"Q"],[21,"R"],[22,"S"],
            [23,"T"],[24,"U"],[25,"V"],[26,"W"],[27,"X"],[28,"Y"],[29,"Z"],
            [30,"1"],[31,"2"],[32,"3"],[33,"4"],[34,"5"],[35,"6"],[36,"7"],[37,"8"],[38,"9"],[39,"0"],
            [40,"Enter"],[41,"Esc"],[42,"Backspace"],[43,"Tab"],[44,"Space"],
            [45,"-"],[46,"="],[47,"["],[48,"]"],[49,"\\"],
            [51,";"],[52,"'"],[53,"`"],[54,","],[55,"."],[56,"/"],
            [58,"F1"],[59,"F2"],[60,"F3"],[61,"F4"],[62,"F5"],[63,"F6"],
            [64,"F7"],[65,"F8"],[66,"F9"],[67,"F10"],[68,"F11"],[69,"F12"],
            [73,"Insert"],[74,"Home"],[75,"PageUp"],[76,"Delete"],[77,"End"],[78,"PageDown"],
            [79,"Right"],[80,"Left"],[81,"Down"],[82,"Up"]
        ]
        for (var i = 0; i < pairs.length; i++) {
            if (pairs[i][0] === code) return pairs[i][1]
        }
        return "?"
    }

    function formatShortcut(mod, kc) {
        var modStr = hidModifierName(mod)
        var keyStr = hidKeyName(kc)
        if (modStr && kc > 0) return modStr + "+" + keyStr
        if (modStr) return modStr
        if (kc > 0) return keyStr
        return ""
    }

    function getKeyShortcutText(ki) {
        var kc = keyConfig.keyConfigs
        if (ki < kc.length) {
            var t = formatShortcut(kc[ki].modifier, kc[ki].keycode)
            if (t.length > 0) return t
        }
        return "未设置"
    }

    /* 09-07: C 键手势 —— gesture 1=双击(dbl) 2=长按(lng) 固定前缀 dbl_/lng_;
     * gesture 0: 常规模式编辑主键 keys[](""), 手势模式编辑独立"单击"块 tap_ ——
     * 两者分开存储, 改手势单击【不会】影响常规模式键, 反之亦然。 */
    function cGesturePfx(ki, g) {
        if (g === 1) return "dbl_"
        if (g === 2) return "lng_"
        return cKeyIsRegular(ki) ? "" : "tap_"
    }
    function cKeyField(ki, g, field) {
        var cfg = keyConfig.keyConfigs[ki]
        if (!cfg) return undefined
        return cfg[cGesturePfx(ki, g) + field]
    }
    function cKeyKeycode(ki, g) { return cKeyField(ki, g, "keycode") || 0 }
    function cKeyActionValue(ki, g) {
        var action = cKeyField(ki, g, "action")
        var kc = cKeyKeycode(ki, g)
        if (action === "mouse") {
            if (kc === 1) return "mouse_left"
            if (kc === 2) return "mouse_right"
            if (kc === 4) return "mouse_middle"
            if (kc === 5) return "wheel_up"
            if (kc === 6) return "wheel_down"
            return "mouse_left"
        }
        if (action === "keyboard") return "keyboard"
        if (action === "multimedia") return "multimedia"
        return "none"
    }
    /* 09-07: C 键模式判定 —— 读上位机【显式落盘】模式 c_mode(0=常规直通 1=手势)。
     * 常规模式: 按下即发/松开即发, 无视双击/长按配置(存而不用, 固件也按此执行)。
     * 旧数据/未读到(无 c_mode 字段)时按旧规则派生: 双击/长按都未设 -> 常规。 */
    function cKeyIsRegular(ki) {
        var cfg = keyConfig.keyConfigs[ki]
        if (cfg && (cfg.c_mode === 0 || cfg.c_mode === 1))
            return cfg.c_mode === 0
        return cKeyField(ki, 1, "action") === "none" && cKeyField(ki, 2, "action") === "none"
    }
    function cKeyStatusText(ki, g) {
        var action = cKeyField(ki, g, "action")
        var kc = cKeyKeycode(ki, g)
        if (action === "mouse") {
            if (kc === 1) return "鼠标·左键"
            if (kc === 2) return "鼠标·右键"
            if (kc === 4) return "鼠标·中键"
            if (kc === 5) return "鼠标·滚轮上"
            if (kc === 6) return "鼠标·滚轮下"
            return "鼠标键"
        }
        if (action === "multimedia") return "多媒体:" + mmCodeName(kc)
        if (action === "keyboard") {
            var t = formatShortcut(cKeyField(ki, g, "modifier") || 0, kc)
            if (t.length > 0) return t
            return "未设置"
        }
        return "未设置"
    }
    /* C 卡摘要: 仅列已设置的手势(未设置不占位, 避免卡片过宽截断)。
     * 09-07: 常规(直通)模式只展示单击动作(双击/长按存而不用, 固件无视),
     * 摘要加"直通"前缀说明按下即发/松开即发, 不再罗列被忽略的手势配置。 */
    function cCardSummaryText(ki) {
        var t0 = cKeyStatusText(ki, 0)
        if (cKeyIsRegular(ki))
            return t0 !== "未设置" ? "直通 · " + t0 : "未设置"
        var parts = []
        var labels = ["单击", "双击", "长按"]
        for (var g = 0; g < 3; g++) {
            var t = cKeyStatusText(ki, g)
            if (t !== "未设置") parts.push(labels[g] + ":" + t)
        }
        return parts.length > 0 ? parts.join("  ") : "未设置"
    }

    /* 09-05 L2/L3/L1 侧键展示辅助（which: 0=L2 1=L3 2=L1，读 l2Key/l3Key/l1Key map） */
    function lKeyMap(which) {
        return which === 0 ? keyConfig.l2Key : which === 1 ? keyConfig.l3Key : keyConfig.l1Key
    }
    function lKeyStatusText(which) {
        var m = lKeyMap(which)
        if (m.action === "airmouse")
            return "空中鼠标 · " + (keyConfig.airMouseMode === 0 ? "单击切换" : "按住移动")
        if (m.action === "mouse") {
            if (m.keycode === 1) return "鼠标·左键"
            if (m.keycode === 2) return "鼠标·右键"
            if (m.keycode === 4) return "鼠标·中键"
            if (m.keycode === 5) return "鼠标·滚轮上"
            if (m.keycode === 6) return "鼠标·滚轮下"
        }
        if (m.action === "multimedia") return "多媒体:" + mmCodeName(m.keycode)
        if (m.action === "keyboard") {
            var t = formatShortcut(m.modifier, m.keycode)
            if (t.length > 0) return t
            return "未设置"
        }
        return "无动作"
    }
    /* 弹窗"快捷键"行: C 键读 keyConfigs, L1(索引 3)/L2/L3(索引 5/6) 读 l1Key/l2Key/l3Key */
    function keyStatusText(ki) {
        if (ki === 3) return lKeyStatusText(2)
        if (ki >= 5) return lKeyStatusText(ki - 5)
        return getKeyShortcutText(ki)
    }
    /* ===================== 09-07: L1/L2/L3 模式 + 三手势辅助 =====================
     * 与 C 键同架构: 每键显式模式(l_mode 0=常规直通 1=手势)。手势字段存于同一
     * l1Key/l2Key/l3Key map: 单击=tap_* 双击=dbl_* 长按=lng_*(固件 l*_tap/dbl/lng_key)。
     * which: 0=L2 1=L3 2=L1。L1(which==2)手势允许 airmouse(用户要求)。 */
    function lGestureField(which, g, field) {
        var m = lKeyMap(which)
        if (!m) return undefined
        var p = g === 1 ? "dbl_" : g === 2 ? "lng_" : "tap_"
        return m[p + field]
    }
    /* 模式判定: 显式 l_mode 优先(0=常规 1=手势); 旧数据无字段 -> 派生(双击/长按
     * 任一真实动作即手势, 与固件 key_config_get_l_mode 一致)。 */
    function lKeyIsRegular(which) {
        var m = lKeyMap(which)
        if (m && (m.l_mode === 0 || m.l_mode === 1))
            return m.l_mode === 0
        var d = lGestureField(which, 1, "action")
        var n = lGestureField(which, 2, "action")
        return (!d || d === "none") && (!n || n === "none")
    }
    function lKeyGestureStatusText(which, g) {
        var action = lGestureField(which, g, "action")
        var kc = lGestureField(which, g, "keycode") || 0
        if (action === "mouse") {
            if (kc === 1) return "鼠标·左键"
            if (kc === 2) return "鼠标·右键"
            if (kc === 4) return "鼠标·中键"
            if (kc === 5) return "鼠标·滚轮上"
            if (kc === 6) return "鼠标·滚轮下"
            return "鼠标键"
        }
        if (action === "airmouse")
            return "空中鼠标·" + (keyConfig.airMouseMode === 0 ? "单击切换" : "按住移动")
        if (action === "multimedia") return "多媒体:" + mmCodeName(kc)
        if (action === "keyboard") {
            var t = formatShortcut(lGestureField(which, g, "modifier") || 0, kc)
            if (t.length > 0) return t
            return "未设置"
        }
        return "未设置"
    }
    /* L 卡摘要: 常规(直通)模式只展示主键动作(加"直通"前缀, 手势配置存而不用);
     * 手势模式按已设置的单击/双击/长按罗列(未设置不占位)。 */
    function lKeySummaryText(which) {
        if (lKeyIsRegular(which)) {
            var t = lKeyStatusText(which)
            if (t === "无动作") return "无动作"
            if (t === "未设置") return "未设置"
            return "直通 · " + t
        }
        var parts = []
        var labels = ["单击", "双击", "长按"]
        for (var g = 0; g < 3; g++) {
            var s = lKeyGestureStatusText(which, g)
            if (s !== "未设置") parts.push(labels[g] + ":" + s)
        }
        return parts.length > 0 ? parts.join("  ") : "未设置"
    }
    /* L 手势组合框当前语义值(action+keycode -> mouse_left/wheel_up/airmouse...) */
    function lKeyGestureActionValue(which, g) {
        var action = lGestureField(which, g, "action")
        var kc = lGestureField(which, g, "keycode") || 0
        if (action === "airmouse") return "airmouse"
        if (action === "mouse") {
            if (kc === 1) return "mouse_left"
            if (kc === 2) return "mouse_right"
            if (kc === 4) return "mouse_middle"
            if (kc === 5) return "wheel_up"
            if (kc === 6) return "wheel_down"
            return "mouse_left"
        }
        if (action === "keyboard") return "keyboard"
        if (action === "multimedia") return "multimedia"
        return "none"
    }
    /* L2/L3/L1 动作组合框当前值(action+keycode -> 组合框语义值) */
    function lKeyActionValue(which) {
        var m = lKeyMap(which)
        if (m.action === "airmouse") return "airmouse"
        if (m.action === "mouse") {
            if (m.keycode === 1) return "mouse_left"
            if (m.keycode === 2) return "mouse_right"
            if (m.keycode === 4) return "mouse_middle"
            if (m.keycode === 5) return "wheel_up"
            if (m.keycode === 6) return "wheel_down"
        }
        if (m.action === "keyboard") return "keyboard"
        if (m.action === "multimedia") return "multimedia"
        return "none"
    }

    /* 09-05: EC 编码器三手势展示/取值辅助（g: 0=顺时针 1=按下 2=逆时针）
     * 10-04: 手势模式新增 3=按压滚动上 4=按压滚动下（数据源 ecCwPressKey/ecCcwPressKey）。
     * 三处展示/取值辅助（ecGestureText/ecActionValue/弹窗 syncCombo）都走本函数, 一处扩展全通。 */
    function ecGestureMap(g) {
        if (g === 3) return keyConfig.ecCwPressKey
        if (g === 4) return keyConfig.ecCcwPressKey
        return g === 0 ? keyConfig.ecCwKey : g === 1 ? keyConfig.ecPressKey : keyConfig.ecCcwKey
    }
    function ecMmKeyText(kc) {
        if (kc === 0xE9) return "音量+"
        if (kc === 0xEA) return "音量-"
        if (kc === 0xE2) return "静音"
        if (kc === 0xCD) return "播放/暂停"
        if (kc === 0xB7) return "停止"
        if (kc === 0xB6) return "上一曲"
        if (kc === 0xB5) return "下一曲"
        return "多媒体键"
    }
    /* 10-04: EC 弹窗行标题 —— 手势模式下"旋转上/下"改称"常规滚动上/下"(参考图),
     * 并新增"按压滚动上/下"两行; 常规模式保持"旋转 上/下"不变。 */
    function ecRowLabel(g) {
        if (g === 3) return "按压滚动上"
        if (g === 4) return "按压滚动下"
        if (g === 1) return "按下"
        if (ecPressMode === 1) return g === 0 ? "常规滚动上" : "常规滚动下"
        return g === 0 ? "旋转 上" : "旋转 下"
    }
    function ecGestureText(g) {
        var m = ecGestureMap(g)
        if (m.action === "airmouse")
            return "空中鼠标·" + (keyConfig.airMouseMode === 0 ? "单击切换" : "按住移动")
        if (m.action === "mouse") {
            if (m.keycode === 5) return "鼠标·滚轮上"
            if (m.keycode === 6) return "鼠标·滚轮下"
            if (m.keycode === 1) return "鼠标·左键"
            if (m.keycode === 2) return "鼠标·右键"
            if (m.keycode === 4) return "鼠标·中键"
        }
        if (m.action === "multimedia") return "多媒体·" + ecMmKeyText(m.keycode)
        if (m.action === "keyboard") {
            var t = formatShortcut(m.modifier, m.keycode)
            if (t.length > 0) return t
            return "未设置"
        }
        return "无动作"
    }
    function mmCodeName(kc) {
        if (kc === 0xE9) return "音量+"
        if (kc === 0xEA) return "音量-"
        if (kc === 0xE2) return "静音"
        if (kc === 0xCD) return "播放/暂停"
        if (kc === 0xB6) return "上一曲"
        if (kc === 0xB5) return "下一曲"
        if (kc === 0xB7) return "停止"
        return "?"
    }
    function ecActionValue(g) {
        var m = ecGestureMap(g)
        if (m.action === "airmouse") return "airmouse"
        if (m.action === "mouse") {
            if (m.keycode === 5) return "wheel_up"
            if (m.keycode === 6) return "wheel_down"
            if (m.keycode === 1) return "mouse_left"
            if (m.keycode === 2) return "mouse_right"
            if (m.keycode === 4) return "mouse_middle"
        }
        if (m.action === "keyboard") return "keyboard"
        if (m.action === "multimedia") return "multimedia"
        return "none"
    }
    function openEcSettings() {
        if (keyConfig.captureIndex >= 0) keyConfig.cancelCapture()
        /* 09-07: 打开时把根模式/手势状态同步到数据真实值(模式显式落盘 ec_mode,
         * 无字段旧数据按双击/长按派生 —— 与 ecPressIsRegular 判定一致) */
        ecPressMode = ecPressIsRegular() ? 0 : 1
        ecPressGesture = 0
        ecSettingsPopup.open()
        syncEcCombos()
    }
    function syncEcCombos() {
        /* 10-04: 行数固定 5(手势模式的按压滚动两行平时不可见, 但其下拉同样需要同步) */
        for (var i = 0; i < ecRowRepeater.count; i++) {
            var item = ecRowRepeater.itemAt(i)
            if (item) item.syncCombo()
        }
    }
    /* ===================== 09-07: EC 按下 模式 + 三手势 辅助 =====================
     * 手势字段存于 ec_press_key map: 单击=tap_* 双击=dbl_* 长按=lng_*(固件
     * ec_press_tap/dbl/lng_key)。g: 0=单击 1=双击 2=长按。与 C/L 键同架构。 */
    function ecPressPfx(g) {
        if (g === 1) return "dbl_"
        if (g === 2) return "lng_"
        return "tap_"
    }
    function ecPressField(g, field) {
        var m = keyConfig.ecPressKey
        if (!m) return undefined
        return m[ecPressPfx(g) + field]
    }
    /* 模式判定: 显式 ec_mode 优先(0=常规 1=手势); 无字段(旧 worker)按双击/长按
     * 派生, 与固件 key_config_get_ec_press_mode 一致。 */
    function ecPressIsRegular() {
        var m = keyConfig.ecPressKey
        if (m && (m.ec_mode === 0 || m.ec_mode === 1))
            return m.ec_mode === 0
        var d = ecPressField(1, "action")
        var n = ecPressField(2, "action")
        return (!d || d === "none") && (!n || n === "none")
    }
    /* EC 按下手势状态文本(弹窗三卡 + 按键卡三子卡共用)。
     * EC 手势合法动作 = EC 常规键同集(NONE/键盘/多媒体/MOUSE 键码 1..6 + AIRMOUSE)。 */
    function ecPressGestureStatusText(g) {
        var action = ecPressField(g, "action")
        var kc = ecPressField(g, "keycode") || 0
        if (action === "mouse") {
            if (kc === 1) return "鼠标·左键"
            if (kc === 2) return "鼠标·右键"
            if (kc === 4) return "鼠标·中键"
            if (kc === 5) return "鼠标·滚轮上"
            if (kc === 6) return "鼠标·滚轮下"
            return "鼠标·键码" + kc
        }
        if (action === "airmouse")
            return "空中鼠标·" + (keyConfig.airMouseMode === 0 ? "单击切换" : "按住移动")
        if (action === "multimedia") return "多媒体·" + ecMmKeyText(kc)
        if (action === "keyboard") {
            var t = formatShortcut(ecPressField(g, "modifier") || 0, kc)
            if (t.length > 0) return t
            return "未设置"
        }
        return "未设置"
    }
    /* EC 按下手势当前值(action+keycode -> 组合框语义值: mouse_left/wheel_up/airmouse...) */
    function ecPressGestureActionValue(g) {
        var action = ecPressField(g, "action")
        var kc = ecPressField(g, "keycode") || 0
        if (action === "airmouse") return "airmouse"
        if (action === "mouse") {
            if (kc === 1) return "mouse_left"
            if (kc === 2) return "mouse_right"
            if (kc === 4) return "mouse_middle"
            if (kc === 5) return "wheel_up"
            if (kc === 6) return "wheel_down"
            return "mouse_left"
        }
        if (action === "keyboard") return "keyboard"
        if (action === "multimedia") return "multimedia"
        return "none"
    }
    /* 动作大类/鼠标细分 语义值 -> 鼠标键码(1 左/2 右/4 中/5 滚轮上/6 滚轮下) */
    function ecMouseKc(v) {
        if (v === "mouse_left") return 1
        if (v === "mouse_right") return 2
        if (v === "mouse_middle") return 4
        if (v === "wheel_up") return 5
        if (v === "wheel_down") return 6
        return 0
    }
    /* 09-03 摇一摇展示辅助（依赖 keyConfig.shake* 属性 NOTIFY，绑定会自动刷新） */
    function shakeSensText() {
        switch (keyConfig.shakeSens) {
        case 0: return "轻"
        case 2: return "强"
        default: return "中"
        }
    }
    function shakeShortcutText() {
        if (keyConfig.shakeAction === "none") return "未设置"
        if (keyConfig.shakeAction === "multimedia") return "多媒体键"
        var t = formatShortcut(keyConfig.shakeModifier, keyConfig.shakeKeycode)
        return t.length > 0 ? t : "未设置"
    }
    function shakeStatusLine() {
        var base = (keyConfig.shakeEnabled === 1) ? ("已开启 · 灵敏度" + shakeSensText())
                                                  : "已关闭"
        if (keyConfig.shakeAction === "none") return base
        return base + " · " + shakeShortcutText()
    }

    /* 灰色外框 - 圆角裁剪 */
    Rectangle {
        anchors.fill: parent
        color: "#e8e8e8"
        radius: 10

        /* 左侧侧边栏 */
        Rectangle {
            anchors.left: parent.left;
            anchors.top: parent.top;
            anchors.bottom: parent.bottom
            //anchors.leftMargin: 8;
            anchors.topMargin: 8;
            anchors.bottomMargin: 8;
            width: 60;
            color: "#e8e8e8";//"#e8e8e8";
            //border { color: "#35B070"; width: 1; }
            radius: 12;
            /* 左上角 logo —— ★ 10-06 用户要求: 点它用**系统浏览器**打开官网,
               与网页端侧边栏 logo 的行为保持一致。
               官网 = 仓库根的 index.html (GitHub Pages): https://lii-x.github.io/VibeKey-F3/
               ⚠️ 用 Qt.openUrlExternally(= QDesktopServices::openUrl), 走系统默认浏览器,
                  不会在应用内嵌页面里打开。 */
            Image {
                id: sidebarLogo;
                anchors.top: parent.top;
                anchors.topMargin: 34;
                anchors.horizontalCenter: parent.horizontalCenter;
                width: 33; height: 33;
                source: "qrc:/Sidebar/logo.png";
                fillMode: Image.PreserveAspectFit;
                smooth: false;
                /* 悬停动效**对齐网页端**(.side-logo-link:hover: opacity .85 + translateY(-1px)):
                   光标移上去时图标会"动一下"(上浮 1px)并轻微变淡。
                   ⚠️ 上浮用 transform(Translate) 实现, **不改 anchors.topMargin** ——
                      改 margin 会连带把下面那列导航图标顶歪。
                   ⚠️ 用 containsMouse 而不是 MouseArea.hovered —— 本机 Qt 6.11 的
                      qmltypes 里 MouseArea 没有 hovered, qmllint 会报 missing-property
                      (两者语义相同, 都需要 hoverEnabled: true)。 */
                opacity: logoArea.containsMouse ? 0.85 : 1.0
                Behavior on opacity { NumberAnimation { duration: 120 } }
                transform: Translate {
                    y: logoArea.containsMouse ? -1 : 0
                    Behavior on y {
                        NumberAnimation { duration: 120; easing.type: Easing.OutQuad }
                    }
                }

                MouseArea {
                    id: logoArea
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onEntered: logoTipDelay.restart()
                    onExited: { logoTipDelay.stop(); logoTip.visible = false; }
                    onClicked: Qt.openUrlExternally("https://lii-x.github.io/VibeKey-F3/")
                }
                /* ★ 10-06 自绘悬停气泡(替代 ToolTip)。
                   为什么不用 ToolTip.background / ToolTip.contentItem 改样式:
                   实测 qmllint 报 "Could not find property background / contentItem" ——
                   附加 ToolTip 对象在 Qt 的 qmltypes 里没声明这两个成员(本机 6.11 全部
                   qmltypes 里都搜不到 QQuickToolTip* 类型), 属于不确定的 API, 不冒险。
                   自己画一个深色圆角气泡, 样式完全可控。
                   ⚠️ parent 用 Overlay.overlay —— 与本文件 09-13 的 fwNoticePopup 同一做法:
                      放进 overlay 层就不会被侧边栏/主内容区压住或裁剪。
                      纯 Rectangle 没有 MouseArea/HoverHandler, 不会吃掉鼠标事件。 */
                Rectangle {
                    id: logoTip
                    parent: Overlay.overlay
                    visible: false
                    width: tipLabel.implicitWidth + 20
                    height: tipLabel.implicitHeight + 12
                    radius: 6
                    color: "#2f3438"
                    border.color: "#4a5157"
                    border.width: 1
                    /* 定位到 logo 右侧并垂直居中。overlay 坐标系, 显示时算一次即可:
                       侧边栏布局是静态的, 唯一会动的是 hover 时那 1px 上浮。 */
                    function show() {
                        var p = sidebarLogo.mapToItem(Overlay.overlay,
                                                     sidebarLogo.width + 10, sidebarLogo.height / 2);
                        x = p.x;
                        y = p.y - height / 2;
                        visible = true;
                    }
                    Text {
                        id: tipLabel
                        anchors.centerIn: parent
                        text: "打开 VibeKey-F3 官网"
                        color: "#eef1f3"
                        font.pixelSize: 12
                    }
                }
                Timer {
                    id: logoTipDelay
                    interval: 500          // 与原 ToolTip.delay 一致
                    onTriggered: logoTip.show()
                }
            }
            Column {
                anchors.horizontalCenter: parent.horizontalCenter;
                anchors.top: sidebarLogo.bottom;
                anchors.topMargin: 18;
                spacing: 20;
                Rectangle {
                    width: 40;
                    height: 40;
                    radius: 10;
                    color: "transparent";
                    //color: currentTab === 1 ? "#e0f5e9" : "#ffffff"
                    Image {
                        anchors.centerIn: parent
                        width: 25; height: 25
                        source: currentTab === 1 ? "qrc:/Sidebar/keyseticon_current.png" : "qrc:/Sidebar/keyseticon.png"
                        fillMode: Image.PreserveAspectFit
                        smooth: false
                    }
                    MouseArea {
                        anchors.fill: parent;
                        onClicked: currentTab = 1;
                    }
                }

                Rectangle {
                    width: 40;
                    height: 40;
                    radius: 10;
                    color: "transparent";
                    Image {
                        anchors.centerIn: parent
                        width: 25; height: 25
                        source: currentTab === 2 ? "qrc:/Sidebar/statusicon_current.png" : "qrc:/Sidebar/statusicon.png"
                        fillMode: Image.PreserveAspectFit
                        smooth: false
                    }
                    MouseArea {
                        anchors.fill: parent;
                        onClicked: currentTab = 2;
                    }
                }

                Rectangle {
                    width: 40;
                    height: 40;
                    radius: 10;
                    color: "transparent";
                    Image {
                        anchors.centerIn: parent
                        width: 25; height: 25
                        source: currentTab === 0 ? "qrc:/Sidebar/otaicon_current.png" : "qrc:/Sidebar/otaiconn.png"
                        fillMode: Image.PreserveAspectFit
                        smooth: false
                    }
                    /* 09-13: 固件有新版本 → 图标右上角小红点(带脉冲; 任意页都能看到) */
                    Rectangle {
                        width: 10; height: 10; radius: 5
                        color: "#ff4d4f"
                        border.color: "#ffffff"; border.width: 1.5
                        anchors.right: parent.right; anchors.rightMargin: 1
                        anchors.top: parent.top; anchors.topMargin: 1
                        visible: otaManager.fwUpdateAvailable
                        SequentialAnimation on scale {
                            loops: Animation.Infinite
                            running: otaManager.fwUpdateAvailable
                            NumberAnimation { from: 1.0; to: 1.5; duration: 650 }
                            NumberAnimation { from: 1.5; to: 1.0; duration: 650 }
                        }
                    }
                    MouseArea {
                        anchors.fill: parent;
                        onClicked: currentTab = 0;
                    }
                }
            }

            /* 底部"更多"按钮 */
            Rectangle {
                width: 40;
                height: 40;
                radius: 10;
                anchors.bottom: parent.bottom;
                anchors.bottomMargin: 14;
                anchors.horizontalCenter: parent.horizontalCenter;
                color: "transparent";
                Image {
                    anchors.centerIn: parent;
                    width: 25; height: 25;
                source: currentTab === 3 ? "qrc:/Sidebar/moreicon_current.png" : "qrc:/Sidebar/moreicon.png";
                fillMode: Image.PreserveAspectFit;
                smooth: false;
            }
            MouseArea {
                anchors.fill: parent;
                onClicked: currentTab = 3;
            }
            }
        }

        /* 09-13: 固件更新提醒弹窗 —— 发现新固件(且已连接设备)时主动弹出, 不再只靠托盘气泡 */
        Popup {
            id: fwNoticePopup
            parent: Overlay.overlay
            x: Math.max(12, Math.min(parent.width - width - 12, (parent.width - width) / 2))
            y: Math.max(40, Math.min(parent.height - height - 12, (parent.height - height) / 2))
            width: 360
            padding: 16
            modal: true
            focus: true
            closePolicy: Popup.CloseOnEscape
            background: Rectangle { radius: 12; color: "white"; border.color: "#faad14"; border.width: 1 }
            Overlay.modal: Rectangle { color: "#80000000"; radius: 12 }
            ColumnLayout {
                width: parent.width
                spacing: 12
                RowLayout {
                    Layout.fillWidth: true; spacing: 8
                    Rectangle { width: 30; height: 30; radius: 15; color: "#fff1cc"
                        Text { anchors.centerIn: parent; text: "↑"; font.pointSize: 18; font.bold: true; color: "#d48806" } }
                    Text { text: "发现新固件"; font.pointSize: 15; font.bold: true; color: "#222" }
                    Item { Layout.fillWidth: true }
                }
                Text {
                    Layout.fillWidth: true; wrapMode: Text.WordWrap
                    text: "在线最新版本：v" + otaManager.latestFwVersion
                          + "\n设备当前版本：v" + (root.curFwVersion() ? root.curFwVersion() : "未知")
                    font.pointSize: 12; color: "#555"
                }
                Text {
                    Layout.fillWidth: true; wrapMode: Text.WordWrap
                    visible: !portDetector.deviceConnected
                    text: "提示：固件升级需要 USB 连接设备（可先在此下载，插上 USB 后再点开始升级）。"
                    font.pointSize: 10; color: "#d48806"
                }
                RowLayout {
                    Layout.fillWidth: true; spacing: 10
                    Item { Layout.fillWidth: true }
                    Rectangle {
                        Layout.preferredWidth: 80; Layout.preferredHeight: 32; radius: 6
                        color: "#f0f0f0"
                        Text { anchors.centerIn: parent; text: "稍后"; font.pointSize: 12; color: "#666" }
                        MouseArea { anchors.fill: parent; onClicked: {
                            root.fwNoticeDismissed = otaManager.latestFwVersion
                            fwNoticePopup.close()
                        } }
                    }
                    Rectangle {
                        Layout.preferredWidth: 96; Layout.preferredHeight: 32; radius: 6
                        color: otaManager.netBusy ? "#ffe7ba" : "#faad14"
                        Text { anchors.centerIn: parent; text: otaManager.netBusy ? "下载中…" : "立即升级"; font.pointSize: 12; font.bold: true; color: "white" }
                        MouseArea { anchors.fill: parent; enabled: !otaManager.netBusy; onClicked: {
                            root.currentTab = 0
                            root.otaSrcMode = 1
                            otaManager.downloadLatestFirmware()
                            fwNoticePopup.close()
                        } }
                    }
                }
            }
        }

        /* 发现新固件时主动弹窗(仅当设备已连接且该版本未被"稍后"忽略) */
        Connections {
            target: otaManager
            function onFirmwareUpdateAvailable(version, current) {
                /* 09-14: USB 或 蓝牙 任一在线即提醒(蓝牙连上时也能感知固件有新版) */
                if (root.deviceOnline() && version !== root.fwNoticeDismissed)
                    fwNoticePopup.open()
            }
        }

        /* 顶部标题栏按钮 - 直接放在灰色外框上 */
        RowLayout {
            anchors.right: parent.right;
            anchors.rightMargin: 8;
            anchors.top: parent.top;
            anchors.topMargin: 4;
            spacing: 0;
            Rectangle {
                width: 34;
                height: 26; radius: 4;
                color: t1BtnMa.containsMouse ? "#d0d0d0" : "transparent";
                Text {
                    anchors.centerIn: parent;
                    text: "\u2014"; font.pixelSize: 12; color: "#888";
                }
                MouseArea {
                    id: t1BtnMa;
                    anchors.fill: parent;
                    hoverEnabled: true;
                    onClicked: systemTray.minimizeToTray();
                }
            }
            Rectangle {
                width: 34;
                height: 26;
                radius: 4;
                color: t2BtnMa.containsMouse ? "#d0d0d0" : "transparent";
                Text {
                    anchors.centerIn: parent;
                    text: "\u25A1"; font.pixelSize: 14;
                    color: "#888";
                }
                MouseArea {
                    id: t2BtnMa;
                    anchors.fill: parent;
                    hoverEnabled: true;
                    onClicked: toggleMaximize();
                }
            }
            Rectangle {
                width: 34; height: 26; radius: 4;
                color: t3BtnMa.containsMouse ? "#f56c6c" : "transparent"
                Text { id: closeBtnText; anchors.centerIn: parent; text: "\u2715"; font.pixelSize: 12; color: t3BtnMa.containsMouse ? "white" : "#888" }
                MouseArea { id: t3BtnMa; anchors.fill: parent; hoverEnabled: true; onClicked: root.close() }
            }
        }

        /* 拖拽区域 - 在标题栏位置 */
        MouseArea {
            anchors.left: parent.left; anchors.leftMargin: 68
            anchors.right: parent.right; anchors.rightMargin: 116
            anchors.top: parent.top; anchors.topMargin: 4
            height: 36
            property point lastMouse
            onPressed: lastMouse = Qt.point(mouseX, mouseY)
            onPositionChanged: { if (pressed) { root.x += mouseX - lastMouse.x; root.y += mouseY - lastMouse.y } }
            onDoubleClicked: toggleMaximize()
        }

        /* 白色内容区 - 带圆角 */
        Rectangle {
            anchors.left: parent.left;
            anchors.leftMargin: 60
            anchors.top: parent.top;
            anchors.topMargin: 30
            anchors.right: parent.right;
            anchors.rightMargin: 8
            anchors.bottom: parent.bottom;
            anchors.bottomMargin: 8
            color: "white";
            radius: 12; clip: true

            ColumnLayout { anchors.fill: parent; anchors.margins: 4; anchors.topMargin: 12; spacing: 0

                /* Header bar */
                Rectangle {
                    Layout.fillWidth: true; height: 56; color: "white"
                    RowLayout { anchors.fill: parent; anchors.leftMargin: 20; anchors.rightMargin: 16; spacing: 12
                        Text { text: currentTab === 0 ? "固件升级" : (currentTab === 1 ? "按键配置" : (currentTab === 2 ? "运行状态" : "更多")); font.pointSize: 16; font.bold: true; color: "#333" }
                        Item { Layout.fillWidth: true }
                        Text {
                            text: keyConfig.busy ? "通信中..." : ""; font.pointSize: 11; color: "#07c160"
                            visible: keyConfig.busy
                        }
                    }
                }

                /* Content area */
                /* ★ 10-05: 本层是 ColumnLayout 的**直接子项**, 原先缺 Layout.fillWidth/fillHeight。
                 * ColumnLayout 对"不 fill"的子项只按 implicit size 分配 —— 于是内容区
                 * **填不满窗口, 下方留白**(用户反馈"主页面显得小")。
                 * 内层 mainStack 虽有 fill*, 但那是**相对本层**生效: 本层不被撑开,
                 * 内层自然也撑不开 —— 这是 fill* 的传递性, 极易忽略。
                 * 补上后内容区随窗口伸缩, 放大窗口才真正有意义。 */
                StackLayout {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                StackLayout {
                    id: mainStack
                    Layout.fillWidth: true; Layout.fillHeight: true; currentIndex: currentTab

                    /* OTA Tab */
                    Rectangle {
                        color: "white"
                        /* 中心背景图标: 绝对定位居中, 不参与布局, 不被挤压 */
                        Image {
                            anchors.centerIn: parent
                            width: 120; height: 120
                            source: "qrc:/otabg.png"
                            fillMode: Image.PreserveAspectFit
                            z: 2
                            opacity: 0.4
                        }
                        /* 09-14: 整页可滚动 —— 新固件横幅 + 在线版本列表可能把底部"开始升级"
                         * 顶出可视区(用户实测: 横幅出现后按钮被挤掉且滚不动)。用 ScrollView
                         * 包裹; 内层 ColumnLayout 里 Item{Layout.fillHeight} 占位在 ScrollView
                         * 中自然塌缩为 0, 按钮紧跟内容, 超出即可滚动查看。 */
                        ScrollView {
                            anchors.fill: parent
                            topPadding: 16
                            bottomPadding: 16
                            ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
                            ScrollBar.vertical.policy: ScrollBar.AsNeeded
                            contentWidth: availableWidth

                        ColumnLayout {
                            width: parent.width - 32
                            x: 16
                            spacing: 16

                            /* 09-13: 固件有新版本提醒(在线最新 > 设备当前) */
                            Rectangle {
                                Layout.fillWidth: true; Layout.alignment: Qt.AlignTop
                                visible: otaManager.fwUpdateAvailable
                                implicitHeight: fwNoticeRow.implicitHeight + 20
                                color: "#fff8e6"; radius: 10
                                border.color: "#faad14"; border.width: 1
                                RowLayout {
                                    id: fwNoticeRow
                                    anchors.fill: parent; anchors.margins: 10; spacing: 10
                                    Text { text: "↑"; font.pointSize: 18; font.bold: true; color: "#faad14" }
                                    ColumnLayout { spacing: 2
                                        Text {
                                            text: "发现新固件 v" + otaManager.latestFwVersion
                                            font.pointSize: 12; font.bold: true; color: "#ad6800"
                                        }
                                        Text {
                                            text: "设备当前 v" + (root.curFwVersion() ? root.curFwVersion() : "未知")
                                                  + " · 建议升级到最新固件"
                                            font.pointSize: 10; color: "#ad6800"
                                        }
                                    }
                                    Item { Layout.fillWidth: true }
                                    Rectangle {
                                        Layout.preferredWidth: 78; Layout.preferredHeight: 28; radius: 6
                                        color: otaManager.netBusy ? "#ffe7ba" : "#faad14"
                                        Text {
                                            anchors.centerIn: parent
                                            text: otaManager.netBusy ? "下载中…" : "立即升级"
                                            font.pointSize: 11; font.bold: true; color: "white"
                                        }
                                        MouseArea {
                                            anchors.fill: parent
                                            enabled: !otaManager.netBusy
                                            onClicked: {
                                                root.otaSrcMode = 1   /* 切到"网络获取"以便看到下载进度 */
                                                otaManager.downloadLatestFirmware()
                                            }
                                        }
                                    }
                                }
                            }

                            /* 设备 */
                            Rectangle {
                                Layout.fillWidth: true; Layout.alignment: Qt.AlignTop; height: 56; color: "white"; radius: 10
                                RowLayout {
                                    anchors.fill: parent; anchors.margins: 12; spacing: 8
                                    Text { text: "设备"; font.pointSize: 14; color: "#999"; Layout.preferredWidth: 40 }
                                    Rectangle {
                                        width: 10; height: 10; radius: 5
                                        color: portDetector.deviceConnected ? "#07c160" : "#cccccc"
                                    }
                                    Text {
                                        text: portDetector.deviceConnected
                                              ? ("已连接 " + portDetector.deviceName + (portDetector.deviceVersion ? (" v" + portDetector.deviceVersion) : ""))
                                              : "USB未连接（固件升级仅支持USB连接）"
                                    font.pointSize: 11
                                    color: portDetector.deviceConnected ? "#333" : "#999"
                                }
                                /* 09-10: 电量仅在 USB 连接后显示(固件升级走 USB); 未插 USB 时
                                 * 蓝牙电量也不展示, 由上方"USB未连接"提示引导用户。 */
                                Loader { sourceComponent: batteryIndicator; Layout.alignment: Qt.AlignVCenter; visible: portDetector.deviceConnected && root.effectiveBatteryPercent >= 0 }
                                Item { Layout.fillWidth: true }
                                    Text {
                                        text: "刷新"; font.pointSize: 13; color: "#07c160"
                                        MouseArea { anchors.fill: parent; anchors.margins: -8; onClicked: portDetector.refreshPorts() }
                                    }
                                }
                                /* 外发光 */
                                Rectangle { anchors.fill: parent; anchors.margins: -2; color: "#20e0e0e0"; radius: parent.radius + 2; z: -1 }
                                Rectangle { anchors.fill: parent; anchors.margins: -4; color: "#0ce0e0e0"; radius: parent.radius + 4; z: -1 }
                            }

                            /* 固件来源 + 选择（09-05: 本地文件 / 网络获取 分段） */
                            Rectangle {
                                Layout.fillWidth: true; Layout.alignment: Qt.AlignTop; color: "white"; radius: 10
                                implicitHeight: otaSrcCol.implicitHeight + 20
                                ColumnLayout {
                                    id: otaSrcCol
                                    anchors.fill: parent
                                    anchors.margins: 10
                                    spacing: 6

                                    /* 来源切换 */
                                    RowLayout {
                                        Layout.fillWidth: true; spacing: 8
                                        Text { text: "固件来源"; font.pointSize: 12; font.bold: true; color: "#333" }
                                        Item { Layout.fillWidth: true }
                                        Rectangle {
                                            width: 76; height: 24; radius: 6
                                            color: root.otaSrcMode === 0 ? "#07c160" : "#eef1f0"
                                            Text { anchors.centerIn: parent; text: "本地文件"; font.pointSize: 11; color: root.otaSrcMode === 0 ? "white" : "#666" }
                                            MouseArea { anchors.fill: parent; onClicked: root.otaSrcMode = 0 }
                                        }
                                        Rectangle {
                                            width: 76; height: 24; radius: 6
                                            color: root.otaSrcMode === 1 ? "#07c160" : "#eef1f0"
                                            Text { anchors.centerIn: parent; text: "网络获取"; font.pointSize: 11; color: root.otaSrcMode === 1 ? "white" : "#666" }
                                            MouseArea { anchors.fill: parent; onClicked: root.otaSrcMode = 1 }
                                        }
                                    }

                                    /* ---- 本地模式: 浏览 .bin ---- */
                                    RowLayout {
                                        Layout.fillWidth: true; spacing: 10
                                        Layout.preferredHeight: root.otaSrcMode === 0 ? -1 : 0
                                        Layout.minimumHeight: 0
                                        Layout.maximumHeight: root.otaSrcMode === 0 ? 16777215 : 0
                                        visible: root.otaSrcMode === 0
                                        Rectangle {
                                            Layout.fillWidth: true; height: 30; color: "#f2f3f5"; radius: 6
                                            Text {
                                                anchors.verticalCenter: parent.verticalCenter
                                                anchors.left: parent.left; anchors.leftMargin: 10
                                                anchors.right: parent.right; anchors.rightMargin: 10
                                                text: otaManager.firmwareInfo ? otaManager.firmwareInfo : "选择固件文件 - 点击浏览选择 .bin 文件"
                                                font.pointSize: 12
                                                color: otaManager.firmwareInfo ? "#333" : "#999"
                                                elide: Text.ElideRight
                                            }
                                        }
                                        Text {
                                            text: "浏览"; font.pointSize: 13; color: "#07c160"
                                            MouseArea { anchors.fill: parent; anchors.margins: -8; onClicked: fileDialog.open() }
                                        }
                                    }

                                    /* ---- 网络模式: 在线版本列表 ---- */
                                    ColumnLayout {
                                        Layout.fillWidth: true
                                        Layout.preferredHeight: root.otaSrcMode === 1 ? -1 : 0
                                        Layout.minimumHeight: 0
                                        Layout.maximumHeight: root.otaSrcMode === 1 ? 16777215 : 0
                                        visible: root.otaSrcMode === 1
                                        spacing: 6

                                        RowLayout {
                                            Layout.fillWidth: true; spacing: 8
                                            Text { text: "在线版本（Gitee 发布）"; font.pointSize: 11; font.bold: true; color: "#333" }
                                            Text {
                                                text: portDetector.deviceVersion ? ("当前设备 v" + portDetector.deviceVersion) : ""
                                                font.pointSize: 10; color: "#999"
                                            }
                                            Item { Layout.fillWidth: true }
                                            Text {
                                                text: otaManager.netBusy ? "处理中…" : "刷新"
                                                font.pointSize: 13
                                                color: otaManager.netBusy ? "#bbb" : "#07c160"
                                                MouseArea {
                                                    anchors.fill: parent; anchors.margins: -8
                                                    enabled: !otaManager.netBusy
                                                    onClicked: otaManager.refreshReleases()
                                                }
                                            }
                                        }

                                        Text {
                                            Layout.fillWidth: true
                                            visible: !otaManager.netBusy && otaManager.releasesModel.count === 0
                                            text: otaManager.releasesModel.count === 0 && !otaManager.netBusy
                                                  ? "暂无可用版本"
                                                  : ""
                                            font.pointSize: 11; color: "#999"
                                            wrapMode: Text.WordWrap
                                        }

                                        /* 版本列表: 用普通 Column 替代 Flickable, 避免高度计算不准遮挡下方文字 */
                                        Column {
                                            Layout.fillWidth: true
                                            spacing: 4
                                            visible: otaManager.releasesModel.count > 0
                                            Repeater {
                                                model: otaManager.releasesModel
                                                delegate: Rectangle {
                                                    width: parent.width
                                                    height: 34
                                                    radius: 6
                                                    color: area.containsMouse ? "#f2f9f5" : "white"
                                                    border.color: area.containsMouse ? "#b9e5cf" : "#ececec"
                                                    border.width: 1
                                                    RowLayout {
                                                        anchors.fill: parent
                                                        anchors.leftMargin: 10; anchors.rightMargin: 8
                                                        spacing: 8
                                                        Text { text: tag; font.pointSize: 12; font.bold: true; color: "#07c160" }
                                                        Text {
                                                            text: file
                                                            font.pointSize: 10; color: "#999"
                                                            elide: Text.ElideRight; Layout.fillWidth: true
                                                        }
                                                        Text { text: root.fmtSizeBytes(size); font.pointSize: 10; color: "#999" }
                                                        Text { text: root.fmtOtaDate(date); font.pointSize: 10; color: "#999" }
                                                        Text {
                                                            text: otaManager.netBusy ? "" : "下载"
                                                            font.pointSize: 11; font.bold: true; color: "#07c160"
                                                        }
                                                    }
                                                    MouseArea {
                                                        id: area
                                                        anchors.fill: parent
                                                        hoverEnabled: true
                                                        enabled: !otaManager.netBusy
                                                        onClicked: otaManager.downloadRelease(index)
                                                    }
                                                }
                                            }
                                        }

                                        Text {
                                            Layout.fillWidth: true
                                            visible: !otaManager.netBusy && otaManager.firmwareInfo.length > 0
                                            text: "已选固件: " + otaManager.firmwareInfo
                                            font.pointSize: 11; color: "#333"
                                            elide: Text.ElideRight
                                        }
                                    }
                                }
                                /* 外发光 */
                                Rectangle { anchors.fill: parent; anchors.margins: -2; color: "#20e0e0e0"; radius: parent.radius + 2; z: -1 }
                                Rectangle { anchors.fill: parent; anchors.margins: -4; color: "#0ce0e0e0"; radius: parent.radius + 4; z: -1 }
                            }



                            FileDialog {
                                id: fileDialog; title: "选择固件"; nameFilters: ["*.bin", "*.*"]
                                onAccepted: {
                                    var path = selectedFile.toString()
                                    if (Qt.platform.os === "windows") path = path.replace(/^file:\/\/\//, "")
                                    else path = path.replace(/^file:\/\//, "")
                                    otaManager.selectFirmware(path)
                                }
                            }

                            /* 进度条（传输时显示）—— 用纯 Rectangle 自绘，避免 QC2 ProgressBar
                               在 native 风格下自定义 contentItem 引发的 warning/崩溃 */
                            Rectangle {
                                Layout.fillWidth: true; height: 56; color: "white"; radius: 8; visible: otaManager.progress > 0 || otaManager.transferring
                                ColumnLayout { anchors.fill: parent; anchors.margins: 12; spacing: 8
                                    RowLayout { Layout.fillWidth: true
                                        Text { text: otaManager.transferring ? "正在传输..." : (otaManager.progress >= 100 ? "传输完成" : "等待开始"); font.pointSize: 11; color: "#666" }
                                        Text { text: otaManager.progress + "%"; font.pointSize: 13; font.bold: true; color: "#333"; Layout.fillWidth: true; horizontalAlignment: Text.AlignRight }
                                    }
                                    Rectangle {
                                        Layout.fillWidth: true; height: 4; radius: 2; color: "#e8e8e8"
                                        clip: true
                                        Rectangle {
                                            width: parent.width * Math.max(0, Math.min(1, otaManager.progress / 100.0))
                                            height: parent.height; radius: 2; color: "#07c160"
                                        }
                                    }
                                    /* 状态文本(08-27): 显示 otaManager.statusMessage —— 让"等待设备重启/
                                       验证中"等过程状态可见; 升级失败走 transferFailed 红色横幅。 */
                                    Text {
                                        text: otaManager.statusMessage
                                        font.pointSize: 11; color: "#666"
                                        visible: otaManager.statusMessage.length > 0
                                        elide: Text.ElideRight
                                        Layout.fillWidth: true
                                        wrapMode: Text.WordWrap
                                    }
                                }
                            }

                            /* 占位: 占满剩余高度, 把开始升级推到底部 */
                            Item { Layout.fillHeight: true }

                            /* 开始升级 */
                            Rectangle {
                                Layout.alignment: Qt.AlignHCenter
                                width: 200; height: 44; radius: 22
                                color: otaManager.transferring ? "#f56c6c" : (sBtnMa.pressed ? "#06ad56" : "#07c160")
                                Text { anchors.centerIn: parent; text: otaManager.transferring ? "取消" : "开始升级"; font.pointSize: 14; font.bold: true; color: "white" }
                                MouseArea { id: sBtnMa; anchors.fill: parent; onClicked: { if (otaManager.transferring) otaManager.cancelTransfer(); else otaManager.startTransfer() } }
                            }
                        }
                        }   /* ScrollView 收尾(09-14) */
                    }

                    /* Key Config Tab */
                    ColumnLayout { spacing: 0

                        /* 设备状态 + 槽位/休眠控制区 */
                        Rectangle {
                            Layout.fillWidth: true
                            Layout.leftMargin: 12
                            Layout.rightMargin: 12
                            Layout.topMargin: 10
                            Layout.bottomMargin: 8
                            height: 98
                            radius: 10
                            color: "#fafbfa"
                            border.color: "#e8edea"
                            border.width: 1
                            clip: true

                            ColumnLayout {
                                anchors.fill: parent
                                spacing: 0

                                /* 顶部：连接状态 + 操作按钮 */
                                RowLayout {
                                    Layout.fillWidth: true
                                    Layout.leftMargin: 12
                                    Layout.rightMargin: 12
                                    Layout.topMargin: 10
                                    Layout.bottomMargin: 5
                                    height: 38
                                    spacing: 6
                                    RowLayout {
                                        spacing: 5
                                        Rectangle {
                                            width: 7; height: 7; radius: 3
                                            color: (portDetector.deviceConnected || lightMonitor.bleConnected) ? "#07c160" : "#ccc"
                                        }
                                        Text {
                                            /* 09-03: 连接状态同时认 USB 与蓝牙, 并明确标注通道;
                                             * 之前只认 USB, 蓝牙连接时误显"USB未连接"。 */
                                            text: {
                                                var usb = portDetector.deviceConnected
                                                var ble = lightMonitor.bleConnected
                                                if (!usb && !ble)
                                                    return "未连接"
                                                var name = ""
                                                if (usb)
                                                    name = portDetector.deviceName + (portDetector.deviceVersion ? (" v" + portDetector.deviceVersion) : "")
                                                else if (ble)
                                                    name = (lightMonitor.bleDeviceName ? lightMonitor.bleDeviceName : "VibeKey") + (lightMonitor.bleDeviceVersion ? (" v" + lightMonitor.bleDeviceVersion) : "")
                                                return "已连接" + (name ? "  " + name : "")
                                            }
                                            font.pointSize: 10
                                            color: (portDetector.deviceConnected || lightMonitor.bleConnected) ? "#07c160" : "#999"
                                        }
                                    }
                                    Loader { sourceComponent: batteryIndicator; Layout.alignment: Qt.AlignVCenter; visible: root.effectiveBatteryPercent >= 0 }
                                    Item { Layout.fillWidth: true }
                                    Rectangle { width: 50; height: 26; radius: 13; color: rBtnMa.pressed ? "#e6f5ec" : "transparent"; border.color: "#07c160"; border.width: 1
                                        Text { anchors.centerIn: parent; text: "读取"; font.pointSize: 10; color: "#07c160" }
                                        MouseArea { id: rBtnMa; anchors.fill: parent; onClicked: { keyConfig.readConfig() } }
                                    }
                                    Rectangle { width: 50; height: 26; radius: 13; color: wBtnMa.pressed ? "#e6f5ec" : "transparent"; border.color: "#07c160"; border.width: 1
                                        Text { anchors.centerIn: parent; text: "写入"; font.pointSize: 10; color: "#07c160" }
                                        MouseArea { id: wBtnMa; anchors.fill: parent; onClicked: keyConfig.writeConfig() }
                                    }
                                    Rectangle { width: 50; height: 26; radius: 13; color: rstBtnMa.pressed ? "#fef0e6" : "transparent"; border.color: "#fa9d3b"; border.width: 1
                                        Text { anchors.centerIn: parent; text: "恢复"; font.pointSize: 10; color: "#fa9d3b" }
                                        MouseArea { id: rstBtnMa; anchors.fill: parent; onClicked: keyConfig.resetConfig() }
                                    }
                                }

                                /* 淡色分隔线 */
                                Rectangle { Layout.fillWidth: true; height: 1; color: "#f0f0f0" }

                                /* 底部：蓝牙槽位 + 自动休眠 */
                                RowLayout {
                                    Layout.fillWidth: true
                                    Layout.leftMargin: 12
                                    Layout.rightMargin: 12
                                    Layout.topMargin: 7
                                    Layout.bottomMargin: 7
                                    height: 34
                                    spacing: 6

                                    /* 蓝牙槽位分段选择器 */
                                    Rectangle {
                                        width: slotSelectorRow.implicitWidth + 4
                                        height: 28
                                        radius: 14
                                        color: "#f4f5f4"
                                        RowLayout {
                                            id: slotSelectorRow
                                            anchors.centerIn: parent
                                            spacing: 3
                                            Repeater {
                                                model: 3
                                                delegate: Rectangle {
                                                    property int slotIdx: index
                                                    width: 40; height: 22; radius: 11
                                                    color: keyConfig.slot === slotIdx ? "#07c160" : "transparent"
                                                    Text {
                                                        anchors.centerIn: parent
                                                        text: "蓝牙" + (slotIdx + 1)
                                                        font.pointSize: 9
                                                        color: keyConfig.slot === slotIdx ? "white" : "#555"
                                                    }
                                                    MouseArea {
                                                        anchors.fill: parent
                                                        cursorShape: Qt.PointingHandCursor
                                                        onClicked: { if (keyConfig.slot !== slotIdx) keyConfig.setSlot(slotIdx) }
                                                    }
                                                }
                                            }
                                        }
                                    }

                                    Item { Layout.fillWidth: true }

                                    /* 自动休眠（紧凑版） */
                                    RowLayout {
                                        spacing: 4
                                        Text { text: "休眠"; font.pointSize: 9; color: "#666" }
                                        ComboBox {
                                            id: sleepMinCombo
                                            Layout.preferredWidth: 78
                                            font.pointSize: 9
                                            model: [
                                                { text: "45 分钟", value: 45 },
                                                { text: "1 小时", value: 60 },
                                                { text: "1.5 小时", value: 90 },
                                                { text: "2 小时", value: 120 },
                                                { text: "3 小时", value: 180 },
                                                { text: "永不", value: 0 },
                                            ]
                                            textRole: "text"
                                            valueRole: "value"
                                            /* ⚠️ 绝不能给 currentIndex 绑表达式: ComboBox 内部
                                             * (打开/关闭弹窗、键盘导航)会写 currentIndex →
                                             * 断绑定, 之后显示可能变空白(实测"点开下拉再收回 →
                                             * 空白")。改为函数 + 显式同步(初始化/数据变更时赋值)。 */
                                            function sleepMinIndex() {
                                                var v = keyConfig.sleepMin
                                                for (var i = 0; i < model.length; i++)
                                                    if (model[i].value === v) return i
                                                return 0
                                            }
                                            Component.onCompleted: currentIndex = sleepMinIndex()
                                            Connections {
                                                target: keyConfig
                                                function onSleepMinChanged() {
                                                    sleepMinCombo.currentIndex = sleepMinCombo.sleepMinIndex()
                                                }
                                            }
                                            /* 防御: currentIndex 可能被控件内部改成无效值, 故仅在
                                             * 合法时才用它的值; 正常选择由弹窗 delegate 直接写回。 */
                                            onActivated: {
                                                if (currentIndex >= 0 && currentIndex < model.length)
                                                    keyConfig.setSleepMin(model[currentIndex].value)
                                            }

                                            background: Rectangle {
                                                implicitWidth: 78
                                                implicitHeight: 26
                                                radius: 8
                                                color: "white"
                                                border.color: "#d0e0d8"
                                                border.width: 1
                                            }
                                            contentItem: Text {
                                                leftPadding: 6
                                                rightPadding: 20
                                                /* ⚠️ 显示文本【完全不依赖 currentIndex/displayText】:
                                                 * 该控件内部会把自己的 currentIndex 改成无效值(实测
                                                 * 打开再收起下拉后就变了), 于是 displayText 取不到 →
                                                 * 空白/「—」。直接由数据源 keyConfig.sleepMin 反查文本,
                                                 * 保证显示永远正确。 */
                                                text: {
                                                    var v = keyConfig.sleepMin
                                                    var m = sleepMinCombo.model
                                                    for (var i = 0; i < m.length; i++)
                                                        if (m[i].value === v) return m[i].text
                                                    return "45 分钟"
                                                }
                                                font: sleepMinCombo.font
                                                color: "#333"
                                                verticalAlignment: Text.AlignVCenter
                                                elide: Text.ElideRight
                                            }
                                            indicator: Canvas {
                                                x: sleepMinCombo.width - width - 7
                                                y: sleepMinCombo.height / 2 - height / 2
                                                width: 7; height: 4
                                                contextType: "2d"
                                                onPaint: {
                                                    var ctx = getContext("2d")
                                                    ctx.reset()
                                                    ctx.moveTo(0, 0)
                                                    ctx.lineTo(width, 0)
                                                    ctx.lineTo(width / 2, height)
                                                    ctx.closePath()
                                                    ctx.fillStyle = "#07c160"
                                                    ctx.fill()
                                                }
                                            }

                                            popup: Popup {
                                                y: sleepMinCombo.height + 4
                                                width: 78
                                                padding: 3
                                                implicitHeight: 30 * sleepMinCombo.model.length + 6
                                                background: Rectangle {
                                                    radius: 8
                                                    color: "white"
                                                    border.color: "#e0e0e0"
                                                    border.width: 1
                                                }
                                                contentItem: ListView {
                                                    clip: true
                                                    implicitHeight: 30 * sleepMinCombo.model.length
                                                    model: sleepMinCombo.delegateModel
                                                    currentIndex: sleepMinCombo.highlightedIndex
                                                    delegate: ItemDelegate {
                                                        width: ListView.view.width
                                                        height: 30
                                                        highlighted: ListView.isCurrentItem
                                                        /* 直接用被点项的 value 写回(不经过 currentIndex/
                                                         * activated —— 控件内部会把 currentIndex 弄坏,
                                                         * 走 activated 会写到错误的值) */
                                                        onClicked: {
                                                            keyConfig.setSleepMin(sleepMinCombo.model[index].value)
                                                            sleepMinCombo.popup.close()
                                                        }
                                                        contentItem: Text {
                                                            text: sleepMinCombo.model[index].text
                                                            color: parent.highlighted ? "#07c160" : "#333"
                                                            font: sleepMinCombo.font
                                                            verticalAlignment: Text.AlignVCenter
                                                            elide: Text.ElideRight
                                                            leftPadding: 6
                                                        }
                                                        background: Rectangle {
                                                            color: parent.highlighted ? "#e6f5ec" : "transparent"
                                                            radius: 6
                                                        }
                                                    }
                                                }
                                            }
                                        }
                                    }

                                    /* 操作状态提示（靠右，在休眠右侧） */
                                    Text {
                                        text: keyConfig.statusMessage
                                        font.pointSize: 10
                                        color: "#07c160"
                                        elide: Text.ElideRight
                                        horizontalAlignment: Text.AlignRight
                                        verticalAlignment: Text.AlignVCenter
                                        visible: text.length > 0
                                    }

                                }

                            }
                        }
                        /* Device visual + settings popup */
                        Rectangle {
                            id: keyConfigArea
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            color: "white"
                            clip: true

                            property real deviceScale: {
                                var availH = mainStack.height - 116
                                var byH = (availH - 160) / 230
                                var byW = (mainStack.width - 320) / 94
                                return Math.max(0.65, Math.min(0.9, Math.min(byH, byW)))
                            }
                            /* ★ 10-05: 卡片与设备图之间的"呼吸间距"(自然像素, 会再乘 deviceScale)。
                             * 原为 左列 10 / 右列 12 —— 实际只有 9~10.8px, 用户反馈
                             * "卡片不要靠产品图太近"。网页端 `03-cards.css` 的三列 gap 是
                             * **32px**(注释:"设备与卡片两侧的呼吸区"), 这里取 32 与之对齐
                             * (实际 ≈28.8px @ deviceScale=0.9)。
                             * 提成属性是为了以后只改这一处 —— 4 个卡片的 defX 都引用它。 */
                            property real cardGap: 32
                            onDeviceScaleChanged: {
                                Qt.callLater(keyConfigArea.relayoutLCards)
                                Qt.callLater(keyConfigArea.relayoutRCards)
                            }
                            property real keyNaturalX: 12
                            property real keyNaturalY: 18
                            property real keyNaturalW: 58
                            property real keyNaturalH: 56
                            property real keyNaturalGap: 8
                            /* KEY L1 侧键在设备图左侧的自然坐标(原图 82x230 像素坐标系) */
                            property real l1NaturalX: 2
                            property real l1NaturalY: 28
                            property real l1NaturalW: 12
                            property real l1NaturalH: 42
                            /* 09-05: EC 编码器旋钮(设备图右缘凸出旋钮)。09-06 修:
                             * 编码器旋钮实际在设备图右缘中上部(C1 键高度, 用户箭头所指),
                             * 原 ecNaturalY=182 指到设备下部(C3 键高度)的红色按钮, 导致
                             * EC 卡片虚线小圆点指错位置。
                             * ecNaturalY = 虚线锚点高度; ecCardNaturalY = 卡片默认停留
                             * 高度(独立控制, 避免锚点上移后卡片与 C1 卡片默认重叠)。 */
                            property real ecNaturalY: 45
                            property real ecNaturalH: 10
                            /* 09-06: 卡片列默认位置。EC 卡片(4行, 高约80px)比 C 卡片
                             * (2行, 高约46px)高近一倍, 小窗口(scale≈0.8)下间距不足会
                             * 叠在一起。为保证任意窗口尺寸下都有间隙:
                             * C1 上移到列顶(-41), EC 放 C1/C2 之间(44, 贴近锚点50),
                             * C2/C3 各下移20 给 EC 让位(虚线斜约18px, 可接受)。 */
                            property real c1CardNaturalY: -51
                            property real ecCardNaturalY: 59
                            property real c2CardNaturalY: 169
                            property real c3CardNaturalY: 259
                            /* 09-06: 右列卡片整体下移微调(自然单位, 随 deviceScale 缩放)；
                             * 09-10: 再向上调一点，避免底部卡片被裁。 */
                            property real rightCardsYOffset: 8
                            /* 09-05: L2/L3 侧键(设备图左缘的两个小圆点凸起)。
                             * l2/l3NaturalY = 热点矩形顶部; 连线端点 = 矩形中心。
                             * lHotNaturalX = 圆点中心 X(自然坐标, 按设备图 82x230 目测)。
                             * 偏了只影响热点/红线的视觉对位, 可微调。 */
                            property real l2NaturalY: 63
                            property real l3NaturalY: 105
                            property real lNaturalW: 14
                            property real lNaturalH: 20
                            property real lHotNaturalX: 3
                            /* 08-31: 绑定到 C++ 的 configRead 属性，比只依赖信号更可靠 */
                            property bool hasRead: keyConfig.configRead
                            onHasReadChanged: lineCanvas.requestPaint()

                            /* 09-07: L/R 列信息卡在默认状态下均分间隙。
                             * 保持每列最上、最下卡片位置不变, 把中间卡片均匀插空, 让默认
                             * 间隙一致。空间不足(会重叠)时退化为 8px 最小间隙并向下推开。
                             * 内容变高/缩放变化/显示时自动触发(Qt.callLater 防绑定环)。 */
                            function distributeCardGaps(items) {
                                var minGap = 8
                                var n = items.length
                                if (n < 2) return
                                // 按自然默认位置排序, 默认状态即 L1/L2/L3 或 C1/EC/C2/C3 顺序
                                items.sort(function(a, b) { return a.defY - b.defY })
                                // 顶/底卡片位置(含手动拖拽偏移)保持不变, 只在它们之间均分
                                var topY = items[0].defY + items[0].dragOffsetY
                                var bottomY = items[n-1].defY + items[n-1].dragOffsetY + items[n-1].height
                                var span = bottomY - topY
                                var totalItemH = 0
                                for (var i = 0; i < n; i++) totalItemH += items[i].height
                                var gap = (span - totalItemH) / (n - 1)
                                if (gap < minGap) gap = minGap
                                var y = topY
                                for (var j = 0; j < n; j++) {
                                    if (j > 0) y += gap
                                    /* 改写 dragOffsetY 而非 y —— 直接写 y 会解除绑定,
                                     * 放大窗口后卡片停在旧位置; 写偏移保留绑定, 默认位置随窗口重算 */
                                    if (y !== items[j].y) items[j].dragOffsetY = y - items[j].defY
                                    y += items[j].height
                                }
                            }

                            function relayoutLCards() {
                                var items = []
                                if (l1InfoCard.visible) items.push(l1InfoCard)
                                for (var i = 0; i < 2; i++) {
                                    var it = lCardRepeater.itemAt(i)
                                    if (it && it.visible) items.push(it)
                                }
                                distributeCardGaps(items)
                            }

                            function relayoutRCards() {
                                var items = []
                                for (var i = 0; i < 3; i++) {
                                    var it = cCardRepeater.itemAt(i)
                                    if (it && it.visible) items.push(it)
                                }
                                if (ecInfoCard.visible) items.push(ecInfoCard)
                                distributeCardGaps(items)
                            }

                            function showKeySettings(ki) {
                                if (keyConfig.captureIndex >= 0 && keyConfig.captureIndex !== ki)
                                    keyConfig.cancelCapture()
                                if (ki === 7) {   /* 09-06: EC 编码器走专属弹窗(三手势动作),
                                                   * 通用弹窗无其编辑区, 原实现打开后只能看不能改 */
                                    openEcSettings()
                                    return
                                }
                                keySettingsPopup.selectedKeyIndex = ki
                                if (ki < 3) {
                                    keySettingsPopup.cGestureState = 0
                                } else if (ki === 3 || ki === 5 || ki === 6) {
                                    /* 09-07: L 键三手势 —— 默认编辑"单击", 模式/手势下拉待 open 后同步 */
                                    keySettingsPopup.lGestureState = 0
                                }
                                keySettingsPopup.open()
                                /* 09-07: 同步放到 open() 之后 —— 弹窗内容实例化后再读写子控件才稳妥 */
                                if (ki < 3) {
                                    keySettingsPopup.syncFromColor()
                                    keySettingsPopup.syncCKeyMode()
                                    keySettingsPopup.syncCKeyCombo()
                                } else if (ki === 3 || ki === 5 || ki === 6) {
                                    keySettingsPopup.syncLModeUi()
                                    keySettingsPopup.syncLGCombo()
                                }
                                syncLKeyCombo()
                            }

                            /* 09-05: L1/L2/L3 动作组合框与设备配置同步。
                             * 每次打开弹窗显式设置(用户选择会断开 currentIndex 的声明式绑定,
                             * 声明式绑定本身不可靠, 必须在 open 前手动同步)。
                             * L1(索引 3) 用 l1Model(含"空中鼠标"), L2/L3(5/6) 用 cModel。 */
                            function syncLKeyCombo() {
                                var ki = keySettingsPopup.selectedKeyIndex
                                if (ki !== 3 && ki !== 5 && ki !== 6) return
                                var which = (ki === 3) ? 2 : ki - 5
                                var av = lKeyActionValue(which)
                                /* 大类: 鼠标族(含空中鼠标)归"鼠标" */
                                var cat = (av === "wheel_up" || av === "wheel_down" || av === "mouse_left" ||
                                           av === "mouse_right" || av === "mouse_middle" || av === "airmouse") ? "mouse" : av
                                var tm = lTypeCombo.model
                                for (var i = 0; i < tm.length; i++) {
                                    if (tm[i].v === cat) { lTypeCombo.currentIndex = i; break }
                                }
                                /* 细分: L1 含空中鼠标, L2/L3 不含 */
                                lTriggerCombo.model = (ki === 3) ? lTypeCombo.l1TriggerModel
                                                                 : lTypeCombo.cTriggerModel
                                var mm = lTriggerCombo.model
                                for (var j = 0; j < mm.length; j++) {
                                    if (mm[j].v === av) { lTriggerCombo.currentIndex = j; break }
                                }
                                /* 多媒体细分同步 */
                                var mk = lKeyMap(which).keycode
                                var mmm = lMmCombo.model
                                for (var k = 0; k < mmm.length; k++) {
                                    if (mmm[k].v === mk) { lMmCombo.currentIndex = k; break }
                                }
                            }

                            /* 返回指定按键在 keyConfigArea 坐标系中的中心点（用于画红线） */
                            function hotspotCenter(ki) {
                                var cx, cy
                                if (ki === 3) { // L1 -> 设备左侧长条侧键中心（侧键凸出在设备图外，用硬坐标修正）
                                    cx = deviceFrame.x - 4 * deviceScale   /* 凸出在设备图左侧外 */
                                    cy = deviceFrame.y + 38 * deviceScale  /* 侧键上部 */
                                } else if (ki >= 0 && ki < 3) { // C1-C3 -> 各自色块右侧边缘中点
                                    cx = deviceFrame.x + (keyNaturalX + keyNaturalW) * deviceScale
                                    cy = deviceFrame.y + (keyNaturalY + ki * (keyNaturalH + keyNaturalGap) + keyNaturalH / 2) * deviceScale
                                } else if (ki === 4) { // 摇一摇 -> 设备中心
                                    cx = deviceFrame.x + deviceFrame.width / 2
                                    cy = deviceFrame.y + deviceFrame.height / 2
                                } else if (ki === 5 || ki === 6) { // 09-05: L2/L3 -> 设备左缘小圆点中心
                                    var lk = (ki === 5) ? l2NaturalY : l3NaturalY
                                    cx = deviceFrame.x + lHotNaturalX * deviceScale
                                    cy = deviceFrame.y + (lk + lNaturalH / 2) * deviceScale
                                } else if (ki === 7) { // 09-05: EC 编码器 -> 右缘中上部凸出旋钮中心(09-06 修: 原指下部红按钮)
                                    cx = deviceFrame.x + deviceFrame.width + 2 * deviceScale
                                    cy = deviceFrame.y + (ecNaturalY + ecNaturalH / 2) * deviceScale
                                } else {
                                    return null
                                }
                                return Qt.point(cx, cy)
                            }

                            /* 08-31: StackLayout 只对自己的直接子项设 visible，本 Rectangle 是孙项，
                             * 它的 visible 恒为 true，onVisibleChanged 永远不会触发。
                             * 因此改用 currentTab 判断当前是否停在按键配置页。 */
                            property bool isActiveTab: currentTab === 1

                            onVisibleChanged: {
                                if (visible) {
                                    /* 09-07 修: 必须与上方 deviceScale 属性绑定【同公式】。
                                     * 旧版此处用了另一套 keyConfigArea 尺寸 + [0.8,1.35] 区间,
                                     * 导致切页再切回时被覆盖成更大的 scale, C/L/EC 卡片纵向
                                     * 间距被整体拉大。改用与属性绑定一致的 mainStack 公式,
                                     * 仅强制重绑一次(隐藏期 mainStack 尺寸异常时的残留值兜底)。 */
                                    deviceScale = Qt.binding(function() {
                                        var availH = mainStack.height - 116
                                        var byH = (availH - 160) / 230
                                        var byW = (mainStack.width - 320) / 94
                                        return Math.max(0.65, Math.min(0.9, Math.min(byH, byW)))
                                    })
                                }
                            }

                            /* 切到按键配置页时，若设备已连上且还没读过，自动读一次 */
                            onIsActiveTabChanged: {
                                if (isActiveTab && portDetector.deviceConnected && !hasRead)
                                    keyConfig.readConfig()
                            }

                            /* 开局就插着设备的情况：deviceConnectedChanged 可能在 QML 加载完成前
                             * 就发过了，上面两个入口都抓不到，这里补一次。 */
                            Component.onCompleted: {
                                if (isActiveTab && portDetector.deviceConnected && !hasRead)
                                    keyConfig.readConfig()
                            }

                            Connections {
                                target: keyConfig
                                function onPhysicalKeyPressed(keyIndex) {
                                    currentTab = 1
                                    keyConfigArea.showKeySettings(keyIndex)
                                }
                                function onCaptured(keyIndex, modifier, keycode) {
                                    refreshKeyTick++
                                    /* 09-07: C 键手势键盘捕获结束(索引=10+ki*3+g) → 同步下拉/编辑态 */
                                    if (keyIndex >= 10 && keyIndex < 19 && keySettingsPopup.opened &&
                                        keySettingsPopup.selectedKeyIndex >= 0 && keySettingsPopup.selectedKeyIndex < 3) {
                                        keySettingsPopup.syncCKeyCombo()
                                        keySettingsPopup.syncCKeyMode()
                                    }
                                    /* 09-07: L 键手势键盘捕获结束(索引=19+li*3+g) → 同步下拉/编辑态 */
                                    if (keyIndex >= 19 && keyIndex < 28 && keySettingsPopup.opened &&
                                        (keySettingsPopup.selectedKeyIndex === 3 ||
                                         keySettingsPopup.selectedKeyIndex === 5 ||
                                         keySettingsPopup.selectedKeyIndex === 6)) {
                                        keySettingsPopup.syncLModeUi()
                                        keySettingsPopup.syncLGCombo()
                                    }
                                }
                            }

                            /* 08-31: 「设备连上就自动读」由 C++ KeyConfigManager 负责
                             * （带 150ms 延迟，等 QML 就绪），这里不再重复触发，避免
                             * 两条路径同时调 readConfig() 冒出"正在处理中..."。
                             * ⚠️ 千万别给 hasRead 赋值！它是绑定到 keyConfig.configRead
                             * 的属性，一旦赋值就断开绑定，之后 configRead 变化不再更新，
                             * 会导致"拔掉再插上不显示"。断线复位由 C++ 处理。 */

                            Item {
                                id: deviceFrame
                                anchors.centerIn: parent
                                width: 82 * keyConfigArea.deviceScale
                                height: 230 * keyConfigArea.deviceScale
                                onXChanged: lineCanvas.requestPaint()
                                onYChanged: lineCanvas.requestPaint()
                                onWidthChanged: lineCanvas.requestPaint()
                                onHeightChanged: lineCanvas.requestPaint()

                                Image {
                                    id: deviceImage
                                    anchors.fill: parent
                                    source: "qrc:/VibeKey-F3.png"
                                    smooth: true
                                    fillMode: Image.PreserveAspectFit
                                }

                                Repeater {
                                    id: keyHotspotRepeater
                                    model: 4
                                    delegate: Rectangle {
                                        id: keyHotspot
                                        property int ki: index
                                        visible: keyConfigArea.hasRead
                                        x: (ki === 3) ? (keyConfigArea.l1NaturalX * keyConfigArea.deviceScale)
                                                      : (keyConfigArea.keyNaturalX * keyConfigArea.deviceScale)
                                        y: (ki === 3) ? (keyConfigArea.l1NaturalY * keyConfigArea.deviceScale)
                                                      : ((keyConfigArea.keyNaturalY + ki * (keyConfigArea.keyNaturalH + keyConfigArea.keyNaturalGap)) * keyConfigArea.deviceScale)
                                        width: (ki === 3) ? (keyConfigArea.l1NaturalW * keyConfigArea.deviceScale)
                                                          : (keyConfigArea.keyNaturalW * keyConfigArea.deviceScale)
                                        height: (ki === 3) ? (keyConfigArea.l1NaturalH * keyConfigArea.deviceScale)
                                                           : (keyConfigArea.keyNaturalH * keyConfigArea.deviceScale)
                                        radius: (ki === 3) ? 6 : 10
                                        color: {
                                            if (ki === 3) return "#15ffffff"
                                            if (keyConfig.keyConfigs[ki].rgb_enabled) {
                                                var alpha = 0.35
                                                var c = keyConfig.keyConfigs[ki].rgb_color
                                                return Qt.rgba(((c >> 16) & 0xFF) / 255, ((c >> 8) & 0xFF) / 255, (c & 0xFF) / 255, alpha)
                                            }
                                            return "#15ffffff"
                                        }
                                        border.color: keySettingsPopup.selectedKeyIndex === ki ? "#07c160" : "transparent"
                                        border.width: 3

                                        MouseArea {
                                            anchors.fill: parent
                                            hoverEnabled: true
                                            cursorShape: Qt.PointingHandCursor
                                            onClicked: keyConfigArea.showKeySettings(ki)
                                        }
                                    }
                                }

                                /* 09-05: L2/L3 侧键热点(设备左缘小圆点, 可点弹出快捷键设置) */
                                Repeater {
                                    id: lKeyHotspotRepeater
                                    model: 2
                                    delegate: Rectangle {
                                        property int ki: 5 + index
                                        visible: keyConfigArea.hasRead
                                        x: (keyConfigArea.lHotNaturalX - keyConfigArea.lNaturalW / 2) * keyConfigArea.deviceScale
                                        y: ((index === 0) ? keyConfigArea.l2NaturalY
                                                          : keyConfigArea.l3NaturalY) * keyConfigArea.deviceScale
                                        width: keyConfigArea.lNaturalW * keyConfigArea.deviceScale
                                        height: keyConfigArea.lNaturalH * keyConfigArea.deviceScale
                                        radius: 6
                                        color: "#15ffffff"
                                        border.color: keySettingsPopup.selectedKeyIndex === ki ? "#07c160" : "transparent"
                                        border.width: 3

                                        MouseArea {
                                            anchors.fill: parent
                                            hoverEnabled: true
                                            cursorShape: Qt.PointingHandCursor
                                            onClicked: keyConfigArea.showKeySettings(ki)
                                        }
                                    }
                                }
                            }

                            /* KEY L1 空中鼠标状态提示卡片（09-03 改造：可拖动，红线连接） */
                            Rectangle {
                                id: l1InfoCard
                                objectName: "cardL1"
                                property int cardIndex: 3
                                property bool hovered: false
                                onHoveredChanged: lineCanvas.requestPaint()
                                visible: keyConfigArea.hasRead
                                onVisibleChanged: if (visible) Qt.callLater(keyConfigArea.relayoutLCards)
                                onHeightChanged: Qt.callLater(keyConfigArea.relayoutLCards)
                                /* 09-07: 默认位置绑定 + 拖动偏移 —— 拖动只改 offset 不破坏绑定,
                                 * 放大窗口时 defX/defY 随窗口重算, 卡片不再错位 */
                                property real defX: deviceFrame.x - width - keyConfigArea.cardGap * keyConfigArea.deviceScale
                                property real defY: deviceFrame.y + keyConfigArea.l1NaturalY * keyConfigArea.deviceScale
                                   + (keyConfigArea.l1NaturalH * keyConfigArea.deviceScale - height) / 2
                                   - 20 * keyConfigArea.deviceScale
                                property real dragOffsetX: 0
                                property real dragOffsetY: 0
                                property real pressCardX: 0
                                property real pressCardY: 0
                                property real pressGlobalX: 0
                                property real pressGlobalY: 0
                                property bool wasDragged: false
                                x: defX + dragOffsetX
                                y: defY + dragOffsetY
                                width: lKeyIsRegular(2) ? Math.max(200, Math.max(l1TitleRow.implicitWidth, l1RegText.implicitWidth) + 16) : 200
                                height: l1InfoCol.height + 12
                                radius: 8
                                color: "#ffffff"
                                border.color: hovered ? "#07c160" : "#ececec"
                                border.width: hovered ? 0.5 : 0

                                /* 柔和投影：两层半透明矩形模拟阴影（避免引入 QtGraphicalEffects） */
                                Rectangle {
                                    anchors.fill: parent
                                    anchors.margins: -2
                                    radius: parent.radius + 2
                                    color: "#0d000000"
                                    z: -1
                                }
                                Rectangle {
                                    anchors.fill: parent
                                    anchors.margins: -4
                                    radius: parent.radius + 4
                                    color: "#04000000"
                                    z: -1
                                }

                                onXChanged: lineCanvas.requestPaint()
                                onYChanged: lineCanvas.requestPaint()

                                ColumnLayout {
                                    id: l1InfoCol
                                    anchors.left: parent.left
                                    anchors.right: parent.right
                                    anchors.verticalCenter: parent.verticalCenter
                                    anchors.leftMargin: 8
                                    anchors.rightMargin: 8
                                    spacing: 5

                                    RowLayout {
                                        id: l1TitleRow
                                        spacing: 4
                                        Text {
                                            text: "KEY L1:"
                                            font.pointSize: 10
                                            color: "#333"
                                            font.bold: true
                                            Layout.fillWidth: true
                                        }
                                        Rectangle {
                                            Layout.preferredHeight: 18
                                            radius: 4
                                            color: "#07c160"
                                            Layout.alignment: Qt.AlignVCenter
                                            Layout.preferredWidth: l1ModeLabel.implicitWidth + 14
                                            Text {
                                                id: l1ModeLabel
                                                anchors.centerIn: parent
                                                text: lKeyIsRegular(2) ? "常规模式" : "手势模式"
                                                font.pointSize: 8
                                                color: "white"
                                                font.bold: true
                                            }
                                        }
                                    }

                                    RowLayout {
                                        visible: !lKeyIsRegular(2)
                                        spacing: 3
                                        Repeater {
                                            model: 3
                                            Rectangle {
                                                Layout.preferredWidth: 58
                                                Layout.preferredHeight: 38
                                                radius: 4
                                                color: "#f5f5f5"
                                                ColumnLayout {
                                                    anchors.fill: parent
                                                    anchors.margins: 2
                                                    spacing: 0
                                                    Text {
                                                        text: ["单击", "双击", "长按"][index]
                                                        font.pointSize: 7
                                                        color: "#999"
                                                        Layout.fillWidth: true
                                                        horizontalAlignment: Text.AlignHCenter
                                                    }
                                                    Text {
                                                        text: { var _ = refreshKeyTick; return lKeyGestureStatusText(2, index) }
                                                        font.pointSize: 8
                                                        color: "#333"
                                                        Layout.fillWidth: true
                                                        horizontalAlignment: Text.AlignHCenter
                                                        elide: Text.ElideRight
                                                    }
                                                }
                                            }
                                        }
                                    }

                                    Rectangle {
                                        visible: lKeyIsRegular(2)
                                        Layout.fillWidth: true
                                        Layout.preferredHeight: 28
                                        radius: 4
                                        color: "#f5f5f5"
                                        Text {
                                            id: l1RegText
                                            anchors.fill: parent
                                            anchors.leftMargin: 6
                                            anchors.rightMargin: 6
                                            verticalAlignment: Text.AlignVCenter
                                            text: { var _ = refreshKeyTick; return lKeyStatusText(2) }
                                            font.pointSize: 9
                                            color: "#333"
                                        }
                                    }
                                }

                                MouseArea {
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    onPressed: {
                                        parent.wasDragged = false
                                        parent.pressCardX = parent.x
                                        parent.pressCardY = parent.y
                                        var _g = mapToGlobal(Qt.point(mouseX, mouseY))
                                        parent.pressGlobalX = _g.x
                                        parent.pressGlobalY = _g.y
                                    }
                                    onPositionChanged: {
                                        if (!pressed) return   /* 09-07 修: 悬停移动不拖动, 仅按住才累加偏移 */
                                        var _g = mapToGlobal(Qt.point(mouseX, mouseY))
                                        var dx = _g.x - parent.pressGlobalX
                                        var dy = _g.y - parent.pressGlobalY
                                        if (Math.abs(dx) > 2 || Math.abs(dy) > 2) {
                                            parent.wasDragged = true
                                            parent.dragOffsetX = Math.max(-parent.defX, Math.min(keyConfigArea.width - parent.width - parent.defX, parent.pressCardX + dx - parent.defX))
                                            parent.dragOffsetY = Math.max(-parent.defY, Math.min(keyConfigArea.height - parent.height - parent.defY, parent.pressCardY + dy - parent.defY))
                                        }
                                    }
                                    cursorShape: pressed ? Qt.ClosedHandCursor : Qt.PointingHandCursor
                                    onClicked: { if (!parent.wasDragged) keyConfigArea.showKeySettings(l1InfoCard.cardIndex) }
                                    onEntered: parent.hovered = true
                                    onExited: parent.hovered = false
                                }
                            }

                            /* 09-03: 摇一摇状态卡片 —— 固定左下角，不连红线，仅点击弹窗。
                             * 仅新固件(52B 回包)才有此功能, 但旧固件读回默认"关", 卡片
                             * 同样显示(写回仍安全: python 用 43B payload, 旧固件忽略新字段)。 */
                            Rectangle {
                                id: shakeInfoCard
                                property int cardIndex: 4
                                property bool hovered: false
                                visible: keyConfigArea.hasRead
                                x: 12
                                y: keyConfigArea.height - height - 12
                                width: shakeInfoCol.width + 16
                                height: shakeInfoCol.height + 12
                                radius: 8
                                color: "#ffffff"
                                border.color: hovered ? "#07c160" : "#ececec"
                                border.width: hovered ? 0.5 : 0

                                Rectangle {
                                    anchors.fill: parent
                                    anchors.margins: -2
                                    radius: parent.radius + 2
                                    color: "#0d000000"
                                    z: -1
                                }
                                Rectangle {
                                    anchors.fill: parent
                                    anchors.margins: -4
                                    radius: parent.radius + 4
                                    color: "#04000000"
                                    z: -1
                                }

                                Column {
                                    id: shakeInfoCol
                                    anchors.centerIn: parent
                                    spacing: 2
                                    Text {
                                        text: "摇一摇:"
                                        font.pointSize: 10
                                        font.bold: true
                                        color: "#666"
                                    }
                                    Text {
                                        text: shakeStatusLine()
                                        font.pointSize: 9
                                        color: "#333"
                                        width: 170
                                        elide: Text.ElideRight
                                    }
                                }

                                MouseArea {
                                    anchors.fill: parent
                                    hoverEnabled: true
                                    cursorShape: Qt.PointingHandCursor
                                    onClicked: {
                                        if (keyConfig.captureIndex === -2)
                                            keyConfig.cancelCapture()   /* 正在捕获摇一摇时点卡片=取消 */
                                        shakeSettingsPopup.open()
                                    }
                                    onEntered: parent.hovered = true
                                    onExited: parent.hovered = false
                                }
                            }

                            /* KEY C1/C2/C3 配置卡片（可拖动，红线连接） */
                            Repeater {
                                id: cCardRepeater
                                model: 3
                                delegate: Rectangle {
                                    property int ki: index
                                    objectName: "cardC" + (ki + 1)
                                    property int cardIndex: ki
                                    property bool hovered: false
                                    onHoveredChanged: lineCanvas.requestPaint()
                                    visible: keyConfigArea.hasRead
                                    onVisibleChanged: if (visible) Qt.callLater(keyConfigArea.relayoutRCards)
                                    onHeightChanged: Qt.callLater(keyConfigArea.relayoutRCards)
                                    /* 09-07: 默认位置绑定 + 拖动偏移(见 L1 卡说明) */
                                    property real defX: deviceFrame.x + deviceFrame.width + keyConfigArea.cardGap * keyConfigArea.deviceScale
                                    property real defY: deviceFrame.y
                                          + (ki === 0 ? keyConfigArea.c1CardNaturalY
                                                      : ki === 1 ? keyConfigArea.c2CardNaturalY
                                                                 : keyConfigArea.c3CardNaturalY) * keyConfigArea.deviceScale
                                          + keyConfigArea.rightCardsYOffset * keyConfigArea.deviceScale
                                          - height / 2
                                    property real dragOffsetX: 0
                                    property real dragOffsetY: 0
                                    property real pressCardX: 0
                                    property real pressCardY: 0
                                    property real pressGlobalX: 0
                                    property real pressGlobalY: 0
                                    property bool wasDragged: false
                                    x: defX + dragOffsetX
                                    y: defY + dragOffsetY
                                    width: 220
                                    height: cCardContent.implicitHeight + 12
                                    radius: 8
                                    color: "#ffffff"
                                    border.color: hovered ? "#07c160" : "#ececec"
                                    border.width: hovered ? 0.5 : 0

                                    Rectangle {
                                        anchors.fill: parent
                                        anchors.margins: -2
                                        radius: parent.radius + 2
                                        color: "#0d000000"
                                        z: -1
                                    }
                                    Rectangle {
                                        anchors.fill: parent
                                        anchors.margins: -4
                                        radius: parent.radius + 4
                                        color: "#04000000"
                                        z: -1
                                    }

                                    onXChanged: lineCanvas.requestPaint()
                                    onYChanged: lineCanvas.requestPaint()

                                    ColumnLayout {
                                            id: cCardContent
                                            anchors.left: parent.left
                                            anchors.right: parent.right
                                            anchors.verticalCenter: parent.verticalCenter
                                            anchors.leftMargin: 8
                                            anchors.rightMargin: 8
                                            spacing: 5

                                            /* 标题行: RGB色块 + KEY C1: + 绿色模式标签 */
                                            RowLayout {
                                                Layout.fillWidth: true
                                                spacing: 4

                                                Rectangle {
                                                    Layout.preferredWidth: 16; Layout.preferredHeight: 16; radius: 4
                                                    Layout.alignment: Qt.AlignVCenter
                                                    color: {
                                                        if (keyConfig.keyConfigs[ki].rgb_enabled) {
                                                            var c = keyConfig.keyConfigs[ki].rgb_color
                                                            return Qt.rgba(((c >> 16) & 0xFF) / 255, ((c >> 8) & 0xFF) / 255, (c & 0xFF) / 255, 0.35)
                                                        }
                                                        return "#cccccc"
                                                    }
                                                    border.color: "#dddddd"
                                                    border.width: 1
                                                }

                                                Text {
                                                    text: "KEY C" + (ki + 1) + ":"
                                                    font.pointSize: 10
                                                    color: "#333"
                                                    font.bold: true
                                                    Layout.fillWidth: true
                                                }

                                                Rectangle {
                                                    Layout.preferredHeight: 20
                                                    radius: 4
                                                    color: "#07c160"
                                                    Layout.alignment: Qt.AlignVCenter
                                                    Layout.preferredWidth: modeLabel.implicitWidth + 16
                                                    Text {
                                                        id: modeLabel
                                                        anchors.centerIn: parent
                                                        text: cKeyIsRegular(ki) ? "常规模式" : "手势模式"
                                                        font.pointSize: 8
                                                        color: "white"
                                                        font.bold: true
                                                    }
                                                }
                                            }

                                            /* 手势模式: 三列 单击/双击/长按 */
                                            RowLayout {
                                                visible: !cKeyIsRegular(ki)
                                                Layout.fillWidth: true
                                                Layout.preferredHeight: 40
                                                spacing: 4

                                                Repeater {
                                                    model: 3
                                                    Rectangle {
                                                        Layout.fillWidth: true
                                                        Layout.preferredHeight: 40
                                                        radius: 4
                                                        color: "#f5f5f5"
                                                        ColumnLayout {
                                                            anchors.fill: parent
                                                            anchors.margins: 3
                                                            spacing: 0
                                                            Text {
                                                                text: ["单击", "双击", "长按"][index]
                                                                font.pointSize: 7
                                                                color: "#999"
                                                                Layout.fillWidth: true
                                                                horizontalAlignment: Text.AlignHCenter
                                                            }
                                                            Text {
                                                                text: { var _ = refreshKeyTick; return cKeyStatusText(ki, index) }
                                                                font.pointSize: 8
                                                                color: "#333"
                                                                Layout.fillWidth: true
                                                                horizontalAlignment: Text.AlignHCenter
                                                                elide: Text.ElideRight
                                                            }
                                                        }
                                                    }
                                                }
                                            }

                                            /* 常规模式: 一列 */
                                            Rectangle {
                                                visible: cKeyIsRegular(ki)
                                                Layout.fillWidth: true
                                                Layout.preferredHeight: 30
                                                radius: 4
                                                color: "#f5f5f5"
                                                Text {
                                                    anchors.fill: parent
                                                    anchors.leftMargin: 8
                                                    verticalAlignment: Text.AlignVCenter
                                                    text: { var _ = refreshKeyTick; return cKeyStatusText(ki, 0) }
                                                    font.pointSize: 9
                                                    color: "#333"
                                                }
                                            }
                                        }

                                        MouseArea {
                                                    anchors.fill: parent
                                            hoverEnabled: true
                                            onPressed: {
                                                parent.wasDragged = false
                                                parent.pressCardX = parent.x
                                                parent.pressCardY = parent.y
                                                var _g = mapToGlobal(Qt.point(mouseX, mouseY))
                                                parent.pressGlobalX = _g.x
                                                parent.pressGlobalY = _g.y
                                            }
                                            onPositionChanged: {
                                                if (!pressed) return   /* 09-07 修: 悬停移动不拖动, 仅按住才累加偏移 */
                                                var _g = mapToGlobal(Qt.point(mouseX, mouseY))
                                                var dx = _g.x - parent.pressGlobalX
                                                var dy = _g.y - parent.pressGlobalY
                                                if (Math.abs(dx) > 2 || Math.abs(dy) > 2) {
                                                    parent.wasDragged = true
                                                    parent.dragOffsetX = Math.max(-parent.defX, Math.min(keyConfigArea.width - parent.width - parent.defX, parent.pressCardX + dx - parent.defX))
                                                    parent.dragOffsetY = Math.max(-parent.defY, Math.min(keyConfigArea.height - parent.height - parent.defY, parent.pressCardY + dy - parent.defY))
                                                }
                                            }
                                            cursorShape: pressed ? Qt.ClosedHandCursor : Qt.PointingHandCursor
                                            onClicked: { if (!parent.wasDragged) keyConfigArea.showKeySettings(ki) }
                                            onEntered: parent.hovered = true
                                            onExited: parent.hovered = false
                                        }
                                    }
                                }

                                /* 09-05: KEY L2/L3 快捷键卡片(设备左缘, 与 C 卡同款可拖动+红线)。
                                 * 无 RGB 硬件 -> 不带色块; 索引 5/6(3=L1, 4=摇一摇)。 */
                                Repeater {
                                    id: lCardRepeater
                                    model: 2
                                    delegate: Rectangle {
                                        property int ki: 5 + index
                                        property int which: index    /* 0=L2 1=L3, 供内层 Repeater 访问 */
                                        property int cardIndex: ki   /* 红线画布按 cardIndex 找端点 */
                                        objectName: "cardL" + (index + 2)
                                        property bool hovered: false
                                    onHoveredChanged: lineCanvas.requestPaint()
                                        visible: keyConfigArea.hasRead
                                        onVisibleChanged: if (visible) Qt.callLater(keyConfigArea.relayoutLCards)
                                        onHeightChanged: Qt.callLater(keyConfigArea.relayoutLCards)
                                        /* 09-07: 默认位置绑定 + 拖动偏移(见 L1 卡说明) */
                                        property real defX: deviceFrame.x - width - keyConfigArea.cardGap * keyConfigArea.deviceScale
                                        property real defY: deviceFrame.y
                                          + (((index === 0) ? keyConfigArea.l2NaturalY : keyConfigArea.l3NaturalY)
                                             + keyConfigArea.lNaturalH / 2) * keyConfigArea.deviceScale
                                          - height / 2
                                        property real dragOffsetX: 0
                                        property real dragOffsetY: 0
                                        property real pressCardX: 0
                                        property real pressCardY: 0
                                        property real pressGlobalX: 0
                                        property real pressGlobalY: 0
                                        property bool wasDragged: false
                                        x: defX + dragOffsetX
                                        y: defY + dragOffsetY
                                        width: lKeyIsRegular(which) ? Math.max(200, Math.max(lTitleRow.implicitWidth, lRegText.implicitWidth) + 16) : 200
                                        height: lCardContent.implicitHeight + 12
                                        radius: 8
                                        color: "#ffffff"
                                        border.color: hovered ? "#07c160" : "#ececec"
                                        border.width: hovered ? 0.5 : 0

                                        Rectangle {
                                            anchors.fill: parent
                                            anchors.margins: -2
                                            radius: parent.radius + 2
                                            color: "#0d000000"
                                            z: -1
                                        }
                                        Rectangle {
                                            anchors.fill: parent
                                            anchors.margins: -4
                                            radius: parent.radius + 4
                                            color: "#04000000"
                                            z: -1
                                        }

                                        onXChanged: lineCanvas.requestPaint()
                                        onYChanged: lineCanvas.requestPaint()

                                        ColumnLayout {
                                            id: lCardContent
                                            anchors.left: parent.left
                                            anchors.right: parent.right
                                            anchors.verticalCenter: parent.verticalCenter
                                            anchors.leftMargin: 8
                                            anchors.rightMargin: 8
                                            spacing: 5

                                            RowLayout {
                                                id: lTitleRow
                                                Layout.fillWidth: true
                                                spacing: 4
                                                Text {
                                                    text: "KEY L" + (which + 2) + ":"
                                                    font.pointSize: 10
                                                    color: "#333"
                                                    font.bold: true
                                                    Layout.fillWidth: true
                                                }
                                                Rectangle {
                                                    Layout.preferredHeight: 18
                                                    radius: 4
                                                    color: "#07c160"
                                                    Layout.alignment: Qt.AlignVCenter
                                                    Layout.preferredWidth: lModeLabel.implicitWidth + 14
                                                    Text {
                                                        id: lModeLabel
                                                        anchors.centerIn: parent
                                                        text: lKeyIsRegular(which) ? "常规模式" : "手势模式"
                                                        font.pointSize: 8
                                                        color: "white"
                                                        font.bold: true
                                                    }
                                                }
                                            }

                                            RowLayout {
                                                visible: !lKeyIsRegular(which)
                                                spacing: 3
                                                Repeater {
                                                    model: 3
                                                    Rectangle {
                                                        Layout.preferredWidth: 58
                                                        Layout.preferredHeight: 38
                                                        radius: 4
                                                        color: "#f5f5f5"
                                                        ColumnLayout {
                                                            anchors.fill: parent
                                                            anchors.margins: 2
                                                            spacing: 0
                                                            Text {
                                                                text: ["单击", "双击", "长按"][index]
                                                                font.pointSize: 7
                                                                color: "#999"
                                                                Layout.fillWidth: true
                                                                horizontalAlignment: Text.AlignHCenter
                                                            }
                                                            Text {
                                                                text: { var _ = refreshKeyTick; return lKeyGestureStatusText(which, index) }
                                                                font.pointSize: 8
                                                                color: "#333"
                                                                Layout.fillWidth: true
                                                                horizontalAlignment: Text.AlignHCenter
                                                                elide: Text.ElideRight
                                                            }
                                                        }
                                                    }
                                                }
                                            }

                                            Rectangle {
                                                visible: lKeyIsRegular(which)
                                                Layout.fillWidth: true
                                                Layout.preferredHeight: 28
                                                radius: 4
                                                color: "#f5f5f5"
                                                Text {
                                                    id: lRegText
                                                    anchors.fill: parent
                                                    anchors.leftMargin: 6
                                                    anchors.rightMargin: 6
                                                    verticalAlignment: Text.AlignVCenter
                                                    text: { var _ = refreshKeyTick; return lKeyStatusText(which) }
                                                    font.pointSize: 9
                                                    color: "#333"
                                                }
                                            }
                                        }

                                        MouseArea {
                                            anchors.fill: parent
                                            hoverEnabled: true
                                            onPressed: {
                                                parent.wasDragged = false
                                                parent.pressCardX = parent.x
                                                parent.pressCardY = parent.y
                                                var _g = mapToGlobal(Qt.point(mouseX, mouseY))
                                                parent.pressGlobalX = _g.x
                                                parent.pressGlobalY = _g.y
                                            }
                                            onPositionChanged: {
                                                if (!pressed) return   /* 09-07 修: 悬停移动不拖动, 仅按住才累加偏移 */
                                                var _g = mapToGlobal(Qt.point(mouseX, mouseY))
                                                var dx = _g.x - parent.pressGlobalX
                                                var dy = _g.y - parent.pressGlobalY
                                                if (Math.abs(dx) > 2 || Math.abs(dy) > 2) {
                                                    parent.wasDragged = true
                                                    parent.dragOffsetX = Math.max(-parent.defX, Math.min(keyConfigArea.width - parent.width - parent.defX, parent.pressCardX + dx - parent.defX))
                                                    parent.dragOffsetY = Math.max(-parent.defY, Math.min(keyConfigArea.height - parent.height - parent.defY, parent.pressCardY + dy - parent.defY))
                                                }
                                            }
                                            cursorShape: pressed ? Qt.ClosedHandCursor : Qt.PointingHandCursor
                                            onClicked: { if (!parent.wasDragged) keyConfigArea.showKeySettings(ki) }
                                            onEntered: parent.hovered = true
                                            onExited: parent.hovered = false
                                        }
                                    }
                                }

                                /* 09-05: EC 编码器配置卡片(三手势: 旋转上/按下/旋转下, 可拖动+红线) */
                                Rectangle {
                                    id: ecInfoCard
                                    property int cardIndex: 7
                                    objectName: "cardEC"
                                    property bool hovered: false
                                    onHoveredChanged: lineCanvas.requestPaint()
                                    visible: keyConfigArea.hasRead
                                    onVisibleChanged: if (visible) Qt.callLater(keyConfigArea.relayoutRCards)
                                    onHeightChanged: Qt.callLater(keyConfigArea.relayoutRCards)
                                    /* 09-07: 默认位置绑定 + 拖动偏移(见 L1 卡说明) */
                                    property real defX: deviceFrame.x + deviceFrame.width + keyConfigArea.cardGap * keyConfigArea.deviceScale
                                    property real defY: deviceFrame.y
                                      + keyConfigArea.ecCardNaturalY * keyConfigArea.deviceScale
                                      + keyConfigArea.rightCardsYOffset * keyConfigArea.deviceScale
                                      - height / 2
                                    property real dragOffsetX: 0
                                    property real dragOffsetY: 0
                                    property real pressCardX: 0
                                    property real pressCardY: 0
                                    property real pressGlobalX: 0
                                    property real pressGlobalY: 0
                                    property bool wasDragged: false
                                    x: defX + dragOffsetX
                                    y: defY + dragOffsetY
                                    width: 220
                                    height: ecCardContent.implicitHeight + 12
                                    radius: 8
                                    color: "#ffffff"
                                    border.color: hovered ? "#07c160" : "#ececec"
                                    border.width: hovered ? 0.5 : 0

                                    Rectangle {
                                        anchors.fill: parent
                                        anchors.margins: -2
                                        radius: parent.radius + 2
                                        color: "#0d000000"
                                        z: -1
                                    }
                                    Rectangle {
                                        anchors.fill: parent
                                        anchors.margins: -4
                                        radius: parent.radius + 4
                                        color: "#04000000"
                                        z: -1
                                    }

                                    onXChanged: lineCanvas.requestPaint()
                                    onYChanged: lineCanvas.requestPaint()

                                    ColumnLayout {
                                        id: ecCardContent
                                        anchors.left: parent.left
                                        anchors.right: parent.right
                                        anchors.verticalCenter: parent.verticalCenter
                                        anchors.leftMargin: 8
                                        anchors.rightMargin: 6
                                        spacing: 4

                                        /* 标题 + 模式徽标 */
                                        RowLayout {
                                            id: ecTitleRow
                                            Layout.fillWidth: true
                                            spacing: 4
                                            Text {
                                                text: "EC 编码器:"
                                                font.pointSize: 10
                                                font.bold: true
                                                color: "#666"
                                                Layout.fillWidth: true
                                                elide: Text.ElideRight
                                            }
                                            Rectangle {
                                                Layout.preferredHeight: 20
                                                radius: 4
                                                color: "#07c160"
                                                Layout.alignment: Qt.AlignVCenter
                                                Layout.preferredWidth: ecModeBadgeText.implicitWidth + 16
                                                Text {
                                                    id: ecModeBadgeText
                                                    anchors.centerIn: parent
                                                    text: ecPressModeGesture ? "手势模式" : "常规模式"
                                                    font.pointSize: 8
                                                    color: "white"
                                                    font.bold: true
                                                }
                                            }
                                        }

                                        /* 上: gesture 0 —— 始终显示动作
                                           10-05: 行首「上:」文字标签换成图标(与网页端 EC 卡一致)——
                                           图标落在卡片白底上, 数值装进灰条; 不参与灰色行底。mirror 表示反向。 */
                                        RowLayout {
                                            Layout.fillWidth: true
                                            Layout.preferredHeight: 22
                                            Layout.leftMargin: 6
                                            Layout.rightMargin: 6
                                            spacing: 4
                                            Image {
                                                Layout.alignment: Qt.AlignVCenter
                                                Layout.preferredWidth: 12
                                                Layout.preferredHeight: 12
                                                source: "qrc:/Sidebar/ec-rotate.png"
                                                fillMode: Image.PreserveAspectFit
                                                smooth: true
                                                mirror: true          /* 原图是「下」的方向 */
                                            }
                                            Rectangle {
                                                Layout.fillWidth: true
                                                Layout.preferredHeight: 22
                                                radius: 4
                                                color: "#f5f7f6"
                                                Text {
                                                    anchors.fill: parent
                                                    anchors.leftMargin: 6
                                                    anchors.rightMargin: 6
                                                    verticalAlignment: Text.AlignVCenter
                                                    text: { var _ = keyConfig.ecCwKey; return ecGestureText(0) }
                                                    font.pointSize: 10
                                                    color: "#333"
                                                    elide: Text.ElideRight
                                                }
                                            }
                                        }

                                        /* 10-04: 压上(按压滚动上) —— 仅手势模式(与弹窗 5 行保持一致) */
                                        RowLayout {
                                            Layout.fillWidth: true
                                            Layout.preferredHeight: 22
                                            Layout.leftMargin: 6
                                            Layout.rightMargin: 6
                                            visible: ecPressModeGesture
                                            spacing: 4
                                            Image {
                                                Layout.alignment: Qt.AlignVCenter
                                                Layout.preferredWidth: 12
                                                Layout.preferredHeight: 12
                                                source: "qrc:/Sidebar/ec-press-rotate.png"
                                                fillMode: Image.PreserveAspectFit
                                                smooth: true
                                                mirror: true
                                            }
                                            Rectangle {
                                                Layout.fillWidth: true
                                                Layout.preferredHeight: 22
                                                radius: 4
                                                color: "#f5f7f6"
                                                Text {
                                                    anchors.fill: parent
                                                    anchors.leftMargin: 6
                                                    anchors.rightMargin: 6
                                                    verticalAlignment: Text.AlignVCenter
                                                    text: { var _ = keyConfig.ecCwPressKey; return ecGestureText(3) }
                                                    font.pointSize: 10
                                                    color: "#333"
                                                    elide: Text.ElideRight
                                                }
                                            }
                                        }

                                        /* 按: 常规显示动作(单行); 手势模式显示单击/双击/长按三小卡(双行)
                                           10-05: 「按:」标签换成按压图标(图标落白底, 数值/三格仍带灰底)。 */
                                        /* 常规行 */
                                        RowLayout {
                                            id: ecPressRowNormal
                                            Layout.fillWidth: true
                                            Layout.preferredHeight: 22
                                            Layout.leftMargin: 6
                                            Layout.rightMargin: 6
                                            visible: !ecPressModeGesture
                                            spacing: 4
                                            Image {
                                                Layout.alignment: Qt.AlignVCenter
                                                Layout.preferredWidth: 12
                                                Layout.preferredHeight: 12
                                                source: "qrc:/Sidebar/ec-press.png"
                                                fillMode: Image.PreserveAspectFit
                                                smooth: true
                                            }
                                            Rectangle {
                                                Layout.fillWidth: true
                                                Layout.preferredHeight: 22
                                                radius: 4
                                                color: "#f5f7f6"
                                                Text {
                                                    anchors.fill: parent
                                                    anchors.leftMargin: 6
                                                    anchors.rightMargin: 6
                                                    verticalAlignment: Text.AlignVCenter
                                                    text: { var _ = keyConfig.ecPressKey; return ecGestureText(1) }
                                                    font.pointSize: 10
                                                    color: "#333"
                                                    elide: Text.ElideRight
                                                }
                                            }
                                        }

                                        /* 手势行 —— 三子卡(单击/双击/长按), 每卡双行: 手势名 + 实时动作状态 */
                                        RowLayout {
                                            id: ecPressRowGesture
                                            Layout.fillWidth: true
                                            Layout.preferredHeight: 38
                                            Layout.leftMargin: 6
                                            Layout.rightMargin: 6
                                            visible: ecPressModeGesture
                                            spacing: 4
                                            Image {
                                                Layout.alignment: Qt.AlignVCenter
                                                Layout.preferredWidth: 12
                                                Layout.preferredHeight: 12
                                                source: "qrc:/Sidebar/ec-press.png"
                                                fillMode: Image.PreserveAspectFit
                                                smooth: true
                                            }
                                            Repeater {
                                                model: [
                                                    { label: "单击", g: 0 },
                                                    { label: "双击", g: 1 },
                                                    { label: "长按", g: 2 }
                                                ]
                                                delegate: Rectangle {
                                                    Layout.fillWidth: true
                                                    Layout.fillHeight: true
                                                    radius: 4
                                                    color: "#f5f7f6"
                                                    border.color: "transparent"
                                                    border.width: 0
                                                    ColumnLayout {
                                                        anchors.fill: parent
                                                        anchors.margins: 1
                                                        spacing: 0
                                                        Text {
                                                            Layout.alignment: Qt.AlignHCenter
                                                            text: modelData.label
                                                            font.pointSize: 7
                                                            color: "#999"
                                                        }
                                                        Text {
                                                            Layout.alignment: Qt.AlignHCenter
                                                            text: { var _ = keyConfig.ecPressKey; return ecPressGestureStatusText(modelData.g) }
                                                            font.pointSize: 8
                                                            color: "#333"
                                                            elide: Text.ElideRight
                                                            Layout.maximumWidth: parent.width - 2
                                                        }
                                                    }
                                                }
                                            }
                                        }

                                        /* 下: gesture 2 —— 始终显示动作 */
                                        RowLayout {
                                            Layout.fillWidth: true
                                            Layout.preferredHeight: 22
                                            Layout.leftMargin: 6
                                            Layout.rightMargin: 6
                                            spacing: 4
                                            Image {
                                                Layout.alignment: Qt.AlignVCenter
                                                Layout.preferredWidth: 12
                                                Layout.preferredHeight: 12
                                                source: "qrc:/Sidebar/ec-rotate.png"
                                                fillMode: Image.PreserveAspectFit
                                                smooth: true
                                            }
                                            Rectangle {
                                                Layout.fillWidth: true
                                                Layout.preferredHeight: 22
                                                radius: 4
                                                color: "#f5f7f6"
                                                Text {
                                                    anchors.fill: parent
                                                    anchors.leftMargin: 6
                                                    anchors.rightMargin: 6
                                                    verticalAlignment: Text.AlignVCenter
                                                    text: { var _ = keyConfig.ecCcwKey; return ecGestureText(2) }
                                                    font.pointSize: 10
                                                    color: "#333"
                                                    elide: Text.ElideRight
                                                }
                                            }
                                        }

                                        /* 10-04: 压下(按压滚动下) —— 仅手势模式 */
                                        RowLayout {
                                            Layout.fillWidth: true
                                            Layout.preferredHeight: 22
                                            Layout.leftMargin: 6
                                            Layout.rightMargin: 6
                                            visible: ecPressModeGesture
                                            spacing: 4
                                            Image {
                                                Layout.alignment: Qt.AlignVCenter
                                                Layout.preferredWidth: 12
                                                Layout.preferredHeight: 12
                                                source: "qrc:/Sidebar/ec-press-rotate.png"
                                                fillMode: Image.PreserveAspectFit
                                                smooth: true
                                            }
                                            Rectangle {
                                                Layout.fillWidth: true
                                                Layout.preferredHeight: 22
                                                radius: 4
                                                color: "#f5f7f6"
                                                Text {
                                                    anchors.fill: parent
                                                    anchors.leftMargin: 6
                                                    anchors.rightMargin: 6
                                                    verticalAlignment: Text.AlignVCenter
                                                    text: { var _ = keyConfig.ecCcwPressKey; return ecGestureText(4) }
                                                    font.pointSize: 10
                                                    color: "#333"
                                                    elide: Text.ElideRight
                                                }
                                            }
                                        }
                                    }

                                    MouseArea {
                                        anchors.fill: parent
                                        hoverEnabled: true
                                        onPressed: {
                                            parent.wasDragged = false
                                            parent.pressCardX = parent.x
                                            parent.pressCardY = parent.y
                                            var _g = mapToGlobal(Qt.point(mouseX, mouseY))
                                            parent.pressGlobalX = _g.x
                                            parent.pressGlobalY = _g.y
                                        }
                                        onPositionChanged: {
                                            if (!pressed) return   /* 09-07 修: 悬停移动不拖动, 仅按住才累加偏移 */
                                            var _g = mapToGlobal(Qt.point(mouseX, mouseY))
                                            var dx = _g.x - parent.pressGlobalX
                                            var dy = _g.y - parent.pressGlobalY
                                            if (Math.abs(dx) > 2 || Math.abs(dy) > 2) {
                                                parent.wasDragged = true
                                                parent.dragOffsetX = Math.max(-parent.defX, Math.min(keyConfigArea.width - parent.width - parent.defX, parent.pressCardX + dx - parent.defX))
                                                parent.dragOffsetY = Math.max(-parent.defY, Math.min(keyConfigArea.height - parent.height - parent.defY, parent.pressCardY + dy - parent.defY))
                                            }
                                        }
                                        cursorShape: pressed ? Qt.ClosedHandCursor : Qt.PointingHandCursor
                                        onClicked: { if (!parent.wasDragged) keyConfigArea.showKeySettings(ecInfoCard.cardIndex) }   /* 09-06 修: 非 delegate 卡片裸 cardIndex 不可见(仅组件根属性可见), 需 id 前缀 */
                                        onEntered: parent.hovered = true
                                        onExited: parent.hovered = false
                                    }
                                }

                            /* 红线连接层：把可拖动卡片中心与对应设备按键中心用红色虚线连接 */
                            Canvas {
                                id: lineCanvas
                                anchors.fill: parent
                                z: 100
                                enabled: false   /* 只绘制, 不拦截鼠标 */
                                Component.onCompleted: requestPaint()
                                Connections {
                                    target: keySettingsPopup
                                    function onSelectedKeyIndexChanged() { lineCanvas.requestPaint() }
                                }

                                onPaint: {
                                    var ctx = getContext("2d")
                                    ctx.reset()
                                    /* ★ 10-05: 虚线样式对齐网页端(web_config `03-cards.css` 的
                                     * `svg.connectors path { stroke-width:1.5; stroke-dasharray:3 4 }`,
                                     * 且未设 stroke-linecap ⇒ 默认 butt; 常态色=`--conn` #b3b9bf,
                                     * hover=`path.hot` ⇒ accent 色且加粗到 2)。
                                     * 原为 lineWidth 2 / dash[6,4] / lineCap "round" —— 圆头+长间隔
                                     * 与网页端观感差得较远(用户反馈"跟网页端不一样")。
                                     * lineWidth 因 hover 要变，放到循环内按需设置。 */
                                    ctx.setLineDash([3, 4])
                                    ctx.lineCap = "butt"
                                    for (var i = 0; i < keyConfigArea.children.length; ++i) {
                                        var item = keyConfigArea.children[i]
                                        if (!item.objectName || item.objectName.indexOf("card") !== 0 || !item.visible)
                                            continue

                                        var ki = item.cardIndex
                                        var to = keyConfigArea.hotspotCenter(ki)
                                        if (!to) continue

                                        /* hover 的卡片虚线变绿且加粗, 其余浅灰(对齐网页端 path.hot / --conn) */
                                        var isHover = item.hovered || (ki === keySettingsPopup.selectedKeyIndex)
                                        ctx.lineWidth = isHover ? 2 : 1.5
                                        ctx.strokeStyle = isHover ? "#07c160" : "#b3b9bf"

                                        /* 线从卡片侧边中点出发：
                                         * 如果按键在卡片右侧，用卡片左边缘；否则用右边缘。
                                         * 这样线不会横穿卡片文字。 */
                                        var itemCenterX = item.x + item.width / 2
                                        var fromSideX = (to.x > itemCenterX) ? item.width : 0
                                        var from = item.mapToItem(keyConfigArea, fromSideX, item.height / 2)

                                        /* 三次贝塞尔曲线：红线从卡片侧边水平出发，
                                         * 到设备按键处水平进入，形成参考图里那种平滑 S 形弧线。
                                         * CP1 在卡片侧边水平外扩，CP2 在按键端水平外扩。 */
                                        var dx = to.x - from.x
                                        var horizontalGap = Math.abs(dx)
                                        if (horizontalGap < 1) horizontalGap = 1

                                        var dir = (to.x > from.x) ? 1 : -1
                                        var cpOffset = Math.min(horizontalGap * 0.5, 100)

                                        var cp1x = from.x + dir * cpOffset
                                        var cp1y = from.y
                                        var cp2x = to.x - dir * cpOffset
                                        var cp2y = to.y

                                        ctx.beginPath()
                                        ctx.moveTo(from.x, from.y)
                                        ctx.bezierCurveTo(cp1x, cp1y, cp2x, cp2y, to.x, to.y)
                                        ctx.stroke()
                                        /* 10-05: 原此处本会在设备端画一个半径 3 的实心圆点,
                                         * 按用户要求去掉(网页端也没有端点标记)。 */
                                    }
                                }
                            }

                            Popup {
                                id: keySettingsPopup
                                parent: Overlay.overlay

                                property int selectedKeyIndex: -1
                                property int cGestureState: 0   /* 09-07: C 键当前编辑手势 0=单击 1=双击 2=长按 */
                                property int cKeyModeIndex: 0    /* 09-07: C 键模式 0=常规 1=手势 (镜像模式下拉) */
                                /* 09-07: L1/L2/L3(3/5/6)模式 + 当前编辑手势 —— 语义同 C 键 */
                                property int lModeIndex: 0       /* L 键模式 0=常规(直通) 1=手势 */
                                property int lGestureState: 0    /* L 键当前编辑手势 0=单击 1=双击 2=长按 */
                                property real pHue: 0
                                property real pSat: 1
                                property real pVal: 1
                                property color svBaseColor: Qt.hsva(pHue / 360, 1, 1, 1)

                                x: Math.max(12, Math.min(parent.width - width - 12, (parent.width - width) / 2))
                                y: Math.max(40, Math.min(parent.height - height - 12, (parent.height - height) / 2))
                                width: 320
                                padding: 14
                                modal: true
                                focus: true
                                closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
                                background: Rectangle { radius: 12; color: "white"; border.color: "#e8e8e8"; border.width: 1 }
                                Overlay.modal: Rectangle { color: "#80000000"; radius: 12 }
                                onClosed: {
                                    var ci = keyConfig.captureIndex
                                    var ki = selectedKeyIndex
                                    if (ci === ki) {
                                        keyConfig.cancelCapture()
                                    } else if (ki >= 0 && ki < 3 && ci >= 10 + ki * 3 && ci <= 10 + ki * 3 + 2) {
                                        keyConfig.cancelCapture()   /* 09-07: C 键手势捕获 */
                                    } else if ((ki === 3 || ki === 5 || ki === 6)) {
                                        var li = (ki === 3) ? 0 : (ki === 5) ? 1 : 2
                                        if (ci >= 19 + li * 3 && ci <= 19 + li * 3 + 2)
                                            keyConfig.cancelCapture()   /* 09-07: L 键手势捕获 */
                                    }
                                    if (selectedKeyIndex >= 0 && selectedKeyIndex < 3) keyConfig.clearKeyRgb(selectedKeyIndex)
                                    selectedKeyIndex = -1
                                }

                                /* 09-07: C 键三手势 —— 打开弹窗/切换手势后把三个下拉同步到当前值 */
                                function syncCKeyCombo() {
                                    var ki = selectedKeyIndex
                                    var g = cGestureState
                                    if (ki < 0 || ki >= 3) return
                                    var av = cKeyActionValue(ki, g)
                                    var tm = cTypeCombo.model
                                    for (var i = 0; i < tm.length; i++) { if (tm[i].v === av) { cTypeCombo.currentIndex = i; break } }
                                    var mm = cTriggerCombo.model
                                    for (var j = 0; j < mm.length; j++) { if (mm[j].v === av) { cTriggerCombo.currentIndex = j; break } }
                                    var kk = cKeyKeycode(ki, g)
                                    var km = cMmCombo.model
                                    for (var k = 0; k < km.length; k++) { if (km[k].v === kk) { cMmCombo.currentIndex = k; break } }
                                }
                                /* 09-07: C 键模式下拉与数据同步 —— 模式已随配置【显式落盘】
                                 * (读 c_mode / setKeyMode), 此处仅把下拉镜像到真实模式;
                                 * 打开弹窗/手势编辑/捕获完成都会调用, 保证下拉不漂移。 */
                                function syncCKeyMode() {
                                    var ki = selectedKeyIndex
                                    if (ki < 0 || ki >= 3) return
                                    if (!cModeCombo) return
                                    var regular = cKeyIsRegular(ki)
                                    cKeyModeIndex = regular ? 0 : 1
                                    cModeCombo.currentIndex = cKeyModeIndex
                                    /* 数据回归常规模式时, 若正编辑双击/长按则回落到单击(其卡片已锁定) */
                                    if (regular && cGestureState !== 0) {
                                        cGestureState = 0
                                        syncCKeyCombo()
                                    }
                                }
                                /* ============ 09-07: L1/L2/L3(3/5/6) 模式+三手势 状态函数 ============ */
                                /* 卡片索引 -> which(0=L2 1=L3 2=L1), 供 setLKey* 使用 */
                                function lWhichNow() {
                                    var ki = selectedKeyIndex
                                    if (ki === 3) return 2   /* L1 */
                                    if (ki === 5) return 0   /* L2 */
                                    if (ki === 6) return 1   /* L3 */
                                    return -1
                                }
                                /* 卡片索引 -> L 键序号 li(0=L1 1=L2 2=L3), 捕获索引 = 19+li*3+g */
                                function lLiNow() {
                                    var ki = selectedKeyIndex
                                    if (ki === 3) return 0
                                    if (ki === 5) return 1
                                    if (ki === 6) return 2
                                    return -1
                                }
                                /* L 模式下拉与数据同步 —— 模式已随配置【显式落盘】(l_mode /
                                 * setLKeyMode), 此处仅把下拉镜像到真实模式; 打开弹窗/模式切换/
                                 * 捕获完成都会调用。 */
                                function syncLModeUi() {
                                    var which = lWhichNow()
                                    if (which < 0) return
                                    if (!lModeCombo) return
                                    var regular = lKeyIsRegular(which)
                                    lModeIndex = regular ? 0 : 1
                                    lModeCombo.currentIndex = lModeIndex
                                    if (regular && lGestureState !== 0) {
                                        lGestureState = 0
                                        syncLGCombo()
                                    }
                                }
                                /* L 手势配置行下拉同步到当前手势(lGestureState)的实际配置 */
                                function syncLGCombo() {
                                    var which = lWhichNow()
                                    var g = lGestureState
                                    if (which < 0 || g < 0 || g > 2) return
                                    if (!lGTypeCombo) return
                                    var av = lKeyGestureActionValue(which, g)
                                    var cat = (av === "mouse_left" || av === "mouse_right" ||
                                               av === "mouse_middle" || av === "wheel_up" ||
                                               av === "wheel_down") ? "mouse" : av
                                    lGTypeCombo.model = (which === 2) ? lGTypeCombo.lgTypeL1
                                                                       : lGTypeCombo.lgTypePlain
                                    var tm = lGTypeCombo.model
                                    for (var i = 0; i < tm.length; i++) {
                                        if (tm[i].v === cat) { lGTypeCombo.currentIndex = i; break }
                                    }
                                    var mm = lGTriggerCombo.model
                                    for (var j = 0; j < mm.length; j++) {
                                        if (mm[j].v === av) { lGTriggerCombo.currentIndex = j; break }
                                    }
                                    var mk = lGestureField(which, g, "keycode") || 0
                                    var mmm = lGMmCombo.model
                                    for (var k = 0; k < mmm.length; k++) {
                                        if (mmm[k].v === mk) { lGMmCombo.currentIndex = k; break }
                                    }
                                }
                                function clamp(v, a, b) { return Math.max(a, Math.min(b, v)); }
                                function rgbToHsv(r, g, b) {
                                    r /= 255; g /= 255; b /= 255;
                                    var max = Math.max(r, g, b), min = Math.min(r, g, b), d = max - min;
                                    var h = 0;
                                    if (d !== 0) {
                                        if (max === r) h = ((g - b) / d) % 6;
                                        else if (max === g) h = (b - r) / d + 2;
                                        else h = (r - g) / d + 4;
                                        h *= 60; if (h < 0) h += 360;
                                    }
                                    var s = (max === 0) ? 0 : d / max;
                                    return { h: h, s: s, v: max };
                                }
                                function hsvToRgb(h, s, v) {
                                    var c = v * s;
                                    var x = c * (1 - Math.abs((h / 60) % 2 - 1));
                                    var m = v - c;
                                    var r = 0, g = 0, b = 0;
                                    if (h < 60) { r = c; g = x; }
                                    else if (h < 120) { r = x; g = c; }
                                    else if (h < 180) { g = c; b = x; }
                                    else if (h < 240) { g = x; b = c; }
                                    else if (h < 300) { r = x; b = c; }
                                    else { r = c; b = x; }
                                    return { r: Math.round((r + m) * 255), g: Math.round((g + m) * 255), b: Math.round((b + m) * 255) };
                                }
                                function toHex2(v) { var s = Math.round(v).toString(16); return s.length < 2 ? "0" + s : s; }
                                function hexText() {
                                    var c = hsvToRgb(pHue, pSat, pVal);
                                    return "#" + toHex2(c.r) + toHex2(c.g) + toHex2(c.b);
                                }
                                function syncFromColor() {
                                    if (selectedKeyIndex < 0 || selectedKeyIndex >= keyConfig.keyConfigs.length) return
                                    var col = keyConfig.keyConfigs[selectedKeyIndex].rgb_color;
                                    var hsv = rgbToHsv((col >> 16) & 0xFF, (col >> 8) & 0xFF, col & 0xFF);
                                    pHue = hsv.h; pSat = hsv.s; pVal = hsv.v;
                                }
                                function applyColor() {
                                    var c = hsvToRgb(pHue, pSat, pVal);
                                    keyConfig.setKeyRgbColor(selectedKeyIndex, c.r, c.g, c.b);
                                    keyConfig.previewKeyRgb(selectedKeyIndex);
                                }
                                function updateSV(rect, mx, my) {
                                    pSat = clamp(mx / rect.width, 0, 1);
                                    pVal = clamp(1 - my / rect.height, 0, 1);
                                    applyColor();
                                }
                                function updateHue(rect, mx) {
                                    pHue = clamp(mx / rect.width * 360, 0, 360);
                                    applyColor();
                                }

                                ColumnLayout {
                                    id: keySettingsColumn
                                    width: parent.width
                                    spacing: 10

                                    RowLayout {
                                        Layout.fillWidth: true
                                        Text {
                                            text: {
                                                if (keySettingsPopup.selectedKeyIndex < 0) return ""
                                                if (keySettingsPopup.selectedKeyIndex === 3) return "KEY L1"
                                                if (keySettingsPopup.selectedKeyIndex === 5) return "KEY L2"
                                                if (keySettingsPopup.selectedKeyIndex === 6) return "KEY L3"
                                                return "KEY C" + (keySettingsPopup.selectedKeyIndex + 1)
                                            }
                                            font.pointSize: 16; font.bold: true; color: "#222"
                                        }
                                        Item { Layout.fillWidth: true }
                                        Rectangle { width: 28; height: 28; radius: 14; color: "#f0f0f0"
                                            Text { anchors.centerIn: parent; text: "×"; font.pointSize: 16; color: "#222" }
                                            MouseArea { anchors.fill: parent; onClicked: keySettingsPopup.close() }
                                        }
                                    }

                                    /* ===================== 09-07: L1/L2/L3 模式 + 三手势 ===================== */
                                    RowLayout {
                                        Layout.fillWidth: true
                                        visible: keySettingsPopup.selectedKeyIndex === 3 ||
                                                 keySettingsPopup.selectedKeyIndex === 5 ||
                                                 keySettingsPopup.selectedKeyIndex === 6
                                        spacing: 8

                                        Text {
                                            text: "模式"
                                            font.pointSize: 11
                                            font.bold: true
                                            color: "#555"
                                            verticalAlignment: Text.AlignVCenter
                                        }

                                        ComboBox {
                                            id: lModeCombo
                                            Layout.preferredWidth: 116
                                            Layout.preferredHeight: 26
                                            focusPolicy: Qt.NoFocus
                                            font.pointSize: 10
                                            model: [
                                                { label: "常规模式", v: 0 },
                                                { label: "手势模式", v: 1 }
                                            ]
                                            textRole: "label"
                                            onActivated: {
                                                var ki = keySettingsPopup.selectedKeyIndex
                                                if (ki !== 3 && ki !== 5 && ki !== 6) return
                                                var which = keySettingsPopup.lWhichNow()
                                                var v = model[currentIndex].v
                                                /* 模式【显式落盘】—— 选什么就是什么, 固件不再看
                                                 * 手势是否设置(常规模式手势配置存而不用)。
                                                 * 切换【不清空】手势内容: 切回手势模式原样恢复。 */
                                                keyConfig.setLKeyMode(which, v)
                                                keySettingsPopup.lModeIndex = v
                                                if (v === 0) {
                                                    /* 常规模式: 取消该键进行中的手势捕获; 编辑回落单击 */
                                                    var li = keySettingsPopup.lLiNow()
                                                    var ci = keyConfig.captureIndex
                                                    if (li >= 0 && ci >= 19 + li * 3 && ci <= 19 + li * 3 + 2)
                                                        keyConfig.cancelCapture()
                                                    keySettingsPopup.lGestureState = 0
                                                }
                                                refreshKeyTick++
                                                keySettingsPopup.syncLModeUi()
                                                keySettingsPopup.syncLGCombo()
                                            }
                                            background: Rectangle {
                                                implicitHeight: 26
                                                radius: 7
                                                color: "white"
                                                border.color: lModeCombo.hovered || lModeCombo.popup.visible ? "#07c160" : "#d0e0d8"
                                                border.width: 1
                                            }
                                            contentItem: Text {
                                                leftPadding: 8; rightPadding: 20
                                                text: lModeCombo.displayText
                                                font: lModeCombo.font
                                                color: "#333"
                                                verticalAlignment: Text.AlignVCenter
                                                elide: Text.ElideRight
                                            }
                                            indicator: Canvas {
                                                x: lModeCombo.width - width - 8
                                                y: lModeCombo.height / 2 - height / 2
                                                width: 9; height: 5
                                                contextType: "2d"
                                                onPaint: {
                                                    var ctx = getContext("2d")
                                                    ctx.reset()
                                                    ctx.moveTo(0, 0); ctx.lineTo(width, 0); ctx.lineTo(width / 2, height)
                                                    ctx.closePath(); ctx.fillStyle = "#07c160"; ctx.fill()
                                                }
                                            }
                                            popup: Popup {
                                                y: lModeCombo.height + 4
                                                width: lModeCombo.width
                                                padding: 3
                                                implicitHeight: 26 * lModeCombo.model.length + 6
                                                background: Rectangle { radius: 8; color: "white"; border.color: "#e0e0e0"; border.width: 1 }
                                                contentItem: ListView {
                                                    clip: true
                                                    implicitHeight: 26 * lModeCombo.model.length
                                                    model: lModeCombo.delegateModel
                                                    currentIndex: lModeCombo.highlightedIndex
                                                    delegate: ItemDelegate {
                                                        width: ListView.view.width
                                                        height: 26
                                                        highlighted: ListView.isCurrentItem
                                                        hoverEnabled: true
                                                        onClicked: lModeCombo.activated(index)
                                                        contentItem: Text {
                                                            text: lModeCombo.model[index].label
                                                            color: parent.highlighted ? "#07c160" : "#333"
                                                            font: lModeCombo.font
                                                            verticalAlignment: Text.AlignVCenter
                                                            elide: Text.ElideRight
                                                            leftPadding: 6
                                                        }
                                                        background: Rectangle {
                                                            color: parent.highlighted ? "#e6f5ec" : (parent.hovered ? "#f4f9f6" : "transparent")
                                                            radius: 6
                                                        }
                                                    }
                                                }
                                            }
                                        }

                                        Text {
                                            Layout.fillWidth: true
                                            horizontalAlignment: Text.AlignRight
                                            verticalAlignment: Text.AlignVCenter
                                            elide: Text.ElideRight
                                            font.pointSize: 9
                                            color: "#999"
                                            text: keySettingsPopup.lModeIndex === 1 ? "可配置单击/双击/长按" : "按下即发/松开即发"
                                        }
                                    }

                                    /* 09-06: 两级选择(与 EC 编码器弹窗同一设计) ——
                                     * 大类(鼠标/键盘组合/多媒体/无动作) + 鼠标类细分触发。
                                     * L1 细分含"空中鼠标", L2/L3 不含。 */
                                    RowLayout {
                                        spacing: 10
                                        Layout.fillWidth: true
                                        visible: (keySettingsPopup.selectedKeyIndex === 3 ||
                                                  keySettingsPopup.selectedKeyIndex === 5 ||
                                                  keySettingsPopup.selectedKeyIndex === 6) &&
                                                 keySettingsPopup.lModeIndex === 0   /* 09-07: 手势模式隐藏主键动作行(手势区编辑) */
                                        Text { text: "动作"; font.pointSize: 12; font.bold: true; color: "#333" }

                                        /* 大类 */
                                        ComboBox {
                                            id: lTypeCombo
                                            Layout.preferredWidth: 112
                                            Layout.preferredHeight: 30
                                            focusPolicy: Qt.NoFocus
                                            font.pointSize: 10
                                            /* 细分选项集: L1 含空中鼠标 */
                                            property var l1TriggerModel: [
                                                { label: "空中鼠标", v: "airmouse" },
                                                { label: "滚轮上",   v: "wheel_up" },
                                                { label: "滚轮下",   v: "wheel_down" },
                                                { label: "左键",     v: "mouse_left" },
                                                { label: "右键",     v: "mouse_right" },
                                                { label: "中键",     v: "mouse_middle" }
                                            ]
                                            property var cTriggerModel: [
                                                { label: "左键",     v: "mouse_left" },
                                                { label: "右键",     v: "mouse_right" },
                                                { label: "中键",     v: "mouse_middle" },
                                                { label: "滚轮上",   v: "wheel_up" },
                                                { label: "滚轮下",   v: "wheel_down" }
                                            ]
                                            model: [
                                                { label: "鼠标",     v: "mouse" },
                                                { label: "键盘组合", v: "keyboard" },
                                                { label: "多媒体",   v: "multimedia" },
                                                { label: "无动作",   v: "none" }
                                            ]
                                            textRole: "label"
                                            onActivated: {
                                                var ki = keySettingsPopup.selectedKeyIndex
                                                if (ki !== 3 && ki !== 5 && ki !== 6) return
                                                var which = (ki === 3) ? 2 : ki - 5
                                                if (model[currentIndex].v === "mouse") {
                                                    /* 保持鼠标族现值, 否则回默认(L1=空中鼠标, L2/L3=左键) */
                                                    var av = lKeyActionValue(which)
                                                    var isM = (av === "wheel_up" || av === "wheel_down" ||
                                                               av === "mouse_left" || av === "mouse_right" ||
                                                               av === "mouse_middle" || av === "airmouse")
                                                    if (!isM) av = (ki === 3) ? "airmouse" : "mouse_left"
                                                    keyConfig.setLKeyAction(which, av)
                                                    syncLKeyCombo()
                                                } else {
                                                    keyConfig.setLKeyAction(which, model[currentIndex].v)
                                                }
                                                refreshKeyTick++
                                            }
                                            background: Rectangle {
                                                implicitWidth: 112; implicitHeight: 30; radius: 8; color: "white"
                                                border.color: lTypeCombo.hovered || lTypeCombo.popup.visible ? "#07c160" : "#d0e0d8"
                                                border.width: 1
                                            }
                                            contentItem: Text {
                                                leftPadding: 8; rightPadding: 22
                                                text: lTypeCombo.displayText; font: lTypeCombo.font
                                                color: "#333"; verticalAlignment: Text.AlignVCenter; elide: Text.ElideRight
                                            }
                                            indicator: Canvas {
                                                x: lTypeCombo.width - width - 9
                                                y: lTypeCombo.height / 2 - height / 2
                                                width: 9; height: 5
                                                contextType: "2d"
                                                onPaint: {
                                                    var ctx = getContext("2d")
                                                    ctx.reset()
                                                    ctx.moveTo(0, 0); ctx.lineTo(width, 0); ctx.lineTo(width / 2, height)
                                                    ctx.closePath(); ctx.fillStyle = "#07c160"; ctx.fill()
                                                }
                                            }
                                            popup: Popup {
                                                y: lTypeCombo.height + 4
                                                width: lTypeCombo.width
                                                padding: 3
                                                implicitHeight: 30 * lTypeCombo.model.length + 6
                                                background: Rectangle { radius: 8; color: "white"; border.color: "#e0e0e0"; border.width: 1 }
                                                contentItem: ListView {
                                                    clip: true
                                                    implicitHeight: 30 * lTypeCombo.model.length
                                                    model: lTypeCombo.delegateModel
                                                    currentIndex: lTypeCombo.highlightedIndex
                                                    delegate: ItemDelegate {
                                                        width: ListView.view.width
                                                        height: 30
                                                        highlighted: ListView.isCurrentItem
                                                        hoverEnabled: true
                                                        onClicked: lTypeCombo.activated(index)
                                                        contentItem: Text {
                                                            text: lTypeCombo.model[index].label
                                                            color: parent.highlighted ? "#07c160" : "#333"
                                                            font: lTypeCombo.font
                                                            verticalAlignment: Text.AlignVCenter
                                                            elide: Text.ElideRight
                                                            leftPadding: 6
                                                        }
                                                        background: Rectangle {
                                                            color: parent.highlighted ? "#e6f5ec" : (parent.hovered ? "#f4f9f6" : "transparent")
                                                            radius: 6
                                                        }
                                                    }
                                                }
                                            }
                                        }

                                        /* 鼠标类细分触发(仅大类=鼠标时显示) */
                                        ComboBox {
                                            id: lTriggerCombo
                                            Layout.preferredWidth: 112
                                            Layout.preferredHeight: 30
                                            focusPolicy: Qt.NoFocus
                                            font.pointSize: 10
                                            model: lTypeCombo.cTriggerModel   /* 09-07 修: 初始 model 防 popup 读 undefined.length 崩溃 */
                                            visible: lTypeCombo.currentIndex === 0
                                            textRole: "label"
                                            onActivated: {
                                                var ki = keySettingsPopup.selectedKeyIndex
                                                if (ki !== 3 && ki !== 5 && ki !== 6) return
                                                var which = (ki === 3) ? 2 : ki - 5
                                                keyConfig.setLKeyAction(which, model[currentIndex].v)
                                                refreshKeyTick++
                                            }
                                            background: Rectangle {
                                                implicitWidth: 112; implicitHeight: 30; radius: 8; color: "white"
                                                border.color: lTriggerCombo.hovered || lTriggerCombo.popup.visible ? "#07c160" : "#d0e0d8"
                                                border.width: 1
                                            }
                                            contentItem: Text {
                                                leftPadding: 8; rightPadding: 22
                                                text: lTriggerCombo.displayText; font: lTriggerCombo.font
                                                color: "#333"; verticalAlignment: Text.AlignVCenter; elide: Text.ElideRight
                                            }
                                            indicator: Canvas {
                                                x: lTriggerCombo.width - width - 9
                                                y: lTriggerCombo.height / 2 - height / 2
                                                width: 9; height: 5
                                                contextType: "2d"
                                                onPaint: {
                                                    var ctx = getContext("2d")
                                                    ctx.reset()
                                                    ctx.moveTo(0, 0); ctx.lineTo(width, 0); ctx.lineTo(width / 2, height)
                                                    ctx.closePath(); ctx.fillStyle = "#07c160"; ctx.fill()
                                                }
                                            }
                                            popup: Popup {
                                                y: lTriggerCombo.height + 4
                                                width: lTriggerCombo.width
                                                padding: 3
                                                implicitHeight: 30 * lTriggerCombo.model.length + 6
                                                background: Rectangle { radius: 8; color: "white"; border.color: "#e0e0e0"; border.width: 1 }
                                                contentItem: ListView {
                                                    clip: true
                                                    implicitHeight: 30 * lTriggerCombo.model.length
                                                    model: lTriggerCombo.delegateModel
                                                    currentIndex: lTriggerCombo.highlightedIndex
                                                    delegate: ItemDelegate {
                                                        width: ListView.view.width
                                                        height: 30
                                                        highlighted: ListView.isCurrentItem
                                                        hoverEnabled: true
                                                        onClicked: lTriggerCombo.activated(index)
                                                        contentItem: Text {
                                                            text: lTriggerCombo.model[index].label
                                                            color: parent.highlighted ? "#07c160" : "#333"
                                                            font: lTriggerCombo.font
                                                            verticalAlignment: Text.AlignVCenter
                                                            elide: Text.ElideRight
                                                            leftPadding: 6
                                                        }
                                                        background: Rectangle {
                                                            color: parent.highlighted ? "#e6f5ec" : (parent.hovered ? "#f4f9f6" : "transparent")
                                                            radius: 6
                                                        }
                                                    }
                                                }
                                            }
                                        }
                                        /* 09-06: 键盘组合 —— 行内快捷键框(点击开始/取消捕获, 与 EC 行同款) */
                                        Rectangle {
                                            id: lKbBox
                                            Layout.preferredWidth: 112
                                            Layout.preferredHeight: 30
                                            visible: lTypeCombo.currentIndex === 1
                                            radius: 8
                                            color: keyConfig.captureIndex === keySettingsPopup.selectedKeyIndex ? "#e6f5ec" : "white"
                                            border.color: keyConfig.captureIndex === keySettingsPopup.selectedKeyIndex ? "#07c160" : "#d0e0d8"
                                            border.width: 1

                                            Text {
                                                anchors.centerIn: parent
                                                width: parent.width - 12
                                                text: {
                                                    var _ = refreshKeyTick
                                                    if (keyConfig.captureIndex === keySettingsPopup.selectedKeyIndex) return "捕获中…"
                                                    var m = lKeyMap(keySettingsPopup.selectedKeyIndex === 3 ? 2 : keySettingsPopup.selectedKeyIndex - 5)
                                                    if (m.action !== "keyboard") return "未设置"
                                                    var t = formatShortcut(m.modifier, m.keycode)
                                                    return t.length > 0 ? t : "未设置"
                                                }
                                                font.pointSize: 10
                                                color: keyConfig.captureIndex === keySettingsPopup.selectedKeyIndex ? "#07c160" : "#333"
                                                horizontalAlignment: Text.AlignHCenter
                                                elide: Text.ElideRight
                                            }
                                            MouseArea {
                                                anchors.fill: parent
                                                onClicked: {
                                                    if (keyConfig.captureIndex === keySettingsPopup.selectedKeyIndex) keyConfig.cancelCapture()
                                                    else keyConfig.startCapture(keySettingsPopup.selectedKeyIndex)
                                                }
                                            }
                                        }

                                        /* 09-06: 多媒体 —— 细分下拉选择具体多媒体键码 */
                                        ComboBox {
                                            id: lMmCombo
                                            Layout.preferredWidth: 112
                                            Layout.preferredHeight: 30
                                            focusPolicy: Qt.NoFocus
                                            font.pointSize: 10
                                            visible: lTypeCombo.currentIndex === 2
                                            model: ecSettingsPopup.mmModel
                                            textRole: "label"
                                            onActivated: {
                                                var ki = keySettingsPopup.selectedKeyIndex
                                                if (ki !== 3 && ki !== 5 && ki !== 6) return
                                                keyConfig.setLKeyMultimedia((ki === 3) ? 2 : ki - 5, model[currentIndex].v)
                                                refreshKeyTick++
                                            }
                                            background: Rectangle {
                                                implicitWidth: 112; implicitHeight: 30; radius: 8; color: "white"
                                                border.color: lMmCombo.hovered || lMmCombo.popup.visible ? "#07c160" : "#d0e0d8"
                                                border.width: 1
                                            }
                                            contentItem: Text {
                                                leftPadding: 8; rightPadding: 22
                                                text: lMmCombo.displayText; font: lMmCombo.font
                                                color: "#333"; verticalAlignment: Text.AlignVCenter; elide: Text.ElideRight
                                            }
                                                indicator: Canvas {
                                                    x: lMmCombo.width - width - 9
                                                    y: lMmCombo.height / 2 - height / 2
                                                    width: 9; height: 5
                                                    contextType: "2d"
                                                    onPaint: {
                                                        var ctx = getContext("2d")
                                                        ctx.reset()
                                                        ctx.moveTo(0, 0); ctx.lineTo(width, 0); ctx.lineTo(width / 2, height)
                                                        ctx.closePath(); ctx.fillStyle = "#07c160"; ctx.fill()
                                                    }
                                                }
                                                popup: Popup {
                                                    y: lMmCombo.height + 4
                                                    width: lMmCombo.width
                                                    padding: 3
                                                    implicitHeight: 30 * lMmCombo.model.length + 6
                                                    background: Rectangle { radius: 8; color: "white"; border.color: "#e0e0e0"; border.width: 1 }
                                                    contentItem: ListView {
                                                        clip: true
                                                        implicitHeight: 30 * lMmCombo.model.length
                                                        model: lMmCombo.delegateModel
                                                        currentIndex: lMmCombo.highlightedIndex
                                                        delegate: ItemDelegate {
                                                            width: ListView.view.width
                                                            height: 30
                                                            highlighted: ListView.isCurrentItem
                                                            hoverEnabled: true
                                                            onClicked: lMmCombo.activated(index)
                                                            contentItem: Text {
                                                                text: lMmCombo.model[index].label
                                                                color: parent.highlighted ? "#07c160" : "#333"
                                                                font: lMmCombo.font
                                                                verticalAlignment: Text.AlignVCenter
                                                                elide: Text.ElideRight
                                                                leftPadding: 6
                                                            }
                                                            background: Rectangle {
                                                                color: parent.highlighted ? "#e6f5ec" : (parent.hovered ? "#f4f9f6" : "transparent")
                                                                radius: 6
                                                            }
                                                        }
                                                    }
                                                }
                                                }


                                        Item { Layout.fillWidth: true }
                                    }

                                    /* ---- 09-07: L 键三手势卡片盒(仅"手势模式"显示; 常规模式
                                     * 隐藏整个盒 —— 单击即主键直通, 无手势语义) ---- */
                                    Rectangle {
                                        Layout.fillWidth: true; height: 104; radius: 8; color: "#f7f7f7"
                                        visible: (keySettingsPopup.selectedKeyIndex === 3 ||
                                                  keySettingsPopup.selectedKeyIndex === 5 ||
                                                  keySettingsPopup.selectedKeyIndex === 6) &&
                                                 keySettingsPopup.lModeIndex === 1
                                        ColumnLayout {
                                            anchors.fill: parent; anchors.margins: 10
                                            spacing: 4
                                            RowLayout {
                                                spacing: 8
                                                Layout.fillWidth: true
                                                property var gestureDefs: [
                                                    { title: "单击", g: 0 },
                                                    { title: "双击", g: 1 },
                                                    { title: "长按", g: 2 }
                                                ]
                                                Repeater {
                                                    model: parent.gestureDefs
                                                    delegate: Rectangle {
                                                        Layout.fillWidth: true
                                                        height: 52
                                                        radius: 6
                                                        color: keySettingsPopup.lGestureState === modelData.g ? "#e6f5ec" : "white"
                                                        border.color: keySettingsPopup.lGestureState === modelData.g ? "#07c160" : "#d5e0da"
                                                        border.width: 1
                                                        ColumnLayout {
                                                            anchors.fill: parent; anchors.margins: 4
                                                            spacing: 2
                                                            Text {
                                                                text: modelData.title
                                                                font.pointSize: 10; font.bold: true
                                                                color: keySettingsPopup.lGestureState === modelData.g ? "#07c160" : "#666"
                                                                Layout.alignment: Qt.AlignHCenter
                                                            }
                                                            Text {
                                                                text: {
                                                                    var _ = refreshKeyTick
                                                                    var which = keySettingsPopup.lWhichNow()
                                                                    return lKeyGestureStatusText(which, modelData.g)
                                                                }
                                                                font.pointSize: 8
                                                                color: "#333"
                                                                Layout.fillWidth: true
                                                                horizontalAlignment: Text.AlignHCenter
                                                                elide: Text.ElideRight
                                                            }
                                                        }
                                                        MouseArea {
                                                            anchors.fill: parent
                                                            onClicked: {
                                                                keySettingsPopup.lGestureState = modelData.g
                                                                keySettingsPopup.syncLGCombo()
                                                            }
                                                        }
                                                    }
                                                }
                                            }
                                            Text {
                                                Layout.fillWidth: true
                                                font.pointSize: 8
                                                color: "#aaa"
                                                elide: Text.ElideRight
                                                text: "点卡片选手势 · 单击/双击(窗 220ms)/长按(800ms) · L1 手势可设空中鼠标"
                                            }
                                        }
                                    }

                                    /* ---- 09-07: L 手势配置行(动作大类 + 鼠标细分/组合键/多媒体 + 捕获) ---- */
                                    RowLayout {
                                        Layout.fillWidth: true
                                        visible: (keySettingsPopup.selectedKeyIndex === 3 ||
                                                  keySettingsPopup.selectedKeyIndex === 5 ||
                                                  keySettingsPopup.selectedKeyIndex === 6) &&
                                                 keySettingsPopup.lModeIndex === 1
                                        spacing: 6

                                        ComboBox {
                                            id: lGTypeCombo
                                            /* 大类选项: L1 额外含"空中鼠标"(用户要求); L2/L3 不含 */
                                            property var lgTypeL1: [
                                                { label: "鼠标", v: "mouse" },
                                                { label: "键盘组合", v: "keyboard" },
                                                { label: "多媒体", v: "multimedia" },
                                                { label: "无动作", v: "none" },
                                                { label: "空中鼠标", v: "airmouse" }
                                            ]
                                            property var lgTypePlain: [
                                                { label: "鼠标", v: "mouse" },
                                                { label: "键盘组合", v: "keyboard" },
                                                { label: "多媒体", v: "multimedia" },
                                                { label: "无动作", v: "none" }
                                            ]
                                            Layout.preferredWidth: 100
                                            Layout.preferredHeight: 30
                                            focusPolicy: Qt.NoFocus
                                            font.pointSize: 10
                                            model: lgTypePlain
                                            textRole: "label"
                                            onActivated: {
                                                var which = keySettingsPopup.lWhichNow()
                                                var g = keySettingsPopup.lGestureState
                                                var v = model[currentIndex].v
                                                if (v === "mouse") {
                                                    /* 保持鼠标族现值(键码 1..6), 否则默认左键 */
                                                    var cur = lKeyGestureActionValue(which, g)
                                                    var curIsMouse = (cur === "mouse_left" || cur === "mouse_right" ||
                                                                      cur === "mouse_middle" || cur === "wheel_up" ||
                                                                      cur === "wheel_down")
                                                    keyConfig.setLKeyGestureAction(which, g, "mouse")
                                                    if (curIsMouse) {
                                                        var kc = cur === "mouse_left" ? 1 : cur === "mouse_right" ? 2
                                                                : cur === "mouse_middle" ? 4 : cur === "wheel_up" ? 5 : 6
                                                        keyConfig.setLKeyGestureKeycode(which, g, kc)
                                                    }
                                                } else {
                                                    keyConfig.setLKeyGestureAction(which, g, v)
                                                }
                                                refreshKeyTick++
                                                keySettingsPopup.syncLGCombo()
                                            }
                                            background: Rectangle {
                                                implicitHeight: 30
                                                radius: 8
                                                color: "white"
                                                border.color: lGTypeCombo.hovered || lGTypeCombo.popup.visible ? "#07c160" : "#d0e0d8"
                                                border.width: 1
                                            }
                                            contentItem: Text {
                                                leftPadding: 8; rightPadding: 22
                                                text: lGTypeCombo.displayText; font: lGTypeCombo.font
                                                color: "#333"; verticalAlignment: Text.AlignVCenter; elide: Text.ElideRight
                                            }
                                            indicator: Canvas {
                                                x: lGTypeCombo.width - width - 9
                                                y: lGTypeCombo.height / 2 - height / 2
                                                width: 9; height: 5
                                                contextType: "2d"
                                                onPaint: {
                                                    var ctx = getContext("2d")
                                                    ctx.reset()
                                                    ctx.moveTo(0, 0); ctx.lineTo(width, 0); ctx.lineTo(width / 2, height)
                                                    ctx.closePath(); ctx.fillStyle = "#07c160"; ctx.fill()
                                                }
                                            }
                                            popup: Popup {
                                                y: lGTypeCombo.height + 4
                                                width: lGTypeCombo.width
                                                padding: 3
                                                implicitHeight: Math.min(30 * lGTypeCombo.model.length + 6, 240)
                                                background: Rectangle { radius: 8; color: "white"; border.color: "#e0e0e0"; border.width: 1 }
                                                contentItem: ListView {
                                                    clip: true
                                                    implicitHeight: Math.min(30 * lGTypeCombo.model.length, 234)
                                                    model: lGTypeCombo.delegateModel
                                                    currentIndex: lGTypeCombo.highlightedIndex
                                                    delegate: ItemDelegate {
                                                        width: ListView.view.width
                                                        height: 30
                                                        highlighted: ListView.isCurrentItem
                                                        hoverEnabled: true
                                                        onClicked: lGTypeCombo.activated(index)
                                                        contentItem: Text {
                                                            text: lGTypeCombo.model[index].label
                                                            color: parent.highlighted ? "#07c160" : "#333"
                                                            font: lGTypeCombo.font
                                                            verticalAlignment: Text.AlignVCenter
                                                            elide: Text.ElideRight
                                                            leftPadding: 6
                                                        }
                                                        background: Rectangle {
                                                            color: parent.highlighted ? "#e6f5ec" : (parent.hovered ? "#f4f9f6" : "transparent")
                                                            radius: 6
                                                        }
                                                    }
                                                }
                                            }
                                        }

                                        /* 鼠标细分(左/右/中/滚轮上下) */
                                        ComboBox {
                                            id: lGTriggerCombo
                                            Layout.preferredWidth: 100
                                            Layout.preferredHeight: 30
                                            focusPolicy: Qt.NoFocus
                                            font.pointSize: 10
                                            model: lTypeCombo.cTriggerModel
                                            visible: lGTypeCombo.currentIndex === 0
                                            textRole: "label"
                                            onActivated: {
                                                var which = keySettingsPopup.lWhichNow()
                                                var g = keySettingsPopup.lGestureState
                                                var v = model[currentIndex].v
                                                var kc = v === "mouse_left" ? 1 : v === "mouse_right" ? 2
                                                        : v === "mouse_middle" ? 4 : v === "wheel_up" ? 5 : 6
                                                keyConfig.setLKeyGestureKeycode(which, g, kc)
                                                refreshKeyTick++
                                                keySettingsPopup.syncLGCombo()
                                            }
                                            background: Rectangle {
                                                implicitHeight: 30
                                                radius: 8
                                                color: "white"
                                                border.color: lGTriggerCombo.hovered || lGTriggerCombo.popup.visible ? "#07c160" : "#d0e0d8"
                                                border.width: 1
                                            }
                                            contentItem: Text {
                                                leftPadding: 8; rightPadding: 22
                                                text: lGTriggerCombo.displayText; font: lGTriggerCombo.font
                                                color: "#333"; verticalAlignment: Text.AlignVCenter; elide: Text.ElideRight
                                            }
                                            indicator: Canvas {
                                                x: lGTriggerCombo.width - width - 9
                                                y: lGTriggerCombo.height / 2 - height / 2
                                                width: 9; height: 5
                                                contextType: "2d"
                                                onPaint: {
                                                    var ctx = getContext("2d")
                                                    ctx.reset()
                                                    ctx.moveTo(0, 0); ctx.lineTo(width, 0); ctx.lineTo(width / 2, height)
                                                    ctx.closePath(); ctx.fillStyle = "#07c160"; ctx.fill()
                                                }
                                            }
                                            popup: Popup {
                                                y: lGTriggerCombo.height + 4
                                                width: lGTriggerCombo.width
                                                padding: 3
                                                implicitHeight: Math.min(30 * lGTriggerCombo.model.length + 6, 240)
                                                background: Rectangle { radius: 8; color: "white"; border.color: "#e0e0e0"; border.width: 1 }
                                                contentItem: ListView {
                                                    clip: true
                                                    implicitHeight: Math.min(30 * lGTriggerCombo.model.length, 234)
                                                    model: lGTriggerCombo.delegateModel
                                                    currentIndex: lGTriggerCombo.highlightedIndex
                                                    delegate: ItemDelegate {
                                                        width: ListView.view.width
                                                        height: 30
                                                        highlighted: ListView.isCurrentItem
                                                        hoverEnabled: true
                                                        onClicked: lGTriggerCombo.activated(index)
                                                        contentItem: Text {
                                                            text: lGTriggerCombo.model[index].label
                                                            color: parent.highlighted ? "#07c160" : "#333"
                                                            font: lGTriggerCombo.font
                                                            verticalAlignment: Text.AlignVCenter
                                                            elide: Text.ElideRight
                                                            leftPadding: 6
                                                        }
                                                        background: Rectangle {
                                                            color: parent.highlighted ? "#e6f5ec" : (parent.hovered ? "#f4f9f6" : "transparent")
                                                            radius: 6
                                                        }
                                                    }
                                                }
                                            }
                                        }

                                        /* 键盘组合: 点击框进入捕获(与 EC 弹窗同款), 捕获中边框变绿+显示"捕获中…" */
                                        Rectangle {
                                            id: lGKbBox
                                            Layout.fillWidth: true
                                            Layout.preferredHeight: 30
                                            radius: 8
                                            color: "white"
                                            property bool hovered: false
                                            property int capTarget: {
                                                var li = keySettingsPopup.lLiNow()
                                                var g = keySettingsPopup.lGestureState
                                                return 19 + li * 3 + g
                                            }
                                            border.color: (keyConfig.captureIndex === lGKbBox.capTarget || lGKbBox.hovered) ? "#07c160" : "#d0e0d8"
                                            border.width: 1
                                            visible: lGTypeCombo.currentIndex === 1
                                            Text {
                                                anchors.fill: parent
                                                anchors.leftMargin: 6
                                                anchors.rightMargin: 6
                                                verticalAlignment: Text.AlignVCenter
                                                horizontalAlignment: Text.AlignHCenter
                                                elide: Text.ElideMiddle
                                                font.pointSize: 10
                                                color: keyConfig.captureIndex === lGKbBox.capTarget ? "#07c160" : "#333"
                                                text: {
                                                    var _ = refreshKeyTick
                                                    if (keyConfig.captureIndex === lGKbBox.capTarget) return "捕获中…"
                                                    var which = keySettingsPopup.lWhichNow()
                                                    var g = keySettingsPopup.lGestureState
                                                    return lKeyGestureStatusText(which, g)
                                                }
                                            }
                                            MouseArea {
                                                anchors.fill: parent
                                                hoverEnabled: true
                                                onEntered: lGKbBox.hovered = true
                                                onExited: lGKbBox.hovered = false
                                                onClicked: {
                                                    var which = keySettingsPopup.lWhichNow()
                                                    var g = keySettingsPopup.lGestureState
                                                    if (keyConfig.captureIndex === lGKbBox.capTarget) keyConfig.cancelCapture()
                                                    else keyConfig.startLKeyGestureCapture(which, g)
                                                }
                                            }
                                        }

                                        /* 多媒体细分 */
                                        ComboBox {
                                            id: lGMmCombo
                                            Layout.preferredWidth: 100
                                            Layout.preferredHeight: 30
                                            focusPolicy: Qt.NoFocus
                                            font.pointSize: 10
                                            model: ecSettingsPopup.mmModel
                                            textRole: "label"
                                            visible: lGTypeCombo.currentIndex === 2
                                            onActivated: {
                                                var which = keySettingsPopup.lWhichNow()
                                                var g = keySettingsPopup.lGestureState
                                                keyConfig.setLKeyGestureAction(which, g, "multimedia")
                                                keyConfig.setLKeyGestureKeycode(which, g, model[currentIndex].v)
                                                refreshKeyTick++
                                                keySettingsPopup.syncLGCombo()
                                            }
                                            background: Rectangle {
                                                implicitHeight: 30
                                                radius: 8
                                                color: "white"
                                                border.color: lGMmCombo.hovered || lGMmCombo.popup.visible ? "#07c160" : "#d0e0d8"
                                                border.width: 1
                                            }
                                            contentItem: Text {
                                                leftPadding: 8; rightPadding: 22
                                                text: lGMmCombo.displayText; font: lGMmCombo.font
                                                color: "#333"; verticalAlignment: Text.AlignVCenter; elide: Text.ElideRight
                                            }
                                            indicator: Canvas {
                                                x: lGMmCombo.width - width - 9
                                                y: lGMmCombo.height / 2 - height / 2
                                                width: 9; height: 5
                                                contextType: "2d"
                                                onPaint: {
                                                    var ctx = getContext("2d")
                                                    ctx.reset()
                                                    ctx.moveTo(0, 0); ctx.lineTo(width, 0); ctx.lineTo(width / 2, height)
                                                    ctx.closePath(); ctx.fillStyle = "#07c160"; ctx.fill()
                                                }
                                            }
                                            popup: Popup {
                                                y: lGMmCombo.height + 4
                                                width: lGMmCombo.width
                                                padding: 3
                                                implicitHeight: Math.min(30 * lGMmCombo.model.length + 6, 240)
                                                background: Rectangle { radius: 8; color: "white"; border.color: "#e0e0e0"; border.width: 1 }
                                                contentItem: ListView {
                                                    clip: true
                                                    implicitHeight: Math.min(30 * lGMmCombo.model.length, 234)
                                                    model: lGMmCombo.delegateModel
                                                    currentIndex: lGMmCombo.highlightedIndex
                                                    delegate: ItemDelegate {
                                                        width: ListView.view.width
                                                        height: 30
                                                        highlighted: ListView.isCurrentItem
                                                        hoverEnabled: true
                                                        onClicked: lGMmCombo.activated(index)
                                                        contentItem: Text {
                                                            text: lGMmCombo.model[index].label
                                                            color: parent.highlighted ? "#07c160" : "#333"
                                                            font: lGMmCombo.font
                                                            verticalAlignment: Text.AlignVCenter
                                                            elide: Text.ElideRight
                                                            leftPadding: 6
                                                        }
                                                        background: Rectangle {
                                                            color: parent.highlighted ? "#e6f5ec" : (parent.hovered ? "#f4f9f6" : "transparent")
                                                            radius: 6
                                                        }
                                                    }
                                                }
                                            }
                                        }
                                        /* 空中鼠标(L1)提示 */
                                        Text {
                                            visible: lGTypeCombo.currentIndex === 4
                                            font.pointSize: 9
                                            color: "#07c160"
                                            text: "触发一次即切换空中鼠标开关"
                                        }
                                    }


                                            /* ---- C 键模式行 ---- */
                                            RowLayout {
                                                Layout.fillWidth: true
                                                visible: keySettingsPopup.selectedKeyIndex >= 0 && keySettingsPopup.selectedKeyIndex < 3
                                                spacing: 8

                                                Text {
                                                    text: "模式"
                                                    font.pointSize: 11
                                                    font.bold: true
                                                    color: "#555"
                                                    verticalAlignment: Text.AlignVCenter
                                                }

                                                ComboBox {
                                                    id: cModeCombo
                                                    Layout.preferredWidth: 116
                                                    Layout.preferredHeight: 26
                                                    focusPolicy: Qt.NoFocus
                                                    font.pointSize: 10
                                                    model: [
                                                        { label: "常规模式", v: 0 },
                                                        { label: "手势模式", v: 1 }
                                                    ]
                                                    textRole: "label"
                                                    onActivated: {
                                                        var ki = keySettingsPopup.selectedKeyIndex
                                                        if (ki < 0 || ki >= 3) return
                                                        var v = model[currentIndex].v
                                                        /* 09-07: 模式【显式落盘】—— 选什么就是什么, 固件不再
                                                         * 看双击/长按是否设置(常规模式下手势配置存而不用)。
                                                         * 因此切换【不清空】双击/长按内容: 切回手势模式原样恢复。 */
                                                        keyConfig.setKeyMode(ki, v)
                                                        keySettingsPopup.cKeyModeIndex = v
                                                        if (v === 0) {
                                                            /* 常规模式: 取消进行中的双击/长按捕获;
                                                             * 若正编辑双击/长按则回落到单击(其卡片已锁定) */
                                                            if (keyConfig.captureIndex === 10 + ki * 3 + 1 ||
                                                                keyConfig.captureIndex === 10 + ki * 3 + 2)
                                                                keyConfig.cancelCapture()
                                                            keySettingsPopup.cGestureState = 0
                                                        }
                                                        refreshKeyTick++
                                                        keySettingsPopup.syncCKeyCombo()
                                                        keySettingsPopup.syncCKeyMode()
                                                    }
                                                    background: Rectangle {
                                                        implicitHeight: 26
                                                        radius: 7
                                                        color: "white"
                                                        border.color: cModeCombo.hovered || cModeCombo.popup.visible ? "#07c160" : "#d0e0d8"
                                                        border.width: 1
                                                    }
                                                    contentItem: Text {
                                                        leftPadding: 8; rightPadding: 20
                                                        text: cModeCombo.displayText
                                                        font: cModeCombo.font
                                                        color: "#333"
                                                        verticalAlignment: Text.AlignVCenter
                                                        elide: Text.ElideRight
                                                    }
                                                    indicator: Canvas {
                                                        x: cModeCombo.width - width - 8
                                                        y: cModeCombo.height / 2 - height / 2
                                                        width: 9; height: 5
                                                        contextType: "2d"
                                                        onPaint: {
                                                            var ctx = getContext("2d")
                                                            ctx.reset()
                                                            ctx.moveTo(0, 0); ctx.lineTo(width, 0); ctx.lineTo(width / 2, height)
                                                            ctx.closePath(); ctx.fillStyle = "#07c160"; ctx.fill()
                                                        }
                                                    }
                                                    popup: Popup {
                                                        y: cModeCombo.height + 4
                                                        width: cModeCombo.width
                                                        padding: 3
                                                        implicitHeight: 26 * cModeCombo.model.length + 6
                                                        background: Rectangle { radius: 8; color: "white"; border.color: "#e0e0e0"; border.width: 1 }
                                                        contentItem: ListView {
                                                            clip: true
                                                            implicitHeight: 26 * cModeCombo.model.length
                                                            model: cModeCombo.delegateModel
                                                            currentIndex: cModeCombo.highlightedIndex
                                                            delegate: ItemDelegate {
                                                                width: ListView.view.width
                                                                height: 26
                                                                highlighted: ListView.isCurrentItem
                                                                hoverEnabled: true
                                                                onClicked: cModeCombo.activated(index)
                                                                contentItem: Text {
                                                                    text: cModeCombo.model[index].label
                                                                    color: parent.highlighted ? "#07c160" : "#333"
                                                                    font: cModeCombo.font
                                                                    verticalAlignment: Text.AlignVCenter
                                                                    elide: Text.ElideRight
                                                                    leftPadding: 6
                                                                }
                                                                background: Rectangle {
                                                                    color: parent.highlighted ? "#e6f5ec" : (parent.hovered ? "#f4f9f6" : "transparent")
                                                                    radius: 6
                                                                }
                                                            }
                                                        }
                                                    }
                                                }

                                                Text {
                                                    Layout.fillWidth: true
                                                    horizontalAlignment: Text.AlignRight
                                                    verticalAlignment: Text.AlignVCenter
                                                    elide: Text.ElideRight
                                                    font.pointSize: 9
                                                    color: "#999"
                                                    text: keySettingsPopup.cKeyModeIndex === 1 ? "可配置单击/双击/长按" : "按下即发 / 松开即发"
                                                }
                                            }
                                                                        Rectangle {
                                        Layout.fillWidth: true; height: 80; radius: 8; color: "#f7f7f7"
                                        /* 09-07: C 键三手势(单击/双击/长按) —— 卡片盒仅"手势模式"显示;
                                         * "常规模式"不显示单击/双击/长按(单击即直通), 只显示下方配置行。
                                         * 手势位 g=0/1/2; 捕获索引 = 10 + ki*3 + g(C++ kCKeyGestureBase)。
                                         * 09-07: 模式行已外提到 keySettingsColumn 中(在盒子之上), 本盒仅装卡片。 */
                                        visible: keySettingsPopup.selectedKeyIndex >= 0 &&
                                                 keySettingsPopup.selectedKeyIndex < 3 &&
                                                 keySettingsPopup.cKeyModeIndex === 1
                                        ColumnLayout {
                                            anchors.fill: parent; anchors.margins: 12
                                            spacing: 8


                                            /* 三列手势卡片 */
                                            RowLayout {
                                                spacing: 8
                                                Layout.fillWidth: true
                                                property var gestureDefs: [
                                                    { title: "单击", g: 0 },
                                                    { title: "双击", g: 1 },
                                                    { title: "长按", g: 2 }
                                                ]
                                                Repeater {
                                                    model: parent.gestureDefs
                                                    delegate: Rectangle {
                                                        Layout.fillWidth: true
                                                        height: 52
                                                        radius: 6
                                                        /* 常规模式下双击/长按锁定(置灰), 单击始终可编辑 */
                                                        property bool cGestureLocked: keySettingsPopup.cKeyModeIndex === 0 && modelData.g !== 0
                                                        color: {
                                                            if (cGestureLocked) return "#f2f4f3"
                                                            return keySettingsPopup.cGestureState === modelData.g ? "#e6f5ec" : "white"
                                                        }
                                                        border.color: {
                                                            if (cGestureLocked) return "#e3e8e5"
                                                            return keySettingsPopup.cGestureState === modelData.g ? "#07c160" : "#d5e0da"
                                                        }
                                                        border.width: 1
                                                        ColumnLayout {
                                                            anchors.fill: parent
                                                            anchors.margins: 6
                                                            spacing: 2
                                                            Text {
                                                                text: modelData.title
                                                                font.pointSize: 10
                                                                font.bold: true
                                                                color: cGestureLocked ? "#aaa" : (keySettingsPopup.cGestureState === modelData.g ? "#07c160" : "#666")
                                                                Layout.alignment: Qt.AlignHCenter
                                                            }
                                                            Text {
                                                                text: {
                                                                    var _ = refreshKeyTick
                                                                    if (cGestureLocked) return "点击启用"
                                                                    return cKeyStatusText(keySettingsPopup.selectedKeyIndex, modelData.g)
                                                                }
                                                                font.pointSize: 9
                                                                color: cGestureLocked ? "#bbb" : "#333"
                                                                Layout.fillWidth: true
                                                                horizontalAlignment: Text.AlignHCenter
                                                                elide: Text.ElideRight
                                                            }
                                                        }
                                                        MouseArea {
                                                            anchors.fill: parent
                                                            onClicked: {
                                                                /* 常规模式点双击/长按 → 自动切入手势模式(显式落盘)
                                                                 * 并编辑该手势; 切回后用户仍需"写入"才下发到设备 */
                                                                if (keySettingsPopup.cKeyModeIndex === 0 && modelData.g !== 0) {
                                                                    keyConfig.setKeyMode(keySettingsPopup.selectedKeyIndex, 1)
                                                                    keySettingsPopup.cKeyModeIndex = 1
                                                                    cModeCombo.currentIndex = 1
                                                                }
                                                                keySettingsPopup.cGestureState = modelData.g
                                                                keySettingsPopup.syncCKeyCombo()
                                                            }
                                                        }
                                                    }
                                                }
                                            }

                                        }
                                    }
                                    /* 配置行: 动作大类 + 触发细分/组合键/多媒体 + 捕获按钮 */
                                    RowLayout {
                                        Layout.fillWidth: true
                                        visible: keySettingsPopup.selectedKeyIndex >= 0 && keySettingsPopup.selectedKeyIndex < 3
                                        spacing: 6

                                        /* 动作大类 */
                                        ComboBox {
                                            id: cTypeCombo
                                            Layout.preferredWidth: 100
                                            Layout.preferredHeight: 30
                                            focusPolicy: Qt.NoFocus
                                            font.pointSize: 10
                                            model: lTypeCombo.model
                                            textRole: "label"
                                            onActivated: {
                                                var ki = keySettingsPopup.selectedKeyIndex
                                                var g = keySettingsPopup.cGestureState
                                                var v = model[currentIndex].v
                                                if (v === "mouse") {
                                                    /* C 键 action 字段存【大类】(mouse/keyboard/multimedia/none),
                                                     * 细分(左/右/中/滚轮)走独立 keycode —— 不能把细分值
                                                     * 写进 action(否则 cKeyActionValue 派生为 none, 下拉弹回
                                                     * "无动作", 见 09-07 修复)。保持鼠标族现值, 否则默认左键。 */
                                                    var cur = cKeyActionValue(ki, g)
                                                    var curIsMouse = (cur === "mouse_left" || cur === "mouse_right" || cur === "mouse_middle" || cur === "wheel_up" || cur === "wheel_down")
                                                    var mkc = 1
                                                    if (curIsMouse) {
                                                        if (cur === "mouse_right") mkc = 2
                                                        else if (cur === "mouse_middle") mkc = 4
                                                        else if (cur === "wheel_up") mkc = 5
                                                        else if (cur === "wheel_down") mkc = 6
                                                    }
                                                    keyConfig.setKeyAction(ki, g, "mouse")
                                                    keyConfig.setKeyKeycode(ki, g, mkc)
                                                } else {
                                                    keyConfig.setKeyAction(ki, g, v)
                                                }
                                                refreshKeyTick++
                                                keySettingsPopup.syncCKeyCombo()
                                                keySettingsPopup.syncCKeyMode()
                                            }
                                            background: Rectangle {
                                                implicitHeight: 30
                                                radius: 8
                                                color: "white"
                                                border.color: cTypeCombo.hovered || cTypeCombo.popup.visible ? "#07c160" : "#d0e0d8"
                                                border.width: 1
                                            }
                                            contentItem: Text {
                                                leftPadding: 8
                                                rightPadding: 22
                                                text: cTypeCombo.displayText
                                                font: cTypeCombo.font
                                                color: "#333"
                                                verticalAlignment: Text.AlignVCenter
                                                elide: Text.ElideRight
                                            }
                                            indicator: Canvas {
                                                x: cTypeCombo.width - width - 9
                                                y: cTypeCombo.height / 2 - height / 2
                                                width: 9; height: 5
                                                contextType: "2d"
                                                onPaint: {
                                                    var ctx = getContext("2d")
                                                    ctx.reset()
                                                    ctx.moveTo(0, 0); ctx.lineTo(width, 0); ctx.lineTo(width / 2, height)
                                                    ctx.closePath(); ctx.fillStyle = "#07c160"; ctx.fill()
                                                }
                                            }
                                            popup: Popup {
                                                y: cTypeCombo.height + 4
                                                width: cTypeCombo.width
                                                padding: 3
                                                implicitHeight: Math.min(30 * cTypeCombo.model.length + 6, 240)
                                                background: Rectangle { radius: 8; color: "white"; border.color: "#e0e0e0"; border.width: 1 }
                                                contentItem: ListView {
                                                    clip: true
                                                    implicitHeight: Math.min(30 * cTypeCombo.model.length, 234)
                                                    model: cTypeCombo.delegateModel
                                                    currentIndex: cTypeCombo.highlightedIndex
                                                    delegate: ItemDelegate {
                                                        width: ListView.view.width
                                                        height: 30
                                                        highlighted: ListView.isCurrentItem
                                                        hoverEnabled: true
                                                        onClicked: cTypeCombo.activated(index)
                                                        contentItem: Text {
                                                            text: cTypeCombo.model[index].label
                                                            color: parent.highlighted ? "#07c160" : "#333"
                                                            font: cTypeCombo.font
                                                            verticalAlignment: Text.AlignVCenter
                                                            elide: Text.ElideRight
                                                            leftPadding: 6
                                                        }
                                                        background: Rectangle {
                                                            color: parent.highlighted ? "#e6f5ec" : (parent.hovered ? "#f4f9f6" : "transparent")
                                                            radius: 6
                                                        }
                                                    }
                                                }
                                            }
                                        }

                                        /* 鼠标细分触发 */
                                        ComboBox {
                                            id: cTriggerCombo
                                            Layout.preferredWidth: 100
                                            Layout.preferredHeight: 30
                                            focusPolicy: Qt.NoFocus
                                            font.pointSize: 10
                                            model: lTypeCombo.cTriggerModel
                                            textRole: "label"
                                            visible: cTypeCombo.currentIndex === 0
                                            onActivated: {
                                                var ki = keySettingsPopup.selectedKeyIndex
                                                var g = keySettingsPopup.cGestureState
                                                var v = model[currentIndex].v
                                                var kc = v === "mouse_left" ? 1 : v === "mouse_right" ? 2 : v === "mouse_middle" ? 4 : v === "wheel_up" ? 5 : 6
                                                keyConfig.setKeyAction(ki, g, "mouse")
                                                keyConfig.setKeyKeycode(ki, g, kc)
                                                refreshKeyTick++
                                                keySettingsPopup.syncCKeyMode()
                                            }
                                            background: Rectangle {
                                                implicitHeight: 30
                                                radius: 8
                                                color: "white"
                                                border.color: cTriggerCombo.hovered || cTriggerCombo.popup.visible ? "#07c160" : "#d0e0d8"
                                                border.width: 1
                                            }
                                            contentItem: Text {
                                                leftPadding: 8
                                                rightPadding: 22
                                                text: cTriggerCombo.displayText
                                                font: cTriggerCombo.font
                                                color: "#333"
                                                verticalAlignment: Text.AlignVCenter
                                                elide: Text.ElideRight
                                            }
                                            indicator: Canvas {
                                                x: cTriggerCombo.width - width - 9
                                                y: cTriggerCombo.height / 2 - height / 2
                                                width: 9; height: 5
                                                contextType: "2d"
                                                onPaint: {
                                                    var ctx = getContext("2d")
                                                    ctx.reset()
                                                    ctx.moveTo(0, 0); ctx.lineTo(width, 0); ctx.lineTo(width / 2, height)
                                                    ctx.closePath(); ctx.fillStyle = "#07c160"; ctx.fill()
                                                }
                                            }
                                            popup: Popup {
                                                y: cTriggerCombo.height + 4
                                                width: cTriggerCombo.width
                                                padding: 3
                                                implicitHeight: Math.min(30 * cTriggerCombo.model.length + 6, 240)
                                                background: Rectangle { radius: 8; color: "white"; border.color: "#e0e0e0"; border.width: 1 }
                                                contentItem: ListView {
                                                    clip: true
                                                    implicitHeight: Math.min(30 * cTriggerCombo.model.length, 234)
                                                    model: cTriggerCombo.delegateModel
                                                    currentIndex: cTriggerCombo.highlightedIndex
                                                    delegate: ItemDelegate {
                                                        width: ListView.view.width
                                                        height: 30
                                                        highlighted: ListView.isCurrentItem
                                                        hoverEnabled: true
                                                        onClicked: cTriggerCombo.activated(index)
                                                        contentItem: Text {
                                                            text: cTriggerCombo.model[index].label
                                                            color: parent.highlighted ? "#07c160" : "#333"
                                                            font: cTriggerCombo.font
                                                            verticalAlignment: Text.AlignVCenter
                                                            elide: Text.ElideRight
                                                            leftPadding: 6
                                                        }
                                                        background: Rectangle {
                                                            color: parent.highlighted ? "#e6f5ec" : (parent.hovered ? "#f4f9f6" : "transparent")
                                                            radius: 6
                                                        }
                                                    }
                                                }
                                            }
                                        }

                                        /* 键盘组合: 点击框进入捕获(与 EC/L 弹窗同款), 捕获中/悬停边框变绿 */
                                        Rectangle {
                                            id: cKbBox
                                            Layout.fillWidth: true
                                            Layout.preferredHeight: 30
                                            radius: 8
                                            color: "white"
                                            property bool hovered: false
                                            property int capTarget: 10 + keySettingsPopup.selectedKeyIndex * 3 + keySettingsPopup.cGestureState
                                            border.color: (keyConfig.captureIndex === cKbBox.capTarget || cKbBox.hovered) ? "#07c160" : "#d0e0d8"
                                            border.width: 1
                                            visible: cTypeCombo.currentIndex === 1
                                            Text {
                                                anchors.fill: parent
                                                anchors.leftMargin: 6
                                                anchors.rightMargin: 6
                                                verticalAlignment: Text.AlignVCenter
                                                horizontalAlignment: Text.AlignHCenter
                                                elide: Text.ElideMiddle
                                                font.pointSize: 10
                                                color: keyConfig.captureIndex === cKbBox.capTarget ? "#07c160" : "#333"
                                                text: {
                                                    var _ = refreshKeyTick
                                                    if (keyConfig.captureIndex === cKbBox.capTarget) return "捕获中…"
                                                    return cKeyStatusText(keySettingsPopup.selectedKeyIndex, keySettingsPopup.cGestureState)
                                                }
                                            }
                                            MouseArea {
                                                anchors.fill: parent
                                                hoverEnabled: true
                                                onEntered: cKbBox.hovered = true
                                                onExited: cKbBox.hovered = false
                                                onClicked: {
                                                    if (keyConfig.captureIndex === cKbBox.capTarget) keyConfig.cancelCapture()
                                                    else keyConfig.startCapture(keySettingsPopup.selectedKeyIndex, keySettingsPopup.cGestureState)
                                                }
                                            }
                                        }

                                        /* 多媒体细分 */
                                        ComboBox {
                                            id: cMmCombo
                                            Layout.preferredWidth: 100
                                            Layout.preferredHeight: 30
                                            focusPolicy: Qt.NoFocus
                                            font.pointSize: 10
                                            model: ecSettingsPopup.mmModel
                                            textRole: "label"
                                            visible: cTypeCombo.currentIndex === 2
                                            onActivated: {
                                                var ki = keySettingsPopup.selectedKeyIndex
                                                var g = keySettingsPopup.cGestureState
                                                keyConfig.setKeyAction(ki, g, "multimedia")
                                                keyConfig.setKeyKeycode(ki, g, model[currentIndex].v)
                                                refreshKeyTick++
                                                keySettingsPopup.syncCKeyMode()
                                            }
                                            background: Rectangle {
                                                implicitHeight: 30
                                                radius: 8
                                                color: "white"
                                                border.color: cMmCombo.hovered || cMmCombo.popup.visible ? "#07c160" : "#d0e0d8"
                                                border.width: 1
                                            }
                                            contentItem: Text {
                                                leftPadding: 8
                                                rightPadding: 22
                                                text: cMmCombo.displayText
                                                font: cMmCombo.font
                                                color: "#333"
                                                verticalAlignment: Text.AlignVCenter
                                                elide: Text.ElideRight
                                            }
                                            indicator: Canvas {
                                                x: cMmCombo.width - width - 9
                                                y: cMmCombo.height / 2 - height / 2
                                                width: 9; height: 5
                                                contextType: "2d"
                                                onPaint: {
                                                    var ctx = getContext("2d")
                                                    ctx.reset()
                                                    ctx.moveTo(0, 0); ctx.lineTo(width, 0); ctx.lineTo(width / 2, height)
                                                    ctx.closePath(); ctx.fillStyle = "#07c160"; ctx.fill()
                                                }
                                            }
                                            popup: Popup {
                                                y: cMmCombo.height + 4
                                                width: cMmCombo.width
                                                padding: 3
                                                implicitHeight: Math.min(30 * cMmCombo.model.length + 6, 240)
                                                background: Rectangle { radius: 8; color: "white"; border.color: "#e0e0e0"; border.width: 1 }
                                                contentItem: ListView {
                                                    clip: true
                                                    implicitHeight: Math.min(30 * cMmCombo.model.length, 234)
                                                    model: cMmCombo.delegateModel
                                                    currentIndex: cMmCombo.highlightedIndex
                                                    delegate: ItemDelegate {
                                                        width: ListView.view.width
                                                        height: 30
                                                        highlighted: ListView.isCurrentItem
                                                        hoverEnabled: true
                                                        onClicked: cMmCombo.activated(index)
                                                        contentItem: Text {
                                                            text: cMmCombo.model[index].label
                                                            color: parent.highlighted ? "#07c160" : "#333"
                                                            font: cMmCombo.font
                                                            verticalAlignment: Text.AlignVCenter
                                                            elide: Text.ElideRight
                                                            leftPadding: 6
                                                        }
                                                        background: Rectangle {
                                                            color: parent.highlighted ? "#e6f5ec" : (parent.hovered ? "#f4f9f6" : "transparent")
                                                            radius: 6
                                                        }
                                                    }
                                                }
                                            }
                                        }
                                    }

                                    /* 09-06: L 键键盘组合的操作步骤引导(仅常规模式; 手势模式另有引导) */
                                    Text {
                                        Layout.fillWidth: true
                                        visible: (keySettingsPopup.selectedKeyIndex === 3 ||
                                                  keySettingsPopup.selectedKeyIndex === 5 ||
                                                  keySettingsPopup.selectedKeyIndex === 6) &&
                                                 keySettingsPopup.lModeIndex === 0
                                        wrapMode: Text.WordWrap
                                        font.pointSize: 10
                                        text: {
                                            var _ = refreshKeyTick
                                            var ki = keySettingsPopup.selectedKeyIndex
                                            if (ki !== 3 && ki !== 5 && ki !== 6) return ""
                                            if (lTypeCombo.currentIndex !== 1 && lTypeCombo.currentIndex !== 2) return ""
                                            if (keyConfig.captureIndex === ki)
                                                return "正在捕获：请在键盘上按下想设置的组合键（纯修饰键如仅 Ctrl 也可）"
                                            var m = lKeyMap(ki === 3 ? 2 : ki - 5)
                                            if (m.action === "multimedia") {
                                                var n = mmCodeName(m.keycode)
                                                if (n === "?") return "已选多媒体：请点击右侧下拉选择具体键码 → 点击上方\"写入\"保存"
                                                return "已选多媒体键：" + n + " · 点击上方\"写入\"按钮保存到当前蓝牙槽位"
                                            }
                                            if (m.action !== "keyboard") return ""
                                            var t = formatShortcut(m.modifier, m.keycode)
                                            if (t.length === 0)
                                                return "设置步骤：① 点击右侧\"未设置\"框 → ② 按下想设置的组合键 → ③ 点击上方\"写入\"保存到当前蓝牙槽位"
                                            return "已设置 " + t + " · 点击上方\"写入\"按钮保存到当前蓝牙槽位"
                                        }
                                        color: {
                                            var ki = keySettingsPopup.selectedKeyIndex
                                            if (keyConfig.captureIndex === ki) return "#07c160"
                                            var m = (ki === 3 || ki === 5 || ki === 6) ? lKeyMap(ki === 3 ? 2 : ki - 5) : null
                                            if (m && m.action === "keyboard" && formatShortcut(m.modifier, m.keycode).length === 0) return "#f56c6c"
                                            return "#999"
                                        }
                                    }

                                    ColumnLayout {
                                        spacing: 10
                                        Layout.fillWidth: true
                                        visible: keySettingsPopup.selectedKeyIndex >= 0 && keySettingsPopup.selectedKeyIndex < 3

                                        RowLayout { spacing: 10
                                            Text { text: "RGB 底光"; font.pointSize: 12; font.bold: true; color: "#333" }
                                            Rectangle { width: 22; height: 22; radius: 4; color: (keySettingsPopup.selectedKeyIndex >= 0 && keyConfig.keyConfigs[keySettingsPopup.selectedKeyIndex].rgb_enabled) ? Qt.hsva(keySettingsPopup.pHue / 360, keySettingsPopup.pSat, keySettingsPopup.pVal, 1) : "#222222" }
                                            Item { Layout.fillWidth: true }
                                            Rectangle { width: 50; height: 26; radius: 13
                                                color: (keySettingsPopup.selectedKeyIndex >= 0 && keyConfig.keyConfigs[keySettingsPopup.selectedKeyIndex].rgb_enabled) ? "#07c160" : "#ccc"
                                                Text { anchors.centerIn: parent; text: (keySettingsPopup.selectedKeyIndex >= 0 && keyConfig.keyConfigs[keySettingsPopup.selectedKeyIndex].rgb_enabled) ? "开" : "关"; font.pointSize: 10; color: "white" }
                                                MouseArea { anchors.fill: parent; onClicked: { if (keySettingsPopup.selectedKeyIndex < 0) return; var on = !keyConfig.keyConfigs[keySettingsPopup.selectedKeyIndex].rgb_enabled; keyConfig.setKeyRgbEnabled(keySettingsPopup.selectedKeyIndex, on); keyConfig.previewKeyRgb(keySettingsPopup.selectedKeyIndex) } }
                                            }
                                        }

                                        RowLayout { spacing: 8
                                            Repeater {
                                                model: [
                                                    {"c": 0xFF3B30}, {"c": 0xFF9500}, {"c": 0xFFCC00}, {"c": 0x34C759},
                                                    {"c": 0x00C7BE}, {"c": 0x007AFF}, {"c": 0x5856D6}, {"c": 0xFFFFFF}
                                                ]
                                                delegate: Rectangle { width: 26; height: 26; radius: 13
                                                    color: Qt.rgba((modelData.c >> 16 & 0xFF) / 255, (modelData.c >> 8 & 0xFF) / 255, (modelData.c & 0xFF) / 255, 1)
                                                    border.color: (keySettingsPopup.selectedKeyIndex >= 0 && keyConfig.keyConfigs[keySettingsPopup.selectedKeyIndex].rgb_color === modelData.c) ? "#333" : "transparent"
                                                    border.width: 2
                                                    MouseArea { anchors.fill: parent; onClicked: { if (keySettingsPopup.selectedKeyIndex < 0) return; var c = modelData.c; keyConfig.setKeyRgbColor(keySettingsPopup.selectedKeyIndex, (c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF); keyConfig.previewKeyRgb(keySettingsPopup.selectedKeyIndex); keySettingsPopup.syncFromColor() } }
                                                }
                                            }
                                        }

                                        ColumnLayout { spacing: 8; Layout.fillWidth: true
                                            Rectangle { id: popupSvRect; Layout.fillWidth: true; height: 70; radius: 10; clip: true
                                                Rectangle { anchors.fill: parent; radius: popupSvRect.radius
                                                    gradient: Gradient { orientation: Gradient.Horizontal
                                                        GradientStop { position: 0; color: "#ffffff" }
                                                        GradientStop { position: 1; color: keySettingsPopup.svBaseColor }
                                                    }
                                                }
                                                Rectangle { anchors.fill: parent; radius: popupSvRect.radius
                                                    gradient: Gradient { orientation: Gradient.Vertical
                                                        GradientStop { position: 0; color: "#00000000" }
                                                        GradientStop { position: 1; color: "#ff000000" }
                                                    }
                                                }
                                                Rectangle { width: 14; height: 14; radius: 7; color: "transparent"; border.color: "white"; border.width: 2
                                                    x: keySettingsPopup.pSat * popupSvRect.width - 7; y: (1 - keySettingsPopup.pVal) * popupSvRect.height - 7 }
                                                MouseArea { anchors.fill: parent
                                                    onPressed: keySettingsPopup.updateSV(popupSvRect, mouseX, mouseY)
                                                    onPositionChanged: { if (pressed) keySettingsPopup.updateSV(popupSvRect, mouseX, mouseY) }
                                                }
                                            }
                                            Rectangle { id: popupHueRect; Layout.fillWidth: true; height: 16; radius: 8
                                                gradient: Gradient { orientation: Gradient.Horizontal
                                                    GradientStop { position: 0.0;    color: "#ff0000" }
                                                    GradientStop { position: 0.1667; color: "#ffff00" }
                                                    GradientStop { position: 0.3333; color: "#00ff00" }
                                                    GradientStop { position: 0.5;    color: "#00ffff" }
                                                    GradientStop { position: 0.6667; color: "#0000ff" }
                                                    GradientStop { position: 0.8333; color: "#ff00ff" }
                                                    GradientStop { position: 1.0;    color: "#ff0000" }
                                                }
                                                Rectangle { width: 4; height: popupHueRect.height; radius: 2; color: "transparent"; border.color: "white"; border.width: 2
                                                    x: (keySettingsPopup.pHue / 360) * popupHueRect.width - 2 }
                                                MouseArea { anchors.fill: parent
                                                    onPressed: keySettingsPopup.updateHue(popupHueRect, mouseX)
                                                    onPositionChanged: { if (pressed) keySettingsPopup.updateHue(popupHueRect, mouseX) }
                                                }
                                            }
                                            RowLayout { spacing: 8
                                                Rectangle { width: 22; height: 22; radius: 4; color: Qt.hsva(keySettingsPopup.pHue / 360, keySettingsPopup.pSat, keySettingsPopup.pVal, 1) }
                                                Text { text: keySettingsPopup.hexText(); font.pointSize: 11; color: "#666" }
                                            }
                                        }

                                        RowLayout { spacing: 6
                                            Text { text: "亮度"; font.pointSize: 11; color: "#888"; Layout.preferredWidth: 36 }
                                            /* ★ 10-04: 量程与固件上限(15%)对齐 —— 0~100 的 16~100 段
                                             * 会被固件一律压到 15，属无效区间。与灯效测试滑块保持一致。 */
                                            Slider { id: popupBrs; from: 0; to: keyConfig.ledBrightnessHardwareCap()
                                                value: (keySettingsPopup.selectedKeyIndex >= 0 && keySettingsPopup.selectedKeyIndex < 3) ? keyConfig.keyConfigs[keySettingsPopup.selectedKeyIndex].rgb_brightness : keyConfig.ledBrightnessHardwareCap(); Layout.fillWidth: true
                                                onMoved: { if (keySettingsPopup.selectedKeyIndex < 0) return; keyConfig.setKeyRgbBrightness(keySettingsPopup.selectedKeyIndex, value); keyConfig.previewKeyRgb(keySettingsPopup.selectedKeyIndex) }
                                                onPressedChanged: { if (keySettingsPopup.selectedKeyIndex >= 0 && !pressed) keyConfig.previewKeyRgb(keySettingsPopup.selectedKeyIndex) }
                                                background: Rectangle {
                                                    id: brsBg
                                                    implicitWidth: 200
                                                    implicitHeight: 10
                                                    width: popupBrs.width
                                                    height: implicitHeight
                                                    radius: 5
                                                    color: "#e0e0e0"
                                                    Rectangle {
                                                        width: popupBrs.visualPosition * parent.width
                                                        height: parent.height
                                                        radius: 5
                                                        color: Qt.hsva(keySettingsPopup.pHue / 360, keySettingsPopup.pSat, keySettingsPopup.pVal, 1)
                                                    }
                                                }
                                                handle: Rectangle {
                                                    x: popupBrs.visualPosition * (popupBrs.width - width)
                                                    anchors.verticalCenter: brsBg.verticalCenter
                                                    implicitWidth: 18
                                                    implicitHeight: 18
                                                    radius: 9
                                                    color: "white"
                                                    border.color: Qt.hsva(keySettingsPopup.pHue / 360, keySettingsPopup.pSat, keySettingsPopup.pVal, 1)
                                                    border.width: 3
                                                }
                                            }
                                            Text { text: (keySettingsPopup.selectedKeyIndex >= 0 && keySettingsPopup.selectedKeyIndex < 3) ? (keyConfig.keyConfigs[keySettingsPopup.selectedKeyIndex].rgb_brightness + "%") : "100%"; font.pointSize: 11; color: "#666"; Layout.preferredWidth: 36; horizontalAlignment: Text.AlignRight }
                                        }
                                    }

                                    /* KEY L1 空中鼠标专属设置(仅当 L1 动作仍为"空中鼠标"时显示;
                                     * 09-05 L1 可改键盘/多媒体/无动作, 此时隐藏) */
                                    ColumnLayout {
                                        spacing: 12
                                        Layout.fillWidth: true
                                        visible: keySettingsPopup.selectedKeyIndex === 3 &&
                                                 keyConfig.l1Key.action === "airmouse" &&
                                                 keySettingsPopup.lModeIndex === 0   /* 09-07: 手势模式下 L1 空中鼠标由手势触发 */

                                        Text { text: "空中鼠标触发方式"; font.pointSize: 12; font.bold: true; color: "#333" }
                                        RowLayout { spacing: 8
                                            Rectangle { Layout.fillWidth: true; height: 32; radius: 6
                                                color: keyConfig.airMouseMode === 0 ? "#e6f5ec" : "#f2f3f5"
                                                border.color: keyConfig.airMouseMode === 0 ? "#07c160" : "transparent"; border.width: 1
                                                Text { anchors.centerIn: parent; text: "单击切换"; font.pointSize: 11; color: keyConfig.airMouseMode === 0 ? "#07c160" : "#666" }
                                                MouseArea { anchors.fill: parent; onClicked: keyConfig.setAirMouseMode(0) }
                                            }
                                            Rectangle { Layout.fillWidth: true; height: 32; radius: 6
                                                color: keyConfig.airMouseMode === 1 ? "#e6f5ec" : "#f2f3f5"
                                                border.color: keyConfig.airMouseMode === 1 ? "#07c160" : "transparent"; border.width: 1
                                                Text { anchors.centerIn: parent; text: "按住移动"; font.pointSize: 11; color: keyConfig.airMouseMode === 1 ? "#07c160" : "#666" }
                                                MouseArea { anchors.fill: parent; onClicked: keyConfig.setAirMouseMode(1) }
                                            }
                                        }
                                        Text { text: "单击切换：按一下开始移动，再按一下停止\n按住移动：按住时移动，松开立即停止"; font.pointSize: 10; color: "#999" }

                                        Text { text: "灵敏度"; font.pointSize: 12; font.bold: true; color: "#333" }
                                        RowLayout { spacing: 8
                                            Rectangle { Layout.fillWidth: true; height: 32; radius: 6
                                                color: keyConfig.airMouseSpeed === 0 ? "#e6f5ec" : "#f2f3f5"
                                                border.color: keyConfig.airMouseSpeed === 0 ? "#07c160" : "transparent"; border.width: 1
                                                Text { anchors.centerIn: parent; text: "慢"; font.pointSize: 11; color: keyConfig.airMouseSpeed === 0 ? "#07c160" : "#666" }
                                                MouseArea { anchors.fill: parent; onClicked: keyConfig.setAirMouseSpeed(0) }
                                            }
                                            Rectangle { Layout.fillWidth: true; height: 32; radius: 6
                                                color: keyConfig.airMouseSpeed === 1 ? "#e6f5ec" : "#f2f3f5"
                                                border.color: keyConfig.airMouseSpeed === 1 ? "#07c160" : "transparent"; border.width: 1
                                                Text { anchors.centerIn: parent; text: "中"; font.pointSize: 11; color: keyConfig.airMouseSpeed === 1 ? "#07c160" : "#666" }
                                                MouseArea { anchors.fill: parent; onClicked: keyConfig.setAirMouseSpeed(1) }
                                            }
                                            Rectangle { Layout.fillWidth: true; height: 32; radius: 6
                                                color: keyConfig.airMouseSpeed === 2 ? "#e6f5ec" : "#f2f3f5"
                                                border.color: keyConfig.airMouseSpeed === 2 ? "#07c160" : "transparent"; border.width: 1
                                                Text { anchors.centerIn: parent; text: "快"; font.pointSize: 11; color: keyConfig.airMouseSpeed === 2 ? "#07c160" : "#666" }
                                                MouseArea { anchors.fill: parent; onClicked: keyConfig.setAirMouseSpeed(2) }
                                            }
                                        }
                                    }

                                    RowLayout { spacing: 8
                                        Item { Layout.fillWidth: true }
                                        Rectangle { width: 84; height: 32; radius: 16; color: "#07c160"
                                            Text { anchors.centerIn: parent; text: "完成"; font.pointSize: 12; color: "white" }
                                            MouseArea { anchors.fill: parent; onClicked: keySettingsPopup.close() }
                                        }
                                    }
                                }
                            }

                            /* 09-03: 摇一摇设置弹窗(独立卡片点击弹出)。
                             * 快捷键类型 = 键盘组合键(捕获)或"无"; 支持纯修饰键组合。
                             * ⚠️ QML 只读绑定 keyConfig.shake* 属性, 禁止反向赋值(断绑定)。 */
                            Popup {
                                id: shakeSettingsPopup
                                parent: Overlay.overlay
                                x: Math.max(12, Math.min(parent.width - width - 12, (parent.width - width) / 2))
                                y: Math.max(40, Math.min(parent.height - height - 12, (parent.height - height) / 2))
                                width: 340
                                padding: 14
                                modal: true
                                focus: true
                                closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
                                background: Rectangle { radius: 12; color: "white"; border.color: "#e8e8e8"; border.width: 1 }
                                Overlay.modal: Rectangle { color: "#80000000"; radius: 12 }
                                onClosed: {
                                    if (keyConfig.captureIndex === -2) keyConfig.cancelCapture()
                                }

                                ColumnLayout {
                                    width: parent.width
                                    spacing: 12

                                    RowLayout {
                                        Layout.fillWidth: true
                                        Text {
                                            text: "摇一摇设置"
                                            font.pointSize: 16; font.bold: true; color: "#222"
                                        }
                                        Item { Layout.fillWidth: true }
                                        Rectangle { width: 28; height: 28; radius: 14; color: "#f0f0f0"
                                            Text { anchors.centerIn: parent; text: "×"; font.pointSize: 16; color: "#222" }
                                            MouseArea { anchors.fill: parent; onClicked: shakeSettingsPopup.close() }
                                        }
                                    }

                                    Text {
                                        text: "快速晃动设备即可触发一次指定快捷键，例如翻页 / 切换静音 / 呼出工具。"
                                        font.pointSize: 10; color: "#999"
                                        Layout.fillWidth: true
                                        wrapMode: Text.WordWrap
                                    }

                                    /* 启用开关 */
                                    RowLayout { spacing: 10; Layout.fillWidth: true
                                        Text { text: "启用摇一摇"; font.pointSize: 12; font.bold: true; color: "#333" }
                                        Item { Layout.fillWidth: true }
                                        Rectangle {
                                            width: 50; height: 26; radius: 13
                                            color: keyConfig.shakeEnabled === 1 ? "#07c160" : "#ccc"
                                            Text { anchors.centerIn: parent
                                                text: keyConfig.shakeEnabled === 1 ? "开" : "关"
                                                font.pointSize: 10; color: "white" }
                                            MouseArea { anchors.fill: parent
                                                onClicked: keyConfig.setShakeEnabled(keyConfig.shakeEnabled === 1 ? 0 : 1) }
                                        }
                                    }

                                    /* 灵敏度 */
                                    Text { text: "灵敏度"; font.pointSize: 12; font.bold: true; color: "#333" }
                                    RowLayout { spacing: 8
                                        Rectangle { Layout.fillWidth: true; height: 32; radius: 6
                                            color: keyConfig.shakeSens === 0 ? "#e6f5ec" : "#f2f3f5"
                                            border.color: keyConfig.shakeSens === 0 ? "#07c160" : "transparent"; border.width: 1
                                            Text { anchors.centerIn: parent; text: "轻"; font.pointSize: 11; color: keyConfig.shakeSens === 0 ? "#07c160" : "#666" }
                                            MouseArea { anchors.fill: parent; onClicked: keyConfig.setShakeSens(0) }
                                        }
                                        Rectangle { Layout.fillWidth: true; height: 32; radius: 6
                                            color: keyConfig.shakeSens === 1 ? "#e6f5ec" : "#f2f3f5"
                                            border.color: keyConfig.shakeSens === 1 ? "#07c160" : "transparent"; border.width: 1
                                            Text { anchors.centerIn: parent; text: "中"; font.pointSize: 11; color: keyConfig.shakeSens === 1 ? "#07c160" : "#666" }
                                            MouseArea { anchors.fill: parent; onClicked: keyConfig.setShakeSens(1) }
                                        }
                                        Rectangle { Layout.fillWidth: true; height: 32; radius: 6
                                            color: keyConfig.shakeSens === 2 ? "#e6f5ec" : "#f2f3f5"
                                            border.color: keyConfig.shakeSens === 2 ? "#07c160" : "transparent"; border.width: 1
                                            Text { anchors.centerIn: parent; text: "强"; font.pointSize: 11; color: keyConfig.shakeSens === 2 ? "#07c160" : "#666" }
                                            MouseArea { anchors.fill: parent; onClicked: keyConfig.setShakeSens(2) }
                                        }
                                    }
                                    Text { text: "轻：轻微晃动即触发，易误触\n中：常规晃动触发\n强：明显甩动才触发，最不易误触"
                                        font.pointSize: 10; color: "#999"; Layout.fillWidth: true; wrapMode: Text.WordWrap }

                                    /* 触发快捷键: 点击框进入捕获(与 C/L/EC 弹窗同款), 捕获中/悬停边框变绿 */
                                    Rectangle {
                                        id: shakeKbBox
                                        Layout.fillWidth: true
                                        height: 52
                                        radius: 8
                                        color: "white"
                                        property bool hovered: false
                                        border.color: (keyConfig.captureIndex === -2 || shakeKbBox.hovered) ? "#07c160" : "#d0e0d8"
                                        border.width: 1
                                        RowLayout {
                                            anchors.fill: parent; anchors.margins: 10; spacing: 8
                                            Text { text: "触发快捷键："; font.pointSize: 11; color: "#666" }
                                            Text {
                                                Layout.fillWidth: true
                                                text: keyConfig.captureIndex === -2 ? "捕获中…" : shakeShortcutText()
                                                font.pointSize: 11; font.bold: true
                                                color: keyConfig.captureIndex === -2 ? "#07c160" : "#333"
                                                elide: Text.ElideRight
                                            }
                                        }
                                        MouseArea {
                                            anchors.fill: parent
                                            hoverEnabled: true
                                            onEntered: shakeKbBox.hovered = true
                                            onExited: shakeKbBox.hovered = false
                                            onClicked: {
                                                if (keyConfig.captureIndex === -2) keyConfig.cancelCapture()
                                                else keyConfig.startShakeCapture()
                                            }
                                        }
                                    }
                                    RowLayout { spacing: 8
                                        Rectangle { width: 62; height: 30; radius: 15
                                            color: "#f0f0f0"
                                            border.color: "#d0d0d0"; border.width: 1
                                            Text { anchors.centerIn: parent; text: "清除"; font.pointSize: 11; color: "#666" }
                                            MouseArea { anchors.fill: parent; onClicked: keyConfig.clearShakeKey() }
                                        }
                                        Item { Layout.fillWidth: true }
                                        Rectangle { width: 84; height: 32; radius: 16; color: "#07c160"
                                            Text { anchors.centerIn: parent; text: "完成"; font.pointSize: 12; color: "white" }
                                            MouseArea { anchors.fill: parent; onClicked: shakeSettingsPopup.close() }
                                        }
                                    }
                                    Text {
                                        text: keyConfig.captureIndex === -2
                                              ? "捕获中：请在键盘上按下想触发的组合键（纯修饰键也可，如仅 Ctrl+Alt）"
                                              : "设置完成后请点击上方“写入”按钮保存到当前蓝牙槽位"
                                        font.pointSize: 10; color: "#f56c6c"
                                        Layout.fillWidth: true
                                        wrapMode: Text.WordWrap
                                    }
                                }
                            }
                        }

                        /* 09-05: EC 编码器设置弹窗(三手势各自可选动作) */
                        Popup {
                            id: ecSettingsPopup
                            parent: Overlay.overlay

                            x: Math.max(12, Math.min(parent.width - width - 12, (parent.width - width) / 2))
                            y: Math.max(40, Math.min(parent.height - height - 12, (parent.height - height) / 2))
                            width: 380
                            padding: 14
                            modal: true
                            focus: true
                            closePolicy: Popup.CloseOnEscape | Popup.CloseOnPressOutside
                            background: Rectangle { radius: 12; color: "white"; border.color: "#e8e8e8"; border.width: 1 }
                            Overlay.modal: Rectangle { color: "#80000000"; radius: 12 }
                            onClosed: {
                                var ci = keyConfig.captureIndex
                                /* 09-07: 主键 7..9 + 按下三手势 28..30 */
                                if ((ci >= 7 && ci <= 9) || (ci >= 28 && ci <= 32)) keyConfig.cancelCapture()
                            }

                            /* 09-06: 两级选择 —— 动作大类 + 鼠标类细分触发
                             * (示意图风格: 动作类型下拉 + 具体触发下拉 + 键盘捕获设置) */
                            property var typeModel: [
                                { label: "鼠标",     v: "mouse" },
                                { label: "键盘组合", v: "keyboard" },
                                { label: "多媒体",   v: "multimedia" },
                                { label: "无动作",   v: "none" }
                            ]
                            property var mouseTriggerModel: [
                                { label: "空中鼠标", v: "airmouse" },
                                { label: "滚轮上", v: "wheel_up" },
                                { label: "滚轮下", v: "wheel_down" },
                                { label: "左键",   v: "mouse_left" },
                                { label: "右键",   v: "mouse_right" },
                                { label: "中键",   v: "mouse_middle" }
                            ]
                            property var mmModel: [
                                { label: "音量+", v: 0xE9 },
                                { label: "音量-", v: 0xEA },
                                { label: "静音",   v: 0xE2 },
                                { label: "播放/暂停", v: 0xCD },
                                { label: "停止",   v: 0xB7 },
                                { label: "上一曲", v: 0xB6 },
                                { label: "下一曲", v: 0xB5 }
                            ]

                            /* 09-07: EC 按下 模式+三手势 UI 状态(数据层待对接) —— alias 到根, 与按键卡共享 */
                            property alias ecPressMode: root.ecPressMode
                            property alias ecPressGesture: root.ecPressGesture

                            ColumnLayout {
                                width: parent.width
                                spacing: 10

                                RowLayout {
                                    Layout.fillWidth: true
                                    Text { text: "EC 编码器"; font.pointSize: 16; font.bold: true; color: "#222" }
                                    Item { Layout.fillWidth: true }
                                    /* 10-05: 「模式」从「按下」行移到标题行(用户要求, 与网页端一致) ——
                                     * 下拉本体就是原来那个 ecPressModeCombo, 原样搬过来(去掉 visible 限制)。 */
                                    Text { text: "模式"; font.pointSize: 11; color: "#666"; Layout.alignment: Qt.AlignVCenter }
                                    ComboBox {
                                        id: ecPressModeCombo
                                        Layout.preferredWidth: 96
                                        Layout.preferredHeight: 26
                                        Layout.alignment: Qt.AlignVCenter
                                        focusPolicy: Qt.NoFocus
                                        font.pointSize: 10
                                        model: [
                                            { label: "常规模式", v: 0 },
                                            { label: "手势模式", v: 1 }
                                        ]
                                        textRole: "label"
                                        currentIndex: ecSettingsPopup.ecPressMode
                                        onActivated: {
                                            var v = model[currentIndex].v
                                            /* 09-07: 模式【显式落盘】(ec_mode) —— 手势配置存而不用,
                                             * 切换不清空: 切回手势模式原样恢复。 */
                                            keyConfig.setEcPressKeyMode(v)
                                            ecSettingsPopup.ecPressMode = v
                                            if (v === 0) {
                                                /* 常规模式: 取消按下三手势进行中的捕获(28..30), 以及
                                                 * 按压滚动两行(31/32 —— 该两行随即隐藏); 编辑回落单击 */
                                                var ci = keyConfig.captureIndex
                                                if ((ci >= 28 && ci <= 30) || ci === 31 || ci === 32)
                                                    keyConfig.cancelCapture()
                                                ecSettingsPopup.ecPressGesture = 0
                                            }
                                            refreshKeyTick++
                                            syncEcCombos()
                                        }
                                        background: Rectangle {
                                            implicitWidth: 96; implicitHeight: 26
                                            radius: 6; color: "white"
                                            border.color: ecPressModeCombo.hovered || ecPressModeCombo.popup.visible ? "#07c160" : "#d0e0d8"
                                            border.width: 1
                                        }
                                        contentItem: Text {
                                            leftPadding: 8; rightPadding: 18
                                            text: ecPressModeCombo.displayText
                                            font: ecPressModeCombo.font; color: "#333"
                                            verticalAlignment: Text.AlignVCenter; elide: Text.ElideRight
                                        }
                                        indicator: Canvas {
                                            x: ecPressModeCombo.width - width - 8
                                            y: ecPressModeCombo.height / 2 - height / 2
                                            width: 8; height: 5; contextType: "2d"
                                            onPaint: {
                                                var ctx = getContext("2d"); ctx.reset()
                                                ctx.moveTo(0, 0); ctx.lineTo(width, 0); ctx.lineTo(width / 2, height)
                                                ctx.closePath(); ctx.fillStyle = "#07c160"; ctx.fill()
                                            }
                                        }
                                        popup: Popup {
                                            y: ecPressModeCombo.height + 4
                                            x: 0
                                            width: ecPressModeCombo.width
                                            padding: 3
                                            implicitHeight: 26 * ecPressModeCombo.model.length + 6
                                            background: Rectangle { radius: 6; color: "white"; border.color: "#e0e0e0"; border.width: 1 }
                                            contentItem: ListView {
                                                clip: true
                                                implicitHeight: 26 * ecPressModeCombo.model.length
                                                model: ecPressModeCombo.delegateModel
                                                currentIndex: ecPressModeCombo.highlightedIndex
                                                delegate: ItemDelegate {
                                                    width: ListView.view.width
                                                    height: 26
                                                    highlighted: ListView.isCurrentItem
                                                    hoverEnabled: true
                                                    onClicked: ecPressModeCombo.activated(index)
                                                    contentItem: Text {
                                                        text: ecPressModeCombo.model[index].label
                                                        color: parent.highlighted ? "#07c160" : "#333"
                                                        font: ecPressModeCombo.font
                                                        verticalAlignment: Text.AlignVCenter
                                                        elide: Text.ElideRight
                                                        leftPadding: 8
                                                    }
                                                    background: Rectangle {
                                                        color: parent.highlighted ? "#e6f5ec" : (parent.hovered ? "#f4f9f6" : "transparent")
                                                        radius: 5
                                                    }
                                                }
                                            }
                                        }
                                    }
                                    Rectangle { width: 28; height: 28; radius: 14; color: "#f0f0f0"
                                        Text { anchors.centerIn: parent; text: "×"; font.pointSize: 16; color: "#222" }
                                        MouseArea { anchors.fill: parent; onClicked: ecSettingsPopup.close() }
                                    }
                                }

ScrollView {
    id: ecScroll
    Layout.fillWidth: true
    Layout.preferredHeight: Math.min(ecRowsCol.implicitHeight + 12, 500)
    contentWidth: availableWidth
    contentHeight: ecRowsCol.implicitHeight
    clip: true
    ScrollBar.vertical.policy: ScrollBar.AsNeeded

    ColumnLayout {
        id: ecRowsCol
        width: ecScroll.availableWidth
        spacing: 0   /* 10-04: 行间距改由每行 Layout.topMargin 控制, 以支持手势模式的分组面板 */
                                    Repeater {
                                        id: ecRowRepeater
                                        /* 10-04: 行模型固定 5 条。手势模式显示"常规滚动上 / 按压滚动上 /
                                         * 按下 / 常规滚动下 / 按压滚动下"(参考图); 常规模式隐藏 3/4 两行
                                         * (ColumnLayout 自动跳过不可见项), 仍是原"旋转上 / 按下 / 旋转下"
                                         * 三行、原样式, 完全不受影响。
                                         * grp = 分组号: 手势模式下同组相邻行合并成一块圆角面板
                                         * (0=上组 1=按下 2=下组)。
                                         * ⚠️ label 仅作标注; 行标题实际由 ecRowLabel(g) 按模式计算。 */
                                        model: [
                                            { label: "旋转 上",    g: 0, cap: 7,  grp: 0 },
                                            { label: "按压滚动上", g: 3, cap: 31, grp: 0 },
                                            { label: "按下",       g: 1, cap: 8,  grp: 1 },
                                            { label: "旋转 下",    g: 2, cap: 9,  grp: 2 },
                                            { label: "按压滚动下", g: 4, cap: 32, grp: 2 }
                                        ]
                                        delegate: ColumnLayout {
                                            id: ecRow
                                            width: parent.width
                                            spacing: 6

                                            property int gesture: modelData.g
                                            property int captureIdx: modelData.cap
                                            /* 10-04: 按压滚动上/下 两行仅手势模式显示 —— 常规模式保持
                                             * 原"旋转上/按下/旋转下"三行 UI 与样式完全不变。 */
                                            property bool scrollPressRow: gesture === 3 || gesture === 4
                                            visible: scrollPressRow ? (ecSettingsPopup.ecPressMode === 1) : true
                                            /* 10-04: 手势模式下面板分组(参考图: 上行组 / 按下 / 下行组)。
                                             * grpTop=组首(保留上圆角) grpBottom=组尾(保留下圆角),
                                             * 组内相接边圆角归零 -> 视觉合并为一块。 */
                                            property bool ecPanelMerge: ecSettingsPopup.ecPressMode === 1
                                            property bool grpTop: !ecPanelMerge || !scrollPressRow
                                            property bool grpBottom: !ecPanelMerge || gesture === 1 || scrollPressRow
                                            /* 组间距: ecRowsCol.spacing 置 0, 由跨组行的 topMargin 提供 8px */
                                            Layout.topMargin: (index > 0 &&
                                                               modelData.grp !== ecRowRepeater.model[index - 1].grp) ? 8 : 0
                                            /* 09-07: 当前行编辑目标是否为"EC 按下手势块"(单击/双击/长按 之一)。
                                             * 仅按下行(gesture===1)且处于手势模式时成立 —— 动作行/捕获/多媒体
                                             * 全部路由到 ecPressGesture 对应的 tap_/dbl_/lng_ 独立字段;
                                             * 否则照旧编辑 ec_cw/ec_press/ec_ccw 主键。 */
                                            property bool editingEcGesture: gesture === 1 && ecPressModeGesture
                                            /* 当前编辑目标语义值(action+keycode -> mouse_left/wheel_up/airmouse...) */
                                            function ecCurActionValue() {
                                                if (editingEcGesture)
                                                    return ecPressGestureActionValue(ecSettingsPopup.ecPressGesture)
                                                return ecActionValue(gesture)
                                            }
                                            /* 当前编辑目标键盘 keycode */
                                            function ecCurKeycode() {
                                                if (editingEcGesture)
                                                    return ecPressField(ecSettingsPopup.ecPressGesture, "keycode") || 0
                                                var m = ecGestureMap(gesture)
                                                return m ? (m.keycode || 0) : 0
                                            }

                                            /* 09-06: 两级选择 —— 动作大类下拉 + 鼠标细分触发同步 */
                                            function syncCombo() {
                                                var av = ecCurActionValue()
                                                var cat = (av === "wheel_up" || av === "wheel_down" || av === "mouse_left" || av === "mouse_right" || av === "mouse_middle" || av === "airmouse") ? "mouse" : av
                                                var tm = ecTypeCombo.model
                                                for (var i = 0; i < tm.length; i++) {
                                                    if (tm[i].v === cat) { ecTypeCombo.currentIndex = i; break }
                                                }
                                                var mm = ecTriggerCombo.model
                                                for (var j = 0; j < mm.length; j++) {
                                                    if (mm[j].v === av) { ecTriggerCombo.currentIndex = j; break }
                                                }
                                                var mk = ecCurKeycode()
                                                var mmm = ecMmCombo.model
                                                for (var k = 0; k < mmm.length; k++) {
                                                    if (mmm[k].v === mk) { ecMmCombo.currentIndex = k; break }
                                                }
                                            }

                                            /* 09-06: 分组背景矩形 —— 三手势各自成块区分
                                             * 10-04: 手势模式下同组相邻行合并(组内相接边不圆角) */
                                            Rectangle {
                                                Layout.fillWidth: true
                                                /* 10-05: 「按下」行原本写死 196/102 —— 那是按"含模式下拉"的内容算的。
                                                 * 模式下拉已移到标题行 ⇒ 改按**内容实测**定高, 否则会多出约 50px 空白
                                                 * (用户反馈"间距太宽")。非「按下」行仍用 46。 */
                                                Layout.preferredHeight: gesture === 1 ? (ecPressRowInner.implicitHeight + 20) : 46
                                                topLeftRadius: grpTop ? 8 : 0
                                                topRightRadius: grpTop ? 8 : 0
                                                bottomLeftRadius: grpBottom ? 8 : 0
                                                bottomRightRadius: grpBottom ? 8 : 0
                                                color: "#f4f7f5"
                                                border.color: "#e3eae5"
                                                border.width: 1

                                                ColumnLayout {
                                                    id: ecPressRowInner   /* 10-05: 外层 Rectangle 按本布局的 implicitHeight 定高 */
                                                    anchors.fill: parent
                                                    anchors.margins: 10
                                                    spacing: 8

                                                    /* 10-05: 「模式」已移到标题行(用户要求); 本行只剩分组标题。 */
                                                    RowLayout {
                                                        visible: gesture === 1
                                                        Layout.fillWidth: true
                                                        spacing: 6
                                                        Text { text: "按下"; font.pointSize: 13; font.bold: true; color: "#333" }
                                                    }

                                                    /* 09-07: 按下手势 模式=手势时 显示单击/双击/长按 三卡片(图2样式: 高 56 双行) */
                                                    RowLayout {
                                                        visible: gesture === 1 && ecSettingsPopup.ecPressMode === 1
                                                        Layout.fillWidth: true
                                                        spacing: 6
                                                        Repeater {
                                                            model: [
                                                                { label: "单击", g: 0 },
                                                                { label: "双击", g: 1 },
                                                                { label: "长按", g: 2 }
                                                            ]
                                                            delegate: Rectangle {
                                                                Layout.fillWidth: true
                                                                height: 56
                                                                radius: 8
                                                                color: ecSettingsPopup.ecPressGesture === modelData.g ? "#e6f5ec" : "#f7f9f8"
                                                                border.color: ecSettingsPopup.ecPressGesture === modelData.g ? "#07c160" : "#e3eae5"
                                                                border.width: 1
                                                                ColumnLayout {
                                                                    anchors.fill: parent
                                                                    anchors.margins: 4
                                                                    spacing: 0
                                                                    Text {
                                                                        Layout.alignment: Qt.AlignHCenter
                                                                        text: modelData.label
                                                                        font.pointSize: 12
                                                                        font.bold: ecSettingsPopup.ecPressGesture === modelData.g
                                                                        color: ecSettingsPopup.ecPressGesture === modelData.g ? "#07c160" : "#444"
                                                                    }
                                                                    Text {
                                                                        Layout.alignment: Qt.AlignHCenter
                                                                        Layout.topMargin: 2
                                                                        text: {
                                                                            var _ = refreshKeyTick
                                                                            return ecPressGestureStatusText(modelData.g)
                                                                        }
                                                                        font.pointSize: 10
                                                                        color: ecSettingsPopup.ecPressGesture === modelData.g ? "#07c160" : "#999"
                                                                    }
                                                                }
                                                                MouseArea {
                                                                    anchors.fill: parent
                                                                    onClicked: {
                                                                        ecSettingsPopup.ecPressGesture = modelData.g
                                                                        /* 编辑目标切到该手势, 动作行下拉同步到它的实际配置 */
                                                                        syncEcCombos()
                                                                    }
                                                                }
                                                            }
                                                        }
                                                    }


                                                    /* 09-07: 动作行 */
                                                    RowLayout {
                                                        Layout.fillWidth: true
                                                        Text {
                                                            text: gesture === 1 ? (editingEcGesture ? ["单击", "双击", "长按"][ecSettingsPopup.ecPressGesture] : "动作") : ecRowLabel(gesture)
                                                            font.pointSize: gesture === 1 ? 13 : 12
                                                            font.bold: true
                                                            color: "#333"
                                                            /* 10-04: 手势模式行标题较长("常规滚动上"/"按压滚动上"), 加宽标签列 */
                                                            Layout.preferredWidth: gesture === 1 ? 50 : (ecSettingsPopup.ecPressMode === 1 ? 66 : 46)
                                                        }
                                                Item { Layout.fillWidth: true }   /* 09-06: 控件组右对齐 */

                                                /* 动作大类: 鼠标 / 键盘组合 / 多媒体 / 无动作 */
                                                ComboBox {
                                                    id: ecTypeCombo
                                                    Layout.preferredWidth: 112
                                                    Layout.preferredHeight: 30
                                                    Layout.alignment: Qt.AlignVCenter
                                                    focusPolicy: Qt.NoFocus
                                                    font.pointSize: 10
                                                    model: ecSettingsPopup.typeModel
                                                    textRole: "label"
                                                    onActivated: {
                                                        var v = model[currentIndex].v
                                                        /* 09-07: 手势模式按下行 -> 编辑目标是 ecPressGesture 手势块 */
                                                        var eg = editingEcGesture ? ecSettingsPopup.ecPressGesture : -1
                                                        if (v === "mouse") {
                                                            var cur = ecCurActionValue()
                                                            var curIsMouse = (cur === "wheel_up" || cur === "wheel_down" || cur === "mouse_left" || cur === "mouse_right" || cur === "mouse_middle")
                                                            if (eg >= 0) {
                                                                keyConfig.setEcPressGestureAction(eg, "mouse")
                                                                if (curIsMouse) keyConfig.setEcPressGestureKeycode(eg, ecMouseKc(cur))
                                                            } else {
                                                                keyConfig.setEcKeyAction(gesture, curIsMouse ? cur : "mouse_left")
                                                            }
                                                            /* 同步触发细分下拉到当前 mouse 值 */
                                                            var mm = ecTriggerCombo.model
                                                            var av2 = ecCurActionValue()
                                                            for (var j = 0; j < mm.length; j++) {
                                                                if (mm[j].v === av2) { ecTriggerCombo.currentIndex = j; break }
                                                            }
                                                        } else if (v === "keyboard") {
                                                            if (eg >= 0) keyConfig.setEcPressGestureAction(eg, "keyboard")
                                                            else keyConfig.setEcKeyAction(gesture, "keyboard")
                                                            refreshKeyTick++
                                                        } else if (v === "multimedia") {
                                                            var mmm = ecMmCombo.model
                                                            var mk2 = ecCurKeycode()
                                                            var found = -1
                                                            for (var l = 0; l < mmm.length; l++) {
                                                                if (mmm[l].v === mk2) { found = l; break }
                                                            }
                                                            if (found < 0) found = 0
                                                            ecMmCombo.currentIndex = found
                                                            if (eg >= 0) keyConfig.setEcPressGestureMultimedia(eg, mmm[found].v)
                                                            else keyConfig.setEcKeyMultimedia(gesture, mmm[found].v)
                                                            refreshKeyTick++
                                                        } else {
                                                            if (eg >= 0) keyConfig.setEcPressGestureAction(eg, v)
                                                            else keyConfig.setEcKeyAction(gesture, v)
                                                        }
                                                        refreshKeyTick++
                                                    }

                                                /* 样式与 sleepMinCombo / lKeyActionCombo 同一设计语言 */
                                                background: Rectangle {
                                                    implicitWidth: 150
                                                    implicitHeight: 30
                                                    radius: 8
                                                    color: "white"
                                                    border.color: ecTypeCombo.hovered || ecTypeCombo.popup.visible ? "#07c160" : "#d0e0d8"
                                                    border.width: 1
                                                }
                                                contentItem: Text {
                                                    leftPadding: 8
                                                    rightPadding: 22
                                                    text: ecTypeCombo.displayText
                                                    font: ecTypeCombo.font
                                                    color: "#333"
                                                    verticalAlignment: Text.AlignVCenter
                                                    elide: Text.ElideRight
                                                }
                                                indicator: Canvas {
                                                    x: ecTypeCombo.width - width - 9
                                                    y: ecTypeCombo.height / 2 - height / 2
                                                    width: 9; height: 5
                                                    contextType: "2d"
                                                    onPaint: {
                                                        var ctx = getContext("2d")
                                                        ctx.reset()
                                                        ctx.moveTo(0, 0)
                                                        ctx.lineTo(width, 0)
                                                        ctx.lineTo(width / 2, height)
                                                        ctx.closePath()
                                                        ctx.fillStyle = "#07c160"
                                                        ctx.fill()
                                                    }
                                                }
                                                popup: Popup {
                                                    y: ecTypeCombo.height + 4
                                                    width: ecTypeCombo.width
                                                    padding: 3
                                                    implicitHeight: 30 * ecTypeCombo.model.length + 6
                                                    background: Rectangle { radius: 8; color: "white"; border.color: "#e0e0e0"; border.width: 1 }
                                                    contentItem: ListView {
                                                        clip: true
                                                        implicitHeight: 30 * ecTypeCombo.model.length
                                                        model: ecTypeCombo.delegateModel
                                                        currentIndex: ecTypeCombo.highlightedIndex
                                                        delegate: ItemDelegate {
                                                            width: ListView.view.width
                                                            height: 30
                                                            highlighted: ListView.isCurrentItem
                                                            hoverEnabled: true
                                                            onClicked: ecTypeCombo.activated(index)
                                                            contentItem: Text {
                                                                text: ecTypeCombo.model[index].label
                                                                color: parent.highlighted ? "#07c160" : "#333"
                                                                font: ecTypeCombo.font
                                                                verticalAlignment: Text.AlignVCenter
                                                                elide: Text.ElideRight
                                                                leftPadding: 6
                                                            }
                                                            background: Rectangle {
                                                                color: parent.highlighted ? "#e6f5ec" : (parent.hovered ? "#f4f9f6" : "transparent")
                                                                radius: 6
                                                            }
                                                        }
                                                    }
                                                }
                                            }

                                            /* 09-06: 具体触发区 —— 鼠标大类显示细分下拉; 键盘组合显示当前快捷键 */
                                            Item {
                                                Layout.preferredWidth: 112
                                                Layout.preferredHeight: 30
                                                Layout.alignment: Qt.AlignVCenter

                                                ComboBox {
                                                    id: ecTriggerCombo
                                                    anchors.fill: parent
                                                    focusPolicy: Qt.NoFocus
                                                    font.pointSize: 10
                                                    model: ecSettingsPopup.mouseTriggerModel
                                                    textRole: "label"
                                                    visible: ecTypeCombo.currentIndex === 0
                                                    onActivated: {
                                                        var v = model[currentIndex].v
                                                        var eg = editingEcGesture ? ecSettingsPopup.ecPressGesture : -1
                                                        if (eg >= 0) {
                                                            /* 手势块: 空中鼠标是独立 action; 其余细分走 mouse+keycode */
                                                            if (v === "airmouse")
                                                                keyConfig.setEcPressGestureAction(eg, "airmouse")
                                                            else {
                                                                keyConfig.setEcPressGestureAction(eg, "mouse")
                                                                keyConfig.setEcPressGestureKeycode(eg, ecMouseKc(v))
                                                            }
                                                        } else {
                                                            keyConfig.setEcKeyAction(gesture, v)
                                                        }
                                                        refreshKeyTick++
                                                    }
                                                    background: Rectangle {
                                                        implicitHeight: 30
                                                        radius: 8
                                                        color: "white"
                                                        border.color: ecTriggerCombo.hovered || ecTriggerCombo.popup.visible ? "#07c160" : "#d0e0d8"
                                                        border.width: 1
                                                    }
                                                    contentItem: Text {
                                                        leftPadding: 8
                                                        rightPadding: 22
                                                        text: ecTriggerCombo.displayText
                                                        font: ecTriggerCombo.font
                                                        color: "#333"
                                                        verticalAlignment: Text.AlignVCenter
                                                        elide: Text.ElideRight
                                                    }
                                                    indicator: Canvas {
                                                        x: ecTriggerCombo.width - width - 9
                                                        y: ecTriggerCombo.height / 2 - height / 2
                                                        width: 9; height: 5
                                                        contextType: "2d"
                                                        onPaint: {
                                                            var ctx = getContext("2d")
                                                            ctx.reset()
                                                            ctx.moveTo(0, 0)
                                                            ctx.lineTo(width, 0)
                                                            ctx.lineTo(width / 2, height)
                                                            ctx.closePath()
                                                            ctx.fillStyle = "#07c160"
                                                            ctx.fill()
                                                        }
                                                    }
                                                    popup: Popup {
                                                        y: ecTriggerCombo.height + 4
                                                        width: ecTriggerCombo.width
                                                        padding: 3
                                                        implicitHeight: 30 * ecTriggerCombo.model.length + 6
                                                        background: Rectangle { radius: 8; color: "white"; border.color: "#e0e0e0"; border.width: 1 }
                                                        contentItem: ListView {
                                                            clip: true
                                                            implicitHeight: 30 * ecTriggerCombo.model.length
                                                            model: ecTriggerCombo.delegateModel
                                                            currentIndex: ecTriggerCombo.highlightedIndex
                                                            delegate: ItemDelegate {
                                                                width: ListView.view.width
                                                                height: 30
                                                                highlighted: ListView.isCurrentItem
                                                                hoverEnabled: true
                                                                onClicked: ecTriggerCombo.activated(index)
                                                                contentItem: Text {
                                                                    text: ecTriggerCombo.model[index].label
                                                                    color: parent.highlighted ? "#07c160" : "#333"
                                                                    font: ecTriggerCombo.font
                                                                    verticalAlignment: Text.AlignVCenter
                                                                    elide: Text.ElideRight
                                                                    leftPadding: 6
                                                                }
                                                                background: Rectangle {
                                                                    color: parent.highlighted ? "#e6f5ec" : (parent.hovered ? "#f4f9f6" : "transparent")
                                                                    radius: 6
                                                                }
                                                            }
                                                        }
                                                    }
                                                }

                                                /* 多媒体: 下拉选择具体多媒体键 */
                                                ComboBox {
                                                    id: ecMmCombo
                                                    anchors.fill: parent
                                                    focusPolicy: Qt.NoFocus
                                                    font.pointSize: 10
                                                    model: ecSettingsPopup.mmModel
                                                    textRole: "label"
                                                    visible: ecTypeCombo.currentIndex === 2
                                                    onActivated: {
                                                        var eg = editingEcGesture ? ecSettingsPopup.ecPressGesture : -1
                                                        if (eg >= 0)
                                                            keyConfig.setEcPressGestureMultimedia(eg, model[currentIndex].v)
                                                        else
                                                            keyConfig.setEcKeyMultimedia(gesture, model[currentIndex].v)
                                                        refreshKeyTick++
                                                    }
                                                    background: Rectangle {
                                                        implicitHeight: 30
                                                        radius: 8
                                                        color: "white"
                                                        border.color: ecMmCombo.hovered || ecMmCombo.popup.visible ? "#07c160" : "#d0e0d8"
                                                        border.width: 1
                                                    }
                                                    contentItem: Text {
                                                        leftPadding: 8
                                                        rightPadding: 22
                                                        text: ecMmCombo.displayText
                                                        font: ecMmCombo.font
                                                        color: "#333"
                                                        verticalAlignment: Text.AlignVCenter
                                                        elide: Text.ElideRight
                                                    }
                                                    indicator: Canvas {
                                                        x: ecMmCombo.width - width - 9
                                                        y: ecMmCombo.height / 2 - height / 2
                                                        width: 9; height: 5
                                                        contextType: "2d"
                                                        onPaint: {
                                                            var ctx = getContext("2d")
                                                            ctx.reset()
                                                            ctx.moveTo(0, 0)
                                                            ctx.lineTo(width, 0)
                                                            ctx.lineTo(width / 2, height)
                                                            ctx.closePath()
                                                            ctx.fillStyle = "#07c160"
                                                            ctx.fill()
                                                        }
                                                    }
                                                    popup: Popup {
                                                        y: ecMmCombo.height + 4
                                                        width: ecMmCombo.width
                                                        padding: 3
                                                        implicitHeight: Math.min(30 * ecMmCombo.model.length + 6, 300)
                                                        background: Rectangle { radius: 8; color: "white"; border.color: "#e0e0e0"; border.width: 1 }
                                                        contentItem: ListView {
                                                            clip: true
                                                            implicitHeight: Math.min(30 * ecMmCombo.model.length, 294)
                                                            model: ecMmCombo.delegateModel
                                                            currentIndex: ecMmCombo.highlightedIndex
                                                            delegate: ItemDelegate {
                                                                width: ListView.view.width
                                                                height: 30
                                                                highlighted: ListView.isCurrentItem
                                                                hoverEnabled: true
                                                                onClicked: ecMmCombo.activated(index)
                                                                contentItem: Text {
                                                                    text: ecMmCombo.model[index].label
                                                                    color: parent.highlighted ? "#07c160" : "#333"
                                                                    font: ecMmCombo.font
                                                                    verticalAlignment: Text.AlignVCenter
                                                                    elide: Text.ElideRight
                                                                    leftPadding: 6
                                                                }
                                                                background: Rectangle {
                                                                    color: parent.highlighted ? "#e6f5ec" : (parent.hovered ? "#f4f9f6" : "transparent")
                                                                    radius: 6
                                                                }
                                                            }
                                                        }
                                                    }
                                                }

                                                /* 键盘组合: 点框进入捕获; 捕获中时边框变绿, 文本显示"捕获中…" */
                                                Rectangle {
                                                    id: ecKbBox
                                                    anchors.fill: parent
                                                    visible: ecTypeCombo.currentIndex === 1
                                                    radius: 8
                                                    color: "white"
                                                    property bool hovered: false
                                                    /* 09-07: 手势模式按下行 -> 捕获目标 = 28+ecPressGesture(startEcPressGestureCapture) */
                                                    property int ecCapNow: editingEcGesture ? (28 + ecSettingsPopup.ecPressGesture) : captureIdx
                                                    border.color: (keyConfig.captureIndex === ecKbBox.ecCapNow || ecKbBox.hovered) ? "#07c160" : "#d0e0d8"
                                                    border.width: 1
                                                    Text {
                                                        anchors.verticalCenter: parent.verticalCenter
                                                        leftPadding: 8
                                                        rightPadding: 8
                                                        text: {
                                                            var _ = refreshKeyTick
                                                            if (keyConfig.captureIndex === ecKbBox.ecCapNow) return "捕获中…"
                                                            var mod = 0, kc = 0
                                                            if (editingEcGesture) {
                                                                mod = ecPressField(ecSettingsPopup.ecPressGesture, "modifier") || 0
                                                                kc = ecPressField(ecSettingsPopup.ecPressGesture, "keycode") || 0
                                                            } else {
                                                                var m = ecGestureMap(gesture)
                                                                mod = m ? (m.modifier || 0) : 0
                                                                kc = m ? (m.keycode || 0) : 0
                                                            }
                                                            var t = formatShortcut(mod, kc)
                                                            return t.length > 0 ? t : "未设置"
                                                        }
                                                        font.pointSize: 10
                                                        color: "#333"
                                                        elide: Text.ElideRight
                                                    }
                                                    MouseArea {
                                                        anchors.fill: parent
                                                        hoverEnabled: true
                                                        onEntered: ecKbBox.hovered = true
                                                        onExited: ecKbBox.hovered = false
                                                        onClicked: {
                                                            if (keyConfig.captureIndex === ecKbBox.ecCapNow) {
                                                                keyConfig.cancelCapture()
                                                            } else if (editingEcGesture) {
                                                                keyConfig.startEcPressGestureCapture(ecSettingsPopup.ecPressGesture)
                                                            } else {
                                                                keyConfig.startCapture(captureIdx)
                                                            }
                                                        }
                                                    }
                                                }
                                            }
                                                                                                }   /* 动作行 RowLayout 收尾 */
                                                }   /* 内嵌 ColumnLayout 收尾(09-06) */
                                            }
                                                /* 09-06: 选中"空中鼠标"时的行内设置(全局配置, 与 KEY L1 共用同一份) */
                                            ColumnLayout {
                                                visible: ecCurActionValue() === "airmouse"
                                                Layout.fillWidth: true
                                                spacing: 8

                                                Text { text: "空中鼠标(移动触发方式)"; font.pointSize: 11; font.bold: true; color: "#333"; Layout.leftMargin: 4 }

                                                Text { text: "触发方式"; font.pointSize: 11; color: "#666"; Layout.leftMargin: 4 }
                                                RowLayout { spacing: 8; Layout.fillWidth: true
                
                                                    Rectangle { Layout.fillWidth: true; height: 32; radius: 6
                                                        color: keyConfig.airMouseMode === 0 ? "#e6f5ec" : "#f2f3f5"
                                                        border.color: keyConfig.airMouseMode === 0 ? "#07c160" : "transparent"; border.width: 1
                                                        Text { anchors.centerIn: parent; text: "单击切换"; font.pointSize: 11; color: keyConfig.airMouseMode === 0 ? "#07c160" : "#666" }
                                                        MouseArea { anchors.fill: parent; onClicked: keyConfig.setAirMouseMode(0) }
                                                    }
                                                    Rectangle { Layout.fillWidth: true; height: 32; radius: 6
                                                        color: keyConfig.airMouseMode === 1 ? "#e6f5ec" : "#f2f3f5"
                                                        border.color: keyConfig.airMouseMode === 1 ? "#07c160" : "transparent"; border.width: 1
                                                        Text { anchors.centerIn: parent; text: "按住移动"; font.pointSize: 11; color: keyConfig.airMouseMode === 1 ? "#07c160" : "#666" }
                                                        MouseArea { anchors.fill: parent; onClicked: keyConfig.setAirMouseMode(1) }
                                                    }
                                                }

                                                Text { text: "单击切换：触发一次开始移动，再触发一次停止（编码器=转一档切换)\n按住移动：按住时移动，松开立即停止（编码器旋转手势不适用按住）"; font.pointSize: 10; color: "#999"; Layout.fillWidth: true; wrapMode: Text.WordWrap; Layout.leftMargin: 4 }

                                                Text { text: "灵敏度"; font.pointSize: 11; color: "#666"; Layout.leftMargin: 4 }
                                                RowLayout { spacing: 8; Layout.fillWidth: true
                                                    Rectangle { Layout.fillWidth: true; height: 32; radius: 6
                                                        color: keyConfig.airMouseSpeed === 0 ? "#e6f5ec" : "#f2f3f5"
                                                        border.color: keyConfig.airMouseSpeed === 0 ? "#07c160" : "transparent"; border.width: 1
                                                        Text { anchors.centerIn: parent; text: "慢"; font.pointSize: 11; color: keyConfig.airMouseSpeed === 0 ? "#07c160" : "#666" }
                                                        MouseArea { anchors.fill: parent; onClicked: keyConfig.setAirMouseSpeed(0) }
                                                    }
                                                    Rectangle { Layout.fillWidth: true; height: 32; radius: 6
                                                        color: keyConfig.airMouseSpeed === 1 ? "#e6f5ec" : "#f2f3f5"
                                                        border.color: keyConfig.airMouseSpeed === 1 ? "#07c160" : "transparent"; border.width: 1
                                                        Text { anchors.centerIn: parent; text: "中"; font.pointSize: 11; color: keyConfig.airMouseSpeed === 1 ? "#07c160" : "#666" }
                                                        MouseArea { anchors.fill: parent; onClicked: keyConfig.setAirMouseSpeed(1) }
                                                    }
                                                    Rectangle { Layout.fillWidth: true; height: 32; radius: 6
                                                        color: keyConfig.airMouseSpeed === 2 ? "#e6f5ec" : "#f2f3f5"
                                                        border.color: keyConfig.airMouseSpeed === 2 ? "#07c160" : "transparent"; border.width: 1
                                                        Text { anchors.centerIn: parent; text: "快"; font.pointSize: 11; color: keyConfig.airMouseSpeed === 2 ? "#07c160" : "#666" }
                                                        MouseArea { anchors.fill: parent; onClicked: keyConfig.setAirMouseSpeed(2) }
                                                    }
                                                }
                                            }}
                                                                    }

                                    }
                                }



                                                                Text {
                                                                    text: "设置完成后请点击上方“写入”按钮保存到当前蓝牙槽位"
                                                                    font.pointSize: 10; color: "#999"
                                                                    Layout.fillWidth: true
                                                                    wrapMode: Text.WordWrap
                                                                }
                                                            }
                                                        }

                                                        /* Status */
                                                        Rectangle {
                                                            Layout.fillWidth: true; height: cfgStatusMsg.length > 0 ? 32 : 0; color: "white"
                                                            visible: cfgStatusMsg.length > 0
                                                            Rectangle { anchors.top: parent.top; width: parent.width; height: 1; color: "#e8e8e8" }
                                                            /* 08-27: 失败信息红色显示(原固定绿色, 失败也被当成成功提示) */
                                                            Text { anchors.centerIn: parent; text: cfgStatusMsg; font.pointSize: 11;
                                                                   color: cfgStatusError ? "#f56c6c" : "#07c160" }
                                                        }

                                                    }

                                                    /* Light Monitor Tab (removed) */

                                                    /* Status Tab - WorkBuddy 运行状态 */
                                                    ScrollView {
                                                        Layout.fillWidth: true
                                                        Layout.fillHeight: true
                                                        ScrollBar.horizontal.policy: ScrollBar.AlwaysOff
                                                        ScrollBar.vertical.policy: ScrollBar.AlwaysOff
                                                        contentWidth: availableWidth

                                                        ColumnLayout {
                                                            id: statusCol
                                                            width: parent.width - 32
                                                            x: 16
                                                            spacing: 16

                                                            /* 设备连接状态：USB + 蓝牙 + 电量（合并为一行） */
                                                            Rectangle {
                                                                Layout.fillWidth: true; height: 56; color: "white"; radius: 8
                                                                Rectangle { anchors.fill: parent; anchors.margins: -2; color: "#20e0e0e0"; radius: parent.radius + 2; z: -1 }
                                                                Rectangle { anchors.fill: parent; anchors.margins: -4; color: "#0ce0e0e0"; radius: parent.radius + 4; z: -1 }
                                                                RowLayout { anchors.fill: parent; anchors.margins: 12; spacing: 8
                                                                    Rectangle {
                                                                        width: 10; height: 10; radius: 5
                                                                        color: (portDetector.deviceConnected || lightMonitor.bleConnected) ? "#07c160" : "#cccccc"
                                                                    }
                                                                    Text {
                                                                        text: {
                                                                            var usb = portDetector.deviceConnected
                                                                            var ble = lightMonitor.bleConnected
                                                                            if (!usb && !ble)
                                                                                return "未连接"
                                                                            var name = ""
                                                                            if (usb)
                                                                                name = portDetector.deviceName + (portDetector.deviceVersion ? (" v" + portDetector.deviceVersion) : "")
                                                                            else if (ble)
                                                                                name = (lightMonitor.bleDeviceName ? lightMonitor.bleDeviceName : "VibeKey") + (lightMonitor.bleDeviceVersion ? (" v" + lightMonitor.bleDeviceVersion) : "")
                                                                            return "已连接" + (name ? "  " + name : "")
                                                                        }
                                                                        font.pointSize: 11
                                                                        color: (portDetector.deviceConnected || lightMonitor.bleConnected) ? "#333" : "#999"
                                                                    }
                                                                    Item { Layout.fillWidth: true }
                                                                    Loader { sourceComponent: batteryIndicator; Layout.alignment: Qt.AlignVCenter; visible: root.effectiveBatteryPercent >= 0 }
                                                                }
                                                            }

                                                            /* WorkBuddy 运行状态面板 */
                                                            Rectangle {
                                                                Layout.fillWidth: true
                                                                Layout.preferredHeight: wbCol.implicitHeight + 32
                                                                color: "white"; radius: 8
                                                                Rectangle { anchors.fill: parent; anchors.margins: -2; color: "#20e0e0e0"; radius: parent.radius + 2; z: -1 }
                                                                Rectangle { anchors.fill: parent; anchors.margins: -4; color: "#0ce0e0e0"; radius: parent.radius + 4; z: -1 }
                                                                ColumnLayout {
                                                                    id: wbCol
                                                                    anchors.left: parent.left; anchors.right: parent.right; anchors.top: parent.top
                                                                    anchors.margins: 16; spacing: 12
                                                                    Text { text: "WorkBuddy 运行状态"; font.pointSize: 13; font.bold: true; color: "#333" }

                                                                    RowLayout { spacing: 14
                                                                        Rectangle {
                                                                            id: wbLight
                                                                            width: 56; height: 56; radius: 28
                                                                            property real breathe: 1
                                                                            property real blink: 1
                                                                            opacity: {
                                                if (workbuddyMonitor.status === "await" || workbuddyMonitor.status === "alert") return blink
                                                if (workbuddyMonitor.active) return breathe
                                                return 1
                                            }
                                            color: {
                                                switch (workbuddyMonitor.status) {
                                                    case "busy": return "#f56c6c"
                                                    case "error": return "#f56c6c"
                                                    case "alert": return "#f5222d"
                                                    case "await": return "#faad14"
                                                    case "idle": return "#07c160"
                                                    default: return "#e8e8e8"
                                                }
                                            }
                                            SequentialAnimation on breathe {
                                                loops: Animation.Infinite
                                                running: workbuddyMonitor.active
                                                         && workbuddyMonitor.status !== "await"
                                                         && workbuddyMonitor.status !== "alert"
                                                NumberAnimation { from: 1.0; to: 0.45; duration: 700 }
                                                NumberAnimation { from: 0.45; to: 1.0; duration: 700 }
                                            }
                                            SequentialAnimation on blink {
                                                loops: Animation.Infinite
                                                running: workbuddyMonitor.status === "await"
                                                         || workbuddyMonitor.status === "alert"
                                                NumberAnimation { from: 1.0; to: 0.15; duration: 350 }
                                                NumberAnimation { from: 0.15; to: 1.0; duration: 350 }
                                            }
                                        }
                                                                            ColumnLayout { spacing: 4
                                                                            Text {
                                                                                text: {
                                                                                    switch (workbuddyMonitor.status) {
                                                                                        case "busy": return "工作中"
                                                                                        case "error": return "出错"
                                                                                        case "alert": return "等待高风险确认"
                                                                                        case "await": return "等待审批/输入"
                                                                                        case "idle": return "空闲"
                                                                                        default: return "未知"
                                                                                    }
                                                                                }
                                                                                font.pointSize: 16; font.bold: true
                                                                                color: workbuddyMonitor.status === "alert" ? "#f5222d"
                                                                                     : (workbuddyMonitor.status === "await" ? "#faad14" : "#333")
                                                                            }
                                                                            Text {
                                                                                text: workbuddyMonitor.status === "alert" ? "WorkBuddy 正在等待你确认高风险操作"
                                                                                     : (workbuddyMonitor.status === "await" ? "WorkBuddy 正在等待你的选择"
                                                                                     : (workbuddyMonitor.active ? "WorkBuddy 正在运行任务" : "WorkBuddy 未运行"))
                                                                                font.pointSize: 11; color: "#999"
                                                                            }
                                                                        }
                                        Item { Layout.fillWidth: true }
                                    }

                                    RowLayout { spacing: 8
                                        Text { text: "当前任务"; font.pointSize: 12; color: "#888"; Layout.preferredWidth: 64 }
                                        Text {
                                            Layout.fillWidth: true; maximumLineCount: 1; elide: Text.ElideRight
                                            text: workbuddyMonitor.taskName ? workbuddyMonitor.taskName : "—"
                                            font.pointSize: 12; color: "#333"
                                        }
                                    }

                                    RowLayout { spacing: 12
                                        Text { text: "联动 VibeKey 灯"; font.pointSize: 12; color: "#333" }
                                        Item { Layout.fillWidth: true }
                                        Rectangle {
                                            width: 50; height: 26; radius: 13
                                            color: workbuddyMonitor.deviceLinked ? "#07c160" : "#ccc"
                                            Text { anchors.centerIn: parent; text: workbuddyMonitor.deviceLinked ? "开" : "关"; font.pointSize: 10; color: "white" }
                                            MouseArea { anchors.fill: parent; onClicked: workbuddyMonitor.setDeviceLinked(!workbuddyMonitor.deviceLinked) }
                                        }
                                    }

                                }
                            }

                            Item { Layout.fillWidth: true; height: 8 }

                            /* 手动测试 + C1/C2/C3 RGB —— 合并成**同一张卡**（10-05 用户要求：它俩是一类，
                               原来拆成两张卡，看起来被分开了；现在同一张卡内分区显示）。 */
                            Rectangle {
                                id: ledTestPanel
                                Layout.fillWidth: true; height: mergedLedCol.implicitHeight + 24
                                color: "white"; radius: 8
                                Rectangle { anchors.fill: parent; anchors.margins: -2; color: "#20e0e0e0"; radius: parent.radius + 2; z: -1 }
                                Rectangle { anchors.fill: parent; anchors.margins: -4; color: "#0ce0e0e0"; radius: parent.radius + 4; z: -1 }

                                // 三颗灯的临时测试态（颜色 0xRRGGBB / 亮度% / 点亮开关）
                                property var testColor: [0xFF2020, 0x20FF20, 0x2020FF]
                                // ★ 10-04: 初始亮度取"固件上限"而非硬编码 —— 上限变了这里自动跟随，
                                // 避免出现"初始值 > 上限"这种自相矛盾的状态。
                                property var testBri: [keyConfig.ledBrightnessHardwareCap(),
                                                      keyConfig.ledBrightnessHardwareCap(),
                                                      keyConfig.ledBrightnessHardwareCap()]
                                property var testOn:  [true, true, true]
                                // 固件锁死的上限；只用于显示提示，不参与裁剪
                                readonly property int hwCap: keyConfig.ledBrightnessHardwareCap()
                                /* 10-05 用户要求：可调范围下限改为 1%（原来 0%，此时灯已完全压黑，
                                   没有意义）。滑块量程 [hwMin, hwCap] = [1, 15]。 */
                                readonly property int hwMin: 1

                                function applyLed(i) {
                                    if (ledTestPanel.testOn[i])
                                        keyConfig.ledTestSet(i,
                                            (ledTestPanel.testColor[i] >> 16) & 0xFF,
                                            (ledTestPanel.testColor[i] >> 8) & 0xFF,
                                            ledTestPanel.testColor[i] & 0xFF,
                                            ledTestPanel.testBri[i])
                                    else
                                        keyConfig.ledTestClear(i)
                                }

                                ColumnLayout { id: mergedLedCol; anchors.fill: parent; anchors.margins: 12; spacing: 8
                                    /* 第 1 行：标题 + 通道（通道从原来靠右挪到标题旁，见用户草稿） */
                                    RowLayout { spacing: 8
                                        Text { text: "手动测试"; font.pointSize: 11; color: "#888" }
                                        Text {
                                            text: {
                                                switch (lightMonitor.channel) {
                                                    case "ble": return "通道：蓝牙"
                                                    case "usb": return "通道：USB"
                                                    default: return "通道：无设备"
                                                }
                                            }
                                            font.pointSize: 11
                                            color: lightMonitor.channel === "none" ? "#999" : "#07c160"
                                        }
                                        Item { Layout.fillWidth: true }
                                    }
                                    /* 第 2 行：状态灯分组标题 + 四个按钮 */
                                    RowLayout { spacing: 8
                                        Text { text: "LED1/LED2/LED3"; font.pointSize: 11; color: "#888" }
                                        Item { Layout.fillWidth: true }
                                        // 状态灯（原黄/绿/红/关灯）—— 走 ble_led_service→rgb_led_set_color，
                                        // **不受固件 15% 限幅影响**，10-04 用户明确要求"状态灯先不动"。
                                        Rectangle { width: 50; height: 28; radius: 14; color: t1Ma.pressed ? "#e6f5ec" : "transparent"; border.color: "#FFAA00"; border.width: 1
                                            Text { anchors.centerIn: parent; text: "黄灯"; font.pointSize: 10; color: "#FFAA00" }
                                            MouseArea { id: t1Ma; anchors.fill: parent; onClicked: lightMonitor.sendState(1) }
                                        }
                                        Rectangle { width: 50; height: 28; radius: 14; color: t2Ma.pressed ? "#e6f5ec" : "transparent"; border.color: "#00FF00"; border.width: 1
                                            Text { anchors.centerIn: parent; text: "绿灯"; font.pointSize: 10; color: "#00FF00" }
                                            MouseArea { id: t2Ma; anchors.fill: parent; onClicked: lightMonitor.sendState(2) }
                                        }
                                        Rectangle { width: 50; height: 28; radius: 14; color: t3Ma.pressed ? "#e6f5ec" : "transparent"; border.color: "#FF0000"; border.width: 1
                                            Text { anchors.centerIn: parent; text: "红灯"; font.pointSize: 10; color: "#FF0000" }
                                            MouseArea { id: t3Ma; anchors.fill: parent; onClicked: lightMonitor.sendState(3) }
                                        }
                                        Rectangle { width: 50; height: 28; radius: 14; color: t4Ma.pressed ? "#e6f5ec" : "transparent"; border.color: "#888"; border.width: 1
                                            Text { anchors.centerIn: parent; text: "关灯"; font.pointSize: 10; color: "#888" }
                                            MouseArea { id: t4Ma; anchors.fill: parent; onClicked: lightMonitor.sendState(0) }
                                        }
                                    }
                                    /* ---- 下面同属"手动测灯"：C1/C2/C3 的 RGB ---- */
                                    /* 10-05 用户要求：两组之间加一条**小小的横线**做分隔。
                                       用 Layout.preferredHeight 而非 height —— 它是布局子项，
                                       设 height 会被 lint 判为 undefined behavior。 */
                                    Rectangle {
                                        Layout.fillWidth: true
                                        Layout.preferredHeight: 1
                                        color: "#ececec"
                                    }
                                    RowLayout { spacing: 8
                                        /* 10-05 用户要求：这个分区标题的颜色与上方 LED1/LED2/LED3 一致（#888）。 */
                                        Text { text: "C1/C2/C3 RGB"; font.pointSize: 12; color: "#888" }
                                        Item { Layout.fillWidth: true }
                                        Rectangle { width: 56; height: 24; radius: 12; color: "#f0f0f0"
                                            Text { anchors.centerIn: parent
                                                   text: "全部熄灭"; font.pointSize: 10; color: "#666" }
                                            MouseArea { anchors.fill: parent
                                                /* ★ 10-05 修复"全部熄灭灭不掉"：原来这里是
                                                   for (i=0..2) keyConfig.ledTestClear(i) —— 下发通道是
                                                   合并式的（只保留最后一次的 idx），循环会被互相覆盖，
                                                   实际只发出 C3。改调 ledTestClearAll()，
                                                   C++ 侧排队逐灯补发（见 KeyConfigManager::ledTestClearAll）。 */
                                                onClicked: {
                                                    ledTestPanel.testOn = [false, false, false]
                                                    keyConfig.ledTestClearAll()
                                                } }
                                        }
                                    }

                                    // ---- 三颗灯各自独立：颜色快选 + 亮度 + 开关 ----
                                    Repeater {
                                        model: 3
                                        delegate: RowLayout {
                                            id: ledRow
                                            spacing: 6
                                            property int ledIdx: index
                                            /* ⚠️ 10-04 崩溃修复：col/bri 由 var 改为 **int** ——
                                               property var 绑定 JS 数组元素时可能得到 undefined/QVariant，
                                               后续参与比较/移位运算会触发 Qt6 Debug 的 Q_ASSERT → abort()。
                                               显式 int 语义明确（0xRRGGBB 数值 / 0~100 百分比）。 */
                                            property int col: ledTestPanel.testColor[index]
                                            property int bri: ledTestPanel.testBri[index]
                                            /* ★ 10-05 用户要求：所有色块的浓淡都跟着「亮度」走。
                                               取**相对上限**的比例（亮度 15% / 上限 15% = 1.0 最实；
                                               调低亮度则色块整体变淡）—— 见下方 8 色块。 */
                                            property real briOpacity: ledTestPanel.hwCap > 0
                                                                      ? Math.max(0, Math.min(1, bri / ledTestPanel.hwCap))
                                                                      : 1.0

                                            /* ★ 10-05 按用户草稿重排：不再分左右半区，整行平铺 ——
                                               名称 + 8色块 + 色值 紧凑靠左，亮度条弹性撑满，百分比贴最右。 */
                                            Text { text: "C" + (ledIdx + 1)
                                                   font.pointSize: 11; font.bold: true; color: "#666"
                                                   Layout.preferredWidth: 18 }
                                            /* 8 色快选。
                                             * ⚠️ 这里必须用 Row，**不能**把 Repeater 直接放进
                                             * 上面的 RowLayout —— Repeater 不是 LayoutItem，
                                             * RowLayout 给它分配不到位置，色块会整个不显示
                             * （10-04 实测：只剩滑块和亮/灭；且 Repeater 会挤占滑块宽度）。
                                             * Row 会正常管理 Repeater 生成的子项。 */
                                            Row {
                                                spacing: 3
                                                Repeater {
                                                    model: [0xFF0000, 0x00FF00, 0x0000FF, 0xFFFF00,
                                                            0xFF00FF, 0x00FFFF, 0xFFFFFF, 0xFF8000]
                                                    delegate: Rectangle {
                                                        width: 17; height: 17; radius: 3
                                                        /* ⚠️ 10-04 色块全白修复：原用 Qt.red()/Qt.green()/Qt.blue() —— 这三个
                                               **不是 QML 的 Qt.* 全局函数**(QColor 的方法, QML 里取不到),
                                               返回 undefined ⇒ Qt.rgba(undefined,...) ⇒ 渲染成白色。
                                               改用项目里已验证可用的位运算(见 1698/2050/4232 行)。 */
                                                        /* ★ 10-05 用户要求：调亮度时**只有色块内部的填充**跟着变淡，
                                                           "选中且亮"的那圈深色框**不要跟着变** ⇒
                                                           把 briOpacity 作为**填充色的 alpha**，
                                                           而不是整个 Rectangle 的 opacity（那样边框也会一起淡）。 */
                                                        color: Qt.rgba(((modelData >> 16) & 0xFF) / 255,
                                                                      ((modelData >> 8) & 0xFF) / 255,
                                                                      (modelData & 0xFF) / 255,
                                                                      ledRow.briOpacity)
                                                        /* ⚠️ 10-04 崩溃修复：原写作 (modelData === col) —— col 是 property var
                                                 (QML 里可能是 QVariant)，与数字做 === 严格比较
                                                 在 Qt6 Debug 下触发 Q_ASSERT → abort()。
                                                 ledRow.col 本身就是 0xRRGGBB 数字，直接比大小即可。 */
                                                        /* ★ 10-05 用户要求：选中色块的"框"表示亮/灭 ——
                                                           - 选中 且 亮 → 深色粗框
                                                           - 选中 但 灭 → 无框
                                                           - 未选中     → 浅灰细框（保证色块本身看得见）
                                                           （原来亮/灭靠右侧那颗重复的指示块表示，已删除。）
                                                           ★ 10-05 追加修复：纯白色块(#FFFFFF)在**白色卡片**上
                                                           "没有框就完全看不见"（用户反馈"白色的灰框看不见了"）
                                                           ⇒ 给它**永久保留一圈灰框**(#bdbdbd)，与亮/灭、选中与否无关。 */
                                                        property bool isWhite: (modelData & 0xFFFFFF) === 0xFFFFFF
                                                        border.color: (modelData != ledRow.col)
                                                                      ? (isWhite ? "#bdbdbd" : "#dcdcdc")
                                                                      : (ledTestPanel.testOn[ledRow.ledIdx]
                                                                         ? "#333"
                                                                         : (isWhite ? "#bdbdbd" : "transparent"))
                                                        border.width: (modelData != ledRow.col)
                                                                      ? 1
                                                                      : (ledTestPanel.testOn[ledRow.ledIdx]
                                                                         ? 2
                                                                         : (isWhite ? 1 : 0))
                                                        /* 10-05: 原来这里是 `opacity: ledRow.briOpacity`（连边框一起淡）——
                                                           已改为只把 briOpacity 混进上面的填充色 alpha，边框保持恒定。 */
                                                        MouseArea { anchors.fill: parent
                                                            /* ★ 10-05 用户要求：色块点击 = 亮/灭开关。
                                                               - 点**当前已选**色块 → 切换 亮↔灭；
                                                               - 点**其它**色块 → 换色并点亮。
                                                               （原最右侧「亮/灭」小按钮已删除，开关语义并入此处） */
                                                            onClicked: {
                                                                if (modelData == ledRow.col) {
                                                                    var t = ledTestPanel.testOn.slice()
                                                                    t[ledIdx] = !t[ledIdx]
                                                                    ledTestPanel.testOn = t
                                                                } else {
                                                                    var c = ledTestPanel.testColor.slice(); c[ledIdx] = modelData
                                                                    ledTestPanel.testColor = c
                                                                    var u = ledTestPanel.testOn.slice(); u[ledIdx] = true
                                                                    ledTestPanel.testOn = u
                                                                }
                                                                ledTestPanel.applyLed(ledIdx)
                                                            }
                                                        }
                                                    }
                                                }
                                            }
                                            /* 十六进制色值 ——
                                             * 用户要能直接看到"现在是什么颜色"和可下发的色值，
                                             * 不必靠猜（10-04 反馈：没见颜色的值）。
                                             * ★ 10-05 用户要求：原来这里那颗"当前颜色指示块"与左侧
                                             *   被选中的色块**重复**，已删除。亮/灭改由"选中色块
                                             *   有没有那圈框"表示（见上方色块 delegate 的 border）。 */
                                            Text {
                                                text: "#" + ("000000" + (ledRow.col & 0xFFFFFF).toString(16)).slice(-6).toUpperCase()
                                                font.pointSize: 9; color: "#888"
                                                Layout.preferredWidth: 52
                                            }
                                            Slider {
                                                id: briSlider
                                                /* ★ 10-04 量程对齐硬件上限；10-05 下限由 0 改为 1
                                                 *（0% 已全黑、无意义）⇒ 量程 [hwMin, hwCap] = [1, 15]。 */
                                                from: ledTestPanel.hwMin; to: ledTestPanel.hwCap; stepSize: 1
                                                value: ledRow.bri
                                                /* 10-05: 亮度条弹性撑满剩余宽度（左侧"名称+色块+色值"之后、
                                                   百分比之前），长度随窗口自适应 —— 见上方"整行平铺"说明。 */
                                                Layout.fillWidth: true
                                                onMoved: {
                                                    var a = ledTestPanel.testBri.slice(); a[ledIdx] = value
                                                    ledTestPanel.testBri = a
                                                    /* ★ 10-05: 灭灯状态下拖亮度**只更新界面、不下发**。
                                                       原写法无条件调 applyLed ⇒ 灭态走 ledTestClear，
                                                       每拖一格就白发一条"熄灭(黑)"命令给固件（USB 下
                                                       等于每格起一个 worker 进程），纯属浪费。
                                                       亮度值本来也不会作为亮度下发，等点亮时才随颜色带走
                                                       （见 applyLed：亮 → ledTestSet 才把 bri 送出去）。 */
                                                    if (ledTestPanel.testOn[ledIdx])
                                                        ledTestPanel.applyLed(ledIdx)
                                                }
                                                background: Rectangle {
                                                    implicitHeight: 8
                                                    radius: 4; color: "#e0e0e0"
                                                    width: briSlider.width; height: implicitHeight
                                                    Rectangle { width: briSlider.visualPosition * parent.width
                                                               height: parent.height; radius: 4; color: "#f56c6c" }
                                                }
                                                handle: Rectangle {
                                                    /* ★ 10-05 修复"拖不到底"：原式多减了 `- width/2`
                                                       ⇒ 手柄整体左移半个身位，visualPosition=1 时也够不到右端，
                                                       看起来就是"拖到底了还差一截"。标准式 = visualPosition×(可用宽−手柄宽)：
                                                       0 时手柄贴左端、1 时贴右端。 */
                                                    x: briSlider.visualPosition * (briSlider.width - width)
                                                    y: (briSlider.height - height) / 2
                                                    width: 14; height: 14; radius: 7
                                                    color: briSlider.pressed ? "#e6f5ec" : "white"
                                                    border.color: "#f56c6c"; border.width: 1.5
                                                }
                                            }
                                            Text { text: bri + "%"
                                                   font.pointSize: 10
                                                   color: (bri > ledTestPanel.hwCap) ? "#d46b08" : "#666"
                                                   Layout.preferredWidth: 36; horizontalAlignment: Text.AlignRight }
                                            /* 10-05: 原「亮/灭」小按钮已删除 —— 开关并入色块点击（见上方 MouseArea），
                                               亮/灭由"选中的色块有没有那圈框"表示。 */
                                        }
                                    }

                                    // ---- 固件限幅说明（滑块量程已对齐硬件上限，此处仅作告知）----
                                    Rectangle {
                                        Layout.fillWidth: true; height: 30; radius: 4
                                        /* 10-04: 滑块量程已 = 固件上限(15%)，因此不再需要
                                         * "有几颗灯超限"的统计与警告配色 —— 那段逻辑已删除
                                         * （超限在 UI 上已不可能发生，留着是死代码）。 */
                                        color: "#f7f7f7"
                                        border.color: "#e8e8e8"
                                        border.width: 1
                                        RowLayout { anchors.fill: parent; anchors.margins: 8; spacing: 6
                                            /* 10-05 用户要求：去掉行首那个 ⓘ 图标（他说的"感叹号"）。 */
                                            Text {
                                                /* 10-05: 文案按用户要求改写（两处 15% 仍取 hwCap，
                                                   上限改了文案自动跟随）。 */
                                                text: "亮度范围 " + ledTestPanel.hwMin + "~" + ledTestPanel.hwCap
                                                      + "%(考虑功耗情况，目前固件出灯口锁死亮度 "
                                                      + ledTestPanel.hwMin + "~" + ledTestPanel.hwCap + "%)"
                                                font.pointSize: 10
                                                color: "#999"
                                                elide: Text.ElideRight
                                                Layout.fillWidth: true
                                            }
                                        }
                                    }
                                }
                            }

                            Item { Layout.fillWidth: true; height: 8 }
                        }
                    }

                    /* More Tab - 空白占位，待填充 */
                    ColumnLayout { spacing: 0; Layout.margins: 16
                        Item { Layout.fillWidth: true; Layout.fillHeight: true }
                        Text {
                            text: "敬请期待";
                            font.pointSize: 14;
                            color: "#ccc";
                            Layout.alignment: Qt.AlignHCenter
                        }
                        Item { Layout.fillWidth: true; Layout.fillHeight: true }
                    }
                }  /* ColumnLayout */
            }  /* white Rectangle */
        }  /* white content Rectangle */
    }  /* outer gray Rectangle */
}

    Connections {
        target: otaManager
        /* 08-27: 失败提示醒目化 —— 切回固件升级页 + 红色 + 显示更久(10s), 不再 3s 一闪而过 */
        onTransferFailed: function(error) {
            cfgStatusMsg = error; cfgStatusError = true
            cfgTimer.interval = 10000; cfgTimer.start()
            currentTab = 0   /* 确保用户在 OTA 页能看到错误横幅 */
        }
        onTransferComplete: {
            cfgStatusMsg = "升级完成"; cfgStatusError = false
            cfgTimer.interval = 3000; cfgTimer.start()
        }
    }

    Connections {
        target: keyConfig
        function onConfigReadComplete() {
            console.log("[QML] configReadComplete fired")
            cfgStatusMsg = "已读取配置信息"
            /* 09-07: 读取后把 EC 按下模式徽标/行可见性同步到数据真实值 */
            ecPressMode = ecPressIsRegular() ? 0 : 1
            ecPressGesture = 0
            refreshKeyTick++
        }
        onConfigWriteComplete: { cfgStatusMsg = "配置已写入"; cfgStatusError = false; cfgTimer.interval = 3000; cfgTimer.start() }
        onConfigResetComplete: { cfgStatusMsg = "已恢复默认，正在读取配置..."; cfgStatusError = false; cfgTimer.interval = 3000; cfgTimer.start()
            ecPressMode = ecPressIsRegular() ? 0 : 1
            ecPressGesture = 0
            refreshKeyTick++
            /* 09-07: 恢复成功后自动回读一次 —— 让各卡片/弹窗显示恢复后的默认值。
             * 用 Qt.callLater 排到事件循环(等 reset worker 进程清理完), 避免抢串口。 */
            Qt.callLater(function() { keyConfig.readConfig() }) }
        onConfigFailed: function(error) { cfgStatusMsg = error; cfgStatusError = true; cfgTimer.interval = 10000; cfgTimer.start() }
    }

    Timer { id: cfgTimer; interval: 3000; onTriggered: { cfgStatusMsg = "" } }

    // 关闭窗口时收进系统托盘，保持后台常驻；仅托盘菜单的“退出”可结束程序
    onClosing: function(close) {
        close.accepted = false
        systemTray.minimizeToTray()
    }
}  /* ApplicationWindow */
