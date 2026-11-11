// MagicBlocks L4 —— 魔法叠叠乐：IBlockSet 实现（真源）
//
// 实现纪律（与 block_set.h 的注释一一对应）：
//   - 真源 = `blocks_`（按插入顺序的线性数组），派生 = `occupied_`（Cell → 下标）；
//   - 撤销**不从 memory 里挪动其它条目**然后重排索引那种花活 —— 直接 erase 再
//     线性重建派生索引。原因：下标会前移，外层持有的 BlockId 在撤销后失效是
//     **刻意的**（设计 §5.3），UI 只持有 stroke_id；
//   - 所有对外输出（strokes_in_order / indices_of_stroke / hash）一律走真源下标序，
//     没有任何一处遍历 `occupied_`。
#include "magicblocks/features/stack/block_set.h"

#include <algorithm>
#include <cstring>

namespace magicblocks::features::stack {

namespace {

// 所有"位置越出合法域"共用的错误码。detail 必须写清楚到底是哪一种，
// 因为调用方要按人话排错（设计 §6.6：所有错误都带 detail）。
StackError out_of_grid(std::string detail) { return StackError::Fail(StackCode::OutOfGrid, std::move(detail)); }

class BlockSet final : public IBlockSet {
 public:
  explicit BlockSet(GridSpec grid) : grid_(grid) {}

  // ---- 写入 ----

  StackResult<std::size_t> add(const Block& b) override {
    if (auto e = check_block(b); !e.ok()) return e;

    if (occupied_.find(b.cell) != occupied_.end()) {
      // I1：同一格里不可能有两块。**不覆盖**（覆盖会让先画的那笔凭空变色/丢块，
      // 而且哈希会依赖插入顺序 —— 那是隐性第二真源）。
      return out_of_grid("格 (" + cell_text(b.cell) + ") 已被占用，拒绝重复插入");
    }

    const std::size_t idx = blocks_.size();
    blocks_.push_back(b);
    occupied_.emplace(b.cell, idx);
    if (b.stroke_id > max_stroke_id_) max_stroke_id_ = b.stroke_id;
    ++stroke_live_[b.stroke_id];
    return idx;
  }

  StackStatus remove_stroke(std::uint32_t stroke_id) override {
    return undo_stroke(stroke_id, nullptr);
  }

  StackStatus undo_stroke(std::uint32_t stroke_id, std::vector<Block>* out) override {
    if (stroke_id == kInvalidStrokeId) {
      return StackStatus::Fail(StackCode::UnknownStroke, "stroke_id=0 是保留值，不存在这一笔");
    }
    auto live = stroke_live_.find(stroke_id);
    if (live == stroke_live_.end() || live->second == 0) {
      return StackStatus::Fail(StackCode::UnknownStroke,
                               "撤销不存在的笔画 stroke_id=" + std::to_string(stroke_id));
    }

    if (out != nullptr) {
      out->clear();
      out->reserve(blocks_.size());
      for (const Block& b : blocks_) {
        if (b.stroke_id == stroke_id) out->push_back(b);
      }
    }

    // 就地删除（保序）。用 remove_if + erase 而不是"逐个 erase"：后者是 O(n²)，
    // 而这里只要每个元素移动一次，且**顺序稳定**（下标序被保住，哈希可复算）。
    const auto it = std::remove_if(blocks_.begin(), blocks_.end(),
                                   [stroke_id](const Block& b) { return b.stroke_id == stroke_id; });
    blocks_.erase(it, blocks_.end());
    stroke_live_.erase(stroke_id);

    // 派生索引整体重建。为什么不用"只删这几格"：删除后所有大于被删下标的位置都会
    // 前移，逐项减一是 O(n) 且容易算错；重建同样是 O(n) 且**不可能漏**。
    rebuild_index();
    return StackStatus::Ok_();
  }

  // ---- 查询 ----

  std::size_t size() const override { return blocks_.size(); }
  const Block* data() const override { return blocks_.empty() ? nullptr : blocks_.data(); }

  const Block* find(Cell c) const override {
    const auto it = occupied_.find(c);
    if (it == occupied_.end()) return nullptr;
    return &blocks_[it->second];
  }

  const Block* at(std::size_t index) const override {
    if (index >= blocks_.size()) return nullptr;
    return &blocks_[index];
  }

  std::uint32_t max_stroke_id() const override { return max_stroke_id_; }
  std::size_t stroke_count() const override { return stroke_live_.size(); }
  std::uint32_t next_stroke_id() const override { return max_stroke_id_ + 1u; }

  void strokes_in_order(std::vector<std::uint32_t>* out) const override {
    if (out == nullptr) return;
    // 从**真源下标序**收集（不是从 stroke_live_ 这个 unordered_map 遍历），
    // 去重后升序输出。两个理由：
    //   1. 遍历 unordered_map 的序不确定 —— 只要有调用方拿它当输出顺序，
    //      golden 就会在换编译器时漂移；
    //   2. 升序让输出与插入顺序无关，便于 CLI 逐笔撤销时得到确定的顺序。
    // 去重用的是"上一次写入的 id"：真源里一笔的块**不一定相邻**（交错落笔），
    // 所以用 found 标记表而不是只比相邻。
    std::vector<std::uint32_t> seen;
    seen.reserve(stroke_live_.size());
    for (const Block& b : blocks_) {
      if (std::find(seen.begin(), seen.end(), b.stroke_id) != seen.end()) continue;
      seen.push_back(b.stroke_id);
    }
    std::sort(seen.begin(), seen.end());
    *out = std::move(seen);
  }

  void indices_of_stroke(std::uint32_t stroke_id, std::vector<std::size_t>* out) const override {
    if (out == nullptr) return;
    out->clear();
    for (std::size_t i = 0; i < blocks_.size(); ++i) {
      if (blocks_[i].stroke_id == stroke_id) out->push_back(i);
    }
  }

  // ---- 改色 ----

  StackStatus set_color(std::size_t index, std::uint32_t rgba) override {
    if (index >= blocks_.size()) {
      return StackStatus::Fail(StackCode::OutOfGrid, "set_color 下标越界: " + std::to_string(index) +
                                                         " >= " + std::to_string(blocks_.size()));
    }
    // 只改 color。cell / stroke_id / flags 一个字节都不碰（设计 §11 的硬判据）。
    blocks_[index].color = rgba;
    return StackStatus::Ok_();
  }

  // ---- 哈希 ----

  std::uint64_t hash() const override {
    std::uint64_t h = kHashSeed;
    for (const Block& b : blocks_) {
      // 逐字节喂 24 B。static_assert 保证无 padding，所以"结构体 memcpy"
      // 与"逐字段喂"在这里是同一件事；用逐字段是为了让口径在代码里可见，
      // 且将来加字段时编译期就会提醒这里要同步。
      h = hash_bytes(h, &b.cell.x, sizeof(b.cell.x));
      h = hash_bytes(h, &b.cell.y, sizeof(b.cell.y));
      h = hash_bytes(h, &b.cell.z, sizeof(b.cell.z));
      h = hash_bytes(h, &b.color, sizeof(b.color));
      h = hash_bytes(h, &b.stroke_id, sizeof(b.stroke_id));
      h = hash_bytes(h, &b.flags, sizeof(b.flags));
    }
    return h;
  }

  // ---- 不变量自检 ----

  StackStatus validate() const override {
    // I3（逐块）
    for (std::size_t i = 0; i < blocks_.size(); ++i) {
      if (auto e = check_block(blocks_[i]); !e.ok()) {
        return StackStatus::Fail(e.error_code(), "第 " + std::to_string(i) + " 块: " + e.text());
      }
    }
    // I1（跨块）：用派生索引反查，命中不等于自身下标即重复。
    for (std::size_t i = 0; i < blocks_.size(); ++i) {
      const auto it = occupied_.find(blocks_[i].cell);
      if (it == occupied_.end() || it->second != i) {
        return StackStatus::Fail(StackCode::OutOfGrid,
                                 "派生索引与真源不一致(第 " + std::to_string(i) + " 块)");
      }
    }
    if (occupied_.size() != blocks_.size()) {
      return StackStatus::Fail(StackCode::OutOfGrid,
                               "派生索引条数与真源不一致: " + std::to_string(occupied_.size()) + " vs " +
                                   std::to_string(blocks_.size()));
    }
    // stroke_live_ 的条数必须等于"真源里不同 stroke_id 的个数"。
    std::vector<std::uint32_t> ids;
    strokes_in_order(&ids);
    if (ids.size() != stroke_live_.size()) {
      return StackStatus::Fail(StackCode::OutOfGrid,
                               "笔画派生缓存与真源不一致: " + std::to_string(stroke_live_.size()) +
                                   " vs " + std::to_string(ids.size()));
    }
    return StackStatus::Ok_();
  }

 private:
  static std::string cell_text(const Cell& c) {
    return std::to_string(c.x) + "," + std::to_string(c.y) + "," + std::to_string(c.z);
  }

  // I3 的单块校验。非法 → 显式错误（不 clamp、不静默清零 flags）。
  StackError check_block(const Block& b) const {
    if (b.cell.y < 0 || b.cell.y >= grid_.layers) {
      return out_of_grid("cell.y=" + std::to_string(b.cell.y) + " 越出 [0, " +
                         std::to_string(grid_.layers) + ")");
    }
    if (b.cell.x < -grid_.half_extent || b.cell.x >= grid_.half_extent) {
      return out_of_grid("cell.x=" + std::to_string(b.cell.x) + " 越出 [-" +
                         std::to_string(grid_.half_extent) + ", " + std::to_string(grid_.half_extent) + ")");
    }
    if (b.cell.z < -grid_.half_extent || b.cell.z >= grid_.half_extent) {
      return out_of_grid("cell.z=" + std::to_string(b.cell.z) + " 越出 [-" +
                         std::to_string(grid_.half_extent) + ", " + std::to_string(grid_.half_extent) + ")");
    }
    if ((b.flags & ~kFlagDefinedMask) != 0u) {
      return out_of_grid("Block.flags 含未定义位 0x" + hex32(b.flags));
    }
    if (b.stroke_id == kInvalidStrokeId) {
      return out_of_grid("stroke_id=0 是保留值，真实笔画必须从 1 起");
    }
    return StackError::Ok_();
  }

  static std::string hex32(std::uint32_t v) {
    static const char* kDigits = "0123456789abcdef";
    std::string s(8, '0');
    for (int i = 7; i >= 0; --i) {
      s[static_cast<std::size_t>(i)] = kDigits[v & 0xFu];
      v >>= 4;
    }
    return s;
  }

  void rebuild_index() {
    occupied_.clear();
    occupied_.reserve(blocks_.size());
    for (std::size_t i = 0; i < blocks_.size(); ++i) occupied_.emplace(blocks_[i].cell, i);
    // stroke_live_ 也要跟着真源重建：撤销只删了被撤的那一笔的键，但为了
    // "派生缓存可随时由真源完全重建"这条纪律，这里索性全量重建。
    stroke_live_.clear();
    for (const Block& b : blocks_) ++stroke_live_[b.stroke_id];
    // **`max_stroke_id_` 刻意不在这里重算。**
    // 它是 I2（单调递增、永不复用）的唯一载体，属于"已分配过的水位"，不是真源的
    // 纯派生量：撤销一笔后真源里确实不再有那个 id，但再把它发给下一笔就违反 I2
    // —— "重做换新 id"会失效，同一 id 先后指向两段不同历史。
    // （这里重算过一次，结果是撤销后 next_stroke_id() 回退、重做拿到旧 id，
    //  BlockSet.StrokeIdIsMonotonicAndNeverReused 当场抓住。此后只有 add() 抬高它。）
  }

  GridSpec grid_{};
  std::vector<Block> blocks_;
  // 派生缓存：Cell → 真源下标。**不进哈希、不用于任何输出顺序。**
  std::unordered_map<Cell, std::size_t, CellHash> occupied_;
  // 派生缓存：stroke_id → 现存块数。同样不进哈希、不用于输出顺序。
  std::unordered_map<std::uint32_t, std::size_t> stroke_live_;
  // 已分配过的最大 id。撤销**不回收**它（I2：永不复用）。
  std::uint32_t max_stroke_id_ = 0;
};

}  // namespace

const char* stack_code_name(StackCode code) {
  switch (code) {
    case StackCode::Ok: return "Ok";
    case StackCode::RayParallelToLayer: return "RayParallelToLayer";
    case StackCode::RayMissesLayer: return "RayMissesLayer";
    case StackCode::OutOfGrid: return "OutOfGrid";
    case StackCode::LayerOutOfRange: return "LayerOutOfRange";
    case StackCode::LockLayerRequired: return "LockLayerRequired";
    case StackCode::UnknownStroke: return "UnknownStroke";
    case StackCode::UnknownTemplate: return "UnknownTemplate";
    case StackCode::TemplateOutOfGrid: return "TemplateOutOfGrid";
    case StackCode::EmptyStroke: return "EmptyStroke";
    case StackCode::ColorOutOfRange: return "ColorOutOfRange";
    case StackCode::InvalidTransition: return "InvalidTransition";
    case StackCode::PendingStroke: return "PendingStroke";
  }
  return "?";
}

StackStatus validate_grid(const GridSpec& grid) {
  if (!(grid.cell_size > 0.0f)) {
    return StackStatus::Fail(StackCode::OutOfGrid, "GridSpec.cell_size 必须 > 0");
  }
  if (grid.layers < 1) {
    return StackStatus::Fail(StackCode::OutOfGrid, "GridSpec.layers 必须 >= 1");
  }
  if (grid.half_extent < 1) {
    return StackStatus::Fail(StackCode::OutOfGrid, "GridSpec.half_extent 必须 >= 1");
  }
  return StackStatus::Ok_();
}

StackResult<std::unique_ptr<IBlockSet>> create_block_set(const GridSpec& grid) {
  if (auto s = validate_grid(grid); !s.ok()) return s.error();
  return std::unique_ptr<IBlockSet>(new BlockSet(grid));
}

StackStatus add_stroke(IBlockSet* set, const std::vector<Cell>& cells, std::uint32_t rgba,
                       std::uint32_t stroke_id) {
  if (set == nullptr) return StackStatus::Fail(StackCode::OutOfGrid, "add_stroke: set 为空");
  if (cells.empty()) return StackStatus::Fail(StackCode::EmptyStroke, "add_stroke: 格列表为空");
  if (stroke_id == kInvalidStrokeId) {
    return StackStatus::Fail(StackCode::UnknownStroke, "add_stroke: stroke_id=0 是保留值");
  }

  std::size_t added = 0;
  for (const Cell& c : cells) {
    Block b;
    b.cell = c;
    b.color = rgba;
    b.stroke_id = stroke_id;
    b.flags = 0;
    auto r = set->add(b);
    if (!r.ok()) {
      // **整笔回滚**：要么整笔成、要么整笔不成，绝不留半根柱（设计 §8）。
      set->remove_stroke(stroke_id);
      return StackStatus::Fail(r.error_code(), "第 " + std::to_string(added) + " 格 " + "(" +
                                             std::to_string(c.x) + "," + std::to_string(c.y) + "," +
                                             std::to_string(c.z) + ") 失败: " + r.text());
    }
    ++added;
  }
  return StackStatus::Ok_();
}

}  // namespace magicblocks::features::stack
