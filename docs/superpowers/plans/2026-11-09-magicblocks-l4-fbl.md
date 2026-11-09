# MagicBlocks L4-F-BL 魔法积木 · 落地计划（第 1 版）

日期：2026-11-09
规格：`docs/superpowers/specs/2026-11-09-magicblocks-l4-fbl-blocks-design.md`（下称"BL 设计"）
上层规格：总 spec（`2026-11-03-magicblocks-render-engine-sdk-design.md`）、
L3 spec（`2026-11-04-magicblocks-l3-primitives-design.md`）
流程规范：`.dsh/skills/build-pipeline/SKILL.md`（审阅门 **≥90** → 执行 → 测试门全绿 → 收尾）

> **状态**：待审阅门。本版**不改任何玩法代码**（用户裁定："必须一项一项来"）。
> 唯一已落地的改动是 §2 的**地基修复**（一行级，不可避免：不做它 L4 出不了图）。

---

## 0. 一句话范围

用 L0–L3 已有能力，落地**第二个玩法 F-BL 魔法积木**：
`InputIntent` 脚本 → 格点吸附落块 / 叠放 / 选中 / 移动 / 删除 →
`IWorld` 实体镜像 → `IBatchBuilder` 实例化合批 → `IOverlay2D` 网格线与选中框 →
**无头离屏 PNG**，并跑通**命令复现**（`doc.hash()` + 逐字节一致的 PNG）。

只补 **1 个 L3 模块**（`IWorldGrid`）与 **6 个 L4 文件**，L2/L1/L0/backend 零改动。

---

## 1. 硬约束自检（build-pipeline §①，缺失即打回）

| 约束 | 本计划如何满足 | 章节 |
|---|---|---|
| **CLI 模式**（无头、可脚本化） | `magicblocks-l3-cli blocks`（既有增强）+ `blocks-sim --script/--record/--replay --verify` | §6 |
| **离屏渲染输出图像** | 全部走 `OffscreenTarget` + `readback_rgba8` → PNG；无窗口/无显示服务 | §6、§9 |
| **平台无关一套代码** | **代码层可机械校验**：`src/features/**` 与 `src/primitives/**` 零 `#ifdef`/零平台宏/零 `WIN32`·`__linux__` 分支（阶段 0 加一条 grep 门控断言）。**本轮验收基线 = Windows + SwiftShader**（本机唯一可执行环境）；`tests/data/README.md` 如实标注 Linux+lavapipe 未在本机执行 | §9 R6、阶段 0 |
| **不回退原则（D8）** | 见 §5：所有失败显式报错；无"跳过 / 降级 / 换库 / 改默认" | §5 |
| **零玩法分支进 SDK** | `world_grid.h` 不含玩法词；G3 门控 + 负向自测 | §4.0 |

---

## 2. 前置事实核对（写计划时实测，带证据）

| # | 断言 | 结论 | 证据 |
|---|---|---|---|
| F1 | 基线全绿 | ✅ **368/368**（57 套件），连跑 6 次一致 | `MagicBlocks/tools/run_tests.ps1` |
| F2 | 构建方式 | ✅ 必须走 `MagicBlocks/tools/build.ps1`（否则 LNK1181） | `tools/build.ps1` |
| F3 | 门控全绿 | ✅ G1/G3/G4/deps/scripts/UIG1/UIG2 rc=0 | `ctest` |
| F4 | **提交通路（开工时是坏的）** | ❌→✅ `OffscreenTarget` 的 private 基类让 `dynamic_cast<IFrameTargetImage*>` 恒为 null；已改 public | `src/backend/vulkan/vulkan_target.h`；`PixelRegression.SubmitCommandListActuallyWritesPixels` |
| F5 | `IWorldGrid` 现成实现 | ❌ 不存在 | 全仓 grep |
| F6 | `IBlockSet/IBlockBrush/IShapeTemplate` | ❌ 不存在（总 spec 划归 L4） | 全仓 grep |
| F7 | `IPicker` 求交能力 | ✅ AABB / 三角形 / 平面 / 球 + `pick_scene` | `primitives/picker.h:69-108` |
| F8 | `PickHit` 的 `point` / `normal` 是否够用 | ⚠️ **`point` 有真值；`normal`/`triangle` 是假值** → GAP-P1，阶段 1.5 修 L3 | `picker.h:25-45`、`picker.cpp:410-412` |
| F9 | `IGizmoSystem` 接线面 | ✅ `configure/update_target/pick_axis/on_input/build_draw/subscribe/snapshot` | `primitives/gizmo.h:126-176` |
| F10 | `IOverlay2D` 图元 | ✅ `push_line/push_dash/push_rect/push_ring/push_tri` | `primitives/overlay2d.h:136-140` |
| F11 | `IMeshFactory` 图元 | ✅ cube/box_faces/quad/grid/cylinder/cone/sphere/prism/trapezoid | `primitives/mesh_factory.h:82-160` |
| F12 | `IMeshLoader` | ✅ `load_obj_text` / `load_obj_file` | `primitives/mesh_loader.h:116-121` |
| F13 | `IBatchBuilder` + `world_render` | ✅ `build_draw_requests(world, batch, desc)` | `primitives/world_render.h` |
| F14 | `IPrimitiveRenderer` | ✅ `register_mesh/register_pipeline/set_instance_buffer/record_frame` | `primitives/primitive_renderer.h:67-98` |
| F15 | `IWorld` 标准组件 | ✅ Transform(0)/LocalBounds(1)/MeshRef(2)/MaterialRef(3)… | `primitives/world.h` |
| F16 | 出图用的管线/着色器 | ✅ `l3_scene.vert/frag`（确定性半兰伯特光照）已编进 `magicblocks_shaders.h` | `shaders/`、`l3_cli/main.cpp:225-313` |
| F17 | 玩法词门控现状 | ⚠️ G3 的 `GAMEPLAY_NAMES` 含 `blocks`，**只在 `src/backend/**` 判**；`src/features/` 合法 | `tools/check_layer_purity.py:29,129-134` |
| F18 | `check_deps_tests.py` 断言用例文件数 | ✅ **不受影响**：它数的是 `tools/tests/test_*.py`（默认 12），**不是** gtest 文件 | `tools/check_deps_tests.py:22,32-38` |
| F19 | 残留堆损坏（全量跑偶发） | ⚠️ 已定位到 private 基类并修复，连跑 6 次稳定；**根因未逐行坐实** → 列 R1 | BL 设计 §14 |
| F20 | 工作区未提交文件 | ⚠️ **119 项未提交**（清理 24 个 `*.obj` 前的 133 项已不含垃圾）；HEAD 只有 L0 + 部分 L2，`src/primitives/`、`include/magicblocks/primitives/`、`include/magicblocks/features/` **整目录未跟踪** | `git ls-tree -r HEAD`；`git status --porcelain` |
| F21 | G7 / G8 门控脚本存在但**未接进 ctest** | ⚠️ `tools/check_l3_no_gameplay_names.py`、`check_l3_command_parity.py` 存在；`CMakeLists.txt` 里 `add_test(NAME magicblocks_gate_*)` **只有 8 条**（g1/g3/g4/deps/scripts/uig1/uig2/ui_scripts） | `Select-String -Pattern "add_test\(NAME magicblocks_gate_"` 计数 = 8 |
| F22 | G7 对 `block/blocks` **有按完整标识符的白名单** | ✅ 有依据：arena 的 `blocks_` 是"内存块"，误报会逼人关掉整条规则 | `check_l3_no_gameplay_names.py:49-62` |
| F23 | `pick_scene` 的 `PickHit::normal` 是**假值** | ❌ `out->normal = Vec3{0,0,1}` 硬编码；`NarrowPhase` 只回报 `t`，物理上无法给法线 | `picker.cpp:411-412`、`picker.h:107` |
| F24 | 仓库根有 24 个误生成的编译产物 | 已清理（5.87 MB `.obj` + 空 `world.cpp`），`.gitignore` 补 `*.obj` | — |

---

## 3. 阶段划分（按依赖顺序，每阶段自带验收）

> **原则**：每阶段结束时 **构建通过 + 新增测试全绿 + 既有 368 条零回归**才算完成。
> 阶段之间不允许"先写一半后面补"；每个阶段是一个可独立评审的增量。

### 阶段 0：基线入库 + 门控补齐（0.5 天）← **在主工作区做，不在工作树内**

> **为什么单列**：`build-pipeline` §③ 规定**工作树只由主会话开一次**、专供本轮特性本体。
> 而"把 L1/L3/F-SK 的既有成果提交入库"是**历史补账**，不是 F-BL 的工作 ——
> 把它塞进工作树会让"按特性提交"的历史变成"一大坨混提交"。所以阶段 0 在 `main` 上直接做，
> 且**每一步必须 build + 全量测试全绿才允许下一步提交**（防止把红基线固化成历史）。

| # | 产出 | 验收 |
|---|---|---|
| 0.1 | **基线上线**：在 `main` 上提交既有成果 —— 但**按"可独立构建的闭包"切分，不是按层切分**。理由：`include/magicblocks/primitives/**`、`src/primitives/**`、`src/l3_cli/**`、`include/magicblocks/features/**`、`src/features/**`、`CMakeLists.txt`、`tests/magicblocks_l3_*.cpp`、`tests/magicblocks_l4_sketch_test.cpp` 之间是**互相引用的一张网**（`CMakeLists.txt` 同时引用 l3_cli 与 sketch 测试），按层切会让每个中间 commit 都构建失败。<br>**实际两刀**：<br>① `feat(baseline): L1–L3 与 F-SK 引擎底座完整入库`（上述整张网一次性提交 + `shaders/l3_*`、`shaders/overlay2d.*`、`include/magicblocks/features/**`、`src/features/**`、`tests/data/golden_l3_*.png`）<br>② `feat(f-sketch): F-SK 验收产物与 golden`（`tests/data/_out_l4_*.png` 清理后仅保留 golden + README 段）<br>③ `docs(l4): F-BL/F-VX 设计与计划` | **每一刀都在 `main` 上 `build.ps1` + `run_tests.ps1` 全绿 + `ctest -R gate` 全绿后才提交**；`git stash` 逐刀验证（先在 HEAD 上 stash 掉未提交文件，再把该刀的文件 pop 出来构建） |
| 0.2 | **补 G7/G8 进 ctest**（GAP-P2）：`add_test(NAME magicblocks_gate_g7_l3_no_gameplay_names ...)`、`magicblocks_gate_g8_l3_command_parity` | `ctest -R gate` 从 **10 项 → 12 项**全绿 |
| 0.3 | **门控负向自测**（追加到**既有** `tools/tests/test_check_scripts.py`，**不得新建 `test_*.py`** —— `check_deps_tests.py:22,35` 对 `tools/tests/test_*.py` 精确断言 = 12） | `magicblocks_gate_scripts_selftest` 绿；`check_deps_tests.py` 仍报"12 个用例文件" |
| 0.4 | 负向样本两则：①`src/backend/` 临时插 `blocks` → G3 必须红；②`include/magicblocks/primitives/` 临时插 `voxel` → G7 必须红；③反向：`src/features/blocks/` 插 `blocks` → G3 必须**绿**（证明玩法层合法） | 三条断言都在 `test_check_scripts.py` 里 |

**风险**：0.1 是本轮唯一触碰历史的一步。若某一刀在 `main` 上构建不过，说明"闭包"没切对 ——
**先补全闭包再提交**，不许把手伸进代码去"改到能过"（那是在掩盖问题），也不许调切分绕过。

---

### 阶段 1：`IWorldGrid`（L3，0.5 天）← 工作树内

| # | 产出 | 验收 |
|---|---|---|
| 1.1 | `include/magicblocks/primitives/world_grid.h`：**`Cell` / `GridSpec` / `GridQuery` / `IWorldGrid` / `create_world_grid()`（全部唯一定义在此）** | 头文件零 `Vk*`；零玩法词（挂 **G7**，不是 G3）；注释说明"按列压缩区间"与 ±16 容量 |
| 1.2 | `src/primitives/world_grid.cpp`：`set_occupied/is_occupied/find_free/occupied_count/clear` | 越界 `set_occupied` **显式 `Status::Invalid`**，不 clamp、不扩容（设计 R4） |
| 1.3 | 单测**单列文件** `tests/magicblocks_l3_world_grid_test.cpp`（纯 CPU，无设备） | 9 例全绿（8 条行为 + 1 条 `static_assert` 归属） |
| 1.4 | `features/blocks/block_types.h` **只 include 不重定义** `Cell`/`GridSpec` | `WorldGrid.CellAndGridSpecAreL3Owned` 编译通过（`static_assert(std::is_same_v<...>)`） |

| 用例 | 判据 |
|---|---|
| `WorldGrid.EmptyFindFree` | `find_free(start)` 返回 `start` 本身 |
| `WorldGrid.SkipsOccupiedColumn` | 连续占 3 格 → 返回第 4 格 |
| `WorldGrid.RespectsMaxRise` | `max_rise=2` 且占 3 格 → 返回 `false`（不返回最后一个） |
| `WorldGrid.RejectsBelowGroundAndOutOfExtent` | `cell.y < 0` 或 `|x|,|z| > half_extent` → `Status::Invalid` |
| `WorldGrid.StartInsideOccupiedStillEscapes` | 起点本身已被占 → 从起点向上找第一个空位，不返回 `start` |
| `WorldGrid.AllFullColumnFailsLoudly` | 整列占满且 `max_rise` 到顶 → `false`（不返回"最后一个格"） |
| `WorldGrid.ClearResets` / `SetFalseReleases` | `clear()` 后 `occupied_count()==0`；`set_occupied(c,false)` 后可再被命中 |
| `WorldGrid.DeterministicUnderRepeatedQueries` | 同一序列两次 → 结果逐字段一致 |
| `WorldGrid.CellAndGridSpecAreL3Owned` | `static_assert(std::is_same_v<features::blocks::Cell, primitives::Cell>)` 与 `GridSpec` 同理 —— L4 一旦重定义同名类型就**编译失败**（Q-BL-9） |

> **为什么单列测试文件**：`world_grid` 是 L3 能力，文件混进 L4 测试会让"底座错了还是玩法错了"无法区分；
> 且它是纯 CPU 数据结构，不该被设备可用性影响。

---

### 阶段 1.5：修复 `IPicker` 法线缺口（L3，GAP-P1，0.5 天）

> **这不是 F-BL 的需求，是 L3 的 bug**（设计 §6.2.1 / §11.5）：`NarrowPhase` 只回报 `t`，
> 所以 `PickHit::normal` 与 `triangle` 永远是假值。F-BL 靠法线决定落块方向，绕不过去。

| # | 产出 | 验收 |
|---|---|---|
| 1.5.1 | `picker.h`：`NarrowPhase` 改为 `bool (*)(std::uint32_t entity, const Ray& ray, PickNarrow* io)`，新增 `struct PickNarrow { float t; Vec3 normal; std::uint32_t triangle; }` | 全仓唯一调用方是 `picker.cpp` 自身（`:188/239/335`）；编译零告警 |
| 1.5.2 | `picker.cpp`：**三处**硬编码 `Vec3{0,0,1}`（`:92` / `:271` `pick_linear` / `:411` `pick_bvh`）全部改为回填真实法线；`pick_linear` 与 `pick_bvh` **结果必须逐字段一致** | 不再出现常量 `Vec3{0,0,1}` 作为命中法线 |
| 1.5.3 | 回归**两条**追加到既有 `tests/magicblocks_l3_bvh_test.cpp`：`PickerBvh.SceneHitReportsRealNormal`（16 个 AABB → 走 `pick_bvh`）、`PickerBvh.LinearPathReportsRealNormal`（3 个 AABB → 走 `pick_linear`，阈值 `kBvhThreshold=8`） | 两条断言**逐字段相等**；顶面 `normal≈(0,1,0)`、+X 面 `≈(1,0,0)`、`triangle != kNoTriangle` |
| 1.5.3b | ⚠️ **前置条件（照抄既有 helper 必红）**：既有 `picker_with`（`magicblocks_l3_bvh_test.cpp:93-99`）与 `sweep`（`:115`）**都不传 `narrow`**，`pick_linear:258`／`pick_bvh:397` 都是 `if (narrow != nullptr && !narrow(...)) continue;` —— 传 `nullptr` 时**根本不会走到填法线的分支**，`normal` 永远是默认值。所以两条新用例必须**自备回调**：<br>`bool box_face_narrow(std::uint32_t entity, const Ray& r, mbp::PickNarrow* io)` —— 对"立方体 6 个面 × 2 三角形"逐面求交，命中后填 `io->normal = 面外法线`、`io->triangle = 面索引*2+三角序号`；<br>调用面也必须改：`pick_scene(cam, px, py, &hit, &box_face_narrow)`（既有 `sweep` 无第 5 参，要新增一个 `sweep_narrow` 或在用例里直接调） | 每条用例**先断言 `hit.triangle != kNoTriangle`** —— 若忘了传回调，第一条断言就红，不会"安静地测了个假通过" |
| 1.5.4 | 既有 `PickerBvh.*` 用例**零回归**（这是改 L3 公共头签名的必付代价） | `ctest -R Picker` 全绿；全量 368 → 370 |

---

### 阶段 2：`BlockDocument`（L4 纯逻辑，1 天）← **可完全脱离设备测**

| # | 产出 | 验收 |
|---|---|---|
| 2.1 | `include/magicblocks/features/blocks/block_types.h`：**`BlockShape` / `BlockId` / `Block`** 三个类型定义在此；`Cell` / `GridSpec` **从 `primitives/world_grid.h` include 引入，不重定义、不改名**（Q-BL-9） | `static_assert(is_trivially_copyable)`；**唯一 include 是 `primitives/world_grid.h`（不是"零 L3 include"）**；`WorldGrid.CellAndGridSpecAreL3Owned` 编译通过 |
| 2.2 | `include/magicblocks/features/blocks/block_document.h` + `src/features/blocks/block_document.cpp`：BL 设计 §4.3 全接口 | — |
| 2.3 | 单测 `BlockDoc.*` ≥ 15 例 | A1 / A9 |

| 用例 | 判据 |
|---|---|
| `BlockDoc.AddReturnsIncreasingIds` | 3 次 add → id 1,2,3 |
| `BlockDoc.AddRejectsOccupiedCell` | 同格 add → 无效 id，`block_count` 不变 |
| `BlockDoc.MoveRejectsOccupiedTarget` | 目标格被占 → `false`，原位置不变 |
| `BlockDoc.RemoveAndUndoRestoresHash` | `hash` 回到删除前的值（**逐位**） |
| `BlockDoc.RedoReinsertsAtIndex` | 重做后顺序与 hash 与撤销前一致 |
| `BlockDoc.HashIgnoresHistory` | 同一个"净状态"由不同操作序列得到 → hash 相同 |
| `BlockDoc.HashDependsOnGridSpec` | 改 `cell_size` → hash 变化 |
| `BlockDoc.HashIsStableAcrossCopies` | 拷贝文档 → hash 相同 |
| `BlockDoc.ClearResetsEverything` | `block_count==0`、`next_block_id` 重置策略明确 |
| `BlockDoc.RemoveMissingIdFails` | 删不存在的 id → 返回 `false`，文档不变 |
| `BlockDoc.SetShapeOnMissingIdFails` | 改不存在 id 的外形 → `false` |
| `BlockDoc.UndoEmptyStackIsNoOp` | 空撤销栈 → `undo()` 返回 `false`（不崩、不改变 hash） |
| `BlockDoc.RedoClearedByNewEdit` | 撤销后做新编辑 → 重做栈被清空（标准 undo 语义） |
| `BlockDoc.HashChangesPerField` | 逐字段改 `shape`/`color`/`cell`/`rot_quarter` → hash 都变 |
| `BlockDoc.BlocksIterateInInsertionOrder` | 遍历顺序 = 插入顺序（hash 稳定的前提） |

---

### 阶段 3：落块与放置（1 天）

| # | 产出 | 验收 |
|---|---|---|
| 3.1 | `block_placement.h/.cpp`：`cell_floor`（带容差）、`resolve_cell_from_hit`（BL 设计 §6.3 的 5 行表）、`place_at_screen` | — |
| 3.2 | 与 `IWorldGrid` 接线：占用 → 洪水搜索 | — |
| 3.3 | 单测 `Placement.*` | A2 / A3 / A4 / A5 |

**关键技术点**：

- 命中点直接取 `PickHit::point`（阶段 1.5 已保证是真值）；法线取 `PickHit::normal`（同上）；
- 面法线判定：`|n.y| > 0.9` 视为顶/底，否则按 `n.x`/`n.z` 的符号取侧邻；
- **容差**：`eps = cell_size * 1e-4`，`floor((p - origin)/cell_size + eps_along_n)`；
- 地面以下（`cell.y < 0`）与 `|x|,|z| > half_extent` → 明确拒绝（`Status::Invalid`）。

| 用例 | 判据 |
|---|---|
| `Placement.SnapIsStable` | 同屏幕点两次 → 同一 `cell` |
| `Placement.OnCellBoundaryIsStable` | 命中点正好在格线上 → 稳定落在同一侧 |
| `Placement.StackOnTopOfBlock` | 命中顶面 → `cell.y = hit.y + 1` |
| `Placement.SideFacePutsNeighbor` | 命中 +X 面 → `cell.x = hit.x + 1` |
| `Placement.RejectsBelowGround` | 命中底面且 `hit.y==0` → 拒绝 |
| `Placement.RejectsOutsideGrid` | `|x| > half_extent` → 拒绝 |
| `Placement.NoOverlapAfterNPlacements` | 确定性种子 200 次落块，逐对 AABB 不相交（A3） |
| `Placement.FloodFindsNextFree` | 满列 → 找到最近空位（A5） |

---

### 阶段 4：出图（积木 + 网格 + overlay，1.5 天）

| # | 产出 | 验收 |
|---|---|---|
| 4.1 | `block_meshes.h/.cpp`：`BlockShape` → `MeshData`（BL 设计 §7.2 表） | A6 |
| 4.2 | `block_scene.h/.cpp`：文档 → `IWorld` 实体镜像（**差量**：只 spawn/despawn 变化的块） | `world.entity_count()` 与 `block_count()` 同步 |
| 4.3 | `block_render.h/.cpp`：一帧出图（BL 设计 §9.1 的 7 步） | 出 PNG |
| 4.4 | 网格平面绘制：`IMeshFactory::grid()` + `IOverlay2D` 格线，**缩放必须显式换算** `scale = 2*half_extent*cell_size`（`grid()` 的基准边长恒为 1，见 BL 设计 §5.2） | `BlocksE2E.GridPlaneMatchesGridSpec`：默认值下地面覆盖 `[-8, +8]`、中心在 `origin` |
| 4.5 | CLI `blocks` 子命令参数接入：`--shape/--palette/--cam-yaw/--cam-pitch/--grid-cells` | 参数非法 → 退出码 2 |

| 用例 | 判据 |
|---|---|
| `BlockMeshes.AllShapesNonEmpty` | 6 种内置外形 `vertices`/`indices` 非空，`bounds` 有限 |
| `BlockMeshes.InstancedScaleIsCellSize` | 实例矩阵缩放 = `cell_size`（读回 `InstanceAttribs` 断言） |
| `Blocks.SteadyStateZeroVma` | 60 帧同文档 → `vma_alloc_count/free_count` 增量 0（A11） |
| `Blocks.RenderProducesNonBackgroundPixels` | PNG 非背景像素 > 1% |
| `BlocksE2E.CliIsByteIdenticalAcrossRuns` | 两次出图逐字节一致（A10） |

---

### 阶段 5：交互会话 + gizmo + 脚本回放（1.5 天）← **命令复现的最小闭环**

| # | 产出 | 验收 |
|---|---|---|
| 5.1 | `block_session.h/.cpp`：`InputIntent` + 显式状态机（BL 设计 §8.2） | A7 |
| 5.2 | gizmo 接线：`IGizmoListener` 在 F-BL 侧把事件翻译成 `move`；**拖拽期间不写文档**（Q-BL-5） | A8 |
| 5.3 | CLI `blocks-sim`：`--script/--record/--replay --verify`，支持 `expect doc_hash` | A12 |
| 5.4 | 脚本行格式写进 `tests/data/README.md` | — |

| 用例 | 判据 |
|---|---|
| `Session.TapSelectsAndSwitches` | 选中 → 点其他块 → 切换（**不清选中**） |
| `Session.TapSameBlockStacks` | **Q-BL-10**：已选中块上再 Tap → 在它上面叠一块、`selected` 仍是它（BL-2 的唯一入口） |
| `Session.TapEmptyWhileSelectedPlaces` | 选中态点空地 → `block_count + 1` **且** `selected()` 变为无效（Q-BL-4：先落块再清选中） |
| `Session.TapEmptyWhileSelectedUsesHitPoint` | 同一屏幕点在 Idle 与 Selected 态下落的 `cell` **完全相同**（证明两条路径共用 `commit_place_at`，没有"取消选中就落错地方"） |
| `Session.PlaceFailureKeepsSelection` | 选中态点越界/超 `max_rise` → 落块失败、**选中保留**、无副作用 |
| `Session.DeleteClearsSelection` | 删除后 `selected()` 无效 |
| `Session.DragSnapsToCell` | 拖到半格 → 文档里是整格 |
| `Session.DragDoesNotWriteUntilEnd` | `DragMove` 多次 → `doc.hash()` 不变（Q-BL-5） |
| `BlocksE2E.ScriptReplayReproducesDoc` | 脚本里每条 `expect doc_hash` 全对 + 最终 PNG 一致 |
| `BlocksE2E.RecordReplayVerifyMatches` | `--record` → `--replay --verify` 通过 |
| `BlocksE2E.BadScriptFailsLoudly` | 未知指令 / hash 不符 → 退出码非 0 且**不写 PNG** |

---

### 阶段 6：golden + ASan + 文档 + 收尾（0.5 天）

| # | 产出 | 验收 |
|---|---|---|
| 6.1 | `tests/data/golden_blocks.png`（640×400，固定脚本） | `check_png_content.py` rc=0 |
| 6.2 | 结构化断言 `BlocksE2E.MatchesGoldenStructure`（**不只比字节**：断言"选中块的包围盒中心在画面里"、"至少 2 种颜色出现"） | A13 |
| 6.3 | `tests/data/README.md` 补 golden_blocks 段：怎么重生成、断言什么、**生成环境声明**（Windows + SwiftShader；Linux+lavapipe 本轮未在本机执行，不宣称一致） | 文档与 R6 口径一致 |
| 6.4 | **ASan 全量（A15，主路径，不是兜底）**：`build-asan` 构建 → 全量跑 → 退出码 0 且输出无 `AddressSanitizer`/`LeakSanitizer` 字样 | R1 的机械判据 |
| 6.5 | `MagicBlocks/docs/L4_DELIVERY.md`（新）+ `ARCHITECTURE.md` §1 进度表更新 L4 行 | 文档与代码一致 |
| 6.6 | 全量：ctest 全绿（368+新增）、**连跑 ≥6 次零失败**、`ctest -R gate` **12 项**全绿 | 附录数字 |

---

## 4. 文件清单（本轮新增/修改）

### 4.0 新增（14 个）

```
include/magicblocks/primitives/world_grid.h          ← L3：Cell / GridSpec / GridQuery / IWorldGrid（★唯一一处定义）
src/primitives/world_grid.cpp
tests/magicblocks_l3_world_grid_test.cpp             ← L3 单列测试（纯 CPU）

include/magicblocks/features/blocks/block_types.h    ← L4：BlockShape/BlockId/Block（**只 include world_grid.h 取 Cell/GridSpec，不重定义**）
include/magicblocks/features/blocks/block_document.h
include/magicblocks/features/blocks/block_placement.h
include/magicblocks/features/blocks/block_meshes.h   ← 内含 kMaxBlockMeshes = 8（F-BL 自己定义的上限）
include/magicblocks/features/blocks/block_scene.h
include/magicblocks/features/blocks/block_session.h
include/magicblocks/features/blocks/block_render.h

src/features/blocks/*.cpp                            （对应 6 个实现）
tests/magicblocks_l4_blocks_test.cpp
tests/data/golden_blocks_script.txt                  ← 脚本 golden（含实测 doc_hash，阶段 6 入库）
```

**类型唯一归属（Q-BL-9，逐条可核）**：

| 类型 | 唯一定义 | 谁用 |
|---|---|---|
| `Cell` | `include/magicblocks/primitives/world_grid.h` | `IWorldGrid` / `BlockDocument` |
| `GridSpec` | 同上 | `IWorldGrid` / `BlockDocument`（持副本并算进 hash） |
| `GridQuery` | 同上 | `IWorldGrid::find_free` |
| `BlockShape` / `BlockId` / `Block` | `features/blocks/block_types.h` | 仅有 L4 |
| `kMaxBlockMeshes` | `features/blocks/block_meshes.h` | 仅有 L4（**不新增 L3 常量**） |

**分层归属铁律**（review 明确要求写进计划，不能只留在推导里）：

| 内容 | 放哪 | 判据 |
|---|---|---|
| 通用格点查询 | `include/magicblocks/primitives/` + `src/primitives/` | 受 **G7** 约束：零玩法名 |
| 玩法（落块/选中/文档/出图） | `include/magicblocks/features/blocks/` + `src/features/blocks/` | 受 **G3** 约束：只禁 `vkCmd`/`CmdDraw`，玩法名**合法** |
| **禁止** | 在 `src/features/blocks/` 里 `#include` 任何 `Vk*` 或直写 GPU 命令 | G3 `:104,134` |

### 4.1 修改（6 个）

| 文件 | 改动 | 风险 |
|---|---|---|
| `CMakeLists.txt` | 加 8 个源文件、注册 2 个新测试文件、**补 G7/G8 两条 gate ctest** | 低 |
| `include/magicblocks/primitives/picker.h` | **GAP-P1**：`NarrowPhase` 签名 + `PickNarrow` | 中：全仓唯一调用方是 `pick_scene` 自身（已核） |
| `src/primitives/picker.cpp` | **GAP-P1**：填写真实 `normal`/`triangle` | 中：既有 `PickerBvh.*` 用例必须全绿 |
| `src/l3_cli/main.cpp` | 新参数 + `blocks-sim` 子命令 | 中：**不得改变** `blocks` 既有默认行为 |
| `tests/data/README.md` | 补 golden_blocks 段 + 环境声明 | 无 |
| `MagicBlocks/docs/ARCHITECTURE.md` | L4 进度行 | 无 |

### 4.2 **明确不改**

`src/backend/**`、`src/scene/**`、`src/ui/**`、`src/primitives/` 除 `world_grid` 与 GAP-P1 的 `picker` 外的既有文件、
`src/core/**`、`shaders/**`（沿用 `l3_scene`）、L0/L1/L2 公共头。

> **结构性重排禁令**：本轮不得为了 F-BL 做任何目录级搬动/重命名
> （`include/magicblocks/**` 的既有文件一律原地不动）。

---

## 5. 失败语义清单（D8：全部显式报错，**无任何兜底**）

| 情形 | 返回 | 谁处理 |
|---|---|---|
| 落点在可落区外 / 地面以下 | `Status::Invalid` | CLI 退出码 1；会话里忽略该次意图 + 记一条诊断（**选中保留**，见设计 §8.2 细则 4） |
| 目标格被占用且洪水超 `max_rise` | `Status::Unavailable` | 同上 |
| `set_occupied` 越出 `±half_extent` | `Status::Invalid`（**不 clamp、不扩容**） | 同上 |
| 未知 `BlockShape` / `shape_variant` 越界 | `Status::Invalid` | 同上 |
| 外形种数超过 `kMaxBlockMeshes`（**F-BL 自己定义** = 8：6 种内置 + 2 个 OBJ 槽位） | `Status::Invalid` + 明确报出上限 | 同上；**不回退共享网格**。注意：L3 的 `types.h:248-249` 只有 `MeshId`/`kInvalidMeshId`，**没有** `kMaxMeshIds` —— 上限归 F-BL 定义，不新增 L3 常量（否则就是 §3.1 纪律下的第三项 L3 改动） |
| OBJ 导入失败 | `MeshLoadResult.status` 非 Ok → CLI 退出码 1 | **不回退成 Cube** |
| 脚本未知指令 / `expect` 不符 | 退出码 1，**不写 PNG** | — |
| 网格为 0 块时出图 | **允许**（渲染空场景 + 网格平面），CLI 打印 `blocks=0` | 空积木场景是合法中间态 |

**参数校验口径（与既有 `l2_cli` 一致，`CMakeLists.txt:541-543`）**：

| 输入 | 退出码 |
|---|---|
| `--help` | 0 |
| 未知参数（`--bogus 1`） | **2** |
| 参数值非法（`--width 0`、`--shape teapot`、`--grid-cells 0`、`--show-grid 7`） | **2** |
| 缺必需参数（`--output` / `--script`） | **2** |
| 文件读不到（`--obj` / `--script` / `--replay` 路径不存在） | **1**（并打印具体路径） |
| 脚本语法错 / `expect` 不符 / `--verify` 不一致 | **1** |
| 渲染或设备失败 | **1** |

---

## 6. CLI 契约

```
magicblocks-l3-cli blocks [--width W] [--height H] [--blocks N] [--seed S]
                          [--shape cube|brick|cylinder|sphere|prism|trapezoid]
                          [--palette R,G,B] [--obj PATH]
                          [--cam-yaw DEG] [--cam-pitch DEG] [--grid-cells K]
                          [--show-grid 0|1] --output PNG

magicblocks-l3-cli blocks-sim --width W --height H --script FILE
                          [--record BIN | --replay BIN --verify]
                          [--frame N] --output PNG
```

- 全部参数**完全推导**画面，不读时钟/鼠标/文件系统状态（OBJ 路径除外，且只在启动期读一次）；
- 同参数两次 → **PNG 逐字节一致**；
- `--replay --verify` 校验命令流哈希 + 文档哈希；
- 输出行必须含：`blocks=<N> doc_hash=<16hex> mesh_hash=<16hex> draws=<K> submit=1`。

### 6.1 脚本原语（`blocks-sim --script`）

```
# 注释
tap X Y                       # 屏幕坐标点击（命中块 → 选中；命中空地 → 落块）
tap-empty X Y                 # ⭐ 选中态下点空地（Q-BL-4）：在屏幕射线的命中处落块，再清选中
set-shape cube|brick|cylinder|sphere|prism|trapezoid
set-color R G B
drag-begin X Y / drag-move X Y / drag-end
delete
undo / redo
expect blocks N               # 块数
expect selected N             # 选中块 id（0 = 无）
expect doc_hash <16hex>
```

### 6.2 Q-BL-4 的逐行脚本示例（**交付物里不允许留占位符**）

**两步契约**（避免"跑完再抄 hash"这种假 golden）：

1. **阶段 5** 先提供 `blocks-sim --script <file> --print-state`：不带 `expect doc_hash` 也能跑，
   跑完把 `blocks/selected/doc_hash` 打到 stdout；
2. **阶段 6** 由人工/脚本把实测值写进 `tests/data/golden_blocks_script.txt`（**含真实 16 位 hash**），
   该文件进仓库、由 CTest `magicblocks_blocks_sim_golden_script` 跑；
   并在文件头注释写明：**"该 hash 由 `blocks-sim` 在本机 Windows+SwiftShader 生成；
   改任何吸附容差 / 相机默认值 / 网格尺寸都必须重生成"**。

脚本内容（`tests/data/golden_blocks_script.txt`，**入库时最后一行必须是实测的 16 位 hex**）：

```
# 场景：先放一块 → 选中它 → 它已选中时再点它（Q-BL-10：叠一块）→ 选中态点空地（Q-BL-4：落块并清选中）
# 校验依据：BL 设计 §8.2 的 Q-BL-10 优先级表 + Q-BL-4 用户裁定

tap 320 300
expect blocks 1
expect selected 0

tap 320 300            # Idle 态命中块 → 选中（Q-BL-10 第一行）
expect blocks 1
expect selected 1

tap 320 300            # 同一块已选中 → 在它上面叠一块（Q-BL-10 第三行 / BL-2）
expect blocks 2
expect selected 1      # 叠完仍选中原块

tap-empty 420 260      # 选中态点空地 → 落块 + 清选中（Q-BL-4）
expect blocks 3
expect selected 0

expect doc_hash <Hex16>   # 阶段 6 用 --print-state 实测值替换；入库前必须是真实 16 位 hex
```

> **为什么第 3 步的 `tap` 和第 2 步一样却期望不同**：这正是 Q-BL-10 的裁决点 ——
> 同一个屏幕坐标，因为**状态不同**（Idle vs Selected 同一块），语义不同。
> 这三条 `expect` 合起来就是 BL-2/BL-3/Q-BL-4/Q-BL-10 四条语义的**完整机械判据**。

既有 `blocks` / `sketch` 子命令的默认行为**不得改变**，回归用具体 CTest 名核对：

```powershell
ctest -R "magicblocks_cli_blocks|magicblocks_cli_sketch" --output-on-failure
```

---

## 7. 测试与门控统计（目标）

| 项 | 目标 | 当前 |
|---|---|---|
| 新增 gtest 用例 | **56**（逐阶段相加，见下表；8+1+2+15+8+7+8+5+2） | 0 |
| 既有用例回归 | 368 → **424**（368 + 56）且 **0 跳过** | 368 |
| 门控项数 | **10**（原 8 条 gate + 2 条 UI 自测）+ **2**（G7/G8）= **12** | 10 |
| `tools/tests/test_*.py` | **保持 12**（负向自测**追加**到 `test_check_scripts.py`，不新建文件） | 12 |
| 全量连跑 | ≥ 6 次，0 失败 | — |
| ASan 全量 | 1 次，退出码 0 且无 sanitizer 报告 | 未跑 |

**逐阶段用例数（可直接相加核对 = 56）**：

| 阶段 | 套件 | 条数 |
|---|---|---|
| 1 | `WorldGrid.*`（8 条：EmptyFindFree / SkipsOccupiedColumn / RespectsMaxRise / RejectsBelowGroundAndOutOfExtent / StartInsideOccupiedStillEscapes / AllFullColumnFailsLoudly / ClearResetsAndFalseReleases / DeterministicUnderRepeatedQueries） | **8** |
| 1 | `WorldGrid.CellAndGridSpecAreL3Owned`（`static_assert` 归属校验，Q-BL-9） | **1** |
| 1.5 | `PickerBvh.SceneHitReportsRealNormal` + `PickerBvh.LinearPathReportsRealNormal` | **2** |
| 2 | `BlockDoc.*`（9 条原有 + 6 条补齐） | **15** |
| 3 | `Placement.*`（8 条） | **8** |
| 4 | `BlockMeshes.AllShapesNonEmpty` / `BlockMeshes.ShapeCountWithinEnumBounds` / `BlockMeshes.InstanceScaleIsCellSize` / `Blocks.SteadyStateZeroVma` / `Blocks.SteadyStateZeroHeapAllocs`（§10 的 `kMaxHeapAllocs=0`）/ `Blocks.BatchPoolSlotsReused`（`IBatchPool` 槽位复用）/ `Blocks.RenderProducesNonBackgroundPixels` | **7** |
| 5 | `Session.*`（TapSelectsAndSwitches / TapSameBlockStacks [Q-BL-10] / TapEmptyWhileSelectedPlaces / TapEmptyWhileSelectedUsesHitPoint / PlaceFailureKeepsSelection / DeleteClearsSelection / DragSnapsToCell / DragDoesNotWriteUntilEnd） | **8** |
| 5 | `BlocksE2E.*`（CliIsByteIdenticalAcrossRuns / ScriptReplayReproducesDoc / RecordReplayVerifyMatches / BadScriptFailsLoudly / GridPlaneMatchesGridSpec） | **5** |
| 6 | `BlocksE2E.*`（MatchesGoldenStructure / GoldenScriptPasses） | **2** |
| | **合计** | **56**（8+1+2+15+8+7+8+5+2） |

> 阶段 4 的 `Blocks.SteadyStateZeroHeapAllocs` 与 `Blocks.BatchPoolSlotsReused` 是对设计 §10
> 三项承诺（`vma_alloc_count` 增量 0、`kMaxHeapAllocs=0`、槽位复用）的逐项机械判据 ——
> 只测 VMA 一项等于把另外两项留给"看起来没泄漏"。

---

## 8. 时间与顺序（人工可核）

| 阶段 | 依赖 | 估时 | 验收门槛 | 位置 |
|---|---|---|---|---|
| 0 基线入库 + G7/G8 接入 | — | 0.5 d | 每刀 build+全量绿；gate 8→**10** 条直测 + 2 条自测 = **12 项** | **主工作区** |
| 1 `IWorldGrid` | 0 | 0.5 d | 8 例绿（独立测试文件） | 工作树 |
| 1.5 `IPicker` GAP-P1 | 1 | 0.5 d | `SceneHitReportsRealNormal` 绿 | 工作树 |
| 2 `BlockDocument` | 1 | 1 d | 15 例绿 | 工作树 |
| 3 落块/放置 | 1.5, 2 | 1 d | 8 例绿 | 工作树 |
| 4 出图 | 2, 3 | 1.5 d | PNG + 稳态零 VMA | 工作树 |
| 5 会话/gizmo/脚本 | 3, 4 | 1.5 d | 复现闭环 + Q-BL-4 脚本 | 工作树 |
| 6 golden/ASan/文档 | 4, 5 | 0.5 d | 全量绿 ×6 + ASan 无报错 | 工作树 |

---

## 9. 风险（与 BL 设计 §12 对齐，**已消除全部兜底措辞**）

| # | 风险 | 处置 |
|---|---|---|
| R1 | 全量偶发堆损坏 | **主路径跑 ASan**（阶段 6.4，A15）：退出码 0 且无 `AddressSanitizer`/`LeakSanitizer` 输出；另加连跑 ≥6 次 |
| R2 | ~~`PickHit` 缺命中点~~ → **GAP-P1 法线是假值** | **阶段 1.5 修 L3**，不绕开（§11.5） |
| R3 | gizmo 拖拽与整格吸附冲突 | Q-BL-5：松手才提交 |
| R4 | `IWorldGrid` 稀疏退化 | 容量先定死 `half_extent = 16`，越界**显式 `Status::Invalid`**；更大场景需求另起 spec |
| R5 | OBJ 让 draw 次数增长 | 首轮只允许**一个**导入外形槽位；超限**报错**（不回退共享网格） |
| R6 | golden 跨平台 | **Windows + SwiftShader 是本轮唯一验收基线**（本机只有这个环境）；Linux+lavapipe 本轮**不宣称**一致，`tests/data/README.md` 里如实写"未在本机执行"，不留"待验证"这种悬空结论 |
| R7 | 玩法词泄漏 | **G3**（`src/backend/**` 出现 `blocks` → 违规）+ **G7**（`src/primitives/**` 出现 `voxel`/`积木` → 违规；`block/blocks` 标识符按既有白名单放行）+ 负向自测（追加进 `test_check_scripts.py`） |
| R8 | `blocks` / `sketch` CLI 默认行为被改动 | 阶段 4 结束前必跑 `ctest -R "magicblocks_cli_blocks|magicblocks_cli_sketch"`，并对照 `git diff src/l3_cli/main.cpp` 确认只新增分支 |

---

## 附录 A：基线数字（阶段 0 完成后**由执行 AGENT 回填并提交**）

| 项 | 值 |
|---|---|
| ctest | 368/368（2026-11-09 实测，连跑 6 次一致） |
| 门控项数 | 10（阶段 0 后应为 **12**：+G7 +G8） |
| 门控 rc | G1/G3/G4/deps/scripts/UIG1/UIG2/UI-scripts 全 0 |
| `check_deps_tests --expect-files` | **12**（`tools/tests/test_*.py`；gtest 文件不计入，F18 已确认） |
| 基线提交切分 | 待阶段 0.1 完成后填（每个 commit 的 hash + 该 commit 的测试结果） |

> **封闭要求**：本附录在阶段 0 完成后由执行 AGENT 把上表全部填成实测值并提交；
> 不允许留"待确认"。设计 §1 的基线表与本附录冲突时，**以本附录（实测）为准**。

## 附录 B：已完成的开工前修复

| 改动 | 文件 | 效果 |
|---|---|---|
| `IFrameTargetImage` 基类 private → public | `src/backend/vulkan/vulkan_target.h` | 提交通路可用；5 个原失败用例转绿，368/368 |
| 清理仓库根误生成的编译产物 | 24 个 `*.obj` + 空 `world.cpp` | 5.87 MB 垃圾出仓库；`.gitignore` 补 `*.obj` |
