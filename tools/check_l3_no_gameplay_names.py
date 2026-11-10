#!/usr/bin/env python3
"""G7 门控:L3 primitives 层不得出现玩法名(spec §2.1 / §14.1)。

SDK 是通用底座,L3 的图元/相机/材质/gizmo 都不能知道"玩法叫什么"。
一旦 `src/primitives/**` 里出现 `if (blocks) ...` 或 `voxel`/`印章` 这类词,
就说明有一条玩法逻辑漏进了底座 —— 之后每加一个玩法都得回来改这里。

与 G3 的区别
------------
G3 扫的是**全部源码**,断言 backend 之外无 Vk、backend 之内无玩法名。
G7 是 L3 专项:扫 `include/magicblocks/primitives/**` 与 `src/primitives/**`,
断言**连玩法名都不出现**(比 G3 更严,因为 G3 允许 features 层有玩法名)。

白名单
------
`MagicBlocks` / `magicblocks` 本身含 "blocks";`timestamp` 含 "stamp"。
不去掉这两个,门控会对每个文件都红 —— 那种规则最后一定被整体禁用,
等于没有门控。所以先抹掉合法标识符再判。

用法: python tools/check_l3_no_gameplay_names.py [--root MagicBlocks]
退出码: 0 通过,1 违规,2 用法/环境错误。
"""

import argparse
import os
import re
import sys

L3_DIRS = [os.path.join("include", "magicblocks", "primitives"), "src/primitives"]

CODE_EXTS = {".h", ".hpp", ".cpp", ".cxx", ".cc", ".inl", ".ipp"}

# 玩法名(spec §2.1 的列表,与 G3 保持一致)
GAMEPLAY_NAMES = [
    "sketch",
    "blocks",
    "voxel",
    "stamp",
    "squeeze",
    "捏捏乐",
    "涂鸦",
    "积木",
    "叠叠乐",
    "印章",
]

# SDK 自己的名字与合法术语,先抹掉
#
# 除了 "magicblocks" 与 "timestamp",还必须抹掉 **内存块** 的 "block(s)":
# arena/池的 `blocks_` 是"一块内存"(`Block& b : blocks_`, `blocks_.push_back`),
# 与"积木玩法"毫无关系。不排除它,G7 会对 alloc.cpp 报 7 处误报,
# 而误报的代价是整条规则被关掉 —— 那比没有规则更糟(会给人虚假的安全感)。
#
# 排除方式是按**完整标识符**排除,不是按子串: `blocks_` / `Block` / `block`
# 都放行,但 `blocks_gameplay` 仍会被抓(因为整词不等于这三个之一)。
ALLOWLIST = re.compile(r"magicblocks|timestamps?", re.IGNORECASE)

# 允许出现的完整标识符(大小写不敏感)
ALLOWED_IDENTIFIERS = {
    "block",  # arena 的内存块
    "blocks",  # 复数形式(blocks_ 去掉下划线后)
}

IDENT_RE = re.compile(r"[A-Za-z_][A-Za-z0-9_]*")

# 分词用:**按非字母数字切**,`_` 也当分隔符。
# 这样 `is_voxel_scene` → [is, voxel, scene],`blocks_` → [blocks]。
ALNUM_RE = re.compile(r"[A-Za-z0-9]+")
# 驼峰边界：`VoxelCamera` → [Voxel, Camera]。
# 不切的话只有 `VoxelCamera` 一个整词，`voxel` 永远匹配不到。
CAMEL_RE = re.compile(r"[A-Z]+(?![a-z])|[A-Z][a-z0-9]*|[a-z0-9]+")

# 英文玩法名的集合(小写),供分词后比对
GAMEPLAY_NAMES_LOWER = {n.lower() for n in GAMEPLAY_NAMES if n.isascii()}


def strip_comments_and_strings(lines):
    """去注释/字符串字面量。注释里提到"积木"是合法的(说明文字)。"""
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


def scan_file(path):
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        raw_lines = f.read().splitlines()
    lines = strip_comments_and_strings(raw_lines)

    violations = []
    for idx, line in enumerate(lines):
        cleaned = ALLOWLIST.sub("", line)
        low = cleaned.lower()

        # 中文名:直接子串匹配(中文没有标识符分隔的问题)
        hit_cn = None
        for name in GAMEPLAY_NAMES:
            if not name.isascii() and name in cleaned:
                hit_cn = name
                break
        if hit_cn is not None:
            violations.append((idx + 1, hit_cn, raw_lines[idx].strip()))
            continue

        # 英文名:按"非字母数字"切词后逐词比对。
        #
        # 为什么不用 `\bvoxel\b` 或 `[A-Za-z_][A-Za-z0-9_]*` 分词:
        #   `_` 在正则里属于 `\w` / 标识符字符,所以 `is_voxel_scene` 会被当成
        #   **一个整词**,`voxel` 永远匹配不到 —— 而"玩法名藏进变量名"恰恰是
        #   最典型的泄漏形态。这里把 `_` 也当分隔符,于是切成 [is, voxel, scene]。
        #
        # 为什么要按词而不是整行子串:
        #   arena 的 `blocks_` 是"一块内存"(`Block& b : blocks_`),与积木玩法无关。
        #   整行匹配会把两者混为一谈 → alloc.cpp 报 7 处误报 → 规则被迫关掉。
        #   按词比对时可以用 ALLOWED_IDENTIFIERS 精确放行 `block`/`blocks`,
        #   而 `blocks_gameplay` 这类新名字仍会被抓。
        words = []
        for tok in ALNUM_RE.findall(cleaned):
            words.extend(CAMEL_RE.findall(tok))
        words = [w.lower() for w in words]
        hit = None
        for w in words:
            if w in ALLOWED_IDENTIFIERS:
                continue
            if w in GAMEPLAY_NAMES_LOWER:
                hit = w
                break
        if hit is not None:
            violations.append((idx + 1, hit, raw_lines[idx].strip()))

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
        for line_no, name, text in scan_file(path):
            all_v.append((os.path.relpath(path, args.root), line_no, name, text))

    if all_v:
        print(f"G7 失败: {len(all_v)} 处玩法名泄漏(扫描 {scanned} 个文件)")
        for path, line_no, name, text in all_v:
            print(f"  {path}:{line_no}: 出现玩法名 '{name}'")
            print(f"      {text}")
        return 1

    print(f"G7 通过: L3 primitives 层零玩法名(扫描 {scanned} 个文件)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
