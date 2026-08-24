#include "core/render/image_io.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

namespace easypainter::render {

bool write_png(const std::string& path, uint32_t w, uint32_t h,
               const std::vector<uint8_t>& rgba) {
  if (rgba.size() < static_cast<size_t>(w) * h * 4) return false;
  if (w == 0 || h == 0) return false;
  const int ok = stbi_write_png(
      path.c_str(), static_cast<int>(w), static_cast<int>(h), 4, rgba.data(),
      static_cast<int>(w) * 4);
  return ok != 0;
}

}  // namespace easypainter::render
