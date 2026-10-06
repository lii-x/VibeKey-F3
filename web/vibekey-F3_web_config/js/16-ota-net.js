/* ===== 固件升级: 固件来源(本地文件 / 网络获取) =====
   对齐上位机 Main.qml 929-1076 + OtaManager 413-557 + workers/net_worker.py。
   数据源与上位机**完全一致**: Gitee 公开 API, 仓库 lii-x/vibe-key-f3, 过滤 *.bin 资产。
     - 列表: GET https://gitee.com/api/v5/repos/lii-x/vibe-key-f3/releases?per_page=20
     - 下载: asset.browser_download_url
   上位机走 python 子进程; 网页端直接用 fetch + ReadableStream 边下边报进度。

   ★ 与上位机 net_worker.py 的两处实测差异(Gitee 真实返回, 别照抄上位机假设):
     1. asset 里**没有 size 字段**(上位机用 HEAD 请求补) -> 网页端用
        response.headers.get('Content-Length') 兜底, 拿不到就显示 "-";
     2. release 里**没有 published_at**, 只有 created_at
        (上位机 `rel.get("published_at") or rel.get("created_at")` 已兼容)。
   下载完成后产出与本地选择**同构**的 otaFirmware, 因此预检/传输/校验链路完全复用。 */

var OTA_REPO = 'lii-x/vibe-key-f3';
var OTA_GH_REPO = 'lii-x/VibeKey-F3';
var OTA_UA = 'VibeKeyF3Web/1.0 (OTA)';

// ---------- 在线版本列表: 多源容错 ----------
// 单一数据源不可靠(实测踩过的坑, 别退回单源):
//   1. 用户系统配置了代理(http_proxy/https_proxy 指向本地端口)时, 浏览器走代理链
//      请求 Gitee 可能直接 reject -> "Failed to fetch", 而 curl/headless 走同代理却通
//      —— 说明差异在浏览器的代理/证书处理, 不是服务端全面不可用。
//   2. Gitee 对某些来源(实测 Origin: https://lii-x.github.io)会间歇性返回 **502**,
//      同一 URL 换 Origin/重试又能 200。
//   3. file:// 打开时 Origin 为 null, 行为与 http(s) 又不同。
// 因此: Gitee API 为主源(与上位机一致), GitHub Releases 为备源, 逐个尝试。
var OTA_LIST_SOURCES = [
  {
    name: 'Gitee',
    url: 'https://gitee.com/api/v5/repos/' + OTA_REPO + '/releases?per_page=20',
    parse: function (j) {
      const out = [];
      if (!Array.isArray(j)) return out;
      j.forEach(function (rel) {
        (rel.assets || []).forEach(function (a) {
          const fname = a.name || '';
          if (!/\.bin$/i.test(fname)) return;
          out.push({
            tag: rel.tag_name || '',
            name: rel.name || rel.tag_name || '',
            date: rel.published_at || rel.created_at || '',
            file: fname,
            url: a.browser_download_url || '',
            size: parseInt(a.size || 0, 10) || 0,
            src: 'gitee',
          });
        });
      });
      return out;
    },
  },
  {
    name: 'GitHub',
    url: 'https://api.github.com/repos/' + OTA_GH_REPO + '/releases?per_page=20',
    parse: function (j) {
      const out = [];
      if (!Array.isArray(j)) return out;
      j.forEach(function (rel) {
        (rel.assets || []).forEach(function (a) {
          const fname = a.name || '';
          if (!/\.bin$/i.test(fname)) return;
          out.push({
            tag: rel.tag_name || '',
            name: rel.name || rel.tag_name || '',
            date: rel.published_at || rel.created_at || '',
            file: fname,
            url: a.browser_download_url || '',
            size: parseInt(a.size || 0, 10) || 0,
            src: 'github',
          });
        });
      });
      return out;
    },
  },
];

// 下载通道表(按顺序尝试, 任一成功即可)。理由见 otaDownloadRelease 上方注释。
// ★ jsDelivr 通道的两个要点(踩过坑):
//   1. 仓库名必须是 **GitHub** 仓库(OTA_GH_REPO), 不能用 Gitee 的 OTA_REPO ——
//      jsDelivr 只代理 GitHub/GitLab/npm, 指向 Gitee 会 404。
//   2. 路径是 releases/firmware/<file> —— 固件随仓库入库(由 tools/sync_firmware.py
//      从 Gitee 拉取校验后同步), 不是仓库根目录。目录说明见 releases/firmware/README.md。
var OTA_MIRRORS = [
  { name: 'jsDelivr',
    build: function (r) {
      return 'https://cdn.jsdelivr.net/gh/' + OTA_GH_REPO + '@master/releases/firmware/' + r.file;
    } },
  { name: 'jsDelivr(版本tag)',
    build: function (r) {
      return 'https://cdn.jsdelivr.net/gh/' + OTA_GH_REPO + '@' + r.tag + '/releases/firmware/' + r.file;
    } },
  { name: 'GitHub 直链', build: function (r) { return r.src === 'github' ? r.url : ''; } },
  { name: 'Gitee 直链', build: function (r) { return r.url; } },
];

var otaSrcMode = 1;        // 0=本地文件 1=网络获取 —— **默认网络获取**(需求: 优先网络获取)
var otaReleases = [];       // 在线版本列表 [{tag,name,date,file,url,size}]
var otaNetBusy = false;     // 网络操作进行中(对齐 QML otaManager.netBusy)
var otaNetAbort = null;     // fetch AbortController
var otaFwVersion = '';      // 设备当前固件版本(用于"发现新固件"横幅与版本比对)

function setOtaSrcMode(mode) {
  otaSrcMode = mode;
  const l = document.getElementById('otaSrcLocal'), n = document.getElementById('otaSrcNet');
  if (l) l.classList.toggle('active', mode === 0);
  if (n) n.classList.toggle('active', mode === 1);
  const lp = document.getElementById('otaLocalPane'), np = document.getElementById('otaNetPane');
  if (lp) lp.style.display = mode === 0 ? '' : 'none';
  if (np) np.style.display = mode === 1 ? '' : 'none';
  // 切到网络模式且还没拉过 -> 立刻拉一次(需求: 优先网络获取, 打开即有列表)
  if (mode === 1 && !otaReleases.length && !otaNetBusy) otaRefreshReleases();
  else updateOtaControls();
}

// 带超时的 fetch(Gitee 偶发 502/挂起时不能无限等)
function otaFetchTimeout(url, opts, ms) {
  const ctl = new AbortController();
  const t = setTimeout(() => ctl.abort(), ms || 15000);
  return fetch(url, Object.assign({}, opts || {}, { signal: ctl.signal }))
    .finally(() => clearTimeout(t));
}

// ---------- 在线版本列表 ----------
async function otaRefreshReleases() {
  if (otaNetBusy) { setOtaStatus('网络操作进行中...'); return; }
  otaNetBusy = true;
  paintOtaNet();
  setOtaStatus('正在获取在线版本...');
  var fails = [];
  try {
    var out = null;
    // 逐源尝试: 任一成功拿到非空列表即停
    for (var i = 0; i < OTA_LIST_SOURCES.length; i++) {
      var s = OTA_LIST_SOURCES[i];
      if (i > 0) setOtaStatus('正在尝试备用源 ' + s.name + ' ...');
      try {
        var r = await otaFetchTimeout(s.url, { headers: { 'User-Agent': OTA_UA } }, 15000);
        if (!r.ok) {
          var err = new Error('HTTP ' + r.status);
          err.status = r.status;      // 403=GitHub 速率限制, 供上层给出可读提示
          throw err;
        }
        var raw = await r.json();
        var got = s.parse(raw);
        if (!got.length) throw new Error('该源暂无 .bin 固件');
        out = got;
        break;
      } catch (e) {
        // 403 多为 GitHub 未认证请求的速率限制(60 次/小时/IP), 提示要登录
        var why = (e && e.name === 'AbortError') ? '超时'
                : (e && e.message) ? e.message : e;
        if (e && e.status === 403) why += '（GitHub 速率限制，稍后重试）';
        fails.push(s.name + '(' + why + ')');
      }
    }
    if (!out) throw new Error(fails.join('；') || '所有数据源均不可用');

    otaReleases = out;
    // 按**版本号**排序, 不能用接口返回顺序。
    // 实测: Gitee 按创建时间倒序返回, 首项常是 v1.0.13, 而最新固件其实是 v1.1.2;
    // 若直接取 [0] 当"最新", 会对已升到 v1.0.20 的设备谎报"发现新固件 v1.0.13"。
    otaReleases.sort(otaCmpRelease);
    // 补 size: Gitee asset 不带 size, 且首跳 302 无 CORS -> HEAD 必失败(实测 reject)。
    // 改走可用通道(带 CORS); 拿不到就留空, 不阻塞主流程。
    for (let k = 0; k < otaReleases.length; k++) {
      if (otaReleases[k].size > 0 || !otaReleases[k].url) continue;
      otaReleases[k].size = await otaHeadSize(otaReleases[k]);
    }
    otaPaintNotice();
    setOtaStatus(otaReleases.length
      ? ('获取到 ' + otaReleases.length + ' 个可用版本，请选择后下载')
      : '仓库暂无可用固件发布');
  } catch (e) {
    otaReleases = [];
    var msg = '获取在线版本失败: ' + (e && e.message ? e.message : e)
      + '。请检查网络/代理后重试，或切到「本地文件」手动选择 .bin。';
    setOtaStatus(msg, true);
    // 同时写日志: 状态栏一行放不下全部源/原因, 日志面板可完整查看
    // (log() 内部用 innerHTML, 故必须转义 —— 消息里含 URL/浏览器串)
    if (typeof log === 'function' && typeof escapeHtml === 'function') {
      log('[OTA] ' + escapeHtml(msg), 'error');
      log('[OTA] 页面来源: ' + escapeHtml(location.protocol + '//' + (location.host || '(本地文件)'))
        + ' | 浏览器: ' + escapeHtml(navigator.userAgent.slice(0, 60)), 'info');
    }
  } finally {
    otaNetBusy = false;
    paintOtaNet();
    updateOtaControls();
  }
}

// 解析 tag 里的版本号: v1.2.3 / 1.2.3 / v1.2 -> [1,2,3]
function otaParseVer(tag) {
  const m = String(tag || '').match(/(\d+)\.(\d+)(?:\.(\d+))?/);
  if (!m) return null;
  return [parseInt(m[1], 10), parseInt(m[2], 10), parseInt(m[3] || '0', 10)];
}
// 降序(新 -> 旧)。无法解析版本的排在最后, 同类按 tag 字典序兜底。
function otaCmpRelease(a, b) {
  const va = otaParseVer(a.tag), vb = otaParseVer(b.tag);
  if (va && vb) {
    for (let i = 0; i < 3; i++) { if (va[i] !== vb[i]) return vb[i] - va[i]; }
    return String(a.tag).localeCompare(String(b.tag));
  }
  if (va) return -1;
  if (vb) return 1;
  return String(b.tag).localeCompare(String(a.tag));
}

// 取固件字节数: 逐条通道试 HEAD, 首个成功即用; 全失败返回 0(UI 显示 "-")。
async function otaHeadSize(rel) {
  for (let i = 0; i < OTA_MIRRORS.length; i++) {
    const url = OTA_MIRRORS[i].build(rel);
    if (!url) continue;
    try {
      const r = await fetch(url, { method: 'HEAD', headers: { 'User-Agent': OTA_UA } });
      if (r.ok) {
        const n = parseInt(r.headers.get('Content-Length') || '0', 10) || 0;
        if (n > 0) return n;
      }
    } catch (e) { /* 该通道不可用, 试下一条 */ }
  }
  return 0;
}

// ---------- 固件预检(与本地选择共用, 抽出来供两条来源复用) ----------
function otaApplyFirmware(name, data, srcLabel) {
  otaFirmware = { name: name, data: data, size: data.length };
  const box = document.getElementById('otaFileName');
  if (box) { box.textContent = name + ' (' + data.length + ' 字节)'; box.classList.add('picked'); }
  let meta = '';
  if (data.length + 8 + 4 > OTA_REGION_LIMIT) {
    meta = '<span class="bad">错误：固件过大，超过 OTA 分区上限 ' + OTA_REGION_LIMIT + ' 字节</span>';
  } else if (!isFwValidImage(data)) {
    meta = '<span class="bad">警告：向量表校验失败，可能不是有效的固件镜像</span>';
  } else if (!containsModelTag(data)) {
    meta = '<span class="bad">警告：固件不含 VibeKey-F3 型号标识，可能不是本设备固件</span>';
  } else {
    meta = '<span class="ok">预检通过（向量表 + 型号标识）</span>';
  }
  document.getElementById('otaMeta').innerHTML = meta;
  setOtaStatus('已' + (srcLabel || '选择') + '固件: ' + name);
  updateOtaControls();
}

// ---------- 下载 ----------
// ★ 跨域实测结论(Gitee 真实行为, 2026-10):
//   固件直链 https://gitee.com/<repo>/releases/download/<tag>/<file> 是**两跳 302**:
//       gitee.com/.../download/...            -> 302 (无 CORS 头)
//       -> gitee.com/.../attach_files/...     -> 302 (无 CORS 头)
//       -> foruda.gitee.com/attach_file/...?token=...  (有 CORS: *)
//   浏览器跨域重定向时, 重定向后的请求被判为跨域 -> fetch 直接 reject "Failed to fetch"。
//   穷举验证过的路都不通:
//     - redirect:'manual'  -> 跨域时响应是 opaqueredirect, 读不到 Location, 且实测 reject
//     - mode:'no-cors'     -> 得到 opaque 响应, body 为 null, 读不出字节
//     - gitee /raw/ 接口   -> 同样不通
//   所以**纯浏览器无法直接从 Gitee 下载固件二进制**。这是 Gitee 的 CORS 限制, 非实现问题。
//
//   解决: 多通道依次尝试, 任一成功即可。
//     通道1  jsDelivr 中转(cdn.jsdelivr.net/gh/<repo>@<tag>/<file>):
//            实测 access-control-allow-origin: *, 可跨域流式读取。
//            前提: 固件 .bin 已随 GitHub 仓库 Releases 提供(可用 GitHub Actions 从 Gitee 同步)。
//     通道2  gitee-proxy.jsdelivr 的 Gitee 专用端点(若 jsDelivr 支持 gitee 源)。
//     通道3  原生 Gitee 直链(Gitee 若补上首跳 CORS 头即自动可用)。
//   全部失败时: 如实提示改用「本地文件」, 并给出可复制的直链, 不用假成功糊弄用户。
async function otaDownloadRelease(idx) {
  if (otaNetBusy) { setOtaStatus('网络操作进行中...'); return; }
  const rel = otaReleases[idx];
  if (!rel) { setOtaStatus('请先选择要下载的版本', true); return; }
  otaNetBusy = true;
  otaNetAbort = new AbortController();
  paintOtaNet();
  otaProgress = 0; otaTransferring = false;
  updateOtaUI();

  var tried = [];
  var lastErr = null;
  try {
    for (var i = 0; i < OTA_MIRRORS.length; i++) {
      var url = OTA_MIRRORS[i].build(rel);
      if (!url) continue;
      if (i > 0) setOtaStatus(OTA_MIRRORS[i].name + ' 通道尝试中...');
      try {
        var r = await fetch(url, { headers: { 'User-Agent': OTA_UA }, signal: otaNetAbort.signal });
        if (!r.ok) throw new Error('HTTP ' + r.status);
        if (!r.body || !r.body.getReader) throw new Error('浏览器不支持流式读取');
        var got = await otaReadStream(r, rel.file);
        if (!got) throw new Error('未读取到数据');
        otaProgress = 0; updateOtaUI();
        otaApplyFirmware(rel.file, got, '下载');
        // 记下本次成功的通道, 后续点击同版本直接复用, 避免每次都从头试
        if (OTA_MIRRORS[i] !== rel._okMirror) { rel._okMirror = OTA_MIRRORS[i]; }
        return;
      } catch (e) {
        if (e && e.name === 'AbortError') throw e;
        tried.push(OTA_MIRRORS[i].name + '(' + (e && e.message ? e.message : e) + ')');
        lastErr = e;
      }
    }
    throw lastErr || new Error('所有下载通道均失败');
  } catch (e) {
    if (e && e.name === 'AbortError') setOtaStatus('下载已取消');
    else {
      setOtaStatus('下载失败：' + tried.join('；') + '。Gitee 不允许浏览器跨域下载固件，'
        + '请点「复制直链」用浏览器下载 .bin 后, 切到「本地文件」选择它。', true);
      var box = document.getElementById('otaNetPicked');
      if (box) {
        box.style.display = '';
        box.innerHTML = '直链: <a href="' + escapeHtml(rel.url) + '" target="_blank" rel="noreferrer">'
          + escapeHtml(rel.file) + '</a>（点击在新标签页下载, 再用「本地文件」选择）';
      }
    }
    otaProgress = 0; updateOtaUI();
  } finally {
    otaNetBusy = false; otaNetAbort = null;
    paintOtaNet();
    updateOtaControls();
  }
}

// 流式读取固件并实时推进进度条
async function otaReadStream(r, fname) {
  const total = parseInt(r.headers.get('Content-Length') || '0', 10) || 0;
  const reader = r.body.getReader();
  const chunks = [];
  let done = 0, lastPct = -1;
  for (;;) {
    const res = await reader.read();
    if (res.done) break;
    chunks.push(res.value);
    done += res.value.length;
    if (total > 0) {
      const pct = Math.floor(done * 100 / total);
      if (pct !== lastPct) { lastPct = pct; otaProgress = pct; updateOtaUI(); }
    }
  }
  if (!done) return null;
  const data = new Uint8Array(done);
  let off = 0;
  chunks.forEach(c => { data.set(c, off); off += c.length; });
  return data;
}

// ---------- 渲染 ----------
function fmtSizeBytes(n) {                     // 对齐 QML fmtSizeBytes
  // 拿不到字节数时返回空串(而不是 "-"): Gitee 不允许跨域 HEAD, size 常常取不到,
  // 显示 "-" 会和右侧日期粘成一团("- 2026-09-18"), 干脆不显示更干净。
  if (!n || n <= 0) return '';
  if (n < 1024) return n + ' B';
  if (n < 1024 * 1024) return (n / 1024).toFixed(1) + ' KB';
  return (n / 1024 / 1024).toFixed(2) + ' MB';
}
function fmtOtaDate(s) {                       // 对齐 QML fmtOtaDate: 只取日期部分
  if (!s) return '';
  const m = String(s).match(/(\d{4})-(\d{2})-(\d{2})/);
  return m ? (m[1] + '-' + m[2] + '-' + m[3]) : String(s);
}
function paintOtaNet() {
  const list = document.getElementById('otaNetList');
  const empty = document.getElementById('otaNetEmpty');
  const cur = document.getElementById('otaNetCur');
  const btn = document.getElementById('otaNetRefresh');
  if (cur) cur.textContent = otaFwVersion ? ('当前设备 v' + otaFwVersion) : '';
  if (btn) {
    btn.textContent = otaNetBusy ? '处理中…' : '刷新';
    btn.classList.toggle('off', otaNetBusy);
  }
  if (empty) empty.style.display = (!otaNetBusy && !otaReleases.length) ? '' : 'none';
  if (!list) return;
  list.innerHTML = otaReleases.map((r, i) => {
    const sz = fmtSizeBytes(r.size);
    return '<div class="ota-rel' + (otaNetBusy ? ' busy' : '') + '" data-i="' + i + '" onclick="otaDownloadRelease(' + i + ')">' +
      '<span class="ota-rel-tag">' + escapeHtml(r.tag) + '</span>' +
      '<span class="ota-rel-file">' + escapeHtml(r.file) + '</span>' +
      (sz ? '<span class="ota-rel-size">' + sz + '</span>' : '') +
      '<span class="ota-rel-date">' + fmtOtaDate(r.date) + '</span>' +
      '<span class="ota-rel-act' + (otaNetBusy ? ' off' : '') + '">下载</span>' +
    '</div>';
  }).join('');
}
function escapeHtml(s) {
  return String(s == null ? '' : s).replace(/[&<>"']/g, c =>
    ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' }[c]));
}

// ---------- 新固件横幅 (QML 856-896) ----------
function otaPaintNotice() {
  const box = document.getElementById('otaNotice');
  if (!box) return;
  const latest = otaReleases.length ? otaReleases[0] : null;
  if (!latest || !latest.tag) { box.style.display = 'none'; return; }
  const latestVer = String(latest.tag).replace(/^v/i, '');
  const curVer = String(otaFwVersion || '').replace(/^v/i, '');
  const hasNew = curVer && latestVer && latestVer !== curVer;
  if (!hasNew) { box.style.display = 'none'; return; }
  box.style.display = '';
  document.getElementById('otaNoticeTitle').textContent = '发现新固件 v' + latestVer;
  document.getElementById('otaNoticeSub').textContent = '设备当前 v' + curVer + ' · 建议升级到最新固件';
  const b = document.getElementById('otaNoticeBtn');
  b.textContent = otaNetBusy ? '下载中…' : '立即升级';
  b.disabled = otaNetBusy;
}
function otaUpgradeNow() {          // QML 886-893: 切到网络模式后直接下载最新
  otaSrcMode = 1;
  setOtaSrcMode(1);
  if (otaReleases.length) otaDownloadRelease(0);
  else otaRefreshReleases();
}

// ---------- 设备行 (QML 899-927) ----------
function otaRefreshDevice() { setOtaStatus('正在刷新设备...'); paintOtaDev(); }
function paintOtaDev() {
  const dot = document.getElementById('otaDevDot');
  const txt = document.getElementById('otaDevText');
  if (!dot || !txt) return;
  const on = !!port;
  dot.classList.toggle('on', on);
  txt.classList.toggle('on', on);
  txt.textContent = on
    ? ('已连接' + (otaFwVersion ? ' v' + otaFwVersion : ''))
    : 'USB未连接（固件升级仅支持USB连接）';
  // 固件升级只支持 USB: 走蓝牙时明确提示(固件自身限制, 非网页端)
  if (on && !otaIsUsb()) txt.textContent += '（当前为蓝牙连接，固件升级需切回 USB）';
  const picked = document.getElementById('otaNetPicked');
  if (picked) {
    const has = !!(otaFirmware && !otaFirmware.__local);
    picked.style.display = has ? '' : 'none';
    if (has) picked.textContent = '已选固件: ' + otaFirmware.name;
  }
}
function otaIsUsb() {
  // 蓝牙连接由 14-ble.js 给 port shim 打 __ble 标记(见 makeBlePortShim 调用处)。
  // 固件升级只支持 USB: 走蓝牙时明确提示(固件自身限制, 非网页端)。
  return !(port && port.__ble);
}

// ---------- 供外部调用: 记录设备固件版本 ----------
function otaSetFwVersion(v) { otaFwVersion = v || ''; otaPaintNotice(); paintOtaDev(); }
