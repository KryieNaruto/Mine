#!/usr/bin/env python3
"""UIG2 门控:主路径每帧单次提交(spec §5.1 P3 / §5.4 末行 / §10 验收门 2)。

§5.4 的硬约束是「UI 单独一次 submit」属禁止项 —— 场景 pass、UI pass、上屏必须在
**同一 CommandBuffer、同一次 submit** 里完成。本门控把这条设计约束静态化:

  `PresentPaths` 之外的文件不得出现队列提交调用;提交只允许出现在 Presenter 的
  帧提交点(`*present*` 文件)里。

== 扫描范围 ==
  `MagicBlocks/src/ui/**` 与 `MagicBlocks/tests/**` 的全部源文件
  (`.h .hpp .cpp .cxx .cc .inl .ipp`;`build`/`.git`/`vendor`/`out`/`.vs` 等目录跳过)。
  与 UIG1 不同,这里 tests/ 不按文件名过滤:任何测试文件里直接提交队列,同样是
  第二条上屏路径。非 UI 层(如 `src/backend/`)的提交由 SDK 自己负责,不在本门控范围。

== 关键字(先剥注释与字符串字面量,只认代码行)==
  1. `->submit(` / `->submit<`  —— 指针形式的提交
  2. `.submit(` / `.submit<`    —— 对象形式的提交(含模板实参写法 `.submit<Bindless>(...)`)
  3. `vkQueueSubmit` / `QueueSubmit`(含 `vkQueueSubmit2`)—— 裸 Vulkan 队列提交
  4. `queue_submit(` / `QueueSubmit(` —— SDK/封装层可能暴露的命名形态(宽松匹配)
  注意:`submit` 后面允许空白,`->submit (`(空格)同样算;但**不以裸 `submit(` 匹配**,
  否则 `Signal::submit(...)` 这种自定义信号会误报 —— 门控被误报逼到关闭就等于没有门控。

== 豁免(仅此一条,窄且写死)==
  **呈递路径**:文件相对路径(小写)<b>basename</b> 含 `present` 的文件允许提交,
  例如 `src/ui/presenter.cpp`、`src/ui/presenter_frame.h`、`src/ui/present/presenter.cpp`。
  `PresentPaths` 指的正是这条链路:§5.2 帧结构里第 4 步 `Submit(cmd)` 的唯一落点。
  判定看 basename 而不是整条路径:否则 `src/ui/present_utils/queue.cpp` 这种
  「目录名顺带叫 present」的文件会被整目录免检 —— 那是可滥用的口子。
  其余文件(`src/ui/router/**`、`src/ui/hud/**`、`src/ui/offscreen/**`、`tests/**`)
  **一处提交都不许有**。
  和 UIG1 一样:豁免由**路径**决定,与内容无关 —— 注释里写记号、`#define` 宏、
  写句 `// allow-submit` 都不会免检。

注释与字符串里的示例写法不计(只认代码行)。

用法: python tools/check_ui_single_submit.py [--root MagicBlocks]
退出码: 0 通过,1 违规,2 用法/环境错误。

已知盲区:
  - 词法门控只能抓「字面上出现的提交调用」。把 `vkQueueSubmit` 收进函数指针表、
    由 `Submitter::DoSubmit()` 转发,或换成自造名字(`enqueue()`)都抓不到 ——
    §10 验收门 2 的**运行时断言**(每帧 submit 计数 = 1)仍是必需的,本脚本只负责
    拦「UI 层出现第二个提交点」这一最容易发生的退化;
  - 只做静态断言,不看是否真的每帧一次(例如在 Presenter 里循环提交两次,本门控是绿的)。
"""

import argparse
import os
import re
import sys

TAG = "[UIG2]"

# `->submit (` 与 `.submit<...>` 都要抓;裸 `submit(` 不收(见 docstring)。
FORBIDDEN_PATTERNS = [
    (re.compile(r"->\s*submit\s*[<(]"), "指针形式提交 ->submit(...)"),
    (re.compile(r"\.\s*submit\s*[<(]"), "对象形式提交 .submit(...)"),
    (re.compile(r"\b(?:vk)?QueueSubmit\w*\b"), "裸队列提交 QueueSubmit / vkQueueSubmit"),
    (re.compile(r"\bqueue_submit\s*\("), "队列提交封装 queue_submit(...)"),
]

CODE_EXTS = {".h", ".hpp", ".cpp", ".cxx", ".cc", ".inl", ".ipp"}
SKIP_DIRS = {"build", ".git", "vendor", "out", ".vs", ".cache", "third_party"}
PRESENT_TOKEN = "present"


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
        s = re.sub(r'"(\\.|[^"\\])*"', '""', s)
        out.append(s)
    return out


def iter_code_files(base):
    for dirpath, dirnames, filenames in os.walk(base):
        dirnames[:] = sorted(d for d in dirnames if d not in SKIP_DIRS)
        for name in sorted(filenames):
            if os.path.splitext(name)[1].lower() in CODE_EXTS:
                yield os.path.join(dirpath, name)


def is_present_path(rel):
    """呈递路径:basename 含 `present`(不区分大小写)。只看 basename,见 docstring。"""
    return PRESENT_TOKEN in os.path.basename(rel).lower()


def collect_files(root):
    victims = []
    for sub in ("src/ui", "tests"):
        base = os.path.join(root, *sub.split("/"))
        if not os.path.isdir(base):
            continue
        for path in iter_code_files(base):
            rel = os.path.relpath(path, root).replace(os.sep, "/").lower()
            victims.append((path, rel))
    return victims


def main(argv):
    # Windows 控制台默认 GBK:中文摘要按 GBK 编不出来会抛 UnicodeEncodeError 退出码 1,
    # 被误读成「违规」。统一改成 UTF-8 + replace —— 编不出的字降级成 '?',绝不因此红。
    try:
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    except (AttributeError, OSError):  # 非 TextIOWrapper(被重定向)时尽力而为
        pass

    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--root", default=None, help="MagicBlocks 根目录")
    args = ap.parse_args(argv)

    here = os.path.dirname(os.path.abspath(__file__))
    root = os.path.abspath(args.root or os.path.join(os.path.dirname(here), "MagicBlocks"))
    if not os.path.isdir(root):
        print("%s 目录不存在: %s" % (TAG, root))
        return 2

    violations = []
    scanned = 0
    scanned_present = 0
    exempt = 0

    for path, rel in collect_files(root):
        present = is_present_path(rel)
        scanned += 1
        if present:
            scanned_present += 1
        try:
            with open(path, "r", encoding="utf-8", errors="replace") as f:
                code_lines = strip_comments_and_strings(f.read().splitlines())
        except OSError as exc:  # 读不到即报错,不静默跳过
            print("%s 读取失败: %s (%s)" % (TAG, path, exc))
            return 2

        for lineno, code in enumerate(code_lines, 1):
            if not code.strip():
                continue
            for pat, why in FORBIDDEN_PATTERNS:
                if not pat.search(code):
                    continue
                if present:
                    exempt += 1  # 呈递路径内允许(帧提交点),只计数不报
                else:
                    violations.append("%s:%d  PresentPaths 之外出现提交调用: %s  ->  %s"
                                      % (rel, lineno, why, code.strip()))

    if violations:
        print("%s 违规 %d 处(单次提交):" % (TAG, len(violations)))
        for v in violations:
            print("  " + v)
        print("  提示:场景 pass / UI pass / 上屏必须在同一 CommandBuffer 同一次 submit 完成"
              "(spec §5.1 P3 / §5.4);提交只允许出现在 `*present*` 文件中。")
        return 1
    print("%s OK 扫描 %d 个 UI 文件,呈递路径 %d 个文件内 %d 处提交(允许),其余文件零提交"
          % (TAG, scanned, scanned_present, exempt))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
