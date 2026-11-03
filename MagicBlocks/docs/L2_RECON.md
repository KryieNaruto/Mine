# MagicBlocks L2 —— 开工前侦察报告（L2_RECON）

> 用途：L2（`scene/` 场景 / 渲染图 / 命令录制回放）开工前的**只读侦察结论**，每条都带
> `文件:行` 证据。所有路径**相对仓库根** `D:\workspace\myspace\Mine`。
> 权威顺序仍按 `MagicBlocks/docs/ARCHITECTURE.md` 开头：**spec > 计划 > 索引 > 注释**；
> 本文件只负责"代码现在到底是什么样，以及它允不允许 S1–S9"。
>
> 侦察时间：2026-11-03。基线实测：`ctest --test-dir MagicBlocks\build -N` = **99 项**；
> 8 个门控 CTest 全 Passed；抽跑 18 个与 L2 相关的既有用例全 Passed。
> 侦察方式：`read`/`grep` + 只读命令（`git status`、`ctest -N`、`ctest -R gate`、
> `python tools/check_{no_geometry_shader,layer_purity,no_raw_alloc,ui_no_readback,ui_single_submit}.py --root MagicBlocks`）。
> **未修改任何源码或构建产物**；唯一写入 = 本文件。

---

## 0. 十件最决定 L2 设计的事（TL;DR）

| # | 结论 | 一句话影响 |
|---|---|---|
| 1 | 录制流在**后端私有** `backend::RecordedCommand`，公共面 `ICommandList` 只有 11 命令 + `command_count()`，**没有任何读回口**（`MagicBlocks/include/magicblocks/resources/command.h:98-149`，`MagicBlocks/src/backend/vulkan/vulkan_command.h:64-79,123`） | S4/S6/S7 的**唯一阻塞点**：要么给 L1 公共头加只读视图，要么在 `src/scene/` 自建录制代理——**二者必选一**，见 §4 |
| 2 | `IPipeline::hash()` 是**纯函数** `hash_pipeline_desc(desc)`：固定初值 + 字段顺序固定 + 只对 SPIR-V 取首/中/尾 4 个字指纹（`MagicBlocks/src/backend/vulkan/vulkan_pipeline.cpp:178-249,427`） | **跨进程稳定**，可安全进命令流与命令哈希；但**不含附件形状**（`AttachmentShape` 不参与哈希），见 §3 |
| 3 | `RenderPassDesc` 里 `IRenderTarget*` 是**裸指针**，`RecordedCommand` **不记录它**（`command.h:45`，`vulkan_command.cpp:382-512` 只记 area/clear/flag） | 序列化天然不需要指针；**回放必须由调用方传 target**，`IRenderTarget` 也**没有稳定 id**（`MagicBlocks/include/magicblocks/device/render_target.h:24-39`） |
| 4 | `IRenderTarget` 的公共面只有 `kind/extent/color_format/samples/两个 AttachmentView/readback_rgba8`——**无 id、无句柄、无 backend 页号** | 想序列化"pass 写哪个附件"只能用**语义描述**（格式/尺寸/深度），或在 L0 加 id 接口；见 §5 |
| 5 | UI 层的 `scene_stub.h` **只被 `router.h` 一处 include**，实体只在 `namespace magicblocks::ui::stub`；UI 层实现**零** `ui::stub::IScene` 用法，只有测试写了一个 fake（`MagicBlocks/include/magicblocks/ui/router.h:23,30-38`，`MagicBlocks/tests/ui_test.cpp:121-136`） | 替换成本**远低于计划预估**：stub 头改成"逐名 `using` 转发"后，`router.h` 与 `ui_test.cpp` **一行都不用改**（`ui::stub::X` 与 `scene::X` 变成同一类型），见 §6.3 |
| 6 | 命令流**不进 GPU**：设备态 `bind_vertex_buffer` / `bind_index_buffer` / `draw*` 都不发 `vkCmd*`，`vkCmdBindVertexBuffers` 全仓 0 处（`vulkan_command.cpp:200-201`，grep 全仓仅注释提及） | L2 回放**当前的"落盘"意义 = 命令流比对**，不是"重放到 GPU 出图"；真出图仍走 `UiOverlayPass`/`draw_selftest` 的独立通路，见 §2.3 |
| 7 | 软件态 `VulkanCommandList`（默认构造）**可完整录制+校验**，`submit_and_wait()` 只复位并把 `recorded_` 清空（`vulkan_command.cpp:76-89,581-592`） | **capture 必须在 `submit_and_wait()` 之前**；S4/S6/S7 的全部用例可以跑在无 Vulkan 设备上，但用 `IRenderDevice::create_command_list()` 拿到的是**设备态**，公共面无参数可选软件态（见 §1.3） |
| 8 | `allocate()` **绝不**自己触发 VMA，桶里没空位就 `OOM("请先 reserve_pages()")`；设备初始化**不预取任何页**（`initial_pages_per_kind = 0` 默认，`MagicBlocks/src/backend/vulkan/vulkan_render_device.cpp:173-175`，`vulkan_allocator.h:73`） | "稳态帧 0 次 VMA"要求 L2 **只在 init 建资源**；`reserve_pages` 可经 `IRenderDevice::memory_allocator()`（`device/render_device.h:42`）在 init 阶段做，不违反 G4（§11 G-6） |
| 9 | 既有 99 个 CTest = 90 gtest + 8 门控 + 6 CLI（`MagicBlocks/CMakeLists.txt:228-284`）；门控**自测**在 tempfile 假项目里跑，**不受** L2 新文件影响；但 `check_deps_tests` 硬断言 `tools/tests` 恰好 12 个 `test_*.py`（`tools/check_deps_tests.py:22-23`） | L2 若新增门控脚本+自测，必须同步改 `--expect-files` 默认值，否则 `magicblocks_gate_deps_tests` 直接红 |
| 10 | `FrameSample.vma_alloc_count` 在 `UiApp` 里是**硬编码 0**，不是真读分配器（`MagicBlocks/src/ui/ui_app.cpp:215`） | `ui_Perf.SingleSubmitAndZeroAllocAreEnforced` 目前**测不出** L2 是否偷偷分配；L2 的"M5 零 VMA"必须**自己**用 `AllocatorStats` 差分断言 |

---

## 1. ICommandList：11 个方法、录制期校验、软件态语义

### 1.1 公共接口（`MagicBlocks/include/magicblocks/resources/command.h`）

| # | 签名 | 行 |
|---|---|---|
| 1 | `virtual Status bind_pipeline(IPipeline* pipeline, u64 pipeline_hash) = 0;` | :105 |
| 2 | `virtual Status bind_vertex_buffer(const ResourceId& buffer, u32 binding = 0, u64 offset = 0) = 0;` | :108-109 |
| 3 | `virtual Status bind_index_buffer(const ResourceId& buffer, IndexFormat format, u64 offset = 0) = 0;` | :110-111 |
| 4 | `virtual Status draw(const DrawArgs& args) = 0;` | :114 |
| 5 | `virtual Status draw_indexed(const DrawArgs& args) = 0;` | :115 |
| 6 | `virtual Status draw_instanced(const DrawArgs& args) = 0;` | :116 |
| 7 | `virtual Status push_constants(u32 stage_mask, u32 offset, u32 size_bytes, const void* data) = 0;` | :121-122 |
| 8 | `virtual Status set_viewport(const Viewport& viewport) = 0;` | :125 |
| 9 | `virtual Status set_scissor(const ScissorRect& rect) = 0;` | :126 |
| 10 | `virtual Status begin_pass(const RenderPassDesc& pass) = 0;` | :130 |
| 11 | `virtual Status end_pass() = 0;` | :131 |

非命令查询：`recording()` / `in_pass()` / `command_count()`（:134-138）、`reset()`（:141）、
`submit_and_wait()`（:145）。**"恰好 11 类命令"是硬裁定**（`command.h:3-9`，rulings 裁定 1），
`Command.RecordsAllOps` 断言 `command_count() == 11`（`MagicBlocks/tests/magicblocks_l1_test.cpp:602`）。

### 1.2 录制期校验（`MagicBlocks/src/backend/vulkan/vulkan_command.cpp`）

| 规则 | 证据 |
|---|---|
| `bind_pipeline`: pipeline 为空 → Invalid；hash 为 0（且 `pipeline->hash()` 也为 0）→ Invalid | `:162-165` |
| 设备态 bind_pipeline 只认 `VulkanPipeline` 实现，否则 `Unsupported`（不静默不绑） | `:167-179` |
| `bind_vertex/index_buffer`: 句柄非法 → Invalid；`offset >= buffer.size` → Invalid | `:195-199, :214-218` |
| **bind_* 在设备态不发 `vkCmdBindVertexBuffers`**（只记句柄） | `:200-201` |
| `draw`: 未 bind_pipeline / 未 in_pass → Invalid（`check_draw_state`）；`vertex_count==0` / `instance_count==0` → Invalid | `:230-236` |
| `draw_indexed`: `index_count==0` / `instance_count==0` → Invalid | `:247-253` |
| `draw_instanced`: `instance_count <= 1` → Invalid（"等于 1 请用 `draw`"） | `:264-275` |
| `push_constants`: 未绑管线 → Invalid；`stage_mask` 只允许 `Vertex(0x1)|Fragment(0x10)`；`size_bytes==0` / 非 4 的倍数 / `data==nullptr` / `offset+size > push_constant_bytes` → Invalid | `:289-304` |
| `begin_pass`: `target==nullptr` → Invalid；上一个 pass 未 end → Invalid；`area` 宽高 0 → Invalid；`ax+aw>ext.width || ay+ah>ext.height` → Invalid（不静默裁剪） | `:382-402` |
| `end_pass`: 不在 pass 内 → Invalid | `:515-516` |
| `submit_and_wait`: 还有未 end_pass → Invalid | `:577-579` |

### 1.3 软件态 vs 设备态（`VulkanCommandList`）

| | 软件态 `VulkanCommandList()` | 设备态 `VulkanCommandList(ctx)` |
|---|---|---|
| 构造 | `MagicBlocks/src/backend/vulkan/vulkan_command.cpp:76` | `:78` |
| `init()` | `recording_ = true`，不建 VkCommandBuffer（`:84-89`） | `ensure_vk_command_buffer()`（`:91-102`） |
| 录制 | 全走校验 + `recorded_` 追加 | 同 + 真实 `vkCmdBeginRenderPass` / `vkCmdBindPipeline`（`begin_pass :410-494`、`bind_pipeline :178`） |
| `submit_and_wait()` | `++submit_count_`，销毁 cb，**清空 `recorded_`/`command_count_`**，`recording_` 保持（`:581-592`） | 真 `vkEndCommandBuffer` + fence 提交 + 同样**清空 `recorded_`**（`:594-631`） |
| `reset()` | `recording_ = !has_ctx_` = true（`:538-546`） | `recording_` 置 false |

**给 L2 的三条硬结论**
1. `IRenderDevice::create_command_list()` 恒返回**设备态**（`MagicBlocks/src/backend/vulkan/vulkan_render_device.cpp:266-272`，
   公共面无 factory 参数可选软件态）→ 若 L2 要"无设备跑 S4/S6"，必须允许调用方传入
   **自己构造的 `ICommandList*`**，而软件态列表只能用后端私有头构造
   （`VulkanCommandList()`，`src/backend/vulkan/vulkan_command.h:84`）。这意味着 **L2 的
   "软件态列表"用例要么是 `tests/` 里 include 后端私有头的测试 TU（现有测试就是这么做的：
   `tests/magicblocks_l1_test.cpp:604` `dynamic_cast<backend::VulkanCommandList*>`），
   要么走 §4 的路线 B（L2 自带代理）。**`src/scene/**` 自己绝不能 include 后端头**。
2. **capture 必须在 `submit_and_wait()` 之前**，否则流已被清空。
3. 设备态首次 `begin_pass` **不置 `recording_`**，只有 `has_vk_pass_target_` 为真（即 target 是
   `OffscreenTarget`）时才置（`vulkan_command.cpp:410-423`）。传别的 `IRenderTarget` 实现时设备态
   会一直 `recording()==false` → 后续 `push_constants` 等不受影响，但 `recording()` 断言不可信。

### 1.4 录制流的内部表示（回答问题 a）

`MagicBlocks/src/backend/vulkan/vulkan_command.h:64-79`：

```cpp
struct RecordedCommand {
  CommandOp op = CommandOp::BindPipeline;
  ResourceId resource{};       // 涉及资源句柄（M3：偏移而非指针）
  u64 packed_resource = 0;     // pack(resource)；0 = 不适用/不可打包
  bool packed_ok = false;
  u64 pipeline_hash = 0;       // BindPipeline 用
  u32 a = 0; u32 b = 0; u32 c = 0;   // 通用整数参数
  int32_t i = 0;               // vertex_offset
  float f[7] = {};             // viewport/clear 等
  u32 pass_area[4] = {};
  u32 width = 0, height = 0;   // set_scissor
  std::vector<std::uint8_t> payload;  // push_constants 字节
};
```

- 操作码表 `enum class CommandOp : uint8_t` 共 11 项（`:47-59`），含 `command_op_name()`（`:61`，实现
  `vulkan_command.cpp:46-72`）。
- 取流口（**全部在后端私有头**）：`const std::vector<RecordedCommand>& recorded() const`（`:123`）、
  `op_count(op)`（`:125`）、`resource_packed(op)`（`:129`）、`bound_pipeline_hash()`（`:134`）、
  `last_pass_desc()`（`:136`）、`submit_count()`（`:131`）。
- 文件头注释明确写了"命令流可以整块序列化(L2)"与"L2 的命令流格式就按这张表发号"
  （`vulkan_command.h:8-10, 46`）。

**每个 op 实际填了哪些字段**（`vulkan_command.cpp`，这是序列化 schema 的事实来源）：

| op | 填入字段 | 行 |
|---|---|---|
| BindPipeline | `pipeline_hash` | :185-188 |
| BindVertexBuffer | `resource`, `packed_resource/packed_ok`, `a=binding`, `b=(u32)offset` | :203-208 |
| BindIndexBuffer | `resource`, packed, `a=(u32)IndexFormat`, `b=(u32)offset` | :219-224 |
| Draw | `a=vertex_count`, `b=instance_count`, `c=first_vertex`, `i=vertex_offset` | :237-243 |
| DrawIndexed | `a=index_count`, `b=instance_count`, `c=first_index`, `i=vertex_offset` | :254-260 |
| DrawInstanced | `a=vertex_count`, `b=instance_count`, `c=first_vertex`, `i=vertex_offset`, **`f[0]=first_instance`（float 存整数）** | :276-283 |
| PushConstants | `payload` + 其它 | :289-… |
| SetViewport / SetScissor | `f[0..5]` / `width,height`（`set_scissor` 的默认值取自 `ScissorRect` 构造，宽度 0 合法） | 见 `:305-380` |
| BeginPass | `pass_area[4]`、`f[0..3]=clear_color`、`f[4]=clear_depth_value`、`f[5]=clear_color?1:0`、`f[6]=clear_depth?1:0` | :498-511 |
| EndPass | 无字段 | :530-532 |

> ⚠ **`a/b/c` 是"通用整数槽"，语义按 op 解释**；`f[0]` 存 `first_instance`（float！）这类复用是
> 序列化/哈希时必须**逐 op 规范化**的原因（否则 `value` 与 `bits` 要分开处理）。

---

## 2. VulkanCommandList 两态、RenderPassDesc 捕获、submit 语义

### 2.1 RenderPassDesc 里捕获了什么

`RenderPassDesc`（`command.h:44-56`）有 8 个字段：`IRenderTarget* target`、`clear_color`、
`clear_color_rgba[4]`、`clear_depth`、`clear_depth_value`、`area_x/y/width/height`。

录制流**只捕获**（`vulkan_command.cpp:498-511`）：
`pass_area = {ax, ay, aw(解析后), ah(解析后)}`、`f[0..3]=clear_color_rgba`、
`f[4]=clear_depth_value`、`f[5]/f[6]=两个 clear 开关的 0/1 编码。
**`target` 指针不捕获**；`area_width==0` 的语义已被解析成 `extent.width`（`:391-392`），
所以"0 = 整图"的信息**丢失**——回放到不同尺寸目标时 area 不会跟着变。

### 2.2 设备态离屏确实会执行（与"命令流不进 GPU"是两回事）

`begin_pass` 在 `target` 能 `dynamic_cast<OffscreenTarget*>` 且 `color_image() != VK_NULL_HANDLE` 时
才建真 render pass / framebuffer（`vulkan_command.cpp:26-34, 424-494`）。也就是说：
设备的 attach 通路是通的，**但 `draw` 系列在设备态根本不发 `vkCmdDraw`**（全仓 `vkCmdDraw` 只有
`vulkan_draw.cpp:202` 和 `vulkan_ui_overlay.cpp:495` 两处，都不在 `vulkan_command.cpp` 里）。

### 2.3 当前"出图"的真正通路（L2 必须知道，否则会误以为录一帧就能看出像素）

| 通路 | 实现 | 证据 |
|---|---|---|
| L0 自检绘制 | `OffscreenTarget::draw_selftest(frame_index)` → `draw_selftest_grid` 内建 render pass + `vkCmdDraw(cmd,4,16,...)` | `vulkan_target.h:67`，`vulkan_draw.cpp:28-~205, :202` |
| UI 叠加层 | `UiOverlayPass::render_offscreen()`：**自己 alloc + begin + submit + waitIdle**，一次 `vkCmdDraw(cmd,4,instance_count,0,0)` | `vulkan_ui_overlay.cpp:397-512`（`:505` `vkQueueSubmit`，`:495` `vkCmdDraw`） |
| 主路径（swapchain） | `VulkanFrameRenderer::render_into_device_image()` **硬返回 false**（"L1/L2 未落地"） | `vulkan_ui_overlay.cpp:543-551` |

`render_to_memory()` = `render_offscreen()` + `target_->readback_rgba8()`（`vulkan_ui_overlay.cpp:553-578`），
所以 **`golden_ui.png` 的像素来自 UI 专用 pass，与 `ICommandList` 无关**。L2 若要独立 golden，
必须**新开一条**"场景 pass 经 ICommandList"的渲染通路（或接受 L2 golden 由另一个后端 pass 产出）。

### 2.4 `submit_and_wait` 的两种语义（问题 a 的尾问）

- **无 ctx（软件态）**：不做任何 GPU 工作，`++submit_count_`、清 `recorded_`/`command_count_`，
  `recording_` **保持不变**（`vulkan_command.cpp:581-592`）。即"提交 = 状态复位"。
- **有 ctx（设备态）**：`vkEndCommandBuffer` → `vkCreateFence` → `vkQueueSubmit` → `vkWaitForFences`
  → 销毁 fence → 销毁命令缓冲 → **同样清空 `recorded_`**（`:594-631`）。
- 注意 `draw_selftest` 与 `UiOverlayPass` 都**不经过** `ICommandList` 的 `submit_and_wait`，
  它们各自 `vkQueueSubmit`（`vulkan_draw.cpp`、`vulkan_ui_overlay.cpp:505`）。

---

## 3. IPipeline::hash()：怎么算、稳不稳、能不能只靠 hash 重绑

### 3.1 计算方式（纯函数，可单测）

`MagicBlocks/src/backend/vulkan/vulkan_pipeline.cpp:195-249`，声明在
`MagicBlocks/src/backend/vulkan/vulkan_pipeline.h:134-135`：

```cpp
u64 hash_pipeline_desc(const PipelineDesc& desc);   // 纯函数
```

- 初值 `0xcbf29ce484222325`（`:196`）；混合函数 FNV-1a 风格
  `h ^= v + 0x9e3779b97f4a7c15 + (h<<6) + (h>>2)`（`:178-181`）。
- 喂入顺序固定（`:197-247`）：`stages.size()` → 每 stage 的 `stage` + `size_bytes` +
  **SPIR-V 首 12 字节与末 4 字节**（`:201-211`，注释明写"不对 SPIR-V 全文做哈希"）→
  `bindings` / `attributes` 每字段 → blend 6 项 → depth 5 项 → raster 5 项 →
  `samples` → `primitive` → `compute` → `push_constant_bytes`。
- `hash_` 在 `init()` 里赋值（`:427`），与设备态/软件态无关。

### 3.2 跨进程稳定性判断

**稳**：
- 无指针、无地址、无时间、无 `std::hash`、无 `unordered_map` 迭代序；只用固定整数常量与
  `memcpy` 取 POD 位模式（`:183-189`）。
- 枚举全部指定了底层类型（`VertexFormat : uint8_t`、`ShaderStage : uint8_t`、`CompareOp : uint8_t`
  等，`MagicBlocks/include/magicblocks/resources/pipeline.h:44-52`、`handles.h:248-268`），
  `memcpy` 到的字节不会因编译器不同而变。
- 纯软件态与设备态同 desc → 同 hash（`:429-432` 只跳过建 Vk 对象）。

**不稳/不覆盖**（L2 必须自己处理）：
1. **不含 `AttachmentShape`**：`hash_ = hash_pipeline_desc(desc_)`（`:427`）而 `shape_` 只在
   `VulkanPipeline(ctx, desc, shape)` 构造里存在（`vulkan_pipeline.h:48-53,118`），**不参与哈希**。
   → 同一 desc 在 RGBA8/有深度 目标上 hash 相同，L2 若按 hash 建 PipelineTable，
   **回放到不同形状目标会用到不匹配的 `VkRenderPass`**（管线自带 render pass，
   `create_render_pass()` `vulkan_pipeline.cpp:438-478`）。
2. **SPIR-V 只取指纹**：不同 shader 若首 12 字节 + 末尾 4 字节相同，hash 相同（`:201-211`）。
   同一份内嵌 SPIR-V 在不同构建里**字节一致**（`glslc` 输出稳定），所以跨进程没问题；
   但"hash 唯一标识管线"这件事只能靠设备层按 `(desc, hash)` 建表来兜（注释 `:202` 自认）。
3. **`float` 字段直接位模式**（`depth_const` / bias 三项，`:237,241-243`）：`-0.0f` 与 `0.0f` 不同
   （这是好事，但要与 L2 的哈希规范口径一致）。

### 3.3 回放时能不能只靠 `pipeline_hash` 重绑？

**不能**。`bind_pipeline(IPipeline*, u64)` 的第一个参数**在公共签名里是必填的非空指针**
（`command.h:105`），实现里第一行就 `if (pipeline == nullptr) return Invalid;`（`vulkan_command.cpp:163`），
并且还会用 `pipeline->push_constant_bytes()` 记录后续校验基准（`:183`），
设备态还要 `dynamic_cast<VulkanPipeline*>` 取 `vk_pipeline()`（`:169-178`）。

→ L2 必须自备 `hash → IPipeline*` 表（计划 `docs/superpowers/plans/2026-11-03-magicblocks-l2.md:241-246` 的
`PipelineTable`）。**表里找不到 → `Unsupported`，不能猜**。这条已被计划确认，代码侧无变更需求。

---

## 4. 阻塞点：录制流不可从公共面读出（问题 a 的核心）

### 4.1 现状事实

| 需要的东西 | 在哪 | 公共面可见？ |
|---|---|---|
| `std::vector<RecordedCommand> recorded()` | `src/backend/vulkan/vulkan_command.h:123` | ❌ 后端私有头，含 `VkCommandBuffer`（`:121,169`）、`#include "backend/vulkan/vk_common.h"`（`:26`） |
| `RecordedCommand` 结构体 | 同上 `:64-79` | ❌ 同文件 |
| `CommandOp` 枚举 | 同上 `:47-59` | ❌ 同文件（但其**取值**必须与 L2 的 `CommandOp` 一致，见 `:46` 注释"L2 的命令流格式就按这张表发号"） |
| 11 命令方法本身 | `include/magicblocks/resources/command.h:98-149` | ✅ |

`src/scene/**` 若 `#include "backend/vulkan/vulkan_command.h"`：
- 会拖入 `Vk*` → 违反 spec §2.1，且 G3 门控**直接报红**（`tools/check_layer_purity.py:119-125`：
  backend 之外出现 `Vk[A-Z]` 或 `vk*Common` 调用即违规；`vulkan_command.h:121` 有 `VkCommandBuffer` 成员）。
- 即使不 include，`dynamic_cast<VulkanCommandList*>` 也需要类型完整定义。

### 4.2 四条可选路线（按"改动面"排序，供裁决）

| 路线 | 做法 | 代价 / 风险 |
|---|---|---|
| **A. L1 公共只读视图**（计划 R1，`l2.md:254-278,370-374`） | `command.h` 加 `RecordedCommandView` + `virtual Result<...> recorded_commands() const` 默认返回 `Unsupported`，后端覆盖 | 改变"11 方法"口径的**措辞**（查询不算命令，`Command.RecordsAllOps` 仍断言 11，`tests/magicblocks_l1_test.cpp:602`）；需改 `vulkan_command.h/.cpp`，且**这个查询还必须通过 G3**（`command.h` 在 backend 之外 → 视图类型必须是纯值 POD，不能拖 `Vk*`） |
| **B. `src/scene/` 录制代理** | L2 自己包一层 `RecordingCommandList : ICommandList`，转发给真 `ICommandList` **并就地记录**（op + 参数都来自入参，不需要读后端） | **零 L1 改动、零门控风险**；缺点是"录到的"是 L2 视角的重建，不是后端真实流（但两边字段一一对应，可加一致性测试）。**回放路径天然对称**（replay 时把 CommandStream 喂给同一接口） |
| C. 把 `CommandOp`/`RecordedCommand` 抽到 L1 公共头 | 移入 `command.h`，后端别名引用 | 触碰 L1 公共面类型定义，`Command.RecordsAllOps` 与 `vulkan_command.cpp` 全量改 include；风险最大 |
| D. 后端暴露 `void*` 不透明流 + L2 反射 | 类型擦除 | 破坏类型安全，且 D8"失败即报错"难以保证；不推荐 |

> **建议**：B 最符合"L2 零越层、零门控风险"，且**不动任何既有测试**。若坚持计划的 A：
> `RecordedCommandView` 必须**逐字段镜像** `RecordedCommand`（含 `packed_resource` / `packed_ok`，
> 否则 `a/b/c/f[]` 这些**按 op 解释的复用槽位**——例如 `f[0]` 存 `first_instance`
> （`vulkan_command.cpp:282`）、`a` 既是 `binding` 又是 `vertex_count`——无法无损还原），
> 且**必须给 `recorded_commands()` 一个默认实现**（返回 `Unsupported`）或同时更新全部实现者；
> 目前 `ICommandList` 只有 `VulkanCommandList` 一个实现（grep `public ICommandList` → 1 处，
> `vulkan_command.h:81`），所以新增纯虚方法只会影响它自己，**但未来 L2 代理/测试替身会继承这个负担**。

---

## 5. IRenderTarget 与 RenderPassDesc 的可序列化性（问题 c）

### 5.1 事实

- `IRenderTarget`（`MagicBlocks/include/magicblocks/device/render_target.h:24-39`）：
  `kind()`、`extent()`、`color_format()`、`samples()`、`color_attachment()`、`depth_attachment()`、
  `readback_rgba8()`。**没有 id / 序号 / 句柄**。
- 后端实现 `OffscreenTarget`（`MagicBlocks/src/backend/vulkan/vulkan_target.h:48-80`）持有
  `desc_` 与 `color_page_`/`depth_page_`（`:76-79`），但**公共 getter 只暴露 `color_image()` 这个 `VkImage`**
  （`:70`，Vk 类型，L2 不可用）；`page_id` 没有 getter。
- `TargetDesc`（`MagicBlocks/include/magicblocks/device/types.h:130-139`）有
  `{kind, extent, samples, with_depth, with_stencil, format, surface}`——**这些是值**，可序列化，
  但 `IRenderTarget` 不提供 `desc()` 反查。
- 交换链目标：`ISwapchain::current_target()`（`MagicBlocks/include/magicblocks/device/swapchain.h:30`），
  同样返回裸 `IRenderTarget*`。

### 5.2 结论与方案

**确定性序列化的唯一可靠做法**：不序列化 target 指针，改为序列化
`{extent, color_format, samples, has_depth, depth_format}` 这组**语义快照**（可由
`IRenderTarget::extent/color_format/samples` + `depth_attachment().valid` 直接取值），
回放时由调用方另行传入 target，并**校验语义快照是否匹配**（不匹配 → `Invalid`，不静默）。

这与计划 §5.1 一致（`docs/superpowers/plans/2026-11-03-magicblocks-l2.md:232-239`），但计划里写的
`area[4]` 需要**补一条**：录制流里的 `pass_area` 已经把 `0` 解析成 extent（`vulkan_command.cpp:391-392`），
所以"整图 area"与"显式 area=0"在流里不可区分——如果 L2 想保留"0=跟随目标尺寸"语义，
必须在 L2 自己的 `PassRecord` 里**另行**记录原始 `area_width==0` 标志（L2 有原始 `RenderPassDesc`，可做到）。

**若一定要给 target 稳定身份**，只能在 L0 加（例如 `IRenderTarget::id()`）；当前**没有**，
L2 **不要**假设存在。

---

## 6. 替换 UI 层 `scene_stub.h`（问题 d）

### 6.1 现状：谁 include、谁使用

| 文件 | 用法 | 证据 |
|---|---|---|
| `MagicBlocks/include/magicblocks/ui/scene_stub.h` | 定义 `namespace magicblocks::ui::stub` 下的 `NodeId/Transform/NodeSnapshot/SceneNodeOp/SceneNodeChange/SceneSnapshot/IScene/RenderFeatureLayer/IRenderFeature/IRenderFeatureRegistry/FeatureContext` | `:23-122` |
| `MagicBlocks/include/magicblocks/ui/router.h` | **唯一** include `scene_stub.h`，并 `using stub::X` 导入 10 个名字 | `:23`、`:30-38` |
| `MagicBlocks/include/magicblocks/ui/ui_app.h` | 只用 `stub::IScene*` / `stub::IRenderFeatureRegistry*` 两个字段 | `:42-43` |
| `MagicBlocks/src/ui/ui_app.cpp` | `stub::FeatureContext ctx{};` 一处 + 逐字段赋值 | `:115-118` |
| `MagicBlocks/src/ui/router.cpp` | **零** `ui::stub::` 字样；只用 `ctx.scene->snapshot()` / `restore()` | grep 结果；`:165-169`、`:209-213` |
| `MagicBlocks/tests/ui_test.cpp` | `class FakeScene final : public ui::stub::IScene`、`ui::stub::SceneSnapshot`、`ui::stub::NodeSnapshot`、`ui::stub::kInvalidNode`（共 9 处：`:121,123,125,126,128,135,241,243,260`） | `:121-136`、`:241-243`、`:260-262` |
| `MagicBlocks/tests/magicblocks_test.cpp`、`tests/magicblocks_l1_test.cpp`、`src/ui/*` 其余文件 | **不含** `scene_stub.h` / `ui::stub::IScene` | grep 全仓仅上表命中 |
| `MagicBlocks/src/ui_cli/main.cpp` | 只用 `ui::FeatureContext` | `:56` |
| `MagicBlocks/CMakeLists.txt` | 头文件列表里登记 `scene_stub.h`（只为 IDE 展示） | `:119` |

`overlay.h` 另有**第二个** `stub` 命名空间（`IOverlay2D`，`include/magicblocks/ui/overlay.h:26-73`），
`hud.cpp` / `overlay_impl.h` / `overlay_renderer.cpp` 用它（`src/ui/overlay_impl.h:22-25,50`）。
**它属于 L3，不属于 L2**，本轮不要动。

### 6.2 替换所需的逐名映射（L2 侧要提供同名/同语义类型）

| `ui::stub::` 名 | 桩定义行 | L2 应提供 | 备注 |
|---|---|---|---|
| `NodeId` = `std::uint64_t` | `scene_stub.h:30` | `magicblocks::scene::NodeId` 同底层类型 | UI 的 `IStateStore` 以它为键 |
| `kInvalidNode` = 0 | `:31` | 同名常量 | |
| `Transform`{`translation[3]`,`rotation[4]`,`scale[3]`} | `:34-38` | 字段名/类型/顺序**逐字一致** | 计划 R5 要求扁平数组一致（`l2.md:378`） |
| `NodeSnapshot`{`id`,`name`,`transform`,`visible`,`parent`} | `:40-46` | 同 5 字段 | `MemoryStateStore::serialize` 按这 5 个字段写文本（`src/ui/router.cpp:50,56-71`） |
| `SceneNodeOp`{Add,Remove,Update} | `:48-52` | 同名枚举 | |
| `SceneNodeChange`{`op`,`node`} | `:54-57` | 同名 | |
| `SceneSnapshot`{`nodes`} | `:61-63` | 同名扁平容器 | |
| `IScene`：`snapshot()` / `changes()` / `clear_changes()` / `restore()` | `:66-80` | L2 `IScene` **必须保留这 4 个方法**（可再加） | `router.cpp` 只用 `snapshot()`+`restore()`；`changes()` 无 UI 消费者 |
| `RenderFeatureLayer`{Scene,Overlay} | `:84-89` | 同名枚举 | |
| `IRenderFeature`：`layer()` / `record()` | `:92-100` | **签名冲突点**：计划写 `record(IFeatureRecordContext&)`（`l2.md:99`），桩是 `record()` 无参 | 见 6.3 |
| `IRenderFeatureRegistry`：`add/remove` | `:103-110` | 同签名 + `features()` | |
| `FeatureContext`{`scene`,`render_features`,`display`} | `:116-120` | 同名 3 字段 | `ui::DisplayInfo` 来自 `ui/types.h`（`include/magicblocks/ui/types.h:92`），**L2 公共头若 include `ui/*` 会反向依赖 UI 层**（见 6.3） |

### 6.3 三个必须先裁决的冲突（侦察发现，计划未覆盖）

1. **`IRenderFeature::record()` 签名**：UI 桩是无参 `virtual void record() = 0;`（`scene_stub.h:99`），
   计划的 L2 是 `record(IFeatureRecordContext&)`（`docs/superpowers/plans/2026-11-03-magicblocks-l2.md:99`）。
   二者只能取一个。**当前 UI 层没有实现者**（grep：`src/ui/` 与 `tests/` 中 `IRenderFeature` 零引用），
   所以**改签名是安全的**，但必须**同时**改 `scene_stub.h` 的转发别名与 `router.h` 的 `using`。
2. **`FeatureContext` 内的 `DisplayInfo` 属于 `magicblocks::ui`**。L2 的 `scene/*.h` 若直接复用
   `ui::DisplayInfo`，就形成 `scene → ui` 的反向依赖（spec §2.1 禁止 L2 include L3/L4，UI 层更在 L2 之上）。
   → 正确做法：`scene_stub.h` 转发时把 `DisplayInfo` 换成 scene 侧等价 POD，或在 `FeatureContext`
   里用 `{width,height,scale}` 三值；`ui_app.cpp:115-118` 的赋值点需同步。
3. **`ui::stub::` 命名空间下还有 `IOverlay2D`（L3）**。若把 `scene_stub.h` 整体改成
   `using namespace ...` 或 `namespace stub = scene`，会**误伤 L3**。安全做法：
   `scene_stub.h` 内改为
   ```cpp
   #include "magicblocks/scene/scene.h"
   #include "magicblocks/scene/render_graph.h"
   namespace magicblocks::ui::stub {
   using scene::IScene; using scene::SceneSnapshot; /* ...逐名 using */
   }
   ```
   —— 这样 `ui::stub::IScene` 与 `scene::IScene` **是同一类型**，`router.h:30-38` 的 `using` 无需改动，
   `ui_test.cpp` 的 9 处 `ui::stub::X` 也继续编译（这就是计划 §12 表里"改成转发别名"的具体形态，
   `l2.md:411`）。**唯一必须改的测试点**：无（若保留全部桩名）。

> **落地检查清单（改 `scene_stub.h` 后必须跑的 5 个用例）**：
> `ui_Router.ExitSavesStateAndReenterRestores`、`ui_Router.StateBlobRoundTrip`、
> `ui_UiApp.OffscreenModeRunsFrames`、`ui_Hud.DrawIsDeterministic`、`ui_Offscreen.MatchesGolden`
> —— 前两个吃 `SceneSnapshot`/`NodeSnapshot` 形状，后三个吃 `FeatureContext`/HUD 图元。
> 另：**改完必须全量重建**（§10 陷阱 12）。

---

## 7. 必须不回归的既有测试（问题 e）

### 7.1 CTest 全表（99 项，`ctest -N` 实测）

| # | 名称 | 来源 |
|---|---|---|
| 1-45 | 45 个 L0/L1 gtest（`Device.*` 8、`Errors.*` 1、`Target.*` 2、`FrameSync.*` 3、`Input.*` 2、`Lifecycle.*` 1、`Swapchain.*` 5、`Offscreen.*` 3、`Allocator.*` 7、`Handle.*` 1、`Buffer.*` 3、`Texture.*` 2、`Sampler.*` 1、`Pipeline.*` 5、`Command.*` 2、`Upload.*` 1） | `tests/magicblocks_test.cpp`、`tests/magicblocks_l1_test.cpp` |
| 46-85 | 40 个 UI gtest（`ui_Router` 5、`ui_Input` 8、`ui_Hud` 5、`ui_Perf` 5、`ui_Presenter` 9、`ui_UiApp` 5、`ui_Offscreen` 3） | `tests/ui_test.cpp` |
| 86-93 | 8 个门控 CTest | `MagicBlocks/CMakeLists.txt:245-262` |
| 94-99 | 6 个 CLI CTest | `MagicBlocks/CMakeLists.txt:267-284` |

### 7.2 L2 最可能打破的 12 个（逐条给"会被什么打破"）

| 名称 | 断言 | L2 风险 | 证据 |
|---|---|---|---|
| `Command.RecordsAllOps` | `command_count()==11` + `all_resource_handles_packed()` | 若路线 A 往 `ICommandList` 加方法并把查询算进计数 | `tests/magicblocks_l1_test.cpp:602-606` |
| `Command.RejectsInvalidRecordingOrder` | 失败录制不计数；空列表 submit 合法；未 end_pass submit → Invalid | 若 L2 改 `submit_and_wait` 语义 | `:610-639` |
| `Pipeline.CreateGraphicsPipeline` | `hash() > 0` | 若改 `hash_pipeline_desc` | `:445-452` |
| `Pipeline.UsesPushConstantsOnly` | `descriptor_set_count()==0`、`push_constant_bytes()==64` | L2 若引入描述符集 | `:487-502` |
| `Pipeline.PolygonModeIsAlwaysFill` | 恒 `VK_POLYGON_MODE_FILL` | — | `:505-514` |
| `Buffer.HandleIsOffsetIntoAllocatorPage` | `pack()` 成功、设备桶 `mapped()==nullptr` | L2 若换分配策略 | `:304-320` |
| `Allocator.SteadyStateFramesHaveZeroVmaCalls` | 稳态帧 VMA 增量 0 | L2 若每帧建资源 | `ctest #28` |
| `Offscreen.MatchesGoldenSingleBaseline` | `golden_l0.png` 差异 ≤1% + **左上角纯红**结构断言 | 若改 `draw_selftest`/读回行序 | `tests/magicblocks_test.cpp:534-576`（`:555-575`） |
| `Offscreen.FrameIndexChangesOutput` | CLI 出图 0≠1、0==0 逐像素 | 若改 CLI `render` | `:578-595` |
| `ui_Offscreen.MatchesGolden` | `golden_ui.png` **差异必须 0**（阈值 8） | 若 L2 改 `UiOverlayPass` 背景清屏色或实例布局 | `tests/ui_test.cpp:1048-1065` |
| `ui_Router.ExitSavesStateAndReenterRestores` / `ui_Router.StateBlobRoundTrip` | `ui::stub::IScene`/`SceneSnapshot`/`NodeSnapshot` 形状与 `MemoryStateStore` 文本格式 | 若改 `scene_stub.h` 字段名/顺序或 `MemoryStateStore` 序列化 | `ui_test.cpp:228-276`；`src/ui/router.cpp:47-143` |
| `ui_UiApp.OffscreenModeRunsFrames` 等 5 项 | `UiApp` 离屏路径 + HUD 图元非空 | 若换 `VulkanFrameRenderer` 实现 | `ui_test.cpp:805-928`（5 项：`:805,817,841,879,906`） |

另：`ui_Perf.SingleSubmitAndZeroAllocAreEnforced` **当前几乎测不到 L2**，因为
`UiApp` 把 `vma_alloc_count` 硬编码成 0（`src/ui/ui_app.cpp:215`），`submit_count` 来自
Presenter 自计数（`:213`）。

### 7.3 CTest 门控（8 项，全部 `ctest -R gate` 实测 Passed）

| CTest | 脚本 | 扫什么 | L2 注意 |
|---|---|---|---|
| `magicblocks_gate_g1_no_geometry_shader` | `tools/check_no_geometry_shader.py` | `shaders/**` + `src/**`（`SHADER_EXTS` 含 `.comp`/`.glsl`，`CODE_EXTS` 含 `.h/.cpp`，`SKIP_DIRS` 含 build/third_party） | L2 若加 shader，只能 `.vert/.frag/.comp` |
| `magicblocks_gate_g3_layer_purity` | `tools/check_layer_purity.py` | `include/**`、`src/**`、`tests/**`、`tools/**`、`shaders/**`；backend 之外禁 `vulkan/` include、`Vk*`/`PFN_vk*`、`vk*`/`vma*` 调用；backend 内禁玩法名 | **`src/scene/**` 自动纳入最严口径**（`tools/check_layer_purity.py:97-125`）；文件数会出现在 OK 行 |
| `magicblocks_gate_g4_no_raw_alloc` | `tools/check_no_raw_alloc.py` | `include/src/tests/tools`；禁 `vkAllocateMemory/FreeMemory/MapMemory/UnmapMemory(`；`vk_mem_alloc.h` 只允许 `src/backend/vulkan/vulkan_allocator.cpp` include | L2 不得 include VMA 头 |
| `magicblocks_gate_deps_tests` | `tools/check_deps_tests.py` | `tools/tests` 必须**恰好 12 个** `test_*.py`，`Ran 191`，failed=0 skipped=0 | 新增 `tools/tests/test_*.py` 必须同步改 `--expect-files`（CMake 调用未传该参数，默认 12，`MagicBlocks/CMakeLists.txt:251-252`；默认值 `tools/check_deps_tests.py:22-23`） |
| `magicblocks_gate_scripts_selftest` | `unittest -p test_check_scripts.py` | G1/G3/G4 自测 | 在 tempfile 假项目里跑，**不碰**真实仓库文件；见 7.4 |
| `magicblocks_gate_uig1_no_readback` | `tools/check_ui_no_readback.py` | `src/ui/**` + `tests/**` 中文件名含 `ui` 的文件；禁 `readback`（宽松匹配）等 8 类；路径含 `offscreen` 豁免 | 若 L2 在 `tests/magicblocks_l2_ui_*.cpp` 里写 `readback` → 红 |
| `magicblocks_gate_uig2_single_submit` | `tools/check_ui_single_submit.py` | `src/ui/**` + `tests/**`；禁 `->submit(`/`.submit(`/`QueueSubmit`/`queue_submit(`；basename 含 `present` 豁免 | **L2 的 `tests/*.cpp` 若直接调 `.submit(` 会被判违规**（`tools/check_ui_single_submit.py:110-119`：扫整个 `tests/`） |
| `magicblocks_gate_ui_scripts_selftest` | `unittest -p test_ui_check_scripts.py` | UIG1/UIG2 自测 | 同上 |

### 7.4 门控自测会不会被 L2 的新文件影响？（实测：**不会**）

`tools/tests/test_check_scripts.py` 的每个用例都在 `tempfile.TemporaryDirectory()` 里用
`make_project(tmp, src_files=..., include_files=..., shaders=...)` **现造一个假项目**再调脚本主体
（`tools/tests/test_check_scripts.py:36-53` 的 `make_project` / `run`），样本全是
`src/backend/vulkan/*.cpp`、`include/magicblocks/device/*.h` 这类**虚构路径**，
且**没有**任何"扫描文件数 == N"的断言（grep `src/scene` / `hud.cpp` / 计数断言 → 0 命中）。

→ 结论：**L2 新增 `src/scene/**`、`include/magicblocks/scene/**` 不会影响 G1/G3/G4 自测**；
脚本的 OK 行会打印新的文件计数（G3 当前 47 外 / 37 内），但那只是日志，不参与判定。

**唯一仍需注意的**：`tools/check_deps_tests.py` 断言 `tools/tests` 下 `test_*.py`
**恰好 12 个**（`tools/check_deps_tests.py:22-23,35-38`，CMake 调用未传 `--expect-files`，
`MagicBlocks/CMakeLists.txt:251-252`）。若 L2 想新增一个门控脚本及其自测，必须**同时**改这个默认值，
否则 `magicblocks_gate_deps_tests` 直接红。

---

## 8. CMake 事实

| 事实 | 证据 |
|---|---|
| 单一静态库 `magicblocks`，**源文件逐个列名**（不是 GLOB）→ 新增 `src/scene/*.cpp` 必须手工加进 `add_library` | `MagicBlocks/CMakeLists.txt:101-178` |
| 公共头也逐个列出（只为 IDE）；`include/magicblocks/scene/*.h` 需加入 | `:102-127` |
| shader 内嵌：`glslc -fshader-stage=<ext>` → `.spv` → `shaders/embed_shaders.py` → `${BINARY_DIR}/magicblocks_shaders.h`；变量名 `k<CamelCase>`（如 `kFullscreenVert`），`alignas(4) inline const uint32_t kX[]` + `kXSize` | `:59-82`；`shaders/embed_shaders.py:18-25,53-58` |
| 内嵌头在**二进制目录**，靠 `target_include_directories(... PRIVATE src ${BINARY_DIR})` 可见 | `:179`；`vulkan_draw.cpp:8` `#include "magicblocks_shaders.h"` |
| 测试：单可执行 `magicblocks_test` 链接 3 个 `.cpp`，`gtest_discover_tests()` 注册（每 TEST 一项 CTest） | `:228-240` |
| 测试宏：`MB_GOLDEN_DIR="<src>/tests/data"`、`MB_CLI_PATH`、`MB_UI_CLI_PATH` | `:236-239` |
| 门控 CTest 用 `${Python3_EXECUTABLE}` + `${MINE_ROOT}/tools/...`，`MINE_ROOT` 来自缓存/`CMAKE_CURRENT_LIST_DIR/..` | `:13-17,244-262` |
| CLI 目标：`magicblocks-l0-cli`(:203)、`magicblocks-ui-cli`(:215)，均 `PRIVATE src` include 目录 + 平台宏 | `:203-222` |
| Windows 必须 `VK_USE_PLATFORM_WIN32_KHR`（否则后端头里的平台枚举缺失） | `:184,209,219,232` |

---

## 9. CLI 事实（写 `magicblocks-l2-cli` 要照抄的东西）

| 事实 | 证据 |
|---|---|
| 退出码约定 `0` 成功 / `1` 运行失败 / `2` 参数错误 / `3` 需提权（loader 看不到 SwiftShader） | `MagicBlocks/src/cli/main.cpp:9`；`docs/ARCHITECTURE.md:316` |
| 参数解析风格：手写 `for` 循环 + `next()` lambda 取下一 token；**未知参数直接 `return 2`** | `src/cli/main.cpp:81-110` |
| UI CLI：`Args` 结构 + `--width/--height/--frames/--output/--feature`，未知参数 → 2 | `src/ui_cli/main.cpp:88-110,289-300` |
| CTest 用 `WILL_FAIL TRUE` 断言"未知参数 = 非 0 退出" | `CMakeLists.txt:282-284` |
| 设备强筛 `require_software_rasterizer = true`（绝不回退物理 GPU） | `src/cli/main.cpp:59-64` |
| 离屏目标创建模板：`TargetDesc{kind=Offscreen, extent, with_depth=false, format=RGBA8_UNORM}` | `src/cli/main.cpp:127-137` |
| PNG 落盘：`magicblocks::write_png_rgba8(path, ImageRgba8, &err)` | `src/cli/main.cpp:163-167`；`src/core/image_io.h` |

L2 计划里的 5 个子命令与退出码（`docs/superpowers/plans/2026-11-03-magicblocks-l2.md:217-228`）与此一致，
照抄即可；**唯一注意**：`replay --verify` 的失败必须走退出码 `1`（运行失败），不要用 `2`。

---

## 10. ARCHITECTURE.md §10 的 15 条陷阱 × "是否适用于 L2"（问题 f）

原文 `MagicBlocks/docs/ARCHITECTURE.md:357-389`。

| # | 陷阱（一行） | 行 | 适用 L2？ | 对 L2 的具体形态 |
|---|---|---|---|---|
| 1 | 别用 PowerShell `Get-Content`/`Set-Content` 改源文件（会 UTF-8→GBK） | :359-360 | **是（高）** | L2 源文件全是中文注释，且 `CMakeLists.txt:7-11` 依赖 `/utf-8`；用编辑器/`write` 工具 |
| 2 | `.ps1` 必须带 UTF-8 BOM | :361 | 否 | L2 不新增 `.ps1` |
| 3 | `half` 是保留字 | :362 | 视情况 | 只有新写 shader 时相关 |
| 4 | `namespace magicblocks` 让 G3 的 `blocks` 误报；豁免别删 | :363-364 | **是（高）** | L2 在 `magicblocks::scene` 里若写含 `blocks` 的标识符：G3 在两个方向都用 allowlist 判断——backend **外**的规则是 `Vk*`/`vk*` 调用，`blocks` 字样本身不违规；**backend 内**才查玩法名，且 allowlist 只抹 `magicblocks`/`timestamp*`（`tools/check_layer_purity.py:29-34,127-133`）。所以 L2 在 `src/scene/` 写 `blocks` 是安全的，**但别去 `src/backend/` 里新增含 `blocks` 的标识符** |
| 5 | `// VMA-injection` 标记的注入区规则 | :365-366 | 否（已失效） | G4 现在**不再豁免**任何裸分配（`tools/check_no_raw_alloc.py:7-10`） |
| 6 | 错误字符串里别写 `vkAllocateMemory(` 带括号 | :367 | **是（中）** | L2 的错误消息若想说明"不走裸分配"，别写成带括号形式 |
| 7 | 设备顺序不保证，别用 `devices[0]` | :368 | 否（L2 不选设备） | L2 只拿 `IRenderDevice*` |
| 8 | "100% tests passed" 不能全信（golden 可能被错基线固化） | :369-370 | **是（高）** | L2 新建 `golden_l2.png` 时必须有**独立结构断言**（计划 R3 建议纯绿标记，`l2.md:376`） |
| 9 | `IFrameSync` 里不写 present | :371 | 否 | L2 不用 present |
| 10 | 删 probe/临时文件后重跑门控 | :372 | **是（高）** | `MagicBlocks/` 根目录**现在就躺着 18 个 `probe*.obj`**（`probe.obj`、`probe2..17.obj`，实测 glob 18 个）；`.obj` 不在 G1 的 `CODE_EXTS`/`OTHER_EXTS` 里（`tools/check_no_geometry_shader.py:37-40`）所以当前不报错，但这是**历史遗留的脏文件**，L2 开工前应清理（删 .obj 不触发重建风险，它们不在 `build/` 里） |
| 11 | `vmaBindImageMemory` 不能绑 `VkBuffer` | :373-376 | 否（但相关） | L2 不碰 VMA；回放读回若用新 `VkBuffer`，必须走 `bind_buffer_page` |
| 12 | Ninja 依赖扫描关闭 → **改头文件不触发重编**，必须 `ninja -t clean` | :377-380 | **是（极高）** | L2 必然改 `scene_stub.h`（UI 公共头）+ 新增 `include/magicblocks/scene/*.h` → **每次改头后全量重建**，否则 `ui_test.cpp.obj` 会留在旧布局上（正是这条踩过的坑） |
| 13 | `VMA_MEMORY_USAGE_AUTO_PREFER_*` 在 SwiftShader 必失败；`memoryTypeBits` 不能填 0 | :381-384 | 否 | L2 不碰 VMA |
| 14 | 读回行序必须翻转；`fullscreen.vert` 的 `(kCols-1-cy)` 是另一回事 | :385-387 | **是（高）** | L2 golden 若自己读回，**必须复用** `readback_rgba8`（已翻转），不要另写 `vkCmdCopyImageToBuffer` |
| 15 | `device::Result::message()` 是方法，`device::Status::message` 是字段 | :388-389 | **是（中）** | L2 大量用 `Result<T>`（`create_*`）；写错是高频编译错误 |

**汇总：15 条里 8 条直接适用于 L2**（#1 #4 #6 #8 #10 #12 #14 #15），其中 **#12（改头必须全量重建）
与 #1（编码）是 L2 最高频的踩坑点**，#14/#8 决定 L2 golden 的可信度。

---

## 11. L1/L0 接口缺口清单（逐条给 file:line，不臆造能力）

| 缺口 | 现状证据 | 影响 S? | 处置建议 |
|---|---|---|---|
| **G-1 录制流不可读**（阻塞） | `src/backend/vulkan/vulkan_command.h:64-79,123` 全在私有头 | S4 / S6 / S7 | §4 的四条路线，**必须裁决** |
| **G-2 回放无 `IPipeline*`** | `command.h:105` + `vulkan_command.cpp:163` 强制非空 | S5 Replay | `PipelineTable`（L2 自建），找不到 → `Unsupported` |
| **G-3 `IRenderTarget` 无身份** | `device/render_target.h:24-39`（无 id） | S3 / S6 | 序列化语义快照（§5.2），回放由调用方传 target |
| **G-4 无"场景插入渲染图"钩子** | `ICommandList` 只有 11 命令，`IRenderGraph` 要自己定义；`UiOverlayPass` 是独立通路（`vulkan_ui_overlay.cpp:397-512`） | S3 / S9 | L2 自己定义 `IRenderGraph::add_pass` + `record(ICommandList&)`；**不要**去改 `UiOverlayPass` |
| **G-5 记 `recording()` 的设备态依赖 target 实现** | `vulkan_command.cpp:410-423`（只有 `OffscreenTarget` 才 `recording_=true`） | S4 | L2 不要用 `recording()` 做流完整性判据；用 `command_count()`/自己的计数 |
| **G-6 稳态 0 VMA 与 `reserve_pages` 的矛盾** | `vulkan_allocator.cpp:824-834`（无空位即 OOM），设备初始化 `initial_pages_per_kind=0`（`vulkan_render_device.cpp:173-175`，`vulkan_allocator.h:73`），公共面 `IRenderDevice` **无** `reserve_pages`（`device/render_device.h:20-59` 只有 `memory_allocator()`） | M5 | L2 init 阶段：`memory_allocator()->reserve_pages(kind, n)` **再** `create_buffer`——这条是**已有能力**（`memory_allocator()` 在 `render_device.h:42`），不违反 G4 |
| **G-7 `first_instance` 用 float 存** | `vulkan_command.cpp:282` | S4 / S7 | 序列化/哈希时按 `u32` 位模式处理，别按 float 比较 |
| **G-8 `area_width==0` 语义丢失** | `vulkan_command.cpp:391-392` | S3 / S6 | L2 的 `PassRecord` 自行记原始标志（§5.2） |
| **G-9 主路径 `render_into_device_image` 恒 false** | `vulkan_ui_overlay.cpp:543-551` | 无（非 L2 范围） | L2 不改；但要知道 `UiApp` 的 swapchain 路径仍不可用 |
| **G-10 `ISwapchain` 无 acquire/present** | `device/swapchain.h:17-31`（只有 `extent/format/rebuild/current_target`） | 无 | 帧循环里"拿目标"只能靠 `current_target()` |
| **G-11 没有 `ICommandList` 的测试替身** | grep `class .*: public ICommandList` → 仅 `VulkanCommandList`（`vulkan_command.h:81`） | S4 | 若走路线 A 的"默认 Unsupported 实现"，替身可后补；走路线 B 则 L2 自己的代理即可测 |
| **G-12 `UiApp` 的 `vma_alloc_count` 是假的** | `src/ui/ui_app.cpp:215` `s.vma_alloc_count = 0;` | M5 的**验收假绿** | L2 的零分配断言必须自己读 `AllocatorStats` 差分（`resources/memory.h:56-71`） |

---

## 12. 十条风险（按严重度）

| 风险 | 证据 | 对策 |
|---|---|---|
| R1 录制流读不出来（S4/S6/S7 全挂） | §4.1 | 先裁决 A/B 路线，**不要**先写序列化 |
| R2 改 `scene_stub.h` 后不重编 → 幽灵行为 | `docs/ARCHITECTURE.md:377-380` | 每次改头 `ninja -t clean` 或删 obj 全量重建 |
| R3 新增门控脚本会让 deps-tests 红 | `tools/check_deps_tests.py:22-23,35-38` | 新增 `tools/tests/test_*.py` 必须同步改 `--expect-files`（或改动 CMake 的调用参数） |
| R4 `IRenderFeature::record()` 签名冲突 | `scene_stub.h:99` vs `l2.md:99` | 改签名（UI 层无实现者，安全），同时改 `router.h:31-32` 别名 |
| R5 `FeatureContext` 把 `ui::DisplayInfo` 拖进 scene | `scene_stub.h:119`，`include/magicblocks/ui/types.h:92` | scene 侧用等价 POD，`ui_app.cpp:115-118` 同步 |
| R6 回放跨进程拿不到 `IPipeline*` | §3.3 | `PipelineTable` + 明确 `Unsupported` |
| R7 命令哈希跨进程漂移 | `pipeline.cpp:237,241-243` 浮点位模式 | L2 哈希只用**位模式 + 规范化二进制**，不用 `==` |
| R8 稳态帧意外分配 | `vulkan_allocator.cpp:824-834` | init 阶段预留页 + 每帧 `AllocatorStats` 差分 |
| R9 L2 golden 被错基线固化 | `docs/ARCHITECTURE.md:369-370`，`tests/magicblocks_test.cpp:555-575` | L2 golden 自带结构断言（如纯绿标记点，`l2.md:376`） |
| R10 UIG2 误伤 L2 测试 | `tools/check_ui_single_submit.py:110-119`（扫整个 `tests/`） | L2 测试文件名**别含** `present`，且**别**写 `.submit(` / `->submit(`（用 `submit_and_wait()` 是安全的，正则只认 `submit\s*[<(]`） |

---

## 13. 建议的开工顺序（依赖安全）

1. 裁决 §4 的录制流路线（A/B）与 §6.3 的三处签名冲突 → 落到计划里再动代码。
2. 先落 **S1 `IScene` + `scene_stub.h` 转发别名**（纯头 + 纯实现，零 Vulkan），
   跑 `magicblocks_test --gtest_filter="ui_Router.*"` 与 `ui_UiApp.*` 确认不回归。
3. 再落 **S2/S3/S9**（提交队列 / 渲染图 / 特性注册表）——全是纯 CPU 逻辑。
4. 再落 **S4/S6/S7**（CommandStream / 序列化 / 哈希）——依赖 1 的裁决；全部用例跑在
   软件态列表上（`VulkanCommandList()` 或 L2 代理）。
5. 最后 **S5 `IRecorder` 三态 + S8 帧自包含 + CLI**；回放需要 `PipelineTable`。
6. 全程：改头 → 全量重建；每步跑 `ctest -R "gate|ui_Router|Command\.|Pipeline\."`。

---

## 附：本次侦察实际执行过的只读命令

```
git status --short ; git log --oneline -8
ctest --test-dir MagicBlocks\build -N                       → 99 项
ctest --test-dir MagicBlocks\build -R "gate"                → 8/8 Passed
ctest --test-dir MagicBlocks\build -R "magicblocks_test|Command\.|Pipeline\.|ui_Router|ui_Offscreen|Offscreen\."  → 18/18 Passed
python tools\check_layer_purity.py     --root MagicBlocks   → [G3] OK rc=0
python tools\check_no_geometry_shader.py --root MagicBlocks → [G1] OK rc=0
python tools\check_no_raw_alloc.py     --root MagicBlocks   → [G4] OK rc=0
python tools\check_ui_no_readback.py   --root MagicBlocks   → [UIG1] OK rc=0
python tools\check_ui_single_submit.py --root MagicBlocks   → [UIG2] OK rc=0
```
（未 build、未编辑任何源码；唯一写入 = 本文件。）
