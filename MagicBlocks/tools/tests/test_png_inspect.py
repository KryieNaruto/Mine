#!/usr/bin/env python3
"""png_inspect.py 的自测。

运行(任选其一;仓库里没有 pytest 也能跑):
  python -m unittest discover -s MagicBlocks/tools/tests -p "test_png_inspect.py" -v
  python -m unittest MagicBlocks/tools/tests/test_png_inspect.py -v
  python MagicBlocks/tools/tests/test_png_inspect.py       # 直接当脚本跑

测试自己构造 PNG,不依赖任何外部图片、不依赖 PIL。
覆盖任务要求的边界:1x1、全同色、双色边界、alpha=0;
另外覆盖:五种扫描线滤镜、2x2 量化、灰度/灰度+α/RGB/调色板+tRNS、
位深 1/16、NaN 般非法输入(CRC 坏、截断、隔行、尺寸不符)、
区域裁剪与越界、差异导出的"相同区域必须是暗灰",以及退出码 0/1/2 的三分语义。
"""
import io
import os
import struct
import sys
import tempfile
import unittest
import zlib

_HERE = os.path.dirname(os.path.abspath(__file__))
_TOOLS = os.path.dirname(_HERE)
if _TOOLS not in sys.path:
    sys.path.insert(0, _TOOLS)

import png_inspect as px  # noqa: E402


# --------------------------------------------------------------------------- #
# 测试用的小 PNG 构造器(故意不复用被测的 encode_png_rgba,避免同源错误)
# --------------------------------------------------------------------------- #
def _chunk(typ, body):
    return (struct.pack(">I", len(body)) + typ + body
            + struct.pack(">I", zlib.crc32(typ + body) & 0xFFFFFFFF))


def make_png(width, height, color_type, bit_depth, raw_rows,
             palette=None, trns=None, filters=None, interlace=0,
             extra_chunks=b"", corrupt_crc=False, drop_iend=False):
    """raw_rows 是已经过滤镜的扫描线字节(每行以滤镜字节开头)。"""
    ihdr = struct.pack(">IIBBBBB", width, height, bit_depth, color_type,
                       0, 0, interlace)
    body = b""
    if palette is not None:
        body += _chunk(b"PLTE", b"".join(bytes(c) for c in palette))
    if trns is not None:
        body += _chunk(b"tRNS", trns)
    body += extra_chunks
    data = b"".join(raw_rows)
    body += _chunk(b"IDAT", zlib.compress(data))
    if not drop_iend:
        body += _chunk(b"IEND", b"")
    out = px.PNG_SIG + _chunk(b"IHDR", ihdr) + body
    if corrupt_crc:
        # 破坏 IDAT 块尾的 CRC
        idx = out.rfind(b"IDAT")
        crc_pos = idx + 4 + struct.unpack(">I", out[idx - 4:idx])[0]
        out = out[:crc_pos] + b"\x00\x00\x00\x00" + out[crc_pos + 4:]
    return out


def rgba_to_filtered(pixels, width, height, filter_cycle=(0,)):
    """把 RGBA 原始字节按给定滤镜轮换编码成扫描线。"""
    stride = width * 4
    bpp = 4
    rows = []
    prev = bytes(stride)
    for y in range(height):
        line = bytes(pixels[y * stride:(y + 1) * stride])
        f = filter_cycle[y % len(filter_cycle)]
        enc = bytearray(stride)
        for x in range(stride):
            a = line[x - bpp] if x >= bpp else 0
            b = prev[x]
            c = prev[x - bpp] if x >= bpp else 0
            if f == 0:
                pred = 0
            elif f == 1:
                pred = a
            elif f == 2:
                pred = b
            elif f == 3:
                pred = (a + b) >> 1
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                pred = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
            else:
                raise ValueError(f)
            enc[x] = (line[x] - pred) & 0xFF
        rows.append(bytes([f]) + bytes(enc))
        prev = line
    return rows


def solid_rgba(width, height, color):
    return bytes(bytes(color) * (width * height))


def checker_rgba(width, height, c1, c2):
    out = bytearray()
    for y in range(height):
        for x in range(width):
            out += bytes(c1 if (x + y) % 2 == 0 else c2)
    return bytes(out)


class _TmpDirMixin:
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory(prefix="png_inspect_test_")
        self.tmp = self._tmp.name

    def tearDown(self):
        self._tmp.cleanup()

    def write(self, name, data):
        path = os.path.join(self.tmp, name)
        with open(path, "wb") as f:
            f.write(data)
        return path

    def write_rgba(self, name, width, height, pixels, filter_cycle=(0,)):
        rows = rgba_to_filtered(pixels, width, height, filter_cycle)
        data = make_png(width, height, 6, 8, rows)
        return self.write(name, data)


# --------------------------------------------------------------------------- #
# 解码 / 元信息
# --------------------------------------------------------------------------- #
class TestDecode(_TmpDirMixin, unittest.TestCase):
    def test_1x1_single_pixel(self):
        data = make_png(1, 1, 6, 8,
                        [b"\x00" + bytes((10, 20, 30, 255))])
        img = px.decode_png_bytes(data)
        self.assertEqual((img.width, img.height), (1, 1))
        self.assertEqual(img.pixel_count, 1)
        self.assertEqual(img.planes[0], b"\x0a")
        self.assertEqual(img.planes[1], b"\x14")
        self.assertEqual(img.planes[2], b"\x1e")
        self.assertEqual(img.planes[3], b"\xff")
        self.assertEqual(img.channels, 4)
        self.assertTrue(img.has_alpha)
        self.assertEqual(img.bpp, 4)

    def test_solid_color(self):
        img = px.decode_png_bytes(
            make_png(4, 3, 6, 8, extent_rows(4, 3, (200, 100, 50, 255))))
        self.assertEqual(img.planes[0], b"\xc8" * 12)
        self.assertEqual(img.planes[1], b"\x64" * 12)
        self.assertEqual(img.planes[2], b"\x32" * 12)

    def test_all_five_filters_roundtrip(self):
        # 用 4x8 的渐变(行与行不同,才会真正触发 Up/Average/Paeth 的预测)
        w, h = 4, 8
        pixels = bytearray()
        for y in range(h):
            for x in range(w):
                pixels += bytes((x * 40, y * 30, (x + y) * 12, 255))
        for f in range(5):
            data = make_png(w, h, 6, 8, rgba_to_filtered(pixels, w, h, (f,)))
            img = px.decode_png_bytes(data)
            got = b"".join(bytes((img.planes[0][i], img.planes[1][i],
                                  img.planes[2][i], img.planes[3][i]))
                           for i in range(w * h))
            self.assertEqual(got, bytes(pixels), "滤镜 %d 解码不一致" % f)

    def test_mixed_filter_cycle(self):
        w, h = 5, 6
        pixels = bytes([(i * 7) & 0xFF for i in range(w * h * 4)])
        data = make_png(w, h, 6, 8, rgba_to_filtered(pixels, w, h, (0, 1, 2, 3, 4)))
        img = px.decode_png_bytes(data)
        self.assertEqual(img.planes[0], bytes(pixels[0::4]))

    def test_illegal_filter_byte(self):
        data = make_png(1, 1, 6, 8, [b"\x09" + bytes(4)])
        with self.assertRaises(px.PngError):
            px.decode_png_bytes(data)

    def test_crc_mismatch_rejected(self):
        data = make_png(2, 2, 6, 8, extent_rows(2, 2, (1, 2, 3, 255)),
                        corrupt_crc=True)
        with self.assertRaises(px.PngError) as ctx:
            px.decode_png_bytes(data)
        self.assertIn("CRC", str(ctx.exception))

    def test_not_a_png(self):
        with self.assertRaises(px.PngError):
            px.decode_png_bytes(b"GIF89a.......")

    def test_truncated_chunk(self):
        data = make_png(2, 2, 6, 8, extent_rows(2, 2, (9, 9, 9, 9)))
        with self.assertRaises(px.PngError):
            px.decode_png_bytes(data[:-6])

    def test_interlaced_rejected_explicitly(self):
        data = make_png(2, 2, 6, 8, extent_rows(2, 2, (1, 1, 1, 1)), interlace=1)
        with self.assertRaises(px.PngError) as ctx:
            px.decode_png_bytes(data)
        self.assertIn("Adam7", str(ctx.exception))

    def test_extra_bytes_rejected(self):
        # 声明 2x2,却塞了 3 行数据
        data = make_png(2, 2, 6, 8, extent_rows(2, 3, (1, 1, 1, 1)))
        with self.assertRaises(px.PngError) as ctx:
            px.decode_png_bytes(data)
        self.assertIn("未消费", str(ctx.exception))

    def test_truncated_rows(self):
        rows = extent_rows(2, 2, (1, 1, 1, 1))
        data = make_png(2, 2, 6, 8, rows[:1])
        with self.assertRaises(px.PngError):
            px.decode_png_bytes(data)

    def test_rgb_color_type(self):
        row = b"\x00" + bytes((10, 20, 30, 40, 50, 60))
        img = px.decode_png_bytes(make_png(2, 1, 2, 8, [row]))
        self.assertEqual(img.channels, 3)
        self.assertFalse(img.has_alpha)
        self.assertEqual(img.planes[3], b"\xff\xff")
        self.assertEqual(img.planes[0], bytes((10, 40)))

    def test_grayscale_and_gray_alpha(self):
        g = px.decode_png_bytes(make_png(3, 1, 0, 8, [b"\x00" + bytes((1, 2, 3))]))
        self.assertEqual(g.channels, 1)
        self.assertFalse(g.has_alpha)
        self.assertEqual(g.planes[0], bytes((1, 2, 3)))
        self.assertEqual(g.planes[1], bytes((1, 2, 3)))  # 灰度复制到 RGB
        ga = px.decode_png_bytes(
            make_png(2, 1, 4, 8, [b"\x00" + bytes((7, 255, 9, 0))]))
        self.assertTrue(ga.has_alpha)
        self.assertEqual(ga.channels, 2)
        self.assertEqual(ga.planes[0], bytes((7, 9)))
        self.assertEqual(ga.planes[3], bytes((255, 0)))

    def test_palette_with_trns(self):
        palette = [(255, 0, 0), (0, 255, 0), (0, 0, 255)]
        row = b"\x00" + bytes((0, 1, 2))
        img = px.decode_png_bytes(make_png(3, 1, 3, 8, [row], palette=palette,
                                           trns=bytes((0, 128))))
        self.assertTrue(img.has_alpha)
        self.assertEqual(img.planes[0], bytes((255, 0, 0)))
        self.assertEqual(img.planes[3], bytes((0, 128, 255)))
        self.assertEqual(img.palette_size, 3)

    def test_palette_index_out_of_range(self):
        palette = [(1, 1, 1)]
        row = b"\x00" + bytes((0, 1))
        with self.assertRaises(px.PngError):
            px.decode_png_bytes(make_png(2, 1, 3, 8, [row], palette=palette))

    def test_bit_depth_1_grayscale(self):
        # 1 位灰度:一字节 8 像素,0xAA = 1,0,1,0,1,0,1,0
        img = px.decode_png_bytes(make_png(8, 1, 0, 1, [b"\x00\xaa"]))
        self.assertEqual(img.planes[0], bytes((1, 0, 1, 0, 1, 0, 1, 0)))

    def test_bit_depth_16_takes_high_byte(self):
        # 16 位:0x1234,0x00FF → 高字节 0x12,0x00
        row = b"\x00" + bytes((0x12, 0x34, 0x00, 0xFF))
        img = px.decode_png_bytes(make_png(2, 1, 0, 16, [row]))
        self.assertEqual(img.planes[0], bytes((0x12, 0x00)))
        self.assertEqual(img.bit_depth, 16)

    def test_illegal_bit_depth_rejected(self):
        with self.assertRaises(px.PngError):
            px.decode_png_bytes(make_png(1, 1, 6, 4, [b"\x00" + bytes(4)]))


def extent_rows(width, height, color, filter_byte=0):
    return [bytes([filter_byte]) + bytes(color) * width for _ in range(height)]


# --------------------------------------------------------------------------- #
# 退出码三分语义 + 断言
# --------------------------------------------------------------------------- #
class TestRunAndExitCodes(_TmpDirMixin, unittest.TestCase):
    def run_tool(self, argv):
        """一次调用 -> (退出码, stdout, stderr)。"""
        out, err = io.StringIO(), io.StringIO()
        code = px.run(argv, stdout=out, stderr=err)
        return code, out.getvalue(), err.getvalue()

    def test_exit_0_without_assert(self):
        p = self.write_rgba("a.png", 2, 2, solid_rgba(2, 2, (1, 2, 3, 255)))
        code, out, _ = self.run_tool([p])
        self.assertEqual(code, 0)
        self.assertIn("width = 2", out)

    def test_exit_1_when_assert_fails(self):
        p = self.write_rgba("a.png", 2, 2, solid_rgba(2, 2, (1, 2, 3, 255)))
        code, out, _ = self.run_tool([p, "--assert", "distinct_colors>5"])
        self.assertEqual(code, 1)
        self.assertIn("[assert FAIL]", out)
        self.assertIn("断言不通过", out)

    def test_exit_0_when_assert_passes(self):
        p = self.write_rgba("a.png", 2, 2, solid_rgba(2, 2, (1, 2, 3, 255)))
        code, out, _ = self.run_tool([p, "--assert", "distinct_colors==1",
                                      "--assert", "width>=2"])
        self.assertEqual(code, 0)
        self.assertIn("全部断言通过", out)

    def test_exit_2_on_missing_file(self):
        code, _, err = self.run_tool([os.path.join(self.tmp, "nope.png")])
        self.assertEqual(code, 2)
        self.assertIn("打不开", err)

    def test_exit_2_on_broken_png(self):
        p = self.write("bad.png", b"\x89PNG\r\n\x1a\n" + b"junk")
        code, _, err = self.run_tool([p])
        self.assertEqual(code, 2)
        self.assertIn("错误", err)

    def test_exit_2_on_bad_assert_syntax(self):
        p = self.write_rgba("a.png", 1, 1, solid_rgba(1, 1, (0, 0, 0, 255)))
        code, _, err = self.run_tool([p, "--assert", "width>>3"])
        self.assertEqual(code, 2)
        self.assertIn("断言", err)

    def test_exit_2_on_unknown_metric(self):
        p = self.write_rgba("a.png", 1, 1, solid_rgba(1, 1, (0, 0, 0, 255)))
        code, out, _ = self.run_tool([p, "--assert", "no_such_metric>0"])
        self.assertEqual(code, 1)
        self.assertIn("没有指标", out)

    def test_exit_2_on_bad_region(self):
        p = self.write_rgba("a.png", 1, 1, solid_rgba(1, 1, (0, 0, 0, 255)))
        code, _, err = self.run_tool([p, "--region", "0,0,0,4"])
        self.assertEqual(code, 2)
        self.assertIn("w/h", err)

    def test_exit_2_on_bad_arguments(self):
        p = self.write_rgba("a.png", 1, 1, solid_rgba(1, 1, (0, 0, 0, 255)))
        code, _, _ = self.run_tool([p, "--no-such-flag"])
        self.assertEqual(code, 2)

    def test_bare_assert_means_nonzero(self):
        name, op, value = px.parse_assert("distinct_colors")
        self.assertEqual((name, op, value), ("distinct_colors", "!=", 0.0))

    def test_assert_operator_parsing(self):
        self.assertEqual(px.parse_assert("x >= -3.5"), ("x", ">=", -3.5))
        self.assertEqual(px.parse_assert("y!=0"), ("y", "!=", 0.0))

    def test_json_output_file(self):
        import json
        p = self.write_rgba("a.png", 2, 2, solid_rgba(2, 2, (1, 2, 3, 255)))
        jp = os.path.join(self.tmp, "sub", "m.json")
        code, _, _ = self.run_tool([p, "--json", jp, "--quiet"])
        self.assertEqual(code, 0)
        with open(jp, encoding="utf-8") as f:
            payload = json.load(f)
        self.assertEqual(payload["width"], 2)
        self.assertEqual(payload["has_alpha"], 1)
        self.assertEqual(payload["distinct_colors"], 1)

    def test_list_metrics_lists_keys(self):
        p = self.write_rgba("a.png", 2, 2, solid_rgba(2, 2, (1, 2, 3, 255)))
        code, out, _ = self.run_tool([p, "--list-metrics"])
        self.assertEqual(code, 0)
        self.assertIn("distinct_colors", out)


# --------------------------------------------------------------------------- #
# 主色统计
# --------------------------------------------------------------------------- #
class TestColorStats(_TmpDirMixin, unittest.TestCase):
    def run_tool(self, argv):
        """一次调用 -> (退出码, stdout, stderr)。"""
        out, err = io.StringIO(), io.StringIO()
        code = px.run(argv, stdout=out, stderr=err)
        return code, out.getvalue(), err.getvalue()

    def test_top_colors_and_shares(self):
        # 4x4:12 个 A + 4 个 B
        pixels = bytearray()
        for i in range(16):
            pixels += bytes((9, 9, 9, 255) if i < 12 else (200, 0, 0, 255))
        p = self.write_rgba("c.png", 4, 4, bytes(pixels))
        code, out, _ = self.run_tool([p, "--colors", "2"])
        self.assertEqual(code, 0)
        self.assertIn("共有 2 种颜色", out)
        self.assertIn("color_top_share = 0.750000", out)
        self.assertIn("color_01_share = 0.250000", out)
        self.assertIn("#090909ff", out)

    def test_solid_image_has_one_dominant_color(self):
        p = self.write_rgba("s.png", 8, 5, solid_rgba(8, 5, (64, 64, 64, 255)))
        code, out, _ = self.run_tool([p, "--colors", "3"])
        self.assertEqual(code, 0)
        self.assertIn("distinct_colors = 1", out)
        self.assertIn("color_top_share = 1.000000", out)
        # "一块纯色"HUD 面板没画出来:颜色种数就是 1
        code2, _, _ = self.run_tool([p, "--assert", "distinct_colors==1"])
        self.assertEqual(code2, 0)

    def test_alpha_is_part_of_color_identity(self):
        pixels = bytes((5, 5, 5, 255, 5, 5, 5, 0))
        p = self.write_rgba("a.png", 2, 1, pixels)
        code, out, _ = self.run_tool([p, "--colors", "2"])
        self.assertEqual(code, 0)
        self.assertIn("distinct_colors = 2", out)


# --------------------------------------------------------------------------- #
# 区域统计
# --------------------------------------------------------------------------- #
class TestRegionStats(_TmpDirMixin, unittest.TestCase):
    def run_tool(self, argv):
        """一次调用 -> (退出码, stdout, stderr)。"""
        out, err = io.StringIO(), io.StringIO()
        code = px.run(argv, stdout=out, stderr=err)
        return code, out.getvalue(), err.getvalue()

    def test_two_color_boundary_region_splits(self):
        # 4x4 上半(2 行)红,下半 2 行蓝,左上角像素是红 → 背景=红
        pixels = bytearray()
        for y in range(4):
            for x in range(4):
                pixels += bytes((255, 0, 0, 255) if y < 2 else (0, 0, 255, 255))
        p = self.write_rgba("b.png", 4, 4, bytes(pixels))
        code, out, _ = self.run_tool([p, "--region", "0,0,4,2", "--region", "0,2,4,2"])
        self.assertEqual(code, 0)
        self.assertIn("r0_nonbg_ratio = 0.000000", out)
        self.assertIn("r1_nonbg_ratio = 1.000000", out)
        self.assertIn("r0_colors = 1", out)
        self.assertIn("r1_colors = 1", out)
        # 纯色块里没有过渡像素
        self.assertIn("r0_has_transition = 0", out)
        self.assertIn("r0_contrast_luma = 0", out)
        self.assertIn("r1_contrast_luma = 0", out)

    def test_contrast_luma_span(self):
        pixels = bytes((0, 0, 0, 255, 255, 255, 255, 255))
        p = self.write_rgba("l.png", 2, 1, pixels)
        code, out, _ = self.run_tool([p, "--region", "0,0,2,1", "--assert",
                                   "r0_contrast_luma==255"])
        self.assertEqual(code, 0)
        self.assertIn("r0_mean_luma = 127.500000", out)

    def test_transition_color_detected_in_region(self):
        # 三列:黑 → 中灰 128 → 白,中灰是"既非背景也非主前景"的过渡色
        pixels = bytes((0, 0, 0, 255, 128, 128, 128, 255,
                        255, 255, 255, 255))
        p = self.write_rgba("t.png", 3, 1, pixels)
        code, out, _ = self.run_tool([p, "--region", "0,0,3,1"])
        self.assertEqual(code, 0)
        self.assertIn("r0_has_transition = 1", out)
        self.assertIn("r0_colors = 3", out)

    def test_region_clipped_and_out_of_bounds(self):
        p = self.write_rgba("c.png", 2, 2, solid_rgba(2, 2, (0, 0, 0, 255)))
        code, out, _ = self.run_tool([p, "--region", "1,1,10,10",
                                   "--region", "50,50,4,4"])
        self.assertEqual(code, 0)
        self.assertIn("超出画布已裁剪", out)
        self.assertIn("r0_w = 1", out)
        self.assertIn("r0_h = 1", out)
        self.assertIn("完全落在图外", out)
        self.assertIn("r1_nonbg_ratio = 0.000000", out)

    def test_region_background_override(self):
        # 左上角是白,显式把背景设成黑之后:白色部分全部算非背景
        pixels = bytes((255, 255, 255, 255, 0, 0, 0, 255))
        p = self.write_rgba("o.png", 2, 1, pixels)
        code, out, _ = self.run_tool([p, "--region", "0,0,2,1"])
        self.assertEqual(code, 0)
        self.assertIn("r0_nonbg_ratio = 0.500000", out)
        code2, out2, _ = self.run_tool([p, "--region", "0,0,2,1", "--bg", "0,0,0"])
        self.assertEqual(code2, 0)
        self.assertIn("r0_nonbg_ratio = 0.500000", out2)

    def test_bg_tolerance_absorbs_compression_noise(self):
        # 背景近似色 (2,2,2) 在 tol=0 下算非背景,在 tol=4 下算背景
        pixels = bytes((0, 0, 0, 255, 2, 2, 2, 255))
        p = self.write_rgba("n.png", 2, 1, pixels)
        _, out0, _ = self.run_tool([p, "--region", "0,0,2,1"])
        self.assertIn("r0_nonbg_ratio = 0.500000", out0)
        _, out4, _ = self.run_tool([p, "--region", "0,0,2,1", "--bg-tol", "4"])
        self.assertIn("r0_nonbg_ratio = 0.000000", out4)


# --------------------------------------------------------------------------- #
# 边沿平滑度(--edge-metric)
# --------------------------------------------------------------------------- #
class TestEdgeMetric(_TmpDirMixin, unittest.TestCase):
    def run_tool(self, argv):
        """一次调用 -> (退出码, stdout, stderr)。"""
        out, err = io.StringIO(), io.StringIO()
        code = px.run(argv, stdout=out, stderr=err)
        return code, out.getvalue(), err.getvalue()

    def _stripes(self, width, height, bg, fg, columns):
        """竖条:第 columns 列起为前景色。"""
        pixels = bytearray()
        for y in range(height):
            for x in range(width):
                pixels += bytes((*bg, 255) if x < columns else (*fg, 255))
        return bytes(pixels)

    def _antialiased(self, width, height, bg, fg, aa):
        pixels = bytearray()
        for y in range(height):
            for x in range(width):
                c = bg if x < 4 else (aa if x == 4 else fg)
                pixels += bytes((*c, 255))
        return bytes(pixels)

    def test_hard_edge_has_no_transition_pixels(self):
        p = self.write_rgba("h.png", 8, 8, self._stripes(8, 8, (0, 0, 0),
                                                         (255, 255, 255), 4))
        code, out, _ = self.run_tool([p, "--edge-metric"])
        self.assertEqual(code, 0)
        self.assertIn("edge_transition_pixels = 0", out)
        self.assertIn("edge_has_edges = 1", out)  # 有交界(邻域过渡)
        self.assertIn("edge_neighbor_transition = 16", out)  # 两列 × 8 行

    def test_antialiased_edge_has_transition_pixels(self):
        p = self.write_rgba("aa.png", 8, 8,
                            self._antialiased(8, 8, (0, 0, 0), (255, 255, 255),
                                              (128, 128, 128)))
        code, out, _ = self.run_tool([p, "--edge-metric"])
        self.assertEqual(code, 0)
        self.assertIn("edge_transition_pixels = 8", out)  # 一整列过渡色
        self.assertIn("edge_has_edges = 1", out)

    def test_aa_beats_hard_edge_with_fg_hint(self):
        hard = self.write_rgba("h.png", 8, 8,
                               self._stripes(8, 8, (0, 0, 0), (255, 255, 255), 4))
        soft = self.write_rgba("s.png", 8, 8,
                               self._antialiased(8, 8, (0, 0, 0), (255, 255, 255),
                                                 (128, 128, 128)))
        _, out_h, _ = self.run_tool([hard, "--edge-metric", "--fg", "255,255,255"])
        _, out_s, _ = self.run_tool([soft, "--edge-metric", "--fg", "255,255,255"])
        self.assertIn("edge_transition_pixels = 0", out_h)
        self.assertIn("edge_transition_pixels = 8", out_s)

    def test_solid_image_has_no_edges(self):
        p = self.write_rgba("s.png", 6, 6, solid_rgba(6, 6, (33, 33, 33, 255)))
        code, out, _ = self.run_tool([p, "--edge-metric"])
        self.assertEqual(code, 0)
        self.assertIn("edge_has_edges = 0", out)
        self.assertIn("edge_transition_pixels = 0", out)
        self.assertIn("edge_neighbor_transition = 0", out)
        self.assertIn("edge_fg_supported = 0", out)

    def test_band_limits_to_scene_region(self):
        # 上半 2 行是"UI 带"(带 AA 软边),下半 2 行是纯背景。
        # band 的正确用途是"把统计范围限定到场景区",不是把有效样本剪光。
        w, h = 10, 4
        pixels = bytearray()
        for y in range(h):
            for x in range(w):
                if y < 2:
                    c = (0, 0, 0) if x < 3 else ((128, 128, 128) if x == 3
                                                 else (255, 255, 255))
                else:
                    c = (0, 0, 0)
                pixels += bytes((*c, 255))
        p = self.write_rgba("band.png", w, h, bytes(pixels))
        _, full, _ = self.run_tool([p, "--edge-metric", "--fg", "255,255,255"])
        self.assertIn("edge_transition_pixels = 2", full)  # 1 列 × 2 行
        self.assertIn("edge_sample_valid = 1", full)
        # 把统计范围限定到下半(场景区):右下角放一个前景块,保证采样区里有真实数据
        pixels2 = bytearray(pixels)
        # (8,3) 像素改成前景色 255
        off = (3 * w + 8) * 4
        pixels2[off:off + 3] = bytes((255, 255, 255))
        p2 = self.write_rgba("band2.png", w, h, bytes(pixels2))
        _, band, _ = self.run_tool([p2, "--edge-metric", "--band", "0,2,10,2",
                                    "--fg", "255,255,255"])
        self.assertIn("edge_foreground_pixels = 1", band)
        self.assertIn("edge_sample_valid = 1", band)
        self.assertIn("edge_transition_pixels = 0", band)  # 场景区没有 AA 过渡色
        # 同一张图,不带 --fg:自动前景 = 最远离背景的色 = 白(UI 带里也有) → 有效
        _, band_auto, _ = self.run_tool([p2, "--edge-metric", "--band", "0,2,10,2"])
        self.assertIn("edge_sample_valid = 1", band_auto)
        # 显式给一个与背景同色的"前景"= 没有可区分前景 → 无效
        code_same, same, _ = self.run_tool([p2, "--edge-metric", "--band", "0,2,10,2",
                                            "--fg", "0,0,0"])
        self.assertEqual(code_same, 1)
        self.assertIn("edge_sample_valid = 0", same)

    def test_narrow_band_without_foreground_is_invalid_not_pass(self):
        # 踩过的坑:用比图窄的 band 去"抠掉 HUD",会把场景前景一起剪光,
        # 于是 transition_pixels=0。那是"没数据",必须报错,不能算通过。
        w, h = 10, 4
        pixels = bytearray()
        for y in range(h):
            for x in range(w):
                # 前景只在右半区(x>=7):左侧 5 列宽的采样区里一个都没有
                c = (255, 255, 255) if (x >= 7 and y < 2) else (0, 0, 0)
                pixels += bytes((*c, 255))
        p = self.write_rgba("narrow.png", w, h, bytes(pixels))
        code, out, _ = self.run_tool([p, "--edge-metric", "--band", "0,0,5,4"])
        self.assertEqual(code, 1, "采样区内没有前景时必须非 0 退出")
        self.assertIn("edge_foreground_pixels = 0", out)
        self.assertIn("edge_sample_valid = 0", out)
        self.assertIn("[edge FAIL]", out)
        self.assertIn("指标无效", out)
        # 显式放行才允许出数(但 sample_valid 仍是 0,报告里照样标明无效)
        code2, out2, _ = self.run_tool([p, "--edge-metric", "--band", "0,0,5,4",
                                        "--allow-empty-sample"])
        self.assertEqual(code2, 0)
        self.assertIn("edge_sample_valid = 0", out2)

    def test_full_image_sample_is_always_valid(self):
        # 全图统计的采样区是整幅图,只要图里有前景就有效
        p = self.write_rgba("f.png", 4, 4,
                            self._stripes(4, 4, (0, 0, 0), (255, 255, 255), 2))
        code, out, _ = self.run_tool([p, "--edge-metric"])
        self.assertEqual(code, 0)
        self.assertIn("edge_sample_valid = 1", out)

    def test_edge_region_limits_analysis(self):
        # 左右各有一条 AA 边,只统计右半边时应只看到一条
        w, h = 12, 4
        pixels = bytearray()
        for y in range(h):
            for x in range(w):
                c = (0, 0, 0)
                if x == 2 or x == 8:
                    c = (128, 128, 128)
                elif 2 < x < 8:
                    c = (255, 255, 255)
                pixels += bytes((*c, 255))
        p = self.write_rgba("e.png", w, h, bytes(pixels))
        _, full, _ = self.run_tool([p, "--edge-metric", "--fg", "255,255,255"])
        self.assertIn("edge_transition_pixels = 8", full)  # 两列 × 4 行
        _, right, _ = self.run_tool([p, "--edge-metric", "--edge-region", "6,0,6,4",
                                  "--fg", "255,255,255"])
        self.assertIn("edge_transition_pixels = 4", right)

    def test_edge_metric_assertable(self):
        p = self.write_rgba("aa.png", 8, 8,
                            self._antialiased(8, 8, (0, 0, 0), (255, 255, 255),
                                              (100, 100, 100)))
        code, _, _ = self.run_tool([p, "--edge-metric", "--assert",
                                 "edge_transition_pixels>=8", "--assert",
                                 "edge_has_edges==1"])
        self.assertEqual(code, 0)
        code_bad, _, _ = self.run_tool([p, "--edge-metric", "--assert",
                                     "edge_transition_pixels==0"])
        self.assertEqual(code_bad, 1)


# --------------------------------------------------------------------------- #
# 差异模式
# --------------------------------------------------------------------------- #
class TestDiff(_TmpDirMixin, unittest.TestCase):
    def run_tool(self, argv):
        """返回 (退出码, stdout, stderr 里的原文)。"""
        out, err = io.StringIO(), io.StringIO()
        code = px.run(argv, stdout=out, stderr=err)
        return code, out.getvalue(), err.getvalue()

    def test_identical_images(self):
        px1 = solid_rgba(4, 4, (10, 10, 10, 255))
        a = self.write_rgba("a.png", 4, 4, px1)
        b = self.write_rgba("b.png", 4, 4, px1, filter_cycle=(0, 2, 4))
        code, out, _ = self.run_tool([a, "--diff", b])
        self.assertEqual(code, 0)
        self.assertIn("diff_pixels = 0", out)
        self.assertIn("diff_bbox_x0 = -1", out)
        self.assertIn("逐像素完全一致", out)

    def test_single_pixel_difference_bbox(self):
        pa = bytearray(solid_rgba(4, 4, (0, 0, 0, 255)))
        pb = bytearray(pa)
        idx = (2 * 4 + 1) * 4          # x=1,y=2
        pb[idx:idx + 4] = bytes((255, 255, 255, 255))
        a = self.write_rgba("a.png", 4, 4, bytes(pa))
        b = self.write_rgba("b.png", 4, 4, bytes(pb))
        code, out, _ = self.run_tool([a, "--diff", b])
        self.assertEqual(code, 0)
        self.assertIn("diff_pixels = 1", out)
        self.assertIn("diff_bbox_x0 = 1", out)
        self.assertIn("diff_bbox_y0 = 2", out)
        self.assertIn("diff_bbox_x1 = 1", out)
        self.assertIn("diff_bbox_y1 = 2", out)
        self.assertIn("diff_bbox_w = 1", out)
        self.assertIn("diff_bbox_h = 1", out)
        self.assertIn("diff_max_channel_delta = 255", out)

    def test_diff_exports_image_and_marks_equal_area(self):
        pa = solid_rgba(4, 4, (0, 0, 0, 255))
        pb = bytearray(pa)
        pb[-4:] = bytes((9, 9, 9, 255))    # 右下角一个像素
        a = self.write_rgba("a.png", 4, 4, pa)
        b = self.write_rgba("b.png", 4, 4, bytes(pb))
        out_png = os.path.join(self.tmp, "diff", "d.png")
        code, out, _ = self.run_tool([a, "--diff", b, "--diff-out", out_png])
        self.assertEqual(code, 0)
        self.assertTrue(os.path.exists(out_png))
        self.assertIn("差异图已导出", out)
        img = px.load_png(out_png)
        self.assertEqual((img.width, img.height), (4, 4))
        # 相同处必须是暗灰 #202020(不是黑,"黑=相同"会与"差异为黑"混淆)
        self.assertEqual(img.rgba_tuple(0), (32, 32, 32, 255))
        # 差异像素按差值×4 放大:9*4=36
        self.assertEqual(img.rgba_tuple(15), (36, 36, 36, 255))

    def test_diff_size_mismatch_is_exit_2(self):
        a = self.write_rgba("a.png", 4, 4, solid_rgba(4, 4, (0, 0, 0, 255)))
        b = self.write_rgba("b.png", 5, 4, solid_rgba(5, 4, (0, 0, 0, 255)))
        code, _, err = self.run_tool([a, "--diff", b])
        self.assertEqual(code, 2)
        self.assertIn("尺寸不一致", err)

    def test_diff_out_without_diff_is_exit_2(self):
        a = self.write_rgba("a.png", 2, 2, solid_rgba(2, 2, (0, 0, 0, 255)))
        code, _, err = self.run_tool([a, "--diff-out", os.path.join(self.tmp, "x.png")])
        self.assertEqual(code, 2)
        self.assertIn("--diff", err)

    def test_diff_assert_zero_diff(self):
        a = self.write_rgba("a.png", 3, 3, checker_rgba(3, 3, (0, 0, 0, 255),
                                                        (255, 255, 255, 255)))
        b = self.write_rgba("b.png", 3, 3, checker_rgba(3, 3, (0, 0, 0, 255),
                                                        (255, 255, 255, 255)),
                            filter_cycle=(4,))
        code, _, _ = self.run_tool([a, "--diff", b, "--assert", "diff_pixels==0"])
        self.assertEqual(code, 0)

    def test_diff_counts_alpha_only_change_as_semantic_equal(self):
        pa = solid_rgba(2, 1, (0, 0, 0, 255))
        pb = bytes((0, 0, 0, 0, 0, 0, 0, 0))
        a = self.write_rgba("a.png", 2, 1, pa)
        b = self.write_rgba("b.png", 2, 1, pb)
        _, out, _ = self.run_tool([a, "--diff", b])
        self.assertIn("diff_pixels = 2", out)
        self.assertIn("diff_semantic_pixels = 0", out)


# --------------------------------------------------------------------------- #
# alpha=0 边界
# --------------------------------------------------------------------------- #
class TestAlphaZero(_TmpDirMixin, unittest.TestCase):
    def run_tool(self, argv):
        """一次调用 -> (退出码, stdout, stderr)。"""
        out, err = io.StringIO(), io.StringIO()
        code = px.run(argv, stdout=out, stderr=err)
        return code, out.getvalue(), err.getvalue()

    def test_fully_transparent_image(self):
        # RGB 有值但 alpha 全 0:不能因此报"无 alpha",也不能把它当"有内容"
        pixels = bytes((200, 100, 50, 0)) * 9
        p = self.write_rgba("t.png", 3, 3, pixels)
        code, out, _ = self.run_tool([p, "--colors", "2", "--region", "0,0,3,3"])
        self.assertEqual(code, 0)
        self.assertIn("has_alpha = 1", out)
        self.assertIn("alpha_min = 0", out)
        self.assertIn("alpha_max = 0", out)
        self.assertIn("has_binary_alpha = 0", out)
        # 同一种半透明颜色仍然只有 1 种颜色
        self.assertIn("distinct_colors = 1", out)
        self.assertIn("r0_alpha_min = 0", out)

    def test_alpha_zero_vs_alpha_255_same_rgb(self):
        a = bytes((7, 7, 7, 255))
        p = self.write_rgba("z.png", 1, 1, bytes((7, 7, 7, 0)))
        code, out, _ = self.run_tool([p, "--assert", "alpha_min==0",
                                   "--assert", "width==1"])
        self.assertEqual(code, 0)
        img = px.load_png(p)
        self.assertEqual(img.planes[3], b"\x00")
        self.assertNotEqual(img.planes[3], a[3:])

    def test_binary_alpha_flagged(self):
        pixels = bytes((1, 1, 1, 0, 1, 1, 1, 255))
        p = self.write_rgba("b.png", 2, 1, pixels)
        _, out, _ = self.run_tool([p])
        self.assertIn("has_binary_alpha = 1", out)

    def test_1x1_alpha_zero(self):
        p = self.write_rgba("1.png", 1, 1, bytes((0, 0, 0, 0)))
        code, out, _ = self.run_tool([p, "--edge-metric", "--region", "0,0,1,1"])
        self.assertEqual(code, 0)
        self.assertIn("edge_has_edges = 0", out)
        self.assertIn("r0_nonbg_ratio = 0.000000", out)


# --------------------------------------------------------------------------- #
# 使用纪律(软约束的硬落地):放行开关不许进自动化验收
# --------------------------------------------------------------------------- #
class TestUsageDiscipline(unittest.TestCase):
    """`--allow-empty-sample` 只允许出现在这些地方:
      * png_inspect.py 本体(定义 + 帮助文本)
      * docs/(说明它为什么存在、以及禁止用于门控)
      * 本文件里专门检查"放行后仍标记无效"的那一条用例
    任何自动化验收(ctest / 门控脚本 / 其它测试)里出现它,就等于把"指标无效"
    当成"通过" —— 那正是这条工具要防的事。
    """

    FLAG = "--allow-empty-sample"
    ALLOWED = ("png_inspect.py", "PNG_INSPECT.md", "test_png_inspect.py")

    def test_flag_only_appears_in_allowed_places(self):
        tools_dir = _TOOLS
        mb_dir = os.path.dirname(tools_dir)          # MagicBlocks/
        offenders = []
        for root, dirs, files in os.walk(mb_dir):
            # 跳过产物目录与版本库
            dirs[:] = [d for d in dirs if d not in
                       ("build", "build-android", "build-asan", ".git",
                        "__pycache__", "tmp")]
            for name in files:
                if name in self.ALLOWED:
                    continue
                if not name.endswith((".py", ".ps1", ".txt", ".cmake",
                                      ".json", ".yml", ".yaml", ".md")):
                    continue
                path = os.path.join(root, name)
                try:
                    with open(path, encoding="utf-8", errors="ignore") as f:
                        text = f.read()
                except OSError:
                    continue
                if self.FLAG in text:
                    offenders.append(os.path.relpath(path, mb_dir))
        self.assertEqual(offenders, [],
                         "`%s` 是人工排查开关,不允许出现在自动化验收/门控里: %s"
                         % (self.FLAG, offenders))

    def test_ctest_config_does_not_use_flag(self):
        # 单独再确认一遍 CMake/测试配置(最常见的落点)
        mb_dir = os.path.dirname(_TOOLS)
        cmake = os.path.join(mb_dir, "CMakeLists.txt")
        if not os.path.exists(cmake):
            self.skipTest("没有 CMakeLists.txt")
        with open(cmake, encoding="utf-8", errors="ignore") as f:
            self.assertNotIn(self.FLAG, f.read())


# --------------------------------------------------------------------------- #
# 编码器自洽(导出差异图 / 测试构造都依赖它)
# --------------------------------------------------------------------------- #
class TestEncoder(unittest.TestCase):
    def test_roundtrip_all_filters(self):
        w, h = 3, 5
        pixels = bytes([(i * 13) & 0xFF for i in range(w * h * 4)])
        for f in range(5):
            data = px.encode_png_rgba(w, h, pixels, filters=(f,))
            img = px.decode_png_bytes(data)
            got = b"".join(bytes((img.planes[0][i], img.planes[1][i],
                                  img.planes[2][i], img.planes[3][i]))
                           for i in range(w * h))
            self.assertEqual(got, pixels)

    def test_roundtrip_1x1(self):
        data = px.encode_png_rgba(1, 1, bytes((1, 2, 3, 4)))
        img = px.decode_png_bytes(data)
        self.assertEqual(img.rgba_tuple(0), (1, 2, 3, 4))

    def test_bad_length_rejected(self):
        with self.assertRaises(ValueError):
            px.encode_png_rgba(2, 2, b"\x00" * 8)


if __name__ == "__main__":
    unittest.main(verbosity=2)
