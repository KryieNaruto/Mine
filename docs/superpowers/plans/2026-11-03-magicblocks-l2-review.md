# MagicBlocks L2 计划审阅结果

> 本文件含两轮审阅。**最新结果见「第 2 轮（v2）」**；第 1 轮（v1）保留为历史记录。

---

# 第 2 轮审阅（第 2 版计划）

日期：2026-11-03 · 审阅对象：`docs/superpowers/plans/2026-11-03-magicblocks-l2.md`（**第 2 版**）
上一轮：第 1 版 **75.9/100 → 打回**，21 条建议 A1-A3 / B4-B8 / C9-C11 / D12-D16 / E17-E21。
本轮性质：**独立重审**——不信任 §13 的自述，逐条对照正文与真实代码；重点找 **v2 新引入的矛盾**，
并对 Lead 提出的两个设计问点（§4.5 `packed_ok` 的强制位置、§4.6 双写的发散风险）给出独立裁决。

## 审阅结果（第 2 轮）

总分：87.6/100 → **通过**

| 维度 | 权重 | 得分 | 扣分原因 |
|---|---|---|---|
| 架构合理性 | 25% | 90 | 分层裁决全部落实且可机械验证：L1 公共头零改动（§5.1/§8）、`namespace magicblocks::scene` 写死（§2.1）、`src/l2_cli` 只依赖公共头（§2.2）、`ui::stub` 不转发（§5.3）。扣分点：① §4.4 的 `AttachmentRef{external, producer_pass}` **无可取用的中间 `IRenderTarget*` 字段**——`producer_pass` 校验通过后使用者拿不到目标，必须补解析入口（否则 `readback` 与录制都无法定位该目标）；② `src/l2_cli/main.cpp` 被 §2.2 限制为「只 include 公共头 + `core/image_io.h`」，而该头在 `src/core/` 下（`target_include_directories` 对库是 `PRIVATE src`），CLI 目标须自行补 include 路径，§8 未列这一 CMake 改动。 |
| 需求完整性 | 25% | 91 | S1-S9 逐条有接口/实现/用例；S3「先离屏再贴图」、S2 相机距离、S8 完整快照三处 v1 缺口的**解法都在正文里写实了**（§4.4 方案甲、§4.3 `sort()` 入参、§4.5 node 段）。扣分点：① §6.1 的**用例数三处不一致**（标题 35 / 汇总 37 / `Stream` 套件标题 9 实列 10），且 §6.2 的 `99+37+6+1=143` 把既有 `magicblocks_gate_deps_tests` 口径算进来，需实测 `ctest -N` 核对；② `--scene demo` 是 v2 新引入的必选项/唯一取值，但 §4.8 的参数表里**没有它**，且 §6.4 的 6 条 CTest **没有一条**用 `--scene demo`（只测了被拒的 `--scene sketch`）——「唯一取值」这条正向路径无覆盖。 |
| 技术可行性 | 20% | 85 | 双写自洽、`PipelineTable` 前提写清、位格式到字节级（`ByteWriter` 逐字段小端 / `bit_cast` / 禁整结构 `memcpy` / 未用槽位写 0 / UTF-8 字节长度）。扣分点：① §4.7 承诺「L2 演示特性用顶点数据画屏幕空间图元」，但 L2 **没有任何 `IPipeline` 创建入口**，也没说谁建管线/谁登记进 `PipelineTable`——`IRecorder` 自己不建，`PipelineTable` 只收调用方给的指针，链条缺一环；② Q2 的双写发散**没有任何条文约束**；③ 回放/复现的 `PipelineTable` 前置在 CLI 路径上隐含最强，但 §6.4 的 `magicblocks_l2_replay_verify` 只断言 rc=0 + PNG，**不验回放是否因缺管线而 Unsupported**。 |
| 可测试性 | 15% | 84 | 37 条按套件分组、每条给了可断言的期望值；新增 7 条正好覆盖 v1 的洞（`RejectsUnpackableResource`、`ReplayMissingPipelineIsUnsupported`、`ReproduceFromCaptureSnapshot`、`JsonDumpIsStable`、`PassOutputFeedsNextPassAsInput`、`DistanceKeyUsesCamera`、`RecordsIntoGraphPass`）；无时钟改 Python 门控 + 自测；golden 有 `L2Golden.MatchesL2Baseline`（容差 0）。扣分点：① 用例数口径三处打架（同上）；② `check_deps_tests.py --expect-files` 处理成「12 或 13，实施时二选一」——**门控要求精确断言，不能留二选一**；③ `SteadyStateZeroVmaCalls` 的「设备态 vs 软件态」没写，而 `allocation_count` 增量 > 0 这个反假绿断言在**软件态**下恒不成立。 |
| 风险与健壮性 | 15% | 86 | 风险表 12 行覆盖 v1 全部坑 + §10.12 清洁构建 + `check_deps_tests` 计数 + golden 自固化；错误路径显式（`Unsupported` 带 hash、`Invalid` 带 page/offset）。扣分点：① **双写部分失败没有定义**（Q2）；② `packed_ok=false` 时「直接 `Invalid`」只考虑非法句柄，**未考虑"合法但未登记"的中间目标**（会撞上 L1 的 `RenderPassDesc.target==nullptr → Invalid`，行为不可控）；③ 设备不可用（无 SwiftShader）的失败语义无明文——建议一句话声明「失败即 FAIL，不 skip」（与 L0 `Golden.RequiresSwiftShaderDevice` 同口径）。 |

---

## 一、v1 的 21 条建议 · 独立核验（不看 §13，只看正文与代码）

| 编号 | v2 落点 | 核验结果 | 证据 |
|---|---|---|---|
| **A1** 删 L1 扩展、改双写 | §5.1、§4.6、§8「不触碰」 | ✅ **真落实**。正文明确「不读 L1 的录制流」并给出理由；§8 明文 `include/magicblocks/resources/**`（含 `command.h`）**不触碰** | 计划 §5.1/§8；代码 `vulkan_command.cpp:587-588,626-627` 与事实 2 一致 |
| **A2** 写死命名空间 | §2.1 | ✅ **真落实**。`namespace magicblocks::scene` 写死，并解释为何不能用 backend 前缀 | `check_layer_purity.py:110-116` |
| **A3** `src/l2_cli` G3 口径 | §2.2 | ✅ **真落实**，且**正确识别了 `src/cli/main.cpp:21` 的正则漏洞并声明「不是许可」**（比 v1 建议更清楚） | `check_layer_purity.py:18` 只认 `vulkan\|volk\|vulkan_beta` |
| **B4** S3「先离屏再贴图」 | §4.4 + 用例 | ✅ **真落实**：`AttachmentRef{Source::ExternalTarget\|PassOutput}` + `producer_pass < 当前下标` + `PassOutputFeedsNextPassAsInput` | 但**缺解析入口**（见 N1） |
| **B5** 离屏纹理二选一 | §4.4 方案甲 + §10 | ✅ **真落实**：明确「不用 `ITexture`」「不引入 `PageKind::DeviceTexture`」，并写进 §10 明确不做 | `l1-rulings.md:33-35` |
| **B6** S2 相机距离 | §4.3 | ✅ **真落实**：`SubmitItem.world_position` + `sort(policy, camera_pos)`，`distance_key` 排序期算、不参与序列化；`DistanceKeyUsesCamera` 断言「两处相机顺序相反」——可判定 | 计划 §4.3 |
| **B7** S8 初始场景快照 | §4.5 node 段 + 两条用例 | ✅ **真落实**：`node_count` + 按 id 升序的全字段 node 记录（含 `local_aabb`），`initial_scene_hash` 由该段单独求 | 计划 §4.5 |
| **B8** JSON 可 diff | §4.5 键序 + `JsonDumpIsStable` + CTest | ✅ **真落实**：键序写死 10 个键，`%.9g`，两次 `to_json` 字节相同，CLI 侧 dump 两次比对 | 计划 §4.5/§6.1 |
| **C9** 事实表 + 提交顺序 | §1 事实 2/3 + §4.6 提交时机 | ✅ **真落实**：事实 2 直接引 `vulkan_command.cpp:587-588` 与 `626-627`（**两态都清**，比 v1 更完整）；§4.6 明写「组装完成 → 才 submit」 | 计划 §1 |
| **C10** 设备态回放前提 | §5.2 + §10 | ✅ **真落实**：合法前提 = 每个 hash 都在表里；缺 = `Unsupported` 带 hash 与已登记列表；ui 私有管线明确不进 | 计划 §5.2 |
| **C11** pass name 来源 | §4.5 裁决表 | ✅ **真落实**：来自 `scene::PassDesc.name`，由 L2 翻译成 `RenderPassDesc`；事实 9 引 `command.h:44-56` 确认 L1 无 name 字段 | 计划 §4.5 |
| **D12** 统一用例数 | §6.1 汇总 37 | ⚠️ **半落实**：汇总写了 37 并解释 35 的由来，**但标题仍写「35 条」、`Stream` 套件标题写「9」实列 10**；且出现「以本条为准」的自我豁免句（见 N2） | 计划 §6.1/§6.2/§11.5 |
| **D13** 无时钟改 Python 门控 | §6.3 + §7 步骤 8 | ✅ **真落实**，且给出了「为什么不用 gtest」的正确理由（事实 18 引 `CMakeLists.txt:236-239` 无 `MB_SRC_DIR`），正则覆盖 8 种时钟调用 + 先剥注释/字符串 + 正/反例自测 | 计划 §6.3 |
| **D14** golden 闭环 | `L2Golden.MatchesL2Baseline` + §6.5 | ✅ **真落实**：容差 0（同 `ui_test.cpp:1058-1066`），SwiftShader 产出 + 左上纯绿 + 右上非纯绿 | 计划 §6.1/§6.5 |
| **D15** 补 7 条用例 | §6.1 | ✅ **真落实**：7 条**全部**在表内 | 计划 §6.1 |
| **D16** CLI 命名 + `--scene` | §6.4 + §4.8 | ✅ 基本落实（6 条 CTest 命名对齐、一条一命令、`--scene sketch → rc=2` 有 CTest）；⚠️ `--scene demo` 正向路径无 CTest、且不在参数表（N6） | 计划 §4.8/§6.4 |
| **E17** `scene_stub.h` 处置 | §5.3 | ✅ **真落实**：不转发、零改动，两套 `IRenderFeature::record()` 与两套 `RenderFeatureLayer` 并存并说明「各自在自己的命名空间里，不重名」——正确；`FakeScene` 零改动 | `scene_stub.h:66-80,84-89,92-99` |
| **E18** §10.12 清洁构建 | §7 步骤 9 + §11.7 | ✅ **真落实**，且正确预判「本轮必踩」 | 计划 §7/§11 |
| **E19** M5 稳态定义 | `SteadyStateZeroVmaCalls` + §11.3 | ✅ 口径落实（暖机 3 + 差分 30 + `allocation_count` 增长）；⚠️ **没写跑在哪一态**，软件态下反假绿断言恒不成立（N4） | `vulkan_command.cpp:581-582` |
| **E20** 缺管线硬失败 + message | §5.2 | ✅ **真落实**：`Unsupported` + hash 十六进制 + 最多 8 条已登记列表 | 计划 §5.2 |
| **E21** 删「默认 Unsupported 兜底」 | §5.1 末句 | ✅ **真落实**：全文已无兜底式表述；`scene_stub.h` 不转发也顺带消除了它 | 计划 §5.1 |
| 自加 1 | `check_deps_tests` 计数 | ⚠️ 发现了（事实 17 正确），**但处理留成二选一**（N3） | `check_deps_tests.py:22,35-38`；实测 12 个 |
| 自加 2 | 帧语义（`IRecorder` 帧 ≠ `IFrameSync` 槽位） | ✅ **合理且正确** | 计划 §6.1 |

**结论：21 + 2 条中，18 条真落实、4 条半落实（D12/D16/E19/自加1）、0 条假落实。**
这是「逐条改到可执行」的质量，不是应付式修订。

---

## 二、v2 新引入的问题（fresh-reviewer 视角，按严重度）

### N1【机制缺陷】`AttachmentRef::PassOutput` 解析不到目标（§4.4）

`struct AttachmentRef { Source source; IRenderTarget* external; u32 producer_pass; }` ——
`PassOutput` 分支**没有 `IRenderTarget*` 可读**，`producer_pass` 只是下标。于是：

- `validate()` 规则 2 说「`PassOutput` 指向的中间目标必须**已登记的有效目标**」，但**登记在哪**没定义；
- `Recorder` 调 `begin_pass(RenderPassDesc{target=…})` 时，怎么从 `producer_pass` 拿到那张目标？
  若靠「`graph.passes()[producer_pass]` 的其它字段反查」，那是隐式约定，不是接口；
- 若约定「中间目标由调用方另建一张表登记」，那张表就是 `IRenderGraph` 的第二个数据源，
  `validate()` 得同时查两处 → 易出「表里有、图里没有」的不一致。

**建议（改一行接口即可）**：`PassDesc` 增加 `IRenderTarget* own_target = nullptr;`（本 pass 产出的中间目标，
`Source::PassOutput` 的上游必须填），`validate()` 规则 2 改成「`passes()[producer_pass].own_target != nullptr`
且未失效」；`Recorder` 解析时直接取 `passes()[producer_pass].own_target`。这样图的声明是**单一数据源**，
也顺带让「A 的输出作 B 的输入」在测试里可断言目标指针相等。

### N2【口径矛盾】用例数 35 / 37 / 143 三处不一致（§6.1 / §6.2 / §11.5）

- §6.1 标题「**35 条**」；`Stream` 套件标题「（9）」但表里 **10 行**；表内总行数 = 6+5+4+10+8+3+1 = **37**；
- §6.1 汇总段落自认「以本条 **37** 为准」——**这是自我豁免，不是修正**；出现「以本条为准」就是下一版的歧义源。
- §6.2/§11.5 的 `99 + 37 + 6 + 1 = 143`：`+1` 若指新增无时钟门控 CTest 则合理，但「既有 99 项」是二手数字，
  且既有 `magicblocks_gate_deps_tests` 已在其中——必须核清「99 究竟含哪些 CTest」（现有 `add_test` 名为
  5+3+2+3 = 13 个门控/CLI 名，其余来自 `gtest_discover_tests` 展开）。

**建议**：**先跑 `ctest -N` 实测现有精确项数**（不用「99」），把 §6.1 标题、套件计数、§6.2、§11.5 统一为同一组数；
删掉「以本条为准」类自我豁免句。

### N3【计数逻辑】新增门控脚本与 G5 精确断言的二选一未被裁决（§6.3）

事实 17 识别得对（`--expect-files 12` 精确断言，`len(files) != expect` 即失败），但 §6.3 写成
「12 → 13（若新门控不新增 `test_*.py` 则保持 12，**实施时二选一并同步改数**）」——**计划里不能留"实施时二选一"**：
G5 是硬门控，选错的直接后果是测试门红。

**建议（推荐前者）**：**不新建** `tools/tests/test_scene_no_clock.py`，把无时钟门控的正/反例直接加进既有
`tools/tests/test_check_scripts.py`（该文件已有 22 条 G1/G3/G4 自测，加两条是同一范式）→
`--expect-files` **保持 12 不动**，`check_deps_tests.py` 也不用改；顺带消掉 §8「修改 `tools/check_deps_tests.py`」这行影响面。

### N4【可行性局部】`SteadyStateZeroVmaCalls` 未写明跑在哪一态（§6.1）

- 用例要 `vma_alloc/free` 增量 0 且 `allocation_count` 增量 > 0；
- **软件态**命令列表从不接触分配器（`vulkan_command.cpp:581-582`：无 ctx 直接 `++submit_count_` 走状态复位），
  所以「稳态帧」必须**由设备态驱动**，让 L2 每帧真的从分页分配器切子块再还回；
- 但 §6.1 全篇按「S4/S6/S7 全部用例跑软件态」的口径写，这条反例没被单独标注。

**建议**：该用例行补「**需要设备态**（`require_sw_device()`，同 L1 口径），暖机 3 帧 → 记 `stats()` → 再跑 30 帧 →
断言差分」，并写明「拿不到软件设备 → 按 `magicblocks_l1_test.cpp:51-62` 的 `ADD_FAILURE` 失败，不 skip」。

### N5【契约】L2 没有任何管线创建入口，`PipelineTable` 的填充者缺位（§4.7 / §5.2）

- §4.7 演示链要求「演示绘制特性」真的画东西 → 需要 `IPipeline`（`PipelineDesc.stages` 至少 VS+FS）；
- `IRecorder` 不建管线（只收 `ICommandList*`）；`PipelineTable::add(IPipeline*)` 只接收调用方给的指针；
- 计划没说 **谁**在 **何时** 建管线并登记。而设备态回放的「合法前提」恰恰是「每个 hash 都在表里」——
  **填充策略就是回放能否成功的关键**，不能由实施者临场决定。

**建议**：§4.7 加一步「演示特性在 `on_attach(IRenderDevice&)` 时 `create_pipeline` 一次（不进每帧）并
`PipelineTable::add`」；写明**表的所有者**（建议 `IRecorder` 持有，`record()`/`replay()` 共用）；以及
「回放进程必须重新创建并重新登记（hash 由 `PipelineDesc` 决定、跨进程稳定——事实 8 已证）」。

### N6【覆盖】`--scene demo` 是唯一合法取值，却没有正向 CTest，也不在参数表（§4.8 / §6.4）

§4.8 裁决「接受 `--scene demo`（唯一取值），其它 rc=2」——这是 v2 新引入的**必选项语义**，但：

- §4.8 的参数表 `record --width … --camera … --record … --output …` **没有 `--scene`** → 它必选还是可选？缺省行为未定义；
- §6.4 的 6 条 CTest 只测了 `--scene sketch → rc=2`，**没有任何一条**用 `--scene demo` 走通正路径。

**建议**：① 参数表补 `--scene`（缺省 = `demo`；显式非 `demo` → rc=2）；② `magicblocks_l2_record` 那条命令加
`--scene demo`，或新增 `magicblocks_l2_scene_demo_ok`（rc=0）——「唯一取值」必须有正向证据。

### N7【类型】`u8` 不在 SDK 公共面，L2 公共头若要用需自带别名（§4.3/§4.4 代码块）

计划代码块用到 `u8`（如 `enum class Source : u8`），但：
`device/types.h` 只有 `u32/u64/i32`（**无 `u8`、无 `f32`**）；`ui/types.h` 有 `f32` 但**同样无 `u8`**；
`resources/handles.h` 用的都是 `std::uint8_t`。

`SceneSnapshot`/`Transform` 要与 UI 桩「同构」（R5），UI 桩用的是 `f32`（`ui/types.h:23`）。
**建议**：L2 公共头统一用 `std::uint8_t` / `float`（或在 `scene/` 内自带别名），并在 §3 表格注明
「不复用 `ui::f32`，避免 L2 反向依赖 UI 头（§4.1 依赖方向不允许）」。不阻断，但实施时会撞编译错误，可零成本消除。

### N8【笔误】§4.5 字节布局「reserved」字段位置写错

```
[8..10)  backend u8;  [10) engine_major u8;  [11..12) reserved u16 = 0
```

`[10)` 是单字节写法，而 `[11..12)` 是 **1 字节**却命名为 `reserved u16 = 0` —— `[8..10)` 已占 2 字节
（backend + engine_major），`[10..12)` 才是剩下的 `reserved u16`。解析端按字节偏移硬编码，
**这一行会直接导致首个 pass 的读偏移错 1 字节**。

**建议**：改为 `[8) backend u8; [9) engine_major u8; [10..12) reserved u16 = 0`。

---

## 三、Lead 的两个问点的独立裁决

### Q1：`packed_ok == false` 时让 `record()` 返回 `Invalid` —— 位置对吗？会破坏合法用例吗？

**裁决：方向对，但位置偏了一点，需要把它从「`record()` 末尾」提到「`packed_ok` 判定处 + 事件级前置校验」。**

1. **不该破坏合法用例**：L1 里 `packed_ok=false` 只会在句柄没通过 `pack()` 时出现，而 `pack()` 失败的
   **全部原因**是（`handles.h:70-79`）：`page_id > 0xFFFF`、`offset % 64 KiB != 0`、`offset_units > 0xFFFF`、
   `id > 0xFFFFFFFF`。第二项被 L1 裁定 3 强制消除（分配器 `effective_align = max(req, kPackGranularity)`）；
   其余三项在当前页规格下（设备页 64 MB = 1024 槽、Host 页 8 MB = 128 槽、id 由分配器递增）**不会发生**。
   所以「`packed_ok=false` ⇒ 句柄不可序列化 ⇒ 流不完整」的因果成立，**不会误杀合法路径**。
2. **但位置要往前挪**：若在 `record()` **末尾**统一检查，此时 `ICommandList` 上对应命令**已经成功录制**
   （`vulkan_command.cpp` 每条命令成功即 `append()` 计数 +1），`record()` 再返回 `Invalid` 就意味着
   「target 上有 N 条命令、CommandStream 里只有 M 条」——**部分成功状态**，正是 Q2 的问题。
   **应在调用 `target->bind_vertex_buffer(...)` 之前**先对句柄做 `pack()` 预检（`pack()` 是 `const` 纯函数），
   预检失败 → 直接 `Invalid`，**一条命令都不录**。
3. **还应覆盖「合法但未登记」的中间目标**：`PassDesc.color_attachment.external` 指向一张已 `release_image_page`
   的目标、或 `producer_pass` 的目标已销毁时，`pack()` 可能仍成功（句柄是值），但 L1 的
   `begin_pass(RenderPassDesc{target=nullptr})` 会报 `Invalid`（`vulkan_command.cpp:383-385`）。这类错误应在
   `IRenderGraph::validate()`（§4.4 规则 2）就拦下，**而不是**靠 `record()` 录到该 pass 时才发现。

**结论**：接受「`packed_ok=false` → `record()` 返回 `Invalid`」，但补三条：① 改成**命令级前置校验**（录之前查，不在录后查）；
② message 必须带 `page_id/offset/size`（`packed_ok` 失败本身不写进流，错误信息是唯一线索）；
③ `IRenderGraph::validate()` 在录之前校验 `external`/`own_target` 可用性（配合 N1）。

### Q2：双写会不会发散？部分失败怎么处理？

**裁决：发散**是真实风险，**v2 没有定义任何一条规则**——这是本轮**最该补的一条**，
也是我唯一认为「不补就等于默认接受未定义行为」的点。

发散的三个具体通道（都有代码依据）：

| 通道 | 机制 | 后果 |
|---|---|---|
| ① **L1 前置校验先于 L2 追加** | L2 追加后调 `target->draw(...)`，L1 可能因「未绑管线/未开 pass/push_constant 越界」返回 `Invalid`（`vulkan_command.cpp:152-158, 291-307`） | 流里有、target 上没有 → **哈希与回放不一致**，且录制那帧的提交不会执行该命令 |
| ② **L1 成功、L2 拒收** | L2 追加时发现 `payload` 超限 / `packed_ok=false`（若按 Q1 在末尾检查） | target 上有、流里没有 → 回放少画一条，**PNG 不一致**（golden 能抓，但"错在流"） |
| ③ **写序不固定** | L2 先 append 还是先调 L1，两种顺序对应两种不一致 | 实施者随手选一个就出 bug，且难以测试 |

**建议（4 条具体规则，可直接写进 §4.6）**：

1. **写序固定**：「**先校验 → 调 L1（失败即 `Invalid` 返回，不 append）→ 成功才 append**」。
   即 L1 是唯一权威，`CommandStream` 只记录**已被 L1 接受**的命令 → 失败模式收敛成「流 ⊆ target」，
   不会出现「流里有 target 上没有」。
2. **失败即整帧作废**：`record()` 中任何一步返回非 Ok → 立即返回该 `Status`，`IRecorder` 状态置回 `Idle`，
   并约定「本帧**禁止**调用 `submit_and_wait()`」——提交是 `record()` 成功后的独立一步（§4.6 已写
   「组装完成 → 才 submit」，需再补「`record()` 失败 → 禁止 submit」）；半成品由 `begin_frame()` 清空。
3. **加一条自检不变量**：`record()` 成功返回前断言 `stream.commands.size() == target->command_count()`——
   **前提是明确「`CommandStream.commands` 是否包含 `begin_pass/end_pass` 两条 op」**，v2 没说。
   建议**包含**（与 `RecordedCommand` 一一对应，`Stream.CommandOpsCoverAllEleven` 要求 11 类全覆盖，**必须含 begin/end**），
   这样上面这个等式成为可断言的强不变量（`command_count()` 只计成功录制，`command.h:136-138`）。
   **这条不变量本身就该是一条用例**：`Recorder.StreamMatchesCommandListCount`。
4. **禁止部分成功**：不接受「允许部分录制再返回失败」的口子（与不回退精神不符）；
   若要保留，则 `Status.message` 必须带「已录 N 条 / 流已记 M 条」两个数——**不建议**。

---

## 四、修改建议（第 3 版收尾用；均为 1-2 行可改完的局部修正，无阻断项）

> 本轮判定**通过**。以下建议不构成打回理由，但建议在第 3 版全部吸收（共 10 条，均可零风险落地）。

1. **N1 `AttachmentRef` 解析入口**：`PassDesc` 增加 `IRenderTarget* own_target`，`validate()` 规则 2 改为校验
   `passes()[producer_pass].own_target` 非空且有效；`Recorder` 直接用它作为 `begin_pass` 的 target；
   补断言「B 的 target == A 的 own_target」进 `PassOutputFeedsNextPassAsInput`。
2. **N2 用例数统一**：删 §6.1 标题的「35」、`Stream` 套件的「（9）」与「以本条为准」句；**先跑 `ctest -N` 实测**
   现有项数再定 §6.2/§11.5 的 99 与 143。
3. **N3 定死 G5 计数**：**不新建** `tools/tests/test_scene_no_clock.py`，把无时钟正/反例加进既有
   `tools/tests/test_check_scripts.py` → `--expect-files` **保持 12**；同步删掉 §8「修改 `check_deps_tests.py`」
   与 §6.3 的「12 或 13」。
4. **N4 补运行态**：`SteadyStateZeroVmaCalls` 注明「需要设备态（`require_sw_device()`）；暖机 3 + 差分 30；
   无软件设备 → `ADD_FAILURE`（不 skip）」。
5. **N5 管线生命周期**：§4.7 加「演示特性在 `on_attach` 时建管线（一次，不进每帧）并 `PipelineTable::add`」；
   写明 `PipelineTable` 归 `IRecorder` 持有、录制/回放共用；回放进程需重建并重新登记。
6. **N6 `--scene demo` 正向覆盖**：§4.8 参数表补 `--scene`（缺省 = `demo`）；§6.4 的 `magicblocks_l2_record`
   加 `--scene demo`（或新增 `magicblocks_l2_scene_demo_ok`）。
7. **N7 基础类型**：L2 公共头用 `std::uint8_t` / `float`，或在 `scene/` 内自带 `using u8/f32`；
   §3 注明「不复用 `ui::f32`，避免反向依赖 UI 头」。
8. **N8 修笔误**：`[8) backend u8; [9) engine_major u8; [10..12) reserved u16 = 0`。
9. **Q1 增强**：`packed_ok` 预检**前移到命令级**（调 L1 之前查 `pack()`），message 带 `page_id/offset/size`；
   未登记/已释放目标在 `IRenderGraph::validate()` 拦下。
10. **Q2 补 4 条双写规则 + 1 条用例**：「先校验 → L1 → append」；`record()` 失败即整帧作废、禁止 submit；
    明确「`commands[]` 含 `begin_pass/end_pass`」；新增 `Recorder.StreamMatchesCommandListCount`
    断言 `stream.commands.size() == target->command_count()`；禁止部分成功。

---

## 五、加分观察（v3 请保留）

1. **§5.1「L1 公共接口一行不改」是第 2 版最正确的一步**——同时消掉了 v1 的越层、`RecordsAllOps==11` 的口径风险、
   `command.h` 的改动面，一箭三雕。
2. **§2.2 对 `src/cli/main.cpp:21` 正则漏洞的判断准确**（`VK_INCLUDE` 只匹配 `vulkan|volk|vulkan_beta` 开头），
   且**明确声明「不是许可」**——"证据 + 结论分开写"，正是审阅最希望看到的。
3. **§4.5 的「三条裁决」表**把 v1 最悬空的三个点（资源身份 / `packed_ok` / pass name）一次定死，每条给了理由与代码依据。
4. **§5.3 判「不转发」**并说明「两套 `record()`、两套枚举并存且不重名」——既保住 UI 用例零风险，又不假称已集成，边界诚实。
5. **§13 的对照表**（21 条 + 2 条自加）说明 Lead 逐条消化了审阅意见；本审阅独立核验确认 **18 条真落实**。

---

## 六、结论

- **21 条建议中 18 条真落实、4 条半落实、0 条假落实**；v1 的三个阻断级缺陷（L1 扩展、命名空间、
  `scene_stub.h` C2259）**全部真解决**且解法正确。
- v2 引入的 8 个新问题**全部是局部/文档级**，无一是阻断级：机制缺口 1 个（N1，改一行接口）、
  口径矛盾 3 个（N2/N3/N6）、可行性 2 个（N4/N5）、笔误/类型 2 个（N7/N8），外加 Q2 的双写规则缺定义。
- **加权 87.6 ≥ 80 → 通过**，可进入执行阶段；建议执行前先吸收上述 10 条（尤其 N1、N3、Q2 三组，均可在半小时内改完）。

---

## 附：本轮实测证据索引

| 断言 | 实测结果 | 证据位置 |
|---|---|---|
| `device/types.h` 无 `u8`/`f32` | ✅ 只有 `u32/u64/i32` | `include/magicblocks/device/types.h:13-15` |
| `ui/types.h` 有 `f32` 但无 `u8` | ✅ | `include/magicblocks/ui/types.h:20-23` |
| `pack()` 的全部失败原因 | ✅ 4 种（page>0xFFFF / offset 未 64KiB 对齐 / offset_units 溢出 / id 溢出） | `include/magicblocks/resources/handles.h:70-79` |
| `pack()` 是 `const` 纯函数（可前置预检） | ✅ | `handles.h:70` |
| `command_count()` 含 begin/end_pass，共 11 类 | ✅ | `resources/command.h:136-138`；`vulkan_command.cpp:139-142` |
| 失败的录制不计入 count | ✅ | `tests/magicblocks_l1_test.cpp:622` |
| `begin_pass` 对 `target==nullptr` 报 Invalid | ✅ | `vulkan_command.cpp:383-385` |
| 软件态提交不碰分配器 | ✅ 无 ctx 直接 `++submit_count_` 走状态复位 | `vulkan_command.cpp:581-582` |
| `draw` 前置校验（未绑管线/未开 pass） | ✅ | `vulkan_command.cpp:152-158` |
| `push_constants` 越界校验 | ✅ | `vulkan_command.cpp:291-307` |
| L1 用例拿不到软件设备时 `ADD_FAILURE`（不 skip） | ✅ | `tests/magicblocks_l1_test.cpp:51-62` |
| `check_deps_tests.py` 精确断言 12 | ✅ 默认 `--expect-files 12`，不等即失败；`tools/tests` 实测 12 个 | `tools/check_deps_tests.py:22,35-38` |
| `test_check_scripts.py` 已有 22 条 G1/G3/G4 自测（可承载无时钟自测） | ✅ 实测 12+10+10 分布 | `tools/tests/test_check_scripts.py` |
| 既有 CTest 门控含 `magicblocks_gate_deps_tests`（不是新增项） | ✅ 已在既有测试集内 | `MagicBlocks/CMakeLists.txt:240-241` |
| `L2Golden` 容差 0 口径与 UI golden 一致 | ✅ `count_differing_pixels` | `tests/ui_test.cpp:1058-1066` |
| G3 的 `VK_INCLUDE` 只匹配 `vulkan\|volk\|vulkan_beta` | ✅ | `tools/check_layer_purity.py:18` |
| `scene_stub.h` 的 4 方法 `IScene` + 无参 `record()` + `RenderFeatureLayer` | ✅ | `include/magicblocks/ui/scene_stub.h:66-80,84-89,92-99` |
| `FakeScene` 继承 `ui::stub::IScene` | ✅ | `tests/ui_test.cpp:121-136` |
| L2 产物尚未创建（计划未越界执行） | ✅ `src/scene/`、`src/l2_cli/`、`tools/check_scene_no_clock.py`、`golden_l2.png` 均不存在 | 目录实测 |

---

# 第 1 轮审阅（第 1 版计划，历史记录）

日期：2026-11-03 · 审阅对象：`docs/superpowers/plans/2026-11-03-magicblocks-l2.md`（第 1 版）

## 审阅结果（第 1 轮）

总分：75.9/100 → **打回**

| 维度 | 权重 | 得分 | 扣分原因 |
|---|---|---|---|
| 架构合理性 | 25% | 73 | ① §5.4 把 `recorded_commands()` 加进 L1 公共 `ICommandList`，跨层暴露录制快照；② 计划**没有声明 L2 实现的命名空间**，按 backend 既有约定实现会被 `check_layer_purity.py:110-116` 判为「backend 之外出现 Vk\*」——即计划对自己的产物**违反 G3**；③ §4.4 的「规范化二进制」与 `ResourceId` 实际语义脱节（只写 `ResourceId` 会丢 `size`，见 `handles.h:81-90`）。 |
| 需求完整性 | 25% | 72 | ① **S3「先离屏、再贴图」**未落地；② **S2「透明按相机距离」**在 `SortKey` 里没有相机位置输入；③ **S8「初始场景快照」**在二进制格式里没有字段（只有 `initial_scene_hash`），`reproduce()` 不可实现；④ 用例数 24/27 矛盾。 |
| 技术可行性 | 20% | 78 | ① 漏掉 `submit_and_wait()` 会 `recorded_.clear()`（`vulkan_command.cpp:581-591,626-630`）→「读录制流」方案不可行；② 设备态 `bind_pipeline` 只认 `VulkanPipeline`，`PipelineTable` 里的 `IPipeline*` 装不进 ui 覆盖层/自检管线；③ `RenderPassDesc`（`command.h:44-56`）没有 name 字段，`PassRecord.name` 来源未定义。 |
| 可测试性 | 15% | 76 | ① 用例数 24 vs 实列 27；② 唯一的 PNG 逻辑在 CLI，gtest 无「L2 渲染图 → 读回 → 像素」用例；③ 「源码级无时钟」写成 gtest 不可行（CMake 只注入 `MB_GOLDEN_DIR`/`MB_CLI_PATH`/`MB_UI_CLI_PATH`）；④ 缺 7 条关键用例。 |
| 风险与健壮性 | 15% | 78 | ① 漏「提交清空录像流」；② 哈希规则与端序/浮点/padding 未完全对齐；③ `scene_stub.h` 转成转发别名会让 `ui_test.cpp:121-136` 的 `FakeScene` **编译失败**（方法集不同集）；④ 漏 ARCHITECTURE §10.12 清洁构建；⑤「默认实现返回 Unsupported 由后端覆盖」属回退。 |

### 修改建议（21 条，已在第 2 版逐条落实）

**A. 架构（阻断级）**
1. 【删掉 §5.4 的 L1 公共扩展】不新增 `ICommandList::recorded_commands()`；改为 `record(..., ICommandList* target)`，由 L2 自己双写 `CommandStream`。
2. 【写死 L2 实现的命名空间】`src/scene/**` 一律在 `namespace magicblocks::scene`，并在 `test_check_scripts.py` 加 G3 负向自测样本。
3. 【定 `src/l2_cli/` 的 G3 口径】L2 CLI 只 include `<magicblocks/...>` 公共头，不得像 `src/cli/main.cpp:21` 那样 include backend 头。

**B. 需求完整性**
4. 【补 S3「先离屏、再贴图」】定义 `AttachmentRef{Source::ExternalTarget|PassOutput, producer_pass}` + `validate()` 规则 + `PassOutputFeedsNextPassAsInput` 用例。
5. 【离屏纹理作为附件二选一并写死】推荐方案甲（用 L2 自建的中间 `IRenderTarget`），不引入 `PageKind::DeviceTexture`。
6. 【补 S2 相机距离输入】`SubmitItem.world_position` + `sort(policy, camera_pos)` 排序期计算 `distance_key`。
7. 【补 S8 初始场景快照】二进制头加 `SceneSnapshot` node 记录（id 升序、name 按 UTF-8 字节），`initial_scene_hash` 由该段求。
8. 【补 JSON 可 diff 验收】键序固定 + 两次 `to_json` 字节相同。

**C. 技术可行性**
9. 【修正事实核对表 + 录制与提交顺序】补「`submit_and_wait()` 清空录制流」这一实测事实；L2 在提交前完成流组装。
10. 【设备态回放给明确结论】合法前提 = 所有被引用管线都经 `create_pipeline` 创建并登记；ui 私有管线不进表。
11. 【pass name 来源】`PassRecord.name` 来自 L2 的 `scene::PassDesc.name`，由 L2 翻译成 `RenderPassDesc`。

**D. 可测试性**
12. 【统一用例数】§0.2/§6.1/§11.5 三处统一，并按实际重算总数。
13. 【「无时钟」改用 Python 门控】新增 `tools/check_scene_no_clock.py` + 自测 + CTest 注册。
14. 【golden 比对闭环】加 `L2Golden.MatchesL2Baseline`（容差 0），`tests/data/README.md` 补 `golden_l2.png` 段。
15. 【补齐 7 条用例】`PipelineTable` 重绑定、`replay` 缺管线 → `Unsupported`、未登记资源 id、JSON 结构、S3 先离屏再贴图、S2 相机距离生效、`reproduce` 从 capture 读快照。
16. 【CLI CTest 对齐命名】一条 `add_test` = 一条命令；`--scene` 本阶段不接受 → rc=2。

**E. 风险与健壮性**
17. 【`scene_stub.h` 迁移二选一并写死】推荐保留 `ui::stub::IScene` 不转发。
18. 【把 ARCHITECTURE §10.12 加进执行顺序】改任何 `include/magicblocks/**` 头后必须 `ninja -t clean`。
19. 【M5 稳态定义】暖机 3 帧 + 差分 30 帧 + `allocation_count` 增量 > 0。
20. 【`PipelineTable` 找不到 = 硬失败】`Unsupported` + message 带 hash 与已登记列表。
21. 【删除所有「默认返回 Unsupported」的兼容写法】改纯虚或不提供。
