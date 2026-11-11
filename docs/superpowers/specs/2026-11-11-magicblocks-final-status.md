# MagicBlocks 最终状态（整链真实完成状态）

- 日期：2026-11-11
- 作者：survey（t13）
- 取数构建目录：`MagicBlocks/build-survey`（由 `tools/build.ps1` 驱动，`vcvars64` + ninja + MSVC）
- 取数时刻：本文件写作时的**同一棵工作区**（未提交改动全量在位），非历史快照
- 性质：只读核实 + 归档。本文不改任何引擎/测试/CMake/门控代码

> 纪律声明：本文所有数字均为**原样粘贴**的命令输出，不做估算、不做汇总推算。
> 每个失败用例按**用例名去重**（gtest 会把同一失败打印两次，直接数 `[  FAILED  ]` 行会翻倍）。
> 「构建阻断」与「测试红条」是两类完全不同的事实，本文全程分列，**绝不混算**。

---

## 0. 一句话结论

- **构建：绿**。`tools/build.ps1 -BuildDir build-survey` 退出码 `0`，`[52/52] Linking CXX executable magicblocks_test.exe`，10 个引擎可执行文件齐备。
- **G8 分层门控：绿**。`G8 通过: L3 与 backend 完全解耦(扫描 36 个文件)`，退出码 `0`。
- **测试：红 11 条（gtest）/ 红 14 条（CTest）**，其中 11 条 gtest 红条全部落在 **Sky ×9 + MSAA ×2**，另有 3 条 CTest 红条是 **UI CLI 的资源根解析问题（用例环境错误，非产品缺陷）**。
- 相较上一轮基线（583 用例 / 571 绿 / 12 红），本轮 **+2 用例、+3 绿、-1 红**：MSAA 从 4 红降到 2 红（`L3_ScenePipelineEdgesSmooth`、`DefaultPath_FskExtrusionIsSmooth` 已转绿），但 Sky 新增 1 红（`Sky.Errors_RejectMissingCubeAndBadSampler`）。

---

## 1. 全量数字（按用例名去重）

### 1.1 gtest（`magicblocks_test.exe`）

原始输出行：

```
[==========] Running 585 tests from 89 test suites.
[==========] 585 tests from 89 test suites ran. (16292 ms total)
[  PASSED  ] 574 tests.
[  FAILED  ] 11 tests, listed below:
```

| 项 | 数值 |
|---|---|
| 运行用例数 | 585 |
| 测试套件数 | 89 |
| 通过 | 574 |
| 失败（**按用例名去重**） | 11 |
| 算术自检 `585 == 574 + 11` | 通过 |
| 进程退出码 | 1（有失败，符合预期语义） |

去重方法：正则抓 `^\[  FAILED  \] (\S+\.\S+)` 全部匹配，按名字计数。原始匹配 **22 条**，唯一 **11 条**，每条恰好出现 `2x`（gtest 的"失败详情 + 末尾清单"双打印）。不使用去重就会把 11 条误报成 22 条。

### 1.2 CTest（`ctest --output-on-failure`，`build-survey`）

原始输出行：

```
98% tests passed, 14 tests failed out of 608
```

| 项 | 数值 |
|---|---|
| CTest 注册用例总数 | 608 |
| 失败 | 14 |
| 通过率 | 98% |
| 进程退出码 | 8 |

**CTest 14 ≠ gtest 11 的对应关系**（这是最容易算错的地方）：
CTest 把每个 gtest 用例注册成独立 test，因此 11 条 gtest 红条在 CTest 里同样计 11 条；**多出的 3 条是 CTest 层独有的 shell/CLI 用例**，gtest 里根本没有：

```
magicblocks_cli_ui_render_png
magicblocks_cli_ui_stats
magicblocks_cli_ui_present
```

11 + 3 = 14，闭合。两个总数属于两套不同的计数体系，**不可相加**，也不可把 14 当作"引擎红条数"。

---

## 2. 红条分组清单

### 2.1 分组汇总

| 分组 | 条数 | 归属 | 性质 |
|---|---|---|---|
| Sky | 9 | skybox / L3 | 真缺陷 |
| MSAA | 2 | msaa / L1+L3 | 真缺陷（已在收敛） |
| 其它（UI CLI，仅 CTest） | 3 | ui / 用例环境 | 用例错误（非产品缺陷） |
| **合计（CTest 口径）** | **14** | | |
| **合计（gtest 口径）** | **11** | | |

### 2.2 MSAA 组（2 条，全部为 gtest + CTest 双计）

| # | 用例名 | 标注 | 证据 / 说明 |
|---|---|---|---|
| 1 | `MsaaSwapchain.CurrentTargetFollowsAcquiredImage` | **真缺陷** | `magicblocks_msaa_test.cpp` 断言失败。属 L1 交换链"当前目标跟随已获取图像"语义未落实 |
| 2 | `MsaaPixel.DefaultPath_UiOffscreenHasMsaaSwitch` | **真缺陷** | 断言点 `magicblocks_msaa_pixel_test.cpp(891)` 直接 `ADD_FAILURE`。UI 离屏合成路径仍未暴露 MSAA 开关（`t2` 根因文档 P0-1 指出的入口缺失） |

**本轮 MSAA 的净进展（重要）**：上轮 4 红里的下列两条**已转绿**，实测 `[       OK ]`：

```
[       OK ] MsaaPixel.L3_ScenePipelineEdgesSmooth (191 ms)
[       OK ] MsaaPixel.DefaultPath_FskExtrusionIsSmooth (193 ms)
```

> 读数陷阱：这两条用例名在日志里**各出现两次**（`[ RUN      ]` 行 + `[       OK ]` 行）。
> 用"首次出现位置向后截取"的方式判读会误报为 FAIL。本文按 `[ RUN ]` 起点到下一个 `[ RUN` 之间的
> 完整片段判定，确认为 OK。

### 2.3 Sky 组（9 条，全部为 gtest + CTest 双计）

| # | 用例名 | 标注 | 直接原因 |
|---|---|---|---|
| 1 | `Sky.DiffCoveringBackgroundAgainstClear` | 真缺陷 | 见 §2.3.1 共同根因 |
| 2 | `Sky.TranslationInvariant` | 真缺陷 | 同上 |
| 3 | `Sky.RotationChangesPixels` | 真缺陷 | 同上 |
| 4 | `Sky.Deterministic` | 真缺陷 | 同上 |
| 5 | `Sky.SixDirectionsShowDifferentFaces` | 真缺陷 | 同上 |
| 6 | `Sky.BackgroundIsNotSolidColor` | 真缺陷 | 同上 |
| 7 | `Sky.RealAssets_CubeMapLoadsAndRenders` | 真缺陷 | 同上 |
| 8 | `Sky.RealAssets_ToggleDiffCoversBackgroundBox` | 真缺陷 | 同上 |
| 9 | `Sky.Errors_RejectMissingCubeAndBadSampler` | **真缺陷（本轮新增红）** | 同上；且本次红的是**正向对照**断言 |

#### 2.3.1 共同根因（单用例复跑实测，非推测）

单跑 `--gtest_filter=Sky.Errors_RejectMissingCubeAndBadSampler` 得到的**真实失败消息**：

```
E:\...\tests\magicblocks_l3_sky_test.cpp(593): error: Value of: good_status.ok()
  Actual: false
Expected: true
合法描述必须能建出管线;实际: VulkanBuffer::upload: staging 分配失败: 内存不足: 需要 12 字节
(需 65536 字节 64 KiB 槽位);已有 75507648 字节;预算 2147483648 字节 [桶 Staging];当前页无空位
—— 请先 reserve_pages() 预取整页
```

判定链条：

1. `SkyPass::prepare()` 需要上传一块 **12 字节**占位顶点缓冲（`src/primitives/sky.cpp:362-386`）。
2. L1 分页分配器的契约是：`allocate()` **永远不自己触发 VMA**，桶内 `first_fit` 失败即返回
   `OOM("...当前页无空位 —— 请先 reserve_pages() 预取整页")`（`src/backend/vulkan/vulkan_allocator.cpp:859-868`）。
   这是**设计好的硬契约**，不是 bug。
3. `sky.cpp` 已经遵守契约，在 `prepare()` 里调了 `reserve_pages(Staging, 1)`。
4. 但 `tests/magicblocks_l3_sky_test.cpp:565` 的 `reserve_pages(*d.value(), 1)` 只预取了 **1 页**，
   而该用例对同一个 `SkyPass` 连续做了 **3 次** `prepare()`（面未上传 → 非法采样 → 合法描述），
   每次都要求一块新的 12 字节 Staging 槽位。页被切满后第 3 次失败。
5. 因此**红的是正向对照** `good_status.ok()`——"合法描述必须能建出管线"没通过；
   两条负向断言（未上传面必须 `Unsupported`、`Repeat` 必须 `InvalidArgument`）反而通过了。
   这类"负向绿、正向红"最容易误读成"错误处理做对了"，实际是**容量预算不足**。

> 补充事实：`t10`（skybox）交付时这条用例在 `build-skybox` 里是绿的（其 output 明确列出
> `[ OK ] Sky.Errors_RejectMissingCubeAndBadSampler`）。本轮转红说明**后续改动改变了每用例的
> 页预算或 `prepare()` 的分配次数**，属回归而非"从未绿过"。

**修复方向（不在本任务范围内，仅供 owner 排期）**：把 Sky 测试的 Staging/Vertex/Texture 预留量
按"本用例实际 prepare 次数 × 每次所需槽位"给足（或让 `SkyPass` 复用占位缓冲而非每次新建），
不要靠单页兜底。断言本身不应放宽。

### 2.4 其它组（3 条，仅 CTest 计，gtest 无对应）

| # | CTest 用例名 | 标注 | 直接原因 |
|---|---|---|---|
| 1 | `magicblocks_cli_ui_render_png` | **用例错误** | `扫描模型目录失败(assets/res/models): scan_model_dir: 目录不存在` |
| 2 | `magicblocks_cli_ui_stats` | **用例错误** | 同上 |
| 3 | `magicblocks_cli_ui_present` | **用例错误** | 同上 |

#### 2.4.1 根因（已实证，非产品缺陷）

- `CTestTestfile.cmake` 中这 3 条 `add_test` **未设置 `WORKING_DIRECTORY`**，CTest 默认用构建目录
  `build-survey` 作为 CWD。
- `src/ui_cli/main.cpp:209-213` 的 `resolve_asset_root()` 回退值是**相对路径** `"assets/res"`
  （优先级：`--asset-root` > 环境变量 `MB_ASSET_ROOT` > `"assets/res"`）。
- 于是 CLI 去找 `build-survey/assets/res/models` —— 该目录**不存在**；真实资源在
  `MagicBlocks/assets/res/models`（`bunny.obj`、`cube.obj`、`panel.obj`、`sphere.obj`、`teapot.obj`）。
- CLI 在目录缺失时**故意 exit 2 并明确报错**（`src/ui_cli/main.cpp:331-343` 的注释写明：空列表与
  "目录找不到"在界面上是两件完全不同的事，不静默退化）。**这个行为是对的，不该改**。

**反证实验**：在仓库根（`assets/res` 可解析处）手动跑同一条命令：

```
magicblocks-ui-cli.exe render --width 320 --height 240 --output build-survey\cli_out\ui_probe.png
→ exit 0
→ wrote build-survey\cli_out\ui_probe.png (320x240)
```

即：**产品功能正常，红的是 CTest 的 CWD 装配**。修复只需给这 3 条 `add_test` 补
`WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}`（或显式传 `--asset-root`），属 UI/构建 owner 的
CMake 改动，本任务不越界实施。

#### 2.4.2 同一实验暴露的**真实**缺陷（CTest 没覆盖到，必须单列）

上面那次 exit 0 的跑批里，缩略图全部失败：

```
缩略图渲染失败[bunny]: 创建实例缓冲失败: VulkanBuffer(Vertex size=112): 内存不足: 需要 112 字节
(需 65536 字节 64 KiB 槽位);已有 8706176 字节;预算 2147483648 字节 [桶 Vertex];当前页无空位 —— 请先 reserve_pages() 预取整页
缩略图渲染失败[cube] / [panel] / [sphere] / [teapot]: 同上
缩略图: 0/5 成功(48x36)
缩略图合成: 5 条
```

- 这是**左列表"真实模型缩略图"在 0/5 成功**的情况下仍然 exit 0 —— 即"缩略图全失败"对退出码
  不可见。它与 §2.3.1 是**同一个分配器契约**（Vertex 桶未预取页），但发生在 UI 侧装配代码里。
- 因为 CTest 那 3 条本来就先挂在资源根上，这个缺陷**至今未被任何门控覆盖**，属"绿了但没验"。
- 归属：ui（左列表缩略图）与 t12 直接相关，建议 t12 收尾时一并处理并加断言。

---

## 3. 构建阻断 vs 测试红条（严格分列）

| 维度 | 构建阻断 | 测试红条 |
|---|---|---|
| 定义 | 编译/链接失败，产不出可执行文件 | 可执行文件产出正常，跑起来断言失败 |
| 本轮事实 | **无**。退出码 0，`[52/52] Linking CXX executable magicblocks_test.exe` | gtest 11 / CTest 14（见 §1） |
| 是否影响取数可信度 | 是（有阻断时测试数字无意义） | 否（数字即事实） |
| 历史说明 | 本轮中途出现过 `C1083: 无法打开包括文件 "cstdint"`，根因是**绕过 `tools/build.ps1` 直调 `cmake --build`**，未导入 vcvars64 的 INCLUDE。改回脚本驱动后消失。**这是取数手法错误，不是代码错误** | — |

**结论：本轮不存在构建阻断，因此 §1 的测试数字是可信的。**

另需注意：历史 issue 中曾出现 `LNK2019 unresolved external VulkanPipeline::VulkanPipeline(...)`，
根因是 `.ninja_deps`/`.ninja_log` 与头文件改动不同步导致目标文件陈旧。本轮已通过脚本全量重建
消除，**当前无此问题**。

### 3.1 可执行文件清单（构建完整性的旁证）

`build-survey` 下 10 个引擎可执行文件全部在位：

```
magicblocks-cube-demo.exe    magicblocks-fsk-cli.exe     magicblocks-l0-cli.exe
magicblocks-l2-cli.exe       magicblocks-l3-cli.exe      magicblocks-model-cli.exe
magicblocks-sink-verify.exe  magicblocks-sky-cli.exe     magicblocks-ui-cli.exe
magicblocks_test.exe
```

---

## 4. G8 分层门控（本轮重点修复项）

### 4.1 当前结果

```
G8 通过: L3 与 backend 完全解耦(扫描 36 个文件)
退出码 0
```

（调用方式：仓库根 `python tools\check_l3_command_parity.py`，脚本默认 `--root MagicBlocks`。
注意脚本位于**仓库根** `tools/`，不在 `MagicBlocks/tools/`。）

### 4.2 门控仍然有效（变异验证，防"假绿"）

为排除"门控恰好通过"的假绿，做了一次注入-还原实验：

1. 在 `src/primitives/shader_library.cpp` 注入一行
   `#include "backend/vulkan/vulkan_render_device.h"`；
2. 重跑门控 → **退出码 1**（门控成功捕获）；
3. 逐字节还原，校验 `restored: True`（文件与原始内容完全一致）。

即：**G8 的绿是真实解耦，不是扫描失效。** 门控脚本本身未做任何修改。

### 4.3 与历史红条的关系

历史 G8 报据为 `src\primitives\shader_library.cpp:19` 与 `:20` 两处 include backend 头。
当前该文件的 include 全量为：

```
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>
#include "magicblocks/primitives/shader_library.h"
```

**零个 backend 头**，且全文件不含 `backend/`、`vulkan`、`vma` 字样。分层违规已消除。

### 4.4 必须同时记录的副作用（诚实披露）

当前 `shader_library.cpp` 走的是 `device_->create_pipeline(make_pipeline_desc(*tmpl, raster, shape))`
（`shader_library.cpp:186-187`），**并未走** `resources::IPipelineFactory::create_shaped_pipeline()`
依赖倒置路径。

- 也就是说：L1 侧新增的 `AttachmentShape` / `IPipelineFactory` / `register_pipeline_factory` /
  `resolve_pipeline_factory` / `src/core/pipeline_factory_table.cpp` 与
  `vulkan_render_device.h::pipeline_factory()` 覆写这一整套机制，**当前在该调用点上未被使用**。
- 分层目标（L3 不依赖 backend）**已达成**；但"L3 通过工厂向 backend 请求按形状建管线"这条
  **目标架构路径尚未接通**。两者不可混为一谈。
- 影响面：`MsaaPixel.L3_ScenePipelineEdgesSmooth` 与 `DefaultPath_FskExtrusionIsSmooth` 已转绿，
  说明 MSAA 的收敛来自其他改动，**不能归功于**这条工厂链路。
- 归属：captain 已裁定 g8 修复走 A 方案；是否需要把该调用点真正切到工厂，需要 captain 另行定夺。

---

## 5. t1~t16 交付物索引

| 任务 | 负责人 | 状态 | 交付物指向 |
|---|---|---|---|
| t1 | survey | completed | [F-SK/F-VX 完成度审计](2026-11-11-magicblocks-fsk-fvx-completion-audit.md)。P0 五项 / P1 九项 / P2 七项 + 11 条假绿清单 |
| t2 | msaa | completed | [MSAA 根因](2026-11-11-magicblocks-msaa-root-cause.md)。H1-H7 假设 + 行号级证据；P0-1 = UI 消费层无 MSAA 入口 |
| t3 | msaa | completed | MSAA 像素级回归用例；全仓 `GTEST_SKIP` 归零（15 处调用点删除，缺能力一律 FAIL 不 SKIP） |
| t4 | fsk | completed | A1 画布+输入链路（`sketch_canvas.h/.cpp`）、A3 双表示下沉 L3（`mesh_factory.h` 的 `PathStyle`/`OutlineSet`）、B1 `magicblocks-fsk-cli`、B2 F-SK golden（`golden_fsk_{persp,ortho,region}.png` + `scripts/fsk_golden.py`）、B3 锁层真走相机 |
| t5 | ui | completed | `tools/png_inspect.py`（纯标准库 PNG 解码+像素分析）、`tools/tests/test_png_inspect.py`（61 用例）、`docs/PNG_INSPECT.md` |
| t6 | msaa | **failed** | 按 captain 裁定 (b) 收尾。未全部达标：11 绿 / 3 红，另有 R6 明确标注未实现。零降级、零放宽断言 |
| t7 | qa | completed | 全量回归门第一批：606 条，593 绿 / 13 红，89.04 s。日志 `build-captain/qa_t7/ctest_full.log` |
| t8 | survey | completed | [UI 资源清单与采样策略](2026-11-11-magicblocks-ui-assets-inventory.md)。六面 PNG 54/54 解码通过；cubeMap/HDR/模型可行性结论 |
| t9 | ui | completed | UI 布局 v1：`hud.h` 的 `ControlKind::ListItem` + `SidebarConfig` + `build_sidebar_hud()`；UI 套件 56/56，UIG1/UIG2/G3/G7 四门全绿 |
| t10 | skybox | **failed** | 引擎侧能力已落地（`SkyCubeMap.*` 实测绿），但**像素级验收未达成**，且过程中造成过一次共享文件事故 |
| t11 | skybox | completed | L3 模型目录接口（`model_catalog.h/.cpp`）、`magicblocks-model-cli`、`tools/verify_model_renders.py`、`tools/fetch-assets.ps1` |
| t12 | ui | in_progress | UI 布局 v2：左列表面板接真实模型缩略与选择、接天空盒 |
| t13 | survey | 本文件 | 最终状态文档 |
| t14 | qa | pending | 全量回归门第二批（依赖 t4/t9/t12/t13） |
| t15 | fvx | **failed** | V1 状态机 / V2 网格指示接线 / V3 真实相机锁层代码完成；V4 golden 与端到端验收被共享构建树红阻断。**重要更正**：t1 审计的 GAP-10（dry-run 与全跑 doc_hash 不一致）在当前源码下**已不成立**（实测两者同为 `18afbcae00c67113`） |
| t16 | msaa | completed | L3 稳态零 VMA（R-SK-11/A11）：`IPrimitiveRenderer::draw_mesh_instanced` + 稳态分配用例；`--gtest_filter=SteadyState*:Renderer*:Sketch*` → 51 tests / 51 PASSED |

### 5.1 新增能力用例（本构建实测运行数）

| 套件前缀 | 用例数 |
|---|---|
| `Canvas*` | 19 |
| `ModelCatalog*` | 14 |
| `PathStyle*` | 8 |
| `GridOverlay*` | 8 |
| `StackSession*` | 7 |
| `StackGolden*` | 6 |
| `SteadyState*` | 4 |
| `SkyCubeMap*` | 2 |

---

## 6. 已知未达成项

按"是否被门控覆盖"分级。**未达成 ≠ 红条**：以下多数项当前是绿的，只是没验到该验的东西。

### 6.1 有红条直接指认

1. **UI 离屏 MSAA 开关未暴露** —— `MsaaPixel.DefaultPath_UiOffscreenHasMsaaSwitch` 红。t2 的 P0-1 结论未闭环。
2. **L1 交换链目标跟随语义** —— `MsaaSwapchain.CurrentTargetFollowsAcquiredImage` 红。
3. **Sky 像素链路整体未验收** —— 9 条 Sky 红，共同根因是测试侧 Staging/Vertex 页预算不足（见 §2.3.1），修复后才能真正检验 skybox。t10 要求的"像素级验收"至今**未达成**。

### 6.2 绿的但覆盖不足（"假绿"风险）

4. **UI CLI 三条 CTest 挂在资源根 CWD 上**（§2.4）—— 产品功能正常，但门控口径错，需补 `WORKING_DIRECTORY`。
5. **左列表缩略图 0/5 成功仍 exit 0**（§2.4.2）—— 真实缺陷，无任何门控覆盖。归 t12。
6. **依赖倒置工厂链路未接通**（§4.4）—— L3 仍直调 `device_->create_pipeline()`。分层目标达成，"按形状建管线经工厂"的目标架构未达成。
7. **`shader_library.cpp` 的补丁形态与历史描述不符** —— 历史记录称该文件曾含 backend include / `AttachmentShape` 字段访问，当前文件两者皆无，且与 git HEAD 内容一致（仅编码/行尾呈现差异）。任何基于"该文件已被改写"的后续假设都需重新核实。

### 6.3 任务级未达成

8. **t6 未全部达标**（状态即 failed）：MSAA 按现状收尾，R6 明示未实现。
9. **t10 未达成像素级验收**（状态即 failed），并有共享文件事故记录。
10. **t15 未达成端到端验收**（状态即 failed）：无画布、无鼠标/触摸事件到 `IStackSession` 的桥、无实时窗口；用户可操作入口仍不存在，只有 `magicblocks-l3-cli stack` 离线出图。
11. **t12 UI 布局 v2 仍 in_progress**：左列表真实模型缩略与选择、接天空盒。
12. **t14 全量回归门第二批未执行**（pending，依赖 t13 等）。

### 6.4 明确为"非缺陷/不改"

- `scan_model_dir` 在目录缺失时报错并 exit 2 —— **正确行为**，不得改成静默空列表。
- `VulkanAllocator::allocate()` 桶满即报 `当前页无空位 —— 请先 reserve_pages()`，不在热路径向 VMA 要页 —— **L1 既定契约**，不得为"让测试变绿"而改成自动长页。

---

## 7. 取数方法与可复现命令

```powershell
# 1) 构建（必须经脚本，否则缺 vcvars64 会假报 C1083）
cd E:\workspace\myspace\Mine\MagicBlocks
powershell -NoProfile -ExecutionPolicy Bypass -File tools\build.ps1 -BuildDir build-survey
#   → exit 0，[52/52] Linking CXX executable magicblocks_test.exe

# 2) G8 分层门控（脚本在仓库根 tools/，默认 --root MagicBlocks）
cd E:\workspace\myspace\Mine
python tools\check_l3_command_parity.py
#   → G8 通过: L3 与 backend 完全解耦(扫描 36 个文件) ，exit 0

# 3) 全量 gtest
cd E:\workspace\myspace\Mine\MagicBlocks
build-survey\magicblocks_test.exe --gtest_output=xml:build-survey\gtest.xml > build-survey\gtest-run.log 2>&1
#   → 585 / 574 / 11

# 4) 全量 ctest
cd E:\workspace\myspace\Mine\MagicBlocks\build-survey
ctest --output-on-failure > ..\build-survey-ctest.log 2>&1
#   → 98% tests passed, 14 tests failed out of 608

# 5) 单条复跑（拿到真实断言消息，验证日志过滤时不要滤掉 vk- 行）
build-survey\magicblocks_test.exe --gtest_filter=Sky.Errors_RejectMissingCubeAndBadSampler
```

日志文件：`MagicBlocks/build-survey-build.log`、`build-survey-gtest.log`、`build-survey-ctest.log`、
`build-survey/gtest-run.log`、`build-survey/sky1.log`。

### 7.1 读数陷阱清单（供后续 QA 复用）

1. **gtest 双打印**：每条失败打印两次，必须先按用例名去重再计数（否则 11 → 22）。
2. **用 `[ RUN ]`/`[ OK ]` 双出现**：用"名字首次出现"定位会误判大量 OK 用例为 FAIL。
3. **gtest 与 CTest 不可相加**：608 与 585 是两套体系，14 与 11 的差是 3 条 CTest 独有 CLI 用例。
4. **CTest 门控不是 gtest 用例**：G8 这类脚本门控只出现在 CTest 层，不能并入 gtest 红条。
5. **UTF-8 日志**：仓库内日志为 UTF-8，PowerShell 默认按 GBK 读会乱码；用 `Get-Content -Encoding utf8`
   或 Python `decode("utf-8", "replace")`。
6. **`vk-ERROR` 噪声**：`VK_MEMORY_ALLOCATE_DEVICE_ADDRESS_BIT` 校验告警在几乎每条 GPU 用例里都刷屏，
   但它**不是**失败原因；过滤日志时别把它当结论，别把它滤掉之后又看不见真正的 `error:` 行。

---

## 8. 小结表

| 项 | 结论 |
|---|---|
| 构建 | 绿（exit 0，52/52，10 exe 齐备），**无构建阻断** |
| G8 分层门控 | 绿（36 文件，变异验证有效） |
| gtest | 585 跑 / 574 绿 / **11 红**（去重后） |
| CTest | 608 跑 / 594 绿 / **14 红** = 11 gtest + 3 CTest 独有 |
| 红条分布 | Sky 9（真缺陷，测试侧页预算） + MSAA 2（真缺陷，收敛中） + UI CLI 3（用例环境） |
| 相对上轮 | 用例 +2、绿 +3、红 −1；MSAA 4→2（两条转绿），Sky 8→9（新增 1 条正向对照红） |
| 最大风险 | 左列表缩略图 0/5 成功却 exit 0，**无门控覆盖**；工厂依赖倒置链路未接通 |
