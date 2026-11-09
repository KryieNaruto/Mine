// MagicBlocks —— Vulkan 后端:设备帧提交器实现(L3 的"命令流真正出图"通路)
//
// 见 vulkan_device_frame.h 的背景说明。本文件的职责就一件事:
//   **把 `VulkanCommandList::recorded()` 里的 11 类命令逐条翻译成 vkCmd* 并提交。**
//
// 翻译表(spec R3 的 11 类,一条不多一条不少):
//   BindPipeline       → vkCmdBindPipeline(图形)
//   BindVertexBuffer   → vkCmdBindVertexBuffers(binding, 1, &vk_buffer, &offset)
//   BindIndexBuffer    → vkCmdBindIndexBuffer(vk_buffer, offset, VkIndexType)
//   Draw               → vkCmdDraw
//   DrawIndexed        → vkCmdDrawIndexed
//   DrawInstanced      → vkCmdDraw(interleaved 形式,first_instance)
//   PushConstants      → vkCmdPushConstants
//   SetViewport        → vkCmdSetViewport
//   SetScissor         → vkCmdSetScissor
//   BeginPass          → vkCmdBeginRenderPass(按目标形状建/复用 framebuffer)
//   EndPass            → vkCmdEndRenderPass
//
// 三层索引修正(与 L0 的"双翻行"教训同源,务必逐条对上):
//   1. 读回端(readback_rgba8)1:1 拷贝,不翻行 → 契约"行序自左上"。
//   2. 视口:Vulkan 的 +y 在视口里向下为正(与像素坐标原点左上一致);
//      **VkViewport::height 设为负** 会把 NDC 的 +y 映射到屏幕上方 ——
//      L0/L2 的着色器是 NDC 直出(不做 y 翻转),因此这里必须用**负高度视口**,
//      否则整个画面上下颠倒,而 readback 又不会翻回来。
//   3. 裁剪:VkRect2D 的 y 仍是像素坐标,y 必须与视口约定一致地取
//      `height - (rect.y + rect.height)` 作为偏移……但本引擎的 set_scissor 传入的
//      是"像素坐标原点左上",而 Vulkan 的 scissor 矩形 y 恒以**上边缘**计 ——
//      因此 scissor 不需要翻转,直接使用。
//
// 关于负高度视口的取舍(重要):GLSL 里 NDC +y 朝上,像素坐标 +y 朝下。
//   若 viewport.height > 0:NDC(+1) → 屏幕顶部?(不对,是底部)
//   Vulkan 规定:viewport 高度为正时,NDC y=-1 映射到 viewport.y,**y 轴向下增长**,
//   即 NDC(+1) 在上。这与"像素坐标原点左上"完全一致,因此
//   **高度取正即可,不需要负高度**。本文件在 RenderPass 之后显式 set_viewport
//   的原因仅是为了 cast 精度与"未显式设置时给默认值"的稳健性。
#pragma once

#include "backend/vulkan/vulkan_device_frame.h"

#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "backend/vulkan/vulkan_buffer.h"
#include "backend/vulkan/vulkan_command.h"
#include "backend/vulkan/vulkan_frame_target.h"
#include "backend/vulkan/vulkan_pipeline.h"
#include "backend/vulkan/vulkan_resource_common.h"
#include "backend/vulkan/vulkan_texture.h"

namespace magicblocks::backend {

using namespace magicblocks::device;

namespace {

Status fail_cmd(std::uint32_t index, const char* what, const std::string& why) {
  return Status::Invalid("submit_device_frame: 第 " + std::to_string(index) + " 条命令(" + what +
                         ")" + why);
}

VkAttachmentLoadOp load_op(bool clear) {
  return clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
}

VkIndexType to_vk_index(IndexFormat f) {
  return f == IndexFormat::UInt16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
}

// begin_pass 的附件形状(与 vulkan_pipeline.cpp 的 create_render_pass 同口径:
// color 恒 RGBA8;有深度附件时第二张附件挂 D16;MSAA 时再追加一张 resolve)。
struct PassShape {
  VkFormat color_format = VK_FORMAT_R8G8B8A8_UNORM;
  bool has_depth = false;
  VkFormat depth_format = VK_FORMAT_D16_UNORM;
  // MSAA 采样数。> 1 时 create_pass 会追加一张单采样 resolve 附件,
  // framebuffer 也必须相应多挂一张 resolve 视图(见 begin_pass 分支)。
  std::uint32_t samples = 1;
  // true = 颜色附件来自交换链图像。交换链图像 acquire 之后处于
  // PRESENT_SRC_KHR 布局，present 之前也必须回到 PRESENT_SRC_KHR，
  // 否则 vkQueuePresentKHR 会因为布局不符而丢帧（哪怕图已经画好了）。
  bool from_swapchain = false;
};

VkRenderPass create_pass(VkDevice device, const PassShape& shape, std::string* err) {
  // 附件排布: [0]=color, [1]=depth(有深度时), [2]=color resolve(MSAA 时)
  // MSAA 的 resolve 附件**必须是最后一个下标**,因为它总是在颜色/深度之后追加。
  VkAttachmentDescription att[3] = {};
  att[0].format = shape.color_format;
  att[0].samples = static_cast<VkSampleCountFlagBits>(shape.samples);
  att[0].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;  // begin 时按实际 clear 值覆盖
  att[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
  att[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
  att[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
  if (shape.from_swapchain) {
    // 交换链图像：从 PRESENT_SRC_KHR 来、回到 PRESENT_SRC_KHR 去。
    att[0].initialLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    att[0].finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
  } else {
    att[0].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    att[0].finalLayout = VK_IMAGE_LAYOUT_GENERAL;  // 读回的约定末态(见 vulkan_target.cpp)
  }

  std::uint32_t count = 1;
  if (shape.has_depth) {
    att[1].format = shape.depth_format;
    att[1].samples = static_cast<VkSampleCountFlagBits>(shape.samples);
    att[1].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    att[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att[1].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    att[1].finalLayout = VK_IMAGE_LAYOUT_GENERAL;
    count = 2;
  }

  // MSAA resolve 附件(断点 B)。
  //
  // 多重采样颜色图像渲染结束后,必须 resolve 成单采样图像才能读回;
  // 没有 pResolveAttachments 时读回拿到的是未定义内容(或第一个采样点)。
  // 位置固定在 count(即最后一个),与 vulkan_draw.cpp / vulkan_pipeline.cpp 一致。
  const bool resolve = shape.samples > 1;
  std::uint32_t resolve_index = 0;
  if (resolve) {
    resolve_index = count;
    att[resolve_index].format = shape.color_format;
    att[resolve_index].samples = VK_SAMPLE_COUNT_1_BIT;
    att[resolve_index].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;  // resolve 会覆盖
    att[resolve_index].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att[resolve_index].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att[resolve_index].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att[resolve_index].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    // 末态与单采样时颜色附件完全一致,保证读回路径不用分支。
    att[resolve_index].finalLayout = att[0].finalLayout;
    ++count;
  }

  VkAttachmentReference color_ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  VkAttachmentReference depth_ref{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
  VkAttachmentReference resolve_ref{resolve_index, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
  VkSubpassDescription sub{};
  sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
  sub.colorAttachmentCount = 1;
  sub.pColorAttachments = &color_ref;
  sub.pDepthStencilAttachment = shape.has_depth ? &depth_ref : nullptr;
  sub.pResolveAttachments = resolve ? &resolve_ref : nullptr;

  // subpass 依赖:没有它,loadOp=CLEAR 与随后的着色写入之间没有任何同步点。
  // Vulkan 规范里这是"隐性竞争",SwiftShader 上表现为整片被清掉(画不出东西),
  // 而且**不会报错** —— 与"render pass 不兼容"同属最难查的一类。
  VkSubpassDependency dep{};
  dep.srcSubpass = VK_SUBPASS_EXTERNAL;
  dep.dstSubpass = 0;
  dep.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  dep.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
  dep.srcAccessMask = 0;
  dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
  dep.dependencyFlags = 0;

  VkRenderPassCreateInfo ci{};
  ci.sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO;
  ci.attachmentCount = count;
  ci.pAttachments = att;
  ci.subpassCount = 1;
  ci.pSubpasses = &sub;
  ci.dependencyCount = 1;
  ci.pDependencies = &dep;
  VkRenderPass rp = VK_NULL_HANDLE;
  const VkResult r = vkCreateRenderPass(device, &ci, nullptr, &rp);
  if (r != VK_SUCCESS) {
    *err = fmt_err("submit_device_frame: vkCreateRenderPass", r);
    return VK_NULL_HANDLE;
  }
  return rp;
}

}  // namespace

Status submit_device_frame(const VulkanContext& ctx, const VulkanCommandList& list,
                           DeviceFrameStats* out_stats) {
  DeviceFrameStats stats{};
  if (ctx.device == VK_NULL_HANDLE || ctx.queue == VK_NULL_HANDLE) {
    return Status::InitFailed("submit_device_frame: 设备未初始化");
  }

  const std::vector<RecordedCommand>& cmds = list.recorded();
  if (cmds.empty()) {
    if (out_stats != nullptr) *out_stats = stats;
    return Status::Ok_();  // 空帧是合法的(与 L1 submit_and_wait 同口径)
  }

  VkCommandBufferAllocateInfo cai{};
  cai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
  cai.commandPool = ctx.cmd_pool;
  cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
  cai.commandBufferCount = 1;
  VkCommandBuffer cmd = VK_NULL_HANDLE;
  VkResult vr = vkAllocateCommandBuffers(ctx.device, &cai, &cmd);
  if (vr != VK_SUCCESS) return Status::InitFailed(fmt_err("submit_device_frame: vkAllocateCommandBuffers", vr));

  VkCommandBufferBeginInfo bi{};
  bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
  bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
  {
    // 原实现没有检查这个返回值。若它失败,之后所有 vkCmd* 都是空操作,
    // 而函数仍会一路走到 vkQueueSubmit 并返回 Ok —— 正是"清屏色对、几何全空"。
    const VkResult br = vkBeginCommandBuffer(cmd, &bi);
    if (br != VK_SUCCESS) {
      return Status::InitFailed(fmt_err("submit_device_frame: vkBeginCommandBuffer", br));
    }
  }

  // 每个 pass 一套资源:这些对象被命令缓冲引用,必须活到执行完成之后。
  struct PassResources {
    VkRenderPass rp = VK_NULL_HANDLE;
    VkFramebuffer fb = VK_NULL_HANDLE;
    // false = 这个 render pass 是**借来的**(复用管线的内建 pass),
    // 生命周期归 VulkanPipeline 所有,这里绝不能销毁。
    bool rp_owned = true;
    std::vector<VkImageView> views;
  };
  std::vector<PassResources> passes;
  auto cleanup = [&]() {
    for (PassResources& p : passes) {
      for (VkImageView v : p.views) {
        if (v != VK_NULL_HANDLE) vkDestroyImageView(ctx.device, v, nullptr);
      }
      if (p.fb != VK_NULL_HANDLE) vkDestroyFramebuffer(ctx.device, p.fb, nullptr);
      if (p.rp_owned && p.rp != VK_NULL_HANDLE) vkDestroyRenderPass(ctx.device, p.rp, nullptr);
    }
    passes.clear();
    vkFreeCommandBuffers(ctx.device, ctx.cmd_pool, 1, &cmd);
  };

  Status s = Status::Ok_();
  bool in_pass = false;
  float viewport_w = 0.0f;
  float viewport_h = 0.0f;

  for (std::size_t k = 0; k < cmds.size(); ++k) {
    const RecordedCommand& c = cmds[k];
    const std::uint32_t idx = static_cast<std::uint32_t>(k);
    switch (c.op) {
      case CommandOp::BeginPass: {
        if (in_pass) {
          s = fail_cmd(idx, "begin_pass", ": 上一个 pass 尚未 end_pass");
          break;
        }
        // pass_area 是 L1 在入流时解析过"0 = 整图"之后的值,可直接用作 renderArea。
        const std::uint32_t ax = c.pass_area[0];
        const std::uint32_t ay = c.pass_area[1];
        const std::uint32_t aw = c.pass_area[2];
        const std::uint32_t ah = c.pass_area[3];
        if (aw == 0 || ah == 0) {
          s = fail_cmd(idx, "begin_pass", ": render area 宽高为 0(该命令不是由 L1 录制的?)");
          break;
        }
        // 目标指针在 L1 的 last_pass_desc() 里只有最后一条;逐条回放需要按顺序
        // 重放,而 L1 只保留最后一条的 target。因此设备帧提交**要求**调用方
        // 为整帧指定同一个 target —— 这与 L3 渲染器的形状一致(一个 target 多 pass)。
        IRenderTarget* target = list.last_pass_desc().target;
        // 走 IFrameTargetImage 而不是 dynamic_cast<OffscreenTarget*>：
        // 交换链图像（pad 的主渲染路径）也实现了这个接口，离屏目标同样实现，
        // 两者对"提交一帧"完全等价，这里不需要任何平台/类型分支。
        auto* frame_img = dynamic_cast<IFrameTargetImage*>(target);
        if (frame_img == nullptr || frame_img->frame_color_image() == VK_NULL_HANDLE) {
          s = fail_cmd(idx, "begin_pass", ": 目标不提供可用的颜色附件图像");
          break;
        }
        const bool from_swapchain = frame_img->frame_from_swapchain();
        PassResources pr;
        PassShape shape;
        shape.color_format = vk_format_of(target->color_format());
        // 深度:只有当目标真的带深度附件时才挂。
        shape.has_depth = target->depth_attachment().valid;
        shape.depth_format = VK_FORMAT_D16_UNORM;
        shape.samples = target->samples() == 0 ? 1u : target->samples();
        shape.from_swapchain = from_swapchain;
        // 【关键一】render pass 必须与将要用它来画的管线**兼容**,否则 Vulkan 会
        // 静默丢弃 vkCmdDraw(不报错、不崩溃,只留一张干净的清屏色)。
        //
        // 【关键二】不能看"回放走到当前位置时绑过哪条管线"——录制顺序是
        // begin_pass → …… → bind_pipeline → draw → end_pass,begin_pass 时
        // 管线还没绑。所以这里**向后扫描**整帧命令流,取第一条 BindPipeline 的管线。
        VulkanPipeline* pass_pipeline = nullptr;
        for (std::size_t j = 0; j < cmds.size(); ++j) {
          if (cmds[j].op == CommandOp::BindPipeline) {
            pass_pipeline = dynamic_cast<VulkanPipeline*>(list.pipeline_at(j));
            break;
          }
        }
        std::string err;
        // 兼容性判断必须**逐项**比较附件形状：Vulkan 规定 render pass 的
        // attachment 格式/采样数/深度存在性都必须与 VkPipeline 创建时用的一致，
        // 否则 vkCmdDraw 被静默丢弃（不报错、只留清屏色）。
        // 早先只比了 has_depth —— 离屏 RGBA8 刚好能过，但交换链是 BGRA8，
        // 于是管线自带的 render pass 被误判为兼容，画面永远是黑的。
        bool pass_compatible = false;
        if (pass_pipeline != nullptr && pass_pipeline->vk_render_pass() != VK_NULL_HANDLE) {
          const AttachmentShape& ps = pass_pipeline->attachment_shape();
          // resolve 附件的"有无"由 samples > 1 决定,两侧必须取同一个口径。
          // 这里显式比一次,而不是靠下面 samples 相等"顺带"覆盖 —— 万一将来
          // 允许 samples>1 但关闭 resolve(某些实现会把颜色直接 resolve 到
          // 交换链),两者就不再等价。
          const bool ps_resolve = ps.samples > 1;
          const bool shape_resolve = shape.samples > 1;
          pass_compatible = ps.has_depth == shape.has_depth &&
                            ps.samples == shape.samples &&
                            ps_resolve == shape_resolve &&
                            ps.color_format == static_cast<Format>(target->color_format()) &&
                            vk_format_of(ps.color_format) == shape.color_format;
        }
        if (pass_compatible) {
          pr.rp = pass_pipeline->vk_render_pass();
          pr.rp_owned = false;
        } else {
          pr.rp = create_pass(ctx.device, shape, &err);
          if (pr.rp == VK_NULL_HANDLE) {
            s = Status::InitFailed(err);
            break;
          }
        }
        // 颜色视图
        VkImageViewCreateInfo vci{};
        vci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        vci.image = frame_img->frame_color_image();
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = shape.color_format;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        VkImageView cv = VK_NULL_HANDLE;
        vr = vkCreateImageView(ctx.device, &vci, nullptr, &cv);
        if (vr != VK_SUCCESS) {
          s = Status::InitFailed(fmt_err("submit_device_frame: vkCreateImageView(color)", vr));
          break;
        }
        pr.views.push_back(cv);
        if (shape.has_depth) {
          // 深度图像由 OffscreenTarget 持有;这里按同尺寸重建一张视图。
          // 注意:OffscreenTarget 不暴露 depth_image(),只用 depth_attachment() 判断
          // 存在性;因此这里**在没有深度图像时不允许挂深度附件**。
          // L3 的 pass 默认不请求深度(clear_depth=false 且管线深度状态关闭),
          // 需要深度时由 OffscreenTarget::with_depth 提供(见 vulkan_target.cpp)。
          VkImage dimg = frame_img->frame_depth_image();
          if (dimg != VK_NULL_HANDLE) {
            VkImageViewCreateInfo dci = vci;
            dci.image = dimg;
            dci.format = shape.depth_format;
            dci.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
            VkImageView dv = VK_NULL_HANDLE;
            vr = vkCreateImageView(ctx.device, &dci, nullptr, &dv);
            if (vr != VK_SUCCESS) {
              s = Status::InitFailed(fmt_err("submit_device_frame: vkCreateImageView(depth)", vr));
              break;
            }
            pr.views.push_back(dv);
          }
        }
        // MSAA resolve 视图。顺序必须与 create_pass 的附件下标严格一致:
        // [0]=color, [1]=depth(若有), 最后=resolve(若有)。
        if (shape.samples > 1) {
          VkImage rimg = frame_img->frame_resolve_image();
          if (rimg == VK_NULL_HANDLE) {
            s = Status::InitFailed(
                "submit_device_frame: 目标是 MSAA(samples=" + std::to_string(shape.samples) +
                ")但没有 resolve 图像");
            break;
          }
          VkImageViewCreateInfo rci = vci;
          rci.image = rimg;
          rci.format = shape.color_format;
          rci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
          VkImageView rv = VK_NULL_HANDLE;
          vr = vkCreateImageView(ctx.device, &rci, nullptr, &rv);
          if (vr != VK_SUCCESS) {
            s = Status::InitFailed(
                fmt_err("submit_device_frame: vkCreateImageView(resolve)", vr));
            break;
          }
          pr.views.push_back(rv);
        }
        VkFramebufferCreateInfo fci{};
        fci.sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO;
        fci.renderPass = pr.rp;
        fci.attachmentCount = static_cast<std::uint32_t>(pr.views.size());
        fci.pAttachments = pr.views.data();
        const Extent2D ext = target->extent();
        fci.width = ext.width;
        fci.height = ext.height;
        fci.layers = 1;
        vr = vkCreateFramebuffer(ctx.device, &fci, nullptr, &pr.fb);
        if (vr != VK_SUCCESS) {
          s = Status::InitFailed(fmt_err("submit_device_frame: vkCreateFramebuffer", vr));
          break;
        }
        passes.push_back(std::move(pr));

        VkClearValue clears[3] = {};
        // L1 的 begin_pass 编码(以 vulkan_command.cpp 的写入顺序**为准**):
        //   f[0..3] = clear_color_rgba
        //   f[4]    = clear_depth_value
        //   f[5]    = clear_color ? 1 : 0
        //   f[6]    = clear_depth ? 1 : 0
        // 早先这里按"f[0]=clear_color 标志、f[1..4]=rgba"读,与录制端完全错位:
        // 清屏色被读成 (g, b, a, clear_depth_value),表现为蓝通道恒 255 ——
        // 而那正是 L3 一条 draw 都画不出时唯一能看见的东西,极易被误判成
        // "格式不对/剔除过激"。两端必须逐字对齐。
        clears[0].color = {{c.f[0], c.f[1], c.f[2], c.f[3]}};
        clears[1].depthStencil = {c.f[4], 0};
        VkRenderPassBeginInfo rbi{};
        rbi.sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
        rbi.renderPass = passes.back().rp;
        rbi.framebuffer = passes.back().fb;
        rbi.renderArea = {{static_cast<int32_t>(ax), static_cast<int32_t>(ay)}, {aw, ah}};
        // clearValueCount 要覆盖到最大的附件下标。resolve 附件是 DONT_CARE,
        // 它的清屏值不被使用,但规范要求 pClearValues 至少有
        // max(attachment 下标 + 1) 个元素,所以 MSAA 时给 3。
        rbi.clearValueCount = shape.samples > 1 ? 3u : (shape.has_depth ? 2u : 1u);
        rbi.pClearValues = clears;
        vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
        in_pass = true;
        ++stats.pass_count;
        // 默认视口/裁剪覆盖整个 render area(L1 也会设,这里给兜底)。
        VkViewport dvp{static_cast<float>(ax), static_cast<float>(ay), static_cast<float>(aw),
                       static_cast<float>(ah), 0.0f, 1.0f};
        vkCmdSetViewport(cmd, 0, 1, &dvp);
        VkRect2D dsc{{static_cast<int32_t>(ax), static_cast<int32_t>(ay)}, {aw, ah}};
        vkCmdSetScissor(cmd, 0, 1, &dsc);
        viewport_w = static_cast<float>(aw);
        viewport_h = static_cast<float>(ah);
        break;
      }
      case CommandOp::EndPass: {
        if (!in_pass) {
          s = fail_cmd(idx, "end_pass", ": 当前不在 pass 内");
          break;
        }
        vkCmdEndRenderPass(cmd);
        in_pass = false;
        break;
      }
      case CommandOp::BindPipeline: {
        IPipeline* raw = list.pipeline_at(idx);
        auto* pipe = dynamic_cast<VulkanPipeline*>(raw);
        if (pipe == nullptr || !pipe->has_vk_pipeline()) {
          s = fail_cmd(idx, "bind_pipeline",
                       ": 管线不是本设备的 VulkanPipeline(是否用了纯软件态管线?)");
          break;
        }
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe->vk_pipeline());
        ++stats.pipeline_binds;
        break;
      }
      case CommandOp::BindVertexBuffer: {
        auto* buf = dynamic_cast<VulkanBuffer*>(list.buffer_at(c.resource));
        if (buf == nullptr || buf->vk_buffer() == VK_NULL_HANDLE) {
          s = fail_cmd(idx, "bind_vertex_buffer", ": 缓冲不是本设备的 VulkanBuffer");
          break;
        }
        // 64 位偏移以 off64 为准(见 RecordedCommand::off64 的注释)。
        const VkDeviceSize off = static_cast<VkDeviceSize>(c.off64);
        const VkBuffer vkb = buf->vk_buffer();
        vkCmdBindVertexBuffers(cmd, c.a, 1, &vkb, &off);
        break;
      }
      case CommandOp::BindIndexBuffer: {
        auto* buf = dynamic_cast<VulkanBuffer*>(list.buffer_at(c.resource));
        if (buf == nullptr || buf->vk_buffer() == VK_NULL_HANDLE) {
          s = fail_cmd(idx, "bind_index_buffer", ": 缓冲不是本设备的 VulkanBuffer");
          break;
        }
        vkCmdBindIndexBuffer(cmd, buf->vk_buffer(), static_cast<VkDeviceSize>(c.off64),
                             to_vk_index(static_cast<IndexFormat>(c.a)));
        break;
      }
      case CommandOp::Draw: {
        if (!in_pass) {
          s = fail_cmd(idx, "draw", ": 不在 pass 内");
          break;
        }
        vkCmdDraw(cmd, c.a, c.b, c.c, 0);
        ++stats.draw_calls;
        stats.instances += c.b;
        break;
      }
      case CommandOp::DrawIndexed: {
        if (!in_pass) {
          s = fail_cmd(idx, "draw_indexed", ": 不在 pass 内");
          break;
        }
        vkCmdDrawIndexed(cmd, c.a, c.b, c.c, c.i, 0);
        ++stats.draw_calls;
        stats.instances += c.b;
        stats.indices += c.a * c.b;
        break;
      }
      case CommandOp::DrawInstanced: {
        if (!in_pass) {
          s = fail_cmd(idx, "draw_instanced", ": 不在 pass 内");
          break;
        }
        // L1 的 draw_instanced 把 first_instance 存在 f[0](见 vulkan_command.cpp)。
        const std::uint32_t first_instance = static_cast<std::uint32_t>(c.f[0]);
        vkCmdDraw(cmd, c.a, c.b, c.c, first_instance);
        ++stats.draw_calls;
        stats.instances += c.b;
        break;
      }
      case CommandOp::PushConstants: {
        IPipeline* raw = list.bound_pipeline();
        auto* pipe = dynamic_cast<VulkanPipeline*>(raw);
        if (pipe == nullptr || pipe->vk_pipeline_layout() == VK_NULL_HANDLE) {
          s = fail_cmd(idx, "push_constants", ": 未绑定本设备的 VulkanPipeline");
          break;
        }
        if (c.payload.size() != c.c) {
          s = fail_cmd(idx, "push_constants", ": payload 长度与 size 不一致");
          break;
        }
        vkCmdPushConstants(cmd, pipe->vk_pipeline_layout(),
                           static_cast<VkShaderStageFlags>(c.a), c.b, c.c, c.payload.data());
        break;
      }
      case CommandOp::SetViewport: {
        // L1 的 set_viewport 编码:f[0]=x, f[1]=y, f[2]=w, f[3]=h, f[4]=minD, f[5]=maxD。
        VkViewport vp{c.f[0], c.f[1], c.f[2], c.f[3], c.f[4], c.f[5]};
        vkCmdSetViewport(cmd, 0, 1, &vp);
        viewport_w = c.f[2];
        viewport_h = c.f[3];
        break;
      }
      case CommandOp::SetScissor: {
        // L1 的 set_scissor 编码:a=x, b=y, width, height(像素坐标原点左上,
        // 与 VkRect2D 的上边缘语义一致,直接使用)。
        VkRect2D r{{static_cast<int32_t>(c.a), static_cast<int32_t>(c.b)}, {c.width, c.height}};
        vkCmdSetScissor(cmd, 0, 1, &r);
        break;
      }
    }
    if (!s.ok()) break;
  }

  if (s.ok()) {
    if (in_pass) s = Status::Invalid("submit_device_frame: 帧结束时仍有未 end_pass 的 pass");
    vkEndCommandBuffer(cmd);
  }

  if (s.ok()) {
    VkFenceCreateInfo fci{};
    fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
    VkFence fence = VK_NULL_HANDLE;
    vr = vkCreateFence(ctx.device, &fci, nullptr, &fence);
    if (vr != VK_SUCCESS) {
      s = Status::InitFailed(fmt_err("submit_device_frame: vkCreateFence", vr));
    } else {
      VkSubmitInfo si{};
      si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
      si.commandBufferCount = 1;
      si.pCommandBuffers = &cmd;
      vr = vkQueueSubmit(ctx.queue, 1, &si, fence);
      if (vr == VK_SUCCESS) vr = vkWaitForFences(ctx.device, 1, &fence, VK_TRUE, UINT64_MAX);
      vkDestroyFence(ctx.device, fence, nullptr);
      if (vr != VK_SUCCESS) s = Status::InitFailed(fmt_err("submit_device_frame: 提交", vr));
    }
  }

  cleanup();
  if (s.ok() && out_stats != nullptr) *out_stats = stats;
  return s;
}

}  // namespace magicblocks::backend
