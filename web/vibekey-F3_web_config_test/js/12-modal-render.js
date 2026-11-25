/* ===== 弹窗骨架: 模式行 / 动作大类两级 / 手势区 / 弹窗开合 =====
   从 assemble_index.py 的内嵌字符串拆出(2026-10-03)。目的: 让这些逻辑成为可独立
   编辑/语法高亮/逐文件 node --check 的 .js，而不是 1200 行 Python 字符串。
   拼接顺序 = 文件名序号，勿调整。 */
// ============================================================
// Studio 同款：点击 Summary 卡片 -> 弹出该分组编辑器 (替代整页冗长编辑器)
//   · 复用原 buildGroup / buildBlockCard 渲染逻辑，保证与固件数据模型一致
//   · 空中鼠标设置并入 L 分组弹窗；摇一摇设置并入 Shake 分组弹窗
// ============================================================
const AIR_MOUSE_HTML = `
  <div class="ed-sec">
    <div class="sh-label sh-label-block">空中鼠标触发方式</div>
    <div class="seg seg-fill">
      <button class="seg-btn" onclick="setAirMouseMode(0)">单击切换</button>
      <button class="seg-btn" onclick="setAirMouseMode(1)">按住移动</button>
    </div>
    <p class="hint sh-hint">单击切换：按一下开始移动，再按一下停止<br>按住移动：按住时移动，松开立即停止</p>
  </div>
  <div class="ed-sec">
    <div class="sh-label sh-label-block">灵敏度</div>
    <div class="seg seg-fill">
      <button class="seg-btn" onclick="setAirMouseSpeed(0)">慢</button>
      <button class="seg-btn" onclick="setAirMouseSpeed(1)">中</button>
      <button class="seg-btn" onclick="setAirMouseSpeed(2)">快</button>
    </div>
  </div>`;

// 空中鼠标设置: 分段按钮 -> 写 config (对齐 QML 4327-4362 的 setAirMouseMode / setAirMouseSpeed)
function setAirMouseMode(v) { config.air_mouse_mode = v; refreshSummaries(); paintAirMouseUi(); }
function setAirMouseSpeed(v) { config.air_mouse_speed = v; refreshSummaries(); paintAirMouseUi(); }
function paintAirMouseUi() {
  const segs = document.querySelectorAll('#editorModalBody .ed-sec .seg.seg-fill .seg-btn');
  // 前两个是触发方式(单击切换/按住移动), 紧接的三个是灵敏度(慢/中/快)
  const groups = [[0, 2, 'air_mouse_mode'], [2, 3, 'air_mouse_speed']];
  groups.forEach(([start, len, key]) => {
    for (let i = 0; i < len; i++) {
      const el = segs[start + i];
      if (el) el.classList.toggle('active', (parseInt(config[key]) || 0) === i);
    }
  });
}

const SHAKE_HTML = `
  <div class="ed-sec">
    <p class="sh-desc">快速晃动设备即可触发一次指定快捷键，例如翻页 / 切换静音 / 呼出工具。</p>
    <div class="ed-row sh-row">
      <span class="sh-label">启用摇一摇</span>
      <span class="ed-flex"></span>
      <button type="button" class="rgb-switch" id="shakeSwitch" onclick="toggleShakeEnabled()"><span>关</span></button>
    </div>
  </div>
  <div class="ed-sec">
    <div class="sh-label sh-label-block">灵敏度</div>
    <div class="seg seg-fill">
      <button class="seg-btn" onclick="setShakeSens(0)">轻</button>
      <button class="seg-btn" onclick="setShakeSens(1)">中</button>
      <button class="seg-btn" onclick="setShakeSens(2)">强</button>
    </div>
    <p class="hint sh-hint">轻：轻微晃动即触发，易误触<br>中：常规晃动触发<br>强：明显甩动才触发，最不易误触</p>
  </div>
  <div class="ed-sec">
    <div class="ed-kb sh-kb" id="shakeKbBox" onclick="toggleShakeCapture()">
      <span class="sh-kb-label">触发快捷键：</span><span class="ed-kb-text" id="shakeKbText">未设置</span>
    </div>
  </div>`;
