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
  bool logical_ready_ = false;
  // ResourceId → 缓冲实现指针(见 bind_known_buffers 的说明)。
  // 设备不拥有这些对象(所有权在调用方的 unique_ptr),这里只存裸指针;
  // 缓冲析构时通过 WeakBuffer 回调把自己摘掉,避免悬垂。
  std::vector<std::pair<resources::ResourceId, resources::IBuffer*>> known_buffers_;
  // 纹理 / 立方体贴图 → 对象指针(bind_sampled_texture 设备态需要)。
  // 立方体贴图与 ITexture 是**两个不同的接口**(ICubeMap 不是 ITexture 的子类:
  // 它只有 face/flush,没有 readback/mip),所以分两张表登记,命令列表按句柄
  // 各查各的 —— 不靠 dynamic_cast 猜类型。
  std::vector<std::pair<resources::ResourceId, resources::ITexture*>> known_textures_;
  std::vector<std::pair<resources::ResourceId, resources::ICubeMap*>> known_cubes_;
  // 采样器句柄 → ISampler*。
  std::vector<std::pair<resources::ResourceId, resources::ISampler*>> known_samplers_;
};

// 测试钩子:强制把枚举到的设备"假装"改名(用于 RefusesPhysicalGpu)。
// 空串 = 不覆盖。仅影响强筛判定,不改变真实设备。
void set_physical_device_override(const std::string& name);
const std::string& physical_device_override();

// 工厂:由 core/device_factory.cpp 调用(公共面不出现 Vk*)。
device::Result<std::unique_ptr<device::IRenderDevice>> create_vulkan_device(
    const device::DeviceDesc& desc);

}  // namespace magicblocks::backend
