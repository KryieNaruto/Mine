// MagicBlocks L3 —— 预设测试模型离屏出图 CLI
//
// 用途(逐个模型单独出图,给 tools/png_inspect.py 断言前景占比 > 0 且非纯背景):
//   magicblocks-model-cli --asset-root <res> --output-dir <dir>
//       扫 <res>/models,把**每个**模型单独渲染成 <dir>/<name>.png
//   magicblocks-model-cli --asset-root <res> --list
//       只打印可用模型名(JSON),不建设备 —— 壳层可以用它填左侧列表
//
// 与 t7(UI 离屏合成)完全无关:它不经过 ui::UiApp / UiOverlayPass,
// 直接用 L3 的 model_catalog + l3_scene 管线出图。
//
// 确定性:相机、光照、清屏色全部由参数推导,不读时钟 → 同样参数逐字节相同。
// 退出码:0 成功 · 1 运行失败 · 2 参数/资源错误(不降级成空图)。
#if !defined(NOMINMAX)
#define NOMINMAX
#endif

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "backend/vulkan/vulkan_target.h"
#include "core/image_io.h"
#include "mb_internal/magicblocks/device/render_device.h"
#include "mb_internal/magicblocks/primitives/batch.h"
#include "mb_internal/magicblocks/primitives/camera.h"
#include "mb_internal/magicblocks/primitives/mesh_factory.h"
#include "mb_internal/magicblocks/primitives/mesh_loader.h"
#include "mb_internal/magicblocks/primitives/model_catalog.h"
#include "mb_internal/magicblocks/primitives/types.h"
#include "mb_internal/magicblocks/resources/buffer.h"
#include "mb_internal/magicblocks/resources/command.h"
#include "mb_internal/magicblocks/resources/memory.h"
#include "mb_internal/magicblocks/resources/pipeline.h"
#include "magicblocks_shaders.h"

namespace dev = magicblocks::device;
namespace res = magicblocks::resources;
namespace mbp = magicblocks::primitives;

namespace {

void usage() {
  std::fprintf(stderr,
               "用法:\n"
               "  magicblocks-model-cli --asset-root DIR --output-dir DIR [选项]\n"
               "  magicblocks-model-cli --asset-root DIR --list\n"
               "\n"
               "  --asset-root DIR   资源根(含 models/ 的目录)。缺省取 %%MB_ASSET_ROOT%%\n"
               "  --models-dir NAME  资源根下的模型子目录(默认 models)\n"
               "  --output-dir DIR   每个模型一张 PNG 的落盘目录\n"
               "  --list             只打印可用模型名(JSON),不建设备\n"
               "  --only NAME        只渲染指定模型(可重复)\n"
               "  --width W --height H  输出尺寸(默认 480x360)\n"
               "  --msaa N           采样数(默认 1)\n"
               "\n"
               "退出码:0 成功 / 1 运行失败 / 2 参数或资源错误。\n");
}

struct Args {
  std::string asset_root;
  std::string models_dir = "models";
  std::string output_dir;
  std::vector<std::string> only;
  bool list = false;
  int width = 480;
  int height = 360;
  int msaa = 1;
};

int parse(int argc, char** argv, Args* a) {
  for (int i = 1; i < argc; ++i) {
    const std::string k = argv[i];
    auto next = [&](const char* n) -> const char* {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "%s 缺少取值\n", n);
        return nullptr;
      }
      return argv[++i];
    };
    if (k == "--asset-root") {
      const char* v = next("--asset-root");
      if (!v) return -1;
      a->asset_root = v;
    } else if (k == "--models-dir") {
      const char* v = next("--models-dir");
      if (!v) return -1;
      a->models_dir = v;
    } else if (k == "--output-dir") {
      const char* v = next("--output-dir");
      if (!v) return -1;
      a->output_dir = v;
    } else if (k == "--only") {
      const char* v = next("--only");
      if (!v) return -1;
      a->only.emplace_back(v);
    } else if (k == "--list") {
      a->list = true;
    } else if (k == "--width") {
      const char* v = next("--width");
      if (!v) return -1;
      a->width = std::atoi(v);
    } else if (k == "--height") {
      const char* v = next("--height");
      if (!v) return -1;
      a->height = std::atoi(v);
    } else if (k == "--msaa") {
      const char* v = next("--msaa");
      if (!v) return -1;
      a->msaa = std::atoi(v);
    } else {
      std::fprintf(stderr, "未知参数: %s\n", k.c_str());
      return -1;
    }
  }
  if (a->asset_root.empty()) return -1;
  if (!a->list && a->output_dir.empty()) return -1;
  if (a->width <= 0 || a->height <= 0) return -1;
  return 0;
}

std::string models_path(const Args& a) {
  std::string p = a.asset_root;
  if (!p.empty() && p.back() != '/' && p.back() != '\\') p.push_back('/');
  p += a.models_dir;
  return p;
}

// 与 l3_cli 的 scene_pipeline_desc 同口径(实例化 3D 管线)。
res::PipelineDesc scene_pipeline_desc(int msaa) {
  res::PipelineDesc d;
  {
    res::VertexInputBinding vb;
    vb.binding = 0;
    vb.stride = sizeof(mbp::Vertex);
    vb.instanced = false;
    d.vertexLayout.bindings.push_back(vb);
  }
  {
    res::VertexInputBinding vb;
    vb.binding = 1;
    vb.stride = sizeof(mbp::InstanceAttribs);
    vb.instanced = true;
    d.vertexLayout.bindings.push_back(vb);
  }
  auto attr = [&d](std::uint32_t loc, std::uint32_t b, res::VertexFormat f, std::uint32_t off) {
    res::VertexAttribute a;
    a.location = loc;
    a.binding = b;
    a.format = f;
    a.offset = off;
    d.vertexLayout.attributes.push_back(a);
  };
  attr(0, 0, res::VertexFormat::Float3, 0);
  attr(1, 0, res::VertexFormat::Float3, 16);
  attr(2, 0, res::VertexFormat::UByte4Norm, 32);
  attr(3, 0, res::VertexFormat::Float2, 36);
  attr(4, 1, res::VertexFormat::Float4, 0);
  attr(5, 1, res::VertexFormat::Float4, 16);
  attr(6, 1, res::VertexFormat::Float4, 32);
  attr(7, 1, res::VertexFormat::Float4, 48);
  attr(8, 1, res::VertexFormat::Float4, 64);
  attr(9, 1, res::VertexFormat::Float4, 80);
  attr(10, 1, res::VertexFormat::Float4, 96);
  {
    res::ShaderModule m;
    m.stage = res::ShaderStage::Vertex;
    m.spirv = reinterpret_cast<const std::uint8_t*>(magicblocks::shaders::kL3_sceneVert);
    m.size_bytes = magicblocks::shaders::kL3_sceneVertSize;
    d.stages.push_back(m);
  }
  {
    res::ShaderModule m;
    m.stage = res::ShaderStage::Fragment;
    m.spirv = reinterpret_cast<const std::uint8_t*>(magicblocks::shaders::kL3_sceneFrag);
    m.size_bytes = magicblocks::shaders::kL3_sceneFragSize;
    d.stages.push_back(m);
  }
  d.blend.enabled = false;
  d.depth.depthTest = true;
  d.depth.depthWrite = true;
  d.raster.cull = res::CullMode::None;  // 模型朝向不定,双面都画
  d.samples = static_cast<res::u32>(msaa > 1 ? msaa : 1);
  d.primitive = res::PrimitiveType::TriangleList;
  d.push_constant_bytes = 128;
  return d;
}

// 渲染单个模型到 PNG。返回空串 = 成功;非空 = 失败原因。
std::string render_model(dev::IRenderDevice& device, const Args& a, const std::string& path,
                         const std::string& name, const std::string& out, bool* had_draw) {
  const int w = a.width;
  const int h = a.height;

  dev::TargetDesc td;
  td.kind = dev::TargetDesc::Kind::Offscreen;
  td.extent = {static_cast<dev::u32>(w), static_cast<dev::u32>(h)};
  td.with_depth = true;
  td.format = dev::Format::RGBA8_UNORM;
  if (a.msaa > 1) td.samples = static_cast<dev::u32>(a.msaa);
  auto tgt = device.create_offscreen_target(td);
  if (!tgt.ok()) return "创建离屏目标失败: " + tgt.message();

  // 加载 + 上传(L3 的 catalog 接口;归一化在内部完成)。
  mbp::ModelNormalize norm;
  norm.enabled = true;
  norm.target_diagonal = 1.0f;
  auto model = mbp::load_and_upload_model(device, path, name, norm);
  if (!model.ok()) return "加载/上传模型失败: " + model.message();
  const mbp::IMesh* mesh = model.value()->mesh();
  if (mesh == nullptr) return "IMesh 为空";

  // 相机:固定视角(不读时钟),把归一化后的模型放进取景框。
  //
  // 取景框按模型**自身**的归一化后包围盒算,而不是写死一个距离 ——
  // 归一化把对角线缩到 1.0,但 panel 这类退化包围盒(厚度为 0)在
  // 归一化前后都一样,写死距离会让它出框。这里用"包围盒半径 / sin(fov/2)"
  // 反推所需距离,再乘 1.15 留边。
  const float half_size = mesh->bounds().empty ? 0.5f : mesh->bounds().radius();
  const float safe_half = half_size > 1e-6f ? half_size : 0.5f;
  mbp::CameraDesc cd;
  cd.kind = mbp::ProjectionKind::Perspective;
  cd.fov_y_degrees = 45.0f;
  cd.near_plane = 0.05f;
  cd.far_plane = 100.0f;
  cd.viewport = {static_cast<mbp::u32>(w), static_cast<mbp::u32>(h)};
  auto camera = mbp::create_camera();
  if (!camera) return "创建相机失败";
  if (auto s = camera->set_desc(cd); !s.ok()) return "相机描述非法: " + s.message;
  const float half_fov_tan = std::tan(45.0f * 0.5f * 3.14159265358979323846f / 180.0f);
  const float dist = (safe_half / half_fov_tan) * 1.15f;
  mbp::CameraPose pose;
  // 固定的三分之四视角:方向不随模型变化,保证不同模型之间的构图可比。
  pose.position = {dist * 0.45f, dist * 0.32f, dist * 1.0f};
  pose.target = {0.0f, 0.0f, 0.0f};
  pose.up = {0.0f, 1.0f, 0.0f};
  if (auto s = camera->set_pose(pose); !s.ok()) return "相机位姿非法: " + s.message;

  // 实例属性(用一个单位矩阵 + 基色)。
  std::vector<mbp::InstanceAttribs> instances(1);
  mbp::InstanceAttribs& inst = instances[0];
  for (int i = 0; i < 16; ++i) inst.model[i] = 0.0f;
  inst.model[0] = inst.model[5] = inst.model[10] = inst.model[15] = 1.0f;
  inst.base_color[0] = 0.85f;
  inst.base_color[1] = 0.72f;
  inst.base_color[2] = 0.35f;
  inst.base_color[3] = 1.0f;
  inst.emissive[0] = inst.emissive[1] = inst.emissive[2] = 0.0f;
  inst.roughness = 0.6f;
  inst.metallic = 0.0f;

  res::BufferDesc ibd_inst;
  ibd_inst.size = sizeof(mbp::InstanceAttribs);
  ibd_inst.usage = res::BufferUsage::Vertex;
  auto inst_buf = device.create_buffer(ibd_inst);
  if (!inst_buf.ok()) return "创建实例缓冲失败: " + inst_buf.message();
  if (auto s = inst_buf.value()->upload(instances.data(), sizeof(mbp::InstanceAttribs)); !s.ok()) {
    return "上传实例失败: " + s.message;
  }
  inst_buf.value()->flush_pending();

  auto pl = device.create_pipeline(scene_pipeline_desc(a.msaa));
  if (!pl.ok()) return "创建场景管线失败: " + pl.message();

  auto cmd = device.create_command_list();
  if (!cmd.ok()) return "创建命令列表失败: " + cmd.message();

  res::RenderPassDesc rp;
  rp.target = tgt.value().get();
  rp.clear_color = true;
  // 深蓝灰:与模型的暖金色对比明显,png_inspect 用左上角做背景色判定。
  rp.clear_color_rgba[0] = 0.06f;
  rp.clear_color_rgba[1] = 0.07f;
  rp.clear_color_rgba[2] = 0.10f;
  rp.clear_color_rgba[3] = 1.0f;
  rp.clear_depth = true;
  rp.clear_depth_value = 1.0f;
  if (auto s = cmd.value()->begin_pass(rp); !s.ok()) return "begin_pass 失败: " + s.message;

  res::Viewport vp;
  vp.width = static_cast<float>(w);
  vp.height = static_cast<float>(h);
  vp.min_depth = 0.0f;
  vp.max_depth = 1.0f;
  if (auto s = cmd.value()->set_viewport(vp); !s.ok()) return "set_viewport 失败: " + s.message;
  res::ScissorRect sr;
  sr.width = static_cast<res::u32>(w);
  sr.height = static_cast<res::u32>(h);
  if (auto s = cmd.value()->set_scissor(sr); !s.ok()) return "set_scissor 失败: " + s.message;

  if (auto s = cmd.value()->bind_pipeline(pl.value().get(), pl.value()->hash()); !s.ok()) {
    return "bind_pipeline 失败: " + s.message;
  }
  struct ScenePC {
    float view_proj[16];
    float normal_matrix[16];
  } pc{};
  std::memcpy(pc.view_proj, camera->view_proj_matrix().m, sizeof(float) * 16);
  pc.normal_matrix[0] = pc.normal_matrix[5] = pc.normal_matrix[10] = pc.normal_matrix[15] = 1.0f;
  if (auto s = cmd.value()->push_constants(0x11u, 0, sizeof(pc), &pc); !s.ok()) {
    return "push_constants 失败: " + s.message;
  }

  const res::IBuffer* vb = mesh->vertex_buffer();
  const res::IBuffer* ib = mesh->index_buffer();
  if (vb == nullptr || ib == nullptr) return "网格没有顶点/索引缓冲";
  if (auto s = cmd.value()->bind_vertex_buffer(vb->id(), 0, 0); !s.ok()) {
    return "bind 顶点缓冲失败: " + s.message;
  }
  if (auto s = cmd.value()->bind_vertex_buffer(inst_buf.value()->id(), 1, 0); !s.ok()) {
    return "bind 实例缓冲失败: " + s.message;
  }
  if (auto s = cmd.value()->bind_index_buffer(ib->id(), res::IndexFormat::UInt32, 0); !s.ok()) {
    return "bind 索引缓冲失败: " + s.message;
  }
  res::DrawArgs da;
  da.index_count = mesh->index_count();
  da.instance_count = 1;
  if (auto s = cmd.value()->draw_indexed(da); !s.ok()) return "draw_indexed 失败: " + s.message;
  if (auto s = cmd.value()->end_pass(); !s.ok()) return "end_pass 失败: " + s.message;
  if (auto s = device.submit_command_list(*cmd.value()); !s.ok()) {
    return "提交失败: " + s.message;
  }
  *had_draw = true;

  std::vector<std::uint8_t> rgba(static_cast<std::size_t>(w) * h * 4u, 0);
  if (auto s = tgt.value()->readback_rgba8(rgba.data(), rgba.size()); !s.ok()) {
    return "读回失败: " + s.message;
  }
  magicblocks::ImageRgba8 img;
  img.width = static_cast<std::uint32_t>(w);
  img.height = static_cast<std::uint32_t>(h);
  img.pixels = std::move(rgba);
  std::string err;
  if (!magicblocks::write_png_rgba8(out, img, &err)) return "写 PNG 失败: " + err;
  return std::string();
}

}  // namespace

int main(int argc, char** argv) {
  Args a;
  if (parse(argc, argv, &a) != 0) {
    usage();
    return 2;
  }
  if (a.asset_root.empty()) {
    const char* env = std::getenv("MB_ASSET_ROOT");
    if (env != nullptr) a.asset_root = env;
  }
  if (a.asset_root.empty()) {
    std::fprintf(stderr, "--asset-root 缺失(或 MB_ASSET_ROOT 未设置)\n");
    return 2;
  }

  const std::string dir = models_path(a);
  auto scanned = mbp::scan_model_dir(dir);
  if (!scanned.ok()) {
    std::fprintf(stderr, "扫描模型目录失败: %s\n", scanned.message().c_str());
    return 2;
  }

  // --list:只打印名字,不建设备。
  if (a.list) {
    std::printf("{\"models\": [");
    bool first = true;
    for (const mbp::ModelEntry& m : scanned.value()) {
      if (!m.ok) continue;
      if (!a.only.empty()) {
        bool want = false;
        for (const std::string& n : a.only) {
          if (n == m.name) want = true;
        }
        if (!want) continue;
      }
      std::printf("%s\"%s\"", first ? "" : ", ", m.name.c_str());
      first = false;
    }
    std::printf("]}\n");
    return 0;
  }

  dev::DeviceDesc dd;
  dd.app_name = "MagicBlocks model CLI";
  dd.require_software_rasterizer = true;
  auto dev_r = dev::create_render_device(dd);
  if (!dev_r.ok()) {
    std::fprintf(stderr, "创建设备失败: %s\n", dev_r.message().c_str());
    return 1;
  }
  dev::IRenderDevice& device = *dev_r.value();
  res::IMemoryAllocator* alloc = device.memory_allocator();
  if (alloc == nullptr) {
    std::fprintf(stderr, "内部错误: 没有内存分配器\n");
    return 1;
  }
  for (res::MemoryKind k :
       {res::MemoryKind::Vertex, res::MemoryKind::Index, res::MemoryKind::Staging}) {
    if (auto s = alloc->reserve_pages(k, 4); !s.ok()) {
      std::fprintf(stderr, "预取页失败: %s\n", s.message.c_str());
      return 1;
    }
  }

  int failures = 0;
  int produced = 0;
  for (const mbp::ModelEntry& m : scanned.value()) {
    if (!a.only.empty()) {
      bool want = false;
      for (const std::string& n : a.only) {
        if (n == m.name) want = true;
      }
      if (!want) continue;
    }
    if (!m.ok) {
      // 扫描阶段就不可用的条目:**也要报**,不静默跳过。
      std::fprintf(stderr, "[skip-not-ok] %s: %s\n", m.name.c_str(), m.message.c_str());
      ++failures;
      continue;
    }
    std::string out = a.output_dir;
    if (!out.empty() && out.back() != '/' && out.back() != '\\') out.push_back('/');
    out += m.name + ".png";

    bool had_draw = false;
    const std::string err = render_model(device, a, m.path, m.name, out, &had_draw);
    if (!err.empty()) {
      std::fprintf(stderr, "[fail] %s: %s\n", m.name.c_str(), err.c_str());
      ++failures;
      continue;
    }
    if (!had_draw) {
      std::fprintf(stderr, "[fail] %s: 没有下发任何 draw\n", m.name.c_str());
      ++failures;
      continue;
    }
    std::printf("wrote %s (%dx%d)\n", out.c_str(), a.width, a.height);
    ++produced;
  }

  if (produced == 0 && failures == 0) {
    std::fprintf(stderr, "没有匹配到任何模型(--only 过滤后为空)\n");
    return 2;
  }
  if (failures != 0) {
    std::fprintf(stderr, "%d 个模型渲染失败\n", failures);
    return 1;
  }
  return 0;
}
