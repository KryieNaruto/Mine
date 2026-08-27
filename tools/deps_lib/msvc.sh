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

# 磁盘扫描的 VS 安装根(可被测试覆盖)。
# 背景:用户已装完整 VS(如 2026,带 C++ 桌面开发即含 MSVC),但 vswhere 可能
# (版本旧 / 实例未被注册 / 预览版)枚举不到 → 只靠 vswhere 会误判"未装"并错装 Build Tools。
# 扫两个默认根(64 位 + 32 位),两者都可能是 VS 安装位置。
MSVC_DISK_BASES=("/c/Program Files/Microsoft Visual Studio" "/c/Program Files (x86)/Microsoft Visual Studio")

# msvc_disk_roots : 输出磁盘上所有含 VC 工具链的 VS 安装根(POSIX),新在前。
# 判据用 *VC/Tools/MSVC*(cl 工具集真实所在),不用 vcvars64.bat:
# VS2026/Insiders/预览版布局可能更复杂,vcvars64.bat 路径深度/存在性不稳,
# 而 VC/Tools/MSVC 存在即证明装了「使用 C++ 的桌面开发」工作负载。find 全扫(不限深),
# 取含 VC/Tools/MSVC 的目录向上裁剪出 VS 安装根;sort -r 让 2026 > 2022 新版优先。
msvc_disk_roots() {
  local base toolset
  for base in "${MSVC_DISK_BASES[@]}"; do
    [ -d "$base" ] || continue
    while IFS= read -r toolset; do
      [ -d "$toolset" ] || continue
      printf '%s\n' "${toolset%/VC/Tools/MSVC}"
    done < <(find "$base" -type d -path '*VC/Tools/MSVC' 2>/dev/null | sort -r)
  done
}

# msvc_has_toolset <root> : root 是否含 C++ 工具集(存在 VC/Tools/MSVC 目录)。
# 这是「装了使用 C++ 的桌面开发 / VCTools」的可靠判据,对齐 paint-pc find_vs。
msvc_has_toolset() {
  [ -d "$1/VC/Tools/MSVC" ]
}

# msvc_is_vs_root <root> : root 是否可用作 VS 根(有工具集目录 或 有 vcvars64.bat)。
msvc_is_vs_root() {
  msvc_has_toolset "$1" || [ -x "$(msvc_vcvars_path "$1")" ]
}

# msvc_find_vcvars <root> : 输出 vcvars64.bat 的 POSIX 路径;找不到输出空。
# 先看 root 下常规位置,再全盘兜底(兼容 Insiders/自定义布局下 vcvars 路径差异)。
# 对齐 paint-pc find_vcvars。
msvc_find_vcvars() {
  local root="${1:-}" base vc
  if [ -n "$root" ] && [ -x "$root/VC/Auxiliary/Build/vcvars64.bat" ]; then
    printf '%s' "$root/VC/Auxiliary/Build/vcvars64.bat"; return 0
  fi
  for base in "${MSVC_DISK_BASES[@]}"; do
    [ -d "$base" ] || continue
    vc="$(find "$base" -type f -name vcvars64.bat -path '*/VC/Auxiliary/Build/vcvars64.bat' 2>/dev/null | head -n1)"
    [ -n "$vc" ] && { printf '%s' "$vc"; return 0; }
  done
  return 1
}

# msvc_locate : 输出含 VC 工具链的 VS/Build Tools 安装根(POSIX 路径);找不到返回 1。
# 定位顺序:VSINSTALLDIR → 磁盘扫描 VC/Tools/MSVC(新版优先)→ vswhere(-all 两连击)。
# 为什么磁盘扫描优先于 vswhere:paint-pc 上 vswhere 只报被误装的 2022 BuildTools,
# 看不见用户的 VS2026(18/Insiders);若先走 vswhere 会抢在真实 VS 前返回 2022。
# 磁盘扫描按 sort -r 新版优先,能选到 18/Insiders(> 2022)。vswhere 仅作补漏。
msvc_locate() {
  # ${VSINSTALLDIR:-} 而非 $VSINSTALLDIR:set -u 下未设置即展开会直接崩,须给默认空。
  # 关键:捕获也要用 ${VSINSTALLDIR:-} —— VSINSTALLDIR 根本未设置时,local vs="$VSINSTALLDIR"
  # 照样展开崩,一样中招。
  local vs="${VSINSTALLDIR:-}" root vswhere diskroot
  if [ -n "$vs" ]; then
    vs="$(msvc_to_posix "$vs")"
    if msvc_is_vs_root "$vs"; then printf '%s' "$vs"; return 0; fi
  fi
  # 磁盘扫描:VC/Tools/MSVC 存在即命中;base 内 sort -r 新版优先(18 > 2022),64 位根在前。
  while IFS= read -r diskroot; do
    [ -n "$diskroot" ] || continue
    if msvc_is_vs_root "$diskroot"; then printf '%s' "$diskroot"; return 0; fi
  done < <(msvc_disk_roots)
  vswhere="$(msvc_resolve_vswhere)"
  if [ -n "$vswhere" ]; then
    # 关键:-all 必须有 —— 否则 vswhere 默认隐藏 Insiders/预览版(用户 VS 装在
    # Microsoft Visual Studio\18\Insiders 就是这种),会查不到。加 -all 包含所有版本。
    # 顺序:带 -requires 精确匹配 → 降级不带 -requires(组件变体/预览版组件 id 不同)。
    root="$("$vswhere" -all -latest -products '*' \
      -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 \
      -property installationPath 2>/dev/null | tr -d '\r' | head -n1 || true)"
    [ -z "$root" ] && root="$("$vswhere" -all -latest -products '*' \
      -property installationPath 2>/dev/null | tr -d '\r' | head -n1 || true)"
    if [ -n "$root" ]; then
      # vswhere 输出反斜杠 Windows 路径 → 先转 POSIX,再判可用。
      # 顺序不能反:MSYS2 里 [ -x "C:\..." ] 会把反斜杠当普通字符,按相对路径判 → 恒失败。
      root="$(msvc_to_posix "$root")"
      if msvc_is_vs_root "$root"; then printf '%s' "$root"; return 0; fi
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
# vcvars 经 msvc_find_vcvars 定位(含全盘兜底),不写死 root 下的猜测路径。
msvc_write_vcvars_sh() {
  local root="$1" out="${2:-}" vcvars
  [ -n "$out" ] || out="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)/.user-deps/vcvars.sh"
  vcvars="$(msvc_find_vcvars "$root")"
  if [ -z "$vcvars" ]; then
    err "定位到 MSVC(root=$root)但找不到 vcvars64.bat"
    return 1
  fi
  mkdir -p "$(dirname "$out")"
  cat > "$out" <<EOF
# MSVC 环境(由 deps_lib/msvc.sh 生成)。构建前 source 以拿到 vcvars64 环境。
export VS_INSTALL_ROOT="$root"
export VC_VARS_BAT="$vcvars"
# 用法:cmd //c "\"\$VC_VARS_BAT\" && set" 可导出完整环境;cmake 会自动找到 cl。
# (MSYS bash 里 /c 会被其运行时转成 C:\ 路径,须写 //c 防转换;python 侧见 build-deps.py _msys_linked)
EOF
  info "已生成 $out"
}
