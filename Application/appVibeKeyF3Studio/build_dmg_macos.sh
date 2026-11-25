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
    QUARANTINE_NOTE="注意（ad-hoc 签名的包才会有）：
  从网络/AirDrop 收到的 DMG 带 com.apple.quarantine 隔离标记。App 本身
  可以用\"右键 → 打开\"放行，但内置的 Python worker 不走这条豁免：它会被
  Gatekeeper 挂住（进程卡在 _dyld_start 永不返回），表现为一直\"设备未连接\"。
  装到 /Applications 后执行一次：

    xattr -dr com.apple.quarantine \"/Applications/$APP_NAME.app\"

  再打开 App 即可。彻底解决需要 Developer ID 签名 + 公证，届时无需用户操作。"
else
    SIGN_NOTE="当前构建已使用 Developer ID 签名。"
    QUARANTINE_NOTE=""
fi
cat > "$STAGING/安装说明.txt" <<EOF
$APP_NAME v$VERSION

安装：
  将 $APP_NAME.app 拖到左侧 Applications 文件夹。

首次打开：
  如果 macOS 提示“无法验证开发者”，请在 Finder 中右键 App → 打开。
  $SIGN_NOTE

$QUARANTINE_NOTE

蓝牙连不上 / 找不到设备：
  1) 设备空闲 60 秒会停止广播进入休眠：先按任意键唤醒，再点客户端里的
     “检查设备”。如果设备正连着另一台电脑，它不会广播——请先关掉那台
     电脑的蓝牙，或把设备关机。
  2) 设备的每个蓝牙槽位只认上一次配对的电脑。换一台电脑时，请把设备关机，
     按住 L2（或 L3，选一个没被占用的槽位）再开机，黄灯闪烁时在
     「系统设置 → 蓝牙」里配对。注意：L1 会清掉原来那台电脑的配对；
     用 L2/L3 可以让两台电脑并存（开机时按 C1/C2/C3 切换槽位）。
  3) 第一次打开客户端若弹出“是否允许使用蓝牙”，请选择“允许”。
     千万别反复点“不允许”——客户端会周期性地新起探测进程，
     弹窗会连环出现；点一次“允许”即可永久生效。
     若误点了“不允许”，到「系统设置 → 隐私与安全性 → 蓝牙」勾选本 App，
     然后重启客户端。
  4) 也可以直接用 USB 数据线连接设备（需数据线，非纯充电线）：
     插线即可配置，不需要蓝牙。
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
