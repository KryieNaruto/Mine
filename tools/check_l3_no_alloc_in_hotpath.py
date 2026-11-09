#!/usr/bin/env python3
"""G6 门控:L3 热路径不得有堆分配(spec §14.1 / U0' / C-L3-b)。

断言 `src/primitives/**` 里的**热路径函数**函数体内不出现:
  - `new` / `new[]` / `delete` / `malloc` / `calloc` / `realloc` / `free`
  - `std::vector` 的 `.push_back` / `.emplace_back` / `.resize` / `.reserve`
    **出现在循环体内**时(循环内的扩容会让帧时间抖动)

热路径怎么认定
--------------
靠函数名后缀白名单。spec 点名的是 `render_frame` / `feed` / `on_input`,
但同类"每帧调用一次"的还有 `build_draw` / `pick_*` / `axis_world_length` /
`build` / `on_gizmo`。这些函数每帧跑 N 次,任何一次分配都会被直接感知。

为什么不用"扫描整个文件":构造/加载路径**本来就该分配**(建材质表、建网格、
create_* 工厂)。一刀切会让门控对每个文件都红,最后被迫整体禁用 ——
那样的规则等于没有。所以只锁每帧函数。

循环体内判定
------------
用大括号深度跟踪:遇到 `for` / `while` 开头的行,记下当前深度,
直到配对的 `}` 闭合为止,这段区间算"循环体内"。

用法: python tools/check_l3_no_alloc_in_hotpath.py [--root MagicBlocks]
退出码: 0 通过,1 违规,2 用法/环境错误。
"""

import argparse
import os
import re
import sys

L3_DIRS = [os.path.join("include", "magicblocks", "primitives"), "src/primitives"]

CODE_EXTS = {".h", ".hpp", ".cpp", ".cxx", ".cc", ".inl", ".ipp"}

# 每帧调用的函数名(允许后缀匹配:render_frame / build_draw / pick_axis / ...)
#
# "build" 必须有:合批器的 `build()`(排序+合并)与覆盖层/图元的 `build()`
# (三角化)都是每帧跑一次的重活。漏掉它,G6 对合批器完全失效 ——
# 而合批器恰恰是最可能偷偷扩容的地方。
HOT_PATH_PATTERNS = [
    "build",
    "render_frame",
    "feed",
    "on_input",
    "on_gizmo",
    "build_draw",
    "pick_axis",
    "pick_scene",
    "pick_mesh_screen",
    "pick_aabb_screen",
    "axis_world_length",
]

ALLOC_RE = re.compile(
    r"(?<![\w.])(?:new\b|new\s*\[|delete\b|delete\s*\[)"  # C++ new/delete
    r"|(?<![\w.:])(?:malloc|calloc|realloc|free)\s*\("  # C 分配
)
# 扩容类调用(只在循环体内判)
GROW_RE = re.compile(r"\.(?:push_back|emplace_back|resize|reserve)\s*\(")
LOOP_RE = re.compile(r"^\s*(?:for|while)\s*\(")

FUNC_START_RE = re.compile(
    r"^\s*(?:[\w:<>,*&\s]+?)\b(\w+)\s*\([^;]*\)\s*(?:const\s*)?(?:override\s*)?"  # 返回类型 名字(...)
    r"(?:final\s*)?\{?\s*$"
)


def strip_comments_and_strings(lines):
    """去注释/字符串字面量,门控只认真代码。"""
    out = []
    in_block = False
    for raw in lines:
        s = raw
        if in_block:
            end = s.find("*/")
            if end < 0:
                out.append("")
                continue
            s = " " * (end + 2) + s[end + 2 :]
            in_block = False
        s = re.sub(r"/\*[^*]*\*/", "", s)
        start = s.find("/*")
        if start >= 0:
            in_block = True
            s = s[:start]
        s = re.sub(r"//.*$", "", s)
        s = re.sub(r'"(?:\\.|[^"\\])*"', '""', s)
        s = re.sub(r"'(?:\\.|[^'\\])*'", "''", s)
        out.append(s)
    return out


def is_hot(name):
    """函数名是否属于"每帧调用"的热路径。

    支持三种形态,因为实际代码三种都有:
      - 完全同名:        `build` / `feed`
      - 有前缀(实现类):  `Overlay2D::build` 剥掉作用域后仍是 `build`
      - 有后缀(具体化):  `render_frame_impl` / `pick_axis_internal`
    """
    for p in HOT_PATH_PATTERNS:
        if name == p:
            return True
        if name.startswith(p + "_") or name.endswith("_" + p):
            return True
    return False


def scan_file(path):
    """返回 [(行号, 原因, 行内容)]"""
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        raw_lines = f.read().splitlines()
    lines = strip_comments_and_strings(raw_lines)

    violations = []
    # 找热路径函数的起止:名字命中后,从 `{` 开始,按深度找配对 `}`
    i = 0
    n = len(lines)
    while i < n:
        m = FUNC_START_RE.match(lines[i])
        if m and is_hot(m.group(1)):
            # 往后找函数体起始 `{`
            j = i
            depth = 0
            started = False
            loop_depth = 0
            loop_until = []
            while j < n:
                line = lines[j]
                for ch in line:
                    if ch == "{":
                        depth += 1
                        started = True
                    elif ch == "}":
                        depth -= 1
                if started:
                    # 热路径体内检查分配
                    if ALLOC_RE.search(line):
                        violations.append((j + 1, "热路径内直接分配", raw_lines[j].strip()))
                    # 循环体判定
                    if LOOP_RE.match(line):
                        loop_until.append(depth - line.count("}") + 1)
                    in_loop = any(depth >= d for d in loop_until if d > 0)
                    if in_loop and GROW_RE.search(line):
                        violations.append(
                            (j + 1, "热路径循环内扩容(会导致帧时间抖动)", raw_lines[j].strip())
                        )
                    loop_until = [d for d in loop_until if depth >= d]
                    if depth <= 0:
                        break
                j += 1
            i = j + 1
            continue
        i += 1
    return violations


def iter_sources(root):
    for d in L3_DIRS:
        base = os.path.join(root, d)
        if not os.path.isdir(base):
            continue
        for dirpath, _dirs, files in os.walk(base):
            for fn in files:
                if os.path.splitext(fn)[1] in CODE_EXTS:
                    yield os.path.join(dirpath, fn)


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default="MagicBlocks")
    args = ap.parse_args(argv)

    if not os.path.isdir(args.root):
        print(f"错误: 找不到根目录 {args.root}", file=sys.stderr)
        return 2

    all_v = []
    scanned = 0
    for path in sorted(iter_sources(args.root)):
        scanned += 1
        for line_no, why, text in scan_file(path):
            all_v.append((os.path.relpath(path, args.root), line_no, why, text))

    if all_v:
        print(f"G6 失败: {len(all_v)} 处热路径违规(扫描 {scanned} 个文件)")
        for path, line_no, why, text in all_v:
            print(f"  {path}:{line_no}: {why}")
            print(f"      {text}")
        return 1

    print(f"G6 通过: L3 热路径零堆分配(扫描 {scanned} 个文件)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
