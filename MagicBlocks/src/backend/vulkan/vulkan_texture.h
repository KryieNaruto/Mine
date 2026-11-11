// MagicBlocks —— Vulkan 后端:L1 纹理与采样器(R1 / R4 / R5)
//
// spec R1 / R4 / R5 / §5.4 的落地(plans-l1 §3,rulings 裁定 2):
//   - **句柄 = 偏移(M3)**:VulkanTexture 持有一个 ResourceId,子块从
//     MemoryKind::Texture 桶切出来(裁定 2:纹理一律 HostVisible 页,于是
//     readback / 上传的 CPU 侧恒可用)。不单独走 VMA 分配。
//   - **R5 上传**:upload() 把像素写进纹理子块(mip 区)并入队;submit_pending()
//     (或丢弃时)把队列一次性做成 vkCmdCopyBufferToImage:布局转换 + 拷贝
//     (UNDEFINED → TRANSFER_DST → SHADER_READ_ONLY)。读回走镜像路径
//     (SHADER_READ_ONLY → TRANSFER_SRC → vkCmdCopyImageToBuffer → 回 SHADER_READ_ONLY),
//     **不**做 vkCmdBlitImage —— 离屏目标继续用 IRenderTarget(裁定 2 的遗留约束)。
//     注意:纹理上传**不**额外切 Staging 子块(64 KiB 粒度会让小 mip 浪费整个槽位),
//     暂存同缓冲区语义由纹理自己的 HostVisible 子块承担;缓冲侧的上传(R5 的
//     Upload.StagingGoesThroughAllocator)在 VulkanBuffer 里严格走 Staging 桶。
//   - **读回口径**:行序自左上、逐像素 R,G,B,A,与 IRenderTarget::readback_rgba8
//     一致(golden 可以两者混用)。深度格式 → Unsupported(明确报错)。
//   - **采样器**:SamplerDesc 的 filter/mip_filter/wrap/compare 逐项校验,
//     非法值 → InvalidArgument(绝不"就近取一个合法值")。
#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "backend/vulkan/vk_common.h"
#include "backend/vulkan/vulkan_allocator.h"
#include "mb_internal/magicblocks/device/result.h"
#include "mb_internal/magicblocks/resources/memory.h"
#include "mb_internal/magicblocks/resources/texture.h"
// VulkanContext / backend::Status 别名(见 vulkan_buffer.h 的同一条注释)。
#include "backend/vulkan/vulkan_target.h"

namespace magicblocks::backend {

using resources::CompareOp;
using resources::FilterMode;
using resources::ISampler;
using resources::ITexture;
using resources::MemoryKind;
using resources::ResourceId;
using resources::SamplerDesc;
using resources::TextureDesc;
using resources::TextureUsage;
using resources::u32;
using resources::u64;
using resources::WrapMode;

// ---------------------------------------------------------------- 纹理

class VulkanTexture final : public ITexture {
 public:
  // 纯软件态:用父分配器已预取的 Texture 桶页,不建 VkImage。
  VulkanTexture(resources::IMemoryAllocator* allocator, const TextureDesc& desc);
  // 设备态:额外建真实 VkImage 并绑定到子块所在页的偏移处。
  VulkanTexture(const VulkanContext& ctx, resources::IMemoryAllocator* allocator,
                const TextureDesc& desc);

  ~VulkanTexture() override;

  VulkanTexture(const VulkanTexture&) = delete;
  VulkanTexture& operator=(const VulkanTexture&) = delete;

  // 两段式创建:构造只存参数,init() 返回具体错误。
  device::Status init();

  // ------------------------------------------------------------ ITexture(R1)
  ResourceId id() const override { return id_; }
  const TextureDesc& desc() const override { return desc_; }
  u64 size() const override { return id_.size; }
  MemoryKind allocation_kind() const override { return MemoryKind::Texture; }

  // R5:像素进 Staging 桶暂存子块;真正 GPU 拷贝在 submit_pending() 里发生。
  device::Status upload(const void* data, u64 size_bytes, u32 level = 0) override;
  // 纹理 → CPU。行序自左上、RGBA8;深度格式 → Unsupported。
  device::Status readback_rgba8(std::uint8_t* out, std::size_t size_bytes) const override;

  // ------------------------------------------------------------ 后端查询(gtest 用)
  bool has_vk_image() const { return vk_image_ != VK_NULL_HANDLE; }
  VkImage vk_image() const { return vk_image_; }
  bool has_vk_view() const { return vk_view_ != VK_NULL_HANDLE; }
  VkImageView vk_view() const { return vk_view_; }
  VkFormat vk_format() const { return vk_format_; }

  u32 page_id() const { return id_.page_id; }
  // 某 mip 级的紧凑字节数(行间无 padding;单测算期望长度用)。
  u64 mip_size_bytes(u32 level) const;
  // 各 mip 级字节数之和(== size() 的组成)。
  u64 total_pixel_bytes() const;

  // R5 上传队列的可观测面。
  u32 pending_upload_count() const { return static_cast<u32>(pending_.size()); }
  u64 pending_upload_bytes() const { return pending_bytes_; }
  u64 upload_begin_count() const { return upload_begin_count_; }
  u64 uploaded_bytes_total() const { return uploaded_bytes_total_; }
  // 纹理**从不单独切 Staging 子块**:分配器的子块粒度是 64 KiB(裁定 3),为 64 字节
  // 的 mip0 占一个 64 KiB 槽位会把 Host 页(8 MB = 128 槽)迅速吃光;纹理本身已在
  // HostVisible 页上(裁定 2),所以 CPU 侧暂存直接用纹理自己的子块。
  // 因此这个数恒为 0,保留它是为了让单测显式断言"没有额外 Staging 分配"。
  u64 staging_alloc_count() const { return 0; }
  // 待提交上传的暂存源恒为纹理子块自身(== id())。
  ResourceId pending_staging_id(u32 index) const;
  // 最近一次提交失败的原因(空 = 全部成功)。
  const std::string& upload_commit_error() const { return commit_error_; }

  // 把队列里的上传真正提交到 GPU(单队列 + fence 等待)。软件态下等价于 memcpy。
  // 失败不重试;已成功的部分计入 uploaded_bytes_total。
  device::Status submit_pending();
  // 丢弃尚未提交的上传批(整帧取消),归还暂存子块。
  void discard_pending();

 private:
  struct PendingUpload {
    ResourceId staging{};
    u32 level = 0;
    u64 offset = 0;  // mip 级内的字节偏移
    u64 size = 0;
  };

  device::Status allocate_block();
  device::Status create_image_object();
  device::Status commit_staging();
  // 读回辅助:把 mip 0 的像素搬进 dst(dst 是紧凑 RGBA8)。
  device::Status readback_device(std::uint8_t* out, std::size_t size_bytes) const;
  device::Status readback_software(std::uint8_t* out, std::size_t size_bytes) const;

  VulkanContext ctx_{};
  bool has_ctx_ = false;
  resources::IMemoryAllocator* allocator_ = nullptr;

  TextureDesc desc_{};
  ResourceId id_{};
  void* mapped_ = nullptr;
  u64 texel_bytes_ = 0;

  VkFormat vk_format_ = VK_FORMAT_UNDEFINED;
  VkImage vk_image_ = VK_NULL_HANDLE;
  VkImageView vk_view_ = VK_NULL_HANDLE;

  std::vector<PendingUpload> pending_;
  u64 pending_bytes_ = 0;
  u64 upload_begin_count_ = 0;
  u64 uploaded_bytes_total_ = 0;
  std::string commit_error_;
};

// ---------------------------------------------------------------- 采样器

class VulkanSampler final : public ISampler {
 public:
  // 设备态(建真实 VkSampler);ctx 无效时降级为纯软件态(只做校验与占位)。
  VulkanSampler(const VulkanContext& ctx, const SamplerDesc& desc);
  explicit VulkanSampler(const SamplerDesc& desc);

  ~VulkanSampler() override;

  VulkanSampler(const VulkanSampler&) = delete;
  VulkanSampler& operator=(const VulkanSampler&) = delete;

  device::Status init();

  // ------------------------------------------------------------ ISampler(R1)
  ResourceId id() const override { return id_; }
  const SamplerDesc& desc() const override { return desc_; }
  u32 slot_index() const override { return slot_index_; }

  // ------------------------------------------------------------ 后端查询(gtest 用)
  bool has_vk_sampler() const { return vk_sampler_ != VK_NULL_HANDLE; }
  VkSampler vk_sampler() const { return vk_sampler_; }
  // SamplerDesc 的合法性检查(init() 用它,单测也可直接调以拿到明确 message)。
  static device::Status validate_desc(const SamplerDesc& desc);
  // 析构钩子:设备在已知采样器表里存的是**裸指针**,对象析构时必须把自己摘掉,
  // 否则下一个用例/下一帧会通过悬垂指针读到已销毁的 VkSampler(校验层表现为
  // "invalid VkSampler ... or has been destroyed",SwiftShader 下直接 SEH 崩溃)。
  void set_destroy_hook(std::function<void(VulkanSampler*)> hook) {
    destroy_hook_ = std::move(hook);
  }

 private:
  VulkanContext ctx_{};
  bool has_ctx_ = false;
  SamplerDesc desc_{};
  u32 slot_index_ = 0;
  ResourceId id_{};  // kSamplerPage / offset 0 / size 1 / id = slot_index(见 .cpp)
  VkSampler vk_sampler_ = VK_NULL_HANDLE;
  std::function<void(VulkanSampler*)> destroy_hook_{};
};

// 全局采样器槽位发号(单队列单线程;进程级单调递增,便于命令流去重)。
u32 allocate_sampler_slot();

// ---------------------------------------------------------------- 立方体贴图用的采样器
//
// 为什么要单独一条:tie 在 ISampler 上的 SamplerDesc 是**按描述去重复用**的
// (同一 SamplerDesc 只建一个 VkSampler,身份就是 slot_index),而立方体贴图的
// 描述符要求 VkSampler 带 VK_SAMPLER_CREATE_FLAG_CUBE_COMPATIBLE_BIT。
// 为了让同一个 ISampler 既能给 2D 用又能给 cube 用,这里按同一份 SamplerDesc
// 另建一个"带 cube 位"的 VkSampler,并以描述为键缓存(同描述只建一次)。
// 若调用方给的 desc 本身不合法(wrap 不是 ClampToEdge / 开了比较采样),
// 返回明确错误,不静默改参数。
device::Status cube_sampler_for(const VulkanContext& ctx, const SamplerDesc& desc, VkSampler* out);
// 某个已存在的 VkSampler 是否带该位(跨描述符写入前的可判断据)。
// 说明:Vulkan 没有"查询 VkSampler 创建标志"的接口,所以只能记录本后端自己建过的
// 带标志的采样器句柄 —— 这也是 cube_sampler_for 返回的对象的唯一来源。
bool sampler_has_cube_flag(VkSampler sampler);
// 释放本设备缓存的 cube 采样器(设备销毁前必须调用,且在 vkDestroyDevice 之前)。
// 不调用 → 每个带天空盒的过程都会漏 VkSampler:
//   VUID-vkDestroyDevice-device-05137: ... leaked objects ... VkSampler
// 只处理该 device 名下的条目,不影响同进程内其他设备的缓存。
void destroy_cube_samplers(VkDevice device);

}  // namespace magicblocks::backend
