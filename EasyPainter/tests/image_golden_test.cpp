#include <gtest/gtest.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "cli/example_points.h"
#include "core/render/image_io.h"
#include "core/render/offscreen.h"
#include "core/render/pipeline.h"
#include "core/render/vulkan_context.h"
#include "core/stroke/input_source.h"
#include "core/stroke/predictor.h"

#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

namespace easypainter::render {

#ifndef GOLDEN_DIR
#define GOLDEN_DIR "."
#endif

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

// 图像 golden:与 easypainter-cli 生成的基准 PNG 逐像素比对(容差阈值)。
// 基准用 lavapipe 软件光栅生成(记录于 README);同一 Predictor 默认参数复算。
TEST(OffscreenRender, MatchesBaselineGolden) {
  const std::string golden_path = std::string(GOLDEN_DIR) + "/golden_render.png";
  int w = 0, h = 0, comp = 0;
  unsigned char* base = stbi_load(golden_path.c_str(), &w, &h, &comp, 4);
  ASSERT_NE(base, nullptr) << "无法加载基准 PNG: " << golden_path;
  ASSERT_EQ(w, 320);
  ASSERT_EQ(h, 240);

  VulkanContext ctx;
  ASSERT_TRUE(ctx.init());
  Pipeline pipeline;
  ASSERT_TRUE(pipeline.init(ctx));

  // 复现 CLI 的建模路径:示例点 → build_events → predictor.predict
  stroke::Predictor predictor;
  const auto modeled =
      predictor.predict(stroke::build_events(cli::example_points()));
  ASSERT_FALSE(modeled.empty());

  auto res = render_offscreen(ctx, pipeline, modeled, 320, 240);
  ASSERT_EQ(res.rgba.size(), static_cast<size_t>(320) * 240 * 4);

  size_t mismatch = 0;
  for (size_t i = 0; i < res.rgba.size(); i += 4) {
    const int dr = std::abs(static_cast<int>(res.rgba[i]) - base[i]);
    const int dg = std::abs(static_cast<int>(res.rgba[i + 1]) - base[i + 1]);
    const int db = std::abs(static_cast<int>(res.rgba[i + 2]) - base[i + 2]);
    const int da = std::abs(static_cast<int>(res.rgba[i + 3]) - base[i + 3]);
    if (dr > 3 || dg > 3 || db > 3 || da > 3) ++mismatch;
  }
  stbi_image_free(base);

  // 允许 ≤1% 像素差异(抗锯齿边缘抖动);超过即判为与基准不一致。
  EXPECT_LT(mismatch, static_cast<size_t>(320) * 240 / 100)
      << "与基准图像像素差异过大: " << mismatch;
}

}  // namespace easypainter::render
