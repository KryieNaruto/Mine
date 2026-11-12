// MagicBlocks —— Android 场景渲染器实现
#include "scene_renderer.h"
#include <android/log.h>

#include <cmath>
#include <cstring>
#include <vector>

#if defined(MB_ANDROID_USE_NDK)
#include <android/native_window.h>
#endif

// 后端内部头：按交换链附件形状建管线需要 AttachmentShape 与 VulkanRenderDevice。
// 这是 backend/ 之外唯一 include 它们的文件（src/android/），符合 G3
// —— 它不写任何 Vk* 类型，只用 AttachmentShape/Format 这类 device 层值类型。
#include "backend/vulkan/vulkan_render_device.h"
#include "backend/vulkan/vulkan_swapchain.h"

#include "magicblocks_shaders.h"

// 模型网格的顶点定义（48 字节/顶点）。模型管线必须按它的真实布局去读，
// 不能沿用内置立方体那份 36 字节布局 —— 详见 model_pipeline_desc()。
#include "magicblocks/primitives/mesh.h"

namespace magicblocks::android {

namespace {
constexpr float kPi = 3.14159265358979323846f;
static_assert(sizeof(PushConstants) == 128, "push constant 必须是 128 字节");

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

// 两条管线描述**故意不放进**上面的匿名 namespace：它们的布局契约必须能被测试
// 断言（声明在 scene_renderer.h，测试在
// tests/magicblocks_android_scene_renderer_test.cpp）。"36 字节步长去读 48 字节
// 数据"这类错误编译期完全看不出来，跑起来也不报错，只能靠断言钉住。
}  // namespace

res::PipelineDesc cube_pipeline_desc(res::u32 samples, bool with_depth) {
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
  // 【深度状态必须与 render pass 的附件一致】
  //
  // 交换链图像本身不带深度附件（VkSwapchainCreateInfoKHR 不允许声明），但
  // VulkanSwapchain 现在会**给每张交换链图另建一张深度图**
  // （见 vulkan_swapchain.cpp 的 create_swapchain_objects + depth_images_），
  // 所以上屏路径的 pass 是有深度附件的。管线必须跟随打开深度测试/写入，
  // 否则"管线带/不带深度附件"与 pass 不一致 → Vulkan 把绘制命令**静默丢弃**：
  // 不报错、不崩溃，屏幕只剩清屏色（OBJ 模型"深度错乱/看不见"就是这个症状）。
  //
  // 三处必须同口径，现已全部到位：
  //   1) VulkanSwapchain 为每张图建深度图，并暴露 has_depth() 探针；
  //   2) rebuild_pipeline_for_swapchain 里 shape.has_depth = swapchain_->has_depth()；
  //   3) 这里让 depthTest/depthWrite 跟随同一个 has_depth 真值。
  // 只改其中一处的症状分别是"静默不画"和"面互相穿透/深度错乱"。
  desc.depth.depthTest = with_depth;
  desc.depth.depthWrite = with_depth;
  desc.depth.depthOp = res::CompareOp::Less;
  // 背面剔除关闭（按需求显式关掉，不依赖 RasterState 的默认值）。
  //
  // 注意：这不是性能开关，反而是**多一点**开销（背面三角形要进光栅化再被
  // 深度测试挡掉）。OBJ 模型的三角形绕序来自第三方导出器，不保证与引擎的
  // FrontFace::CounterClockwise 一致；开背面剔除时，绕序反了的模型会整片消失。
  // 关掉它可以让任意绕序的模型都正常显示，代价由深度测试兜底。
  desc.raster.cull = res::CullMode::None;
  desc.push_constant_bytes = static_cast<res::u32>(sizeof(PushConstants));
  // 管线的采样数**必须**与交换链 render pass 的采样数逐项相同,否则
  // create_pipeline_for_shape 会直接拒绝(形状 samples=4 与 desc samples=1 不一致)。
  // 以前这里漏了赋值 → 恒为 1 → 开 ×4 必然建管线失败。
  desc.samples = samples;
  return desc;
}

// 模型专用管线：顶点布局必须与 primitives::mesh_vertex_layout() 逐字节一致。
//
// 这里曾经直接复用 cube_pipeline_desc()，而内置立方体的 Vertex 是 36 字节
// （pos3 + nrm3 + col3 全 float），模型网格的 Vertex 是 **48 字节**
// （pos3 + pad / nrm3 + pad / color ubyte4norm + uv2 + pad，见 mesh.h）。
// 用 36 字节的 stride 去读 48 字节的数据，第二个顶点就起在错位的字节上，
// 于是兔子的每个三角形都散成互不相连的碎片 —— 就是"三角形爆炸乱飞"。
//
// 颜色属性也被换了格式：模型是 RGBA8 归一化（location 2），不是 vec3。
// 模型没有颜色语义（loader 填白），着色器按法线明暗成灰白即可，但**格式和
// 偏移必须说对**，否则同样会读到邻居顶点的字节。
res::PipelineDesc model_pipeline_desc(res::u32 samples, bool with_depth) {
  res::PipelineDesc desc = cube_pipeline_desc(samples, with_depth);

  desc.vertexLayout.bindings.clear();
  desc.vertexLayout.attributes.clear();

  res::VertexInputBinding binding{};
  binding.binding = 0;
  binding.stride = static_cast<res::u32>(sizeof(primitives::Vertex));
  binding.instanced = false;

  res::VertexAttribute pos{};
  pos.location = 0;
  pos.binding = 0;
  pos.format = res::VertexFormat::Float3;
  pos.offset = static_cast<res::u32>(offsetof(primitives::Vertex, position));

  res::VertexAttribute nrm{};
  nrm.location = 1;
  nrm.binding = 0;
  nrm.format = res::VertexFormat::Float3;
  nrm.offset = static_cast<res::u32>(offsetof(primitives::Vertex, normal));

  res::VertexAttribute col{};
  col.location = 2;
  col.binding = 0;
  col.format = res::VertexFormat::UByte4Norm;
  col.offset = static_cast<res::u32>(offsetof(primitives::Vertex, color));

  desc.vertexLayout.bindings.push_back(binding);
  desc.vertexLayout.attributes.push_back(pos);
  desc.vertexLayout.attributes.push_back(nrm);
  desc.vertexLayout.attributes.push_back(col);
  return desc;
}

SceneRenderer::~SceneRenderer() {
  detach_surface();
  device_.reset();
}

bool SceneRenderer::init(std::string* err) {
  auto fail = [&](const std::string& m) {
    if (err) *err = m;
    last_error_ = m;
    return false;
  };

  dev::DeviceDesc dd;
  dd.app_name = "MagicBlocksPad";
  dd.enable_validation = false;
  // pad 上是真实的 Mali GPU —— 不能用软件光栅筛选。
  dd.require_software_rasterizer = false;

  auto dr = dev::create_render_device(dd);
  if (!dr.ok()) return fail("创建设备失败: " + dr.message());
  device_ = dr.take();

  return create_resources(err);
}

bool SceneRenderer::create_resources(std::string* err) {
  auto fail = [&](const std::string& m) {
    if (err) *err = m;
    last_error_ = m;
    return false;
  };

  const std::vector<Vertex> verts = build_vertices();
  const std::vector<std::uint32_t> inds = build_indices();

  // 分配器的桶不会自动长页，必须显式预取（L1 既定契约）。
  res::IMemoryAllocator* alloc = device_->memory_allocator();
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
  auto vb = device_->create_buffer(vbd);
  if (!vb.ok()) return fail("创建顶点缓冲失败: " + vb.message());
  vertex_buffer_ = vb.take();
  if (auto s = vertex_buffer_->upload(verts.data(), vbd.size); !s.ok()) {
    return fail("上传顶点失败: " + s.message);
  }
  vertex_buffer_->flush_pending();  // upload() 只是入队，必须 flush

  // 索引缓冲
  res::BufferDesc ibd;
  ibd.size = inds.size() * sizeof(std::uint32_t);
  ibd.usage = res::BufferUsage::Index;
  auto ib = device_->create_buffer(ibd);
  if (!ib.ok()) return fail("创建索引缓冲失败: " + ib.message());
  index_buffer_ = ib.take();
  if (auto s = index_buffer_->upload(inds.data(), ibd.size); !s.ok()) {
    return fail("上传索引失败: " + s.message);
  }
  index_count_ = static_cast<std::uint32_t>(inds.size());
  index_buffer_->flush_pending();

  // 管线(attach 之前的初始管线永远是 1x:此时还没有交换链)
  auto pr = device_->create_pipeline(cube_pipeline_desc(1, false));
  if (!pr.ok()) return fail("创建管线失败: " + pr.message());
  pipeline_ = pr.take();
  pipeline_hash_ = pipeline_->hash();

  return true;
}

void SceneRenderer::set_render_scale(float scale) {
  // 夹到 [0.25, 1.0]：低于 25% 画面糊到无法判断内容；高于 100% 没有意义
  // （交换链生成大于屏幕的图只是白花带宽）。
  if (!(scale > 0.0f)) scale = 1.0f;  // 同时挡住 NaN
  if (scale < 0.25f) scale = 0.25f;
  if (scale > 1.0f) scale = 1.0f;
  render_scale_ = scale;
}

bool SceneRenderer::attach_surface(void* window, int width, int height, std::string* err) {
  std::lock_guard<std::mutex> lk(mutex_);
  auto fail = [&](const std::string& m) {
    if (err) *err = m;
    last_error_ = m;
    return false;
  };

  if (!device_) return fail("attach_surface: 设备未初始化");
  if (!window) return fail("attach_surface: ANativeWindow 为空");
  if (width <= 0 || height <= 0) return fail("attach_surface: 尺寸非法");

  swapchain_.reset();
  release_window();

  dev::NativeSurface ns;
  ns.kind = dev::NativeSurface::Kind::Android;
  ns.a = window;
  ns.b = 0;

  // 走带采样的入口：set_requested_msaa 必须先于 init()，而公共面
  // create_swapchain() 不带采样参数。MSAA 不可用时后端会退回 1x 并打日志，
  // 不会让这里失败 —— 所以只有一条创建路径，别写成"先建一条再换一条"，
  // 那会白建一条交换链（连同它下面的 surface 与图）。
  //
  // 深度：swapchain image 自己不可能带深度，但玩法（涂鸦挤出体、以后的积木/
  // 叠叠乐）都是**实体**，没有深度就是穿帮。这里为每张交换链图另建一份深度图，
  // 建不出来时后端会退回"无深度"，我们用 swapchain_has_depth_ 问出真实结果，
  // 并让玩法管线的深度开关与之同口径（否则 vkCmdDraw 被静默丢弃）。
  // 上屏分辨率缩放：交换链按比例建小图，合成器负责放大铺满。
  //
  // 宽高比必须用**原始 surface 尺寸**算（下面 aspect 用的是 width_/height_），
  // 所以这里先记下 surface 的原尺寸，再用缩放后的尺寸建交换链。两者分开存：
  //   surface_width_/height_  → 宽高比、触摸坐标映射（永远用真实屏幕尺寸）
  //   width_/height_          → 交换链像素尺寸、viewport/scissor
  // 混用会让缩放后画面变形或触摸错位。
  surface_width_ = width;
  surface_height_ = height;
  const int sc_w = std::max(1, static_cast<int>(std::lround(width * render_scale_)));
  const int sc_h = std::max(1, static_cast<int>(std::lround(height * render_scale_)));

  const dev::Extent2D sc_extent{static_cast<dev::u32>(sc_w), static_cast<dev::u32>(sc_h)};
  auto* vkdev_sc = dynamic_cast<backend::VulkanRenderDevice*>(device_.get());
  dev::Result<std::unique_ptr<dev::ISwapchain>> sc =
      (vkdev_sc != nullptr)
          ? vkdev_sc->create_swapchain_with_msaa(ns, sc_extent, msaa_samples_, true)
          : device_->create_swapchain(ns, sc_extent);
  if (!sc.ok()) return fail("创建交换链失败: " + sc.message());
  swapchain_ = sc.take();
  native_window_ = window;  // 引用由 JNI 层产生，归本对象持有

  // 实际生效的采样数：设备不支持时后端会退回 1x（并已打日志）。
  // 后面建管线必须用**这个**值，否则管线按 4 采样建、pass 按 1 采样开，
  // vkCmdDraw 会被静默丢弃 —— 屏幕上只剩清屏色，且没有任何报错。
  effective_msaa_samples_ = 1;
  swapchain_has_depth_ = false;
  if (auto* vsc_samples = dynamic_cast<backend::VulkanSwapchain*>(swapchain_.get())) {
    effective_msaa_samples_ = vsc_samples->msaa_samples();
    swapchain_has_depth_ = vsc_samples->has_depth();
  }

  width_ = sc_w;
  height_ = sc_h;
  frame_count_ = 0;

  // 记录屏幕旋转：把 VkSurfaceTransformFlagBitsKHR 转成角度。
  // 这台 pad 是 ROTATE_90 —— 见头文件里对 width_/height_ 与手势方向的说明。
  pre_transform_ = 0;
  rotation_deg_ = 0;
  if (auto* vsc = dynamic_cast<backend::VulkanSwapchain*>(swapchain_.get())) {
    const std::uint32_t pt = vsc->pre_transform();
    pre_transform_ = pt;
    if (pt & VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR) rotation_deg_ = 90;
    else if (pt & VK_SURFACE_TRANSFORM_ROTATE_180_BIT_KHR) rotation_deg_ = 180;
    else if (pt & VK_SURFACE_TRANSFORM_ROTATE_270_BIT_KHR) rotation_deg_ = 270;
  }

  // 管线必须按**交换链真实颜色格式**重建。
  // 理由：VkPipeline 绑定的 VkRenderPass 与 begin_pass 用的 render pass
  // 必须逐项兼容（格式/采样/深度）。create_resources() 里建的是离屏
  // RGBA8 形状，而 pad 的交换链可能是 RGBA8 也可能是 BGRA8，
  // 不匹配时 vkCmdDraw 被**静默丢弃** —— 屏幕上只剩清屏色，不报错。
  if (!rebuild_pipeline_for_swapchain(err)) return false;

  last_error_.clear();
  return true;
}

bool SceneRenderer::rebuild_pipeline_for_swapchain(std::string* err) {
  auto fail = [&](const std::string& m) {
    if (err) *err = m;
    last_error_ = m;
    return false;
  };

  auto* vkdev = dynamic_cast<backend::VulkanRenderDevice*>(device_.get());
  if (vkdev == nullptr) return fail("交换链渲染需要 VulkanRenderDevice");

  const device::Format fmt = swapchain_->format();
  backend::AttachmentShape shape{};
  shape.color_format = fmt;
  // 【深度开关必须与管线、与目标三者同口径】
  //
  // 交换链图像本身没有深度附件（VkSwapchainCreateInfoKHR 不允许声明），
  // 本工程的 VulkanSwapchain 现在会给每张图另建一份深度图（set_requested_depth），
  // 所以这里按**真实结果**填 has_depth —— 建深度图失败时后端会退回"无深度"，
  // 而 cube 管线的 depthTest 本来就是 false，两者仍然一致。
  //
  // 三者只要有一处不一致，管线与 pass 就不兼容，vkCmdDraw 被静默丢弃。
  shape.has_depth = swapchain_has_depth_;
  shape.depth_format = device::Format::D16_UNORM;
  // 必须用 **实际生效** 的采样数，且与交换链建 MS 图时的值一致。
  // 这里曾经硬编码 1，导致 MS 图建好却没任何管线/附件用它 —— 开了 ×4 等于没开。
  shape.samples = effective_msaa_samples_;
  // 2026-11-11（t6）：AttachmentShape 现在**带** from_swapchain，且必须置真。
  // 以前注释写着"没有这个字段，这个差异由设备层自行判定"——那句话是错的：
  // 设备层只在**回退分支**（自己 create_pass）里才有 from_swapchain，
  // 一旦复用管线自带 pass，管线 pass 的 finalLayout 恒为 GENERAL →
  // vkQueuePresentKHR 报 E-01430（must be in PRESENT_SRC_KHR but is in GENERAL），
  // 上屏帧被丢掉。现在两边同口径，pass_compatible() 也会比较它。
  shape.from_swapchain = true;

  auto pr = vkdev->create_pipeline_for_shape(cube_pipeline_desc(effective_msaa_samples_, swapchain_has_depth_), shape);
  if (!pr.ok()) return fail("按交换链形状建管线失败: " + pr.message());
  pipeline_ = pr.take();
  pipeline_hash_ = pipeline_->hash();

  // 模型管线同形状、同采样数，只换顶点输入布局。它必须跟立方体管线一起
  // 重建 —— 采样数一变，旧的模型管线就与 render pass 不兼容了。
  auto mp = vkdev->create_pipeline_for_shape(model_pipeline_desc(effective_msaa_samples_, swapchain_has_depth_), shape);
  if (!mp.ok()) return fail("按交换链形状建模型管线失败: " + mp.message());
  model_pipeline_ = mp.take();
  model_pipeline_hash_ = model_pipeline_->hash();

  // 玩法管线：同一套附件形状（格式/采样/深度/from_swapchain 必须逐项相同），
  // 只有顶点与实例布局不同（48B 顶点 + 112B 实例属性）。
  // 它必须在这里一起重建 —— 换 MSAA 档位等于换了 render pass，旧管线立刻不兼容。
  auto pp = vkdev->create_pipeline_for_shape(
      SketchSession::pipeline_desc(effective_msaa_samples_, swapchain_has_depth_), shape);
  if (!pp.ok()) return fail("按交换链形状建玩法管线失败: " + pp.message());
  play_pipeline_ = pp.take();
  play_pipeline_hash_ = play_pipeline_->hash();
  return true;
}

void SceneRenderer::release_window() {
#if defined(MB_ANDROID_USE_NDK)
  if (native_window_) {
    ANativeWindow_release(reinterpret_cast<ANativeWindow*>(native_window_));
    native_window_ = nullptr;
  }
#else
  native_window_ = nullptr;
#endif
}

void SceneRenderer::detach_surface() {
  std::lock_guard<std::mutex> lk(mutex_);
  swapchain_.reset();  // 必须先于 device 销毁
  release_window();
  // 交换链没了 → 下次拿到的 backbuffer 内容**未定义**，不能再依赖
  // "上一帧画的还在里面"。作废全部记录，逼下一帧整屏 clear。
  for (auto& p : last_pose_) p.valid = false;
}

bool SceneRenderer::content_unchanged(float qx, float qy, float qz, float qw, float scale,
                                      float pan_x, float pan_y) {
  // 按本帧 acquire 到的那张图取记录 —— 见 kMaxTrackedImages 的注释。
  const std::uint32_t key = swapchain_ ? swapchain_->current_image_key() : 0u;
  if (key >= kMaxTrackedImages) {
    // 图太多，追不动：本帧强制 clear，且不记状态（下次仍然强制 clear）。
    last_image_key_ = key;
    return false;
  }
  last_image_key_ = key;
  LastPose& p = last_pose_[key];
  const bool same = p.valid && p.qx == qx && p.qy == qy && p.qz == qz && p.qw == qw &&
                    p.scale == scale && p.pan_x == pan_x && p.pan_y == pan_y &&
                    p.play_revision == play_revision_ &&
                    p.width == width_ && p.height == height_ &&
                    p.samples == effective_msaa_samples_ && p.play_mode == play_mode_;
  p.qx = qx;
  p.qy = qy;
  p.qz = qz;
  p.qw = qw;
  p.scale = scale;
  p.pan_x = pan_x;
  p.pan_y = pan_y;
  p.play_revision = play_revision_;
  p.width = width_;
  p.height = height_;
  p.samples = effective_msaa_samples_;
  p.play_mode = play_mode_;
  p.valid = true;
  return same;
}

bool SceneRenderer::record_into(dev::IRenderTarget* target, float qx, float qy, float qz,
                                float qw, float scale, float pan_x, float pan_y,
                                std::string* err) {
  auto fail = [&](const std::string& m) {
    if (err) *err = m;
    last_error_ = m;
    return false;
  };

  auto cr = device_->create_command_list();
  if (!cr.ok()) return fail("创建命令列表失败: " + cr.message());
  res::ICommandList* cmd = cr.value().get();

  // ==========================================================================
  // 相机：**自由四元数**（Kotlin 侧每帧右乘增量进来）
  // ==========================================================================
  //
  // 三个硬要求必须同时满足：
  //   1. 左右拖 → 绕**当前相机自己的 up** 转（不是世界 Y）
  //   2. 上下拖 → 绕**当前相机自己的 right** 转
  //   3. 360° 无死角，**不能有任何角度夹取**
  //
  // 欧拉角做不到：它把“水平”写死成世界 Y（违反 1），且
  // 俯仰过 ±90° 会万向节锁死（违反 3）。
  //
  // 只有四元数能全部满足：四元数乘法里 **右乘 = 绕机体轴**，
  // 而“机体轴”的定义就是相机此刻的 right / up。Kotlin 侧每帧做
  //
  //       q ← q * Rx(Δpitch) * Ry(Δyaw)
  //
  // 于是第 1、2 条自动成立（完全不需要“取轴”这个动作），
  // 四元数没有奇点所以第 3 条也成立。
  //
  // 这里只把四个数还原成旋转矩阵并取列向量：
  //
  //     R = | 1-2(y²+z²)   2(xy-wz)     2(xz+wy)   |
  //         | 2(xy+wz)     1-2(x²+z²)   2(yz-wx)   |
  //         | 2(xz-wy)     2(yz+wx)     1-2(x²+y²) |
  //
  //     列 0 = right、列 1 = up、列 2 = back（相机看向 -back）
  //
  // 环绕关系：eye = center + dist * back，看向 center，up 用 R 的列 1。
  // 列 1 与列 2 天然正交，所以 make_look_at 不会退化——前提是
  // 四元数保持单位长度（Kotlin 每次右乘后都归一化）。
  const float dist = 2.6f / (scale > 0.05f ? scale : 0.05f);

  // ==========================================================================
  // 平移 == **纯视口平移**：视点恒在世界原点，只有眼睛在动
  // ==========================================================================
  //
  // 以前的写法是
  //     orbit_center = (pan_x, pan_y, 0)
  //     eye = orbit_center + dist * back
  // 即"把环绕中心一起搬走"。后果：双指一平移，物体的世界坐标没变、但相机
  // 绕着**新中心**转，于是"旋转视点"跟着平移跑了 —— 用户要的是
  // "只挪视口，旋转仍然绕着原点 (0,0,0)"。
  //
  // 现在视点恒为原点，平移只作用在 eye 上：
  //     eye = dist * back + pan_x * right + pan_y * up
  // 这样相机仍在看原点（target 永远是 (0,0,0)），只是站的位置偏了，效果就是
  // 整个画面在视口里平移 —— 和"移动取景框"而不是"搬走物体"。
  //
  // 用相机自身的 right / up（R 的列 0 / 列 1）而不是世界 X/Y：否则相机转到
  // 侧面后，"往右拖"会把画面往世界 +X 推，看起来就是平移方向不跟手。
  const prim::Vec3 orbit_center{0.f, 0.f, 0.f};

  const float qn = std::sqrt(qx * qx + qy * qy + qz * qz + qw * qw);
  const float iq = qn > 1e-6f ? 1.f / qn : 1.f;
  const float ux = qx * iq, uy = qy * iq, uz = qz * iq, uw = qw * iq;

  // R 的三列 = 机体轴在世界里的方向（列向量约定，与下面 up/back 的写法同源）：
  //   列 0 = right、列 1 = up、列 2 = back（相机看向 -back）
  const prim::Vec3 right{1.f - 2.f * (uy * uy + uz * uz), 2.f * (ux * uy + uw * uz),
                         2.f * (ux * uz - uw * uy)};
  const prim::Vec3 up{2.f * (ux * uy - uw * uz), 1.f - 2.f * (ux * ux + uz * uz),
                      2.f * (uy * uz + uw * ux)};
  const prim::Vec3 back{2.f * (ux * uz + uw * uy), 2.f * (uy * uz - uw * ux),
                        1.f - 2.f * (ux * ux + uy * uy)};

  const prim::Vec3 eye{
      dist * back.x + pan_x * right.x + pan_y * up.x,
      dist * back.y + pan_x * right.y + pan_y * up.y,
      dist * back.z + pan_x * right.z + pan_y * up.z,
  };
  const prim::Mat4 view = prim::make_look_at(eye, orbit_center, up);

  // 投影宽高比必须用**屏幕上实际呈现**的尺寸，而不是 swapchain image 的尺寸。
  //
  // 这台 pad 是横屏应用但面板物理竖置（ROTATE_90）。swapchain image 是
  // 2160x1440（未旋转），SurfaceFlinger 合成时把它旋转 90° 铺满屏幕，
  // 屏幕上看到的是 1440x2160 —— 两个方向的像素密度差 1.5 倍。
  //
  // 若直接用 image 的 2160/1440 = 1.5 当 aspect，画出来的正方体会被横向拉长
  // 1.5 倍（实测 891x396，宽高比 2.25 = 1.5 x 1.5），看起来就是个长方体。
  // 遇到 90/270 旋转时取倒数，让投影在最终屏幕朝向下正确。
  // 【2026-11-11 修】这张 cube / 模型的画面被拉成"竖直长条"的根因就在这里。
  //
  // 上面写的是对的：内容画在 2160x1440 的 image 里，合成器再整体转 90° 上屏，
  // 所以**投影必须按 image 自己的宽高比 1.5**，旋转由合成器负责。
  // 原代码在 rot90 时取倒数 → 用 0.667 做投影，x 方向被压 2.25 倍。
  // 合成器又把它转回横屏铺满，结果是：竖直方向占满、水平方向只剩 1/2.25
  // —— 屏幕上就是一根又高又窄的竖条（实测 891 宽 x 890 高那块颜色团）。
  //
  // 曾经这里的原因写的是"物理竖置面板，两个方向像素密度差 1.5 倍"，
  // 那个推理不成立：旋转是**刚性**变换，不改像素密度。
  // ⚠️ 宽高比必须用 **surface 真实尺寸**，不能用 width_/height_（那是缩放后的
  // 交换链像素尺寸）。两者在 render_scale=1 时相同；一旦降分辨率，用缩放后的
  // 尺寸算 aspect 会得到同样的比值（等比缩放不改比值），但取整误差会被放大，
  // 且语义上也不对 —— 反正真实尺寸就在这里，没有理由用它。
  const int aw = surface_width_ > 0 ? surface_width_ : width_;
  const int ah = surface_height_ > 0 ? surface_height_ : height_;
  const float aspect = static_cast<float>(aw) / static_cast<float>(ah);
  const prim::Mat4 proj = prim::make_perspective(60.f * kPi / 180.f, aspect, 0.1f, 100.f);
  const prim::Mat4 view_proj = proj * view;

  // 模型矩阵：**恒等**。物体待在世界原点不动，旋转全部由上面的相机实现。
  //
  // 这里曾经是 `world = rot_yaw * rot_pitch`（转物体）。改掉的两个理由：
  //   1. 转物体时两次旋转不交换 —— pitch 一大幅度，yaw 的表现就不再是干净的
  //      水平转，横竖两个方向互相污染；
  //   2. 用户要的是"摄像机检视模型"，不是"把模型拎起来转"：转相机时拖动方向
  //      恒等于画面转动方向，这才是看模型该有的手感。
  //
  // 别把 yaw/pitch 又搬回这里 —— 那会让轴向依赖重新分散到两个文件里。
  // OBJ 加载时已经做过归一化（ModelNormalize），模型本身就落在原点附近、
  // 尺寸约等于单位球，所以恒等矩阵下的取景与内置立方体一致。
  const prim::Mat4 world = prim::Mat4::identity();

  PushConstants pc{};
  std::memcpy(pc.view_proj, view_proj.m, sizeof(pc.view_proj));
  std::memcpy(pc.world, world.m, sizeof(pc.world));

  // ── 跳过整屏清除（性能优化 ①）────────────────────────────────────────
  //
  // 2160x1440 @ MSAA4 的整屏 clear 是一趟 ~1240 万像素的写带宽，实测就是
  // 这个场景 15.5ms/帧里的大头。画面**没变化**时这一趟是纯浪费：交换链图
  // 里本来就还是上一帧画好的内容，LOAD 而不是 CLEAR 就能得到同样的一帧。
  //
  // 判据（全部满足才跳过）：
  //   * 构图没变：型号 / 玩法 / MSAA / 上屏分辨率 / 目标尺寸 都相同
  //   * 姿态没变：七个相机参数逐位相同
  //   * 玩法画面没变：sketch 的笔画几何版本号相同（见 sketch_revision_）
  //
  // 注意 clear 标志**参与 render pass 兼容性比较**（AttachmentShape 有
  // clear_color/clear_depth），所以这里只能走"管线自带 pass 恒 CLEAR"之外的
  // 那条路：设备帧会在 pass_compatible 判定失败时**按本次形状新建 pass**。
  // 也就是说 clear=false 时不能复用管线自带 pass，必须让后端自建 —— 这正是
  // vulkan_device_frame.cpp 里已经实现好的分工，不需要额外改动。
  const bool static_frame = content_unchanged(qx, qy, qz, qw, scale, pan_x, pan_y);
  res::RenderPassDesc pass{};
  pass.target = target;
  pass.clear_color = !static_frame;
  pass.clear_color_rgba[0] = 0.06f;
  pass.clear_color_rgba[1] = 0.07f;
  pass.clear_color_rgba[2] = 0.10f;
  pass.clear_color_rgba[3] = 1.0f;
  pass.clear_depth = !static_frame;
  pass.clear_depth_value = 1.0f;

  if (auto s = cmd->begin_pass(pass); !s.ok()) return fail("begin_pass: " + s.message);

  res::Viewport vp{};
  vp.width = static_cast<float>(width_);
  vp.height = static_cast<float>(height_);
  vp.min_depth = 0.f;
  vp.max_depth = 1.f;
  if (auto s = cmd->set_viewport(vp); !s.ok()) return fail("set_viewport: " + s.message);

  res::ScissorRect sc{};
  sc.width = static_cast<dev::u32>(width_);
  sc.height = static_cast<dev::u32>(height_);
  if (auto s = cmd->set_scissor(sc); !s.ok()) return fail("set_scissor: " + s.message);

  // 记住本帧的姿态：触摸求交必须复用**同一个相机**，否则"手指点在 A、
  // 笔画出现在 B"。sync_session_camera 用与上面完全相同的公式重建相机。
  last_q_[0] = qx;
  last_q_[1] = qy;
  last_q_[2] = qz;
  last_q_[3] = qw;
  last_scale_ = scale;
  last_pan_[0] = pan_x;
  last_pan_[1] = pan_y;

  // ============================== 涂鸦玩法 ==============================
  //
  // 玩法激活时画的是**挤出实体**，不是立方体/模型：实体已在世界坐标里
  // （画布变换在 build 时烘焙进网格），世界矩阵恒等，靠 z 挤出方向朝观察者。
  //
  // 注意这里**不**清空模型状态：用户从涂鸦切回"无玩法"时原来的模型还在，
  // 不需要重新从列表里选一次。
  // 【空画布不进这个分支】
  //
  // 设备层有条护栏：一次提交里零个 draw → 直接判失败（"提交成功但没有任何
  // draw 被下发"）。这条护栏是对的（它挡的是"录了 pass 却什么都没画"这种
  // 静默缺陷），所以不能在那边放宽。
  //
  // 于是空画布（刚切进涂鸦 / 刚清空）必须回落到下面的立方体/模型分支：
  // 那一支至少画一个立方体，draw_calls > 0，护栏不触发。视觉上用户看到的是
  // 清屏色 + 立方体，而不是一块死黑的区域。
  if (play_mode_ == "sketch" && sketch_ && sketch_->initialized() &&
      play_pipeline_ != nullptr && sketch_->stroke_count() > 0) {
    // 文档若有改动，先在这里把它落成 GPU 网格。
    //
    // 必须在**渲染线程**上做：这里已经持着 mutex_、且还没 acquire 下一张图，
    // 慢一点最多只是这一帧晚到，不会像 UI 线程那样把已 acquire 的图像悬着。
    rebuild_sketch_mesh_locked();

    sketch_->set_camera_pose(qx, qy, qz, qw, scale, pan_x, pan_y, width_, height_);
    if (auto s = cmd->bind_pipeline(play_pipeline_.get(), play_pipeline_hash_); !s.ok()) {
      return fail("bind_pipeline(play): " + s.message);
    }
    if (!sketch_->record_draw(cmd, view_proj.m, play_pipeline_hash_, err)) return false;
    if (auto s = cmd->end_pass(); !s.ok()) return fail("end_pass: " + s.message);
    if (auto s = device_->submit_command_list(*cmd); !s.ok()) {
      return fail("提交命令列表失败: " + s.message);
    }
    return true;
  }

  // 涂鸦模式下但画布是空的：网格可能还是脏的，仍然在渲染线程上清一次，
  // 保证"清空画布"之后第一笔不会再看到旧网格。
  if (play_mode_ == "sketch" && sketch_ != nullptr) rebuild_sketch_mesh_locked();

  // 画哪一层：选了预设模型就画模型，否则画内置立方体。
  //
  // 两者**顶点布局不同**，所以管线也必须不同：
  //   内置立方体 : 36B/顶点 = pos3 + nrm3 + col3（全 float）
  //   模型网格   : 48B/顶点 = pos3+pad / nrm3+pad / RGBA8 + uv2 + pad
  // 以前这里写着"两者布局一致，共用同一条管线"—— 那句是错的，也是兔子
  // 三角形爆炸乱飞的直接原因：用 36B 的步长读 48B 的数据，从第二个顶点起
  // 每个字段都错位，三角形互不相连。
  //
  // 着色器同一个：模型顶点色是白色，片元按法线明暗成灰白，形状清晰可辨。
  bool drawing_model = false;
  const res::IBuffer* vb = vertex_buffer_.get();
  const res::IBuffer* ib = index_buffer_.get();
  std::uint32_t draw_indices = index_count_;
  if (model_ != nullptr) {
    const prim::IMesh* mesh = model_->mesh();
    if (mesh != nullptr && mesh->vertex_buffer() != nullptr && mesh->index_buffer() != nullptr &&
        model_pipeline_ != nullptr) {
      vb = mesh->vertex_buffer();
      ib = mesh->index_buffer();
      draw_indices = mesh->index_count();
      drawing_model = true;
    }
  }

  res::IPipeline* pipe = drawing_model ? model_pipeline_.get() : pipeline_.get();
  const std::uint64_t pipe_hash = drawing_model ? model_pipeline_hash_ : pipeline_hash_;
  if (auto s = cmd->bind_pipeline(pipe, pipe_hash); !s.ok()) {
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
  if (auto s = device_->submit_command_list(*cmd); !s.ok()) {
    return fail("提交命令列表失败: " + s.message);
  }
  return true;
}

bool SceneRenderer::render_frame(float qx, float qy, float qz, float qw, float scale,
                                 float pan_x, float pan_y, std::string* err) {
  std::lock_guard<std::mutex> lk(mutex_);
  auto fail = [&](const std::string& m) {
    if (err) *err = m;
    last_error_ = m;
    return false;
  };

  if (!device_) return fail("render_frame: 设备未初始化");
  if (!swapchain_) return fail("render_frame: 没有 Surface");

  // 1) 取下一张可写 image（无回读）
  auto ar = swapchain_->acquire_next_image();
  if (ar.status == dev::ISwapchain::ImageStatus::OutOfDate) {
    // surface 尺寸变了：按当前 extent 重建后重试一次
    auto rb = swapchain_->rebuild(swapchain_->extent());
    if (!rb.ok()) return fail("交换链重建失败: " + rb.message);
    ar = swapchain_->acquire_next_image();
  }
  if (ar.status != dev::ISwapchain::ImageStatus::Ok || !ar.target) {
    return fail("acquire 失败: " + ar.message);
  }

  // 2) 录进这张 image 并提交
  if (!record_into(ar.target, qx, qy, qz, qw, scale, pan_x, pan_y, err)) {
    // 【录制失败也必须把图还回去】
    //
    // 图已经 acquire 了。这里直接 return，那张 presentable image 就再也不回
    // BufferQueue —— 3~5 张图全被吞掉之后，vkAcquireNextImageKHR 永久超时，
    // 渲染线程每秒重试几百次、画面彻底冻住，而唯一可见的错误变成"acquire 超时"
    // （真正的原因被它盖掉了）。
    //
    // 必须用 present_without_submit 而不是 present_current：后者会等
    // render_finished 信号量，而本帧没提交过、那颗信号量永远不会被 signal，
    // present 会永久阻塞。详见 device/swapchain.h 里的接口注释。
    (void)swapchain_->present_without_submit();
    return false;
  }

  // 3) 呈现到 Surface（无回读）
  const auto ps = swapchain_->present_current();
  if (ps == dev::ISwapchain::ImageStatus::OutOfDate) {
    auto rb = swapchain_->rebuild(swapchain_->extent());
    if (!rb.ok()) return fail("present 后重建失败: " + rb.message);
    return true;  // 这帧丢了，下一帧就好
  }
  if (ps != dev::ISwapchain::ImageStatus::Ok) {
    return fail("present 失败(status=" + std::to_string(static_cast<int>(ps)) + ")");
  }

  ++frame_count_;
  last_error_.clear();
  return true;
}

// ---------------------------------------------------------------- 预设模型

bool SceneRenderer::load_model(const std::string& path, const std::string& name,
                               std::string* err) {
  std::lock_guard<std::mutex> lk(mutex_);
  auto fail = [&](const std::string& m) {
    if (err) *err = m;
    last_error_ = m;
    return false;
  };

  if (!device_) return fail("load_model: 设备未初始化");
  if (path.empty()) return fail("load_model: 路径为空");

  // 上传要落库的分配器页：Vertex / Index / Staging 各预取一页（L1 既定契约，
  // 与 create_resources 同口径）。缺页时 device 会返回 OOM 并给出明确数字，
  // 这里再补一次是保证"第一次点列表就能成功"，而不是要点两次。
  res::IMemoryAllocator* alloc = device_->memory_allocator();
  for (res::MemoryKind kind :
       {res::MemoryKind::Vertex, res::MemoryKind::Index, res::MemoryKind::Staging}) {
    if (auto s = alloc->reserve_pages(kind, 1); !s.ok()) {
      return fail("load_model: 预取内存页失败: " + s.message);
    }
  }

  // 归一化口径与 L3 预设目录一致（包围盒对角线 → 1.0）：pad 上列表里几个模型
  // 的原始尺寸差 1~2 个数量级，不归一化的话切到球体就只剩一个像素点。
  // 失败时**不**改动现有 model_：用户点了个坏文件，画面应该保持原样。
  auto loaded = prim::load_and_upload_model(*device_, path, name.empty() ? path : name,
                                            prim::ModelNormalize{});
  if (!loaded.ok()) {
    return fail("load_model(" + name + ") 失败: " + loaded.message());
  }
  model_ = loaded.take();
  model_name_ = name;
  last_error_.clear();
  return true;
}

void SceneRenderer::clear_model() {
  std::lock_guard<std::mutex> lk(mutex_);
  model_.reset();
  model_name_.clear();
}

std::vector<std::uint32_t> SceneRenderer::supported_msaa_levels() const {
  // 直接转发设备 caps：这是驱动 vkGetPhysicalDeviceImageFormatProperties
  // 查出来的真实结果，不是猜的。UI 拿它来循环切换，就不会点到不支持的档位。
  if (!device_) return {1u};
  const auto& lv = device_->caps().supported_msaa_levels;
  if (lv.empty()) return {1u};  // 老后端没填这个字段时至少保证 1x 可用
  return lv;
}

std::string SceneRenderer::status_text() const {
  std::string s = "frames=" + std::to_string(frame_count_);
  s += " size=" + std::to_string(width_) + "x" + std::to_string(height_);
  s += swapchain_ ? " surface=ok" : " surface=none";
  s += " rot=" + std::to_string(rotation_deg_);  // 调试用：验证手势轴向时要看它
  s += " model=" + (model_ ? model_name_ : std::string("cube"));
  s += " play=" + play_mode_;
  s += swapchain_has_depth_ ? " depth=on" : " depth=off";
  if (sketch_ && play_mode_ == "sketch") s += " " + sketch_->status_text();
  if (!last_error_.empty()) s += " last_error=" + last_error_;
  return s;
}

// ================================================================ 玩法

bool SceneRenderer::set_play_mode(const std::string& mode_id, std::string* err) {
  std::lock_guard<std::mutex> lk(mutex_);
  auto fail = [&](const std::string& m) {
    if (err) *err = m;
    last_error_ = m;
    return false;
  };

  // "none" 永远可用：它是"回到立方体/模型演示"。切回来时**不销毁**涂鸦会话
  // （笔画还在，切回去画面立即可见；销毁重建反而要重传几何）。
  if (mode_id == "none") {
    play_mode_ = "none";
    last_error_.clear();
    return true;
  }

  if (mode_id == "sketch") {
    if (!device_) return fail("set_play_mode: 设备未初始化");
    if (play_pipeline_ == nullptr) {
      return fail("set_play_mode: 玩法管线未就绪（Surface 还没 attach?）");
    }
    if (!sketch_) {
      // 画布参数：2.0 x 1.5 的世界平面，贴在 XY 上、法线朝 +z（正对默认视角），
      // 挤出厚度 0.08。与 l3_cli 的 sketch 子命令同一组默认值，CLI 出的图与
      // 屏幕上看到的是同一套几何口径。
      featsketch::CanvasParams cp;
      cp.width = 2.0f;
      cp.height = 1.5f;
      cp.extrude_thickness = 0.08f;
      cp.clamp_to_canvas = true;
      cp.min_spacing = 0.008f;         // 约 2 个像素的外接距离，去掉抖动重采样
      cp.max_points_per_stroke = 1024; // 上屏一笔不需要 4096 个点
      auto session = std::make_unique<SketchSession>();
      if (!session->init(device_.get(), cp, err)) return false;
      sketch_ = std::move(session);
    }
    play_mode_ = "sketch";
    last_error_.clear();
    return true;
  }

  // 其余玩法（积木 / 叠叠乐 / 印章）尚未在这条上屏路径上接线。
  // 明确失败，并且**不改变**当前玩法 —— 用户按下去发现没反应，
  // 比"切到了一个画不出东西的模式"更容易诊断。
  return fail("set_play_mode: 玩法 " + mode_id + " 尚未接入上屏渲染");
}

bool SceneRenderer::sketch_input(float px, float py, int kind, std::string* err) {
  std::lock_guard<std::mutex> lk(mutex_);
  auto fail = [&](const std::string& m) {
    if (err) *err = m;
    last_error_ = m;
    return false;
  };
  if (play_mode_ != "sketch" || !sketch_) return fail("sketch_input: 未处于涂鸦玩法");
  if (width_ <= 0 || height_ <= 0) return fail("sketch_input: 视口尺寸无效");

  // 屏幕像素 → 交换链图像像素。
  //
  // 视口是 swapchain image 的 width_ x height_，而触摸事件的坐标是**屏幕**
  // 尺寸；两者只在"图像与屏幕 1:1"时相等。这台 pad 恰好是这样（IDENTITY
  // preTransform、SurfaceView 铺满），所以这里是比例换算而不是恒等 ——
  // 换个设备/分屏时自动正确，不需要再改一处。
  //
  // 调用方（Kotlin）送进来的已经是像素坐标，比例由它按视口算好再乘，
  // 这里只做纵向翻转（见下）。为了让 C++ 侧不依赖 Kotlin 的视口认知，
  // Kotlin 送来的已经是 **image 像素**坐标，所以这里不做缩放。

  prim::StrokeEventKind k = prim::StrokeEventKind::Move;
  switch (kind) {
    case 0: k = prim::StrokeEventKind::Begin; break;
    case 1: k = prim::StrokeEventKind::Move; break;
    case 2: k = prim::StrokeEventKind::End; break;
    case 3: k = prim::StrokeEventKind::Cancel; break;
    default: return fail("sketch_input: 未知事件类型 " + std::to_string(kind));
  }

  // 相机必须与本帧画面一致：render_frame 每帧会在录制前刷新一次，
  // 但**第一帧还没渲染就按下**时也要有可用相机，所以这里无条件再同步一次
  // （纯数学，微秒级；比"猜测第一次渲染已经发生"稳）。
  sketch_->set_camera_pose(last_q_[0], last_q_[1], last_q_[2], last_q_[3], last_scale_,
                           last_pan_[0], last_pan_[1], width_, height_);

  const bool committed = sketch_->feed_screen(px, py, k);
  if (committed) {
    // 【只标记，不重建】GPU 网格留给渲染线程的下一帧（见 rebuild_sketch_mesh_locked）。
    //
    // 这里曾经是 `if (!sketch_->rebuild(err)) return false;` —— 直接在 UI 线程上
    // 持着 mutex_ 跑 build_meshes（全部笔画重算 + 建新缓冲 + 上传 + flush）。
    // 手势提交的瞬间，渲染线程正好在 acquire 与 submit 之间，于是它排队等锁；
    // 这一帧的交换链图像已经 acquire、却迟迟不 present，连续几帧后
    // BufferQueue 无可用缓冲 → "dequeueBuffer timed out (-38)" → 画面永久卡死
    // （内存里 frames 计数冻住、fps=0，而 UI 线程看起来一切正常）。
    sketch_dirty_ = true;
    ++play_revision_;  // 画面内容变了 —— 不能跳过整屏 clear
    last_error_.clear();
    return true;
  }
  last_error_.clear();
  return false;
}

bool SceneRenderer::sketch_undo(std::string* err) {
  std::lock_guard<std::mutex> lk(mutex_);
  auto fail = [&](const std::string& m) {
    if (err) *err = m;
    last_error_ = m;
    return false;
  };
  if (!sketch_) return fail("sketch_undo: 涂鸦会话未建立");
  if (!sketch_->undo()) return fail("sketch_undo: 没有可撤销的笔画");
  sketch_dirty_ = true;  // 同 sketch_input：GPU 重建交给渲染线程
  ++play_revision_;      // 画面内容变了 —— 不能跳过整屏 clear
  last_error_.clear();
  return true;
}

bool SceneRenderer::sketch_clear(std::string* err) {
  std::lock_guard<std::mutex> lk(mutex_);
  auto fail = [&](const std::string& m) {
    if (err) *err = m;
    last_error_ = m;
    return false;
  };
  if (!sketch_) return fail("sketch_clear: 涂鸦会话未建立");
  sketch_->clear();
  sketch_dirty_ = true;
  ++play_revision_;  // 画面内容变了 —— 不能跳过整屏 clear
  last_error_.clear();
  return true;
}

std::size_t SceneRenderer::sketch_stroke_count() {
  std::lock_guard<std::mutex> lk(mutex_);
  return sketch_ ? sketch_->stroke_count() : 0;
}

// ---------------------------------------------------------------- 整条折线提交
//
// 与 sketch_input 的分工：那条是手写时的逐点流，这条是"画布画完了整条交过来"。
// 两条最终都落到 SketchSession 的同一份文档上，所以 3D 侧不需要知道
// 笔迹是从哪条路径来的 —— 挤出、撤销、清空全都一致。

bool SceneRenderer::begin_push_strokes(std::string* err) {
  std::lock_guard<std::mutex> lk(mutex_);
  auto fail = [&](const std::string& m) {
    if (err) *err = m;
    last_error_ = m;
    return false;
  };
  if (play_mode_ != "sketch" || !sketch_) return fail("begin_push_strokes: 未处于涂鸦玩法");
  push_pending_ = true;
  last_error_.clear();
  return true;
}

bool SceneRenderer::push_stroke(const float* points, std::size_t point_count,
                                float width_ratio, std::string* err) {
  std::lock_guard<std::mutex> lk(mutex_);
  auto fail = [&](const std::string& m) {
    // 失败时把整批作废：半份文档比没有更糟 —— 画布上看得见、3D 里却少一笔，
    // 用户会以为是挤出算法坏了。
    push_pending_ = false;
    if (err) *err = m;
    last_error_ = m;
    return false;
  };
  if (!push_pending_) return fail("push_stroke: 未调用 begin_push_strokes");
  if (!sketch_) return fail("push_stroke: 涂鸦会话未建立");
  if (points == nullptr || point_count < 2) return fail("push_stroke: 点数不足 2");

  if (!sketch_->push_stroke(points, point_count, width_ratio, err)) {
    return fail(err && !err->empty() ? *err : "push_stroke: 折线被拒绝");
  }
  last_error_.clear();
  return true;
}

bool SceneRenderer::end_push_strokes(std::string* err) {
  std::lock_guard<std::mutex> lk(mutex_);
  auto fail = [&](const std::string& m) {
    push_pending_ = false;
    if (err) *err = m;
    last_error_ = m;
    return false;
  };
  if (!push_pending_) return fail("end_push_strokes: 未调用 begin_push_strokes");
  push_pending_ = false;
  // 同 sketch_input：只标记脏，网格重建留给渲染线程（否则会卡死 BufferQueue）。
  sketch_dirty_ = true;
  ++play_revision_;
  last_error_.clear();
  return true;
}

void SceneRenderer::rebuild_sketch_mesh_locked() {
  if (!sketch_dirty_ || !sketch_) return;
  // 先清标志再重建：即使这一刻 rebuild 失败，也不要每帧重试同一份坏几何
  // （那会把一次失败放大成每帧几十毫秒的浪费）。失败原因由 last_error_ 暴露。
  sketch_dirty_ = false;
  std::string why;
  if (!sketch_->rebuild(&why)) {
    last_error_ = why;
    return;
  }
  // rebuild 成功会覆盖 last_error_ 之外的状态；这里只清掉涂鸦子系统的错误，
  // 不越权清理渲染错误（那个由 render_frame 自己维护）。
  last_error_.clear();
}

}  // namespace magicblocks::android
