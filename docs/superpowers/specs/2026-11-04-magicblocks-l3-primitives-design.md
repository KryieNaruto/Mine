# 魔法功能预览 —— L3 primitives 设计（抽象 + 性能 + ECS/材质/Gizmo/命令流）

日期：2026-11-04
状态：设计定稿（第 1 版，承接 L0~L2 已落地）
关联：
- 总设计：`docs/superpowers/specs/2026-11-03-magicblocks-render-engine-sdk-design.md`（下称"总 spec"）
- 计算库选型：`docs/superpowers/specs/2026-11-03-magicblocks-compute-deps-selection.md`
- L2 命令模式：`docs/superpowers/plans/2026-11-03-magicblocks-l2.md`
- 架构索引：`MagicBlocks/docs/ARCHITECTURE.md`

---

## 0. 本轮用户指令（逐条，作为验收口径）

用户原话要点（本设计逐条落实并编号，后续每节都标注覆盖哪条）：

| # | 用户要求 | 本设计落地章节 |
|---|---|---|
| U0 | **先抽象，再区分模型用途** | §2 抽象总则 + §3 分层；先定"能力原语"，再定"用途"映射 |
| U0' | **性能是第一考虑对象** | §4 性能模型（热路径零分配、SOA、实例化、批合并、缓存） |
| U1 | 完成**基础摄像机类** | §5 `ICamera` |
| U2 | 先完成**简易画笔事件 → "画"** | §6 `IPainter`（输入事件 → 笔画 → 渲染） |
| U3 | **基础模型类**用 **ECS** 思想：功能作组件拼装，可叠加 | §7 `IWorld` ECS（Archetype 列式存储） |
| U4 | 构建**对象池**复用对象 | §8 `ILinearAllocator`/`IObjectPool`/`IBatchPool` |
| U5 | **Material 抽象好**，处理好 **Shader ↔ Material 联动** | §9 `IShaderLibrary`/`IMaterial`/`IBindingSet` |
| U6 | **Gizmo 事件足够解耦**，不能到处打补丁 | §10 `IGizmoSystem`（事件总线 + 声明式轴 + 状态机） |
| U7 | **必须考虑命令录制回放** | §11 命令流集成（所有可变状态进流） |
| U8 | 按难易程度区分先后 | §13 落地顺序与里程碑 |

---

## 1. 现状基线（L0~L2 已落地的事实，L3 必须兼容）

| 层 | 事实（约束 L3 的硬边界） |
|---|---|
| L0 | `IRenderDevice` 是唯一设备入口；离屏目标与窗口目标同构；`InputEvent{kind,pointer_id,position,delta,button,timestamp_us}` 是**唯一事件源**（D6）；不读时钟 |
| L1 | 资源**一律**经 `IRenderDevice::create_*` 创建；分配器**先 `reserve_pages` 再 `allocate`**，否则 `NoSpace`；稳态帧 VMA alloc/free 必须 0（M5）；`ICommandList` **恰好 11 个方法**；`push_constant_bytes` 是唯一参数通道（R4，无描述符集） |
| L2 | `ICommandList::draw*` **只记录不下发**（U1 裁定）→ `CommandStream.commands.size() == target->command_count()` 是强不变量（W4）；`IRecorder` 双写（先 L1 后流）；`IScene`/`ISubmitQueue`/`IRenderGraph`/`IRenderFeatureRegistry` 是值语义快照 |
| 门控 | G1 无 GS；G3 后端外零 `Vk*`、backend 内零玩法名、`src/features/**` 不许写 GPU 命令；G4 无裸分配；`check_scene_no_clock` 禁 `include/magicblocks/scene/**` 与 `src/scene/**` 读时钟 |

**由此推出的三条 L3 硬约束**：

- **C-L3-a（录制等式）**：L3 的每一次绘制提交必须**经由** `IRecorder` 对应的 11 类命令，
  且 **L3 渲染器的命令序列必须是 CPU 状态的纯函数**（同场景 + 同相机 → 同命令流）。
  → 一切影响像素的状态（材质、变换、可见性、gizmo 高亮、笔画进度）必须可从 `IScene` +
  `CommandStream` 重建，**不允许**藏在 `ICommandList` 之外的黑盒里。
- **C-L3-b（零分配热路径）**：稳态帧不得 `new`/`malloc`/VMA 调用。→ §4.1、§8。
- **C-L3-c（层归属）**：`IPicker` 需要读回 → 必须经 `IRenderTarget::readback_rgba8`，
  而 **G3 禁止 backend 外出现 `Vk*`**（`readback_rgba8` 签名是无关类型的，OK），
  但 **`src/features/**` 不许写 GPU 命令**，所以 picker 本身放 L3，玩法只调它。

---

## 2. 抽象总则（U0：先抽象，再区分用途）

### 2.1 抽象的顺序（为什么必须先抽象）

四个玩法（涂鸦 / 积木 / 叠叠乐 / 印章）如果先按玩法写，就会出现"给积木加 gizmo 特例、
给叠叠乐加内面特例"的补丁式代码。总 spec §2.2 已经证明每个需求都有通用接口；
L3 要做的是**把这套通用接口的"最小不可再分能力"先定死**，让玩法只能组合，不能新增分支。

所以顺序是：

```
第 1 步 定"能力"（什么能做）   →  ICamera / IMesh / IShaderLibrary / IMaterial / IWorld / IPainter / IGizmoSystem / IPicker
第 2 步 定"拼接"（谁能拼谁）   →  IWorld 的组件注册表 + Component 描述符（ECS）
第 3 步 定"复用"（怎么不销毁） →  ILinearAllocator / IObjectPool / FrameArena
第 4 步 定"用途"（谁用哪几样） →  §12 用途矩阵：每个玩法 = 若干原语的组合，**零新接口**
```

第 4 步刻意**只产出表格**，不产出接口：一旦某个玩法需要一个新的公开接口，就说明第 1 步
抽象不够通用，应回头改第 1 步（而不是在 L4 加特例）。

### 2.2 抽象粒度判据（三选一，不满足就不抽）

| 判据 | 含义 | 例 |
|---|---|---|
| A1 有 ≥ 2 个玩法用 | 至少两处复用 | `ICamera`（四个都用） |
| A2 有性能杠杆 | 抽象本身带来批合并/复用/缓存 | `IMaterial`（相同材质 → 一次 drawcall） |
| A3 是录制边界 | 状态必须进命令流才能回放 | `IWorld`（组件值 → `IScene` 快照） |

不满足三条的（例如"印章底板圆角"）留在 L4 用参数表达。

### 2.3 命名与命名空间

- 命名空间 `magicblocks::primitives`（L3），头目录 `include/magicblocks/primitives/`。
- 类型只用 `device::u32/u64/i32` 别名或全名 `std::uint*_t`（与 L2 同口径）。
- **L3 公共头零 `Vk*`**；L3 不 `#include` backend。
- 数学用 `glm`（池内已有，header-only），但**只在 L3 内部与公共头的值类型里**用，
  公共接口签名尽量用 POD（便于命令流与序列化）。

---

## 3. 分层与文件布局

```
include/magicblocks/primitives/      ← L3 公共面（零 Vk*，L4 只 include 这里）
  types.h          vec/mat 值类型、Box/Plane/Ray、颜色、句柄
  camera.h         ICamera / CameraDesc / ProjectionKind / OrbitControls
  mesh.h           IMesh / MeshData / VertexFormat（L3 自己的顶点布局）
  mesh_factory.h   IMeshFactory（Cube/Box/Cylinder/Cone/Sphere/Prism/Trapezoid/Quad）+ FromOutline
  mesh_loader.h    IMeshLoader::obj()
  shader_library.h IShaderLibrary / ShaderKey / ShaderModuleRef / UniformFieldDesc
  material.h       IMaterial / MaterialDesc / MaterialKey / MaterialTable / IBindingSet / StandardParams
  world.h          IWorld(ECS) / EntityId / ComponentKind / ComponentDesc / View<T>
  painter.h        IPainter / StrokeDesc / StrokeEvent / PainterState
  gizmo.h          IGizmoSystem / GizmoTarget / GizmoAxis / GizmoEvent / GizmoListener
  picker.h         IPicker / PickDesc / PickResult / Intersector
  overlay2d.h      IOverlay2D / LineDesc / DashDesc / RectDesc
  renderer.h       IPrimitiveRenderer（L3 渲染器：把 World+Camera+Material 变成 11 类命令）
  alloc.h          ILinearAllocator / IObjectPool<T> / IFrameArena / IBatchPool
  primitive_factory.h  create_* 工厂（唯一入口）

src/primitives/                      ← L3 实现（零 Vk*；允许 include glm）
  camera.cpp  mesh.cpp  mesh_factory.cpp  mesh_loader.cpp
  shader_library.cpp  material.cpp  world.cpp  world_storage.h
  painter.cpp  gizmo.cpp  picker.cpp  overlay2d.cpp
  renderer.cpp  renderer_batch.h  alloc.cpp  primitive_factory.cpp

shaders/primitives/                  ← 新增 GLSL（**仍只有 vert/frag**，G1）
  std_pbr.vert/.frag      实例化 PBR-ish（半球光 + metallic/roughness）
  std_unlit.vert/.frag    纯色/顶点色（积木、UI）
  stroke_ribbon.vert/.frag 屏幕空间条带（画笔）
  line2d.vert/.frag       屏幕恒宽线（gizmo / 虚线框 / 网格）
  pickid.vert/.frag       拾取（输出实体 id 到 RGBA8）
  screen_quad.vert/.frag  全屏合成/清屏
```

`src/primitives/**` 在 G3 的 "backend 外" 分支，所以**不得出现任何 `Vk*`**；
也不在 `src/features/**`，所以不受"不许写 GPU 命令"限制 —— 但 L3 写命令**只能经由**
`IRecorder` 的 11 类方法（§11），这是 L3 的自我约束 + 新门控 G6（§14）。

---

## 4. 性能模型（U0'：性能第一）

### 4.1 热路径预算（稳态帧，设计目标）

| 资源 | 预算 | 手段 |
|---|---|---|
| 堆分配次数 | **0** | FrameArena（每帧线性分配，帧末整体回退，§8.2） |
| VMA alloc/free | **0**（继承 M5） | 几何/材质变更才走分配；稳态帧无变更 |
| drawcall 数 | ≤ **300**（总 spec §8） | 按 (ShaderKey, MaterialKey, MeshId) 合批（§4.3） |
| 管线切换 | ≤ 16 次/帧 | 排序键把 pipeline 放最前 |
| 顶点上传 | 仅脏块 | 实例属性缓冲按 archetype 分块，脏 chunk 上传 |

### 4.2 ECS 用 SOA 列式存储（U3 的前提）

实体 = `EntityId{u32 index, u32 gen}`。组件按 **Archetype** 分组：一组组件集合
（如 `{Transform, MeshRef, MaterialRef}`）对应一个 archetype，archetype 内的每个组件是
**一条紧凑的列数组**（`std::vector<Transform>`）。好处：

- 遍历 = 顺序访存（cache 友好），这是 L3 最大的性能杠杆；
- 排序/合批 = 对 archetype 内的列做索引排序（`std::sort` 索引数组 + 间接取），
  **不搬动大对象**；
- `View<C...>` 按 archetype 过滤后**只遍历包含所需组件列的 archetype**，不查哈希表。

### 4.3 合批（决定 drawcall 数）

```
排序键（升序，稳定 tie-break = EntityId）：
  [0] pipeline_hash      （ShaderKey + 固定功能状态）
  [1] material_key       （MaterialKey 的 64 位哈希）
  [2] mesh_id            （几何缓冲偏移）
  [3] transparency       （0 不透明 / 1 透明；透明内部按距离**降序**）
  [4] entity_id
```
相邻项具有相同 `(pipeline_hash, material_key, mesh_id)` 且**实例容量未满** → 合并成一次
`draw_instanced`。实例数据写进 §8.3 的 `IBatchPool` 环形实例缓冲（帧内只增不改）。

> **为什么不是 `ISubmitQueue`**：L2 的 `DrawItem` 字段（`mesh_key/first_index/index_count`）
> 不足以表达实例数据；且 L2 排序键是 `(material, depth)`，没有 pipeline 与 mesh 键。
> **决定**：L3 自带合批器 `IBatchBuilder`（`src/primitives/renderer_batch.h`），
> 最终仍以 `DrawItem` 形式喂 `ISubmitQueue` 只为**测试断言顺序**；真正的 draw 参数由
> 合批器产出。这条选择写进 §12 决策表（D-L3-3），避免后人以为"绕过了 L2"。

### 4.4 缓存（避免重复计算）

| 缓存 | 键 | 失效条件 |
|---|---|---|
| 管线缓存 | `IPipeline::hash()`（L1 已有） | 永久 |
| 材质缓存 | `MaterialKey`（desc 规范化后的 64 位哈希） | desc 变更 |
| 网格缓存 | `MeshKey`（图元参数 + 轮廓轮廓） | 参数变更 |
| 世界矩阵缓存 | `EntityId` + Transform 版本号 | Transform 脏 |
| 包围盒/球 | 同上 | bounds 脏 |

### 4.5 不做的事（防止过度设计）

- 不做 SIMD 显式优化（先量测再优化；MSVC `/O2` 的自动向量化通常够）；
- 不做 Job 系统 / 多线程（G5 单队列单线程至今成立）；
- 不做通用 reflection（shader 反射只做"你要的 uniform 名字 → offset/size"的静态表，§9.2）。

---

## 5. `ICamera`（U1：基础摄像机）

### 5.1 接口

```cpp
namespace magicblocks::primitives {

enum class ProjectionKind : std::uint8_t { Perspective = 0, Orthographic = 1 };

struct CameraDesc {
  ProjectionKind kind = ProjectionKind::Perspective;
  float fov_y_degrees   = 45.0f;   // 仅透视
  float ortho_height    = 2.0f;    // 仅正交（世界单位的高度）
  float near_plane      = 0.1f;
  float far_plane       = 100.0f;
  bool  reverse_z       = false;   // 需要 DeviceCaps.rev_z_supported，否则 create 报 Unsupported
  Extent2D viewport{1, 1};         // 像素；用于矩阵与屏幕↔世界
  // 以下只用于"屏幕恒定尺寸"（gizmo/覆盖层）
  bool  y_down_ndc      = true;    // 与离屏读回行序一致（自左上）
};

struct CameraPose {                 // 值语义，可直接进命令流
  float position[3] = {0, 0, 5};
  float target[3]   = {0, 0, 0};
  float up[3]       = {0, 1, 0};
};

class ICamera {
 public:
  virtual ~ICamera() = default;

  virtual const CameraDesc& desc() const = 0;
  virtual const CameraPose& pose() const = 0;
  virtual Status set_desc(const CameraDesc&) = 0;          // 校验失败 → Invalid/Unsupported
  virtual Status set_pose(const CameraPose&) = 0;

  // 矩阵（行主或列主一致即可，见 §5.3；本设计用列主，与 glm 一致）
  virtual const float (&view_matrix() const)[16] = 0;      // << 返回类型见注
  virtual const float (&proj_matrix() const)[16] = 0;
  virtual const float (&view_proj_matrix() const)[16] = 0;

  // 屏幕 ↔ 世界（像素坐标，原点左上；与 InputEvent.position 同口径）
  virtual Status screen_to_ray(float px, float py, Ray* out) const = 0;
  virtual Status world_to_screen(const float p[3], float* px, float* py,
                                 float* depth) const = 0;
  // 屏幕常量像素 → 世界尺寸（gizmo 恒定尺寸、屏幕恒宽线的核心原语）
  virtual float world_size_per_pixel(const float world_pos[3]) const = 0;

  // orbit / pan / zoom（值语义，不读时钟，不进命令流 —— 只改 pose）
  virtual void orbit(float delta_yaw_deg, float delta_pitch_deg) = 0;
  virtual void pan(float delta_px_x, float delta_px_y) = 0;
  virtual void zoom(float delta) = 0;                      // 透视：改距离；正交：改 ortho_height
  virtual void focus_on(const float center[3], float radius) = 0;  // frame/fit

  // 相机快照（进 CommandStream 的"初始状态"，§11）
  virtual CameraSnapshot snapshot() const = 0;
  virtual Status restore(const CameraSnapshot&) = 0;
};
```

> **注**：C++ 不能返回指向内部数组的引用时带 `(&...)[16]` 这种"引用到数组"的签名在接口里
> 是合法的，但更简单是返回 **`glm::mat4` 的 POD 镜像**：`struct Mat4 { float m[16]; }`。
> **决定**：公共头用 `struct Mat4 { float v[16]; }`（列主，`v[col*4+row]`），零依赖。

### 5.2 关键决策

| 决策 | 理由 |
|---|---|
| D-L3-1 **矩阵用 POD `Mat4`，不直接暴露 `glm::mat4`** | 公共头零第三方依赖；`Mat4` 可直接写进命令流 payload 与哈希 |
| D-L3-2 **`y_down_ndc`** | 离屏读回自左上（ARCHITECTURE §6），`screen_to_ray` 与拾取必须同口径，否则"点了上边选下边" |
| D-L3-3 **reverse_z 可选** | 需要 caps；不满足 → `Unsupported`（不回退原则 D8） |
| D-L3-4 **`world_size_per_pixel` 独立成方法** | gizmo 屏幕恒定尺寸、屏幕恒宽线的唯一计算点；避免各调用方各写一份（U6 的解耦前提） |

### 5.3 性能

- 矩阵惰性计算 + 版本号脏标记：`set_pose` 只置脏，首次 `view_matrix()` 才重算；
  一帧内多次读取 = 1 次矩阵乘。
- `screen_to_ray` 用**逆 view_proj** 缓存（与 proj 同脏），不每次 `glm::inverse`。

---

## 6. `IPainter`（U2：先做"画"这个简易功能）

### 6.1 目标

最简单可运行的闭环：**收到触摸事件 → 生成笔迹几何 → 渲染出来**，且能录制回放。
不做布尔合并、不做挤出（那些是 L4 的组合）。

### 6.2 接口

```cpp
enum class StrokeEventKind : std::uint8_t { Begin = 0, Move = 1, End = 2, Cancel = 3 };

struct StrokeEvent {           // 由 device::InputEvent 一一映射（D6），时间戳原样带过来
  StrokeEventKind kind = StrokeEventKind::Move;
  std::uint32_t pointer_id = 0;
  float x = 0.f, y = 0.f;      // 像素，原点左上
  std::int64_t timestamp_us = 0;
};

struct StrokeDesc {
  float width_px = 6.0f;
  Color4 color{1, 1, 1, 1};
  bool round_cap = true;
  bool smoothing = true;       // 中心线平滑（Catmull-Rom 细分到 ≤2px 步长）
  int  max_points_per_stroke = 4096;
};
```

### 6.2.1 链路（性能关键）

```
InputEvent ──map──► StrokeEvent ──► IPainter::feed()
                                        │
                     ┌──────────────────┴──────────────────┐
                     │ 只追加"新增条带顶点"（增量，不重建整笔） │
                     │ 写入 §8.3 BatchPool 的环形顶点缓冲      │
                     └──────────────────┬──────────────────┘
                                        ▼
                              本帧 SubmitStrokeDraw()（一次 draw）
```

- 一笔 = 一条**屏幕空间条带**（triangle strip 语义，用 `TriangleList` 展开为 2 个三角形/段，
  仍是 `PrimitiveType::TriangleList` —— 不改 L1 的图元枚举）。
- **增量**：`Move` 只追加一个新段（4~6 个顶点），不重算已有段。这是"画"能不能跑 60fps 的关键。
- **笔画状态**（点列表、当前宽度）是**可序列化值**（§11）：录制只记 `StrokeEvent`，
  回放时重放事件即可重建同样几何（确定性：平滑与细分是纯函数）。

### 6.3 与命令模式的关系（U7）

- **录制**：`IRecorder::record_event()` 记录 `StrokeEvent`（已是 L2 的 `EventRecord`，字段一一对应）。
- **回放**：`IRecorder::replay()` 重录命令；`reproduce()` 重放事件 → 重跑 painter 逻辑 → 重录命令。
- **不变量**：`painter.commands() == recorder 已录命令`（命令流是唯一真源）。

### 6.4 与 L4 的关系

`IPainter` 只做"画在 2D 画布"。**挤出成 3D** 是 L4 的 `FromOutline` 组合。
`IPainter::extract_outline()` 提供轮廓（`std::vector<std::vector<Vec2>>`，含孔），
L4 拿它去 `IMeshFactory::FromOutline()`。这就把"涂鸦"变成"painter + mesh_factory 两步"。

---

## 7. `IWorld`（U3：ECS 组件拼装）

### 7.1 组件 = 功能，按需拼装

```cpp
// 组件用"平凡可拷贝 POD"（便于列式存储与快照）
struct TransformComp { float t[3]; float q[4]; float s[3]; };   // 与 scene::Transform 同序
struct LocalBoundsComp { Box box; };
struct MeshRefComp { MeshId mesh = kInvalidMeshId; };
struct MaterialRefComp { std::uint32_t material = 0; };         // MaterialTable 槽位
struct StrokeComp { std::uint32_t stroke_id = 0; };             // 一个实体 = 一笔
struct LayerComp { std::uint32_t layer = 0; };                  // 叠叠乐锁层
struct VisibleComp { bool visible = true; };
struct GizmoTargetComp { std::uint32_t handle = 0; };            // 一个实体可被 gizmo 操作
```

组件的**注册**必须显式：`IWorld::register_component(kind, sizeof, name)`，注册顺序决定
archetype 位序 —— 这让"新增一个功能 = 注册一个组件 + 一个 System"，与接口无关。

### 7.2 接口

```cpp
class IWorld {
 public:
  // ---- 组件注册（启动期一次）----
  virtual Status register_component(ComponentKind kind, std::uint16_t size_bytes,
                                    const char* name) = 0;

  // ---- 实体 ----
  virtual Result<EntityId> spawn() = 0;
  virtual bool despawn(EntityId) = 0;
  virtual bool alive(EntityId) const = 0;

  // ---- 组件（O(1) 访问；不在同一 archetype 会自动迁移）----
  virtual Status add_component(EntityId, ComponentKind) = 0;
  virtual Status remove_component(EntityId, ComponentKind) = 0;
  virtual bool has_component(EntityId, ComponentKind) const = 0;
  virtual void* component(EntityId, ComponentKind) = 0;   // 类型擦除指针
  template <typename T> T* get(EntityId);                 // 便捷包装（按静态 kind 映射）

  // ---- 遍历（性能核心：只走含所需组件列的 archetype）----
  virtual ComponentView view(std::uint64_t component_mask) = 0;
  // ComponentView: 按 archetype 分块，每块给 (count, const void* const* columns)

  // ---- 脏标记（合批与上传用）----
  virtual void mark_dirty(EntityId, DirtyKind) = 0;
  virtual std::uint64_t version() const = 0;              // 任何变更 +1（命令流哈希用）

  // ---- 与 L2 的桥（U7：快照进命令流）----
  virtual Status sync_to_scene(IScene& scene) const = 0;  // 增量同步 Transform/可见性/包围盒
  virtual WorldSnapshot snapshot() const = 0;             // 按 archetype 序列化
  virtual Status restore(const WorldSnapshot&) = 0;       // 全或无
};
```

### 7.3 与 `IScene` 的关系（重要决策）

`IScene` 是 L2 的**节点树 + 值语义快照**，已经承担了"进命令流"的职责。
`IWorld` 不替换它，而是**在其上做 ECS 视图**：

| 方案 | 评价 |
|---|---|
| A. `IWorld` 内部同时维护 `IScene` 镜像 | 双份状态，易漂移 |
| **B. `IWorld` 只做 ECS 存储；`sync_to_scene()` 把变化推到 `IScene`**（选定） | 单一真源进命令流；ECS 的热路径仍是列式 |

**决定**：ECS 是"运行时高频改动的表示"，`IScene` 是"进入命令流的权威快照"。同步是
**增量**的（只推脏实体），由渲染器在 `begin_frame` 前调用一次。

### 7.4 性能

- 实体删除用 **swap-remove + 返回被移动实体的新位置**（`EntityId` 带 gen，外部句柄不失效）。
- 组件迁移（添加/移除组件）会 move 整行 —— 所以**不要把组件拆得太碎**（迁移是 O(列宽)）。
  设计上一个实体典型 3~5 个组件。
- `view()` 返回的 `ComponentView` 是**迭代器对象**，不分配（内部预分配 archetype 列表）。

---

## 8. 对象池 / 分配器（U4）

### 8.1 三种池，三个用途

| 池 | 生命周期 | 用途 | 实现 |
|---|---|---|---|
| `IFrameArena` | 一帧 | 临时矩阵数组、排序索引、合批键 | 线性 bump，帧末 `reset()`（O(1)） |
| `IObjectPool<T>` | 长于帧、可增长 | 实体行、Stroke、GizmoHandle | 自由链表 + 分块 `std::vector<Chunk>`，**不做单块 realloc**（避免搬移导致指针失效） |
| `IBatchPool` | 一帧（环形） | 实例属性缓冲、条带顶点缓冲 | 每帧一个 slot 环形复用（与 `IFrameSync` 同构），**不分配** |

### 8.2 `IFrameArena`

```cpp
class IFrameArena {
  virtual void* alloc(std::size_t bytes, std::size_t align) = 0;  // 超页 → 新增大块（罕见）
  virtual void reset() = 0;                                       // 帧末
  virtual std::size_t bytes_used() const = 0;
};
```

### 8.3 `IBatchPool`（性能核心）

- 按 **帧槽位**（`in_flight_frames`）各持一块 host-visible 环形缓冲（从 L1 分配器切块，
  启动期 `reserve_pages` 一次）；
- 每帧 `begin_frame(slot)` 把写游标归零，`upload()` 把新增实例数据写入同一 buffer 的偏移；
- **稳态帧 0 次分配**（M5 直接受益）。

### 8.4 复用 = 不销毁

几何/材质/管线都按"键"缓存（§4.4），`destroy` 只是引用计数减一，归零才回收进池。
对象池对外只暴露 `acquire()/release()`，**不暴露 `new/delete`**。

---

## 9. `IMaterial` 与 Shader 联动（U5，最难也最关键）

### 9.1 问题

L1 只有 push constant（128 字节移动端上限），没有描述符集。若"每个材质一个 push constant 块"，
一个 pass 里切换材质就要一次 `push_constants` + 一次 draw，材质数直接等于 drawcall 数 —— 不可接受。

### 9.2 方案：**Shader 模板 + 参数槽 + 实例属性**

```
ShaderKey（管线选择键）          MaterialKey（合批键）
┌──────────────────────┐        ┌───────────────────────┐
│ shading_model        │        │ StandardParams 的值哈希 │
│  Unlit / PBR / Line  │        │ (color/rough/metal/emissive) │
│ flags:               │        └───────────────────────┘
│   vertex_color, uv0, │
│   alpha_test, blend  │        → 材质参数**随实例**上传到实例属性缓冲，
└──────────────────────┘          不占 push constant，不切管线
```

**关键结论**：`StandardParams` 放进**逐实例顶点属性**（`VertexFormat::Float4` × 2 或 packed），
于是：

| 场景 | 材质切换代价 |
|---|---|
| 同一 ShaderKey 下不同颜色 | **0**（同一 draw，实例属性里不同） |
| 不同 ShaderKey（如 Unlit↔PBR） | 1 次 bind_pipeline |
| 需要纹理/色表的材质 | 色表纹理走 256 色索引（实例属性带 u8 索引） |

### 9.3 接口

```cpp
// --- Shader 库：名字 → SPIR-V + 顶点布局 + 参数表 ---
struct ShaderKey {
  std::uint32_t name_hash = 0;     // FNV-1a("std_pbr")
  std::uint32_t flags     = 0;     // 位组合（vertex_color/uv0/alpha_test/...）
  bool operator==(const ShaderKey&) const = default;
};

class IShaderLibrary {
 public:
  // 启动期登记一份 shader 模板（内嵌 SPIR-V，R6）
  virtual Status register_shader(const char* name, const ShaderModuleRef& vs,
                                 const ShaderModuleRef& fs, const VertexLayout& layout,
                                 const std::vector<UniformFieldDesc>& fields) = 0;
  virtual Result<ShaderHandle> acquire(const ShaderKey&, PrimitiveType, const BlendState&,
                                       const DepthState&, const RasterState&) = 0;
  virtual bool has(const char* name) const = 0;
  virtual std::size_t count() const = 0;
};

// --- 材质：只是"一组参数 + 一个 shader 键"的值 ---
struct MaterialKey { std::uint64_t v = 0; };        // 由 MaterialDesc 规范化派生
struct StandardParams {                              // 全部 16 字节对齐，逐实例上传
  float base_color[4] = {1,1,1,1};
  float roughness = 0.5f;
  float metallic  = 0.0f;
  float emissive  = 0.0f;
  std::uint32_t color_index = 0;   // 256 色表索引（0 = 不用色表）
};

struct MaterialDesc {
  ShaderKey shader{};
  StandardParams params{};
  bool transparent = false;
  // 可选纹理（本轮只需要色表）
  ColorTableRef color_table{};
};

class IMaterial {
 public:
  virtual MaterialKey key() const = 0;
  virtual const MaterialDesc& desc() const = 0;
};

// --- MaterialTable：键 → 槽位（合批键），一次登记复用 ---
class IMaterialTable {
 public:
  virtual Result<std::uint32_t> intern(const MaterialDesc&) = 0;  // 相同 desc → 相同槽位
  virtual const MaterialDesc& desc_of(std::uint32_t slot) const = 0;
  virtual const IMaterial* material(std::uint32_t slot) const = 0;
  virtual std::size_t size() const = 0;
};
```

### 9.4 联动规则（写死，不留自由裁量）

1. **材质不含管线**。`MaterialDesc::shader` 只是一个 `ShaderKey`；真正的 `PipelineDesc`
   由 `IShaderLibrary` 用 `(ShaderKey, PrimitiveType, blend, depth, raster)` 派生并缓存。
   → 换材质**不会**悄悄换深度状态（那是 pass 的事）。
2. **材质参数不进 push constant**，进实例属性（§9.2）。
3. **同 ShaderKey 的材质参数差异不拆 drawcall**（§9.2 表）。
4. **透明材质必须与不透明材质用不同 ShaderKey**（保证合批排序键能把它们彻底分开），
   而不是靠一个 `transparent` 布尔在渲染器里分支。
5. **材质变更 = 重新 `intern()`**，旧槽位保留（槽位不回收 → 命令流里的槽位号稳定）。
6. **材质库快照进命令流**（§11.3）：`MaterialKey` + 槽位表。

### 9.5 性能

- `MaterialKey` 是**预计算哈希**，合批比较是 64 位整数比较；
- `intern()` 用开放寻址哈希表，`MaterialKey` 冲突时逐字段比对；
- 实例属性上传 32 字节/实体，1 万个实体 = 320 KB/帧，走 BatchPool 环形（无分配）。

---

## 10. `IGizmoSystem`（U6：事件足够解耦）

### 10.1 问题（用户点名的"补丁式代码"）

常见写法：在 input handler 里 `if (gizmo.hit_test(...)) { ... }`，在 renderer 里
`if (selected) draw_gizmo(...)`，在 scene 里 `if (dragging_axis == X) ...` —— 三处补丁。

### 10.2 方案：**事件总线 + 声明式轴 + 显式状态机**

```
         InputEvent（D6，唯一事件源）
              │
              ▼
   ┌─────────────────────────────┐
   │ IGizmoSystem::on_input()    │  ← 唯一入口；内部：hit-test → 状态机 → 发事件
   └──────────┬──────────────────┘
              │ GizmoEvent（值类型，可进命令流）
              ▼
   ┌─────────────────────────────┐
   │ IGizmoListener 订阅者列表    │  ← 玩法在这里响应；**gizmo 自己不改世界**
   └─────────────────────────────┘
              │
              ▼
   玩法调 IWorld::set_transform / 或直接改 GizmoTarget
```

**四条解耦规则**：

1. **Gizmo 不拥有世界**：它只发 `GizmoEvent{Hovered/BeginDrag/Drag/EndDrag/Canceled, axis, delta, …}`，
   由订阅者改世界。`IGizmoSystem` 内部没有任何 `if (gameplay_name)`。
2. **轴是数据，不是分支**：`GizmoAxis{ id, 局部方向, 颜色, 是否旋转环, 顺序 }` 由
   `configure_target()` 按需声明；增删轴不改代码。
3. **Screen 恒定尺寸是相机原语**：`ICamera::world_size_per_pixel()`（§5.1），gizmo 只调它。
4. **屏幕空间拾取是单点**：`pick_axis(TargetHandle, px, py)` 返回 `axis_id`，命中阈值为像素；
   调用方不需要知道射线/阈值。

### 10.3 接口

```cpp
enum class GizmoMode : std::uint8_t { Translate = 0, Rotate = 1, Scale = 2 };

struct GizmoAxis {
  std::uint8_t id = 0;
  float direction[3] = {1, 0, 0};
  float extent = 1.0f;          // 世界单位基准（再乘 world_size_per_pixel）
  Color4 color{1, 0.3f, 0.2f, 1};
};

struct GizmoTargetDesc {
  GizmoMode mode = GizmoMode::Translate;
  const float* center = nullptr;     // 世界坐标（每帧由玩法给）
  const float* orientation = nullptr; // 3x3，可空 = 世界轴
  float screen_radius_px = 80.0f;
  std::vector<GizmoAxis> axes;       // 声明式
};

struct GizmoEvent {
  enum class Kind : std::uint8_t { HoverBegin, HoverEnd, DragBegin, Drag, DragEnd, DragCancel };
  Kind kind;
  std::uint32_t target = 0;
  std::uint8_t axis = 0;
  float delta[3] = {0, 0, 0};       // 世界空间位移/旋转弧度/缩放
  float total[3] = {0, 0, 0};       // 自 DragBegin 起的累计
  std::int64_t timestamp_us = 0;    // 来自流，不读时钟
};

class IGizmoListener {
 public:
  virtual void on_gizmo(const GizmoEvent&) = 0;
};

class IGizmoSystem {
 public:
  virtual Result<std::uint32_t> configure(const GizmoTargetDesc&) = 0;  // 返回 target handle
  virtual void release(std::uint32_t target) = 0;

  // **唯一**输入入口。返回 true 表示被 gizmo 消费（玩法不再处理这次事件）。
  virtual bool on_input(const device::InputEvent&, const ICamera&) = 0;

  virtual bool dragging() const = 0;
  virtual void cancel_drag() = 0;       // Esc/失焦

  virtual void subscribe(IGizmoListener*) = 0;
  virtual void unsubscribe(IGizmoListener*) = 0;

  // 渲染：gizmo 自己产出一组 DrawItem，由渲染器统一提交
  virtual Status build_draw(const ICamera&, IOverlay2D&) = 0;

  // 命令流：状态可快照/恢复（§11）
  virtual GizmoSnapshot snapshot() const = 0;
};

std::unique_ptr<IGizmoSystem> create_gizmo_system();
```

### 10.4 状态机（显式，可断言）

```
Idle ──hit──► Hovered ──down──► Dragging ──up──► Idle
  ▲              │                  │
  └── leave ─────┘                  └── cancel ──► Idle（发 DragCancel）
```
非法转移（Idle 收到 Drag）→ 忽略 + 计数（不 assert 崩溃，但测试断言不变量）。

---

## 11. 命令录制回放集成（U7）

### 11.1 铁律

> **L3 的全部绘制状态 = `IScene` 快照 + `MaterialTable` 快照 + `CameraPose` + 笔画事件序列 +
> gizmo 状态。五样都进 `CommandStream`。除此之外 L3 不得有任何影响像素的黑盒状态。**

### 11.2 数据流

```
玩法逻辑 ──► IWorld（ECS，高频）
                 │ sync_to_scene（只推脏实体，出帧前一次）
                 ▼
            IScene（L2 权威快照）──► CommandStream.scene
                 │
IRecorder::begin_frame(frame_index, extent, scene)
                 │
                 ├─ 渲染器产命令：begin_pass … bind_pipeline … push_constants … draw_instanced … end_pass
                 │   全部经由 IRenderer 的 11 类方法（双写 W1-W4 自动成立）
                 ▼
CommandStream（可序列化）──► replay()（命令重录）
                        └──► reproduce()（重放输入事件 → 重跑 painter/gizmo → 重录）
```

### 11.3 扩展 `CommandStream` 的落点（**不破坏 L2 格式**）

L2 的 `CommandStream` 已有 `scene` / `events` / `passes` / `commands`。
L3 追加的状态**不建议改 L2 二进制格式**（会升版本、破坏已有 golden 与 `l2_record`）。
**决定（D-L3-5）**：L3 的额外状态编码成 **L3 自己的 `CommandStream` 包装**：

```cpp
struct PrimitiveFrameState {          // 独立于 L2 的二进制，版本号独立
  CameraSnapshot camera;
  std::vector<MaterialSlot> materials;   // 槽位 → MaterialDesc 规范化字节
  WorldSnapshot world;                   // 或复用 CommandStream.scene
  std::vector<StrokeEvent> strokes;
  GizmoSnapshot gizmo;
  std::uint64_t state_hash = 0;
};
```
由 `L3Recorder` 在写 L2 流之前 `encode()` 前者，读回时先 `decode()` 后者。
L2 门控与既有 198 个 deps 测试**完全不受影响**。

### 11.4 确定性校验（三条）

| 校验 | 标准 |
|---|---|
| 录制 vs 回放 | 同一 `CommandStream` 重录 → `stream_hash` 一致 |
| 复现 | 重放 `StrokeEvent` + gizmo 事件 → 同 hash |
| PNG | 离屏出图逐像素一致（L3 golden） |

---

## 12. 用途矩阵（U0：抽象完成后再区分模型用途）

**每个玩法只允许"选原语 + 给参数"，不新增接口**（若需新增 → 回头改 §3 抽象）。

| 玩法 | 摄像机 | 模型（ECS 组件） | 材质 | 画笔 | Gizmo | 拾取 | 覆盖层 |
|---|---|---|---|---|---|---|---|
| 魔法涂鸦 F-SK | 正交 | `StrokeComp` + `TransformComp` | `std::unlit`（顶点色） | ✅ 核心 | ✖ | ✖ | ✖ |
| 魔法积木 F-BL | 透视（orbit） | `MeshRef(图元)` + `MaterialRef` + `Transform` + `GizmoTarget` | `std\pbr` / `std_unlit` | ✖ | ✅ 平移/旋转/缩放 | ✅ AABB/三角形 | ✅ 虚线框 + 0,0,0 网格 |
| 魔法叠叠乐 F-VX | 透视（锁定） | `BlockSetComp`（L4 系统持有一组方块实例，见 R-2 修正） | `std_unlit` + `color_table` | ✅ 方块笔刷 | ✖ | ✅ 方块扫掠求交 | ✅ 网格指示 |
| 印章 F-ST | 俯视正交/透视 | `MeshRef(FromOutline)` + `MaterialRef(金属)` | `std_pbr`（metallic=0.9, rough=0.2） | ✖ | ✖ | ✅ 平面/三角形 | ✖ |

> **R-2 裁定（2026-11-04 修正）**：总 spec 原 P7 的 `IVoxelGrid`（稠密 chunk + 可替换面生成器）
> **不是叠叠乐的正确数据模型** —— 叠叠乐的本质是"一堆小方块沿画笔扫过的路径叠加"，
> 需要方块**实例身份**（颜色、来源笔迹、可整笔撤销），体素会把它压成"格子里有没有东西"。
>
> 修正后：
> - 叠叠乐运行时数据 = **`IBlockSet` 方块实例列表**（总 spec §4 P7a），不是体素网格；
> - 落笔 = **`IBlockBrush` 扫掠求交**（P7b）：路径采样点 + 笔刷半径/层高 → 与已有方块去重 → 批量 emit；
> - 渲染 = **一次实例化 draw call**（同一 `MeshRef(图元)` + 每实例变换/颜色），
>   "只画朝相机 3 个内侧面"靠 `DepthState{depthTest=false, depthWrite+const}` 实现，**与存储无关**；
> - 体素化（`IVoxelizer`）只用于 **OBJ 精模 → 方块** 的**一次性**预处理，输出同样是 `IBlockSet`。
>
> 本题裁定的 L3 范围：`IVoxelGrid` **不进 L3**（也不再是 L4 的运行时结构）；
> L3 提供的是 `IMesh`（图元网格）、`ICamera`、`IOverlay2D`、`IPicker`、`IMaterial`、
> `IBatchBuilder`（实例化合批）—— 叠叠乐的方块渲染完全走这些通用能力，**零玩法专用接口**。

### 12.1 决策表（编号，便于评审与回滚）

| 编号 | 决策 | 备选 | 理由 |
|---|---|---|---|
| D-L3-1 | 公共头矩阵用 POD `Mat4` | `glm::mat4` | 零依赖、可直接进命令流 |
| D-L3-2 | `screen_to_ray` / `world_to_screen` 统一像素坐标原点左上 | NDC 原点左下 | 与 `InputEvent` 同口径，避免点击偏移 |
| D-L3-3 | 透明/不透明用不同 `ShaderKey` | 一个 bool 标志 | 合批排序键能彻底分开 |
| D-L3-4 | L3 合批器独立（`IBatchBuilder`），仍喂 `ISubmitQueue` 供断言 | 直接改 L2 | 不改 L2，门控与既有测试不受影响 |
| D-L3-5 | L3 额外状态独立编码，不改 L2 二进制格式 | 升 L2 版本 | 避免破坏 198 个依赖测试与 l2 golden |
| D-L3-6 | **叠叠乐用方块实例集合（`IBlockSet`），不用体素网格** | 稠密体素 chunk | 体素丢失方块身份、无法整笔撤销、不支持非轴对齐；10k 方块 ≈ 480 KB vs 体素 16 MB+（2026-11-04 修正原"体素归 L4"） |
| D-L3-7 | 对象池暴露 `acquire/release`，不暴露 `new/delete` | 直接 `new` | 强制 0 分配热路径 |
| D-L3-8 | `IWorld` 用 ECS 存储，`IScene` 是命令流权威快照 | 二者合一 | 单一真源进命令流 |

---

## 13. 落地顺序（U8：按难易区分先后）

难度 = 代码量 × 概念风险 × 对后续的阻塞程度。**先做"能画出东西"的最小闭环，再做抽象深化。**

### 里程碑 M1 —— "能画"的最小闭环（难度 ★）

1. `types.h`（Mat4/Vec/Ray/Color/Box）
2. `ICamera`（透视 + 正交 + orbit/pan/zoom + 屏幕↔世界）—— 零 GPU 依赖，纯数学，最先把测试打满
3. `IShaderLibrary` + 4 个基础 shader（`std_unlit` / `line2d` / `stroke_ribbon`）
4. `IBatchPool` + `IFrameArena`
5. `IPainter`（stroke → ribbon 几何 → draw）
6. `IPrimitiveRenderer` 最小版（一个 pass：清屏 + 画笔画）
7. CLI `paint` 子命令 + golden 出图（无头）

**验收**：`magicblocks-l3-cli paint --width 320 --height 200 --strokes 3 --output paint.png`
产出一张有笔画的 PNG；同参数两次逐字节一致；录制/回放 hash 一致。

### 里程碑 M2 —— ECS + 材质（难度 ★★）

8. `IWorld`（archetype SOA + 组件注册 + view）
9. `IMesh` / `MeshData` + `IMeshFactory` 图元（Cube/Box/Quad）—— 先三个，够积木用
10. `MaterialTable` + `IMaterial` + 实例属性上传（§9.2）
11. `IPrimitiveRenderer` 完整版（合批排序 + 实例化 draw）
12. CLI `blocks` 子命令（几个彩色积木）

**验收**：一个场景 1 万实体 → drawcall ≤ 4（按材质/网格合批）；稳态帧分配 0；
命令流重放 hash 一致。

### 里程碑 M3 —— Gizmo + 拾取（难度 ★★★）

13. `IOverlay2D`（屏幕恒宽线、虚线、矩形，无深度）
14. `IPicker`（射线 + AABB / 三角形 / 平面）
15. `IGizmoSystem`（事件总线 + 状态机 + 屏幕恒定尺寸 + 屏幕空间拾取）
16. CLI `gizmo` 子命令（拖动一个盒子，事件全部从 stdin 事件流驱动）

**验收**：gizmo 在任何相机距离下像素尺寸恒定（断言世界尺寸 ∝ `world_size_per_pixel`）；
拖动事件序列可录制/回放；**gizmo 改动零处进入玩法代码**（代码评审）。

### 里程碑 M4 —— 扩展图元 + OBJ + 轮廓（难度 ★★★★）

17. `IMeshFactory` 其余图元（Cylinder/Cone/Sphere/Prism/Trapezoid）
18. `FromOutline`（libtess2 三角化 + Clipper2 合并 + meshoptimizer 优化）
19. `IMeshLoader::obj`（tinyobjloader）
20. `IPicker` 的 BVH 加速（场景实体多时）

### 里程碑 M5 —— 集成门控与回归（难度 ★★★）

21. 新门控 G6（L3 零堆分配热路径，见 §14）
22. L3 golden 三张（paint / blocks / gizmo）
23. 命令模式三态校验（record → replay → reproduce）在 L3 上跑通

---

## 14. 门控与测试

### 14.1 新增门控（`tools/`）

| 门控 | 断言 | 对应 |
|---|---|---|
| G6 `check_l3_no_alloc_in_hotpath` | `src/primitives/**` 的 `render_frame` / `feed` / `on_input` 函数体内无 `new`/`malloc`/`std::vector` 扩容 / `push_back` 在循环外 | U0' / C-L3-b |
| G7 `check_l3_no_gameplay_names` | `src/primitives/**` 与 `include/magicblocks/primitives/**` 无玩法名（同 G3 的列表，不含 SDK 名白名单外） | spec §2.1 |
| G8 `check_l3_command_parity` | `src/primitives/**` 不直接 include backend、不出现 `Vk*`、不出现 raw command（G3 已覆盖，这里做 L3 专项快照） | G3 |

### 14.2 单元测试（gtest，`tests/magicblocks_l3_test.cpp`）

| 套件 | 用例 |
|---|---|
| Camera | `PerspectiveRoundTrip` · `OrthographicRoundTrip` · `ScreenToRayMatchesWorldToScreen` · `WorldSizePerPixelIsScaleInvariant` · `OrbitPanZoomKeepTarget` · `ReverseZRequiresCaps` |
| Painter | `StrokeGeneratesRibbon` · `MoveIsIncremental` · `CancelDropsStroke` · `ReplayRebuildsSameGeometry` |
| World | `SpawnDespawnStableGen` · `AddComponentMigrates` · `ViewOnlyVisitedArchetypes` · `SyncToSceneIsIncremental` · `SnapshotRoundTrip` |
| Material | `InternDedupsSameDesc` · `DifferentShaderKeepsSeparateBatch` · `SameShaderMergesDraws` · `ParamsGoToInstanceAttributes` |
| Gizmo | `ScreenSizeConstantAcrossCameraDistance` · `AxisHitThresholdIsPixels` · `DragEventsAreDeterministic` · `CancelDragEmitsCancel` · `GameplayReceivesOnlyEvents` |
| Pool | `FrameArenaResetIsO1` · `BatchPoolSteadyStateZeroAlloc` · `ObjectPoolReusesSlots` |
| Renderer | `BatchesByShaderMaterialMesh` · `StableSortTieBreak` · `CommandStreamReplayMatchesHash` |

### 14.3 端到端（ctest）

- `magicblocks_l3_render_paint` / `_blocks` / `_gizmo`（无头 PNG）
- `magicblocks_l3_record` → `replay --verify` → `reproduce --verify`
- 现有 198 个依赖测试 + 全部 G1/G3/G4 门控**必须保持全绿**

---

## 15. 明确不做（本轮）

| 项 | 决定 |
|---|---|
| 运行时体素网格（`IVoxelGrid`） | **不做**（§12 R-2 修正）：叠叠乐用方块实例集合；体素化只作 OBJ→方块的一次性预处理 |
| 布尔运算 / 路径合并 | L4 组合（用 Clipper2） |
| 阴影 / 动画 / 骨骼 | 不做（总 spec §10） |
| glTF | 不做 |
| 多线程 / 多队列 | 不做（G5 至今成立） |
| 显式 SIMD | 不做，先量测 |
| 描述符集 / bindless | 不做（R4 策略仍是 push constant + 实例属性） |
| L2 二进制格式变更 | 不做（D-L3-5：L3 状态独立编码） |

---

## 16. 风险与对策

| 风险 | 影响 | 对策 |
|---|---|---|
| 实例属性缓冲容量（移动端 128B push constant 不够放材质） | 高 | 材质放实例属性（§9.2），push constant 只放 view_proj（64B）+ 时间（可选） |
| ECS archetype 迁移抖动（频繁加删组件） | 中 | 组件设计"预分配"，按 archetype 一次性创建（spawn 时给全组件） |
| Gizmo 事件与玩法逻辑耦合 | 中 | 事件总线 + 只读目标（§10.2 规则 1） |
| 合批排序稳定性 | 高 | 排序键全为整数 + tie-break 稳定（EntityId），量化浮点（同 L2 手法） |
| 命令流增量编码未做 | 中 | 第一版全量；规模上来后加差分（§11.3 预留 `state_hash`） |

---

## 17. 与总 spec 的对应

| 总 spec | L3 落地 |
|---|---|
| P1 `IMeshFactory` 图元 | M2（三个）+ M4（其余） |
| P2 `FromOutline` | M4 |
| P3 `IMeshLoader::Obj` | M4 |
| P4 `IPicker` | M3 |
| P5 `IOverlay2D` | M3 |
| P6 `IFillPath` | M1 = `IPainter`（最小版） |
| P7 `IVoxelGrid` | **不做**（§12 R-2 修正）→ 叠叠乐改用方块实例集合 `IBlockSet` + `IBlockBrush`（总 spec §4 P7a/P7b） |
| P8 `IMaterial`/`PbrParams` | M2（§9） |
| P9 `ICamera` | M1（§5） |
| P10 `IWorldGrid` | L4（依赖 Picker） |
| P11 `IGizmoSystem` | M3（§10） |
| P12 通用状态 | 已由 L1 `DepthState`/`CullState` 覆盖，L3 只组合 |
