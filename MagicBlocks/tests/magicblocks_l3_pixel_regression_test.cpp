// MagicBlocks L3 —— **像素级**渲染回归测试
//
// ============================ 为什么需要这个文件 ============================
//
// 这条测试的诞生背景值得写在这里，因为它是本项目最容易复发的一类坑。
//
// 当时的现象：L3 的 `paint` 子命令**成功退出**、写出了 PNG、日志一切正常，
// 但图上一个像素都没有 —— 只有清屏色。排查中发现：
//
//   1. 仓库里**所有** "像素正确" 的断言（L0/L2 golden）走的都是
//      `OffscreenTarget::draw_selftest()` 这条**目标内部**的独立绘制路径。
//      它自己建 render pass、自己建管线、自己提交，与生产路径毫无关系。
//   2. 生产路径是 `ICommandList` 录制 + `IRenderDevice::submit_command_list()`，
//      这条路径的**像素正确性从未被任何测试验证过**。
//
// 于是 260 个测试全绿，而生产路径一像素都画不出来。测试通过率在这里是
// 无意义的：**没有任何一条测试看过提交路径产出的像素**。
//
// 根因（已修）：`IBuffer::upload()` 只把数据 memcpy 进 Staging 桶并挂进
// `pending_`，真正拷到 GPU 侧缓冲要等 `flush_pending()`。漏掉这步时：
//   - 不报错；`upload()` 返回 Ok；
//   - GPU 上顶点缓冲仍是全 0 → 所有三角形退化到原点 → 光栅化不出任何片元；
//   - 清屏色照常生效，画面"干净得像刚清过屏"。
//
// 本文件的职责就是把这条路径钉死：**真的上传、真的录制、真的提交、
// 真的读回像素，然后断言像素不是清屏色。**
//
// 判据刻意做得对"实现细节"不敏感（不锁具体颜色值，只锁"画出来了"），
// 这样它抓的是"零像素"这个真实故障，而不是某种特定渲染结果的指纹。
#include <gtest/gtest.h>

#include <array>
#include <cstdlib>
#include <cstdint>
#include <memory>
#include <vector>

#include "mb_internal/magicblocks/device/render_device.h"
#include "mb_internal/magicblocks/primitives/types.h"
#include "mb_internal/magicblocks/resources/buffer.h"
#include "mb_internal/magicblocks/resources/command.h"
#include "mb_internal/magicblocks/resources/pipeline.h"
#include "magicblocks_shaders.h"

namespace dev = magicblocks::device;
namespace res = magicblocks::resources;
namespace mbp = magicblocks::primitives;

namespace {

constexpr dev::u32 kW = 64;
constexpr dev::u32 kH = 64;

// 清屏色刻意选一个"不像任何合法输出"的值，这样"读回 == 清屏色"能可靠地
// 判定为"一个像素都没画"。
constexpr float kClearR = 0.25f;
constexpr float kClearG = 0.50f;
constexpr float kClearB = 0.75f;

dev::Result<std::unique_ptr<dev::IRenderDevice>> make_device() {
  dev::DeviceDesc d;
  d.app_name = "MagicBlocks L3 Pixel Regression";
  d.require_software_rasterizer = true;
  return dev::create_render_device(d);
}

dev::Result<std::unique_ptr<dev::IRenderTarget>> make_target(dev::IRenderDevice& device) {
  dev::TargetDesc td;
  td.kind = dev::TargetDesc::Kind::Offscreen;
  td.extent = {kW, kH};
  td.with_depth = false;
  td.format = dev::Format::RGBA8_UNORM;
  return device.create_offscreen_target(td);
}

// 一个覆盖**整个视口**的三角形（像素坐标，原点左上）。
// 用大三角形而不是贴边四边形，是为了让"线性插值 / 裁剪 / 半个像素规则"
// 这些细节都不会影响"至少有一部分像素被写到"这个判据。
struct PosVertex {
  float x = 0.0f;
  float y = 0.0f;
};

res::PipelineDesc pixel_pipeline_desc() {
  res::PipelineDesc d;

  res::VertexInputBinding vb;
  vb.binding = 0;
  vb.stride = sizeof(PosVertex);
  vb.instanced = false;
  d.vertexLayout.bindings.push_back(vb);

  res::VertexAttribute a;
  a.location = 0;
  a.binding = 0;
  a.format = res::VertexFormat::Float2;
  a.offset = 0;
  d.vertexLayout.attributes.push_back(a);

  {
    res::ShaderModule m;
    m.stage = res::ShaderStage::Vertex;
    m.spirv = reinterpret_cast<const std::uint8_t*>(magicblocks::shaders::kPixel_regressionVert);
    m.size_bytes = magicblocks::shaders::kPixel_regressionVertSize;
    d.stages.push_back(m);
  }
  {
    res::ShaderModule m;
    m.stage = res::ShaderStage::Fragment;
    m.spirv = reinterpret_cast<const std::uint8_t*>(magicblocks::shaders::kPixel_regressionFrag);
    m.size_bytes = magicblocks::shaders::kPixel_regressionFragSize;
    d.stages.push_back(m);
  }

  d.blend.enabled = false;          // 不混合：输出就是最终像素，判据最直接
  d.depth.depthTest = false;
  d.depth.depthWrite = false;
  d.raster.cull = res::CullMode::None;
  d.samples = 1;
  d.primitive = res::PrimitiveType::TriangleList;
  d.push_constant_bytes = 0;        // 这个测试不需要常量
  return d;
}

// 读回整张图，统计"与清屏色不同"的像素数。
std::size_t count_non_clear_pixels(dev::IRenderTarget& target) {
  std::vector<std::uint8_t> px(static_cast<std::size_t>(kW) * kH * 4u, 0);
  if (!target.readback_rgba8(px.data(), px.size()).ok()) return 0;

  const std::uint8_t cr = static_cast<std::uint8_t>(kClearR * 255.0f + 0.5f);
  const std::uint8_t cg = static_cast<std::uint8_t>(kClearG * 255.0f + 0.5f);
  const std::uint8_t cb = static_cast<std::uint8_t>(kClearB * 255.0f + 0.5f);

  std::size_t n = 0;
  for (std::size_t i = 0; i < px.size(); i += 4) {
    // 允许 ±2 的量化误差：清屏色经过 float → unorm 的舍入在实现间可能有 1 的差别。
    const int dr = std::abs(static_cast<int>(px[i + 0]) - cr);
    const int dg = std::abs(static_cast<int>(px[i + 1]) - cg);
    const int db = std::abs(static_cast<int>(px[i + 2]) - cb);
    if (dr > 2 || dg > 2 || db > 2) ++n;
  }
  return n;
}

}  // namespace

// ================================================================ 像素级回归

TEST(PixelRegression, SubmitCommandListActuallyWritesPixels) {
  // 这条测试存在的唯一理由：**证明提交路径产出的像素不是清屏色**。
  // 它会在漏掉 flush_pending()、几何退化、shader 坐标跑飞、
  // render pass 不兼容等所有"静默空图"故障下失败。
  auto dev_r = make_device();
  if (!dev_r.ok()) {
    ADD_FAILURE() << "需要软件光栅设备: " << dev_r.message();
    return;
  }
  dev::IRenderDevice* device = dev_r.value().get();

  // L1 的分配器桶不会自动长页，建缓冲前必须预取（含 upload 内部要用的 Staging）。
  {
    res::IMemoryAllocator* alloc = device->memory_allocator();
    for (res::MemoryKind kind :
         {res::MemoryKind::Vertex, res::MemoryKind::Index, res::MemoryKind::Staging}) {
      ASSERT_TRUE(alloc->reserve_pages(kind, 2).ok());
    }
  }

  auto tgt_r = make_target(*device);
  ASSERT_TRUE(tgt_r.ok()) << tgt_r.message();
  dev::IRenderTarget* target = tgt_r.value().get();

  // ---- 几何：一个覆盖整屏的大三角形 ----
  // 直接给 NDC：一个覆盖整屏的大三角形（三个顶点都在视口外，确保覆盖全部像素）。
  // 不在这里验证像素→NDC 的换算 —— 那是 L2 golden 的职责。
  const std::array<PosVertex, 3> verts = {{
      {-3.0f, -3.0f},
      {3.0f, -3.0f},
      {-3.0f, 3.0f},
  }};

  res::BufferDesc vbd;
  vbd.size = verts.size() * sizeof(PosVertex);
  vbd.usage = res::BufferUsage::Vertex;
  auto vb_r = device->create_buffer(vbd);
  ASSERT_TRUE(vb_r.ok()) << vb_r.message();
  res::IBuffer* vb = vb_r.value().get();
  ASSERT_TRUE(vb->upload(verts.data(), vbd.size).ok());

  // ---- 关键：upload() 只是入队，这里才真正拷到 GPU ----
  vb->flush_pending();

  // 自检：若上传批还挂着，说明 flush 没生效，后面的断言会得出误导性的结论。
  ASSERT_EQ(vb->pending_upload_bytes(), 0u)
      << "upload() 之后 upload 批未清空 —— flush_pending() 没生效。"
         "这正是历史上 L3 输出空图的根因。";

  // ---- 管线 ----
  auto pl_r = device->create_pipeline(pixel_pipeline_desc());
  ASSERT_TRUE(pl_r.ok()) << pl_r.message();
  res::IPipeline* pipeline = pl_r.value().get();

  // ---- 录制 ----
  auto cmd_r = device->create_command_list();
  ASSERT_TRUE(cmd_r.ok()) << cmd_r.message();
  res::ICommandList* cmd = cmd_r.value().get();

  res::RenderPassDesc rp;
  rp.target = target;
  rp.clear_color = true;
  rp.clear_color_rgba[0] = kClearR;
  rp.clear_color_rgba[1] = kClearG;
  rp.clear_color_rgba[2] = kClearB;
  rp.clear_color_rgba[3] = 1.0f;
  rp.clear_depth = false;
  ASSERT_TRUE(cmd->begin_pass(rp).ok());

  res::Viewport vp;
  vp.width = static_cast<float>(kW);
  vp.height = static_cast<float>(kH);
  vp.min_depth = 0.0f;
  vp.max_depth = 1.0f;
  ASSERT_TRUE(cmd->set_viewport(vp).ok());

  res::ScissorRect sr;
  sr.width = kW;
  sr.height = kH;
  ASSERT_TRUE(cmd->set_scissor(sr).ok());

  ASSERT_TRUE(cmd->bind_pipeline(pipeline, 0x504958454u).ok());
  ASSERT_TRUE(cmd->bind_vertex_buffer(vb->id(), 0, 0).ok());

  res::DrawArgs da;
  da.vertex_count = static_cast<dev::u32>(verts.size());
  da.instance_count = 1;
  ASSERT_TRUE(cmd->draw(da).ok());

  ASSERT_TRUE(cmd->end_pass().ok());

  // ---- 提交：这一步才真正把命令变成像素 ----
  // 注意 submit_command_list 内部有 "draw_calls == 0 就报错" 的保护，
  // 所以它返回 Ok 意味着 draw 确实下发了；但**不代表像素写进了目标**，
  // 下面必须自己读回验证。
  ASSERT_TRUE(device->submit_command_list(*cmd).ok());

  // ---- 判据 ----
  const std::size_t painted = count_non_clear_pixels(*target);
  const std::size_t total = static_cast<std::size_t>(kW) * kH;
  // 那个大三角形恰好覆盖半屏（斜边对角），所以阈值取 1/8 就足够有判别力：
  // "一个像素都没画" 时 painted == 0，任何真实的遮挡/退化故障也都会远低于这个数。
  EXPECT_GT(painted, total / 8)
      << "提交路径产出的像素几乎全是清屏色（非清屏像素 " << painted << "/" << total
      << "）。draw 已下发但没写出像素，典型原因是：upload 之后漏了 flush_pending()、"
         "顶点坐标全为 0 导致三角形退化、或 shader 把顶点推出了裁剪区。";
}

TEST(PixelRegression, UploadWithoutFlushIsDetectable) {
  // 把历史故障**主动复现**成一条测试：不调 flush_pending()，
  // pending_upload_bytes() 必须非 0。这条断言是上面那条测试的"元断言" ——
  // 它保证了"忘记 flush"这件事在测试层面是可观测的，而不是静默的。
  auto dev_r = make_device();
  if (!dev_r.ok()) {
    ADD_FAILURE() << "需要软件光栅设备: " << dev_r.message();
    return;
  }
  dev::IRenderDevice* device = dev_r.value().get();

  {
    res::IMemoryAllocator* alloc = device->memory_allocator();
    for (res::MemoryKind kind : {res::MemoryKind::Vertex, res::MemoryKind::Staging}) {
      ASSERT_TRUE(alloc->reserve_pages(kind, 2).ok());
    }
  }

  const std::array<PosVertex, 3> verts = {{{0.0f, 0.0f}, {1.0f, 0.0f}, {0.0f, 1.0f}}};
  res::BufferDesc vbd;
  vbd.size = verts.size() * sizeof(PosVertex);
  vbd.usage = res::BufferUsage::Vertex;
  auto vb_r = device->create_buffer(vbd);
  ASSERT_TRUE(vb_r.ok()) << vb_r.message();
  res::IBuffer* vb = vb_r.value().get();

  ASSERT_TRUE(vb->upload(verts.data(), vbd.size).ok());
  EXPECT_GT(vb->pending_upload_bytes(), 0u)
      << "upload() 之后应立即有未提交数据；若这里是 0，说明 upload 语义变了，"
         "上一条测试的 flush 断言会失去意义。";

  vb->flush_pending();
  EXPECT_EQ(vb->pending_upload_bytes(), 0u);
}
