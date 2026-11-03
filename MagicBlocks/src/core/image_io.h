// MagicBlocks —— 图像 IO:PNG 读写(不引入第三方依赖)
//
// L0 只需要 RGBA8 PNG。编码用 store 模式(无压缩正确性风险),
// 解码支持 store 与非交错 filter 0-4 的 zlib deflate 最小子集。
// 这样 CLI 落盘与 golden 比对都不需要外部库。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace magicblocks {

struct ImageRgba8 {
  std::uint32_t width = 0;
  std::uint32_t height = 0;
  std::vector<std::uint8_t> pixels;  // width*height*4,R,G,B,A

  std::size_t byte_size() const {
    return static_cast<std::size_t>(width) * height * 4u;
  }
};

// 写 PNG。失败时返回 false 并填 err。
bool write_png_rgba8(const std::string& path, const ImageRgba8& img, std::string* err);

// 读 PNG(仅 RGBA8 / RGB8 非交错,filter 0-4;store 与 deflate 两种块)。
bool read_png_rgba8(const std::string& path, ImageRgba8* out, std::string* err);

// 逐像素比较:返回差异像素数(任一通道差 > tolerance)。
std::size_t count_differing_pixels(const ImageRgba8& a, const ImageRgba8& b, int tolerance);

}  // namespace magicblocks
