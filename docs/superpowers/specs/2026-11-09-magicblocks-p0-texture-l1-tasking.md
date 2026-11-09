# P0 纹理采样通道 —— L1 接线任务书（已裁定方案 X1）

日期：2026-11-09
状态：**用户已裁定，待 L1 方实施**
上游：`2026-11-09-magicblocks-l4-fst-l0-l3-gap-list.md` §3
裁定人：用户（大肥鱼），2026-11-09

---

## 0. 裁定结论（用户原话落定）

| 项 | 裁定 |
|---|---|
| 路线 | **路线一：完整纹理双通道** |
| §3.4 方案 | **X1** —— `ICommandList` 加第 12 个方法 `bind_sampled_texture` |
| D13 `R8G8_UNORM` | **不加**，用 `RGBA8_UNORM` 打包（R=颜色，G=深度，B=A=0） |
| D14 盖印痕迹 | 不由 L3 补，F-ST 侧用 `IOverlay2D` 拼 |
| P0-5 `FromOutline` UV 归一化 | 不由 L3 承担，F-ST 侧用 `IMesh::bounds()` 自己归一化 |
| §6 并发假失败 | 顺手加 `RESOURCE_LOCK vulkan` |

**分工**：L1（`device/` + `resources/` + Vulkan 后端）由 **L1 会话**负责；
L3 侧（shader 模板、`PipelineDesc` 使用方、测试）由 L3 会话负责。

---

## 1. 为什么选 X1（记录裁定理由，避免后续反复）

`ICommandList` 现有 11 个录制方法。清单 §3.4 把它列为"需要架构裁定"，
因为补纹理采样必然要动 L1 公共接口。

选 X1 的理由：
- 总 spec §4 R3 列的本来就是 11 个**能力**，`bindPipeline` 的职责
  扩展到"绑定采样器"是自然的，与 R3 不冲突。
- "恰好 11 个"是当时的实现事实，不是不可违背的契约。
- X2（塞进 `bind_pipeline` 重载 / `RenderPassDesc`）语义含混：
  纹理是**逐 draw** 的，不是逐 pass 的，会埋坑。
- X3（push constant 传槽位 + UBO 纹理表）仍然需要描述符集访问那张表，绕不开。

---

## 2. L1 交付清单

### 2.1 `resources/pipeline.h` — `PipelineDesc` 增加采样纹理声明

```cpp
// PipelineDesc 内新增字段
u32 sampled_texture_slots = 0;   // 0 = 不建描述符集（默认，既有行为不变）
```

**必须满足**：默认 `0` → `IPipeline::descriptor_set_count()` 仍返回 0，
既有 5 张 golden **逐字节不变**。这是 P0-1 的验收口径。

实现建议：`sampled_texture_slots > 0` 时建 **1 个** descriptor set layout
（`VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER`，`descriptorCount = sampled_texture_slots`），
`descriptor_set_count()` 返回 1。**不做** bindless / 纹理数组 / 多集。

### 2.2 `resources/command.h` — `ICommandList` 加第 12 个方法

```cpp
/// 绑定一张可采样 2D 纹理到 slot。pipeline 必须声明过 sampled_texture_slots > slot。
virtual Status bind_sampled_texture(const ResourceId& texture,
                                    const ResourceId& sampler,
                                    u32 slot) = 0;
```

**同步要改的**：
- `ICommandList` 的方法数不变量（有门控/测试断言"恰好 11 个"）→ 改成 12
- 所有 `ICommandList` 实现：`VulkanCommandList` + 若有 mock/测试替身
- SQL / 门控脚本里引用方法数的地方
- `command_count()` 语义：这个绑定算不算一条命令？**建议算**，
  与 `bind_pipeline` 一致；若不算，需在头注释写明

### 2.3 `resources/texture.h` — 采样器状态

若 `ITexture` 尚无 sampler 描述，需要一个能表达 wrap/filter 的结构
（`SamplerDesc{min_filter, mag_filter, wrap_u, wrap_v, mip_mode}`）。
**若已有就复用，不新造**。

### 2.4 Vulkan 后端

- `VulkanPipeline`：按 `sampled_texture_slots` 建 layout + pool + set，
  `vkCreateSampler` / 写 `VkDescriptorImageInfo`
- `VulkanCommandList::bind_sampled_texture`：记录到命令流，`submit_device_frame`
  时下发 `vkCmdBindDescriptorSets`
- 命令录制/回放（`IRecorder` 双写）要覆盖这个新方法 —— **这是 L3 的命令录制回放约束**

### 2.5 单测（L1 侧自测）

| 用例 | 断言 |
|---|---|
| 默认不建描述符集 | `sampled_texture_slots=0` → `descriptor_set_count()==0` |
| 声明 1 slot | `sampled_texture_slots=1` → `descriptor_set_count()==1` |
| slot 越界 | `slot >= sampled_texture_slots` → 返回错误 Status，不崩 |
| 录制回放 | 录一遍含 `bind_sampled_texture` 的命令流，回放后纹理绑定生效 |

---

## 3. L3 侧（L3 会话负责，L1 不用管）

- **P0-4**：新增 `std_pbr_textured` shader 模板（only vert/frag，过 G1 门控），
  片元采样颜色贴图，顶点/片元按 uv 采样深度做位移
- **P0-1 使用方**：L3 的 pipeline 构造路径传 `sampled_texture_slots`
- **集成测试**：同一 mesh + 同一相机，**换纹理内容 → 像素随之变化**
  （这是 P0-3 的核心验收）
- **集成测试**：纹理绑定 + `draw_instanced` 同时可用（印章要多枚）
- **RESOURCE_LOCK vulkan**：`CMakeLists.txt` 给会用 Vulkan 的用例打锁

---

## 4. 验收口径（交付时自检）

| 项 | 验收 |
|---|---|
| 基线不回归 | `ctest -j 1` 全绿，既有 5 张 golden **逐字节不变** |
| P0-1 | `sampled_texture_slots=0` 时既有管线与像素逐字节不变 |
| P0-3 | 换纹理内容 → 输出像素变化（证明链路真的通了） |
| P0-3 | 纹理绑定 + 实例化 draw 同时可用 |
| P0-4 | 新 shader 过 `check_no_geometry_shader`（rc=0，只有 vert/frag） |
| 门控 | G1/G3/G4/G7/G8 全部 rc=0 |

> ⚠️ **验收一律用 `ctest -j 1`**。`-j 4` 会有 5~29 条用例以 `0xC0000409`
> 假失败（SwiftShader 多进程争用），不是回归。加了 `RESOURCE_LOCK vulkan` 后才可并行。

---

## 5. 高风险管理点（最容易出错的地方）

1. **`upload()` 之后必须 `flush_pending()`**。L1 的 `IBuffer::upload()` 只 memcpy 进
   Staging 并压 `pending_`，真正的 GPU 拷贝在 `flush_pending()` 里。漏掉 = 全零缓冲 =
   空图，**且不报任何错**。纹理上传同理。
   自检：绘制前断言 `pending_upload_bytes() == 0`。

2. **`Box{Vec3 lo, Vec3 hi}` 是陷阱**。`Box` 有 3 个成员 `{min,max,empty}`，
   `empty` 默认 `true`，聚合初始化只给两个参数会静默得到"空盒"。
   用 `Box::of(lo,hi)` 或显式传 `false`。

3. **描述符集布局与 shader 的 `set`/`binding` 编号必须对上**。
   建议固定：`set=0, binding=0` = 颜色贴图，`binding=1` = 深度贴图。

4. **命令录制回放**：`IRecorder` 是双写模式，新增的 `bind_sampled_texture`
   如果在录制态漏写，回放时纹理就绑不上，而**画面只是"没有纹理"不会报错**。

---

## 6. 给 L1 会话的开场白（可直接粘贴）

> 你的任务：按 `docs/superpowers/specs/2026-11-09-magicblocks-p0-texture-l1-tasking.md`
> 实施 P0 纹理采样通道的 L1 部分。用户已裁定走 **X1**：`ICommandList` 加第 12 个
> 方法 `bind_sampled_texture(texture, sampler, slot)`；`PipelineDesc` 加
> `u32 sampled_texture_slots = 0`，默认 0 时既有行为与 golden 逐字节不变；
> 不加 `R8G8` 格式（用 RGBA8 打包）。请只改 `src/device/**`、`src/resources/**`
> 和 Vulkan 后端，不要碰 `include/magicblocks/primitives/**` 和 `src/primitives/**`
> （L3 侧我来做）。完成后请回报改了哪些文件、单测结果、以及 `ctest -j 1` 全绿数。
