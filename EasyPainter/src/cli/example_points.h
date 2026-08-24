#pragma once

#include <vector>

#include "core/stroke/types.h"

namespace easypainter::cli {

// 内置示例点(已归一化到 [0,1]×[0,1]):一条 S 形笔画。
// 零参数 CLI 与图像 golden 共用,保证确定性;Normalize 对它为幂等无操作。
inline std::vector<stroke::Vec2> example_points() {
  return {
      {0.0f, 1.0f},    {0.125f, 0.822f}, {0.25f, 0.511f}, {0.375f, 0.222f},
      {0.5f, 0.0f},    {0.625f, 0.222f}, {0.75f, 0.511f}, {0.875f, 0.822f},
      {1.0f, 1.0f},
  };
}

}  // namespace easypainter::cli
