#pragma once

#include <cstdint>
#include <vector>

#include "core/render/pipeline.h"
#include "core/render/vulkan_context.h"
#include "core/stroke/types.h"

namespace easypainter::render {

// 离屏渲染结果:RGBA8 像素。
struct OffscreenResult {
  uint32_t width = 0;
  uint32_t height = 0;
  std::vector<uint8_t> rgba;
};

// 把归一化 [0,1] 点离屏渲染为 w×h 的 RGBA8 图像。
OffscreenResult render_offscreen(const VulkanContext& ctx, const Pipeline& pipeline,
                                 const std::vector<stroke::Vec2>& pts, uint32_t w,
                                 uint32_t h);

}  // namespace easypainter::render
