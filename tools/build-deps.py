#!/usr/bin/env python3
"""一键预编译三方库进池(默认 release + debug 双变体)。"""
from __future__ import annotations

import argparse
import os
import sys

from deps_lib import MINE_ROOT, cmake_driver, fetch, manifest, msvc_env, pool
from deps_lib.manifest import LibSpec


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
    # Windows 控制台是 GBK,子进程输出(经 errors="replace" 解码)可能含 U+FFFD,
    # 直接 print 回 GBK 会 UnicodeEncodeError 崩掉,长编译中断。全局把编码错误处理
    # 降为 replace:宁可打 `?` 也不崩(详见 cmake_driver._make_output_safe)。
    cmake_driver._make_output_safe()
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
    if not msvc_env.ensure_msvc_env(MINE_ROOT):
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
        # SwiftShader 需 glslang 子模块(Vulkan 必需);CMake configure 里拉全量易 502,
        # 这里提前浅克隆+重试,就位后其 InitSubmodule 检测到 .git 直接跳过。
        if lib.name == "swiftshader":
            ok, err = fetch.ensure_swiftshader_submodules(MINE_ROOT)
            if not ok:
                summary["failed"].append(f"{manifest.ver_dir(lib.name, lib.tag)} [submodule] {err}")
                print(f"  SwiftShader 子模块失败: {err}", file=sys.stderr)
                continue
        for v in variants:
            key = f"{manifest.ver_dir(lib.name, lib.tag)} [{v}]"
            if pool.is_built(MINE_ROOT, lib.name, lib.tag, v, lib.options):
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
