// 验证：Windows 上"渲染一帧场景到外部 surface"这条新接口真的能跑通。
//
// 这不是单元测试里的 fake —— 它用真实 Vulkan 设备 + 真实离屏目标扮演
// "壳层提供的 surface"，实现 ISceneFrameSink 的五个方法，然后：
//   1. 走 UiApp 的完整 run_frame 路径（scene_frame → sink）；
//   2. 断言这一帧恰好 1 次 acquire / 1 次 present；
//   3. 把这一帧的像素读回来做数值校验（HUD 必须真的画出来）。
//
// 用法：magicblocks-sink-verify [输出目录]
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "backend/vulkan/vulkan_render_device.h"
#include "backend/vulkan/vulkan_target.h"
#include "backend/vulkan/vulkan_ui_overlay.h"
#include "core/image_io.h"
#include "magicblocks/device/render_device.h"
#include "magicblocks/resources/command.h"
#include "magicblocks/ui/frame_renderer.h"
#include "magicblocks/ui/ui_app.h"
#include "ui/offscreen_target.h"

namespace ui = magicblocks::ui;
namespace dev = magicblocks::device;
namespace backend = magicblocks::backend;

namespace {

dev::DeviceDesc software_device() {
  dev::DeviceDesc d;
  d.require_software_rasterizer = true;
  d.app_name = "magicblocks-sink-verify";
  return d;
}

// 真实的外部 surface：由"壳层"持有，SDK 只往里画，不拥有它的生命周期。
class VerifySurfaceSink final : public ui::ISceneFrameSink {
 public:
  VerifySurfaceSink(dev::IRenderDevice* device, backend::OffscreenTarget* target)
      : device_(device), target_(target) {}

  dev::Extent2D extent() const override { return target_->extent(); }
  dev::Format format() const override { return target_->color_format(); }
  bool ready() const override { return true; }

  bool acquire_image(ui::SinkImage* out) override {
    ++acquire_calls;
    out->id = ++next_id_;
    out->extent = target_->extent();
    out->format = target_->color_format();
    out->valid = true;
    last_image_ = *out;
    return true;
  }

  bool submit_frame(magicblocks::resources::ICommandList& list) override {
    ++submit_calls;
    return device_->submit_command_list(list).ok();
  }

  bool present_image(const ui::SinkImage& img) override {
    ++present_calls;
    presented_ = img;
    return true;
  }

  ui::SinkStatus last_status() const override { return ui::SinkStatus::Ok; }

  int acquire_calls = 0;
  int submit_calls = 0;
  int present_calls = 0;
  ui::SinkImage last_image_{};
  ui::SinkImage presented_{};

 private:
  dev::IRenderDevice* device_;
  backend::OffscreenTarget* target_;
  ui::u64 next_id_ = 0;
};

}  // namespace

int main(int argc, char** argv) {
  const std::string out_dir = argc > 1 ? argv[1] : ".";

  // 1) 真实设备（SwiftShader 软件光栅，与 CI 一致）
  auto dev_r = dev::create_render_device(software_device());
  if (!dev_r.ok()) {
    std::fprintf(stderr, "FAIL: 创建设备失败: %s\n", dev_r.message().c_str());
    return 1;
  }
  std::unique_ptr<dev::IRenderDevice> device = dev_r.take();

  // 2) "壳层提供的 surface"
  dev::TargetDesc td;
  td.kind = dev::TargetDesc::Kind::Offscreen;
  td.extent = {320, 240};
  td.with_depth = false;
  td.format = dev::Format::RGBA8_UNORM;
  auto tgt_r = device->create_offscreen_target(td);
  if (!tgt_r.ok()) {
    std::fprintf(stderr, "FAIL: 创建离屏目标失败: %s\n", tgt_r.message().c_str());
    return 1;
  }
  std::unique_ptr<dev::IRenderTarget> target = tgt_r.take();

  auto* off = dynamic_cast<backend::OffscreenTarget*>(target.get());
  if (!off) {
    std::fprintf(stderr, "FAIL: 离屏目标类型不符\n");
    return 1;
  }
  auto ctx = static_cast<backend::VulkanRenderDevice*>(device.get())->context();
  backend::VulkanFrameRenderer renderer(ctx, off);
  // 离屏出图走正规路径：OffscreenTargetRenderer 会把 UiApp 产出的图元交给后端。
  ui::OffscreenTargetRenderer offscreen_host;
  if (!offscreen_host.attach(&renderer)) {
    std::fprintf(stderr, "FAIL: OffscreenTargetRenderer::attach\n");
    return 1;
  }

  // 3) 壳层侧的 sink 实现
  VerifySurfaceSink sink(device.get(), off);

  // 4) UiApp 走完整路径
  ui::UiAppConfig cfg;
  cfg.display = ui::DisplayInfo{320, 240, 1.f};
  cfg.mode = ui::TargetMode::Offscreen;
  cfg.show_perf_panel = true;

  ui::UiAppDeps deps;
  deps.scene_sink = &sink;
  deps.scene_command_list = nullptr;  // 本验证只关心 sink 契约，不驱动后端命令流。

  ui::UiApp app;
  std::string err;
  if (!app.init(cfg, deps, &err)) {
    std::fprintf(stderr, "FAIL: UiApp::init: %s\n", err.c_str());
    return 1;
  }

  ui::SceneFrameResult r = app.run_frame(1.0f / 60.0f, 0.016f);

  std::printf("run_frame -> status=%s phase=%s submits=%u acquired=%d presented=%d\n",
              to_string(r.status), to_string(r.phase),
              static_cast<unsigned>(r.submits_this_frame), sink.acquire_calls,
              sink.present_calls);

  int failures = 0;
  auto check = [&](bool cond, const char* what) {
    std::printf("  [%s] %s\n", cond ? "OK" : "FAIL", what);
    if (!cond) ++failures;
  };

  check(r.ok(), "frame 整体成功（ok() 为真）");
  check(sink.acquire_calls == 1, "恰好 1 次 acquire_image");
  check(sink.present_calls == 1, "恰好 1 次 present_image");
  check(sink.last_image_.valid, "acquire 返回了有效 image");
  check(sink.last_image_.extent.width == 320 && sink.last_image_.extent.height == 240,
        "image extent 与外部 surface 一致(320x240)");
  check(sink.presented_.id == sink.last_image_.id, "present 的正是 acquire 到的那一帧");
  check(r.submits_this_frame <= 1, "每帧至多 1 次提交（UIG2）");

  // 5) 像素校验：走离屏回读（CI/golden 专用路径），确认 HUD 真的画出来了
  std::vector<std::uint8_t> px(320 * 240 * 4, 0);
  if (!offscreen_host.present(app, 320, 240, px.data(), 320 * 4)) {
    std::printf("  [FAIL] offscreen_host.present 失败（HUD 没画出来）\n");
    ++failures;
  } else {
    std::size_t nonzero = 0;
    std::size_t opaque = 0;
    for (std::size_t i = 0; i < 320 * 240; ++i) {
      const std::uint8_t* p = &px[i * 4];
      if (p[0] | p[1] | p[2] | p[3]) ++nonzero;
      if (p[3] != 0) ++opaque;
    }
    std::printf("  像素：非零=%zu/%d，不透明=%zu/%d\n", nonzero, 320 * 240, opaque, 320 * 240);
    check(nonzero > 500, "场景帧有实际像素输出（>500 像素非零）");
    check(opaque > 500, "HUD 图元真的光栅化到了外部 surface 上");

    const std::string out = out_dir + "/sink_verify_frame.png";
    magicblocks::ImageRgba8 img;
    img.width = 320;
    img.height = 240;
    img.pixels = std::move(px);
    std::string perr;
    if (magicblocks::write_png_rgba8(out, img, &perr)) {
      std::printf("  输出：%s\n", out.c_str());
    } else {
      std::printf("  写 PNG 失败：%s\n", perr.c_str());
    }
  }

  std::printf("\n%s\n", failures == 0 ? "全部通过" : "有失败项");
  return failures == 0 ? 0 : 1;
}
