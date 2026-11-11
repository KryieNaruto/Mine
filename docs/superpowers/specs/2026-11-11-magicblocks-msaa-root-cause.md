# MagicBlocks「MSAA 没有效果」—— L1 离屏 / L3 管线 / L0 一帧路径全链路根因定位

日期：2026-11-11
类型：**P0 阻断 · 只定位不改代码**（修法由后续任务决定）
定位人：msaa（agent-teams `magicblocks-engine-features` 任务 t2）
结论口径：每一条假设都有「代码行号证据」+「本机复现命令/实测输出」+「判定：成立 / 证伪」。

---

## 0. 一页结论（TL;DR）

**用户说的「没有效果」，不是 resolve 链路坏了，而是用户那条路径上根本没有 MSAA 入口、
或者入口开了却什么都没变。**

| 优先级 | 结论 | 证据 |
|---|---|---|
| **P0-1** | **UI 消费层（用户唯一能看到的界面路径）完全没有 MSAA 入口**：离屏 UI 合成由 CPU 软件光栅化器逐像素写出（注释原文「光栅化刻意**不加抗锯齿**、坐标四舍五入到整数像素」），GPU 侧 UI pass 硬编码 `rasterizationSamples = 1`；`UiAppConfig`/`Hud`/`Overlay2D` 公共面没有任何 samples/msaa 字段。 | `src/ui/overlay_renderer.cpp:17`、`src/backend/vulkan/vulkan_ui_overlay.cpp:141,213-215`、`src/ui_cli/main.cpp:144-149`、`include/magicblocks/ui/perf.h:112`（只有一句 `ui_msaa_enabled()`，无消费者） |
| **P0-2** | **默认路径全员 1 采样**：l3-cli 五个子命令、ui-cli、l0-cli、cube-demo、Android pad 真实入口都不带 `--msaa`/`set_requested_msaa`。用户「默认用」的那条路 MSAA 就是关的。 | `src/l3_cli/main.cpp:84`、`src/ui_cli/main.cpp:144-149`、`src/cli/main.cpp`（无 msaa 参数）、`src/cube_demo/main.cpp`、`src/android/scene_renderer.cpp:101-102`（`msaa_samples_ = 1`） |
| **P0-3** | **Android 真机路径：管线绑的是 L3 `create_pipeline(desc)` 的 pass，而该 pass 的 `has_depth` 恒为 false**，与目标声明「有深度」→ 走 `create_pass()` 回退分支；即便把回退分支修成「与目标同形」，交换链的 `pDepthStencilAttachment` 又会指向一张不存在的附件 `[1]`。**这条链上 MSAA 的 resolve 附件永远接不到 framebuffer。** | `vulkan_render_device.cpp:285-292` → `vulkan_pipeline.cpp:531`、`vulkan_pipeline.h:56-61`；`android/scene_renderer.cpp:299-305` |
| **P1-4** | **`PassShapeCore`/`build_attachment_descriptions()`/`pass_clear_value_count()` 在代码里不存在**，2026-11-09 任务书声称的「两份附件描述共用一份实现」从未落地 → `create_pass()` 与 `create_render_pass()` 仍然是两份手写、口径不一。这是 P0-3 能长期潜伏的土壤。 | 全仓 grep 零命中（见 §3.4） |
| **P1-5** | L3 图形管线路径（l3-cli / F-SK / F-VX）**目标带深度、管线不带深度** → 设备帧提交永远不看目标的深度附件，`clear_depth` 被静默忽略。与 MSAA 无关但同属「附件形状漂移」。 | `vulkan_pipeline.cpp:573-585`、`vulkan_device_frame.cpp:278-280` |
| **P2-6** | `--msaa` 只对**离屏目标**的采样数生效；`PipelineDesc.samples` 只用于 `rasterizationSamples`，**不参与 render pass 兼容性判定**。今天能出效果纯属「目标有深度 ⇒ 永远走 `create_pass()` 回退分支」的巧合耦合。 | `vulkan_device_frame.cpp:281,302-316` |
| **P3-7** | caps 报 `{1,4}` 是**正确**的（全局限制 ∩ 按格式查询，与 `vkGetPhysicalDeviceImageFormatProperties` 实测一致）。顺带勘误任务书的归因：`to_vk_samples` **不做任何校验**（64 走 default → 1x），真正拦住 64 的是 `OffscreenTarget::init()` 的数值反推比较 `vulkan_target.cpp:131-134` + `color_format_supports_samples()`。 | `vulkan_device.cpp:98-134`、`vulkan_resource_common.cpp:166-196`、`vulkan_target.cpp:122-144` |
| **P1-8** | **第二类管线入口同样丢形状**：`ShaderLibrary::acquire_pipeline(key, raster, shape)` 把 `TargetShape` 算进缓存键却只调 `create_pipeline(desc)`，`shape` 没进后端（`shader_library.cpp:186-187`）；Android 唯一在用的 `create_pipeline_for_shape` 又把 `has_depth=false` 写死（`android/scene_renderer.cpp:299-305`）。 | 见左 |
| **附录** | `vkQueuePresentKHR` 的 `UNDEFINED` 布局校验错误 = **测试自己从不向交换链图渲染**（基线 skip），与 MSAA 无关；`clearValueCount 3 > 2` / Framebuffer 采样数不匹配只在旧二进制里出现，需全量重建后复测（§6）。两者都不该被当成「MSAA 无效」。 | 见 §6 |

> **一句话给修复任务**：先把「入口」补齐（UI 层 + 默认路径 + 形状传递），再谈 resolve 正确性；
> 只修 backend 的 resolve 附件，用户仍然会说「没效果」。

---

## 1. 被测基线（本次定位的实测环境）

```powershell
cd MagicBlocks\build
.\magicblocks-l0-cli.exe device
```

```
"device_name": "SwiftShader Device (LLVM 10.0.0)",
"supported_msaa_levels": [1, 4],
"depth_format_count": 2,
"texture_format_count": 3
```

全量测试（**0 失败，但 2 条 SKIP**，见 §7）：

```powershell
cmd /c ".\magicblocks_test.exe > tmp\test_out.txt 2>&1"   # exit=0
# [==========] 474 tests from 73 test suites ran.
# [  PASSED  ] 473 tests.
# [  SKIPPED ] 2 tests
#   Msaa.RejectsUnsupportedLevel               (该设备恰好支持 64 采样)
#   MsaaSwapchain.CurrentTargetFollowsAcquiredImage (索引恒 0，见 §6.1)
```

> ⚠️ captain 的预检说「唯一跳过是 `Msaa.RejectsUnsupportedLevel`」，**实际有 2 条**。
> 第二条 `MsaaSwapchain.CurrentTargetFollowsAcquiredImage` 的 skip 原因就是 §6.1 那条
> `vkQueuePresentKHR` 布局错误 —— 这两件事在预检里被混在一起了。

---

## 2. 链路地图（谁把 samples 传给谁）

```
[用户入口]
  l3-cli stack/sketch  --msaa N ─┐
  l3-cli paint/blocks/gizmo --msaa N ─┤
  ui-cli render/stats             ─┼─► 没有 --msaa 参数（P0-1/P0-2）
  l0-cli render                   ─┤
  cube-demo                       ─┤
  Android pad (JNI setMsaaSamples)┘  ← 唯一有 UI 开关的真机入口

[L1 离屏目标]  OffscreenTarget::init()          vulkan_target.cpp:114-178
  desc_.samples ─► color_ (MS, samples=N, 无 TRANSFER_SRC)          :156-159
                 └► resolve_ (SS, samples=1, 可读回)                :163-168
                 └► depth_  (D16, samples=N)                       :170-176
  readback_rgba8() 读 resolve_image()                               :213

[L3 设备帧提交] submit_device_frame()            vulkan_device_frame.cpp:183
  shape.samples   = target->samples()                               :281
  shape.has_depth = target->depth_attachment().valid                :279
  pass_compatible = 管线自带 pass 的形状 vs shape 逐项比较           :302-316
      └─ 兼容 → 复用管线 pass；不兼容 → create_pass(shape)          :330-339
  framebuffer: [color] [depth?] [resolve?]                          :341-412
  clearValueCount = samples>1 ? 3 : has_depth ? 2 : 1               :434

[L3 管线]      VulkanPipeline::create_render_pass() vulkan_pipeline.cpp:559-645
  shape_.samples ─► atts[0/1].samples                               :567,577
  shape_.has_depth ─► pDepthStencilAttachment                       :612
  ms.rasterizationSamples = to_vk_samples(desc_.samples)            :735
```

`samples` 从 `TargetDesc` 到 `VkAttachmentDescription` **是通的**（L1 确实建了 MS 图 + resolve 图）。
断点在**入口**与**形状一致性**，不在 resolve 本体。

---

## 3. 假设清单与判定

| # | 假设 | 判定 | 一句话 |
|---|---|---|---|
| H1 | `supported_msaa_levels` 填错 / 与真实格式支持不一致 | **证伪** | caps `{1,4}` 与驱动按格式查询一致 |
| H2 | L1 的 MS 图 / resolve 图 / 读回目标搞错 | **证伪** | 建图、usage、读回目标全部正确 |
| H3 | L3 `rasterizationSamples` / resolve 附件 / subpass 依赖 与 `target->samples()` 不一致 | **证伪（默认路径）** / **成立（管线带深度时）** | 默认形状下一致；`has_depth=true` 时管线自己那张 pass 是 1 采样且无 resolve |
| H4 | PushConstants / viewport 受 MSAA 影响 | **证伪** | 无关联；`push_constant_bytes` 与 samples 无关 |
| **H5** | **默认路径 / UI 层 / F-SK / F-VX 内面 pass 没有 MSAA 可用入口** | **成立（主因）** | 用户「没有效果」最可能就是这里 |
| **H6** | **交换链（真机）路径：管线绑的 pass 与目标形状不一致 → resolve 永远接不上** | **成立** | 见 P0-3 |
| H7 | `vkQueuePresentKHR` 布局错误与 MSAA 路径有关 | **证伪（但与「MSAA 看起来没效果」的观测混在一起）** | 是测试自己从不画交换链图 |

---

### 3.1 H1：`DeviceCaps::supported_msaa_levels` 与 `vkGetPhysicalDeviceImageFormatProperties` 是否一致

**判定：证伪（caps 是准的）。**

实现（两步取交集）：

- `src/backend/vulkan/vulkan_device.cpp:98-134` `collect_msaa()`：
  ```cpp
  const VkSampleCountFlags color = lim.framebufferColorSampleCounts;
  const VkSampleCountFlags depth = lim.framebufferDepthSampleCounts;
  ... fmt_mask &= query_color_sample_counts(phys, kMsaaProbeColorFormats[i]);
  push(VK_SAMPLE_COUNT_1_BIT, 1); push(4,4); push(8,8); ...
  ```
- 按格式查询（usage 与 MS 图实际建图条件一致，只 `COLOR_ATTACHMENT`）：
  `src/backend/vulkan/vulkan_resource_common.cpp:145-164`；
  候选格式 `kMsaaProbeColorFormats = {B8G8R8A8_UNORM, R8G8B8A8_UNORM, B8G8R8A8_SRGB, R8G8B8A8_SRGB}`
  `vulkan_resource_common.cpp:140-143`。
- 由 caps 门控的消费点：`l3_cli/main.cpp:406-427`（`--msaa` 校验）、`android/scene_renderer.cpp:489-495`、
  `vulkan_render_device.cpp:104-113`（`DeviceDesc.requested_msaa`）。
- 测试守门：`src/backend/vulkan/msaa_render_pass_test.cpp:76-145`（`Msaa.CapsMatchRealFormatSupport`）
  对 caps 里每个 `n>1` 直接用 **同一个 `VK_FORMAT_B8G8R8A8_UNORM`** 问驱动，caps 比驱动宽就红。

**本机实测**：`magicblocks_test.exe --gtest_filter=Msaa.*` 四条 caps 相关用例全绿；
`l0-cli device` 输出 `supported_msaa_levels: [1, 4]`；
`l3-cli blocks --msaa 8` → `--msaa 8 不被设备支持（...）。支持列表: 1 4`。

**顺带纠正任务书的一处事实错误（影响后续任务的验收写法）**：
`docs/superpowers/specs/2026-11-09-magicblocks-msaa-tasking.md:151-153` 写
「`to_vk_samples(64)` 回落到 `VK_SAMPLE_COUNT_1_BIT`，于是 caps 校验永远非 0」。
实测 `to_vk_samples`（`vulkan_resource_common.cpp:181-196`）**只对字面量 2/4/8/16 返回对应位，
其余一切（含 64）走 `default: return VK_SAMPLE_COUNT_1_BIT`** —— 所以任务书这句本身没错。
真正做「拒绝」的不是它，而是 `OffscreenTarget::init()` 的数值反推比较：

```cpp
// src/backend/vulkan/vulkan_target.cpp:127-134
const VkSampleCountFlagBits vk_samples = to_vk_samples(want_samples);
if (want_samples > 1 && static_cast<std::uint32_t>(vk_samples) != want_samples) {
  return Status::Unsupported("OffscreenTarget: samples=... 不是合法的 Vulkan 采样数(只支持 2/4/8/16)");
}
```

以及第二层防线 `color_format_supports_samples()`（`vulkan_resource_common.cpp:166-179`，
先 `switch` 只放 2/4/8/16，再按格式查驱动）。
结论：**拒绝逻辑在 `vulkan_target.cpp:131-134` 与 `color_format_supports_samples()`，
`to_vk_samples` 只做翻译、不做校验**；后续任务不要在这些地方「再补一次校验」。

---

### 3.2 H2：`OffscreenTarget` 的 MS 图 / layout / resolve 附件 / 读回目标

**判定：证伪（这一层是对的）。**

| 环节 | 位置 | 事实 |
|---|---|---|
| MS 颜色图创建 | `vulkan_target.cpp:146-159` | `samples = vk_samples`；usage = `COLOR_ATTACHMENT \| TRANSFER_DST`（**不含 TRANSFER_SRC**，正确：多重采样图禁止 `vkCmdCopyImageToBuffer`） |
| resolve 图创建 | `vulkan_target.cpp:163-168` | `VK_SAMPLE_COUNT_1_BIT`，usage = `COLOR_ATTACHMENT\|TRANSFER_SRC\|TRANSFER_DST` |
| 深度图 | `vulkan_target.cpp:170-176` | **`samples = vk_samples`**（深度也必须多采样，否则 pass 附件采样数不匹配） |
| 读回目标 | `vulkan_target.cpp:210-213` | `const VkImage src_image = resolve_image();` → 读**单采样 resolve 图**，不读 MS 图 |
| resolve 回落 | `vulkan_target.h:97-99` | `samples == 1` 时 `resolve_image() == color_`，历史路径零变化 |
| 读回 layout 屏障 | `vulkan_target.cpp:284-285` | `oldLayout = VK_IMAGE_LAYOUT_GENERAL` — 与 `create_pass()` 的 `att[resolve].finalLayout = att[0].finalLayout = GENERAL`（`vulkan_device_frame.cpp:138,105`）严丝合缝 |
| 帧目标虚函数 | `vulkan_target.h:114-116` | `frame_color_image() = color_`、**`frame_resolve_image() = resolve_image()`**（2026-11-09 那个「基类默认返回 color_」的坑已经修好） |
| 自检旁路 | `vulkan_draw.cpp:57-89,122-143` | `draw_selftest` 的 pass 采样数取自 `target->samples()`，resolve 视图另建 |

**实测**（captain 已做、本次未重复）：`l3-cli sketch --msaa 4` 与 `--msaa 1` 的 PNG 差 2417/256000 像素，
边沿出现过渡色 → resolve 链路在**离屏路径**上确实工作。

**本次独立复核的一条旁证**：
```powershell
.\magicblocks-l3-cli.exe stack --layers 8 --strokes 3 --inner-pass off      --output st_noinner.png     # 9A16FE…
.\magicblocks-l3-cli.exe stack --layers 8 --strokes 3 --inner-pass off      --output st_noinner2.png    # 9A16FE…  （逐字节可复现）
.\magicblocks-l3-cli.exe stack --layers 8 --strokes 3 --inner-pass off --msaa 4 --output st_noinner_4.png # DF5643…
```
→ 4x 与 1x 不同 = 有抗锯齿；两次 1x 完全相同 = 确定性没被 MSAA 改动破坏。

---

### 3.3 H3：`vulkan_pipeline` 的 `rasterizationSamples` / resolve 附件 / subpass 依赖 是否与 `target->samples()` 一致

**判定：默认路径证伪；「目标有深度」路径成立（且这是 Android 真机的现实）。**

#### 3.3.1 管线自带 pass 的实际形状

`VulkanPipeline::init()`：

```cpp
// src/backend/vulkan/vulkan_pipeline.cpp:529-532
if (desc_.samples == 0) desc_.samples = 1;
if (shape_.samples == 0) shape_.samples = desc_.samples;
if (!desc_.compute) shape_.samples = desc_.samples;   // samples 对齐，OK
```

但 **`shape_.has_depth` 从来不会被任何代码置位**，它只有两个来源：

1. 构造函数的 `AttachmentShape` 参数（`vulkan_pipeline.h:56-61`，默认 `has_depth = false`）；
2. 设备工厂 `VulkanRenderDevice::create_pipeline(desc)` —— **不传 shape**：
   ```cpp
   // src/backend/vulkan/vulkan_render_device.cpp:285-292
   auto p = std::make_unique<VulkanPipeline>(ctx_, desc);
   ```
   （传 shape 的是另一个入口 `create_pipeline_for_shape`，L3 公共面根本不暴露，全仓只有
   `android/scene_renderer.cpp:312` 一处在用。）

于是管线自带 pass 恒为 `{color = desc.samples, has_depth = false, resolve = 自身 samples>1}`
（`vulkan_pipeline.cpp:564-613`）。
`ms.rasterizationSamples = to_vk_samples(desc_.samples)`（`vulkan_pipeline.cpp:735`）——
这一处**是对的**，`rasterizationSamples` 与 `desc_.samples` 一致。

#### 3.3.2 render pass 兼容性判定

```cpp
// src/backend/vulkan/vulkan_device_frame.cpp:302-316
const bool ps_resolve   = ps.samples > 1;
const bool shape_resolve = shape.samples > 1;
pass_compatible = ps.has_depth == shape.has_depth &&
                  ps.samples   == shape.samples   &&
                  ps_resolve   == shape_resolve   &&
                  ps.color_format == ... && vk_format_of(ps.color_format) == shape.color_format;
```

`shape.samples = target->samples()`（`:281`），`shape.has_depth = target->depth_attachment().valid`（`:279`）。

**结论分两种情况**：
- `target->depth_attachment().valid == false`（ui-cli、L0 自检目标、无深度离屏目标）：
  管线 pass `{samples=N, has_depth=false}` 与 shape 完全一致 → **复用管线自带 pass** →
  该 pass 带 `pResolveAttachments`（`vulkan_pipeline.cpp:613`）→ MSAA 生效。
  **这是唯一一条「管线自带 pass + MSAA」真正自洽的情形。**
- `target->depth_attachment().valid == true`（**l3-cli 的 sketch/stack，以及 Android 交换链**）：
  shape `{has_depth=true}` ≠ 管线 `{has_depth=false}` → **永远不兼容**，一律走
  `create_pass(ctx.device, shape, &err)`（`:330-339`）。

  → 此刻 **`PipelineDesc.samples` 对 render pass 没有任何影响**：pass 的采样数只由 `target->samples()` 决定。
  → 而在交换链上行，`create_pass(shape)` 带 `has_depth` 时会把 `pDepthStencilAttachment`
  指到附件下标 1（`vulkan_device_frame.cpp:143,149`），framebuffer 也去取
  `frame_img->frame_depth_image()`（`:360`）—— 交换链实现恒返回 `VK_NULL_HANDLE`
  （`vulkan_swapchain.cpp:59`）→ `fci.attachmentCount` 只有 1，而 pass 声明 2 个附件
  → `vkCreateFramebuffer` 失败或校验层报错。**这条链上 MSAA resolve 根本到不了 framebuffer。**

#### 3.3.3 subpass 依赖

两处完全一致（都是单条 `EXTERNAL → 0`，`COLOR_ATTACHMENT_OUTPUT → COLOR_ATTACHMENT_OUTPUT`，
`dstAccessMask = COLOR_ATTACHMENT_WRITE`）：
`vulkan_pipeline.cpp:625-632` 与 `vulkan_device_frame.cpp:155-162`。
**判定：证伪（没有漏依赖）。**
但没有「subpass 退出到 resolve 完成」的依赖 —— 因为 resolve 发生在 `vkCmdEndRenderPass` 内部，
由 pass 的 `finalLayout` 转换隐式完成，目前靠 `readback_rgba8()` 自己的
`GENERAL → TRANSFER_SRC_OPTIMAL` 屏障（`vulkan_target.cpp:282-293`，`srcAccessMask = COLOR_ATTACHMENT_WRITE`）兜住。
**这是「能用但依赖巧合」的写法**，修复任务里应显式补 barrier 或改用 `vkCmdResolveImage`。

#### 3.3.4 PushConstants / viewport 是否受影响

**证伪（无关联）。**
`push_constant_bytes` / `plci.pushConstantRangeCount`（`vulkan_pipeline.cpp:776-796`）与 samples 无关；
`vkCmdSetViewport/Scissor` 与 samples 无关（`vulkan_device_frame.cpp:440-446,540-553`）。
MSAA 只改光栅化采样数、附件采样数与 resolve 目标，不改坐标映射，不需要对 viewport 做任何补偿。

---

### 3.4 H5：默认路径 / UI 层 / F-SK / F-VX 内面 pass 有没有 MSAA 入口

**判定：成立（用户「没有效果」的主因）。**

#### (a) UI 消费层：**完整的一帧 UI 图是 CPU 画的，GPU 只负责拼**

- 离屏 UI 合成 = 纯 CPU 软件光栅化：
  `src/ui/overlay_renderer.cpp:17`
  > 「光栅化刻意**不加抗锯齿**、坐标四舍五入到整数像素 —— 同一输入逐像素同输出。」
  `blend_px`（`:38-46`）逐像素四舍五入，线条/描边全部硬边。
  `ui-cli render` 走的就是这条路：`src/ui_cli/main.cpp:217` → `OffscreenTargetRenderer::present()`
  → `src/ui/scene_offscreen.cpp:38-72`（**只用 GPU 画「图元 → 像素」的合成，图元几何本身已经是 CPU 舍入过的整数矩形**）。
- GPU 侧 UI pass 硬编码 1 采样：
  `src/backend/vulkan/vulkan_ui_overlay.cpp:141`（`att.samples = VK_SAMPLE_COUNT_1_BIT`）、
  `:213-215`（`ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT`）。
- UI 公共面**没有任何 samples 字段**：`include/magicblocks/ui/*.h` 里唯一与 msaa 有关的字符串是
  `perf.h:112` 的 `bool ui_msaa_enabled() const;  // Full 才开`，实现对 `perf_governor.cpp:136`，
  **全仓无消费者**（`grep ui_msaa_enabled` 只有声明与定义）。
- ui-cli 创建目标时明确 `td.with_depth = false;` 且**不设 `td.samples`**：
  `src/ui_cli/main.cpp:144-149`。

→ **在 UI 层无论怎么点，都不可能出 MSAA 效果。**

#### (b) 默认路径（不带 `--msaa`）

| 入口 | 是否可开 MSAA | 位置 |
|---|---|---|
| `l3-cli paint/blocks/gizmo/sketch/stack` | 有 `--msaa N`，**缺省 1** | `src/l3_cli/main.cpp:81-84`、`apply_msaa()` `:406-427` |
| `ui-cli render/stats/present` | **无** | `src/ui_cli/main.cpp:76-114`（参数表里没有 msaa） |
| `l0-cli render`（L0 自检 grid） | **无**（`draw_selftest` 用目标默认 1 采样；`src/cli/main.cpp:151` 不传 samples） | — |
| `cube-demo` | 无 | `src/cube_demo/main.cpp`（grep `msaa`/`samples` 零命中） |
| `magicblocks-sink-verify` / `ShellSurfaceSink::attach` | 用 `create_swapchain()`，**不带 MSAA** | `src/ui/scene_sink_host.cpp:42` |
| Android pad（唯一带 UI 开关的） | `create_swapchain_with_msaa(..., msaa_samples_)`，`msaa_samples_` 默认 **1** | `src/android/scene_renderer.cpp:101-102,245-248` |

**实测（5 个子命令全部默认 1x，stderr 无任何 `[frame] shape.samples=` 非 1 的记录）**：
```powershell
foreach ($c in 'paint','blocks','gizmo','sketch','stack') {
  .\magicblocks-l3-cli.exe $c --width 320 --height 240 --output tmp\v_$c.png }
```
→ 全部正常出图，但都是 1 采样。**用户「默认打开程序看到的画面」就是 1 采样。**

#### (c) F-SK 魔法涂鸦 5 步链路

`l3-cli sketch`（`src/l3_cli/main.cpp:760-1100` 区间）：
`apply_msaa(device, args.msaa, &td)`（`:763`）→ `create_pipeline(scene_pipeline_desc(args.msaa))`（`:1025`）→
`scene_pipeline_desc()` 里 `d.samples = msaa > 1 ? msaa : 1`（`:392`）。
链路是通的，**但只要用户不显式敲 `--msaa 4`，就是 1 采样**。
而且 F-SK 走的是「离屏骨架」，**F-SK 在 UI 层没有渲染出口**（UI 层的玩法注册是空桩
`EmptyFeatureHost`，`src/ui_cli/main.cpp:53-65`）→ 用户在 UI 里永远看不到 F-SK 的 MSAA。

#### (d) F-VX 魔法叠叠乐：双 pass 里的**内面 pass**

`src/features/stack/stack_render.cpp:404-484` 两个 pass 各自 `begin_pass → set_viewport → bind_pipeline → …`。
两个 pass 用的是**同一个 target**，所以 `shape.samples` 相同、`create_pass()` 形状相同 → **不会出现「两个 pass 采样数打架」**。

但有两个真实问题，修复任务必须一并处理：

1. **`clear_depth` 被静默丢弃**（与 MSAA 无关但同为形状漂移）：
   `stack_render.cpp:415` 传 `rp.clear_depth = pr.clear`，而设备帧提交里
   `PassShape::has_depth = target->depth_attachment().valid`（`vulkan_device_frame.cpp:279`）在
   **管线 pass 复用分支**恒为 false（§3.3.1）→ 走回退分支时才有深度；
   两条分支的 `att[1].loadOp` 都写死 `CLEAR`（`vulkan_device_frame.cpp:112`），L1 录制端的
   `f[5]/f[6]` 清屏标志在设备帧提交里**完全没被读**（`:414-425` 只读 `f[0..4]`）。
   → pass B 的 `clear_depth=false` 不起作用，深度每次都被 clear。
2. **内面 pass 今天在 320×240 下是「看起来没画」的**（实测）：
   ```powershell
   .\magicblocks-l3-cli.exe stack --width 320 --height 240 --layers 8 --strokes 3 --inner-pass on  --output st_inner_on_320.png  # CBBD63…
   .\magicblocks-l3-cli.exe stack --width 320 --height 240 --layers 8 --strokes 3 --inner-pass off --output st_noinner_320.png    # 9F93E5…
   .\magicblocks-l3-cli.exe stack --width 640 --height 400 --layers 8 --strokes 3 --inner-pass off --output st_noinner.png        # 9A16FE…
   .\magicblocks-l3-cli.exe stack --width 640 --height 400 --layers 8 --strokes 3 --inner-pass on  --output st_inner_on.png       # 9A16FE…  ← 与 off 逐字节相同
   ```
   640×400 时 `--inner-pass on/off` **输出完全相同**（口径：`inner-pass` 的统计里 `pass 2 / drawcall 2` 确有下发，
   但像素零差异）。这不是 MSAA 的锅，是**内面 pass 的绘制结果被表面 pass 完全覆盖**（表面 pass 带
   `depthWrite` + 半透明 blend `ONE / ONE_MINUS_SRC_ALPHA`，`stack_render.cpp:92-96,548-556`）。
   → **用户看到的「叠叠乐内部透光」在当前构建里根本不存在**，这会进一步加深「调了参数没变化 → 引擎坏了」的印象。

#### (e) 结论

> 用户「L1 与 L3 的 MSAA 没有效果」，最可能的真实路径是：
> **打开 UI（ui-cli / Android 壳层的 UI 合成）→ UI 层一次 MSAA 都没有（P0-1）；
> 或跑 l3-cli 默认参数 → 1 采样（P0-2）；或跑 Android pad → 交换链路径 resolve 接不上（P0-3）。**
> captain 在 l3-cli 上实测出 4x/1x 有差异，那只是**三条路径里唯一一条真正接通的**，
> 而它恰好是最不像「用户界面」的那条。

---

### 3.5 H7：`vkQueuePresentKHR` 的 `UNDEFINED` 布局错误是否与 MSAA 有关

**判定：证伪（与 MSAA 无关），但需要单独记账，别让它继续污染「MSAA 无效」的判断。**

#### 6.1 布局错误的真实成因：测试自己从不向交换链图渲染

```
[vk-ERROR][VALIDATION] vkQueuePresentKHR(): pPresentInfo images passed to present must be in
layout VK_IMAGE_LAYOUT_PRESENT_SRC_KHR or VK_IMAGE_LAYOUT_SHARED_PRESENT_KHR but
VkImage 0x70000000007 is in VK_IMAGE_LAYOUT_UNDEFINED.
```

现场：`magicblocks_test.exe` 第 1467-1506 行（`MsaaSwapchain.CurrentTargetFollowsAcquiredImage`）。
该用例 `tests/magicblocks_msaa_test.cpp:629-653` 只做
`acquire_next_image() → present_current()` 循环，**中间没有任何 `begin_pass`/`submit`**。
交换链图的初始布局是 `UNDEFINED`，没人把它转到 `PRESENT_SRC_KHR` → 校验层报错 → 驱动把 0 号图反复交回
→ `saw_nonzero == false` → `:657-667` 的 `GTEST_SKIP`。

**与 MSAA 无关的证据**：
- 报错的图像是 `VkImage 0x70000000007`（交换链图本体），不是 MS 图；
- 同一段落里测试自己写的 skip 原因就是这个现象（`:661-664`）；
- 唯一会走 `PRESENT_SRC_KHR` 的分支在 `vulkan_device_frame.cpp:99-102`
  （`PassShape::from_swapchain` 为真时 `initialLayout = finalLayout = PRESENT_SRC_KHR`）。

#### 但这里藏着两个**真** bug（修复任务要一起修）

**(i) 没有任何代码把 `from_swapchain` 置为 true。**
`PassShape::from_swapchain` 由 `frame_img->frame_from_swapchain()` 赋值
（`vulkan_device_frame.cpp:274,282`），但：

- `AttachmentShape`（`vulkan_pipeline.h:56-61`）**没有 `from_swapchain` 字段**；
- `VulkanPipeline::create_render_pass()`（`vulkan_pipeline.cpp:559-645`）**没有 `from_swapchain` 分支**，
  `atts[0].finalLayout` 恒为 `VK_IMAGE_LAYOUT_GENERAL`（`:573`）。

于是：**只要交换链路径复用了管线自带 pass，present 就必然因为布局不是 `PRESENT_SRC_KHR` 而失效。**
2026-11-09 任务书 `2026-11-09-magicblocks-msaa-tasking.md:46` 写的
「`AttachmentShape` 加 `bool from_swapchain`，`pass_compatible()` 比较它」—— **两件事都没落地**。

> 今天 Android 之所以还没被这条打中，纯粹是因为 §3.3.1 那个「`has_depth` 不一致 ⇒ 永远不复用管线 pass」的巧合，
> 让它走了 `create_pass()` 的 `from_swapchain=true` 分支。**一旦按「正确姿势」修好形状传递，present 会立刻全坏。**
> 这是修复任务最容易踩的连环坑。

**(ii) `create_pass()` 的 `clearValueCount` 在 `has_depth=true & samples>1` 时不对。**
`vulkan_device_frame.cpp:434`：`rbi.clearValueCount = shape.samples > 1 ? 3u : (shape.has_depth ? 2u : 1u);`
附件数 = `1(color) + 1(depth) + 1(resolve) = 3` → 3 是对的；
但 `has_depth=false & samples>1` 时附件数是 2、这里给 3。
测试输出里的 `clearValueCount > attachmentCount (3 > 2)` 就是这么来的（`tmp\test_out.txt:1440`）。
> ⚠️ **该现象来自旧二进制**：当前源码 `create_pass()` 的 `count` 与 `rbi.clearValueCount` 已经自洽
> （`1+has_depth+samples>1`）。但 `vulkan_command.cpp:507-509` 里 L1 自己录制的 render pass
> **硬编码 `att[0].samples = VK_SAMPLE_COUNT_1_BIT` 且只挂 1 个附件** —— 那是「端到端」的 MSAA 断点，
> 见 §4 修复清单 R5。**构建必须全量重建后复测，不能沿用旧二进制结论。**

#### 6.3 其它与 MSAA 无关但会干扰判断的校验错误（实测 `test_out.txt`，共 163 条 vk-ERROR/WARN）

| 消息 | 次数 | 归属 |
|---|---|---|
| `VkMemoryAllocateFlagsInfo.flags` 含 `DEVICE_ADDRESS_BIT` 但未启用 `bufferDeviceAddress` | 43×2 | `vulkan_allocator.cpp:701-711`（VMA 开了 BDA，但 `vulkan_render_device.cpp:154-163` 创建逻辑设备时没启用该特性） |
| `vkCmdPipelineBarrier ... dstAccessMask SHADER_READ 不被 COLOR_ATTACHMENT_OUTPUT 支持` | 16 | **`vulkan_target.cpp:312-314`** —— 读回后的回切屏障 `dstAccessMask = SHADER_READ \| COLOR_ATTACHMENT_READ` 配 `COLOR_ATTACHMENT_OUTPUT` 阶段 |
| `vkCreateSwapchainKHR minImageCount=2 双缓冲` | 12 | WARN/PERFORMANCE，无行为影响 |
| `Vertex attribute at location 0 not consumed` | 10 | 交换链 cube demo 顶点布局冗余 |
| `vkQueuePresentKHR layout UNDEFINED` | 9 | §6.1 |
| `vkCmdClearColorImage ... COLOR_ATTACHMENT_OPTIMAL` 非法布局 | 4 | **`vulkan_ui_overlay.cpp:479`**（UI 离屏清屏用了 `COLOR_ATTACHMENT_OPTIMAL`，规范只允许 `TRANSFER_DST/SHARED_PRESENT/GENERAL`）—— 这条就是 UI 层那条路的实锤瑕疵，见 §4 R1 |
| `vkEndCommandBuffer ... invalid state (objects invalidated)` | 8 | `vulkan_command.cpp` 提前销毁 framebuffer/pass 的历史坑的残留路径 |

---

## 4. 直接指导修复的清单（**本轮不实施**）

按「先通路，后正确性」排序；每条都给了定位点，后续任务直接照做即可。

| ID | 修复项 | 位置 | 为什么必须先做 |
|---|---|---|---|
| **R1** | **给 UI 层加 MSAA 入口**：`UiAppConfig`/`UiOverlayPass` 暴露 samples；UI pass 的 `att.samples` 与 `rasterizationSamples` 取自目标采样数；离屏 UI 合成若保留 CPU 光栅，则必须给出「GPU 路径才有 AA」的明确说明或直接切到 GPU 合成。顺手修 `vulkan_ui_overlay.cpp:479` 的非法清屏布局。 | `vulkan_ui_overlay.cpp:141,213-215,479`、`include/magicblocks/ui/*.h`、`ui_cli/main.cpp:144-149` | 用户看到的一切界面都在这儿，**不改这条，用户永远说「没效果」** |
| **R2** | **默认路径给 MSAA 一个可见入口**：ui-cli/l0-cli/cube-demo 加 `--msaa`；`ShellSurfaceSink::attach` 走 `create_swapchain_with_msaa`；Android 的 `msaa_samples_` 默认值与 Kotlin 开关对齐（现在默认 1）。 | `ui_cli/main.cpp`、`cli/main.cpp`、`cube_demo/main.cpp`、`ui/scene_sink_host.cpp:42`、`android/scene_renderer.cpp:101-102` | 没有入口 = 没有效果，与 backend 是否修好无关 |
| **R3** | **统一「附件形状」的唯一真源**：把 `PassShapeCore` / `build_attachment_descriptions()` / `pass_clear_value_count()`（任务书 `:42-45` 承诺过、代码里不存在）真正落到 `vulkan_resource_common.{h,cpp}`，`create_pass()`（`vulkan_device_frame.cpp:89-179`）与 `create_render_pass()`（`vulkan_pipeline.cpp:559-645`）都改调它。 | 见左 | 两份手写是 P0-3 潜伏的土壤 |
| **R4** | **`has_depth` 必须来自目标而不是「猜」**：`create_pipeline` 需要拿到 `target` 的形状（或让 L3 走 `create_pipeline_for_shape`，但那要动 G3 分层，需 captain 裁定）；同时 `AttachmentShape` **补齐 `from_swapchain` 并让 `pass_compatible()` 比较它**（否则修好 R3/R4 后 present 会全坏，见 §3.5(i)）。 | `vulkan_render_device.cpp:285-292`、`vulkan_pipeline.h:56-61`、`vulkan_device_frame.cpp:302-316` | Android 真机 resolve 接不上的直接原因 |
| **R5** | **端到端 MSAA（L1 命令列表 → 设备帧）**：`vulkan_command.cpp:507-530` 的 `begin_pass` 自录 pass 硬编码 `samples=1` 且只挂 color 附件；MSAA 时它必须退化为「只录像、不开真 pass」（交给 `submit_device_frame`），否则 pass 采样数与 MS 图不匹配。 | `vulkan_command.cpp:490-574` | L3 若改走「L1 自录 pass + `bind_pipeline`」就是第二个全黑入口 |
| **R6** | **清屏标志要真落地**：`vulkan_device_frame.cpp:414-425` 只读 `f[0..4]`，`f[5]/f[6]`（clear_color / clear_depth 标志）从未被读；`att[1].loadOp` 写死 `CLEAR`。F-VX 的「pass B 不清深度」契约（`stack_render.cpp:415`）现在是**假的**。 | 见左 | 会影响用户对「叠叠乐」的观感判断 |
| **R7** | **内面 pass 像素零差异**：`--inner-pass on/off` 在 640×400 逐字节相同（§3.4(d)）。要么是 D-VX-4′ 的 blend/depth 组合被表面 pass 覆盖，要么是内面 pass 没真正画出像素 —— **必须查清**，否则「调了没变化」会被继续误判成 MSAA 问题。 | `stack_render.cpp:548-570`、`vulkan_pipeline.cpp:738-755` | F-VX 验收前置 |
| **R8** | 读回回切屏障的 access 位（`vulkan_target.cpp:312-314`）与 `vkCmdClearColorImage` 布局（`vulkan_ui_overlay.cpp:479`）按规范修正。 | 见左 | 校验层 16+4 条错误的真凶，会淹没真正的 MSAA 报错 |
| **R9b** | **第二类管线入口也有同样的形状丢失**：`ShaderLibrary::acquire_pipeline(key, raster, shape)`（`src/primitives/shader_library.cpp:156-187`）把 `TargetShape`（含 samples）算进缓存键，却**只调 `create_pipeline(desc)`**，`shape` 没有传到后端 → 建出来的管线自带 pass 恒为 1 采样。R3/R4 修完后必须同时修这里，否则「材质/着色器库」这条路径仍然无 AA。 | `shader_library.cpp:186-187` | 同一 bug 的第二个入口，漏了就会「修好了但没生效」 |
| **R9** | 两个 SKIP 必须消灭（全仓 0 跳过纪律）：`Msaa.RejectsUnsupportedLevel`（`tests/magicblocks_msaa_test.cpp:306-310`）改成用一个 caps 明确不含的合法值 + 构造性注入；`MsaaSwapchain.CurrentTargetFollowsAcquiredImage` 改成先 `begin_pass`+`submit` 再 present（顺带把 §6.1 的 present 布局错误一起消掉）。 | `tests/magicblocks_msaa_test.cpp:296-319,582-668` | 「0 跳过」是团队纪律；且这两条 skip 正好遮住了两个真 bug |

---

## 5. 复现命令（全部可逐条粘贴执行）

```powershell
cd MagicBlocks\build

# 1) 设备能力（caps 是否准）
.\magicblocks-l0-cli.exe device

# 2) 全量测试 + 校验层消息取证
cmd /c ".\magicblocks_test.exe > tmp\test_out.txt 2>&1"     # exit=0，473 passed / 2 skipped
Select-String -Path tmp\test_out.txt -Pattern "vk-(ERROR|WARN)" | Measure-Object   # 163 条

# 3) 离屏路径 MSAA 确实工作（captain 已做，可复核）
.\magicblocks-l3-cli.exe sketch --width 800 --height 320 --msaa 4 --output tmp\t_sk4.png
.\magicblocks-l3-cli.exe sketch --width 800 --height 320 --msaa 1 --output tmp\t_sk1.png

# 4) 默认路径全员 1 采样（5 个子命令，无一处报 samples>1）
foreach ($c in 'paint','blocks','gizmo','sketch','stack') {
  .\magicblocks-l3-cli.exe $c --width 320 --height 240 --output "tmp\v_$c.png" }

# 5) UI 层没有 MSAA 入口（参数表里就没有 --msaa；输出与 golden 逐字节一致）
.\magicblocks-ui-cli.exe render --width 320 --height 240 --output tmp\probe\ui320.png
Get-FileHash tmp\probe\ui320.png,..\tests\data\golden_ui.png     # 同为 DD9C57B8…

# 6) F-VX 内面 pass 像素零差异
.\magicblocks-l3-cli.exe stack --width 640 --height 400 --layers 8 --strokes 3 --inner-pass on  --output tmp\probe\st_inner_on.png
.\magicblocks-l3-cli.exe stack --width 640 --height 400 --layers 8 --strokes 3 --inner-pass off --output tmp\probe\st_noinner.png
Get-FileHash tmp\probe\st_inner_on.png,tmp\probe\st_noinner.png  # 同为 C865D40D…（640x400）

# 7) 只跑那条 MSAA 核心用例，看它自己的校验层报错（framebuffer/pass 形状 + invalidated）
cmd /c ".\magicblocks_test.exe --gtest_filter=Msaa.EdgesActuallySmooth > tmp\es.txt 2>&1"
Get-Content tmp\es.txt | Select-String "vk-ERROR"
```

---

## 6. 覆盖面与诚实交代（别让后来人以为它被证明过）

1. **本次全部在 SwiftShader（Windows）+ headless 离屏上验证**。真机（Android Mali）的交换链路径
   **本机无法执行**，§3.5(i) 的 present 结论是**静态推导**（`from_swapchain` 字段缺失、`finalLayout` 恒 GENERAL、
   `AttachmentShape` 无该字段），叠加了 `build_android` 目录存在但无法运行的事实 —— 标为「推导成立，未实测」。
2. **同一条用例单独跑与整文件跑，校验层输出不同**：
   `tmp\es.txt`（`--gtest_filter=Msaa.EdgesActuallySmooth`）里**没有** `vkCreateFramebuffer` 采样数不匹配、
   也没有 `clearValueCount 3 > 2`；而整文件跑 `tmp\test_out.txt:1437-1446` **有**。
   说明「4 采样图挂到 1 采样 pass 上」的那条路径**确实存在于某条调用链**，
   但静态检索没定位到具体调用点（`vulkan_device_frame.cpp:302-316` 的兼容性判定与
   `vulkan_draw.cpp:57-93` 的 `draw_selftest` 旁路看起来都是自洽的）。
   **修复任务必须先全量重建 + 单用例复跑到 0 条 MSAA 相关 vk-ERROR，再谈「MSAA 已修好」。**
   —— **「绿」不等于「干净」**。
3. **§3.5(ii) 的 `clearValueCount 3 > 2` 只在旧二进制里出现**：
   `build/` 的 `magicblocks_test.exe` 产物时间戳早于工作区 `.cpp`
   （`ninja` 增量构建有「失败目标不更新依赖记录」的历史坑，任务书 `:409-426` 有记录）。
   当前源码 `vulkan_device_frame.cpp:434` 已经自洽。**任何后续验收必须先 `ninja -t clean` 全量重建再复测。**
4. 任务书 `2026-11-09-magicblocks-msaa-tasking.md` 的多处历史结论（`PassShapeCore` 已抽出、
   `AttachmentShape.from_swapchain` 已加）**与当前代码不符**，本文以代码为准，建议在修复任务开场时一并勘误。

---

## 7. 判定汇总（给 captain 的一页表）

| 假设 | 判定 | 一句话证据 |
|---|---|---|
| H1 caps 与真实格式支持不一致 | **证伪** | `collect_msaa()` 全局 ∩ 按格式交集；`Msaa.CapsMatchRealFormatSupport` 绿；`l0-cli device` → `[1,4]` |
| H2 OffscreenTarget 建图/布局/resolve/读回搭错 | **证伪** | `vulkan_target.cpp:146-176,213`；MS 图不带 TRANSFER_SRC；读回读 `resolve_image()` |
| H3 管线 `rasterizationSamples`/resolve/subpass 与 `target->samples()` 不一致 | 默认路径**证伪**；`has_depth=true` 路径**成立** | `vulkan_pipeline.cpp:735` 是对齐的；但 `shape_.has_depth` 恒 false（`vulkan_render_device.cpp:285-292`）→ 永远走 `create_pass()` |
| H4 PushConstants / viewport 受 MSAA 影响 | **证伪** | 与 samples 无数据流 |
| **H5 默认路径/UI/F-SK/F-VX 内面 pass 没有 MSAA 入口** | **成立（主因）** | UI 层 CPU 光栅 `overlay_renderer.cpp:17`；`ui_cli` 无 `--msaa`；`vulkan_ui_overlay.cpp:213-215` 硬编码 1 采样；5 个 l3 子命令默认 1x |
| **H6 交换链路径管线 pass 与目标形状不一致 → resolve 接不上** | **成立（推导）** | `AttachmentShape` 无 `from_swapchain`；`create_render_pass()` `finalLayout` 恒 GENERAL；交换链 `frame_depth_image()` 恒空 |
| H7 `vkQueuePresentKHR` 布局错误与 MSAA 有关 | **证伪**（但引出两个真 bug） | 测试从不画交换链图（`magicblocks_msaa_test.cpp:629-653` 无 begin_pass/submit）；`AttachmentShape` 无 `from_swapchain`；`clearValueCount` 旧二进制 |

---

## 8. 改动清单（本任务只写文档，未改任何代码）

| 文件 | 性质 |
|---|---|
| `docs/superpowers/specs/2026-11-11-magicblocks-msaa-root-cause.md` | **新增**（本文件） |

`MagicBlocks/` 下的源码、测试、CMake **零改动**；所有实测产物只落在
`MagicBlocks/build/tmp/`（`test_out.txt`、`es.txt`、`probe\*.png`、`v_*.png`、`t_sk4*.png`），
属于构建目录的临时取证，不入库。
