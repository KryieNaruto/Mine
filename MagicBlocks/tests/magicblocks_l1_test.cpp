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
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "magicblocks/device/render_device.h"
#include "magicblocks/resources/buffer.h"
#include "magicblocks/resources/command.h"
#include "magicblocks/resources/handles.h"
#include "magicblocks/resources/memory.h"
#include "magicblocks/resources/pipeline.h"
#include "magicblocks/resources/texture.h"

// 测试要触达后端自检绘制与分页分配器的具体类型(同 L0 测试的做法)。
#include "backend/vulkan/vulkan_allocator.h"
#include "backend/vulkan/vulkan_command.h"
#include "backend/vulkan/vulkan_draw.h"
#include "backend/vulkan/vulkan_pipeline.h"
#include "backend/vulkan/vulkan_render_device.h"  // VulkanRenderDevice::context()
#include "backend/vulkan/vulkan_sync_core.h"      // SyncModel.* 直接查槽位/退休栈
#include "backend/vulkan/vulkan_target.h"
// 双通道独立性用例要直接驱动 UI 通道(离屏覆盖层)并与 SDK 通道对拍。
#include "backend/vulkan/vulkan_ui_overlay.h"
// 构建期由 shaders/embed_shaders.py 生成的引擎内嵌 SPIR-V(R6 通路)。
#include "magicblocks_shaders.h"

namespace mb = magicblocks;
namespace dev = magicblocks::device;
namespace res = magicblocks::resources;
namespace backend = magicblocks::backend;

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

// 【t23 新增 · 发现的既有缺陷的回归护栏】
//
// validate() 对**图像页**(image page)误判:
//   - `allocate_image_page` 建页时写 `p.slot_count = 0`(见 vulkan_allocator.cpp
//     里 "不切槽位" 那行),因为图像页是整块绑定给一个 VkImage 的,不切 64 KiB 槽;
//   - 但 `validate()` 的页内几何检查无条件要求 `slot_count != 0`,
//     且**没有**对 `image_bound` 页做豁免。
//   ⇒ 任何一次 readback_rgba8(它必然 allocate_image_page)之后,
//     allocator->validate() 就必然报 "页 N 槽位数非法"。
//
// 实测(探针,已复核):干净态 validate => OK;分配一个未绑定、未提交、
// 未读回的 image page 后 => "页 0 槽位数非法"。
//
// 危害:validate() 是测试里"分配器账本自洽"的权威判据(本文件开头纪律第 3 条)。
// 它一旦恒假,所有在 readback 之后做的账本校验全部失真 —— 正是"判据失效"
// 而不是"被测对象坏了"的那种坑。
//
// 本用例把正确行为钉住:图像页存在时 validate() 必须仍然通过。
// 注意:这条**不是**我能在 L1/L3 范围内修的(validate 与 image page 都属
// 后端分配器内部),所以我只用例化 + 上报,不改实现。
TEST(Allocator, ValidateAcceptsImagePages) {
  auto d = require_sw_device();
  if (!d) return;
  res::IMemoryAllocator* a = d->memory_allocator();
  ASSERT_NE(a, nullptr);
  auto* pager = dynamic_cast<magicblocks::backend::VulkanPagingAllocator*>(a);
  ASSERT_NE(pager, nullptr) << "需要后端子类才能直接建图像页";

  ASSERT_TRUE(a->validate().ok()) << "初始应通过: " << a->validate().message;

  magicblocks::backend::VulkanPagingAllocator::ImagePageRequest req{};
  req.size = 64 * 1024;
  req.alignment = 64;
  req.memory_type_bits = 0xFFu;
  req.device_local = false;
  auto page = pager->allocate_image_page(req);
  ASSERT_TRUE(page.ok()) << page.message();

  // 只建页,不绑定、不提交、不读回 —— 排除任何在途/回收因素。
  EXPECT_TRUE(a->validate().ok())
      << "存在图像页时 validate() 必须通过,实际: " << a->validate().message
      << " (图像页 slot_count 恒为 0,validate 需对 image_bound 页豁免)";

  ASSERT_TRUE(pager->release_image_page(page.value().page_id).ok());
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

// M4 的另一半(上面 IdlePageEvictedAfterIdleFrames 只测了"该淘汰时就淘汰").
//
// 本条测**"不该淘汰时绝不淘汰"**:页被标记为"有在途帧引用"(in_flight)时,
// 即便已经空置超过 page_idle_frames 帧,**也必须留在册上**。
//
// 为什么单独立一条 —— 这是构造出来的、能一票区分"防护真的生效"与
// "防护是死代码"的可执行判据:
//
//   trim() 的淘汰条件是 4 个谓词的合取(vulkan_allocator.cpp:1114-1117):
//     live == 0 && !in_flight && free_bytes >= page_size && idle_frames >= threshold
//   本用例先把另外 3 个谓词**显式断言**摆成"满足态",于是剩下的差别**只有** in_flight。
//   此时:
//     - 若 in_flight 防护生效  → 页留下 -> 本用例绿;
//     - 若 in_flight 是死代码  → 页被淘汰 -> 本用例红。
//
// 特别注意避免写成"恒绿的空壳":如果只是简单地 reserve_pages + allocate + free,
// 页会因为 free_bytes 不足(碎片未合并回整页)**本来就**不会被淘汰,
// 那样即便 in_flight 完全没接线,用例也会绿 —— 毫无判别力。
// 所以下面每一步都先断言"该谓词已满足",再谈 in_flight 的差别。
TEST(Allocator, PageNotEvictedWhileInFlight) {
  auto d = require_sw_device();
  if (!d) return;
  res::IMemoryAllocator* a = d->memory_allocator();
  ASSERT_NE(a, nullptr);

  ASSERT_TRUE(a->reserve_pages(res::MemoryKind::Texture, 1).ok());

  // ---- 造出"除 in_flight 外全部淘汰条件都满足"的页 ----
  auto r = a->allocate(res::MemoryKind::Texture, 4096, 64);
  ASSERT_TRUE(r.ok()) << r.message();
  const dev::u32 page = r.value().id.page_id;

  // 释放唯一的活子块 -> live 归零,且 release_slot 会把它与前后残段合并,
  // 最终整页应回到"单个覆盖整页的空闲块"(free_bytes == page_size)。
  ASSERT_TRUE(a->free(r.value().id).ok());

  const dev::u32 idle_frames = a->page_idle_frames();
  ASSERT_EQ(idle_frames, 120u) << "spec §6.2-4:默认空置 120 帧才退回";

  // 前提校验:排掉"因为 free_bytes 不够,所以本来就不会淘汰"这条干扰路径。
  {
    res::PageDesc probe{};
    ASSERT_TRUE(a->page_desc(page, &probe)) << "刚释放的页应当仍在册";
    EXPECT_EQ(probe.live_allocations, 0u) << "live 必须归零,否则淘汰条件本身不满足";
    EXPECT_EQ(probe.free_block_count, 1u)
        << "整页应合并成 1 个空闲块(证明子块确实合并回可分配池);"
           "若这里不是 1,说明页淘汰的前置条件在测试里根本构造不出来,"
           "本条用例就失去判别力,必须先停下来查清而不是放宽断言";
  }

  // ---- 置 in_flight = true ----
  // evict_count 是**设备级累计值**:用差分,不用绝对值 —— 设备初始化阶段若建过/退过别的页,
  // 基线就不是 0,绝对断言会误判。
  const dev::u64 evict_base = a->stats().evict_count;
  a->set_page_in_flight(page, true);
  {
    res::PageDesc probe{};
    ASSERT_TRUE(a->page_desc(page, &probe));
    ASSERT_TRUE(probe.in_flight) << "set_page_in_flight(true) 未生效";
  }

  // 推进到远超阈值。此时 live==0 / free_bytes==page_size / idle_frames>=threshold
  // 三个谓词都已满足,**唯一的拦截者只能是 in_flight**。
  for (dev::u32 f = 1; f <= idle_frames + 10; ++f) {
    a->begin_frame(f);
    res::PageDesc probe{};
    ASSERT_TRUE(a->page_desc(page, &probe))
        << "第 " << f << " 帧:页在 in_flight 期间被淘汰了 —— "
        << "in_flight 防护未生效(死代码)";
  }

  // 显式确认"空置够久"这个谓词确实已满足:否则本用例仍可能是恒绿的。
  {
    res::PageDesc probe{};
    ASSERT_TRUE(a->page_desc(page, &probe));
    ASSERT_GE(probe.idle_frames, static_cast<dev::u64>(idle_frames))
        << "空置帧数没到阈值,本条用例不具备判别力";
    ASSERT_TRUE(probe.in_flight) << "in_flight 标记在中途丢失";
  }

  EXPECT_EQ(a->stats().evict_count, evict_base) << "in_flight 期间不应产生任何页淘汰";

  // ---- 置 in_flight = false:此时才应退役 ----
  a->set_page_in_flight(page, false);
  const dev::u64 evict_before = a->stats().evict_count;
  a->begin_frame(idle_frames + 11);
  ASSERT_TRUE(a->trim().ok());

  EXPECT_GT(a->stats().evict_count, evict_before)
      << "解除 in_flight 后,已满足全部淘汰条件的页必须被退役;"
         "否则说明 trim() 的淘汰能力本身是假的";

  res::PageDesc gone{};
  EXPECT_FALSE(a->page_desc(page, &gone)) << "已淘汰的页不应还登记在册";
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

  dev::TargetDesc tdesc;
  tdesc.kind = dev::TargetDesc::Kind::Offscreen;
  tdesc.extent = {8, 8};
  tdesc.with_depth = false;
  tdesc.format = dev::Format::RGBA8_UNORM;
  auto tgt = d->create_offscreen_target(tdesc);
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
  dev::TargetDesc tdesc;
  tdesc.kind = dev::TargetDesc::Kind::Offscreen;
  tdesc.extent = {8, 8};
  tdesc.with_depth = false;
  tdesc.format = dev::Format::RGBA8_UNORM;
  auto tgt = d->create_offscreen_target(tdesc);
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
    // 采样器用专用页号(kSamplerPage),超出 pack() 的 16 位页宽,
    // 所以采样器的稳定身份直接记在 packed_sampler = id(= slot_index)。
    EXPECT_NE(c.packed_sampler, 0u) << "采样器槽位号必须记进命令流";
    EXPECT_EQ(c.packed_sampler, c.sampler.id) << "采样器身份 = slot_index";
    EXPECT_EQ(c.sampler.page_id, res::ResourceId::kSamplerPage)
        << "采样器句柄用专用页号,不占真实内存页";
    EXPECT_EQ(c.resource.page_id, tr.value()->id().page_id);
    if (c.a == 0) saw_slot0 = true;
    if (c.a == 1) saw_slot1 = true;
  }
  EXPECT_TRUE(saw_slot0);
  EXPECT_TRUE(saw_slot1);
}

// ================================================================ 多帧并行同步模型
//
// 下面两条用例是"submit 不再同步"这一改造的**回归护栏**。
//
// 改造前:submit_device_frame 返回前自己 vkWaitForFences 同步,
//         所以"submit 返回时 GPU 已经画完"是白送的;socket 侧所有回收点
//         (R1~R15)都隐含吃这个前提,没人显式写下来。
// 改造后:submit 只是把命令压进本槽队列就返回,GPU 可能还在跑。
//         于是"我把这块缓冲/这一页丢掉"与"GPU 还在读它"之间**必须**
//         有显式的在途保护,否则就是 use-after-free —— 而这种 UAF 在
//         SwiftShader 上常常**不崩也不报 validation**,只是像素偶发错误。
//
// 这两条用例不测"能不能跑",测的是"保护到底有没有接线":
//   1. BufferLifecycleAcrossFrameRing —— N+2 帧:同一块缓冲被连续 N+2 帧
//      提交引用期间页不得被 trim() 退回;交出引用后必须能正常退回
//      (证明判据两个方向都通,不是恒绿的空壳)。
//   2. UiAndSdkChannelsStayIndependent —— 两条通道各自持有渲染目标时,
//      UI 通道的布局约定不得污染 SDK 通道的读回约定。

// ---------------------------------------------------------------- N+2 帧缓冲生命周期
//
// 【判据为什么是这两个方向】
//   - 只测"在途期间没被淘汰"→ 恒绿空壳:如果 trim() 根本不工作,
//     或者这块页因为别的原因(live!=0、free_bytes 不够)本来就不会被淘汰,
//     那么即便在途保护**完全没接线**,用例也会绿。
//   - 所以必须再测"交出引用后确实会被淘汰",证明这条页**真的处在
//     可淘汰的边缘**,于是在途标记是它唯一的救命稻草。
//   两个方向一起才构成判别力。
TEST(SyncModel, BufferLifecycleAcrossFrameRing) {
  auto d = require_sw_device();
  if (!d) return;
  res::IMemoryAllocator* a = d->memory_allocator();
  ASSERT_NE(a, nullptr);
  dev::IFrameSync* fs = d->frame_sync();
  ASSERT_NE(fs, nullptr);

  const dev::u32 n = fs->in_flight_count();
  ASSERT_TRUE(n == 2u || n == 3u) << "帧环大小必须是 2 或 3,实际 " << n;

  ASSERT_TRUE(a->reserve_pages(res::MemoryKind::Vertex, 1).ok());
  ASSERT_TRUE(a->reserve_pages(res::MemoryKind::Uniform, 1).ok());

  // graphics_desc() 默认开深度测试/写(PipelineDesc::depthTest/depthWrite 默认 true),
  // 而本用例的离屏目标 with_depth=false。管线与 render pass 的附件形状必须逐位
  // 兼容(VUID-vkCmdDraw-renderPass-02684),否则 validation 直接报红。
  // 这里要的只是"命令流里引用这块顶点缓冲",不需要深度语义,故显式关掉。
  res::PipelineDesc pd = graphics_desc();
  pd.push_constant_bytes = 64;
  pd.depth.depthTest = false;
  pd.depth.depthWrite = false;
  auto pr = d->create_pipeline(pd);
  ASSERT_TRUE(pr.ok()) << pr.message();
  res::IPipeline* pl = pr.value().get();

  dev::TargetDesc td;
  td.kind = dev::TargetDesc::Kind::Offscreen;
  td.extent = {16, 16};
  td.with_depth = false;
  auto tgt = d->create_offscreen_target(td);
  ASSERT_TRUE(tgt.ok()) << tgt.message();

  // 这块顶点缓冲就是被反复引用的对象。4096 字节远小于一个页,
  // 因此它和可能的其他子块共存 —— 这里刻意只建这一块,让它独占一页,
  // 于是"它被释放 ⇒ 整页 live 归零 ⇒ 走到 trim() 的判定点"。
  res::BufferDesc vbd;
  vbd.size = 4096;
  vbd.usage = res::BufferUsage::Vertex;
  auto vbr = d->create_buffer(vbd);
  ASSERT_TRUE(vbr.ok()) << vbr.message();
  res::IBuffer* vb = vbr.value().get();
  const dev::u32 vb_page = vb->id().page_id;

  // ---- 阶段一:连续提交 N+2 帧,每帧都引用这块缓冲 ----
  //
  // 为什么是 N+2 而不是 N:N 帧刚好覆盖一轮环,第 N 帧提交时第 0 帧的槽
  // 被复用(wait_slot 等它跑完),这是"最紧"的时刻;再 +2 帧让环完整转
  // 一圈半,确保跨轮的引用计数也正确(轮与轮之间不应该有任何泄漏)。
  const dev::u32 frames = n + 2;
  for (dev::u32 f = 0; f < frames; ++f) {
    auto cr = d->create_command_list();
    ASSERT_TRUE(cr.ok()) << cr.message();
    res::ICommandList* cl = cr.value().get();

    res::RenderPassDesc rp;
    rp.target = tgt.value().get();
    rp.clear_color = true;
    ASSERT_TRUE(cl->begin_pass(rp).ok());
    ASSERT_TRUE(cl->bind_pipeline(pl, pl->hash()).ok());
    ASSERT_TRUE(cl->bind_vertex_buffer(vb->id(), 0, 0).ok());
    res::DrawArgs da;
    da.vertex_count = 3;
    ASSERT_TRUE(cl->draw(da).ok());
    ASSERT_TRUE(cl->end_pass().ok());

    dev::Status s = d->submit_command_list(*cl);
    ASSERT_TRUE(s.ok()) << "第 " << f << " 帧提交失败: " << s.message;

    // 提交返回 ≠ GPU 画完。这里**显式**不等,正是要点:
    // 若实现偷偷在 submit 里同步等待,下面的在途断言照样成立,
    // 所以"是否真的并行"由帧环大小与 fence 语义保证(见 L0 用例),
    // 本用例只保证**不因并行而踩内存**。
    //
    // 在途保护必须已经接线:这块缓冲所在页此刻不得被 trim() 退回。
    //
    // 注意:这里**不能**再手动 a->begin_frame(...)。submit_command_list 内部经
    // submit_device_frame → sync_core.begin_frame 已经触发过帧边界回调
    // (on_frame_boundary → clear_pages_in_flight + allocator_->begin_frame)。
    // 再手动推一次帧号会:① 让分配器的 idle 计数多走一遍;
    // ② 更关键的是此时本帧的槽位标记刚被清掉,再查 in_flight 必然是 false,
    //    断言就成了恒假的噪声。这里只调 trim(),验证"清标记之后、页仍活着"。
    ASSERT_TRUE(a->trim().ok());

    res::PageDesc probe{};
    ASSERT_TRUE(a->page_desc(vb_page, &probe))
        << "第 " << f << " 帧:被引用的页被整页退回 —— 在途保护未生效(UAF)"
        << "(帧环 N=" << n << ")";
    EXPECT_GE(probe.live_allocations, 1u) << "缓冲对象还活着,子块必须在册";
  }

  // 走到这里说明 N+2 帧内页一直活着。但"一直活着"可能只是因为
  // live != 0(缓冲对象还没销毁)。必须先证明它确实处在可淘汰边缘,
  // 后续的"交出引用后能淘汰"才有意义。
  {
    res::PageDesc probe{};
    ASSERT_TRUE(a->page_desc(vb_page, &probe));
    EXPECT_GE(probe.live_allocations, 1u) << "缓冲对象还在,子块当然还活着";
  }

  // ---- 阶段二:销毁缓冲,交出所有引用 ----
  //
  // 这是所有回收点里最危险的一个:~VulkanBuffer 会把子块还给页,
  // 此时 live 归零,但页上可能还有在途帧的引用(最后几帧的 fence 未信号)。
  // 在途标记必须仍然把它拦住。
  const dev::u64 evict_base = a->stats().evict_count;
  vbr.value().reset();
  vb = nullptr;

  // 这里也不手动推帧号(理由同阶段一)。只 trim:若页上仍有在途引用,
  // trim 的 `if (p.in_flight) continue;` 会拦住它。
  ASSERT_TRUE(a->trim().ok());

  res::PageDesc after_release{};

  // 注意:这里**不**断言页一定还在。因为提交都是异步的,GPU 可能已经把
  // 最后几帧跑完,fence 已信号,那么页在这一步被正常淘汰是**合法**的。
  // 强行要求页必须还在,等于要求"GPU 一定还没跑完",那是不稳定的。
  //
  // 也不用 allocator->validate() 在这里做"账本自洽"判据:
  // 本用例建了离屏目标,create_offscreen_target 内部就会 allocate_image_page,
  // 而 validate() 对图像页有既有误判(见 Allocator.ValidateAcceptsImagePages)。
  // 那个缺陷会让本句恒假,从而掩盖本用例真正要守的东西。
  // 图像页缺陷由那条专门用例独立看守,这里只用与本用例相关的判据。
  const bool alive_after_release = a->page_desc(vb_page, &after_release);

  // ---- 阶段三:用**真实提交**推进帧环,交出在途引用后必须能正常淘汰 ----
  //
  // 【为什么必须用真实提交,而不是 allocator->begin_frame(f)】
  // 在途标记是按**槽位**记引用计数的(page_slot_refs_ / pages_in_flight_):
  //   - 每次提交把页记入"刚才那一帧的槽";
  //   - 某个槽的 fence 确认后,begin_frame 的回调**只清该槽**那一份;
  //   - 一个页只要还在任一槽的集合里,对分配器就报 in_flight=true。
  // 阶段一连续 N+2 帧都引用同一页,于是它同时挂在全部 N 个槽上;
  // 要把它清干净,必须有 N 次**跨槽**的帧边界 —— 只有真实提交才会产生。
  // 直接调 allocator->begin_frame(f) 只推分配器帧号,**不碰**页的槽位引用表。
  //
  // 本阶段每帧提交一次(不再引用那块已销毁的缓冲),让帧环完整转足够多圈,
  // 每个槽都轮到若干次边界;再叠加超过空置阈值的帧数,页就必须被退回。
  const dev::u32 idle = a->page_idle_frames();
  const dev::u32 settle = idle + 16;  // 足够覆盖:N×槽 + idle 阈值
  for (dev::u32 f = 0; f < settle; ++f) {
    auto cr = d->create_command_list();
    ASSERT_TRUE(cr.ok()) << cr.message();
    res::ICommandList* cl = cr.value().get();
    res::RenderPassDesc rp;
    rp.target = tgt.value().get();
    rp.clear_color = true;
    ASSERT_TRUE(cl->begin_pass(rp).ok());
    ASSERT_TRUE(cl->bind_pipeline(pl, pl->hash()).ok());
    res::DrawArgs da;
    da.vertex_count = 3;
    ASSERT_TRUE(cl->draw(da).ok());
    ASSERT_TRUE(cl->end_pass().ok());
    ASSERT_TRUE(d->submit_command_list(*cl).ok()) << "阶段三第 " << f << " 帧提交失败";
  }
  // 再叠一批纯分配器帧号,确保 idle 计数早已越过阈值。
  for (dev::u32 f = 0; f < idle + 16; ++f) {
    a->begin_frame(static_cast<dev::u64>(10000 + f));
  }
  ASSERT_TRUE(a->trim().ok());

  res::PageDesc after{};
  // 收尾:这块缓冲已销毁,它所在的页在充分出清后必须被回收。
  if (a->page_desc(vb_page, &after)) {
    std::fprintf(stderr,
                 "[diag] page=%u live=%llu in_flight=%d idle=%llu freeblocks=%u bytes_in_use=%llu\n",
                 vb_page, static_cast<unsigned long long>(after.live_allocations),
                 after.in_flight ? 1 : 0, static_cast<unsigned long long>(after.idle_frames),
                 after.free_block_count, static_cast<unsigned long long>(after.bytes_in_use));
    EXPECT_FALSE(after.in_flight)
        << "缓冲已销毁且帧环已转够,页上不应再有任何在途引用"
        << "(in_flight 标记泄漏 = 该页永远无法回收)";
    EXPECT_GT(a->stats().evict_count, evict_base)
        << "页已无引用、已越过空置阈值,trim() 必须把它退回 VMA";
  } else {
    // 页已经被退回 —— 这正是期望结局。
    EXPECT_GT(a->stats().evict_count, evict_base);
  }
}

// ---------------------------------------------------------------- 双通道布局独立性
//
// 【背景:这是改造后最容易静默出错的地方】
// 设备上有两条互不相干的提交通道:
//
//   SDK 通道(场景/网格):vulkan_device_frame.cpp:118-120
//       离屏:initialLayout = clear ? UNDEFINED : GENERAL
//             finalLayout   = GENERAL            ← 读回的约定末态
//       读回:vulkan_target.cpp:303
//             oldLayout = GENERAL → TRANSFER_SRC_OPTIMAL → 回到 GENERAL
//
//   UI 通道(离屏覆盖层):vulkan_ui_overlay.cpp:557 / 582 / 198
//       自己先 UNDEFINED → GENERAL 清屏,再 GENERAL → COLOR_ATTACHMENT_OPTIMAL,
//       pass 的 finalLayout 也硬编码 COLOR_ATTACHMENT_OPTIMAL。
//
// 两条通道各自**硬编码**了自己那一侧的布局,谁都不知道对方把这张图留在了
// 什么布局上。各自独占一个渲染目标时完全没问题(本用例前半段证明这一点)。
//
// 一旦两条通道指向**同一个**目标,布局约定立刻打架:
//   UI 跑完把图留在 COLOR_ATTACHMENT_OPTIMAL,SDK 读回却声明 oldLayout=GENERAL。
//   这是**布局不符**(VUID-VkImageMemoryBarrier-oldLayout-01197 一类),
//   后果不是崩溃,而是像素错误 —— 背景丢失/白块,且不一定有 validation 报告。
//
// 所以本用例守的是:**两条通道必须各自独占目标,不得合并**。
// 前半段验证"独立时都对",后半段验证"合并后的冲突是可观测的、不是玄学"。
//
// 说明:后半段若某驱动恰好容忍了布局不符,我们不能凭空断言失败 ——
// 那会变成依赖驱动的脆弱用例。所以后半段的判据取"要么返回值报错,
// 要么像素与独立路径不一致",两者之一成立即可:无论走哪条,都证明
// 合并是**不可接受**的,而这正是要守的结论。
//
// 【本函数体曾经被误删,2026-11-11 由队长按原始断言重建】
// 变量名与断言消息逐字取自 build-captain-chk2/magicblocks_test.exe 的只读
// 数据段(该 exe 编译于本函数仍在树内的时点),不是凭空补写:
//   sdk_t/sdk_off/ui_t/ui_off/submit_sdk_frame/fresh/fpx/again/
//   merged_renderer/merged_rejected 以及全部 << 消息文本均与二进制一致。
// 重建后必须重新全绿才算数 —— 见下面三条断言各自的判别力说明。
TEST(SyncModel, UiAndSdkChannelsStayIndependent) {
  auto d = require_sw_device();
  if (!d) return;

  // 两条通道各自需要一个**独立**的离屏目标。共用一张图正是本用例要否定的形态。
  const dev::u32 kW = 16, kH = 16;
  // fullscreen.vert 的网格是写死的 4x4(shaders/fullscreen.vert 的 kCols = 4u)。
  const dev::u32 kCols = 4, kRows = 4;

  // 需要后端子类才能拿到 context():VulkanFrameRenderer 的构造要 VulkanContext。
  auto* vkdev = dynamic_cast<magicblocks::backend::VulkanRenderDevice*>(d.get());
  ASSERT_NE(vkdev, nullptr) << "需要后端子类才能拿到 context()";

  // ⚠️ TargetDesc 必须**具名赋值**,不能用位置聚合初始化。
  //    成员顺序是 kind, extent, samples, with_depth, ... —— 写成
  //    TargetDesc{Kind::Offscreen, {w,h}, false} 时,那个 false 会落进 **samples**
  //    而不是 with_depth,于是 with_depth 保持默认的 true:
  //    目标悄悄带上深度附件,命令缓冲那边的 pass 就有深度,而管线自带的 pass
  //    没有 —— 每条 vkCmdDraw 都是 VUID-vkCmdDraw-renderPass-02684,而且
  //    用例照样能绿(像素判据不受影响),完全靠校验层才发现。
  auto make_target = [&]() {
    dev::TargetDesc td;
    td.kind = dev::TargetDesc::Kind::Offscreen;
    td.extent = {kW, kH};
    td.samples = 1;
    td.with_depth = false;  // 本条用例只关心颜色通道的布局独立性
    return d->create_offscreen_target(td);
  };

  auto sdk_t = make_target();
  ASSERT_TRUE(sdk_t.ok()) << sdk_t.message();
  auto& sdk_off = sdk_t.value();
  auto ui_t = make_target();
  ASSERT_TRUE(ui_t.ok()) << ui_t.message();
  auto& ui_off = ui_t.value();

  // 前置:两个目标必须是**两张不同的图**。若复用了同一张,本用例立刻失去判别力
  // (后面"两条通道产出不同像素"就会变成"同一个目标自己比自己")。
  auto* sdk_img = dynamic_cast<magicblocks::backend::OffscreenTarget*>(sdk_off.get());
  auto* ui_img = dynamic_cast<magicblocks::backend::OffscreenTarget*>(ui_off.get());
  ASSERT_NE(sdk_img, nullptr);
  ASSERT_NE(ui_img, nullptr);
  ASSERT_NE(ui_img->color_image(), sdk_img->color_image())
      << "两个渲染目标必须是不同的 VkImage,否则不是双通道独立性测试";

  // ---- SDK 通道:一条命令流,用 fullscreen 自检网格画出 4x4 色块 ----
  //
  // 【管线形状必须与 fullscreen.vert 的约定逐位一致】
  // 那个着色器**不读顶点缓冲**,格位与颜色全部由 gl_VertexIndex /
  // gl_InstanceIndex 程序化推导(见 shaders/fullscreen.vert 顶部注释),
  // 所以:
  //   - 不能设 push_constant_bytes(声明了却没有 shader 用 → 布局不一致);
  //   - 不能声明顶点输入(graphics_desc() 绑的属性没人消费,只会让
  //     校验层报 "attribute not consumed");
  //   - 绘制必须是 6 顶点 x 16 实例(TRIANGLE_LIST 四边形 x 4x4 格)。
  // 这正是 draw_selftest_grid() 用的形状。曾经这里沿用 graphics_desc()
  // 并只画 3 个顶点,结果是整屏只剩清屏色(纯黑)、实例 0 的红点根本落不进去,
  // 而且不报错 —— 那会让本用例的"通道独立性"判据整个失去判别力。
  res::PipelineDesc pd;
  pd.stages.push_back({res::ShaderStage::Vertex,
                       reinterpret_cast<const std::uint8_t*>(mb::shaders::kFullscreenVert),
                       mb::shaders::kFullscreenVertSize});
  pd.stages.push_back({res::ShaderStage::Fragment,
                       reinterpret_cast<const std::uint8_t*>(mb::shaders::kFullscreenFrag),
                       mb::shaders::kFullscreenFragSize});
  pd.depth.depthTest = false;
  pd.depth.depthWrite = false;
  // ⚠️ depthConstWrite 也必须显式关掉。create_pipeline 的派生规则是
  //   shape.has_depth = samples>0 && (depthTest || depthWrite || **depthConstWrite**)
  // 只关前两个的话,管线自带的 pass 仍会挂一个深度附件,而本用例的目标是
  // with_depth=false —— 于是每次 vkCmdDraw 都被校验层判为
  // VUID-vkCmdDraw-renderPass-02684(附件形状不兼容),管线自带的 pass 也不能复用,
  // 每次都退回 create_pass()。实测这一处贡献了 11 条该 VUID。
  pd.depth.depthConstWrite = false;
  // 自检网格的四边形在两个三角形里绕序相反,任何背面剔除都会吃掉一半;
  // 这条用例要的是"整屏 4x4 都在",所以显式关剔除,不依赖默认值。
  pd.raster.cull = res::CullMode::None;
  auto pr = d->create_pipeline(pd);
  ASSERT_TRUE(pr.ok()) << pr.message();
  res::IPipeline* pl = pr.value().get();

  auto submit_sdk_frame = [&](res::IRenderTarget* tgt) -> dev::Status {
    auto cr = d->create_command_list();
    if (!cr.ok()) return cr.status();
    res::ICommandList* cl = cr.value().get();
    res::RenderPassDesc rp;
    rp.target = tgt;
    rp.clear_color = true;
    dev::Status s = cl->begin_pass(rp);
    if (!s.ok()) return s;
    s = cl->bind_pipeline(pl, pl->hash());
    if (!s.ok()) return s;
    res::DrawArgs da;
    da.vertex_count = 6;          // fullscreen.vert 的 6 顶点四边���
    da.instance_count = kCols * kRows;  // 4x4 色块;实例 0 = 左上角纯红标定点
    s = cl->draw(da);
    if (!s.ok()) return s;
    s = cl->end_pass();
    if (!s.ok()) return s;
    return d->submit_command_list(*cl);
  };

  std::vector<std::uint8_t> sdk_px(static_cast<std::size_t>(kW) * kH * 4u, 0);
  ASSERT_TRUE(submit_sdk_frame(sdk_off.get()).ok()) << "SDK 帧提交失败";
  ASSERT_TRUE(sdk_off->readback_rgba8(sdk_px.data(), sdk_px.size()).ok())
      << "SDK 通道读回失败";

  // ---- 同一条通道连跑第二帧(同一个目标复用)----
  // 多帧并行 + 目标复用是最容易出现"上一帧的布局残留影响下一帧"的地方。
  std::vector<std::uint8_t> again(static_cast<std::size_t>(kW) * kH * 4u, 0);
  ASSERT_TRUE(submit_sdk_frame(sdk_off.get()).ok()) << "SDK 第二帧提交失败";
  ASSERT_TRUE(sdk_off->readback_rgba8(again.data(), again.size()).ok())
      << "SDK 第二帧读回失败";

  // ---- 换一张**新**目标再跑一次,作为"干净基线" ----
  // 与上面复用目标的读数对比:两者应当逐像素一致。
  // 不一致 = 目标复用引入了残留状态(布局没转回去 / 上一帧内容没被清掉)。
  auto fresh_t = make_target();
  ASSERT_TRUE(fresh_t.ok()) << fresh_t.message();
  auto& fresh = fresh_t.value();
  std::vector<std::uint8_t> fpx(static_cast<std::size_t>(kW) * kH * 4u, 0);
  ASSERT_TRUE(submit_sdk_frame(fresh.get()).ok()) << "干净目标提交失败";
  ASSERT_TRUE(fresh->readback_rgba8(fpx.data(), fpx.size()).ok()) << "干净目标读回失败";

  // ---- UI 通道:独占自己的目标,画一层纯绿覆盖 ----
  magicblocks::backend::VulkanFrameRenderer ui_renderer(
      vkdev->context(), dynamic_cast<magicblocks::backend::OffscreenTarget*>(ui_off.get()));
  std::vector<std::uint8_t> ui_px(static_cast<std::size_t>(kW) * kH * 4u, 0);
  ASSERT_TRUE(ui_renderer.overlay_pass()
                  .append_rect(0.f, 0.f, static_cast<float>(kW), static_cast<float>(kH), 0.0f, 1.0f,
                               0.0f, 1.0f)
                  .ok());
  ASSERT_TRUE(ui_renderer.render_to_memory(kW, kH, ui_px.data(), kW * 4u))
      << "UI 通道独占目标时必须能正常渲染 + 读回";

  // ---- 【本用例真正的判据】UI 渲染完之后,SDK 目标必须**一个字节都没变** ----
  //
  // 上面那些比较都不足以证明"两条通道各自独占目标",必须在这里补一刀:
  //   - `sdk_px` 是 UI 渲染**之前**的快照,UI 之后再没人读过 sdk_off ⇒
  //     `sdk_is_red` / `sdk_is_red && ui_is_green` 天然恒真;
  //   - `ui_px` 读的是渲染器**自己构造时那个 target**,两条通道真被指向同一张图时,
  //     它读到的也是那张图 ⇒ `sdk_lu != ui_r` 成立靠的是"旧快照 vs 新内容"的
  //     **时间差**,而不是"两个不同目标"的**空间隔离**;
  //   - `merged_rejected` 同样退化成"UI 能画出绿"的同义反复。
  //
  // 实测证据(2026-11-11,把 ui_renderer 故意指向 sdk_off 做变异):
  //   [MUT] sdk_off after UI = (0,255,0)  sdk_px snapshot = (0,0,0)
  //   [       OK ] SyncModel.UiAndSdkChannelsStayIndependent
  //   即 SDK 目标确实被 UI 盖成绿色,而上面全部断言仍然通过。
  //
  // 所以判据只能是"UI 跑完之后重读 SDK 目标,仍与快照逐字节相同"。
  // 这一条在变异下必红(0,255,0) != (0,0,0),在正常态必绿。
  std::vector<std::uint8_t> sdk_after(static_cast<std::size_t>(kW) * kH * 4u, 0);
  ASSERT_TRUE(sdk_off->readback_rgba8(sdk_after.data(), sdk_after.size()).ok())
      << "UI 渲染之后读不回 SDK 目标";
  EXPECT_TRUE(sdk_after == sdk_px)
      << "UI 通道渲染之后,SDK 通道的目标像素发生了变化 —— 两条通道没有真正各占一张图。"
      << "首像素:UI 之前 (" << unsigned(sdk_px[0]) << "," << unsigned(sdk_px[1]) << ","
      << unsigned(sdk_px[2]) << "),UI 之后 (" << unsigned(sdk_after[0]) << ","
      << unsigned(sdk_after[1]) << "," << unsigned(sdk_after[2]) << ")";

  // ---- 目标复用不得引入残留 ----
  // 同一目标连跑两帧、以及换一张新目标各跑一帧,三条路径的像素必须完全一致。
  // 任何不一致都说明"上一帧的布局/内容残留"会影响下一帧 —— 正是多帧并行下
  // 最容易静默发生、又最难从画面上看出来的一类错误。
  EXPECT_TRUE(again == sdk_px)
      << "同一个目标连跑两帧,像素不一致:目标复用时残留了上一帧的状态";
  EXPECT_TRUE(fpx == sdk_px)
      << "干净目标与复用目标的像素不一致:复用路径引入了残留(新目标才是无状态基线)";

  // ---- 判据 ①:SDK 通道留下的像素是它自己画的纯红 ----
  //
  // 采样点取**实例 0 的格心**,不是 (0,0) 也不是 (kH/4, kW/4):
  // fullscreen.vert 把实例铺成 kCols×kRows 个格子,每格内缩到 0.42(留缝),
  // 所以格内只有中央约 84% 是实体色,格子边界处是清屏色。
  // 实例 0 → cx=0, cy=0 → 图像左上角那一格;该格在 16x16 图上占像素
  // [0,4)x[0,4),格心 = (2,2)。写死 (4,4) 会正好落在第 0/1 格的缝上,
  // 读到清屏色,于是"绘制其实成功了"也会被判成失败。
  const dev::u32 cell_w = (kW + kCols - 1) / kCols;
  const dev::u32 cell_h = (kH + kRows - 1) / kRows;
  const dev::u32 inst0_x = cell_w / 2;  // 实例 0 的格心
  const dev::u32 inst0_y = cell_h / 2;
  const std::size_t inst0_off =
      (static_cast<std::size_t>(inst0_y) * kW + inst0_x) * 4u;

  const std::uint8_t* sdk_lu = &sdk_px[inst0_off];
  EXPECT_GE(sdk_lu[0], 200u)
      << "SDK 通道左上角必须是实例 0 的纯红;若不是,要么通道序错,要么目标被污染"
      << "(采样点 " << inst0_x << "," << inst0_y << " 读到 R=" << unsigned(sdk_lu[0])
      << " G=" << unsigned(sdk_lu[1]) << " B=" << unsigned(sdk_lu[2]) << ")";
  EXPECT_LE(sdk_lu[1], 255u);
  EXPECT_LE(sdk_lu[2], 255u);
  const bool sdk_is_red = sdk_lu[0] > 200u;
  const std::uint8_t* ui_lu = &ui_px[inst0_off];
  const bool ui_is_green = ui_lu[1] > 200u && ui_lu[0] < 60u;
  { std::vector<std::uint8_t> nowpx(static_cast<std::size_t>(kW) * kH * 4u, 0);
    if (sdk_off->readback_rgba8(nowpx.data(), nowpx.size()).ok()) {
      const std::uint8_t* q = &nowpx[inst0_off];
    } }
  const std::uint8_t ui_r = ui_lu[0], ui_g = ui_lu[1], ui_b = ui_lu[2];
  (void)ui_g;
  EXPECT_TRUE(ui_is_green) << "UI 通道目标不是它自己画的纯绿";
  EXPECT_TRUE(sdk_is_red && ui_is_green)
      << "SDK 通道目标被 UI 通道污染了(双通道不独立)";
  EXPECT_TRUE(sdk_lu[0] != ui_r || sdk_lu[1] != ui_g || sdk_lu[2] != ui_b)
      << "两条通道产出了相同像素,本用例失去判别力";

  // ---- 判据 ②:把两条通道指向同一张图,冲突必须可观测 ----
  //
  // 判据取"要么调用报错,要么像素与独立路径不同",两者之一成立即可
  // (驱动容忍布局不符时不能凭空断言失败,那会变成依赖驱动的脆弱用例)。
  magicblocks::backend::VulkanFrameRenderer merged_renderer(
      vkdev->context(), dynamic_cast<magicblocks::backend::OffscreenTarget*>(sdk_off.get()));
  const bool appended = merged_renderer.overlay_pass()
                            .append_rect(0.f, 0.f, static_cast<float>(kW),
                                         static_cast<float>(kH), 0.0f, 1.0f, 0.0f, 1.0f)
                            .ok();
  std::vector<std::uint8_t> merged_px(static_cast<std::size_t>(kW) * kH * 4u, 0);
  const bool drew =
      appended && merged_renderer.render_to_memory(kW, kH, merged_px.data(), kW * 4u);
  const bool merged_rejected =
      !drew || merged_px[0] != sdk_px[0] || merged_px[1] != sdk_px[1] ||
      merged_px[2] != sdk_px[2];
  EXPECT_TRUE(merged_rejected)
      << "把 UI 通道和 SDK 通道指向同一张图之后,冲突**既没有报错也没有改变像素**。"
      << "这说明两条通道的布局约定在合并后依然'看起来正常',"
      << "正是白块/丢背景且 validation 不报错的危险形态。";
}
// 帧环推进 / 分配器帧边界的**外部可观测**探针(2026-11-11 / t23)
// 目标:钉住"守卫没人驱动就静默退化成死代码"这一病症
// ============================================================================
//
// 病症(实测,不是推测):
//   IMemoryAllocator::begin_frame() 在 src/ 里**零调用**(除了测试自己)时,
//   trim() 也零调用、set_page_in_flight() 也零调用 —— 于是
//   vulkan_allocator.cpp 里的 `if (p.in_flight) continue;` 恒假、整页 LRU
//   淘汰永不到来,而**没有任何一层报错**。这和 RED-1(validate() 无条件拒绝
//   图像页)同族:不是判错了,是那个判别器根本不跑。
//
// 所以这两个用例只断言**调用方看得见的两条硬事实**,不碰驱动内部实现:
//   ① 一次 submit_command_list() ? 设备**恰好**推进一帧,且槽号按
//      (起点 + 已提交帧数) % N 循环 —— 判断依据是 core->**last_submitted_slot()**
//      (本帧槽),不是 current_slot()(下一帧槽,两者恒差 1 mod N)。
//   ② 该帧的**帧边界确实被驱动过**:被提交绑定的内存页,空置计数按 k*N 增长,
//      且本帧在途标记在该槽 retire 之后被解除。0 = 链路断(死代码),
//      偏小 = 漏槽,偏大 = 重复驱动。
//
// 为什么不需要碰内部实现:一个进程 = 一个 RenderDevice = 一个 VulkanSyncCore
// (registry 按 VkDevice 登记),trim()/in_flight 都是**设备级**状态,测试无法
// 自己造一个 core 去"假装推进帧"。只要外部调用序列正确,帧边界就必须被驱动;
// 这是调用方契约,与"设备内部怎么做"无关。

TEST(SyncCore, FrameRingAdvancesPerSubmit) {
  auto d = require_sw_device();
  if (!d) return;
  res::IMemoryAllocator* a = d->memory_allocator();
  ASSERT_NE(a, nullptr);
  dev::IFrameSync* fs = d->frame_sync();
  ASSERT_NE(fs, nullptr);

  const dev::u32 n = fs->in_flight_count();
  ASSERT_TRUE(n == 2u || n == 3u) << "帧环大小必须是 2 或 3,实为 " << n;

  // 帧环推进是本用例的**前提**:没有句柄说明帧环已被换成别的对象,
  // 后面所有关于"槽号轮转"的断言都失去意义。
  // 沿用本文件已有的 dynamic_cast 取具体实现(最宽松的写法);
  // 不引入任何"仅测试专用钩子" —— 探针本身属于设计的一部分。
  auto* vd = dynamic_cast<magicblocks::backend::VulkanRenderDevice*>(d.get());
  ASSERT_NE(vd, nullptr) << "require_sw_device() 必须返回 VulkanRenderDevice";
  auto* core = vd->sync_core_impl();
  ASSERT_NE(core, nullptr)
      << "create_render_device() 没有把 VulkanSyncCore 接上 —— 帧环根本不存在,"
         "本用例的前置条件不成立,请停下来问而不是放宽断言";

  // graphics_desc() 默认开深度测试/写;靶目标 render pass 没有深度附件,
  // 绑定就是 VUID-vkCmdDraw-renderPass-02684(与 SyncModel.BufferLifecycle 同口径)。
  res::PipelineDesc pd = graphics_desc();
  pd.push_constant_bytes = 64;
  pd.depth.depthTest = false;
  pd.depth.depthWrite = false;
  auto pr = d->create_pipeline(pd);
  ASSERT_TRUE(pr.ok()) << pr.message();
  res::IPipeline* pl = pr.value().get();

  dev::TargetDesc td;
  td.kind = dev::TargetDesc::Kind::Offscreen;
  td.extent = {16, 16};
  td.with_depth = false;
  auto tgt = d->create_offscreen_target(td);
  ASSERT_TRUE(tgt.ok()) << tgt.message();

  // 帧环**不是**从 0 开始的:设备 init 之前的预热提交(探针/自测)已经在同一个
  // core 上推进过 frame_index_。所以这里取的是"查询值",不是"初始值 0"。
  //
  // 两条恒等式(全部来自 end_frame 的 ++frame_index_):
  //   current_slot()        = frame_index_ % N              (下一帧要用的槽)
  //   last_submitted_slot() = (frame_index_ + N - 1) % N    (最近提交完的那一帧)
  // 二者恒差 1(mod N);一次 submit 之后,last_submitted_slot 必须**原样**等于
  // 提交前那一刻的 current_slot() —— 这是"一次提交 = 一帧"的可观测形式。
  //
  // 【前置条件:frame_index_ > 0】last_submitted_slot() 的语义是"**刚提交完**的那一帧",
  // 它由 (frame_index_ + N - 1) % N 算出。在 frame_index_ == 0(本设备**一帧都还没
  // 提交过**)时,这个访问器没有可指代的帧 —— (0 + N - 1) % N 只是个算术余数,不是
  // "上一帧的槽"。此时拿它去和 current_slot() 比对,是拿一个无意义的数当真值。
  //
  // 历史上这条断言在 N == 2 时**偶然**成立:0+1 与 (0+2-1)%2 都等于 1。换成 N == 3
  // 就露出原形(0 vs 2)。也就是说旧断言把"N=2 的巧合"当成了"槽位算术恒等式"。
  // 这里改为先确保真的提交过帧,再验算术 —— 断言本身反而更严格。
  if (fs->frame_index() == 0) {
    fs->advance_frame_index();  // 契约允许:只推进帧号,不提交、不复位任何 fence
  }
  ASSERT_GT(fs->frame_index(), 0ull)
      << "advance_frame_index 没有推进帧号,last_submitted_slot() 无从谈起";
  const dev::u32 first_slot = core->last_submitted_slot();
  // 恒等式方向必须是 last + 1 ≡ current (mod N),**不是** current + 1 ≡ last。
  //
  //   current_slot()        = frame_index_ % N            ← 下一帧要用的槽
  //   last_submitted_slot() = (frame_index_ + N - 1) % N  ← 刚提交完的槽
  //   代进去:(last + 1) % N = (frame_index_ + N) % N = frame_index_ % N = current ✓
  //
  // 旧断言写成了 (current + 1) % N == last —— 方向反了。它在 N == 2 时**碰巧**
  // 也成立(+1 与 -1 在模 2 下同余),所以一直没人发现;换成 N == 3 立刻暴露
  // (例:fidx=1 时 cur=1、last=0,(1+1)%3=2 ≠ 0)。即旧断言把"模 2 的自反性"
  // 当成了恒等式,这跟槽位算术对不对毫无关系。
  EXPECT_EQ((core->last_submitted_slot() + 1) % n, core->current_slot())
      << "last_submitted_slot 的下一槽必须等于 current_slot"
         "(end_frame 的 ++frame_index_ 恰好差 1);不等说明槽位算术已经坏了";

  const dev::u64 evict_base = a->stats().evict_count;

  // ---- 连续提交 N+1 帧 ----
  // N 帧刚好绕一圈回到起点;多 1 帧作为区分"轮转"与"原地不动"的对照。
  std::vector<dev::u32> submitted_slots;
  const dev::u32 frames = n + 1;
  for (dev::u32 f = 0; f < frames; ++f) {
    auto cr = d->create_command_list();
    ASSERT_TRUE(cr.ok()) << cr.message();
    res::ICommandList* cl = cr.value().get();

    res::RenderPassDesc rp;
    rp.target = tgt.value().get();
    rp.clear_color = true;
    ASSERT_TRUE(cl->begin_pass(rp).ok());
    ASSERT_TRUE(cl->bind_pipeline(pl, pl->hash()).ok());
    res::DrawArgs da;
    da.vertex_count = 3;
    ASSERT_TRUE(cl->draw(da).ok());
    ASSERT_TRUE(cl->end_pass().ok());

    // 提交前记下"下一帧要用哪个槽":提交完成后 last_submitted_slot 必须等于它。
    // 同时记下单调帧号,用来把"推进 1 帧"和"N=2 时推进 3 帧"区分开。
    const dev::u32 expected_slot = core->current_slot();
    const dev::u64 index_before = core->frame_index();

    dev::Status s = d->submit_command_list(*cl);
    ASSERT_TRUE(s.ok()) << "第 " << f << " 帧提交失败: " << s.message;

    // ---- 事实 ①-a:本帧槽号 = 提交前的 current_slot() ----
    EXPECT_EQ(core->last_submitted_slot(), expected_slot)
        << "第 " << f << " 帧:本帧槽号应为提交前的 current_slot() = " << expected_slot
        << ",实为 " << core->last_submitted_slot()
        << "。恒为同一个值 = last_submitted_slot()/end_frame 的推进已经坏了";

    // ---- 事实 ①-b:帧号确实被 end_frame 推进了恰好 1 ----
    // 槽号从 frame_index_ 派生,单看槽号会在 N=2 时把"推进 1"和"推进 3"看成一样;
    // 帧号是单调量,这里直接钉住"一次提交 = 帧号 +1"。
    EXPECT_EQ(core->frame_index(), index_before + 1)
        << "第 " << f << " 帧:提交前帧号 " << index_before << ",提交后 " << core->frame_index()
        << "。不等 = end_frame 的 ++frame_index_ 没有每帧推进一次"
           "(帧边界永远不会回来,在途页永不解锁)";
    EXPECT_EQ(core->last_submitted_slot(), static_cast<dev::u32>(index_before % n))
        << "第 " << f << " 帧:本帧槽号应由提交前帧号推出 = " << (index_before % n)
        << ",实为 " << core->last_submitted_slot()
        << "。槽号与帧号脱钩 = last_submitted_slot() 的取模算术坏了";

    submitted_slots.push_back(core->last_submitted_slot());
  }

  // 槽位必须真正轮转:N+1 帧的槽号集合应当覆盖全部 N 个槽。
  {
    std::vector<dev::u32> uniq = submitted_slots;
    std::sort(uniq.begin(), uniq.end());
    uniq.erase(std::unique(uniq.begin(), uniq.end()), uniq.end());
    EXPECT_EQ(uniq.size(), static_cast<std::size_t>(n))
        << "连续 " << frames << " 帧提交应覆盖全部 " << n
        << " 个槽(证明帧环在单调推进),实际只用到 " << uniq.size() << " 个";
  }

  // ---- 事实 ②(弱形式):期间不该有任何整页淘汰 ----
  // evict_count 是"确实发生了淘汰"的**设备级累计值**,只能比增量。
  EXPECT_EQ(a->stats().evict_count, evict_base)
      << "这些页还有活子块、空置也没到阈值,期间不应发生任何整页淘汰";
}

// ----------------------------------------------------------------------------
// 分配器帧边界是否真的被驱动(强形式)
// ----------------------------------------------------------------------------
//
// 与上一个用例的分工:那里只证"帧环在转",这里证"帧边界真的落到了 L1 分配器上"
// —— 即 set_frame_tick → on_frame_boundary → clear_pages_in_flight +
// allocator->begin_frame() 这条链**在真实提交路径里**是活的。
//
// 用一页**没有任何活子块**的内存页做探针:
//   - live_allocations == 0 && free_bytes == page_size,所以
//     `p.live != 0` / `free_bytes < page_size` 两道闸门天然通过;
//   - 剩下的两道闸门(live==0 之后的 idle_frames 计数、in_flight 解除)才是
//     begin_frame / set_page_in_flight 的唯一驱动力;
//   - idle_frames 在"完全空置"时**每帧 +1**。所以:
//       一次设备提交 ?
//         推进 N 次帧边界 => idle_frames 恰好 +N(并 trim 一次);
//         in_flight 先立后破(同一个 begin_frame 里先 clear 再 advance,
//         顺序见 vulkan_render_device.cpp:on_frame_boundary)=>
//         提交返回后页面**在册且不在途**。
//   于是把 idle_frames 的**增量**钉在 k*N 上:
//       0   = 链路断(set_frame_tick 没接上 / begin_frame 没人调),死代码;
//       N-1 = 漏槽(只 clear 不 advance,或只推进了 partial);
//       2N  = 重复驱动(同一次提交被 tick 了两次)。
//
// 注意:这不是在造一条"测试专用通路"。页空闲、缓冲常驻,全部是真实的资源
// 生命周期;唯一的"人造"部分是**数字段**:让空闲计数先归零,才能用增量断言
// 而不受设备 init 期间历史帧的影响(那部分是不可控的)。
TEST(SyncCore, AllocatorFrameBoundaryIsActuallyDriven) {
  auto d = require_sw_device();
  if (!d) return;
  res::IMemoryAllocator* a = d->memory_allocator();
  ASSERT_NE(a, nullptr);
  dev::IFrameSync* fs = d->frame_sync();
  ASSERT_NE(fs, nullptr);
  const dev::u32 n = fs->in_flight_count();
  ASSERT_TRUE(n == 2u || n == 3u) << "帧环大小必须是 2 或 3,实为 " << n;

  auto* vd = dynamic_cast<magicblocks::backend::VulkanRenderDevice*>(d.get());
  ASSERT_NE(vd, nullptr);
  auto* core = vd->sync_core_impl();
  ASSERT_NE(core, nullptr);

  res::PipelineDesc pd = graphics_desc();
  pd.push_constant_bytes = 64;
  pd.depth.depthTest = false;
  pd.depth.depthWrite = false;
  auto pr = d->create_pipeline(pd);
  ASSERT_TRUE(pr.ok()) << pr.message();
  res::IPipeline* pl = pr.value().get();

  dev::TargetDesc td;
  td.kind = dev::TargetDesc::Kind::Offscreen;
  td.extent = {16, 16};
  td.with_depth = false;
  auto tgt = d->create_offscreen_target(td);
  ASSERT_TRUE(tgt.ok()) << tgt.message();

  // ---- 造一页"完全空置、无活子块"的探针页 ----
  // 先 reserve 一页独占,再分配/立即 free 一个小块:释放后空块合并,
  // 该页就回到 free_bytes == page_size / live == 0 的"可淘汰候选"状态,
  // 与 Allocator.PageNotEvictedWhileInFlight 的造法同口径。
  ASSERT_TRUE(a->reserve_pages(res::MemoryKind::Index, 1).ok());
  auto probe_r = a->allocate(res::MemoryKind::Index, 4096, 64);
  ASSERT_TRUE(probe_r.ok()) << probe_r.message();
  const dev::u32 probe_page = probe_r.value().id.page_id;
  ASSERT_TRUE(a->free(probe_r.value().id).ok());

  // 再建一个**常驻不释放**的顶点缓冲:它让每次提交都能真的绑上 GPU 内存,
  // 从而在提交路径上真的走过 record_live_resource → mark_pages_in_flight。
  // 它用的是另一页(Vertex 桶),与探针页互不干扰。
  ASSERT_TRUE(a->reserve_pages(res::MemoryKind::Vertex, 1).ok());
  res::BufferDesc vbd;
  vbd.size = 4096;
  vbd.usage = res::BufferUsage::Vertex;
  auto vbr = d->create_buffer(vbd);
  ASSERT_TRUE(vbr.ok()) << vbr.message();
  res::IBuffer* vb = vbr.value().get();

  // 前置:探针页确实处在"可淘汰候选"状态。若不成立,后面 idle_frames 根本不会
  // 增长(它只为完全空置的页 +1),用例会失去判别力 —— 先停下来问。
  {
    res::PageDesc pre{};
    ASSERT_TRUE(a->page_desc(probe_page, &pre));
    ASSERT_EQ(pre.live_allocations, 0u)
        << "探针页还有活子块,idle_frames 永远不会增长,本用例失去判别力";
    ASSERT_EQ(pre.free_block_count, 1u)
        << "探针页的空块没有合并回整页,free_bytes < page_size,"
           "trim() 的碎片闸门会挡住它,本用例失去判别力";
    ASSERT_FALSE(pre.in_flight) << "探针页此刻不该在途(还没提交过它)";
  }

  // ---- 数字段:把空闲计数清零,好看增量 ----
  // 设备 init 及之前的预热提交给这一页留下的历史 idle_frames 是不可控的;
  // 走掉本帧边界后,完全空置的页 idle_frames 会被重新清零一次,增量就可以
  // 直接从 0 起算。这里用**分配再释放**的同一手法让它立刻"完全空置":
  // 已经空置了,只需推进一帧边界把计数从"历史值"拉回 0 起点即可。
  // 做法不是伪造状态,而是**让 trim 的阈值永远够不到**(见下面的阈值前置检查),
  // 于是探针页在整个用例期间一直活着,idle_frames 单调 +1。
  {
    res::PageDesc pre{};
    ASSERT_TRUE(a->page_desc(probe_page, &pre));
    // 阈值必须远大于本用例推进的帧数,否则半路整页被淘汰、page_desc 失败,
    // 我们就分不清"淘汰了"和"链路断了"。
    ASSERT_LT(static_cast<dev::u64>(3) * n, static_cast<dev::u64>(a->page_idle_frames()))
        << "淘汰阈值 " << a->page_idle_frames() << " 不大于 3*N = " << (3 * n)
        << ",本用例推进期间探针页会被淘汰,断言失去判别力 —— 停下来问,不要放宽";
  }

  auto submit_once = [&]() -> dev::Status {
    auto cr = d->create_command_list();
    if (!cr.ok()) return cr.status();
    res::ICommandList* cl = cr.value().get();
    res::RenderPassDesc rp;
    rp.target = tgt.value().get();
    rp.clear_color = true;
    dev::Status s = cl->begin_pass(rp);
    if (!s.ok()) return s;
    s = cl->bind_pipeline(pl, pl->hash());
    if (!s.ok()) return s;
    s = cl->bind_vertex_buffer(vb->id(), 0, 0);
    if (!s.ok()) return s;
    res::DrawArgs da;
    da.vertex_count = 3;
    s = cl->draw(da);
    if (!s.ok()) return s;
    s = cl->end_pass();
    if (!s.ok()) return s;
    return d->submit_command_list(*cl);
  };

  auto idle_of_page = [&]() -> dev::u64 {
    res::PageDesc p{};
    if (!a->page_desc(probe_page, &p)) return 0xFFFFFFFFull;
    return p.idle_frames;
  };

  // 起点取自**刚提交完的那一刻**:从这一刻起,每个新提交恰好驱动一帧边界。
  // (设备 init 预热期间推过的那些帧边界已经落在 d0 里,用增量把它们消掉。)
  const dev::u64 d0 = idle_of_page();
  ASSERT_NE(d0, 0xFFFFFFFFull) << "探针页已被淘汰,无法继续";
  const dev::u64 evict_base = a->stats().evict_count;

  for (dev::u32 k = 1; k <= 3; ++k) {
    dev::Status s = submit_once();
    ASSERT_TRUE(s.ok()) << "第 " << k << " 次提交失败: " << s.message;

    const dev::u64 dk = idle_of_page();
    ASSERT_NE(dk, 0xFFFFFFFFull)
        << "第 " << k << " 次提交后探针页不见了(=被淘汰);阈值前置检查没生效";
    // ---- 一次提交 = allocator->begin_frame() 恰好一次 ⇒ 空置帧数 +1 ----
    //
    // 这里**不是** k*N。N 是并行槽位数(同时在飞的帧数上限),不是"每帧推进几格":
    // 每一帧都在自己的 begin_frame() 里过一遍帧边界,所以一个完全空置的页
    // 每帧 +1,N 帧之后才 +N。写成 k*N 等于把"每次提交驱动了 N 次帧边界"
    // 当成正确结果,方向是反的。
    EXPECT_EQ(dk - d0, static_cast<dev::u64>(k))
        << "提交 " << k << " 次后,空置页的 idle_frames 应增加 k = " << k << ",实际增加 "
        << (dk - d0)
        << "。0 = set_frame_tick → on_frame_boundary → allocator->begin_frame()"
           " 这条链没有真正驱动分配器帧边界(in_flight 守卫会静默退化成死代码);"
           "偏大 = 一次提交驱动了多次帧边界";
  }

  // ---- 在途标记必须是"先立后破"(用**真的被绑定**的那一页来验)----
  //
  // 探针页是 Index 桶里一个没人用的空页,任何提交都不会绑它,
  // 因此它从头到尾都不会被标成在途 —— 拿它验"标记能不能解除"是无效判据
  // (标记从未立起,自然也无从泄漏)。真正会被 mark_pages_in_flight 记上的
  // 是 vb 所在的那一页。
  //
  // 立:submit_command_list → mark_pages_in_flight(本帧槽)
  // 破:该槽的 begin_frame → clear_pages_in_flight(同槽)
  // 所以提交返回的**那一刻**它应在途(本帧 fence 还没信号),再跑满 N 帧
  // (每个槽都被轮过一遍)之后必须解除。恒真 = 破的路径从没跑过。
  const dev::u32 vb_page = vb->id().page_id;

  // ---- 在途标记必须是"先立后破"(用**真的被绑定**的那一页来验)----
  //
  // 探针页是 Index 桶里一个没人用的空页,任何提交都不会绑它,
  // 因此它从头到尾都不会被标成在途 —— 拿它验"标记能不能解除"是无效判据
  // (标记从未立起,自然也无从泄漏)。真正会被 mark_pages_in_flight 记上的
  // 是 vb 所在的那一页。
  //
  // 立:submit_command_list → mark_pages_in_flight(本帧槽)
  // 破:该槽的 begin_frame → clear_pages_in_flight(同槽)
  //
  // ⚠️ 判据不能写成"再跑 N 帧就该解除":只要提交里还绑着这个 buffer,
  //    每一帧都会重新把它标成在途,跑多少帧都还是 true(实测 refs 恒为 2、
  //    两个槽都攥着同一个页)。那会把**正确行为**判成失败。
  //    要观测"会解除",必须先**停止引用该页**,再用不含它的提交把帧环空转
  //    满 N 帧 —— 那时该页最后一次的标记才轮到被对应的槽清掉。
  {
    dev::Status s = submit_once();
    ASSERT_TRUE(s.ok()) << "在途闭环检查的提交失败: " << s.message;
    res::PageDesc in_flight_before{};
    ASSERT_TRUE(a->page_desc(vb_page, &in_flight_before));
    EXPECT_TRUE(in_flight_before.in_flight)
        << "峰值缓冲所在页在提交刚返回时应标为在途(其槽位 fence 尚未信号);"
           "恒假 = mark_pages_in_flight 没有生效,trim() 会在 GPU 还在读内存时整页退给 VMA";
  }

  // 空转提交:同一条命令流,但**不绑** vb,因此不会重新标记它的页。
  auto submit_without_vb = [&]() -> dev::Status {
    auto cr = d->create_command_list();
    if (!cr.ok()) return cr.status();
    res::ICommandList* cl = cr.value().get();
    res::RenderPassDesc rp;
    rp.target = tgt.value().get();
    rp.clear_color = true;
    dev::Status s = cl->begin_pass(rp);
    if (!s.ok()) return s;
    s = cl->bind_pipeline(pl, pl->hash());
    if (!s.ok()) return s;
    res::DrawArgs da;
    da.vertex_count = 6;
    da.instance_count = 16;
    s = cl->draw(da);  // fullscreen.vert 不读顶点缓冲:不绑 vb 也能正常出图
    if (!s.ok()) return s;
    s = cl->end_pass();
    if (!s.ok()) return s;
    return d->submit_command_list(*cl);
  };

  // 空转 N 帧:每一帧的 begin_frame 各清一个槽,跑满 N 帧后 vb 的页最后一次
  // 登记必然已被清掉,引用计数归零 → in_flight 解除。
  for (dev::u32 k = 0; k < n; ++k) {
    dev::Status s = submit_without_vb();
    ASSERT_TRUE(s.ok()) << "空转第 " << k << " 帧提交失败: " << s.message;
  }
  {
    res::PageDesc in_flight_after{};
    ASSERT_TRUE(a->page_desc(vb_page, &in_flight_after));
    const auto& per_slot = vd->pages_in_flight_for_test();
    std::size_t still_held = 0;
    for (const auto& v : per_slot) {
      still_held += static_cast<std::size_t>(
          std::count(v.begin(), v.end(), vb_page) ? 1 : 0);
    }
    EXPECT_FALSE(in_flight_after.in_flight)
        << "停止引用该页并空转 N = " << n
        << " 帧后,它的在途标记应已被各槽的 fence 依次解除;"
           "恒真 = clear_pages_in_flight 从没真正退过引用,页会被永久钉住、永不淘汰"
        << "(仍持有它的槽数=" << still_held << ")";
  }

  // 尾判:3*N 帧远未达到淘汰阈值(已在循环外断言),期间不应有任何整页淘汰。
  EXPECT_EQ(a->stats().evict_count, evict_base)
      << "探针页还有空闲、其它页也有活子块,期间不该淘汰任何页";
}


