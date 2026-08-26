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

# 4) 回归:vswhere 过滤只要求 VC 工具链,不得硬编码 Windows10SDK。
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
