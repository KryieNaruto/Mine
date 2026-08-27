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
    curl -fL --retry 3 -o "$VK_SDK_EXE" \
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
# PATH 前置 $QT_PREFIX/bin(Qt6 运行期 DLL 需要)与 $VULKAN_SDK/Bin(glslc.exe——
# EasyPainter/CMakeLists.txt 的 shader 自定义命令用裸命令 `glslc`,构建期靠 PATH 找到)。
# \$PATH 转义成字面量,source 时才展开,避免写成生成时刻的 PATH。
# VULKAN_SDK:CMake FindVulkan.cmake 自动认 ENV{VULKAN_SDK},find_package(Vulkan) 靠它。
cat > "$USER_DEPS/env.sh" <<EOF
export MINE_ROOT="$MINE_ROOT"
export USER_DEPS="$USER_DEPS"
export VK_ICD_FILENAMES="$SWSS_ICD"
export VK_DRIVER_FILES="$SWSS_ICD"
export VULKAN_SDK="$VULKAN_SDK"
export CMAKE_PREFIX_PATH="$QT_PREFIX:$MINE_ROOT/third_party/_install/glfw-3.4/release"
export PATH="$QT_PREFIX/bin:$VULKAN_SDK/Bin:\$PATH"
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
