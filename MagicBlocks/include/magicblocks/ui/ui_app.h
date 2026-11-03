// MagicBlocks UI 消费层 —— 对外唯一入口(spec §2、§4)
//
// 上层(App Shell:Activity / NativeActivity / 桌面壳)只 include 这一个头,
// 不需要知道 Router/HUD/InputRouter/Presenter 之间怎么连 —— 那些是 UI 层的
// 内部装配,本头把它们按 spec §5.2 的帧顺序串起来。
//
// 一次 `UiApp::run_frame()` 的固定顺序(与 spec §5.2 的 1~6 步一一对应):
//
//   1. 路由推进(当前玩法 on_update)
//   2. HUD 绘制 → overlay 图元(UI pass 的内容)
//   3. PerfGovernor 判预算 / 档位
//   4. Presenter 走 wait → acquire → record(scene+UI) → submit → present
//   5. 统计回报(帧时间分桶、drawcall、VMA 计数、输入延迟)
//
// 关键是第 3、4 步**不做任何 CPU 回读**(spec §5.4 硬禁止):UI 层的绘制产物
// 是 overlay 图元,录进同一帧的命令缓冲,和场景一起进同一张 swapchain image。
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "magicblocks/device/events.h"
#include "magicblocks/device/lifecycle.h"
#include "magicblocks/ui/frame_renderer.h"
#include "magicblocks/ui/hud.h"
#include "magicblocks/ui/input.h"
#include "magicblocks/ui/perf.h"
#include "magicblocks/ui/presenter.h"
#include "magicblocks/ui/router.h"
#include "magicblocks/ui/types.h"

namespace magicblocks::ui {

// 组装所需的一切。全部**外部拥有**,UiApp 不析构它们(析构顺序由壳决定)。
struct UiAppDeps {
  device::ISwapchain* swapchain = nullptr;       // 可为 null(离屏模式)
  IPresentSurface* present_surface = nullptr;    // 主路径必需
  IFrameRenderer* frame_renderer = nullptr;      // 主路径必需
  device::IRenderTarget* offscreen_target = nullptr;  // Offscreen 模式用
  IStateStore* state_store = nullptr;            // 可为 null
  stub::IScene* scene = nullptr;                 // 可为 null(空 FeatureHost 骨架也能跑)
  stub::IRenderFeatureRegistry* render_features = nullptr;
};

struct UiAppConfig {
  DisplayInfo display{1280, 720, 1.f};
  TargetMode mode = TargetMode::Swapchain;
  PresenterConfig presenter{};
  GovConfig governor{};
  HudLayoutConfig hud{};
  InputRouterConfig input{};
  bool show_perf_panel = true;
};

class UiApp {
 public:
  UiApp();
  ~UiApp();

  UiApp(const UiApp&) = delete;
  UiApp& operator=(const UiApp&) = delete;

  // 初始化:装配 Router/HUD/InputRouter/Presenter。失败返回 false + message。
  bool init(const UiAppConfig& cfg, const UiAppDeps& deps, std::string* error);

  // 编译期注册一个玩法(Router 只按 ID 查表,UI 层不含玩法名)。
  void register_feature(FeatureRegistration reg);

  // 输入:原始事件先进 InputRouter(§7 仲裁),再进 HUD/Router 的对应出口。
  // HUD 按钮点击最终经由 Router::callback() 触发 push/pop —— 这是**唯一入口**。
  void on_input(const device::InputEvent& ev);

  // 生命周期:SurfaceChanged → rebuild(G7);SurfaceDestroyed/Paused → 暂停 present。
  void on_lifecycle(const device::AppLifecycleEvent& ev);

  // 跑一帧。timing 由外部注入(UI 层不内建时钟,保证命令复现可确定性)。
  FrameResult run_frame(f32 scene_ms, f32 ui_ms);

  // 离屏出图(spec §5.5):golden / CLI / 录制专用。**只有这条路径会读回**。
  bool render_offscreen_to_memory(u32 width, u32 height, std::uint8_t* out_rgba8,
                                  u32 row_bytes);

  // 子系统访问(测试与调试用;正常运行由 UiApp 内部驱动)。
  Router& router();
  Hud& hud();
  InputRouter& input();
  Presenter& presenter();
  PerfGovernor& governor();

  // 统计面板文本(G8)。
  std::string debug_panel() const;

 private:
  // 内部扩展接口的取用点(`src/ui/ui_app_internal.h` 里的 `as_internal()` 经由
  // `UiAppAccess` 访问)。只声明 friend,不把任何类型暴露给公开面的用户;
  // `UiApp` 本身依然不是多态类型(不能被继承替换)。
  friend struct UiAppAccess;

  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace magicblocks::ui
