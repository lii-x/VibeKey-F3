// ============================================================
// 协议常量 (与 config_worker.py / 固件一致)
//  08-27: 新增槽位字段(buf[5]=0~2) + 休眠时间(sleep_min)
//  08-31: 新增设备信息握手 CMD_QUERY(0x05)
// ============================================================
const CONF_MAGIC = 0x434F4E46;  // 'CONF'
const KEYC_MAGIC = 0x4B455943;  // 'KEYC'
const CMD_READ   = 0x01;
const CMD_WRITE  = 0x02;
const CMD_RESET  = 0x03;
const CMD_QUERY  = 0x05;        // 设备信息握手: 返回 "VibeKey-F3|版本|电量|槽位\n"
const CMD_RGB    = 0x06;
const CMD_LED    = 0x04;      // LED 灯效下发 (USB 通道, payload=1字节状态)

// 09-27: 新增 airmouse(4) —— 空中鼠标开关动作。
//   仅 KEY L1 / EC 相关键允许(用户要求: L1 手势里也能开关空中鼠标, 每触发切换一次,
//   与 EC 旋转/L1 手势语义一致); L2/L3 与 C 键手势不允许。
const ACTIONS = { 0: 'none', 1: 'keyboard', 2: 'mouse', 3: 'multimedia', 4: 'airmouse' };
const ACTION_IDS = { 'none': 0, 'keyboard': 1, 'mouse': 2, 'multimedia': 3, 'airmouse': 4 };

// 09-27: 键块元数据 —— 驱动"通用键块渲染器"，避免为 17 个键块各写一套 UI。
//   path   : config 内的取值路径(数组用 .0/.1/.2)
//   label  : 界面标题
//   allow  : 允许的动作类型(决定动作下拉的选项)
//   rgb    : 是否有 RGB 底光硬件(仅 C1-C3 有; 其余字段存而不用, 固件忽略)
//   capture: 是否提供"按键捕获"按钮
const KEY_BLOCKS = [
  // ---- C 键：主键(常规模式) + 手势三件套 ----
  { path: 'keys.0', label: 'KEY C1 主键（常规模式）', allow: ['none','keyboard','mouse','multimedia'], rgb: true,  capture: true },
  { path: 'keys.1', label: 'KEY C2 主键（常规模式）', allow: ['none','keyboard','mouse','multimedia'], rgb: true,  capture: true },
  { path: 'keys.2', label: 'KEY C3 主键（常规模式）', allow: ['none','keyboard','mouse','multimedia'], rgb: true,  capture: true },
  { path: 'c_tap.0', label: 'C1 手势·单击', allow: ['none','keyboard','mouse','multimedia'], rgb: false, capture: true },
  { path: 'c_dbl.0', label: 'C1 手势·双击', allow: ['none','keyboard','mouse','multimedia'], rgb: false, capture: true },
  { path: 'c_lng.0', label: 'C1 手势·长按', allow: ['none','keyboard','mouse','multimedia'], rgb: false, capture: true },
  { path: 'c_tap.1', label: 'C2 手势·单击', allow: ['none','keyboard','mouse','multimedia'], rgb: false, capture: true },
  { path: 'c_dbl.1', label: 'C2 手势·双击', allow: ['none','keyboard','mouse','multimedia'], rgb: false, capture: true },
  { path: 'c_lng.1', label: 'C2 手势·长按', allow: ['none','keyboard','mouse','multimedia'], rgb: false, capture: true },
  { path: 'c_tap.2', label: 'C3 手势·单击', allow: ['none','keyboard','mouse','multimedia'], rgb: false, capture: true },
  { path: 'c_dbl.2', label: 'C3 手势·双击', allow: ['none','keyboard','mouse','multimedia'], rgb: false, capture: true },
  { path: 'c_lng.2', label: 'C3 手势·长按', allow: ['none','keyboard','mouse','multimedia'], rgb: false, capture: true },
  // ---- L 键：常规键 + 手势三件套(L1 额外允许 airmouse) ----
  { path: 'l_key.0', label: 'KEY L1 主键（常规模式）', allow: ['none','keyboard','mouse','multimedia','airmouse'], rgb: false, capture: true },
  { path: 'l_key.1', label: 'KEY L2 主键（常规模式）', allow: ['none','keyboard','mouse','multimedia'], rgb: false, capture: true },
  { path: 'l_key.2', label: 'KEY L3 主键（常规模式）', allow: ['none','keyboard','mouse','multimedia'], rgb: false, capture: true },
  { path: 'l_tap.0', label: 'L1 手势·单击', allow: ['none','keyboard','mouse','multimedia','airmouse'], rgb: false, capture: true },
  { path: 'l_dbl.0', label: 'L1 手势·双击', allow: ['none','keyboard','mouse','multimedia','airmouse'], rgb: false, capture: true },
  { path: 'l_lng.0', label: 'L1 手势·长按', allow: ['none','keyboard','mouse','multimedia','airmouse'], rgb: false, capture: true },
  { path: 'l_tap.1', label: 'L2 手势·单击', allow: ['none','keyboard','mouse','multimedia'], rgb: false, capture: true },
  { path: 'l_dbl.1', label: 'L2 手势·双击', allow: ['none','keyboard','mouse','multimedia'], rgb: false, capture: true },
  { path: 'l_lng.1', label: 'L2 手势·长按', allow: ['none','keyboard','mouse','multimedia'], rgb: false, capture: true },
  { path: 'l_tap.2', label: 'L3 手势·单击', allow: ['none','keyboard','mouse','multimedia'], rgb: false, capture: true },
  { path: 'l_dbl.2', label: 'L3 手势·双击', allow: ['none','keyboard','mouse','multimedia'], rgb: false, capture: true },
  { path: 'l_lng.2', label: 'L3 手势·长按', allow: ['none','keyboard','mouse','multimedia'], rgb: false, capture: true },
  // ---- EC 编码器(旋转是离散步进, 无手势概念; 按下支持模式) ----
  { path: 'ec_cw',    label: 'EC 正转一步', allow: ['none','keyboard','mouse','multimedia','airmouse'], rgb: false, capture: true },
  // 10-05: 按压滚动上/下 —— 手势模式下「按住旋钮旋转」。此前只有 config/读写/摘要卡,
  //   漏了这里的元数据 ⇒ 弹窗里那两行拿不到 label 与 allow(会显示成裸 path + 空动作下拉)。
  { path: 'ec_cw_press',  label: 'EC 按压滚动上', allow: ['none','keyboard','mouse','multimedia','airmouse'], rgb: false, capture: true },
  { path: 'ec_press', label: 'EC 按下（常规模式）', allow: ['none','keyboard','mouse','multimedia','airmouse'], rgb: false, capture: true },
  { path: 'ec_ccw',   label: 'EC 反转一步', allow: ['none','keyboard','mouse','multimedia','airmouse'], rgb: false, capture: true },
  { path: 'ec_ccw_press', label: 'EC 按压滚动下', allow: ['none','keyboard','mouse','multimedia','airmouse'], rgb: false, capture: true },
  { path: 'ec_press_tap', label: 'EC 按下·手势单击', allow: ['none','keyboard','mouse','multimedia','airmouse'], rgb: false, capture: true },
  { path: 'ec_press_dbl', label: 'EC 按下·手势双击', allow: ['none','keyboard','mouse','multimedia','airmouse'], rgb: false, capture: true },
  { path: 'ec_press_lng', label: 'EC 按下·手势长按', allow: ['none','keyboard','mouse','multimedia','airmouse'], rgb: false, capture: true },
  // ---- 摇一摇 ----
  { path: 'shake_key', label: '摇一摇触发键', allow: ['none','keyboard','multimedia'], rgb: false, capture: true },
];

// 各功能区包含的键块(用于分区渲染)
const KEY_GROUPS = [
  { title: 'KEY C1 / C2 / C3（主键 · 三手势）', modeKey: 'c_mode', modePaths: ['c_mode.0','c_mode.1','c_mode.2'],
    modeLabels: ['C1 模式','C2 模式','C3 模式'],
    blocks: ['keys.0','c_tap.0','c_dbl.0','c_lng.0',
             'keys.1','c_tap.1','c_dbl.1','c_lng.1',
             'keys.2','c_tap.2','c_dbl.2','c_lng.2'] },
  { title: 'KEY L1 / L2 / L3（主键 · 三手势）', modeKey: 'l_mode', modePaths: ['l_mode.0','l_mode.1','l_mode.2'],
    modeLabels: ['L1 模式','L2 模式','L3 模式'],
    blocks: ['l_key.0','l_tap.0','l_dbl.0','l_lng.0',
             'l_key.1','l_tap.1','l_dbl.1','l_lng.1',
             'l_key.2','l_tap.2','l_dbl.2','l_lng.2'] },
  // 10-05: 新增 ec_cw_press / ec_ccw_press（按压滚动，手势模式下按住旋钮旋转）；
  //   gestureOnlyBlocks = 仅在手势模式(ec_press_mode==1)下渲染 —— 它们在常规模式下不生效，
  //   一直显示会让用户"配了没反应"（与上位机的显隐规则保持一致）。
  { title: 'EC 编码器（正转 / 按压滚动 / 按下 / 反转）', modeKey: 'ec_press_mode', modePaths: ['ec_press_mode'],
    modeLabels: ['EC 按下模式'],
    gestureOnlyBlocks: ['ec_cw_press','ec_ccw_press'],
    blocks: ['ec_cw','ec_cw_press','ec_press','ec_ccw','ec_ccw_press',
             'ec_press_tap','ec_press_dbl','ec_press_lng'] },
  { title: '摇一摇（Shake）', modeKey: null, modePaths: [], modeLabels: [],
    blocks: ['shake_key'] },
];

// 08-27: 与上位机 UI 命名对齐 (底层协议名仍为 KEY3/4/5)
const KEY_NAMES = ['KEY C1', 'KEY C2', 'KEY C3'];

// 修饰键位图
const MODIFIERS = [
  { bit: 0x01, label: 'LCtrl', name: '左 Ctrl' },
  { bit: 0x02, label: 'LShift', name: '左 Shift' },
  { bit: 0x04, label: 'LAlt', name: '左 Alt' },
  { bit: 0x08, label: 'LWin', name: '左 Win' },
  { bit: 0x10, label: 'RCtrl', name: '右 Ctrl' },
  { bit: 0x20, label: 'RShift', name: '右 Shift' },
  { bit: 0x40, label: 'RAlt', name: '右 Alt' },
  { bit: 0x80, label: 'RWin', name: '右 Win' },
];

// HID 键码表 (与 KeyConfigManager.cpp 一致)
const HID_KEYS = [
  { text: '无', value: 0 },
  { text: 'A', value: 4 }, { text: 'B', value: 5 }, { text: 'C', value: 6 },
  { text: 'D', value: 7 }, { text: 'E', value: 8 }, { text: 'F', value: 9 },
  { text: 'G', value: 10 }, { text: 'H', value: 11 }, { text: 'I', value: 12 },
  { text: 'J', value: 13 }, { text: 'K', value: 14 }, { text: 'L', value: 15 },
  { text: 'M', value: 16 }, { text: 'N', value: 17 }, { text: 'O', value: 18 },
  { text: 'P', value: 19 }, { text: 'Q', value: 20 }, { text: 'R', value: 21 },
  { text: 'S', value: 22 }, { text: 'T', value: 23 }, { text: 'U', value: 24 },
  { text: 'V', value: 25 }, { text: 'W', value: 26 }, { text: 'X', value: 27 },
  { text: 'Y', value: 28 }, { text: 'Z', value: 29 },
  { text: '1', value: 30 }, { text: '2', value: 31 }, { text: '3', value: 32 },
  { text: '4', value: 33 }, { text: '5', value: 34 }, { text: '6', value: 35 },
  { text: '7', value: 36 }, { text: '8', value: 37 }, { text: '9', value: 38 },
  { text: '0', value: 39 },
  { text: 'Enter', value: 40 }, { text: 'Esc', value: 41 },
  { text: 'Backspace', value: 42 }, { text: 'Tab', value: 43 },
  { text: 'Space', value: 44 }, { text: '-', value: 45 }, { text: '=', value: 46 },
  { text: '[', value: 47 }, { text: ']', value: 48 }, { text: '\\', value: 49 },
  { text: ';', value: 51 }, { text: "'", value: 52 }, { text: '`', value: 53 },
  { text: ',', value: 54 }, { text: '.', value: 55 }, { text: '/', value: 56 },
  { text: 'CapsLock', value: 57 },
  { text: 'F1', value: 58 }, { text: 'F2', value: 59 }, { text: 'F3', value: 60 },
  { text: 'F4', value: 61 }, { text: 'F5', value: 62 }, { text: 'F6', value: 63 },
  { text: 'F7', value: 64 }, { text: 'F8', value: 65 }, { text: 'F9', value: 66 },
  { text: 'F10', value: 67 }, { text: 'F11', value: 68 }, { text: 'F12', value: 69 },
  { text: 'PrintScreen', value: 70 }, { text: 'ScrollLock', value: 71 },
  { text: 'Pause', value: 72 }, { text: 'Insert', value: 73 }, { text: 'Home', value: 74 },
  { text: 'PageUp', value: 75 }, { text: 'Delete', value: 76 }, { text: 'End', value: 77 },
  { text: 'PageDown', value: 78 }, { text: 'Right', value: 79 }, { text: 'Left', value: 80 },
  { text: 'Down', value: 81 }, { text: 'Up', value: 82 },
];

// 多媒体键码 (常用)
const MEDIA_KEYS = [
  { text: '音量+', value: 0xE9 }, { text: '音量-', value: 0xEA },
  { text: '静音', value: 0xE2 }, { text: '播放/暂停', value: 0xCD },
  { text: '下一曲', value: 0xB5 }, { text: '上一曲', value: 0xB6 },
  { text: '停止', value: 0xB7 }, { text: '浏览器主页', value: 0x8A },
  { text: '邮件', value: 0x8A }, { text: '搜索', value: 0xE6 },
];

// 鼠标键码
// 键值必须与固件一致: Firmware/src/main.c:1749-1751
//   EC_KC_WHEEL_UP=0x05 / EC_KC_WHEEL_DOWN=0x06
// (QML Main.qml:466-486 同样按 1/2/4/5/6 映射)。曾误用 USB HID 标准的 8/16,
//  导致设备里的 5/6 在下拉里找不到对应项 -> 细分显示空白。
const MOUSE_KEYS = [
  { text: '左键', value: 1 }, { text: '右键', value: 2 },
  { text: '中键', value: 4 }, { text: '滚轮上', value: 5 },
  { text: '滚轮下', value: 6 },
];

// 08-27: 自动休眠档位 (0=永不)
const SLEEP_OPTIONS = [45, 60, 90, 120, 180, 0];

// ============================================================
// 主题切换 (深色 / 浅色 / 跟随系统)
// ============================================================
const THEME_KEY = 'vibekey-theme';
const THEME_LABELS = { dark: '🌙 深色', light: '☀️ 浅色', auto: '🖥️ 跟随' };

function applyTheme(theme) {
  const root = document.documentElement;
  if (theme === 'auto') {
    root.dataset.theme = window.matchMedia('(prefers-color-scheme: dark)').matches ? 'dark' : 'light';
  } else {
    root.dataset.theme = theme;
  }
  const btn = document.getElementById('themeBtn');
  if (btn) btn.textContent = THEME_LABELS[theme] || THEME_LABELS.auto;
}

function cycleTheme() {
  const current = localStorage.getItem(THEME_KEY) || 'auto';
  const next = current === 'dark' ? 'light' : (current === 'light' ? 'auto' : 'dark');
  localStorage.setItem(THEME_KEY, next);
  applyTheme(next);
}

// 跟随系统模式下, 系统主题变化时自动切换
window.matchMedia('(prefers-color-scheme: dark)').addEventListener('change', () => {
  if ((localStorage.getItem(THEME_KEY) || 'auto') === 'auto') applyTheme('auto');
});

// ============================================================
// 状态
// ============================================================
let port = null;
let reader = null;
let writer = null;
let keepReading = false;
let isBusy = false;
let rxBuffer = new Uint8Array(0);  // 全局接收缓冲区
let rxResolve = null;               // 等待数据的 Promise resolver

// 08-27: 当前编辑槽位 0~2 (蓝牙1~3)
let currentSlot = 0;
// 08-31: 设备信息 (握手查询得到)
let deviceInfo = { name: '', version: '', battery: -1, slot: -1 };
// 设备上次上报的活跃蓝牙槽位。用于检测"设备端真正切换槽位"(相对上次上报发生变化)，
// 避免轮询把用户手动选择的编辑槽位弹回 (09-01 修复: 手动选槽被 5s 轮询覆盖)。
let lastReportedDeviceSlot = -1;

// ============================================================
// 配置数据结构（09-27 扩展：完整对齐固件 key_config_storage_t，sizeof 304）
//
// 9 字节键块（与固件 key_config_t 一一对应）：
//   { action_type, modifier, keycode, reserved, rgb_en, r, g, b, bri }
// 键块的三种用途：
//   · 常规键（按下/松开直通）：keys[](C1-C3) / l_key[](L1-L3) / ec_cw / ec_press / ec_ccw
//   · 手势键（该键"模式"选"手势"时生效）：*_tap(单击) / *_dbl(双击) / *_lng(长按)
//   · 摇一摇快捷键：shake_key
// ⚠️ 只有 C1-C3 有 RGB 底光硬件；其余键块的 rgb_* 字段固件存而不用。
// ⚠️ 每个键由 KEY_BLOCKS 的 path 定位（数组形式用 '数组名.下标'，如 'c_dbl.0'）。
// ============================================================
// 10-05: 新增可选参数 modifier（默认 0，向后兼容所有旧调用）。
// 用途：EC 按压滚动默认是 LCtrl+[ / LCtrl+]，需要带修饰键，而原签名只能给 action/keycode。
function mkKey(name, action, keycode, rgb, modifier) {
  return {
    name: name || '',
    action: action || 'none',
    modifier: modifier || 0,
    keycode: keycode || 0,
    rgb_enabled: !!rgb,
    rgb_color: 0x20A0FF,
    rgb_brightness: 15,
  };
}

let config = {
  // ---- 头部 ----
  air_mouse_mode: 1,     // 0=单击切换 1=按住移动
  air_mouse_speed: 1,    // 0=慢 1=中 2=快
  air_mouse_dir: 0,      // 0=0° 1=90° 2=180° 3=270°
  sleep_min: 45,         // 0=永不; 45~240 合法
  // ---- 摇一摇 ----
  shake_enabled: 0,
  shake_sens: 1,         // 0=轻 1=适中 2=用力
  shake_key: mkKey('摇一摇', 'none', 0, false),
  // ---- C1 / C2 / C3 ----
  keys: [
    mkKey('KEY C1', 'keyboard', 0, true),
    mkKey('KEY C2', 'keyboard', 0, true),
    mkKey('KEY C3', 'keyboard', 0, true),
  ],
  c_mode: [0, 0, 0],                             // 0=常规(直通) 1=手势
  c_tap: [mkKey(), mkKey(), mkKey()],            // 手势·单击(独立存储)
  c_dbl: [mkKey(), mkKey(), mkKey()],            // 手势·双击
  c_lng: [mkKey(), mkKey(), mkKey()],            // 手势·长按
  // ---- L1 / L2 / L3 ----
  l_mode: [0, 0, 0],
  l_key: [
    mkKey('KEY L1', 'airmouse', 0, false),       // 历史默认: L1=空中鼠标开关
    mkKey('KEY L2', 'mouse', 1, false),          // 历史默认: L2=鼠标左键(按住保持)
    mkKey('KEY L3', 'mouse', 2, false),          // 历史默认: L3=鼠标右键
  ],
  l_tap: [mkKey(), mkKey(), mkKey()],
  l_dbl: [mkKey(), mkKey(), mkKey()],
  l_lng: [mkKey(), mkKey(), mkKey()],
  // ---- EC 编码器（10-05: 默认值对齐固件 key_config.c 的出厂默认）----
  //   ⚠️ 原为正转/反转 = none（无动作），与固件的"滚轮上/下"不一致 —— 若"未读取就写入"
  //      会把设备的正转/反转写成无动作。现统一为固件默认值。
  ec_cw:    mkKey('EC 正转', 'mouse', 5, false),          // 固件: MOUSE 0x05 滚轮上
  ec_press: mkKey('EC 按下', 'mouse', 4, false),          // 中键
  ec_ccw:   mkKey('EC 反转', 'mouse', 6, false),          // 固件: MOUSE 0x06 滚轮下
  ec_press_mode: 1,                                      // 1=手势（固件默认）
  ec_press_tap: mkKey('EC 按下·单击', 'mouse', 4, false),  // 手势单击=中键（与常规体验一致）
  ec_press_dbl: mkKey('EC 按下·双击', 'none', 0, false),
  ec_press_lng: mkKey('EC 按下·长按', 'none', 0, false),
  // ---- 10-05 新增：EC 按压滚动（手势模式下按住旋钮旋转）----
  //   固件默认 = KEYBOARD + modifier 0x01(LCtrl) + keycode 0x2F('[') / 0x30(']')
  ec_cw_press:  mkKey('EC 按压滚动上', 'keyboard', 0x2F, false, 0x01),
  ec_ccw_press: mkKey('EC 按压滚动下', 'keyboard', 0x30, false, 0x01),
  // ---- 诊断: 本次 READ 回包实际长度(用于判断固件新旧, 不参与写入) ----
  raw_len: 0,
};

// ---- 路径访问工具（'c_dbl.0' → config.c_dbl[0]）----
function getBlock(path) {
  const parts = String(path).split('.');
  let o = config;
  for (const p of parts) { if (o == null) return null; o = o[p]; }
  return o;
}
function setBlock(path, val) {
  const parts = String(path).split('.');
  let o = config;
  for (let i = 0; i < parts.length - 1; i++) { o = o[parts[i]]; if (o == null) return; }
  o[parts[parts.length - 1]] = val;
}
function getMeta(path) { return KEY_BLOCKS.find(b => b.path === path) || { label: path, allow: [], rgb: false, capture: false }; }

let capturingPath = '';   // 09-27: 路径式捕获（'' 表示当前未在捕获；原 capturingIndex 已废弃）
let captureMods = 0;

// ============================================================
// 日志 & Toast
// ============================================================
function log(msg, type = '') {
  const panel = document.getElementById('logPanel');
  const time = new Date().toLocaleTimeString('zh-CN', { hour12: false });
  const line = document.createElement('div');
  line.className = 'log-line ' + type;
  line.innerHTML = `<span class="log-time">[${time}]</span>${msg}`;
  panel.appendChild(line);
  panel.scrollTop = panel.scrollHeight;
}

function toast(msg, type = 'info') {
  const el = document.getElementById('toast');
  el.textContent = msg;
  el.className = 'toast ' + type + ' show';
  setTimeout(() => el.classList.remove('show'), 3000);
}

// ============================================================
// 槽位 & 休眠 (08-27)
// ============================================================
function setSlot(idx, silent) {
  if (idx < 0) idx = 0;
  if (idx > 2) idx = 2;
  if (currentSlot === idx) { updateSlotUI(); return; }
  currentSlot = idx;
  updateSlotUI();
  log('已切换到蓝牙' + (idx + 1) + ' 槽位', 'info');
  // 切槽即读取该槽配置(丢弃当前未保存的编辑内容), 与上位机 setSlot 一致
  if (!silent) readConfig();
}

function updateSlotUI() {
  document.querySelectorAll('.slot-btn').forEach(b => {
    b.classList.toggle('active', parseInt(b.dataset.slot) === currentSlot);
  });
}

function setSleepMin() {
  config.sleep_min = parseInt(document.getElementById('sleepMin').value);
  if (isNaN(config.sleep_min)) config.sleep_min = 0;
  log('休眠时间: ' + sleepLabel(config.sleep_min) + ' (写入后生效)', 'info');
}

function sleepLabel(min) {
  if (min <= 0) return '永不';
  if (min % 60 === 0) return (min / 60) + ' 小时';
  if (min === 90) return '1.5 小时';
  return min + ' 分钟';
}

function updateSleepUI() {
  const sel = document.getElementById('sleepMin');
  sel.value = String(config.sleep_min);
}

// ============================================================
// Web Serial 连接
// ============================================================

// 打开一个 SerialPort (设置波特率/DTR/RTS + 等待就绪 + 清空输入缓冲区), 成功返回 true
async function openSerialPort(p) {
  try {
    await p.open({ baudRate: 115200, dataBits: 8, stopBits: 1, parity: 'none' });
    try { await p.setSignals({ dataTerminalReady: true, requestToSend: true }); } catch (e) { /* 部分设备不需要 */ }
    await sleep(400);
    // 清空输入缓冲区
    try {
      const dr = p.readable.getReader();
      while (true) {
        const { value, done } = await Promise.race([
          dr.read(),
          new Promise(res => setTimeout(() => res({ value: undefined, done: true }), 150))
        ]);
        if (done || !value) break;
      }
      dr.releaseLock();
    } catch (e) { /* ignore */ }
    return true;
  } catch (e) {
    return false;
  }
}

// 端口打开后的通用后续: 更新 UI + 启动读循环 + 握手查询 + 读取当前槽配置
async function onPortOpened() {
  document.getElementById('statusDot').classList.add('connected');
  document.getElementById('statusText').textContent = '已连接';
  document.getElementById('connectBtn').style.display = 'none';
  document.getElementById('disconnectBtn').style.display = 'inline-flex';
  setControlsBusy(false);
  updateLedChannel();
  updateOtaControls();
  keepReading = true;
  readLoop();
  const info = await sendQuery();
  if (info && info.name) {
    applyDeviceInfo(info, false);
    if (info.slot >= 1 && info.slot <= 3) {
      currentSlot = info.slot - 1;
      updateSlotUI();
      lastReportedDeviceSlot = info.slot;
      log('设备当前蓝牙槽位: 蓝牙' + info.slot, 'info');
    }
  }
  setTimeout(readConfig, 300);
}

// 向指定端口发握手查询, 验证是否为 VibeKey-F3 设备 (用于自动连接时的设备识别)
async function verifyVibeKey(p) {
  try {
    const savedBuf = rxBuffer;
    rxBuffer = new Uint8Array(0);
    const q = buildHeader(CMD_QUERY, 0, 0);
    const w = p.writable.getWriter();
    await w.write(q);
    w.releaseLock();
    const line = await otaReadLine(1500, p);
    rxBuffer = savedBuf;
    const info = parseDeviceInfo(line);
    return !!(info.name && info.name.toLowerCase().includes('vibekey'));
  } catch (e) {
    return false;
  }
}

// 自动识别并连接已授权的 USB 设备 (页面加载 / 热插拔时调用)
async function autoConnectUsb() {
  if (port || isBusy || otaTransferring || capturingPath) return;
  try {
    const ports = await navigator.serial.getPorts();
    for (const p of ports) {
      if (!(await openSerialPort(p))) continue;
      // 验证是否为 VibeKey-F3, 不是则关闭继续试下一个
      if (!(await verifyVibeKey(p))) {
        try { await p.close(); } catch (e) {}
        continue;
      }
      port = p;
      log('自动连接到已授权 USB 设备', 'success');
      toast('已自动连接设备', 'success');
      await onPortOpened();
      return;
    }
  } catch (e) { /* 静默, 等用户手动连接 */ }
}

async function connectDevice() {
  if (!('serial' in navigator)) {
    toast('当前浏览器不支持 Web Serial API，请使用 Chrome 或 Edge', 'error');
    log('浏览器不支持 Web Serial API', 'error');
    return;
  }
  try {
    if (port) { toast('设备已连接', 'info'); return; }
    // 不过滤 VID，列出所有可用串口（VibeKey-F3 用 MCU 自带 CDC ACM，VID 不固定）
    port = await navigator.serial.requestPort();
    if (!(await openSerialPort(port))) throw new Error('无法打开串口（可能被其他程序占用）');
    log('串口已打开: 115200 8N1, DTR/RTS=ON', 'success');
    toast('设备连接成功', 'success');
    await onPortOpened();
  } catch (e) {
    port = null;
    if (e.name === 'NotFoundError') {
      log('用户取消了设备选择', 'warn');
    } else if (e.name === 'NetworkError') {
      log('串口被占用，请关闭桌面版 VibeKey Studio 后重试', 'error');
      toast('串口被占用，请先关闭桌面版 Studio', 'error');
    } else {
      log('连接失败: ' + e.name + ' - ' + e.message, 'error');
      toast('连接失败: ' + e.message, 'error');
    }
    log('排查: 1)确认设备已插USB  2)关闭桌面版Studio释放串口  3)设备管理器确认COM口存在  4)用Chrome/Edge浏览器', 'warn');
  }
}

async function disconnectDevice() {
  keepReading = false;
  rxBuffer = new Uint8Array(0);
  if (rxResolve) { rxResolve(); rxResolve = null; }
  if (reader) { try { await reader.cancel(); } catch(e){} }
  if (port) {
    try { await port.setSignals({ dataTerminalReady: false, requestToSend: false }); } catch(e){}
    try { await port.close(); } catch(e){}
  }
  port = null;
  deviceInfo = { name: '', version: '', battery: -1, slot: -1 };
  lastReportedDeviceSlot = -1;
  document.getElementById('deviceInfo').style.display = 'none';
  log('已断开连接', 'warn');
  toast('已断开', 'info');

  document.getElementById('statusDot').classList.remove('connected');
  document.getElementById('statusText').textContent = '未连接';
  document.getElementById('connectBtn').style.display = 'inline-flex';
  document.getElementById('disconnectBtn').style.display = 'none';
  setControlsBusy(true);
  updateLedChannel();
  updateOtaControls();
}

// 持续读取循环：所有收到的数据放入 rxBuffer，通知等待者
async function readLoop() {
  while (keepReading && port && port.readable) {
    try {
      reader = port.readable.getReader();
      const { value, done } = await reader.read();
      reader.releaseLock();
      if (done) break;
      if (value && value.length > 0) {
        // 追加到缓冲区
        const newBuf = new Uint8Array(rxBuffer.length + value.length);
        newBuf.set(rxBuffer, 0);
        newBuf.set(value, rxBuffer.length);
        rxBuffer = newBuf;
        log('← 收到数据: ' + bytesToHex(value) + ' (缓冲区共 ' + rxBuffer.length + ' 字节)', 'info');
        // 通知等待者
        if (rxResolve) {
          const r = rxResolve;
          rxResolve = null;
          r();
        }
      }
    } catch (e) {
      if (keepReading) {
        log('读取循环错误: ' + e.message, 'error');
        // 设备意外断开，自动更新 UI 状态
        keepReading = false;
        port = null;
        document.getElementById('statusDot').classList.remove('connected');
        document.getElementById('statusText').textContent = '已断开';
        document.getElementById('connectBtn').style.display = 'inline-flex';
        document.getElementById('disconnectBtn').style.display = 'none';
        setControlsBusy(true);
        updateLedChannel();
        updateOtaControls();
        toast('设备已断开', 'error');
      }
      break;
    }
  }
}

// 等待缓冲区中有至少 n 字节，或超时
function waitForBytes(n, timeout) {
  return new Promise((resolve) => {
    const deadline = Date.now() + timeout;
    let settled = false;
    let timer = null;
    const check = () => {
      if (settled) return;
      if (rxBuffer.length >= n) {
        settled = true;
        if (timer) clearTimeout(timer);
        if (rxResolve === check) rxResolve = null;
        resolve(true);
      } else if (Date.now() >= deadline) {
        settled = true;
        if (rxResolve === check) rxResolve = null;
        resolve(false);
      } else {
        rxResolve = check;
      }
    };
    timer = setTimeout(() => { if (!settled) check(); }, timeout);
    check();
  });
}

// 09-27: 变长响应专用等待器
//   背景: READ 回包长度随固件版本演进(38 → 52 → 68 → 104 → 160 → 164 → 192 → 276 → 304),
//   用固定 expectLen 会在旧固件上白等满整个超时。这里改成
//   "至少收到 minLen 字节、且此后静默 quietMs 无新增" 即认为收完。
//   返回 true 表示至少收到了 minLen 字节; 调用方应按 rxBuffer 实际长度解析(降级兼容)。
function waitForBytesQuiet(minLen, quietMs = 220, timeout = 3000) {
  return new Promise((resolve) => {
    const deadline = Date.now() + timeout;
    let settled = false;
    let lastLen = -1;
    let lastChange = Date.now();
    let timer = null;

    const poll = () => {
      if (settled) return;
      const now = Date.now();
      if (rxBuffer.length !== lastLen) { lastLen = rxBuffer.length; lastChange = now; }
      const done = (rxBuffer.length >= minLen) && ((now - lastChange) >= quietMs);
      if (done || now >= deadline) {
        settled = true;
        if (timer) clearInterval(timer);
        resolve(rxBuffer.length >= minLen);
        return;
      }
      if (rxBuffer.length < minLen && now >= deadline) {
        settled = true;
        if (timer) clearInterval(timer);
        resolve(false);
      }
    };
    timer = setInterval(poll, 25);
    poll();
  });
}

// 等待缓冲区内出现换行(设备信息握手回包为文本行)
function waitForLine(timeout) {
  return new Promise((resolve) => {
    const deadline = Date.now() + timeout;
    let settled = false;
    let timer = null;
    const check = () => {
      if (settled) return;
      if (rxBuffer.indexOf(0x0A) >= 0) {
        settled = true;
        if (timer) clearTimeout(timer);
        if (rxResolve === check) rxResolve = null;
        resolve(true);
      } else if (Date.now() >= deadline) {
        settled = true;
        if (rxResolve === check) rxResolve = null;
        resolve(false);
      } else {
        rxResolve = check;
      }
    };
    timer = setTimeout(() => { if (!settled) check(); }, timeout);
    check();
  });
}

// 从 rxBuffer 提取一行并移除
function extractLine() {
  const idx = rxBuffer.indexOf(0x0A);
  if (idx < 0) return null;
  const line = Array.from(rxBuffer.slice(0, idx)).map(b => String.fromCharCode(b)).join('').trim();
  rxBuffer = rxBuffer.slice(idx + 1);
  return line;
}

// ============================================================
// 底层通信
// ============================================================
// 08-27: buf[5] 携带槽位(0~2=槽1~3); 设备信息查询槽位为 0
function buildHeader(cmd, payloadLen, slot) {
  const buf = new ArrayBuffer(8);
  const dv = new DataView(buf);
  dv.setUint32(0, CONF_MAGIC, true);  // LE
  dv.setUint8(4, cmd);
  dv.setUint8(5, slot || 0);
  dv.setUint16(6, payloadLen, true);  // LE
  return new Uint8Array(buf);
}

async function sendAndReceive(data, expectLen = 16, timeout = 3000) {
  if (!port || !port.writable) throw new Error('设备未连接');

  // 清空接收缓冲区
  rxBuffer = new Uint8Array(0);

  // 发送
  writer = port.writable.getWriter();
  await writer.write(data);
  writer.releaseLock();
  log('→ 发送: ' + bytesToHex(data), 'info');

  // 等待响应
  const got = await waitForBytes(expectLen, timeout);
  if (!got && rxBuffer.length === 0) {
    log('未收到响应 (超时 ' + timeout + 'ms)', 'error');
    return new Uint8Array(0);
  }

  // 从缓冲区取出 expectLen 字节（如果数据更多，保留多余的）
  const take = Math.min(expectLen, rxBuffer.length);
  const resp = rxBuffer.slice(0, take);
  rxBuffer = rxBuffer.slice(take);
  log('← 响应: ' + bytesToHex(resp) + ' (' + resp.length + ' bytes)', 'success');
  return resp;
}

// 08-31: 设备信息握手 (cmd 0x05)。返回 "VibeKey-F3|版本|电量|槽位\n" 文本行
async function sendQuery() {
  if (!port || !port.writable) return null;
  try {
    rxBuffer = new Uint8Array(0);
    const q = buildHeader(CMD_QUERY, 0, 0);
    writer = port.writable.getWriter();
    await writer.write(q);
    writer.releaseLock();
    log('→ 查询设备信息: ' + bytesToHex(q), 'info');
    const got = await waitForLine(1500);
    if (!got) {
      log('设备未响应信息查询', 'warn');
      return null;
    }
    const line = extractLine();
    log('← 设备信息: ' + line, 'success');
    return parseDeviceInfo(line);
  } catch (e) {
    log('设备信息查询失败: ' + e.message, 'warn');
    return null;
  }
}

function parseDeviceInfo(line) {
  const parts = line.split('|');
  const info = {
    name: parts[0] || '',
    version: parts.length > 1 ? parts[1] : '',
    battery: -1,
    slot: -1,
  };
  if (parts.length > 2) {
    const b = parseInt(parts[2]);
    info.battery = (b >= 0 && b <= 100) ? b : -1;
  }
  if (parts.length > 3) {
    const s = parseInt(parts[3]);
    info.slot = (s >= 1 && s <= 3) ? s : -1;
  }
  return info;
}

function applyDeviceInfo(info, syncSlot) {
  deviceInfo = info;
  const el = document.getElementById('deviceInfo');
  if (info.name) {
    let txt = info.name;
    if (info.version) txt += ' v' + info.version;
    if (info.battery >= 0 && info.battery <= 100) txt += ' · 电量 ' + info.battery + '%';
    el.textContent = txt;
    el.style.display = 'inline';
  } else {
    el.textContent = '';
    el.style.display = 'none';
  }
  // 设备端切换蓝牙槽位 -> 同步更新编辑槽位并重新读取。
  // 09-01 修复: 仅当设备上报的槽位【相对上次上报发生变化】(即设备端真切了槽)才跟随;
  // 否则轮询会持续用设备当前活跃槽位覆盖用户手动选择的编辑槽位, 导致"点槽2又弹回槽1"。
  if (syncSlot && info.slot >= 1 && info.slot <= 3) {
    if (lastReportedDeviceSlot !== -1 && lastReportedDeviceSlot !== info.slot) {
      log('检测到设备切换到蓝牙' + info.slot + ' 槽位，自动同步', 'info');
      setSlot(info.slot - 1);
    }
    lastReportedDeviceSlot = info.slot;
  }
}

// 08-31: 周期轮询设备信息(电量/槽位)，上位机由 PortDetector 每 1.5s 轮询；
// 网页版 5s 一次，读/写进行中自动跳过，避免抢占串口
async function pollDeviceInfo() {
  if (isBusy || capturingPath || otaTransferring || !port) return;
  const info = await sendQuery();
  if (!info || !info.name) return;
  applyDeviceInfo(info, true);
}

function bytesToHex(bytes) {
  return Array.from(bytes).map(b => b.toString(16).padStart(2, '0').toUpperCase()).join(' ');
}

function sleep(ms) { return new Promise(r => setTimeout(r, ms)); }

// ============================================================
// 配置读写 (08-27: 全部带槽位 + 休眠时间)
// ============================================================
// 09-27: 9 字节键块解析（READ 回包是结构体内存布局，off 为结构体内绝对偏移）
//   布局: [action_type][modifier][keycode][reserved][rgb_en][r][g][b][bri]
function parseKeyBlock(dv, off) {
  return {
    name: '',
    action: ACTIONS[dv.getUint8(off)] || 'none',
    modifier: dv.getUint8(off + 1),
    keycode: dv.getUint8(off + 2),
    rgb_enabled: dv.getUint8(off + 4) !== 0,
    rgb_color: (dv.getUint8(off + 5) << 16) | (dv.getUint8(off + 6) << 8) | dv.getUint8(off + 7),
    rgb_brightness: dv.getUint8(off + 8),
  };
}

// 09-27: 只发送、不等待（配合 waitForBytesQuiet 处理变长响应）
async function sendRaw(data) {
  if (!port || !port.writable) throw new Error('设备未连接');
  rxBuffer = new Uint8Array(0);
  writer = port.writable.getWriter();
  await writer.write(data);
  writer.releaseLock();
  log('→ 发送: ' + bytesToHex(data), 'info');
}

// 09-27: 把键块序列化成 9 字节（WRITE payload 用，紧凑布局）
function putKeyBlock(u8, off, k) {
  u8[off]     = ACTION_IDS[k.action] || 0;
  u8[off + 1] = k.modifier & 0xFF;
  u8[off + 2] = k.keycode & 0xFF;
  u8[off + 3] = 0;                                  // reserved
  u8[off + 4] = k.rgb_enabled ? 1 : 0;
  u8[off + 5] = (k.rgb_color >> 16) & 0xFF;
  u8[off + 6] = (k.rgb_color >> 8) & 0xFF;
  u8[off + 7] = k.rgb_color & 0xFF;
  u8[off + 8] = k.rgb_brightness & 0xFF;
}

async function readConfig() {
  if (isBusy) { toast('正在处理中...', 'info'); return; }
  isBusy = true;
  setControlsBusy(true);
  log('→ 读取配置 (蓝牙' + (currentSlot + 1) + ' 槽位)...', 'info');

  try {
    const header = buildHeader(CMD_READ, 0, currentSlot);
    // 09-27: 固件 sizeof 已到 304；旧固件可能只回 38/52/68/104/160/164/192/276。
    // 用"静默判定"收包，再按**实际长度**分段解析 —— 未覆盖的字段保持默认值(平滑降级)。
    await sendRaw(header);
    await waitForBytesQuiet(8, 220, 3000);
    const resp = rxBuffer.slice();
    rxBuffer = new Uint8Array(0);

    if (resp.length < 8) throw new Error('响应太短 (' + resp.length + ' 字节)');

    const dv = new DataView(resp.buffer, resp.byteOffset, resp.byteLength);
    const magic = dv.getUint32(0, true);
    if (magic !== KEYC_MAGIC) throw new Error('响应魔数错误: 0x' + magic.toString(16));

    const len = resp.length;
    config.raw_len = len;
    const has = (n) => len >= n;                       // 分段判定
    const rd = (off) => ({ ...parseKeyBlock(dv, off), name: '' });

    // ---- 头部 ----
    const numKeys = Math.min(dv.getUint8(5) || 3, 3);
    config.air_mouse_mode  = dv.getUint8(6);
    config.air_mouse_speed = dv.getUint8(7);

    // ---- C1/C2/C3 主键 @8 / 17 / 26 ----
    for (let i = 0; i < numKeys; i++) {
      const b = rd(8 + i * 9);
      b.name = KEY_NAMES[i];
      config.keys[i] = b;
    }
    // ---- 尾部公共区 ----
    if (has(38)) {                                     // 36: sleep_min(LE16)
      const sm = dv.getUint16(36, true);
      config.sleep_min = (sm === 0xFFFF) ? 45 : sm;
    }
    if (has(39)) config.air_mouse_dir = dv.getUint8(38);
    if (has(41)) { config.shake_enabled = dv.getUint8(39); config.shake_sens = dv.getUint8(40); }
    if (has(50)) config.shake_key = rd(41);
    // ---- L2 @50 / L3 @59 / L1 @68（顺序如此，见 key_config.h）----
    if (has(59)) config.l_key[1] = rd(50);
    if (has(68)) config.l_key[2] = rd(59);
    if (has(77)) config.l_key[0] = rd(68);
    // ---- EC 正转 @77 / 按下 @86 / 反转 @95 ----
    if (has(86))  config.ec_cw    = rd(77);
    if (has(95))  config.ec_press = rd(86);
    if (has(104)) config.ec_ccw   = rd(95);
    // ---- C 键双击/长按 @104..157 ----
    if (has(113)) config.c_dbl[0] = rd(104);
    if (has(122)) config.c_lng[0] = rd(113);
    if (has(131)) config.c_dbl[1] = rd(122);
    if (has(140)) config.c_lng[1] = rd(131);
    if (has(149)) config.c_dbl[2] = rd(140);
    if (has(158)) config.c_lng[2] = rd(149);
    // ---- C 键模式 @160..162 ----
    if (has(161)) config.c_mode[0] = dv.getUint8(160);
    if (has(162)) config.c_mode[1] = dv.getUint8(161);
    if (has(163)) config.c_mode[2] = dv.getUint8(162);
    // ---- C 手势单击 @164 / 173 / 182 ----
    if (has(173)) config.c_tap[0] = rd(164);
    if (has(182)) config.c_tap[1] = rd(173);
    if (has(191)) config.c_tap[2] = rd(182);
    // ---- L 键模式 @192..194 ----
    if (has(193)) config.l_mode[0] = dv.getUint8(192);
    if (has(194)) config.l_mode[1] = dv.getUint8(193);
    if (has(195)) config.l_mode[2] = dv.getUint8(194);
    // ---- L 键双击/长按 @195..248 ----
    if (has(204)) config.l_dbl[0] = rd(195);
    if (has(213)) config.l_lng[0] = rd(204);
    if (has(222)) config.l_dbl[1] = rd(213);
    if (has(231)) config.l_lng[1] = rd(222);
    if (has(240)) config.l_dbl[2] = rd(231);
    if (has(249)) config.l_lng[2] = rd(240);
    // ---- L 手势单击 @249 / 258 / 267 ----
    if (has(258)) config.l_tap[0] = rd(249);
    if (has(267)) config.l_tap[1] = rd(258);
    if (has(276)) config.l_tap[2] = rd(267);
    // ---- EC 按下：模式 @276 + 双击@277/长按@286/单击@295 ----
    if (has(277)) config.ec_press_mode = dv.getUint8(276);
    if (has(286)) config.ec_press_dbl = rd(277);
    if (has(295)) config.ec_press_lng = rd(286);
    if (has(304)) config.ec_press_tap = rd(295);
    // ---- 10-05 新增：EC 按压滚动 @304 / @313 ----
    //   旧固件回包不到这个长度 ⇒ has() 为假 ⇒ 保持 config 里的默认值（与上位机同策略）
    if (has(313)) config.ec_cw_press  = rd(304);
    if (has(322)) config.ec_ccw_press = rd(313);

    // ---- 同步头部/摇一摇的 UI 控件（键块由 renderKeys 统一渲染）----
    const _amMode = document.getElementById('airMouseMode'); if (_amMode) _amMode.value = config.air_mouse_mode;
    const _amSpeed = document.getElementById('airMouseSpeed'); if (_amSpeed) _amSpeed.value = config.air_mouse_speed;
    const elDir = document.getElementById('airMouseDir');
    if (elDir) elDir.value = String(config.air_mouse_dir);
    const elShkEn = document.getElementById('shakeEnabled');
    if (elShkEn) elShkEn.value = String(config.shake_enabled ? 1 : 0);
    const elShkSens = document.getElementById('shakeSens');
    if (elShkSens) elShkSens.value = String(config.shake_sens);
    updateSleepUI();

    renderKeys();
    log('✓ 配置读取成功 (蓝牙' + (currentSlot + 1) + ', 回包 ' + len + 'B' +
        (len >= 304 ? ' 完整协议' : ' 旧固件/部分字段') + ', ' + numKeys + ' 键, 空中鼠标 mode=' +
        config.air_mouse_mode + ' speed=' + config.air_mouse_speed + ' dir=' + config.air_mouse_dir +
        ', 休眠=' + sleepLabel(config.sleep_min) + ')', 'success');
    toast('配置读取成功', 'success');
  } catch (e) {
    log('✗ 读取失败: ' + e.message, 'error');
    toast('读取失败: ' + e.message, 'error');
  } finally {
    isBusy = false;
    setControlsBusy(false);
  }
}

async function writeConfig() {
  if (isBusy) { toast('正在处理中...', 'info'); return; }
  isBusy = true;
  setControlsBusy(true);
  log('→ 写入配置 (蓝牙' + (currentSlot + 1) + ' 槽位)...', 'info');

  try {
    // 10-05: 完整 payload = 315 字节（原 297 + EC 按压滚动两键 9+9）。
    //   紧凑布局，无 padding，payload 索引 + 8 = buf 索引。
    //   固件按 data_len 分段判读 ⇒ 发全量对旧固件同样安全（超出部分被忽略），
    //   因此这里总是发满，不做版本协商。
    const payload = new Uint8Array(315);
    payload[0] = config.air_mouse_mode & 0xFF;
    payload[1] = config.air_mouse_speed & 0xFF;

    // C1/C2/C3 主键 @2/11/20
    for (let i = 0; i < 3; i++) putKeyBlock(payload, 2 + i * 9, config.keys[i]);

    // 29: sleep_min(LE16)。0=永不; 45~240 合法; <45 钳制到 45
    let sm = config.sleep_min;
    if (isNaN(sm) || sm < 0) sm = 0;
    if (sm > 240) sm = 240;
    if (sm > 0 && sm < 45) sm = 45;
    payload[29] = sm & 0xFF;
    payload[30] = (sm >> 8) & 0xFF;

    // 31..42: 方向 / 摇一摇(开关·灵敏度·触发键)
    payload[31] = config.air_mouse_dir & 0xFF;
    payload[32] = config.shake_enabled ? 1 : 0;
    payload[33] = config.shake_sens & 0xFF;
    putKeyBlock(payload, 34, config.shake_key);

    // 43..96: L2 / L3 / L1 / EC正转 / EC按下 / EC反转
    putKeyBlock(payload, 43, config.l_key[1]);
    putKeyBlock(payload, 52, config.l_key[2]);
    putKeyBlock(payload, 61, config.l_key[0]);
    putKeyBlock(payload, 70, config.ec_cw);
    putKeyBlock(payload, 79, config.ec_press);
    putKeyBlock(payload, 88, config.ec_ccw);

    // 97..150: C1/C2/C3 的双击、长按
    putKeyBlock(payload,  97, config.c_dbl[0]);
    putKeyBlock(payload, 106, config.c_lng[0]);
    putKeyBlock(payload, 115, config.c_dbl[1]);
    putKeyBlock(payload, 124, config.c_lng[1]);
    putKeyBlock(payload, 133, config.c_dbl[2]);
    putKeyBlock(payload, 142, config.c_lng[2]);

    // 151..152 占位(reserved_tail) | 153..155 C 键模式 | 156 占位
    payload[153] = config.c_mode[0] & 0xFF;
    payload[154] = config.c_mode[1] & 0xFF;
    payload[155] = config.c_mode[2] & 0xFF;

    // 157..183: C 手势单击 | 184 占位
    putKeyBlock(payload, 157, config.c_tap[0]);
    putKeyBlock(payload, 166, config.c_tap[1]);
    putKeyBlock(payload, 175, config.c_tap[2]);

    // 185..187: L 键模式
    payload[185] = config.l_mode[0] & 0xFF;
    payload[186] = config.l_mode[1] & 0xFF;
    payload[187] = config.l_mode[2] & 0xFF;

    // 188..241: L1/L2/L3 双击、长按
    putKeyBlock(payload, 188, config.l_dbl[0]);
    putKeyBlock(payload, 197, config.l_lng[0]);
    putKeyBlock(payload, 206, config.l_dbl[1]);
    putKeyBlock(payload, 215, config.l_lng[1]);
    putKeyBlock(payload, 224, config.l_dbl[2]);
    putKeyBlock(payload, 233, config.l_lng[2]);

    // 242..268: L 手势单击
    putKeyBlock(payload, 242, config.l_tap[0]);
    putKeyBlock(payload, 251, config.l_tap[1]);
    putKeyBlock(payload, 260, config.l_tap[2]);

    // 269: EC 按下模式 | 270..296: EC 按下 双击/长按/单击
    payload[269] = config.ec_press_mode & 0xFF;
    putKeyBlock(payload, 270, config.ec_press_dbl);
    putKeyBlock(payload, 279, config.ec_press_lng);
    putKeyBlock(payload, 288, config.ec_press_tap);

    // 297..305 / 306..314: EC 按压滚动（10-05 新增，对应固件结构体 @304 / @313）
    putKeyBlock(payload, 297, config.ec_cw_press);
    putKeyBlock(payload, 306, config.ec_ccw_press);

    const header = buildHeader(CMD_WRITE, payload.length, currentSlot);
    const data = new Uint8Array(header.length + payload.length);
    data.set(header, 0);
    data.set(payload, header.length);

    const resp = await sendAndReceive(data, 1);  // 设备返回 1 字节状态码 (0x00=成功)
    if (resp.length >= 1 && resp[0] === 0x00) {
      log('✓ 配置写入成功 (蓝牙' + (currentSlot + 1) + ' 槽位)', 'success');
      toast('写入成功', 'success');
    } else {
      throw new Error('设备拒绝配置 (resp[0]=' + (resp[0] ?? -1) + ')');
    }
  } catch (e) {
    log('✗ 写入失败: ' + e.message, 'error');
    toast('写入失败: ' + e.message, 'error');
  } finally {
    isBusy = false;
    setControlsBusy(false);
  }
}

async function resetConfig() {
  if (isBusy) { toast('正在处理中...', 'info'); return; }
  if (!confirm('确定要恢复蓝牙' + (currentSlot + 1) + ' 槽位的默认配置吗？当前所有改键设置将被清除。')) return;

  isBusy = true;
  setControlsBusy(true);
  log('→ 恢复默认配置 (蓝牙' + (currentSlot + 1) + ' 槽位)...', 'info');

  try {
    const header = buildHeader(CMD_RESET, 0, currentSlot);
    const resp = await sendAndReceive(header, 1);  // 设备返回 1 字节状态码
    if (resp.length >= 1 && resp[0] === 0x00) {
      log('✓ 已恢复默认配置 (蓝牙' + (currentSlot + 1) + ' 槽位)', 'success');
      toast('已恢复默认', 'success');
      setTimeout(readConfig, 300);
    } else {
      throw new Error('设备拒绝重置');
    }
  } catch (e) {
    log('✗ 恢复默认失败: ' + e.message, 'error');
    toast('恢复失败: ' + e.message, 'error');
  } finally {
    isBusy = false;
    setControlsBusy(false);
  }
}

async function previewRgb(keyIndex) {
  if (!port || isBusy) return;
  const k = config.keys[keyIndex];
  try {
    const payload = new Uint8Array(6);
    payload[0] = keyIndex;
    payload[1] = k.rgb_enabled ? 1 : 0;
    payload[2] = (k.rgb_color >> 16) & 0xFF;
    payload[3] = (k.rgb_color >> 8) & 0xFF;
    payload[4] = k.rgb_color & 0xFF;
    payload[5] = k.rgb_brightness;

    const header = buildHeader(CMD_RGB, payload.length, 0);
    const data = new Uint8Array(header.length + payload.length);
    data.set(header, 0);
    data.set(payload, header.length);

    // 预览不阻塞主流程，fire-and-forget
    if (port.writable) {
      const w = port.writable.getWriter();
      await w.write(data);
      w.releaseLock();
    }
  } catch (e) { /* 预览失败静默 */ }
}

function setControlsBusy(busy) {
  ['readBtn', 'writeBtn', 'resetBtn'].forEach(id => {
    document.getElementById(id).disabled = busy || !port;
  });
  document.querySelectorAll('.slot-btn').forEach(b => {
    b.disabled = busy || !port;
  });
  document.getElementById('sleepMin').disabled = busy || !port;
}

// ============================================================
// 灯效测试 (手动测试 LED) —— 与上位机 LightMonitor 一致
//   通道优先级: 蓝牙(BLE) > USB > 无设备
//   BLE: 服务 0xFF00 / 特征 0xFF01, 写 1 字节状态 (ble_led_worker.py)
//   USB: CMD_LED(0x04) + 1 字节状态 (config_worker.py conf_led)
//   状态: 0=关灯 1=黄灯 2=绿灯 3=红灯
// ============================================================
const LED_SVC_UUID   = '0000ff00-0000-1000-8000-00805f9b34fb';
const LED_CHAR_UUID  = '0000ff01-0000-1000-8000-00805f9b34fb';
const BATT_SVC_UUID  = '0000180f-0000-1000-8000-00805f9b34fb';
const BATT_CHAR_UUID = '00002a19-0000-1000-8000-00805f9b34fb';

let bleDevice = null;
let bleLedChar = null;
let bleBattery = -1;

function ledChannel() {
  if (bleDevice && bleDevice.gatt && bleDevice.gatt.connected && bleLedChar) return 'ble';
  if (port) return 'usb';
  return 'none';
}

function updateLedChannel() {
  const ch = ledChannel();
  const badge = document.getElementById('ledChannel');
  /* ★ 10-06: 「连接蓝牙」按钮已按用户要求从页面移除 ⇒ 这里必须 null 保护。
     本函数在页面加载时(以及每次通道变化时)都会跑, 直接写 btn.textContent 会抛
     TypeError 并中断调用方。按钮不在了, BLE 分支只更新徽章/电量即可。 */
  const btn = document.getElementById('bleLedBtn');
  const batt = document.getElementById('ledBattery');
  if (!badge) return;
  if (ch === 'ble') {
    badge.textContent = '通道：蓝牙';
    badge.className = 'channel-badge ble';
    if (btn) btn.textContent = '断开蓝牙';
    if (batt) batt.textContent = (bleBattery >= 0 && bleBattery <= 100) ? '电量 ' + bleBattery + '%' : '';
  } else if (ch === 'usb') {
    badge.textContent = '通道：USB';
    badge.className = 'channel-badge usb';
    if (btn) btn.textContent = '连接蓝牙';
    if (batt) batt.textContent = '';
  } else {
    badge.textContent = '通道：无设备';
    badge.className = 'channel-badge none';
    if (btn) btn.textContent = '连接蓝牙';
    if (batt) batt.textContent = '';
  }
}

async function toggleBleLed() {
  if (bleDevice && bleDevice.gatt && bleDevice.gatt.connected) {
    disconnectBleLed();
  } else {
    await connectBleLed();
  }
}

async function connectBleLed() {
  if (!('bluetooth' in navigator)) {
    toast('当前浏览器不支持 Web Bluetooth，请用 Chrome / Edge', 'error');
    log('浏览器不支持 Web Bluetooth API', 'error');
    return;
  }
  try {
    log('请求配对 VibeKey-F3 蓝牙设备...', 'info');
    bleDevice = await navigator.bluetooth.requestDevice({
      // 不用 services 过滤: VibeKey-F3 不在广播里带 0xFF00, 会导致 Chrome "找不到兼容设备"。
      // acceptAllDevices 显示所有附近设备, 用户手动选 VibeKey-F3; optionalServices 声明后续要访问的服务。
      acceptAllDevices: true,
      optionalServices: [0xff00, 0x180f],
    });
    log('已选择设备: ' + (bleDevice.name || 'VibeKey-F3'), 'success');
    const server = await bleDevice.gatt.connect();
    log('GATT 已连接，发现 LED 服务 (0xFF00)...', 'info');
    let svc;
    try {
      svc = await server.getPrimaryService(0xff00);
    } catch (_) {
      throw new Error('该设备不支持 VibeKey LED 服务 (0xFF00)，请确认选择的是 VibeKey-F3 设备，或设备固件已支持 LED 服务');
    }
    bleLedChar = await svc.getCharacteristic(0xff01);
    log('LED 特征已就绪 (0xFF01)', 'success');
    // 读取电量 (Battery Service 0x180F / 0x2A19)，失败静默
    try {
      const bs = await server.getPrimaryService(0x180f);
      const bc = await bs.getCharacteristic(0x2a19);
      const val = await bc.readValue();
      bleBattery = val.getUint8(0);
    } catch (e) {
      bleBattery = -1;
    }
    bleDevice.addEventListener('gattserverdisconnected', () => {
      bleLedChar = null;
      bleBattery = -1;
      log('蓝牙已断开 (GATT 断开)', 'warn');
      updateLedChannel();
    });
    updateLedChannel();
    toast('蓝牙 LED 服务已连接', 'success');
  } catch (e) {
    if (e.name === 'NotFoundError') {
      log('用户取消了蓝牙配对', 'warn');
    } else {
      log('蓝牙连接失败: ' + e.name + ' - ' + e.message, 'error');
      toast('蓝牙连接失败: ' + e.message, 'error');
    }
    bleDevice = null;
    bleLedChar = null;
    bleBattery = -1;
    updateLedChannel();
  }
}

function disconnectBleLed() {
  try {
    if (bleDevice && bleDevice.gatt && bleDevice.gatt.connected) bleDevice.gatt.disconnect();
  } catch (e) { /* ignore */ }
  bleLedChar = null;
  bleDevice = null;
  bleBattery = -1;
  log('蓝牙已断开', 'warn');
  updateLedChannel();
}

// USB 通道: CMD_LED(0x04), 帧头 slot=0 len=1 + 1 字节状态
async function sendLedUsb(state) {
  const header = buildHeader(CMD_LED, 1, 0);
  const data = new Uint8Array(9);
  data.set(header, 0);
  data[8] = state;
  const resp = await sendAndReceive(data, 1);
  return resp.length >= 1 && resp[0] === 0x00;
}

// BLE 通道: 写 1 字节到 0xFF01 (优先 with-response, 失败回退 without-response)
async function sendLedBle(state) {
  if (!bleLedChar) return false;
  const buf = new Uint8Array([state]);
  try {
    await bleLedChar.writeValue(buf);
    return true;
  } catch (e) {
    try {
      await bleLedChar.writeValueWithoutResponse(buf);
      return true;
    } catch (e2) {
      return false;
    }
  }
}

async function sendLed(state) {
  const names = { 0: '关灯', 1: '黄灯', 2: '绿灯', 3: '红灯' };
  const ch = ledChannel();
  if (ch === 'ble') {
    const ok = await sendLedBle(state);
    if (ok) { log('✓ LED 手动测试: ' + names[state] + ' (蓝牙)', 'success'); toast(names[state], 'success'); }
    else { log('✗ LED 下发失败 (蓝牙)', 'error'); toast('蓝牙 LED 下发失败', 'error'); }
  } else if (ch === 'usb') {
    if (isBusy) { toast('正在处理中...', 'info'); return; }
    isBusy = true;
    setControlsBusy(true);
    try {
      const ok = await sendLedUsb(state);
      if (ok) { log('✓ LED 手动测试: ' + names[state] + ' (USB)', 'success'); toast(names[state], 'success'); }
      else { log('✗ LED 下发失败 (USB)', 'error'); toast('USB LED 下发失败', 'error'); }
    } catch (e) {
      log('✗ LED 下发异常: ' + e.message, 'error');
      toast('LED 下发异常', 'error');
    } finally {
      isBusy = false;
      setControlsBusy(false);
    }
  } else {
    log('LED 手动测试失败: 无可用通道', 'warn');
    toast('无可用通道：请连接 USB 或先连接蓝牙', 'warn');
  }
}

// ============================================================
// 固件升级 (OTA) —— 与上位机 ota_worker.py 完全对齐 (USB 串口)
//   帧: [OTA_MAGIC(0x4F544100) LE + 固件长度 LE] + 固件体 + CRC32(LE)
//   预检: 分区上限 / ARMv7-M 向量表 / 内嵌型号标识
//   升级后: 重握手验证版本 (设备会重启 + USB 重新枚举)
// ============================================================
const OTA_MAGIC = 0x4F544100;
const OTA_REGION_LIMIT = 0x12C00000 - 0x12680000;  // 0x580000
const FW_MODEL_TAG = 'VIBEKEYF3-FWIMG-7F3A9C21';

let otaFirmware = null;       // { name, data:Uint8Array, size }
let otaTransferring = false;
let otaAbort = false;
let otaProgress = 0;

// 标准 CRC32 (zlib 兼容, 反射多项式 0xEDB88320)
const CRC_TABLE = (() => {
  const t = new Uint32Array(256);
  for (let n = 0; n < 256; n++) {
    let c = n;
    for (let k = 0; k < 8; k++) c = (c & 1) ? (0xEDB88320 ^ (c >>> 1)) : (c >>> 1);
    t[n] = c >>> 0;
  }
  return t;
})();

function crc32(data) {
  let c = 0xFFFFFFFF;
  for (let i = 0; i < data.length; i++) {
    c = CRC_TABLE[(c ^ data[i]) & 0xFF] ^ (c >>> 8);
  }
  return (c ^ 0xFFFFFFFF) >>> 0;
}

// ARMv7-M 向量表预检 (与 ota_worker.py is_valid_fw_image 一致)
function isFwValidImage(data) {
  if (data.length < 8) return false;
  const dv = new DataView(data.buffer, data.byteOffset, data.byteLength);
  const sp = dv.getUint32(0, true);      // 初始 MSP 应在 RAM 区
  if (!(sp >= 0x20000000 && sp < 0x30000000)) return false;
  const reset = (dv.getUint32(4, true) & ~1) >>> 0;  // Reset 向量应指向 Flash XIP + Thumb
  return reset >= 0x12000000 && reset <= 0x13FFFFFF;
}

// 固件内嵌型号标识
function containsModelTag(data) {
  const tag = new TextEncoder().encode(FW_MODEL_TAG);
  const limit = data.length - tag.length;
  for (let i = 0; i <= limit; i++) {
    let match = true;
    for (let j = 0; j < tag.length; j++) {
      if (data[i + j] !== tag[j]) { match = false; break; }
    }
    if (match) return true;
  }
  return false;
}

function selectFirmwareFile(input) {
  const f = input.files && input.files[0];
  if (!f) return;
  const reader = new FileReader();
  reader.onload = () => {
    const data = new Uint8Array(reader.result);
    // 预检 + 状态提示统一走 otaApplyFirmware(16-ota-net.js), 与网络下载共用一条链路。
    // __local 标记来源, 供「已选固件」行区分显示。
    otaFirmware = { name: f.name, data: data, size: data.length, __local: true };
    if (typeof otaApplyFirmware === 'function') otaApplyFirmware(f.name, data, '选择');
    else {
      document.getElementById('otaFileName').textContent = f.name + ' (' + data.length + ' 字节)';
      setOtaStatus('已选择固件: ' + f.name);
      updateOtaControls();
    }
  };
  reader.readAsArrayBuffer(f);
}

function setOtaStatus(msg, isError) {
  const el = document.getElementById('otaStatus');
  el.textContent = msg;
  el.className = 'ota-status' + (isError ? ' err' : '');
}

function updateOtaUI() {
  const bar = document.getElementById('otaProgressBar');
  if (bar) bar.style.width = otaProgress + '%';
  const wrap = document.getElementById('otaProgressWrap');
  if (wrap) wrap.style.display = (otaProgress > 0 || otaTransferring) ? '' : 'none';
}

function updateOtaControls() {
  const btn = document.getElementById('otaBtn');
  if (otaTransferring) {
    btn.textContent = '取消';
    btn.className = 'btn btn-danger';
    btn.disabled = false;
  } else {
    btn.textContent = '开始升级';
    btn.className = 'btn btn-success';
    // 固件升级只支持 USB: 蓝牙连接时禁用(与 QML 911 的提示一致, 固件自身限制)
    const usbOK = (typeof otaIsUsb === 'function') ? otaIsUsb() : true;
    btn.disabled = !(otaFirmware && port && usbOK);
  }
  if (typeof paintOtaDev === 'function') paintOtaDev();
}

// 设备重启后重连 USB 串口并握手验证版本; 返回新版本字符串或 null
// 09-01 修复: 设备 USB 重新枚举后旧 SerialPort 对象失效, 除了重开旧端口,
//   还通过 navigator.serial.getPorts() 查找重新枚举的新端口并连接。
async function otaReopenAndVerify() {
  let newPort = null;
  for (let i = 0; i < 25 && !otaAbort; i++) {
    // 1) 尝试重开旧 port (某些系统/浏览器下重新枚举后旧对象仍可用)
    if (port) {
      try { try { await port.close(); } catch (e) {} } catch (e) {}
      if (await openSerialPort(port)) { newPort = port; break; }
    }
    // 2) 旧 port 不可用, 从 getPorts() 找重新枚举的新 port
    try {
      const ports = await navigator.serial.getPorts();
      for (const p of ports) {
        if (p === port) continue;   // 跳过已试过的旧对象
        if (await openSerialPort(p)) { newPort = p; break; }
      }
      if (newPort) break;
    } catch (e) { /* getPorts 暂不可用, 等下一轮 */ }
    await sleep(1000);
  }
  if (!newPort) return null;
  if (newPort !== port) {
    port = newPort;
    log('OTA 后设备重新枚举，已连接新串口', 'info');
  }
  // 握手验证版本
  try {
    rxBuffer = new Uint8Array(0);
    const q = buildHeader(CMD_QUERY, 0, 0);
    const w = port.writable.getWriter();
    await w.write(q);
    w.releaseLock();
    const line = await otaReadLine(2000);
    const info = parseDeviceInfo(line);
    return info.version || null;
  } catch (e) {
    return null;
  }
}

// 从串口读取一行(带超时), 用于 OTA 验证握手 / 自动连接设备验证
async function otaReadLine(timeout, p) {
  const target = p || port;
  if (!target || !target.readable) return '';
  const reader = target.readable.getReader();
  const decoder = new TextDecoder();
  let buf = '';
  const deadline = Date.now() + timeout;
  try {
    while (Date.now() < deadline) {
      const remaining = deadline - Date.now();
      if (remaining <= 0) break;
      const { value, done } = await Promise.race([
        reader.read(),
        new Promise(res => setTimeout(() => res({ value: undefined, done: true }), Math.min(200, remaining)))
      ]);
      if (done) break;
      if (value) {
        buf += decoder.decode(value, { stream: true });
        if (buf.includes('\n')) break;
      }
    }
  } finally {
    reader.releaseLock();
  }
  const nl = buf.indexOf('\n');
  if (nl >= 0) buf = buf.slice(0, nl);
  return buf.trim();
}

async function startOta() {
  if (otaTransferring) { cancelOta(); return; }
  if (!otaFirmware) { toast('请先选择固件文件', 'warn'); return; }
  if (!port || !port.writable) { toast('请先连接设备 (USB)', 'warn'); return; }

  const fw = otaFirmware.data;
  // PC 端预检
  if (fw.length + 8 + 4 > OTA_REGION_LIMIT) {
    setOtaStatus('固件过大，超过 OTA 分区上限', true);
    toast('固件过大，无法升级', 'error');
    return;
  }
  if (!isFwValidImage(fw)) {
    setOtaStatus('所选文件不是有效的固件镜像（向量表校验失败），请选择正确的 .bin 固件', true);
    toast('无效固件：向量表校验失败', 'error');
    return;
  }
  if (!containsModelTag(fw)) {
    setOtaStatus('固件文件不含本设备型号标识，可能不是 VibeKey-F3 固件', true);
    toast('非 VibeKey-F3 固件', 'error');
    return;
  }

  otaTransferring = true;
  otaAbort = false;
  otaProgress = 0;
  isBusy = true;                 // 阻塞配置读写/LED-USB, 避免并发抢占串口
  setOtaStatus('正在启动升级...');
  updateOtaUI();
  updateOtaControls();
  setControlsBusy(true);

  // 停止读循环与设备信息轮询, 避免 CONF 查询包混入固件流导致设备端 CRC 校验失败
  keepReading = false;
  if (rxResolve) { rxResolve(); rxResolve = null; }
  if (reader) { try { await reader.cancel(); } catch (e) { /* ignore */ } }

  let otaSucceeded = false;
  try {
    const oldVersion = deviceInfo.version || null;

    // 帧头: OTA_MAGIC + 固件长度 (LE, 不含 CRC)
    const header = new ArrayBuffer(8);
    const hdv = new DataView(header);
    hdv.setUint32(0, OTA_MAGIC, true);
    hdv.setUint32(4, fw.length, true);

    setOtaStatus('发送固件头...');
    let w = port.writable.getWriter();
    await w.write(new Uint8Array(header));
    w.releaseLock();
    await sleep(100);

    // payload = 固件体 + CRC32(LE)
    const crc = crc32(fw);
    const payload = new Uint8Array(fw.length + 4);
    payload.set(fw, 0);
    const cdv = new DataView(payload.buffer);
    cdv.setUint32(fw.length, crc, true);

    setOtaStatus('正在传输固件...');
    const chunkSize = 512;
    let sent = 0;
    while (sent < payload.length) {
      if (otaAbort) throw new Error('CANCELLED');
      const end = Math.min(sent + chunkSize, payload.length);
      const chunk = payload.subarray(sent, end);
      w = port.writable.getWriter();
      await w.write(chunk);
      w.releaseLock();
      sent = end;
      otaProgress = Math.floor(sent * 100 / payload.length);
      updateOtaUI();
    }

    setOtaStatus('固件已发送(含CRC)，等待设备重启...');
    await sleep(5000);   // 等待 reboot + USB 重新枚举

    // 重新握手验证
    setOtaStatus('验证设备是否重新上线...');
    const newVersion = await otaReopenAndVerify();
    if (otaAbort) throw new Error('CANCELLED');
    if (newVersion === null) {
      throw new Error('升级后设备未重新上线，可能已变砖，请检查设备');
    }
    if (oldVersion && oldVersion === newVersion) {
      throw new Error('升级未生效：设备版本未变化（固件校验可能失败），请重试');
    }
    otaSucceeded = true;
    otaProgress = 100;
    updateOtaUI();
    setOtaStatus('升级完成，设备版本 ' + newVersion);
    toast('固件升级完成', 'success');
  } catch (e) {
    if (otaAbort || e.message === 'CANCELLED') {
      setOtaStatus('传输已取消（设备将回退旧固件）');
      toast('已取消升级', 'info');
    } else {
      setOtaStatus('升级失败：' + e.message, true);
      toast('升级失败：' + e.message, 'error');
    }
  } finally {
    otaTransferring = false;
    otaAbort = false;
    isBusy = false;
    if (!otaSucceeded) otaProgress = 0;
    updateOtaUI();
    updateOtaControls();
    setControlsBusy(false);
    // 恢复连接: 重新启动读循环与轮询, 刷新设备信息
    if (port && port.readable) {
      keepReading = true;
      readLoop();
    }
    updateLedChannel();
    const info = await sendQuery();
    if (info && info.name) {
      applyDeviceInfo(info, false);
      // OTA 后设备重启, 重新建立槽位基线, 避免旧基线误判"设备切槽"
      lastReportedDeviceSlot = (info.slot >= 1 && info.slot <= 3) ? info.slot : -1;
    }
  }
}

function cancelOta() {
  if (!otaTransferring) return;
  otaAbort = true;
  setOtaStatus('正在取消...');
}

// ============================================================
// UI 渲染
// ============================================================
// ============================================================
// 通用键块渲染（09-27）
//   17 个键块共用一套渲染逻辑，全部由 KEY_BLOCKS / KEY_GROUPS 的 path 驱动。
//   捕获状态用 capturingPath（字符串路径）而非下标，从而支持任意键块。
// ============================================================
function renderKeys() {
  const host = document.getElementById('keysGrid');
  if (!host) return;
  host.innerHTML = '';
  KEY_GROUPS.forEach(g => host.appendChild(buildGroup(g)));
}

function buildGroup(g) {
  const wrap = document.createElement('div');
  wrap.className = 'key-group';

  let html = `<div class="group-title">${g.title}</div>`;

  // 模式选择器：常规(按下/松开直通) 还是 手势(单击/双击/长按)
  if (g.modePaths && g.modePaths.length) {
    html += '<div class="mode-row">';
    g.modePaths.forEach((p, i) => {
      const v = parseInt(getBlock(p)) || 0;
      html += `<div class="mode-item">
        <span class="mode-label">${g.modeLabels[i]}</span>
        <select onchange="setModeValue('${p}', this.value)">
          <option value="0" ${v === 0 ? 'selected' : ''}>常规（按下 / 松开直通）</option>
          <option value="1" ${v === 1 ? 'selected' : ''}>手势（单击 / 双击 / 长按）</option>
        </select>
      </div>`;
    });
    html += '</div>';
  }

  html += '<div class="keys-grid inner">';
  // 10-05: gestureOnlyBlocks —— 列在其中的块仅在手势模式(modeKey 值为 1)时渲染。
  //   没有该字段的组不受影响（_modeOk 对 modeKey 为 null 的组恒真）。
  const _modeOk = !g.modeKey || (parseInt(getBlock(g.modeKey)) === 1);
  g.blocks.forEach(p => {
    if (!_modeOk && (g.gestureOnlyBlocks || []).includes(p)) return;
    html += buildBlockCard(p);
  });
  html += '</div>';

  wrap.innerHTML = html;
  return wrap;
}

function buildBlockCard(path) {
  const k = getBlock(path);
  if (!k) return '';
  const meta = getMeta(path);
  const colorHex = '#' + (k.rgb_color >>> 0).toString(16).padStart(6, '0');
  const previewBg = k.rgb_enabled ? colorHex : '#000';
  const isCap = (capturingPath === path);
  const show = (a) => (k.action === a ? '' : 'display:none');

  let html = `<div class="key-card${isCap ? ' capturing-card' : ''}">
    <div class="key-card-header"><div class="key-name">`;
  if (meta.rgb) html += `<span class="key-preview" style="background:${previewBg};"></span>`;
  html += `${meta.label}<span class="badge badge-${k.action}">${actionLabel(k.action)}</span>
    </div></div>`;

  // 动作类型（选项由该键允许的动作集决定）
  html += `<div class="form-group"><label>动作类型</label>
    <select onchange="setBlockAction('${path}', this.value)">` +
    actionOptionsHtml(meta.allow, k.action) + `</select></div>`;

  // 修饰键（仅键盘）
  html += `<div class="form-group" style="${show('keyboard')}">
    <label>修饰键 (可多选)</label><div class="modifiers">` +
    MODIFIERS.map(m => `<div class="mod-check ${(k.modifier & m.bit) ? 'active' : ''}"
        onclick="toggleBlockModifier('${path}', ${m.bit}, this)">
        <input type="checkbox" ${(k.modifier & m.bit) ? 'checked' : ''}><span>${m.label}</span>
      </div>`).join('') +
    `</div></div>`;

  // HID 键码 + 按键捕获（仅键盘）
  html += `<div class="form-group" style="${show('keyboard')}">
    <label>HID 键码</label><div class="form-row">
      <select onchange="setBlockKeycode('${path}', parseInt(this.value))">` +
    HID_KEYS.map(h => `<option value="${h.value}" ${k.keycode === h.value ? 'selected' : ''}>${h.text} (0x${h.value.toString(16).toUpperCase().padStart(2, '0')})</option>`).join('') +
    `</select></div>` +
    (meta.capture ? `<button class="capture-btn ${isCap ? 'capturing' : ''}" onclick="startCapture('${path}')">
        ${isCap ? '请按下目标按键... (点击取消)' : '按键捕获 (点击后按键盘)'}</button>` : '') +
    `</div>`;

  // 鼠标键
  html += `<div class="form-group" style="${show('mouse')}">
    <label>鼠标按键</label>
    <select onchange="setBlockKeycode('${path}', parseInt(this.value))">` +
    MOUSE_KEYS.map(m => `<option value="${m.value}" ${k.keycode === m.value ? 'selected' : ''}>${m.text}</option>`).join('') +
    `</select></div>`;

  // 多媒体键
  html += `<div class="form-group" style="${show('multimedia')}">
    <label>多媒体功能</label>
    <select onchange="setBlockKeycode('${path}', parseInt(this.value))">` +
    MEDIA_KEYS.map(m => `<option value="${m.value}" ${k.keycode === m.value ? 'selected' : ''}>${m.text} (0x${m.value.toString(16).toUpperCase()})</option>`).join('') +
    `</select></div>`;

  // 空中鼠标开关：无键码
  if (k.action === 'airmouse') {
    html += `<div class="hint" style="margin-top:6px;">空中鼠标开关：每次触发切换一次开 / 关，无需键码</div>`;
  }

  // RGB 底光（仅 C1-C3 有硬件；其余键块该字段固件存而不用）
  if (meta.rgb) {
    html += `<div class="rgb-section"><div class="rgb-row">
      <span style="font-size:12px;color:var(--text-dim);">RGB 底光</span>
      <div class="toggle ${k.rgb_enabled ? 'on' : ''}" onclick="toggleBlockRgb('${path}', this)"></div>
      <div class="color-picker-wrap">
        <input type="color" value="${colorHex}" onchange="setBlockRgbColor('${path}', this.value)" ${k.rgb_enabled ? '' : 'disabled'}>
      </div>
      <input type="range" min="0" max="100" value="${k.rgb_brightness}"
        oninput="setBlockRgbBrightness('${path}', this.value, this)" ${k.rgb_enabled ? '' : 'disabled'}>
      <span class="brightness-val">${k.rgb_brightness}%</span>
    </div><div class="hint">调整颜色/亮度时实时预览到设备，点击"写入配置"后保存</div></div>`;
  }

  html += `</div>`;
  return html;
}

// 按 allow 列表生成动作下拉项
function actionOptionsHtml(allow, cur) {
  const LABEL = {
    none: '无 (禁用)', keyboard: '键盘按键', mouse: '鼠标按键',
    multimedia: '多媒体键', airmouse: '空中鼠标开关',
  };
  const list = (allow && allow.length) ? allow : ['none', 'keyboard', 'mouse', 'multimedia'];
  return list.map(a => `<option value="${a}" ${cur === a ? 'selected' : ''}>${LABEL[a] || a}</option>`).join('');
}

function actionLabel(a) {
  return { none: '禁用', keyboard: '键盘', mouse: '鼠标', multimedia: '多媒体', airmouse: '空中鼠标' }[a] || a;
}

// ============================================================
// 路径式编辑操作（09-27）
//   全部以 KEY_BLOCKS 的 path 为键（如 'keys.0' / 'c_dbl.1' / 'c1_lng'），
//   从而 17 个键块共用同一套 setter，无需为每个键单独写函数。
// ============================================================
function setBlockAction(path, action) {
  const k = getBlock(path);
  if (!k) return;
  k.action = action;
  if (action === 'mouse') k.keycode = 1;                 // 默认鼠标左键
  else if (action === 'multimedia') k.keycode = 0xE9;    // 默认音量+
  else if (action === 'none' || action === 'airmouse') { k.modifier = 0; k.keycode = 0; }
  renderKeys();
}

function toggleBlockModifier(path, bit, el) {
  const k = getBlock(path);
  if (!k) return;
  if (k.modifier & bit) { k.modifier &= ~bit; el.classList.remove('active'); }
  else { k.modifier |= bit; el.classList.add('active'); }
}

function setBlockKeycode(path, val) {
  const k = getBlock(path);
  if (k) k.keycode = (parseInt(val) || 0) & 0xFF;
}

function toggleBlockRgb(path, el) {
  const k = getBlock(path);
  if (!k) return;
  k.rgb_enabled = !k.rgb_enabled;
  el.classList.toggle('on', k.rgb_enabled);
  renderKeys();                                          // 重新渲染以更新 disabled 状态
  if (k.rgb_enabled) previewRgbPath(path);
}

function setBlockRgbColor(path, hex) {
  const k = getBlock(path);
  if (!k) return;
  const r = parseInt(hex.slice(1, 3), 16);
  const g = parseInt(hex.slice(3, 5), 16);
  const b = parseInt(hex.slice(5, 7), 16);
  k.rgb_color = (r << 16) | (g << 8) | b;
  previewRgbPath(path);
}

function setBlockRgbBrightness(path, val, el) {
  const k = getBlock(path);
  if (!k) return;
  k.rgb_brightness = parseInt(val) || 0;
  if (el && el.nextElementSibling) el.nextElementSibling.textContent = k.rgb_brightness + '%';
  previewRgbPath(path);
}

// 仅有 RGB 硬件的键块（C1-C3）才走实时预览，其余键块该字段固件存而不用
const BLOCK_RGB_INDEX = { 'keys.0': 0, 'keys.1': 1, 'keys.2': 2 };
function previewRgbPath(path) {
  const i = BLOCK_RGB_INDEX[path];
  if (i !== undefined) previewRgb(i);
}

// 模式值（C / L / EC按下：0=常规直通 1=手势）
function setModeValue(path, val) {
  setBlock(path, parseInt(val) || 0);
  // 10-05: 模式切换会影响"仅手势模式显示"的块（KEY_BLOCKS.gestureOnlyBlocks，
  //   目前是 EC 的按压滚动两键），必须重渲染才能让它们出现/消失。
  //   与上方"重新渲染以更新 disabled 状态"的做法一致。
  renderKeys();
}

function updateAirMouse() {
  const em = document.getElementById('airMouseMode');
  const es = document.getElementById('airMouseSpeed');
  const ed = document.getElementById('airMouseDir');
  if (em) config.air_mouse_mode = parseInt(em.value) || 0;
  if (es) config.air_mouse_speed = parseInt(es.value) || 0;
  if (ed) config.air_mouse_dir = parseInt(ed.value) || 0;
}

// 摇一摇：开关 + 灵敏度（触发键走 shake_key 键块）
function updateShake() {
  const en = document.getElementById('shakeEnabled');
  const se = document.getElementById('shakeSens');
  if (en) config.shake_enabled = parseInt(en.value) || 0;
  if (se) config.shake_sens = parseInt(se.value) || 0;
}

// ============================================================
// 按键捕获（路径版：任意键块都可捕获）
// ============================================================
// 10-05: 新增「先按主键、再按修饰键」的组合键捕获。
// 起因: Ctrl+Space 是 Windows 输入法切换热键, keydown **根本不会到达网页**
//       (浏览器/系统层就被吃掉), 所以"按住 Ctrl 再按空格"这条路在网页端怎么都
//       抓不到 → 用户反馈"按 Ctrl+空格设置不了"。
//
// 捕获有三种结束方式, 互不干扰:
//   ① 修饰键 -> 主键: 按下主键时立即组合提交(零等待, 原来的主流用法)
//   ② 主键单独按: **按下时先"暂存"不提交**, 松开后再等 CAP_RELEASE_GRACE_MS;
//      期间若按下修饰键 -> 升级为「修饰键 + 主键」并立即收尾; 没人按修饰键就按
//      主键本身提交(空格->松开 = 空格)。这样"按了空格什么都不想要"不会被
//      1.2s 的升级窗口拖住, 也不会弹两次 toast。
//   ③ 只按一个修饰键: keyup 时提交"纯修饰键"(见 15-capture-edit.js)
const CAP_RELEASE_GRACE_MS = 500;   // 主键松开后, 仍可按修饰键升级的宽限窗口
let stagedKey = null;          // 暂存中的主键: { path, hid, label, code }
let stageTimer = 0;

function _clearStageTimer() {
  if (stageTimer) { clearTimeout(stageTimer); stageTimer = 0; }
}

function _commitStaged() {
  const s = stagedKey;
  stagedKey = null;
  _clearStageTimer();
  if (!s || capturingPath !== s.path) return;
  commitCapture(captureMods, s.hid, s.label);
}

function startCapture(path) {
  if (capturingPath === path) { cancelCapture(); return; }
  capturingPath = path;
  captureMods = 0;
  stagedKey = null;
  _clearStageTimer();
  renderKeys();
  log('开始捕获「' + getMeta(path).label + '」的按键（可先按主键，再按修饰键组成组合键）...', 'info');
}

function cancelCapture() {
  capturingPath = '';
  captureMods = 0;
  stagedKey = null;
  _clearStageTimer();
  renderKeys();
}

/* 提交「修饰键 + 主键」到正在捕获的键块, 并结束捕获(只弹一次 toast)。 */
function commitCapture(mod, hid, keyLabel) {
  const path = capturingPath;
  const k = getBlock(path);
  if (!k) { cancelCapture(); return; }
  const label = getMeta(path).label;
  k.action = 'keyboard';
  k.modifier = mod & 0xff;
  k.keycode = hid;
  const pretty = webFormatShortcut(k.modifier, k.keycode) || keyLabel;
  log('✓ 捕获「' + label + '」: mod=0x' + k.modifier.toString(16) + ' keycode=' + hid + ' → ' + pretty, 'success');
  toast('捕获成功: ' + pretty, 'success');
  refreshSummaries();
  paintShortcutBoxes();
  paintGestureCards();
  cancelCapture();
}

const CAP_MOD_MAP = {
  'ControlLeft': 0x01, 'ControlRight': 0x10,
  'ShiftLeft': 0x02, 'ShiftRight': 0x20,
  'AltLeft': 0x04, 'AltRight': 0x40,
  'MetaLeft': 0x08, 'MetaRight': 0x80,
};

// 全局键盘监听用于捕获
document.addEventListener('keydown', (e) => {
  if (!capturingPath) return;
  e.preventDefault();
  e.stopPropagation();

  // 修饰键
  if (CAP_MOD_MAP[e.code]) {
    captureMods |= CAP_MOD_MAP[e.code];
    // 暂存了主键 => 组装成组合键并立即收尾(支持"按住主键再按 Alt"和"松开主键再按 Alt"两种顺序)
    if (stagedKey && stagedKey.path === capturingPath) _commitStaged();
    return;
  }

  // 普通键
  const hid = jsKeyToHid(e);
  if (hid < 0) return;
  if (captureMods) {                 // ① 先修饰键后主键: 立即组合提交
    commitCapture(captureMods, hid, e.key);
  } else {                            // ② 单独主键: 先暂存, 松开(或宽限期到)才提交
    stagedKey = { path: capturingPath, hid, label: e.key, code: e.code };
    renderKeys();
    paintShortcutBoxes();
    log('已暂存主键「' + e.key + '」：直接松开 = 只设这个键；松开前/后再按一个修饰键 = 组成组合键');
  }
}, true);

document.addEventListener('keyup', (e) => {
  if (!capturingPath) return;
  // 主键松开 -> 开一个宽限窗口: 期间按修饰键就升级为组合键, 否则按主键本身提交
  if (stagedKey && stagedKey.code === e.code) {
    _clearStageTimer();
    stageTimer = setTimeout(_commitStaged, CAP_RELEASE_GRACE_MS);
    return;
  }
  // 修饰键: 暂存主键期间不清位(否则主键会被当纯修饰键提交/丢掉)
  if (CAP_MOD_MAP[e.code]) {
    if (stagedKey && stagedKey.path === capturingPath) return;
    captureMods &= ~CAP_MOD_MAP[e.code];
  }
}, true);

function jsKeyToHid(e) {
  // 字母 A-Z
  if (e.code.startsWith('Key')) {
    return e.code.charCodeAt(3) - 'A'.charCodeAt(0) + 4;
  }
  // 数字 0-9
  if (e.code.startsWith('Digit')) {
    const n = parseInt(e.code.slice(5));
    return n === 0 ? 39 : n + 29;
  }
  // F1-F12
  if (e.code.startsWith('F') && /^F\d+$/.test(e.code)) {
    const n = parseInt(e.code.slice(1));
    if (n >= 1 && n <= 12) return n + 57;
  }
  // 其他常用键
  const map = {
    'Enter': 40, 'NumpadEnter': 40, 'Escape': 41,
    'Backspace': 42, 'Tab': 43, 'Space': 44,
    'Minus': 45, 'Equal': 46, 'BracketLeft': 47,
    'BracketRight': 48, 'Backslash': 49, 'Semicolon': 51,
    'Quote': 52, 'Backquote': 53, 'Comma': 54,
    'Period': 55, 'Slash': 56, 'CapsLock': 57,
    'PrintScreen': 70, 'ScrollLock': 71, 'Pause': 72,
    'Insert': 73, 'Home': 74, 'PageUp': 75,
    'Delete': 76, 'End': 77, 'PageDown': 78,
    'ArrowRight': 79, 'ArrowLeft': 80, 'ArrowDown': 81, 'ArrowUp': 82,
    'Numpad0': 98, 'Numpad1': 89, 'Numpad2': 90, 'Numpad3': 91,
    'Numpad4': 92, 'Numpad5': 93, 'Numpad6': 94, 'Numpad7': 95,
    'Numpad8': 96, 'Numpad9': 97,
  };
  return map[e.code] ?? -1;
}

// ============================================================
// 初始化
// ============================================================
window.addEventListener('load', () => {
  // 初始化主题按钮 (head 内联脚本已提前设置 data-theme 防闪烁)
  applyTheme(localStorage.getItem(THEME_KEY) || 'auto');
  renderKeys();
  updateSlotUI();
  updateSleepUI();
  updateOtaUI();
  updateOtaControls();
  log('VibeKey-F3 已就绪 (C/L/EC 三手势 · 摇一摇 · 空中鼠标 · 槽位 · 休眠 · LED · OTA)', 'success');
  if (!('serial' in navigator)) {
    log('警告: 当前浏览器不支持 Web Serial API', 'error');
    toast('请使用 Chrome 或 Edge 浏览器', 'error');
  }
  // 09-01: 自动识别并连接已授权的 USB 设备; 热插拔时也自动连接
  if ('serial' in navigator) {
    autoConnectUsb();
    navigator.serial.addEventListener('connect', () => {
      if (!port && !otaTransferring) autoConnectUsb();
    });
  }
  // 08-31: 周期轮询设备信息(电量/槽位同步)
  setInterval(pollDeviceInfo, 5000);
});

