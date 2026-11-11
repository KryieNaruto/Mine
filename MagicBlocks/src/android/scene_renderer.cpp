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
  auto pr = device_->create_pipeline(cube_pipeline_desc(1));
  if (!pr.ok()) return fail("创建管线失败: " + pr.message());
  pipeline_ = pr.take();
  pipeline_hash_ = pipeline_->hash();

  return true;
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
  const dev::Extent2D sc_extent{static_cast<dev::u32>(width), static_cast<dev::u32>(height)};
  auto* vkdev_sc = dynamic_cast<backend::VulkanRenderDevice*>(device_.get());
  dev::Result<std::unique_ptr<dev::ISwapchain>> sc =
      (vkdev_sc != nullptr)
          ? vkdev_sc->create_swapchain_with_msaa(ns, sc_extent, msaa_samples_)
          : device_->create_swapchain(ns, sc_extent);
  if (!sc.ok()) return fail("创建交换链失败: " + sc.message());
  swapchain_ = sc.take();
  native_window_ = window;  // 引用由 JNI 层产生，归本对象持有

  // 实际生效的采样数：设备不支持时后端会退回 1x（并已打日志）。
  // 后面建管线必须用**这个**值，否则管线按 4 采样建、pass 按 1 采样开，
  // vkCmdDraw 会被静默丢弃 —— 屏幕上只剩清屏色，且没有任何报错。
  effective_msaa_samples_ = 1;
  if (auto* vsc_samples = dynamic_cast<backend::VulkanSwapchain*>(swapchain_.get())) {
    effective_msaa_samples_ = vsc_samples->msaa_samples();
  }

  width_ = width;
  height_ = height;
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
  shape.has_depth = false;
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

  auto pr = vkdev->create_pipeline_for_shape(cube_pipeline_desc(effective_msaa_samples_), shape);
  if (!pr.ok()) return fail("按交换链形状建管线失败: " + pr.message());
  pipeline_ = pr.take();
  pipeline_hash_ = pipeline_->hash();
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
}

bool SceneRenderer::record_into(dev::IRenderTarget* target, float yaw, float pitch, float scale,
                                float pan_x, float pan_y, std::string* err) {
  auto fail = [&](const std::string& m) {
    if (err) *err = m;
    last_error_ = m;
    return false;
  };

  auto cr = device_->create_command_list();
  if (!cr.ok()) return fail("创建命令列表失败: " + cr.message());
  res::ICommandList* cmd = cr.value().get();

  // 相机：eye 随 scale 拉远/拉近（双指缩放就是改这个距离）。
  //
  // 平移（pan）通过**移动相机**实现，而不是移动模型：相机和物体一起偏移，
  // 画面就往反方向滑，手感与"拖动画面"一致。pan_x/pan_y 是世界空间偏移，
  // Kotlin 侧已按当前可视宽度换算好，所以不同缩放下"跟手"程度一致。
  const float dist = 2.6f / (scale > 0.05f ? scale : 0.05f);
  const prim::Vec3 eye{pan_x, pan_y, dist};
  const prim::Vec3 center{pan_x, pan_y, 0.f};
  const prim::Vec3 up{0.f, 1.f, 0.f};
  const prim::Mat4 view = prim::make_look_at(eye, center, up);

  // 投影宽高比必须用**屏幕上实际呈现**的尺寸，而不是 swapchain image 的尺寸。
  //
  // 这台 pad 是横屏应用但面板物理竖置（ROTATE_90）。swapchain image 是
  // 2160x1440（未旋转），SurfaceFlinger 合成时把它旋转 90° 铺满屏幕，
  // 屏幕上看到的是 1440x2160 —— 两个方向的像素密度差 1.5 倍。
  //
  // 若直接用 image 的 2160/1440 = 1.5 当 aspect，画出来的正方体会被横向拉长
  // 1.5 倍（实测 891x396，宽高比 2.25 = 1.5 x 1.5），看起来就是个长方体。
  // 遇到 90/270 旋转时取倒数，让投影在最终屏幕朝向下正确。
  const bool rot90 = (rotation_deg_ == 90 || rotation_deg_ == 270);
  const float img_aspect = static_cast<float>(width_) / static_cast<float>(height_);
  const float aspect = rot90 ? (1.f / img_aspect) : img_aspect;
  const prim::Mat4 proj = prim::make_perspective(60.f * kPi / 180.f, aspect, 0.1f, 100.f);
  const prim::Mat4 view_proj = proj * view;

  // 模型矩阵：先 pitch（绕 X）再 yaw（绕 Y）—— 即"拖动旋转"。
  //
  // 注意旋转屏幕下两个轴要互换：手指在屏幕上左右滑，产生的是屏幕空间的 dX，
  // 而这个 dX 在**未旋转的 image 空间**里是纵向。若不互换，就会出现
  // "左右拖像上下转、上下拖像左右转"（用户反馈的问题 1）。
  // Kotlin 侧统一按"屏幕坐标系"上报（x=屏幕右，y=屏幕下），这里做轴映射。
  const float rot_x = rot90 ? pitch : yaw;   // 绕 X 轴（屏幕左右滑 → 上下翻）
  const float rot_y = rot90 ? yaw : pitch;   // 绕 Y 轴（屏幕上下滑 → 左右转）

  const prim::Mat4 rot_pitch =
      prim::make_rotation_quat(std::sin(rot_x * 0.5f), 0.f, 0.f, std::cos(rot_x * 0.5f));
  const prim::Mat4 rot_yaw =
      prim::make_rotation_quat(0.f, std::sin(rot_y * 0.5f), 0.f, std::cos(rot_y * 0.5f));
  const prim::Mat4 world = rot_yaw * rot_pitch;

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
  vp.width = static_cast<float>(width_);
  vp.height = static_cast<float>(height_);
  vp.min_depth = 0.f;
  vp.max_depth = 1.f;
  if (auto s = cmd->set_viewport(vp); !s.ok()) return fail("set_viewport: " + s.message);

  res::ScissorRect sc{};
  sc.width = static_cast<dev::u32>(width_);
  sc.height = static_cast<dev::u32>(height_);
  if (auto s = cmd->set_scissor(sc); !s.ok()) return fail("set_scissor: " + s.message);

  if (auto s = cmd->bind_pipeline(pipeline_.get(), pipeline_hash_); !s.ok()) {
    return fail("bind_pipeline: " + s.message);
  }

  constexpr dev::u32 kStageVertex = 0x1u;
  constexpr dev::u32 kStageFragment = 0x10u;
  if (auto s = cmd->push_constants(kStageVertex | kStageFragment, 0, sizeof(pc), &pc); !s.ok()) {
    return fail("push_constants: " + s.message);
  }
  if (auto s = cmd->bind_vertex_buffer(vertex_buffer_->id(), 0, 0); !s.ok()) {
    return fail("bind_vertex_buffer: " + s.message);
  }
  if (auto s = cmd->bind_index_buffer(index_buffer_->id(), res::IndexFormat::UInt32, 0); !s.ok()) {
    return fail("bind_index_buffer: " + s.message);
  }

  res::DrawArgs da{};
  da.index_count = index_count_;
  da.instance_count = 1;
  if (auto s = cmd->draw_indexed(da); !s.ok()) return fail("draw_indexed: " + s.message);
  if (auto s = cmd->end_pass(); !s.ok()) return fail("end_pass: " + s.message);

  // 真正下发 vkCmdDraw*：录得再对，不提交也不出图。
  if (auto s = device_->submit_command_list(*cmd); !s.ok()) {
    return fail("提交命令列表失败: " + s.message);
  }
  return true;
}

bool SceneRenderer::render_frame(float yaw, float pitch, float scale, float pan_x, float pan_y,
                                 std::string* err) {
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
  if (!record_into(ar.target, yaw, pitch, scale, pan_x, pan_y, err)) return false;

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
  if (!last_error_.empty()) s += " last_error=" + last_error_;
  return s;
}

}  // namespace magicblocks::android
