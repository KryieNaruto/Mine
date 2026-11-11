// MagicBlocks L4 —— 魔法涂鸦：文档模型实现（F-SK 基座）
#include "mb_internal/magicblocks/features/sketch/sketch_document.h"

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

bool same_color(const primitives::Color4& a, const primitives::Color4& b) { return a == b; }

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

bool SketchDocument::set_stroke_color(StrokeId id, primitives::Color4 color) {
  for (std::size_t i = 0; i < strokes_.size(); ++i) {
    if (strokes_[i].id != id) continue;

    if (same_color(strokes_[i].color, color)) return true;  // 空操作，不压栈

    SketchEdit edit;
    edit.kind = SketchEdit::Kind::Recolor;
    edit.index = i;
    edit.stroke = strokes_[i];          // 存改之前的整笔（含颜色）
    edit.previous_color = strokes_[i].color;
    edit.stroke.color = color;          // 重做时要恢复新颜色
    push_edit(std::move(edit));

    strokes_[i].color = color;
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
    case SketchEdit::Kind::Recolor: {
      // 撤销"改色" = 把颜色换回 previous_color。几何一个字节都不动。
      for (Stroke& s : strokes_) {
        if (s.id == edit.stroke.id) {
          s.color = edit.previous_color;
          break;
        }
      }
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
    case SketchEdit::Kind::Recolor: {
      for (Stroke& s : strokes_) {
        if (s.id == edit.stroke.id) {
          s.color = edit.stroke.color;  // push 时存的是新颜色
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

std::uint64_t SketchDocument::geometry_hash() const {
  // 与 hash() 同构，唯独**不含颜色**四个分量。
  // 刻意不写成 "hash() XOR 颜色哈希" —— 那样改色不一定让 geometry_hash 不变
  // （XOR 会抵消到 0 的极端情形），也没法解释"几何"到底指什么。
  std::uint64_t h = 0xC0FFEEull;
  h = mix(h ^ static_cast<std::uint64_t>(strokes_.size()));
  for (const Stroke& s : strokes_) {
    h = mix(h ^ s.id.value);
    h = mix(h ^ static_cast<std::uint64_t>(s.points.size()));
    h = hash_float(h, s.width);
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

bool SketchDocument::supports_region_op(RegionOp op) { return primitives::supports_region_op(op); }

StyleCheck check_style(const Stroke& stroke, float close_tolerance) {
  // 委托给 L3 的 make_outline：可行性检查与实际几何生成必须是**同一份**实现。
  // 这里只借用它的错误串，不产生几何（Points 传引用，L3 不会写回）。
  const primitives::OutlineResult r =
      primitives::make_outline(stroke.points, stroke.style, stroke.width, stroke.round_cap,
                               6u, close_tolerance);
  if (r.ok()) return {true, ""};
  return {false, r.error};
}

}  // namespace magicblocks::features::sketch
