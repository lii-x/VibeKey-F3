# -*- coding: utf-8 -*-
import base64, os

WS = r"A:/VibeKey-F3/web/vibekey-F3_web_config"
STUDIO = r"A:/VibeKey-F3/Application/appVibeKeyF3Studio/resources/Sidebar"

def b64(path):
    with open(path, "rb") as f:
        return "data:image/png;base64," + base64.b64encode(f.read()).decode()

WS_PARENT_RES = r"A:/VibeKey-F3/Application/appVibeKeyF3Studio/resources"

# 1) 读取模板并替换图标占位符 (使用 Studio 桌面端真实图标资源)
with open(os.path.join(WS, "template.html"), encoding="utf-8") as f:
    html = f.read()

repl = {
    "__LOGO__": os.path.join(STUDIO, "logo.png"),
    "__KEYSET__": os.path.join(STUDIO, "keyseticon.png"), "__KEYSET_ON__": os.path.join(STUDIO, "keyseticon_current.png"),
    "__STATUS__": os.path.join(STUDIO, "statusicon.png"), "__STATUS_ON__": os.path.join(STUDIO, "statusicon_current.png"),
    "__OTA__": os.path.join(STUDIO, "otaiconn.png"), "__OTA_ON__": os.path.join(STUDIO, "otaicon_current.png"),
    "__MORE__": os.path.join(STUDIO, "moreicon.png"), "__MORE_ON__": os.path.join(STUDIO, "moreicon_current.png"),
    "__DEVICE__": os.path.join(WS_PARENT_RES, "VibeKey-F3.png"),
}
for ph, fn in repl.items():
    html = html.replace(ph, b64(fn))
    assert ph not in html, ph

# 1.1) CSS 直接以 <项目>/css/ 为唯一源 —— index.html 用 <link> 引用它们, 不再拷贝。
#      css/ 与 index.html 同级入库, 好处: 只有一份、能在 GitHub 上直接阅读与修改。
#      曾因一次正则误删整段内联 <style> 导致页面裸奔(2026-10-03), 故这里强校验每个引用都真实存在。
import re as _re
_css_dir = os.path.join(WS, "css")
for _href in _re.findall(r'<link rel="stylesheet" href="css/([^"]+)">', html):
    _p = os.path.join(_css_dir, _href)
    if not os.path.exists(_p) or os.path.getsize(_p) == 0:
        raise SystemExit("FATAL: index.html 引用了不存在或为空的 CSS -> " + _href)
_css_n = len(_re.findall(r'<link rel="stylesheet"', html))
if _css_n == 0:
    raise SystemExit("FATAL: index.html 里没有任何 CSS 引用, 页面会裸奔")

# 2) 读取原始 JS: 唯一源就是 js/00-original.js
#    历史上这里会尝试从 vibekey_web_config.html 的第 679..2574 行重新提取, 该文件已丢失。
#    保留那条路径是个雷: 一旦它重新出现, 提取结果会静默覆盖 00-original.js 里的手工修正
#    (MOUSE_KEYS 键值表、启动文案、resolveTarget 空守卫), 且没有任何提示。
#    现在 00-original.js 已随项目入库, 直接以它为准。
_orig = os.path.join(WS, "js/00-original.js")
if not os.path.exists(_orig):
    raise SystemExit("FATAL: 缺少原始 JS 源 js/00-original.js")
with open(_orig, encoding="utf-8") as f:
    js = f.read()

# 2.1) 安全补丁：原 readConfig 在同步空中鼠标控件时未做空守卫。
#      本版把空中鼠标 / 摇一摇控件移入弹窗，元素仅在弹窗打开时存在，
#      故在此为 airMouseMode / airMouseSpeed 两处补上空守卫，避免读取配置时抛错。
js = js.replace(
    "    document.getElementById('airMouseMode').value = config.air_mouse_mode;\n"
    "    document.getElementById('airMouseSpeed').value = config.air_mouse_speed;\n",
    "    const _amMode = document.getElementById('airMouseMode'); if (_amMode) _amMode.value = config.air_mouse_mode;\n"
    "    const _amSpeed = document.getElementById('airMouseSpeed'); if (_amSpeed) _amSpeed.value = config.air_mouse_speed;\n"
)
assert "document.getElementById('airMouseMode').value" not in js, "guard patch failed (mode)"
assert "document.getElementById('airMouseSpeed').value" not in js, "guard patch failed (speed)"

# 3) 拼接页签切换 / 卡片 / 弹窗 / 捕获 / BLE 等定制逻辑
#    源在 <项目>/js/*.js, 按 _JS_ORDER 拼接。拆出成独立 .js 的原因:
#    这些逻辑原先是 1200 行 Python 字符串, 无法语法高亮/逐文件语法检查, 脚本化改动极易锚点失配。
JS_SRC = os.path.join(WS, "js")
_JS_ORDER = ["09-app-scale.js", "10-tab-switch.js", "11-cards.js", "12-modal-render.js",
             "13-shake-airmouse.js", "14-ble.js", "15-capture-edit.js", "16-ota-net.js",
             "17-vkbd.js", "18-select-popup.js", "19-led-rgb.js"]
_parts = []
for _fn in _JS_ORDER:
    _p = os.path.join(JS_SRC, _fn)
    if not os.path.exists(_p):
        raise SystemExit("FATAL: 缺少 JS 源文件 " + _fn)
    with open(_p, encoding="utf-8") as _f:
        _t = _f.read()
    # 去掉文件头部的说明注释块(拼接时不需要)
    if _t.startswith("/* ====="):
        _e = _t.index("*/") + 2
        _t = _t[_e:].lstrip("\n")
    if not _t.strip():
        raise SystemExit("FATAL: JS 源文件为空 " + _fn)
    _parts.append(_t.rstrip("\n"))
tab_js = "\n" + "\n\n".join(_parts) + "\n"

with open(os.path.join(WS, "index.html"), "w", encoding="utf-8", newline="\n") as f:
    f.write(html)
    f.write(js)
    f.write(tab_js)
    f.write("</script>\n</body>\n</html>\n")

print("OK, size:", os.path.getsize(os.path.join(WS, "index.html")))
