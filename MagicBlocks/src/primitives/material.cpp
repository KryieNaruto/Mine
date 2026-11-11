// MagicBlocks SDK —— L3 材质表实现
//
// 关键点(见 material.h 顶部的设计结论):
//   - 材质是**值**,表只做 desc → 槽位 的去重;
//   - 槽位不回收,保证命令流按槽位回放可复现;
//   - 稳态路径(已登记过的材质)只有一次哈希查找 + 一次数组下标,不分配内存。
#include <cstring>
#include <type_traits>
#include <unordered_map>
#include <vector>

#include "mb_internal/magicblocks/primitives/material.h"

namespace magicblocks::primitives {

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
inline void mix_bits(std::uint64_t* h, const T& v) {
  std::uint64_t raw = 0;
  std::memcpy(&raw, &v, sizeof(T));
  // -0.0 与 0.0 规范化成同一个 bit 模式(spec §4.4:材质缓存须与浮点正负零无关)。
  if constexpr (sizeof(T) == 4 && std::is_floating_point_v<T>) {
    if (raw == 0x80000000ull) raw = 0;
  } else if constexpr (sizeof(T) == 8 && std::is_floating_point_v<T>) {
    if (raw == 0x8000000000000000ull) raw = 0;
  }
  *h ^= raw + 0x9e3779b97f4a7c15ull + (*h << 6) + (*h >> 2);
}

// 管线键:与 shader_library.cpp 的 hash_pipeline_key 必须逐字段一致(两处共用语义)。
// 这里只算 ShaderKey + RasterState;TargetShape 由渲染器在派生管线时再叠加,
// 因为材质本身不关心附件形状(spec §9.4 规则 1)。
std::uint64_t hash_pipeline_key_material(const ShaderKey& key, const RasterStateL3& raster) {
  std::uint64_t h = 0xcbf29ce484222325ull;
  mix_bits(&h, key.name_hash);
  mix_bits(&h, key.flags);
  mix_bits(&h, static_cast<std::uint32_t>(raster.cull));
  mix_bits(&h, static_cast<std::uint32_t>(raster.primitive));
  mix_bits(&h, static_cast<std::uint32_t>(raster.depth_test ? 1u : 0u));
  mix_bits(&h, static_cast<std::uint32_t>(raster.depth_write ? 1u : 0u));
  mix_bits(&h, static_cast<std::uint32_t>(raster.blend ? 1u : 0u));
  return h;
}

class Material final : public IMaterial {
 public:
  Material(MaterialSlot slot, const MaterialDesc& desc)
      : slot_(slot), desc_(desc), key_(hash_material_desc(desc)),
        pipeline_key_(hash_pipeline_key_material(desc.shader, desc.raster)) {}

  MaterialSlot slot() const override { return slot_; }
  MaterialKey key() const override { return key_; }
  const MaterialDesc& desc() const override { return desc_; }
  u64 pipeline_key() const override { return pipeline_key_; }

 private:
  MaterialSlot slot_;
  MaterialDesc desc_;
  MaterialKey key_;
  u64 pipeline_key_;
};

class MaterialTable final : public IMaterialTable {
 public:
  MaterialTable() {
    // 槽位 0 恒为默认材质(spec §9.4 规则 5)。
    const MaterialDesc d = default_material_desc();
    add(d);
  }

  Result<MaterialSlot> intern(const MaterialDesc& desc) override {
    // 快路径:哈希命中 + 全字段比较(哈希碰撞不能靠哈希本身判定相等)。
    const MaterialKey k = hash_material_desc(desc);
    auto range = by_key_.equal_range(k);
    for (auto it = range.first; it != range.second; ++it) {
      if (materials_[it->second]->desc() == desc) {
        ++dedup_hits_;
        return materials_[it->second]->slot();
      }
    }
    return add(desc);
  }

  const IMaterial* material(MaterialSlot slot) const override {
    if (slot >= materials_.size()) return nullptr;
    return materials_[slot].get();
  }

  const MaterialDesc* desc_of(MaterialSlot slot) const override {
    const IMaterial* m = material(slot);
    return m == nullptr ? nullptr : &m->desc();
  }

  std::size_t size() const override { return materials_.size(); }
  u64 dedup_hits() const override { return dedup_hits_; }

  std::vector<MaterialDesc> snapshot() const override {
    std::vector<MaterialDesc> out;
    out.reserve(materials_.size());
    for (const auto& m : materials_) out.push_back(m->desc());
    return out;
  }

  Status restore(const std::vector<MaterialDesc>& slots) override {
    if (slots.empty()) return Status::Invalid("MaterialTable: 快照不能为空(至少要含默认材质)");
    if (slots[0] != default_material_desc()) {
      return Status::Invalid("MaterialTable: 槽位 0 必须是默认材质");
    }
    // 前缀必须与现有槽位逐项相同:槽位一旦分配就不可变更(否则历史命令流会错位)。
    const std::size_t keep = materials_.size() < slots.size() ? materials_.size() : slots.size();
    for (std::size_t i = 0; i < keep; ++i) {
      if (!(materials_[i]->desc() == slots[i])) {
        return Status::Invalid("MaterialTable: 快照与现有槽位在前缀上不一致(槽位不可重排)");
      }
    }
    // 只在尾部追加(幂等:已有则去重命中)。
    for (std::size_t i = keep; i < slots.size(); ++i) {
      Result<MaterialSlot> s = intern(slots[i]);
      if (!s.ok()) return s.status();
      if (s.value() != i) {
        return Status::Invalid("MaterialTable: 快照内部有重复材质(槽位顺序不成立)");
      }
    }
    return Status::Ok_();
  }

 private:
  Result<MaterialSlot> add(const MaterialDesc& desc) {
    const MaterialSlot slot = static_cast<MaterialSlot>(materials_.size());
    auto m = std::make_unique<Material>(slot, desc);
    const MaterialKey k = m->key();
    materials_.push_back(std::move(m));
    by_key_.emplace(k, slot);
    return slot;
  }

  std::vector<std::unique_ptr<Material>> materials_;
  std::unordered_multimap<MaterialKey, std::size_t> by_key_;
  u64 dedup_hits_ = 0;
};

}  // namespace

MaterialKey hash_material_desc(const MaterialDesc& desc) {
  std::uint64_t h = 0xcbf29ce484222325ull;
  mix_bits(&h, desc.shader.name_hash);
  mix_bits(&h, desc.shader.flags);
  for (int i = 0; i < 4; ++i) mix_bits(&h, desc.params.base_color[i]);
  mix_bits(&h, desc.params.roughness);
  mix_bits(&h, desc.params.metallic);
  mix_bits(&h, desc.params.emissive);
  mix_bits(&h, desc.params.color_slot);
  mix_bits(&h, static_cast<std::uint32_t>(desc.raster.cull));
  mix_bits(&h, static_cast<std::uint32_t>(desc.raster.primitive));
  mix_bits(&h, static_cast<std::uint32_t>(desc.raster.depth_test ? 1u : 0u));
  mix_bits(&h, static_cast<std::uint32_t>(desc.raster.depth_write ? 1u : 0u));
  mix_bits(&h, static_cast<std::uint32_t>(desc.raster.blend ? 1u : 0u));
  mix_bits(&h, static_cast<std::uint32_t>(desc.transparent ? 1u : 0u));
  // 保留位:任何字段遗漏都会让两个不同材质撞键,因此这里用 fnv1a64 收尾而不是仅靠位混合。
  return static_cast<MaterialKey>(fnv1a64(&h, sizeof(h), 0xcbf29ce484222325ull));
}

MaterialDesc default_material_desc() {
  MaterialDesc d;
  d.shader = make_shader_key(shader_names::kStandardUnlit, kShaderFlagVertexColor);
  d.params = StandardParams{};
  d.raster = RasterStateL3{};
  d.transparent = false;
  return d;
}

std::unique_ptr<IMaterialTable> create_material_table() { return std::make_unique<MaterialTable>(); }

}  // namespace magicblocks::primitives
