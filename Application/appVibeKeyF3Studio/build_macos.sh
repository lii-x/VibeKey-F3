#!/bin/bash
# ============================================================================
#  build_macos.sh - VibeKey-F3 Studio macOS 构建脚本
#
#    ./build_macos.sh              -> universal2 Release 构建 + 内置 Python + 签名
#    ./build_macos.sh clean        -> 删除 build/macos 后完整重建
#    ./build_macos.sh build        -> 只编译（不做 macdeployqt / Python / 签名）
#
#  默认产物:
#    build/macos/appVibeKeyF3Studio.app
#    build/macos/VibeKey-F3_Studio_v<版本>_universal.dmg（可拖拽安装）
#    - 官方 Qt 6.8.x（universal2: arm64 + x86_64）
#    - 最低 macOS 14.0
#    - 内置两套独立 Python 运行时（arm64 / x86_64），用户无需自己装 Python
#    - 构建完成后自动 codesign（默认 ad-hoc）+ 制作 DMG
#
#  Qt 依赖（官方 Qt，不要用 Homebrew Qt）:
#    python3 -m pip install aqtinstall
#    aqt install-qt mac desktop 6.8.3 clang_64 --modules qtserialport --outputdir "$HOME/Qt"
#    export QT_PREFIX="$HOME/Qt/6.8.3/macos"
#
#  可选环境变量:
#    QT_PREFIX            官方 Qt 根目录，默认 $HOME/Qt/6.8.3/macos
#    MIN_MACOS            最低系统版本，默认 14.0
#    ARCHS                架构，默认 "arm64;x86_64"；只开发可设 arm64
#    CODESIGN_IDENTITY    签名身份，默认 "-"（ad-hoc）；发布填 Developer ID
#    GITHUB_PROXY         访问 GitHub Release 的镜像前缀（默认 ghproxy.net；
#                         传空字符串可强制直连 GitHub；下载内容用固定 SHA256 校验）
#    PY_RUNTIME_DIR       已准备好的 Python 运行时目录（默认 build/macos/python-runtime）
#    MAKE_DMG             默认 1，构建完自动打 DMG；设 0 可跳过
#
#  说明：universal2 构建需要为两个架构各准备一套 Python；脚本在 Apple Silicon
#  上会用 Rosetta 2 执行 x86_64 Python。Intel Mac 上请用 ARCHS=x86_64（只出
#  Intel 包），或在一台 Apple Silicon 机器/CI 上构建 universal2。
# ============================================================================
set -euo pipefail

cd "$(dirname "$0")"

BUILD_DIR="build/macos"
QT_PREFIX="${QT_PREFIX:-$HOME/Qt/6.8.3/macos}"
MIN_MACOS="${MIN_MACOS:-14.0}"
ARCHS="${ARCHS:-arm64;x86_64}"
CODESIGN_IDENTITY="${CODESIGN_IDENTITY:--}"
GITHUB_PROXY="${GITHUB_PROXY-https://ghproxy.net}"
PY_RUNTIME_DIR="${PY_RUNTIME_DIR:-$BUILD_DIR/python-runtime}"
MODE="${1:-all}"

# python-build-standalone 固定版本：自包含、可重定位、带 pip；
# 两个架构各一套，运行时由 PythonLocator 按当前进程架构选择。
PY_VERSION="3.12.16"
PY_RELEASE="20261123"
PY_SHA_ARM64="0de87cfc2649849da10a54084f87fd01fa1e653478e32eee92198c213550e15c"
PY_SHA_X86_64="da628e539cebac13f43a3dc42dd99826aeab254387d3bc43478a73267a362713"

if [[ "$MODE" == "clean" ]]; then
    echo "==> 清理 $BUILD_DIR"
    rm -rf "$BUILD_DIR"
    MODE=all
fi

if [[ ! -d "$QT_PREFIX/lib/cmake/Qt6Quick" ]]; then
    cat >&2 <<EOF
[ERROR] 未找到官方 Qt6: $QT_PREFIX
请安装官方 Qt 6.8.x（Homebrew 的 Qt 是单架构，不能做 universal2）:
  python3 -m pip install aqtinstall
  aqt install-qt mac desktop 6.8.3 clang_64 --modules qtserialport --outputdir "\$HOME/Qt"
或显式指定: QT_PREFIX=/path/to/Qt/6.8.3/macos ./build_macos.sh
EOF
    exit 1
fi
if [[ ! -x "$QT_PREFIX/bin/macdeployqt" ]]; then
    echo "[ERROR] 找不到 $QT_PREFIX/bin/macdeployqt" >&2
    exit 1
fi

echo "==> 构建配置"
QT_MINOS="$(vtool -show-build "$QT_PREFIX/lib/QtCore.framework/Versions/A/QtCore" 2>/dev/null | awk '/minos/{print $2; exit}')"
echo "    Qt       : $QT_PREFIX"
echo "    Qt 最低系统: ${QT_MINOS:-未知}"
echo "    目标系统  : macOS $MIN_MACOS"
echo "    架构      : $ARCHS"
echo "    签名      : $CODESIGN_IDENTITY"

# 防止误用 Homebrew Qt（单架构 / 可能按当前 macOS 26 构建）：
# 若 Qt 自身要求的最低系统高于目标，产出的 .app 在 14/15 上仍然起不来。
if [[ -n "$QT_MINOS" ]] && \
   [[ "$(printf '%s\n%s\n' "$MIN_MACOS" "$QT_MINOS" | sort -V | tail -1)" == "$QT_MINOS" ]] && \
   [[ "$QT_MINOS" != "$MIN_MACOS" ]]; then
    echo "[ERROR] Qt 最低系统 $QT_MINOS 高于目标 $MIN_MACOS；请使用官方 Qt 6.8.x（minos 12.0）" >&2
    exit 1
fi

echo "==> CMake 配置 (Release)"
cmake -B "$BUILD_DIR" -S . \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_PREFIX_PATH="$QT_PREFIX" \
    -DCMAKE_OSX_DEPLOYMENT_TARGET="$MIN_MACOS" \
    -DCMAKE_OSX_ARCHITECTURES="$ARCHS"

echo "==> 编译"
cmake --build "$BUILD_DIR" --parallel "$(sysctl -n hw.ncpu)"

APP="$BUILD_DIR/appVibeKeyF3Studio.app"
if [[ "$MODE" == "build" ]]; then
    echo "==> 完成（跳过打包/签名）: $APP"
    exit 0
fi

# ---------------------------------------------------------------------------
# 内置 Python 运行时
# ---------------------------------------------------------------------------
runtime_url() {
    local arch="$1"
    echo "https://github.com/astral-sh/python-build-standalone/releases/download/${PY_RELEASE}/cpython-${PY_VERSION}%2B${PY_RELEASE}-${arch}-apple-darwin-install_only_stripped.tar.gz"
}

runtime_sha() {
    case "$1" in
        arm64)  echo "$PY_SHA_ARM64" ;;
        x86_64) echo "$PY_SHA_X86_64" ;;
        *)      echo "" ;;
    esac
}

runtime_run() {
    # 在正确架构下执行 Python（Apple Silicon 上跑 x86_64 需要 Rosetta 2）
    local arch="$1"; shift
    if [[ "$arch" == "x86_64" ]]; then
        arch -x86_64 "$@"
    else
        "$@"
    fi
}

runtime_ok() {
    local arch="$1" dir="$2"
    local py="$dir/python/bin/python3"
    [[ -x "$py" ]] || return 1
    # -s：忽略用户级 site-packages。运行期 main.cpp 给 worker 设了
    # PYTHONNOUSERSITE=1，校验必须用同样的隔离语义，否则构建机上残留的
    # 旧依赖（如 ~/.local 里的 bleak 3.0.2）会掩盖 bundle 自身缺件。
    runtime_run "$arch" "$py" -s -c \
        'import serial, bleak; from bleak import BleakAdapter' >/dev/null 2>&1
}

download_runtime() {
    local url="$1" out="$2" expected="$3"
    local try_urls=()
    # 有镜像时优先镜像（国内直连 GitHub Release 常超时）
    if [[ -n "$GITHUB_PROXY" ]]; then
        try_urls+=("${GITHUB_PROXY%/}/$url")
    fi
    try_urls+=("$url")
    local u
    for u in "${try_urls[@]}"; do
        echo "    下载: $u"
        rm -f "$out"
        if curl --http1.1 -fL --retry 3 --retry-delay 2 \
                --connect-timeout 20 --max-time 900 -o "$out" "$u"; then
            local actual
            actual="$(shasum -a 256 "$out" | awk '{print $1}')"
            if [[ "$actual" == "$expected" ]]; then
                return 0
            fi
            echo "    [warn] SHA256 不匹配（$actual），换下一个源"
        else
            echo "    [warn] 下载失败，换下一个源"
        fi
    done
    rm -f "$out"
    return 1
}

prepare_runtime() {
    local arch="$1"
    local dir="$PY_RUNTIME_DIR/$arch"
    local py="$dir/python/bin/python3"
    if runtime_ok "$arch" "$dir"; then
        echo "==> 复用已准备好的 Python $arch: $dir"
        return 0
    fi

    echo "==> 准备内置 Python $arch ($PY_VERSION)"
    mkdir -p "$dir"
    rm -rf "$dir/python" "$dir/runtime.tar.gz"
    if ! download_runtime "$(runtime_url "$arch")" "$dir/runtime.tar.gz" "$(runtime_sha "$arch")"; then
        echo "[ERROR] 无法下载 Python $arch 运行时。可设置 GITHUB_PROXY 后重试。" >&2
        exit 1
    fi
    tar -xzf "$dir/runtime.tar.gz" -C "$dir"
    rm -f "$dir/runtime.tar.gz"

    if ! runtime_run "$arch" "$py" -m pip --version >/dev/null 2>&1; then
        echo "[ERROR] 内置 Python $arch 没有 pip；请检查下载包是否为 install_only" >&2
        exit 1
    fi
    echo "    安装 pyserial + bleak ..."
    if ! runtime_run "$arch" "$py" -m pip install --no-cache-dir --disable-pip-version-check -q \
            "pyserial==3.5" "bleak==3.1.0"; then
        echo "[ERROR] 安装 Python 依赖失败（$arch）。x86_64 运行时需要 Rosetta 2；" >&2
        echo "        可在 Apple Silicon 上执行: softwareupdate --install-rosetta --agree-to-license" >&2
        exit 1
    fi
    if ! runtime_ok "$arch" "$dir"; then
        echo "[ERROR] Python $arch 运行时校验失败（serial/bleak/BleakAdapter）" >&2
        exit 1
    fi
}

# 只准备本次实际构建的架构
BUILD_ARCHS=()
IFS=';' read -r -a BUILD_ARCHS <<< "$ARCHS"
for arch in "${BUILD_ARCHS[@]}"; do
    case "$arch" in
        arm64|x86_64) ;;
        *) echo "[ERROR] 不支持的架构: $arch（只支持 arm64 / x86_64）" >&2; exit 1 ;;
    esac
    if ! lipo -info "$QT_PREFIX/lib/QtCore.framework/Versions/A/QtCore" 2>/dev/null | grep -qw "$arch"; then
        echo "[ERROR] 当前 Qt 不包含 $arch 架构；universal2 构建必须使用官方 Qt" >&2
        exit 1
    fi
    prepare_runtime "$arch"
done

# ---------------------------------------------------------------------------
# macdeployqt
# ---------------------------------------------------------------------------
echo "==> macdeployqt 打包"
set +e
"$QT_PREFIX/bin/macdeployqt" "$APP" \
    -qmldir=qml \
    -libpath="$QT_PREFIX/lib" >"$BUILD_DIR/macdeployqt.log" 2>&1
MACDEPLOY_RC=$?
set -e
# MacOS/ 下的 *.py 不是 Mach-O，macdeployqt 会报 "not an object file"；这是无害噪音。
grep -v "is not an object file" "$BUILD_DIR/macdeployqt.log" || true
if [[ $MACDEPLOY_RC -ne 0 ]]; then
    echo "[ERROR] macdeployqt 失败 (exit $MACDEPLOY_RC)，完整日志: $BUILD_DIR/macdeployqt.log" >&2
    exit "$MACDEPLOY_RC"
fi

# ---------------------------------------------------------------------------
# 把 Python 运行时放进 Contents/Resources/python/<arch>/
# ---------------------------------------------------------------------------
for arch in "${BUILD_ARCHS[@]}"; do
    SRC="$PY_RUNTIME_DIR/$arch/python"
    DST="$APP/Contents/Resources/python/$arch"
    echo "==> 内置 Python $arch"
    rm -rf "$DST"
    mkdir -p "$(dirname "$DST")"
    ditto "$SRC" "$DST"
done

# Python 在安装依赖/本地测试时会生成 __pycache__；这些 .pyc 不能进签名包：
# 运行期一旦新增/更新就会破坏 codesign 的资源封条。运行时已通过
# PYTHONDONTWRITEBYTECODE=1 禁止再生成，这里把构建期残留清干净。
find "$APP/Contents/MacOS" "$APP/Contents/Resources/python" \
    -type d -name __pycache__ -prune -exec rm -rf {} + 2>/dev/null || true
find "$APP/Contents/MacOS" "$APP/Contents/Resources/python" \
    -type f -name '*.pyc' -delete 2>/dev/null || true

# ---------------------------------------------------------------------------
# 校验 bundle 内的运行时是自包含的
# ---------------------------------------------------------------------------
# 这一步是"换一台 Mac 就看不到设备"事故的守门员：
#   内置运行时的依赖必须能从 bundle 自己解析出来。CPython 默认会把
#   用户级 site-packages(~/.local) 排在 bundle 目录之前，用户机器上残留的
#   旧版本会劫持内置依赖（实测 bleak 3.0.2 vs 内置 3.1.0 → BleakAdapter
#   ImportError → 所有 BLE worker 崩 → 界面永远"未连接"）。
#   运行期已用 PYTHONNOUSERSITE=1 隔离，这里用同样的 -s 复验，并额外确认
#   模块确实来自 bundle 内部而非任何外部路径。
echo "==> 校验内置 Python 自包含"
BUNDLE_REAL="$(cd "$APP" && pwd -P)"
for arch in "${BUILD_ARCHS[@]}"; do
    PYBIN="$APP/Contents/Resources/python/$arch/bin/python3"
    if [[ ! -x "$PYBIN" ]]; then
        echo "[ERROR] 缺少内置 Python: $PYBIN" >&2
        exit 1
    fi
    # 注意用 realpath 比对：构建常发生在 /tmp 或符号链接目录下，
    # 直接比字符串会把 /tmp/x 和 /private/tmp/x 判成不同。
    if ! PROBE_OUT="$(VK_EXPECT_BUNDLE="$BUNDLE_REAL" \
            runtime_run "$arch" "$PYBIN" -s -c '
import os, sys
import serial, bleak
from bleak import BleakAdapter
root = os.path.realpath(os.environ["VK_EXPECT_BUNDLE"])
paths = [os.path.realpath(sys.prefix),
         os.path.realpath(serial.__file__),
         os.path.realpath(bleak.__file__)]
outside = [p for p in paths if not p.startswith(root + os.sep)]
if outside:
    print("NOT_SELF_CONTAINED")
    for p in paths:
        print("  " + p)
    raise SystemExit(1)
print(bleak.__file__)
' 2>&1)"; then
        echo "[ERROR] 内置 Python $arch 不能自包含（依赖被 bundle 外路径劫持）：" >&2
        echo "$PROBE_OUT" >&2
        exit 1
    fi
    echo "    内置 Python $arch OK"
done

# ---------------------------------------------------------------------------
# 重新签名（macdeployqt / Python 拷贝都会让原链接期签名失效）
# ---------------------------------------------------------------------------
echo "==> codesign"
xattr -cr "$APP" || true
if [[ "$CODESIGN_IDENTITY" == "-" ]]; then
    codesign --force --deep --sign - "$APP"
else
    codesign --force --deep --options runtime --timestamp \
        --sign "$CODESIGN_IDENTITY" "$APP"
fi
codesign --verify --deep --strict --verbose=2 "$APP"

# ---------------------------------------------------------------------------
# DMG（可拖拽安装）
# ---------------------------------------------------------------------------
if [[ "${MAKE_DMG:-1}" == "1" ]]; then
    echo
    echo "==> 制作 DMG"
    CODESIGN_IDENTITY="$CODESIGN_IDENTITY" ./build_dmg_macos.sh "$APP"
fi

# ---------------------------------------------------------------------------
# 结果
# ---------------------------------------------------------------------------
echo
echo "==> 产物: $(pwd)/$APP"
echo "    bundle id : $(plutil -extract CFBundleIdentifier raw "$APP/Contents/Info.plist")"
echo "    version   : $(plutil -extract CFBundleShortVersionString raw "$APP/Contents/Info.plist")"
echo "    min macOS : $(plutil -extract LSMinimumSystemVersion raw "$APP/Contents/Info.plist")"
echo "    arch      : $(file "$APP/Contents/MacOS/appVibeKeyF3Studio" | sed 's/.*: //')"
for arch in arm64 x86_64; do
    if lipo -info "$APP/Contents/MacOS/appVibeKeyF3Studio" 2>/dev/null | grep -qw "$arch"; then
        echo "    minos/$arch: $(vtool -show-build -arch "$arch" "$APP/Contents/MacOS/appVibeKeyF3Studio" 2>/dev/null | awk '/minos/{print $2; exit}')"
    fi
done
echo "    大小      : $(du -sh "$APP" | cut -f1)"
if spctl --assess --type execute "$APP" >/dev/null 2>&1; then
    echo "    Gatekeeper: accepted"
else
    echo "    Gatekeeper: 未通过（ad-hoc 签名正常；对外分发需 Developer ID + notarization）"
fi
echo
echo "运行: open \"$APP\""
