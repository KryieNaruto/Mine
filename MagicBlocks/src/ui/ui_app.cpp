// MagicBlocks UI 消费层 —— 顶层装配实现(spec §2、§4、§5)
//
// 本文件是 UI 层**唯一的串联点**:Router / Hud / InputRouter / PerfGovernor /
// SceneFrameDriver 各自独立,在这里按下面的顺序装配起来。
//
// ============================================================================
// 路线变更(必读)
// ============================================================================
//
// 旧版把「场景 pass + UI pass」录进同一 command list 再上屏。调研 v2 §7 判定
// 该路线**作废**:UI 合成交给壳层,SDK 只负责把 3D 场景渲染到壳层给的 surface。
//
// 一次 `run_frame()` 的固定顺序:
//   1. 路由推进     router.update(dt)            —— 当前玩法推进
//   2. 浮层绘制     hud.draw(overlay)             —— 画在**场景之上**的 UI 图元
//   3. 预算判定     governor.report_frame(...)    —— 用**上一帧**的真值判档
//   4. 场景驱动     scene_frame_.run_frame()      —— acquire → record → submit → present
//   5. 统计回报     (drawcall / submit / 延迟)    —— 不产生任何阻塞
//
// 第 3 步刻意用上一帧样本:本帧的 GPU 时间此刻还不知道,拿不到就不编。
// 这是"UI 层不读时钟"纪律的具体形态(与 SDK `EventStream` 同一条纪律)。
//
// 第 4 步**不碰任何 CPU 回读**(门控 check_ui_no_readback.py):
//   场景命令列表由玩法/渲染特性在 SDK 侧录好,这里只把它交给壳层的 sink。
#include "magicblocks/ui/ui_app.h"

#include <algorithm>
#include <cstring>
#include <new>
#include <string>
#include <utility>

#include "overlay_impl.h"
#include "scene_sink_host.h"
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
  SceneFrameDriver scene_frame_;
  detail::Overlay2D overlay_;

  // 侧栏(UI 布局 v1)。条目数据来自 cfg.sidebar / set_sidebar(),UI 层不写死。
  SidebarIds sidebar_ids{};
  // 右列表下标 → 玩法 ID。条目文案与 ID 都由外部给,UI 层只做"点了就交给 Router"。
  std::vector<FeatureId> sidebar_routes;
  std::function<void()> back_action;

  // 默认状态存储。所有权在 Impl,但**析构点必须留在 router.cpp** ——
  // MemoryStateStore 的 pimpl 只在那里是完整类型,别的 TU 直接持有会让
  // unique_ptr 的删除器遇到不完整类型。所以这里只留裸指针 + 一个 release()。
  MemoryStateStore* default_store = new MemoryStateStore();

  FrameSample last_sample{};
  bool have_last_sample = false;
  u64 frame_counter = 0;
  bool initialized = false;

  // 浮层图元：每一帧产出一次。**不进**场景命令列表（UI 合成归壳层）。  //
  // 设计：内部累积器 `overlay_` 是**唯一**的图元产出点（离屏通道与
  // `as_internal()->overlay()` 都读它）；若壳层提供了 `IOverlay2D`，
  // 同一批图元再转发一份给壳层去合成。两者内容一致，只是消费方不同。
  void draw_overlay() {
    overlay_.begin(Rect{0.f, 0.f, static_cast<f32>(cfg.display.width),
                        static_cast<f32>(cfg.display.height)});
    hud_.draw(overlay_);
    overlay_.end();
    forward_overlay_to_shell();
  }

  // 把内部累积的图元转发给壳层的 IOverlay2D（若有）。
  void forward_overlay_to_shell() {
    if (!deps.overlay) return;
    deps.overlay->begin(Rect{0.f, 0.f, static_cast<f32>(cfg.display.width),
                             static_cast<f32>(cfg.display.height)});
    for (const auto& p : overlay_.primitives()) {
      const Rect r{p.a.x, p.a.y, p.b.x - p.a.x, p.b.y - p.a.y};
      switch (p.kind) {
        case detail::PrimitiveKind::Fill:
          deps.overlay->fill_rect(r, p.color);
          break;
        case detail::PrimitiveKind::Stroke:
          deps.overlay->stroke_rect(r, p.color);
          break;
        case detail::PrimitiveKind::Line: {
          stub::StrokeStyle style{};
          style.width_px = p.width;
          deps.overlay->line(p.a, p.b, p.color, style);
          break;
        }
      }
    }
    deps.overlay->end();
  }

  // UiAppInternal
  void draw_ui_for_current_target() override { draw_overlay(); }
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
  // 主路径少了 sink 就无法上屏 —— 明确报错,不静默退化成离屏(spec §3 方案 A 禁止)。
  // 注意:离屏模式(TargetMode::Offscreen)允许 sink 为空,由调用方自己接管。
  if (cfg.mode == TargetMode::Swapchain) {
    if (!deps.scene_sink) {
      if (error) *error = "UiApp::init: Swapchain 模式需要 scene_sink";
      return false;
    }
  }

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

  // UI 布局 v1:用外部注入的条目构建左右侧栏。
  // 条目为空也照常构建(空列表 + 面板 + 标题),不会退化成"没有面板"。
  d.sidebar_routes.assign(cfg.sidebar.right_items.size(), FeatureId{});
  SidebarConfig sidebar = cfg.sidebar;
  // 右列表的点击先落到"下标 → 路由"上;路由表由 bind_sidebar_route() 填。
  // 这里用捕获本 Impl 的 lambda,保证 set_sidebar 重建后依然有效。
  sidebar.on_right_item = [&d](std::uint32_t index) {
    if (index >= d.sidebar_routes.size()) return;
    const FeatureId& id = d.sidebar_routes[index];
    if (id.empty()) return;  // 没绑路由的条目:点击只做高亮,不路由
    d.router_.push(id);
  };
  d.sidebar_ids = build_sidebar_hud(&d.hud_, cfg.hud, cfg.display, sidebar);
  d.input_router_.set_hud(&d.hud_);  // 重建控件树后重新指向同一实例(指针未变,防御性)
  // 返回按钮:默认经 Router 回 HomeScreen(唯一入口)
  if (auto* back = d.hud_.find(d.sidebar_ids.back_button)) {
    back->on_click = [&d]() {
      if (d.back_action) {
        d.back_action();
      } else {
        d.router_.pop();
      }
    };
  }

  // 场景帧驱动:配置在构造期定死(in_flight 属不可变决策),
  // 所以这里用最直白的做法 —— 换一个新的实例。
  d.scene_frame_.~SceneFrameDriver();
  new (&d.scene_frame_) SceneFrameDriver(cfg.scene_frame);
  d.scene_frame_.set_sink(deps.scene_sink);
  d.scene_frame_.set_command_list(deps.scene_command_list);

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

void UiApp::set_sidebar(const SidebarConfig& sidebar, std::string* error) {
  Impl& d = *impl_;
  if (!d.initialized) {
    if (error) *error = "set_sidebar: UiApp 尚未 init";
    return;
  }
  d.cfg.sidebar = sidebar;
  d.sidebar_routes.assign(sidebar.right_items.size(), FeatureId{});

  SidebarConfig s = sidebar;
  s.on_right_item = [&d](std::uint32_t index) {
    if (index >= d.sidebar_routes.size()) return;
    const FeatureId& id = d.sidebar_routes[index];
    if (id.empty()) return;
    d.router_.push(id);
  };
  d.sidebar_ids = build_sidebar_hud(&d.hud_, d.cfg.hud, d.cfg.display, s);
  if (auto* back = d.hud_.find(d.sidebar_ids.back_button)) {
    back->on_click = [&d]() {
      if (d.back_action) {
        d.back_action();
      } else {
        d.router_.pop();
      }
    };
  }
}

const SidebarIds& UiApp::sidebar_ids() const { return impl_->sidebar_ids; }

bool UiApp::bind_sidebar_route(std::uint32_t index, const FeatureId& feature_id) {
  Impl& d = *impl_;
  if (index >= d.sidebar_routes.size()) return false;
  d.sidebar_routes[index] = feature_id;
  return true;
}

void UiApp::set_back_action(std::function<void()> action) {
  Impl& d = *impl_;
  d.back_action = std::move(action);
}

void UiApp::on_input(const device::InputEvent& ev) {
  Impl& d = *impl_;
  if (!d.initialized) return;
  // G6:输入到达即登记"当前帧号"(不读时钟 —— 帧号就是时间基准)
  d.scene_frame_.mark_input(0);
  d.input_router_.route(ev);
}

void UiApp::on_lifecycle(const device::AppLifecycleEvent& ev) {
  Impl& d = *impl_;
  if (!d.initialized) return;

  // 驱动侧只跟前后台;surface 的创建/重建归壳层(它才是 ISceneFrameSink 的实现方)。
  d.scene_frame_.on_lifecycle(ev);

  switch (ev.kind) {
    case device::LifecycleKind::SurfaceChanged:
      // 旋转/缩放:改尺寸 + 侧栏按新尺寸重排 + 交换链重建(不是每帧重建,spec §5.4)
      if (ev.width && ev.height) {
        d.cfg.display.width = ev.width;
        d.cfg.display.height = ev.height;
        // 侧栏几何依赖屏幕尺寸 → 用同一份条目数据重建(路由绑定随后恢复)
        const std::vector<FeatureId> routes = d.sidebar_routes;
        std::string err;
        set_sidebar(d.cfg.sidebar, &err);
        if (d.sidebar_routes.size() == routes.size()) d.sidebar_routes = routes;
        // 若壳层用的是我们的桌面参考 sink,顺手帮它重建;移动端壳层自己处理。
        if (auto* host = dynamic_cast<ShellSurfaceSink*>(d.deps.scene_sink)) {
          host->rebuild(device::Extent2D{ev.width, ev.height}, &err);
        }
      }
      break;
    case device::LifecycleKind::Paused:
    case device::LifecycleKind::SurfaceDestroyed:
    case device::LifecycleKind::SurfaceCreated:
    case device::LifecycleKind::Resumed:
    case device::LifecycleKind::Unknown:
      break;
  }
}

SceneFrameResult UiApp::run_frame(f32 scene_ms, f32 ui_ms) {
  Impl& d = *impl_;
  SceneFrameResult result{};
  if (!d.initialized) {
    result.status = FrameStatus::InvalidState;
    return result;
  }

  // 1) 路由推进(HomeScreen 下是空操作)
  d.router_.update(1.f / 60.f);

  // 2) 浮层绘制 → 图元。**不进场景命令列表**:UI 合成由壳层完成(调研 v2 §7)。
  d.draw_overlay();
  const stub::IOverlay2D::Stats ov_stats = d.deps.overlay ? d.deps.overlay->stats() : stub::IOverlay2D::Stats{};

  // 3) 预算判定:用上一帧的真值(本帧 GPU 时间此刻未知,不编造)
  if (d.have_last_sample) d.governor_.report_frame(d.last_sample);

  // 4) 场景帧:acquire → record → submit → present(每帧恰好一次提交)
  result = d.scene_frame_.run_frame();

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
  s.present_queue_depth = d.scene_frame_.frames_in_flight();
  s.input_latency_frames = result.input_latency_frames;
  s.ui_bytes_touched = s.vertices * 16u;  // 每顶点 16B;UI 太贵时这里会先涨
  d.last_sample = s;
  d.have_last_sample = true;

  d.input_router_.end_frame();
  return result;
}

Router& UiApp::router() { return impl_->router_; }
Hud& UiApp::hud() { return impl_->hud_; }
InputRouter& UiApp::input() { return impl_->input_router_; }
SceneFrameDriver& UiApp::scene_frame() { return impl_->scene_frame_; }
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
  out += "scene_frames=" + std::to_string(d.scene_frame_.history().size()) +
         " last=" + to_string(d.scene_frame_.last().status) + "\n";
  return out;
}

}  // namespace magicblocks::ui
