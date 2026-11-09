// MagicBlocks —— Vulkan 后端:L1 资源对象层共用小工具的实现
//
// 页注册表的作用(VMA 唯一注入点之后的绑定桥):
//   VkBuffer / VkImage 的 vkBind*Memory(device, handle, memory, offset) 需要一个
//   **VkDeviceMemory**。这个句柄只能由分页分配器持有(它才是 VMA 的唯一注入点),
//   而分配器头刻意不暴露 VmaAllocation。于是:
//     分配器(页建立/销毁时)→ register_page_memory / forget_page_memory → 本注册表
//     → bind_buffer_memory / bind_image_memory → vkBind*Memory
//   注册表只在"页真正换了宿主内存"时被写,热路径(子块切分)完全不碰它。
#include "backend/vulkan/vulkan_resource_common.h"

#include <map>
#include <string>

#include "backend/vulkan/vulkan_allocator.h"

namespace magicblocks::backend {
namespace {

// 页注册表:page_id → (VkDeviceMemory, offset, size)。
// 页号由分配器单调发号、**回收后不复用**,所以表是稀疏的;节点量级是几十张页,
// 红黑树开销可以忽略,换来"查页失败即可明确报错"的确定性。
std::map<std::uint32_t, PageBinding>& registry() {
  static std::map<std::uint32_t, PageBinding> table;
  return table;
}

std::mutex& mutex_of_registry() {
  static std::mutex m;
  return m;
}

}  // namespace

std::mutex& page_registry_mutex() { return mutex_of_registry(); }

void register_page_memory(std::uint32_t page_id, const PageBinding& binding) {
  std::lock_guard<std::mutex> lock(mutex_of_registry());
  registry()[page_id] = binding;
}

void sync_page_memory(resources::IMemoryAllocator* allocator, std::uint32_t page_id) {
  if (allocator == nullptr) return;
  auto* paging = dynamic_cast<VulkanPagingAllocator*>(allocator);
  if (paging == nullptr) return;  // 别的分配器实现:没有可登记的 Vk 内存
  std::uintptr_t mem = 0;
  u64 offset = 0;
  if (!paging->page_memory_handle(page_id, &mem, &offset)) return;
  resources::PageDesc pd{};
  if (!paging->page_desc(page_id, &pd)) return;
  PageBinding pb{};
  pb.memory = reinterpret_cast<VkDeviceMemory>(mem);
  pb.offset = offset;
  pb.size = pd.page_size;
  register_page_memory(page_id, pb);
}

void forget_page_memory(std::uint32_t page_id) {
  std::lock_guard<std::mutex> lock(mutex_of_registry());
  registry().erase(page_id);
}

bool page_binding(std::uint32_t page_id, PageBinding* out) {
  std::lock_guard<std::mutex> lock(mutex_of_registry());
  auto it = registry().find(page_id);
  if (it == registry().end()) return false;
  if (out) *out = it->second;
  return true;
}

std::uint32_t registered_page_count() {
  std::lock_guard<std::mutex> lock(mutex_of_registry());
  return static_cast<std::uint32_t>(registry().size());
}

void clear_page_memory() {
  std::lock_guard<std::mutex> lock(mutex_of_registry());
  registry().clear();
}

namespace {

const char* const kNoPageBinding =
    "所在页没有登记 VkDeviceMemory(设备态请确认页由 VulkanPagingAllocator 以 VMA 绑定态"
    "reserve_pages() 预取,并且 reserve 后调用过 sync_page_memory)";

// 子块 [id.offset, id.end()) 必须落在页 [0, size) 内。
bool check_range(const resources::ResourceId& id, const PageBinding& pb, std::string* err) {
  if (static_cast<u64>(id.offset) + id.size > pb.size) {
    *err = "子块 [" + std::to_string(id.offset) + ", " + std::to_string(id.end()) +
           ") 越出页 " + std::to_string(id.page_id) + " 的区间 [0, " + std::to_string(pb.size) +
           ")";
    return false;
  }
  return true;
}

VkDeviceSize page_offset_of(const resources::ResourceId& id, const PageBinding& pb) {
  return static_cast<VkDeviceSize>(pb.offset) + static_cast<VkDeviceSize>(id.offset);
}

}  // namespace

Status bind_buffer_memory(VkDevice device, VkBuffer buffer,
                          const resources::ResourceId& id) {
  if (buffer == VK_NULL_HANDLE) return Status::Invalid("bind_buffer: VkBuffer 为空");
  PageBinding pb{};
  if (!page_binding(id.page_id, &pb) || pb.memory == VK_NULL_HANDLE) {
    return Status::Invalid(std::string("bind_buffer: 页 ") + std::to_string(id.page_id) + " " +
                           kNoPageBinding);
  }
  std::string err;
  if (!check_range(id, pb, &err)) return Status::Invalid("bind_buffer: " + err);
  // 按偏移绑定页内存:子块按 64 KiB 对齐(rulings 裁定 3 的强制保证),页按 64 KiB
  // 对齐,因此 VkBuffer 的 size/alignment 要求必然被满足,不需要再查
  // vkGetBufferMemoryRequirements2。
  const VkResult r = vkBindBufferMemory(device, buffer, pb.memory, page_offset_of(id, pb));
  if (r != VK_SUCCESS) return Status::InitFailed(fmt_err("vkBindBufferMemory", r));
  return Status::Ok_();
}

Status bind_image_memory(VkDevice device, VkImage image, const resources::ResourceId& id) {
  if (image == VK_NULL_HANDLE) return Status::Invalid("bind_image: VkImage 为空");
  PageBinding pb{};
  if (!page_binding(id.page_id, &pb) || pb.memory == VK_NULL_HANDLE) {
    return Status::Invalid(std::string("bind_image: 页 ") + std::to_string(id.page_id) + " " +
                           kNoPageBinding);
  }
  std::string err;
  if (!check_range(id, pb, &err)) return Status::Invalid("bind_image: " + err);
  const VkResult r = vkBindImageMemory(device, image, pb.memory, page_offset_of(id, pb));
  if (r != VK_SUCCESS) return Status::InitFailed(fmt_err("vkBindImageMemory", r));
  return Status::Ok_();
}

VkFormat vk_format_of(device::Format f) { return to_vk_format(f); }

VkSampleCountFlagBits to_vk_samples(std::uint32_t n) {
  // 取值集合与 collect_msaa()(vulkan_device.cpp)对齐:1/2/4/8/16。
  // 16 也列上是因为桌面 GPU 普遍支持;移动端通常只到 4。
  switch (n) {
    case 2:
      return VK_SAMPLE_COUNT_2_BIT;
    case 4:
      return VK_SAMPLE_COUNT_4_BIT;
    case 8:
      return VK_SAMPLE_COUNT_8_BIT;
    case 16:
      return VK_SAMPLE_COUNT_16_BIT;
    default:
      return VK_SAMPLE_COUNT_1_BIT;
  }
}

std::uint32_t format_pixel_bytes(device::Format f) {
  switch (f) {
    case device::Format::RGBA8_UNORM:
    case device::Format::BGRA8_UNORM:
    case device::Format::D24_UNORM_S8_UINT:
    case device::Format::D32_SFLOAT:
      return 4;
    case device::Format::R8_UNORM:
      return 1;
    case device::Format::D16_UNORM:
      return 2;
    case device::Format::Unknown:
      break;
  }
  return 0;
}

bool is_depth_format(device::Format f) {
  return f == device::Format::D16_UNORM || f == device::Format::D24_UNORM_S8_UINT ||
         f == device::Format::D32_SFLOAT;
}

bool can_readback_rgba8(device::Format f) {
  // R8 / 深度格式没有完整的 RGBA 通道 → 不静默降级;BGRA8 做一次 R<->B 重排后按 RGBA8 产出。
  return f == device::Format::RGBA8_UNORM || f == device::Format::BGRA8_UNORM;
}

}  // namespace magicblocks::backend
