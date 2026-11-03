#!/usr/bin/env python3
"""G4 门控:内存路径唯一(spec §3 G4 / §7;L1 口径,plans/2026-11-03-magicblocks-l1.md §5)。

断言:
  1. **全仓零裸分配**:任何被扫描的代码文件里都不得出现
     `vkAllocateMemory` / `vkFreeMemory` / `vkMapMemory` / `vkUnmapMemory` 的调用。
     L0 版曾在 `MagicBlocks/src/backend/` 内豁免 `// VMA-injection` 标记块里的裸分配
     (为 L1 注入预留);L1 已落地 VMA 分页分配器,该预留**失效**:
     `// VMA-injection` 标记可以继续留在注释里当「注入点存档」,但它**不再豁免任何调用**,
     带着标记的裸分配同样判违规(这是本轮收紧的关键点,自测里有对应负向样本)。
  2. **VMA 唯一注入点**:`vk_mem_alloc.h` 只允许在
     `MagicBlocks/src/backend/vulkan/vulkan_allocator.cpp` 里 `#include`
     (附带 `VMA_IMPLEMENTATION` 的那唯一一个 TU)。别的文件 include 它 → 违规。
  3. 扫描范围:`include/**`、`src/**`、`tests/**`、`tools/**`(含 `src/backend/**`)。
  4. 排除第三方与构建产物目录(`build`/`build-*`/`vendor`/`third_party`/`out`/`.git` 等):
     `third_party/_install/vma-v3.5.0/include/vk_mem_alloc.h` 等 vendor 头不在口径内。

注释与字符串里的示例写法不计入(只认代码行)。

用法: python tools/check_no_raw_alloc.py [--root MagicBlocks]
退出码: 0 通过,1 违规,2 用法/环境错误。
"""

import argparse
import os
import re
import sys

# 裸分配入口。带括号才算调用:错误字符串里写 "vkAllocateMemory" 不带括号不算
# (ARCHITECTURE.md §10 第 6 条)。
RAW_ALLOC = re.compile(r"\bvk(AllocateMemory|FreeMemory|MapMemory|UnmapMemory)\s*\(")

# 已失效的 L0 豁免标记:仅用于在违规信息里提示「标记不再豁免」。
VMA_INJECTION_MARK = "VMA-injection"

VMA_HEADER = "vk_mem_alloc.h"
VMA_INCLUDE = re.compile(r"#\s*include\s*[<\"][^<>\"\n]*" + re.escape(VMA_HEADER) + r"[>\"]")
VMA_IMPL = re.compile(r"^\s*#\s*define\s+VMA_IMPLEMENTATION\b")
# VMA 唯一注入点(仓库相对路径,统一用 / 分隔)。
VMA_ALLOWED_INCLUDE = "src/backend/vulkan/vulkan_allocator.cpp"

CODE_EXTS = {".h", ".hpp", ".cpp", ".cxx", ".cc", ".inl", ".ipp"}

SCAN_SUBDIRS = ("include", "src", "tests", "tools")

# 目录名:构建产物 / 第三方 / VCS 元数据,不扫。
SKIP_DIR_NAMES = {"build", "vendor", "third_party", "out", ".git", ".vs", ".vscode", "__pycache__"}


def _skip_dir(name):
    low = name.lower()
    return low in SKIP_DIR_NAMES or low.startswith("build-") or low.startswith("build_")


def strip_comments(lines, strip_strings=True):
    """去行注释 / 块注释 / 字符串字面量,门控只认真正的代码。

    `strip_strings=False` 时保留字符串内容:扫 `#include "vk_mem_alloc.h"` 时不能把
    带引号的头文件名当字符串抹掉,否则引号写法永远漏检。
    """
    out = []
    in_block = False
    for raw in lines:
        s = raw
        if in_block:
            end = s.find("*/")
            if end == -1:
                out.append("")
                continue
            s = s[end + 2:]
            in_block = False
        while True:
            start = s.find("/*")
            if start == -1:
                break
            end = s.find("*/", start + 2)
            if end == -1:
                s = s[:start]
                in_block = True
                break
            s = s[:start] + s[end + 2:]
        s = s.split("//", 1)[0]
        if strip_strings:
            # 去掉字符串字面量,避免 "vkAllocateMemory(" 这类注释性文本误判
            s = re.sub(r'"(\\.|[^"\\])*"', '""', s)
        out.append(s)
    return out


def _in_l0_injection_block(raw_lines, lineno):
    """该行是否处在 L0 的 `// VMA-injection` 注入区里(仅用于提示,不影响判定)。

    照搬 L0 版的区段语义:标记行之后(含标记行本身)到下一个空行为止;标记行是注释,
    本身也会被剥成空行,故标记行自身不清零。这样独占一行、行尾注释两种写法都能识别。
    """
    active = False
    for idx, raw in enumerate(raw_lines, 1):
        has_mark = VMA_INJECTION_MARK in raw
        if has_mark:
            active = True
        if idx == lineno:
            return active
        if not raw.strip() and not has_mark:
            active = False
    return False


def iter_code_files(base):
    for dirpath, dirnames, filenames in os.walk(base):
        dirnames[:] = sorted(d for d in dirnames if not _skip_dir(d))
        for name in sorted(filenames):
            if os.path.splitext(name)[1].lower() in CODE_EXTS:
                yield os.path.join(dirpath, name)


def check_file(path, rel):
    """返回 (裸分配违规列表, VMA include 违规列表, 扫描行数)。"""
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            raw_lines = f.read().splitlines()
    except OSError as exc:
        raise OSError("读取失败: %s (%s)" % (path, exc))
    code_lines = strip_comments(raw_lines)
    # include 必须按位置匹配:去掉字符串过滤会连 `#include "vk_mem_alloc.h"` 的头文件名
    # 一起抹掉,但保留字符串又会让 `const char* s = "#include <vk_mem_alloc.h>";` 误报。
    # 所以用「剥注释、保留字符串」的行,并要求 `#include` 出现在该行第一个非空白处。
    include_lines = strip_comments(raw_lines, strip_strings=False)

    allowed_target = rel == VMA_ALLOWED_INCLUDE
    alloc_violations = []
    vma_violations = []

    for lineno, code in enumerate(code_lines, 1):
        if not code.strip():
            continue
        raw = raw_lines[lineno - 1]
        m = RAW_ALLOC.search(code)
        if m:
            where = "backend 内" if rel.startswith("src/backend/") else "backend 之外"
            msg = "%s:%d  %s出现裸内存分配 `vk%s(`(全仓零裸分配): %s" % (
                rel, lineno, where, m.group(1), code.strip())
            if _in_l0_injection_block(raw_lines, lineno):
                # L0 的豁免标记:显式提示已失效,免得有人照着旧文档以为这里可以放过。
                msg += " [// %s 标记自 L1 起不再豁免]" % VMA_INJECTION_MARK
            alloc_violations.append(msg)

    for lineno, code in enumerate(include_lines, 1):
        stripped = code.strip()
        if not stripped:
            continue
        # 只认行首指令形态:`#include "..."` / `#  include <...>`。
        # 反例 `const char* s = "#include <vk_mem_alloc.h>";` 以 const 开头,不算 include。
        is_directive = stripped.startswith("#") and stripped.lstrip("#").lstrip().startswith("include")
        if is_directive and VMA_INCLUDE.search(stripped) and not allowed_target:
            vma_violations.append(
                "%s:%d  只允许 %s 内 include %s(VMA 唯一注入点): %s"
                % (rel, lineno, VMA_ALLOWED_INCLUDE, VMA_HEADER, stripped))
        if VMA_IMPL.search(stripped) and not allowed_target:
            vma_violations.append(
                "%s:%d  VMA_IMPLEMENTATION 只允许在 %s 内定义: %s"
                % (rel, lineno, VMA_ALLOWED_INCLUDE, stripped))

    return alloc_violations, vma_violations, len(code_lines)


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--root", default=None, help="MagicBlocks 根目录")
    args = ap.parse_args(argv)

    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.abspath(args.root or os.path.join(os.path.dirname(here), "MagicBlocks"))
    if not os.path.isdir(root):
        print("[G4] 目录不存在: %s" % root)
        return 2

    violations = []
    scanned = 0

    for sub in SCAN_SUBDIRS:
        base = os.path.join(root, sub)
        if not os.path.isdir(base):
            continue
        for path in iter_code_files(base):
            rel = os.path.relpath(path, root).replace(os.sep, "/")
            try:
                alloc_v, vma_v, _ = check_file(path, rel)
            except OSError as exc:
                print("[G4] %s" % exc)
                return 2
            scanned += 1
            violations.extend(alloc_v)
            violations.extend(vma_v)

    if violations:
        print("[G4] 违规 %d 处(内存路径唯一):" % len(violations))
        for v in violations:
            print("  " + v)
        return 1
    print("[G4] OK 扫描 %d 个文件,全仓零裸内存分配、%s 仅 %s 一处 include"
          % (scanned, VMA_HEADER, VMA_ALLOWED_INCLUDE))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
