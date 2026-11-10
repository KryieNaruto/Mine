// MagicBlocks L4 —— 魔法叠叠乐：笔刷链路（射线 → 层平面 → 吸附 → 扫掠 → 去重）
//
// ============================ 一条笔迹怎么变成柱 ============================
//   Down(px,py) ──screen_to_ray──▶ Ray ──▶ 层平面求交 ──▶ 量化+吸附 ──▶ Cell(c0)
//   Move/Up(p)  ──────────────── 同样的三步 ─────────────▶ Cell(ck)
//   c0..ck 逐段 supercover DDA ──▶ 每格 emit 一次 ──▶ 去重（occupied）──▶ 插入 Block
//
// 为什么每个采样点都要重新投一遍射线、而不是在平面内做 2D 插值（设计 §6）：
// 相机可能在拖动期间被动过（哪怕只是小抖动），在平面 2D 插值会用错的相机参数；
// 而"像素 → 射线 → 层平面"这条链**只用当前相机**，天然对相机变化一致。
//
// ============================ 三条确定性纪律 ============================
//   1. **吸附是唯一的量化点**（§6.2）。世界坐标由 `origin + cell * cell_size` 单向派生，
//      永不回写；吸附前把交点按 `quantum = 1e-4 * cell_size` 量化，让"恰好落线"
//      的点稳定落在同一侧 —— 这是确定性要求，不是容差，所以输入在格线上时
//      结果**仍然是那个整数格**，不会漂到邻居。
//   2. **扫掠在整数空间做，且必须是 supercover**（§6.3）。只取端点或只穿格心会
//      在斜线上漏格 —— 漏格在画面上就是"柱子断了一截"，而且不报任何错。
//   3. **去重判据是整数格相等**（§6.4），不是"距离小于某阈值"。因此 I1 唯一性
//      由构造保证，不依赖插入顺序；且去重**不跨层**（同 (x,z) 不同 y 是两块）。
//
// ============================ 层锁 ============================
// F-VX-6：落笔时把 cell.y **定格**在 locked_layer，相机俯仰变化不再影响它。
// 状态机：unlocked ──lock(layer)──▶ locked ──unlock()──▶ unlocked。
// `lock()` 必须显式给 layer；不给 → LockLayerRequired（**不**默认锁第 0 层，
// 那会让"忘了设"表现为"怎么刷都刷在最底层"这种极难归因的现象）。
#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "magicblocks/features/stack/block_set.h"
#include "magicblocks/features/stack/stack_types.h"
#include "magicblocks/primitives/types.h"

namespace magicblocks::features::stack {

using primitives::Ray;
using primitives::Vec3;

// 一次落笔的统计。`skipped_occupied` **不进**哈希（设计 §6.4），只给人看。
struct StrokeStats {
  std::uint32_t sampled = 0;          // 尝试吸附的采样点数
  std::uint32_t cells_emitted = 0;    // DDA emit 出的格数（含被去重拦下的）
  std::uint32_t cells_added = 0;      // 真源实际新增的格数
  std::uint32_t skipped_occupied = 0; // 命中已占格而跳过的次数
};

// 一次落笔的结果（纯数据，可直接喂 IBlockSet）。
struct StrokePlan {
  std::vector<Cell> cells;  // 按插入顺序；**不重复**、**不含已占格**
  StrokeStats stats;
  // 层锁在这一笔生效时的层号（未锁定 = -1）。CLI 打印用。
  std::int32_t locked_layer = -1;
};

// 笔刷配置。
struct BrushConfig {
  GridSpec grid{};
  // 一根柱的高度（层数）。1 = 单层（会走到 instance_count==1 的边界，见阶段 3）。
  std::int32_t column_height = 1;
};

// 层锁控制器（独立小状态机，可脱离笔刷单测）。
class LayerLock {
 public:
  explicit LayerLock(const GridSpec& grid) : grid_(grid) {}

  // 锁定到某层。layer 越界 → LayerOutOfRange（**不**钳到合法范围）。
  StackStatus lock(std::int32_t layer);
  StackStatus unlock();
  bool locked() const { return locked_; }
  std::int32_t layer() const { return locked_ ? layer_ : -1; }

 private:
  GridSpec grid_{};
  bool locked_ = false;
  std::int32_t layer_ = 0;
};

// 笔刷：一次落笔的完整链路。
//
// 生命期纪律：`begin()` 之后才能 `sample()`，`end()` 产出 StrokePlan。
// 这是刻意的两段式 —— "一次落笔只产生一份 plan、只改一次真源"（设计 §7 关键判据）。
class IBlockBrush {
 public:
  virtual ~IBlockBrush() = default;

  // 开始一次落笔。会把内部采样缓存清空。
  virtual void begin() = 0;

  // 投一个像素采样点。
  //   - 射线平行层平面 → RayParallelToLayer
  //   - 交点在相机背后 / 层平面在背后 → RayMissesLayer
  //   - 吸附后越出 half_extent → OutOfGrid（**不** clamp）
  // 任一失败即**整笔失败**：调用方拿到错误就丢弃本次落笔，不得"跳过这一格"。
  virtual StackStatus sample(const Ray& ray) = 0;

  // 结束落笔，产出 plan。
  // 没有任何有效格 → EmptyStroke。
  virtual StackResult<StrokePlan> end() const = 0;

  // 便捷：一次性投完一串射线（等价 begin + N 次 sample + end）。
  virtual StackResult<StrokePlan> stroke(const Ray* rays, std::size_t count) = 0;

  // 当前层锁（笔刷持有同一份，便于在同一处定格/查询）。
  virtual LayerLock& layer_lock() = 0;
  virtual const LayerLock& layer_lock() const = 0;

  // 把 plan 落成块：每格从底部向上摞 `column_height` 块。
  // 整笔原子（§7）：任何一格失败 → 一块都不留。
  virtual StackStatus commit(IBlockSet* set, const StrokePlan& plan, std::uint32_t rgba,
                             std::uint32_t stroke_id) const = 0;

  // 一步到位：投一串射线 → 出 plan → 落成块。返回本次落笔的 plan（含统计）。
  virtual StackResult<StrokePlan> stroke_and_commit(IBlockSet* set, const Ray* rays,
                                                    std::size_t count, std::uint32_t rgba,
                                                    std::uint32_t stroke_id) = 0;
};

std::unique_ptr<IBlockBrush> create_block_brush(const BrushConfig& cfg);

// ---------------------------------------------------------------- 纯函数（可独立单测）

// 射线 → 层平面 → 量化 → 吸附 → Cell。设计 §6.1 + §6.2。
//
// `layer_y` = 目标层号；层平面的世界高度 = `origin[1] + layer_y * cell_size`。
// 注意：**层锁未启用时**由调用方先算出"落在第几层"，再调本函数；
// 层锁启用时直接传 locked_layer。
StackResult<Cell> snap_ray_to_cell(const Ray& ray, const GridSpec& grid, std::int32_t layer_y);

// 射线 → 层平面交点（不做吸附），供需要"精确落点"的调用方用。
StackResult<Vec3> ray_to_layer_point(const Ray& ray, const GridSpec& grid, std::int32_t layer_y);

// 未锁层时，用射线自动决定落在哪一层：取射线进入网格体积时的第一层。
//
// 规则（可复算）：把射线参数 t=0 处 y 值映射到层号并夹进 [0, layers)；
// 射线朝下穿过多层时取**最上**的一层（"从顶上往下刷，刷在顶面"）。
StackResult<std::int32_t> pick_layer_for_ray(const Ray& ray, const GridSpec& grid);

// 3D supercover DDA：把 a → b 的线段经过的**每一个**格按行进顺序写进 out（含 a、含 b）。
//
// 纯整数运算，无除法误差；"只被线段擦过一角"的格也会 emit。
void supercover_cells(Cell a, Cell b, std::vector<Cell>* out);

}  // namespace magicblocks::features::stack
