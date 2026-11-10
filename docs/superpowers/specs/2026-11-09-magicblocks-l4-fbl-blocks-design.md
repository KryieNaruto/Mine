# 魔法功能预览 —— L4-F-BL 魔法积木 设计

日期：2026-11-09
状态：**设计稿（待审阅门 ≥90 放行，见 `.dsh/skills/build-pipeline/SKILL.md` §②）**，本文件不含玩法代码改动
关联：
- 总设计：`docs/superpowers/specs/2026-11-03-magicblocks-render-engine-sdk-design.md`（下称"总 spec"）
- L3 原语：`docs/superpowers/specs/2026-11-04-magicblocks-l3-primitives-design.md`（下称"L3 spec"）
- F-SK 先知：`docs/superpowers/specs/2026-11-05-magicblocks-l4-fsk-sketch-design.md`（下称"F-SK spec"）
- 架构索引：`MagicBlocks/docs/ARCHITECTURE.md`、`MagicBlocks/docs/L3_DELIVERY.md`
- 配套执行计划：`docs/superpowers/plans/2026-11-09-magicblocks-l4-fbl.md`

---

## 0. 本轮用户指令与硬边界

用户 2026-11-04 / 2026-11-09 的原话：

> 当前已经完成了 L0-L3，现在开始进行 L4 的落地。这个已经开始是玩法的构建了，**必须一项一项来**。
> 1. 使用 L0-L3 搭建的引擎基础，开始 **F-BL 的魔法积木**功能实现。

由此得到本轮硬边界（与 F-SK 完全同构）：

| # | 边界 | 依据 |
|---|---|---|
| B1 | **一项一项来**：本轮只做 F-BL。**不**顺带设计 F-VX / F-ST / F-PN，只给它们留通用扩展点 | 用户原话 |
| B2 | F-BL 只**组合** L0–L3 的通用能力；需要新增"只有积木用得到"的公开接口 = L3 抽象不足，应回头补 L3，而不是在 features 里开洞 | 总 spec §2.1/§2.2，F-SK spec §0 |
| B3 | SDK 内部（L0–L3）**零玩法分支**；`backend/` 不得出现玩法名（含 `blocks`） | G3 门控 `tools/check_layer_purity.py` |
| B4 | **回退/降级禁令**：能力不足或参数非法一律显式报错，禁止"跳过""退化成另一种行为" | D8 / 总 spec §3 |
| B5 | **CLI + 离屏是硬约束**：每一项交付必须有无头入口与 PNG 落盘 | 总 spec §5.4；build-pipeline §① |

> **本轮交付方式**：按 F-SK 的既有节奏，先出**设计 spec（本文）+ 执行计划**。
> 代码落地在审阅通过后进行，期间每一阶段自带验收（见 plans §3）。

---

## 1. 现状基线（2026-11-09 实测，带证据）

> 口径：本节全部实跑/实读得到。文档与代码冲突时以代码为准，并回头修本文。

### 1.1 构建与测试基线

| 断言 | 实测结论 | 证据 |
|---|---|---|
| 构建可重复 | ✅ `powershell -ExecutionPolicy Bypass -File MagicBlocks\tools\build.ps1` 成功；直接 `cmake --build` 会 LNK1181（缺 vcvars），构建**必须**走脚本 | `MagicBlocks/tools/build.ps1` |
| 测试全绿 | ✅ 368/368（57 套件），连跑 6 次一致 | **数字的唯一真源 = 计划附录 A（实测）** |
| 门控 rc | ✅ 全 0 | 同上（附录取实测值） |
| PNG 有内容 | ✅ F-SK 的 5 张验收图非背景像素占比 4.8%–7.6% | `tools/check_png_content.py` |

> **基线数字不在这里重复写**：本节只列"能否跑通"的定性结论；
> 所有可变的数字（用例数、门控项数、连跑次数、ASan 结果）**唯一真源是
> `docs/superpowers/plans/2026-11-09-magicblocks-l4-fbl.md` 附录 A**。
> 两处各写一份数字，迟早会臭。

### 1.2 本轮开工时修掉的一个**阻塞性**回归

开工基线不是全绿：**368 个用例里有 5 个失败**，其中最关键的一条是
`PixelRegression.SubmitCommandListActuallyWritesPixels` —— 它证明的是"生产提交路径真的写像素"。

- **根因**：`src/backend/vulkan/vulkan_target.h` 里
  `class OffscreenTarget final : public device::IRenderTarget, public IFrameTargetImage`
  用了 **private 继承**（`class` 默认私有）。于是 `submit_device_frame()` 里的
  `dynamic_cast<IFrameTargetImage*>(target)` **恒为 nullptr**，每一帧都报
  「submit_device_frame: 第 0 条命令(begin_pass): 目标不提供可用的颜色附件图像」。
- **为什么危险**：编译器不报错（私有基类合法）。修好之后又暴露出一个**只在全量套件里出现**的
  退出期堆损坏（单条跑必过，全量跑偶发 `vkDestroyImage: Invalid device`）；现已稳定通过，但
  **列为遗留风险 R1**（§14）。
- **处置**：基类改为 public（一行改动 + 20 行注释钉死原因），重建后 368/368。
- **对 F-BL 的意义**：F-BL 的每一条验收都要出 PNG，这条通路不通则整个 L4 不可达。
  这也是 F-SK spec §8 列过的"L3 缺口"之一，本轮顺手补上。

> 教训写进 §14.2：**继承访问级不是编译期能约束的东西，只能靠"真实提交路径 + 像素判据"的测试抓。**

### 1.3 L3 现在能给 F-BL 的能力（逐项核对，均已实测可用）

| F-BL 需要 | L3 现成能力 | 文件 | F-BL 是否直接用 |
|---|---|---|---|
| 积木外形（Cube/Box/Cylinder/Cone/Sphere/Prism/Trapezoid） | `IMeshFactory` 全部图元 + `box_faces(mask)` | `primitives/mesh_factory.h` | ✅ 直接用 |
| 导入 OBJ 精模 | `IMeshLoader::load_obj_text/file`（tinyobjloader） | `primitives/mesh_loader.h` | ✅ 直接用 |
| 透视/正交相机、屏幕↔世界 | `ICamera`（含 `screen_to_ray` / `world_to_screen` / `world_size_per_pixel` / orbit/pan/zoom） | `primitives/camera.h` | ✅ 直接用 |
| 点选（屏幕→射线→求交） | `IPicker`：AABB / 三角形 / 平面 / 球 + `begin_scene/add_object/pick_scene` | `primitives/picker.h` | ✅ 直接用 |
| 选中虚线**无深度**包围盒 | `IOverlay2D::push_dash/push_rect/push_line` | `primitives/overlay2d.h` | ✅ 直接用 |
| Gizmo（移动/旋转/缩放，屏幕恒定尺寸） | `IGizmoSystem`：轴声明、`pick_axis`、拖拽状态机、快照 | `primitives/gizmo.h` | ✅ 直接用 |
| 实例化合批（万级方块 1 次 draw） | `IBatchBuilder` + `build_draw_requests` + `IPrimitiveRenderer` | `primitives/batch.h`、`world_render.h`、`primitive_renderer.h` | ✅ 直接用 |
| ECS 存储实体 | `IWorld`（archetype SOA）+ `register_standard_components` | `primitives/world.h` | ✅ 直接用 |
| 纯色/简单光照材质 | `IMaterialTable` + `l3_scene.frag`（半兰伯特 + 高光，**确定性**光照） | `primitives/material.h`、`shaders/l3_scene.frag` | ✅ 直接用 |
| 帧预算 / 零扩容 | `IFrameArena` / `IBatchPool` | `primitives/alloc.h` | ✅ 直接用 |
| 命令流录制/回放 | L2 `IScene` / `CommandStream` / `IRecorder` | `scene/*.h` | ⚠️ F-BL 首轮只用 L3 的录制通路（见 §9） |

**结论：总 spec 给 F-BL 列的组合路径里，L3 已覆盖 6/8 项。**

### 1.4 仍然缺的东西（F-BL 必须面对）

| 缺口 | 总 spec 位置 | 归属 | 说明 |
|---|---|---|---|
| `IWorldGrid`（均匀网格占用查询 / 洪水搜索空位） | P10 | **L3** | 未实现。F-BL 的"点击自动找空位"依赖它 |
| `IBlockSet`（方块实例集合，按笔增删，AABB 范围查询） | P7a | **L4**（用户 2026-11-04 明确划归 L4） | 未实现 |
| `IBlockBrush`（扫掠求交 → 批量 emit） | P7b | **L4** | 未实现；首轮 F-BL 只需要一个弱化版（单点落块） |
| `IShapeTemplate`（"任意形状"= 一组方块实例） | P7c | **L4** | 未实现；由 `IMeshLoader` 导入路替代（见 §7.4） |
| 玩法数据模型的**撤销/重做** | — | **L4** | F-SK 已有 `SketchDocument` 的 undo/redo 范式，F-BL 对齐它 |
| 玩法文档的**确定性指纹** | 总 spec §5.3 | **L4** | F-SK 的 `doc.hash()` 范式 |

> **关键裁定（Q-BL-1）**：`IWorldGrid` 进 **L3**（它是通用"空间占用查询"，印章/叠叠乐同样要用，
> 不含任何玩法语义）；`IBlockSet/IBlockBrush/IShapeTemplate` 留在 **L4 features**（它们是玩法数据模型）。
> 这条与总 spec §4「P10 在 L3、P7a/P7b/P7c 在 L4」完全一致，不需要改总 spec。

---

## 2. F-BL 的玩法定义（先定语义，再谈接口）

> 这一节是**体验契约**。接口、命令流、测试全部从它推导；反过来，任何接口如果表达不了这里的
> 一句话，就是抽象不足。

### 2.1 一句话

> **在 0,0,0 网格平面上，用手指/鼠标点一下，就在"点到的位置"长出一块积木；
> 积木可以换外形、换颜色、能点选、能移动、能删除；网格平面与选中包围盒必须看得见。**

### 2.2 六条验收行为（对应总 spec §4 的"积木 6 条"）

| # | 行为 | 用户可见结果 |
|---|---|---|
| BL-1 | **落块**：点空地 → 在该格长出一块积木 | 点一下就多一块（**选中态下点空地同样落块**，见 Q-BL-4） |
| BL-2 | **叠放**：**已选中一块时再点它** → 在它上面再叠一块（Q-BL-10） | 能往上堆，不穿模 |
| BL-3 | **换形**：切换图元（立方/长条/圆柱/球/棱柱/棱台） | 后续落块用新外形 |
| BL-4 | **换色**：切换颜色 | 后续落块用新颜色（已落的不变） |
| BL-5 | **选中**：点已有积木 → 它被选中（虚线包围盒 + gizmo） | 看得出选中了谁 |
| BL-6 | **移动/删除**：拖动 gizmo 移动选中块；删除键删掉选中块 | 能改、能删 |

辅助（必须可见，否则"看起来能点"却不落块时无从判断）：

| # | 行为 |
|---|---|
| BL-7 | **0,0,0 网格平面**渲染 + 网格线指示（`IOverlay2D` 网格线，格子尺寸 = `cell_size`） |

### 2.3 与 F-VX（叠叠乐）的边界（防止本轮越界）

| | **F-BL 魔法积木（本轮）** | F-VX 魔法叠叠乐（不在本轮） |
|---|---|---|
| 落笔方式 | **单点落块**（点一下一块） | **扫掠落块**（一笔拖过去长一条） |
| 撤销粒度 | 单块操作 | **整笔**（`stroke_id`） |
| 数据结构 | `BlockDocument`（有序实例表） | `IBlockSet` + `stroke_id` |
| 特殊渲染 | 常规深度测试 | 内面渲染（`depthTest=OFF + depthWrite 常量`） |
| 层锁 | 无 | 有（笔刷锁层） |

> 两者都建立在同一组底层件上（格点吸附 / 占用查询 / 合并生成 / 实例化绘制）。
> 本文设计的 `GridSnap` + `IWorldGrid` 是 F-VX 能直接复用的那部分 —— 这就是"通用扩展点"。

### 2.4 明确不做（首轮）

| 不做 | 理由 |
|---|---|
| 旋转积木（90° 步进） | gizmo 有 Rotate 轴，但积木对齐网格时旋转会破格。**接口预留 `Block::rot_quarter`，首轮恒 0** |
| 自由浮点放置（`FreeFloat`） | 总 spec Q1 已裁定：必须贴齐 0,0,0 网格 |
| 多选 / 框选 / 复制粘贴 | 单点选足够验收 BL-5/6 |
| 积木间物理/重力/碰撞 | 不做（不是物理引擎） |
| 外部贴图 / PBR 材质 | 纯色即可（`l3_scene.frag` 已够） |
| OBJ 导入的 UI 入口 | 能力由 `IMeshLoader` 提供，CLI 参数验证（BL-3 的旁支），不做交互式导入 |
| 撤销栈的持久化 | 进程内即可 |
| F-VX / F-ST / F-PN 的任何实现 | 用户裁定"一项一项来" |

---

## 3. 分层与落位（谁写在哪一层）

```
┌─────────────────────────────────────────────────────────────┐
│ 壳层（Compose / SDK 之外）：按钮、调色板、模式切换               │
│   → 只发 SDK 的"意图"（见 §8 InputIntent），不碰几何             │
├─────────────────────────────────────────────────────────────┤
│ L4  src/features/blocks/                 ← 本轮新增            │
│   block_document.{h,cpp}   积木文档：有序实例表 + 撤销/重做 + hash│
│   block_types.h            格点坐标 / 外形 / 颜色 / 编辑操作      │
│   block_placement.{h,cpp}  点→射线→命中→吸附→占用检查→落块/叠放   │
│   block_meshes.{h,cpp}     文档 → (merged) MeshData 集合          │
│   block_scene.{h,cpp}      文档 → IWorld 实体镜像（供合批/绘制）   │
│   block_session.{h,cpp}    交互会话：选中 / gizmo 接线 / 意图分发  │
│   block_render.{h,cpp}     一帧出图：世界→合批→overlay→命令流     │
├─────────────────────────────────────────────────────────────┤
│ L3  src/primitives/                       ← 本轮补 1 个模块      │
│   ★ world_grid.{h,cpp}     IWorldGrid：格点占用查询 + 洪水找空位  │
│   （其余全部复用：camera / picker / gizmo / overlay2d / batch /  │
│     mesh_factory / mesh_loader / world / material / renderer）   │
├─────────────────────────────────────────────────────────────┤
│ L2 / L1 / L0 / backend：**一行不改**                            │
└─────────────────────────────────────────────────────────────┘
```

### 3.1 为什么 `IWorldGrid` 一定在 L3 而不是 features

1. **它本来就是总 spec 已立项的通用能力**（总 spec `P10` 条目，即 `IWorldGrid`）。
   本轮不是"因为 F-BL 需要才给 L3 开口子"，而是**按总 spec 既有条目落地**；
   F-BL 只是它的第一个客户。**论证链是"总 spec 已立项 → 本轮落地"，不是"玩法需要 → 补 L3"。**
2. **语义通用**：它只是"一组 (cell, level) 占用项的查询结构"，不含"积木/叠叠乐/印章"任何词；
3. **至少两个客户**：F-BL 的落块避让 + F-VX 的扫掠去重；
4. **反面论证**：把搜索算法塞进 `block_placement.cpp` 会让 F-VX 复制一份 —— 那正是总 spec §2.2
   要禁止的"专用通道"的反面：**重复实现**。

> **纪律（对应 F-SK spec §0 的同一条底线）**：**不得因为某个玩法"顺手需要"就给 L0–L3 加公开接口。**
> 本轮的 L3 改动只有两项，且**都已有立项依据**：①`IWorldGrid`（总 spec P10 既有条目，本轮落地）；
> ②`IPicker` 法线缺口修复（见 §6.2，L3 自身语义 bug，与玩法无关）。
> 任何第三项 L3 改动都必须先回本设计并重新过审阅门。

### 3.2 依赖方向（单向，可机械校验）

```
block_session → block_placement → world_grid(L3)
block_render  → block_scene → IWorld(L3) → build_draw_requests(L3) → IBatchBuilder(L3)
block_render  → overlay2d(L3) + primitive_renderer(L3)
block_document: 只依赖 `world_grid.h`（`Cell`/`GridSpec` 归 L3，见 §3.2.1 的修正）
block_document: 只依赖 `world_grid.h`（`Cell`/`GridSpec` 归 L3，见 §3.2.1 的修正）
```

`block_document` 除 `world_grid.h` 外不依赖其他 L3 头文件 —— 这样它仍可脱离设备、脱离相机做**纯逻辑单测**，
和 F-SK 的 `SketchDocument` 一个套路。

#### 3.2.1 类型归属：**每个类型只在一层定义一次**（Q-BL-9）

> 上一版设计有一处**自相矛盾**：`GridSpec` 既写在 `features::blocks`（§4.3），又被 `IWorldGrid`
> 的形参使用（§6.4）—— 那等于让 **L3 依赖 L4 类型**，直接违反单向依赖。这里写死：

| 类型 | 唯一定义位置 | 理由 |
|---|---|---|
| `struct Cell { std::int32_t x, y, z; }` | **L3** `include/magicblocks/primitives/world_grid.h` | 它是 `IWorldGrid` 的形参类型，放 L4 会倒置依赖 |
| `struct GridSpec { Vec3 origin; float cell_size; std::int32_t half_extent; }` | **L3** `world_grid.h` | `set_occupied` 越界判定需要它；`BlockDocument` 只**持有一份副本** |
| `BlockShape` / `BlockId` / `Block` | **L4** `features/blocks/block_types.h` | 与格点无关，是玩法概念 |
| `GridQuery { Cell start; std::int32_t max_rise; }` | **L3** `world_grid.h` | `find_free` 的入参 |

`features/blocks/block_types.h` 对 `Cell` / `GridSpec` **只 `#include <magicblocks/primitives/world_grid.h>`，不另定义**。

> **机械校验**：阶段 1 加一条静态断言用例
> `WorldGrid.CellAndGridSpecAreL3Owned`：
>
> ```cpp
> static_assert(std::is_same_v<magicblocks::features::blocks::Cell,
>                              magicblocks::primitives::Cell>);
> static_assert(std::is_same_v<magicblocks::features::blocks::GridSpec,
>                              magicblocks::primitives::GridSpec>);
> ```
>
> 若哪天有人在 L4 里重新声明一个 `GridSpec`，这两条断言会直接编译失败 ——
> 比"靠 review 记住"可靠。**若断言写不出来（因为 L4 确实定义了同名类型），就是设计走偏了。**

> ⚠️ 注意：`block_document` 原先在 §3.2 声明的"零 L3 依赖"**已不成立**，它必须 include `world_grid.h` 取 `Cell`/`GridSpec`。
> 取 `Cell` / `GridSpec`。这是有意的取舍：**类型归属正确性 > 头文件纯度**；
> `world_grid.h` 是纯数据 + 纯函数，不含设备/相机，**纯逻辑单测仍然成立**（不创建 `IWorldGrid` 实例即可）。

### 3.3 门控归属（**两套门控，别挂错**）

| 目录 | 门控 | 规则 | 对 `blocks` 一词的处理 |
|---|---|---|---|
| `src/backend/**` | **G3** `tools/check_layer_purity.py:103,126-135` | backend 内不得出现玩法名 | 违规（`blocks` 在 `GAMEPLAY_NAMES` 里） |
| `src/features/**` | **G3** `:104,134` | 只禁 `vkCmd` / `CmdDraw`，**不禁玩法名** | **合法**（这正是玩法层） |
| `include/magicblocks/primitives/**` + `src/primitives/**` | **G7** `tools/check_l3_no_gameplay_names.py` | L3 层零玩法名（比 G3 更严） | **违规**，但见下方白名单说明 |

> **G7 的 `blocks` 白名单是真实的、有依据的，不是漏洞**：
> `check_l3_no_gameplay_names.py:56-62` 用 `ALLOWLIST = magicblocks|timestamps?` 先抹掉 SDK 名，
> 再用 `ALLOWED_IDENTIFIERS = {"block", "blocks"}` **按完整标识符**放行 —— 注释写明理由：
> `arena`/池的 `blocks_` 是"一块内存"，与积木玩法无关，误报会导致整条规则被关掉（比没有规则更糟）。
> 所以 `world_grid.h` 里出现 `block`/`blocks` 标识符**不会**被 G7 抓；但 `voxel`/`积木`/`blocks_gameplay`
> 这类词会被抓。
>
> **结论**：`world_grid.h` 挂 **G7**（不是 G3）；G3 只负责 `src/backend/**` 那一路与
> `src/features/**` 的 GPU 命令禁令。本设计 §11 验收 A14 按此口径断言。

---

## 4. 数据模型：`BlockDocument`

### 4.1 核心取舍：文档存"格点整数"，不存浮点矩阵

F-SK 的教训是"文档存点不存网格"。F-BL 更进一步：

| 方案 | 撤销精度 | 命令流哈希 | 浮点漂移 | 内存 |
|---|---|---|---|---|
| 存 `Transform`（float 矩阵） | 每步都是新值 | ❌ 哈希不稳定 | ❌ 同一次点击两次可能差 1e-7 | 64 B/块 |
| **存 `cell`（int32×3）+ `level`（int32）+ 枚举** | ✅ 整数精确 | ✅ 稳定 | ✅ 天然无漂移 | **16 B/块** |

**裁定（Q-BL-2）**：`Block.pos` 用 `std::int32_t cell[3]` 整格坐标，与总 spec Q1 的 A 方案一致。
换算：`world = grid_origin + cell * cell_size`，`grid_origin` / `cell_size` **是文档级常量、进 hash**。

### 4.2 类型定义

```cpp
namespace magicblocks::features::blocks {

// 格点坐标（整数，绝不 float）。三轴分别存，便于 memcpy / 逐字节 hash。
struct Cell {
  std::int32_t x = 0, y = 0, z = 0;
  friend bool operator==(Cell a, Cell b);   // 逐字段 ==
};

// 积木外形。枚举而不是字符串：命令流里要能序列化，哈希要稳定。
// 与 L3 的 IMeshFactory 一一对应，但**不是**把工厂搬过来 ——
// 这里只是"要哪种外形"的选择项，几何由 L3 生成。
enum class BlockShape : std::uint8_t {
  Cube = 0,        // 1×1×1
  Brick = 1,       // 2×1×1（长条）
  Cylinder = 2,
  Sphere = 3,
  Prism = 4,       // 六棱柱
  Trapezoid = 5,   // 棱台（上细下粗）
  Imported = 6,    // 由 IMeshLoader 导入的外部网格（见 §7.4）
};

// 积木身份。**单调递增、永不复用**（同 F-SK 的 StrokeId / D-SK-3）：
// 撤销后重做会拿新 id，"撤销栈里的 id"与"画布上的 id"永不撞车。
struct BlockId {
  std::uint64_t value = 0;
  bool valid() const { return value != 0; }
};

// 一块积木。
struct Block {
  BlockId id{};
  Cell cell{};                        // 左下前角所在格点
  std::uint8_t rot_quarter = 0;       // 绕 +Y 的 90° 步进（首轮恒 0，接口预留）
  BlockShape shape = BlockShape::Cube;
  primitives::Color4 color{0.85f, 0.42f, 0.30f, 1.0f};
  std::uint16_t shape_variant = 0;    // Imported 时指向网格槽位；其余恒 0
};
```

### 4.3 文档接口

```cpp
class BlockDocument {
 public:
  // ---- 网格常量（进 hash；创建后不可变，避免"改常量让旧布局失效"）----
  const GridSpec& grid() const;
  explicit BlockDocument(const GridSpec& grid = GridSpec::Default());

  // ---- 实例 ----
  const std::vector<Block>& blocks() const;
  std::size_t block_count() const;
  const Block* find(BlockId id) const;
  // 该格是否已被占用（同一格只允许一块 —— 这是"不穿模"的最简保证）
  const Block* block_at(Cell c) const;

  // ---- 编辑（全部压撤销栈）----
  BlockId add(Block b);                  // 格已被占用 → 返回无效 id 且不改文档（D8）
  bool move(BlockId id, Cell to);        // 目标格被占 → false
  bool set_color(BlockId id, Color4 c);
  bool remove(BlockId id);

  // ---- 撤销 / 重做（与 F-SK 同粒度语义：一次用户操作 = 一步）----
  bool can_undo() const; bool can_redo() const;
  bool undo(); bool redo();

  // ---- 确定性指纹（命令流回放校验的"眼睛"）----
  // 覆盖：grid 常量 + 每块的 id/cell/rot/shape/color；**逐位**混合，不用 epsilon。
  std::uint64_t hash() const;

  std::uint64_t next_block_id() const;
};

struct BlockEdit {          // 撤销栈项（值语义，可平凡拷贝）
  enum class Kind : std::uint8_t { Add, Remove, Move, Recolor };
  Kind kind;
  Block before;             // Remove/Add 时存整块
  Cell to{};                // Move 的目标
  Color4 color{};           // Recolor 的新值
  std::size_t index = 0;    // 在 blocks_ 里的位置（重做插回原处，保持顺序稳定）
};
```

### 4.4 三条不变量

| # | 不变量 | 为什么 |
|---|---|---|
| I1 | `blocks()` 的**顺序就是绘制顺序**，且与 hash 绑定 | 同色块的绘制顺序影响透明混合；顺序不稳 = 回放像素不稳 |
| I2 | `BlockId` 单调递增且**永不复用** | 撤销/重做后 id 撞车会产生"幽灵状态"，只在特定序列复现 |
| I3 | **同格唯一**：任意 `cell` 至多一块 | 穿模的最简保证；也把占用查询降成 O(log n)/O(1) |

---

## 5. 格点与网格平面（BL-7）

### 5.1 `GridSpec`（**定义在 L3 `world_grid.h`**，见 §3.2.1）

```cpp
// include/magicblocks/primitives/world_grid.h —— L3，唯一一处定义
struct GridSpec {
  Vec3 origin{0.0f, 0.0f, 0.0f};  // 0 号格点左下前角的世界坐标
  float cell_size = 0.5f;         // 一格边长（世界单位）
  std::int32_t half_extent = 16;  // 可落区 ±16 格
  static GridSpec Default();
};
```

**为什么 `origin` 和 `cell_size` 在文档里而不是在渲染参数里**：
它们决定"一次点击落到哪一格"与"看到的格子"。若渲染层用一套、落块用另一套，
用户会觉得"格子画歪了"——这正是总 spec Q1 强调的"看到的格子 = 落的格子"。
`BlockDocument` 持有一份 `GridSpec` **副本**，并把它算进 `hash()`（§4.3）。

### 5.2 网格平面渲染（**尺寸必须显式换算，不能用单位矩阵**）

实读 `src/primitives/mesh_factory.cpp:174-198`：

```cpp
MeshData grid(u32 divisions) const override {
  const float step = 1.0f / static_cast<float>(n);
  v.position[0] = -kHalf + x * step;   // kHalf = 0.5f
  v.position[2] = -kHalf + z * step;
```

即 **`grid(n)` 始终只占 `[-0.5, +0.5]²`**（1×1 世界单位），与 `n` 无关；
`n` 只影响**内部剖分数**，不改变外框尺寸。所以正确写法是：

```cpp
// 让地面正好覆盖 ±(half_extent * cell_size)
const float side  = static_cast<float>(2 * spec.half_extent) * spec.cell_size;  // 边长（世界单位）
const float scale = side / 1.0f;                                                // grid() 的基准边长是 1
instance.position = spec.origin;      // 地面中心 = GridSpec 原点（默认 0,0,0）
instance.scale    = Vec3{scale, 1.0f, scale};
// divisions 只控制网格线密度，与尺寸无关
const MeshData ground = factory.grid(static_cast<u32>(2 * spec.half_extent));
```

代入默认值 `half_extent = 16`、`cell_size = 0.5`：
**边长 = 2×16×0.5 = 16.0 世界单位，覆盖 `[-8, +8]`，中心在 `origin`**。

> **上一版写的是"用单位矩阵缩放平移"——那是错的**：单位矩阵会让 32 格的地面只有 1×1 世界单位，
> 与 `cell_size` 毫无关系，画面里格子会挤在原点附近。这条错误由审阅门抓出，已改。

- **网格线指示**：`IOverlay2D` 线（**屏幕空间**，恒宽 1px），把地面网格的格线投影后画出来。
  格线端点由 `GridSpec` 推导（`origin + (i * cell_size, 0, ±half_extent*cell_size)`），
  **与地面的缩放推导共用同一个函数** `grid_world_extent(spec)`，避免两处各算一遍再画歪。
- **可落区边界**：`±half_extent` 之外的落点**明确拒绝**（D8），而不是落在地面外看不着的地方。

> **不做**：地面阴影、网格渐隐、轴标签。首轮"看得见格子"即可。

> **验收**：`BlocksE2E.GridPlaneMatchesGridSpec` —— 断言渲染用的地面实例
> `scale == 2*half_extent*cell_size`、`position == spec.origin`；并断言 `grid_world_extent(spec)`
> 与 `IWorldGrid` 的可落区判定用的是**同一个** `spec`。

---

## 6. 落块与叠放（BL-1 / BL-2）

### 6.1 落块链路（7 步，全 CPU，无 GPU 读回）

```
屏幕 (px, py)
   │ ICamera::screen_to_ray
   ▼
世界射线 Ray
   │ IPicker::begin_scene / add_object(cell, AABB) / pick_scene
   ▼
命中块（含命中面法线 n）
   │  无命中 → 与 y=0 的网格平面求交（IPicker::intersect_plane）
   ▼
世界点 p
   │  格点吸附：cell = floor((p - origin) / cell_size)  （按命中面沿法线偏移半格，见 §6.3）
   ▼
候选 Cell
   │  IWorldGrid::occupancy 查询 + half_extent 边界检查
   ▼
若被占用 → 沿 +Y 洪水搜索最近空位（IWorldGrid::find_free）
   ▼
BlockDocument::add  →  Mesh 增量合并  →  IWorld 实体镜像增量更新
```

### 6.2 `IPicker` 的用法细节（**含一处必须修的 L3 缺口**）

`primitives/picker.h` 的 `PickHit` 字段是够的：

```cpp
struct PickHit {
  std::uint32_t entity;    // 0 = 纯几何（无归属实体）
  float         distance;  // 沿射线的 t；-1 = 无效（默认值刻意是 -1）
  Vec3          point;     // ✅ 命中点（世界）
  Vec3          normal;    // ⚠️ 字段存在，但 pick_scene 填的是**假值**，见下
  std::uint32_t triangle;  // 非三角网格命中 = kNoTriangle
  bool valid() const;
};
```

#### 6.2.1 已核实的 L3 缺陷（**GAP-P1**）

实读 `src/primitives/picker.cpp:406-414`：

```cpp
if (!found) return false;
if (out) {
  out->entity   = best_entity;
  out->distance = best;
  out->point    = ray.origin + ray.direction * best;   // ✅ 真值
  out->normal   = Vec3{0.0f, 0.0f, 1.0f};              // ❌ 硬编码恒 +Z
  out->triangle = kNoTriangle;                         // ❌ 恒 kNoTriangle
}
```

**为什么这是 L3 自身的 bug，而不是 F-BL 的需求**：`NarrowPhase` 的回调签名是

```cpp
using NarrowPhase = bool (*)(std::uint32_t entity, const Ray& ray, float* out_t);
```

它**只能回报一个 `t`**，没有通道把"命中了哪个三角形、那个三角形的法线是什么"传回来。
于是 `PickHit::normal` / `triangle` 这两个字段**永远不可能被正确填充** ——
无论谁调用 `pick_scene`，拿到的都是 `+Z`。这是接口的表达能力缺口，与玩法无关。

**F-BL 依赖 `normal` 决定落块方向**（§6.3：顶面往上堆、侧面往旁边贴），所以：

| 项 | 决定 |
|---|---|
| 修在哪 | **L3**：`include/magicblocks/primitives/picker.h` + `src/primitives/picker.cpp` |
| 怎么修 | `NarrowPhase` 改为带回填结构：`bool (*)(std::uint32_t entity, const Ray& ray, PickNarrow* io)`，`PickNarrow{ float t; Vec3 normal; std::uint32_t triangle; }`；**两条拾取路径都要改**（见下） |
| 兼容性 | `NarrowPhase` 目前**零外部调用方**（全仓只有 `picker.cpp:188/239/335` 自身），改签名不破坏既有代码 |
| **禁止的做法** | 在 `features/blocks/` 里自己写三角形求交绕开 —— 那等于在玩法层复制 L3 能力，正是总 spec §2.2 要禁止的"专用通道" |
| 回归判据 | **两条用例**（阈值两侧各一条，见 §6.2.1 下方） |

##### ⚠️ `picker.cpp` 有**三处**硬编码 `Vec3{0.0f, 0.0f, 1.0f}`，不是一处

实读 `src/primitives/picker.cpp`：

| 行 | 所在函数 | 何时走到 |
|---|---|---|
| `:92` | `intersect_triangle` 的非命中分支之外的**局部默认法线** | 三角形退化时 |
| `:271` | `pick_linear` | `objects_.size() <= kBvhThreshold`（`kBvhThreshold = 8`，`:219`） |
| `:411` | `pick_bvh` | `objects_.size() > kBvhThreshold` |

`:238` 的注释明确写着「BVH 路径必须与线性给出完全一致」——
**所以只改 `pick_bvh` 会让 8 个以内的小场景拿到假法线、9 个以上拿到真法线**，
表现为"积木一多，落块方向就对了"，这是最难查的一类 bug。

**回归必须是两条、且断言同一组结果**：

| 用例 | 场景 | 断言 |
|---|---|---|
| `PickerBvh.SceneHitReportsRealNormal` | 登记 **16** 个 AABB（走 `pick_bvh`） | 向下打立方体顶面 → `normal≈(0,1,0)`；+X 面 → `normal≈(1,0,0)`；`triangle != kNoTriangle` |
| `PickerBvh.LinearPathReportsRealNormal` | 登记 **3** 个 AABB（走 `pick_linear`） | **同样三条断言，结果逐字段与上一条相等** |

> 第二条用例的判据写成"与 `SceneHitReportsRealNormal` **逐字段相等**"，
> 不是"各自看起来对" —— 这样任何一个路径掉队都会红。

> **Q-BL-6 的状态**：`point` 问题**已关闭**（确有真值）；`normal` 问题**升级为 GAP-P1**，
> 列入计划**阶段 1.5**（在 `IWorldGrid` 之后、落块逻辑之前），不许绕开、不许留到"实现期再说"。

#### 6.2.2 用法

```cpp
PickHit hit;
if (picker.pick_scene(camera, px, py, &hit)) {           // 命中积木 → 按 hit.normal 走 §6.3
  ...
} else if (picker.intersect_plane(ray, ground_plane, &t)) {  // 命中地面
  ...
}
```

> 唯一要小心的是 `valid()`：默认 `distance = -1`，但 `pick_scene` 的返回布尔值才是判据 ——
> 调用方**用返回值判断**，不要单独看 `hit.valid()`（`picker.h:30-36` 已记录过这个坑）。

### 6.3 吸附规则（必须钉死，否则"点两下出三块"）

| 情形 | 规则 |
|---|---|
| 命中**地面** | `cell = cell_floor(p)`；`cell.y = 0` |
| **选中态下点同一个已选中块**（Q-BL-10 的叠放入口） | `cell = hit_cell + (0, +1, 0)`；**不看命中面法线** —— 叠放永远是"往上堆"，用户点顶面、侧面、底面都堆到上面 |
| `Idle` 态命中积木 | **不落块**，改为选中（Q-BL-10）；吸附规则不参与 |
| 命中**积木顶面**（法线 +Y）且该块未被选中 | 需要显式"落块到该块上方"时（如脚本原语 `tap-on-top`）才走：`cell = hit_cell + (0,1,0)` |
| 命中**积木侧面**（法线 ±X/±Z）且该块未被选中 | 同上，`cell = hit_cell + n`（沿法线贴一块） |
| 命中**积木底面**（法线 -Y） | `cell.y = hit_cell.y - 1`（**允许**，但为负时拒绝：地面以下不可放） |
| 命中**外表面朝上但被占用** | 沿 +Y 找最近空位（§6.4） |

`cell_floor(p, axis)` = `floor((p[axis] - origin[axis]) / cell_size)`，**带 1e-4 的容差**：
命中点恰好落在格线上时，浮点误差会让 `floor` 掉进相邻格。容差取
`epsilon = cell_size * 1e-4`，先 `p += epsilon` 再 floor（方向与法线一致），保证
"点在格线上"稳定落到同侧。这条容差写进文档 hash 之外的常量表，改它要跑 golden。

### 6.4 洪水搜索（自动找空位）

```cpp
// L3 新增：primitives/world_grid.h
struct GridQuery {
  Cell     start;
  std::int32_t max_rise = 64;      // 最多往上找多少格
  bool     only_up = true;         // 只沿 +Y（首轮 F-BL 的口径）
};
struct GridOccupancy { Cell cell; bool occupied; };

class IWorldGrid {
 public:
  virtual void clear() = 0;
  virtual void set_occupied(Cell cell, bool occupied) = 0;
  virtual bool is_occupied(Cell cell) const = 0;
  // 从 start 沿 +Y 找到第一个未占用格；超 max_rise 返回 false（不返回"最后一个" ——
  // 那等于静默改用户意图）。
  virtual bool find_free(const GridQuery& q, Cell* out) const = 0;
  virtual std::size_t occupied_count() const = 0;
};
std::unique_ptr<IWorldGrid> create_world_grid();
```

实现：`std::unordered_map` 或有序 `std::map` 键 = `(x,z) → 已占用的 y 区间压缩表`。
首轮按"每列一个 bitset/紧凑区间"实现，查询 O(log n)。

> **为什么不做三维哈希**：积木天然是"一列一列往上堆"，按列压缩既省内存又让
> `find_free` 变成 O(log n) 的**区间查找**而不是逐格探测。

### 6.5 叠放的"不穿模"判据（机械可测）

> 任意两块积木的 **AABB 在 world 空间不相交**（`Cell` 唯一性 + 同尺寸格子已保证）。
> 测试 `Placement.NoOverlapAfterNPlacements`：随机（**确定性种子**）落 200 块，
> 逐对检查 AABB 不相交；并检查 `document.hash()` 在两次同种子运行下逐位相同。

---

## 7. 积木几何：合并生成 + 增量更新

### 7.1 为什么需要"合并"

`blocks` 子命令（L3 CLI）已经证明：**N 个实体共用 1 个单位网格 + 实例矩阵**，
1 次 draw / 1 次 pipeline bind。但 F-BL 有个 L3 CLI 没覆盖的点：**外形会变**。

| 场景 | 直接做法 | 问题 |
|---|---|---|
| 1 万个立方积木 | 1 个 Cube 网格 + 1 万实例 | ✅ 完美 |
| 1 万个立方 + 1 个球 + 1 个圆柱 | 3 个网格 + 3 组实例 | ✅ 仍然只有 3 次 draw（`IBatchBuilder` 按 MeshId 批） |
| 每个积木都能导入自定义 OBJ | **每种 OBJ 一个 MeshId** | ⚠️ draw 次数 = 不同 OBJ 种数 |

**结论（Q-BL-3）**：**不做几何合并**。走 L3 既有的"单位网格 + 实例矩阵"路线，
不同外形 = 不同 `MeshId`；`BlockShape` 枚举只有 7 种，**draw 上限天然 ≤ 7**，
远低于门限 `kMaxDrawcalls = 300`。

> 早期想法是"把同色同形积木合并成一个网格"——被否决，因为它把
> "改一块的选中包围盒"退化成"重建整批网格"，而 L3 的实例化已经把这笔账算清了。

### 7.2 外形 → MeshId 映射（`block_meshes.cpp`）

```cpp
struct BlockMeshSet {
  // 下标 = BlockShape；未启用的外形是空 MeshData。
  primitives::MeshData shape_mesh[7];
  // Imported：槽位 → 已导入的 MeshData（每个"导入"多一张）
  std::vector<primitives::MeshData> imported;
  std::uint64_t mesh_hash = 0;   // 沿用 F-SK 口径：派生结果也要有指纹
};
BlockMeshSet build_block_meshes(const BlockDocument& doc, const MeshOptions& opts);
```

| `BlockShape` | L3 调用 | 备注 |
|---|---|---|
| Cube | `IMeshFactory::cube()` | 单位网格，实例缩放 = `cell_size` |
| Brick | `IMeshFactory::cube()` | 尺寸靠实例矩阵的非均匀缩放（1×1×1 → 2×1×1） |
| Cylinder | `cylinder(0.5, 1.0, 24)` | 半径:高度比固定，实例缩放会**压成椭圆**（`mesh_factory.h` 已注明） |
| Sphere | `sphere(0.5, 16, 24)` | 同上，只能等比缩 |
| Prism | `prism(0.5, 1.0, 6)` | 平直着色棱柱 |
| Trapezoid | `trapezoid(0.5, 0.3, 1.0, 16)` | 上细下粗 |
| Imported | `IMeshLoader::load_obj_text()` | 失败必须报错（`MeshLoadResult.status`） |

> **Brick 的形状取舍**：用实例非均匀缩放而不是 `box_faces()`，因为后者会把面从网格里
> 剔掉、再也放不回来；而 Brick 只是"缩放后的 Cube"，用缩放表达最简单且零额外内存。

### 7.3 增量更新（稳态零 VMA）

一旦落块，**不允许**重建整个 `BlockMeshSet`。因为：
- `MeshData` 重建 = 新 `MeshId` = 合批排序键全变 = 整帧重录；
- L3 的 `IMesh` 不可变（`mesh.h` 已注明），"改一块的网格"只能换 MeshId。

所以：**积木只改实例矩阵，不改网格**。落块 → `IWorld::spawn` 一个实体 + 写 `TransformComp`
即完成，实例缓冲在帧内由 `build_draw_requests` 统一填充。

### 7.4 OBJ 导入（`Imported`）

- 首轮**只提供能力**：CLI `--obj <path>` 把外部网格作为一个 `Imported` 外形加进 `BlockMeshSet`，
  之后的落块都用它；
- 导入**只做一次**（启动期），落块不重复解析文件 —— 这样命令回放不受文件系统状态影响；
- 导入失败（文件不存在/无法解析）→ **明确报错并终止**，不回退成 Cube（D8）。

---

## 8. 选中与交互（BL-5 / BL-6）

### 8.1 输入意图（`InputIntent`）

F-BL 的交互面只暴露**意图**，不暴露"怎么画的"：

```cpp
struct InputIntent {
  enum class Kind : std::uint8_t {
    Tap,            // 点击（选中 / 落块）
    DragBegin,      // 按下（可能是 gizmo 拖拽的开始）
    DragMove,       // 移动
    DragEnd,
    SetShape,       // 换外形
    SetColor,       // 换颜色
    DeleteSelected, // 删除选中
    Undo, Redo,
    OrbitCamera, PanCamera, ZoomCamera,
  };
  Kind kind;
  float px = 0, py = 0;             // 屏幕坐标（Tap / Drag*）
  std::uint8_t shape = 0;           // SetShape
  primitives::Color4 color{};       // SetColor
  float scalar = 0;                 // OrbitCamera/PanCamera/ZoomCamera 的增量
};
```

**为什么不用 `device::InputEvent` 直接接**：`InputEvent` 是 L0 的平台事件（触摸点、键码、
时间戳），而 F-BL 需要的是"意图"。中间的转换必须有一层（Android 手势识别 / 桌面鼠标 / 测试脚本），
把转换留在**调用方**，F-BL 只吃 `InputIntent` —— 这样 **CLI 与真机走同一条路径**，
测试也能不碰设备直接喂脚本。

> `InputIntent::Kind` 里**没有时间戳**：F-BL 的任何行为都不读时钟（总 spec §5.3 确定性）。
> 需要时间的地方（拖拽节流）由调用方的 `Refreshrate` 决定，不影响文档 hash。

### 8.2 `BlockSession`：交互会话（唯一状态机）

```cpp
class BlockSession {
 public:
  BlockSession(BlockDocument* doc, BlockPlacement* placement, ICamera* camera);
  // 喂一个意图，返回它是否改变了文档（供"是否需要重绘"判断）
  bool on_intent(const InputIntent& in);
  BlockId selected() const;
  void configure_gizmo(IGizmoSystem* gizmo);   // 把选中块接到 gizmo 上
  const std::uint64_t doc_hash() const;
};
```

状态机（**显式**，不用隐式分支）：

> **Q-BL-10（Tap 语义优先级，必须先读这条再读下面的状态图）**
>
> BL-2 说"点已有积木顶面 → 上面再叠一块"，§8.2 又说"Tap 命中块 → 选中" ——
> **同一个输入有两种语义，上一版没有裁决，这是设计漏洞。** 现在写死：
>
> | 状态 | Tap 命中的对象 | 语义 |
> |---|---|---|
> | `Idle` | 任意积木 | **选中**（不叠） |
> | `Selected` | 与当前选中**不同**的积木 | **切换选中**（不叠） |
> | `Selected` | 与当前选中**相同**的积木 | **在它上面叠一块**（BL-2） |
> | `Idle` | 空地 / 地面 | 在命中位置**落块** |
> | `Selected` | 空地 / 地面 | **落块 + 清选中**（Q-BL-4） |
>
> **一句话规则**：`Tap` = "点了一个东西 → 处理那个东西"；`叠放` 是"**对当前选中块**再点一次"。
> 这样 BL-1/BL-2/BL-3(选中) 三种语义**不重叠**，且不需要"长按/右键/模式开关"这类额外交互。
>
> **为什么不反过来（Tap 命中就叠）**：那样在没有选中态的回退路径下，用户想选中一块却叠了一块，
> 而且**选中根本无从进入**（第一次点就叠了）—— 那会让 BL-3 死锁。

```
Idle ──Tap 命中块────► Selected（选中它，不叠）
Idle ──Tap 空地──────► Idle（落块）
Selected ──Tap 同一块──► Selected（**在它上面叠一块**，BL-2；不 toggle、不取消）
Selected ──Tap 其他块──► Selected（切换选中，不叠）
Selected ──Tap 空地────► Idle（**先落块**，再取消选中）   ← Q-BL-4 用户裁定
Selected ──gizmo 拖拽──► Dragging ──DragEnd──► Selected（并把 cell 吸附到整格）
Selected ──Delete──────► Idle
```

> **与上一版的变化**：`Selected ──Tap 同一块──► Selected（不 toggle）` 现在明确为
> **"叠一块"**，而不再只是"什么也不做"。上一版为了让 §6.3 的"点顶面 → `cell.y+1`"有意义，
> 却不敢把它写进状态机，导致 BL-2 在选中态下**没有任何入口** —— 这是同一个矛盾的另一面。

> **Q-BL-4（用户 2026-11-09 裁定）**：**选中态点空地 → 在点击处落块**。
> 用户原话：「选中态点空地，物体放置在**屏幕转世界**的位置上。**需要落块**。」
>
> 落地细则：
>
> 1. **先落块、再清选中**：顺序必须是 `place → clear_selection`。反过来写，
>    落块用的 `BlockShape` / `Color4` 如果将来做成"跟随选中块的当前外形"，
>    清选中后就会退化成默认外形 —— 这类"顺序依赖"的 bug 只会在联调时暴露，
>    所以在状态机注释里直接写死顺序。
> 2. **两条落块路径共用同一个函数**（`BlockSession::commit_place_at(px, py)`），
>    `Idle` 与 `Selected` 两个分支只差"落完是否清选中"。禁止复制两份吸附逻辑。
> 3. **"屏幕转世界"的链路**：`ICamera::screen_to_ray(px, py)` → `IPicker` 求交 →
>    `PickHit.point` → §6.3 吸附规则。**落地用的是命中点吸附后的整格**，
>    不是把屏幕坐标直接塞进文档 —— 文档只认 `int32 cell[3]`（Q-BL-2）。
> 4. **落块失败（越界/超 max_rise）怎么办**：报错并**保留选中**。理由：用户此刻
>    在编辑一个具体物体，落块失败不该顺带丢掉他的选中态（失败要响，但不要连带副作用）。
>
> CLI 脚本里的期望值同步为：`tap 空地` 在选中态下 `block_count + 1` **且** `selected == 0`，
> 例如 `expect blocks 3` + `expect selected 0`（见 §9.3 脚本语法）。

### 8.3 Gizmo 接线（BL-6）

`IGizmoSystem` 的既有契约（`gizmo.h`）：

1. `configure(GizmoTargetDesc{...})` 返回 target id；
2. `on_input(InputEvent, camera)` 驱动拖拽状态机，产出 `GizmoEvent`；
3. `build_draw(camera, overlay)` 把轴渲染进 `IOverlay2D`；
4. `snapshot/restore` 做回放。

F-BL 侧要做的事**只有三件**：

| 事 | 做法 |
|---|---|
| 选中变化时更新 gizmo | `update_target(target, desc)` 把 `position` 换成选中块的 AABB 中心 |
| 收 gizmo 事件 | 实现 `IGizmoListener`，**在 F-BL 里**翻译成 `BlockDocument::move`（**吸附到整格**：`cell = round((center - origin)/cell_size)` 再走 §6.3 的占用检查） |
| 画选中框 | `IOverlay2D::push_dash` 画 12 条棱（虚线，无深度） |

> **关键约束**：gizmo 拖拽产生的是**连续世界坐标**，而积木只认整格。
> 所以"拖拽中"只更新 gizmo 的**视觉**位置，"松手"时才写文档 —— 否则一次拖拽会产生
> 上百次 `move`，撤销栈被淹没。→ **裁定 Q-BL-5**：拖拽期间不写文档，`DragEnd` 时提交一次。

---

## 9. 出图与命令回放

### 9.1 一帧的组成（`block_render.cpp`）

```
1) BlockScene: 文档 → IWorld 实体镜像（只增删改差量，稳态零结构变更）
2) build_draw_requests(world, batch, {camera, mesh_info})  → DrawRequest
3) IBatchBuilder::build()                                  → 批次
4) IPrimitiveRenderer::record_frame(cmd, batches, desc)
5) IOverlay2D: 网格线 + 选中虚线框 + gizmo 轴 → 一次 overlay pass
6) IRenderDevice::submit_command_list(cmd)                 → 像素
7) OffscreenTarget::readback_rgba8 → PNG
```

**唯一的 submit 在调用方（CLI/壳层）**：UIG2 门控要求"一帧一次 submit"，渲染器只录不发。

### 9.2 三态确定性（总 spec §5.3）

| 校验 | F-BL 的落地 | 判据 |
|---|---|---|
| 录制 vs 回放 | 同一 `CommandStream` 重放到离屏 target | 命令哈希一致 + **PNG 逐字节一致** |
| **复现**（操作可复现） | 回放 `InputIntent` 序列，从空文档重跑 `BlockSession` | `doc.hash()` 一致 + `mesh_hash` 一致 + PNG 逐字节一致 |
| 跨版本 | 文档/流带版本号 | 版本不符明确报错 |

> **F-BL 的"命令复现操作"最小闭环**（这正是总 spec §5 要预埋的能力）：
> `magicblocks-l3-cli blocks-sim --script <intents.txt>` —— 不读时钟、不读鼠标，
> 按脚本喂意图，输出逐步 `doc_hash` + 最终 PNG。脚本就是"用户操作"的文本表示。

### 9.3 CLI（无头、离屏，硬约束 B5）

```
magicblocks-l3-cli blocks --width 640 --height 400 \
      [--blocks N] [--shape cube|brick|cylinder|sphere|prism|trapezoid] \
      [--palette r,g,b] [--obj path.obj] [--cam-yaw deg] [--cam-pitch deg] \
      [--grid-cells 16] [--output blocks.png]

magicblocks-l3-cli blocks-sim --width 640 --height 400 --script sim.txt \
      [--record sim.bin] [--replay sim.bin --verify] --output sim_final.png
```

- `blocks`：**保留既有行为**（L3 已有的实例化演示），新增参数走同一入口；
- `blocks-sim`：F-BL 的**主验收入口**。脚本行格式（一行一个意图）：

```
# 注释
tap 320 200          # 命中积木 → 选中（若无积木则落块）
set-shape cylinder
set-color 0.9 0.3 0.2
tap 400 180          # 选中态下点空地 → 在屏幕→世界的命中处落块，并清选中
drag-begin 400 180
drag-move 410 170
drag-end
delete
undo
expect blocks 3      # 块数（Q-BL-4 的选中态落块必须体现在这条上）
expect selected 0    # 当前选中块 id（0 = 未选中）
expect doc_hash 5b189304596cbda8
```

`expect <键> <值>` 让脚本自带**断言**：回放过程中任一条期望不符即退出码非 0、且不写 PNG。
键只有三类：`blocks`（块数）、`selected`（选中的 `BlockId`，0 表示无）、`doc_hash`（16 位十六进制）。
这是"命令复现操作"的机械判据 —— 一个脚本同时钉死了**输入序列**与**每一步的状态**。

---

## 10. 性能与内存（C7 / M5）

| 指标 | 目标 | 何以保证 |
|---|---|---|
| draw 次数 | ≤ 7（外形种数）+ overlay 1 | 单位网格 + 实例矩阵（§7.1） |
| 稳态 VMA alloc/free | **0** | 落块只 `spawn` 实体 + 写组件；网格在启动期一次性上传 |
| 帧内堆分配 | `kMaxHeapAllocs = 0` | `IFrameArena` + `IBatchPool`（L3 既有） |
| 实例缓冲 | 每帧 `write()` 不新建资源 | `IBatchPool` 槽位（L3 既有） |
| 文档内存 | ≈ 16 B/块 + id | `Block` 是 POD、`std::vector<Block>` 紧凑 |
| 占用查询 | O(log n) | 按列压缩区间的 `IWorldGrid`（§6.4） |

**测试**：`Blocks.SteadyStateZeroVma`（连续 60 帧同文档，`AllocatorStats` 的
`vma_alloc_count` / `vma_free_count` 增量为 0）。

---

## 11. 验收门（A1–A16）

> 与 F-SK 的验收表同构；每条都有**机械判据**，不靠"看起来对"。

| # | 验收 | 判据 | 测试名（拟定） |
|---|---|---|---|
| A1 | 文档纯逻辑 | `BlockDocument` 不 include 设备/相机头；加/删/改/撤销/重做全对 | `BlockDoc.*`（≥15 例） |
| A2 | 格点吸附 | 同一点两次点击 → 同一 `cell`；点在格线上 → 稳定同侧 | `Placement.SnapIsStable` |
| A3 | 不穿模 | 200 次确定性落块，逐对 AABB 不相交 | `Placement.NoOverlapAfterNPlacements` |
| A4 | 叠放 | 点顶面 → 上一格；侧面 → 邻格；底面 → 下一格 | `Placement.StackOnTopOfBlock` 等 |
| A5 | 洪水找空位 | 满列 → 最近空位；超 `max_rise` → 明确失败；**全满/越界/起点在障碍内三条边界** | `Placement.FloodFindsNextFree`、`WorldGrid.*`（6 例，见计划阶段 1） |
| A6 | 六种外形 | 每种外形产出非空网格且 `bounds` 有限；**MeshId 数量上限被断言** | `BlockMeshes.AllShapesNonEmpty`、`BlockMeshes.ShapeCountWithinEnumBounds` |
| A7 | 选中 + 选中态落块 | Tap 命中 → `selected()` 非空；点其他块 → 切换；**选中态点空地 → 在屏幕射线的命中处落块、落完清选中**（Q-BL-4） | `Session.TapSelectsAndSwitches`、`Session.TapEmptyWhileSelectedPlaces`、`Session.TapEmptyWhileSelectedUsesHitPoint`、`Session.PlaceFailureKeepsSelection` |
| A8 | 移动吸附 | gizmo 拖到半格 → 落回整格；拖拽期间 `doc.hash()` 不变 | `Session.DragSnapsToCell`、`Session.DragDoesNotWriteUntilEnd` |
| A9 | 删除 + 撤销 | 删除后 `block_count-1`；撤销后恢复且 `hash` 回到原值 | `BlockDoc.RemoveAndUndoRestoresHash` |
| A10 | 确定性出图 | 同参数两次 CLI → PNG 逐字节一致 | `BlocksE2E.CliIsByteIdenticalAcrossRuns` |
| A11 | 稳态零 VMA | §10 | `Blocks.SteadyStateZeroVma` |
| A12 | 命令复现 | 脚本回放 → `expect blocks/selected/doc_hash` 全对 + 最终 PNG 一致；坏脚本 → 非 0 且**不写 PNG** | `BlocksE2E.ScriptReplayReproducesDoc`、`BlocksE2E.BadScriptFailsLoudly` |
| A13 | golden | ≥1 张 PNG 进 `tests/data/` 并有结构化断言（不是只比字节） | `BlocksE2E.MatchesGoldenStructure` |
| A14 | 门控 | G1/G3/G4/UIG1/UIG2/scripts 全 rc=0；**新增 ctest 项的 G7/G8 全 rc=0**；`src/features/blocks/**` 出现 `blocks` 不被 G3 误杀；`include/magicblocks/primitives/world_grid.h` 写 `voxel` 会被 G7 抓（负向自测） | `magicblocks_gate_g7_l3_no_gameplay_names`、`magicblocks_gate_g8_l3_command_parity`、`tools/tests/test_check_scripts.py` 追补用例 |
| A15 | **ASan 全量** | `build-asan` 构建后全量跑完，退出码 0 且输出无 `AddressSanitizer` / `LeakSanitizer` 字样（R1 主路径，不是兜底） | CLI 级 CTest 或阶段 6 固定步骤（见计划阶段 6） |
| A16 | **继承可见性常驻回归** | `PixelRegression.SubmitCommandListActuallyWritesPixels` 常驻绿 —— 这是 private 基类那次事故的唯一机械判据（设计 §15），必须点名写进验收，不能只留在文档叙述里 | 既有用例，列为本轮"不许改它的判据" |

---

## 11.5 本轮必须修复的 SDK 缺口（GAP 清单）

> 这三条**不是玩法需求**，是 L3/门控**自身的缺陷**。不修就无法按设计落地，
> 且"绕开"会直接违反总 spec §2.2。全部列入计划阶段 1.5 / 阶段 0，**没有"实现期再说"**。

| # | 缺口 | 证据 | 修法 | 回归判据 |
|---|---|---|---|---|
| **GAP-P1** | `IPicker::pick_scene` 的 `PickHit::normal` 恒为 `(0,0,1)`、`triangle` 恒 `kNoTriangle` | `src/primitives/picker.cpp:411-412`；`NarrowPhase` 只有 `float* out_t`（`picker.h:107`），无通道回报法线 | `NarrowPhase` 改签名为 `bool (*)(std::uint32_t, const Ray&, PickNarrow*)`，`PickNarrow{float t; Vec3 normal; std::uint32_t triangle;}`；`pick_scene` 在更近时连法线一起记（当前零外部调用方） | `PickerBvh.SceneHitReportsRealNormal`：向下打立方体顶面 → `normal≈(0,1,0)`；打侧面 → `normal≈(±1,0,0)`；`triangle != kNoTriangle` |
| **GAP-P2** | G7（`check_l3_no_gameplay_names.py`）与 G8（`check_l3_command_parity.py`）**存在但没接进 ctest** | `CMakeLists.txt:462-479` 只注册了 G1/G3/G4/deps/scripts/UIG1/UIG2/ui_scripts | 补两条 `add_test`：`magicblocks_gate_g7_l3_no_gameplay_names`、`magicblocks_gate_g8_l3_command_parity` | `ctest -R gate` 从 10 项变 12 项全绿；`world_grid.h` 里塞 `voxel` 必须让 G7 变红（负向自测写进 `tools/tests/test_check_scripts.py`，**追加到既有文件，不新建**） |
| **GAP-P3** | 基线从未提交：HEAD 只有 L0 + 部分 L2，**L1/L3/F-SK 整目录未跟踪** | `git ls-tree HEAD` 无 `src/primitives/`、`include/magicblocks/primitives/`；`git status` 133 项 | **阶段 0**：在 `baseline/l3-fsk` 分支上分阶段提交（每步过 build+test），全部绿后 `--no-ff` 合入 `main`；**工作树只开一次**给 F-BL 本体 | 合并后 `main` 上 368/368 + 12 门控全绿 |

---

## 12. 风险与对策

| # | 风险 | 影响 | 对策 |
|---|---|---|---|
| R1 | **全量测试退出期偶发堆损坏**（已于本轮定位到 private 基类，修后连跑 6 次稳定；根因未逐行坐实） | 高 | **主路径固定跑 ASan**：阶段 6 必跑一次 `build-asan` 全量（`MagicBlocks/build-asan` 已存在）并列为验收 A15；判据 = 退出码 0 且输出无 `AddressSanitizer`/`LeakSanitizer` 字样。另加全量连跑 ≥6 次作为日常门槛 |
| R2 | **`pick_scene` 的 `PickHit::normal` / `triangle` 恒为假值**（L3 缺陷 GAP-P1，见 §6.2.1） | 高 | **不绕开**：阶段 1.5 改 `NarrowPhase` 带 `PickNarrow*` 回填真实法线 + 三角形索引，并加 `PickerBvh.SceneHitReportsRealNormal` 常驻回归 |
| R3 | gizmo 拖拽与积木整格吸附打架 | 中 | Q-BL-5：拖拽期间不写文档，松手才提交 |
| R4 | `IWorldGrid` 的"按列压缩"在稀疏大场景退化 | 低 | **先定容量再定实现**：`GridSpec.half_extent` 首轮固定 16（±16 格），`set_occupied` 越界**显式返回 `Status::Invalid`**（不 clamp、不扩容）；真有更大场景需求时按新 spec 换三维哈希，**不在本轮留"必要时再换"的口子** |
| R5 | OBJ 导入让 draw 次数与外形种数解耦失败 | 低 | 首轮只允许**一个**导入外形槽位；超过则报错（D8） |
| R6 | 代码层平台无关失守 | 中 | 阶段 0 加 grep 门控断言：`src/features/**`、`src/primitives/**` 零 `#ifdef`／零 `WIN32`·`_WIN32`·`__linux__`·`ANDROID`·`__APPLE__`。**可断言项，必须绿**（与 R9 分工：R6 = 代码里不许有平台分支；R9 = golden 只有一份） |
| R7 | 玩法词泄漏进 SDK | 中 | **两套门控**：G3 扫 `src/backend/**`（`blocks` 违规）；G7 扫 `include/magicblocks/primitives/**` 与 `src/primitives/**`（`voxel`/`积木` 等违规，`block`/`blocks` 标识符按 G7 既有白名单放行，见 §3.3）；两者都要**接进 ctest**（G7 当前未注册，列 GAP-P2） |
| R8 | 既有 368 测试回归 | 高 | G5：任何改动后必须 368+ 全绿且 0 跳过 |
| **R9** | **golden 只有单平台基线**（SKILL `:52`/`:138` 要求"双平台同一套测试与 golden"） | **高** | **本轮显式未达成**（本机只有 Windows+SwiftShader，无 Linux 环境）。<br>**后果**：①golden PNG 与 `doc_hash` 只在 Windows+SwiftShader 上生成过一次，Linux/Android/iOS 下曲面细分与浮点差异可能让 golden 不匹配；②跨平台回归**无法在本轮拦截**。<br>**主路径处置**：`tests/data/README.md` 的 golden 段写明"仅在 Windows+SwiftShader 生成与校验"；任何改动吸附容差／相机默认值／网格尺寸后，**必须两平台各生成一次**。<br>**必补工单**：合并后立即开"F-BL golden 双平台基线"任务（Linux+lavapipe 构建 + 跑全部 golden + 差异归因），**在下一个 F-* 迭代开工前完成**。<br>（与计划 §9 R9 逐字一致） |

---

## 13. 裁定记录

> 用户 2026-11-09 授权：**"继续"** —— 即按 F-SK 的节奏推进，并采用本文给出的推荐裁定；
> 随后用户单独裁定 **Q-BL-4**（选中态点空地要落块），该条已按其原话改定。

| # | 问题 | **最终裁定** | 依据 |
|---|---|---|---|
| **Q-BL-1** | `IWorldGrid` 放 L3 还是 L4 | ✅ **L3**（`primitives/world_grid.h`） | **总 spec P10 既有条目**，本轮按通用能力落地；多客户（F-BL + F-VX）。论证链不是"玩法需要→补 L3"（见 §3.1 纪律） |
| **Q-BL-2** | 位置存整数格点还是浮点矩阵 | ✅ **`int32 cell[3]`** | 总 spec Q1（A 方案），命令流 hash 稳定 |
| **Q-BL-3** | 是否做几何合并 | ✅ **不做**；单位网格 + 实例矩阵，draw ≤ 外形种数 | `IBatchBuilder` 已按 MeshId 批；合并会把"改一块"退化成重建 |
| **Q-BL-4** | 选中态点空地 | ✅ **在屏幕→世界的命中位置落块**，落完再清选中（用户 2026-11-09 裁定） | 用户原话"物体放置在屏幕转世界的位置上，需要落块"；细则见 §8.2 |
| **Q-BL-5** | gizmo 拖拽期间是否实时写文档 | ✅ **否**；`DragEnd` 时提交一次并吸附整格 | 撤销栈不被淹没；吸附在松手时才有意义 |
| Q-BL-6 | `PickHit` 是否够用 | ⚠️ **部分关闭**：`point` 有真值；`normal`/`triangle` 是假值 → 升级为 **GAP-P1**，阶段 1.5 修 L3（§6.2.1） | `primitives/picker.h:25-45`、`src/primitives/picker.cpp:406-414` |
| Q-BL-7 | 是否支持旋转 | ✅ **接口预留 `rot_quarter`，首轮恒 0** | 旋转破格，属 VX/后续 |
| Q-BL-8 | `BlockId` 是否复用 | ✅ **永不复用**（单调递增） | 同 F-SK D-SK-3 |
| **Q-BL-9** | `Cell` / `GridSpec` 归 L3 还是 L4 | ✅ **都归 L3**（`world_grid.h` 唯一定义），L4 只 include 不重定义 | 它们是 `IWorldGrid` 的形参类型，放 L4 会倒置依赖（§3.2.1）；用 `static_assert` 机械校验 |
| **Q-BL-10** | Tap 命中积木：选中还是叠放 | ✅ **`Tap` 一律"处理点到的东西"**：Idle→选中、选中别的块→切换、**选中同一块→在它上面叠一块** | BL-2 与 BL-3 同一输入的语义冲突必须裁决（§8.2）；若反过来则"选中无从进入"会让 BL-3 死锁 |

---

## 14. 与总 spec 的对应

| 总 spec | F-BL 落地 |
|---|---|
| §4 F-BL"`MeshFactory`/`Obj` → `ICamera` 透视 → `IPicker` 点选 → 实例化绘制；放置 = 包围盒 + `IWorldGrid` + 沿法线洪水搜索（`min.y ≥ 0`）；选中 = `IOverlay2D` 无深度虚线包围盒 + `IGizmoSystem` 屏幕恒定尺寸" | §6（放置/洪水）、§8.3（选中/gizmo）、§7（实例化绘制） |
| §4 P1 图元 / P3 OBJ | §7.2 / §7.4 |
| §4 P4 `IPicker` | §6.2 |
| §4 P5 `IOverlay2D` 屏幕恒宽 | §5.2（网格线）、§8.3（选中框） |
| §4 P10 `IWorldGrid` 洪水找空位 | §6.4（**总 spec 既有条目，本轮落地**） |
| §4 P11 `IGizmoSystem` 屏幕恒定尺寸 | §8.3 |
| §2.1 依赖方向 / 零玩法分支 | §3.2 + G3 门控 |
| §5.3 录制/回放/复现 + 确定性 | §9.2、A12 |
| §5.4 离屏一等公民 + CLI | §9.3 |
| §6 C7 大块分配 + 稳态零 VMA | §10、A11 |
| §11 验收门 3/4/5 | A10/A12/A13 |
| **用户"必须一项一项来"** | 本文只做 F-BL；F-VX/F-ST/F-PN 不设计（§2.3 划了边界） |

---

## 15. 本轮先修的地基问题（已在本次会话完成）

| 问题 | 处置 | 结果 |
|---|---|---|
| `OffscreenTarget` 用 private 继承 `IFrameTargetImage` → `submit_device_frame` 恒拿不到颜色附件 → **L3/L4 全链路出不了图** | `src/backend/vulkan/vulkan_target.h` 基类改 public + 20 行注释钉死原因 | 368/368 全绿，连跑 6 次一致；F-SK 的 5 张 PNG 内容校验通过 |
| 该 bug 是 F-SK spec §8 点名的"L3 缺口"之一 | 同上 | F-BL 的出图通路随即可用 |

> **不自作主张的说明**：这次改动是**一行级**的地基修复，不做它就无法验证任何 L4 玩法。
> 其余所有设计/实现仍在 spec + plans 门后等你确认。
