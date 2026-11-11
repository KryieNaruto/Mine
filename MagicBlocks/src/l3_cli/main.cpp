// MagicBlocks L3 —— 命令行工具
//
// 两个子命令，对应 spec §13 的 M1 与 M2 验收：
//
//   paint  ：IPainter 出笔画 → 离屏渲染 → PNG。验证"能画"这个最小闭环。
//   blocks ：IWorld(ECS) + IMeshFactory + IBatchBuilder → 若干积木 → PNG。
//            验证 ECS/合批/实例化整条链路。
//
// 为什么单独一个 CLI 而不是塞进 l2_cli：
//   l2_cli 验证的是"命令流结构"，用 selftest 写死几何；l3_cli 验证的是
//   "真的画出几何"，走 ICommandList 录制 + submit_command_list 下发。
//   两条路径的失败模式完全不同，混在一个可执行文件里会让 golden 互相干扰。
//
// 出图确定性（spec §11）：
//   所有输入都由命令行参数**完全推导**，不读时钟、不读鼠标、不读文件系统状态。
//   同样的参数两次运行 → 逐字节相同的 PNG。这是 golden 基线成立的前提。
// 必须在**任何**头文件之前：Windows SDK 的 <windows.h>(会经 Vulkan 平台头引入)
// 把 min/max 定义成宏，破坏 std::numeric_limits<T>::max()。
// world.h 里虽然也有防御，但那时 windows.h 可能已经先被拖进来了，
// 所以这里的定义才真正有效。
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "backend/vulkan/vulkan_target.h"
#include "core/image_io.h"
#include "magicblocks/device/render_device.h"
#include "magicblocks/primitives/batch.h"
#include "magicblocks/primitives/camera.h"
#include "magicblocks/primitives/material.h"
#include "magicblocks/primitives/mesh_factory.h"
#include "magicblocks/primitives/gizmo.h"
#include "magicblocks/primitives/overlay2d.h"
#include "magicblocks/primitives/painter.h"
#include "magicblocks/primitives/picker.h"
#include "magicblocks/primitives/primitive_renderer.h"
#include "magicblocks/primitives/types.h"
#include "magicblocks/primitives/world.h"
#include "magicblocks/primitives/world_render.h"
#include "magicblocks/resources/buffer.h"
#include "magicblocks/resources/command.h"
#include "magicblocks/resources/pipeline.h"
#include "magicblocks/features/stack/block_brush.h"
#include "magicblocks/features/stack/block_set.h"
#include "magicblocks/features/stack/color_mode.h"
#include "magicblocks/features/stack/grid_overlay.h"
#include "magicblocks/features/stack/shape_template.h"
#include "magicblocks/features/stack/stack_render.h"
#include "magicblocks/features/stack/stack_session.h"
#include "magicblocks/features/stack/stack_types.h"
#include "magicblocks/features/sketch/sketch_document.h"
#include "magicblocks/features/sketch/sketch_pipeline.h"
#include "magicblocks_shaders.h"

namespace dev = magicblocks::device;
namespace res = magicblocks::resources;
namespace mbp = magicblocks::primitives;
namespace featsketch = magicblocks::features::sketch;
namespace mbst = magicblocks::features::stack;

namespace {

// ---------------------------------------------------------------- 参数

struct Args {
  std::string cmd;
  int width = 640;
  int height = 400;
  std::string output;
  int strokes = 3;
  int blocks = 12;
  unsigned seed = 1;
  // ---- MSAA（演示用开关）----
  // 缺省 1 = 不开启。**不加 --msaa 时行为与引入该参数前完全一致**，
  // 这是 golden 逐字节不变的前提。取值为 DeviceCaps.supported_msaa_levels
  // 里的值（通常 1/2/4/8），不在列表里就明确报错，不静默降级。
  int msaa = 1;
  // ---- F-SK（涂鸦）----
  int sketch_pattern = 0;  // 0=波浪(Ribbon) 1=方框(Region) 2=交叉(并集)
  bool merge = false;      // 是否把所有 Ribbon 并成一个实体
  // ---- F-VX（魔法叠叠乐）----
  // 以下只给 stack 子命令用。缺省值即契约默认值（spec §13.1）。
  int layers = 8;
  float cell_size = 0.5f;
  int column_height = 1;
  std::string shape_template;  // 空 = 不用模板（按列高堆）
  int template_radius = 2;
  std::string color_mode;      // 空 = flat
  int lock_layer = -1;         // -1 = 不锁层
  // --inner-pass：缺省 on（契约默认），但取值必须显式；
  // 不接受 auto/default/空这类"让程序自己决定"的写法。
  bool inner_pass = true;
  bool inner_pass_given = false;
  bool dry_run = false;  // 只跑到逻辑阶段，不建设备不出图（§13.3）
  // ---- 网格指示（需求「立方体…带网格指示」）----
  // 缺省 **开**：需求原文点名要有网格指示，缺省关掉等于需求没落地。
  // 关掉只为一件事：让"开/关两张图必须不同"的像素用例有对照图。
  bool grid_overlay = true;
};

void print_usage() {
  std::printf(
      "用法:\n"
      "  magicblocks-l3-cli paint  --width 640 --height 400 --strokes 3 --output paint.png\n"
      "  magicblocks-l3-cli blocks --width 640 --height 400 --blocks 12 --output blocks.png\n"
      "  magicblocks-l3-cli gizmo  --width 640 --height 400 --output gizmo.png\n"
      "  magicblocks-l3-cli sketch --width 640 --height 400 --strokes 3\n"
      "                            [--sketch-pattern 0|1|2] [--merge] --output sketch.png\n"
      "  magicblocks-l3-cli stack  --width 640 --height 400 --layers 8 --strokes 3\n"
      "                            [--cell-size 0.5] [--column-height 1]\n"
      "                            [--template square|circle|cross|sphere] [--template-radius 2]\n"
      "                            [--color-mode flat|by-layer|by-stroke] [--lock-layer N]\n"
      "                            [--inner-pass on|off] [--dry-run] --output stack.png\n"
      "          stack 的网格指示缺省开，可用 [--grid-overlay on|off] 关掉做对照。\n"
      "\n"
      "通用可选参数:\n"
      "  --msaa N    MSAA 采样数(1=关闭,缺省;可填设备支持的 2/4/8)。\n"
      "              缺省不开,纯演示用开关;取值不被设备支持时明确报错。\n"
      "\n"
      "各子命令都不读时钟/鼠标,同样的参数产出**逐字节相同**的 PNG(golden 前提)。\n");
}

bool take_value(int argc, char** argv, int* i, const char* flag, std::string* out) {
  if (*i + 1 >= argc) {
    std::fprintf(stderr, "%s 缺少参数值\n", flag);
    return false;
  }
  *out = argv[++*i];
  return true;
}

int parse_args(int argc, char** argv, Args* out) {
  if (argc < 2) return -1;
  out->cmd = argv[1];
  for (int i = 2; i < argc; ++i) {
    const std::string a = argv[i];
    std::string v;
    if (a == "--width") {
      if (!take_value(argc, argv, &i, "--width", &v)) return -2;
      out->width = std::atoi(v.c_str());
    } else if (a == "--height") {
      if (!take_value(argc, argv, &i, "--height", &v)) return -2;
      out->height = std::atoi(v.c_str());
    } else if (a == "--output") {
      if (!take_value(argc, argv, &i, "--output", &v)) return -2;
      out->output = v;
    } else if (a == "--strokes") {
      if (!take_value(argc, argv, &i, "--strokes", &v)) return -2;
      out->strokes = std::atoi(v.c_str());
    } else if (a == "--blocks") {
      if (!take_value(argc, argv, &i, "--blocks", &v)) return -2;
      out->blocks = std::atoi(v.c_str());
    } else if (a == "--sketch-pattern") {
      if (!take_value(argc, argv, &i, "--sketch-pattern", &v)) return -2;
      out->sketch_pattern = std::atoi(v.c_str());
    } else if (a == "--merge") {
      out->merge = true;
    } else if (a == "--msaa") {
      if (!take_value(argc, argv, &i, "--msaa", &v)) return -2;
      out->msaa = std::atoi(v.c_str());
    } else if (a == "--layers") {
      if (!take_value(argc, argv, &i, "--layers", &v)) return -2;
      out->layers = std::atoi(v.c_str());
    } else if (a == "--cell-size") {
      if (!take_value(argc, argv, &i, "--cell-size", &v)) return -2;
      out->cell_size = static_cast<float>(std::atof(v.c_str()));
    } else if (a == "--column-height") {
      if (!take_value(argc, argv, &i, "--column-height", &v)) return -2;
      out->column_height = std::atoi(v.c_str());
    } else if (a == "--template") {
      if (!take_value(argc, argv, &i, "--template", &v)) return -2;
      out->shape_template = v;
    } else if (a == "--template-radius") {
      if (!take_value(argc, argv, &i, "--template-radius", &v)) return -2;
      out->template_radius = std::atoi(v.c_str());
    } else if (a == "--color-mode") {
      if (!take_value(argc, argv, &i, "--color-mode", &v)) return -2;
      out->color_mode = v;
    } else if (a == "--lock-layer") {
      if (!take_value(argc, argv, &i, "--lock-layer", &v)) return -2;
      out->lock_layer = std::atoi(v.c_str());
    } else if (a == "--inner-pass") {
      if (!take_value(argc, argv, &i, "--inner-pass", &v)) return -2;
      // 只接受 on/off/1/0/true/false。**不接受** auto/default/空值：
      // "让程序自己决定内面 pass 开不开"等于把正确性交给隐式规则，
      // 出图结果会随缺省值改动而变而无人知情。
      if (v == "on" || v == "1" || v == "true") {
        out->inner_pass = true;
      } else if (v == "off" || v == "0" || v == "false") {
        out->inner_pass = false;
      } else {
        std::fprintf(stderr, "--inner-pass 取值必须显式(on|off|1|0|true|false), 实际: %s\n",
                     v.c_str());
        return -2;
      }
      out->inner_pass_given = true;
    } else if (a == "--dry-run") {
      out->dry_run = true;
    } else if (a == "--grid-overlay") {
      if (!take_value(argc, argv, &i, "--grid-overlay", &v)) return -2;
      // 与 --inner-pass 同一纪律：取值必须显式，不接受"让程序决定"。
      if (v == "on" || v == "1" || v == "true") {
        out->grid_overlay = true;
      } else if (v == "off" || v == "0" || v == "false") {
        out->grid_overlay = false;
      } else {
        std::fprintf(stderr, "--grid-overlay 取值必须显式(on|off|1|0|true|false), 实际: %s\n",
                     v.c_str());
        return -2;
      }
    } else if (a == "--inner-depth-const") {
      // §13.1 / D-VX-4′：该参数**不存在**，且永远不该存在。
      // 它对应 depthConstWrite → depthBounds 特性（本设备从未请求，属 UB）。
      // 给出专门一条错误信息，而不是笼统的"未知参数"，避免有人以为是自己拼错。
      std::fprintf(stderr,
                   "--inner-depth-const 不可用: 它依赖未启用的 depthBounds 特性(D-VX-4′)");
      return -2;
    } else if (a == "--seed") {
      if (!take_value(argc, argv, &i, "--seed", &v)) return -2;
      out->seed = static_cast<unsigned>(std::strtoul(v.c_str(), nullptr, 10));
    } else if (a == "-h" || a == "--help") {
      return -1;
    } else {
      std::fprintf(stderr, "未知参数: %s\n", a.c_str());
      return -2;
    }
  }
  return 0;
}

// ---------------------------------------------------------------- 设备

dev::DeviceDesc software_desc() {
  dev::DeviceDesc d;
  d.backend = dev::BackendType::Vulkan;
  d.enable_validation = false;
  // 要求软件光栅器:CI 无 GPU,且 SwiftShader 的行为跨机器可复现,
  // golden 才有意义。
  d.require_software_rasterizer = true;
  return d;
}

bool save_png(const std::string& path, const std::vector<std::uint8_t>& rgba, int w, int h) {
  magicblocks::ImageRgba8 img;
  img.width = static_cast<std::uint32_t>(w);
  img.height = static_cast<std::uint32_t>(h);
  img.pixels = rgba;
  std::string err;
  if (!magicblocks::write_png_rgba8(path, img, &err)) {
    std::fprintf(stderr, "写 PNG 失败: %s\n", err.c_str());
    return false;
  }
  return true;
}

// ---------------------------------------------------------------- 管线描述

// 笔画条带的管线：屏幕空间、无深度、预乘 alpha 混合、双面可见。
res::PipelineDesc stroke_pipeline_desc(int msaa = 1) {
  res::PipelineDesc d;
  // binding 0：每顶点，stride = sizeof(StrokeVertex) = 32
  res::VertexInputBinding vb;
  vb.binding = 0;
  vb.stride = sizeof(mbp::StrokeVertex);
  vb.instanced = false;
  d.vertexLayout.bindings.push_back(vb);

  // location 0 = position(float2)  location 1 = color(ubyte4norm)
  {
    res::VertexAttribute a;
    a.location = 0;
    a.binding = 0;
    a.format = res::VertexFormat::Float2;
    a.offset = 0;
    d.vertexLayout.attributes.push_back(a);
  }
  {
    res::VertexAttribute a;
    a.location = 1;
    a.binding = 0;
    a.format = res::VertexFormat::UByte4Norm;
    a.offset = 16;  // StrokeVertex::color 的偏移
    d.vertexLayout.attributes.push_back(a);
  }

  {
    res::ShaderModule m;
    m.stage = res::ShaderStage::Vertex;
    m.spirv = reinterpret_cast<const std::uint8_t*>(magicblocks::shaders::kStroke_ribbonVert);
    m.size_bytes = magicblocks::shaders::kStroke_ribbonVertSize;
    d.stages.push_back(m);
  }
  {
    // ShaderModule 带默认成员初始化器 → 不是聚合体,不能用 {a,b,c} 花括号构造。
    res::ShaderModule m;
    m.stage = res::ShaderStage::Fragment;
    m.spirv = reinterpret_cast<const std::uint8_t*>(magicblocks::shaders::kStroke_ribbonFrag);
    m.size_bytes = magicblocks::shaders::kStroke_ribbonFragSize;
    d.stages.push_back(m);
  }

  d.blend.enabled = true;
  // 预乘 alpha：笔迹叠加时不会出现黑边
  d.blend.src_color = res::BlendFactor::One;
  d.blend.dst_color = res::BlendFactor::OneMinusSrcAlpha;
  d.blend.src_alpha = res::BlendFactor::One;
  d.blend.dst_alpha = res::BlendFactor::OneMinusSrcAlpha;
  d.depth.depthTest = false;
  d.depth.depthWrite = false;
  d.raster.cull = res::CullMode::None;
  d.samples = static_cast<res::u32>(msaa > 1 ? msaa : 1);
  d.primitive = res::PrimitiveType::TriangleList;
  // push constant：viewport_size(8) + pad(8) = 16
  d.push_constant_bytes = 16;
  return d;
}

// 实例化 3D 图元的管线：深度测试开、背面剔除、预乘 alpha。
res::PipelineDesc scene_pipeline_desc(int msaa = 1) {
  res::PipelineDesc d;

  // binding 0：每顶点（与 mesh.h 的 Vertex 一致，stride 48）
  {
    res::VertexInputBinding vb;
    vb.binding = 0;
    vb.stride = sizeof(mbp::Vertex);
    vb.instanced = false;
    d.vertexLayout.bindings.push_back(vb);
  }
  // binding 1：每实例（与 batch.h 的 InstanceAttribs 一致）
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

  // 顶点属性
  attr(0, 0, res::VertexFormat::Float3, 0);   // position
  attr(1, 0, res::VertexFormat::Float3, 16);  // normal
  attr(2, 0, res::VertexFormat::UByte4Norm, 32);  // color
  attr(3, 0, res::VertexFormat::Float2, 36);  // uv

  // 实例属性。偏移必须与 batch.h 的 InstanceAttribs 逐字节对齐：
  //   model[16]     @0   (64 B)
  //   base_color[4] @64  (16 B)
  //   emissive[3]   @80  (12 B)
  //   roughness     @92  (4 B)
  //   metallic      @96  (4 B)
  //   _pad[3]       @100 (12 B)  → 总 112 B
  // 写成常量而不是猜，是因为下面这些偏移一旦错位，后果是"颜色/金属度悄悄
  // 取自别的字段"，画面看着像能看、实际全错 —— 比直接崩还难查。
  constexpr std::uint32_t kOffModel = 0;
  constexpr std::uint32_t kOffBaseColor = 64;
  constexpr std::uint32_t kOffEmissive = 80;

  attr(4, 1, res::VertexFormat::Float4, kOffModel + 0);
  attr(5, 1, res::VertexFormat::Float4, kOffModel + 16);
  attr(6, 1, res::VertexFormat::Float4, kOffModel + 32);
  attr(7, 1, res::VertexFormat::Float4, kOffModel + 48);
  attr(8, 1, res::VertexFormat::Float4, kOffBaseColor);      // base_color
  attr(9, 1, res::VertexFormat::Float4, kOffEmissive);       // emissive.xyz + roughness
  // location 10 只需要 metallic 一个 float，但顶点属性不能是 Float1；
  // 用 Float4 会越过 stride 尾部（112-96=16 刚好够），这里改成 Float4 读
  // metallic + _pad[0..2] —— 合法且不越界。
  attr(10, 1, res::VertexFormat::Float4, 96);                // metallic + _pad

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

  d.blend.enabled = true;
  d.blend.src_color = res::BlendFactor::One;
  d.blend.dst_color = res::BlendFactor::OneMinusSrcAlpha;
  d.blend.src_alpha = res::BlendFactor::One;
  d.blend.dst_alpha = res::BlendFactor::OneMinusSrcAlpha;

  d.depth.depthTest = true;
  d.depth.depthWrite = true;
  d.raster.cull = res::CullMode::Back;
  d.samples = static_cast<res::u32>(msaa > 1 ? msaa : 1);
  d.primitive = res::PrimitiveType::TriangleList;
  // push constant：view_proj(64) + normal_matrix(64) = 128（移动端上限）
  d.push_constant_bytes = 128;
  return d;
}

// ---------------------------------------------------------------- MSAA 辅助

// 把 --msaa 落到 TargetDesc，并按设备能力校验。
//
// 两条路径都要拦：CLI 层给一次明确的报错信息，设备层 / OffscreenTarget 还会
// 各自再校验一次（那才是真正的守门人）。这里先拦是为了在**建任何 Vulkan 对象
// 之前**就把错误吐给用户，而不是让它在某个离屏目标创建时才炸。
bool apply_msaa(const dev::IRenderDevice& device, int msaa, dev::TargetDesc* td) {
  if (msaa == 1) return true;  // 缺省路径：一个字段都不动，历史行为零变化
  if (msaa <= 0) {
    std::fprintf(stderr, "--msaa %d 非法：必须是 2 的幂（1/2/4/8）\n", msaa);
    return false;
  }
  if ((msaa & (msaa - 1)) != 0) {
    std::fprintf(stderr, "--msaa %d 非法：不是 2 的幂（合法值 1/2/4/8）\n", msaa);
    return false;
  }
  const dev::DeviceCaps& caps = device.caps();
  bool supported = false;
  for (dev::u32 lvl : caps.supported_msaa_levels) {
    if (lvl == static_cast<dev::u32>(msaa)) supported = true;
  }
  if (!supported) {
    std::fprintf(stderr, "--msaa %d 不被设备支持（%s）。支持列表:", msaa, caps.device_name.c_str());
    for (dev::u32 lvl : caps.supported_msaa_levels) std::fprintf(stderr, " %u", lvl);
    std::fprintf(stderr, "\n");
    return false;
  }
  td->samples = static_cast<dev::u32>(msaa);
  return true;
}

// 把 --msaa 同步到管线描述：render pass 的采样数与管线必须一致。
// samples == 1 时**不写**该字段，保证与引入本参数前完全一致。
void apply_msaa_pipeline(int msaa, res::PipelineDesc* d) {
  if (msaa > 1) d->samples = static_cast<res::u32>(msaa);
}

// ---------------------------------------------------------------- paint

int cmd_paint(const Args& args) {
  if (args.output.empty()) {
    std::fprintf(stderr, "--output 必填\n");
    return 2;
  }

  auto dev_r = dev::create_render_device(software_desc());
  if (!dev_r.ok()) {
    std::fprintf(stderr, "创建设备失败(code=%d): %s\n", static_cast<int>(dev_r.status().code),
                 dev_r.message().c_str());
    return 1;
  }
  dev::IRenderDevice* device = dev_r.value().get();

  // L1 的分配器桶不会自动长页：建任何缓冲之前必须显式预取,
  // 否则小缓冲会报「当前页无空位」(这是 L1 的既定契约,不是 bug)。
  // upload() 内部还要一次 Staging 分配,所以 Staging 桶也要预取。
  {
    res::IMemoryAllocator* alloc = device->memory_allocator();
    for (res::MemoryKind kind :
         {res::MemoryKind::Vertex, res::MemoryKind::Index, res::MemoryKind::Staging}) {
      if (auto s = alloc->reserve_pages(kind, 2); !s.ok()) {
        std::fprintf(stderr, "预取内存页失败: %s\n", s.message.c_str());
        return 1;
      }
    }
  }

  dev::TargetDesc td;
  td.kind = dev::TargetDesc::Kind::Offscreen;
  td.extent = {static_cast<dev::u32>(args.width), static_cast<dev::u32>(args.height)};
  td.with_depth = false;  // 笔迹不需要深度
  if (!apply_msaa(*device, args.msaa, &td)) return 2;
  auto tgt_r = device->create_offscreen_target(td);
  if (!tgt_r.ok()) {
    std::fprintf(stderr, "创建离屏目标失败: %s\n", tgt_r.message().c_str());
    return 1;
  }
  dev::IRenderTarget* target = tgt_r.value().get();

  // ---- 生成笔画几何 ----
  //
  // 用参数化的正弦波画 args.strokes 条笔画。所有坐标由 (i, j, width, height)
  // 与固定常数推导，不含任何随机数生成器的"实现相关"状态 ——
  // 同一版本的 std::mt19937 也可复现，但正弦波更直观也更稳定（不用担心
  // 标准库实现差异），所以选正弦。
  auto painter = mbp::create_painter();
  mbp::StrokeDesc sd;
  sd.width_px = 5.0f;
  sd.round_cap = true;
  sd.smoothing = true;
  sd.max_points_per_stroke = 4096;
  painter->set_desc(sd);

  const float w = static_cast<float>(args.width);
  const float h = static_cast<float>(args.height);

  for (int s = 0; s < args.strokes; ++s) {
    const float t = (args.strokes <= 1) ? 0.5f
                                        : static_cast<float>(s) / static_cast<float>(args.strokes - 1);
    // 每条笔迹一个确定的颜色（黄金分割色相，视觉上互相区分）
    sd.color.r = 0.5f + 0.5f * std::cos(6.2831853f * t);
    sd.color.g = 0.5f + 0.5f * std::cos(6.2831853f * (t + 0.333f));
    sd.color.b = 0.5f + 0.5f * std::cos(6.2831853f * (t + 0.667f));
    sd.color.a = 1.0f;
    painter->set_desc(sd);

    const float base_y = h * (0.2f + 0.6f * t);
    const float amp = h * 0.10f;
    mbp::StrokeEvent begin;
    begin.kind = mbp::StrokeEventKind::Begin;
    begin.x = w * 0.05f;
    begin.y = base_y;
    painter->feed(begin);

    const int kSteps = 64;
    for (int k = 1; k <= kSteps; ++k) {
      const float u = static_cast<float>(k) / static_cast<float>(kSteps);
      mbp::StrokeEvent mv;
      mv.kind = mbp::StrokeEventKind::Move;
      mv.x = w * (0.05f + 0.90f * u);
      mv.y = base_y + amp * std::sin(u * 12.566371f + t * 3.1415926f);
      painter->feed(mv);
    }

    mbp::StrokeEvent end;
    end.kind = mbp::StrokeEventKind::End;
    end.x = w * 0.95f;
    end.y = base_y + amp * std::sin(12.566371f + t * 3.1415926f);
    painter->feed(end);
  }

  const mbp::StrokeGeometry& geo = painter->geometry();
  std::printf("[paint] %d 笔, %zu 顶点, %zu 索引, hash=%016llx\n", args.strokes,
              geo.vertices.size(), geo.indices.size(),
              static_cast<unsigned long long>(painter->geometry_hash()));

  if (geo.vertices.empty()) {
    std::fprintf(stderr, "内部错误:画笔没有产出几何\n");
    return 1;
  }

  // ---- 上传几何到 GPU ----
  const std::vector<mbp::StrokeVertex>& use_verts = geo.vertices;
  const std::vector<std::uint32_t>& use_idx = geo.indices;

  res::BufferDesc vbd;
  vbd.size = use_verts.size() * sizeof(mbp::StrokeVertex);
  vbd.usage = res::BufferUsage::Vertex;
  auto vb_r = device->create_buffer(vbd);
  if (!vb_r.ok()) {
    std::fprintf(stderr, "创建顶点缓冲失败: %s\n", vb_r.message().c_str());
    return 1;
  }
  res::IBuffer* vb = vb_r.value().get();
  if (!vb->upload(use_verts.data(), vbd.size).ok()) {
    std::fprintf(stderr, "写顶点缓冲失败\n");
    return 1;
  }

  res::BufferDesc ibd;
  ibd.size = use_idx.size() * sizeof(std::uint32_t);
  ibd.usage = res::BufferUsage::Index;
  auto ib_r = device->create_buffer(ibd);
  if (!ib_r.ok()) {
    std::fprintf(stderr, "创建索引缓冲失败: %s\n", ib_r.message().c_str());
    return 1;
  }
  res::IBuffer* ib = ib_r.value().get();
  if (!ib->upload(use_idx.data(), ibd.size).ok()) {
    std::fprintf(stderr, "写索引缓冲失败\n");
    return 1;
  }

  // 【必须】upload() 只是把数据入队到 Staging 桶，**不会**拷到 GPU 侧缓冲。
  // 只有 flush_pending() 才真正跑 vkCmdCopyBuffer。漏掉这一步时：
  //   - 没有任何报错；
  //   - 顶点缓冲在 GPU 上仍是全 0 → 所有三角形退化到原点 → 一像素都不画；
  //   - 清屏色照常生效，于是画面看起来"正常但空白"，极难定位。
  // 这正是 L3 CLI 第一版空图的根因，所以这里配一条硬断言。
  vb->flush_pending();
  ib->flush_pending();
  {
    const std::uint64_t vp = vb->pending_upload_bytes();
    const std::uint64_t ip = ib->pending_upload_bytes();
    std::fprintf(stderr, "[paint] 上传批 v=%llu i=%llu (必须都为 0)\n",
                 static_cast<unsigned long long>(vp), static_cast<unsigned long long>(ip));
    if (vp != 0 || ip != 0) {
      std::fprintf(stderr, "上传批未提交:漏了 flush_pending()\n");
      return 1;
    }
  }

  // ---- 建管线 ----
  auto pl_r = device->create_pipeline(stroke_pipeline_desc(args.msaa));
  if (!pl_r.ok()) {
    std::fprintf(stderr, "创建笔画管线失败: %s\n", pl_r.message().c_str());
    return 1;
  }
  res::IPipeline* pipeline = pl_r.value().get();

  // ---- 录制 ----
  auto cmd_r = device->create_command_list();
  if (!cmd_r.ok()) {
    std::fprintf(stderr, "创建命令列表失败: %s\n", cmd_r.message().c_str());
    return 1;
  }
  res::ICommandList* cmd = cmd_r.value().get();

  res::RenderPassDesc rp;
  rp.target = target;
  rp.clear_color = true;
  rp.clear_color_rgba[0] = 0.06f;
  rp.clear_color_rgba[1] = 0.07f;
  rp.clear_color_rgba[2] = 0.10f;
  rp.clear_color_rgba[3] = 1.0f;
  rp.clear_depth = false;
  if (auto s = cmd->begin_pass(rp); !s.ok()) {
    std::fprintf(stderr, "begin_pass 失败: %s\n", s.message.c_str());
    return 1;
  }

  res::Viewport vp;
  vp.width = w;
  vp.height = h;
  vp.min_depth = 0.0f;
  vp.max_depth = 1.0f;
  if (auto s = cmd->set_viewport(vp); !s.ok()) {
    std::fprintf(stderr, "set_viewport 失败: %s\n", s.message.c_str());
    return 1;
  }
  res::ScissorRect sr;
  sr.width = static_cast<res::u32>(args.width);
  sr.height = static_cast<res::u32>(args.height);
  if (auto s = cmd->set_scissor(sr); !s.ok()) {
    std::fprintf(stderr, "set_scissor 失败: %s\n", s.message.c_str());
    return 1;
  }

  if (auto s = cmd->bind_pipeline(pipeline, 0x5354'524F'4B45ull); !s.ok()) {
    std::fprintf(stderr, "bind_pipeline 失败: %s\n", s.message.c_str());
    return 1;
  }

  // push constant：视口尺寸（像素 → NDC 需要）
  struct {
    float viewport_size[2];
    float pad[2];
  } pc;
  pc.viewport_size[0] = w;
  pc.viewport_size[1] = h;
  pc.pad[0] = 0.0f;
  pc.pad[1] = 0.0f;
  constexpr dev::u32 kStageVertex = 0x1u;
  constexpr dev::u32 kStageFragment = 0x10u;
  if (auto s = cmd->push_constants(kStageVertex | kStageFragment, 0, sizeof(pc), &pc); !s.ok()) {
    std::fprintf(stderr, "push_constants 失败: %s\n", s.message.c_str());
    return 1;
  }

  if (auto s = cmd->bind_vertex_buffer(vb->id(), 0, 0); !s.ok()) {
    std::fprintf(stderr, "bind_vertex_buffer 失败: %s\n", s.message.c_str());
    return 1;
  }
  if (auto s = cmd->bind_index_buffer(ib->id(), res::IndexFormat::UInt32, 0); !s.ok()) {
    std::fprintf(stderr, "bind_index_buffer 失败: %s\n", s.message.c_str());
    return 1;
  }

  res::DrawArgs da;
  da.index_count = static_cast<res::u32>(use_idx.size());
  da.instance_count = 1;
  if (auto s = cmd->draw_indexed(da); !s.ok()) {
    std::fprintf(stderr, "draw_indexed 失败: %s\n", s.message.c_str());
    return 1;
  }

  if (auto s = cmd->end_pass(); !s.ok()) {
    std::fprintf(stderr, "end_pass 失败: %s\n", s.message.c_str());
    return 1;
  }

  {
    // 打印实际下发的几何规模与包围盒。这是"画出来了"之外最重要的一条自检：
    // 顶点坐标若落在视口外，着色器再对也不会出现像素，而报错是零条的。
    float minx = 1e9f, maxx = -1e9f, miny = 1e9f, maxy = -1e9f;
    for (const auto& v : geo.vertices) {
      if (v.position[0] < minx) minx = v.position[0];
      if (v.position[0] > maxx) maxx = v.position[0];
      if (v.position[1] < miny) miny = v.position[1];
      if (v.position[1] > maxy) maxy = v.position[1];
    }
    std::fprintf(stderr, "[paint] 顶点 %zu 索引 %zu | 包围盒 x[%.1f,%.1f] y[%.1f,%.1f] | 视口 %ux%u\n",
                 geo.vertices.size(), geo.indices.size(), minx, maxx, miny, maxy, args.width,
                 args.height);
    if (maxx < 0.0f || maxy < 0.0f || minx > static_cast<float>(args.width) ||
        miny > static_cast<float>(args.height)) {
      std::fprintf(stderr, "几何完全落在视口外,不会产生任何像素\n");
      return 1;
    }
  }

  // ---- 提交（每帧恰好一次，UIG2）----
  if (auto s = device->submit_command_list(*cmd); !s.ok()) {
    std::fprintf(stderr, "提交失败: %s\n", s.message.c_str());
    return 1;
  }

  // ---- 读回 + 存 PNG ----
  std::vector<std::uint8_t> rgba(static_cast<std::size_t>(args.width) * args.height * 4u, 0);
  if (auto s = target->readback_rgba8(rgba.data(), rgba.size()); !s.ok()) {
    std::fprintf(stderr, "读回失败: %s\n", s.message.c_str());
    return 1;
  }

  if (!save_png(args.output, rgba, args.width, args.height)) return 1;
  std::printf("[paint] 已写出 %s (%dx%d)\n", args.output.c_str(), args.width, args.height);
  return 0;
}

// ---------------------------------------------------------------- blocks

// ECS + 网格工厂 + 合批器 + 实例缓冲的整条链路。
//
// 这个子命令验证的是 L3 的"3D 积木"主线：world 里放一堆实体 →
// `build_draw_requests` 翻成 DrawRequest → 合批器排序合并 →
// 一次性把实例矩阵写进实例缓冲 → 单次 draw_instanced。
//
// 与 paint 的差别：paint 只有一笔"_笔迹几何_"，blocks 有 N 个"_实体_"，
// 而它们的几何是**同一个立方体网格的不同实例**。这正是 §4.3 合批要解决的
// 问题：N 个实体只该产生 1 次 draw、1 次 pipeline bind、1 次实例缓冲上传。
int cmd_blocks(const Args& args) {
  if (args.output.empty()) {
    std::fprintf(stderr, "--output 必填\n");
    return 2;
  }
  if (args.blocks <= 0) {
    std::fprintf(stderr, "--blocks 必须 > 0\n");
    return 2;
  }

  auto dev_r = dev::create_render_device(software_desc());
  if (!dev_r.ok()) {
    std::fprintf(stderr, "创建设备失败(code=%d): %s\n", static_cast<int>(dev_r.status().code),
                 dev_r.message().c_str());
    return 1;
  }
  dev::IRenderDevice* device = dev_r.value().get();

  {
    res::IMemoryAllocator* alloc = device->memory_allocator();
    for (res::MemoryKind kind :
         {res::MemoryKind::Vertex, res::MemoryKind::Index, res::MemoryKind::Staging}) {
      if (auto s = alloc->reserve_pages(kind, 4); !s.ok()) {
        std::fprintf(stderr, "预取内存页失败: %s\n", s.message.c_str());
        return 1;
      }
    }
  }

  dev::TargetDesc td;
  td.kind = dev::TargetDesc::Kind::Offscreen;
  td.extent = {static_cast<dev::u32>(args.width), static_cast<dev::u32>(args.height)};
  td.with_depth = true;  // 3D 积木必须走深度测试，否则穿透顺序全错
  if (!apply_msaa(*device, args.msaa, &td)) return 2;
  auto tgt_r = device->create_offscreen_target(td);
  if (!tgt_r.ok()) {
    std::fprintf(stderr, "创建离屏目标失败: %s\n", tgt_r.message().c_str());
    return 1;
  }
  dev::IRenderTarget* target = tgt_r.value().get();

  const int w = args.width;
  const int h = args.height;

  // ---- 1) 网格：所有积木共用一个单位立方体 ----
  auto factory = mbp::create_mesh_factory();
  if (!factory) {
    std::fprintf(stderr, "创建网格工厂失败\n");
    return 1;
  }
  const mbp::MeshData cube_data = factory->cube();
  if (cube_data.vertices.empty() || cube_data.indices.empty()) {
    std::fprintf(stderr, "立方体网格为空\n");
    return 1;
  }

  res::BufferDesc vbd;
  vbd.size = cube_data.vertices.size() * sizeof(mbp::Vertex);
  vbd.usage = res::BufferUsage::Vertex;
  auto vb_r = device->create_buffer(vbd);
  if (!vb_r.ok()) {
    std::fprintf(stderr, "创建顶点缓冲失败: %s\n", vb_r.message().c_str());
    return 1;
  }
  res::IBuffer* vb = vb_r.value().get();
  if (!vb->upload(cube_data.vertices.data(), vbd.size).ok()) {
    std::fprintf(stderr, "上传顶点失败\n");
    return 1;
  }

  res::BufferDesc ibd;
  ibd.size = cube_data.indices.size() * sizeof(std::uint32_t);
  ibd.usage = res::BufferUsage::Index;
  auto ib_r = device->create_buffer(ibd);
  if (!ib_r.ok()) {
    std::fprintf(stderr, "创建索引缓冲失败: %s\n", ib_r.message().c_str());
    return 1;
  }
  res::IBuffer* ib = ib_r.value().get();
  if (!ib->upload(cube_data.indices.data(), ibd.size).ok()) {
    std::fprintf(stderr, "上传索引失败\n");
    return 1;
  }

  // upload() 只入队，必须 flush 才真正拷到 GPU（见 paint 里的长注释）。
  vb->flush_pending();
  ib->flush_pending();

  // ---- 2) 相机：一个固定视角的透视相机 ----
  // 不读时钟：视角由 --seed 决定，同样的参数必然得到同样的图（golden 前提）。
  mbp::CameraDesc cd;
  cd.kind = mbp::ProjectionKind::Perspective;
  cd.fov_y_degrees = 50.0f;
  cd.near_plane = 0.1f;
  cd.far_plane = 200.0f;
  cd.viewport.width = static_cast<mbp::u32>(w);
  cd.viewport.height = static_cast<mbp::u32>(h);
  auto camera_owned = mbp::create_camera();
  if (!camera_owned) {
    std::fprintf(stderr, "创建相机失败\n");
    return 1;
  }
  mbp::ICamera* camera = camera_owned.get();
  if (auto s = camera->set_desc(cd); !s.ok()) {
    std::fprintf(stderr, "设置相机参数失败: %s\n", s.message.c_str());
    return 1;
  }

  {
    const float yaw = static_cast<float>(args.seed % 360u) * 0.01745329f;
    mbp::CameraPose pose;
    pose.position[0] = 6.0f * std::sin(yaw);
    pose.position[1] = 4.0f;
    pose.position[2] = 6.0f * std::cos(yaw);
    pose.target[0] = 0.0f;
    pose.target[1] = 0.0f;
    pose.target[2] = 0.0f;
    if (auto s = camera->set_pose(pose); !s.ok()) {
      std::fprintf(stderr, "设置相机位姿失败: %s\n", s.message.c_str());
      return 1;
    }
  }

  // ---- 3) World：生成 N 个实体，每个带 Transform + MeshRef + MaterialRef ----
  auto world = mbp::create_world();
  if (!world) {
    std::fprintf(stderr, "创建 world 失败\n");
    return 1;
  }
  if (auto s = mbp::register_standard_components(*world); !s.ok()) {
    std::fprintf(stderr, "注册标准组件失败: %s\n", s.message.c_str());
    return 1;
  }

  // 材质表：每个槽一个颜色。合批器按 material_key 分组，
  // 所以"用几种材质"直接决定会切几次管线状态。
  auto materials = mbp::create_material_table();
  if (!materials) {
    std::fprintf(stderr, "创建材质表失败\n");
    return 1;
  }
  constexpr int kPaletteCount = 4;
  std::uint32_t palette[kPaletteCount] = {0, 0, 0, 0};
  float palette_rgb[kPaletteCount][3] = {{0.90f, 0.25f, 0.22f},
                                         {0.25f, 0.78f, 0.36f},
                                         {0.26f, 0.45f, 0.92f},
                                         {0.95f, 0.78f, 0.20f}};
  for (int k = 0; k < kPaletteCount; ++k) {
    mbp::MaterialDesc md;
    md.params.base_color[0] = palette_rgb[k][0];
    md.params.base_color[1] = palette_rgb[k][1];
    md.params.base_color[2] = palette_rgb[k][2];
    md.params.base_color[3] = 1.0f;
    md.shader = mbp::make_shader_key(mbp::shader_names::kStandardPbr,
                                  mbp::kShaderFlagVertexNormal);
    auto slot = materials->intern(md);
    if (!slot.ok()) {
      std::fprintf(stderr, "创建材质失败: %s\n", slot.message().c_str());
      return 1;
    }
    palette[k] = slot.value();
  }

  // 网格登记：合批器需要 MeshId → MeshInfo（索引数/首索引/顶点偏移）。
  // 这里只有一个网格，所以登记一次就够。
  const mbp::MeshId kCubeMeshId = 1;
  std::vector<mbp::MeshInfo> mesh_info(2);
  mesh_info[kCubeMeshId].index_count = static_cast<std::uint32_t>(cube_data.indices.size());
  mesh_info[kCubeMeshId].first_index = 0;
  mesh_info[kCubeMeshId].vertex_offset = 0;
  mesh_info[kCubeMeshId].vertex_count = static_cast<std::uint32_t>(cube_data.vertices.size());

  // 立方体按"螺旋立方体堆"摆：位置由序号决定，不读时钟也不读随机源。
  for (int i = 0; i < args.blocks; ++i) {
    auto e = world->spawn();
    if (!e.ok()) {
      std::fprintf(stderr, "spawn 失败: %s\n", e.message().c_str());
      return 1;
    }
    const mbp::EntityId id = e.value();
    if (auto s = world->add_component(id, mbp::TransformComp::kComponentKind); !s.ok()) {
      std::fprintf(stderr, "加 Transform 失败: %s\n", s.message.c_str());
      return 1;
    }
    if (auto s = world->add_component(id, mbp::MeshRefComp::kComponentKind); !s.ok()) {
      std::fprintf(stderr, "加 MeshRef 失败: %s\n", s.message.c_str());
      return 1;
    }
    if (auto s = world->add_component(id, mbp::MaterialRefComp::kComponentKind); !s.ok()) {
      std::fprintf(stderr, "加 MaterialRef 失败: %s\n", s.message.c_str());
      return 1;
    }

    auto* tf = static_cast<mbp::TransformComp*>(
        world->component(id, mbp::TransformComp::kComponentKind));
    auto* mr = static_cast<mbp::MeshRefComp*>(world->component(id, mbp::MeshRefComp::kComponentKind));
    auto* mat = static_cast<mbp::MaterialRefComp*>(
        world->component(id, mbp::MaterialRefComp::kComponentKind));
    if (tf == nullptr || mr == nullptr || mat == nullptr) {
      std::fprintf(stderr, "取组件指针失败\n");
      return 1;
    }

    // 摆成一座"旋转塔"：每层 4 个方块站在同一个圆环上，但整层相对下一层
    // 转 30°。这样从任何视角看，四层的方块都不会被上下层完全遮挡，
    // 每种材质色在画面上都占有可见像素。
    //
    // 为什么不用朴素四角堆叠：四个角上上下下对齐后，透视下前两个方块会把
    // 后两个完全挡住 —— 于是"4 种颜色"在画面上只剩 2 种，看起来像材质
    // 没生效。这是 L3 视觉验证最容易踩的坑，所以位置刻意错开。
    // 位置完全由序号决定，不读时钟/随机源，保证 golden 可复现。
    const int layer = i / 4;
    const int corner = i % 4;
    const float kRingRadius = 1.05f;
    const float kLayerTwist = 0.5236f;  // 30°
    const float angle = static_cast<float>(corner) * 1.5707963f +
                        static_cast<float>(layer) * kLayerTwist;
    tf->translation[0] = kRingRadius * std::cos(angle);
    tf->translation[1] = static_cast<float>(layer) * 1.35f;
    tf->translation[2] = kRingRadius * std::sin(angle);
    tf->scale[0] = 1.0f;
    tf->scale[1] = 1.0f;
    tf->scale[2] = 1.0f;

    mr->mesh = kCubeMeshId;
    mat->material = palette[i % kPaletteCount];
  }

  // ---- 4) 合批：实体 → DrawRequest → 排序合并 ----
  auto builder = mbp::create_batch_builder();
  if (!builder) {
    std::fprintf(stderr, "创建合批器失败\n");
    return 1;
  }
  builder->begin_frame(0);

  std::vector<mbp::Mat4> instance_matrices(static_cast<std::size_t>(args.blocks));
  mbp::DrawRequestBuildDesc bd;
  bd.camera = camera;
  bd.frustum_cull = true;
  bd.mesh_info = &mesh_info;
  bd.out_matrices = instance_matrices.data();
  bd.out_matrix_capacity = instance_matrices.size();
  const mbp::DrawRequestBuildStats bs = mbp::build_draw_requests(*world, *builder, bd);

  if (bs.matrices_written < bs.requests_pushed) {
    std::fprintf(stderr, "实例矩阵容量不足: %u/%u\n", bs.matrices_written, bs.requests_pushed);
    return 1;
  }

  builder->build();
  const std::vector<mbp::BatchDraw>& batches = builder->batches();
  const mbp::BatchStats batch_stats = builder->stats();

  // ---- 5) 实例缓冲：把每个实体的世界矩阵与材质颜色写进去 ----
  //
  // `build_draw_requests` 的 `instance_index` 就是写入 `out_matrices` 的顺序，
  // 也就是实体的遍历顺序；合批只重排**提交顺序**，不会改写矩阵，
  // 所以这里按 0..N-1 直接拼实例即可。
  //
  // 颜色取自该实体 MaterialRefComp 指向的材质槽。之所以要在这里把颜色
  // "烘"进实例属性而不是每槽一次 push_constant：push constant 只有 128 字节
  // (R4)，装不下每实体颜色；而实例属性是每实例的，正好合适。
  std::vector<mbp::InstanceAttribs> instances(instance_matrices.size());
  for (std::size_t i = 0; i < instance_matrices.size(); ++i) {
    instances[i] = mbp::InstanceAttribs{};
    std::memcpy(instances[i].model, instance_matrices[i].m, sizeof(float) * 16);
    // 颜色按 spawn 时的同一个轮换公式取（i % kPaletteCount）。
    // 这里必须与上面生成实体时的取模**完全一致** —— 两处一旦分叉，
    // 画面上就是"颜色配错了实体"，而且没有任何报错。
    const int k = static_cast<int>(i % static_cast<std::size_t>(kPaletteCount));
    instances[i].base_color[0] = palette_rgb[k][0];
    instances[i].base_color[1] = palette_rgb[k][1];
    instances[i].base_color[2] = palette_rgb[k][2];
    instances[i].base_color[3] = 1.0f;
    instances[i].roughness = 0.6f;
    instances[i].metallic = 0.0f;
  }

  res::BufferDesc instd;
  instd.size = instances.size() * sizeof(mbp::InstanceAttribs);
  instd.usage = res::BufferUsage::Vertex;
  auto inst_r = device->create_buffer(instd);
  if (!inst_r.ok()) {
    std::fprintf(stderr, "创建实例缓冲失败: %s\n", inst_r.message().c_str());
    return 1;
  }
  res::IBuffer* inst_buf = inst_r.value().get();
  if (!inst_buf->upload(instances.data(), instd.size).ok()) {
    std::fprintf(stderr, "上传实例失败\n");
    return 1;
  }
  inst_buf->flush_pending();

  // ---- 6) 管线 ----
  auto pl_r = device->create_pipeline(scene_pipeline_desc(args.msaa));
  if (!pl_r.ok()) {
    std::fprintf(stderr, "创建场景管线失败: %s\n", pl_r.message().c_str());
    return 1;
  }
  res::IPipeline* pipeline = pl_r.value().get();

  // ---- 7) 录制 ----
  auto cmd_r = device->create_command_list();
  if (!cmd_r.ok()) {
    std::fprintf(stderr, "创建命令列表失败: %s\n", cmd_r.message().c_str());
    return 1;
  }
  res::ICommandList* cmd = cmd_r.value().get();

  res::RenderPassDesc rp;
  rp.target = target;
  rp.clear_color = true;
  rp.clear_color_rgba[0] = 0.06f;
  rp.clear_color_rgba[1] = 0.07f;
  rp.clear_color_rgba[2] = 0.10f;
  rp.clear_color_rgba[3] = 1.0f;
  rp.clear_depth = true;
  rp.clear_depth_value = 1.0f;
  if (auto s = cmd->begin_pass(rp); !s.ok()) {
    std::fprintf(stderr, "begin_pass 失败: %s\n", s.message.c_str());
    return 1;
  }

  res::Viewport vp;
  vp.width = static_cast<float>(w);
  vp.height = static_cast<float>(h);
  vp.min_depth = 0.0f;
  vp.max_depth = 1.0f;
  if (auto s = cmd->set_viewport(vp); !s.ok()) {
    std::fprintf(stderr, "set_viewport 失败: %s\n", s.message.c_str());
    return 1;
  }
  res::ScissorRect sr;
  sr.width = static_cast<res::u32>(w);
  sr.height = static_cast<res::u32>(h);
  if (auto s = cmd->set_scissor(sr); !s.ok()) {
    std::fprintf(stderr, "set_scissor 失败: %s\n", s.message.c_str());
    return 1;
  }

  if (auto s = cmd->bind_pipeline(pipeline, 0x4c33'5343'454e'45ull); !s.ok()) {
    std::fprintf(stderr, "bind_pipeline 失败: %s\n", s.message.c_str());
    return 1;
  }

  // push constant：view_proj(64) + normal_matrix(64) = 128 字节（移动端上限）
  struct ScenePC {
    float view_proj[16];
    float normal_matrix[16];
  } pc{};
  std::memcpy(pc.view_proj, camera->view_proj_matrix().m, sizeof(float) * 16);
  // 法线矩阵：本 demo 只有缩放=1 的刚体，直接取 view_proj 的旋转部分即可；
  // 严格做法是 transpose(inverse(model))，见 L3 spec §6.2.2。
  // 这里用一个简单的单位矩阵占位，避免与 view_proj 搞混。
  pc.normal_matrix[0] = 1.0f;
  pc.normal_matrix[5] = 1.0f;
  pc.normal_matrix[10] = 1.0f;
  pc.normal_matrix[15] = 1.0f;
  constexpr dev::u32 kStageVertex = 0x1u;
  constexpr dev::u32 kStageFragment = 0x10u;
  if (auto s = cmd->push_constants(kStageVertex | kStageFragment, 0, sizeof(pc), &pc); !s.ok()) {
    std::fprintf(stderr, "push_constants 失败: %s\n", s.message.c_str());
    return 1;
  }

  if (auto s = cmd->bind_vertex_buffer(vb->id(), 0, 0); !s.ok()) {
    std::fprintf(stderr, "bind 顶点缓冲失败: %s\n", s.message.c_str());
    return 1;
  }
  if (auto s = cmd->bind_vertex_buffer(inst_buf->id(), 1, 0); !s.ok()) {
    std::fprintf(stderr, "bind 实例缓冲失败: %s\n", s.message.c_str());
    return 1;
  }
  if (auto s = cmd->bind_index_buffer(ib->id(), res::IndexFormat::UInt32, 0); !s.ok()) {
    std::fprintf(stderr, "bind 索引缓冲失败: %s\n", s.message.c_str());
    return 1;
  }

  res::DrawArgs da;
  da.index_count = static_cast<res::u32>(cube_data.indices.size());
  da.instance_count = static_cast<res::u32>(instances.size());
  if (auto s = cmd->draw_indexed(da); !s.ok()) {
    std::fprintf(stderr, "draw_indexed 失败: %s\n", s.message.c_str());
    return 1;
  }

  if (auto s = cmd->end_pass(); !s.ok()) {
    std::fprintf(stderr, "end_pass 失败: %s\n", s.message.c_str());
    return 1;
  }

  if (auto s = device->submit_command_list(*cmd); !s.ok()) {
    std::fprintf(stderr, "提交失败: %s\n", s.message.c_str());
    return 1;
  }

  // ---- 8) 读回 + 存 PNG ----
  std::vector<std::uint8_t> rgba(static_cast<std::size_t>(w) * h * 4u, 0);
  if (auto s = target->readback_rgba8(rgba.data(), rgba.size()); !s.ok()) {
    std::fprintf(stderr, "读回失败: %s\n", s.message.c_str());
    return 1;
  }

  magicblocks::ImageRgba8 img;
  img.width = static_cast<std::uint32_t>(w);
  img.height = static_cast<std::uint32_t>(h);
  img.pixels = std::move(rgba);
  std::string err;
  if (!magicblocks::write_png_rgba8(args.output, img, &err)) {
    std::fprintf(stderr, "写 PNG 失败: %s\n", err.c_str());
    return 1;
  }

  std::printf("[blocks] %d 个体素 | 实体 %u → 请求 %u → 合批 %u (合并 %u) | 管线切换 %u\n",
              args.blocks, world->entity_count(), bs.requests_pushed, batch_stats.batch_count,
              batch_stats.merged_requests, batch_stats.pipeline_switches);
  std::printf("[blocks] 已写出 %s (%dx%d)\n", args.output.c_str(), w, h);
  return 0;
}

// ---------------------------------------------------------------- gizmo

// 覆盖层管线：屏幕空间纯色三角形，无深度测试、无背面剔除、预乘 alpha。
//
// 与 scene_pipeline_desc 的关键差别：**depthTest 必须关**。
// 覆盖层画在场景之上（gizmo 永远可见）；如果开深度测试，被物体挡住的轴
// 会消失 —— 看起来像"gizmo 坏了"。
res::PipelineDesc overlay_pipeline_desc(int msaa = 1) {
  res::PipelineDesc d;

  // binding 0：每顶点，stride = sizeof(OverlayVertex) = 24
  res::VertexInputBinding vb;
  vb.binding = 0;
  vb.stride = sizeof(mbp::OverlayVertex);
  vb.instanced = false;
  d.vertexLayout.bindings.push_back(vb);

  // location 0 = position(float2, 像素坐标)  location 1 = color(ubyte4norm)
  {
    res::VertexAttribute a;
    a.location = 0;
    a.binding = 0;
    a.format = res::VertexFormat::Float2;
    a.offset = 0;
    d.vertexLayout.attributes.push_back(a);
  }
  {
    res::VertexAttribute a;
    a.location = 1;
    a.binding = 0;
    a.format = res::VertexFormat::UByte4Norm;
    a.offset = 8;
    d.vertexLayout.attributes.push_back(a);
  }

  {
    res::ShaderModule m;
    m.stage = res::ShaderStage::Vertex;
    m.spirv = reinterpret_cast<const std::uint8_t*>(magicblocks::shaders::kOverlay2dVert);
    m.size_bytes = magicblocks::shaders::kOverlay2dVertSize;
    d.stages.push_back(m);
  }
  {
    res::ShaderModule m;
    m.stage = res::ShaderStage::Fragment;
    m.spirv = reinterpret_cast<const std::uint8_t*>(magicblocks::shaders::kOverlay2dFrag);
    m.size_bytes = magicblocks::shaders::kOverlay2dFragSize;
    d.stages.push_back(m);
  }

  d.blend.enabled = true;
  d.blend.src_color = res::BlendFactor::One;
  d.blend.dst_color = res::BlendFactor::OneMinusSrcAlpha;
  d.blend.src_alpha = res::BlendFactor::One;
  d.blend.dst_alpha = res::BlendFactor::OneMinusSrcAlpha;

  // 覆盖层永远画在最上面：不测深度、也不写深度
  d.depth.depthTest = false;
  d.depth.depthWrite = false;
  d.raster.cull = res::CullMode::None;
  d.samples = static_cast<res::u32>(msaa > 1 ? msaa : 1);
  d.primitive = res::PrimitiveType::TriangleList;
  // push constant：只有 viewport_size (float2) = 8 字节
  d.push_constant_bytes = 8;
  return d;
}

// 把"一块积木 + 一个 gizmo"画出来，gizmo 的交互**完全由事件流驱动**。
//
// 这是 M3 的验收项：拖动事件序列可录制/回放，且 gizmo 改动零处进入玩法代码。
// 这里的"玩法代码"是下面的 DemoPlayer（一个 IGizmoListener 实现），
// 它自己决定把 Drag 增量加到哪个实体的 Transform 上 —— gizmo 不知道也不关心。
int cmd_gizmo(const Args& args) {
  if (args.output.empty()) {
    std::fprintf(stderr, "--output 必填\n");
    return 2;
  }

  auto dev_r = dev::create_render_device(software_desc());
  if (!dev_r.ok()) {
    std::fprintf(stderr, "创建设备失败(code=%d): %s\n", static_cast<int>(dev_r.status().code),
                 dev_r.message().c_str());
    return 1;
  }
  dev::IRenderDevice* device = dev_r.value().get();

  {
    res::IMemoryAllocator* alloc = device->memory_allocator();
    for (res::MemoryKind kind :
         {res::MemoryKind::Vertex, res::MemoryKind::Index, res::MemoryKind::Staging}) {
      if (auto s = alloc->reserve_pages(kind, 4); !s.ok()) {
        std::fprintf(stderr, "预取内存页失败: %s\n", s.message.c_str());
        return 1;
      }
    }
  }

  const int w = args.width;
  const int h = args.height;

  dev::TargetDesc td;
  td.kind = dev::TargetDesc::Kind::Offscreen;
  td.extent = {static_cast<dev::u32>(w), static_cast<dev::u32>(h)};
  td.with_depth = true;
  if (!apply_msaa(*device, args.msaa, &td)) return 2;
  auto tgt_r = device->create_offscreen_target(td);
  if (!tgt_r.ok()) {
    std::fprintf(stderr, "创建离屏目标失败: %s\n", tgt_r.message().c_str());
    return 1;
  }
  dev::IRenderTarget* target = tgt_r.value().get();

  // ---- 相机 ----
  auto camera_owned = mbp::create_camera();
  if (!camera_owned) {
    std::fprintf(stderr, "创建相机失败\n");
    return 1;
  }
  mbp::ICamera* camera = camera_owned.get();
  {
    mbp::CameraDesc cd;
    cd.kind = mbp::ProjectionKind::Perspective;
    cd.fov_y_degrees = 50.0f;
    cd.near_plane = 0.1f;
    cd.far_plane = 200.0f;
    cd.viewport.width = static_cast<mbp::u32>(w);
    cd.viewport.height = static_cast<mbp::u32>(h);
    if (auto s = camera->set_desc(cd); !s.ok()) {
      std::fprintf(stderr, "设置相机参数失败: %s\n", s.message.c_str());
      return 1;
    }
    mbp::CameraPose pose;
    pose.position = {4.5f, 3.5f, 7.0f};
    pose.target = {0.0f, 0.6f, 0.0f};
    pose.up = {0.0f, 1.0f, 0.0f};
    if (auto s = camera->set_pose(pose); !s.ok()) {
      std::fprintf(stderr, "设置相机位姿失败: %s\n", s.message.c_str());
      return 1;
    }
  }

  // ---- World：一块被 gizmo 操作的积木 ----
  auto world = mbp::create_world();
  if (!world) {
    std::fprintf(stderr, "创建 world 失败\n");
    return 1;
  }
  if (auto s = mbp::register_standard_components(*world); !s.ok()) {
    std::fprintf(stderr, "注册标准组件失败: %s\n", s.message.c_str());
    return 1;
  }

  auto factory = mbp::create_mesh_factory();
  if (!factory) {
    std::fprintf(stderr, "创建网格工厂失败\n");
    return 1;
  }
  const mbp::MeshData cube_data = factory->cube();

  const mbp::MeshId kCubeMeshId = 1;
  std::vector<mbp::MeshInfo> mesh_info(2);
  mesh_info[kCubeMeshId].index_count = static_cast<std::uint32_t>(cube_data.indices.size());
  mesh_info[kCubeMeshId].vertex_count = static_cast<std::uint32_t>(cube_data.vertices.size());

  auto materials = mbp::create_material_table();
  if (!materials) {
    std::fprintf(stderr, "创建材质表失败\n");
    return 1;
  }
  mbp::MaterialDesc md;
  md.params.base_color[0] = 0.35f;
  md.params.base_color[1] = 0.62f;
  md.params.base_color[2] = 0.92f;
  md.params.base_color[3] = 1.0f;
  md.shader = mbp::make_shader_key(mbp::shader_names::kStandardPbr, mbp::kShaderFlagVertexNormal);
  auto slot_r = materials->intern(md);
  if (!slot_r.ok()) {
    std::fprintf(stderr, "创建材质失败: %s\n", slot_r.message().c_str());
    return 1;
  }
  const std::uint32_t kSlot = slot_r.value();

  mbp::EntityId box_entity{};
  {
    auto e = world->spawn();
    if (!e.ok()) {
      std::fprintf(stderr, "spawn 失败: %s\n", e.message().c_str());
      return 1;
    }
    box_entity = e.value();
    world->add_component(box_entity, mbp::TransformComp::kComponentKind);
    world->add_component(box_entity, mbp::MeshRefComp::kComponentKind);
    world->add_component(box_entity, mbp::MaterialRefComp::kComponentKind);
    auto* tf = static_cast<mbp::TransformComp*>(
        world->component(box_entity, mbp::TransformComp::kComponentKind));
    auto* mr = static_cast<mbp::MeshRefComp*>(
        world->component(box_entity, mbp::MeshRefComp::kComponentKind));
    auto* mat = static_cast<mbp::MaterialRefComp*>(
        world->component(box_entity, mbp::MaterialRefComp::kComponentKind));
    if (tf == nullptr || mr == nullptr || mat == nullptr) {
      std::fprintf(stderr, "取组件指针失败\n");
      return 1;
    }
    mr->mesh = kCubeMeshId;
    mat->material = kSlot;
  }

  // ---- gizmo：三个轴，由**数据**声明 ----
  auto gizmo = mbp::create_gizmo_system();
  if (!gizmo) {
    std::fprintf(stderr, "创建 gizmo 失败\n");
    return 1;
  }
  mbp::GizmoTargetDesc gd;
  gd.mode = mbp::GizmoMode::Translate;
  gd.center[0] = 0.0f;
  gd.center[1] = 0.0f;
  gd.center[2] = 0.0f;
  gd.screen_radius_px = 90.0f;
  {
    mbp::GizmoAxis ax;
    ax.id = 0;
    ax.direction[0] = 1.0f;
    ax.color = {0.95f, 0.25f, 0.22f, 1.0f};
    gd.axes.push_back(ax);
    mbp::GizmoAxis ay;
    ay.id = 1;
    ay.direction[1] = 1.0f;
    ay.color = {0.30f, 0.90f, 0.32f, 1.0f};
    gd.axes.push_back(ay);
    mbp::GizmoAxis az;
    az.id = 2;
    az.direction[2] = 1.0f;
    az.color = {0.30f, 0.48f, 0.98f, 1.0f};
    gd.axes.push_back(az);
  }
  const std::uint32_t kGizmoTarget = gizmo->configure(gd);

  // ---- 玩法侧：唯一订阅 gizmo 的地方 ----
  // gizmo 不知道自己被谁用、改的是什么。它只发事件，这里自己改 world。
  struct DemoPlayer final : public mbp::IGizmoListener {
    mbp::IWorld* world = nullptr;
    mbp::EntityId entity{};
    std::vector<mbp::GizmoEvent> log;
    void on_gizmo(const mbp::GizmoEvent& e) override {
      log.push_back(e);
      if (world == nullptr) return;
      if (e.kind != mbp::GizmoEvent::Kind::Drag) return;
      auto* tf = static_cast<mbp::TransformComp*>(
          world->component(entity, mbp::TransformComp::kComponentKind));
      if (tf == nullptr) return;
      // 玩法自己的规则：只接受 X/Y 轴的平移，Z 轴忽略。
      // 这条规则完全在玩法侧 —— gizmo 里没有任何对应分支。
      if (e.axis == 0) tf->translation[0] += e.delta[0];
      if (e.axis == 1) tf->translation[1] += e.delta[1];
    }
  } player;
  player.world = world.get();
  player.entity = box_entity;
  gizmo->subscribe(&player);

  // ---- 事件流：录制 → 回放 ----
  // 事件全部由参数推导（不读鼠标），因此同参数必然得到同一张图。
  dev::EventStream stream;
  {
    const float cx = static_cast<float>(w) * 0.5f;
    const float cy = static_cast<float>(h) * 0.5f;
    auto push = [&](dev::InputEventKind kind, float x, float y, std::int64_t ts) {
      dev::InputEvent e;
      e.kind = kind;
      e.position = {x, y};
      e.timestamp_us = ts;
      stream.append(e);
    };

    // X 轴在屏幕上的方向：把世界 (len,0,0) 投到屏幕
    const float axis_len = gizmo->axis_world_length(kGizmoTarget, 0, *camera);
    float ax = cx;
    float ay = cy;
    float ad = 0.0f;
    if (camera->world_to_screen({axis_len * 0.55f, 0.0f, 0.0f}, &ax, &ay, &ad)) {
      // 悬停到轴上 → 按下 → 沿 +x 拖 42 像素 → 松开
      push(dev::InputEventKind::PointerMove, ax, ay, 1'000'000);
      push(dev::InputEventKind::PointerDown, ax, ay, 1'010'000);
      for (int i = 1; i <= 6; ++i) {
        push(dev::InputEventKind::PointerMove, ax + static_cast<float>(i) * 7.0f, ay,
             1'010'000 + i * 16'666);
      }
      push(dev::InputEventKind::PointerUp, ax + 42.0f, ay, 1'120'000);
    }
  }

  // 回放事件流（gizmo 的唯一输入入口）
  stream.rewind();
  {
    dev::InputEvent e;
    while (stream.next(&e)) {
      gizmo->on_input(e, *camera);
    }
  }
  if (player.log.empty()) {
    std::fprintf(stderr, "事件流没有产生任何 gizmo 事件（录制段为空？）\n");
    return 1;
  }

  // ---- 场景合批 ----
  auto builder = mbp::create_batch_builder();
  if (!builder) {
    std::fprintf(stderr, "创建合批器失败\n");
    return 1;
  }
  builder->begin_frame(0);

  std::vector<mbp::Mat4> instance_matrices(1);
  mbp::DrawRequestBuildDesc bd;
  bd.camera = camera;
  bd.frustum_cull = false;  // 只有一块积木，剔除没意义且可能误删
  bd.mesh_info = &mesh_info;
  bd.out_matrices = instance_matrices.data();
  bd.out_matrix_capacity = instance_matrices.size();
  mbp::build_draw_requests(*world, *builder, bd);
  builder->build();

  // ---- 上缓冲 ----
  res::BufferDesc vbd;
  vbd.size = cube_data.vertices.size() * sizeof(mbp::Vertex);
  vbd.usage = res::BufferUsage::Vertex;
  auto vb_r = device->create_buffer(vbd);
  if (!vb_r.ok()) {
    std::fprintf(stderr, "创建顶点缓冲失败: %s\n", vb_r.message().c_str());
    return 1;
  }
  res::IBuffer* vb = vb_r.value().get();
  vb->upload(cube_data.vertices.data(), vbd.size);

  res::BufferDesc ibd;
  ibd.size = cube_data.indices.size() * sizeof(std::uint32_t);
  ibd.usage = res::BufferUsage::Index;
  auto ib_r = device->create_buffer(ibd);
  if (!ib_r.ok()) {
    std::fprintf(stderr, "创建索引缓冲失败: %s\n", ib_r.message().c_str());
    return 1;
  }
  res::IBuffer* ib = ib_r.value().get();
  ib->upload(cube_data.indices.data(), ibd.size);

  mbp::InstanceAttribs inst{};
  std::memcpy(inst.model, instance_matrices[0].m, sizeof(float) * 16);
  inst.base_color[0] = 1.0f;
  inst.base_color[1] = 1.0f;
  inst.base_color[2] = 1.0f;
  inst.base_color[3] = 1.0f;
  inst.roughness = 0.5f;
  inst.metallic = 0.0f;

  res::BufferDesc instd;
  instd.size = sizeof(mbp::InstanceAttribs);
  instd.usage = res::BufferUsage::Vertex;
  auto inst_r = device->create_buffer(instd);
  if (!inst_r.ok()) {
    std::fprintf(stderr, "创建实例缓冲失败: %s\n", inst_r.message().c_str());
    return 1;
  }
  res::IBuffer* inst_buf = inst_r.value().get();
  inst_buf->upload(&inst, instd.size);

  // **关键**：upload() 只入队，必须 flush 才真正拷到 GPU。
  vb->flush_pending();
  ib->flush_pending();
  inst_buf->flush_pending();
  if (vb->pending_upload_bytes() != 0 || ib->pending_upload_bytes() != 0 ||
      inst_buf->pending_upload_bytes() != 0) {
    std::fprintf(stderr, "上传未 flush —— GPU 侧会是全零缓冲，画不出任何东西\n");
    return 1;
  }

  // ---- 覆盖层：gizmo 自己往 IOverlay2D 里 push ----
  auto overlay = mbp::create_overlay2d();
  if (!overlay) {
    std::fprintf(stderr, "创建覆盖层失败\n");
    return 1;
  }
  overlay->set_viewport(static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h));
  overlay->begin_frame();
  overlay->reserve(64, 8);
  if (auto s = gizmo->build_draw(*camera, *overlay); !s.ok()) {
    std::fprintf(stderr, "gizmo build_draw 失败: %s\n", s.message.c_str());
    return 1;
  }
  overlay->build();
  if (overlay->geometry().vertices.empty()) {
    std::fprintf(stderr, "覆盖层几何为空 —— gizmo 没画出东西\n");
    return 1;
  }

  res::BufferDesc ovb;
  ovb.size = overlay->geometry().vertices.size() * sizeof(mbp::OverlayVertex);
  ovb.usage = res::BufferUsage::Vertex;
  auto ovb_r = device->create_buffer(ovb);
  if (!ovb_r.ok()) {
    std::fprintf(stderr, "创建覆盖层顶点缓冲失败: %s\n", ovb_r.message().c_str());
    return 1;
  }
  res::IBuffer* ovb_buf = ovb_r.value().get();
  ovb_buf->upload(overlay->geometry().vertices.data(), ovb.size);

  res::BufferDesc oib;
  oib.size = overlay->geometry().indices.size() * sizeof(std::uint32_t);
  oib.usage = res::BufferUsage::Index;
  auto oib_r = device->create_buffer(oib);
  if (!oib_r.ok()) {
    std::fprintf(stderr, "创建覆盖层索引缓冲失败: %s\n", oib_r.message().c_str());
    return 1;
  }
  res::IBuffer* oib_buf = oib_r.value().get();
  oib_buf->upload(overlay->geometry().indices.data(), oib.size);
  ovb_buf->flush_pending();
  oib_buf->flush_pending();

  // ---- 管线 ----
  auto scene_pl = device->create_pipeline(scene_pipeline_desc(args.msaa));
  if (!scene_pl.ok()) {
    std::fprintf(stderr, "创建场景管线失败: %s\n", scene_pl.message().c_str());
    return 1;
  }
  auto overlay_pl = device->create_pipeline(overlay_pipeline_desc(args.msaa));
  if (!overlay_pl.ok()) {
    std::fprintf(stderr, "创建覆盖层管线失败: %s\n", overlay_pl.message().c_str());
    return 1;
  }

  // ---- 录制 ----
  auto cmd_r = device->create_command_list();
  if (!cmd_r.ok()) {
    std::fprintf(stderr, "创建命令列表失败: %s\n", cmd_r.message().c_str());
    return 1;
  }
  res::ICommandList* cmd = cmd_r.value().get();

  res::RenderPassDesc rp;
  rp.target = target;
  rp.clear_color = true;
  rp.clear_color_rgba[0] = 0.06f;
  rp.clear_color_rgba[1] = 0.07f;
  rp.clear_color_rgba[2] = 0.10f;
  rp.clear_color_rgba[3] = 1.0f;
  rp.clear_depth = true;
  rp.clear_depth_value = 1.0f;
  if (!cmd->begin_pass(rp).ok()) {
    std::fprintf(stderr, "begin_pass 失败\n");
    return 1;
  }

  res::Viewport vp;
  vp.width = static_cast<float>(w);
  vp.height = static_cast<float>(h);
  vp.min_depth = 0.0f;
  vp.max_depth = 1.0f;
  cmd->set_viewport(vp);
  res::ScissorRect sr;
  sr.width = static_cast<res::u32>(w);
  sr.height = static_cast<res::u32>(h);
  cmd->set_scissor(sr);

  constexpr dev::u32 kStageVertex = 0x1u;
  constexpr dev::u32 kStageFragment = 0x10u;

  // 1) 场景：一块积木
  if (!cmd->bind_pipeline(scene_pl.value().get(), 0x4c33'5343'454e'45ull).ok()) {
    std::fprintf(stderr, "bind 场景管线失败\n");
    return 1;
  }
  {
    struct ScenePC {
      float view_proj[16];
      float normal_matrix[16];
    } pc{};
    std::memcpy(pc.view_proj, camera->view_proj_matrix().m, sizeof(float) * 16);
    pc.normal_matrix[0] = 1.0f;
    pc.normal_matrix[5] = 1.0f;
    pc.normal_matrix[10] = 1.0f;
    pc.normal_matrix[15] = 1.0f;
    if (!cmd->push_constants(kStageVertex | kStageFragment, 0, sizeof(pc), &pc).ok()) {
      std::fprintf(stderr, "push_constants(scene) 失败\n");
      return 1;
    }
  }
  cmd->bind_vertex_buffer(vb->id(), 0, 0);
  cmd->bind_vertex_buffer(inst_buf->id(), 1, 0);
  cmd->bind_index_buffer(ib->id(), res::IndexFormat::UInt32, 0);
  {
    res::DrawArgs da;
    da.index_count = static_cast<res::u32>(cube_data.indices.size());
    da.instance_count = 1;
    if (!cmd->draw_indexed(da).ok()) {
      std::fprintf(stderr, "draw_indexed(scene) 失败\n");
      return 1;
    }
  }

  // 2) 覆盖层：gizmo 全部几何，一次 draw
  if (!cmd->bind_pipeline(overlay_pl.value().get(), 0x4f56'524c'4159'0001ull).ok()) {
    std::fprintf(stderr, "bind 覆盖层管线失败\n");
    return 1;
  }
  {
    float vp_size[2] = {static_cast<float>(w), static_cast<float>(h)};
    if (!cmd->push_constants(kStageVertex | kStageFragment, 0, sizeof(vp_size), vp_size).ok()) {
      std::fprintf(stderr, "push_constants(overlay) 失败\n");
      return 1;
    }
  }
  cmd->bind_vertex_buffer(ovb_buf->id(), 0, 0);
  cmd->bind_index_buffer(oib_buf->id(), res::IndexFormat::UInt32, 0);
  {
    res::DrawArgs da;
    da.index_count = static_cast<res::u32>(overlay->geometry().indices.size());
    da.instance_count = 1;
    if (!cmd->draw_indexed(da).ok()) {
      std::fprintf(stderr, "draw_indexed(overlay) 失败\n");
      return 1;
    }
  }

  if (!cmd->end_pass().ok()) {
    std::fprintf(stderr, "end_pass 失败\n");
    return 1;
  }
  if (auto s = device->submit_command_list(*cmd); !s.ok()) {
    std::fprintf(stderr, "提交失败: %s\n", s.message.c_str());
    return 1;
  }

  // ---- 读回 + PNG ----
  std::vector<std::uint8_t> rgba(static_cast<std::size_t>(w) * h * 4u, 0);
  if (auto s = target->readback_rgba8(rgba.data(), rgba.size()); !s.ok()) {
    std::fprintf(stderr, "读回失败: %s\n", s.message.c_str());
    return 1;
  }
  magicblocks::ImageRgba8 img;
  img.width = static_cast<std::uint32_t>(w);
  img.height = static_cast<std::uint32_t>(h);
  img.pixels = std::move(rgba);
  std::string err;
  if (!magicblocks::write_png_rgba8(args.output, img, &err)) {
    std::fprintf(stderr, "写 PNG 失败: %s\n", err.c_str());
    return 1;
  }

  // ---- 统计 ----
  std::size_t drag_count = 0;
  for (const auto& e : player.log) {
    if (e.kind == mbp::GizmoEvent::Kind::Drag) ++drag_count;
  }
  const mbp::OverlayStats& os = overlay->stats();
  const auto* tf = static_cast<const mbp::TransformComp*>(
      world->component(box_entity, mbp::TransformComp::kComponentKind));
  std::printf("[gizmo] 事件流 %zu 条 → gizmo 事件 %zu 条 (Drag %zu)\n", stream.size(),
              player.log.size(), drag_count);
  std::printf("[gizmo] 积木 X 位移 %.4f（由订阅者应用，gizmo 未碰世界）\n",
              tf != nullptr ? tf->translation[0] : 0.0f);
  std::printf("[gizmo] 覆盖层 %u 线 / %u 环 / %u 三角 → %u 顶点 %u 索引, 1 次 draw\n",
              os.line_count, os.ring_count, os.tri_count, os.vertex_count, os.index_count);
  std::printf("[gizmo] 已写出 %s (%dx%d)\n", args.output.c_str(), w, h);
  return 0;
}

// MagicBlocks L4 F-SK —— 涂鸦子命令实现体（被 patch_cli2.py 注入到 l3_cli/main.cpp）
//
// 这一步是 L4 玩法"能跑通"的唯一硬证据：前面所有单测都只证明几何算对了，
// 只有出图才能证明「文档 → 轮廓 → 挤出 → 上传 → 录制 → 提交 → 读回」
// 整条链路是通的。
//
// 确定性：所有坐标由 (--strokes, --sketch-pattern) 与固定常数推导，
// 不用随机数、不读时间，所以同样参数产出逐字节相同的 PNG（golden 前提）。

// 在画布平面（XY）上生成一组确定的笔画。三种 pattern 覆盖三条主要分支：
//   0 = 波浪线（Ribbon，"路径"）
//   1 = 闭合方框（Region，"路径包围的布尔区域"）
//   2 = 交叉两笔（验证 merge_strokes 的并集）
std::vector<featsketch::Stroke> make_sketch_strokes(int strokes, int pattern) {
  std::vector<featsketch::Stroke> out;
  if (strokes <= 0) return out;

  for (int i = 0; i < strokes; ++i) {
    const float t = (strokes <= 1) ? 0.5f
                                   : static_cast<float>(i) / static_cast<float>(strokes - 1);

    featsketch::Stroke s;
    s.width = 0.10f;
    s.round_cap = true;
    // 每笔一个确定颜色（黄金分割色相，视觉上互相区分）
    s.color.r = 0.5f + 0.5f * std::cos(6.2831853f * t);
    s.color.g = 0.5f + 0.5f * std::cos(6.2831853f * (t + 0.333f));
    s.color.b = 0.5f + 0.5f * std::cos(6.2831853f * (t + 0.667f));
    s.color.a = 1.0f;

    switch (pattern) {
      case 1: {
        s.style = featsketch::PathStyle::Region;
        s.closed = true;
        const float cx = (t - 0.5f) * 1.0f;
        const float half = 0.26f;
        s.points = {{cx - half, -half}, {cx + half, -half}, {cx + half, half},
                    {cx - half, half},  {cx - half, -half}};  // 显式闭合
        break;
      }
      case 2: {
        s.style = featsketch::PathStyle::Ribbon;
        const float off = (t - 0.5f) * 0.8f;
        if (i % 2 == 0) {
          s.points = {{-0.7f, -0.5f + off}, {0.7f, 0.5f + off}};
        } else {
          s.points = {{-0.7f, 0.5f + off}, {0.7f, -0.5f + off}};
        }
        break;
      }
      default: {
        s.style = featsketch::PathStyle::Ribbon;
        const float base_y = (t - 0.5f) * 1.1f;
        constexpr int kSteps = 32;
        s.points.reserve(kSteps + 1);
        for (int k = 0; k <= kSteps; ++k) {
          const float u = static_cast<float>(k) / static_cast<float>(kSteps);
          s.points.push_back({-0.8f + 1.6f * u,
                              base_y + 0.16f * std::sin(u * 9.42478f + t * 3.14159f)});
        }
        break;
      }
    }
    out.push_back(std::move(s));
  }
  return out;
}

int cmd_sketch(const Args& args) {
  if (args.output.empty()) {
    std::fprintf(stderr, "--output 必填\n");
    return 2;
  }
  if (args.width <= 0 || args.height <= 0) {
    std::fprintf(stderr, "--width/--height 必须为正\n");
    return 2;
  }

  // ---- 1) 构造文档（纯 CPU，不碰设备）----
  featsketch::SketchDocument doc;
  for (auto& s : make_sketch_strokes(args.strokes, args.sketch_pattern)) {
    if (!doc.add_stroke(std::move(s)).valid()) {
      std::fprintf(stderr, "添加笔画失败(参数非法)\n");
      return 1;
    }
  }

  // ---- 2) 文档 → 轮廓 → 挤出网格 ----
  featsketch::BuildOptions bopts;
  bopts.thickness = 0.12f;
  bopts.merge_strokes = args.merge;
  const featsketch::SketchMeshSet set = featsketch::build_meshes(doc, bopts);

  // 失败必须响：不支持的 RegionOp / 未闭合 Region 等在这里就终止。
  // 绝不"带着错误继续出图"——那样 golden 会把一个错误状态锁死。
  if (!set.ok()) {
    std::fprintf(stderr, "[sketch] 管线报错 %zu 条:\n", set.errors.size());
    for (const auto& e : set.errors) {
      std::fprintf(stderr, "  [code=%d] stroke#%llu %s\n", static_cast<int>(e.code),
                   static_cast<unsigned long long>(e.stroke.value), e.detail.c_str());
    }
    return 1;
  }
  if (set.meshes.empty()) {
    std::fprintf(stderr, "[sketch] 没有产生任何实体,拒绝出图(--strokes 是否为 0?)\n");
    return 1;
  }

  std::printf(
      "[sketch] 笔画 %zu | 实体 %zu | 顶点 %zu 索引 %zu | doc_hash=%016llx mesh_hash=%016llx\n",
      doc.stroke_count(), set.meshes.size(), set.vertex_count(), set.triangle_count() * 3,
      static_cast<unsigned long long>(doc.hash()),
      static_cast<unsigned long long>(set.mesh_hash));

  // ---- 3) 设备 + 离屏目标 ----
  auto dev_r = dev::create_render_device(software_desc());
  if (!dev_r.ok()) {
    std::fprintf(stderr, "创建设备失败(code=%d): %s\n", static_cast<int>(dev_r.status().code),
                 dev_r.message().c_str());
    return 1;
  }
  dev::IRenderDevice* device = dev_r.value().get();

  {
    res::IMemoryAllocator* alloc = device->memory_allocator();
    for (res::MemoryKind kind :
         {res::MemoryKind::Vertex, res::MemoryKind::Index, res::MemoryKind::Staging}) {
      if (auto s = alloc->reserve_pages(kind, 4); !s.ok()) {
        std::fprintf(stderr, "预取内存页失败: %s\n", s.message.c_str());
        return 1;
      }
    }
  }

  const int w = args.width;
  const int h = args.height;

  dev::TargetDesc td;
  td.kind = dev::TargetDesc::Kind::Offscreen;
  td.extent = {static_cast<dev::u32>(w), static_cast<dev::u32>(h)};
  td.with_depth = true;  // 挤出体是实体，必须走深度测试否则穿帮
  if (!apply_msaa(*device, args.msaa, &td)) return 2;
  auto tgt_r = device->create_offscreen_target(td);
  if (!tgt_r.ok()) {
    std::fprintf(stderr, "创建离屏目标失败: %s\n", tgt_r.message().c_str());
    return 1;
  }
  dev::IRenderTarget* target = tgt_r.value().get();

  // ---- 4) 相机：斜俯视，能同时看到顶盖与侧壁 ----
  auto camera_owned = mbp::create_camera();
  if (!camera_owned) {
    std::fprintf(stderr, "创建相机失败\n");
    return 1;
  }
  mbp::ICamera* camera = camera_owned.get();
  {
    mbp::CameraDesc cd;
    cd.kind = mbp::ProjectionKind::Perspective;
    cd.fov_y_degrees = 45.0f;
    cd.near_plane = 0.05f;
    cd.far_plane = 100.0f;
    cd.viewport.width = static_cast<mbp::u32>(w);
    cd.viewport.height = static_cast<mbp::u32>(h);
    if (auto s = camera->set_desc(cd); !s.ok()) {
      std::fprintf(stderr, "设置相机参数失败: %s\n", s.message.c_str());
      return 1;
    }
    mbp::CameraPose pose;
    pose.position = {1.5f, 1.3f, 2.5f};
    pose.target = {0.0f, 0.0f, 0.0f};
    pose.up = {0.0f, 1.0f, 0.0f};
    if (auto s = camera->set_pose(pose); !s.ok()) {
      std::fprintf(stderr, "设置相机位姿失败: %s\n", s.message.c_str());
      return 1;
    }
  }

  // ---- 5) 上传每个实体的几何 ----
  // 【必须】用 unique_ptr 持有，不能用裸指针。
  // create_buffer 返回 Result<unique_ptr<IBuffer>>，若只存 .get() 的裸指针，
  // 那个 unique_ptr 在循环迭代结束时就析构了 —— 缓冲被销毁，
  // 存下来的指针变成悬空指针，提交时才报「缓冲不是本设备的 VulkanBuffer」。
  struct GpuMesh {
    std::unique_ptr<res::IBuffer> vb;
    std::unique_ptr<res::IBuffer> ib;
    std::uint32_t index_count = 0;
    mbp::Color4 color;
  };
  std::vector<GpuMesh> gpu;
  gpu.reserve(set.meshes.size());

  for (const featsketch::StrokeMesh& sm : set.meshes) {
    const mbp::MeshData& md = sm.mesh;

    res::BufferDesc vbd;
    vbd.size = md.vertices.size() * sizeof(mbp::Vertex);
    vbd.usage = res::BufferUsage::Vertex;
    auto vb_r = device->create_buffer(vbd);
    if (!vb_r.ok()) {
      std::fprintf(stderr, "创建顶点缓冲失败: %s\n", vb_r.message().c_str());
      return 1;
    }
    std::unique_ptr<res::IBuffer> vb = std::move(vb_r.value());
    if (!vb->upload(md.vertices.data(), vbd.size).ok()) {
      std::fprintf(stderr, "写顶点缓冲失败\n");
      return 1;
    }

    res::BufferDesc ibd;
    ibd.size = md.indices.size() * sizeof(std::uint32_t);
    ibd.usage = res::BufferUsage::Index;
    auto ib_r = device->create_buffer(ibd);
    if (!ib_r.ok()) {
      std::fprintf(stderr, "创建索引缓冲失败: %s\n", ib_r.message().c_str());
      return 1;
    }
    std::unique_ptr<res::IBuffer> ib = std::move(ib_r.value());
    if (!ib->upload(md.indices.data(), ibd.size).ok()) {
      std::fprintf(stderr, "写索引缓冲失败\n");
      return 1;
    }

    // 【必须】upload() 只入队 Staging，不跑 vkCmdCopyBuffer。
    vb->flush_pending();
    ib->flush_pending();
    if (vb->pending_upload_bytes() != 0 || ib->pending_upload_bytes() != 0) {
      std::fprintf(stderr, "上传批未提交:漏了 flush_pending()\n");
      return 1;
    }

    GpuMesh g;
    g.vb = std::move(vb);
    g.ib = std::move(ib);
    g.index_count = static_cast<std::uint32_t>(md.indices.size());
    g.color = sm.color;
    gpu.push_back(std::move(g));  // GpuMesh 持有 unique_ptr，不可拷贝
  }

  // 每个实体一条实例属性（世界矩阵 = 单位阵）
  std::vector<mbp::InstanceAttribs> instances(gpu.size());
  for (std::size_t i = 0; i < gpu.size(); ++i) {
    instances[i] = mbp::InstanceAttribs{};
    instances[i].model[0] = 1.0f;
    instances[i].model[5] = 1.0f;
    instances[i].model[10] = 1.0f;
    instances[i].model[15] = 1.0f;
    instances[i].base_color[0] = gpu[i].color.r;
    instances[i].base_color[1] = gpu[i].color.g;
    instances[i].base_color[2] = gpu[i].color.b;
    instances[i].base_color[3] = gpu[i].color.a;
    instances[i].roughness = 0.55f;
    instances[i].metallic = 0.0f;
  }

  res::BufferDesc instd;
  instd.size = instances.size() * sizeof(mbp::InstanceAttribs);
  instd.usage = res::BufferUsage::Vertex;
  auto inst_r = device->create_buffer(instd);
  if (!inst_r.ok()) {
    std::fprintf(stderr, "创建实例缓冲失败: %s\n", inst_r.message().c_str());
    return 1;
  }
  res::IBuffer* inst_buf = inst_r.value().get();
  if (!inst_buf->upload(instances.data(), instd.size).ok()) {
    std::fprintf(stderr, "上传实例失败\n");
    return 1;
  }
  inst_buf->flush_pending();

  // ---- 6) 管线 ----
  // 直接复用 L3 已有的实例化 3D 管线（顶点/实例布局、深度、剔除、混合全一致）
  auto pl_r = device->create_pipeline(scene_pipeline_desc(args.msaa));
  if (!pl_r.ok()) {
    std::fprintf(stderr, "创建涂鸦管线失败: %s\n", pl_r.message().c_str());
    return 1;
  }
  res::IPipeline* pipeline = pl_r.value().get();

  // ---- 7) 录制 ----
  auto cmd_r = device->create_command_list();
  if (!cmd_r.ok()) {
    std::fprintf(stderr, "创建命令列表失败: %s\n", cmd_r.message().c_str());
    return 1;
  }
  res::ICommandList* cmd = cmd_r.value().get();

  res::RenderPassDesc rp;
  rp.target = target;
  rp.clear_color = true;
  rp.clear_color_rgba[0] = 0.06f;
  rp.clear_color_rgba[1] = 0.07f;
  rp.clear_color_rgba[2] = 0.10f;
  rp.clear_color_rgba[3] = 1.0f;
  rp.clear_depth = true;
  rp.clear_depth_value = 1.0f;
  if (auto s = cmd->begin_pass(rp); !s.ok()) {
    std::fprintf(stderr, "begin_pass 失败: %s\n", s.message.c_str());
    return 1;
  }

  res::Viewport vpv;
  vpv.width = static_cast<float>(w);
  vpv.height = static_cast<float>(h);
  vpv.min_depth = 0.0f;
  vpv.max_depth = 1.0f;
  if (auto s = cmd->set_viewport(vpv); !s.ok()) {
    std::fprintf(stderr, "set_viewport 失败: %s\n", s.message.c_str());
    return 1;
  }
  res::ScissorRect sr;
  sr.width = static_cast<res::u32>(w);
  sr.height = static_cast<res::u32>(h);
  if (auto s = cmd->set_scissor(sr); !s.ok()) {
    std::fprintf(stderr, "set_scissor 失败: %s\n", s.message.c_str());
    return 1;
  }

  if (auto s = cmd->bind_pipeline(pipeline, 0x534B'4554'4348ull); !s.ok()) {
    std::fprintf(stderr, "bind_pipeline 失败: %s\n", s.message.c_str());
    return 1;
  }

  struct SketchPC {
    float view_proj[16];
    float normal_matrix[16];
  } pc{};
  std::memcpy(pc.view_proj, camera->view_proj_matrix().m, sizeof(float) * 16);
  // 本 demo 无非均匀缩放，法线矩阵取单位阵即可（见 L3 spec §6.2.2）
  pc.normal_matrix[0] = 1.0f;
  pc.normal_matrix[5] = 1.0f;
  pc.normal_matrix[10] = 1.0f;
  pc.normal_matrix[15] = 1.0f;
  constexpr dev::u32 kStageVertex = 0x1u;
  constexpr dev::u32 kStageFragment = 0x10u;
  if (auto s = cmd->push_constants(kStageVertex | kStageFragment, 0, sizeof(pc), &pc); !s.ok()) {
    std::fprintf(stderr, "push_constants 失败: %s\n", s.message.c_str());
    return 1;
  }

  if (auto s = cmd->bind_vertex_buffer(inst_buf->id(), 1, 0); !s.ok()) {
    std::fprintf(stderr, "bind 实例缓冲失败: %s\n", s.message.c_str());
    return 1;
  }

  // 逐实体一次 indexed draw：各实体顶点/索引缓冲不同，无法并成一次 draw
  // （那要求共用缓冲 + first_index 偏移）。首轮实体数在笔画数量级（个位数），
  // 这个取舍是合理的；实体数上千时应改为共享缓冲 + 偏移。
  for (std::size_t i = 0; i < gpu.size(); ++i) {
    if (auto s = cmd->bind_vertex_buffer(gpu[i].vb->id(), 0, 0); !s.ok()) {
      std::fprintf(stderr, "bind 顶点缓冲失败: %s\n", s.message.c_str());
      return 1;
    }
    if (auto s = cmd->bind_index_buffer(gpu[i].ib->id(), res::IndexFormat::UInt32, 0); !s.ok()) {
      std::fprintf(stderr, "bind 索引缓冲失败: %s\n", s.message.c_str());
      return 1;
    }
    res::DrawArgs da;
    da.index_count = gpu[i].index_count;
    da.instance_count = 1;
    da.first_instance = static_cast<res::u32>(i);
    if (auto s = cmd->draw_indexed(da); !s.ok()) {
      std::fprintf(stderr, "draw_indexed 失败: %s\n", s.message.c_str());
      return 1;
    }
  }

  if (auto s = cmd->end_pass(); !s.ok()) {
    std::fprintf(stderr, "end_pass 失败: %s\n", s.message.c_str());
    return 1;
  }

  // ---- 8) 提交（每帧恰好一次，G5）----
  if (auto s = device->submit_command_list(*cmd); !s.ok()) {
    std::fprintf(stderr, "提交失败: %s\n", s.message.c_str());
    return 1;
  }

  // ---- 9) 读回 + 存 PNG ----
  std::vector<std::uint8_t> rgba(static_cast<std::size_t>(w) * h * 4u, 0);
  if (auto s = target->readback_rgba8(rgba.data(), rgba.size()); !s.ok()) {
    std::fprintf(stderr, "读回失败: %s\n", s.message.c_str());
    return 1;
  }

  magicblocks::ImageRgba8 img;
  img.width = static_cast<std::uint32_t>(w);
  img.height = static_cast<std::uint32_t>(h);
  img.pixels = std::move(rgba);
  std::string err;
  if (!magicblocks::write_png_rgba8(args.output, img, &err)) {
    std::fprintf(stderr, "写 PNG 失败: %s\n", err.c_str());
    return 1;
  }

  std::printf("[sketch] 已写出 %s (%dx%d) | merge=%d pattern=%d\n", args.output.c_str(), w, h,
              args.merge ? 1 : 0, args.sketch_pattern);
  return 0;
}

// ================================================================ F-VX：魔法叠叠乐

// 一笔"输入笔迹"的世界坐标（生成式，不读随机数/时钟）。
struct StackStrokeInput {
  std::vector<mbp::Vec2> points;  // 世界 xz 平面上的连续坐标
  int plane_layer = 0;            // 该笔投在哪个层平面上
};

// 为第 i 笔生成确定的笔迹：绕原点的圆弧段。所有量由 i / strokes 完全决定
// ⇒ 同参数出图逐字节相同（spec §13.3）。
std::vector<StackStrokeInput> make_stack_strokes(int strokes, int layers, float cell_size,
                                                 int half_extent, int column_height) {
  std::vector<StackStrokeInput> out;
  if (strokes <= 0) return out;
  const float span = static_cast<float>(half_extent) * cell_size;
  // 列高的顶部不能越过网格顶面：第 L 层起摞 column_height 块要求
  // L + column_height <= layers。第一版没约束，--column-height 3 时第一笔
  // 就撞在"柱高越顶"上整笔失败 —— 参数本身没写错，是生成器把笔放错了层。
  const int max_base = (layers > 0) ? (layers - column_height) : 0;
  const int layer_span = (max_base > 0) ? (max_base + 1) : 1;
  for (int i = 0; i < strokes; ++i) {
    StackStrokeInput s;
    s.plane_layer = (i % layer_span);
    // 半径随笔数递增：后画的圈更大，视觉上"越叠越外扩"。
    const float r = span * (0.42f + 0.07f * static_cast<float>(i));
    // 起始角黄金分割步进：每笔错开且不重复。
    const float a0 = 6.2831853f * 0.6180339887f * static_cast<float>(i);
    constexpr int kSeg = 28;
    s.points.reserve(kSeg + 1);
    for (int k = 0; k <= kSeg; ++k) {
      const float a = a0 + 6.2831853f * static_cast<float>(k) / static_cast<float>(kSeg);
      s.points.push_back(mbp::Vec2{r * std::cos(a), r * std::sin(a)});
    }
    out.push_back(std::move(s));
  }
  return out;
}

mbst::ColorMode parse_color_mode(const std::string& text, bool* ok) {
  *ok = true;
  if (text.empty() || text == "flat") return mbst::ColorMode::Flat;
  if (text == "by-layer") return mbst::ColorMode::ByLayer;
  if (text == "by-stroke") return mbst::ColorMode::ByStroke;
  *ok = false;
  return mbst::ColorMode::Flat;
}

int cmd_stack(const Args& args) {
  // ---- 0) 参数校验：全部在此处响，绝不带着非法参数往下走 ----
  if (args.output.empty() && !args.dry_run) {
    std::fprintf(stderr, "[stack] 参数错误: --output 必填\n");
    return 2;
  }
  if (args.width <= 0 || args.height <= 0) {
    std::fprintf(stderr, "[stack] 参数错误: --width/--height 必须为正\n");
    return 2;
  }
  if (args.layers <= 0) {
    std::fprintf(stderr, "[stack] 参数错误: --layers 必须为正\n");
    return 2;
  }
  if (!(args.cell_size > 0.0f)) {
    std::fprintf(stderr, "[stack] 参数错误: --cell-size 必须 > 0\n");
    return 2;
  }
  if (args.strokes <= 0) {
    // 0 笔 = 没有任何方块，必须拒绝。若照写一张纯背景色 PNG，
    // 那张空图就会被 golden 锁死成"正确的图"。
    std::fprintf(stderr, "[stack] 参数错误: --strokes 必须 >= 1\n");
    return 2;
  }
  if (args.column_height <= 0) {
    std::fprintf(stderr, "[stack] 参数错误: --column-height 必须 >= 1\n");
    return 2;
  }
  if (!args.shape_template.empty() && args.column_height != 1) {
    // spec §12：两种堆叠规则不能混，混起来结果没有唯一解释。
    std::fprintf(stderr,
                 "[stack] 参数错误: --template 与 --column-height 互斥"
                 "（同时给出时堆叠规则无唯一解释）\n");
    return 2;
  }
  if (args.template_radius < 1) {
    std::fprintf(stderr, "[stack] 参数错误: --template-radius 必须 >= 1\n");
    return 2;
  }

  bool cm_ok = false;
  const mbst::ColorMode color_mode = parse_color_mode(args.color_mode, &cm_ok);
  if (!cm_ok) {
    std::fprintf(stderr, "[stack] 参数错误: 未知 --color-mode %s\n", args.color_mode.c_str());
    return 2;
  }

  mbst::GridSpec grid;
  grid.origin[0] = 0.0f;
  grid.origin[1] = 0.0f;
  grid.origin[2] = 0.0f;
  grid.cell_size = args.cell_size;
  grid.layers = args.layers;
  grid.half_extent = 8;  // 原点居中 ⇒ 合法域 [-8, 8)

  // 模板先解析（未知名字在建设备之前就响）。
  std::unique_ptr<mbst::IShapeTemplate> tmpl_api;
  mbst::ShapeTemplate tmpl{};
  std::vector<mbst::Cell> tmpl_offsets;
  if (!args.shape_template.empty()) {
    tmpl_api = mbst::create_shape_template();
    auto gen = tmpl_api->generate(args.shape_template, args.template_radius);
    if (!gen.ok()) {
      std::fprintf(stderr, "[stack] 参数错误: %s\n", gen.text().c_str());
      return 2;
    }
    tmpl_offsets = std::move(gen).take();
    tmpl.name = args.shape_template.c_str();
    tmpl.offsets = tmpl_offsets.data();
    tmpl.count = tmpl_offsets.size();
  }

  // ---- 1) 逻辑：构造笔迹（纯 CPU，不碰设备）----
  const std::vector<StackStrokeInput> inputs =
      make_stack_strokes(args.strokes, args.layers, args.cell_size, grid.half_extent,
                         args.column_height);

  // ---- 2) 逐笔吸附 → DDA → 去重 → IStackSession（设计 §3.1 的状态机，不再用过程式流程）----
  //
  // 为什么这里必须走会话而不是直接调 IBlockSet：
  //   · 「一笔的完整生命周期」（落笔 → 采样 → 抬笔 → 可被撤销）原先只存在于
  //     这段函数的代码顺序里，没有任何对象持有，因此"撤销"在渲染路径上无法被验证；
  //   · 会话把非法顺序（抬笔后还 Move、失败未补救就开下一笔）变成**显式错误**，
  //     而不是悄悄做出某个结果。
  std::unique_ptr<mbst::IStackSession> session = mbst::create_stack_session(grid);
  if (!session) {
    std::fprintf(stderr, "[code=%d] GridSpec 非法: cell_size=%g layers=%d half_extent=%d\n",
                 static_cast<int>(mbst::StackCode::OutOfGrid), static_cast<double>(grid.cell_size),
                 grid.layers, grid.half_extent);
    return 1;
  }
  mbst::IBlockSet* set = &session->mutable_set();

  mbst::BrushConfig bcfg;
  bcfg.grid = grid;
  bcfg.column_height = args.column_height;
  std::unique_ptr<mbst::IBlockBrush> brush = mbst::create_block_brush(bcfg);
  if (!brush) {
    std::fprintf(stderr, "[stack] 创建笔刷失败\n");
    return 1;
  }

  // 层锁在下面的逐笔循环里设：每笔按自己的 plane_layer 锁，
  // --lock-layer 作为用户覆盖项把全部笔迹压到同一层。
  // 不要在这里预先锁一次 —— 那会让后面每笔的 lock() 覆盖它，等于白写。

  // 每笔一个确定颜色（黄金分割色相，视觉上互相区分）。
  const mbst::ColorPalette stroke_colors = mbst::make_default_palette(inputs.size());

  std::uint32_t total_skipped = 0;
  std::uint32_t total_emitted = 0;
  // 跨笔重叠被滤掉的格数（与笔迹内部的 total_skipped 分开统计：
  // 前者是"后画的圈压在先画的圈上"，是叠叠乐的正常形态；
  // 后者是同一条笔迹内部的 DDA 重复。）
  std::uint32_t total_cross_skipped = 0;
  std::uint32_t total_samples = 0;
  std::vector<mbp::Ray> rays;
  for (std::size_t i = 0; i < inputs.size(); ++i) {
    const StackStrokeInput& in = inputs[i];
    const float plane_y =
        grid.origin[1] + (static_cast<float>(in.plane_layer) + 0.5f) * grid.cell_size;

    // **必须逐笔锁层**，不能只靠射线高度决定落在哪一层。
    //
    // 理由（实测踩到）：未锁层时笔刷走 `pick_layer_for_ray`，它的判据是
    // "射线与网格体积的第一交点所在的层"。我们的射线从网格**上方**垂直向下打，
    // 第一交点恒在顶面 ⇒ 永远落在第 layers-1 层。结果 `--strokes 3` 生成的三笔
    //（本意分别在第 0/1/2 层）全部摞在第 7 层，屏幕上只剩最上面那一圈；
    // 而 `--column-height 3` 立刻撞"柱高越顶"。这不是生成器的错，是调用方
    // 没告诉笔刷"我要第几层"。
    // `--lock-layer` 是用户覆盖项；没给时按每笔自己的 plane_layer 锁。
    //
    // 锁层走**会话**（§6.5 的子状态机）与笔刷**两处**：笔刷侧的 LayerLock 决定
    // 射线吸附到哪一层，会话侧的 locked_layer 决定 GridOverlay 高亮哪一层、
    // 并进会话快照。两处必须同一个值，否则"画在第 2 层、高亮第 0 层"。
    const std::int32_t want_layer = (args.lock_layer >= 0) ? args.lock_layer : in.plane_layer;
    if (auto ls = brush->layer_lock().lock(want_layer); !ls.ok()) {
      std::fprintf(stderr, "[code=%d] %s\n", static_cast<int>(ls.error_code()), ls.text().c_str());
      return 1;
    }
    if (auto ls = session->lock_layer(want_layer); !ls.ok()) {
      std::fprintf(stderr, "[code=%d] %s\n", static_cast<int>(ls.error_code()), ls.text().c_str());
      return 1;
    }

    // 平面笔迹 → 世界射线：从该层平面上方垂直向下，落点即笔迹点。
    rays.clear();
    rays.reserve(in.points.size());
    for (const mbp::Vec2& p : in.points) {
      rays.push_back(mbp::Ray{mbp::Vec3{p.x, plane_y + 8.0f, p.y}, mbp::Vec3{0.0f, -1.0f, 0.0f}});
    }

    auto plan_r = brush->stroke(rays.data(), rays.size());
    if (!plan_r.ok()) {
      std::fprintf(stderr, "[code=%d] 第 %zu 笔: %s\n", static_cast<int>(plan_r.error_code()), i,
                   plan_r.text().c_str());
      return 1;
    }
    const mbst::StrokePlan plan = plan_r.value();
    total_skipped += plan.stats.skipped_occupied;
    total_emitted += plan.stats.cells_emitted;
    if (plan.cells.empty()) continue;

    // 有模板时，把每个已吸附格当作模板锚点展开；否则按列高直接落块。
    //
    // ⚠️ 会话的 `end_stroke` 对"格已被占用"的语义是：该格不生效并计数进
    // skipped_occupied（§6.4 / §8），整笔**不**失败 —— 后画的圈压在先画的圈上
    // 是叠叠乐的常态。这里因此不再需要"手动滤掉已占格"的旁路：把整笔的格
    // （含重叠）交给会话，由会话在落块时去重并统计跨笔重叠。
    //
    // 模板分支的插入顺序是 `(y,z,x)` 升序（既有实现如此），它**与按下标序的
    // 列高分支不同**：真源下标序 → doc_hash，所以这里的顺序是 golden 契约的一部分，
    // 不许趁改写顺手改成"更自然"的顺序（那会让所有模板 golden 全部漂移）。
    std::vector<mbst::Cell> cells;
    if (tmpl.count > 0) {
      for (const mbst::Cell& anchor : plan.cells) {
        auto ex = mbst::expand_template(grid, anchor, tmpl);
        if (!ex.ok()) {
          std::fprintf(stderr, "[code=%d] %s\n", static_cast<int>(ex.error_code()),
                       ex.text().c_str());
          return 1;
        }
        const std::vector<mbst::Cell>& v = ex.value();
        cells.insert(cells.end(), v.begin(), v.end());
      }
      std::sort(cells.begin(), cells.end());
      cells.erase(std::unique(cells.begin(), cells.end()), cells.end());
    } else {
      cells.reserve(plan.cells.size() * static_cast<std::size_t>(args.column_height));
      for (const mbst::Cell& base : plan.cells) {
        for (int k = 0; k < args.column_height; ++k) {
          mbst::Cell c;
          c.x = base.x;
          c.y = base.y + k;
          c.z = base.z;
          cells.push_back(c);
        }
      }
    }

    // 一次落笔 = 会话里的一次完整生命周期：begin → move×N → end。
    // 这里把同一笔的格按"采样批次"喂进去，保留 Move 的语义（便于将来接真实输入）。
    mbst::StrokeRequest req;
    req.rgba = stroke_colors[i];
    req.layer = want_layer;
    if (auto bs = session->begin_stroke(req); !bs.ok()) {
      std::fprintf(stderr, "[code=%d] 第 %zu 笔 begin_stroke: %s\n",
                   static_cast<int>(bs.error_code()), i, bs.text().c_str());
      return 1;
    }
    ++total_samples;
    if (auto ms = session->move_sample(cells); !ms.ok()) {
      std::fprintf(stderr, "[code=%d] 第 %zu 笔 move_sample: %s\n",
                   static_cast<int>(ms.error_code()), i, ms.text().c_str());
      return 1;
    }
    auto committed = session->end_stroke();
    if (!committed.ok()) {
      std::fprintf(stderr, "[code=%d] 第 %zu 笔 end_stroke: %s\n",
                   static_cast<int>(committed.error_code()), i, committed.text().c_str());
      return 1;
    }
    total_cross_skipped += committed.value().skipped_occupied;
  }

  if (set->size() == 0) {
    std::fprintf(stderr, "[stack] 逻辑错误: 没有产生任何方块，拒绝出图\n");
    return 1;
  }

  // ---- 颜色模式 ----
  // 三种模式对色板形状的要求不同（Flat 必须恰好 1 项），所以按模式构造，
  // 而不是拿"每笔一色"那张表硬套 —— 后者在 Flat 下必然报 ColorOutOfRange。
  // 走会话的 apply_color_mode：改色会清空撤销栈（改色之后"撤销整笔"已不成立）。
  mbst::ColorPalette mode_palette;
  switch (color_mode) {
    case mbst::ColorMode::Flat:
      mode_palette = mbst::make_default_palette(1);  // 整堆同色
      break;
    case mbst::ColorMode::ByLayer:
      // 每层一色：层数就是天然基数，取模不会绕回。
      mode_palette = mbst::make_default_palette(static_cast<std::size_t>(grid.layers));
      break;
    case mbst::ColorMode::ByStroke:
      mode_palette = stroke_colors;  // 每笔一色
      break;
  }
  auto color_r = session->apply_color_mode(color_mode, mode_palette);
  if (!color_r.ok()) {
    std::fprintf(stderr, "[code=%d] %s\n", static_cast<int>(color_r.error_code()),
                 color_r.text().c_str());
    return 1;
  }
  const mbst::ColorApplyStats cstats{};
  (void)cstats;

  const std::uint64_t doc_hash = set->hash();

  // ---- dry-run：到此为止，不建设备、不出图（spec §13.3）----
  if (args.dry_run) {
    std::printf("[stack] dry-run 笔画 %zu | 方块 %zu | 发射 %u 跳过 %u | doc_hash=%016llx\n",
                set->stroke_count(), set->size(), total_emitted, total_skipped,
                static_cast<unsigned long long>(doc_hash));
    return 0;
  }  // ---- 3) 设备 ----
  auto dev_r = dev::create_render_device(software_desc());
  if (!dev_r.ok()) {
    std::fprintf(stderr, "[stack] 创建设备失败: %s\n", dev_r.message().c_str());
    return 1;
  }
  dev::IRenderDevice* device = dev_r.value().get();
  {
    res::IMemoryAllocator* alloc = device->memory_allocator();
    for (res::MemoryKind kind :
         {res::MemoryKind::Vertex, res::MemoryKind::Index, res::MemoryKind::Staging}) {
      if (auto s = alloc->reserve_pages(kind, 4); !s.ok()) {
        std::fprintf(stderr, "[stack] 预取内存页失败: %s\n", s.message.c_str());
        return 1;
      }
    }
  }

  const int w = args.width;
  const int h = args.height;

  // ---- 4) 离屏目标（必须带深度：表面 pass 要按深度裁决）----
  dev::TargetDesc td;
  td.kind = dev::TargetDesc::Kind::Offscreen;
  td.extent = {static_cast<dev::u32>(w), static_cast<dev::u32>(h)};
  td.with_depth = true;
  if (!apply_msaa(*device, args.msaa, &td)) return 2;
  auto tgt_r = device->create_offscreen_target(td);
  if (!tgt_r.ok()) {
    std::fprintf(stderr, "[stack] 创建离屏目标失败: %s\n", tgt_r.message().c_str());
    return 1;
  }
  dev::IRenderTarget* target = tgt_r.value().get();

  // 相机：斜俯视，能看到顶盖 + 侧壁 + 层间缝隙。
  auto camera_owned = mbp::create_camera();
  if (!camera_owned) {
    std::fprintf(stderr, "[stack] 创建相机失败\n");
    return 1;
  }
  mbp::ICamera* camera = camera_owned.get();
  {
    mbp::CameraDesc cd;
    cd.kind = mbp::ProjectionKind::Perspective;
    cd.fov_y_degrees = 45.0f;
    cd.near_plane = 0.05f;
    cd.far_plane = 100.0f;
    cd.viewport.width = static_cast<mbp::u32>(w);
    cd.viewport.height = static_cast<mbp::u32>(h);
    if (auto s = camera->set_desc(cd); !s.ok()) {
      std::fprintf(stderr, "[stack] 设置相机参数失败: %s\n", s.message.c_str());
      return 1;
    }
    mbp::CameraPose pose;
    // 网格世界尺度：cell_size=0.5、half_extent=8 ⇒ 可堆叠区域约 [-4,4]，
    // 高度 layers*cell_size = 4。相机距离必须与这个尺度匹配 ——
    // 第一版用了 (4.2,3.6,5.0) 看 (0,0.9,0)，曝光后发现物体被压成画面最下面
    // 6 像素高的一条（几何全在视锥底边附近），也就是"出了图但等于没画"。
    // 这里改成 2.5 倍视野距离，让 ±4 的世界角点都落在画面内。
    pose.position = {9.0f, 7.0f, 9.0f};
    pose.target = {0.0f, 1.2f, 0.0f};
    pose.up = {0.0f, 1.0f, 0.0f};
    if (auto s = camera->set_pose(pose); !s.ok()) {
      std::fprintf(stderr, "[stack] 设置相机位姿失败: %s\n", s.message.c_str());
      return 1;
    }
  }

  mbst::StackFrameDesc frame;
  frame.set = set;
  std::memcpy(frame.view_proj, camera->view_proj_matrix().m, sizeof(float) * 16);
  frame.width = static_cast<std::uint32_t>(w);
  frame.height = static_cast<std::uint32_t>(h);
  frame.clear_color[0] = 0.06f;
  frame.clear_color[1] = 0.07f;
  frame.clear_color[2] = 0.10f;
  frame.clear_color[3] = 1.0f;
  frame.inner_pass_enabled = args.inner_pass;
  // debug_inner_pass_first 恒 false：生产路径不提供关掉正确性的旋钮。

  // ---- 5) 两条管线 + 实例缓冲 + 网格指示 ----
  //
  // 网格指示（需求「立方体…带网格指示」）此前**只在组件层有实现**，CLI 出图里
  // 一行都没用 —— 典型"组件级真绿、集成级假绿"。现在它经 StackFrameOverlay 接进
  // record_frame：渲染器在录完表面/内面两个 pass 后调 collect，把网格线 + 锁定层
  // 高亮 + 光标格收进 overlay。覆盖层自己的那一次 draw 由本函数录制，
  // **不计入**双 pass 的 2 次预算（grid_overlay.h 顶部）。
  auto renderer = mbst::create_stack_renderer();
  if (auto s = renderer->create_passes(device, static_cast<std::uint32_t>(args.msaa)); !s.ok()) {
    std::fprintf(stderr, "[stack] 创建双 pass 失败: %s\n", s.text().c_str());
    return 1;
  }
  if (auto s = renderer->build_instances(grid, *set); !s.ok()) {
    std::fprintf(stderr, "[stack] 构建实例失败: %s\n", s.text().c_str());
    return 1;
  }

  std::unique_ptr<mbst::IGridOverlay> grid_overlay;
  std::unique_ptr<mbp::IOverlay2D> overlay_collector;
  mbst::OverlayBuffers overlay_buffers;  // 必须活到 submit 之后
  if (args.grid_overlay) {
    grid_overlay = mbst::create_grid_overlay();
    overlay_collector = mbp::create_overlay2d();
    if (!grid_overlay || !overlay_collector) {
      std::fprintf(stderr, "[stack] 创建网格指示失败\n");
      return 1;
    }
    overlay_collector->set_viewport(static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h));
    overlay_collector->begin_frame();
    overlay_collector->reserve(static_cast<std::uint32_t>(2 * grid.half_extent * 2 + 8), 8);
    if (auto s = grid_overlay->create_passes(device, static_cast<std::uint32_t>(args.msaa));
        !s.ok()) {
      std::fprintf(stderr, "[stack] 创建网格指示管线失败: %s\n", s.text().c_str());
      return 1;
    }
    frame.overlay.overlay = grid_overlay.get();
    frame.overlay.collector = overlay_collector.get();
    frame.overlay.grid = grid;
    // 画哪一层的网格：锁了层就是锁定层；没锁（CLI 缺省会逐笔锁，所以这里通常
    // 有值）就退到第 0 层 —— 这是 StackOverlayDesc 文档写明的口径。
    frame.overlay.desc.active_layer = session->layer_locked() ? session->locked_layer() : 0;
    frame.overlay.desc.draw_grid_lines = true;
    // ② 锁定层高亮：**CLI 里必须关掉**。
    //
    // 实测（2026-11-11）：这部分把整层投影成一个**屏幕对齐的轴对齐矩形**
    // （grid_overlay.cpp 里对 4 个角取 min/max），再半透明铺满。斜俯视下
    // "整层顶面"的屏幕包围盒几乎是整幅画面，于是它把下面已经画好的积木
    // 整片盖掉（实测 200x140 上 96.7% 背景被压成 65% 黑、积木色几乎全被吃掉）。
    // 那不是一个"网格指示"，那是一块挡板：需求要的是"看得见在哪一格上画"，
    // 不是"看不见画了什么"。①（细网格线）+ ③（光标格）已经足够表达网格指示，
    // 且都是线条/小框，不会遮住积木。②留给真正需要层板的场景（组件层用例仍覆盖它）。
    frame.overlay.desc.draw_locked_layer = false;
    // 网格线必须**半透明**。理由（实测 2026-11-11）：`GridOverlayStyle` 的缺省线色是
    // alpha 0.85（预乘混合下 ≈ 不透明），于是网格线把下面的积木像素整条盖死 ——
    // 实测 `--color-mode flat` 与 `by-layer` 两张图在有网格线时颜色种数 7 vs 7、
    // 关闭后 6 vs 16，也就是"网格指示把改色的效果遮住了"。
    // 网格指示的语义是"看见你在哪一格上画"，不是"把画挡住"，
    // 所以这里把线宽压到 1px、alpha 降到 0.45，让积木颜色透过来。
    {
      mbst::GridOverlayStyle style;
      style.grid_line = mbp::Color4{0.55f, 0.62f, 0.78f, 0.45f};
      style.grid_line_px = 1.0f;
      style.cursor_fill = mbp::Color4{1.00f, 0.85f, 0.30f, 0.22f};
      style.cursor_edge = mbp::Color4{1.00f, 0.82f, 0.25f, 0.75f};
      style.cursor_edge_px = 1.5f;
      grid_overlay->set_style(style);
    }
    // 光标格 = 最后一笔的起点（CLI 没有鼠标；用"笔从哪落下"代替光标，
    // 让"按下的那一刻决定层数"这条需求在画面上有可见的落点）。
    if (!inputs.empty() && !inputs.back().points.empty()) {
      const mbp::Vec2& p0 = inputs.back().points.front();
      const std::int32_t lay = session->layer_locked() ? session->locked_layer()
                                                       : inputs.back().plane_layer;
      auto c = mbst::snap_ray_to_cell(
          mbp::Ray{mbp::Vec3{p0.x, grid.origin[1] + static_cast<float>(lay) * grid.cell_size + 8.0f,
                             p0.y},
                   mbp::Vec3{0.0f, -1.0f, 0.0f}},
          grid, lay);
      if (c.ok()) {
        frame.overlay.desc.has_cursor = true;
        frame.overlay.desc.cursor = c.value();
      }
    }
    frame.overlay.project_with_frame_matrix = true;
  }

  // ---- 6) 录制（渲染器只录不交）----
  auto cmd_r = device->create_command_list();
  if (!cmd_r.ok()) {
    std::fprintf(stderr, "[stack] 创建命令列表失败: %s\n", cmd_r.message().c_str());
    return 1;
  }
  res::ICommandList* cmd = cmd_r.value().get();

  renderer->reset_stats();
  if (auto s = renderer->record_frame(*cmd, target, frame); !s.ok()) {
    std::fprintf(stderr, "[stack] 录制失败: %s\n", s.text().c_str());
    return 1;
  }

  // ---- 6b) 网格指示：build → 用自己的管线录一次 draw（独立 pass）----
  //
  // 独立 pass 的理由：覆盖层管线是**屏幕空间 2D + 预乘混合 + 关深度**，
  // 与积木两条管线都不同（`GridOverlay.OverlayIsItsOwnDrawNotPartOfTwoPassBudget`
  // 断言 hash 互不相等）。它必须开在第二/第三个 begin_pass 里，且**不** clear
  // （清了就把积木整帧擦掉）。失败即报错，不许"画不出来就当没配"。
  if (args.grid_overlay) {
    overlay_collector->build();
    res::RenderPassDesc orp;
    orp.target = target;
    orp.clear_color = false;
    // 深度：这一帧没有任何别的 pass 读深度了（表面/内面都已在上面录完），
    // 所以按"不影响已有像素"的默认值清 1.0 即可。覆盖层管线自己 `depthTest=false`，
    // clear 值对它不产生任何裁决。
    orp.clear_depth = false;
    // ⚠️ **area 必须显式给满整幅**。
    //
    // 实测（2026-11-11）：不给 area 时后端把 renderArea 当成 (0,0,0,0)，
    // vkCmdBeginRenderPass 的 renderArea 为空 —— 于是覆盖层的两个三角形全被裁掉，
    // 整幅画面被涂成清屏色（Ubyte4 零 = 全黑），连下面已经画好的积木一起吃掉。
    // 这不是"网格指示没显示"，是"网格指示把整帧擦黑了"，必须给全额 area。
    orp.area_width = static_cast<res::u32>(w);
    orp.area_height = static_cast<res::u32>(h);
    if (auto s = cmd->begin_pass(orp); !s.ok()) {
      std::fprintf(stderr, "[stack] 网格指示 begin_pass 失败: %s\n", s.message.c_str());
      return 1;
    }
    if (auto s = grid_overlay->record(*cmd, overlay_collector->geometry(),
                                      static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h),
                                      &overlay_buffers);
        !s.ok()) {
      std::fprintf(stderr, "[stack] 网格指示录制失败: %s\n", s.text().c_str());
      return 1;
    }
    if (auto s = cmd->end_pass(); !s.ok()) {
      std::fprintf(stderr, "[stack] 网格指示 end_pass 失败: %s\n", s.message.c_str());
      return 1;
    }
    if (!renderer->stats().overlay_collected) {
      std::fprintf(stderr, "[stack] 网格指示没有被收进覆盖层（接线断了）\n");
      return 1;
    }
  }

  // ---- 7) 一帧一次提交（G5：提交权在调用方）----
  if (auto s = device->submit_command_list(*cmd); !s.ok()) {
    std::fprintf(stderr, "[stack] 提交失败: %s\n", s.message.c_str());
    return 1;
  }

  // ---- 8) 读回 + 写 PNG ----
  std::vector<std::uint8_t> rgba(static_cast<std::size_t>(w) * h * 4u, 0);
  if (auto s = target->readback_rgba8(rgba.data(), rgba.size()); !s.ok()) {
    std::fprintf(stderr, "[stack] 读回失败: %s\n", s.message.c_str());
    return 1;
  }

  magicblocks::ImageRgba8 img;
  img.width = static_cast<std::uint32_t>(w);
  img.height = static_cast<std::uint32_t>(h);
  img.pixels = std::move(rgba);
  std::string err;
  if (!magicblocks::write_png_rgba8(args.output, img, &err)) {
    std::fprintf(stderr, "[stack] 写 PNG 失败: %s\n", err.c_str());
    return 1;
  }

  const mbst::StackRenderStats& st = renderer->stats();
  std::printf(
      "[stack] 笔画 %zu | 方块 %zu -> 请求 %zu -> 合批 %u (pass %u) | drawcall %u 管线切换 %u | "
      "doc_hash=%016llx inst=%u | 网格指示 %s(%u 线 %u 框) | 已写出 %s (%dx%d)\n",
      set->stroke_count(), set->size(), set->size(), st.draw_calls, st.begin_passes, st.draw_calls,
      st.pipeline_binds, static_cast<unsigned long long>(doc_hash), st.instances,
      st.overlay_collected ? "on" : "off", st.overlay_lines, st.overlay_rects, args.output.c_str(),
      w, h);
  // 会话自检：真源必须满足 I1/I3，且所有笔都停在 idle（不许把一笔留在半截）。
  if (auto v = set->validate(); !v.ok()) {
    std::fprintf(stderr, "[stack] 会话真源自检失败: %s\n", v.text().c_str());
    return 1;
  }
  if (session->state() != mbst::SessionState::Idle) {
    std::fprintf(stderr, "[stack] 会话状态 %s：落笔没有闭合\n",
                 mbst::session_state_name(session->state()));
    return 1;
  }
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  Args args;
  const int pr = parse_args(argc, argv, &args);
  if (pr == -1) {
    print_usage();
    return 0;
  }
  if (pr != 0) {
    print_usage();
    return 2;
  }

  if (args.cmd == "paint") return cmd_paint(args);
  if (args.cmd == "blocks") return cmd_blocks(args);
  if (args.cmd == "gizmo") return cmd_gizmo(args);
  if (args.cmd == "sketch") return cmd_sketch(args);
  if (args.cmd == "stack") return cmd_stack(args);

  std::fprintf(stderr, "未知子命令: %s\n", args.cmd.c_str());
  print_usage();
  return 2;
}
