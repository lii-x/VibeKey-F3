/* ===== 按键捕获(含纯修饰键) + 弹窗内块编辑器 DOM 改造 / 各级 paint 函数 / 卡片拖动 =====
   从 assemble_index.py 的内嵌字符串拆出(2026-10-03)。目的: 让这些逻辑成为可独立
   编辑/语法高亮/逐文件 node --check 的 .js，而不是 1200 行 Python 字符串。
   拼接顺序 = 文件名序号，勿调整。 */
// ===== 纯修饰键捕获 (对齐 QML 4185 / 4508) =====
// 现象: 原 keydown 处理器里修饰键只 `captureMods |= bit; return;` 累积后返回,
//       永远不结束捕获; 而 keyup 又把累积的位清掉 => 只能捕获「修饰键+字母」,
//       单独按 Ctrl/Shift/Alt 存不下来。
// QML 明确支持纯修饰键(提示原文「纯修饰键如仅 Ctrl 也可」/「纯修饰键也可，如仅 Ctrl+Alt」),
// formatShortcut(157-164) 对 keycode=0 + modifier 也会正常显示成「Ctrl」。
// 做法: 监听 keyup, 若仍在捕获且松开的键是修饰键, 就用「当前修饰键 ∪ 刚松开的那位」以
//       keycode=0 提交。与原 keyup 谁先执行都不影响(用 | 把刚松开的位加回去)。
const CAP_MOD_BITS = {
  ControlLeft: 0x01, ControlRight: 0x10,
  ShiftLeft: 0x02, ShiftRight: 0x20,
  AltLeft: 0x04, AltRight: 0x40,
  MetaLeft: 0x08, MetaRight: 0x80,
};
function commitModifierOnlyCapture(bit, e) {
  const mods = (captureMods | bit) & 0xff;
  if (!mods) return;                       // 没有修饰键可提交(理论上不会发生)
  const k = getBlock(capturingPath);
  if (!k) { cancelCapture(); return; }
  const label = getMeta(capturingPath).label;
  k.action = 'keyboard';
  k.modifier = mods;
  k.keycode = 0;                          // 0 = 只有修饰键, 无主键 (QML 约定)
  const names = MODIFIERS.filter(m => mods & m.bit).map(m => m.label);
  log('✓ 捕获「' + label + '」: 纯修饰键 mod=0x' + mods.toString(16) + ' (' + names.join('+') + ')', 'success');
  toast('捕获成功: ' + names.join('+'), 'success');
  refreshSummaries();
  paintShortcutBoxes();
  paintGestureCards();
  cancelCapture();
}
document.addEventListener('keyup', (e) => {
  if (!capturingPath) return;
  const bit = CAP_MOD_BITS[e.code];
  if (!bit) return;
  /* 10-05: 已暂存主键时什么都不做 —— 主键会由"松开/宽限期"或"再按修饰键"统一提交;
     若在这里提交"纯修饰键", 空格→Alt 这类"先主键后修饰键"会把主键丢掉。 */
  if (stagedKey && stagedKey.path === capturingPath) return;
  commitModifierOnlyCapture(bit, e);
}, true);

// ===== 弹窗内块编辑器精简为 QML 结构 (Main.qml 2974-3215) =====
// QML 的键盘组合是一整个「快捷键框」: 修饰键由捕获时一并写入 (onCaptured 1633 传入 modifier+keycode),
// 弹窗里**没有**修饰键勾选、也没有原始 HID 键码下拉 —— 网页端把这两样去掉, 换成同款框。
function webFormatShortcut(mod, kc) {                 // 对齐 QML formatShortcut 157-164
  const mods = MODIFIERS.filter(m => (mod & m.bit)).map(m => m.label);
  const hit = HID_KEYS.find(h => h.value === kc);
  const keyStr = (kc > 0 && hit) ? hit.text : '';
  if (mods.length && keyStr) return mods.join('+') + '+' + keyStr;
  if (mods.length) return mods.join('+');
  return keyStr;
}
function paintShortcutBoxes() {                       // 捕获中/捕获完成都要刷新框内文字
  document.querySelectorAll('#editorModalBody .ed-kb').forEach(box => {
    const path = box.dataset.kbPath;
    const k = getBlock(path);
    const cap = (capturingPath === path);
    box.classList.toggle('capturing', cap);
    const t = box.querySelector('.ed-kb-text');
    if (!t) return;
    if (cap) {
      /* 10-05: 窄框里提示语显示不全(被截断), 框内只显示按键名本身。
         两种按键顺序的说明放在 title 悬浮提示与日志里, 不再往框里塞长句。
         键名优先走 HID_KEYS 查表(webFormatShortcut): e.key 对空格是字面 ' ',
         直接显示会看起来是空白, 必须换成可读的「空格」。 */
      if (stagedKey && stagedKey.path === path) {
        const pretty = webFormatShortcut(0, stagedKey.hid);   // 查表失败时返回 ''
        t.textContent = pretty || stagedKey.label || '捕获中…';
        return;
      }
      t.textContent = '捕获中…';
      return;
    }
    const s = (k && k.action === 'keyboard') ? webFormatShortcut(k.modifier, k.keycode) : '';
    t.textContent = s || '未设置';
  });
}
// ===== 动作「大类 + 细分」两级 (对齐 QML 2974-3215) =====
// QML: 大类下拉(键盘组合/鼠标/多媒体/无动作) + 二级控件
//      键盘组合 -> 快捷键框 / 鼠标 -> 鼠标细分下拉 / 多媒体 -> 多媒体细分下拉 / 无动作 -> 无
// 网页端底层是扁平 action('mouse'+keycode / 'airmouse'), 这里在 DOM 层做双向映射, 不改数据模型
const ACT_CATS = [
  { v: 'keyboard', t: '键盘组合' }, { v: 'mouse', t: '鼠标' },
  { v: 'multimedia', t: '多媒体' }, { v: 'none', t: '无动作' },
];
function actCatsOf(allow) {
  const a = allow || [];
  return ACT_CATS.filter(c =>
    c.v === 'keyboard' ? a.indexOf('keyboard') >= 0 :
    c.v === 'mouse' ? (a.indexOf('mouse') >= 0 || a.indexOf('airmouse') >= 0) :
    c.v === 'multimedia' ? a.indexOf('multimedia') >= 0 : a.indexOf('none') >= 0);
}
function actCategoryOf(k) {
  if (!k) return 'none';
  if (k.action === 'keyboard') return 'keyboard';
  if (k.action === 'mouse' || k.action === 'airmouse') return 'mouse';
  if (k.action === 'multimedia') return 'multimedia';
  return 'none';
}
function actCategoryLabel(k) {
  // actionLabel 'airmouse' 叫「空中鼠标开关」, 在"鼠标"大类下作为细分项, 标签用「空中鼠标」
  if (k && k.action === 'airmouse') return '空中鼠标';
  const m = { keyboard: '键盘组合', mouse: '鼠标按键', multimedia: '多媒体键', none: '无 (禁用)' };
  return (k && m[k.action]) || '无 (禁用)';
}
function fillActSub(sel, cat, allow, k) {
  if (!sel) return;
  let html = '';
  if (cat === 'mouse') {
    html = MOUSE_KEYS.map(m => `<option value="${m.value}">${m.text}</option>`).join('');
    if ((allow || []).indexOf('airmouse') >= 0) html += '<option value="airmouse">空中鼠标</option>';
    const kc = (k && k.keycode) || 0;
    const cur = (k && k.action === 'airmouse') ? 'airmouse' : String(kc);
    // 设备里存了列表外的键值(历史版本误写的 8/16 等) -> 显示成禁用项, 便于识别,
    // 绝不静默回落到第一项(那会让用户以为配的是左键)
    const known = MOUSE_KEYS.some(m => String(m.value) === cur) || cur === 'airmouse';
    if (!known) {
      // 0 = 动作是鼠标但没配具体键; 其余是历史版本误写的值(8/16)。
      // 都补一个禁用项说明真实情况, 绝不静默回落到第一项(那会让用户以为配的是左键)
      const label = (cur === '0') ? '未设置' : ('未知键码 ' + cur);
      html = `<option value="${cur}" disabled>${label}</option>` + html;
    }
    sel.innerHTML = html;
    sel.value = cur;   // 未知值会选中那个禁用项(靠 value 精确匹配)
  } else if (cat === 'multimedia') {
    html = MEDIA_KEYS.map(m => `<option value="${m.value}">${m.text}</option>`).join('');
    sel.innerHTML = html;
    const want = String((k && k.keycode) || 0);
    sel.value = MEDIA_KEYS.some(m => String(m.value) === want) ? want : String(MEDIA_KEYS[0].value);
  } else {
    sel.innerHTML = '';
  }
}
function applyActCategory(path, cat) {
  if (!path) return;
  const k = getBlock(path);
  if (!k) return;
  if (cat === 'keyboard') { k.action = 'keyboard'; }
  else if (cat === 'mouse') { k.action = 'mouse'; if (!k.keycode) k.keycode = 1; }
  else if (cat === 'multimedia') { k.action = 'multimedia'; if (!k.keycode) k.keycode = MEDIA_KEYS[0].value; }
  else { k.action = 'none'; }
  refreshSummaries();
  renderModal();
}
function applyActSub(path, val) {
  if (!path) return;
  const k = getBlock(path);
  if (!k) return;
  if (val === 'airmouse') k.action = 'airmouse';
  else { k.keycode = parseInt(val) || 0; k.action = (actCategoryOf(k) === 'multimedia') ? 'multimedia' : 'mouse'; }
  refreshSummaries();
  renderModal();
}

function qmlifyBlockEditors() {
  document.querySelectorAll('#editorModalBody .ed-block').forEach(block => {
    const path = block.dataset.path;
    if (!path) return;
    const groups = [].slice.call(block.querySelectorAll('.form-group'));
    let actGroup = null, actSel = null;
    const subs = [];                       // 鼠标 / 多媒体 的下拉, 按出现顺序
    let kbBox = null;
    groups.forEach(g => {
      const lb = g.querySelector('label');
      const t = lb ? lb.textContent.trim() : '';
      if (t.indexOf('修饰键') === 0) { g.remove(); return; }        // QML 无此控件
      if (t.indexOf('动作类型') === 0) { actGroup = g; actSel = g.querySelector('select'); return; }
      if (t.indexOf('HID 键码') === 0) {                              // -> QML cKbBox 4054-4078
        g.querySelectorAll('.form-row, .capture-btn').forEach(n => n.remove());
        lb.remove();
        kbBox = document.createElement('div');
        kbBox.className = 'ed-kb';
        kbBox.dataset.kbPath = path;
        kbBox.title = '点击后按下想设置的组合键：可「先按修饰键再按主键」，也可「先按主键(如空格)再按修饰键(如 Alt)」组成组合';
        kbBox.innerHTML = '<span class="ed-kb-text"></span>';
        kbBox.addEventListener('click', () => startCapture(path));
        g.appendChild(kbBox);
        return;
      }
      if (t === '鼠标按键' || t === '多媒体功能') {                   // 细分下拉由 actSubSel 统一承担
        g.remove();
      }
    });
    // QML 3865-3874: RowLayout{spacing 6} = 大类下拉(100) + 细分下拉/快捷键框, **无 label**
    if (!actGroup) return;
    const meta = getMeta(path) || {};
    const allow = meta.allow || [];
    // 大类下拉: 选项换成 QML 的四个大类
    actSel.className = 'ed-sel ed-act-sel';
    actSel.innerHTML = actCatsOf(allow).map(c => `<option value="${c.v}">${c.t}</option>`).join('');
    actSel.onchange = () => applyActCategory(path, actSel.value);
    // 二级: 细分下拉(鼠标/多媒体) —— 快捷键框已有, 键盘组合时用框
    const actSubSel = document.createElement('select');
    actSubSel.className = 'ed-sub';
    actSubSel.onchange = () => applyActSub(path, actSubSel.value);
    actGroup.className = 'form-group ed-act-row';
    actGroup.innerHTML = '';
    // 「按下」块的动作行需要行首标签 (QML 4815)
    if (path.indexOf('ec_press') === 0) {
      const gl = document.createElement('span');
      gl.className = 'ec-act-label';
      gl.textContent = (_modalPath && isGesture(resolveTarget(_modalPath)) ? GESTURE_NAMES[_ges] : '动作');
      actGroup.appendChild(gl);
    }
    actGroup.appendChild(actSel);
    if (kbBox) actGroup.appendChild(kbBox);
    actGroup.appendChild(actSubSel);
    block.appendChild(actGroup);
    // 原来的 .key-card 容器内容已被搬空/搬走, 移除空壳 (否则残留一层盒子)
    const shell = block.querySelector('.key-card');
    if (shell) shell.remove();
  });
  paintBlockRows();
  paintShortcutBoxes();
}

// 动作大类决定细分控件的可见性 (QML 用 ComboBox.onCurrentIndexChanged 切 visible)。
// 原实现的 display:none 是渲染时写死的内联样式, 改动作不会重渲染弹窗 -> 这里统一重刷
function paintBlockRows() {
  document.querySelectorAll('#editorModalBody .ed-block').forEach(block => {
    const path = block.dataset.path;
    const k = getBlock(path);
    const row = block.querySelector('.ed-act-row');
    if (!k || !row) return;
    const meta = getMeta(path) || {};
    const allow = meta.allow || [];
    const cat = actCategoryOf(k);
    const actSel = row.querySelector('.ed-act-sel');
    const subSel = row.querySelector('.ed-sub');
    const kb = row.querySelector('.ed-kb');
    if (actSel) {
      const opts = actCatsOf(allow);
      if (!opts.some(c => c.v === cat) && opts.length) {
        actSel.innerHTML = opts.map(c => `<option value="${c.v}">${c.t}</option>`).join('');
      }
      actSel.value = cat;
    }
    if (kb) kb.style.display = (cat === 'keyboard') ? '' : 'none';
    if (subSel) {
      const showSub = (cat === 'mouse' || cat === 'multimedia');
      subSel.style.display = showSub ? '' : 'none';
      if (showSub) fillActSub(subSel, cat, allow, k);
    }
  });
}

function buildModalBody() {
  if (_modalPath == null) return;
  const t = resolveTarget(_modalPath);
  if (!t) return;
  const body = document.getElementById('editorModalBody');
  if (!body) return;

  let h = '';
  if (t.kind === 'SHAKE') {
    h += SHAKE_HTML;   // 红色提示是卡片级元素(.modal-note), 排在按钮行之后, 见 build_index_template
  } else {
    const ges = isGesture(t);
    if (t.kind !== 'EC') h += `<div class="ed-sec">${modeRowHTML(t)}</div>`;   // EC 的模式行在「按下」块内
    if (t.kind === 'EC') {
      // QML 4595-4600 gestureDefs + 4647-4653 分组背景块:
      // 三个手势各占一个带底色的块(radius8 #f4f7f5 边#e3eae5), 标签在块内左侧, 控件在右侧
      const grp = (label, inner) =>
        `<div class="ec-group"><div class="ec-row"><span class="ec-row-label">${label}</span>${inner}</div></div>`;
      // 10-05: 行标题与行数对齐上位机新版弹窗(参考图) —— 手势模式下「旋转上/下」
      //   改称「常规滚动上/下」, 并补「按压滚动上/下」两行; 常规模式下按压滚动不生效,
      //   保持原「旋转上 / 按下 / 旋转下」三行 (同 QML 4684-4706 的显隐规则)。
      h += grp(ges ? '常规滚动上' : '旋转 上', blockEditorHTML('ec_cw'));
      if (ges) h += grp('按压滚动上', blockEditorHTML('ec_cw_press'));
      // 10-05: 「模式」不再挂在这一行 —— 已移到弹窗标题行(见函数尾部 modal-head-mode 那段)。
      h += `<div class="ec-group">
          <div class="ec-row"><span class="ec-row-label">按下</span></div>
          ${ges ? gestureTabsHTML(t) + blockEditorHTML(t.ges[_ges]) : blockEditorHTML(t.reg)}</div>`;
      h += grp(ges ? '常规滚动下' : '旋转 下', blockEditorHTML('ec_ccw'));
      if (ges) h += grp('按压滚动下', blockEditorHTML('ec_ccw_press'));
    } else {
      h += `<div class="ed-sec">` +
           (ges ? gestureTabsHTML(t) + blockEditorHTML(t.ges[_ges]) : blockEditorHTML(t.reg)) + `</div>`;
    }
    // 「RGB 底光」标题由 rgbEditorHTML 自己输出 (QML 4215 就是这个 12px 粗体标题),
    // 这里不再包一层「底光」, 否则弹窗里会出现两个标题
    if (t.kind === 'C') h += `<div class="ed-sec">${rgbEditorHTML(t.rgbPath)}</div>`;
    if (t.kind === 'L' && t.idx === 0 && airmouseOnL1()) h += AIR_MOUSE_HTML;
  }
  body.innerHTML = h;

  // 10-05: EC 的「模式」下拉移到弹窗标题行右侧(用户要求, 与上位机一致)。
  //   它在 .modal-head 里、不在 #editorModalBody 里, 因此不走上面的 body.innerHTML 重建,
  //   需要在这里手动维护(幂等: 已存在就只更新内容, 非 EC 弹窗则移除)。
  //   setModeValue 在 MODAL_REFRESH_FNS 中 ⇒ 改模式会重走 renderModal() → 回到这里重新填充。
  {
    const head = document.querySelector('#editorModal .modal-head');
    if (head) {
      let hm = head.querySelector('.modal-head-mode');
      if (t.kind === 'EC') {
        if (!hm) {
          hm = document.createElement('span');
          hm.className = 'modal-head-mode';
          head.insertBefore(hm, head.querySelector('.modal-close'));
        }
        hm.innerHTML = `<span class="ed-label-inline">模式</span>${modeSelectHTML(t)}`;
      } else if (hm) {
        hm.remove();
      }
    }
  }

  // 弹窗内空中鼠标 / 摇一摇控件从 config 取当前值 (不再依赖全局元素)
  const set = (id, v) => { const el = body.querySelector('#' + id); if (el) el.value = String(v); };
  set('airMouseMode', config.air_mouse_mode);
  set('airMouseSpeed', config.air_mouse_speed);
  set('airMouseDir', config.air_mouse_dir);
  set('shakeEnabled', config.shake_enabled ? 1 : 0);
  set('shakeSens', config.shake_sens);
}
function openEditor(path) {
  const tgt = resolveTarget(path);
  if (!tgt) return;
  _modalPath = path;
  _ges = 0;                                   // 每次打开默认编辑「单击」
  const t = document.getElementById('modalTitle');
  if (t) t.textContent = tgt.title;           // 与 Studio 一致: 标题就是按键名
  // 弹窗宽度随类型切换 (对齐 QML: 按键 320 / 摇一摇 340 / EC 380)
  const mc = document.querySelector('#editorModal .modal-card');
  if (mc) mc.dataset.kind = tgt.kind;
  renderModal();
  const modal = document.getElementById('editorModal');
  if (!modal) return;
  modal.style.display = 'flex';
  requestAnimationFrame(() => modal.classList.add('show'));
  document.addEventListener('keydown', _modalEsc);
  updateConnHot();                           // 编辑中的按键: 虚线保持绿色
}
function _modalEsc(e) { if (e.key === 'Escape') closeEditor(); }
function closeEditor() {
  const modal = document.getElementById('editorModal');
  if (!modal) return;
  modal.classList.remove('show');
  setTimeout(() => { modal.style.display = 'none'; }, 180);
  _modalPath = null;
  document.removeEventListener('keydown', _modalEsc);
  /* 10-05: 关弹窗要一并收起虚拟键盘。入口 .vkbd-fab 靠 `#editorModal.show ~`
     会被 CSS 自动隐藏, 但面板 .vkbd.show 不受那条规则管, 不收的话它会变成
     一个悬在空页面上的孤儿键盘(且 _modalPath 已 null, 目标已失效)。 */
  if (typeof vkbdClose === 'function') vkbdClose();
  if (typeof spClose === 'function') spClose();   // 同理: 自绘下拉弹层也要收
  updateConnHot();
}
function renderModal() {
  buildModalBody();
  // 弹窗重建后重新同步 HSV 并重绑拖拽 (renderModal 会被 toggleBlockRgb 触发)
  syncRgbFromBlock();
  bindRgbPicker();
  qmlifyBlockEditors();
  paintGestureCards();
  paintShakeUi();
  paintAirMouseUi();
  paintEcActLabel();
  /* 10-05: 虚拟键盘的「写入目标」下拉必须跟着弹窗重建同步。
     renderModal 会整体替换 #editorModalBody —— 切手势页(c_tap->c_dbl)或
     切模式(常规 keys.0 -> 手势)时, 可见的快捷键框已经换人了, 下拉若还指着
     旧路径, 屏幕键盘就会写到用户看不见的块上(静默写错)。 */
  if (typeof vkbdSyncTargets === 'function') vkbdSyncTargets();
  /* 同理: 自绘下拉弹层是挂在 body 上的, 弹窗 DOM 一换它就指向了已销毁的 select。
     这里主动收起, 避免留一个点了没反应的浮层。 */
  if (typeof spClose === 'function') spClose();
}

// ===== Studio 同款：卡片可拖动 =====
//   · 按下拖动 -> 移动卡片 (限制在 studio-layout 内, 与 Studio 一致)
//   · 未移动就抬起 -> 视为点击, 打开编辑弹窗 (Studio 的 wasDragged 语义)
//   · 双击卡片 -> 复位到默认位置
let _drag = null;
function dragClamp(card, x, y) {
  const layout = document.getElementById('studioLayout');
  if (!layout) return { x: x, y: y };
  const lr = layout.getBoundingClientRect();
  const r = card.getBoundingClientRect();
  const cur = CARD_DRAG[card.dataset.path] || { x: 0, y: 0 };
  // lr / r 都是视觉像素, cur 与返回值是 CSS 像素(写进 style.transform) -> 全部换算,
  // 否则缩放后拖动距离会随缩放比例被放大/缩小, 表现为"卡片跟不住鼠标、位置会偏离"。
  const sc = appScale();
  const baseL = (r.left - lr.left) / sc - cur.x, baseT = (r.top - lr.top) / sc - cur.y;
  const cw = r.width / sc, ch = r.height / sc;
  const lw = lr.width / sc, lh = lr.height / sc;
  const M = 6;
  const minX = M - baseL, maxX = lw - M - baseL - cw;
  const minY = M - baseT, maxY = lh - M - baseT - ch;
  return {
    x: Math.min(Math.max(x, minX), Math.max(minX, maxX)),
    y: Math.min(Math.max(y, minY), Math.max(minY, maxY)),
  };
}
function resetCardPos(card) {                            // 复位到默认位置
  if (!card || !card.dataset.path) return;
  if (card.dataset.path === 'shake_key') return;        // 固定在左下角, 无位置可复位
  delete CARD_DRAG[card.dataset.path];
  card.style.transform = '';
  card.classList.remove('moved');
  drawConnectors();
}
['sumLeft', 'sumRight'].forEach(id => {
  const el = document.getElementById(id);
  if (!el) return;
  el.addEventListener('pointerdown', e => {
    if (e.target.closest('.sum-reset')) return;          // 复位按钮不触发拖动
    const card = e.target.closest('.sum-card');
    if (!card || !card.dataset.path) return;
    // 摇一摇固定在左下角: 不可拖动 (对齐 QML, shakeInfoCard 没有 dragOffset 属性),
    // 但仍要能点击打开弹窗 —— 所以照常建立 _drag 并打 fixed 标记, 由 pointermove 直接跳过
    // 触屏设备(pointer: coarse)没有 hover, 拖动只会误触卡片 -> 同样按 fixed 处理:
    // pointermove 跳过位移, 但 endDrag 仍会走 openEditor, 所以点按照常打开弹窗
    const fixed = card.dataset.path === 'shake_key' || IS_TOUCH;
    const cur = CARD_DRAG[card.dataset.path] || { x: 0, y: 0 };
    _drag = { card: card, path: card.dataset.path, sx: e.clientX, sy: e.clientY,
              ox: cur.x, oy: cur.y, moved: false, fixed: fixed };
    if (fixed) return;
    try { card.setPointerCapture(e.pointerId); } catch (err) {}
    e.preventDefault();
  });
  el.addEventListener('click', e => {                    // 点复位按钮
    const btn = e.target.closest('.sum-reset');
    if (!btn) return;
    e.stopPropagation();
    resetCardPos(btn.closest('.sum-card'));
  });
  el.addEventListener('mouseover', updateConnHot);       // 悬停 -> 虚线变绿
  el.addEventListener('mouseout', updateConnHot);
  el.addEventListener('dblclick', e => {                 // 双击也可复位
    resetCardPos((e.target.closest('.sum-card')));
  });
});
// 键帽: 点击打开编辑弹窗, 悬停同步虚线颜色 (Studio keyHotspot 行为)
(function () {
  const df = document.querySelector('.device-frame');
  if (!df) return;
  df.addEventListener('click', e => {
    const cap = e.target.closest('.keycap');
    if (cap && cap.dataset.path) openEditor(cap.dataset.path);
  });
  df.addEventListener('mouseover', updateConnHot);
  df.addEventListener('mouseout', updateConnHot);
})();
window.addEventListener('pointermove', e => {
  if (!_drag || _drag.fixed) return;
  // clientX/Y 是**视觉像素**, 而 CARD_DRAG 与 style.transform 用 CSS 像素 -> 需除以 scale,
  // 否则缩放状态下拖动距离被放大 scale 倍, 卡片跟不住鼠标(用户反馈"位置会偏离")。
  const sc = appScale();
  const dx = (e.clientX - _drag.sx) / sc, dy = (e.clientY - _drag.sy) / sc;
  if (!_drag.moved && Math.abs(dx) + Math.abs(dy) > 4) {
    _drag.moved = true;
    _drag.card.classList.add('dragging');
    _drag.card.classList.add('moved');                   // 显示复位按钮
  }
  if (!_drag.moved) return;
  const p = dragClamp(_drag.card, _drag.ox + dx, _drag.oy + dy);
  CARD_DRAG[_drag.path] = p;
  _drag.card.style.transform = `translate(${p.x}px, ${p.y}px)`;
  drawConnectors();                                      // 虚线跟随卡片
});
function endDrag(openIfTap) {
  if (!_drag) return;
  const d = _drag; _drag = null;
  d.card.classList.remove('dragging');
  if (openIfTap && !d.moved) openEditor(d.path);
  drawConnectors();
}
window.addEventListener('pointerup', () => endDrag(true));
window.addEventListener('pointercancel', () => endDrag(false));

// ===== 视觉像素 -> CSS 像素 的换算(重要) =====
// getBoundingClientRect() 返回的是**视觉像素**(已被祖先 transform: scale 缩放),
// 而 style.height/left/top/width/translate 期望的是 **CSS 像素(布局像素)**。
// 窗口变窄触发 --app-scale < 1 后, 若把视觉像素直接写回 style, 相当于缩放两次
// -> 键帽偏移、卡片纵向定位漂移(用户反馈"这块位置有问题, 会偏离")。
// 统一用本函数把视觉像素还原成 CSS 像素再写回。
// 拖动位移也一样: clientX/Y 是视觉像素, 必须除以 scale 才能得到 CSS 像素的位移。
function appScale() {
  var v = parseFloat(getComputedStyle(document.documentElement).getPropertyValue('--app-scale'));
  return (isFinite(v) && v > 0.01) ? v : 1;
}
function toCss(visualPx) { return visualPx / appScale(); }

// 设备与卡片之间的虚线连接线 (运行时按实际布局绘制, 主题自适应)
function drawConnectors() {
  const layout = document.getElementById('studioLayout');
  if (!layout) return;
  const svg = layout.querySelector('svg.connectors');
  const view = document.getElementById('view-keys');
  if (!svg || !view || !view.classList.contains('active')) return;
  const lr = layout.getBoundingClientRect();
  if (lr.width < 10) return;
  const devEl = layout.querySelector('.device-frame');
  if (!devEl) return;
  const dev = devEl.getBoundingClientRect();
  // svg 的 viewBox 单位 == 其 CSS 像素尺寸(布局像素), 而 getBoundingClientRect 给的是
  // 视觉像素。若直接把视觉像素写进 viewBox, 缩放时 path 会被二次拉伸(线不贴卡片)。
  // 故这里统一换算成 CSS 像素, 与 svg 的 CSS 宽高一致。
  const sc = appScale();
  svg.setAttribute('viewBox', `0 0 ${lr.width / sc} ${lr.height / sc}`);
  svg.setAttribute('preserveAspectRatio', 'none');
  const devTop = toCss(dev.top - lr.top), devH = toCss(dev.height);
  // 图像实际矩形 (object-fit:contain): 键帽定位与连线共用, 任何宽度下都要重算
  const img = layout.querySelector('.device-img');
  const ir = (img && img.naturalWidth) ? img.naturalWidth / img.naturalHeight : 319 / 875;
  const imgH = Math.min(devH, toCss(dev.width) / ir);
  const imgW = Math.min(toCss(dev.width), devH * ir);
  const imgTop = devTop + (devH - imgH) / 2;
  const imgLeft = toCss(dev.left - lr.left) + (toCss(dev.width) - imgW) / 2;
  const lCards = [...document.querySelectorAll('#sumLeft .sum-card')].filter(c => c.dataset.path !== 'shake_key');
  const rCards = [...document.querySelectorAll('#sumRight .sum-card')];
  const L_NUB = { 'l_key.0': 0.1383, 'l_key.1': 0.2880, 'l_key.2': 0.4543 };
  const R_BTN = { 'keys.0': 0.1989, 'keys.1': 0.4789, 'keys.2': 0.7577, 'ec_press': 0.2011 };
  const segs = [];                 // 每张卡片一条 path, 带 data-for, 便于选中态高亮
  lCards.forEach((c, i) => {
    const r = c.getBoundingClientRect();
    const g = L_NUB[c.dataset.path];
    const f = (g !== undefined) ? g : (i + 0.5) / lCards.length;
    const y1 = imgTop + imgH * f;
    // 线从卡片侧边**中点**出发(QML 2613-2616 item.height/2), 不用按键 Y 夹取,
    // 否则线会斜着插进卡片、看起来像穿过了文字
    const y2 = toCss(r.top - lr.top) + toCss(r.height) / 2;
    const x1 = imgLeft + 1;
    const x2 = toCss(r.right - lr.left);
    const mx = (x1 + x2) / 2;
    segs.push(`<path data-for="${c.dataset.path}" d="M ${x1} ${y1} C ${mx} ${y1}, ${mx} ${y2}, ${x2} ${y2}"/>`);
  });
  rCards.forEach((c, i) => {
    const r = c.getBoundingClientRect();
    const isEC = c.dataset.path === 'ec_press';
    // C1/C2/C3: 锚点 = 键帽色块右缘的垂直居中 (与 KEYCAP_GEO 自动同步, 键帽微调后虚线跟随)
    // EC: 旋钮在右缘上部, PNG 实测凸起 y 142..211 (中心 0.2023), 突出至 x=318 (0.9969)
    //     (右缘 0.7994 处的红色凸起是另一个部件, 不是 EC —— 补充25 改错了, 此处纠正回来)
    const cap = KEYCAP_GEO[c.dataset.path];
    const g = cap ? (cap.top + KEYCAP_H / 2) : R_BTN[c.dataset.path];
    const f = (g !== undefined) ? g : (i + 0.5) / rCards.length;
    const y1 = imgTop + imgH * (isEC ? 0.2023 : f);
    const y2 = toCss(r.top - lr.top) + toCss(r.height) / 2;   // 卡片左侧中点, 同 QML
    const x1 = isEC ? (imgLeft + imgW * 0.9969) : (imgLeft + imgW * (KEYCAP_LEFT + KEYCAP_W));
    const x2 = toCss(r.left - lr.left);
    const mx = (x1 + x2) / 2;
    segs.push(`<path data-for="${c.dataset.path}" d="M ${x1} ${y1} C ${mx} ${y1}, ${mx} ${y2}, ${x2} ${y2}"/>`);
  });
  svg.innerHTML = segs.join('');
  updateConnHot();
  // 键帽颜色叠加层与虚线共用图像矩形(均为 CSS 像素)。几何见 KEYCAP_GEO / KEYCAP_LEFT 注释。
  updateKeycaps(imgTop - devTop, imgW, imgH);
  alignCardsToDevice(devTop, devH, imgTop - devTop, imgH);
}

// 卡片纵向定位: 对齐 QML distributeCardGaps (Main.qml) —— 首卡中心贴「首个按键」高度,
// 末卡中心贴「末个按键」高度, 中间用 space-between 均分。
// 各按键在设备图像上的高度比例取自 PNG 实测 (与连线锚点同一套数据)。
const CARD_ANCHOR_FRAC = {
  lKey: [0.1383, 0.2880, 0.4543],   // L1 / L2 / L3 侧键凸起中心
  rKey: [0.1989, 0.4789, 0.7577],   // C1 / C2 / C3 键帽中心 (EC 0.2023 参与均分)
};
function alignCardsToDevice(devTop, devH, imgOffY, imgH) {
  const place = (col, firstFrac, lastFrac) => {
    if (!col) return;
    const cards = [...col.children].filter(c => c.classList.contains('sum-card'));
    if (!cards.length) return;
    // getBoundingClientRect 是视觉像素, style.height 是 CSS 像素 -> 必须换算,
    // 否则 --app-scale < 1 时列高被二次缩小, 卡片纵向定位整体上移(用户反馈"位置会偏离")。
    // 用 offsetHeight 更稳: 它不受 transform 影响, 本身就是 CSS 像素。
    const hs = cards.map(c => c.offsetHeight);
    const need = hs.reduce((a, b) => a + b, 0);
    // 列高取「设备高」与「卡片总高」的较大者: QML distributeCardGaps 只保证 minGap 8,
    // 卡片堆不下时允许向下溢出 (4 张右侧卡片在 QML 里同样会超出设备底边)
    const H = Math.max(devH, need);
    let padT = Math.max(0, imgOffY + imgH * firstFrac - hs[0] / 2);
    // lastFrac 为 null 时末卡贴底 (摇一摇在 QML 里是独立的左下角卡片, Main.qml 1910-1915)
    let padB = lastFrac == null ? 0 : Math.max(0, imgOffY + imgH * (1 - lastFrac) - hs[hs.length - 1] / 2);
    const spare = H - need - padT - padB;
    if (spare < 0) { const cut = -spare / 2; padT = Math.max(0, padT - cut); padB = Math.max(0, padB - cut); }
    col.style.height = H + 'px';
    col.style.justifyContent = 'space-between';
    col.style.paddingTop = padT + 'px';
    col.style.paddingBottom = padB + 'px';
  };
  place(document.getElementById('sumLeft'), CARD_ANCHOR_FRAC.lKey[0], CARD_ANCHOR_FRAC.lKey[2]);
  place(document.getElementById('sumRight'), CARD_ANCHOR_FRAC.rKey[0], CARD_ANCHOR_FRAC.rKey[2]);
}
function clearCardAlign() {
  ['sumLeft', 'sumRight'].forEach(id => {
    const col = document.getElementById(id);
    if (!col) return;
    col.style.height = col.style.justifyContent = '';
    col.style.paddingTop = col.style.paddingBottom = '';
  });
}

// 键帽颜色: rgb_enabled 时 rgba(rgb,0.45) multiply 叠加 (立体感透出),
// 关闭时白色 8% 微光 (#15ffffff); 点击打开编辑弹窗, 悬停/选中描绿边
const KEYCAP_GEO = {
  'keys.0': { top: 0.0775 },
  'keys.1': { top: 0.3592 },
  'keys.2': { top: 0.6350 },   // C3 再上移 0.0031 (用户: "还是差一点"; 0.6361 曾被默认接受, 略高一点)
};
const KEYCAP_LEFT = 0.1433, KEYCAP_W = 0.7073, KEYCAP_H = 0.2490;
// fy / imgW / imgH 均由 drawConnectors 传入, 已是 **CSS 像素**(不是视觉像素),
// 否则键帽色块会被 transform:scale 再缩一次, 缩放时与真实键位错位。
function updateKeycaps(fy, imgW, imgH) {
  document.querySelectorAll('.keycap[data-path]').forEach(el => {
    const g = KEYCAP_GEO[el.dataset.path];
    if (!g) return;
    el.style.display = 'block';
    el.style.left = (imgW * KEYCAP_LEFT) + 'px';
    el.style.width = (imgW * KEYCAP_W) + 'px';
    el.style.top = (fy + imgH * g.top) + 'px';
    el.style.height = (imgH * KEYCAP_H) + 'px';
    const k = getBlock(el.dataset.path) || {};
    if (k.rgb_enabled) {
      const c = k.rgb_color >>> 0;
      el.style.background = 'rgba(' + ((c >> 16) & 255) + ',' + ((c >> 8) & 255) + ',' + (c & 255) + ',0.45)';
    } else {
      el.style.background = 'rgba(255,255,255,0.08)';
    }
  });
}

// 虚线选中态: 卡片悬停/拖动中/编辑弹窗打开时, 对应虚线变绿 (与卡片绿色边框同步)
function updateConnHot() {
  const svg = document.querySelector('#studioLayout svg.connectors');
  if (!svg) return;
  svg.querySelectorAll('path[data-for]').forEach(p => {
    const card = document.querySelector('.sum-card[data-path="' + p.dataset.for + '"]');
    const cap = document.querySelector('.keycap[data-path="' + p.dataset.for + '"]');
    const hot = !!((card && (card.classList.contains('dragging') || card.matches(':hover'))) ||
                   (cap && cap.matches(':hover')) ||
                   _modalPath === p.dataset.for);
    p.classList.toggle('hot', hot);
    if (cap) cap.classList.toggle('sel', _modalPath === p.dataset.for);   // Studio: 选中键帽描绿边
  });
}

function refreshSummaries() {
  renderSummaries();
  paintShortcutBoxes();   // 捕获开始/结束/完成都要更新框内文字
  paintBlockRows();      // 动作大类变了, 细分控件可见性要跟着变
  paintGestureCards();  // 手势卡里的动作摘要要跟着捕获/设置更新
  paintShakeUi();     // 摇一摇开关/灵敏度/捕获框状态
  paintAirMouseUi(); // 空中鼠标触发方式/灵敏度选中态
  drawConnectors();                        // 同步刷新: 键帽颜色/虚线立即跟随 (如改 RGB 后)
  requestAnimationFrame(drawConnectors);   // 再等一帧兜底, 防布局未稳
}

// 原有编辑操作后联动刷新 Summary (包装全局函数, 不侵入原逻辑)
// 结构性编辑 (renderKeys / updateAirMouse / updateShake) 同时重渲染打开中的弹窗
const MODAL_REFRESH_FNS = new Set(['renderKeys', 'updateAirMouse', 'updateShake', 'setModeValue']);
['setBlockAction', 'setBlockKeycode', 'toggleBlockModifier', 'setBlockRgbColor',
 'setBlockRgbBrightness', 'toggleBlockRgb', 'setModeValue', 'updateAirMouse',
 'updateShake', 'renderKeys'].forEach(fn => {
  const orig = window[fn];
  if (typeof orig === 'function') {
    window[fn] = function(...a) {
      const r = orig.apply(this, a);
      refreshSummaries();
      if (MODAL_REFRESH_FNS.has(fn) && _modalPath != null) renderModal();
      return r;
    };
  }
});

// 布局变化时重绘连接线
const _studioRO = new ResizeObserver(() => drawConnectors());
const _sl = document.getElementById('studioLayout');
if (_sl) _studioRO.observe(_sl);
window.addEventListener('resize', drawConnectors);
window.addEventListener('load', drawConnectors);   // 图片/字体加载完成后重绘一次, 防止首次定位偏移

// 初始渲染 (默认配置即可显示, 与 Studio 截图一致)
refreshSummaries();
