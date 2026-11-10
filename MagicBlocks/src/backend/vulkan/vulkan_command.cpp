// MagicBlocks —— Vulkan 后端:L1 命令列表实现(R3 / M3 / G5)
#include "backend/vulkan/vulkan_command.h"

#include <cstring>
#include <string>

#include "backend/vulkan/vulkan_pipeline.h"
#include "backend/vulkan/vulkan_resource_common.h"
#include "backend/vulkan/vulkan_texture.h"

namespace magicblocks::backend {

using namespace magicblocks::device;

namespace {

// stage_mask 只允许 Vertex/Fragment 的位(R4:push constant 只服务这两个 stage)。
// 位取值与 VkShaderStageFlagBits 对齐,免得再翻译一次。
constexpr u32 kStageVertexBit = 0x00000001u;
constexpr u32 kStageFragmentBit = 0x00000010u;
constexpr u32 kAllowedPushStages = kStageVertexBit | kStageFragmentBit;

// pass 的附件目标:设备态要拿底层 VkImage 才能建 framebuffer,而 IRenderTarget 的
// 公共面刻意不暴露 VkImage。这里认后端的实现类型;认不出来就退化成"纯录制"
// (L1 的 Command.SubmitExecutes 用的是 OffscreenTarget,正属可识别的一支)。
VkImage target_color_image(IRenderTarget* target) {
  auto* off = dynamic_cast<OffscreenTarget*>(target);
  return off == nullptr ? VK_NULL_HANDLE : off->color_image();
}

Format target_color_format(IRenderTarget* target) {
  auto* off = dynamic_cast<OffscreenTarget*>(target);
  return off == nullptr ? Format::Unknown : off->color_format();
}

VkIndexType to_vk_index_type(IndexFormat f) {
  return f == IndexFormat::UInt16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
}

void hash_mix(u64* h, u64 v) {
  *h ^= v + 0x9e3779b97f4a7c15ull + (*h << 6) + (*h >> 2);
}

}  // namespace

const char* command_op_name(CommandOp op) {
  switch (op) {
    case CommandOp::BindPipeline:
      return "bind_pipeline";
    case CommandOp::BindVertexBuffer:
      return "bind_vertex_buffer";
    case CommandOp::BindIndexBuffer:
      return "bind_index_buffer";
    case CommandOp::Draw:
      return "draw";
    case CommandOp::DrawIndexed:
      return "draw_indexed";
    case CommandOp::DrawInstanced:
      return "draw_instanced";
    case CommandOp::PushConstants:
      return "push_constants";
    case CommandOp::SetViewport:
      return "set_viewport";
    case CommandOp::SetScissor:
      return "set_scissor";
    case CommandOp::BeginPass:
      return "begin_pass";
    case CommandOp::EndPass:
      return "end_pass";
    case CommandOp::BindSampledTexture:
      return "bind_sampled_texture";
  }
  return "?";
}

// ---------------------------------------------------------------- 构造 / 析构 / init

VulkanCommandList::VulkanCommandList() : has_ctx_(false) {}

VulkanCommandList::VulkanCommandList(const VulkanContext& ctx) : ctx_(ctx), has_ctx_(true) {}

VulkanCommandList::~VulkanCommandList() {
  destroy_vk_command_buffer();
}

Status VulkanCommandList::init() {
  // 与其它 L1 资源一致的两段式:构造不失败,init 报 Status。
  if (has_ctx_) return ensure_vk_command_buffer();
  recording_ = true;  // 软件态:没有 VkCommandBuffer,录制标记单独维护
  return Status::Ok_();
}

Status VulkanCommandList::ensure_vk_command_buffer() {
  if (!has_ctx_ || ctx_.device == VK_NULL_HANDLE) return Status::Ok_();
  if (vk_cmd_ != VK_NULL_HANDLE) return Status::Ok_();
  VkCommandBufferAllocateInfo cai{};
  cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  cai.commandPool = ctx_.cmd_pool;
  cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cai.commandBufferCount = 1;
  const VkResult r = vkAllocateCommandBuffers(ctx_.device, &cai, &vk_cmd_);
  if (r != VK_SUCCESS) return Status::InitFailed(fmt_err("vkAllocateCommandBuffers", r));
  return Status::Ok_();
}

void VulkanCommandList::destroy_vk_command_buffer() {
  if (ctx_.device == VK_NULL_HANDLE) {
    vk_cmd_ = VK_NULL_HANDLE;
    vk_recording_ = false;
    return;
  }
  if (vk_framebuffer_ != VK_NULL_HANDLE) {
    vkDestroyFramebuffer(ctx_.device, vk_framebuffer_, nullptr);
    vk_framebuffer_ = VK_NULL_HANDLE;
  }
  if (vk_color_view_ != VK_NULL_HANDLE) {
    vkDestroyImageView(ctx_.device, vk_color_view_, nullptr);
    vk_color_view_ = VK_NULL_HANDLE;
  }
  if (vk_depth_view_ != VK_NULL_HANDLE) {
    vkDestroyImageView(ctx_.device, vk_depth_view_, nullptr);
    vk_depth_view_ = VK_NULL_HANDLE;
  }
  if (vk_render_pass_ != VK_NULL_HANDLE) {
    vkDestroyRenderPass(ctx_.device, vk_render_pass_, nullptr);
    vk_render_pass_ = VK_NULL_HANDLE;
  }
  if (vk_cmd_ != VK_NULL_HANDLE) {
    if (vk_recording_) {
      // 半截录制的命令缓冲不能直接释放:先结束再释放(内容丢弃)。
      (void)vkEndCommandBuffer(vk_cmd_);
      vk_recording_ = false;
    }
    vkFreeCommandBuffers(ctx_.device, ctx_.cmd_pool, 1, &vk_cmd_);
    vk_cmd_ = VK_NULL_HANDLE;
  }
}

// ---------------------------------------------------------------- 录制工具

void VulkanCommandList::append(const RecordedCommand& cmd) {
  recorded_.push_back(cmd);
  // 旁路表与 recorded_ 严格等长:绑定类命令在各自 append 之后回填指针。
  command_pipeline_.push_back(nullptr);
  ++command_count_;  // 唯一的计数入口:每成功录制一次命令 +1(裁定 1)
}

IBuffer* VulkanCommandList::buffer_at(const ResourceId& id) const {
  if (!id.valid()) return nullptr;
  // 录制期缓冲数量是"场景规模"级(几十~几百),线性查足够;不做哈希表
  // (热路径上这是命令录制期一次调用,不是每顶点/每片元)。
  for (const auto& kv : buffers_) {
    if (kv.first.page_id == id.page_id && kv.first.offset == id.offset) return kv.second;
  }
  return nullptr;
}

// 记录一条"绑定了缓冲"的命令:旁路表登记句柄 → 指针。调用方在 append 之后调用。
void VulkanCommandList::remember_buffer(const ResourceId& id, IBuffer* buffer) {
  if (!id.valid() || buffer == nullptr) return;
  for (auto& kv : buffers_) {
    if (kv.first.page_id == id.page_id && kv.first.offset == id.offset) {
      kv.second = buffer;
      return;
    }
  }
  buffers_.emplace_back(id, buffer);
}

void VulkanCommandList::set_resource(RecordedCommand* cmd, const ResourceId& id) {
  cmd->resource = id;
  // M3:命令流记录**偏移**而不是指针。pack() 是"记录偏移"的落地前提,
  // 失败说明句柄不可序列化(分配器对齐契约被破坏),这里如实标记,不静默清零。
  cmd->packed_ok = id.pack(&cmd->packed_resource);
  if (!cmd->packed_ok) cmd->packed_resource = 0;
}

Status VulkanCommandList::check_draw_state(const char* what) const {
  if (!in_pass_) return Status::Invalid(std::string("ICommandList::") + what + ": 不在 pass 内");
  if (bound_.pipeline == nullptr) {
    return Status::Invalid(std::string("ICommandList::") + what + ": 未绑定管线");
  }
  return Status::Ok_();
}

// ---------------------------------------------------------------- R3 命令 1

Status VulkanCommandList::bind_pipeline(IPipeline* pipeline, u64 pipeline_hash) {
  if (pipeline == nullptr) return Status::Invalid("ICommandList::bind_pipeline: pipeline 为空");
  const u64 h = pipeline_hash != 0 ? pipeline_hash : pipeline->hash();
  if (h == 0) return Status::Invalid("ICommandList::bind_pipeline: pipeline_hash 为 0");

  if (vk_recording_) {
    // 认不出具体实现类型时无法拿到 VkPipeline:明确报 Unsupported,而不是悄悄不绑。
    auto* vk_pipeline = dynamic_cast<VulkanPipeline*>(pipeline);
    if (vk_pipeline == nullptr) {
      return Status::Unsupported(
          "ICommandList::bind_pipeline: 设备态只支持 VulkanPipeline 实现");
    }
    if (!vk_pipeline->has_vk_pipeline()) {
      return Status::Unsupported(
          "ICommandList::bind_pipeline: 该管线不是设备态创建的(无 VkPipeline)");
    }
    vkCmdBindPipeline(vk_cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, vk_pipeline->vk_pipeline());
  }

  bound_.pipeline = pipeline;
  bound_.hash = h;
  bound_.push_constant_bytes = pipeline->push_constant_bytes();

  RecordedCommand cmd;
  cmd.op = CommandOp::BindPipeline;
  cmd.pipeline_hash = h;
  append(cmd);
  command_pipeline_.back() = pipeline;  // 旁路表:设备帧提交要取 VkPipeline*
  return Status::Ok_();
}

// ---------------------------------------------------------------- R3 命令 2 / 3

Status VulkanCommandList::bind_vertex_buffer(const ResourceId& buffer, u32 binding, u64 offset) {
  if (!buffer.valid()) return Status::Invalid("ICommandList::bind_vertex_buffer: 句柄非法");
  if (offset >= buffer.size) {
    return Status::Invalid("ICommandList::bind_vertex_buffer: offset " + std::to_string(offset) +
                           " 越出缓冲大小 " + std::to_string(buffer.size));
  }
  // 设备态:VkBuffer 实体由设备层按 ResourceId 管理(见 vulkan_buffer.h 的说明);
  // 本轮录制层只记句柄,真实绑定在设备帧提交路径(vulkan_device_frame.cpp)上解析。
  // 因此这里不做 vkCmdBindVertexBuffers。

  RecordedCommand cmd;
  cmd.op = CommandOp::BindVertexBuffer;
  set_resource(&cmd, buffer);
  cmd.a = binding;
  // b 保持旧口径(低 32 位),off64 是权威的 64 位值(见 RecordedCommand::off64)。
  cmd.b = static_cast<u32>(offset);
  cmd.off64 = offset;
  append(cmd);
  return Status::Ok_();
}

Status VulkanCommandList::bind_index_buffer(const ResourceId& buffer, IndexFormat format,
                                            u64 offset) {
  if (!buffer.valid()) return Status::Invalid("ICommandList::bind_index_buffer: 句柄非法");
  if (offset >= buffer.size) {
    return Status::Invalid("ICommandList::bind_index_buffer: offset " + std::to_string(offset) +
                           " 越出缓冲大小 " + std::to_string(buffer.size));
  }
  RecordedCommand cmd;
  cmd.op = CommandOp::BindIndexBuffer;
  set_resource(&cmd, buffer);
  cmd.a = static_cast<u32>(format);
  cmd.b = static_cast<u32>(offset);
  cmd.off64 = offset;
  append(cmd);
  return Status::Ok_();
}

Status VulkanCommandList::remember_buffer_binding(const ResourceId& id, IBuffer* buffer) {
  remember_buffer(id, buffer);
  return Status::Ok_();
}

ITexture* VulkanCommandList::texture_at(const ResourceId& id) const {
  if (!id.valid()) return nullptr;
  for (const auto& kv : textures_) {
    if (kv.first.page_id == id.page_id && kv.first.offset == id.offset) return kv.second;
  }
  return nullptr;
}

ISampler* VulkanCommandList::sampler_at(const ResourceId& id) const {
  if (!id.valid()) return nullptr;
  for (const auto& kv : samplers_) {
    if (kv.first.page_id == id.page_id && kv.first.offset == id.offset) return kv.second;
  }
  return nullptr;
}

Status VulkanCommandList::remember_texture_sampler(const ResourceId& texture,
                                                   ITexture* tex, const ResourceId& sampler,
                                                   ISampler* smp) {
  if (texture.valid() && tex != nullptr) {
    bool found = false;
    for (auto& kv : textures_) {
      if (kv.first.page_id == texture.page_id && kv.first.offset == texture.offset) {
        kv.second = tex;
        found = true;
        break;
      }
    }
    if (!found) textures_.emplace_back(texture, tex);
  }
  if (sampler.valid() && smp != nullptr) {
    bool found = false;
    for (auto& kv : samplers_) {
      if (kv.first.page_id == sampler.page_id && kv.first.offset == sampler.offset) {
        kv.second = smp;
        found = true;
        break;
      }
    }
    if (!found) samplers_.emplace_back(sampler, smp);
  }
  return Status::Ok_();
}

// ---------------------------------------------------------------- R3 命令 4 / 5 / 6

Status VulkanCommandList::draw(const DrawArgs& args) {
  Status s = check_draw_state("draw");
  if (!s.ok()) return s;
  if (args.vertex_count == 0) return Status::Invalid("ICommandList::draw: vertex_count 为 0");
  if (args.instance_count == 0) {
    return Status::Invalid("ICommandList::draw: instance_count 为 0");
  }
  RecordedCommand cmd;
  cmd.op = CommandOp::Draw;
  cmd.a = args.vertex_count;
  cmd.b = args.instance_count;
  cmd.c = args.first_vertex;
  cmd.i = args.vertex_offset;
  append(cmd);
  return Status::Ok_();
}

Status VulkanCommandList::draw_indexed(const DrawArgs& args) {
  Status s = check_draw_state("draw_indexed");
  if (!s.ok()) return s;
  if (args.index_count == 0) return Status::Invalid("ICommandList::draw_indexed: index_count 为 0");
  if (args.instance_count == 0) {
    return Status::Invalid("ICommandList::draw_indexed: instance_count 为 0");
  }
  RecordedCommand cmd;
  cmd.op = CommandOp::DrawIndexed;
  cmd.a = args.index_count;
  cmd.b = args.instance_count;
  cmd.c = args.first_index;
  cmd.i = args.vertex_offset;
  append(cmd);
  return Status::Ok_();
}

Status VulkanCommandList::draw_instanced(const DrawArgs& args) {
  Status s = check_draw_state("draw_instanced");
  if (!s.ok()) return s;
  if (args.vertex_count == 0) {
    return Status::Invalid("ICommandList::draw_instanced: vertex_count 为 0");
  }
  if (args.instance_count <= 1) {
    // 实例数为 1 的"实例化绘制"是调用方语义错误:走 draw() 就够,
    // 保留这条分支是为了让 G2 的实例化路径在命令流里可区分。
    return Status::Invalid("ICommandList::draw_instanced: instance_count 必须 > 1(等于 1 请用 "
                           "draw)");
  }
  RecordedCommand cmd;
  cmd.op = CommandOp::DrawInstanced;
  cmd.a = args.vertex_count;
  cmd.b = args.instance_count;
  cmd.c = args.first_vertex;
  cmd.i = args.vertex_offset;
  cmd.f[0] = static_cast<float>(args.first_instance);
  append(cmd);
  return Status::Ok_();
}

// ---------------------------------------------------------------- R3 命令 7

Status VulkanCommandList::push_constants(u32 stage_mask, u32 offset, u32 size_bytes,
                                         const void* data) {
  if (bound_.pipeline == nullptr) {
    return Status::Invalid("ICommandList::push_constants: 未绑定管线");
  }
  if ((stage_mask & kAllowedPushStages) == 0 || (stage_mask & ~kAllowedPushStages) != 0) {
    return Status::Invalid("ICommandList::push_constants: stage_mask 只允许 Vertex|Fragment,得到 " +
                           std::to_string(stage_mask));
  }
  if (size_bytes == 0) return Status::Invalid("ICommandList::push_constants: size_bytes 为 0");
  if (size_bytes % 4 != 0) {
    return Status::Invalid("ICommandList::push_constants: size_bytes 必须是 4 的倍数");
  }
  if (data == nullptr) return Status::Invalid("ICommandList::push_constants: data 为空");
  if (offset + size_bytes > bound_.push_constant_bytes) {
    return Status::Invalid("ICommandList::push_constants: offset+size=" +
                           std::to_string(offset + size_bytes) + " 超过管线 push constant 大小 " +
                           std::to_string(bound_.push_constant_bytes));
  }
  if (vk_recording_) {
    auto* vk_pipeline = dynamic_cast<VulkanPipeline*>(bound_.pipeline);
    if (vk_pipeline == nullptr) {
      return Status::Unsupported("ICommandList::push_constants: 设备态只支持 VulkanPipeline");
    }
    vkCmdPushConstants(vk_cmd_, vk_pipeline->vk_pipeline_layout(),
                       static_cast<VkShaderStageFlags>(stage_mask), offset, size_bytes, data);
  }

  RecordedCommand cmd;
  cmd.op = CommandOp::PushConstants;
  cmd.a = stage_mask;
  cmd.b = offset;
  cmd.c = size_bytes;
  cmd.payload.resize(size_bytes);
  std::memcpy(cmd.payload.data(), data, size_bytes);
  append(cmd);
  return Status::Ok_();
}

// ---------------------------------------------------------------- R3 命令 8 / 9

Status VulkanCommandList::set_viewport(const Viewport& viewport) {
  if (viewport.width <= 0.0f || viewport.height <= 0.0f) {
    return Status::Invalid("ICommandList::set_viewport: width/height 必须 > 0");
  }
  if (viewport.min_depth < 0.0f || viewport.min_depth > 1.0f || viewport.max_depth < 0.0f ||
      viewport.max_depth > 1.0f) {
    return Status::Invalid("ICommandList::set_viewport: depth 区间必须落在 [0,1]");
  }
  if (viewport.min_depth > viewport.max_depth) {
    return Status::Invalid("ICommandList::set_viewport: min_depth 不能大于 max_depth");
  }
  if (vk_recording_) {
    // Vulkan 的视口 y 向下为正,与公共 Viewport 的"像素坐标原点左上"一致;
    // L0 的 ui_overlay.vert 在着色器里做过一次 y 翻转,命令列表不再翻第二次
    // (vulkan_target.cpp 注释里的"双翻行"教训)。
    VkViewport vp{viewport.x, viewport.y, viewport.width, viewport.height, viewport.min_depth,
                  viewport.max_depth};
    vkCmdSetViewport(vk_cmd_, 0, 1, &vp);
  }
  RecordedCommand cmd;
  cmd.op = CommandOp::SetViewport;
  cmd.f[0] = viewport.x;
  cmd.f[1] = viewport.y;
  cmd.f[2] = viewport.width;
  cmd.f[3] = viewport.height;
  cmd.f[4] = viewport.min_depth;
  cmd.f[5] = viewport.max_depth;
  append(cmd);
  return Status::Ok_();
}

Status VulkanCommandList::set_scissor(const ScissorRect& rect) {
  if (rect.width == 0 || rect.height == 0) {
    return Status::Invalid("ICommandList::set_scissor: width/height 必须 > 0");
  }
  if (vk_recording_) {
    VkRect2D r{{static_cast<int32_t>(rect.x), static_cast<int32_t>(rect.y)},
               {rect.width, rect.height}};
    vkCmdSetScissor(vk_cmd_, 0, 1, &r);
  }
  RecordedCommand cmd;
  cmd.op = CommandOp::SetScissor;
  cmd.a = rect.x;
  cmd.b = rect.y;
  cmd.width = rect.width;
  cmd.height = rect.height;
  append(cmd);
  return Status::Ok_();
}

// ---------------------------------------------------------------- R3 命令 10 / 11

Status VulkanCommandList::begin_pass(const RenderPassDesc& pass) {
  if (pass.target == nullptr) {
    return Status::Invalid("ICommandList::begin_pass: target 为空");
  }
  if (in_pass_) return Status::Invalid("ICommandList::begin_pass: 上一个 pass 还没 end_pass");

  const Extent2D ext = pass.target->extent();
  const u32 ax = pass.area_x;
  const u32 ay = pass.area_y;
  const u32 aw = pass.area_width == 0 ? ext.width : pass.area_width;
  const u32 ah = pass.area_height == 0 ? ext.height : pass.area_height;
  if (aw == 0 || ah == 0) {
    return Status::Invalid("ICommandList::begin_pass: area 宽高必须 > 0");
  }
  // 离屏是 golden 的唯一来源:"画到边界外"不允许被静默裁剪。
  if (ax + aw > ext.width || ay + ah > ext.height) {
    return Status::Invalid("ICommandList::begin_pass: render area [" + std::to_string(ax) + "," +
                           std::to_string(ay) + " + " + std::to_string(aw) + "x" +
                           std::to_string(ah) + "] 越出目标 " + std::to_string(ext.width) + "x" +
                           std::to_string(ext.height));
  }

  if (!recording_) {
    Status s = init();
    if (!s.ok()) return s;
  }

  // 设备态且目标是后端离屏实现:开真实 render pass(附件格式取自目标本身)。
  if (vk_recording_ || (has_ctx_ && ctx_.device != VK_NULL_HANDLE)) {
    Status s = ensure_vk_command_buffer();
    if (!s.ok()) return s;
    if (!vk_recording_) {
      VkCommandBufferBeginInfo bi{};
      bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
      bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
      const VkResult r = vkBeginCommandBuffer(vk_cmd_, &bi);
      if (r != VK_SUCCESS) return Status::InitFailed(fmt_err("vkBeginCommandBuffer", r));
      vk_recording_ = true;
      // recording_ 是 command.h 里 recording() 的唯一来源。之前只有软件态
      // 在 init() 里置它,设备态会一直报 "未在录制",与 in_pass_ 自相矛盾。
      recording_ = true;
    }
    VkImage color = target_color_image(pass.target);
    if (color != VK_NULL_HANDLE) {
      const VkFormat fmt = vk_format_of(target_color_format(pass.target));
      VkAttachmentDescription att[1] = {};
      att[0].format = fmt;
      att[0].samples = VK_SAMPLE_COUNT_1_BIT;
      att[0].loadOp = pass.clear_color ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
      att[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
      att[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
      att[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
      // IRenderTarget::readback_rgba8 的契约是"目标末态 GENERAL"(见 vulkan_target.cpp),
      // 因此这里 UNDEFINED → GENERAL,读回路径的 GENERAL → TRANSFER_SRC 屏障才能成立。
      att[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
      att[0].finalLayout = VK_IMAGE_LAYOUT_GENERAL;
      VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
      VkSubpassDescription sub{};
      sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
      sub.colorAttachmentCount = 1;
      sub.pColorAttachments = &ref;
      VkRenderPassCreateInfo rpci{};
      rpci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
      rpci.attachmentCount = 1;
      rpci.pAttachments = att;
      rpci.subpassCount = 1;
      rpci.pSubpasses = &sub;
      VkResult r = vkCreateRenderPass(ctx_.device, &rpci, nullptr, &vk_render_pass_);
      if (r != VK_SUCCESS) return Status::InitFailed(fmt_err("vkCreateRenderPass(cmd)", r));

      VkImageViewCreateInfo vci{};
      vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
      vci.image = color;
      vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
      vci.format = fmt;
      vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
      r = vkCreateImageView(ctx_.device, &vci, nullptr, &vk_color_view_);
      if (r != VK_SUCCESS) return Status::InitFailed(fmt_err("vkCreateImageView(cmd)", r));

      VkFramebufferCreateInfo fci{};
      fci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
      fci.renderPass = vk_render_pass_;
      fci.attachmentCount = 1;
      fci.pAttachments = &vk_color_view_;
      fci.width = ext.width;
      fci.height = ext.height;
      fci.layers = 1;
      r = vkCreateFramebuffer(ctx_.device, &fci, nullptr, &vk_framebuffer_);
      if (r != VK_SUCCESS) return Status::InitFailed(fmt_err("vkCreateFramebuffer(cmd)", r));

      VkClearValue clear{};
      clear.color = {{pass.clear_color_rgba[0], pass.clear_color_rgba[1], pass.clear_color_rgba[2],
                      pass.clear_color_rgba[3]}};
      VkRenderPassBeginInfo rbi{};
      rbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
      rbi.renderPass = vk_render_pass_;
      rbi.framebuffer = vk_framebuffer_;
      rbi.renderArea = {{static_cast<int32_t>(ax), static_cast<int32_t>(ay)}, {aw, ah}};
      rbi.clearValueCount = 1;
      rbi.pClearValues = &clear;
      vkCmdBeginRenderPass(vk_cmd_, &rbi, VK_SUBPASS_CONTENTS_INLINE);
      has_vk_pass_target_ = true;
      // 未显式设置视口/裁剪时,给一个覆盖整个 render area 的默认值
      // (set_viewport / set_scissor 会覆盖它)。
      VkViewport vp{static_cast<float>(ax), static_cast<float>(ay), static_cast<float>(aw),
                    static_cast<float>(ah), 0.0f, 1.0f};
      VkRect2D sc{{static_cast<int32_t>(ax), static_cast<int32_t>(ay)}, {aw, ah}};
      vkCmdSetViewport(vk_cmd_, 0, 1, &vp);
      vkCmdSetScissor(vk_cmd_, 0, 1, &sc);
    } else {
      has_vk_pass_target_ = false;
    }
  }

  last_pass_ = pass;
  in_pass_ = true;
  RecordedCommand cmd;
  cmd.op = CommandOp::BeginPass;
  cmd.pass_area[0] = ax;
  cmd.pass_area[1] = ay;
  cmd.pass_area[2] = aw;
  cmd.pass_area[3] = ah;
  cmd.f[0] = pass.clear_color_rgba[0];
  cmd.f[1] = pass.clear_color_rgba[1];
  cmd.f[2] = pass.clear_color_rgba[2];
  cmd.f[3] = pass.clear_color_rgba[3];
  cmd.f[4] = pass.clear_depth_value;
  cmd.f[5] = pass.clear_color ? 1.0f : 0.0f;
  cmd.f[6] = pass.clear_depth ? 1.0f : 0.0f;
  append(cmd);
  return Status::Ok_();
}

Status VulkanCommandList::end_pass() {
  if (!in_pass_) return Status::Invalid("ICommandList::end_pass: 当前不在 pass 内");
  if (vk_recording_ && has_vk_pass_target_) {
    vkCmdEndRenderPass(vk_cmd_);
  }
  // **不能在这里销毁 framebuffer / image view / render pass。**
  //
  // 这三个对象被刚刚录进 vk_cmd_ 的 vkCmdBeginRenderPass/vkCmdEndRenderPass
  // 引用;Vulkan 要求它们活到命令缓冲**执行完成**为止。早先在这里直接销毁,
  // 会让 vkQueueSubmit / vkWaitForFences 读到已释放的句柄 —— 表现为随机的
  // 访问冲突(约 85% 崩溃率,与录制内容无关)。
  //
  // 正确做法:交给 submit_and_wait() 在 fence 等到之后统一销毁
  // (见 pass_targets_pending_cleanup_ 分支)。
  if (vk_framebuffer_ != VK_NULL_HANDLE || vk_render_pass_ != VK_NULL_HANDLE) {
    pass_targets_pending_cleanup_ = true;
  }
  has_vk_pass_target_ = false;
  in_pass_ = false;
  RecordedCommand cmd;
  cmd.op = CommandOp::EndPass;
  append(cmd);
  return Status::Ok_();
}

// ---------------------------------------------------------------- P0 命令 12(X1)

Status VulkanCommandList::bind_sampled_texture(const ResourceId& texture,
                                               const ResourceId& sampler, u32 slot) {
  // 录制期校验:未开 pass / 未绑管线 → InvalidArgument(与 draw 同口径)。
  if (!in_pass_) {
    return Status::Invalid("ICommandList::bind_sampled_texture: 不在 pass 内");
  }
  if (bound_.pipeline == nullptr) {
    return Status::Invalid("ICommandList::bind_sampled_texture: 未绑定管线");
  }
  if (!texture.valid()) {
    return Status::Invalid("ICommandList::bind_sampled_texture: texture 句柄无效");
  }
  if (!sampler.valid()) {
    return Status::Invalid("ICommandList::bind_sampled_texture: sampler 句柄无效");
  }
  // slot 越界 → InvalidArgument,绝不静默丢弃(否则画面"没纹理"且不报错)。
  const u32 declared = bound_.pipeline->sampled_texture_slots();
  if (slot >= declared) {
    return Status::Invalid("ICommandList::bind_sampled_texture: slot " + std::to_string(slot) +
                           " 越界(当前管线只声明了 " + std::to_string(declared) +
                           " 个采样纹理槽;PipelineDesc::sampled_texture_slots)");
  }

  if (vk_recording_ && has_ctx_) {
    // 设备态:此刻就把描述符写进去并绑定。描述符写入按 slot 覆盖,
    // 同一帧内后写生效(与 Vulkan 的 descriptor 语义一致)。
    //
    // 注意:公共接口只给 ResourceId(M3 值语义),所以设备态必须先由
    // remember_texture_sampler(tex_id, ITexture*, smp_id, ISampler*) 登记过,
    // 否则拿不到 VkImageView/VkSampler —— 此时明确报 Unsupported,
    // 绝不当成"绑成功了但没纹理"。
    auto* vk_pipeline = dynamic_cast<VulkanPipeline*>(bound_.pipeline);
    if (vk_pipeline == nullptr) {
      return Status::Unsupported(
          "ICommandList::bind_sampled_texture: 设备态只支持 VulkanPipeline 实现");
    }
    if (!vk_pipeline->has_descriptor_set()) {
      return Status::Unsupported(
          "ICommandList::bind_sampled_texture: 该管线没有设备态描述符集");
    }
    ITexture* tex = texture_at(texture);
    if (tex == nullptr) {
      return Status::Unsupported(
          "ICommandList::bind_sampled_texture: 未登记该纹理对象(设备态需要真实 ITexture*)");
    }
    ISampler* smp = sampler_at(sampler);
    if (smp == nullptr) {
      return Status::Unsupported(
          "ICommandList::bind_sampled_texture: 未登记该采样器对象(设备态需要真实 ISampler*)");
    }
    auto* vk_tex = dynamic_cast<VulkanTexture*>(tex);
    auto* vk_smp = dynamic_cast<VulkanSampler*>(smp);
    if (vk_tex == nullptr || !vk_tex->has_vk_view()) {
      return Status::Unsupported(
          "ICommandList::bind_sampled_texture: 纹理不是设备态创建的(无 VkImageView)");
    }
    if (vk_smp == nullptr || !vk_smp->has_vk_sampler()) {
      return Status::Unsupported(
          "ICommandList::bind_sampled_texture: 采样器不是设备态创建的(无 VkSampler)");
    }
    const Status ws = vk_pipeline->write_sampled_texture(slot, vk_tex->vk_view(),
                                                         vk_smp->vk_sampler());
    if (!ws.ok()) return ws;
    const VkDescriptorSet ds = vk_pipeline->vk_descriptor_set();
    vkCmdBindDescriptorSets(vk_cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            vk_pipeline->vk_pipeline_layout(), 0, 1, &ds, 0, nullptr);
  }

  RecordedCommand cmd;
  cmd.op = CommandOp::BindSampledTexture;
  set_resource(&cmd, texture);  // resource = 纹理(M3:命令流只记偏移)
  cmd.a = slot;                 // 槽位
  cmd.sampler = sampler;
  // 采样器句柄的 page_id 是 kSamplerPage(见 handles.h),超出 pack() 的 16 位页号
  // 宽度,所以**不要求**打包成功;采样器的稳定身份就是 id(= slot_index),
  // 单独记进 packed_sampler 供 L2 去重/复现用。
  cmd.packed_sampler = sampler.id;
  append(cmd);
  return Status::Ok_();
}

// ---------------------------------------------------------------- 状态 / 复位

void VulkanCommandList::reset() {
  destroy_vk_command_buffer();
  recorded_.clear();
  command_pipeline_.clear();
  buffers_.clear();
  // P0(X1):textures_/samplers_ 同样保留(见 submit_and_wait 的说明)——
  // 它们是弱引用映射,不随命令流清空而失效。
  command_count_ = 0;
  in_pass_ = false;
  bound_ = Bound{};
  last_pass_ = RenderPassDesc{};
  recording_ = !has_ctx_;  // 软件态复位后直接进入可录制状态
}

u32 VulkanCommandList::op_count(CommandOp op) const {
  u32 n = 0;
  for (const RecordedCommand& c : recorded_) {
    if (c.op == op) ++n;
  }
  return n;
}

bool VulkanCommandList::resource_packed(CommandOp op) const {
  switch (op) {
    case CommandOp::BindVertexBuffer:
    case CommandOp::BindIndexBuffer:
      break;
    default:
      return true;  // 不携带资源的命令不需要打包
  }
  for (const RecordedCommand& c : recorded_) {
    if (c.op == op && !c.packed_ok) return false;
  }
  return true;
}

bool VulkanCommandList::all_resource_handles_packed() const {
  return resource_packed(CommandOp::BindVertexBuffer) &&
         resource_packed(CommandOp::BindIndexBuffer);
}

// ---------------------------------------------------------------- 提交(G5 单队列)

Status VulkanCommandList::submit_and_wait() {
  // 未 end_pass 就提交 → InvalidArgument(command.h 明文:不放过半个 pass)。
  if (in_pass_) return Status::Invalid("ICommandList::submit_and_wait: 还有未 end_pass 的 pass");

  if (!has_ctx_ || ctx_.device == VK_NULL_HANDLE) {
    // 软件态:录制内容已经是"执行结果",提交只做状态复位。
    ++submit_count_;
    last_submit_error_.clear();
    const bool keep = recording_;
    destroy_vk_command_buffer();
    pass_targets_pending_cleanup_ = false;
    recorded_.clear();
    command_pipeline_.clear();
    buffers_.clear();
    // P0(X1):textures_/samplers_ 刻意**不清** —— 它们只是"句柄 → 有效对象"的
    // 弱引用映射,不随命令流失效;保留下来,同一批纹理在下一帧再绑时仍能取到
    // VkImageView/VkSampler(否则每帧都要调用方重新 remember,极易漏)。
    command_count_ = 0;
    bound_ = Bound{};
    recording_ = keep;
    return Status::Ok_();
  }

  if (vk_recording_) {
    const VkResult r = vkEndCommandBuffer(vk_cmd_);
    if (r != VK_SUCCESS) {
      vk_recording_ = false;
      return Status::InitFailed(fmt_err("vkEndCommandBuffer", r));
    }
    vk_recording_ = false;
  }

  // 单队列 + fence:确定性完成语义(离屏 CI 路径要它)。
  VkFenceCreateInfo fci{};
  fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  VkFence fence = VK_NULL_HANDLE;
  VkResult r = vkCreateFence(ctx_.device, &fci, nullptr, &fence);
  if (r != VK_SUCCESS) return Status::InitFailed(fmt_err("vkCreateFence(命令提交)", r));

  VkSubmitInfo si{};
  si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  si.commandBufferCount = vk_cmd_ != VK_NULL_HANDLE ? 1u : 0u;
  si.pCommandBuffers = vk_cmd_ != VK_NULL_HANDLE ? &vk_cmd_ : nullptr;
  r = vkQueueSubmit(ctx_.queue, 1, &si, fence);
  if (r == VK_SUCCESS) r = vkWaitForFences(ctx_.device, 1, &fence, VK_TRUE, UINT64_MAX);
  vkDestroyFence(ctx_.device, fence, nullptr);
  if (r != VK_SUCCESS) {
    last_submit_error_ = fmt_err("ICommandList::submit_and_wait", r);
    return Status::InitFailed(last_submit_error_);
  }

  ++submit_count_;
  last_submit_error_.clear();
  // 命令缓冲已执行完毕(fence 已等到),此时才能安全销毁 pass 期间创建的
  // render pass / framebuffer / image view。
  if (pass_targets_pending_cleanup_) {
    if (vk_framebuffer_ != VK_NULL_HANDLE) {
      vkDestroyFramebuffer(ctx_.device, vk_framebuffer_, nullptr);
      vk_framebuffer_ = VK_NULL_HANDLE;
    }
    if (vk_color_view_ != VK_NULL_HANDLE) {
      vkDestroyImageView(ctx_.device, vk_color_view_, nullptr);
      vk_color_view_ = VK_NULL_HANDLE;
    }
    if (vk_depth_view_ != VK_NULL_HANDLE) {
      vkDestroyImageView(ctx_.device, vk_depth_view_, nullptr);
      vk_depth_view_ = VK_NULL_HANDLE;
    }
    if (vk_render_pass_ != VK_NULL_HANDLE) {
      vkDestroyRenderPass(ctx_.device, vk_render_pass_, nullptr);
      vk_render_pass_ = VK_NULL_HANDLE;
    }
    pass_targets_pending_cleanup_ = false;
  }
  // 提交后命令列表回到可复用状态:命令缓冲释放,下一轮 begin_pass 重新走 begin 录制路径。
  destroy_vk_command_buffer();
  recorded_.clear();
  command_pipeline_.clear();
  buffers_.clear();
  command_count_ = 0;
  bound_ = Bound{};
  in_pass_ = false;
  recording_ = false;
  return Status::Ok_();
}

}  // namespace magicblocks::backend
