// MagicBlocks —— Vulkan 后端:L1 立方体贴图(R1 扩展,整块图像页)
//
// 与 VulkanTexture 同源(ruling 2:R1 的内存一律来自分页分配器),差别在"形状":
//
//   **一张 6 层 VkImage + 一整块图像页 + 一个 VIEW_TYPE_CUBE 视图。**
//
// 为什么走**图像页**(allocate_image_page)而不是 Texture 桶的桶内子分配:
//   第一版用 `allocate(MemoryKind::Texture, 24 MiB)` 给 1024²×6 面的立方体要内存,
//   被 "单资源超过整页 8 MiB" 拦下 —— 那条限制针对的是**桶内子分配**
//   (多个小资源共用一页的 Texture/Vertex/Index 桶)。
//   立方体贴图和 L0 的离屏附件是同一类东西:**整块图像**,它的内存本来就该
//   按 `VkMemoryRequirements` 要一整页(见 vulkan_target.cpp 的离屏附件,
//   走的正是 allocate_image_page + bind_image_page)。
//   C7 的原文是"不裸调 vkAllocateMemory / vmaAllocateMemory",不是
//   "所有资源都必须塞进 8 MiB 的 Host 页";而硬件上的 cube 视图又要求六层
//   在**同一张 image** 里(六张单层图拼不成 cube 视图)——
//   所以"分页到六张图"在这条硬件约束下不成立,正确的形态就是整块图像页。
//
// 内存账(1024² RGBA8):6 × 4 MiB = 24 MiB = 一张图像页。
//
// 上传:upload_face 把面数据 memcpy 进该页映射的第 i 层区间;flush() 用一次
// vkCmdCopyBufferToImage(每个已上传面一个区域)+ 一次整图布局转换
// (UNDEFINED/READ_ONLY → TRANSFER_DST → SHADER_READ_ONLY)。
//
// 没有 readback / mip / samples / 其它用途:这些在公共接口里根本不存在,
// 所以这里也不需要任何"不支持"分支(能力边界是结构性的)。
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "backend/vulkan/vk_common.h"
#include "backend/vulkan/vulkan_allocator.h"
#include "backend/vulkan/vulkan_target.h"
#include "mb_internal/magicblocks/resources/texture.h"

namespace magicblocks::backend {

using resources::CubeFace;
using resources::CubeMapDesc;
using resources::ICubeMap;
using resources::ResourceId;

class VulkanCubeMap final : public ICubeMap {
 public:
  // 纯软件态:只建映射存储,不建 VkImage(单测在没有设备的机器上也能验校验逻辑)。
  VulkanCubeMap(resources::IMemoryAllocator* allocator, const CubeMapDesc& desc);
  // 设备态:额外建 6 层 VkImage + cube 视图,并整块绑定到一张图像页。
  VulkanCubeMap(const VulkanContext& ctx, resources::IMemoryAllocator* allocator,
                const CubeMapDesc& desc);

  ~VulkanCubeMap() override;

  VulkanCubeMap(const VulkanCubeMap&) = delete;
  VulkanCubeMap& operator=(const VulkanCubeMap&) = delete;

  // 两段式创建:构造只存参数,init() 返回具体错误(非法描述 → Invalid/Unsupported)。
  device::Status init();

  // ------------------------------------------------------------ ICubeMap(R1)
  // 立方体的句柄:用 kSamplerPage 之外的保留页号表达"整块图像资源",id 是槽位号。
  ResourceId id() const override { return id_; }
  const CubeMapDesc& desc() const override { return desc_; }
  u64 face_size() const override { return face_bytes_; }
  device::Status upload_face(CubeFace face, const void* data, u64 size_bytes) override;
  device::Status flush() override;
  bool face_uploaded(CubeFace face) const override;

  // ------------------------------------------------------------ 后端查询(单测/提交器用)
  bool has_vk_image() const { return vk_image_ != VK_NULL_HANDLE; }
  VkImageView vk_view() const { return vk_view_; }
  VkFormat vk_format() const { return vk_format_; }
  // 整图占用的图像页(0 个或多个;一个 24 MiB 的立方体 = 1 页)。
  u32 image_page_id() const { return image_page_; }
  // 整图字节数(六面合计,= VkMemoryRequirements::size 的上界)。
  u64 total_bytes() const { return face_bytes_ * resources::kCubeFaceCount; }
  // 待提交的面数(flush 之后归零)。
  u32 pending_face_count() const { return static_cast<u32>(pending_.size()); }
  // 最近一次 flush 失败原因(空 = 成功)。
  const std::string& flush_error() const { return flush_error_; }

 private:
  struct PendingFace {
    CubeFace face = CubeFace::PositiveX;
  };

  device::Status allocate_storage();
  device::Status create_image_object();

  VulkanContext ctx_{};
  bool has_ctx_ = false;
  resources::IMemoryAllocator* allocator_ = nullptr;

  CubeMapDesc desc_{};
  ResourceId id_{};
  u32 image_page_ = ResourceId::kInvalidPage;
  // 软件态下的映射存储(设备态用图像页的常驻映射)。
  std::vector<std::uint8_t> software_storage_;
  std::uint8_t* mapped_ = nullptr;
  u64 face_bytes_ = 0;

  VkFormat vk_format_ = VK_FORMAT_UNDEFINED;
  VkImage vk_image_ = VK_NULL_HANDLE;
  VkImageView vk_view_ = VK_NULL_HANDLE;

  bool face_dirty_[resources::kCubeFaceCount] = {false, false, false, false, false, false};
  std::vector<PendingFace> pending_;
  // flush 是否成功跑过至少一次(决定后续 flush 的 oldLayout)。
  bool flushed_once_ = false;
  std::string flush_error_;
};

}  // namespace magicblocks::backend
