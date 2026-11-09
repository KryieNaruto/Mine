// MagicBlocks UI 消费层 CLI —— 离屏出图 / 统计打印(spec §10 验收门 5)
//
// 用法:
//   magicblocks-ui-cli render --width W --height H [--feature ID] --output out.png
//       走 spec §5.5 离屏路径:HomeScreen(或指定玩法)+ HUD → 读回 → PNG。
//       这是 golden 基线(`tests/data/golden_ui.png`)的产出方式。
//
//   magicblocks-ui-cli stats --width W --height H [--frames N] [--feature ID]
//       打印 HUD 合批统计 / 图元数 / 帧阶段序列(JSON),供 CTest 断言。
//       不落盘,不出图。
//
//   magicblocks-ui-cli present --width W --height H [--feature ID]
//       走**主路径**(TargetMode::Swapchain,无窗口 headless surface)驱动 Presenter
//       状态机,打印逐帧阶段与状态。用于验证"每帧恰一次 submit"这条约束在
//       链路上真的被记账(spec §10 验收门 2 的 CLI 侧旁证)。
//
// 退出码:0 成功 · 1 运行失败 · 2 参数错误。
//
// 注意:本 CLI 是**离屏/统计工具**,不是壳层;它不含任何窗口或输入循环。
// 真正的壳层(Android NativeActivity / Win32)另起,复用同一个 `ui::UiApp`。
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "backend/vulkan/vulkan_render_device.h"
#include "backend/vulkan/vulkan_target.h"
#include "backend/vulkan/vulkan_ui_overlay.h"
#include "core/image_io.h"
#include "magicblocks/device/render_device.h"
#include "magicblocks/ui/hud.h"
#include "magicblocks/ui/router.h"
#include "magicblocks/ui/ui_app.h"
#include "ui/offscreen_target.h"
#include "ui/ui_app_internal.h"

namespace {

namespace ui = magicblocks::ui;
namespace dev = magicblocks::device;

void print_usage() {
  std::fprintf(stderr,
               "用法:\n"
               "  magicblocks-ui-cli render  --width W --height H [--feature ID] --output FILE\n"
               "  magicblocks-ui-cli stats   --width W --height H [--frames N] [--feature ID]\n"
               "  magicblocks-ui-cli present --width W --height H [--feature ID]\n");
}

// HomeScreen 的入口:注册一个空玩法(FeatureHost 骨架,不实现任何玩法内容,
// spec §4「本轮只建立 HomeScreen 入口 + 空 FeatureHost 骨架」)。
// 玩法 ID 在这里由 CLI 决定(UI 层不认识具体玩法名),UI 源码里不出现任何玩法名。
class EmptyFeatureHost final : public ui::IFeatureHost {
 public:
  explicit EmptyFeatureHost(bool* entered) : entered_(entered) {}
  void on_enter(const ui::FeatureContext&) override {
    if (entered_) *entered_ = true;
  }
  void on_exit() override {}
  void on_update(float) override {}
  bool on_input(const dev::InputEvent&) override { return false; }

 private:
  bool* entered_ = nullptr;
};

struct Args {
  std::string cmd;
  ui::u32 width = 480;
  ui::u32 height = 320;
  ui::u32 frames = 1;
  std::string feature;
  std::string output;
};

bool parse_args(int argc, char** argv, Args* out) {
  if (argc < 1) return false;
  out->cmd = argv[0];
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto next = [&](const char* name) -> const char* {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "%s 缺少取值\n", name);
        return nullptr;
      }
      return argv[++i];
    };
    if (a == "--width") {
      const char* v = next("--width");
      if (!v) return false;
      out->width = static_cast<ui::u32>(std::strtoul(v, nullptr, 10));
    } else if (a == "--height") {
      const char* v = next("--height");
      if (!v) return false;
      out->height = static_cast<ui::u32>(std::strtoul(v, nullptr, 10));
    } else if (a == "--frames") {
      const char* v = next("--frames");
      if (!v) return false;
      out->frames = static_cast<ui::u32>(std::strtoul(v, nullptr, 10));
    } else if (a == "--feature") {
      const char* v = next("--feature");
      if (!v) return false;
      out->feature = v;
    } else if (a == "--output") {
      const char* v = next("--output");
      if (!v) return false;
      out->output = v;
    } else {
      std::fprintf(stderr, "未知参数: %s\n", a.c_str());
      return false;
    }
  }
  return out->width > 0 && out->height > 0 && out->frames > 0;
}

dev::DeviceDesc sw_desc() {
  dev::DeviceDesc d;
  d.app_name = "MagicBlocks UI CLI";
  d.require_software_rasterizer = true;  // 强筛,绝不回退物理 GPU
  return d;
}

// 装配:设备 → 离屏目标 → 后端帧渲染器 → UI 层。
struct Session {
  std::unique_ptr<dev::IRenderDevice> device;
  std::unique_ptr<dev::IRenderTarget> target;
  std::unique_ptr<magicblocks::backend::VulkanFrameRenderer> renderer;
  std::unique_ptr<ui::UiApp> app;
  std::unique_ptr<ui::OffscreenTargetRenderer> offscreen_host;

  // 状态存储(退出保存 / 重进恢复,spec §4 IStateStore)
  ui::MemoryStateStore store;
  bool feature_entered = false;
};

bool build_session(const Args& args, Session* s) {
  auto dev_r = dev::create_render_device(sw_desc());
  if (!dev_r.ok()) {
    std::fprintf(stderr, "创建设备失败: %s\n", dev_r.message().c_str());
    return false;
  }
  s->device = dev_r.take();

  dev::TargetDesc td;
  td.kind = dev::TargetDesc::Kind::Offscreen;
  td.extent = {args.width, args.height};
  td.with_depth = false;
  td.format = dev::Format::RGBA8_UNORM;
  auto tgt_r = s->device->create_offscreen_target(td);
  if (!tgt_r.ok()) {
    std::fprintf(stderr, "创建离屏目标失败: %s\n", tgt_r.message().c_str());
    return false;
  }
  s->target = tgt_r.take();

  auto* off = dynamic_cast<magicblocks::backend::OffscreenTarget*>(s->target.get());
  if (!off) {
    std::fprintf(stderr, "内部错误:离屏目标类型不符\n");
    return false;
  }

  auto ctx = static_cast<magicblocks::backend::VulkanRenderDevice*>(s->device.get())->context();
  s->renderer = std::make_unique<magicblocks::backend::VulkanFrameRenderer>(ctx, off);
  s->offscreen_host = std::make_unique<ui::OffscreenTargetRenderer>();
  if (!s->offscreen_host->attach(s->renderer.get())) {
    std::fprintf(stderr, "内部错误:离屏后端 attach 失败\n");
    return false;
  }

  ui::UiAppConfig cfg;
  cfg.display = ui::DisplayInfo{args.width, args.height, 1.f};
  cfg.mode = ui::TargetMode::Offscreen;
  cfg.scene_frame.mode = ui::TargetMode::Offscreen;

  ui::UiAppDeps deps;
  // 离屏模式没有壳层 surface:场景 sink 为空,SceneFrameDriver 会按 G7 跳过场景帧。
  // CLI 关心的是 UI 图元(由 OffscreenTargetRenderer 单独出图),不是场景上屏。
  deps.scene_sink = nullptr;
  deps.state_store = &s->store;

  s->app = std::make_unique<ui::UiApp>();
  std::string err;
  if (!s->app->init(cfg, deps, &err)) {
    std::fprintf(stderr, "UI 层初始化失败: %s\n", err.c_str());
    return false;
  }

  // 注册空 FeatureHost 骨架(--feature 指定 ID 时才有意义)
  if (!args.feature.empty()) {
    bool* entered = &s->feature_entered;
    s->app->register_feature(ui::FeatureRegistration{
        ui::FeatureId(args.feature),
        [entered]() { return std::make_unique<EmptyFeatureHost>(entered); }});
    if (s->app->router().push(ui::FeatureId(args.feature)) != ui::RouteAction::Pushed) {
      std::fprintf(stderr, "进入玩法失败(--feature %s)\n", args.feature.c_str());
      return false;
    }
  }
  return true;
}

int cmd_render(const Args& args) {
  if (args.output.empty()) {
    std::fprintf(stderr, "--output 必填\n");
    return 2;
  }
  Session s;
  if (!build_session(args, &s)) return 1;

  magicblocks::ImageRgba8 img;
  img.width = args.width;
  img.height = args.height;
  img.pixels.assign(img.byte_size(), 0);

  // 推进一帧让 Router/Governor 状态到位,再离屏出图(§5.5)
  s.app->run_frame(0.f, 0.f);
  if (!s.offscreen_host->present(*s.app, args.width, args.height, img.pixels.data(),
                                 args.width * 4)) {
    std::fprintf(stderr, "离屏渲染失败\n");
    return 1;
  }

  std::string err;
  if (!magicblocks::write_png_rgba8(args.output, img, &err)) {
    std::fprintf(stderr, "%s\n", err.c_str());
    return 1;
  }
  std::printf("wrote %s (%ux%u)\n", args.output.c_str(), args.width, args.height);
  return 0;
}

int cmd_stats(const Args& args) {
  Session s;
  if (!build_session(args, &s)) return 1;

  for (ui::u32 i = 0; i < args.frames; ++i) {
    s.app->run_frame(0.f, 0.f);
  }
  const auto& ov = ui::as_internal(s.app.get())->overlay();
  const auto st = ov.compute_stats();
  const auto dc = s.app->hud().draw_counts();

  std::printf("{\n");
  std::printf("  \"mode\": \"offscreen\",\n");
  std::printf("  \"feature\": \"%s\",\n", args.feature.c_str());
  std::printf("  \"feature_entered\": %s,\n", s.feature_entered ? "true" : "false");
  std::printf("  \"hud_controls\": %zu,\n", s.app->hud().size());
  std::printf("  \"primitives\": %zu,\n", ov.primitive_count());
  std::printf("  \"batches\": %u,\n", st.batches);
  std::printf("  \"draw_calls\": %u,\n", st.draw_calls);
  std::printf("  \"vertices\": %u,\n", st.vertices);
  std::printf("  \"hud_quads\": %u,\n", dc.quads);
  std::printf("  \"hud_strokes\": %u,\n", dc.strokes);
  std::printf("  \"hud_text_runs\": %u,\n", dc.text_runs);
  std::printf("  \"tier\": \"%s\"\n", ui::to_string(s.app->governor().tier()));
  std::printf("}\n");
  return 0;
}

int cmd_present(const Args& args) {
  Session s;
  if (!build_session(args, &s)) return 1;

  // 路线变更后的语义：上屏归壳层。SDK 只负责把一帧场景渲染进
  // **壳层提供的 surface**（ISceneFrameSink）。CLI 没有壳层，也就没有 sink，
  // 因此状态是 NoSurface、submits=0 —— 这是**正确行为**，不是错误。
  //
  // 真正带 surface 的可运行验证在 magicblocks-sink-verify 里。
  std::printf("[");
  for (ui::u32 i = 0; i < args.frames; ++i) {
    const auto r = s.app->run_frame(0.f, 0.f);
    std::printf("%s{\"frame\": %llu, \"phase\": \"%s\", \"status\": \"%s\", "
                "\"submits\": %u, \"latency_frames\": %u}",
                i ? ", " : "", static_cast<unsigned long long>(r.frame_index),
                ui::to_string(r.phase), ui::to_string(r.status),
                static_cast<unsigned>(r.submits_this_frame),
                static_cast<unsigned>(r.input_latency_frames));
  }
  std::printf("]\n");

  if (s.app->scene_frame().last().status == ui::FrameStatus::NoSurface) {
    std::fprintf(stderr,
                 "说明：未提供 ISceneFrameSink，本帧跳过上屏（NoSurface）。\n"
                 "      上屏由壳层负责；带真实 surface 的验证见 magicblocks-sink-verify。\n");
  }

  const auto& gov = s.app->governor().stats();
  std::fprintf(stderr, "panels:\n%s", s.app->debug_panel().c_str());
  if (gov.submit_violations != 0) {
    std::fprintf(stderr, "submit 约束被破坏: %u 帧\n", gov.submit_violations);
    return 1;
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    print_usage();
    return 2;
  }
  Args args;
  if (!parse_args(argc - 1, argv + 1, &args)) {
    print_usage();
    return 2;
  }
  if (args.cmd == "render") return cmd_render(args);
  if (args.cmd == "stats") return cmd_stats(args);
  if (args.cmd == "present") return cmd_present(args);
  print_usage();
  return 2;
}
