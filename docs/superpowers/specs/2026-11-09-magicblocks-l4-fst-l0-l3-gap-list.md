# L4-F-ST 印章 —— L0–L3 需补能力清单（交补齐方，非 F-ST 实现方）

日期：2026-11-09
状态：**缺口清单（待补齐方交付）**
关联：
- 总 spec：`docs/superpowers/specs/2026-11-03-magicblocks-render-engine-sdk-design.md`（下称"总 spec"）
- L3 spec：`docs/superpowers/specs/2026-11-04-magicblocks-l3-primitives-design.md`
- F-SK 依赖评估体例：`docs/superpowers/specs/2026-11-06-magicblocks-l4-fsk-dependency-assessment.md`

---

## 0. 一句话结论

**F-ST（印章）需要的能力里，L0–L3 已备齐约 80%**；真正卡住的只有**一项架构级缺口：纹理采样通道**。
另有三项小缺口（倒角挤出、双通道纹理格式、印面深度用法的 L4 表达）属可选项，按下面优先级决定补不补。

> **口径**：本清单只列"F-ST 需要、而 L0–L3 现在没有"的东西。
> 凡 L3 已有就**不重复要** —— 不趁这次机会加通用性无关的抽象。

---

## 1. 基线核查（2026-11-09 实测）

| 断言 | 结论 | 证据 |
|---|---|---|
| 构建 | ✅ 可用（`tools/build.ps1`） | 实跑 |
| 测试 | ✅ **389/389 全绿（必须以 `-j 1` 串行跑）** | 实跑 |
| ⚠️ 并行跑测试会假失败 | **`ctest -j 4` 会有 5~29 条用例以 `0xC0000409` 失败**，串行全绿；同一进程跑完 368 条单测也全绿 | 本轮实测，见 §6 |
| 门控 G1/G3/G4 | ✅ rc=0 | 实跑 `tools/check_*.py` |
| L4 层现状 | ✅ `features/sketch` 已落地（F-SK） | `include/magicblocks/features/sketch/` |

---

## 2. 依赖矩阵：F-ST 需要 vs L0–L3 实际提供

| # | F-ST 需要 | 现状 | 提供者 | 缺口 |
|---|---|---|---|---|
| D1 | 印章底板轮廓（圆/方/任意 2D 轮廓）→ 实体 | ✅ **完整** | `IMeshFactory::FromOutline` + `ExtrudeParams` | — |
| D2 | 程序化图元（圆柱/圆台做把手） | ✅ **完整** | `IMeshFactory::cylinder / trapezoid` | — |
| D3 | 金属把手材质（metallic/roughness） | ✅ **完整**（参数位齐） | `StandardParams{metallic, roughness, emissive}` | 见 **D11**（`std_pbr` 未真正实现 PBR） |
| D4 | 俯视正交/透视相机 | ✅ **完整** | `ICamera`（正交+透视+`screen_to_ray`） | — |
| D5 | 落印拾取（射线 ∩ 平面 / 三角形） | ✅ **完整** | `IPicker::intersect_plane / intersect_triangle / pick_mesh_screen` | — |
| D6 | 顶点带 UV（贴图坐标） | ✅ **完整** | `primitives::Vertex` 有 `uv[2]`（48B 布局） | — |
| D7 | 深度附件 + 遮挡（透视下"五官"被凸起挡住） | ✅ **完整** | `TargetDesc::with_depth` + `DepthState` | — |
| D8 | 离屏 + 读回 + PNG | ✅ **完整** | `create_offscreen_target` + `readback_rgba8` + `write_png_rgba8` | — |
| D9 | 实例化批量绘制（多枚印章） | ✅ **完整** | `IBatchBuilder` + `draw_instanced` | — |
| **D10** | **纹理采样：印面 = 颜色 + 深度双贴图** | ❌ **完全没有** | — | **P0 架构级缺口** |
| D11 | 真正的 PBR 金属（IBL/BRDF） | ⚠️ **参数位齐、着色近似** | `StandardParams{metallic, roughness}` 有；`l3_scene.frag` 用 roughness/metallic 调 Blinn-Phong 高光（非 GGX/IBL），**观感上"金属"可辨但不够真** | **P2 视觉品质可选** |
| D12 | 挤出倒角（圆角底板观感） | ❌ 无 | `ExtrudeParams` 无 bevel | **P2 可选** |
| D13 | 双通道纹理格式（R8G8） | ❌ 无 | `Format` 只有 RGBA8/BGRA8/R8/深度格式 | 见 **D10 的实施建议** |
| D14 | 印章"盖到纸上"的印痕（落地痕迹） | ❌ 无 | — | F-ST 用叠加 2D 面近似（L4 自己拼） |

**统计**：14 项里 **9 项已具备**，1 项架构级缺口（D10），4 项可选（D11/D12/D13/D14）。

---

## 3. P0 缺口：纹理采样通道（**这是唯一真正卡住 F-ST 的**）

### 3.1 为什么现在做不了

总 spec §4 F-ST 定义：

> 印面 = **颜色 + 深度双贴图**（透视下靠深度遮挡凸显五官）

但 L1/L3 的渲染管线**在结构上不支持采样纹理**：

| 事实 | 证据 |
|---|---|
| 没有公共描述符集 | `pipeline.h:20` 注释原文：「本轮不建描述符集 → `IPipeline::descriptor_set_count()` 必须为 0」 |
| 没有绑定纹理的命令 | `ICommandList` 只有 11 个录制方法（`command.h`），**没有** `bind_descriptor_set` / `bind_texture` |
| 材质参数只走实例属性 | `material.h` 头注释：push constant 只放 view_proj，材质参数逐实例 |
| **没有任何 shader 采样纹理** | 全仓 `shaders/*.frag` grep `sampler`/`texture(` → **0 命中** |
| `ITexture` 只能上传/读回，不能被渲染管线引用 | `texture.h` 无 sampler binding 语义 |

也就是说：`ITexture` 有了，但**从"一张纹理"到"片元着色器能采样它"之间的整条链路不存在**。

### 3.2 建议的补法（最小、且不破坏既有不变量）

> **原则**：只补"一张可采样的 2D 纹理"，**不做** bindless / 纹理数组 / 多集描述符。
> 保持 R4「push constant 优先」不变，只加**一个**描述符集位。

| 子项 | 内容 | 必须满足 |
|---|---|---|
| **P0-1** | `PipelineDesc` 增加 `bool uses_sampled_texture`（或 `u32 sampled_texture_slots`） | 默认 false → **既有管线行为与 golden 逐字节不变** |
| **P0-2** | `ICommandList` 增加 `bind_sampled_texture(const ResourceId& texture, const ResourceId& sampler, u32 slot)` | ⚠️ 会打破「`ICommandList` 恰好 11 个方法」的既有不变量 → **需要用户/架构裁定**（见 §3.4） |
| **P0-3** | L1 后端：`VulkanPipeline` 按 `uses_sampled_texture` 建 descriptor set layout；`VulkanCommandList` 支持绑定；`submit_device_frame` 下发 `vkCmdBindDescriptorSets` | 单测：绑定纹理后渲染出的像素随纹理内容变化 |
| **P0-4** | L3 增加一个**可采样纹理的 shader 模板**（`std_pbr_textured`）供 L4 复用 | ⚠️ 注意：`IShaderLibrary::register_template` 目前**没有任何调用点** —— 现有 CLI/测试都是"手写 `PipelineDesc` + 内嵌 SPIR-V"。所以这一步究竟走"登记进 library"还是"按既有样板直接给 `PipelineDesc`"，取决于补齐方的裁定；**F-ST 只要求有一个可复用的入口，不强制形式** |
| **P0-5** | `FromOutline` 的盖面 UV 目前是**世界坐标原样 `(x, y)`**（`mesh_factory.cpp:935-936`），**不是 `[0,1]` 归一化** | 现有用法是"世界空间平铺"，对格子/砖纹合适；但印章需要**"把一张印面图案恰好铺满底板"**。要么 F-ST 自己按底板包围盒重算 UV，要么给 `ExtrudeParams` 加 `uv_mode{World, Normalized}`。**建议**：F-ST 侧处理即可（拿 `IMesh::bounds()` 归一化），**不一定要 L3 改** |
| **P0-6** | 纹理格式：**`Format::R8G8_UNORM`** 便于"颜色 + 深度"双通道打包 | 若不加，可用 RGBA8 存（R=颜色, G=深度, B=A=0），**可不加** |

### 3.3 深度贴图到底怎么用（F-ST 的语义，决定 P0 的最小形态）

"印面 = 颜色 + 深度"在玩法上的含义：

```
印面是一张平面（底板正面 z = 0）
  u,v  → 查纹理
  color = tex.r        （这一点的颜色，是印油/釉色）
  depth = tex.g        （这一点凸起多少，是五官/纹样的高度）
  → 顶点/片元沿 +Z 位移 depth * max_relief
```

两种实现路径：

| 路径 | 做法 | 需要的 L0–L3 |
|---|---|---|
| **A. 顶点位移（推荐）** | 印面细分成网格，顶点着色器按 uv 采样深度图做位移；不需要片元采样 | 仍需 P0-1/2/3/4（只是采样发生在 VS） |
| **B. 视差/法线贴图** | 只采样颜色，用深度图做视差偏移或转法线 | 同 A，另需切线空间 |
| **C. 纯几何** | 不采样，把图案做成一堆小凸块几何 | **不需要补任何东西**（但玩法语义变成"图案=几何"） |

> **请补齐方注意**：A/B 都要 P0-1..P0-4。C 不需要补，但离总 spec 原文更远。
> 详见 §5「给用户的建议」。

### 3.4 需要裁定的架构问题（补 P0-2 前必须答）

`ICommandList` 现在**恰好 11 个录制方法**，F-SK 设计稿（Q-SK-5）曾明确把"改 L1 公共接口"列为不可接受方案。
补纹理采样**必然要动** L1 公共接口，因此这属于**架构级裁定**，不能由实现方自行决定：

| 方案 | 代价 | 评价 |
|---|---|---|
| **X1** 给 `ICommandList` 加第 12 个方法 `bind_sampled_texture` | 破坏"恰好 11 个"的既有不变量；需要同步更新 SQL/门控/测试 | 最直接、最易懂 |
| **X2** 把纹理绑定塞进 `bind_pipeline` 的重载 / `RenderPassDesc` | 不动方法数，但语义含混（纹理是逐 draw 的，不是逐 pass 的） | 会埋坑 |
| **X3** 用 push constant 传纹理"槽位索引"，纹理表放 UBO | 仍然需要描述符集去访问那张纹理表 | 绕不开 |
| **X4** 本轮不做纹理，F-ST 走 §3.3-C | 零 L1 改动，但偏离总 spec 原文 | 见 §5 |

**我的建议**：如果要在这一轮按总 spec 原意落地 F-ST，选 **X1** —— 它是唯一"诚实"的做法；
"恰好 11 个"只是当时的实现事实，不是不可违背的契约（总 spec §4 R3 列的就是 11 个能力，
其中 `bindPipeline` 的职责扩展到"绑定采样器"是自然的）。

---

## 4. P2 可选缺口（不补也能出图，补了观感更好）

| # | 缺口 | 影响 | 补法 | 建议 |
|---|---|---|---|---|
| **D11** | `std_pbr` 只有参数、没有真正的 PBR 着色 | 金属把手看起来只是"亮的纯色"，不像金属 | 换/加一个 `l3_scene.frag` 的实现（加环境反射近似即可，不需要 IBL 贴图） | **建议补**，成本小、观感差异明显 |
| **D12** | `ExtrudeParams` 无倒角 | 底板边缘是硬直角，不够"印章感" | `bevel` + `bevel_radius`（沿轮廓边做一轮 offset 收窄再抬起） | **可选**，F-ST 首轮可以硬直角 |
| **D13** | 无 `R8G8_UNORM` | 双通道只能用 RGBA8 打包（浪费带宽） | `Format` 加一项 + backend `vk_format` + `DeviceCaps.texture_formats` | **可不加**（G 通道用 RGBA8 的 B 位即可） |
| **D14** | 无"盖印痕迹"（落在纸上的印痕扩散） | 缺"按下 → 回弹 → 留下印痕"的反馈 | F-ST 用 `IOverlay2D` 或一个贴花面近似，**L4 自己拼** | **F-ST 侧解决**，不用 L3 补 |

---

## 5. 给用户的建议（三条路线，按对总 spec 的忠实度排序）

| 路线 | 需要的 L0–L3 补充 | 工期感觉 | 忠实度 |
|---|---|---|---|
| **路线一：完整纹理双通道** | **P0-1..P0-4（含 X1 裁定）** + 可选 D11/D12（P0-5 由 F-ST 侧自理） | 最大 | **完全符合总 spec §4 F-ST** |
| **路线二：几何印面 + PBR 金属** | 只需 D11（PBR 实现），可选 D12 倒角 | 最小（几乎不用补） | 偏离"双贴图"原文，但玩法元素（底板/印面/把手/五官凸起）都在 |
| **路线三：先二后一** | 本轮补 D11（小），F-ST 按几何印面落地出图验收；纹理双贴图单开一轮补 P0 | 中等 | 分两步逼近原文 |

> **我的推荐是路线三**：因为 D11 是"小改动、大观感"，而 P0 是"大改动、需架构裁定"，
> 二者混在一轮里会让 F-ST 的验收被一个 L1 重构拖住。但这属于你的裁定，我不擅自定。

---

## 6. 附带发现：并行跑测试会假失败（与 F-ST 无关，但会误导验收）

| 现象 | 结论 |
|---|---|
| `ctest -j 4` → 5~29 条用例 `0xC0000409` 失败 | **不是回归**：串行 `-j 1` 全绿（389/389）；同一进程一次跑完 368 条单测也全绿 |
| 单条用例单独起进程跑 | 通过 |
| 根因方向 | 多进程并发争用同一 SwiftShader 驱动/临时目录；现象集中在 `vkDestroyImage: Invalid device`（设备已销毁后还调） |
| 对后续工作的要求 | **验收一律用 `ctest -j 1`**，并在文档里写明；否则每次都会误判成"新代码引入了回归" |

> 建议补齐方顺手加一条：CMake/CTest 侧把可并行的用例打上 `RESOURCE_LOCK vulkan`，
> 从根上消除这个假失败。这不阻塞 F-ST。

---

## 7. 补齐交付后的验收口径（给补齐方）

补齐方交付时请按此自检，F-ST 方据此复核：

| 项 | 验收 |
|---|---|
| 基线不回归 | `ctest -j 1` **389/N 全绿**，既有 5 张 golden 逐字节不变 |
| 门控全绿 | G1（无 GS）/G3（分层纯度）/G4（无裸分配）/G7（L3 无玩法名）/G8 全部 rc=0 |
| P0-1 | `uses_sampled_texture=false` 时，既有管线行为与像素**逐字节不变**（用既有 golden 断言） |
| P0-3 | 新单测：同一 mesh、同一相机，**换纹理内容 → 输出像素随之变化**（证明采样真的通了） |
| P0-3 | 新单测：纹理绑定 + 实例化 draw 同时可用（印章要多枚） |
| P0-4 | 新 shader 模板在 `check_no_geometry_shader` 下 rc=0（只有 vert/frag） |
| P0-5（若由补齐方承担） | `FromOutline` 上盖 UV 覆盖 `[0,1]²`（单测断言）。**默认可不由补齐方承担**，F-ST 侧按 `IMesh::bounds()` 归一化 |
| 文档 | 更新 `MagicBlocks/docs/L3_DELIVERY.md` + 总 spec §4 对应行状态 |

---

## 8. 明确不需要 L0–L3 补的（避免多要）

| 项 | 理由 |
|---|---|
| `IBlockSet` / `IBlockBrush` / `IShapeTemplate` / `IVoxelizer` | 属叠叠乐 F-VX，与印章无关 |
| `IGizmoSystem` | 印章无 gizmo 需求（L3 spec §12 用途矩阵已标 ✖） |
| 阴影 / 动画 / 骨骼 / 多线程 / 多队列 | 总 spec §10 明确不做 |
| OBJ 加载增强 | 印章用程序化轮廓即可，不改 loader |
| 运行时体素 | 总 spec 已裁定不做 |
| F-ST 的"盖印痕迹 / 回弹动画" | F-ST 侧用 `IOverlay2D` 拼，不新增 L3 |
