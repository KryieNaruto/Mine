#!/usr/bin/env python3
"""L2 门控:场景层不得读时钟(spec §3 D13;plans-l2 §5)。

断言:
  1. `MagicBlocks/include/magicblocks/scene/**` 与 `MagicBlocks/src/scene/**`
     的**代码行**里不得出现任何时钟/计时 API:
       std::chrono / steady_clock / system_clock / high_resolution_clock /
       time( / clock( / GetTickCount / QueryPerformanceCounter
  2. 同时不接受 `gettimeofday` / `clock_gettime` / `std::timespec_get`。

为什么要有这条门:命令流必须是**确定性**的(S4/S7 —— 同一帧重复录制哈希相同)。
一旦 L2 里出现 `now()`,时间戳就会渗进 `EventRecord::timestamp_us`、
`PassRecord` 或命令流的任何字段,哈希随之漂移,golden 与回放比对全部失效。
因此时间戳只能由**调用方**传入(见 `EventRecord::timestamp_us` 的注释:
「由调用方给;L2 绝不读时钟」)。

扫描范围(相对 --root,默认 MagicBlocks):
  include/magicblocks/scene/**   src/scene/**
不扫描 tests / tools:测试与门控脚本本身可以用时钟。

只认**代码行**:注释里的示例写法、字符串字面量里的字样都不算违规
(与 check_no_raw_alloc.py 的口径一致)。

用法: python tools/check_scene_no_clock.py [--root MagicBlocks]
退出码: 0 通过,1 违规,2 用法/环境错误。
"""

import argparse
import os
import re
import sys

# 时钟 / 计时 API。带括号的必须真的像一次调用(允许中间有空白)。
CLOCK_PATTERNS = [
    (re.compile(r"\bstd\s*::\s*chrono\b"), "std::chrono"),
    (re.compile(r"\bstd\s*::\s*steady_clock\b"), "std::steady_clock"),
    (re.compile(r"\bstd\s*::\s*system_clock\b"), "std::system_clock"),
    (re.compile(r"\bstd\s*::\s*high_resolution_clock\b"), "std::high_resolution_clock"),
    (re.compile(r"\bsteady_clock\b"), "steady_clock"),
    (re.compile(r"\bsystem_clock\b"), "system_clock"),
    (re.compile(r"\bhigh_resolution_clock\b"), "high_resolution_clock"),
    (re.compile(r"\btime\s*\("), "time("),
    (re.compile(r"\bclock\s*\("), "clock("),
    (re.compile(r"\bGetTickCount(?:64)?\s*\("), "GetTickCount"),
    (re.compile(r"\bQueryPerformanceCounter\s*\("), "QueryPerformanceCounter"),
    (re.compile(r"\bgettimeofday\s*\("), "gettimeofday"),
    (re.compile(r"\bclock_gettime\s*\("), "clock_gettime"),
    (re.compile(r"\btimespec_get\s*\("), "timespec_get"),
    (re.compile(r"\bGetSystemTime(?:AsPreciseFileTime)?\s*\("), "GetSystemTime*"),
    (re.compile(r"\bGetLocalTime\s*\("), "GetLocalTime"),
]

CODE_EXTS = {".h", ".hpp", ".cpp", ".cxx", ".cc", ".inl", ".ipp"}

# 相对 --root 的扫描根。
SCAN_DIRS = ("include/magicblocks/scene", "src/scene")

SKIP_DIR_NAMES = {"build", "vendor", "third_party", "out", ".git", ".vs", ".vscode", "__pycache__"}


def _skip_dir(name):
    low = name.lower()
    return low in SKIP_DIR_NAMES or low.startswith("build-") or low.startswith("build_")


def strip_comments_and_strings(lines):
    """去掉行注释 / 块注释 / 字符与字符串字面量,只留可执行代码。

    与 check_no_raw_alloc.py 同口径:门控只认代码行,注释里的示例与
    字符串里提到的 API 名字不算违规。
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
        # 去掉字符串/字符字面量,避免 "time(" 这类文本误判。
        s = re.sub(r'"(\\.|[^"\\])*"', '""', s)
        s = re.sub(r"'(\\.|[^'\\])*'", "''", s)
        out.append(s)
    return out


def iter_code_files(base):
    for dirpath, dirnames, filenames in os.walk(base):
        dirnames[:] = sorted(d for d in dirnames if not _skip_dir(d))
        for name in sorted(filenames):
            if os.path.splitext(name)[1].lower() in CODE_EXTS:
                yield os.path.join(dirpath, name)


def check_file(path, rel):
    """返回 (违规信息列表, 扫描行数)。"""
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            raw_lines = f.read().splitlines()
    except OSError as exc:
        raise OSError("读取失败: %s (%s)" % (path, exc))

    code_lines = strip_comments_and_strings(raw_lines)
    violations = []
    for lineno, code in enumerate(code_lines, 1):
        if not code.strip():
            continue
        for pattern, label in CLOCK_PATTERNS:
            if pattern.search(code):
                violations.append(
                    "%s:%d  L2 不得读时钟(命中 `%s`): %s"
                    % (rel, lineno, label, code.strip())
                )
                break
    return violations, len(code_lines)


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--root", default=None, help="MagicBlocks 根目录")
    args = ap.parse_args(argv)

    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.abspath(args.root or os.path.join(os.path.dirname(here), "MagicBlocks"))
    if not os.path.isdir(root):
        sys.stderr.write("找不到 MagicBlocks 根目录: %s\n" % root)
        return 2

    total_files = 0
    total_lines = 0
    all_violations = []

    for sub in SCAN_DIRS:
        base = os.path.join(root, *sub.split("/"))
        if not os.path.isdir(base):
            sys.stderr.write("警告: 扫描目录不存在,已跳过: %s\n" % os.path.join(root, sub))
            continue
        for path in iter_code_files(base):
            rel = os.path.relpath(path, root).replace(os.sep, "/")
            violations, nlines = check_file(path, rel)
            total_files += 1
            total_lines += nlines
            all_violations.extend(violations)

    if total_files == 0:
        sys.stderr.write("警告: 没有扫到任何文件(检查 --root 是否正确)\n")

    if all_violations:
        # 与其它门控脚本一致:诊断信息走 stdout(自测据此断言关键字)。
        sys.stdout.write("L2 时钟门控失败(spec D13):场景层不得读时钟\n")
        for v in all_violations:
            sys.stdout.write("  " + v + "\n")
        sys.stdout.write(
            "正确做法:时间戳由调用方给(EventRecord::timestamp_us 等),"
            "L2 内部绝不调用 now()。\n"
        )
        return 1

    sys.stdout.write(
        "L2 时钟门控通过: %d 个文件 / %d 行代码,未发现时钟 API\n"
        % (total_files, total_lines)
    )
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
