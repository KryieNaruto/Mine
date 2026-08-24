#include <gtest/gtest.h>

#include <cstdio>
#include <cstring>
#include <vector>

#include "core/render/image_io.h"
#include "core/render/offscreen.h"
#include "core/render/pipeline.h"
#include "core/render/vulkan_context.h"

namespace easypainter::render {

TEST(ImageIO, WritesPngFile) {
  constexpr const char* kPath = "/tmp/easypainter_imageio_test.png";
  std::vector<uint8_t> rgba(4 * 2 * 2, 255);  // 2x2 全白 RGBA
  ASSERT_TRUE(write_png(kPath, 2, 2, rgba));

  FILE* f = std::fopen(kPath, "rb");
  ASSERT_NE(f, nullptr);
  unsigned char magic[8] = {0};
  ASSERT_EQ(std::fread(magic, 1, 8, f), 8u);
  std::fclose(f);
  std::remove(kPath);

  const unsigned char kPngMagic[8] = {0x89, 0x50, 0x4E, 0x47,
                                      0x0D, 0x0A, 0x1A, 0x0A};
  EXPECT_EQ(std::memcmp(magic, kPngMagic, 8), 0);
}

// 离屏渲染冒烟测试:在 lavapipe 上真实渲染一条折线,断言输出存在红色笔画像素。
TEST(OffscreenRender, ProducesImageOnLavapipe) {
  VulkanContext ctx;
  ASSERT_TRUE(ctx.init()) << "VulkanContext 初始化失败(需要 lavapipe 软件光栅)";
  Pipeline pipeline;
  ASSERT_TRUE(pipeline.init(ctx)) << "Pipeline 初始化失败";

  const std::vector<stroke::Vec2> pts = {
      {0.1f, 0.1f}, {0.5f, 0.5f}, {0.9f, 0.1f}};
  constexpr uint32_t kSize = 64;
  auto res = render_offscreen(ctx, pipeline, pts, kSize, kSize);
  ASSERT_EQ(res.width, kSize);
  ASSERT_EQ(res.height, kSize);
  ASSERT_EQ(res.rgba.size(), static_cast<size_t>(kSize) * kSize * 4);

  bool has_red = false;
  for (size_t i = 0; i < res.rgba.size(); i += 4) {
    if (res.rgba[i] > 200 && res.rgba[i + 1] < 60 && res.rgba[i + 2] < 60) {
      has_red = true;
      break;
    }
  }
  EXPECT_TRUE(has_red) << "渲染输出无红色笔画像素";
}

}  // namespace easypainter::render
