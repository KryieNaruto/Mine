// MagicBlocks L1 测试 —— spec §4 L1 R1~R6 / M1~M6、§6 内存设计、plans-l1 §6 用例表
//
// 纪律(spec §9 / 不回退原则):
//  - M5 用「计数差分」判稳态,不靠肉眼:C7 的效果必须可计量为 0/帧;
//  - M3 的句柄必须能真的 pack() 进命令流 —— 分配器产出的 offset 若没对齐到
//    64 KiB,pack() 会失败,这是硬判据而不是"看起来对";
//  - 每个用例都先跑一遍 allocator->validate(),内部不一致立刻红;
//  - 门控用例(G4)在 ctest 侧跑脚本,不在这里重复实现。
#include <gtest/gtest.h>

#include <array>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "magicblocks/device/render_device.h"
#include "magicblocks/resources/buffer.h"#include "magicblocks/resources/command.h"
#include "magicblocks/resources/handles.h"
#include "magicblocks/resources/memory.h"
#include "magicblocks/resources/pipeline.h"
#include "magicblocks/resources/texture.h"

// 测试要触达后端自检绘制与分页分配器的具体类型(同 L0 测试的做法)。
#include "backend/vulkan/vulkan_allocator.h"
#include "backend/vulkan/vulkan_command.h"
#include "backend/vulkan/vulkan_pipeline.h"
#include "backend/vulkan/vulkan_target.h"
// 构建期由 shaders/embed_shaders.py 生成的引擎内嵌 SPIR-V(R6 通路)。
#include "magicblocks_shaders.h"

namespace mb = magicblocks;
namespace dev = magicblocks::device;
namespace res = magicblocks::resources;

namespace {

bool is_software_name(const std::string& n) {
  return n.find("SwiftShader") != std::string::npos || n.find("llvmpipe") != std::string::npos ||
         n.find("lavapipe") != std::string::npos;
}

dev::DeviceDesc sw_desc() {
  dev::DeviceDesc d;
  d.app_name = "MagicBlocks L1 Test";
  d.require_software_rasterizer = true;
  return d;
}

// L1 用例统一入口:仍是软件光栅(SwiftShader 强筛,绝不回退物理 GPU)。
std::unique_ptr<dev::IRenderDevice> require_sw_device() {
  auto r = dev::create_render_device(sw_desc());
  if (!r.ok()) {
    ADD_FAILURE() << "创建设备失败: " << r.message();
    return nullptr;
  }
  std::unique_ptr<dev::IRenderDevice> d = r.take();
  if (!is_software_name(d->caps().device_name)) {
    ADD_FAILURE() << "要求软件光栅设备,实际 deviceName=\"" << d->caps().device_name << "\"";
    return nullptr;
  }
  return d;
}

// 一段最小的"合法图形管线描述"。
//
// SPIR-V 必须来自真实编译产物:凭手写"看起来像头"的字节骗 vkCreateShaderModule,
// SwiftShader 会直接崩在测试进程里(实测 SEH 0xc0000005 / "Id is 0")。
// 这里直接用 build 时由 shaders/embed_shaders.py 生成的引擎内嵌着色器(R6 的通路),
// L1 只校验管线对象与光栅/深度状态映射,不重复验证着色器本身。
res::PipelineDesc graphics_desc() {
  res::PipelineDesc p;
  p.stages.push_back({res::ShaderStage::Vertex,
                      reinterpret_cast<const std::uint8_t*>(mb::shaders::kFullscreenVert),
                      mb::shaders::kFullscreenVertSize});
  p.stages.push_back({res::ShaderStage::Fragment,
                      reinterpret_cast<const std::uint8_t*>(mb::shaders::kFullscreenFrag),
                      mb::shaders::kFullscreenFragSize});
  p.vertexLayout.bindings.push_back({0, 12, false});
  p.vertexLayout.attributes.push_back({0, 0, res::VertexFormat::Float3, 0});
  return p;
}

}  // namespace

// ================================================================ M1 / M2 分配器

// M2:同桶连续分配落在同一页,offset 互不重叠;M3:句柄可直接 pack 进命令流。
TEST(Allocator, AllocatesFromSamePageWithDistinctOffsets) {
  auto d = require_sw_device();
  if (!d) return;
  res::IMemoryAllocator* a = d->memory_allocator();
  ASSERT_NE(a, nullptr);
  ASSERT_TRUE(a->reserve_pages(res::MemoryKind::Vertex, 1).ok());

  auto r1 = a->allocate(res::MemoryKind::Vertex, 1024, 256);
  auto r2 = a->allocate(res::MemoryKind::Vertex, 2048, 256);
  ASSERT_TRUE(r1.ok()) << r1.message();
  ASSERT_TRUE(r2.ok()) << r2.message();

  const res::ResourceId i1 = r1.value().id;
  const res::ResourceId i2 = r2.value().id;
  EXPECT_TRUE(i1.valid());
  EXPECT_TRUE(i2.valid());
  // 同页复用:C7 的核心承诺 —— 小资源不各占一块显存。
  EXPECT_EQ(i1.page_id, i2.page_id) << "同桶分配应落在同一大页";
  EXPECT_NE(i1.offset, i2.offset) << "offset 必须互不重叠";
  EXPECT_NE(i1.id, i2.id);

  // M3 裁定:pack() 要求 offset % 64 KiB == 0 —— 分配器必须保证这一点。
  dev::u64 packed = 0;
  EXPECT_TRUE(i1.pack(&packed)) << "分配器产出的句柄必须能 pack,offset=" << i1.offset;
  EXPECT_TRUE(i2.pack(&packed)) << "分配器产出的句柄必须能 pack,offset=" << i2.offset;

  EXPECT_TRUE(a->validate().ok()) << a->validate().message;
}

// M4:free 只把子块还回本页并 O(1) 合并,不立即退回 VMA。
TEST(Allocator, FreeKeepsPageAndMergesNeighbours) {
  auto d = require_sw_device();
  if (!d) return;
  res::IMemoryAllocator* a = d->memory_allocator();
  ASSERT_NE(a, nullptr);
  ASSERT_TRUE(a->reserve_pages(res::MemoryKind::Index, 1).ok());

  const res::AllocatorStats before = a->stats();
  auto r1 = a->allocate(res::MemoryKind::Index, 4096, 64);
  auto r2 = a->allocate(res::MemoryKind::Index, 4096, 64);
  ASSERT_TRUE(r1.ok()) << r1.message();
  ASSERT_TRUE(r2.ok()) << r2.message();
  const dev::u32 page = r1.value().id.page_id;

  EXPECT_TRUE(a->free(r1.value().id).ok());
  EXPECT_TRUE(a->free(r2.value().id).ok());

  const res::AllocatorStats after = a->stats();
  // 页还在:free 不触发 VMA 退回(M4 稳定驻留)。
  EXPECT_EQ(after.vma_free_count, before.vma_free_count) << "free 不得立即退回 VMA";
  EXPECT_GT(after.merge_count, before.merge_count) << "相邻空闲块应被合并";

  res::PageDesc pd{};
  ASSERT_TRUE(a->page_desc(page, &pd));
  EXPECT_EQ(pd.live_allocations, 0u);
  EXPECT_EQ(pd.free_block_count, 1u) << "两个相邻块释放后应合并成一个空闲块";

  // 释放后同一位置应可再次分配(复用而不是新开页)。
  auto r3 = a->allocate(res::MemoryKind::Index, 4096, 64);
  ASSERT_TRUE(r3.ok()) << r3.message();
  EXPECT_EQ(r3.value().id.page_id, page) << "释放的空间应被复用,不得新开页";

  EXPECT_TRUE(a->validate().ok()) << a->validate().message;
}

// M5 / C7:稳态帧的 VMA alloc/free 增量必须恰好为 0。
TEST(Allocator, SteadyStateFramesHaveZeroVmaCalls) {
  auto d = require_sw_device();
  if (!d) return;
  res::IMemoryAllocator* a = d->memory_allocator();
  ASSERT_NE(a, nullptr);
  // 稳态 = 页已预取完毕、每帧都做同样一套分配/释放。
  ASSERT_TRUE(a->reserve_pages(res::MemoryKind::Vertex, 1).ok());
  ASSERT_TRUE(a->reserve_pages(res::MemoryKind::Uniform, 1).ok());

  auto one_frame = [&](dev::u64 frame) {
    a->begin_frame(frame);
    std::vector<res::ResourceId> ids;
    for (int i = 0; i < 8; ++i) {
      auto r = a->allocate(res::MemoryKind::Vertex, 1024, 64);
      if (!r.ok()) {
        ADD_FAILURE() << "帧内分配失败: " << r.message();
        return;
      }
      ids.push_back(r.value().id);
    }
    auto u = a->allocate(res::MemoryKind::Uniform, 256, 64);
    if (u.ok()) ids.push_back(u.value().id);
    for (const res::ResourceId& id : ids) EXPECT_TRUE(a->free(id).ok());
    EXPECT_TRUE(a->trim().ok());
  };

  // 暖机 3 帧把页/桶稳定下来,再取差分 —— 否则测的是"首次分配"而不是稳态。
  for (dev::u64 f = 0; f < 3; ++f) one_frame(f);

  const res::AllocatorStats before = a->stats();
  for (dev::u64 f = 3; f < 33; ++f) one_frame(f);
  const res::AllocatorStats after = a->stats();

  EXPECT_EQ(after.vma_alloc_count - before.vma_alloc_count, 0u)
      << "稳态帧出现了 VMA 分配(C7 违规)";
  EXPECT_EQ(after.vma_free_count - before.vma_free_count, 0u)
      << "稳态帧出现了 VMA 释放(M4 违规)";
  EXPECT_GT(after.allocation_count - before.allocation_count, 0u) << "确实发生了子块分配";
  EXPECT_TRUE(a->validate().ok()) << a->validate().message;
}

// M4:页空置超过 page_idle_frames() 后才整页退回 VMA。
TEST(Allocator, IdlePageEvictedAfterIdleFrames) {
  auto d = require_sw_device();
  if (!d) return;
  res::IMemoryAllocator* a = d->memory_allocator();
  ASSERT_NE(a, nullptr);
  ASSERT_TRUE(a->reserve_pages(res::MemoryKind::Texture, 2).ok());

  auto r = a->allocate(res::MemoryKind::Texture, 4096, 64);
  ASSERT_TRUE(r.ok()) << r.message();
  const dev::u32 page = r.value().id.page_id;
  ASSERT_TRUE(a->free(r.value().id).ok());

  const dev::u32 idle_frames = a->page_idle_frames();
  EXPECT_EQ(idle_frames, 120u) << "spec §6.2-4:默认空置 120 帧才退回";

  // begin_frame 自己会推进空置计数并在帧边界顺手 trim()(spec §6.2-5),
  // 所以这里只推进帧,不再额外 trim。
  // 阈值语义是「空置帧数 >= 120 才退回」,因此第 1..119 帧之后都必须还在。
  for (dev::u32 f = 1; f < idle_frames; ++f) {
    a->begin_frame(f);
    res::PageDesc probe{};
    ASSERT_TRUE(a->page_desc(page, &probe))
        << "第 " << f << " 帧:未到阈值(" << idle_frames << ")就淘汰了这页";
  }

  // 越过阈值:整页退回。
  const dev::u64 evict_before = a->stats().evict_count;
  a->begin_frame(idle_frames + 2);
  ASSERT_TRUE(a->trim().ok());
  EXPECT_GT(a->stats().evict_count, evict_before) << "超过空置阈值应整页退回 VMA";

  res::PageDesc pd{};
  EXPECT_FALSE(a->page_desc(page, &pd)) << "已淘汰的页不应还登记在册";
  EXPECT_TRUE(a->validate().ok()) << a->validate().message;
}

// M6:超预算 → OutOfMemory,且 message 带「需要 / 已有 / 预算」三个数。
TEST(Allocator, OverBudgetFailsWithDetailedMessage) {
  auto d = require_sw_device();
  if (!d) return;
  res::IMemoryAllocator* a = d->memory_allocator();
  ASSERT_NE(a, nullptr);
  ASSERT_TRUE(a->reserve_pages(res::MemoryKind::Vertex, 1).ok());

  auto tiny = a->allocate(res::MemoryKind::Vertex, 1024, 64);
  ASSERT_TRUE(tiny.ok()) << tiny.message();
  ASSERT_TRUE(a->free(tiny.value().id).ok());

  // 把预算压到很小,再要一块明显超出的子块。
  const dev::u64 real_budget = a->memory_budget();
  a->set_memory_budget(1024);

  auto big = a->allocate(res::MemoryKind::Vertex, 64ull * 1024ull * 1024ull, 64);
  EXPECT_FALSE(big.ok()) << "超预算必须失败,不能静默降级";
  if (!big.ok()) {
    EXPECT_EQ(big.status().code, dev::StatusCode::OutOfMemory);
    const std::string& m = big.message();
    EXPECT_NE(m.find("需要"), std::string::npos) << "message=" << m;
    EXPECT_NE(m.find("已有"), std::string::npos) << "message=" << m;
    EXPECT_NE(m.find("预算"), std::string::npos) << "message=" << m;
  }

  a->set_memory_budget(real_budget);
  EXPECT_TRUE(a->validate().ok()) << a->validate().message;
}

// ================================================================ M3 句柄

TEST(Handle, PackRoundTripAndRejectsMisalignment) {
  res::ResourceId id{};
  id.page_id = 3;
  id.offset = 4ull * 65536ull;  // 256 KiB = 4 个对齐单位
  id.size = 4096;
  id.id = 7;

  dev::u64 packed = 0;
  ASSERT_TRUE(id.pack(&packed));

  const res::ResourceId back = res::ResourceId::unpack(packed);
  EXPECT_EQ(back.page_id, id.page_id);
  EXPECT_EQ(back.offset, id.offset);
  EXPECT_EQ(back.id, id.id);
  // size 不参与打包(由资源表反查),解包后恒为 0。
  EXPECT_EQ(back.size, 0u);

  // 未对齐到 64 KiB 的偏移必须被拒绝,而不是静默截断。
  res::ResourceId bad = id;
  bad.offset = 100;
  dev::u64 ignored = 0;
  EXPECT_FALSE(bad.pack(&ignored)) << "offset 未对齐 64 KiB 必须拒绝打包";
}

// ================================================================ R1 buffer

TEST(Buffer, HandleIsOffsetIntoAllocatorPage) {
  auto d = require_sw_device();
  if (!d) return;
  res::IMemoryAllocator* a = d->memory_allocator();
  ASSERT_NE(a, nullptr);
  ASSERT_TRUE(a->reserve_pages(res::MemoryKind::Vertex, 1).ok());

  res::BufferDesc bd;
  bd.size = 4096;
  bd.usage = res::BufferUsage::Vertex;
  auto b = d->create_buffer(bd);
  ASSERT_TRUE(b.ok()) << b.message();

  const res::ResourceId id = b.value()->id();
  EXPECT_TRUE(id.valid());
  res::PageDesc pd{};
  ASSERT_TRUE(a->page_desc(id.page_id, &pd)) << "缓冲句柄必须指向分配器管理的页";
  EXPECT_EQ(pd.kind, res::PageKind::Device);
  EXPECT_GT(pd.live_allocations, 0u);

  dev::u64 packed = 0;
  EXPECT_TRUE(id.pack(&packed)) << "缓冲句柄必须可进命令流";
  EXPECT_EQ(b.value()->allocation_kind(), res::MemoryKind::Vertex);
  EXPECT_GE(b.value()->size(), 4096u);

  // 设备桶不可从 CPU 映射 —— 必须明确返回 nullptr,不是"碰巧能用"。
  EXPECT_EQ(b.value()->mapped(), nullptr);
  const std::uint8_t one_byte = 0xAB;
  EXPECT_EQ(b.value()->write(&one_byte, 0, 1).code, dev::StatusCode::Unsupported)
      << "设备页缓冲的 CPU 写必须是 Unsupported(引导用 upload),不是静默成功";
}

TEST(Buffer, StagingBufferRoundTripsThroughCpu) {
  auto d = require_sw_device();
  if (!d) return;
  res::IMemoryAllocator* a = d->memory_allocator();
  ASSERT_NE(a, nullptr);
  ASSERT_TRUE(a->reserve_pages(res::MemoryKind::Staging, 1).ok());

  res::BufferDesc bd;
  bd.size = 256;
  bd.usage = res::BufferUsage::Staging;
  bd.persistent_map = true;
  auto br = d->create_buffer(bd);
  ASSERT_TRUE(br.ok()) << br.message();
  res::IBuffer* b = br.value().get();

  // Staging 落 HostVisible 页(裁定 2),mapped() 必须可用。
  EXPECT_EQ(b->allocation_kind(), res::MemoryKind::Staging);
  EXPECT_EQ(res::page_kind_of(b->allocation_kind()), res::PageKind::HostVisible);
  ASSERT_NE(b->mapped(), nullptr) << "Staging 缓冲必须可 CPU 映射";

  std::array<std::uint8_t, 256> src{};
  for (std::size_t i = 0; i < src.size(); ++i) src[i] = static_cast<std::uint8_t>(i * 3 + 1);
  ASSERT_TRUE(b->write(src.data(), 0, src.size()).ok());

  std::array<std::uint8_t, 256> dst{};
  ASSERT_TRUE(b->read(dst.data(), 0, dst.size()).ok());
  EXPECT_EQ(std::memcmp(src.data(), dst.data(), src.size()), 0) << "写入/读回必须逐字节一致";

  // 越界写必须显式报错。
  EXPECT_EQ(b->write(src.data(), 200, 100).code, dev::StatusCode::InvalidArgument);
}

// ================================================================ R1 texture

TEST(Texture, CreateUploadAndReadbackRoundTrip) {
  auto d = require_sw_device();
  if (!d) return;
  res::IMemoryAllocator* a = d->memory_allocator();
  ASSERT_NE(a, nullptr);
  ASSERT_TRUE(a->reserve_pages(res::MemoryKind::Texture, 1).ok());

  res::TextureDesc td;
  td.extent = {4, 3};
  td.format = dev::Format::RGBA8_UNORM;
  td.usage = res::TextureUsage::Sampled;
  auto tr = d->create_texture(td);
  ASSERT_TRUE(tr.ok()) << tr.message();
  res::ITexture* t = tr.value().get();

  EXPECT_EQ(t->allocation_kind(), res::MemoryKind::Texture);
  EXPECT_EQ(t->size(), 4u * 3u * 4u);
  res::PageDesc pd{};
  ASSERT_TRUE(a->page_desc(t->id().page_id, &pd)) << "纹理句柄必须指向分配器管理的页";
  EXPECT_EQ(pd.kind, res::PageKind::HostVisible) << "裁定 2:纹理落 HostVisible 页";

  std::array<std::uint8_t, 48> pixels{};
  for (std::size_t i = 0; i < pixels.size(); ++i) pixels[i] = static_cast<std::uint8_t>(i * 5 + 7);
  ASSERT_TRUE(t->upload(pixels.data(), pixels.size(), 0).ok());

  // 设备态读回要从 Staging 桶切一块 CPU 可见缓冲(R5),分配器不会自己触发 VMA,
  // 所以调用方(未来是 L2 帧调度)必须先 reserve_pages。
  ASSERT_TRUE(a->reserve_pages(res::MemoryKind::Staging, 1).ok());

  std::array<std::uint8_t, 48> back{};
  const dev::Status rb = t->readback_rgba8(back.data(), back.size());
  ASSERT_TRUE(rb.ok()) << rb.message;
  EXPECT_EQ(std::memcmp(pixels.data(), back.data(), pixels.size()), 0);
}

TEST(Texture, RejectsInvalidDescAndDepthReadback) {
  auto d = require_sw_device();
  if (!d) return;
  res::IMemoryAllocator* a = d->memory_allocator();
  ASSERT_NE(a, nullptr);
  ASSERT_TRUE(a->reserve_pages(res::MemoryKind::Texture, 1).ok());

  res::TextureDesc zero;
  zero.extent = {0, 4};
  auto bad = d->create_texture(zero);
  EXPECT_FALSE(bad.ok()) << "extent 为 0 必须拒绝";
  EXPECT_EQ(bad.status().code, dev::StatusCode::InvalidArgument);

  // 深度格式的按 RGBA8 读回必须明确 Unsupported,不能返回垃圾。
  res::TextureDesc depth;
  depth.extent = {2, 2};
  depth.format = dev::Format::D16_UNORM;
  depth.usage = res::TextureUsage::Sampled;
  auto tr = d->create_texture(depth);
  if (tr.ok()) {
    std::array<std::uint8_t, 16> buf{};
    EXPECT_EQ(tr.value()->readback_rgba8(buf.data(), buf.size()).code,
              dev::StatusCode::Unsupported);
  }
}

// ================================================================ R1 sampler

TEST(Sampler, CreatesValidAndRejectsInvalidFilter) {
  auto d = require_sw_device();
  if (!d) return;

  res::SamplerDesc sd;
  sd.min_filter = res::FilterMode::Linear;
  sd.mag_filter = res::FilterMode::Nearest;
  sd.wrap_u = res::WrapMode::Repeat;
  auto sr = d->create_sampler(sd);
  ASSERT_TRUE(sr.ok()) << sr.message();
  res::ISampler* s = sr.value().get();
  // 采样器不占分配器页(它没有显存),身份用槽位号表达,slot_index 从 1 起发。
  EXPECT_GT(s->slot_index(), 0u) << "采样器必须有非 0 槽位号";
  EXPECT_EQ(s->desc().mag_filter, res::FilterMode::Nearest);
  EXPECT_EQ(s->desc().wrap_u, res::WrapMode::Repeat);

  // 非法枚举值(fill 整字节造一个不存在的 filter)。
  res::SamplerDesc bad;
  bad.min_filter = static_cast<res::FilterMode>(200);
  auto bs = d->create_sampler(bad);
  EXPECT_FALSE(bs.ok()) << "非法 filter 必须拒绝,不得就近取合法值";
}

// ================================================================ R2 pipeline

TEST(Pipeline, CreateGraphicsPipeline) {
  auto d = require_sw_device();
  if (!d) return;
  auto p = d->create_pipeline(graphics_desc());
  ASSERT_TRUE(p.ok()) << p.message();
  EXPECT_FALSE(p.value()->is_compute());
  EXPECT_GT(p.value()->hash(), 0u) << "管线必须有稳定的内部缓存句柄";
}

TEST(Pipeline, RejectsEmptyStages) {
  auto d = require_sw_device();
  if (!d) return;
  res::PipelineDesc p;
  auto r = d->create_pipeline(p);
  EXPECT_FALSE(r.ok()) << "stages 为空必须拒绝";
  EXPECT_EQ(r.status().code, dev::StatusCode::InvalidArgument);
}

// P12:深度状态默认「测试 + 写」,且带常量深度写能力。
TEST(Pipeline, DepthStateDefaultsAndConstWrite) {
  res::DepthState ds;
  EXPECT_TRUE(ds.depthTest);
  EXPECT_TRUE(ds.depthWrite);
  EXPECT_EQ(ds.depthOp, res::CompareOp::Less);
  EXPECT_FALSE(ds.depthConstWrite);

  auto d = require_sw_device();
  if (!d) return;
  res::PipelineDesc pd = graphics_desc();
  pd.depth.depthConstWrite = true;
  pd.depth.depth_const = 0.25f;
  auto p = d->create_pipeline(pd);
  ASSERT_TRUE(p.ok()) << p.message();
  EXPECT_TRUE(p.value()->desc().depth.depthConstWrite);
  EXPECT_FLOAT_EQ(p.value()->desc().depth.depth_const, 0.25f);
  // P12 让 depthConstWrite 真正生效,不是只存字段。
  auto* vp = dynamic_cast<magicblocks::backend::VulkanPipeline*>(p.value().get());
  ASSERT_NE(vp, nullptr);
  EXPECT_TRUE(vp->effective_depth_state().depthConstWrite);
}

// R4:本轮不建描述符集,数据只走 push constant。
TEST(Pipeline, UsesPushConstantsOnly) {
  auto d = require_sw_device();
  if (!d) return;
  res::PipelineDesc pd = graphics_desc();
  pd.push_constant_bytes = 64;
  auto p = d->create_pipeline(pd);
  ASSERT_TRUE(p.ok()) << p.message();
  EXPECT_EQ(p.value()->descriptor_set_count(), 0u) << "R4:本轮不建描述符集";
  EXPECT_EQ(p.value()->push_constant_bytes(), 64u);

  // 超出设备上限必须显式报错。
  res::PipelineDesc big = graphics_desc();
  big.push_constant_bytes = d->caps().max_push_constant_size + 4;
  auto pb = d->create_pipeline(big);
  EXPECT_FALSE(pb.ok()) << "push constant 超上限必须拒绝";
}

// R2/G1:光栅状态里没有线框多边形模式的位置 —— 管线恒为 FILL。
TEST(Pipeline, PolygonModeIsAlwaysFill) {
  auto d = require_sw_device();
  if (!d) return;
  auto p = d->create_pipeline(graphics_desc());
  ASSERT_TRUE(p.ok()) << p.message();
  auto* vp = dynamic_cast<magicblocks::backend::VulkanPipeline*>(p.value().get());
  ASSERT_NE(vp, nullptr);
  EXPECT_EQ(magicblocks::backend::VulkanPipeline::polygon_mode(), VK_POLYGON_MODE_FILL)
      << "spec R2 不支持线框模式,后端不得出现其他 polygonMode";
}

// ================================================================ R3 command list

// R3:11 类命令各录一次 → command_count() == 11(裁定:命令数是 11 而非 10)。
TEST(Command, RecordsAllOps) {
  auto d = require_sw_device();
  if (!d) return;
  res::IMemoryAllocator* a = d->memory_allocator();
  ASSERT_NE(a, nullptr);
  ASSERT_TRUE(a->reserve_pages(res::MemoryKind::Vertex, 1).ok());
  ASSERT_TRUE(a->reserve_pages(res::MemoryKind::Index, 1).ok());
  ASSERT_TRUE(a->reserve_pages(res::MemoryKind::Uniform, 1).ok());

  // push constant 必须落在管线声明的区间内,否则是（正确的）InvalidArgument。
  // 所以这里显式要一段 push constant 空间。
  res::PipelineDesc pd = graphics_desc();
  pd.push_constant_bytes = 64;
  auto pr = d->create_pipeline(pd);
  ASSERT_TRUE(pr.ok()) << pr.message();
  res::IPipeline* pl = pr.value().get();
  res::BufferDesc vbd;
  vbd.size = 4096;
  vbd.usage = res::BufferUsage::Vertex;
  auto vb = d->create_buffer(vbd);
  ASSERT_TRUE(vb.ok()) << vb.message();

  res::BufferDesc ibd;
  ibd.size = 4096;
  ibd.usage = res::BufferUsage::Index;
  auto ib = d->create_buffer(ibd);
  ASSERT_TRUE(ib.ok()) << ib.message();

  dev::TargetDesc td;
  td.kind = dev::TargetDesc::Kind::Offscreen;
  td.extent = {16, 16};
  td.with_depth = false;
  auto tgt = d->create_offscreen_target(td);
  ASSERT_TRUE(tgt.ok()) << tgt.message();

  auto cr = d->create_command_list();
  ASSERT_TRUE(cr.ok()) << cr.message();
  res::ICommandList* cl = cr.value().get();
  EXPECT_FALSE(cl->recording());
  EXPECT_FALSE(cl->in_pass());

  res::RenderPassDesc rp;
  rp.target = tgt.value().get();
  rp.clear_color = true;
  ASSERT_TRUE(cl->begin_pass(rp).ok());
  EXPECT_TRUE(cl->recording());
  EXPECT_TRUE(cl->in_pass());

  ASSERT_TRUE(cl->bind_pipeline(pl, pl->hash()).ok());
  ASSERT_TRUE(cl->bind_vertex_buffer(vb.value()->id(), 0, 0).ok());
  ASSERT_TRUE(cl->bind_index_buffer(ib.value()->id(), res::IndexFormat::UInt16, 0).ok());

  res::DrawArgs da;
  da.vertex_count = 3;
  ASSERT_TRUE(cl->draw(da).ok());

  res::DrawArgs di;
  di.index_count = 3;
  ASSERT_TRUE(cl->draw_indexed(di).ok());

  res::DrawArgs din;
  din.vertex_count = 3;
  din.instance_count = 4;
  ASSERT_TRUE(cl->draw_instanced(din).ok());

  const float color[4] = {1.0f, 0.0f, 0.0f, 1.0f};
  ASSERT_TRUE(cl->push_constants(0x1u, 0, sizeof(color), color).ok());

  res::Viewport vpt;
  vpt.width = 16.0f;
  vpt.height = 16.0f;
  vpt.max_depth = 1.0f;
  ASSERT_TRUE(cl->set_viewport(vpt).ok());

  res::ScissorRect sc;
  sc.width = 16;
  sc.height = 16;
  ASSERT_TRUE(cl->set_scissor(sc).ok());

  ASSERT_TRUE(cl->end_pass().ok());
  EXPECT_FALSE(cl->in_pass());

  // 裁定:11 类命令(begin_pass/end_pass 各算一类)。
  EXPECT_EQ(cl->command_count(), 11u) << "R3 命令数必须是 11";

  auto* vcl = dynamic_cast<magicblocks::backend::VulkanCommandList*>(cl);
  ASSERT_NE(vcl, nullptr);
  EXPECT_TRUE(vcl->all_resource_handles_packed()) << "命令流里的句柄必须都能 pack";
}

// R3:录制期校验 —— 未 bind_pipeline 就 draw、未 begin_pass 就 draw 都要报错。
TEST(Command, RejectsInvalidRecordingOrder) {
  auto d = require_sw_device();
  if (!d) return;
  auto cr = d->create_command_list();
  ASSERT_TRUE(cr.ok()) << cr.message();
  res::ICommandList* cl = cr.value().get();

  res::DrawArgs da;
  da.vertex_count = 3;
  // 未 begin_pass:拒绝。
  EXPECT_EQ(cl->draw(da).code, dev::StatusCode::InvalidArgument);
  EXPECT_EQ(cl->end_pass().code, dev::StatusCode::InvalidArgument);
  EXPECT_EQ(cl->command_count(), 0u) << "失败的录制不得计入命令数";

  // 不提交是合法的:此时并没有"半个 pass"(根本没开 pass)。
  EXPECT_TRUE(cl->submit_and_wait().ok()) << "空列表提交不该报错";

  // 开了 pass 却不 end_pass 就提交:必须拒绝 —— 这才是"半个 pass"。
  dev::TargetDesc td;
  td.kind = dev::TargetDesc::Kind::Offscreen;
  td.extent = {8, 8};
  td.with_depth = false;
  auto tgt = d->create_offscreen_target(td);
  ASSERT_TRUE(tgt.ok()) << tgt.message();
  res::RenderPassDesc rp;
  rp.target = tgt.value().get();
  ASSERT_TRUE(cl->begin_pass(rp).ok());
  EXPECT_EQ(cl->submit_and_wait().code, dev::StatusCode::InvalidArgument)
      << "未 end_pass 就提交必须拒绝";
}

// ================================================================ R5 上传路径

// R5:上传经由 staging 桶,不是每次上传都新开一块显存。
TEST(Upload, StagingGoesThroughAllocator) {
  auto d = require_sw_device();
  if (!d) return;
  res::IMemoryAllocator* a = d->memory_allocator();
  ASSERT_NE(a, nullptr);
  ASSERT_TRUE(a->reserve_pages(res::MemoryKind::Staging, 1).ok());
  ASSERT_TRUE(a->reserve_pages(res::MemoryKind::Texture, 1).ok());

  res::TextureDesc td;
  td.extent = {2, 2};
  td.format = dev::Format::RGBA8_UNORM;
  auto tr = d->create_texture(td);
  ASSERT_TRUE(tr.ok()) << tr.message();
  res::ITexture* t = tr.value().get();

  const res::AllocatorStats before = a->stats();
  std::array<std::uint8_t, 16> px{};
  for (int i = 0; i < 8; ++i) {
    ASSERT_TRUE(t->upload(px.data(), px.size(), 0).ok());
  }
  const res::AllocatorStats after = a->stats();

  // 多次上传应按桶复用,而不是线性堆出 N 块新显存。
  const dev::u64 new_pages = after.vma_alloc_count - before.vma_alloc_count;
  EXPECT_EQ(new_pages, 0u) << "8 次上传不应新开 vma 页(应由 staging 桶复用)";
  EXPECT_TRUE(a->validate().ok()) << a->validate().message;
}

// ================================================================ 整体一致性

// L1 收口:设备销毁前分配器自检必须干净,且活子块可全部回收。
TEST(Allocator, DeviceTeardownLeavesConsistentState) {
  auto d = require_sw_device();
  if (!d) return;
  res::IMemoryAllocator* a = d->memory_allocator();
  ASSERT_NE(a, nullptr);
  ASSERT_TRUE(a->reserve_pages(res::MemoryKind::Vertex, 1).ok());

  const dev::u32 before_pages = a->page_count(res::MemoryKind::Vertex);
  std::vector<res::ResourceId> ids;
  for (int i = 0; i < 32; ++i) {
    auto r = a->allocate(res::MemoryKind::Vertex, 512, 64);
    ASSERT_TRUE(r.ok()) << r.message();
    ids.push_back(r.value().id);
  }
  EXPECT_EQ(a->page_count(res::MemoryKind::Vertex), before_pages)
      << "32 个小缓冲应全落在已预取的同一页里";
  for (const res::ResourceId& id : ids) EXPECT_TRUE(a->free(id).ok());

  EXPECT_TRUE(a->validate().ok()) << a->validate().message;
}

// ================================================================ P0 纹理采样(X1)
//
// 裁定: ICommandList 加第 12 个方法 bind_sampled_texture(texture, sampler, slot);
// PipelineDesc 加 u32 sampled_texture_slots = 0;默认 0 时既有行为与 golden 逐字节不变。

namespace {

// 一条声明了 n 个采样纹理槽的图形管线。
res::PipelineDesc textured_desc(res::u32 slots) {
  res::PipelineDesc p = graphics_desc();
  p.sampled_texture_slots = slots;
  return p;
}

// 一张 4×4 RGBA8 的可采样纹理描述(P0 用例共用)。
res::TextureDesc unit_tex() {
  res::TextureDesc td;
  td.extent = {4, 4};
  td.format = dev::Format::RGBA8_UNORM;
  td.usage = res::TextureUsage::Sampled;
  return td;
}

}  // namespace

// P0-1:默认 sampled_texture_slots == 0 → 不建描述符集,既有行为不变。
TEST(PipelineSampledTexture, DefaultSlotCountKeepsZeroDescriptorSets) {
  auto d = require_sw_device();
  if (!d) return;

  res::PipelineDesc def = graphics_desc();  // 不给 sampled_texture_slots(默认 0)
  EXPECT_EQ(def.sampled_texture_slots, 0u) << "默认值必须是 0";

  auto p = d->create_pipeline(def);
  ASSERT_TRUE(p.ok()) << p.message();
  EXPECT_EQ(p.value()->descriptor_set_count(), 0u)
      << "P0-1:slots=0 必须仍返回 0 个描述符集(既有管线逐字节不变的结构性保证)";
  EXPECT_EQ(p.value()->sampled_texture_slots(), 0u);

  // 设备态也不应偷偷建出描述符集对象。
  auto* vp = dynamic_cast<magicblocks::backend::VulkanPipeline*>(p.value().get());
  if (vp != nullptr) {
    EXPECT_FALSE(vp->has_descriptor_set()) << "slots=0 时不得建 VkDescriptorSet";
    EXPECT_EQ(vp->vk_set_layout(), VK_NULL_HANDLE) << "slots=0 时不得建 VkDescriptorSetLayout";
    EXPECT_EQ(vp->vk_descriptor_pool(), VK_NULL_HANDLE) << "slots=0 时不得建 VkDescriptorPool";
  }
}

// P0-1:声明 1 个槽 → 恰好 1 个描述符集。
TEST(PipelineSampledTexture, OneSlotCreatesExactlyOneDescriptorSet) {
  auto d = require_sw_device();
  if (!d) return;

  auto p = d->create_pipeline(textured_desc(1));
  ASSERT_TRUE(p.ok()) << p.message();
  EXPECT_EQ(p.value()->descriptor_set_count(), 1u) << "slots>0 必须建恰好 1 个描述符集";
  EXPECT_EQ(p.value()->sampled_texture_slots(), 1u);

  auto* vp = dynamic_cast<magicblocks::backend::VulkanPipeline*>(p.value().get());
  ASSERT_NE(vp, nullptr);
  EXPECT_TRUE(vp->has_descriptor_set()) << "设备态必须真正分配出 VkDescriptorSet";
  EXPECT_NE(vp->vk_set_layout(), VK_NULL_HANDLE);
  EXPECT_NE(vp->vk_descriptor_pool(), VK_NULL_HANDLE);

  // 声明多个槽也仍是 1 个 set(本轮不做多集)。
  auto p4 = d->create_pipeline(textured_desc(4));
  ASSERT_TRUE(p4.ok()) << p4.message();
  EXPECT_EQ(p4.value()->descriptor_set_count(), 1u)
      << "多 slot 仍是 1 个 set(array),不是多个 set";
  EXPECT_EQ(p4.value()->sampled_texture_slots(), 4u);
}

// P0:slot 越界 → 返回错误 Status,不崩、不静默丢弃。
TEST(PipelineSampledTexture, RejectsOutOfRangeSlot) {
  auto d = require_sw_device();
  if (!d) return;
  res::IMemoryAllocator* a = d->memory_allocator();
  ASSERT_NE(a, nullptr);
  ASSERT_TRUE(a->reserve_pages(res::MemoryKind::Texture, 1).ok());
  ASSERT_TRUE(a->reserve_pages(res::MemoryKind::Staging, 1).ok());

  // 管线只声明 1 个槽。
  auto p = d->create_pipeline(textured_desc(1));
  ASSERT_TRUE(p.ok()) << p.message();

  auto tr = d->create_texture(unit_tex());
  ASSERT_TRUE(tr.ok()) << tr.message();
  auto sm = d->create_sampler(res::SamplerDesc{});
  ASSERT_TRUE(sm.ok()) << sm.message();

  auto tgt = d->create_offscreen_target({8, 8, dev::Format::RGBA8_UNORM});
  ASSERT_TRUE(tgt.ok()) << tgt.message();
  auto cl = d->create_command_list();
  ASSERT_TRUE(cl.ok()) << cl.message();
  auto* vcl = dynamic_cast<magicblocks::backend::VulkanCommandList*>(cl.value().get());
  ASSERT_NE(vcl, nullptr);
  ASSERT_TRUE(vcl->init().ok());
  ASSERT_TRUE(vcl->remember_texture_sampler(tr.value()->id(), tr.value().get(),
                                            sm.value()->id(), sm.value().get())
                  .ok());

  res::RenderPassDesc rp;
  rp.target = tgt.value().get();
  ASSERT_TRUE(cl.value()->begin_pass(rp).ok());
  ASSERT_TRUE(cl.value()->bind_pipeline(p.value().get(), p.value()->hash()).ok());

  const res::u32 before = cl.value()->command_count();
  const dev::Status bad = cl.value()->bind_sampled_texture(tr.value()->id(), sm.value()->id(), 1);
  EXPECT_FALSE(bad.ok()) << "slot 越界必须失败";
  EXPECT_EQ(bad.code, dev::StatusCode::InvalidArgument);
  EXPECT_EQ(cl.value()->command_count(), before) << "失败的绑定不得计入命令数";

  // slot 0 在声明了 1 个槽的管线上是合法的(证明失败原因是越界而非其他)。
  const dev::Status good =
      cl.value()->bind_sampled_texture(tr.value()->id(), sm.value()->id(), 0);
  EXPECT_TRUE(good.ok()) << good.message;
  EXPECT_EQ(cl.value()->command_count(), before + 1)
      << "成功的绑定必须计入命令数(与 bind_pipeline 同口径)";
  ASSERT_TRUE(cl.value()->end_pass().ok());
}

// P0:命令录制 —— bind_sampled_texture 进入命令流,句柄按偏移打包(M3)。
TEST(PipelineSampledTexture, RecordsIntoCommandStream) {
  auto d = require_sw_device();
  if (!d) return;
  res::IMemoryAllocator* a = d->memory_allocator();
  ASSERT_NE(a, nullptr);
  ASSERT_TRUE(a->reserve_pages(res::MemoryKind::Texture, 1).ok());

  auto p = d->create_pipeline(textured_desc(2));
  ASSERT_TRUE(p.ok()) << p.message();
  auto tr = d->create_texture(unit_tex());
  ASSERT_TRUE(tr.ok()) << tr.message();
  auto sm = d->create_sampler(res::SamplerDesc{});
  ASSERT_TRUE(sm.ok()) << sm.message();
  auto tgt = d->create_offscreen_target({8, 8, dev::Format::RGBA8_UNORM});
  ASSERT_TRUE(tgt.ok()) << tgt.message();
  auto cl = d->create_command_list();
  ASSERT_TRUE(cl.ok()) << cl.message();
  auto* vcl = dynamic_cast<magicblocks::backend::VulkanCommandList*>(cl.value().get());
  ASSERT_NE(vcl, nullptr);
  ASSERT_TRUE(vcl->init().ok());

  res::RenderPassDesc rp;
  rp.target = tgt.value().get();
  ASSERT_TRUE(cl.value()->begin_pass(rp).ok());
  ASSERT_TRUE(cl.value()->bind_pipeline(p.value().get(), p.value()->hash()).ok());
  ASSERT_TRUE(cl.value()->bind_sampled_texture(tr.value()->id(), sm.value()->id(), 0).ok());
  ASSERT_TRUE(cl.value()->bind_sampled_texture(tr.value()->id(), sm.value()->id(), 1).ok());
  ASSERT_TRUE(cl.value()->end_pass().ok());

  // op 计数:两次纹理绑定都应落进命令流。
  EXPECT_EQ(vcl->op_count(magicblocks::backend::CommandOp::BindSampledTexture), 2u)
      << "每次 bind_sampled_texture 都必须录制进命令流";
  EXPECT_EQ(vcl->command_count(), 5u) << "begin_pass + bind_pipeline + 2×bind + end_pass";

  // M3:句柄按 page_id/offset 打包,不是指针。
  const auto& rec = vcl->recorded();
  bool saw_slot0 = false;
  bool saw_slot1 = false;
  for (const auto& c : rec) {
    if (c.op != magicblocks::backend::CommandOp::BindSampledTexture) continue;
    EXPECT_TRUE(c.packed_ok) << "纹理句柄必须能 pack 进命令流(M3)";
    magicblocks::resources::u64 expect = 0;
    ASSERT_TRUE(c.resource.pack(&expect)) << "纹理句柄必须可打包";
    EXPECT_EQ(c.packed_resource, expect) << "命令流存的是偏移不是指针";
    EXPECT_NE(c.packed_sampler, 0u) << "采样器句柄也必须打包";
    EXPECT_EQ(c.resource.page_id, tr.value()->id().page_id);
    if (c.a == 0) saw_slot0 = true;
    if (c.a == 1) saw_slot1 = true;
  }
  EXPECT_TRUE(saw_slot0);
  EXPECT_TRUE(saw_slot1);
}
