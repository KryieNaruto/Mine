// MagicBlocks —— Vulkan 后端:L1 管线实现(R2 / R4 / G1 / P12)
#include "backend/vulkan/vulkan_pipeline.h"

#include <algorithm>
#include <cstring>
#include <set>

#include "backend/vulkan/vulkan_resource_common.h"

namespace magicblocks::backend {

using namespace magicblocks::device;

namespace {

constexpr u32 kDefaultMaxPushConstants = 128;  // 移动端下限(spec R4:实测 >= 128)
constexpr u32 kDefaultMaxPushConstantStages = 2;  // Vertex + Fragment

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

VkSampleCountFlagBits to_vk_samples(u32 n) {
  switch (n) {
    case 2:
      return VK_SAMPLE_COUNT_2_BIT;
    case 4:
      return VK_SAMPLE_COUNT_4_BIT;
    case 8:
      return VK_SAMPLE_COUNT_8_BIT;
    case 16:
      return VK_SAMPLE_COUNT_16_BIT;
    default:
      return VK_SAMPLE_COUNT_1_BIT;
  }
}

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
  if (ctx_.device != VK_NULL_HANDLE) {
    if (vk_pipeline_ != VK_NULL_HANDLE) vkDestroyPipeline(ctx_.device, vk_pipeline_, nullptr);
    if (vk_layout_ != VK_NULL_HANDLE) vkDestroyPipelineLayout(ctx_.device, vk_layout_, nullptr);
    if (vk_render_pass_ != VK_NULL_HANDLE) {
      vkDestroyRenderPass(ctx_.device, vk_render_pass_, nullptr);
    }
    for (VkShaderModule m : vk_modules_) vkDestroyShaderModule(ctx_.device, m, nullptr);
  }
  vk_pipeline_ = VK_NULL_HANDLE;
  vk_layout_ = VK_NULL_HANDLE;
  vk_render_pass_ = VK_NULL_HANDLE;
  vk_modules_.clear();
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
  hash_ = hash_pipeline_desc(desc_);

  if (!has_ctx_ || ctx_.device == VK_NULL_HANDLE) {
    soft_ready_ = true;
    return Status::Ok_();
  }
  return desc_.compute ? build_compute() : build_graphics();
}

// ---------------------------------------------------------------- 图形管线

Status VulkanPipeline::create_render_pass() {
  VkAttachmentDescription atts[2] = {};
  std::uint32_t att_count = 0;
  atts[0].format = vk_format_of(shape_.color_format);
  atts[0].samples = to_vk_samples(shape_.samples);
  atts[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  atts[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  atts[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  atts[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  atts[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  atts[0].finalLayout = VK_IMAGE_LAYOUT_GENERAL;
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
  VkAttachmentReference color_ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  VkAttachmentReference depth_ref{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
  VkSubpassDescription sub{};
  sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  sub.colorAttachmentCount = 1;
  sub.pColorAttachments = &color_ref;
  sub.pDepthStencilAttachment = shape_.has_depth ? &depth_ref : nullptr;

  VkRenderPassCreateInfo rpci{};
  rpci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
  rpci.attachmentCount = att_count;
  rpci.pAttachments = atts;
  rpci.subpassCount = 1;
  rpci.pSubpasses = &sub;
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

  // ---- 管线布局(R4:只建 push constant range,setLayoutCount 恒 0) ----
  VkPushConstantRange pcr{};
  pcr.stageFlags = static_cast<VkShaderStageFlags>(push_stage_mask_);
  pcr.offset = 0;
  pcr.size = push_constant_bytes_;
  VkPipelineLayoutCreateInfo plci{};
  plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  plci.setLayoutCount = 0;        // ← R4:不建描述符集
  plci.pSetLayouts = nullptr;     // ← Pipeline.UsesPushConstantsOnly 的结构性证据
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
  VkPipelineLayoutCreateInfo plci{};
  plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  plci.setLayoutCount = 0;  // R4
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
