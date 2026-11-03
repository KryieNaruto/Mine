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
#include "backend/vulkan/vulkan_ui_overlay.h"

#include "backend/vulkan/vulkan_allocator.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include "magicblocks_shaders.h"
// <windows.h> 鐨?min/max 瀹忎細鎶?std::min/std::max 鎷嗗潖(C2589)銆?
// 只在包含 Vulkan 头之后就地解除宏,不改全局编译选项。
#ifdef max
#undef max
#endif
#ifdef min
#undef min
#endif

namespace magicblocks::backend {

using namespace magicblocks::device;

namespace {

// 实例上限:G3 的 UI 顶点上限 8000 → 每实例 4 顶点 → 2048 实例。
// 超限**明确报错**,不静默丢图元(丢图元会让 UI 看起来"随机少一块")。
constexpr std::uint32_t kHardInstanceLimit = 2048;
// 每个实例在 SSBO 中占 2 个 vec4(rect + color)
constexpr std::uint32_t kVec4PerInstance = 2;
constexpr VkDeviceSize kBytesPerInstance = sizeof(float) * 4 * kVec4PerInstance;

VkShaderModule make_module(VkDevice dev, const std::uint32_t* code, std::size_t words) {
  VkShaderModuleCreateInfo ci{};
  ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  ci.codeSize = words * sizeof(std::uint32_t);
  ci.pCode = code;
  VkShaderModule m = VK_NULL_HANDLE;
  if (vkCreateShaderModule(dev, &ci, nullptr, &m) != VK_SUCCESS) return VK_NULL_HANDLE;
  return m;
}

}  // namespace

// ---------------------------------------------------------------- UiOverlayPass

UiOverlayPass::UiOverlayPass(const VulkanContext& ctx, OffscreenTarget* target)
    : ctx_(ctx), target_(target) {}

UiOverlayPass::~UiOverlayPass() {
  destroy_pipeline();
  if (instance_buffer_) vkDestroyBuffer(ctx_.device, instance_buffer_, nullptr);
  if (instance_chunk_.valid() && ctx_.allocator != nullptr) {
    ctx_.allocator->free(instance_chunk_);
    instance_chunk_ = resources::ResourceId{};
  }
  instance_mapped_ = nullptr;
}

std::uint32_t UiOverlayPass::max_instances_hard_limit() { return kHardInstanceLimit; }

Status UiOverlayPass::init() {
  if (!target_) return Status::Invalid("UiOverlayPass: target 为空");
  return ensure_pipeline();
}

Status UiOverlayPass::ensure_pipeline() {
  if (ready_) return Status::Ok_();

  const Extent2D ext = target_->extent();
  const VkFormat color_fmt = to_vk_format(target_->color_format());

  // --- 绑定:实例数组走 storage buffer(binding 0)---
  VkDescriptorSetLayoutBinding binding{};
  binding.binding = 0;
  binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  binding.descriptorCount = 1;
  binding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
  VkDescriptorSetLayoutCreateInfo dslci{};
  dslci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  dslci.bindingCount = 1;
  dslci.pBindings = &binding;
  VkResult r = vkCreateDescriptorSetLayout(ctx_.device, &dslci, nullptr, &set_layout_);
  if (r != VK_SUCCESS) return Status::InitFailed(fmt_err("vkCreateDescriptorSetLayout", r));

  VkDescriptorPoolSize pool_size{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
  VkDescriptorPoolCreateInfo dpci{};
  dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  dpci.maxSets = 1;
  dpci.poolSizeCount = 1;
  dpci.pPoolSizes = &pool_size;
  r = vkCreateDescriptorPool(ctx_.device, &dpci, nullptr, &desc_pool_);
  if (r != VK_SUCCESS) return Status::InitFailed(fmt_err("vkCreateDescriptorPool", r));

  VkDescriptorSetAllocateInfo dsai{};
  dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  dsai.descriptorPool = desc_pool_;
  dsai.descriptorSetCount = 1;
  dsai.pSetLayouts = &set_layout_;
  r = vkAllocateDescriptorSets(ctx_.device, &dsai, &desc_set_);
  if (r != VK_SUCCESS) return Status::InitFailed(fmt_err("vkAllocateDescriptorSets", r));

  // --- push constant:鐟欏棗褰涚亸鍝勵嚟 ---
  VkPushConstantRange push{};
  push.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
  push.offset = 0;
  push.size = sizeof(float) * 2;

  VkPipelineLayoutCreateInfo plci{};
  plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  plci.setLayoutCount = 1;
  plci.pSetLayouts = &set_layout_;
  plci.pushConstantRangeCount = 1;
  plci.pPushConstantRanges = &push;
  r = vkCreatePipelineLayout(ctx_.device, &plci, nullptr, &pipeline_layout_);
  if (r != VK_SUCCESS) return Status::InitFailed(fmt_err("vkCreatePipelineLayout", r));

  // --- render pass:LoadOp::Load(閸欑姴濮為崷銊ユ簚閺咁垯绠ｆ稉?spec 鎼?.3)---
  VkAttachmentDescription att{};
  att.format = color_fmt;
  att.samples = VK_SAMPLE_COUNT_1_BIT;
  att.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
  att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  att.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  att.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;  // 读回前必须转 TRANSFER_SRC

  VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  VkSubpassDescription sub{};
  sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  sub.colorAttachmentCount = 1;
  sub.pColorAttachments = &ref;

  VkSubpassDependency dep{};
  dep.srcSubpass = VK_SUBPASS_EXTERNAL;
  dep.dstSubpass = 0;
  dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  dep.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;

  VkRenderPassCreateInfo rpci{};
  rpci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
  rpci.attachmentCount = 1;
  rpci.pAttachments = &att;
  rpci.subpassCount = 1;
  rpci.pSubpasses = &sub;
  rpci.dependencyCount = 1;
  rpci.pDependencies = &dep;
  r = vkCreateRenderPass(ctx_.device, &rpci, nullptr, &render_pass_);
  if (r != VK_SUCCESS) return Status::InitFailed(fmt_err("vkCreateRenderPass(ui)", r));

  // --- 管线:实例化四边形 = 4 个程序化顶点 x N 实例(无 GS)---
  VkShaderModule vs = make_module(ctx_.device, shaders::kUi_overlayVert,
                                  shaders::kUi_overlayVertSize / sizeof(std::uint32_t));
  VkShaderModule fs = make_module(ctx_.device, shaders::kUi_overlayFrag,
                                  shaders::kUi_overlayFragSize / sizeof(std::uint32_t));
  if (!vs || !fs) {
    if (vs) vkDestroyShaderModule(ctx_.device, vs, nullptr);
    if (fs) vkDestroyShaderModule(ctx_.device, fs, nullptr);
    return Status::InitFailed("UI 着色器模块编译失败(见 logcat)");
  }

  VkPipelineShaderStageCreateInfo stages[2] = {};
  stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = vs;
  stages[0].pName = "main";
  stages[1] = stages[0];
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = fs;

  VkPipelineVertexInputStateCreateInfo vi{};
  vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
  VkPipelineInputAssemblyStateCreateInfo ia{};
  ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
  ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
  VkViewport vp{0.f, 0.f, static_cast<float>(ext.width), static_cast<float>(ext.height), 0.f, 1.f};
  VkRect2D sc{{0, 0}, {ext.width, ext.height}};
  VkPipelineViewportStateCreateInfo vps{};
  vps.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
  vps.viewportCount = 1;
  vps.pViewports = &vp;
  vps.scissorCount = 1;
  vps.pScissors = &sc;
  VkPipelineRasterizationStateCreateInfo rs{};
  rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
  rs.polygonMode = VK_POLYGON_MODE_FILL;
  rs.cullMode = VK_CULL_MODE_NONE;
  rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  rs.lineWidth = 1.f;
  VkPipelineMultisampleStateCreateInfo ms{};
  ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
  ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  // 旁路(spec §5.2 Pass 2):UI 图元在场景之后合成进同一 image
  VkPipelineDepthStencilStateCreateInfo ds{};
  ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
  ds.depthTestEnable = VK_FALSE;
  ds.depthWriteEnable = VK_FALSE;
  // alpha 濞ｅ嘲鎮?闂堢偤顣╂稊?SRC_ALPHA / ONE_MINUS_SRC_ALPHA
  VkPipelineColorBlendAttachmentState cba{};
  cba.colorWriteMask = 0xF;
  cba.blendEnable = VK_TRUE;
  cba.srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA;
  cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
  cba.colorBlendOp = VK_BLEND_OP_ADD;
  cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
  cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
  cba.alphaBlendOp = VK_BLEND_OP_ADD;
  VkPipelineColorBlendStateCreateInfo cb{};
  cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
  cb.attachmentCount = 1;
  cb.pAttachments = &cba;

  VkGraphicsPipelineCreateInfo pci{};
  pci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  pci.stageCount = 2;
  pci.pStages = stages;
  pci.pVertexInputState = &vi;
  pci.pInputAssemblyState = &ia;
  pci.pViewportState = &vps;
  pci.pRasterizationState = &rs;
  pci.pMultisampleState = &ms;
  pci.pDepthStencilState = &ds;
  pci.pColorBlendState = &cb;
  pci.layout = pipeline_layout_;
  pci.renderPass = render_pass_;
  r = vkCreateGraphicsPipelines(ctx_.device, VK_NULL_HANDLE, 1, &pci, nullptr, &pipeline_);
  vkDestroyShaderModule(ctx_.device, vs, nullptr);
  vkDestroyShaderModule(ctx_.device, fs, nullptr);
  if (r != VK_SUCCESS) return Status::InitFailed(fmt_err("vkCreateGraphicsPipelines(ui)", r));

  // --- 实例缓冲区:预分配到硬上限,稳态帧 0 次分配 ---
  Status s = set_capacity(kHardInstanceLimit);
  if (!s.ok()) return s;

  ready_ = true;
  return Status::Ok_();
}

Status UiOverlayPass::set_capacity(std::uint32_t max_instances) {
  if (max_instances == 0 || max_instances > kHardInstanceLimit) {
    return Status::Invalid("UiOverlayPass::set_capacity: 超出硬实例上限 " +
                           std::to_string(kHardInstanceLimit));
  }
  if (instance_buffer_ && capacity_ >= max_instances) {
    capacity_ = max_instances;  // 閸欘亝鏁圭紓鈺呪偓鏄忕帆鐎瑰綊鍣?娑撳秹鍣搁崚鍡涘帳
    return Status::Ok_();
  }
  const VkDeviceSize bytes = kBytesPerInstance * max_instances;
  VkBufferCreateInfo bci{};
  bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  bci.size = bytes;
  bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT;
  VkBuffer buf = VK_NULL_HANDLE;
  VkResult r = vkCreateBuffer(ctx_.device, &bci, nullptr, &buf);
  if (r != VK_SUCCESS) return Status::OOM(fmt_err("vkCreateBuffer(ui instances)", r));

  // G4「全仓零裸分配」:实例缓冲的内存**必须**来自 L1 分页分配器。
  // UI 实例是每帧重写的动态数据 → 走 Staging 桶(HostVisible 页,可持久映射)。
  //
  // VkBuffer 必须**按偏移绑定**到所在页的整块内存上,否则描述符写入会被验证层
  // 判为 "buffer is used with no memory bound" 并可能直接崩。分配器头刻意不暴露
  // VmaAllocation,所以用 page_memory_handle() 取 {内存句柄, 页内偏移},
  // 这里把它解释成 VkDeviceMemory + offset(与 VulkanBuffer 同一套做法)。
  if (ctx_.allocator == nullptr) {
    vkDestroyBuffer(ctx_.device, buf, nullptr);
    return Status::InitFailed("UiOverlayPass: 后端分配器未接入(L1 之后不允许裸分配)");
  }
  constexpr resources::MemoryKind kKind = resources::MemoryKind::Staging;
  if (ctx_.allocator->page_count(kKind) == 0) {
    Status rsv = ctx_.allocator->reserve_pages(kKind, 1);
    if (!rsv.ok()) {
      vkDestroyBuffer(ctx_.device, buf, nullptr);
      return Status::OOM("UiOverlayPass 预留实例页: " + rsv.message);
    }
  }
  auto mem = ctx_.allocator->allocate(kKind, bytes, 16);
  if (!mem.ok()) {
    vkDestroyBuffer(ctx_.device, buf, nullptr);
    return Status::OOM("UiOverlayPass 实例缓冲区: " + mem.message());
  }
  const resources::ResourceId chunk = mem.value().id;

  std::uintptr_t page_memory = 0;
  u64 page_offset = 0;
  if (!ctx_.allocator->page_memory_handle(chunk.page_id, &page_memory, &page_offset)) {
    ctx_.allocator->free(chunk);
    vkDestroyBuffer(ctx_.device, buf, nullptr);
    return Status::InitFailed("UiOverlayPass: 实例子块所在页没有可取的内存句柄");
  }
  r = vkBindBufferMemory(ctx_.device, buf, reinterpret_cast<VkDeviceMemory>(page_memory),
                         static_cast<VkDeviceSize>(page_offset + chunk.offset));
  if (r != VK_SUCCESS) {
    ctx_.allocator->free(chunk);
    vkDestroyBuffer(ctx_.device, buf, nullptr);
    return Status::InitFailed(fmt_err("vkBindBufferMemory(ui instances)", r));
  }

  void* mapped = mem.value().mapped;
  if (mapped == nullptr) {
    ctx_.allocator->free(chunk);
    vkDestroyBuffer(ctx_.device, buf, nullptr);
    return Status::InitFailed("UiOverlayPass: 实例缓冲未映射(桶选了设备页)");
  }

  // 旧的先销毁(只在上限扩大时发生,不属稳态帧)
  if (instance_buffer_) {
    vkDestroyBuffer(ctx_.device, instance_buffer_, nullptr);
    if (instance_chunk_.valid()) ctx_.allocator->free(instance_chunk_);
  }
  instance_buffer_ = buf;
  instance_chunk_ = chunk;
  instance_mapped_ = static_cast<std::uint8_t*>(mapped);
  capacity_ = max_instances;
  instances_.clear();
  instances_.reserve(max_instances);

  VkDescriptorBufferInfo dbi{instance_buffer_, 0, bytes};
  VkWriteDescriptorSet w{};
  w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  w.dstSet = desc_set_;
  w.dstBinding = 0;
  w.descriptorCount = 1;
  w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
  w.pBufferInfo = &dbi;
  vkUpdateDescriptorSets(ctx_.device, 1, &w, 0, nullptr);
  return Status::Ok_();
}

Status UiOverlayPass::append_rect(float x0, float y0, float x1, float y1, float r, float g,
                                  float b, float a) {
  if (instances_.size() >= capacity_) {
    return Status::Unsupported("UiOverlayPass: 鐎圭偘绶ラ弫鎷岀Т娑撳﹪妾?" + std::to_string(capacity_) +
                               "(UI drawcall 妫板嫮鐣荤憴?spec 鎼? G3)");
  }
  UiInstance inst;
  inst.x0 = std::min(x0, x1);
  inst.y0 = std::min(y0, y1);
  inst.x1 = std::max(x0, x1);
  inst.y1 = std::max(y0, y1);
  inst.r = r;
  inst.g = g;
  inst.b = b;
  inst.a = a;
  instances_.push_back(inst);
  return Status::Ok_();
}

Status UiOverlayPass::append_stroke(float x0, float y0, float x1, float y1, float r, float g,
                                    float b, float a) {
  constexpr float kT = 1.f;  // 线段宽度 1px(HUD 里所有线都是这个宽度)
  Status s = append_rect(x0, y0, x1, y0 + kT, r, g, b, a);  // 上边
  if (!s.ok()) return s;
  s = append_rect(x0, y1 - kT, x1, y1, r, g, b, a);  // 下边
  if (!s.ok()) return s;
  s = append_rect(x0, y0, x0 + kT, y1, r, g, b, a);  // 左边
  if (!s.ok()) return s;
  return append_rect(x1 - kT, y0, x1, y1, r, g, b, a);  // 右边
}

Status UiOverlayPass::append_line(float ax, float ay, float bx, float by, float width, float r,
                                  float g, float b, float a) {
  // 线段退化成轴对齐细矩形(demo 的 HUD 只用水平/垂直/斜线;斜线按包围盒画)
  // 线段退化成轴对齐细矩形(demo 的 HUD 只用水平/垂直/斜线;斜线按包围盒画,
  // 视觉上可接受且完全确定性)。真正的线段图元等 L3 IOverlay2D 落地后替换。
  const float t = std::max(1.f, width) * 0.5f;
  return append_rect(std::min(ax, bx) - t, std::min(ay, by) - t, std::max(ax, bx) + t,
                     std::max(ay, by) + t, r, g, b, a);
}

std::uint32_t UiOverlayPass::instance_count() const {
  return static_cast<std::uint32_t>(instances_.size());
}

Status UiOverlayPass::render_offscreen() {
  if (!ready_) {
    Status s = ensure_pipeline();
    if (!s.ok()) return s;
  }
  if (!target_) return Status::Invalid("UiOverlayPass: target 为空");
  if (instances_.empty()) {
    draw_calls_ = 0;
    return Status::Ok_();  // 閺?UI:娑撳秴缍?pass,娴ｅ棔绡冩稉宥囩暬婢惰精瑙?
  }

  // 1) 鐎圭偘绶ラ弫鐗堝祦 閳?閹镐椒绠欓弰鐘茬殸缂傛挸鍟?娑撯偓濞?memcpy)
  void* mapped = nullptr;
  VkDeviceSize bytes = kBytesPerInstance * instances_.size();
  // 持久映射页:直接 memcpy,不再 map/unmap(G4 + 稳态 0 分配)
  VkResult r;
  if (instance_mapped_ == nullptr) return Status::InitFailed("UiOverlayPass: 实例页未映射");
  std::memcpy(instance_mapped_, instances_.data(), static_cast<std::size_t>(bytes));

  // 2) 实例数据 memcpy 进持久映射缓冲,再一次性交给 GPU
  const Extent2D ext = target_->extent();
  VkImageViewCreateInfo vci{};
  vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  vci.image = target_->color_image();
  vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
  vci.format = to_vk_format(target_->color_format());
  vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  VkImageView view = VK_NULL_HANDLE;
  r = vkCreateImageView(ctx_.device, &vci, nullptr, &view);
  if (r != VK_SUCCESS) return Status::InitFailed(fmt_err("vkCreateImageView(ui)", r));

  VkFramebufferCreateInfo fci{};
  fci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
  fci.renderPass = render_pass_;
  fci.attachmentCount = 1;
  fci.pAttachments = &view;
  fci.width = ext.width;
  fci.height = ext.height;
  fci.layers = 1;
  VkFramebuffer fb = VK_NULL_HANDLE;
  r = vkCreateFramebuffer(ctx_.device, &fci, nullptr, &fb);
  if (r != VK_SUCCESS) {
    vkDestroyImageView(ctx_.device, view, nullptr);
    return Status::InitFailed(fmt_err("vkCreateFramebuffer(ui)", r));
  }

  // 3) 场景 pass 之后开 UI pass(LoadOp::Load),不清屏
  VkCommandBufferAllocateInfo cai{};
  cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  cai.commandPool = ctx_.cmd_pool;
  cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cai.commandBufferCount = 1;
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  vkAllocateCommandBuffers(ctx_.device, &cai, &cmd);
  VkCommandBufferBeginInfo bi{};
  bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  vkBeginCommandBuffer(cmd, &bi);

  // 收尾:转到 COLOR_ATTACHMENT_OPTIMAL(读回前必须转 TRANSFER_SRC)
  VkImageMemoryBarrier to_att{};
  to_att.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  to_att.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  to_att.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  to_att.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  to_att.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  to_att.image = target_->color_image();
  to_att.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  to_att.srcAccessMask = 0;
  to_att.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                       VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1,
                       &to_att);

  // 场景 pass 的替身:清成背景色(spec §5.2 Pass 1 的桩;真场景由 L2 提供)
  // LOAD 的 render pass 无法清屏,因此单独来一次 clear(离屏路径才有这一步)
  VkClearColorValue clear_color{};
  clear_color.float32[0] = 16.f / 255.f;
  clear_color.float32[1] = 18.f / 255.f;
  clear_color.float32[2] = 22.f / 255.f;
  clear_color.float32[3] = 1.f;
  VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  vkCmdClearColorImage(cmd, target_->color_image(), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                       &clear_color, 1, &range);

  VkRenderPassBeginInfo rbi{};
  rbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
  rbi.renderPass = render_pass_;
  rbi.framebuffer = fb;
  rbi.renderArea = {{0, 0}, {ext.width, ext.height}};
  rbi.clearValueCount = 0;
  vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
  vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 0, 1, &desc_set_,
                          0, nullptr);
  const float push[2] = {static_cast<float>(ext.width), static_cast<float>(ext.height)};
  vkCmdPushConstants(cmd, pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(push), push);
  // 一次 drawInstanced 画完全部 UI 图元(单 drawcall,合批完成)
  vkCmdDraw(cmd, 4, instance_count(), 0, 0);
  vkCmdEndRenderPass(cmd);
  vkEndCommandBuffer(cmd);
  draw_calls_ = 1;

  // 4) 单次 submit(离屏路径;UI 层 Presenter 不再为它提交)
  VkSubmitInfo si{};
  si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  si.commandBufferCount = 1;
  si.pCommandBuffers = &cmd;
  r = vkQueueSubmit(ctx_.queue, 1, &si, VK_NULL_HANDLE);
  if (r == VK_SUCCESS) r = vkQueueWaitIdle(ctx_.queue);
  vkFreeCommandBuffers(ctx_.device, ctx_.cmd_pool, 1, &cmd);
  vkDestroyFramebuffer(ctx_.device, fb, nullptr);
  vkDestroyImageView(ctx_.device, view, nullptr);
  if (r != VK_SUCCESS) return Status::InitFailed(fmt_err("UI pass 绘制失败", r));
  return Status::Ok_();
}

Status UiOverlayPass::readback(std::uint8_t* out, std::size_t bytes) {
  if (!target_) return Status::Invalid("UiOverlayPass: target 为空");
  return target_->readback_rgba8(out, bytes);
}

void UiOverlayPass::destroy_pipeline() {
  if (pipeline_) vkDestroyPipeline(ctx_.device, pipeline_, nullptr);
  if (pipeline_layout_) vkDestroyPipelineLayout(ctx_.device, pipeline_layout_, nullptr);
  if (render_pass_) vkDestroyRenderPass(ctx_.device, render_pass_, nullptr);
  if (desc_pool_) vkDestroyDescriptorPool(ctx_.device, desc_pool_, nullptr);
  if (set_layout_) vkDestroyDescriptorSetLayout(ctx_.device, set_layout_, nullptr);
  pipeline_ = VK_NULL_HANDLE;
  pipeline_layout_ = VK_NULL_HANDLE;
  render_pass_ = VK_NULL_HANDLE;
  desc_pool_ = VK_NULL_HANDLE;
  set_layout_ = VK_NULL_HANDLE;
  desc_set_ = VK_NULL_HANDLE;
  ready_ = false;
}

// ---------------------------------------------------------------- VulkanFrameRenderer

VulkanFrameRenderer::VulkanFrameRenderer(const VulkanContext& ctx, OffscreenTarget* offscreen)
    : ctx_(ctx), offscreen_(offscreen) {
  if (offscreen_) pass_ = std::make_unique<UiOverlayPass>(ctx_, offscreen_);
}

VulkanFrameRenderer::~VulkanFrameRenderer() = default;

bool VulkanFrameRenderer::render_into_device_image(std::uint64_t, ui::FrameImage* out_image,
                                                   std::uint64_t* out_present_semaphore) {
  // 主路径:SDK L1 ICommandList + L2 IRenderGraph 还没落地,无法把 UI 合成进
  // 调用方给的 command buffer,也无法转 PRESENT 布局
  // 明确失败,不假装成功;等 L1/L2 落地后替换(签名不变,UI 层零改动)
  (void)out_image;
  (void)out_present_semaphore;
  return false;
}

bool VulkanFrameRenderer::render_to_memory(ui::u32 width, ui::u32 height,
                                           std::uint8_t* out_rgba8, ui::u32 row_bytes) {
  if (!offscreen_ || !pass_ || !out_rgba8) return false;
  if (offscreen_->extent().width != width || offscreen_->extent().height != height) return false;
  Status s = pass_->render_offscreen();
  if (!s.ok()) {
    std::fprintf(stderr, "[ui] render_offscreen 失败: %s\n", s.message.c_str());
    return false;
  }
  // 读回(离屏路径才允许)。目标格式是 RGBA8,行序自左上,与契约一致。
  std::vector<std::uint8_t> tmp(static_cast<std::size_t>(width) * height * 4u);
  s = pass_->readback(tmp.data(), tmp.size());
  if (!s.ok()) {
    std::fprintf(stderr, "[ui] readback 失败: %s\n", s.message.c_str());
    return false;
  }
  if (row_bytes == width * 4u) {
    std::memcpy(out_rgba8, tmp.data(), tmp.size());
  } else {
    for (ui::u32 y = 0; y < height; ++y) {
      std::memcpy(out_rgba8 + static_cast<std::size_t>(y) * row_bytes,
                  tmp.data() + static_cast<std::size_t>(y) * width * 4u, width * 4u);
    }
  }
  return true;
}

}  // namespace magicblocks::backend
