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

# setup.bat 双击入口(仓库根,非 tools/ 下):存在且引用 setup-env.sh。
SETUP_BAT="$ROOT/../setup.bat"
[ -f "$SETUP_BAT" ] || { echo "FAIL: setup.bat 不存在"; exit 1; }
grep -q "setup-env.sh" "$SETUP_BAT" && echo "PASS setup.bat 存在且引用 setup-env.sh" || {
  echo "FAIL: setup.bat 未引用 setup-env.sh"
  exit 1
}

# 回归:setup.bat 必须纯 ASCII + CRLF。cmd.exe 按系统代码页(非 UTF-8)读批处理文件,
# 文件里的 UTF-8 中文(如「。」E3 80 82)会被解析成非法 token,报
# "。 was unexpected at this time." 后脚本中止 → 双击一闪即退(Windows 实测复现)。
# 批处理稳健配方 = 纯 ASCII 内容 + CRLF 行尾(由 .gitattributes 的 eol=crlf 保证)。
# 先剔除行尾符 CR/LF/TAB(0x0D/0x0A/0x09,属 ASCII 但不在可打印范围),剩下的若还有
# [^ -~] 命中的就是高字节(0x80-0xFF,即 UTF-8 中文),那才是会让 cmd.exe 解析失败的。
# 注意:grep 也必须 LC_ALL=C——zh_CN.UTF-8 等多字节 locale 下,grep 对 [^ -~] 的
# 匹配会误判纯 ASCII(本机实测:LC_ALL=C tr 后接无 LC_ALL 的 grep,470 字节纯 ASCII
# 被 grep 命中);两侧都强制 C locale 才按字节比较。
if LC_ALL=C tr -d '\r\n\t' < "$SETUP_BAT" | LC_ALL=C grep -q '[^ -~]'; then
  echo "FAIL: setup.bat 含非 ASCII 字节(cmd.exe 会解析失败闪退)"
  exit 1
fi
cr="$(LC_ALL=C tr -cd '\r' < "$SETUP_BAT" | wc -c)"
nl="$(wc -l < "$SETUP_BAT")"
if [ "$cr" -ne "$nl" ]; then
  echo "FAIL: setup.bat 行尾不是 CRLF(CR=$cr, LF=$nl)"
  exit 1
fi
echo "PASS setup.bat 纯 ASCII + CRLF(规避 cmd.exe 解析闪退)"
