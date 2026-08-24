#include "core/render/offscreen.h"

#include <cstdio>
#include <cstring>

namespace easypainter::render {

namespace {

uint32_t FindMemoryType(VkPhysicalDevice pd, uint32_t type_bits, VkFlags want) {
  VkPhysicalDeviceMemoryProperties mp;
  vkGetPhysicalDeviceMemoryProperties(pd, &mp);
  for (uint32_t i = 0; i < mp.memoryTypeCount; ++i) {
    if (type_bits & (1u << i)) {
      if ((mp.memoryTypes[i].propertyFlags & want) == want) return i;
    }
  }
  return 0;
}

void TransitionImage(VkCommandBuffer cmd, VkImage img, VkImageLayout old_layout,
                     VkImageLayout new_layout, VkAccessFlags src_access,
                     VkAccessFlags dst_access, VkPipelineStageFlags src_stage,
                     VkPipelineStageFlags dst_stage) {
  VkImageMemoryBarrier b{};
  b.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
  b.oldLayout = old_layout;
  b.newLayout = new_layout;
  b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
  b.image = img;
  b.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  b.subresourceRange.levelCount = 1;
  b.subresourceRange.layerCount = 1;
  b.srcAccessMask = src_access;
  b.dstAccessMask = dst_access;
  vkCmdPipelineBarrier(cmd, src_stage, dst_stage, 0, 0, nullptr, 0, nullptr, 1, &b);
}

}  // namespace

OffscreenResult render_offscreen(const VulkanContext& ctx, const Pipeline& pipeline,
                                 const std::vector<stroke::Vec2>& pts, uint32_t w,
                                 uint32_t h) {
  OffscreenResult out;
  if (w == 0 || h == 0) return out;
  VkDevice dev = ctx.device();
  out.width = w;
  out.height = h;
  const VkDeviceSize kSize = static_cast<VkDeviceSize>(w) * h * 4;
  out.rgba.assign(static_cast<size_t>(kSize), 0);

  // --- 离屏颜色图像(COLOR_ATTACHMENT | TRANSFER_SRC) ---
  VkImageCreateInfo ici{};
  ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  ici.imageType = VK_IMAGE_TYPE_2D;
  ici.format = VK_FORMAT_R8G8B8A8_UNORM;
  ici.extent = {w, h, 1};
  ici.mipLevels = 1;
  ici.arrayLayers = 1;
  ici.samples = VK_SAMPLE_COUNT_1_BIT;
  ici.tiling = VK_IMAGE_TILING_OPTIMAL;
  ici.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
  ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  VkImage img = VK_NULL_HANDLE;
  VkDeviceMemory img_mem = VK_NULL_HANDLE;
  if (vkCreateImage(dev, &ici, nullptr, &img) != VK_SUCCESS) return out;

  VkMemoryRequirements mr;
  vkGetImageMemoryRequirements(dev, img, &mr);
  VkMemoryAllocateInfo ai{};
  ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  ai.allocationSize = mr.size;
  ai.memoryTypeIndex = FindMemoryType(ctx.physical_device(), mr.memoryTypeBits,
                                      VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
  if (vkAllocateMemory(dev, &ai, nullptr, &img_mem) != VK_SUCCESS ||
      vkBindImageMemory(dev, img, img_mem, 0) != VK_SUCCESS) {
    std::fprintf(stderr, "[offscreen] image alloc/bind failed\n");
    vkDestroyImage(dev, img, nullptr);
    return out;
  }

  VkImageViewCreateInfo iv{};
  iv.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  iv.image = img;
  iv.viewType = VK_IMAGE_VIEW_TYPE_2D;
  iv.format = VK_FORMAT_R8G8B8A8_UNORM;
  iv.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  iv.subresourceRange.levelCount = 1;
  iv.subresourceRange.layerCount = 1;
  VkImageView view = VK_NULL_HANDLE;
  if (vkCreateImageView(dev, &iv, nullptr, &view) != VK_SUCCESS) {
    vkDestroyImage(dev, img, nullptr);
    vkFreeMemory(dev, img_mem, nullptr);
    return out;
  }

  VkFramebufferCreateInfo fci{};
  fci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
  fci.renderPass = pipeline.render_pass();
  fci.attachmentCount = 1;
  fci.pAttachments = &view;
  fci.width = w;
  fci.height = h;
  fci.layers = 1;
  VkFramebuffer fb = VK_NULL_HANDLE;
  if (vkCreateFramebuffer(dev, &fci, nullptr, &fb) != VK_SUCCESS) {
    vkDestroyImageView(dev, view, nullptr);
    vkDestroyImage(dev, img, nullptr);
    vkFreeMemory(dev, img_mem, nullptr);
    return out;
  }

  // --- host-visible 读回 buffer ---
  VkBufferCreateInfo bci{};
  bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  bci.size = kSize;
  bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
  bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  VkBuffer buf = VK_NULL_HANDLE;
  VkDeviceMemory buf_mem = VK_NULL_HANDLE;
  if (vkCreateBuffer(dev, &bci, nullptr, &buf) != VK_SUCCESS) {
    vkDestroyFramebuffer(dev, fb, nullptr);
    vkDestroyImageView(dev, view, nullptr);
    vkDestroyImage(dev, img, nullptr);
    vkFreeMemory(dev, img_mem, nullptr);
    return out;
  }
  vkGetBufferMemoryRequirements(dev, buf, &mr);
  ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
  ai.allocationSize = mr.size;
  ai.memoryTypeIndex = FindMemoryType(ctx.physical_device(), mr.memoryTypeBits,
                                      VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                          VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
  if (vkAllocateMemory(dev, &ai, nullptr, &buf_mem) != VK_SUCCESS ||
      vkBindBufferMemory(dev, buf, buf_mem, 0) != VK_SUCCESS) {
    vkDestroyBuffer(dev, buf, nullptr);
    vkDestroyFramebuffer(dev, fb, nullptr);
    vkDestroyImageView(dev, view, nullptr);
    vkDestroyImage(dev, img, nullptr);
    vkFreeMemory(dev, img_mem, nullptr);
    return out;
  }

  // --- 记录并提交渲染命令 ---
  VkCommandBufferAllocateInfo ca{};
  ca.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  ca.commandPool = ctx.command_pool();
  ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  ca.commandBufferCount = 1;
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  if (vkAllocateCommandBuffers(dev, &ca, &cmd) != VK_SUCCESS) {
    vkDestroyBuffer(dev, buf, nullptr);
    vkFreeMemory(dev, buf_mem, nullptr);
    vkDestroyFramebuffer(dev, fb, nullptr);
    vkDestroyImageView(dev, view, nullptr);
    vkDestroyImage(dev, img, nullptr);
    vkFreeMemory(dev, img_mem, nullptr);
    return out;
  }

  // --- 顶点 buffer(生命周期须覆盖命令提交执行,避免 use-after-free) ---
  VkBuffer vb = VK_NULL_HANDLE;
  VkDeviceMemory vb_mem = VK_NULL_HANDLE;
  if (!pts.empty()) {
    const VkDeviceSize vbytes =
        static_cast<VkDeviceSize>(pts.size()) * sizeof(stroke::Vec2);
    VkBufferCreateInfo vbci{};
    vbci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    vbci.size = vbytes;
    vbci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
    vbci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(dev, &vbci, nullptr, &vb) == VK_SUCCESS) {
      vkGetBufferMemoryRequirements(dev, vb, &mr);
      ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
      ai.allocationSize = mr.size;
      ai.memoryTypeIndex = FindMemoryType(
          ctx.physical_device(), mr.memoryTypeBits,
          VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
      if (vkAllocateMemory(dev, &ai, nullptr, &vb_mem) == VK_SUCCESS &&
          vkBindBufferMemory(dev, vb, vb_mem, 0) == VK_SUCCESS) {
        void* vdata = nullptr;
        vkMapMemory(dev, vb_mem, 0, vbytes, 0, &vdata);
        std::memcpy(vdata, pts.data(), static_cast<size_t>(vbytes));
        vkUnmapMemory(dev, vb_mem);
      }
    }
  }

  VkCommandBufferBeginInfo bi{};
  bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  vkBeginCommandBuffer(cmd, &bi);

  TransitionImage(cmd, img, VK_IMAGE_LAYOUT_UNDEFINED,
                  VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, 0,
                  VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                  VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                  VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT);

  VkRenderPassBeginInfo rpb{};
  rpb.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
  rpb.renderPass = pipeline.render_pass();
  rpb.framebuffer = fb;
  rpb.renderArea = {{0, 0}, {w, h}};
  VkClearValue clear = {{{0.f, 0.f, 0.f, 1.f}}};
  rpb.clearValueCount = 1;
  rpb.pClearValues = &clear;
  vkCmdBeginRenderPass(cmd, &rpb, VK_SUBPASS_CONTENTS_INLINE);
  if (vb != VK_NULL_HANDLE) {
    pipeline.draw(cmd, vb, static_cast<uint32_t>(pts.size()), w, h);
  }
  vkCmdEndRenderPass(cmd);

  TransitionImage(cmd, img, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                  VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                  VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                  VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
                  VK_PIPELINE_STAGE_TRANSFER_BIT);

  VkBufferImageCopy region{};
  region.imageSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
  region.imageSubresource.layerCount = 1;
  region.imageExtent = {w, h, 1};
  vkCmdCopyImageToBuffer(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buf, 1, &region);

  vkEndCommandBuffer(cmd);
  VkSubmitInfo si{};
  si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
  si.commandBufferCount = 1;
  si.pCommandBuffers = &cmd;
  vkQueueSubmit(ctx.queue(), 1, &si, VK_NULL_HANDLE);
  vkDeviceWaitIdle(dev);

  void* data = nullptr;
  vkMapMemory(dev, buf_mem, 0, kSize, 0, &data);
  std::memcpy(out.rgba.data(), data, static_cast<size_t>(kSize));
  vkUnmapMemory(dev, buf_mem);

  vkFreeCommandBuffers(dev, ctx.command_pool(), 1, &cmd);
  if (vb != VK_NULL_HANDLE) vkDestroyBuffer(dev, vb, nullptr);
  if (vb_mem != VK_NULL_HANDLE) vkFreeMemory(dev, vb_mem, nullptr);
  vkDestroyBuffer(dev, buf, nullptr);
  vkFreeMemory(dev, buf_mem, nullptr);
  vkDestroyFramebuffer(dev, fb, nullptr);
  vkDestroyImageView(dev, view, nullptr);
  vkDestroyImage(dev, img, nullptr);
  vkFreeMemory(dev, img_mem, nullptr);
  return out;
}

}  // namespace easypainter::render
