# L4-F-SK 依赖判断报告

日期：2026-11-06
前置：L3 已落地（ctest **310/310**，0 失败 0 跳过）
关联：`docs/superpowers/specs/2026-11-05-magicblocks-l4-fsk-sketch-design.md`

---

## 0. 一句话结论

**L3 已把 F-SK 需要的"画"与"渲染"全部备好，唯一缺口是"轮廓 → 3D 挤出"（`FromOutline`）。**
补上这一块，F-SK 就能基于既有能力完整落地，**不需要新增 `backend_hooks.h`**（旧设计的高估）。

---

## 1. 基线核实（2026-11-06 实测）

| 断言 | 结论 | 证据 |
|---|---|---|
| 构建 | ✅ 无需重编（`ninja: no work to do`） | `build.ps1` |
| 测试 | ✅ **310/310**，0 失败 0 跳过 | ctest |
| L3 模块数 | ✅ 15 头 + 15 源（含 alloc/batch/gizmo/mesh/mesh_factory/overlay2d/painter/picker/primitive_renderer/world/world_render） | `include/magicblocks/primitives/` |
| L4 层 | ❌ 不存在（`src/features`、`include/magicblocks/features` 均无） | `Test-Path` |

---

## 2. 依赖矩阵（F-SK 需要 vs L3 实际提供）

| # | F-SK 需要 | L3 是否提供 | 提供者 | 缺口 |
|---|---|---|---|---|
| D1 | 笔画事件 → 笔迹几何 | ✅ **完整** | `IPainter`（`feed`/`geometry`/`extract_outline`/`geometry_hash`） | — |
| D2 | 笔刷参数（宽/色/圆头/平滑） | ✅ **完整** | `StrokeDesc` | — |
| D3 | 事件类型 | ✅ **完整** | `StrokeEvent{kind,x,y,timestamp_us}` | — |
| D4 | 相机（正交/透视） | ✅ **完整** | `ICamera` | — |
| D5 | 网格数据与 GPU 上传 | ✅ **完整** | `MeshData`/`Vertex`/`create_mesh`/`mesh_vertex_layout` | — |
| D6 | 图元生成 | ✅ 完整（cube/box/quad/grid/cylinder/cone/sphere） | `IMeshFactory` | — |
| D7 | **轮廓 → 3D 挤出（含孔三角化 + 侧壁）** | ❌ **缺失** | — | **唯一硬缺口** |
| D8 | 合批 | ✅ **完整** | `IBatchBuilder`/`DrawRequest`/`BatchDraw`/`InstanceAttribs` | — |
| D9 | 渲染（录制命令） | ✅ **完整** | `IPrimitiveRenderer::record_frame` | 但其覆盖面需验证 |
| D10 | 实例化 draw + 深度 | ✅ **完整**（L3 CLI `blocks` 已验证） | `scene_pipeline_desc()` 样板 | — |
| D11 | **命令真正下发** | ✅ **完整** | `IRenderDevice::submit_command_list()` | — |
| D12 | 离屏目标 + 读回 | ✅ **完整** | `create_offscreen_target`/`readback_rgba8` | — |
| D13 | PNG 落盘 | ✅ **完整** | `write_png_rgba8` | — |
| D14 | 离屏 CLI 样板 | ✅ **完整** | `l3_cli`（paint/blocks/gizmo） | — |
| D15 | 材质参数逐实例 | ✅ **完整** | `InstanceAttribs.base_color` | — |
| D16 | 命令流录制回放 | ✅ 完整（L2） | `IRecorder` | F-SK 需组合使用 |

**统计**：16 项中 **15 项已具备，1 项缺失（D7 挤出）**。

---

## 3. 关键修正：旧设计高估了渲染通路

| 旧设计（2026-11-05） | 实测（2026-11-06） | 结论 |
|---|---|---|
| "L1 `draw*` 只记录，必须新增 `primitives/backend_hooks.h` 才能出像素" | ❌ **不成立**。`IRenderDevice::submit_command_list()`（公共接口，L0 层）内部走 `submit_device_frame`，**已能把 11 类命令真正编码下发** | **`backend_hooks.h` 整个不需要** |
| 需要新 shader（std_unlit / stroke_ribbon / canvas_quad） | ❌ **不成立**。`l3_scene.vert/frag`（实例化 3D + 80/20 光照）、`stroke_ribbon`、`overlay2d` 已存在 | **复用既有 shader** |
| 需要实现 `IPrimitiveRenderer` 最小版 | ❌ **不成立**。已存在；且 CLI 已有"手写录制"的成熟样板 | 复用 |
| 需要 `IWorld` ECS | ❌ 不需要（与原判一致） | — |

**证据**：`src/backend/vulkan/vulkan_render_device.cpp:295-323`

```cpp
Status VulkanRenderDevice::submit_command_list(resources::ICommandList& list) {
  auto* cmd = dynamic_cast<VulkanCommandList*>(&list);
  if (cmd->in_pass()) return Status::Invalid(...);
  if (cmd->command_count() == 0) return Status::Invalid("命令列表为空");
  bind_known_buffers(*cmd);
  DeviceFrameStats stats{};
  Status s = submit_device_frame(ctx_, *cmd, &stats);   // ← 真正 vkCmd*
  if (stats.draw_calls == 0) return Status::Invalid("提交成功但没有任何 draw 被下发");
  return Status::Ok_();
}
```

**这大幅简化了工作量**：原计划的阶段 1（backend_hooks，1 天）**整段删除**。

---

## 4. 唯一缺口：`FromOutline` 的规格

### 4.1 输入

`IPainter::extract_outline()` 当前返回**中心线点列**（`std::vector<std::vector<Vec2>>`），
注释写明"供 L4 的 FromOutline 挤出"。因此 L4 需要：

```
中心线点列 → （1）按笔宽 offset 成闭合轮廓 → （2）挤出成 MeshData
```

### 4.2 需要实现的两步

| 步 | 说明 | 依赖 |
|---|---|---|
| **S1 轮廓生成** | 中心线 + `width_px` + `round_cap` → 闭合多边形（胶囊串联）。多笔合并 → 并集 | 可**纯手写**（无需 Clipper2）：胶囊 = 每段矩形 + 两端半圆，多边形并集可用"逐段凸包合并"或直接不做并集（每笔独立） |
| **S2 挤出** | 轮廓多边形（可能带孔）→ 三角化 → 上下盖 + 侧壁 | 三角化：**耳切法**可覆盖简单多边形；带孔需桥接 |

### 4.3 技术选型（依赖判断的落地）

| 方案 | 评估 | 决定 |
|---|---|---|
| libtess2（已在池，`tess2.lib`） | 成熟、支持孔、结果确定 | ✅ **选定**（带孔三角化的正解） |
| Clipper2（已在池） | 用于多笔并集/偏移 | ✅ 用于 S1 的 offset/union |
| 手写耳切 | 简单多边形可用，带孔需桥接算法，易出退化 | ❌ 不作为主路径 |

> **但要注意**：两者虽然"已在池"，**尚未接入 CMake**（`CMakeLists.txt` 只 link `Vulkan::Vulkan`）。
> 这是本次要新增的依赖接入工作。

---

## 5. 落地路径（修正后的计划）

| 阶段 | 内容 | 原估 | 修正后 |
|---|---|---|---|
| ~~1~~ | ~~backend_hooks~~ | 1 天 | **删除（不需要）** |
| **A** | 依赖接入：`find_package(tess2/Clipper2)` + link | 0.5 天 | 0.5 天 |
| **B** | **`FromOutline`**：轮廓生成 + 带孔三角化 + 上下盖 + 侧壁 | 1.5 天 | 1.5 天 |
| **C** | L4 `features/sketch`：文档 + 事件驱动 + 挤出 + 合批 | 2 天 | **1 天**（复用 L3 样板） |
| **D** | `magicblocks-fsk-cli`：无头离屏出图 | 1.5 天 | **0.5 天**（照抄 paint/blocks） |
| **E** | 测试 + golden + 门控 | 1.5 天 | 1 天 |
| **合计** | | ~10 天 | **~4.5 天** |

---

## 6. 现在的执行决定

按用户"**尝试开始**"的指示，本次执行**阶段 A + B + C + D + E 的最小纵向切片**：

1. **依赖接入** tess2 + Clipper2；
2. 实现 **`from_outline_extrude`**（L3 能力，放 `primitives/mesh_factory` 体系，不新增玩法名）；
3. 实现 **L4 `features/sketch`**（唯一允许出现"涂鸦"语义的层）；
4. **`magicblocks-fsk-cli render`** 无头出 PNG；
5. 单测 + CTest + 全量回归。

**验收**：PNG 落盘且非空 + 结构断言 + 310 条基线零回归。
