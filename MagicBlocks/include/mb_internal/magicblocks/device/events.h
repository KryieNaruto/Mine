// MagicBlocks SDK —— 统一输入事件流(D6)
//
// 命令复现的关键:事件流必须可完整回放。因此 EventStream 只接受
// 调用方给定的 timestamp_us,头文件内**不出现任何时钟调用**
// (不读 steady_clock / system_clock),回放时时间戳原样复现。
#pragma once

#include <cstdint>
#include <vector>

namespace magicblocks::device {

struct Extent2D;  // fwd

enum class InputEventKind : std::uint8_t {
  Unknown = 0,
  PointerDown,
  PointerMove,
  PointerUp,
  PointerCancel,
  KeyDown,
  KeyUp,
  Resize,
  Focus,
  Pause,
  Resume,
  Custom,
};

struct Vec2f {
  float x = 0.f;
  float y = 0.f;
};

struct InputEvent {
  InputEventKind kind = InputEventKind::Unknown;
  std::uint32_t pointer_id = 0;   // 多指
  Vec2f position{};               // 屏幕/像素坐标
  Vec2f delta{};                  // 相对位移(move 用)
  std::int32_t button = 0;        // 0=左键/主触点
  std::uint32_t width = 0;        // Resize 用
  std::uint32_t height = 0;       // Resize 用
  std::int64_t timestamp_us = 0;  // 由录制方给定,SDK 内绝不改写
  bool handled = false;

  bool operator==(const InputEvent& o) const {
    return kind == o.kind && pointer_id == o.pointer_id && position.x == o.position.x &&
           position.y == o.position.y && delta.x == o.delta.x && delta.y == o.delta.y &&
           button == o.button && width == o.width && height == o.height &&
           timestamp_us == o.timestamp_us && handled == o.handled;
  }
  bool operator!=(const InputEvent& o) const { return !(*this == o); }
};

// 录制用的事件追加器 + 回放游标。
class EventStream {
 public:
  void append(const InputEvent& e) { events_.push_back(e); }

  std::size_t size() const { return events_.size(); }
  const std::vector<InputEvent>& events() const { return events_; }

  // 回放:把游标回到起点。
  void rewind() { cursor_ = 0; }

  // 取下一条;到头返回 false(调用方据此结束回放)。
  bool next(InputEvent* out) {
    if (cursor_ >= events_.size()) return false;
    if (out) *out = events_[cursor_];
    ++cursor_;
    return true;
  }

  std::size_t cursor() const { return cursor_; }
  std::size_t pending() const { return events_.size() - cursor_; }

 private:
  std::vector<InputEvent> events_;
  std::size_t cursor_ = 0;
};

}  // namespace magicblocks::device
