# 给 L1 会话的同步（2026-11-09 16:0x）

> 来源：Lead。三件事，**其中两件需要你别做**。
> 如果你是 L1 会话，请读完再动手。

---

## 1. `build_graphics()` 的 `Status ds` 已被改名 `desc_status`

**原因**：它和同一函数 714 行的
`VkPipelineDepthStencilStateCreateInfo ds{}`（P12 深度/模板状态）重名，
`error C2371: "ds": 重定义；不同的基类型`，**整个 magicblocks 库编不出来**。

**不是你的代码被覆盖**，是变量重名。纯局部变量改名，零语义变化，
`sampled_texture_slots` 的逻辑分支一行都没动。

- 你若接着改这段：沿用 `desc_status`，或把 `create_descriptor_objects()`
  的返回值改成内联 `if`，但**别再用 `ds`**。
- `build_compute()` 里 815 行的 `Status ds` 没有冲突，保持原样。
- MSAA 侧已在 758 行上方补了注释说明原因，防止二次撞车。

---

## 2. `vulkan_command.h` 缺 `ITexture`/`ISampler` 的 using，阻塞全仓构建

`vulkan_command.h:160,161,226,227` 使用了 `ITexture*` / `ISampler*`，
但 38-48 行的 using 列表里没有它们：

```
38: using resources::DrawArgs;
39: using resources::IBuffer;
40: using resources::ICommandList;
41: using resources::IndexFormat;
42: using resources::IPipeline;
43: using resources::RenderPassDesc;
44: using resources::ResourceId;
45: using resources::ScissorRect;
46: using resources::Viewport;      ← 到这里就停了
47: using resources::u32;
48: using resources::u64;
```

→ `error C2065: "ITexture": 未声明的标识符`

已授权 MSAA 侧补两行（纯命名空间补全）：

```cpp
using resources::ISampler;
using resources::ITexture;
```

**但 `texture_at()` / `sampler_at()` 的实现是你的**，MSAA 侧不会碰。
如果你还没写实现，链接期会报 undefined symbol —— 那是你欠的活，请补上。

---

## 3. 新增了 MSAA 抗锯齿，请知悉它动了哪些共享文件

**用户裁定**：MSAA 4x、采样数跟随 `DeviceCaps.supported_msaa_levels` 可调（2/4/8）、
**默认关闭**（samples=1）、演示时 `--msaa N` 开启、自动回归继续关闭。
所以 389 条用例和 3 张 golden 必须逐字节不变。

**MSAA 侧已改的文件**：

| 文件 | 改动 |
|---|---|
| `vulkan_draw.cpp` | 断点 A：`ms.rasterizationSamples` 不再硬编码 1 |
| `vulkan_target.h/.cpp` | `OffscreenTarget` 新增单采样 `resolve_` 图像；`init()` 放开 samples 校验；`readback_rgba8` 改读 resolve 图 |
| `vulkan_resource_common.h/.cpp` | 新增全仓唯一的 `to_vk_samples()` |
| `vulkan_device_frame.cpp` | `create_pass()` 加 resolve 附件、framebuffer 追加 resolve 视图、`pass_compatible` 比较 resolve 有无 |
| `vulkan_frame_target.h` | `IFrameTargetImage` 加 `frame_resolve_image()`，**默认回落到 color，交换链不用改** |
| `vulkan_pipeline.cpp` 571-632 | `create_render_pass()` 加 resolve 附件 |

`vulkan_pipeline.cpp` 里 MSAA 改的是 `create_render_pass()`，
**与你改的描述符集段（451-537、758-781）不重叠**。

---

## 4. 区域划分（请相互避让）

| 范围 | 归属 |
|---|---|
| `vulkan_pipeline.cpp` 571-632（resolve）、`vulkan_pipeline.cpp` 719-724（`rasterizationSamples`） | MSAA |
| `vulkan_draw.cpp`、`vulkan_target.*`、`vulkan_resource_common.*`、`vulkan_device_frame.*`、`vulkan_frame_target.h` | MSAA |
| `vulkan_pipeline.cpp` 451-537、758-781（描述符集 / `sampled_texture_slots`） | **L1 P0** |
| `pipeline.h` 的 `sampled_texture_slots`、`command.h` 的 `bind_sampled_texture`、`vulkan_command.*` | **L1 P0** |

**共同规则**：
- 改动前先 `read` 确认最新内容，不要基于旧版本编辑
- 只做增量修改，不要重写整个函数
- 报 `FS_STALE_VERSION` 就重新 read 再改
- 发现对方正在改同一段代码 → 停下来记录冲突点，不要强行覆盖

---

## 5. 需要 L1 回报三件事

1. `texture_at` / `sampler_at` 的实现写了没有
2. `bind_sampled_texture` 在 `ICommandList`（第 12 个方法）和
   `VulkanCommandList` 里完成到哪一步
3. 预计什么时候能让 `ctest -j 1` 跑起来
   （两边都卡在构建上，谁先绿谁先验）

如果 MSAA 侧的操作挡住了你，直接说。反过来也一样。
