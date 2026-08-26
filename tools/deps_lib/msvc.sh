#!/usr/bin/env bash
# MSVC 工具链发现 + 自动安装(Build Tools)+ vcvars 导出。由 win-deps.sh source。
# 不单独执行。
set -euo pipefail

# 辅助函数:若宿主脚本已定义则复用其版本(避免 source 后覆盖宿主格式,如 setup-env 用无前缀 info/err)。
if ! declare -F info >/dev/null 2>&1; then info() { printf '[INFO] %s\n' "$*"; }; fi
if ! declare -F err  >/dev/null 2>&1; then err()  { printf '[ERROR] %s\n' "$*" >&2; }; fi
if ! declare -F die  >/dev/null 2>&1; then die()  { err "$*"; exit 1; }; fi
if ! declare -F has  >/dev/null 2>&1; then has()  { command -v "$1" >/dev/null 2>&1; }; fi

# vswhere 默认路径(32 位安装的 x86 程序,安装到 64 位系统)
VSWHERE="/c/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe"
[ -x "$VSWHERE" ] || VSWHERE="$(command -v vswhere.exe 2>/dev/null || true)"

# msvc_vcvars_path <root> : 输出 vcvars64.bat 的 POSIX 路径。
msvc_vcvars_path() {
  printf '%s/VC/Auxiliary/Build/vcvars64.bat' "$1"
}

# msvc_locate : 输出含 VC 工具链 + Windows SDK 的 VS/Build Tools 安装根;找不到返回 1。
msvc_locate() {
  # ${VSINSTALLDIR:-} 而非 $VSINSTALLDIR:set -u 下未设置即展开会直接崩,须给默认空
  if [ -n "${VSINSTALLDIR:-}" ] && [ -x "$(msvc_vcvars_path "$VSINSTALLDIR")" ]; then
    printf '%s' "$VSINSTALLDIR"; return 0
  fi
  if [ -n "$VSWHERE" ]; then
    local root
    root="$("$VSWHERE" -latest -products '*' -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 -requires Microsoft.VisualStudio.Component.Windows10SDK -property installationPath 2>/dev/null | tr -d '\r' || true)"
    if [ -n "$root" ] && [ -x "$(msvc_vcvars_path "$root")" ]; then
      # vswhere 输出 Windows 路径,转 POSIX
      if has cygpath; then root="$(cygpath -u "$root" 2>/dev/null || printf '%s' "$root")"; fi
      printf '%s' "$root"; return 0
    fi
  fi
  return 1
}

# msvc_install_buildtools : 下载 vs_buildtools.exe 并静默装 Build Tools(免管理员,用户级)。
# 组件:VC 工具链 + Windows SDK + Ninja。
msvc_install_buildtools() {
  local dl="$1/buildtools-installer.exe"
  info "下载 VS Build Tools 安装器到 $dl ..."
  curl -fL --retry 3 -o "$dl" \
    "https://aka.ms/vs/17/release/vs_buildtools.exe" || die "下载 vs_buildtools.exe 失败(需能出网)"
  info "静默安装 Build Tools(约 2GB,含 VC + WinSDK + Ninja)..."
  "$dl" --quiet --norestart --nocache \
    --add Microsoft.VisualStudio.Workload.VCTools --includeRecommended \
    --wait --force || { rm -f "$dl"; die "Build Tools 安装失败(可手动重跑安装器)"; }
  rm -f "$dl"
}

# msvc_ensure : 定位 MSVC 安装根;无则自动装。返回安装根。
msvc_ensure() {
  local root
  if root="$(msvc_locate)"; then
    info "MSVC 已安装: $root"
    printf '%s' "$root"; return 0
  fi
  info "未检测到 MSVC(Build Tools)。将自动安装 ..."
  msvc_install_buildtools "$(dirname "${BASH_SOURCE[0]}")/../../.user-deps" || die "Build Tools 自动安装失败"
  # 安装后重新定位
  if root="$(msvc_locate)"; then
    info "MSVC 安装完成: $root"
    printf '%s' "$root"; return 0
  fi
  die "Build Tools 已安装但定位失败。请手动运行 vs_buildtools.exe 或设置 VSINSTALLDIR 指向其安装根。"
}

# msvc_write_vcvars_sh <root> : 写 .user-deps/vcvars.sh(记录 vcvars64.bat 路径)。
msvc_write_vcvars_sh() {
  local root="$1" out="${2:-}"
  [ -n "$out" ] || out="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)/.user-deps/vcvars.sh"
  mkdir -p "$(dirname "$out")"
  cat > "$out" <<EOF
# MSVC 环境(由 deps_lib/msvc.sh 生成)。构建前 source 以拿到 vcvars64 环境。
export VS_INSTALL_ROOT="$root"
export VC_VARS_BAT="$(msvc_vcvars_path "$root")"
# 用法:cmd /c "\"\$VC_VARS_BAT\" && set" 可导出完整环境;cmake 会自动找到 cl。
EOF
  info "已生成 $out"
}
