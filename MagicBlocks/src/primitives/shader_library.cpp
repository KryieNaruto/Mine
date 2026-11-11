// MagicBlocks SDK —— L3 `IShaderLibrary` 实现
//
// 两件事:
//   1. 模板登记(名字 → SPIR-V + 顶点布局 + push constant 字节数);
//   2. 管线派生 + 缓存:键 = (ShaderKey, RasterStateL3, TargetShape)。
//
// 缓存是 L3 的性能关键之一:`acquire_pipeline` 在稳态帧里**只做一次哈希查找**,
// 不建任何对象、不分配内存(命中路径上连 std::string 都不构造)。
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "mb_internal/magicblocks/primitives/shader_library.h"

// t6 / R9b:按形状派生管线的**公共**入口(不引入任何 backend:: / Vk* 头)。
#include "mb_internal/magicblocks/resources/pipeline.h"

namespace magicblocks::primitives {

// ---------------------------------------------------------------- 哈希工具

namespace {

inline std::uint64_t fnv1a64(const void* data, std::size_t size, std::uint64_t seed) {
  const auto* p = static_cast<const std::uint8_t*>(data);
  std::uint64_t h = seed;
  for (std::size_t i = 0; i < size; ++i) {
    h ^= p[i];
    h *= 0x100000001b3ull;
  }
  return h;
}

template <typename T>
inline void hash_mix(std::uint64_t* h, const T& v) {
  std::uint64_t raw = 0;
  static_assert(sizeof(T) <= sizeof(std::uint64_t), "hash_mix 只处理 <= 8 字节的平凡值");
  std::memcpy(&raw, &v, sizeof(T));
  *h ^= raw + 0x9e3779b97f4a7c15ull + (*h << 6) + (*h >> 2);
}

// 管线键:ShaderKey + RasterState + TargetShape。**与 MaterialKey 分开**:
// 合批排序时第一键是 pipeline_key(决定 bind_pipeline),第二键才是 material。
std::uint64_t hash_pipeline_key(const ShaderKey& key, const RasterStateL3& raster,
                                const TargetShape& shape) {
  std::uint64_t h = 0xcbf29ce484222325ull;
  hash_mix(&h, key.name_hash);
  hash_mix(&h, key.flags);
  hash_mix(&h, static_cast<std::uint32_t>(raster.cull));
  hash_mix(&h, static_cast<std::uint32_t>(raster.primitive));
  hash_mix(&h, static_cast<std::uint32_t>(raster.depth_test ? 1u : 0u));
  hash_mix(&h, static_cast<std::uint32_t>(raster.depth_write ? 1u : 0u));
  hash_mix(&h, static_cast<std::uint32_t>(raster.blend ? 1u : 0u));
  hash_mix(&h, static_cast<std::uint32_t>(shape.color_format));
  hash_mix(&h, static_cast<std::uint32_t>(shape.has_depth ? 1u : 0u));
  hash_mix(&h, shape.samples);
  return h;
}

ShaderModule to_module(ShaderStage stage, const ShaderModuleRef& ref) {
  ShaderModule m;
  m.stage = stage;
  m.spirv = ref.spirv;
  m.size_bytes = ref.size_bytes;
  return m;
}

// 把 L3 的固定功能状态翻译成 L1 的 PipelineDesc(spec §9.4 规则 1:
// 材质不碰深度状态,这里由 library 按 RasterState 统一决定)。
PipelineDesc make_pipeline_desc(const ShaderTemplateDesc& tmpl, const RasterStateL3& raster,
                                const TargetShape& shape) {
  PipelineDesc d;
  d.stages.push_back(to_module(ShaderStage::Vertex, tmpl.vertex));
  d.stages.push_back(to_module(ShaderStage::Fragment, tmpl.fragment));
  d.vertexLayout = tmpl.layout;
  d.primitive = raster.primitive;
  d.push_constant_bytes = tmpl.push_constant_bytes;
  d.samples = shape.samples;
  d.depth.depthTest = raster.depth_test;
  d.depth.depthWrite = raster.depth_write;
  d.raster.cull = raster.cull;
  d.blend.enabled = raster.blend;
  if (raster.blend) {
    // 标准 alpha 混合:src*a + dst*(1-a) 分别作用在颜色与 alpha 上。
    d.blend.src_color = resources::BlendFactor::SrcAlpha;
    d.blend.dst_color = resources::BlendFactor::OneMinusSrcAlpha;
    d.blend.src_alpha = resources::BlendFactor::One;
    d.blend.dst_alpha = resources::BlendFactor::OneMinusSrcAlpha;
    d.blend.color_op = resources::BlendOp::Add;
    d.blend.alpha_op = resources::BlendOp::Add;
  }
  return d;
}

}  // namespace

// ---------------------------------------------------------------- 公共小工具

std::uint32_t shader_name_hash(const char* name) {
  if (name == nullptr) return 0;
  // FNV-1a 32:跨进程稳定,命令流可复现。
  std::uint32_t h = 0x811c9dc5u;
  for (const char* p = name; *p != '\0'; ++p) {
    h ^= static_cast<std::uint8_t>(*p);
    h *= 0x01000193u;
  }
  return h;
}

ShaderKey make_shader_key(const char* name, std::uint32_t flags) {
  ShaderKey k;
  k.name_hash = shader_name_hash(name);
  k.flags = flags;
  return k;
}

// ---------------------------------------------------------------- 库实现

namespace {

class ShaderLibrary final : public IShaderLibrary {
 public:
  explicit ShaderLibrary(device::IRenderDevice* device) : device_(device) {}

  Status register_template(const ShaderTemplateDesc& desc) override {
    if (desc.name.empty()) return Status::Invalid("ShaderTemplate: name 不能为空");
    if (!desc.vertex.valid()) {
      return Status::Invalid("ShaderTemplate " + desc.name + ": vertex SPIR-V 无效");
    }
    if (!desc.fragment.valid()) {
      return Status::Invalid("ShaderTemplate " + desc.name + ": fragment SPIR-V 无效");
    }
    if (desc.layout.empty()) {
      return Status::Invalid("ShaderTemplate " + desc.name + ": 顶点布局为空");
    }
    const std::uint32_t nh = shader_name_hash(desc.name.c_str());
    auto it = index_.find(nh);
    if (it != index_.end()) {
      return Status::Invalid("ShaderTemplate: 名字 '" + desc.name + "' 重复登记");
    }
    index_.emplace(nh, templates_.size());
    templates_.push_back(desc);
    return Status::Ok_();
  }

  bool has(const char* name) const override {
    if (name == nullptr) return false;
    return index_.find(shader_name_hash(name)) != index_.end();
  }

  std::size_t template_count() const override { return templates_.size(); }

  const VertexLayout* layout_for(const ShaderKey& key) const override {
    const ShaderTemplateDesc* t = find(key);
    return t == nullptr ? nullptr : &t->layout;
  }

  Result<IPipeline*> acquire_pipeline(const ShaderKey& key, const RasterStateL3& raster,
                                      const TargetShape& shape) override {
    const std::uint64_t pk = hash_pipeline_key(key, raster, shape);
    // 快路径:命中就返回。键是 (ShaderKey, Raster, Shape) 的组合哈希,
    // 理论上有碰撞可能,因此命中后再逐字段比对一次(不省这一步)。
    auto range = by_key_.equal_range(pk);
    for (auto it = range.first; it != range.second; ++it) {
      PipelineEntry& e = entries_[it->second];
      if (e.key == key && e.raster == raster && e.shape == shape) {
        ++e.created;  // 仅计数,便于断言"命中不重建"
        if (e.pipeline == nullptr) {
          return Status::Unsupported("ShaderLibrary: 纯软件态下该组合没有可绑定管线");
        }
        return e.pipeline;
      }
    }

    const ShaderTemplateDesc* tmpl = find(key);
    if (tmpl == nullptr) {
      return Status::Invalid("ShaderLibrary: 未登记的 shader key(先 register_template)");
    }

    // 纯软件态(device_ == nullptr):L3 的材质/哈希逻辑必须能在无设备的 ctest 里跑,
    // 但**不能**伪造一个可绑定管线。直接返回 Unsupported 且**不缓存** ——
    // 否则上层会把"失败占位项"当成命中,后续接设备时再也建不出真管线。
    if (device_ == nullptr) {
      return Status::Unsupported(
          "ShaderLibrary: 未绑定设备(纯软件态不能派生可绑定管线;材质/哈希功能仍可用)");
    }

    // t6 / R9b(2026-11-11):这里过去**只**调 create_pipeline(desc),把 shape 丢了。
    // 后果:命中本缓存键的管线自带 pass 恒为"单采样、无深度",与带深度/带 MSAA
    // 的目标永远不兼容 → 设备帧提交只能退回自己建 pass,resolve 附件接不上。
    //
    // 修法是用 G8 的**单向注入**桥(不引入任何 backend:: 头):
    //   resources::resolve_pipeline_factory(device) → IPipelineFactory
    //                                 → create_shaped_pipeline(device, desc, shape)
    // 形状从 L3 的 TargetShape 逐字段翻译成公共值类型 resources::AttachmentShape。
    //
    // 能力纪律:解析不出工厂(纯软件态 / 后端未注册)时**显式回落**到
    // create_pipeline(desc),但仅当形状本就是"默认形状"时才允许 —— 否则
    // 形状会静默丢失,正是上面那条 bug 的复现路径。所以复杂形状拿不到工厂
    // 一律返回 Unsupported,绝不悄悄按 1 采样建管线。
    PipelineDesc desc = make_pipeline_desc(*tmpl, raster, shape);
    Result<std::unique_ptr<IPipeline>> r = Status::Unsupported("ShaderLibrary: 未走到任何派生分支");
    if (resources::IPipelineFactory* factory = resources::resolve_pipeline_factory(device_)) {
      resources::AttachmentShape att;
      att.color_format = shape.color_format;
      att.has_depth = shape.has_depth;
      att.depth_format = device::Format::D16_UNORM;  // L3 的 TargetShape 不区分深度格式
      att.samples = shape.samples == 0 ? 1u : shape.samples;
      // from_swapchain / clear_* 由设备帧在提交时按真实目标重算,
      // 这里只负责"按目标形状选对 pass",不预设末态。
      att.from_swapchain = false;
      r = factory->create_shaped_pipeline(*device_, desc, att);
    } else {
      const bool default_shape = shape.color_format == device::Format::RGBA8_UNORM &&
                                 !shape.has_depth && (shape.samples == 0 || shape.samples == 1);
      if (!default_shape) {
        return Status::Unsupported(
            "ShaderLibrary: 没有可用的按形状派生工厂,而目标形状不是默认形状"
            "(samples=" + std::to_string(shape.samples) +
            ") —— 拒绝按单采样建管线(会静默丢失 MSAA/深度)");
      }
      r = device_->create_pipeline(desc);
    }
    if (!r.ok()) {
      return Status(r.status().code, "ShaderLibrary(" + tmpl->name + "): " + r.message());
    }
    std::unique_ptr<IPipeline> owned = std::move(r.value());
    IPipeline* raw = owned.get();
    if (owned) owned_.push_back(std::move(owned));

    PipelineEntry e;
    e.key = key;
    e.raster = raster;
    e.shape = shape;
    e.pipeline = raw;
    e.hash = pk;
    const std::size_t idx = entries_.size();
    entries_.push_back(e);
    by_key_.emplace(pk, idx);
    ++create_count_;
    return raw;
  }

  std::size_t pipeline_count() const override { return entries_.size(); }
  std::uint64_t pipeline_create_count() const override { return create_count_; }

  void clear_pipelines() override {
    owned_.clear();
    entries_.clear();
    by_key_.clear();
  }

 private:
  const ShaderTemplateDesc* find(const ShaderKey& key) const {
    if (!key.valid()) return nullptr;
    auto it = index_.find(key.name_hash);
    if (it == index_.end()) return nullptr;
    return &templates_[it->second];
  }

  device::IRenderDevice* device_ = nullptr;
  std::vector<ShaderTemplateDesc> templates_;
  std::unordered_map<std::uint32_t, std::size_t> index_;   // name_hash → templates_ 下标

  std::vector<PipelineEntry> entries_;
  std::unordered_multimap<std::uint64_t, std::size_t> by_key_;
  std::vector<std::unique_ptr<IPipeline>> owned_;
  std::uint64_t create_count_ = 0;
};

}  // namespace

// ---------------------------------------------------------------- 工厂

std::unique_ptr<IShaderLibrary> create_shader_library(device::IRenderDevice* device) {
  // 注意:这里**不自动登记**任何模板。SPIR-V 的内嵌与登记由上层
  // (embedded_shaders.cpp / CLI / 测试)按其实际拥有的字节码决定,
  // 避免库里硬编码一份"假装有 shader"的空表。
  return std::make_unique<ShaderLibrary>(device);
}

}  // namespace magicblocks::primitives
