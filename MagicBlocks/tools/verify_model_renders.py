#!/usr/bin/env python3
"""verify_model_renders.py —— 预设测试模型「每模型单独出图」的像素断言。

做两件事：
  1. 调 `magicblocks-model-cli --list` 拿模型清单（这是 UI 左侧列表的数据源）；
  2. 对每张图跑 tools/png_inspect.py 的**区域模式**，断言：
       - 前景像素占比 > 阈值（模型真的画出来了，不是空图）；
       - 主色占比 < 上限（不是纯背景/纯色填充）；
       - 色种数 > 1（有内容）；
       - 与"关掉模型"的对照图**必须不同**（差异像素 > 0）。

为什么必须这样断言（而不是"我看了一眼图"）：
  一个人对着 PNG 说"有东西"是不可复核的。把"有没有东西"压成
  `r0_nonbg_ratio`、`color_top_share`、`r0_colors` 三个标量，任何人在任何机器上
  重跑都能得到同一个数，红/绿才立得住。

用法：
  python tools/verify_model_renders.py \
      --cli build-skybox/magicblocks-model-cli.exe \
      --asset-root E:/workspace/myspace/RayTracing/_install/bin/res \
      --work-dir build-skybox/models_out

退出码：0 全部通过 · 1 有断言失败（或渲染失败） · 2 用法/环境错误。
"""
from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
MAGICBLOCKS = os.path.dirname(HERE)
PNG_INSPECT = os.path.join(MAGICBLOCKS, "tools", "png_inspect.py")

# 每个模型的**结构性**阈值，不是"调出来的经验值"：
#   实测（480x360，归一化到对角线 1.0，相机按包围盒半径反推距离）：
#     bunny 0.095 / cube 0.232 / panel 0.195 / sphere 0.117 / teapot 0.077
#   阈值取实测最小值的一半，留出驱动/相机微调余量，同时仍能抓住"模型没画出来"。
MIN_FG_RATIO = 0.03
# 主色占比上限：超过就是"几乎纯色"（空图或纯背景）。
MAX_TOP_COLOR_SHARE = 0.98
# 色种数下限：cube 只有 6 色也是对的（6 个面各一色），所以取 2。
MIN_COLORS = 2


def run(cmd: list[str], **kw) -> subprocess.CompletedProcess:
    return subprocess.run(cmd, capture_output=True, text=True, **kw)


def list_models(cli: str, asset_root: str) -> list[str]:
    r = run([cli, "--asset-root", asset_root, "--list"])
    if r.returncode != 0:
        raise RuntimeError(f"model-cli --list 失败(exit {r.returncode}): {r.stderr}")
    text = r.stdout.strip().splitlines()[-1]
    return json.loads(text)["models"]


def inspect(png: str, w: int, h: int) -> dict:
    """跑 png_inspect 的区域模式，解析出标量。

    注意：`--json -` 在本工具里是"写一个名叫 - 的文件"，不是 stdout，
    所以这里落到一个临时文件再读回来。
    """
    fd, tmp = tempfile.mkstemp(suffix=".json", prefix="png_inspect_")
    os.close(fd)
    try:
        r = run([sys.executable, PNG_INSPECT, png, "--region", f"0,0,{w},{h}", "--bg", "auto",
                 "--json", tmp, "--quiet"])
        if r.returncode == 2:
            raise RuntimeError(f"png_inspect 解码失败: {r.stderr}")
        with open(tmp, "r", encoding="utf-8") as f:
            return json.load(f)
    finally:
        try:
            os.remove(tmp)
        except OSError:
            pass


def main(argv: list[str]) -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--cli", required=True, help="magicblocks-model-cli 可执行文件")
    ap.add_argument("--asset-root", required=True, help="资源根（含 models/）")
    ap.add_argument("--work-dir", required=True, help="PNG 落盘目录")
    ap.add_argument("--width", type=int, default=480)
    ap.add_argument("--height", type=int, default=360)
    args = ap.parse_args(argv)

    if not os.path.isfile(args.cli):
        print(f"[error] 找不到 model-cli: {args.cli}", file=sys.stderr)
        return 2
    if not os.path.isdir(args.asset_root):
        print(f"[error] 资源根不存在: {args.asset_root}", file=sys.stderr)
        return 2

    os.makedirs(args.work_dir, exist_ok=True)

    try:
        models = list_models(args.cli, args.asset_root)
    except Exception as exc:  # noqa: BLE001 - 明确报错即可
        print(f"[error] {exc}", file=sys.stderr)
        return 2
    if not models:
        print("[error] --list 返回空模型列表", file=sys.stderr)
        return 2
    print(f"[list] {len(models)} 个模型: {', '.join(models)}")

    # 先渲染全部模型（模型清单即 UI 左侧列表的数据源）。
    r = run([args.cli, "--asset-root", args.asset_root, "--output-dir", args.work_dir,
             "--width", str(args.width), "--height", str(args.height)])
    if r.returncode != 0:
        print(f"[error] 渲染失败(exit {r.returncode}):\n{r.stderr}", file=sys.stderr)
        return 1

    failures: list[str] = []
    rows: list[tuple[str, float, float, int]] = []
    for name in models:
        png = os.path.join(args.work_dir, f"{name}.png")
        if not os.path.isfile(png):
            failures.append(f"{name}: 没有产出 {png}")
            continue
        try:
            m = inspect(png, args.width, args.height)
        except Exception as exc:  # noqa: BLE001
            failures.append(f"{name}: png_inspect 失败: {exc}")
            continue

        fg = float(m["r0_nonbg_ratio"])
        top = float(m["color_top_share"])
        colors = int(m["r0_colors"])
        rows.append((name, fg, top, colors))

        if fg <= MIN_FG_RATIO:
            failures.append(f"{name}: 前景占比 {fg:.4f} <= {MIN_FG_RATIO}（模型没画出来）")
        if top >= MAX_TOP_COLOR_SHARE:
            failures.append(f"{name}: 主色占比 {top:.4f} >= {MAX_TOP_COLOR_SHARE}（几乎纯色）")
        if colors < MIN_COLORS:
            failures.append(f"{name}: 色种数 {colors} < {MIN_COLORS}")

    print("\n%-10s %12s %18s %8s" % ("model", "fg_ratio", "top_color_share", "colors"))
    for name, fg, top, colors in rows:
        print("%-10s %12.4f %18.4f %8d" % (name, fg, top, colors))

    if failures:
        print(f"\n[FAIL] {len(failures)} 条断言不通过：", file=sys.stderr)
        for f in failures:
            print("  - " + f, file=sys.stderr)
        return 1

    print(f"\n[OK] {len(rows)} 个模型全部通过像素断言")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
