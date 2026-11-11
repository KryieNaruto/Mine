// MagicBlocks —— Vulkan 后端:L1 管线(R2 / R4 / G1 / P12)
//
// spec R2 / R4 / C4 / G1 / P12 的落地(plans-l1 §3):
//   - **默认只建 push constant range,不建描述符集布局(R4)**:
//     PipelineDesc::sampled_texture_slots == 0(默认)时 pSetLayouts / setLayoutCount
//     恒为 0,descriptor_set_count() 恒返回 0 —— 这是 Pipeline.UsesPushConstantsOnly
//     的断言点,也是 P0-1「既有 5 张 golden 逐字节不变」的结构性保证。
//   - **P0 纹理采样(裁定 X1)**:sampled_texture_slots > 0 时额外建**恰好 1 个**
//     VkDescriptorSetLayout(single combined image sampler，
//     descriptorCount = sampled_texture_slots),set=0 / binding=slot;
//     并配一个 VkDescriptorPool + 1 个 VkDescriptorSet,由
//     ICommandList::bind_sampled_texture 写入并 vkCmdBindDescriptorSets。
//     **不做** bindless / 纹理数组 / 多集。sampled_texture_slots == 0 的管线
//     不建 pool/set,资源占用与历史完全一致。
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

// 管线兼容的附件形状(必需,不再有"默认值")。
//
// 历史(2026-11-11 修):这里曾经是 `has_depth = false` 的默认值,而
// `create_pipeline(desc)` 走的正是这个默认构造 → **管线自带 pass 永远没有深度附件**。
// 后果(t2 报告 §3.3.1):
//   1) 任何带深度的目标都与管线 pass 不兼容 → 每帧都退回 create_pass(),
//      `PipelineDesc.samples` 对 render pass 完全无效;
//   2) L1 的 `clear_depth` 标志被静默丢弃;
//   3) 交换链一旦走到复用分支,`finalLayout` 又不是 PRESENT_SRC_KHR → present 全坏。
// 现在形状**必须由调用方按目标真实形状派生**,并在 init() 里做一致性校验;
// 不再有任何"猜一个默认值"的路径。
// 2026-11-11 (t13/g8): the shape value now lives in the public L1 header
// (resources::AttachmentShape) so that L3 can derive shaped pipelines without
// including any backend header. This alias keeps every existing backend call site
// source-compatible; the two definitions are **identical field for field**.
using AttachmentShape = resources::AttachmentShape;

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
  // R4 / P0:sampled_texture_slots == 0 → 0;> 0 → 1(本轮不做多集,恒不超过 1)。
  u32 descriptor_set_count() const override { return set_layouts_ ? 1u : 0u; }
  u32 sampled_texture_slots() const override { return desc_.sampled_texture_slots; }
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

  // ------------------------------------------------------------ P0 描述符集(X1)
  // 设备态且 sampled_texture_slots > 0 时非空;否则全是 VK_NULL_HANDLE。
  VkDescriptorSetLayout vk_set_layout() const { return set_layouts_; }
  VkDescriptorPool vk_descriptor_pool() const { return desc_pool_; }
  VkDescriptorSet vk_descriptor_set() const { return desc_set_; }
  bool has_descriptor_set() const { return desc_set_ != VK_NULL_HANDLE; }
  // 写某个 slot 的 combined image sampler。set 必须是本管线 set_layouts_ 的子集,
  // slot 必须 < sampled_texture_slots(越界 → InvalidArgument)。
  // 注意:VkDescriptorSet 的写入**不要求**命令缓冲处于录制态,但本后端统一由
  // VulkanCommandList 在提交前调用,保证"一帧内最后一次绑定生效"。
  //
  // cube_compatible(V1 立方体贴图):view 是 VIEW_TYPE_CUBE 时,描述符里的采样器
  // 必须带 VK_SAMPLER_CREATE_FLAG_CUBE_COMPATIBLE_BIT,否则是校验层 ERROR、
  // 采样行为未定义。采样器对象由调用方提供(见 backend::cube_sampler_for),
  // 这里只做"带了该位"的事后校验 —— 不静默替换成一个调用方没要的对象。
  device::Status write_sampled_texture(u32 slot, VkImageView view, VkSampler sampler,
                                       bool cube_compatible = false);

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

  // P0(X1):sampled_texture_slots > 0 时才建。set=0,binding=0..slots-1。
  VkDescriptorSetLayout set_layouts_ = VK_NULL_HANDLE;
  VkDescriptorPool desc_pool_ = VK_NULL_HANDLE;
  VkDescriptorSet desc_set_ = VK_NULL_HANDLE;
  // 建描述符集的对象(顺序与销毁相反)。
  device::Status create_descriptor_objects();

  // 纯软件态下"已创建"的标记(hash/校验通过即为 true)。
  bool soft_ready_ = false;
};

// 把 desc 算成一个稳定的 64 位哈希(命令流按它去重;纯函数,单测可直接调)。
u64 hash_pipeline_desc(const PipelineDesc& desc);

// 把 desc + 附件形状算成哈希。**基础哈希仍是 hash_pipeline_desc(desc)**,
// 附件形状只作为附加信息混合进来 —— 即"同一 desc 不同附件形状"得到不同 hash,
// 而"同一 desc 且形状为默认值"保持与历史 hash 完全一致(不破坏既有命令流 golden)。
//
// 为什么需要:VkPipeline 与 VkRenderPass 必须兼容。L1 的 create_pipeline(desc) 用
// 默认形状 {RGBA8, 无深度} 建 pass,管线就绑不进"带深度附件"的 pass。
// L3 的渲染器要画 3D(深度),所以必须让管线按目标形状建 pass。
u64 hash_pipeline_desc_with_shape(const PipelineDesc& desc, const AttachmentShape& shape);

}  // namespace magicblocks::backend
