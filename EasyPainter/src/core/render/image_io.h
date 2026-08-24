#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace easypainter::render {

// 把 RGBA8 像素(长度 ≥ w*h*4)写入 PNG 文件。成功返回 true。
bool write_png(const std::string& path, uint32_t w, uint32_t h,
               const std::vector<uint8_t>& rgba);

}  // namespace easypainter::render
