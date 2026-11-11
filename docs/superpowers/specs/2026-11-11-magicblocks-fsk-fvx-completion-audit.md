# MagicBlocks L4 F-SK / F-VX 完成度审计：需求 → 实现 → 验收缺口清单

- 日期：2026-11-11
- 审计人：survey（magicblocks-engine-features 团队，任务 t1）
- 性质：**只读调查 + 文档**。本轮不改引擎 / 测试 / CMake 任何一行。
- 口径：**每条结论必须带 `file:line` 或命令输出**；没有证据的一律写「未验证」，不臆测。

**输入文档**

| 类别 | 路径 |
|---|---|
| 需求原文 | `E:\workspace\ugeespace\wifi\screencast\qsw文档\魔法功能预览.md`（第 2 行 = 魔法涂鸦；第 10–16 行 = 魔法叠叠乐六条） |
| F-SK 设计 | `docs/superpowers/specs/2026-11-05-magicblocks-l4-fsk-sketch-design.md`（下称 **SK 设计**） |
| F-SK 计划 | `docs/superpowers/plans/2026-11-05-magicblocks-l4-fsk.md`（下称 **SK 计划**） |
| R-SK-12 正式要求 | `docs/superpowers/specs/2026-11-10-magicblocks-l3-outline-subdivision-request.md` |
| F-VX 设计 | `docs/superpowers/specs/2026-11-09-magicblocks-l4-fvx-stack-design.md`（下称 **VX 设计**） |
| F-VX 计划 | `docs/superpowers/plans/2026-11-09-magicblocks-l4-fvx.md`（下称 **VX 计划**） |
| 代码 | `MagicBlocks/include/magicblocks/features/**`、`MagicBlocks/src/features/**`、`MagicBlocks/src/l3_cli/main.cpp` |
| 测试 | `MagicBlocks/tests/magicblocks_l4_sketch_test.cpp`、`magicblocks_l4_stack_test.cpp`、`magicblocks_l4_stack_command_test.cpp` |

---

## 0. 结论速览（先看这一节）

总体判断：F-SK 与 F-VX 的「骨架」都在，大部分单测级验收项可达；
但两者都**不是按设计/计划交付的完整形态**：F-SK 缺「画布 + 输入链路」这一需求原文点名保留的环节，
F-VX 缺状态机头件，并把「网格指示」留在「有实现、无出图」状态；另有 2 个基础设施缺口
（稳态零 VMA 从未落地、golden 基线从未落地）同时影响两条玩法。

最终统计：**P0 阻断 5 项 / P1 应有 9 项 / P2 可选 7 项**（见 §7）。

| 缺口 | 级别 | 一句话 | 影响 |
|---|---|---|---|
| **GAP-01** F-SK 输入链路缺失 | **P0** | 需求原文第 2 行要求「**只做简易画布**与挤出面功能」；设计 §6.1 的全链是 `InputEvent → StrokeEvent → IPainter → OutlineSet → FromOutline`。实际代码里 **`InputEvent`/`StrokeEvent` → `IPainter` 这一跳根本不存在**：`feed(` 在 `src/features/` 下 0 命中，涂鸦轮廓改由 `mesh_factory::stroke_outline()` 直接从「世界坐标点列」生成（`src/features/sketch/sketch_pipeline.cpp:93,147`） | 需求里唯一没被裁掉的交互环节缺失；「画布」不存在（见 GAP-02） |
| **GAP-02** F-SK「画布」不存在 | **P0** | SK 设计 §5.2（`:272-279`）定义 `CanvasParams{width,height,extrude_thickness,clamp_to_canvas}`，D-SK-5（`:381-382`）把「画布平面 + `ICamera::screen_to_ray` 求交」定为唯一换算点。代码里**没有** `CanvasParams`，`screen_to_ray` 在 `src/features/` 零引用 | 笔画坐标直接写死在 demo 生成器里（`src/l3_cli/main.cpp:1731-1784`），屏幕坐标 ↔ 画布坐标这一层没有 |
| **GAP-03** F-SK R-SK-12 双表示「只在 L4 私建、L3 没有」 | **P1** | SK 设计 §6.3.1(c)（`:452-469`）明写 `PathStyle`/`RegionOp`/`OutlineOptions`/`OutlineSet.source_style` 属 **L3 primitives**；实际枚举定义在 `include/magicblocks/features/sketch/sketch_document.h:56-76`，而 L3 的 `primitives::OutlineSet`（`include/magicblocks/primitives/mesh_factory.h:59-63`）**只有** `outer/holes`，没有 `source_style`/`source_hash`/`bounds` | A13 第 3 条（`source_style` 与输入一致）无对应断言；双表示的「接口预留」是 L4 私有，别的玩法（F-ST 底板）拿不到 |
| **GAP-04** F-VX 缺状态机头件 `IStackSession` | **P0** | VX 设计 §3.1（`:186-199`）列出的文件含 `stack_session.h`（「命令 / 撤销 / 改色 / 层锁的状态机」）；`include/magicblocks/features/stack/` 实查只有 9 个头（`block_brush/block_set/color_mode/grid_overlay/shape_template/stack_command/stack_render/stack_types` + 无 session）。CLI 用过程式流程替代（`src/l3_cli/main.cpp:2165-2537`） | 「整笔撤销」只在数据层满足（`features/stack/block_set.h:50-53`）：CLI 不提供撤销，命令流撤销只覆盖数据层 —— **没有任何「撤销」在 F-VX 渲染路径上端到端走通过** |
| **GAP-05** F-VX 网格指示「写好了但没接进出图」 | **P0** | 需求第 11 行要求「立方体…**带网格指示**」。`GridOverlay` 已实现并有 8 条像素/几何用例（`tests/magicblocks_l4_stack_test.cpp:1357-1620`），但 CLI 的 `cmd_stack()` **一行都没用**它（`grep -n "grid_ov\|create_grid_overlay" src/l3_cli/main.cpp` → 仅 `:1527` 一处 gizmo 注释），`StackFrameDesc` 也没有 overlay 字段（`include/magicblocks/features/stack/stack_render.h:85-118`） | 像素级的「网格指示」在任何出图路径里都观察不到（典型「组件级真绿、集成级假绿」，见 §4-F11） |
| **GAP-06** 稳态零 VMA（R-SK-11 / A11 / M5）从未落地 | **P0** | SK 计划阶段 4.4-4.7（`:142-147`）要求 `IPrimitiveRenderer::begin_frame/draw_mesh_instanced/end_frame` + `Renderer.SteadyStateZeroVma`。实际 `include/magicblocks/primitives/primitive_renderer.h:72-96` 是 `register_mesh/register_pipeline/set_instance_buffer/record_frame(cmd, IBatchBuilder)`，**没有** `draw_mesh_instanced`；全仓用例名 grep `SteadyState`/`ZeroVma` → 0 命中 | A11 不可验收；F-SK 的 drawcall 预算（≤3）也无断言 —— 实际「每笔一次 indexed draw」（`src/l3_cli/main.cpp:2066-2083`） |
| **GAP-07** golden 基线从未落地 | **P0** | SK 计划 7.1（`:185`）要求 `tests/data/golden_fsk_{ortho,persp}.png`；VX 计划阶段 6（`:364-371`）要求 golden。`MagicBlocks/tests/data/` 实查只有 `golden_l0.png`/`golden_l2.png`/`golden_ui.png` + 11 个 `_out_l4_*.png`（后者是**用例自己的输出产物**，不是基线）；`CMakeLists.txt` grep `golden_fsk`/`golden_l4` → 0 命中；`tests/data/README.md` 只登记前三个 | A2/A8 的「防假绿」结构断言没有基线可防回归；用例产物写进源码目录（`CMakeLists.txt:466`） |
| **GAP-08** F-SK 的 CLI 是「另一个 CLI」 | **P1** | SK 计划 §6（`:235-255`）的全部命令是 `magicblocks-fsk-cli.exe render/replay/reproduce/dump`；实际**没有**这个可执行文件（`build/*.exe` 只有 cube-demo / l0-cli / l2-cli / l3-cli / sink-verify / ui-cli / test），F-SK 落在 `magicblocks-l3-cli.exe sketch`（`src/l3_cli/main.cpp:2556`），且**没有** `--camera ortho\|perspective`（相机写死透视，`src/l3_cli/main.cpp:1876`）、没有 `replay/reproduce/dump`（分派表 `:2553-2561` 里没有） | A3（相机双模态）、A7/A8（F-SK 三态）不可验收 |
| **GAP-09** 锁层未在真实相机路径验证 | **P1** | `StackLayerLock.LockedLayerIgnoresCameraPitch`（`tests/magicblocks_l4_stack_test.cpp:761`）在笔刷层用「换射线方向」模拟俯仰；CLI 的 `cmd_stack()` 射线恒为垂直向下（`src/l3_cli/main.cpp:2298`），**从不**调用 `ICamera::screen_to_ray` | 需求第 15 行「可以指定层数操作，不跨过层」在真实相机路径上未验证 |
| **GAP-10** `--dry-run` 与全跑 `doc_hash` 不一致 | **P1** | 实测：`--strokes 3 --dry-run` → `doc_hash=18afbcae00c67113`；同参数全跑 → `doc_hash=5237df81ff5510a3`（§5 命令 3 与命令 4）。VX 设计 §13.3（`:671`）与 A9（`:734`）要求两者一致。两处都读 `set->hash()`（`src/l3_cli/main.cpp:2392` 与 `:2398`），**根因本文未定位**，只陈述实测事实（见 §6 局限 3） | A9 判据实际不成立；现有用例 `CliDryRunMatchesFullRun`（`tests/magicblocks_l4_stack_test.cpp:2138-2160`）只比 15 位十六进制，恰好避开差异 —— 见 §4-F1 |

---

## 1. F-SK：需求 → 实现 → 验收逐条比对

### 1.1 需求原文（`魔法功能预览.md:2`）

> 1. 魔法涂鸦：2维图形（透明）转3D 挤出面。**只做简易画布与挤出面功能。**

SK 设计把它拆成 R-SK-1…R-SK-12（`:133-146`），验收拆成 A1…A13（`:682-695`）。

### 1.2 R-SK-1 … R-SK-12

| 需求 | 预期实现位置（设计/计划） | 实际实现位置（file:line） | 有测试？ | 实测证据 | 结论 / 缺口 |
|---|---|---|---|---|---|
| **R-SK-1** 透明 2D 画布：指针/触摸事件驱动画笔 | `IPainter::feed(StrokeEvent)`（SK 设计 `:340-353`）；`SketchFeature::on_input(InputEvent)`（SK 计划 5.2 `:162`） | **事件→笔画：不存在。** `IPainter` 只吃屏幕像素 `StrokeEvent`（`include/magicblocks/primitives/painter.h:40-46,98-142`），而 `feed(` 在 `src/features/` 下 **0 命中**；`device::InputEvent`（`include/magicblocks/device/events.h:35-53`）从未被喂进任何代码。F-SK 笔画由 `make_sketch_strokes()` 直接造点（`src/l3_cli/main.cpp:1731-1784`）。另：`IPainter::extract_outline()` 返回**中心线点列**（`src/primitives/painter.cpp:103-116`），F-SK 没调它 | 仅 `SketchEndToEnd.*` 出图；**无**「事件→笔画」用例 | 见左两列 | **P0（GAP-01）** |
| **R-SK-2** 笔画增量：Move 只追加、不重建；Begin/Cancel 语义 | `IPainter`（SK 计划 3.3 `:127`） | `Painter::feed / cancel_current`（`src/primitives/painter.cpp:49-101`） | `Painter` 套件 13 条（`magicblocks_test.exe --gtest_list_tests`） | L3 侧有；F-SK 侧不使用 | **P1**：能力存在但没接进玩法；`l4_sketch_test` 无增量断言 |
| **R-SK-3** 轮廓提取：单笔→1 条外轮廓；画圈→带孔 | `Outline.SingleStrokeOneLoop` / `ClosedLoopHasHole`（SK 计划 3.6 `:130`） | 单笔 → `stroke_outline()`（`include/magicblocks/primitives/mesh_factory.h:79-80`，实现 `src/primitives/mesh_factory.cpp:1031`）；带孔能力在 `from_outline`（`mesh_factory.cpp:875`），但 F-SK 的 `build_meshes` **只填 `outer`**，`holes` 永远为空（`src/features/sketch/sketch_pipeline.cpp:171,176`） | `SketchDualRepresentation.RegionStrokeCoversItsInterior`（`l4_sketch_test.cpp:194`）只验 bbox | Region 2 笔 → 顶点 48 / 索引 72（§5 命令 2），是两个纯方盘、无孔 | **P1**：F-SK 无「画圈得孔」用例，A5 的 `outer=1,holes=1` 不存在 |
| **R-SK-4** 挤出成 3D：三角化（含孔）→ 上下盖 + 侧壁；水密 | `from_outline` + `Extrude.WatertightEdges`（SK 计划 2.3/2.4 `:103-104`） | `from_outline()`（`src/primitives/mesh_factory.cpp:875`，`tessTesselate` `:905`）；F-SK 调用点 `src/features/sketch/sketch_pipeline.cpp:189` | `FromOutline` 12 条（SimpleSquare/SquareWithHole/Overlapping/ZeroThickness/CapFlags/Empty/Degenerate/Duplicate/Normals/ZBase/Deterministic/IndicesInRange — `tests/magicblocks_l3_outline_test.cpp`），**无** `WatertightEdges` | 全仓 grep `Watertight` → 0 命中（§5 命令 7） | **P1**：**A6 判据（每条无向边恰好 2 个三角形）不存在**，而 SK 设计 `:518-519` 称其为「关键验收」 |
| **R-SK-5** 相机：正交与透视都能看挤出体 | `--camera ortho\|perspective`（SK 设计 `:685`、SK 计划 `:243-248`） | 相机写死 `ProjectionKind::Perspective`（`src/l3_cli/main.cpp:1876`）；`parse_args`（`:134-219`）无 `--camera` 分支 | 无 | `print_usage`（`:110-111`）里没有 `--camera` | **P1（GAP-08）**：A3 不可验收 |
| **R-SK-6** 3D 显示：不透明实体、随视角变化、深度正确 | 深度目标 + 光照 | `td.with_depth = true`（`:1858`）；`scene_pipeline_desc()` 深度测试开（`:389-390`） | 无旋转/遮挡断言 | 斜俯视出图非空（§5 命令 1） | **P2**：只有「非空」 |
| **R-SK-7** 无头离屏出 PNG | `magicblocks-fsk-cli render`（SK 设计 `:702-715`） | `magicblocks-l3-cli sketch`（`:1786-2116`，`write_png_rgba8` `:2108`） | `SketchEndToEnd.CliProducesRealPng/RegionPatternAlsoRenders/MergePatternRendersSingleSolid/CliRejectsZeroStrokes/CliRequiresOutput`（`l4_sketch_test.cpp:471-566`） | `[sketch] 已写出 .\tmp\audit_sk_ribbon.png (320x200)`，EXIT=0；`--strokes 0` → EXIT=1；缺 `--output` → EXIT=2（§5 命令 1/2） | **通过** |
| **R-SK-8** 命令模式：录制 → 回放 → 复现 | `record/replay/reproduce --verify` + `replay_png_match`（SK 设计 `:689-690`） | **不存在**（`main()` 分派 `:2553-2561` 只有 paint/blocks/gizmo/sketch/stack；`--record` 不在 `parse_args` 里 → 未知参数 exit 2） | 无 | — | **P1（GAP-08）**：A7/A8 不可验收；只能验「同参数两次运行逐字节相同」（`SketchEndToEnd.CliIsByteIdenticalAcrossRuns`，`l4_sketch_test.cpp:494`；我方复核 SHA1 `29EA6A61…`） |
| **R-SK-9** 撤销/重做一笔；改色不重建几何 | `Document.UndoRedoRoundTrip` / `ColorChangeKeepsGeometry`（SK 计划 5.5 `:165`） | `SketchDocument::undo/redo/hash`（`src/features/sketch/sketch_document.cpp:85-174`）；**`set_stroke_color` 不存在**（`sketch_document.h:125-175`，SK 设计 `:312` 的接口未落地） | `SketchDocument.UndoRedoRestoresExactly`（`l4_sketch_test.cpp:69`）、`RemoveStrokeUndoInsertsBackAtOriginalIndex`（`:84`）、`NewEditInvalidatesRedoStack`（`:103`）、`IdsAreNotReusedAfterUndoOrClear`（`:113`） | 文档 hash 往返一致（用例通过） | **P1**：A9 的「**渲染后像素回退**」缺失；A10「改色不动几何」**整条缺失**（无改色 API） |
| **R-SK-10** 零玩法分支 | G8 门控（SK 设计 `:768`） | 无 `tools/check_l4_no_engine_names.py`（`tools/` 实查 18 个脚本，无此名）；由 G7 `tools/check_l3_no_gameplay_names.py` + G3 `tools/check_layer_purity.py` 覆盖 | `StackGates.G7GateExitZeroAndScansNTimes` / `G3LayerPurityExitZero`（`tests/magicblocks_l4_stack_command_test.cpp:300-327`） | G7 exit 0（扫描 32 文件）、G3 exit 0（`backend 41 个文件`）（§5 命令 6） | **通过**（G8 专名门控未建，语义等价） |
| **R-SK-11** 稳态帧零分配、零 VMA | `IPrimitiveRenderer` 最小版 + `Renderer.SteadyStateZeroVma`（SK 计划 4.4-4.7 `:142-147`） | **不存在**（`primitive_renderer.h:72-96` 无 `draw_mesh_instanced`）；F-SK 自己 `create_buffer` 三次（`:1916,1930,1976`）、每笔一次 draw（`:2066-2083`） | 0 | 474 条用例全表无 `SteadyState*`（§5 命令 5） | **P0（GAP-06）**：A11 不可验收 |
| **R-SK-12** 笔画双表示（路径 / 路径包围的布尔区域） | SK 设计 §6.3.1（`:404-495`）；另见 `2026-11-10-magicblocks-l3-outline-subdivision-request.md` | `PathStyle/RegionOp`（`features/sketch/sketch_document.h:56-76`）、`Stroke.style/closed`（`:92-104`）、`check_style`（`sketch_document.cpp:178-207`）、Ribbon 走 `stroke_outline`（`sketch_pipeline.cpp:147`）、Region 走 `region_ring`（`:47-59`）、`RegionOp≠Union` → `UnsupportedRegionOp`（`:137-142`）、`supports_region_op`（`sketch_document.cpp:176`） | `SketchCapability.OnlyUnionIsSupportedForRegion`（`:148`）、`RegionRequiresClosingAndDoesNotAutoClose`（`:166`）、`SketchDualRepresentation.RibbonAndRegionOnSamePointsAreDifferentSolids`（`:215`）、`SketchFailures.UnsupportedRegionOpIsReportedNotSilentlyDowngraded`（`:259`）、`OpenRegionStrokeIsReported`（`:274`）、`RegionPatternAlsoRenders`（`:513`） | 两种表示产出不同实体：Ribbon 3 笔 → 顶点 1332/索引 2628；Region 2 笔 → 顶点 48/索引 72（§5 命令 1/2）；`--sketch-pattern 1` EXIT=0 | **部分通过**。缺口（**P1，GAP-03**）：① `OutlineOptions`/`OutlineSet{source_style,source_hash,bounds}` 没进 L3；② A13 第 3 条断言缺失；③ 双表示无命令流承载（无命令流，见 GAP-08）。④ 注：`2026-11-10` 那份文档是 **F-ST 前置**（`from_outline_subdivided`，`mesh_factory.h` 无声明、grep 0 命中），与 F-SK 的 R-SK-12 是不同要求，不影响 F-SK |

### 1.3 A1 … A13（SK 设计 `:682-695`）

| # | 判据 | 现状 | 证据 | 级别 |
|---|---|---|---|---|
| A1 | CLI 无头出图 exit 0 + PNG 非空 | ✅ | §5 命令 1 | — |
| A2 | 结构断言：中心非背景 ∈[3%,60%]、四角为背景 | ⚠️ 用例里没有；我方独立复算 `fg_ratio=7.20%`、四角均为 `(15,18,26,255)` | §5 命令 9 | **P1**（含 GAP-07） |
| A3 | ortho / perspective 两张逐像素不同 | ❌ 不可执行（无 `--camera`） | GAP-08 | **P1** |
| A4 | `feed()` 一次 Move 后点数 +1 | ⚠️ 能力在 L3（`painter.cpp:49`），F-SK 不用 | GAP-01 | **P1** |
| A5 | 一笔 outer=1 holes=0；画一圈 outer=1 holes=1 | ❌ 无用例；F-SK 永不生成 `holes` | `sketch_pipeline.cpp:171,176` | **P1** |
| A6 | 水密（每条无向边恰好 2 次） | ❌ | grep `Watertight` = 0 | **P1** |
| A7 | 三态哈希相同 | ❌ 无命令流 | GAP-08 | **P1** |
| A8 | 回放帧与录制帧逐像素一致 | ❌ | GAP-08 | **P1** |
| A9 | undo 后 doc_hash 回退 + **像素回退** | ⚠️ 只有 hash（`l4_sketch_test.cpp:69-101`） | — | **P1** |
| A10 | 改色后顶点/索引不变，仅实例色变 | ❌ 无 `set_stroke_color` | `sketch_document.h:125-175` | **P1** |
| A11 | 稳态零 VMA / 零堆分配 | ❌ | GAP-06 | **P0** |
| A12 | 门控 + 既有用例零回归 | ⚠️ G7/G3 exit 0；用例总数已到 474（设计写的 193 是旧基线） | §5 命令 5/6 | — |
| A13 | 双表示预留有效（Unsupported + Region 闭合 + `source_style` 一致） | ⚠️ 前两条有（`l4_sketch_test.cpp:148-175,259-283`），第三条无 | GAP-03 | **P1** |

### 1.4 F-SK 与需求原文的直接对账

| 原文要素 | 实现 | 级别 |
|---|---|---|
| 「2维图形」 | `Stroke{points(Vec2), width, style}`（`sketch_document.h:92-104`） | ✅ |
| 「（透明）」 | 无任何透明/alpha 相关的 F-SK 代码；笔画 alpha 恒 1.0（`src/l3_cli/main.cpp:1746`），管线预乘混合（`:383-387`） | **P2**（语义未定义，未验证） |
| 「转3D 挤出面」 | `from_outline`（`mesh_factory.cpp:875`）+ `build_meshes`（`sketch_pipeline.cpp:108`） | ✅ |
| 「简易画布」 | **不存在**（GAP-02） | **P0** |
| 「挤出面功能」 | 单笔/合并两种挤出（`sketch_pipeline.cpp:159-205`） | ✅ |

---

## 2. F-VX：需求 → 实现 → 验收逐条比对

### 2.1 需求原文（`魔法功能预览.md:10-16`）

| 原文 | 设计编号（VX 设计 `:18-25`） |
|---|---|
| 场景默认是一个立方体，但是关闭深度测试，永远显示内部面，3个面，带网格指示 | F-VX-3 |
| 画笔路径最小生成单位就是一个小网格 | F-VX-1 |
| 画笔按下的那一刻决定层数…不会自动往上覆盖新增一层 | F-VX-6 |
| 模型要支持改颜色模式，路径划过去不是建小方块了，而是改颜色 | F-VX-4 |
| 可以指定层数操作，不跨过层 | F-VX-6 |
| 扩展，体素可以改成任意形状 | F-VX-5 |

> ⚠️ **口径提示**：VX 设计 `:0.1` 改写了自己的六条，其中「跨笔独立 `stroke_id`、整笔撤销」（F-VX-2）**在用户原文里没有**；反过来原文的「**3 个面**」「**关闭深度测试**」在 F-VX-3 里被吸收成「内面始终可见」。本审计两边都查。

### 2.2 六条可验收行为（VX 设计 `:149-156`）与 A1…A14（`:720-739`）

| 行为 / 验收 | 实际实现（file:line） | 有测试 | 实测证据 | 结论 / 缺口 |
|---|---|---|---|---|
| **行为1** 画一笔 → 沿笔迹出现列 | 吸附 `src/features/stack/block_brush.cpp:106-113`（量化 `:22`）；列生成 `src/l3_cli/main.cpp:2336-2344` | `StackSweep.DiagonalHitsEveryCell`(548)、`ColumnHeightHonored`(699) | `--strokes 3` → 92 方块、drawcall 2（§5 命令 3） | ✅ |
| **行为2** 位置必然在整数格 | `snap()` 整数化 `block_brush.cpp:106` | `StackSnap.*` 6 条（`l4_stack_test.cpp:420-512`） | 115 条相关用例全 PASSED（§5 命令 5） | ✅ |
| **行为3** 撤销一笔 → 方块同时消失 | 数据层 `block_set.h:50-53`；CLI **无撤销**；渲染层要压紧实例（VX 设计 §9 `:445-448`），`IStackSession` 缺失 | `BlockSet.RemoveStrokeDropsExactlyThatStroke`(125)、`UndoStrokeReturnsBlocksInSourceOrder`(346)、`DerivedIndexIsRebuiltAfterUndo`(371)、`StackCommandMode.UndoAndColorReplayIdentically`(191) | 全绿（数据层） | ⚠️ **P1**：数据层 ✅，渲染层「实例压紧 + 像素回退」无断言 |
| **行为4** 改色只改 `base_color` | `src/features/stack/color_mode.cpp:48-90`（`pick_color` `:18-32`） | `StackColor.*` 8 条；`ColorChangeTouchesOnlyColorBytesInInstanceBuffer`(1751)；`CliColorModeChangesPixelsWithoutChangingShape`(2162) | flat vs by-layer：非背景像素数**完全相同**（35713/35713），颜色种数 6 vs 16（`_out_l4_stack_cm_flat/layer.png`，§5 命令 9） | ✅（但 A6 用例只断言 `offsetof==64`，见 §4-F2） |
| **行为5** 内面 pass 打开时被遮内面仍可见 | `stack_render.h:78-79`；`src/features/stack/stack_render.cpp:550-567`（`depthTest=false` `:561`、`cull=None` `:562`、`depthWrite=true` `:567`） | `StackRender.PassPairIsDistinct`(1018)、`BothPassesRenderOnDepthTarget`(1060)、`TwoDrawCallsOneSubmit`(1118)、`InnerPassChangesPixels`(1171)、`InnerPassIsAfterSurfacePass`(1221)、`SingleInstanceColumnRenders`(1265) | `--inner-pass on` → `合批 2 (pass 2) drawcall 2`；`off` → `合批 1 (pass 1) drawcall 1`；两图不同（distinct 16 vs 10，同 fg_ratio 7.84%）（§5 命令 3/9） | ✅（「**3 个面**」无任何对应用例） |
| **行为6** 锁层后相机俯仰变化仍在同一层 | `block_brush.cpp`（`lock` 越界 `:42`）；CLI 每笔 `lock()` `src/l3_cli/main.cpp:2288-2292`，射线恒垂直 `:2298` | `StackLayerLock.*` 4 条（`l4_stack_test.cpp:761-838`） | 用例全绿；`--lock-layer 9` → `[code=4] lock(9) 越界：合法层号 [0, 8)` exit 1（§5 命令 4） | ⚠️ **P1（GAP-09）** |
| **A1** 吸附与手算一致 | — | `StackSnap.RayToCellExact/ExactGridLinePoint/NegativeCoordinates/NonUnitCellSize` | 全绿 | ✅ |
| **A2** supercover 不漏格 | — | `StackSweep.DiagonalHitsEveryCell/NoCornerLeak/SinglePointEmitsOnce` | 全绿 | ✅ |
| **A3** 去重 + `skipped_occupied` | — | `StackSweep.DuplicatePassDoesNotReEmit`(578)、`SelfOverlappingStrokeDedupesWithinOneStroke`(607)、`OccupiedCellIsRejectedAtomically`(629) | 全绿 | ✅ |
| **A4** 整笔撤销 | — | `BlockSet.RemoveStroke*`、`UndoStroke*` | 全绿 | ✅（数据层） |
| **A5** 错误码全集 | `StackError::Code`（VX 设计 `:399-414`）；名字在 `src/features/stack/block_set.cpp:263-269` | 散落：`BlockSet.InvalidGridIsRejectedNotClamped`(183)、`FlagsAndLayerDomainAreEnforced`(224)、`EmptyStrokeIsExplicitError`(279)、`ShapeTemplate.UnknownTemplateIsExplicitError`(1851) | CLI 5 种错误退出码符合 §13.4（§5 命令 4） | ⚠️ **P2**：无统一 `StackFailures.*` 套件 |
| **A6** 改色只动 base_color 字节 | — | `ColorChangeTouchesOnlyColorBytesInInstanceBuffer`(1751) | 见 §4-F2 | ⚠️ 弱断言 |
| **A7** 双 pass hash 不同 + 带深度目标都出像素 | — | `PassPairIsDistinct` + `BothPassesRenderOnDepthTarget` | 全绿 | ✅ |
| **A8** `--inner-pass on/off` 两图不同 | CLI `src/l3_cli/main.cpp:2478` | `StackRender.InnerPassChangesPixels`(1171) | ✅ 实测差异（§5 命令 3/9） | ✅ |
| **A9** `--dry-run` 统计与全跑一致 | `src/l3_cli/main.cpp:2392,2395-2400` | `CliDryRunMatchesFullRun`(2138) | ❌ 两个 `doc_hash` 不同：`18afbcae…` vs `5237df81…`（§5 命令 3/4） | **P1（GAP-10）** |
| **A10** CLI 出真 PNG + 两次逐字节相同 | `src/l3_cli/main.cpp:2512-2527` | `CliProducesRealPng`(2048)、`CliIsByteIdenticalAcrossRuns`(2070) | ✅ 两次运行 SHA256 相同（§5 命令 4） | ✅（但无「前景占比 > 0」断言，见 §4-F4） |
| **A11** 缺 `--output` exit 2；`--strokes 0` exit 1 且不写文件 | `src/l3_cli/main.cpp:2167-2188` | `CliRequiresOutput`(2104)、`CliRejectsZeroStrokes`(2091) | 实测 exit 2 / exit **2**（`--strokes 0` 走参数分支 `:2183-2188`；设计 A11 写的是 exit 1） | ⚠️ **P2**（退出码口径不一致） |
| **A12** 层锁（相机俯仰 3 次） | 同行为 6 | `StackLayerLock.LockedLayerIgnoresCameraPitch`(761) | 全绿 | ⚠️ P1（GAP-09） |
| **A13** 门禁 + `src/features/stack/` 零 vkCmd | G3 + G7 | `StackGates.*` 3 条（`magicblocks_l4_stack_command_test.cpp:300-381`） | G3/G7 exit 0（§5 命令 6） | ✅（UIG1/UIG2 是 CTest 条目 `CMakeLists.txt:498-500`，未纳入 `StackGates`） |
| **A14** 回归全绿、只增不减 | — | — | `build/Testing/Temporary/LastTest.log`：`497/497` 条目均已执行（2026-11-10 15:07）；gtest 总数 474 | ⚠️ 未见汇总行，本次未复跑全量（见 §6 局限 1） |

### 2.3 六条需求（VX 设计 §0.1）落地度

| 需求 | 落地 | 级别 |
|---|---|---|
| **F-VX-1** 扫掠吸附堆叠 | ✅ `block_brush` + CLI 列生成 | ✅ |
| **F-VX-2** 跨笔 `stroke_id` + 整笔撤销 | ⚠️ 数据层 ✅（`block_set.h:50`、`BlockSet.StrokeIdIsMonotonicAndNeverReused`(199)），上端撤销缺失（GAP-04） | **P1** |
| **F-VX-3** 内面始终可见 | ✅ 双 pass（`stack_render.cpp:550-567`） | ✅ |
| **F-VX-4** 颜色模式 | ✅ `color_mode.cpp` + CLI 三模式 | ✅ |
| **F-VX-5** `IShapeTemplate` | ✅ `src/features/stack/shape_template.cpp` + 6 条用例；CLI `--template` | ✅ 实测 `circle` 222 块；`sphere` 在 `--lock-layer 4` 下 332 块（§5 命令 4）。注：`sphere` 在默认层 0 会 `[code=8] 模板第 0 个偏移 (0,-2,0) 展开后 y=-2 越出 [0, 8)` exit 1（符合设计 §12.2，但 CLI 帮助未提示需配 `--lock-layer`） |
| **F-VX-6** 笔刷层锁 | ✅ `block_brush` 层锁 | ⚠️ GAP-09 |

### 2.4 用户原文里 VX 设计未覆盖的点

| 原文 | 现状 | 级别 |
|---|---|---|
| 「场景默认是一个**立方体**」 | 无「默认立方体」概念；CLI 要求 `--strokes >= 1`（`src/l3_cli/main.cpp:2183`） | **P2** |
| 「**关闭深度测试**，永远显示内部面」 | 只对内面 pass 成立（`stack_render.cpp:561`），表面 pass 仍开深度（`:550`）——与设计 §10.1 一致、与原文措辞不同 | **P2**（需用户确认口径） |
| 「**3 个面**」 | 无任何实现或断言 | **P1** |
| 「**带网格指示**」 | 组件有、出图没接（GAP-05） | **P0** |
| 「体素可改成任意形状（扩展）」 | `IShapeTemplate` 4 内置 + 可注册（VX 设计 §12.3 `:620-624`） | ✅ |

---

## 3. 结构性缺口（跨玩法）

| # | 缺口 | 证据 | 级别 |
|---|---|---|---|
| S-1 | **F-SK 没有「事件 → 笔画 → 轮廓」链路** | `src/features/sketch/` 只有 `sketch_document.cpp` / `sketch_pipeline.cpp`；`painter.h:110` 的 `feed` 在 features 里 0 调用 | **P0** |
| S-2 | **`IPainter::extract_outline()` 与设计语义不符**：设计要轮廓，实现返回中心线点列（`src/primitives/painter.cpp:103-116`），F-SK 因此绕开它用 `stroke_outline` | 同左 | **P1** |
| S-3 | **`IPrimitiveRenderer` 与设计差一代**：设计要 `begin_frame/draw_mesh_instanced/end_frame`（SK 计划 4.4 `:143`），实现是 `record_frame(cmd, IBatchBuilder)`（`primitive_renderer.h:72-96`） | 同左 | **P0** |
| S-4 | **`IStackSession` 不存在** | `include/magicblocks/features/stack/` 9 个头无此文件（VX 设计 §3.1 `:191`） | **P0** |
| S-5 | **`scene_no_clock` 没扩到 `src/features/**`**（SK 设计 `:769`、SK 计划 7.3 `:187`） | `tools/check_scene_no_clock.py:56 SCAN_DIRS = ("include/magicblocks/scene", "src/scene")`；实测 `14 个文件 / 3697 行代码`；`CMakeLists.txt` 里 `check_scene_no_clock` 0 命中 | **P1** |
| S-6 | **golden 基线不存在** | `tests/data/` 只有 `golden_l0/l2/ui.png` + 11 个 `_out_l4_*.png`；`CMakeLists.txt` grep `golden_fsk`/`golden_l4` = 0 | **P1** |
| S-7 | **测试产物写进源码目录** | `CMakeLists.txt:466 MB_TEST_DATA_DIR=.../tests/data`；输出写到 `tests/data/_out_l4_*.png`（`l4_sketch_test.cpp:465-467`、`l4_stack_test.cpp:2042-2044`） | **P2** |
| S-8 | **CLI 参数不按子命令隔离** | 实测 `sketch … --template circle` → exit 0（参数被忽略）；`stack … --sketch-pattern 1` → exit 0（§5 命令 8）。SK 设计 §12 要求未知参数 exit 2 | **P2** |
| S-9 | **F-VX 的 MSAA 与像素用例口径不一**：CLI 用 `--msaa`（`src/l3_cli/main.cpp:2427`），`StackRender.*` 像素用例恒 `samples=1`（`l4_stack_test.cpp:997`） | 同左 | **P2** |

---

## 4. 「有测试但没验到像素/几何」的假绿清单

| # | 位置 | 声称验的事 | 实际断言 | 判定 | 级别 |
|---|---|---|---|---|---|
| **F1** | `tests/magicblocks_l4_stack_test.cpp:2138-2160` `StackEndToEnd.CliDryRunMatchesFullRun` | A9：dry-run 与全跑的 `doc_hash` 完全一致 | `EXPECT_EQ(dry_out.substr(dp, 25), full_out.substr(fp, 25))`（`:2155`），`dp`/`fp` 指向 `"doc_hash="` 的 `d`（`:2150`）→ 实际只比 `"doc_hash=" + 15 位十六进制`。实测两值 `18afbcae00c67113` vs `5237df81ff5510a3`，第一位就不同（§5 命令 3/4） | ⚠️ 断言与设计意图不符：真正的 16 位 hash 从未被完整比较，用例 PASS 推不出 A9 成立 | **P1** |
| **F2** | `tests/magicblocks_l4_stack_test.cpp:1811-1814` `StackColor.ColorChangeTouchesOnlyColorBytesInInstanceBuffer` | A6：「改色前后实例缓冲除 64-79 外**逐字节相同**」 | 只断言 `offsetof(InstanceAttribs, base_color)==64`、`sizeof==112`；改色后**没有**再 `build_instances` 做字节比较 | ❌ 名字承诺 > 实际断言 | **P1** |
| **F3** | `tests/magicblocks_l4_stack_test.cpp:1748` `StackColor.ByStrokeUsesStrokeId` 尾行 | 同笔内所有块同色 | `EXPECT_EQ(set->at(0)->color, set->at(0)->color)` —— 自比，恒真 | ❌ 空断言 | **P2** |
| **F4** | `tests/magicblocks_l4_stack_test.cpp:2048-2068` `StackEndToEnd.CliProducesRealPng` | A10：「前景占比 > 0、非纯背景」 | 只查 PNG 签名 + `bytes.size() > 2000` + stdout 含 `doc_hash=`/`drawcall 2`；**无像素统计**。用例自己的注释（`:2224-2226`）也承认「那三张 320x200 图字节数完全相同却是错图」 | ⚠️ 弱（我方复算当前图案正确：fg 6.5%、bbox 102×58，§5 命令 9；但用例守不住） | **P1** |
| **F5** | `tests/magicblocks_l4_sketch_test.cpp:471-492` `SketchEndToEnd.CliProducesRealPng` | A1/A2：PNG 非空 + 结构断言 | 只查 PNG 签名 + `size>2000` + stdout 含 `mesh_hash=`；**没有**四角/占比断言。实测 320×200 的 9 个 `_out_l4_*.png` **全部 256283 字节** —— 字节数完全不区分内容 | ⚠️ 弱 | **P1** |
| **F6** | `tests/magicblocks_l4_sketch_test.cpp:513-529` `RegionPatternAlsoRenders`、`:531-547` `MergePatternRendersSingleSolid` | Region / 合并能出图 | `is_png` + `size>2000`（后者另查 stdout 含 `实体 1 `） | ⚠️ 弱：实测 distinct 颜色 4（region）/ 12（merge）/ 39（wave），但用例不验 | **P2** |
| **F7** | `tests/magicblocks_l4_stack_test.cpp:2104-2108` `CliRequiresOutput` | A11：缺 `--output` → exit 2 | 只查 `rc==2` | ✅（但 `--strokes 0` 口径不一致，见 A11） | **P2** |
| **F8** | `tests/magicblocks_l4_stack_test.cpp:2091-2102` `CliRejectsZeroStrokes` | A11：`--strokes 0` → exit **1** | `EXPECT_NE(rc, 0)`；实测 rc==**2**（`src/l3_cli/main.cpp:2183-2188`） | ⚠️ 与设计偏离 | **P2** |
| **F9** | 整个 `Sketch*` 套件（8 套件 28 条） | R-SK-1/2：事件驱动 + 增量 | 无任何用例喂 `device::InputEvent` 或 `StrokeEvent` | ❌ 用例覆盖为零 | **P0**（随 GAP-01） |
| **F10** | 整个 `StackGates` 套件 | A13：「UIG1/UIG2 全绿」 | 只跑 G7/G3/自建 vk 扫描；UIG1/UIG2 是 CTest 条目（`CMakeLists.txt:498-500`），不在 `StackGates` | ⚠️ 部分 | **P2** |
| **F11** | `GridOverlay.*` 8 条（`tests/magicblocks_l4_stack_test.cpp:1357-1620`） | 需求「带网格指示」 | 只验 `GridOverlay` 自身几何/像素（如 `OverlayPixelsAppearOnTarget`(1538)、`EmptyGeometryRefusesToDraw`(1600)）；CLI 出图里没有 overlay | ⚠️ 组件级真绿、集成级假绿 | **P0** |

> **不构成假绿、但值得写进结论的**：`StackRender.*` / `StackCellOrder.*` 的像素断言是真硬的 —— `analyze()` 统计前景比例、bbox 长宽比、中心偏移、颜色数（`l4_stack_test.cpp:871-908`，用例 `:1060-1317`），`SameBoardDifferentStrokeOrderGivesSameInnerPixels`（`:2547-2620`）做全帧逐字节比较。这是本仓最扎实的像素护栏。

---

## 5. 实测命令与输出（可复现）

环境：Windows，`E:\workspace\myspace\Mine\MagicBlocks\build` 下既有二进制（2026-11-10 14:33 构建）。像素统计用临时脚本 `MagicBlocks/build/tmp/audit_png.py`（**非入库路径**）。

```text
[1] magicblocks-l3-cli.exe sketch --width 320 --height 200 --strokes 3 --output .\tmp\audit_sk_ribbon.png
[sketch] 笔画 3 | 实体 3 | 顶点 1332 索引 2628 | doc_hash=49d6ebfd9fc02367 mesh_hash=a8926c2372cbbdc7
[sketch] 已写出 .\tmp\audit_sk_ribbon.png (320x200) | merge=0 pattern=0
EXIT=0

[2] ... sketch --width 320 --height 200 --strokes 2 --sketch-pattern 1 --output .\tmp\audit_sk_region.png
[sketch] 笔画 2 | 实体 2 | 顶点 48 索引 72 | doc_hash=7c6798d58e5eed08 mesh_hash=0e05114c74f766ca
EXIT=0
    ... --strokes 4 --sketch-pattern 2 --merge  ->  实体 1 | 顶点 320 索引 672   EXIT=0
    ... --strokes 0                             ->  [sketch] 没有产生任何实体,拒绝出图  EXIT=1
    ... (缺 --output)                           ->  --output 必填                  EXIT=2
    两次同参数运行 SHA1 相同：29EA6A6133BDA1C9EE84CDF99703888999AF1C92

[3] magicblocks-l3-cli.exe stack --width 320 --height 240 --layers 8 --strokes 3 --cell-size 0.5 --column-height 1 --color-mode by-layer --output .\tmp\audit_vx_basic.png
[stack] 笔画 3 | 方块 92 -> 请求 92 -> 合批 2 (pass 2) | drawcall 2 管线切换 2 | doc_hash=5237df81ff5510a3 inst=92 | 已写出 .\tmp\audit_vx_basic.png (320x240)
EXIT=0
    ... --inner-pass off  ->  合批 1 (pass 1) | drawcall 1 管线切换 1 | doc_hash=5237df81ff5510a3   EXIT=0
    ... --strokes 3 --dry-run
[stack] dry-run 笔画 3 | 方块 92 | 发射 95 跳过 3 | doc_hash=18afbcae00c67113
EXIT=0                          <<< 与 [3] 的 doc_hash 不同 —— A9 不成立

[4] stack --strokes 2 --template circle --template-radius 2   -> 方块 222   EXIT=0
    stack --strokes 2 --template sphere --template-radius 2   -> [code=8] 模板第 0 个偏移 (0,-2,0) 展开后 y=-2 越出 [0, 8)   EXIT=1
    stack --layers 12 --strokes 1 --lock-layer 4 --template sphere --template-radius 2 -> 方块 332   EXIT=0
    stack --inner-depth-const 0            -> --inner-depth-const 不可用: 它依赖未启用的 depthBounds 特性(D-VX-4′)   EXIT=2
    stack --strokes 0                      -> [stack] 参数错误: --strokes 必须 >= 1   EXIT=2   （设计 A11 写 exit 1）
    stack（缺 --output）                   -> [stack] 参数错误: --output 必填          EXIT=2
    stack --lock-layer 9                   -> [code=4] lock(9) 越界：合法层号 [0, 8)    EXIT=1
    stack --template square --column-height 2 -> [stack] 参数错误: --template 与 --column-height 互斥   EXIT=2
    同参数两次运行 SHA256 相同：C4125573AB9B6CA1E5BDF17EA05414D7EBA22BC6DA682056AAD1E89A0A3B82BB

[5] magicblocks_test.exe --gtest_filter='Sketch*:Stack*:BlockSet*:ShapeTemplate*:GridOverlay*'
[==========] 115 tests from 21 test suites ran. (5276 ms total)
[  PASSED  ] 115 tests.
magicblocks_test.exe --gtest_filter='Sketch*'  -> 28 tests from 8 suites, PASSED 28
--gtest_list_tests: 全仓 474 条用例（71 个套件）

[6] 门控
check_scene_no_clock.py --root MagicBlocks  -> 通过: 14 个文件 / 3697 行代码   EXIT=0  （SCAN_DIRS 不含 src/features）
check_l3_no_gameplay_names.py (G7)          -> 通过（扫描 32 个文件）          EXIT=0
check_layer_purity.py  (G3)                 -> [G3] OK backend 扫描 137 个文件  EXIT=0

[7] 存在性检查
grep "Watertight"        tests/**           -> 0 命中（A6 无判据）
grep "SteadyState|ZeroVma"                 -> 0 命中（A11 无判据）
grep "from_outline_subdivided" include src -> 0 命中（F-ST 前置未开工，本次不追）
grep "create_pipeline_for_shape" src/features -> 0 命中（D-VX-8′ 遵守）
grep "grid_ov|create_grid_overlay" src/l3_cli -> 仅 1 处注释（gizmo :1527），stack 未接
grep "feed(" src/features                  -> 0 命中（GAP-01）
build/*.exe: cube-demo / l0-cli / l2-cli / l3-cli / sink-verify / ui-cli / test —— 无 magicblocks-fsk-cli

[8] CLI 参数不隔离（S-8）
l3-cli sketch --strokes 1 --template circle --output x.png   -> 已写出 (320x200)  EXIT=0
l3-cli stack  --strokes 1 --sketch-pattern 1 --output x.png  -> 已写出 (64x64)    EXIT=0

[9] 像素统计（背景 RGBA(15,18,26,255)）
audit_sk_ribbon.png   fg=0.0720 distinct=39 corners=全背景 bbox=126x120
audit_sk_region.png   fg=0.0503 distinct=4  corners=全背景 bbox=109x65
audit_sk_merge.png    fg=0.0755 distinct=12 corners=全背景 bbox=115x143
audit_vx_basic.png    fg=0.0784 distinct=16 corners=全背景 bbox=122x70
audit_vx_innpf_off    fg=0.0784 distinct=10 corners=全背景 bbox=122x70
_out_l4_stack_cm_flat.png   fg=0.0700 distinct=6  bbox=82x46
_out_l4_stack_cm_layer.png  fg=0.0700 distinct=16 bbox=82x46   <<< 同几何，仅颜色变
320x200 的 9 个 _out_l4_*.png（wave/det_a/det_b/merge/region/stack_basic/stack_det_a/stack_det_b/stack_dryrun）全部 256283 字节
```

### MSAA 旁证（供团队另一条线参考，不属本任务范围）

```text
blocks --msaa 1 vs --msaa 4 (160x120)：differing_pixels = 499/19200
sketch --msaa 1 vs --msaa 4 (160x120)：differing_pixels = 653/19200
```
含义：`l3-cli blocks/sketch` 的 `--msaa` 确实改变像素（管线 `d.samples` 在 `src/l3_cli/main.cpp:392` 透传）；F-VX 同理（`renderer->create_passes(device, args.msaa)`，`:2483`）。这是「MSAA 修复」在 CLI 层的可观察快照，**不代表** GPU 端解析/呈现路径正确（需另测）。

---

## 6. 本次审计的局限（必须如实说明）

1. **没有复跑全量 `ctest`**（497 条，软件光栅）。只跑了 `Sketch*/Stack*/BlockSet*/ShapeTemplate*/GridOverlay*` 共 115 条（全绿），并读取 `build/Testing/Temporary/LastTest.log`（2026-11-10 15:07 起，497/497 条目均已执行）。**「100% tests passed」汇总行未截取到**，A14 的最终判定留给 CI。
2. 二进制是 **2026-11-10 14:33** 的，而工作区当前**有未提交改动**：`git status` 显示 `MagicBlocks/CMakeLists.txt`、`src/backend/vulkan/*`、`tests/magicblocks_msaa_test.cpp`、`tests/magicblocks_test.cpp`、`src/android/*` 等 **19 个文件**（含 MSAA 线）。**本文的像素/CLI 实测对应那一版二进制，不等于当前源码。**任何修复之后须用 `tools/build.ps1` 重建再复核。
   > 附注：`CMakeLists.txt` 的未提交改动把 `src/backend/vulkan/msaa_render_pass_test.cpp` 加进 `magicblocks_test` 源表 —— 属 L1/L3 MSAA 线，**不在本审计范围**，本文不评价。
3. **GAP-10（dry-run 与全跑 hash 不同）的根因本文未定位**：两处读的都是 `set->hash()`（`src/l3_cli/main.cpp:2392` 与 `:2398`），`--dry-run` 分支在 `:2395`；本文只陈述实测事实，根因留给实现方。
4. 「像素是否正确」只基于 `fg_ratio / distinct colors / bbox / 四角背景 / 重复运行逐字节一致`，**没有**与人工对照图或 golden 比对（无 golden，见 GAP-07）。能证明「画出了东西且确定性」，**不能证明「画的是需求要的那个东西」**。
5. `tests/data/_out_l4_*.png` 是用例运行留下的产物，不是基线，本文没有把它当基线用。

---

## 7. 缺口分级汇总（供队长排期）

### P0 —— 阻断

| ID | 缺口 | 建议（不在本任务范围） |
|---|---|---|
| P0-1 | **GAP-01 + GAP-02** F-SK 无「画布 + 输入链路」 | 补 `CanvasParams` + `InputEvent → StrokeEvent → IPainter`（或等价最小画布），把 `screen_to_ray ∩ 画布平面` 落进 features；至少一条「事件序列 → 笔画点」单测 |
| P0-2 | **GAP-04** F-VX 缺 `IStackSession` | 补状态机头 + 实现，把「落笔/撤销/改色/锁层」收口；CLI 改为调用它；补「撤销后像素回退」用例 |
| P0-3 | **GAP-05** 网格指示没接进出图 | `StackFrameDesc` 加 overlay 段、CLI 录制 overlay pass；加「带/不带 overlay 像素不同」用例 |
| P0-4 | **GAP-06** 稳态零 VMA（R-SK-11/A11） | 补 `IPrimitiveRenderer` 最小版 + `SteadyStateZeroVma` 用例；或在 SK 设计里**显式撤销 A11 并记录裁定**（不能沉默） |
| P0-5 | **GAP-07** golden 基线缺失（A2/A8 防假绿的前提） | 生成 `golden_fsk_*.png` / `golden_l4_stack_*.png` 并登记进 `tests/data/README.md`；把结构断言（四角背景 / 中心占比 / bbox 长宽比 / 颜色数）做成用例 |

### P1 —— 应有（判据写了但不存在，或存在但验不到）

- **GAP-03** 双表示回到 L3：把 `PathStyle/RegionOp/OutlineOptions/OutlineSet.source_style` 挪（或镜像）到 `primitives`，补 `source_style 与输入一致` 断言（A13 第 3 条）
- **GAP-08** F-SK 的 `--camera ortho|perspective` 与三态（`record/replay/reproduce/dump`）—— 决定 A3/A7/A8
- **GAP-09** 笔刷层锁走**真实相机**路径的用例（A12 原意）
- **GAP-10 / F1** `--dry-run` 与全跑 `doc_hash` 一致（先修用例的 `substr` 取位，再查实现）
- **§4-F2…F6** 把弱断言/空断言换成真正的像素或字节比较（A6 的「只改 64-79 逐字节相同」、`ByStrokeUsesStrokeId` 尾行自比、两个 CLI e2e 的「字节数 > 2000 即算画出东西」）
- **A5 / A6** 带孔轮廓用例（`outer=1, holes=1`）与水密用例（每条无向边 2 次）
- **R-SK-9 / R-SK-10** `set_stroke_color`（改色不动几何）+ 撤销后**像素**回退
- **S-5** `scene_no_clock` 扫描范围扩到 `src/features/**`
- **F-VX「3 个面」**（原文字样）的落地裁定

### P2 —— 可选 / 文档校准

- `--strokes 0` 退出码设计写 1、实现是 2
- CLI 参数跨子命令不隔离（S-8）
- 「透明」「默认立方体」「关闭深度测试」的措辞与实现口径对齐
- 测试产物写进 `tests/data/`（S-7）
- `sphere` 模板必须配 `--lock-layer`，CLI 帮助未说明
- `tests/data/README.md` 未登记 L4 产物
- `StackTypes.CellYZXOrderIsContract`（`l4_stack_test.cpp:91-92`）**与设计一致**（不是缺陷），但 `a={5,0,0} < b={0,1,0}` 一条同时混入「y 优先于 x」，建议拆成两条更易读

> **计数**：P0 = 5（P0-1 含 GAP-01+GAP-02；P0-2=GAP-04；P0-3=GAP-05；P0-4=GAP-06；P0-5=GAP-07）
> P1 = 9（GAP-03 / GAP-08 / GAP-09 / GAP-10 / §4-F2…F6 / A5+A6 / R-SK-9+R-SK-10 / S-5）
> P2 = 7

---

## 附录 A：关键文件与行号索引

| 主题 | 位置 |
|---|---|
| F-SK 文档模型 / 双表示枚举 | `MagicBlocks/include/magicblocks/features/sketch/sketch_document.h:56-104` |
| F-SK 撤销/重做/哈希 | `MagicBlocks/src/features/sketch/sketch_document.cpp:39-176` |
| F-SK 轮廓 / 挤出管线 | `MagicBlocks/src/features/sketch/sketch_pipeline.cpp:69-218`（`stroke_outline` `:93,147`、`from_outline` `:189`） |
| F-SK CLI | `MagicBlocks/src/l3_cli/main.cpp:1731-2116`，分派 `:2556` |
| L3 `from_outline` / `stroke_outline` | `MagicBlocks/include/magicblocks/primitives/mesh_factory.h:72,79`；`MagicBlocks/src/primitives/mesh_factory.cpp:875,905,1031` |
| L3 `IPainter` | `MagicBlocks/include/magicblocks/primitives/painter.h:98-142`；`MagicBlocks/src/primitives/painter.cpp:44-116` |
| L3 `IPrimitiveRenderer` | `MagicBlocks/include/magicblocks/primitives/primitive_renderer.h:72-96` |
| F-VX 数据 / 笔刷 / 渲染 / 覆盖层 / 颜色 / 模板 / 命令流 | `MagicBlocks/src/features/stack/{block_set,block_brush,stack_render,grid_overlay,color_mode,shape_template,stack_command}.cpp` |
| F-VX 双 pass desc | `MagicBlocks/include/magicblocks/features/stack/stack_render.h:78-79`；`MagicBlocks/src/features/stack/stack_render.cpp:550-567` |
| F-VX CLI | `MagicBlocks/src/l3_cli/main.cpp:2165-2537`，分派 `:2557` |
| 测试 | `MagicBlocks/tests/magicblocks_l4_sketch_test.cpp`、`magicblocks_l4_stack_test.cpp`、`magicblocks_l4_stack_command_test.cpp` |
| 门控 | `tools/check_layer_purity.py`、`tools/check_l3_no_gameplay_names.py`、`tools/check_scene_no_clock.py:56`、`MagicBlocks/CMakeLists.txt:482-502` |

## 附录 B：本文中「未验证」的条目（不得当作结论使用）

- 全量 `ctest 497/497` 是否 100% 通过（只见到条目被执行）
- 跨平台（Linux/lavapipe）一致性
- 「透明」「3 个面」两项需求是否本意如本文字面理解
- GAP-10 的根因
- 真实设备上 MSAA 的解析（resolve）路径是否正确
