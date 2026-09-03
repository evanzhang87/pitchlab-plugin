#!/usr/bin/env bash
# PitchLab 一键更新 + 校验 + 安装 (macOS)
#
# 用法:   ./update-pitchlab.sh
# 依赖:   curl unzip lipo codesign xattr  (macOS 自带)
#         若无 Release 资产，会回退用 `gh` 拉最新 Actions 产物(需 brew install gh && gh auth login)
#
set -euo pipefail

REPO="evanzhang87/pitchlab-plugin"
ASSET="PitchLab-macOS-universal"
VST3_DIR="$HOME/Library/Audio/Plug-Ins/VST3"
COMP_DIR="$HOME/Library/Audio/Plug-Ins/Components"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

echo "== PitchLab 一键更新 (macOS) =="
echo "   来源: $REPO"

# ---------- 1) 下载：优先 Release 资产(公开免token)，否则回退 gh ----------
ZIP="$TMP/download.zip"
if curl -fsSL --max-time 300 -o "$ZIP" \
     "https://github.com/$REPO/releases/latest/download/$ASSET.zip" 2>/dev/null; then
  echo "  [下载] GitHub Release 最新资产"
else
  echo "  [下载] 无 Release 资产，回退用 gh 拉最新 Actions 产物..."
  if ! command -v gh >/dev/null 2>&1; then
    cat <<'EOF'
  错误: 需要 gh CLI 且已登录才能从 Actions 产物下载。
    brew install gh
    gh auth login
  或者让 workflow 自动发布 Release 后再运行本脚本。
EOF
    exit 1
  fi
  gh run download -n "$ASSET" -R "$REPO" -D "$TMP" >/dev/null 2>&1 || true
  F=$(find "$TMP" -name "*.zip" | head -1 || true)
  if [ -z "$F" ]; then echo "  错误: 未取到产物"; exit 1; fi
  cp "$F" "$ZIP"
fi
[ -s "$ZIP" ] || { echo "  错误: 下载为空"; exit 1; }

# ---------- 2) 解包(自动解开可能的嵌套 zip) ----------
mkdir -p "$TMP/x"
unzip -q "$ZIP" -d "$TMP/x"
for f in "$TMP"/x/*.zip; do
  if [ -e "$f" ]; then unzip -qo "$f" -d "$TMP/x" && rm -f "$f"; fi
done
VST3_BUNDLE=$(find "$TMP/x" -name "PitchLab.vst3" -type d | head -1)
AU_BUNDLE=$(find "$TMP/x" -name "PitchLab.component" -type d | head -1)
if [ -z "$VST3_BUNDLE" ]; then
  echo "错误: 未找到 PitchLab.vst3，解包内容如下："; find "$TMP/x" -maxdepth 4 | head -30; exit 1
fi

# ---------- 3) 校验 ----------
echo "== 校验 =="
MAIN="$VST3_BUNDLE/Contents/MacOS/PitchLab"
if command -v lipo >/dev/null 2>&1 && [ -f "$MAIN" ]; then
  ARCH="$(lipo -archs "$MAIN" 2>/dev/null || true)"
  echo "  架构: $ARCH"
  if echo "$ARCH" | grep -q arm64 && echo "$ARCH" | grep -q x86_64; then
    echo "  [架构] OK 通用二进制 (arm64 + x86_64)"
  else
    echo "  [架构] 警告: 不是通用二进制 -> $ARCH"
  fi
fi

# 去隔离 + 重新本地签名(DAW 校验需要)
for b in "$VST3_BUNDLE" "$AU_BUNDLE"; do
  [ -z "$b" ] && continue
  xattr -dr com.apple.quarantine "$b" 2>/dev/null || true
  codesign --force --deep -s - "$b" 2>/dev/null || true
done
if codesign --verify --deep --strict "$VST3_BUNDLE" 2>/dev/null; then
  echo "  [签名] OK"
else
  echo "  [签名] 校验未通过，仍尝试安装(若 DAW 拒绝请复查 codesign --verify)"
fi

# ---------- 4) 安装(旧版备份为 .bak) ----------
mkdir -p "$VST3_DIR" "$COMP_DIR"
install_bundle () {
  local src="$1" dst="$2"
  [ -e "$dst" ] && rm -rf "$dst.bak" && mv "$dst" "$dst.bak"
  ditto "$src" "$dst"
}
install_bundle "$VST3_BUNDLE" "$VST3_DIR/PitchLab.vst3"
echo "  VST3 -> $VST3_DIR/PitchLab.vst3"
if [ -n "$AU_BUNDLE" ]; then
  install_bundle "$AU_BUNDLE" "$COMP_DIR/PitchLab.component"
  echo "  AU   -> $COMP_DIR/PitchLab.component"
fi

echo ""
echo "完成! 请完全退出并重启 Fender Studio Pro，"
echo "然后 Settings -> Locations -> VST Plug-ins -> Rescan Failed Plug-ins。"
