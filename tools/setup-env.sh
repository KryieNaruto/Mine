#!/usr/bin/env bash
# 系统工具链检测/安装:cmake / ninja / g++ / pkg-config / git / python3。
# 只装系统依赖,不碰三方库(拉取/编译由 fetch-deps.py / build-deps.py 负责)。
set -euo pipefail

info() { printf '%s\n' "$*"; }
warn() { printf 'WARN: %s\n' "$*" >&2; }
err()  { printf 'ERROR: %s\n' "$*" >&2; }
has()  { command -v "$1" >/dev/null 2>&1; }

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

probe() {
  info "=== 系统工具链探测 ==="
  HARD_MISS=0; MISS_DETAILS=()
  chk "cmake"      "3.22" "cmake --version"
  chk "ninja"      ""     "ninja --version"
  chk "g++"        "11"   "g++ --version"
  chk "pkg-config" ""     "pkg-config --version"
  chk "git"        ""     "git --version"
  chk "python3"    "3.8"  "python3 --version"
}

print_help() {
  cat <<'EOF'
用法: tools/setup-env.sh [--check] [--help]

  检测/安装系统工具链(cmake/ninja/g++/pkg-config/git/python3)。
  --check    只探测不安装;硬依赖缺失时非零退出。
  -h,--help  打印本帮助。
  默认       探测;硬依赖缺失且存在 apt-get 时 sudo 自动安装,否则打印指引。
EOF
}

attempt_apt() {
  if ! has apt-get; then
    warn "未检测到 apt-get,无法自动安装。请手动安装缺失项: ${MISS_DETAILS[*]}"
    return 1
  fi
  info "将 sudo apt-get 安装缺失硬依赖(需 root 权限,Ctrl-C 取消):"
  sudo apt-get update && sudo apt-get install -y \
    cmake ninja-build build-essential pkg-config git python3
}

main() {
  local mode="install"
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
    info "硬依赖齐全。"
    exit 0
  fi

  if [ "$mode" = "check" ]; then
    err "硬依赖缺失 ${HARD_MISS} 项: ${MISS_DETAILS[*]}"
    exit 1
  fi

  if attempt_apt; then
    info "安装完成,重新探测:"
    probe
    [ "$HARD_MISS" -eq 0 ] && exit 0
    err "安装后仍有缺失: ${MISS_DETAILS[*]}"
    exit 1
  fi
  exit 1
}

main "$@"
