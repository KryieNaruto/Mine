// MagicBlocks —— MSAA 抗锯齿接线测试
//
// ============================ 为什么需要这个文件 ============================
//
// MSAA 在本引擎里曾经是"上层 API 齐了、底层断了两处"的状态：
//
//   断点 A：`vulkan_draw.cpp` 的 VkPipelineMultisampleStateCreateInfo 把
//           `rasterizationSamples` 硬编码成 VK_SAMPLE_COUNT_1_BIT。
//           于是即使 PipelineDesc.samples=4、render pass 的附件也是 4 采样，
//           管线仍按 1 采样光栅化 → framebuffer 采样数不匹配。
//
//   断点 B：全仓没有 `pResolveAttachments`。多重采样颜色渲染完之后不会被
//           resolve 成单采样图像，读回拿到的是未定义内容。
//
// 再加上更上游还有一道墙：`OffscreenTarget::init()` 曾经直接
// `if (desc_.samples != 1) return Unsupported("L0 只支持 samples=1")`，
// 目标图像永远只建 1 个采样点 —— 后面再怎么改都白改。
//
// 这三处都改完之后，**唯一能证明"真的接上了"的东西就是像素本身**：
// 同样的几何、同样的相机，samples=1 出的是硬阶梯边沿，
// samples=4 的边沿上必须出现大量"中间色"过渡像素。
//
// 所以本文件的核心用例是 `Msaa.EdgesActuallySmooth`：
// 它统计"既不是前景色、也不是背景色"的过渡像素数，断言 4x 显著多于 1x。
// 这条断言对实现细节不敏感（不锁具体颜色、不锁具体数值），
// 只锁"边沿确实被抗锯齿了"这一件事。
#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <set>
#include <vector>

#include "magicblocks/device/render_device.h"
#include "magicblocks/resources/buffer.h"
#include "magicblocks/resources/command.h"
#include "magicblocks/resources/pipeline.h"
#include "magicblocks_shaders.h"

namespace dev = magicblocks::device;
namespace res = magicblocks::resources;

namespace {

constexpr dev::u32 kW = 96;
constexpr dev::u32 kH = 96;

// 清屏色与前景色刻意选得"三点都对得很开"，这样"在两者之间"的像素
// 只可能是抗锯齿产生的混合色，不会与量化误差混淆。
// ⚠️ 前景色必须与 shaders/pixel_regression.frag 实际输出**逐通道对齐**：
//    那边写的是 vec4(1.0, 0.0, 0.0, 1.0) —— 纯**红**，不是白。
//    这条曾经写成白色，导致每 2451 个纯红前景像素全被误判成"过渡像素"，
//    1x 与 4x 的统计因此完全失去判别力（1x 反而"过渡"更多）。
constexpr float kClearR = 0.0f;
constexpr float kClearG = 0.0f;
constexpr float kClearB = 0.0f;
constexpr float kFgR = 1.0f;
constexpr float kFgG = 0.0f;
constexpr float kFgB = 0.0f;

dev::Result<std::unique_ptr<dev::IRenderDevice>> make_device() {
  dev::DeviceDesc d;
  d.app_name = "MagicBlocks MSAA";
  d.require_software_rasterizer = true;
  return dev::create_render_device(d);
}

// 一个斜放的三角形：三条边**全部**不与像素网格对齐。
// 这是关键 —— 轴对齐的边在 1 采样下也会"看起来是直的"，
// 只有斜边才能让硬阶梯 / 抗锯齿的差别显形。
struct PosVertex {
  float x = 0.0f;
  float y = 0.0f;
};

constexpr std::array<PosVertex, 3> kSlantedTriangle = {{
    {-0.82f, -0.74f},
    {0.61f, -0.31f},
    {-0.42f, 0.87f},
}};

res::PipelineDesc msaa_pipeline_desc(dev::u32 samples) {
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

  d.blend.enabled = false;
  d.depth.depthTest = false;
  d.depth.depthWrite = false;
  d.raster.cull = res::CullMode::None;
  d.samples = samples;
  d.primitive = res::PrimitiveType::TriangleList;
  d.push_constant_bytes = 0;
  return d;
}

// 用给定采样数渲染一次，返回整张 RGBA8 像素。
dev::Result<std::vector<std::uint8_t>> render_slanted(dev::IRenderDevice& device,
                                                     dev::u32 samples) {
  {
    res::IMemoryAllocator* alloc = device.memory_allocator();
    for (res::MemoryKind kind :
         {res::MemoryKind::Vertex, res::MemoryKind::Staging}) {
      dev::Status s = alloc->reserve_pages(kind, 2);
      if (!s.ok()) return s;
    }
  }

  dev::TargetDesc td;
  td.kind = dev::TargetDesc::Kind::Offscreen;
  td.extent = {kW, kH};
  td.with_depth = false;
  td.format = dev::Format::RGBA8_UNORM;
  td.samples = samples;
  auto tgt_r = device.create_offscreen_target(td);
  if (!tgt_r.ok()) return tgt_r.status();

  dev::IRenderTarget* target = tgt_r.value().get();

  res::BufferDesc vbd;
  vbd.size = kSlantedTriangle.size() * sizeof(PosVertex);
  vbd.usage = res::BufferUsage::Vertex;
  auto vb_r = device.create_buffer(vbd);
  if (!vb_r.ok()) return vb_r.status();
  res::IBuffer* vb = vb_r.value().get();
  if (!vb->upload(kSlantedTriangle.data(), vbd.size).ok()) {
    return dev::Status::InitFailed("MSAA 测试: upload 失败");
  }
  // upload() 只入队；不 flush 就是全零缓冲 = 三角形退化 = 空图（且不报错）。
  vb->flush_pending();

  auto pl_r = device.create_pipeline(msaa_pipeline_desc(samples));
  if (!pl_r.ok()) return pl_r.status();
  res::IPipeline* pipeline = pl_r.value().get();

  auto cmd_r = device.create_command_list();
  if (!cmd_r.ok()) return cmd_r.status();
  res::ICommandList* cmd = cmd_r.value().get();

  res::RenderPassDesc rp;
  rp.target = target;
  rp.clear_color = true;
  rp.clear_color_rgba[0] = kClearR;
  rp.clear_color_rgba[1] = kClearG;
  rp.clear_color_rgba[2] = kClearB;
  rp.clear_color_rgba[3] = 1.0f;
  rp.clear_depth = false;
  if (!cmd->begin_pass(rp).ok()) return dev::Status::InitFailed("MSAA 测试: begin_pass 失败");

  res::Viewport vp;
  vp.width = static_cast<float>(kW);
  vp.height = static_cast<float>(kH);
  vp.min_depth = 0.0f;
  vp.max_depth = 1.0f;
  cmd->set_viewport(vp);

  res::ScissorRect sr;
  sr.width = kW;
  sr.height = kH;
  cmd->set_scissor(sr);

  cmd->bind_pipeline(pipeline, 0x4D534141u + samples);
  cmd->bind_vertex_buffer(vb->id(), 0, 0);

  res::DrawArgs da;
  da.vertex_count = static_cast<dev::u32>(kSlantedTriangle.size());
  da.instance_count = 1;
  if (!cmd->draw(da).ok()) return dev::Status::InitFailed("MSAA 测试: draw 失败");
  if (!cmd->end_pass().ok()) return dev::Status::InitFailed("MSAA 测试: end_pass 失败");

  dev::Status sub = device.submit_command_list(*cmd);
  if (!sub.ok()) return sub;

  std::vector<std::uint8_t> px(static_cast<std::size_t>(kW) * kH * 4u, 0);
  dev::Status rb = target->readback_rgba8(px.data(), px.size());
  if (!rb.ok()) return rb;
  return px;
}

bool is_near(std::uint8_t v, float target) {
  const int t = static_cast<int>(target * 255.0f + 0.5f);
  return std::abs(static_cast<int>(v) - t) <= 4;
}

// "过渡像素" = 既不是纯背景（清屏色）也不是纯前景（纯白）的像素。
// 硬阶梯边沿（1 采样）只有这两种像素；MSAA 在边沿半覆盖的地方会混出中间色。
std::size_t count_transition_pixels(const std::vector<std::uint8_t>& px) {
  std::size_t n = 0;
  for (std::size_t i = 0; i + 3 < px.size(); i += 4) {
    const std::uint8_t r = px[i + 0];
    const std::uint8_t g = px[i + 1];
    const std::uint8_t b = px[i + 2];
    const bool is_bg = is_near(r, kClearR) && is_near(g, kClearG) && is_near(b, kClearB);
    const bool is_fg = is_near(r, kFgR) && is_near(g, kFgG) && is_near(b, kFgB);
    if (!is_bg && !is_fg) ++n;
  }
  return n;
}

// 唯一颜色数：MSAA 生效时中间色变多，这个数会明显上涨。
std::size_t count_unique_colors(const std::vector<std::uint8_t>& px) {
  std::set<std::uint32_t> seen;
  for (std::size_t i = 0; i + 3 < px.size(); i += 4) {
    const std::uint32_t c = (static_cast<std::uint32_t>(px[i + 0]) << 16) |
                            (static_cast<std::uint32_t>(px[i + 1]) << 8) |
                            static_cast<std::uint32_t>(px[i + 2]);
    seen.insert(c);
  }
  return seen.size();
}

bool has_msaa_in_caps(const dev::DeviceCaps& caps, dev::u32 level) {
  for (dev::u32 lvl : caps.supported_msaa_levels) {
    if (lvl == level) return true;
  }
  return false;
}

}  // namespace

// ================================================================ MSAA

TEST(Msaa, SupportedLevelsFromCaps) {
  // 设备能力必须报出 MSAA 等级，且 1（=关闭）永远在列表里。
  // 没有这条，后面的 --msaa 校验与 target 的采样数校验都无从谈起。
  auto dev_r = make_device();
  if (!dev_r.ok()) {
    ADD_FAILURE() << "需要软件光栅设备: " << dev_r.message();
    return;
  }
  const dev::DeviceCaps& caps = dev_r.value()->caps();
  ASSERT_FALSE(caps.supported_msaa_levels.empty()) << "supported_msaa_levels 不能为空";
  EXPECT_TRUE(has_msaa_in_caps(caps, 1u)) << "列表必须含 1（无 MSAA）";
}

TEST(Msaa, DefaultIsSingleSample) {
  // 缺省必须是 1 采样：这是"不加 --msaa 时 3 张 golden 逐字节不变"的前提。
  res::PipelineDesc d;
  EXPECT_EQ(d.samples, 1u) << "PipelineDesc.samples 缺省必须是 1";
}

TEST(Msaa, RejectsNonPowerOfTwo) {
  // 非 2 的幂必须在 target 创建阶段就被拒，而不是静默降级。
  auto dev_r = make_device();
  if (!dev_r.ok()) {
    ADD_FAILURE() << "需要软件光栅设备: " << dev_r.message();
    return;
  }
  dev::IRenderDevice* device = dev_r.value().get();

  dev::TargetDesc td;
  td.kind = dev::TargetDesc::Kind::Offscreen;
  td.extent = {32, 32};
  td.with_depth = false;
  td.samples = 3;  // 非 2 的幂
  auto tgt_r = device->create_offscreen_target(td);
  EXPECT_FALSE(tgt_r.ok()) << "samples=3 应当被拒";
}

TEST(Msaa, RejectsUnsupportedLevel) {
  // caps 里没有的等级必须明确报错，不静默降级成 1。
  auto dev_r = make_device();
  if (!dev_r.ok()) {
    ADD_FAILURE() << "需要软件光栅设备: " << dev_r.message();
    return;
  }
  dev::IRenderDevice* device = dev_r.value().get();
  const dev::DeviceCaps& caps = device->caps();

  // 找一个 caps 明确不支持的合法 2 的幂。64 在 SwiftShader 上通常不支持。
  dev::u32 bad = 64;
  if (has_msaa_in_caps(caps, bad)) {
    GTEST_SKIP() << "该设备恰好支持 64 采样，跳过（无法构造不被支持的合法值）";
  }

  dev::TargetDesc td;
  td.kind = dev::TargetDesc::Kind::Offscreen;
  td.extent = {32, 32};
  td.with_depth = false;
  td.samples = bad;
  auto tgt_r = device->create_offscreen_target(td);
  EXPECT_FALSE(tgt_r.ok()) << "samples=" << bad << " 不在 caps 里，应当被拒";
}

TEST(Msaa, EdgesActuallySmooth) {
  // ★ 核心用例：唯一能证明 MSAA 真的接上了的测试。
  //
  // 同一个斜三角形，分别用 samples=1 与 samples=4 各渲染一次，
  // 统计"过渡像素"（既非纯背景也非纯前景）：
  //   - 1 采样：每个像素要么整块盖住（纯白）要么没盖住（纯黑），过渡像素≈0
  //   - 4 采样：边沿像素按 4 个采样点的覆盖率混色，过渡像素显著更多
  // 断点 A（光栅化采样数）或断点 B（resolve 附件）任何一处没接上，这条都会失败。
  auto dev_r = make_device();
  if (!dev_r.ok()) {
    ADD_FAILURE() << "需要软件光栅设备: " << dev_r.message();
    return;
  }
  dev::IRenderDevice* device = dev_r.value().get();

  if (!has_msaa_in_caps(device->caps(), 4u)) {
    GTEST_SKIP() << "该设备不支持 4x MSAA，跳过";
  }

  auto px1 = render_slanted(*device, 1);
  ASSERT_TRUE(px1.ok()) << "samples=1 渲染失败: " << px1.message();

  auto px4 = render_slanted(*device, 4);
  ASSERT_TRUE(px4.ok()) << "samples=4 渲染失败: " << px4.message();
  ASSERT_EQ(px4.value().size(), px1.value().size());

  const std::size_t t1 = count_transition_pixels(px1.value());
  const std::size_t t4 = count_transition_pixels(px4.value());
  const std::size_t u1 = count_unique_colors(px1.value());
  const std::size_t u4 = count_unique_colors(px4.value());

  // 判据刻意**只用相对关系**，不设绝对下界。
  //
  // 为什么不写 `EXPECT_LT(t1, 8)`：某些驱动（含 SwiftShader）可能对 1 采样路径
  // 也开着隐式 AA/走样抑制，那会让 t1 不是 0 而是几十。一旦把 t1 钉在绝对数字上，
  // 这条测试就变成了"驱动实现指纹"，换驱动就红，而我们真正关心的从来不是
  // "1x 恰好有多少过渡像素"，而是"4x 比 1x 明显更平滑"这个**关系**。
  //
  // 同理下面也不断言具体颜色值 —— MSAA 的采样点位置由驱动决定，不可移植。

  // ★ 核心断言：4x 的过渡像素必须是 1x 的 4 倍以上。
  // 断点没接上时（rasterizationSamples 仍硬编码 1、或缺 pResolveAttachments、
  // 或目标仍是单采样），t4 会与 t1 基本持平，这条立刻失败。
  // 4 倍这个阈值取得比理论值保守：真实 MSAA 在斜边上通常是 3~10 倍。
  EXPECT_GT(t4, t1 * 4)
      << "MSAA 没有生效：samples=1 过渡像素 " << t1 << "，samples=4 过渡像素 " << t4
      << "（要求 4x > 4×1x）。若两者接近，说明 rasterizationSamples 仍是硬编码 1"
         "（断点 A），或 resolve 附件没接上（断点 B），或目标图像仍是单采样。";

  // 第二重独立证据：唯一颜色数。1 采样只有清晰前景/背景两色，
  // 4 采样会沿边沿摊出一整片中间灰。这条与"过渡像素数"口径不同，
  // 两个都过才说明是真的抗锯齿，而不是某处数值抖动。
  EXPECT_GT(u4, u1) << "samples=4 的唯一颜色数（" << u4 << "）应当多于 1 采样（" << u1 << "）";
}
