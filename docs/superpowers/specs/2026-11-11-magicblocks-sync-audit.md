# MagicBlocks 同步点全量审计（7 处 wait-idle + 全部隐式同步点）

- 日期：2026-11-11
- 作者：survey（t18）
- 性质：**只读审计**。本文未改动任何代码、测试、CMake、门控
- 审计对象：`MagicBlocks/src/backend/vulkan/**` 全部同步原语
- 证据口径：全部结论附 `文件:行号`。查不到的一律写「未确认」，不臆测

---

## 0. 结论摘要

| 项 | 事实 |
|---|---|
| `vkDeviceWaitIdle` | **4 处**（`vulkan_render_device.cpp:66`、`:320`、`vulkan_swapchain.cpp:82`、`:291`） |
| `vkQueueWaitIdle` | **3 处**（`vulkan_draw.cpp:282`、`vulkan_target.cpp:330`、`vulkan_ui_overlay.cpp:626`） |
| `VkSemaphore` | **0 个**。全仓无 `vkCreateSemaphore`，`vkAcquireNextImageKHR` 与 `vkQueuePresentKHR` 的 semaphore 参数全传 `VK_NULL_HANDLE` |
| `VkFence` | **存在两套**：`IFrameSync` 的每槽 fence（`vulkan_frame_sync.cpp`）+ 设备级 `submit_fences_[4]` 池（`vulkan_render_device.h:138-139`） |
| 真正的多帧并行 | **不存在**。`submit_device_frame()` 在**每次 submit 后立即** `vkWaitForFences`（`vulkan_device_frame.cpp:715`），帧与帧严格串行 |
| `IFrameSync` 是否被渲染路径使用 | **没有**。`frame_sync()` / `wait_slot()` / `advance_frame_index()` 在 `src/` 中**零调用**，只有测试调用（`tests/magicblocks_test.cpp:312-352`） |

**一句话**：多帧环形的**数据结构已经存在**（`IFrameSync` + fence 池），但**帧调度逻辑从未接上**。`submit_device_frame` 用"提交后同步等 fence"把每一帧压成串行，再加上 `cleanup()` 在函数返回前销毁全部帧内对象，使得"异步"在当前结构下**做不到**，不是"改个参数就能开"。

### 0.1 对队长背景信息的两处更正（重要）

| 队长的说法 | 审计结论 | 证据 |
|---|---|---|
| `vulkan_swapchain.cpp:291`（`destroy_swapchain`）「被 `rebuild()` 每帧调用」 | **不成立**。`rebuild()` 只在 **OUT_OF_DATE 条件触发**时调用，不是每帧 | `scene_renderer.cpp:513-517`（acquire 报 OutOfDate 才 rebuild）、`:528-531`（present 报 OutOfDate 才 rebuild）、`ui_app.cpp:280`（窗口 resize 事件才 rebuild） |
| 「最开始的设计居然没有多帧并行」 | **准确**，但补充一个事实：`IFrameSync`（含每槽 fence、`slot_for_frame`、`wait_slot`、`in_flight_count`）**已经实现且可工作**，只是渲染路径从未调用它 | 实现在 `vulkan_frame_sync.cpp:8-48`、`vulkan_frame_sync.h`；调用面仅 `tests/magicblocks_test.cpp:312-352`；`src/` 内零命中 |

这两点不改变用户裁定（渲染路径 wait 必须删、必须实现真多帧并行），但会改变工作量估计：**不是"从零建同步设施"，而是"把已有设施接进帧循环，并解掉阻止异步的结构耦合"**。

---

## 1. 七处 wait-idle 逐点剖析

每个点给出：`文件:行号` / 等待语义词 / 实际在等什么 / 不删每帧阻塞量级 / 上游调用链 / 下游依赖 / 可删性判定。

### 1.1 `vulkan_draw.cpp:282` — `vkQueueWaitIdle`

```cpp
281:    r = vkQueueSubmit(ctx.queue, 1, &si, VK_NULL_HANDLE);
282:    if (r == VK_SUCCESS) r = vkQueueWaitIdle(ctx.queue);
283:    vkFreeCommandBuffers(ctx.device, ctx.cmd_pool, 1, &cmd);
```

- **所在函数**：`draw_selftest_grid`（`vulkan_draw.cpp:30`），L0/L2 自检网格绘制
- **实际在等什么**：等**本次提交**在队列上完成。因为 submit 传的 fence 是 `VK_NULL_HANDLE`（`:281`），没有别的回收手段，所以只能用队列级全排空。是"本帧"，但**粒度是整条队列**（会把同队列其它提交一起等）
- **下游依赖**（函数返回前立刻销毁/复用）：
  - `:283` `vkFreeCommandBuffers` —— 命令缓冲必须在执行结束后才能释放
  - `:287-294` 销毁 `pipe` / `layout` / `fb` / `resolve_view` / `view` / `rp` / `vs` / `fs` —— **全部是本次提交正在引用的对象**
- **上游调用链**：
  - `src/cli/main.cpp:151` → `OffscreenTarget::draw_selftest(frame_index)`
  - `src/l2_cli/main.cpp:556` → `OffscreenTarget::draw_selftest(...)`
  - → `vulkan_target.cpp:184` `OffscreenTarget::draw_selftest()` → `draw_selftest_grid(...)`
  - **在帧的哪个阶段**：L0/L2 的 golden 出图旁路（`vulkan_device_frame.h:7` 注释明确「L0/L2 的 golden 只能靠 `draw_selftest()` 这条旁路出图」）。**不在每帧渲染主路径上**，是 CLI/测试的单次出图
- **不删时每帧阻塞量级**：该函数**不每帧调用**（只在 CLI 出图时调用一次）。若按每帧算，等于**整帧 GPU 时间**（含 submit 排队 + 执行），典型软件光栅 320×240 约 5–20 ms 级；真机 GPU 上为数 ms。**结论：这不是每帧性能瓶颈，但它是"每次出图都必须等一次全队列排空"**
- **可删性判定**：**可以改**，但不能简单删。正确做法是把 `VK_NULL_HANDLE` 换成 fence（或复用 `submit_fences_` 池），等**该 fence** 而非整队列。当前因为"提交后立刻销毁自己刚建的 pipeline/framebuffer/render pass"，必须有某种等待。**这条属"应在重构中一并改造"，不属于渲染主路径**

### 1.2 `vulkan_target.cpp:330` — `vkQueueWaitIdle`

```cpp
329:    r = vkQueueSubmit(ctx_.queue, 1, &si, VK_NULL_HANDLE);
330:    if (r == VK_SUCCESS) r = vkQueueWaitIdle(ctx_.queue);
332:  vkFreeCommandBuffers(ctx_.device, ctx_.cmd_pool, 1, &cmd);
```

- **所在函数**：`OffscreenTarget::readback_rgba8`（`vulkan_target.cpp` 读回路径）
- **实际在等什么**：等本次 `vkCmdCopyImageToBuffer` 完成，因为**紧接着要用 CPU 读 mapped 内存**
- **下游依赖**：
  - `:332` `vkFreeCommandBuffers`
  - `:346-352` 从 `mapped` 指针 `memcpy` 出像素到调用方缓冲区
  - `:356` `cleanup_buffer()`
- **上游调用链**：`IRenderTarget::readback_rgba8` 契约 → UI 层 `UiOverlayPass::readback()`（`vulkan_ui_overlay.cpp:635-638`）/ L3 CLI 出图 / 各 golden 用例
- **在帧的哪个阶段**：**读回阶段**，且代码注释与 G8 门控规则都说明"读回必须只在离线/低频路径"（`tools/check_l3_command_parity.py:18-25` 明确 L3 禁止裸 readback）
- **不删时每帧阻塞量级**：**每帧只要读回就必然阻塞整帧**（GPU→CPU 全同步）。这是 CPU 读像素的**固有代价**，不是可以靠 semaphore 消除的
- **可删性判定**：**应当保留其"等待"语义，但可把 `VK_NULL_HANDLE` 改 fence 的定向等待**。**不可改为异步**：CPU 立刻要读这块内存。属"合理等待"，但**不在渲染主路径**（离线路径）
- **注意**：`:334` 的 `cleanup_buffer()` 在失败分支提前返回，与 `:356` 的正常分支重复——两处都调，不是 double-free（函数内应幂等），但值得 owner 复核「未确认」其是否幂等

### 1.3 `vulkan_ui_overlay.cpp:626` — `vkQueueWaitIdle`

```cpp
625:  r = vkQueueSubmit(ctx_.queue, 1, &si, VK_NULL_HANDLE);
626:  if (r == VK_SUCCESS) r = vkQueueWaitIdle(ctx_.queue);
627:  vkFreeCommandBuffers(ctx_.device, ctx_.cmd_pool, 1, &cmd);
628:  vkDestroyFramebuffer(ctx_.device, fb, nullptr);
629:  vkDestroyImageView(ctx_.device, view, nullptr);
630:  if (att_views[1] != VK_NULL_HANDLE) vkDestroyImageView(ctx_.device, att_views[1], nullptr);
```

- **所在函数**：`UiOverlayPass::render_offscreen`（`vulkan_ui_overlay.cpp:463`）
- **实际在等什么**：等 UI 叠加层渲染完成，因为紧接着销毁本帧使用的 framebuffer / image view
- **下游依赖**：`:627` 释放 cmd；`:628-630` 销毁 `fb`、`view`、`att_views[1]`（全部被本次提交引用）
- **上游调用链**：`VulkanFrameRenderer::render_to_memory()`（`vulkan_ui_overlay.cpp:680`）→ `pass_->render_offscreen()`。再往上：UI CLI 出图 / `magicblocks_cli_ui_*` / UI golden 用例
- **在帧的哪个阶段**：**离屏 UI 合成阶段**。注意 `:620` 注释「单次 submit(离屏路径;UI 层 Presenter 不再为它提交)」——这是离屏路径，**不走交换链 present**
- **不删时每帧阻塞量级**：**每帧一次整队列排空**。这一条是**真正在每帧路径上**的（每次 UI 离屏渲染都会走）。阻塞量 = 本帧 GPU 执行时间
- **可删性判定**：**必须删/改**。改法与 1.1 同构：改为按 fence 定向等，或把 `fb`/`view` 纳入帧槽资源池延迟销毁

### 1.4 `vulkan_render_device.cpp:66` — `vkDeviceWaitIdle`（**白名单·保留**）

- **所在函数**：`VulkanRenderDevice::~VulkanRenderDevice()`
- **实际在等什么**：等**全部在飞帧**完成，然后才能安全销毁 fence 池、命令池、分配器、VkDevice
- **下游依赖**：`:70-73` 清资源表 → `:76` `frame_sync_.reset()`（销毁 fence）→ `:79-84` 销毁 `submit_fences_[]` → `:85` 销毁命令池 → `:86` 销毁分配器 → `:96-97` `vkDestroyDevice`
- **保留理由**：见 §6
- **判定**：**保留**

### 1.5 `vulkan_render_device.cpp:320` — `vkDeviceWaitIdle`（`wait_idle()` 虚函数实现）

```cpp
318: Status VulkanRenderDevice::wait_idle() {
320:   VkResult r = vkDeviceWaitIdle(ctx_.device);
```

- **所在函数**：`IRenderDevice::wait_idle()` 的 Vulkan 实现
- **实际在等什么**：全部在飞帧
- **性质**：这是**接口语义本身就是"等空闲"**的显式 API，不是隐式同步。调用方主动要求排空
- **上游调用方**：需逐个确认。`src/` 内直接调用面**未确认**（本次未穷举所有 `wait_idle()` 调用点）
- **判定**：**保留接口，但要审查调用方**。若有渲染主路径调用它，那是调用方的错，应删调用方而不是删实现。**需 owner 补一次调用点普查**（我这次只审了 backend，`wait_idle()` 的语义必须保留）

### 1.6 `vulkan_swapchain.cpp:82` — `vkDeviceWaitIdle`（**白名单·保留**）

- **所在函数**：`VulkanSwapchain::~VulkanSwapchain()`
- **实际在等什么**：等所有已 acquire 的 presentable image 使用完成
- **保留理由**：见 §6。`VUID-vkDestroySwapchainKHR-swapchain-01282` 明确要求
- **判定**：**保留**

### 1.7 `vulkan_swapchain.cpp:291` — `vkDeviceWaitIdle`（`destroy_swapchain`）

```cpp
286: Status VulkanSwapchain::destroy_swapchain() {
287:   views_.clear();
288:   destroy_msaa_images();
289:   images_.clear();
290:   if (swapchain_) {
291:     vkDeviceWaitIdle(ctx_.device);
292:     vkDestroySwapchainKHR(ctx_.device, swapchain_, nullptr);
```

- **实际在等什么**：等所有在飞帧完成，才能销毁交换链
- **上游调用链**（**已更正队长说法**）：
  - `VulkanSwapchain::rebuild()`（`:319-327`）→ `destroy_swapchain()`（`:324`）
  - `rebuild()` 调用点仅 3 处，**全部是条件触发**：
    - `scene_renderer.cpp:515`：`acquire_next_image()` 返回 `OutOfDate` 时
    - `scene_renderer.cpp:529`：`present_current()` 返回 `OutOfDate` 时
    - `ui_app.cpp:280`：窗口 resize 事件
  - **结论：不是每帧调用**。但**一旦触发**（resize 拖动、旋转屏）就是整设备排空
- **判定**：**保留等待语义**（销毁交换链必须有此保证），但**若实现了完整帧同步，可降级为"等全部 in-flight fence"而非 `vkDeviceWaitIdle`**。属低优先级优化，不是"渲染路径必须删"的那一类

### 1.8 七点分类汇总

| # | 位置 | 级别 | 是否每帧路径 | 判定 |
|---|---|---|---|---|
| 1 | `vulkan_draw.cpp:282` | Queue | 否（CLI 出图旁路） | 改 fence 定向等 |
| 2 | `vulkan_target.cpp:330` | Queue | 否（读回离线路径） | 改 fence 定向等（CPU 必须等） |
| 3 | `vulkan_ui_overlay.cpp:626` | Queue | **是**（每次 UI 离屏渲染） | **必须改** |
| 4 | `vulkan_render_device.cpp:66` | Device | 否（析构一次） | **保留**（白名单） |
| 5 | `vulkan_render_device.cpp:320` | Device | 取决于调用方（未确认） | 保留实现，审查调用方 |
| 6 | `vulkan_swapchain.cpp:82` | Device | 否（析构一次） | **保留**（白名单） |
| 7 | `vulkan_swapchain.cpp:291` | Device | 否（仅 OUT_OF_DATE / resize） | 保留语义，可降级为 fence 等 |

> 严格按用户裁定「销毁前的 wait-idle 可以保留，渲染路径上的必须全部删除」：
> **纯销毁路径保留 = #4、#6**；**渲染路径必须处理 = #3（最重）、#1、#2**；**#5、#7 属灰色地带，建议按"销毁/重建语义"保留但审查调用频次**。

---

## 2. 上游调用链与下游依赖（可异步性的唯一判据）

### 2.1 帧渲染主路径实测拓扑

```
[UI 离屏路径]
UiApp / ui_cli
  └─ VulkanFrameRenderer::render_to_memory()           vulkan_ui_overlay.cpp:676
       ├─ UiOverlayPass::render_offscreen()            vulkan_ui_overlay.cpp:463
       │    ├─ vkQueueSubmit(fence=NULL)               :625
       │    ├─ vkQueueWaitIdle                         :626  ← wait #3
       │    ├─ vkFreeCommandBuffers                    :627
       │    └─ vkDestroyFramebuffer/ImageView          :628-630
       └─ UiOverlayPass::readback()                    vulkan_ui_overlay.cpp:635
            └─ OffscreenTarget::readback_rgba8()
                 ├─ vkQueueSubmit(fence=NULL)
                 ├─ vkQueueWaitIdle                   vulkan_target.cpp:330  ← wait #2
                 └─ CPU memcpy 像素

[设备帧路径（L3/L4 场景）]
L3/L4 → VulkanRenderDevice::submit_command_list()      vulkan_render_device.cpp:613
  ├─ fence_slot 轮转，从 submit_fences_[4] 取         :640-641
  └─ submit_device_frame(ctx, *cmd, &stats, &fence)    :642
       ├─ vkAllocateCommandBuffers                     vulkan_device_frame.cpp:221
       ├─ 逐 pass vkCreateImageView/Framebuffer/RPass  :406/425/449/465
       ├─ vkQueueSubmit(fence 有值)                    :711
       ├─ vkWaitForFences(1s 超时)                     :715  ← 隐藏同步点（见 §3.3）
       └─ cleanup(): 销毁全部 views/fb/rp + vkFreeCommandBuffers   :246-256, :728

[交换链路径（上屏）]
scene_renderer.cpp:512  acquire_next_image()           vulkan_swapchain.cpp:356
                          └─ vkAcquireNextImageKHR(semaphore=NULL, fence=NULL)  :387-388
scene_renderer.cpp:527  present_current()              vulkan_swapchain.cpp:425
                          └─ vkQueuePresentKHR(**无 waitSemaphoreCount**)       :428-435
```

### 2.2 下游依赖清单（函数返回前就被销毁的对象）

| 位置 | 被销毁对象 | 被谁引用 |
|---|---|---|
| `vulkan_device_frame.cpp:248-250`（`cleanup()`） | 所有 `VkImageView` | 命令缓冲 render pass 附件 |
| `vulkan_device_frame.cpp:251` | 所有 `VkFramebuffer` | 命令缓冲 `vkCmdBeginRenderPass` |
| `vulkan_device_frame.cpp:252` | 所有自有 `VkRenderPass`（`rp_owned==true`） | 命令缓冲 + `VkPipeline` |
| `vulkan_device_frame.cpp:255` | `VkCommandBuffer` | 刚提交的队列操作 |
| `vulkan_draw.cpp:283-294` | cmd + pipeline + layout + fb + view + rp + 2 个 shader module | 刚提交的队列操作 |
| `vulkan_ui_overlay.cpp:627-630` | cmd + fb + view + att_views[1] | 刚提交的队列操作 |
| `vulkan_target.cpp:332` | cmd | 刚提交的队列操作 |

**这张表就是"不能异步"的根因**：所有帧内对象都在函数返回前销毁，而销毁的前提是"GPU 用完了"。当前用 `wait-idle` / `wait-fence` 换来这个前提。**要实现多帧并行，必须把这些对象从"栈上/函数内"搬到"帧槽池里"延迟销毁**，否则删了等待就是 use-after-free + VUID。

---

## 3. 帧结构缺陷清单

### 3.1 `acquire_next_image()` 不传 semaphore — **确认**

`vulkan_swapchain.cpp:387-388`：

```cpp
const VkResult r = vkAcquireNextImageKHR(ctx_.device, swapchain_, 1'000'000'000ull,
                                         VK_NULL_HANDLE, VK_NULL_HANDLE, &idx);
```

两个参数分别是 `semaphore` 和 `fence`，**都是 NULL**。后果：**acquire 与后续渲染 submit 之间没有任何 GPU 侧的排序**。当前之所以"能跑"，是因为 `submit_device_frame` 每次都同步等 fence，帧间已经串行，碰巧不出错。

`:369-385` 的长注释解释了为什么删掉了 fence：**曾经用 fence 守 acquire 导致稳定 1fps**（fence 建时带 `SIGNALED`，而 `vkAcquireNextImageKHR` 在有图可拿时不保证立即 signal）。这个诊断是对的，但**正解是用 semaphore 而不是 fence**，注释里"acquire 内部实现同步，调用方拿到的图即可直接写"这个结论**对单帧成立、对多帧不成立**。

### 3.2 `present_current()` 不带 wait semaphore — **确认**

`vulkan_swapchain.cpp:428-435`：

```cpp
VkPresentInfoKHR pi{};
pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
pi.swapchainCount = 1;
pi.pSwapchains = &swapchain_;
pi.pImageIndices = &current_index_;
// 不等待信号量:本 SDK 在 submit 时已用 fence 保证顺序,见 vulkan_device_frame.cpp。
const VkResult r = vkQueuePresentKHR(ctx_.queue, &pi);
```

`waitSemaphoreCount` **从未赋值**（默认 0）。`:433` 的注释自我承认"不等待信号量，靠 submit 的 fence 保证顺序"。**这对多帧并行是致命缺陷**：present 可能在渲染完成前发生 → 屏幕撕裂 / 显示未完成内容。这是实现多帧并行时**必须补上的第一条 semaphore**。

### 3.3 `submit_device_frame()` 同步 `vkWaitForFences` — **确认，是串行的根因**

`vulkan_device_frame.cpp:711-720`：

```cpp
vr = vkQueueSubmit(ctx.queue, 1, &si, fence);
if (vr != VK_SUCCESS) { ... }
else {
  vr = vkWaitForFences(ctx.device, 1, &fence, VK_TRUE, kSubmitTimeoutNs);  // :715, 1s 超时
  ...
}
```

`:667-677` 的注释**明确写出**了这个等待存在的理由：

> 「这个等待**必须**保留:紧随其后的 `cleanup()` 会销毁本帧的 framebuffer / imageView / renderPass,调用方在函数返回后也会立刻丢弃命令列表。丢掉等待就是 use-after-free + VUID。」

即：**同步等待是 `cleanup()` 的直接后果**。要并行，二者必须一起改：`cleanup()` 改为"延迟到该槽 fence 完成后再销毁"，等待改为"只等**上一轮同槽**的 fence"。

- **每帧阻塞量级**：等于本帧 GPU 执行时间（全同步）。这是当前 1 GPU 帧 = 1 CPU 帧的严格串行来源
- **超时语义另有一坑**：`kSubmitTimeoutNs = 1s`（`:678`），超时返回 `DeviceLost`。若真改成异步而保留 1s 超时，慢帧会被误判成设备丢失

### 3.4 command buffer 每帧 allocate + 每帧 free — **确认**

- 分配：`vulkan_device_frame.cpp:221` `vkAllocateCommandBuffers(ctx.device, &cai, &cmd)`（每次调用都分配）
- 释放：`vulkan_device_frame.cpp:255` `vkFreeCommandBuffers(...)`（`cleanup()` 内）
- 同理 `vulkan_draw.cpp:251`/`:283`、`vulkan_ui_overlay.cpp`（`cmd` 分配后 `:627` 释放）、`vulkan_target.cpp:269`/`:332`

**为什么这直接堵死并行**：命令缓冲在提交后立刻被回收。若两帧同时在飞，第二帧分配时**可能拿到第一帧仍在使用的内存**（或驱动阻塞）。必须改为**每槽一个持久命令缓冲，帧首 `vkResetCommandBuffer`**。

- **命令池已具备条件**：`vulkan_render_device.cpp:247` 已设置 `VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT`，**因此支持单独 reset 某个缓冲**，做帧槽池**不需要改池的 flags**。这是好消息
- 但命令池是**单个共享池**（`vulkan_render_device.cpp:249`），多线程录制会冲突；**单线程多帧在飞则无碍**（因为 reset 是逐缓冲的）

### 3.5 帧结构缺陷汇总表

| # | 缺陷 | 位置 | 并行必需修复 |
|---|---|---|---|
| F1 | acquire 无 semaphore | `vulkan_swapchain.cpp:387-388` | 是（需 image_available 信号量） |
| F2 | present 无 wait semaphore | `vulkan_swapchain.cpp:428-435` | 是（需 render_finished 信号量） |
| F3 | submit 后同步等 fence | `vulkan_device_frame.cpp:715` | 是（改为等上一轮同槽） |
| F4 | cleanup 在返回前销毁全部帧内对象 | `vulkan_device_frame.cpp:246-256, :728` | 是（改为帧槽延迟回收） |
| F5 | cmd 每帧 alloc + free | `vulkan_device_frame.cpp:221/255` 等 | 是（改为每槽持久缓冲） |
| F6 | `IFrameSync` 未接入渲染路径 | `src/` 内零调用 | 是（接进帧循环） |

---

## 4. 资源生命周期阻塞点（逐处列出）

`vulkan_device_frame.cpp` 的 `cleanup()`（`:246-256`）在 **`submit_device_frame` 返回前**（`:728`）无条件执行，销毁：

| 行号 | 销毁对象 | 创建于 | 生命周期问题 |
|---|---|---|---|
| `:248-250` | `p.views` 全部 `VkImageView` | `:406`（color）、`:425`（depth）、`:449`（resolve） | 每 pass 每帧新建，供 framebuffer 引用 |
| `:251` | `p.fb` `VkFramebuffer` | `:465` | 每 pass 每帧新建 |
| `:252` | `p.rp` `VkRenderPass`（仅 `rp_owned`） | `:192` `create_pass()` | 每 pass 每帧新建（可缓存，见下） |
| `:255` | `cmd` 命令缓冲 | `:221` | 每帧新建 |

补充：`rp_owned == false` 的 pass **不销毁**（`:242` 注释：「借来的：复用管线的内建 pass，生命周期归 `VulkanPipeline`」）。这条分支是安全的。

**额外阻塞点（非 `cleanup` 内）**：

| 位置 | 对象 | 处置 |
|---|---|---|
| `vulkan_draw.cpp:287-294` | pipeline / layout / fb / view×2 / rp / shader module ×2 | 函数返回前销毁 |
| `vulkan_ui_overlay.cpp:628-630` | fb / view / att_views[1] | 函数返回前销毁 |
| `vulkan_render_device.cpp:649` | `cmd->discard_vk_side_channel()` | 提交后立刻丢弃 `begin_pass` 期间建的 Vk 对象（`:643-648` 注释说明这是**必需的**，否则 `VUID-...-00059`） |
| `vulkan_swapchain.cpp:287-289` | `views_` / `msaa_images_` / `images_` | `destroy_swapchain` 内，`vkDeviceWaitIdle` 之前就 clear 了 vector（**注意顺序**：`views_.clear()` 在 `:291` 的等待**之前**） |

> **`vulkan_swapchain.cpp:287-291` 的顺序值得 owner 复核**：`views_.clear()` 与 `destroy_msaa_images()` 都在 `vkDeviceWaitIdle` **之前**执行。虽然 `views_` 是 `unique_ptr<SwapchainImageView>`、其析构只 destroy `VkImageView`（不碰 VkImage），但它销毁 image view 时 GPU 可能仍在用。**这属于潜在 VUID，写「未确认」**——本次未实测验证是否触发。

**改造方向（供 owner 参考，非本文实施）**：把 `PassResources` 从栈上 `std::vector` 搬到**帧槽**里，`cleanup()` 改为 `defer_destroy(slot)`，由 `wait_slot(slot)` 之后统一回收。`VkRenderPass` 可进一步按 `PassShape` 做 map 缓存（同 shape 复用），这能顺带砍掉每帧 `vkCreateRenderPass` 的开销。

---

## 5. subpass 依赖现状分析

### 5.1 现状

`vulkan_device_frame.cpp:174-190`：

```cpp
VkSubpassDependency dep{};
dep.srcSubpass = VK_SUBPASS_EXTERNAL;
dep.dstSubpass = 0;
dep.srcStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
dep.dstStageMask  = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
dep.srcAccessMask = 0;
dep.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
dep.dependencyFlags = 0;

ci.subpassCount = 1;        // :187
ci.dependencyCount = 1;     // :189
ci.pDependencies = &dep;
```

**确认队长说法**：只有**一条** `EXTERNAL → 0`，且 `subpassCount == 1`（**没有子 subpass**，所以"子 subpass 间依赖"这一问题在当前结构下**不存在**）。

### 5.2 缺失的依赖

| 缺失项 | 为什么需要 | 缺的后果 |
|---|---|---|
| **`0 → EXTERNAL` 出口依赖** | 保证 render pass 末尾的 attachment 写入对后续读者（present engine、下一次同一图像的采样/传输）可见 | 当前**完全缺失**。present 之后立刻用这张图，或下一帧同图复用，存在**写后读未同步**。这一条在多帧并行下会**直接变成可见的图片撕裂/闪帧** |
| **acquire → 首 pass 的布局转换依赖** | 交换链图像 acquire 后处于 `PRESENT_SRC_KHR`，render pass 会把它转到 `COLOR_ATTACHMENT_OPTIMAL` | 当前依赖只覆盖 `COLOR_ATTACHMENT_OUTPUT` 阶段，`initialLayout` 的转换由驱动的隐式 barrier 完成。**在无 semaphore 时 acquire 与本 pass 无 GPU 排序** —— 见 F1 |
| **深度附件写依赖** | `att[1].finalLayout = VK_IMAGE_LAYOUT_GENERAL`（`:136`），下一 pass `LOAD` 时要读 | `dstAccessMask` 只有 `COLOR_ATTACHMENT_WRITE`，**未含 `DEPTH_STENCIL_ATTACHMENT_WRITE`**。同帧多 pass（F-VX 的表面→内面→网格指示）依赖 layout 自洽（`:129-135` 注释），**但缺显式深度依赖** |

> **重要**：`:171-173` 的注释明确写了"没有它，loadOp=CLEAR 与随后的着色写入之间没有任何同步点…… SwiftShader 上表现为整片被清掉，**而且不会报错**"。这说明**现有这条依赖是踩坑换来的**，改造时**不能删**，只能**补出口依赖**。

### 5.3 建议补的依赖形态（供 owner 参考）

```
入口（保留现有）: EXTERNAL → 0
   srcStageMask  = COLOR_ATTACHMENT_OUTPUT | EARLY_FRAGMENT_TESTS | LATE_FRAGMENT_TESTS
   dstStageMask  = COLOR_ATTACHMENT_OUTPUT | EARLY_FRAGMENT_TESTS | LATE_FRAGMENT_TESTS
   srcAccessMask = 0
   dstAccessMask = COLOR_ATTACHMENT_WRITE | DEPTH_STENCIL_ATTACHMENT_WRITE

出口（新增）: 0 → EXTERNAL
   srcStageMask  = COLOR_ATTACHMENT_OUTPUT | LATE_FRAGMENT_TESTS
   dstStageMask  = BOTTOM_OF_PIPE (或 COLOR_ATTACHMENT_OUTPUT，若同队列继续用该图)
   srcAccessMask = COLOR_ATTACHMENT_WRITE | DEPTH_STENCIL_ATTACHMENT_WRITE
   dstAccessMask = 0
```

`VkPipeline` 侧的 `vulkan_pipeline.cpp` 有独立 `create_render_pass()`，其依赖形态**本次未逐条比对**，写「未确认」；但 `vulkan_command.cpp:598-601` 注释提到两处必须保持 `finalLayout` 契约一致。

---

## 6. wait-idle 保留白名单

严格按用户裁定「销毁前的 wait-idle 可以保留」。白名单**仅两处**：

### 6.1 `vulkan_render_device.cpp:66` — `~VulkanRenderDevice()`

**为什么安全且必要**：
1. 这是**设备的最后一道关**。紧随其后销毁的是 VkDevice 的**子对象**：fence 池（`:79-84`）、命令池（`:85`）、VMA 分配器（`:86`）、立方体采样器缓存（`:96`）、VkDevice 本身（`:97`）。任何一个仍有在飞工作都会触发 `VUID-vkDestroyDevice-device-05137`（leaked objects）或直接崩溃
2. **频率 = 1 次/设备生命周期**，对帧率零影响
3. `:76-78` 的注释已记录过事故：「逐帧提交的 fence 池同样必须在 VkDevice 之前销毁,否则 VUID。上面那句 vkDeviceWaitIdle 已保证没有在飞的帧」——**这个顺序是有事故背书的**

### 6.2 `vulkan_swapchain.cpp:82` — `~VulkanSwapchain()`

**为什么安全且必要**：
1. 规范硬要求：`:76-79` 注释引用 `VUID-vkDestroySwapchainKHR-swapchain-01282`「All uses of presentable images acquired from swapchain must have completed execution」
2. present 只是把图交给显示引擎，**GPU 可能仍在用它**。没有这道等待就是 UB
3. 频率 = 1 次/交换链生命周期

### 6.3 灰色地带（建议保留语义但可优化，**不在白名单内**）

| 位置 | 建议 | 理由 |
|---|---|---|
| `vulkan_render_device.cpp:320` `wait_idle()` | **保留实现**，审查调用方 | 接口语义就是"等空闲"；但若有每帧调用者，是调用方的错 |
| `vulkan_swapchain.cpp:291` `destroy_swapchain()` | 可降级为"等全部 in-flight fence" | 是销毁路径（非渲染路径），但**不是白名单条目**，因它可能被 resize 触发 |

---

## 7. 建议的 in-flight 帧数与每槽同步对象

### 7.1 建议 in-flight 帧数：**2**（与现有 `DeviceDesc::in_flight_frames` 默认值一致）

证据与理由：

- 默认值已是 2：`include/magicblocks/device/types.h:70` `u32 in_flight_frames = 2;`，且 `render_device.cpp:104-107` **强制校验必须是 2 或 3**
- `frame_sync.h:19-20` 同样规定「必须是 2 或 3」
- **建议取 2 而非 3**：
  1. 当前 `core` 路径（L1 分页分配器）对在飞帧资源有约束，2 帧是**最小可行的并行度**，能覆盖"CPU 录制第 N+1 帧时 GPU 还在画第 N 帧"这一唯一有价值的重叠
  2. 3 帧需要 3 倍帧槽资源（framebuffer / image view / cmd buffer），而**本项目主要跑 SwiftShader 软件光栅**，多一帧的显存/内存代价换不来吞吐
  3. 交换链图像数需 ≥ in-flight 帧数才能不饥饿；`vulkan_swapchain.cpp:274-282` 按 `images_.size()` 建视图，**未确认**其典型值是否 ≥3（本次未实测驱动返回的 imageCount）—— **取 2 最保守**
- **保留 3 的可选性**：接口已允许，实现不应硬编码 2

### 7.2 每槽需要的同步对象

| 对象 | 数量/槽 | 总计（2 槽） | 用途 | 现状 |
|---|---|---|---|---|
| `VkSemaphore`（image_available） | 1 | 2 | `vkAcquireNextImageKHR` signal → 帧 submit 的 wait | **0，需新建** |
| `VkSemaphore`（render_finished） | 1 | 2 | 帧 submit signal → `vkQueuePresentKHR` 的 wait | **0，需新建** |
| `VkFence`（frame in flight） | 1 | 2 | 帧 submit 完成 → 下一轮该槽复用前 `wait` | 已有 `IFrameSync`（`vulkan_frame_sync.cpp:8-48`），**未接入** |
| `VkCommandBuffer` | 1 | 2 | 每槽持久，帧首 `vkResetCommandBuffer` | 现状每帧 alloc/free（`vulkan_device_frame.cpp:221/255`） |
| 帧槽资源池（`VkFramebuffer`/`VkImageView`/`VkRenderPass`） | 按 pass 数 | 2× 当前 | 延迟到该槽 fence 完成后回收 | 现状 `cleanup()` 立即销毁 |

**为什么 acquire 和 present 要分开两个 semaphore 池**：
- acquire 的 semaphore 由 `vkAcquireNextImageKHR` signal、由帧 submit 消费
- present 的 semaphore 由帧 submit signal、由 `vkQueuePresentKHR` 消费
- 二者**配对对象不同**（一个是 acquire 引擎，一个是 present 引擎），不能共用一个。若共用，同一槽下一轮的 acquire 会覆盖尚未被 present 消费的信号 —— 这是经典的 semaphore 复用错误

**注意 `submit_fences_[4]` 与 `IFrameSync` 是两套 fence**：`vulkan_render_device.h:138-139`（4 槽）与 `vulkan_frame_sync.cpp`（2/3 槽）职责重叠。**重构时应合并为一套**，否则两套 fence 的等待顺序难以推理。`kSubmitFenceSlots = 4` 与 `in_flight_frames ∈ {2,3}` **不是同一数量**，这是一个现存的不一致。

### 7.3 同步对象必须挂在哪（**关键约束**）

**必须挂在 `VulkanRenderDevice` 成员上，生命周期严格等于 VkDevice。**

正面证据：`vulkan_render_device.h:129-139`

```cpp
// 为什么放在这里而不是 VulkanContext 里:VulkanContext 是纯句柄容器,会被
// 广泛按值传递;把需要 vkCreateFence/vkDestroyFence 的拥有型资源塞进去,
// 会让"谁负责销毁"变得含糊。放在 device 里则生命周期天然等于 VkDevice。
//
// 为什么不是 thread_local 缓存:VkDevice 句柄地址会被驱动复用 —— 销毁旧设备
// 再建新设备,新设备完全可能拿到同一个地址,"地址相同"会被误判成"同一个设备",
// 于是拿已销毁的 VkFence 去 vkResetFences → 无效对象 + 崩溃(实测过一次)。
```

**这条注释就是用户提到的"thread_local 版因 VkDevice 句柄地址被驱动复用,炸掉 106 条用例"事故的现场记录**。设计要求：

1. **不要 `thread_local`**：VkDevice 句柄地址被驱动复用，"地址相同"≠"同一个设备"。全仓仅剩这 1 处 `thread_local` 字样且是注释（grep `thread_local` 命中 1 条，即 `vulkan_render_device.h:135`）
2. **不要塞进 `VulkanContext`**：它按值广泛传递，拥有型资源放进去会让所有权含糊（同上注释）
3. **析构顺序**：frame sync（含 semaphore/fence）必须**早于** VkDevice 销毁。现有 `~VulkanRenderDevice` 已有正确骨架（`:76` `frame_sync_.reset()` 在 `:97` `vkDestroyDevice` 之前），新增的 semaphore 必须**遵循同样顺序**放在同一批
4. **不要用静态/进程级缓存键控 VkDevice 地址**：同上理由

**建议落点**（形态，非代码）：

```
class VulkanRenderDevice {
  std::unique_ptr<VulkanFrameSync> frame_sync_;   // 已有，扩展为含 semaphore
  // 新增：每槽 semaphore（VkSemaphore image_available_[N]; VkSemaphore render_finished_[N];）
  // 新增：每槽 cmd buffer + 帧槽资源池
};
// 析构：先 destroy semaphores / fences，再 destroy cmd pool / allocator，最后 vkDestroyDevice
```

---

## 8. 与验收标准的差距（事实陈述）

用户给的验收：**零 VUID + 586/586 + 609/609 + 真机实测**。

本次实测基线（`build-survey`，本审计前测得）：

| 指标 | 当前值 | 目标 | 差距 |
|---|---|---|---|
| gtest 用例数 | `585 tests from 89 test suites ran` | 586 | **-1**（重构需净增 1 条用例） |
| gtest 通过 | `[  PASSED  ] 574 tests.` | 586 | **-12**（含现存 11 条红 + 新增 1 条） |
| CTest 注册数 | `Total Tests: 608` | 609 | **-1** |
| VUID | **未确认**（本次只做静态审计，未跑校验层统计） | 0 | 未测 |

**必须说明**：本次审计是**静态代码审计**，未运行校验层、未做真机实测。「零 VUID」与「真机实测」两项**我无法在当前范围内确认**，写「未确认」。重构后必须由 owner/qa 用 `--enable_validation` 跑全量并统计 `vk-ERROR` 数量。

> 另注：现存 11 条红条（Sky ×9、MSAA ×2）与本次同步重构**无直接因果关系**，但 Sky 那 9 条的根因是 `reserve_pages` 页预算不足（见 `2026-11-11-magicblocks-final-status.md` §2.3.1），重构若改动分配器帧边界挂点（`IFrameSync::advance_frame_index` 是"L1 VMA 帧边界挂点"，`frame_sync.h:28`）**可能影响它们**，需回归确认。

---

## 9. 未确认项清单（诚实披露）

以下项本次**未能查实**，不做推测：

1. **`wait_idle()`（`vulkan_render_device.cpp:318`）的全部调用点** —— 本次只审 backend，未穷举 `src/` 与 `tests/` 中所有 `IRenderDevice::wait_idle()` 调用。若渲染主路径有调用者，需单列。
2. **`vulkan_pipeline.cpp` 的 `create_render_pass()` 依赖形态** —— 未逐条比对，可能有独立的一套依赖配置。
3. **`cleanup_buffer()` 是否幂等** —— `vulkan_target.cpp:334` 与 `:356` 两处调用，未确认重复调用安全。
4. **交换链典型 imageCount** —— `vulkan_swapchain.cpp:274-282` 按实际数量建视图，未实测 SwiftShader 返回值，因此无法确证 in_flight=2 是否会导致饥饿。
5. **零 VUID 现状** —— 未运行校验层统计，无法给出当前 VUID 数量。
6. **真机实测** —— 未做。
7. **`vulkan_swapchain.cpp:287-291` 的销毁顺序是否触发 VUID** —— `views_.clear()` 先于 `vkDeviceWaitIdle`，理论上存在风险，未实测验证。
8. **`vkAcquireNextImageKHR` 在无 semaphore 时的多帧行为** —— 未实测；当前单帧串行下不暴露问题。

---

## 10. 结论与给 owner 的重构要点

1. **不是从零造轮子**：`IFrameSync` + fence 池已存在，只是未接入。核心工作是**接线 + 解耦**。
2. **串行的根因是 `submit_device_frame` 的同步 `vkWaitForFences`（`:715`）与紧随的 `cleanup()`（`:728`）**。二者必须**同时改**：等待改为等上一轮同槽，`cleanup` 改为帧槽延迟回收。只删等待 = use-after-free；只改 cleanup = 等待无意义。
3. **两条 semaphore 是必须品**（acquire 一条、present 一条，不能共用），当前**零个**。
4. **命令缓冲改为每槽持久**，命令池已带 `RESET_COMMAND_BUFFER_BIT`，**不需要改池**。
5. **subpass 依赖要补出口项**，现有入口那条是踩坑换来的，**不要删**。
6. **白名单严格只有 2 处**：`vulkan_render_device.cpp:66`、`vulkan_swapchain.cpp:82`。
7. **同步对象挂 device 成员，绝不 thread_local**（事故现场：`vulkan_render_device.h:135-137`）。
8. **两套 fence 池需合并**（`submit_fences_[4]` vs `in_flight 2|3` 数量不一致）。
