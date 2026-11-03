// MagicBlocks —— Vulkan 后端:渲染目标实现(离屏)
#include "backend/vulkan/vulkan_target.h"

#include <cstring>

#include "backend/vulkan/vulkan_allocator.h"
#include "backend/vulkan/vulkan_draw.h"

namespace magicblocks::backend {

using namespace magicblocks::device;

VkFormat to_vk_format(Format f) {
  switch (f) {
    case Format::RGBA8_UNORM: return VK_FORMAT_R8G8B8A8_UNORM;
    case Format::BGRA8_UNORM: return VK_FORMAT_B8G8R8A8_UNORM;
    case Format::R8_UNORM: return VK_FORMAT_R8_UNORM;
    case Format::D16_UNORM: return VK_FORMAT_D16_UNORM;
    case Format::D24_UNORM_S8_UINT: return VK_FORMAT_D24_UNORM_S8_UINT;
    case Format::D32_SFLOAT: return VK_FORMAT_D32_SFLOAT;
    default: return VK_FORMAT_UNDEFINED;
  }
}

VkFormat to_vk_depth_format(DepthFormat f) {
  switch (f) {
    case DepthFormat::D16_UNORM: return VK_FORMAT_D16_UNORM;
    case DepthFormat::D24_UNORM_S8_UINT: return VK_FORMAT_D24_UNORM_S8_UINT;
    case DepthFormat::D32_SFLOAT: return VK_FORMAT_D32_SFLOAT;
    default: return VK_FORMAT_D16_UNORM;
  }
}

std::uint32_t find_memory_type(const VulkanContext& ctx, std::uint32_t type_bits,
                               VkMemoryPropertyFlags flags) {
  for (std::uint32_t i = 0; i < ctx.mem.memoryTypeCount; ++i) {
    if ((type_bits & (1u << i)) &&
        (ctx.mem.memoryTypes[i].propertyFlags & flags) == flags) {
      return i;
    }
  }
  return UINT32_MAX;
}

Status create_image(const VulkanContext& ctx, VkFormat fmt, VkImageUsageFlags usage,
                    VkImageLayout initial_layout, std::uint32_t w, std::uint32_t h,
                    VkImage* out_image, std::uint32_t* out_page) {
  *out_image = VK_NULL_HANDLE;
  *out_page = 0xFFFFFFFFu;
  if (ctx.allocator == nullptr) {
    return Status::InitFailed("create_image: 后端分配器未接入(L1 之后不允许裸分配)");
  }

  VkImageCreateInfo ici{};
  ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  ici.imageType = VK_IMAGE_TYPE_2D;
  ici.format = fmt;
  ici.extent = {w, h, 1};
  ici.mipLevels = 1;
  ici.arrayLayers = 1;
  ici.samples = VK_SAMPLE_COUNT_1_BIT;
  ici.tiling = VK_IMAGE_TILING_OPTIMAL;
  ici.usage = usage;
  ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  ici.initialLayout = initial_layout;
  VkResult r = vkCreateImage(ctx.device, &ici, nullptr, out_image);
  if (r != VK_SUCCESS) return Status::OOM(fmt_err("vkCreateImage", r));

  // L1:G4「全仓零裸分配」—— 图像内存一律从分页分配器要专用页(仍是 VMA 分配,
  // 但 VkImage 必须整块绑定到一个 allocation 的 offset 0,不能与别的资源共享)。
  VkMemoryRequirements req{};
  vkGetImageMemoryRequirements(ctx.device, *out_image, &req);

  VulkanPagingAllocator::ImagePageRequest page_req;
  page_req.size = req.size;
  page_req.alignment = req.alignment;
  page_req.memory_type_bits = req.memoryTypeBits;
  page_req.device_local = true;
  auto page = ctx.allocator->allocate_image_page(page_req);
  if (!page.ok()) {
    vkDestroyImage(ctx.device, *out_image, nullptr);
    *out_image = VK_NULL_HANDLE;
    return Status::OOM("create_image: " + page.message());
  }

  Status bind = ctx.allocator->bind_image_page(page.value().page_id,
                                               reinterpret_cast<std::uintptr_t>(*out_image));
  if (!bind.ok()) {
    ctx.allocator->release_image_page(page.value().page_id);
    vkDestroyImage(ctx.device, *out_image, nullptr);
    *out_image = VK_NULL_HANDLE;
    return Status::InitFailed("create_image: " + bind.message);
  }
  *out_page = page.value().page_id;
  return Status::Ok_();
}

// ---------------------------------------------------------------- OffscreenTarget

OffscreenTarget::~OffscreenTarget() {
  // 顺序关键:先销毁 image,再把它占的分配器页退回去(页承载着这块 VkDeviceMemory)。
  if (color_) vkDestroyImage(ctx_.device, color_, nullptr);
  if (depth_) vkDestroyImage(ctx_.device, depth_, nullptr);
  if (ctx_.allocator != nullptr) {
    if (color_page_ != 0xFFFFFFFFu) ctx_.allocator->release_image_page(color_page_);
    if (depth_page_ != 0xFFFFFFFFu) ctx_.allocator->release_image_page(depth_page_);
  }
}

Status OffscreenTarget::init() {
  if (desc_.extent.width == 0 || desc_.extent.height == 0) {
    return Status::Invalid("OffscreenTarget: extent 必须 > 0");
  }
  // 采样数必须由设备能力保证;L0 单采样(MSAA 的解析附件留 L1/L2)
  if (desc_.samples != 1) {
    return Status::Unsupported("OffscreenTarget: L0 只支持 samples=1(MSAA 留 L1)");
  }
  Status s = create_image(ctx_, to_vk_format(desc_.format),
                          VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                              VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                          VK_IMAGE_LAYOUT_UNDEFINED, desc_.extent.width, desc_.extent.height,
                          &color_, &color_page_);
  if (!s.ok()) return s;

  if (desc_.with_depth) {
    s = create_image(ctx_, to_vk_depth_format(DepthFormat::D16_UNORM),
                     VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                     VK_IMAGE_LAYOUT_UNDEFINED, desc_.extent.width, desc_.extent.height, &depth_,
                     &depth_page_);
    if (!s.ok()) return s;
  }
  return Status::Ok_();
}

Status OffscreenTarget::draw_selftest(std::uint64_t frame_index, SelftestKind kind) {
  if (!color_) return Status::Unsupported("draw_selftest: 无颜色附件");
  const SelftestShaders s = (kind == SelftestKind::kL2Scene) ? SelftestShaders::L2Scene
                                                             : SelftestShaders::L0Default;
  return draw_selftest_grid(ctx_, this, frame_index, s);
}

AttachmentView OffscreenTarget::color_attachment() const {  AttachmentView v{};
  v.extent = desc_.extent;
  v.format = desc_.format;
  v.valid = color_ != VK_NULL_HANDLE;
  return v;
}

AttachmentView OffscreenTarget::depth_attachment() const {
  AttachmentView v{};
  v.extent = desc_.extent;
  v.format = Format::D16_UNORM;
  v.valid = depth_ != VK_NULL_HANDLE;
  return v;
}

Status OffscreenTarget::readback_rgba8(std::uint8_t* out, std::size_t size_bytes) const {
  if (!out) return Status::Invalid("readback: out 为空");
  const std::size_t need = static_cast<std::size_t>(desc_.extent.width) * desc_.extent.height * 4u;
  if (size_bytes < need) {
    return Status::Invalid("readback: 缓冲过小,需要 " + std::to_string(need) + " 字节");
  }
  if (!color_) return Status::Unsupported("readback: 无颜色附件");

  const VkDeviceSize bytes = static_cast<VkDeviceSize>(need);
  VkBufferCreateInfo bci{};
  bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  bci.size = bytes;
  bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  VkBuffer buf = VK_NULL_HANDLE;
  VkResult r = vkCreateBuffer(ctx_.device, &bci, nullptr, &buf);
  if (r != VK_SUCCESS) return Status::OOM(fmt_err("vkCreateBuffer", r));

  VkMemoryRequirements req{};
  vkGetBufferMemoryRequirements(ctx_.device, buf, &req);
  // L1:G4「全仓零裸分配」—— 读回缓冲从分页分配器要一块 HOST_VISIBLE 专用页
  // (VkBuffer 与 VkImage 一样只能整块绑定,所以走专用页而非桶内子块)。
  std::uint32_t readback_page = 0xFFFFFFFFu;
  void* mapped = nullptr;
  if (ctx_.allocator == nullptr) {
    vkDestroyBuffer(ctx_.device, buf, nullptr);
    return Status::InitFailed("readback: 后端分配器未接入(L1 之后不允许裸分配)");
  }
  {
    VulkanPagingAllocator::ImagePageRequest page_req;
    page_req.size = req.size;
    page_req.alignment = req.alignment;
    page_req.memory_type_bits = req.memoryTypeBits;
    page_req.device_local = false;  // HOST_VISIBLE|COHERENT,CPU 可读
    auto page = ctx_.allocator->allocate_image_page(page_req);
    if (!page.ok()) {
      vkDestroyBuffer(ctx_.device, buf, nullptr);
      return Status::OOM("readback: " + page.message());
    }
    readback_page = page.value().page_id;
    Status bind = ctx_.allocator->bind_buffer_page(readback_page,
                                                   reinterpret_cast<std::uintptr_t>(buf));
    if (!bind.ok()) {
      ctx_.allocator->release_image_page(readback_page);
      vkDestroyBuffer(ctx_.device, buf, nullptr);
      return Status::InitFailed("readback: " + bind.message);
    }
    mapped = ctx_.allocator->image_page_mapped_ptr(readback_page);
  }

  // 从这里起,任何失败都必须同时释放 buf 与 readback_page。
  auto cleanup_buffer = [&]() {
    vkDestroyBuffer(ctx_.device, buf, nullptr);
    if (readback_page != 0xFFFFFFFFu) ctx_.allocator->release_image_page(readback_page);
  };

  // 一次性命令:布局转换 → copy → 回 HOST
  VkCommandBufferAllocateInfo cai{};
  cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  cai.commandPool = ctx_.cmd_pool;
  cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cai.commandBufferCount = 1;
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  if (vkAllocateCommandBuffers(ctx_.device, &cai, &cmd) != VK_SUCCESS || cmd == VK_NULL_HANDLE) {
    cleanup_buffer();
    return Status::InitFailed("readback: vkAllocateCommandBuffers 失败");
  }
  VkCommandBufferBeginInfo bi{};
  bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  if (vkBeginCommandBuffer(cmd, &bi) != VK_SUCCESS) {
    vkFreeCommandBuffers(ctx_.device, ctx_.cmd_pool, 1, &cmd);
    cleanup_buffer();
    return Status::InitFailed("readback: vkBeginCommandBuffer 失败");
  }

  VkImageMemoryBarrier to_dst{};
  to_dst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  to_dst.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
  to_dst.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  to_dst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  to_dst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  to_dst.image = color_;
  to_dst.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  to_dst.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  to_dst.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                       VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &to_dst);

  VkBufferImageCopy region{};
  region.bufferOffset = 0;
  region.bufferRowLength = 0;
  region.bufferImageHeight = 0;
  region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
  region.imageExtent = {desc_.extent.width, desc_.extent.height, 1};
  vkCmdCopyImageToBuffer(cmd, color_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &region);

  VkImageMemoryBarrier back{};
  back.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  back.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
  back.newLayout = VK_IMAGE_LAYOUT_GENERAL;
  back.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  back.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  back.image = color_;
  back.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  back.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
  back.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
  vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                       VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1,
                       &back);
  r = vkEndCommandBuffer(cmd);
  if (r == VK_SUCCESS) {
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    r = vkQueueSubmit(ctx_.queue, 1, &si, VK_NULL_HANDLE);
    if (r == VK_SUCCESS) r = vkQueueWaitIdle(ctx_.queue);
  }
  vkFreeCommandBuffers(ctx_.device, ctx_.cmd_pool, 1, &cmd);
  if (r != VK_SUCCESS) {
    cleanup_buffer();
    return Status::InitFailed(fmt_err("readback 提交", r));
  }

  // Host 页 MAPPED 常驻(spec §6.2-7):无需 vkMapMemory,直接读指针。
  //
  // 行序必须**逐行翻转**:vkCmdCopyImageToBuffer 写到 buffer 里时,区域的
  // origin 在图像左下角,产出的第 0 行是图像**底部**行;而
  // IRenderTarget::readback_rgba8 的契约是"行序自左上角"(PNG/CLI 一致)。
  // 实测(SwiftShader):不翻行时整幅图上下颠倒,实例 0 的纯红标定点会跑到
  // 左下角,(40,30) 与 (40,210) 恰好互换 —— golden 的结构化断言会抓住它。
  Status out_status = Status::Ok_();
  if (mapped != nullptr) {
    const std::size_t row = static_cast<std::size_t>(desc_.extent.width) * 4u;
    const auto* src = static_cast<const std::uint8_t*>(mapped);
    const std::size_t rows = desc_.extent.height;
    for (std::size_t y = 0; y < rows; ++y) {
      std::memcpy(out + y * row, src + (rows - 1 - y) * row, row);
    }
  } else {
    out_status = Status::InitFailed("readback: 读回页未建立 CPU 映射");
  }
  cleanup_buffer();
  return out_status;
}

}  // namespace magicblocks::backend
