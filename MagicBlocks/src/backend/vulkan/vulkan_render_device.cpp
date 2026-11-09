// MagicBlocks —— Vulkan 后端:设备实现 + 工厂
#include "backend/vulkan/vulkan_render_device.h"

#include <cstring>

#include "backend/vulkan/vulkan_draw.h"
#include "backend/vulkan/vulkan_buffer.h"
#include "backend/vulkan/vulkan_command.h"
#include "backend/vulkan/vulkan_device_frame.h"
#include "backend/vulkan/vulkan_pipeline.h"
#include "backend/vulkan/vulkan_swapchain.h"
#include "backend/vulkan/vulkan_texture.h"

namespace magicblocks::backend {

using namespace magicblocks::device;

namespace {
std::string g_device_override;
}

void set_physical_device_override(const std::string& name) { g_device_override = name; }
const std::string& physical_device_override() { return g_device_override; }

VulkanRenderDevice::~VulkanRenderDevice() {
  if (ctx_.device) vkDeviceWaitIdle(ctx_.device);
  // 顺序关键:fence 必须在 VkDevice 之前销毁;
  // 分配器要在 VkDevice 之前销毁(VMA 持有 VkDevice),但晚于所有资源。
  frame_sync_.reset();
  if (ctx_.cmd_pool) vkDestroyCommandPool(ctx_.device, ctx_.cmd_pool, nullptr);
  allocator_.reset();
  ctx_.allocator = nullptr;
  if (ctx_.device) vkDestroyDevice(ctx_.device, nullptr);
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

  VkPhysicalDeviceFeatures feats{};
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

  // 帧同步:in_flight 已校验
  frame_sync_ = std::make_unique<VulkanFrameSync>(ctx_.device, desc.in_flight_frames);
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

Status VulkanRenderDevice::notify_lifecycle(const AppLifecycleEvent& ev) {
  (void)ev;
  return Status::Ok_();
}

Status VulkanRenderDevice::wait_idle() {
  if (!ctx_.device) return Status::Ok_();
  VkResult r = vkDeviceWaitIdle(ctx_.device);
  if (r != VK_SUCCESS) return Status::DeviceLost(fmt_err("vkDeviceWaitIdle", r));
  return Status::Ok_();
}

// ---------------------------------------------------------------- L1 资源层工厂

Result<std::unique_ptr<resources::IBuffer>> VulkanRenderDevice::create_buffer(
    const resources::BufferDesc& desc) {
  if (!logical_ready_ || allocator_ == nullptr) return Status::InitFailed("设备未初始化");
  auto b = std::make_unique<VulkanBuffer>(ctx_, allocator_.get(), desc);
  Status s = b->init();
  if (!s.ok()) return s;
  // 登记句柄 → 指针,供 submit_command_list() 反查(见 bind_known_buffers)。
  if (b->id().valid()) known_buffers_.emplace_back(b->id(), b.get());
  return std::unique_ptr<resources::IBuffer>(std::move(b));
}

Result<std::unique_ptr<resources::ITexture>> VulkanRenderDevice::create_texture(
    const resources::TextureDesc& desc) {
  if (!logical_ready_ || allocator_ == nullptr) return Status::InitFailed("设备未初始化");
  auto t = std::make_unique<VulkanTexture>(ctx_, allocator_.get(), desc);
  Status s = t->init();
  if (!s.ok()) return s;
  return std::unique_ptr<resources::ITexture>(std::move(t));
}

Result<std::unique_ptr<resources::ISampler>> VulkanRenderDevice::create_sampler(
    const resources::SamplerDesc& desc) {
  if (!logical_ready_) return Status::InitFailed("设备未初始化");
  auto s = std::make_unique<VulkanSampler>(ctx_, desc);
  Status st = s->init();
  if (!st.ok()) return st;
  return std::unique_ptr<resources::ISampler>(std::move(s));
}

Result<std::unique_ptr<resources::IPipeline>> VulkanRenderDevice::create_pipeline(
    const resources::PipelineDesc& desc) {
  if (!logical_ready_) return Status::InitFailed("设备未初始化");
  auto p = std::make_unique<VulkanPipeline>(ctx_, desc);
  Status s = p->init();
  if (!s.ok()) return s;
  return std::unique_ptr<resources::IPipeline>(std::move(p));
}

Result<std::unique_ptr<resources::IPipeline>> VulkanRenderDevice::create_pipeline_for_shape(
    const resources::PipelineDesc& desc, const AttachmentShape& shape) {
  if (!logical_ready_) return Status::InitFailed("设备未初始化");
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
  return std::unique_ptr<resources::ICommandList>(std::move(c));
}

// ---------------------------------------------------------------- 设备帧提交

// 命令列表里只有 ResourceId,设备帧提交需要 IBuffer*。这里按设备自己登记过的
// 句柄表把指针补进命令列表(命令列表内部维护 ResourceId → IBuffer* 的反查表)。
void VulkanRenderDevice::bind_known_buffers(VulkanCommandList& list) const {
  for (const auto& kv : known_buffers_) {
    (void)list.remember_buffer_binding(kv.first, kv.second);
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

  // 走已经实现完整的设备帧提交通路(11 类命令 → vkCmd* → fence 等待)。
  // 统计只用于本地断言:录了命令却一个 draw 都没下发属于配置错误,要报出来,
  // 而不是返回成功让上层去翻一张空图。
  DeviceFrameStats stats{};
  Status s = submit_device_frame(ctx_, *cmd, &stats);
  if (!s.ok()) return s;
  if (stats.draw_calls == 0) {
    return Status::Invalid("submit_command_list: 提交成功但没有任何 draw 被下发");
  }
  return Status::Ok_();
}

// ---------------------------------------------------------------- 工厂

Result<std::unique_ptr<IRenderDevice>> create_vulkan_device(const DeviceDesc& desc) {
  auto dev = std::make_unique<VulkanRenderDevice>();
  Status s = dev->init(desc, physical_device_override());
  if (!s.ok()) {
    return s;
  }
  return std::unique_ptr<IRenderDevice>(std::move(dev));
}

}  // namespace magicblocks::backend
