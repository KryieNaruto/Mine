# MSAA 抗锯齿 —— 接线任务书（已裁定）

日期：2026-11-09（2026-11-10 重大修订）
状态：**✅ 已交付（497/497 全绿，含 Android 真机根因修复）**
裁定人：用户（大肥鱼），2026-11-09
实施：msaa-worker
验收：Lead 独立复核（clean 全量重建 + 逐项实测）

---

## 交付结果（2026-11-10 最终版）

| 项 | 结果 |
|---|---|
| 全量回归 | **497/497 全绿**（1 skip 为已批准的环境限制） |
| 构建方式 | `ninja -t clean` 后**全量重建**，`BUILD_EXIT=0` |
| 3 张 golden | **逐字节不变**：`ca05a552a0223c4e` / `a181bf7595ceccf1` / `dd9c57b82f93b5dd` |
| AA 效果 | `blocks --blocks 8`：**12 色 → 113 色**（320×240，Lead 独立实测） |
| G3 分层纯度 | `[G3] OK backend 共扫到 137 个文件、backend 内 41 个文件，分层纯净` |

### ⚠️ 2026-11-09 的「395/395 已交付」是**不完整**的交付

那一版只验证了**离屏路径**，就说"抗锯齿完成了"。用户在 Android 真机上开了 MSAA X4
**完全没效果**。教训见下面「验收标准」一节。

验收标准应当是：**用户实际使用的路径出效果**，不是"我能跑通的那条路"。

---

## Android 真机 MSAA 无效果：五个根因

| # | 根因 | 位置 |
|---|---|---|
| 1 | `create_render_pass()` 不知道 `from_swapchain`，交换链上 `finalLayout` 恒为 `GENERAL`（应为 `PRESENT_SRC_KHR`）→ present 因布局不符丢帧 | `vulkan_pipeline.cpp` |
| 2 | `clearValueCount` 在 `has_depth=false` + `samples>1` 时报 3，实际附件 2 → 校验层抓到 `clearValueCount > attachmentCount (3 > 2)` | `vulkan_device_frame.cpp:418` |
| 3 | 所有 swapchain view 共用**一张** MSAA 图 → 多帧 in-flight 串扰 | `vulkan_swapchain.cpp` |
| 4 | `current_target()` 硬编码 `views_[0]`，而 `acquire_next_image()` 返回 `views_[idx]` | `vulkan_swapchain.cpp` |
| 5 | **最致命**：`shape.samples` **写死 1** —— MS 图建了，但**没有任何 pass 用它** | `src/android/scene_renderer.cpp` `rebuild_pipeline_for_swapchain()` |

### 修法

- 抽出共用纯函数到 `src/backend/vulkan/vulkan_resource_common.{h,cpp}`：
  `PassShapeCore`、`build_attachment_descriptions()`、`pass_clear_value_count()`，
  让 `VulkanPipeline::create_render_pass()` 与 `device_frame::create_pass()`
  走**同一份**附件描述逻辑（此前两份各自硬编码，必然漂移）。
- `AttachmentShape` 加 `bool from_swapchain`，`pass_compatible()` 比较它。
- `ISwapchain` 新增 `virtual IRenderTarget* target_at(std::uint32_t index)`
  带默认实现（返回 `current_target()`），注释写明「单图后端可接受；多图后端
  **必须覆写**」。`VulkanSwapchain` 覆写为 `views_[index]`（越界 nullptr）。
- 交换链改 **per-image MSAA**：`std::vector<VkImage> msaa_images_` +
  `std::vector<std::uint32_t> msaa_pages_`，逐张走分页分配器、逐张 release。
- 建 MS 图**之前**用 `color_format_supports_samples()` 复核，不支持则退回 1x + 打日志。

---

## 8x 静默降级（2026-11-10 修复）

**现象**：请求 `samples=8` → `effective msaa = 8`、`msaa_images=2`，但
`vkCreateImage` 被验证层报错（`VK_SAMPLE_COUNT_8_BIT is not supported by
format VK_FORMAT_B8G8R8A8_UNORM`）→ **拿到一张无效 MS 图，锯齿照旧且零日志**。
这是"静默降级"，只是从 1x 搬到了 8x。

**修法（两层防线，只有 B 是真正的修复）**：

- **方案 B（真正的修复，本机有判别力）**：`vulkan_swapchain.cpp` 建图**之前**调用
  `color_format_supports_samples(ctx_.phys, chosen.format, requested_msaa_)`，
  不支持就 `Status::Unsupported` → 走已有退回路径 + 日志。
  **不依赖验证层**——验证层在正式版和用户手机上根本不启用。
- **方案 A（纵深防御，本机零行为变化）**：`vulkan_device.cpp collect_msaa` 改为
  `全局限制 ∩ 各候选颜色格式的 vkGetPhysicalDeviceImageFormatProperties 交集`。
  **本机 SwiftShader 的全局 `framebufferColorSampleCounts` 本来就不含 8**，
  故本层在这台机器上零行为变化、注入不可能红。它的作用是在"全局允许但某格式
  不允许"的设备上守门。**如果将来有人问"8x 是怎么修的"，答案是方案 B，不是方案 A。**

**顺手修的真 bug**：`color_format_supports_samples` 对 3 和 64 判为"支持"
（因为 `to_vk_samples` 对非法值回退到 `VK_SAMPLE_COUNT_1_BIT`，而 1 永远存在）。
已改成显式 `switch` 只放 2/4/8/16，`default: return false`。

**唯一硬证据**（把 `color_format_supports_samples` 注入为恒 `return true`）：
```
magicblocks_msaa_test.cpp(720): error: Expected equality of these values:
  chain->msaa_samples()     Which is: 8   vs  1u
magicblocks_msaa_test.cpp(727): error: Expected equality of these values:
  chain->msaa_image_count() Which is: 2   vs  0u
```
红的形状与用户症状完全一致：**建了 2 张无效 MS 图、零提示**。

**用户视角最终确认**（Lead 独立实测）：
```
$ magicblocks-l3-cli blocks --msaa 8
--msaa 8 不被设备支持（SwiftShader Device (LLVM 10.0.0)）。支持列表: 1 4
$ magicblocks-l3-cli blocks --msaa 4
[blocks] 8 个体素 | ... | 已写出 (320x240)
```

---

## 验收标准（最重要的一条教训）

**"我能跑通的那条路"不等于"用户使用的那条路"。**

第一轮交付只验了离屏路径就宣布完成，用户在 Android 真机上看到完全没效果。
此后每轮验收都要求：**用户实际操作的那条路径出效果**，否则不算完成。

---

## 构建脚本假绿（2026-11-10 修复）

`_mb_build.bat` 原写作 `if (...) ( ... echo BUILD_EXIT=%ERRORLEVEL% )` ——
**`%VAR%` 在括号块里是解析期展开**，打印的是 `call vcvars64.bat` 的退出码（恒 0），
不是 `cmake` 的。已改为 `setlocal enabledelayedexpansion` + `!RC!`。

**判断构建成败的正确姿势**：以 `ctest` 能否跑起来 + 通过数变化为准，
`BUILD_EXIT` 只做参考。

---

## 交付结果（2026-11-09 16:4x，已被上文取代，留档）

| 项 | 结果 |
|---|---|
| 全量回归 | **395/395 全绿**（389 原有 + 6 新增） |
| 构建方式 | `ninja -t clean` 后**全量重建**，排除增量构建假象 |
| 3 张 golden | **逐字节不变**（`golden_l0/l2/ui.png` SHA256 未变） |
| AA 效果 | `blocks --blocks 8`：**12 色 → 113 色**（320×240，Lead 独立实测） |
| 门控 | G1/G3/G4/G7/G8 + deps + UIG1/UIG2 全绿 |

**AA 证据（Lead 独立复测，非采信报告）**：
```
magicblocks-l3-cli blocks --blocks 8            → 12  种唯一颜色
magicblocks-l3-cli blocks --blocks 8 --msaa 4   → 113 种唯一颜色
两张 PNG SHA256 不同
```

---

## 实施中发现的三个真 bug

### bug 1（最隐蔽）：`OffscreenTarget` 没重写 `frame_resolve_image()`

`IFrameTargetImage` 基类默认实现是 `return frame_color_image();`（给交换链兜底）。
实现方加了 `resolve_image()` 但没重写虚函数 `frame_resolve_image()`，于是
framebuffer 的 resolve 附件拿到的是**那张 4 采样图**，而 render pass 声明它是 1 采样
→ **采样数不匹配 → `vkCmdDraw` 被静默丢弃，画面全黑、零报错**。

诊断手法：打日志发现 4x 时 `frame_color_image()` 与 `frame_resolve_image()`
返回**同一指针**。

### bug 2：`samples=64` 没被拒

`to_vk_samples(64)` 回落到 `VK_SAMPLE_COUNT_1_BIT`，于是 caps 校验
`(caps & 1)` 永远非 0 → 校验形同虚设。
改成按 `want_samples` 反推真实位比对，认不出直接拒绝。

### bug 3（测试自身）：前景色常量写错

测试把前景写成白色 `(1,1,1)`，但 `pixel_regression.frag` 输出纯红 `(1,0,0)`，
导致 1x 的纯红前景像素全被误判成"过渡像素"，统计失去判别力。

---

## 已知覆盖缺口（归 L1 P0）

"录制一段含 `bind_sampled_texture` 的命令流再回放"**本轮做不到**：

- `include/magicblocks/scene/command_stream.h` 的 `CommandOp` 枚举只到
  `EndPass`（0-10），**没有 sampled/texture 对应 opcode**
- `IRecorder` 也没有 `record_bind_sampled_texture`

即 L1 尚未把第 12 条命令纳入命令流格式。已能验证的部分已验
（`ui_SceneFrame.BindSampledTextureCountsAsCommand` 证明它计入 `command_count()`）。

---

## 裁定结论（原文保留）

| 项 | 裁定 |
|---|---|
| 技术方案 | **MSAA**，默认 **4x**，采样数跟随 `DeviceCaps.supported_msaa_levels` 可调（2/4/8） |
| 默认状态 | **关闭**（samples=1）。抗锯齿作为**演示用开关**，人为勾选开启 |
| 自动回归 | **继续关闭**，389 条用例与 3 张 golden **逐字节不变** |

> 这条裁定很关键：MSAA 会改像素，而 golden 是逐字节比对。
> 所以**默认必须是 1 采样**，既有行为零变化；AA 只在显式请求时生效。

**实际设备能力**：SwiftShader 上 `DeviceCaps.supported_msaa_levels` 只有 `{1, 4}`。
`--msaa 8` 会明确报错并列出支持列表，符合"不静默降级"的裁定。

---

## 1. 现状诊断（已实测，勿重复调查）

### 1.1 上层 API 早就齐了

| 已有 | 位置 |
|---|---|
| `DeviceCaps.supported_msaa_levels` | `include/magicblocks/device/types.h:86`，`vulkan_device.cpp:72 collect_msaa()` 已按"颜色+深度同时支持"收集 |
| `PipelineDesc.samples` | `include/magicblocks/resources/pipeline.h:194` |
| 校验：2 的幂 + 被 caps 支持 | `vulkan_pipeline.cpp:382-391` |
| `TextureDesc.samples` | `include/magicblocks/resources/texture.h:40` |
| `TargetDesc.samples` | `include/magicblocks/device/types.h:134` |
| `PassShape.samples` → `att[].samples` | `vulkan_device_frame.cpp:250` 从 `target->samples()` 传导 |
| 管线自带 render pass 的 samples | `vulkan_pipeline.cpp:572,582`（`to_vk_samples(shape_.samples)`） |

**结论：`samples` 从 API 到 `VkAttachmentDescription` 是通的。**

### 1.2 真正断在三处

#### 断点 0（最靠前，原先漏查）：`vulkan_target.cpp` 直接拒绝 samples != 1

`src/backend/vulkan/vulkan_target.cpp:115`：
```cpp
if (desc_.samples != 1) {
  return Status::Unsupported("OffscreenTarget: L0 只支持 samples=1(MSAA 留 L1)");
}
```

**这一行不放开，samples 根本到不了 render pass。** 它就是当年"MSAA 留 L1"留下的坑。

同文件 `:61`：`ici.samples = VK_SAMPLE_COUNT_1_BIT;` 也要改成 `desc_.samples`，
否则会变成"render pass 要 4 采样、图像是 1 采样"的致命不匹配。

**放开要求**：改成校验 samples ∈ `DeviceCaps.supported_msaa_levels`，
不支持的等级**明确返回错误，不静默降级**。

#### 断点 A：光栅化采样数硬编码

`src/backend/vulkan/vulkan_draw.cpp:145`：
```cpp
VkPipelineMultisampleStateCreateInfo ms{};
ms.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;   // ← 硬编码，管线 samples=4 也按 1 光栅化
```

**这是最关键的一处**：即使把 `PipelineDesc.samples` 设成 4，
render pass 的附件也是 4 采样，但管线按 1 采样光栅化 →
**VkRenderPassBeginInfo 的 framebuffer 采样数不匹配，完整校验层会报错；
不报错时表现为"还是没抗锯齿"**。

修法：`ms.rasterizationSamples = to_vk_samples(desc.samples)`。
`to_vk_samples()` 已存在于 `vulkan_pipeline.cpp:127`，
但它在匿名 namespace 里，需要提到共享位置或复制一份。

#### 断点 B：全仓没有 resolve 附件

`pResolveAttachments` 在 `vulkan_pipeline.cpp` 与 `vulkan_device_frame.cpp`
**两处都没设置**。

MSAA 渲染到多重采样图像后，必须有 resolve 降采样成单采样图像才能读回。
没有 resolve → 读回的图会是错的（或只能拿到第一个采样点）。

**两处都要改**，因为 `submit_device_frame` 会优先复用管线自带的 render pass：
- `vulkan_pipeline.cpp:593-597` 的 `VkSubpassDescription sub`
- `vulkan_device_frame.cpp` 的 `create_pass()` 里同样的 `sub`

---

## 2. 实施步骤

### 2.1 `vulkan_draw.cpp` — 光栅化采样数

```cpp
// 从 PipelineDesc 取，而不是硬编码
ms.rasterizationSamples = to_vk_samples(samples);
ms.sampleShadingEnable = VK_FALSE;   // 不做 per-sample shading（性能）
ms.minSampleShading = 0.0f;
```

`to_vk_samples` 需要可见。建议放到 `vulkan_internal.h` 或类似共享头里，
`vulkan_pipeline.cpp` 与 `vulkan_draw.cpp` 共用一份，**不要复制两份**。

### 2.2 `vulkan_pipeline.cpp` — resolve 附件

当 `shape_.samples > 1` 时：

1. 附件数组扩容：`att[2]` → `att[3]`（color MS + depth MS + color resolve）
   实际排布建议：`[0]=color(MS)`, `[1]=depth(MS)`, `[2]=color_resolve(1 sample)`
2. `atts[2]`：`format` 同 color，`samples = VK_SAMPLE_COUNT_1_BIT`，
   `loadOp = DONT_CARE`，`storeOp = STORE`，
   `initialLayout = UNDEFINED`，`finalLayout = VK_IMAGE_LAYOUT_GENERAL`
   （与单采样时 color 的 finalLayout 一致，保证读回路径不变）
3. `sub.pResolveAttachments = &resolve_ref`（指向 `atts[2]`）
4. `atts[0].storeOp` 可改为 `DONT_CARE`（MS 内容 resolve 后无用）

**samples == 1 时必须完全走原路径**，一个字节都不能变。

### 2.3 `vulkan_device_frame.cpp` — 同样处理

`create_pass()` 里做与 2.2 完全一致的改动，并保证
`create_pass` 与 `create_render_pass` 的 **render pass 兼容性判定**
（`vulkan_device_frame.cpp:274-277` 的 `pass_compatible`）把 resolve 的
有无也纳入比较。

### 2.4 framebuffer 视图

`vulkan_device_frame.cpp:315-346` 建 view 的地方要相应增加
resolve 目标视图（单采样颜色图像）。若 `OffscreenTarget` 没有暴露
"单采样 resolve 图像"，需要在 `vulkan_target.cpp` 里补。

### 2.5 `samples=1` 时的严格零变化（验收口径）

- 3 张 golden（`golden_l0.png` / `golden_l2.png` / `golden_ui.png`）逐字节不变
- 389 条用例全绿

---

## 3. 新增用例

| 用例 | 断言 |
|---|---|
| `Msaa.DefaultIsSingleSample` | 不显式设 samples → `PipelineDesc.samples == 1`，`descriptor`/行为与既有一致 |
| `Msaa.SupportedLevelsFromCaps` | `DeviceCaps.supported_msaa_levels` 非空，含 1 |
| `Msaa.RejectsUnsupportedLevel` | 设成 caps 里没有的值 → `Status` 非 ok |
| `Msaa.RejectsNonPowerOfTwo` | samples=3 → 拒绝 |
| **`Msaa.EdgesActuallySmooth`** | **核心**：画同一几何，`samples=1` vs `samples=4`，统计边沿过渡像素数，4x 必须**显著更多** |

> `Msaa.EdgesActuallySmooth` 是唯一能证明"真的接上了"的用例。
> 我实测过现状：`blocks --blocks 8` 出图**只有 12 种唯一颜色**（每面一个纯色），
> 边沿是硬阶梯。4x 之后中间色应显著增加。

---

## 4. 演示开关

用户要求"作为演示用开关"。建议做成 CLI 参数：

```
magicblocks-l3-cli blocks --blocks 16 --msaa 4 --output x.png
magicblocks-l3-cli paint  --strokes 4 --msaa 4 --output x.png
magicblocks-l3-cli gizmo  --msaa 4 --output x.png
```

- `--msaa` 缺省 = 1（不开），**不加参数时行为与现在完全一致**
- 取值不在 `DeviceCaps.supported_msaa_levels` → 明确报错，不静默降级

可选：加一个基准图对比脚本 `tools/compare_msaa.py`，
展示开/关 AA 的边沿像素差异。

---

## 5. 并发注意（重要）

**`src/backend/vulkan/**` 目前由「积木引擎L1」会话在改 P0 纹理采样，正在活跃编辑**
（`vulkan_pipeline.cpp` 于 15:44:53 被改动）。

MSAA 要改的 `vulkan_pipeline.cpp` 与 `vulkan_draw.cpp` **与 L1 的工作重叠**。

### 5.1 已发生的撞车（2026-11-09 15:5x，已解决）

L1 的 P0 改动在 `build_graphics()` 里加了 `Status ds = Status::Ok_();`（原 769 行），
与同一函数 726 行的 `VkPipelineDepthStencilStateCreateInfo ds{}`（P12 深度/模板状态）
重名 → `error C2371: "ds": 重定义；不同的基类型`，**整个 magicblocks 库编不出来**。

已由 MSAA 侧改名为 `desc_status`（纯局部变量改名，零语义变化）并补了注释说明原因。

> **给 L1**：`build_graphics()` 里那个 `Status ds` 已改名为 `desc_status`，
> **不是你的代码被覆盖**，是变量重名。你若接着改这段，请沿用 `desc_status`，
> 或把 `create_descriptor_objects()` 的返回值改成内联 `if`，
> 但**别再用 `ds`**（714 行的 `VkPipelineDepthStencilStateCreateInfo ds` 不能动）。
> `build_compute()` 里的 `Status ds`（815 行）没有冲突，保持原样。

### 5.2 继续推进的要求

**要求**：
- 改动前先 `read` 目标文件确认最新内容（不要基于旧版本编辑）
- 只做**增量修改**，不要重写整个函数
- 每次 `edit` 后若报 `FS_STALE_VERSION`，重新 read 再改
- 如果发现 L1 正在改同一段代码，**停下来，在任务里记录冲突点**，不要强行覆盖

**区域划分**（当前互不重叠）：

| 范围 | 归属 |
|---|---|
| `vulkan_pipeline.cpp` 571-632（resolve 附件）、719-724（`rasterizationSamples`） | MSAA |
| `vulkan_pipeline.cpp` 451-537、758-781（描述符集 / `sampled_texture_slots`） | L1 P0 |
| `vulkan_draw.cpp`、`vulkan_target.cpp` | MSAA |
| `pipeline.h` 的 `sampled_texture_slots`、`command.h` 的 `bind_sampled_texture` | L1 P0 |

---

## 6. 验收自检

| 项 | 验收 |
|---|---|
| 默认零变化 | 不带 `--msaa` 时 3 张 golden 逐字节不变 |
| 回归 | `ctest -j 1` 全绿（389 + 新增） |
| AA 真的生效 | `Msaa.EdgesActuallySmooth` 通过：4x 边沿过渡像素显著多于 1x |
| 可调 | `--msaa 2 / 4 / 8` 在 caps 支持时都可用 |
| 拒绝非法值 | `--msaa 3` / caps 不支持的值 → 明确报错 |
| 门控 | G1/G3/G4/G7/G8 全 rc=0 |

> 验收用 `ctest -j 1`。（`RESOURCE_LOCK vulkan` 已加，`-j 4` 理论上也可，
> 但 AA 改动涉及 framebuffer，保守起见串行验收。）

---

## 7. 高风险点（前人踩过的坑，别重复）

1. **`upload()` 后必须 `flush_pending()`** —— 漏掉 = 全零缓冲 = 空图，**完全不报错**。
2. **`Box{lo, hi}` 是陷阱** —— 三成员 `{min,max,empty}`，`empty` 默认 `true`，
   聚合初始化只给两个参数得到"空盒"。用 `Box::of()`。
3. **subpass 依赖不能省** —— 删了会整片保持清屏色且不报错
   （`vulkan_pipeline.cpp:599-608` 有详细注释）。
4. **`create_pass` 与 `create_render_pass` 必须保持一致** ——
   设备帧提交优先复用管线自带 render pass，两条路径不一致会得到两种行为，极难查。
5. **`PassShape.samples` 别忘填** —— `vulkan_device_frame.cpp:250` 从
   `target->samples()` 取，`0` 会被当成 `1`。

### 7.1 增量构建会骗人（2026-11-09 实测）

**现象**：改了 `vulkan_target.h`（15:56）后，`vulkan_render_device.cpp.obj`
仍停在 14:18，ninga 认为无需重编。于是 `make_unique<OffscreenTarget>` 用的是
**旧的 sizeof**，对象布局错位：`samples=3` 本该在校验期被拒，实际在析构时段错误
（`0xC0000005`）；`samples=64` 本该被 caps 拒绝，结果返回 ok。

**归因（重要，别搞错）**：这**不是** ninja 依赖系统坏了。
实测 touch `vulkan_target.h` 后 `ninja -n` 正确列出 3 个待重编目标。
真实原因是：**当时 `vulkan_command.cpp` / `recorder.cpp` 编译失败，
ninja 不会为失败的目标更新 `.ninja_deps` 条目**，这些 `.obj` 就停在旧时间戳。
这是"失败目标没有新依赖记录"的正常行为。

**规则**：
> 一旦出现过编译失败，**之后的增量构建结果一律不可信**。
> 改过公共头文件（尤其是类成员布局）后，必须先
> `ninja -t clean` 或删 `build/` 再全量重建，然后才能跑测试。

**本条的代价**：Lead 上一轮"389/389 全绿"的结论是在 L1 未完成代码上跑的，
受此影响不可信，需在构建解锁后重跑。

`, `.mp4` 等，但不是产物格式。
