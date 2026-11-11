// MagicBlocks L3 —— IWorld 的内部存储结构（archetype SOA 的落地细节）
//
// 只在 src/primitives/world.cpp 里 include。与公开头 world.h 拆开，是因为
// Archetype 的列数组布局（裸字节 + 位序）是实现细节：公开头一旦暴露它，
// "换一种列式布局"就成了破坏性改动，而 §4.2 的布局还可能随性能实测调整。
//
// 内存所有权（G4 零裸分配）：
//   - 列数据与 id 数组：`std::vector<std::uint8_t>` / `std::vector<EntityId>`。
//     不用 IObjectPool 的理由是列数组会**变长**，而 IObjectPool 的 acquire/release
//     是固定槽位语义；§8.1 说的是"实体行 / Stroke / GizmoHandle"用池，
//     列缓冲属于"几何变更才分配"那一类（§4.1），由 vector 自带的增长策略处理。
//   - 迁移与同步用的**临时**缓冲：优先从注入的 IFrameArena 借，
//     借不到（没注入）才退回内部 std::vector —— 这是热路径零分配的落点。
#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "mb_internal/magicblocks/primitives/world.h"

namespace magicblocks::primitives {
namespace detail {

// 第 kind 列在一个 mask 里的序号（比它低的置位个数）。O(1)。
// mask 不含 kind 时返回 kMaxComponents —— 调用方据此判"没有这一列"。
inline std::size_t slot_of_in_mask(ComponentMask mask, ComponentKind kind) {
  const ComponentMask bit = component_bit(kind);
  if (kind >= kMaxComponents || (mask & bit) == 0) return kMaxComponents;
  return static_cast<std::size_t>(std::popcount(mask & (bit - 1)));
}

// 一个 archetype = 一种组件集合在某个时刻的全部实体行。
//
// 两条数组严格同长：`ids[r]` 与"每个组件列的第 r 行"指的是同一行。
// 行数只从 `ids.size()` 取 —— 曾经想维护一个独立 count 字段，结果两处
// 更新不同步（swap-remove 时漏改 count），列里留下了上一轮的脏数据。
struct Archetype {
  ComponentMask mask = 0;
  std::vector<EntityId> ids;

  // 按**位序**排列的列：`columns[k]` 对应 mask 的第 k 个置位（从低位到高位）。
  // 每列长度 = row_capacity * column_sizes[k] **字节**，不是元素个数 ——
  // 列里存的是裸字节，只有 `component()` 按调用方给的 T* 解释时才成为元素。
  // 用 vector<uint8_t> 而不是 vector<T>：T 是调用方的类型，世界只有 size_bytes。
  std::vector<std::vector<std::uint8_t>> columns;
  std::vector<u32> column_sizes;

  // 行容量。单独记而不是从 vector::capacity() 推：vector 的容量增长策略只是
  // "至少能装下 size()"，按字节反推会算出半个元素的容量，搬行时容易踩越界。
  std::size_t row_capacity = 0;

  std::size_t size() const { return ids.size(); }
  bool empty() const { return ids.empty(); }

  // 第 k 列第 row 行的字节地址（第 0 行 = 列的基址）。
  void* column_row(std::size_t k, std::size_t row) {
    return columns[k].data() + row * static_cast<std::size_t>(column_sizes[k]);
  }
  const void* column_row(std::size_t k, std::size_t row) const {
    return columns[k].data() + row * static_cast<std::size_t>(column_sizes[k]);
  }

  std::size_t slot_of(ComponentKind kind) const { return slot_of_in_mask(mask, kind); }

  // 保证能放 rows 行。容量按 2 倍增长并至少到 8 行 —— 每次迁移只增删一行，
  // 按行精确扩容就是"每加一个组件搬一次整个 archetype"的平方级灾难。
  //
  // 新扩出来的字节清零：调试期能立刻看出"读到 0 说明这行没写过"，
  // 也让快照在未初始化列上不产生随机差异（命令流哈希必须确定性）。
  void reserve_rows(std::size_t rows) {
    if (rows <= row_capacity) return;
    std::size_t want = row_capacity == 0 ? 8 : row_capacity * 2;
    while (want < rows) want *= 2;
    for (std::size_t k = 0; k < columns.size(); ++k) {
      columns[k].resize(want * static_cast<std::size_t>(column_sizes[k]), std::uint8_t{0});
    }
    row_capacity = want;
  }

  // 在末尾追加一行（返回新行号）。列容量不足时先扩容；
  // **id 槽位立刻写入一个无效值**：调用方随后会把它改成真正的 id，但只要
  // 忘了改，这里就是"无效 id"而不是未初始化内存 —— 后者会让 8 号 index 的
  // 残值看起来像一个合法实体（这条曾经真的让一个用例读到别人的行）。
  //
  // 注意 ids 的 **size** 才是行数：`ids.push_back` 会让 size 恰好多 1，
  // 列本身按 row_capacity 预分配（size 只是"已用行数"），两者不要求同长。
  std::size_t push_row() {
    reserve_rows(ids.size() + 1);
    const std::size_t row = ids.size();
    ids.push_back(EntityId{});
    return row;
  }

  // 把 archetype 收缩到 n 行（despawn 之后）。**不缩容量**：despawn 之后马上
  // spawn 是极常见的序列（实体池化），缩容量会把省下的内存立刻再分配回去。
  void shrink_to(std::size_t n) { ids.resize(n); }

  // 清空所有行但保留容量（clear_entities 用）。
  void clear_rows() { ids.clear(); }
};

}  // namespace detail
}  // namespace magicblocks::primitives
