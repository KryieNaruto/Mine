#!/usr/bin/env python3
"""一键预编译三方库进池(默认 release + debug 双变体)。"""
from __future__ import annotations

import argparse
import os
import sys

from deps_lib import MINE_ROOT, cmake_driver, fetch, manifest, pool


def _collect_libs(args) -> list:
    return fetch.collect_libs(args)


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

    gm = manifest.load_global_manifest(MINE_ROOT)
    libs = _collect_libs(args)
    variants = _target_variants(gm, args.variant)
    if not libs:
        print("无需要编译的库。")
        return 0

    summary = {"built": [], "skipped": [], "failed": []}
    lock = pool.load_lock(MINE_ROOT)

    for lib in libs:
        for v in variants:
            key = f"{manifest.ver_dir(lib.name, lib.tag)} [{v}]"
            if pool.is_built(MINE_ROOT, lib.name, lib.tag, v):
                summary["skipped"].append(key)
                continue
            print(f"编译 {manifest.ver_dir(lib.name, lib.tag)} [{v}] …")
            ok, err = cmake_driver.build_lib(MINE_ROOT, lib, v, args.jobs)
            if ok:
                summary["built"].append(key)
                lock.setdefault(manifest.ver_dir(lib.name, lib.tag), {})
                lock[manifest.ver_dir(lib.name, lib.tag)].setdefault("built", {})
                lock[manifest.ver_dir(lib.name, lib.tag)]["built"][v] = True
            else:
                summary["failed"].append(key)
                print(f"  失败日志(尾部):\n{err[-2000:]}\n", file=sys.stderr)

    pool.save_lock(MINE_ROOT, lock)
    for k in ("built", "skipped", "failed"):
        for item in summary[k]:
            print(f"[{k.upper()}] {item}")
    print(f"汇总: 已编 {len(summary['built'])} / 跳过 {len(summary['skipped'])} / 失败 {len(summary['failed'])}")
    return 1 if summary["failed"] else 0


if __name__ == "__main__":
    sys.exit(main())
