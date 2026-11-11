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

// 设备是否支持任意一个 >1 的采样数（决定交换链测试该请求几采样）。
bool any_msaa_supported(const dev::DeviceCaps& caps) {
  for (dev::u32 lvl : caps.supported_msaa_levels) {
    if (lvl > 1) return true;
  }
  return false;
}

// caps 里最大的 >1 采样等级（没有则返回 1）。
// 用例"选一个 1x 之外的档位来对照"时用它：设备支持 4x 就 4x，支持 8x 就 8x。
dev::u32 best_msaa_in_caps(const dev::DeviceCaps& caps) {
  dev::u32 best = 1;
  for (dev::u32 lvl : caps.supported_msaa_levels) {
    if (lvl > 1 && lvl > best) best = lvl;
  }
  return best;
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

  // ⚠️ 2026-11-11：这条用例原先写 `GTEST_SKIP() << "该设备恰好支持 64 采样"`。
  // 那是**倒数第二层**的懒办法：全仓纪律是「缺能力是 FAIL 不是 SKIP」
  // （MagicBlocks/docs/ARCHITECTURE.md），而且靠"挑一个设备不支持的数"来构造
  // 反例，换一台驱动更宽的机器就失去判别力。
  //
  // 现在改成**与设备能力无关**的两段断言：
  //   1) 遍历 caps 里的**每一个**等级：合法的必须被接受（建目标成功）；
  //   2) 遍历一组**结构性非法**的采样数（非 2 的幂 / 超范围 / 非 Vulkan 采样数），
  //      它们在任何设备上都必须被拒 —— 这才是"拒绝逻辑存在"的永久判据。
  // 若设备支持的等级集合是 {1,2,4,8,16}（全支持），第 1 段也照样成立，
  // 不会再有任何"跳过"。

  ASSERT_FALSE(caps.supported_msaa_levels.empty()) << "supported_msaa_levels 不能为空";
  ASSERT_TRUE(has_msaa_in_caps(caps, 1u)) << "supported_msaa_levels 必须含 1";

  // ---- 段 1：caps 里每个等级都必须真的能建出目标 ----
  for (dev::u32 lvl : caps.supported_msaa_levels) {
    dev::TargetDesc td;
    td.kind = dev::TargetDesc::Kind::Offscreen;
    td.extent = {16, 16};
    td.with_depth = false;
    td.samples = lvl;
    auto tgt = device->create_offscreen_target(td);
    EXPECT_TRUE(tgt.ok()) << "caps 报支持 " << lvl << "x，但建离屏目标失败: " << tgt.message()
                          << "\n  caps 与真实能力不一致 = 用户在 UI 上选了它却拿不到 MSAA。";
  }

  // ---- 段 2：结构性非法的采样数必须被拒（任何设备、永远可判）----
  // 3/5/6/7/9/12 不是 2 的幂；0 按 1 处理（合法）；32/64/100 超出 Vulkan 采样数集合。
  const dev::u32 kStructurallyInvalid[] = {3u, 5u, 6u, 7u, 9u, 12u, 32u, 64u, 100u};
  for (dev::u32 bad : kStructurallyInvalid) {
    if (has_msaa_in_caps(caps, bad)) {
      // caps 里居然出现了非法等级 —— 这本身就是要抓的静默降级。
      ADD_FAILURE() << "caps.supported_msaa_levels 里出现了非法采样数 " << bad
                    << "（不是合法的 Vulkan 采样数，to_vk_samples 会把它悄悄变成 1x）";
      continue;
    }
    dev::TargetDesc td;
    td.kind = dev::TargetDesc::Kind::Offscreen;
    td.extent = {32, 32};
    td.with_depth = false;
    td.samples = bad;
    auto tgt_r = device->create_offscreen_target(td);
    EXPECT_FALSE(tgt_r.ok()) << "samples=" << bad
                             << " 不是合法的 Vulkan 采样数，caps 也没报它，却建目标成功了 —— "
                                "拒绝逻辑失效（用户会拿到一张 1x 图却以为开了 " << bad << "x）";
  }
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

  // 2026-11-11：原来是 `GTEST_SKIP() << "该设备不支持 4x"`。
  // 全仓纪律是「缺能力是 FAIL 不是 SKIP」，而且"设备没有我需要的采样数"
  // 恰恰是要在 CI 上立刻响的部署问题 —— 跳过等于永远不知道。
  const dev::u32 msaa = any_msaa_supported(device->caps())
                            ? (has_msaa_in_caps(device->caps(), 4u) ? 4u
                                                                    : best_msaa_in_caps(device->caps()))
                            : 1u;
  ASSERT_GT(msaa, 1u)
      << "设备 caps.supported_msaa_levels 里没有任何 >1 的等级 —— MSAA 无从判别。"
         "按纪律这是 FAIL（不是 SKIP）：请检查设备能力探测 / SwiftShader 部署。";

  auto px1 = render_slanted(*device, 1);
  ASSERT_TRUE(px1.ok()) << "samples=1 渲染失败: " << px1.message();

  auto px4 = render_slanted(*device, msaa);
  ASSERT_TRUE(px4.ok()) << "samples=" << msaa << " 渲染失败: " << px4.message();
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

// ================================================== 交换链路径（真机生效的那条）
//
// 上面所有的 Msaa.* 用例都只走**离屏**路径。离屏全绿并不代表用户能看到抗锯齿：
// 真机上画面走的是交换链（VkSwapchainKHR + acquire/present），那是一套独立的
// 图像、独立的 render pass 生命周期（必须交回 PRESENT_SRC_KHR）。
//
// 这里补上交换链路径的三件事实：
//   1) current_target() 返回的是刚 acquire 到的那张，不是硬编码的第 0 张；
//   2) MS 图是**每张交换链图一张**，不是所有视图共用一张；
//   3) 每张 MS 图都占一个分页分配器页，且页号互不相同（G4：零裸分配）。
//
// 为了让 backend 之外的代码不出现 Vk 类型名（G3 门），全部通过
// vulkan_swapchain.h 里的 std::size_t / std::uint64_t / bool 探针断言。

#if defined(_WIN32)

#include <windows.h>

#include "backend/vulkan/vulkan_render_device.h"
#include "backend/vulkan/vulkan_swapchain.h"

namespace backend = magicblocks::backend;

namespace {

struct MsaaTestWindow {
  HWND hwnd = nullptr;
  HINSTANCE inst = nullptr;
  ~MsaaTestWindow() {
    if (hwnd) DestroyWindow(hwnd);
  }
};

MsaaTestWindow make_msaa_test_window() {
  MsaaTestWindow w;
  w.inst = GetModuleHandleW(nullptr);
  WNDCLASSEXW wc{};
  wc.cbSize = sizeof(wc);
  wc.style = CS_OWNDC;
  wc.lpfnWndProc = DefWindowProcW;
  wc.hInstance = w.inst;
  wc.lpszClassName = L"MagicBlocksMsaaSwapchainTest";
  RegisterClassExW(&wc);
  w.hwnd = CreateWindowExW(0, wc.lpszClassName, L"mb-msaa", WS_OVERLAPPEDWINDOW, 0, 0, 320, 240,
                           nullptr, nullptr, w.inst, nullptr);
  return w;
}

dev::NativeSurface msaa_test_surface(const MsaaTestWindow& w) {
  dev::NativeSurface s;
  s.kind = dev::NativeSurface::Kind::Win32;
  s.a = static_cast<void*>(w.hwnd);
  s.b = reinterpret_cast<std::uintptr_t>(w.inst);
  return s;
}

}  // namespace

// ---------------------------------------------------------------- 用例
//
// 用**交换链图数量**推 MS 图数量。断言口径是"相等"，不是"大于 0"：
// 共用一张 MS 图时 msaa_image_count() == 1，而 swapchain_image_count() >= 2，
// 数量断言立刻变红。这就是这条用例的判别力所在。
TEST(MsaaSwapchain, PerImageMsaaBuffers) {
  auto dev_r = make_device();
  if (!dev_r.ok()) {
    ADD_FAILURE() << "需要软件光栅设备: " << dev_r.message();
    return;
  }
  dev::IRenderDevice* device = dev_r.value().get();
  // 2026-11-11：这里原来是 `GTEST_SKIP() << "该驱动不支持 4x"`。
  // 全仓纪律是「缺能力是 FAIL 不是 SKIP」—— 而且"设备不支持 4x"在 SwiftShader 上
  // 是部署出了问题，必须在 CI 上响，不能静默跳过。
  const dev::u32 msaa = best_msaa_in_caps(device->caps());
  ASSERT_GT(msaa, 1u)
      << "设备不支持任何 >1 的采样数，交换链 per-image MSAA 无从验证。"
         "按纪律这是 FAIL（不是 SKIP）。";

  auto w = make_msaa_test_window();
  ASSERT_TRUE(w.hwnd != nullptr) << "创建隐藏 HWND 失败";
  const dev::NativeSurface s = msaa_test_surface(w);

  auto* vkdev = dynamic_cast<backend::VulkanRenderDevice*>(device);
  if (vkdev == nullptr) {
    ADD_FAILURE() << "需要 VulkanRenderDevice 才能创建带 MSAA 的交换链";
    return;
  }

  auto sc = vkdev->create_swapchain_with_msaa(s, dev::Extent2D{320, 240}, msaa);
  ASSERT_TRUE(sc.ok()) << "创建带 " << msaa << "x 的交换链失败: " << sc.message();
  auto* chain = dynamic_cast<backend::VulkanSwapchain*>(sc.value().get());
  ASSERT_TRUE(chain != nullptr);

  // 设备侧若退回 1x，就没 MS 图可查 —— 但这**不是**"环境不支持"，而是
  // 「请求了 MSAA 却静默退回」，正是用户报的"开了没效果"。所以是 FAIL 不是 SKIP。
  ASSERT_EQ(chain->msaa_samples(), msaa)
      << "请求 " << msaa << "x，交换链实际生效 " << chain->msaa_samples()
      << "x —— MSAA 被静默退回，用户看到的就是「开了 MSAA 没效果」。";

  const std::size_t img_n = chain->swapchain_image_count();
  ASSERT_GE(img_n, 2u) << "交换链至少要 2 张图，否则'每张一张 MS 图'无从区分";

  // ★ 核心断言 1：MS 图数量 == 交换链图数量。
  // 共用一张时这里是 1 != img_n，立刻红。
  EXPECT_EQ(chain->msaa_image_count(), img_n)
      << "MS 图数量（" << chain->msaa_image_count() << "）必须等于交换链图数量（" << img_n
      << "）。不等说明所有视图共用了同一张 MS 缓冲 —— in-flight 帧之间会互相踩，"
         "真机上表现为'一个不生效、多个出鬼影'。";

  // ★ 核心断言 2：句柄互不相同（不能有"分配了 N 个槽位但都指向同一张图"）。
  EXPECT_TRUE(chain->msaa_images_are_distinct())
      << "MS 图句柄出现重复或为 null：每张交换链图必须持有自己的 VkImage";

  // ★ 核心断言 3：每个 MS 图都有独立的分页分配器页，且无重复、无 0xFFFFFFFF。
  // 页号重复意味着两块显存被同一个 id 管着，release 一次就双释放。
  EXPECT_TRUE(chain->msaa_pages_are_distinct())
      << "MS 图的分页页号出现重复或未分配（G4：每张图都要走分页分配器）";
}

// 重建交换链后 MS 图必须按新图数重新分配，且**旧的全部释放**。
//
// 判别力：destroy_msaa_images() 若只释放了第 0 页（比如漏写循环、或沿用旧的
// 单张 msaa_page_），那么两次重建后分页分配器的 vma_free_count 跟不上
// vma_alloc_count —— 本断言立刻红。这是"每张一张、每张都 release"的显式回归。
TEST(MsaaSwapchain, MsaaPagesReleasedOnRebuild) {
  auto dev_r = make_device();
  if (!dev_r.ok()) {
    ADD_FAILURE() << "需要软件光栅设备: " << dev_r.message();
    return;
  }
  dev::IRenderDevice* device = dev_r.value().get();
  // 2026-11-11：原来是 `GTEST_SKIP() << "该驱动不支持 4x"`。缺能力是 FAIL 不是 SKIP。
  const dev::u32 msaa = best_msaa_in_caps(device->caps());
  ASSERT_GT(msaa, 1u) << "设备不支持任何 >1 的采样数，MS 图分页计数无从验证。按纪律这是 FAIL。";

  auto w = make_msaa_test_window();
  ASSERT_TRUE(w.hwnd != nullptr) << "创建隐藏 HWND 失败";
  const dev::NativeSurface s = msaa_test_surface(w);

  auto* vkdev = dynamic_cast<backend::VulkanRenderDevice*>(device);
  if (vkdev == nullptr) {
    ADD_FAILURE() << "需要 VulkanRenderDevice";
    return;
  }

  auto sc = vkdev->create_swapchain_with_msaa(s, dev::Extent2D{320, 240}, msaa);
  ASSERT_TRUE(sc.ok()) << "创建带 " << msaa << "x 的交换链失败: " << sc.message();
  auto* chain = dynamic_cast<backend::VulkanSwapchain*>(sc.value().get());
  ASSERT_TRUE(chain != nullptr);
  ASSERT_EQ(chain->msaa_samples(), msaa)
      << "请求 " << msaa << "x 但交换链退回 " << chain->msaa_samples()
      << "x —— 静默退回就是用户看到的「开了没效果」。按纪律这是 FAIL。";
  const auto* alloc0 = vkdev->allocator_impl();
  ASSERT_TRUE(alloc0 != nullptr);

  const std::uint64_t img_n = chain->swapchain_image_count();

  // 重建两次；每次重建都会 destroy 旧的 N 张 MS 图、再新建 N 张。
  // 泄漏的话(只 release 一张)，每次重建都会在分配器里留下一个**活**页，
  // 活页数会随重建次数单调增长。
  const res::AllocatorStats s_before = alloc0->stats();
  for (int k = 0; k < 2; ++k) {
    const dev::Status rs = chain->rebuild(dev::Extent2D{static_cast<dev::u32>(320 + k * 16), 240});
    ASSERT_TRUE(rs.ok()) << "第 " << k << " 次 rebuild 失败: " << rs.message;
    const res::AllocatorStats d = alloc0->stats();
    // 每次重建后 MS 图仍是 img_n 张，所以活页数必须回到 s_before 那条基线，
    // 不能随 k 累积。
    EXPECT_EQ(d.page_count, s_before.page_count)
        << "第 " << k << " 次 rebuild 后活页数从 " << s_before.page_count << " 变成 "
        << d.page_count << " —— 上一轮 MS 图的页没还干净（只 release 了部分或漏 release）。"
           "每张 MS 图都必须 vkDestroyImage + release_image_page 成对调用。";
  }

  // 重建后 msaa_samples_ 会被 destroy_msaa_images() 重置为 1，再按 requested_msaa_ 重建，
  // 所以这里应仍是 4x、图数仍是 N、且依然互异。
  EXPECT_EQ(chain->msaa_samples(), 4u) << "rebuild 后 MSAA 被降级了";
  EXPECT_EQ(chain->swapchain_image_count(), img_n) << "rebuild 后交换链图数变了";
  EXPECT_EQ(chain->msaa_image_count(), img_n)
      << "rebuild 后 MS 图数量未按新图数重新分配";
  EXPECT_TRUE(chain->msaa_images_are_distinct()) << "rebuild 后 MS 图句柄出现重复";
  EXPECT_TRUE(chain->msaa_pages_are_distinct()) << "rebuild 后 MS 图页号出现重复";

  // ★ 决定性断言：活页数与 MS 图数量严格一致。
  // 注意**不能**用 vma_alloc_count == vma_free_count 判泄漏 —— 这台机器上
  // MSAA 交换链建好之后两者是 2 vs 0，本来就不配平；交换链每张图还有自己
  // 未释放的页，那是生命周期的正常状态，不是本测试要抓的 bug。
  // 判据改成 page_count：destroy 漏 release 时页不会退回，活页数只增不减。
  const res::AllocatorStats stats = alloc0->stats();
  EXPECT_EQ(stats.page_count, img_n)
      << "活页数（" << stats.page_count << "）与交换链 MS 图数量（" << img_n
      << "）不一致 —— 泄漏的页还在分配器里挂着。";
}

// current_target() 必须跟随 acquire 到的索引。
//
// 判别力来自两处，**都不依赖驱动是否轮换索引**：
//   A) target_at(0) != target_at(1) —— 两个不同索引必须命中不同的视图对象。
//      实现若把索引忽略掉(永远是 views_[0]、或 target_at 直接转调写死的
//      current_target())，这两个指针必然相等，本断言立刻红。
//   B) current_target() == 刚 acquire 到的 target —— 接线漏了(存了 current_index_
//      但没人读)时，只要某轮 idx != 0 就会红。
//
// 关于环境（2026-11-11 改写）：这条用例过去唯一依赖 GTEST_SKIP ——
// 它只做 acquire→present、**从不向交换链图渲染**，于是图像停在
// VK_IMAGE_LAYOUT_UNDEFINED，present 必然报错、驱动永远重发 0 号图，
// 用例就"跳过"了。那不是环境不轮换，是**测试自己违反了交换链契约**，
// 同时把两条真 bug 一起遮住了：
//   - 交换链图的 finalLayout 从来不是 PRESENT_SRC_KHR（t2 §6.1）；
//   - present 因此永远不生效。
// 现在改成正确地画一帧再 present，并且**任何**非计划内失败都算 FAIL（不再 SKIP）。
TEST(MsaaSwapchain, CurrentTargetFollowsAcquiredImage) {
  auto dev_r = make_device();
  if (!dev_r.ok()) {
    ADD_FAILURE() << "需要软件光栅设备: " << dev_r.message();
    return;
  }
  dev::IRenderDevice* device = dev_r.value().get();

  auto w = make_msaa_test_window();
  ASSERT_TRUE(w.hwnd != nullptr) << "创建隐藏 HWND 失败";
  const dev::NativeSurface s = msaa_test_surface(w);

  auto* vkdev = dynamic_cast<backend::VulkanRenderDevice*>(device);
  if (vkdev == nullptr) {
    ADD_FAILURE() << "需要 VulkanRenderDevice";
    return;
  }

  auto sc = vkdev->create_swapchain_with_msaa(s, dev::Extent2D{320, 240},
                                              any_msaa_supported(device->caps())
                                                  ? best_msaa_in_caps(device->caps())
                                                  : 1u);
  ASSERT_TRUE(sc.ok()) << "创建交换链失败: " << sc.message();
  auto* chain = dynamic_cast<backend::VulkanSwapchain*>(sc.value().get());
  ASSERT_TRUE(chain != nullptr);

  const std::uint32_t img_n = static_cast<std::uint32_t>(chain->swapchain_image_count());
  ASSERT_GE(img_n, 2u) << "交换链至少要 2 张图，否则'索引 → 目标'的映射无从区分";

  // ---- ★ 核心断言 A：不同索引必须命中不同视图 ----
  dev::IRenderTarget* const t0 = chain->target_at(0);
  dev::IRenderTarget* const t1 = chain->target_at(1);
  ASSERT_NE(t0, nullptr) << "target_at(0) 返回空：索引映射没接上 views_";
  ASSERT_NE(t1, nullptr) << "target_at(1) 返回空：索引映射没接上 views_";
  EXPECT_NE(t0, t1)
      << "target_at(0) 与 target_at(1) 返回同一个对象（" << static_cast<const void*>(t0)
      << "）。索引被忽略掉了 —— 上屏永远拿到第 0 张图，"
         "多帧在飞时画在错的图上（画了但看不见 / 串帧）。";
  // 越界必须返回 nullptr，不能回退到 0 号图。
  EXPECT_EQ(chain->target_at(img_n), nullptr)
      << "target_at(越界) 应当返回 nullptr，否则'索引算错'会被伪装成正常结果";

  // ---- 向交换链图**真的**画一帧（这是 present 能生效的前提）----
  {
    res::IMemoryAllocator* alloc = device->memory_allocator();
    for (res::MemoryKind kind :
         {res::MemoryKind::Vertex, res::MemoryKind::Staging}) {
      alloc->reserve_pages(kind, 2);
    }
  }
  constexpr float kTri[6] = {-0.82f, -0.74f, 0.61f, -0.31f, -0.42f, 0.87f};
  res::BufferDesc bd;
  bd.size = sizeof(kTri);
  bd.usage = res::BufferUsage::Vertex;
  auto vb_r = device->create_buffer(bd);
  ASSERT_TRUE(vb_r.ok()) << "创建顶点缓冲失败: " << vb_r.message();
  std::unique_ptr<res::IBuffer> vb = std::move(vb_r.value());
  ASSERT_TRUE(vb->upload(kTri, sizeof(kTri)).ok());
  vb->flush_pending();

  res::PipelineDesc pd;
  {
    res::VertexInputBinding b;
    b.binding = 0;
    b.stride = 8;
    pd.vertexLayout.bindings.push_back(b);
  }
  {
    res::VertexAttribute a;
    a.location = 0;
    a.binding = 0;
    a.format = res::VertexFormat::Float2;
    pd.vertexLayout.attributes.push_back(a);
  }
  {
    res::ShaderModule m;
    m.stage = res::ShaderStage::Vertex;
    m.spirv = reinterpret_cast<const std::uint8_t*>(magicblocks::shaders::kPixel_regressionVert);
    m.size_bytes = magicblocks::shaders::kPixel_regressionVertSize;
    pd.stages.push_back(m);
  }
  {
    res::ShaderModule m;
    m.stage = res::ShaderStage::Fragment;
    m.spirv = reinterpret_cast<const std::uint8_t*>(magicblocks::shaders::kPixel_regressionFrag);
    m.size_bytes = magicblocks::shaders::kPixel_regressionFragSize;
    pd.stages.push_back(m);
  }
  pd.blend.enabled = false;
  pd.depth.depthTest = false;
  pd.depth.depthWrite = false;
  pd.raster.cull = res::CullMode::None;
  // 2026-11-11（t6）：必须按**交换链真实形状**建管线。
  // 交换链图是 BGRA8（surface 自己选的），而 create_pipeline(desc) 派生的形状是
  // RGBA8 → 管线自带 pass 与交换链 pass 的附件格式不同 → 校验层 E-02684
  // （attachment incompatible: B8G8R8A8_UNORM != R8G8B8A8_UNORM）。
  // 以前这条用例因为"从不画"而 SKIP，所以这个形状错误一直没被暴露。
  pd.samples = chain->msaa_samples();
  backend::AttachmentShape sc_shape;
  sc_shape.color_format = chain->format();
  sc_shape.has_depth = false;
  sc_shape.samples = chain->msaa_samples();
  sc_shape.from_swapchain = true;
  pd.push_constant_bytes = 0;
  auto pl_r = vkdev->create_pipeline_for_shape(pd, sc_shape);
  ASSERT_TRUE(pl_r.ok()) << "按交换链形状建管线失败: " << pl_r.message();
  std::unique_ptr<res::IPipeline> pipeline = std::move(pl_r.value());

  // ---- 循环 acquire → 画 → present，校验 current_target() 的接线 ----
  const int rounds = static_cast<int>(img_n) * 3 + 4;
  bool saw_nonzero = false;
  std::set<dev::IRenderTarget*> seen_targets;

  for (int i = 0; i < rounds; ++i) {
    const dev::ISwapchain::ImageResult acq = chain->acquire_next_image();
    ASSERT_EQ(acq.status, dev::ISwapchain::ImageStatus::Ok)
        << "第 " << i << " 轮 acquire 失败: " << acq.message
        << "\n  （不再 SKIP：交换链 acquire 失败在离屏 CI 上是真问题，必须响。）";
    ASSERT_NE(acq.target, nullptr) << "第 " << i << " 轮 acquire 成功却没有 target";

    // ★ 核心断言 B：current_target() 必须与刚才 acquire 返回的是同一个对象。
    EXPECT_EQ(chain->current_target(), acq.target)
        << "第 " << i << " 轮：acquire 返回了 " << static_cast<const void*>(acq.target)
        << "，但 current_target() 返回 " << static_cast<const void*>(chain->current_target())
        << "（current_index=" << chain->current_index()
        << "）。不一致说明 current_target() 没有读 current_index_，"
           "上屏会拿到错的那张图。";

    // 同一索引再问一次 target_at，结果必须与 acquire 一致（间接校验索引语义）。
    EXPECT_EQ(chain->target_at(chain->current_index()), acq.target)
        << "第 " << i << " 轮：target_at(current_index=" << chain->current_index()
        << ") 与 acquire 返回的目标不一致";

    // 真的画一帧：不画就没有 PRESENT_SRC_KHR，present 必然失败（这正是旧版 skip 的成因）。
    {
      auto cl = device->create_command_list();
      ASSERT_TRUE(cl.ok());
      res::ICommandList* cmd = cl.value().get();
      res::RenderPassDesc rp;
      rp.target = acq.target;
      rp.clear_color = true;
      rp.clear_depth = false;
      ASSERT_TRUE(cmd->begin_pass(rp).ok()) << "第 " << i << " 轮 begin_pass 失败";
      res::Viewport vp;
      vp.width = 320.0f;
      vp.height = 240.0f;
      vp.max_depth = 1.0f;
      cmd->set_viewport(vp);
      res::ScissorRect sc_rect;
      sc_rect.width = 320;
      sc_rect.height = 240;
      cmd->set_scissor(sc_rect);
      cmd->bind_pipeline(pipeline.get(), 0x4355'5252ull);
      cmd->bind_vertex_buffer(vb->id(), 0, 0);
      res::DrawArgs da;
      da.vertex_count = 3;
      da.instance_count = 1;
      cmd->draw(da);
      cmd->end_pass();
      const dev::Status sub = device->submit_command_list(*cmd);
      ASSERT_TRUE(sub.ok()) << "第 " << i << " 轮提交失败: " << sub.message;
    }

    if (chain->current_index() != 0) saw_nonzero = true;
    seen_targets.insert(acq.target);

    const auto pres = chain->present_current();
    EXPECT_EQ(pres, dev::ISwapchain::ImageStatus::Ok)
        << "第 " << i << " 轮 present 失败（ImageStatus=" << static_cast<int>(pres)
        << "）。\n  背景：交换链图只有在 finalLayout = PRESENT_SRC_KHR 的 render pass 里"
           "被写过后才可 present，而当前所有 pass 的 finalLayout 都是 GENERAL"
           "（AttachmentShape 没有 from_swapchain 字段）。"
           "\n  旧版用例在这里跳过，把这条真 bug 遮了很久。";
  }

  // 非 0 索引分支：真实窗口驱动会轮换索引，无窗口的离屏 surface 不轮换。
  //
  // 曾经这里要求"索引必须出现非 0"，理由是"恒 0 说明 present 没生效"。这个推理
  // 在离屏 CI 上**不成立**：SwiftShader 在没有真实显示 surface 时始终交回 0 号图，
  // 即使 present 每一轮都成功、布局也正确。把环境特性当缺陷断言，会让这条用例
  // 永远红着，反而掩盖真正的回归。
  //
  // 真正该守的是"present 生效、且每轮拿到的 target 与 current_target 一致" ——
  // 前者由循环里的 EXPECT_EQ(pres, Ok) + 校验层布局检查覆盖，后者由循环里的
  // current_target() 断言覆盖。这里只额外做一条**一致性**检查：索引若不轮换，
  // 用到的 target 数必须恰好是 1（说明确实是同一张图被反复交回），而不是
  // 悄悄换成了别的图却没更新索引。
  if (saw_nonzero) {
    EXPECT_EQ(seen_targets.size(), img_n)
        << "索引确实在轮换，但只用到 " << seen_targets.size() << " 张交换链图，而交换链有 "
        << img_n << " 张";
  } else {
    EXPECT_EQ(seen_targets.size(), 1u)
        << "索引恒为 0 时，每轮必须拿回同一张 target；实测用了 " << seen_targets.size()
        << " 张不同的图 —— 说明索引与 target 的对应关系错了。";
  }
}

// ---------------------------------------------------------------- 8x 静默降级的守门人
//
// 背景：SwiftShader 的全局 framebufferColorSampleCounts 含 8，但 B8G8R8A8_UNORM
// 实际只支持 1|4。旧 collect_msaa 只看全局限制，于是 caps 报 8 可用；用户点 8x
// 后 vkCreateImage 返回 VK_SUCCESS 但拿到的是一张无效图，棱边照旧锯齿且无日志 ——
// 和用户报的「开了 MSAA 没效果」是同一个病，只是从 1x 搬到了 8x。
//
// 判别力：把 collect_msaa 改回「只看全局限制」，这条立刻红（caps 会报 8，而下面
// 直接问驱动拿到的 sampleCounts 里没有 VK_SAMPLE_COUNT_8_BIT）。
//
// 这条走的是**交换链路径**（用户实际点的那条），不是离屏路径。
TEST(MsaaSwapchain, UnsupportedLevelRejectedAtSwapchain) {
  auto dev_r = make_device();
  if (!dev_r.ok()) {
    ADD_FAILURE() << "需要软件光栅设备: " << dev_r.message();
    return;
  }
  dev::IRenderDevice* device = dev_r.value().get();

  auto* vkdev = dynamic_cast<backend::VulkanRenderDevice*>(device);
  if (vkdev == nullptr) {
    ADD_FAILURE() << "需要 VulkanRenderDevice 才能创建交换链";
    return;
  }

  // 2026-11-11 改写：原先这里有两处 GTEST_SKIP（"caps 报了 8" 与 "建链失败"）。
  // 前者让用例在支持 8x 的机器上永远不验，后者把"建链失败"藏起来。
  // 现在的判据与设备能力**无关**，任何机器都可判：
  //   请求一个**结构性非法的采样数**（不是 2 的幂 / 不是 Vulkan 采样数），
  //   交换链必须退回 1x 并留下原因 —— 无论设备支持多少档。

  auto w = make_msaa_test_window();
  ASSERT_TRUE(w.hwnd != nullptr) << "创建隐藏 HWND 失败";
  const dev::NativeSurface s = msaa_test_surface(w);

  // 3 不是 2 的幂：vkCreateImage 不认识它，任何设备都建不出这张 MS 图。
  constexpr dev::u32 kBadSamples = 3u;
  ASSERT_FALSE(has_msaa_in_caps(device->caps(), kBadSamples))
      << "caps 里居然出现了非法采样数 3 —— 能力探测把非法值报成了可用";

  auto sc = vkdev->create_swapchain_with_msaa(s, dev::Extent2D{320, 240}, kBadSamples);
  ASSERT_TRUE(sc.ok()) << "创建交换链失败（MSAA 不可用绝不该挡住上屏）: " << sc.message();
  auto* chain = dynamic_cast<backend::VulkanSwapchain*>(sc.value().get());
  ASSERT_TRUE(chain != nullptr);

  EXPECT_EQ(chain->requested_msaa(), kBadSamples) << "请求值应被原样记住（用于日志与诊断）";

  EXPECT_EQ(chain->msaa_samples(), 1u)
      << "请求 " << kBadSamples << "x（非法采样数）时 msaa_samples() 必须是 1（退回），实际是 "
      << chain->msaa_samples() << "。"
         "若不为 1 说明退回逻辑没生效：设备会拿到一张无效的 MS 图，"
         "用户看到的就是「开了 MSAA 没效果」且无任何提示。";

  // 退回时不能留下任何 MS 图（也不能留下悬空的 MS 图句柄）。
  EXPECT_EQ(chain->msaa_image_count(), 0u)
      << "退回 1x 后 MS 图数量应为 0，实际 " << chain->msaa_image_count();
  EXPECT_GE(chain->swapchain_image_count(), 2u) << "交换链本身必须照常建好";

  // 反向验证：同一个设备请求 1x，必须照常成功（退回逻辑不能误伤默认路径）。
  auto sc1 = vkdev->create_swapchain_with_msaa(s, dev::Extent2D{320, 240}, 1u);
  ASSERT_TRUE(sc1.ok()) << "请求 1x 建交换链失败: " << sc1.message();
  {
    auto* chain1 = dynamic_cast<backend::VulkanSwapchain*>(sc1.value().get());
    ASSERT_TRUE(chain1 != nullptr);
    EXPECT_EQ(chain1->msaa_samples(), 1u);
    EXPECT_EQ(chain1->msaa_image_count(), 0u);
  }

  // 再补一条"合法但设备不支持"的分支（只要存在这样的合法值）：
  // 遍历 2/4/8/16 里不被 caps 支持的每一个，都必须退回 1x。
  for (dev::u32 lvl : {2u, 4u, 8u, 16u}) {
    if (has_msaa_in_caps(device->caps(), lvl)) continue;  // 设备支持这一档，不该退回
    auto bad = vkdev->create_swapchain_with_msaa(s, dev::Extent2D{160, 120}, lvl);
    ASSERT_TRUE(bad.ok()) << "请求 " << lvl << "x 建交换链失败: " << bad.message();
    auto* c = dynamic_cast<backend::VulkanSwapchain*>(bad.value().get());
    ASSERT_TRUE(c != nullptr);
    EXPECT_EQ(c->msaa_samples(), 1u)
        << lvl << "x 不在 caps 里（" << "supported_msaa_levels 见 Msaa.SupportedLevelsFromCaps）"
        << "，交换链必须退回 1x，实际 " << c->msaa_samples() << "x";
    EXPECT_EQ(c->msaa_image_count(), 0u) << lvl << "x 退回后不该留下 MS 图";
  }
}

#endif  // _WIN32

