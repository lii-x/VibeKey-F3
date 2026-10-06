/* ===== 窄窗整体缩放(宽窗不动) =====
   需求(用户): "窗口放大缩小, 里面的内容其实都是不变的" + "要像之前一样宽"。
   两条合起来的准确含义, 分两个方向处理:
     1. **窗口变宽** -> 什么都不做。窗体本身是 width:100% + max-width, 会跟着窗口变宽,
        两侧留白变多(这就是"之前"的观感)。内容不参与: 卡片宽度在 CSS 里已锁死
        (左 232 / 右 252), 字号间距全是 px, 三列 auto auto auto + justify-content:center
        居中, 所以内容恒定, 只是居中在更宽的窗体里。
     2. **窗口变窄**(窄于内容最小宽 --design-w) -> 整体等比缩小, 避免挤压重排。
        缩放只靠 transform, 不改任何布局尺寸, 所以内容绝不会变形。
   与上位机关系: Main.qml 里各控件用固定 preferredWidth/Layout, 不随窗口拉伸,
   这里对齐的正是这一点; 窗体外框跟随浏览器窗口, 是网页版应有的行为, 不该被锁死。 */
(function () {
  var CHROME = 48;   // .app 的 height 公式里扣掉的窗口留白
  var PAD = 48;      // body 的 padding: 24px * 2, 缩放基准要扣掉
  function design() {
    var cs = getComputedStyle(document.documentElement);
    // 兜底值必须与 05-responsive.css 的 --design-w 保持一致(752 = 侧栏72 + padding40 + 三列640)。
    // 写成 712 会让窗口在 712..752 区间误判"装得下" -> 不缩放 -> 三列被压 -> 卡片压住设备图。
    var w = parseFloat(cs.getPropertyValue('--design-w')) || 752;
    var h = parseFloat(cs.getPropertyValue('--design-h')) || 560;
    return { w: w, h: h };
  }
  function applyScale() {
    var d = design();
    var availW = window.innerWidth - PAD;
    var availH = window.innerHeight - CHROME;
    // 只在装不下时缩小。availW >= d.w 时 s = 1, 窗体按 100% 正常变宽(不缩不放)。
    // 注意: 必须用 >= 而非 >, 且 d.w 要是真实最小需求, 否则临界点会漏判。
    var s = Math.min(1, availW / d.w, availH / d.h);
    if (!isFinite(s) || s < 0.05) s = 0.05;   // 下限防 0 除
    document.documentElement.style.setProperty('--app-scale', String(s));
    // 缩放后连线坐标要重算(getBoundingClientRect 已含 scale, 故直接重画即可)
    if (typeof drawConnectors === 'function') { try { drawConnectors(); } catch (e) { /* 布局未稳 */ } }
  }
  applyScale();
  window.addEventListener('resize', applyScale);
  window.addEventListener('orientationchange', applyScale);
})();
