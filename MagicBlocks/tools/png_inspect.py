#!/usr/bin/env python3
"""png_inspect —— 机器可读的 PNG 像素分析器(纯标准库,不依赖 PIL)。

成立的理由:
  审阅者(模型/CI)看不了图片。"图长什么样"只能变成数字才可核对。
  本工具把一张图压成**可断言的标量**:主色占比、区域内非背景占比、
  亮度跨度、差异像素数与包围盒、边沿过渡色像素数。

能力(均可组合):
  --info         尺寸 / 通道数 / 位深 / 颜色类型 / 是否有 alpha(任何模式都先打)
  --colors N     主色统计 top-N(量化到 8 bit),带占比与总颜色种数;默认 8,总是执行
  --region R     指定区域分析(非背景占比 / 颜色种数 / 亮度跨度 / 过渡色像素)
                 R 形如 x,y,w,h;可重复给出多个 --region
  --edge-metric  边沿平滑度(3x3 邻域过渡像素数)—— 判断 MSAA/抗锯齿是否生效
                 --band x,y,w,h 把统计范围**限定到场景区**(宽度必须取满图宽)
                 采样区内 edge_foreground_pixels == 0 时**默认退出码 1**:
                 那表示"本次统计无效",不是"没有边缘"(见下方 --allow-empty-sample)
  --diff B       与另一张图逐像素对比:差异数 / 差异包围盒 / 导出差异图

背景判定(--bg):
  auto(默认) = 左上角像素;也可以显式给 r,g,b。`corner:` 前缀可用。
  非背景判定带容差 --bg-tol(默认 0,即严格逐字节相等)。
  前景可由 --fg r,g,b 显式给出;不给时取"区域内离背景最远的颜色"。

用法示例:
  python tools/png_inspect.py shot.png --info --colors 8 --edge-metric
  python tools/png_inspect.py shot.png --region 0,0,320,60 --region 0,260,320,380
  # 场景区 = HUD 下沿到画面底部,宽度取满:
  python tools/png_inspect.py shot.png --edge-metric --band 0,260,2160,1180
  python tools/png_inspect.py shot.png --edge-metric --assert edge_transition_pixels>500
  python tools/png_inspect.py a.png --diff b.png --diff-out d.png --assert diff_pixels==0

断言(--assert,可重复):不满足时退出码 1。
  NAME OP VALUE     OP ∈ > >= < <= == != ;NAME 取报告中任一标量键(见下)
  edge_transition_pixels>0 这类简写等价于 NAME>VALUE;裸 NAME 表示该值必须非 0。

报告里的标量键(--list-metrics 列出全部,机器可直接解析 "key = value" 行):
  width height bit_depth color_type bpp bytes_per_channel channels(1/2/3/4)
  has_alpha alpha_min alpha_max has_binary_alpha
  distinct_colors color_top_share color_00_share color_01_share ...(最大单色占比 0..1)
  区域(<idx> 从 0 起):
    r<idx>_x/y/w/h r<idx>_nonbg_ratio r<idx>_colors(=distinct_colors)
    r<idx>_contrast_luma r<idx>_mean_luma/min_luma/max_luma(0..255)
    r<idx>_transition_pixels r<idx>_has_transition r<idx>_fg_r/g/b
  edge 模式:edge_transition_pixels edge_neighbor_transition edge_ratio
            edge_neighbor_ratio edge_background_pixels edge_foreground_pixels
            edge_fg_supported edge_has_edges(0/1) edge_sample_valid(0/1)
  diff 模式:diff_pixels diff_ratio diff_max_channel_delta diff_mean_channel_delta
            diff_semantic_pixels(忽略 alpha 不同)diff_pixels_both_background
            diff_bbox_x0/y0/x1/y1(x1/y1 为包含端;无差异时均为 -1)diff_bbox_w/h
            diff_bbox_area_ratio(包围盒面积 / 全图面积)diff_bbox_width_ratio
            diff_bbox_height_ratio

退出码:
  0  报告成功且全部断言通过
  1  断言不通过,**或** --edge-metric 的采样区内没有前景像素(指标无效)
  2  用法/文件/解码错误(含"两张图尺寸不一致":需先裁齐再比,工具不替你猜)

关于 --edge-metric 的有效性(踩过的坑):
  用比图窄的 --band/--edge-region 去"抠掉 HUD"会把场景区一起剪掉,采样区里
  连一个前景像素都没有,于是 transition_pixels=0 —— 那是"没数据",不是"没变化"。
  所以本工具在 edge_foreground_pixels==0 时默认报错退出(1),要强行出数必须显式
  给 --allow-empty-sample。判读任何 edge 指标前先看 edge_foreground_pixels > 0。

不支持的 PNG 形态会**显式报错**而不是猜:隔行扫描(Adam7)、非法滤镜字节、
  调色板索引越界、CRC 不符、末尾多出未消费字节。16 位样本按高字节量化。
"""
from __future__ import annotations

import argparse
import os
import re
import struct
import sys
import zlib
from collections import Counter

PNG_SIG = b"\x89PNG\r\n\x1a\n"

# 颜色类型 → 每像素样本数
_COLOR_TYPE_SAMPLES = {0: 1, 2: 3, 3: 1, 4: 2, 6: 4}
# PNG 规定的通道数(调色板图对外报告为 3 通道 RGB,见 Image.channels)
_LEGAL_BIT_DEPTHS = {
    0: (1, 2, 4, 8, 16),
    2: (8, 16),
    3: (1, 2, 4, 8),
    4: (8, 16),
    6: (8, 16),
}
_LUMA_R, _LUMA_G, _LUMA_B = 299, 587, 114  # luma = (W+R + W*G + W*B) / 1000


class PngError(Exception):
    """PNG 无法被本工具可信地解释。"""


# --------------------------------------------------------------------------- #
# 解码
# --------------------------------------------------------------------------- #
class Image:
    """解码后的图像。

    planes[0..2] 恒为 RGB(已展开调色板 / 灰度复制),planes[3] 为 alpha
    (没有 alpha 时是一个全 255 的 bytes,长度为 w*h)。
    """

    __slots__ = ("width", "height", "bit_depth", "color_type", "planes",
                 "transparent_color", "palette_size", "trns_alpha")

    def __init__(self, width, height, bit_depth, color_type, planes,
                 transparent_color=None, palette_size=None, trns_alpha=False):
        self.width = width
        self.height = height
        self.bit_depth = bit_depth
        self.color_type = color_type
        self.planes = planes
        self.transparent_color = transparent_color
        self.palette_size = palette_size
        self.trns_alpha = trns_alpha

    @property
    def pixel_count(self):
        return self.width * self.height

    @property
    def bpp(self):
        """每像素字节数(RGBA 口径)。"""
        return 4

    @property
    def channels(self):
        """对外报告的通道数:1(灰度)/ 2(灰度+α)/ 3(RGB)/ 4(RGBA)。"""
        return {0: 1, 2: 3, 3: 3, 4: 2, 6: 4}[self.color_type]

    @property
    def has_alpha(self):
        """文件真的携带 alpha(颜色类型带 α,或带 tRNS 色键/调色板透明度)。"""
        return (self.color_type in (4, 6) or self.transparent_color is not None
                or self.trns_alpha)

    def rgba_tuple(self, index):
        return (self.planes[0][index], self.planes[1][index],
                self.planes[2][index], self.planes[3][index])


def _unfilter(raw, width, height, row_bytes, filter_bpp):
    """撤销 5 种扫描线滤镜。filters_used 会返回出现过的滤镜字节集合。"""
    stride = row_bytes
    out = bytearray(height * stride)
    prev = bytearray(stride)
    pos = 0
    filters_used = set()
    for y in range(height):
        if pos >= len(raw):
            raise PngError("IDAT 数据不足:第 %d 行读不到滤镜字节" % y)
        f = raw[pos]
        pos += 1
        filters_used.add(f)
        line = bytearray(raw[pos:pos + stride])
        if len(line) != stride:
            raise PngError("IDAT 数据不足:第 %d 行只有 %d 字节(需 %d)"
                           % (y, len(line), stride))
        pos += stride
        if f == 0:
            pass
        elif f == 1:  # Sub
            for x in range(filter_bpp, stride):
                line[x] = (line[x] + line[x - filter_bpp]) & 0xFF
        elif f == 2:  # Up
            if y:
                for x in range(stride):
                    line[x] = (line[x] + prev[x]) & 0xFF
        elif f == 3:  # Average
            for x in range(stride):
                a = line[x - filter_bpp] if x >= filter_bpp else 0
                line[x] = (line[x] + ((a + prev[x]) >> 1)) & 0xFF
        elif f == 4:  # Paeth
            for x in range(stride):
                a = line[x - filter_bpp] if x >= filter_bpp else 0
                b = prev[x]
                c = prev[x - filter_bpp] if x >= filter_bpp else 0
                p = a + b - c
                pa = abs(p - a)
                pb = abs(p - b)
                pc = abs(p - c)
                if pa <= pb and pa <= pc:
                    pr = a
                elif pb <= pc:
                    pr = b
                else:
                    pr = c
                line[x] = (line[x] + pr) & 0xFF
        else:
            raise PngError("非法扫描线滤镜字节 %d(第 %d 行),合法值 0..4" % (f, y))
        out[y * stride:(y + 1) * stride] = line
        prev = line
    if pos != len(raw) and all(b == 0 for b in raw[pos:]):
        # zlib 尾部允许 0 填充(部分编码器会写),不算错误
        pass
    elif pos != len(raw):
        raise PngError("IDAT 解压后有 %d 字节未消费(尺寸/位深与数据不符?)"
                       % (len(raw) - pos))
    return out, filters_used


def _expand_bits(row, count, bit_depth):
    """把 1/2/4/8 位样本展开成每样本 1 字节。"""
    if bit_depth == 8:
        return bytes(row[:count])
    if bit_depth == 16:
        # 只取高字节(量化到 8 bit 的既定口径)
        return bytes(row[i * 2] for i in range(count))
    per_byte = 8 // bit_depth
    mask = (1 << bit_depth) - 1
    out = bytearray(count)
    for i in range(count):
        byte = row[i // per_byte]
        shift = 8 - bit_depth * (i % per_byte + 1)
        out[i] = (byte >> shift) & mask
    return bytes(out)


def decode_png_bytes(data):
    """把 PNG 字节解码成 Image。任何不确定的地方都抛 PngError。"""
    if data[:8] != PNG_SIG:
        raise PngError("不是 PNG 文件(签名不符)")
    pos = 8
    ihdr = None
    idat_parts = []
    palette = None
    trns = None
    while pos < len(data):
        if pos + 8 > len(data):
            raise PngError("块头被截断(chunk 头需 8 字节)")
        length, typ = struct.unpack(">I4s", data[pos:pos + 8])
        body = data[pos + 8:pos + 8 + length]
        if len(body) != length:
            raise PngError("块 %r 被截断:声明 %d 字节,实际 %d"
                           % (typ, length, len(body)))
        crc_pos = pos + 8 + length
        if crc_pos + 4 > len(data):
            raise PngError("块 %r 缺 CRC" % (typ,))
        (crc,) = struct.unpack(">I", data[crc_pos:crc_pos + 4])
        actual = zlib.crc32(typ + body) & 0xFFFFFFFF
        if crc != actual:
            raise PngError("块 %r CRC 校验失败(声明 %08x 实际 %08x)"
                           % (typ, crc, actual))
        if typ == b"IHDR":
            if length != 13:
                raise PngError("IHDR 长度应为 13,实际 %d" % length)
            ihdr = struct.unpack(">IIBBBBB", body)
        elif typ == b"PLTE":
            palette = [tuple(body[i:i + 3]) for i in range(0, len(body) - 2, 3)]
        elif typ == b"tRNS":
            trns = bytes(body)
        elif typ == b"IDAT":
            idat_parts.append(body)
        elif typ == b"IEND":
            break
        pos = crc_pos + 4
    if ihdr is None:
        raise PngError("缺 IHDR")
    width, height, bit_depth, color_type, comp, filt, interlace = ihdr
    if width == 0 or height == 0:
        raise PngError("尺寸非法:%dx%d" % (width, height))
    if color_type not in _COLOR_TYPE_SAMPLES:
        raise PngError("非法颜色类型 %d" % color_type)
    if bit_depth not in _LEGAL_BIT_DEPTHS[color_type]:
        raise PngError("颜色类型 %d 不允许位深 %d" % (color_type, bit_depth))
    if comp != 0 or filt != 0:
        raise PngError("压缩/滤镜方法必须为 0(实际 %d/%d)" % (comp, filt))
    if interlace != 0:
        raise PngError("不支持隔行扫描(Adam7);请用 --interlace 外的工具先转成非隔行")
    if not idat_parts:
        raise PngError("缺 IDAT")
    if color_type == 3 and palette is None:
        raise PngError("调色板图缺 PLTE")

    samples = _COLOR_TYPE_SAMPLES[color_type]
    bits_per_pixel = samples * bit_depth
    row_bytes = (width * bits_per_pixel + 7) // 8
    filter_bpp = max(1, bits_per_pixel // 8)

    try:
        raw = zlib.decompress(b"".join(idat_parts))
    except zlib.error as exc:
        raise PngError("IDAT 解压失败: %s" % exc)
    # per-row padding: 用紧凑 stride 计算,避免把尾部 0 填充误判成多行
    out, _filters = _unfilter(raw, width, height, row_bytes, filter_bpp)

    n = width * height
    r_plane = bytearray(n)
    g_plane = bytearray(n)
    b_plane = bytearray(n)
    a_plane = bytearray(b"\xff") * n
    transparent_color = None

    for y in range(height):
        row = out[y * row_bytes:(y + 1) * row_bytes]
        vals = _expand_bits(row, width * samples, bit_depth)
        base = y * width
        if color_type == 0:
            for x in range(width):
                v = vals[x]
                r_plane[base + x] = v
                g_plane[base + x] = v
                b_plane[base + x] = v
        elif color_type == 2:
            for x in range(width):
                r_plane[base + x] = vals[3 * x]
                g_plane[base + x] = vals[3 * x + 1]
                b_plane[base + x] = vals[3 * x + 2]
        elif color_type == 3:
            max_idx = len(palette) - 1
            for x in range(width):
                idx = vals[x]
                if idx > max_idx:
                    raise PngError("调色板索引 %d 越界(PLTE 只有 %d 项)"
                                   % (idx, len(palette)))
                pr, pg, pb = palette[idx]
                r_plane[base + x] = pr
                g_plane[base + x] = pg
                b_plane[base + x] = pb
                if trns is not None and idx < len(trns):
                    a_plane[base + x] = trns[idx]
        elif color_type == 4:
            for x in range(width):
                v = vals[2 * x]
                r_plane[base + x] = v
                g_plane[base + x] = v
                b_plane[base + x] = v
                a_plane[base + x] = vals[2 * x + 1]
        else:  # 6
            for x in range(width):
                r_plane[base + x] = vals[4 * x]
                g_plane[base + x] = vals[4 * x + 1]
                b_plane[base + x] = vals[4 * x + 2]
                a_plane[base + x] = vals[4 * x + 3]

    if trns is not None and color_type in (0, 2):
        # tRNS 色键:R(2B)、G(2B)、B(2B),未用的分量按 0 处理
        if color_type == 0 and len(trns) >= 2:
            key = struct.unpack(">H", trns[:2])[0]
            key8 = key >> 8 if bit_depth == 16 else key
            transparent_color = (key8, key8, key8)
        elif color_type == 2 and len(trns) >= 6:
            k = struct.unpack(">HHH", trns[:6])
            if bit_depth == 16:
                transparent_color = tuple(v >> 8 for v in k)
            else:
                transparent_color = k

    return Image(width, height, bit_depth, color_type,
                 (bytes(r_plane), bytes(g_plane), bytes(b_plane), bytes(a_plane)),
                 transparent_color=transparent_color,
                 palette_size=(len(palette) if palette is not None else None),
                 trns_alpha=(color_type == 3 and trns is not None))


def load_png(path):
    try:
        with open(path, "rb") as f:
            data = f.read()
    except OSError as exc:
        raise PngError("打不开 %s: %s" % (path, exc))
    return decode_png_bytes(data)


# --------------------------------------------------------------------------- #
# 编码(只用于导出差异图 / 测试构造)
# --------------------------------------------------------------------------- #
def _chunk(typ, body):
    return (struct.pack(">I", len(body)) + typ + body
            + struct.pack(">I", zlib.crc32(typ + body) & 0xFFFFFFFF))


def encode_png_rgba(width, height, pixels, filters=(0,)):
    """把 RGBA 字节串编码成 PNG。filters 轮换,便于测试覆盖全部滤镜。"""
    if len(pixels) != width * height * 4:
        raise ValueError("像素字节数应为 %d,实际 %d"
                         % (width * height * 4, len(pixels)))
    ihdr = struct.pack(">IIBBBBB", width, height, 8, 6, 0, 0, 0)
    stride = width * 4
    bpp = 4
    raw = bytearray()
    prev = bytes(stride)
    for y in range(height):
        line = bytes(pixels[y * stride:(y + 1) * stride])
        f = filters[y % len(filters)]
        enc = bytearray(stride)
        if f == 0:
            enc[:] = line
        elif f == 1:
            for x in range(stride):
                a = line[x - bpp] if x >= bpp else 0
                enc[x] = (line[x] - a) & 0xFF
        elif f == 2:
            for x in range(stride):
                enc[x] = (line[x] - prev[x]) & 0xFF
        elif f == 3:
            for x in range(stride):
                a = line[x - bpp] if x >= bpp else 0
                enc[x] = (line[x] - ((a + prev[x]) >> 1)) & 0xFF
        elif f == 4:
            for x in range(stride):
                a = line[x - bpp] if x >= bpp else 0
                b = prev[x]
                c = prev[x - bpp] if x >= bpp else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                enc[x] = (line[x] - pr) & 0xFF
        else:
            raise ValueError("非法滤镜 %d" % f)
        raw.append(f)
        raw += enc
        prev = line
    return (PNG_SIG + _chunk(b"IHDR", ihdr)
            + _chunk(b"IDAT", zlib.compress(bytes(raw), 9))
            + _chunk(b"IEND", b""))


def write_png_rgba(path, width, height, pixels):
    data = encode_png_rgba(width, height, pixels)
    parent = os.path.dirname(os.path.abspath(path))
    if parent:
        os.makedirs(parent, exist_ok=True)
    with open(path, "wb") as f:
        f.write(data)


# --------------------------------------------------------------------------- #
# 分析
# --------------------------------------------------------------------------- #
def luma(r, g, b):
    return (_LUMA_R * r + _LUMA_G * g + _LUMA_B * b) // 1000


def parse_rgb(text):
    parts = [p.strip() for p in text.split(",")]
    if len(parts) == 4:
        parts = parts[:3]
    if len(parts) != 3:
        raise ValueError("颜色应为 r,g,b(或 r,g,b,a),收到 %r" % text)
    out = []
    for p in parts:
        v = int(p, 0)
        if not 0 <= v <= 255:
            raise ValueError("颜色分量 %d 越界(0..255)" % v)
        out.append(v)
    return tuple(out)


def parse_region(text):
    parts = [p.strip() for p in text.split(",")]
    if len(parts) != 4:
        raise ValueError("--region 应为 x,y,w,h,收到 %r" % text)
    try:
        x, y, w, h = (int(p, 0) for p in parts)
    except ValueError:
        raise ValueError("--region 四个分量必须是整数:%r" % text)
    if w <= 0 or h <= 0:
        raise ValueError("--region 的 w/h 必须 > 0:%r" % text)
    return x, y, w, h


def resolve_background(img, spec):
    """返回 (r,g,b)。spec 为 None/auto → 左上角像素。"""
    if spec is None or spec.lower() == "auto":
        return img.rgba_tuple(0)[:3]
    s = spec.strip()
    if s.lower().startswith("corner:"):  # 兼容旧脚本的显式写法
        return img.rgba_tuple(0)[:3]
    return parse_rgb(s)


class Report:
    """收集标量指标,统一打印为 'key = value' 行(机器可解析)。"""

    def __init__(self):
        self.scalars = {}
        self.lines = []

    def set(self, key, value):
        self.scalars[key] = value

    def add(self, text):
        self.lines.append(text)

    def emit(self, out):
        for line in self.lines:
            out.write(line + "\n")
        for key, value in self.scalars.items():
            if isinstance(value, float):
                out.write("%s = %.6f\n" % (key, value))
            else:
                out.write("%s = %s\n" % (key, value))


def _clip_region(width, height, region):
    x, y, w, h = region
    x0 = max(0, min(width, x))
    y0 = max(0, min(height, y))
    x1 = max(x0, min(width, x + w))
    y1 = max(y0, min(height, y + h))
    return x0, y0, x1, y1


def info_block(img, report):
    rgb_planes = img.planes
    a = rgb_planes[3]
    alpha_min = min(a)
    alpha_max = max(a)
    binary = alpha_min == 0 and alpha_max == 255 and all(v in (0, 255) for v in a)
    if not img.has_alpha:
        alpha_min, alpha_max, binary = 255, 255, False
    report.set("width", img.width)
    report.set("height", img.height)
    report.set("bit_depth", img.bit_depth)
    report.set("color_type", img.color_type)
    report.set("bpp", img.bpp)
    report.set("bytes_per_channel", 1)
    report.set("channels", img.channels)
    report.set("has_alpha", 1 if img.has_alpha else 0)
    report.set("alpha_min", alpha_min)
    report.set("alpha_max", alpha_max)
    report.set("has_binary_alpha", 1 if binary else 0)
    report.add("[info] %dx%d  bpp=%d(每通道 1 字节)  位深=%d  颜色类型=%d  "
               "通道数=%d  alpha=%s (min=%d max=%d%s)"
               % (img.width, img.height, img.bpp, img.bit_depth, img.color_type,
                  img.channels, "有" if img.has_alpha else "无", alpha_min,
                  alpha_max, " 二值" if binary else ""))
    if img.color_type == 3:
        report.add("[info] 调色板 %d 项%s"
                   % (img.palette_size,
                      ",带 tRNS 透明度" if img.trns_alpha else ""))
    if img.transparent_color is not None:
        report.add("[info] tRNS 色键 %s" % (img.transparent_color,))


def color_stats(img, top_n, report):
    r, g, b, a = img.planes
    n = img.pixel_count
    counter = Counter()
    with_alpha = img.has_alpha
    for i in range(n):
        if with_alpha:
            counter[(r[i], g[i], b[i], a[i])] += 1
        else:
            counter[(r[i], g[i], b[i])] += 1
    report.set("distinct_colors", len(counter))
    top = counter.most_common(max(1, top_n))
    report.set("color_top_share", top[0][1] / n if top else 0.0)
    report.add("[colors] 共有 %d 种颜色(量化到 8 bit);top-%d:"
               % (len(counter), len(top)))
    for i, (color, count) in enumerate(top):
        share = count / n
        hexs = "#%02x%02x%02x" % color[:3]
        if len(color) == 4:
            hexs += "%02x" % color[3]
        report.set("color_%02d_share" % i, share)
        report.add("  #%d %-9s %7d 像素  %6.2f%%" % (i, hexs, count, share * 100.0))
    return counter


def _is_background(color_rgb, bg, tol):
    for i in range(3):
        if abs(color_rgb[i] - bg[i]) > tol:
            return False
    return True


def region_stats(img, region, bg, bg_tol, index, report, fg_ref=None):
    x0, y0, x1, y1 = _clip_region(img.width, img.height, region)
    r, g, b, a = img.planes
    colors = Counter()
    nonbg = 0
    total = (x1 - x0) * (y1 - y0)
    luma_min, luma_max, luma_sum = 255, 0, 0
    alpha_min = 255
    for y in range(y0, y1):
        base = y * img.width
        for x in range(x0, x1):
            i = base + x
            rgb = (r[i], g[i], b[i])
            colors[rgb] += 1
            if alpha_min > a[i]:
                alpha_min = a[i]
            if not _is_background(rgb, bg, bg_tol):
                nonbg += 1
            L = luma(rgb[0], rgb[1], rgb[2])
            luma_sum += L
            if L < luma_min:
                luma_min = L
            if L > luma_max:
                luma_max = L
    clipped = (x0, y0, x1 - x0, y1 - y0) != region
    pre = "r%d_" % index
    if total == 0:
        report.set(pre + "nonbg_ratio", 0.0)
        report.set(pre + "colors", 0)
        report.set(pre + "distinct_colors", 0)
        report.set(pre + "contrast_luma", 0)
        report.set(pre + "mean_luma", 0)
        report.set(pre + "transition_pixels", 0)
        report.set(pre + "edge_ratio", 0.0)
        report.set(pre + "has_edges", 0)
        report.set(pre + "has_transition", 0)
        report.add("[region %d] %s 完全落在图外,无可统计像素" % (index, region))
        return
    contrast = luma_max - luma_min
    mean_luma = luma_sum / total
    # "边缘过渡像素":既不是背景、也不是区域内最主要的前景色(--fg 或自动推断)。
    # 全图统计里 HUD 文字/按钮本身就是过渡色,所以这个数只在**单色块区域**上有意义;
    # 判断 MSAA 请用 --edge-metric + --band。
    if fg_ref is not None:
        fg = tuple(fg_ref)
    else:
        fg, fg_cnt = None, 0
        for color, cnt in colors.items():
            if _is_background(color, bg, bg_tol):
                continue
            if cnt > fg_cnt or (cnt == fg_cnt and fg is not None and color > fg):
                fg, fg_cnt = color, cnt
    tr = 0
    if fg is not None:
        for color, cnt in colors.items():
            if _is_background(color, bg, bg_tol):
                continue
            if _is_background(color, fg, bg_tol):
                continue
            tr += cnt
    report.set(pre + "x", x0)
    report.set(pre + "y", y0)
    report.set(pre + "w", x1 - x0)
    report.set(pre + "h", y1 - y0)
    report.set(pre + "nonbg_ratio", nonbg / total)
    report.set(pre + "colors", len(colors))
    report.set(pre + "distinct_colors", len(colors))
    report.set(pre + "contrast_luma", contrast)
    report.set(pre + "mean_luma", mean_luma)
    report.set(pre + "min_luma", luma_min)
    report.set(pre + "max_luma", luma_max)
    report.set(pre + "alpha_min", alpha_min)
    report.set(pre + "has_transition", 1 if tr > 0 else 0)
    report.set(pre + "fg_r", fg[0] if fg else -1)
    report.set(pre + "fg_g", fg[1] if fg else -1)
    report.set(pre + "fg_b", fg[2] if fg else -1)
    report.add("[region %d] %s(裁剪后 x=%d y=%d w=%d h=%d%s)"
               % (index, region, x0, y0, x1 - x0, y1 - y0,
                  ", 超出画布已裁剪" if clipped else ""))
    report.add("  背景色=%s 容差=%d  → 非背景像素 %d/%d = %.2f%%"
               % ("#%02x%02x%02x" % bg, bg_tol, nonbg, total, 100.0 * nonbg / total))
    report.add("  颜色种数=%d  亮度跨度=%d (min=%d max=%d mean=%.1f)  alpha_min=%d"
               % (len(colors), contrast, luma_min, luma_max, mean_luma, alpha_min))
    report.add("  主前景色=%s  过渡色像素=%d(占 %.2f%%)%s"
               % (("#%02x%02x%02x" % fg) if fg else "无(区域内全是背景)",
                  tr, 100.0 * tr / total,
                  "" if tr else "  ← 只有纯背景+纯前景(硬边)或纯背景"))
    top = colors.most_common(5)
    report.add("  区域内 top-%d: %s" % (len(top), ", ".join(
        "#%02x%02x%02x(%.1f%%)" % (c[0], c[1], c[2], 100.0 * n / total)
        for c, n in top)))


def edge_metrics(img, region, bg, bg_tol, report, prefix="edge_", index=None,
                 band=None, fg_ref=None):
    """3x3 邻域过渡像素统计。

    分类口径(全部基于 8 bit RGB + 容差 bg_tol):
      背景 B:与 --bg 逐通道差 ≤ bg_tol
      前景 F:--fg 显式给出时用它;否则取区域内**离背景最远**的颜色
               (距离相同则取像素更多的那个色,避免随机挑中噪点色)
      过渡 T:既不属于 B 也不属于 F
    输出两类计数:
      transition_pixels  = 自身为 T 的像素数(硬抗锯齿 / MSAA 的中间色都会落这里)
      neighbor_transition= 自身为 B/F、但 3x3 邻域里同时存在 B 和 F 的像素
                           (覆盖"没有中间色、只是采样位置抖动"的情况)
    has_edges = (B 与 F 都出现) 且 (transition_pixels>0 或 neighbor_transition>0)

    band = (x,y,w,h) 时**只统计带内像素**,但带外一圈像素仍参与 3x3 邻域分类
    (--band 的用处:HUD 一条横带本身就有按钮/文字的软边,不排除它会让
     边沿指标在 UI 改动下抖动,MSAA 对比时必须抠掉)。
    """
    x0, y0, x1, y1 = (_clip_region(img.width, img.height, region)
                      if region is not None else (0, 0, img.width, img.height))
    bx0, by0, bx1, by1 = (_clip_region(img.width, img.height, band)
                          if band is not None else (x0, y0, x1, y1))
    if bx0 < x0 or by0 < y0 or bx1 > x1 or by1 > y1:
        # 带超出区域时,把区域扩大到包含整条带(区域只是"搜索前景/统计范围"的上界)
        x0, y0, x1, y1 = min(x0, bx0), min(y0, by0), max(x1, bx1), max(y1, by1)
    w, h = img.width, img.height
    r, g, b, a = img.planes
    total = (bx1 - bx0) * (by1 - by0)
    if total <= 0:
        for k in ("transition_pixels", "nonbg_pixels", "ratio", "has_edges",
                  "background_pixels", "foreground_pixels", "undetermined_pixels",
                  "neighbor_transition", "distinct_colors", "pixels"):
            report.set(prefix + k, 0)
        report.add("[edge] 区域为空,无法统计")
        return
    # 1) 确定前景参考色(在 (x0,y0)-(x1,y1) 这个"分析窗口"内找)
    color_count = Counter()
    for y in range(y0, y1):
        base = y * w
        for x in range(x0, x1):
            i = base + x
            color_count[(r[i], g[i], b[i])] += 1
    auto_fg = None
    best_dist = -1
    for color, cnt in color_count.items():
        d = max(abs(color[0] - bg[0]), abs(color[1] - bg[1]), abs(color[2] - bg[2]))
        if d > best_dist or (d == best_dist and cnt > color_count.get(auto_fg, 0)):
            auto_fg, best_dist = color, d
    explicit_fg = fg_ref is not None
    if explicit_fg:
        fg = tuple(fg_ref)
        fg_dist = max(abs(fg[0] - bg[0]), abs(fg[1] - bg[1]), abs(fg[2] - bg[2]))
    else:
        fg = auto_fg
        fg_dist = best_dist
    fg_is_distinct = fg_dist > bg_tol
    # 自动推断出的前景是否真的出现在**采样区**(band 内)里 —— 这才是"有没有数据"的判据。
    # 只靠 fg_dist 不够:分析窗口可以是整幅图,而 band 可能恰好剪掉了所有前景。
    fg_in_sample = False
    if fg_is_distinct:
        for yy in range(by0 - y0, by1 - y0):
            off = yy * (x1 - x0)
            for xx in range(bx0 - x0, bx1 - x0):
                cls_at = (r[(y0 + yy) * w + (x0 + xx)], g[(y0 + yy) * w + (x0 + xx)],
                          b[(y0 + yy) * w + (x0 + xx)])
                if _is_background(cls_at, fg, bg_tol):
                    fg_in_sample = True
                    break
            if fg_in_sample:
                break
    # 2) 逐像素分类(整个分析窗口都分类,便于带外一圈参与邻域)
    win_w = x1 - x0
    win_h = y1 - y0
    cls = bytearray(win_w * win_h)  # 0=B 1=F 2=T
    for yy, y in enumerate(range(y0, y1)):
        base = y * w
        off = yy * win_w
        for xx, x in enumerate(range(x0, x1)):
            i = base + x
            if _is_background((r[i], g[i], b[i]), bg, bg_tol):
                cls[off + xx] = 0
            elif fg_is_distinct and _is_background((r[i], g[i], b[i]), fg, bg_tol):
                cls[off + xx] = 1
            else:
                cls[off + xx] = 2
    # 3) 只统计带内像素(band 之外的像素仅作为邻域上下文)
    band_x0 = bx0 - x0
    band_y0 = by0 - y0
    bg_n = fg_n = tr_n = 0
    for yy in range(band_y0, band_y0 + (by1 - by0)):
        off = yy * win_w
        for xx in range(band_x0, band_x0 + (bx1 - bx0)):
            c = cls[off + xx]
            if c == 0:
                bg_n += 1
            elif c == 1:
                fg_n += 1
            else:
                tr_n += 1
    # 4) 邻域过渡:自身 B/F 且 3x3 邻域内同时有 B 与 F(邻域可越出带,但不出分析窗口)
    neighbor_tr = 0
    for yy in range(band_y0, band_y0 + (by1 - by0)):
        for xx in range(band_x0, band_x0 + (bx1 - bx0)):
            if cls[yy * win_w + xx] == 2:
                continue
            seen_b = seen_f = False
            for dy in (-1, 0, 1):
                ny = yy + dy
                if ny < 0 or ny >= win_h:
                    continue
                row_off = ny * win_w
                for dx in (-1, 0, 1):
                    nx = xx + dx
                    if nx < 0 or nx >= win_w:
                        continue
                    c = cls[row_off + nx]
                    if c == 0:
                        seen_b = True
                    elif c == 1:
                        seen_f = True
            if seen_b and seen_f:
                neighbor_tr += 1
    has_edges = 1 if (bg_n > 0 and fg_n > 0 and
                      (tr_n > 0 or neighbor_tr > 0)) else 0
    # 采样有效性:采样区内没有可区分的前景 = 本次统计无效(这是"没数据",
    # 不是"没有边缘")。典型来源:用比图窄的 --band/--edge-region 把场景区剪光了,
    # 或者把 band 画到 HUD 上(HUD 全是过渡色,反而没有"前景")。
    # 唯一例外:全图统计 + 整幅图是纯色 —— 图本身没有边缘可找(valid=1,has_edges=0)。
    whole_image = (bx0 == 0 and by0 == 0 and bx1 == w and by1 == h)
    if not fg_is_distinct:
        sample_valid = 1 if whole_image else 0
    else:
        sample_valid = 1 if fg_in_sample else 0
    report.set(prefix + "sample_valid", sample_valid)
    report.set(prefix + "auto_fg_distinct", 1 if fg_is_distinct else 0)
    report.set(prefix + "fg_in_sample", 1 if (fg_n > 0) else 0)
    report.set(prefix + "region_x", bx0)
    report.set(prefix + "region_y", by0)
    report.set(prefix + "region_w", bx1 - bx0)
    report.set(prefix + "region_h", by1 - by0)
    report.set(prefix + "pixels", total)
    report.set(prefix + "background_pixels", bg_n)
    report.set(prefix + "foreground_pixels", fg_n)
    report.set(prefix + "undetermined_pixels", tr_n)
    report.set(prefix + "transition_pixels", tr_n)
    report.set(prefix + "nonbg_pixels", fg_n + tr_n)
    report.set(prefix + "neighbor_transition", neighbor_tr)
    report.set(prefix + "ratio", tr_n / total)
    report.set(prefix + "neighbor_ratio", neighbor_tr / total)
    report.set(prefix + "distinct_colors", len(color_count))
    report.set(prefix + "has_edges", has_edges)
    report.set(prefix + "fg_supported", 1 if fg_is_distinct else 0)
    report.set(prefix + "fg_r", fg[0] if fg else 0)
    report.set(prefix + "fg_g", fg[1] if fg else 0)
    report.set(prefix + "fg_b", fg[2] if fg else 0)
    report.add("[edge] 统计范围=(%d,%d)-(%d,%d)%s 背景=#%02x%02x%02x 容差=%d"
               % (bx0, by0, bx1, by1,
                  " (分析窗口 %d,%d,%d,%d,带外一圈只作邻域上下文)"
                  % (x0, y0, x1 - x0, y1 - y0) if band is not None else "",
                  bg[0], bg[1], bg[2], bg_tol))
    report.add("[edge] 前景参考=#%02x%02x%02x(%s,dist=%d)%s"
               % (fg[0], fg[1], fg[2],
                  "--fg 显式指定" if explicit_fg else "离背景最远色", fg_dist,
                  "" if fg_is_distinct else "  ← 区域内没有可区分的前景"))
    report.add("[edge] 背景像素=%d 前景像素=%d 过渡像素=%d(占 %.2f%%) "
               "邻域过渡像素=%d(占 %.2f%%)  颜色种数=%d"
               % (bg_n, fg_n, tr_n, 100.0 * tr_n / total, neighbor_tr,
                  100.0 * neighbor_tr / total, len(color_count)))
    report.add("[edge] 判定: %s"
               % ("检测到边缘(前景与背景交界,且存在过渡)" if has_edges
                  else "未检测到边缘(全背景 / 全前景 / 无过渡)"))
    if not sample_valid:
        report.add("[edge] [报错] 采样区内 foreground_pixels=0(或前景与背景不可区分)"
                   " → 本次指标**无效**,不能读成『没有边缘/没有变化』。"
                   "请把 --band/--edge-region 放宽到能包含场景前景(宽度取满图宽)。")
    if index is not None:
        # 给 --region + --edge-metric 同时使用时补上 r<i>_ 前缀的别名
        for k in ("transition_pixels", "neighbor_transition", "has_edges", "ratio"):
            report.set("r%d_%s" % (index, "edge_" + k), report.scalars[prefix + k])
        report.set("r%d_edge_ratio" % index, report.scalars[prefix + "ratio"])
        report.set("r%d_has_edges" % index, report.scalars[prefix + "has_edges"])


def diff_images(img_a, img_b, bg, bg_tol, out_path, report):
    if (img_a.width, img_a.height) != (img_b.width, img_b.height):
        raise PngError("尺寸不一致:a=%dx%d b=%dx%d;请先用 --region 裁齐同一区域再比"
                       % (img_a.width, img_a.height, img_b.width, img_b.height))
    w, h = img_a.width, img_a.height
    n = w * h
    ra, ga, ba, aa = img_a.planes
    rb, gb, bb, ab = img_b.planes
    diff_px = 0
    semantic_px = 0
    max_delta = 0
    sum_delta = 0
    bx0, by0, bx1, by1 = n, n, -1, -1
    out_px = bytearray(n * 4)
    for i in range(n):
        dr = abs(ra[i] - rb[i])
        dg = abs(ga[i] - gb[i])
        db = abs(ba[i] - bb[i])
        da = abs(aa[i] - ab[i])
        d = max(dr, dg, db, da)
        if d:
            diff_px += 1
            x = i % w
            y = i // w
            if x < bx0:
                bx0 = x
            if y < by0:
                by0 = y
            if x > bx1:
                bx1 = x
            if y > by1:
                by1 = y
        if max(dr, dg, db):
            semantic_px += 1
        if d > max_delta:
            max_delta = d
        sum_delta += d
        # 差异图:相同处画成暗灰(不是纯黑,便于与"差异为黑"区分),不同处画成放大后的差值
        if d == 0:
            out_px[i * 4:i * 4 + 4] = bytes((32, 32, 32, 255))
        else:
            k = min(255, d * 4)
            out_px[i * 4:i * 4 + 4] = bytes((k, k, k, 255))
    report.set("diff_width", w)
    report.set("diff_height", h)
    report.set("diff_pixels", diff_px)
    report.set("diff_ratio", diff_px / n)
    report.set("diff_semantic_pixels", semantic_px)
    report.set("diff_max_channel_delta", max_delta)
    report.set("diff_mean_channel_delta", sum_delta / n)
    if diff_px:
        report.set("diff_bbox_x0", bx0)
        report.set("diff_bbox_y0", by0)
        report.set("diff_bbox_x1", bx1)
        report.set("diff_bbox_y1", by1)
        report.set("diff_bbox_w", bx1 - bx0 + 1)
        report.set("diff_bbox_h", by1 - by0 + 1)
        # 包围盒相对全图的比例:用来区分"集中在一条窄带"还是"铺满画面"
        report.set("diff_bbox_area_ratio",
                   ((bx1 - bx0 + 1) * (by1 - by0 + 1)) / n)
        report.set("diff_bbox_width_ratio", (bx1 - bx0 + 1) / w)
        report.set("diff_bbox_height_ratio", (by1 - by0 + 1) / h)
    else:
        for k in ("x0", "y0", "x1", "y1", "w", "h"):
            report.set("diff_bbox_" + k, -1)
        report.set("diff_bbox_area_ratio", 0.0)
        report.set("diff_bbox_width_ratio", 0.0)
        report.set("diff_bbox_height_ratio", 0.0)
    report.add("[diff] %dx%d 逐像素比较(RGBA)"
               "  → 差异像素 %d/%d = %.4f%%"
               % (w, h, diff_px, n, 100.0 * diff_px / n))
    if diff_px:
        report.add("[diff] 差异包围盒 x=[%d,%d] y=[%d,%d] 尺寸 %dx%d"
                   % (bx0, bx1, by0, by1, bx1 - bx0 + 1, by1 - by0 + 1))
        report.add("[diff] 包围盒占图:宽 %.1f%% 高 %.1f%% 面积 %.1f%%"
                   % (100.0 * (bx1 - bx0 + 1) / w, 100.0 * (by1 - by0 + 1) / h,
                      100.0 * ((bx1 - bx0 + 1) * (by1 - by0 + 1)) / n))
    else:
        report.add("[diff] 两图逐像素完全一致(差异包围盒为空:-1)")
    report.add("[diff] 只考虑 RGB 的差异像素=%d(alpha 差异单独计)"
               % semantic_px)
    report.add("[diff] 通道差 max=%d mean=%.4f"
               % (max_delta, sum_delta / n))
    if out_path:
        write_png_rgba(out_path, w, h, out_px)
        report.add("[diff] 差异图已导出: %s(相同=暗灰 #202020,不同=差值×4 灰度)"
                   % out_path)
        report.set("diff_out", 1)
    else:
        report.set("diff_out", 0)
    # 背景口径下有多少差异像素落在背景上(便于判断"只是背景抖动"还是"结构变了")
    on_bg = 0
    for i in range(n):
        if (ra[i], ga[i], ba[i]) == (rb[i], gb[i], bb[i]):
            continue
        if _is_background((ra[i], ga[i], ba[i]), bg, bg_tol) and \
                _is_background((rb[i], gb[i], bb[i]), bg, bg_tol):
            on_bg += 1
    report.set("diff_pixels_both_background", on_bg)


# --------------------------------------------------------------------------- #
# 断言
# --------------------------------------------------------------------------- #
_ASSERT_RE = re.compile(r"^\s*([A-Za-z_][A-Za-z0-9_]*)\s*(>=|<=|==|!=|>|<)?\s*"
                        r"(-?[0-9.eE+-]*)\s*$")


def parse_assert(text):
    m = _ASSERT_RE.match(text)
    if not m:
        raise ValueError("无法解析断言 %r(格式: NAME OP VALUE)" % text)
    name, op, value = m.group(1), m.group(2), m.group(3)
    if op is None:
        if value != "":
            raise ValueError("断言 %r 缺比较符(例如 %s>0)" % (text, name))
        op, value = "!=", "0"       # 裸 NAME:非 0 即通过
    if value == "":
        raise ValueError("断言 %r 缺比较值" % text)
    try:
        number = float(value)
    except ValueError:
        raise ValueError("断言 %r 的比较值不是数字:%r" % (text, value))
    return name, op, number


def evaluate_assertions(assertions, scalars):
    """返回 (failures, checked)。failures 是 [(text, reason)]。"""
    failures = []
    checked = 0
    for text in assertions:
        name, op, number = parse_assert(text)
        if name not in scalars:
            failures.append((text, "报告里没有指标 %r(用 --list-metrics 看可用键)"
                             % name))
            continue
        actual = scalars[name]
        actual_f = float(actual)
        ok = {
            ">": actual_f > number,
            ">=": actual_f >= number,
            "<": actual_f < number,
            "<=": actual_f <= number,
            "==": actual_f == number,
            "!=": actual_f != number,
        }[op]
        checked += 1
        if not ok:
            failures.append((text, "实际 %s %s %s 不成立"
                             % (fmt_num(actual), _negate(op), fmt_num(number))))
    return failures, checked


def _negate(op):
    return {">": "<=", ">=": "<", "<": ">=", "<=": ">", "==": "!=", "!=": "=="}[op]


def fmt_num(v):
    if isinstance(v, float):
        return ("%.6f" % v).rstrip("0").rstrip(".") or "0"
    return str(v)


# --------------------------------------------------------------------------- #
# CLI
# --------------------------------------------------------------------------- #
class _Parser(argparse.ArgumentParser):
    """让 argparse 的用法错误走退出码 2 而不是 1(1 专留给断言失败)。"""

    def error(self, message):
        self.print_usage(sys.stderr)
        sys.stderr.write("%s: error: %s\n" % (self.prog, message))
        raise SystemExit(2)


def build_parser():
    p = _Parser(
        prog="png_inspect.py",
        description="机器可读的 PNG 像素分析(纯标准库)。所有数字以 'key = value' "
                    "行输出,可被 --assert 直接断言。",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="退出码: 0 全部断言通过 / 1 断言不通过 / 2 用法或解码错误")
    p.add_argument("png", help="被分析的 PNG")
    p.add_argument("--info", action="store_true",
                   help="打印尺寸/通道/位深/颜色类型/alpha(任何模式都会先打)")
    p.add_argument("--colors", nargs="?", const=8, type=int, default=8,
                   metavar="N", help="主色统计 top-N(默认 8,总是执行),同时报告颜色总数")
    p.add_argument("--region", action="append", default=[], metavar="x,y,w,h",
                   help="区域统计,可重复")
    p.add_argument("--edge-metric", action="store_true",
                   help="边沿平滑度:3x3 邻域过渡像素统计(判断 MSAA/抗锯齿)")
    p.add_argument("--region-colors", action="store_true",
                   help="区域统计里也打印区域内 top-5 颜色(默认已打印)")
    p.add_argument("--edge-region", default=None, metavar="x,y,w,h",
                   help="--edge-metric 的分析窗口(默认全图)")
    p.add_argument("--band", default=None, metavar="x,y,w,h",
                   help="--edge-metric 把统计范围**限定到场景区**(宽度必须取满图宽,"
                        "否则会剪掉场景前景 → 指标无效);带外一圈仍参与 3x3 邻域")
    p.add_argument("--allow-empty-sample", action="store_true",
                   help="采样区内 edge_foreground_pixels==0 时也正常退出(默认退出码 1:"
                        "『没数据』不许被读成『没变化』)")
    p.add_argument("--diff", default=None, metavar="B.png",
                   help="与另一张图逐像素对比")
    p.add_argument("--diff-out", default=None, metavar="D.png",
                   help="导出差异图(配合 --diff)")
    p.add_argument("--bg", default="auto", metavar="auto|r,g,b",
                   help="背景色,默认 auto = 左上角像素")
    p.add_argument("--bg-tol", type=int, default=0, metavar="T",
                   help="背景/前景判定容差(逐通道,默认 0)")

    def _color(text):
        try:
            return parse_rgb(text)
        except ValueError as exc:
            raise argparse.ArgumentTypeError(str(exc))

    p.add_argument("--fg", type=_color, default=None, metavar="r,g,b",
                   help="显式指定前景参考色(默认取区域内离背景最远的颜色)")
    p.add_argument("--assert", dest="asserts", action="append", default=[],
                   metavar="NAME OP VALUE", help="断言,可重复;不满足则退出码 1")
    p.add_argument("--list-metrics", action="store_true",
                   help="分析完列出报告里全部标量键后退出")
    p.add_argument("--json", nargs="?", const="-", default=None, metavar="PATH",
                   help="额外把标量指标写成 JSON('-' = 打到 stdout)")
    p.add_argument("--quiet", action="store_true", help="只打断言结论")
    return p


def run(argv, stdout=None, stderr=None):
    """可编程入口:返回退出码。测试直接调它,不 fork 子进程。"""
    out = stdout if stdout is not None else sys.stdout
    err = stderr if stderr is not None else sys.stderr
    parser = build_parser()
    try:
        args = parser.parse_args(argv)
    except SystemExit as exc:
        return int(exc.code) if exc.code is not None else 0
    if args.bg_tol < 0:
        err.write("png_inspect: --bg-tol 不能为负\n")
        return 2
    try:
        regions = [parse_region(t) for t in args.region]
        edge_region = parse_region(args.edge_region) if args.edge_region else None
        band = parse_region(args.band) if args.band else None
    except ValueError as exc:
        err.write("png_inspect: %s\n" % exc)
        return 2
    # 先校验断言格式,再干活 —— 格式错是用法错误(2),不是断言失败(1)
    try:
        for text in args.asserts:
            parse_assert(text)
    except ValueError as exc:
        err.write("png_inspect: %s\n" % exc)
        return 2

    report = Report()
    try:
        img = load_png(args.png)
        bg = resolve_background(img, args.bg)
        report.add("[file] %s" % args.png)
        info_block(img, report)
        if args.colors is not None:
            if args.colors <= 0:
                raise ValueError("--colors 必须 > 0")
            color_stats(img, args.colors, report)
        for idx, region in enumerate(regions):
            region_stats(img, region, bg, args.bg_tol, idx, report, fg_ref=args.fg)
        if args.edge_metric:
            edge_metrics(img, edge_region, bg, args.bg_tol, report,
                         prefix="edge_", index=None, band=band, fg_ref=args.fg)
        if args.diff:
            other = load_png(args.diff)
            report.add("[file] diff 对照: %s" % args.diff)
            diff_images(img, other, bg, args.bg_tol, args.diff_out, report)
        elif args.diff_out:
            raise ValueError("--diff-out 需要配合 --diff")
    except (PngError, ValueError) as exc:
        err.write("png_inspect: 错误: %s\n" % exc)
        return 2

    if args.list_metrics:
        report.add("[metrics] 可用标量键:")
        report.add("  " + " ".join(report.scalars.keys()))
    if not args.quiet:
        report.emit(out)

    json_stdout = args.json == "-"
    if args.json:
        import json
        payload = json.dumps(report.scalars, ensure_ascii=False, indent=2,
                             sort_keys=True)
        if json_stdout:
            if not args.quiet:
                # 报告已经打完了,用分隔注释把 JSON 隔开,肉眼与管道解析都不混淆
                out.write("# ---- JSON(report.scalars)开始;--quiet 可只输出 JSON ----\n")
            out.write(payload + "\n")
        else:
            parent = os.path.dirname(os.path.abspath(args.json))
            if parent:
                os.makedirs(parent, exist_ok=True)
            with open(args.json, "w", encoding="utf-8") as f:
                f.write(payload + "\n")

    failures, checked = evaluate_assertions(args.asserts, report.scalars)
    for text, reason in failures:
        out.write("[assert FAIL] %s  ← %s\n" % (text, reason))
    if args.asserts:
        out.write("[assert] %d/%d 通过\n"
                  % (checked - len(failures), len(args.asserts)))

    # 采样有效性守卫(决定性失败,不静默):edge 指标没有前景样本 = 本次统计无效。
    # 这道守卫与断言分开判,即使没给 --assert 也必须报错,否则 0 会被当成"没变化"。
    invalid_sample = (args.edge_metric and
                      report.scalars.get("edge_sample_valid", 1) == 0 and
                      not args.allow_empty_sample)
    if invalid_sample:
        out.write("[edge FAIL] 采样区内 edge_foreground_pixels=%d → 指标无效"
                  "(不是『没有边缘』)。放宽 --band/--edge-region 让场景前景进入采样区;"
                  "确需强行出数请显式加 --allow-empty-sample\n"
                  % report.scalars.get("edge_foreground_pixels", 0))

    if failures or invalid_sample:
        out.write("结论: 断言不通过(%d 条断言 + %d 条无效指标)\n"
                  % (len(failures), 1 if invalid_sample else 0))
        return 1
    if args.asserts:
        out.write("结论: 全部断言通过\n")
    else:
        out.write("结论: 报告完成(未给 --assert)\n")
    return 0


def main(argv=None):
    try:
        return run(sys.argv[1:] if argv is None else argv)
    except BrokenPipeError:
        return 2


if __name__ == "__main__":
    sys.exit(main())
