/* ===== 摘要卡片: 三列布局定位 / 卡片渲染 / 手势三格 / 复位按钮 / 虚线绘制 =====
   从 assemble_index.py 的内嵌字符串拆出(2026-10-03)。目的: 让这些逻辑成为可独立
   编辑/语法高亮/逐文件 node --check 的 .js，而不是 1200 行 Python 字符串。
   拼接顺序 = 文件名序号，勿调整。 */
// ============================================================
// Studio 同款：设备居中 + Summary 卡片环绕 (数据派生自 config)
// ============================================================
function keySummaryText(k) {
  if (!k || k.action === 'none') return '未设置';
  if (k.action === 'keyboard') {
    const mods = MODIFIERS.filter(m => (k.modifier & m.bit)).map(m => m.label);
    const key = HID_KEYS.find(h => h.value === k.keycode);
    const hasKey = !!(key && key.value);
    // 纯修饰键(keycode=0)只显示修饰键, 不拼主键 (QML formatShortcut 159-160)
    if (mods.length) return hasKey ? mods.join('+') + ' + ' + key.text : mods.join('+');
    return hasKey ? key.text : '无';
  }
  if (k.action === 'mouse') {
    const m = MOUSE_KEYS.find(m => m.value === k.keycode);
    return '鼠标 ' + (m ? m.text : '');
  }
  if (k.action === 'multimedia') {
    const m = MEDIA_KEYS.find(m => m.value === k.keycode);
    return m ? m.text : '多媒体';
  }
  if (k.action === 'airmouse') {
    return '空中鼠标 · ' + (config.air_mouse_mode === 1 ? '按住移动' : '单击切换');
  }
  return '';
}
function hexColor(v) { return '#' + (v >>> 0).toString(16).padStart(6, '0'); }

// 卡片拖动偏移 (path -> {x,y}), 语义对齐 Studio 的 dragOffsetX/Y:
// 配置刷新会重建卡片 DOM, 因此偏移按 path 存在这里, 渲染时再套回 transform。
const CARD_DRAG = {};
// 触屏设备: 卡片不可拖动(见 pointerdown 的 fixed 分支)
const IS_TOUCH = window.matchMedia('(pointer: coarse)').matches || ('ontouchstart' in window);

function sumCardHTML(title, badgeGesture, body, icColor, variant, path) {
  const badge = badgeGesture == null ? '' :
    `<span class="sum-badge${badgeGesture ? ' gesture' : ''}">${badgeGesture ? '手势模式' : '常规模式'}</span>`;
  // 色块 (sum-ic) = RGB 底光指示, 仅 C1/C2/C3 有 RGB 硬件才渲染 (对齐 Studio: 无 RGB 硬件不带色块)
  const ic = icColor ? `<span class="sum-ic" style="background:${icColor}"></span>` : '';
  const dg = (path && CARD_DRAG[path]) || null;
  const mv = !!(dg && (dg.x || dg.y));
  const tf = mv ? ` style="transform: translate(${dg.x}px, ${dg.y}px)"` : '';
  return `<div class="sum-card${mv ? ' moved' : ''}" data-path="${path || ''}"${tf}>
    <div class="sum-head">${ic}
      <span class="sum-title">${title}</span>${badge}
      ${path === 'shake_key' ? '' : '<button class="sum-reset" type="button" title="恢复默认位置" aria-label="恢复默认位置">&#8635;</button>'}</div>
    <div class="sum-body${variant ? ' ' + variant : ''}">${body}</div>
  </div>`;
}

// 手势模式卡片: 三个独立小框, 不是挤成一行 (QML 2083-2120 / 1827-1862)
// 每格: radius4 #f5f5f5, 上=手势名 7pt #999 居中, 下=当前动作 8pt #333 居中省略
function gestureBody(tapP, dblP, lngP) {
  const cell = (name, p) => {
    const b = getBlock(p);
    const t = keySummaryText(b);
    const txt = (!b || b.action === 'none' || !t) ? '未设置' : t;
    return `<div class="sum-ges-cell"><span class="g-name">${name}</span><span class="g-state">${txt}</span></div>`;
  };
  return `<div class="sum-ges">${cell('单击', tapP)}${cell('双击', dblP)}${cell('长按', lngP)}</div>`;
}

function renderSummaries() {
  const left = document.getElementById('sumLeft');
  const right = document.getElementById('sumRight');
  if (!left || !right) return;
  const kOf = p => getBlock(p) || { action: 'none' };
  const modeOf = (arr, i) => (arr && arr[i]) ? arr[i] : 0;

  // 左列: L1 / L2 / L3 + 摇一摇 (无 RGB 硬件, 不带色块)
  let lh = '';
  for (let i = 0; i < 3; i++) {
    const gesture = modeOf(config.l_mode, i) === 1;
    const body = gesture ? gestureBody('l_tap.' + i, 'l_dbl.' + i, 'l_lng.' + i)
                         : keySummaryText(kOf('l_key.' + i));
    lh += sumCardHTML(`KEY L${i + 1}:`, gesture, body, null, gesture ? 'ges' : '', 'l_key.' + i);
  }
  const shakeOn = config.shake_enabled === 1;
  lh += sumCardHTML('摇一摇:', null,
    shakeOn ? ('已开启 · ' + keySummaryText(kOf('shake_key'))) : '<span class="dim">已关闭</span>',
    null, '', 'shake_key');
  left.innerHTML = lh;

  // 右列: KEY C1, EC 编码器, KEY C2, KEY C3 (仅 C1-C3 有 RGB 硬件, 色块显示底光颜色)
  const cCard = (i) => {
    const gesture = modeOf(config.c_mode, i) === 1;
    const body = gesture ? gestureBody('c_tap.' + i, 'c_dbl.' + i, 'c_lng.' + i)
                         : keySummaryText(kOf('keys.' + i));
    const k = kOf('keys.' + i);
    const meta = getMeta('keys.' + i);
    const ic = (meta && meta.rgb) ? (k.rgb_enabled ? hexColor(k.rgb_color) : 'var(--q-knob-off)') : '';
    return sumCardHTML(`KEY C${i + 1}:`, gesture, body, ic, gesture ? 'ges' : '', 'keys.' + i);
  };
  const ecMode = config.ec_press_mode === 1;
  // 10-05: 行首图标取代原「上:/压上:/按:/下:/压下:」文字标签(参考设计图)。
  //   ec-ic-rot=旋转(上/下) · ec-ic-prot=按压旋转(压上/压下) · ec-ic-press=按压。
  //   镜像方向: 原图 = 「下/压下」; 「上/压上」加 flip(scaleX(-1)), 不额外切图。
  //   行不带底色(同「按」手势行): 图标直接落在卡片白底上 —— 灰底只在数值条/三格小卡上。
  const ecRow = (ic, v, flip) =>
    `<div class="ec-row"><i class="ec-ic ${ic}${flip ? ' flip' : ''}"></i>` +
    `<span class="ec-txt">${keySummaryText(kOf(v))}</span></div>`;
  // 「按」行: 常规模式=单行动作; 手势模式=单击/双击/长按 三格(同 QML 2544 手势行)
  const ecGesRow = () =>
    `<div class="ec-row ec-row-ges"><i class="ec-ic ec-ic-press"></i>` +
    gestureBody('ec_press_tap', 'ec_press_dbl', 'ec_press_lng') + `</div>`;
  const ecBody = (() => {
    // 10-05: 手势模式下补"压上/压下"两行（对齐上位机 EC 主卡摘要的
    //   上 / 压上 / 按 / 下 / 压下 顺序）；常规模式下按压滚动不生效，故不显示。
    // ★ 图标方向: **原图(未镜像)代表「下 / 压下」**; 「上 / 压上」用 scaleX(-1) 的镜像版。
    //   (10-05 用户指出最初弄反了 —— 滚轮上应显示「滚轮下」那个方向的图标, 两组一起对调。)
    const rows = [ecRow('ec-ic-rot', 'ec_cw', true)];
    if (ecMode) rows.push(ecRow('ec-ic-prot', 'ec_cw_press', true));
    rows.push(ecMode ? ecGesRow() : ecRow('ec-ic-press', 'ec_press'));
    rows.push(ecRow('ec-ic-rot', 'ec_ccw'));
    if (ecMode) rows.push(ecRow('ec-ic-prot', 'ec_ccw_press'));
    return `<div class="ec-rows">${rows.join('')}</div>`;
  })();
  right.innerHTML = cCard(0) + sumCardHTML('EC 编码器:', ecMode, ecBody, null, 'multi', 'ec_press') + cCard(1) + cCard(2);
}
