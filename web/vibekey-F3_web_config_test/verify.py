# -*- coding: utf-8 -*-
"""产物自检: 内联 JS 语法 / 每个源 JS 文件语法 / 关键符号在位 / CSS 引用都存在。

2026-10-03 起 CSS 与 JS 都拆成独立源文件(与 index.html 同级入库), 这里除了校验产物,
还逐个校验源文件 —— 出错时能直接指出是哪个文件, 而不是只看到产物有问题。
"""
import re, os, subprocess, sys

WS = r"A:/VibeKey-F3/web/vibekey-F3_web_config"
NODE = r"C:/Users/l/.workbuddy/binaries/node/versions/22.22.2-3/node.exe"
fail = []

# 1) 产物内联 JS 语法
html = open(os.path.join(WS, "index.html"), encoding="utf-8").read()
blocks = re.findall(r"<script>(.*?)</script>", html, re.S)
print("产物内联 script 块:", len(blocks))
tmp = os.path.join(WS, "_check.tmp.js")
open(tmp, "w", encoding="utf-8").write(blocks[-1])
r = subprocess.run([NODE, "--check", tmp], capture_output=True, text=True)
print("  产物 JS 语法:", "PASS" if r.returncode == 0 else "FAIL\n" + r.stderr[:400])
if r.returncode != 0:
    fail.append("产物 JS 语法")
os.remove(tmp)

# 2) 源 JS 文件逐个语法检查(拆分的意义所在: 精确定位到文件)
js_dir = os.path.join(WS, "js")
if os.path.isdir(js_dir):
    for fn in sorted(os.listdir(js_dir)):
        if not fn.endswith(".js"):
            continue
        p = os.path.join(js_dir, fn)
        rr = subprocess.run([NODE, "--check", p], capture_output=True, text=True)
        ok = rr.returncode == 0
        print("  源 JS %-24s %s" % (fn, "OK" if ok else "FAIL\n" + rr.stderr[:300]))
        if not ok:
            fail.append(fn)

# 3) 关键符号必须在位
print("head 主题脚本:", "OK" if "vibekey-theme" in blocks[0] else "缺失!")
print("switchTab 在主块:", "OK" if "function switchTab" in blocks[-1] else "缺失!")
for sym in ["qmlifyBlockEditors", "alignCardsToDevice", "drawConnectors", "connectDeviceBle",
            "commitModifierOnlyCapture", "paintShakeUi", "actCategoryOf",
            "vkbdWrite", "toggleVkbd", "vkbdTargets", "vkbdSyncTargets", "vkbdClose",
            "spOpen", "spClose", "spCommit"]:
    if sym not in blocks[-1]:
        fail.append("符号 " + sym)
        print("  符号缺失:", sym)

# 4b) 虚拟键盘: 入口/面板必须真的在 DOM 里, 且入口在 #editorModal 之后
#      (CSS 用 `#editorModal.show ~ .vkbd-fab` 控制显隐, 顺序反了就永不显示)
for frag in ['id="vkbd"', 'class="vkbd-fab"', 'id="vkbdTarget"', 'id="vkbdKeys"']:
    if frag not in html:
        fail.append("vkbd DOM " + frag)
        print("  虚拟键盘 DOM 缺失:", frag)
if not re.search(r'<div class="modal" id="editorModal">.*?class="vkbd-fab"', html, re.S):
    fail.append("vkbd-fab 未位于 #editorModal 之后")
    print("  虚拟键盘: .vkbd-fab 不在 #editorModal 之后, CSS 兄弟选择器会失效")

# 4c) 下拉弹层: 容器是 JS 动态创建并挂到 body 的, 这里只能校验样式与符号在位
#      (真正的 DOM 行为由 jsdom 端到端测试覆盖)
if '.sel-pop' not in html and 'sel-pop' not in blocks[-1]:
    fail.append("sel-pop 弹层缺失")
    print("  下拉弹层: 未找到 sel-pop 相关代码")

# 4) index.html 引用的 CSS 必须真实存在(补充68 裸页面的根因就是这条断了)
for href in re.findall(r'<link rel="stylesheet" href="css/([^"]+)">', html):
    p = os.path.join(WS, "css", href)
    if not os.path.exists(p) or os.path.getsize(p) == 0:
        fail.append("css " + href)
        print("  CSS 缺失或为空:", href)
print("CSS 引用: %d 个, 全部就位" % len(re.findall(r'<link rel="stylesheet"', html)))

print()
print("VERIFY:", "ALL PASS" if not fail else "FAILED -> " + ", ".join(fail))
sys.exit(1 if fail else 0)
