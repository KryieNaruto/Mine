# L1~L3 同步适配前置普查（t20 只读准备）

- 日期：2026-11-11
- 作者：syncl13（t20 负责人的开工前普查）
- 性质：**只读**。本文未改动任何引擎 / 测试 / CMake / 门控代码（`git status` 确认）
- 依赖：`2026-11-11-magicblocks-sync-audit.md`（survey / t18）
- 前置状态：t19（L0 同步核心）由 synccore 进行中。截至本文写作时仓内已出现
  `src/backend/vulkan/vulkan_sync_core.h/.cpp`（**未接线**，无任何 include 方），
  本文第 6 节记录其接口形态对 t20 的约束

---

## 0. 结论摘要（t20 的作业面）

| 项 | 事实 |
|---|---|
| L1/L3 里**在 submit 之后、fence 未信号之前**回收 GPU 资源的地方 | **9 处**，见 §1。其中 **2 处是"合法但必须改成定向等"**（读回路径），**1 处是真隐患**（`stack_render.cpp` 重建缓冲） |
| `submit_command_list` 调用点（src/ + tests/） | **29 处真实调用**（src 15 / tests 14）+ 1 处定义体内部转调（`vulkan_render_device.cpp:641`）。逐个分类见 §2 |
| 其中"submit 返回后立刻读回" | **14 处**（全部是离线 gold/CLI 出图，**等待语义必须保留**） |
| 其中"submit 返回后立刻复用/销毁 GPU 资源" | **1 处真隐患** + **3 处已由调用方契约兜住** |
| `advance_frame_index()`（L1 VMA 帧边界挂点）在 `src/` 的调用面 | **0**。只有 `tests/magicblocks_test.cpp:339/352` 两处。**L1 分配器的 `begin_frame()` 在 src/ 里也零调用** —— 帧边界目前根本没有接到生产路径上 |
| 现在已经在用"fence 定向等"的 L1 设施 | `submit_once_sync`（`vulkan_resource_common.h:72-112`），4 个 L1 资源路径都走它。**t20 不需要新建这套机制，只需把 L0 的两条旁路换成同构的做法** |
| 需改 `src/android/`（队长 outOfScope） | **需要，仅 `scene_renderer.cpp` 一处**。见 §4，**我不落笔** |

---

## 1. L1 资源池 / VMA：submit 之后、fence 未信号之前的回收点

判定口径：**这个释放（`allocator->free` / `release_image_page` / `vkDestroy*`）有没有可能发生在
"某次 submit 已经入队、但该次提交的 fence 还没信号"的时间窗内？**

### 1.1 清单

| # | 位置 | 释放什么 | 当前靠什么保证安全 | t20 处置 |
|---|---|---|---|---|
| R1 | `vulkan_resource_common.h:72-112` `submit_once_sync` | （不释放，是**同步器**） | 自带 `vkCreateFence` → `vkQueueSubmit(fence)` → `vkWaitForFences(UINT64_MAX)` → `vkDestroyFence`。**已经是"定向等这一次提交"** | **保留**。它是 t20 的参照实现，不是要被改造的对象 |
| R2 | `vulkan_buffer.cpp:297-409` `flush_pending_device_pages` | `:377-380` `destroy_staging`（`release_image_page(staging_page)` + `vkDestroyBuffer(staging_buf)`），在 `:382` submit 之后于 `:408` 调用 | **靠 R1**：`submit_once_sync` 返回时 fence 已信号 | 保留（R1 不变即安全） |
| R3 | `vulkan_texture.cpp:333-...` `submit_pending` | 临时 page_buf（覆盖整页的 `VkBuffer`，`:399` 建） | 靠 R1 | 保留 |
| R4 | `vulkan_texture.cpp:504-619` `readback_device` | `:615-618` `vkDestroyBuffer(page_buf)`、`:619` `allocator->free(rb.id)` | 靠 R1；`:614` 注释明写「submit_once_sync 已 fence 等待」 | 保留 |
| R5 | `vulkan_cubemap.cpp:279-364` `flush` | 六面上传的临时缓冲，`:364` 注释「submit_once_sync 已 fence 等待」 | 靠 R1 | 保留 |
| R6 | `vulkan_target.cpp:329-332` **`readback_rgba8`** | `:332` `vkFreeCommandBuffers`，随后 `:356` `cleanup_buffer()`（`:257-260` 销毁 `VkBuffer` + `release_image_page`） | **靠 `:330` `vkQueueWaitIdle`** ← 队长点名的第 2 条 | **必须改**。CPU 紧接着 `:346-352` 要 `memcpy` mapped 内存，**等待不可删**，只能改成 fence 定向等（用 `submit_and_wait_directed` 或 `submit_once_sync` 同构做法） |
| R7 | `vulkan_draw.cpp:281-283` + `:287-294` **`draw_selftest_grid`** | `:283` 命令缓冲；`:287-294` pipeline / layout / framebuffer / 2× imageview / renderpass / 2× shader module —— **全部被刚提交的队列操作引用** | **靠 `:282` `vkQueueWaitIdle`** ← 队长点名的第 1 条 | **必须改**。同 R6：改 fence 定向等。注意这里 `r` 是 `VkResult`，改造后要保证失败分支仍然不跳过销毁 |
| R8 | `vulkan_device_frame.cpp:243-798` `submit_device_frame` | 帧内 views / fb / rp（`:304-309` `destroy_now` 仅失败路径；成功路径 `:791` `retire_into_slot()` 交给槽位退休栈） | **t19 已完成**：`:270` `core->begin_frame` 只等本槽 fence；`:783` 提交时挂 `image_available`/`render_finished`；`:794` `end_frame`；**已删除提交后的 `vkWaitForFences` 与返回前 `cleanup()`** | **t19 已落地**。原 R8 里的 `vulkan_device_frame.cpp:715` 同步等与 `:728` cleanup 已不存在 |
| R9 | `vulkan_render_device.cpp:677` `discard_vk_side_channel` | 丢弃 `VulkanCommandList` 在 `begin_pass` 期间建的 Vk 侧对象 | **已核实安全，风险解除**：`vulkan_command.h:150-161` 明确这套对象是"顺带"建的**僵尸对象**，`submit_device_frame` 另用 `slot.cmd` 重放，**从未被提交**。`vulkan_command.cpp:107-134` 的 `destroy_vk_command_buffer` 只 end/destroy 自己的对象，不碰 `slot.cmd`。因此"提交后立刻丢弃"与 GPU 无关 | **无需改动**。t20 只做回归确认 |
| R10 | `vulkan_ui_overlay.cpp:625-630` **`render_offscreen`** | `:627` cmd、`:628` framebuffer、`:629` imageview、`:630` `att_views[1]` | **靠 `:626` `vkQueueWaitIdle`**（**截至本文写作仍未改**） | **归 t19**（队长已明确）。t20 只验证"不串行化" |
| R11 | `vulkan_allocator.cpp:1069-1092` `begin_frame` → `:1091` `trim()` → `:1105-1132` | 整页 `free_page_storage` = `vmaFreeMemory` | 靠 `:1114` `if (p.live != 0) continue;` + `:1115` `if (p.in_flight) continue;` + `:1117` `idle_frames < threshold`（**行号已复核，未漂移**） | **安全**。`in_flight` 标志是本文件唯一的在途守卫，但见 §1.3 的缺口 |
| R12 | `vulkan_allocator.cpp:768-782` `~VulkanPagingAllocator` | 全部页 `vmaFreeMemory` + `vmaDestroyAllocator` | 靠 `vulkan_render_device.cpp:75` `vkDeviceWaitIdle`（白名单，注释在 `:66`） | **保留** |
| R13 | `vulkan_buffer.cpp:75-89` `~VulkanBuffer` | `vkDestroyBuffer` + `allocator->free(id_)` | **没有任何 fence 保护**——靠调用方"缓冲活到 submit 之后" | **见 §2 的 T-风险**：`stack_render.cpp:319-324` 可能违背这个契约 |
| R14 | `vulkan_texture.cpp:99-109` `~VulkanTexture`、`vulkan_cubemap.cpp:46-58` `~VulkanCubeMap` | `vkDestroyImageView` / `vkDestroyImage` / `allocator->free` / `release_image_page` | 同上，纯靠调用方持有期 | 调用方都在 submit 前建好、submit 后不再动 → 安全（§2 已逐个核对） |
| R15 | `vulkan_target.cpp:102-112` `~OffscreenTarget` | 3 张 image + 3 个 image page | 纯靠调用方持有期 | §2 核对：**全部 14 个读回点在 submit 后仍持有 target** → 安全 |

### 1.2 一句话分类

- **靠 `submit_once_sync`（已是定向等）保安全的：R2~R5** —— 共 4 处，t20 **不动**。
- **靠 `vkQueueWaitIdle` 保安全的：R6、R7**（我改）、**R10**（t19 改）。
- **靠 `vkDeviceWaitIdle` 白名单保安全的：R12**（保留）。
- **靠调用方持有期、无 fence 保护：R13~R15** —— 这是 t20 里唯一需要重新核验的对象族。

### 1.3 L1 分配器的帧边界缺口（重要，必须报给队长）

`IFrameSync::advance_frame_index()` 被文档称作"L1 VMA 帧边界挂点"（`frame_sync.h:28`），但实测：

| 事实 | 证据 |
|---|---|
| `advance_frame_index()` 在 `src/` **零调用** | 全仓 grep 只命中 `tests/magicblocks_test.cpp:339`、`:352` |
| `IMemoryAllocator::begin_frame()` 在 `src/` **零调用** | 全仓 grep 只命中 `tests/*` 与各 `ISceneRecorder/IBatchPool/IOverlay2D` 的**同名不同接口**方法 |
| 后果 | `VulkanPagingAllocator::begin_frame` 里的 `vmaSetCurrentFrameIndex` 与页空置计数**在生产路径上从未推进**；`trim()` 只在测试调 `begin_frame` 时才被执行 |

**这对 t20 的含义**：t19 若把 `advance_frame_index` 改名为别的挂点、或把它接进渲染帧循环，
那才是 L1 帧边界第一次真正生效 —— 此时 `trim()` 会**开始真的回收页**。
页回收 = `vmaFreeMemory`，而 §1.1 R11 的守卫 `in_flight` **全仓没有任何地方 set 成 true**
（`set_page_in_flight` 仅定义于 `vulkan_allocator.cpp:1094`，`src/` 内零调用）。
即：**帧边界一旦接上，L1 就可能在多帧并行下退掉一张仍被在飞帧引用的页**。

> 这一条我不擅自改（它属于 L1 语义，且与 t19 的接线方式强耦合）。
> **请队长裁定**：由 t19 一并接线，还是给 t20 追加范围（含 `set_page_in_flight` 的接线）。

另：队长的背景里提到「Sky 9 条红的历史根因是 `reserve_pages` 页预算」。
本次普查确认该结论在代码上是自洽的（`vulkan_allocator.cpp:873-883`：`allocate` **绝不**自己
触发 `reserve_pages`，桶里没空块就直接 OOM），且 `sky.cpp:378-380` 的 `reserve_pages(Staging,1)`
就是那个修复。**t20 不会碰 `reserve_pages` 与页预算判定**，因此 Sky 用例的回归风险
只来自上面那条"帧边界接上后 trim 开始生效"。

---

## 2. `submit_command_list` 调用点普查（L2 / L3 / L4）

### 2.1 分类口径

- **A 类：submit 后立刻读回（CPU 要像素）** → 等待语义**必须保留**，只允许把"整队列/整设备等"
  换成"定向等这一次提交"。t20 在这些点**不改行为**（它们不需要改，`submit_device_frame`
  内部的等待位置由 t19 决定）。
- **B 类：submit 后马上提交下一帧（同 target / 同缓冲）** → 需要保证上一帧该槽位 fence 已信号。
- **C 类：submit 后销毁命令列表 / 其引用的缓冲** → 需要保证 GPU 已用完。
- **D 类：submit 后只读 CPU 统计、不碰 GPU 资源** → 无风险。

### 2.2 src/ 生产调用点（15 处真实调用）

> 统计口径：`grep -n 'submit_command_list('` 命中 src 16 条，其中 `vulkan_render_device.cpp:641`
> 是**定义体内部**调用（在 §2.2 P18 单列），其余 15 条是外部真实调用。

| # | 位置 | 类 | 提交后立刻做了什么 | 判定 |
|---|---|---|---|---|
| P1 | `ui_cli/render_backend.cpp:344`（`render_thumbnail`） | **A** | `:351` `readback_rgba8` | 必须等。**保持** |
| P2 | `ui_cli/render_backend.cpp:503`（天空盒） | **A** | `:509` `sky_tgt->readback_rgba8` | 必须等。**保持** |
| P3 | `ui_cli/render_backend.cpp:577`（模型） | **A + B** | `:586/:596` `readback_rgba8`；且与 P2 共用 `sky_tgt`/`tgt` 序列 | 必须等。**保持** |
| P4 | `l3_cli/main.cpp:721` | **A** | `:728` readback | 保持 |
| P5 | `l3_cli/main.cpp:1141` | **A** | `:1148` readback | 保持 |
| P6 | `l3_cli/main.cpp:1698` | **A** | `:1705` readback | 保持 |
| P7 | `l3_cli/main.cpp:2110` | **A** | `:2117` readback | 保持 |
| P8 | `l3_cli/main.cpp:2673`（F-VX 叠叠乐） | **A + C** | `:2680` readback；`overlay_buffers`（`:2547` 函数作用域）与 `cmd`（`:2615`）**活到函数结束** | 保持。`overlay_buffers` 生命周期契约**已满足** |
| P9 | `model_cli/main.cpp:325` | **A** | 随后 readback | 保持 |
| P10 | `sky_cli/main.cpp:532` | **A** | 随后 readback | 保持 |
| P11 | `fsk_cli/main.cpp:954` | **A** | 随后 readback | 保持 |
| P12 | `cube_demo/main.cpp:468` | **B** | 逐帧循环 `submit` → `present`（交换链） | 需 t19 的 per-slot fence 保证；**t20 不改** |
| P13 | `sink_verify/main.cpp:61` | **D** | 只返回 `.ok()` | 无风险 |
| P14 | `ui/scene_sink_host.cpp:123` | **B** | 紧接着 `:138` `present_current()` | 依赖 t19 的 render_finished 信号量。**t20 不改** |
| P15 | `android/scene_renderer.cpp:493` | **B** | `:527` `present_current()` | 同 P14。**在队长 outOfScope 内，我不动**（见 §4） |

**两条不带 `submit_command_list` 字样的间接提交路径**（它们是 `submit_command_list` 的**下游**，
不是调用方，但同样受 t19 改造影响，单独列出以免遗漏）：

| # | 位置 | 类 | 说明 |
|---|---|---|---|
| I1 | `l2_cli/main.cpp` → `OffscreenTarget::draw_selftest` → `vulkan_draw.cpp:281` | **A** | 走 §1.1 R7 的 `vkQueueWaitIdle`。**我改 R7 即覆盖** |
| I2 | UI 离屏 → `vulkan_ui_overlay.cpp:625` | **A** | 随后 `:635-638` `readback` → §1.1 R6。**t19 改 R10；R6 我改** |

**定义体内部转调 1 处**：`vulkan_render_device.cpp:670`（`submit_command_list` 里调 `submit_device_frame`）
→ 见 §1.1 R8 + R9。**t19 决定语义，R9 已核实安全。**

> ⚠️ **实况快照（我写作时 t19 正在落地）**：`vulkan_device_frame.cpp:243-244` 的签名已改为
> `submit_device_frame(..., VulkanSyncCore* core)`，而 `vulkan_render_device.cpp:670` 仍传
> `&submit_fences_[fence_slot]`（旧签名）—— 说明 synccore **尚未改完调用方**。
> 我按"t19 交付后再开工"执行，不碰这两个文件。

### 2.3 tests/ 调用点（14 处）

全部是"A 类：submit 后立刻 readback 做像素断言"或"D 类：只断言返回码"。
逐个核对后**无一处**在 submit 后销毁 submit 引用的 GPU 资源：

`magicblocks_msaa_test.cpp:199/796`、`magicblocks_msaa_pixel_test.cpp:511/830/1442/1545/1632`、
`magicblocks_l3_pixel_regression_test.cpp:244`、`magicblocks_l3_steady_state_test.cpp:330`、
`magicblocks_l3_sky_test.cpp:248`、`magicblocks_l4_stack_test.cpp:1009/1587/2274/2402`
（其中 `:1583` 与 `:1614` 的 `OverlayBuffers bufs` 都是**函数作用域**，满足"活到 submit 之后"契约）。

> **对 t20 的含义**：测试侧不需要改。风险全部在"引擎内部是否还保住这些对象"。

### 2.4 唯一的真隐患：`stack_render.cpp` 重建实例缓冲

```cpp
// src/features/stack/stack_render.cpp
319:  if (auto s = upload_instances(&surface_buffer_, surface_instances_); !s.ok()) return s;
320:  if (same_order) {
321:    inner_buffer_.reset();  // 显式表明"不持有第二份"
322:  } else if (auto s = upload_instances(&inner_buffer_, inner_instances_); !s.ok()) {
```

`upload_instances`（`:122-144`）第一行就是 `slot->reset()`（`:124`）—— **先销毁旧缓冲，再建新缓冲**。

- 当前安全的原因：`submit_device_frame` 在 `:715` 同步等 fence，所以 `submit_command_list` 返回时
  上一帧 GPU 已用完 → 下一帧再 `reset` 一定安全。**L3_CLI 是严格的 `build_instances`→`record`→`submit`→`readback`
  串行（`l3_cli/main.cpp:2540/2623/2673`），所以这条隐患目前不出现。**
- **t19 一旦让 submit 异步，这条就变成 use-after-free。**
  **前提已成立**：`vulkan_device_frame.cpp:783-797` 现在提交后**不再等待**就直接返回，
  帧内对象交给退休栈托管。于是 `submit_command_list` 返回时 GPU **可能仍在读**上一帧的实例缓冲。
  `slot->reset()` → `~VulkanBuffer`（`vulkan_buffer.cpp:75-89`）→ `vkDestroyBuffer` +
  `allocator->free(id_)`，而 `VulkanBuffer` **不在**退休栈的托管类型里（`FrameRetireList`
  只有 renderPass / imageView / framebuffer，`vulkan_sync_core.h:63-65`）。
- **t20 处置**：把它改成"按帧槽环形持有 N 份实例缓冲"（`in_flight_frames` 份，帧号 `% N` 取用），
  或"延迟销毁到该槽 fence 信号"。**这是我在 t20 里真正要动的 L3 代码，且是必修项。**

### 2.5 已由调用方契约兜住的三处（核对通过，不改）

| 位置 | 契约 | 实测是否满足 |
|---|---|---|
| `grid_overlay.cpp:373-377` `out_buffers` 移交 | 注释「缓冲必须活到 submit 之后」 | ✅ `l3_cli/main.cpp:2547` 与 `l4_stack_test.cpp:1583` 都是函数作用域 |
| `stack_render.h:110` `collector` | 「调用方持有，活到 submit」 | ✅ `l3_cli/main.cpp:2546` 函数作用域 |
| `stack_render.cpp:201-230` `vertex_buffer_`/`index_buffer_` | 只在 `create_passes` 里建一次 | ✅ 与每帧 submit 无关 |

---

## 3. UI 通道与 SDK 通道的独立性核验（队长第 3 条）

### 3.1 两条通道在 L0 里的实际形态

| | UI 通道 | SDK 通道 |
|---|---|---|
| 入口 | `UiOverlayPass::render_offscreen`（`vulkan_ui_overlay.cpp:463`） | `submit_device_frame`（`vulkan_device_frame.cpp:202`） |
| 提交 | `:625`（fence = NULL） | `:711`（fence = 池内 fence） |
| 当前等待 | `:626` `vkQueueWaitIdle` | `:715` `vkWaitForFences(1s)` |
| 建的对象 | `:498-519` view / `att_views[1]` / fb | `:406/425/449/465` views / fb / rp |
| 目标图像 | 调用方给的 `OffscreenTarget` | 同左 |
| 命令来源 | **自己录**（`:609-618` 内联 vkCmd*） | **回放 `ICommandList`** |

**关键结论：两条通道共用同一个 `ctx.queue` 与同一个 `OffscreenTarget`，但它们的对象与提交都是各自独立的。**
`vulkan_ui_overlay.cpp:620` 的注释也明写「单次 submit（离屏路径；UI 层 Presenter 不再为它提交）」。

### 3.2 串行化的真正风险点（不是 wait 本身）

两条通道**共用同一个 VkImage**（同一个 OffscreenTarget 的颜色附件），而它们对这张图的布局约定不同：

| 通道 | 颜色附件 `initialLayout` | 末态 `finalLayout` | 依据 |
|---|---|---|---|
| SDK | `clear_color ? UNDEFINED : GENERAL` | **`GENERAL`** | `vulkan_device_frame.cpp:116-118` |
| UI | **`COLOR_ATTACHMENT_OPTIMAL`**（写死） | 非 MSAA：`GENERAL`；MSAA：`COLOR_ATTACHMENT_OPTIMAL` | `vulkan_ui_overlay.cpp:191/197` |

UI 的 `loadOp = LOAD`（`:187`）意味着它**真的读**上一阶段的内容，而它假定进来时是
`COLOR_ATTACHMENT_OPTIMAL`。SDK 通道留下的却是 `GENERAL`。

> **这就是"UI 通道与 SDK 通道必须仍然独立"的技术含义**：不是"两个 fence 不能互相等"，
> 而是"**不能把两条通道的提交合并成一次、也不能让任一条改掉对方的布局假定**"。
> 一旦 t19/t20 里有人图省事把 UI pass 塞进 SDK 的帧提交，`GENERAL → COLOR_ATTACHMENT_OPTIMAL`
> 这一跳就没有任何 barrier 覆盖 → 白色斑块 / 底色丢失，且**校验层不一定报**。

**我在 t20 的核验动作**（不需要改代码，只需确认）：
1. `vulkan_ui_overlay.cpp:626` 的等待被 t19 换成"等某个 fence"后，该 fence **不能是 SDK 那套 per-slot fence**。
   若共用，一次 UI 出图会连带等掉 SDK 的槽位 → 真串行化。
2. 反过来，`submit_and_wait_directed`（`vulkan_sync_core.cpp:275`）**占用的是 `core->current_slot()`**
   —— 如果 UI 通道和读回通道都调它，它们会**抢同一个槽位**。这是"两条通道被串行化"的具体机制。
   §6 有专门条目。

### 3.3 每帧路径上的实际数量

UI 离屏出图**每次**是 `render_offscreen` + `readback` 两段 = **2 次提交、2 次等待**
（`vulkan_ui_overlay.cpp:625-626` 与 `vulkan_target.cpp:329-330` 各一次）。
L3 场景出图是 1 次（`vulkan_device_frame.cpp:711`）+ 1 次读回。

---

## 4. `src/android/` 与 `ui_app.cpp` 的 rebuild 路径（队长第 4 条：只写"若要改需改哪几行"，不落笔）

### 4.1 实测调用形态（先更正一处前提）

队长说「`swapchain.cpp:291` 不是每帧调，只在 OUT_OF_DATE 触发（`scene_renderer.cpp:515/529`、`ui_app.cpp:280` resize）」——
**这个结论准确**，本次逐行回读确认：

| # | 位置 | 触发条件 | 实际代码 |
|---|---|---|---|
| B1 | `scene_renderer.cpp:513-518` | `acquire_next_image()` 返回 `OutOfDate` | `:515` `swapchain_->rebuild(swapchain_->extent())`，随后 `:517` **重新 acquire 一次** |
| B2 | `scene_renderer.cpp:527-532` | `present_current()` 返回 `OutOfDate` | `:529` `rebuild(...)`，`:531` `return true`（**该帧丢弃**） |
| B3 | `ui_app.cpp:279-281` | `LifecycleKind::SurfaceChanged` 且 `ev.width && ev.height` | 只在 `dynamic_cast<ShellSurfaceSink*>` 成功时 `host->rebuild(...)`；**移动端壳层不经过这里** |

三条最终都进 `VulkanSwapchain::rebuild`（`vulkan_swapchain.cpp:319-327`）→ `destroy_swapchain()`（`:286-296`）→ `:291` `vkDeviceWaitIdle`。

### 4.2 「若要改需改哪几行」

> ⚠️ **实况更新**：synccore 在 t19 中**已经改完**了这一节（`vulkan_swapchain.cpp` 当前状态见下方对照）。
> 保留原文是为了说明"为什么这么改"，并留下核验依据。

**前提**：t19 提供"按设备排空在飞帧"的能力（`wait_device_frames_idle` / `core->drain`）。

| 文件 | 行（原始） | 现状 | 若要改，应改成 |
|---|---|---|---|
| `vulkan_swapchain.cpp` | **291** | **已改**（现在在 `:321`）：`wait_device_frames_idle(ctx_)` 取代 `vkDeviceWaitIdle` | ✅ 与我建议一致 |
| `vulkan_swapchain.cpp` | **287-289** | **已改**（现在在 `:322-325`）：等待**先于** `views_.clear()` / `destroy_msaa_images()` / `images_.clear()` | ✅ 顺序已修正，比我建议的更彻底 |
| `vulkan_swapchain.cpp` | **82** | **已改**（现在在 `:93`，且在 `views_.clear()`(`:95`) 之前）：析构仍保留 device 级等待 | ✅ 白名单保留正确（VUID-01282 硬要求） |
| `scene_renderer.cpp` | **512-518** | `acquire` → 若 OutOfDate → `rebuild` → 再 `acquire` | **不需要改调用形态**。swapchain 内部已改成 fence 等，这里自动受益 |
| `scene_renderer.cpp` | **527-532** | `present` → 若 OutOfDate → `rebuild`，本帧丢弃 | 同上，不改 |
| `ui_app.cpp` | **279-281** | 只在桌面参考 sink 上 rebuild | **不需要改**。移动端由 Kotlin/JNI 壳层自行处理 |

**结论：需要改 `src/android/` 的行数是 0。**

> 给队长：`scene_renderer.cpp` **我只读未写**。若最终发现确实要动它（例如要给 `rebuild` 传入
> "当前在飞的 image index"），我会先报再动。

### 4.3 `ui_swapchain` 侧的一个既有不一致（报给队长）

`vulkan_swapchain.cpp:369-388` 的长注释说明**acquire 不传 fence/semaphore**，
而 `vulkan_sync_core.h:84-85` 的思路是 `image_available` 信号量。
t19 接线的同时会改到 `acquire_next_image`（swapchain 内部）——
**这与我 R6/R7 的改造文件不重叠**（我改 `vulkan_target.cpp` / `vulkan_draw.cpp`），
但 `destroy_swapchain` 的 291 行**属 L0**，我不动。

---

## 5. 我 t20 的落笔边界（避免与 t19 互相覆盖）

| 文件 | t19（synccore） | t20（我） | 冲突？ |
|---|---|---|---|
| `vulkan_sync_core.h/.cpp` | 拥有 | **只读调用** | 否（我依赖其接口） |
| `vulkan_render_device.h/.cpp` | 拥有（成员、析构、`submit_command_list`） | 只读 | 否 |
| `vulkan_device_frame.h/.cpp` | 拥有 | 只读 | 否 |
| `vulkan_ui_overlay.cpp` | 拥有（`:626`） | 只读 | 否 |
| `vulkan_swapchain.cpp` | 拥有 | 只读 | 否 |
| **`vulkan_target.cpp`** | 只声明由我改 `:329-330` | **我改** | 需在开工时对齐一次 |
| **`vulkan_draw.cpp`** | 只声明由我改 `:281-282` | **我改** | 需在开工时对齐一次 |
| **`vulkan_resource_common.h`** | 无 | 可能改（`submit_once_sync` 复用 t19 设施） | 需对齐 |
| **`src/features/stack/stack_render.cpp`** | 无 | **我改**（§2.4） | 否 |
| `src/android/scene_renderer.cpp` | 无 | **不动**（队长 outOfScope） | 否 |

---

## 6. 对 t19 接口的前置审阅意见（基于仓内未接线版，供 synccore 参考）

以下是我按 L1/L3 使用面读 `vulkan_sync_core.h/.cpp` 时发现的、会**直接打到 t20 作业面**的问题。
这些不是"审阅 verdict"，只是前置反馈；最终以 t19 交付版本为准。

1. **`submit_and_wait_directed` 抢占 `current_slot()`（`vulkan_sync_core.cpp:275`）**
   它的注释（`:287-288`）说"不推进 frame_index"，但**用了 `core->current_slot()` 这个槽的 fence**。
   同一帧里若 UI 出图与 SDK 读回都调它 → 第二次会先等第一次的 fence（串行化），
   更糟的是第二次会 **reset 一个刚被本帧 SDK 提交占用、尚未信号的 fence** → `VUID-vkResetFences`。
   **这正是队长第 3 条担心的"UI 通道与 SDK 通道被串行化"的具体机制。**
   建议：辅助提交用**独立的 fence 池**（不与 N 个帧槽共享），或明确"每帧最多调用一次"。

2. **`begin_frame` 里 `vkResetCommandBuffer`（`:188`）与持久 cmd 的配对**
   `FrameSlot::cmd` 是环形的，`begin_frame` 只 reset **当前槽**的 cmd。
   但 `submit_device_frame` 目前自己是 `vkAllocateCommandBuffers`（`:221`）——
   若 t19 改为用 `slot.cmd`，则 `submit_command_list` 的调用方（§2.2 的 18 处）
   **在 `submit_command_list` 返回后仍持有 `ICommandList`**。
   需要保证：`ICommandList`（L1 的录制器）与 `slot.cmd`（L0 的回放命令缓冲）是**两样东西**，
   不能混用同一个对象，否则 `discard_vk_side_channel()`（`vulkan_render_device.cpp:649`）的语义会崩。

3. **`end_frame(VkFence submitted)`（`:195-205`）注释与实现相反**
   注释写「把 fence 复位（必须在 `vkQueueSubmit` **之前**）」，但实现是在 `end_frame` 里 reset ——
   如果调用方顺序是 `begin_frame → submit → end_frame`，那 reset 发生在 submit **之后**，
   会把刚被 GPU 信号化的 fence 重新置为未信号 → 下一次 `begin_frame` 等满超时（1fps 事故的翻版）。
   建议确认调用顺序，或把复位挪进 `begin_frame`、`end_frame` 只推进序号。

4. **`FrameRetireList` 不含 `VkBuffer`/`VkImage`**（`:63-65` 只有 renderPass/imageView/framebuffer）
   §2.4 的 `stack_render` 实例缓冲需要的是**延迟销毁 GPU 资源对象**，退休栈装不下。
   t20 会自己用"帧槽环形持有"，请 t19 确认不会与退休栈的帧序冲突。

5. **`wait_latest_frame` 等 `current_slot()`（`:325`）**
   读回前"等最近一帧"在 N 槽模型下**不是** `current_slot()` —— 最近一次提交发生在
   `frame_index_-1` 对应的槽。当前实现等的是**下一帧要用的那个槽**，语义偏差。
   L1/L3 的 14 个读回点（§2.2）都依赖"读回前 GPU 已写完"，这一条若不对，会有偶发读到旧像素。

---

## 7. 基线口径（不在本文拍数字）

队长已明确：**验收数字以 t19 交付时的实测为准**，我改前跑一次、改后再跑一次，报告给前后对比。
本文**不预填任何 gtest / CTest 数字**（survey 报 585/574、CTest 608，与我未实测的状态无关）。

t20 开工后的取数顺序（固定，避免口径漂移）：
1. 在 t19 交付后的干净树上，用与 qa 相同的构建目录重建（**改头文件布局必须删 build 全量重建**）。
2. 原样贴 `[==========] N tests from M test suites ran.` 与 `[  PASSED  ] K tests.` 两行，不汇总。
3. CTest 用 `ctest -N` 取注册数，`--test-dir` 与 build 目录一致。
4. 静态硬指标自查：
   `vkDeviceWaitIdle` / `vkQueueWaitIdle` 全仓只剩 `vulkan_render_device.cpp:66` 与 `vulkan_swapchain.cpp:82`。
   我改完必验。

---

## 8. 未确认项（诚实披露）

1. **N 槽模型下"读回前等最近一帧"的正确写法** —— 依赖 t19 最终接口，本文只指出 `wait_latest_frame`
   的语义偏差（§6.5），未给出实现。
2. **`discard_vk_side_channel` 在异步 submit 后是否仍安全** —— **已核实安全**（§1.1 R9）：
   它销毁的是 L1 录制器的"僵尸"对象，从未被提交，与 `slot.cmd` 无关。**风险解除**。
3. **`vulkan_swapchain.cpp:287-289` 的销毁顺序是否真的会触发 VUID** —— 本文确认为**代码层面的隐患**
   （销毁在等待之前），但**未实测**是否会报错。
4. **UI 通道 `initialLayout = COLOR_ATTACHMENT_OPTIMAL` 与 SDK 的 `GENERAL` 冲突在实际执行顺序下
   是否已被某个隐式转换兜住** —— 未实测。只确认了两边的声明值不同。
5. **`ship` 侧（Android 真机）** —— 本文全部基于源码阅读，未在真机上验证任何时序。

---

## 9. 给队长的一页摘要

- 我的 t20 作业面最终收敛为 **4 个文件、5 个改动点**（按 t19 已落地版本重估）：
  1. `vulkan_target.cpp:329-330` —— 读回等待改定向等（**不可删**）
  2. `vulkan_draw.cpp:281-282` —— CLI 出图旁路改定向等
  3. `vulkan_resource_common.h:72-112` —— `submit_once_sync` 若需复用 t19 设施则对齐（可能 0 改动）
  4. `src/features/stack/stack_render.cpp:122-144 + 319-324` —— 实例缓冲改为帧槽环形（**必修**：t19 已提交异步，此隐患已成立）
  5. （可选）`vulkan_allocator.cpp` 的 `in_flight` 接线 —— **需队长裁定归属**
- **风险更新（相对 t18 审计）**：
  - R9 `discard_vk_side_channel` —— **已核实安全，风险解除**（销毁的是从未提交的僵尸对象）
  - R8 `submit_device_frame` —— **t19 已落地**（异步提交 + 退休栈）
  - §2.4 `stack_render` —— **由"潜在隐患"升级为"确定隐患"，必须修**
- 有 **2 个需要队长先裁定的问题**：
  - **Q1**：L1 帧边界（`advance_frame_index` / `begin_frame` / `set_page_in_flight`）**在 src/ 里全零调用**，
    而 `trim()` 依赖 `in_flight` 守卫、`in_flight` 无人置位。t19 接线后 trim 首次生效 →
    **多帧并行下可能退掉在飞页**。归 t19 还是追加给 t20？
  - **Q2**：`submit_and_wait_directed` 抢 `current_slot()`（§6.1）会让 UI 通道与 SDK 通道串行化，
    且可能 reset 一个未信号的 fence。请转达 synccore 让其用独立 fence 池。
- **`src/android/scene_renderer.cpp` 只需 0 行改动**（rebuild 的调用形态不必动，改全在 `vulkan_swapchain.cpp`）。

---

## 10. 实况快照：写作期间 t19 的落地进度（供对齐用）

我在写本文期间观察到 synccore 正在实时改 L0。以下是**截至本文最后一次回读**的状态，
供双方对齐用；**以 t19 交付版本为准**。

### 10.1 已落地（synccore 已改）

| 项 | 位置 | 状态 |
|---|---|---|
| 同步核心 | `vulkan_sync_core.h/.cpp`（新增） | 已接线：`vulkan_render_device.h:13` 已 include |
| `submit_device_frame` 签名 | `vulkan_device_frame.cpp:243-244` | 改为 `(..., VulkanSyncCore* core)` |
| 帧开始 | `vulkan_device_frame.cpp:270` | `core->begin_frame(1s)` 只等本槽 fence |
| 提交（异步） | `vulkan_device_frame.cpp:762-788` | 挂 `image_available`（wait）/ `render_finished`（signal）/ `in_flight`（fence） |
| 帧内对象 | `vulkan_device_frame.cpp:791` `retire_into_slot()` | 交退休栈托管，**已删除提交后的 `vkWaitForFences` 与返回前 `cleanup()`** |
| 帧结束 | `vulkan_device_frame.cpp:794` | `core->end_frame(VK_NULL_HANDLE)`（fence 已在 `:778` 提交前复位） |
| `destroy_swapchain` | `vulkan_swapchain.cpp:321` | `wait_device_frames_idle(ctx_)` 取代 `vkDeviceWaitIdle`；**等待已挪到 `:323-325` 的 clear 之前** |
| `~VulkanSwapchain` | `vulkan_swapchain.cpp:93` | 保留 device 级等待（白名单，VUID-01282） |
| `wait_idle()` | `vulkan_render_device.cpp:343-348` | 改为走同步核心，无核心时退回 `vkDeviceWaitIdle` |

### 10.2 尚未改（截至最后一次回读）

| 位置 | 现状 | 归属 |
|---|---|---|
| `vulkan_render_device.cpp:670` | 仍传 `&submit_fences_[fence_slot]`（旧签名），与 `vulkan_device_frame.cpp:244` 新签名**不匹配** | t19 待收尾 |

### 10.2b ⚠️ **我的三个作业点已被 synccore 在 t19 里直接改完**

写作期间 synccore 越过原定边界，把 §9 里属于 t20 的三处一起改了。
**我未参与、未授权，但逐行复核后确认改法正确**（与我在 §9 提的方案一致）：

| 原 t20 作业点 | synccore 的改法 | 我的复核结论 |
|---|---|---|
| `vulkan_target.cpp:330` 读回 | `:338` `submit_and_wait_directed(ctx_, cmd, 1s)`；`:339-349` 两个分支都 `vkFreeCommandBuffers` | ✅ 等待保留（CPU 仍 `memcpy`），粒度从 queue 级降为本次提交；失败分支不再漏放 cmd |
| `vulkan_draw.cpp:282` CLI 旁路 | `:286` `submit_and_wait_directed(ctx, cmd, 1s)`；`:287` 先放 cmd | ✅ 同构；`:278-281` 的 `si` 变成未使用变量（**编译告警待观察**） |
| `vulkan_ui_overlay.cpp:626` UI 通道 | `:634` `submit_and_wait_directed(ctx_, cmd, 1s)` | ✅ 归 t19，本就该它改 |

**对 t20 的实际影响**：作业面从"改 3 处同步"缩减为 **只剩 L3 层**：

1. **必修**：`src/features/stack/stack_render.cpp:122-144 + 319-324` 实例缓冲帧槽化（§2.4）——
   t19 已提交异步，这是当前**唯一尚未修复的 use-after-free 隐患**。
2. **核验**：UI 通道与 SDK 通道是否被 `submit_and_wait_directed` 串行化（§3 / §6.1）。
3. **裁定待定**：L1 `in_flight` 帧边界接线（§1.3）。

### 10.3 当前 `vkDeviceWaitIdle` / `vkQueueWaitIdle` 实点分布（`src/`，最后一次回读）

```
真调用点（非注释）：
  vulkan_render_device.cpp:75  vkDeviceWaitIdle  ← 白名单（析构）
  vulkan_swapchain.cpp:93      vkDeviceWaitIdle  ← 白名单（析构）
  vulkan_sync_core.cpp:327     vkDeviceWaitIdle  ← 新出现：wait_latest_frame 的兜底分支
  vulkan_sync_core.cpp:347     vkDeviceWaitIdle  ← 新出现：wait_device_frames_idle 的兜底分支
```

> ✅ **`vkQueueWaitIdle` 已全仓归零**（`vulkan_draw` / `vulkan_target` / `vulkan_ui_overlay` 三处全部改成
> `submit_and_wait_directed`）。
>
> ⚠️ **给队长 / synccore 的一条硬指标提示**：队长的判据是「`vkDeviceWaitIdle` / `vkQueueWaitIdle`
> 全仓只剩 `vulkan_render_device.cpp:66` 与 `vulkan_swapchain.cpp:82` 两处」。
> 当前 `vulkan_sync_core.cpp:327` 与 `:347` 是**两处新的真调用**（都在"无同步核心登记"的兜底分支，
> 生产路径不会走到，字面上仍是调用）。若按**字面**执行门控，它们会让门控红。
> 建议把口径明确成「生产路径（有同步核心时）不执行」，或让门控识别这两处兜底。
> **我不擅自改 `vulkan_sync_core.cpp`（t19 文件）**，仅报告。

### 10.4 我 t20 开工的判定条件

**等 t19 交付**（`vulkan_render_device.cpp:670` 改完、签名对齐、能全量构建通过）后我才开工。
理由：§10.2b 的 L3 隐患只有在"submit 真正异步"的前提下才成立，而 `:670` 目前仍传旧签名，
说明 t19 的接线尚未完成 —— 此时改 L3 会基于一个半成品前提。

---

## 11. 队长裁定后的复核准备（第二轮 · 仍是只读）

队长已就 Q1 / Q2 / Q4 作出裁定，并把 t20 靶面重切为「以 L1–L3 全量复核为核心」。
本节记录我在 t19 交付**前**能做实的判定，使 t19 一落地只需「验证」而非「重新推理」。

### 11.1 队长裁定摘要（记录，避免口径漂移）

| 编号 | 裁定 | 对我 t20 的要求 |
|---|---|---|
| Q1 L1 帧边界归属 | 归 **synccore / t19** | 我改为**事后复核**：验证 `begin_frame` / `advance_frame_index` 接线没让 `vmaFreeMemory` 在飞帧里回收在用的页，并逐条给 R1–R15 结论 |
| Q2 `submit_and_wait_directed` 抢 `current_slot()` | 判**必修**，要 synccore 改成独立 fence | 我交付时**复核它真改到位**，不采信"报告说改了" |
| Q4 `sync_core.cpp:327/:347` 两处 wait-idle | 判**踩线，要求删除**，白名单不扩 | 我交付前自检：全仓只剩 2 处，且都在析构内 |
| L3 `stack_render.cpp:124` UAF | **已改派 synccore**（需扩 `FrameRetireList` 支持 `VkBuffer`） | 我**不重复改**，改为复核 + 补回归用例 |
| 写码范围 | `src/scene/` `src/primitives/` `src/backend/vulkan/` | `src/android/`、`src/ui/`、`src/features/` 只报告不落笔 |

> 注意裁定 §5「写码范围」与 §2「L3 stack_render 复核 + 补用例」的交界：
> `src/features/stack/` 不在我的写码范围内，因此**回归用例要落在 `tests/`**，不去改 `src/features/` 的实现。

### 11.2 ⚠️ 本轮最重的发现：L1 分配器的帧边界防护**目前是空转的**

这不是 t19 引入的回归，而是 L1 **从来就没接上电**；但 t19 把帧边界接上之后，它会**从"沉睡缺陷"变成"活缺陷"**。

证据链（全部实测回读，四步闭合）：

| 步 | 事实 | 位置 |
|---|---|---|
| 1 | `trim()` 在 `src/` 的**唯一**调用点是 `begin_frame()` 内部 | `vulkan_allocator.cpp:1091` `(void)trim();` |
| 2 | `IMemoryAllocator::begin_frame()` 在 `src/` **零调用**（只有 `tests/magicblocks_l1_test.cpp:165/216/224` 三处） | 全仓 grep |
| 3 | `set_page_in_flight()` 在 `src/` **零调用** → `p.in_flight` 自建页置 false（`:586`）后**永远是 false** | `vulkan_allocator.cpp:1094`（定义，无调用方） |
| 4 | `VulkanRenderDevice` **从不**向分配器推帧号（`memory_allocator()` 只是暴露接口，无 `begin_frame` 转发） | `vulkan_render_device.h:63` |

**推论**：`trim()` 里 `:1115` 的 `if (p.in_flight) continue;` 这一道"M4 硬条件"防护**恒不成立**，
等于不存在。一旦 t19 把 `begin_frame` 接到生产路径，`trim()` 每帧都会跑，而此时
`p.in_flight` 仍恒为 false → 满足 `live==0 && 空闲满 120 帧` 的页会被直接
`:1118 free_page_storage(&p)` → `:524 vmaFreeMemory(vma, p->vma_allocation)`，
**不管有没有在飞帧还引用着这块内存**。

**现阶段危害的定量缓解（如实说明，不夸大）**：

- `page_idle_frames` 默认 **120**（`vulkan_allocator.h:67`），而 `in_flight_frames` 只有 **2 或 3**
  （`vulkan_render_device.cpp:113` 校验）。**120 ≫ 3**，所以对"同一页被连续帧引用"这一最常见情形，
  阈值本身兜住了 —— 页要空置满 120 帧才会被考虑回收，在飞帧最多 3 帧，不会踩到。
- 因此这**不是**一个会立刻炸的 UAF，而是一个**防护机制名存实亡**的架构缺口：
  真正危险的是 `in_flight` 语义（"有在途帧引用"）与 `live`（"有活子块"）**不是同一回事**。
  一个页可以 `live == 0`（子块都已 free）却仍被上一帧的命令缓冲引用着（延迟销毁尚未发生），
  这正是 `set_page_in_flight` 存在的理由。它现在没被调用 → 这道判断被绕过。
- 另外 `begin_frame` 里 `:1076-1080` 的 `vmaSetCurrentFrameIndex` 同样从未执行，
  VMA 自己的延迟释放也**从未**与引擎帧边界对齐过。

**处置**：Q1 已判归 synccore。我的责任是 **t19 交付后验证"接线后防护真的生效"**，即
确认真实生产路径上 `set_page_in_flight` 被成对调用（置 true / 置 false），
而不是只说"`begin_frame` 接上了"。**这是 §11.1 里"逐条给 R1–R15 结论"的关键前提。**

### 11.3 现有测试**没有**覆盖 `in_flight` 防护（需补用例）

`tests/magicblocks_l1_test.cpp` 里与页回收相关的只有两个用例，都不覆盖本缺口：

| 用例 | 行 | 验证什么 | 是否覆盖 in_flight |
|---|---|---|---|
| `Allocator.SteadyStateFramesHaveZeroVmaCalls` | `:155-194` | 稳态帧 VMA 分配/释放增量 == 0 | ❌ |
| `Allocator.IdlePageEvictedAfterIdleFrames` | `:197-231` | 空置满 120 帧**应当**淘汰 | ❌ |

`IdlePageEvictedAfterIdleFrames` 断言的是"**该淘汰时确实淘汰了**"，方向正好相反于
"**不该淘汰时没有被淘汰**"。**没有任何用例验证"有在途引用时不得回收"**。

t20 要补的用例（`tests/` 下，不碰 `src/features/`）：

- `Allocator.PageNotEvictedWhileInFlight`：`reserve_pages` → 分配 → `free` 子块 →
  `set_page_in_flight(page, true)` → 推进 >120 帧 `begin_frame` → 断言页**仍在册** →
  `set_page_in_flight(page, false)` → 再推进至超阈值 → 断言此时**才**被淘汰。
  这条用例在防护接线前会红、接线后才会绿，是判定 §11.2 是否真修好的**可执行判据**。

### 11.4 Q2 / Q4 的复核脚本（t19 交付后逐条执行，不采信报告）

```powershell
# Q4 硬指标：全仓真调用点必须恰好 2 处，且都在析构函数内
Get-ChildItem -Recurse -Path src -Include *.cpp,*.h |
  Select-String -Pattern 'vk(Device|Queue)WaitIdle\s*\(' |
  Where-Object { $_.Line -notmatch '^\s*//' } |
  ForEach-Object { "{0}:{1}  {2}" -f $_.Filename, $_.LineNumber, $_.Line.Trim() }
# 期望：vulkan_render_device.cpp（~VulkanRenderDevice 内）、vulkan_swapchain.cpp（~VulkanSwapchain 内）
# 若 vulkan_sync_core.cpp 仍出现 → Q4 未落实，报红
```

```powershell
# Q2：submit_and_wait_directed 是否仍拿 current_slot()
Select-String -Path src\backend\vulkan\vulkan_sync_core.cpp -Pattern 'submit_and_wait_directed' -Context 0,25
# 判据：函数体内不得出现 current_slot()；应使用独立的 fence（frame_sync 资源或专用 aux fence）
```

### 11.5 我 t20 交付物的确定形态（先写死，避免临场漂移）

1. **复核报告**：R1–R15 逐条「新异步模型下是否仍安全」+ 29 处 submit 调用点逐条结论。
2. **补的用例**：§11.3 的 `PageNotEvictedWhileInFlight`；stack 侧「上一帧 buffer 在 GPU 仍在读时不被销毁」的回归用例。
3. **前/后基线对照**：t19 交付树上跑一次、我改完后跑一次，贴 `[==========]` / `[  PASSED  ]` 原文，不用 585/574/608 旧数。
4. **Q2 / Q4 / 双通道独立性的实测复核结论**。

### 11.6 当前卡点（截至本节写作）

t19 **仍未交付**：

- `vulkan_allocator.cpp` 最后修改 `15:16:46`，**本轮 t19 全程未触碰** —— L1 一行没改，
  印证 §11.2 的"L1 从未接电"判断。
- `submit_device_frame` 调用点 `vulkan_render_device.cpp:673` **签名已对齐**（已传 `sync_core_.get()`），
  相对上轮快照（`:670` 传旧 `&submit_fences_[]`）**有进展**。
- Q4 的两处 wait-idle **仍在**（`vulkan_sync_core.cpp:327/347`）→ 按队长的裁定属于待删状态。

---

## 12. 第三轮：队长的两处更正 + 我对 `stack_render` 修法的独立核查

### 12.1 更正一：队长关于 `free_bytes` 的保守判断被推翻（队长已确认）

队长原判断：「页淘汰还有个 `free_bytes >= page_size` 前置条件，子块 free 只是记 `live -= n`，
并不一定把空闲字节合并回可分配池 —— 所以现状是『双重沉睡』」。

**经逐层源码验证，该判断不成立。** 三步证据：

| 步 | 事实 | 位置 |
|---|---|---|
| 1 | `create_page` 用**单个覆盖整页的 Slot** 初始化：`offset=0, cap=page_size, in_free_list=true`。新页一建出来就 `free_bytes == page_size`、`live == 0` | `vulkan_allocator.cpp:545-548` |
| 2 | `carve` 按「[前残段][活块][后残段]」重建，**两个残段都 `in_free_list = true`** 回到空闲池 —— 不是"只记 live-=n 不合并" | `vulkan_allocator.cpp:342-378`（残段构造 `:355-361` / `:369-375`） |
| 3 | `release_slot` 执行**左右合并**并把被吞的 slot `erase`，最终整页并回**一个** `cap == page_size` 的块 | `vulkan_allocator.cpp:407-434` |

**结论：L1 页淘汰能力是真的**，`free_bytes >= page_size` 完全可构造，不需要重新排期。

现状的准确表述应当是：**「`begin_frame` 没驱动（沉睡） + `in_flight` 是死代码（防护空转）」**。
淘汰机制本身工作正常 —— `Allocator.IdlePageEvictedAfterIdleFrames:197-231` 一直绿着就是证据。

> 队长已回复接受本更正，并记为「我关于 `free_bytes` 的保守判断是错的」。

### 12.2 更正二：t19 范围收口后的实际靶面

队长把 t19 收口到最小可交付集，并把 `stack_render.cpp:124` 的 UAF **改派回 synccore**
（原派给 synccore → 队长一度考虑复用帧槽环 → 最终要求 synccore 回答「为什么不用帧槽环」）。

对我 t20 的影响：

| 项 | 状态 |
|---|---|
| `stack_render.cpp:124` UAF | **不由我改**。我提供验证用例，验证「它修好了」，不写替代实现 |
| `Allocator.PageNotEvictedWhileInFlight` | **我已写入**（94 行，纯新增），等 t19 自洽后跑 |
| R1–R15 / 29 处调用点复核 | **待 t19 交付后执行** |

### 12.3 我独立核查 `src/primitives/alloc.cpp:183` 帧槽环 —— 队长推断**方向对、结论需修正**

队长质询 synccore 的切入点是：*「你验证过 `src/primitives/alloc.cpp:183` 附近**已经是帧槽环形实例缓冲**。
如果那条路已经存在，`stack_render.cpp:124` 的正确修法很可能是**复用现有帧槽环**。」*

我核查后认为**这个推断需要一处关键修正**：

| 项 | 事实 | 位置 |
|---|---|---|
| `BatchPool` 确实是帧槽环 | 注释原文「帧槽环形实例缓冲。每槽一块独立内存，第一次 `begin_frame` 到该槽时按需分配」；`begin_frame(slot)` 做 `current_slot_ = slot % frame_slots_` | `src/primitives/alloc.cpp:183` / `:213-215` |
| **但它分配的是 CPU 内存** | `::operator new(need, std::align_val_t{kSlotAlign}, std::nothrow)` —— 纯 CPU 堆，**不是 GPU 缓冲** | `src/primitives/alloc.cpp:238-239` |
| 接口自述也明确 | `slot_data()` 注释：「取当前槽块的 **CPU 可写首址**，交给 L1 `IBuffer` 上传」 | `include/magicblocks/primitives/alloc.h:233-234` |
| `StackRender` 实际用的是 GPU 缓冲 | `std::unique_ptr<res::IBuffer> surface_buffer_ / inner_buffer_`；`upload_instances` 里 `device_->create_buffer(bd)` | `src/features/stack/stack_render.cpp:540-541` / `:131` |

**精确结论**：`BatchPool` 是 **CPU 侧**的实例数据环形缓冲，`stack_render.cpp:124` 的 UAF 是 **GPU `VkBuffer`** 的
生命周期问题 —— **`IBatchPool` 本身解决不了这个 UAF**，它管的是「实例数据放哪块 CPU 内存」。

**但这恰恰指出了成本最低的正确修法**，而且这个修法的可行性我也验证过了：

- `IFrameSync` 已经提供了一整套帧槽环所需的 API，**无需任何 L0 改动**：
  `in_flight_count()`（环大小）、`slot_for_frame(frame_index)`（→ 槽位）、`wait_slot(slot)`（复用前等待）、
  `advance_frame_index()` —— 见 `include/magicblocks/device/frame_sync.h:20-31`。
- `StackRenderer` 通过 `device_->frame_sync()` 就能拿到它（`IRenderDevice::frame_sync()`，
  `include/magicblocks/device/render_device.h:59`），`device_` 已经是 `IRenderDevice*`（`stack_render.cpp:532`）。
- 因此正确修法是：把 `surface_buffer_` / `inner_buffer_` 从**单个** `unique_ptr` 改成
  **N 个元素的环**（N = `in_flight_count()`），索引走 `slot_for_frame()`，复用前 `wait_slot()`。
  这正是 `BatchPool` 已验证过的 `slot % frame_slots_` 模式的 GPU 版本，**完全不需要动
  `create_buffer` 的签名、不需要 `RetiredBuffer`、不需要碰任何 L0 基础设施**。

> 这就是队长「影响面不清就回退」那条纪律的实证依据：`RetiredBuffer` 方案改的是
> `IRenderDevice::create_buffer` 这个**全仓基础设施**（`vulkan_render_device.cpp:440` 一行就影响所有调用方），
> 而帧槽环方案只改 `stack_render.cpp` 一个文件的两个成员。**后者影响面小一到两个数量级。**

### 12.4 我 t20 的取证纪律（队长本轮明确要求）

队长对「必红→变绿」叙事给了三种情形的处理口径，我照此执行，不做加工：

| t19 交付后首次跑 `PageNotEvictedWhileInFlight` 的结果 | 我的动作 |
|---|---|
| **红** | 直接存档，这就是活的「必红」证据 |
| **绿** | 用 `git stash` / 临时注释掉 `set_page_in_flight` 的调用点**再跑一次**，验证是否变红；能变红才算拿到判据 |
| **绿且无法通过回退证明** | 判为「**判据不成立**」，如实报告，绝不强行说证据成立 |

同时记队长本轮两句话，作为后续自查口径：

1. **「我给的判断也要你验，不要因为是队长说的就直接采信。」**
   本文已有两次实践：§12.1（`free_bytes`）、§12.3（帧槽环是 CPU 侧非 GPU 侧）。
2. **报告里写清楚实际观察到什么，不要为了凑叙事而加工。**
