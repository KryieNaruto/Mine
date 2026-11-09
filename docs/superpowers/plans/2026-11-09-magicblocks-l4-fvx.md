# L4 F-VX 魔法叠叠乐 · 落地执行计划

日期：2026-11-09
前置文档：
- 设计规格 `docs/superpowers/specs/2026-11-09-magicblocks-l4-fvx-stack-design.md`（下称「设计」）
- 依赖报告 `docs/superpowers/specs/2026-11-09-magicblocks-l4-fvx-dependency-assessment.md`（下称「依赖」）
- 总 spec `docs/superpowers/specs/2026-11-03-magicblocks-render-engine-sdk-design.md`
- 范本 `docs/superpowers/plans/2026-11-05-magicblocks-l4-fsk.md`（F-SK 已落地、已出图）
- 门控 `build-pipeline`（审阅 ≥90、测试门 100 分）

---

## 0. 一句话范围

**改动范围口径（M9，精确版）**：**库与公共面零改动**（`include/magicblocks/{device,resources,scene,primitives}/` 与 `src/{backend,resources,scene,primitives}/` 一行不改）；实际会碰到的文件只有 5 处 —— 新增 `include/magicblocks/features/stack/*.h`、新增 `src/features/stack/*.cpp`、新增 `tests/magicblocks_l4_stack_test.cpp`、在 `MagicBlocks/CMakeLists.txt` 登记（`:243-245` 核心库源表旁 + `:423-442` 测试源表，新行插在 `:442` 之前）、在 `src/l3_cli/main.cpp` 加 `stack` 子命令与分派。**L0–L3「库」零改动**，`l3_cli` 是**消费方**不是库。

---

## 1. 硬约束自检（缺一即整份打回）

| # | 约束 | 本计划的落实 |
|---|---|---|
| H1 | **CLI 模式**（无头/脚本化） | 阶段 6 落地 `magicblocks-l3-cli stack`，全部参数见设计 §13.1；无窗口、无交互、无鼠标时钟读取 |
| H2 | **离屏渲染输出图像** | 阶段 6 走 `create_offscreen_target({Offscreen, extent, with_depth=true})` → `readback_rgba8` → `write_png_rgba8`，落盘真 PNG；阶段 3 起即时验图 |
| H3 | **平台无关一套渲染代码** | 渲染核心只用公共面（`IPrimitiveRenderer`/`IBatchBuilder`/`IOverlay2D`/`ICamera`/`IPicker`）；**零** `Vk*`、零后端私有头（§2.B 的裁定 D-VX-8′ 保证不需要） |
| H4 | **不回退/不兜底** | 非法参数、未知模板、锁层越界、空笔一律显式错误码 + 人话 message + exit 1/2；**无** clamp、无「跳过这一格」、无「关掉内面 pass 降级」 |
| H5 | **不污染 L0–L3** | `feat`/`src` 只新增 `features/stack/` 与 `src/features/stack/`；门禁 G1/G3/G4/G7/G8/UIG1/UIG2 全绿 |
| H6 | **一项一项来** | 阶段 0–7 严格串行；每阶段结束必须「构建通过 + 新增测试全绿 + 既有 389 条零回归」才可进入下一阶段 |
| H7 | **工作树隔离** | 执行/测试在 `.claude/worktrees/<任务名>` 独立 `git worktree` 内；收尾合并回 main 后删除工作树（`build-pipeline` ⑤） |

---

## 2. 前置事实核对（全部带证据，本轮实测）

### 2.A 基线

| 断言 | 结论 | 证据 |
|---|---|---|
| 构建 | ✅ 已有产物可用 | `MagicBlocks/build/*.exe` |
| 测试基线 | ✅ **389/389**，0 失败 0 跳过 | `ctest --test-dir MagicBlocks/build` → `100% tests passed, 0 tests failed out of 389` |
| L3 出图链路可用 | ✅ | `magicblocks-l3-cli blocks --width 320 --height 240 --blocks 8 --output ...` → `[blocks] 8 个体素 \| 实体 8 → 请求 8 → 合批 4 (合并 4) \| 管线切换 0` + 已写出 PNG |

### 2.B **关键裁定：修正设计 §10.8 / D-VX-8 的误判（P0-1 不成立）**

设计 §10.8 与 D-VX-8 主张「带深度的离屏管线无法通过公共面创建，必须先给 L1 补 `uses_depth_attachment`（P0-1），否则双 pass 落不了地」。

**本计划核实后判定：该前提错误，P0-1 不是阻塞项，本轮到阶段 0 之前不需要动 L1。**

证据链（三条互相独立）：

1. **既有三个带深度的子命令，走的全是公共 `create_pipeline`，且都出对了图。**
   `l3_cli/main.cpp` 中 `td.with_depth = true` 出现于 `:639`（`blocks`）、`:1130`（`gizmo`）、`:1733`（`sketch`）；而这三处建管线只调用 `device->create_pipeline(...)`（`:901`、`:1446`、`:1864`），**从未**调用 `create_pipeline_for_shape`。
2. **`create_pipeline_for_shape` 全仓只有一个调用点。**
   `src/android/scene_renderer.cpp:275`，而且传的是 `shape.has_depth = false`（`:272`）。即公共面与后端面在「是否带深度」这件事上，实际从未产生过分歧。
3. **后端在形状不匹配时会自建 render pass，功能正确但不复用管线的 pass。**
   `vulkan_device_frame.cpp` 的 `pass_compatible` 判定（比较 `has_depth` / `samples` / `color_format`）不通过时，走 `create_pass(ctx.device, shape, &err)` 现建一个 pass。
   ⚠️ 设计 §10.5 据此担心「多一次 pass 创建 + 两套 pass 依赖不一致 → 像素不同」。**本计划判为不阻塞**：`create_pass`（`vulkan_device_frame.cpp:84-148`）与 `create_render_pass`（`vulkan_pipeline.cpp:454-515`）**逐项一致**，所以「两套 pass 依赖不一致→像素不同」的担忧**事实不成立**；另一颗硬钉子：`hash_pipeline_desc_with_shape` 在默认形状下**退化**为 `hash_pipeline_desc`（`vulkan_pipeline.cpp:251-256`），故公共面管线 hash 与 shape 无关。

   ⚠️ **但「不动 L1」目前缺回归护栏（评审发现，已复核）**：`tests/` 中 `with_depth = true` **0 处** —— 全部是 `with_depth = false`（`magicblocks_l1_test.cpp:550/631`、`magicblocks_l2_test.cpp:68`、`magicblocks_l3_pixel_regression_test.cpp:71`、`magicblocks_test.cpp:70/269`、`ui_test.cpp:994`）；`MB_L3_CLI_PATH` 的唯一使用者是 sketch，**blocks/gizmo 没有 CTest 像素断言**。因此阶段 3 必须新增**首批「公共面 + `with_depth=true`」像素回归用例**，否则本裁定无护栏。

**裁定 D-VX-8′（覆盖设计的 D-VX-8）**：
- F-VX **不新增** `resources::PipelineDesc::uses_depth_attachment`，**不改** L1 公共面、**不改**后端。
- 双 pass 的两个 `PipelineDesc` 均走公共 `create_pipeline(desc)`；深度行为完全由 `DepthState` + `RasterState` 决定，与 `has_depth` 形状字段无关。
- `create_pipeline_for_shape` 保持 Android 专用，**L4 禁止调用**（否则 L4 依赖后端私有头，破坏单向依赖）。
- 若阶段 3 实测真的出现「两个 pass 形状不兼容导致像素异常」，**停工如实报告**，不在本计划内私自扩 L1 —— 依 `build-pipeline`「缺失即补齐或如实报告阻塞」，而非静默绕过。

### 2.C 深度能力的事实（承接依赖报告，已复核）

| 事实 | 证据 | 对 F-VX 的影响 |
|---|---|---|
| `depthConstWrite` 落成的是 `depthBounds`，不是「写常量深度」 | `vulkan_pipeline.cpp:614`（`depthBoundsTestEnable`）、`:619-620`（上下界钉成 `depth_const`） | 内面 pass 的语义**不依赖**它精确；靠 `depthTest=false + cull=None` 即可 |
| `depthBounds` 特性从未被请求 | `vulkan_render_device.cpp:150,157`（`VkPhysicalDeviceFeatures feats{}` 全 0） | 若强依赖 `depthConstWrite` 属未定义行为 → 故 **D-VX-4′：内面 pass 不承担正确性于 `depthConstWrite`** |
| `DepthState` 三个开关全 false 时后端不挂 depth-stencil 状态 | `vulkan_pipeline.cpp:668-671` | 内面 pass 必须至少 `depthWrite=true` |
| 内面先画 = 白画（与只有表面逐像素 0 差异） | 依赖报告探针 K 例；设计 §1.4 | pass 顺序：**表面在前、内面在后** |
| `depth_const = 0.0` 会让整帧变清屏色 | 依赖报告探针 T 例（76800/76800 全背景）—— ⚠️ **但该观测与 E/J 例互斥,见设计 §1.4 勘误 E-VX-2,不得作为设计依据** | F-VX **不设 `depth_const`**,`depthConstWrite=false` 时该字段不参与 |

**裁定 D-VX-4′**：内面 pass 的**正确性来源**只有三件事 —— `depthTest=false` + `CullMode::None` + **排在表面 pass 之后**（`DepthState` 至少一个写位为真，否则后端不挂深度状态）。

**`depthConstWrite` 在 F-VX 中全面禁用**：不置位、不暴露 CLI、不作任何验收判据。
理由：它被落成 `depthBounds`（`vulkan_pipeline.cpp:614-620`），而 `depthBounds` 特性**从未被请求**（`vulkan_render_device.cpp:150,157` 是 `VkPhysicalDeviceFeatures feats{}` 全 0，全仓 0 处置位）→ 对未启用特性置 `depthBoundsTestEnable=VK_TRUE` 属**未定义行为**。
**并且设计要求中 T 例（`depth_const=0.0` 全背景）与 E/J 例（`depth_const=1.0` 逐像素等同不开）在同一驱动上互相矛盾、不可能同时成立** → 两条都只能算「不可复现的单次观测」，**均不得作为设计依据**（设计已记勘误 E-VX-2）。
不新增 `--inner-depth-const` / `--allow-inner-depth-near`（把 UB 固化成 API 是设计错误）。
**A8 只断言「on/off 两图像素不同」，不断言任何 `depth_const` 语义。**

**设备级风险（评审 M3）**：`feats{}` 全 0 同时关掉了 `depthBounds` / `depthClamp` / `fillModeNonSolid` / `wideLines` / `logicOp` 等**所有默认 false 特性**。现行代码恰好没用到，属运气。F-VX **不得依赖其中任意一项**（登记为 R2）。

### 2.D 其余前置

| 项 | 事实 | 证据 |
|---|---|---|
| `InstanceAttribs` = 112 B，`base_color` @64 | 改色只改这一段 | `batch.h:100-109` |
| `DrawRequest::pipeline_hash` 是主排序键，逐批 `bind_pipeline` | 两份 desc → 2 个 draw | `batch.h:55`、`primitive_renderer.cpp:58,72-83` |
| `IPicker` 无「方块实例」求交 | 落笔用射线→层平面，不需要 | `picker.h:5` |
| G7 禁用词含 `blocks`/`voxel`/`叠叠乐`/`积木`，但 `stack` 未登记 | 玩法名只准在 `features/` 与文档 | `tools/check_l3_no_gameplay_names.py` |
| 新测试 `.cpp` 必须登记 `CMakeLists.txt` | `add_executable(magicblocks_test ...)` 为 **`:423-442`**；**右括号在 `:442`**（`tests/magicblocks_msaa_test.cpp)`）。新增行插在 **`:442` 之前**（即 `tests/magicblocks_l4_sketch_test.cpp`(:441) 与 `msaa_test.cpp)`(:442) 之间），**不要**把右括号留在新行上 | 否则静默不编译 |
| `ctest` 必须 `-j 1` | 顺序执行，避免设备竞争 | 沿用 F-SK 做法 |

---

## 3. 阶段划分（严格串行，每阶段自带验收）

> 每阶段结束的统一验收口令：
> ```
> ninja -C <worktree>/MagicBlocks/build      # 等价 build.ps1
> ctest --test-dir <worktree>/MagicBlocks/build -j 1
> ```
> 通过判据：**构建 0 错；用例 = 389 + 本阶段新增数，0 失败 0 跳过。**任一不满足 → 停在本阶段修复，不得进入下一阶段。

| 阶段 | 主题 | 产出 | 新增用例（预估，**以实测回填**） | 结束后用例数 |
|---|---|---|---|---|
| 0 | 基线冻结 + 门禁确认 | 无（只记录） | 0 | 389 |
| 1 | `Cell`/`Block`/`GridSpec`/`IBlockSet`（纯 CPU） | 3 文件 | ~14 | 回填 |
| 2 | `IBlockBrush`（射线→吸附→DDA→去重）+ 层锁 | 2 文件 | ~17 | 回填 |
| 3 | 渲染接线：双 pass + 内面 + **首批公共面带深度像素回归** | 2 文件 | ~6 | 回填 |
| 4 | `IOverlay2D` 层网格 | 1 文件（改 2 处） | ~3 | 回填 |
| 5 | 颜色模式 + `IShapeTemplate` | 2 文件 | ~11 | 回填 |
| 6 | CLI `stack` 子命令 + 离屏 PNG + golden | 1 文件（改 2 处） | ~6 | 回填 |
| 7 | 命令流三态 + 全量回归 + 收尾 | 文档 | ~4 | 回填 |

> 每个阶段结束时把该行「结束后用例数」**按实测 ctest 输出回填**，不允许估算值留档。**只增不减**是硬要求（A14）：任一阶段出现用例数下降 = 有测试被删/被跳过，立即停下排查。

---

## 4. 阶段 0 · 基线冻结与门禁确认

### 4.1 工作
1. 建立隔离工作树：`git worktree add .claude/worktrees/20261109-l4-fvx -b task/20261109-l4-fvx main`（由主会话执行一次）。
2. 在工作树内独立构建 + 跑全量测试，记录真实基线（**不得引用主工作区数字**）。
3. 确认 G7 门禁对 `features/` 的策略：`tools/check_l3_no_gameplay_names.py` 只扫 `include/magicblocks/primitives` 与 `src/primitives`，**不扫** `features/` → `stack`/`叠叠乐` 命名在 `features/` 层合法。
4. 确认 `tools/check_layer_purity.py`（G3）对 `src/features/` 的规则：禁止 `Vk*`、禁止 `vkCmd`/`CmdDraw`。

### 4.2 验收命令
```bash
git worktree list                                   # 必须出现 .claude/worktrees/20261109-l4-fvx
ctest --test-dir MagicBlocks/build -j 1             # 记录真实用例数（预期 389）
python tools/check_l3_no_gameplay_names.py --root MagicBlocks
python tools/check_layer_purity.py --root MagicBlocks
```

⚠️ **门禁口令带 `--root MagicBlocks`（评审 M5，第二轮已实测纠正）**：
`check_l3_no_gameplay_names.py:158` 的 `--root` **默认值就是 `"MagicBlocks"`**，因此从仓库根跑不带 `--root` **同样正确**（实测两种跑法均为「扫描 32 个文件」exit 0）。
**先前记录的「不带 `--root` 会扫 0 文件判通过」是错误结论，已撤销** —— 不存在门禁假绿。保留 `--root MagicBlocks` 只为显式无歧义，并仍建议断言输出里的扫描文件数 **N > 0**（当前实测 N=32）作为低成本自检。

### 4.3 失败排查
- 工作树内构建失败 → 三方库池不在工作树内，按 `tools/setup-env.sh` **主路径**重新拉取/编译（不回退、不复制主工作区二进制）。
- 用例数 ≠ 389 → 先查工作树是否与 main 同步，勿带病开工。

---

## 5. 阶段 1 · 数据模型 + `IBlockSet`（纯 CPU，零 GPU）

### 5.1 产出文件
```
include/magicblocks/features/stack/block_set.h     // Cell / Block / GridSpec / IBlockSet
src/features/stack/block_set.cpp
tests/magicblocks_l4_stack_test.cpp                // 新建；注册进 CMakeLists.txt:423-442 源表，插在 :442 之前
```

### 5.2 实施要点
- `Cell{int32 x,y,z}`、`Block{Cell; uint32 color; uint32 stroke_id; uint32 flags}`，`static_assert(sizeof(Block)==24)` 与 `alignof==4`。
- `IBlockSet`：`add(Block)`/`add_stroke(cells, color, stroke_id)`/`remove_stroke(stroke_id)`/`contains(Cell)`/`query_stroke`/`hash()`/`size()`/`cells_in_index_order()`。
- `stroke_id` 单调递增且**永不复用**（I2）。
- `occupied` 用 `unordered_map<Cell>` 做**派生缓存**，可随时从真源重建，**不进哈希、不得用于输出遍历**（确定性铁律）。
- 哈希口径写死：对真源数组**下标序**逐字节 `Block` 累加，不允许遍历 `unordered_map`。
- **本阶段不引入任何 GPU/设备代码**，测试不创建 RenderDevice。

### 5.3 验收命令
```bash
ninja -C MagicBlocks/build
ctest --test-dir MagicBlocks/build -j 1
```

### 5.4 测试用例清单
- `BlockSet.SizeOfBlockIs24`
- `BlockSet.AddThenContains`
- `BlockSet.AddIsIdempotentByCell`
- `BlockSet.StrokeIdMonotonicNeverReused`
- `BlockSet.RemoveStrokeRemovesExactlyThatStroke`
- `BlockSet.RemoveStrokeLeavesOtherBlocksByteIdentical`
- `BlockSet.RemoveUnknownStrokeReturnsUnknownStroke`
- `BlockSet.HashChangesWhenColorChanges`
- `BlockSet.HashStableAcrossRuns`
- `BlockSet.OutputOrderIsIndexOrderNotHashOrder`
- `BlockSet.GridSpecDefaults`
- `BlockSet.CellYOutOfRangeRejected`
- `BlockSet.FlagsUndefinedBitsRejected`
- `BlockSet.LayerOutOfRangeReturnsLayerOutOfRange`

### 5.5 失败排查
- `sizeof(Block)!=24` → 检查是否有隐式填充；`Cell` 必须 3×int32。
- 哈希跨运行不稳 → 十有八九遍历了 `unordered_map`，改为真源下标序。
- 用例没被编译 → 忘了登记 `CMakeLists.txt:423-442`（插在 :442 之前）。

---

## 6. 阶段 2 · `IBlockBrush`（射线→吸附→DDA→去重）+ 层锁

### 6.1 产出文件
```
include/magicblocks/features/stack/block_brush.h
src/features/stack/block_brush.cpp
```

### 6.2 实施要点
- 链路：`ICamera::screen_to_ray`（`camera.h:86`）→ `IPicker::intersect_plane`（`picker.h:77`）→ `floor((p - origin)/cell_size)` 整数吸附 → 3D supercover DDA → 去重。
- **去重判据 = 整数 `Cell` 精确相等**，禁止 ε 比较（浮点 ε 会在格线两侧都不等 → 重叠方块）。
- 层锁：`lock(layer)` / `unlock()`；`locked` 时 `cell.y` **恒等于** `locked_layer`，相机俯仰不影响。未给 layer 调 `lock` → `LockLayerRequired`；越界 → `LayerOutOfRange`（**不 clamp**）。
- 扫掠去重跑：同格二次落笔不重复 emit，计数进 `skipped_occupied`。
- 错误码按设计 §6.6 全集实现，每条带 `detail`。
- 「整笔成或整笔不成」：柱高越界 → 整笔回滚，不留半根柱。

### 6.3 验收命令
```bash
ninja -C MagicBlocks/build
ctest --test-dir MagicBlocks/build -j 1
```

### 6.4 测试用例清单
- `StackSnap.RayToCellExact`
- `StackSnap.ExactGridLinePoint`
- `StackSnap.NegativeCoordinates`
- `StackSnap.NonUnitCellSize`
- `StackSnap.RayParallelToLayerReturnsRayParallelToLayer`
- `StackSnap.BehindCameraReturnsRayMissesLayer`
- `StackSweep.DiagonalHitsEveryCell`
- `StackSweep.NoCornerLeak`
- `StackSweep.SinglePointEmitsOnce`
- `StackSweep.DuplicatePassDoesNotReEmit`
- `StackSweep.SkippedOccupiedCounted`
- `StackSweep.ColumnHeightHonored`
- `StackSweep.ColumnOverflowFailsWholeStroke`
- `StackLayerLock.LockedLayerIgnoresCameraPitch`
- `StackLayerLock.UnlockRestoresGridHit`
- `StackLayerLock.LockWithoutLayerReturnsLockLayerRequired`
- `StackLayerLock.LockOutOfRangeReturnsLayerOutOfRange`

### 6.5 失败排查
- 斜线漏格 → DDA 步进用了「采样点」而非 supercover 边界穿越，改用整数边界。
- `a==b` 出两格 → DDA 起止未去重。
- 锁层在 Move 期间漂移 → 锁层被在使用时**重算**了，应在 Down 时定格。

---

## 7. 阶段 3 · 渲染接线：双 pass + 内面渲染（首次出图）

### 7.1 产出文件
```
include/magicblocks/features/stack/stack_render.h
src/features/stack/stack_render.cpp
```

### 7.2 实施要点
- 两个 `PipelineDesc`：pass A（`depthTest=depthWrite=true`, `depthOp=LessEqual`, `cull=Back`）、pass B（`depthTest=false`, `depthWrite=true`, `cull=None`, **`depthConstWrite=false`**，依 D-VX-4′ 禁用）。
- 两者均走公共 `device->create_pipeline(desc)`（裁定 D-VX-8′：**不调用** `create_pipeline_for_shape`，**不改** L1）。
- 同一份实例缓冲绑定两次；差别只在 `bind_pipeline`。两个 `pipeline_hash` → 2 个 `DrawRequest` → **2 drawcall**。
- 一帧内 `begin_pass`→…→`end_pass`，再 `begin_pass`→…→`end_pass`，**一次** `submit_command_list`（UIG1/UIG2）。
- pass A `clear`，pass B **不** clear。
- 内面实例按 `(y,z,x)` 升序写入实例缓冲（D-VX-5，把「内面之间谁盖谁」压成契约）。
- **不设 `depth_const`**（`depthConstWrite=false`，该字段不参与、不参与语义；D-VX-4′）。禁止为了「让 hash 不同」而去动它 —— `depthTest` / `cull` 的差异已足以产生不同 `pipeline_hash`。
- 渲染器只**录制**命令，不提交；提交权在调用方。

### 7.3 验收命令
```bash
ninja -C MagicBlocks/build
ctest --test-dir MagicBlocks/build -j 1
```
**本阶段必须第一次亲眼看到图**：在测试内离屏出 PNG 并做**进程内**像素断言（`readback_rgba8` 后直接统计，见 M8 说明），非背景像素占比 > 0。

> **M8 说明（评审发现，已复核；行号第二轮修正）**：`tools/verify_png_content.py` **不在** `CMakeLists.txt` 的 `add_test` 条目中（真实区间 **`:463-544`**，共 21 条），因此 gtest 进程**无权**也**无法**调用它作为通过条件。它的两条口径对 F-VX 也不完全适用：「跨度 >20%」判据（脚本 `:140-146`）对「高柱 + 大片空地」的构图可能误报。
> 因此：**测试门判据一律在 gtest 进程内实现**（`readback_rgba8` → 自行统计前景占比 / 颜色数 / bbox）；`verify_png_content.py` **只作人工诊断辅助**，不作为任何用例的通过条件。CLI 端到端用例断言「文件存在 + 尺寸正确 + 前景占比 > 0 + 两次运行逐字节相同」，四者都在进程内做。

### 7.4 测试用例清单
- `StackRender.PassPairIsDistinct`（两个 `pipeline_hash` 不同 —— **只断言 hash，不断言附件形状**）
- `StackRender.BothPassesRenderOnDepthTarget`（在 `with_depth=true` 的离屏目标上两 pass 都真的画出像素；**这是首批「公共面 + 带深度」像素回归**，补齐当前 0 处护栏）
- `StackRender.TwoDrawCallsOneSubmit`
- `StackRender.InnerPassChangesPixels`（on/off 两图不同）
- `StackRender.InnerPassIsAfterSurfacePass`（先把内面换到前面跑一版，确认像素与只有表面相同 → 反证顺序契约）
- `StackRender.SingleInstanceColumnRenders`（**边界**：`column_height=1` 单格笔迹 → `instance_count==1`）

> **M6 说明（评审发现，已复核）**：原 A7 要求「断言管线 `attachment_shape().has_depth == true`」，但 `attachment_shape()` **只存在于后端私有头** `vulkan_pipeline.h:88-89`，公共面 `IPipeline` 无此接口，且公共面 hash 与 shape 无关（`vulkan_pipeline.cpp:251-256`）→ **原 A7 在当前公共面下不可实现**。故拆成上面两条：`PassPairIsDistinct`（hash，公共面可断言）+ `BothPassesRenderOnDepthTarget`（像素，公共面可断言）。
> **`BothPassesRenderOnDepthTarget` 实现纪律**：只 include 公共头。`check_layer_purity.py:97` 连 `tests/` 也扫，若为断言形状而 include 后端私有头，会撞 G3 精神 —— 禁止。
> **`SingleInstanceColumnRenders` 由来**：`primitive_renderer.cpp:102` 仅在 `instance_count > 1` 时绑定 binding 1。`column_height=1` 的单格笔迹会走到 `instance_count==1`，此时 binding 1 **不绑定**。
> **解法（M9，二选一，实现时必须显式选定并在注释写明）**：
> - **旁路**：单实例批次不进实例化路径，改用非实例化绘制（`instance_count=1` 时按普通 vertex 输入走）；或
> - **pad**：实例缓冲尾部补一条「退化实例」（全零矩阵 → 投影到零面积），使 `instance_count` 恒 ≥ 2。
> 无论选哪条，用例必须断言：**不出现「`instance_buffer_ != nullptr && instance_count == 1`」的组合**（对旁路：该组合不产生；对 pad：计数已变 2）。若实现后该组合仍出现且渲染错位，**停工报告**，不得靠「反正图上有东西」蒙混。

### 7.5 失败排查
- 全黑/全清屏色 → ① 深度目标没建（`with_depth=false`）；② `begin_pass` 形状与管线不匹配（后端会自建 pass，但若依赖不一致会花屏）→ 查 `vulkan_device_frame.cpp` 的 `pass_compatible`；③ 实例缓冲没上传（`pending_upload_bytes()!=0`）。
- 内面 pass 毫无效果 → ① 顺序反了（内面在前 = 白画）；② `DepthState` 三开关全 false，后端没挂深度状态（`vulkan_pipeline.cpp:668-671`）；③ `cull` 没设 `None`。
- `depthConstWrite` 行为诡异 → 依 D-VX-4′：**F-VX 不置位它**；`depthBounds` 特性未启用（`vulkan_render_device.cpp:150,157`），不要为它去改后端、也不要打开它「试试看」。

---

## 8. 阶段 4 · `IOverlay2D` 分层网格示意

### 8.1 产出文件
```
src/features/stack/stack_render.cpp（追加网格录制）
include/magicblocks/features/stack/stack_render.h（追加 overlay 参数）
```

### 8.2 实施要点
- 用 `IOverlay2D::push_line`（`overlay2d.h:136`）画当前层网格；`push_dash` / `push_rect` 可选。
- 网格线位置由 `GridSpec` 与 `ICamera` 投影得到，**必须**与 `cell_size` 严格一致（用同一条 `GridSpec`，不复制常量）。
- 网格属于 UI 层：`overlay2d` 录制在**最后**，且自成一次 pass/draw（不计入「2 drawcall」的方块预算）。

### 8.3 验收命令
```bash
ninja -C MagicBlocks/build && ctest --test-dir MagicBlocks/build -j 1
# 诊断辅助(非测试门判据;测试门判据在 gtest 进程内)
```

### 8.4 测试用例清单
- `StackOverlay.GridLineCountMatchesLayers`
- `StackOverlay.GridLinesAlignToCellSize`
- `StackOverlay.OverlayDoesNotChangeBlockPixels`（关/开网格，方块区域内像素差只在线上）

### 8.5 失败排查
- 网格与方块错位 → 网格用了复制出来的 `cell_size`，改为同一 `GridSpec`。
- 网格吃掉了方块 → `overlay` 没在最后录制 / 打开了深度测试。

---

## 9. 阶段 5 · 颜色模式 + `IShapeTemplate`

### 9.1 产出文件
```
include/magicblocks/features/stack/shape_template.h
src/features/stack/shape_template.cpp
```

### 9.2 实施要点
- `Flat` / `ByLayer` / `ByStroke` 三种模式**只改 `Block.color` → `InstanceAttribs::base_color`（偏移 64）**；几何零重建（顶点/索引缓冲、网格工厂调用次数 = 0）。
- 色板确定性：黄金分割 hue 序列（沿用 `l3_cli/main.cpp:1606-1657` 做法），无随机、无时钟。
- 空色板 → `ColorOutOfRange`。
- `IShapeTemplate`：模板 = **整数格偏移集合**（非网格）；内置 `square`/`circle`/`cross`/`sphere` 用整数判据生成（如球用 `x²+y²+z² ≤ r²`），偏移排序后 emit 保证确定。
- 未知模板 → `UnknownTemplate`；模板把方块推出网格 → `TemplateOutOfGrid`（整笔失败）。

### 9.3 验收命令
```bash
ninja -C MagicBlocks/build && ctest --test-dir MagicBlocks/build -j 1
```

### 9.4 测试用例清单
- `StackColor.FlatChangesOnlyBaseColorBytes`
- `StackColor.ByLayerUsesCellY`
- `StackColor.ByStrokeUsesStrokeId`
- `StackColor.NoGeometryRebuild`
- `StackColor.EmptyPaletteReturnsColorOutOfRange`
- `StackShapeTemplate.SquareOffsetsExact`
- `StackShapeTemplate.CircleOffsetsExact`
- `StackShapeTemplate.CrossOffsetsExact`
- `StackShapeTemplate.SphereOffsetsExact`
- `StackShapeTemplate.UnknownTemplateReturnsUnknownTemplate`
- `StackShapeTemplate.OutOfGridFailsWholeStroke`

### 9.5 失败排查
- 改色导致几何重建 → 检查是否重跑 `mesh_factory::cube()`
- 球形偏移不闭合 → 判据用了 `<` 而锚点偏移未统一；先排序再断言

---

## 10. 阶段 6 · CLI `stack` 子命令 + 离屏 PNG + golden

### 10.1 产出文件
```
MagicBlocks/src/l3_cli/main.cpp（新增 cmd_stack() + 分派 + print_usage）
MagicBlocks/CMakeLists.txt（无新增源；确认 l3-cli 已含 features/stack）
tests/magicblocks_l4_stack_test.cpp（追加 StackEndToEnd.*）
```

### 10.2 实施要点
- 子命令 `stack`，参数全集见设计 §13.1；`--output` **必填**，缺失 → usage + exit 2。
- 出图 7 步照抄 `l3_cli/main.cpp:1661-1990` 骨架：
  1. 构造文档（纯逻辑）2. 吸附+扫掠+去重 3. 创建设备 `software_desc()` 4. 离屏目标 `with_depth=true` 5. 上传+`flush_pending()`+断言归零 6. pass A + pass B 录制 + 一次 submit 7. `readback_rgba8` → `write_png_rgba8`
- 确定性：同命令行跑两次 → PNG 逐字节相同。
- `--dry-run`：只走到步骤 2，打印 `blocks/strokes/skipped_occupied/doc_hash/instance_bytes/drawcalls`，**不创建设备、不出图**，exit 0。
- ~~`--inner-depth-const 0.0` 必须配 `--allow-inner-depth-near`~~ → **该参数不存在**（D-VX-4′）。CLI **不提供** `--inner-depth-const` / `--allow-inner-depth-near`；若命令行出现，按「未知参数」usage 错误 + exit 2。

### 10.3 验收命令
```bash
ninja -C MagicBlocks/build
MagicBlocks/build/magicblocks-l3-cli.exe stack --width 320 --height 240 --layers 8 \
    --strokes 3 --cell-size 0.5 --column-height 1 --color-mode by-layer --output out.png
# 诊断辅助(非测试门判据)
# 两次运行逐字节相同
MagicBlocks/build/magicblocks-l3-cli.exe stack ... --output out2.png
fc /b out.png out2.png
ctest --test-dir MagicBlocks/build -j 1
```

### 10.4 测试用例清单
- `StackEndToEnd.CliProducesRealPng`
- `StackEndToEnd.CliIsByteIdenticalAcrossRuns`
- `StackEndToEnd.CliRejectsZeroStrokes`
- `StackEndToEnd.CliRequiresOutput`
- `StackEndToEnd.CliDryRunMatchesFullRun`
- `StackEndToEnd.CliInnerPassOffDiffersFromOn`

### 10.5 失败排查
- PNG 存在但是纯背景 → 进程内统计前景占比为 0；查 §7.5 的三条。
- 两次运行字节不同 → 有 `unordered_map` 遍历序 / 时间 / 随机混入。
- `--dry-run` 与全跑统计不一致 → dry-run 走了不同的吸附路径（应复用同一个函数）。

---

## 11. 阶段 7 · 命令流三态 + 全量回归 + 收尾

### 11.1 实施要点
- 录制：`IRecorder` 记录 F-VX 操作流（Down/Move/Up/lock/unlock/改色/撤销）。
- **`stroke_id` → `EventRecord` 映射（M10，必须写进实现）**：录制时把「一次落笔」编码为**一条** `EventRecord`，其载荷含 `stroke_id` + 该笔全部 `(px,py)` 路径点 + 颜色 + 锁定层；回放时**按记录里的 `stroke_id` 原值**重建（而非重新分配），否则 I2「单调不复用」在回放态与记录态会得到不同 id，哈希必然不等。撤销/改色各自是独立 `EventRecord`，按记录顺序重放。
- 回放：同样输入 → 同样的 `IBlockSet::hash` + 同样像素。
- 三态校验：记录态 / 回放态 / 复现态，hash + 像素逐字节相等。
- 全量回归 `ctest -j 1`，用例只增不减（≥389 + 新增）。
- 文档：更新 `MagicBlocks/docs/ARCHITECTURE.md` §1 进度表（L4 第二个玩法 F-VX）+ `L3_DELIVERY.md`。
- 门控：G1/G3/G4/G7/G8/UIG1/UIG2 全绿。

### 11.2 测试用例清单
- `StackCommandMode.RecordReplayReproduceHashEqual`
- `StackCommandMode.ReplayPixelsByteIdentical`
- `StackCommandMode.ReplayPreservesStrokeIdValues`（**M10**：回放态每笔 `stroke_id` 与记录态**逐值相等**，断言 I2「单调不复用」在回放路径下不被破坏）
- `StackGates.G7GateExitZeroAndScansNTimes`（**可测版**：直接执行 `check_l3_no_gameplay_names.py --root MagicBlocks`，断言 exit 0 且输出含「扫描 N 个文件」、N > 0。**不写**「在 primitives 里搜 `stack` 期望命中」—— `stack` 不在 G7 词表内，那种用例必然空过）
- `StackGates.NoVkInFeaturesStack`

### 11.3 收尾（`build-pipeline` ⑤，顺序不可颠倒）
1. 工作树内提交：`feat(l4-fvx): 魔法叠叠乐 ...`
2. 主工作区 `git merge --no-ff task/20261109-l4-fvx`
3. `git worktree remove` + `git branch -d`
4. **主工作区复查**：重新构建 + 重跑 `ctest -j 1`，确认 main 全绿
5. 清理临时 PNG、确认 `git status` 干净

---

## 12. 与 L3 缺口的关系

| 依赖报告的缺口 | 本计划处置 |
|---|---|
| G1 `IBlockSet` 缺失 | 阶段 1 新建（L4 内，符合层归属） |
| G2 `IBlockBrush` 缺失 | 阶段 2 新建 |
| G3 `IShapeTemplate` 缺失 | 阶段 5 新建 |
| G4 `depthConstWrite` 语义不符 | **不修后端**（D-VX-4′）；内面正确性不依赖它；建议后续补 spec 勘误（总 spec §4 P12 / L3 §6A.3） |
| G4′ `depthBounds` 特性未请求 | 同上，不依赖 |
| D7 `IPicker` 无方块实例求交 | 不需要（落笔走射线→层平面） |
| D16 离屏深度硬编码 `D16_UNORM` | 不阻塞（`vulkan_target.cpp:126`）；保持不动 |
| 设计 §10.8 / D-VX-8 的 P0-1 | **裁定不成立**（§2.B），本轮不动 L1 |

**结论：本轮到 `features/stack/` + `src/features/stack/` + `tests/` + `CMakeLists.txt` + `l3_cli/main.cpp`，L0–L3 源码零改动。**

---

## 13. 风险登记

| # | 风险 | 概率 | 影响 | 缓解 |
|---|---|---|---|---|
| R1 | 双 pass 形状不兼容导致像素异常 | 低 | 高 | `blocks`/`gizmo`/`sketch` 已同路径稳定出图（§2.B）；阶段 3 首次出图即验；异常则停工报告 |
| R2 | `depthBounds` 等**全部默认 false 特性**因 `feats{}` 全 0 而未启用（`vulkan_render_device.cpp:150,157`） | 中 | **高** | D-VX-4′：`depthConstWrite` 全面禁用、不暴露 CLI；F-VX **不得依赖** `depthBounds`/`depthClamp`/`fillModeNonSolid`/`wideLines`/`logicOp` 任意一项 |
| R3 | 内面 pass 顺序写反 → 白画 | 中 | 高 | 契约「表面在前」；专项用例 `InnerPassIsAfterSurfacePass` 反证 |
| R4 | 实例缓冲稳态触达 VMA | 低 | 中 | 阶段 6 断言 `pending_upload_bytes()==0`；`reserve_pages` 预取 |
| R5 | golden 跨平台不一致 | 中 | 中 | 渲染核心平台无关；平台差异只在软件光栅驱动（Win=SwiftShader / Linux=lavapipe）。**策略写死（M10，对齐仓库既有约定 `MagicBlocks/tests/data/README.md:15`）**：golden **单一目录、双平台共用**；**像素容差 ≤1%/每通道**（不是逐字节）；「两次运行逐字节相同」的确定性断言**仅限同平台同驱动**。**不允许**用「按平台目录隔离」放松校验 —— 那等于放弃一致性 |
| R6 | 确定性被 `unordered_map` 遍历序破坏 | 中 | 高 | 输出一律走真源下标序；`HashStableAcrossRuns` + `OutputOrderIsIndexOrderNotHashOrder` 双用例 |
| R7 | G7 误伤 `features/` 命名 | 低 | 低 | 阶段 0 已确认 G7 不扫 `features/` |
| R8 | 新测试未登记 `CMakeLists.txt` → 静默不编译 | 中 | 中 | 阶段 1 起核对用例数增量 |
| R9 | 工作树三方库缺失 | 中 | 中 | `tools/setup-env.sh` 主路径重新拉取（不回退、不复制主工作区产物） |
| R10 | ~~门禁假绿~~ **撤销（第二轮实测纠正）**：`--root` 默认即 `MagicBlocks`（`check_l3_no_gameplay_names.py:158`），两种跑法均扫 32 文件 exit 0，不存在静默假绿 | — | — | 保留「断言扫描文件数 N > 0」作为低成本自检 |
| R14 | **头文件契约与实现不符**：`pipeline.h:14-15` 注释写「叠叠乐 = depthTest=false + depthWrite=true(depthConst=near)」，而 `depthConstWrite` 实际落成未启用的 `depthBounds`（UB）；该注释还被设计 §3.4 当正面证据引用 | 中 | 中 | F-VX **不引用**该头注释立论；`depthConstWrite` 依 D-VX-4′ 禁用；建议后续补 spec 勘误（总 spec §4 P12 / L3 §6A.3） |
| R11 | 测试为断言附件形状而 include 后端私有头 → 撞 G3（`check_layer_purity.py:97` 连 `tests/` 也扫） | 中 | 中 | 用例只 include 公共头；A7 已拆为 hash + 像素两条公共面可断言用例（§7.4） |
| R12 | 「公共面 + `with_depth=true`」路径当前 0 处测试护栏 | 高 | 高 | 阶段 3 首批新增 `BothPassesRenderOnDepthTarget` 等像素回归用例 |
| R13 | CLI e2e 用例通过率依赖外部 python 脚本 | 低 | 中 | 依 M8，全部判据进程内实现，不依赖 `verify_png_content.py` |

---

## 14. 审阅门自检（提交 review 前）

- [x] CLI 模式（H1）
- [x] 离屏渲染输出图像（H2）
- [x] 平台无关一套渲染代码，无窗口/显示服务依赖（H3）
- [x] 无回退/兜底路径（H4）
- [x] 需求全覆盖（逐条对号，非笼统声明）：
  - F-VX-1 扫掠吸附堆叠 → **阶段 2**
  - F-VX-2 整笔撤销 → **阶段 1**（`IBlockSet::remove_stroke`）+ 阶段 2 的 `stroke_id` 分配
  - F-VX-3 内面渲染 → **阶段 3**
  - F-VX-4 颜色模式 → **阶段 5**
  - F-VX-5 `IShapeTemplate` → **阶段 5**
  - F-VX-6 笔刷层锁 → **阶段 2**
  - 映射权威版见设计 §0.2
- [x] 验收可度量：A1–A14 → 各阶段用例清单
- [x] 每阶段「构建 + 新增全绿 + 零回归」串行闸门
- [x] 风险已登记（R1–R14）
