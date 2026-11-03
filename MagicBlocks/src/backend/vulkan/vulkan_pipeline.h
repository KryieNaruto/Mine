// MagicBlocks —— Vulkan 后端:L1 管线(R2 / R4 / G1 / P12)
//
// spec R2 / R4 / C4 / G1 / P12 的落地(plans-l1 §3):
//   - **只建 push constant range,不建描述符集布局(R4)**:pSetLayouts / setLayoutCount
//     恒为 0,descriptor_set_count() 恒返回 0 —— 这是 Pipeline.UsesPushConstantsOnly
//     的断言点。"少量 UBO 页 + 动态色表纹理"本轮同样不建描述符集。
//   - **polygonMode 恒 VK_POLYGON_MODE_FILL**:RasterState 里根本没有 polygon_mode
//     字段,线框模式既不可表达也不可传进来(R2 明文不支持)。
//   - **无几何/细分着色器**:ShaderStage 只有 Vertex/Fragment/Compute,创建时按
//     "图形/计算"两条互斥规则校验,不需要也不存在"拒绝几何着色器"的分支(G1 结构性保证)。
//   - **P12 全深度状态**:depthTest/depthWrite/depthOp/depthConstWrite 全部落到
//     VkPipelineDepthStencilStateCreateInfo(depthConstWrite 通过 depthBoundsTest +
//     depthBounds 表达"整片恒定深度写入")。
//   - **VkRenderPass 兼容性**:PipelineDesc 里没有附件格式字段(那是 RenderPassDesc
//     的事),因此本类按"L1 的 pass 形状"建一张**兼容的**(compatible)VkRenderPass,
//     供 vkCmdBeginRenderPass 使用;附件格式与 MSAA 在可表达时取自一个可选的
//     AttachmentShape 参数,未给出则用 1 采样 RGBA8 颜色附件(与 L0 离屏目标一致)。
//
// 纯软件态(不给 ctx):做全部参数校验并给出 hash,不建任何 Vk 对象 —— 校验逻辑
// 因此可以在无 Vulkan 设备的 ctest 里被完整覆盖。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "backend/vulkan/vk_common.h"
#include "magicblocks/device/result.h"
#include "magicblocks/resources/pipeline.h"
// VulkanContext / backend::Status 别名(见 vulkan_buffer.h 的同一条注释)。
#include "backend/vulkan/vulkan_target.h"

namespace magicblocks::backend {

using resources::CullMode;
using resources::DepthState;
using resources::FrontFace;
using resources::IPipeline;
using resources::PipelineDesc;
using resources::PrimitiveType;
using resources::ShaderModule;
using resources::ShaderStage;
using resources::u32;
using resources::u64;

// 管线兼容的附件形状(可选)。不给时按 {RGBA8, 1 采样, 无深度} 建 pass:
// 那是 L0 离屏目标的形状,L1 的命令列表也用它(裁定 2:渲染附件仍走 IRenderTarget)。
struct AttachmentShape {
  device::Format color_format = device::Format::RGBA8_UNORM;
  bool has_depth = false;
  device::Format depth_format = device::Format::D16_UNORM;
  u32 samples = 1;
};

// 图形管线的哈希/生命周期缓存由调用方(设备层)负责;本类只负责"一个 desc → 一个管线"。
class VulkanPipeline final : public IPipeline {
 public:
  // 纯软件态。
  explicit VulkanPipeline(const PipelineDesc& desc);
  // 设备态(建真实 VkPipeline;compute=true 时建计算管线)。
  VulkanPipeline(const VulkanContext& ctx, const PipelineDesc& desc);
  VulkanPipeline(const VulkanContext& ctx, const PipelineDesc& desc,
                 const AttachmentShape& shape);

  ~VulkanPipeline() override;

  VulkanPipeline(const VulkanPipeline&) = delete;
  VulkanPipeline& operator=(const VulkanPipeline&) = delete;

  device::Status init();

  // ------------------------------------------------------------ IPipeline(R2)
  const PipelineDesc& desc() const override { return desc_; }
  bool is_compute() const override { return desc_.compute; }
  // R4:本轮不建描述符集 → 恒为 0。
  u32 descriptor_set_count() const override { return 0; }
  // 实际生效的 push constant 字节数(validate() 失败时保持 0)。
  u32 push_constant_bytes() const override { return push_constant_bytes_; }
  u64 hash() const override { return hash_; }

  // ------------------------------------------------------------ 后端查询(gtest 用)
  bool has_vk_pipeline() const { return vk_pipeline_ != VK_NULL_HANDLE; }
  VkPipeline vk_pipeline() const { return vk_pipeline_; }
  VkPipelineLayout vk_pipeline_layout() const { return vk_layout_; }
  VkRenderPass vk_render_pass() const { return vk_render_pass_; }
  // polygonMode 实测值(恒 VK_POLYGON_MODE_FILL;R2 的"不支持线框"由它断言)。
  static VkPolygonMode polygon_mode() { return VK_POLYGON_MODE_FILL; }
  // 创建时用的附件形状(默认值见 AttachmentShape)。
  const AttachmentShape& attachment_shape() const { return shape_; }
  // 深度状态是否与 desc().depth 完全一致(设备态才会落进 VkPipelineDepthStencilState;
  // 纯软件态恒等于 desc().depth 的规范化结果,便于单测两种态同口径)。
  const DepthState& effective_depth_state() const { return effective_depth_; }
  // R4 的结构性证据:管线布局创建时用的 stage 位。
  u32 push_constant_stage_mask() const { return push_stage_mask_; }

  // 校验 PipelineDesc。单测可直接调以拿到具体错误码与 message:
  //   stages 空 → InvalidArgument;图形管线缺 Vertex/Fragment → InvalidArgument;
  //   图形管线混入 Compute → InvalidArgument;计算管线混入 Vertex/Fragment → InvalidArgument;
  //   push_constant_bytes 超限 → Unsupported;samples 非法 → Unsupported。
  // max_push_constant_size == 0 表示"未探测到能力值",此时按 128(移动端下限)判。
  static device::Status validate(const PipelineDesc& desc, u32 max_push_constant_size,
                                 u32 supported_samples_bitmask);

 private:
  struct StageModule {
    ShaderStage stage = ShaderStage::Vertex;
    const std::uint8_t* spirv = nullptr;
    std::size_t size_bytes = 0;
  };

  device::Status build_graphics();
  device::Status build_compute();
  device::Status create_render_pass();

  VulkanContext ctx_{};
  bool has_ctx_ = false;
  PipelineDesc desc_{};
  AttachmentShape shape_{};

  u32 push_constant_bytes_ = 0;
  u32 push_stage_mask_ = 0;
  u64 hash_ = 0;
  DepthState effective_depth_{};

  VkPipeline vk_pipeline_ = VK_NULL_HANDLE;
  VkPipelineLayout vk_layout_ = VK_NULL_HANDLE;
  VkRenderPass vk_render_pass_ = VK_NULL_HANDLE;
  std::vector<VkShaderModule> vk_modules_;

  // 纯软件态下"已创建"的标记(hash/校验通过即为 true)。
  bool soft_ready_ = false;
};

// 把 desc 算成一个稳定的 64 位哈希(命令流按它去重;纯函数,单测可直接调)。
u64 hash_pipeline_desc(const PipelineDesc& desc);

}  // namespace magicblocks::backend
