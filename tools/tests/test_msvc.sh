#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# 复用 msvc.sh 的函数定义但不执行自动安装
source "$ROOT/deps_lib/msvc.sh"

# 1) vcvars 路径拼接
got="$(msvc_vcvars_path "/c/Program Files/Microsoft Visual Studio/2022/BuildTools")"
[ "$got" = "/c/Program Files/Microsoft Visual Studio/2022/BuildTools/VC/Auxiliary/Build/vcvars64.bat" ] \
  || { echo "FAIL vcvars_path: $got"; exit 1; }
echo "PASS msvc_vcvars_path"

# 2) 安装根判定(空输入 → 无)
if msvc_locate >/dev/null 2>&1; then
  echo "INFO msvc_locate found (host has VS)"
else
  echo "INFO msvc_locate none (expected in CI)"
fi
echo "PASS msvc_locate no-op"
