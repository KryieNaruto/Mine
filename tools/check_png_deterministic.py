#!/usr/bin/env python3
"""断言 CLI 子命令的产出是**确定性的**(spec §11 命令回放的前提)。

做法:同样的参数跑两次,两次 PNG 必须逐字节相同。

为什么这件事必须自动化
----------------------
"不读时钟/不读随机源"是一条**很容易在重构中悄悄破掉**的性质:
某个字段时间戳忘了从事件流传入、某处顺手加了 `std::chrono::now()`,
或者遍历顺序依赖了 `unordered_map` 的桶布局 —— 这些都不会报错,
只会让 golden 在某次提交后默默变化。

单元测试抓不到它(单测只跑一次,没有"两次比对"这回事),
所以这里用 CLI 跑两遍。

用法:
  python tools/check_png_deterministic.py --cli <exe> --sub gizmo \
      --width 320 --height 240 --outdir <dir>
退出码: 0 确定性 / 1 不确定 或 运行失败 / 2 用法错误。
"""

import argparse
import hashlib
import os
import subprocess
import sys


def sha256_of(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 16), b""):
            h.update(chunk)
    return h.hexdigest()


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--cli", required=True)
    ap.add_argument("--sub", required=True)
    ap.add_argument("--width", type=int, default=320)
    ap.add_argument("--height", type=int, default=240)
    ap.add_argument("--outdir", required=True)
    args = ap.parse_args(argv)

    if not os.path.isfile(args.cli):
        print(f"错误: 找不到 CLI {args.cli}", file=sys.stderr)
        return 2
    os.makedirs(args.outdir, exist_ok=True)

    hashes = []
    for run in (1, 2):
        out = os.path.join(args.outdir, f"determinism_{args.sub}_{run}.png")
        if os.path.exists(out):
            os.remove(out)
        cmd = [
            args.cli,
            args.sub,
            "--width",
            str(args.width),
            "--height",
            str(args.height),
            "--output",
            out,
        ]
        # 用 bytes 捕获而不是 text=True:CLI 输出是 UTF-8 中文,
        # 而 Windows 上 subprocess 的默认文本解码是 GBK,会直接抛
        # UnicodeDecodeError(实测踩过)。只在需要打印时手动解码。
        proc = subprocess.run(cmd, capture_output=True)
        if proc.returncode != 0:
            print(f"错误: 第 {run} 次运行失败(退出码 {proc.returncode})", file=sys.stderr)
            sys.stderr.write(proc.stdout.decode("utf-8", errors="replace"))
            sys.stderr.write(proc.stderr.decode("utf-8", errors="replace"))
            return 1
        if not os.path.isfile(out):
            print(f"错误: 第 {run} 次运行没有产出 {out}", file=sys.stderr)
            return 1
        hashes.append(sha256_of(out))

    if hashes[0] != hashes[1]:
        print(f"失败: '{args.sub}' 不确定 —— 同样的参数两次产出不同")
        print(f"  第 1 次: {hashes[0]}")
        print(f"  第 2 次: {hashes[1]}")
        print("  常见原因: 某处读了时钟/随机源,或遍历顺序依赖了哈希表布局。")
        return 1

    print(f"通过: '{args.sub}' 确定性(scha256={hashes[0][:16]}...)")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
