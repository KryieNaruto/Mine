#!/usr/bin/env python3
"""G8 门控:L3 primitives 层命令流对等性(spec §14.1 / C-L3-c)。

断言 `include/magicblocks/primitives/**` 与 `src/primitives/**`:
  1. 不 include 任何 backend 头(`backend/`、`vulkan*`、`vma*`);
  2. 不出现 `Vk*` / `PFN_vk*` 类型、`vkXxx(` / `vmaXxx(` 调用;
  3. 不直接调 `readback_*`(GPU 读回会 stall 整帧,必须经
     `IRenderTarget::readback_rgba8` 且只在离线/低频路径);
  4. 不出现裸命令录制入口(`begin_render_pass` / `cmd_*` 之类),
     下发改走 `ICommandList`。

与 G3 的关系
------------
G3 已覆盖 (1)(2) 的**全局**版本。G8 是 L3 的**专项快照**:即使将来 G3 因为
白名单放宽而漏掉什么,L3 这一层仍然被单独锁死。门控重叠不是浪费 ——
分层门控的价值就在于"某一层破了一扇门,另一扇还关着"。

为什么单列 `readback`
---------------------
L3 的 `IPicker` 名字里带 "pick",很容易被写成"渲染后读回像素"。
那会**同步等待 GPU**,把帧时间从 2ms 拉到 16ms 以上,而且让
"录制→回放→复现"三态校验失效(读回结果依赖当帧时序)。
所以 L3 里出现 readback 必须是显式设计过的(比如离屏截图的
`check_png` 工具路径),而不是随手写的拾取。

用法: python tools/check_l3_command_parity.py [--root MagicBlocks]
退出码: 0 通过,1 违规,2 用法/环境错误。
"""

import argparse
import os
import re
import sys

L3_DIRS = [os.path.join("include", "magicblocks", "primitives"), "src/primitives"]

CODE_EXTS = {".h", ".hpp", ".cpp", ".cxx", ".cc", ".inl", ".ipp"}

BACKEND_INCLUDE = re.compile(
    r'#\s*include\s*[<"](?:.*backend/|.*vulkan[\w/]*\.h|.*vma[\w/]*\.h|vulkan_beta)[>"]'
)
VK_TYPE = re.compile(r"\b(?:Vk|PFN_vk)[A-Z]\w*")
VK_CALL = re.compile(r"\b(?:vk|vma)[A-Z]\w*\s*\(")

# 裸读回 / 裸渲染命令:下发改走 ICommandList,读回走 IRenderTarget。
RAW_READBACK = re.compile(r"\b(?:vkMapMemory|vmaMapMemory|waitIdle|device_wait_idle)\s*\(")

RAW_CMD = re.compile(
    r"\b(?:vkCmd|vkQueueSubmit|vkBeginCommandBuffer|vkEndCommandBuffer|vkAllocateCommandBuffers)\w*\s*\("
)


def strip_comments_and_strings(lines):
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
        # **include 检查必须用原始行**:
        # `strip_comments_and_strings` 会把 `#include "x.h"` 的路径替换成 `""`,
        # 于是 BACKEND_INCLUDE 永远匹配不到 —— 这条规则会静默失效。
        # (实测踩过:变异注入 backend include 后 G8 仍然通过。)
        # 注释里的 include 不算泄漏,所以仍用去注释后的结果做行首判断。
        if re.match(r"\s*#\s*include", line) and BACKEND_INCLUDE.search(raw_lines[idx]):
            violations.append((idx + 1, "L3 直接 include 了 backend 头", raw_lines[idx].strip()))
            continue
        if VK_TYPE.search(line):
            violations.append((idx + 1, "L3 出现 Vulkan 类型", raw_lines[idx].strip()))
            continue
        if VK_CALL.search(line):
            violations.append((idx + 1, "L3 出现 Vulkan/VMA 调用", raw_lines[idx].strip()))
            continue
        if RAW_READBACK.search(line):
            violations.append(
                (idx + 1, "L3 出现裸读回/等待(会 stall 整帧)", raw_lines[idx].strip())
            )
            continue
        if RAW_CMD.search(line):
            violations.append(
                (idx + 1, "L3 出现裸渲染命令(应走 ICommandList)", raw_lines[idx].strip())
            )
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
        print(f"G8 失败: {len(all_v)} 处命令流对等性违规(扫描 {scanned} 个文件)")
        for path, line_no, why, text in all_v:
            print(f"  {path}:{line_no}: {why}")
            print(f"      {text}")
        return 1

    print(f"G8 通过: L3 与 backend 完全解耦(扫描 {scanned} 个文件)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
