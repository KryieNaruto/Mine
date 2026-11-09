// MagicBlocks UI 消费层 —— 帧渲染器桩(IFrameRenderer 的 Vulkan 侧落地)
//
// ⚠ 本文件在 `src/backend/vulkan/` 下,**是 SDK 后端的文件,不是 UI 逻辑**。
// 放这里的唯一原因是 G3 门控:backend 之外禁止出现任何 Vk* 类型。UI 消费层
// (`src/ui/`)因此对 Vulkan 完全无感,符合 spec §2「UI 层只调公共头」。
//
// 它承担的是**桩**的角色:SDK 的 L1 ICommandList / L2 IRenderGraph 还没落地,
// 所以今天的渲染表现为:
//   - 离屏路径(§5.5):把 UI 图元合批后作为**实例化四边形**渲染进离屏 image
//     (无几何着色器,gl_InstanceIndex 驱动),再读回 RGBA8 —— 这是 spec §10
//     验收门 5「HomeScreen + 空 FeatureHost 的离屏截图进 golden」赖以成立的通路;
//   - 主路径(§5.2):录制窗口目标帧时**明确返回 Unsupported**。
//     这是刻意的:主路径要求"场景 + UI 同帧同 image",而在 L1/L2 落地前,
//     UI pass 的附件只可能是离屏 image(没有 acquire/present 通路)。假装成功
//     会让 UI 层的 Presenter 在真设备上静默空转 —— 违反不回退原则。
//
// 替换路径(SDK L1/L2 落地后):
//   1. `record_ui_instances()` 换成 ICommandList::drawInstanced 调用;
//   2. `render_into_device_image()` 改为在调用方给定的 command buffer 内录
//      [Scene pass] + [UI pass(LoadOp::Load)],并转 PRESENT 布局;
//   3. 签名不变,UI 层零改动。
#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "backend/vulkan/vk_common.h"
#include "backend/vulkan/vulkan_target.h"
#include "magicblocks/resources/memory.h"
#include "magicblocks/ui/frame_renderer.h"

namespace magicblocks::backend {

// 一个 UI 图元解析成 GPU 侧的实例参数(矩形 + 颜色)。与 backend 无关的 POD,
// 是 UI 层与 backend 之间的唯一数据契约。
struct UiInstance {
  float x0 = 0.f, y0 = 0.f, x1 = 0.f, y1 = 0.f;
  float r = 0.f, g = 0.f, b = 0.f, a = 1.f;
};

// UI 覆盖层 pass 的 Vulkan 实现(对 OffscreenTarget 录制)。
// 每一帧:重置 instance 数组(不释放) → 提交 instance → 一次 drawInstanced。
class UiOverlayPass {
 public:
  UiOverlayPass(const VulkanContext& ctx, OffscreenTarget* target);
  ~UiOverlayPass();

  UiOverlayPass(const UiOverlayPass&) = delete;
  UiOverlayPass& operator=(const UiOverlayPass&) = delete;

  device::Status init();

  // 预分配 instance 上限(超了明确报错,不静默丢图元)。
  device::Status set_capacity(std::uint32_t max_instances);

  // 一帧的实例列表。按 UI 图元顺序追加 → 绘制顺序确定 → golden 可复现。
  // 实心矩形 = 1 个实例;描边矩形 = 4 个实例(四条边);线段 = 1 个实例(退化的细矩形)。
  void reset_instances() { instances_.clear(); }
  device::Status append_rect(float x0, float y0, float x1, float y1, float r, float g, float b,
                             float a);
  device::Status append_stroke(float x0, float y0, float x1, float y1, float r, float g, float b,
                               float a);
  device::Status append_line(float ax, float ay, float bx, float by, float width, float r, float g,
                             float b, float a);

  // 录制并提交一次 UI pass(单次 submit;本函数只负责"这一帧的 UI"这一件事,
  // 供 IFrameRenderer 的离屏路径调用)。
  device::Status render_offscreen();

  // 读回离屏结果(仅金标/CLI 用;规范上是离屏路径才允许的读回)。
  device::Status readback(std::uint8_t* out, std::size_t bytes);

  std::uint32_t instance_count() const;
  std::uint32_t draw_call_count() const { return draw_calls_; }

  static std::uint32_t max_instances_hard_limit();

 private:
  device::Status ensure_pipeline();
  void destroy_pipeline();

  VulkanContext ctx_;
  OffscreenTarget* target_ = nullptr;

  // 预分配的实例数据(host-visible,一帧内一次上传;稳态帧 0 次分配)
  std::vector<UiInstance> instances_;
  std::uint32_t capacity_ = 0;

  VkBuffer instance_buffer_ = VK_NULL_HANDLE;
  // 实例缓冲的内存来自 L1 分页分配器的**桶内子块**(MemoryKind::Uniform),持久映射:
  // 稳态帧只 memcpy,0 次分配、0 次 map/unmap(G4 + spec §6 G5)。
  resources::ResourceId instance_chunk_{};
  std::uint8_t* instance_mapped_ = nullptr;
  VkRenderPass render_pass_ = VK_NULL_HANDLE;
  VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
  VkPipeline pipeline_ = VK_NULL_HANDLE;
  VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
  VkDescriptorPool desc_pool_ = VK_NULL_HANDLE;
  VkDescriptorSet desc_set_ = VK_NULL_HANDLE;
  std::uint32_t draw_calls_ = 0;
  bool ready_ = false;
};

// 离屏 UI 图元渲染器。
//
// 路线变更(调研 v2 §7)后它**不再**实现 ui::IFrameRenderer:
// 上屏路径已改为壳层的 ISceneFrameSink,UI 合成不再由 SDK 承担。
// 这个类现在只负责一件事 —— 把 UI 图元离屏画出来并读回(CI / golden 用)。
class VulkanFrameRenderer final {
 public:
  VulkanFrameRenderer(const VulkanContext& ctx, OffscreenTarget* offscreen);
  ~VulkanFrameRenderer();

  // 离屏路径:UI 图元 → 实例化渲染 → 读回。
  bool render_to_memory(ui::u32 width, ui::u32 height, std::uint8_t* out_rgba8,
                        ui::u32 row_bytes);

  // UI 层在每帧绘制前调用,把图元灌进来(见 ui_app.cpp 的调用点)。
  UiOverlayPass& overlay_pass() { return *pass_; }

 private:
  VulkanContext ctx_;
  OffscreenTarget* offscreen_ = nullptr;
  std::unique_ptr<UiOverlayPass> pass_;
};

}  // namespace magicblocks::backend
