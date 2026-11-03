// MagicBlocks L0 CLI —— 无头离屏渲染入口
//
// 两个命令:
//   magicblocks-l0-cli device
//       打印 DeviceCaps(JSON),不依赖窗口/显示服务。
//   magicblocks-l0-cli render --width W --height H --frame-index N --output out.png
//       纯离屏:RTT 自检绘制 → RGBA8 读回 → PNG 落盘。**不需要窗口**。
//
// 退出码:0 成功;1 运行失败;2 参数错误;3 需要提权才能让 loader 看到 SwiftShader。
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "core/image_io.h"
#include "magicblocks/device/render_device.h"

// CLI 需要触达 L0 自检绘制(离屏出图),这里直接用后端的离屏目标类型。
// 注意:这是 **CLI 内部**依赖,不属于 SDK 公共面(公共面仍只有 IRenderTarget)。
#include "backend/vulkan/vulkan_target.h"

namespace {

using namespace magicblocks::device;

void print_usage() {
  std::fprintf(stderr,
               "用法:\n"
               "  magicblocks-l0-cli device\n"
               "  magicblocks-l0-cli render --width W --height H --frame-index N --output FILE\n");
}

std::string caps_to_json(const DeviceCaps& c) {
  std::string s = "{\n";
  s += "  \"device_name\": \"" + c.device_name + "\",\n";
  s += "  \"api_version\": " + std::to_string(c.api_version) + ",\n";
  s += "  \"driver_version\": " + std::to_string(c.driver_version) + ",\n";
  s += "  \"is_software_rasterizer\": " + std::string(c.is_software_rasterizer ? "true" : "false") + ",\n";
  s += "  \"rev_z_supported\": " + std::string(c.rev_z_supported ? "true" : "false") + ",\n";
  s += "  \"max_push_constant_size\": " + std::to_string(c.max_push_constant_size) + ",\n";
  s += "  \"max_uniform_buffer_range\": " + std::to_string(c.max_uniform_buffer_range) + ",\n";
  s += "  \"timestamp_queries\": " + std::string(c.timestamp_queries ? "true" : "false") + ",\n";
  s += "  \"memory_budget\": " + std::to_string(c.memory_budget) + ",\n";
  s += "  \"memory_heap_total\": " + std::to_string(c.memory_heap_total) + ",\n";
  s += "  \"memory_budget_from_extension\": " +
       std::string(c.memory_budget_from_extension ? "true" : "false") + ",\n";
  s += "  \"supported_msaa_levels\": [";
  for (std::size_t i = 0; i < c.supported_msaa_levels.size(); ++i) {
    if (i) s += ", ";
    s += std::to_string(c.supported_msaa_levels[i]);
  }
  s += "],\n";
  s += "  \"depth_format_count\": " + std::to_string(c.depth_formats.size()) + ",\n";
  s += "  \"texture_format_count\": " + std::to_string(c.texture_formats.size()) + "\n";
  s += "}\n";
  return s;
}

DeviceDesc default_desc() {
  DeviceDesc d;
  d.app_name = "MagicBlocks CLI";
  d.require_software_rasterizer = true;  // 强筛:绝不静默跑物理 GPU
  return d;
}

int cmd_device() {
  auto r = create_render_device(default_desc());
  if (!r.ok()) {
    std::fprintf(stderr, "创建设备失败(code=%d): %s\n", static_cast<int>(r.status().code),
                 r.message().c_str());
    return 1;
  }
  std::printf("%s", caps_to_json(r.value()->caps()).c_str());
  return 0;
}

int cmd_render(int argc, char** argv) {
  u32 width = 320, height = 240;
  u64 frame_index = 0;
  std::string output;
  for (int i = 0; i < argc; ++i) {
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
      if (!v) return 2;
      width = static_cast<u32>(std::strtoul(v, nullptr, 10));
    } else if (a == "--height") {
      const char* v = next("--height");
      if (!v) return 2;
      height = static_cast<u32>(std::strtoul(v, nullptr, 10));
    } else if (a == "--frame-index") {
      const char* v = next("--frame-index");
      if (!v) return 2;
      frame_index = std::strtoull(v, nullptr, 10);
    } else if (a == "--output") {
      const char* v = next("--output");
      if (!v) return 2;
      output = v;
    } else {
      std::fprintf(stderr, "未知参数: %s\n", a.c_str());
      return 2;
    }
  }
  if (output.empty()) {
    std::fprintf(stderr, "--output 必填\n");
    return 2;
  }
  if (width == 0 || height == 0) {
    std::fprintf(stderr, "宽高必须 > 0\n");
    return 2;
  }

  auto dev = create_render_device(default_desc());
  if (!dev.ok()) {
    std::fprintf(stderr, "创建设备失败(code=%d): %s\n", static_cast<int>(dev.status().code),
                 dev.message().c_str());
    return 1;
  }

  TargetDesc td;
  td.kind = TargetDesc::Kind::Offscreen;
  td.extent = {width, height};
  td.with_depth = false;
  td.format = Format::RGBA8_UNORM;
  auto tgt = dev.value()->create_offscreen_target(td);
  if (!tgt.ok()) {
    std::fprintf(stderr, "创建离屏目标失败(code=%d): %s\n", static_cast<int>(tgt.status().code),
                 tgt.message().c_str());
    return 1;
  }

  magicblocks::ImageRgba8 img;
  img.width = width;
  img.height = height;
  img.pixels.assign(img.byte_size(), 0);

  // 自检绘制:实例化 4x4 色块(无几何着色器)。frame_index 参与实例相位,
  // 所以 --frame-index 0 与 1 的输出**必然不同**,且同 index 逐像素可复现。
  auto* off = dynamic_cast<magicblocks::backend::OffscreenTarget*>(tgt.value().get());
  if (!off) {
    std::fprintf(stderr, "内部错误:离屏目标类型不符\n");
    return 1;
  }
  Status ds = off->draw_selftest(frame_index);
  if (!ds.ok()) {
    std::fprintf(stderr, "绘制失败(code=%d): %s\n", static_cast<int>(ds.code), ds.message.c_str());
    return 1;
  }

  Status st = tgt.value()->readback_rgba8(img.pixels.data(), img.pixels.size());
  if (!st.ok()) {
    std::fprintf(stderr, "读回失败(code=%d): %s\n", static_cast<int>(st.code), st.message.c_str());
    return 1;
  }

  std::string err;
  if (!magicblocks::write_png_rgba8(output, img, &err)) {
    std::fprintf(stderr, "%s\n", err.c_str());
    return 1;
  }
  std::printf("wrote %s (%ux%u, frame_index=%llu)\n", output.c_str(), width, height,
              static_cast<unsigned long long>(frame_index));
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) {
    print_usage();
    return 2;
  }
  const std::string cmd = argv[1];
  if (cmd == "device") return cmd_device();
  if (cmd == "render") return cmd_render(argc - 2, argv + 2);
  print_usage();
  return 2;
}
