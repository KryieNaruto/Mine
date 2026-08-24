#include "core/render/pipeline.h"

#include "core/render/vulkan_context.h"
#include "stroke_shaders.h"  // 构建期由 embed_shaders.py 生成

namespace easypainter::render {

Pipeline::~Pipeline() {
  if (device_ == VK_NULL_HANDLE) return;
  if (pipeline_ != VK_NULL_HANDLE) vkDestroyPipeline(device_, pipeline_, nullptr);
  if (layout_ != VK_NULL_HANDLE) vkDestroyPipelineLayout(device_, layout_, nullptr);
  if (render_pass_ != VK_NULL_HANDLE) vkDestroyRenderPass(device_, render_pass_, nullptr);
}

bool Pipeline::init(const VulkanContext& ctx) {
  device_ = ctx.device();

  VkAttachmentDescription color{};
  color.format = VK_FORMAT_R8G8B8A8_UNORM;
  color.samples = VK_SAMPLE_COUNT_1_BIT;
  color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
  color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
  color.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;

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
  if (vkCreateRenderPass(device_, &rp, nullptr, &render_pass_) != VK_SUCCESS) return false;

  VkPushConstantRange pc{};
  pc.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
  pc.offset = 0;
  pc.size = 16;  // 两个 vec2: uScale, uOffset

  VkPipelineLayoutCreateInfo pl{};
  pl.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
  pl.pushConstantRangeCount = 1;
  pl.pPushConstantRanges = &pc;
  if (vkCreatePipelineLayout(device_, &pl, nullptr, &layout_) != VK_SUCCESS) return false;

  VkShaderModuleCreateInfo sci{};
  sci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
  sci.codeSize = kStrokeVertSpv.size() * sizeof(uint32_t);
  sci.pCode = kStrokeVertSpv.data();
  VkShaderModule vs = VK_NULL_HANDLE;
  if (vkCreateShaderModule(device_, &sci, nullptr, &vs) != VK_SUCCESS) return false;
  sci.codeSize = kStrokeFragSpv.size() * sizeof(uint32_t);
  sci.pCode = kStrokeFragSpv.data();
  VkShaderModule fs = VK_NULL_HANDLE;
  if (vkCreateShaderModule(device_, &sci, nullptr, &fs) != VK_SUCCESS) {
    vkDestroyShaderModule(device_, vs, nullptr);
    return false;
  }

  VkPipelineShaderStageCreateInfo stages[2]{};
  stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
  stages[0].module = vs;
  stages[0].pName = "main";
  stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
  stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
  stages[1].module = fs;
  stages[1].pName = "main";

  VkVertexInputBindingDescription bind{};
  bind.binding = 0;
  bind.stride = sizeof(float) * 2;
  bind.inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
  VkVertexInputAttributeDescription attr{};
  attr.location = 0;
  attr.binding = 0;
  attr.format = VK_FORMAT_R32G32_SFLOAT;
  attr.offset = 0;

  VkPipelineVertexInputStateCreateInfo vi{};
  vi.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
  vi.vertexBindingDescriptionCount = 1;
  vi.pVertexBindingDescriptions = &bind;
  vi.vertexAttributeDescriptionCount = 1;
  vi.pVertexAttributeDescriptions = &attr;

  VkPipelineInputAssemblyStateCreateInfo ia{};
  ia.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
  ia.topology = VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;

  VkPipelineViewportStateCreateInfo vp{};
  vp.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
  vp.viewportCount = 1;
  vp.scissorCount = 1;

  VkPipelineRasterizationStateCreateInfo rs{};
  rs.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
  rs.polygonMode = VK_POLYGON_MODE_FILL;
  rs.cullMode = VK_CULL_MODE_NONE;
  rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
  rs.lineWidth = 1.0f;

  VkPipelineMultisampleStateCreateInfo ms{};
  ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
  ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

  VkPipelineColorBlendAttachmentState blend{};
  blend.blendEnable = VK_FALSE;
  blend.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
  VkPipelineColorBlendStateCreateInfo cb{};
  cb.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
  cb.attachmentCount = 1;
  cb.pAttachments = &blend;

  VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
  VkPipelineDynamicStateCreateInfo ds{};
  ds.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
  ds.dynamicStateCount = 2;
  ds.pDynamicStates = dyn;

  VkGraphicsPipelineCreateInfo gp{};
  gp.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
  gp.stageCount = 2;
  gp.pStages = stages;
  gp.pVertexInputState = &vi;
  gp.pInputAssemblyState = &ia;
  gp.pViewportState = &vp;
  gp.pRasterizationState = &rs;
  gp.pMultisampleState = &ms;
  gp.pColorBlendState = &cb;
  gp.pDynamicState = &ds;
  gp.layout = layout_;
  gp.renderPass = render_pass_;
  gp.subpass = 0;

  const VkResult r =
      vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &gp, nullptr, &pipeline_);
  vkDestroyShaderModule(device_, vs, nullptr);
  vkDestroyShaderModule(device_, fs, nullptr);
  return r == VK_SUCCESS;
}

void Pipeline::draw(VkCommandBuffer cmd, VkBuffer vertex_buffer, uint32_t vertex_count,
                    uint32_t w, uint32_t h, float scale, float ox, float oy) const {
  if (vertex_count == 0) return;

  VkViewport vp{0.f, 0.f, static_cast<float>(w), static_cast<float>(h), 0.f, 1.f};
  vkCmdSetViewport(cmd, 0, 1, &vp);
  VkRect2D sc{{0, 0}, {w, h}};
  vkCmdSetScissor(cmd, 0, 1, &sc);

  vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
  VkDeviceSize offset = 0;
  vkCmdBindVertexBuffers(cmd, 0, 1, &vertex_buffer, &offset);
  const float pc[4] = {scale, scale, ox, oy};  // uScale.xy = (scale,scale), uOffset.xy
  vkCmdPushConstants(cmd, layout_, VK_SHADER_STAGE_VERTEX_BIT, 0, 16, pc);
  vkCmdDraw(cmd, vertex_count, 1, 0, 0);
}

}  // namespace easypainter::render
