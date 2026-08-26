#!/usr/bin/env python3
"""一键拉取三方库源码进池。只拉取,不编译。"""
from __future__ import annotations

import argparse
import sys

from deps_lib import MINE_ROOT, fetch


def main(argv=None) -> int:
    p = argparse.ArgumentParser(description="拉取三方库源码进共享池")
    g = p.add_mutually_exclusive_group(required=True)
    g.add_argument("--project", metavar="DIR", help="按项目 deps.yaml 的 use 集拉取")
    g.add_argument("--all", action="store_true", help="拉取全局清单全部库")
    p.add_argument("--jobs", type=int, default=4, help="并行 clone 数(默认 4)")
    args = p.parse_args(argv)
    if args.jobs < 1:
        p.error("--jobs 必须 ≥ 1")

    libs = fetch.collect_libs(args)
    if not libs:
        print("无需要拉取的库。")
        return 0

    summary = fetch.run(MINE_ROOT, libs, args.jobs)
    for k in ("fetched", "skipped", "failed"):
        for item in summary[k]:
            print(f"[{k.upper()}] {item}", flush=True)
    print(f"汇总: 拉取 {len(summary['fetched'])} / 跳过 {len(summary['skipped'])} / 失败 {len(summary['failed'])}", flush=True)
    return 1 if summary["failed"] else 0


if __name__ == "__main__":
    sys.exit(main())
