#!/usr/bin/env bash
# 一键搭建开发环境(全链路):系统工具链 + 用户级系统依赖(Vulkan/X11/lavapipe/Xvfb/Qt)
# + 三方库池(fetch-deps + build-deps)。幂等:已装/已编的自动跳过。
#   --check  只探测并报告缺失项,不安装(CI 用);硬依赖缺失时非零退出。
#   默认     检测缺失 → 自动安装 → 拉取并预编译三方库 → 最终探针验证。
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

# Windows 直接在 Git Bash 里继续;工具链(VS Build Tools / 独立下载)由 win-deps.sh 部署。

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
  # 工具链在 .user-deps/env.sh 的 PATH 里(Windows 由 win-deps.sh 部署);探测前先 source,
  # 否则 Windows --check 会把已装的 cmake/ninja/python3(只在 env.sh PATH)误报 MISS。
  # shellcheck disable=SC1090
  [ -f "$USER_DEPS_ENV" ] && . "$USER_DEPS_ENV"
  info "=== 系统工具链探测(${OS_PLATFORM:-linux}) ==="
  HARD_MISS=0; MISS_DETAILS=()
  if [ "$OS_PLATFORM" = "windows" ]; then
    # Windows:探测 MSVC(vcvars + cl)+ ninja + cmake + glslc/vulkan.h;不再硬要求 g++/pkg-config
    chk "ninja"  "" "ninja --version"
    chk "cmake" "3.22" "cmake --version"
    # MSVC 定位(经 msvc.sh)
    # shellcheck disable=SC1091
    . "$MINE_ROOT/tools/deps_lib/msvc.sh"
    if msvc_locate >/dev/null 2>&1; then
      info "[OK]   MSVC: $(msvc_locate)"
    else
      HARD_MISS=$((HARD_MISS+1)); MISS_DETAILS+=("MSVC(需 VS Build Tools)")
      printf '[MISS] MSVC: 未安装\n'
    fi
    chk_user_deps_windows
  else
    chk "cmake" "3.22" "cmake --version"
    chk "g++"   "11"   "g++ --version"
    chk "pkg-config" "" "pkg-config --version"
    chk_user_deps
  fi
  chk "git" "" "git --version"
  chk "python3" "3.8" "python3 --version"
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

  默认(全链路一键):检测系统工具链与用户级系统依赖缺失 → 自动安装
  (Windows:VS Build Tools/独立工具链;Linux:无 sudo dpkg -x 到 .user-deps/usr)→
  拉取三方库源码并预编译进共享池(fetch-deps --all + build-deps --all)→ 最终探针验证。
  幂等:已装/已编自动跳过。

  --check    只探测;硬依赖缺失时非零退出(不安装,CI 用)。
  -h,--help  打印本帮助。

Linux:   无 sudo,全部落盘 $MINE_ROOT/.user-deps/(工具链/Vulkan/X11/lavapipe/Xvfb)。
Windows: Git Bash 直接跑;工具链用 VS Build Tools / 独立下载,再部署 Vulkan + Qt6;SwiftShader 走池构建。
完成后再于每个构建/运行 shell 中 source .user-deps/env.sh。
EOF
}

# 池是否已按 default_variant 预编译完(按 deps.yaml 的 libs 逐一检查 .built 标记)。
# 只查 default_variant(release,标准构建所需);debug 变体由需要时 build-deps 单独补。
# 返回 0=已就绪(跳过 fetch/build),1=有缺(需构建)。python 子进程反向 exit:
#   - 发现缺失 → exit 1 → bash 侧 pool_built 返回 1(未就绪)
#   - 全部已建 → exit 0 → bash 侧 pool_built 返回 0(就绪)
# 用环境变量传 root 给子进程(heredoc 里不能用变量插值)。
pool_built() {
  local py
  py="$(command -v python3 || true)"
  [ -n "$py" ] || { return 1; }
  MINE_ROOT="$MINE_ROOT" "$py" - <<'PYEOF' || return 1
import os, sys, yaml
root = os.environ["MINE_ROOT"]
# deps.yaml 为 UTF-8;Windows 下 Python 默认 locale 编码是 gbk,必须显式指定,否则解码报错
m = yaml.safe_load(open(os.path.join(root, "third_party", "deps.yaml"), encoding="utf-8"))
variant = m.get("default_variant") or "release"
for name, spec in (m.get("libs") or {}).items():
    bf = os.path.join(root, "third_party", "_install", f"{name}-{spec.get('tag','')}", variant, ".built")
    if not os.path.exists(bf):
        sys.exit(1)  # 缺 .built → 需重编(触发下方 fetch+build)
    # 选项指纹:改 deps.yaml 的 options 后应触发重编;旧 .built 无 opts= 行(无法回溯)则放行
    try:
        content = open(bf, encoding="utf-8").read()
    except Exception:
        sys.exit(1)
    opts = "|".join(sorted(spec.get("options") or []))
    if "opts=" in content and ("opts=" + opts) not in content:
        sys.exit(1)  # 已记录过选项但和现在不一致 → 需重编
sys.exit(0)
PYEOF
}

auto_install() {
  info "=== 全链路一键搭建开始 ==="

  # 1) 用户级系统依赖(含工具链) —— Linux 无 sudo dpkg -x;Windows 自动转交 win-deps.sh
  if [ "$OS_PLATFORM" = "windows" ]; then
    info "Windows: 依赖部署由 install-user-deps.sh 转交 win-deps.sh(工具链/VS/Vulkan/Qt6)"
  fi
  bash "$MINE_ROOT/tools/install-user-deps.sh"

  # Android 工具链(JDK + SDK;探测复用优先,缺失才下载;Windows/Linux 共用)
  bash "$MINE_ROOT/tools/android-deps.sh"

  # 2) 载入用户级环境
  # shellcheck disable=SC1090
  [ -f "$USER_DEPS_ENV" ] && . "$USER_DEPS_ENV"

  # 3) 三方库池:未拉则拉,未编则编
  if ! pool_built; then
    info "=== 拉取三方库源码 ==="
    python3 -u "$MINE_ROOT/tools/fetch-deps.py" --all
    info "=== 预编译三方库进池 ==="
    python3 -u "$MINE_ROOT/tools/build-deps.py" --all
  else
    info "=== 三方库池已就绪,跳过 fetch/build ==="
  fi

  info "=== 生成 IDE 工程(vs → .sln)==="
  python3 -u "$MINE_ROOT/tools/gen-projects.py" --all

  # 4) 最终校验
  info "=== 最终校验 ==="
  probe
  if [ "$HARD_MISS" -eq 0 ]; then
    info "环境就绪。构建前先: source $USER_DEPS_ENV"
    lavapipe_hint
    return 0
  fi
  err "仍有硬依赖缺失 ${HARD_MISS} 项: ${MISS_DETAILS[*]}"
  return 1
}

main() {
  local mode="auto"
  if [ "$#" -gt 1 ]; then err "参数过多: $*"; exit 2; fi
  if [ "$#" -eq 1 ]; then
    case "$1" in
      --check) mode="check" ;;
      -h|--help) print_help; exit 0 ;;
      *) err "未知参数: $1"; exit 2 ;;
    esac
  fi

  if [ "$mode" = "check" ]; then
    probe
    if [ "$HARD_MISS" -eq 0 ]; then
      info "硬依赖齐全。构建前先 source .user-deps/env.sh。"
      lavapipe_hint
      exit 0
    fi
    err "硬依赖缺失 ${HARD_MISS} 项: ${MISS_DETAILS[*]}"
    warn "Vulkan/X11/lavapipe/Xvfb 为无 sudo 用户级部署。运行 tools/setup-env.sh(无 --check)一键自动安装。"
    exit 1
  fi

  auto_install
  # auto_install 内部 return 0/1 成为脚本退出码
}

main "$@"
