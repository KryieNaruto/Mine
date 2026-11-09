# F-VX 魔法叠叠乐 审阅结论（review 门）

> 日期：2026-11-09
> 审阅对象：
> - 设计 `docs/superpowers/specs/2026-11-09-magicblocks-l4-fvx-stack-design.md`（728 行）
> - 计划 `docs/superpowers/plans/2026-11-09-magicblocks-l4-fvx.md`（449 行，**执行依据**）
> - 参考 `docs/superpowers/specs/2026-11-09-magicblocks-l4-fvx-dependency-assessment.md`（245 行）
> - 总 spec `docs/superpowers/specs/2026-11-03-magicblocks-render-engine-sdk-design.md`
> 门控：`build-pipeline` ② 审阅门，五维加权，**≥90 通过，<90 打回，无四舍五入**
> 纪律：本文件是审阅者唯一产出；未修改任何源码 / 设计 / 计划文档。

---

## 0. 结论速览

| 项 | 结果 |
|---|---|
| 加权总分 | **81.70 / 100** |
| 门控判定 | **打回**（门槛 90） |
| 硬约束检查 | 四条**全部满足**（CLI 模式 ✅ / 离屏出图 ✅ / 无回退兜底 ✅ / L0–L3 无玩法专用通道 ✅）→ 不属于「违反硬约束整份打回」 |
| 打回原因 | 设计内部矛盾与未定义行为依赖尚未回写，计划把若干**验证性缺口**写成**断言**，且计划与设计在多处源码事实上冲突、执行内容与「L4 零改动」范围自相矛盾 |

---

## 1. 三个已裁定事项的独立核查

### 事项 1：计划的 D-VX-8′ 推翻设计的 P0-1 —— **裁定正确，但设计未回改，属 spec 缺陷**

#### 1.1 证据链逐条独立复核（全部成立）

| # | 计划/用户给的证据 | 我的复核结果 | 结论 |
|---|---|---|---|
| 1 | `l3_cli/main.cpp:639/:1130/:1733` 是 `td.with_depth = true` | 已读原文：`:639`（blocks，注释「3D 积木必须走深度测试」）、`:1130`（gizmo）、`:1733`（sketch，注释「挤出体是实体」）；另 `:348` 为 paint 的 `with_depth=false`（不算） | ✅ 成立 |
| 2 | 三处建管线只调 `create_pipeline`，从未调 `create_pipeline_for_shape` | `:901`（blocks）、`:1446`（gizmo）、`:1864`（sketch）均为 `device->create_pipeline(scene_pipeline_desc())`；`:1451` 另建 overlay 管线，同样走 `create_pipeline` | ✅ 成立 |
| 3 | `create_pipeline_for_shape` 全仓唯一调用点 = `src/android/scene_renderer.cpp:275`，且 `shape.has_depth = false`（`:272`） | 全仓 grep `create_pipeline_for_shape` 共 3 处：定义 `vulkan_render_device.h:58`、实现 `vulkan_render_device.cpp:277`、**唯一调用** `src/android/scene_renderer.cpp:274`；`:271` 为 `shape.has_depth = false`（计划写 `:272` 是 `samples = 1`，**行号偏 1，结论不受影响**） | ✅ 成立（行号需更正为 `:274` / `:271`） |
| 4 | `pass_compatible` 不通过时 `create_pass(...)` 现建 pass | `vulkan_device_frame.cpp:271-278` 比较 `has_depth` / `samples` / `color_format`；`:279-283` 不兼容即 `create_pass(ctx.device, shape, &err)` | ✅ 成立 |

**关键补强（计划 §2.B 未写、但让裁定更硬的证据）**：

- 公共 `create_pipeline(desc)` 走的是 `VulkanPipeline(ctx, desc)` 两参构造（`vulkan_render_device.cpp:268-275` → `vulkan_pipeline.cpp:400-401`），**不填 `shape_`**，于是 `shape_` 保持头文件默认值 `{RGBA8_UNORM, has_depth=false, samples=1}`（`vulkan_pipeline.h:48-53`）。
- `init()` 里 `hash_ = hash_pipeline_desc_with_shape(desc_, shape_)`（`vulkan_pipeline.cpp:443`），而默认形状时该函数**直接退化为** `hash_pipeline_desc(desc)`（`:251-256`）。**推论：经公共面建的管线，其 `hash()` 与「宽度/深度/采样」无关，只由 desc 决定。** 这正是既有 goldens 能保持逐字节不变的结构性原因。
- 因此对于 `with_depth=true` 的离屏目标，`pass_compatible` 必然为 false（`ps.has_depth=false` vs `shape.has_depth=true`，`:274`），后端一定走 `create_pass` 自建带深度的 pass（`:283`、`:84-148`）。`:84-148` 的 `create_pass` 与 `vulkan_pipeline.cpp:454-515` 的 `create_render_pass` 在 attachment 描述、`initialLayout=UNDEFINED`/`finalLayout=GENERAL`、subpass 依赖上**逐项一致**（对照 `:88-99` ↔ `:459-464`；`:102-112` ↔ `:466-476`；`:121-141` ↔ `:485-512`）。**所以「两套 pass 依赖不一致 → 像素不同」的担忧在事实层面不成立。**

**裁定：`不改 L1` 是正确的。** P0-1（给 `resources::PipelineDesc` 加 `uses_depth_attachment`）确实**不是阻塞项**——带深度的双 pass 完全可以通过公共面建管线，pass 由后端按目标形状现建，且像素路径已被 `blocks`/`gizmo`/`sketch` 三个子命令长期使用。

#### 1.2 计划 §2.B 论证里有一处「过度外推」必须修正

计划 §2.B 第 3 条写：

> 「`blocks`/`gizmo`/`sketch` 已在完全相同的路径上稳定出图且 golden 稳定（389 条测试含其 golden 断言），说明该路径像素是确定的。」

**这条表述不准确，需要更正：**

- `blocks`/`gizmo`/`sketch` 三个子命令的像素正确性**进入 CTest 的唯一通道**是 `l3_cli` **进程级** e2e：`CMakeLists.txt:454` 定义 `MB_L3_CLI_PATH`，唯一使用者是 `tests/magicblocks_l4_sketch_test.cpp:428`（`run_sketch_cli`），即 **只有 sketch 有 e2e**；blocks / gizmo **没有任何 CTest 像素断言**。
- `tests/magicblocks_l3_pixel_regression_test.cpp:149` 的 `PixelRegression.SubmitCommandListActuallyWritesPixels` 确实覆盖「`submit_command_list` 真写像素」，但它用 `td.with_depth = false`（`:71`），**并未覆盖 `has_depth=true`**。
- 全仓 `tests/` 中 `with_depth = true` **0 处命中**；即「公共面 + 带深度目标 → `create_pass` 现建 pass」这条具体路径**今天不在任何 CTest 断言之下**。

所以 D-VX-8′ 的裁定**成立**（证据链 1/2/4 + 我补的哈希与 pass 一致性证据足以支撑），但计划「389 条测试已经把这条路径钉死」的说法**不成立**：它把「CLI 能出图」升级成了「测试已断言」，属于把未验证的东西写成已验证。

**影响**：D-VX-8′ 的置信度仍然够（有结构性证据），但计划必须补一条**真正覆盖 `has_depth=true` 公共面路径**的用例，否则「不动 L1」这个重大裁定缺少回归护栏——将来后端一改就静默回归。

#### 1.3 §10.8 / D-VX-8 是否算 spec 缺陷？—— **算，且必须回改设计**

**算 spec 缺陷，必须回改。** 理由：

1. 设计 §10.8 与 §18 第 1 条用**命令式**写「**必须补 P0-1**…不做这条，带深度的双 pass 无法通过公共面建管线」（设计 `:500-509`、`:725`），D-VX-8 同口径（`:698`）；而计划 §2.B 已实证该前提为假。**两份文档现在互相矛盾，且设计是「执行前设计规格，供后续 plans 与实现直接照做」**（设计 `:3`）——留着一个被证伪的强制前置项，等同给实现者埋雷。
2. 更严重的是，设计 §10.2 与 §10.8 还**自相矛盾**：
   - `:459` 说「**必须**通过带附件形状的入口建管线，且形状必须 `has_depth = true`」；
   - `:509` 说「无论选哪条，都**不允许**让 F-VX 走『凭后端私有头直连』的路子」。
   - 但带形状的入口 `create_pipeline_for_shape(desc, shape)` 只声明在**后端私有头** `src/backend/vulkan/vulkan_render_device.h:58`。**「必须走带形状入口」与「不许用后端私有头」在设计内部就已经不可同时满足**——这本身就是设计缺陷，跟 P0-1 无关。
3. 计划 §12 已经写了「设计 §10.8 / D-VX-8 的 P0-1：**裁定不成立**（§2.B），本轮不动 L1」（计划 `:418`），**但只在计划里改，没改设计**。按设计 §18 第 4 条的口径（「如与实现者的实测结论不符，请**改本文档**而不是改实现」，`:728`），这次是**计划**给出了与设计不符的实测结论，同样应该回改设计。

**结论**：属于 spec 缺陷，**需要在进入执行前回改设计**，最小改动集见 §4 修改建议 M1。

---

### 事项 2：D-VX-4′ 说内面正确性不依赖 depthConstWrite —— **事实成立；设计 §10.4 第 4 条确属依赖未定义行为**

#### 2.1 `depthBounds` 特性从未请求 —— 事实成立（比依赖报告描述更严重）

| 证据 | 位置 | 说明 |
|---|---|---|
| `VkPhysicalDeviceFeatures feats{}` 全 0 | `vulkan_render_device.cpp:150` | 零初始化 → 所有特性位为 `VK_FALSE` |
| `dci.pEnabledFeatures = &feats` | `vulkan_render_device.cpp:157` | 直接把全 0 的 feats 交出去 |
| 全仓 `depthBounds` 置位 | grep `depthBounds` 全仓仅 4 处命中：`vulkan_pipeline.cpp:614/615`（2 处为 `depthBoundsTestEnable` 与注释）、`vulkan_pipeline.h:12-13`（注释） | **0 处**对 `feats.depthBounds` 置 `VK_TRUE` |

**比依赖报告更严重的一点**：`feats` 是**全 0**，意味着不只是 `depthBounds`，连 Vulkan 规范的**默认能力**也被关掉了——`depthClamp`、`fillModeNonSolid`、`wideLines`、`logicOp`、`largePoints` 等在 `VkPhysicalDeviceFeatures` 里默认 false 的特性**同样未启用**。

- 我核查了这三个「默认 false 特性」是否被使用：`rs.depthClampEnable = VK_FALSE`（`vulkan_pipeline.cpp:591`）、`polygonMode` 恒为 `VK_POLYGON_MODE_FILL`（`vulkan_pipeline.h:87`）、`cb.logicOpEnable = VK_FALSE`（`vulkan_pipeline.cpp:641`）——**当前都没用到，所以现在没炸**。
- 但这说明 `feats{}` 全 0 是**整条管线的隐藏地雷**，`depthConstWrite` 只是第一个踩上的。计划 §2.C 只记为「`depthConstWrite` 行为未定义」是**低估**了，应登记为「L0 设备创建未启用任何 `VkPhysicalDeviceFeatures`」这一级风险。

#### 2.2 `depthConstWrite` 落成 `depthBounds`，且语义与公共头不符

- 落地方式：`vulkan_pipeline.cpp:614` `ds.depthBoundsTestEnable = desc_.depth.depthConstWrite`；`:618-624` 开启时 `minDepthBounds = maxDepthBounds = clamp(depth_const, 0, 1)`。
- 公共头承诺：`include/magicblocks/resources/pipeline.h:143-151` 明文「`depthWrite=true` 且 `depthConstWrite=true` 时**把整片区域写成 `depth_const` 值**，而不是写片元自身深度」。**Vulkan 的 `VkPipelineDepthStencilStateCreateInfo` 没有任何「写常量深度」字段**，该语义在 Vulkan 1.0/1.1 不可表达 → 实现与契约不符，依赖报告 §3.1 的判定成立。

#### 2.3 设计 §10.4 第 4 条是否属于依赖未定义行为 —— **是，明确属于**

设计 §10.4 第 4 条（`:473`）原文：

> 「**`depthConstWrite = true` + `depth_const` 取 far**：通过 depthBounds 把「本次写入的深度值」整体钉到一个常量（`vulkan_pipeline.cpp` 行 614-624），使内面写进去的深度是**距离最远**的一档。」

逐项判定：

1. **「把写入的深度值钉成常量」本身就不存在**——`depthBounds` 是**片元裁剪测试**（深度落在 `[min,max]` 外则丢弃），**不写深度**。所以这条陈述描述了一个仓库里不存在的机制。
2. **「在 `depth_const=1.0` 时整片通过测试」与设计自己的实测 T 例冲突**。设计 `:113` 记录 T 例（`depthConstWrite=true, depth_const=0.0`）整帧 0.00% 前景、76800/76800 采样点是清屏色——这**恰恰**说明 `depthBoundsTestEnable=VK_TRUE` 在该驱动上**是生效的**；那么当 `depth_const=1.0` 时，只有深度恰为 1.0 的片元能存活，画面应当同样接近全空。而设计 `:114` 的 E/J 例却记录「与无 const 的内面 pass 逐像素相同（39.45% 前景 / 44.08 亮度）」。**T 与 E 在同一个驱动上不可能同时成立**——两条实测结论互相矛盾，其中至少一条是错的。（这也间接说明：所谓「drive 忽略未启用 depthBounds」的解释站不住，真实情况是行为未定义、任何观测都不能外推。）
3. **无论哪种观测成立，前提都是「用了未启用的特性」= Vulkan 规范的未定义行为**（`feats{}` 全 0，见 2.1）。以未定义行为为语义核心写契约，是本设计最硬的一处缺陷。

**结论**：设计 §10.4 第 4 条**属于依赖未定义行为**，必须删除或改写成「不承担语义」，并同步修 D-VX-4（`:694`）、§10.1 表（`:449-450`）、§10.3（`:466`）、§13.1（`:588`、`:597`）、§13.4（`:625`）、A7（`:676`）。

**计划的处置是否正确？——方向对，但不够彻底。** 计划 D-VX-4′（`:75`）说「`depthConstWrite` 仅作为可选的像素观感旋钮，不作为 A7/A8 的通过条件」，这是**正确的收口**，但：
- 计划 §7.2（`:239`）仍然要求实现 pass B 的 `depthConstWrite`「可选」，§7.2（`:245`）仍写「`depth_const` 默认 `1.0f`，禁止硬编码 `0.0f`」，§10.2（`:353`）仍要求实现 `--inner-depth-const 0.0` + `--allow-inner-depth-near` 的参数校验。**既然判定它只是「观感旋钮」且行为未定义，还要求实现它的参数校验与默认值，就是把未定义行为的接口固化成契约**，浪费工时且给 golden 埋不稳定源。应当**默认关闭、不实现 CLI 暴露**（或整项移出本轮）。
- 计划 §2.C 把 §10.4 第 4 条的矛盾（T vs E）**没有识别出来**，只照抄了「`depth_const=0.0` 会整帧变清屏色」这一条。矛盾未消除，后续 golden 会在「pass B 到底出了什么像素」上失去判据。

---

### 事项 3：四条硬约束是否真落地

| 硬约束 | 结论 | 判定依据 |
|---|---|---|
| **CLI 模式 + 离屏渲染输出图像** | ✅ **落地，是可执行条款** | 计划 H1/H2（`:23-24`）；阶段 6（`:338-379`）给出子命令、参数全集、7 步骨架、`verify_png_content.py` 断言、`fc /b` 双跑逐字节比对；e2e 用例 `StackEndToEnd.CliProducesRealPng` / `…ByteIdenticalAcrossRuns`（`:368-369`）有**现成范本**：`tests/magicblocks_l4_sketch_test.cpp:471-511` 用 `MB_L3_CLI_PATH`（`CMakeLists.txt:454`）进程级跑 CLI 并校验 PNG 签名/大小/双跑一致。CLI 是**现有** `magicblocks-l3-cli` 的新子命令（`l3_cli/main.cpp:2006-2013` 分派点），非新建，路径真实。渲染核心只走 `create_offscreen_target` + `submit_command_list` + `readback_rgba8` + `write_png_rgba8`，**零窗口/显示服务依赖**；平台差异仅在 SwiftShader/lavapipe（计划 `:432`）。**可执行、可判真伪。** |
| **不回退 / 不兜底** | ✅ **落地** | 设计 B3（`:47`）、§2.4（`:158-165`）、§6.6 错误码全集（`:388-402`）、§13.4 退出码表（`:622-627`）、D-VX-9（`:699`）；计划 H4（`:26`）、各阶段「失败排查」**全部指向修主路径**而非降级（如 `:177` 哈希不稳 → 改真源下标序，不换算法）；§2.B 末尾「若阶段 3 实测异常 → **停工如实报告**，不私自扩 L1」（`:63`）；风险表 R1/R9 的缓解也都是「主路径重来」而非兜底（`:428`、`:436`）。**未发现任何「替代路径 / 降级跳过 / 预编译产物」设计。** 唯一边缘项：设计 §13.3 的 `--dry-run`（`:617`）——它明确声明「不是降级路径」，且只跑到步骤 2、不创建设备、exit 0，属**功能开关**而非回退，不构成违反。 |
| **平台无关一套渲染代码** | ✅ **落地** | 设计 §3.2 依赖方向只向下、只 include `primitives/`、`scene/`、`resources/`、`device/`（`:193-196`）；计划 H3（`:25`）明确「渲染核心只用公共面，**零** `Vk*`、零后端私有头」；有**静态门禁**：`tools/check_layer_purity.py:119-125` 断言 `backend/` 之外零 `Vk*`/`vk*()`，`:134-135` 断言 `src/features/**` 内零 `vkCmd`/`CmdDraw`，并已注册为 CTest（`CMakeLists.txt:464-465`）。计划另设 `StackGates.NoVkInFeaturesStack`（`:396`）。测试面两侧同套（`check_deps_tests.py:22-23` 要求 `tools/tests` 用例文件数精确等于 12）。**门禁可执行、已入库。** |
| **不在 L0–L3 开玩法专用通道** | ✅ **落地（无硬违反）** | 计划 §12 结论「L0–L3 源码零改动」（`:420`）；设计 §3.4 逐条证伪「需要新能力」（`:209-215`）；G7 门禁 `tools/check_l3_no_gameplay_names.py:29` 只扫 `include/magicblocks/primitives` 与 `src/primitives`，禁用词表 `:34-45` 含 `sketch/blocks/voxel/stamp/叠叠乐/积木/印章`，故 `stack` 命名在 `features/` 合法（计划 `:119` 已核对）；本玩法为**新增** `features/stack/*`（`CMakeLists.txt:244-245` 旁登记），不动 `primitives/`。**⚠️ 但有范围自相矛盾，见 §3 第 7 条。** |

**四条硬约束均满足**，未触发「违反硬约束整份打回」。但第四条存在**执行范围与声明不一致**的问题（下一节）。

---

## 2. 计划与设计在源码事实上的一致性核查（发现 4 处硬冲突）

| # | 出处 | 计划/设计写法 | 实测事实 | 严重度 |
|---|---|---|---|---|
| **F1** | 设计 `:189`、计划 `:120` | 门禁口令用 `python tools/check_l3_no_gameplay_names.py`（`cwd` = 仓库根） | CMake 实际用 `--root ${CMAKE_CURRENT_SOURCE_DIR}` 即 `MagicBlocks`（`CMakeLists.txt:462-463`）；脚本 default `--root "MagicBlocks"`（`check_l3_no_gameplay_names.py:158`）。`L3_DIRS` 是**相对**路径（`:29`），从仓库根跑会因 `os.path.isdir` 不存在而**静默跳过全部文件**，打印「G7 通过…扫描 0 个文件」→ **假绿** | **高**（门禁可被无声绕过） |
| **F2** | 设计 `:203` | `stack` 不在 G7 名单；`blocks`/`voxel`/`叠叠乐`/`积木` 在；「标识符白名单只允许 `block`/`blocks` 出现在已登记上下文」 | `GAMEPLAY_NAMES`（`:34-45`）确实不含 `stack` ✅；`ALLOWED_IDENTIFIERS = {"block","blocks"}`（`:59-62`）是**按词无条件放行**，不区分上下文；抹除用 `ALLOWME`/`ALLOWLIST = "magicblocks\|timestamps?"`（`:56`）按子串抹，注释说明与实现基本一致 | 低（结论对，措辞略夸张） |
| **F3** | 计划 `:25` H3 与 `:258` 用例名 | H3：「渲染核心只用公共面（`IPrimitiveRenderer`/`IBatchBuilder`/…）」；用例 `StackRender.BothPipelinesBuiltViaPublicApi` | `IPrimitiveRenderer::record_frame`（`primitive_renderer.h:49-146`）**不调用 `begin_pass`**（全文件无 `begin_pass`），它只做 `bind_pipeline`/`bind_vertex_buffer`/`draw_indexed`。`l3_cli/main.cpp:1474` 的场景 pass 由**调用方手工** `begin_pass` 后 `:1513-1521` 手工绑定/绘制。**双 pass 必须在渲染器之外手工录制**，「渲染核心只用 `IPrimitiveRenderer`」与「两次 `begin_pass`」不可兼得 | **中** |
| **F4** | 计划 `:253` | 阶段 3 用 `python tools/verify_png_content.py` 断言非背景占比 | 该脚本路径正确（`tools/verify_png_content.py` 存在，`:108-152`），但它是 **进程外脚本**，`gtest_discover_tests` 注册的 CTest 用例（`CMakeLists.txt:455-456`）不会调用它；且它的判据 `:140-146` 含「前后景占比 <85%」「亮度跨度 ≥20」「横向/纵向跨度 >20%」等**几何分布型**判据，对 F-VX 的「高柱 + 空地」构图是**未知是否成立**的（柱状布局横向跨度可能不足 20% 宽 → 误报 FAIL）。计划把 CLAUDE 口径的 `>0` 与脚本口径混写 | **中** |
| **F5** | 设计 `:526`、计划 `:309` | 色板「沿用 `l3_cli/main.cpp:1606-1657` 的黄金分割 hue 序列」 | 该行号区间存在（文件共 2014 行），**属 sketch 段的色板生成**，可复用 ✅ — 但计划 `:309` 引 `main.cpp:1606-1657` 同样区间，需实现者自行抽取为公共函数（否则 `features/stack` 要复制 52 行）。**未给抽取方案**，有复制粘贴风险 | 低 |
| **F6** | 计划 `:191` | `ICamera::screen_to_ray` 标注 `camera.h:86`；`IPicker::intersect_plane` 标注 `picker.h:77` | 逐条核对：`camera.h:86` ✅、`picker.h:77` ✅、`overlay2d.h:136` ✅（`push_line`）、`batch.h:100-109` ✅（`InstanceAttribs`）、`primitive_renderer.h:83` ✅（`register_pipeline`）、`vulkan_target.cpp:126` ✅（硬编码 D16）、`vulkan_pipeline.cpp:614/668-671` ✅、`vulkan_render_device.cpp:150/157` ✅、`vulkan_device_frame.cpp:234/219/271-288` ✅、`vulkan_pipeline.h:48-53` ✅ | — （**引证质量高**，仅 `scene_renderer.cpp:272/275` 偏 1 行） |

---

## 3. 其他实质问题（逐条带证据）

### 3.1 【高】`IBlockSet` 的 `static_assert` 要求注释与断言矛盾

- 设计 `:239`：`static_assert(sizeof(Block) == 24, "Block 必须 24B,与 L3 §6A.1 的 32B SOA 口径对齐(见 §4.3)")` —— **断言值是 24，注释说与 32B 口径对齐**，自相矛盾。
- 设计 `:245` 又称「L3 的 `Block` 是 32 B（`int32 cell[3]` + `uint32 color` + `uint32 stroke_id` + `uint32 flags` + 预留）」，并说「若实现时 L3 的 `Block` 已对外暴露，`Block` 直接**复用**其定义」。
- **实测**：L3 的 `Block` **并不存在**——依赖报告 §4.1 穷举（`:149-155`）与 L3 spec `:939` 都写明 `IBlockSet` 不在 L3 范围、由 L4 实现。所以「复用 L3 定义」是一条**不存在的退路**（虽然它被写成第一选项），而 24 B 与 32 B 两个口径同时出现在设计里，实现者会直接卡住。
- 计划 `:146` 只抄了 `sizeof(Block)==24` 与 `alignof==4`，**没解决 32B 口径冲突**，也没说明 `static_assert(sizeof(Block) == 32)` 到底是哪个。

### 3.2 【高】A8 的像素判据把「实测探针数字」写成了验收断言

- 设计 A8（`:677`）：「`on` 图的不同颜色数 **>** `off` 图（本机实测 `on` 前景内至少多出 2 档色；见 §1.4 的 7 vs 5 / 21870 差别像素）」。
- 但 §1.4 的探针几何是「**8 个立方体沿 X 排成一排**」（`:102`），与真实叠叠乐（**互相遮挡的柱林**）不同；设计自己在 `:120` 就承认「真实叠叠乐是互相遮挡的方块堆，这时不做遮挡判断会让远处方块的内面盖到近处」。
- 把单排探针的「7 vs 5 色 / 21870 差异像素」写进 A8 当验收阈值，是**用一个不适用的实验当判据**。
- **计划的处置是对的**（D-VX-4′ `:75`：「A8 只要求 on/off 两图像素不同，不要求整体更亮」），但**计划没有回头把 A8 的措辞降级为「像素不同」**，形成设计与计划两套判据并存。执行时按哪份走？未定 → 会在测试门吵架。

### 3.3 【高】A7 的「附件形状 `has_depth == true`」用公共查询无法断言

- 设计 A7（`:676`）要求：「两者管线的附件形状 `has_depth == true` 且与目标一致；`effective_depth_state()` 与 §10.1 表逐字段相等」。
- **实测**：`attachment_shape()` 是 `VulkanPipeline` 的**后端查询**（`vulkan_pipeline.h:88-89`），`IPipeline` 公共面**没有**此方法（公共面只有 `desc()`/`hash()`/`is_compute()`/`push_constant_bytes()`，见 `vulkan_pipeline.h:72-79`）。
- 因此这条断言要么（a）测试 `#include "backend/vulkan/vulkan_pipeline.h"` —— 但 `magicblocks_test` 的 include 目录含 `${CMAKE_CURRENT_SOURCE_DIR}/src`（`CMakeLists.txt:443`），**技术上可行**，却与计划 H3「零后端私有头」冲突；要么（b）改成断言 `pipeline_hash`，而**经公共面的管线 hash 与 shape 无关**（`vulkan_pipeline.cpp:251-256`），根本反映不了 `has_depth`。
- **A7 在当前设计下不可按原文实现。** 计划的 `StackRender.PassPairIsDistinct`（`:256`，只断言两 hash 不同）**降级了 A7**，但没有说明降级依据，也没补「公共面无法验证深度形状」的替代方案。

### 3.4 【中】实例化批次 `instance_count > 1` 的分支会改变绑定行为

- 设计 §10.6（`:487`）：「同一份实例缓冲绑定两次：pass A 与 pass B 的 `bind_vertex_buffer(inst.id(), 1, 0)` 指向同一个 buffer」。
- **实测**：`primitive_renderer.cpp:102` 的条件是 `if (instance_buffer_ != nullptr && b.instance_count > 1)`。若某个 batch 的实例数恰为 1，binding 1 **不会被绑定**，但 `:117` 仍把 `instance_count` 置为 1 而画出来 —— 此时 binding 1 是**上一条 batch 残留的绑定**，实例数据错位。F-VX 的 `column_height=1` + 小笔迹场景**很容易造出 instance_count==1 的 batch**。
- 设计/计划都没有识别这个边界。计划 `:244` 的 `(y,z,x)` 排序与 `:241` 的「2 个 DrawRequest」都假设两个 batch 各自实例数 > 1。

### 3.5 【中】「L0–L3 零改动」与阶段 4/6 的实际改动自相矛盾

- 计划 §0（`:15`）与 §12（`:420`）声明：**渲染核心 L0–L3 零改动**；但 §12 同句又列出允许改的文件：`features/stack/` + `src/features/stack/` + `tests/` + `CMakeLists.txt` + `l3_cli/main.cpp`。
- `l3_cli/main.cpp` 属 **L3 CLI**；阶段 4（`:273`）与阶段 5、阶段 6（`:342`）都要改它，阶段 4 还要 `IOverlay2D` 录制。
- 这不是「违反硬约束」（新增子命令不算「玩法专用通道」，符合既有 paint/blocks/gizmo/sketch 并列模式，`main.cpp:2006-2013`），但 **\"零改动\" 的措辞与 \"改 main.cpp\" 的清单在同一行内互斥**，会让实现者/测试者对立项口径产生分歧。
- **应改口径为**：「L0–L3 的**库与 SDK 公共面零改动**；`l3_cli/main.cpp` 仅新增 `cmd_stack()` 与分派/usage 三处，不改既有子命令行为」。

### 3.6 【中】overlay 的坐标系与 `world_to_screen` 的存在性已核实，但设计 §8 缺前提

- 计划 `:278` 用 `IOverlay2D::push_line`（`overlay2d.h:136` ✅）；`LineDesc` 坐标是「屏幕恒宽线段，宽度单位是**像素**」（`overlay2d.h:33-39` ✅）。
- 设计 §8（`:279` 一带）/计划 `:279` 说「网格线位置由 `GridSpec` 与 `ICamera` 投影得到」——**需要 `ICamera::world_to_screen`**，该接口存在（`camera.h:89` ✅）。
- 但设计 §3.4「为什么 L0–L3 够用」表（`:209-215`）**没有把 `world_to_screen` 列进去**（只列了 `screen_to_ray` / `intersect_plane`）。计划 `:191` 的笔刷链路也没提它。这是一条**未被计入依赖矩阵**的实际依赖，虽存在但漏登记 —— 说明 §3.4 的「逐条证伪」并不完整。

### 3.7 【中】`--inner-depth-const` 的契约建立在已判未定义的行为上（见事项 2）

- 设计 §13.1（`:597`）：`--inner-depth-const` 取 `0.0` 必须配 `--allow-inner-depth-near`；§13.4（`:625`）把它列为参数错误；§13.3/§13.1 默认 `on`（`:596`）。
- 计划 D-VX-4′ 已判 `depthConstWrite` **不承担正确性**、`depthBounds` **未启用 → 未定义行为**，却仍然保留（§7.2 `:245`、§10.2 `:353`）这套 CLI 契约。**契约建立在未定义行为上**，golden 无法稳定，且一旦真机和 SwiftShader 表现不同就会「本机绿、别机红」。

### 3.8 【中】计划「新增用例数」实为估算，但表格给成精确数

- 计划 §3 表（`:99-108`）列 6 个阶段「新增用例 / 结束后用例数」：403/419/424/427/437/443/446。
- §3 下方 `:110` 自己承认「阶段 1–5 的『新增用例数』为预估」——但表头写着确定的「结束后用例数」，且阶段 4/7 的产出「1 文件（改 2 处）」「文档」与用例数不匹配（阶段 7 说 ~3 条但清单列了 4 条 `:393-396`）。
- 「只增不减」是硬要求（A14），但**基线数**在阶段 0 才实测（`:125` 写「`381..=? 记录真实数（预期 389）`」留了个占位符 `381`）。这类「估算写成精确值 + 占位符残留」会在测试门制造无谓的判据争议。

### 3.9 【中】阶段 7 的「命令流三态」没有前置依赖说明

- 计划 `:385-387`：「录制：`IRecorder` 记录 F-VX 操作流（Down/Move/Up/lock/unlock/改色/撤销）；三态校验：记录态/回放态/复现态，hash + 像素逐字节相等」。
- 依赖报告 D19（`:57`）称 `IRecorder::record_event`（`recorder.h:127`）、`capture/replay/reproduce`（`command_stream.h:154,165,172`）**完整具备**。但我**未在计划中找到** F-VX 的 `stroke_id` / `lock` 语义如何映射到 `EventRecord{kind,pointer_id,x,y,dx,dy,button,ts}`（`command_stream.h:100-112`）的说明 —— 依赖报告也承认「无 `stroke_id` 概念，但那是 L4 语义，录到事件层即可」（`:57`）。
- 「录到事件层即可」意味着**回放时要重跑整条吸附/扫掠/去重链路**才能得到同样的 `IBlockSet`。这条链路若引入任何浮点不确定性（设计 §6.2 `:353` 的 1e-4·cell_size 量化），三态校验必挂。计划没有把「量化步长的确定性」与「三态像素一致」关联起来。

### 3.10 【中】goldens 跨平台策略只有一行风险，无落地条款

- 计划 R5（`:432`）：Win=SwiftShader / Linux=lavapipe，golden 按平台目录隔离。
- 但硬约束要求「双平台**同一套测试与 golden**」（build-pipeline 常见错误条目：测试必须平台无关）。**「按平台目录隔离 golden」与「同一套 golden」直接冲突** —— 两侧各存一份等于放弃跨平台一致性校验。计划没有说清到底哪一种是目标。

---

## 4. 五维评分

### 架构合理性 —— 88 / 100
- **给分**：分层清晰（`features/stack/` + `src/features/stack/`，设计 §3.1 `:173-187`），依赖单向向下（§3.2 `:193-196`），数据模型选型有据（线性 SOA 而非 ECS，§4.3 `:253-258`，依赖报告 §3.2 `:115-124` 双方案对比后裁定），门禁可执行且已入库（`check_layer_purity.py:119-135`，`CMakeLists.txt:464-465`），模块职责单一（6 头 5 源，设计 `:174-187`）。
- **扣分（-12）**：
  - **-5**：设计 §10.2 `:459` 与 §10.8 `:509` 内部互斥（「必须走带形状入口（后端私有头）」vs「不许用后端私有头」），架构上不可能同时满足。
  - **-3**：设计 §4.1 `:239`/`:245` 的 `Block` 24 B vs 32 B 口径自相矛盾，且「复用 L3 定义」这条退路在 L3 里并不存在（L3 spec `:939`）。
  - **-2**：设计 §3.4 `:209-215`「为什么 L0–L3 够用」表漏登记 `ICamera::world_to_screen`（`camera.h:89`，§8 网格投影必需）。
  - **-2**：计划 H3 `:25`「渲染核心只用 `IPrimitiveRenderer`」与实际（`record_frame` 不 `begin_pass`，`primitive_renderer.cpp:49-146` 无 `begin_pass`；必须手工录制，`l3_cli/main.cpp:1474/1513-1521`）不符，职责边界描述失真。

### 需求完整性 —— 90 / 100
- **给分**：总 spec §4 的 F-VX 六条需求逐条有编号、有落点（设计 §0.1 `:18-25`），§0.2 需求→章节映射表完整（`:29-41`），验收 A1–A14 全部可度量且大多有对应用例（§15 `:668-683`），CLI 契约穷举了参数/退出码/输出格式（§13 `:577-633`），模板生成规则是整数、可复算（§12.2 `:554-563`）。硬约束四条全部覆盖（计划 §1 `:21-29`）。
- **扣分（-10）**：
  - **-4**：A8 `:677` 把「8 立方体单排练」的探针数字当作真实叠叠乐的验收阈值，而设计自己在 `:120` 已承认该几何不适用 → 需求验收判据不自洽。
  - **-3**：A7 `:676` 要求断言管线附件形状 `has_depth==true`，但公共面 `IPipeline` **无** `attachment_shape()`（仅后端 `vulkan_pipeline.h:88-89`），且公共面管线 hash 与 shape 无关（`vulkan_pipeline.cpp:251-256`）→ 该验收点当前**无法按原文验证**。
  - **-3**：计划 §3 `:99-108` 的用例数表把估算写成精确值、阶段 0 验收命令残留占位符 `381`（`:125`），与「只增不减」硬要求（A14 `:683`）配合时会制造无效争议；阶段划分缺「CLI 必须能被 CTest 直采」的覆盖要求（设计 §13.5 `:635-645` 已列 5 条 e2e，但计划 §10.4 `:367-373` 漏了 `CliDryRunMatchesFullRun` 之外的 1 条并有 1 条超集）。

### 技术可行性 —— 75 / 100
- **给分**：核心链路真实可跑（`blocks` 子命令既已出 PNG，设计 §1.5 `:126`）；引证质量高（我抽查 12 处行号，11 处精确命中，见 F6）；`depthConstWrite` 陷阱被独立识别（依赖报告 §3.1 `:68-98`，计划 §2.C `:65-75`）；D-VX-8′ 的证据链经我复核**成立**，且比计划自述的证据更强（`hash_pipeline_desc_with_shape` 默认形状退化 `vulkan_pipeline.cpp:251-256`、两条 render pass 路径逐项一致 `:84-148` ↔ `:454-515`）。
- **扣分（-25）**：
  - **-8**：**D-VX-4′ 收口不彻底**。已判 `depthBounds` 未启用 = 未定义行为（`vulkan_render_device.cpp:150,157` 全 0 feats），却仍在计划 §7.2 `:245`（`depth_const` 默认 1.0）、§10.2 `:353`（`--inner-depth-const 0.0` 需豁免参数）要求实现这套契约 → 把未定义行为固化成 API。
  - **-6**：**事项 2 中 T/E 两例实测结论互斥**（设计 `:113` vs `:114`）。计划照抄「`0.0` 变清屏色」而**未识别矛盾**，导致 pass B 的像素行为缺乏可信依据；一旦 golden 锁定，跨驱动必崩。
  - **-5**：**D-VX-8′ 的验证护栏缺失**。全仓 `tests/` 中 `with_depth = true` **0 处**；`PixelRegression.*` 用 `with_depth=false`（`magicblocks_l3_pixel_regression_test.cpp:71`）；`blocks`/`gizmo` 无 CTest 像素断言（`MB_L3_CLI_PATH` 唯一使用者 `magicblocks_l4_sketch_test.cpp:428` 只跑 sketch）。计划 §2.B `:57` 却称「389 条测试含其 golden 断言」——**把未断言的东西说成已断言**。
  - **-3**：计划 §2.C 只把「`feats{}` 全 0」记为 `depthConstWrite` 单点风险；实际它同时关闭了 `depthClamp`/`fillModeNonSolid`/`wideLines`/`logicOp` 等默认 false 特性（现行代码未用到，`vulkan_pipeline.cpp:591/641`、`vulkan_pipeline.h:87`），属**设备级**隐患，被低估。
  - **-3**：门禁口令 `python tools/check_l3_no_gameplay_names.py` 从仓库根执行会**静默扫 0 个文件并判「通过」**（`check_l3_no_gameplay_names.py:29` 相对路径 + `:158` default `MagicBlocks`；CMake 正确用法是 `--root`，`CMakeLists.txt:462-463`）。设计 `:189` 与计划 `:120` 都用了错误写法 → 门禁假绿。

### 可测试性 —— 78 / 100
- **给分**：用例清单密度高且命名规范（阶段 1/2/5 共 40+ 条具名用例 `:160-173`/`:205-221`/`:320-330`），纯 CPU 层可独立测（阶段 1「不创建 RenderDevice」`:151`），有 e2e 范本（`magicblocks_l4_sketch_test.cpp:471-511` 的 `_popen` + PNG 签名 + 双跑逐字节），确定性要求明确（设计 §13.3 `:615-618`），等价类覆盖了负坐标/格线点/非单位 cell（`StackSnap.*` `:205-208`）。
- **扣分（-22）**：
  - **-6**：A7 不可按原文实现（同需求完整性扣分理由），计划把 `PassPairIsDistinct` 降级为「两 hash 不同」（`:256`）却未说明降级依据、未补替代方案。
  - **-5**：计划 `:253`/`:285`/`:360` 调用 `python tools/verify_png_content.py`，但该脚本不在 `CMakeLists.txt` 的 `add_test` 清单里（`:462-543` 共 21 条，无 `verify_png_content`），gtest CTest 用例**无权调用**；且脚本判据 `:140-146` 含「横向跨度 >20% 宽」「纵向跨度 >20% 高」，对高柱+空地构图**可能误报**。计划把进程外脚本当门禁写，无注册方式。
  - **-5**：`depthConstWrite` 相关用例缺失。计划 §7.4（`:255-260`）5 条用例中**无一条**验证「`depthConstWrite` 开启后像素如何变化」，却又在 §7.2 `:245` 要求默认 1.0 → 实现了却测不了、测了也无基准。
  - **-3**：`instance_count == 1` 的边界（`primitive_renderer.cpp:102`）无对应用例；`column_height=1` 时正是该边界。
  - **-3**：计划 §10.4 `:367-373` 的 6 条 e2e 与设计 §13.5 `:640-645` 的 5 条**不完全对应**（设计有 `CliDryRunMatchesFullRun` ✅ 计划有；设计无 `CliInnerPassOffDiffersFromOn`，计划多出 —— 属合理增强，但未在设计侧登记）。

### 风险与健壮性 —— 70 / 100
- **给分**：风险表 R1–R9 覆盖了双 pass 兼容性、`depthConstWrite` UB、顺序写反、VMA、跨平台、`unordered_map` 序、G7 误伤、CMake 漏登记、工作树三方库（计划 §13 `:426-436`）；「整笔成或整笔不成」（设计 §8 `:426`）避免了半根柱；错误码全集 + 人话 message + 退出码表（§6.6 / §13.4）；§2.B 末尾确立了「实测异常 → 停工如实报告」的阻塞协议（`:63`），符合「不回退」口径。
- **扣分（-30）**：
  - **-8**：**风险 R2 的缓解措施是空的**。计划把 `depthConstWrite` 登记为「中/中」风险，缓解写「不作为正确性来源；A8 只断言 on/off 像素不同」（`:429`）——但**没有降级为「默认关闭 + 不暴露 CLI」**，实际缓解只是「不用它」，而计划 §7.2/§10.2 仍在实现它。
  - **-6**：**T/E 实测矛盾未被当作风险登记**。设计 `:113`（`0.0` → 全清屏）与 `:114`（`1.0` → 与不开 const 逐像素相同）在 depthBounds 生效前提下互斥；无论哪条错，都意味着 pass B 的深度行为**不可预测**，R2 应升为「高」并升级处置。
  - **-5**：**门禁假绿风险未登记**（F1：`--root` 缺失 → 扫 0 文件判通过），且设计 `:189`、计划 `:120` 的验收口令就是错误写法 → 阶段 0「门禁确认」（计划 `:119-120`）会给出虚假的安全感。
  - **-4**：**F5 与 H3 的冲突未登记**：`StackRender.BothPipelinesBuiltViaPublicApi`（`:258`）若为验证 `has_depth` 而 include 后端私有头，就破坏 `check_layer_purity` 的 G3 精神（`src/` 之外零 Vk，但 `tests/` 也在扫描范围 `check_layer_purity.py:97`）。计划未登记这条相互约束。
  - **-4**：**跨平台 golden 策略自相矛盾**（R5 `:432`「按平台目录隔离 golden」vs 硬约束「双平台同一套 golden」），且「一套渲染代码」的**像素一致性**没有验证手段（golden 隔离 = 放弃比较）。
  - **-3**：设计 §18 第 4 条（`:728`）要求「与实测不符请改本文档」，但本轮**计划推翻了设计 §10.8 却没有回改设计**（见事项 1.3）—— 流程性风险：执行者可能照旧设计去补 L1。

---

## 5. 加权总分

| 维度 | 权重 | 得分 | 加权 |
|---|---|---|---|
| 架构合理性 | 25% | 88 | 22.00 |
| 需求完整性 | 25% | 90 | 22.50 |
| 技术可行性 | 20% | 75 | 15.00 |
| 可测试性 | 15% | 78 | 11.70 |
| 风险与健壮性 | 15% | 70 | 10.50 |
| **合计** | **100%** | — | **81.70** |

**总分 81.70 / 100 → 打回（门槛 90，差 8.30 分）。**

> 说明：总分主要被「技术可行性（75）」与「风险与健壮性（70）」拖低。两维的共同根因只有**两件事**：
> ① 事项 2 的未定义行为依赖没有被彻底移除，只被降级成「可选的旋钮」；
> ② 事项 1 的裁定正确但没有回改设计，且缺回归护栏。
> 把这两件事做干净、再修掉 §2 的 4 处硬冲突，重审即可过门。

---

## 6. 修改建议（逐条可执行，按优先级）

> 本轮**不强制**打回后重写全文；下列 M1–M10 全部落地后可重新提交审阅。

### M1【必改·事项 1】回改设计，删除 P0-1 前置项
- 设计 `:500-509`（§10.8）整节改写为「**公共面已足够**」：说明 `create_pipeline(desc)` 走 `VulkanPipeline(ctx,desc)` 两参构造（`vulkan_pipeline.cpp:400-401`）→ `shape_` 为默认 `{RGBA8, has_depth=false, samples=1}`（`vulkan_pipeline.h:48-53`）→ `vulkan_device_frame.cpp:271-283` 因 `has_depth` 不匹配而 `create_pass` 现建带深度 pass（`:84-148`，与 `vulkan_pipeline.cpp:454-515` 逐项一致）→ 功能正确。删除 P0-1 / P0-2 两个方案。
- 设计 `:725`（§18 第 1 条）删除「必须补 P0-1」，改为「**无 L1 前置依赖**」；`enable_if` 保留「默认 false 时既有 goldens 逐字节不变」这条回归要求的**结构性原因**（`hash_pipeline_desc_with_shape` 默认形状退化，`:251-256`），但不再要求新增字段。
- 设计 `:698`（D-VX-8）改写为 D-VX-8′（采用计划口径），并在 §16 表头注明「D-VX-8 已由计划 §2.B 推翻，本文已更替」。
- 设计 `:459`（§10.2）删除「**必须**通过带附件形状的入口建管线」，改为「两个 pass 均走公共 `create_pipeline(desc)`」——同时消解 `:459` 与 `:509` 的内部互斥。

### M2【必改·事项 2】删除对未定义行为的一切依赖
- 删除设计 §10.4 第 4 条（`:473`）；§10.1 表中 `depthConstWrite` 列（`:449`）改为 `false`、`depth_const` 行（`:450`）删除。
- D-VX-4（`:694`）改写为「**内面 pass 不使用 `depthConstWrite`**；正确性 = `depthTest=false` + `cull=None` + 表面 pass 在前 + `DepthState` 至少一个写位为 true」。
- §10.3（`:466`）删除「`depth_const` 一律从 `StackRenderDesc::inner_depth_const` 取」；§13.1（`:588`）删除 `--inner-depth-const`，`:597` 整条删除；§13.4（`:625`）删除对应参数错误行。
- §10.4 第 5 条（`:474`）与 §1.4 结论 2（`:119`）：**必须修正 T/E 互斥**——要么本轮重跑探针得到自洽结果，要么把两条都标为「在未启用特性上观测，不可复现，不作判据」。不能同时保留。
- 计划 §7.2（`:239`/`:245`）、§10.2（`:353`）同步删除 `depthConstWrite` 与 `--inner-depth-const` 的落地要求。
- 计划 R2（`:429`）升级为「**高**」，缓解改为「本玩法**零使用** `depthConstWrite`；`depthBounds` 未启用即不使用（`vulkan_render_device.cpp:150,157`）」。

### M3【必改·事项 2 附】把「`feats{}` 全 0」登记为设备级风险
- 计划 §2.C 表与 §13 风险表新增一条：「L0 设备创建未启用任何 `VkPhysicalDeviceFeatures`（`vulkan_render_device.cpp:150,157`），使 `depthClamp`/`fillModeNonSolid`/`wideLines`/`logicOp`/`depthBounds` 等默认 false 特性全部不可用。F-VX **不得**依赖其中任何一项；若将来需要，**必须**走「给 `feats` 置位」的 L0 主路径，不得绕行。」
- 在计划 §7.5（`:265`）把「不要为它去改后端」升级为「**禁止**任何 `depthConstWrite`/`depthBounds` 相关的后端改动，本玩法不依赖」。

### M4【必改·事项 1 附】给 D-VX-8′ 补回归护栏
- 新增一条**设备态用例**（放进阶段 3，替换或补充 `StackRender.PassPairIsDistinct`）：在 `with_depth=true` 的离屏目标上，用**公共** `create_pipeline(desc)` 建两条管线、各录一个 pass、提交一次、`readback_rgba8`，断言**前景像素占比 > 0**。
- 理由写进计划 §2.B：现存 `tests/` 无 `with_depth=true` 用例，`PixelRegression.*` 用 `with_depth=false`（`magicblocks_l3_pixel_regression_test.cpp:71`），`blocks`/`gizmo` 无 CTest 像素断言 —— 这条用例是「不动 L1」这一重大裁定的**唯一回归护栏**。
- 同时**删除**计划 §2.B `:57` 的「389 条测试含其 golden 断言」表述，改为如实描述：「`sketch` 有进程级 e2e（`magicblocks_l4_sketch_test.cpp:471-511`）；`blocks`/`gizmo` 无 CTest 像素断言；`has_depth=true` 公共面路径当前无覆盖 → 本阶段补。」

### M5【必改】修正门禁口令（F1，假绿风险）
- 设计 `:189`、计划 `:120`、计划 `:126-127` 的口令改为带 `--root`：
  `python tools/check_l3_no_gameplay_names.py --root MagicBlocks`
  `python tools/check_layer_purity.py --root MagicBlocks`
- 阶段 0 验收（计划 `:125`）**必须**断言输出里的 `扫描 N 个文件` 中 N > 0，否则视为门禁未生效。
- 设计 `:203` 的「标识符白名单只允许 `block`/`blocks` 出现在已登记上下文」改为如实描述：「`ALLOWED_IDENTIFIERS = {block, blocks}` 按词无条件放行（`check_l3_no_gameplay_names.py:59-62`）」。

### M6【必改】修正 A7 的可验证性
- A7（设计 `:676`）改写为两条可执行的：
  1. `StackRender.PassPairIsDistinct`：两个 pass 的 `IPipeline::hash()` 不同（公共面，`vulkan_pipeline.h:79`）。
  2. `StackRender.BothPassesRenderOnDepthTarget`：在 `with_depth=true` 目标上两 pass 各自出前景像素（即 M4 的用例）。
- 删除「附件形状 `has_depth == true`」的公共面断言，并在设计 §15 加注：「`attachment_shape()` 仅后端可查（`vulkan_pipeline.h:88-89`），公共面不可断言；侧证由 M4 的像素用例承担。」
- 计划 `:258` 的 `BothPipelinesBuiltViaPublicApi` 保留（它验证的是「没调 `create_pipeline_for_shape`」，可用 grep 门禁或代码审查落实），但需在计划里**明确它不做 shape 断言**。

### M7【必改】统一 Block 大小口径（3.1）
- 设计 `:239`/`:245`：二选一**写死**。建议 **24 B**（`Cell` 12 + `color` 4 + `stroke_id` 4 + `flags` 4），并把 `static_assert(sizeof(Block) == 24)` 的注释改为「与 L3 §6A.1 的 `int32 cell[3]` + 3×u32 字段布局一致（`Cell` 只是 `cell[3]` 的具名视图，不增减字节）」。
- **删除**「若 L3 的 `Block` 已对外暴露则复用其定义」——L3 无此类型（L3 spec `:939`，依赖报告 `:149-155`），这是不存在的退路。
- 计划 `:146` 同步改为 24 B，并补 `BlockSet.SizeOfBlockIs24` 的期望值说明。

### M8【必改】修正 overlay 依赖登记与 verify_png 的接法（3.6 / F4）
- 设计 §3.4 表（`:209-215`）新增一行：「需要世界→屏幕投影？不需要新增。`ICamera::world_to_screen(p, px, py, depth)` 已存在（`camera.h:89`），`IOverlay2D::LineDesc` 用像素坐标（`overlay2d.h:33-39`）。」
- 计划 `:253`/`:285`/`:360` 的 `verify_png_content.py` 调用必须**改为 CTest 可执行形式**：要么注册成 `add_test(NAME magicblocks_cli_stack_png ...)`（仿 `CMakeLists.txt:484-488` 的 `magicblocks_cli_render_png`），要么在 gtest 内用 `std::filesystem` 读 PNG 头 + 统计前景像素（仿 `magicblocks_l4_sketch_test.cpp:460-463` 的 `is_png` + `:484` 的 size 门槛）。**不得**依赖开发者手工执行脚本。
- 明确 `verify_png_content.py:140-146` 的「跨度 >20%」判据对 F-VX 构图是否适用；不适用则在计划里删掉该项，改为「非背景像素占比 > 1%」。

### M9【应改】范围措辞与实例边界（3.4 / 3.5）
- 计划 §0（`:15`）与 §12（`:420`）改口径：「**L0–L3 库与 SDK 公共面零改动**；`l3_cli/main.cpp` 仅新增 `cmd_stack()`、分派行（仿 `:2006-2013`）与 `print_usage` 条目三处，**不改**既有子命令行为。」
- 新增用例与实施要点（计划 §7.2 `:241-244`）：明确「pass A/pass B 两个 batch 的 `instance_count` 均须 > 1（`primitive_renderer.cpp:102` 仅在该条件下绑定 binding 1）；`column_height=1` 且单格笔迹时须断言 `instance_count > 1` 或走非实例化路径」。补一条用例 `StackRender.SingleInstanceBatchStillBindsInstanceBuffer`。

### M10【应改】计划 §3 用例表与阶段 7 确定性（3.8 / 3.9 / 3.10）
- 计划 §3（`:99-108`）表头「结束后用例数」改为「预计（阶段 0 实测基线 + 本阶段实测增量）」，并删掉 `:125` 的占位符 `381`，改为「记录实测数，阶段 0 结束时回填」。
- 阶段 7（`:385-387`）补一段：`stroke_id` / `lock` 语义如何映射到 `EventRecord`（`command_stream.h:100-112`）；并明确「三态像素一致依赖 §6.2 的 1e-4·cell_size 量化是**确定性**而非容差（设计 `:353`）」。
- R5（`:432`）二选一并写死：
  - (a) 「双平台**同一套** golden 逐字节比对」——则须先论证 SwiftShader/lavapipe 像素逐字节一致（大概率不成立）；
  - (b) 「同一套**判据** + 平台无关的**统计容差**（前景占比/亮度跨度），golden 只锁**结构量**不锁字节」。
  建议 (b)，并在设计 §15 A10（`:679`）把「两次运行逐字节相同」限定为**同平台同驱动**内的确定性要求（这与 build-pipeline 的「双平台同一套测试」不冲突）。

---

## 7. 硬约束门禁结论

| 硬约束 | 是否违反 | 备注 |
|---|---|---|
| CLI 模式 **+** 离屏渲染输出图像 | **否** | 子命令 `stack` + `with_depth=true` 离屏目标 + `readback_rgba8` + `write_png_rgba8` + 双跑逐字节，全部落到可执行条款（设计 §13 / 计划阶段 6）；有现成 e2e 范本 `magicblocks_l4_sketch_test.cpp:471-511` |
| 不得设计回退 / 兜底路径 | **否** | 无替代库、无预编译产物、无降级跳过；`--dry-run` 为功能开关且明确声明非降级（设计 `:617`）；异常路径一律「停工如实报告」（计划 `:63`） |
| 不得在 L0–L3 开玩法专用通道 | **否** | L0–L3 库零改动；仅新增 `features/stack/` + `l3_cli` 新子命令；G7 只扫 `primitives/`（`check_l3_no_gameplay_names.py:29`），`stack` 不在禁用词表（`:34-45`）。**但「零改动」措辞与 `l3_cli/main.cpp` 变更清单互斥，见 M9** |

→ **不触发「违反硬约束整份打回」**；打回仅基于五维总分 81.70 < 90。

---

## 8. 重审前置条件（Checklist）

打回修订后请确认：

- [ ] M1：设计 §10.8 / §18 / D-VX-8 / §10.2 四处回改，P0-1 已删除
- [ ] M2：设计 §10.1/§10.3/§10.4/D-VX-4/§13.1/§13.4 与计划 §7.2/§10.2 中 `depthConstWrite` 相关内容已移除；T/E 矛盾已消解或降级为不可复现观测
- [ ] M3：设备级 `feats{}` 全 0 风险已登记
- [ ] M4：`with_depth=true` 公共面像素回归用例已加入阶段 3；`389 条测试含 golden 断言` 的错误表述已删除
- [ ] M5：门禁口令统一为 `--root MagicBlocks`，并断言扫描文件数 > 0
- [ ] M6：A7 已拆成两条公共面可执行断言
- [ ] M7：`Block` 大小唯一口径（建议 24 B），「复用 L3 定义」退路已删除
- [ ] M8：`verify_png_content` 以 CTest/gtest 形式接入；跨度判据适用性已说明
- [ ] M9：范围口径改为「库与公共面零改动 + `l3_cli` 三处」；`instance_count==1` 边界已覆盖
- [ ] M10：用例数表改为「实测回填」；阶段 7 补 `stroke_id`→`EventRecord` 映射与量化确定性；R5 跨平台 golden 策略已写死

> 预计修订后可达：架构 93–95、需求 94–96、技术 88–92、测试 88–92、风险 85–90 → 加权 **90.6–93.5**，可过门。
