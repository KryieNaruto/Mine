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

# --- ④ Vulkan SDK(头 + glslc + loader,均用 MSYS2 原生 pacman 包) ---
# 注:不用 Lunarg SDK zip —— 其 windows/vulkan-sdk-latest.zip 实际重定向为 .exe 安装包,
# unzip 会报 "plain executable, not an archive"。MSYS2 的 mingw-w64-x86_64-vulkan-headers/
# shaderc/vulkan-loader 恰好提供 vulkan.h + glslc.exe + libvulkan,且已在 PATH。
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

# --- ⑤ SwiftShader(经池构建,见 Task 2;此处仅确保 ICD 路径) ---
SWSS_ICD="$MINE_ROOT/third_party/_install/swiftshader-master/release/vk_swiftshader_icd.json"
if [ -f "$SWSS_ICD" ]; then
  SWSS_BIN="$(dirname "$SWSS_ICD")"
  info "⑤ SwiftShader ICD: $SWSS_ICD"
else
  warn "⑤ SwiftShader ICD 未找到: $SWSS_ICD(将由 tools/build-deps.py --all 构建产出;本步先继续生成 env.sh)"
  SWSS_BIN=""
fi

# --- ⑥ Qt6:改走 aqtinstall(MSVC 预编译) ---
# aqtinstall --outputdir 落盘在 <outputdir>/<qt_version>/<target_arch>/(installer.py:1652)
QT_PREFIX="$USER_DEPS/qt/6.5.3/msvc2019_64"
install_qt_msvc() {
  local qt_root="$USER_DEPS/qt"
  if [ -f "$QT_PREFIX/bin/qmake.exe" ]; then
    info "⑥ Qt6 MSVC 已存在,跳过"; return 0
  fi
  has python3 || die "python3 缺失"
  python3 -m pip install --quiet aqtinstall || die "aqtinstall 安装失败"
  mkdir -p "$qt_root"
  info "⑥ aqtinstall 下载 Qt6 MSVC 预编译(约 1GB)..."
  python3 -m aqt install-qt windows desktop 6.5.3 win64_msvc2019_64 \
    --outputdir "$qt_root" || die "Qt6 下载失败"
}
install_qt_msvc

# --- ⑦ 生成 env.sh(MSVC 前缀 + Qt6 前缀 + SwiftShader ICD;不含 /mingw64) ---
cat > "$USER_DEPS/env.sh" <<EOF
export MINE_ROOT="$MINE_ROOT"
export USER_DEPS="$USER_DEPS"
export VK_ICD_FILENAMES="$SWSS_ICD"
export VK_DRIVER_FILES="$SWSS_ICD"
export CMAKE_PREFIX_PATH="$QT_PREFIX:$MINE_ROOT/third_party/_install/glfw-3.4/release"
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
