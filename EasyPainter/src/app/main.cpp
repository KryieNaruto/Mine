// easypainter:窗口端(GLFW + Vulkan swapchain + ImGui)。
// 拖拽鼠标采集笔画 → predictor 建模 → 离屏同款 pipeline 渲染 → ImGui 调参面板 → present。
#include <vulkan/vulkan.h>  // 先于 glfw3.h,使 glfwCreateWindowSurface 可见

#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>

#include "app/gui.h"
#include "backends/imgui_impl_glfw.h"
#include "backends/imgui_impl_vulkan.h"
#include "core/render/pipeline.h"
#include "core/render/vulkan_context.h"
#include "core/stroke/input_source.h"
#include "core/stroke/predictor.h"
#include "imgui.h"

using namespace easypainter;

namespace {

constexpr uint32_t kWindowW = 1024;
constexpr uint32_t kWindowH = 768;

struct SwapchainResources {
  VkSurfaceKHR surface = VK_NULL_HANDLE;
  VkSwapchainKHR swapchain = VK_NULL_HANDLE;
  VkFormat format = VK_FORMAT_UNDEFINED;
  VkExtent2D extent{};
  std::vector<VkImageView> views;
  VkRenderPass render_pass = VK_NULL_HANDLE;
  std::vector<VkFramebuffer> framebuffers;
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  VkSemaphore image_ready = VK_NULL_HANDLE;
  VkSemaphore render_done = VK_NULL_HANDLE;
  VkFence in_flight = VK_NULL_HANDLE;
};

bool CreateSwapchain(const render::VulkanContext& ctx, GLFWwindow* window,
                     SwapchainResources& sc) {
  const VkDevice dev = ctx.device();
  VkSurfaceCapabilitiesKHR caps;
  vkGetPhysicalDeviceSurfaceCapabilitiesKHR(ctx.physical_device(), sc.surface, &caps);

  uint32_t nf = 0;
  vkGetPhysicalDeviceSurfaceFormatsKHR(ctx.physical_device(), sc.surface, &nf, nullptr);
  std::vector<VkSurfaceFormatKHR> fmts(nf);
  vkGetPhysicalDeviceSurfaceFormatsKHR(ctx.physical_device(), sc.surface, &nf,
                                       fmts.data());
  VkSurfaceFormatKHR chosen = fmts.empty() ? VkSurfaceFormatKHR{VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR} : fmts[0];
  for (const auto& f : fmts) {
    if (f.format == VK_FORMAT_R8G8B8A8_UNORM ||
        f.format == VK_FORMAT_B8G8R8A8_UNORM) {
      chosen = f;
      break;
    }
  }
  sc.format = chosen.format;

  int w = 0, h = 0;
  glfwGetFramebufferSize(window, &w, &h);
  sc.extent = {static_cast<uint32_t>(std::max(w, 1)), static_cast<uint32_t>(std::max(h, 1))};
  sc.extent.width = std::clamp(sc.extent.width, caps.minImageExtent.width,
                               caps.maxImageExtent.width);
  sc.extent.height = std::clamp(sc.extent.height, caps.minImageExtent.height,
                                caps.maxImageExtent.height);

  uint32_t min_images = caps.minImageCount + 1;
  if (caps.maxImageCount > 0 && min_images > caps.maxImageCount)
    min_images = caps.maxImageCount;

  VkSwapchainCreateInfoKHR sci{};
  sci.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR;
  sci.surface = sc.surface;
  sci.minImageCount = min_images;
  sci.imageFormat = chosen.format;
  sci.imageColorSpace = chosen.colorSpace;
  sci.imageExtent = sc.extent;
  sci.imageArrayLayers = 1;
  sci.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
  sci.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
  sci.preTransform = caps.currentTransform;
  sci.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
  sci.presentMode = VK_PRESENT_MODE_FIFO_KHR;
  sci.clipped = VK_TRUE;
  if (vkCreateSwapchainKHR(dev, &sci, nullptr, &sc.swapchain) != VK_SUCCESS) {
    std::fprintf(stderr, "[app] vkCreateSwapchainKHR failed\n");
    return false;
  }

  uint32_t ni = 0;
  vkGetSwapchainImagesKHR(dev, sc.swapchain, &ni, nullptr);
  std::vector<VkImage> images(ni);
  vkGetSwapchainImagesKHR(dev, sc.swapchain, &ni, images.data());

  sc.views.resize(ni);
  for (uint32_t i = 0; i < ni; ++i) {
    VkImageViewCreateInfo iv{};
    iv.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    iv.image = images[i];
    iv.viewType = VK_IMAGE_VIEW_TYPE_2D;
    iv.format = chosen.format;
    iv.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    iv.subresourceRange.levelCount = 1;
    iv.subresourceRange.layerCount = 1;
    vkCreateImageView(dev, &iv, nullptr, &sc.views[i]);
  }

  // render pass(swapchain 格式,ImGui 与 stroke pipeline 共用兼容)
  VkAttachmentDescription color{};
  color.format = chosen.format;
  color.samples = VK_SAMPLE_COUNT_1_BIT;
  color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  color.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
  VkAttachmentReference ref{};
  ref.attachment = 0;
  ref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
  VkSubpassDescription sub{};
  sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  sub.colorAttachmentCount = 1;
  sub.pColorAttachments = &ref;
  VkSubpassDependency dep{};
  dep.srcSubpass = VK_SUBPASS_EXTERNAL;
  dep.dstSubpass = 0;
  dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  VkRenderPassCreateInfo rp{};
  rp.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
  rp.attachmentCount = 1;
  rp.pAttachments = &color;
  rp.subpassCount = 1;
  rp.pSubpasses = &sub;
  rp.dependencyCount = 1;
  rp.pDependencies = &dep;
  if (vkCreateRenderPass(dev, &rp, nullptr, &sc.render_pass) != VK_SUCCESS)
    return false;

  sc.framebuffers.resize(ni);
  for (uint32_t i = 0; i < ni; ++i) {
    VkFramebufferCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
    fci.renderPass = sc.render_pass;
    fci.attachmentCount = 1;
    fci.pAttachments = &sc.views[i];
    fci.width = sc.extent.width;
    fci.height = sc.extent.height;
    fci.layers = 1;
    vkCreateFramebuffer(dev, &fci, nullptr, &sc.framebuffers[i]);
  }

  VkCommandBufferAllocateInfo ca{};
  ca.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  ca.commandPool = ctx.command_pool();
  ca.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  ca.commandBufferCount = 1;
  vkAllocateCommandBuffers(dev, &ca, &sc.cmd);

  VkSemaphoreCreateInfo semi{};
  semi.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
  vkCreateSemaphore(dev, &semi, nullptr, &sc.image_ready);
  vkCreateSemaphore(dev, &semi, nullptr, &sc.render_done);
  VkFenceCreateInfo fe{};
  fe.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
  fe.flags = VK_FENCE_CREATE_SIGNALED_BIT;
  vkCreateFence(dev, &fe, nullptr, &sc.in_flight);
  return true;
}

void DestroySwapchain(const render::VulkanContext& ctx, SwapchainResources& sc) {
  const VkDevice dev = ctx.device();
  vkDeviceWaitIdle(dev);
  if (sc.in_flight) vkDestroyFence(dev, sc.in_flight, nullptr);
  if (sc.render_done) vkDestroySemaphore(dev, sc.render_done, nullptr);
  if (sc.image_ready) vkDestroySemaphore(dev, sc.image_ready, nullptr);
  for (auto f : sc.framebuffers) vkDestroyFramebuffer(dev, f, nullptr);
  if (sc.render_pass) vkDestroyRenderPass(dev, sc.render_pass, nullptr);
  for (auto v : sc.views) vkDestroyImageView(dev, v, nullptr);
  if (sc.swapchain) vkDestroySwapchainKHR(dev, sc.swapchain, nullptr);
  if (sc.surface) vkDestroySurfaceKHR(ctx.instance(), sc.surface, nullptr);
}

}  // namespace

int main(int argc, char** argv) {
  (void)argc;
  (void)argv;

  if (!glfwInit()) {
    std::fprintf(stderr, "[app] glfwInit failed\n");
    return 1;
  }
  glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
  GLFWwindow* window =
      glfwCreateWindow(kWindowW, kWindowH, "EasyPainter", nullptr, nullptr);
  if (!window) {
    std::fprintf(stderr, "[app] glfwCreateWindow failed\n");
    glfwTerminate();
    return 1;
  }

  uint32_t ext_count = 0;
  const char** glfw_exts = glfwGetRequiredInstanceExtensions(&ext_count);
  std::vector<const char*> instance_exts(glfw_exts, glfw_exts + ext_count);

  render::VulkanContext ctx;
  if (!ctx.init_instance(instance_exts)) return 1;

  SwapchainResources sc;
  if (glfwCreateWindowSurface(ctx.instance(), window, nullptr, &sc.surface) !=
      VK_SUCCESS) {
    std::fprintf(stderr, "[app] glfwCreateWindowSurface failed\n");
    return 1;
  }
  const std::vector<const char*> device_exts = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
  if (!ctx.init_device(sc.surface, device_exts)) return 1;
  if (!CreateSwapchain(ctx, window, sc)) return 1;

  // 笔画 pipeline(与 swapchain 格式一致;离屏同款 shader)
  render::Pipeline pipeline;
  if (!pipeline.init(ctx, sc.format)) return 1;

  // ImGui
  VkDescriptorPoolSize pool_sizes[] = {
      {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 8}};
  VkDescriptorPoolCreateInfo dpi{};
  dpi.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
  dpi.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
  dpi.maxSets = 4;
  dpi.poolSizeCount = 1;
  dpi.pPoolSizes = pool_sizes;
  VkDescriptorPool desc_pool = VK_NULL_HANDLE;
  vkCreateDescriptorPool(ctx.device(), &dpi, nullptr, &desc_pool);

  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImGui_ImplGlfw_InitForVulkan(window, true);
  ImGui_ImplVulkan_InitInfo vk_init{};
  vk_init.ApiVersion = VK_API_VERSION_1_0;
  vk_init.Instance = ctx.instance();
  vk_init.PhysicalDevice = ctx.physical_device();
  vk_init.Device = ctx.device();
  vk_init.QueueFamily = ctx.queue_family();
  vk_init.Queue = ctx.queue();
  vk_init.DescriptorPool = desc_pool;
  vk_init.MinImageCount = 2;
  vk_init.ImageCount = static_cast<uint32_t>(sc.views.size());
  vk_init.PipelineInfoMain.RenderPass = sc.render_pass;
  vk_init.PipelineInfoMain.Subpass = 0;
  vk_init.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
  ImGui_ImplVulkan_Init(&vk_init);  // 字体纹理由首次 NewFrame 自动上传

  // 应用状态
  stroke::PredictorConfig cfg;
  bool cfg_dirty = false;
  stroke::Predictor predictor(cfg);
  std::vector<stroke::Vec2> captured;
  std::vector<stroke::Vec2> modeled;
  bool drawing = false;
  float latency_ms = 0.f;
  auto last = std::chrono::steady_clock::now();

  while (!glfwWindowShouldClose(window)) {
    glfwPollEvents();

    // 鼠标采集
    if (glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS) {
      double mx = 0, my = 0;
      glfwGetCursorPos(window, &mx, &my);
      int w = 0, h = 0;
      glfwGetWindowSize(window, &w, &h);
      const float nx = static_cast<float>(mx) / static_cast<float>(std::max(w, 1));
      const float ny = static_cast<float>(my) / static_cast<float>(std::max(h, 1));
      if (!drawing) {
        captured.clear();
        drawing = true;
      }
      captured.push_back({std::clamp(nx, 0.f, 1.f), std::clamp(ny, 0.f, 1.f)});
    } else {
      drawing = false;
    }

    if (cfg_dirty) {
      predictor.set_config(cfg);  // 重建模型器应用新参数
      cfg_dirty = false;
    }

    // 建模(逐帧重算,简化实现;预测轨迹实时跟随)
    if (!captured.empty()) {
      const auto t0 = std::chrono::steady_clock::now();
      modeled = predictor.predict(stroke::build_events(captured));
      const auto t1 = std::chrono::steady_clock::now();
      latency_ms =
          std::chrono::duration<float, std::milli>(t1 - t0).count() /
          static_cast<float>(std::max<size_t>(1, captured.size()));
    }

    // 帧渲染
    uint32_t image_index = 0;
    vkWaitForFences(ctx.device(), 1, &sc.in_flight, VK_TRUE, UINT64_MAX);
    vkResetFences(ctx.device(), 1, &sc.in_flight);
    vkAcquireNextImageKHR(ctx.device(), sc.swapchain, UINT64_MAX, sc.image_ready,
                          VK_NULL_HANDLE, &image_index);

    vkResetCommandBuffer(sc.cmd, 0);
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    vkBeginCommandBuffer(sc.cmd, &bi);

    VkClearValue clear = {{{0.05f, 0.05f, 0.07f, 1.f}}};
    VkRenderPassBeginInfo rpb{};
    rpb.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rpb.renderPass = sc.render_pass;
    rpb.framebuffer = sc.framebuffers[image_index];
    rpb.renderArea = {{0, 0}, {sc.extent.width, sc.extent.height}};
    rpb.clearValueCount = 1;
    rpb.pClearValues = &clear;
    vkCmdBeginRenderPass(sc.cmd, &rpb, VK_SUBPASS_CONTENTS_INLINE);

    if (modeled.size() >= 2) {
      // 顶点 buffer(生命周期覆盖提交)
      VkBuffer vb = VK_NULL_HANDLE;
      VkDeviceMemory vb_mem = VK_NULL_HANDLE;
      const VkDeviceSize bytes =
          static_cast<VkDeviceSize>(modeled.size()) * sizeof(stroke::Vec2);
      VkBufferCreateInfo bci{};
      bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
      bci.size = bytes;
      bci.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT;
      bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
      vkCreateBuffer(ctx.device(), &bci, nullptr, &vb);
      VkMemoryRequirements mr;
      vkGetBufferMemoryRequirements(ctx.device(), vb, &mr);
      VkMemoryAllocateInfo ai{};
      ai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
      ai.allocationSize = mr.size;
      // 简化:取第一个 host 可见类型(窗口 demo 够用)
      VkPhysicalDeviceMemoryProperties mp;
      vkGetPhysicalDeviceMemoryProperties(ctx.physical_device(), &mp);
      for (uint32_t t = 0; t < mp.memoryTypeCount; ++t) {
        if ((mr.memoryTypeBits & (1u << t)) &&
            (mp.memoryTypes[t].propertyFlags &
             (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
              VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
                (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                 VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
          ai.memoryTypeIndex = t;
          break;
        }
      }
      vkAllocateMemory(ctx.device(), &ai, nullptr, &vb_mem);
      vkBindBufferMemory(ctx.device(), vb, vb_mem, 0);
      void* data = nullptr;
      vkMapMemory(ctx.device(), vb_mem, 0, bytes, 0, &data);
      std::memcpy(data, modeled.data(), static_cast<size_t>(bytes));
      vkUnmapMemory(ctx.device(), vb_mem);

      pipeline.draw(sc.cmd, vb, static_cast<uint32_t>(modeled.size()),
                    sc.extent.width, sc.extent.height);

      vkDestroyBuffer(ctx.device(), vb, nullptr);
      vkFreeMemory(ctx.device(), vb_mem, nullptr);
    }

    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplGlfw_NewFrame();
    ImGui::NewFrame();
    app::render_tuning_panel(cfg, cfg_dirty, latency_ms, &predictor);
    ImGui::Render();
    ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), sc.cmd);

    vkCmdEndRenderPass(sc.cmd);
    vkEndCommandBuffer(sc.cmd);

    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.waitSemaphoreCount = 1;
    si.pWaitSemaphores = &sc.image_ready;
    si.pWaitDstStageMask = &wait_stage;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &sc.cmd;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores = &sc.render_done;
    vkQueueSubmit(ctx.queue(), 1, &si, sc.in_flight);

    VkPresentInfoKHR pi{};
    pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores = &sc.render_done;
    pi.swapchainCount = 1;
    pi.pSwapchains = &sc.swapchain;
    pi.pImageIndices = &image_index;
    vkQueuePresentKHR(ctx.queue(), &pi);

    (void)last;
  }

  vkDeviceWaitIdle(ctx.device());
  ImGui_ImplVulkan_Shutdown();
  ImGui_ImplGlfw_Shutdown();
  ImGui::DestroyContext();
  vkDestroyDescriptorPool(ctx.device(), desc_pool, nullptr);
  DestroySwapchain(ctx, sc);
  glfwDestroyWindow(window);
  glfwTerminate();
  return 0;
}
