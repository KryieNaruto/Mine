#!/usr/bin/env python3
"""新建项目脚手架:从模板生成骨架 + 写 deps.yaml + 薄封装脚本。"""
from __future__ import annotations

import argparse
import os
import re
import shutil
import sys

from deps_lib import MINE_ROOT, manifest

LANGS = {"cpp", "python", "web"}
_NAME_RE = re.compile(r"^[A-Za-z0-9_-]+$")


def render_template(src: str, dst: str, ctx: dict) -> None:
    """递归复制 src 到 dst,替换文本文件中的 {{KEY}} 占位符。"""
    os.makedirs(dst, exist_ok=True)
    for root, dirs, files in os.walk(src):
        rel = os.path.relpath(root, src)
        target_dir = os.path.join(dst, rel)
        os.makedirs(target_dir, exist_ok=True)
        for f in files:
            src_path = os.path.join(root, f)
            dst_path = os.path.join(target_dir, f)
            if f.endswith(".tmpl"):
                dst_path = dst_path[:-5]
                with open(src_path, "r", encoding="utf-8") as fh:
                    content = fh.read()
                for k, v in ctx.items():
                    content = content.replace("{{" + k + "}}", v)
                with open(dst_path, "w", encoding="utf-8") as fh:
                    fh.write(content)
            else:
                with open(src_path, "r", encoding="utf-8") as fh:
                    content = fh.read()
                for k, v in ctx.items():
                    content = content.replace("{{" + k + "}}", v)
                with open(dst_path, "w", encoding="utf-8") as fh:
                    fh.write(content)
    # 复制空目录(如 python/src 的包目录占位已含 __init__.py,此处兜底)
    for root, dirs, _files in os.walk(src):
        rel = os.path.relpath(root, src)
        target_dir = os.path.join(dst, rel)
        for d in dirs:
            os.makedirs(os.path.join(target_dir, d), exist_ok=True)


def _build_find_link(use: list) -> tuple:
    """按库名生成 find_package 与 target_link_libraries 片段(初版映射表)。"""
    # 常见库名 -> CMake package / target(无 config 的库走 add_subdirectory 之外均需用户补充)
    find_lines = []
    link_lines = []
    for name in use:
        find_lines.append(f"find_package({name} CONFIG REQUIRED)")
        link_lines.append(f"{name}::{name}")
    return "\n".join(find_lines), " ".join(link_lines)


def main(argv=None) -> int:
    p = argparse.ArgumentParser(description="新建项目脚手架")
    p.add_argument("lang", choices=sorted(LANGS), help="项目类型")
    p.add_argument("name", help="项目名(目录名)")
    p.add_argument("--libs", default="", help="逗号分隔库名,写入 deps.yaml use(默认空)")
    args = p.parse_args(argv)

    if not _NAME_RE.match(args.name):
        print(f"错误: 项目名 '{args.name}' 非法,仅允许 [A-Za-z0-9_-]。", file=sys.stderr)
        return 2

    dst = os.path.join(MINE_ROOT, args.name)
    if os.path.exists(dst):
        print(f"错误: 目录已存在 {dst}", file=sys.stderr)
        return 2

    use = [x.strip() for x in args.libs.split(",") if x.strip()]
    # 校验库名在全局清单存在
    gm = manifest.load_global_manifest(MINE_ROOT)
    try:
        manifest.resolve_libs(gm, use)  # 未定义会抛 KeyError
    except KeyError as e:
        print(f"错误: {e}", file=sys.stderr)
        return 2

    find_frag, link_frag = _build_find_link(use)
    ctx = {
        "PROJECT_NAME": args.name,
        "DEPS": ", ".join(use),
        "DEPS_FIND": find_frag,
        "DEPS_LINK": f"target_link_libraries({args.name} PRIVATE {link_frag})" if use else "",
    }

    src_tpl = os.path.join(MINE_ROOT, "tools", "templates", args.lang)
    render_template(src_tpl, dst, ctx)

    print(f"已创建项目 {dst}")
    print(f"  依赖: {use or '无'}")
    if use:
        print("  下一步:")
        print(f"    python3 tools/fetch-deps.py --project {args.name}")
        print(f"    python3 tools/build-deps.py --project {args.name}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
