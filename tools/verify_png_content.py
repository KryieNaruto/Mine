"""PNG 内容客观校验：不依赖肉眼，直接解像素统计。

背景：本会话的模型不能读图，所以"文件生成了"不能当作"画对了"。
本脚本用 zlib 解 PNG 的 IDAT，统计：
  - 非背景像素占比（背景是 0.06/0.07/0.10 ≈ rgb(15,18,26)）
  - 颜色多样性（有几笔不同颜色 → 验证逐笔颜色确实生效）
  - 亮度分布（验证 3D 光照：同一实体应有明暗渐变，不是纯平色）
  - 垂直/水平跨度（验证几何铺开了而不是聚在一点）
"""
import struct
import sys
import zlib
from collections import Counter


def load_png(path):
    data = open(path, 'rb').read()
    assert data[:8] == b'\x89PNG\r\n\x1a\n', 'not a PNG'
    pos = 8
    idat = b''
    w = h = bitdepth = colortype = None
    while pos < len(data):
        (length,) = struct.unpack('>I', data[pos:pos + 4])
        ctype = data[pos + 4:pos + 8]
        chunk = data[pos + 8:pos + 8 + length]
        if ctype == b'IHDR':
            w, h, bitdepth, colortype = struct.unpack('>IIBB', chunk[:10])
        elif ctype == b'IDAT':
            idat += chunk
        elif ctype == b'IEND':
            break
        pos += 12 + length
    raw = zlib.decompress(idat)
    assert bitdepth == 8 and colortype == 6, f'expected RGBA8, got {bitdepth}/{colortype}'
    stride = w * 4
    out = bytearray(h * stride)
    prev = bytearray(stride)
    p = 0
    for y in range(h):
        f = raw[p]
        p += 1
        line = bytearray(raw[p:p + stride])
        p += stride
        if f == 1:      # Sub
            for i in range(4, stride):
                line[i] = (line[i] + line[i - 4]) & 0xFF
        elif f == 2:    # Up
            for i in range(stride):
                line[i] = (line[i] + prev[i]) & 0xFF
        elif f == 3:    # Average
            for i in range(stride):
                a = line[i - 4] if i >= 4 else 0
                line[i] = (line[i] + ((a + prev[i]) >> 1)) & 0xFF
        elif f == 4:    # Paeth
            for i in range(stride):
                a = line[i - 4] if i >= 4 else 0
                b = prev[i]
                c = prev[i - 4] if i >= 4 else 0
                pp = a + b - c
                pa, pb, pc = abs(pp - a), abs(pp - b), abs(pp - c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[i] = (line[i] + pr) & 0xFF
        out[y * stride:(y + 1) * stride] = line
        prev = line
    return w, h, out


def analyze(path):
    w, h, px = load_png(path)
    # 背景色（与 CLI 的 clear color 一致，8bit 量化后）
    bg = (15, 18, 26)

    def is_bg(r, g, b):
        return abs(r - bg[0]) <= 2 and abs(g - bg[1]) <= 2 and abs(b - bg[2]) <= 2

    fg = 0
    minx, maxx, miny, maxy = w, -1, h, -1
    colors = Counter()
    lums = []
    for y in range(h):
        base = y * w * 4
        for x in range(w):
            i = base + x * 4
            r, g, b = px[i], px[i + 1], px[i + 2]
            if is_bg(r, g, b):
                continue
            fg += 1
            if x < minx: minx = x
            if x > maxx: maxx = x
            if y < miny: miny = y
            if y > maxy: maxy = y
            colors[(r // 32, g // 32, b // 32)] += 1
            lums.append((r * 299 + g * 587 + b * 114) // 1000)

    total = w * h
    print(f'file        : {path}')
    print(f'size        : {w}x{h}')
    print(f'fg pixels   : {fg} ({100.0*fg/total:.2f}%)')
    print(f'bbox        : x[{minx},{maxx}] y[{miny},{maxy}]  span {maxx-minx+1}x{maxy-miny+1}')
    print(f'distinct色组 : {len(colors)}')
    if lums:
        lums.sort()
        print(f'lum min/max : {lums[0]} / {lums[-1]}')
        print(f'lum unique  : {len(set(lums))}')
    return fg, total, maxx - minx + 1, maxy - miny + 1, len(colors)


if __name__ == '__main__':
    ok = True
    for path in sys.argv[1:]:
        fg, total, sw, sh, ncol = analyze(path)
        w, h, px = load_png(path)

        # 3D 明暗判据：前景像素的亮度分布应当有明显梯度。
        # 纯平色（没走光照/没有深度）时亮度几乎只有一个值；
        # 真实 3D 实体因为有多个法线朝向（顶盖 vs 侧壁），
        # 亮度必然散布成一段区间。
        bg = (15, 18, 26)
        lums = set()
        for y in range(h):
            base = y * w * 4
            for x in range(w):
                i = base + x * 4
                r, g, b = px[i], px[i + 1], px[i + 2]
                if abs(r - bg[0]) <= 2 and abs(g - bg[1]) <= 2 and abs(b - bg[2]) <= 2:
                    continue
                lums.add((r * 299 + g * 587 + b * 114) // 1000)
        lum_span = (max(lums) - min(lums)) if lums else 0

        # 断言：非空、不铺满、铺开、有 3D 明暗
        #
        # 「铺开」的比较基准必须是**图像尺寸**，不是像素总数。
        # 第一版写成 sw > total // 1（拿宽度去比像素总数），
        # 逻辑上永远不可能成立 —— 断言本身写错比断言失败更危险，
        # 因为它会让一个正确的渲染结果永远"不通过"。
        #
        # 「颜色种类」指标已废弃：它依赖量化桶宽，不同 pattern 的
        # 笔色接近时会自然并桶，不是错误。改用**亮度跨度**判断
        # "有没有 3D 光照"，这个指标与图案无关。
        checks = [
            ('非背景像素 >1%（画出来了）', fg > total * 0.01),
            ('非背景像素 <85%（不是糊满屏）', fg < total * 0.85),
            (f'横向跨度 >20% 宽（{sw}/{w}）', sw > w * 0.20),
            (f'纵向跨度 >20% 高（{sh}/{h}）', sh > h * 0.20),
            (f'亮度跨度 >=20（3D 明暗生效，实测 {lum_span}）', lum_span >= 20),
        ]
        for name, passed in checks:
            print(f'  [{"PASS" if passed else "FAIL"}] {name}')
            ok = ok and passed
        print()
    print('RESULT:', 'PASS' if ok else 'FAIL')
    sys.exit(0 if ok else 1)
