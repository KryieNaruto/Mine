// MagicBlocks UI 消费层 —— CLI 侧缩略图 / 天空盒 / 场景渲染实现(UI 布局 v2)
//
// 见 render_backend.h 顶部说明:本文件是 v2 唯一碰 GPU 的地方,UI 库本体不碰。
#include "render_backend.h"

#include <cmath>
#include <cstring>
#include <string>
#include <vector>

#include "mb_internal/magicblocks/device/render_target.h"
#include "mb_internal/magicblocks/primitives/batch.h"
#include "mb_internal/magicblocks/primitives/camera.h"
#include "mb_internal/magicblocks/primitives/mesh.h"
#include "mb_internal/magicblocks/primitives/model_catalog.h"
#include "mb_internal/magicblocks/resources/buffer.h"
#include "mb_internal/magicblocks/resources/command.h"
#include "mb_internal/magicblocks/resources/pipeline.h"
#include "magicblocks_shaders.h"  // kL3_sceneVert / kL3_sceneFrag

namespace magicblocks::ui_cli {

// 本 TU 内部短别名(命名空间作用域内,只服务本文件的实现)。
// 注意:别名只能在 .cpp 里定义 —— 放头文件会与别的 TU 的 `namespace dev = ...`
// 冲突,而 `using X = namespace` 在 C++ 里根本不合法(头文件因此写全限定)。
namespace dev = magicblocks::device;
namespace mbp = magicblocks::primitives;
namespace res = magicblocks::resources;

namespace {

constexpr float kPi = 3.14159265358979323846f;
constexpr float kDegToRad = kPi / 180.0f;

// 与 model_cli / l3_cli 同口径的实例化 3D 管线(顶点 + 实例属性)。
res::PipelineDesc scene_pipeline_desc(dev::u32 samples, bool has_depth) {
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
  d.raster.cull = res::CullMode::None;  // 模型朝向不定
  d.samples = samples;
  d.primitive = res::PrimitiveType::TriangleList;
  d.push_constant_bytes = 128;
  d.attachment.color_format = dev::Format::RGBA8_UNORM;
  d.attachment.has_depth = has_depth;
  d.attachment.samples = samples;
  return d;
}

// 按方位角/仰角把相机放到半径为 dist 的球面上,看向原点。
//
// 为什么不用 orbit():缩略图必须逐字节可复现,而 orbit 是**增量**接口 ——
// 多调一次就变一张图。这里直接算绝对位姿。
bool place_camera(mbp::ICamera& camera, float dist, float yaw_deg, float pitch_deg,
                  std::string* error) {
  const float yaw = yaw_deg * kDegToRad;
  const float pitch = pitch_deg * kDegToRad;
  const float cp = std::cos(pitch);
  mbp::CameraPose pose;
  pose.position = {dist * cp * std::sin(yaw), dist * std::sin(pitch), dist * cp * std::cos(yaw)};
  pose.target = {0.0f, 0.0f, 0.0f};
  pose.up = {0.0f, 1.0f, 0.0f};
  if (auto s = camera.set_pose(pose); !s.ok()) {
    if (error) *error = "相机位姿非法: " + s.message;
    return false;
  }
  return true;
}

// 按模型归一化后的包围盒半径反推相机距离(framing_margin 越小越满)。
float framing_distance(const mbp::IMesh& mesh, float fov_y_degrees, float margin) {
  const float half_size = mesh.bounds().empty ? 0.5f : mesh.bounds().radius();
  const float safe_half = half_size > 1e-6f ? half_size : 0.5f;
  const float half_fov_tan = std::tan(fov_y_degrees * 0.5f * kDegToRad);
  return (safe_half / half_fov_tan) * margin;
}

// 一条固定基色的实例属性(与 model_cli 一致,保证跨任务构图可比)。
mbp::InstanceAttribs default_instance() {
  mbp::InstanceAttribs inst{};
  inst.model[0] = inst.model[5] = inst.model[10] = inst.model[15] = 1.0f;
  inst.base_color[0] = 0.85f;
  inst.base_color[1] = 0.72f;
  inst.base_color[2] = 0.35f;
  inst.base_color[3] = 1.0f;
  inst.roughness = 0.6f;
  inst.metallic = 0.0f;
  return inst;
}

// 推 128 字节 push constant:view_proj + 法线矩阵。
struct ScenePC {
  float view_proj[16];
  float normal_matrix[16];
};

ScenePC make_pc(const mbp::ICamera& camera) {
  ScenePC pc{};
  std::memcpy(pc.view_proj, camera.view_proj_matrix().m, sizeof(float) * 16);
  pc.normal_matrix[0] = pc.normal_matrix[5] = pc.normal_matrix[10] =
      pc.normal_matrix[15] = 1.0f;
  return pc;
}

}  // namespace

// ---------------------------------------------------------------- 场景资源

struct SceneResources::Owned {
  std::unique_ptr<res::IPipeline> pipeline;
  std::unique_ptr<res::IBuffer> instance_buffer;
};

SceneResources::SceneResources() = default;
SceneResources::~SceneResources() = default;
SceneResources::SceneResources(SceneResources&&) noexcept = default;
SceneResources& SceneResources::operator=(SceneResources&&) noexcept = default;

std::unique_ptr<SceneResources> SceneResources::create(dev::IRenderDevice& device,
                                                       std::uint32_t samples, bool has_depth) {
  auto out = std::make_unique<SceneResources>();
  out->owned_ = std::make_unique<Owned>();

  auto pl = device.create_pipeline(scene_pipeline_desc(samples, has_depth));
  if (!pl.ok()) {
    out->error_ = "创建场景管线失败: " + pl.message();
    return out;  // ok_ = false
  }
  out->owned_->pipeline = pl.take();
  out->pipeline_ = out->owned_->pipeline.get();

  const mbp::InstanceAttribs inst = default_instance();
  res::BufferDesc ibd;
  ibd.size = sizeof(mbp::InstanceAttribs);
  ibd.usage = res::BufferUsage::Vertex;
  auto ib = device.create_buffer(ibd);
  if (!ib.ok()) {
    out->error_ = "创建实例缓冲失败: " + ib.message();
    return out;
  }
  out->owned_->instance_buffer = ib.take();
  out->instance_buffer_ = out->owned_->instance_buffer.get();
  if (auto s = out->instance_buffer_->upload(&inst, sizeof(inst)); !s.ok()) {
    out->error_ = "上传实例失败: " + s.message;
    return out;
  }
  out->instance_buffer_->flush_pending();

  out->ok_ = true;
  return out;
}

// ---------------------------------------------------------------- 缩略图

Thumbnail render_thumbnail(dev::IRenderDevice& device, SceneResources& res,
                           const std::string& path, const std::string& name,
                           const ThumbnailConfig& cfg) {
  Thumbnail out;
  out.name = name;
  out.path = path;
  out.width = cfg.width;
  out.height = cfg.height;

  if (cfg.width == 0 || cfg.height == 0) {
    out.error = "缩略图尺寸为 0";
    return out;
  }
  if (!res.ok()) {
    out.error = "场景资源未就绪: " + res.error();
    return out;
  }

  dev::TargetDesc td;
  td.kind = dev::TargetDesc::Kind::Offscreen;
  td.extent = {cfg.width, cfg.height};
  td.with_depth = true;  // 缩略图也要正确遮挡
  td.format = dev::Format::RGBA8_UNORM;
  auto tgt = device.create_offscreen_target(td);
  if (!tgt.ok()) {
    out.error = "创建缩略图离屏目标失败: " + tgt.message();
    return out;
  }

  mbp::ModelNormalize norm;
  norm.enabled = true;
  norm.target_diagonal = cfg.target_diagonal;
  auto model = mbp::load_and_upload_model(device, path, name, norm);
  if (!model.ok()) {
    out.error = "加载/上传模型失败: " + model.message();
    return out;  // 失败条目照样返回(ok=false),由列表显示原因
  }
  const mbp::IMesh* mesh = model.value()->mesh();
  if (mesh == nullptr) {
    out.error = "IMesh 为空";
    return out;
  }

  mbp::CameraDesc cd;
  cd.kind = mbp::ProjectionKind::Perspective;
  cd.fov_y_degrees = 45.0f;
  cd.near_plane = 0.05f;
  cd.far_plane = 100.0f;
  cd.viewport = {cfg.width, cfg.height};
  auto camera = mbp::create_camera();
  if (!camera) {
    out.error = "创建相机失败";
    return out;
  }
  if (auto s = camera->set_desc(cd); !s.ok()) {
    out.error = "相机描述非法: " + s.message;
    return out;
  }
  const float dist = framing_distance(*mesh, cd.fov_y_degrees, cfg.framing_margin);
  if (!place_camera(*camera, dist, cfg.yaw_degrees, cfg.pitch_degrees, &out.error)) {
    return out;
  }

  auto cmd = device.create_command_list();
  if (!cmd.ok()) {
    out.error = "创建命令列表失败: " + cmd.message();
    return out;
  }

  res::RenderPassDesc rp;
  rp.target = tgt.value().get();
  rp.clear_color = true;
  rp.clear_color_rgba[0] = static_cast<float>(cfg.clear_rgb[0]) / 255.f;
  rp.clear_color_rgba[1] = static_cast<float>(cfg.clear_rgb[1]) / 255.f;
  rp.clear_color_rgba[2] = static_cast<float>(cfg.clear_rgb[2]) / 255.f;
  rp.clear_color_rgba[3] = 1.0f;
  rp.clear_depth = true;
  rp.clear_depth_value = 1.0f;
  if (auto s = cmd.value()->begin_pass(rp); !s.ok()) {
    out.error = "begin_pass 失败: " + s.message;
    return out;
  }

  res::Viewport vp;
  vp.width = static_cast<float>(cfg.width);
  vp.height = static_cast<float>(cfg.height);
  vp.min_depth = 0.0f;
  vp.max_depth = 1.0f;
  if (auto s = cmd.value()->set_viewport(vp); !s.ok()) {
    out.error = "set_viewport 失败: " + s.message;
    return out;
  }
  res::ScissorRect sr;
  sr.width = cfg.width;
  sr.height = cfg.height;
  if (auto s = cmd.value()->set_scissor(sr); !s.ok()) {
    out.error = "set_scissor 失败: " + s.message;
    return out;
  }

  res::IPipeline* pl = res.pipeline();
  if (auto s = cmd.value()->bind_pipeline(pl, pl->hash()); !s.ok()) {
    out.error = "bind_pipeline 失败: " + s.message;
    return out;
  }
  const ScenePC pc = make_pc(*camera);
  if (auto s = cmd.value()->push_constants(0x11u, 0, sizeof(pc), &pc); !s.ok()) {
    out.error = "push_constants 失败: " + s.message;
    return out;
  }

  const res::IBuffer* vb = mesh->vertex_buffer();
  const res::IBuffer* ib = mesh->index_buffer();
  if (vb == nullptr || ib == nullptr) {
    out.error = "网格没有顶点/索引缓冲";
    return out;
  }
  if (auto s = cmd.value()->bind_vertex_buffer(vb->id(), 0, 0); !s.ok()) {
    out.error = "bind 顶点缓冲失败: " + s.message;
    return out;
  }
  if (auto s = cmd.value()->bind_vertex_buffer(res.instance_buffer()->id(), 1, 0); !s.ok()) {
    out.error = "bind 实例缓冲失败: " + s.message;
    return out;
  }
  if (auto s = cmd.value()->bind_index_buffer(ib->id(), res::IndexFormat::UInt32, 0); !s.ok()) {
    out.error = "bind 索引缓冲失败: " + s.message;
    return out;
  }
  res::DrawArgs da;
  da.index_count = mesh->index_count();
  da.instance_count = 1;
  if (auto s = cmd.value()->draw_indexed(da); !s.ok()) {
    out.error = "draw_indexed 失败: " + s.message;
    return out;
  }
  if (auto s = cmd.value()->end_pass(); !s.ok()) {
    out.error = "end_pass 失败: " + s.message;
    return out;
  }
  // UIG2:一帧恰好一次提交(缩略图也是"一帧")
  if (auto s = device.submit_command_list(*cmd.value()); !s.ok()) {
    out.error = "提交失败: " + s.message;
    return out;
  }

  // 离屏读回(spec §5.5 唯一允许的口子;UIG1 豁免路径就叫 offscreen)
  out.rgba.assign(static_cast<std::size_t>(cfg.width) * cfg.height * 4u, 0);
  if (auto s = tgt.value()->readback_rgba8(out.rgba.data(), out.rgba.size()); !s.ok()) {
    out.error = "读回失败: " + s.message;
    out.rgba.clear();
    return out;
  }
  out.ok = true;
  return out;
}

std::vector<Thumbnail> render_thumbnails(dev::IRenderDevice& device, SceneResources& res,
                                         const std::vector<std::string>& names,
                                         const std::vector<std::string>& paths,
                                         const ThumbnailConfig& cfg) {
  std::vector<Thumbnail> out;
  const std::size_t n = names.size() < paths.size() ? names.size() : paths.size();
  out.reserve(n);
  for (std::size_t i = 0; i < n; ++i) {
    out.push_back(render_thumbnail(device, res, paths[i], names[i], cfg));
  }
  return out;
}

// ---------------------------------------------------------------- 场景

bool render_scene_area(dev::IRenderDevice& device, SceneResources& res,
                       mbp::ISkyPass* sky, const res::ICubeMap* sky_cube,
                       const std::string& model_path,
                       const std::string& model_name, const SceneRenderConfig& cfg,
                       std::uint8_t* pixels, std::size_t row_pitch_bytes, std::string* error) {
  auto fail = [error](const std::string& m) {
    if (error) *error = m;
    return false;
  };
  if (pixels == nullptr) return fail("像素缓冲为空");
  if (!res.ok()) return fail("场景资源未就绪: " + res.error());
  if (cfg.width == 0 || cfg.height == 0) return fail("场景尺寸为 0");

  const std::uint32_t ax = cfg.area_x;
  const std::uint32_t ay = cfg.area_y;
  const std::uint32_t aw = cfg.area_width ? cfg.area_width : cfg.width;
  const std::uint32_t ah = cfg.area_height ? cfg.area_height : cfg.height;
  if (ax >= cfg.width || ay >= cfg.height) return fail("场景区域起点越界");
  if (aw == 0 || ah == 0) return fail("场景区域尺寸为 0");
  // 区域裁剪到帧内(不越界写)
  const std::uint32_t cw = (ax + aw > cfg.width) ? (cfg.width - ax) : aw;
  const std::uint32_t ch = (ay + ah > cfg.height) ? (cfg.height - ay) : ah;

  // 天空盒单独画在无深度目标上,读回后作为底色(见下面的详细说明)。
  std::vector<std::uint8_t> sky_pixels;
  bool have_sky_pixels = false;

  // ---- 模型(可选:没有选中模型时只画天空/背景) ----
  std::unique_ptr<mbp::ILoadedModel> model;
  if (!model_path.empty()) {
    mbp::ModelNormalize norm;
    norm.enabled = true;
    norm.target_diagonal = cfg.target_diagonal;
    auto m = mbp::load_and_upload_model(device, model_path, model_name, norm);
    if (!m.ok()) return fail("加载/上传模型失败: " + m.message());
    model = m.take();
  }

  // ---- 相机(天空盒与模型共用同一个 view_proj) ----
  mbp::CameraDesc cd;
  cd.kind = mbp::ProjectionKind::Perspective;
  cd.fov_y_degrees = 45.0f;
  cd.near_plane = 0.05f;
  cd.far_plane = 100.0f;
  cd.viewport = {cw, ch};  // 宽高比由 viewport 推导,CameraDesc 没有 aspect 字段
  auto camera = mbp::create_camera();
  if (!camera) return fail("创建相机失败");
  if (auto s = camera->set_desc(cd); !s.ok()) return fail("相机描述非法: " + s.message);
  {
    float dist = 3.0f;
    if (model && model->mesh() != nullptr) {
      dist = framing_distance(*model->mesh(), cd.fov_y_degrees, cfg.framing_margin);
    }
    if (!place_camera(*camera, dist, cfg.yaw_degrees, cfg.pitch_degrees, error)) return false;
  }

  // ---- 离屏目标(只覆盖区域,读回后贴回区域的像素) ----
  //
  // 深度附件**恒开**:实体管线在 SceneResources::create(..., has_depth=true) 里
  // 已声明深度附件,render pass 的形状必须与它逐项一致,否则 vkCmdDraw 被静默丢弃。
  // 早先按"有没有模型"开关深度,直接导致天空管线每帧重建 ——
  // 重建出来的管线没有绑过描述符集,回放里那条早先录下的 bind_sampled_texture
  // 落空(VUID-vkCmdDraw-None-08600),实测 0xC0000005。
  // 形状固定成一种,天空与实体两条管线都只建一次。
  dev::TargetDesc td;
  td.kind = dev::TargetDesc::Kind::Offscreen;
  td.extent = {cw, ch};
  td.with_depth = true;
  td.format = dev::Format::RGBA8_UNORM;
  auto tgt = device.create_offscreen_target(td);
  if (!tgt.ok()) return fail("创建场景离屏目标失败: " + tgt.message());

  // ---- 天空盒:独立的无深度目标 + 独立命令列表 ----
  //
  // 为什么天空盒不能画在带深度的主目标上(三个坑叠在一起,都是实测踩出来的):
  //   1) 设备帧回放的 bind_sampled_texture 用 pipeline_at(idx) 取管线,该下标是一条
  //      BindSampledTexture 记录本身(旁路表只在 BindPipeline 那条填指针),于是回落到
  //      list.bound_pipeline() —— 那是**录制结束时**的绑定。天空画在前、实体画在后,
  //      回落就拿到实体管线(slots=0),报"管线没有设备态描述符集"。
  //      → 天空必须自成一条命令流,它的 bind_pipeline 之后不再有别的 bind_pipeline。
  //   2) 天空管线刻意 depthTest/depthWrite 全关(它不写深度),而
  //      vulkan_render_device.cpp:471 的 shape.has_depth 正是**由管线深度状态派生**的:
  //      这条管线的内建 render pass 永远没有深度附件。主目标带深度 →
  //      VUID-vkCmdDraw-renderPass-02684(实测 0xC0000005)。
  //      → 天空必须有一个**不带深度**的目标。
  //   3) prepare() 按 has_depth 重建管线,重建后的管线没绑过描述符集。
  //      → 形状固定成 has_depth=false,只建一次。
  // 所以:天空 → 无深度临时目标 → 读回 → 贴到主目标上方实体 pass 之前。
  if (sky != nullptr) {
    dev::TargetDesc skyd;
    skyd.kind = dev::TargetDesc::Kind::Offscreen;
    skyd.extent = {cw, ch};
    skyd.with_depth = false;  // 见上面 (2):天空管线本来就没有深度附件
    skyd.format = dev::Format::RGBA8_UNORM;
    auto sky_tgt = device.create_offscreen_target(skyd);
    if (!sky_tgt.ok()) return fail("创建天空离屏目标失败: " + sky_tgt.message());

    // 形状 + 立方体贴图 + 采样器**一次给全**:setup_sky() 刻意不提前 prepare
    // (见那里的说明),所以这里是这条管线唯一一次建立点。
    mbp::SkyPassDesc sd = sky->desc();
    sd.target_shape.color_format = dev::Format::RGBA8_UNORM;
    sd.target_shape.has_depth = false;
    sd.target_shape.samples = 1;
    sd.cube = sky_cube;
    // 天空盒要求 Linear + ClampToEdge(面内回绕会把边缘拉成条纹)。
    sd.sampler.min_filter = res::FilterMode::Linear;
    sd.sampler.mag_filter = res::FilterMode::Linear;
    sd.sampler.wrap_u = res::WrapMode::ClampToEdge;
    sd.sampler.wrap_v = res::WrapMode::ClampToEdge;
    if (auto s = sky->prepare(sd); !s.ok()) return fail("天空通道 prepare 失败: " + s.message);
    sky->set_target(sky_tgt.value().get());

    auto sky_cmd = device.create_command_list();
    if (!sky_cmd.ok()) return fail("创建天空命令列表失败: " + sky_cmd.message());

    mbp::SkyRecordOptions opt;
    opt.clear_color = true;
    opt.clear_depth = false;  // 这个目标没有深度附件
    opt.clear_color_rgba[0] = static_cast<float>(cfg.clear_rgb[0]) / 255.f;
    opt.clear_color_rgba[1] = static_cast<float>(cfg.clear_rgb[1]) / 255.f;
    opt.clear_color_rgba[2] = static_cast<float>(cfg.clear_rgb[2]) / 255.f;
    opt.clear_color_rgba[3] = 1.0f;
    if (auto s = sky->record(*sky_cmd.value(), camera->snapshot(), opt); !s.ok()) {
      return fail("天空盒 record 失败: " + s.message);
    }
    if (auto s = sky_cmd.value()->end_pass(); !s.ok()) {
      return fail("天空 end_pass 失败: " + s.message);
    }
    if (auto s = device.submit_command_list(*sky_cmd.value()); !s.ok()) {
      return fail("天空盒提交失败: " + s.message);
    }

    // 读回天空结果,等实体画完后作为底色贴进主目标区域。
    sky_pixels.assign(static_cast<std::size_t>(cw) * ch * 4u, 0);
    if (auto s = sky_tgt.value()->readback_rgba8(sky_pixels.data(), sky_pixels.size()); !s.ok()) {
      return fail("天空读回失败: " + s.message);
    }
    have_sky_pixels = true;
  }

  // ---- 模型:自己的命令列表(与天空盒完全分开) ----
  if (model && model->mesh() != nullptr) {
    const mbp::IMesh& mesh = *model->mesh();
    const res::IBuffer* vb = mesh.vertex_buffer();
    const res::IBuffer* ib = mesh.index_buffer();
    if (vb == nullptr || ib == nullptr) return fail("网格没有顶点/索引缓冲");

    auto cmd = device.create_command_list();
    if (!cmd.ok()) return fail("创建命令列表失败: " + cmd.message());

    res::RenderPassDesc rp;
    rp.target = tgt.value().get();
    // 一律清成 cfg.clear_rgb:下面的合成拿"清屏色"当"模型没盖住"的哨兵,
    // 所以这里必须是确定的清屏色,不能不清(不清就是上一帧的垃圾数据)。
    rp.clear_color = true;
    rp.clear_color_rgba[0] = static_cast<float>(cfg.clear_rgb[0]) / 255.f;
    rp.clear_color_rgba[1] = static_cast<float>(cfg.clear_rgb[1]) / 255.f;
    rp.clear_color_rgba[2] = static_cast<float>(cfg.clear_rgb[2]) / 255.f;
    rp.clear_color_rgba[3] = 1.0f;
    rp.clear_depth = td.with_depth;
    rp.clear_depth_value = 1.0f;
    if (auto s = cmd.value()->begin_pass(rp); !s.ok()) return fail("begin_pass 失败: " + s.message);

    res::Viewport vp;
    vp.width = static_cast<float>(cw);
    vp.height = static_cast<float>(ch);
    vp.min_depth = 0.0f;
    vp.max_depth = 1.0f;
    if (auto s = cmd.value()->set_viewport(vp); !s.ok()) {
      return fail("set_viewport 失败: " + s.message);
    }
    res::ScissorRect sr;
    sr.width = cw;
    sr.height = ch;
    if (auto s = cmd.value()->set_scissor(sr); !s.ok()) {
      return fail("set_scissor 失败: " + s.message);
    }

    res::IPipeline* pl = res.pipeline();
    if (auto s = cmd.value()->bind_pipeline(pl, pl->hash()); !s.ok()) {
      return fail("bind_pipeline 失败: " + s.message);
    }
    const ScenePC pc = make_pc(*camera);
    if (auto s = cmd.value()->push_constants(0x11u, 0, sizeof(pc), &pc); !s.ok()) {
      return fail("push_constants 失败: " + s.message);
    }
    if (auto s = cmd.value()->bind_vertex_buffer(vb->id(), 0, 0); !s.ok()) {
      return fail("bind 顶点缓冲失败: " + s.message);
    }
    if (auto s = cmd.value()->bind_vertex_buffer(res.instance_buffer()->id(), 1, 0); !s.ok()) {
      return fail("bind 实例缓冲失败: " + s.message);
    }
    if (auto s = cmd.value()->bind_index_buffer(ib->id(), res::IndexFormat::UInt32, 0); !s.ok()) {
      return fail("bind 索引缓冲失败: " + s.message);
    }
    res::DrawArgs da;
    da.index_count = mesh.index_count();
    da.instance_count = 1;
    if (auto s = cmd.value()->draw_indexed(da); !s.ok()) {
      return fail("draw_indexed 失败: " + s.message);
    }
    if (auto s = cmd.value()->end_pass(); !s.ok()) return fail("end_pass 失败: " + s.message);
    if (auto s = device.submit_command_list(*cmd.value()); !s.ok()) {
      return fail("提交失败: " + s.message);
    }
  }

  std::vector<std::uint8_t> rgba(static_cast<std::size_t>(cw) * ch * 4u, 0);
  const bool model_drawn = (model && model->mesh() != nullptr);
  if (!have_sky_pixels) {
    // 没有天空盒:主目标自带清屏色,直接读回。
    if (auto s = tgt.value()->readback_rgba8(rgba.data(), rgba.size()); !s.ok()) {
      return fail("读回失败: " + s.message);
    }
  } else if (!model_drawn) {
    // 只有天空:主目标一次都没被提交过,读它是未定义内容 —— 直接用天空结果。
    rgba = sky_pixels;
  } else {
    // 有天空盒也有模型:模型 pass 清成了 cfg.clear_rgb,拿它当"模型没盖住"的哨兵,
    // 这些像素取天空结果,其余取模型结果。
    std::vector<std::uint8_t> model_pixels(static_cast<std::size_t>(cw) * ch * 4u, 0);
    if (auto s = tgt.value()->readback_rgba8(model_pixels.data(), model_pixels.size()); !s.ok()) {
      return fail("读回失败: " + s.message);
    }
    const std::uint8_t br = static_cast<std::uint8_t>(cfg.clear_rgb[0]);
    const std::uint8_t bg = static_cast<std::uint8_t>(cfg.clear_rgb[1]);
    const std::uint8_t bb = static_cast<std::uint8_t>(cfg.clear_rgb[2]);
    for (std::size_t i = 0; i < rgba.size(); i += 4) {
      const bool is_clear = model_pixels[i + 0] == br && model_pixels[i + 1] == bg &&
                            model_pixels[i + 2] == bb;
      const std::uint8_t* src = is_clear ? (sky_pixels.data() + i) : (model_pixels.data() + i);
      rgba[i + 0] = src[0];
      rgba[i + 1] = src[1];
      rgba[i + 2] = src[2];
      rgba[i + 3] = src[3];
    }
  }
  // 贴回区域的像素(区域外一个字节都不动)
  for (std::uint32_t y = 0; y < ch; ++y) {
    std::uint8_t* dst = pixels + static_cast<std::size_t>(ay + y) * row_pitch_bytes +
                        static_cast<std::size_t>(ax) * 4u;
    std::memcpy(dst, rgba.data() + static_cast<std::size_t>(y) * cw * 4u,
                static_cast<std::size_t>(cw) * 4u);
  }
  return true;
}

// ---------------------------------------------------------------- 天空盒

SkySetup setup_sky(dev::IRenderDevice& device, const std::string& asset_root,
                   const std::string& scene) {
  SkySetup s;
  s.scene = scene;
  if (asset_root.empty()) {
    s.error = "未提供资源根(--asset-root 或 MB_ASSET_ROOT)";
    return s;
  }
  if (scene.empty()) {
    s.error = "未指定天空场景名";
    return s;
  }
  const std::string cube_dir = asset_root + "/cubeMap";
  auto cube = mbp::create_cube_map_from_faces(device, cube_dir, scene);
  if (!cube.ok()) {
    s.error = "建立方体贴图失败(" + cube_dir + "): " + cube.message();
    return s;
  }
  s.cube = cube.take();

  s.pass = mbp::create_sky_pass(device);
  if (!s.pass) {
    s.error = "创建天空通道失败(create_sky_pass 返回空)";
    return s;
  }
  // **不在这里 prepare**:管线形状必须与最终渲染目标逐项一致(颜色格式 / 有无深度 /
  // 采样数),而"有没有深度"取决于本次是否同时画模型 —— 那要到 render_scene_area()
  // 才知道。早先这里按 has_depth=true 先建一次,画纯天空时再以 has_depth=false
  // 重建,重建出来的管线**没有绑过描述符集**,回放里那条早先录下的
  // bind_sampled_texture 就落空(VUID-vkCmdDraw-None-08600),实测直接 0xC0000005。
  // SkyPass::prepare 是幂等的,由 render_scene_area() 用真实形状统一 prepare 一次。
  s.ok = true;
  return s;
}

}  // namespace magicblocks::ui_cli
