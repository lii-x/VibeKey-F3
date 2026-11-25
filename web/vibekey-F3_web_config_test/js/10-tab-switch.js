/* ===== 页签切换: 侧边栏导航 / 视图切换 / 标题联动 (同 Studio) =====
   从 assemble_index.py 的内嵌字符串拆出(2026-10-03)。目的: 让这些逻辑成为可独立
   编辑/语法高亮/逐文件 node --check 的 .js，而不是 1200 行 Python 字符串。
   拼接顺序 = 文件名序号，勿调整。 */
// ============================================================
// Studio 风格侧边栏页签切换
// ============================================================
const TAB_TITLES = { keys: '按键配置', status: '运行状态', ota: '固件升级', more: '更多' };
function switchTab(name) {
  document.querySelectorAll('.nav-btn').forEach(b => b.classList.toggle('active', b.dataset.tab === name));
  document.querySelectorAll('.view').forEach(v => v.classList.toggle('active', v.id === 'view-' + name));
  const t = document.getElementById('tbTitle');
  if (t) t.textContent = TAB_TITLES[name] || name;
  const views = document.querySelector('.views');
  if (views) views.scrollTop = 0;
  if (name === 'keys') requestAnimationFrame(drawConnectors);
  // OTA 页首次进入: 初始化「本地/网络」分段(默认网络获取)并按需拉一次在线版本。
  // 延后一帧, 等 OTA 页的 DOM 可见后再画, 否则 getComputedStyle/offsetWidth 恒为 0。
  if (name === 'ota' && typeof setOtaSrcMode === 'function') {
    requestAnimationFrame(() => { setOtaSrcMode(otaSrcMode); paintOtaNet(); paintOtaDev(); otaPaintNotice(); });
  }
}
