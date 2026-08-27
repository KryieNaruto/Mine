#!/usr/bin/env python3
"""一键预编译三方库进池(默认 release + debug 双变体)。"""
from __future__ import annotations

import argparse
import glob
import os
import re
import shutil
import subprocess
import sys

from deps_lib import MINE_ROOT, cmake_driver, fetch, manifest, pool
from deps_lib.manifest import LibSpec


def _vcvars_bat() -> str:
    """定位 vcvars64.bat 的 Windows 路径;找不到返回空。

    优先读 .user-deps/vcvars.sh(win-deps.sh 生成,记录 VC_VARS_BAT,MSYS 风格路径;
    win-deps 经 msvc_locate 已选到正确 VS 根,如 18/Insiders);
    缺则回退扫描标准 VS 安装根,优先含 VC/Tools/MSVC(真实 C++ 工具集)的实例。
    返回 Windows 风格路径(带盘符反斜杠),供 cmd 调用(开关按运行时选 /c 或 //c)。
    """
    # 1) win-deps.sh 已写的 vcvars.sh —— 最可靠,含 msvc_locate 选中的根
    vs = os.path.join(MINE_ROOT, ".user-deps", "vcvars.sh")
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
            root = bat[: bat.find("VC\\Auxiliary\\Build")]
            if os.path.isdir(os.path.join(root, "VC", "Tools", "MSVC")):
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


def _msys_linked() -> bool:
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


def _ensure_msvc_env() -> bool:
    """Windows 上把 MSVC(vcvars64)环境注入 os.environ,确保池用 cl 编译。

    根因:cmake_driver 跑 cmake 时 PATH 里没有 cl(MSYS2 只有 g++),CMake 自动选 MinGW,
    SwiftShader 的 __nop()(MSVC-only)直接崩。vcvars64.bat 只在 cmd 进程内改环境,
    因此用 `cmd /c "<vcvars> && set"`(开关按运行时是否 MSYS 链接选 /c 或 //c,
    见 _msys_linked)捕获全部 KEY=VALUE 再 apply 到父进程。
    找不到 vcvars/导出失败 → 打印清晰报错返回 False(调用方停止,别静默走 MinGW)。
    """
    if not pool.on_windows():
        return True  # Linux 无 MSVC 需求
    if os.environ.get("VCINSTALLDIR") and shutil.which("cl"):
        return True  # 已在 MSVC 环境
    vcvars = _vcvars_bat()
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
    # 选 `/c` 或 `//c`(见 _msys_linked)。
    # 卡死根因二(开关改对后暴露):list2cmdline 会把命令内引号转义成 `\"`,cmd /c 剥
    # 首尾引号后留下 `\"...\"` → 执行 `\"C:\...vcvars64.bat\"` → 'not recognized'。
    # 因此不把带引号的命令塞给 cmd /c,改写临时 .cmd 包装 `call vcvars && set`,cmd
    # 只跑无空格无引号的裸文件名,不触发任何转义;batch 语法里 call + 引号是合法的。
    import tempfile
    tmp = tempfile.gettempdir()
    env_txt = os.path.join(tmp, f"vcvars_{os.getpid()}.txt")
    bat_name = f"vcvars_{os.getpid()}.cmd"
    out = None
    print(f"[INFO] 注入 MSVC 环境(vcvars64: {vcvars}) …", flush=True)
    cmd_switch = "//c" if _msys_linked() else "/c"
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
    print(f"[INFO] MSVC 环境已注入(cl: {shutil.which('cl')}),池将用 MSVC 编译", flush=True)
    return True


def _collect_libs(args) -> list:
    return fetch.collect_libs(args)


def topo_expand(libs: list, global_libs: dict) -> list:
    """把 depends_on 传递闭包并入需集,返回拓扑先序列表(依赖在前)。

    环检测:访问中再遇 → RuntimeError;缺定义 → RuntimeError。
    """
    by_name = {lib.name: lib for lib in libs}
    order = []
    state = {}  # 0=未访问 1=访问中 2=完成

    def _spec_of(name: str) -> LibSpec:
        lib = by_name.get(name)
        if lib is not None:
            return lib
        d = global_libs.get(name)
        if d is None:
            raise RuntimeError(f"依赖库 '{name}' 未在全局清单定义")
        lib = LibSpec(
            name=name,
            repo=d["repo"],
            tag=str(d["tag"]),
            build=d.get("build", "cmake"),
            options=d.get("options", []) or [],
            depends_on=d.get("depends_on", []) or [],
            windows_package=d.get("windows_package", "") or "",
        )
        by_name[name] = lib
        return lib

    def visit(name: str, stack: list) -> None:
        st = state.get(name, 0)
        if st == 2:
            return
        if st == 1:
            raise RuntimeError(f"依赖环: {' -> '.join(stack + [name])}")
        state[name] = 1
        lib = _spec_of(name)
        for dep in lib.depends_on:
            visit(dep, stack + [name])
        state[name] = 2
        order.append(lib)

    for lib in libs:
        visit(lib.name, [lib.name])
    return order


def _target_variants(gm: dict, arg: str) -> list:
    avail = manifest.variants(gm)
    if arg == "all":
        return avail
    if arg not in avail:
        raise SystemExit(f"variant '{arg}' 不在清单 variants 中: {avail}")
    return [arg]


def main(argv=None) -> int:
    p = argparse.ArgumentParser(description="预编译三方库进共享池")
    g = p.add_mutually_exclusive_group(required=True)
    g.add_argument("--project", metavar="DIR")
    g.add_argument("--all", action="store_true")
    p.add_argument("--variant", default="all", help="release|debug|all(默认 all)")
    p.add_argument("--jobs", type=int, default=os.cpu_count() or 4, help="ninja -j(默认 CPU 核数)")
    args = p.parse_args(argv)
    if args.jobs < 1:
        p.error("--jobs 必须 ≥ 1")

    gm = manifest.load_global_manifest(MINE_ROOT)
    raw = _collect_libs(args)
    try:
        libs = topo_expand(raw, gm.get("libs", {}) or {})
    except RuntimeError as e:
        print(f"依赖拓扑错误: {e}", file=sys.stderr)
        return 2
    variants = _target_variants(gm, args.variant)
    if not libs:
        print("无需要编译的库。")
        return 0

    # Windows:池依赖必须用 MSVC(cl)编译;先注入 vcvars 环境,失败即停(绝不静默走 MinGW)
    if not _ensure_msvc_env():
        return 3

    summary = {"built": [], "skipped": [], "failed": []}
    lock = pool.load_lock(MINE_ROOT)

    for lib in libs:
        # Windows 上由 pacman 预编译包提供的库:不 fetch、不编译,直接视为满足
        if pool.is_pacman_provided(MINE_ROOT, lib.name):
            for v in variants:
                summary["skipped"].append(f"{manifest.ver_dir(lib.name, lib.tag)} [{v}] (pacman)")
            continue
        if not pool.is_fetched(MINE_ROOT, lib.name, lib.tag):
            ok, msg = fetch.clone_lib(MINE_ROOT, lib)
            if not ok:
                summary["failed"].append(f"{manifest.ver_dir(lib.name, lib.tag)} [fetch] {msg}")
                print(f"  拉取失败: {msg}", file=sys.stderr)
                continue
        for v in variants:
            key = f"{manifest.ver_dir(lib.name, lib.tag)} [{v}]"
            if pool.is_built(MINE_ROOT, lib.name, lib.tag, v):
                summary["skipped"].append(key)
                continue
            print(f"编译 {manifest.ver_dir(lib.name, lib.tag)} [{v}] …", flush=True)
            ok, err = cmake_driver.build_lib(MINE_ROOT, lib, v, args.jobs)
            if ok:
                summary["built"].append(key)
                lock.setdefault(manifest.ver_dir(lib.name, lib.tag), {})
                lock[manifest.ver_dir(lib.name, lib.tag)].setdefault("built", {})
                lock[manifest.ver_dir(lib.name, lib.tag)]["built"][v] = True
            else:
                summary["failed"].append(key)
                print(f"  失败日志(尾部):\n{err[-2000:]}\n", file=sys.stderr, flush=True)

    pool.save_lock(MINE_ROOT, lock)
    for k in ("built", "skipped", "failed"):
        for item in summary[k]:
            print(f"[{k.upper()}] {item}", flush=True)
    print(f"汇总: 已编 {len(summary['built'])} / 跳过 {len(summary['skipped'])} / 失败 {len(summary['failed'])}", flush=True)
    return 1 if summary["failed"] else 0


if __name__ == "__main__":
    sys.exit(main())
