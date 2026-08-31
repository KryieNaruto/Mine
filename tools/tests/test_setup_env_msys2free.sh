#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"

# MSYS2-free 回归:setup-env.sh / install-user-deps.sh / win-deps.sh 三处均不得再引导 MSYS2 或调 pacman。
fail() { echo "FAIL: $*"; exit 1; }

for f in setup-env.sh install-user-deps.sh win-deps.sh; do
  script="$ROOT/$f"
  [ -f "$script" ] || fail "$f 不存在"
  if grep -qE 'ensure_msys2|msys2_locate|deps_lib/msys2\.sh' "$script"; then
    fail "$f 仍引用 MSYS2 引导"
  fi
  # 功能性 pacman 调用(非注释)不得出现
  if grep -qE '^[[:space:]]*(pacman |pacman -|pacman-key)' "$script"; then
    fail "$f 仍调用 pacman"
  fi
done

# win-deps.sh 必须保留的 MSYS2-free 工具链要素:VS 自带 CMake/Ninja + 独立 python + 7zr + 镜像下载。
grep -q 'VS_CMAKE_BIN' "$ROOT/win-deps.sh" || fail "win-deps.sh 缺 VS_CMAKE_BIN"
grep -q 'install_standalone_python' "$ROOT/win-deps.sh" || fail "win-deps.sh 缺独立 python 安装"

# setup.bat 仍须纯 ASCII + CRLF(双击入口硬约束)。
SETUP_BAT="$ROOT/../setup.bat"
if LC_ALL=C tr -d '\r\n\t' < "$SETUP_BAT" | LC_ALL=C grep -q '[^ -~]'; then
  fail "setup.bat 含非 ASCII 字节(cmd.exe 会解析失败闪退)"
fi

echo "PASS 三脚本均无 MSYS2/pacman 引导残留 + win-deps 工具链要素齐全 + setup.bat 纯 ASCII"
