// MagicBlocks L4 —— 魔法叠叠乐：IStackSession 实现（状态机 + 会话级撤销）
//
// 实现纪律（与 stack_session.h 的注释一一对应）：
//   1. **唯一的状态写入点**是 `transition_to()`；任何会改 `state_` 的操作都必须先过
//      `session_transition_allowed()`。于是"事件在哪个状态被接受"只有一份判据，
//      不可能出现"这里放行、那里拒绝"。
//   2. **失败不吞**。非法转移返回 InvalidTransition 并带人话 detail；合法但业务失败的
//      转移（抬笔越界 / 空笔 / 撤销不存在的笔）一律保持/回到一个**明确定义**的状态，
//      并且真源一个字节都不留在半截。D-VX-9：不 clamp、不跳过、不近似。
//   3. **撤销靠快照回滚**，不靠重放反操作。`begin_stroke` 拷一份真源，`end_stroke`
//      成功后丢掉；撤销 = 把快照写回新真源。因此"撤销后"与"从未画过"逐字节相同。
//   4. 不碰设备、不碰 Vulkan、不读时钟（G3/G4/G8）。
#include "magicblocks/features/stack/stack_session.h"

#include <algorithm>
#include <cstdio>
#include <utility>

namespace magicblocks::features::stack {

namespace {

std::string cell_text(const Cell& c) {
  return "(" + std::to_string(c.x) + "," + std::to_string(c.y) + "," + std::to_string(c.z) + ")";
}

std::uint64_t hash_u64(std::uint64_t h, const void* p, std::size_t n) {
  return hash_bytes(h, p, n);
}

class StackSession final : public IStackSession {
 public:
  explicit StackSession(GridSpec grid) : grid_(grid) {
    auto r = create_block_set(grid_);
    // 构造入口已用 validate_grid 过滤，这里不可能失败；但契约上不当"不可能"处理。
    if (r.ok()) set_ = std::move(r).take();
  }

  // ---------------------------------------------------------------- 只读

  GridSpec grid() const override { return grid_; }
  const IBlockSet& set() const override { return *set_; }
  IBlockSet& mutable_set() override { return *set_; }
  SessionState state() const override { return state_; }

  std::string describe() const override {
    char buf[192];
    std::snprintf(buf, sizeof(buf), "%s|locked=%d|gen=%llu|committed=%llu|samples=%u|undo=%zu|redo=%zu",
                  session_state_name(state_), locked_layer_, static_cast<unsigned long long>(stroke_gen_),
                  static_cast<unsigned long long>(committed_strokes_), pending_samples_,
                  undo_stack_.size(), redo_stack_.size());
    return std::string(buf);
  }

  SessionSnapshot snapshot() const override {
    SessionSnapshot s;
    s.state = state_;
    s.locked_layer = locked_layer_;
    s.stroke_gen = stroke_gen_;
    s.committed_strokes = committed_strokes_;
    s.pending_samples = pending_samples_;
    s.last_stroke_id = last_stroke_id_;
    s.undo_depth = undo_stack_.size();
    s.redo_depth = redo_stack_.size();
    for (const UndoEntry& e : undo_stack_) s.undo_ids.push_back(e.stroke_id);

    std::uint64_t h = kHashSeed;
    h = hash_u64(h, &s.state, sizeof(s.state));
    h = hash_u64(h, &s.locked_layer, sizeof(s.locked_layer));
    h = hash_u64(h, &s.stroke_gen, sizeof(s.stroke_gen));
    h = hash_u64(h, &s.committed_strokes, sizeof(s.committed_strokes));
    h = hash_u64(h, &s.pending_samples, sizeof(s.pending_samples));
    h = hash_u64(h, &s.last_stroke_id, sizeof(s.last_stroke_id));
    const std::uint64_t nundo = static_cast<std::uint64_t>(s.undo_depth);
    const std::uint64_t nredo = static_cast<std::uint64_t>(s.redo_depth);
    h = hash_u64(h, &nundo, sizeof(nundo));
    h = hash_u64(h, &nredo, sizeof(nredo));
    for (std::uint32_t id : s.undo_ids) h = hash_u64(h, &id, sizeof(id));
    s.hash = h;
    return s;
  }

  // ---------------------------------------------------------------- 落笔三段式

  StackStatus begin_stroke(const StrokeRequest& req) override {
    if (auto a = require_event(SessionEvent::BeginStroke); !a.ok()) return a;
    if (set_ == nullptr) return fail(StackCode::OutOfGrid, "会话没有真源");

    // 快照真源（整表，按真源下标序）。这里**只拷真源**，不拷派生索引 ——
    // 回滚时重建一个全新真源比"逆操作删格"更不容易漏。
    undo_snapshot_ = committed_copy();
    undo_pending_ = true;
    pending_.cells.clear();
    pending_.rgba = req.rgba;
    pending_layer_ = req.layer;
    pending_samples_ = 0;

    // 请求里已经带着格（一次性落笔）也算一次采样，便于统计口径统一。
    if (!req.cells.empty()) {
      pending_.cells = req.cells;
      ++pending_samples_;
    }
    last_stroke_id_ = kInvalidStrokeId;
    ++stroke_gen_;
    transition_to(SessionState::Stroking);
    return StackStatus::Ok_();
  }

  StackStatus move_sample(const std::vector<Cell>& cells) override {
    if (auto a = require_event(SessionEvent::MoveSample); !a.ok()) return a;
    pending_.cells.insert(pending_.cells.end(), cells.begin(), cells.end());
    ++pending_samples_;
    return StackStatus::Ok_();
  }

  StackResult<StrokeResult> end_stroke() override {
    if (auto a = require_event(SessionEvent::EndStroke); !a.ok()) return a.error();
    return commit_pending();
  }

  StackStatus rollback_failed_stroke() override {
    if (auto a = require_event(SessionEvent::RollbackFailedStroke); !a.ok()) return a;
    // 把 begin_stroke 时的快照写回 —— 即使某条失败路径把真源改了一半，这里也会归位。
    if (undo_pending_) {
      if (auto s = restore_set_from(undo_snapshot_); !s.ok()) {
        return fail(s.error_code(), "回滚到落笔前的快照失败: " + s.text());
      }
    }
    pending_.cells.clear();
    pending_samples_ = 0;
    pending_layer_ = -1;
    undo_pending_ = false;
    undo_snapshot_.clear();
    transition_to(SessionState::Idle);
    return StackStatus::Ok_();
  }

  StackResult<StrokeResult> commit_failed_stroke(const std::vector<Cell>& cells) override {
    if (auto a = require_event(SessionEvent::CommitFailedStroke); !a.ok()) return a.error();
    pending_.cells = cells;
    ++pending_samples_;
    return commit_pending();
  }

  // ---------------------------------------------------------------- 撤销 / 重做

  StackStatus undo_stroke(std::uint32_t stroke_id) override {
    if (auto a = require_event(SessionEvent::UndoStroke); !a.ok()) return a;
    if (stroke_id == kInvalidStrokeId) {
      return fail(StackCode::UnknownStroke, "stroke_id=0 是保留值，不存在这一笔");
    }

    auto it = std::find_if(undo_stack_.begin(), undo_stack_.end(),
                           [stroke_id](const UndoEntry& e) { return e.stroke_id == stroke_id; });
    if (it == undo_stack_.end()) {
      // 区分两种"撤销不到"：真源里根本没有这笔（UnknownStroke）vs 这笔还在但不在撤销栈里
      // （例如改色清空过撤销栈）。两者都是显式错误，只是 detail 不同。
      if (has_stroke(stroke_id)) {
        return fail(StackCode::UnknownStroke,
                    "笔 " + std::to_string(stroke_id) + " 的撤销记录已被清空（改色之后不能再撤销它）");
      }
      return fail(StackCode::UnknownStroke, "撤销不存在的笔 stroke_id=" + std::to_string(stroke_id));
    }

    // 把该笔移到重做栈顶。
    UndoEntry entry = std::move(*it);
    undo_stack_.erase(it);
    redo_stack_.push_back(entry);
    return restore_without(entry.stroke_id);
  }

  StackStatus undo_last() override {
    if (auto a = require_event(SessionEvent::UndoLast); !a.ok()) return a;
    if (undo_stack_.empty()) {
      return fail(StackCode::UnknownStroke, "撤销栈为空：没有可撤销的笔");
    }
    UndoEntry entry = std::move(undo_stack_.back());
    undo_stack_.pop_back();
    const std::uint32_t id = entry.stroke_id;
    redo_stack_.push_back(std::move(entry));
    return restore_without(id);
  }

  StackResult<RedoResult> redo_last() override {
    if (auto a = require_event(SessionEvent::RedoLast); !a.ok()) return a.error();
    if (redo_stack_.empty()) {
      return StackStatus::Fail(StackCode::UnknownStroke, "重做栈为空：没有可重做的笔").error();
    }
    const UndoEntry entry = redo_stack_.back();
    // I2：重做用**新的** stroke_id，绝不回收旧 id。
    const std::uint32_t new_id = set_->next_stroke_id();
    const std::vector<Block> before = committed_copy();
    for (const Block& b : entry.blocks) {
      Block nb = b;
      nb.stroke_id = new_id;
      auto r = set_->add(nb);
      if (!r.ok()) {
        // 整笔原子：恢复成重做前的样子再报错（绝不留下半笔）。
        if (auto rb = restore_set_from(before); !rb.ok()) return rb.error();
        redo_stack_.pop_back(); // 这一条已经不可重放（格被占了），移出栈，避免反复失败。
        return StackStatus::Fail(r.error_code(), "重做笔 " + std::to_string(entry.stroke_id) +
                                                    " 时第 " + cell_text(b.cell) + " 格失败: " + r.text())
            .error();
      }
    }
    redo_stack_.pop_back();
    UndoEntry u;
    u.stroke_id = new_id;
    u.layer = entry.layer;
    u.blocks = entry.blocks;
    for (Block& b : u.blocks) b.stroke_id = new_id;
    undo_stack_.push_back(std::move(u));
    last_stroke_id_ = new_id;
    RedoResult out;
    out.stroke_id = new_id;
    out.cells_added = static_cast<std::uint32_t>(entry.blocks.size());
    return out;
  }

  std::size_t undo_depth() const override { return undo_stack_.size(); }
  std::size_t redo_depth() const override { return redo_stack_.size(); }
  const std::vector<UndoEntry>& undo_stack() const override { return undo_stack_; }

  // ---------------------------------------------------------------- 改色

  StackStatus set_stroke_color(std::uint32_t stroke_id, std::uint32_t rgba) override {
    if (auto a = require_event(SessionEvent::SetStrokeColor); !a.ok()) return a;
    std::vector<std::size_t> idx;
    set_->indices_of_stroke(stroke_id, &idx);
    if (idx.empty()) {
      return fail(StackCode::UnknownStroke,
                  "改色：笔 stroke_id=" + std::to_string(stroke_id) + " 不在真源里");
    }
    for (std::size_t i : idx) {
      if (auto s = set_->set_color(i, rgba); !s.ok()) return s;
    }
    // 改色之后"撤销这一笔"的语义不再是"回到那笔落笔前"（颜色已经变了），
    // 所以撤销栈整体失效 —— 明确清空，而不是留着一个会给出错误画面的入口。
    undo_stack_.clear();
    redo_stack_.clear();
    return StackStatus::Ok_();
  }

  StackResult<SessionColorResult> apply_color_mode(ColorMode mode,
                                                   const ColorPalette& palette) override {
    if (auto a = require_event(SessionEvent::ApplyColorMode); !a.ok()) return a.error();
    ColorApplyStats st{};
    if (auto s = features::stack::apply_color_mode(*set_, mode, palette, &st); !s.ok()) {
      return s.error();
    }
    undo_stack_.clear();
    redo_stack_.clear();
    SessionColorResult out;
    out.blocks_changed = st.blocks_changed;
    out.undo_depth = undo_stack_.size();
    return out;
  }

  // ---------------------------------------------------------------- 层锁

  StackStatus lock_layer(std::int32_t layer) override {
    if (auto a = require_event(SessionEvent::LockLayer); !a.ok()) return a;
    if (layer < 0 || layer >= grid_.layers) {
      return fail(StackCode::LayerOutOfRange, "lock(" + std::to_string(layer) + ") 越界：合法层号 [0, " +
                                                 std::to_string(grid_.layers) + ")");
    }
    locked_layer_ = layer;
    return StackStatus::Ok_();
  }

  StackStatus unlock_layer() override {
    if (auto a = require_event(SessionEvent::UnlockLayer); !a.ok()) return a;
    locked_layer_ = -1;
    return StackStatus::Ok_();
  }

  bool layer_locked() const override { return locked_layer_ >= 0; }
  std::int32_t locked_layer() const override { return locked_layer_; }

 private:
  // ---- 状态转移：唯一写入点 ----
  StackStatus transition_to(SessionState next) {
    state_ = next;
    return StackStatus::Ok_();
  }

  static StackStatus fail(StackCode c, std::string d) { return StackStatus::Fail(c, std::move(d)); }

  // 拒绝事件时给出"怎么走才对"，而不是一句 InvalidTransition。
  StackStatus require_event(SessionEvent e) {
    if (session_transition_allowed(state_, e)) return StackStatus::Ok_();
    return fail(StackCode::InvalidTransition,
                std::string("状态机拒绝事件 ") + session_event_name(e) + "：当前状态 " +
                    session_state_name(state_) + wanted_hint(state_));
  }

  static std::string wanted_hint(SessionState s) {
    switch (s) {
      case SessionState::Idle:
        return "（idle 下只接受 begin_stroke / undo / redo / 改色 / 层锁）";
      case SessionState::Stroking:
        return "（落笔进行中：只接受 move_sample / end_stroke）";
      case SessionState::StrokeFailed:
        return "（上次落笔失败：必须先 commit_failed_stroke 或 rollback_failed_stroke）";
    }
    return "";
  }

  bool has_stroke(std::uint32_t stroke_id) const {
    std::vector<std::uint32_t> ids;
    set_->strokes_in_order(&ids);
    return std::find(ids.begin(), ids.end(), stroke_id) != ids.end();
  }

  // 把真源整表拷一份（撤销/重做失败时恢复用）。
  std::vector<Block> committed_copy() const {
    std::vector<Block> v(set_->size());
    for (std::size_t i = 0; i < v.size(); ++i) {
      const Block* b = set_->at(i);
      if (b != nullptr) v[i] = *b;
    }
    return v;
  }

  // 用一份 Block 列表重建真源（保持下标序）。失败返回错误，不改动当前真源直到成功。
  StackStatus restore_set_from(const std::vector<Block>& blocks) {
    auto fresh_r = create_block_set(grid_);
    if (!fresh_r.ok()) return fresh_r.error();
    std::unique_ptr<IBlockSet> fresh = std::move(fresh_r).take();
    for (const Block& b : blocks) {
      auto r = fresh->add(b);
      if (!r.ok()) return StackStatus::Fail(r.error_code(), "重建真源失败: " + r.text());
    }
    set_ = std::move(fresh);
    return StackStatus::Ok_();
  }

  // 撤销：把真源恢复成"这一笔从未落过"的样子。
  //
  // 走的是"按真源下标序重建、跳过该笔"：与快照回滚给出**同一份**真源
  // （同样 24 B 记录、同样顺序），但不需要额外保存"那一笔当时"的整表。
  // 两条路径都经过 `restore_set_from`，口径一致。
  StackStatus restore_without(std::uint32_t stroke_id) {
    std::vector<Block> keep;
    keep.reserve(set_->size());
    for (std::size_t i = 0; i < set_->size(); ++i) {
      const Block* b = set_->at(i);
      if (b == nullptr) continue;
      if (b->stroke_id == stroke_id) continue;
      keep.push_back(*b);
    }
    if (auto s = restore_set_from(keep); !s.ok()) return s.error();
    last_stroke_id_ = stroke_id;
    return StackStatus::Ok_();
  }

  // 抬笔落块。整笔原子：任一格失败 → 真源回到 begin_stroke 之前。
  StackResult<StrokeResult> commit_pending() {
    // 去重：整数格相等（§6.4）。用"已见列表"而不是哈希集合，保证顺序可复算。
    std::vector<Cell> unique;
    unique.reserve(pending_.cells.size());
    for (const Cell& c : pending_.cells) {
      if (std::find(unique.begin(), unique.end(), c) == unique.end()) unique.push_back(c);
    }

    StrokeResult res;
    res.cells_requested = static_cast<std::uint32_t>(pending_.cells.size());

    if (unique.empty()) {
      // 空笔是**显式失败**（§6.6 EmptyStroke），且要进 StrokeFailed 让调用方必须响。
      pending_.cells.clear();
      pending_samples_ = 0;
      transition_to(SessionState::StrokeFailed);
      return StackStatus::Fail(StackCode::EmptyStroke, "抬笔：这一笔没有任何有效格（整笔未落成）").error();
    }

    const std::uint32_t id = set_->next_stroke_id();
    std::uint32_t added = 0;
    std::uint32_t skipped = 0;
    for (const Cell& c : unique) {
      if (set_->find(c) != nullptr) {
        // 已占格：整笔里的这一格不生效，计数进 skipped_occupied（§6.4 / §8），
        // **不**整笔失败 —— 后画的圈压在先画的圈上是叠叠乐的常态。
        ++skipped;
        continue;
      }
      Block b;
      b.cell = c;
      b.color = pending_.rgba;
      b.stroke_id = id;
      b.flags = 0;
      auto r = set_->add(b);
      if (!r.ok()) {
        // 越界 / flags 非法等硬失败 → **整笔回滚**，真源回到 begin_stroke 之前
        // （快照写回，不走"删这一笔的格" —— 那样万一有格已经和前一笔同 id 就危险了）。
        if (auto rs = restore_set_from(undo_snapshot_); !rs.ok()) {
          transition_to(SessionState::StrokeFailed);
          return StackStatus::Fail(rs.error_code(), "整笔回滚失败: " + rs.text()).error();
        }
        transition_to(SessionState::StrokeFailed);
        return StackStatus::Fail(r.error_code(), "抬笔整笔失败于格 " + cell_text(c) + ": " + r.text()).error();
      }
      ++added;
    }

    if (added == 0) {
      // 整笔都被占格吞掉：data 没变，但这不是错误（§8）；记一次已提交，id 不消耗。
      pending_.cells.clear();
      pending_samples_ = 0;
      undo_pending_ = false;
      undo_snapshot_.clear();
      transition_to(SessionState::Idle);
      res.stroke_id = kInvalidStrokeId;
      res.cells_added = 0;
      res.skipped_occupied = skipped;
      return res;
    }

    // 把这一笔按真源下标序收进撤销栈。
    UndoEntry entry;
    entry.stroke_id = id;
    entry.layer = pending_layer_;
    std::vector<std::size_t> idx;
    set_->indices_of_stroke(id, &idx);
    entry.blocks.reserve(idx.size());
    for (std::size_t i : idx) {
      const Block* b = set_->at(i);
      if (b != nullptr) entry.blocks.push_back(*b);
    }
    undo_stack_.push_back(std::move(entry));
    // 新落笔让重做栈失效：否则"撤销 → 重做"会把一笔画到早已不成立的画面上。
    redo_stack_.clear();

    pending_.cells.clear();
    pending_samples_ = 0;
    undo_pending_ = false;
    undo_snapshot_.clear();
    ++committed_strokes_;
    last_stroke_id_ = id;
    transition_to(SessionState::Idle);

    res.stroke_id = id;
    res.cells_added = added;
    res.skipped_occupied = skipped;
    return res;
  }

  GridSpec grid_{};
  std::unique_ptr<IBlockSet> set_;
  SessionState state_ = SessionState::Idle;

  // 当前落笔
  StrokeRequest pending_{};
  std::int32_t pending_layer_ = -1;
  std::uint32_t pending_samples_ = 0;
  bool undo_pending_ = false;        // 当前落笔还没有提交（快照仍然有效）
  std::vector<Block> undo_snapshot_; // begin_stroke 时的整表快照（按真源下标序）

  // 层锁
  std::int32_t locked_layer_ = -1;

  // 计数器（快照/确定性断言用）
  std::uint64_t stroke_gen_ = 0;
  std::uint64_t committed_strokes_ = 0;
  std::uint32_t last_stroke_id_ = kInvalidStrokeId;

  std::vector<UndoEntry> undo_stack_;
  std::vector<UndoEntry> redo_stack_;
};

}  // namespace

const char* session_state_name(SessionState s) {
  switch (s) {
    case SessionState::Idle:
      return "idle";
    case SessionState::Stroking:
      return "stroking";
    case SessionState::StrokeFailed:
      return "stroke_failed";
  }
  return "?";
}

const char* session_event_name(SessionEvent e) {
  switch (e) {
    case SessionEvent::BeginStroke:
      return "begin_stroke";
    case SessionEvent::MoveSample:
      return "move_sample";
    case SessionEvent::EndStroke:
      return "end_stroke";
    case SessionEvent::CommitFailedStroke:
      return "commit_failed_stroke";
    case SessionEvent::RollbackFailedStroke:
      return "rollback_failed_stroke";
    case SessionEvent::UndoStroke:
      return "undo_stroke";
    case SessionEvent::UndoLast:
      return "undo_last";
    case SessionEvent::RedoLast:
      return "redo_last";
    case SessionEvent::LockLayer:
      return "lock_layer";
    case SessionEvent::UnlockLayer:
      return "unlock_layer";
    case SessionEvent::SetStrokeColor:
      return "set_stroke_color";
    case SessionEvent::ApplyColorMode:
      return "apply_color_mode";
  }
  return "?";
}

bool session_transition_allowed(SessionState from, SessionEvent e) {
  switch (from) {
    case SessionState::Idle:
      // idle 下只接受"开一段新事务"的事件。抬笔/采样/补救在 idle 一律非法。
      switch (e) {
        case SessionEvent::BeginStroke:
        case SessionEvent::UndoStroke:
        case SessionEvent::UndoLast:
        case SessionEvent::RedoLast:
        case SessionEvent::LockLayer:
        case SessionEvent::UnlockLayer:
        case SessionEvent::SetStrokeColor:
        case SessionEvent::ApplyColorMode:
          return true;
        case SessionEvent::MoveSample:
        case SessionEvent::EndStroke:
        case SessionEvent::CommitFailedStroke:
        case SessionEvent::RollbackFailedStroke:
          return false;
      }
      return false;
    case SessionState::Stroking:
      // 落笔进行中：只允许采样与抬笔。**不许**中途撤销/改色/锁层 ——
      // 那些会在"一笔还没定稿"时改真源，让撤销快照与真源对不上。
      return e == SessionEvent::MoveSample || e == SessionEvent::EndStroke;
    case SessionState::StrokeFailed:
      // 失败之后必须显式补救，不许直接开始下一笔（否则失败会被悄悄遗忘）。
      return e == SessionEvent::CommitFailedStroke || e == SessionEvent::RollbackFailedStroke;
  }
  return false;
}

std::unique_ptr<IStackSession> create_stack_session(const GridSpec& grid) {
  if (auto s = validate_grid(grid); !s.ok()) return nullptr;
  return std::make_unique<StackSession>(grid);
}

}  // namespace magicblocks::features::stack
