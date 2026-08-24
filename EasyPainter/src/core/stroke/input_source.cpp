#include "core/stroke/input_source.h"

namespace easypainter::stroke {

// 固定时间步长(秒):与 windowed 采集速率/CLI 内置示例一致。
constexpr float kTimeStepS = 0.05f;

std::vector<InputEvent> build_events(const std::vector<Vec2>& pts) {
  std::vector<InputEvent> events;
  if (pts.empty()) return events;
  events.reserve(pts.size());
  for (size_t i = 0; i < pts.size(); ++i) {
    InputType t = (i == 0)             ? InputType::kDown
                  : (i + 1 == pts.size()) ? InputType::kUp
                                          : InputType::kMove;
    events.push_back(
        InputEvent{t, pts[i], static_cast<float>(i) * kTimeStepS});
  }
  return events;
}

}  // namespace easypainter::stroke
