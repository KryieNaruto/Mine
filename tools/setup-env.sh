#!/usr/bin/env bash
# 系统工具链检测:cmake / ninja / g++ / pkg-config / git / python3 + 用户级系统依赖部署状态。
# 无 sudo 场景:Vulkan/X11/lavapipe/Xvfb 由 tools/install-user-deps.sh 用户级部署到 .user-deps/。
set -euo pipefail

# 平台判定:MSYS*/MINGW* → Windows;否则 Linux
case "$(uname -s)" in
  MSYS*|MINGW*) OS_PLATFORM="windows" ;;
  *)            OS_PLATFORM="linux" ;;
esac

info() { printf '%s\n' "$*"; }
warn() { printf 'WARN: %s\n' "$*" >&2; }
err()  { printf 'ERROR: %s\n' "$*" >&2; }
has()  { command -v "$1" >/dev/null 2>&1; }

MINE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
USER_DEPS_ENV="$MINE_ROOT/.user-deps/env.sh"

extract_version() {
  local s="$1"
  if [[ "$s" =~ ([0-9]+(\.[0-9]+)+) ]]; then printf '%s' "${BASH_REMATCH[1]}"
  elif [[ "$s" =~ ([0-9]+) ]]; then printf '%s' "${BASH_REMATCH[1]}"; fi
}
ver_seg() { local v="$1" i="$2" s; s="$(printf '%s' "$v" | cut -d. -f"$((i+1))" 2>/dev/null || true)"; case "$s" in ''|*[!0-9]*) printf 0;; *) printf '%d' "$((10#$s))";; esac; }
ver_ge() { local a="$1" b="$2" i sa sb; for i in 0 1 2 3; do sa="$(ver_seg "$a" "$i")"; sb="$(ver_seg "$b" "$i")"; [ "$sa" -gt "$sb" ] && return 0; [ "$sa" -lt "$sb" ] && return 1; done; return 0; }

HARD_MISS=0
MISS_DETAILS=()

chk() { # name 最低版本 探测命令 详情
  local name="$1" min="$2" v vn ok=1
  if ! has "$(printf '%s' "$3" | awk '{print $1}')"; then ok=0; fi
  if [ "$ok" = 1 ]; then
    v="$($3 2>/dev/null | head -n1 || true)"
    vn="$(extract_version "$v")"
    if [ -n "$min" ] && [ -n "$vn" ] && ! ver_ge "$vn" "$min"; then ok=0; fi
  fi
  if [ "$ok" = 0 ]; then HARD_MISS=$((HARD_MISS+1)); MISS_DETAILS+=("$name(需 ${min:-任意})"); fi
  if [ "$ok" = 1 ]; then printf '[OK]   %s: %s\n' "$name" "${v:-已安装}"; else printf '[MISS] %s: 缺失或版本过低\n' "$name"; fi
}

chk_user_deps() {
  if [ -f "$USER_DEPS_ENV" ]; then
    printf '[OK]   user-deps: %s\n' "$USER_DEPS_ENV"
  else
    HARD_MISS=$((HARD_MISS+1)); MISS_DETAILS+=("user-deps(Vulkan/X11/lavapipe/Xvfb)")
    printf '[MISS] user-deps: 未部署(Vulkan 头/glslc/X11/lavapipe 用户级依赖)\n'
  fi
}

chk_user_deps_windows() {
  if [ -f "$MINE_ROOT/.user-deps/env.sh" ]; then
    printf '[OK]   user-deps: %s\n' "$MINE_ROOT/.user-deps/env.sh"
  else
    HARD_MISS=$((HARD_MISS+1)); MISS_DETAILS+=("user-deps(Vulkan SDK + SwiftShader + Qt6)")
    printf '[MISS] user-deps: 未部署(先执行 tools/install-user-deps.sh)\n'
  fi
}

probe() {
  info "=== 系统工具链探测(${OS_PLATFORM:-linux}) ==="
  HARD_MISS=0; MISS_DETAILS=()
  chk "cmake"      "3.22" "cmake --version"
  chk "ninja"      ""     "ninja --version"
  chk "g++"        "11"   "g++ --version"
  chk "pkg-config" ""     "pkg-config --version"
  chk "git"        ""     "git --version"
  chk "python3"    "3.8"  "python3 --version"
  if [ "$OS_PLATFORM" = "windows" ]; then
    chk_user_deps_windows
  else
    chk_user_deps
  fi
}

lavapipe_hint() {
  [ "$OS_PLATFORM" = "windows" ] && return 0
  # 可选:无 GPU 时给出软件光栅建议,不阻塞
  if ! ls /dev/dri/* >/dev/null 2>&1; then
    warn "未检测到 GPU 设备(/dev/dri 为空);离屏渲染依赖 lavapipe(已由 install-user-deps.sh 部署)"
  fi
}

print_help() {
  cat <<'EOF'
用法: tools/setup-env.sh [--check] [--help]

  检测系统工具链(cmake/ninja/g++/pkg-config/git/python3)与用户级系统依赖部署状态。
  --check    只探测;硬依赖缺失时非零退出。
  -h,--help  打印本帮助。
  默认       探测;缺失时打印修复指引,非零退出(无 sudo 自动安装)。

用户级系统依赖缺失时,先执行:
    tools/install-user-deps.sh
  再于每个构建/运行 shell 中 source .user-deps/env.sh。
  Linux:          Vulkan SDK / X11 头 / lavapipe / Xvfb。
  Windows(MSYS2): Vulkan SDK / SwiftShader / Qt6(自动转交 tools/win-deps.sh)。
EOF
}

main() {
  local mode="check"
  if [ "$#" -gt 1 ]; then err "参数过多: $*"; exit 2; fi
  if [ "$#" -eq 1 ]; then
    case "$1" in
      --check) mode="check" ;;
      -h|--help) print_help; exit 0 ;;
      *) err "未知参数: $1"; exit 2 ;;
    esac
  fi

  probe
  if [ "$HARD_MISS" -eq 0 ]; then
    info "硬依赖齐全。构建前先 source .user-deps/env.sh。"
    lavapipe_hint
    exit 0
  fi

  err "硬依赖缺失 ${HARD_MISS} 项: ${MISS_DETAILS[*]}"
  warn "Vulkan/X11/lavapipe/Xvfb 为无 sudo 用户级部署,请先执行: tools/install-user-deps.sh"
  exit 1
}

main "$@"
