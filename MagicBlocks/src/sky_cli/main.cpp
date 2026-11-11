// MagicBlocks L3 —— 方向背景(sky)离屏出图 CLI
//
// 只做一件事:**按给定相机渲染一帧**(可开关天空盒),落盘 RGBA8 PNG。
// 它存在的理由是给审阅/CI 提供**可核对的像素证据**:
//   - 开/关对比(`--sky on|off`)交给 tools/png_inspect.py --diff 出差异与包围盒;
//   - 背景区主色/色种数交给 png_inspect.py --region;
//   - 旋转/平移的正确性交给两组 `--yaw/--pitch` 与 `--pan` 的组合对比。
//
// 所有输入都由命令行**完全推导**:不读时钟、不读鼠标、不加抖动/随机,
// 同样的参数两次运行产出**逐字节相同**的 PNG(与 l3_cli 同口径)。
//
// 退出码:0 成功 · 1 运行失败 · 2 参数错误 / 资源缺失(不降级成纯色清屏)。
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
#include "magicblocks/primitives/camera.h"
#include "magicblocks/primitives/sky.h"
#include "magicblocks/primitives/types.h"
#include "magicblocks/resources/buffer.h"
#include "magicblocks/resources/command.h"
#include "magicblocks/resources/memory.h"
#include "magicblocks/resources/pipeline.h"
#include "magicblocks_shaders.h"

namespace dev = magicblocks::device;
namespace res = magicblocks::resources;
namespace mbp = magicblocks::primitives;

namespace {

void print_usage() {
  std::fprintf(stderr,
               "用法:\n"
               "  magicblocks-sky-cli --output out.png [选项]\n"
               "\n"
               "  --width W --height H        输出尺寸(默认 640x400)\n"
               "  --asset-root DIR            资源根(含 cubeMap/ 的目录)。缺省取 %%MB_ASSET_ROOT%%\n"
               "  --scene NAME                cubeMap 下的场景名(默认 qwantani_dusk_2_puresky_1k)\n"
               "  --sky on|off                是否画天空盒(默认 on)。off = 纯清屏 + 实体\n"
               "  --clear r,g,b               清屏色(0..255,默认 0,0,0)\n"
               "  --yaw DEG --pitch DEG       相机朝向(默认 yaw=0 pitch=0)\n"
               "  --pan X,Y,Z                 相机位置(默认 0,0,0)。只改位置 = 平移用例\n"
               "  --fov DEG                   垂直视角(默认 60)\n"
               "  --blocks N                  在视野里放 N 块不透明实体(默认 0;证明天空不遮挡实体)\n"
               "  --msaa N                    采样数(默认 1)\n"
               "  --stats                     打印本轮统计(JSON)到 stdout\n"
               "\n"
               "退出码:0 成功 / 1 运行失败 / 2 参数错误或资源缺失(不降级成纯色清屏)。\n");
}

struct Args {
  dev::u32 width = 640;
  dev::u32 height = 400;
  std::string output;
  std::string asset_root;
  std::string scene = "qwantani_dusk_2_puresky_1k";
  bool sky = true;
  float clear[3] = {0.0f, 0.0f, 0.0f};
  float yaw_deg = 0.0f;
  float pitch_deg = 0.0f;
  float pan[3] = {0.0f, 0.0f, 0.0f};
  float fov_deg = 60.0f;
  int blocks = 0;
  int msaa = 1;
  bool print_stats = false;
};

bool parse_floats(const char* s, float* out, int n) {
  int i = 0;
  const char* p = s;
  while (i < n) {
    char* end = nullptr;
    const double v = std::strtod(p, &end);
    if (end == p) return false;
    out[i++] = static_cast<float>(v);
    if (*end == ',') {
      p = end + 1;
      continue;
    }
    if (*end == '\0') break;
    return false;
  }
  return i == n;
}

int parse_args(int argc, char** argv, Args* out) {
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
      if (!v) return -1;
      out->width = static_cast<dev::u32>(std::strtoul(v, nullptr, 10));
    } else if (a == "--height") {
      const char* v = next("--height");
      if (!v) return -1;
      out->height = static_cast<dev::u32>(std::strtoul(v, nullptr, 10));
    } else if (a == "--output") {
      const char* v = next("--output");
      if (!v) return -1;
      out->output = v;
    } else if (a == "--asset-root") {
      const char* v = next("--asset-root");
      if (!v) return -1;
      out->asset_root = v;
    } else if (a == "--scene") {
      const char* v = next("--scene");
      if (!v) return -1;
      out->scene = v;
    } else if (a == "--sky") {
      const char* v = next("--sky");
      if (!v) return -1;
      if (std::strcmp(v, "on") == 0) {
        out->sky = true;
      } else if (std::strcmp(v, "off") == 0) {
        out->sky = false;
      } else {
        std::fprintf(stderr, "--sky 只接受 on|off,得到 %s\n", v);
        return -1;
      }
    } else if (a == "--clear") {
      const char* v = next("--clear");
      if (!v || !parse_floats(v, out->clear, 3)) {
        std::fprintf(stderr, "--clear 需要 r,g,b\n");
        return -1;
      }
    } else if (a == "--yaw") {
      const char* v = next("--yaw");
      if (!v) return -1;
      out->yaw_deg = static_cast<float>(std::strtod(v, nullptr));
    } else if (a == "--pitch") {
      const char* v = next("--pitch");
      if (!v) return -1;
      out->pitch_deg = static_cast<float>(std::strtod(v, nullptr));
    } else if (a == "--pan") {
      const char* v = next("--pan");
      if (!v || !parse_floats(v, out->pan, 3)) {
        std::fprintf(stderr, "--pan 需要 x,y,z\n");
        return -1;
      }
    } else if (a == "--fov") {
      const char* v = next("--fov");
      if (!v) return -1;
      out->fov_deg = static_cast<float>(std::strtod(v, nullptr));
    } else if (a == "--blocks") {
      const char* v = next("--blocks");
      if (!v) return -1;
      out->blocks = std::atoi(v);
    } else if (a == "--msaa") {
      const char* v = next("--msaa");
      if (!v) return -1;
      out->msaa = std::atoi(v);
    } else if (a == "--stats") {
      out->print_stats = true;
    } else {
      std::fprintf(stderr, "未知参数: %s\n", a.c_str());
      return -1;
    }
  }
  if (out->output.empty()) {
    std::fprintf(stderr, "--output 必填\n");
    return -1;
  }
  if (out->width == 0 || out->height == 0) {
    std::fprintf(stderr, "--width/--height 必须 > 0\n");
    return -1;
  }
  return 0;
}

// 由 yaw/pitch/pan 造相机位姿:yaw 绕 +y、pitch 绕右轴,+y 向上。
mbp::CameraSnapshot make_camera(const Args& a) {
  mbp::CameraSnapshot s;
  s.desc.kind = mbp::ProjectionKind::Perspective;
  s.desc.fov_y_degrees = a.fov_deg;
  s.desc.near_plane = 0.1f;
  s.desc.far_plane = 1000.0f;
  s.desc.viewport = {a.width, a.height};
  s.pose.position = {a.pan[0], a.pan[1], a.pan[2]};
  constexpr double kPi = 3.14159265358979323846;
  const double yaw = static_cast<double>(a.yaw_deg) * kPi / 180.0;
  const double pitch = static_cast<double>(a.pitch_deg) * kPi / 180.0;
  const mbp::Vec3 dir{static_cast<float>(std::sin(yaw) * std::cos(pitch)),
                      static_cast<float>(std::sin(pitch)),
                      static_cast<float>(-std::cos(yaw) * std::cos(pitch))};
  s.pose.target = s.pose.position + dir;
  s.pose.up = {0.0f, 1.0f, 0.0f};
  return s;
}

// ---------------------------------------------------------------- 不透明实体

struct SolidVertex {
  float pos[3];
  float color[4];
};
static_assert(sizeof(SolidVertex) == 28, "位置 12B + 颜色 16B");

// 以相机为中心,沿视线 d 处画 count 个方块(屏幕风车排布)。
// 它们的作用只有一个:证明天空**不写深度**,实体像素能盖住天空。
std::vector<SolidVertex> build_blocks(const mbp::CameraSnapshot& cam, int count) {
  std::vector<SolidVertex> out;
  if (count <= 0) return out;
  const mbp::Vec3 f = mbp::normalize(cam.pose.target - cam.pose.position);
  const mbp::Vec3 r = mbp::normalize(mbp::cross(f, cam.pose.up));
  const mbp::Vec3 u = mbp::cross(r, f);
  const float dist = 4.0f;
  const float col[4] = {0.95f, 0.55f, 0.20f, 1.0f};
  for (int i = 0; i < count; ++i) {
    const float t = (count == 1) ? 0.0f
                                 : (static_cast<float>(i) / static_cast<float>(count - 1) * 2.0f -
                                    1.0f);
    const mbp::Vec3 c = cam.pose.position + f * dist + r * (t * 1.8f) - u * 0.15f;
    const float half = 0.35f;
    // 两个三角形(屏幕平面上)。
    const float quad[6][2] = {{-1, 1}, {1, 1}, {1, -1}, {-1, 1}, {1, -1}, {-1, -1}};
    for (const auto& q : quad) {
      SolidVertex v{};
      const mbp::Vec3 p = c + r * (q[0] * half) + u * (q[1] * half);
      v.pos[0] = p.x;
      v.pos[1] = p.y;
      v.pos[2] = p.z;
      std::memcpy(v.color, col, sizeof(col));
      out.push_back(v);
    }
  }
  return out;
}

res::PipelineDesc solid_pipeline_desc(int samples) {
  res::PipelineDesc d;
  res::VertexInputBinding vb;
  vb.binding = 0;
  vb.stride = sizeof(SolidVertex);
  vb.instanced = false;
  d.vertexLayout.bindings.push_back(vb);
  res::VertexAttribute a0;
  a0.location = 0;
  a0.binding = 0;
  a0.format = res::VertexFormat::Float3;
  a0.offset = 0;
  d.vertexLayout.attributes.push_back(a0);
  res::VertexAttribute a1;
  a1.location = 1;
  a1.binding = 0;
  a1.format = res::VertexFormat::Float4;
  a1.offset = sizeof(float) * 3;
  d.vertexLayout.attributes.push_back(a1);
  res::ShaderModule vs;
  vs.stage = res::ShaderStage::Vertex;
  vs.spirv = reinterpret_cast<const std::uint8_t*>(magicblocks::shaders::kSolidVert);
  vs.size_bytes = magicblocks::shaders::kSolidVertSize;
  d.stages.push_back(vs);
  res::ShaderModule fs;
  fs.stage = res::ShaderStage::Fragment;
  fs.spirv = reinterpret_cast<const std::uint8_t*>(magicblocks::shaders::kSolidFrag);
  fs.size_bytes = magicblocks::shaders::kSolidFragSize;
  d.stages.push_back(fs);
  d.blend.enabled = false;
  // 不写深度:目标是"盖住天空就行",不需要深度附件(天空也没深度)。
  d.depth.depthTest = false;
  d.depth.depthWrite = false;
  d.raster.cull = res::CullMode::None;
  d.samples = static_cast<res::u32>(samples > 1 ? samples : 1);
  d.push_constant_bytes = 64;  // view_proj
  return d;
}

}  // namespace

int main(int argc, char** argv) {
  Args args;
  if (parse_args(argc, argv, &args) != 0) {
    print_usage();
    return 2;
  }

  std::string asset_root = args.asset_root;
  if (asset_root.empty()) {
    const char* env = std::getenv("MB_ASSET_ROOT");
    if (env != nullptr) asset_root = env;
  }
  // 需要六面 PNG 的两种情况:(a) 要画天空盒;(b) 零可见 draw 时要用一条被裁掉的
  // 天空盒 draw 满足"必须有 draw"的提交契约。只有 --sky off --blocks N(N>0) 不需要。
  const bool need_cube = args.sky || args.blocks <= 0;
  if (asset_root.empty() && need_cube) {
    std::fprintf(stderr,
                 "--asset-root 缺失(或环境变量 MB_ASSET_ROOT 未设置):天空盒需要六面 PNG。\n"
                 "  本工具**不会**在资源缺失时退化成纯色清屏。\n");
    return 2;
  }

  dev::DeviceDesc dd;
  dd.app_name = "MagicBlocks sky CLI";
  dd.require_software_rasterizer = true;
  auto dev_r = dev::create_render_device(dd);
  if (!dev_r.ok()) {
    std::fprintf(stderr, "创建设备失败: %s\n", dev_r.message().c_str());
    return 1;
  }
  dev::IRenderDevice& device = *dev_r.value();

  // 纹理页:1024² 立方体 = 24 MiB;分配器一页 8 MiB → 至少 3 页,预留 4 页。
  res::IMemoryAllocator* alloc = device.memory_allocator();
  if (alloc == nullptr) {
    std::fprintf(stderr, "内部错误:没有内存分配器\n");
    return 1;
  }
  if (auto s = alloc->reserve_pages(res::MemoryKind::Texture, 4); !s.ok()) {
    std::fprintf(stderr, "预留纹理页失败: %s\n", s.message.c_str());
    return 1;
  }
  for (res::MemoryKind k : {res::MemoryKind::Vertex, res::MemoryKind::Index,
                            res::MemoryKind::Staging}) {
    if (auto s = alloc->reserve_pages(k, 1); !s.ok()) {
      std::fprintf(stderr, "预留页失败: %s\n", s.message.c_str());
      return 1;
    }
  }

  std::unique_ptr<res::ICubeMap> cube;
  // 即使 --sky off 也把六面载进来:off 帧的提交通路需要一条"被裁掉的天空盒 draw"
  // 来满足 submit_command_list 的"必须有 draw"契约。立方体贴图是稀缺资源,
  // 载入成本与是否可见无关,这里不省这一步。
  if (need_cube) {
    auto c = mbp::create_cube_map_from_faces(device, asset_root + "/cubeMap", args.scene);
    if (!c.ok()) {
      std::fprintf(stderr, "加载天空盒六面失败: %s\n", c.message().c_str());
      return 2;  // 资源问题 = 参数错误(不降级)
    }
    cube = c.take();
  }

  // ---- 目标 ----
  dev::TargetDesc td;
  td.kind = dev::TargetDesc::Kind::Offscreen;
  td.extent = {args.width, args.height};
  td.with_depth = false;
  td.format = dev::Format::RGBA8_UNORM;
  if (args.msaa > 1) td.samples = static_cast<dev::u32>(args.msaa);
  auto tgt = device.create_offscreen_target(td);
  if (!tgt.ok()) {
    std::fprintf(stderr, "创建离屏目标失败: %s\n", tgt.message().c_str());
    return 1;
  }
  dev::IRenderTarget* target = tgt.value().get();

  auto cmd = device.create_command_list();
  if (!cmd.ok()) {
    std::fprintf(stderr, "创建命令列表失败: %s\n", cmd.message().c_str());
    return 1;
  }

  const mbp::CameraSnapshot cam = make_camera(args);

  // 实体的世界 → 裁剪变换:走 ICamera(与 L3 场景同一个矩阵来源)。
  mbp::Mat4 view_proj = mbp::Mat4::identity();
  if (args.blocks > 0) {
    auto camera = mbp::create_camera();
    if (!camera) {
      std::fprintf(stderr, "内部错误:无法创建相机\n");
      return 1;
    }
    if (auto s = camera->set_desc(cam.desc); !s.ok()) {
      std::fprintf(stderr, "相机描述非法: %s\n", s.message.c_str());
      return 1;
    }
    if (auto s = camera->set_pose(cam.pose); !s.ok()) {
      std::fprintf(stderr, "相机位姿非法: %s\n", s.message.c_str());
      return 1;
    }
    view_proj = camera->view_proj_matrix();
  }

  // ---- 一个 pass:清屏 → 天空 → 实体 ----
  res::RenderPassDesc rp;
  rp.target = target;
  rp.clear_color = true;
  rp.clear_color_rgba[0] = args.clear[0] / 255.0f;
  rp.clear_color_rgba[1] = args.clear[1] / 255.0f;
  rp.clear_color_rgba[2] = args.clear[2] / 255.0f;
  rp.clear_color_rgba[3] = 1.0f;
  rp.clear_depth = false;
  if (auto s = cmd.value()->begin_pass(rp); !s.ok()) {
    std::fprintf(stderr, "begin_pass 失败: %s\n", s.message.c_str());
    return 1;
  }
  // 需要在同一 pass 里继续录制:天空通道必须与之共用(不自己开 pass)。
  dev::u32 sky_draws = 0;
  // ⚠️ 所有权(2026-11-11 根因):命令流里存的是**指针**,不是值。
  // `bind_pipeline(pipeline_.get(), ...)` 只把地址录进 RecordedCommand,
  // 真正的 vkCmdBindPipeline 发生在 submit_command_list() 回放时。
  // 所以**每个被录进命令流的管线/SkyPass 必须活到 submit 之后**。
  // 以前 `auto sky = create_sky_pass(...)` 写在 if 块里,块一结束就析构,
  // 于是 submit 时 dynamic_cast 拿到的是已析构对象 —— 表现为
  // "管线不是本设备的 VulkanPipeline",而且 vkCmdEndRenderPass 先报
  // "command buffer ... objects bound to it were invalidated"。
  // 这里把它们提到与 cmd 同级的生命周期,submit 之后再析构。
  std::unique_ptr<mbp::ISkyPass> sky;             // 延迟到 submit 之后析构
  std::unique_ptr<res::IPipeline> solid_pipeline;  // 同上
  // 天空通道**先于**实体录制(它是背景层),且不清屏(上面已经清过)。
  if (args.sky) {
    // 天空通道放在本 pass 里录制:不自己开 pass(clear 全关),避免嵌套 pass。
    sky = mbp::create_sky_pass(device);
    mbp::SkyPassDesc sd;
    sd.target_shape.color_format = dev::Format::RGBA8_UNORM;
    sd.target_shape.has_depth = false;
    sd.target_shape.samples = args.msaa > 1 ? static_cast<dev::u32>(args.msaa) : 1u;
    sd.cube = cube.get();
    if (auto s = sky->prepare(sd); !s.ok()) {
      std::fprintf(stderr, "天空盒 prepare 失败: %s\n", s.message.c_str());
      return 1;
    }
    mbp::SkyRecordOptions so;
    so.clear_color = false;
    so.clear_depth = false;
    if (auto s = sky->record(*cmd.value(), cam, so); !s.ok()) {
      std::fprintf(stderr, "天空盒 record 失败: %s\n", s.message.c_str());
      return 1;
    }
    if (sky->stats().draws == 0) {
      std::fprintf(stderr, "天空盒没有下发任何 draw（配置错误）\n");
      return 1;
    }
    sky_draws = sky->stats().draws;
  }

  // ---- 实体:证明"天空不写深度、实体能盖住它" ----
  dev::u32 solid_draws = 0;
  const std::vector<SolidVertex> verts = build_blocks(cam, args.blocks);
  if (!verts.empty()) {
    res::BufferDesc bd;
    bd.size = verts.size() * sizeof(SolidVertex);
    bd.usage = res::BufferUsage::Vertex;
    auto vb = device.create_buffer(bd);
    if (!vb.ok()) {
      std::fprintf(stderr, "创建顶点缓冲失败: %s\n", vb.message().c_str());
      return 1;
    }
    if (auto s = vb.value()->upload(verts.data(), bd.size); !s.ok()) {
      std::fprintf(stderr, "上传顶点失败: %s\n", s.message.c_str());
      return 1;
    }
    vb.value()->flush_pending();
    auto pl = device.create_pipeline(solid_pipeline_desc(args.msaa));
    if (!pl.ok()) {
      std::fprintf(stderr, "创建实体管线失败: %s\n", pl.message().c_str());
      return 1;
    }
    if (auto s = cmd.value()->bind_pipeline(pl.value().get(), pl.value()->hash()); !s.ok()) {
      std::fprintf(stderr, "绑定实体管线失败: %s\n", s.message.c_str());
      return 1;
    }
    // Vertex | Fragment 两个 stage 的位(VkShaderStageFlagBits 同值)。
    constexpr dev::u32 kStageVertexFragment = 0x1u | 0x10u;
    if (auto s = cmd.value()->push_constants(kStageVertexFragment, 0, sizeof(float) * 16,
                                             view_proj.m);
        !s.ok()) {
      std::fprintf(stderr, "实体 push_constants 失败: %s\n", s.message.c_str());
      return 1;
    }
    if (auto s = cmd.value()->bind_vertex_buffer(vb.value()->id(), 0, 0); !s.ok()) {
      std::fprintf(stderr, "绑定顶点缓冲失败: %s\n", s.message.c_str());
      return 1;
    }
    res::DrawArgs da;
    da.vertex_count = static_cast<dev::u32>(verts.size());
    da.instance_count = 1;
    if (auto s = cmd.value()->draw(da); !s.ok()) {
      std::fprintf(stderr, "实体 draw 失败: %s\n", s.message.c_str());
      return 1;
    }
    solid_draws = 1;
  }

  // `--sky off --blocks 0`:本轮只有"清屏"这一个动作,没有任何可见 draw。
  // 但 submit_command_list 的硬契约是"没有 draw 就拒绝提交",于是清屏永远执行不到,
  // 输出文件也不会生成 —— 这正是 --sky off 曾经直接返回 1 的原因。
  //
  // 解法(与 tests/magicblocks_l3_sky_test.cpp 的 off-frame 一致):仍然录制并提交一条
  // **天空盒自己的 draw**,但用 1x1 越界 scissor 把它完全裁掉 —— 通路确实执行了,
  // 却没有片元落到目标上,于是结果就是纯清屏基线。
  // (set_scissor 拒绝 0 宽高,所以不能用 0x0 去"关掉"它。)
  // 这里复用天空盒管线,而不是再造一条实体管线:零 draw 场景连顶点缓冲都没有。
  if (sky_draws == 0 && solid_draws == 0) {
    sky = mbp::create_sky_pass(device);
    mbp::SkyPassDesc sd;
    sd.target_shape.color_format = dev::Format::RGBA8_UNORM;
    sd.target_shape.has_depth = false;
    sd.target_shape.samples = args.msaa > 1 ? static_cast<dev::u32>(args.msaa) : 1u;
    sd.cube = cube.get();
    if (auto s = sky->prepare(sd); !s.ok()) {
      std::fprintf(stderr, "天空盒 prepare 失败: %s\n", s.message.c_str());
      return 1;
    }
    res::ScissorRect off_frame{};
    off_frame.x = static_cast<dev::i32>(args.width);
    off_frame.y = 0;
    off_frame.width = 1;
    off_frame.height = 1;
    if (auto s = cmd.value()->set_scissor(off_frame); !s.ok()) {
      std::fprintf(stderr, "设置屏外 scissor 失败: %s\n", s.message.c_str());
      return 1;
    }
    mbp::SkyRecordOptions so;
    so.clear_color = false;
    so.clear_depth = false;
    if (auto s = sky->record(*cmd.value(), cam, so); !s.ok()) {
      std::fprintf(stderr, "屏外天空盒 draw 失败: %s\n", s.message.c_str());
      return 1;
    }
  }

  if (auto s = cmd.value()->end_pass(); !s.ok()) {
    std::fprintf(stderr, "end_pass 失败: %s\n", s.message.c_str());
    return 1;
  }
  if (auto s = device.submit_command_list(*cmd.value()); !s.ok()) {
    std::fprintf(stderr, "提交失败: %s\n", s.message.c_str());
    return 1;
  }

  std::vector<std::uint8_t> rgba(static_cast<std::size_t>(args.width) * args.height * 4u, 0);
  if (auto s = target->readback_rgba8(rgba.data(), rgba.size()); !s.ok()) {
    std::fprintf(stderr, "读回失败: %s\n", s.message.c_str());
    return 1;
  }
  magicblocks::ImageRgba8 img;
  img.width = args.width;
  img.height = args.height;
  img.pixels = std::move(rgba);
  std::string err;
  if (!magicblocks::write_png_rgba8(args.output, img, &err)) {
    std::fprintf(stderr, "写 PNG 失败: %s\n", err.c_str());
    return 1;
  }

  if (args.print_stats) {
    std::printf(
        "{\"ok\": true, \"sky_draws\": %u, \"solid_draws\": %u, \"validation_errors\": %u}\n",
        static_cast<unsigned>(sky_draws), static_cast<unsigned>(solid_draws),
        static_cast<unsigned>(device.validation_error_count()));
  }
  std::printf("wrote %s (%ux%u) sky=%s\n", args.output.c_str(), args.width, args.height,
              args.sky ? "on" : "off");
  return 0;
}
