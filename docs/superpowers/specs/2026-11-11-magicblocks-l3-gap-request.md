# MagicBlocks L3 能力缺口申请：`l3_scene` 管线的 MSAA 与 `ShaderLibrary` 的分层违规

- 日期：2026-11-11
- 提交人：fsk（magicblocks-engine-features 团队，任务 t4）
- 性质：**需求文档**。本轮不在 L4 自建降级版，也不越界改 L1/L3 的归属代码。
- 出处：t4 全量回归（`build-fsk`，553 用例）里的两条红灯 + G8 门控红灯。

---

## 0. 结论速览

| # | 缺口 | 实测证据 | 影响 | 建议归属 |
|---|---|---|---|---|
| **L3-GAP-1** | `l3_scene` 管线在**带深度目标**上打开 MSAA 后，像素**逐字节不变** | `MsaaPixel.L3_ScenePipelineEdgesSmooth` + `MsaaPixel.DefaultPath_FskExtrusionIsSmooth` 两条红，`differing=0`、`unique_colors 2 vs 2` | F-SK 的挤出体、F-VX 的积木都走这条管线 ⇒ 两条玩法的边缘永远是硬锯齿；"MSAA 修复"在玩法路径上等于没修 | **msaa 角色**（L1/L3 引擎能力） |
| **L3-GAP-2** | `src/primitives/shader_library.cpp` 直接 include 了两个 `backend/vulkan/*` 私有头 | G8 门控 `rc=1`：`shader_library.cpp:19,20: L3 直接 include 了 backend 头` | L3 与 backend 解耦被打破；G8 是 CTest 条目，**全量回归因此不可能全绿** | **msaa 角色 / skybox 角色**（该改动是 t6「按目标形状派生管线」引入的） |

---

## 1. L3-GAP-1：`l3_scene` 管线的 MSAA 不生效

### 1.1 复现

```
build-fsk\magicblocks_test.exe --gtest_filter=MsaaPixel.L3_ScenePipelineEdgesSmooth
build-fsk\magicblocks_test.exe --gtest_filter=MsaaPixel.DefaultPath_FskExtrusionIsSmooth
```

两条都在同一处失败（`tests/magicblocks_msaa_pixel_test.cpp:523`）：

```
error: Expected: (differing) != (0u), actual: 0 vs 0
默认路径/F-SK 挤出体: 1x 与 4x 的输出**逐字节相同** —— MSAA 完全没生效。
    差异像素: 0 / 总像素（两档输出是否逐字节相同: 是（MSAA 完全没生效！））
    唯一色数: 1x=2  4x=2
```

### 1.2 分层判据：不是「场景没画出来」

同一次运行里这几条**是绿的**，所以问题被精确定位在"带深度 + l3_scene"这一组合：

| 用例 | 结果 | 说明 |
|---|---|---|
| `MsaaPixel.L1_ResolveIsFinalImage` | ✅ | 无深度的全屏四边形，resolve 正确 |
| `MsaaPixel.L3_StrokeRibbonEdgesSmooth` | ✅ | `stroke_ribbon` 管线（屏幕空间、**无深度**）4x 生效 |
| `MsaaPixel.L3_Overlay2dEdgesSmooth` | ✅ | `overlay2d` 管线（**无深度**）4x 生效 |
| `MsaaPixel.L3_ScenePipelineEdgesSmooth` | ❌ | `l3_scene`（**带深度**）4x **完全不变** |
| `MsaaPixel.DefaultPath_FskExtrusionIsSmooth` | ❌ | 同上 |

规律很干净：**只要目标带深度，l3_scene 的 MSAA 就失效**；不带宽度的管线都正常。
这排除了"MSAA 完全没实现"，指向"带深度目标的 render pass / 管线兼容分支"。

### 1.3 与 t2 报告的关系（供 msaa 角色参考，未核实为根因）

`docs/superpowers/specs/2026-11-11-magicblocks-msaa-taskings.md` 那一路的叙述里
"永远走 `create_pass()` 回退分支"指的是 `vulkan_device_frame.cpp` 的 pass 兼容判断。
t4 只做了**观察**，没有定位到行：

- 需要疑似点：`src/backend/vulkan/vulkan_device_frame.cpp` 里 `pass_compatible` 的计算
  （`ps.samples == shape.samples` 那几项）与 `create_pass(ctx.device, shape, &err)` 的分支；
- 以及 `src/primitives/shader_library.cpp` 的 `create_pipeline_for_shape` 是否把
  `samples` 真的传给了后端（t6 的注释说"以前只调 `create_pipeline(desc)`，
  后端拿到默认形状（无深度）"，这正是本缺口最可能的同源问题）。

**这不是 L4 能修的**：修它要在 L1/L3 的 pass 兼容逻辑里动，L4 只在消费端看到像素不变。

### 1.4 对 L4 的影响（为什么必须报出来而不是绕过）

- F-SK：`magicblocks-fsk-cli` 的挤出体走 `l3_scene` + `with_depth=true`
  —— 用户看到的仍然是硬锯齿边缘；
- F-VX：`stack_render` 的两个 pass 也都是 `l3_scene` + 带深度目标；
- 因此即使 CLI 的 `--msaa 4` 一切正常（像素确实变了），**玩法路径上的锯齿问题没有解决**。

按任务书纪律，L4 **不**为此自建一条"自己的 MSAA pass"：那会在 L4 复制一份
后端能力，且必然与 L3 的 pass 兼容规则分叉。

---

## 2. L3-GAP-2：`shader_library.cpp` 的 backend 依赖打破了 G8

### 2.1 复现

```
python tools/check_l3_command_parity.py --root MagicBlocks
G8 失败: 2 处命令流对等性违规(扫描 34 个文件)
  src\primitives\shader_library.cpp:19: L3 直接 include 了 backend 头
      #include "backend/vulkan/vulkan_pipeline.h"
  src\primitives\shader_library.cpp:20: L3 直接 include 了 backend 头
      #include "backend/vulkan/vulkan_render_device.h"  // create_pipeline_for_shape（后端私有）
EXIT=1
```

改动来源（`git diff` 作者注释）：

```
// 2026-11-11（t6）：形状必须**真的送进后端**。
// 以前这里把 TargetShape 算进缓存键，却只调 create_pipeline(desc) ——
// 后端拿到的是默认形状（无深度），于是"按目标形状派生管线"完全没发生。
```

### 2.2 问题

`create_pipeline_for_shape()` 与 `backend::AttachmentShape` 是**后端私有**的。
L3 想要"按目标形状派生管线"，正确做法是让公共 `PipelineDesc` 自己带上
形状字段（`has_depth` / `samples` / `color_format`），由 backend 在
`create_pipeline()` 内部做一致性校验 —— 而不是让 L3 反向 include 后端头。

G8 的语义正是钉住这条（`tools/check_l3_command_parity.py:1-25`）：
「L3 与 backend 完全解耦」。现在这条门禁**红了**，而它是 CTest 的一个条目，
所以 `ctest` 全绿在结构上已经不可能。

### 2.3 建议

1. 把 `AttachmentShape` 的三元组（`color_format` / `has_depth` / `samples`）
   下沉到 `magicblocks/resources/pipeline.h` 的 `PipelineDesc`（字段已大半存在）；
2. `ShaderLibrary::create` 只调公共 `create_pipeline(d)`，把形状写进 `d`；
3. 删掉 `shader_library.cpp` 的两行 backend include，G8 应恢复 rc=0；
4. 若确实需要"管线与 pass 形状不兼容时明确报错"，让 backend 在
   `create_pipeline()` 里返回 `Unsupported`（这本来就是它所处的层）。

---

## 3. 本轮 t4 的边界

- 本文**只提需求**，不改 `src/backend/**`、不改 `src/primitives/shader_library.cpp`
  （那是 t6/msaa 的改动，越界改会制造"两个人各改一半"）。
- t4 的交付（A1/A3/B1/B2/B3）已全部落地并在 `build-fsk` 上验证：
  77 条相关用例全绿、golden 三份已生成并独立结构校验通过、门控 G1/G3/G4/G7/
  scene_no_clock 全 rc=0；**只剩 G8 与本文件第 1 节的两条 MSAA 红灯不属于 t4 范围**。
