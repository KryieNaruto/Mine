# 共享库:MSYS2 定位 / 自动安装 / 重进。由 setup-env.sh 与 install-user-deps.sh source。
# 背景:Git Bash 的 uname 也报 MINGW*,但无 pacman,必须回到真正的 MSYS2 环境。
# 本库在 Windows 且缺 pacman 时:找已装 MSYS2 → 重进;未装 → 下载官方安装器静默装到用户目录 → 重进。
# 仅内部调用,不单独执行。
set -euo pipefail

# msys2_candidates : 输出候选 MSYS2 根目录(每行一个)。
msys2_candidates() {
  local appdata="${LOCALAPPDATA:-}"; appdata="${appdata//\\//}"
  printf '%s\n' \
    "${MSYS2_ROOT:-}" \
    "$appdata/Programs/MSYS2" \
    "/c/msys64" "/c/msys2" "/c/tools/msys64" \
    "/d/msys64" "/d/msys2" "/e/msys64" "/e/msys2" \
    "/f/msys64" "/f/msys2" \
    "$HOME/msys64" "$HOME/msys2" "$HOME/AppData/Local/Programs/MSYS2"
}

# msys2_is_valid <root> : 该根目录是否为可用 MSYS2(有 pacman + bash)。
msys2_is_valid() {
  [ -x "$1/usr/bin/pacman" ] && [ -x "$1/usr/bin/bash.exe" ]
}

# msys2_locate : 找第一个有效 MSYS2 根,输出到 stdout;找不到返回 1。
msys2_locate() {
  local cand
  while IFS= read -r cand; do
    [ -n "$cand" ] || continue
    if msys2_is_valid "$cand"; then
      printf '%s' "$cand"; return 0
    fi
  done < <(msys2_candidates)
  return 1
}

# msys2_install_to <root> : 下载官方安装器并静默安装到 <root>(POSIX 风格,如 /c/Users/x/msys64)。
msys2_install_to() {
  local root="$1" dl winroot
  dl="$MINE_ROOT/.user-deps/msys2-installer.exe"
  mkdir -p "$(dirname "$dl")"
  info "下载 MSYS2 安装器(约 85MB)到 $dl ..."
  curl -fL --retry 3 -o "$dl" \
    "https://github.com/msys2/msys2-installer/releases/latest/download/msys2-x86_64-latest.exe" \
    || { err "下载 MSYS2 安装器失败(需能出网)"; return 1; }
  # --root 需 Windows 路径(如 C:/Users/x/msys64);Git Bash/MSYS2 下 cygpath 必然存在。
  if has cygpath; then
    winroot="$(cygpath -m "$root")"
  else
    winroot="$root"
  fi
  info "静默安装 MSYS2 到 $winroot ..."
  # 官方 CLI 无人值守:in = install,--confirm-command = 免确认,--accept-messages = 接受消息,--root = 目标目录。
  "$dl" in --confirm-command --accept-messages --root "$winroot" || return 1
  rm -f "$dl"
}

# msys2_enter <root> : 前置 MSYS2 bin 到 PATH 并 exec 其 bash 重跑当前脚本。
msys2_enter() {
  local root="$1"
  info "转入 MSYS2: $root 重新执行..."
  export PATH="$root/usr/bin:$root/bin:$root/mingw64/bin:$PATH"
  exec "$root/usr/bin/bash.exe" "$SELF_PATH" "$@"
}

# ensure_msys2 [args...] : 主入口。Windows 且缺 pacman 时调用。
#   已装→重进;未装→自动装到用户目录→重进;装了但 pacman 坏→重装。
# 若既找不到也无法安装(无网/无 curl)→ 明确报错,exit 1。
ensure_msys2() {
  local root found=""
  # 已装且 pacman 可用 → 直接用
  if root="$(msys2_locate)"; then
    msys2_enter "$root" "$@"
  fi
  # 未找到:自动安装到 $HOME/msys64(用户目录,免管理员)
  local target="$HOME/msys64"
  info "未检测到已装 MSYS2。将自动安装到 $target ..."
  if msys2_install_to "$target"; then
    # 安装后应在该目录,再定位一次确认
    if root="$(msys2_locate)"; then
      msys2_enter "$root" "$@"
    fi
    err "MSYS2 已安装但定位失败: $target"
    exit 1
  fi
  err "MSYS2 自动安装失败。请手动安装 https://www.msys2.org/ 后设置 MSYS2_ROOT 指向其目录再试。"
  exit 1
}
