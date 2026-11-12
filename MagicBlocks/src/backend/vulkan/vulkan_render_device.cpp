// MagicBlocks —— Vulkan 后端:设备实现 + 工厂
#include "backend/vulkan/vulkan_render_device.h"

#include <algorithm>
#include <cstring>

#include "backend/vulkan/vulkan_draw.h"
#include "backend/vulkan/vulkan_cubemap.h"
#include "backend/vulkan/vulkan_buffer.h"
#include "backend/vulkan/vulkan_command.h"
#include "backend/vulkan/vulkan_device_frame.h"
#include "backend/vulkan/vulkan_pipeline.h"
#include "backend/vulkan/vulkan_resource_common.h"  // is_power_of_two_u32 / is_depth_format_of
#include "backend/vulkan/vulkan_swapchain.h"
#include "backend/vulkan/vulkan_texture.h"

namespace magicblocks::backend {

using namespace magicblocks::device;

namespace {
std::string g_device_override;
}

void set_physical_device_override(const std::string& name) { g_device_override = name; }
const std::string& physical_device_override() { return g_device_override; }

// ---------------------------------------------------------------- G8 / t13
//
// Shaped-pipeline factory handed to L1 as an opaque interface, so L3
// (primitives/shader_library.cpp) can derive a pipeline for a target shape
// without including any backend header.
//
// Dependency direction is strictly implementation -> abstraction:
//   INIT: the backend registers a resolver built on L1 public types;
//   RUN : IRenderDevice::pipeline_factory() resolves it for that device;
//   L3  : calls resources::IPipelineFactory::create_shaped_pipeline().
namespace {

class VulkanPipelineFactory final : public resources::IPipelineFactory {
 public:
  device::Result<std::unique_ptr<resources::IPipeline>> create_shaped_pipeline(
      device::IRenderDevice& device, const resources::PipelineDesc& desc,
      const resources::AttachmentShape& shape) override {
    VulkanRenderDevice* vk = dynamic_cast<VulkanRenderDevice*>(&device);
    if (vk == nullptr) {
      return device::Status::Unsupported(
          "create_shaped_pipeline: device is not the Vulkan backend");
    }
    return vk->create_pipeline_for_shape(desc, shape);
  }
};

VulkanPipelineFactory g_pipeline_factory;

resources::IPipelineFactory* resolve_vulkan_pipeline_factory(device::IRenderDevice* device) {
  if (device == nullptr) return nullptr;
  if (device->backend() != device::BackendType::Vulkan) return nullptr;
  return &g_pipeline_factory;
}

}  // namespace


VulkanRenderDevice::~VulkanRenderDevice() {
  // 【白名单第 1 处 / 全仓仅剩 2 处 vkDeviceWaitIdle 之一】设备析构。
  //
  // 为什么这里必须保留 device 级全停,而不能像渲染路径那样改成槽位 fence:
  // 紧随其后的操作是 vkDestroyCommandPool / vmaDestroyAllocator / vkDestroyDevice,
  // 它们会**一次性**拆掉所有帧槽、所有资源、所有分配器页。任何一帧只要还在 GPU 上
  // 跑,销毁就是 use-after-free + VUID-vkDestroyDevice-device-05137(leaked objects)。
  // 槽位 fence 只能证明"我们提交过的那几批完成",证明不了"外部通过 ctx_ 发起的一次性
  // 提交都完成了" —— 析构点必须用最强的等待。
  // 析构是每进程一次,不是 per-frame,代价可接受。
  if (ctx_.device) vkDeviceWaitIdle(ctx_.device);
  // 清空已知资源表:表里存的是裸指针,这里先清掉,避免之后(或在 vkDestroyDevice
  // 之后)仍持有采样器的析构钩子 —— 那个钩子的 lambda 捕获了 this,设备析构完成
  // 后再回调就是 use-after-free。
  known_samplers_.clear();
  known_textures_.clear();
  known_cubes_.clear();
  known_buffers_.clear();
  pages_in_flight_.clear();
  page_slot_refs_.clear();
  // 顺序关键:fence 必须在 VkDevice 之前销毁;
  // 分配器要在 VkDevice 之前销毁(VMA 持有 VkDevice),但晚于所有资源。
  frame_sync_.reset();
  // 逐帧提交的 fence 池 → 已被 VulkanSyncCore 取代(见 vulkan_render_device.h 的注释)。
  // 同步核心同样必须在 VkDevice 之前销毁:它持有 N 槽的 VkSemaphore / VkFence /
  // 持久 VkCommandBuffer,还持有各槽的**延迟销毁栈**(framebuffer/imageView/renderPass)。
  // 上面的 vkDeviceWaitIdle 已保证没有在飞的帧,所以这里的销毁是安全的。
  if (sync_core_) {
    sync_core_->shutdown();
    sync_core_.reset();
  }
  if (ctx_.cmd_pool) vkDestroyCommandPool(ctx_.device, ctx_.cmd_pool, nullptr);
  // 摘掉分配器上的反向引用:它在 allocator_.reset() 之后就不存在了,
  // 而活着的资源(调用方还持有的 unique_ptr)析构时会经它回头找本设备。
  // 设备先死的情况下那些资源只会看到 nullptr(见 forget_* 的说明)。
  if (allocator_ != nullptr) allocator_->set_owner_device(nullptr);
  allocator_.reset();
  ctx_.allocator = nullptr;
  if (ctx_.device) {
    // 进程级的 CUBE 采样器缓存按 VkDevice 持有句柄,设备销毁前必须**真正销毁**
    // 这些 VkSampler,否则每一帧(每个带天空盒的进程)都会漏掉它们:
    //   VUID-vkDestroyDevice-device-05137: ... 1 leaked objects ... VkSampler
    // 注意这里必须用 destroy_cube_samplers(销毁句柄 + 清表),不能只用
    // cube_sampler_cache_forget(只清表、句柄泄漏)。实测:仅 forget 时
    // Sky 套件 8 条用例各漏 1 个,正好 8 条 vk-ERROR。
    // 顺序仍在 vkDestroyDevice 之前:VkSampler 是 VkDevice 的子对象。
    destroy_cube_samplers(ctx_.device);
    vkDestroyDevice(ctx_.device, nullptr);
  }
  destroy_instance(&inst_);
}

Status VulkanRenderDevice::init(const DeviceDesc& desc, const std::string& device_override) {
  // ---- 参数校验(D8:非法参数立刻报错,不"尽量跑") ----
  if (desc.in_flight_frames < 2 || desc.in_flight_frames > 3) {
    return Status::Invalid("in_flight_frames 必须是 2 或 3,得到 " +
                           std::to_string(desc.in_flight_frames));
  }
  if (desc.backend != BackendType::Vulkan) {
    return Status::Unsupported("本轮只实现 Vulkan 后端");
  }

  std::string err;
  // API 版本必须与 backend 实际用到的功能一致:
  //   - VulkanPagingAllocator 建 VMA 时写的是 vulkanApiVersion = VK_API_VERSION_1_1
  //   - L1/L2 资源路径大量调用 vkGetPhysicalDevice*2()（1.1 才提升为核心）
  // 之前这里请求的是 1.0,与 VMA 声明的 1.1 不一致:校验层会报
  // UNASSIGNED-API-Version-Violation,驱动侧 VMA 走 1.0 兼容路径并破坏内部状态,
  // 表现为「创建命令列表后,设备析构时堆损坏(0xc0000374)」——
  // 一个非常难查的 bug:崩在 vkDestroyDevice,根因却在实例创建。
  // SwiftShader 支持 1.3,请求 1.1 是安全下限。
  if (!create_instance(desc.enable_validation, VK_API_VERSION_1_1, &inst_, &err)) {
    return Status::InitFailed(err.empty() ? "创建 Vulkan 实例失败" : err);
  }

  // ---- 枚举物理设备 ----
  std::uint32_t count = 0;
  VkResult r = vkEnumeratePhysicalDevices(inst_.instance, &count, nullptr);
  if (r != VK_SUCCESS || count == 0) {
    return Status::Unsupported("没有可用的 Vulkan 物理设备(" + vk_result_str(r) + ")");
  }
  std::vector<VkPhysicalDevice> phys(count);
  vkEnumeratePhysicalDevices(inst_.instance, &count, phys.data());

  // ---- 设备强筛(C4/E4):require_software_rasterizer 时绝不回退物理 GPU ----
  VkPhysicalDevice chosen = VK_NULL_HANDLE;
  VkPhysicalDeviceProperties chosen_props{};
  std::string seen;
  for (auto p : phys) {
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(p, &props);
    std::string name = device_override.empty() ? std::string(props.deviceName) : device_override;
    bool soft = is_software_rasterizer_name(name.c_str(), props.deviceType);
    if (!seen.empty()) seen += ", ";
    seen += name;
    if (desc.require_software_rasterizer && !soft) continue;
    chosen = p;
    chosen_props = props;
    if (!device_override.empty()) {
      std::snprintf(chosen_props.deviceName, sizeof(chosen_props.deviceName), "%s",
                    device_override.c_str());
    }
    break;
  }
  if (!chosen) {
    return Status::Unsupported(
        std::string("require_software_rasterizer=true 但没有软件光栅设备;已枚举:") + seen +
        "(不会回退到物理 GPU)");
  }

  // ---- 能力探测(D2) ----
  VkPhysicalDeviceMemoryProperties mem{};
  vkGetPhysicalDeviceMemoryProperties(chosen, &mem);
  fill_device_caps(chosen, chosen_props, mem, false, false, &caps_);

  // ---- 请求项校验(A③):不可满足即 Unsupported,绝不静默降级 ----
  if (desc.request_rev_z && !caps_.rev_z_supported) {
    return Status::Unsupported("请求 reverse-Z,但设备不支持 D32_SFLOAT 深度");
  }
  if (desc.requested_msaa != 1) {
    bool found = false;
    for (auto lvl : caps_.supported_msaa_levels) {
      if (lvl == desc.requested_msaa) found = true;
    }
    if (!found) {
      return Status::Unsupported("请求 MSAA " + std::to_string(desc.requested_msaa) +
                                 ", 设备不支持该等级");
    }
  }

  // ---- 队列族:优先带 GRAPHICS 的 ----
  std::uint32_t qn = 0;
  vkGetPhysicalDeviceQueueFamilyProperties(chosen, &qn, nullptr);
  std::vector<VkQueueFamilyProperties> qprops(qn);
  vkGetPhysicalDeviceQueueFamilyProperties(chosen, &qn, qprops.data());
  std::uint32_t qfam = UINT32_MAX;
  for (std::uint32_t i = 0; i < qn; ++i) {
    if (qprops[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
      qfam = i;
      break;
    }
  }
  if (qfam == UINT32_MAX) return Status::Unsupported("没有 GRAPHICS 队列族");

  // ---- 逻辑设备 ----
  float prio = 1.0f;
  VkDeviceQueueCreateInfo qci{};
  qci.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
  qci.queueFamilyIndex = qfam;
  qci.queueCount = 1;
  qci.pQueuePriorities = &prio;

  std::vector<const char*> dev_exts;
  // swapchain 扩展:枚举可用性后再决定,供窗口路径使用
  std::uint32_t en = 0;
  vkEnumerateDeviceExtensionProperties(chosen, nullptr, &en, nullptr);
  std::vector<VkExtensionProperties> wexts(en);
  if (en) vkEnumerateDeviceExtensionProperties(chosen, nullptr, &en, wexts.data());
  for (const auto& e : wexts) {
    if (std::strcmp(e.extensionName, VK_KHR_SWAPCHAIN_EXTENSION_NAME) == 0) {
      dev_exts.push_back(VK_KHR_SWAPCHAIN_EXTENSION_NAME);
      break;
    }
  }

  // 先查询物理设备特性再创建逻辑设备。
  // 直接构造全零 VkPhysicalDeviceFeatures 当 pEnabledFeatures 传入是违规的
  // (校验层报: called before getting physical device features from
  //  vkGetPhysicalDeviceFeatures or vkGetPhysicalDeviceFeatures2)。
  VkPhysicalDeviceFeatures supported_feats{};
  vkGetPhysicalDeviceFeatures(chosen, &supported_feats);
  VkPhysicalDeviceFeatures feats{};
  feats.samplerAnisotropy = supported_feats.samplerAnisotropy;
  feats.shaderStorageImageWriteWithoutFormat =
      supported_feats.shaderStorageImageWriteWithoutFormat;
  feats.independentBlend = supported_feats.independentBlend;
  feats.fillModeNonSolid = supported_feats.fillModeNonSolid;

  VkDeviceCreateInfo dci{};
  dci.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
  dci.queueCreateInfoCount = 1;
  dci.pQueueCreateInfos = &qci;
  dci.enabledExtensionCount = static_cast<std::uint32_t>(dev_exts.size());
  dci.ppEnabledExtensionNames = dev_exts.empty() ? nullptr : dev_exts.data();
  dci.pEnabledFeatures = &feats;
  r = vkCreateDevice(chosen, &dci, nullptr, &ctx_.device);
  if (r != VK_SUCCESS) return Status::InitFailed(fmt_err("vkCreateDevice", r));

  ctx_.instance = inst_.instance;
  ctx_.phys = chosen;
  ctx_.props = chosen_props;
  ctx_.mem = mem;
  ctx_.queue_family = qfam;
  vkGetDeviceQueue(ctx_.device, qfam, 0, &ctx_.queue);

  VkCommandPoolCreateInfo cpci{};
  cpci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
  cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
  cpci.queueFamilyIndex = qfam;
  r = vkCreateCommandPool(ctx_.device, &cpci, nullptr, &ctx_.cmd_pool);
  if (r != VK_SUCCESS) return Status::InitFailed(fmt_err("vkCreateCommandPool", r));

  // ---- L1 分页分配器(spec C7 / M1-M6) ----
  // 必须在任何资源/目标创建之前建好:全仓零裸分配(G4),所有显存都从它来。
  {
    AllocatorHandles handles;
    handles.instance = reinterpret_cast<std::uintptr_t>(ctx_.instance);
    handles.physical_device = reinterpret_cast<std::uintptr_t>(ctx_.phys);
    handles.device = reinterpret_cast<std::uintptr_t>(ctx_.device);
    AllocatorConfig acfg;
    acfg.memory_budget = caps_.memory_budget;  // M6:预算来自 L0 的 DeviceCaps
    allocator_ = std::make_unique<VulkanPagingAllocator>(handles, acfg);
    Status as = allocator_->init();
    if (!as.ok()) return as;  // 硬失败:绝不静默退回裸分配
    ctx_.allocator = allocator_.get();
  }

  // ---- L0 多帧并行同步核心 ----
  //
  // N 就是 desc.in_flight_frames(与 IFrameSync 同一个数,不再有"fence 池 4 个、
  // in-flight 2 个"这种两套口径)。每槽:image_available/render_finished 语义量
  // 各一、submit fence、**持久**命令缓冲、延迟销毁栈。
  // 命令池已带 RESET_COMMAND_BUFFER_BIT(见上面的 cpci.flags),所以复用只需
  // vkResetCommandBuffer,不必新建池。
  //
  // 【顺序关键:core 先建,frame_sync 后建】IFrameSync 现在只是 core 的薄委托
  // (它不再自己持有任何 VkFence),所以必须先有 core 才能建它 —— 这也是
  // "帧号单一真源"的结构保证:FrameSync 读不到任何属于自己的计数器。
  sync_core_ = std::make_unique<VulkanSyncCore>();
  Status sync_status = sync_core_->init(ctx_.device, ctx_.cmd_pool, desc.in_flight_frames);
  if (!sync_status.ok()) {
    sync_core_.reset();
    return sync_status;  // 硬失败:没有同步核心就没有多帧并行,绝不静默退回串行
  }
  // 帧同步:纯委托,不拥有 core(core 由本设备独占持有)。
  frame_sync_ = std::make_unique<VulkanFrameSync>(sync_core_.get());
  // ---- 帧边界接线:让 L1 分配器的"在飞帧防护"从死代码变成活代码 ----
  //
  // 改造前 trim() 的唯一调用点在 IMemoryAllocator::begin_frame() 内部,而它在
  // src/ 里**零调用**;于是 set_page_in_flight() 也从没人调用、p.in_flight 恒为
  // false,trim() 的 `if (p.in_flight) continue;` 是一道恒假的判断。
  // 一旦 begin_frame 接上生产路径,trim() 就变成每帧都跑的真淘汰路径 ——
  // 那道防护必须同时变成真的,否则就是"页刚被淘汰、GPU 还在读"。
  //
  // 所以:同步核心每确认一个槽位跑完(begin_frame/drain),就回调这里;
  // 这里按顺序做 解除在途标记 → 推帧号(内部 trim)。
  // 帧号来源是 frame_index_(单调递增),**不是** current_slot()(0..N-1 循环),
  // 两者语义不同:分配器要的是"哪一帧已经彻底出清",槽号给不出这个信息。
  // 按槽记账的在途页表:槽数与同步核心的 N 必须一致,所以在这里一次分配好,
  // 之后 mark/clear 只按下标访问,不再扩容(扩容会在指针/迭代期间改变大小)。
  pages_in_flight_.assign(static_cast<std::size_t>(desc.in_flight_frames), {});
  sync_core_->set_frame_tick([this](std::uint64_t frame_index, std::uint32_t slot) {
    on_frame_boundary(frame_index, slot);
  });
  // 资源 → 设备的反向引用:资源析构时要通知本设备把裸指针从 known_* 表里摘掉。
  // 走分配器转一手是因为资源手上只有 (ctx, allocator),而分配器由本设备独占持有
  // 且生命周期覆盖全部资源(见 VulkanPagingAllocator::set_owner_device)。
  if (allocator_ != nullptr) allocator_->set_owner_device(this);
  logical_ready_ = true;
  return Status::Ok_();
}

Result<std::unique_ptr<IRenderTarget>> VulkanRenderDevice::create_offscreen_target(
    const TargetDesc& desc) {
  if (!logical_ready_) return Status::InitFailed("设备未初始化");
  if (desc.kind == TargetDesc::Kind::Window) {
    return Status::Invalid("离屏目标不能用 kind=Window,请用 create_window_target");
  }
  auto t = std::make_unique<OffscreenTarget>(ctx_, desc);
  Status s = t->init();
  if (!s.ok()) return s;
  return std::unique_ptr<IRenderTarget>(std::move(t));
}

Result<std::unique_ptr<IRenderTarget>> VulkanRenderDevice::create_window_target(
    const TargetDesc& desc) {
  (void)desc;
  // L0 不提供独立窗口目标:窗口出图走 create_swapchain + current_target()。
  return Status::Unsupported(
      "create_window_target: L0 用 create_swapchain() 承载窗口出图(保持单一 owning 路径)");
}

Result<std::unique_ptr<ISwapchain>> VulkanRenderDevice::create_swapchain(const NativeSurface& surface,
                                                                        Extent2D extent) {
  if (!logical_ready_) return Status::InitFailed("设备未初始化");
  auto sc = std::make_unique<VulkanSwapchain>(ctx_, surface);
  Status s = sc->init(extent);
  if (!s.ok()) return s;
  return std::unique_ptr<ISwapchain>(std::move(sc));
}

Result<std::unique_ptr<ISwapchain>> VulkanRenderDevice::create_swapchain_with_msaa(
    const NativeSurface& surface, Extent2D extent, std::uint32_t msaa_samples) {
  if (!logical_ready_) return Status::InitFailed("设备未初始化");
  auto sc = std::make_unique<VulkanSwapchain>(ctx_, surface);
  // 顺序不能反：采样数决定 init() 里要建几张什么规格的 MS 图。
  sc->set_requested_msaa(msaa_samples);
  Status s = sc->init(extent);
  if (!s.ok()) return s;
  return std::unique_ptr<ISwapchain>(std::move(sc));
}

Status VulkanRenderDevice::notify_lifecycle(const AppLifecycleEvent& ev) {
  (void)ev;
  return Status::Ok_();
}

Status VulkanRenderDevice::wait_idle() {
  if (!ctx_.device) return Status::Ok_();
  // 【改动】原先直接 vkDeviceWaitIdle —— 渲染路径上每调一次就把 CPU/GPU 重叠清零。
  //
  // 现在语义不变("调用返回后,本设备发起的 GPU 工作全部完成"),但实现是
  // **排空在飞帧的槽位 fence**,而不是 queue/device 级全停。
  // 设备一定登记过自己(create_render_device → init → sync_core_->init),
  // 所以这里不会命中"未登记"分支。
  Status s = wait_device_frames_idle(ctx_);
  return s;
}

// ---------------------------------------------------------------- 资源自注销

// 三个泛型版本,只做"按指针相等删一条"。
namespace {

template <typename Entry>
void erase_entry_by_pointer(std::vector<Entry>& table, const void* victim) {
  if (victim == nullptr) return;
  for (auto it = table.begin(); it != table.end(); ++it) {
    if (static_cast<const void*>(it->second) == victim) {
      table.erase(it);
      return;
    }
  }
}

}  // namespace

void VulkanRenderDevice::forget_known_buffer(const resources::IBuffer* buffer) {
  erase_entry_by_pointer(known_buffers_, buffer);
}

void VulkanRenderDevice::forget_known_texture(const resources::ITexture* texture) {
  erase_entry_by_pointer(known_textures_, texture);
}

void VulkanRenderDevice::forget_known_cube_map(const resources::ICubeMap* cube) {
  erase_entry_by_pointer(known_cubes_, cube);
}

// ---------------------------------------------------------------- 帧边界 / 在飞页防护

void VulkanRenderDevice::mark_pages_in_flight(std::uint32_t slot,
                                              const std::vector<resources::u32>& pages) {
  if (allocator_ == nullptr) return;
  if (slot >= pages_in_flight_.size()) return;
  auto& slot_pages = pages_in_flight_[slot];
  for (resources::u32 page_id : pages) {
    // 同一槽内去重:这一个槽对同一个页只持有一份引用。
    //
    // 去重只能发生在"本槽"这一层,并且必须与 clear_pages_in_flight 的
    // 逐页 −1 严格对称,否则就是"页被永远钉住、trim() 的整页淘汰静默失效"。
    if (std::find(slot_pages.begin(), slot_pages.end(), page_id) != slot_pages.end()) continue;
    slot_pages.push_back(page_id);
    // 引用计数 +1;0 → 1 时才需要真的告诉分配器"这一个页在途"。
    auto it = std::find_if(page_slot_refs_.begin(), page_slot_refs_.end(),
                           [page_id](const std::pair<resources::u32, std::uint32_t>& kv) {
                             return kv.first == page_id;
                           });
    if (it == page_slot_refs_.end()) {
      page_slot_refs_.emplace_back(page_id, 1u);
      allocator_->set_page_in_flight(page_id, true);
    } else {
      ++it->second;
    }
  }
}

void VulkanRenderDevice::clear_pages_in_flight(std::uint32_t slot) {
  if (slot >= pages_in_flight_.size()) return;
  auto& slot_pages = pages_in_flight_[slot];
  // 与 mark_pages_in_flight 严格对称:遍历本槽登记过的**每一个**页,逐个 −1。
  //
  // 计数 >1 说明还有别的槽持有同一个页:本槽这份引用已经退还,但对分配器
  // 仍要继续报 in_flight(其余槽可能还在 GPU 上读它),所以只 −1 不解除。
  for (resources::u32 page_id : slot_pages) {
    auto it = std::find_if(page_slot_refs_.begin(), page_slot_refs_.end(),
                           [page_id](const std::pair<resources::u32, std::uint32_t>& kv) {
                             return kv.first == page_id;
                           });
    if (it == page_slot_refs_.end()) continue;  // 记账已丢:不该发生,但别越界
    if (it->second > 1) {
      --it->second;
      continue;
    }
    page_slot_refs_.erase(it);
    // 最后一个引用也没了 —— 这才是唯一可以解除保护的时刻。
    if (allocator_ != nullptr) allocator_->set_page_in_flight(page_id, false);
  }
  // 本槽这一轮的登记全部出清(与上面逐页 −1 一一对应)。
  slot_pages.clear();
}

void VulkanRenderDevice::on_frame_boundary(std::uint64_t frame_index, std::uint32_t slot) {
  // 到这里,同步核心已经用 fence 证明"槽 slot 上那一帧跑完了"。
  // **只清这一个槽**的在途标记:其余 N−1 个槽可能还在 GPU 上跑,它们的页不能松手。
  // (曾经无条件全清 —— 那会在第 f+1 帧的边界就解开第 f 帧仍在读的页。)
  clear_pages_in_flight(slot);
  if (allocator_ != nullptr) allocator_->begin_frame(frame_index);
}

// ---------------------------------------------------------------- L1 资源层工厂

Result<std::unique_ptr<resources::IBuffer>> VulkanRenderDevice::create_buffer(
    const resources::BufferDesc& desc) {
  if (!logical_ready_ || allocator_ == nullptr) return Status::InitFailed("设备未初始化");
  auto b = std::make_unique<VulkanBuffer>(ctx_, allocator_.get(), desc);
  Status s = b->init();
  if (!s.ok()) return s;
  // 登记句柄 → 指针,供 submit_command_list() 反查(见 bind_known_buffers)。
  //
  // 用"覆盖同 id"而不是 emplace_back:表里存的是裸指针,而资源析构后没有任何
  // 摘表钩子。ResourceId 会被复用(分页分配器回收页 → 新资源拿到同一个 id),
  // 若只追加不覆盖,反查会命中**已释放的旧指针** → 0xC0000005 /
  // vkEndCommandBuffer: invalid state,且表现为"单跑绿、全量红"。
  // 覆盖保证了"同一个 id 永远只指向最新一次登记的对象"。
  if (b->id().valid()) {
    const auto id = b->id();
    bool replaced = false;
    for (auto& kv : known_buffers_) {
      if (kv.first == id) {
        kv.second = b.get();
        replaced = true;
        break;
      }
    }
    if (!replaced) known_buffers_.emplace_back(id, b.get());
  }
  // 返回裸 VulkanBuffer(所有权在调用方的 unique_ptr)。
  //
  // 【曾在这里加过"退役缓冲包装"做延迟销毁,已删除】
  // 起因是 F-VX 的 src/features/stack/stack_render.cpp:124 每帧 `slot->reset()`,
  // 会销毁上一帧 GPU 可能还在读的 VkBuffer。我在 L0 造了一套"退役栈"来兜住它,
  // 但那是让全仓基础设施(create_buffer)去承担某一个 feature 的生命周期 ——
  // 影响面覆盖所有层,收益却只服务于一处。而正确的修法成本低一到两个数量级:
  // IFrameSync 已提供 in_flight_count()/slot_for_frame()/wait_slot(),
  // StackRenderer 通过 device_->frame_sync() 直接就能拿到,把那两个缓冲成员
  // 改成 N 元素环即可,**只动 stack_render.cpp 一个文件**。已交 F-VX。
  return std::unique_ptr<resources::IBuffer>(std::move(b));
}

Result<std::unique_ptr<resources::ITexture>> VulkanRenderDevice::create_texture(
    const resources::TextureDesc& desc) {
  if (!logical_ready_ || allocator_ == nullptr) return Status::InitFailed("设备未初始化");
  auto t = std::make_unique<VulkanTexture>(ctx_, allocator_.get(), desc);
  Status s = t->init();
  if (!s.ok()) return s;
  // 登记句柄 → 指针,供 submit_command_list() 反查(bind_sampled_texture 需要)。
  // 覆盖同 id,理由见 create_buffer。
  if (t->id().valid()) {
    const auto id = t->id();
    bool replaced = false;
    for (auto& kv : known_textures_) {
      if (kv.first == id) {
        kv.second = t.get();
        replaced = true;
        break;
      }
    }
    if (!replaced) known_textures_.emplace_back(id, t.get());
  }
  return std::unique_ptr<resources::ITexture>(std::move(t));
}

Result<std::unique_ptr<resources::ICubeMap>> VulkanRenderDevice::create_cube_map(
    const resources::CubeMapDesc& desc) {
  if (!logical_ready_ || allocator_ == nullptr) return Status::InitFailed("设备未初始化");
  auto c = std::make_unique<VulkanCubeMap>(ctx_, allocator_.get(), desc);
  Status s = c->init();
  if (!s.ok()) return s;
  // 立方体贴图与 ITexture 是两个接口(ICubeMap 不是 ITexture),单独一张表。
  // 覆盖同 id,理由见 create_buffer。
  if (c->id().valid()) {
    const auto cid = c->id();
    bool replaced = false;
    for (auto& kv : known_cubes_) {
      if (kv.first == cid) {
        kv.second = c.get();
        replaced = true;
        break;
      }
    }
    if (!replaced) known_cubes_.emplace_back(cid, c.get());
  }
  return std::unique_ptr<resources::ICubeMap>(std::move(c));
}

Result<std::unique_ptr<resources::ISampler>> VulkanRenderDevice::create_sampler(
    const resources::SamplerDesc& desc) {
  if (!logical_ready_) return Status::InitFailed("设备未初始化");
  auto s = std::make_unique<VulkanSampler>(ctx_, desc);
  Status st = s->init();
  if (!st.ok()) return st;
  // 采样器的句柄页号是 kSamplerPage、offset 恒 0(见 VulkanSampler 构造),
  // 因此**所有采样器的 (page_id, offset) 完全相同** —— 反查表不能靠 id 区分它们。
  //
  // 这意味着:若用 emplace_back 只追加,表里会堆满历史上创建过的每一个 VulkanSampler
  // 裸指针,而反查(sampler_at)命中**第一条**,即最陈旧的那个 —— 前面的用例
  // 结束后对象已析构,下一个用例一用就是 SEH 0xc0000005。
  // 实测表现:单条 Sky.* 用例跑绿,但在全量串跑里("前一条用例刚销毁采样器")
  // 变红/崩溃。所以这里必须**覆盖式登记**:同一个 (page,offset) 键永远只保留最新
  // 一次创建的对象,旧对象被覆盖后被反查彻底遗忘。
  //
  // 表键用 s->id()(VulkanSampler 构造时已设为 kSamplerPage / offset 0 / size 1 /
  // id = slot_index,见 vulkan_texture.cpp)。**槽位号 id 是唯一身份**:
  // 所有采样器的 (page_id, offset) 完全相同,只有 id 不同。
  // 覆盖语义按 id 匹配 —— 同一槽位号重建时覆盖,不同槽位号各占一行。
  {
    const auto key = s->id();
    bool replaced = false;
    for (auto& kv : known_samplers_) {
      if (kv.first.id == key.id) {
        kv.second = s.get();
        replaced = true;
        break;
      }
    }
    if (!replaced) known_samplers_.emplace_back(key, s.get());
    // 析构摘表:known_samplers_ 存的是裸指针,对象一析构这里就成了悬垂指针。
    // 下一个用例/下一帧反查就会读到已销毁的 VkSampler —— 校验层报
    // "invalid VkSampler ... or has been destroyed",SwiftShader 下直接 SEH。
    // 所以必须由对象在析构时把自己从表里摘掉(按指针身份匹配,和键无关)。
    s->set_destroy_hook([this](VulkanSampler* dying) {
      auto it = std::find_if(known_samplers_.begin(), known_samplers_.end(),
                             [dying](const auto& kv) { return kv.second == dying; });
      if (it != known_samplers_.end()) known_samplers_.erase(it);
    });
  }
  return std::unique_ptr<resources::ISampler>(std::move(s));
}

Result<std::unique_ptr<resources::IPipeline>> VulkanRenderDevice::create_pipeline(
    const resources::PipelineDesc& desc) {
  if (!logical_ready_) return Status::InitFailed("设备未初始化");
  // ⚠️ 公共面 create_pipeline(desc) **不携带附件形状** —— 这是 IPipeline 的既定契约,
  // 改它会打断 L1/L3 的全部调用点。所以这条入口建出来的管线自带 pass 固定是
  // 「RGBA8 + 1 采样 + 无深度 + 非交换链」,只能画到恰好同形的离屏目标上。
  //
  // 2026-11-11 修:以前这里**依赖默认构造**得到一个 has_depth=false 的形状,而没有任何
  // 地方把这件事说出来,于是 L3 的所有管线(带深度目标 + samples=4)都静默退回到
  // create_pass() 回退分支 —— t6 的案例表明这会让 `PipelineDesc.samples` 对 render pass
  // 完全无效、`clear_depth` 被丢弃。
  //
  // 现在改成**显式推导**:颜色格式取 RGBA8(与离屏默认一致),深度取自 desc.depth —— 
  // 只要管线声明了深度测试/深度写/常量深度写,它的自带 pass 就必须有深度附件,
  // 否则该管线永远不可能与"带深度的目标"兼容。samples 取自 desc.samples。
  // 这样 `create_pipeline(scene_pipeline_desc(4))`(depthTest+depthWrite=true)得到的
  // 就是 {RGBA8, 1 采样? 不 —— 4 采样, 有深度},与 l3-cli / F-SK 的真实目标形状一致。
  // 颜色格式:公共面只能表达"离屏默认形状"(RGBA8)。
  // 必须与 **L0 建离屏目标时的取法**同源,而不是"设备首选颜色格式"。
  //
  // 为什么改(2026-11-11):以前这里取 `caps_.texture_formats[0]`(SwiftShader 上是
  // BGRA8,枚举值 2),而 `OffscreenTarget` 用的是 `TargetDesc::format`,
  // 其默认值是 `RGBA8_UNORM`(`device/render_target.h`,枚举值 1)。
  // 两处从不同的来源取同一个字段 —— 这是"派生与目标分叉"的结构性隐患:
  // SwiftShader 恰好把 RGBA8 排在 texture_formats[0] 之外,只要换实现或换顺序就会翻车。
  //
  // ⚠️ 但请注意:这不是天空盒那 8 条像素用例全清的根因。实测改完之后
  // `[pipeline] ... color_format=1` 与改之前完全一样(1 本来就是 RGBA8_UNORM,
  // `Format` 的 Unknown=0 之后第一个枚举),即**颜色格式一直是对的**。
  // 天空盒真正的失败点是"管线不是本设备的 VulkanPipeline"——见 sky.cpp 的
  // 设备归属问题。这里只是一次正确的去隐患,不是那个 bug 的修复。
  AttachmentShape shape;
  shape.color_format = device::Format::RGBA8_UNORM;
  // 显式输出一次:这条派生规则是"静默失效"的高发区(MSAA/深度都曾经死在这里)。
  std::fprintf(stderr,
               "[pipeline] create_pipeline 派生形状: color_format=%u has_depth=%d samples=%u\n",
               static_cast<unsigned>(shape.color_format),
               (desc.depth.depthTest || desc.depth.depthWrite || desc.depth.depthConstWrite) ? 1 : 0,
               static_cast<unsigned>(desc.samples == 0 ? 1u : desc.samples));
  shape.has_depth = desc.samples > 0 && (desc.depth.depthTest || desc.depth.depthWrite ||
                                         desc.depth.depthConstWrite);
  shape.depth_format = device::Format::D16_UNORM;  // 与 OffscreenTarget 建图格式一致
  shape.samples = desc.samples == 0 ? 1u : desc.samples;
  shape.from_swapchain = false;  // 公共面不建交换链管线,见 create_pipeline_for_shape

  // 能力纪律(t6 硬约束):按形状派生的每一个字段都必须自洽,不自洽就明确报错。
  if (shape.samples > 1 && !is_power_of_two_u32(shape.samples)) {
    return Status::Invalid("create_pipeline: samples=" + std::to_string(shape.samples) +
                           " 不是 2 的幂,无法派生附件形状");
  }
  auto p = std::make_unique<VulkanPipeline>(ctx_, desc, shape);
  Status s = p->init();
  if (!s.ok()) return s;
  return std::unique_ptr<resources::IPipeline>(std::move(p));
}

Result<std::unique_ptr<resources::IPipeline>> VulkanRenderDevice::create_pipeline_for_shape(
    const resources::PipelineDesc& desc, const resources::AttachmentShape& shape) {
  if (!logical_ready_) return Status::InitFailed("设备未初始化");
  // 能力纪律:形状的三个关键字段必须被调用方**明确给出**且自洽。
  //   - color_format 不能是 Unknown(否则 vk_format_of 得到 UNDEFINED,建 pass 必失败);
  //   - samples 必须是 2 的幂;
  //   - 有深度时深度格式必须是深度格式。
  if (shape.color_format == device::Format::Unknown) {
    return Status::Invalid("create_pipeline_for_shape: color_format=Unknown —— "
                           "必须按目标真实颜色格式派生(见 IRenderTarget::color_format)");
  }
  if (shape.samples == 0 || !is_power_of_two_u32(shape.samples)) {
    return Status::Invalid("create_pipeline_for_shape: samples=" +
                           std::to_string(shape.samples) + " 必须是 >=1 的 2 的幂");
  }
  if (shape.has_depth && !is_depth_format_of(shape.depth_format)) {
    return Status::Invalid("create_pipeline_for_shape: has_depth=true 但 depth_format 不是深度格式"
                           " —— 必须按目标真实深度格式派生");
  }
  // samples 必须与 desc 一致:管线按 N 采样光栅、pass 按 M 采样声明时,
  // vkCmdDraw 会被静默丢弃(只留清屏色)。在创建期就说清楚,比在屏幕上查黑屏便宜。
  const u32 desc_samples = desc.samples == 0 ? 1u : desc.samples;
  if (shape.samples != desc_samples) {
    return Status::Invalid("create_pipeline_for_shape: 形状 samples=" +
                           std::to_string(shape.samples) + " 与 PipelineDesc.samples=" +
                           std::to_string(desc_samples) +
                           " 不一致 —— render pass 与管线的采样数必须逐项相同");
  }
  auto p = std::make_unique<VulkanPipeline>(ctx_, desc, shape);
  Status s = p->init();
  if (!s.ok()) return s;
  return std::unique_ptr<resources::IPipeline>(std::move(p));
}

Result<std::unique_ptr<resources::ICommandList>> VulkanRenderDevice::create_command_list() {
  if (!logical_ready_) return Status::InitFailed("设备未初始化");
  auto c = std::make_unique<VulkanCommandList>(ctx_);
  Status s = c->init();
  if (!s.ok()) return s;
  // **把设备的资源表挂成"随用随刷"的回调**,而不是创建时快照一次。
  //
  // 为什么必须是这样:句柄 → 指针 的反查表若在创建时固化,那么"先建命令列表、
  // 后建纹理/采样器"这条最自然的顺序就会在 bind_sampled_texture 里报
  // "未登记该纹理/采样器对象"(实测:天空盒在 CLI 与测试里都栽在这一步),
  // 而唯一的补救是调 backend 私有入口 —— L3 看不到。
  // 挂回调之后,任何一个命令列表在 begin_pass 时都能看到**当时**已存在的全部资源,
  // 调用方不必关心创建顺序。
  VulkanCommandList* raw = c.get();
  raw->set_sync_resources_callback([this](VulkanCommandList& list) {
    bind_known_buffers(list);
    bind_known_textures(list);
  });
  return std::unique_ptr<resources::ICommandList>(std::move(c));
}

// ---------------------------------------------------------------- 设备帧提交

// 命令列表里只有 ResourceId,设备帧提交需要 IBuffer*。这里按设备自己登记过的
// 句柄表把指针补进命令列表(命令列表内部维护 ResourceId → IBuffer* 的反查表)。
//
// ⚠️ 只登记**本次命令流实际引用到的句柄**(2026-11-11 修跨用例崩溃)。
// 以前这里无条件遍历 `known_buffers_` 全表登记,而那张表**只增不减**:
// 资源析构后指针仍留在表里,命令列表拿到的就是悬垂指针。
// 单条用例跑看不出来(进程里只有一个资源),但全量串跑时大量资源反复创建/销毁,
// ResourceId 会被复用 → 反查命中已释放内存 → `0xC0000005` /
// `vkEndCommandBuffer(): command buffer is in an invalid state`,
// 而且表现为"单跑绿、全量红"的幽灵失败。
void VulkanRenderDevice::bind_known_buffers(VulkanCommandList& list) const {
  // ⚠️ **必须全量登记,不能只登记"命令流里已出现的句柄"** —— 与 bind_known_textures
  // 是同一个鸡生蛋问题(见那里的长注释)。
  //
  // bind_vertex_buffer / bind_index_buffer 会在 **append(cmd) 之前**调用
  // sync_resources()(为了在录制期就能查到句柄),此刻这条 bind 命令还没进
  // recorded_ —— 于是"按 recorded() 过滤"的条件永远为假,缓冲永远登记不上,
  // 录制期报 "bind_vertex_buffer: ..." 失败,进而 begin_pass 半途返回错误、
  // in_pass_ 停在 false,后续 draw 被拦成 "draw_indexed: 不在 pass 内"。
  // 这正是 F-SK / F-VX 出不了图的直接原因。
  //
  // 陈旧句柄的正确解法不是"过滤",而是**资源析构时把自己从表里摘掉**
  // (与 known_textures_ 同口径,见 VulkanRenderDevice 的说明)。
  for (const auto& kv : known_buffers_) {
    (void)list.remember_buffer_binding(kv.first, kv.second);
  }
}

void VulkanRenderDevice::bind_known_textures(VulkanCommandList& list) const {
  // 纹理 / 立方体贴图 / 采样器是三张独立的表,而 remember_texture_sampler 一次登记
  // 一对(纹理 + 采样器)。这里按表分别登记。
  //
  // ⚠️ **必须全量登记,不能只登记"命令流里已出现的句柄"**。
  //
  // 这里曾经按 `list.recorded()` 里出现过的 ResourceId 过滤(本意是"别把指向已析构
  // 资源的陈旧句柄灌进去"),结果造成一个鸡生蛋问题:
  //   bind_sampled_texture 在**录制期**就要查这张表(R1/X1 契约要求槽位越界之类的
  //   错误在录制时报出来),而那时这条命令**还没有进 recorded_** ——
  //   于是过滤条件永远为假,采样器永远登记不上,录制期直接报
  //   "未登记该采样器对象"。表现为 8 条 Sky.* 用例全红(单独跑也红)。
  //
  // 陈旧句柄的正确解法不是"过滤",而是**资源析构时把自己从表里摘掉**
  // (与 known_buffers_ 的弱引用同口径;见 VulkanRenderDevice 的说明)。
  // 表本身只增不减的话会留下悬垂指针 —— 那正是上一轮的崩溃来源。
  for (const auto& kv : known_textures_) {
    (void)list.remember_texture_sampler(kv.first, kv.second, resources::ResourceId{}, nullptr);
  }
  for (const auto& kv : known_cubes_) {
    (void)list.remember_cube_map(kv.first, kv.second);
  }
  for (const auto& kv : known_samplers_) {
    (void)list.remember_texture_sampler(resources::ResourceId{}, nullptr, kv.first, kv.second);
  }
}

Status VulkanRenderDevice::submit_command_list(resources::ICommandList& list) {
  if (!logical_ready_) return Status::InitFailed("submit_command_list: 设备未初始化");

  // 认具体实现类型拿 recorded():公共面故意不暴露命令流细节。
  auto* cmd = dynamic_cast<VulkanCommandList*>(&list);
  if (cmd == nullptr) {
    return Status::Unsupported(
        "submit_command_list: 只支持由本设备的 create_command_list() 产生的命令列表");
  }
  if (cmd->in_pass()) {
    return Status::Invalid("submit_command_list: 命令列表还有未 end_pass 的 pass");
  }
  if (cmd->command_count() == 0) {
    return Status::Invalid("submit_command_list: 命令列表为空(没有可下发的绘制)");
  }

  // 补全句柄 → 指针的反查表,否则设备帧提交会报"缓冲不是本设备的 VulkanBuffer"。
  bind_known_buffers(*cmd);
  // 纹理/采样器同理(bind_sampled_texture 的设备态需要 ITexture*/ISampler*)。
  bind_known_textures(*cmd);

  // ---- 【进入设备帧提交之前】先丢掉旁路 render pass ----
  //
  // t23 实测(不是推测):begin_pass 的 simple_shape 分支(vulkan_command.cpp)
  // 会在设备态**另建**一条真实 VkRenderPass(initialLayout=UNDEFINED,
  // finalLayout=GENERAL)+ framebuffer + 视图,把 vkCmdBeginRenderPass 录进旁路
  // vk_cmd_ —— 而 submit_device_frame() 根本不执行 vk_cmd_,它用槽位命令缓冲
  // 重放 recorded_ 并**自己**建 pass。
  //
  // 只要旁路对象还活着,调用方 target 的 VkImage 就同时挂在两个布局链矛盾的
  // pass 上;submit_device_frame 内部 begin_frame 复位命令缓冲之后,
  // 回放的这次绘制就**静默不出片元**,整屏只剩清屏色(校验层不一定报警)。
  // 所以丢弃必须发生在这里,而不是像原来那样等提交返回后才做 ——
  // 那样已经晚了:被吃掉的是**这一次**绘制。
  //
  // 丢弃是安全的:命令流 recorded_ 是值语义的独立副本;走设备帧提交这条路,
  // vk_cmd_ 里的内容全是死代码(真正要它的 submit_and_wait() 是另一条通路)。
  cmd->abandon_vk_side_channel();

  // 走设备帧提交通路(11 类命令 → vkCmd* → 本槽位 semaphore/fence,异步)。
  // 统计只用于本地断言:录了命令却一个 draw 都没下发属于配置错误,要报出来,
  // 而不是返回成功让上层去翻一张空图。
  DeviceFrameStats stats{};
  // 【fence 池已并入 VulkanSyncCore】原来这里用 submit_fences_[4] 轮转把 fence 传给
  // submit_device_frame();现在 N 槽的 fence/信号量/命令缓冲都在 sync_core_ 里,
  // 数量与 IFrameSync::in_flight_count() 是同一个数。
  //
  // 【提交不再同步阻塞】submit_device_frame 返回时 GPU 可能还在跑:
  //   - present 侧由 render_finished 信号量守(vulkan_swapchain.cpp);
  //   - 需要像素的调用方读回时由 readback_rgba8() 自己定向等(见 vulkan_target.cpp)。
  Status s = sync_core_ ? submit_device_frame(ctx_, *cmd, &stats, sync_core_.get())
                        : Status::InitFailed("submit_command_list: 缺少同步核心(设备未初始化?)");
  // ---- 在飞帧防护:把本批提交实际引用的内存页标成"在途" ----
  //
  // 提交成功之后,这批命令绑定的页(顶点/索引/Uniform)在被 GPU 读完之前
  // 都不允许被 L1 的 trim() 整页退掉。改造前这一步根本不存在:begin_frame
  // 零调用 → trim() 零调用 → `if (p.in_flight) continue;` 恒假。
  // 现在改成真淘汰路径了,这里必须成对地立起标记:
  //   置 true:提交时(此处);
  //   置 false:该槽位 fence 确认信号后(见 on_frame_boundary → clear_pages_in_flight)。
  // 句柄是 M3 的值语义(page_id + offset),而页淘汰的粒度正是 page_id。
  //
  // ⚠️ 页清单来自 submit_device_frame 的 `stats.pages`(**翻译成功并入过
  //    vkCmdBind\* 的**那些),不是从 recorded() 里扫出来的。命令流里记了句柄、
  //    但设备帧提交因为类型不符而跳过的那条没有下发,把它标成在途就等于
  //    让一页永远淘汰不掉。
  if (s.ok()) {
    // 记入**刚提交完的那一帧所在槽**。
    //
    // 【必须是 last_submitted_slot(),不是 current_slot()】2026-11-11 修(t20 复核发现)。
    // submit_device_frame 内部先 core->begin_frame() 认领槽,提交后再调
    // core->end_frame() —— 而 end_frame 会执行 ++frame_index_,于是**本函数返回时**
    // current_slot() 已经指向**下一帧**的槽。用它会一错到底:
    //   - 在飞页标记记到下一帧的槽上:下一帧 begin_frame 一来就 clear_pages_in_flight
    //     把本帧的页全当"已跑完"松手 → trim() 可在这帧还在 GPU 上时整页退给 VMA;
    //   - 缓冲退休票据托管到下一帧的槽上:下一帧 begin_frame 一进来就对该槽
    //     destroy_all() → 本帧还在读的 VkBuffer 被销毁(use-after-free)。
    // last_submitted_slot() 给的是 (frame_index_ + N - 1) % N,即真正的本帧槽。
    const std::uint32_t slot = sync_core_ ? sync_core_->last_submitted_slot() : 0u;
    mark_pages_in_flight(slot, stats.pages);
    // ---- 把这些 VkBuffer 的销毁托管给**本帧槽** ----
    //
    // 提交已经发出,调用方随时可能销毁这些缓冲(gtest 的 FrameRig 出作用域、
    // F-VX 的 slot->reset())。这里把它们的托管票据压进本帧槽的退休栈,
    // 该槽 fence 信号后才真正 vkDestroyBuffer。
    //
    // 只托管**本帧真的绑定给 GPU 的**缓冲(stats.buffers 里那些 VkBuffer),
    // 按句柄反查登记表:托管全表会让每帧把所有历史缓冲都拖住 N 帧。
    // 反查以**句柄相等**(kv.second->vk_buffer())为准 —— 表里存的是裸指针,
    // 而 VulkanBuffer 析构时会把自己从表里摘掉(见 ~VulkanBuffer →
    // VulkanRenderDevice::forget_known_buffer),所以表里不会有悬垂指针。
    if (sync_core_ && sync_core_->ready()) {
      for (const std::pair<resources::ResourceId, resources::IBuffer*>& kv : known_buffers_) {
        auto* vb = dynamic_cast<VulkanBuffer*>(kv.second);
        if (vb == nullptr) continue;
        const VkBuffer handle = vb->vk_buffer();
        if (handle == VK_NULL_HANDLE) continue;
        if (std::find(stats.buffers.begin(), stats.buffers.end(), handle) ==
            stats.buffers.end()) {
          continue;
        }
        auto ticket = vb->retire_ticket();
        if (ticket != nullptr) sync_core_->slot(slot).retire.push_shared_buffer(ticket);
      }
    }
  }
  // 立刻丢弃本列表在 begin_pass 期间顺带建的
  // VkCommandBuffer / framebuffer / render pass / image view:
  // submit_device_frame 用的是它自己的(槽位持久)命令缓冲,这一套从未被使用,
  // 却一直处于录制态并引用调用方 target 的 VkImage。留到析构就会在
  // image 已被调用方销毁后调 vkEndCommandBuffer → VUID-...-00059。
  // 返回值只用于决定是否继续,丢弃动作本身必须无条件执行。
  //
  // 【2026-11-11 / t23】不止是"留到析构会 00059",它还直接**吃掉一次绘制**。
  // 除命令缓冲外,旁路里还有一条**从未被执行**的真实 render pass:
  //   begin_pass 的 simple_shape 分支(vulkan_command.cpp)建了
  //   VkRenderPass(initialLayout=UNDEFINED, finalLayout=GENERAL)、
  //   framebuffer,并把 vkCmdBeginRenderPass 录进 vk_cmd_。
  //   而 submit_device_frame() 重放 recorded_ 时**另建**其自己的 pass。
  // 两个问题:
  //   1. attach 到同一张 VkImage 的布局链有两套互相矛盾的声明;
  //   2. 更要命的是 —— 一旦这条旁路 pass 建过,靶图在调用方眼里就已经
  //      "被某个 pass 占着",后续同一张图上的**第二次**绘制会静默不出片元。
  // 实测现象:UiAndSdkChannelsStayIndependent 里 SDK 通道在同一目标上
  // 退化成"整屏只剩清屏色"。所以这里改成 abandon_vk_side_channel(),
  // 把这些对象真正销毁并复位命令缓冲,而不只是丢句柄。
  if (!s.ok()) {
    cmd->abandon_vk_side_channel();
    return s;
  }
  // ---- 旁路 render pass 必须挡在 vkBeginCommandBuffer 之前丢掉 ----
  //
  // 【t23 实测】只做上面那次"收尾丢弃"是不够的:
  //   submit_device_frame() 内部先 core->begin_frame()(它会复位槽位命令缓冲,
  //   这一步就是 vkBeginCommandBuffer),**之后**才回放 recorded_ 建它自己的
  //   render pass。此时旁路的 VkRenderPass / VkFramebuffer 仍然活着,而
  //   framebuffer 里挂的就是调用方 target 的 VkImageView。
  //   在驱动看来,这张图同时被两个活动 pass 引用、布局链互相矛盾 ——
  //   结果是**这一次**绘制就静默不出片元(整屏只剩清屏色),而不是下一次。
  //
  // 所以顺序必须是:进入设备帧提交**之前**先丢掉旁路,提交之后再兜一次底。
  //
  // 为什么可以安全丢弃:别的地方要的 vk_cmd_(见 ICommandList::discard_vk_side_channel
  // 的注释)是给 submit_and_wait() 用的;而 submit_command_list() 这一路
  // 走的是 submit_device_frame() 的槽位命令缓冲,vk_cmd_ 全程死代码。
  // 命令流 recorded_ 是**值语义**独立副本,不受命令缓冲销毁影响。
  if (stats.draw_calls == 0) {
    return Status::Invalid("submit_command_list: 提交成功但没有任何 draw 被下发");
  }
  // 帧环不变量探针:本帧到底在提交**之前**丢弃过几次旁路。
  // 记在这里(成功路径的末尾),供 SyncCore.FrameRingAdvancesPerSubmit 在
  // 外部调用方跑完之后复核 —— 见 vulkan_render_device.h 的说明。
  last_frame_side_channel_abandons_ = cmd->side_channel_abandon_count();
  return Status::Ok_();
}

// ---------------------------------------------------------------- 工厂

Result<std::unique_ptr<IRenderDevice>> create_vulkan_device(const DeviceDesc& desc) {
  auto dev = std::make_unique<VulkanRenderDevice>();
  Status s = dev->init(desc, physical_device_override());
  if (!s.ok()) {
    return s;
  }
  // G8 / t13: publish the shaped-pipeline resolver to L1 exactly once.
  // Idempotent: register_pipeline_factory() just stores the function pointer.
  resources::register_pipeline_factory(&resolve_vulkan_pipeline_factory);
  return std::unique_ptr<IRenderDevice>(std::move(dev));
}

}  // namespace magicblocks::backend

