# 魔法功能预览 —— L4-F-SK 魔法涂鸦 设计

日期：2026-11-05
状态：**设计稿（待审阅门 ≥80 放行）**，不含代码改动
关联：
- 总设计：`docs/superpowers/specs/2026-11-03-magicblocks-render-engine-sdk-design.md`（下称"总 spec"）
- L3 原语：`docs/superpowers/specs/2026-11-04-magicblocks-l3-primitives-design.md`（下称"L3 spec"）
- 计算库选型：`docs/superpowers/specs/2026-11-03-magicblocks-compute-deps-selection.md`
- 架构索引：`MagicBlocks/docs/ARCHITECTURE.md`
- 配套执行计划：`docs/superpowers/plans/2026-11-05-magicblocks-l4-fsk.md`

---

## 0. 本轮用户指令（原文与解读）

用户 2026-11-04 的原话：

> 当前已经完成了 L0-L3，现在开始进行 L4 的落地。这个已经开始是玩法的构建了，必须一项一项来。
> 1. 使用 L0-L3 搭建的引擎基础，开始 F-SK 的魔法涂鸦功能实现。

用户 2026-11-05 对落地方式的裁定：

| 问题 | 用户裁定 |
|---|---|
| L3 有缺口（`IPainter`/`FromOutline`/渲染器不存在）怎么办 | **先只做设计文档（F-SK 设计 spec + 计划），不写代码** |
| F-SK 本轮范围 | **先等待，我检查前面任务完成情况** |

由此得到本轮**硬边界**：

- **本轮只产出文档**：一份设计（本文）+ 一份执行计划（plans）。**不动任何源码、不建工程、不跑构建**。
- **F-SK 是四项玩法中的第一项**（"必须一项一项来"）：本文不得顺带设计 F-VX/F-BL/F-ST 的落地，
  但必须为它们**预留通用扩展点**（否则又回到"专用通道"，违总 spec §2.2）。
- **F-SK 只允许组合 L0–L3 的通用能力**（总 spec §2.1、§2.2；L3 spec §2.1、§12）。
  一旦需要新增一个只有涂鸦用得到的公开接口，就说明 L3 抽象不足，应回头补 L3，而**不是**在 features 里开洞。

### 0.1 四件必须写进文档的事（来自此前对话的确认）

1. **L3 实际只落地了 4 个模块**（types / camera / shader_library / material），
   而 L3 spec §13 的 M1–M5 大部分还没落地。F-SK 依赖其中若干项 → 本文必须**如实列出缺口**
   并给出"按 F-SK 最小可用子集补齐"的边界（§3）。
2. **L1 的 `draw*` 只记录不下发**（plans-l1 的 U1 裁定），L3 新增的 `submit_device_frame` 是
   唯一"命令流真正出图"的通路，而它的 L3 侧钩子 `primitives/backend_hooks.h` **尚不存在**。
   F-SK 要出 PNG 就必须先补这条通路（§8）。
3. **依赖池已备齐** `libtess2-v1.0.2` / `Clipper2_2.0.1` / `tinyobjloader-v1.0.7` / `ink-stroke-modeler-main`
   （无拉取风险，无回退路径）。
4. **CLI + 离屏是硬约束**（build-pipeline SKILL §①）：本文必须给出无头入口与 PNG 落盘方式（§12）。

---

## 1. 现在的引擎能给什么：现状基线（全部实测，带证据）

> 口径：本节是**写文档时**（2026-11-05）从 `E:\workspace\myspace\Mine` 实跑 / 实读得到的结论。
> 文档与代码冲突时以代码为准，并回头修本文。

### 1.1 构建与测试基线

| 断言 | 实测结论 | 证据 |
|---|---|---|
| 构建可重复 | ✅ `powershell -ExecutionPolicy Bypass -File MagicBlocks\tools\build.ps1` 成功。**直接 `cmake --build` 会 LNK1181 找不到 kernel32.lib**（缺 vcvars 环境），构建必须走该脚本 | `MagicBlocks/tools/build.ps1:12-29` |
| 测试全绿 | ✅ `ctest` **193/193，0 失败 0 跳过** | 本轮开工前实跑 |
| 门控全绿 | ✅ G1/G3/G4/deps/scripts/scene_no_clock/ui 全 rc=0 | 同上 |

### 1.2 已落地的 L3 面（只有 4 个模块）

| 文件 | 内容 | 对 F-SK 的意义 |
|---|---|---|
| `include/magicblocks/primitives/types.h` | `Vec2/3/4` `Mat4` `Color4` `Box` `Ray` `Plane` `RayHit` + 求交纯函数 + `MeshId/MaterialSlot` | ✅ F-SK 全部依赖 |
| `include/magicblocks/primitives/camera.h` | `ICamera`（正交/透视、`screen_to_ray`、`world_to_screen`、`world_size_per_pixel`、orbit/pan/zoom、`snapshot/restore`） | ✅ 画布相机 |
| `include/magicblocks/primitives/material.h` | `StandardParams` / `MaterialDesc` / `MaterialKey` / `IMaterialTable::intern()` | ✅ 笔色材质 |
| `include/magicblocks/primitives/shader_library.h` | `IShaderLibrary` + `ShaderKey` + `RasterStateL3` + `TargetShape` + **已写死的顶点布局约定**与 96 字节 push constant 布局 | ⚠️ 只登记模板，**不建内置模板集** |
| `src/primitives/{types,camera,material,shader_library}.cpp` | 上述实现 | — |
| `tests/magicblocks_l3_test.cpp` | 50 条用例（Camera 21 / Math 6 / Intersect 7 / Material 10 / ShaderLibrary 6） | — |

### 1.3 尚未落地的 L3 面（F-SK 直接依赖项）

| 缺口 | L3 spec 位置 | F-SK 是否必需 | 说明 |
|---|---|---|---|
| `IPainter`（笔画 → 路径采样点） | §6 | ✅ **必需** | 涂鸦的输入口 |
| `IFillPath`（2D 路径光栅：线段 walk / 胶囊 / 圆头笔刷） | 总 spec P6 | ✅ **必需** | 与 `IPainter` 合并落地（P6 = M1 最小版 `IPainter`） |
| `IMeshFactory::FromOutline`（带孔轮廓 → 三角化 → 上下盖 + 侧壁） | P2 / M4 | ✅ **必需** | 2D 涂鸦 → 3D 挤出 |
| `IMesh` / `MeshData` 与图元 | P1 / M2 | ✅ **必需** | 顶点/索引数据载体 |
| `IPrimitiveRenderer`（把 World+Camera+材质变成 11 类命令 + 合批 + 实例属性） | §4.3 / §11 | ✅ **必需** | 但要的只是"一条 pass + 实例化 draw"的最小版 |
| `primitives/backend_hooks.h`（L3 侧不透明钩子） | 尚未定义 | ✅ **必需** | 否则 `draw*` 不出像素（§8） |
| L3 内置 shader 模板集（`std_unlit` / `stroke_ribbon` 等） | §3 / §9.3 | ✅ **必需** | 现在 `create_shader_library()` 只给空库 |
| `IWorld`（ECS） | §7 | ❌ 不需要 | F-SK 只有"一笔 = 一个实体"，用最简列表 + `IScene` 同步即可（§7.2 决策） |
| `IGizmoSystem` / `IPicker` / `IOverlay2D` | §10 / P4 / P5 | ❌ 不需要 | L3 spec §12 用途矩阵：F-SK 的 Gizmo ✖、拾取 ✖、覆盖层 ✖ |
| `IBlockSet` / `IBlockBrush` / `IShapeTemplate` / `IVoxelizer` | 总 spec P7a/b/c | ❌ 不需要 | 属叠叠乐，F-VX 阶段 |

> **结论**：F-SK 需要补齐的是 L3 spec 里 M1（`IPainter`）+ M2 的一小部分（`IMesh`/渲染器最小版）
> + M4 的一项（`FromOutline`）+ 一个新钩子头。**不需要**整层 ECS / Gizmo / Picker / Overlay2D。

### 1.4 渲染通路现状（F-SK 出图的关键前提）

| 断言 | 实测结论 | 证据 |
|---|---|---|
| L1 `draw*` 只记录不下发 | ✅ 命令进 `recorded_`，无 `vkCmdDraw` | `include/magicblocks/resources/command.h:113-116`；`src/backend/vulkan/vulkan_device_frame.h:3-7` |
| 已有"真下发"通路 | ✅ `backend::submit_device_frame(ctx, list, stats)` 把 11 类命令逐条翻译成 `vkCmd*` 并 fence 等待 | `src/backend/vulkan/vulkan_device_frame.h:58-68` |
| 管线按目标形状建 | ✅ `VulkanPipeline(ctx, desc, AttachmentShape)` + `hash_pipeline_desc_with_shape` | `src/backend/vulkan/vulkan_pipeline.h:62-63,144` |
| **L3 侧钩子** | ❌ **不存在**：`primitives/backend_hooks.h` 未被创建，grep 全仓无引用 | 本轮实查 |
| 无深度窗口可开 | ✅ `TargetDesc::with_depth`，`OffscreenTarget` 有 `depth_image()` | `include/magicblocks/device/render_target.h:34`；`vulkan_target.h:77` |

### 1.5 L2 命令模式现状

| 断言 | 实测结论 | 证据 |
|---|---|---|
| `IRecorder` 双写 + 三态 | ✅ 已落地，`commands.size() == command_count()` 是强不变量 | `include/magicblocks/scene/recorder.h:10-17` |
| 回放的语义 | ⚠️ **只做"流还原 + 哈希比对"，不重放出图** | `recorder.h:19-22` |
| 场景可进流 | ✅ `begin_frame(frame_index, extent, scene)` 完整快照 | `recorder.h:124` |
| 输入事件可进流 | ✅ `record_event(EventRecord)` | `recorder.h:127` |
| `--scene` 只认 `demo` | ✅ 遇到 `sketch` 等一律退出码 2 并指向 L4 | `src/l2_cli/main.cpp:22-23`；CTest `magicblocks_l2_scene_preset_rejected` |

> **对 F-SK 的直接结论**：录制/回放/复现的**骨架已经存在**，F-SK 要做的是
> ①把"笔画事件 → 渲染命令"做成**纯函数**，②把 `--scene sketch` 接上，③在回放时允许"重放出图"
> （这是 F-SK 验收门"PNG 逐像素一致"的前提，§11.5）。

### 1.6 L4 层现在有什么

| 项 | 状态 |
|---|---|
| `MagicBlocks/src/features/` | **不存在**（G3 门控还没扫它，§14 第 1 条） |
| G3 玩法名禁令 | 已生效：`backend/` 与 `primitives/` 内禁 `sketch/blocks/voxel/stamp/...`；`src/features/` 是**唯一豁免区** |
| 依赖池 | ✅ `libtess2-v1.0.2`、`Clipper2_2.0.1`、`tinyobjloader-v1.0.7`、`ink-stroke-modeler-main`、`glm-1.0.1` |

---

## 2. 需求拆解：F-SK 到底要什么

总 spec §4 对 F-SK 只给了一行：`IFillPath`（透明 2D 画布）→ `FromOutline` 挤出 → `ICamera` 正交/透视，
需求映射是"**2D 透明 → 3D 挤出面**"。

把这一行拆成可验收的需求点（编号 `R-SK-*`，后续逐条回溯）：

| # | 需求 | 来源 | 可度量验收 |
|---|---|---|---|
| **R-SK-1** | 透明 2D 画布：在画布平面上画笔画（触摸/指针事件驱动） | 总 spec §4 F-SK | 给定事件序列 → 笔画点数与端点坐标可断言 |
| **R-SK-2** | 笔画连续：`Move` 增量追加、不重建整笔；`Begin/Cancel` 语义正确 | L3 spec §6.2.1 | 每新增一个 `Move` 只追加新采样点（增量断言） |
| **R-SK-3** | 轮廓提取：笔画 → 带孔的闭合 2D 轮廓（多笔、自交、笔刷闭合圈） | L3 spec §6.4（`IPainter::extract_outline`） | 单笔 → 1 条外轮廓；两笔相交 → 外轮廓 + 孔（或合并，按规则 §6.3） |
| **R-SK-4** | 挤出成 3D：轮廓 → 三角化（含孔）→ 上下盖 + 侧壁 | 总 spec P2 | 三角数/顶点数守恒；水密性（每条边恰好 2 个三角形）可断言 |
| **R-SK-5** | 相机：正交与透视两种都能看挤出体 | 总 spec §4 F-SK | 同场景两相机各出一张 PNG，且都能看到几何 |
| **R-SK-6** | 3D 显示：不透明实体、随视角变化、深度正确 | 隐含（"3D 挤出面"） | 旋转相机前后 PNG 不同；遮挡关系正确 |
| **R-SK-7** | 无头离屏出 PNG | build-pipeline SKILL §①、总 spec §11.3 | CLI 退出码 0 + PNG 落盘 + 结构断言 |
| **R-SK-8** | 命令模式：录制 → 回放 → 复现，哈希与 PNG 一致 | 总 spec §11.4 | 三次哈希相等 + 逐像素一致 |
| **R-SK-9** | 可编辑性：撤销/重做一笔；改笔刷颜色不重建几何 | 玩法常识（详见 §7.4 裁定） | 撤销后 `stroke_count` 减一且像素回退；改色后顶点数不变 |
| **R-SK-10** | 零玩法分支（SDK 内不出现涂鸦专用分支） | 总 spec §2.1 / C5 | 门控 G3/G7 扫描 + 代码评审 |
| **R-SK-11** | 稳态帧热路径零分配、零 VMA 调用 | L3 spec C-L3-b、总 spec M5 | `AllocatorStats` 增量断言 + 计数器断言 |
| **R-SK-12** | **笔画双表示**：一笔同时携带 ①**路径**（带状描边，`PathStyle::Ribbon`）与 ②**路径包围的布尔区域**（闭合填充区，`PathStyle::Region`）。首轮实现 `Ribbon`，`Region` 预留接口与数据位但可后置 | 用户 2026-11-05 补充要求 | 同一事件序列在两种样式下产生不同的 `OutlineSet`：`Ribbon` 的轮廓宽 = 笔宽；`Region` 的轮廓 = 路径围成的多边形（不自带笔宽） |

> **R-SK-12 的来由与边界（用户 2026-11-05 原话）**：
> "后续的笔画路径分为**路径**，和**路径包围的布尔区域**，需要预留一下。"
>
> 这条要求的本质是：**笔画的下游用途不止一种**。
> - 当成"**路径**"用 → 笔画是一条有宽度的带子（描边/笔画本身），挤出后是一根管/带；
> - 当成"**路径包围的布尔区域**"用 → 笔画围出的是**一片面**（像用笔圈出一块地），
>   这片面要和其它笔画做**布尔运算**（并集/差集），再整体挤出。
>
> 这两者**共用同一份输入数据**（采样点），只在"如何从采样点得到轮廓"这一步分叉。
> 因此预留的正确位置是 **`PathStyle` 参数 + `OutlineSet` 的来源标注**，
> 而**不是**新增一套笔画类型或新增玩法分支 —— 否则就违总 spec §2.1 的"零玩法分支"。
>
> **本轮（首轮）范围**：实现 `Ribbon`（含圆头/方头封口）+ **`Region` 的接口与数据位**；
> `Region` 的布尔求值内核（差集等）落地在**后续阶段**（§3 表中标注为"预留"）。
> 理由见 §6.3.1 的可行性分析：`Region` 需要 Clipper2 的"开放路径→闭合区域"求值，
> 这比首轮需要的并集复杂，且**首轮验收（四张 PNG）不依赖它**。

> **裁定 D-SK-9（本条由本设计提出，已在 2026-11-05 经用户授权"使用你的判断"采纳）**：R-SK-9 的"撤销/重做/改色"是从玩法常识补的，
> 总 spec 未明文要求。本文按**保留**设计（成本很低：笔画列表 + 一次重挤出），并在 §7.4 给出关闭开关。

---

## 3. 范围裁定：补齐哪些 L3（最小可用子集）

> **原则**：只补 F-SK 真正需要的、且被 L3 spec 明确列出过的原语；**不新增涂鸦专用接口**；
> 不做 ECS/Gizmo/Picker/Overlay2D（它们对 F-SK 无需求，L3 spec §12 矩阵已显示 ✖）。

| L3 项 | 落地与否 | 理由 | 是否会被后续玩法复用 |
|---|---|---|---|
| `primitives/mesh.h`（`IMesh`/`MeshVertex`/baked 顶点布局） | ✅ 新增 | 所有 3D 玩法都要 | F-VX/F-BL/F-ST |
| `IMeshFactory`：`Quad`（画布）+ `FromOutline`（含孔三角化 + 上下盖 + 侧壁） | ✅ 新增 | F-SK 核心；F-ST 底板轮廓也要 | F-ST |
| `IMeshFactory`：`Cube/Box/Cylinder/Cone/Sphere/Trapezoid/Prism/Prism` | ❌ 不做 | F-SK 用不到（M4 的其余项） | F-BL/F-VX 阶段再补 |
| `IMeshLoader::Obj` | ❌ 不做 | F-SK 用不到 | F-BL 阶段 |
| `IPainter`（`feed()` / `extract_outline()` / 增量条带） | ✅ 新增 | F-SK 核心 | F-VX 笔刷路径采样 |
| `IFillPath`（= `IPainter` 的路径采样内核：线段 walk / 胶囊 / 圆头） | ✅ 新增（并入 painter.cpp） | P6 即 M1 最小版 | F-VX |
| **`PathStyle` / `RegionOp` / `OutlineSet.source_style`**（R-SK-12 双表示） | ✅ 接口与数据位**全部预留**；`Ribbon` 实现、`Region` 仅闭合（布尔后置） | 用户 2026-11-05 要求 | F-ST 底板轮廓、后续涂鸦区域模式 |
| `IPrimitiveRenderer`（最小版：1 个 pass + 实例化合批 + 实例属性环） | ✅ 新增（最小版） | 出图必需 | 全部玩法 |
| L3 内置 shader 模板集（`std_unlit` + `stroke_ribbon` + `canvas_quad`） | ✅ 新增 | 出图必需 | 全部玩法 |
| `primitives/backend_hooks.h` + 后端实现 | ✅ 新增 | 出图必需（§8） | 全部玩法 |
| `IWorld` ECS / `IGizmoSystem` / `IPicker` / `IOverlay2D` / 池（`IFrameArena`/`IBatchPool` 的完整版） | ❌ 不做 | 无需求 | 后续玩法阶段 |
| Clipper2 布尔合并（多笔轮廓互相合并成单一外轮廓） | ⚠️ **仅用于"重叠合并"策略**，见 §6.3 裁定 | 玩法手感 | — |

---

## 4. 总体架构：F-SK 在四层里的位置

```
┌──────────────────────────────────────────────────────────────────────┐
│ L4 features/sketch/   ← **唯一允许出现"涂鸦"语义的地方**               │
│   sketch_feature.h/.cpp     SketchFeature: InputEvent → 笔画 → 轮廓→网格 │
│   sketch_document.h         SketchDocument: 笔画列表(可快照/可撤销)     │
│   sketch_params.h           SketchParams: 画布尺寸/笔宽/挤出厚度/相机预设 │
├──────────────────────────────────────────────────────────────────────┤
│ L3 primitives/  (本次只补 F-SK 需要的子集)                            │
│   painter.h   IPainter / StrokeDesc / StrokeEvent / OutlineSet      │
│   mesh.h      IMesh / MeshVertex / MeshData                          │
│   mesh_factory.h  IMeshFactory::Quad / FromOutline(ExtrudeParams)   │
│   renderer.h  IPrimitiveRenderer(最小版：pass + 合批 + 实例属性环)     │
│   backend_hooks.h  (不透明钩子：submit_frame / create_pipeline_*)    │
│   shader_library.h 已有：+ 内置模板集 std_unlit / stroke_ribbon       │
│   camera.h / material.h / types.h 已有(不改)                         │
├──────────────────────────────────────────────────────────────────────┤
│ L2 scene/  已有：IScene / 渲染图 / CommandStream / IRecorder(不改)   │
├──────────────────────────────────────────────────────────────────────┤
│ L1 resources/ 已有：IBuffer/ITexture/IPipeline/ICommandList(不改)    │
├──────────────────────────────────────────────────────────────────────┤
│ L0 device/ 已有：IRenderDevice / IRenderTarget(不改)                 │
├──────────────────────────────────────────────────────────────────────┤
│ backend/vulkan  已有：submit_device_frame(不改；只被 hooks 调用)      │
└──────────────────────────────────────────────────────────────────────┘
```

### 4.1 文件布局（本次新增/修改，只在计划阶段确认，代码在下一轮写）

```
MagicBlocks/include/magicblocks/primitives/
  mesh.h              ← 新增：IMesh / MeshVertex / MeshData / baked 顶点布局常量
  mesh_factory.h      ← 新增：IMeshFactory + ExtrudeParams + Quad/FromOutline
  painter.h           ← 新增：IPainter / StrokeDesc / StrokeEvent / OutlineSet / OutlineOptions
  renderer.h          ← 新增：IPrimitiveRenderer / FrameDesc / RenderStats
  backend_hooks.h     ← 新增：L3 侧不透明钩子（零 Vk*）
MagicBlocks/src/primitives/
  mesh.cpp mesh_factory.cpp painter.cpp renderer.cpp
  embedded_shaders.h  ← 新增：内置模板集的 SPIR-V 引用（由 CMake 内嵌头提供）
  backend_hooks.cpp   ← 新增：弱符号分发到 backend（见 §8.2）
MagicBlocks/src/backend/vulkan/
  vulkan_hooks.cpp    ← 新增：实现 hooks（唯一 Vk 侧）
MagicBlocks/shaders/
  std_unlit.vert/.frag      ← 新增：实例化纯色/顶点色 3D（挤出体）
  stroke_ribbon.vert/.frag  ← 新增：屏幕空间笔画条带
  canvas_quad.vert/.frag    ← 新增：全屏画布/清屏（可选）
MagicBlocks/include/magicblocks/features/sketch/
  sketch_document.h   ← 新增：笔画列表 + 快照
  sketch_feature.h    ← 新增：SketchFeature（组合调用 L3）
MagicBlocks/src/features/sketch/
  sketch_document.cpp sketch_feature.cpp
MagicBlocks/src/fsk_cli/main.cpp        ← 新增：magicblocks-fsk-cli（无头、离屏）
MagicBlocks/tests/magicblocks_fsk_test.cpp  ← 新增
MagicBlocks/tests/data/golden_fsk_*.png     ← 新增
MagicBlocks/CMakeLists.txt                  ← 改：源文件 + shader + CLI + CTest
tools/check_l4_no_engine_names.py           ← 新增：G8（§14）
tools/tests/test_check_scripts.py           ← 改：加 G8 正/反例（**不新增 test_*.py**）
```

> **命名与门控**：`src/features/**` 与 `include/magicblocks/features/**` 是**唯一**允许出现
> `sketch` / 涂鸦 等玩法词的地方。L3 新增文件里**不得**出现（G7/G8 守着）。

---

## 5. 数据模型：`SketchDocument`（L4）

### 5.1 为什么玩法层要有一个"文档"

F-SK 的权威状态是"**用户画了哪些笔画**"，而不是"世界上有哪些三角形"。
把它做成一个可快照的值，才能满足：
- 命令模式的**复现**（§11.5：从同一初始文档 + 同一事件流重跑，得到同一份文档）；
- 撤销/改色（§7.4：改笔画属性后**重新挤出**，比改网格便宜也更不容易出错）。

### 5.2 结构

```cpp
namespace magicblocks::features {

// 画布参数(**场景常量**，进快照；不随帧变化)。
struct CanvasParams {
  // 画布在"画布平面"上的世界尺寸（单色 2D 坐标：原点在画布左下，+x 右，+y 上）。
  // 屏幕像素与画布坐标的换算 = 画布相机(P9 正交)的 screen↔world，不另造一套。
  float width = 2.0f;
  float height = 1.5f;
  float extrude_thickness = 0.08f;   // 沿画布法线(+z)挤出厚度
  bool  clamp_to_canvas = true;      // 笔画走出画布：夹到边界（§7.3 裁定）
};

// 一笔：**逻辑数据**，不是几何。
struct SketchStroke {
  std::uint32_t stroke_id = 0;       // 单调递增，不复用（撤销后不回收，保证命令流稳定）

  // ★ R-SK-12 的双表示：同一份 points，两种下游语义（详见 §6.3.1）。
  //   PathStyle::Ribbon  → 路径：按笔宽 offset 成带状轮廓（首轮实现）
  //   PathStyle::Region  → 路径包围的布尔区域：路径围成的多边形参与布尔运算（预留）
  PathStyle   path_style = PathStyle::Ribbon;
  // 布尔组合方式。仅当 path_style == Region 时有意义；
  // 首轮只实现 Union（其它值返回 Unsupported，不静默按 Union 处理）。
  RegionOp    region_op  = RegionOp::Union;

  StrokeDesc desc{};                 // 笔宽/颜色/是否平滑（L3 primitives 的值类型）
                                   //   ★ 注意：Region 语义下 width 不参与轮廓生成
  std::vector<StrokePoint> points;   // 采样点（画布 2D 坐标 + 时间戳），见 §6.2
  bool closed_cap = false;           // 是否首尾闭合（自成一圈 → 挤出后有孔）
};

// 文档 = 笔画列表 + 画布参数。**值语义快照**（可直接进命令流、可哈希）。
struct SketchSnapshot {
  CanvasParams canvas{};
  std::vector<SketchStroke> strokes;
  std::uint32_t next_stroke_id = 1;
  std::uint64_t doc_hash = 0;        // 规范化哈希（§11.4）
};

class ISketchDocument {
 public:
  virtual Status add_stroke(SketchStroke stroke) = 0;   // R-SK-1/2
  virtual Status undo_last() = 0;                        // R-SK-9（D-SK-9 保留）
  virtual Status redo() = 0;                             // R-SK-9
  virtual Status set_stroke_color(std::uint32_t stroke_id, Color4) = 0;  // R-SK-9，不动几何
  virtual const std::vector<SketchStroke>& strokes() const = 0;
  virtual bool can_undo() const = 0;
  virtual bool can_redo() const = 0;
  virtual SketchSnapshot snapshot() const = 0;
  virtual Status restore(const SketchSnapshot&) = 0;
};

std::unique_ptr<ISketchDocument> create_sketch_document(const CanvasParams&);
}
```

### 5.3 决策

| 编号 | 决策 | 备选 | 理由 |
|---|---|---|---|
| **D-SK-1** | 笔画存**逻辑采样点**，几何由"挤出"现场生成 | 存三角网格 | 复现/撤销/改色都要回到逻辑层；存网格则每次改色都要重建 |
| **D-SK-2** | 坐标统一在**画布平面 2D**（世界单位），不存像素 | 存像素坐标 | 像素与视口绑定，改窗口尺寸文档就废；画布↔像素只在 `ICamera` 一处换算 |
| **D-SK-3** | `stroke_id` 单调不复用 | 复用已删 id | 命令流/哈希稳定（与 L3 spec D-L3-7"整数抗漂移"同一思路） |
| **D-SK-4** | 文档快照**独立编码**进命令流（L3 spec D-L3-5 的口径） | 塞进 L2 `CommandStream.scene` | L2 的 `IScene` 节点树表达不了"笔画"；且改 L2 二进制会破既有 golden |

---

## 6. 画的链路：事件 → 笔画 → 轮廓 → 网格

### 6.1 全景

```
device::InputEvent ──①映射──► StrokeEvent ──②IPainter.feed()──► StrokePoint[]（画布 2D）
                                                                    │
                                                         ③IPainter.extract_outline()
                                                                    ▼
                                                        OutlineSet{ 外轮廓[], 孔[] }
                                                                    │
                                                         ④IMeshFactory::FromOutline()
                                                                    ▼
                                        IMesh{ 顶点/法线/索引, 上下盖 + 侧壁 }
                                                                    │
                                                         ⑤渲染（实例化 draw + 深度）
                                                                    ▼
                                                        离屏 PNG / 屏幕
```

**这条链的每一跳都是纯函数**（同输入同输出、无时钟、无随机），这是 R-SK-8 复现的前提。

### 6.2 ① 事件映射（`InputEvent` → `StrokeEvent`）

- 复用的是**已有的 L0 事件流**（总 spec D6）：`device::InputEvent` 是唯一事件源，
  命令复现回放同一事件流。**不新增 L4 事件类型**。
- **已核实**（`include/magicblocks/device/events.h:35-53`）：字段为
  `kind / pointer_id / position(Vec2f{f32 x,y}) / delta / button / width / height / timestamp_us / handled`，
  `InputEventKind` 含 `PointerDown/PointerMove/PointerUp/PointerCancel`（`events.h:15-28`）。
  与 L3 spec §6.2 的 `StrokeEvent{kind, pointer_id, x, y, timestamp_us}` **可 1:1 映射**：
  `x = position.x`、`y = position.y`、`timestamp_us` 原样带过。
- L2 的 `scene::EventRecord` 已经能进命令流（`recorder.h:127`），所以**录制只记事件**。
- **时间戳只用流里的值**，不读时钟（`check_scene_no_clock.py` 口径，且该门控要扩到
  `src/features/**`，见 §14）。

### 6.3 ② 笔画采样与 ③ 轮廓提取（`IPainter`）

L3 spec §6 已经定了接口骨架，本设计补两处**它留给 L4 的空白**：

**(a) 屏幕 → 画布的投影方式**

| 方案 | 说明 | 评价 |
|---|---|---|
| A. 用**画布相机**正交投影的逆（`screen_to_ray` ∩ 画布平面） | 复用 `ICamera`（L3 spec D-L3-4 的唯一换算点） | ✅ **选定** |
| B. 直接用像素坐标线性映射到画布 | 自造一套换算 | ❌ 与 D-L3-2 冲突，后续透视画布就废了 |

**裁定 D-SK-5**：画布输入一律走 `ICamera::screen_to_ray(px,py)` ∩ `Plane{normal=画布法线, d}`。
正交画布下这条射线等价于像素→画布的线性映射，透视画布下依然正确（为 F-ST 复用）。

**(b) 轮廓提取策略：重叠笔画的合并方式**

| 策略 | 行为 | 成本 |
|---|---|---|
| `PerStroke` | 每笔一个独立挤出体（一笔一个网格，互相穿插） | 低，但重叠处会看到内部面阶跃 |
| **`Merge`（选定）** | 所有笔画轮廓 → Clipper2 并集 → 一条外轮廓 + N 个孔 | 每帧/每次改动一次布尔 |

**裁定 D-SK-6**：默认 `Merge`。理由：涂鸦的核心观感是"画的东西连成一片"，
`PerStroke` 在交叉处会出现明显的 z-fighting 与内部面。Clipper2 已在依赖池（§1.6），无拉取风险。
`StrokeDesc::fill_rule`（非零/奇偶）与合并开关成为**参数**，不是玩法分支。

**(c) 笔刷封口形态**

- 圆头笔刷（`round_cap=true`）：线段扫掠成"胶囊"，封口是半圆；
- 方头：直角封口；
- 两条都由 `IFillPath` 的线段 walk 生成，**不新增接口**。

**性能约束（C-L3-b）**：`feed()` 只追加新段采样点，**不重建整笔**；
`extract_outline()` 的合并与三角化是**dirty 驱动的**——只有笔画发生变化才重算（§9.2）。

### 6.3.1 R-SK-12：路径 vs 路径包围的布尔区域

> 用户 2026-11-05：**"后续的笔画路径分为路径，和路径包围的布尔区域，需要预留一下。"**

#### (a) 两者到底是什么

同一串采样点 `p0…pn`，有两种下游解读：

| 表示 | 几何含义 | 轮廓来源 | 视觉结果（挤出后） |
|---|---|---|---|
| **路径 `Ribbon`** | 一条**有宽度的带子**，宽度 = `StrokeDesc::width_px` | 沿中心线两侧各偏移 `width/2`，两端按 `round_cap` 封口 | 一根"管/缎带"，像马克笔画的一道 |
| **区域 `Region`** | 路径**围成的面积**，宽度概念不适用 | 把路径当**闭合多边形的边界**（自动闭合首尾），得到一片面 | 一块"薄片"，像用笔圈出一块地再整片凸起 |

判别式很直接：**路径关心"线有多粗"，区域关心"圈起了多大面积"**。
用户在涂鸦里既可能画线（路径），也可能画圈再填（区域），两者都要支持。

#### (b) 为什么它是"预留"而不是"本轮实现"

可行性核对（这是不给"回退方案"、而是如实分期的依据）：

| 环节 | `Ribbon`（首轮） | `Region`（预留） | 依赖是否已就绪 |
|---|---|---|---|
| 采样点 | 相同 | 相同 | ✅ 共用，无额外成本 |
| 轮廓生成 | 中心线 **offset**（Clipper2 `InflatePaths`，开放路径+圆头） | 路径**闭合**成 Polygon，无需 offset | ✅ Clipper2 已在池 |
| 多笔布尔 | 各笔 ribbon 轮廓**并集**（`Union`） | 需要 `Union` **与** `Difference`（"挖洞"式阴影/高光常用） | ⚠️ Clipper2 都有，但**差集的语义与叠加顺序要单独定义** |
| 与路径互操作 | — | **区域 与 路径必须能共存并正确合成**（例如先画区域再描边） | ❌ 首轮未定义合成规则，需专门一轮设计 |
| 默认颜色/深度 | 单色 | 区域可能带**填充色**与**描边色**（双色） | ❌ `StandardParams` 单色不足，要实例属性扩展 |

**因此**：`Region` 的**数据位与接口**在首轮就留好（改起来零成本），
但**布尔求值内核与双色填充**留到后续阶段。这样"预留"是真的预留，而不是"以后推倒重来"。

#### (c) 落地方案：参数 + 来源标注，**不新增笔画类型**

```cpp
// ==== L3 primitives（本轮新增，见 §3）====
// 路径的"下游用途"参数：不是玩法分支，任何玩法都能用参数组合出来。
enum class PathStyle : std::uint8_t {
  Ribbon = 0,   // 路径：按笔宽 offset 成带状轮廓
  Region = 1,   // 区域：路径闭合后作为多边形边界
};

enum class RegionOp : std::uint8_t {
  Union = 0,        // 并集（首轮唯一实现）
  Difference = 1,   // 差集（预留：从已有区域挖掉）
  Intersect = 2,    // 交集（预留）
  Xor = 3,          // 异或（预留）
};

struct OutlineOptions {
  PathStyle        path_style = PathStyle::Ribbon;
  RegionOp         region_op  = RegionOp::Union;
  bool             merge       = true;   // 多笔轮廓是否并集（D-SK-6）
  float            width       = 6.0f;   // 仅 Ribbon 使用
  bool             round_cap   = true;   // 仅 Ribbon 使用
  std::uint32_t    curve_steps = 8;      // 圆弧离散段数（Ribbon 封口 / 平滑）
};

// 轮廓集合：**显式标注来源**，让下游（挤出/调试/测试）知道这片轮廓是怎么来的。
struct OutlineSet {
  std::vector<Contour> outer;      // 外轮廓（CCW）
  std::vector<Contour> holes;      // 孔（CW）
  PathStyle source_style = PathStyle::Ribbon;   // ← 预留：来源标注
  std::uint64_t source_hash = 0;                // 生成它的输入哈希（dirty 检测/复现校验）
  Box bounds{};                                 // 画布 2D 包围盒（挤出后变换用）
};
```

**为什么这是"通用能力"而不是"涂鸦专用"**：
`PathStyle` 描述的是**2D 路径几何的两种标准解释**（描边 / 填充），
这在任何 2D 内容里都成立（印章底板轮廓、甚至叠叠乐的笔刷路径都能用），
因此它属于 L3 `IPainter` 的参数，符合"抽象粒度判据 A1/A2"（≥2 处复用 / 有性能杠杆）。
**L4 只负责"选哪一种"**，不写 `if (is_sketch) ...`。

#### (d) 首轮的实现边界（写死，避免执行阶段自行扩张）

| 项 | 首轮 | 备注 |
|---|---|---|
| `PathStyle::Ribbon` | ✅ **完整实现** | A5/A6 验收针对它 |
| `PathStyle::Region` 的**采样→闭合多边形** | ✅ 实现 | 只是"首尾闭合"，成本极低 |
| `PathStyle::Region` 的**布尔组合** | ⚠️ **仅接收参数**；`region_op != Union` → 返回 `Unsupported` | **明确报错，不静默降级** |
| 区域与路径的**混合合成规则** | ❌ 不定义 | 留待后续一轮（见 (b) 表最后两行） |
| CLI | `--path-style ribbon\|region`，`region` 只保证单笔闭合填充可用 | — |

#### (e) 验收如何覆盖"预留"的有效性

预留必须**可验证**，否则等于没做。首轮加两条用例：

- `Outline.RegionStyleClosesPath`：同一事件序列 → `path_style=Region` 的 `outer` 是闭合多边形，
  且 **不含** 笔宽 offset（验证"区域"与"路径"确实分叉）；
- `Outline.UnsupportedRegionOpReported`：`region_op=Difference` → 返回 `Unsupported` 且 message 点名
  （验证"未实现的能力显式报错"，符合 D8 不回退）。


### 6.4 ④ 挤出（`IMeshFactory::FromOutline`）

```
OutlineSet{ outer[], holes[] }
   │
   ├─ libtess2（GLU tessellator 语义）：非零/奇偶填充规则 + 带孔三角化 → 上盖三角形
   ├─ z=0 上盖 + z=-thickness 下盖（法线分别 +z / -z）
   └─ 沿轮廓边生成侧壁四边形（每边 2 三角形，法线为边法向）
        ├─ 外轮廓：法线朝外
        └─ 孔轮廓：法线朝**内**（即朝孔心），保证挤出体是封闭实体
```

`ExtrudeParams`（L4 通过参数组合，不是新接口）：

| 字段 | 说明 |
|---|---|
| `thickness` | 挤出厚度（沿画布法线） |
| `bevel` / `bevel_radius` | 可选倒角（F-SK 初期为 0；预留给 F-ST） |
| `cap_top` / `cap_bottom` | 是否生成上/下盖（F-ST 印面可能要单面） |
| `winding` | 三角形绕序（与 `FrontFace::CounterClockwise` 对齐，供背面剔除） |

**关键验收（R-SK-4 水密性）**：对生成的 `IMesh` 做"每条无向边恰好出现 2 次"的断言——
这是"挤出体是封闭实体"的机械判据，也是 3D 遮挡正确的前提。

### 6.5 ⑤ 渲染（`IPrimitiveRenderer` 最小版）

L3 spec §4.3 的合批器本次只落地**F-SK 需要的最小集**：

```
每帧：
  ① 相机 → PushConstantsFrame{ view_proj(64B) + viewport(16B) + misc(16B) }   ← 96B，每帧 1 次
  ② 挤出体：一次 draw_instanced（顶点/索引缓冲 + 逐实例：世界矩阵4列 + base_color + params）
     材质参数逐实例（L3 spec §9.2 规则 2），因此"改一笔颜色"= 改实例数据，不切管线、不拆 draw
  ③ 笔画条带（可选叠加，用于"正在画的那一笔"预览）：stroke_ribbon + 屏幕空间顶点
```

- **无几何着色器（G1/G2）**：全部顶点来自已烘焙的 `IMesh` 缓冲，批量靠 `draw_instanced`。
- **深度**：F-SK 画布是 3D 实体，目标必须 `with_depth=true`；
  管线要按目标形状派生（`TargetShape{color_format, has_depth=true, samples}`，L3 spec §9.3 已有）。
- **drawcall 预算**：单帧 ≤ 3（1 次挤出体 + 1 次笔画预览 + 1 次可选背景）。

---

## 7. 交互与玩法行为（R-SK-1/2/9）

### 7.1 状态机（显式，可断言）

```
Idle ──PointerDown(画布内)──► Drawing ──PointerMove──► Drawing（追加采样点）
  ▲                              │                         │
  │                              └──PointerUp──────────────┘──► Idle（提交一笔）
  │                              └──PointerCancel / 无效事件 ──► Idle（丢弃该笔，不提交）
```
非法转移（Idle 收到 Move）→ **忽略并计数**（不崩溃，测试断言计数为 0）。

### 7.2 与世界（`IScene`）的关系

F-SK **不引入 ECS**（§3）：一笔一个挤出体，笔画的增删直接同步为 `IScene` 上的节点
（`add_node` / `remove_node`），交由 L2 的 `IRecorder::begin_frame(..., scene)` 快照进命令流。

> 这符合 L3 spec §7.3 的决策 B："`IScene` 是进入命令流的权威快照"——
> F-SK 只是没有 ECS 这一层中介，因为它的实体数 = 笔画数，没有列式存储收益。

### 7.3 边界情况（R-SK-11 之外的健壮性）

| 情形 | 行为 |
|---|---|
| 笔画走出画布 | `clamp_to_canvas=true` → 夹到边界；`false` → 明确允许（由参数决定，不静默） |
| 单点笔画（只有 Down 没有 Move） | 生成一个圆点（半径 = 笔宽/2），仍可挤出 |
| 采样点过密（< 0.5px 间隔） | 按最小间距剔除（`IPainter` 内部，确定性规则） |
| 笔画数 > 上限（默认 256） | `add_stroke` 返回明确错误（`OutOfRange`），不静默丢弃 |
| 轮廓三角化失败（退化/自交未处理） | 返回 `Status` 错误（libtess2 失败不吞），CLI 退出码非 0 |
| 撤销后重做 | 恢复同一 `stroke_id` 与采样点（文档快照往返） |

### 7.4 R-SK-9 的开关（D-SK-9）

若用户确认"只要验收门四张 PNG"，则把 `undo/redo/set_stroke_color` 从**首轮实现**移出，
仅保留 `ISketchDocument` 的 `snapshot/restore`（复现需要）。**接口不变**，实现分阶段交付。

---

## 8. 出图通路：`primitives/backend_hooks.h`（本次最关键的一个新接口头）

### 8.1 问题

- L3 的 `IPrimitiveRenderer` 必须**产生像素**（否则 R-SK-7/8 无法验收）；
- 真正下发 `vkCmd*` 的只有 `backend::submit_device_frame`（`src/backend/vulkan/vulkan_device_frame.h:67`）；
- 但 **G3 门控禁止 `src/primitives/**` 出现 `Vk*`**，也禁止 include backend 头
  （`vulkan_device_frame.h:19-21` 自己写了"L3 只通过不透明入口使用"）。

### 8.2 方案：不透明钩子（值类型 + 函数指针表）

```cpp
// include/magicblocks/primitives/backend_hooks.h —— **零 Vk***
namespace magicblocks::primitives::hooks {

// 一次"真提交"的结果统计（与 backend::DeviceFrameStats 字段一一对应，但不共享类型）
struct FrameStats {
  std::uint32_t draw_calls = 0;
  std::uint32_t instances = 0;
  std::uint32_t pass_count = 0;
  std::uint32_t pipeline_binds = 0;
};

// 已由 L3 录制完毕的命令列表 → 真正编码成 GPU 命令并等待完成。
// `command_list` 为 resources::ICommandList*（公共类型，不含 Vk）。
Status submit_recorded(device::IRenderDevice& device, resources::ICommandList& list,
                       FrameStats* out);

// 按目标形状派生管线（DepthState/RasterState/VertexLayout 全部是 L1 公共类型）。
// 返回的 IPipeline* 由调用方或缓存持有。
Result<resources::IPipeline*> create_shaped_pipeline(device::IRenderDevice& device,
                                                     const resources::PipelineDesc& desc,
                                                     const TargetShape& shape);
}
```

**实现侧（唯一 Vk 的位置）**：`src/backend/vulkan/vulkan_hooks.cpp` 里 `dynamic_cast` 到
`VulkanRenderDevice` / `VulkanCommandList` / `OffscreenTarget`，再调 `submit_device_frame`。
L3 侧只调这两个函数，**永远看不到 Vk 类型**。G3 门控与门控自测都会覆盖这条边界。

### 8.3 为什么不是"让 L4 直接调 backend"

| 方案 | 判定 |
|---|---|
| L4 `src/features/**` include backend 头 | ❌ 违 G3（且 features 不许写 GPU 命令，总 spec G3 原文） |
| 新增 L1 公共方法 `ICommandList::submit_immediately()` | ❌ 改 L1 公共接口 = 破坏 L1 的"恰好 11 个方法"不变量（`command.h:4-9`） |
| **不透明钩子头（选定）** | ✅ 零 Vk、零 L1 改动、L3 渲染器可单测 |

---

## 9. 命令流与确定性（R-SK-8）

### 9.1 五样状态进流（L3 spec §11.1 铁律的 F-SK 实例化）

| 状态 | 进流方式 |
|---|---|
| 相机 | `CameraSnapshot`（desc + pose），已有 |
| 材质 | `MaterialTable::snapshot()`，已有 |
| 场景 | L2 `CommandStream.scene`（`begin_frame(..., scene)`），已有 |
| **笔画事件** | L2 `EventRecord`（`record_event`），已有 |
| **F-SK 文档** | `SketchSnapshot`，**独立编码**（D-SK-4，L3 spec D-L3-5 口径） |

### 9.2 确定性要求（逐条）

1. **无时钟**：时间戳只从事件流取；`check_scene_no_clock.py` 的扫描范围扩到 `src/features/**`。
2. **无随机**：无 `rand`/`std::random`/时间种子。
3. **无哈希序遍历**：所有"按 id 遍历"的地方用有序容器或排序后遍历
   （`std::unordered_map` 只用于查找，不用于产生顺序）。
4. **浮点只在** `extract_outline` / `triangulate` 内部使用；**进命令流的**只有整数与
   `bit_cast` 后的浮点字节（材质哈希已按此实现，`material.h:61-62`）。
5. **脏驱动重算**：挤出只在笔画变化时重算，保证"同文档 → 同网格字节"。

### 9.3 三条校验（对齐总 spec §5.3）

| 校验 | F-SK 做法 | 通过标准 |
|---|---|---|
| 录制 vs 回放 | 同一 `CommandStream` → `replay()` | `stream_hash` 一致 |
| **回放出图** | 回放后 `submit_recorded` 到**新离屏目标** → PNG | 与录制帧 PNG **逐像素一致** |
| 复现 | 回放 `EventRecord` → 重跑 `SketchFeature` → 重录 | `stream_hash` 一致 + PNG 一致 |

> **注意**：L2 现在的 `replay()` 只做"流还原 + 哈希比对"（`recorder.h:19-22`），
> F-SK 需要额外做"回放后真的提交一次"这一跳。这**不改 L2 接口**，在 CLI 层组合即可（§12）。

---

## 10. 性能与内存（R-SK-11）

| 项 | 预算 | 手段 |
|---|---|---|
| 稳态帧堆分配 | 0 | 采样点数组按容量预分配；`reserve` 在 `Begin` 时一次完成 |
| 稳态帧 VMA alloc/free | **0**（M5 硬断言） | 顶点/索引/实例缓冲在**首次**挤出时创建，之后 `write()` 覆写（Host 可见桶，`IBuffer::write` 不触发分配） |
| 挤出重算 | 只在笔画变化帧 | 文档版本号 → `dirty` |
| drawcall | ≤ 3 | 单次 `draw_instanced` |
| 三角面 | 单笔 ≤ 4k，全画布 ≤ 200k（总 spec §8） | 轮廓简化阈值（折线 Douglas-Peucker，参数化） |

> **实例缓冲为什么必须是 Host 可见**：设备页（Vertex/Index/Uniform）`mapped()` 返回 nullptr
> （`include/magicblocks/resources/buffer.h:8-12`），实例数据每帧要重写，只能走
> `BufferUsage::Staging`（映射到 `MemoryKind::Staging` → `PageKind::HostVisible`，`handles.h:116-133`）。
> 这是**在 L1 现有能力内的唯一正解**，不改 L1。

---

## 11. 验收标准（可度量，逐条回溯需求）

| # | 验收项 | 判定 | 回溯 |
|---|---|---|---|
| A1 | CLI 无头出图 | `magicblocks-fsk-cli render --width 480 --height 360 --output sketch.png` 退出码 0，PNG 落盘且非空 | R-SK-7 || A2 | 结构断言（防假绿） | PNG 中心区域存在**非背景色**像素占比 ∈ [3%, 60%]；四角为背景色 | R-SK-1/6 |
| A3 | 相机双模态 | `--camera ortho` 与 `--camera perspective` 各出一张，且两张**逐像素不同** | R-SK-5 |
| A4 | 笔画增量 | 单测：`feed()` 一次 `Move` 后点数 +1，已有采样点字节不变 | R-SK-2 |
| A5 | 轮廓与孔 | 单测：一笔 → outer=1, holes=0；画一圈 → outer=1, holes=1 | R-SK-3 |
| A6 | 挤出水密 | 单测：生成的 `IMesh` 每条无向边出现恰好 2 次 | R-SK-4 |
| A7 | 命令三态 | `record` → `replay --verify` → `reproduce --verify` 全 0；三者 `hash=` 相同 | R-SK-8 |
| A8 | 回放逐像素一致 | 回放帧 PNG 与录制帧 PNG `count_differing_pixels == 0` | R-SK-8 |
| A9 | 撤销 | 单测：`undo_last()` 后 `doc_hash` 回到上一版；渲染后像素回退 | R-SK-9 |
| A10 | 改色不动几何 | 单测：`set_stroke_color` 后顶点/索引字节不变，只有实例颜色变 | R-SK-9 |
| A11 | 稳态零分配 | 单测：暖机 3 帧 + 差分 30 帧，`vma_alloc_count`/`vma_free_count` 增量 = 0，堆分配计数器 = 0 | R-SK-11 |
| A12 | 门控 | G1/G3/G4/G7/G8/scene_no_clock（扩到 features）全 rc=0；既有 193 条**零回归** | R-SK-10 |
| A13 | **双表示预留有效** | `region_op=Difference` 返回 `Unsupported`；`path_style=Region` 产出闭合且无笔宽 offset 的轮廓；`OutlineSet.source_style` 与输入一致 | **R-SK-12** |

---

## 12. CLI 设计（build-pipeline 硬约束：CLI + 离屏出图）

```
magicblocks-fsk-cli render
    --width W --height H                  # 离屏目标尺寸
    --strokes N                           # 演示事件序列的笔画数（确定性生成）
    --camera ortho|perspective            # R-SK-5
    --thickness F                         # 挤出厚度
    --frame-index N                       # 参与命令流与哈希
    --events FILE                         # 可选：从文件读事件序列（复现用）
    --record cap.bin                      # 可选：落盘命令流（二进制）
    --output out.png                      # 必填

magicblocks-fsk-cli replay --replay cap.bin [--verify] --output out.png
magicblocks-fsk-cli reproduce --replay cap.bin [--verify] --output out.png
magicblocks-fsk-cli dump --replay cap.bin [--json]
```

- **退出码**：`0` 成功 · `1` 运行失败 · `2` 参数错误 · `3` 需提权让 loader 看到 SwiftShader（沿用 L0 CLI 口径）。
- **未知参数 → 退出码 2**（不静默忽略，沿用 L2 CLI 的 CTest 约定）。
- **演示事件序列是纯函数**：由 `--strokes N` 与固定步长推导，**读时钟为 0**，
  跨进程/跨机器逐字节一致（这是 AG 金样可跨平台的前提）。
- 同时把 `magicblocks-l2-cli --scene sketch` 从"拒绝"改为"接入 F-SK"（总 spec §5.4 的 CLI 形态），
  **保留** `--scene demo` 的既有行为与 golden 不变。

---

## 13. 测试设计

### 13.1 单元测试（`tests/magicblocks_fsk_test.cpp`）

| 套件 | 用例 | 守的是 |
|---|---|---|
| Painter | `StrokeGeneratesPoints` · `MoveIsIncremental` · `CancelDropsStroke` · `MinSpacingFiltersDensePoints` · `ClampToCanvas` | R-SK-1/2 |
| Outline | `SingleStrokeOneLoop` · `ClosedLoopHasHole` · `MergeRemovesOverlap` · `DegenerateStrokeErrors` | R-SK-3 |
| Extrude | `WatertightEdges` · `WindingMatchesFrontFace` · `ThicknessAffectsBounds` · `HoleSideWallsFaceInward` | R-SK-4 |
| Document | `UndoRedoRoundTrip` · `ColorChangeKeepsGeometry` · `SnapshotHashStable` · `StrokeIdNeverReused` | R-SK-9 |
| Feature | `EventSequenceProducesOneStroke` · `PointerCancelLeavesNoStroke` · `SameEventsSameSnapshot` | R-SK-1/8 |
| Renderer | `OneFrameThreeDrawsAtMost` · `SteadyStateZeroVma` · `PipelineDerivedByTargetShape` | R-SK-11/G2 |
| Hooks | `SubmitRecordedProducesPixels` · `CreateShapedPipelineMatchesTarget` | §8 |

### 13.2 端到端（CTest）

| 用例 | 内容 |
|---|---|
| `magicblocks_fsk_render_ortho` | 无头出 PNG（A1/A2） |
| `magicblocks_fsk_render_perspective` | 无头出 PNG，与 ortho 不同（A3） |
| `magicblocks_fsk_record` | 出 PNG + 落盘命令流（FIXTURES_SETUP） |
| `magicblocks_fsk_replay_verify` | 哈希一致（A7） |
| `magicblocks_fsk_replay_png_match` | 回放帧与录制帧逐像素一致（A8） |
| `magicblocks_fsk_reproduce_verify` | 复现哈希一致（A7） |
| `magicblocks_fsk_bad_arg` | 未知参数 → 退出码 2（WILL_FAIL） |
| `magicblocks_fsk_golden` | 与 `tests/data/golden_fsk_ortho.png` 比对（容差 ≤2/通道）+ 结构断言 |

### 13.3 golden 规矩（沿用 ARCHITECTURE §6）

- 只在**渲染逻辑有意变更**后重生成；
- 生成后必须**独立验证结构断言**（非背景像素占比、四角为背景），防止"错基线自我固化"；
- 单一基线（Windows SwiftShader）；Linux lavapipe 的跨平台一致性属**待验证**项（如实标注，不宣称）。

---

## 14. 门控（新增/扩展）

| 门控 | 断言 | 状态 |
|---|---|---|
| G1 无几何着色器 | `shaders/**` 无 `geom` stage；`PipelineDesc.stages` 结构上不含 GS | 已有，新增 shader 需通过 |
| G3 分层纯度 | L2+ 无 `Vk*`；**backend 与 primitives 内无玩法名** | 已有，**扩展扫描目录加入 `src/features/**` 的上行禁区检查** |
| G4 无裸分配 | `backend/` 外无 `vkAllocateMemory` | 已有 |
| **G8（新增）`check_l4_no_engine_names`** | `include/magicblocks/{device,resources,scene,primitives}/**` 与 `src/{backend,scene,primitives}/**` 中**不出现** `sketch`/`Sketch`/`涂鸦`；反向：`src/features/sketch/**` 不得 include backend 头 | 新增（需同步 `check_deps_tests.py` 的 `--expect-files` 口径，计划阶段核实） |
| scene_no_clock | 时钟读取禁令扩到 `src/features/**` | 改 |
| 依赖回归 | 既有 193 条零回归 | 已有 |

---

## 15. 明确不做（本轮/首轮）

| 项 | 决定 |
|---|---|
| F-VX / F-BL / F-ST | 不做（"必须一项一项来"） |
| `IMeshFactory` 的 Cube/Cylinder/Sphere 等图元 | 不做（F-SK 用不到；F-BL 阶段补） |
| `IMeshLoader::Obj` | 不做 |
| `IWorld` ECS / `IGizmoSystem` / `IPicker` / `IOverlay2D` | 不做（F-SK 无需求） |
| 布尔运算的"求差/求交" | 不做（只用并集） |
| 纹理/材质 PBR | 不做（纯色 unlit 即可） |
| 阴影/动画/骨骼/多线程/多队列 | 不做（总 spec §10） |
| 回退路径（找不到驱动就跳过、三角化失败就跳过） | **不做**（不回退原则） |

---

## 16. 风险与对策

| 风险 | 影响 | 对策 |
|---|---|---|
| **L3 缺口比预期大**（painter/mesh/renderer/hooks 全要现写） | 高 | 计划拆成独立阶段，每阶段自带验收（plans §3） |
| 挤出通道（libtess2）在自交轮廓上失败 | 中 | Clipper2 **先**把轮廓规范化为"简单多边形（无自交）"再三角化 |
| 稳态零 VMA 与"每帧重写实例"冲突 | 中 | 实例缓冲走 Host 可见桶 + `write()`，不新建资源（§10） |
| 回放出图与录制帧不一致（浮点/序） | 高 | 全部状态进流 + 确定性规则（§9.2）+ A8 逐像素断言 |
| golden 跨平台不一致 | 中 | 单一基线 + 结构断言双保险；Linux 一致性列为**待验证**，不宣称 |
| 游戏词泄漏进 SDK | 中 | G8 门控 + 负向自测样本 |

---

## 17. 裁定记录

> 2026-11-05 用户答复："**使用你的判断，我看了一下没有问题。Q-SK-2，后续的笔画路径分为路径，
> 和路径包围的布尔区域，需要预留一下。**"
> → 即：Q-SK-2 **不采用二选一**，改用 R-SK-12 的**双表示**（§6.3.1）；
> 其余问题授权本设计自行裁定，下表为最终裁定。

| # | 问题 | **最终裁定** | 依据 |
|---|---|---|---|
| Q-SK-1 | R-SK-9（撤销/重做/改色）是否进首轮 | ✅ **进首轮**（保留阶段 5） | 成本低：笔画列表 + 一次重挤出 |
| **Q-SK-2** | 重叠笔画：`Merge` 还是 `PerStroke` | ✅ **改为双表示**：`PathStyle::Ribbon`（路径，首轮实现）+ `PathStyle::Region`（布尔区域，接口预留）。多笔轮廓默认 `merge=true` | **用户 2026-11-05 明确要求**；详见 §6.3.1 |
| Q-SK-3 | 画布相机是否允许交互（orbit/pan）进首轮 | ✅ **不进首轮**，只做 2 个预设姿态（ortho/perspective） | 控制事件/命令流复杂度；`ICamera::orbit/pan` 已在 L3 可直接调用 |
| Q-SK-4 | golden 基线是否纳入本轮验收 | ✅ **纳入** | 总 spec §11.3 要求 ≥1 张 PNG |
| Q-SK-5 | 是否允许新增 `primitives/backend_hooks.h` | ✅ **允许**（选项 A）。理由见下 | 不加则出不了像素 |

**Q-SK-5 的裁定理由**：选项 B（让 L4 直接调 backend）违总 spec G3 门控
（`src/features/**` 不许写 GPU 命令、不许 include backend 头，则 L4 无法出图，F-SK 直接不可达）；
选项 C（改 L1 公共接口）破坏 L1"`ICommandList` 恰好 11 个方法"的既有不变量。
A 是唯一同时满足 G3、不动 L1、可单测的方案；且 `vulkan_device_frame.h:76-81` 早已预留
"L3 通过 `primitives/backend_hooks.h` 的不透明入口取用"这一设计，属**既有计划的遗漏补齐**，不是新增通道。

---

## 18. 与总 spec 的对应

| 总 spec | F-SK 落地 |
|---|---|
| §4 L4 F-SK"`IFillPath` → `FromOutline` 挤出 → `ICamera` 正交/透视" | §6 全链路 |
| §2.1 依赖方向 / 零玩法分支 | §4 分层图 + G8 门控 |
| P6 `IFillPath` | `IPainter`（§6.3） |
| **P6+ 路径双表示（用户 2026-11-05 增补）** | **`PathStyle` / `RegionOp`（§6.3.1，R-SK-12）** |
| P2 `FromOutline` | `IMeshFactory::FromOutline`（§6.4） |
| P9 `ICamera` | 已落地，直接用（§6.3 D-SK-5） |
| §5 命令模式三态 + 序列化 + 确定性 | §9、A7/A8 |
| §5.4 离屏一等公民 + CLI | §12 |
| §6.2/6.3 句柄=偏移、命令流记偏移 | 实例缓冲/顶点缓冲均记 `ResourceId` |
| §11 验收门 3/4/5 | A1/A2/A7/A8/A12 |
| M5 稳态零 VMA | A11 |
