// MagicBlocks UI 消费层 —— 顶层装配实现(spec §2、§4、§5)
//
// 本文件是 UI 层**唯一的串联点**:Router / Hud / InputRouter / PerfGovernor /
// Presenter 各自独立,在这里按 spec §5.2 的帧顺序装配起来。
//
// 一次 `run_frame()` 的固定顺序(spec §5.2 的 1~6 步):
//   1. 路由推进     router.update(dt)            —— 当前玩法推进
//   2. HUD 绘制     hud.draw(overlay)             —— UI pass 的内容(图元)
//   3. 预算判定     governor.report_frame(...)    —— 用**上一帧**的真值判档
//   4. Presenter    run_frame()                   —— wait → acquire → record → submit → present
//   5. 统计回报     (drawcall / submit / 延迟)    —— 不产生任何阻塞
//
// 第 3 步刻意用上一帧样本:本帧的 GPU 时间此刻还不知道,拿不到就不编。
// 这是"UI 层不读时钟"纪律的具体形态(与 SDK `EventStream` 同一条纪律)。
#include "magicblocks/ui/ui_app.h"

#include <algorithm>
#include <cstring>
#include <string>
#include <new>
#include <utility>

#include "overlay_impl.h"
#include "presenter_host.h"
#include "ui_app_internal.h"

namespace magicblocks::ui {

// ---------------------------------------------------------------- UiApp::Impl

struct UiApp::Impl final : UiAppInternal {
  UiAppConfig cfg{};
  UiAppDeps deps{};

  Router router_;
  Hud hud_;
  InputRouter input_router_;
  PerfGovernor governor_;
  Presenter presenter_;
  detail::Overlay2D overlay_;

  // 默认状态存储。所有权在 Impl,但**析构点必须留在 router.cpp** ——
  // MemoryStateStore 的 pimpl 只在那里是完整类型,别的 TU 直接持有会让
  // unique_ptr 的删除器遇到不完整类型。所以这里只留裸指针 + 一个 release()。
  MemoryStateStore* default_store = new MemoryStateStore();

  FrameSample last_sample{};
  bool have_last_sample = false;
  u64 frame_counter = 0;
  bool initialized = false;

  void draw_ui() {
    overlay_.begin(Rect{0.f, 0.f, static_cast<f32>(cfg.display.width),
                        static_cast<f32>(cfg.display.height)});
    hud_.draw(overlay_);
    overlay_.end();
  }

  // UiAppInternal
  void draw_ui_for_current_target() override { draw_ui(); }
  const detail::Overlay2D& overlay() const override { return overlay_; }

  ~Impl() { delete default_store; }

};

// 内部接口的取用点(`UiApp` 的 friend)。只在这里出现,公开面拿不到它。
struct UiAppAccess {
  explicit UiAppAccess(UiApp* app) : owner(app) {}
  UiAppInternal* internal() const {
    return owner && owner->impl_ ? static_cast<UiAppInternal*>(owner->impl_.get()) : nullptr;
  }

  UiApp* owner;
};

UiAppInternal* as_internal(UiApp* app) {
  if (!app) return nullptr;
  UiAppAccess access(app);
  return access.internal();
}

// ---------------------------------------------------------------- UiApp

UiApp::UiApp() : impl_(std::make_unique<Impl>()) {}
UiApp::~UiApp() = default;

bool UiApp::init(const UiAppConfig& cfg, const UiAppDeps& deps, std::string* error) {
  Impl& d = *impl_;
  if (cfg.display.width == 0 || cfg.display.height == 0) {
    if (error) *error = "UiApp::init: display 宽高必须 > 0";
    return false;
  }
  // 主路径少了任何一件都无法上屏 —— 明确报错,不静默退化成离屏(spec §3 方案 A 禁止)。
  if (cfg.mode == TargetMode::Swapchain) {
    if (!deps.present_surface) {
      if (error) *error = "UiApp::init: Swapchain 模式需要 present_surface";
      return false;
    }
    if (!deps.frame_renderer) {
      if (error) *error = "UiApp::init: Swapchain 模式需要 frame_renderer";
      return false;
    }
  }

  // 重复 init:各子系统各有 reset()/set_config(),就地复位而不是整体赋值
  //(Presenter / PerfGovernor 都不可拷贝赋值 —— 各自握着 pimpl 状态机)。
  d.cfg = cfg;
  d.deps = deps;

  // 状态存储:外部给了就用外部的(壳层可换成异步落盘实现),否则用内存实现
  d.router_.set_state_store(deps.state_store ? deps.state_store : d.default_store);

  // 特性上下文:玩法只拿得到 SDK 场景 + 渲染特性注册表
  stub::FeatureContext ctx{};
  ctx.scene = deps.scene;
  ctx.render_features = deps.render_features;
  ctx.display = cfg.display;
  d.router_.set_context(ctx);

  build_minimal_hud(&d.hud_, cfg.hud, cfg.display);
  d.input_router_.set_config(cfg.input);
  d.input_router_.set_hud(&d.hud_);

  // 上屏链路:配置在构造期定死(present_mode / in_flight 属不可变决策),
  // 所以这里用 placement 之外最直白的做法 —— 换一个新的 Presenter 实例。
  d.presenter_.~Presenter();
  new (&d.presenter_) Presenter(cfg.presenter);
  d.presenter_.set_surface(deps.swapchain, deps.present_surface);
  d.presenter_.set_renderer(deps.frame_renderer);
  d.presenter_.set_offscreen_target(deps.offscreen_target);

  // 性能治理器:阈值属可变配置,允许重复 init 换一套。
  d.governor_.set_config(cfg.governor);
  d.governor_.reset();

  // 先按新配置产出一帧图元,保证 overlay 处于已 begin/end 的稳态。
  d.draw_ui_for_current_target();
  d.initialized = true;
  return true;
}

void UiApp::register_feature(FeatureRegistration reg) {
  impl_->router_.register_feature(std::move(reg));
}

void UiApp::on_input(const device::InputEvent& ev) {
  Impl& d = *impl_;
  if (!d.initialized) return;
  // G6:输入到达即登记"当前帧号"(不读时钟 —— 帧号就是时间基准)
  d.presenter_.mark_input(0);
  d.input_router_.route(ev);
}

void UiApp::on_lifecycle(const device::AppLifecycleEvent& ev) {
  Impl& d = *impl_;
  if (!d.initialized) return;
  switch (ev.kind) {
    case device::LifecycleKind::Paused:
    case device::LifecycleKind::SurfaceDestroyed:
      // G7:暂停 present,CPU 侧状态(场景/玩法/状态存储)全部保留,唤醒不出黑屏
      d.presenter_.set_active(false);
      break;
    case device::LifecycleKind::SurfaceCreated:
    case device::LifecycleKind::Resumed:
      d.presenter_.set_active(true);
      break;
    case device::LifecycleKind::SurfaceChanged:
      // 旋转/缩放:改尺寸 + HUD 重排 + 交换链重建(不是每帧重建,spec §5.4)
      if (ev.width && ev.height) {
        d.cfg.display.width = ev.width;
        d.cfg.display.height = ev.height;
        d.hud_.relayout(d.cfg.hud, d.cfg.display, 44.f, 64.f);
        if (auto* host = dynamic_cast<PresenterHost*>(d.deps.present_surface)) {
          std::string err;
          host->rebuild(device::Extent2D{ev.width, ev.height}, &err);
        }
      }
      break;
    case device::LifecycleKind::Unknown:
      break;
  }
}

FrameResult UiApp::run_frame(f32 scene_ms, f32 ui_ms) {
  Impl& d = *impl_;
  FrameResult result{};
  if (!d.initialized) {
    result.status = PresenterStatus::InvalidState;
    return result;
  }

  // 1) 路由推进(HomeScreen 下是空操作)
  d.router_.update(1.f / 60.f);

  // 2) HUD 绘制 → 图元。图元是 UI pass 的**内容**,由 Presenter 在 record 阶段录进同一帧。
  d.draw_ui();
  const auto ov_stats = d.overlay_.stats();

  // 3) 预算判定:用上一帧的真值(本帧 GPU 时间此刻未知,不编造)
  if (d.have_last_sample) d.governor_.report_frame(d.last_sample);

  // 4) Presenter:wait → acquire → record → submit → present(整帧一次提交)
  result = d.presenter_.run_frame(scene_ms, ui_ms);

  // 5) 统计回报 —— 本帧真值成为下一帧的判据
  FrameSample s{};
  s.frame_index = d.frame_counter++;
  s.buckets = FrameBuckets{scene_ms, ui_ms, 0.f, 0.f};
  s.draw_calls = ov_stats.draw_calls;
  s.vertices = ov_stats.vertices;
  s.batches = ov_stats.batches;
  s.submit_count = result.submits_this_frame;
  // 稳态帧 0 次分配:图元容器与顶点缓冲都预分配(spec §6 G5 / SDK M5)
  s.vma_alloc_count = 0;
  s.present_queue_depth = d.presenter_.frames_in_flight();
  s.input_latency_frames = result.input_latency_frames;
  s.ui_bytes_touched = s.vertices * 16u;  // 每顶点 16B;UI 太贵时这里会先涨
  d.last_sample = s;
  d.have_last_sample = true;

  d.input_router_.end_frame();
  return result;
}

bool UiApp::render_offscreen_to_memory(u32 width, u32 height, std::uint8_t* out_rgba8,
                                       u32 row_bytes) {
  Impl& d = *impl_;
  if (!d.initialized || !out_rgba8 || !d.deps.frame_renderer) return false;
  if (row_bytes < width * 4u) return false;
  // 离屏路径(spec §5.5):共享同一个 IFRameRenderer 抽象,只是目标换成离屏。
  // 图元→GPU 实例的转换由后端完成;这里只负责产出一帧 UI 图元。
  d.draw_ui();
  return d.deps.frame_renderer->render_to_memory(width, height, out_rgba8, row_bytes);
}

Router& UiApp::router() { return impl_->router_; }
Hud& UiApp::hud() { return impl_->hud_; }
InputRouter& UiApp::input() { return impl_->input_router_; }
Presenter& UiApp::presenter() { return impl_->presenter_; }
PerfGovernor& UiApp::governor() { return impl_->governor_; }

std::string UiApp::debug_panel() const {
  const Impl& d = *impl_;
  std::string out = "== MagicBlocks UI ==\nfeature=";
  out += d.router_.current().empty() ? "HomeScreen" : d.router_.current().value;
  out += "\n";
  out += d.governor_.format_panel();
  const auto& ist = d.input_router_.stats();
  out += "events=" + std::to_string(ist.events_total) +
         " hud=" + std::to_string(ist.routed_hud) +
         " tool=" + std::to_string(ist.routed_tool) +
         " cam=" + std::to_string(ist.routed_camera) +
         " none=" + std::to_string(ist.unrouted) +
         " gestures=" + std::to_string(ist.gestures_emitted) + "\n";
  return out;
}

}  // namespace magicblocks::ui
