// MagicBlocks —— Vulkan 后端:L1 纹理与采样器实现(R1 / R4 / R5)
#include "backend/vulkan/vulkan_texture.h"

#include <algorithm>
#include <atomic>
#include <cstring>

#include "backend/vulkan/vulkan_resource_common.h"

namespace magicblocks::backend {

using namespace magicblocks::device;

namespace {

using Byte = std::uint8_t;

constexpr u32 kMaxMipLevels = 16;

bool is_power_of_two(u32 v) { return v != 0 && (v & (v - 1)) == 0; }

VkFilter to_vk_filter(FilterMode f) {
  return f == FilterMode::Nearest ? VK_FILTER_NEAREST : VK_FILTER_LINEAR;
}

VkSamplerAddressMode to_vk_wrap(WrapMode w) {
  switch (w) {
    case WrapMode::ClampToEdge:
      return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    case WrapMode::Repeat:
      return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    case WrapMode::MirroredRepeat:
      return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
  }
  return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
}

// CompareOp → VkCompareOp。Disabled 只对采样器有意义(不做比较采样),
// 这里返回 NEVER 只是占位,真正"Disabled"的判定在创建 VkSampler 时用 enableCompare=0。
VkCompareOp to_vk_compare(CompareOp op) {
  switch (op) {
    case CompareOp::Disabled:
      return VK_COMPARE_OP_NEVER;
    case CompareOp::Never:
      return VK_COMPARE_OP_NEVER;
    case CompareOp::Less:
      return VK_COMPARE_OP_LESS;
    case CompareOp::Equal:
      return VK_COMPARE_OP_EQUAL;
    case CompareOp::LessOrEqual:
      return VK_COMPARE_OP_LESS_OR_EQUAL;
    case CompareOp::Greater:
      return VK_COMPARE_OP_GREATER;
    case CompareOp::NotEqual:
      return VK_COMPARE_OP_NOT_EQUAL;
    case CompareOp::GreaterOrEqual:
      return VK_COMPARE_OP_GREATER_OR_EQUAL;
    case CompareOp::Always:
      return VK_COMPARE_OP_ALWAYS;
  }
  return VK_COMPARE_OP_NEVER;
}

VkImageUsageFlags usage_flags_of(TextureUsage usage) {
  switch (usage) {
    case TextureUsage::ColorTarget:
      return VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
             VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    case TextureUsage::DepthTarget:
      return VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
             VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    case TextureUsage::TransferSrc:
      return VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    case TextureUsage::Sampled:
      return VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
             VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    case TextureUsage::TransferDst:
      return VK_IMAGE_USAGE_TRANSFER_DST_BIT;
  }
  return VK_IMAGE_USAGE_SAMPLED_BIT;
}

// 是否 MSAA(mip 链对多采样图像无意义 → mip_levels 必须为 1)。
bool usage_allows_msaa(TextureUsage usage) {
  return usage == TextureUsage::ColorTarget || usage == TextureUsage::DepthTarget;
}

}  // namespace

// ---------------------------------------------------------------- 纹理:构造 / 析构

VulkanTexture::VulkanTexture(resources::IMemoryAllocator* allocator, const TextureDesc& desc)
    : has_ctx_(false), allocator_(allocator), desc_(desc) {}

VulkanTexture::VulkanTexture(const VulkanContext& ctx, resources::IMemoryAllocator* allocator,
                             const TextureDesc& desc)
    : ctx_(ctx), has_ctx_(true), allocator_(allocator), desc_(desc) {}

VulkanTexture::~VulkanTexture() {
  // 未提交的上传批先丢弃(暂存子块归还)。
  discard_pending();
  if (ctx_.device != VK_NULL_HANDLE) {
    if (vk_view_ != VK_NULL_HANDLE) vkDestroyImageView(ctx_.device, vk_view_, nullptr);
    if (vk_image_ != VK_NULL_HANDLE) vkDestroyImage(ctx_.device, vk_image_, nullptr);
  }
  vk_view_ = VK_NULL_HANDLE;
  vk_image_ = VK_NULL_HANDLE;
  if (allocator_ != nullptr && id_.valid()) {
    (void)allocator_->free(id_);  // M4:归还本页,不立即退 VMA
  }
  id_ = ResourceId{};
  mapped_ = nullptr;
}

// ---------------------------------------------------------------- 纹理:init

u64 VulkanTexture::mip_size_bytes(u32 level) const {
  if (level >= desc_.mip_levels) return 0;
  const u32 w = (std::max)(1u, desc_.extent.width >> level);
  const u32 h = (std::max)(1u, desc_.extent.height >> level);
  return static_cast<u64>(w) * h * texel_bytes_;
}

u64 VulkanTexture::total_pixel_bytes() const {
  u64 total = 0;
  for (u32 l = 0; l < desc_.mip_levels; ++l) total += mip_size_bytes(l);
  return total;
}

Status VulkanTexture::init() {
  if (allocator_ == nullptr) return Status::Invalid("VulkanTexture: 分配器为空");
  if (desc_.extent.width == 0 || desc_.extent.height == 0) {
    return Status::Invalid("VulkanTexture: extent 宽高都必须 > 0");
  }
  if (desc_.format == Format::Unknown) {
    return Status::Invalid("VulkanTexture: format 不能是 Unknown");
  }
  texel_bytes_ = format_pixel_bytes(desc_.format);
  if (texel_bytes_ == 0) {
    return Status::Unsupported("VulkanTexture: 不支持的格式(无法计算像素字节数)");
  }
  if (desc_.mip_levels == 0) return Status::Invalid("VulkanTexture: mip_levels 必须 >= 1");
  if (desc_.mip_levels > kMaxMipLevels) {
    return Status::Unsupported("VulkanTexture: mip_levels 上限 " +
                               std::to_string(kMaxMipLevels) + ",得到 " +
                               std::to_string(desc_.mip_levels));
  }
  // mip 链不能比 1x1 还长:最深层是 1x1 时层数已经到顶。
  u32 max_levels = 1;
  for (u32 w = desc_.extent.width, h = desc_.extent.height; w > 1 || h > 1;) {
    w = (std::max)(1u, w >> 1);
    h = (std::max)(1u, h >> 1);
    ++max_levels;
  }
  if (desc_.mip_levels > max_levels) {
    return Status::Invalid("VulkanTexture: mip_levels=" + std::to_string(desc_.mip_levels) +
                           " 超过 " + std::to_string(desc_.extent.width) + "x" +
                           std::to_string(desc_.extent.height) + " 的完整链长 " +
                           std::to_string(max_levels));
  }
  if (desc_.samples == 0 || !is_power_of_two(desc_.samples)) {
    return Status::Invalid("VulkanTexture: samples 必须是 2 的幂,得到 " +
                           std::to_string(desc_.samples));
  }
  if (desc_.samples > 1) {
    if (!usage_allows_msaa(desc_.usage)) {
      return Status::Invalid("VulkanTexture: samples>1 只允许 ColorTarget/DepthTarget(裁定 2)");
    }
    if (desc_.mip_levels != 1) {
      return Status::Invalid("VulkanTexture: MSAA 纹理不支持 mip 链");
    }
  }
  vk_format_ = vk_format_of(desc_.format);
  if (vk_format_ == VK_FORMAT_UNDEFINED) {
    return Status::Unsupported("VulkanTexture: 没有对应的 VkFormat");
  }
  if (id_.valid()) return Status::Ok_();  // 幂等
  return allocate_block();
}

Status VulkanTexture::allocate_block() {
  const u64 need = total_pixel_bytes();
  if (need == 0) return Status::Invalid("VulkanTexture: 像素字节数为 0");

  Result<resources::AllocResult> r = allocator_->allocate(MemoryKind::Texture, need, 4);
  if (!r.ok()) {
    return Status(r.status().code, std::string("VulkanTexture(") +
                                       std::to_string(desc_.extent.width) + "x" +
                                       std::to_string(desc_.extent.height) + ", size=" +
                                       std::to_string(need) + "): " + r.status().message);
  }
  id_ = r.value().id;
  mapped_ = r.value().mapped;

  // 裁定 2:纹理一律落 HostVisible 页 → 映射恒可用。真拿不到映射说明分配器
  // 的页类型与 page_kind_of(Texture) 漂移了,这是必须立刻暴露的契约破坏。
  if (mapped_ == nullptr) {
    (void)allocator_->free(id_);
    id_ = ResourceId{};
    return Status::InitFailed(
        "VulkanTexture: Texture 桶的页没有常驻映射(裁定 2 要求纹理落在 HostVisible 页)");
  }
  std::memset(mapped_, 0, static_cast<std::size_t>(id_.size));

  if (has_ctx_ && ctx_.device != VK_NULL_HANDLE) {
    return create_image_object();
  }
  return Status::Ok_();
}

Status VulkanTexture::create_image_object() {
  // 先把页的宿主 VkDeviceMemory 登记进页注册表(纯软件态 / 非 VMA 分配器时是空操作)。
  sync_page_memory(allocator_, id_.page_id);
  VkImageCreateInfo ici{};
  ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
  ici.imageType = VK_IMAGE_TYPE_2D;
  ici.format = vk_format_;
  ici.extent = {desc_.extent.width, desc_.extent.height, 1};
  ici.mipLevels = desc_.mip_levels;
  ici.arrayLayers = 1;
  switch (desc_.samples) {
    case 2:
      ici.samples = VK_SAMPLE_COUNT_2_BIT;
      break;
    case 4:
      ici.samples = VK_SAMPLE_COUNT_4_BIT;
      break;
    case 8:
      ici.samples = VK_SAMPLE_COUNT_8_BIT;
      break;
    case 16:
      ici.samples = VK_SAMPLE_COUNT_16_BIT;
      break;
    default:
      ici.samples = VK_SAMPLE_COUNT_1_BIT;
      break;
  }
  ici.tiling = VK_IMAGE_TILING_OPTIMAL;
  ici.usage = usage_flags_of(desc_.usage);
  ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
  ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  VkResult vr = vkCreateImage(ctx_.device, &ici, nullptr, &vk_image_);
  if (vr != VK_SUCCESS) {
    (void)allocator_->free(id_);
    id_ = ResourceId{};
    mapped_ = nullptr;
    return Status::OOM(fmt_err("vkCreateImage(纹理)", vr));
  }
  Status s = bind_image_memory(ctx_.device, vk_image_, id_);
  if (!s.ok()) {
    vkDestroyImage(ctx_.device, vk_image_, nullptr);
    vk_image_ = VK_NULL_HANDLE;
    (void)allocator_->free(id_);
    id_ = ResourceId{};
    mapped_ = nullptr;
    return s;
  }

  VkImageViewCreateInfo vci{};
  vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  vci.image = vk_image_;
  vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
  vci.format = vk_format_;
  vci.subresourceRange.aspectMask =
      is_depth_format(desc_.format) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
  vci.subresourceRange.baseMipLevel = 0;
  vci.subresourceRange.levelCount = desc_.mip_levels;
  vci.subresourceRange.baseArrayLayer = 0;
  vci.subresourceRange.layerCount = 1;
  vr = vkCreateImageView(ctx_.device, &vci, nullptr, &vk_view_);
  if (vr != VK_SUCCESS) {
    vkDestroyImage(ctx_.device, vk_image_, nullptr);
    vk_image_ = VK_NULL_HANDLE;
    (void)allocator_->free(id_);
    id_ = ResourceId{};
    mapped_ = nullptr;
    return Status::InitFailed(fmt_err("vkCreateImageView(纹理)", vr));
  }
  return Status::Ok_();
}

// ---------------------------------------------------------------- 纹理:上传(R5)

Status VulkanTexture::upload(const void* data, u64 size_bytes, u32 level) {
  if (!id_.valid()) return Status::Invalid("VulkanTexture::upload: 纹理尚未创建");
  if (data == nullptr) return Status::Invalid("VulkanTexture::upload: data 为空");
  if (level >= desc_.mip_levels) {
    return Status::Invalid("VulkanTexture::upload: level=" + std::to_string(level) + " 越界(共 " +
                           std::to_string(desc_.mip_levels) + " 级)");
  }
  const u64 mip_bytes = mip_size_bytes(level);
  if (size_bytes != mip_bytes) {
    // 纹理上传是"整级替换"语义:少给会留旧数据、多给会被静默截断,两种都不可接受。
    return Status::Invalid("VulkanTexture::upload: 第 " + std::to_string(level) + " 级需要 " +
                           std::to_string(mip_bytes) + " 字节,得到 " +
                           std::to_string(size_bytes) + " 字节");
  }

  // R5:CPU 侧暂存。**不单独切 Staging 子块** —— 分配器的子块粒度是 64 KiB
  // (rulings 裁定 3),为 64 字节的 mip0 切一个 64 KiB 槽位会把 Host 页(8 MB = 128 槽)
  // 迅速吃光。纹理本身已经落在 HostVisible 页上(裁定 2),所以暂存直接**暂存在纹理
  // 自己的子块里**:upload() 先把像素写进 mip 区,再把 (level, 字节) 记为"待提交",
  // submit_pending() 时用一次 vkCmdCopyBufferToImage 从同一子块搬到 GPU 侧 VkImage
  // (或软件态下什么都不用做 —— 数据已经就位)。
  u64 dst_off = 0;
  for (u32 l = 0; l < level; ++l) dst_off += mip_size_bytes(l);
  std::memcpy(static_cast<std::uint8_t*>(mapped_) + dst_off, data,
              static_cast<std::size_t>(mip_bytes));

  PendingUpload up;
  up.staging = id_;  // 源 = 纹理子块自身(见上;不再是独立 Staging 子块)
  up.level = level;
  up.offset = dst_off;
  up.size = mip_bytes;
  pending_.push_back(up);
  pending_bytes_ += mip_bytes;
  ++upload_begin_count_;
  return Status::Ok_();
}

resources::ResourceId VulkanTexture::pending_staging_id(u32 index) const {
  if (index >= pending_.size()) return ResourceId{};
  return pending_[index].staging;  // 恒等于 id():暂存源就是纹理子块本身
}

void VulkanTexture::discard_pending() {
  // 暂存源就是纹理子块本身(见 upload 的说明),没有独立子块要归还;
  // 丢弃只需清空队列。
  pending_.clear();
  pending_bytes_ = 0;
}

Status VulkanTexture::submit_pending() {
  if (pending_.empty()) return Status::Ok_();

  if (!has_ctx_ || ctx_.device == VK_NULL_HANDLE) {
    // 软件态:upload() 已经把像素写进 mip 区(暂存即纹理子块),语义与 GPU 拷贝等价。
    for (const PendingUpload& up : pending_) uploaded_bytes_total_ += up.size;
    pending_.clear();
    pending_bytes_ = 0;
    commit_error_.clear();
    return Status::Ok_();
  }

  // 设备态:一次性提交(单队列 + fence)。每笔上传 = 屏障 + CopyBufferToImage。
  // 暂存子块是页内裸偏移,没有独立 VkBuffer 对象,因此这里用一张临时 VkBuffer
  // 覆盖"整页"并以其 offset 绑定(见 vulkan_resource_common.h 的页注册表),
  // 只把本批的字节拷进 GPU 侧 VkImage。
  const VkImage image = vk_image_;
  const Format fmt = desc_.format;
  const u32 w = desc_.extent.width;
  const u32 h = desc_.extent.height;
  std::vector<PendingUpload> batch = pending_;
  const u64 batch_bytes = pending_bytes_;

  Status s = submit_once_sync(ctx_, "VulkanTexture::submit_pending", [&](VkCommandBuffer cmd) {
    VkImageAspectFlags aspect =
        is_depth_format(fmt) ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    VkImageMemoryBarrier to_dst{};
    to_dst.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    // 布局必须与 readback_device / bind_pipeline 的假设一致:
    //   - 颜色纹理:UNDEFINED → TRANSFER_DST → SHADER_READ_ONLY(readback 从 SHADER_READ_ONLY 出发)
    //   - 深度纹理:只投递到 DEPTH_STENCIL_READ_ONLY(回读路径本来就会拒绝深度格式,
    //     不引入第二条 TRANSFER 路径 —— 见 readback_rgba8 的 Unsupported)
    const bool depth = is_depth_format(fmt);
    to_dst.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    to_dst.newLayout = depth ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                             : VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    to_dst.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_dst.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_dst.image = image;
    to_dst.subresourceRange.aspectMask = aspect;
    to_dst.subresourceRange.baseMipLevel = 0;
    to_dst.subresourceRange.levelCount = desc_.mip_levels;
    to_dst.subresourceRange.baseArrayLayer = 0;
    to_dst.subresourceRange.layerCount = 1;
    to_dst.srcAccessMask = 0;
    to_dst.dstAccessMask = depth ? 0 : VK_ACCESS_TRANSFER_WRITE_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                         depth ? VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT
                               : VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &to_dst);

    if (!depth) {
      // 上传源 = 纹理子块本身(HostVisible 页,CPU 已写好)。临时建一张覆盖**其所在页**
      // 的 VkBuffer 并按子块 offset 绑定(见 vulkan_resource_common.h 的页注册表)。
      // 这是 VkBuffer 对象,不是 VkDeviceMemory 分配 —— 不改变稳态帧的 VMA 计数。
      for (const PendingUpload& up : batch) {
        resources::PageDesc pd{};
        if (!allocator_->page_desc(up.staging.page_id, &pd)) {
          return Status::Invalid("submit_pending: 暂存块所在页不存在");
        }
        VkBufferCreateInfo bci{};
        bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
        bci.size = pd.page_size;
        bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        VkBuffer page_buf = VK_NULL_HANDLE;
        VkResult vr = vkCreateBuffer(ctx_.device, &bci, nullptr, &page_buf);
        if (vr != VK_SUCCESS) return Status::OOM(fmt_err("vkCreateBuffer(暂存页)", vr));

        PageBinding pb{};
        if (!page_binding(up.staging.page_id, &pb) || pb.memory == VK_NULL_HANDLE) {
          vkDestroyBuffer(ctx_.device, page_buf, nullptr);
          return Status::Invalid("submit_pending: 暂存页未登记 VkDeviceMemory(先 reserve_pages)");
        }
        vr = vkBindBufferMemory(ctx_.device, page_buf, pb.memory,
                                pb.offset + static_cast<VkDeviceSize>(up.offset));
        if (vr != VK_SUCCESS) {
          vkDestroyBuffer(ctx_.device, page_buf, nullptr);
          return Status::InitFailed(fmt_err("vkBindBufferMemory(暂存页)", vr));
        }

        VkBufferImageCopy region{};
        region.bufferOffset = 0;
        region.bufferRowLength = 0;
        region.bufferImageHeight = 0;
        region.imageSubresource.aspectMask = aspect;
        region.imageSubresource.mipLevel = up.level;
        region.imageSubresource.baseArrayLayer = 0;
        region.imageSubresource.layerCount = 1;
        region.imageExtent = {(std::max)(1u, w >> up.level), (std::max)(1u, h >> up.level), 1};
        vkCmdCopyBufferToImage(cmd, page_buf, image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                               &region);
        vkDestroyBuffer(ctx_.device, page_buf, nullptr);
      }

      // 回读/采样用的末态:SHADER_READ_ONLY(readback_device 的 oldLayout 就是它)。
      VkImageMemoryBarrier to_read{};
      to_read.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
      to_read.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
      to_read.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
      to_read.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      to_read.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
      to_read.image = image;
      to_read.subresourceRange = to_dst.subresourceRange;
      to_read.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
      to_read.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
      vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                           VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                           &to_read);
    }
    return Status::Ok_();
  });

  if (s.ok()) {
    uploaded_bytes_total_ += batch_bytes;
    commit_error_.clear();
  } else {
    commit_error_ = s.message;
  }
  // 暂存源就是纹理子块本身,没有独立子块要归还(失败也不重试)。
  pending_.clear();
  pending_bytes_ = 0;
  return s;
}

// ---------------------------------------------------------------- 纹理:读回(§5.4)

Status VulkanTexture::readback_rgba8(std::uint8_t* out, std::size_t size_bytes) const {
  if (!id_.valid()) return Status::Invalid("VulkanTexture::readback: 纹理尚未创建");
  if (out == nullptr) return Status::Invalid("VulkanTexture::readback: out 为空");
  if (is_depth_format(desc_.format)) {
    return Status::Unsupported(
        "VulkanTexture::readback: 深度格式无法按 RGBA8 读回(明确报错,不做伪映射)");
  }
  if (!can_readback_rgba8(desc_.format)) {
    return Status::Unsupported("VulkanTexture::readback: 只有 RGBA8/BGRA8 能按 RGBA8 读回;R8 "
                               "请按单通道语义另开接口");
  }
  const std::size_t need =
      static_cast<std::size_t>(desc_.extent.width) * desc_.extent.height * 4u;
  if (size_bytes < need) {
    return Status::Invalid("VulkanTexture::readback: 缓冲过小,需要 " + std::to_string(need) +
                           " 字节,得到 " + std::to_string(size_bytes));
  }
  if (desc_.samples != 1) {
    return Status::Unsupported("VulkanTexture::readback: MSAA 纹理需要解析附件后才能读回"
                               "(spec §10 明确不做)");
  }
  if (!has_ctx_ || ctx_.device == VK_NULL_HANDLE) return readback_software(out, size_bytes);
  return readback_device(out, size_bytes);
}

Status VulkanTexture::readback_software(std::uint8_t* out, std::size_t size_bytes) const {
  // 软件态:子块本身就是 mip0 的紧凑像素(RGBA8 直接 memcpy;BGRA8 换一次通道)。
  const std::size_t need =
      static_cast<std::size_t>(desc_.extent.width) * desc_.extent.height * 4u;
  (void)size_bytes;
  if (desc_.format == Format::RGBA8_UNORM) {
    std::memcpy(out, mapped_, need);
    return Status::Ok_();
  }
  const std::uint8_t* src = static_cast<const std::uint8_t*>(mapped_);
  for (std::size_t i = 0; i < need; i += 4) {
    out[i + 0] = src[i + 2];  // B → R
    out[i + 1] = src[i + 1];  // G
    out[i + 2] = src[i + 0];  // R → B
    out[i + 3] = src[i + 3];  // A
  }
  return Status::Ok_();
}

Status VulkanTexture::readback_device(std::uint8_t* out, std::size_t size_bytes) const {
  const std::size_t need =
      static_cast<std::size_t>(desc_.extent.width) * desc_.extent.height * 4u;
  (void)size_bytes;

  // 读回目标也从 Staging 桶切(R5:一切 CPU 可见的缓冲区都走分页分配器)。
  Result<resources::AllocResult> r = allocator_->allocate(MemoryKind::Staging, need, 4);
  if (!r.ok()) {
    return Status(r.status().code, std::string("readback: 读回缓冲分配失败: ") +
                                       r.status().message);
  }
  const resources::AllocResult rb = r.value();
  if (rb.mapped == nullptr) {
    (void)allocator_->free(rb.id);
    return Status::InitFailed("readback: 读回缓冲拿不到常驻映射");
  }

  const VkImage image = vk_image_;
  const Format fmt = desc_.format;
  const u32 w = desc_.extent.width;
  const u32 h = desc_.extent.height;
  const resources::ResourceId rb_id = rb.id;

  // 页级临时 VkBuffer 必须活到 submit 执行完毕之后:vkCmdCopyImageToBuffer 只把
  // 命令记进命令缓冲区,真正执行在 submit 时。若在录制 lambda 内销毁,GPU 会读到
  // 已释放的 buffer(SwiftShader 上表现为偶发 0xC0000409 / 0xC0000005)。
  // 因此把 handle 提到外面,由 submit 返回后再销毁。
  VkBuffer page_buf = VK_NULL_HANDLE;

  Status s = submit_once_sync(ctx_, "VulkanTexture::readback", [&](VkCommandBuffer cmd) {
    VkImageMemoryBarrier to_src{};
    to_src.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    to_src.oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    to_src.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    to_src.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_src.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    to_src.image = image;
    to_src.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    to_src.subresourceRange.baseMipLevel = 0;
    to_src.subresourceRange.levelCount = 1;
    to_src.subresourceRange.baseArrayLayer = 0;
    to_src.subresourceRange.layerCount = 1;
    to_src.srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
    to_src.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         0, 0, nullptr, 0, nullptr, 1, &to_src);

    // 覆盖整页的临时 VkBuffer,按读回子块 offset 绑定(与 submit_pending 同一手法)。
    resources::PageDesc pd{};
    if (!allocator_->page_desc(rb_id.page_id, &pd)) {
      return Status::Invalid("readback: 读回块所在页不存在");
    }
    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = pd.page_size;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    VkResult vr = vkCreateBuffer(ctx_.device, &bci, nullptr, &page_buf);
    if (vr != VK_SUCCESS) return Status::OOM(fmt_err("vkCreateBuffer(读回页)", vr));
    PageBinding pb{};
    Status bs = Status::Ok_();
    if (!page_binding(rb_id.page_id, &pb) || pb.memory == VK_NULL_HANDLE) {
      bs = Status::Invalid("readback: 读回页未登记 VkDeviceMemory(先 reserve_pages)");
    } else {
      vr = vkBindBufferMemory(ctx_.device, page_buf, pb.memory,
                              pb.offset + static_cast<VkDeviceSize>(rb_id.offset));
      if (vr != VK_SUCCESS) bs = Status::InitFailed(fmt_err("vkBindBufferMemory(读回页)", vr));
    }
    if (!bs.ok()) return bs;  // buffer 由 submit 之后统一销毁

    VkBufferImageCopy region{};
    region.bufferOffset = 0;
    region.bufferRowLength = 0;
    region.bufferImageHeight = 0;
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {w, h, 1};
    vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, page_buf, 1,
                           &region);

    VkImageMemoryBarrier back{};
    back.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    back.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    back.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    back.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    back.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    back.image = image;
    back.subresourceRange = to_src.subresourceRange;
    back.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    back.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 1,
                         &back);
    return Status::Ok_();
  });

  if (s.ok()) {
    // vkCmdCopyImageToBuffer 按原生行序自上而下产出:mip0 第 0 行就是图像顶部,
    // 与 IRenderTarget::readback_rgba8 的契约一致 —— 1:1 拷贝,不翻行。
    if (fmt == Format::RGBA8_UNORM) {
      std::memcpy(out, rb.mapped, need);
    } else {
      const std::uint8_t* src = static_cast<const std::uint8_t*>(rb.mapped);
      for (std::size_t i = 0; i < need; i += 4) {
        out[i + 0] = src[i + 2];
        out[i + 1] = src[i + 1];
        out[i + 2] = src[i + 0];
        out[i + 3] = src[i + 3];
      }
    }
  }
  // submit_once_sync 已 fence 等待,GPU 不再引用 page_buf,此时销毁才安全。
  if (page_buf != VK_NULL_HANDLE) {
    vkDestroyBuffer(ctx_.device, page_buf, nullptr);
    page_buf = VK_NULL_HANDLE;
  }
  (void)allocator_->free(rb.id);
  return s;
}

// ---------------------------------------------------------------- 采样器

// 全局采样器槽位发号(单队列单线程;进程级单调递增,便于命令流去重)。
// 从 1 开始:0 保留给"未分配",避免 0 被误当成有效槽位。
static std::atomic<u32> g_sampler_slot{1};

u32 allocate_sampler_slot() { return g_sampler_slot.fetch_add(1, std::memory_order_relaxed); }

VulkanSampler::VulkanSampler(const SamplerDesc& desc) : has_ctx_(false), desc_(desc) {
  slot_index_ = allocate_sampler_slot();
}

VulkanSampler::VulkanSampler(const VulkanContext& ctx, const SamplerDesc& desc)
    : ctx_(ctx), has_ctx_(true), desc_(desc) {
  slot_index_ = allocate_sampler_slot();
}

VulkanSampler::~VulkanSampler() {
  if (vk_sampler_ != VK_NULL_HANDLE && ctx_.device != VK_NULL_HANDLE) {
    vkDestroySampler(ctx_.device, vk_sampler_, nullptr);
  }
  vk_sampler_ = VK_NULL_HANDLE;
}

Status VulkanSampler::validate_desc(const SamplerDesc& desc) {
  // 枚举本身只有合法值,但 SamplerDesc 可能被 C 风格 memcpy / 网络反序列化填进
  // 越界字节;逐项显式判定,而不是"就近取一个合法值"(texture.h 的明文要求)。
  const Byte min_f = static_cast<Byte>(desc.min_filter);
  const Byte mag_f = static_cast<Byte>(desc.mag_filter);
  const Byte mip_f = static_cast<Byte>(desc.mip_filter);
  if (min_f > static_cast<Byte>(FilterMode::Linear) ||
      mag_f > static_cast<Byte>(FilterMode::Linear) ||
      mip_f > static_cast<Byte>(FilterMode::Linear)) {
    return Status::Invalid("SamplerDesc: filter 非法(只允许 Nearest/Linear),得到 min=" +
                           std::to_string(min_f) + " mag=" + std::to_string(mag_f) + " mip=" +
                           std::to_string(mip_f));
  }
  const Byte wu = static_cast<Byte>(desc.wrap_u);
  const Byte wv = static_cast<Byte>(desc.wrap_v);
  if (wu > static_cast<Byte>(WrapMode::MirroredRepeat) ||
      wv > static_cast<Byte>(WrapMode::MirroredRepeat)) {
    return Status::Invalid("SamplerDesc: wrap 非法(只允许 ClampToEdge/Repeat/MirroredRepeat)");
  }
  const Byte cmp = static_cast<Byte>(desc.compare);
  if (cmp > static_cast<Byte>(CompareOp::Always)) {
    return Status::Invalid("SamplerDesc: compare 非法,得到 " + std::to_string(cmp));
  }
  return Status::Ok_();
}

Status VulkanSampler::init() {
  Status s = validate_desc(desc_);
  if (!s.ok()) return s;

  if (!has_ctx_ || ctx_.device == VK_NULL_HANDLE) return Status::Ok_();  // 软件态只做校验

  VkSamplerCreateInfo sci{};
  sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
  sci.magFilter = to_vk_filter(desc_.mag_filter);
  sci.minFilter = to_vk_filter(desc_.min_filter);
  sci.mipmapMode =
      desc_.mip_filter == FilterMode::Nearest ? VK_SAMPLER_MIPMAP_MODE_NEAREST
                                              : VK_SAMPLER_MIPMAP_MODE_LINEAR;
  sci.addressModeU = to_vk_wrap(desc_.wrap_u);
  sci.addressModeV = to_vk_wrap(desc_.wrap_v);
  sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
  sci.mipLodBias = 0.0f;
  // R4:本轮不建描述符集,采样器不需要各向异性/比较采样之外的特性;
  // anisotropy 需要 device feature,本轮不开启 → 设为 1.0(关闭)。
  sci.anisotropyEnable = VK_FALSE;
  sci.maxAnisotropy = 1.0f;
  sci.compareEnable = desc_.compare != CompareOp::Disabled ? VK_TRUE : VK_FALSE;
  sci.compareOp = to_vk_compare(desc_.compare);
  sci.minLod = 0.0f;
  sci.maxLod = VK_LOD_CLAMP_NONE;
  sci.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
  sci.unnormalizedCoordinates = VK_FALSE;
  const VkResult r = vkCreateSampler(ctx_.device, &sci, nullptr, &vk_sampler_);
  if (r != VK_SUCCESS) return Status::InitFailed(fmt_err("vkCreateSampler", r));
  return Status::Ok_();
}

}  // namespace magicblocks::backend
