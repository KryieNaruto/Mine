// MagicBlocks —— Vulkan 后端:离屏自检绘制
//
// L0 只做一件事:用 fullscreen.vert/frag 往离屏目标画 4x4 色块(实例化驱动,
// 不需要几何着色器 —— spec C4 硬禁)。这是 SDK 自检/CLI 出图的通路,
// **不含任何场景概念**;L2 会另起渲染图,本文件到时候退化为"测试绘制例程"。
#pragma once

#include <memory>

#include "backend/vulkan/vk_common.h"
#include "backend/vulkan/vulkan_target.h"

namespace magicblocks::backend {

// 用哪一套 shader 画这 4x4 色块。
//
// `L0Default` 是 fullscreen.vert/frag(纯红标定点 + 黑清屏),L0 golden 用它;
// `L2Scene` 是 l2_scene.vert/frag(纯绿标定点 + 深蓝清屏),L2 golden 用它。
// 两套输出必须**逐像素不同**,否则 golden_l0 与 golden_l2 会互相冒充
// (plans-l2 §6.5 的"两套 golden 不得互相通过")。
enum class SelftestShaders {
  L0Default,  // fullscreen.vert / fullscreen.frag
  L2Scene,    // l2_scene.vert / l2_scene.frag
};

// 把 4x4 色块画进 target。frame_index 参与像素相位,使 0/1 输出可区分。
device::Status draw_selftest_grid(const VulkanContext& ctx, OffscreenTarget* target,
                                  std::uint64_t frame_index,
                                  SelftestShaders shaders = SelftestShaders::L0Default);

}  // namespace magicblocks::backend
