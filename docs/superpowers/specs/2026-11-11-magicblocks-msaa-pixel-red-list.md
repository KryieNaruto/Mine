# MSAA 像素级回归：红灯清单与 t6 修复映射（t3 交付）

日期：2026-11-11
定位依据：[2026-11-11-magicblocks-msaa-root-cause.md](2026-11-11-magicblocks-msaa-root-cause.md)（t2）
本文件负责：把 t2 的**静态结论**变成**会红的用例**，并把每条红灯映射到 t6 的具体修复项。

---

## 0. 证据时效性声明（captain 明确要求区分）

| 数据 | 来源二进制 | 构建时间戳 | 可信度 |
|---|---|---|---|
| §2 的 9 绿 / 5 红结论 | `MagicBlocks/build/magicblocks_test.exe`，含**我的两处测试修正**（cull=None、ui-cli 回退路径）之前的那一版 | 2026-11-11 10:57（`magicblocks_test.exe` 长度 3,635,200） | **逻辑有效、数值待复核**：那版二进制里 `L3_ScenePipelineEdgesSmooth` 的 `覆盖率 0` 是**我的测试 bug**（剔除导致场景为空），已修；修完没有再跑成功（见 §3）。 |
| 5 条红灯的**性质**（缺入口 / 缺深度附件 / 场景空） | 同一版 + 源码静态核对 | 同上 | 高（有源码行号，不依赖二进制） |
| 全量 ctest 基线 | 拆除前旧二进制 | 2026-11-11 10:41 | **不可用于最终验收** |

> ⚠️ **最终验收必须先 `ninja -t clean` 全量重建再跑**（ARCHITECTURE §10.12：
> 一旦出现过编译失败，之后的增量构建结果一律不可信）。本节所有数值在重建后要重跑一遍。

---

## 1. 用例总表（14 条，全部像素级 / 真实光栅 + 真实提交 + 真实读回）

文件：`MagicBlocks/tests/magicblocks_msaa_pixel_test.cpp`（+ 纯测试头 `tests/msaa_pixel_shared.h`）

| # | 用例 | 守的东西 | t2 假设 | 状态（旧二进制） |
|---|---|---|---|---|
| 1 | `MsaaPixel.L1_OffscreenEdgeCoverageIsIdeal` | 覆盖率落在 CPU 理想区间 `[hit, hit+miss]` | H2（resolve 正确性） | **绿** |
| 2 | `MsaaPixel.L1_OffscreenIsDeterministic` | 同参数两次渲染逐字节相同 | 反回归（golden 前提） | **绿** |
| 3 | `MsaaPixel.L1_ResolveIsFinalImage` | 读回的是 resolve 后的单采样图，不是 MS 图 | H2 | **绿** |
| 4 | `MsaaPixel.L3_ScenePipelineEdgesSmooth` | l3_scene 管线 AA 生效 | H3 | 红（**测试自身 bug**，见 §2.1） |
| 5 | `MsaaPixel.L3_StrokeRibbonEdgesSmooth` | stroke_ribbon 管线 AA 生效 | H3 | **绿** |
| 6 | `MsaaPixel.L3_Overlay2dEdgesSmooth` | overlay2d 管线 AA 生效 | H3 | **绿** |
| 7 | `MsaaPixel.DefaultPath_FskExtrusionIsSmooth` | F-SK 挤出体（l3_scene + 带深度目标）AA 生效 + 缺省 samples=1 是显式行为 | H3 + 裁定 | 红（同 4） |
| 8 | `MsaaPixel.DefaultPath_FvxInnerPassIsSmoothAndDistinct` | ① inner-pass on/off 像素差异 > 0（+ 差异包围盒）② 开 MSAA 后像素变软 | H5(d) | **绿**（①在 128×128 下差异非 0，与 t2 在 640×400 的观察**不一致**，见 §2.2） |
| 9 | `MsaaPixel.DefaultPath_UiOffscreenHasMsaaSwitch` | UI 消费层有无 MSAA 入口 | H5(a) / R1 | 红（预期：无入口） |
| 10 | `MsaaPixel.DefaultPath_UiOffscreenIsSmooth` | ui-cli 是否接受 `--msaa` | H5(a) / R1 | 红（预期：被拒） |
| 11 | `MsaaPixel.Swapchain_ResolveReachesPresentedImage` | 交换链 per-image MS 图 + MS 图 ≠ 交换链图 | H3 / P0-3 | **绿** |
| 12 | `MsaaPixel.Swapchain_PresentLayoutIsPresentSrc` | present 返回 Ok 且校验层零新增错误 | H7 / from_swapchain | **绿**（注意：1x 路径，见 §2.3） |
| 13 | `MsaaPixel.Swapchain_SteadyStateNoVmaTraffic` | 稳态帧 `vma_alloc_count` / `vma_free_count` 增量 = 0 | captain 收窄口径 | **绿** |
| 14 | `MsaaPixel.Pipeline_DeclaresDepthAttachment` | 管线自带 pass 是否声明深度附件 | **P0-3 本体** | 红（预期：`has_depth == false`） |

---

## 2. 红灯/需澄清项的逐条分析

### 2.1 #4 / #7：`覆盖率 0` 是**我的测试 bug**，不是引擎 bug

旧二进制报：

```
magicblocks_msaa_pixel_test.cpp(503): error: Expected: (a.coverage) > (0.01), actual: 0 vs 0.01
```

根因（源码核对，`src/primitives/mesh_factory.cpp:113-126`）：`MeshFactory::cube()` 的六个面
全部按 **外法线 + u×v=n 逆时针** 生成。相机正对立方体的 -Z 面看进去，
**可见面的绕序是顺时针**，在 `CullMode::Back` + `FrontFace=CCW` 下被整面剔除 →
场景为空 → 覆盖率 0 → 后面所有 AA 断言失去判别对象。

修法（已改，未跑）：该场景的管线改用 `CullMode::None`。**这不是放宽判据** ——
判据是"4x 的边沿比 1x 软"，与剔除与否无关；开着剔除只会得到一张空图。

> captain 提醒的"覆盖率 0 与没开 MSAA 是两件事"完全正确，本用例的分层就是照这个做的：
> `ASSERT_GT(coverage, 0.01)` 先证明**场景画出来了**，之后才断言 AA。

### 2.2 #8：F-VX 内面 pass 在 128×128 下差异非 0，与 t2 的 640×400 观察不同

t2 的观察：`magicblocks-l3-cli.exe stack --width 640 --height 400 --inner-pass on/off` 逐字节相同。
本用例在 **128×128 + 4 方块柱** 下：`inner-pass on/off` 的差异像素 **> 0**（用例绿）。

两者不矛盾但**都没被解释清楚**，t6 必须复核：

- 可能是分辨率依赖（128 下表面 pass 的覆盖不足，内面 pass 有像素漏出来）；
- 也可能是 t2 那次 640×400 的参数（8 层 × 3 笔、相机拉远）让内面被完全覆盖。

**t6 要求**：用本用例的差异包围盒 + `magicblocks-l3-cli stack` 的 PNG 做交叉验证，
给出"内面 pass 到底有没有视觉贡献、在什么条件下没有"的结论；在结论出来之前，
**不要**把 `--inner-pass` 当成"已验证可用"。

### 2.3 #12：present 布局用例**只在 1x 下绿**，4x 的真实风险没覆盖

用例名 `Swapchain_PresentLayoutIsPresentSrc`，但今天它请求的是 **1x** 交换链
（因为 4x 路径会被 §2.1 的剔除问题/形状问题干扰）。

而 t2 §3.5(i) 的静态推导是：**一旦把 `has_depth` 接对、让 exchange 链复用管线自带 pass，
present 就会因为 `finalLayout` 恒 GENERAL 而全坏**。该推导在 4x + 管线 pass 复用分支下
**没有对应用例**。t6 里 R4 落地时必须补：4x 交换链 → 复用管线 pass → present 仍必须 Ok。

---

## 3. 构建阻断（跨成员）

工作区里有别的成员正在加天空盒（`IRenderDevice::create_cube_map`）。在我做最后一次
"带修正"的重建时，库编译失败：

```
src/backend/vulkan/vulkan_command.cpp(685): error C2061: 标识符“VulkanCubeMap”
src/backend/vulkan/vulkan_command.cpp(690): error C3861: “cube_sampler_for”: 找不到标识符
src/backend/vulkan/vulkan_render_device.cpp(260): error C2027: 使用了未定义类型“magicblocks::backend::VulkanBuffer”
FAILED: CMakeFiles/magicblocks.dir/src/backend/vulkan/vulkan_command.cpp.obj
```

结论：**这三处都属于 skybox 的 in-flight 改动**（captain 已下 P0 指令让 skybox 先修构建）。
我不改别人的文件。构建一恢复就立即 `ninja -t clean` 全量重建并重跑 §1 全表，替换本节证据。

---

## 4. 红灯 → t6 修复项映射

| 红灯 | 归因（t2） | t6 修复项 | 验收方式 |
|---|---|---|---|
| #14 `Pipeline_DeclaresDepthAttachment` | P0-3：`create_pipeline(desc)` 从不传 `AttachmentShape`，`has_depth` 恒 false | **R4**：`create_pipeline` 把目标形状（`has_depth` + `samples` + `from_swapchain`）传进 `VulkanPipeline` | 用例 14 变绿 + `MsaaPixel.L3_ScenePipelineEdgesSmooth` 不再依赖回退分支 |
| #9 / #10 UI 层无入口 | P0-1：UI 合成是 CPU 光栅；GPU UI pass 硬编码 1 采样；无 samples 字段 | **R1**：`UiOverlayPass` / `UiAppConfig` / ui-cli 暴露采样数；离屏 UI 若要 AA 必须走 GPU 合成 | 两条用例改成"开 4x 后 UI 边沿出现过渡色" |
| #4 / #7（修完测试 bug 后是否还红） | H3：默认形状下 pass 复用管线 pass，`PipelineDesc.samples` 不参与兼容判定 | **R3**：`PassShapeCore` / `build_attachment_descriptions()` / `pass_clear_value_count()` 落到 `vulkan_resource_common`；两处 pass 共用一份 | 用例 4/7 变绿，且 `clear_depth` 标志真的被读（**R6**） |
| （立即可见风险）present 全坏 | `AttachmentShape` 无 `from_swapchain`、`create_render_pass()` `finalLayout` 恒 GENERAL | **R4 的第二半**：补 `from_swapchain` + `pass_compatible()` 比较它 | §2.3 要求的新用例：4x 交换链 + 管线 pass 复用 + present Ok |
| （F-VX 观感） | `clear_depth` 标志从未被读；内面 pass 贡献不明 | **R6** + §2.2 的交叉验证 | 差异包围盒 + CLI PNG 交叉 |

---

## 5. 本任务改动清单（只碰测试与 CMake）

| 文件 | 性质 |
|---|---|
| `MagicBlocks/tests/magicblocks_msaa_pixel_test.cpp` | **新增** 14 条像素级用例 |
| `MagicBlocks/tests/msaa_pixel_shared.h` | **新增** 测试工具头（几何、CPU 理想覆盖率、诊断文本、BMP 转储；零 Vk*） |
| `MagicBlocks/tests/magicblocks_msaa_test.cpp` | **改**：删除全部 12 处 `GTEST_SKIP`，改"缺能力=FAIL"+遍历 caps 全等级；`CurrentTargetFollowsAcquiredImage` 改为"先真画一帧再 present"（原 SKIP 的唯一来源） |
| `MagicBlocks/src/backend/vulkan/msaa_render_pass_test.cpp` | **改**：删除全部 3 处 `GTEST_SKIP`（纯测试文件，无实现改动） |
| `MagicBlocks/CMakeLists.txt` | **改**：新增 `tests/magicblocks_msaa_pixel_test.cpp` 到测试目标 |

零渲染实现改动。
