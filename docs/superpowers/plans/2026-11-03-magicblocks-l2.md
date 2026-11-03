# MagicBlocks L2（scene / 渲染图 / 命令模式）落地计划 · 第 3 版

日期：2026-11-03 · 规格：`docs/superpowers/specs/2026-11-03-magicblocks-render-engine-sdk-design.md`
（§2、§4 L2 S1–S9、§5 命令模式与离屏、§6.3 句柄耦合、§7 门控、§8、§11、§13 第 2 阶段）
前置：`docs/superpowers/plans/2026-11-03-magicblocks-l0.md`（L0 已落地，ctest 32/32）
与 `docs/superpowers/plans/2026-11-03-magicblocks-l1.md`（L1 已落地，**现有全量 ctest 99/99，0 失败 0 跳过**）

> **审阅历史**
> - 第 1 版：加权 **75.9/100 → 打回**（架构 73 / 需求 72 / 可行性 78 / 可测性 76 / 健壮性 78）。
>   审阅报告：`docs/superpowers/plans/2026-11-03-magicblocks-l2-review.md`，共 21 条修改建议（A1-A3 / B4-B8 /
>   C9-C11 / D12-D16 / E17-E21），每条附实测 `文件:行号` 证据。
> - **第 2 版：加权 87.6/100 → 通过**（架构 90 / 需求 91 / 可行性 85 / 可测性 84 / 健壮性 86）。
>   独立核验：v1 的 21 条中 **18 条真落实、4 条半落实、0 条假落实**；v1 三个阻断项全部真解决。
>   同时给出 **Q1/Q2 两个裁决** 与 **N1-N8 八个新问题**（全部局部级、无阻断）。
> - **第 3 版（本版）：在通过稿基础上吸收 Q1/Q2 + N1-N8**，不留任何"实施时二选一"或未定义行为，
>   逐条见 §14。**本版无需再走审阅门**（只收敛上一轮已认可方向上的细节）。

## 0. 本轮范围

### 0.1 一句话范围

只落 **L2 `scene/`**：SDK 公共头 `magicblocks/scene/*`（`IScene` / `ISubmitQueue` / `IRenderGraph` /
`CommandStream` 帧录制 / `IRecorder` 三态 / 序列化 / 命令哈希 / `IRenderFeature` 注册表），
外加无头 CLI `magicblocks-l2-cli` 在**离屏目标**上走**L2 自己的渲染图**录制 → 回放 → 复现并落盘 PNG。

L3 primitives / L4 features **不做**；**L0/L1 的公共接口一行不改**（第 2 版的硬约束，见 §5.1）。

### 0.2 本轮要闭合的硬约束（逐条对应 spec）

| 约束 | 在本轮的落地 | 判定方式 |
|---|---|---|
| **S1** `IScene` | 节点树 + TRS + 包围盒/包围球 + 可见性；`NodeId` 单调不复用 | `Scene.*` 6 条 |
| **S2** `ISubmitQueue` | 不透明(材质→深度) / 透明(相机距离 远→近)；**相机位置是 `sort()` 入参**；tie-break = `tie_break_id` | `SubmitQueue.*` 5 条 |
| **S3** `IRenderGraph` | pass 声明 + `AttachmentRef`(外部目标 / 上游 pass 输出) + 读写图 + **"先离屏、再贴图"** | `RenderGraph.*` 4 条 |
| **S4** 帧命令录制 | **L2 自己双写**：命令既录进 `ICommandList`，又同步进 `CommandStream`（不读 L1 录制流） | `Stream.*` 9 条 |
| **S5** 三态 `IRecorder` | `Recording` / `Replaying` / `Reproduce`，状态机非法转移即报错 | `Recorder.*` 8 条 |
| **S6** 序列化 | 二进制（magic+版本）+ JSON（键序固定、可 diff）；save/load | `Stream.*` 中 4 条 |
| **S7** 确定性 | 命令哈希 = 规范化字节流的 FNV-1a 64（§4.5 位格式写死） | `Stream.Hash*` 2 条 |
| **S8** 帧自包含 | `frame_index` + 输入事件时间戳 + **完整 `SceneSnapshot` 本体**入流 | `Stream.FrameIsSelfContained` + `Recorder.ReproduceFromCaptureSnapshot` |
| **S9** `IRenderFeature` 注册表 | 特性按稳定注册序进渲染图；SDK 内部零玩法名 | `FeatureRegistry.*` 3 条 + G3 门控 |
| **C4/G1** 无 GS | L2 不新增 shader stage | G1 门控（`PipelineDesc.stages` 结构上已禁） |
| **G3** 分层纯度 | `include/magicblocks/scene/**`、`src/scene/**`、`src/l2_cli/**` 零 `Vk*` | G3 门控 + 负向自测样本 |
| **G4** 内存路径唯一 | L2 不直接申请显存：只用 `IRenderDevice::create_*`；命令流只记 `ResourceId`（打包成 u64） | G4 门控 |
| **G5** 单队列单线程 | 整帧**一次** `submit_and_wait()`，且在 `CommandStream` 组装完成**之后** | `Recorder.OneSubmitPerFrame` |
| **M5** 稳态零 VMA | L2 每帧不新建 GPU 资源；**暖机 3 帧 + 差分 30 帧**（L1 同口径） | `Recorder.SteadyStateZeroVmaCalls` |
| **离屏 + CLI** | `magicblocks-l2-cli` 无窗口、无显示服务 | CTest 6 项 |
| **跨版本** | 版本不匹配 → `Unsupported`，message 带两个版本号 | `Stream.RejectsVersionMismatch` |
| **不回退** | 全部错误路径显式报错；**没有任何"默认实现兜底"** | 见 §5.4 |

## 1. 关键事实核对（全部实测，带 `文件:行号`）

| # | 断言 | 实测结论 | 证据 |
|---|---|---|---|
| 1 | 现有基线全绿 | ✅ `ctest` **99/99**（0 失败 0 跳过）；G1/G3/G4 rc=0；G5 `ran=191 failed=0 skipped=0` | 本轮开工前实跑 |
| 2 | **`submit_and_wait()` 会清空录制流** | ✅ **软件态与设备态都清**：`recorded_.clear(); command_count_ = 0;` | `src/backend/vulkan/vulkan_command.cpp:587-588`（软件态）、`626-627`（设备态） |
| 3 | → 因此「提交后读回命令流」不可行 | ✅ 结论 2 的直接推论；「先读再提交」又违 G5 | 结论 2 |
| 4 | 软件态命令列表可无设备录制 | ✅ 无 ctx 时 `init()` 置 `recording_ = true`，校验与计数照常 | `vulkan_command.cpp:76-89` |
| 5 | `CommandOp` 已定号 0..10 | ✅ 与 R3 的 11 类命令一一对应 | `vulkan_command.h:47-59` |
| 6 | `set_resource()` 每次都调 `pack()` | ✅ 失败置 `packed_ok=false` 但**不阻塞录制** | `vulkan_command.cpp:144-150` |
| 7 | `pack()` **丢弃 `size`** | ✅ `unpack()` 后 `size == 0`，size 靠资源表按 id 反查 | `include/magicblocks/resources/handles.h:61-64, 81-90` |
| 8 | `IPipeline::hash()` 不含 Vk 句柄 | ✅ 由 `PipelineDesc` 逐字段混合得出，**跨进程稳定** | `vulkan_pipeline.cpp:178-249` |
| 9 | `RenderPassDesc` **没有 name 字段** | ✅ 只有 `target/clear_color/clear_color_rgba/clear_depth/clear_depth_value/area_*` | `include/magicblocks/resources/command.h:44-56` |
| 10 | 设备态 `draw` 需要真 `VulkanPipeline` | ⚠️ `bind_pipeline` 认不出实现类型 → `Unsupported` | `vulkan_command.cpp:167-179` |
| 11 | `IRenderTarget` 无稳定标识 | ❌ 只有 kind/extent/format/samples/附件视图/readback | `include/magicblocks/device/render_target.h:24-39` |
| 12 | L1 `ITexture` **不是渲染附件** | ✅ rulings 明文列为遗留约束 | `2026-11-03-magicblocks-l1-rulings.md:33-35` |
| 13 | UI 桩 `IScene` 只有 4 个方法 | ✅ `snapshot/changes/clear_changes/restore` | `include/magicblocks/ui/scene_stub.h:66-80` |
| 14 | `FakeScene` 继承 `ui::stub::IScene` | ✅ → 转发成 `using` 别名会 C2259 | `tests/ui_test.cpp:121-136` |
| 15 | G3 对**非 backend** 文件扫 `Vk*` | ✅ `include/src/tests/tools/shaders` 全部扫，`src/scene/`、`src/l2_cli/` 都会被覆盖 | `tools/check_layer_purity.py:88-116` |
| 16 | `src/cli/main.cpp` include 了 backend 头却 G3 仍绿 | ✅ **是正则漏洞**：`VK_INCLUDE` 只认 `vulkan\|volk\|vulkan_beta` 开头，`backend/...` 不匹配 | `src/cli/main.cpp:21`；`check_layer_purity.py:18` |
| 17 | `check_deps_tests.py` **精确断言 12 个用例文件** | ✅ `--expect-files 12`，`len(files) != 12` 直接失败 → **加门控脚本必须同步改这个数** | `tools/check_deps_tests.py:22, 35-38` |
| 18 | CMake 只注入 3 个路径常量 | ✅ `MB_GOLDEN_DIR` / `MB_CLI_PATH` / `MB_UI_CLI_PATH`，**无 `MB_SRC_DIR`** | `CMakeLists.txt:236-239` |
| 19 | `RecordsAllOps` 断言 `command_count() == 11` | ✅ 且注释写明"每多一条命令就要在 L2 命令流里多一个 op 编号" | `tests/magicblocks_l1_test.cpp:602`；`command.h:7-9` |
| 20 | L1 稳态测试口径 = 暖机 3 帧 + 差分 30 帧 | ✅ | `tests/magicblocks_l1_test.cpp:355-360` |
| 21 | UI golden 用容差 0 的 `count_differing_pixels` | ✅ | `tests/ui_test.cpp:1058-1066` |
| 22 | `draw_selftest_grid()` 自建 pass/pipeline、直接 `vkCmdDraw`、**绕过 `ICommandList`** | ✅ 所以它不能证明 L2 的任何约束 | `src/backend/vulkan/vulkan_draw.cpp:28-224`；`ARCHITECTURE.md` §7 |
| 23 | 命名空间含 `blocks` 会被 G3 误判 | ✅ `magicblocks` 被豁免，独立 `blocks` 才违规 | `tools/check_layer_purity.py:29-34` |

## 2. 产物布局（只列新增/修改）与命名空间

```
MagicBlocks/
  include/magicblocks/scene/          # ← 新增：L2 公共面（零 Vk*）
    scene.h          NodeId / Node / Transform / Aabb / Sphere / SceneSnapshot / IScene
    submit_queue.h   SubmitItem{world_position,...} / SortKey / SortPolicy / ISubmitQueue
    render_graph.h   AttachmentRef / PassDesc / IRenderGraph / IRenderFeature / 注册表
    command_stream.h CommandRecord / PassRecord / CommandStream / command_stream_hash
    frame_capture.h  FrameCapture（binary/JSON/文件）/ FrameMeta
    recorder.h       IRecorder + 三态 + FeatureRecordContext + PipelineTable
  src/scene/                          # ← 新增：L2 实现（零 Vk*，namespace magicblocks::scene）
    scene_impl.{h,cpp}
    submit_queue.cpp
    render_graph.cpp
    command_stream.{h,cpp}      # ByteWriter/ByteReader + 规范化 + 哈希
    frame_capture.{h,cpp}       # 二进制编解码 + JSON
    recorder.cpp                # 三态状态机 + PipelineTable
    feature_registry.cpp
  src/l2_cli/main.cpp                 # ← 新增：magicblocks-l2-cli（只 include <magicblocks/...> + core/image_io.h）
  tests/magicblocks_l2_test.cpp       # ← 新增：L2 用例（见 §6.1，共 35 条）
  tests/data/golden_l2.png            # ← 新增：L2 golden（SwiftShader 产出）
  CMakeLists.txt                      # ← 改：源文件 + CLI + CTest + 新门控
  README.md / docs/ARCHITECTURE.md    # ← 改：L2 段落与进度表
tools/
  check_scene_no_clock.py             # ← 新增：D13 的门控（src/scene + include/magicblocks/scene 无时钟）
  test_check_scripts.py               # ← 改：加 G3 负向样本 + 无时钟门控正/反例（**不新增 test_*.py**，--expect-files 保持 12）
  tests/test_check_scripts.py         # ← 改：加 G3 负向样本 + 无时钟门控正/反例自测
```

### 2.1 命名空间写死（第 1 版最硬的结构漏洞，A2）

**`src/scene/**` 与 `include/magicblocks/scene/**` 一律在 `namespace magicblocks::scene`。**

- **不许**用 `magicblocks::backend`：`check_layer_purity.py:110-116` 对「非 `src/backend/` 的文件」
  扫 `Vk*` / `PFN_vk*`，backend 前缀的文件会被判违规（第 1 版没写命名空间，等于计划对自己的产物违反 G3）。
- 该名不含玩法词（`magicblocks` 被门控豁免，`scene` 不在玩法名表里），G3 的玩法名扫描安全。

### 2.2 `src/l2_cli/` 的 G3 口径（A3）

`src/l2_cli/main.cpp` **会被 G3 扫到**（`SCAN_SUBDIRS` 含 `src`）。硬规则：

- **只允许** include `"magicblocks/..."` 公共头 + `"core/image_io.h"`（PNG 写盘，无 Vk）。
- **禁止** include 任何 `backend/...` 头。第 1 版的「不构成先例」判断依据：
  `src/cli/main.cpp:21` 之所以没被 G3 抓到，只是因为 `VK_INCLUDE` 正则
  （`check_layer_purity.py:18`）只认 `vulkan|volk|vulkan_beta` 开头，`backend/vulkan/vulkan_target.h`
  不匹配 —— **这是正则漏洞，不是许可**。L2 新代码不依赖漏洞。
- 出图链路：`IRenderTarget::readback_rgba8()` → `magicblocks::write_png_rgba8()`，
  **完全不碰** `draw_selftest_grid()`（`vulkan_draw.cpp`，ARCHITECTURE §7 明文"别当渲染图起点"）。

## 3. S1–S9 功能点 → 接口 → 实现 → 用例（唯一事实来源）

| 功能点 | 接口（公共头） | 实现 | 用例（§6.1） |
|---|---|---|---|
| **S1** 节点树 / TRS / 包围盒 / 可见性 / 稳定 ID | `scene.h`：`IScene::create_node(name, parent)`→`NodeId`；`set_transform` / `set_visible` / `set_local_bounds`；`node(id)`；`world_matrix(id)`；`world_aabb(id)` / `world_sphere(id)`；`snapshot()` / `restore()` | `scene_impl.cpp` | `Scene.NodeIdIsStableAcrossMutate` · `Scene.WorldBoundsIncludeChildren` · `Scene.InvisibleNodesAreSkipped` · `Scene.RestoreRoundTrips` · `Scene.RestoreRejectsBadSnapshot` · `Scene.SnapshotIsIdOrdered` |
| **S2** 提交队列排序（相机距离 + 稳定 tie-break） | `submit_queue.h`：`SubmitItem{node_id, pass, layer, material_key, depth_key, world_position, tie_break_id}`；`sort(policy, camera_pos)` **在排序期**算距离；只读视图 `items()` | `submit_queue.cpp` | `SubmitQueue.OpaqueSortedByMaterialThenDepth` · `SubmitQueue.TransparentSortedBackToFront` · `SubmitQueue.DistanceKeyUsesCamera` · `SubmitQueue.TieBreakIsDrawItemId` · `SubmitQueue.SortIsDeterministic` |
| **S3** pass 声明 + 附件依赖 + 读写图 + **先离屏再贴图** | `render_graph.h`：`AttachmentRef{Source::ExternalTarget \| PassOutput}`；`PassDesc{name, layer, channel, color_attachment, load_op, clear_color, depth, writes_color_texture, reads_textures}`；`IRenderGraph::add_pass` / `validate()` / `passes()` | `render_graph.cpp` | `RenderGraph.PassesAreInDeclarationOrder` · `RenderGraph.ValidateIsIdempotent` · `RenderGraph.PassOutputFeedsNextPassAsInput` · `RenderGraph.RejectsReadBeforeWrite` |
| **S4** 整帧命令流（**L2 双写**） | `command_stream.h`：`CommandStream{meta, scene_snapshot, passes[], commands[]}`；`CommandRecord` 与 L1 `RecordedCommand` **字段一一对应**但**只存 `packed_resource`（u64）** | `command_stream.cpp` | `Stream.RecordsWholeFrame` · `Stream.CommandOpsCoverAllEleven` |
| **S5** 三态 `IRecorder` | `recorder.h`：`state()`；`begin_frame(frame_index, events)`；`record(graph, scene, queue, ICommandList* target)`；`replay(stream, target, ICommandList*)`；`reproduce(stream, IScene&, ReproduceFn)`；`capture()` | `recorder.cpp` | `Recorder.*` 8 条 |
| **S6** 序列化 | `frame_capture.h`：`encode_binary` / `decode_binary` / `to_json` / `from_json` / `save` / `load` | `frame_capture.cpp` | `Stream.BinaryRoundTripIsByteIdentical` · `Stream.JsonRoundTripMatchesBinary` · `Stream.JsonDumpIsStable` · `Stream.RejectsVersionMismatch` · `Stream.RejectsTruncatedInput` |
| **S7** 命令哈希 | `command_stream.h`：`command_stream_hash(stream)`（§4.5 定点规则） | `command_stream.cpp` | `Stream.HashIsStableAcrossRuns` · `Stream.HashChangesOnAnyField` |
| **S8** 帧自包含 | `CommandStream::meta`：`format_version` / `engine_version` / `backend` / `frame_index` / `extent` / `input_events[]`；`CommandStream::scene_snapshot`（**完整节点本体**） | `frame_capture.cpp` | `Stream.FrameIsSelfContained` · `Recorder.ReproduceFromCaptureSnapshot` |
| **S9** 特性注册表 | `render_graph.h`：`IRenderFeature{layer, record(FeatureRecordContext&)}` + `IRenderFeatureRegistry::add/remove/features()` | `feature_registry.cpp` | `FeatureRegistry.RecordsInRegistrationOrder` · `FeatureRegistry.RemoveKeepsOrder` · `FeatureRegistry.RecordsIntoGraphPass` |

## 4. 核心设计

### 4.1 依赖方向（机械保证）

```
src/scene/**   → include/magicblocks/{scene,resources,device}/**   ✅ 允许
src/scene/**   → src/backend/**                                    ❌ 禁止（G3 判 Vk*）
include/magicblocks/scene/** → magicblocks/{device,resources}/** + <cstdint>/<string>/<vector>/<functional>/<memory>
```

L2 只经 `IRenderDevice::create_*` 与 `ICommandList*` 拿资源，**看不见任何后端类型**。

### 4.2 `IScene`（S1）

- **NodeId 稳定**：`u64` 单调递增、**不复用**（删除只置失效位）。理由：UI 层 `IStateStore` 以 NodeId 为键，
  复用会让"退出重进"恢复到错误节点。
- **变换**：`Transform{translation[3], rotation(quat)[4], scale[3]}`（与 UI 桩逐字段一致）；
  世界矩阵 = `parent.world × T·R·S`；四元数**不归一化写回**（保证 snapshot 字节稳定）。
- **包围盒**：`local_aabb` + 8 角点变换后的 AABB（含子节点）；`world_sphere` = AABB 中心 + 半径
  （取 scale 各轴最大值）。**明确取舍**：不做旋转紧球——L2 阶段的消费者（Culling/拾取）用 AABB 足够，
  紧球只影响效率不影响正确性。
- **可见性**：`visible=false` 的节点与**子树**都不进提交队列。
- **快照/恢复**：`SceneSnapshot` 扁平数组（与 UI 桩同构），**按 NodeId 升序**（`Scene.SnapshotIsIdOrdered`）；
  `restore()` **先全量校验后应用**（环 / 悬空 parent / id 冲突 → 返回 false 且场景**不变**）。

### 4.3 `ISubmitQueue`（S2）—— 相机距离是 `sort()` 的入参（B6）

```cpp
struct SubmitItem {
  u64 node_id = 0;
  u32 pass = 0;
  SubmitLayer layer = SubmitLayer::Opaque;   // Opaque=0 / Transparent=1
  u64 material_key = 0;      // L3 的 IMaterial 定义取值，L2 只按它排序
  u32 depth_key = 0;         // 提交方给的 0..N 深度槽（供不透明前向化）
  float world_position[3] = {0,0,0};   // ← 距离的**数据来源**
  u64 tie_break_id = 0;      // 稳定 tie-break（建议 = NodeId 或 draw item 序号）
};
Status sort(SortPolicy policy, const float camera_pos[3]);
```

- **距离在 `sort()` 里现算**（不是 `push()` 时算死，提交方不可能知道相机位置）：
  `distance_key = quantize(sqrt(dx²+dy²+dz²) × 1024)` → `u32`（定点，**不参与序列化**，只在排序期存在）。
  量化理由：浮点在某些 x87/FMA 路径下可能折叠出差异，量化成全序整数后比较是确定的。
- 排序键（字典序）：**Opaque** = `(pass ↑, material_key ↑, depth_key ↑, tie_break_id ↑)`；
  **Transparent** = `(pass ↑, distance_key ↓, tie_break_id ↑)`。
- 实现用 `std::stable_sort` + 显式比较器；同键相对序由插入序兜底，所以结果唯一。

### 4.4 `IRenderGraph`（S3）—— "先离屏、再贴图"（B4/B5）

```cpp
struct AttachmentRef {
  enum class Source : std::uint8_t { ExternalTarget = 0, PassOutput = 1 };
  Source source = Source::ExternalTarget;
  device::IRenderTarget* external = nullptr;  // Source::ExternalTarget 时必填
  std::uint32_t producer_pass = 0;            // Source::PassOutput 时必填（声明序下标）
};

struct PassDesc {
  std::string name;
  RenderFeatureLayer layer = RenderFeatureLayer::Scene;
  std::uint8_t channel = 0;
  AttachmentRef color_attachment{};
  device::IRenderTarget* own_target = nullptr;   // ← N1：本 pass 的"产物目标"
  LoadOp load_op = LoadOp::Clear;
  float clear_color[4] = {0, 0, 0, 1};
  bool has_depth = true;
  float depth_clear_value = 1.0f;
  std::vector<std::uint32_t> reads_pass_outputs;  // 本 pass 读取哪些上游 pass 的输出
};
```

- **N1 解析入口（关键）**：`PassOutput` **不能只有下标** —— 使用者必须能拿到目标去 `begin_pass` 和读回。
  规则：**每个 pass 都声明 `own_target`**（它写到哪里），`PassOutput` 解析 =
  `graph.passes()[producer_pass].own_target`。
  - `own_target == nullptr` → 表示"写到**当前帧的外部目标**"（即 `IRecorder::record()` 传入的 target），
    这正是最后一个 pass 的常见情形。
  - 提供 `Result<device::IRenderTarget*> resolve(AttachmentRef) const`：
    解析成功返回目标指针；`producer_pass` 越界/下标 ≥ 当前 pass → `Invalid`。
  - `Recorder::record()` 用 `resolve()` 拿到目标再调 `ICommandList::begin_pass(RenderPassDesc{target, ...})`，
    **不自己猜**。
- **方案甲（采纳）**：`PassOutput` 指向**上游 pass 的 `IRenderTarget*`**（L2 自己用
  `IRenderDevice::create_offscreen_target` 建的中间目标）。**不用** L1 的 `ITexture` ——
  rulings 遗留约束（`ITexture` 不是渲染附件）继续生效，**不引入 `PageKind::DeviceTexture`**（超出本轮）。
- `validate()` 规则（**在 `record()` 之前统一跑**，不是录制中撞错）：
  1. `PassOutput.producer_pass` 必须 **< 当前 pass 下标**（否则 `Invalid`，message 带 pass 名与下标）；
  2. `PassOutput` 解析出的目标**必须非 nullptr**（`ExternalTarget` 的 `external` 为空、或 `PassOutput`
     解析到空目标 → `Invalid`）。**中间目标"合法但未登记"由这里拦**（Q1 裁决：不在 `record()` 里靠撞
     L1 的 `target == nullptr → Invalid`，那是不可控的偶发失败）；
  3. `reads_pass_outputs` 里引用的上游输出必须存在且 < 当前下标（否则 `Invalid`）。

### 4.4.1 公共头的类型口径（N7）

**L2 公共头只用 `std::uint8_t` / `std::uint32_t` / `std::uint64_t` / `std::int32_t` / `float` / `std::size_t`**，
**不复用** `device::u32/u64` 之外的别名，**更不复用** `ui::f32`：

- `include/magicblocks/ui/types.h` 有 `f32`，但 L2 → ui 是**反向依赖**（ui 消费 SDK，不是 SDK 依赖 ui），
  复用会造成层序颠倒。
- L2 写 `float` 而不用 `u8`/`f32`：仓库里 `u8` 根本不存在（`device/types.h` 只有 `u32/u64/i32`），
  v1/v2 草案里的 `u8` 是笔误。
- 例外：`ResourceId` / `Format` / `MemoryKind` 等**直接复用 L1 类型**（值语义，非别名）。

### 4.5 `CommandStream` 位格式（S4/S6/S7/S8，C9/C10/C11/B7/B8）

**总规则：只经 `ByteWriter` 逐字段写小端；`float` 走 `std::bit_cast<std::uint32_t>` 后再 `put_u32`；
禁止把任何整个结构体 `memcpy` 进流。** 解析端 `ByteReader` 同名对称，越界即 `Invalid`。
`u8` = `std::uint8_t`，`u16/u32/u64` = `std::uint16_t/uint32_t/uint64_t`，`i32/i64` = 有符号对应类型。

**头 12 字节（N8 修正：每字节区间相加后必须 == 12）**：

```
[0..4)    magic "MBL2"          4B
[4..6)    format_version u16    2B   = 1
[6..8)    reserved u16          2B   = 0
[8)       backend u8            1B
[9)       engine_major u8       1B
[10..12)  reserved u16          2B   = 0
          ── 合计 12B ──（v2 曾把 [10) / [11..12) 写成区间，错 1 字节；解析端从 [12) 读 frame_index）
```

```
[12..20)  frame_index u64
[20..28)  extent.width u32, extent.height u32
[28..36)  event_count u64
          × event: kind u8, pointer_id u32, x float, y float, dx float, dy float,
                   button i32, width u32, height u32, timestamp_us i64, handled u8
[..)      node_count u64                                   ← S8 的完整初始场景（B7）
          × node（**按 id 升序**）:
             id u64, parent u64,
             name_len u32（**UTF-8 字节数**，不 trim、不规范化）+ name bytes,
             translation[3] float, rotation[4] float, scale[3] float,
             local_aabb min[3] float + max[3] float, visible u8
[..)      pass_count u64
          × pass: name_len u32 + name bytes（UTF-8 字节数）, layer u8, channel u8,
                  load_op u8, color_source u8, producer_pass u32,
                  color_format u8（Format 枚举值，固定）, has_depth u8, depth_clear float,
                  clear_color[4] float, area[4] u32
[..)      command_count u64
          × command: op u8, packed_resource u64（0 = 不适用）,
                     pipeline_hash u64, a u32, b u32, c u32, i i32,
                     f[7] float（未用槽位必须写 0）, pass_area[4] u32,
                     width u32, height u32,
                     payload_len u32 + payload bytes
[..)      stream_hash u64（= 前面全部字节的 FNV-1a，读入端必须复算比对）
```

**三条裁决（第 1 版悬空的点）**：

| 裁决 | 结论 | 理由 |
|---|---|---|
| **资源身份写什么** | **只写 `packed_resource`（u64）**，不写完整 `ResourceId` | `pack()` 已含 page/offset/id；`size` 靠资源表按 id 反查（`handles.h:61-64` 的原始设计）。同时写两份会让流里出现可互相矛盾的信息 |
| **`packed_ok == false` 时** | **录制期直接失败**（`record()` 返回 `Invalid`，message 带 `page_id/offset`） | 现状 L1 允许 `packed_ok=false` 继续提交，但命令流**没有** `packed_ok` 字段——写进去就是静默丢信息，违反"不回退" |
| **pass name 来源** | 来自 **L2 的 `scene::PassDesc.name`**，由 L2 翻译成 `RenderPassDesc` 调 `begin_pass` | `RenderPassDesc`（`command.h:44-56`）**没有 name 字段**且不改 L1 签名（C11） |

**命令哈希（S7）**：`stream_hash` = 对 `[0, 末尾-8)` 的全部字节做 FNV-1a 64
（初值 `0xcbf29ce484222325`，质数 `0x100000001b3`）。
`initial_scene_hash` 由**场景快照编码段**单独求同款哈希（供 `reproduce()` 校验"同一初始场景"）。
`-0.0f` 与 `0.0f` 位模式不同 → 哈希不同（有意：流里就是不同字节）。

**JSON（S6/B8）**：键序固定为
`format_version, engine_version, backend, frame_index, extent, input_events, scene_snapshot, passes, commands, hash`；
浮点 `%.9g`；`to_json` 连续两次必须**字节完全相同**（`Stream.JsonDumpIsStable`）。

### 4.6 `IRecorder` 三态（S5）+ 双写（A1/C9）

```
                begin_frame(frame_index, events)
   Idle ─────────────────────────────────► Recording ──capture()──► Idle
     ├──replay(stream, target, cmd_list)──► Replaying ──► Idle（哈希必须 == stream_hash）
     └──reproduce(stream, scene, fn)──────► Reproduce ──► Idle（哈希必须 == stream_hash）
```

- **Recording = L2 双写**：`record()` 拿 `ICommandList* target`，**每一条命令都由 L2 发起**：
  L2 调 `target->begin_pass(...)`／`draw(...)` 的同时，把同一条命令追加进 `CommandStream`。
  **不读 L1 的录制流**（`recorded()` 是 backend 私有，且提交时会清空——事实 2/3）。
  于是 **L1 公共接口一行不改**，裁定 1 的"11 个命令方法"结构保证不受影响（C9/A1）。

#### 4.6.1 双写发散的四条铁律（Q2 裁决，第 3 版新增）

双写天然有"两条通道可能不一致"的风险（L1 有前置校验：未绑管线就 draw、未进 pass 就 draw 等，
`vulkan_command.cpp:152-158`；L2 自身也可能拒收）。**规则写死，不留未定义行为**：

| # | 规则 | 具体 |
|---|---|---|
| **W1** | **写序固定：先校验 → 调 L1 → 成功才 append** | 每条命令的三步顺序不可调换。**L1 是唯一权威**：L1 返回非 Ok，则 `record()` 立刻返回该 Status，**不 append**。 |
| **W2** | **`packed_ok` 预检在调 L1 之前**（Q1 裁决） | 因为 `pack()` 是 const 纯函数，L2 在调 `target->bind_*()` **之前**先 `pack()` 一次：失败 → 直接 `Invalid`（message 带 `page_id/offset/size`），**一条都不录**。这样绝不会出现"L1 记了 N 条、流里 M 条"的部分成功态。 |
| **W3** | **帧内失败即整帧作废** | `record()` 任一步失败：状态机立即回 `Idle`，`CommandStream` 丢弃（半成品由下一次 `begin_frame()` 清空），**禁止**随后 `submit_and_wait()`（调用它会拿到 `Invalid`，而不是把半截命令提交上 GPU）。 |
| **W4** | **`commands[]` 含 `begin_pass`/`end_pass`** | 与 L1 的 `command_count()` 口径一致（`command.h:136-138` 明文 11 类都计数）。因此得到**强不变量**：`stream.commands.size() == target->command_count()` —— 做成用例 `Recorder.StreamMatchesCommandListCount`。 |

#### 4.6.1.1 侦察裁决 R9/R10（U1/U2，第 3 版新增，附实测证据）

`MagicBlocks/docs/L2_RECON.md` 开工侦察提出两条**计划必须明确**、否则会写出"看着绿、其实没验证"的用例的事实。
Lead 已独立复核，两条均**成立**，裁决如下。

| 编号 | 事实（已复核） | 裁决 |
|---|---|---|
| **U1** | `ICommandList::draw*` **不发 `vkCmdDraw`**：`vulkan_command.cpp:230-245,247-262,264-279` 只 `append(cmd)`；全仓 `vkCmdDraw` 仅出现在 `vulkan_draw.cpp:202`、`vulkan_ui_overlay.cpp:495`，**都不在命令列表里**。`begin_pass` 设备态**会**发 `vkCmdBeginRenderPass`（`:482`），`bind_pipeline`/`push_constants`/`viewport`/`scissor` 也真发（`:178,313,347,368`）。 | **回放"落盘"的本轮语义 = 命令流还原 + 哈希比对，不是"重放到 GPU 出图"。** 计划 §4.7 原来隐含"replay 会出图"是错的，**改**：`replay --verify` 比对 **命令哈希 + 目标读回**，其中读回由**录制时**的 `submit_and_wait()` 产出；`replay` 子命令**只重录不重提**（重录进一张软件态/新列表后 `command_count()` 与哈希比对）。L2 golden 的像素来自**录制**那次真实提交，**不是**回放。这一条写进 §10「明确不做」，并在 §6.5 增加结构化断言旁证。 |
| **U2** | 录制流里 `pass_area` **已丢失"0=整图"语义**：`vulkan_command.cpp:391-392` 把 `area_width==0` 解析成 `extent.width` 再入流。 | L2 的 `PassRecord` **必须另存原始 `area_width/area_height`（含 0）**，不采用 L1 解析后的值，否则 `BinaryRoundTripIsByteIdentical` 会把 0 抹成 extent 宽度而破坏"声明即所得"。§4.5 的 `area[4] u32` 明确指**原始调用值**。 |

- **提交时机**：`CommandStream` 组装完成（且全部命令都 return Ok）→ 才 `target->submit_and_wait()`（每帧 1 次，G5）。
- **Replaying**：只把流里的命令**逐条重录**进一张**新的** `ICommandList`，再比对哈希。
  `bind_pipeline` 只有 `pipeline_hash` → 查 `PipelineTable`（§5.2）。
- **Reproduce**：从流里**读 `scene_snapshot` 重建场景**（`Recorder.ReproduceFromCaptureSnapshot`），
  回放事件流，跑 `ReproduceFn`，得到新流，与录制流比特级比对。
- **不读时钟**：三态都不读任何时钟；时间戳原样从流里取（S8）。由门控 `check_scene_no_clock.py` 机械保证（D13）。

#### 4.6.2 管线生命周期（N5：`PipelineTable` 的填充者）

v2 漏了"谁建管线、谁登记"这一环。第 3 版明确：

```
IRecorder::on_attach(IRenderDevice& device, const Resources& resources)
    │  ① 建 L2 演示/渲染所需 IPipeline（经 device.create_pipeline）
    │  ② 逐个登记进 PipelineTable（键 = pipeline->hash()）
    │  ③ 建顶点缓冲（create_buffer，建一次）
    └─ 之后每帧 record() 直接用，不新建任何 GPU 资源（M5 的来源）
```

- `PipelineTable` **由 `IRecorder` 持有**（不是全局、不是静态），`on_attach` 是唯一填充入口。
- `record()` 里 L2 自己发起的 `bind_pipeline` 用 `table.add()` 过的管线，天然满足"每个 hash 都在表里"。
- **设备态回放的合法前提**（§5.2）：`replay()` 之前调用方必须已 `on_attach` 过（同一 recorder 或同设备的新 recorder），
  否则缺 hash → `Unsupported`（硬失败，带 hash 与已登记列表）。
- L2 **不提供** `create_pipeline` 的新入口，**只调用** `IRenderDevice::create_pipeline`（L1 既有）——
  这样 L2 不会变成第二条资源创建路径（G4）。

### 4.7 L2 自己的出图通路（**不是** L0 自检绘制的复用）

**关键区分**：`draw_selftest_grid()`（事实 22）自己建 render pass / pipeline、直接 `vkCmdDraw`、
**绕过 `ICommandList`**，因此**不能**证明 L2 的任何约束。L2 的出图必须走这条**真实通路**：

```
src/l2_cli 内置演示场景（若干 AABB 节点 + 一个相机位置）
   ├─► IScene::create_node / set_transform / set_local_bounds            (S1)
   ├─► IRenderGraph::add_pass(pass0 清屏+不透明, pass1 叠加)             (S3)
   ├─► IRenderFeature 注册"演示绘制特性"→ 每帧写 ISubmitQueue            (S9)
   ├─► ISubmitQueue::sort(policy, camera_pos)                            (S2)
   ├─► IRecorder::record(graph, scene, queue, cmd_list) → CommandStream   (S4/S5)
   ├─► cmd_list->submit_and_wait()（每帧仅 1 次）                         (G5)
   └─► IRenderTarget::readback_rgba8() → write_png_rgba8()              (C6)
```

回放/复现走同一链的**末端**：`replay(stream, target, cmd_list)` 重录进新列表 → 提交 → 读回 → PNG，
与录制 PNG 逐像素比对。**整条链没有任何一处调用 `draw_selftest_grid()`。**

> **为什么 L2 只画屏幕空间色块/线条**：`IMeshFactory` / `ICamera` 属 **L3**（spec §13 第 3 阶段）。
> L2 的演示特性用顶点数据（`IRenderDevice::create_buffer` **建一次**，不进每帧分配）画屏幕空间图元，
> 足以让 S1–S9 全链路真实跑通并被像素验证，同时不越 L3 边界。L3 落地后只替换这个特性，L2 其余部分零改动——
> 这正是 S9 插件式设计要证明的。

### 4.8 离屏 + CLI（C6 / spec §5.4）

| 子命令 | 参数 | 退出码 |
|---|---|---|
| `record` | `--scene demo --width --height --frame-index --events N --camera X,Y,Z --record cap.bin --output out.png` | 0/1/2/3 |
| `replay` | `--replay cap.bin --output out.png --verify` | 0/1/2 |
| `reproduce` | `--replay cap.bin --output out.png --verify` | 0/1/2 |
| `dump` | `--replay cap.bin --json` | 0/1/2 |
| `device` | — | 0/1/3 |

退出码沿用 L0 约定：`0` 成功 · `1` 运行失败 · `2` 参数错误 · `3` 需提权才能让 loader 看到 SwiftShader。
全部路径**不需要窗口/显示服务**。

**`--scene` 的行为（D16 裁决）**：spec §5.4 的 `--scene sketch|blocks|voxel|stamp` 是**全量 CLI 的最终形状**，
四个 preset 属 **L4**（spec §13 第 5 阶段），且**玩法名在 SDK 内部被 G3 明令禁止**。
所以 L2 阶段：**接受 `--scene demo`（唯一取值）**；遇到任何其它取值（含四个玩法名）→ **rc=2**，
stderr 打印「preset 属 L4（spec §13 第 5 阶段），L2 仅支持 `--scene demo`」。
这不构成降级：§5.4 的 CLI 目的是"离屏可脚本化出图"，`--scene demo` 已完整证明该通路。

## 5. 与 L0/L1 的边界（第 2 版的硬约束）

### 5.1 不改 L1 公共接口（A1/E21）

第 1 版拟加的 `ICommandList::recorded_commands()` **删除**。理由（事实 2/3）：
`submit_and_wait()` 成功即 `recorded_.clear()`，「提交后读录制流」恒空；「先读后提交」违 G5。
唯一自洽解就是 §4.6 的**双写**。副产物：`Command.RecordsAllOps == 11` 的口径**完全不受影响**。

**凡是"默认实现 X / 后端覆盖之"的写法一律删除**，公共接口一律纯虚或干脆不提供（E21）。

### 5.2 `PipelineTable`（设备态回放的前提，C10/E20）

```cpp
class PipelineTable {          // src/scene/recorder.cpp；公共头里只声明
 public:
  Status add(resources::IPipeline* pipeline);      // 以 pipeline->hash() 为键
  resources::IPipeline* find(u64 hash) const;
  std::size_t size() const;
  static constexpr u32 kMaxListedOnMiss = 8;
};
```

- `IPipeline::hash()` 不含 Vk 句柄（事实 8）→ 用户经 `IRenderDevice::create_pipeline` 拿到的管线可登记。
- **设备态回放的合法前提**：流里引用的**每个** `pipeline_hash` 都必须在 `PipelineTable` 里。
- **找不到 = 硬失败**（`Unsupported`）：message 带 **缺失 hash 的 16 进制** + **本进程已登记的 hash 列表（最多 8 个）**。
- **明确不做**（写进 §10）：ui 覆盖层用的后端私有管线（不在 `IPipeline*` 体系里）**不进** L2 的图；
  L2 的 CLI 回放**只用自己建的管线**，不混 ui 覆盖层。将来要让 ui 覆盖层混进来，需要 ui 层改用 L1 `IPipeline`。

### 5.3 `ui::stub` 的处置（**不转发**，E17/g）

**裁决：`ui::stub::IScene` 保持原样，不转发、不改名。** 依据：桩的 `IScene` 是
`snapshot/changes/clear_changes/restore` 四个方法（事实 13），与 L2 的 12 个方法**完全不同集**；
转成 `using` 别名会让 `tests/ui_test.cpp:121` 的 `FakeScene` 抛 **C2259**（事实 14），且
`FeatureContext.scene`（`scene_stub.h:116-120`）的类型语义也会漂移。

因此本轮：

- `include/magicblocks/ui/scene_stub.h` **一行不改**；`ui::stub::IRenderFeature::record()`（无参）
  与 L2 的 `scene::IRenderFeature::record(FeatureRecordContext&)` **并存**，两套 `RenderFeatureLayer`
  枚举也**并存**（各自在自己的命名空间里，不重名）。
- `ui_test.cpp` 的 `FakeScene` **不改**；`ui_Offscreen.MatchesGolden` 与
  `ui_Router.ExitSavesStateAndReenterRestores` 必须**逐像素/逐字段不变**。
- 对接点写进 §10「明确不做」：**ui 层接入 L2 场景**是后续独立任务（需要适配器 + 迁移 12 方法），
  本轮不在范围内，**这样 UI 用例零风险**。

#### 5.3.1 侦察对照：为什么不采纳"逐名 `using` 转发"（第 3 版补充说明）

侦察报告 §6.3 指出：`scene_stub.h` 只被 `ui/router.h:23` 一处 include，`router.h:30-38` 已把 10 个名字
`using stub::X;` 导入 `magicblocks::ui`，因此把 `scene_stub.h` 改成 **逐名 `using scene::X;`**
在**语法上**只需要改一处，`router.h` 与 `ui_test.cpp` 可以不动。

Lead 复核后**确认这个机械成本判断成立**（`router.h:30-38` 确实是纯别名块），**但仍维持"不转发"**，
理由从"改不动"改为"不该在本层改"：

| 理由 | 说明 |
|---|---|
| **1. 转发的语义前提不成立** | 转发会让 `ui::stub::IScene` 变成 `scene::IScene` 的**同一类型**，而后者有 12 个方法；`ui_test.cpp:121` 的 `FakeScene` 只实现 4 个 → **C2259 抽象类无法实例化**。要让转发成立，必须**同时**改 `ui_test.cpp`（4 → 12 方法），而 `ui_test.cpp` 是当前 **99/99 的既有基线**，且 `ui_Offscreen.MatchesGolden` 容差 0。**这已不是"一行不改"**，侦察 §6.3 的"一行都不用改"隐含假设了 `FakeScene` 能编译过。 |
| **2. 两条签名冲突是真实的不兼容** | 侦察 §6.3 自己列出三处冲突：桩 `record()` **无参** vs 计划 `record(FeatureRecordContext&)`；`FeatureContext.display` 是 `ui::DisplayInfo`（`scene_stub.h:119` ← `ui/types.h:92`），转发会让 **scene 反向依赖 UI**（违反 spec §2.1）；`ui::stub` 里还住着 L3 的 `IOverlay2D`（`ui/overlay.h`），不能整体换命名空间。**这三处每一处都要改 UI 公共面 + UI 测试**。 |
| **3. 本轮目标不含 UI 集成** | L2 阶段交付的是 `scene::` 层自身可用 + CLI 离屏出图。UI 接入需要"适配器 + 迁移 12 方法 + 改 FakeScene + 重新采 `golden_ui.png`"，是一个**独立工作量**，塞进 L2 只会让 99/99 基线冒无谓风险。 |

> **结论**：转发是**后续 UI 集成任务的正确方向**（届时连同 `FakeScene`、`DisplayInfo`、`IOverlay2D` 一起改），
> 但**不是 L2 的改动**。本轮 `scene_stub.h` 零改动，`ui::stub` 与 `scene` 两套接口**并存**。
> 这条已记入 §10「明确不做」，并在 §14 标记为 **R11**。


## 6. 测试设计（测试门：0 失败 0 跳过）

### 6.1 L2 gtest —— **38 条**（D12/D15/D14；N2 修正：标题/汇总/套件三处口径已对齐）

| 套件 | 用例 | 断言要点 |
|---|---|---|
| `Scene`（6） | `NodeIdIsStableAcrossMutate` | add/remove/add：旧 id 不复活；新 id ≠ 任何旧 id |
| | `WorldBoundsIncludeChildren` | 子偏移后父 AABB 包住子；scale 生效 |
| | `InvisibleNodesAreSkipped` | `visible=false` 的节点与子树都不进提交视图 |
| | `RestoreRoundTrips` | snapshot → 清空 → restore → snapshot 逐字段相等 |
| | `RestoreRejectsBadSnapshot` | 环/悬空 parent/id 冲突 → false 且场景不变 |
| | `SnapshotIsIdOrdered` | 乱序增删后 `snapshot()` 仍按 id 升序 |
| `SubmitQueue`（5） | `OpaqueSortedByMaterialThenDepth` | 手搓 key 断言输出顺序 |
| | `TransparentSortedBackToFront` | 量化后 远→近 |
| | `DistanceKeyUsesCamera` | **同一队列两处相机 → 透明顺序相反**（B6） |
| | `TieBreakIsDrawItemId` | 全键相等按 `tie_break_id` 升序（与插入序相反也正确） |
| | `SortIsDeterministic` | 两次 `sort` 结果逐字节相同 |
| `RenderGraph`（4） | `PassesAreInDeclarationOrder` | `passes()` 顺序 == 声明序 |
| | `ValidateIsIdempotent` | 连续两次 validate 都 Ok，不改状态 |
| | `PassOutputFeedsNextPassAsInput` | **A 的输出作 B 的输入 → validate Ok**（B4/B5） |
| | `RejectsReadBeforeWrite` | 读未写/引用不存在 producer/前向引用/**未登记目标** → `Invalid` + 指明 pass |
| `Stream`（**10**） | `RecordsWholeFrame` | 2 pass × N 命令 → pass/command 计数正确 |
| | `CommandOpsCoverAllEleven` | 11 类 op 全覆盖（不依赖 L1 私有头） |
| | `BinaryRoundTripIsByteIdentical` | encode→decode→encode 字节完全一致 |
| | `JsonRoundTripMatchesBinary` | `from_json(to_json(s))` 哈希 == `s` 哈希 |
| | `JsonDumpIsStable` | 两次 `to_json` **字节相同** + 键集合与顺序固定（B8） |
| | `RejectsVersionMismatch` | 改版本号 → `Unsupported`，message 含两个版本 |
| | `RejectsTruncatedInput` | 截断/magic 坏/尾部哈希不符 → `Invalid`（不返回半截） |
| | `HashIsStableAcrossRuns` | 固定输入 → 常量 hash（防实现漂移） |
| | `HashChangesOnAnyField` | 逐字段变异（24+ 字段）全部改变 hash（含 `-0.0` vs `0.0`） |
| | `FrameIsSelfContained` | meta 含 frame_index + 事件时间戳；**含完整节点本体**（B7/D14） |
| `Recorder`（**9**） | `RecordThenReplayMatchesHash` | 录制 hash == 回放 hash |
| | `ReproduceMatchesHash` | 复现 hash == 录制 hash |
| | `ReproduceFromCaptureSnapshot` | **从流里读快照重建**，读出场景与录制时逐字段相等（B7） |
| | `IllegalTransitionIsRejected` | Idle 下 `capture()`/Recording 下 `replay()` → `Invalid` |
| | `OneSubmitPerFrame` | 每帧 `submit_count()` 增量 == 1（G5） |
| | `ReplayMissingPipelineIsUnsupported` | 流引用未登记管线 → `Unsupported`，message 含 hash 十六进制（C10/E20） |
| | `RejectsUnpackableResource` | `packed_ok=false` → `record()` 返回 `Invalid`（§4.5 裁决） |
| | `SteadyStateZeroVmaCalls` | **设备态**（`require_sw_device()`；无设备 `ADD_FAILURE` + `return`，**不 skip**）**暖机 3 帧 + 差分 30 帧**：`vma_alloc/free` 增量 **0**，且 `allocation_count` 增量 > 0（防假绿，E19）。软件态列表从不碰分配器（`vulkan_command.cpp:581-582`），故本条**必须设备态**（N4） |
| `FeatureRegistry`（3） | `RecordsInRegistrationOrder` | 3 个特性按注册序录制 |
| | `RemoveKeepsOrder` | 移除中间项后其余相对序不变 |
| | `RecordsIntoGraphPass` | 特性被录进其 `layer` 对应的 pass（S9 的"注册进渲染图"） |
| `L2Golden`（1） | `MatchesL2Baseline` | L2 渲染图 320×240 → 读回 → 与 `tests/data/golden_l2.png` 逐像素（容差 **0**，同 `ui_test.cpp:1058-1066` 口径）（D14） |

**汇总（N2：三处口径已统一，无"以本条为准"的自我豁免）**：
Scene 6 + SubmitQueue 5 + RenderGraph 4 + Stream 10 + Recorder **9** + FeatureRegistry 3 + L2Golden 1 = **38 条**。
（Recorder 比 v2 的 8 多 1 条：新增 `StreamMatchesCommandListCount`（W4 的强不变量）。）

### 6.2 现有回归（不回退）

基线为**实测** `ctest -N` = **99**（本轮开工前实跑）。

- `ctest` 从 **99** 增加到 `99 + 38（L2 gtest）+ 7（CLI）+ 1（无时钟门控）= 145`，**0 失败 0 跳过**。
- `golden_l0.png` / `golden_ui.png` **逐像素不变**（L2 不改 L0/UI 渲染逻辑，也不改 `scene_stub.h`）。
- `Command.RecordsAllOps == 11` **不变**（L1 公共接口零改动）。

### 6.3 门控

| 门控 | 现状 | 本轮 |
|---|---|---|
| G1 无 GS | rc=0 | 不变（L2 不新增 stage） |
| G3 分层纯度 | rc=0 | **新增** `src/scene/**`、`src/l2_cli/**` 被自动覆盖；加**负向自测样本**「`src/scene/foo.cpp` 里出现 `VkImage` → G3 必红」 |
| G4 内存路径 | rc=0 | 不变（L2 无裸分配） |
| G5 依赖回归 | `ran=191`、**`--expect-files 12`** | **保持 12 不变**：无时钟门控的**正/反例测例加进既有 `tools/tests/test_check_scripts.py`**，**不新建** `test_*.py`（N3：硬门控不许"二选一"，也不改这个精确计数） |
| **新：无时钟** | — | `tools/check_scene_no_clock.py` 扫 `src/scene/**` + `include/magicblocks/scene/**`，命中 `std::chrono\|steady_clock\|system_clock\|high_resolution_clock\|time\(|clock\(\|GetTickCount\|QueryPerformanceCounter` 即失败；先剥注释/字符串（同 `check_layer_purity.py:31-57`）。自测（加进 `test_check_scripts.py`）：注释里出现 `steady_clock` → 通过；`src/scene/foo.cpp` 用 `steady_clock` → 必红 |

**为什么"无时钟"走 Python 门控而不是 gtest**（D13）：gtest 的工作目录无保证（`ctest` 从 `build/` 启动），
`CMakeLists.txt:236-239` 只注入 `MB_GOLDEN_DIR/MB_CLI_PATH/MB_UI_CLI_PATH`（事实 18），
没有源码路径；而 Python 门控与 G1/G3/G4 的既有范式一致，且有现成自测机制。

### 6.4 CLI 端到端（CTest，**7 项**，D16 对齐命名 + N6 补正向 `--scene demo`）

| CTest 名 | 命令 | 判定 |
|---|---|---|
| `magicblocks_l2_record` | `record --scene demo --width 320 --height 240 --frame-index 3 --camera 0,1.5,4 --record cap.bin --output out.png` | rc=0，两个文件都存在（**正向覆盖 `--scene demo`**） |
| `magicblocks_l2_replay_verify` | `replay --replay cap.bin --output out2.png --verify` | rc=0，与 out.png **逐像素一致** |
| `magicblocks_l2_reproduce_verify` | `reproduce --replay cap.bin --output out3.png --verify` | rc=0，逐像素一致 |
| `magicblocks_l2_dump_json` | `dump --replay cap.bin --json` 跑两次 → 输出**字节相同** | rc=0（B8 的 CLI 侧旁证） |
| `magicblocks_l2_replay_missing_cap` | `replay --replay nonexistent.bin --output x.png` | rc=1（**不静默产出空图**） |
| `magicblocks_l2_scene_preset_rejected` | `record --scene sketch --output x.png` | **rc=2**（D16：preset 属 L4） |
| `magicblocks_l2_cli_bad_arg` | `record --bogus 1` | rc=2（`WILL_FAIL`） |

### 6.5 结构化断言（防"错基线自我固化"）

1. L2 golden **必须由 SwiftShader 产出**，生成后断言**左上角纯绿**（`R<60 && G>200`）、
   **右上角不是纯绿**（与 L0 的纯红标记**不同**，两套 golden 可独立判定）（D14 / ARCHITECTURE §10.8）。
2. 回放与复现的 PNG 与录制 PNG **逐像素一致**（不依赖 golden）。
3. 命令哈希跨三个子命令一致（record / replay / reproduce 都打印 `hash=`，CTest 比对）。

## 7. 执行顺序

1. **公共头** `include/magicblocks/scene/*`（纯接口，零 `Vk*`，`namespace magicblocks::scene`）→ 编译期红。
2. **`command_stream.{h,cpp}`**：`ByteWriter/ByteReader` + 规范化 + 二进制 + 哈希（**不依赖 Vulkan**）。
3. **`frame_capture.cpp`**：二进制编解码 + JSON（键序固定）。
4. **`scene_impl` / `submit_queue` / `render_graph` / `feature_registry`**（纯逻辑）。
5. **`recorder.cpp`**：三态状态机 + 双写 + `PipelineTable`。
6. **L2 gtest**（§6.1 全 38 条）先绿（其中 S4/S6/S7 全跑软件态命令列表）。
7. **`src/l2_cli/main.cpp`** + 端到端 CTest + golden 产出（SwiftShader + 左上角纯绿断言）。
8. **新门控** `tools/check_scene_no_clock.py` + 自测（正/反例加进既有 `tests/test_check_scripts.py`，**不动** `check_deps_tests.py` 的 12 计数）。
9. **改任何 `include/magicblocks/**` 头之后必须 `ninja -C MagicBlocks\build -t clean` 再全量构建**
   （ARCHITECTURE §10.12；本轮会动 `CMakeLists.txt` 与新增公共头，**必踩**）（E18）。
10. **文档**：`README.md` / `docs/ARCHITECTURE.md` 的 L2 段与进度表 + `tests/data/README.md` 的 `golden_l2.png` 段。

## 8. 影响面核对

- **新增**：`include/magicblocks/scene/*.h`（6 头）、`src/scene/*`（8 文件）、`src/l2_cli/main.cpp`、
  `tests/magicblocks_l2_test.cpp`、`tests/data/golden_l2.png`、`tools/check_scene_no_clock.py`。
- **修改**：`CMakeLists.txt`、`tools/tests/test_check_scripts.py`（G3 负向样本 + 无时钟门控正/反例；**`check_deps_tests.py` 不改**）、
  `README.md`、`docs/ARCHITECTURE.md`、`tests/data/README.md`。
- **不触碰**（第 2 版明确）：`include/magicblocks/resources/**`（**含 `command.h`**）、`include/magicblocks/device/**`、
  `include/magicblocks/ui/**`（**含 `scene_stub.h`**）、`src/backend/**`（**含 `vulkan_command.cpp`**）、
  `src/ui/**`、`tests/ui_test.cpp`、`tests/magicblocks_l1_test.cpp`、`EasyPainter/`、`third_party/*`、`tools/deps_lib/*`。
- **既有回归**：`tools/tests` 全绿；ctest 99 → 145。

## 9. 待裁决项（已在第 2 版全部裁决，留档）

| 编号 | 问题 | 第 2 版裁决 |
|---|---|---|
| R1 | 给 L1 加 `recorded_commands()`？ | **不加**。改 L2 双写（§4.6），L1 公共接口零改动 |
| R2 | `ReproduceFn` 形态 | `std::function<Status(IScene&, const std::vector<InputEvent>&, ISubmitQueue&)>` |
| R3 | L2 golden 标记色 | **纯绿**（L0 已用纯红），二者可独立判定 |
| R4 | `material_key` 来源 | 提交方给 `u64`，L2 只排序；取值语义归 L3 的 `IMaterial` |
| R5 | `SceneSnapshot` 字段 | `{id, parent, name, transform, local_aabb, visible}`，与 UI 桩**同构**（但不转发，§5.3） |
| **R6** | CLI `--scene` | L2 只支持 `--scene demo`；其它取值 rc=2 并指向 L4（§4.8） |
| **R7** | 资源身份写 packed 还是四字段 | 只写 `packed_resource`；`packed_ok=false` 时 `record()` 直接失败（§4.5） |
| **R8** | pass name 来源 | 来自 L2 的 `scene::PassDesc.name`（L1 无此字段，§4.5） |

## 10. 明确不做（本轮）

- **描述符集 / bindless / 纹理图集**（L1 R4 已定 push constant 优先）
- **多线程录制 / 多队列 / 异步计算**（G5）
- **L3 primitives / L4 features / 玩法实现**
- **`ITexture` 当渲染附件**（rulings 遗留约束继续生效；"再贴图"用中间 `IRenderTarget`，§4.4）
- **ui 层接入 L2 场景**（适配器与 12 方法迁移是独立任务；本轮 `scene_stub.h` 零改动，§5.3）
- **ui 覆盖层管线进 L2 的图**（后端私有管线不在 `IPipeline*` 体系里，§5.2）
- **iOS / Metal / GLES**、**阴影 / 动画 / 骨骼**、**性能优化**、**MSAA 解析 / rev-Z 实际启用**

## 11. 验收（spec §11 的 L2 子集）

1. **接口纯度**：G1/G3/G4 零违规；G3 负向样本必红；G5 全绿（含计数同步）。
2. **命令模式**：录制 → 回放 → 复现，**哈希与 PNG 一致**；二进制 + JSON 往返一致且 JSON 可 diff。
3. **内存门槛**：稳态（暖机 3 + 差分 30 帧）`vma_alloc/free` 增量 **0**，且 `allocation_count` 增长（非假绿）。
4. **CLI + 离屏**：6 项端到端 CTest 全绿，无需窗口/显示服务。
5. **测试门**：**145 项，0 失败 0 跳过**（99 既有 + 38 L2 + 7 CLI + 1 无时钟门控）。
6. **零回退**：`golden_l0.png` / `golden_ui.png` **逐像素不变**；`RecordsAllOps == 11` 不变。
7. **清洁构建**：`ninja -t clean` 后全量构建 + ctest 全绿（E18 / ARCHITECTURE §10.12）。

## 12. 风险与对策

| 风险 | 对策 |
|---|---|
| 以为能读 L1 录制流（提交即清空） | **双写**（§4.6）；事实 2/3 写进 §1 |
| L2 实现落错命名空间 → G3 红 | 写死 `magicblocks::scene`（§2.1）+ 负向自测样本 |
| `src/l2_cli` 依赖 backend 头 → G3 红 | 只 include `magicblocks/...` + `core/image_io.h`（§2.2） |
| 设备态回放缺管线 | `PipelineTable` + 找不到即 `Unsupported`（带 hash 与已登记列表）；用例覆盖 |
| 浮点哈希/序列化漂移 | ByteWriter 逐字段小端、`bit_cast` 喂位模式、禁整结构 `memcpy`、未用槽位写 0 |
| 排序不稳导致 golden 漂移 | 显式 SortKey + `stable_sort` + tie-break + 确定性用例 |
| 设备态 `draw` 需真 `VkPipeline` → 软件态测不了 S4 | S4/S6/S7 **全部**用例跑软件态列表；设备态只在 CLI 端到端验 |
| `scene_stub.h` 改动破坏 UI 用例 | **不转发**（§5.3）：`scene_stub.h` 零改动，`FakeScene` 零改动 |
| 改头文件不触发重编 | 每次改头后 `ninja -t clean`（§7 步骤 9） |
| golden 自我固化 | SwiftShader 产出 + 左上角纯绿 / 右上角非纯绿结构化断言（§6.5） |
| 新门控脚本破坏 G5 计数 | **不新增 `test_*.py`**：正/反例加进既有 `test_check_scripts.py`，`--expect-files 12` 保持不变（N3 / 事实 17） |
| 无 SwiftShader（提权） | 沿用 L0 退出码 3 + `register_swiftshader_icd.ps1`；本机已实测可用 |

## 13. 第 1 版审阅意见落实对照（21 条 + 2 条自加）

| 审阅编号 | 内容 | 第 2 版落点 |
|---|---|---|
| A1 | 删除 L1 公共扩展，改双写 | §5.1、§4.6、§8「不触碰」 |
| A2 | 写死 L2 命名空间 | §2.1 |
| A3 | 定 `src/l2_cli` 的 G3 口径 | §2.2 |
| B4 | 补 S3「先离屏再贴图」 | §4.4 |
| B5 | 离屏纹理能力二选一 | §4.4（采纳方案甲）、§10 |
| B6 | S2 补相机距离输入 | §4.3、用例 `DistanceKeyUsesCamera` |
| B7 | S8 补初始场景快照 | §4.5 格式段、`Recorder.ReproduceFromCaptureSnapshot` |
| B8 | JSON 可 diff 验收 | §4.5、用例 `JsonDumpIsStable`、CTest `magicblocks_l2_dump_json` |
| C9 | 修正事实表 + 提交顺序 | §1 事实 2/3/4、§4.6「提交时机」 |
| C10 | 设备态回放明确结论 | §5.2、§10 |
| C11 | pass name 来源 | §4.5 裁决表 |
| D12 | 统一用例数 | §6.1（38 条）、§6.2、§11.5（145） |
| D13 | 无时钟改 Python 门控 | §6.3、§7 步骤 8 |
| D14 | golden 比对闭环 | §6.1 `L2Golden.MatchesL2Baseline`、§6.5 |
| D15 | 补齐 7 条用例 | `RejectsUnpackableResource`(新)、`ReplayMissingPipelineIsUnsupported`、`ReproduceFromCaptureSnapshot`、`JsonDumpIsStable`、`PassOutputFeedsNextPassAsInput`、`DistanceKeyUsesCamera`、`RecordsIntoGraphPass` |
| D16 | CLI CTest 命名 + `--scene` 行为 | §6.4、§4.8 |
| E17 | `scene_stub.h` 迁移二选一 | §5.3（**不转发**） |
| E18 | ARCHITECTURE §10.12 清洁构建 | §7 步骤 9、§11.7 |
| E19 | M5 稳态定义 | §6.1 `SteadyStateZeroVmaCalls`、§11.3 |
| E20 | 缺管线硬失败 + message 细节 | §5.2 |
| E21 | 删除"默认 Unsupported 兜底" | §5.1 末句 |
| **自加 1** | `check_deps_tests.py` 精确断言 12 个用例文件 | §1 事实 17、§6.3、§12（**第 3 版做法不变更该计数**：无时钟正/反例并入既有 `test_check_scripts.py`） |
| **自加 2** | N=2/3 帧环形复用下，`M5` 稳态测试的"帧"指 `IRecorder` 的帧而非 `IFrameSync` 槽位 | §6.1 `SteadyStateZeroVmaCalls`（与 L1 `magicblocks_l1_test.cpp:355-360` 同口径，直接复用其 `begin_frame` 循环） |

## 14. 第 2 轮审阅意见落实对照（Q1/Q2 + N1-N8）

| 编号 | 内容 | 第 3 版落点 |
|---|---|---|
| **Q1** | `packed_ok==false` 的拦截位置：必须在 `target->bind_*()` **之前**预检，否则留下部分成功态 | §4.6.1 铁律 **W2**（`pack()` 是 const 纯函数，先 pack 再调 L1）；"合法但未登记的目标"改由 `validate()` 拦（§4.4 规则 2） |
| **Q2** | 双写发散无规则 = 接受未定义行为 | §4.6.1 **W1/W2/W3/W4** 四条铁律（先校验→调 L1→成功才 append；预检；帧内失败整帧作废且禁止 submit；`commands[]` 含 begin/end_pass） |
| Q2 派生 | `commands[]` 是否含 begin/end_pass（v2 未说） | **含**（W4），得到强不变量 `stream.commands.size() == target->command_count()`；新增用例 `Recorder.StreamMatchesCommandListCount` |
| **N1** | `AttachmentRef::PassOutput` 无可取用的 `IRenderTarget*` | §4.4 新增 `PassDesc::own_target` + `IRenderGraph::resolve(AttachmentRef)` 解析入口；`validate()` 规则 2 校验解析结果非空 |
| **N2** | 用例数 35/37/143 三处打架 + "以本条为准"自我豁免 | §6.1 标题/汇总/套件三处统一为 **38 条**；§6.2/§11.5 统一 **145**；基线用实测 `ctest -N` = 99；**删除自我豁免句** |
| **N3** | `--expect-files` 留"12 或 13，实施时二选一" | §6.3 定死：**保持 12**，无时钟正/反例并入既有 `tools/tests/test_check_scripts.py`，**不新建 `test_*.py`**；§8 修改清单同步 |
| **N4** | `SteadyStateZeroVmaCalls` 未写运行态，反假绿断言在软件态恒不成立 | §6.1 改为**设备态**（`require_sw_device()`，无设备 `ADD_FAILURE` 不 skip）；注明软件态列表从不碰分配器（`vulkan_command.cpp:581-582`） |
| **N5** | L2 无管线创建入口、`PipelineTable` 填充者缺位 | §4.6.2 **新增一节**：`IRecorder::on_attach()` 建管线/顶点缓冲并登记 `PipelineTable`；表由 `IRecorder` 持有；L2 只调用 `IRenderDevice::create_pipeline` |
| **N6** | `--scene demo` 是唯一取值但参数表没有它、CTest 无正向覆盖 | §4.8 参数表补 `--scene demo`；§6.4 新增**正向** `magicblocks_l2_record`（带 `--scene demo`）+ `magicblocks_l2_replay_missing_cap`（rc=1），CLI 项 6 → **7** |
| **N7** | 计划代码块用 `u8`，公共面并无此别名 | §4.4.1 **新增一节**：公共头只用 `std::uint8_t/uint32_t/uint64_t/int32_t` + `float`；**不复用** `ui::f32`（反向依赖）；§4.5 位格式段同步改写 |
| **N8** | §4.5 头部 `reserved` 区间错 1 字节 | §4.5 头部改成**逐字节区间**并标注合计 12B，明确从 `[12)` 读 `frame_index` |
| **R9/U1** | 命令流的 `draw*` **不发 `vkCmdDraw`**，回放不等于重放出图 | §4.6.1.1 U1（含义澄清 + `replay` 只重录不重提 + golden 来源是录制那次提交）、§10 |
| **R10/U2** | `pass_area` 的"0 = 整图"语义在设计端已丢 | §4.6.1.1 U2（`PassRecord` 存原始值，§4.5 的 `area[4]` 指原始调用值） |
| **R11** | 侦察建议 `scene_stub.h` 改逐名 `using` 转发（成本≈1 行） | §5.3.1 **维持不转发**：转发要求 `ui::stub::IScene` 与 `scene::IScene` 同型 → `ui_test.cpp:121` 的 `FakeScene` 只实现 4/12 方法必 C2259；叠加 `record()` 签名、`DisplayInfo` 反向依赖、`IOverlay2D` 三处冲突 → 属独立 UI 集成任务，非 L2 改动 |
| **R12** | 侦察提示 UIG2 门控扫整个 `tests/`，禁 `submit\s*[<(]`；M5"零 VMA"在 UI 侧是假绿 | §6.1 `SteadyStateZeroVmaCalls` 自己用 `AllocatorStats` 差分（不信 `FrameSample`）；L2 测试**不写** `.submit(`/`->submit(`，只用 `submit_and_wait()` |


