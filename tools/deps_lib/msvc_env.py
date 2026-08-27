"""MSVC(vcvars64)环境探测与注入 —— 供 build-deps.py / gen-projects.py 复用。"""
from __future__ import annotations

import glob
import os
import re
import shutil
import subprocess
import sys
import tempfile

from . import pool


def find_vcvars_bat(root: str) -> str:
    """定位 vcvars64.bat 的 Windows 路径;找不到返回空。

    优先读 <root>/.user-deps/vcvars.sh(win-deps.sh 生成,记录 VC_VARS_BAT,MSYS 风格路径;
    win-deps 经 msvc_locate 已选到正确 VS 根,如 18/Insiders);
    缺则回退扫描标准 VS 安装根,优先含 VC/Tools/MSVC(真实 C++ 工具集)的实例。
    返回 Windows 风格路径(带盘符反斜杠),供 cmd 调用(开关按运行时选 /c 或 //c)。
    """
    # 1) win-deps.sh 已写的 vcvars.sh —— 最可靠,含 msvc_locate 选中的根
    vs = os.path.join(root, ".user-deps", "vcvars.sh")
    if os.path.isfile(vs):
        try:
            with open(vs, encoding="utf-8", errors="replace") as f:
                for line in f:
                    m = re.match(r'^\s*export\s+VC_VARS_BAT="?(.+?)"?\s*$', line)
                    if m:
                        p = m.group(1).strip()
                        if p:
                            # vcvars.sh 存的是 MSYS 风格(/c/...),cmd 只认盘符反斜杠路径;
                            # 不转换就原样给 cmd,cmd 会剥引号按空格切,执行 '/Program' → rc=1。
                            if re.match(r"^/[a-zA-Z]/", p):
                                p = p[1].upper() + ":\\" + p[3:].replace("/", "\\")  # /c/... → C:\...
                            return p
        except OSError:
            pass
    # 2) 磁盘扫描标准 VS 根(仅当 vcvars.sh 缺失)。
    #    VS 版本目录命名不统一(v18 与 2022 并存,18 实际比 2022 新),不能按字典序选;
    #    因此只要求"存在 VC/Tools/MSVC(证明装了 C++ 工具集)"即采用,否则取第一个可用。
    bases = (r"C:\Program Files\Microsoft Visual Studio",
             r"C:\Program Files (x86)\Microsoft Visual Studio")
    plain = []
    for base in bases:
        for bat in glob.glob(os.path.join(base, "*", "*", "VC", "Auxiliary", "Build", "vcvars64.bat")):
            vs_root = bat[: bat.find("VC\\Auxiliary\\Build")]
            if os.path.isdir(os.path.join(vs_root, "VC", "Tools", "MSVC")):
                return bat
            plain.append(bat)
    return plain[0] if plain else ""


def _tail(path: str, n: int = 800) -> str:
    """读文件尾部 n 字节(失败回退空串),用于 vcvars 导出失败的报错展示。"""
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            data = f.read()
        return data[-n:]
    except OSError:
        return ""


def is_msys_linked_python() -> bool:
    """当前 Python 进程是否加载了 MSYS2 运行时(msys-2.0.dll)。

    MSYS 链接的 python 里 subprocess 参数会被其运行时做路径转换(`/c` → `C:\\`),
    所以 cmd 开关要写 `//c` 防转换;原生 Windows python(本仓库工具链装的
    mingw-w64-x86_64-python)参数原样传递,必须写 `/c`——`//c` 会让 cmd 打开交互
    shell 等 stdin,卡死到 timeout(本机已复现交互 banner)。GetModuleHandleW 查
    DLL 是否已加载来判定;Linux 上恒 False(无 MSVC 需求)。
    """
    if not pool.on_windows():
        return False
    try:
        import ctypes
        # 句柄是 64 位指针,默认 restype=c_int 会截断致误判;显式定 c_void_p + argtypes。
        _gmw = ctypes.windll.kernel32.GetModuleHandleW
        _gmw.restype = ctypes.c_void_p
        _gmw.argtypes = [ctypes.c_wchar_p]
        return bool(_gmw("msys-2.0.dll"))
    except Exception:
        return False


def ensure_msvc_env(root: str) -> bool:
    """Windows 上把 MSVC(vcvars64)环境注入 os.environ,确保用 cl 编译。

    根因:PATH 里没有 cl(MSYS2 只有 g++),CMake 自动选 MinGW,SwiftShader 的
    __nop()(MSVC-only)直接崩。vcvars64.bat 只在 cmd 进程内改环境,因此用
    `cmd /c "<vcvars> && set"`(开关按运行时是否 MSYS 链接选 /c 或 //c,见
    is_msys_linked_python)捕获全部 KEY=VALUE 再 apply 到父进程。
    找不到 vcvars/导出失败 → 打印清晰报错返回 False(调用方停止,别静默走 MinGW)。
    """
    if not pool.on_windows():
        return True  # Linux 无 MSVC 需求
    if os.environ.get("VCINSTALLDIR") and shutil.which("cl"):
        return True  # 已在 MSVC 环境
    vcvars = find_vcvars_bat(root)
    if not vcvars:
        print("[ERROR] 未找到 vcvars64.bat。请先运行 tools/install-user-deps.sh(win-deps.sh 会定位/装 Build Tools 并写 .user-deps/vcvars.sh)。",
              file=sys.stderr)
        return False
    # 用文件重定向而非管道(capture_output)读 vcvars 的 `set` 输出:
    # cmd 的子进程链(vcvars 会拉起更多 bat)会持有 stdout 管道,capture_output 等 EOF
    # 永远等不到 → 静默卡死。文件上无 EOF 可等,不会死锁。用二进制写避免
    # Windows 文本模式换行/编码坑。
    # 卡死根因一:`//c` 在原生 Windows python 下原样进 cmd,cmd 不认该开关,打开交互
    # shell 等 stdin → 卡到 timeout,vcvars 根本没跑。开关按运行时是否 MSYS 链接
    # 选 `/c` 或 `//c`(见 is_msys_linked_python)。
    # 卡死根因二(开关改对后暴露):list2cmdline 会把命令内引号转义成 `\"`,cmd /c 剥
    # 首尾引号后留下 `\"...\"` → 执行 `\"C:\...vcvars64.bat\"` → 'not recognized'。
    # 因此不把带引号的命令塞给 cmd /c,改写临时 .cmd 包装 `call vcvars && set`,cmd
    # 只跑无空格无引号的裸文件名,不触发任何转义;batch 语法里 call + 引号是合法的。
    tmp = tempfile.gettempdir()
    env_txt = os.path.join(tmp, f"vcvars_{os.getpid()}.txt")
    bat_name = f"vcvars_{os.getpid()}.cmd"
    out = None
    print(f"[INFO] 注入 MSVC 环境(vcvars64: {vcvars}) …", flush=True)
    cmd_switch = "//c" if is_msys_linked_python() else "/c"
    try:
        with open(os.path.join(tmp, bat_name), "w", encoding="utf-8") as _b:
            _b.write("@echo off\r\n")
            _b.write(f'call "{vcvars}"\r\n')
            _b.write("set\r\n")
        with open(env_txt, "wb") as _f:
            _f.write(b"")
            _f.flush()
            out = subprocess.run(
                ["cmd", cmd_switch, bat_name],
                cwd=tmp, stdout=_f, timeout=120,
            )
    except (OSError, subprocess.TimeoutExpired) as e:
        print(f"[ERROR] 无法执行 vcvars64.bat: {e}", file=sys.stderr)
        return False
    finally:
        try:
            os.remove(os.path.join(tmp, bat_name))
        except OSError:
            pass
    if out.returncode != 0:
        print(f"[ERROR] vcvars64.bat 执行失败(rc={out.returncode}):\n{_tail(env_txt, 800)}", file=sys.stderr)
        return False
    applied = 0
    with open(env_txt, "r", encoding="utf-8", errors="replace") as _f:
        out_text = _f.read()
    for line in out_text.splitlines():
        # vcvars 的 set 输出形如 "PATH=C:\...;..."(首行可能是提示/空行,按 = 切首个)
        if "=" not in line:
            continue
        key, _, val = line.partition("=")
        key = key.strip()
        if not key or any(c in key for c in " \t\r\n"):
            continue
        os.environ[key] = val.strip("\r")
        applied += 1
    if not shutil.which("cl"):
        print("[ERROR] 已导出 vcvars 环境但 PATH 里仍无 cl.exe,MSVC 工具链不可用。", file=sys.stderr)
        return False
    print(f"[INFO] MSVC 环境已注入(cl: {shutil.which('cl')}),将用 MSVC 编译", flush=True)
    return True
