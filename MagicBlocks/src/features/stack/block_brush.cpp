// MagicBlocks L4 —— 魔法叠叠乐：笔刷链路实现
//
// 算法逐条对应 block_brush.h 顶部的三条纪律，且**每一步都能单独测**：
//   snap_ray_to_cell / pick_layer_for_ray / supercover_cells 都是纯函数。
// 这样"斜线漏格"这类问题可以在 DDA 层面直接定位，而不必走完整条渲染链。
#include "mb_internal/magicblocks/features/stack/block_brush.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>

namespace magicblocks::features::stack {

namespace {

// 吸附前的量化步长（设计 §6.2）：`quantum = kQuantizeEps * cell_size`。
//
// 为什么必须量化而不是直接 floor：`floor` 对"恰好落在格线上"的浮点值极其敏感 ——
// 交点的浮点误差方向取决于驱动/编译器，同一个像素在两次运行里可能落到格的
// 两侧，golden 就不可复现了。把交点先量化到 1e-4 格，让"恰好落线"的点稳定
// 落在同一侧。**这不是容差**：量化之后仍用整数 floor 吸附，整数格是唯一真值。
constexpr float kQuantizeEps = 1e-4f;

// 判断射线"平行层平面"的阈值。取 |dir.y| 相对 |dir| 的比值：
// 设计 §6.1 要求 n·dir == 0 报 RayParallelToLayer。用相对阈值而不是绝对阈值，
// 否则世界尺度一变（cell_size 变小）判定就漂。
constexpr float kParallelEps = 1e-6f;

StackError err(StackCode c, std::string d) { return StackError::Fail(c, std::move(d)); }

std::string cell_str(const Cell& c) {
  return "(" + std::to_string(c.x) + "," + std::to_string(c.y) + "," + std::to_string(c.z) + ")";
}

}  // namespace

// ---------------------------------------------------------------- 层锁

StackStatus LayerLock::lock(std::int32_t layer) {
  // 越界**不**钳到合法范围（设计 §6.5）：钳位会让调用方以为自己的参数生效了。
  if (layer < 0 || layer >= grid_.layers) {
    return StackStatus::Fail(StackCode::LayerOutOfRange,
                             "lock(" + std::to_string(layer) + ") 越界：合法层号 [0, " +
                                 std::to_string(grid_.layers) + ")");
  }
  locked_ = true;
  layer_ = layer;
  return StackStatus::Ok_();
}

StackStatus LayerLock::unlock() {
  // 本来就未锁 → 空操作是合法的（unlock 是幂等的"恢复自由"语义）。
  locked_ = false;
  layer_ = 0;
  return StackStatus::Ok_();
}

// ---------------------------------------------------------------- 射线 → 层平面点

StackResult<Vec3> ray_to_layer_point(const Ray& ray, const GridSpec& grid, std::int32_t layer_y) {
  if (layer_y < 0 || layer_y >= grid.layers) {
    return err(StackCode::OutOfGrid, "层号 " + std::to_string(layer_y) + " 越出 [0, " +
                                         std::to_string(grid.layers) + ")");
  }
  const Vec3& o = ray.origin;
  const Vec3& d = ray.direction;
  // 平面 n=(0,1,0)、平面上一点 p0 = (0, plane_y, 0) ⇒ y = plane_y。
  const float plane_y = grid.origin[1] + static_cast<float>(layer_y) * grid.cell_size;

  const float dlen = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
  if (!(dlen > 0.0f)) {
    return err(StackCode::RayParallelToLayer, "射线方向为零向量，无法与层平面求交");
  }
  if (std::fabs(d.y) <= kParallelEps * dlen) {
    return err(StackCode::RayParallelToLayer,
               "射线平行于层平面 y=" + std::to_string(plane_y) + "（dir.y≈0）");
  }

  // t = (plane_y - o.y) / d.y。t < 0 表示交点在射线**起点之后的反方向**
  // —— 对屏幕射线就是"点在相机背后"，必须报错而不是取绝对值。
  const float t = (plane_y - o.y) / d.y;
  if (!(t >= 0.0f)) {
    return err(StackCode::RayMissesLayer,
               "层平面 y=" + std::to_string(plane_y) + " 的交点在相机背后 (t=" +
                   std::to_string(t) + ")");
  }

  Vec3 p;
  p.x = o.x + d.x * t;
  p.y = plane_y;  // 用平面高度，不用 o.y + d.y*t：后者带舍入误差，会让"层内点"不在层上
  p.z = o.z + d.z * t;
  return p;
}

// ---------------------------------------------------------------- 吸附

StackResult<Cell> snap_ray_to_cell(const Ray& ray, const GridSpec& grid, std::int32_t layer_y) {
  auto pt = ray_to_layer_point(ray, grid, layer_y);
  if (!pt.ok()) return pt.error();
  const Vec3 p = pt.value();

  if (!(grid.cell_size > 0.0f)) {
    return err(StackCode::OutOfGrid, "GridSpec.cell_size 必须 > 0");
  }
  const float cs = grid.cell_size;
  const float quant = kQuantizeEps * cs;

  auto axis_cell = [&](float world, float origin) -> std::int32_t {
    // 1) 先量化到网格：p = round(p / quant) * quant。
    // 2) 再 floor((p - origin) / cs)。
    // 量化步长取 1e-4 格，比 float 有效位远小，所以它对"不在格线上"的点毫无影响；
    // 只对"数值上落在格线旁边 1e-5 格以内"的抖动起稳定作用。
    const float q = std::round(world / quant) * quant;
    const float rel = (q - origin) / cs;
    // floor 而不是 round：格 i 覆盖 [i, i+1) * cs + origin（半开区间），
    // 所以 origin 本身属于格 0，origin + cs 属于格 1。
    return static_cast<std::int32_t>(std::floor(rel));
  };

  Cell c;
  c.x = axis_cell(p.x, grid.origin[0]);
  c.y = layer_y;
  c.z = axis_cell(p.z, grid.origin[2]);

  // 越出网格 → 显式报错，**不** clamp（clamp 会让笔迹贴边变成"墙上一条黑边"）。
  if (c.x < -grid.half_extent || c.x >= grid.half_extent) {
    return err(StackCode::OutOfGrid, "吸附格 " + cell_str(c) + " 的 x 越出 [-" +
                                         std::to_string(grid.half_extent) + ", " +
                                         std::to_string(grid.half_extent) + ")");
  }
  if (c.z < -grid.half_extent || c.z >= grid.half_extent) {
    return err(StackCode::OutOfGrid, "吸附格 " + cell_str(c) + " 的 z 越出 [-" +
                                         std::to_string(grid.half_extent) + ", " +
                                         std::to_string(grid.half_extent) + ")");
  }
  return c;
}

// ---------------------------------------------------------------- 自动选层

StackResult<std::int32_t> pick_layer_for_ray(const Ray& ray, const GridSpec& grid) {
  if (grid.layers < 1) {
    return err(StackCode::OutOfGrid, "GridSpec.layers 必须 >= 1");
  }
  const Vec3& d = ray.direction;
  const float dlen = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
  if (!(dlen > 0.0f)) {
    return err(StackCode::RayParallelToLayer, "射线方向为零向量");
  }

  // 射线的 y(t) = o.y + d.y * t。网格的层平面范围是 [y0, y_hi]，
  //   y0   = origin[1] + 0 * cs           （第 0 层平面）
  //   y_hi = origin[1] + layers * cs      （第 layers 层平面 = 顶面）
  // 我们取"射线与该体积相交区间里 **y 最大** 的那一层" —— 也就是从上往下刷时
  // 刷在顶面，从下往上刷时刷在最底层。这对应"笔尖先碰到哪里"的直觉。
  const float cs = grid.cell_size;
  const float y0 = grid.origin[1];
  const float y_hi = grid.origin[1] + static_cast<float>(grid.layers) * cs;

  // 射线平行于层平面：y 恒定，直接映射层号（夹进 [0, layers) 是**吸附语义**，
  // 不是错误 —— 水平刷子必然在某一层上。这里必须显式说明，否则和 §6.5 的
  // "越界报错"看起来矛盾：那条管的是 `lock(layer)` 的**参数**，这条是几何落点。
  if (std::fabs(d.y) <= kParallelEps * dlen) {
    const float rel = (ray.origin.y - y0) / cs;
    std::int32_t lay = static_cast<std::int32_t>(std::floor(rel));
    if (lay < 0) lay = 0;
    if (lay > grid.layers - 1) lay = grid.layers - 1;
    return lay;
  }

  // 求射线与 [y0, y_hi] 区间的参数交集 [t1, t2]，并裁到 t >= 0 ——
  // 这就是"射线 vs 网格体积"的 slab 测试在 y 轴上的那一维，只是不区分入/出。
  float ta = (y0 - ray.origin.y) / d.y;
  float tb = (y_hi - ray.origin.y) / d.y;
  if (ta > tb) std::swap(ta, tb);
  const float t1 = std::max(ta, 0.0f);
  const float t2 = tb;
  if (!(t1 <= t2)) {
    return err(StackCode::RayMissesLayer, "射线不与网格的层范围相交（交点在相机背后）");
  }

  // **取"射线最先碰到的那一层"，也就是 t 较小的那一端所在的层。**
  //
  // 这里连续犯过两次方向错误，都值得写下来，因为两次都"看起来对"：
  //   (1) 用 t_enter（贴近相机的那次 slab 穿越）—— 相机在上、网格在下时
  //       那次穿越其实在 y_hi（顶面），碰巧对；但相机在**下方**时同样的代码
  //       就给出最顶层，完全反了。
  //   (2) 改用"y 较大的那端"—— 对从上看是对的，但从下看仍然是最大值，
  //       于是从下往上刷也落在顶层。上下都测才发现它根本不是"最近"语义。
  //
  // 正确判据是**最近**：比较 t1 与 t2，取 t 小的那个交点所在的层。
  // 这才是"笔尖先碰到哪里"，与相机在网格上方还是下方无关。
  const float t_first = std::min(t1, t2);
  const float y_first = ray.origin.y + d.y * t_first;

  // 交点的 y → 层号。夹进合法范围是**几何落点**的边界处理（恰好贴在顶面时
  // 落到最顶层），与 §6.5 "lock() 参数越界报错"是两件事，不要混。
  float rel = (y_first - y0) / cs;
  std::int32_t lay = static_cast<std::int32_t>(std::floor(rel));
  if (lay < 0) lay = 0;
  if (lay > grid.layers - 1) lay = grid.layers - 1;
  return lay;
}

// ---------------------------------------------------------------- supercover DDA

void supercover_cells(Cell a, Cell b, std::vector<Cell>* out) {
  if (out == nullptr) return;
  // 先 emit 起点。**a == b 的段只 emit a 一次**（设计 §6.3 判据）。
  out->push_back(a);
  if (a == b) return;

  // 三个轴上的步进方向与"剩余量"。全整数，无除法误差。
  const std::int32_t dx = (b.x > a.x) ? 1 : ((b.x < a.x) ? -1 : 0);
  const std::int32_t dy = (b.y > a.y) ? 1 : ((b.y < a.y) ? -1 : 0);
  const std::int32_t dz = (b.z > a.z) ? 1 : ((b.z < a.z) ? -1 : 0);

  const std::int32_t adx = std::abs(b.x - a.x);
  const std::int32_t ady = std::abs(b.y - a.y);
  const std::int32_t adz = std::abs(b.z - a.z);

  // Amanatides & Woo 的整数版：每一步选择"下一次穿过轴线平面的时刻最早"的那一轴。
  // 穿过时刻（单位：段长的分数）：
  //   x 轴下一次穿过在 (sx + 0.5) / adx，同理 y、z。
  // 比较两个分数 a_num/a_den 与 b_num/b_den（分母 = 2*ad），交叉相乘避免浮点。
  // 交叉相乘的结果 < 2^31，因为网格半宽 <= 2^15（int32 格坐标足够大的前提）。
  // 时刻**相等**时按 x → y → z 的固定优先级步进 —— 对角线因此只有一条确定的
  // 格路径，两次运行完全一致（确定性要求，不是性能优化）。

  std::int32_t cx = a.x;
  std::int32_t cy = a.y;
  std::int32_t cz = a.z;
  std::int32_t sx = 0;  // 已在该轴上前进的格数
  std::int32_t sy = 0;
  std::int32_t sz = 0;

  // 最多走 adx+ady+adz 步（每一步至少靠近 b 一格）。
  const std::int32_t total = adx + ady + adz;
  for (std::int32_t step = 0; step < total; ++step) {
    // 各轴下一次穿过的时刻（分子/分母形式：(2*s+1) / (2*ad)）。
    const std::int64_t x_num = 2 * sx + 1;
    const std::int64_t y_num = 2 * sy + 1;
    const std::int64_t z_num = 2 * sz + 1;

    bool take_x = false;
    bool take_y = false;
    bool take_z = false;

    if (dx != 0 && sx < adx) {
      // x 最早？
      bool earliest = true;
      if (dy != 0 && sy < ady) {
        // x_num/(2*adx) <= y_num/(2*ady)  ⇔  x_num*ady <= y_num*adx
        if (x_num * ady > y_num * adx) earliest = false;
      }
      if (earliest && dz != 0 && sz < adz) {
        if (x_num * adz > z_num * adx) earliest = false;
      }
      take_x = earliest;
    }
    if (!take_x && dy != 0 && sy < ady) {
      bool earliest = true;
      if (dz != 0 && sz < adz) {
        if (y_num * adz > z_num * ady) earliest = false;
      }
      take_y = earliest;
    }
    if (!take_x && !take_y && dz != 0 && sz < adz) take_z = true;

    // **步进必须乘方向 dx/dy/dz**。
    // 这里曾经写成 `++cx` / `++cy` / `++cz` —— 正向画（dx=+1）完全正常，
    // 反向画（从右往左拖、dx=-1）却会**朝反方向继续走**：
    // supercover_cells({1,7,0},{0,7,0}) 给出 (1,7,0)(2,7,0)，凭空多出一格
    // 而丢失真正的地面路径。表现为"往回涂的时候柱子往旁边长"。
    if (take_x) {
      cx += dx;
      ++sx;
    }
    if (take_y) {
      cy += dy;
      ++sy;
    }
    if (take_z) {
      cz += dz;
      ++sz;
    }
    if (!take_x && !take_y && !take_z) break;  // 理论上到不了；防御性退出

    Cell c;
    c.x = cx;
    c.y = cy;
    c.z = cz;
    // 到达 b 就停（判别式可能让最后一次多走一格的情形不存在，但显式断言更稳）。
    if (c == b) {
      out->push_back(c);
      return;
    }
    out->push_back(c);
  }
}

// ---------------------------------------------------------------- 笔刷

namespace {

class BlockBrush final : public IBlockBrush {
 public:
  explicit BlockBrush(const BrushConfig& cfg) : cfg_(cfg), lock_(cfg.grid) {}

  void begin() override {
    samples_.clear();
    failed_ = false;
  }

  StackStatus sample(const Ray& ray) override {
    if (failed_) {
      return StackStatus::Fail(StackCode::EmptyStroke, "本次落笔已失败，后续采样被拒绝");
    }
    Cell c;
    if (lock_.locked()) {
      // F-VX-6：锁层时 cell.y **恒等于** locked_layer，相机俯仰不参与。
      auto r = snap_ray_to_cell(ray, cfg_.grid, lock_.layer());
      if (!r.ok()) {
        failed_ = true;
        return StackStatus::Fail(r.error_code(), "第 " + std::to_string(samples_.size()) +
                                                     " 个采样点吸附失败: " + r.text());
      }
      c = r.value();
    } else {
      auto lay = pick_layer_for_ray(ray, cfg_.grid);
      if (!lay.ok()) {
        failed_ = true;
        return StackStatus::Fail(lay.error_code(), "第 " + std::to_string(samples_.size()) +
                                                       " 个采样点选层失败: " + lay.text());
      }
      auto r = snap_ray_to_cell(ray, cfg_.grid, lay.value());
      if (!r.ok()) {
        failed_ = true;
        return StackStatus::Fail(r.error_code(), "第 " + std::to_string(samples_.size()) +
                                                     " 个采样点吸附失败: " + r.text());
      }
      c = r.value();
    }

    // 去相邻重复（设计 §6.3 第 1 步）：同一格连投两次不产生空段。
    if (!samples_.empty() && samples_.back() == c) {
      ++sampled_dups_;
      return StackStatus::Ok_();
    }
    samples_.push_back(c);
    return StackStatus::Ok_();
  }

  StackResult<StrokePlan> end() const override {
    if (failed_) {
      return err(StackCode::EmptyStroke, "本次落笔已失败（见上一个采样点的错误），拒绝出 plan");
    }
    if (samples_.empty()) {
      return err(StackCode::EmptyStroke, "笔迹没有任何有效格（采样点为空或全部被折叠）");
    }

    StrokePlan plan;
    plan.stats.sampled = static_cast<std::uint32_t>(samples_.size());

    // **柱高溢出必须在 emit 之前判**：如果先 emit、再逐格发现越界，就会留下
    // "半根柱"（前几格成功、后面的失败）——那违反了整笔原子性。
    // 一格的柱顶 = y + column_height - 1，必须 <= layers-1。
    const std::int32_t top_slack = cfg_.grid.layers - cfg_.column_height;
    if (cfg_.column_height < 1) {
      return err(StackCode::OutOfGrid, "column_height 必须 >= 1（实际 " +
                                           std::to_string(cfg_.column_height) + "）");
    }
    for (const Cell& s : samples_) {
      if (s.y > top_slack) {
        return err(StackCode::OutOfGrid,
                   "柱高 " + std::to_string(cfg_.column_height) + " 从第 " + std::to_string(s.y) +
                       " 层起放会越过顶面（layers=" + std::to_string(cfg_.grid.layers) +
                       "）：整笔失败，不留半根柱");
      }
    }

    // 逐段 supercover。段本身由"去相邻重复"保证非空，所以 a == b 不会发生；
    // 但为了防御（万一将来有人改掉那步），这里仍按 §6.3 判据处理。
    // emit 表是**局部**变量：`end()` 是 const（两次 end() 必须给出同一 plan，
    // 这是"一次落笔一份 plan"的语义），所以不能往成员里写累计状态。
    std::vector<Cell> emitted;
    std::vector<Cell> seg;
    for (std::size_t i = 1; i < samples_.size(); ++i) {
      seg.clear();
      supercover_cells(samples_[i - 1], samples_[i], &seg);
      // 每段的第一格是上一段的末格，跳过它以免重复 emit。
      for (std::size_t k = (i == 1) ? 0 : 1; k < seg.size(); ++k) emitted.push_back(seg[k]);
    }
    if (samples_.size() == 1) emitted.push_back(samples_.front());

    // 去重（§6.4）。判据是**整数格相等**；用"已发射"表做线性查重 ——
    // 一笔的格数在数百量级，线性查找比维护哈希容器更简单，且**没有遍历序问题**。
    // 注意去重是**在 (x,y,z) 全三维上**做的：同 (x,z) 不同 y 是两块，不是重复。
    plan.cells.reserve(emitted.size());
    for (const Cell& c : emitted) {
      ++plan.stats.cells_emitted;
      const bool dup = std::find(plan.cells.begin(), plan.cells.end(), c) != plan.cells.end();
      if (dup) {
        ++plan.stats.skipped_occupied;
        continue;
      }
      plan.cells.push_back(c);
      ++plan.stats.cells_added;
    }
    plan.locked_layer = lock_.layer();
    if (plan.cells.empty()) {
      return err(StackCode::EmptyStroke, "笔迹没有任何有效格（全部被去重）");
    }
    return plan;
  }

  // 把 plan 落成块：每格从底部向上摞 column_height 块。
  // 判据（§7）：整笔要么全成、要么全不成；**一次落笔只改一次真源**。
  StackStatus commit(IBlockSet* set, const StrokePlan& plan, std::uint32_t rgba,
                     std::uint32_t stroke_id) const {
    if (set == nullptr) return StackStatus::Fail(StackCode::OutOfGrid, "commit: set 为空");
    if (plan.cells.empty()) {
      return StackStatus::Fail(StackCode::EmptyStroke, "commit: plan 没有任何格");
    }
    // 先算出全部要插的块（含柱高），再一次性 add —— 这样"某格已被占"时
    // add_stroke 的内部回滚能保住前面的块，不会留半根柱。
    std::vector<Cell> all;
    all.reserve(plan.cells.size() * static_cast<std::size_t>(cfg_.column_height));
    for (const Cell& base : plan.cells) {
      for (std::int32_t k = 0; k < cfg_.column_height; ++k) {
        Cell c;
        c.x = base.x;
        c.y = base.y + k;
        c.z = base.z;
        all.push_back(c);
      }
    }
    auto r = add_stroke(set, all, rgba, stroke_id);
    if (!r.ok()) {
      return StackStatus::Fail(r.error_code(), "commit 失败: " + r.text());
    }
    return StackStatus::Ok_();
  }

  StackResult<StrokePlan> stroke(const Ray* rays, std::size_t count) override {
    begin();
    for (std::size_t i = 0; i < count; ++i) {
      auto s = sample(rays[i]);
      if (!s.ok()) return s.error();
    }
    return end();
  }

  StackResult<StrokePlan> stroke_and_commit(IBlockSet* set, const Ray* rays, std::size_t count,
                                            std::uint32_t rgba,
                                            std::uint32_t stroke_id) override {
    auto plan = stroke(rays, count);
    if (!plan.ok()) return plan.error();
    auto st = commit(set, plan.value(), rgba, stroke_id);
    if (!st.ok()) return StackError::Fail(st.error_code(), st.text());
    return plan;
  }

  LayerLock& layer_lock() override { return lock_; }
  const LayerLock& layer_lock() const override { return lock_; }

 private:
  BrushConfig cfg_{};
  LayerLock lock_{cfg_.grid};
  std::vector<Cell> samples_;   // 吸附后的折线（已去相邻重复）
  std::uint32_t sampled_dups_ = 0;
  bool failed_ = false;
};

}  // namespace

std::unique_ptr<IBlockBrush> create_block_brush(const BrushConfig& cfg) {
  return std::make_unique<BlockBrush>(cfg);
}

}  // namespace magicblocks::features::stack
