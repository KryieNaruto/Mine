// MagicBlocks —— Vulkan 后端:L1 立方体贴图实现(R1 扩展,整块图像页)
//
// 契约与设计取舍见 vulkan_cubemap.h(尤其是"为什么 cubemap 走图像页而不是
// Texture 桶的桶内子分配"这一条 —— 那是第一版失败的根因)。
//
// 三条必须守住的纪律:
//   1. 不裸分配:内存一律经 L1 分页分配器的图像页 API(G4 / C7),
//      本文件没有 vkAllocateMemory / vmaAllocateMemory。
//   2. 一次 flush 转整图:拷贝只针对已上传面,但布局转换覆盖六层
//      (描述符写入固定 SHADER_READ_ONLY_OPTIMAL)。
//   3. 失败即报错:描述非法 / 无映射 / 建图失败 / 绑定失败一律带原因返回,
//      绝不退化成"画个纯色"或者"少一面也照用"。
#include "backend/vulkan/vulkan_cubemap.h"

#include <cstring>

#include "backend/vulkan/vulkan_resource_common.h"

namespace magicblocks::backend {

using namespace magicblocks::device;

namespace {

bool face_index_valid(CubeFace face) {
  return static_cast<u32>(face) < resources::kCubeFaceCount;
}

// 全局立方体贴图槽位发号(与采样器同口径:进程级单调递增,便于命令流去重)。
std::uint32_t next_cube_slot() {
  static std::uint32_t slot = 1;
  return slot++;
}

}  // namespace

// ---------------------------------------------------------------- 构造 / 析构

VulkanCubeMap::VulkanCubeMap(resources::IMemoryAllocator* allocator, const CubeMapDesc& desc)
    : has_ctx_(false), allocator_(allocator), desc_(desc) {}

VulkanCubeMap::VulkanCubeMap(const VulkanContext& ctx, resources::IMemoryAllocator* allocator,
                             const CubeMapDesc& desc)
    : ctx_(ctx), has_ctx_(true), allocator_(allocator), desc_(desc) {}

VulkanCubeMap::~VulkanCubeMap() {
  if (ctx_.device != VK_NULL_HANDLE) {
    if (vk_view_ != VK_NULL_HANDLE) vkDestroyImageView(ctx_.device, vk_view_, nullptr);
    if (vk_image_ != VK_NULL_HANDLE) vkDestroyImage(ctx_.device, vk_image_, nullptr);
  }
  vk_view_ = VK_NULL_HANDLE;
  vk_image_ = VK_NULL_HANDLE;
  // 图像页必须在 VkImage 销毁之后退回。这里每次都重新取分配器,不缓存裸指针 ——
  // VulkanPagingAllocator 的生命周期由设备掌握,缓存指针会把"析构顺序"变成隐性前提。
  if (image_page_ != ResourceId::kInvalidPage) {
    if (auto* paging = dynamic_cast<VulkanPagingAllocator*>(allocator_)) {
      paging->unmap_image_page(image_page_);  // 与 create_image_object 的 map 成对
      (void)paging->release_image_page(image_page_);
    }
  }
  image_page_ = ResourceId::kInvalidPage;
  mapped_ = nullptr;
  id_ = ResourceId{};
}

// ---------------------------------------------------------------- init

Status VulkanCubeMap::init() {
  if (allocator_ == nullptr) return Status::Invalid("VulkanCubeMap: 分配器为空");
  if (desc_.extent.width == 0 || desc_.extent.height == 0) {
    return Status::Invalid("VulkanCubeMap: extent 宽高都必须 > 0");
  }
  // 立方体的六面在硬件上必须同尺寸(视图的 layerCount=6 隐含同一 extent)。
  if (desc_.extent.width != desc_.extent.height) {
    return Status::Invalid("VulkanCubeMap: 立方体六面必须同宽高,得到 " +
                           std::to_string(desc_.extent.width) + "x" +
                           std::to_string(desc_.extent.height));
  }
  if (desc_.format == Format::Unknown) {
    return Status::Invalid("VulkanCubeMap: format 不能是 Unknown");
  }
  // 本轮只接 RGBA8_UNORM:8bit 彩色格式里唯一被"读图 -> 上传 -> 采样"三处都实测过的。
  if (desc_.format != Format::RGBA8_UNORM) {
    return Status::Unsupported("VulkanCubeMap: 本轮只支持 RGBA8_UNORM,得到 format=" +
                               std::to_string(static_cast<int>(desc_.format)));
  }
  if (desc_.usage != resources::TextureUsage::Sampled &&
      desc_.usage != resources::TextureUsage::TransferDst) {
    return Status::Unsupported(
        "VulkanCubeMap: 立方体贴图只能是 Sampled / TransferDst(不做渲染附件)");
  }
  vk_format_ = vk_format_of(desc_.format);
  if (vk_format_ == VK_FORMAT_UNDEFINED) {
    return Status::Unsupported("VulkanCubeMap: 没有对应的 VkFormat");
  }
  face_bytes_ = static_cast<u64>(desc_.extent.width) * desc_.extent.height * 4ull;
  if (face_bytes_ == 0) return Status::Invalid("VulkanCubeMap: 单面字节数为 0");
  if (id_.valid()) return Status::Ok_();  // 幂等
  return allocate_storage();
}

Status VulkanCubeMap::allocate_storage() {
  // 句柄:整块图像资源不占桶内页,用保留页号(与采样器的 kSamplerPage 区分开),
  // id 是进程级槽位号(命令流去重/复现用)。
  // 保留页号必须同时避开 kInvalidPage(0xFFFFFFFF)与 kSamplerPage(0xFFFFFFFE) ——
  // 它与这两个都语义不同,踩上任何一个都会让 valid()/反查表把立方体贴图
  // 当成"无效"或"另一个采样器"。
  id_.page_id = 0xFFFFFFFCu;
  id_.offset = 0;
  id_.size = face_bytes_ * resources::kCubeFaceCount;
  id_.id = next_cube_slot();

  if (has_ctx_ && ctx_.device != VK_NULL_HANDLE) {
    // 设备态:先建图,再按 VkMemoryRequirements 要一整块图像页并整块绑定。
    return create_image_object();
  }
  // 软件态:自己持有一块映射存储,让上传/校验逻辑在没有 GPU 时也能跑。
  software_storage_.assign(static_cast<std::size_t>(id_.size), 0);
  mapped_ = software_storage_.data();
  return Status::Ok_();
}

Status VulkanCubeMap::create_image_object() {
  VkImageCreateInfo ici{};
  ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  // CUBE_COMPATIBLE 是"这张图可以被 cube 视图引用"的硬性声明;漏了它,
  // vkCreateImageView(VIEW_TYPE_CUBE) 会失败(不是静默降级)。
  ici.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
  ici.imageType = VK_IMAGE_TYPE_2D;
  ici.format = vk_format_;
  ici.extent = {desc_.extent.width, desc_.extent.height, 1};
  ici.mipLevels = 1;
  ici.arrayLayers = resources::kCubeFaceCount;
  ici.samples = VK_SAMPLE_COUNT_1_BIT;
  ici.tiling = VK_IMAGE_TILING_OPTIMAL;
  ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  const VkResult vr = vkCreateImage(ctx_.device, &ici, nullptr, &vk_image_);
  if (vr != VK_SUCCESS) {
    id_ = ResourceId{};
    return Status::OOM(fmt_err("vkCreateImage(立方体贴图)", vr));
  }

  // 图像页 API 是 backend 内部分配器(VulkanPagingAllocator)的能力,
  // 公共面 IMemoryAllocator 没有它 —— 与 vulkan_target.cpp 的离屏附件同一条路。
  VulkanPagingAllocator* paging = dynamic_cast<VulkanPagingAllocator*>(allocator_);
  if (paging == nullptr) {
    vkDestroyImage(ctx_.device, vk_image_, nullptr);
    vk_image_ = VK_NULL_HANDLE;
    id_ = ResourceId{};
    return Status::Unsupported(
        "VulkanCubeMap: 当前分配器不提供图像页 API(VulkanPagingAllocator),"
        "无法给 6 层立方体贴图分配整块内存(不做桶内拆分降级)");
  }

  VkMemoryRequirements mem{};
  vkGetImageMemoryRequirements(ctx_.device, vk_image_, &mem);
  VulkanPagingAllocator::ImagePageRequest req;
  req.size = static_cast<u64>(mem.size);
  req.alignment = static_cast<u64>(mem.alignment);
  req.memory_type_bits = static_cast<u32>(mem.memoryTypeBits);
  // 上传需要在 CPU 侧写像素,所以页必须是 HOST_VISIBLE|HOST_COHERENT ——
  // 与 MemoryKind::Texture 桶的落点一致(裁定 2:纹理一律 HostVisible)。
  // 这是"CPU 写、GPU 采样"的标准做法,不需要再额外要一块 DEVICE_LOCAL 做中转。
  req.device_local = false;
  auto page = paging->allocate_image_page(req);
  if (!page.ok()) {
    vkDestroyImage(ctx_.device, vk_image_, nullptr);
    vk_image_ = VK_NULL_HANDLE;
    id_ = ResourceId{};
    return Status(page.status().code,
                  std::string("VulkanCubeMap: 申请图像页失败: ") + page.status().message);
  }
  image_page_ = page.value().page_id;
  const Status bind =
      paging->bind_image_page(image_page_, reinterpret_cast<std::uintptr_t>(vk_image_));
  if (!bind.ok()) {
    (void)paging->release_image_page(image_page_);
    image_page_ = ResourceId::kInvalidPage;
    vkDestroyImage(ctx_.device, vk_image_, nullptr);
    vk_image_ = VK_NULL_HANDLE;
    id_ = ResourceId{};
    return bind;
  }
  // 图像页登记进后端页注册表(flush 时按 page_id 取 VkDeviceMemory 建临时读源缓冲)。
  sync_page_memory(allocator_, image_page_);
  // 显式映射:HOST_VISIBLE 图像页默认不预映射(VMA 只在 MAPPED 标志下才映射),
  // 而立方体贴图必须从 CPU 侧灌六面像素 —— 见 VulkanPagingAllocator::map_image_page。
  mapped_ = static_cast<std::uint8_t*>(paging->map_image_page(image_page_));
  if (mapped_ == nullptr) {
    (void)paging->release_image_page(image_page_);
    image_page_ = ResourceId::kInvalidPage;
    vkDestroyImage(ctx_.device, vk_image_, nullptr);
    vk_image_ = VK_NULL_HANDLE;
    id_ = ResourceId{};
    return Status::InitFailed("VulkanCubeMap: 图像页拿不到 CPU 映射(无法上传像素)");
  }

  VkImageViewCreateInfo vci{};
  vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  vci.image = vk_image_;
  vci.viewType = VK_IMAGE_VIEW_TYPE_CUBE;
  vci.format = vk_format_;
  vci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  vci.subresourceRange.baseMipLevel = 0;
  vci.subresourceRange.levelCount = 1;
  vci.subresourceRange.baseArrayLayer = 0;
  vci.subresourceRange.layerCount = resources::kCubeFaceCount;
  const VkResult view_r = vkCreateImageView(ctx_.device, &vci, nullptr, &vk_view_);
  if (view_r != VK_SUCCESS) {
    paging->unmap_image_page(image_page_);
    (void)paging->release_image_page(image_page_);
    image_page_ = ResourceId::kInvalidPage;
    vkDestroyImage(ctx_.device, vk_image_, nullptr);
    vk_image_ = VK_NULL_HANDLE;
    id_ = ResourceId{};
    return Status::InitFailed(fmt_err("vkCreateImageView(立方体贴图)", view_r));
  }
  return Status::Ok_();
}

// ---------------------------------------------------------------- 上传

Status VulkanCubeMap::upload_face(CubeFace face, const void* data, u64 size_bytes) {
  if (!id_.valid()) return Status::Invalid("VulkanCubeMap::upload_face: 立方体贴图尚未创建");
  if (!face_index_valid(face)) {
    return Status::Invalid("VulkanCubeMap::upload_face: face=" +
                           std::to_string(static_cast<int>(face)) + " 越界(只有 0..5)");
  }
  if (data == nullptr) return Status::Invalid("VulkanCubeMap::upload_face: data 为空");
  if (size_bytes != face_bytes_) {
    // 整面替换语义:少给会留下错误像素、多给会被静默截断,两种都不可接受。
    return Status::Invalid("VulkanCubeMap::upload_face: 面 " +
                           std::to_string(static_cast<int>(face)) + " 需要 " +
                           std::to_string(face_bytes_) + " 字节,得到 " +
                           std::to_string(size_bytes) + " 字节");
  }
  if (mapped_ == nullptr) {
    return Status::InitFailed("VulkanCubeMap::upload_face: 图像页没有常驻映射(CPU 侧无法上传)");
  }
  const u32 layer = static_cast<u32>(face);
  std::memcpy(mapped_ + static_cast<std::size_t>(layer) * face_bytes_, data,
              static_cast<std::size_t>(face_bytes_));
  face_dirty_[layer] = true;
  // 同一面重复上传只保留一条待提交记录(最后一次写入生效)。
  for (const PendingFace& p : pending_) {
    if (p.face == face) return Status::Ok_();
  }
  pending_.push_back(PendingFace{face});
  return Status::Ok_();
}

bool VulkanCubeMap::face_uploaded(CubeFace face) const {
  if (!face_index_valid(face)) return false;
  return face_dirty_[static_cast<u32>(face)];
}

Status VulkanCubeMap::flush() {
  if (!id_.valid()) return Status::Invalid("VulkanCubeMap::flush: 立方体贴图尚未创建");
  if (pending_.empty()) return Status::Ok_();

  if (!has_ctx_ || ctx_.device == VK_NULL_HANDLE) {
    // 软件态:面数据已经在映射存储里,语义与 GPU 拷贝等价。
    pending_.clear();
    flush_error_.clear();
    flushed_once_ = true;
    return Status::Ok_();
  }

  const VkImage image = vk_image_;
  const u32 w = desc_.extent.width;
  const u32 h = desc_.extent.height;
  const bool old_is_read_only = flushed_once_;
  std::vector<PendingFace> batch = pending_;
  // 临时 VkBuffer 必须活到提交执行完成,所以从 lambda 带出来、fence 等到后再销毁。
  VkBuffer src_buffer = VK_NULL_HANDLE;

  Status s = submit_once_sync(ctx_, "VulkanCubeMap::flush", [&](VkCommandBuffer cmd) {
    VkImageSubresourceRange full{};
    full.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    full.baseMipLevel = 0;
    full.levelCount = 1;
    full.baseArrayLayer = 0;
    full.layerCount = resources::kCubeFaceCount;

    // 1) 六层整图 -> TRANSFER_DST(再次上传时从 SHADER_READ_ONLY 转回)。
    VkImageMemoryBarrier to_dst{};
    to_dst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    to_dst.oldLayout = old_is_read_only ? VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                                        : VK_IMAGE_LAYOUT_UNDEFINED;
    to_dst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_dst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_dst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_dst.image = image;
    to_dst.subresourceRange = full;
    to_dst.srcAccessMask = old_is_read_only ? VK_ACCESS_SHADER_READ_BIT : 0;
    to_dst.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd,
                         old_is_read_only ? VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT
                                          : VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &to_dst);

    // 2) 上传源 = 立方体自己的图像页(CPU 已写好)。
    //    与 VulkanTexture::submit_pending 同法:临时建一张覆盖整页的 VkBuffer 并按
    //    页绑定的 offset 绑定。这是 VkBuffer 对象,不是显存分配 ——
    //    不改变稳态帧的 VMA 计数。
    resources::PageDesc pd{};
    if (!allocator_->page_desc(image_page_, &pd)) {
      return Status::Invalid("VulkanCubeMap::flush: 图像页不存在");
    }
    PageBinding pb{};
    if (!page_binding(image_page_, &pb) || pb.memory == VK_NULL_HANDLE) {
      return Status::Invalid("VulkanCubeMap::flush: 图像页未登记 VkDeviceMemory");
    }
    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = pd.page_size;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkResult vr = vkCreateBuffer(ctx_.device, &bci, nullptr, &src_buffer);
    if (vr != VK_SUCCESS) return Status::OOM(fmt_err("vkCreateBuffer(立方体贴图读源)", vr));
    vr = vkBindBufferMemory(ctx_.device, src_buffer, pb.memory, pb.offset);
    if (vr != VK_SUCCESS) {
      return Status::InitFailed(fmt_err("vkBindBufferMemory(立方体贴图读源)", vr));
    }

    std::vector<VkBufferImageCopy> regions;
    regions.reserve(batch.size());
    for (const PendingFace& p : batch) {
      VkBufferImageCopy region{};
      region.bufferOffset = static_cast<VkDeviceSize>(static_cast<u32>(p.face)) * face_bytes_;
      region.bufferRowLength = 0;
      region.bufferImageHeight = 0;
      region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
      region.imageSubresource.mipLevel = 0;
      region.imageSubresource.baseArrayLayer = static_cast<u32>(p.face);
      region.imageSubresource.layerCount = 1;
      region.imageOffset = {0, 0, 0};
      region.imageExtent = {w, h, 1};
      regions.push_back(region);
    }
    vkCmdCopyBufferToImage(cmd, src_buffer, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                           static_cast<u32>(regions.size()), regions.data());

    // 3) 全部六层 -> SHADER_READ_ONLY(描述符写入固定用这个布局)。
    VkImageMemoryBarrier to_read{};
    to_read.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    to_read.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_read.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    to_read.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_read.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_read.image = image;
    to_read.subresourceRange = full;
    to_read.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    to_read.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &to_read);
    return Status::Ok_();
  });

  // submit_once_sync 已 fence 等待(或根本没提交),临时缓冲可以安全销毁。
  if (src_buffer != VK_NULL_HANDLE) vkDestroyBuffer(ctx_.device, src_buffer, nullptr);

  if (s.ok()) {
    flush_error_.clear();
    flushed_once_ = true;
  } else {
    flush_error_ = s.message;
  }
  // 失败也不重试(与 VulkanTexture::submit_pending 同口径);面数据仍在映射里,
  // 调用方可以修好原因后再次 flush。
  pending_.clear();
  return s;
}

}  // namespace magicblocks::backend
