# L4-F-VX 依赖判断报告

日期：2026-11-09
前置：L3 已落地 + L4 首个玩法 F-SK 已落地（ctest **389/389**，0 失败 0 跳过，本次实跑复核）
关联：`docs/superpowers/specs/2026-11-03-magicblocks-render-engine-sdk-design.md`（§2.2 / §4 / §4.1 / §14 Q1）
　　　`docs/superpowers/specs/2026-11-04-magicblocks-l3-primitives-design.md`（§6A）
格式范本：`docs/superpowers/specs/2026-11-06-magicblocks-l4-fsk-dependency-assessment.md`

---

## 0. 一句话结论

**"渲染"与"通用能力"这一侧 L0–L3 基本齐备（射线→平面→AABB、IOverlay2D、IBatchBuilder/IPrimitiveRenderer 的双 pass、IWorld ECS、IRecorder 全部实测存在且够用）；**
**真正的缺口只有两块：① `IBlockSet / IBlockBrush / IShapeTemplate` 三个玩法数据模型整个不存在（按裁定本就属 L4，需从零写）；② `depthConstWrite` 的实现与 spec 描述不符（落成了 `depthBounds`，写不出"常量深度"）。**
后半块**不阻塞 F-VX 落地**——用"两次 bind_pipeline + 两次 draw"就能得到正确的内面叠加效果，但必须**把 §4 P12 的"常量深度写"表述改掉**，否则会在假前提上写代码。

---

## 1. 基线核实（2026-11-09 实测）

| 断言 | 结论 | 证据 |
|---|---|---|
| 构建 | ✅ 已有构建产物，无需重编 | `MagicBlocks/build/*.exe` |
| 测试 | ✅ **389/389**，0 失败 0 跳过，用时 49.46 s（本次实跑复核，非引用） | `ctest` 输出 `100% tests passed, 0 tests failed out of 389` |
| 测试总数 | ✅ 389 | `ctest -N` → `Total Tests: 389` |
| L3 模块数 | ✅ 16 头 + 16 源 | `include/magicblocks/primitives/` 列出 alloc/batch/camera/gizmo/material/mesh/mesh_factory/mesh_loader/overlay2d/painter/picker/primitive_renderer/shader_library/types/world/world_render |
| L4 层 | ✅ **已存在**（F-SK 已落地，与 F-SK 报告当时的"不存在"不同） | `include/magicblocks/features/sketch/`（sketch_document.h / sketch_pipeline.h）、`src/features/sketch/` |
| 软件光栅设备 | ✅ SwiftShader Device (LLVM 10.0.0)，`api_version=0x403000` | `magicblocks-l0-cli device` |
| 三个玩法数据模型 | ❌ **全仓 0 处** | 见 §4.1 |

> 说明：F-SK 报告（2026-11-06）记录的基线是 310/310；本次为 389/389，增量 79 条来自 F-SK 及其回归。

---

## 2. 依赖矩阵（F-VX 需要 vs L0–L3 实际提供）

| # | F-VX 需要 | L3 是否提供 | 提供者（证据） | 缺口 |
|---|---|---|---|---|
| D1 | **方块实例集合**（`Block{cell[3],color,stroke_id,flags}`、按 `stroke_id` 整笔删、按 AABB 查） | ❌ **缺失** | — （`IBlockSet` 全仓 0 处） | **硬缺口 G1** |
| D2 | **画笔扫掠求解**（路径点 + 半径/层高 → 去重 → emit） | ❌ **缺失** | — （`IBlockBrush` 全仓 0 处） | **硬缺口 G2** |
| D3 | **任意形状模板**（一组 Block / OBJ→方块） | ❌ **缺失** | — （`IShapeTemplate`/`IVoxelizer` 全仓 0 处） | **G3**（本轮可不做，标扩展点） |
| D4 | 屏幕像素 → 世界射线 | ✅ **完整** | `ICamera::screen_to_ray(float px, float py)` `camera.h:86` | — |
| D5 | 射线 vs 平面（求落笔平面交点） | ✅ **完整** | `IPicker::intersect_plane(ray, Plane, float*)` `picker.h:77` | — |
| D6 | 射线 vs AABB（查已有方块占用） | ✅ **完整** | `IPicker::intersect_box(ray, Box, float*)` `picker.h:69` | — |
| D7 | 射线 vs **方块实例**（聚合查询） | ⚠️ 部分 | `picker.h:5` 只列 AABB/三角形/平面/球；spec §4 P4 要求"方块实例"求交，实际无此入口 | 可**用 D6 线性扫**替代（≤10k 方块可接受），非阻塞 |
| D8 | 基础数学类型 `Vec3/Ray/Plane/Box/Mat4` | ✅ **完整** | `types.h:186(Box)/205(Ray)/212(Plane)` | — |
| D9 | 屏幕恒宽线 / 虚线 / 矩形（画格点网格指示） | ✅ **完整** | `IOverlay2D::push_line/push_dash/push_rect` `overlay2d.h:136-139`；`set_viewport` `:130` | 无内建 grid 图元，网格 = N 条 `push_line`（够用） |
| D10 | 方块几何（单位立方） | ✅ **完整** | `IMeshFactory::cube()` `mesh_factory.h:92`；`box_faces(mask)` `:98` | — |
| D11 | 实例化绘制（每实例 cell/color） | ✅ **完整** | `InstanceAttribs`（model/base_color/…）`batch.h:100`；逐实例绑定 `VertexInputBinding{instanced=true}` `pipeline.h:86`；CLI 样板 `l3_cli/main.cpp:1494-1524` | — |
| D12 | **一次双 pass**（表面 pass + 内面 pass） | ✅ **完整（无需改 L3）** | `IPrimitiveRenderer::record_frame` 遍历全部 `BatchDraw` 并逐批 `bind_pipeline(b.pipeline_hash)` `primitive_renderer.cpp:58,72-83`；`DrawRequest::pipeline_hash` 是主排序键 `batch.h:55` | 同一份方块 `push` 两次、用两个 `pipeline_hash`，即得 2 次 draw。**不是缺口** |
| D13 | 每次 draw 的深度/剔除状态可独立配置 | ✅ **完整** | 每个 `PipelineDesc` 自带 `DepthState`+`RasterState`，管线可用 `register_pipeline(key, pl)` 注册多份 `primitive_renderer.h:83` | — |
| D14 | **"内面渲染"= 关深度测 + 背面可见** | ✅ **完整** | `depthTest=false` + `RasterState::cull=None` `pipeline.h:146-152,154-158` | — |
| D15 | **"写常量深度"（depthConstWrite）** | ⚠️ **名义有、语义不符** | 字段存在 `pipeline.h:150-151`；实现落成 `depthBoundsTestEnable` + `minDepthBounds=maxDepthBounds=depth_const` `vulkan_pipeline.cpp:614-624` | **缺口 G4（关键修正）**，见 §3/§4.4 |
| D16 | 深度格式（离屏目标） | ✅ 有，但**硬编码 D16** | `vulkan_target.cpp:126` 固定 `DepthFormat::D16_UNORM`；`depth_attachment()` 亦固定 D16 `:152` | 与"常量深度"叠加时精度 ~1/65536，若真做常量深度需换 D32（G4 附注） |
| D17 | 方块实例集合的存储载体 | ✅ 二选一均可 | `IWorld` ECS 完整可用 `world.h:344+`（`spawn/add_component/view(mask)/snapshot`）；`build_draw_requests(world,builder,desc)` `world_render.h:144` | **裁定见 §3.2：不进 ECS，用独立线性数组** |
| D18 | 每块一个 stroke_id / 颜色 | ✅ 可表达 | `IWorld` 组件任意注册 `world.h:358`；但线性数组更直接 | — |
| D19 | 操作流录制（Down/Move/Up + 时间戳，可复现） | ✅ **完整** | `IRecorder::record_event` `recorder.h:127`；`EventRecord{kind,pointer_id,x,y,dx,dy,button,ts}` `command_stream.h:100-112`；`capture/replay/reproduce` `:154,165,172` | 无 `stroke_id` 概念，但那是 L4 语义，**录到事件层即可**，非缺口 |
| D20 | 命令流确定性（哈希稳定） | ✅ **完整** | `CommandStream::stream_hash` `command_stream.h:124`；整数 `cell` 天然稳定（§4.1 裁定） | — |
| D21 | 离屏出图 + PNG 落盘 | ✅ **完整** | `create_offscreen_target` / `readback_rgba8` `render_target.h:38` / `write_png_rgba8`；样板 `l3_cli` 四个子命令 | — |
| D22 | 帧内零分配 / 对象池 | ✅ **完整** | `IFrameArena`/`IBatchPool` `alloc.h`；批次池接线 `batch.h:142-143` | — |

**统计**：22 项中 **完整具备 16 项**，**部分/需替代 2 项（D7、D16）**，**语义不符 1 项（D15）**，**整块缺失 3 项（D1/D2/D3 属 L4 玩法层）**。

---

## 3. 关键修正

### 3.1 修正一：`depthConstWrite` 并没有"写常量深度"

| 文档表述 | 实测 | 结论 |
|---|---|---|
| L3 spec §6A.3：`depthConstWrite = near` → "写常量深度 → 稳定排序" | `vulkan_pipeline.cpp:614-624` 把它落成 `depthBoundsTestEnable=VK_TRUE` + `minDepthBounds=maxDepthBounds=depth_const` | ❌ **depthBounds 不写深度**，只做片元裁剪 |
| `pipeline.h:144-145`："depthWrite=true 且 depthConstWrite=true 时把整片区域写成 depth_const 值" | Vulkan 规范中 `VkPipelineDepthStencilStateCreateInfo` 没有"写常量深度"字段；`ds` 里与常量深度相关的只有 `minDepthBounds/maxDepthBounds` | ❌ **该语义在 Vulkan 1.0/1.1 中不可表达** |
| `pipeline.h:151`：`depth_const = 1.0f` 默认 "(near=0,far=1)" | `depth_const` 全仓仅在 3 处出现：哈希 `:236-237`、注释 `:615`、`minDepthBounds` `:619`。**从未作为"写入值"参与任何逻辑** | ❌ 三个证据点闭环 |

**证据（全仓穷举，`Select-String` 扫 `src/` 与 `tests/` 全部 .cpp/.h）**：

```
vulkan_pipeline.cpp:236  hash_mix(&h, desc.depth.depthConstWrite ? 1u : 0u);
vulkan_pipeline.cpp:614  ds.depthBoundsTestEnable = desc_.depth.depthConstWrite ? VK_TRUE : VK_FALSE;
vulkan_pipeline.cpp:618  if (desc_.depth.depthConstWrite) {
vulkan_pipeline.cpp:669  desc_.depth.depthConstWrite
magicblocks_l1_test.cpp:474/478/483  （仅字段 round-trip 断言）
```

`depth_const` 只在 `:237`（哈希）、`:615`（注释）、`:619`（`minDepthBounds`）出现 —— **没有任何"写值"通路**。

**新增的第二个事实（比 F-SK 报告更严）**：`depthBounds` **特性从未被启用**。
`vulkan_render_device.cpp:150-157` 用零初始化的 `VkPhysicalDeviceFeatures feats{}` 直接
`dci.pEnabledFeatures = &feats`，全仓搜不到对 `depthBounds` 的置位（`Select-String` 结果只有 `vulkan_pipeline.cpp` 里的 `depthBoundsTestEnable`）。
而 `vulkaninfo` 显示 SwiftShader 该特性**支持但未请求**：

```
vulkaninfo（SwiftShader 段）:  depthBounds = true      ← 支持
vulkan_render_device.cpp:150-157: feats{} 全零         ← 从未请求
```

按 Vulkan 规范，使用未启用的特性属未定义行为。

**实跑探针**（临时目录 `%TEMP%\mb_dep_probe\probe.cpp`，链接既有 `magicblocks.lib`，**未改任何源码**）：

```
device = SwiftShader Device (LLVM 10.0.0)；api_version = 0x403000
离屏目标 OK (with_depth=1)          ← D16
[surface (depthTest=ON,cull=Back)]        管线创建 OK / 出图+回读 OK
[inner   (depthConstWrite,cull=None)]     管线创建 OK / 出图+回读 OK
表面 pass 非背景像素 = 1536 / 3072
内面 pass 非背景像素 = 1536 / 3072
```

**读出两件事**：
1. SwiftShader **没有报错也没有整片剔除**，画面照常 —— 说明它忽略了未启用的 depthBounds（未定义行为下的"幸运"表现，**换驱动/换真机会变**）。
2. 因此"内面渲染"当前**能出像素，但靠的是"关深度测 + cull=None"本身**，`depthConstWrite` 是**摆设**。

### 3.2 修正二：`IBlockSet` 不该进 ECS —— 独立线性数组

| 方案 | 评估 | 证据 |
|---|---|---|
| 进 `IWorld` ECS（1 块 = 1 实体） | ❌ 10k 方块 = 10k 实体 + 10k 次 `add_component` 迁移；撤销一笔要 `erase_if` 逐实体 `despawn`（O(n) 次 swap-remove） | `world.h:396-398` 组件迁移是 O(列宽)；`world_render.h:144` 还要再走一遍 `build_draw_requests` |
| **独立 SOA 线性数组（采纳）** | ✅ 与 L3 spec §6A.1/§6A.4 完全一致（"10k 方块 ≈ 160~480 KB"、"`erase_if(stroke_id)` O(n)"）；渲染时**直接**灌 `InstanceAttribs` 后 `push`，不经 ECS | L3 spec §6A.1:346-352（`Block` 32B SOA）；§6A.4 表 |
| 混合（Block 数组 + ECS 仅存"场景装饰"） | ⚠️ 双源，易漂移 | — |

**裁定**：`BlockSet` 为 **L4 自持的 SOA 线性数组**，不进 `IWorld`。`IWorld` 留给 F-BL（魔法积木，那块确实需要实体语义/放置查询）。
这与 F-SK 报告 §3 的结论同构（"不需要 `IWorld` ECS"）。

### 3.3 修正三：双 pass **不需要新增任何 L3 接口**

F-SK 报告修正了"高估渲染通路"。F-VX 这里同理，且更强 —— 已有一个**现成的双管线单 pass 样板**：

```cpp
// l3_cli/main.cpp:1494  场景管线
cmd->bind_pipeline(scene_pl.value().get(), 0x4c33'5343'454e'45ull);
... draw_indexed(da);
// l3_cli/main.cpp:1527  覆盖层管线（同一条 command list，第二次 bind）
cmd->bind_pipeline(overlay_pl.value().get(), 0x4f56'524c'4159'0001ull);
... draw_indexed(da);
```

按 `IBatchBuilder` 语义，只要方块被 `push` 两次、两次给不同 `pipeline_hash`，
`record_frame` 就会按 `pipeline_hash` 主键排序（`batch.h:55`）并逐批 `bind_pipeline`（`primitive_renderer.cpp:83`），
**一次 `record_frame` = 2 次 draw = 2 个 pass**。`BatchStats.pipeline_switches` 预算 ≤16（`batch.h:89`），2 次切换远在预算内。

---

## 4. 缺口详述

### 4.1 G1 + G2：`IBlockSet` / `IBlockBrush`（唯一真正的硬缺口）

**证据（全仓穷举，`MagicBlocks/include` + `MagicBlocks/src` 递归扫 `.h`/`.cpp`，0 命中）**：

```
$ Get-ChildItem MagicBlocks\include,MagicBlocks\src -Recurse -Include *.h,*.cpp |
    Select-String -Pattern 'IBlockSet|IBlockBrush|IShapeTemplate|IVoxelizer|IWorldGrid|IVoxelGrid|ISpatialIndex'
（no output）
```

**这不是"漏做"，而是用户裁定**。两处文档明确把这三个类型划归 L4：

- 主 spec `:488-489`：
  > 注:`IBlockSet` / `IBlockBrush` / `IShapeTemplate` 属 **L4 玩法层**（主 spec §4 P7a/P7b/P7c），
  > 不在 L3 primitives 范围内（用户 2026-11-04 明确：严格按 L3 需求，未列入 L3 的不做）。
- `MagicBlocks/docs/L3_DELIVERY.md`（"明确不做"节）：
  > `IBlockSet` / `IBlockBrush` / `IShapeTemplate` / `IVoxelizer` / `IPlacementPolicy` / `FreeFloat`
  > **不在 L3 范围**。…由 L4 `src/features/**` 实现。

**结论**：G1/G2 属**预期缺失**，工作量应计在 L4，不应算作"L3 漏交付"。需从零写的量按 L3 spec §6A 已给出完整规格：

| 落地项 | 规格出处 | 关键点 |
|---|---|---|
| `Block` 结构 | L3 spec §6A.1:346-352 | `int32 cell[3]`（不是 float）+ `color` + `stroke_id` + `flags`，32 B，SOA 拆列 |
| 增删 | §6A.1 / §4.1 表 | `erase_if(stroke_id)` 整笔撤销 O(n) |
| 坐标换算 | §6A.1:358 | `world = origin + cell * cell_size`，`origin/cell_size` 为场景常量、进快照 |
| 落笔链路 | §6A.2:360-376 | `screen_to_ray` → 线段与层平面求交 → `floor((p-origin)/cell_size)` → 3D 超覆盖 DDA 去重 |
| 层锁定 | §6A.2 关键点 3 | `Down` 时锁层，`Move` 期间不变 |
| 改色 | §6A.2 关键点 4 | 只改 `Block.color`，不动几何 |

### 4.2 G3：`IShapeTemplate` / `IVoxelizer`

主 spec §4 P7c 规定它是"任意形状"扩展点。**本轮 F-VX 最小纵向切片可完全不做**（只做规则立方体，模板 = `cube()`），
按 spec 标为扩展点即可。若要做，`IMeshFactory::cube()` 已够用，无新增底层能力需求。

### 4.3 D7：`IPicker` 缺"方块实例"求交入口

`picker.h:5` 的注释自述只做 AABB/三角形/平面/球四类；spec §4 P4 写的是"求交器 AABB / 三角面 / 平面 / **方块实例**"。
差一个"把 BlockSet 包成可批量求交"的入口。**非阻塞**：落笔链路（§6A.2）走的是"射线 vs 平面 + 格点取整 + DDA"，
根本不依赖射线-方块求交；只有"点选已有方块"才需要，可用 `intersect_box` 线性扫代替。

### 4.4 G4：`depthConstWrite` 语义不符（**必须覆盖 spec 表述，否则会写错代码**）

见 §3.1 全部证据。落地处置建议：

| 处置 | 说明 |
|---|---|
| **A. F-VX 侧不用 `depthConstWrite`**（推荐） | 内面 pass 只用 `depthTest=false` + `depthWrite=false` + `cull=None` + `depthOp=Always`；靠"先表面后内面"的绘制顺序 + 不混合实现正确叠加。**无需改任何 L0/L1 源码**，无回归风险。 |
| B. 修 L1 实现（改为真常量深度） | Vulkan 无此原生能力，需走"片元着色器强制 `gl_FragDepth = const`" + 管线绑定标志位；属 L1 变更，要新开一条 shader 变体 + 重跑 389 基线 |
| C. 删掉 `depthConstWrite` 字段 | 破坏 `Pipeline.DepthStateDefaultsAndConstWrite`（`magicblocks_l1_test.cpp:464-484`），需同步改门控 |

**裁定**：走 **A**。同时在 spec §4 P12 与 L3 spec §6A.3 补一条勘误，
把"`depthTest=OFF + depthWrite=ON(depthConst=near)`"改成"`depthTest=OFF + cull=None + 分两遍绘制`"，
避免后续实现者照着假前提写。

**附注（D16）**：若将来真做常量深度，`vulkan_target.cpp:126` 的硬编码 D16 需一并换 D32（`rev_z_supported` 也依赖 D32，见 `vulkan_device.cpp:115`）。

---

## 5. 落地路径估算

| 阶段 | 内容 | 原估 | 修正后 |
|---|---|---|---|
| ~~0~~ | ~~新增 L3 数据模型 / 新增双 pass 接口~~ | 2 天 | **删除（判定不需要）** |
| **A** | `features/blocks/block_set`：`Block` SOA 数组 + `erase_if(stroke_id)` + AABB 查询 + 快照哈希 | 1.5 天 | **1 天**（规格已写死在 §6A，直接抄） |
| **B** | `features/blocks/block_brush`：`screen_to_ray` → 层平面求交 → `floor` 格点吸附 → 3D DDA 去重 → emit | 2 天 | **1.5 天**（射线/平面/Box 全部现成） |
| **C** | 双 pass 渲染：两份 `PipelineDesc`（表面 / 内面）→ 两 `pipeline_hash` → 方块 push 两次 → `record_frame` | 1 天 | **0.5 天**（`l3_cli:1494-1524` 现成样板） |
| **D** | `IOverlay2D` 格点网格（`push_line` × N，用同一 `cell_size` 保证"看到 = 落到"） | 0.5 天 | **0.5 天** |
| **E** | L4 CLI `magicblocks-fvx-cli` 无头离屏出图（照抄 paint/blocks） | 1.5 天 | **0.5 天** |
| **F** | 单测 + golden + 389 条基线零回归 | 1.5 天 | **1 天** |
| **G** | spec 勘误（§4 P12、§6A.3 的 depthConstWrite 表述） | — | **0.25 天**（§4.4 处置 A 的配套） |
| **合计** | | ~8 天 | **~5.25 天** |

**注**：相比 F-SK 的 4.5 天略长，主要差在 G1/G2 是**从零写数据模型 + 扫掠算法**（F-SK 的 `FromOutline` 是单个函数），
而渲染/叠加/出图三段都被既有样板大幅压缩。

---

## 6. 现在的执行决定

**依赖判断结论：可以开工，无需任何 L0–L3 源码改动。**

1. **数据模型自己写**：`IBlockSet` / `IBlockBrush` 落在 `src/features/blocks/`，按 L3 spec §6A 规格实现；**不进 `IWorld` ECS**（§3.2）。
2. **双 pass 用既有合批**：方块 `push` 两次 + 两个 `pipeline_hash`，一次 `record_frame` 出 2 次 draw（§3.3）。
3. **内面渲染走处置 A**：`depthTest=false + depthWrite=false + cull=None`，**不使用** `depthConstWrite`（§4.4）。
4. **格点网格用 `IOverlay2D::push_line`**，`cell_size` 与落笔共用同一常量。
5. **`IShapeTemplate` 本轮不做**，只留扩展点。
6. **补一条 spec 勘误**（§4 P12 / L3 §6A.3 的 depthConstWrite 表述），避免假前提扩散。
7. **验收**：PNG 落盘且非空 + 结构断言（`pipeline_switches ≤ 16`、`scene_draws == 2`、`erase_if` 后块数归零）+ **389 条基线零回归**。

**不做**：不改 `vulkan_pipeline.cpp`、不改 `pipeline.h`、不改 `vulkan_target.cpp`（D16 保持在 G4 处置 B 时再动）。

---

## 附：探针复现方式

探针源码放在 `%TEMP%\mb_dep_probe\probe.cpp`（**仓外，不入库**），链接既有 `MagicBlocks/build/magicblocks.lib`，
仅调用公共接口（`create_render_device` / `create_offscreen_target` / `create_pipeline` / `create_command_list` / `readback_rgba8`），
构造两份仅深度/剔除不同的 `PipelineDesc`，各出一张 64×48 图并逐像素统计。复现需 `/utf-8` 编译（源码含中文注释，936 代码页会撕裂）。
