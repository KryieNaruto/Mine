# MagicBlocks —— 立方体渲染打通：四个真实缺陷的定位与修复

日期：2026-11-05
状态：已修复并验证

## 0. 结论先行

L0–L3 此前**无法**输出任何带几何的画面。经过本轮排查，定位到 **4 个相互独立的
真实缺陷**，全部修复后：

- 立方体正常渲染，360° 旋转序列 12 帧全部不同；
- 238 个单测通过（原 233 + 新增 5）；
- 5 个门禁脚本全部 OK（G1/G3/G4/UIG1/UIG2）。

**关键教训**：这 4 个缺陷里，有 3 个是"Vulkan 静默失败"——不报错、不崩溃、
`VkResult` 全是 `VK_SUCCESS`，只是画面上什么都没有。逐个靠"猜"浪费了大量时间，
最终是靠**注入诊断输出 + 二分剥离变量**定位的。

---

## 1. 缺陷一：`ICommandList` 的 draw 从不执行

**现象**：`submit_and_wait()` 之后图像只有清屏色。

**根因**：L1 的 `VulkanCommandList::draw*` 只有**录制**语义 —— 把命令写入
`recorded_` 列表，**从不调用 `vkCmdDraw*`**。`vulkan_command.cpp` 里
`vkCmdDraw` 出现次数为 **0**。

真正的执行发生在后端内部的 `submit_device_frame()`
（`src/backend/vulkan/vulkan_device_frame.cpp`），它把 11 类命令逐条翻译成
`vkCmd*` —— 但**这个函数在全仓有零个调用方**，是彻底的死代码。

同时 `IRenderDevice` 也没有任何公开的"提交命令列表"入口。

**修复**：
- `IRenderDevice` 新增 `virtual Status submit_command_list(resources::ICommandList&) = 0;`
- 实现里把命令列表 dynamic_cast 成 `VulkanCommandList`，调用
  `submit_device_frame()`，并在 `draw_calls == 0` 时明确报错。

**连带问题**：`submit_device_frame` 需要 `ResourceId → IBuffer*` 反查表才能拿到
`VkBuffer`，但这张表（`VulkanCommandList::buffers_`）**从来没有被填充过**。
修复方式：设备侧维护 `known_buffers_`，提交前用
`remember_buffer_binding()` 把已知句柄挂上去。

---

## 2. 缺陷二：清屏值索引两端错位

**现象**：修好入口后画面变成**纯蓝** `(18, 26, 255)`，且所有像素完全一致。

**根因**：录制端与回放端对 `RecordedCommand::f[]` 的**索引用法完全不一致**。

录制端（`vulkan_command.cpp` 的 `begin_pass`）实际写入：

| 索引 | 含义 |
|---|---|
| `f[0..3]` | `clear_color_rgba` |
| `f[4]` | `clear_depth_value` |
| `f[5]` | `clear_color ? 1 : 0` |
| `f[6]` | `clear_depth ? 1 : 0` |

回放端（`vulkan_device_frame.cpp`）却按另一套约定读：

```cpp
// 错误版本
clears[0].color = {{c.f[1], c.f[2], c.f[3], c.f[4]}};  // 期望 f[1..4] 是 rgba
clears[1].depthStencil = {c.f[6], 0};
```

于是清屏色被读成 `(g, b, a, clear_depth_value)` —— 蓝通道恒为 255，正好解释了
`(18, 26, 255)`。

**这个缺陷特别隐蔽**：因为画面唯一的颜色就是那个被读错的清屏色，很容易被误判成
"格式不匹配"或"剔除方向反了"。

**修复**：按录制端为准重写回放端，并在两处都加上逐字段对照注释。

---

## 3. 缺陷三：render pass 与管线不兼容（静默丢弃 draw）

**现象**：清屏色已经精确正确（`(15,18,26)`），说明 pass / framebuffer / readback
全部正常 —— 但几何依旧是空的。

**诊断方法（关键）**：把顶点着色器换成"**直接输出固定 NDC 三角形、零顶点输入、
零 push constant**"的版本。依然一个字都不出 → 排除顶点数据、矩阵、属性格式、
剔除方向，只剩"draw 根本没执行"。

**根因**：Vulkan 要求 `vkCmdDraw` 时的 render pass 必须与管线创建时用的
render pass **兼容**。回放 `begin_pass` 时自己新建了一个 `VkRenderPass`
（`create_pass()`），但绑定的管线是用 `VulkanPipeline` **自己的**
`vk_render_pass_` 建的。两个不同的 `VkRenderPass` 对象 → 不兼容 →
驱动**静默丢弃** draw。

**修复**：回放 `begin_pass` 时优先**借用管线内建的 render pass** 来建 framebuffer，
并给 `PassResources` 加 `rp_owned` 标记，避免误销毁管线拥有的 pass。

**过程中踩的坑（值得记录）**：
1. 第一版补丁用"回放走到当前位置时最近一次 `bind_pipeline` 的管线"来挑 pass ——
   但录制顺序是 `begin_pass → …… → bind_pipeline → draw`，
   **`begin_pass` 时管线还没绑**，条件恒为假，补丁从未生效。正确做法是
   **向后扫描整帧命令流**取第一条 `BindPipeline`。
2. 补丁中间一度出现 `if (false && ...)`，把整个借用分支关死了。

---

## 4. 缺陷四：设备页缓冲的上传通道根本没实现

**现象**：诊断三角形（不读顶点）能画出来，但一旦读取顶点数据就全空。

**根因**：`VulkanBuffer::commit_upload()` 里对**设备页目标**
（Vertex / Index / Uniform / Storage）直接返回：

```cpp
return Status::Unsupported(
    "VulkanBuffer::upload: 设备页目标需要在命令提交阶段物化暂存 VkBuffer"
    "(L2 帧批处理);本轮请在 BufferUsage::Staging 的缓冲上用 write() 写数据");
```

也就是说 **顶点/索引数据从未真正进入 GPU**。上传注释说这活儿留给
"L2 帧批处理"，而那部分并不存在。

**更难发现的是**：`flush_pending()` 把这个失败只记进 `upload_commit_error_`
（一个无人查询的字段），而 `pending_upload_bytes()` 照常归零 ——
所以 demo 的自检 `vp != 0 || ip != 0` **完美通过**，骗过了自己。

**修复**：实现 `flush_pending_device_pages()`：
1. 物化一张 `TRANSFER_SRC` 的临时 staging `VkBuffer`；
2. 把 `pending_` 里所有区间合并成一次 `vkCmdCopyBuffer`；
3. 插入 `VkBufferMemoryBarrier`（`TRANSFER_WRITE → VERTEX/INDEX/UNIFORM/SHADER_READ`）；
4. `submit_once_sync()` 提交并等待。

**G4 约束**：staging 的 `VkDeviceMemory` **不能**用 `vkAllocateMemory` /
`vkMapMemory`（"全仓零裸分配"门禁立刻报错）。必须走分页分配器的
`allocate_image_page(device_local=false)` + `bind_buffer_page()` +
`image_page_mapped_ptr()` —— 与 `readback_rgba8` 同一套路。

---

## 5. 调试过程中的两个工程陷阱

### 5.1 构建产物时间戳导致着色器未重编

我把 `cube.frag` 临时改成"输出纯红"做诊断，之后用 `Copy-Item` 从备份还原。
还原后源码 mtime 变得**比 `.spv` 更旧**，ninja 判定无需重建 ——
于是**诊断用的纯红片元着色器一直被链进二进制**，导致后续所有"属性读取"实验
都得到恒定的 `(255,0,0)`，白白误导了好几轮。

**教训**：改过着色器后，若怀疑未生效，直接
`Remove-Item build\*.spv` 强制重生成，或核对 `.spv` 的 mtime。

### 5.2 绕向与剔除

`make_perspective` 内含一次 y 翻转（世界 +y 朝上 → Vulkan NDC +y 朝下），
这会**翻转屏幕空间绕向**。因此模型空间全 CCW 的面，在管线里需要
`FrontFace::Clockwise` + `CullMode::Back`。

诊断阶段曾把 `cull` 设为 `None` 才让三角形出现 —— 那是为了确认"
几何是否真的到达光栅化器"，**不是**最终配置。

---

## 6. 验证结果

### 6.1 单帧（yaw=35°, pitch=25°, 480×360）

4 个唯一颜色，3 个可见面：

| 颜色 | 面 | 像素数 |
|---|---|---|
| `(61, 35, 10)` | +Z 黄 | 9239 |
| `(77, 20, 20)` | +X 红 | 8382 |
| `(38, 19, 51)` | -Y 紫 | 6769 |

包围盒 `x:154-344, y:82-261`，前景占 14.1%。

与 CPU 参考光栅化器（独立实现的同一套矩阵链）结果一致。

### 6.2 旋转序列（12 帧，30° 步进）

- 12 帧 md5 **全部不同**；
- 相邻帧像素差 12777 个，差异区域限定在立方体包围盒内；
- 可见面数在 4 / 5 / 6 之间变化 —— 符合旋转时面数变化的预期。

### 6.3 门禁

| 门禁 | 结果 |
|---|---|
| `check_no_geometry_shader.py` (G1) | OK，149 文件，无几何着色器 |
| `check_layer_purity.py` (G3) | OK，backend 外 96 / 内 39，分层纯净 |
| `check_no_raw_alloc.py` (G4) | OK，135 文件，全仓零裸分配 |
| `check_ui_no_readback.py` (UIG1) | OK，零回读 |
| `check_ui_single_submit.py` (UIG2) | OK，1 处提交 |

单测：**238 / 238 通过**。

---

## 7. 对上屏路线的影响（重要）

调研 v2（`2026-11-05-magicblocks-ui-rendering-route-research.md`）§7 已判定
旧的"场景 + UI 同帧、同一 command list、单次 submit"路线**作废**，
上屏改为壳层持有（Android `SurfaceView` / iOS `CAMetalLayer`）。

本轮打通的 `submit_command_list()` 正是新路线所需要的那块地基：
**"把一帧场景渲染到外部 surface"** 的实现路径。

UI 层的重建（`ISceneFrameSink` + `IOverlay2D`）见
[UI 层重建计划](2026-11-05-magicblocks-ui-shell-hosted.md)。
