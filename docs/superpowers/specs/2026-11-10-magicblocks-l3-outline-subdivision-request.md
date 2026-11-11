# L3 需补能力：轮廓细分挤出（F-ST 前置）

- 日期：2026-11-10
- 提出来源：`docs/superpowers/specs/2026-11-10-magicblocks-l4-fst-stamp-design.md` §12.1
- 用户裁定：**选项 A —— 补 L3 细分能力**（不允 L4 自建，不允降级放行）
- 状态：待实现；F-ST 本体实现阻塞于此

---

## §1 一句话

给 L3 加一个**能控制盖面密度**的挤出：轮廓 → 内部均匀细分 → 挤出成封闭实体。
现有 `from_outline` 保持不变（它就是"不细分"，很多场景要的就是它）。

---

## §2 为什么必须有（证据）

F-ST 母规格 R3：**"透视下靠深度遮挡凸显五官"**。
实现手段是 `l3_scene_textured.vert:60`：

```glsl
float relief = texture(uStampTex, inUv).g * max(inMaterial2.y, 0.0);
...
world.xyz += relief_dir * relief;   // :69
```

这是**顶点位移**：只有顶点能移动。

而当前 `from_outline` 产出的盖面顶点**只有轮廓点**：

| 证据位置 | 内容 |
|---|---|
| `MagicBlocks/src/primitives/mesh_factory.cpp:902` | `tessTesselate(tess, TESS_WINDING_NONZERO, TESS_POLYGONS, 3, 2, nullptr)` —— 无密度参数 |
| `mesh_factory.cpp:911` | `const int vcount = tessGetVertexCount(tess);` —— 顶点数直接取自 tess 输出 |
| `third_party/_src/libtess2-v1.0.2/Source/tess.c:984-995` | `tessSetOption` 只支持 `TESS_CONSTRAINED_DELAUNAY_TRIANGULATION` / `TESS_REVERSE_CONTOURS`，**没有密度选项** |

### §2.1 一个必须说清的误区

看到 `TESS_CONSTRAINED_DELAUNAY_TRIANGULATION` 可能以为能让 libtess2 细分 —— **不能**。

`tess.c:451 tessMeshRefineDelaunay()` 的实现只有一步：
```
while (!stackEmpty(&stack)) { if (!tesedgeIsLocallyDelaunay(e)) tessMeshFlipEdge(mesh, e); }
```
它是**边翻转**（edge flip），在**已有顶点**之间重新连线，使三角形更"正"。
它**不插入任何新顶点**。全库源码 grep `steiner` / `SteinerPoint` **零命中**。

结论：细分必须由 L3 自己实现，libtess2 只能用在"给定了点集之后怎么连"这一步。

---

## §3 需求

### §3.1 接口

```cpp
// MagicBlocks/include/magicblocks/primitives/mesh_factory.h

// 带盖面密度控制的挤出。
//
// 与 from_outline 的唯一差别：盖面不是"轮廓原样三角化"，而是先把轮廓围成的
// 区域按 target_edge_len 细分出一批内部顶点，再三角化。内部顶点使顶点位移
// 这类逐顶点效果（例如印面浮雕）能在区域中部生效。
//
// 契约：
//  - 结果仍是**封闭实体**：每条无向边恰好 2 个三角形（与 from_outline 同判据）
//  - 轮廓边界顶点**必须保留**（细分点不得吃掉原始轮廓点，否则外形会走样）
//  - 所有盖面顶点都在轮廓围成区域**内部或边界上**，不得外溢（外形不变）
//  - target_edge_len <= 0 / 非有限 → 返回空 MeshData（不 clamp，不猜默认值）
//  - 预计三角形数超过 max_triangles → 返回空 MeshData（拒绝，不静默降级）
//  - 轮廓空 / 退化 → 返回空 MeshData（同 from_outline）
MeshData from_outline_subdivided(const OutlineSet& outline, const ExtrudeParams& params,
                                 float target_edge_len,
                                 std::uint32_t max_triangles = 200000);
```

**为什么不用 `IMeshFactory` 虚方法**：`from_outline` 现在是自由函数（`mesh_factory.h:72`），
`IMeshFactory` 的虚方法都是"单位图元"。细分挤出需要轮廓参数，与 `from_outline` 同级更自然。
若你更希望它是 `IMeshFactory` 成员，也没有障碍 —— 但请保持与 `from_outline` 同样的语义层级。

### §3.2 语义要求

| # | 要求 | 为什么 |
|---|---|---|
| S1 | 保持 `from_outline` 的输出规则：上下盖顶点分开、法线 ±Z、侧壁逐边生成、`z_base/z1` 一致 | 两者必须可互换，行为不能分叉 |
| S2 | uv 仍写世界坐标 `(x,y)` | `from_outline` 现在就是这样（`mesh_factory.cpp:935-936`）；归一化是 L4 的职责，不在 L3 |
| S3 | 确定的输出：同一输入两次调用，顶点/索引**逐字节相同** | 全仓确定性约束；不能依赖哈希表遍历顺序 |
| S4 | 轮廓边界顶点是输出的子集，不得被"塌陷"吸收 | 外形走样会让印章轮廓变形 |
| S5 | 内部顶点按三角网重心/质心放置，**不得落在区域外** | 外溢会改变外形 |
| S6 | 带孔轮廓正确：孔内不得出现三角形 | `OutlineSet.holes` 已支持 |
| S7 | 性能：半径 0.5 圆、`target_edge_len=0.005`（约 7.3 万三角形）应在 **200 ms 内**完成 | F-ST 每次改参数都会重建 |

### §3.3 建议实现路线（不强制，只要满足 S1–S7）

1. 用现有 `from_outline` 的路径拿到**粗三角网**（libtess2，TESS_POLYGONS）；
2. 对每个粗三角形做**1-to-4 或 1-to-N 递归细分**（Loop 式中间点），
   细分深度由 `target_edge_len` 决定；
3. 对落在轮廓**外部**的新顶点做剔除 —— 用 libtess2 的 `TESS_BOUNDARY_CONTOURS`
   或 Clipper2 的 `PointInPolygon`（依赖池里已有 `Clipper2_2.0.1`）判断；
4. 边界处的三角形做一次**缝合**（把越界的部分裁掉、用轮廓顶点补三角形），
   保证 S4（边界顶点保留）与 S5（不越界）；
5. 剩下就是 `from_outline` 的盖面/侧壁生成逻辑 —— **建议直接复用**，
   把"拿到三角网"与"三角网 → 实体网格"拆成两个内部函数，避免两套侧壁代码。

> 步骤 3/4 是难点。若你判断用 Clipper2 裁剪更稳，也可以：
> 生成密集点集 → Clipper2 求 `intersect(点集凸包/网格, 轮廓)` → 用裁剪后的多边形调 libtess2。
> 只要最终满足 S1–S7，路线自定。

### §3.4 不做（避免范围蔓延）

- 不做自适应细分（不按曲率加密）—— F-ST 要的是均匀密度；
- 不做 tetrahedralize / 体积网格；
- 不改 `from_outline` 现有行为（它的 12 个既有单测必须原样通过）；
- 不加 `IMeshFactory` 上的虚线方法除非你判断更合适。

---

## §4 验收标准

### §4.1 单测（加在 `MagicBlocks/tests/magicblocks_l3_outline_test.cpp`）

| 用例 | 断言 |
|---|---|
| `Subdiv.Watertight` | 每条无向边恰好 2 个三角形（与 `Extrude.Watertight` 同判据） |
| `Subdiv.DenserThanPlain` | 同一轮廓下三角形数 ≥ `from_outline` 的 10 倍（半径 0.5 圆、`target_edge_len=0.05`） |
| `Subdiv.BoundaryPreserved` | 原始轮廓点全部出现在输出顶点中（容差 1e-6） |
| `Subdiv.NoVertexOutside` | 所有盖面顶点满足 `inside_or_on(outline)` |
| `Subdiv.HoleRespected` | 带孔轮廓：孔内无顶点、无三角形 |
| `Subdiv.Deterministic` | 两次调用，`vertices`/`indices` 逐字节相同 |
| `Subdiv.RejectsNonPositiveEdgeLen` | `target_edge_len<=0` / NaN / inf → 空网格 |
| `Subdiv.RejectsOverBudget` | `max_triangles` 设小 → 空网格（不是截断输出） |
| `Subdiv.EmptyOutline` | 空轮廓 → 空网格，不崩 |
| `Subdiv.LegacyUnaffected` | `from_outline` 全部既有断言不变 |

### §4.2 回归

- `ctest --test-dir build -j 1` → 全绿（当前基线 401/401）
- 10 道门禁全 rc=0（`check_layer_purity` / `check_no_geometry_shader` / `check_no_raw_alloc` /
  `check_l3_no_gameplay_names` / `check_l3_command_parity` / `check_l3_no_alloc_in_hotpath` /
  `check_offscreen_first` / `check_scene_no_clock` / `check_ui_no_readback`）
- 5 张既有 golden PNG 逐字节不变

---

## §5 完成后的交接

L3 落地并全绿后，回一句"细分好了"，我接着做 F-ST 本体
（`stamp_document` / `stamp_pipeline` / CLI `stamp` 子命令 / 单测 / golden）。
F-ST 的设计规格已经全部就绪，不依赖别的未定事项。
