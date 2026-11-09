#!/usr/bin/env python3
"""检查 PNG 是否真的画出了东西（不是纯背景色）。

用法: python check_png_content.py <png>

退出码:
  0  画面上有内容（非背景像素占比 > 0.5%）
  1  基本是纯背景色（"录了命令但没出像素" 的典型症状）

为什么要这个工具:
  L1 的 ICommandList::draw* 在设备态只把命令录进流,不下发 vkCmdDraw*。
  必须调 IRenderDevice::submit_command_list() 才真正出图。漏掉这一步时
  CLI 会"成功退出"并写出一张纯背景色的 PNG —— 没有任何报错。
  这个脚本就是那条缺失的断言。
"""
import struct
import sys
import zlib


def read_png(path):
    data = open(path, "rb").read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        raise ValueError("不是 PNG 文件")
    i = 8
    w = h = 0
    ct = 0
    idat = b""
    while i < len(data):
        ln = struct.unpack(">I", data[i:i + 4])[0]
        typ = data[i + 4:i + 8]
        chunk = data[i + 8:i + 8 + ln]
        if typ == b"IHDR":
            w, h, _bd, ct = struct.unpack(">IIBB", chunk[:10])
        elif typ == b"IDAT":
            idat += chunk
        i += 12 + ln
    raw = zlib.decompress(idat)
    bpp = 4 if ct == 6 else 3
    stride = w * bpp
    out = bytearray(h * stride)
    pos = 0
    for y in range(h):
        f = raw[pos]
        pos += 1
        line = bytearray(raw[pos:pos + stride])
        pos += stride
        if f == 1:
            for x in range(bpp, stride):
                line[x] = (line[x] + line[x - bpp]) & 0xFF
        elif f == 2 and y > 0:
            prev = out[(y - 1) * stride:y * stride]
            for x in range(stride):
                line[x] = (line[x] + prev[x]) & 0xFF
        elif f == 3:
            prev = out[(y - 1) * stride:y * stride] if y > 0 else bytearray(stride)
            for x in range(stride):
                a = line[x - bpp] if x >= bpp else 0
                line[x] = (line[x] + ((a + prev[x]) >> 1)) & 0xFF
        elif f == 4:
            prev = out[(y - 1) * stride:y * stride] if y > 0 else bytearray(stride)
            for x in range(stride):
                a = line[x - bpp] if x >= bpp else 0
                b = prev[x]
                c = prev[x - bpp] if x >= bpp else 0
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[x] = (line[x] + pr) & 0xFF
        out[y * stride:(y + 1) * stride] = line
    return w, h, bpp, out


def main(argv):
    if not argv:
        print("用法: check_png_content.py <png>")
        return 2
    w, h, bpp, px = read_png(argv[0])
    bg = (px[0], px[1], px[2])
    nonbg = 0
    total = 0
    colors = set()
    for y in range(0, h, 2):
        for x in range(0, w, 2):
            o = (y * w + x) * bpp
            p = (px[o], px[o + 1], px[o + 2])
            total += 1
            if p != bg:
                nonbg += 1
                colors.add(p)
    ratio = 100.0 * nonbg / max(total, 1)
    print("尺寸 %dx%d bpp=%d" % (w, h, bpp))
    print("背景色 %s (左上角像素)" % (bg,))
    print("非背景采样像素 %d/%d = %.1f%%" % (nonbg, total, ratio))
    print("不同颜色数 %d" % len(colors))
    if colors:
        print("前 8 种颜色 %s" % (sorted(colors)[:8],))
    if ratio < 0.5:
        print("结论: 画面几乎是纯背景色 —— 请检查 submit_command_list 是否被调用")
        return 1
    print("结论: 画面上有内容")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
