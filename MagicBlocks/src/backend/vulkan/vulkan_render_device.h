// MagicBlocks —— Vulkan 后端:设备实现 + 工厂(spec L0 D1/D2/D7/D8)
#pragma once

#include <memory>
#include <string>
#include <vector>

#include "backend/vulkan/vk_common.h"
#include "backend/vulkan/vulkan_allocator.h"
#include "backend/vulkan/vulkan_frame_sync.h"
#include "backend/vulkan/vulkan_instance.h"
#include "backend/vulkan/vulkan_pipeline.h"
#include "backend/vulkan/vulkan_sync_core.h"
#include "backend/vulkan/vulkan_target.h"
#include "magicblocks/device/render_device.h"
namespace magicblocks::backend {

// 前置声明：bind_known_buffers 只需要引用，不需要完整类型；
// 完整定义在 vulkan_command.h，设备实现文件里再 include。
class VulkanCommandList;

class VulkanRenderDevice final : public device::IRenderDevice {
 public:
  VulkanRenderDevice() = default;
  ~VulkanRenderDevice() override;

  // 两段式:先 init(会做能力探测与强筛),再使用。
  device::Status init(const device::DeviceDesc& desc, const std::string& device_override);

  device::BackendType backend() const override { return device::BackendType::Vulkan; }
  const device::DeviceCaps& caps() const override { return caps_; }

  device::Result<std::unique_ptr<device::IRenderTarget>> create_offscreen_target(
      const device::TargetDesc& desc) override;
  device::Result<std::unique_ptr<device::IRenderTarget>> create_window_target(
      const device::TargetDesc& desc) override;
  device::Result<std::unique_ptr<device::ISwapchain>> create_swapchain(
      const device::NativeSurface& surface, device::Extent2D extent) override;

  // 后端内部：按指定 MSAA 采样数建交换链（0/1 等同关掉）。
  //
  // 为什么需要这条单独入口：MSAA 图必须在 init() 里建，而 set_requested_msaa()
  // 又必须早于 init()。公共面 create_swapchain() 不带采样参数，而上层
  // （Android 的 scene_renderer / CLI）只能看到 device::ISwapchain，
  // 拿不到 backend 私有的 VulkanSwapchain::set_requested_msaa()。
  // 所以把"设采样数 + init"这组顺序敏感的两步收进后端，调用方只传数字。
  //
  // 设备不支持时**不报错**：退回 1x 并在 stderr/Android log 说明原因 ——
  // MSAA 不可用绝不该挡住上屏。
  device::Result<std::unique_ptr<device::ISwapchain>> create_swapchain_with_msaa(
      const device::NativeSurface& surface, device::Extent2D extent, std::uint32_t msaa_samples);

  device::IFrameSync* frame_sync() override { return frame_sync_.get(); }
  // G8 / t13: expose the shaped-pipeline factory to L3 through the L0 abstraction.
  // L1 keeps the resolver table (src/core/pipeline_factory_table.cpp); this device
  // only has to say "which factory belongs to me", so no static object is needed.
  resources::IPipelineFactory* pipeline_factory() override {
    return resources::resolve_pipeline_factory(this);
  }


  // ---- L1 资源层 ----
  resources::IMemoryAllocator* memory_allocator() override { return allocator_.get(); }
  device::Result<std::unique_ptr<resources::IBuffer>> create_buffer(
      const resources::BufferDesc& desc) override;
  device::Result<std::unique_ptr<resources::ITexture>> create_texture(
      const resources::TextureDesc& desc) override;
  device::Result<std::unique_ptr<resources::ISampler>> create_sampler(
      const resources::SamplerDesc& desc) override;
  device::Result<std::unique_ptr<resources::ICubeMap>> create_cube_map(
      const resources::CubeMapDesc& desc) override;
  device::Result<std::unique_ptr<resources::IPipeline>> create_pipeline(
      const resources::PipelineDesc& desc) override;
  // 后端内部：按指定附件形状建管线。
  // 为什么需要：VkPipeline 绑定的 VkRenderPass 必须与 submit_device_frame
  // 用来 begin_pass 的 render pass **逐项兼容**（颜色格式/采样数/深度）。
  // 公共 create_pipeline(desc) 固定用 RGBA8 离屏形状，渲染到交换链
  // （BGRA8 或 RGBA8，由 surface 决定）时必须用这条入口。
  //
  // ⚠️ 类型必须写全限定 `resources::AttachmentShape`。
  // 这里曾经写裸名 `AttachmentShape`，而它的定义在 resources/pipeline.h（纯值类型,
  // 零 Vk*，是 G8 裁决后的公共面）。裸名会让不同 TU 按各自的 include 顺序解析到
  // 不同的类型 —— 表现为 `dumpbin` 里库中有符号、链接时却 LNK2019 签名不匹配
  // （库内是 @73 = resources::,测试侧请求 @23 = backend::）。
  device::Result<std::unique_ptr<resources::IPipeline>> create_pipeline_for_shape(
      const resources::PipelineDesc& desc, const resources::AttachmentShape& shape);
  device::Result<std::unique_ptr<resources::ICommandList>> create_command_list() override;

  // L0→L1:把已录制完成的命令列表真正编码成 vkCmd* 并提交。
  // 内部转调 submit_device_frame()(那条通路早就实现完整,只是从来没接到
  // 公开接口上,导致 L2/L3 录完命令后无法出图)。
  device::Status submit_command_list(resources::ICommandList& list) override;

  device::Status notify_lifecycle(const device::AppLifecycleEvent& ev) override;
  device::Status wait_idle() override;

  // 测试/CLI 用:直接拿到 context 与帧同步的具体类型
  const VulkanContext& context() const { return ctx_; }
  VulkanFrameSync* frame_sync_impl() { return frame_sync_.get(); }
  // L1:分页分配器(公共面通过 IRenderDevice::memory_allocator() 取接口指针)
  VulkanPagingAllocator* allocator_impl() { return allocator_.get(); }
  // L0 同步核心(测试/CLI 探针用):N 槽 semaphore+fence+持久命令缓冲。
  VulkanSyncCore* sync_core_impl() { return sync_core_.get(); }
  // 按槽记账的在途页清单(**只读**,诊断/测试用)。
  //
  // 存在的理由:mark_pages_in_flight 把页记进"本帧槽",clear_pages_in_flight 在
  // 该槽的帧边界把同一槽的清单全部清空。两条路径用的槽号必须一致,否则标记有立
  // 无破、页面永远淘汰不掉 —— 而这个错误在 page_desc().in_flight 上只表现为
  // "一个恒真的布尔",看不出是哪一槽攥着它。把记账直接暴露出来,才能让
  // SyncCore.AllocatorFrameBoundaryIsActuallyDriven 在失败时指出具体槽位。
  const std::vector<std::vector<resources::u32>>& pages_in_flight_for_test() const {
    return pages_in_flight_;
  }
  // 最近一次 submit_command_list() 成功时,本帧在 submit_device_frame **之前**
  // 丢弃旁路 render pass 的次数。**恒为 1**;任何其他值都说明
  //   - > 1:提交之后又丢弃了一次 —— 那会把这一次绘制吃掉(整屏只剩清屏色);
  //   - 0 :根本没丢弃 —— 调用方 target 的 VkImage 同时挂在两条矛盾的布局链上。
  // 供 SyncCore.FrameRingAdvancesPerSubmit 在外部调用方跑完后复核这条不变量:
  // 帧环要换槽复用命令缓冲与目标,就必须先把旁路彻底丢掉,这是同一件事的两面。
  std::uint32_t last_frame_side_channel_abandons() const {
    return last_frame_side_channel_abandons_;
  }
  // 资源析构时把自己从 known_* 表里摘掉。
  //
  // 为什么必须有:那几张表存的是**裸指针**(设备不拥有资源,所有权在调用方的
  // unique_ptr),而 ResourceId 会被分页分配器回收复用。表若只增不减,反查就会
  // 命中已释放的对象 —— 实测表现是 0xC0000005 / 堆损坏 0xC0000409 /
  // "vkBeginCommandBuffer: Invalid commandBuffer",而且是"单跑绿、全量红"。
  //
  // 由 VulkanBuffer / VulkanTexture / VulkanCubeMap / VulkanSampler 的析构函数
  // 调用。设备先死(析构)时活着,资源先死时表里的指针立刻消失,两个方向都安全。
  void forget_known_buffer(const resources::IBuffer* buffer);
  void forget_known_texture(const resources::ITexture* texture);
  void forget_known_cube_map(const resources::ICubeMap* cube);
  // 校验层的可计量判据(Device.CreateAndDestroy / RunsFifteenFrames)
  std::uint32_t validation_error_count() const override { return inst_.error_count; }
  std::uint32_t validation_message_count() const override { return inst_.message_count; }
  bool validation_enabled() const override { return inst_.validation_enabled; }

 private:
  // 设备创建的缓冲资源句柄 → 实现指针。
  //
  // 为什么需要:公共面 resources::ICommandList::bind_vertex_buffer 只收
  // ResourceId(值语义,M3,这是正确的公共设计),命令列表自己拿不到 IBuffer*;
  // 而设备帧提交必须拿到 VkBuffer 才能 vkCmdBindVertexBuffers。这张表就是
  // 两者之间缺失的那座桥 —— 提交前按命令里的句柄把指针补进命令列表。
  void bind_known_buffers(VulkanCommandList& list) const;
  // 纹理/采样器/AO 的同一座桥(P0 / X1 的 bind_sampled_texture 需要 ITexture* /
  // ISampler* 才能取 VkImageView / VkSampler)。只登记**设备态**创建出来的对象;
  // 对象析构时由 WeakTexture 回调摘掉(设备不拥有它们)。
  void bind_known_textures(VulkanCommandList& list) const;

  // 提交前把一批页标成"在途",该槽 fence 之后清掉(见 pages_in_flight_ 的说明)。
  void mark_pages_in_flight(std::uint32_t slot, const std::vector<resources::u32>& pages);
  void clear_pages_in_flight(std::uint32_t slot);
  // 帧边界回调挂在同步核心上,lambda 要访问私有成员,所以它必须是成员函数
  // (由 init() 用 [this](u64, u32) 装上去)。
  void on_frame_boundary(std::uint64_t frame_index, std::uint32_t slot);

  InstanceBundle inst_{};
  VulkanContext ctx_{};
  device::DeviceCaps caps_{};
  // L1 分页分配器。必须**晚于** ctx_ 构造、**早于** VkDevice 销毁:
  // 它在析构时会 vmaDestroyAllocator,而 VMA 持有 VkDevice。
  std::unique_ptr<VulkanPagingAllocator> allocator_;
  // 注意:frame_sync_ 必须**晚于**每个帧槽对应的 VkDevice 之后再析构,
  // 所以这里用 unique_ptr 并在 ~VulkanRenderDevice 里显式先 reset,
  // 保证 "先销毁 fence、再销毁 VkDevice" 的顺序。
  std::unique_ptr<VulkanFrameSync> frame_sync_;
  // ---- L0 多帧并行同步核心(2026-11-11 重构)----
  //
  // 为什么挂在这里:VulkanContext 是纯句柄容器,会被 backend 各层**按值**持有
  // (Swapchain / OffscreenTarget / VulkanCommandList 各存一份),把拥有型资源放进去
  // 会让"谁销毁"含糊、且变成 N 份拷贝持有同一组句柄;thread_local 则因 VkDevice
  // 句柄值被驱动复用而误判"同一个设备",拿已销毁的 VkFence 去复位 → 崩溃(实测 106 条用例)。
  // 放在设备成员上,生命周期天然等于 VkDevice。
  //
  // 它**取代**了原先这里的 submit_fences_[4] 轮转池:那套池只解决"别每帧建/毁 fence",
  // 但提交后仍要同步等 fence,下一帧还是串行 —— 等于"池化了一个串行锁"。
  // 现在 put/fence/命令缓冲/延迟销毁全部收敛到 VulkanSyncCore 的 N 槽上,N 与
  // IFrameSync::in_flight_count() 是**同一个数**(DeviceDesc::in_flight_frames),不再两套口径。
  std::unique_ptr<VulkanSyncCore> sync_core_;
  bool logical_ready_ = false;
  // ResourceId → 缓冲实现指针(见 bind_known_buffers 的说明)。
  // 设备不拥有这些对象(所有权在调用方的 unique_ptr),这里只存裸指针;
  // 缓冲析构时通过 WeakBuffer 回调把自己摘掉,避免悬垂。
  //
  // 【2026-11-11 补记:上面这句 WeakBuffer 承诺曾经是**空头支票**】
  // 查 HEAD(00f0237) 源码:这三个表的注释都写着"析构时由 Weak* 回调摘掉",
  // 但 vulkan_buffer.cpp / vulkan_texture.cpp / vulkan_cubemap.cpp 里
  // **没有任何摘除代码**(`forget_known|WeakBuffer|回调` 零命中),
  // 而 vulkan_render_device.cpp 里这三张表**只有 .clear(),全文无 erase**
  // (全文件唯一的 erase 是 known_samplers_ 的,与此无关)。
  // 即:它们是只增不减的裸指针表,而 bind_known_buffers / bind_sampled_texture
  // 每帧都在遍历它们做反查。对象析构后指针留在表里,ResourceId 一旦被复用,
  // 反查就命中已释放内存 —— 实证形态是 0xC0000005 /
  // "vkBeginCommandBuffer: Invalid commandBuffer" / 且只在全量串跑出现。
  //
  // 现在由 VulkanPagingAllocator::owner_device() 反查驱动 forget_known_*()
  // 三个函数兑现这句承诺(见各资源析构里的 set/forget_known_* 调用)。
  // **别把 forget_known_* 当成可选优化删掉** —— 删了就是把这条老路放回来。
  std::vector<std::pair<resources::ResourceId, resources::IBuffer*>> known_buffers_;
  // 纹理 / 立方体贴图 → 对象指针(bind_sampled_texture 设备态需要)。
  // 立方体贴图与 ITexture 是**两个不同的接口**(ICubeMap 不是 ITexture 的子类:
  // 它只有 face/flush,没有 readback/mip),所以分两张表登记,命令列表按句柄
  // 各查各的 —— 不靠 dynamic_cast 猜类型。
  std::vector<std::pair<resources::ResourceId, resources::ITexture*>> known_textures_;
  std::vector<std::pair<resources::ResourceId, resources::ICubeMap*>> known_cubes_;
  // 采样器句柄 → ISampler*。
  std::vector<std::pair<resources::ResourceId, resources::ISampler*>> known_samplers_;
  // ---- 页在途标记:**按槽记账**(2026-11-11,修正一次粒度错误)----
  //
  // 目的:让 IMemoryAllocator::trim() 里 `if (p.in_flight) continue;`
  // (vulkan_allocator.cpp:1115,注释自称「M4 硬条件」)真正生效。改造前
  // IMemoryAllocator::begin_frame() 在 src/ 零调用 → trim() 零调用 →
  // set_page_in_flight() 零调用 → p.in_flight 恒为 false,那是一道死代码。
  //
  // ⚠️ 粒度:每槽一份,不能是一份全局集合。
  // begin_frame 只在**当前槽**就绪时触发回调,此刻其余 N−1 个槽可能还在 GPU 上跑。
  // 若回调里无条件全清,那么"第 f 帧提交、第 f+1 帧被上层销毁的缓冲"其页在
  // 第 f+1 帧边界之后就失去保护,trim() 一旦选中就能在 GPU 还在读时整页退给 VMA。
  // 所以:提交时把页记入**当前槽**那一份;某槽 fence 确认后只清该槽那一份;
  // 一个页只要还在**任一槽**的集合里,对分配器就报 in_flight=true。
  std::vector<std::vector<resources::u32>> pages_in_flight_;
  // 每个页当前被多少个槽登记着(引用计数;>0 ⇒ in_flight=true)。
  // 用引用计数而不是"重新扫全部槽":N 只有 2~3,但清一个槽时逐页重扫会变成 O(P·N)。
  std::vector<std::pair<resources::u32, std::uint32_t>> page_slot_refs_;
  // 最近一次成功提交时"提交前丢弃旁路"的次数(见 last_frame_side_channel_abandons())。
  std::uint32_t last_frame_side_channel_abandons_ = 0;
};

// 测试钩子:强制把枚举到的设备"假装"改名(用于 RefusesPhysicalGpu)。
// 空串 = 不覆盖。仅影响强筛判定,不改变真实设备。
void set_physical_device_override(const std::string& name);
const std::string& physical_device_override();

// 工厂:由 core/device_factory.cpp 调用(公共面不出现 Vk*)。
device::Result<std::unique_ptr<device::IRenderDevice>> create_vulkan_device(
    const device::DeviceDesc& desc);

}  // namespace magicblocks::backend
