// MagicBlocks L3 —— 合批器（spec §4.3 / 决策 D-L3-3、D-L3-4）
//
// 职责：把"一堆待画的东西"压成**尽可能少的 drawcall**，并且把结果
// 按确定的顺序排好，让命令流是确定性的（spec §11 回放要求）。
//
// §4.1 的三个预算全在这里兑现：
//   drawcall ≤ 300        —— 相邻同 (pipeline, material, mesh) 合并成一次 instanced draw
//   管线切换 ≤ 16         —— 排序键把 pipeline 放最前，同管线的绘制必然相邻
//   顶点上传 仅脏块        —— 实例数据追加进 IBatchPool 环形缓冲，只传新增部分
//
// 为什么 L3 自带合批器而不是复用 L2 的 ISubmitQueue（决策 D-L3-4）：
//   L2 的 DrawItem 字段（mesh_key/first_index/index_count）表达不了实例数据；
//   L2 的排序键是 (material, depth)，没有 pipeline 与 mesh 键。
//   硬改 L2 会破坏 198 个依赖测试与 l2 golden（D-L3-5 同理）。
//   所以 L3 自己排、自己合，最后**仍以 DrawItem 形式喂 ISubmitQueue**，
//   让既有测试能断言顺序 —— 不绕过 L2，只是不依赖它的排序能力。
//
// 硬约束：本头零 Vk*、不读时钟、不分配（用 IFrameArena）。
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "mb_internal/magicblocks/primitives/alloc.h"
#include "mb_internal/magicblocks/primitives/material.h"
#include "mb_internal/magicblocks/primitives/types.h"

namespace magicblocks::primitives {

using device::u32;
using device::u64;

// 未解析的管线哈希哨兵。
//
// 为什么需要它：IWorld 不持有材质表，算不出真正的管线键，只能填这个哨兵。
// **合批前必须由渲染器改写成本帧真实的 pipeline_hash。**
// 未改写就把这些请求交给 build() 的后果：所有请求被当成同一管线合并，
// `pipeline_switches` 恒为 0 —— 而它是 §4.1 的门控指标（≤16），
// 恒 0 等于指标失效、永远显示完美。拿含哨兵值的请求去断言
// `pipeline_switches`，得到的是**无意义的绿灯**。
//
// 用 0 这个值是因为它是"哈希没填"的自然表示（FNV 等哈希对空输入也可能得 0，
// 但真实管线键总是由 register_pipeline 显式给出，0 被 register_pipeline 拒绝），
// 导出成具名常量是为了让 grep 能找全所有需要改写的地点。
inline constexpr u64 kUnresolvedPipelineHash = 0;

// 一次待提交的绘制输入。由 IWorld 遍历 / IPainter 产出。
//
// 这是一个**扁平值类型**（POD），不是多态对象：
//   合批要对上万个实例排序，任何间接（指针、虚调用）都会把 cache 打散，
//   并且让排序键无法一次性比较完。值语义也天然满足 C6（命令流值语义）。
struct DrawRequest {
  u64 pipeline_hash = 0;      // [0] 主排序键（ShakerKey + 固定功能状态）
  MaterialKey material_key = 0;  // [1]
  MeshId mesh = kInvalidMeshId;   // [2]
  std::uint32_t instance_index = 0;  // 指向实例属性数组的下标
  std::uint32_t entity_id = 0;       // [4] 稳定 tie-break
  std::uint32_t index_count = 0;     // 该 mesh 的索引数（0 = 非索引绘制）
  std::uint32_t first_index = 0;
  std::int32_t vertex_offset = 0;
  std::uint32_t vertex_count = 0;
  bool transparent = false;   // [3] 0 不透明 / 1 透明（D-L3-3：由 ShaderKey 位决定）
  bool visible = true;
};

// 一次实际提交的 draw。合批的产物。
struct BatchDraw {
  u64 pipeline_hash = 0;
  MaterialKey material_key = 0;
  MeshId mesh = kInvalidMeshId;
  std::uint32_t index_count = 0;
  std::uint32_t first_index = 0;
  std::int32_t vertex_offset = 0;
  std::uint32_t vertex_count = 0;
  bool indexed = true;
  bool transparent = false;
  // 这一批的实例在 IBatchPool 里的字节区间。
  std::uint32_t instance_offset = 0;
  std::uint32_t instance_count = 0;
  std::uint32_t first_entity = 0;  // 便于测试断言 tie-break
};

// 合批统计 —— 直接对应 §4.1 的预算，供门控 G9 断言。
struct BatchStats {
  std::uint32_t request_count = 0;   // 输入条数
  std::uint32_t batch_count = 0;     // 输出 drawcall 数（预算 ≤ 300）
  std::uint32_t pipeline_switches = 0;  // 相邻 batch 的 pipeline 变化次数（预算 ≤ 16）
  std::uint32_t merged_requests = 0;    // 被合并进已有 batch 的条数
  std::uint32_t skipped_invisible = 0;
  std::uint32_t instance_bytes = 0;  // 本帧实例属性上传字节
};

// 实例属性：每个实例一条，按 §4.3 排序后连续写入 IBatchPool。
//
// 布局刻意做成 48 字节（3 个 16B 对齐块）：
//   移动端实例属性走 step-rate 顶点缓冲，按 16B 对齐访存最友好；
//   48 不是 2 的幂，但 16B 对齐已足够，且省下来的字节能多装 1/3 的实例。
struct InstanceAttribs {
  float model[16];   // 64 B：世界矩阵（列主序）
  // 材质参数放这里而不是 push constant（spec §9.2 规则 A）：
  //   128B push constant 在移动端是硬上限，放不下每实例的材质。
  float base_color[4];
  float emissive[3];
  float roughness = 0.0f;
  float metallic = 0.0f;
  float _pad[3] = {0.0f, 0.0f, 0.0f};
};

class IBatchBuilder {
 public:
  IBatchBuilder() = default;
  IBatchBuilder(const IBatchBuilder&) = delete;
  IBatchBuilder& operator=(const IBatchBuilder&) = delete;
  virtual ~IBatchBuilder() = default;

  // ---- 帧边界 ----
  virtual void begin_frame(u32 frame_slot) = 0;

  // 递交一条绘制请求。不做分配（容量由 reserve 预热）。
  virtual void push(const DrawRequest& req) = 0;

  // 递交一批请求（更常见的用法，一次拷进内部数组）。
  virtual void push_batch(const DrawRequest* reqs, std::size_t count) = 0;

  // 排序 + 合批。产出 batches() 并按需把实例属性写进 batch pool。
  virtual void build() = 0;

  // ---- 结果 ----
  virtual const std::vector<BatchDraw>& batches() const = 0;
  virtual const BatchStats& stats() const = 0;
  // 本帧合批后的确定性命中值（排序结果的指纹，供回放校验）。
  virtual std::uint64_t order_hash() const = 0;

  // ---- 预热 / 预算 ----
  virtual void reserve(std::size_t max_requests) = 0;
  virtual void set_allow_growth(bool allow) = 0;

  // 依赖注入：arena 用于排序索引，batch pool 用于实例属性。
  // 两者都可以为空（为空时退化为内部 std::vector —— 仅用于单元测试）。
  virtual void set_frame_arena(IFrameArena* arena) = 0;
  virtual void set_batch_pool(IBatchPool* pool) = 0;
  virtual void set_stats(IFrameStats* stats) = 0;

  // 实例属性上传回调：合批器算好每批的实例区间后，由这个函数把数据写进
  // IBatchPool 并回填 instance_offset。为空则只统计字节数。
  virtual void set_instance_writer(
      bool (*write)(void* user, const InstanceAttribs* data, u32 count, u32* out_offset),
      void* user) = 0;
};

std::unique_ptr<IBatchBuilder> create_batch_builder();

}  // namespace magicblocks::primitives
