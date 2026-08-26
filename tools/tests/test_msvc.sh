#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# 复用 msvc.sh 的函数定义但不执行自动安装
source "$ROOT/deps_lib/msvc.sh"

# 1) vcvars 路径拼接(纯函数)
got="$(msvc_vcvars_path "/c/Program Files/Microsoft Visual Studio/2022/BuildTools")"
[ "$got" = "/c/Program Files/Microsoft Visual Studio/2022/BuildTools/VC/Auxiliary/Build/vcvars64.bat" ] \
  || { echo "FAIL vcvars_path: $got"; exit 1; }
echo "PASS msvc_vcvars_path"

# 2) VSINSTALLDIR 优先:指向真实存在 vcvars64.bat 的目录即直接命中
# chmod +x 模拟 MSYS2 语义(MSYS2 下 .bat/.exe 无论权限位均可执行)
TMPD="$(mktemp -d)"
trap 'rm -rf "$TMPD"' EXIT
mkdir -p "$TMPD/vs/VC/Auxiliary/Build"
touch "$TMPD/vs/VC/Auxiliary/Build/vcvars64.bat"
chmod +x "$TMPD/vs/VC/Auxiliary/Build/vcvars64.bat"
got="$(VSINSTALLDIR="$TMPD/vs" msvc_locate)"
[ "$got" = "$TMPD/vs" ] || { echo "FAIL VSINSTALLDIR: $got"; exit 1; }
echo "PASS msvc_locate VSINSTALLDIR 优先"

# 2b) 回归:VSINSTALLDIR 完全未设置(set -u 下最易崩的形态)。paint-pc 实际报
# 'VSINSTALLDIR: unbound variable' —— 捕获赋值不得用 local vs="$VSINSTALLDIR"。
# 这里在全新子 shell 里 unset 后再调,必须不崩且返回 1(无 vswhere 环境)。
(
  unset VSINSTALLDIR
  if msvc_locate >/dev/null 2>&1; then
    echo "FAIL: 未设置 VSINSTALLDIR 且无 vswhere 时不应定位成功"
    exit 1
  fi
) || { echo "FAIL: VSINSTALLDIR 未设置时 msvc_locate 崩溃(unbound variable)"; exit 1; }
echo "PASS msvc_locate 未设置 VSINSTALLDIR 不崩(unbound 回归)"

# 3) 回归:vswhere 每次调用现找,不得用 source 时的缓存。
# 干净机器上 vswhere.exe 随 VS Installer 一起装 —— 首次自动装 Build Tools 前并不存在。
# 源入时(PATH 无 vswhere)缓存为空,装完后再现找必须命中,否则自动装完依然定位失败。
# unset VSWHERE:清掉旧实现的 source 时缓存,强制走现找路径(仅当该变量存在)。
unset VSWHERE 2>/dev/null || true
FAKE_VS="$TMPD/vs2"
mkdir -p "$FAKE_VS/VC/Auxiliary/Build"
touch "$FAKE_VS/VC/Auxiliary/Build/vcvars64.bat"
chmod +x "$FAKE_VS/VC/Auxiliary/Build/vcvars64.bat"
ARGS_LOG="$TMPD/vswhere.args.log"
FAKE_VSWHERE="$TMPD/bin/vswhere.exe"
mkdir -p "$(dirname "$FAKE_VSWHERE")"
cat > "$FAKE_VSWHERE" <<EOF
#!/usr/bin/env bash
# 模拟 vswhere:把每次调用参数记录到 $ARGS_LOG;遇 -property 即输出实例根 $FAKE_VS。
printf '%s\n' "\$@" >> "$ARGS_LOG"
for a in "\$@"; do [ "\$a" = "-property" ] && { printf '%s\n' "$FAKE_VS"; exit 0; }; done
exit 0
EOF
chmod +x "$FAKE_VSWHERE"

: > "$ARGS_LOG"
got="$(PATH="$TMPD/bin:$PATH" VSINSTALLDIR= msvc_locate)" \
  || { echo "FAIL: 装完 Build Tools 后 vswhere 现找失败(source 缓存陈旧)"; exit 1; }
[ "$got" = "$FAKE_VS" ] || { echo "FAIL locate(现找)root: $got"; exit 1; }
echo "PASS vswhere 每次调用现找(装后重新定位)"

# 4) 回归:vswhere 输出的反斜杠 Windows 路径(C:\...)必须先转 POSIX(/c/...)再判存在。
# 真实环境:MSYS2 的 cygpath 把 C:\... → /c/...;这里用假 cygpath 模拟,保证任何平台可测。
# 若定位在转换前用 [ -x "C:\..." ] 判存在(反斜杠当普通字符→相对路径),必失败 → 本次必红。
CYPATH="$TMPD/cygpath"
mkdir -p "$(dirname "$CYPATH")"
cat > "$CYPATH" <<EOF
#!/usr/bin/env bash
# 模拟 MSYS2 cygpath -u:Windows 盘符路径 C:\X\Y → $TMPD/X/Y(映射到临时目录,保证任何平台可建目录判存在);
# 已有 POSIX 路径则原样返回。
case "\$1" in
  -u) ;;
  *) exit 0 ;;
esac
in="\$2"
if [[ "\$in" =~ ^([A-Za-z]):(.*)$ ]]; then
  printf '%s%s\n' "$TMPD" "\$(printf '%s' "\${BASH_REMATCH[2]}" | sed 's|\\\\|/|g')"
else
  printf '%s\n' "\$in"
fi
EOF
chmod +x "$CYPATH"

# 让 msvc.sh 认为有 cygpath(在其源码里 has cygpath → command -v)
CYG_BIN="$TMPD/cygbin"
mkdir -p "$CYG_BIN"
ln -sf "$CYPATH" "$CYG_BIN/cygpath"

# 假 vswhere 输出反斜杠路径(真实 vswhere 行为)
FAKE_VS_WIN="C:\\\\Program Files (x86)\\\\Microsoft Visual Studio\\\\2022\\\\BuildTools"
# 假 cygpath 把 C:\... 映射到临时目录;此处即假 vswhere 实例根
FAKE_VS_POSIX="$TMPD/Program Files (x86)/Microsoft Visual Studio/2022/BuildTools"
mkdir -p "$FAKE_VS_POSIX/VC/Auxiliary/Build"
touch "$FAKE_VS_POSIX/VC/Auxiliary/Build/vcvars64.bat"
chmod +x "$FAKE_VS_POSIX/VC/Auxiliary/Build/vcvars64.bat"
cat > "$TMPD/bin/vswhere.exe" <<EOF
#!/usr/bin/env bash
printf '%s\n' "\$@" >> "$ARGS_LOG"
for a in "\$@"; do [ "\$a" = "-property" ] && { printf '%s\n' "$FAKE_VS_WIN"; exit 0; }; done
exit 0
EOF
chmod +x "$TMPD/bin/vswhere.exe"

got="$(PATH="$TMPD/bin:$CYG_BIN:$PATH" VSINSTALLDIR= msvc_locate)" \
  || { echo "FAIL: 反斜杠 Windows 路径定位失败(vswhere 输出未先转 POSIX?)"; exit 1; }
[ "$got" = "$FAKE_VS_POSIX" ] || { echo "FAIL: 反斜杠路径转换后 root 错误: $got"; exit 1; }
echo "PASS vswhere 反斜杠路径先转 POSIX 再判存在"

# 5) 回归:vswhere 过滤只要求 VC 工具链,不得硬编码 Windows10SDK。
# Windows SDK 组件 id 随系统而异(Win10=Windows10SDK、Win11=Windows11SDK.<ver>),
# 硬编码 Windows10SDK 会漏掉 Win11 实例导致已装仍定位失败。vcvars64.bat 存在性已是可靠判定。
: > "$ARGS_LOG"
PATH="$TMPD/bin:$PATH" VSINSTALLDIR= msvc_locate >/dev/null 2>&1 || true
args="$(cat "$ARGS_LOG")"
if printf '%s' "$args" | grep -q 'Windows10SDK'; then
  echo "FAIL: vswhere 仍要求 Windows10SDK(Win11 下 Build Tools 定位不到)"
  exit 1
fi
printf '%s' "$args" | grep -q 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64' \
  || { echo "FAIL: vswhere 未要求 VC 工具链组件"; exit 1; }
echo "PASS vswhere 过滤不含 Windows10SDK,保留 VC.Tools.x86.x64"

# 6) 兜底:磁盘扫描 —— 用户已装 VS(如 2026,含「使用 C++ 的桌面开发」)但 vswhere 漏报时应直接从磁盘找到。
# 判据 = VC/Tools/MSVC 目录(工具集真实所在,证明装了 C++ 工作负载),且 vcvars64.bat 也需存在。
# 2026 用 Insiders 布局(18/Insiders,即 paint-pc 注释里 -all 才能查到的场景),2022 为普通 BuildTools。
DISKROOT="$TMPD/diskroots"
mkdir -p "$TMPD/emptybin" \
         "$DISKROOT/2022/BuildTools/VC/Tools/MSVC/14.40/bin/Hostx64/x64" \
         "$DISKROOT/2022/BuildTools/VC/Auxiliary/Build" \
         "$DISKROOT/2026/Insiders/VC/Tools/MSVC/14.50/bin/Hostx64/x64" \
         "$DISKROOT/2026/Insiders/VC/Auxiliary/Build"
touch "$DISKROOT/2022/BuildTools/VC/Auxiliary/Build/vcvars64.bat" \
      "$DISKROOT/2026/Insiders/VC/Auxiliary/Build/vcvars64.bat"
chmod +x "$DISKROOT/2022/BuildTools/VC/Auxiliary/Build/vcvars64.bat" \
         "$DISKROOT/2026/Insiders/VC/Auxiliary/Build/vcvars64.bat"
MSVC_DISK_BASES=("$DISKROOT")
# 无 vswhere 环境 + VSINSTALLDIR 未设置 → 应命中磁盘扫描,且新版(2026/Insiders)在前
got="$( ( unset VSINSTALLDIR; PATH="$TMPD/emptybin:$PATH" msvc_locate ) )" \
  || { echo "FAIL: 磁盘扫描兜底未命中(用户已装 VS 但 vswhere 漏报)"; exit 1; }
[ "$got" = "$DISKROOT/2026/Insiders" ] || { echo "FAIL: 磁盘扫描 root 错误(应选新版2026/Insiders): $got"; exit 1; }
echo "PASS 磁盘扫描兜底命中已装 VS(2026/Insiders 优先)"

# 7) msvc_find_vcvars:root 常规位置缺 vcvars 时,全盘兜底应找到其它位置的 vcvars64.bat。
# paint-pc 的 find_vcvars 就是这么做的(定位 VS 根不依赖 vcvars 路径,vcvars 另找)。
# 构造:2026/Insiders 无常规 vcvars,只有 2022 有 → 兜底应命中 2022 的。
DISKROOT2="$TMPD/diskroots2"
mkdir -p "$DISKROOT2/2026/Insiders/VC/Auxiliary/Build" \
         "$DISKROOT2/2022/BuildTools/VC/Auxiliary/Build"
touch "$DISKROOT2/2022/BuildTools/VC/Auxiliary/Build/vcvars64.bat"
chmod +x "$DISKROOT2/2022/BuildTools/VC/Auxiliary/Build/vcvars64.bat"
MSVC_DISK_BASES=("$DISKROOT2")   # 覆盖扫描根,让兜底只在这两个里找
VCVARS_FALLBACK="$DISKROOT2/2022/BuildTools/VC/Auxiliary/Build/vcvars64.bat"
gotvc="$(msvc_find_vcvars "$DISKROOT2/2026/Insiders")"
[ "$gotvc" = "$VCVARS_FALLBACK" ] || { echo "FAIL: msvc_find_vcvars 全盘兜底错误: $gotvc"; exit 1; }
echo "PASS msvc_find_vcvars 全盘兜底找到 vcvars64.bat"

# 8) msvc_write_vcvars_sh:VC_VARS_BAT 应指向 msvc_find_vcvars 实际找到的路径(非猜测位)。
MSVC_DISK_BASES=("$DISKROOT2")
OUTSH="$TMPD/vcvars_out.sh"
msvc_write_vcvars_sh "$DISKROOT2/2026/Insiders" "$OUTSH"
grep -q "VC_VARS_BAT=\"$VCVARS_FALLBACK\"" "$OUTSH" || { echo "FAIL: vcvars.sh 未写全盘兜底路径"; exit 1; }
echo "PASS msvc_write_vcvars_sh 写全盘兜底 vcvars 路径"

# 9) 关键回归:磁盘有 VS2026/Insiders 且 vswhere 只报 2022 BuildTools → 必须选 2026/Insiders。
# paint-pc 实测就是这个场景(vswhere 看不见 18/Insiders);若 vswhere 优先会错选 2022。
# 构造:磁盘上有 2026/Insiders(工具集+vcvars),vswhere 报 2022 BuildTools。
PRIO="$TMPD/prio"
mkdir -p "$PRIO/2026/Insiders/VC/Tools/MSVC/14.50/bin/Hostx64/x64" \
         "$PRIO/2026/Insiders/VC/Auxiliary/Build" \
         "$PRIO/bin" "$PRIO/cygbin" "$PRIO/emptybin"
touch "$PRIO/2026/Insiders/VC/Auxiliary/Build/vcvars64.bat"
chmod +x "$PRIO/2026/Insiders/VC/Auxiliary/Build/vcvars64.bat"
PRIO_CYP="$PRIO/cygpath"; cat > "$PRIO_CYP" <<'EOF'
#!/usr/bin/env bash
[ "$1" = "-u" ] || exit 0
in="$2"
case "$in" in
  C:*) printf '%s\n' '/tmp/PRIO_BT';;   # vswhere 报的 2022 映射到不存在路径 → 判不可用
  *) printf '%s\n' "$in";;
esac
EOF
chmod +x "$PRIO_CYP"; ln -sf "$PRIO_CYP" "$PRIO/cygbin/cygpath"
cat > "$PRIO/bin/vswhere.exe" <<'EOF'
#!/usr/bin/env bash
for a in "$@"; do [ "$a" = "-property" ] && { printf '%s\n' 'C:\Program Files (x86)\Microsoft Visual Studio\2022\BuildTools'; exit 0; }; done
exit 0
EOF
chmod +x "$PRIO/bin/vswhere.exe"
MSVC_DISK_BASES=("$PRIO/2026")
# 磁盘优先 → 应选 2026/Insiders,而非 vswhere 报的 2022 BuildTools
got="$( ( unset VSINSTALLDIR; MSVC_DISK_BASES="$PRIO/2026" PATH="$PRIO/bin:$PRIO/cygbin:$PATH" msvc_locate ) )"
[ "$got" = "$PRIO/2026/Insiders" ] || { echo "FAIL: 磁盘未优先于 vswhere(错选 vswhere 报的 2022): $got"; exit 1; }
echo "PASS 磁盘扫描优先于 vswhere(错选 2022 修复)"
MSVC_DISK_BASES=("$DISKROOT")
