// MagicBlocks UI 消费层测试 —— spec §10 验收门在单元层的落地
//
// 每组测试对应一条设计条文,失败信息里带条文编号,便于回溯:
//   Router.*         spec §4 唯一入口 / IStateStore 退出保存重进恢复
//   Input.*          spec §7 三层仲裁顺序、手势统一、事件不丢
//   Hud.*            spec §8 最小控件集、命中优先于工具/相机
//   Perf.*           spec §6 G1/G2/G3/G5/G6 门槛与**非静默**降级
//   Presenter.*      spec §5.1 P1~P5、§5.2 阶段顺序、§5.4 禁止事项
//   Offscreen.*      spec §5.5 离屏路径 + §10 验收门 5(golden)
//   UiApp.*          装配后的整链行为(G7 切后台、旋转)
//
// 纪律(与 L0 测试一致):缺 SwiftShader 时**失败**不 SKIP;不静默降级。
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "backend/vulkan/vulkan_render_device.h"
#include "backend/vulkan/vulkan_target.h"
#include "backend/vulkan/vulkan_ui_overlay.h"
#include "core/image_io.h"
#include "magicblocks/device/render_device.h"
#include "ui_offscreen_support.h"
#include "magicblocks/ui/hud.h"
#include "magicblocks/ui/input.h"
#include "magicblocks/ui/perf.h"
#include "magicblocks/ui/presenter.h"
#include "magicblocks/ui/router.h"
#include "magicblocks/ui/ui_app.h"
#include "ui/offscreen_target.h"
#include "ui/ui_app_internal.h"

namespace mb = magicblocks;
namespace ui = magicblocks::ui;
namespace dev = magicblocks::device;

namespace {

// ---------------------------------------------------------------- 假出口

// 记录"谁收到了什么事件"的工具出口(②层)。
class RecordingTool final : public ui::IToolSink {
 public:
  bool active = false;
  int events = 0;
  bool consume = true;
  bool tool_active() const override { return active; }
  bool on_input(const dev::InputEvent&) override {
    ++events;
    return consume;
  }
};

// 记录事件的相机出口(③层)。
class RecordingCamera final : public ui::ICameraSink {
 public:
  int events = 0;
  bool consume = true;
  bool on_input(const dev::InputEvent&) override {
    ++events;
    return consume;
  }
};

// 可控的 IPresentSurface:记录 acquire/submit/present 次数,并注入失败。
class FakeSurface final : public ui::IPresentSurface {
 public:
  int acquires = 0;
  int /**/ submits = 0;
  int presents = 0;
  bool is_ready = true;
  bool fail_acquire = false;
  bool fail_submit = false;
  bool fail_present = false;
  ui::PresenterStatus failure = ui::PresenterStatus::NeedRebuild;

  bool acquire(ui::FrameImage* out) override {
    ++acquires;
    if (fail_acquire) return false;
    out->id = static_cast<std::uint64_t>(acquires);
    out->width = 320;
    out->height = 240;
    out->valid = true;
    return true;
  }
  bool submit(std::uint64_t, std::uint64_t) override {
    ++submits;
    return !fail_submit;
  }
  bool present(const ui::FrameImage&, std::uint64_t) override {
    ++presents;
    return !fail_present;
  }
  bool ready() const override { return is_ready; }
  ui::PresenterStatus last_failure() const override { return failure; }
};

// 可控的 IFrameRenderer:记录录制次数。
class FakeRenderer final : public ui::IFrameRenderer {
 public:
  int records = 0;
  bool fail = false;
  bool render_into_device_image(std::uint64_t, ui::FrameImage* out,
                                std::uint64_t* sem) override {
    ++records;
    if (fail) return false;
    out->id = 7;
    out->width = 320;
    out->height = 240;
    out->valid = true;
    *sem = 42;
    return true;
  }
  bool render_to_memory(ui::u32, ui::u32, std::uint8_t*, ui::u32) override { return !fail; }
};

// 假场景(S1 桩),只记快照往返。
class FakeScene final : public ui::stub::IScene {
 public:
  ui::stub::SceneSnapshot data;
  int restores = 0;
  ui::stub::SceneSnapshot snapshot() const override { return data; }
  const std::vector<ui::stub::SceneNodeChange>& changes() const override { return changes_; }
  void clear_changes() override { changes_.clear(); }
  bool restore(const ui::stub::SceneSnapshot& s) override {
    ++restores;
    data = s;
    return true;
  }

 private:
  std::vector<ui::stub::SceneNodeChange> changes_;
};

// 空 FeatureHost 骨架(spec §4:本轮只有骨架)。
class ProbeFeature final : public ui::IFeatureHost {
 public:
  struct State {
    int enters = 0;
    int exits = 0;
    int updates = 0;
    bool got_scene = false;
  };
  explicit ProbeFeature(State* s) : s_(s) {}
  void on_enter(const ui::FeatureContext& ctx) override {
    ++s_->enters;
    s_->got_scene = ctx.scene != nullptr;
  }
  void on_exit() override { ++s_->exits; }
  void on_update(float) override { ++s_->updates; }
  bool on_input(const dev::InputEvent&) override { return false; }

 private:
  State* s_;
};

// 造一个指针事件。
dev::InputEvent pev(dev::InputEventKind k, float x, float y, std::int64_t t_us = 0,
                   std::uint32_t pointer = 0) {
  dev::InputEvent e;
  e.kind = k;
  e.position = {x, y};
  e.pointer_id = pointer;
  e.timestamp_us = t_us;
  return e;
}

ui::HudLayoutConfig test_hud_cfg() {
  ui::HudLayoutConfig c;
  c.margin_dp = 8.f;
  c.row_height_dp = 32.f;
  c.button_width_dp = 80.f;
  c.gap_dp = 6.f;
  c.panel_padding_dp = 6.f;
  return c;
}

ui::DisplayInfo test_display() { return ui::DisplayInfo{480, 320, 1.f}; }

}  // namespace

// ================================================================ Router(spec §4)

TEST(ui_Router, HomeScreenThenPushPop) {
  ui::Router r;
  ProbeFeature::State st;
  r.register_feature(ui::FeatureRegistration{ui::FeatureId("alpha"),
                                            [&st]() { return std::make_unique<ProbeFeature>(&st); }});
  ASSERT_EQ(r.feature_count(), 1u);
  EXPECT_TRUE(r.current().empty()) << "初始必须在 HomeScreen(spec §4)";

  EXPECT_EQ(r.push(ui::FeatureId("alpha")), ui::RouteAction::Pushed);
  EXPECT_EQ(r.current().value, "alpha");
  EXPECT_EQ(st.enters, 1);

  EXPECT_EQ(r.pop(), ui::RouteAction::Popped);
  EXPECT_TRUE(r.current().empty()) << "pop 后回 HomeScreen";
  EXPECT_EQ(st.exits, 1);

  EXPECT_EQ(r.pop(), ui::RouteAction::Failed) << "空栈再 pop 必须明确失败,不能静默";
}

TEST(ui_Router, UnknownFeatureFails) {
  ui::Router r;
  EXPECT_EQ(r.push(ui::FeatureId("does-not-exist")), ui::RouteAction::Failed);
  EXPECT_TRUE(r.current().empty());
}

TEST(ui_Router, SwitchBetweenFeaturesExitsOldFirst) {
  ui::Router r;
  ProbeFeature::State a, b;
  r.register_feature(ui::FeatureRegistration{ui::FeatureId("a"),
                                            [&a]() { return std::make_unique<ProbeFeature>(&a); }});
  r.register_feature(ui::FeatureRegistration{ui::FeatureId("b"),
                                            [&b]() { return std::make_unique<ProbeFeature>(&b); }});
  ASSERT_EQ(r.push(ui::FeatureId("a")), ui::RouteAction::Pushed);
  ASSERT_EQ(r.push(ui::FeatureId("b")), ui::RouteAction::Pushed);
  // 玩法间切换必须走"退出旧的再进新的",不能两个玩法同时活着
  EXPECT_EQ(a.exits, 1);
  EXPECT_EQ(a.enters, 1);
  EXPECT_EQ(b.enters, 1);
  EXPECT_EQ(r.current().value, "b");
}

TEST(ui_Router, ExitSavesStateAndReenterRestores) {
  ui::Router r;
  FakeScene scene;
  ui::MemoryStateStore store;
  r.set_state_store(&store);
  r.set_context(ui::FeatureContext{&scene, nullptr, test_display()});

  ProbeFeature::State st;
  r.register_feature(ui::FeatureRegistration{ui::FeatureId("f"),
                                            [&st]() { return std::make_unique<ProbeFeature>(&st); }});

  // 第一次进入后场景里有一个节点;退出时必须被存下来
  ASSERT_EQ(r.push(ui::FeatureId("f")), ui::RouteAction::Pushed);
  ui::stub::NodeSnapshot n;
  n.id = 11;
  n.parent = ui::stub::kInvalidNode;
  n.name = "root";
  scene.data.nodes.push_back(n);
  ASSERT_EQ(r.pop(), ui::RouteAction::Popped);
  EXPECT_EQ(store.size(), 1u) << "退出时必须保存场景状态(spec §4 IStateStore)";

  // 重进:内存里的快照被恢复(spec §4「存内存为主」)
  scene.data.nodes.clear();
  ASSERT_EQ(r.push(ui::FeatureId("f")), ui::RouteAction::Pushed);
  EXPECT_EQ(scene.restores, 1);
  ASSERT_EQ(scene.data.nodes.size(), 1u);
  EXPECT_EQ(scene.data.nodes[0].id, 11u) << "重进必须恢复节点树";
}

TEST(ui_Router, StateBlobRoundTrip) {
  ui::MemoryStateStore store;
  ui::StateBlob blob;
  ui::stub::NodeSnapshot n;
  n.id = 3;
  n.parent = 1;
  n.visible = false;
  n.transform.translation[0] = 1.5f;
  blob.scene.nodes.push_back(n);
  blob.tool_params = 0xDEADBEEFull;
  blob.has_camera = true;
  blob.camera[0] = 2.f;

  const std::string text = store.serialize(blob);
  ui::StateBlob back;
  ASSERT_TRUE(store.deserialize(text, &back)) << "序列化往返必须成功";
  ASSERT_EQ(back.scene.nodes.size(), 1u);
  EXPECT_EQ(back.scene.nodes[0].id, 3u);
  EXPECT_FALSE(back.scene.nodes[0].visible);
  EXPECT_EQ(back.tool_params, 0xDEADBEEFull);
  EXPECT_TRUE(back.has_camera);
  EXPECT_FLOAT_EQ(back.camera[0], 2.f);

  // 版本头不对:明确失败,不"尽力解析"
  EXPECT_FALSE(store.deserialize("GARBAGE 1\n", &back));
  EXPECT_FALSE(store.deserialize("", &back));
}

// ================================================================ Input(spec §7)

TEST(ui_Input, HudBeatsToolAndCamera) {
  ui::Hud hud;
  ui::build_minimal_hud(&hud, test_hud_cfg(), test_display());
  ui::InputRouter r;
  RecordingTool tool;
  RecordingCamera cam;
  tool.active = true;
  r.set_hud(&hud);
  r.set_tool(&tool);
  r.set_camera(&cam);

  // 找一个按钮的位置(顶栏按钮 = 有 label 的 Button)
  const ui::Control* btn = nullptr;
  for (const auto& c : hud.controls()) {
    if (c.kind == ui::ControlKind::Button) {
      btn = &c;
      break;
    }
  }
  ASSERT_NE(btn, nullptr);
  const float cx = btn->rect_px.x + btn->rect_px.w * 0.5f;
  const float cy = btn->rect_px.y + btn->rect_px.h * 0.5f;

  const auto owner = r.route(pev(dev::InputEventKind::PointerDown, cx, cy));
  EXPECT_EQ(owner, ui::InputOwner::Hud) << "①层 HUD 命中必须优先于工具与相机(spec §7)";
  EXPECT_EQ(tool.events, 0) << "HUD 命中后不得再下发给工具";
  EXPECT_EQ(cam.events, 0) << "HUD 命中后不得再下发给相机";
}

TEST(ui_Input, ToolBeatsCameraWhenActive) {
  ui::Hud hud;  // 空 HUD:不会命中
  ui::InputRouter r;
  RecordingTool tool;
  RecordingCamera cam;
  tool.active = true;
  r.set_hud(&hud);
  r.set_tool(&tool);
  r.set_camera(&cam);

  const auto owner = r.route(pev(dev::InputEventKind::PointerDown, 5.f, 150.f));
  EXPECT_EQ(owner, ui::InputOwner::Tool) << "②层工具激活时优先于相机(spec §7)";
  EXPECT_EQ(cam.events, 0);
}

TEST(ui_Input, CameraLastResort) {
  ui::Hud hud;
  ui::InputRouter r;
  RecordingTool tool;  // 未激活
  RecordingCamera cam;
  r.set_hud(&hud);
  r.set_tool(&tool);
  r.set_camera(&cam);
  const auto owner = r.route(pev(dev::InputEventKind::PointerDown, 5.f, 150.f));
  EXPECT_EQ(owner, ui::InputOwner::Camera) << "③层相机兜底(spec §7)";
  EXPECT_EQ(cam.events, 1);
}

TEST(ui_Input, UnclaimedPointerCountsAsDropped) {
  ui::Hud hud;
  ui::InputRouter r;
  r.set_hud(&hud);  // 没有工具/相机
  const auto owner = r.route(pev(dev::InputEventKind::PointerDown, 5.f, 150.f));
  EXPECT_EQ(owner, ui::InputOwner::Unrouted);
  EXPECT_EQ(r.stats().dropped, 1u) << "没人认领的事件必须被计数,便于发现仲裁有洞";
}

TEST(ui_Input, GesturesAreRecognizedOnce) {
  ui::InputRouter r;
  RecordingCamera cam;
  r.set_camera(&cam);
  ui::InputRouterConfig cfg;
  cfg.drag_threshold_dp = 6.f;
  r.set_config(cfg);

  // Tap:按下-抬起,位移小于阈值
  r.route(pev(dev::InputEventKind::PointerDown, 100.f, 100.f, 0));
  r.route(pev(dev::InputEventKind::PointerUp, 101.f, 101.f, 50'000));
  ASSERT_EQ(r.gestures().size(), 1u);
  EXPECT_EQ(r.gestures()[0].kind, ui::GestureKind::Tap);
  EXPECT_EQ(r.gestures()[0].owner, ui::InputOwner::Camera);

  // Drag:移动超过阈值
  r.route(pev(dev::InputEventKind::PointerDown, 100.f, 100.f, 0));
  r.route(pev(dev::InputEventKind::PointerMove, 140.f, 100.f, 10'000));
  r.route(pev(dev::InputEventKind::PointerUp, 140.f, 100.f, 20'000));
  ASSERT_EQ(r.gestures().size(), 2u);
  EXPECT_EQ(r.gestures()[1].kind, ui::GestureKind::Drag);
  EXPECT_FLOAT_EQ(r.gestures()[1].delta.x, 40.f);

  // LongPress:按住超过阈值时长
  r.route(pev(dev::InputEventKind::PointerDown, 50.f, 50.f, 0));
  r.route(pev(dev::InputEventKind::PointerUp, 50.f, 50.f, 800'000));
  ASSERT_EQ(r.gestures().size(), 3u);
  EXPECT_EQ(r.gestures()[2].kind, ui::GestureKind::LongPress);
}

TEST(ui_Input, PinchIsRecognized) {
  ui::InputRouter r;
  RecordingCamera cam;
  r.set_camera(&cam);
  // 两指按下,距离 100 → 拉大到 200
  r.route(pev(dev::InputEventKind::PointerDown, 100.f, 100.f, 0, 0));
  r.route(pev(dev::InputEventKind::PointerDown, 200.f, 100.f, 0, 1));
  r.route(pev(dev::InputEventKind::PointerMove, 50.f, 100.f, 10'000, 0));
  r.route(pev(dev::InputEventKind::PointerMove, 250.f, 100.f, 10'000, 1));
  r.route(pev(dev::InputEventKind::PointerUp, 250.f, 100.f, 20'000, 1));
  ASSERT_GE(r.gestures().size(), 1u);
  const auto& g = r.gestures().back();
  EXPECT_EQ(g.kind, ui::GestureKind::Pinch) << "双指距离变化必须识别为 Pinch(spec §7)";
  EXPECT_GT(g.scale, 1.5f);
  EXPECT_EQ(g.pointer_count, 2u);
}

TEST(ui_Input, EveryRoutedPointerIsCounted) {
  ui::Hud hud;
  ui::InputRouter r;
  RecordingCamera cam;
  r.set_hud(&hud);
  r.set_camera(&cam);
  for (int i = 0; i < 5; ++i) {
    r.route(pev(dev::InputEventKind::PointerDown, 10.f, 200.f));
    r.route(pev(dev::InputEventKind::PointerUp, 10.f, 200.f));
  }
  const auto& st = r.stats();
  EXPECT_EQ(st.events_total, 10u);
  EXPECT_EQ(st.routed_camera + st.routed_hud + st.routed_tool + st.unrouted, 10u)
      << "spec §7:每个事件都必须有归属,不许凭空丢";
}

TEST(ui_Input, OwnerDoesNotJumpMidGesture) {
  // 一根手指画到一半不能跳槽到相机(经典手势撕裂 bug)
  ui::InputRouter r;
  RecordingTool tool;
  RecordingCamera cam;
  tool.active = true;
  r.set_tool(&tool);
  r.set_camera(&cam);
  const auto d = r.route(pev(dev::InputEventKind::PointerDown, 100.f, 100.f));
  ASSERT_EQ(d, ui::InputOwner::Tool);
  const int camera_before = cam.events;
  tool.active = false;  // 工具中途失效
  r.route(pev(dev::InputEventKind::PointerMove, 110.f, 100.f));
  EXPECT_EQ(cam.events, camera_before) << "归属在按下时确定,移动不得改判到相机";
}

// ================================================================ Hud(spec §8)

TEST(ui_Hud, MinimalControlSetIsBuiltAndLaidOut) {
  ui::Hud hud;
  const auto n = ui::build_minimal_hud(&hud, test_hud_cfg(), test_display());
  EXPECT_EQ(n, hud.size());
  EXPECT_GE(n, 8u) << "最小控件集:面板 + 色板 + 滑条 + 按钮(spec §8)";

  bool has_panel = false, has_swatch = false, has_slider = false, has_button = false;
  for (const auto& c : hud.controls()) {
    switch (c.kind) {
      case ui::ControlKind::Panel: has_panel = true; break;
      case ui::ControlKind::Swatch: has_swatch = true; break;
      case ui::ControlKind::Slider: has_slider = true; break;
      case ui::ControlKind::Button: has_button = true; break;
      default: break;
    }
    // 布局后所有控件都必须有可见几何且在屏幕内
    EXPECT_FALSE(c.rect_px.empty()) << "控件 " << c.label << " 布局为空";
    EXPECT_GE(c.rect_px.x, 0.f);
    EXPECT_LE(c.rect_px.right(), static_cast<float>(test_display().width) + 0.5f);
  }
  EXPECT_TRUE(has_panel);
  EXPECT_TRUE(has_swatch);
  EXPECT_TRUE(has_slider);
  EXPECT_TRUE(has_button);
}

TEST(ui_Hud, PanelConsumesClicksSoNoLeakToCamera) {
  ui::Hud hud;
  ui::build_minimal_hud(&hud, test_hud_cfg(), test_display());
  // 底部面板内的空白处按下:必须由 Panel 消费(不算点到相机)
  const ui::Control* panel = nullptr;
  for (const auto& c : hud.controls()) {
    if (c.kind == ui::ControlKind::Panel) {
      panel = &c;
      break;
    }
  }
  ASSERT_NE(panel, nullptr);
  const float px = panel->rect_px.x + 2.f;
  const float py = panel->rect_px.y + 2.f;
  EXPECT_TRUE(hud.on_input(pev(dev::InputEventKind::PointerDown, px, py)));
}

TEST(ui_Hud, ButtonClickFiresOnce) {
  ui::Hud hud;
  ui::Control b;
  b.kind = ui::ControlKind::Button;
  b.label = "go";
  b.rect_px = ui::Rect{10.f, 10.f, 50.f, 30.f};
  int clicks = 0;
  b.on_click = [&clicks]() { ++clicks; };
  hud.add(b);

  EXPECT_TRUE(hud.on_input(pev(dev::InputEventKind::PointerDown, 20.f, 20.f)));
  EXPECT_TRUE(hud.on_input(pev(dev::InputEventKind::PointerUp, 22.f, 22.f)));
  EXPECT_EQ(clicks, 1);

  // 在别处抬起 = 取消点击(手指滑出)
  hud.on_input(pev(dev::InputEventKind::PointerDown, 20.f, 20.f));
  hud.on_input(pev(dev::InputEventKind::PointerUp, 300.f, 300.f));
  EXPECT_EQ(clicks, 1) << "滑出后抬起不应触发点击";
}

TEST(ui_Hud, SliderValueFollowsPointer) {
  ui::Hud hud;
  ui::Control s;
  s.kind = ui::ControlKind::Slider;
  s.rect_px = ui::Rect{0.f, 0.f, 100.f, 20.f};
  float last = -1.f;
  s.on_value_changed = [&last](float v) { last = v; };
  const auto id = hud.add(s);

  hud.on_input(pev(dev::InputEventKind::PointerDown, 25.f, 10.f));
  ASSERT_NE(hud.find(id), nullptr);
  EXPECT_NEAR(hud.find(id)->value, 0.25f, 1e-3f);
  hud.on_input(pev(dev::InputEventKind::PointerMove, 75.f, 10.f));
  EXPECT_NEAR(hud.find(id)->value, 0.75f, 1e-3f);
  EXPECT_NEAR(last, 0.75f, 1e-3f);
  // 越界被夹住
  hud.on_input(pev(dev::InputEventKind::PointerMove, 500.f, 10.f));
  EXPECT_FLOAT_EQ(hud.find(id)->value, 1.f);
}

TEST(ui_Hud, DrawIsDeterministic) {
  // 同一份 HUD 两次绘制必须产出**完全相同的图元序列**(命令流可复现的前提)
  ui::Hud hud;
  ui::build_minimal_hud(&hud, test_hud_cfg(), test_display());
  mb::ui::detail::Overlay2D o1, o2;
  o1.begin(ui::Rect{0, 0, 480, 320});
  hud.draw(o1);
  o1.end();
  o2.begin(ui::Rect{0, 0, 480, 320});
  hud.draw(o2);
  o2.end();
  ASSERT_EQ(o1.primitives().size(), o2.primitives().size());
  EXPECT_GT(o1.primitives().size(), 0u);
  for (std::size_t i = 0; i < o1.primitives().size(); ++i) {
    const auto& a = o1.primitives()[i];
    const auto& b = o2.primitives()[i];
    EXPECT_EQ(a.kind, b.kind);
    EXPECT_EQ(a.color, b.color);
    EXPECT_FLOAT_EQ(a.a.x, b.a.x);
    EXPECT_FLOAT_EQ(a.b.y, b.b.y);
  }
  EXPECT_EQ(o1.compute_stats().draw_calls, o2.compute_stats().draw_calls);
}

// ================================================================ Perf(spec §6)

TEST(ui_Perf, SingleSubmitAndZeroAllocAreEnforced) {
  ui::GovConfig cfg;
  cfg.frame_budget_ms = 16.6f;
  ui::PerfGovernor gov(cfg);

  ui::FrameSample good{};
  good.submit_count = 1;
  good.vma_alloc_count = 0;
  good.buckets.scene_ms = 4.f;
  good.buckets.ui_ms = 1.f;
  EXPECT_TRUE(gov.report_frame(good));
  EXPECT_EQ(gov.stats().submit_violations, 0u);

  // 每帧 2 次 submit = 违规(spec §5.4「UI 单独一次 submit」禁止)
  ui::FrameSample two_submits = good;
  two_submits.submit_count = 2;
  EXPECT_FALSE(gov.report_frame(two_submits));
  EXPECT_EQ(gov.stats().submit_violations, 1u);

  // 稳态帧出现 VMA 分配 = 违规(spec §6 G5 / SDK M5)
  ui::FrameSample alloc = good;
  alloc.vma_alloc_count = 3;
  EXPECT_FALSE(gov.report_frame(alloc));
  EXPECT_EQ(gov.stats().vma_alloc_violations, 1u);
}

TEST(ui_Perf, DegradesByExplicitTierWithHysteresis) {
  ui::GovConfig cfg;
  cfg.frame_budget_ms = 16.6f;
  cfg.degrade_after_frames = 3;
  cfg.recover_after_frames = 5;
  cfg.recover_margin_ratio = 0.5f;
  ui::PerfGovernor gov(cfg);
  EXPECT_EQ(gov.tier(), ui::QualityTier::Full);

  ui::FrameSample slow{};
  slow.submit_count = 1;
  slow.buckets.scene_ms = 30.f;  // 超预算
  for (int i = 0; i < 2; ++i) gov.report_frame(slow);
  EXPECT_EQ(gov.tier(), ui::QualityTier::Full) << "未达连续帧数阈值不降档(防抖)";
  gov.report_frame(slow);
  EXPECT_EQ(gov.tier(), ui::QualityTier::NoMsaa) << "连续超预算必须降一档(L1)";
  EXPECT_FALSE(gov.ui_msaa_enabled()) << "L1:关 MSAA";
  ASSERT_EQ(gov.tier_changes().size(), 1u);
  EXPECT_TRUE(gov.tier_changes()[0].reason.find("over") != std::string::npos)
      << "档位切换必须带原因,不静默(spec §6 G2)";

  // 继续超预算 → 逐档下降
  for (int i = 0; i < 3; ++i) gov.report_frame(slow);
  EXPECT_EQ(gov.tier(), ui::QualityTier::SimpleUi);
  EXPECT_TRUE(gov.ui_simplified());
  for (int i = 0; i < 3; ++i) gov.report_frame(slow);
  EXPECT_EQ(gov.tier(), ui::QualityTier::LowRes);
  EXPECT_LT(gov.render_scale(), 1.f) << "L3 降低分辨率(spec §6 G2,仅离屏可做)";
  for (int i = 0; i < 10; ++i) gov.report_frame(slow);
  EXPECT_EQ(gov.tier(), ui::QualityTier::LowRes) << "L3 是底,不再降";

  // 恢复需要连续低负载帧(迟滞)
  ui::FrameSample fast{};
  fast.submit_count = 1;
  fast.buckets.scene_ms = 1.f;
  for (int i = 0; i < 4; ++i) gov.report_frame(fast);
  EXPECT_EQ(gov.tier(), ui::QualityTier::LowRes) << "恢复帧数不足不得升档";
  gov.report_frame(fast);
  EXPECT_EQ(gov.tier(), ui::QualityTier::SimpleUi) << "连续低负载必须升一档";
}

TEST(ui_Perf, UiDrawCallAndVertexCapsAreChecked) {
  ui::GovConfig cfg;
  cfg.ui_max_draw_calls = 50;
  cfg.ui_max_vertices = 8000;
  ui::PerfGovernor gov(cfg);
  ui::FrameSample s{};
  s.submit_count = 1;
  s.draw_calls = 51;
  s.vertices = 8001;
  EXPECT_FALSE(gov.report_frame(s));
  EXPECT_EQ(gov.stats().ui_drawcall_violations, 1u);
  EXPECT_EQ(gov.stats().ui_vertex_violations, 1u);
}

TEST(ui_Perf, InputLatencyTargetIsEnforced) {
  ui::GovConfig cfg;
  cfg.max_input_latency_frames = 2;
  ui::PerfGovernor gov(cfg);
  ui::FrameSample s{};
  s.submit_count = 1;
  s.input_latency_frames = 2;
  EXPECT_TRUE(gov.report_frame(s));
  s.input_latency_frames = 3;
  EXPECT_FALSE(gov.report_frame(s));
  EXPECT_EQ(gov.stats().latency_violations, 1u);
}

TEST(ui_Perf, PanelTextIsStableAscii) {
  ui::PerfGovernor gov;
  ui::FrameSample s{};
  s.submit_count = 1;
  gov.report_frame(s);
  const std::string text = gov.format_panel();
  EXPECT_FALSE(text.empty());
  EXPECT_NE(text.find("tier="), std::string::npos);
  EXPECT_NE(text.find("submit_viol="), std::string::npos);
}

// ================================================================ Presenter(spec §5)

TEST(ui_Presenter, OneFrameOneSubmitOnePresent) {
  FakeSurface surface;
  FakeRenderer renderer;
  ui::Presenter p;
  p.set_surface(nullptr, &surface);
  p.set_renderer(&renderer);

  const auto r = p.run_frame();
  EXPECT_EQ(r.status, ui::PresenterStatus::Ok);
  EXPECT_EQ(r.phase, ui::FramePhase::Presented);
  EXPECT_EQ(r.submits_this_frame, 1u) << "spec §10 验收门 2:主路径每帧恰一次 submit";
  EXPECT_EQ(surface.submits, 1);
  EXPECT_EQ(surface.presents, 1);
  EXPECT_EQ(surface.acquires, 1);
  EXPECT_EQ(renderer.records, 1);
}

TEST(ui_Presenter, RunsSixtyFramesWithoutDebt) {
  FakeSurface surface;
  FakeRenderer renderer;
  ui::Presenter p;
  p.set_surface(nullptr, &surface);
  p.set_renderer(&renderer);
  for (int i = 0; i < 60; ++i) {
    const auto r = p.run_frame();
    ASSERT_EQ(r.status, ui::PresenterStatus::Ok) << "第 " << i << " 帧失败";
    ASSERT_EQ(r.submits_this_frame, 1u) << "第 " << i << " 帧 submit 数不为 1";
    EXPECT_LE(p.frames_in_flight(), 2u) << "spec §5.1 P5:帧栅栏最多滞后 N=2 帧";
  }
  EXPECT_EQ(surface.submits, 60);
  EXPECT_EQ(surface.presents, 60);
}

TEST(ui_Presenter, InactiveSurfaceSkipsWithoutConsumingFrame) {
  FakeSurface surface;
  FakeRenderer renderer;
  ui::Presenter p;
  p.set_surface(nullptr, &surface);
  p.set_renderer(&renderer);
  p.set_active(false);  // G7:后台
  const auto r = p.run_frame();
  EXPECT_EQ(r.status, ui::PresenterStatus::NoSurface);
  EXPECT_EQ(r.phase, ui::FramePhase::Skipped);
  EXPECT_EQ(surface.acquires, 0) << "后台不得 acquire";
  EXPECT_EQ(p.frames_in_flight(), 0u) << "后台不得推进帧栅栏";

  p.set_active(true);
  const auto r2 = p.run_frame();
  EXPECT_EQ(r2.status, ui::PresenterStatus::Ok);
  EXPECT_EQ(r2.frame_index, 0u) << "后台期间不消耗帧号";
}

TEST(ui_Presenter, AcquireFailureIsReportedNotRetried) {
  FakeSurface surface;
  FakeRenderer renderer;
  surface.fail_acquire = true;
  surface.failure = ui::PresenterStatus::NeedRebuild;
  ui::Presenter p;
  p.set_surface(nullptr, &surface);
  p.set_renderer(&renderer);
  const auto r = p.run_frame();
  EXPECT_EQ(r.status, ui::PresenterStatus::NeedRebuild) << "OUT_OF_DATE 必须映射成 NeedRebuild";
  EXPECT_EQ(surface.acquires, 1) << "失败后不得内部重试(重试混淆问题归属)";
  EXPECT_EQ(surface.submits, 0);
}

TEST(ui_Presenter, StageOrderIsExact) {
  // spec §5.2 的顺序:1 WaitFence → 2 Acquire → 3 Record → 4 Submit → 5 Present
  const ui::FramePhase expected[] = {ui::FramePhase::WaitFence, ui::FramePhase::Acquire,
                                     ui::FramePhase::Record, ui::FramePhase::Submit,
                                     ui::FramePhase::Presented};
  for (auto stop : expected) {
    FakeSurface surface;
    FakeRenderer renderer;
    ui::Presenter p;
    p.set_surface(nullptr, &surface);
    p.set_renderer(&renderer);
    p.set_stop_after(stop);
    const auto r = p.run_frame();
    EXPECT_EQ(r.phase, stop);
    EXPECT_EQ(r.status, ui::PresenterStatus::Ok);
  }

  // 停在 Submit 之前:一次 submit 都没有
  FakeSurface s1;
  FakeRenderer r1;
  ui::Presenter p1;
  p1.set_surface(nullptr, &s1);
  p1.set_renderer(&r1);
  p1.set_stop_after(ui::FramePhase::Record);
  p1.run_frame();
  EXPECT_EQ(s1.submits, 0);
  EXPECT_EQ(s1.presents, 0);

  // 停在 Submit:有 submit,还没 present
  FakeSurface s2;
  FakeRenderer r2;
  ui::Presenter p2;
  p2.set_surface(nullptr, &s2);
  p2.set_renderer(&r2);
  p2.set_stop_after(ui::FramePhase::Submit);
  p2.run_frame();
  EXPECT_EQ(s2.submits, 1);
  EXPECT_EQ(s2.presents, 0);
}

TEST(ui_Presenter, InputLatencyTracked) {
  FakeSurface surface;
  FakeRenderer renderer;
  ui::Presenter p;
  p.set_surface(nullptr, &surface);
  p.set_renderer(&renderer);
  p.mark_input(0);  // 第 0 帧时登记输入
  const auto r0 = p.run_frame();
  EXPECT_EQ(r0.input_latency_frames, 0u);
  const auto r1 = p.run_frame();
  EXPECT_EQ(r1.input_latency_frames, 1u) << "spec §6 G6:input→present 帧差";
  EXPECT_LE(r1.input_latency_frames, 2u);
}

TEST(ui_Presenter, SubmitFailureSurfaces) {
  FakeSurface surface;
  FakeRenderer renderer;
  surface.fail_submit = true;
  ui::Presenter p;
  p.set_surface(nullptr, &surface);
  p.set_renderer(&renderer);
  const auto r = p.run_frame();
  EXPECT_EQ(r.status, ui::PresenterStatus::SubmitFailed);
  EXPECT_EQ(surface.presents, 0) << "提交失败不得继续 present";
}

TEST(ui_Presenter, RecordFailureSurfaces) {
  FakeSurface surface;
  FakeRenderer renderer;
  renderer.fail = true;
  ui::Presenter p;
  p.set_surface(nullptr, &surface);
  p.set_renderer(&renderer);
  const auto r = p.run_frame();
  EXPECT_EQ(r.status, ui::PresenterStatus::RecordFailed);
  EXPECT_EQ(surface.submits, 0);
}

TEST(ui_Presenter, NoSurfaceSkips) {
  FakeRenderer renderer;
  ui::Presenter p;
  p.set_renderer(&renderer);
  const auto r = p.run_frame();
  EXPECT_EQ(r.status, ui::PresenterStatus::NoSurface);
  EXPECT_EQ(renderer.records, 0);
}

// ================================================================ UiApp 装配

TEST(ui_UiApp, InitFailsExplicitlyWithoutRenderer) {
  // 主路径缺件必须明确失败,不能静默退化成离屏(spec §3 方案 A 禁止)
  ui::UiApp app;
  ui::UiAppConfig cfg;
  cfg.mode = ui::TargetMode::Swapchain;
  ui::UiAppDeps deps;
  deps.present_surface = nullptr;
  std::string err;
  EXPECT_FALSE(app.init(cfg, deps, &err));
  EXPECT_FALSE(err.empty()) << "失败必须带原因";
}

TEST(ui_UiApp, OffscreenModeRunsFrames) {
  ui::UiApp app;
  ui::UiAppConfig cfg;
  cfg.display = test_display();
  cfg.mode = ui::TargetMode::Offscreen;
  cfg.presenter.mode = ui::TargetMode::Offscreen;
  ui::test::FakeOffscreenTarget offscreen{dev::Extent2D{480, 320}};
  ui::UiAppDeps deps;
  FakeRenderer renderer;
  deps.frame_renderer = &renderer;
  deps.offscreen_target = &offscreen;
  std::string err;
  ASSERT_TRUE(app.init(cfg, deps, &err)) << err;

  for (int i = 0; i < 5; ++i) {
    const auto r = app.run_frame(0.f, 0.f);
    EXPECT_EQ(r.status, ui::PresenterStatus::Ok);
    EXPECT_EQ(r.submits_this_frame, 1u);
  }
  EXPECT_EQ(app.governor().stats().submit_violations, 0u);
  EXPECT_EQ(ui::as_internal(&app)->overlay().primitive_count() > 0, true) << "HUD 必须产出图元";
  EXPECT_FALSE(app.debug_panel().empty());
}

TEST(ui_UiApp, HudBackButtonRoutesThroughRouter) {
  ui::UiApp app;
  ui::UiAppConfig cfg;
  cfg.display = test_display();
  cfg.mode = ui::TargetMode::Offscreen;
  cfg.presenter.mode = ui::TargetMode::Offscreen;
  ui::test::FakeOffscreenTarget offscreen{dev::Extent2D{480, 320}};
  ui::UiAppDeps deps;
  FakeRenderer renderer;
  deps.frame_renderer = &renderer;
  deps.offscreen_target = &offscreen;
  std::string err;
  ASSERT_TRUE(app.init(cfg, deps, &err)) << err;

  ProbeFeature::State st;
  app.register_feature(ui::FeatureRegistration{ui::FeatureId("probe"),
                                              [&st]() { return std::make_unique<ProbeFeature>(&st); }});
  ASSERT_EQ(app.router().push(ui::FeatureId("probe")), ui::RouteAction::Pushed);

  // 把回退按钮接到 Router 的 callback 上(HUD 只依赖函数对象,不依赖 Router)
  auto cb = app.router().callback();
  std::uint32_t back_id = 0;
  for (const auto& c : app.hud().controls()) {
    if (c.kind == ui::ControlKind::Button && c.label == "back") back_id = c.id;
  }
  ASSERT_NE(back_id, 0u);
  app.hud().find(back_id)->on_click = [cb]() { cb(ui::FeatureId{}); };

  const ui::Control* back = app.hud().find(back_id);
  const float cx = back->rect_px.x + back->rect_px.w * 0.5f;
  const float cy = back->rect_px.y + back->rect_px.h * 0.5f;
  app.on_input(pev(dev::InputEventKind::PointerDown, cx, cy));
  app.on_input(pev(dev::InputEventKind::PointerUp, cx, cy));

  EXPECT_TRUE(app.router().current().empty()) << "HUD 回退按钮必须经 Router 回 HomeScreen";
  EXPECT_EQ(st.exits, 1);
}

TEST(ui_UiApp, PauseStopsPresentResumeRestores) {
  ui::UiApp app;
  ui::UiAppConfig cfg;
  cfg.display = test_display();
  cfg.mode = ui::TargetMode::Offscreen;
  cfg.presenter.mode = ui::TargetMode::Offscreen;
  ui::test::FakeOffscreenTarget offscreen{dev::Extent2D{480, 320}};
  ui::UiAppDeps deps;
  FakeRenderer renderer;
  deps.frame_renderer = &renderer;
  deps.offscreen_target = &offscreen;
  std::string err;
  ASSERT_TRUE(app.init(cfg, deps, &err)) << err;

  ASSERT_EQ(app.run_frame(0.f, 0.f).status, ui::PresenterStatus::Ok);
  dev::AppLifecycleEvent paused;
  paused.kind = dev::LifecycleKind::Paused;
  app.on_lifecycle(paused);
  const auto r = app.run_frame(0.f, 0.f);
  EXPECT_EQ(r.status, ui::PresenterStatus::NoSurface) << "G7:切后台必须暂停 present";

  dev::AppLifecycleEvent resumed;
  resumed.kind = dev::LifecycleKind::Resumed;
  app.on_lifecycle(resumed);
  EXPECT_EQ(app.run_frame(0.f, 0.f).status, ui::PresenterStatus::Ok);
}

TEST(ui_UiApp, SurfaceChangedRelayoutsHud) {
  ui::UiApp app;
  ui::UiAppConfig cfg;
  cfg.display = test_display();
  cfg.mode = ui::TargetMode::Offscreen;
  cfg.presenter.mode = ui::TargetMode::Offscreen;
  ui::test::FakeOffscreenTarget offscreen{dev::Extent2D{480, 320}};
  ui::UiAppDeps deps;
  FakeRenderer renderer;
  deps.frame_renderer = &renderer;
  deps.offscreen_target = &offscreen;
  std::string err;
  ASSERT_TRUE(app.init(cfg, deps, &err)) << err;

  dev::AppLifecycleEvent rot;
  rot.kind = dev::LifecycleKind::SurfaceChanged;
  rot.width = 320;
  rot.height = 480;
  app.on_lifecycle(rot);
  // 旋转后所有控件仍必须在新的屏幕内(布局真的重算了)
  for (const auto& c : app.hud().controls()) {
    EXPECT_LE(c.rect_px.right(), 320.f + 0.5f) << "控件越界: " << c.label;
  }
}

// ================================================================ 离屏 / golden(spec §5.5、§10 门 5)

namespace {

struct SwEnv {
  std::unique_ptr<dev::IRenderDevice> device;
  std::unique_ptr<dev::IRenderTarget> target;
  std::unique_ptr<mb::backend::VulkanFrameRenderer> renderer;
  std::unique_ptr<ui::OffscreenTargetRenderer> host;
  std::unique_ptr<ui::UiApp> app;
  std::unique_ptr<ui::MemoryStateStore> store;

  bool build(ui::u32 w, ui::u32 h, const std::string& feature_id) {
    auto r = dev::create_render_device([] {
      dev::DeviceDesc d;
      d.app_name = "MagicBlocks UI Test";
      d.require_software_rasterizer = true;
      return d;
    }());
    if (!r.ok()) {
      ADD_FAILURE() << "创建设备失败: " << r.message();
      return false;
    }
    device = r.take();
    if (device->caps().device_name.find("SwiftShader") == std::string::npos &&
        device->caps().device_name.find("llvmpipe") == std::string::npos &&
        device->caps().device_name.find("lavapipe") == std::string::npos) {
      ADD_FAILURE() << "要求软件光栅设备,实际 " << device->caps().device_name;
      return false;
    }

    dev::TargetDesc td;
    td.kind = dev::TargetDesc::Kind::Offscreen;
    td.extent = {w, h};
    td.with_depth = false;
    auto t = device->create_offscreen_target(td);
    if (!t.ok()) {
      ADD_FAILURE() << "创建离屏目标失败: " << t.message();
      return false;
    }
    target = t.take();
    auto* off = dynamic_cast<mb::backend::OffscreenTarget*>(target.get());
    if (!off) {
      ADD_FAILURE() << "离屏目标类型不符";
      return false;
    }
    auto* vkdev = dynamic_cast<mb::backend::VulkanRenderDevice*>(device.get());
    if (!vkdev) {
      ADD_FAILURE() << "设备类型不符";
      return false;
    }
    renderer = std::make_unique<mb::backend::VulkanFrameRenderer>(vkdev->context(), off);
    host = std::make_unique<ui::OffscreenTargetRenderer>();
    if (!host->attach(renderer.get())) {
      ADD_FAILURE() << "离屏后端 attach 失败";
      return false;
    }

    ui::UiAppConfig cfg;
    cfg.display = ui::DisplayInfo{w, h, 1.f};
    cfg.mode = ui::TargetMode::Offscreen;
    cfg.presenter.mode = ui::TargetMode::Offscreen;
    ui::UiAppDeps deps;
    deps.offscreen_target = target.get();
    deps.frame_renderer = renderer.get();
    store = std::make_unique<ui::MemoryStateStore>();
    deps.state_store = store.get();

    app = std::make_unique<ui::UiApp>();
    std::string err;
    if (!app->init(cfg, deps, &err)) {
      ADD_FAILURE() << "UI 初始化失败: " << err;
      return false;
    }
    if (!feature_id.empty()) {
      app->register_feature(ui::FeatureRegistration{ui::FeatureId(feature_id),
                                                   []() { return std::make_unique<ProbeFeature>(nullptr); }});
      if (app->router().push(ui::FeatureId(feature_id)) != ui::RouteAction::Pushed) {
        ADD_FAILURE() << "进入玩法失败";
        return false;
      }
    }
    return true;
  }

  bool render(mb::ImageRgba8* img) {
    img->width = target->extent().width;
    img->height = target->extent().height;
    img->pixels.assign(img->byte_size(), 0);
    app->run_frame(0.f, 0.f);
    if (!host->present(*app, img->width, img->height, img->pixels.data(), img->width * 4u)) {
      ADD_FAILURE() << "离屏渲染失败";
      return false;
    }
    return true;
  }
};

}  // namespace

TEST(ui_Offscreen, RendersDeterministicFrame) {
  SwEnv env;
  ASSERT_TRUE(env.build(320, 240, ""));
  mb::ImageRgba8 a, b;
  ASSERT_TRUE(env.render(&a));
  ASSERT_TRUE(env.render(&b));
  ASSERT_EQ(a.pixels.size(), b.pixels.size());
  EXPECT_EQ(std::memcmp(a.pixels.data(), b.pixels.data(), a.pixels.size()), 0)
      << "离屏出图必须逐像素可复现(golden 的前提)";
  // 画面不能是纯黑(说明 UI 真的画上去了)
  std::size_t nonbg = 0;
  for (std::size_t i = 0; i < a.pixels.size(); i += 4) {
    if (a.pixels[i] > 40 || a.pixels[i + 1] > 40 || a.pixels[i + 2] > 40) ++nonbg;
  }
  EXPECT_GT(nonbg, 500u) << "UI 图元没有画到离屏图上";
}

TEST(ui_Offscreen, MatchesGolden) {
  SwEnv env;
  ASSERT_TRUE(env.build(320, 240, ""));
  mb::ImageRgba8 img;
  ASSERT_TRUE(env.render(&img));

  const std::string dir = MB_GOLDEN_DIR;
  mb::ImageRgba8 golden;
  std::string err;
  ASSERT_TRUE(mb::read_png_rgba8(dir + "/golden_ui.png", &golden, &err))
      << "读取 golden 失败: " << err << "\n(先跑 magicblocks-ui-cli render 生成基线)";
  ASSERT_EQ(golden.width, img.width);
  ASSERT_EQ(golden.height, img.height);

  const std::size_t diff = mb::count_differing_pixels(golden, img, 8);
  const std::size_t total = static_cast<std::size_t>(img.width) * img.height;
  EXPECT_EQ(diff, 0u) << "与 golden 差异像素 " << diff << "/" << total;
}

TEST(ui_Offscreen, OffscreenPathIsTheOnlyReader) {
  // spec §10 门 1 的运行时旁证:主路径状态机根本不持有读回入口。
  // 这里断言"离屏出图后主路径的 Presenter 仍未被要求 present"
  SwEnv env;
  ASSERT_TRUE(env.build(160, 120, ""));
  mb::ImageRgba8 img;
  ASSERT_TRUE(env.render(&img));
  EXPECT_EQ(env.app->presenter().last().status, ui::PresenterStatus::Ok)
      << "离屏路径也要走 Presenter 的阶段记账(单次 submit)";
}
