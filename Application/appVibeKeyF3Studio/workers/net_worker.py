"""net_worker.py —— 上位机"网络获取固件"进程(独立一次性)

职责:
  list [bin|setup] [force]
                          拉取 Gitee Releases 列表, 过滤出 *.bin 固件资产,
                          单行打印 RELEASES_JSON:<compact-json> 后打印 RELEASES_DONE
                          10-05: 带本地缓存(TTL 30min, 见下方 CACHE_*); 传 force 绕过缓存
  download <url> <dest>   流式下载到 <dest>, 打印 PROGRESS:n, 成功后 DOWNLOAD_OK:<path>
  usage                   打印用法

输出协议(对齐 OtaManager 行解析):
  PROGRESS:<0-100> / STATUS:<文本> / ERROR:<文本> / DOWNLOAD_OK:<path> / RELEASES_JSON:<json>

设计:
  - 仅 stdlib(urllib/json), 无第三方依赖 —— 打包内置 python 可直接运行;
  - 仓库固定: lii-x/vibe-key-f3(公开), 可选 GITEE_TOKEN 环境变量以支持私有/防限流;
  - Windows 被 QProcess 管道捕获时强制 stdout UTF-8, 与 Qt fromUtf8 对齐。
"""
import json
import os
import re
import sys
import time
import tempfile
import urllib.parse
import urllib.request

try:
    sys.stdout.reconfigure(encoding='utf-8')
    sys.stderr.reconfigure(encoding='utf-8')
except AttributeError:
    pass  # Python < 3.7

REPO = "lii-x/vibe-key-f3"
API_BASE = "https://gitee.com/api/v5"
RELEASES_URL = f"{API_BASE}/repos/{REPO}/releases"
UA = "VibeKeyF3Studio/1.0 (OTA net worker)"

# ======================= 版本列表缓存（10-05 新增）=======================
# 为什么加：Gitee API 对**匿名**请求限流很紧（实测触发 `403 Forbidden
# (Rate Limit Exceeded)`，连 /repos/<owner>/<repo> 基本信息都拒）。而上位机
# `main.cpp` 启动 3s 后就会自动 checkNow()、OtaManager 也会拉 list ——
# **每次重启都打一次 API**。调试期反复重启几十次就把匿名配额耗尽，
# 表现为"OTA 在线获取版本获取不了"（10-05 实测就是这个）。
#
# 策略（三层）：
#   ① TTL 内直接吃缓存，**根本不请求** —— 挡住"密集重启"这个主要来源；
#   ② 请求成功 -> 写缓存（供下次用）；
#   ③ 请求失败 -> 用旧缓存兜底（最多 CACHE_STALE_MAX_SEC），
#      保证界面仍有版本可显示，而不是一片空白 + 无头绪的报错。
CACHE_DIR       = os.path.join(tempfile.gettempdir(), "vibekey_ota")
CACHE_TTL_SEC       = 1800         # 30 分钟内复用缓存, 不发请求
CACHE_STALE_MAX_SEC = 7 * 86400    # 请求失败时, 最多用 7 天内的旧缓存兜底
# TTL 取值理由: 版本列表不需要实时(半小时内不会变), 而 Gitee 匿名配额很小。
# 取 30min 可确保"无论怎么重启, 每小时最多 2 次请求", 再也打不爆配额。
# 需要立即刷新时用 `list <bin|setup> force` 绕过 TTL（手动"检查更新"用）。


def _cache_file(kind):
    return os.path.join(CACHE_DIR, "list_%s.json" % kind)


def _cache_read(kind):
    """返回 (ts, data)；无缓存/损坏 -> (0.0, None)。"""
    try:
        with open(_cache_file(kind), "r", encoding="utf-8") as f:
            obj = json.load(f)
        data = obj.get("data")
        if isinstance(data, list):
            return float(obj.get("ts") or 0), data
    except Exception:
        pass
    return 0.0, None


def _cache_write(kind, data):
    """原子写（先 .tmp 再 replace），失败静默 —— 缓存只是优化, 不该影响主流程。"""
    try:
        os.makedirs(CACHE_DIR, exist_ok=True)
        tmp = _cache_file(kind) + ".tmp"
        with open(tmp, "w", encoding="utf-8") as f:
            json.dump({"ts": time.time(), "data": data}, f, ensure_ascii=False)
        os.replace(tmp, _cache_file(kind))
    except Exception:
        pass


def _emit_list(out):
    """统一的列表输出（协议见文件头）。缓存路径与网络路径共用，保证格式一致。"""
    print("RELEASES_JSON:" + json.dumps(out, ensure_ascii=False), flush=True)
    print("RELEASES_DONE", flush=True)


def _fallback_or_fail(kind, cached, ts, why):
    """网络失败时的收尾：有可用旧缓存就用它，否则报错（403 额外给可操作提示）。"""
    if cached is not None and (time.time() - ts) < CACHE_STALE_MAX_SEC:
        print("CACHE_STALE:获取失败(%s), 改用 %.0f 分钟前的缓存"
              % (why, (time.time() - ts) / 60.0), file=sys.stderr)
        _emit_list(cached)
        return 0
    print(f"ERROR:获取版本列表失败: {why}", flush=True)
    if "403" in str(why):
        print("ERROR:原因: Gitee 匿名 API 配额已耗尽(限流, 通常按小时恢复)。"
              "可设置 GITEE_TOKEN 环境变量以提高配额, 或稍后再试。", flush=True)
    return 1


def _token():
    return os.environ.get("GITEE_TOKEN", "").strip()


def _open_url(url, timeout=30):
    req = urllib.request.Request(url, headers={"User-Agent": UA})
    return urllib.request.urlopen(req, timeout=timeout)


def _head_size(url):
    """Gitee releases API 的 asset 不带 size, HEAD 下载地址拿真实字节数(失败静默返回 0)。"""
    try:
        req = urllib.request.Request(url, headers={"User-Agent": UA}, method="HEAD")
        with urllib.request.urlopen(req, timeout=30) as resp:
            return int(resp.headers.get("Content-Length") or 0)
    except Exception:
        return 0


def _setup_asset(fname):
    """上位机安装包资产: VibeKey-F3_Studio_Setup_v<ver>.exe (大小写不敏感)"""
    low = fname.lower()
    if not (low.startswith("vibekey-f3_studio_setup_") and low.endswith(".exe")):
        return None
    m = re.search(r"_v(\d+(?:\.\d+){1,2})\.exe$", low)
    return m.group(1) if m else "0"


def cmd_list(kind="bin", force=False):
    if kind not in ("bin", "setup"):
        print(f"ERROR:未知资产类型 {kind} (可选 bin|setup)", flush=True)
        return 2

    # ① 缓存新鲜且未强制刷新 -> 直接返回, 不碰网络（防密集重启打爆 Gitee 匿名配额）
    ts, cached = _cache_read(kind)
    if not force and cached is not None and (time.time() - ts) < CACHE_TTL_SEC:
        print("CACHE_HIT:复用 %.0f 秒前的缓存(未请求 Gitee)" % (time.time() - ts),
              file=sys.stderr)
        _emit_list(cached)
        return 0

    params = {"per_page": 20}
    tok = _token()
    if tok:
        params["access_token"] = tok
    sep = "?" if "?" not in RELEASES_URL else "&"
    url = RELEASES_URL + sep + urllib.parse.urlencode(params)

    # ② 请求；任何异常都走 _fallback_or_fail（旧缓存兜底 / 明确报错）
    try:
        with _open_url(url) as resp:
            raw = resp.read().decode("utf-8", errors="replace")
        releases = json.loads(raw)
        if not isinstance(releases, list):
            raise ValueError("版本列表格式异常: " + str(releases)[:200])
    except Exception as e:
        why = "HTTP %s %s" % (e.code, e.reason) if isinstance(e, urllib.error.HTTPError) else str(e)
        return _fallback_or_fail(kind, cached, ts, why)

    out = []
    for rel in releases:
        tag = rel.get("tag_name", "")
        assets = rel.get("assets") or []
        for a in assets:
            fname = a.get("name", "")
            ver = ""
            if kind == "bin":
                if not fname.lower().endswith(".bin"):
                    continue
            elif kind == "setup":
                ver = _setup_asset(fname)
                if ver is None:
                    continue
            else:
                print(f"ERROR:未知资产类型 {kind} (可选 bin|setup)", flush=True)
                return 2
            url = a.get("browser_download_url") or ""
            size = int(a.get("size") or 0)
            if size <= 0 and url:
                size = _head_size(url)
            out.append({
                "tag": tag,
                "name": rel.get("name") or tag,
                "date": rel.get("published_at") or rel.get("created_at") or "",
                "body": rel.get("body") or "",
                "file": fname,
                "url": url,
                "size": size,
                "ver": ver,
            })
    # ② 成功 -> 写缓存, 供 TTL 内复用与失败兜底
    _cache_write(kind, out)
    _emit_list(out)
    if not out:
        hint = "固件 .bin" if kind == "bin" else "上位机安装包"
        print(f"STATUS:仓库暂无可用{hint}发布", flush=True)
    return 0


def cmd_download(url, dest):
    if not url.startswith("http"):
        print("ERROR:无效下载地址", flush=True)
        return 1
    req = urllib.request.Request(url, headers={"User-Agent": UA})
    tok = _token()
    # browser_download_url 通常可直接匿名下载; 附带 token 供私有仓库
    try:
        with urllib.request.urlopen(req, timeout=60) as resp:
            total = int(resp.headers.get("Content-Length") or 0)
            done = 0
            last_pct = -1
            with open(dest, "wb") as f:
                while True:
                    chunk = resp.read(65536)
                    if not chunk:
                        break
                    f.write(chunk)
                    done += len(chunk)
                    if total > 0:
                        pct = int(done * 100 // total)
                        if pct != last_pct:
                            last_pct = pct
                            print(f"PROGRESS:{pct}", flush=True)
    except urllib.error.HTTPError as e:
        print(f"ERROR:下载失败 HTTP {e.code}: {e.reason}", flush=True)
        try:
            os.remove(dest)
        except OSError:
            pass
        return 1
    except Exception as e:
        print(f"ERROR:下载失败: {e}", flush=True)
        try:
            os.remove(dest)
        except OSError:
            pass
        return 1
    print("DOWNLOAD_OK:" + dest, flush=True)
    return 0


def main():
    args = sys.argv[1:]
    if not args:
        print("Usage: net_worker.py list [bin|setup] | download <url> <dest>", flush=True)
        return 2
    cmd = args[0]
    # 10-05: docstring 里写了 usage 但一直没实现 -> 补上, 免得文档与实现不符
    if cmd == "usage":
        print("Usage: net_worker.py list [bin|setup] | download <url> <dest>", flush=True)
        return 0
    if cmd == "list":
        kind = args[1] if len(args) > 1 else "bin"
        # 10-05: 第三个参数 "force" 绕过缓存 TTL（手动"检查更新"用）
        force = len(args) > 2 and args[2] == "force"
        return cmd_list(kind, force)
    if cmd == "download":
        if len(args) < 3:
            print("ERROR:Usage: net_worker.py download <url> <dest>", flush=True)
            return 2
        return cmd_download(args[1], args[2])
    print(f"ERROR:未知命令 {cmd}", flush=True)
    return 2


if __name__ == "__main__":
    sys.exit(main())
