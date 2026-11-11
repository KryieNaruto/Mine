// MagicBlocks UI 消费层 —— 内部装配声明(只给 src/ui/ 与测试用)
//
// `UiApp` 的公开面(`include/magicblocks/ui/ui_app.h`)只给壳层用:
//   init / register_feature / on_input / on_lifecycle / run_frame
// 但离屏后端与测试需要两样公开面刻意**不暴露**的东西:
//   1. `draw_ui_for_current_target()` —— 离屏路径复用同一份 HUD 绘制代码;
//   2. `overlay()` —— 图元序列(离屏后端要把它转成 GPU 实例)。
//
// 把这两样留在 `src/ui/`:`include/` 里看不到它们,也就没人能拿它绕开 Presenter。
#pragma once

#include <cstdint>

#include "mb_internal/magicblocks/ui/ui_app.h"
#include "overlay_impl.h"

namespace magicblocks::ui {

// UiApp 的内部扩展接口(由 `UiApp::Impl` 实现)。
//
// 注意它挂在 **Impl** 上而不是 `UiApp` 上:公开面 `UiApp` 刻意不是多态类型
// (没有虚函数、不能继承),所以拿不到它 —— 想用内部接口只能 include 本私有头,
// 而本私有头不在 `include/` 里,壳层自然看不到。
class UiAppInternal {
 public:
  virtual ~UiAppInternal() = default;

  // 让 HUD 把本帧图元画进 overlay。**不推进帧号、不做提交**;
  // 离屏后端与主路径的渲染器都在"录制"阶段调用它。
  virtual void draw_ui_for_current_target() = 0;

  // 本帧图元序列(只读)。
  virtual const detail::Overlay2D& overlay() const = 0;
};

// 取内部接口(实现见 ui_app.cpp;内部按 Impl 自己的虚表取,不做 dynamic_cast)。
UiAppInternal* as_internal(UiApp* app);

}  // namespace magicblocks::ui
