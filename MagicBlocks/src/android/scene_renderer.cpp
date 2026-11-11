// MagicBlocks —— 场景渲染器实现（PIMPL）
//
// 所有 SDK 内部头**只在这里** include。scene_renderer.h 里没有它们(见那头文件顶部
// 的说明)。壳层通过 mb_capi.h 的 C ABI 进来，见 src/capi/mb_capi.cpp。
#include "scene_renderer.h"

#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#if defined(MB_ANDROID_USE_NDK)
#include <android/native_window.h>
#endif

// ⚠ 这里**不许**再出现 "backend/..." 的 include。
//
// 这一层(android/)是最靠近 UI 的一层,它只认识公共面(device/、resources/、
// primitives/)加上 core/device_ops.h 那**一个**转发面。需要后端私有知识的动作
// (按采样数建链、按交换链形状建管线、读实际采样数/旋转角)全部经 core:: 转发,
// backend 的类型与 VK_ 宏一步都不许跨过来。
// 判定是机器做的:tools/check_layer_purity.py(G3),不是靠记忆。
#include "core/device_ops.h"
#include "mb_internal/magicblocks/device/render_device.h"
#include "mb_internal/magicblocks/device/swapchain.h"
#include "mb_internal/magicblocks/primitives/model_catalog.h"
#include "mb_internal/magicblocks/primitives/types.h"
#include "mb_internal/magicblocks/resources/buffer.h"
#include "mb_internal/magicblocks/resources/command.h"
#include "mb_internal/magicblocks/resources/pipeline.h"

#include "magicblocks_shaders.h"

namespace magicblocks::android {

namespace dev = magicblocks::device;
namespace res = magicblocks::resources;
namespace prim = magicblocks::primitives;
namespace core = magicblocks::core;

namespace {

// 源文件内部的实现细节(std::string/vector)只在这里出现 —— 不进头文件。
constexpr float kPi = 3.14159265358979323846f;

// 与 shaders/cube.vert 的 push_constant 块一一对应（128 字节 = 两个 mat4）。
struct PushConstants {
  float view_proj[16];  // offset 0
  float world[16];      // offset 64
};
static_assert(sizeof(PushConstants) == 128, "push constant 必须是 128 字节");

// 一个面 4 个顶点，6 面共 24 个；每面独立颜色/法线 → 六面明暗不同。
struct Vertex {
  float px, py, pz;
  float nx, ny, nz;
  float r, g, b;
};

struct FaceDef {
  float normal[3];
  float corners[4][3];
  float color[3];
};

// 每面按"从外面看逆时针"给出四角（与背面剔除的 FrontFace 一致）。
const FaceDef kFaces[6] = {
    {{1, 0, 0},
     {{0.5f, -0.5f, 0.5f}, {0.5f, -0.5f, -0.5f}, {0.5f, 0.5f, -0.5f}, {0.5f, 0.5f, 0.5f}},
     {0.85f, 0.22f, 0.22f}},
    {{-1, 0, 0},
     {{-0.5f, -0.5f, -0.5f}, {-0.5f, -0.5f, 0.5f}, {-0.5f, 0.5f, 0.5f}, {-0.5f, 0.5f, -0.5f}},
     {0.20f, 0.70f, 0.80f}},
    {{0, 1, 0},
     {{-0.5f, 0.5f, 0.5f}, {0.5f, 0.5f, 0.5f}, {0.5f, 0.5f, -0.5f}, {-0.5f, 0.5f, -0.5f}},
     {0.25f, 0.80f, 0.30f}},
    {{0, -1, 0},
     {{-0.5f, -0.5f, -0.5f}, {0.5f, -0.5f, -0.5f}, {0.5f, -0.5f, 0.5f}, {-0.5f, -0.5f, 0.5f}},
     {0.60f, 0.30f, 0.80f}},
    {{0, 0, 1},
     {{-0.5f, -0.5f, 0.5f}, {0.5f, -0.5f, 0.5f}, {0.5f, 0.5f, 0.5f}, {-0.5f, 0.5f, 0.5f}},
     {0.95f, 0.80f, 0.20f}},
    {{0, 0, -1},
     {{0.5f, -0.5f, -0.5f}, {-0.5f, -0.5f, -0.5f}, {-0.5f, 0.5f, -0.5f}, {0.5f, 0.5f, -0.5f}},
     {0.95f, 0.55f, 0.15f}},
};

std::vector<Vertex> build_vertices() {
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

std::vector<std::uint32_t> build_indices() {
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

res::PipelineDesc cube_pipeline_desc(res::u32 samples) {
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

  res::VertexAttribute nrm{};
  nrm.location = 1;
  nrm.binding = 0;
  nrm.format = res::VertexFormat::Float3;
  nrm.offset = offsetof(Vertex, nx);

  res::VertexAttribute col{};
  col.location = 2;
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
  desc.depth.depthTest = false;
  desc.depth.depthWrite = false;
  desc.push_constant_bytes = static_cast<res::u32>(sizeof(PushConstants));
  // 管线的采样数**必须**与交换链 render pass 的采样数逐项相同,否则
  // create_pipeline_for_shape 会直接拒绝(形状 samples=4 与 desc samples=1 不一致)。
  // 以前这里漏了赋值 → 恒为 1 → 开 ×4 必然建管线失败。
  desc.samples = samples;
  return desc;
}

}  // namespace

// ============================================================================
// Impl —— 真正的实现。SDK 内部类型全部藏在这里。
// ============================================================================
struct SceneRenderer::Impl {
  bool create_resources(std::string* err);
  void release_window();
  bool rebuild_pipeline_for_swapchain(std::string* err);
  bool record_into(dev::IRenderTarget* target, float qx, float qy, float qz, float qw, float scale,
                   float pan_x, float pan_y, std::string* err);

  std::unique_ptr<dev::IRenderDevice> device;
  std::unique_ptr<dev::ISwapchain> swapchain;
  std::uint32_t msaa_samples = 1;
  std::uint32_t effective_msaa_samples = 1;
  std::unique_ptr<res::IBuffer> vertex_buffer;
  std::unique_ptr<res::IBuffer> index_buffer;
  std::unique_ptr<res::IPipeline> pipeline;
  std::uint64_t pipeline_hash = 0;
  std::uint32_t index_count = 0;

  // 当前选中的预设模型（null = 画内置立方体）。所有权归这里：加载成功才替换，
  // 失败时旧模型原样保留 —— 用户点了列表里一个坏文件，画面不该跟着变黑。
  std::unique_ptr<prim::ILoadedModel> model;
  std::string model_name;

  // ANativeWindow 引用：attach 时持有，detach 时释放。
  void* native_window = nullptr;

  int width = 0;
  int height = 0;

  // 屏幕相对 swapchain image 的旋转（度：0/90/180/270）。
  //
  // pad 是横屏应用但面板物理竖置，SurfaceFlinger 合成时把 image 旋转 90° 铺满屏幕。
  // 后果：宽高比要取倒数（否则正方体被拉成 1.5:1），手势方向要补偿。
  //
  // 取值由 backend 的 `VulkanSwapchain::surface_rotation_degrees()` 解码 ——
  // 这里**不再**碰 `VK_SURFACE_TRANSFORM_*`（G3:VK_ 宏不得出现在 backend/ 之外）。
  int rotation_deg = 0;

  std::uint64_t frame_count = 0;
  std::string last_error;
  mutable std::mutex mutex;  // 壳层线程与渲染线程并发时的保护
};

// ---------------------------------------------------------------- 生命周期

SceneRenderer::SceneRenderer() : impl_(std::make_unique<Impl>()) {}

SceneRenderer::~SceneRenderer() {
  // ==========================================================================
  // 析构顺序 —— 这不是"整理一下"，是 t3 抓到的真实崩溃
  // ==========================================================================
  //
  // Impl 的成员按**声明顺序**销毁，而 `device` 声明在最前 —— 于是 reset(device)
  // 之后 pipeline/vertex/index 还活着，等它们析构时 `ctx_.device` 已是野值：
  //     vkDestroyPipeline: Invalid device [VUID-vkDestroyPipeline-device-parameter]
  // 在真实 loader 下这是 access violation(mb_capi_smoke 实测 0xC0000005)。
  //
  // 旧代码靠 `device_.reset()` 关掉一个 unique_ptr —— 它的**声明位置**决定了
  // 它们比 device 晚死，但旧类里 device_ 也是第一个成员……之所以以前没炸，
  // 是因为旧 JNI 路径下每个句柄都是 new 出来的，析构时机被 Application 控制，
  // 且崩溃会被 tester 当成"应用退出"忽略。C ABI 把它变成**每次 destroy 都走**
  // 的必经之路，问题因此暴露。
  //
  // 正确顺序：交换链 → 场景 GPU 资源(管线/缓冲/模型) → 设备。
  auto& self = *impl_;
  std::lock_guard<std::mutex> lk(self.mutex);
  self.swapchain.reset();
  self.release_window();
  self.pipeline.reset();
  self.vertex_buffer.reset();
  self.index_buffer.reset();
  self.model.reset();
  self.device.reset();
}

bool SceneRenderer::init(std::string* err) {
  auto& self = *impl_;
  std::lock_guard<std::mutex> lk(self.mutex);
  auto fail = [&](const std::string& m) {
    if (err) *err = m;
    self.last_error = m;
    return false;
  };

  dev::DeviceDesc dd;
  dd.app_name = "MagicBlocksPad";
  dd.enable_validation = false;
  // pad 上是真实的 Mali GPU —— 不能用软件光栅筛选。
  dd.require_software_rasterizer = false;

  auto dr = dev::create_render_device(dd);
  if (!dr.ok()) return fail("创建设备失败: " + dr.message());
  self.device = dr.take();

  return self.create_resources(err);
}

bool SceneRenderer::Impl::create_resources(std::string* err) {
  auto& self = *this;
  auto fail = [&](const std::string& m) {
    if (err) *err = m;
    self.last_error = m;
    return false;
  };

  const std::vector<Vertex> verts = build_vertices();
  const std::vector<std::uint32_t> inds = build_indices();

  // 分配器的桶不会自动长页，必须显式预取（L1 既定契约）。
  res::IMemoryAllocator* alloc = device->memory_allocator();
  for (res::MemoryKind kind :
       {res::MemoryKind::Vertex, res::MemoryKind::Index, res::MemoryKind::Staging}) {
    if (auto s = alloc->reserve_pages(kind, 1); !s.ok()) {
      return fail("预取内存页失败: " + s.message);
    }
  }

  // 顶点缓冲
  res::BufferDesc vbd;
  vbd.size = verts.size() * sizeof(Vertex);
  vbd.usage = res::BufferUsage::Vertex;
  auto vb = device->create_buffer(vbd);
  if (!vb.ok()) return fail("创建顶点缓冲失败: " + vb.message());
  vertex_buffer = vb.take();
  if (auto s = vertex_buffer->upload(verts.data(), vbd.size); !s.ok()) {
    return fail("上传顶点失败: " + s.message);
  }
  vertex_buffer->flush_pending();  // upload() 只是入队，必须 flush

  // 索引缓冲
  res::BufferDesc ibd;
  ibd.size = inds.size() * sizeof(std::uint32_t);
  ibd.usage = res::BufferUsage::Index;
  auto ib = device->create_buffer(ibd);
  if (!ib.ok()) return fail("创建索引缓冲失败: " + ib.message());
  index_buffer = ib.take();
  if (auto s = index_buffer->upload(inds.data(), ibd.size); !s.ok()) {
    return fail("上传索引失败: " + s.message);
  }
  index_count = static_cast<std::uint32_t>(inds.size());
  index_buffer->flush_pending();

  // 管线(attach 之前的初始管线永远是 1x:此时还没有交换链)
  auto pr = device->create_pipeline(cube_pipeline_desc(1));
  if (!pr.ok()) return fail("创建管线失败: " + pr.message());
  pipeline = pr.take();
  pipeline_hash = pipeline->hash();

  return true;
}

void SceneRenderer::Impl::release_window() {
#if defined(MB_ANDROID_USE_NDK)
  if (native_window) {
    ANativeWindow_release(reinterpret_cast<ANativeWindow*>(native_window));
    native_window = nullptr;
  }
#else
  native_window = nullptr;
#endif
}

void SceneRenderer::detach_surface() {
  auto& self = *impl_;
  std::lock_guard<std::mutex> lk(self.mutex);
  self.swapchain.reset();  // 必须先于 device 销毁
  self.release_window();
}

// ---------------------------------------------------------------- surface

bool SceneRenderer::attach_surface(void* window, int width, int height, std::string* err) {
  auto& self = *impl_;
  std::lock_guard<std::mutex> lk(self.mutex);
  auto fail = [&](const std::string& m) {
    if (err) *err = m;
    self.last_error = m;
    return false;
  };

  if (!self.device) return fail("attach_surface: 设备未初始化");
  if (!window) return fail("attach_surface: ANativeWindow 为空");
  if (width <= 0 || height <= 0) return fail("attach_surface: 尺寸非法");

  self.swapchain.reset();
  self.release_window();

  dev::NativeSurface ns;
  ns.kind = dev::NativeSurface::Kind::Android;
  ns.a = window;
  ns.b = 0;

  // 走带采样的入口：set_requested_msaa 必须先于 init()，而公共面
  // create_swapchain() 不带采样参数。MSAA 不可用时后端会退回 1x 并打日志，
  // 不会让这里失败 —— 所以只有一条创建路径，别写成"先建一条再换一条"，
  // 那会白建一条交换链（连同它下面的 surface 与图）。
  //
  // ⚠ 这一层**不许**知道 backend 的实现类型（不许 dynamic_cast<backend::…>，
  //   也不许自己拿 VulkanSwapchain::msaa_samples()）：那是 G3 分层纯度门控判红的
  //   反向依赖。带采样的建链入口收进 backends::DeviceFactory 之后，这里只调公共接口。
  const dev::Extent2D sc_extent{static_cast<dev::u32>(width), static_cast<dev::u32>(height)};
  dev::Result<std::unique_ptr<dev::ISwapchain>> sc =
      core::create_swapchain_with_msaa(self.device.get(), ns, sc_extent, self.msaa_samples);
  if (!sc.ok()) return fail("创建交换链失败: " + sc.message());
  self.swapchain = sc.take();
  self.native_window = window;  // 引用由壳层产生，归本对象持有

  // 实际生效的采样数：设备不支持时后端会退回 1x（并已打日志）。
  // 后面建管线必须用**这个**值，否则管线按 4 采样建、pass 按 1 采样开，
  // vkCmdDraw 会被静默丢弃 —— 屏幕上只剩清屏色，且没有任何报错。
  // 现在这个值由后端经核心中间层报告，本层不再 casting 到 VulkanSwapchain。
  self.effective_msaa_samples = core::swapchain_msaa_samples(self.swapchain.get());

  self.width = width;
  self.height = height;
  self.frame_count = 0;

  // 记录屏幕旋转。**解码在 backend 里做**(surface_rotation_degrees),
  // 这里只拿一个平台无关的角度 —— 以前这里是 3 行 `pt & VK_SURFACE_TRANSFORM_*`,
  // 让 VK_ 宏漏到 backend/ 之外。
  self.rotation_deg = core::swapchain_rotation_degrees(self.swapchain.get());

  // 管线必须按**交换链真实颜色格式**重建。
  // 理由：VkPipeline 绑定的 VkRenderPass 与 begin_pass 用的 render pass
  // 必须逐项兼容（格式/采样/深度）。不兼容时 vkCmdDraw 被**静默丢弃**。
  if (!self.rebuild_pipeline_for_swapchain(err)) return false;

  self.last_error.clear();
  return true;
}

bool SceneRenderer::Impl::rebuild_pipeline_for_swapchain(std::string* err) {
  auto& self = *this;
  auto fail = [&](const std::string& m) {
    if (err) *err = m;
    self.last_error = m;
    return false;
  };

  // 形状怎么填(颜色格式/采样数/from_swapchain/pass 的 initial-final layout)
  // 由后端决定，见 VulkanRenderDevice::create_swapchain_pipeline 的注释。
  // 2026-11-11（t6）：AttachmentShape 现在**带** from_swapchain，且必须置真；
  // 一旦复用管线自带 pass，管线 pass 的 finalLayout 恒为 GENERAL →
  // vkQueuePresentKHR 报 E-01430，上屏帧被丢掉。这条约束现在由后端保证。
  dev::Result<std::unique_ptr<res::IPipeline>> pr = core::create_swapchain_pipeline(
      device.get(), swapchain.get(), cube_pipeline_desc(effective_msaa_samples),
      effective_msaa_samples);
  if (!pr.ok()) return fail("按交换链形状建管线失败: " + pr.message());
  pipeline = pr.take();
  pipeline_hash = pipeline->hash();
  return true;
}

bool SceneRenderer::surface_ready() const {
  std::lock_guard<std::mutex> lk(impl_->mutex);
  return impl_->swapchain != nullptr;
}

// ---------------------------------------------------------------- 渲染

bool SceneRenderer::Impl::record_into(dev::IRenderTarget* target, float qx, float qy, float qz,
                                      float qw, float scale, float pan_x, float pan_y,
                                      std::string* err) {
  auto& self = *this;
  auto fail = [&](const std::string& m) {
    if (err) *err = m;
    self.last_error = m;
    return false;
  };

  auto cr = device->create_command_list();
  if (!cr.ok()) return fail("创建命令列表失败: " + cr.message());
  res::ICommandList* cmd = cr.value().get();

  // ==========================================================================
  // 相机：**四元数**姿态，永远绕**机体系三轴**转（arcball / turntable）
  // ==========================================================================
  //
  // 历史教训，别改回去：
  //   1. 最早的版本转的是**物体**（world = rot_yaw * rot_pitch）。两次旋转不
  //      可交换，pitch 一大幅度 yaw 就不再是干净的水平转。
  //   2. 第二版改成 (yaw, pitch) 球面参数相机。仍然把方向写成"世界 Y 多少度 +
  //      抬升多少度"，本质是万向节。
  //   3. 现在是**累积四元数**：拖动时右乘"相对相机自身轴"的小旋转，
  //          q ← q * Δq,  Δq = Δq_pitch(局部 X) * Δq_yaw(局部 Y)
  //      相机轴 = q 的列向量，所以"上下拖 = 绕相机 X、左右拖 = 绕相机 Y"，
  //      无论当前朝向如何都成立，360° 任意角度无死角、无夹取。
  const float dist = 2.6f / (scale > 0.05f ? scale : 0.05f);
  const prim::Vec3 orbit_center{pan_x, pan_y, 0.f};
  // 归一化一次再求逆：连续相乘几千次后浮点会漂，不归一化会慢慢变形。
  const float qn = std::sqrt(qx * qx + qy * qy + qz * qz + qw * qw);
  const float iq = qn > 1e-6f ? 1.f / qn : 1.f;
  const float ux = qx * iq, uy = qy * iq, uz = qz * iq, uw = qw * iq;
  // q 的列向量就是**相机自身的轴**（单位四元数 → 旋转矩阵 R，列 0=右、1=上、2=后）：
  //   back  = 2(xz - wy, yz + wx, 0.5 - x² - y²)
  // 相机看向 -back，所以"环绕中心 → eye"的方向就是 back。
  const prim::Vec3 back{2.f * (ux * uz - uw * uy), 2.f * (uy * uz + uw * ux),
                        1.f - 2.f * (ux * ux + uy * uy)};
  // up 取相机自身的上轴（列 1）。它天然与 back 正交，俯视/仰视都不会退化。
  const prim::Vec3 up{2.f * (ux * uy + uw * uz), 1.f - 2.f * (ux * ux + uz * uz),
                      2.f * (uy * uz - uw * ux)};
  const prim::Vec3 eye{orbit_center.x - dist * back.x, orbit_center.y - dist * back.y,
                       orbit_center.z - dist * back.z};
  const prim::Mat4 view = prim::make_look_at(eye, orbit_center, up);

  // 投影宽高比必须用**屏幕上实际呈现**的尺寸，而不是 swapchain image 的尺寸。
  // pad 是横屏应用但面板物理竖置（ROTATE_90），遇到 90/270 时取倒数。
  const bool rot90 = (rotation_deg == 90 || rotation_deg == 270);
  const float img_aspect = static_cast<float>(width) / static_cast<float>(height);
  const float aspect = rot90 ? (1.f / img_aspect) : img_aspect;
  const prim::Mat4 proj = prim::make_perspective(60.f * kPi / 180.f, aspect, 0.1f, 100.f);
  const prim::Mat4 view_proj = proj * view;

  // 模型矩阵：**恒等**。物体待在世界原点不动，旋转全部由上面的相机实现。
  // OBJ 加载时已经做过归一化，所以恒等矩阵下的取景与内置立方体一致。
  const prim::Mat4 world = prim::Mat4::identity();

  PushConstants pc{};
  std::memcpy(pc.view_proj, view_proj.m, sizeof(pc.view_proj));
  std::memcpy(pc.world, world.m, sizeof(pc.world));

  res::RenderPassDesc pass{};
  pass.target = target;
  pass.clear_color = true;
  pass.clear_color_rgba[0] = 0.06f;
  pass.clear_color_rgba[1] = 0.07f;
  pass.clear_color_rgba[2] = 0.10f;
  pass.clear_color_rgba[3] = 1.0f;
  pass.clear_depth = true;
  pass.clear_depth_value = 1.0f;

  if (auto s = cmd->begin_pass(pass); !s.ok()) return fail("begin_pass: " + s.message);

  res::Viewport vp{};
  vp.width = static_cast<float>(width);
  vp.height = static_cast<float>(height);
  vp.min_depth = 0.f;
  vp.max_depth = 1.f;
  if (auto s = cmd->set_viewport(vp); !s.ok()) return fail("set_viewport: " + s.message);

  res::ScissorRect sc{};
  sc.width = static_cast<dev::u32>(width);
  sc.height = static_cast<dev::u32>(height);
  if (auto s = cmd->set_scissor(sc); !s.ok()) return fail("set_scissor: " + s.message);

  // 画哪一层：选了预设模型就画模型，否则画内置立方体。两者的顶点布局一致
  // （位置/法线/颜色各 3 个 float），所以共用同一条管线。
  const res::IBuffer* vb = vertex_buffer.get();
  const res::IBuffer* ib = index_buffer.get();
  std::uint32_t draw_indices = index_count;
  if (model != nullptr) {
    const prim::IMesh* mesh = model->mesh();
    if (mesh != nullptr && mesh->vertex_buffer() != nullptr && mesh->index_buffer() != nullptr) {
      vb = mesh->vertex_buffer();
      ib = mesh->index_buffer();
      draw_indices = mesh->index_count();
    }
  }

  if (auto s = cmd->bind_pipeline(pipeline.get(), pipeline_hash); !s.ok()) {
    return fail("bind_pipeline: " + s.message);
  }

  constexpr dev::u32 kStageVertex = 0x1u;
  constexpr dev::u32 kStageFragment = 0x10u;
  if (auto s = cmd->push_constants(kStageVertex | kStageFragment, 0, sizeof(pc), &pc); !s.ok()) {
    return fail("push_constants: " + s.message);
  }
  if (auto s = cmd->bind_vertex_buffer(vb->id(), 0, 0); !s.ok()) {
    return fail("bind_vertex_buffer: " + s.message);
  }
  if (auto s = cmd->bind_index_buffer(ib->id(), res::IndexFormat::UInt32, 0); !s.ok()) {
    return fail("bind_index_buffer: " + s.message);
  }

  res::DrawArgs da{};
  da.index_count = draw_indices;
  da.instance_count = 1;
  if (auto s = cmd->draw_indexed(da); !s.ok()) return fail("draw_indexed: " + s.message);
  if (auto s = cmd->end_pass(); !s.ok()) return fail("end_pass: " + s.message);

  // 真正下发 vkCmdDraw*：录得再对，不提交也不出图。
  if (auto s = device->submit_command_list(*cmd); !s.ok()) {
    return fail("提交命令列表失败: " + s.message);
  }
  return true;
}

bool SceneRenderer::render_frame(float qx, float qy, float qz, float qw, float scale, float pan_x,
                                 float pan_y, std::string* err) {
  auto& self = *impl_;
  std::lock_guard<std::mutex> lk(self.mutex);
  auto fail = [&](const std::string& m) {
    if (err) *err = m;
    self.last_error = m;
    return false;
  };

  if (!self.device) return fail("render_frame: 设备未初始化");
  if (!self.swapchain) return fail("render_frame: 没有 Surface");

  // 1) 取下一张可写 image（无回读）
  auto ar = self.swapchain->acquire_next_image();
  if (ar.status == dev::ISwapchain::ImageStatus::OutOfDate) {
    // surface 尺寸变了：按当前 extent 重建后重试一次
    auto rb = self.swapchain->rebuild(self.swapchain->extent());
    if (!rb.ok()) return fail("交换链重建失败: " + rb.message);
    ar = self.swapchain->acquire_next_image();
  }
  if (ar.status != dev::ISwapchain::ImageStatus::Ok || !ar.target) {
    return fail("acquire 失败: " + ar.message);
  }

  // 2) 录进这张 image 并提交
  if (!self.record_into(ar.target, qx, qy, qz, qw, scale, pan_x, pan_y, err)) return false;

  // 3) 呈现到 Surface（无回读）
  const auto ps = self.swapchain->present_current();
  if (ps == dev::ISwapchain::ImageStatus::OutOfDate) {
    auto rb = self.swapchain->rebuild(self.swapchain->extent());
    if (!rb.ok()) return fail("present 后重建失败: " + rb.message);
    return true;  // 这帧丢了，下一帧就好
  }
  if (ps != dev::ISwapchain::ImageStatus::Ok) {
    return fail("present 失败(status=" + std::to_string(static_cast<int>(ps)) + ")");
  }

  ++self.frame_count;
  self.last_error.clear();
  return true;
}

// ---------------------------------------------------------------- 预设模型

bool SceneRenderer::load_model(const std::string& path, const std::string& name,
                               std::string* err) {
  auto& self = *impl_;
  std::lock_guard<std::mutex> lk(self.mutex);
  auto fail = [&](const std::string& m) {
    if (err) *err = m;
    self.last_error = m;
    return false;
  };

  if (!self.device) return fail("load_model: 设备未初始化");
  if (path.empty()) return fail("load_model: 路径为空");

  // 上传要落库的分配器页：Vertex / Index / Staging 各预取一页（L1 既定契约）。
  res::IMemoryAllocator* alloc = self.device->memory_allocator();
  for (res::MemoryKind kind :
       {res::MemoryKind::Vertex, res::MemoryKind::Index, res::MemoryKind::Staging}) {
    if (auto s = alloc->reserve_pages(kind, 1); !s.ok()) {
      return fail("load_model: 预取内存页失败: " + s.message);
    }
  }

  // 归一化口径与 L3 预设目录一致（包围盒对角线 → 1.0）。
  // 失败时**不**改动现有 model：用户点了个坏文件，画面应该保持原样。
  auto loaded = prim::load_and_upload_model(*self.device, path, name.empty() ? path : name,
                                            prim::ModelNormalize{});
  if (!loaded.ok()) {
    return fail("load_model(" + name + ") 失败: " + loaded.message());
  }
  self.model = loaded.take();
  self.model_name = name;
  self.last_error.clear();
  return true;
}

void SceneRenderer::clear_model() {
  auto& self = *impl_;
  std::lock_guard<std::mutex> lk(self.mutex);
  self.model.reset();
  self.model_name.clear();
}

// ---------------------------------------------------------------- 查询

void SceneRenderer::set_msaa_samples(std::uint32_t n) {
  std::lock_guard<std::mutex> lk(impl_->mutex);
  impl_->msaa_samples = n == 0 ? 1u : n;
}

std::uint32_t SceneRenderer::effective_msaa_samples() const {
  std::lock_guard<std::mutex> lk(impl_->mutex);
  return impl_->effective_msaa_samples;
}

std::vector<std::uint32_t> SceneRenderer::supported_msaa_levels() const {
  std::lock_guard<std::mutex> lk(impl_->mutex);
  const auto& self = *impl_;
  // 直接转发设备 caps：这是驱动 vkGetPhysicalDeviceImageFormatProperties
  // 查出来的真实结果，不是猜的。UI 拿它来循环切换，就不会点到不支持的档位。
  if (!self.device) return {1u};
  const auto& lv = self.device->caps().supported_msaa_levels;
  if (lv.empty()) return {1u};  // 老后端没填这个字段时至少保证 1x 可用
  return lv;
}

std::string SceneRenderer::status_text() const {
  std::lock_guard<std::mutex> lk(impl_->mutex);
  const auto& self = *impl_;
  std::string s = "frames=" + std::to_string(self.frame_count);
  s += " size=" + std::to_string(self.width) + "x" + std::to_string(self.height);
  s += self.swapchain ? " surface=ok" : " surface=none";
  s += " rot=" + std::to_string(self.rotation_deg);  // 调试用：验证手势轴向时要看它
  s += " model=" + (self.model ? self.model_name : std::string("cube"));
  if (!self.last_error.empty()) s += " last_error=" + self.last_error;
  return s;
}

}  // namespace magicblocks::android
