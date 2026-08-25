#!/usr/bin/env bash
# Windows(MSYS2)依赖部署:Vulkan SDK zip + SwiftShader + Qt6(pacman)。
# 由 install-user-deps.sh 平台分支调用;也可单独执行。
set -euo pipefail
info() { printf '[INFO] %s\n' "$*"; }
err()  { printf '[ERROR] %s\n' "$*" >&2; }
die()  { err "$*"; exit 1; }
warn() { printf '[WARN] %s\n' "$*"; }
has()  { command -v "$1" >/dev/null 2>&1; }

MINE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
USER_DEPS="$MINE_ROOT/.user-deps"
DEB_CACHE="$USER_DEPS/.deb-cache"
mkdir -p "$USER_DEPS" "$DEB_CACHE"

# --- ① Vulkan SDK(zip,含 glslc.exe + vulkan.h) ---
SDK_URL="https://sdk.lunarg.com/sdk/download/latest/windows/vulkan-sdk-latest.zip"
SDK_ZIP="$DEB_CACHE/vulkan-sdk-latest.zip"
SDK_DIR="$USER_DEPS/vulkan-sdk"
if [ ! -d "$SDK_DIR" ]; then
  info "① 下载 Vulkan SDK ($SDK_URL)"
  curl -fL --retry 3 -o "$SDK_ZIP" "$SDK_URL"
  mkdir -p "$SDK_DIR"
  unzip -q -o "$SDK_ZIP" -d "$SDK_DIR"
fi
# 找 glslc.exe 与 include/vulkan/vulkan.h
GLSLC="$(find "$SDK_DIR" -name 'glslc.exe' -type f | head -1 || true)"
[ -n "$GLSLC" ] || die "SDK 缺少 glslc.exe"
SDK_BIN="$(dirname "$GLSLC")"
VULKAN_INC="$(find "$SDK_DIR" -type d -path '*/Include/vulkan' | head -1 || true)"
[ -n "$VULKAN_INC" ] || die "SDK 缺少 Include/vulkan/vulkan.h"
info "① glslc: $GLSLC;vulkan.h: $VULKAN_INC/vulkan.h"

# --- ② SwiftShader(经池构建,见 Task 2;此处仅确保 ICD 路径) ---
SWSS_ICD="$MINE_ROOT/third_party/_install/swiftshader-master/release/vk_swiftshader_icd.json"
if [ -f "$SWSS_ICD" ]; then
  SWSS_BIN="$(dirname "$SWSS_ICD")"
  info "② SwiftShader ICD: $SWSS_ICD"
else
  warn "② SwiftShader ICD 未找到: $SWSS_ICD(将由 tools/build-deps.py --all 构建产出;本步先继续生成 env.sh)"
  SWSS_BIN=""
fi

# --- ③ Qt6(pacman) ---
if ! pacman -Q mingw-w64-x86_64-qt6-base >/dev/null 2>&1; then
  info "③ pacman 安装 Qt6 base"
  pacman -S --needed --noconfirm mingw-w64-x86_64-qt6-base
else
  info "③ Qt6 已安装"
fi

# --- ④ 生成 env.sh ---
cat > "$USER_DEPS/env.sh" <<EOF
# Mine Windows(MSYS2)用户级依赖环境(由 win-deps.sh 生成)。
export MINE_ROOT="$MINE_ROOT"
export USER_DEPS="$USER_DEPS"
export PATH="$SDK_BIN:\$PATH"
export VK_ICD_FILENAMES="$SWSS_ICD"
export VK_DRIVER_FILES="$SWSS_ICD"
export CMAKE_PREFIX_PATH="$SDK_BIN/..:$MINE_ROOT/third_party/_install/glfw-3.4/release"
EOF
info "④ 已生成 $USER_DEPS/env.sh"

# --- ⑤ 离屏 Vulkan 探针(SwiftShader 能创建 device) ---
"$GLSLC" -fshader-stage=fragment -o "$DEB_CACHE/probe.frag.spv" - <<'EOF' || die "glslc 编译失败"
#version 450
layout(location=0) out vec4 outColor;
void main(){ outColor = vec4(1.0,0.0,0.0,1.0); }
EOF
info "⑤ glslc 探针通过"
info "完成。使用前: source $USER_DEPS/env.sh"
