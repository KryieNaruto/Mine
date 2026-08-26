#!/usr/bin/env bash
# MSVC 工具链发现 + 自动安装(Build Tools)+ vcvars 导出。由 win-deps.sh source。
# 不单独执行。
set -euo pipefail

# 辅助函数:若宿主脚本已定义则复用其版本(避免 source 后覆盖宿主格式,如 setup-env 用无前缀 info/err)。
if ! declare -F info >/dev/null 2>&1; then info() { printf '[INFO] %s\n' "$*"; }; fi
if ! declare -F err  >/dev/null 2>&1; then err()  { printf '[ERROR] %s\n' "$*" >&2; }; fi
if ! declare -F die  >/dev/null 2>&1; then die()  { err "$*"; exit 1; }; fi
if ! declare -F has  >/dev/null 2>&1; then has()  { command -v "$1" >/dev/null 2>&1; }; fi

# msvc_vcvars_path <root> : 输出 vcvars64.bat 的 POSIX 路径。
msvc_vcvars_path() {
  printf '%s/VC/Auxiliary/Build/vcvars64.bat' "$1"
}

# msvc_resolve_vswhere : 输出 vswhere.exe 路径;找不到输出空。
# vswhere.exe 随 VS Installer 一起装 —— 干净机器首次自动装 Build Tools 前并不存在,
# 因此每次调用现找,绝不能在 source 时缓存(否则自动装完再定位仍拿空路径 → 定位失败)。
msvc_resolve_vswhere() {
  # 默认路径:32 位安装的 x86 程序,安装到 64 位系统
  local p="/c/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe"
  if [ -x "$p" ]; then printf '%s' "$p"; return 0; fi
  command -v vswhere.exe 2>/dev/null || true
}

# msvc_to_posix <path> : Windows 路径(反斜杠,如 C:\...)转 POSIX(/c/...);已有 POSIX 则原样。
msvc_to_posix() {
  if has cygpath; then cygpath -u "$1" 2>/dev/null || printf '%s' "$1"
  else printf '%s' "$1"; fi
}

# 磁盘扫描的 VS 安装根(可被测试覆盖)。直接找 vcvars64.bat 真实文件,不依赖 vswhere。
# 背景:用户已装完整 VS(如 2026,带 C++ 桌面开发即含 MSVC),但 vswhere 可能
# (版本旧 / 实例未被注册)枚举不到新实例 → 只靠 vswhere 会误判"未装"并错装 Build Tools。
MSVC_DISK_BASES=("/c/Program Files/Microsoft Visual Studio" "/c/Program Files (x86)/Microsoft Visual Studio")

# msvc_disk_roots : 输出磁盘上所有含 VC 工具链(存在 vcvars64.bat)的 VS 安装根,新在前。
msvc_disk_roots() {
  local base vcvars
  for base in "${MSVC_DISK_BASES[@]}"; do
    [ -d "$base" ] || continue
    while IFS= read -r vcvars; do
      [ -x "$vcvars" ] || continue
      printf '%s\n' "${vcvars%/VC/Auxiliary/Build/vcvars64.bat}"
    done < <(find "$base" -type f -name vcvars64.bat \
        -path '*/VC/Auxiliary/Build/vcvars64.bat' 2>/dev/null | sort -r)
  done
}

# msvc_locate : 输出含 VC 工具链的 VS/Build Tools 安装根(POSIX 路径);找不到返回 1。
# 三层:VSINSTALLDIR → vswhere → 磁盘扫描兜底。只要求 VC 工具链组件,
# 不强求 Windows SDK:SDK 组件 id 随系统而异(Win10=Windows10SDK、Win11=Windows11SDK.<ver>),
# 硬编码任一都会漏掉另一半;是否可用由 vcvars64.bat 存在性 + 构建期探测兜底。
msvc_locate() {
  # ${VSINSTALLDIR:-} 而非 $VSINSTALLDIR:set -u 下未设置即展开会直接崩,须给默认空。
  # 关键:捕获也要用 ${VSINSTALLDIR:-} —— VSINSTALLDIR 根本未设置时,local vs="$VSINSTALLDIR"
  # 照样展开崩,一样中招。
  local vs="${VSINSTALLDIR:-}" root vcvars
  if [ -n "$vs" ]; then
    vs="$(msvc_to_posix "$vs")"
    vcvars="$(msvc_vcvars_path "$vs")"
    if [ -x "$vcvars" ]; then
      printf '%s' "$vs"; return 0
    fi
  fi
  local vswhere
  vswhere="$(msvc_resolve_vswhere)"
  if [ -n "$vswhere" ]; then
    # vswhere 输出反斜杠 Windows 路径 → 先转 POSIX,再查 vcvars 存在性。
    # 顺序不能反:MSYS2 里 [ -x "C:\..." ] 会把反斜杠当普通字符,按相对路径判 → 恒失败。
    root="$("$vswhere" -latest -products '*' \
      -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 \
      -property installationPath 2>/dev/null | tr -d '\r' || true)"
    if [ -n "$root" ]; then
      root="$(msvc_to_posix "$root")"
      vcvars="$(msvc_vcvars_path "$root")"
      if [ -x "$vcvars" ]; then
        printf '%s' "$root"; return 0
      fi
    fi
  fi
  # 兜底:vswhere 漏报新 VS 实例时,直接扫磁盘 vcvars64.bat(sort -r → 新版在前,如 2026 > 2022)
  local diskroot
  while IFS= read -r diskroot; do
    [ -n "$diskroot" ] || continue
    if [ -x "$(msvc_vcvars_path "$diskroot")" ]; then
      printf '%s' "$diskroot"; return 0
    fi
  done < <(msvc_disk_roots)
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
