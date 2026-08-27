#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WIN_DEPS="$ROOT/win-deps.sh"

# 复用 win-deps.sh 的 env.sh 生成段(抽出为函数前,直接测关键不变量):
# 抽出 'cat > "$USER_DEPS/env.sh" <<EOF' 到 'EOF' 之间的 env.sh 主体,断言不含 /mingw64。
env_block="$(awk '
  index($0, "USER_DEPS/env.sh") { in_env=1; next }
  in_env && $0 == "EOF" { exit }
  in_env { print }
' "$WIN_DEPS")"

[ -n "$env_block" ] || { echo "FAIL: 未抽取到 env.sh 生成段"; exit 1; }
printf '%s\n' "$env_block" | grep -q 'CMAKE_PREFIX_PATH' || { echo "FAIL: env.sh 缺 CMAKE_PREFIX_PATH"; exit 1; }
if printf '%s\n' "$env_block" | grep -q 'mingw64'; then
  echo "FAIL: env.sh 仍含 /mingw64"; exit 1
fi
# env.sh 前缀引用 QT_PREFIX;QT_PREFIX 定义须含 <version>/<target_arch> 层。
# 版本号在 QT_VER,arch 在 QT_PREFIX(格式 <user-deps>/qt/<QT_VER>/msvc2019_64)。
qt_ver="$(grep -E '^QT_VER=' "$WIN_DEPS" || true)"
[ -n "$qt_ver" ] || { echo "FAIL: win-deps 缺 QT_VER 定义"; exit 1; }
printf '%s\n' "$qt_ver" | grep -qE '6\.5\.3' || { echo "FAIL: QT_VER 非 6.5.3"; exit 1; }
qt_def="$(grep -nE '^QT_PREFIX=' "$WIN_DEPS" || true)"
[ -n "$qt_def" ] || { echo "FAIL: win-deps 缺 QT_PREFIX 定义"; exit 1; }
if ! printf '%s\n' "$qt_def" | grep -qE 'qt/\$QT_VER/msvc2019_64'; then
  echo "FAIL: QT_PREFIX 缺 <version>/<arch> 层"; exit 1
fi
# env.sh 前缀确实引用 QT_PREFIX(而非硬编码 /mingw64)
if ! printf '%s\n' "$env_block" | grep -qE 'CMAKE_PREFIX_PATH="\$QT_PREFIX:'; then
  echo "FAIL: env.sh CMAKE_PREFIX_PATH 未引用 QT_PREFIX"; exit 1
fi
# Qt 部署:不再走 pip/aqtinstall(MSYS2 python 无 pip),改直下官方 MSVC 预编译 + 7z 解压
if grep -qE 'pip install aqtinstall|aqt install-qt' "$WIN_DEPS"; then
  echo "FAIL: win-deps 仍依赖 pip/aqtinstall(MSYS2 python 无 pip)"; exit 1
fi
if ! grep -qE '7z x ' "$WIN_DEPS"; then
  echo "FAIL: win-deps 缺 7z 解压 Qt 预编译"; exit 1
fi
# runtime:env.sh 应前置 $QT_PREFIX/bin 到 PATH(Qt6 DLL 运行期可寻)
printf '%s\n' "$env_block" | grep -qE 'export PATH="\$QT_PREFIX/bin:\\\$PATH"' \
  || { echo "FAIL: env.sh 未把 Qt bin 前置到 PATH"; exit 1; }

# Vulkan SDK(MSVC 兼容,find_package(Vulkan) 需要):win-deps.sh 静默装 LunarG SDK 到
# .user-deps 下(自包含,不碰系统 C:\VulkanSDK),并把 VULKAN_SDK 写进 env.sh 供
# setup-env.sh source 后传给 gen-projects.py 子进程(CMake FindVulkan 自动认 ENV{VULKAN_SDK})。
if ! grep -qE 'sdk\.lunarg\.com/sdk/download/latest/windows/vulkan_sdk\.exe' "$WIN_DEPS"; then
  echo "FAIL: win-deps 缺 LunarG Vulkan SDK(MSVC 兼容)下载 URL"; exit 1
fi
if ! grep -qE -- '--accept-licenses' "$WIN_DEPS" || ! grep -qE -- '--confirm-command' "$WIN_DEPS"; then
  echo "FAIL: win-deps 缺 Vulkan SDK 静默安装参数(--accept-licenses/--confirm-command)"; exit 1
fi
vk_dir_def="$(grep -nE '^VULKAN_SDK_DIR=' "$WIN_DEPS" || true)"
[ -n "$vk_dir_def" ] || { echo "FAIL: win-deps 缺 VULKAN_SDK_DIR 定义"; exit 1; }
if ! printf '%s\n' "$vk_dir_def" | grep -qE '\$USER_DEPS/'; then
  echo "FAIL: VULKAN_SDK_DIR 未落在 .user-deps 下(应自包含,不碰系统 C:\\VulkanSDK)"; exit 1
fi
if ! printf '%s\n' "$env_block" | grep -qE '^export VULKAN_SDK='; then
  echo "FAIL: env.sh 缺 VULKAN_SDK(CMake FindVulkan 需要 ENV{VULKAN_SDK} 才能在 MSVC 下定位)"; exit 1
fi

echo "PASS env.sh no /mingw64 + QT_PREFIX 含 6.5.3/msvc2019_64 + 直下预编译 + Vulkan SDK MSVC 兼容"
