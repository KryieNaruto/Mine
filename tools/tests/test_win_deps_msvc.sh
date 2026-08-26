#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WIN_DEPS="$ROOT/win-deps.sh"

# 复用 win-deps.sh 的 env.sh 生成段(抽出为函数前,直接测关键不变量):
# 抽出 'cat > "$USER_DEPS/env.sh" <<EOF' 到 'EOF' 之间的 env.sh 主体,断言不含 /mingw64。
env_block="$(awk '
  index($0, "USER_DEPS/env.sh") { in_env=1; next }
  in_env && $0 == "EOF" { exit }
  in_env { print }
' "$WIN_DEPS")"

[ -n "$env_block" ] || { echo "FAIL: 未抽取到 env.sh 生成段"; exit 1; }
printf '%s\n' "$env_block" | grep -q 'CMAKE_PREFIX_PATH' || { echo "FAIL: env.sh 缺 CMAKE_PREFIX_PATH"; exit 1; }
if printf '%s\n' "$env_block" | grep -q 'mingw64'; then
  echo "FAIL: env.sh 仍含 /mingw64"; exit 1
fi
# env.sh 前缀引用 QT_PREFIX;QT_PREFIX 定义须含 <qt_version>/<target_arch> 层
# (aqtinstall 落盘在 <outputdir>/<qt_version>/<target_arch>/)
qt_def="$(grep -nE '^QT_PREFIX=' "$WIN_DEPS" || true)"
[ -n "$qt_def" ] || { echo "FAIL: win-deps 缺 QT_PREFIX 定义"; exit 1; }
if ! printf '%s\n' "$qt_def" | grep -qE 'qt/6\.5\.3/msvc2019_64'; then
  echo "FAIL: QT_PREFIX 缺 <version>/<arch> 层"; exit 1
fi
# env.sh 前缀确实引用 QT_PREFIX(而非硬编码 /mingw64)
if ! printf '%s\n' "$env_block" | grep -qE 'CMAKE_PREFIX_PATH="\$QT_PREFIX:'; then
  echo "FAIL: env.sh CMAKE_PREFIX_PATH 未引用 QT_PREFIX"; exit 1
fi
echo "PASS env.sh no /mingw64 + QT_PREFIX 含 6.5.3/msvc2019_64"
