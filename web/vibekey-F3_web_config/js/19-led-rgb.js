/* ============================================================
   19-led-rgb.js —— 灯效测试面板「C1/C2/C3 RGB」测试区 (10-06, 对齐上位机)

   上位机对应实现: Application/appVibeKeyF3Studio/qml/Main.qml 的 ledTestPanel。
   本节只做**临时测试**: 不写 config、不落盘, 刷新即失效。

   ★ 下发复用现成的 CMD_RGB(0x06) —— 与弹窗里改键色时的实时预览(previewRgb)同一通道。
     帧体 6 字节: [keyIndex, enabled, r, g, b, brightness]  (与 previewRgb 完全一致)
   ★ 只走 USB: previewRgb 也是 `if (!port || isBusy) return;` —— 网页端蓝牙没有 RGB 通道,
     这里保持一致, 无 USB 时第一次操作会提示一次(不刷屏)。

   ★ 行为约定(与上位机逐条对齐):
     1. 8 色快选: 点**已选**色块 → 亮/灭切换; 点**其它**色块 → 选色并点亮。
     2. 亮度条量程 = 固件上限 [1, 15] (固件出灯口锁死); 灭态拖亮度**只更新界面、不下发**。
     3. 色块浓淡 = 亮度 / 上限 (相对比例) —— 但只作用于**填充**, 边框不受影响。
     4. 边框 = 亮/灭指示: 选中且亮→深粗框; 选中但灭→无框; 未选中→浅灰细框;
        纯白块(0xFFFFFF)在白色卡片上**永久保留一圈灰框**(没框就整个看不见)。
     5. 「全部熄灭」把三颗一起关掉。
   ============================================================ */

// 固件出灯口锁死的亮度上限 (对齐上位机 KeyConfigManager::kLedBrightnessHwCapPct = 15)
const LED_RGB_MIN = 1;
const LED_RGB_MAX = 15;

// 8 色快选 (与上位机 Main.qml 的 model 数组一致)
const LED_RGB_PALETTE = [0xFF0000, 0x00FF00, 0x0000FF, 0xFFFF00,
                         0xFF00FF, 0x00FFFF, 0xFFFFFF, 0xFF8000];

// 临时测试态: 颜色 0xRRGGBB / 亮度% / 点亮开关 (默认色与上位机一致)
const ledRgbState = {
  color: [0xFF2020, 0x20FF20, 0x2020FF],
  bri:   [LED_RGB_MAX, LED_RGB_MAX, LED_RGB_MAX],
  on:    [true, true, true],
};

function ledRgbHex(v) {
  return '#' + ('000000' + ((v >>> 0) & 0xFFFFFF).toString(16)).slice(-6).toUpperCase();
}
function ledRgbRgba(v, a) {
  return 'rgba(' + ((v >> 16) & 255) + ',' + ((v >> 8) & 255) + ',' + (v & 255) + ',' + a + ')';
}
// 浓淡 = 亮度相对上限的比例 (15/15 = 1 最实, 调低则变淡)
function ledRgbOpacity(i) {
  const b = ledRgbState.bri[i];
  return Math.max(0, Math.min(1, b / LED_RGB_MAX));
}
function ledRgbIsWhite(v) { return ((v >>> 0) & 0xFFFFFF) === 0xFFFFFF; }
// 亮度条已填充比例 (给 CSS 的 --fill 用, 见 09-led-rgb.css .led-rng)
function ledRgbFillPct(b) {
  return Math.round((b - LED_RGB_MIN) / (LED_RGB_MAX - LED_RGB_MIN) * 100);
}

// 重渲染整个 RGB 测试区 (3 行 + 底部说明条)
function renderLedRgb() {
  const box = document.getElementById('ledRgbRows');
  if (!box) return;
  let h = '';
  for (let i = 0; i < 3; i++) {
    const sel = ledRgbState.color[i], on = ledRgbState.on[i], a = ledRgbOpacity(i);
    h += '<div class="led-rgb-row"><span class="led-c">C' + (i + 1) + '</span><div class="led-sws">';
    for (let p = 0; p < LED_RGB_PALETTE.length; p++) {
      const v = LED_RGB_PALETTE[p];
      const isSel = (v === sel);
      // ⚠️ class 顺序无关, 优先级由 09-led-rgb.css 里规则的书写顺序决定
      let cls = 'led-sw';
      if (ledRgbIsWhite(v)) cls += ' w';
      if (isSel && on) cls += ' sel';
      else if (isSel && !on) cls += ' sel-off';
      h += '<button type="button" class="' + cls + '" title="' + ledRgbHex(v) + '"'
        + ' style="background:' + ledRgbRgba(v, a) + '"'
        + ' onclick="ledTestPick(' + i + ',' + v + ')"></button>';
    }
    h += '</div><span class="led-hex">' + ledRgbHex(sel) + '</span>'
      + '<input type="range" class="led-rng" min="' + LED_RGB_MIN + '" max="' + LED_RGB_MAX + '" step="1"'
      + ' value="' + ledRgbState.bri[i] + '" style="--fill:' + ledRgbFillPct(ledRgbState.bri[i]) + '%"'
      + ' oninput="ledTestBri(' + i + ',this.value)">'
      + '<span class="led-pct">' + ledRgbState.bri[i] + '%</span></div>';
  }
  box.innerHTML = h;
  const note = document.getElementById('ledRgbNote');
  if (note) {
    note.textContent = '亮度范围 ' + LED_RGB_MIN + '~' + LED_RGB_MAX
      + '%(考虑功耗情况，目前固件出灯口锁死亮度 ' + LED_RGB_MIN + '~' + LED_RGB_MAX + '%)';
  }
  const allOff = document.getElementById('ledAllOffBtn');
  if (allOff) allOff.textContent = '全部熄灭';
}

// 点色块: 已选 → 亮/灭切换; 其它 → 选色并点亮
function ledTestPick(i, v) {
  if (ledRgbState.color[i] === v) ledRgbState.on[i] = !ledRgbState.on[i];
  else { ledRgbState.color[i] = v; ledRgbState.on[i] = true; }
  renderLedRgb();
  sendLedRgbTest(i);
}

// 拖亮度: 灭态只更新界面, 不下发 (否则每拖一格都白发一条命令)
// ⚠️ 这里**不能**调 renderLedRgb() —— 它会 innerHTML 重建整个区, 正在拖的 range
//    会被换掉, 拖动当场中断。改成只改本行的: 色块填充 alpha / 百分比 / 填充条。
//    ("选中且亮"的深框不受亮度影响, 所以无需动 class。)
function ledTestBri(i, val) {
  let b = parseInt(val, 10);
  if (isNaN(b)) b = LED_RGB_MIN;
  b = Math.max(LED_RGB_MIN, Math.min(LED_RGB_MAX, b));
  ledRgbState.bri[i] = b;
  const row = document.querySelectorAll('#ledRgbRows .led-rgb-row')[i];
  if (row) {
    const a = ledRgbOpacity(i);
    const sws = row.querySelectorAll('.led-sw');
    for (let p = 0; p < sws.length && p < LED_RGB_PALETTE.length; p++) {
      sws[p].style.background = ledRgbRgba(LED_RGB_PALETTE[p], a);
    }
    const pctEl = row.querySelector('.led-pct');
    if (pctEl) pctEl.textContent = b + '%';
    const rng = row.querySelector('.led-rng');
    if (rng) rng.style.setProperty('--fill', ledRgbFillPct(b) + '%');
  }
  if (ledRgbState.on[i]) sendLedRgbTest(i);
}

// 全部熄灭
function ledTestAllOff() {
  ledRgbState.on = [false, false, false];
  renderLedRgb();
  for (let i = 0; i < 3; i++) sendLedRgbTest(i);
}

// ---- 下发 (CMD_RGB 0x06, 仅 USB; 串行化避免并发 getWriter) ----
let _ledRgbChain = Promise.resolve();
let _ledRgbNoUsbWarned = false;

function sendLedRgbTest(i) {
  _ledRgbChain = _ledRgbChain.then(() => _writeLedRgbFrame(i)).catch(() => {});
  return _ledRgbChain;
}

async function _writeLedRgbFrame(i) {
  if (!port || !port.writable) {
    if (!_ledRgbNoUsbWarned) {
      _ledRgbNoUsbWarned = true;
      log('RGB 测试需要 USB 连接（网页版蓝牙通道暂不支持 RGB 下发）', 'warn');
    }
    return;
  }
  if (isBusy) return;   // 读/写进行中, 与 previewRgb 一致直接跳过
  const st = ledRgbState;
  const en  = st.on[i] ? 1 : 0;
  const col = st.on[i] ? st.color[i] : 0;      // 灭 = 发黑
  const bri = st.on[i] ? st.bri[i] : 0;
  const payload = new Uint8Array(6);
  payload[0] = i; payload[1] = en;
  payload[2] = (col >> 16) & 0xFF;
  payload[3] = (col >> 8) & 0xFF;
  payload[4] = col & 0xFF;
  payload[5] = bri;
  const header = buildHeader(CMD_RGB, payload.length, 0);
  const data = new Uint8Array(header.length + payload.length);
  data.set(header, 0);
  data.set(payload, header.length);
  try {
    const w = port.writable.getWriter();
    await w.write(data);
    w.releaseLock();
  } catch (e) { /* 预览式 fire-and-forget, 失败静默 */ }
}

// ---- 初始化: 页面脚本在 body 末尾, 此时 #ledRgbRows 已存在; 兜底再挂一次 DOMContentLoaded ----
(function initLedRgb() {
  if (document.getElementById('ledRgbRows')) renderLedRgb();
  else document.addEventListener('DOMContentLoaded', renderLedRgb);
})();
