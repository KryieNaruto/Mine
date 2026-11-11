// MagicBlocks L3 —— 方向背景(sky)通道实现
//
// 契约与用法见 include/magicblocks/primitives/sky.h。本文件只做三件事:
//   1. 六面 PNG → L1 立方体贴图(失败即报错,不降级);
//   2. 相机 → 「逆投影 × 视图旋转的逆」矩阵(只含旋转,这是"随转不移"的来源);
//   3. 一次全屏三角形 draw 的录制(不写深度、不读时间)。
//
// 本文件**不包含任何 Vk***(G3),也不出现玩法名(G7)。
#if !defined(NOMINMAX)
#define NOMINMAX
#endif

#include "magicblocks/primitives/sky.h"

#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>

#include "core/image_io.h"
#include "magicblocks_shaders.h"

namespace magicblocks::primitives {

namespace {

namespace shaders = magicblocks::shaders;

constexpr u32 kStageVertex = 0x1u;    // 与 VkShaderStageFlagBits 同值(与 renderer 同口径)
constexpr u32 kStageFragment = 0x10u;

// push constant 只有一个 mat4(逆投影 × 视图旋转的逆)。
constexpr u32 kPushConstantBytes = 64;

// 全屏三角形的顶点数(顶点由 gl_VertexIndex 生成,不需要顶点缓冲)。
constexpr u32 kFullscreenTriangleVertices = 3;

// 天空盒的采样方式:Filter Linear / wrap ClampToEdge(sky.h 的明文要求)。
resources::SamplerDesc default_sky_sampler() {
  resources::SamplerDesc s;
  s.min_filter = resources::FilterMode::Linear;
  s.mag_filter = resources::FilterMode::Linear;
  s.mip_filter = resources::FilterMode::Nearest;  // mip_levels = 1,不参与
  s.wrap_u = resources::WrapMode::ClampToEdge;
  s.wrap_v = resources::WrapMode::ClampToEdge;
  s.compare = resources::CompareOp::Disabled;
  return s;
}

bool sampler_is_usable_for_sky(const resources::SamplerDesc& s) {
  return s.min_filter == resources::FilterMode::Linear &&
         s.mag_filter == resources::FilterMode::Linear &&
         s.wrap_u == resources::WrapMode::ClampToEdge &&
         s.wrap_v == resources::WrapMode::ClampToEdge &&
         s.compare == resources::CompareOp::Disabled;
}

bool shape_equal(const SkyTargetShape& a, const SkyTargetShape& b) {
  return a.color_format == b.color_format && a.has_depth == b.has_depth && a.samples == b.samples;
}

// 顶点布局:全屏三角形**没有自己的顶点缓冲**。
//
// 这里用 12 字节 = 一个 float3:它是引擎里最小、也是所有 shader 模板都在用的
// 顶点属性格式(fullscreen.vert / l3_scene.vert 的 location 0)。虽然本通道的
// shader 完全不读任何 location(顶点坐标由 gl_VertexIndex 生成),但 layout 仍要
// 声明一个合法的占位流:
//   - L1 的 PipelineDesc 校验要求 stride > 0(为"每个顶点流都真读一段数据"设的护栏),
//     而且属性必须落在 stride 内、binding 必须存在;
//   - 不去放宽那条护栏 —— 为了一个全屏三角形放松校验,会让真正的布局错误也溜进来。
// 调用方只需绑一块 12 字节全零的顶点缓冲即可(内容永不参与计算)。
constexpr u32 kPlaceholderVertexStride = 12;

resources::VertexLayout fullscreen_triangle_layout() {
  resources::VertexLayout layout;
  resources::VertexInputBinding b;
  b.binding = 0;
  b.stride = kPlaceholderVertexStride;
  b.instanced = false;
  layout.bindings.push_back(b);
  resources::VertexAttribute a;
  a.location = 0;
  a.binding = 0;
  a.format = resources::VertexFormat::Float3;
  a.offset = 0;
  layout.attributes.push_back(a);
  return layout;
}

}  // namespace

// ---------------------------------------------------------------- 面文件命名

const char* cube_face_suffix(resources::CubeFace face) {
  switch (face) {
    case resources::CubeFace::PositiveX:
      return "_right";
    case resources::CubeFace::NegativeX:
      return "_left";
    case resources::CubeFace::PositiveY:
      return "_top";
    case resources::CubeFace::NegativeY:
      return "_bottom";
    case resources::CubeFace::PositiveZ:
      return "_front";
    case resources::CubeFace::NegativeZ:
      return "_back";
  }
  return "";
}

std::string CubeFacePaths::path(resources::CubeFace face) const {
  std::string p = dir;
  if (!p.empty() && p.back() != '/' && p.back() != '\\') p.push_back('/');
  p += base;
  p += cube_face_suffix(face);
  p += ".png";
  return p;
}

// ---------------------------------------------------------------- 六面 → 立方体贴图

Result<std::unique_ptr<resources::ICubeMap>> create_cube_map_from_faces(
    device::IRenderDevice& device, const std::string& cube_map_dir, const std::string& base_name) {
  if (cube_map_dir.empty()) {
    return Status::Invalid("create_cube_map_from_faces: 目录为空(资源根缺失)");
  }
  if (base_name.empty()) {
    return Status::Invalid(
        "create_cube_map_from_faces: 场景名为空(例如 qwantani_dusk_2_puresky_1k)");
  }
  const CubeFacePaths paths{cube_map_dir, base_name};

  // 先**全部读进来再建纹理**:这样"第六面缺失"不会留下一个占着 24 MB 的半成品。
  magicblocks::ImageRgba8 images[resources::kCubeFaceCount];
  for (u32 i = 0; i < resources::kCubeFaceCount; ++i) {
    const auto face = static_cast<resources::CubeFace>(i);
    const std::string p = paths.path(face);
    std::string err;
    if (!magicblocks::read_png_rgba8(p, &images[i], &err)) {
      // 缺失/读不动都是明确失败:message 里带**期望路径**,不让调用方去猜。
      // 用 IoError(Status 没有单独的 NotFound 码;路径 + 解码器原因都在 message 里)。
      return Status::Io("create_cube_map_from_faces: 读第 " + std::to_string(i) + " 面失败 (" +
                        p + "): " + err);
    }
    if (images[i].width == 0 || images[i].height == 0) {
      return Status::Unsupported("create_cube_map_from_faces: 第 " + std::to_string(i) + " 面(" +
                                 p + ")尺寸为 0");
    }
  }
  const u32 w = images[0].width;
  const u32 h = images[0].height;
  for (u32 i = 1; i < resources::kCubeFaceCount; ++i) {
    if (images[i].width != w || images[i].height != h) {
      return Status::Invalid("create_cube_map_from_faces: 六面尺寸不一致 —— 面 0 为 " +
                             std::to_string(w) + "x" + std::to_string(h) + ",面 " +
                             std::to_string(i) + " 为 " + std::to_string(images[i].width) + "x" +
                             std::to_string(images[i].height) + "(立方体贴图要求六面同尺寸)");
    }
  }

  resources::CubeMapDesc cd;
  cd.extent = {w, h};
  cd.format = device::Format::RGBA8_UNORM;
  cd.usage = resources::TextureUsage::Sampled;
  auto made = device.create_cube_map(cd);
  if (!made.ok()) {
    return Status(made.status().code,
                  std::string("create_cube_map_from_faces: 建设备纹理失败: ") + made.message());
  }
  std::unique_ptr<resources::ICubeMap> cube = made.take();
  for (u32 i = 0; i < resources::kCubeFaceCount; ++i) {
    const auto face = static_cast<resources::CubeFace>(i);
    const Status s = cube->upload_face(face, images[i].pixels.data(), images[i].byte_size());
    if (!s.ok()) return s;
  }
  const Status f = cube->flush();
  if (!f.ok()) return f;
  return cube;
}

Result<std::unique_ptr<resources::ICubeMap>> create_cube_map_from_pixels(
    device::IRenderDevice& device, u32 width, u32 height,
    const std::uint8_t* const faces[resources::kCubeFaceCount]) {
  if (width == 0 || height == 0) return Status::Invalid("create_cube_map_from_pixels: 尺寸为 0");
  if (faces == nullptr) return Status::Invalid("create_cube_map_from_pixels: faces 为空");
  for (u32 i = 0; i < resources::kCubeFaceCount; ++i) {
    if (faces[i] == nullptr) {
      return Status::Invalid("create_cube_map_from_pixels: 第 " + std::to_string(i) + " 面为空");
    }
  }
  resources::CubeMapDesc cd;
  cd.extent = {width, height};
  cd.format = device::Format::RGBA8_UNORM;
  cd.usage = resources::TextureUsage::Sampled;
  auto made = device.create_cube_map(cd);
  if (!made.ok()) return made.status();
  std::unique_ptr<resources::ICubeMap> cube = made.take();
  const u64 bytes = cube->face_size();
  for (u32 i = 0; i < resources::kCubeFaceCount; ++i) {
    const Status s = cube->upload_face(static_cast<resources::CubeFace>(i), faces[i], bytes);
    if (!s.ok()) return s;
  }
  const Status f = cube->flush();
  if (!f.ok()) return f;
  return cube;
}

// ---------------------------------------------------------------- 相机 → 方向矩阵

bool make_sky_view_rotation_inverse(const Mat4& view, const Mat4& proj, Mat4* out) {
  if (out == nullptr) return false;
  // 视图矩阵的平移列被**整列丢掉**:剩下一张正交的旋转矩阵(行列式 ±1)。
  // 这样它的逆仍然是纯旋转,天空方向与相机位置无关 —— 这正是
  // "平移不变、旋转才变"这条判据的数学来源。
  Mat4 rot;
  for (float& v : rot.m) v = 0.0f;
  for (int col = 0; col < 3; ++col) {
    for (int row = 0; row < 3; ++row) rot.at(col, row) = view.at(col, row);
  }
  rot.at(3, 3) = 1.0f;

  // 视图矩阵的 3x3 必须是正交阵(长度 1、两两正交);否则说明调用方给了带缩放的
  // 视图矩阵,那时"方向"没有定义。明确失败而不是悄悄归一化。
  const Vec3 r0{rot.at(0, 0), rot.at(0, 1), rot.at(0, 2)};
  const Vec3 r1{rot.at(1, 0), rot.at(1, 1), rot.at(1, 2)};
  const Vec3 r2{rot.at(2, 0), rot.at(2, 1), rot.at(2, 2)};
  const float eps = 1e-3f;
  if (std::fabs(length(r0) - 1.0f) > eps || std::fabs(length(r1) - 1.0f) > eps ||
      std::fabs(length(r2) - 1.0f) > eps || std::fabs(dot(r0, r1)) > eps ||
      std::fabs(dot(r0, r2)) > eps || std::fabs(dot(r1, r2)) > eps) {
    return false;
  }

  bool view_ok = false;
  bool proj_ok = false;
  const Mat4 inv_view = rot.inverse(&view_ok);
  const Mat4 inv_proj = proj.inverse(&proj_ok);
  if (!view_ok || !proj_ok) return false;

  const Mat4 m = inv_proj * inv_view;
  std::memcpy(out->m, m.m, sizeof(out->m));
  return true;
}

bool make_sky_view_rotation_inverse(const CameraSnapshot& camera, Mat4* out) {
  if (out == nullptr) return false;
  const CameraPose& pose = camera.pose;
  const Vec3 f = normalize(pose.target - pose.position);
  const Vec3 up = normalize(pose.up);
  // 视线与 up 共线 → 右向量为零、视图矩阵退化。明确失败(与 ICamera 的 ±89.9°
  // 夹取同口径:合法性由相机保证,这里只做兜底)。
  if (length(f) < 1e-6f || length(up) < 1e-6f || std::fabs(dot(f, up)) > 0.9999f) return false;
  const Vec3 r = normalize(cross(f, up));
  const Vec3 u = cross(r, f);

  // 视图矩阵(右手系,+y 向上):行 = [r; u; -f],平移为零(见上面的说明)。
  Mat4 view;
  for (float& v : view.m) v = 0.0f;
  view.at(0, 0) = r.x;
  view.at(1, 0) = r.y;
  view.at(2, 0) = r.z;
  view.at(0, 1) = u.x;
  view.at(1, 1) = u.y;
  view.at(2, 1) = u.z;
  view.at(0, 2) = -f.x;
  view.at(1, 2) = -f.y;
  view.at(2, 2) = -f.z;
  view.at(3, 3) = 1.0f;

  // 投影:与 ICamera 同一套矩阵(透视 y 取负 = NDC +y 朝下,见 types.cpp)。
  Mat4 proj;
  if (camera.desc.kind == ProjectionKind::Orthographic) {
    proj = make_orthographic(-1.0f, 1.0f, -1.0f, 1.0f, camera.desc.near_plane,
                             camera.desc.far_plane, camera.desc.reverse_z);
  } else {
    constexpr float kPi = 3.14159265358979323846f;
    const float fov = camera.desc.fov_y_degrees * kPi / 180.0f;
    // 宽高比 = 1:方向只取决于"像素 → 方向"的映射,而全屏三角形的 NDC 覆盖
    // 整个视口;视口的实际宽高比由调用方在投影里编码 —— 这里用快照里的
    // viewport 尺寸还原(aspect = w/h),否则非方形目标会拉伸。
    const float aw = static_cast<float>(camera.desc.viewport.width);
    const float ah = static_cast<float>(camera.desc.viewport.height);
    const float aspect = (aw > 0.0f && ah > 0.0f) ? (aw / ah) : 1.0f;
    proj = make_perspective(fov, aspect, camera.desc.near_plane, camera.desc.far_plane,
                            camera.desc.reverse_z);
  }
  return make_sky_view_rotation_inverse(view, proj, out);
}

// ---------------------------------------------------------------- 通道实现

namespace {

class SkyPass final : public ISkyPass {
 public:
  explicit SkyPass(device::IRenderDevice& device) : device_(device) {}

  Status prepare(const SkyPassDesc& desc) override {
    if (desc.cube == nullptr) {
      return Status::Invalid("SkyPass::prepare: cube 为空(必须先建好六面立方体贴图)");
    }
    if (desc.target_shape.samples == 0) {
      return Status::Invalid("SkyPass::prepare: target_shape.samples 必须 >= 1");
    }
    if (!sampler_is_usable_for_sky(desc.sampler)) {
      return Status::Invalid(
          "SkyPass::prepare: 天空盒要求 Linear 采样 + ClampToEdge 寻址 + 不做比较采样");
    }
    for (u32 i = 0; i < resources::kCubeFaceCount; ++i) {
      if (!desc.cube->face_uploaded(static_cast<resources::CubeFace>(i))) {
        return Status::Unsupported("SkyPass::prepare: 立方体贴图第 " + std::to_string(i) +
                                   " 面尚未上传(采样结果会是未定义内容)");
      }
    }

    // 幂等:同形状 + 同贴图 → 不重建管线/采样器。
    const bool same_shape = shape_equal(desc_.target_shape, desc.target_shape);
    if (prepared_ && same_shape && desc_.cube == desc.cube && sampler_ != nullptr) {
      desc_.cube = desc.cube;
      return Status::Ok_();
    }

    if (sampler_ == nullptr) {
      auto s = device_.create_sampler(desc.sampler);
      if (!s.ok()) return s.status();
      sampler_ = std::move(s.value());
    }

    resources::PipelineDesc pd;
    pd.vertexLayout = fullscreen_triangle_layout();
    {
      resources::ShaderModule m;
      m.stage = resources::ShaderStage::Vertex;
      m.spirv = reinterpret_cast<const std::uint8_t*>(shaders::kSkyVert);
      m.size_bytes = shaders::kSkyVertSize;
      pd.stages.push_back(m);
    }
    {
      resources::ShaderModule m;
      m.stage = resources::ShaderStage::Fragment;
      m.spirv = reinterpret_cast<const std::uint8_t*>(shaders::kSkyFrag);
      m.size_bytes = shaders::kSkyFragSize;
      pd.stages.push_back(m);
    }
    // 天空不是"半透明的背景":整体覆盖,不混合。
    pd.blend.enabled = false;
    // **不写深度、不做深度测试**:场景任意几何都覆盖天空。
    pd.depth.depthTest = false;
    pd.depth.depthWrite = false;
    // 全屏三角形在 Vulkan 的 +y 朝下 NDC 里是顺时针,剔除关掉以免依赖朝向。
    pd.raster.cull = resources::CullMode::None;
    pd.primitive = resources::PrimitiveType::TriangleList;
    pd.samples = desc.target_shape.samples;
    pd.push_constant_bytes = kPushConstantBytes;
    pd.sampled_texture_slots = 1;  // slot 0 = 立方体贴图
    // 按目标形状建管线(pass 的深度附件存在性是形状的一部分,见 sky.h 的说明)。
    auto p = device_.create_pipeline(pd);
    if (!p.ok()) return p.status();
    pipeline_ = std::move(p.value());

    // 占位顶点缓冲(12 字节全零):layout 需要一块"存在且够长"的绑定。
    // 它的内容永远不被读取(shader 只用 gl_VertexIndex),所以只需一次性上传。
    if (!placeholder_vertex_buffer_) {
      resources::BufferDesc vbd;
      vbd.size = kPlaceholderVertexStride;
      vbd.usage = resources::BufferUsage::Vertex;
      auto vb = device_.create_buffer(vbd);
      if (!vb.ok()) return vb.status();
      const std::uint8_t zeros[kPlaceholderVertexStride] = {};
      // L1 分页分配器的硬契约:upload() 走 Staging 桶,而 allocate() **不会**
      // 自己触发 VMA —— 调用方必须先 reserve_pages 预取整页。漏了这一步,
      // upload 会返回 "当前页无空位 —— 请先 reserve_pages() 预取整页",
      // 表现为 SkyPass::prepare 直接失败、所有 Sky.* 像素用例全红。
      {
        auto* alloc = device_.memory_allocator();
        if (alloc == nullptr) {
          return Status::InitFailed("SkyPass::prepare: 设备没有内存分配器(无法预取 Staging 页)");
        }
        const Status rs = alloc->reserve_pages(resources::MemoryKind::Staging, 1);
        if (!rs.ok()) return rs;
      }
      const Status us = vb.value()->upload(zeros, sizeof(zeros));
      if (!us.ok()) return us;
      vb.value()->flush_pending();
      placeholder_vertex_buffer_ = std::move(vb.value());
    }

    desc_ = desc;
    if (sampler_ != nullptr) desc_.sampler = sampler_->desc();
    prepared_ = true;
    return Status::Ok_();
  }

  Status record(resources::ICommandList& cmd, const CameraSnapshot& camera,
                const SkyRecordOptions& options) override {
    if (!prepared_ || pipeline_ == nullptr) {
      return Status::Invalid("SkyPass::record: 先 prepare()(管线/贴图尚未就绪)");
    }
    if (desc_.cube == nullptr) {
      return Status::Invalid("SkyPass::record: 没有立方体贴图,拒绝录制(不退化成纯色清屏)");
    }
    Mat4 inv_proj_rot;
    if (!make_sky_view_rotation_inverse(camera, &inv_proj_rot)) {
      return Status::Invalid(
          "SkyPass::record: 当前相机的视图/投影不可逆(检查 up 与视线是否共线、near/far)");
    }

    SkyRecordStats st;
    std::memcpy(st.inv_proj_rot, inv_proj_rot.m, sizeof(st.inv_proj_rot));

    if (options.clear_color || options.clear_depth) {
      resources::RenderPassDesc rp;
      rp.target = current_target_;  // 由 set_target()/begin 由调用方给的命令列表决定
      rp.clear_color = options.clear_color;
      std::memcpy(rp.clear_color_rgba, options.clear_color_rgba, sizeof(rp.clear_color_rgba));
      rp.clear_depth = options.clear_depth;
      rp.clear_depth_value = options.clear_depth_value;
      rp.area_x = options.area_x;
      rp.area_y = options.area_y;
      rp.area_width = options.area_width;
      rp.area_height = options.area_height;
      if (rp.target == nullptr) {
        return Status::Invalid(
            "SkyPass::record: 需要开 pass 但没给目标(先 set_target(),或把 clear_color/clear_depth "
            "都设为 false 与场景共用 pass)");
      }
      const Status s = cmd.begin_pass(rp);
      if (!s.ok()) return s;
      st.passes = 1;
    }

    Status s = cmd.bind_pipeline(pipeline_.get(), pipeline_->hash());
    if (!s.ok()) return s;
    ++st.pipeline_binds;

    s = cmd.bind_sampled_texture(desc_.cube->id(), sampler_->id(), 0);
    if (!s.ok()) return s;
    ++st.texture_binds;

    s = cmd.push_constants(kStageVertex | kStageFragment, 0, kPushConstantBytes, inv_proj_rot.m);
    if (!s.ok()) return s;
    ++st.push_constant_sets;

    // 全屏三角形的顶点由 gl_VertexIndex 生成,但仍要绑一块合法的顶点流
    // (见 fullscreen_triangle_layout 的说明)。内容永不参与计算。
    if (placeholder_vertex_buffer_ != nullptr) {
      s = cmd.bind_vertex_buffer(placeholder_vertex_buffer_->id(), 0, 0);
      if (!s.ok()) return s;
    }

    resources::DrawArgs da;
    da.vertex_count = kFullscreenTriangleVertices;
    da.instance_count = 1;
    s = cmd.draw(da);
    if (!s.ok()) return s;
    ++st.draws;

    stats_ = st;
    return Status::Ok_();
  }

  Status set_downscale(u32 level) override {
    if (level != 0) {
      return Status::Unsupported("SkyPass::set_downscale: 本轮只支持 0(全分辨率),得到 " +
                                 std::to_string(level));
    }
    return Status::Ok_();
  }

  void set_target(device::IRenderTarget* target) override { current_target_ = target; }

  const SkyPassDesc& desc() const override { return desc_; }
  const SkyRecordStats& stats() const override { return stats_; }
  void reset_stats() override { stats_ = SkyRecordStats{}; }

 private:
  device::IRenderDevice& device_;
  SkyPassDesc desc_{};
  std::unique_ptr<resources::IPipeline> pipeline_;
  std::unique_ptr<resources::ISampler> sampler_;
  std::unique_ptr<resources::IBuffer> placeholder_vertex_buffer_;
  device::IRenderTarget* current_target_ = nullptr;
  SkyRecordStats stats_{};
  bool prepared_ = false;
};

}  // namespace

std::unique_ptr<ISkyPass> create_sky_pass(device::IRenderDevice& device) {
  return std::make_unique<SkyPass>(device);
}

}  // namespace magicblocks::primitives
