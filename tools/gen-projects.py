#!/usr/bin/env python3
"""按项目 deps.yaml 的 type: 字段生成对应 IDE 工程(当前仅 vs → .sln)。"""
from __future__ import annotations

import argparse
import os
import sys

from deps_lib import MINE_ROOT, cmake_driver, project_gen


def _run(root: str, projects: list, variant: str, generator: str | None) -> int:
    summary = {"generated": [], "skipped": [], "todo": [], "failed": []}
    for name, deps_yaml in projects:
        type_name = project_gen.project_type(deps_yaml)
        ok, msg = project_gen.generate(root, name, type_name, variant, generator)
        if ok and msg.startswith("跳过"):
            summary["skipped"].append(f"{name} ({type_name}) {msg}")
        elif ok:
            summary["generated"].append(f"{name} ({type_name}) → {msg}")
        elif msg.startswith("未实现"):
            summary["todo"].append(f"{name} ({type_name}) {msg}")
        else:
            summary["failed"].append(f"{name} ({type_name}) {msg}")
            print(f"  失败: {msg}", file=sys.stderr, flush=True)

    for key, label in (("generated", "GENERATED"), ("skipped", "SKIPPED"),
                        ("todo", "TODO"), ("failed", "FAILED")):
        for item in summary[key]:
            print(f"[{label}] {item}", flush=True)
    print(
        f"汇总: 生成 {len(summary['generated'])} / 跳过 {len(summary['skipped'])} / "
        f"未实现 {len(summary['todo'])} / 失败 {len(summary['failed'])}",
        flush=True,
    )
    return 1 if summary["failed"] else 0


def main(argv=None) -> int:
    cmake_driver._make_output_safe()
    p = argparse.ArgumentParser(description="按项目类型生成 IDE 工程")
    g = p.add_mutually_exclusive_group(required=True)
    g.add_argument("--project", metavar="DIR")
    g.add_argument("--all", action="store_true")
    p.add_argument("--variant", default="release", help="指向池的哪个变体(默认 release)")
    p.add_argument("--generator", default=None, help="覆盖 VS generator 名(调试用)")
    args = p.parse_args(argv)

    if args.project:
        deps_yaml = os.path.join(MINE_ROOT, args.project, "deps.yaml")
        if not os.path.isfile(deps_yaml):
            p.error(f"项目清单不存在: {deps_yaml}")
        projects = [(args.project, deps_yaml)]
    else:
        projects = project_gen.list_projects(MINE_ROOT)
        if not projects:
            print("未发现任何项目。")
            return 0

    return _run(MINE_ROOT, projects, args.variant, args.generator)


if __name__ == "__main__":
    sys.exit(main())
