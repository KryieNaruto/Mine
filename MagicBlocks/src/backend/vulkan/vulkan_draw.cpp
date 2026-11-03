// MagicBlocks —— Vulkan 后端:离屏自检绘制实现
#include "backend/vulkan/vulkan_draw.h"

#include <array>
#include <cstring>
#include <vector>

#include "magicblocks_shaders.h"

namespace magicblocks::backend {

using namespace magicblocks::device;

namespace {

VkShaderModule make_module(VkDevice device, const std::uint32_t* code, std::size_t words) {
  VkShaderModuleCreateInfo ci{};
  ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  ci.codeSize = words * sizeof(std::uint32_t);
  ci.pCode = code;
  VkShaderModule m = VK_NULL_HANDLE;
  if (vkCreateShaderModule(device, &ci, nullptr, &m) != VK_SUCCESS) return VK_NULL_HANDLE;
  return m;
}

}  // namespace

Status draw_selftest_grid(const VulkanContext& ctx, OffscreenTarget* target, u64 frame_index,
                          SelftestShaders shaders_kind) {
  if (!target) return Status::Invalid("draw: target 为空");
  VkImage color = target->color_image();
  if (!color) return Status::Unsupported("draw: 目标无颜色附件");

  // 两套 shader 都是「4 顶点 unit quad + 16 实例」的形状,差别只在配色与清屏色。
  const bool l2 = (shaders_kind == SelftestShaders::L2Scene);
  const std::uint32_t* vs_code = l2 ? shaders::kL2_sceneVert : shaders::kFullscreenVert;
  const std::size_t vs_bytes = l2 ? shaders::kL2_sceneVertSize : shaders::kFullscreenVertSize;
  const std::uint32_t* fs_code = l2 ? shaders::kL2_sceneFrag : shaders::kFullscreenFrag;
  const std::size_t fs_bytes = l2 ? shaders::kL2_sceneFragSize : shaders::kFullscreenFragSize;

  VkShaderModule vs = make_module(ctx.device, vs_code, vs_bytes / sizeof(std::uint32_t));
  VkShaderModule fs = make_module(ctx.device, fs_code, fs_bytes / sizeof(std::uint32_t));
  if (!vs || !fs) {
    if (vs) vkDestroyShaderModule(ctx.device, vs, nullptr);
    if (fs) vkDestroyShaderModule(ctx.device, fs, nullptr);
    return Status::InitFailed("draw: 创建 shader module 失败");
  }

  // 渲染过程:颜色附件初值 CLEAR(黑色),末态 GENERAL 以便读回
  VkAttachmentDescription att{};
  att.format = to_vk_format(target->color_format());
  att.samples = VK_SAMPLE_COUNT_1_BIT;
  att.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  att.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  att.finalLayout = VK_IMAGE_LAYOUT_GENERAL;

  VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  VkSubpassDescription sub{};
  sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  sub.colorAttachmentCount = 1;
  sub.pColorAttachments = &ref;

  VkRenderPassCreateInfo rpci{};
  rpci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
  rpci.attachmentCount = 1;
  rpci.pAttachments = &att;
  rpci.subpassCount = 1;
  rpci.pSubpasses = &sub;
  VkRenderPass rp = VK_NULL_HANDLE;
  VkResult r = vkCreateRenderPass(ctx.device, &rpci, nullptr, &rp);
  if (r != VK_SUCCESS) {
    vkDestroyShaderModule(ctx.device, vs, nullptr);
    vkDestroyShaderModule(ctx.device, fs, nullptr);
    return Status::InitFailed(fmt_err("vkCreateRenderPass", r));
  }

  VkImageViewCreateInfo vci{};
  vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
  vci.image = color;
  vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
  vci.format = to_vk_format(target->color_format());
  vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
  VkImageView view = VK_NULL_HANDLE;
  r = vkCreateImageView(ctx.device, &vci, nullptr, &view);
  if (r != VK_SUCCESS) {
    vkDestroyRenderPass(ctx.device, rp, nullptr);
    vkDestroyShaderModule(ctx.device, vs, nullptr);
    vkDestroyShaderModule(ctx.device, fs, nullptr);
    return Status::InitFailed(fmt_err("vkCreateImageView", r));
  }

  const Extent2D ext = target->extent();
  VkFramebufferCreateInfo fci{};
  fci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
  fci.renderPass = rp;
  fci.attachmentCount = 1;
  fci.pAttachments = &view;
  fci.width = ext.width;
  fci.height = ext.height;
  fci.layers = 1;
  VkFramebuffer fb = VK_NULL_HANDLE;
  r = vkCreateFramebuffer(ctx.device, &fci, nullptr, &fb);
  if (r != VK_SUCCESS) {
    vkDestroyImageView(ctx.device, view, nullptr);
    vkDestroyRenderPass(ctx.device, rp, nullptr);
    vkDestroyShaderModule(ctx.device, vs, nullptr);
    vkDestroyShaderModule(ctx.device, fs, nullptr);
    return Status::InitFailed(fmt_err("vkCreateFramebuffer", r));
  }

  // 管线:无顶点缓冲,4x4 色块靠 gl_InstanceIndex 驱动(不写"场景")
  VkPipelineShaderStageCreateInfo stages[2] = {};
  stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = vs;
  stages[0].pName = "main";
  stages[1] = stages[0];
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = fs;

  VkPipelineVertexInputStateCreateInfo vi{};
  vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
  VkPipelineInputAssemblyStateCreateInfo ia{};
  ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
  ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
  VkViewport vp{0.f, 0.f, static_cast<float>(ext.width), static_cast<float>(ext.height), 0.f, 1.f};
  VkRect2D sc{{0, 0}, {ext.width, ext.height}};
  VkPipelineViewportStateCreateInfo vps{};
  vps.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
  vps.viewportCount = 1;
  vps.pViewports = &vp;
  vps.scissorCount = 1;
  vps.pScissors = &sc;
  VkPipelineRasterizationStateCreateInfo rs{};
  rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
  rs.polygonMode = VK_POLYGON_MODE_FILL;
  rs.cullMode = VK_CULL_MODE_NONE;
  rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  rs.lineWidth = 1.f;
  VkPipelineMultisampleStateCreateInfo ms{};
  ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
  ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
  VkPipelineColorBlendAttachmentState cba{};
  cba.colorWriteMask = 0xF;
  cba.blendEnable = VK_FALSE;
  VkPipelineColorBlendStateCreateInfo cb{};
  cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
  cb.attachmentCount = 1;
  cb.pAttachments = &cba;
  VkPipelineLayoutCreateInfo plci{};
  plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  VkPipelineLayout layout = VK_NULL_HANDLE;
  r = vkCreatePipelineLayout(ctx.device, &plci, nullptr, &layout);
  if (r != VK_SUCCESS) {
    vkDestroyFramebuffer(ctx.device, fb, nullptr);
    vkDestroyImageView(ctx.device, view, nullptr);
    vkDestroyRenderPass(ctx.device, rp, nullptr);
    vkDestroyShaderModule(ctx.device, vs, nullptr);
    vkDestroyShaderModule(ctx.device, fs, nullptr);
    return Status::InitFailed(fmt_err("vkCreatePipelineLayout", r));
  }

  VkGraphicsPipelineCreateInfo pci{};
  pci.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  pci.stageCount = 2;
  pci.pStages = stages;
  pci.pVertexInputState = &vi;
  pci.pInputAssemblyState = &ia;
  pci.pViewportState = &vps;
  pci.pRasterizationState = &rs;
  pci.pMultisampleState = &ms;
  pci.pColorBlendState = &cb;
  pci.layout = layout;
  pci.renderPass = rp;
  VkPipeline pipe = VK_NULL_HANDLE;
  r = vkCreateGraphicsPipelines(ctx.device, VK_NULL_HANDLE, 1, &pci, nullptr, &pipe);

  Status out = Status::Ok_();
  if (r != VK_SUCCESS) {
    out = Status::InitFailed(fmt_err("vkCreateGraphicsPipelines", r));
  } else {
    // 记录帧:清屏 → 重建 Viewport → 16 个实例一次画完
    VkCommandBufferAllocateInfo cai{};
    cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
    cai.commandPool = ctx.cmd_pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VkCommandBuffer cmd = VK_NULL_HANDLE;
    vkAllocateCommandBuffers(ctx.device, &cai, &cmd);
    VkCommandBufferBeginInfo bi{};
    bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &bi);
    VkClearValue clear{};
    if (l2) {
      // L2:深蓝清屏(0.05/0.08/0.20),与 L0 的纯黑逐像素可区分。
      clear.color = {{0.05f, 0.08f, 0.20f, 1.f}};
    } else {
      // L0:纯黑清屏。
      clear.color = {{0.f, 0.f, 0.f, 1.f}};
    }
    VkRenderPassBeginInfo rbi{};
    rbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rbi.renderPass = rp;
    rbi.framebuffer = fb;
    rbi.renderArea = sc;
    rbi.clearValueCount = 1;
    rbi.pClearValues = &clear;
    vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
    vkCmdDraw(cmd, 4, 16, 0, static_cast<std::uint32_t>(frame_index & 1u));  // 4 顶点 x 16 实例
    vkCmdEndRenderPass(cmd);
    vkEndCommandBuffer(cmd);

    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    r = vkQueueSubmit(ctx.queue, 1, &si, VK_NULL_HANDLE);
    if (r == VK_SUCCESS) r = vkQueueWaitIdle(ctx.queue);
    vkFreeCommandBuffers(ctx.device, ctx.cmd_pool, 1, &cmd);
    if (r != VK_SUCCESS) out = Status::InitFailed(fmt_err("draw 提交", r));
  }

  if (pipe) vkDestroyPipeline(ctx.device, pipe, nullptr);
  vkDestroyPipelineLayout(ctx.device, layout, nullptr);
  vkDestroyFramebuffer(ctx.device, fb, nullptr);
  vkDestroyImageView(ctx.device, view, nullptr);
  vkDestroyRenderPass(ctx.device, rp, nullptr);
  vkDestroyShaderModule(ctx.device, vs, nullptr);
  vkDestroyShaderModule(ctx.device, fs, nullptr);
  return out;
}

}  // namespace magicblocks::backend
