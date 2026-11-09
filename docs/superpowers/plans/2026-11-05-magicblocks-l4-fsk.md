# MagicBlocks L4-F-SK 魔法涂鸦 · 落地计划（第 1 版）

日期：2026-11-05
规格：`docs/superpowers/specs/2026-11-05-magicblocks-l4-fsk-sketch-design.md`（下称"SK 设计"）
上层规格：`docs/superpowers/specs/2026-11-03-magicblocks-render-engine-sdk-design.md`（总 spec）、
`docs/superpowers/specs/2026-11-04-magicblocks-l3-primitives-design.md`（L3 spec）
流程规范：`.claude/skills/build-pipeline/SKILL.md`（审阅门 ≥80 → 执行 → 测试门全绿 → 收尾）

> **状态**：待审阅门。本版**未写任何代码**（用户 2026-11-05 裁定："先只做设计文档，不写代码"）。

---

## 0. 一句话范围

用 L0–L3 已有能力，落地**第一个玩法 F-SK 魔法涂鸦**：触摸事件 → 2D 笔画 → 带孔轮廓 →
`FromOutline` 挤出成 3D → `ICamera` 正交/透视 → **无头离屏 PNG**，并跑通
**录制 → 回放 → 复现**三态命令校验（哈希 + 逐像素一致）。

同时只补齐 F-SK 真正需要的 L3 子集（SK 设计 §3），**不新增玩法专用接口**。

---

## 1. 硬约束自检（build-pipeline §①，缺失即打回）

| 约束 | 本计划如何满足 | 章节 |
|---|---|---|
| **CLI 模式**（无头、可脚本化） | `magicblocks-fsk-cli render/replay/reproduce/dump`，全部无需 GUI | §6 |
| **离屏渲染输出图像** | 所有出图走 `IRenderTarget`（Offscreen）+ `readback_rgba8` → PNG；无窗口/无显示服务 | §6、§9 |
| **平台无关一套代码** | 与 L0–L2 同口径；差异只在软件光栅驱动（Win=SwiftShader，Linux=lavapipe） | §9.3 |
| **不回退原则** | 见 §5：所有失败路径显式报错；无"降级跳过/替代库/预编译产物" | §5 |

---

## 2. 前置事实核对（写计划时实测，带证据）

| # | 断言 | 结论 | 证据 |
|---|---|---|---|
| F1 | 基线全绿 | ✅ ctest **193/193**，0 失败 0 跳过 | 实跑 |
| F2 | 构建方式 | ✅ 必须走 `MagicBlocks/tools/build.ps1`（否则 LNK1181 kernel32.lib） | `tools/build.ps1:12-29` |
| F3 | L3 已落地模块 | ✅ 仅 types/camera/material/shader_library | `include/magicblocks/primitives/` |
| F4 | `IPainter` / `mesh` / `renderer` / `backend_hooks` | ❌ **均不存在** | 全仓 grep |
| F5 | 真下发通路 | ✅ `backend::submit_device_frame` 已存在 | `vulkan_device_frame.h:67` |
| F6 | L3 侧钩子头 | ❌ `primitives/backend_hooks.h` 不存在 | 全仓 grep |
| F7 | `scene::IScene` 增删节点 | ✅ `add_node/remove_node/set_transform/set_local_bounds/set_visible` | `scene/scene.h:31-40` |
| F8 | `device::InputEvent` 字段 | ✅ `kind/pointer_id/position/delta/button/width/height/timestamp_us/handled` | `device/events.h:35-53` |
| F9 | 输入事件种类 | ✅ 含 `PointerDown/Move/Up/Cancel` | `device/events.h:15-28` |
| F10 | L1 `draw*` 只记录 | ✅ 不下发 vkCmdDraw | `resources/command.h:113-116` |
| F11 | 实例缓冲须 Host 可见 | ✅ 设备页 `mapped()==nullptr`；只能用 Staging → HostVisible 页 | `resources/buffer.h:8-12`；`handles.h:116-133` |
| F12 | 三方库已编译入池 | ✅ `tess2.lib`(+`tess2Config.cmake`)、`Clipper2.lib`(+`Clipper2::Clipper2`)、`InkStrokeModeler` | `third_party/_install/*/release` |
| F13 | `--scene sketch` 现状 | ✅ 被显式拒绝（退出码 2，指向 L4） | `l2_cli/main.cpp:22-23`；CTest `magicblocks_l2_scene_preset_rejected` |
| F14 | `replay()` 不出图 | ✅ 只做流还原 + 哈希比对 | `scene/recorder.h:19-22` |
| F15 | 玩法词门控现状 | ✅ G3 在 backend/primitives 内禁玩法名；`src/features/` 未存在 | `tools/check_layer_purity.py` |
| F16 | `check_deps_tests.py` 精确断言用例文件数 | ⚠️ 默认 `--expect-files 12`，新增用例文件需同步 | `tools/check_deps_tests.py:22,35` |
| F17 | CMake 已有 `magicblocks-cube-demo` | ⚠️ 说明 L3 已有"透视 3D 出图"demo（非 SDK 层），本计划**不改它** | `CMakeLists.txt:262` |
| F18 | **工作区在计划撰写期间被并发修改** | ⚠️ `CMakeLists.txt`(14:35)、`src/primitives/alloc.cpp`(14:36)、`src/cube_demo/main.cpp`(14:33) | 见 §11 风险 R1 |

---

## 3. 阶段划分（按依赖顺序，每阶段自带验收）

> **原则**：每阶段结束时**构建通过 + 新增测试全绿 + 既有 193 条零回归**才算完成。
> 阶段之间**不允许**"先写一半后面补"——每阶段都是一个可独立评审的增量。

### 阶段 0：基线与门控预留（0.5 天）

| # | 产出 | 验收 |
|---|---|---|
| 0.1 | 记录基线：ctest 193/193、G1/G3/G4 全绿 | 写入计划附录 |
| 0.2 | 新增门控骨架 `tools/check_l4_no_engine_names.py`（先只实现"引擎目录不得出现玩法词"方向） | 正例 rc=0；负例（人为插入 `sketch`）rc≠0 |
| 0.3 | `tools/tests/test_check_scripts.py` 加 G8 正/反例 | 门控自测全绿（**不新增 test_*.py**） |
| 0.4 | 控制台注册 G8 到 CMake | `ctest -R gate_l4` 通过 |

**风险**：0.2 的扫描目录要与 G3 的豁免口径一致（`magicblocks` 豁免，`types.h` 等同名头不影响）。

---

### 阶段 1：出图通路 `backend_hooks`（1 天）← **整个 F-SK 的地基**

| # | 产出 | 验收 |
|---|---|---|
| 1.1 | `include/magicblocks/primitives/backend_hooks.h`（零 `Vk*`）：`FrameStats`、`submit_recorded()`、`create_shaped_pipeline()` | 公共头不含 `Vk`（G3）；含中文注释说明为什么不能直接 include backend |
| 1.2 | `src/backend/vulkan/vulkan_hooks.cpp`：`dynamic_cast` 分发到 `submit_device_frame` / `VulkanPipeline(ctx, desc, shape)` | 编译通过；G3 允许（在 backend 内） |
| 1.3 | `src/primitives/backend_hooks.cpp`：公共头声明 → 后端符号（**单一实现，无弱符号兜底**） | 链接通过 |
| 1.4 | 单测 `Hooks.SubmitRecordedProducesPixels`：录一条 `begin_pass + bind + draw + end_pass`，提交后读回像素非全背景 | 像素数 > 0 |
| 1.5 | 单测 `Hooks.CreateShapedPipelineMatchesTarget`：带深度目标派生管线，`depth_attachment().valid==true` 时管线创建成功 | `pipeline != nullptr` |

**关键技术点（写代码前必读）**：

- `submit_recorded` 需要 `IRenderDevice&` 才能拿到 `VulkanContext`：走 `dynamic_cast<VulkanRenderDevice*>`；
  失败 → `Unsupported`（**不静默忽略**）。
- `create_shaped_pipeline` 返回的 `IPipeline*` 由 **L3 侧持有**（`unique_ptr` 或缓存表），
  生命周期归调用方；backend 不登记。
- **不新增 L1 方法是硬约束**：L1 `ICommandList` 恰好 11 个方法（`resources/command.h:4-9`）。

---

### 阶段 2：`IMesh` + `FromOutline` 挤出（1.5 天）

| # | 产出 | 验收 |
|---|---|---|
| 2.1 | `primitives/mesh.h`：`MeshVertex{position,normal,uv}`、`IMesh`（`vertex_buffer()`/`index_buffer()`/`index_count()`/`bounds()`）、`create_mesh_from_data()` | 布局与 L3 shader 约定一致（`shader_library.h:17-26`） |
| 2.2 | `primitives/mesh_factory.h`：`IMeshFactory::quad()`、`from_outline(const OutlineSet&, const ExtrudeParams&)` | 头文件零 Vk，含中文说明 |
| 2.3 | `src/primitives/mesh_factory.cpp`：Clipper2 规范化 → libtess2 带孔三角化 → 上下盖 + 侧壁 | 见 2.4/2.5 测试 |
| 2.4 | 单测 `Extrude.WatertightEdges`：每条无向边恰好 2 个三角形 | A6 |
| 2.5 | 单测 `Extrude.WindingMatchesFrontFace`、`Extrude.HoleSideWallsFaceInward`、`Extrude.ThicknessAffectsBounds` | — |
| 2.6 | 单测 `Extrude.TriangulationIsDeterministic`：同输入两次得到逐字节相同的顶点/索引 | A8 前提 |

**依赖接入方式**（F12 已确认）：

```cmake
find_package(tess2 CONFIG REQUIRED ${_pool_only})      # 提供 tess2.lib
find_package(Clipper2 CONFIG REQUIRED ${_pool_only})   # 提供 Clipper2::Clipper2
target_link_libraries(magicblocks PRIVATE tess2::tess2 Clipper2::Clipper2)
```

> 包名以 `third_party/_install/*/release/lib/cmake/**/*Config.cmake` 实测为准；
> 计划阶段已确认两者都存在，执行时若名字不符 → **报阻塞**（不写兜底）。

---

### 阶段 3：`IPainter` 笔画链路（1.5 天）

| # | 产出 | 验收 |
|---|---|---|
| 3.1 | `primitives/painter.h`：`StrokeEvent`、`StrokeDesc`、`StrokePoint`、**`PathStyle`/`RegionOp`**、`OutlineOptions`、`OutlineSet`（含 `source_style`）、`IPainter`（`begin/move/end/cancel/feed/extract_outline/reset`） | 与 L3 spec §6.2 一致，字段可 1:1 映射（F8/F9）；**R-SK-12 双表示的数据位齐全** |
| 3.2 | `src/primitives/painter.cpp`：屏幕像素 → 画布 2D（`ICamera::screen_to_ray` ∩ 画布平面，D-SK-5） | 见 3.4 |
| 3.3 | 增量规则：`move` 只追加采样点；最小间距剔除；`cancel` 丢弃整笔 | 见 3.4/3.5 |
| 3.4 | 单测 `Painter.MoveIsIncremental`（A4）、`Painter.CancelDropsStroke`、`Painter.MinSpacingFiltersDensePoints`、`Painter.ClampToCanvas` | A4 |
| 3.5 | 轮廓提取：**`Ribbon`** = 中心线 Clipper2 `InflatePaths`（开放路径 + 圆头/方头封口）；**`Region`** = 首尾闭合多边形（不做笔宽 offset） | 见 3.6 |
| 3.6 | 单测 `Outline.SingleStrokeOneLoop`、`Outline.ClosedLoopHasHole`、`Outline.DegenerateStrokeErrors`；**`Outline.RegionStyleClosesPath`、`Outline.UnsupportedRegionOpReported`** | A5、**A13** |

**关键设计点**：笔画 → 轮廓的"胶囊外轮廓"生成放在 `painter.cpp` 内，
**用 Clipper2 的 `offset`（圆头/方头）**，不手写几何 —— 保证与 `FromOutline` 共用同一套轮廓表示。

---

### 阶段 4：最小渲染器 + 内置 shader 模板（1.5 天）

| # | 产出 | 验收 |
|---|---|---|
| 4.1 | `shaders/std_unlit.vert/.frag`：实例化纯色/顶点色 + 法线 → 简单半球光照；只 vert/frag（G1） | glslc 编译通过；G1 门控绿 |
| 4.2 | `shaders/stroke_ribbon.vert/.frag`：屏幕空间条带（可选叠加层） | 同上 |
| 4.3 | `src/primitives/embedded_shaders.h` + `create_shader_library()` 登记内置模板集 | `ShaderLibrary.BuiltinTemplatesRegistered` |
| 4.4 | `primitives/renderer.h`：`IPrimitiveRenderer::begin_frame/ draw_mesh_instanced / end_frame`；`FrameDesc{camera, target, clear_color}` | 头零 Vk |
| 4.5 | `src/primitives/renderer.cpp`：push constant（96B，`shader_library.h:118-131`）+ 实例属性写入 Host 可见缓冲 + 合批 | 见 4.6/4.7 |
| 4.6 | 单测 `Renderer.OneFrameAtMostThreeDraws`、`Renderer.PipelineDerivedByTargetShape` | G2 / A11 |
| 4.7 | 单测 `Renderer.SteadyStateZeroVma`（暖机 3 + 差分 30，`AllocatorStats` 增量 0） | A11 |

**性能实现要点（SK 设计 §10）**：

- 实例属性缓冲**只创建一次**（首次 `draw_mesh_instanced`），容量按上限预分配，
  之后每帧 `write()` 覆写 → **稳态零 VMA**（F11）。
- 顶点/索引缓冲在 `create_mesh_from_data` 时创建（挤出重算时**复用同尺寸缓冲**，尺寸变化才重建并计数）。

---

### 阶段 5：L4 `features/sketch` 玩法组合（2 天）

| # | 产出 | 验收 |
|---|---|---|
| 5.1 | `include/magicblocks/features/sketch/sketch_document.h` + `.cpp`：`SketchStroke` / `SketchSnapshot` / `ISketchDocument`（add/undo/redo/set_color/snapshot/restore） | 见 5.5 |
| 5.2 | `include/magicblocks/features/sketch/sketch_feature.h` + `.cpp`：`SketchFeature::on_input(InputEvent)` → `IPainter`；`build_frame()` → 挤出 + 渲染 | 见 5.6 |
| 5.3 | `src/features/sketch/` 的 G3 约束：**不得 include backend 头、不得写 GPU 命令**（只调 L3 公共接口） | G8 门控 |
| 5.4 | 状态机：Idle/Drawing，非法转移计数（SK 设计 §7.1） | — |
| 5.5 | 单测 `Document.UndoRedoRoundTrip`、`ColorChangeKeepsGeometry`、`SnapshotHashStable`、`StrokeIdNeverReused` | A9/A10 |
| 5.6 | 单测 `Feature.EventSequenceProducesOneStroke`、`PointerCancelLeavesNoStroke`、`SameEventsSameSnapshot` | R-SK-1/8 |

---

### 阶段 6：CLI + 离屏 + 命令流三态（1.5 天）

| # | 产出 | 验收 |
|---|---|---|
| 6.1 | `src/fsk_cli/main.cpp`：`render/replay/reproduce/dump` 四个子命令（§6） | 见 6.3 |
| 6.2 | `SketchSnapshot` 独立编解码（D-SK-4）；命令流落盘 `.mbfsk` | 往返一致 |
| 6.3 | CTest：`render_ortho` / `render_perspective` / `record` / `replay_verify` / `replay_png_match` / `reproduce_verify` / `bad_arg` | A1/A2/A3/A7/A8 |
| 6.4 | `magicblocks-l2-cli --scene sketch` 接入（保留 `demo` 行为不变） | 既有 `l2_*` CTest 不回归 |

---

### 阶段 7：golden + 门控收口 + 文档（1 天）

| # | 产出 | 验收 |
|---|---|---|
| 7.1 | 生成 `tests/data/golden_fsk_ortho.png` / `golden_fsk_persp.png`，并做**结构断言**（四角为背景、中心非背景占比 ∈[3%,60%]） | A2 |
| 7.2 | G8 门控完整版（含"features 不得 include backend"反向检查）+ 自测 | A12 |
| 7.3 | `check_scene_no_clock.py` 扫描范围扩到 `src/features/**` | 全绿 |
| 7.4 | `check_deps_tests.py --expect-files` 口径同步（F16） | 全绿 |
| 7.5 | 更新 `MagicBlocks/docs/ARCHITECTURE.md` 进度表 + 总 spec §4 L4 章节状态 | 文档一致 |
| 7.6 | 全量回归：ctest 全绿、四门控 rc=0 | A12 |

**总工期估算**：约 **10 个工作日**（按上述阶段相加）。

---

## 4. 文件变更清单（只列新增/修改）

```
新增 include/magicblocks/primitives/{mesh.h,mesh_factory.h,painter.h,renderer.h,backend_hooks.h}
新增 include/magicblocks/features/sketch/{sketch_document.h,sketch_feature.h}
新增 src/primitives/{mesh.cpp,mesh_factory.cpp,painter.cpp,renderer.cpp,backend_hooks.cpp,embedded_shaders.h}
新增 src/backend/vulkan/vulkan_hooks.cpp
新增 src/features/sketch/{sketch_document.cpp,sketch_feature.cpp}
新增 src/fsk_cli/main.cpp
新增 shaders/{std_unlit.vert,std_unlit.frag,stroke_ribbon.vert,stroke_ribbon.frag}
新增 tests/magicblocks_fsk_test.cpp
新增 tests/data/golden_fsk_{ortho,persp}.png
新增 tools/check_l4_no_engine_names.py
改   MagicBlocks/CMakeLists.txt（shader 列表、源文件、新 CLI、CTest、门控、find_package）
改   tools/tests/test_check_scripts.py（加 G8 正/反例）
改   tools/check_scene_no_clock.py（扫描范围加 features）
改   MagicBlocks/docs/ARCHITECTURE.md、docs/superpowers/specs/2026-11-03-...-sdk-design.md（状态）
```

**不改动**：`include/magicblocks/{device,resources,scene}/**`、`src/{backend,vulkan}/*`（除新增 hooks 文件）、
`src/scene/**`、`src/ui/**`、既有测试文件、既有 golden。→ 保证 193 条基线零回归。

---

## 5. 不回退原则的落地（逐条）

| 情形 | 本计划行为 | 禁止的做法 |
|---|---|---|
| SwiftShader/lavapipe 找不到 | CLI 退出码 3 并说明需提权/设 `VK_DRIVER_FILES` | ❌ 回退到物理 GPU |
| libtess2/Clipper2 包名不符 | **报阻塞**，不写替代实现 | ❌ 手写三角化兜底 |
| 轮廓三角化失败 | 返回 `Status` 错误，CLI 非 0 | ❌ 跳过该笔画继续出图 |
| golden 不匹配 | 测试失败 | ❌ 自动重生成 golden |
| 深度附件不支持 | 返回 `Unsupported` | ❌ 关掉深度继续画 |
| 稳态出现 VMA 分配 | 测试失败 | ❌ 放宽断言 |

---

## 6. CLI 规格（硬约束的证据）

见 SK 设计 §12。计划阶段补充**可执行的验收命令**：

```powershell
# 构建（必须走脚本，F2）
powershell -ExecutionPolicy Bypass -File MagicBlocks\tools\build.ps1

# 出图
MagicBlocks\build\magicblocks-fsk-cli.exe render --width 480 --height 360 `
    --strokes 3 --camera ortho --output out\sketch_ortho.png
MagicBlocks\build\magicblocks-fsk-cli.exe render --width 480 --height 360 `
    --strokes 3 --camera perspective --output out\sketch_persp.png
# 路径/区域双表示（R-SK-12）
MagicBlocks\build\magicblocks-fsk-cli.exe render --path-style region --strokes 1 `
    --camera ortho --output out\sketch_region.png

# 三态
MagicBlocks\build\magicblocks-fsk-cli.exe render --strokes 3 --camera ortho `
    --record out\cap.bin --output out\rec.png
MagicBlocks\build\magicblocks-fsk-cli.exe replay    --replay out\cap.bin --verify --output out\rep.png
MagicBlocks\build\magicblocks-fsk-cli.exe reproduce --replay out\cap.bin --verify --output out\rpr.png
```

**验收判据**：三条命令都退出 0；`rec.png` 与 `rep.png` **逐像素一致**；三次打印的 `hash=` 相同。

---

## 7. 测试矩阵（回溯到需求）

| 需求 | 单测 | CTest | 验收 |
|---|---|---|---|
| R-SK-1 透明 2D 画布 | `Feature.EventSequenceProducesOneStroke` | `fsk_render_ortho` | A1/A2 |
| R-SK-2 笔画连续增量 | `Painter.MoveIsIncremental` | — | A4 |
| R-SK-3 带孔轮廓 | `Outline.*` | — | A5 |
| R-SK-4 挤出成 3D | `Extrude.*` | — | A6 |
| R-SK-5 相机双模态 | — | `render_ortho` + `render_perspective` | A3 |
| R-SK-6 3D 显示正确 | `Renderer.*` | `fsk_golden` | A2/A8 |
| R-SK-7 无头出 PNG | — | `fsk_render_*` | A1 |
| R-SK-8 命令三态 | — | `record/replay_verify/replay_png_match/reproduce_verify` | A7/A8 |
| R-SK-9 撤销/改色 | `Document.*` | — | A9/A10 |
| R-SK-10 零玩法分支 | — | G8 门控 | A12 |
| R-SK-11 零分配 | `Renderer.SteadyStateZeroVma` | — | A11 |
| **R-SK-12 路径 / 布尔区域双表示** | `Outline.RegionStyleClosesPath` · `Outline.UnsupportedRegionOpReported` · `Document.PathStyleRoundTrips` | `fsk_render_region`（可选，验证区域样式出图） | **A13** |

---

## 8. 收尾（build-pipeline §⑤）

1. 清理 `build/`、`out/` 临时产物；
2. 更新 `ARCHITECTURE.md`（进度表：L3 补了哪些、L4-F-SK 已落地）；
3. 确认 `git status` 无意外未跟踪文件；
4. 产出收尾摘要（审阅得分 + 测试结果 + 变更清单 + 遗留项）。

---

## 9. 已裁定（2026-11-05 用户授权"使用你的判断"）

| # | 问题 | **裁定** | 影响 |
|---|---|---|---|
| **Q-SK-1** | R-SK-9（撤销/重做/改色）是否进首轮 | ✅ 保留（阶段 5） | +0.5 天 |
| **Q-SK-2** | 重叠笔画：并集还是各自挤出 | ✅ **不二选一**：改为双表示 `PathStyle::Ribbon`（路径，实现）+ `PathStyle::Region`（布尔区域，预留）。多笔默认并集 | 阶段 3 增加 `RegionOp` 参数与 2 条用例；**不减任何阶段** |
| **Q-SK-3** | 画布是否要交互（orbit/pan） | ✅ 否，只 2 个预设相机 | — |
| **Q-SK-4** | golden 是否纳入验收 | ✅ 是 | — |
| **Q-SK-5** | 允许新增 `primitives/backend_hooks.h` 吗 | ✅ **是**（选项 A：零 Vk 不透明钩子） | 不加则 F-SK 出不了像素 |

> **Q-SK-5 裁定理由（为什么不是 B/C）**：B（L4 直接调 backend）违 G3 门控——features 不许 include
> backend 头、不许写 GPU 命令；C（改 L1 加 `submit_immediately()`）破坏 L1"恰好 11 个方法"不变量。
> A 是唯一同时满足 G3、零改 L1、可单测的方案，且 `vulkan_device_frame.h:76-81` 早已预留此入口。

### 9.1 R-SK-12 对计划的实际影响（已并入阶段 3，设计见 SK 设计 §6.3.1）

| 变更点 | 内容 | 成本 |
|---|---|---|
| `painter.h` | 新增 `PathStyle` / `RegionOp` 枚举；`OutlineOptions` 带 `path_style`/`region_op`；`OutlineSet` 带 `source_style`/`source_hash` | 接口层面，~20 行 |
| `SketchStroke` | 新增 `path_style` / `region_op` 字段并**进快照与哈希** | ~10 行 |
| `painter.cpp` | `Ribbon` 用 Clipper2 `InflatePaths`；`Region` 只做"首尾闭合" | ~40 行 |
| 错误路径 | `region_op != Union` → `Unsupported`（**显式报错，不静默按 Union**） | 5 行 |
| 用例 | `Outline.RegionStyleClosesPath`、`Outline.UnsupportedRegionOpReported`、`Document.PathStyleRoundTrips` | 3 条 |
| **不做** | 区域的 `Difference/Intersect/Xor` 求值、区域与路径的混合合成、区域双色填充 | **明确后置**，首轮不实现、不假装实现 |

---

## 10. 与 L3 缺口的关系（如实说明）

用户 2026-11-04 裁定"严格按 L3 需求，未列入 L3 的不做"。
本计划补的 5 个 L3 文件中，**4 个在 L3 spec 中明确列出**（`painter.h`=§6、`mesh.h`/`mesh_factory.h`=P1/P2、
`renderer.h`=§4.3/§11），**1 个不在**（`backend_hooks.h`）—— 它是 `vulkan_device_frame.h:76-81`
提到的"L3 通过 `primitives/backend_hooks.h` 的弱符号接口取用"，属**既有计划的遗漏**，不是新增玩法通道。
→ 列为 Q-SK-5 请用户确认。

---

## 11. 风险登记

| # | 风险 | 影响 | 对策 |
|---|---|---|---|
| **R1** | **工作区被并发修改**：`CMakeLists.txt`(11-05 14:35)、`src/primitives/alloc.cpp`(14:36)、`src/cube_demo/main.cpp`(14:33) 在本计划撰写期间变更，来源不明 | 高：执行阶段可能与未完成改动冲突 | 执行前**冻结工作区**并重跑 F1 基线；本计划不碰 `cube_demo` 与 `alloc.*` |
| R2 | L3 缺口比预期大 | 中 | 阶段化交付，每阶段独立验收 |
| R3 | libtess2 在自交轮廓上失败 | 中 | Clipper2 先规范化（阶段 2.3） |
| R4 | 回放帧与录制帧不一致 | 高 | 全状态进流 + 确定性单测（2.6）+ A8 |
| R5 | 稳态 VMA 非 0 | 中 | 实例缓冲常驻 + `write()`（阶段 4.7） |
| R6 | golden 跨平台不一致 | 中 | 单一基线 + 结构断言；Linux 一致性列**待验证**，不宣称 |
