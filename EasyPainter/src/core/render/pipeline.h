#pragma once

#include <cstdint>
#include <vector>

#include <vulkan/vulkan.h>

#include "core/stroke/types.h"

namespace easypainter::render {

class VulkanContext;

// 笔画绘制管线:render pass + 图形管线 + 嵌入的 shader(.spv 构建期由 glslc 生成)。
class Pipeline {
 public:
  Pipeline() = default;
  ~Pipeline();
  Pipeline(const Pipeline&) = delete;
  Pipeline& operator=(const Pipeline&) = delete;

  // 创建 render pass + pipeline(shader 从生成的 stroke_shaders.h 取)。
  // color_format 默认 R8G8B8A8_UNORM(headless 离屏);windowed 传 swapchain 格式。
  bool init(const VulkanContext& ctx,
            VkFormat color_format = VK_FORMAT_R8G8B8A8_UNORM);

  // 在已开始的 render pass 内记录一次笔画绘制。顶点 buffer 由调用方创建并负责在
  // 命令提交执行完后释放(避免 use-after-free)。pts 经 (scale, offset) 映到归一化 [0,1]。
  void draw(VkCommandBuffer cmd, VkBuffer vertex_buffer, uint32_t vertex_count,
            uint32_t w, uint32_t h, float scale = 1.0f, float ox = 0.0f,
            float oy = 0.0f) const;

  VkRenderPass render_pass() const { return render_pass_; }

 private:
  VkDevice device_ = VK_NULL_HANDLE;
  VkRenderPass render_pass_ = VK_NULL_HANDLE;
  VkPipelineLayout layout_ = VK_NULL_HANDLE;
  VkPipeline pipeline_ = VK_NULL_HANDLE;
};

}  // namespace easypainter::render
