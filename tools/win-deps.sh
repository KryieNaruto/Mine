#!/usr/bin/env bash
# Windows(Git Bash + MSVC)依赖部署:工具链 + Vulkan(MSVC 兼容)+ Qt6(MSVC 预编译)+ SwiftShader(池构建)。
# 由 install-user-deps.sh 平台分支调用;也可单独执行。
# 不依赖 MSYS2/pacman:工具链按"复用现成 → VS Build Tools 自带(VCTools 含 CMake+Ninja)→ 独立下载"三级补齐。
set -euo pipefail
info() { printf '[INFO] %s\n' "$*"; }
err()  { printf '[ERROR] %s\n' "$*" >&2; }
die()  { err "$*"; exit 1; }
warn() { printf '[WARN] %s\n' "$*"; }
has()  { command -v "$1" >/dev/null 2>&1; }

MINE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
USER_DEPS="$MINE_ROOT/.user-deps"
DEB_CACHE="$USER_DEPS/.deb-cache"
TOOL_BIN="$USER_DEPS/bin"
mkdir -p "$USER_DEPS" "$DEB_CACHE" "$TOOL_BIN"
# 独立工具链(shims / standalone 二进制)前置;MSYS2-free 下 cmake/ninja/python3/7zr 都靠它或 VS。
export PATH="$TOOL_BIN:$PATH"

# 镜像优先下载:依次尝试各 URL,首个成功(非空文件)即返回;全部失败返回 1。
# 每 URL 带 --max-time 防"卡死无输出"(git bash 下 github 直连常不可达,必须能失败继续/明确报错)。
dl_mirror() {
  local dest="$1"; shift
  local url
  for url in "$@"; do
    if curl -fL --retry 3 --max-time 1800 -o "$dest" "$url" 2>/dev/null; then
      [ -s "$dest" ] && return 0
    fi
  done
  return 1
}

# --- 0. MSVC 工具链:定位/自动安装 Build Tools(VCTools 工作负载自带 CMake + Ninja),写 vcvars.sh ---
# shellcheck disable=SC1091
. "$MINE_ROOT/tools/deps_lib/msvc.sh"
msvc_ensure
MSVC_ROOT="$(msvc_locate)" || die "MSVC 定位失败"
msvc_write_vcvars_sh "$MSVC_ROOT"

# --- ① CMake / Ninja:复用现成 → VS 自带(VCTools)→ 独立下载到 $TOOL_BIN ---
VS_CMAKE_BIN=""
VS_NINJA_BIN=""
if ! has cmake; then
  if [ -x "$MSVC_ROOT/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin/cmake.exe" ]; then
    VS_CMAKE_BIN="$MSVC_ROOT/Common7/IDE/CommonExtensions/Microsoft/CMake/CMake/bin"
    info "① cmake 复用 VS 自带: $VS_CMAKE_BIN"
  else
    CMAKE_VER="3.30.0"
    info "① 未找到 cmake,下载独立包($CMAKE_VER)到 $TOOL_BIN ..."
    CMAKE_ZIP="$DEB_CACHE/cmake.zip"
    dl_mirror "$CMAKE_ZIP" \
      "https://mirrors.tuna.tsinghua.edu.cn/github-release/Kitware/CMake/v$CMAKE_VER/cmake-$CMAKE_VER-windows-x86_64.zip" \
      "https://github.com/Kitware/CMake/releases/download/v$CMAKE_VER/cmake-$CMAKE_VER-windows-x86_64.zip" \
      || die "cmake 独立包下载失败(镜像全挂);可安装 VS Build Tools(自带 cmake)后重跑"
    CMAKE_X="$DEB_CACHE/cmake-x"
    rm -rf "$CMAKE_X"; mkdir -p "$CMAKE_X"
    ( cd "$CMAKE_X" && unzip -qo "$CMAKE_ZIP" ) || die "cmake 独立包解压失败"
    # zip 内为 cmake-<ver>-windows-x86_64/bin/*(cmake.exe 及其 DLL),整体拷进 TOOL_BIN
    cp -f "$CMAKE_X"/*/bin/* "$TOOL_BIN/" 2>/dev/null || die "cmake 独立包结构异常"
    rm -rf "$CMAKE_ZIP" "$CMAKE_X"
    has cmake || die "cmake 安装后仍不可用"
    info "① cmake 就绪: $TOOL_BIN/cmake.exe"
  fi
fi
if ! has ninja; then
  if [ -x "$MSVC_ROOT/Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja/ninja.exe" ]; then
    VS_NINJA_BIN="$MSVC_ROOT/Common7/IDE/CommonExtensions/Microsoft/CMake/Ninja"
    info "① ninja 复用 VS 自带: $VS_NINJA_BIN"
  else
    NINJA_VER="1.11.1"
    info "① 未找到 ninja,下载独立包到 $TOOL_BIN ..."
    NINJA_ZIP="$DEB_CACHE/ninja.zip"
    dl_mirror "$NINJA_ZIP" \
      "https://mirrors.tuna.tsinghua.edu.cn/github-release/ninja-build/ninja/v$NINJA_VER/ninja-win.zip" \
      "https://github.com/ninja-build/ninja/releases/download/v$NINJA_VER/ninja-win.zip" \
      || die "ninja 独立包下载失败(镜像全挂)"
    ( cd "$DEB_CACHE" && unzip -qo "$NINJA_ZIP" -d "$TOOL_BIN" ) || die "ninja 独立包解压失败"
    rm -f "$NINJA_ZIP"
    has ninja || die "ninja 安装后仍不可用"
    info "① ninja 就绪: $TOOL_BIN/ninja.exe"
  fi
fi

# --- ② Python3(需带 yaml):复用现成 → 独立安装到 .user-deps/python + pyyaml + python3 shim ---
PYPI_MIRROR="https://pypi.tuna.tsinghua.edu.cn/simple"
PY_VER="3.12.7"
PY_DIR=""   # 非空 = 用独立 Python(其目录同时进 env.sh 的 PATH,python3 shim 是 exec 真实 python.exe 的包装脚本)
ensure_pyyaml() { # $1 = python 命令;装 yaml 到该解释器
  "$1" -m pip install --no-warn-script-location -q -i "$PYPI_MIRROR" pyyaml 2>/dev/null \
    || "$1" -m pip install --no-warn-script-location -q pyyaml 2>/dev/null
}
install_standalone_python() {
  local _exe="$DEB_CACHE/python-setup.exe"
  PY_DIR="$USER_DEPS/python"
  info "② 下载独立 Python($PY_VER)静默安装到 $PY_DIR ..."
  dl_mirror "$_exe" \
    "https://registry.npmmirror.com/-/binary/python/$PY_VER/python-$PY_VER-amd64.exe" \
    "https://mirrors.huaweicloud.com/python/$PY_VER/python-$PY_VER-amd64.exe" \
    "https://www.python.org/ftp/python/$PY_VER/python-$PY_VER-amd64.exe" \
    || die "Python 安装器下载失败(镜像全挂);可手动装 Python 3.8+ 并确保 python3 可执行后重跑"
  # --quiet 而非 /quiet:Git Bash/MSYS 会把 /xxx 参数做路径转换,双横线前缀不受影响(Burn 两种都认)。
  "$_exe" --quiet InstallAllUsers=0 PrependPath=0 Include_launcher=0 \
    Include_test=0 Include_doc=0 Include_pip=1 \
    "TargetDir=$(cygpath -m "$PY_DIR")" || die "Python 静默安装失败(可手动装 Python 3.8+ 后重跑)"
  rm -f "$_exe"
  [ -x "$PY_DIR/python.exe" ] || die "Python 安装后缺 python.exe"
  info "② 安装 pyyaml ..."
  ensure_pyyaml "$PY_DIR/python.exe" || die "pyyaml 安装失败(检查网络)"
  # python3 shim:脚本统一调 python3。用 bash 包装脚本 exec 真实 python.exe(而非 copy exe)——copy 会让
  # python 按新位置解析 sys.prefix,stdlib/site-packages(pyyaml)全找不到;包装脚本无此问题。
  cat > "$TOOL_BIN/python3" <<EOF
#!/bin/bash
exec "$PY_DIR/python.exe" "\$@"
EOF
  chmod +x "$TOOL_BIN/python3"
  info "② python3 就绪: $TOOL_BIN/python3 → $PY_DIR/python.exe"
}
if has python3 && python3 -c 'import yaml' >/dev/null 2>&1; then
  info "② python3 复用现成: $(command -v python3)"
elif has python3; then
  info "② python3 缺 yaml,补装 ..."
  if ! ensure_pyyaml python3; then
    warn "现有 python3 装不上 yaml,改装独立 Python"
    install_standalone_python
  fi
else
  install_standalone_python
fi

# --- ③ 7z / 7zr(Qt6 预编译 .7z 解压用;VS 不自带,Git Bash 也不带) ---
if has 7z; then _7z="7z"
elif has 7zr; then _7z="7zr"
else
  info "③ 下载 7zr.exe(解压 Qt 预编译)到 $TOOL_BIN ..."
  dl_mirror "$TOOL_BIN/7zr.exe" "https://www.7-zip.org/a/7zr.exe" \
    || die "7zr 下载失败;可手动放一份 7zr.exe 到 $TOOL_BIN/ 后重跑"
  _7z="7zr"
fi

# --- ④ Vulkan(单一来源:一份 MSVC 兼容 Vulkan SDK,glslc.exe 与 find_package(Vulkan) 都用它) ---
# 曾经拆两路:MSYS2 pacman 装 glslc/vulkan.h(MinGW ABI,给 shader 编译)+ 另装一份
# LunarG SDK 给 find_package(Vulkan)。冗余,且用户机器上往往已经装了真正的 Vulkan
# SDK(如 C:\VulkanSDK\<ver>,VULKAN_SDK 环境变量已设)——这种情况下 pacman 那份完全
# 多余,还会让人误以为项目依赖 MinGW 版 Vulkan。统一成一路:先探测机器上是否已有
# 可用 Vulkan SDK(VULKAN_SDK 环境变量,或磁盘扫描 C:\VulkanSDK\*,对齐 msvc_locate
# 的"先找现成、找不到才装"套路),有就直接复用(glslc.exe 与 vulkan.h/vulkan-1.lib
# SDK 自带);没有才静默装 LunarG 官方 SDK 到 .user-deps 下。

# vulkan_sdk_valid <root> : root 是否是可用的 Vulkan SDK(含 MSVC 需要的头/库/glslc)。
vulkan_sdk_valid() {
  local d
  d="$(cygpath -u "$1" 2>/dev/null || printf '%s' "$1")"
  [ -f "$d/Include/vulkan/vulkan.h" ] && [ -f "$d/Lib/vulkan-1.lib" ] && [ -f "$d/Bin/glslc.exe" ]
}

VULKAN_SDK_DIR=""
if [ -n "${VULKAN_SDK:-}" ] && vulkan_sdk_valid "$VULKAN_SDK"; then
  VULKAN_SDK_DIR="$(cygpath -u "$VULKAN_SDK" 2>/dev/null || printf '%s' "$VULKAN_SDK")"
  info "④ 复用已安装的 Vulkan SDK(\$VULKAN_SDK): $VULKAN_SDK_DIR"
else
  # VULKAN_SDK 环境变量在当前 shell 可能不可见(装完未重启终端);磁盘扫描
  # C:\VulkanSDK\* 兜底,目录名形如 1.4.357.0,sort -rV 取版本号最大的一个在前。
  for cand in /c/VulkanSDK/*/; do
    [ -d "$cand" ] || continue
    cand="${cand%/}"
    if vulkan_sdk_valid "$cand"; then
      VULKAN_SDK_DIR="$cand"
      info "④ 磁盘扫描到已安装 Vulkan SDK: $VULKAN_SDK_DIR"
      break
    fi
  done
fi

if [ -z "$VULKAN_SDK_DIR" ]; then
  VULKAN_SDK_DIR="$USER_DEPS/vulkan-sdk"
  if vulkan_sdk_valid "$VULKAN_SDK_DIR"; then
    info "④ Vulkan SDK(此前已装到 .user-deps)已存在: $VULKAN_SDK_DIR"
  else
    info "④ 未检测到已安装的 Vulkan SDK,下载 LunarG 官方 SDK(约几百 MB)..."
    VK_SDK_EXE="$DEB_CACHE/vulkan_sdk.exe"
    curl -fL --retry 3 --max-time 1800 -o "$VK_SDK_EXE" \
      "https://sdk.lunarg.com/sdk/download/latest/windows/vulkan_sdk.exe" \
      || die "Vulkan SDK 下载失败(需能出网)"
    info "④ 静默安装 Vulkan SDK 到 $VULKAN_SDK_DIR ..."
    # --root 指到 .user-deps 下的自定义目录(免管理员,不碰系统 C:\VulkanSDK);
    # --accept-licenses --default-answer --confirm-command install 是 LunarG 文档给出的
    # 全非交互静默安装组合(Qt Installer Framework 底层)。
    "$VK_SDK_EXE" --root "$(cygpath -m "$VULKAN_SDK_DIR")" \
      --accept-licenses --default-answer --confirm-command install \
      || { rm -f "$VK_SDK_EXE"; die "Vulkan SDK 静默安装失败。可从 https://vulkan.lunarg.com/sdk/home 手动装一份(装完重跑本脚本会自动探测复用,无需再改本脚本)。"; }
    rm -f "$VK_SDK_EXE"
    vulkan_sdk_valid "$VULKAN_SDK_DIR" \
      || die "Vulkan SDK 安装完成但未找到 Include/vulkan/vulkan.h、Lib/vulkan-1.lib 或 Bin/glslc.exe(安装可能不完整)"
  fi
fi

GLSLC="$VULKAN_SDK_DIR/Bin/glslc.exe"
[ -f "$GLSLC" ] || die "Vulkan SDK 缺 Bin/glslc.exe: $VULKAN_SDK_DIR"
info "④ glslc: $GLSLC;vulkan.h: $VULKAN_SDK_DIR/Include/vulkan/vulkan.h"
# 无论是复用到的还是刚装的,VULKAN_SDK 都要写进 env.sh 才能跨进程边界传下去:
# setup-env.sh 用 `bash install-user-deps.sh` 子进程调用本脚本(非 source),这里
# export 出去过不了进程边界;唯一可靠路径是写文件,由 setup-env.sh 稍后
# `. .user-deps/env.sh` 源进自己进程,再传给 gen-projects.py 子进程(CMake 的
# FindVulkan.cmake 自动认 ENV{VULKAN_SDK},零 CMakeLists 改动)。
VULKAN_SDK="$(cygpath -m "$VULKAN_SDK_DIR")"

# --- ⑤ SwiftShader(经池构建,见 Task 2;此处仅确保 ICD 路径) ---
SWSS_ICD="$MINE_ROOT/third_party/_install/swiftshader-master/release/vk_swiftshader_icd.json"
if [ -f "$SWSS_ICD" ]; then
  SWSS_BIN="$(dirname "$SWSS_ICD")"
  info "⑤ SwiftShader ICD: $SWSS_ICD"
else
  warn "⑤ SwiftShader ICD 未找到: $SWSS_ICD(将由 tools/build-deps.py --all 构建产出;本步先继续生成 env.sh)"
  SWSS_BIN=""
fi

# --- ⑥ Qt6:直接下载官方 MSVC 预编译(弃 aqtinstall/pip —— 独立 Python 也无 aqt;官方预编译即完整) ---
# Qt 官方在线仓库的 qtbase-...-X86_64.7z 即完整 MSVC 预编译(含 bin/qmake.exe、lib/cmake/Qt6、
# Qt6Core/Gui/Widgets/Test DLL 与 import lib),解压即用,CMake find_package(Qt6) 直接消费。
QT_VER="6.5.3"
QT_MODULE="qt6_653"
QT_MODULE_DIR="qt.qt6.653.win64_msvc2019_64"
QT_PREFIX="$USER_DEPS/qt/$QT_VER/msvc2019_64"
QT_REPO_BASE="online/qtsdkrepository/windows_x86/desktop/$QT_MODULE/$QT_MODULE_DIR"
# Qt 在线仓库镜像根(与下载源同思路,官方兜底放最后)
QT_MIRRORS=(
  "https://mirrors.tuna.tsinghua.edu.cn/qt"
  "https://mirrors.ustc.edu.cn/qtproject"
  "https://mirror.nju.edu.cn/qt"
  "https://mirrors.cloud.tencent.com/qt"
  "https://mirrors.aliyun.com/qt"
  "https://download.qt.io"
)
install_qt_msvc() {
  local qt_root="$USER_DEPS/qt" base href arch qm
  if [ -f "$QT_PREFIX/bin/qmake.exe" ] && [ -d "$QT_PREFIX/lib/cmake/Qt6" ]; then
    info "⑥ Qt6 MSVC 已存在,跳过"; return 0
  fi
  # 挑可达镜像:列出模块目录,提取 qtbase-...-X86_64.7z 文件名(不硬编码带时间戳的文件名)
  base=""; href=""
  for m in "${QT_MIRRORS[@]}"; do
    if curl -sf --max-time 10 -o "$DEB_CACHE/qt.list" "$m/$QT_REPO_BASE/" 2>/dev/null; then
      # 提取 basename:<版本>-<build>qtbase-...-X86_64.7z。文件名不含 /;
      # href 形如 "6.5.3-0-202309260341qtbase-...-X86_64.7z"(版本 1-2 段、build 为长数字)。
      # 用 [^"/] 排除路径分隔符,保证取的是 basename;排除 debug。
      href="$(grep -oE '[^"/]*qtbase-[^"]*X86_64\.7z' "$DEB_CACHE/qt.list" | grep -vi 'debug' | head -n1 || true)"
      if [ -n "$href" ]; then base="$m"; break; fi
    fi
  done
  [ -n "$base" ] && [ -n "$href" ] || die "Qt 在线仓库不可达(镜像全挂或找不到 qtbase MSVC 预编译)"
  arch="$DEB_CACHE/$href"
  info "⑥ 下载 Qt6 MSVC 预编译($QT_VER, qtbase, 约 50MB)← $base ..."
  curl -fL --retry 3 --max-time 1800 -o "$arch" "$base/$QT_REPO_BASE/$href" || die "Qt6 预编译下载失败"
  info "⑥ 解压到 $qt_root ..."
  # 官方 7z 内部自带 <ver>/<arch> 两层(验证:6.5.3/msvc2019_64/),直接解到 qt_root,
  # 使 $QT_PREFIX(=$qt_root/$QT_VER/msvc2019_64)天然指向解压根,不产生嵌套。
  rm -rf "$qt_root"; mkdir -p "$qt_root"
  "$_7z" x -y "$arch" -o"$qt_root" >/dev/null 2>&1 || { rm -f "$arch"; die "Qt6 预编译解压失败"; }
  rm -f "$arch"
  # 防御:7z 内部布局若与预期不符(无 <ver>/<arch> 层),find 反推 prefix(<prefix>/bin/qmake.exe)
  if [ ! -f "$QT_PREFIX/bin/qmake.exe" ]; then
    qm="$(find "$qt_root" -type f -name qmake.exe 2>/dev/null | head -n1 || true)"
    [ -n "$qm" ] || die "Qt 解压后未找到 qmake.exe"
    QT_PREFIX="$(dirname "$(dirname "$qm")")"
  fi
  [ -d "$QT_PREFIX/lib/cmake/Qt6" ] || die "Qt 预编译缺 lib/cmake/Qt6(CMake 无法 find_package(Qt6))"
  # 写 bin/qt.conf 使其可重定位(对齐 aqtinstall 的 make_qtconf):
  # qmake 等工具凭此找到 prefix;CMake 的 Qt6Config.cmake 自身可重定位不需要,但便宜且防坑。
  cat > "$QT_PREFIX/bin/qt.conf" <<EOF
[Paths]
Prefix=..
EOF
  info "⑥ Qt6 MSVC 就绪: $QT_PREFIX"
}
install_qt_msvc

# --- ⑦ 生成 env.sh(工具链 + Qt6 + SwiftShader ICD + Vulkan SDK;不含 /mingw64) ---
# PATH 前置:工具链(.user-deps/bin 及独立 python 目录/VS 自带 cmake&ninja)→ Qt6 bin → Vulkan Bin。
# \$PATH 转义成字面量,source 时才展开,避免写成生成时刻的 PATH。
# VULKAN_SDK:CMake FindVulkan.cmake 自动认 ENV{VULKAN_SDK},find_package(Vulkan) 靠它。
TOOLCHAIN_PATH="$TOOL_BIN"
[ -n "$PY_DIR" ] && TOOLCHAIN_PATH="$TOOLCHAIN_PATH:$PY_DIR"
[ -n "$VS_CMAKE_BIN" ] && TOOLCHAIN_PATH="$TOOLCHAIN_PATH:$VS_CMAKE_BIN"
[ -n "$VS_NINJA_BIN" ] && TOOLCHAIN_PATH="$TOOLCHAIN_PATH:$VS_NINJA_BIN"
cat > "$USER_DEPS/env.sh" <<EOF
export MINE_ROOT="$MINE_ROOT"
export USER_DEPS="$USER_DEPS"
export VK_ICD_FILENAMES="$SWSS_ICD"
export VK_DRIVER_FILES="$SWSS_ICD"
export VULKAN_SDK="$VULKAN_SDK"
export CMAKE_PREFIX_PATH="$QT_PREFIX:$MINE_ROOT/third_party/_install/glfw-3.4/release"
export PATH="$TOOLCHAIN_PATH:$QT_PREFIX/bin:$VULKAN_SDK/Bin:\$PATH"
EOF
info "⑦ 已生成 $USER_DEPS/env.sh"

# --- ⑧ 离屏 Vulkan 探针(SwiftShader 能创建 device) ---
"$GLSLC" -fshader-stage=fragment -o "$DEB_CACHE/probe.frag.spv" - <<'EOF' || die "glslc 编译失败"
#version 450
layout(location=0) out vec4 outColor;
void main(){ outColor = vec4(1.0,0.0,0.0,1.0); }
EOF
info "⑧ glslc 探针通过"
info "完成。使用前: source $USER_DEPS/env.sh"
