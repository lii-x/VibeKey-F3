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
// ★ jsDelivr 通道的三个要点(踩过坑):
//   1. 仓库名必须是 **GitHub** 仓库(OTA_GH_REPO), 不能用 Gitee 的 OTA_REPO ——
//      jsDelivr 只代理 GitHub/GitLab/npm, 指向 Gitee 会 404。
//   2. 路径是 releases/firmware/<file> —— 固件随仓库入库(由 tools/sync_firmware.py
//      从 Gitee 拉取校验后同步), 不是仓库根目录。目录说明见 releases/firmware/README.md。
//   3. ★ 分支名必须写 @main —— 2026-10-07 仓库重建后只剩 main 一个分支。
//      写 @master 时 jsDelivr 仍返回 200(它缓存了旧分支的假象), 缓存一过期固件
//      下载就静默失效; 下面的"版本tag"通道是真正的兜底。
var OTA_MIRRORS = [
  { name: 'jsDelivr',
    build: function (r) {
      // ★ 必须用 @main: 2026-10-07 仓库重建后只剩main 一个分支(旧的 master 已删)。
      //   写 @master 时 jsDelivr 会一直返回 200 —— 那是它缓存了旧分支内容的"假象",
      //   缓存过期后固件下载会静默失效。tag 通道(下一条)是真正的兜底。
      return 'https://cdn.jsdelivr.net/gh/' + OTA_GH_REPO + '@main/releases/firmware/' + r.file;
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
var otaFwAutoTried = false; // 设备上线时"列表为空就补拉一次"只做一次(对齐 OtaManager::m_fwAutoTried)

/* ---------- 列表缓存(对齐上位机 net_worker.py 的三层策略) ----------
   为什么必须有: Gitee 对匿名请求限流很紧(403 Rate Limit Exceeded 是常态),
   页面每次打开都裸打一次 API 很容易把配额打光。策略与客户端一致:
     ① TTL 内直接吃缓存, 根本不发请求;
     ② 请求成功 -> 写缓存;
     ③ 请求失败 -> 用旧缓存兜底(最多 7 天), 至少让界面/红点还有判断依据。
   ⚠️ localStorage 在无痕模式/某些 file:// 场景会抛异常, 一律 try/catch 静默降级。 */
var OTA_CACHE_KEY = 'vkf3_ota_list_v1';
var OTA_CACHE_TTL_SEC = 1800;             // 30 分钟
var OTA_CACHE_STALE_MAX_SEC = 7 * 86400;  // 失败兜底最多用 7 天前的缓存
var OTA_DISMISS_KEY = 'vkf3_ota_notice_dismissed_v1';  // "稍后"忽略的版本号(对齐 fwNoticeDismissed)

function otaCacheRead(maxAgeSec) {
  try {
    const raw = localStorage.getItem(OTA_CACHE_KEY);
    if (!raw) return null;
    const o = JSON.parse(raw);
    if (!o || !Array.isArray(o.list) || !o.list.length || !o.ts) return null;
    const ageSec = (Date.now() - o.ts) / 1000;
    if (!(ageSec <= maxAgeSec)) return null;
    o.ageSec = ageSec;
    return o;                     // {ts, ageSec, list}
  } catch (e) { return null; }
}
function otaCacheWrite(list) {
  try {
    // 只存可序列化字段: _okMirror 里带函数, 存进去会变成 {name} 污染下次使用
    const clean = list.map(function (r) {
      return { tag: r.tag, name: r.name, date: r.date, file: r.file, url: r.url, size: r.size, src: r.src };
    });
    localStorage.setItem(OTA_CACHE_KEY, JSON.stringify({ ts: Date.now(), list: clean }));
  } catch (e) { /* 配额满/无痕模式: 不缓存也能用, 只是下次要重新拉 */ }
}
function otaDismissedVer() {
  try { return localStorage.getItem(OTA_DISMISS_KEY) || ''; } catch (e) { return ''; }
}
// 「稍后」: 记住这个版本不再提示横幅(红点仍保留, 与客户端一致 ——
//   客户端"稍后"只关弹窗, 侧栏红点照样常亮)。
function otaDismissNotice() {
  const latest = otaLatestRelease();
  if (!latest || !latest.tag) return;
  const ver = String(latest.tag).replace(/^v/i, '');
  try { localStorage.setItem(OTA_DISMISS_KEY, ver); } catch (e) { /* 忽略 */ }
  otaPaintNotice();
}
// 取在线最新版本(列表已按版本降序排好)
function otaLatestRelease() {
  return (otaReleases && otaReleases.length) ? otaReleases[0] : null;
}
// 是否有新版: 只有【线上版本 > 设备版本】才算(数字逐段比较, 对齐 OtaManager::cmpVersion)。
// 设备版本比线上还新时(本地 1.2.8 vs 线上 1.1.x)绝不能说"发现新固件"。
function otaHasNewFw() {
  const latest = otaLatestRelease();
  const cur = String(otaFwVersion || '').replace(/^v/i, '');
  if (!latest || !latest.tag || !cur) return false;
  return otaCmpVer(String(latest.tag).replace(/^v/i, ''), cur) > 0;
}
// 侧栏 OTA 图标右上角红点: 任意页可见(对齐 QML 的 10px 脉冲圆点)
function otaPaintBadge() {
  const b = document.getElementById('otaNavBadge');
  if (!b) return;
  if (otaHasNewFw()) b.removeAttribute('hidden');
  else b.setAttribute('hidden', '');
}

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
// mode: 'silent' = 启动 / 设备上线时的静默检查(失败不弹错, 优先吃缓存)
//       'force'  = 用户点「刷新」, 绕过 TTL 强制拉最新(对齐上位机手动"检查更新")
//       省略     = 首次进入 OTA 页, 允许吃 30 分钟内的缓存
async function otaRefreshReleases(mode) {
  const silent = (mode === 'silent');
  const force = (mode === 'force');

  if (otaNetBusy) { if (!silent) setOtaStatus('网络操作进行中...'); return; }

  // ① TTL 内(30 分钟)的缓存直接吃, 根本不发请求 —— 保护 Gitee 匿名配额, 与客户端同策略。
  //    手动「刷新」传 force 跳过这一步(用户意图就是要最新)。
  if (!force) {
    const fresh = otaCacheRead(OTA_CACHE_TTL_SEC);
    if (fresh) {
      otaReleases = fresh.list;
      otaReleases.sort(otaCmpRelease);
      otaPaintNotice(); paintOtaNet(); updateOtaControls();
      if (!silent) {
        setOtaStatus('已使用 ' + Math.max(1, Math.round(fresh.ageSec / 60)) + ' 分钟前的缓存列表'
          + '（点「刷新」可强制获取最新）');
      }
      return;
    }
  }

  otaNetBusy = true;
  paintOtaNet();
  if (!silent) setOtaStatus('正在获取在线版本...');
  var fails = [];
  try {
    var out = null;
    // 逐源尝试: 任一成功拿到非空列表即停
    for (var i = 0; i < OTA_LIST_SOURCES.length; i++) {
      var s = OTA_LIST_SOURCES[i];
      if (i > 0 && !silent) setOtaStatus('正在尝试备用源 ' + s.name + ' ...');
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
    otaCacheWrite(otaReleases);      // ② 成功 -> 写缓存, 供下次 TTL 内直取
    otaPaintNotice();
    setOtaStatus(otaReleases.length
      ? ('获取到 ' + otaReleases.length + ' 个可用版本，请选择后下载')
      : '仓库暂无可用固件发布');
  } catch (e) {
    // ③ 网络失败 -> 旧缓存兜底(最多 7 天): 至少让红点/横幅还能判断有没有新版,
    //    而不是一片空白 + 报错(客户端 net_worker.py 也是这么兜的)。
    const stale = otaCacheRead(OTA_CACHE_STALE_MAX_SEC);
    if (stale) {
      otaReleases = stale.list;
      otaReleases.sort(otaCmpRelease);
      otaPaintNotice();
      const mins = Math.max(1, Math.round(stale.ageSec / 60));
      const ago = mins >= 120 ? Math.round(mins / 60) + ' 小时' : mins + ' 分钟';
      setOtaStatus('网络获取失败，已使用 ' + ago + '前的缓存列表'
        + (e && e.message ? '（' + e.message + '）' : ''), true);
      return;
    }
    otaReleases = [];
    otaPaintNotice();
    var msg = '获取在线版本失败: ' + (e && e.message ? e.message : e)
      + '。请检查网络/代理后重试，或切到「本地文件」手动选择 .bin。';
    if (silent) {
      // 静默检查失败不打扰用户(页面没开 OTA 页时尤其不能弹), 只留一条日志
      if (typeof log === 'function' && typeof escapeHtml === 'function') {
        log('[OTA] 后台检查失败（不影响其他功能）: ' + escapeHtml(msg), 'info');
      }
    } else {
      setOtaStatus(msg, true);
      // 同时写日志: 状态栏一行放不下全部源/原因, 日志面板可完整查看
      // (log() 内部用 innerHTML, 故必须转义 —— 消息里含 URL/浏览器串)
      if (typeof log === 'function' && typeof escapeHtml === 'function') {
        log('[OTA] ' + escapeHtml(msg), 'error');
        log('[OTA] 页面来源: ' + escapeHtml(location.protocol + '//' + (location.host || '(本地文件)'))
          + ' | 浏览器: ' + escapeHtml(navigator.userAgent.slice(0, 60)), 'info');
      }
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

// 版本号逐段数字比较: a>b 返回正数, 相等 0, a<b 负数(对齐上位机 OtaManager::cmpVersion)。
// ⚠️ 不能用字符串比较: "1.1.10" < "1.1.9" 会判错。
function otaCmpVer(a, b) {
  const va = otaParseVer(a), vb = otaParseVer(b);
  if (!va || !vb) return String(a || '').localeCompare(String(b || ''));
  for (let i = 0; i < 3; i++) { if (va[i] !== vb[i]) return va[i] - vb[i]; }
  return 0;
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
  /* 列表可能还没加载过(OtaSetFwVersion 会在连上设备的瞬间就被 applyDeviceInfo 调用,
     早于首次 otaRefreshReleases) ⇒ 这里对 otaReleases 兜底, 不能直接 .length/.map。 */
  const rels = otaReleases || [];
  if (cur) cur.textContent = otaFwVersion ? ('当前设备 v' + otaFwVersion) : '';
  if (btn) {
    btn.textContent = otaNetBusy ? '处理中…' : '刷新';
    btn.classList.toggle('off', otaNetBusy);
  }
  if (empty) empty.style.display = (!otaNetBusy && !rels.length) ? '' : 'none';
  if (!list) return;
  list.innerHTML = rels.map((r, i) => {
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
  otaPaintBadge();     // 红点先更新: 它在侧栏, 任何页都可见, 不依赖横幅 DOM
  const box = document.getElementById('otaNotice');
  if (!box) return;
  const latest = otaLatestRelease();
  const latestVer = (latest && latest.tag) ? String(latest.tag).replace(/^v/i, '') : '';
  const curVer = String(otaFwVersion || '').replace(/^v/i, '');
  /* 判定标准与上位机一致: 只有【线上版本 > 设备版本】才提示(数字逐段比较)。
     原来用 != 比较, 设备版本比线上还新时(本地 1.2.8 vs 线上 1.1.x)会提示
     "发现新固件 v1.1.x", 等于劝用户降级。
     另: 用户点过「稍后」且线上版本没变 -> 横幅不再出现(红点保留, 同客户端)。 */
  if (!otaHasNewFw() || otaDismissedVer() === latestVer) { box.style.display = 'none'; return; }
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
// 调用点在 applyDeviceInfo(00-original.js) —— 每次刷新设备信息都会同步。
// 三个渲染函数都要重画: 横幅+红点(有无新版) / 列表头(当前设备 vX) / 设备行(已连接 vX)。
function otaSetFwVersion(v) {
  otaFwVersion = v || '';
  otaPaintNotice();
  paintOtaNet();
  paintOtaDev();
  /* 设备上线但还没有在线列表 -> 静默补拉一次(对齐 OtaManager.cpp:128-135
     "连上设备(USB 或蓝牙)即能对比出固件有没有新版")。
     只做一次: otaFwAutoTried, 防反复联网把 Gitee 匿名配额打光。 */
  if (otaFwVersion && !(otaReleases && otaReleases.length) && !otaNetBusy && !otaFwAutoTried) {
    otaFwAutoTried = true;
    setTimeout(function () { otaRefreshReleases('silent'); }, 0);
  }
}

/* ---------- 启动静默检查 (对齐客户端 main.cpp:99) ----------
   客户端是启动 5 秒后静默拉一次列表(吃缓存); 网页端同理 —— 这样用户不必先点进
   「固件升级」页, 只要设备连着, 侧栏 OTA 图标就能亮红点。
   ⚠️ 延迟 1.2s: 让设备自动连接/首页渲染先完成, 别和 Web Serial 授权抢时序。
   ⚠️ 交给 otaRefreshReleases('silent'): 命中 30 分钟缓存时零请求, 失败也不弹错。 */
(function otaStartupCheck() {
  const run = function () {
    setTimeout(function () { otaRefreshReleases('silent'); }, 1200);
  };
  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', run);
  else run();
})();
