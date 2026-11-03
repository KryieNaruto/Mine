#!/usr/bin/env python3
"""门控:既有 tools/tests 全绿(spec §7 check_deps_tests)。

本次 MagicBlocks/L0 改动不得破坏工作空间 `tools/tests` 的既有行为。脚本:
  1. 用 `python -m unittest discover -s tools/tests -p "test_*.py"` 跑全部用例;
  2. 断言发现到的用例文件数 **精确等于** 预期(默认 12),防"用例被删/被改名"造成的假绿;
  3. 任一失败 / 错误 / 跳过 > 0 即判失败(测试门只认 0 失败 0 跳过)。

用法: python tools/check_deps_tests.py [--expect-files 12]
退出码: 0 通过,1 失败(有失败/跳过/用例文件数不符),2 用法/环境错误。
"""

import argparse
import os
import re
import subprocess
import sys


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--expect-files", type=int, default=12,
                    help="tools/tests 期望的 test_*.py 文件数(精确相等,防止用例被静默删除)")
    args = ap.parse_args(argv)

    here = os.path.dirname(os.path.abspath(__file__))
    tests_dir = os.path.join(here, "tests")
    if not os.path.isdir(tests_dir):
        print("[deps-tests] 目录不存在: %s" % tests_dir)
        return 2

    files = sorted(n for n in os.listdir(tests_dir)
                   if n.startswith("test_") and n.endswith(".py"))
    # 精确相等断言(第 3 轮审阅指出:只判 >= 时删掉一个用例文件仍会绿)。
    if len(files) != args.expect_files:
        print("[deps-tests] FAIL 用例文件数 %d != 预期 %d:%s"
              % (len(files), args.expect_files, files))
        return 1
    print("[deps-tests] 发现 %d 个用例文件" % len(files))

    cmd = [sys.executable, "-m", "unittest", "discover", "-s", tests_dir, "-p", "test_*.py", "-v"]
    # 子进程 cwd 用工作空间根,保证用例里的相对路径假设成立
    root = os.path.dirname(here)
    proc = subprocess.run(cmd, cwd=root, capture_output=True, text=True, encoding="utf-8", errors="replace")
    out = (proc.stdout or "") + (proc.stderr or "")

    m = re.search(r"^Ran (\d+) tests?", out, re.M)
    ran = int(m.group(1)) if m else 0
    failed = 0
    skipped = 0
    mf = re.search(r"^FAILED \((.*)\)$", out, re.M)
    if mf:
        fm = re.search(r"failures=(\d+)", mf.group(1))
        em = re.search(r"errors=(\d+)", mf.group(1))
        failed = (int(fm.group(1)) if fm else 0) + (int(em.group(1)) if em else 0)
    ms = re.search(r"skipped=(\d+)", out)
    if ms:
        skipped = int(ms.group(1))

    tail = "\n".join(out.strip().splitlines()[-25:])
    if proc.returncode != 0 or failed > 0 or skipped > 0 or ran == 0:
        print("[deps-tests] FAIL rc=%d ran=%d failed=%d skipped=%d" % (proc.returncode, ran, failed, skipped))
        print(tail)
        return 1

    print("[deps-tests] OK ran=%d failed=0 skipped=0" % ran)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
