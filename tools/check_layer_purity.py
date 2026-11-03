#!/usr/bin/env python3
"""G3 门控:分层纯度(spec §3 G3 / §7)。

断言三件事:
  1. `src/backend/**` 之外,任何源/头文件不得出现 Vulkan:
     `#include <vulkan/...>`、`Vk*` / `PFN_vk*` 类型、`vkXxx(` / `vmaXxx(` 调用。
  2. `src/backend/**` 之内不得出现玩法名(sketch/blocks/voxel/stamp/捏捏乐/涂鸦/积木/叠叠乐/印章),
     SDK 内部零玩法分支。
  3. `src/features/**`(L4)不得直接写 GPU 命令(`vkCmd*` / `ICommandList` 之外的低层命令)。

第 1 版实现的缺陷(本版修复):原脚本**整个跳过 `src/backend/`**,而条文写的是
「backend 之外不得出现 Vk」——跳过后等于永远不可能抓到 backend 外的泄漏,门控形同虚设。

注释与字符串里的示例写法不计入(只认代码行)。

用法: python tools/check_layer_purity.py [--root MagicBlocks]
退出码: 0 通过,1 违规,2 用法/环境错误。
"""

import argparse
import os
import re
import sys

VK_INCLUDE = re.compile(r"#\s*include\s*[<\"](vulkan|volk|vulkan_beta)[/\"\.]")
VK_TYPE = re.compile(r"\b(?:Vk|PFN_vk)[A-Z]\w*")
VK_CALL = re.compile(r"\b(?:vk|vma)[A-Z]\w*\s*\(")

GAMEPLAY_NAMES = ["sketch", "blocks", "voxel", "stamp", "squeeze", "捏捏乐", "涂鸦", "积木", "叠叠乐", "印章"]

# SDK 自己的名字就含玩法词("MagicBlocks" 含 "blocks","magicblocks" 是命名空间),
# 而 Vulkan 的 timestamp 查询里含 "stamp"。先把这些合法标识符抹掉再判玩法名,
# 否则门控会对每个文件都红 —— 那样的规则会被迫整体忽略,等于没有门控。
SDK_NAME_ALLOWLIST = re.compile(r"magicblocks|timestamps?", re.IGNORECASE)

CODE_EXTS = {".h", ".hpp", ".cpp", ".cxx", ".cc", ".inl", ".ipp"}


def strip_comments_and_strings(lines):
    """去行注释 / 块注释 / 字符串字面量,门控只认真正的代码。"""
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
        # 去掉字符串字面量,避免 "VkFoo" 这类注释性文本误判
        s = re.sub(r'"(\\.|[^"\\])*"', '""', s)
        out.append(s)
    return out


def iter_code_files(base):
    for dirpath, dirnames, filenames in os.walk(base):
        dirnames[:] = sorted(d for d in dirnames if d not in {"build", ".git", "vendor", "out", ".vs"})
        for name in sorted(filenames):
            if os.path.splitext(name)[1].lower() in CODE_EXTS:
                yield os.path.join(dirpath, name)


def read_code_lines(path):
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        return strip_comments_and_strings(f.read().splitlines())


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--root", default=None, help="MagicBlocks 根目录")
    args = ap.parse_args(argv)

    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.abspath(args.root or os.path.join(os.path.dirname(here), "MagicBlocks"))
    if not os.path.isdir(root):
        print("[G3] 目录不存在: %s" % root)
        return 2

    violations = []
    scanned_non_backend = 0
    scanned_backend = 0

    for sub in ("include", "src", "tests", "tools", "shaders"):
        base = os.path.join(root, sub)
        if not os.path.isdir(base):
            continue
        for path in iter_code_files(base):
            rel = os.path.relpath(path, root).replace(os.sep, "/")
            in_backend = rel.startswith("src/backend/")
            in_features = rel.startswith("src/features/")
            try:
                lines = read_code_lines(path)
            except OSError as exc:
                print("[G3] 读取失败: %s (%s)" % (path, exc))
                return 2

            if in_backend:
                scanned_backend += 1
            else:
                scanned_non_backend += 1

            for lineno, code in enumerate(lines, 1):
                if not code.strip():
                    continue
                if not in_backend:
                    if VK_INCLUDE.search(code):
                        violations.append("%s:%d  backend 之外 include Vulkan 头: %s" % (rel, lineno, code.strip()))
                    if VK_TYPE.search(code):
                        violations.append("%s:%d  backend 之外出现 Vk* 类型: %s" % (rel, lineno, code.strip()))
                    if VK_CALL.search(code):
                        violations.append("%s:%d  backend 之外调用 Vulkan/VMA 函数: %s" % (rel, lineno, code.strip()))
                else:
                    low = SDK_NAME_ALLOWLIST.sub("__sdk__", code.lower())
                    for name in GAMEPLAY_NAMES:
                        # 抹掉 SDK 合法名之后再判:magicblocks / MagicBlocks 里
                        # 的 "blocks" 不算玩法泄漏,而 draw_sketch_special /
                        # 独立出现的 blocks 仍然算。
                        if name in low:
                            violations.append("%s:%d  backend 内出现玩法名 '%s': %s" % (rel, lineno, name, code.strip()))
                if in_features and ("vkCmd" in code or "CmdDraw" in code):
                    violations.append("%s:%d  features 层直接写 GPU 命令: %s" % (rel, lineno, code.strip()))

    if violations:
        print("[G3] 违规 %d 处(分层纯度):" % len(violations))
        for v in violations:
            print("  " + v)
        return 1
    print("[G3] OK backend 外扫描 %d 个文件、backend 内 %d 个文件,分层纯净"
          % (scanned_non_backend, scanned_backend))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
