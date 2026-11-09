// MagicBlocks —— Vulkan 后端:L1 缓冲资源实现(R1 / R5)
#include "backend/vulkan/vulkan_buffer.h"

#include <cstring>
#include <string>

#include "backend/vulkan/vulkan_resource_common.h"

namespace magicblocks::backend {

using namespace magicblocks::device;

namespace {

// 桶内的默认对齐。分配器会把它与 64 KiB 取大(rulings 裁定 3),这里只是"尽量给
// GPU 想要的粒度":顶点 4、索引 2、uniform/storage 16。
u64 default_align_of(BufferUsage usage) {
  switch (usage) {
    case BufferUsage::Vertex:
      return 4;
    case BufferUsage::Index:
      return 2;
    case BufferUsage::Uniform:
    case BufferUsage::Storage:
      return 16;
    case BufferUsage::Staging:
      break;
  }
  return 4;
}

bool is_power_of_two(u64 v) { return v != 0 && (v & (v - 1)) == 0; }

const char* usage_name(BufferUsage u) {
  switch (u) {
    case BufferUsage::Vertex:
      return "Vertex";
    case BufferUsage::Index:
      return "Index";
    case BufferUsage::Uniform:
      return "Uniform";
    case BufferUsage::Storage:
      return "Storage";
    case BufferUsage::Staging:
      return "Staging";
  }
  return "?";
}

// 子块区间校验:offset + size 必须落在 [0, id.size)。
bool range_ok(const resources::ResourceId& id, u64 offset, u64 size, std::string* err) {
  if (size == 0) {
    *err = "size 必须 > 0";
    return false;
  }
  if (offset > id.size || size > id.size - offset) {
    *err = "区间 [" + std::to_string(offset) + ", " + std::to_string(offset + size) +
           ") 越出缓冲大小 " + std::to_string(id.size);
    return false;
  }
  return true;
}

}  // namespace

// ---------------------------------------------------------------- 构造 / 析构

VulkanBuffer::VulkanBuffer(resources::IMemoryAllocator* allocator, const BufferDesc& desc)
    : has_ctx_(false), allocator_(allocator), desc_(desc) {}

VulkanBuffer::VulkanBuffer(const VulkanContext& ctx, resources::IMemoryAllocator* allocator,
                           const BufferDesc& desc)
    : ctx_(ctx), has_ctx_(true), allocator_(allocator), desc_(desc) {}

VulkanBuffer::~VulkanBuffer() {
  // M4:先丢弃未提交的上传批(它们的暂存子块也要归还),再归还本缓冲的子块。
  flush_pending();
  if (vk_buffer_ != VK_NULL_HANDLE && ctx_.device != VK_NULL_HANDLE) {
    vkDestroyBuffer(ctx_.device, vk_buffer_, nullptr);
  }
  vk_buffer_ = VK_NULL_HANDLE;
  if (allocator_ != nullptr && id_.valid()) {
    // 回归本页(不立即退 VMA)。析构里不因 free 失败而崩:失败只说明句柄已归还,
    // 分配器自己的 validate() 会把这类账目不一致报出来。
    (void)allocator_->free(id_);
  }
  id_ = ResourceId{};
  mapped_ = nullptr;
}

// ---------------------------------------------------------------- init

Status VulkanBuffer::init() {
  if (allocator_ == nullptr) return Status::Invalid("VulkanBuffer: 分配器为空");
  if (desc_.size == 0) return Status::Invalid("VulkanBuffer: size 必须 > 0");
  if (desc_.alignment != 0 && !is_power_of_two(desc_.alignment)) {
    return Status::Invalid("VulkanBuffer: alignment 必须是 2 的幂,得到 " +
                           std::to_string(desc_.alignment));
  }
  if (id_.valid()) return Status::Ok_();  // 幂等
  return allocate_block();
}

Status VulkanBuffer::allocate_block() {
  kind_ = resources::memory_kind_of(desc_.usage);
  const u64 align = desc_.alignment != 0 ? desc_.alignment : default_align_of(desc_.usage);

  Result<resources::AllocResult> r = allocator_->allocate(kind_, desc_.size, align);
  if (!r.ok()) {
    // 分配器的 message 已经带「需要/已有/预算」;这里补上缓冲用途,便于上层定位
    // 是哪个桶没预取。OutOfMemory / Unsupported 的状态码原样透传,不降级成 InvalidArgument。
    return Status(r.status().code, std::string("VulkanBuffer(") + usage_name(desc_.usage) +
                                       " size=" + std::to_string(desc_.size) + "): " +
                                       r.status().message);
  }
  id_ = r.value().id;
  mapped_ = r.value().mapped;

  // 设备页桶 CPU 不可见是**预期**行为(mapped == nullptr,不算失败);
  // 但 desc().persistent_map 明确要了常驻映射时必须诚实报 Unsupported,
  // 而不是给一个之后 write() 必然失败的缓冲。
  if (desc_.persistent_map && mapped_ == nullptr) {
    (void)allocator_->free(id_);
    id_ = ResourceId{};
    mapped_ = nullptr;
    return Status::Unsupported("VulkanBuffer: usage=" + std::string(usage_name(desc_.usage)) +
                               " 落在设备页桶,CPU 不可映射;持久映射请用 BufferUsage::Staging");
  }

  // 设备态:建真实 VkBuffer 并绑定到子块所在页的偏移处。
  if (has_ctx_ && ctx_.device != VK_NULL_HANDLE) {
    // 先把页的宿主 VkDeviceMemory 登记进页注册表(纯软件态 / 非 VMA 分配器时是空操作),
    // 否则下面的 bind_buffer_memory 会以"页未登记"明确失败。
    sync_page_memory(allocator_, id_.page_id);
    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    // 缓冲对象只覆盖**子块**大小:同页其他子块各自有独立的 VkBuffer 对象,
    // 页内偏移由 ResourceId.offset 表达(命令流记录的就是它)。
    bci.size = desc_.size;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    switch (desc_.usage) {
      case BufferUsage::Vertex:
        bci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        break;
      case BufferUsage::Index:
        bci.usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        break;
      case BufferUsage::Uniform:
        bci.usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        break;
      case BufferUsage::Storage:
        bci.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        break;
      case BufferUsage::Staging:
        // R5 的 CPU 侧暂存:既是上传源,也要能当读回目标。
        bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        break;
    }
    VkResult vr = vkCreateBuffer(ctx_.device, &bci, nullptr, &vk_buffer_);
    if (vr != VK_SUCCESS) {
      (void)allocator_->free(id_);
      id_ = ResourceId{};
      mapped_ = nullptr;
      return Status::OOM(fmt_err("vkCreateBuffer", vr));
    }
    Status s = bind_buffer_memory(ctx_.device, vk_buffer_, id_);
    if (!s.ok()) {
      vkDestroyBuffer(ctx_.device, vk_buffer_, nullptr);
      vk_buffer_ = VK_NULL_HANDLE;
      (void)allocator_->free(id_);
      id_ = ResourceId{};
      mapped_ = nullptr;
      return s;
    }
    vk_offset_ = id_.offset;
  }
  return Status::Ok_();
}

// ---------------------------------------------------------------- 读写(R1)

Status VulkanBuffer::write(const void* data, u64 offset, u64 size) {
  if (!id_.valid()) return Status::Invalid("VulkanBuffer::write: 缓冲尚未创建");
  if (data == nullptr) return Status::Invalid("VulkanBuffer::write: data 为空");
  std::string err;
  if (!range_ok(id_, offset, size, &err)) return Status::Invalid("VulkanBuffer::write: " + err);
  if (mapped_ == nullptr) {
    // D8:设备页 CPU 不可见必须明确报错,绝不"悄悄忽略这次写入"。
    return Status::Unsupported("VulkanBuffer::write: usage=" +
                               std::string(usage_name(desc_.usage)) +
                               " 落在设备页桶,CPU 不可映射;请改用 upload()(R5 staging 路径)");
  }
  std::memcpy(static_cast<std::uint8_t*>(mapped_) + offset, data, static_cast<std::size_t>(size));
  return Status::Ok_();
}

Status VulkanBuffer::read(void* out, u64 offset, u64 size) const {
  if (!id_.valid()) return Status::Invalid("VulkanBuffer::read: 缓冲尚未创建");
  if (out == nullptr) return Status::Invalid("VulkanBuffer::read: out 为空");
  std::string err;
  if (!range_ok(id_, offset, size, &err)) return Status::Invalid("VulkanBuffer::read: " + err);
  if (mapped_ == nullptr) {
    return Status::Unsupported("VulkanBuffer::read: usage=" +
                               std::string(usage_name(desc_.usage)) +
                               " 落在设备页桶,CPU 不可读;请用 staging 缓冲 + vkCmdCopyBuffer");
  }
  std::memcpy(out, static_cast<const std::uint8_t*>(mapped_) + offset,
              static_cast<std::size_t>(size));
  return Status::Ok_();
}

// ---------------------------------------------------------------- R5 上传队列

Status VulkanBuffer::upload(const void* data, u64 size, u64 dst_offset) {
  if (!id_.valid()) return Status::Invalid("VulkanBuffer::upload: 缓冲尚未创建");
  if (data == nullptr) return Status::Invalid("VulkanBuffer::upload: data 为空");
  std::string err;
  if (!range_ok(id_, dst_offset, size, &err)) {
    return Status::Invalid("VulkanBuffer::upload: 目标" + err);
  }

  // R5:暂存缓冲**也**从分页分配器的 Staging 桶切(Upload.StagingGoesThroughAllocator
  // 就是查这条:AllocStats.bucket_alloc_count[Staging] 必须增长)。
  Result<resources::AllocResult> s =
      allocator_->allocate(MemoryKind::Staging, size, default_align_of(BufferUsage::Staging));
  if (!s.ok()) {
    return Status(s.status().code, std::string("VulkanBuffer::upload: staging 分配失败: ") +
                                       s.status().message);
  }
  const resources::AllocResult& staging = s.value();
  if (staging.mapped == nullptr) {
    (void)allocator_->free(staging.id);
    return Status::InitFailed(
        "VulkanBuffer::upload: staging 子块拿不到常驻映射(Host 页应恒可映射)");
  }
  std::memcpy(staging.mapped, data, static_cast<std::size_t>(size));

  PendingUpload up;
  up.staging = staging.id;
  up.dst_offset = dst_offset;
  up.size = size;
  pending_.push_back(up);
  pending_bytes_ += size;
  ++upload_begin_count_;
  upload_bytes_total_ += size;
  return Status::Ok_();
}

void VulkanBuffer::flush_pending() {
  if (pending_.empty()) {
    pending_bytes_ = 0;
    return;
  }
  upload_commit_error_.clear();

  // 目标缓冲自己可映射 → 走 memcpy 逐条提交(Staging 用途)。
  // 目标不可映射(设备页:Vertex/Index/Uniform/Storage)→ 必须走真实 GPU 拷贝。
  if (mapped_ != nullptr) {
    for (const PendingUpload& up : pending_) {
      Status s = commit_upload(up);
      if (s.ok()) {
        uploaded_bytes_total_ += up.size;
      } else if (upload_commit_error_.empty()) {
        // 记下第一条失败原因供上层查询;不重试(重试语义留给 L2 的帧内批处理)。
        upload_commit_error_ = s.message;
      }
      // 无论成败都归还暂存子块(M4:归还本页,不退 VMA)。
      (void)allocator_->free(up.staging);
    }
  } else {
    Status s = flush_pending_device_pages();
    if (s.ok()) {
      for (const PendingUpload& up : pending_) uploaded_bytes_total_ += up.size;
    } else {
      upload_commit_error_ = s.message;
    }
  }

  pending_.clear();
  pending_bytes_ = 0;
}

Status VulkanBuffer::flush_pending_device_pages() {
  if (!has_ctx_ || ctx_.device == VK_NULL_HANDLE || ctx_.queue == VK_NULL_HANDLE) {
    return Status::Unsupported(
        "VulkanBuffer::flush_pending: 纯软件态没有 VkDevice,设备页缓冲无法上传"
        "(请用带 ctx 的设备态构造)");
  }
  if (vk_buffer_ == VK_NULL_HANDLE) {
    return Status::Invalid("VulkanBuffer::flush_pending: 设备态缓冲没有 VkBuffer");
  }

  // 合并所有待提交区间:最左起点 + 覆盖到最右终点,一次拷贝。
  // (顶点/索引上传的调用方通常一次 upload 整块,合并后即一条记录)
  u64 lo = pending_[0].dst_offset;
  u64 hi = pending_[0].dst_offset + pending_[0].size;
  for (const PendingUpload& up : pending_) {
    lo = up.dst_offset < lo ? up.dst_offset : lo;
    const u64 end = up.dst_offset + up.size;
    hi = end > hi ? end : hi;
  }
  const u64 span = hi - lo;
  if (span == 0) return Status::Ok_();

  // 1) 物化一张 HOST_VISIBLE 的临时 staging VkBuffer,把 pending 数据按偏移拷进去。
  VkBufferCreateInfo bci{};
  bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
  bci.size = static_cast<VkDeviceSize>(span);
  bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
  bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

  VkBuffer staging_buf = VK_NULL_HANDLE;
  VkResult vr = vkCreateBuffer(ctx_.device, &bci, nullptr, &staging_buf);
  if (vr != VK_SUCCESS) {
    return Status::OOM(std::string("flush_pending: 创建 staging VkBuffer 失败: ") +
                       std::to_string(static_cast<int>(vr)));
  }

  VkMemoryRequirements req{};
  vkGetBufferMemoryRequirements(ctx_.device, staging_buf, &req);

  // L1:G4「全仓零裸分配」—— staging 的 VkDeviceMemory **也**必须从分页分配器要专用页,
  // 不允许出现 vkAllocateMemory / vkMapMemory(vkBindBufferMemory 用 bind_buffer_page)。
  // device_local=false 拿到的是 HOST_VISIBLE|COHERENT 且 MAPPED 常驻的页。
  VulkanPagingAllocator::ImagePageRequest page_req;
  page_req.size = req.size;
  page_req.alignment = req.alignment;
  page_req.memory_type_bits = req.memoryTypeBits;
  page_req.device_local = false;
  auto page = ctx_.allocator->allocate_image_page(page_req);
  if (!page.ok()) {
    vkDestroyBuffer(ctx_.device, staging_buf, nullptr);
    return Status::OOM("flush_pending: staging 页分配失败: " + page.message());
  }
  const std::uint32_t staging_page = page.value().page_id;

  Status bind = ctx_.allocator->bind_buffer_page(staging_page, reinterpret_cast<std::uintptr_t>(staging_buf));
  if (!bind.ok()) {
    ctx_.allocator->release_image_page(staging_page);
    vkDestroyBuffer(ctx_.device, staging_buf, nullptr);
    return Status::InitFailed("flush_pending: " + bind.message);
  }

  void* mapped = ctx_.allocator->image_page_mapped_ptr(staging_page);
  if (mapped == nullptr) {
    ctx_.allocator->release_image_page(staging_page);
    vkDestroyBuffer(ctx_.device, staging_buf, nullptr);
    return Status::InitFailed("flush_pending: staging 页拿不到常驻映射");
  }

  for (const PendingUpload& up : pending_) {
    const void* src = allocator_->mapped_ptr(up.staging);
    if (src == nullptr) {
      ctx_.allocator->release_image_page(staging_page);
      vkDestroyBuffer(ctx_.device, staging_buf, nullptr);
      return Status::Invalid("flush_pending: 暂存子块映射丢失");
    }
    std::memcpy(static_cast<std::uint8_t*>(mapped) + (up.dst_offset - lo), src,
                static_cast<std::size_t>(up.size));
  }

  // 2) 用一次性命令缓冲做 vkCmdCopyBuffer,提交并等待完成。
  auto destroy_staging = [&]() {
    ctx_.allocator->release_image_page(staging_page);
    vkDestroyBuffer(ctx_.device, staging_buf, nullptr);
  };

  Status out = submit_once_sync(ctx_, "flush_pending: 设备页上传", [&](VkCommandBuffer cmd) {
    VkBufferCopy region{};
    region.srcOffset = 0;
    region.dstOffset = static_cast<VkDeviceSize>(lo);
    region.size = static_cast<VkDeviceSize>(span);
    vkCmdCopyBuffer(cmd, staging_buf, vk_buffer_, 1, &region);

    // 3) 让后续的 GPU 读取看见拷贝结果。
    VkBufferMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER;
    barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barrier.dstAccessMask = VK_ACCESS_VERTEX_ATTRIBUTE_READ_BIT | VK_ACCESS_INDEX_READ_BIT |
                            VK_ACCESS_UNIFORM_READ_BIT | VK_ACCESS_SHADER_READ_BIT;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.buffer = vk_buffer_;
    barrier.offset = 0;
    barrier.size = VK_WHOLE_SIZE;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                         VK_PIPELINE_STAGE_VERTEX_INPUT_BIT |
                             VK_PIPELINE_STAGE_VERTEX_SHADER_BIT |
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         0, 0, nullptr, 1, &barrier, 0, nullptr);
    return Status::Ok_();
  });

  destroy_staging();
  return out;
}

Status VulkanBuffer::commit_upload(const PendingUpload& up) {
  const void* src = allocator_->mapped_ptr(up.staging);
  if (src == nullptr) return Status::Invalid("commit_upload: 暂存子块映射丢失");
  if (mapped_ != nullptr) {
    // 目标缓冲本身可映射(Staging 用途):直接拷进子块内偏移。
    std::memcpy(static_cast<std::uint8_t*>(mapped_) + up.dst_offset, src,
                static_cast<std::size_t>(up.size));
    return Status::Ok_();
  }
  // 设备页目标由 flush_pending() 统一走 flush_pending_device_pages()(vkCmdCopyBuffer),
  // 不在这里逐条处理 —— 单条记录各建一次 staging 会白白多出 N 次提交。
  return Status::Ok_();
}

resources::ResourceId VulkanBuffer::pending_staging_id(u32 index) const {
  if (index >= pending_.size()) return ResourceId{};
  return pending_[index].staging;
}

}  // namespace magicblocks::backend
