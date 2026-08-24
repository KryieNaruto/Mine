// easypainter-cli:无头离屏渲染。读采样点 → predictor 建模 → 离屏渲染 → 写 PNG。
// 用法: easypainter-cli [--input <x,y文件>] [--output <out.png>] [--width N --height M]
//                      [--stroke <spring,drag>]
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

#include "cli/example_points.h"
#include "core/render/image_io.h"
#include "core/render/offscreen.h"
#include "core/render/pipeline.h"
#include "core/render/vulkan_context.h"
#include "core/stroke/input_source.h"
#include "core/stroke/predictor.h"

namespace {

struct Args {
  std::string input;
  std::string output = "out.png";
  uint32_t width = 640;
  uint32_t height = 480;
  bool has_stroke = false;
  float stroke_spring = 0.f;
  float stroke_drag = 0.f;
};

bool ParseArgs(int argc, char** argv, Args& args) {
  auto next = [&](int& i) -> const char* {
    return (i + 1 < argc) ? argv[++i] : nullptr;
  };
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    if (a == "--input") {
      const char* v = next(i);
      if (!v) return false;
      args.input = v;
    } else if (a == "--output") {
      const char* v = next(i);
      if (!v) return false;
      args.output = v;
    } else if (a == "--width") {
      const char* v = next(i);
      if (!v || std::sscanf(v, "%u", &args.width) != 1) return false;
    } else if (a == "--height") {
      const char* v = next(i);
      if (!v || std::sscanf(v, "%u", &args.height) != 1) return false;
    } else if (a == "--stroke") {
      const char* v = next(i);
      if (!v || std::sscanf(v, "%f,%f", &args.stroke_spring, &args.stroke_drag) != 2)
        return false;
      args.has_stroke = true;
    } else {
      std::fprintf(stderr, "未知参数: %s\n", a.c_str());
      return false;
    }
  }
  return true;
}

std::vector<easypainter::stroke::Vec2> LoadPoints(const std::string& path) {
  std::vector<easypainter::stroke::Vec2> pts;
  std::ifstream f(path);
  std::string line;
  while (std::getline(f, line)) {
    if (line.empty() || line[0] == '#') continue;
    float x = 0.f, y = 0.f;
    if (std::sscanf(line.c_str(), "%f,%f", &x, &y) == 2) pts.push_back({x, y});
  }
  return pts;
}

// 归一化到 [0,1] 包围盒(避免退化:范围过小则保持)。
void Normalize(std::vector<easypainter::stroke::Vec2>& pts) {
  if (pts.empty()) return;
  float minx = pts[0].x, maxx = pts[0].x, miny = pts[0].y, maxy = pts[0].y;
  for (const auto& p : pts) {
    if (p.x < minx) minx = p.x;
    if (p.x > maxx) maxx = p.x;
    if (p.y < miny) miny = p.y;
    if (p.y > maxy) maxy = p.y;
  }
  const float rx = (maxx - minx) < 1e-6f ? 1.f : (maxx - minx);
  const float ry = (maxy - miny) < 1e-6f ? 1.f : (maxy - miny);
  for (auto& p : pts) {
    p.x = (p.x - minx) / rx;
    p.y = (p.y - miny) / ry;
  }
}

}  // namespace

int main(int argc, char** argv) {
  using namespace easypainter;

  Args args;
  if (!ParseArgs(argc, argv, args)) {
    std::fprintf(stderr,
                 "用法: easypainter-cli [--input <x,y文件>] [--output <out.png>] "
                 "[--width N --height M] [--stroke <spring,drag>]\n");
    return 2;
  }

  std::vector<stroke::Vec2> pts =
      args.input.empty() ? cli::example_points() : LoadPoints(args.input);
  Normalize(pts);
  if (pts.empty()) {
    std::fprintf(stderr, "无有效输入点\n");
    return 1;
  }

  stroke::PredictorConfig cfg;
  if (args.has_stroke) {
    cfg.spring_mass_constant = args.stroke_spring;
    cfg.drag_constant = args.stroke_drag;
  }
  stroke::Predictor predictor(cfg);
  const auto modeled = predictor.predict(stroke::build_events(pts));
  const std::vector<stroke::Vec2>& render_pts = modeled.empty() ? pts : modeled;

  render::VulkanContext ctx;
  if (!ctx.init()) {
    std::fprintf(stderr, "Vulkan 初始化失败(需要 lavapipe 软件光栅)\n");
    return 1;
  }
  render::Pipeline pipeline;
  if (!pipeline.init(ctx)) {
    std::fprintf(stderr, "Pipeline 初始化失败\n");
    return 1;
  }
  auto res = render::render_offscreen(ctx, pipeline, render_pts, args.width, args.height);
  if (res.rgba.empty()) {
    std::fprintf(stderr, "离屏渲染失败\n");
    return 1;
  }
  if (!render::write_png(args.output, res.width, res.height, res.rgba)) {
    std::fprintf(stderr, "写 PNG 失败: %s\n", args.output.c_str());
    return 1;
  }
  std::fprintf(stderr, "已写出 %s (%ux%u, %zu 点)\n", args.output.c_str(), res.width,
               res.height, render_pts.size());
  return 0;
}
