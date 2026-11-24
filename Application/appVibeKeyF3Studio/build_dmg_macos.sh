#!/bin/bash
# ============================================================================
#  build_dmg_macos.sh - 把已签名的 .app 打成可拖拽安装的 DMG
#
#  用法:
#    ./build_dmg_macos.sh [path/to/appVibeKeyF3Studio.app]
#    DMG_PATH=/path/to/out.dmg ./build_dmg_macos.sh
#
#  产物默认:
#    build/macos/VibeKey-F3_Studio_v<版本>_<universal|arm64|x86_64>.dmg
#
#  DMG 内容:
#    - VibeKey-F3 Studio.app
#    - Applications 快捷方式（拖进去即可安装）
#    - 安装说明.txt
# ============================================================================
set -euo pipefail

cd "$(dirname "$0")"

APP="${1:-build/macos/appVibeKeyF3Studio.app}"
CODESIGN_IDENTITY="${CODESIGN_IDENTITY:--}"

if [[ ! -d "$APP" ]]; then
    echo "[ERROR] 找不到 app bundle: $APP" >&2
    exit 1
fi

VERSION="$(plutil -extract CFBundleShortVersionString raw "$APP/Contents/Info.plist")"
APP_NAME="$(plutil -extract CFBundleName raw "$APP/Contents/Info.plist")"
ARCH="unknown"
if lipo -info "$APP/Contents/MacOS/appVibeKeyF3Studio" 2>/dev/null | grep -qw arm64 \
   && lipo -info "$APP/Contents/MacOS/appVibeKeyF3Studio" 2>/dev/null | grep -qw x86_64; then
    ARCH="universal"
elif lipo -info "$APP/Contents/MacOS/appVibeKeyF3Studio" 2>/dev/null | grep -qw arm64; then
    ARCH="arm64"
elif lipo -info "$APP/Contents/MacOS/appVibeKeyF3Studio" 2>/dev/null | grep -qw x86_64; then
    ARCH="x86_64"
fi

DMG="${DMG_PATH:-build/macos/VibeKey-F3_Studio_v${VERSION}_${ARCH}.dmg}"
mkdir -p "$(dirname "$DMG")"

STAGING="$(mktemp -d "${TMPDIR:-/tmp}/vibekey-dmg.XXXXXX")"
MOUNT_DIR="$(mktemp -d "${TMPDIR:-/tmp}/vibekey-dmg-mount.XXXXXX")"
cleanup() {
    if mount | grep -q "on $MOUNT_DIR "; then
        hdiutil detach "$MOUNT_DIR" -quiet >/dev/null 2>&1 || true
    fi
    rm -rf "$STAGING" "$MOUNT_DIR"
}
trap cleanup EXIT

echo "==> 准备 DMG 内容"
ditto "$APP" "$STAGING/$APP_NAME.app"
ln -s /Applications "$STAGING/Applications"
if [[ "$CODESIGN_IDENTITY" == "-" ]]; then
    SIGN_NOTE="当前构建为 ad-hoc 签名；对外正式分发需 Developer ID 签名 + 公证。"
else
    SIGN_NOTE="当前构建已使用 Developer ID 签名。"
fi
cat > "$STAGING/安装说明.txt" <<EOF
$APP_NAME v$VERSION

安装：
  将 $APP_NAME.app 拖到左侧 Applications 文件夹。

首次打开：
  如果 macOS 提示“无法验证开发者”，请在 Finder 中右键 App → 打开。
  $SIGN_NOTE
EOF

echo "==> 生成 DMG: $DMG"
rm -f "$DMG"
hdiutil create \
    -volname "$APP_NAME $VERSION" \
    -srcfolder "$STAGING" \
    -fs HFS+ \
    -format UDZO \
    -ov \
    "$DMG" >/dev/null

# DMG 本身也签一下；ad-hoc 仅用于完整性，Gatekeeper 仍以后续公证为准。
echo "==> 签名 DMG"
if ! codesign --force --sign "$CODESIGN_IDENTITY" "$DMG" >/dev/null 2>&1; then
    echo "    [warn] DMG 签名失败（不影响本地安装/校验）"
fi

echo "==> 校验 DMG"
hdiutil verify "$DMG" >/dev/null
hdiutil attach "$DMG" -nobrowse -readonly -mountpoint "$MOUNT_DIR" >/dev/null
trap - EXIT
if [[ ! -d "$MOUNT_DIR/$APP_NAME.app" ]]; then
    hdiutil detach "$MOUNT_DIR" -quiet >/dev/null 2>&1 || true
    rm -rf "$STAGING" "$MOUNT_DIR"
    echo "[ERROR] DMG 内缺少 $APP_NAME.app" >&2
    exit 1
fi
if [[ ! -L "$MOUNT_DIR/Applications" ]]; then
    hdiutil detach "$MOUNT_DIR" -quiet >/dev/null 2>&1 || true
    rm -rf "$STAGING" "$MOUNT_DIR"
    echo "[ERROR] DMG 内缺少 Applications 快捷方式" >&2
    exit 1
fi
codesign --verify --deep --strict "$MOUNT_DIR/$APP_NAME.app"
hdiutil detach "$MOUNT_DIR" -quiet >/dev/null
rm -rf "$STAGING" "$MOUNT_DIR"

echo
echo "==> DMG: $(pwd)/$DMG"
echo "    version : $VERSION"
echo "    arch    : $ARCH"
echo "    大小    : $(du -sh "$DMG" | cut -f1)"
if spctl --assess --type execute "$APP" >/dev/null 2>&1; then
    echo "    Gatekeeper: accepted"
else
    echo "    Gatekeeper: 未通过（ad-hoc 签名正常；对外分发需 Developer ID + notarization）"
fi
echo
echo "安装: open \"$DMG\""
