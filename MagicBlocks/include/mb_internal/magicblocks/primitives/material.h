// MagicBlocks SDK —— L3 `IMaterial` / `IMaterialTable`:材质与 Shader 的联动(spec §9,用户要求 U5)
//
// ============================ 设计结论(先读这段) ============================
//
// L1 只有 push constant(R4,无描述符集),移动端上限 128 字节。材质的参数如果
// 走 push constant,那么"换一个颜色"= 一次 push_constants + 一次 draw,
// 材质数量直接等于 drawcall 数量 —— 在积木/叠叠乐这种"几百个不同颜色块"的场景
// 里直接崩掉帧率。
//
// 因此本设计的分配是(**性能第一**:U0'):
//
//   参数种类              | 放哪                       | 切换代价
//   ----------------------|----------------------------|------------------
//   view_proj / 视口       | push constant(96B)         | 每帧 1 次(或每 pass 1 次)
//   材质颜色/粗糙度/金属度  | **逐实例属性**(StandardParams) | **0**(同一 draw 里不同实例)
//   shader 模板(光照模型)   | ShaderKey → 管线            | 1 次 bind_pipeline
//   深度/混合/剔除         | RasterStateL3(独立于材质)    | 1 次 bind_pipeline
//
// 由此得到两条**硬规则**(spec §9.4,不要凭直觉改):
//   规则 A:**同 ShaderKey + 同 RasterState 不同材质参数 → 合并成一个 draw**。
//   规则 B:**透明与不透明用不同 ShaderKey**(kShaderFlagAlphaBlend 进 key),
//           这样合批排序键天然把它们分开,不需要在渲染器里写 if(transparent)。
//
// 材质 = 一个**值**(MaterialDesc),不是对象;`IMaterialTable::intern()` 把它
// 换成一个稳定的槽位号(MaterialSlot),这个槽位进入命令流,回放时按表还原。
// ===========================================================================
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "mb_internal/magicblocks/device/result.h"
#include "mb_internal/magicblocks/primitives/shader_library.h"
#include "mb_internal/magicblocks/primitives/types.h"

namespace magicblocks::primitives {

// ---------------------------------------------------------------- 参数(spec §9.2)

// 标准参数集:**逐实例**上传(32 字节,对齐到 16)。
// 字段顺序与 shader 的 layout(location = 7/8) 逐字段对应,不要随意重排。
struct StandardParams {
  float base_color[4] = {1.0f, 1.0f, 1.0f, 1.0f};   // 白
  float roughness = 0.5f;
  float metallic = 0.0f;
  float emissive = 0.0f;
  std::uint32_t color_slot = 0;   // 256 色表槽位(0 = 直接用 base_color)

  bool operator==(const StandardParams& o) const {
    for (int i = 0; i < 4; ++i) {
      if (base_color[i] != o.base_color[i]) return false;
    }
    return roughness == o.roughness && metallic == o.metallic && emissive == o.emissive &&
           color_slot == o.color_slot;
  }
  bool operator!=(const StandardParams& o) const { return !(*this == o); }
};

// 材质描述(**规范化**后参与哈希:浮点按 bit_cast 比较,-0.0 与 0.0 视为相等,
// NaN 视为不相等 —— 命令流/去重都靠它,spec §4.4 材质缓存)。
struct MaterialDesc {
  ShaderKey shader{};                 // shader 模板选择(名字 + flags)
  StandardParams params{};
  RasterStateL3 raster{};             // 固定功能状态(不进 key,进 pipeline 派生)

  // 是否夹在"透明批次"里。注意:**不改 ShaderKey** ——
  // 规范做法是调用方把 kShaderFlagAlphaBlend 放进 shader.flags(规则 B),
  // 这里只记录语义,供渲染器/测试查询。
  bool transparent = false;

  bool operator==(const MaterialDesc& o) const {
    return shader == o.shader && params == o.params && raster == o.raster &&
           transparent == o.transparent;
  }
  bool operator!=(const MaterialDesc& o) const { return !(*this == o); }
};

// 材质键:MaterialDesc 的 64 位稳定哈希(跨进程一致 —— 命名流要按它复现)。
using MaterialKey = std::uint64_t;

// 规范化后的材质哈希。浮点逐字段 bit_cast,再 FNV-1a 链式混合。
MaterialKey hash_material_desc(const MaterialDesc& desc);

// ---------------------------------------------------------------- 材质对象

// 一个不可变的材质(值语义的对象化包装)。`IMaterialTable` 为每个**唯一 desc**
// 造一个,并给一个稳定槽位。
class IMaterial {
 public:
  virtual ~IMaterial() = default;

  virtual MaterialSlot slot() const = 0;
  virtual MaterialKey key() const = 0;
  virtual const MaterialDesc& desc() const = 0;
  // 该材质对应的管线键(派生自 shader + raster),合批排序的第一关键字。
  virtual u64 pipeline_key() const = 0;
};

// ---------------------------------------------------------------- 材质表

// 键 → 槽位。**槽位一旦分配就不回收**(spec §9.4 规则 5):
// 命令流里存的是槽位号,回收会让"同一个槽位号在不同帧指向不同材质",
// 回放就不再可复现。
class IMaterialTable {
 public:
  IMaterialTable(const IMaterialTable&) = delete;
  IMaterialTable& operator=(const IMaterialTable&) = delete;
  virtual ~IMaterialTable() = default;

  // 登记一个材质:相同 desc → **同一个槽位**(去重,返回已有槽位)。
  // 槽位 0 恒为默认材质(白、Unlit、不透明),在工厂里预置。
  virtual Result<MaterialSlot> intern(const MaterialDesc& desc) = 0;

  // 按槽位取。槽位不存在 → nullptr(不抛)。
  virtual const IMaterial* material(MaterialSlot slot) const = 0;
  virtual const MaterialDesc* desc_of(MaterialSlot slot) const = 0;

  virtual std::size_t size() const = 0;
  // 去重命中次数(性能断言:重复 intern 不新增槽位)。
  virtual u64 dedup_hits() const = 0;

  // 快照(命令流):槽位 0..size-1 按序的 desc。**顺序即槽位**,
  // 因此 restore 要求快照前缀与现有槽位一致(不支持删改历史槽位)。
  virtual std::vector<MaterialDesc> snapshot() const = 0;
  virtual Status restore(const std::vector<MaterialDesc>& slots) = 0;

 protected:
  IMaterialTable() = default;
};

// 默认材质(槽位 0)。
MaterialDesc default_material_desc();

// 工厂:纯 CPU 表,不经 GPU(GPU 侧的实例属性由渲染器按槽位取参数)。
std::unique_ptr<IMaterialTable> create_material_table();

}  // namespace magicblocks::primitives
