#!/usr/bin/env bash
# Windows(MSYS2+MSVC)依赖部署:基础工具链 + Vulkan(pacman)+ SwiftShader(池构建) + Qt6(aqtinstall/MSVC 预编译)。
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

# MSVC 工具链发现/自动安装 + vcvars 导出(Task 1)
# shellcheck disable=SC1091
. "$MINE_ROOT/tools/deps_lib/msvc.sh"

# MSVC 工具链(替代 MinGW g++):定位/自动安装 Build Tools,并写 vcvars.sh。
msvc_ensure
MSVC_ROOT="$(msvc_locate)" || die "MSVC 定位失败"
msvc_write_vcvars_sh "$MSVC_ROOT"

# --- ① MSYS2 国内镜像源(加速 pacman;首个 pacman 前必须完成) ---
# 镜像列表(均经实测 200 可达)。测速挑最快,写入 mirrorlist.mingw64 与 mirrorlist.msys。
# 海外用户若全部不可达则保留官方默认,不影响使用。
CN_MIRRORS=(
  "https://mirrors.tuna.tsinghua.edu.cn/msys2"
  "https://mirrors.ustc.edu.cn/msys2"
  "https://mirrors.cloud.tencent.com/msys2"
  "https://mirror.nju.edu.cn/msys2"
  "https://mirrors.aliyun.com/msys2"
)
setup_mirrors() {
  local -a good=()
  local m t
  # 测速:请求各镜像 mingw64.db 头部,取响应时间
  for m in "${CN_MIRRORS[@]}"; do
    t="$(curl -so /dev/null -w '%{time_total}' --max-time 8 "$m/mingw/mingw64/mingw64.db" 2>/dev/null || echo "999")"
    t="${t:-999}"
    # 用时间排序,跳过不可达
    if [ "$t" != "999" ] && [ -n "$t" ]; then
      good+=("$t $m")
    fi
  done
  if [ "${#good[@]}" -eq 0 ]; then
    warn "① 国内镜像均不可达,保留官方镜像源"
    return 0
  fi
  # 按时间升序排序
  mapfile -t sorted < <(printf '%s\n' "${good[@]}" | sort -n)
  # 写 mirrorlist(mingw64 与 msys),最快的放最前(Server 首行优先)
  # win-deps.sh 只在 MSYS2 里运行,/usr/bin 即 /etc/pacman.d 所在。
  for f in /etc/pacman.d/mirrorlist.mingw64 /etc/pacman.d/mirrorlist.msys; do
    : > "$f"
    for entry in "${sorted[@]}"; do
      m="${entry#* }"
      if [ "$f" = "/etc/pacman.d/mirrorlist.msys" ]; then
        printf 'Server = %s/msys/$arch\n' "$m" >> "$f"
      else
        printf 'Server = %s/mingw/mingw64\n' "$m" >> "$f"
      fi
    done
  done
  info "① 已写入国内 MSYS2 镜像源(最快在前): ${sorted[0]#* }"
}
setup_mirrors

# pacman 首次同步:刷新数据库与 keyring(新装 MSYS2 必需,幂等)
pacman_sync() {
  pacman -Sy --noconfirm || {
    warn "pacman -Sy 失败,尝试初始化 keyring..."
    pacman-key --init 2>/dev/null || true
    pacman-key --populate msys2 2>/dev/null || true
    pacman -Sy --noconfirm || die "pacman 数据库同步失败(检查镜像源/网络)"
  }
}
pacman_sync

# --- ② 基础工具链(不再装 MinGW g++,只保证 ninja/cmake/git/python3/glslc) ---
MISS_TOOLS=()
has ninja    || MISS_TOOLS+=(mingw-w64-x86_64-ninja)
has cmake    || MISS_TOOLS+=(mingw-w64-x86_64-cmake)
has git      || MISS_TOOLS+=(git)
has python3  || MISS_TOOLS+=(mingw-w64-x86_64-python)
has python3 && ! python3 -c 'import yaml' >/dev/null 2>&1 && MISS_TOOLS+=(mingw-w64-x86_64-python-yaml)
if [ "${#MISS_TOOLS[@]}" -gt 0 ]; then
  info "② pacman 安装基础工具链: ${MISS_TOOLS[*]}"
  pacman -S --needed --noconfirm "${MISS_TOOLS[@]}"
else
  info "② 基础工具链齐全"
fi

# --- ③ windows_package:MSVC 下不再用 pacman 预编译三方库(pool.is_pacman_provided 恒 False)→ 删除 ---

# --- ④ Vulkan(glslc 用 MSYS2 pacman 包;find_package(Vulkan) 用 MSVC 兼容的 LunarG SDK) ---
# 注:不用 Lunarg SDK zip 装 glslc/vulkan.h —— 其 windows/vulkan-sdk-latest.zip 实际重定向为
# .exe 安装包,unzip 会报 "plain executable, not an archive"。MSYS2 的
# mingw-w64-x86_64-vulkan-headers/shaderc/vulkan-loader 恰好提供 vulkan.h + glslc.exe +
# libvulkan,且已在 PATH,足够 shader 编译用。
PAC_VK="mingw-w64-x86_64-vulkan-headers mingw-w64-x86_64-shaderc mingw-w64-x86_64-vulkan-loader"
info "④ pacman 安装 Vulkan 依赖: $PAC_VK"
pacman -S --needed --noconfirm $PAC_VK

# 定位 glslc.exe 与 vulkan.h(均在 /mingw64,已在 PATH,此处仅校验存在性)
GLSLC="$(command -v glslc.exe || true)"
[ -n "$GLSLC" ] || die "未找到 glslc.exe(确认已装 mingw-w64-x86_64-shaderc)"
GLSLC="$(cygpath -u "$GLSLC" 2>/dev/null || printf '%s' "$GLSLC")"
VULKAN_INC="$(dirname "$(dirname "$GLSLC")")/include/vulkan"
[ -f "$VULKAN_INC/vulkan.h" ] || die "未找到 vulkan.h(确认已装 mingw-w64-x86_64-vulkan-headers)"
info "④ glslc: $GLSLC;vulkan.h: $VULKAN_INC/vulkan.h"

# CMake 的 find_package(Vulkan)(EasyPainter/CMakeLists.txt 需要)在 MSVC 下无法用上面的
# MSYS2 包 —— libvulkan 是 MinGW 格式 .dll.a,link.exe 认不了;/mingw64 也不在
# CMAKE_PREFIX_PATH 里(MSVC 迁移时特意去掉,见 docs/superpowers/specs/2026-08-25-msvc-design.md)。
# 需要一份真正的 MSVC 兼容 Vulkan SDK(vulkan-1.lib + vulkan/vulkan.h)。装到 .user-deps 下
# 自包含(不碰系统 C:\VulkanSDK),VULKAN_SDK 写进 env.sh —— CMake FindVulkan.cmake 自动认
# ENV{VULKAN_SDK},零 CMakeLists 改动。
VULKAN_SDK_DIR="$USER_DEPS/vulkan-sdk"
if [ -f "$VULKAN_SDK_DIR/Include/vulkan/vulkan.h" ] && [ -f "$VULKAN_SDK_DIR/Lib/vulkan-1.lib" ]; then
  info "④ Vulkan SDK(MSVC 兼容)已存在: $VULKAN_SDK_DIR"
else
  info "④ 下载 Vulkan SDK(MSVC 兼容,LunarG,约几百 MB)..."
  VK_SDK_EXE="$DEB_CACHE/vulkan_sdk.exe"
  curl -fL --retry 3 -o "$VK_SDK_EXE" \
    "https://sdk.lunarg.com/sdk/download/latest/windows/vulkan_sdk.exe" \
    || die "Vulkan SDK 下载失败(需能出网)"
  info "④ 静默安装 Vulkan SDK 到 $VULKAN_SDK_DIR ..."
  # --root 指到 .user-deps 下的自定义目录(免管理员,不碰系统 C:\VulkanSDK);
  # --accept-licenses --default-answer --confirm-command install 是 LunarG 文档给出的
  # 全非交互静默安装组合(Qt Installer Framework 底层)。
  "$VK_SDK_EXE" --root "$(cygpath -m "$VULKAN_SDK_DIR")" \
    --accept-licenses --default-answer --confirm-command install \
    || { rm -f "$VK_SDK_EXE"; die "Vulkan SDK 安装失败(可手动重跑安装器)"; }
  rm -f "$VK_SDK_EXE"
  [ -f "$VULKAN_SDK_DIR/Include/vulkan/vulkan.h" ] && [ -f "$VULKAN_SDK_DIR/Lib/vulkan-1.lib" ] \
    || die "Vulkan SDK 安装完成但未找到 Include/vulkan/vulkan.h 或 Lib/vulkan-1.lib(安装可能不完整)"
fi
# 装完的 VULKAN_SDK 是系统环境变量,当前已在跑的 shell(含本脚本进程)看不到,
# 且 setup-env.sh 用 `bash install-user-deps.sh` 子进程调用本脚本(非 source),
# 这里 export 出去也过不了进程边界 —— 唯一可靠路径是写进 env.sh,由 setup-env.sh
# 稍后 `. .user-deps/env.sh` 源进自己进程,再传给 gen-projects.py 子进程。
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

# --- ⑥ Qt6:直接下载官方 MSVC 预编译(弃 aqtinstall/pip —— MSYS2 python 无 pip) ---
# Qt 官方在线仓库的 qtbase-...-X86_64.7z 即完整 MSVC 预编译(含 bin/qmake.exe、lib/cmake/Qt6、
# Qt6Core/Gui/Widgets/Test DLL 与 import lib),解压即用,CMake find_package(Qt6) 直接消费。
QT_VER="6.5.3"
QT_MODULE="qt6_653"
QT_MODULE_DIR="qt.qt6.653.win64_msvc2019_64"
QT_PREFIX="$USER_DEPS/qt/$QT_VER/msvc2019_64"
QT_REPO_BASE="online/qtsdkrepository/windows_x86/desktop/$QT_MODULE/$QT_MODULE_DIR"
# Qt 在线仓库镜像根(与 ① MSYS2 镜像同思路,官方兜底放最后)
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
  has 7z || { info "⑥ 安装 p7zip(解压 Qt 预编译)"; pacman -S --needed --noconfirm p7zip; }
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
  curl -fL --retry 3 -o "$arch" "$base/$QT_REPO_BASE/$href" || die "Qt6 预编译下载失败"
  info "⑥ 解压到 $qt_root ..."
  # 官方 7z 内部自带 <ver>/<arch> 两层(验证:6.5.3/msvc2019_64/),直接解到 qt_root,
  # 使 $QT_PREFIX(=$qt_root/$QT_VER/msvc2019_64)天然指向解压根,不产生嵌套。
  rm -rf "$qt_root"; mkdir -p "$qt_root"
  7z x -y "$arch" -o"$qt_root" >/dev/null 2>&1 || { rm -f "$arch"; die "Qt6 预编译解压失败"; }
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

# --- ⑦ 生成 env.sh(MSVC 前缀 + Qt6 前缀 + SwiftShader ICD + Vulkan SDK;不含 /mingw64) ---
# PATH 前置 $QT_PREFIX/bin:Qt6 运行期 DLL(Qt6Core/Gui/Widgets/Test)需在此可被找到。
# \$PATH 转义成字面量,source 时才展开,避免写成生成时刻的 PATH。
# VULKAN_SDK:CMake FindVulkan.cmake 自动认 ENV{VULKAN_SDK},find_package(Vulkan) 靠它。
cat > "$USER_DEPS/env.sh" <<EOF
export MINE_ROOT="$MINE_ROOT"
export USER_DEPS="$USER_DEPS"
export VK_ICD_FILENAMES="$SWSS_ICD"
export VK_DRIVER_FILES="$SWSS_ICD"
export VULKAN_SDK="$VULKAN_SDK"
export CMAKE_PREFIX_PATH="$QT_PREFIX:$MINE_ROOT/third_party/_install/glfw-3.4/release"
export PATH="$QT_PREFIX/bin:\$PATH"
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
