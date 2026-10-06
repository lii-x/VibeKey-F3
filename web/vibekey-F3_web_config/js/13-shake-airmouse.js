/* ===== 摇一摇弹窗 + 空中鼠标设置 + RGB 取色器渲染与绘制 =====
   从 assemble_index.py 的内嵌字符串拆出(2026-10-03)。目的: 让这些逻辑成为可独立
   编辑/语法高亮/逐文件 node --check 的 .js，而不是 1200 行 Python 字符串。
   拼接顺序 = 文件名序号，勿调整。 */
// ===== 摇一摇弹窗逻辑 (对齐 QML 4381-4518) =====
// QML: 说明文字 / 启用开关(50x26 圆角开关) / 灵敏度三段按钮(轻中高) /
//      触发快捷键框(52 高, 点击进入捕获) / 清除+完成 / 底部红色提示
function shakeShortcutText() {                 // 对齐 QML shakeShortcutText
  const k = getBlock('shake_key');
  if (!k || k.action === 'none') return '未设置';
  if (k.action === 'keyboard') return webFormatShortcut(k.modifier, k.keycode) || '未设置';
  if (k.action === 'multimedia') {
    const m = MEDIA_KEYS.find(x => x.value === k.keycode);
    return m ? m.text : '多媒体';
  }
  if (k.action === 'mouse') {
    const m = MOUSE_KEYS.find(x => x.value === k.keycode);
    return m ? m.text : '鼠标';
  }
  return '未设置';
}
function toggleShakeEnabled() {
  config.shake_enabled = config.shake_enabled ? 0 : 1;
  refreshSummaries();
  paintShakeUi();
}
function setShakeSens(i) {
  config.shake_sens = i;
  refreshSummaries();
  paintShakeUi();
}
function clearShakeKey() {                      // 对齐 QML clearShakeKey
  const k = getBlock('shake_key');
  if (!k) return;
  k.action = 'none'; k.modifier = 0; k.keycode = 0;
  refreshSummaries();
  paintShakeUi();
}
function toggleShakeCapture() {                 // 捕获用 captureIndex === -2 (QML), 网页端用路径
  if (capturingPath === 'shake_key') cancelCapture();
  else startCapture('shake_key');
}
function paintShakeUi() {
  const sw = document.getElementById('shakeSwitch');
  if (sw) {
    const on = !!config.shake_enabled;
    sw.classList.toggle('on', on);
    sw.firstElementChild.textContent = on ? '开' : '关';
  }
  const segs = document.querySelectorAll('.seg-fill .seg-btn');
  for (let i = 0; i < segs.length; i++) segs[i].classList.toggle('active', (parseInt(config.shake_sens) || 0) === i);
  const box = document.getElementById('shakeKbBox');
  const txt = document.getElementById('shakeKbText');
  if (txt) {
    const cap = (capturingPath === 'shake_key');
    txt.textContent = cap ? '捕获中…' : shakeShortcutText();
    txt.classList.toggle('capturing', cap);
  }
  if (box) box.classList.toggle('capturing', (capturingPath === 'shake_key'));
  const note = document.getElementById('shakeNote');
  if (note) {
    note.textContent = (capturingPath === 'shake_key')
      ? '捕获中：请在键盘上按下想触发的组合键（纯修饰键也可，如仅 Ctrl+Alt）'
      : '设置完成后请点击上方"写入"按钮保存到当前蓝牙槽位';
  }
}

let _modalPath = null;
let _ges = 0;   // 当前编辑的手势 0=单击 1=双击 2=长按

// 由 Summary 卡片的 data-path 解析出「单键编辑目标」(对齐 Studio 的 per-key 弹窗)
function resolveTarget(path) {
  if (!path) return null;          // 未打开弹窗时 _modalPath 为 null, 下面 path.match 会抛
  let m;
  if ((m = path.match(/^keys\.(\d)$/))) {
    const i = m[1];
    return { kind: 'C', idx: +i, title: 'KEY C' + (+i + 1), rgbPath: path,
             modePath: 'c_mode.' + i, reg: path,
             ges: ['c_tap.' + i, 'c_dbl.' + i, 'c_lng.' + i] };
  }
  if ((m = path.match(/^l_key\.(\d)$/))) {
    const i = m[1];
    return { kind: 'L', idx: +i, title: 'KEY L' + (+i + 1),
             modePath: 'l_mode.' + i, reg: path,
             ges: ['l_tap.' + i, 'l_dbl.' + i, 'l_lng.' + i] };
  }
  if (path === 'ec_press') {
    return { kind: 'EC', title: 'EC 编码器', modePath: 'ec_press_mode', reg: 'ec_press',
             ges: ['ec_press_tap', 'ec_press_dbl', 'ec_press_lng'] };
  }
  if (path === 'shake_key') return { kind: 'SHAKE', title: '摇一摇设置' };
  return null;
}
function isGesture(t) { return !!t.modePath && (parseInt(getBlock(t.modePath)) || 0) === 1; }
// L1: 相关键块中任一个动作设为空中鼠标时, 才显示空中鼠标专属设置 (对齐 Studio)
function airmouseOnL1() {
  return ['l_key.0', 'l_tap.0', 'l_dbl.0', 'l_lng.0'].some(p => {
    const k = getBlock(p); return k && k.action === 'airmouse';
  });
}
function modeSelectHTML(t) {
  const v = parseInt(getBlock(t.modePath)) || 0;
  return `<select class="ed-sel" onchange="setModeValue('${t.modePath}', this.value)">
      <option value="0"${v === 0 ? ' selected' : ''}>常规模式</option>
      <option value="1"${v === 1 ? ' selected' : ''}>手势模式</option>
    </select>`;
}
function modeRowHTML(t) {
  const v = parseInt(getBlock(t.modePath)) || 0;
  // 提示文字与下拉同行右对齐 (QML 2963-2971: Layout.fillWidth + horizontalAlignment: AlignRight)
  return `<div class="ed-row"><span class="ed-label">模式</span>${modeSelectHTML(t)}
      <span class="hint ed-row-hint">${v === 1 ? '可配置单击/双击/长按' : '按下即发/松开即发'}</span></div>`;
}
const GESTURE_NAMES = ['单击', '双击', '长按'];
// 手势区: 复刻 QML 的「三张手势卡片装在灰盒里」(Main.qml 3274-3340)
// 盒 radius8 #f7f7f7 + margins10 + spacing4; 卡 fillWidth x52 radius6,
// 选中 #e6f5ec + #07c160 边, 未选 白底 + #d5e0da 边; 卡内标题 10pt 粗(选中绿/否则 #666) + 状态 8pt #333
function gestureTabsHTML(t) {
  return `<div class="ed-ges">
    <div class="ed-ges-row">${GESTURE_NAMES.map((n, i) => `
      <div class="ed-ges-card${_ges === i ? ' on' : ''}" data-g="${i}" onclick="_setGes(${i})">
        <span class="ed-ges-t">${n}</span>
        <span class="ed-ges-s"></span>
      </div>`).join('')}</div>
    <div class="ed-ges-hint">点卡片选手势 · 单击/双击(窗 220ms)/长按(800ms)${t.kind === 'L' && t.idx === 0 ? ' · L1 手势可设空中鼠标' : ''}</div>
  </div>`;
}
// 卡片里的状态文字 = 该手势当前动作摘要 (QML lKeyGestureStatusText / cKeyStatusText)
function paintGestureCards() {
  if (!_modalPath) return;
  const t = resolveTarget(_modalPath);
  if (!t || !t.ges) return;
  document.querySelectorAll('#editorModalBody .ed-ges-card').forEach(card => {
    const s = card.querySelector('.ed-ges-s');
    if (!s) return;
    const g = +card.dataset.g;
    const blk = getBlock(t.ges[g]);
    const txt = keySummaryText(blk);
    s.textContent = (!blk || blk.action === 'none' || !txt) ? '未设置' : txt;
  });
}
function _setGes(i) { _ges = i; renderModal(); }
// 「按下」块动作行标签跟着模式/手势变 (QML 4815)
function paintEcActLabel() {
  const el = document.querySelector('#editorModalBody .ec-act-label');
  if (!el || !_modalPath) return;
  const t = resolveTarget(_modalPath);
  el.textContent = (t && isGesture(t)) ? GESTURE_NAMES[_ges] : '动作';
}
// 单个键块编辑器 (复用原 buildBlockCard, 在弹窗内由 CSS 隐去卡片标题与 RGB 段)
function blockEditorHTML(path) { return `<div class="ed-block" data-path="${path}">${buildBlockCard(path)}</div>`; }
// RGB 底光独立渲染 (仅 C1-C3 有硬件, 与当前是否常规/手势模式无关)
// 结构与状态完全对齐 QML 4209-4307: HSV 三元组 (pHue 0-360 / pSat / pVal) 驱动
// SV 面板 + 色相条 + 8 个预设色 + 开关, 而不是网页原生的 color/range 控件
const _rgbHsv = { h: 0, s: 1, v: 1 };
const RGB_PRESETS = [0xFF3B30, 0xFF9500, 0xFFCC00, 0x34C759, 0x00C7BE, 0x007AFF, 0x5856D6, 0xFFFFFF];
function _hex2(v) { v = Math.max(0, Math.min(255, Math.round(v))); const t = v.toString(16); return t.length < 2 ? '0' + t : t; }
function rgbToHsv(r, g, b) {
  r /= 255; g /= 255; b /= 255;
  const mx = Math.max(r, g, b), mn = Math.min(r, g, b), d = mx - mn;
  let h = 0;
  if (d) {
    if (mx === r) h = ((g - b) / d) % 6;
    else if (mx === g) h = (b - r) / d + 2;
    else h = (r - g) / d + 4;
    h *= 60; if (h < 0) h += 360;
  }
  return { h: h, s: mx ? d / mx : 0, v: mx };
}
function hsvToRgb(h, s, v) {
  h = ((h % 360) + 360) % 360;
  const c = v * s, x = c * (1 - Math.abs((h / 60) % 2 - 1)), m = v - c;
  let r = 0, g = 0, b = 0;
  if (h < 60) { r = c; g = x; } else if (h < 120) { r = x; g = c; }
  else if (h < 180) { g = c; b = x; } else if (h < 240) { g = x; b = c; }
  else if (h < 300) { r = x; b = c; } else { r = c; b = x; }
  return { r: Math.round((r + m) * 255), g: Math.round((g + m) * 255), b: Math.round((b + m) * 255) };
}
function hsvHex() {
  const c = hsvToRgb(_rgbHsv.h, _rgbHsv.s, _rgbHsv.v);
  return '#' + _hex2(c.r) + _hex2(c.g) + _hex2(c.b);
}
function hsvRgba(a) {
  const c = hsvToRgb(_rgbHsv.h, _rgbHsv.s, _rgbHsv.v);
  return 'rgba(' + c.r + ',' + c.g + ',' + c.b + ',' + (a === undefined ? 1 : a) + ')';
}
// 只刷新取色器自身, 不重建弹窗 DOM —— 拖拽 SV/色相条时必须靠这个
function paintRgbUi() {
  const $ = id => document.getElementById(id);
  const hex = hsvHex();
  const k = (_modalPath ? getBlock(_modalPath) : null);
  const chip = $('rgbChip');
  if (chip) chip.style.background = (k && !k.rgb_enabled) ? '#222222' : hex;
  const chip2 = $('rgbValChip');
  if (chip2) chip2.style.background = hex;
  const hx = $('rgbHex');
  if (hx) hx.textContent = hex.toUpperCase();
  const sv = $('rgbSv');
  if (sv) {
    sv.style.setProperty('--base', hsvRgba(1));
    const dot = sv.querySelector('.rgb-sv-dot');
    if (dot) { dot.style.left = (_rgbHsv.s * 100) + '%'; dot.style.top = ((1 - _rgbHsv.v) * 100) + '%'; }
  }
  const hue = $('rgbHue');
  if (hue) {
    const d = hue.querySelector('.rgb-hue-dot');
    if (d) d.style.left = (_rgbHsv.h / 360 * 100) + '%';
  }
  if (k) {
    RGB_PRESETS.forEach((c, i) => {
      const el = document.querySelector('.rgb-preset[data-i="' + i + '"]');
      if (el) el.classList.toggle('sel', (k.rgb_color >>> 0) === c);
    });
    const sw = $('rgbSwitch');
    if (sw) { sw.classList.toggle('on', !!k.rgb_enabled); sw.firstElementChild.textContent = k.rgb_enabled ? '开' : '关'; }
    const brs = $('rgbBrs');
    if (brs) {
      const p = Math.max(0, Math.min(100, parseInt(k.rgb_brightness) || 0));
      const fill = brs.querySelector('.rgb-fill'), knob = brs.querySelector('.rgb-knob');
      if (fill) { fill.style.width = p + '%'; fill.style.background = hex; }
      if (knob) { knob.style.left = p + '%'; knob.style.borderColor = hex; }
      const bv = $('rgbBrsVal');
      if (bv) bv.textContent = p + '%';
    }
  }
}
function applyRgbColor() {                    // HSV -> 写配置 + 实时预览到设备 (QML applyColor)
  if (!_modalPath) return;
  setBlockRgbColor(_modalPath, hsvHex());
  paintRgbUi();
}
function syncRgbFromBlock() {                 // 配置色 -> HSV (QML syncFromColor 2808)
  const k = (_modalPath ? getBlock(_modalPath) : null);
  if (!k) return;
  const c = k.rgb_color >>> 0;
  const hsv = rgbToHsv((c >> 16) & 255, (c >> 8) & 255, c & 255);
  _rgbHsv.h = hsv.h; _rgbHsv.s = hsv.s; _rgbHsv.v = hsv.v;
  paintRgbUi();
}
function setRgbPreset(i) {
  const c = RGB_PRESETS[i];
  if (c === undefined) return;
  const hsv = rgbToHsv((c >> 16) & 255, (c >> 8) & 255, c & 255);
  _rgbHsv.h = hsv.h; _rgbHsv.s = hsv.s; _rgbHsv.v = hsv.v;
  applyRgbColor();
}
function toggleRgbSwitch() {
  if (!_modalPath) return;
  toggleBlockRgb(_modalPath, document.getElementById('rgbSwitch'));  // 内部会重渲染弹窗
  syncRgbFromBlock();
  bindRgbPicker();
}
// 拖拽: SV 面板 / 色相条 / 亮度条 (QML onPressed + onPositionChanged 2809-2826)
function bindRgbPicker() {
  const drag = (el, onMove) => {
    if (!el || el.dataset.bound) return;
    el.dataset.bound = '1';
    el.addEventListener('pointerdown', e => {
      try { el.setPointerCapture(e.pointerId); } catch (err) {}
      onMove(e); e.preventDefault(); e.stopPropagation();
    });
    el.addEventListener('pointermove', e => {
      if (el.hasPointerCapture && el.hasPointerCapture(e.pointerId)) { onMove(e); e.stopPropagation(); }
    });
  };
  const sv = document.getElementById('rgbSv');
  drag(sv, e => {
    const r = sv.getBoundingClientRect();
    _rgbHsv.s = Math.max(0, Math.min(1, (e.clientX - r.left) / r.width));
    _rgbHsv.v = Math.max(0, Math.min(1, 1 - (e.clientY - r.top) / r.height));
    applyRgbColor();
  });
  const hue = document.getElementById('rgbHue');
  drag(hue, e => {
    const r = hue.getBoundingClientRect();
    _rgbHsv.h = Math.max(0, Math.min(1, (e.clientX - r.left) / r.width)) * 360;
    applyRgbColor();
  });
  const brs = document.getElementById('rgbBrs');
  drag(brs, e => {
    const r = brs.getBoundingClientRect();
    const v = Math.max(0, Math.min(100, Math.round((e.clientX - r.left) / r.width * 100)));
    setBlockRgbBrightness(_modalPath, v, null);
    paintRgbUi();
  });
}
function rgbEditorHTML(path) {
  const k = getBlock(path);
  if (!k) return '';
  return `<div class="rgb-section">
    <div class="rgb-head">
      <span class="rgb-title">RGB 底光</span>
      <span class="rgb-chip" id="rgbChip"></span>
      <span class="rgb-flex"></span>
      <button type="button" class="rgb-switch" id="rgbSwitch" onclick="toggleRgbSwitch()"><span>开</span></button>
    </div>
    <div class="rgb-presets">${RGB_PRESETS.map((c, i) =>
      `<span class="rgb-preset" data-i="${i}" style="background:#${c.toString(16).padStart(6, '0')}" onclick="setRgbPreset(${i})"></span>`).join('')}</div>
    <div class="rgb-sv" id="rgbSv"><i class="rgb-sv-h"></i><i class="rgb-sv-v"></i><i class="rgb-sv-dot"></i></div>
    <div class="rgb-hue" id="rgbHue"><i class="rgb-hue-dot"></i></div>
    <div class="rgb-val"><span class="rgb-chip" id="rgbValChip"></span><span class="rgb-hex" id="rgbHex">#000000</span></div>
    <div class="rgb-brs">
      <span class="rgb-brs-label">亮度</span>
      <div class="rgb-track" id="rgbBrs"><i class="rgb-fill"></i><i class="rgb-knob"></i></div>
      <span class="rgb-brs-val" id="rgbBrsVal">100%</span>
    </div>
  </div>`;
}
