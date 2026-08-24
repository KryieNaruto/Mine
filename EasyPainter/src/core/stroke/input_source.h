#pragma once

#include <vector>

#include "core/stroke/types.h"

namespace easypainter::stroke {

// 把采样点流转为输入事件序列:首点 kDown、末点 kUp、中间 kMove;time_s 固定步长递增。
std::vector<InputEvent> build_events(const std::vector<Vec2>& pts);

}  // namespace easypainter::stroke
