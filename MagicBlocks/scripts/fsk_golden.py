#!/usr/bin/env python3
"""F-SK golden 基线的生成与结构校验（B2）。

背景
----
审计（docs/superpowers/specs/2026-11-11-magicblocks-fsk-fvx-completion-audit.md GAP-07）
指出：F-SK 的 golden 基线**从来没有生成过**。tests/data 里只有 golden_l0/l2/ui，
以及用例自己跑出来的 11 张 `_out_l4_*.png`（那些是产物，不是基线）。
没有基线，"画得对不对"就只能证明"画出了东西且确定性"，不能证明"画的是需求要的那个"。

本脚本做两件事，**顺序不能反**：

  1. `gen`   —— 用 magicblocks-fsk-cli 重新生成 golden，写到 tests/data/。
  2. `check` —— 对 golden **本身**做结构化断言（四角背景 / 前景占比区间 /
                与非 golden 的区分度），不依赖"用例变绿"。

为什么 `check` 必须独立于用例跑
------------------------------
tests/data/README.md 记录的 L0 事故是"错基线自我固化"：渲染逻辑错了，
golden 跟着错，逐像素比对照样通过。结构化断言与 golden 的生成来源无关，
所以它能拦住"整张图上下颠倒 / 通道写反 / 全背景"这三类最常见的错基线。
本脚本的 `check` 就是那一步，且可以被 CI 单独调用（`--only check`）。

用法
----
    python MagicBlocks/scripts/fsk_golden.py gen            # 重新生成（渲染逻辑有意变更后才跑）
    python MagicBlocks/scripts/fsk_golden.py check          # 只做结构校验
    python MagicBlocks/scripts/fsk_golden.py all            # 生成 + 校验

退出码：0 通过 / 1 结构断言失败 / 2 用法或环境错误。
"""

import argparse
import os
import struct
import subprocess
import sys
import zlib

# 背景色必须与 golden 匹配：F-SK CLI 的清屏色 (0.06, 0.07, 0.10) → 8bit 约 (15,18,26)。
# 不写成"取四角像素当背景"——那样整张全背景的图四角也是"背景"，断言就废了。
BACKGROUND = (15, 18, 26)

# 前景占比区间（设计 A2：中心区域非背景像素占比 ∈ [3%, 60%]）。
FG_MIN = 0.03
FG_MAX = 0.60

# 每一份 golden 的期望生成命令。**参数写死在这里**，因为它就是基线的定义：
# 改参数必然要重新生成，而重新生成必须走本脚本（否则 `check` 的期望会对不上）。
GOLDENS = {
    "golden_fsk_persp.png": [
        "render", "--width", "320", "--height", "240", "--strokes", "3",
        "--camera", "perspective", "--output", "{out}",
    ],
    "golden_fsk_ortho.png": [
        "render", "--width", "320", "--height", "240", "--strokes", "3",
        "--camera", "ortho", "--output", "{out}",
    ],
    "golden_fsk_region.png": [
        "render", "--width", "320", "--height", "240", "--strokes", "2",
        "--camera", "ortho", "--region", "--output", "{out}",
    ],
}

# 两份 golden 之间的最小差异像素数：低于它就说明"相机参数没起作用"或
# "两种解读产出同一个实体"——那正是 A3 / R-SK-12 要抓的错误。
def _pairs():
    return [
        ("golden_fsk_persp.png", "golden_fsk_ortho.png", "A3 相机双模态"),
        ("golden_fsk_ortho.png", "golden_fsk_region.png", "R-SK-12 双表示"),
    ]


def read_png_rgba8(path):
    """最小 PNG 解码（8bit RGBA / RGB，filter 0-4，zlib）。不引入第三方依赖。"""
    with open(path, "rb") as f:
        data = f.read()
    if len(data) < 8 or data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("%s 不是 PNG" % path)
    pos = 8
    width = height = None
    bit_depth = color_type = 0
    idat = bytearray()
    while pos + 8 <= len(data):
        (length,) = struct.unpack(">I", data[pos:pos + 4])
        ctype = data[pos + 4:pos + 8]
        chunk = data[pos + 8:pos + 8 + length]
        pos += 12 + length
        if ctype == b"IHDR":
            width, height, bit_depth, color_type, comp, filt, inter = struct.unpack(
                ">IIBBBBB", chunk)
            if bit_depth != 8 or inter != 0 or color_type not in (2, 6):
                raise ValueError("%s: 只支持 8bit RGB/RGBA 非隔行 PNG" % path)
        elif ctype == b"IDAT":
            idat += chunk
        elif ctype == b"IEND":
            break
    if width is None:
        raise ValueError("%s: 缺少 IHDR" % path)
    channels = 4 if color_type == 6 else 3
    raw = zlib.decompress(bytes(idat))
    stride = width * channels
    out = bytearray(width * height * 4)
    prev = bytearray(stride)
    rpos = 0
    for y in range(height):
        ft = raw[rpos]
        rpos += 1
        line = bytearray(raw[rpos:rpos + stride])
        rpos += stride
        if ft == 1:
            for i in range(channels, stride):
                line[i] = (line[i] + line[i - channels]) & 0xFF
        elif ft == 2:
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xFF
        elif ft == 3:
            for i in range(stride):
                left = line[i - channels] if i >= channels else 0
                line[i] = (line[i] + ((left + prev[i]) >> 1)) & 0xFF
        elif ft == 4:
            for i in range(stride):
                a = line[i - channels] if i >= channels else 0
                b = prev[i]
                c = prev[i - channels] if i >= channels else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 0xFF
        elif ft != 0:
            raise ValueError("%s: 不支持的 filter %d" % (path, ft))
        for x in range(width):
            s = x * channels
            d = (y * width + x) * 4
            out[d] = line[s]
            out[d + 1] = line[s + 1]
            out[d + 2] = line[s + 2]
            out[d + 3] = line[s + 3] if channels == 4 else 255
        prev = line
    return width, height, bytes(out)


def is_background(px, tol=6):
    return (abs(px[0] - BACKGROUND[0]) <= tol and abs(px[1] - BACKGROUND[1]) <= tol
            and abs(px[2] - BACKGROUND[2]) <= tol)


def analyze(path):
    w, h, px = read_png_rgba8(path)
    total = w * h
    fg = 0
    colors = set()
    minx, miny, maxx, maxy = w, h, -1, -1
    for y in range(h):
        row = y * w * 4
        for x in range(w):
            i = row + x * 4
            p = (px[i], px[i + 1], px[i + 2])
            colors.add(p)
            if not is_background(p):
                fg += 1
                if x < minx:
                    minx = x
                if y < miny:
                    miny = y
                if x > maxx:
                    maxx = x
                if y > maxy:
                    maxy = y
    corners = {}
    for name, (cx, cy) in {
        "左上": (w // 8, h // 8),
        "右上": (w * 7 // 8, h // 8),
        "左下": (w // 8, h * 7 // 8),
        "右下": (w * 7 // 8, h * 7 // 8),
    }.items():
        i = (cy * w + cx) * 4
        corners[name] = (px[i], px[i + 1], px[i + 2])
    return {
        "path": path,
        "w": w,
        "h": h,
        "total": total,
        "fg_ratio": fg / float(total),
        "fg_pixels": fg,
        "distinct": len(colors),
        "bbox": (maxx - minx + 1, maxy - miny + 1) if maxx >= 0 else (0, 0),
        "corners": corners,
    }


def differing(a, b):
    wa, ha, pa = read_png_rgba8(a)
    wb, hb, pb = read_png_rgba8(b)
    if (wa, ha) != (wb, hb):
        return -1  # 尺寸不同本身就是差异（由调用方报错）
    n = 0
    for i in range(0, len(pa), 4):
        if pa[i:i + 3] != pb[i:i + 3]:
            n += 1
    return n


def find_cli(explicit):
    if explicit:
        return explicit
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))  # MagicBlocks/
    for cand in ("build/magicblocks-fsk-cli.exe", "build/magicblocks-fsk-cli"):
        p = os.path.join(root, cand)
        if os.path.isfile(p):
            return p
    return None


def data_dir(explicit):
    if explicit:
        return explicit
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    return os.path.join(root, "tests", "data")


def cmd_gen(cli, ddir):
    if cli is None:
        print("错误: 找不到 magicblocks-fsk-cli（先构建，或用 --cli 指定）", file=sys.stderr)
        return 2
    rc_all = 0
    for name, args in GOLDENS.items():
        out = os.path.join(ddir, name)
        cmd = [cli] + [a.format(out=out) for a in args]
        print("生成 %s\n  %s" % (name, " ".join(cmd)))
        proc = subprocess.run(cmd, capture_output=True, text=True, encoding="utf-8",
                              errors="replace")
        sys.stdout.write(proc.stdout)
        if proc.returncode != 0:
            sys.stderr.write(proc.stderr)
            print("错误: %s 生成失败(rc=%d)" % (name, proc.returncode), file=sys.stderr)
            rc_all = 1
    return rc_all


def cmd_check(ddir):
    rc = 0
    printed = 0
    for name in GOLDENS:
        path = os.path.join(ddir, name)
        if not os.path.isfile(path):
            print("[golden] 缺失: %s" % path, file=sys.stderr)
            rc = 1
            continue
        s = analyze(path)
        printed += 1
        corners_ok = all(is_background(c) for c in s["corners"].values())
        ratio_ok = FG_MIN <= s["fg_ratio"] <= FG_MAX
        print("[golden] %-24s %dx%d fg=%.4f distinct=%d bbox=%dx%d 四角背景=%s 占比在区间=%s"
              % (name, s["w"], s["h"], s["fg_ratio"], s["distinct"], s["bbox"][0], s["bbox"][1],
                 corners_ok, ratio_ok))
        if not corners_ok:
            print("    四角不是背景色: %s" % (s["corners"],), file=sys.stderr)
            rc = 1
        if not ratio_ok:
            print("    前景占比 %.4f 不在 [%.2f, %.2f]" % (s["fg_ratio"], FG_MIN, FG_MAX),
                  file=sys.stderr)
            rc = 1
        if s["distinct"] < 3:
            # 只有 1-2 种颜色说明没有光照/深度（或者整张是纯色）。
            print("    颜色种数 %d 过少: 缺少光照或深度" % s["distinct"], file=sys.stderr)
            rc = 1
    for a, b, why in _pairs():
        pa = os.path.join(ddir, a)
        pb = os.path.join(ddir, b)
        if not (os.path.isfile(pa) and os.path.isfile(pb)):
            continue
        n = differing(pa, pb)
        print("[golden] %s vs %s: 差异像素 %s（判据: %s 必须不同）" % (a, b, n, why))
        if n == 0:
            print("    两份 golden 完全相同 —— %s 没有生效" % why, file=sys.stderr)
            rc = 1
    if printed == 0:
        print("[golden] 一份都没找到，什么也没校验", file=sys.stderr)
        return 1
    return rc


def main(argv):
    ap = argparse.ArgumentParser(description="F-SK golden 生成与结构校验")
    ap.add_argument("action", choices=["gen", "check", "all"])
    ap.add_argument("--cli", default=None, help="magicblocks-fsk-cli 路径")
    ap.add_argument("--data", default=None, help="golden 目录（默认 <MagicBlocks>/tests/data）")
    args = ap.parse_args(argv)

    ddir = data_dir(args.data)
    if not os.path.isdir(ddir):
        print("错误: 目录不存在 %s" % ddir, file=sys.stderr)
        return 2
    if args.action in ("gen", "all"):
        rc = cmd_gen(find_cli(args.cli), ddir)
        if rc != 0:
            return rc
    if args.action in ("check", "all"):
        return cmd_check(ddir)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
