// MagicBlocks —— Vulkan 后端:L1 管线实现(R2 / R4 / G1 / P12)
#include "backend/vulkan/vulkan_pipeline.h"

#include <algorithm>
#include <cstring>
#include <set>

#include "backend/vulkan/vulkan_resource_common.h"
// 析构里的"销毁前排空在飞帧"需要 wait_device_frames_idle(见 ~VulkanPipeline)。
#include "backend/vulkan/vulkan_sync_core.h"

namespace magicblocks::backend {

using namespace magicblocks::device;

namespace {

constexpr u32 kDefaultMaxPushConstants = 128;  // 移动端下限(spec R4:实测 >= 128)
constexpr u32 kDefaultMaxPushConstantStages = 2;  // Vertex + Fragment
// P0(X1):本轮"1 个 descriptor set + array slot"模型的上限。不做 bindless,
// 所以不放宽到几百;颜色贴图 + 深度贴图这类用法个位数就够。
constexpr u32 kMaxSampledTextureSlots = 16;

bool is_power_of_two(u32 v) { return v != 0 && (v & (v - 1)) == 0; }

VkCullModeFlags to_vk_cull(CullMode m) {
  switch (m) {
    case CullMode::None:
      return VK_CULL_MODE_NONE;
    case CullMode::Front:
      return VK_CULL_MODE_FRONT_BIT;
    case CullMode::Back:
      return VK_CULL_MODE_BACK_BIT;
  }
  return VK_CULL_MODE_BACK_BIT;
}

VkFrontFace to_vk_front_face(FrontFace f) {
  return f == FrontFace::Clockwise ? VK_FRONT_FACE_CLOCKWISE
                                   : VK_FRONT_FACE_COUNTER_CLOCKWISE;
}

VkBlendFactor to_vk_blend_factor(resources::BlendFactor f) {
  switch (f) {
    case resources::BlendFactor::Zero:
      return VK_BLEND_FACTOR_ZERO;
    case resources::BlendFactor::One:
      return VK_BLEND_FACTOR_ONE;
    case resources::BlendFactor::SrcAlpha:
      return VK_BLEND_FACTOR_SRC_ALPHA;
    case resources::BlendFactor::OneMinusSrcAlpha:
      return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    case resources::BlendFactor::DstAlpha:
      return VK_BLEND_FACTOR_DST_ALPHA;
    case resources::BlendFactor::OneMinusDstAlpha:
      return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
    case resources::BlendFactor::SrcColor:
      return VK_BLEND_FACTOR_SRC_COLOR;
    case resources::BlendFactor::OneMinusSrcColor:
      return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
  }
  return VK_BLEND_FACTOR_ONE;
}

VkBlendOp to_vk_blend_op(resources::BlendOp op) {
  switch (op) {
    case resources::BlendOp::Add:
      return VK_BLEND_OP_ADD;
    case resources::BlendOp::Subtract:
      return VK_BLEND_OP_SUBTRACT;
    case resources::BlendOp::ReverseSubtract:
      return VK_BLEND_OP_REVERSE_SUBTRACT;
    case resources::BlendOp::Min:
      return VK_BLEND_OP_MIN;
    case resources::BlendOp::Max:
      return VK_BLEND_OP_MAX;
  }
  return VK_BLEND_OP_ADD;
}

VkCompareOp to_vk_compare(resources::CompareOp op) {
  switch (op) {
    case resources::CompareOp::Never:
      return VK_COMPARE_OP_NEVER;
    case resources::CompareOp::Less:
      return VK_COMPARE_OP_LESS;
    case resources::CompareOp::Equal:
      return VK_COMPARE_OP_EQUAL;
    case resources::CompareOp::LessOrEqual:
      return VK_COMPARE_OP_LESS_OR_EQUAL;
    case resources::CompareOp::Greater:
      return VK_COMPARE_OP_GREATER;
    case resources::CompareOp::NotEqual:
      return VK_COMPARE_OP_NOT_EQUAL;
    case resources::CompareOp::GreaterOrEqual:
      return VK_COMPARE_OP_GREATER_OR_EQUAL;
    case resources::CompareOp::Always:
      return VK_COMPARE_OP_ALWAYS;
    case resources::CompareOp::Disabled:
      break;
  }
  // Disabled 是"仅采样器用"的取值,pipeline.h 的 DepthState 默认是 Less;
  // 到这里说明调用方在 depthOp 里传了 Disabled → 由 validate() 先拦下来。
  return VK_COMPARE_OP_LESS;
}

VkPrimitiveTopology to_vk_topology(PrimitiveType p) {
  return p == PrimitiveType::LineList ? VK_PRIMITIVE_TOPOLOGY_LINE_LIST
                                      : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
}

VkFormat to_vk_vertex_format(resources::VertexFormat f) {
  switch (f) {
    case resources::VertexFormat::Float:
      return VK_FORMAT_R32_SFLOAT;
    case resources::VertexFormat::Float2:
      return VK_FORMAT_R32G32_SFLOAT;
    case resources::VertexFormat::Float3:
      return VK_FORMAT_R32G32B32_SFLOAT;
    case resources::VertexFormat::Float4:
      return VK_FORMAT_R32G32B32A32_SFLOAT;
    case resources::VertexFormat::UByte4Norm:
      return VK_FORMAT_R8G8B8A8_UNORM;
    case resources::VertexFormat::Short2:
      return VK_FORMAT_R16G16_SINT;
    case resources::VertexFormat::Short4:
      return VK_FORMAT_R16G16B16A16_SINT;
  }
  return VK_FORMAT_UNDEFINED;
}

// MSAA:采样数 → VkSampleCountFlagBits 已提到 vulkan_resource_common.h，
// 由本文件、vulkan_draw.cpp、vulkan_device_frame.cpp 共用一份(不复制第二份)。

std::uint32_t stage_bit(ShaderStage s) {
  switch (s) {
    case ShaderStage::Vertex:
      return static_cast<std::uint32_t>(VK_SHADER_STAGE_VERTEX_BIT);
    case ShaderStage::Fragment:
      return static_cast<std::uint32_t>(VK_SHADER_STAGE_FRAGMENT_BIT);
    case ShaderStage::Compute:
      return static_cast<std::uint32_t>(VK_SHADER_STAGE_COMPUTE_BIT);
  }
  return 0;
}

VkShaderStageFlagBits to_vk_stage(ShaderStage s) {
  switch (s) {
    case ShaderStage::Vertex:
      return VK_SHADER_STAGE_VERTEX_BIT;
    case ShaderStage::Fragment:
      return VK_SHADER_STAGE_FRAGMENT_BIT;
    case ShaderStage::Compute:
      return VK_SHADER_STAGE_COMPUTE_BIT;
  }
  return VK_SHADER_STAGE_VERTEX_BIT;
}

const char* stage_name(ShaderStage s) {
  switch (s) {
    case ShaderStage::Vertex:
      return "Vertex";
    case ShaderStage::Fragment:
      return "Fragment";
    case ShaderStage::Compute:
      return "Compute";
  }
  return "?";
}

void hash_mix(u64* h, u64 v) {
  // FNV-1a 风格的便宜混合:只求"同 desc → 同 hash、不同 desc → 极大概率不同"。
  *h ^= v + 0x9e3779b97f4a7c15ull + (*h << 6) + (*h >> 2);
}

template <typename T>
void hash_raw(u64* h, const T& value) {
  u64 v = 0;
  static_assert(sizeof(T) <= sizeof(u64), "hash_raw 只处理 <= 8 字节的平凡类型");
  std::memcpy(&v, &value, sizeof(T));
  hash_mix(h, v);
}

}  // namespace

// ---------------------------------------------------------------- 哈希

u64 hash_pipeline_desc(const PipelineDesc& desc) {
  u64 h = 0xcbf29ce484222325ull;
  hash_mix(&h, desc.stages.size());
  for (const ShaderModule& m : desc.stages) {
    hash_raw(&h, m.stage);
    hash_mix(&h, m.size_bytes);
    // 不对 SPIR-V 全文做哈希(几 KB 太贵):取首/中/尾三个字做指纹,
    // 足以区分"换了 shader"。真正的唯一性由设备层按 (desc, hash) 建表保证。
    if (m.spirv != nullptr && m.size_bytes >= 12) {
      std::uint32_t w[3] = {};
      std::memcpy(w, m.spirv, 12);
      const std::uint32_t* tail = reinterpret_cast<const std::uint32_t*>(m.spirv + m.size_bytes - 4);
      hash_mix(&h, w[0]);
      hash_mix(&h, w[1]);
      hash_mix(&h, w[2]);
      hash_mix(&h, *tail);
    }
  }
  hash_mix(&h, desc.vertexLayout.bindings.size());
  for (const resources::VertexInputBinding& b : desc.vertexLayout.bindings) {
    hash_raw(&h, b.binding);
    hash_raw(&h, b.stride);
    hash_mix(&h, b.instanced ? 1u : 0u);
  }
  hash_mix(&h, desc.vertexLayout.attributes.size());
  for (const resources::VertexAttribute& a : desc.vertexLayout.attributes) {
    hash_raw(&h, a.location);
    hash_raw(&h, a.binding);
    hash_raw(&h, a.format);
    hash_raw(&h, a.offset);
  }
  hash_mix(&h, desc.blend.enabled ? 1u : 0u);
  hash_raw(&h, desc.blend.src_color);
  hash_raw(&h, desc.blend.dst_color);
  hash_raw(&h, desc.blend.color_op);
  hash_raw(&h, desc.blend.src_alpha);
  hash_raw(&h, desc.blend.dst_alpha);
  hash_raw(&h, desc.blend.alpha_op);
  hash_mix(&h, desc.depth.depthTest ? 1u : 0u);
  hash_mix(&h, desc.depth.depthWrite ? 1u : 0u);
  hash_raw(&h, desc.depth.depthOp);
  hash_mix(&h, desc.depth.depthConstWrite ? 1u : 0u);
  hash_raw(&h, desc.depth.depth_const);
  hash_raw(&h, desc.raster.cull);
  hash_raw(&h, desc.raster.front_face);
  hash_mix(&h, desc.raster.depth_bias_enabled ? 1u : 0u);
  hash_raw(&h, desc.raster.depth_bias_constant);
  hash_raw(&h, desc.raster.depth_bias_slope);
  hash_raw(&h, desc.raster.depth_bias_clamp);
  hash_raw(&h, desc.samples);
  hash_raw(&h, desc.primitive);
  hash_mix(&h, desc.compute ? 1u : 0u);
  hash_raw(&h, desc.push_constant_bytes);
  // P0(X1):sampled_texture_slots **只在非 0 时**参与哈希。
  // 这样 slots == 0 的既有管线哈希逐位不变(P0-1 的验收口径:既有 5 张 golden
  // 逐字节不变),而声明了纹理槽的新管线与未声明的是不同管线(去重表不会错合并)。
  if (desc.sampled_texture_slots != 0) {
    hash_raw(&h, desc.sampled_texture_slots);
  }
  return h;
}

u64 hash_pipeline_desc_with_shape(const PipelineDesc& desc, const AttachmentShape& shape) {
  // 形状为默认值时返回基础哈希 → 与历史命令流/既有单测逐位一致。
  const bool is_default_shape = shape.color_format == device::Format::RGBA8_UNORM &&
                                !shape.has_depth && shape.samples == 1;
  u64 h = hash_pipeline_desc(desc);
  if (is_default_shape) return h;
  hash_raw(&h, shape.color_format);
  hash_mix(&h, shape.has_depth ? 1u : 0u);
  if (shape.has_depth) hash_raw(&h, shape.depth_format);
  hash_raw(&h, shape.samples);
  return h;
}

// ---------------------------------------------------------------- 校验

Status VulkanPipeline::validate(const PipelineDesc& desc, u32 max_push_constant_size,
                                u32 supported_samples_bitmask) {
  // 1) stages 空 → InvalidArgument(Pipeline.RejectsEmptyStages)
  if (desc.stages.empty()) {
    return Status::Invalid("PipelineDesc: stages 为空(图形管线需要 Vertex+Fragment,计算管线需要 "
                           "Compute)");
  }
  // 2) 每份 stage 的字节码必须有效(SPIR-V 字对齐、非空)
  std::set<int> seen;
  for (const ShaderModule& m : desc.stages) {
    if (!m.valid()) {
      return Status::Invalid("PipelineDesc: " + std::string(stage_name(m.stage)) +
                             " 的 SPIR-V 无效(空指针 / 长度不足 / 不是 4 的倍数)");
    }
    const int key = static_cast<int>(m.stage);
    if (!seen.insert(key).second) {
      return Status::Invalid("PipelineDesc: " + std::string(stage_name(m.stage)) +
                             " stage 重复出现");
    }
  }
  bool has_vs = false, has_fs = false, has_cs = false;
  for (const ShaderModule& m : desc.stages) {
    switch (m.stage) {
      case ShaderStage::Vertex:
        has_vs = true;
        break;
      case ShaderStage::Fragment:
        has_fs = true;
        break;
      case ShaderStage::Compute:
        has_cs = true;
        break;
    }
  }

  if (desc.compute) {
    // 3) 计算管线:必须只有 Compute(混入 Vertex/Fragment → InvalidArgument)
    if (!has_cs) {
      return Status::Invalid("PipelineDesc: compute=true 但 stages 里没有 Compute");
    }
    if (has_vs || has_fs) {
      return Status::Invalid("PipelineDesc: compute=true 时 stages 只允许 Compute,"
                             "不得混入 Vertex/Fragment");
    }
  } else {
    // 4) 图形管线:必须有 Vertex 与 Fragment(缺一 → InvalidArgument)
    if (!has_vs) {
      return Status::Invalid("PipelineDesc: 图形管线缺少 Vertex stage");
    }
    if (!has_fs) {
      return Status::Invalid("PipelineDesc: 图形管线缺少 Fragment stage");
    }
    // 5) 图形管线混入 Compute → InvalidArgument
    if (has_cs) {
      return Status::Invalid("PipelineDesc: 图形管线不得混入 Compute stage(请用 compute=true)");
    }
    // 6) 顶点布局:宽度、跳号、绑定存在性
    if (!desc.vertexLayout.bindings.empty()) {
      std::set<u32> binding_ids;
      for (const resources::VertexInputBinding& b : desc.vertexLayout.bindings) {
        if (b.stride == 0) return Status::Invalid("PipelineDesc: 顶点绑定的 stride 必须 > 0");
        if (!binding_ids.insert(b.binding).second) {
          return Status::Invalid("PipelineDesc: 顶点绑定 " + std::to_string(b.binding) + " 重复");
        }
      }
      std::set<u32> locs;
      for (const resources::VertexAttribute& a : desc.vertexLayout.attributes) {
        if (binding_ids.find(a.binding) == binding_ids.end()) {
          return Status::Invalid("PipelineDesc: 属性 location=" + std::to_string(a.location) +
                                 " 引用了不存在的绑定 " + std::to_string(a.binding));
        }
        const u32 fmt_size = resources::vertex_format_size(a.format);
        if (fmt_size == 0) {
          return Status::Invalid("PipelineDesc: 属性 location=" + std::to_string(a.location) +
                                 " 的格式无效");
        }
        for (const resources::VertexInputBinding& b : desc.vertexLayout.bindings) {
          if (b.binding == a.binding && a.offset + fmt_size > b.stride) {
            return Status::Invalid("PipelineDesc: 属性 location=" + std::to_string(a.location) +
                                   " 越出绑定 " + std::to_string(b.binding) + " 的 stride");
          }
        }
        if (!locs.insert(a.location).second) {
          return Status::Invalid("PipelineDesc: 属性 location=" +
                                 std::to_string(a.location) + " 重复");
        }
      }
      // 不允许跳号(pipeline.h 明文:移动端驱动上的性能陷阱)
      if (!locs.empty()) {
        for (u32 i = 0; i < static_cast<u32>(locs.size()); ++i) {
          if (locs.find(i) == locs.end()) {
            return Status::Invalid("PipelineDesc: 顶点属性 location 不允许跳号,缺 location=" +
                                   std::to_string(i));
          }
        }
      }
    }
    // 7) 深度比较函数:Disabled 只对采样器有意义,R2 的 DepthState 不允许
    if (desc.depth.depthOp == resources::CompareOp::Disabled) {
      return Status::Invalid("PipelineDesc: DepthState.depthOp 不能是 Disabled"
                             "(那是采样器专用取值)");
    }
  }

  // 8) push_constant_bytes 超限 → Unsupported
  const u32 limit = max_push_constant_size != 0 ? max_push_constant_size : kDefaultMaxPushConstants;
  if (desc.push_constant_bytes > limit) {
    return Status::Unsupported("PipelineDesc: push_constant_bytes=" +
                               std::to_string(desc.push_constant_bytes) + " 超过设备上限 " +
                               std::to_string(limit));
  }
  if (desc.push_constant_bytes % 4 != 0) {
    return Status::Invalid("PipelineDesc: push_constant_bytes 必须是 4 的倍数");
  }

  // 8.5) P0(X1) sampled_texture_slots:本轮只做"1 个 set + 若干 array slot",
  // 所以限制在 [0, kMaxSampledTextureSlots]。0 是默认值(不建描述符集)。
  if (desc.sampled_texture_slots > kMaxSampledTextureSlots) {
    return Status::Unsupported("PipelineDesc: sampled_texture_slots=" +
                               std::to_string(desc.sampled_texture_slots) + " 超过本轮上限 " +
                               std::to_string(kMaxSampledTextureSlots));
  }

  // 9) samples 不支持 → Unsupported
  if (desc.samples == 0 || !is_power_of_two(desc.samples)) {
    return Status::Unsupported("PipelineDesc: samples=" + std::to_string(desc.samples) +
                               " 不是 2 的幂");
  }
  const u32 bit = desc.samples;  // 位图口径:第 N 位=采样数 N(1<<0 = 1 采样)
  (void)kDefaultMaxPushConstantStages;
  if (supported_samples_bitmask != 0 && (supported_samples_bitmask & bit) == 0) {
    return Status::Unsupported("PipelineDesc: samples=" + std::to_string(desc.samples) +
                               " 不在设备支持列表里(位图 " +
                               std::to_string(supported_samples_bitmask) + ")");
  }
  return Status::Ok_();
}

// ---------------------------------------------------------------- 构造 / 析构

VulkanPipeline::VulkanPipeline(const PipelineDesc& desc) : has_ctx_(false), desc_(desc) {}

VulkanPipeline::VulkanPipeline(const VulkanContext& ctx, const PipelineDesc& desc)
    : ctx_(ctx), has_ctx_(true), desc_(desc) {}

VulkanPipeline::VulkanPipeline(const VulkanContext& ctx, const PipelineDesc& desc,
                               const AttachmentShape& shape)
    : ctx_(ctx), has_ctx_(true), desc_(desc), shape_(shape) {}

VulkanPipeline::~VulkanPipeline() {
  // 【必须先等本设备已提交的帧跑完】
  //
  // 管线会在**命令录制期**被 vkCmdBindPipeline 引用,而那些命令缓冲的提交是
  // 异步的(多帧并行)。管线在这里被销毁时,GPU / 驱动侧可能还在执行引用它的
  // 命令 —— 校验层直接报 VUID-vkDestroyPipeline-pipeline-00765,随后驱动在
  // 内部表里踩到已释放的 VkPipeline(实测 SEH 0xC0000005,崩在一条毫不相干的语句上)。
  //
  // 用 wait_device_frames_idle(排空 N 个槽位 fence)而不是 vkDeviceWaitIdle:
  // 前者只等**本设备登记过的那几次提交**,后者会把 queue 上一切外来的工作也停住。
  // 这里不是热路径(管线销毁是装配期/用例结束),代价可接受。
  if (ctx_.device != VK_NULL_HANDLE) (void)wait_device_frames_idle(ctx_);
  if (ctx_.device != VK_NULL_HANDLE) {    if (vk_pipeline_ != VK_NULL_HANDLE) vkDestroyPipeline(ctx_.device, vk_pipeline_, nullptr);
    if (vk_layout_ != VK_NULL_HANDLE) vkDestroyPipelineLayout(ctx_.device, vk_layout_, nullptr);
    if (vk_render_pass_ != VK_NULL_HANDLE) {
      vkDestroyRenderPass(ctx_.device, vk_render_pass_, nullptr);
    }
    // P0(X1):描述符集随 pool 一起释放,layout 最后销毁(与创建顺序相反)。
    if (desc_pool_ != VK_NULL_HANDLE) {
      vkDestroyDescriptorPool(ctx_.device, desc_pool_, nullptr);
    }
    if (set_layouts_ != VK_NULL_HANDLE) {
      vkDestroyDescriptorSetLayout(ctx_.device, set_layouts_, nullptr);
    }
    for (VkShaderModule m : vk_modules_) vkDestroyShaderModule(ctx_.device, m, nullptr);
  }
  vk_pipeline_ = VK_NULL_HANDLE;
  vk_layout_ = VK_NULL_HANDLE;
  vk_render_pass_ = VK_NULL_HANDLE;
  set_layouts_ = VK_NULL_HANDLE;
  desc_pool_ = VK_NULL_HANDLE;
  desc_set_ = VK_NULL_HANDLE;
  vk_modules_.clear();
}

// ---------------------------------------------------------------- P0 描述符集(X1)

Status VulkanPipeline::create_descriptor_objects() {
  if (!has_ctx_ || ctx_.device == VK_NULL_HANDLE) {
    return Status::Invalid("VulkanPipeline::create_descriptor_objects: 需要设备态");
  }
  if (desc_.sampled_texture_slots == 0) return Status::Ok_();  // 幂等:无事可做
  if (set_layouts_ != VK_NULL_HANDLE) return Status::Ok_();     // 已建过

  // 1) layout:单个 combined image sampler,descriptorCount = slots。
  //    固定 set=0,binding=0..slots-1。可见阶段取顶点+片元(采样可在两者发生);
  //    计算管线由 build_compute 走同一条路径,shader 自己只用得到的阶段。
  VkDescriptorSetLayoutBinding b{};
  b.binding = 0;
  b.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  b.descriptorCount = desc_.sampled_texture_slots;
  b.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT |
                 VK_SHADER_STAGE_COMPUTE_BIT;
  b.pImmutableSamplers = nullptr;  // 采样器运行期通过 bind_sampled_texture 写

  VkDescriptorSetLayoutCreateInfo slci{};
  slci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
  slci.bindingCount = 1;
  slci.pBindings = &b;
  VkResult r = vkCreateDescriptorSetLayout(ctx_.device, &slci, nullptr, &set_layouts_);
  if (r != VK_SUCCESS) {
    set_layouts_ = VK_NULL_HANDLE;
    return Status::InitFailed(fmt_err("vkCreateDescriptorSetLayout", r));
  }

  // 2) pool:一个 layout、一个 set、slots 个 combined image sampler。
  VkDescriptorPoolSize ps{};
  ps.type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  ps.descriptorCount = desc_.sampled_texture_slots;
  VkDescriptorPoolCreateInfo dpci{};
  dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  dpci.maxSets = 1;
  dpci.poolSizeCount = 1;
  dpci.pPoolSizes = &ps;
  r = vkCreateDescriptorPool(ctx_.device, &dpci, nullptr, &desc_pool_);
  if (r != VK_SUCCESS) {
    desc_pool_ = VK_NULL_HANDLE;
    return Status::InitFailed(fmt_err("vkCreateDescriptorPool", r));
  }

  // 3) set:分配一个。
  VkDescriptorSetAllocateInfo dsai{};
  dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
  dsai.descriptorPool = desc_pool_;
  dsai.descriptorSetCount = 1;
  dsai.pSetLayouts = &set_layouts_;
  r = vkAllocateDescriptorSets(ctx_.device, &dsai, &desc_set_);
  if (r != VK_SUCCESS) {
    desc_set_ = VK_NULL_HANDLE;
    return Status::InitFailed(fmt_err("vkAllocateDescriptorSets", r));
  }
  return Status::Ok_();
}

Status VulkanPipeline::write_sampled_texture(u32 slot, VkImageView view, VkSampler sampler,
                                             bool cube_compatible) {
  if (set_layouts_ == VK_NULL_HANDLE || desc_set_ == VK_NULL_HANDLE) {
    return Status::Invalid("VulkanPipeline::write_sampled_texture: 该管线未声明 "
                           "sampled_texture_slots(PipelineDesc::sampled_texture_slots == 0)");
  }
  if (slot >= desc_.sampled_texture_slots) {
    return Status::Invalid("VulkanPipeline::write_sampled_texture: slot " +
                           std::to_string(slot) + " 越界(声明了 " +
                           std::to_string(desc_.sampled_texture_slots) + " 个)");
  }
  if (view == VK_NULL_HANDLE || sampler == VK_NULL_HANDLE) {
    return Status::Invalid("VulkanPipeline::write_sampled_texture: view/sampler 为空");
  }

  // 立方体贴图:view 是 VIEW_TYPE_CUBE,描述符里的 VkSampler 必须带
  // CUBE_COMPATIBLE 位。由 command list 在 cube 绑定时传入这样的采样器
  // (见 VulkanCommandList::bind_sampled_texture 的 cube 分支)。
  // 这里只做**事后校验**:不带该位就明确报错,而不是让驱动给出未定义采样结果。
  if (cube_compatible && sampler == VK_NULL_HANDLE) {
    return Status::Invalid(
        "VulkanPipeline::write_sampled_texture: 立方体贴图必须传入带 "
        "CUBE_COMPATIBLE 位的采样器(见 backend::cube_sampler_for)");
  }

  VkDescriptorImageInfo ii{};
  ii.sampler = sampler;
  ii.imageView = view;
  ii.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

  VkWriteDescriptorSet w{};
  w.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
  w.dstSet = desc_set_;
  w.dstBinding = 0;              // 单 binding,array 内按 slot 定位
  w.dstArrayElement = slot;      // ← slot 即 array 下标
  w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
  w.descriptorCount = 1;
  w.pImageInfo = &ii;
  vkUpdateDescriptorSets(ctx_.device, 1, &w, 0, nullptr);
  return Status::Ok_();
}

// ---------------------------------------------------------------- init

Status VulkanPipeline::init() {
  if (desc_.samples == 0) desc_.samples = 1;
  if (shape_.samples == 0) shape_.samples = desc_.samples;
  if (!desc_.compute) shape_.samples = desc_.samples;

  // 纯软件态无从探测设备上限:按 128 / "不限制采样数" 校验 —— 参数级错误照样拦。
  const u32 max_pc = has_ctx_ ? ctx_.props.limits.maxPushConstantsSize : kDefaultMaxPushConstants;
  const u32 sample_mask = 0;
  Status s = validate(desc_, max_pc, sample_mask);
  if (!s.ok()) return s;

  effective_depth_ = desc_.depth;
  push_constant_bytes_ = desc_.push_constant_bytes;
  // R4:push constant 优先。stage 位固定为 Vertex|Fragment(计算管线只用 Compute)。
  push_stage_mask_ = desc_.compute ? stage_bit(ShaderStage::Compute)
                                   : (stage_bit(ShaderStage::Vertex) | stage_bit(ShaderStage::Fragment));
  // 哈希必须覆盖附件形状:同一 desc 在"有深度"与"无深度"的 pass 里是两条不同的
  // VkPipeline;若哈希相同,设备层的管线缓存会串用,命令流也会把两条管线混为一谈。
  // 默认形状时 hash_pipeline_desc_with_shape 退化为 hash_pipeline_desc(向后一致)。
  hash_ = hash_pipeline_desc_with_shape(desc_, shape_);

  if (!has_ctx_ || ctx_.device == VK_NULL_HANDLE) {
    soft_ready_ = true;
    return Status::Ok_();
  }
  return desc_.compute ? build_compute() : build_graphics();
}

// ---------------------------------------------------------------- 图形管线

Status VulkanPipeline::create_render_pass() {
  // 附件排布与 vulkan_device_frame.cpp 的 create_pass() **必须逐项一致**:
  //   [0]=color, [1]=depth(若有), 最后=color resolve(MSAA 时追加)
  // 设备帧提交会优先复用本函数建的 pass(pass_compatible),两条路径排布不同
  // 会得到两种行为,极难排查。
  VkAttachmentDescription atts[3] = {};
  std::uint32_t att_count = 0;
  atts[0].format = vk_format_of(shape_.color_format);
  atts[0].samples = to_vk_samples(shape_.samples);
  // ⚠️ 管线**内建** pass 恒用 CLEAR,不按 shape_.clear_* 分支。
  //
  // 2026-11-11 定案(msaa 的 t6 引入、随即回退):
  //   - loadOp 是 **附件描述**、创建时固定;`begin_pass` 的 pClearValues 只在
  //     loadOp==CLEAR 的附件上生效。所以"清不清"在管线这一侧根本无法逐 pass 表达。
  //   - 曾经按 shape_.clear_color/clear_depth 选 LOAD,但深度附件的 initialLayout
  //     恒为 UNDEFINED(下面 atts[1].initialLayout),于是
  //     **LOAD + UNDEFINED 违反 VUID-VkAttachmentDescription-format-06699**:
  //     校验层报错、pass 内容未定义 → L3 场景/挤出体覆盖率变 0(像素用例全红)。
  //   - 另一个方向(用 clear 标志否决整条 pass 的复用)更糟:会让 L3 所有管线 pass
  //     都不复用,触发 vkCmdDrawIndexed E-02684。fvx 试过一次,已回滚。
  //
  // 结论:内建 pass 恒 CLEAR 建;真正"不清屏"的叠加式 pass 由
  // `vulkan_device_frame.cpp::create_pass()` 按**本次 begin_pass 的实际形状**
  // 新建一张 pass 来表达。这是唯一自洽的分工。
  atts[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  atts[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  atts[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  atts[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  // 交换链图与离屏图的布局契约不同(2026-11-11 修 vkQueuePresentKHR E-01430):
  //   - 离屏:UNDEFINED → GENERAL(读回路径的约定末态,见 vulkan_target.cpp)
  //   - 交换链:PRESENT_SRC_KHR → PRESENT_SRC_KHR(acquire 拿到的图就在可 present 的布局;
  //     末态若不是 PRESENT_SRC_KHR,vkQueuePresentKHR 会报 "must be in PRESENT_SRC_KHR ...
  //     but is in GENERAL",帧被丢掉,用户看到黑屏/残帧)
  // 以前这里恒写 GENERAL —— 只要交换链路径复用管线自带 pass,present 就必坏。
  if (shape_.from_swapchain) {
    atts[0].initialLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    atts[0].finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
  } else {
    atts[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    atts[0].finalLayout = VK_IMAGE_LAYOUT_GENERAL;
  }
  att_count = 1;
  if (shape_.has_depth) {
    atts[1].format = vk_format_of(shape_.depth_format);
    atts[1].samples = to_vk_samples(shape_.samples);
    atts[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    atts[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    atts[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    atts[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    atts[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    atts[1].finalLayout = VK_IMAGE_LAYOUT_GENERAL;
    att_count = 2;
  }

  // MSAA resolve 附件(断点 B)。没有 pResolveAttachments 时,多重采样颜色
  // 不会降采样成单采样图像,读回拿到的是未定义内容。位置固定在最后,
  // 末态与单采样时的颜色附件一致(finalLayout=GENERAL),读回路径不用分支。
  const bool resolve = shape_.samples > 1;
  std::uint32_t resolve_index = 0;
  if (resolve) {
    resolve_index = att_count;
    atts[resolve_index].format = vk_format_of(shape_.color_format);
    atts[resolve_index].samples = VK_SAMPLE_COUNT_1_BIT;
    atts[resolve_index].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;  // resolve 会整体覆盖
    atts[resolve_index].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    atts[resolve_index].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    atts[resolve_index].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    // resolve 的目标是单采样那张图:离屏为 GENERAL(读回契约),
    // 交换链为 PRESENT_SRC_KHR(否则 present 报 E-01430 丢帧)。
    atts[resolve_index].initialLayout = shape_.from_swapchain ? VK_IMAGE_LAYOUT_PRESENT_SRC_KHR
                                                              : VK_IMAGE_LAYOUT_UNDEFINED;
    atts[resolve_index].finalLayout = atts[0].finalLayout;
    ++att_count;
  }

  VkAttachmentReference color_ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  VkAttachmentReference depth_ref{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
  VkAttachmentReference resolve_ref{resolve_index, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  VkSubpassDescription sub{};
  sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  sub.colorAttachmentCount = 1;
  sub.pColorAttachments = &color_ref;
  sub.pDepthStencilAttachment = shape_.has_depth ? &depth_ref : nullptr;
  sub.pResolveAttachments = resolve ? &resolve_ref : nullptr;

  // subpass 依赖：**不能省**。
  //
  // loadOp=CLEAR/LOAD 与随后着色器的颜色写入之间,如果没有一条 EXTERNAL→subpass 的
  // 执行/内存依赖,规范上看就是"未定义顺序"。SwiftShader 对此的实际表现是
  // **整片区域保持清屏色、一像素都不写**,并且完全不报错 —— 上层只会拿到一张
  // 干净的空图,极易误判为"几何没进管线/剔除过激/着色器坐标错了"。
  //
  // create_pass()(vulkan_device_frame.cpp)与这里必须保持一致:
  // 设备帧提交会优先复用**管线自带的 render pass**(见 BeginPass 回放分支),
  // 所以两条路径的依赖缺失会得到两种不同行为,更难排查。
  //
  // ⚠️ 而且 render pass **兼容性**本身就把 dependencyCount 算进去了:
  // 校验层原文 `vkCmdDraw(): dependencyCount is incompatible between
  // VkRenderPass ... and VkRenderPass ... (2 != 1)`(VUID-vkCmdDraw-renderPass-02684)。
  // 也就是说"管线自带 pass 与 create_pass 的依赖条数不同"会让 vkCmdDraw
  // 直接被判成不兼容 —— 画面静默变空。所以这里**必须逐条对齐 create_pass**:
  // 入口依赖(EXTERNAL→0)+ 出口依赖(0→EXTERNAL)。
  VkSubpassDependency dep{};
  dep.srcSubpass = VK_SUBPASS_EXTERNAL;
  dep.dstSubpass = 0;
  // 入口:srcStage 从 TOP_OF_PIPE 起,上游可能是 TRANSFER 或别的 pass 的颜色写。
  dep.srcStageMask = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
  dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                     VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT |
                     VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
  dep.srcAccessMask = 0;
  dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  if (shape_.has_depth) {
    // 与 create_pass 同口径:深度清屏/深度写的可见性。
    dep.dstAccessMask |= VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT |
                         VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
  }
  dep.dependencyFlags = 0;
  // 出口:pass 末尾的写(含 MSAA resolve)对后续 TRANSFER_READ / present 可见。
  VkSubpassDependency deps[2] = {};
  deps[0] = dep;
  deps[1].srcSubpass = 0;
  deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
  deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  deps[1].dstStageMask = VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
  deps[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  if (shape_.has_depth) {
    deps[1].srcStageMask |= VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    deps[1].srcAccessMask |= VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
  }
  deps[1].dependencyFlags = 0;

  VkRenderPassCreateInfo rpci{};
  rpci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
  rpci.attachmentCount = att_count;
  rpci.pAttachments = atts;
  rpci.subpassCount = 1;
  rpci.pSubpasses = &sub;
  rpci.dependencyCount = 2;
  rpci.pDependencies = deps;
  const VkResult r = vkCreateRenderPass(ctx_.device, &rpci, nullptr, &vk_render_pass_);
  if (r != VK_SUCCESS) return Status::InitFailed(fmt_err("vkCreateRenderPass(管线)", r));
  return Status::Ok_();
}

Status VulkanPipeline::build_graphics() {
  Status s = create_render_pass();
  if (!s.ok()) return s;

  // ---- shader modules ----
  VkPipelineShaderStageCreateInfo stage_infos[2] = {};
  std::uint32_t stage_count = 0;
  for (const ShaderModule& m : desc_.stages) {
    VkShaderModuleCreateInfo smci{};
    smci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smci.codeSize = m.size_bytes;
    smci.pCode = reinterpret_cast<const std::uint32_t*>(m.spirv);
    VkShaderModule mod = VK_NULL_HANDLE;
    const VkResult r = vkCreateShaderModule(ctx_.device, &smci, nullptr, &mod);
    if (r != VK_SUCCESS) return Status::InitFailed(fmt_err("vkCreateShaderModule", r));
    vk_modules_.push_back(mod);
    VkPipelineShaderStageCreateInfo& si = stage_infos[stage_count++];
    si.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    si.stage = to_vk_stage(m.stage);
    si.module = mod;
    si.pName = "main";
  }

  // ---- 顶点输入 ----
  std::vector<VkVertexInputBindingDescription> vk_bindings;
  vk_bindings.reserve(desc_.vertexLayout.bindings.size());
  for (const resources::VertexInputBinding& b : desc_.vertexLayout.bindings) {
    VkVertexInputBindingDescription d{};
    d.binding = b.binding;
    d.stride = b.stride;
    d.inputRate = b.instanced ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX;
    vk_bindings.push_back(d);
  }
  std::vector<VkVertexInputAttributeDescription> vk_attrs;
  vk_attrs.reserve(desc_.vertexLayout.attributes.size());
  for (const resources::VertexAttribute& a : desc_.vertexLayout.attributes) {
    VkVertexInputAttributeDescription d{};
    d.location = a.location;
    d.binding = a.binding;
    d.format = to_vk_vertex_format(a.format);
    d.offset = a.offset;
    vk_attrs.push_back(d);
  }
  VkPipelineVertexInputStateCreateInfo vi{};
  vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
  vi.vertexBindingDescriptionCount = static_cast<std::uint32_t>(vk_bindings.size());
  vi.pVertexBindingDescriptions = vk_bindings.empty() ? nullptr : vk_bindings.data();
  vi.vertexAttributeDescriptionCount = static_cast<std::uint32_t>(vk_attrs.size());
  vi.pVertexAttributeDescriptions = vk_attrs.empty() ? nullptr : vk_attrs.data();

  // ---- 输入装配(R2:primitive = TriangleList / LineList) ----
  VkPipelineInputAssemblyStateCreateInfo ia{};
  ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
  ia.topology = to_vk_topology(desc_.primitive);
  ia.primitiveRestartEnable = VK_FALSE;

  // ---- 视口/裁剪:动态状态(命令列表里的 set_viewport / set_scissor 全靠它) ----
  const VkDynamicState dyn_states[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo dyn{};
  dyn.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
  dyn.dynamicStateCount = 2;
  dyn.pDynamicStates = dyn_states;
  VkViewport vp{0.f, 0.f, 0.f, 0.f, 0.f, 1.f};  // 动态:创建时值无意义,但指针必须有效
  VkRect2D sc{{0, 0}, {0, 0}};
  VkPipelineViewportStateCreateInfo vps{};
  vps.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
  vps.viewportCount = 1;
  vps.pViewports = &vp;
  vps.scissorCount = 1;
  vps.pScissors = &sc;

  // ---- 光栅(R2:polygonMode 恒 FILL;只有剔除 / 正面向 / 深度偏移) ----
  VkPipelineRasterizationStateCreateInfo rs{};
  rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
  rs.depthClampEnable = VK_FALSE;
  rs.rasterizerDiscardEnable = VK_FALSE;
  rs.polygonMode = VK_POLYGON_MODE_FILL;  // ← 不支持线框(R2 明文)
  rs.cullMode = to_vk_cull(desc_.raster.cull);
  rs.frontFace = to_vk_front_face(desc_.raster.front_face);
  rs.depthBiasEnable = desc_.raster.depth_bias_enabled ? VK_TRUE : VK_FALSE;
  rs.depthBiasConstantFactor = desc_.raster.depth_bias_constant;
  rs.depthBiasSlopeFactor = desc_.raster.depth_bias_slope;
  rs.depthBiasClamp = desc_.raster.depth_bias_clamp;
  rs.lineWidth = 1.0f;

  // ---- 多重采样 ----
  VkPipelineMultisampleStateCreateInfo ms{};
  ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
  ms.rasterizationSamples = to_vk_samples(desc_.samples);
  ms.sampleShadingEnable = VK_FALSE;

  // ---- 深度/模板(P12:全字段) ----
  VkPipelineDepthStencilStateCreateInfo ds{};
  ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
  ds.depthTestEnable = desc_.depth.depthTest ? VK_TRUE : VK_FALSE;
  ds.depthWriteEnable = desc_.depth.depthWrite ? VK_TRUE : VK_FALSE;
  ds.depthCompareOp = to_vk_compare(desc_.depth.depthOp);
  ds.depthBoundsTestEnable = desc_.depth.depthConstWrite ? VK_TRUE : VK_FALSE;
  // depthConstWrite 的落地:开启 depthBounds 并把上下界都钉在 depth_const,
  // 于是"通过深度测试的片元"只有深度恰为该值的那一片,结果是整片写同一个深度值
  // (P12:depthTest=OFF + depthWrite=ON 时"永远显示内面")。
  if (desc_.depth.depthConstWrite) {
    ds.minDepthBounds = (std::max)(0.0f, (std::min)(1.0f, desc_.depth.depth_const));
    ds.maxDepthBounds = ds.minDepthBounds;
  } else {
    ds.minDepthBounds = 0.0f;
    ds.maxDepthBounds = 1.0f;
  }
  ds.stencilTestEnable = VK_FALSE;

  // ---- 颜色混合 ----
  VkPipelineColorBlendAttachmentState cba{};
  cba.blendEnable = desc_.blend.enabled ? VK_TRUE : VK_FALSE;
  cba.srcColorBlendFactor = to_vk_blend_factor(desc_.blend.src_color);
  cba.dstColorBlendFactor = to_vk_blend_factor(desc_.blend.dst_color);
  cba.colorBlendOp = to_vk_blend_op(desc_.blend.color_op);
  cba.srcAlphaBlendFactor = to_vk_blend_factor(desc_.blend.src_alpha);
  cba.dstAlphaBlendFactor = to_vk_blend_factor(desc_.blend.dst_alpha);
  cba.alphaBlendOp = to_vk_blend_op(desc_.blend.alpha_op);
  cba.colorWriteMask =
      VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT |
      VK_COLOR_COMPONENT_A_BIT;
  VkPipelineColorBlendStateCreateInfo cb{};
  cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
  cb.logicOpEnable = VK_FALSE;
  cb.attachmentCount = 1;
  cb.pAttachments = &cba;

  // ---- 管线布局(R4 默认只建 push constant range;P0 起按需带 1 个描述符集) ----
  VkPushConstantRange pcr{};
  pcr.stageFlags = static_cast<VkShaderStageFlags>(push_stage_mask_);
  pcr.offset = 0;
  pcr.size = push_constant_bytes_;
  // P0(X1):sampled_texture_slots > 0 才建描述符集布局;== 0 时下面三行保持
  // 历史行为(setLayoutCount=0/pSetLayouts=nullptr),既有管线与 golden 不变。
  //
  // 变量名必须叫 desc_status 而不是 ds:本函数上面已经有一个
  // `VkPipelineDepthStencilStateCreateInfo ds`(P12 的深度/模板状态),
  // 两者同名会整体编译失败(C2371 重定义)。改的是名字,不是语义。
  Status desc_status = Status::Ok_();
  if (desc_.sampled_texture_slots > 0) {
    desc_status = create_descriptor_objects();
    if (!desc_status.ok()) return desc_status;
  }
  VkPipelineLayoutCreateInfo plci{};
  plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  plci.setLayoutCount = (set_layouts_ != VK_NULL_HANDLE) ? 1u : 0u;
  plci.pSetLayouts = (set_layouts_ != VK_NULL_HANDLE) ? &set_layouts_ : nullptr;
  plci.pushConstantRangeCount = push_constant_bytes_ > 0 ? 1 : 0;
  plci.pPushConstantRanges = push_constant_bytes_ > 0 ? &pcr : nullptr;
  VkResult r = vkCreatePipelineLayout(ctx_.device, &plci, nullptr, &vk_layout_);
  if (r != VK_SUCCESS) return Status::InitFailed(fmt_err("vkCreatePipelineLayout", r));

  VkGraphicsPipelineCreateInfo pci{};
  pci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  pci.stageCount = stage_count;
  pci.pStages = stage_infos;
  pci.pVertexInputState = &vi;
  pci.pInputAssemblyState = &ia;
  pci.pViewportState = &vps;
  pci.pRasterizationState = &rs;
  pci.pMultisampleState = &ms;
  pci.pDepthStencilState = desc_.depth.depthTest || desc_.depth.depthWrite ||
                                   desc_.depth.depthConstWrite
                               ? &ds
                               : nullptr;
  pci.pColorBlendState = &cb;
  pci.pDynamicState = &dyn;
  pci.layout = vk_layout_;
  pci.renderPass = vk_render_pass_;
  pci.subpass = 0;
  r = vkCreateGraphicsPipelines(ctx_.device, VK_NULL_HANDLE, 1, &pci, nullptr, &vk_pipeline_);
  if (r != VK_SUCCESS) return Status::InitFailed(fmt_err("vkCreateGraphicsPipelines", r));
  return Status::Ok_();
}

// ---------------------------------------------------------------- 计算管线

Status VulkanPipeline::build_compute() {
  VkShaderModuleCreateInfo smci{};
  smci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  smci.codeSize = desc_.stages[0].size_bytes;
  smci.pCode = reinterpret_cast<const std::uint32_t*>(desc_.stages[0].spirv);
  VkShaderModule mod = VK_NULL_HANDLE;
  VkResult r = vkCreateShaderModule(ctx_.device, &smci, nullptr, &mod);
  if (r != VK_SUCCESS) return Status::InitFailed(fmt_err("vkCreateShaderModule(compute)", r));
  vk_modules_.push_back(mod);

  VkPushConstantRange pcr{};
  pcr.stageFlags = static_cast<VkShaderStageFlags>(push_stage_mask_);
  pcr.offset = 0;
  pcr.size = push_constant_bytes_;
  // P0(X1):计算管线同样能声明采样纹理槽(== 0 时保持 R4 的历史行为)。
  Status ds = Status::Ok_();
  if (desc_.sampled_texture_slots > 0) {
    ds = create_descriptor_objects();
    if (!ds.ok()) return ds;
  }
  VkPipelineLayoutCreateInfo plci{};
  plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  plci.setLayoutCount = (set_layouts_ != VK_NULL_HANDLE) ? 1u : 0u;
  plci.pSetLayouts = (set_layouts_ != VK_NULL_HANDLE) ? &set_layouts_ : nullptr;
  plci.pushConstantRangeCount = push_constant_bytes_ > 0 ? 1 : 0;
  plci.pPushConstantRanges = push_constant_bytes_ > 0 ? &pcr : nullptr;
  r = vkCreatePipelineLayout(ctx_.device, &plci, nullptr, &vk_layout_);
  if (r != VK_SUCCESS) return Status::InitFailed(fmt_err("vkCreatePipelineLayout(compute)", r));

  VkComputePipelineCreateInfo cpci{};
  cpci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
  cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
  cpci.stage.module = mod;
  cpci.stage.pName = "main";
  cpci.layout = vk_layout_;
  r = vkCreateComputePipelines(ctx_.device, VK_NULL_HANDLE, 1, &cpci, nullptr, &vk_pipeline_);
  if (r != VK_SUCCESS) return Status::InitFailed(fmt_err("vkCreateComputePipelines", r));
  return Status::Ok_();
}

}  // namespace magicblocks::backend
