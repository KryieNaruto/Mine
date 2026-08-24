#pragma once

#include <vector>

namespace easypainter::stroke {

// 2D 点/向量(平台无关,与 ink::stroke_model::Vec2 互转)。
struct Vec2 {
  float x = 0.f;
  float y = 0.f;
};

// 输入事件类型:首点 kDown、末点 kUp、中间 kMove。
enum class InputType { kDown, kMove, kUp };

// 一个采样输入事件。
struct InputEvent {
  InputType type = InputType::kMove;
  Vec2 pos{};
  float time_s = 0.f;  // 相对笔画起始的时间(秒)
};

}  // namespace easypainter::stroke
