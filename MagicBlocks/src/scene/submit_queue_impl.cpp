// MagicBlocks SDK —— L2 S2 提交队列实现(不透明 material+depth / 透明远到近 / 稳定排序)
//
// 排序键**全部量化成整数**再比较(spec §6.3 / R7):
//   - 浮点直接比较会踩 `-0.0 == 0.0`、NaN、以及不同编译器 FMA 收敛差异,
//     导致同一输入在不同构建上排出的顺序不同 → 命令流哈希漂移。
//   - 量化用 `std::llround(round(v * kQuantScale))`,对 `-0.0` 与 `+0.0`
//     给同一个整数,对 NaN 走显式兜底(归到最远处),因此结果是全序。
// 零时钟、零 GPU、零显存。
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <vector>

#include "scene/scene_internal.h"

namespace magicblocks::scene {
namespace {

// 量化精度:1/1024 ≈ 0.001,远小于任何有意义的深度/距离差异,
// 又足以把"浮点末位抖动"吸收掉。
constexpr double kQuantScale = 1024.0;

// 把 float 量化成 int64。NaN/Inf 走显式兜底,不交给 UB。
std::int64_t quantize(float v) {
  if (!std::isfinite(v)) {
    // NaN → 0;-Inf → 最小;+Inf → 最大。保证全序且确定性。
    if (std::isnan(v)) return 0;
    return v < 0.0f ? std::numeric_limits<std::int64_t>::min()
                    : std::numeric_limits<std::int64_t>::max();
  }
  const double scaled = static_cast<double>(v) * kQuantScale;
  // 越界保护(极端浮点值乘 1024 后可能超出 int64)。
  if (scaled >= static_cast<double>(std::numeric_limits<std::int64_t>::max())) {
    return std::numeric_limits<std::int64_t>::max();
  }
  if (scaled <= static_cast<double>(std::numeric_limits<std::int64_t>::min())) {
    return std::numeric_limits<std::int64_t>::min();
  }
  return static_cast<std::int64_t>(std::llround(scaled));
}

// 相机到绘制项中心的**平方距离量化**。开方没必要(单调),省一次 sqrt,
// 也少一处浮点不确定来源。
std::int64_t distance_key_sq(const float center[3], const SubmitCamera& cam) {
  const double dx = static_cast<double>(center[0]) - static_cast<double>(cam.position[0]);
  const double dy = static_cast<double>(center[1]) - static_cast<double>(cam.position[1]);
  const double dz = static_cast<double>(center[2]) - static_cast<double>(cam.position[2]);
  const double d2 = dx * dx + dy * dy + dz * dz;
  if (!std::isfinite(d2)) return 0;
  const double scaled = d2 * kQuantScale;
  if (scaled >= static_cast<double>(std::numeric_limits<std::int64_t>::max())) {
    return std::numeric_limits<std::int64_t>::max();
  }
  if (scaled <= static_cast<double>(std::numeric_limits<std::int64_t>::min())) {
    return std::numeric_limits<std::int64_t>::min();
  }
  return static_cast<std::int64_t>(std::llround(scaled));
}

class SubmitQueueImpl final : public ISubmitQueue {
 public:
  void add(const DrawItem& item) override {
    pending_.push_back(item);
    sorted_valid_ = false;
  }

  void clear() override {
    pending_.clear();
    sorted_.clear();
    sorted_valid_ = false;
  }

  std::size_t size() const override { return pending_.size(); }

  const std::vector<DrawItem>& sort(const SubmitCamera& camera) override {
    // 预计算每条项的排序键,排序时不再做任何浮点运算 ——
    // 这样"两次排序结果逐字节相同"是结构性的,不依赖 std::sort 的稳定性。
    struct Keyed {
      std::size_t original_index = 0;
      bool transparent = false;
      std::uint32_t material = 0;
      std::int64_t primary = 0;    // 不透明: 量化 depth;透明: 量化距离平方
      std::uint64_t tie_break_id = 0;
    };
    std::vector<Keyed> keys;
    keys.reserve(pending_.size());
    for (std::size_t k = 0; k < pending_.size(); ++k) {
      const DrawItem& item = pending_[k];
      Keyed key;
      key.original_index = k;
      key.transparent = item.transparent;
      key.material = item.material;
      key.primary = item.transparent ? distance_key_sq(item.center, camera) : quantize(item.depth);
      key.tie_break_id = item.tie_break_id;
      keys.push_back(key);
    }

    // 全序比较器 —— 每个分支都不返回"相等",最后一级用插入序兜底:
    //   **不透明**(前):material 升序 → depth 升序(近到远) → tie_break_id 升序
    //   **透明**(后):距离**降序**(远到近) → material 升序 → tie_break_id 升序
    std::sort(keys.begin(), keys.end(), [](const Keyed& a, const Keyed& b) {
      if (a.transparent != b.transparent) return !a.transparent;  // 不透明在前
      if (a.transparent) {
        if (a.primary != b.primary) return a.primary > b.primary;  // 远到近
        if (a.material != b.material) return a.material < b.material;
        if (a.tie_break_id != b.tie_break_id) return a.tie_break_id < b.tie_break_id;
        return a.original_index < b.original_index;
      }
      if (a.material != b.material) return a.material < b.material;
      if (a.primary != b.primary) return a.primary < b.primary;    // 近到远
      if (a.tie_break_id != b.tie_break_id) return a.tie_break_id < b.tie_break_id;
      return a.original_index < b.original_index;
    });

    sorted_.clear();
    sorted_.reserve(keys.size());
    for (const Keyed& key : keys) sorted_.push_back(pending_[key.original_index]);
    sorted_valid_ = true;

    for (const DrawItem& item : pending_) {
      if (item.transparent) {
        ++transparent_count_;
      } else {
        ++opaque_count_;
      }
    }
    return sorted_;
  }

  const std::vector<DrawItem>& sorted() const override { return sorted_; }

  std::size_t opaque_count() const override {
    if (sorted_valid_) return opaque_count_;
    std::size_t n = 0;
    for (const DrawItem& item : pending_) {
      if (!item.transparent) ++n;
    }
    return n;
  }

  std::size_t transparent_count() const override {
    if (sorted_valid_) return transparent_count_;
    std::size_t n = 0;
    for (const DrawItem& item : pending_) {
      if (item.transparent) ++n;
    }
    return n;
  }

 private:
  std::vector<DrawItem> pending_;
  std::vector<DrawItem> sorted_;
  bool sorted_valid_ = false;
  std::size_t opaque_count_ = 0;
  std::size_t transparent_count_ = 0;
};

}  // namespace

std::unique_ptr<ISubmitQueue> create_submit_queue() {
  return std::make_unique<SubmitQueueImpl>();
}

}  // namespace magicblocks::scene
