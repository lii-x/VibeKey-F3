/* ===== 虚拟键盘 (仅网页端) =====
   从 build 顺序末位新增(2026-10-05)。依赖前面已定义的
   getBlock / webFormatShortcut / paintShortcutBoxes / commitCapture 所在的数据模型。

   为什么需要它:
     网页端捕获组合键靠 document 的真实 keydown, 但 Windows 输入法/系统热键
     (最典型就是 Ctrl+空格 = 中英切换) 会在事件到达网页之前就吃掉组合键,
     用户按了没反应、也设不上。屏幕键盘完全绕开系统热键 —— 这是网页端独有的兜底,
     桌面端 QML 直接读原生键盘事件, 不需要这层。

   写入目标的选择:
     弹窗里快捷键框可能有多个(手势模式 3~5 个, EC 4 个), 写错框是静默失败,
     所以面板顶部必须有「目标」下拉, 打开时默认选中正在捕获的那个(没有就选第一个)。

   面板与入口的显隐不写在 openEditor/closeEditor 里, 而是用 CSS 兄弟选择器
   (#editorModal.show ~ .vkbd-fab) —— 见 css/06-vkbd.css 顶部说明。 */
/* 修饰键位图: 左右各 4 个, 与 HID 标准键盘报告第 0 字节一致
   (bit0 LCtrl / bit1 LShift / bit2 LAlt / bit3 LWin
    bit4 RCtrl / bit5 RShift / bit6 RAlt / bit7 RWin)。
   固件是整字节透传进 HID report[0] (Firmware/src/ble_hid.c:225), 不做左右区分,
   所以左右手都能生效 —— 出厂默认槽位 2 的 C1 就是 RAlt(modifier=0x40), 是现成的实证。
   ★ 这里的 label 用 MODIFIERS 里的全名(LCtrl/RShift/...)。虚拟键盘同时支持左右手,
     若沿用 'Ctrl' 这种简称, 写入 RAlt 后显示成 "Ctrl" 会让用户以为没生效。 */
const VKBD_MODS_L = [
  { bit: 0x01, label: 'LCtrl' }, { bit: 0x02, label: 'LShift' },
  { bit: 0x04, label: 'LAlt' }, { bit: 0x08, label: 'LWin' },
];
const VKBD_MODS_R = [
  { bit: 0x10, label: 'RCtrl' }, { bit: 0x20, label: 'RShift' },
  { bit: 0x40, label: 'RAlt' }, { bit: 0x80, label: 'RWin' },
];
/* 键帽表: 直接给 HID 码, 不走 jsKeyToHid —— 那边要 KeyboardEvent.code,
   屏幕键盘没有真实事件, 硬造一个 e 对象只会把映射表复制一份、易失配。
   修饰键帽写成 [名字, 修饰位, 1], 第三位为 1 表示"这是修饰键, 不写入主键码"。
   ★ 修饰位必须**逐个键帽写死**, 不能靠名字查表: 底排里 Shift/Alt/Ctrl 各出现两次
     (左手一个、右手一个), 名字查表天然二义 —— 早期版本正是因此把左右手全写成左手位。 */
const VKBD_ROWS = [
  [['Esc', 41], ['F1', 58], ['F2', 59], ['F3', 60], ['F4', 61], ['F5', 62],
   ['F6', 63], ['F7', 64], ['F8', 65], ['F9', 66], ['F10', 67], ['F11', 68], ['F12', 69]],
  [['`', 53], ['1', 30], ['2', 31], ['3', 32], ['4', 33], ['5', 34], ['6', 35],
   ['7', 36], ['8', 37], ['9', 38], ['0', 39], ['-', 45], ['=', 46], ['Bksp', 42]],
  [['Tab', 43], ['Q', 20], ['W', 26], ['E', 8], ['R', 21], ['T', 23], ['Y', 28],
   ['U', 24], ['I', 12], ['O', 18], ['P', 19], ['[', 47], [']', 48], ['\\', 49]],
  [['Caps', 57], ['A', 4], ['S', 22], ['D', 7], ['F', 9], ['G', 10], ['H', 11],
   ['J', 13], ['K', 14], ['L', 15], [';', 51], ["'", 52], ['Enter', 40]],
  /* 第 5 行: 左 Shift 在最左, 右 Shift 在最右 —— 与真实键盘位置一致, 点哪个就是哪个手 */
  [['Shift', 0x02, 1], ['Z', 29], ['X', 27], ['C', 6], ['V', 25], ['B', 5], ['N', 17],
   ['M', 16], [',', 54], ['.', 55], ['/', 56], ['Shift', 0x20, 1]],
  [['Ctrl', 0x01, 1], ['Alt', 0x04, 1], ['Win', 0x08, 1], ['Space', 44],
   ['Alt', 0x40, 1], ['Ctrl', 0x10, 1], ['←', 80], ['↑', 82], ['↓', 81], ['→', 79]],
  [['Ins', 73], ['Home', 74], ['PgUp', 75], ['Del', 76], ['End', 77], ['PgDn', 78]],
];
/* 修饰键帽的显示名映射: 只用于 title/日志, 让用户悬停就能确认"这是右手的"。
   键帽本身显示短名(Shift/Ctrl/...), 靠**位置**表达左右手 —— 与真实键盘一致。 */
const VKBD_MOD_TITLE = {
  0x01: '左 Ctrl (LCtrl)', 0x02: '左 Shift (LShift)', 0x04: '左 Alt (LAlt)', 0x08: '左 Win (LWin)',
  0x10: '右 Ctrl (RCtrl)', 0x20: '右 Shift (RShift)', 0x40: '右 Alt (RAlt)', 0x80: '右 Win (RWin)',
};

let _vkbdPath = '';        // 当前写入目标
let _vkbdMods = 0;         // 已点选的修饰键位图
let _vkbdRight = false;    // 顶部修饰键条当前显示左手还是右手(行内键帽不受它影响)

/* 收集弹窗内所有可写快捷键的块。摇一摇的快捷键框没有 data-kb-path(它由
   toggleShakeCapture 走 'shake_key' 路径), 这里补进来, 否则摇一摇用不了屏幕键盘。 */
function vkbdTargets() {
  const out = [];
  document.querySelectorAll('#editorModalBody .ed-kb[data-kb-path]').forEach(box => {
    const p = box.dataset.kbPath;
    out.push({ path: p, label: (getMeta(p) || {}).label || p });
  });
  if (document.getElementById('shakeKbBox')) {
    out.push({ path: 'shake_key', label: (getMeta('shake_key') || {}).label || '摇一摇' });
  }
  return out;
}
/* 重建「写入目标」下拉, 并决定选中项。
   ★ 为什么必须能"重建": 手势切换(_setGes)/模式切换(setModeValue)/改大类都会调 renderModal()
     把 #editorModalBody 整个换掉 —— 常规模式的 keys.0 消失、换成 c_dbl.0。
     若下拉只在打开时建一次, 切到手势模式后它仍指向 keys.0, 屏幕键盘会把组合键
     写到**用户看不见的那个块**上(静默写错, 比报错更难查)。用户可见的框变了, 下拉必须跟着变。

   选中项的解析顺序(优先从上):
     1. capturingPath —— 用户正在捕获的框。这是最强的意图信号, 必须压过一切。
     2. 上次选中的 _vkbdPath, 若它在新列表里仍然存在(例如 EC 三个框都在, 只换了手势页)。
     3. 新列表的第一个。 */
function vkbdSyncTargets() {
  const sel = document.getElementById('vkbdTarget');
  if (!sel) return [];
  const ts = vkbdTargets();
  sel.innerHTML = ts.map(t => `<option value="${t.path}">${t.label}</option>`).join('')
    || '<option value="">（弹窗内没有可设置的快捷键）</option>';
  const want = (capturingPath && ts.some(t => t.path === capturingPath))
    ? capturingPath
    : (ts.some(t => t.path === _vkbdPath) ? _vkbdPath : (ts[0] ? ts[0].path : ''));
  sel.value = want;
  _vkbdPath = want;
  return ts;
}
function vkbdBuildPanel() {
  const panel = document.getElementById('vkbd');
  if (!panel) return;
  const sel = document.getElementById('vkbdTarget');
  const rows = document.getElementById('vkbdKeys');
  const mods = document.getElementById('vkbdMods');
  if (!sel || !rows || !mods) return;

  vkbdSyncTargets();

  vkbdBuildMods();
  rows.innerHTML = VKBD_ROWS.map(r => '<div class="vkbd-row">' + r.map(k => {
    /* 三元组 [名字, 位/HID码, 1] = 修饰键帽; 二元组 [名字, HID码] = 普通键。
       修饰键帽的位是**逐个写死**的(见 VKBD_ROWS 注释), 不用名字查表。 */
    if (k[2]) {
      const title = VKBD_MOD_TITLE[k[1]] || k[0];
      return `<button type="button" class="vkbd-key vkbd-mod" data-bit="${k[1]}" `
        + `data-label="${k[0]}" title="${title}">${k[0]}</button>`;
    }
    return `<button type="button" class="vkbd-key" data-hid="${k[1]}" `
      + `data-label="${k[0]}">${k[0]}</button>`;
  }).join('') + '</div>').join('');
  paintVkbdMods();
}
/* 顶部修饰键条: 左手/右手两组切换。
   为什么需要这个切换: 行内键帽已经能按位置区分左右手了, 但真实键盘上有些布局
   (尤其是 60% 配列) 右手修饰键不好认, 且用户想快速点"右边那组"时不该在
   底排里找。切到右手后四个键帽变成 RCtrl/RShift/RAlt/RWin。 */
function vkbdBuildMods() {
  const mods = document.getElementById('vkbdMods');
  if (!mods) return;
  const list = _vkbdRight ? VKBD_MODS_R : VKBD_MODS_L;
  mods.innerHTML = list
    .map(m => `<button type="button" class="vkbd-mod" data-bit="${m.bit}" `
      + `data-label="${m.label}" title="${VKBD_MOD_TITLE[m.bit]}">${m.label}</button>`)
    .join('');
}
function toggleVkbd() {
  const panel = document.getElementById('vkbd');
  if (!panel) return;
  const on = !panel.classList.contains('show');
  if (on) vkbdBuildPanel();
  panel.classList.toggle('show', on);
  log(on ? '已打开虚拟键盘（可绕过 Ctrl+空格 这类被系统吃掉的组合键）' : '已关闭虚拟键盘');
}
/* 关闭面板并清掉已点选的修饰键。
   由 closeEditor 调用: 弹窗一关, 目标块(_modalPath)已失效, 留着修饰键位图
   会让下次打开时莫名其妙带着上次的修饰键。 */
function vkbdClose() {
  const panel = document.getElementById('vkbd');
  if (panel) panel.classList.remove('show');
  _vkbdMods = 0;
  _vkbdRight = false;
  _vkbdPath = '';
  const btn = document.getElementById('vkbdHand');
  if (btn) { btn.textContent = '左手'; btn.classList.remove('right'); }
  /* 位图清零后必须重绘一次: 不重绘的话键帽上的 .on 高亮会留在 DOM 上,
     面板下次打开时(vkbdBuildPanel 会重绘)虽会自愈, 但关闭状态下残留高亮
     属于状态不一致, 也让"关闭后是否真的清干净"无法断言。 */
  paintVkbdMods();
  const prev = document.getElementById('vkbdPrev');
  if (prev) prev.textContent = '';
}
function paintVkbdMods() {
  document.querySelectorAll('#vkbd .vkbd-mod').forEach(b => {
    b.classList.toggle('on', !!(Number(b.dataset.bit) & _vkbdMods));
  });
  const prev = document.getElementById('vkbdPrev');
  if (prev) {
    /* 用 MODIFIERS(全 8 位, 带 LCtrl/RAlt 这类全名)而不是 VKBD_MODS_L/R:
       顶部只显示当前那 4 个, 但已选中的可能来自另一只手(行内键帽), 预览要能全量报出。 */
    const names = MODIFIERS.filter(m => m.bit & _vkbdMods).map(m => m.label);
    prev.textContent = names.length
      ? '已选修饰键：' + names.join('+') + ' → 再点一个字母/数字键完成'
      : '未选修饰键：直接点字母/数字键即为单键（也可先点 Shift/Ctrl/Alt/Win；'
        + '顶部可切左右手，行内键帽按位置区分）';
  }
}
/* 顶部修饰键条切左右手。已点选的修饰位**不清零** —— 用户常是"左手 Ctrl + 右手 Alt"
   这样混搭, 切手只该换键帽显示, 不该丢已选状态。 */
function toggleVkbdHand() {
  _vkbdRight = !_vkbdRight;
  vkbdBuildMods();
  paintVkbdMods();
  const btn = document.getElementById('vkbdHand');
  if (btn) {
    btn.textContent = _vkbdRight ? '右手' : '左手';
    btn.classList.toggle('right', _vkbdRight);
  }
  log('虚拟键盘修饰键条 → ' + (_vkbdRight ? '右手 (RCtrl/RShift/RAlt/RWin)' : '左手 (LCtrl/LShift/LAlt/LWin)'));
}
/* 写入目标块。抽成独立函数而不是复用 commitCapture: 后者吃 capturingPath
   且写完就 cancelCapture, 屏幕键盘要连续设置多个框, 不能每写一个就打断捕获态。 */
function vkbdWrite(hid, label) {
  const path = _vkbdPath;
  if (!path) { toast('请先在虚拟键盘顶部选择写入目标', 'info'); return; }
  const k = getBlock(path);
  if (!k) return;
  k.action = 'keyboard';
  k.modifier = _vkbdMods & 0xff;
  k.keycode = hid;
  const pretty = webFormatShortcut(k.modifier, k.keycode) || label;
  log('✓ 虚拟键盘设置「' + ((getMeta(path) || {}).label || path) + '」: mod=0x'
    + k.modifier.toString(16) + ' keycode=' + hid + ' → ' + pretty, 'success');
  toast('已设置: ' + pretty, 'success');
  _vkbdMods = 0;
  // 大类下拉要跟着切到「键盘组合」, 否则快捷键框仍是隐藏的, 用户看不到结果
  paintBlockRows();
  paintShortcutBoxes();
  paintGestureCards();
  if (path === 'shake_key') paintShakeUi();
  refreshSummaries();
  paintVkbdMods();
}
function vkbdClear() {
  const path = _vkbdPath;
  if (!path) return;
  const k = getBlock(path);
  if (!k) return;
  k.action = 'none'; k.modifier = 0; k.keycode = 0;
  log('已清除「' + ((getMeta(path) || {}).label || path) + '」的快捷键');
  toast('已清除', 'info');
  _vkbdMods = 0;
  paintBlockRows();
  paintShortcutBoxes();
  paintGestureCards();
  if (path === 'shake_key') paintShakeUi();
  refreshSummaries();
  paintVkbdMods();
}
document.addEventListener('click', (e) => {
  const btn = e.target.closest ? e.target.closest('#vkbd .vkbd-mod, #vkbd .vkbd-key') : null;
  if (!btn) return;
  e.preventDefault();
  const bit = Number(btn.dataset.bit);
  if (bit) {                       // 修饰键: 切换位, 不写入
    _vkbdMods ^= bit;
    paintVkbdMods();
    return;
  }
  if (btn.dataset.hid) vkbdWrite(Number(btn.dataset.hid), btn.dataset.label);
}, true);
// 顶部目标下拉
document.addEventListener('change', (e) => {
  const sel = e.target;
  if (sel && sel.id === 'vkbdTarget') {
    _vkbdPath = sel.value;
    log('虚拟键盘写入目标 → ' + ((getMeta(sel.value) || {}).label || sel.value || '无'));
  }
});
/* 「仅修饰键」: 复用 vkbdWrite 传 hid=0 —— QML 约定 keycode=0 即纯修饰键
   (与 commitModifierOnlyCapture 同一套语义, 见 formatShortcut 157-164)。 */
document.addEventListener('click', (e) => {
  const b = e.target.closest ? e.target.closest('#vkbdModsOnly, #vkbdClear') : null;
  if (!b) return;
  e.preventDefault();
  if (b.id === 'vkbdClear') vkbdClear();
  else {
    if (!_vkbdMods) { toast('请先点选 Ctrl / Shift / Alt / Win', 'info'); return; }
    vkbdWrite(0, webFormatShortcut(_vkbdMods, 0));
  }
}, true);
