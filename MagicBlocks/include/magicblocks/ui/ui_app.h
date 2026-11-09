// MagicBlocks UI 消费层 —— 对外唯一入口（spec §2、§4）
//
// 上层（App Shell：Activity / NativeActivity / 桌面壳）只 include 这一个头，
// 不需要知道 Router / HUD / InputRouter / SceneFrameDriver 之间怎么连。
//
// ============================================================================
// 路线变更（必读）
// ============================================================================
//
// 旧版 `UiApp` 把「场景 pass + UI pass」录进同一 command list 再上屏。
// 调研 v2 §7 判定该路线作废：**UI 合成交给壳层**，SDK 只负责渲染 3D 场景。
//
// 因此 `UiApp::run_frame()` 的职责变成：
//
//   1. 路由推进（当前玩法 on_update）
//   2. HUD 产出**浮层图元** → 交给 `IOverlay2D`（画在场景上，唯一通道）
//   3. PerfGovernor 判预算 / 档位
//   4. SceneFrameDriver 把**场景命令列表**交给壳层的 `ISceneFrameSink`
//   5. 统计回报（帧时间分桶、drawcall、输入延迟）
//
// 注意第 2 步：HUD 的图元**不再**进命令列表做 UI pass，而是走 overlay 通道。
// 这是 iOS 上"Compose 永远在 3D 之上"这一硬约束要求的（见调研 v2 §4）。
//
// 第 4 步**不做任何 CPU 回读**（门控 check_ui_no_readback.py）。
#pragma once

#include <cstdint>
#include <memory>
#include <string>

#include "magicblocks/device/lifecycle.h"
#include "magicblocks/resources/command.h"
#include "magicblocks/ui/frame_renderer.h"
#include "magicblocks/ui/hud.h"
#include "magicblocks/ui/input.h"
#include "magicblocks/ui/overlay.h"
#include "magicblocks/ui/perf.h"
#include "magicblocks/ui/router.h"
#include "magicblocks/ui/scene_frame.h"
#include "magicblocks/ui/scene_stub.h"
#include "magicblocks/ui/types.h"

namespace magicblocks::ui {

// 组装所需的一切。全部**外部拥有**，UiApp 不析构它们（析构顺序由壳决定）。
struct UiAppDeps {
  // 壳层提供的场景出口（Android SurfaceView / iOS CAMetalLayer / 桌面）。
  ISceneFrameSink* scene_sink = nullptr;
  // SDK 侧录好的一帧场景命令列表（由玩法/渲染特性填充）。
  resources::ICommandList* scene_command_list = nullptr;
  // 浮层通道：画在场景之上的 UI 图元（HUD、笔刷轨迹、gizmo）。
  stub::IOverlay2D* overlay = nullptr;
  IStateStore* state_store = nullptr;  // 可为 null
  stub::IScene* scene = nullptr;       // 可为 null（空 FeatureHost 骨架也能跑）
  stub::IRenderFeatureRegistry* render_features = nullptr;
};

struct UiAppConfig {
  DisplayInfo display{1280, 720, 1.f};
  TargetMode mode = TargetMode::Swapchain;
  SceneFrameConfig scene_frame{};
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

  // 初始化：装配 Router / HUD / InputRouter / SceneFrameDriver。
  bool init(const UiAppConfig& cfg, const UiAppDeps& deps, std::string* error);

  // 编译期注册一个玩法（Router 只按 ID 查表，UI 层不含玩法名）。
  void register_feature(FeatureRegistration reg);

  // 输入：原始事件先进 InputRouter（§7 仲裁），再进 HUD / Router 的对应出口。
  // HUD 按钮点击最终经由 Router::callback() 触发 push/pop —— 这是**唯一入口**。
  void on_input(const device::InputEvent& ev);

  // 生命周期：SurfaceChanged → 壳层 rebuild；Paused → 暂停提交。
  void on_lifecycle(const device::AppLifecycleEvent& ev);

  // 跑一帧。timing 由外部注入（UI 层不内建时钟，保证命令复现可确定性）。
  SceneFrameResult run_frame(f32 scene_ms, f32 ui_ms);

  // 子系统访问（测试与调试用；正常运行由 UiApp 内部驱动）。
  Router& router();
  Hud& hud();
  InputRouter& input();
  SceneFrameDriver& scene_frame();
  PerfGovernor& governor();

  // 统计面板文本（G8）。
  std::string debug_panel() const;

 private:
  // 内部扩展接口的取用点（`src/ui/ui_app_internal.h` 里的 `as_internal()` 经由
  // `UiAppAccess` 访问）。只声明 friend，不把任何类型暴露给公开面的用户；
  // `UiApp` 本身依然不是多态类型（不能被继承替换）。
  friend struct UiAppAccess;

  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace magicblocks::ui
