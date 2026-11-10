// MagicBlocks L4 —— 魔法叠叠乐：IBlockSet（真源）
//
// ============================ 真源 vs 派生缓存 ============================
// `IBlockSet` 是**唯一真源**：一块方块就是一条 `Block` 记录，按插入顺序连续存放。
// 派生缓存（`occupied` 索引）可以由真源随时完全重建，不参与哈希、不参与输出顺序。
//
// 为什么用线性数组 + 派生 `unordered_map`，而不是 ECS/空间树（设计 §4.3）：
//   1. 真源必须可**逐字节序列化**以做哈希（`hash()`），线性 `std::vector<Block>`
//      是最直接的形态；
//   2. 本玩法只有两种查询："这个格有没有方块"与"这一笔有哪些方块"。
//      前者用 `unordered_map<Cell, index>` 做**派生缓存**；后者靠 `stroke_id`
//      线性扫描（10k 方块 < 0.1 ms）；
//   3. **绝不引入 unordered_map 遍历序做输出顺序**：所有输出遍历一律走真源数组的
//      下标序。这条是确定性铁律，破坏了 golden 就不再可比。
//
// ============================ 不变量 ============================
//   I1: (cell, stroke_id) 唯一 —— 同一格里不可能有两块 Block（由 §6.4 去重判据保证）
//   I2: stroke_id 单调递增且永不复用；撤销一笔不回收它的 id，重做时用新的 id
//   I3: Block.flags 的未定义位必须为 0；cell.y ∈ [0, layers)
//
// 三条不变量在这里**显式校验并报错**，不做 clamp、不做静默修正（D-VX-9）。
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

#include "magicblocks/features/stack/stack_types.h"

namespace magicblocks::features::stack {

// GridSpec 合法性检查（非法即报错，不裁剪）。设计 §5.1。
StackStatus validate_grid(const GridSpec& grid);

class IBlockSet {
 public:
  virtual ~IBlockSet() = default;

  // ---- 写入（全部返回新块的稳定下标；失败返回错误而非"尽力而为"）----

  // 追加一块。违反 I1（格已被占）/ I3（flags 未定义位、y 越界）→ 显式报错，
  // 不覆盖、不跳过、不 clamp。
  virtual StackResult<std::size_t> add(const Block& b) = 0;

  // 整笔撤销：按 stroke_id 移除该笔的全部方块。
  //
  // 撤销不存在的 stroke_id → UnknownStroke，**不是**空操作（设计 §5.3）。
  virtual StackStatus remove_stroke(std::uint32_t stroke_id) = 0;

  // 整笔撤销，并把被移除的 Block 按**真源下标序**写进 out（供重做用）。
  virtual StackStatus undo_stroke(std::uint32_t stroke_id, std::vector<Block>* out) = 0;

  // ---- 查询 ----

  virtual std::size_t size() const = 0;
  virtual const Block* data() const = 0;
  // 存在返回指针，否则 nullptr。
  virtual const Block* find(Cell c) const = 0;
  // 按真源下标取（越界返回 nullptr）。
  virtual const Block* at(std::size_t index) const = 0;
  // 已分配过的最大 stroke_id（0 = 从未分配）。
  virtual std::uint32_t max_stroke_id() const = 0;
  // 当前真源里出现过的**不同** stroke_id 个数（撤销后不再计入）。
  virtual std::size_t stroke_count() const = 0;
  // 下一次分配会得到的 stroke_id（单调递增，永不复用）。
  virtual std::uint32_t next_stroke_id() const = 0;

  // 把当前真源里出现过的 stroke_id 按**升序**写进 out（确定性：不是哈希容器遍历序）。
  virtual void strokes_in_order(std::vector<std::uint32_t>* out) const = 0;

  // 某一笔的全部方块下标，按真源下标序。
  virtual void indices_of_stroke(std::uint32_t stroke_id, std::vector<std::size_t>* out) const = 0;

  // ---- 改色（只改 color，不许动 cell/stroke_id/flags）----

  // 下标越界 → 显式错误（不做静默忽略）。用 OutOfGrid 的前提是本玩法只有这一
  // 类"位置/下标超出合法域"的错误码；detail 里写明是下标越界而不是格越界。
  virtual StackStatus set_color(std::size_t index, std::uint32_t rgba) = 0;

  // ---- 确定性哈希 ----

  // 口径（写死，设计 §5.2）：按真源数组**下标序**逐字节喂
  // (cell.x, cell.y, cell.z, color, stroke_id, flags)。
  // **不**包含 unordered_map 内容、不包含未初始化填充位（static_assert 保证 24B 无 padding）。
  virtual std::uint64_t hash() const = 0;

  // 校验全部不变量（I1/I3），返回第一条违反的人话描述。空 = 全部成立。
  // 这不是渲染路径的一部分，只给测试与 --dry-run 自检用。
  virtual StackStatus validate() const = 0;
};

// 构造一个绑定到给定 GridSpec 的空真源。
//
// `grid` 非法 → 返回错误（不返回一个"参数被夹过"的集合）。
StackResult<std::unique_ptr<IBlockSet>> create_block_set(const GridSpec& grid);

// 便捷：一次落一整笔。
//
// `cells` 按调用方给出的顺序逐格插入（顺序决定真源下标序 → 决定哈希与实例序，
// 所以这里的顺序必须是**可复算**的，不能来自哈希容器）。
// 任一格失败 → **整笔回滚**（把本调用已插入的块全部撤掉），要么整笔成、要么整笔不成。
StackStatus add_stroke(IBlockSet* set, const std::vector<Cell>& cells, std::uint32_t rgba,
                       std::uint32_t stroke_id);

}  // namespace magicblocks::features::stack
