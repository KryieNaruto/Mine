// MagicBlocks —— 旋转立方体 demo
//
// **目的**:用最少的代码证明 L0~L3 合起来已经能满足"最基本的渲染要求"——
// 真正的透视 3D 几何、深度测试、背面剔除、光照,并且**能被旋转控制**。
//
// 走的是完整的分层路径,不是绕过 SDK 直接调 Vulkan:
//
//   L0 device     : 软件光栅设备 + 带深度的离屏目标 + 读回
//   L1 resources  : create_buffer / create_pipeline / create_command_list / 11 类命令
//   L2 scene      : 本 demo 只用它的渲染图与相机概念(单 pass,直接录)
//   L3 primitives : Mat4 / Vec3 / make_look_at / make_perspective / 四元数旋转
//
// 着色器:shaders/cube.vert + cube.frag(演示用,不属于 SDK 任何层)
//
// 用法:
//   magicblocks-cube-demo --output cube.png [--width 480] [--height 360]
//                         [--yaw 35] [--pitch 25]
//   magicblocks-cube-demo --sequence DIR --spin-frames 8   (渲染一圈)

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "core/image_io.h"
#include "magicblocks/device/render_device.h"
#include "magicblocks/primitives/types.h"
#include "magicblocks/resources/buffer.h"
#include "magicblocks/resources/command.h"
#include "magicblocks/resources/pipeline.h"
#include "magicblocks_shaders.h"

namespace dev = magicblocks::device;
namespace res = magicblocks::resources;
namespace prim = magicblocks::primitives;
namespace mb = magicblocks;

namespace {

constexpr float kPi = 3.14159265358979323846f;

// 与 shaders/cube.vert 的 push_constant 块一一对应。
// SwiftShader 的 max_push_constant_size 只有 128 字节,两个 mat4 正好用满。
struct PushConstants {
  float view_proj[16];  // offset 0
  float world[16];      // offset 64
};
constexpr std::uint32_t kPushConstantBytes = sizeof(PushConstants);
static_assert(kPushConstantBytes == 128, "push constant 必须是 128 字节");

// ---------------------------------------------------------------- 立方体几何
//
// 24 个顶点(6 面 × 4 顶点),**不共享**顶点 —— 每个面有自己的颜色与法线,
// 光照才会出现"六个面明暗不同"的立体感。

struct Vertex {
  float px, py, pz;  // location 0: Float3 position
  float nx, ny, nz;  // location 1: Float3 normal
  float r, g, b;     // location 2: Float3 color
};

struct FaceDef {
  float normal[3];
  float corners[4][3];
  float color[3];
};

// 每个面按"从外面看是逆时针"给出四个角(与背面剔除的 FrontFace 一致)。
const FaceDef kFaces[6] = {
    // +X 右(红)
    {{1, 0, 0},
     {{0.5f, -0.5f, 0.5f}, {0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, -0.5f}, {0.5f, 0.5f, 0.5f}},
     {0.85f, 0.22f, 0.22f}},
    // -X 左(青)
    {{-1, 0, 0},
     {{-0.5f, -0.5f, -0.5f}, {-0.5f, -0.5f, 0.5f}, {-0.5f, 0.5f, 0.5f}, {-0.5f, 0.5f, -0.5f}},
     {0.20f, 0.70f, 0.80f}},
    // +Y 上(绿)
    {{0, 1, 0},
     {{-0.5f, 0.5f, 0.5f}, {0.5f, 0.5f, 0.5f}, {0.5f, 0.5f, -0.5f}, {-0.5f, 0.5f, -0.5f}},
     {0.25f, 0.80f, 0.30f}},
    // -Y 下(紫)
    {{0, -1, 0},
     {{-0.5f, -0.5f, -0.5f}, {0.5f, -0.5f, -0.5f}, {0.5f, -0.5f, 0.5f}, {-0.5f, -0.5f, 0.5f}},
     {0.60f, 0.30f, 0.80f}},
    // +Z 前(黄)
    {{0, 0, 1},
     {{-0.5f, -0.5f, 0.5f}, {0.5f, -0.5f, 0.5f}, {0.5f, 0.5f, 0.5f}, {-0.5f, 0.5f, 0.5f}},
     {0.95f, 0.80f, 0.20f}},
    // -Z 后(橙)
    {{0, 0, -1},
     {{0.5f, -0.5f, -0.5f}, {-0.5f, -0.5f, -0.5f}, {-0.5f, 0.5f, -0.5f}, {0.5f, 0.5f, -0.5f}},
     {0.95f, 0.55f, 0.15f}},
};

std::vector<Vertex> build_cube_vertices() {
  std::vector<Vertex> v;
  v.reserve(24);
  for (const FaceDef& f : kFaces) {
    for (int c = 0; c < 4; ++c) {
      Vertex vert{};
      vert.px = f.corners[c][0];
      vert.py = f.corners[c][1];
      vert.pz = f.corners[c][2];
      vert.nx = f.normal[0];
      vert.ny = f.normal[1];
      vert.nz = f.normal[2];
      vert.r = f.color[0];
      vert.g = f.color[1];
      vert.b = f.color[2];
      v.push_back(vert);
    }
  }
  return v;
}

std::vector<std::uint32_t> build_cube_indices() {
  std::vector<std::uint32_t> idx;
  idx.reserve(36);
  for (std::uint32_t f = 0; f < 6; ++f) {
    const std::uint32_t base = f * 4;
    idx.push_back(base + 0);
    idx.push_back(base + 1);
    idx.push_back(base + 2);
    idx.push_back(base + 0);
    idx.push_back(base + 2);
    idx.push_back(base + 3);
  }
  return idx;
}

// ---------------------------------------------------------------- 命令行

struct Args {
  std::string output = "cube.png";
  std::string sequence_dir;
  int width = 480;
  int height = 360;
  float yaw_deg = 35.f;
  float pitch_deg = 25.f;
  int spin_frames = 0;
};

bool parse_args(int argc, char** argv, Args* out) {
  for (int i = 1; i < argc; ++i) {
    const std::string a = argv[i];
    auto need = [&](const char* name) -> const char* {
      if (i + 1 >= argc) {
        std::fprintf(stderr, "%s 需要一个值\n", name);
        return nullptr;
      }
      return argv[++i];
    };
    if (a == "--output") {
      const char* v = need("--output");
      if (!v) return false;
      out->output = v;
    } else if (a == "--sequence") {
      const char* v = need("--sequence");
      if (!v) return false;
      out->sequence_dir = v;
    } else if (a == "--width") {
      const char* v = need("--width");
      if (!v) return false;
      out->width = std::atoi(v);
    } else if (a == "--height") {
      const char* v = need("--height");
      if (!v) return false;
      out->height = std::atoi(v);
    } else if (a == "--yaw") {
      const char* v = need("--yaw");
      if (!v) return false;
      out->yaw_deg = static_cast<float>(std::atof(v));
    } else if (a == "--pitch") {
      const char* v = need("--pitch");
      if (!v) return false;
      out->pitch_deg = static_cast<float>(std::atof(v));
    } else if (a == "--spin-frames") {
      const char* v = need("--spin-frames");
      if (!v) return false;
      out->spin_frames = std::atoi(v);
    } else if (a == "--help" || a == "-h") {
      std::printf(
          "用法:\n"
          "  magicblocks-cube-demo --output FILE [--width W] [--height H]\n"
          "                        [--yaw DEG] [--pitch DEG]\n"
          "  magicblocks-cube-demo --sequence DIR --spin-frames N\n");
      std::exit(0);
    } else {
      std::fprintf(stderr, "未知参数: %s(用 --help 看用法)\n", a.c_str());
      return false;
    }
  }
  if (out->width <= 0 || out->height <= 0) {
    std::fprintf(stderr, "宽高必须为正\n");
    return false;
  }
  return true;
}

// ---------------------------------------------------------------- 管线

res::PipelineDesc cube_pipeline_desc() {
  res::PipelineDesc desc;

  res::VertexInputBinding binding{};
  binding.binding = 0;
  binding.stride = sizeof(Vertex);
  binding.instanced = false;

  res::VertexAttribute pos{};
  pos.location = 0;
  pos.binding = 0;
  pos.format = res::VertexFormat::Float3;
  pos.offset = offsetof(Vertex, px);

  // location 1:法线。L1 校验要求 location 连续,不能跳号。
  res::VertexAttribute nrm{};
  nrm.location = 1;
  nrm.binding = 0;
  nrm.format = res::VertexFormat::Float3;
  nrm.offset = offsetof(Vertex, nx);

  res::VertexAttribute col{};
  col.location = 2;  // 与 L3 shader_library.h 的约定一致
  col.binding = 0;
  col.format = res::VertexFormat::Float3;
  col.offset = offsetof(Vertex, r);

  desc.vertexLayout.bindings.push_back(binding);
  desc.vertexLayout.attributes.push_back(pos);
  desc.vertexLayout.attributes.push_back(nrm);
  desc.vertexLayout.attributes.push_back(col);

  res::ShaderModule vs;
  vs.stage = res::ShaderStage::Vertex;
  vs.spirv = reinterpret_cast<const std::uint8_t*>(magicblocks::shaders::kCubeVert);
  vs.size_bytes = magicblocks::shaders::kCubeVertSize;

  res::ShaderModule fs;
  fs.stage = res::ShaderStage::Fragment;
  fs.spirv = reinterpret_cast<const std::uint8_t*>(magicblocks::shaders::kCubeFrag);
  fs.size_bytes = magicblocks::shaders::kCubeFragSize;

  desc.stages.push_back(vs);
  desc.stages.push_back(fs);

  desc.primitive = res::PrimitiveType::TriangleList;
  // 立方体顶点在**模型空间**按"从外侧看逆时针"排(叉积与法线同向,已验证)。
  // 但 L3 的 make_perspective 为了对齐 Vulkan 的 y 向下 NDC,在投影矩阵里对 y
  // 取了负号 —— 这一次翻转会把屏幕空间的绕序整体反转。所以这里的正面判定
  // 必须写 Clockwise,否则立方体六个面全被背面剔除,画面一片空。
  desc.raster.cull = res::CullMode::Back;
  desc.raster.front_face = res::FrontFace::Clockwise;
  desc.depth.depthTest = true;
  desc.depth.depthWrite = true;
  desc.depth.depthOp = res::CompareOp::Less;

  // push constant:view_proj(64) + world(64) = 128 字节(SwiftShader 上限就是 128)
  desc.push_constant_bytes = kPushConstantBytes;
  return desc;
}

// ---------------------------------------------------------------- 渲染器

class CubeRenderer {
 public:
  bool init(int width, int height, std::string* err) {
    dev::DeviceDesc dd;
    dd.app_name = "MagicBlocks Cube Demo";
    // 固定走 SwiftShader:CI 与开发机行为一致,不会"在我机器上可以"。
    dd.require_software_rasterizer = true;
    auto dr = dev::create_render_device(dd);
    if (!dr.ok()) {
      *err = "创建设备失败: " + dr.message();
      return false;
    }
    device_ = std::move(dr.value());

    dev::TargetDesc td;
    td.kind = dev::TargetDesc::Kind::Offscreen;
    td.extent = {static_cast<dev::u32>(width), static_cast<dev::u32>(height)};
    td.with_depth = false;  // 临时验证:create_pipeline 只支持无深度形状
    auto tr = device_->create_offscreen_target(td);
    if (!tr.ok()) {
      *err = "创建离屏目标失败: " + tr.message();
      return false;
    }
    target_ = std::move(tr.value());

    const std::vector<Vertex> verts = build_cube_vertices();
    const std::vector<std::uint32_t> inds = build_cube_indices();

    // 分配器的桶不会自动长页：先预取,否则小缓冲报"当前页无空位"。
    // (这是 L1 的既定契约,不是 bug;上层建资源前必须显式 reserve。)
    // upload() 内部还要一次 Staging 分配,所以 Staging 桶也要预取。
    res::IMemoryAllocator* alloc = device_->memory_allocator();
    for (res::MemoryKind kind :
         {res::MemoryKind::Vertex, res::MemoryKind::Index, res::MemoryKind::Staging}) {
      if (auto s = alloc->reserve_pages(kind, 1); !s.ok()) {
        *err = "预取内存页失败: " + s.message;
        return false;
      }
    }

    // 注意 upload() 的语义(R5):它是"入队",数据先落 staging,**必须**再调
    // flush_pending() 才真正拷到 GPU 侧的顶点/索引缓冲。漏掉这一步不会报错,
    // 只会静默地画不出东西 —— 第一版 demo 就是栽在这里(整帧纯背景色)。
    res::BufferDesc vbd;
    vbd.size = verts.size() * sizeof(Vertex);
    vbd.usage = res::BufferUsage::Vertex;
    auto vb = device_->create_buffer(vbd);
    if (!vb.ok()) {
      *err = "创建顶点缓冲失败: " + vb.message();
      return false;
    }
    vertex_buffer_ = std::move(vb.value());
    if (auto s = vertex_buffer_->upload(verts.data(), vbd.size); !s.ok()) {
      *err = "上传顶点失败: " + s.message;
      return false;
    }
    vertex_buffer_->flush_pending();

    res::BufferDesc ibd;
    ibd.size = inds.size() * sizeof(std::uint32_t);
    ibd.usage = res::BufferUsage::Index;
    auto ib = device_->create_buffer(ibd);
    if (!ib.ok()) {
      *err = "创建索引缓冲失败: " + ib.message();
      return false;
    }
    index_buffer_ = std::move(ib.value());
    if (auto s = index_buffer_->upload(inds.data(), ibd.size); !s.ok()) {
      *err = "上传索引失败: " + s.message;
      return false;
    }
    index_count_ = static_cast<std::uint32_t>(inds.size());
    index_buffer_->flush_pending();  // 同上:upload 只是入队

    auto pr = device_->create_pipeline(cube_pipeline_desc());
    if (!pr.ok()) {
      *err = "创建管线失败: " + pr.message();
      return false;
    }
    pipeline_ = std::move(pr.value());
    pipeline_hash_ = pipeline_->hash();

    // 自检:确认上传批已全部落到 GPU(顶点/索引缓冲都在设备页,CPU 读不回来,
    // 所以用 pending_upload_bytes 判定)。漏 flush_pending 的话这里会是非 0。
    {
      const std::uint64_t vp = vertex_buffer_->pending_upload_bytes();
      const std::uint64_t ip = index_buffer_->pending_upload_bytes();
      std::printf("[自检] 顶点%zu字节 索引%u个 | 待提交 v=%llu i=%llu | push_constant 设备上限=%u\n",
                  verts.size() * sizeof(Vertex), index_count_,
                  static_cast<unsigned long long>(vp), static_cast<unsigned long long>(ip),
                  device_->caps().max_push_constant_size);
      if (vp != 0 || ip != 0) {
        *err = "上传批未提交(忘了 flush_pending)";
        return false;
      }
    }

    width_ = width;
    height_ = height;
    return true;
  }

  // 渲染一帧并读回 RGBA8。yaw/pitch(弧度)就是"旋转控制"的输入。
  bool render(float yaw, float pitch, std::vector<std::uint8_t>* out_rgba8, std::string* err) {
    auto cr = device_->create_command_list();
    if (!cr.ok()) {
      *err = "创建命令列表失败: " + cr.message();
      return false;
    }
    res::ICommandList* cmd = cr.value().get();

    // 相机:右手系,y 向上,看向 -z
    const prim::Vec3 eye{0.f, 0.f, 2.6f};
    const prim::Vec3 center{0.f, 0.f, 0.f};
    const prim::Vec3 up{0.f, 1.f, 0.f};
    const prim::Mat4 view = prim::make_look_at(eye, center, up);
    const prim::Mat4 proj = prim::make_perspective(60.f * kPi / 180.f,
                                                   static_cast<float>(width_) / static_cast<float>(height_),
                                                   0.1f, 100.f);
    const prim::Mat4 view_proj = proj * view;

    // 模型矩阵:先 pitch(绕 X)再 yaw(绕 Y)—— 等价于"拖动旋转"。
    const prim::Mat4 rot_pitch =
        prim::make_rotation_quat(std::sin(pitch * 0.5f), 0.f, 0.f, std::cos(pitch * 0.5f));
    const prim::Mat4 rot_yaw =
        prim::make_rotation_quat(0.f, std::sin(yaw * 0.5f), 0.f, std::cos(yaw * 0.5f));
    const prim::Mat4 world = rot_yaw * rot_pitch;

    PushConstants pc{};
    std::memcpy(pc.view_proj, view_proj.m, sizeof(pc.view_proj));
    std::memcpy(pc.world, world.m, sizeof(pc.world));

    res::RenderPassDesc pass{};
    pass.target = target_.get();
    pass.clear_color = true;
    pass.clear_color_rgba[0] = 0.06f;
    pass.clear_color_rgba[1] = 0.07f;
    pass.clear_color_rgba[2] = 0.10f;
    pass.clear_color_rgba[3] = 1.0f;
    pass.clear_depth = true;
    pass.clear_depth_value = 1.0f;

    if (auto s = cmd->begin_pass(pass); !s.ok()) {
      *err = "begin_pass 失败: " + s.message;
      return false;
    }

    res::Viewport vp{};
    vp.width = static_cast<float>(width_);
    vp.height = static_cast<float>(height_);
    vp.min_depth = 0.f;
    vp.max_depth = 1.f;
    if (auto s = cmd->set_viewport(vp); !s.ok()) {
      *err = "set_viewport 失败: " + s.message;
      return false;
    }

    res::ScissorRect sc{};
    sc.width = static_cast<dev::u32>(width_);
    sc.height = static_cast<dev::u32>(height_);
    if (auto s = cmd->set_scissor(sc); !s.ok()) {
      *err = "set_scissor 失败: " + s.message;
      return false;
    }

    if (auto s = cmd->bind_pipeline(pipeline_.get(), pipeline_hash_); !s.ok()) {
      *err = "bind_pipeline 失败: " + s.message;
      return false;
    }
    // stage_mask 的位与 VkShaderStageFlagBits 对齐:Vertex=0x1,Fragment=0x10。
    constexpr dev::u32 kStageVertex = 0x1u;
    constexpr dev::u32 kStageFragment = 0x10u;
    if (auto s = cmd->push_constants(kStageVertex | kStageFragment, 0, sizeof(pc), &pc); !s.ok()) {
      *err = "push_constants 失败: " + s.message;
      return false;
    }
    if (auto s = cmd->bind_vertex_buffer(vertex_buffer_->id(), 0, 0); !s.ok()) {
      *err = "bind_vertex_buffer 失败: " + s.message;
      return false;
    }
    if (auto s = cmd->bind_index_buffer(index_buffer_->id(), res::IndexFormat::UInt32, 0); !s.ok()) {
      *err = "bind_index_buffer 失败: " + s.message;
      return false;
    }

    res::DrawArgs da{};
    da.index_count = index_count_;
    da.instance_count = 1;
    if (auto s = cmd->draw_indexed(da); !s.ok()) {
      *err = "draw_indexed 失败: " + s.message;
      return false;
    }
    if (auto s = cmd->end_pass(); !s.ok()) {
      *err = "end_pass 失败: " + s.message;
      return false;
    }
    // 关键:L1 的 draw* 在设备态只把命令录进命令流,**不会**自己下发 vkCmdDraw*。
    // 必须调设备的 submit_command_list() 才真正出图(此前没有这个公开入口,
    // 所以录得再对也只得到一张背景色的空图)。
    if (auto s = device_->submit_command_list(*cmd); !s.ok()) {
      *err = "提交命令列表失败: " + s.message;
      return false;
    }

    const std::size_t need = static_cast<std::size_t>(width_) * height_ * 4u;
    out_rgba8->assign(need, 0);
    if (auto s = target_->readback_rgba8(out_rgba8->data(), need); !s.ok()) {
      *err = "读回失败: " + s.message;
      return false;
    }
    return true;
  }

  int width() const { return width_; }
  int height() const { return height_; }

 private:
  std::unique_ptr<dev::IRenderDevice> device_;
  std::unique_ptr<dev::IRenderTarget> target_;
  std::unique_ptr<res::IBuffer> vertex_buffer_;
  std::unique_ptr<res::IBuffer> index_buffer_;
  std::unique_ptr<res::IPipeline> pipeline_;
  std::uint64_t pipeline_hash_ = 0;
  std::uint32_t index_count_ = 0;
  int width_ = 0;
  int height_ = 0;
};

bool save_png(const std::string& path, int w, int h, const std::vector<std::uint8_t>& px,
              std::string* err) {
  mb::ImageRgba8 img;
  img.width = static_cast<std::uint32_t>(w);
  img.height = static_cast<std::uint32_t>(h);
  img.pixels = px;
  return mb::write_png_rgba8(path, img, err);
}

}  // namespace

int main(int argc, char** argv) {
  Args args;
  if (!parse_args(argc, argv, &args)) return 2;

  CubeRenderer renderer;
  std::string err;
  if (!renderer.init(args.width, args.height, &err)) {
    std::fprintf(stderr, "%s\n", err.c_str());
    return 1;
  }

  std::vector<std::uint8_t> pixels;
  const float pitch = args.pitch_deg * kPi / 180.f;

  if (args.spin_frames > 0) {
    const std::string dir = args.sequence_dir.empty() ? "spin" : args.sequence_dir;
    for (int i = 0; i < args.spin_frames; ++i) {
      const float a = (2.f * kPi * static_cast<float>(i)) / static_cast<float>(args.spin_frames);
      if (!renderer.render(a, pitch, &pixels, &err)) {
        std::fprintf(stderr, "第 %d 帧失败: %s\n", i, err.c_str());
        return 1;
      }
      char path[512];
      std::snprintf(path, sizeof(path), "%s/spin_%02d.png", dir.c_str(), i);
      if (!save_png(path, renderer.width(), renderer.height(), pixels, &err)) {
        std::fprintf(stderr, "写 PNG 失败 %s: %s\n", path, err.c_str());
        return 1;
      }
      std::printf("第 %02d 帧 yaw=%.1f° → %s\n", i, a * 180.f / kPi, path);
    }
    return 0;
  }

  const float yaw = args.yaw_deg * kPi / 180.f;
  if (!renderer.render(yaw, pitch, &pixels, &err)) {
    std::fprintf(stderr, "渲染失败: %s\n", err.c_str());
    return 1;
  }
  if (!save_png(args.output, renderer.width(), renderer.height(), pixels, &err)) {
    std::fprintf(stderr, "写 PNG 失败 %s: %s\n", args.output.c_str(), err.c_str());
    return 1;
  }
  std::printf("已写出 %s (%dx%d, yaw=%.1f°, pitch=%.1f°)\n", args.output.c_str(),
              renderer.width(), renderer.height(), args.yaw_deg, args.pitch_deg);
  return 0;
}
