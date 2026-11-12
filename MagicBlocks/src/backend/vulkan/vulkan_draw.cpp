// MagicBlocks —— Vulkan 后端:离屏自检绘制实现
#include "backend/vulkan/vulkan_draw.h"

#include <array>
#include <cstring>
#include <vector>

#include "magicblocks_shaders.h"

#include "backend/vulkan/vulkan_resource_common.h"  // to_vk_samples(MSAA 共用的唯一一份)
#include "backend/vulkan/vulkan_sync_core.h"        // submit_and_wait_directed(定向等待本次提交)

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

  // 两套 shader 都是「6 顶点(两个三角形)unit quad + 16 实例」的形状,差别只在配色与清屏色。
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
  //
  // MSAA:采样数必须取自目标本身。这里曾经硬编码 VK_SAMPLE_COUNT_1_BIT,
  // 于是即使目标图像是 4 采样,render pass 仍按 1 采样描述 →
  // framebuffer 与 pass 的采样数不一致,完整校验层会报错、不报错时表现为
  // "根本没抗锯齿"。samples > 1 时再挂一张单采样 resolve 附件(见下)。
  const std::uint32_t samples = target->samples() == 0 ? 1u : target->samples();
  const VkSampleCountFlagBits vk_samples = to_vk_samples(samples);

  VkAttachmentDescription atts[2] = {};
  atts[0].format = to_vk_format(target->color_format());
  atts[0].samples = vk_samples;
  atts[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  atts[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  atts[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  atts[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  atts[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  atts[0].finalLayout = VK_IMAGE_LAYOUT_GENERAL;

  const bool resolve = (samples > 1);
  if (resolve) {
    // 多重采样颜色 → 单采样 resolve 目标。resolve 结果就是"读回看到的那张图"。
    atts[1].format = to_vk_format(target->color_format());
    atts[1].samples = VK_SAMPLE_COUNT_1_BIT;
    atts[1].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    atts[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    atts[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    atts[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    atts[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    atts[1].finalLayout = VK_IMAGE_LAYOUT_GENERAL;  // 与单采样路径一致,读回约定不变
  }

  VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  VkAttachmentReference resolve_ref{1, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  VkSubpassDescription sub{};
  sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  sub.colorAttachmentCount = 1;
  sub.pColorAttachments = &ref;
  sub.pResolveAttachments = resolve ? &resolve_ref : nullptr;

  VkRenderPassCreateInfo rpci{};
  rpci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
  rpci.attachmentCount = resolve ? 2u : 1u;
  rpci.pAttachments = atts;
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

  // MSAA 的 resolve 目标视图(单采样颜色图像)。samples == 1 时保持 VK_NULL_HANDLE,
  // 下面的 framebuffer 就还是"只有一个附件"的老样子(逐字节不变的前提)。
  VkImageView resolve_view = VK_NULL_HANDLE;
  if (resolve) {
    VkImageViewCreateInfo rci = vci;
    rci.image = target->resolve_image();
    if (rci.image == VK_NULL_HANDLE) {
      vkDestroyImageView(ctx.device, view, nullptr);
      vkDestroyRenderPass(ctx.device, rp, nullptr);
      vkDestroyShaderModule(ctx.device, vs, nullptr);
      vkDestroyShaderModule(ctx.device, fs, nullptr);
      return Status::InitFailed("draw: 目标 samples>1 但没有 resolve 图像(OffscreenTarget::init 未建)");
    }
    r = vkCreateImageView(ctx.device, &rci, nullptr, &resolve_view);
    if (r != VK_SUCCESS) {
      vkDestroyImageView(ctx.device, view, nullptr);
      vkDestroyRenderPass(ctx.device, rp, nullptr);
      vkDestroyShaderModule(ctx.device, vs, nullptr);
      vkDestroyShaderModule(ctx.device, fs, nullptr);
      return Status::InitFailed(fmt_err("vkCreateImageView(resolve)", r));
    }
  }

  const VkImageView fb_views[2] = {view, resolve_view};
  VkFramebufferCreateInfo fci{};
  fci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
  fci.renderPass = rp;
  fci.attachmentCount = resolve ? 2u : 1u;
  fci.pAttachments = fb_views;
  fci.width = ext.width;
  fci.height = ext.height;
  fci.layers = 1;
  VkFramebuffer fb = VK_NULL_HANDLE;
  r = vkCreateFramebuffer(ctx.device, &fci, nullptr, &fb);
  if (r != VK_SUCCESS) {
    if (resolve_view != VK_NULL_HANDLE) {
      vkDestroyImageView(ctx.device, resolve_view, nullptr);
    }
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
  // 拓扑必须与所用着色器的 gl_VertexIndex 约定一致:
  //   - fullscreen.vert:6 顶点 TRIANGLE_LIST 四边形(左下,右下,右上)+(左下,右上,左上)
  //   - l2_scene.vert  :同样的 6 顶点 TRIANGLE_LIST 排布
  // 这里曾硬编码 TRIANGLE_STRIP + 4 顶点 —— 对 fullscreen.vert 只会画出三角形
  // (0,1,2):四边形缺一半、实例 0 的标定点也落不进去,而且不报任何错。
  // 与 resources::PrimitiveType 的口径也一致:vulkan_pipeline.cpp 的
  // to_vk_topology 对非 LineList 一律给 TRIANGLE_LIST。
  ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
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
  // 断点 A(本任务的核心):这里曾经硬编码 VK_SAMPLE_COUNT_1_BIT。
  // 必须与 render pass 附件的采样数一致,否则 vkCmdDraw 被静默丢弃
  // (不报错,只留清屏色)或被校验层判为 framebuffer 不匹配。
  ms.rasterizationSamples = vk_samples;
  ms.sampleShadingEnable = VK_FALSE;  // 不做 per-sample shading(性能);MSAA 不需要它
  ms.minSampleShading = 0.0f;
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
    if (resolve_view != VK_NULL_HANDLE) vkDestroyImageView(ctx.device, resolve_view, nullptr);
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
    vkCmdDraw(cmd, 6, 16, 0, static_cast<std::uint32_t>(frame_index & 1u));  // 6 顶点 x 16 实例
    vkCmdEndRenderPass(cmd);
    vkEndCommandBuffer(cmd);

    VkSubmitInfo si{};
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    // 【2026-11-11】原来是 vkQueueSubmit(…, VK_NULL_HANDLE) + vkQueueWaitIdle:
    // 一个 queue 级全停。这里要的其实只是"这本函数返回前 GPU 已经画完" ——
    // 因为紧随其后就要销毁 pipeline / framebuffer / view,并且调用方会立刻读回。
    // 改成"提交并定向等待这一次提交"(槽位 fence 或临时 fence),不碰队列全局状态。
    Status sub = submit_and_wait_directed(ctx, cmd, 1'000'000'000ull);
    vkFreeCommandBuffers(ctx.device, ctx.cmd_pool, 1, &cmd);
    if (!sub.ok()) out = Status::InitFailed("draw 提交: " + sub.message);
  }

  if (pipe) vkDestroyPipeline(ctx.device, pipe, nullptr);
  vkDestroyPipelineLayout(ctx.device, layout, nullptr);
  vkDestroyFramebuffer(ctx.device, fb, nullptr);
  if (resolve_view != VK_NULL_HANDLE) vkDestroyImageView(ctx.device, resolve_view, nullptr);
  vkDestroyImageView(ctx.device, view, nullptr);
  vkDestroyRenderPass(ctx.device, rp, nullptr);
  vkDestroyShaderModule(ctx.device, vs, nullptr);
  vkDestroyShaderModule(ctx.device, fs, nullptr);
  return out;
}

}  // namespace magicblocks::backend
