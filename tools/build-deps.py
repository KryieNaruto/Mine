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
    返回 Windows 风格路径(带盘符反斜杠),供 cmd //c 调用。
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


def _ensure_msvc_env() -> bool:
    """Windows 上把 MSVC(vcvars64)环境注入 os.environ,确保池用 cl 编译。

    根因:cmake_driver 跑 cmake 时 PATH 里没有 cl(MSYS2 只有 g++),CMake 自动选 MinGW,
    SwiftShader 的 __nop()(MSVC-only)直接崩。vcvars64.bat 只在 cmd 进程内改环境,
    因此用 `cmd //c "<vcvars> && set"` 捕获全部 KEY=VALUE 再 apply 到父进程。
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
    try:
        out = subprocess.run(
            ["cmd", "//c", f'"{vcvars}" && set'],
            capture_output=True, text=True, errors="replace", timeout=120,
        )
    except (OSError, subprocess.TimeoutExpired) as e:
        print(f"[ERROR] 无法执行 vcvars64.bat: {e}", file=sys.stderr)
        return False
    if out.returncode != 0:
        print(f"[ERROR] vcvars64.bat 执行失败(rc={out.returncode}):\n{out.stderr[-800:]}", file=sys.stderr)
        return False
    applied = 0
    for line in out.stdout.splitlines():
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
