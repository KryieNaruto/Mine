// MagicBlocks L4 —— 魔法涂鸦：文档模型实现（F-SK 基座）
#include "magicblocks/features/sketch/sketch_document.h"

#include <cmath>
#include <cstring>

namespace magicblocks::features::sketch {
namespace {

// 64 位混合（splitmix64 的 finalizer）。
// 选它是因为它把相邻整数打散得很彻底，而文档哈希的输入高度连续
// （id 递增、点坐标相邻），用简单乘法混合会大量碰撞。
std::uint64_t mix(std::uint64_t x) {
  x += 0x9E3779B97F4A7C15ull;
  x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
  x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
  return x ^ (x >> 31);
}

// 把 float 的**位模式**揉进哈希。
// 不用 static_cast<std::uint64_t>(float)：那会做数值转换，
// -0.0f 和 0.0f 会撞成同一个值，而回放校验要求逐位一致。
std::uint64_t hash_float(std::uint64_t seed, float v) {
  std::uint32_t bits = 0;
  std::memcpy(&bits, &v, sizeof(bits));
  return mix(seed ^ (static_cast<std::uint64_t>(bits) << 1));
}

}  // namespace

const Stroke* SketchDocument::find(StrokeId id) const {
  if (!id.valid()) return nullptr;
  for (const Stroke& s : strokes_) {
    if (s.id == id) return &s;
  }
  return nullptr;
}

StrokeId SketchDocument::add_stroke(Stroke stroke) {
  // 点数不足的笔画不是"有点怪"，是**无效输入**：一个点的折线没有长度，
  // 生成的胶囊轮廓是空集，挤出的网格是空的。与其让它在下游变成
  // 一张空网格，不如在这里拒绝并保持文档不变。
  if (stroke.points.empty()) return StrokeId{};
  if (!(stroke.width > 0.0f)) return StrokeId{};

  if (stroke.max_points != 0 && stroke.points.size() > stroke.max_points) {
    stroke.points.resize(stroke.max_points);
  }

  stroke.id = StrokeId{next_id_++};

  SketchEdit edit;
  edit.kind = SketchEdit::Kind::AddStroke;
  edit.index = strokes_.size();
  edit.stroke = stroke;  // 完整存一份，供 undo 后重做插回
  push_edit(std::move(edit));

  strokes_.push_back(std::move(stroke));
  return strokes_.back().id;
}

bool SketchDocument::remove_stroke(StrokeId id) {
  for (std::size_t i = 0; i < strokes_.size(); ++i) {
    if (strokes_[i].id != id) continue;

    SketchEdit edit;
    edit.kind = SketchEdit::Kind::RemoveStroke;
    edit.index = i;
    edit.stroke = strokes_[i];
    push_edit(std::move(edit));

    strokes_.erase(strokes_.begin() + static_cast<std::ptrdiff_t>(i));
    return true;
  }
  return false;
}

void SketchDocument::push_edit(SketchEdit edit) {
  undo_.push_back(std::move(edit));
  // 新的编辑动作会让"重做栈"失效 —— 这是撤销/重做的标准语义。
  // 不在这里清会导致 redo 把一条已被新编辑覆盖的历史又插回去。
  redo_.clear();
}

bool SketchDocument::undo() {
  if (undo_.empty()) return false;
  SketchEdit edit = std::move(undo_.back());
  undo_.pop_back();

  switch (edit.kind) {
    case SketchEdit::Kind::AddStroke: {
      // 撤销"加" = 删掉。按 id 找而不是按 index —— index 可能因后续
      // 中间删除笔画而失效，id 才是稳定身份。
      for (std::size_t i = 0; i < strokes_.size(); ++i) {
        if (strokes_[i].id == edit.stroke.id) {
          strokes_.erase(strokes_.begin() + static_cast<std::ptrdiff_t>(i));
          break;
        }
      }
      break;
    }
    case SketchEdit::Kind::RemoveStroke: {
      // 撤销"删" = 插回原位置。index 超出当前长度时退化为追加，
      // 而不是崩 —— 这是防御性的，正常历史链上不会发生。
      const std::size_t at = edit.index <= strokes_.size() ? edit.index : strokes_.size();
      strokes_.insert(strokes_.begin() + static_cast<std::ptrdiff_t>(at), edit.stroke);
      break;
    }
  }

  redo_.push_back(std::move(edit));
  return true;
}

bool SketchDocument::redo() {
  if (redo_.empty()) return false;
  SketchEdit edit = std::move(redo_.back());
  redo_.pop_back();

  switch (edit.kind) {
    case SketchEdit::Kind::AddStroke: {
      const std::size_t at = edit.index <= strokes_.size() ? edit.index : strokes_.size();
      strokes_.insert(strokes_.begin() + static_cast<std::ptrdiff_t>(at), edit.stroke);
      break;
    }
    case SketchEdit::Kind::RemoveStroke: {
      for (std::size_t i = 0; i < strokes_.size(); ++i) {
        if (strokes_[i].id == edit.stroke.id) {
          strokes_.erase(strokes_.begin() + static_cast<std::ptrdiff_t>(i));
          break;
        }
      }
      break;
    }
  }

  undo_.push_back(std::move(edit));
  return true;
}

void SketchDocument::clear_history() {
  undo_.clear();
  redo_.clear();
}

void SketchDocument::clear() {
  strokes_.clear();
  undo_.clear();
  redo_.clear();
  // next_id_ 刻意**不重置**：清空画布后重画的笔画应当拿到新 id，
  // 否则旧命令流里残留的 id 会意外命中新笔画。
}

std::uint64_t SketchDocument::hash() const {
  std::uint64_t h = 0xC0FFEEull;
  h = mix(h ^ static_cast<std::uint64_t>(strokes_.size()));
  for (const Stroke& s : strokes_) {
    h = mix(h ^ s.id.value);
    h = mix(h ^ static_cast<std::uint64_t>(s.points.size()));
    h = hash_float(h, s.width);
    h = hash_float(h, s.color.r);
    h = hash_float(h, s.color.g);
    h = hash_float(h, s.color.b);
    h = hash_float(h, s.color.a);
    h = mix(h ^ (s.round_cap ? 1u : 2u));
    h = mix(h ^ (static_cast<std::uint64_t>(s.style) << 8));
    h = mix(h ^ (s.closed ? 1u : 2u));
    for (const Vec2& p : s.points) {
      h = hash_float(h, p.x);
      h = hash_float(h, p.y);
    }
  }
  return h;
}

bool SketchDocument::supports_region_op(RegionOp op) { return op == RegionOp::Union; }

StyleCheck check_style(const Stroke& stroke, float close_tolerance) {
  switch (stroke.style) {
    case PathStyle::Ribbon:
      if (stroke.points.size() < 2) {
        return {false, "Ribbon 需要至少 2 个采样点"};
      }
      if (!(stroke.width > 0.0f)) {
        return {false, "Ribbon 需要正的笔宽"};
      }
      return {true, ""};

    case PathStyle::Region: {
      // Region 要求折线真的闭合。**不自动闭合**：
      // 自动连首尾会把用户画的"C"变成"O"，那是改变用户意图，
      // 属于静默修正 —— D8 明令禁止。
      if (stroke.points.size() < 3) {
        return {false, "Region 需要至少 3 个采样点"};
      }
      const Vec2& a = stroke.points.front();
      const Vec2& b = stroke.points.back();
      const float dx = b.x - a.x;
      const float dy = b.y - a.y;
      if (std::sqrt(dx * dx + dy * dy) > close_tolerance) {
        return {false, "Region 要求折线首尾闭合(超出容差)"};
      }
      return {true, ""};
    }
  }
  return {false, "未知的 PathStyle"};
}

}  // namespace magicblocks::features::sketch
