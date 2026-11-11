# MagicBlocks L4 玩法 F-ST：印章 —— 设计规格

- 版本：v2（2026-11-10）—— v1 的 §15 四个开放问题已由用户裁定，见 §16
- 状态：**已裁定**；L3 细分能力（§12.1 选项 A）为前置条件，落地后开始实现
- 上游母规格：`docs/superpowers/specs/2026-11-03-magicblocks-render-engine-sdk-design.md`（§276 F-ST 行）
- 层规格：`docs/superpowers/specs/2026-11-04-magicblocks-l3-primitives-design.md`（§12 用法矩阵 F-ST 行）
- 同级先例：`docs/superpowers/specs/2026-11-05-magicblocks-l4-fsk-sketch-design.md`（F-SK）
- 前序缺口清单：`docs/superpowers/specs/2026-11-09-magicblocks-l4-fst-l0-l3-gap-list.md`

---

## §0 用户指令（原文照录）

> 2026-11-03-magicblocks-render-engine-sdk-design.md 当前已经完成了L0-L3，现在开始进行L4的落地。这个已经开始是玩法的构建了，必须一项一项来。 1. 使用L0-L3搭建的引擎基础，开始F-ST的印章功能实现。

> 告诉我你需要L0~L3补充什么，我让他们补，你等我消息

> 开始协助修复，使用路线A。完成最终印章功能

三条指令合成的约束：
1. **一项一项来**：F-ST 只做印章，不顺手做别的玩法。
2. **不越界要能力**：L0–L3 该补的必须点名，补完再动 L4；L4 不得靠开洞绕过。
3. **路线 A**：P0 纹理采样通道（set 0 / binding 0 单槽）由 L0–L3 补齐，F-ST 在其上实现。

---

## §1 母规格对 F-ST 的要求（逐字引用）

`2026-11-03-...-design.md` §276：

> | F-ST 印章 | 底板轮廓 FromOutline / 模板；印面 = 颜色 + 深度双贴图（透视下靠深度遮挡凸显五官）；把手 = PbrParams 金属 + 圆柱/圆台 |

`2026-11-04-...-l3-primitives-design.md` §12 用法矩阵：

> | 印章 F-ST | 俯视正交/透视 | MeshRef(FromOutline) + MaterialRef(金属) | std_pbr（metallic=0.9, rough=0.2） | ✖画笔 | ✖Gizmo | ✅平面/三角形拾取 | ✖覆盖层 |

拆成可验收的五件事：

| # | 母规格要求 | 落地手段 |
|---|---|---|
| R1 | 底板轮廓由 FromOutline 或模板给出 | `IMeshFactory::from_outline` + 内置模板集 |
| R2 | 印面 = 颜色 + 深度双贴图 | P0 通道 RGBA8：R=色、G=深，`l3_scene_textured.vert` 顶点位移 |
| R3 | 透视下靠深度遮挡凸显五官 | 顶点位移 + 深度附件（非视差近似） |
| R4 | 把手 = 金属 + 圆柱/圆台 | `cylinder` / `trapezoid` + `metallic=0.9 / roughness=0.2` |
| R5 | 落印拾取 | `IPicker::intersect_plane` / `pick_mesh_screen` |

---

## §2 基线（实测，非声称）

在本工作区 `.agents/worktrees/20261110-l4-fst`（分支 `20261110-l4-fst`，基线 `854ab2d` + `0415e87`）：

| 项 | 结果 | 命令 |
|---|---|---|
| 构建 | 通过 | `MagicBlocks/tools/build.ps1` |
| 单测 | **401/401 通过**，0 失败 | `ctest --test-dir build -j 1` |
| 分层纯度 | rc=0，120 文件 | `tools/check_layer_purity.py` |
| 无几何着色器 | rc=0，180 文件 | `tools/check_no_geometry_shader.py` |
| 无裸分配 | rc=0，160 文件 | `tools/check_no_raw_alloc.py` |
| L3 无玩法名 | rc=0，32 文件 | `tools/check_l3_no_gameplay_names.py` |
| L3/backend 命令对等 | rc=0，32 文件 | `tools/check_l3_command_parity.py` |
| 热路径无分配 | rc=0，32 文件 | `tools/check_l3_no_alloc_in_hotpath.py` |
| 离屏优先 | rc=0 | `tools/check_offscreen_first.py` |
| 场景无时钟 | rc=0，3697 行 | `tools/check_scene_no_clock.py` |
| UI 无回读 | rc=0，12 文件 | `tools/check_ui_no_readback.py` |

> 注：`ctest -j N`（N>1）在本仓库会出现假失败（多个用例并发抢同一 Vulkan 设备，
> `0xC0000409`）。**验收一律 `-j 1`**。

---

## §3 本轮开工前修掉的一个静默错误（`0415e87`）

P0 通道补齐后，`l3_scene_textured.vert:60` 读：

```glsl
float relief = texture(uStampTex, inUv).g * max(inMaterial2.y, 0.0);
```

它把 `layout(location = 10) in vec4 inMaterial2` 的 **.y 当作 `max_relief`**。
但当时的 `InstanceAttribs`（`batch.h`）是：

```
model[16]@0(64B) base_color[4]@64 emissive[3]@80 roughness@92 metallic@96 _pad[3]@100 → 112B
```

`.y` 落在 `_pad[0]`，**恒为 0**。后果：
- `relief` 恒 0 → 印面永远是平的 → 母规格 R3「靠深度遮挡凸显五官」**完全失效**；
- 不报错、不崩溃；画面"看着有个印面"，实际没有浮雕 —— 比直接崩更难查；
- 全仓此前 `max_relief` 零命中，该 shader 也从未被任何 pipeline 引用过（从没渲染过一像素）。

修法：把 `_pad[3]` 拆成 `max_relief@100` + `_pad[2]@104..111`，**stride 仍 112B**，
顶点格式与管线布局零改动。`batch.h` 加 6 条 `static_assert` 锁偏移，
`l3_cli/main.cpp` 的 `scene_pipeline_desc()` 加 `static_assert(offsetof(max_relief) == 100)` 双保险。

> 这条记在本规格里，是因为它是 F-ST 的**第一块地基**：
> 没有它，后面所有印面纹理都渲染成一块平板。

---

## §4 层归属与接口边界

```
L4  features/stamp/     ← 本轮新增（唯一允许出现"印章"字样的地方）
L3  primitives/         ← 只调用，不改
L2  scene/
L1  resources/          ← P0 通道已就位
L0  device/
    backend/vulkan/     ← 只有这层碰 Vk*
```

**硬规则**（`tools/check_l3_no_gameplay_names.py` 机械校验）：
- "印章"/"stamp" 只允许出现在 `include/magicblocks/features/**` 与 `src/features/**`；
- L0–L3 不得引用 `features/` 任何头文件；
- F-ST 若需要 L3 没有的能力 → **回去补 L3**，不在 features 里开洞。

---

## §5 数据模型：文档是真源，网格是派生缓存

沿用 F-SK 的分界（`sketch_document.h` 顶部 §"为什么存点不存网格"），理由相同且更强：

| 存文档（本轮做法） | 存网格（反例） |
|---|---|
| 改颜色/深度/尺寸只需改参数 | 改一个参数要重录整块印章 |
| 撤销重做与浮点抖动解耦 | 重三角化一点抖动就算不同状态 |
| 文档 KB 级 | 网格 MB 级 |

### §5.1 文档结构

```cpp
namespace magicblocks::features::stamp {

// 印面纹理：RGBA8，R=颜色，G=深度，B/A 保留（=0）
struct StampTexture {
  std::uint32_t width = 0;    // 必须 > 0
  std::uint32_t height = 0;   // 必须 > 0
  // 长度必须 == width*height*4，行序自上而下逐像素 R,G,B,A
  std::vector<std::uint8_t> rgba8;
  std::uint64_t hash() const;   // FNV-1a 64，逐字节
};

// 底板轮廓来源：模板 或 自定义轮廓（母规格 R1 "FromOutline / 模板"）
enum class OutlineSource : std::uint8_t {
  Template = 0,   // 用内置模板（圆/方/花瓣/星）
  Custom = 1,     // 用用户给的闭合轮廓
};

enum class TemplateKind : std::uint8_t {
  Circle = 0, Square = 1, Petal = 2, Star = 3,
};

struct StampSpec {
  // ---- 底板 ----
  OutlineSource outline_source = OutlineSource::Template;
  TemplateKind template_kind = TemplateKind::Circle;
  float board_radius = 0.5f;        // 模板半径；Custom 时为轮廓包围盒归一化尺度
  std::vector<Vec2> custom_outline; // 闭合环，不重复首点；仅 Custom 有效
  float board_thickness = 0.08f;    // 底板挤出厚度

  // ---- 印面（母规格 R2/R3）----
  StampTexture face;                // 必填，宽高 > 0
  float max_relief = 0.35f;         // 浮雕最大高度（世界单位）
  Color4 face_color{0.85f, 0.12f, 0.12f, 1.0f};  // 印油色，与贴图 R 相乘

  // ---- 把手（母规格 R4）----
  bool has_handle = true;
  float handle_radius_bottom = 0.10f;
  float handle_radius_top = 0.13f;
  float handle_height = 0.45f;
  float handle_metallic = 0.9f;     // L3 spec §12 硬指定
  float handle_roughness = 0.2f;    // L3 spec §12 硬指定
};
```

### §5.2 文档

```cpp
class StampDocument {
 public:
  // 一枚印章 = 一份 spec（本轮不做"文档含多枚印章"）
  const StampSpec& spec() const;
  // 改任何字段都走 set_spec，内部压撤销栈并推进 revision
  void set_spec(StampSpec spec);
  std::uint64_t revision() const;
  std::uint64_t hash() const;       // spec 全字段 FNV-1a 64（含 face.rgba8 逐字节）

  bool can_undo() const; bool can_redo() const;
  bool undo(); bool redo();
  // 反复 set_spec 同一份 spec 不产生多余撤销项（相同时直接返回）
  // 撤销栈上限 64（与 F-SK 一致）
};
```

**不做**：多印章文档、图层、印章组合。母规格 F-ST 行只描述"一枚印章长什么样"，
落印是场景层的事（§8）。

---

## §6 规格 → 网格（纯 CPU，无设备可单测）

```cpp
struct StampMesh {
  primitives::MeshData board;   // 底板 + 印面浮雕承载面
  primitives::MeshData handle;  // 把手；has_handle=false 时为空
  color_       Color4 board_color;
  ...
};

struct StampError {
  enum class Code : std::uint8_t {
    EmptyTexture = 1,      // face 宽/高为 0 或 rgba8 长度不符
    DegenerateOutline = 2, // 模板/Custom 轮廓退化（点<3、面积≈0）
    ExtrudeFailed = 3,     // from_outline 返回空
    HandleParamsInvalid = 4, // 半径/高度 <= 0
  };
  Code code;
  std::string message;     // 人读信息，用于 CLI/日志
};

struct StampMeshSet {
  std::vector<StampMesh> meshes;   // 正常恒为 1 个元素
  std::vector<StampError> errors;
  std::uint64_t mesh_hash = 0;     // 网格顶点+索引指纹，与 doc hash 互为"两只眼"
  bool ok() const { return errors.empty(); }
};

// 预检：不挤出，只判断这份 spec 会不会失败（CLI --dry-run 用）
StampMeshSet build_outlines(const StampSpec& spec);
// 真网格
StampMeshSet build_meshes(const StampSpec& spec);
```

### §6.1 底板网格

```
1) 取轮廓环（模板生成 或 custom_outline）
2) from_outline(rings, {thickness=board_thickness, cap_bottom=true, cap_top=true, z_base=0, color})
3) UV 归一化：from_outline 给的是**世界坐标原样 (x,y)**（mesh_factory.cpp:935-936），
   不是 [0,1]。这里按 bounds 线性映射到 [0,1]：
     u = (x - bounds.min.x) / bounds.extent.x
     v = (y - bounds.min.y) / bounds.extent.y
   贴图采样依赖这一步 —— 少了它，贴图按世界单位平铺，印章一出图就是满屏斑点。
   extents 为 0 → DegenerateOutline
```

### §6.2 把手网格

```
has_handle ? trapezoid(handle_radius_bottom, handle_radius_top, handle_height, segments=32)
           : 空
放置：底面贴底板顶面 (z = board_thickness)，即整体沿 +Z 平移 board_thickness
```

`trapezoid` 是 L3 为"圆台把手"专门加的（`mesh_factory.h:143`），**本轮直接用，不改 L3**。

### §6.3 错误处理（失败必响）

- 任一 `Code` 命中 → 进 `errors`，且 `meshes` 字段**不返回半成品**；
- CLI 遇到非空 `errors` → 打印每条 `message` 并**退出码非 0**；
- 绝不"跳过坏印章继续渲染好的那些"（那会让用户以为落印成功了）。

---

## §7 渲染（母规格 R2/R3/R4）

### §7.1 管线

```cpp
// 印面管线：唯一与 l3_scene 的差别是多了 1 个纹理槽
res::PipelineDesc stamp_face_pipeline_desc(int msaa) {
  res::PipelineDesc d = scene_pipeline_desc(msaa);  // 复用顶点/实例布局、深度、剔除
  d.sampled_texture_slots = 1;                      // ← P0 通道
  d.stages = { kL3_scene_texturedVert, kL3_scene_texturedFrag };
  return d;                                         // descriptor_set_count() == 1
}
```

> `scene_pipeline_desc(msaa)` 仍然 `sampled_texture_slots = 0` ⇒ `descriptor_set_count() == 0`。
> 既有所有管线与 5 张 golden 逐字节不变。

> `sampled_texture_slots = 1` ⇒ `descriptor_set_count() == 1`（`pipeline.h:231-237`）。
> 这是 P0-1 的验收口径；既有所有管线仍为 0，5 张 golden 逐字节不变。

### §7.2 每帧录制

```
begin_pass(target with_depth=true, clear)
set_viewport / set_scissor
bind_vertex_buffer(instances, slot 1)          ← 两条管线布局相同，只绑一次

# ---- 底板：贴图管线 ----
bind_pipeline(face_pipeline)
push_constants(view_proj, normal_matrix)
bind_sampled_texture(face_texture, linear_sampler, slot 0)   ← 第 12 个命令
bind_vertex_buffer(board_vb, 0, 0) → bind_index_buffer(board_ib) → draw_indexed

# ---- 把手：无纹理管线（§7.3）----
if (has_handle) {
  bind_pipeline(scene_pipeline)                ← 切管线
  push_constants(view_proj, normal_matrix)     ← 必须重发（不共享状态）
  bind_vertex_buffer(handle_vb, 0, 0) → bind_index_buffer(handle_ib) → draw_indexed
}
end_pass → submit_command_list → readback_rgba8 → write_png_rgba8
```

**实例属性逐项**（`InstanceAttribs`，112B）：

| 字段 | 底板 | 把手 |
|---|---|---|
| `model` | 单位阵 | 单位阵（网格已在局部空间摆好） |
| `base_color` | `face_color` | 金属灰 `{0.72,0.74,0.78,1}` |
| `roughness` | 0.55 | `handle_roughness` = 0.2 |
| `metallic` | 0.0 | `handle_metallic` = 0.9 |
| `max_relief` | `spec.max_relief` | **0**（把手不浮雕） |

> 底板与把手走**两条不同管线**（§7.3），实例缓冲仍是同一个（布局相同、slot 1 只绑一次）。

### §7.3 把手走独立无纹理管线（用户裁定：方案 B）

两条管线：

| 对象 | 管线 | `sampled_texture_slots` | 实例 |
|---|---|---|---|
| 底板（印面） | `stamp_face_pipeline_desc()`，`l3_scene_textured` VS/FS | 1 | 1 个：`max_relief = spec.max_relief` |
| 把手 | `scene_pipeline_desc(msaa)`（**复用现有 L3 管线**） | 0 | 1 个：`max_relief = 0` |

**为什么**：底板与把手共用贴图管线时，`l3_scene_textured.frag:49` 的
`vec3 base = vColor.rgb * vec3(stampR)` 会把印面贴图的 R 乘到金属把手上 ——
把手会沾上印油色。要在共用管线下消掉，得在 CPU 侧预除把手 UV 处的贴图平均 R，
逻辑绕且对 UV 语义有隐含依赖（`trapezoid` 的 UV 语义并未文档化）。

独立管线只多一次 `bind_pipeline`，换来两个收益：
- 把手颜色 = `base_color` 精确值，不需要任何补偿；
- 把手网格完全不需要 UV，F-ST 不必关心 `trapezoid` 的 UV 约定。

**代价与处理**：同一次 `begin_pass` 内切管线是合法的（`ICommandList` 允许），
但**必须重新 `push_constants`** —— 布局虽然相同（96B），不同管线不共享 push constant 状态。
录制顺序固定为：`bind_pipeline(face) → push_constants → bind_texture → draw(board)`，
再 `bind_pipeline(scene) → push_constants → draw(handle)`（`bind_vertex_buffer(instances, 1)` 不用重绑，
两个管线用同一套顶点/实例布局）。

### §7.4 相机

母规格 §12 行："俯视正交/透视"。F-ST 默认**斜俯视透视**（`fov_y=45°`，
position≈`(0.9, -1.15, 1.35)`，target 在印面中心），因为：
- 正交俯视看不到把手，"印章"辨识度为零；
- 透视 + 斜角能同时看到印面浮雕与把手侧面 —— 母规格 R3 要的正是这个视角。

正交模式作为 `--ortho` 选项保留（用于验证 UUID 归一化与浮雕观感）。

---

## §8 落印（母规格 R5：平面/三角形拾取）

落印不在本轮渲染路径里，但拾取入口必须存在且有测试：

```cpp
// 屏幕点 → 印面平面上的落印点
struct StampDrop {
  bool hit = false;
  Vec3 world_point{};     // 落印中心（世界坐标）
  Vec2 face_uv{};         // 落在印面纹理的哪个 uv（供后续"贴到画布"用）
};

StampDrop drop_on_plane(const IPicker& picker, const ICamera& camera, float px, float py,
                        const Plane& target_plane) const;
```

只用 `IPicker::intersect_plane`（L3 已有），不新增 L3 能力。
"印到画布上留下痕迹"属于画布/场景层，**本轮不做**（母规格 F-ST 行未要求）。

---

## §9 CLI（离屏一等公民）

```
magicblocks-l3-cli stamp --width 800 --height 600 --output stamp.png
                         [--template circle|square|petal|star] [--radius 0.5]
                         [--relief 0.35] [--face-color R,G,B]
                         [--no-handle] [--handle-height 0.45]
                         [--ortho] [--msaa N] [--dry-run]
                         [--texture-pattern 0|1|2]   # 0=同心圆 1=人脸 2=文字格
```

`--texture-pattern` 用**确定性生成器**造贴图（不读随机/时钟、不读外部文件），三种图案各自
考不同的东西：同心圆考径向频率、人脸考局部高频（五官）、文字格考直角边。

> 用户裁定：**只做内置三种确定性图案**，不引入 PNG 解码入口。
> 理由：验收要逐字节可复现，外部图片会引入解码器不确定性与新依赖。

**验收**：
- 同参数跑两次，PNG **逐字节相同**（`tools/check_png_deterministic.py`）；
- `--dry-run` 不建设备、不出图，只报 spec 校验结果与预计顶点/三角数；
- 任何 `StampError` → 退出码非 0 且打印 `message`。

---

## §10 测试计划

| 组 | 用例（至少） | 断言 |
|---|---|---|
| 文档 | `StampDocument.DefaultSpec` | 默认 spec 合法，hash 稳定 |
| 文档 | `StampDocument.SetSpecUndoRedo` | 撤销/重做回到逐字段相同状态 |
| 文档 | `StampDocument.NoOpSetNoUndo` | 设同一份 spec 不增加撤销项 |
| 文档 | `StampDocument.HashCoversTexture` | 只改 face 一个像素，document hash 必变 |
| 网格 | `StampMesh.BoardWatertight` | 有上下盖 ⇒ 每条无向边恰好 2 个三角形 |
| 网格 | `StampMesh.UvNormalized` | 所有顶点 uv ∈ [0,1]（容差 1e-5） |
| 网格 | `StampMesh.HandlePlacedOnTop` | 把手 z 最小值 == board_thickness |
| 网格 | `StampMesh.AllTemplatesBuild` | circle/square/petal/star 都出非空网格 |
| 错误 | `StampError.EmptyTexture/EdegenetateOutline/...` | 各 Code 精确命中，`ok()==false` |
| 确定性 | `StampMesh.MeshHashStable` | 同 spec 两次 build_meshes，mesh_hash 相同 |
| 管线 | `StampPipeline.OneSampledSlot` | `descriptor_set_count()==1`、`sampled_texture_slots()==1` |
| 管线 | `StampPipeline.SceneUnchanged` | `scene_pipeline_desc()` 仍为 0 个描述符集 |
| 端到端 | `StampEndToEnd.RendersPng` | 出图非全背景色，含印油色像素 |
| 端到端 | `StampEndToEnd.ReliefChangesPixels` | 只改 face.G（深度）→ 输出像素**必变**（证明 max_relief 真的通了） |
| 端到端 | `StampEndToEnd.Deterministic` | 同参数两次出图逐字节相同 |
| 端到端 | `StampEndToEnd.CliRejectsBadArgs` | 非法参数退出码非 0 |

> `ReliefChangesPixels` 是**本次修复的机械证明**：
> 修复前 G 通道怎么改都不影响输出（因为 .y 恒 0）。

---

## §11 硬约束自检

| 约束 | 本规格如何满足 |
|---|---|
| 无几何着色器 | 只用 VS/FS；`check_no_geometry_shader` 门禁 |
| 无裸分配 | 全部 `unique_ptr` / `std::vector`；`check_no_raw_alloc` 门禁 |
| 分层单向 | features → primitives/resources/device；`check_layer_purity` 门禁 |
| 玩法词不进 L0–L3 | `check_l3_no_gameplay_names` 门禁 |
| 命令模式 | 只录 `ICommandList`，`submit_command_list` 下发；不直接碰 vkCmd* |
| 确定性 | 无时间/随机/指针序/线程数依赖；两次出图逐字节相同 |
| 失败必响 | `StampError` 全链路显式返回，CLI 退出码非 0，无降级分支 |
| 离屏优先 | CLI 全走 offscreen + `readback_rgba8` + `write_png_rgba8` |
| 不越层开洞 | 只用 L3 已有 API；缺能力回去补 L3（P0 通道即为此补的） |

---

## §12 风险与开放问题

| # | 风险/问题 | 处置 |
|---|---|---|
| RISK-1 | 顶点位移的浮雕分辨率受网格细分限制：底板只有轮廓边界，内部无顶点 → **浮雕细节会被插值抹平** | ✅ 已裁定：补 L3 细分（选项 A），见 §12.1 |
| RISK-2 | `trapezoid` 把手 UV 语义未文档化 | ✅ 已消解：把手走独立无纹理管线（§7.3），不需要 UV |
| RISK-3 | 把手被贴图 R 染色 | ✅ 已裁定：方案 B 独立管线（§7.3） |
| RISK-4 | 共享工作区并行构建污染测试结果 | 一律在独立 worktree + `-j 1` |
| RISK-5 | 细分后底板顶点数暴涨（印面 0.5 半径、目标边长 0.005 ⇒ 约 3 万顶点），挤出后翻倍 | 见 §12.1 的预算与上限校验 |
| RISK-6 | 切管线后忘记重发 push constant → 把手位置错乱 | `StampEndToEnd.HandleUsesOwnPushConstants` 用对角位置断言 |

### §12.1 RISK-1：浮雕分辨率（已裁定：选项 A）

当前底板来自 `from_outline`，**只有轮廓边界上有顶点，印面内部是一个大三角扇**。
顶点位移只能在顶点处发生 → 印面中心的五官**根本无法凸起**，只会被插值成一块缓坡。

证据：
- `mesh_factory.cpp:902` `tessTesselate(tess, TESS_WINDING_NONZERO, TESS_POLYGONS, 3, 2, nullptr)`
  —— 最后参数 `normal` 传 `nullptr`，没有细分选项；
- 顶点数就是 `tessGetVertexCount`（`:911`），即输入轮廓的点数与少量 Steiner 点；
- `l3_scene_textured.vert:60` 的 `relief` 只影响**已有顶点**。

**裁定（用户）**：选项 A —— 补 L3 细分能力。

L3 需新增（详见 `2026-11-10-magicblocks-l3-outline-subdivision-request.md`）：

```cpp
// 在挤出前把轮廓内部按目标边长细化，得到足够密的盖面网格。
MeshData from_outline_subdivided(const OutlineSet& outline, const ExtrudeParams& params,
                                 float target_edge_len);
```

**预算**（`target_edge_len = 0.005`、半径 0.5 的圆）：
面积 π·0.25 ≈ 0.785，每三角形 ≈ √3/4·(0.005)² ≈ 1.08e-5 ⇒ 约 7.3 万三角形 / 3.7 万顶点；
挤出后（上下盖各一份顶点 + 侧壁）约 7.5 万顶点 / 15 万索引 ≈ 4.9 MB。
可接受，但**F-ST 必须校验上限**：`target_edge_len` 使预计三角形数超过 20 万 → 返回
`StampError::Code::SubdivisionTooDense` 并拒绝，而不是默默吃满内存。

---

## §13 交付物清单（本轮）

见 §17（裁定后更新）。

---

## §14 与母规格的对应

| 母规格条目 | 本规格章节 |
|---|---|
| §276 F-ST 行（底板/印面双贴图/把手金属） | §1、§6、§7 |
| P4 picker「印章落印」 | §8 |
| P8 material（std_pbr metallic .9 rough .2） | §7.1、§7.2 |
| C4 无几何着色器 | §11 |
| C5 命令模式 | §7.2 |
| C6 确定性 | §10、§11 |
| C7 失败必响 | §6.3 |
| L3 spec §12 F-ST 行 | §1 |

---

## §15 审阅要点

（v1 的四个问题已由用户裁定，见 §16。本节保留为空，留给下一轮审阅新增问题。）

---

## §16 裁定记录（2026-11-10）

| # | 问题 | 裁定 | 影响章节 |
|---|---|---|---|
| 1 | RISK-1 浮雕分辨率 | **A. 补 L3 细分能力** | §6.1、§12.1、§13、§17 |
| 2 | 把手渲染方式 | **B. 单独无纹理管线** | §7.1、§7.2、§7.3 |
| 3 | 贴图来源 | **内置三种确定性图案** | §9 |
| 4 | 文档是否只含单枚印章 | v1 §5.2 方案保留（单枚），未提异议 | §5.2 |

### §16.1 裁定 1 带来的依赖

F-ST 的 §6.1 改为调用 `from_outline_subdivided`，它属于 L3，**当前不存在**。
因此本轮实现分两段：

| 段 | 内容 | 前置 |
|---|---|---|
| ① L3 细分能力 | `from_outline_subdivided` + 单测 | 无 |
| ② F-ST 本体的实现 | §5–§11 全部 | ① 落地并通过全量回归 |

需求书：`docs/superpowers/specs/2026-11-10-magicblocks-l3-outline-subdivision-request.md`

---

## §17 更新后的交付物清单

| 阶段 | 文件 | 说明 |
|---|---|---|
| ① | `MagicBlocks/include/magicblocks/primitives/mesh_factory.h` | 加 `from_outline_subdivided` 声明 |
| ① | `MagicBlocks/src/primitives/mesh_factory.cpp` | 实现（边界保留约束 Delaunay 细分） |
| ① | `MagicBlocks/tests/magicblocks_l3_outline_test.cpp` | 细分用例 |
| ② | `MagicBlocks/include/magicblocks/features/stamp/stamp_document.h` | 文档 + spec + 错误码 |
| ② | `MagicBlocks/include/magicblocks/features/stamp/stamp_pipeline.h` | build_outlines / build_meshes |
| ② | `MagicBlocks/src/features/stamp/stamp_document.cpp` | 撤销重做 / hash / 校验 |
| ② | `MagicBlocks/src/features/stamp/stamp_pipeline.cpp` | 模板 → 轮廓 → 细分挤出 → UV 归一化 |
| ② | `MagicBlocks/src/l3_cli/main.cpp`（+stamp 子命令） | CLI 出图 |
| ② | `MagicBlocks/tests/magicblocks_l4_stamp_test.cpp` | §10 全部用例 |
| ② | `MagicBlocks/tests/data/stamp_golden.png` | golden（逐字节比对） |
| ② | `MagicBlocks/CMakeLists.txt` | 源文件 + 测试文件 + `check_deps_tests.py --expect-files` 计数 |
