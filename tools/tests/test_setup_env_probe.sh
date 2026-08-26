#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# 验证 setup-env.sh 中 probe 的 Windows 代码路径不探测 g++/pkg-config(改为 MSVC),但保留 cmake。
# Windows 路径 = probe() 函数体去掉 Linux(else)分支;g++/pkg-config 只应留在 Linux 分支;cmake 两平台都需要。
win_path="$(awk '
  /^probe\(\) \{/ { inprobe=1; next }
  inprobe && /^\}/ { exit }
  inprobe && /^  else$/ { inlinux=1; next }
  inprobe && inlinux && /^  fi$/ { inlinux=0; next }
  inprobe && !inlinux { print }
' "$ROOT/setup-env.sh")"

[ -n "$win_path" ] || { echo "FAIL: 未抽取到 probe Windows 路径"; exit 1; }
if printf '%s\n' "$win_path" | grep -qE 'chk "(g\+\+|pkg-config)"'; then
  echo "FAIL: probe Windows 路径仍探测 g++/pkg-config(应改 MSVC)"
  exit 1
fi
if ! printf '%s\n' "$win_path" | grep -qE 'chk "cmake"'; then
  echo "FAIL: probe Windows 路径未探测 cmake(build-deps 依赖)"
  exit 1
fi
echo "PASS probe no g++/pkg-config hard-dep; cmake present"

grep -q "msvc" "$ROOT/setup-env.sh" && echo "PASS probe references msvc" || {
  echo "FAIL: probe 未引用 msvc"
  exit 1
}
