#!/usr/bin/env python3
"""UIG1 门控:上屏零回读(spec §5.4 / §5.5 / §10 验收门 1)。

断言 UI 消费层**呈递路径**与**全 UI 层**都不得出现 CPU 回读渲染结果的调用。

== 扫描范围 ==
  1. `MagicBlocks/src/ui/**` —— 全部 UI 层源码(不论文件名)。
  2. `MagicBlocks/tests/**` —— 只纳入**文件名含 `ui`**(不区分大小写)的文件,
     例如 `tests/ui_presenter_test.cpp`、`tests/UI_Smoke.cpp`。
     理由:tests/ 下还有 SDK 的测试(如 `magicblocks_test.cpp`),SDK 侧的离屏
     golden 读回是合法的,不能把整个 tests/ 当成 UI 层;只认文件名里的 `ui` 标记,
     口径与 spec「扫描 UI 层与 Presenter」一致,且不依赖目录同名猜测。
     注意 `build` / `.git` / `vendor` / `out` / `.vs` 等目录整体跳过(与既有脚本一致)。
  源文件后缀:`.h .hpp .cpp .cxx .cc .inl .ipp`(与 check_no_raw_alloc.py 相同)。

== 两种路径 ==
  A. **呈递路径**:文件路径(相对 root、已转小写、正斜杠)里**含 `present`**,
     例如 `src/ui/presenter.cpp`、`src/ui/presenter_frame.cpp`、`src/ui/present/presenter.h`、
     `tests/ui_present_smoke.cpp`。呈递路径上出现下列任一即**违规**。
  B. **全 UI 层**:上面扫描范围内的全部文件。出现下列任一即**违规**。

== 关键字(先剥注释与字符串字面量,只认代码行)==
  1. `readback_rgba8`  —— 读回接口直呼,属"CPU 读回任何渲染结果用于 UI"(§5.4 第 1 行)
  2. `CopyImageToBuffer` / `vkCmdCopyImageToBuffer` —— GPU→CPU 拷贝命令
  3. `->map(` / `.map(` —— 指针/对象形式的 map 读写(含非 coherent 内存)
     (故意不收裸 `map(`:`std::map(`、`unordered_map(` 会误报;`vkMapMemory(` 单独成条)
  4. `vkMapMemory` / `vmaMapMemory`
  5. `present_to_cpu` —— 明确的上屏回读通道名
  6. `readback`        —— **宽松匹配**:标识符里任意位置出现 `readback` 即算
     (`readback_*`、`*_readback`、`doReadback`...)。宽松是故意的:§5.4 的硬约束是
     "不要有这个能力",名字换皮不该放过;代价是可能误报**无关**词的拼写,但 `readback`
     不是任何常见 C++ 标识符的子串,实测噪声为零。若将来真要放行,必须改本门控口径,
     **不允许**用注释/宏记号免检(见下)。

== 豁免(仅此一条,窄且写死)==
  路径里含 `offscreen`(不区分大小写)的文件豁免。spec §5.5 明文:离屏路径
  `TargetMode::Offscreen` 渲染到离屏 image → `save_png`,只用于 CLI/CI/golden/命令回放,
  **上屏路径不经过它** —— 读回本来就发生在那里,所以它必须能编译通过。
  实际形态:`src/ui/offscreen/**` 整目录,以及 `src/ui/**` 下任何路径段含 `offscreen` 的
  文件(如 `src/ui/presenter_offscreen.cpp`、`src/ui/offscreen_capture.h`)。
  这条豁免**由路径决定、与文件内容无关**:注释里写个记号、`#define` 个宏、写句
  `// allow-readback` 都**不会**免检 —— 门控只看路径。想扩豁免范围,只能改这个脚本
  (改路径规则),那是一次需要审阅的改动,不是源码里加一行注释就能做的。

注释与字符串里的示例写法不计(只认代码行):`// 不要 vkCmdCopyImageToBuffer(...)`、
`"readback_rgba8("` 这类文本不算违规。

用法: python tools/check_ui_no_readback.py [--root MagicBlocks]
退出码: 0 通过,1 违规,2 用法/环境错误。

已知盲区(写在这里免得被误认为"门控绿 = 绝对安全"):
  - 只做词法匹配,不做语义分析:经函数指针/宏间接转发、或换名封装
    (`gpu->FetchPixels(...)`)的回读抓不到;
  - `vkCopy*` / `vkCmdBlitImage` 之类没有 map 也不是 CopyImageToBuffer 的
    「多一次全屏 copy」形态不在本条门控范围(P2 零额外 copy 需另立统计/门控);
  - 扫描范围只覆盖 `src/ui` 与 `tests` 里文件名含 `ui` 的文件;若上屏代码被挪到
    别处(例如 `src/app/`),本门控不会看它。
"""

import argparse
import os
import re
import sys

TAG = "[UIG1]"

# 关键字与判定依据(逐条注释见模块 docstring)。
FORBIDDEN_PATTERNS = [
    (re.compile(r"\breadback_rgba8\b"), "读回接口 readback_rgba8"),
    (re.compile(r"\bvkCmdCopyImageToBuffer\b"), "GPU→CPU 拷贝命令 vkCmdCopyImageToBuffer"),
    (re.compile(r"\bCopyImageToBuffer\b"), "GPU→CPU 拷贝命令 CopyImageToBuffer"),
    (re.compile(r"(?:->|\.)\s*map\s*\("), "map 读写指针(->map( / .map()"),
    (re.compile(r"\bvkMapMemory\b"), "裸 vkMapMemory 映射显存"),
    (re.compile(r"\bvmaMapMemory\b"), "VMA vmaMapMemory 映射显存"),
    (re.compile(r"\bpresent_to_cpu\b"), "上屏回读通道 present_to_cpu"),
    # 宽松匹配:标识符内任意位置含 readback。`\bre[aA]dback` 是多余的小心,
    # 大小写分别用 re.I 统一处理。
    (re.compile(r"readback", re.IGNORECASE), "回读(readback 宽松匹配)"),
]

CODE_EXTS = {".h", ".hpp", ".cpp", ".cxx", ".cc", ".inl", ".ipp"}
SKIP_DIRS = {"build", ".git", "vendor", "out", ".vs", ".cache", "third_party"}
EXEMPT_MARK = "offscreen"


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
        # 去掉字符串字面量,避免 "vkMapMemory" 这类注释性文本误判
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
    """呈递路径:相对路径里含 `present`(不区分大小写)。"""
    return "present" in rel.lower()


def is_exempt(rel):
    """唯一豁免:路径含 `offscreen`(spec §5.5,离屏 golden/CI 路径允许读回)。

    只看路径、不看内容 —— 注释/宏记号不可能触发豁免。
    """
    return EXEMPT_MARK in rel.lower()


def collect_files(root):
    """返回 [(绝对路径, 相对路径)];tests/ 下只收文件名含 `ui` 的文件。"""
    victims = []
    ui_dir = os.path.join(root, "src", "ui")
    if os.path.isdir(ui_dir):
        for path in iter_code_files(ui_dir):
            # 相对路径统一小写 + 正斜杠:呈递/豁免都是大小写不敏感的子串判定
            victims.append((path, os.path.relpath(path, root).replace(os.sep, "/").lower()))

    tests_dir = os.path.join(root, "tests")
    if os.path.isdir(tests_dir):
        for path in iter_code_files(tests_dir):
            if "ui" in os.path.basename(path).lower():
                victims.append((path, os.path.relpath(path, root).replace(os.sep, "/").lower()))
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
        exempted = is_exempt(rel)
        if exempted:
            exempt += 1  # 豁免文件不读内容,只计数
            continue
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
            # 一行命中多条只报最具体的一条:`readback_rgba8(` 同时会被宽松的 `readback`
            # 命中,重复报会让「违规 N 处」里的 N 虚高,也不好定位。
            for pat, why in FORBIDDEN_PATTERNS:
                if pat.search(code):
                    where = "呈递路径" if present else "全 UI 层"
                    violations.append("%s:%d  [%s] %s  ->  %s"
                                      % (rel, lineno, where, why, code.strip()))
                    break

    if violations:
        print("%s 违规 %d 处(上屏零回读):" % (TAG, len(violations)))
        for v in violations:
            print("  " + v)
        print("  提示:呈递路径不得有 CPU 回读能力(spec §5.4);只有路径含 `offscreen` 的"
              "离屏文件豁免(§5.5),注释/宏记号不免检。")
        return 1
    print("%s OK 扫描 %d 个 UI 文件(其中呈递路径 %d 个),豁免 %d 个 offscreen 文件,零回读"
          % (TAG, scanned, scanned_present, exempt))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
