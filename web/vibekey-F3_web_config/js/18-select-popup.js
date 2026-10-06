/* ===== 下拉弹层皮肤 (仅网页端) =====
   问题: <select> 的**闭合框**能用 CSS 美化(已有 .ed-sel 那套), 但**展开的选项列表**
   是操作系统/浏览器自绘的原生弹层, CSS 完全够不着 —— 用户看到的就是系统默认的
   灰白列表, 与上位机 QML 的观感对不上(用户反馈「下拉框还是默认的, 不好看」)。

   做法: 保留原生 <select> 作为**唯一数据源**(值、change 事件、disabled、布局全不动),
   只拦截它的 mousedown, 阻止系统弹层, 改由我们自己按 QML 规格画一个列表。
   选完写回 sel.selectedIndex 并派发冒泡的 change —— 页面上所有
   onchange="setModeValue(...)" / onchange="setBlockAction(...)" 一行都不用改。

   为什么不做成"自定义控件替换原生 select":
   弹窗内下拉的宽度/伸缩全靠 CSS 选择器(.ed-sel min-width、.ed-act-sel flex:0 0 100px、
   .form-group > select flex:1 …), 换成 div 就得把每条选择器重写一遍并处理直接子代
   选择器失效, 收益为零、风险很大。保留原生元素则布局原封不动。

   QML 规格 (Main.qml 2986-3045 / 3191-3244):
     弹层 y = 控件高 + 4 / 左右同宽 / padding 3 / radius 8 / 白底 / 边 #e0e0e0
     选项行 高 26(模式行) 或 30 / radius 6 / 文字左内边距 6 / 10pt
       当前项 底 #e6f5ec 字 #07c160   hover 底 #f4f9f6 字 #333   其余 透明底 #333 */
const SP_EDGE = 8;          // 弹层与视口边缘的最小间距
const SP_MAX_H = 264;       // 弹层最大高(超出内部滚动)。QML 不滚是因为桌面弹窗够高,
                            // 网页端动作类别/摇一摇等选项多时必须能滚

let _spSel = null;          // 当前打开弹层的原生 select
let _spHl = -1;             // 键盘高亮的选项下标

function spEsc(s) {
  return String(s).replace(/&/g, '&amp;').replace(/</g, '&lt;')
    .replace(/>/g, '&gt;').replace(/"/g, '&quot;');
}
function spPopupEl() {
  let p = document.getElementById('selPopup');
  if (!p) {
    p = document.createElement('div');
    p.className = 'sel-pop';
    p.id = 'selPopup';
    document.body.appendChild(p);
  }
  return p;
}
function spIsOpen(sel) { return !!_spSel && (!sel || _spSel === sel); }

function spOpen(sel) {
  if (!sel || sel.disabled || sel.options.length === 0) return;
  const pop = spPopupEl();
  _spSel = sel;
  _spHl = sel.selectedIndex >= 0 ? sel.selectedIndex : 0;
  pop.innerHTML = [...sel.options].map((o, i) =>
    `<div class="sel-opt${o.disabled ? ' dis' : ''}" data-i="${i}">${spEsc(o.textContent)}</div>`
  ).join('');
  spPaintHl();
  /* 先 show 再量尺寸: 未显示的元素 offsetWidth 为 0, 没法定宽与判方向。
     同时把 left/top 归零, 避免用上一次的位置量出错误的可用空间。 */
  pop.classList.add('show');
  pop.style.left = '0px';
  pop.style.top = '0px';
  const r = sel.getBoundingClientRect();
  pop.style.minWidth = Math.round(r.width) + 'px';
  /* 弹层行高跟控件走: QML 模式行高 26(2956) / 动作大类行高 30(3074、3178)。
     不按控件高度选行高的话, 高的下拉会弹出矮行, 看着"贴不到一起"。 */
  pop.dataset.tall = r.height >= 30 ? '1' : '0';
  const pw = pop.offsetWidth, ph = pop.offsetHeight;
  let left = r.left;
  if (left + pw > window.innerWidth - SP_EDGE) left = window.innerWidth - SP_EDGE - pw;
  if (left < SP_EDGE) left = SP_EDGE;
  let top = r.bottom + 4;                                   // QML: y = height + 4
  if (top + ph > window.innerHeight - SP_EDGE && r.top - 4 - ph > SP_EDGE) {
    top = r.top - 4 - ph;                                   // 下方放不下就翻到上方
  }
  pop.style.left = Math.round(left) + 'px';
  pop.style.top = Math.round(top) + 'px';
}
function spPaintHl() {
  const pop = document.getElementById('selPopup');
  if (!pop) return;
  [...pop.children].forEach((el, i) => {
    el.classList.toggle('hl', i === _spHl);
  });
  const cur = pop.children[_spHl];
  if (cur && cur.scrollIntoView) cur.scrollIntoView({ block: 'nearest' });
}
function spClose() {
  const pop = document.getElementById('selPopup');
  if (pop) pop.classList.remove('show');
  _spSel = null;
  _spHl = -1;
}
function spCommit(i) {
  const sel = _spSel;
  if (!sel) return;
  const opt = sel.options[i];
  if (!opt || opt.disabled) return;
  /* 先收起再派发 change。
     顺序有讲究: change 会触发 setModeValue -> renderKeys -> renderModal, 把弹窗 DOM
     整个换掉; 若此时弹层还开着, 它就会指向一个已被销毁的 select。先收最干净,
     也保证 handler 万一抛错时弹层不会留在屏幕上。
     sel 已经拿在手里, spClose 把 _spSel 置空不影响下面这一句。 */
  spClose();
  sel.selectedIndex = i;
  sel.dispatchEvent(new Event('change', { bubbles: true }));
}
/* 鼠标: 拦截原生弹层。用 mousedown 而非 click —— Chrome/Edge/Firefox 都是在
   mousedown 阶段展开系统下拉, preventDefault 才能拦住, click 阶段已经晚了。 */
document.addEventListener('mousedown', (e) => {
  const t = e.target;
  if (!t || !t.closest) return;
  const sel = t.closest('select');
  if (sel) {
    if (sel.disabled) return;
    e.preventDefault();                      // 挡住系统弹层(同时会挡掉聚焦, 无妨)
    if (spIsOpen(sel)) spClose(); else spOpen(sel);
    return;
  }
  const opt = t.closest('.sel-opt');
  if (opt) {
    e.preventDefault();
    spCommit(Number(opt.dataset.i));
    return;
  }
  if (_spSel) spClose();                     // 点其它任何地方都收起
}, true);
/* 悬停时同步键盘高亮位: 否则鼠标移开后方向键会从旧位置继续, 看着"跳" */
document.addEventListener('mousemove', (e) => {
  if (!_spSel || !e.target || !e.target.closest) return;
  const opt = e.target.closest('.sel-opt');
  if (!opt || !opt.parentNode || opt.parentNode.id !== 'selPopup') return;
  const i = Number(opt.dataset.i);
  if (i !== _spHl) { _spHl = i; spPaintHl(); }
}, true);

/* 键盘: 原生 select 的键盘行为会直接改值并回车提交, 与我们自绘弹层冲突, 故全部接管。
   捕获阶段 + stopPropagation: Esc 必须只关弹层, 不能顺带把整个编辑弹窗关掉
   (closeEditor 也在 document 上监听 Escape)。 */
document.addEventListener('keydown', (e) => {
  const sel = _spSel;
  const onSelect = e.target && e.target.tagName === 'SELECT';
  if (!sel && !onSelect) return;
  const target = sel || e.target;
  if (onSelect && !sel) {
    if (['ArrowDown', 'ArrowUp', ' ', 'Enter'].includes(e.key)) {
      e.preventDefault(); e.stopPropagation();
      spOpen(target);
      return;
    }
    return;
  }
  switch (e.key) {
    case 'Escape':
      e.preventDefault(); e.stopPropagation(); spClose(); break;
    case 'ArrowDown':
      e.preventDefault(); e.stopPropagation();
      _spHl = Math.min(_spHl + 1, target.options.length - 1); spPaintHl(); break;
    case 'ArrowUp':
      e.preventDefault(); e.stopPropagation();
      _spHl = Math.max(_spHl - 1, 0); spPaintHl(); break;
    case 'Home':
      e.preventDefault(); e.stopPropagation(); _spHl = 0; spPaintHl(); break;
    case 'End':
      e.preventDefault(); e.stopPropagation(); _spHl = target.options.length - 1; spPaintHl(); break;
    case 'Enter':
    case ' ':
      e.preventDefault(); e.stopPropagation(); spCommit(_spHl); break;
    case 'Tab':
      spClose(); break;                      // 不拦截, 让焦点正常走
    default: break;
  }
}, true);

/* 滚动/缩放后弹层会与控件错位(弹层是 fixed 定位, 不随容器滚)。直接收起最稳。
   滚动不冒泡, 但捕获阶段能在 document 上收到。 */
window.addEventListener('scroll', () => { if (_spSel) spClose(); }, true);
window.addEventListener('resize', () => { if (_spSel) spClose(); });
