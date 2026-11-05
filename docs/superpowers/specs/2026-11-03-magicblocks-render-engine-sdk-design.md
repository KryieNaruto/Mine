# 魔法功能预览 —— 渲染引擎 SDK 设计（MagicBlocks）

日期：2026-11-03（2026-11-05 第 5 轮：追加 UI 绘制路线裁定）
状态：设计定稿（第 5 轮，已就用户硬约束 + UI 合成方案一裁定修订）

## 1. 背景与目标

「魔法」系列四个玩法（魔法涂鸦 / 魔法积木 / 魔法叠叠乐 / 魔法捏捏乐 / 印章）目标平台是
**Android + iOS**，当前阶段**只做渲染引擎**，不做业务 UI。

本文档定义的是**一个通用渲染引擎 SDK**及其接口布局，不是「给这四个玩法写专用渲染器」：

> 玩法只是 SDK 的客户，用 SDK 的**通用能力**组合出来；SDK 内部不得出现玩法专用通道。

同时本轮设计必须把三件事作为**一等公民**：

1. **无几何着色器**（移动端 Mali/Adreno 对 GS 支持差且慢）——不是"尽量不用"，是**硬禁**。
2. **命令模式**——所有渲染提交成为可录制、可回放、可复现、可序列化的命令流，为之后的
   「命令复现操作」打地基。
3. **内存分配**——使用 **AMD VMA v3.5.0**，**优先分配大内存块、再以偏移切小资源复用**，
   避免 GPU 内存频繁创建/销毁。

### 1.1 用户已确认的约束（逐条）

| # | 约束 | 落地章节 |
|---|---|---|
| C1 | 目标 Android + iOS；本轮只做 Android，iOS 只做设计考虑（不搭工程） | §2、§9 |
| C2 | 后端用 **RHI 抽象层**；GLES / Metal **本轮不做**，只落地 **Vulkan** 后端 | §2 |
| C3 | 本轮产出**只是一份设计文档**（spec），不写代码、不建工程 | 全文 |
| C4 | **禁止几何着色器** | §3、§7 门控 G1/G2 |
| C5 | **以 SDK 出接口的方式布局**，不做专门专用，要通用覆盖 | §2、§4 |
| C6 | **预留离屏与命令模式**，之后要能命令复现操作 | §5 |
| C7 | **内存分配现在就设计**：VMA、大块分配 + 偏移小内存、避免频繁创建销毁 | §6 |
| C8 | **UI 不自研，用现成框架**（Compose）；SDK 只做 3D 场景渲染 | §2.3、§9 |
| C9 | **UI 合成采用方案一**（2026-11-05 用户最终裁定）：引擎输出 1、UI 输出 2，两者独立、由系统合成器合并 | §2.3、§5.5 |

### 1.2 UI 与 3D 的分工（2026-11-05 追加）

用户原话：「UI 必须用现成的，不然 UI 全都得自研」，随后最终裁定合成方案为**方案一**。

> **一句话**：**SDK 画 3D，Compose 画控件，系统合成器合并。SDK 不参与 UI 绘制，也不参与最终合成。**

这条裁定推翻了早期草案里「场景 + UI 同帧、同一 command list、单次 submit」的想法。
调研依据见 [UI 绘制路线调研](2026-11-05-magicblocks-ui-rendering-route-research.md)。

## 2. 分层与接口布局（SDK 出接口方式）

```
┌────────────────────────────────────────────────────────────┐
│ 应用壳层（平台原生，SDK 之外）                                 │
│   Android: FrameLayout                                       │
│     ├─ [0] SurfaceView   ← SDK 渲染 3D 场景（输出 1）         │
│     └─ [1] ComposeView   ← Compose 渲染控件（输出 2）         │
│   iOS: root UIView                                           │
│     ├─ [0] 原生 3D 视图 / CAMetalLayer（输出 1）               │
│     └─ [1] ComposeUIViewController（输出 2）                  │
│   ⟶ 两路输出由 SurfaceFlinger / CoreAnimation 独立合成上屏     │
├────────────────────────────────────────────────────────────┤
│ 玩法业务层（魔法涂鸦 / 积木 / 叠叠乐 / 印章 —— SDK 的客户）      │
│   只 #include <magicblocks/...> 公共头，不碰任何后端类型        │
├────────────────────────────────────────────────────────────┤
│ L4 features/    玩法组合层（每个功能 = L0–L3 通用调用的组合）    │
├────────────────────────────────────────────────────────────┤
│ L3 primitives/  通用能力层（mesh factory / path fill / block set   │
│                 / picker / gizmo / overlay2d / pbr material）  │
├────────────────────────────────────────────────────────────┤
│ L2 scene/       场景与渲染图（IScene / IRenderGraph /           │
│                 ISubmitQueue / FrameCapture / IRecorder）       │
├────────────────────────────────────────────────────────────┤
│ L1 resources/   资源、命令、内存（IBuffer / ITexture /          │
│                 IPipeline / ICommandList / IMemoryAllocator）    │
├────────────────────────────────────────────────────────────┤
│ L0 device/      设备与平台（IRenderDevice / ISwapchain /        │
│                 IRenderTarget / InputEvent / 生命周期）         │
├────────────────────────────────────────────────────────────┤
│ backend/vulkan  唯一后端实现（Vk 类型只允许出现在这一层）        │
│                 内部使用 VMA v3.5.0 做分页+偏移子分配           │
└────────────────────────────────────────────────────────────┘

※ SDK 的边界止于「把 3D 画面画进一个 Surface」。
  UI 不在 SDK 内，合成也不在 SDK 内（方案一）。
```

### 2.1 依赖方向（单向）

- L4 → L3 → L2 → L1 → L0；**反向依赖禁止**（L2 不能 include L3，L1 不能 include L2）。
- L2–L4 不得 `#include <vulkan/vulkan.h>`、不得出现 `Vk*` 类型。
- `backend/` 不得出现任何玩法名（`sketch` / `blocks` / `voxel` / `stamp`）。
- L4 features 只组合调用 L3/L2 公共接口；SDK 内部（L0–L3）**零玩法分支**。

### 2.2 为什么这不是"专用"

| 玩法需求 | SDK 提供的通用能力 | 玩法层做的事 |
|---|---|---|
| 2D 透明画布 → 3D 挤出 | `IFillPath`（路径光栅）+ `IMeshFactory::FromOutline`（带孔轮廓 → 三角化 → 侧壁/上下盖） | 拼两步、给参数 |
| 圆柱/球/长方/梯形… + 导入 OBJ | `IMeshFactory` 图元生成器 + `IMeshLoader::Obj` | 选图元、给尺寸 |
| 点击模型自动找空位（洪水） | `IPicker`（射线求交）+ 场景包围盒查询 + `IWorldGrid`（均匀网格占用查询） | 写放置搜索策略 |
| 选中虚线无深度包围盒 / 操作 UI 跟随视角不缩放 | `IOverlay2D`（屏幕空间线、虚线、恒宽）+ `IGizmoSystem`（屏幕恒定尺寸 + 屏幕空间拾取） | 声明包围盒与轴 |
| 透视、0,0,0 网格地面 | `ICamera`（正交/透视）、`IOverlay2D` | 配置 |
| 叠叠乐内面渲染 / 关深度测 / 网格指示 / 颜色模式 / 层 / 整笔撤销 | `IBlockSet`（方块实例，按 `stroke_id` 整笔增删）+ `IBlockBrush`（扫掠求交）+ 通用 `DepthState`（含 constant depth write）+ `IOverlay2D` | 组织笔刷逻辑（**不用体素网格**，见 §4.1） |
| 印章底板轮廓 / 印面深度突出五官 / 金属把手 | `IMeshFactory::FromOutline` + `IMaterial`(PBR/纯色) + 纹理双通道（颜色+深度） | 选择资源与参数 |

**结论**：玩法之间共享的每一条需求都有对应通用接口；没有一条需求需要新增"玩法专用渲染分支"。

### 2.3 UI 合成方案一裁定（2026-11-05）

#### 2.3.1 两个方案的差别

| | **方案一（采纳）** | 方案二（否决） |
|---|---|---|
| 引擎输出 | 结果 1（3D 场景 → 一张 Surface） | 3D 场景 |
| UI 输出 | 结果 2（Compose → 另一张 Surface） | UI 纹理（需导出为纹理） |
| 合成者 | **系统合成器**（Android SurfaceFlinger / iOS CoreAnimation） | **引擎**（UI 纹理作为一次 draw 进引擎 pass） |
| 每帧额外带宽 | **0**（系统合成器本就每帧合成所有 layer，且有专用硬件 DPU） | **+2 次全屏读写**（UI 写一次纹理 + 引擎采样一次） |
| 3D/UI 帧率 | **完全解耦** | 强绑定，UI 静止时引擎仍要等/浪费一次合成 |
| 复杂度 | 几十行（两个兄弟 view） | 数百行：`AHardwareBuffer` 导入 + `VK_EXT_external_memory_android_hardware_buffer` + 同步 + 色彩空间 |
| iOS 可行性 | ✅ CMP 的 `UIKitView` 本身就是这个模型 | ❌ CMP 无公开 API 导出 `MTLTexture` |
| 唯一优势 | — | 可对 UI+3D 做统一后处理 / UI 可被 3D 遮挡 / UI 可作为 3D 纹理 |

#### 2.3.2 为什么选方案一

1. **四个玩法的 UI 全是环绕式控件**（顶栏 / 底栏 / 调色板 / 工具面板），
   **没有任何一个需要"被 3D 物体遮挡"**。
2. **UI 必须永远可点** —— 若 3D 能盖住 UI，用户就点不到按钮，方案二反而引入 Z 序管理负担。
3. **带宽与功耗更优**：现代 SoC 的显示控制器（DPU）合成多图层几乎不占 GPU；
   方案二把便宜的事（硬件合成）交给 GPU 去做，是负优化。
4. **延迟更低**：点按钮后 Compose 立即刷新自己的 Surface，不必等引擎下一帧。
5. **符合 R1 裁定**：SDK 本就把上屏移交壳层，方案一是它的自然延伸。

#### 2.3.3 架构落位

```
Android:  FrameLayout
   ├─ [0] SurfaceView    ← SDK 渲染 3D（独立 Surface，独立 layer）
   └─ [1] ComposeView    ← Compose 渲染控件（独立 Surface，透明背景）
   触摸 → FrameLayout.dispatchTouchEvent
          ├─ 命中 Compose 交互区 → 给 Compose
          └─ 否则 → SurfaceView → JNI → SDK InputRouter

iOS:  root UIView
   ├─ [0] 原生 3D 视图（MoltenVK → CAMetalLayer）
   └─ [1] ComposeUIViewController 的视图
   注意 CMP 的 UIKitView 模型是「原生 view 插到 Compose 渲染之后 + DstAtop 挖洞」，
   即原生 3D 层永远在 Compose 之下；Info.plist 必须带 CADisableMinimumFrameDurationOnPhone
```

#### 2.3.4 门控（新增）

| 编号 | 门控 | 检查方式 |
|---|---|---|
| **UIG1** | **SDK 零 CPU 回读** | 静态扫描 `src/` 无 `vkCmdCopyImageToBuffer` 到 CPU / `vkMapMemory` 用于读像素；运行时计数必须为 0 |
| **UIG2** | **SDK 不参与 UI 合成** | `include/magicblocks/**` 不得出现 `UI`/`Compose`/`Canvas`/`widget` 概念的接口；SDK 每帧只有**一次 submit**（自己的 3D 场景） |
| **UIG3** | **3D 层不得进 Compose 渲染树** | 壳层代码评审：3D view 必须是 Compose 的**兄弟节点**，不得被 `AndroidView { SurfaceView }` 包进 Compose 树 |
| **UIG4** | **禁止 TextureView** | 壳层静态扫描 `TextureView` 出现即失败（会多一次 GPU→GPU 拷贝） |

#### 2.3.5 预留：未来若要方案二

若将来真需要「UI 与 3D 统一后处理」，只需把 Compose 的输出换成一张纹理，
SDK 侧增加一次 `draw`（`ICompositeSource` 抽象位），**架构不用改**。
本轮**不实现**，只保留这个回滚点。


## 3. 硬性技术门控

| 编号 | 门控 | 检查方式 |
|---|---|---|
| G1 | **无几何着色器** | (a) `IPipeline` 的 shader stage 只接受 `Vertex/Fragment/Compute`；(b) 构建期扫描 `shaders/**/*`，出现 `#version` 之外的 `layout(...) in` 且 stage 名为 `geom`/`geometry`，或 `glslc -fshader-stage=geom/geometry` 直接失败；(c) `tools/check_no_geometry_shader` 脚本扫描源码中出现 `triangles with adjacency` / `GL_GEOMETRY_SHADER` 等字样即失败 |
| G2 | 无 GS 的替代方案 | 一切"造几何"必须走：**实例化绘制**（`drawInstanced`，顶点着色器按 `gl_InstanceIndex` 取参数）+ push constant/UBO 参数表；体素方块、印章圆柱/圆台、gizmo 轴环、线框盒全部用实例化 + 程序化顶点 |
| G3 | 分层纯度 | L2+ 无 `Vk` 类型；backend 无玩法名；features 不直接构造 GPU 命令 |
| G4 | 内存路径唯一 | `backend/` 外禁止 `vkAllocateMemory/vkFreeMemory`；`backend/` 内除 VMA 注入点外禁止裸分配 |
| G5 | 单队列 | 不做多队列/异步计算/多线程录制 |
| **UIG1** | **SDK 零 CPU 回读** | 静态扫描 + 运行时计数（上屏路径读回像素 = 0 次） |
| **UIG2** | **SDK 不参与 UI 合成** | 公共头无 UI/Compose/Canvas 概念；每帧一次 submit |
| **UIG3** | **3D 层不进 Compose 渲染树** | 壳层评审：必须是兄弟节点 |
| **UIG4** | **禁止 TextureView** | 壳层静态扫描 |

## 4. 基础功能点清单（按层）

### L0 device —— 设备与平台

| # | 功能点 | 说明 |
|---|---|---|
| D1 | `IRenderDevice` 与 `DeviceDesc` | 创建/销毁；`BackendType` 枚举（本轮仅 `Vulkan`） |
| D2 | `DeviceCaps` 能力探测 | 支持的深度格式（D16/D24S8）、MSAA 级别、纹理格式、是否支持 rev-Z、最大 push constant、时间戳查询、压缩纹理（ASTC/ETC2）、**VMA 显存预算** |
| D3 | `ISwapchain` | Android `ANativeWindow` + `VK_KHR_android_surface`；桌面 GLFW（CI/工具链）；重建时机：旋转、`APP_CMD_INIT_WINDOW/DESTROY_WINDOW`、前后台；present mode 默认 FIFO、可探测 MAILBOX |
| D4 | `IRenderTarget` 同构抽象 | `WindowTarget` 与 `OffscreenTarget` 是同一种对象（颜色 + 深度 + 可选模板 + 可选 MSAA resolve）；离屏支持读回 RGBA8；**CLI/CI/golden 全走离屏** |
| D5 | 帧同步 | fence / semaphore 封装；N=2 或 3 的帧环形资源复用 |
| D6 | `InputEvent` 统一事件流 | 触摸/指针/窗口事件统一；**命令复现回放同一事件流** |
| D7 | 生命周期 | Android `onPause/onResume/onSurfaceChanged`；iOS 的 `CAMetalLayer` 等价抽象位预留（不实现） |
| D8 | 失败即报错 | 能力/预算不足 → 明确报错，不静默降级（不回退原则） |

### L1 resources / memory / command

| # | 功能点 | 说明 |
|---|---|---|
| R1 | `IBuffer` / `ITexture` / `ISampler` | 顶点/索引/UBO/存储；纹理 RGBA8/R8/深度/Mipmap/可选压缩；**全部从内存页切偏移**，不单独走 VMA 分配 |
| R2 | `IPipeline` + `PipelineDesc` | `{vertexLayout, shaders(Vertex/Fragment/Compute), blend, depthStencil, raster(cullMode/depthBias/wireframe? 不支持线框多边形模式), msaa, primitive(Rect/Line)}`；**无 geometry stage 字段** |
| R3 | `ICommandList` | `bindPipeline` / `bindVertexBuffer` / `bindIndexBuffer` / `draw` / `drawIndexed` / `drawInstanced` / `pushConstants` / `setViewport` / `setScissor` / `beginPass` / `endPass` |
| R4 | 描述符策略 | **push constant 优先 + 少量 UBO 页 + 一张动态色表纹理**；避免描述符集频繁创建 |
| R5 | 上传队列 | staging 缓冲 → GPU 副本，按帧批处理；上传也走分页分配器 |
| R6 | shader 工具链 | GLSL（仅 Vertex/Fragment/Compute）→ glslc → SPIR-V → 内嵌 C++ 头（沿用 `EasyPainter/shaders/embed_shaders.py` 范式） |
| **M1** | `IMemoryAllocator` | 抽象接口 + 默认 `VmaAllocator` 实现（VMA v3.5.0）；SDK 不暴露裸 `vkAllocateMemory` |
| **M2** | **分页 + 偏移子分配** | 设备内存按 `64 MB` 大页、Host 可见按 `8 MB` 页；页内按用途分桶（Vertex/Index/Uniform/Texture/Staging），**桶内空闲链表切出偏移**给多个小资源共用一页 |
| **M3** | **句柄 = 偏移** | 资源句柄为 `{page_id, offset, size, id}`；绘制时只传偏移（UBO 环形缓冲 = 连续大 buffer 的偏移 + push constant）；命令流记录偏移而非指针 → 可复现 |
| **M4** | **稳定驻留** | 分配/释放与创建/销毁解耦：销毁 = 把子块还回本页（O(1) 合并），**热路径 0 次 VMA 调用**；页空置超过 N 帧且无在途引用才退回 VMA |
| **M5** | 计数门槛 | 稳态帧（无资源变化）**VMA alloc/free 次数必须为 0**（单测断言） |
| **M6** | 预算与淘汰 | `DeviceCaps` 给出显存预算；超预算明确报错；淘汰策略只做整页 LRU 释放 |

### L2 scene —— 场景、渲染图、命令模式

| # | 功能点 | 说明 |
|---|---|---|
| S1 | `IScene` | 节点树 + TRS 变换 + 包围盒/包围球 + 可见性；节点 ID 稳定 |
| S2 | `ISubmitQueue` | 每帧提交排序（不透明按材质/深度、透明按相机距离），**排序键必须稳定**（含 tie-break） |
| S3 | `IRenderGraph` | pass 声明 + 附件依赖 + 读写图，统一"先离屏、再贴图"的合成范式 |
| S4 | **帧命令录制** | 整帧 = 一条 `CommandStream`：pass 列表 + 命令 + 资源偏移引用 + 参数快照 |
| S5 | **命令模式三态** `IRecorder` | `Recording`（录制）/ `Replaying`（直接回放 GPU 命令）/ `Reproduce`（重放输入事件 + 同源场景重建） |
| S6 | **序列化** | `CommandStream` 二进制（带版本号）+ 可选 JSON 文本（可 diff）；`FrameCapture::save/load` |
| S7 | **确定性** | 命令哈希（命令序列 + 参数 + 偏移）作为回放/复现校验；排序稳定 |
| S8 | 帧自包含 | 捕获的帧携带 `frame_index` 与输入事件时间戳，复现不依赖实时时钟 |
| S9 | `IRenderFeature` 注册表 | 特性以插件式注册进渲染图（引擎不内建玩法） |

### L3 primitives —— 通用能力（覆盖所有需求）

> **2026-11-04 修正（叠叠乐数据模型）**：原 `P7 IVoxelGrid`（稠密 chunk）**不是叠叠乐的正确数据模型**。
> 叠叠乐的本质是"**一堆小方块沿画笔扫过的路径叠加**"，需要的是**方块实例集合 + 扫描求交**，
> 而不是体素网格。体素化只是"OBJ 精模 → 方块"的**一次性预处理**，不是运行时数据结构。
> 详见 §4.1。修正后 P7 → P7a/P7b/P7c，P6 也随之调整。

| # | 接口 | 覆盖需求 |
|---|---|---|
| P1 | `IMeshFactory`：`Cube / Box / Cylinder / Cone / Sphere / Tetrahedron / Trapezoid / Prism`，参数化（尺寸、分段、封盖） | 积木基础模型 |
| P2 | `IMeshFactory::FromOutline`：2D 轮廓（可带孔）→ 三角化 → 上下盖 + 侧壁挤出（libtess2） | 涂鸦挤出、印章底板轮廓 |
| P3 | `IMeshLoader::Obj`：`v/vt/f`（多边形三角化、缺法线重算），失败明确报错（tinyobjloader） | 积木导入 OBJ |
| P4 | `IPicker`：屏幕 → 世界射线（正交/透视）；求交器 AABB / 三角面 / 平面 / **方块实例** | 积木点选、叠叠乐刷子、印章落印 |
| P5 | `IOverlay2D`：屏幕/像素空间线、虚线、描边、矩形（**屏幕恒宽**） | 积木虚线线框、叠叠乐网格指示 |
| P6 | `IFillPath`：2D 路径光栅化（线段 walk / 胶囊）、圆头笔刷 → 输出**路径采样点集**（不再是"最近体素落笔"） | 涂鸦笔画、叠叠乐笔刷路径 |
| **P7a** | **`IBlockSet`（原 `IVoxelGrid`）**：方块**实例列表**（`position/rotation/size/color/stroke_id`）；按 `stroke_id` 整笔增删、按 AABB 范围查询；**线性存储，非网格** | 叠叠乐主数据结构；撤销一笔 = O(n) 删整笔 |
| **P7b** | **`IBlockBrush`**：画笔扫掠求解 —— 路径采样点 + 笔刷半径/层高 → 与已存在方块求交去重 → 批量 emit 新方块；**"格子"概念由调用方策略注入**（轴对齐网格 / 自由放置 / 模板） | 叠叠乐落笔；自动堆叠不穿模 |
| **P7c** | **`IShapeTemplate`**："任意形状" = 一组方块实例（模板文件），不是体素网格；`IVoxelizer`（一次性）把 OBJ/网格 → 方块实例（**只在导入时用**） | 叠叠乐任意形状扩展点、OBJ 精模转方块 |
| P8 | `IMaterial` / `PbrParams`：纯色 + 可选 PBR（metallic/roughness）、半球光近似 | 积木纯色、印章金属把手 |
| P9 | `ICamera`：正交 + 透视（fovY / near / far / rev-Z 可选）、orbit/pan/zoom、屏幕↔世界 | 四个功能共用 |
| P10 | `IWorldGrid`：均匀网格占用查询（洪水搜索空位） | 积木自动放置 |
| P11 | `IGizmoSystem`：轴/环几何（实例化 + 程序化顶点）+ **屏幕恒定尺寸** + 跟随视角 + 屏幕空间拾取解算 | 积木移动/旋转/缩放 UI |
| P12 | 通用状态：`DepthState{depthTest, depthWrite, depthOp, depthConstWrite}`，`CullState` | 叠叠乐"永远显示内面"= `depthTest=OFF + depthWrite=ON(depthConst=near)` + 背面可见 |
| P13 | **可选** `ISpatialIndex`（BVH）：方块实例数量大时的占用查询加速；**不引入时 `IBlockSet` 线性扫描即可** | 叠叠乐大场景、积木大模型拾取 |

#### 4.1 叠叠乐数据模型裁定（为什么放弃体素）

| 需求 | 体素网格 | **方块实例集合（采纳）** |
|---|---|---|
| 一笔扫过路径叠一堆小方块 | ⚠️ 丢失方块身份 | ✅ 天然 |
| **撤销一笔**（整笔消失） | ❌ 只能逐格改 | ✅ `erase_if(stroke_id)` |
| 一笔一个颜色 | ⚠️ 靠逐格染色模拟 | ✅ `Block.color` |
| 改色不动几何 | ❌ 需重建 chunk | ✅ 改 4 字节 |
| 方块数 > 数万 | ❌ 内存与重建爆 | ✅ 10k 方块 ≈ 480 KB；体素同规模 16 MB+ |
| 改一笔不动几何 | ❌ 需重编 chunk 面 | ✅ 只增删实例，零重编 |
| **任意形状** | ❌ 只能是单位立方联合 | ✅ 换一个 `IShapeTemplate` |
| **OBJ 精模转小方块** | ✅ **体素化唯一真正合适的场景** | ✅ 走一次性 `IVoxelizer`，输出**同一个 `IBlockSet`** |

**结论**：体素是**输入侧的一次性预处理**（P7c 的 `IVoxelizer`），不是运行时数据结构。
运行时只有 `IBlockSet`（存什么）+ `IBlockBrush`（怎么长）+ `IShapeTemplate`（长成什么形状）。

> **Q1 已裁定（2026-11-04，选 A）**：方块**必须贴齐 0,0,0 轴对齐网格**，尺寸统一，位置只能落在整格点上。
> 实现上 `Block.cell` 用 **`int32[3]`** 格点索引而非浮点位置 —— 抗浮点漂移、命令流哈希稳定、省内存。
> 落笔链路 = 射线 → 落笔层平面 → `floor((p-origin)/cell_size)` 格点吸附 → 3D 超覆盖 DDA 扫掠去重。
> 详见 L3 设计 §6A。

### L4 features —— 玩法组合（只用 L0–L3）

| 功能 | 组合路径 | 需求映射 |
|---|---|---|
| F-SK 魔法涂鸦 | `IFillPath`（透明 2D 画布）→ `FromOutline` 挤出 → `ICamera` 正交/透视 | 2D 透明 → 3D 挤出面 |
| F-BL 魔法积木 | `MeshFactory`/`Obj` → `ICamera` 透视 → `IPicker` 点选 → 实例化绘制；放置 = 包围盒 + `IWorldGrid` + 沿法线洪水搜索（`min.y ≥ 0`）；选中 = `IOverlay2D` 无深度虚线包围盒 + `IGizmoSystem` 屏幕恒定尺寸 | 积木 6 条（含 0,0,0 网格平面） |
| F-VX 魔法叠叠乐 | `IBlockBrush`（射线 → 格点吸附 → 超覆盖扫掠）→ `IBlockSet` 增删（按 `stroke_id` 整笔可撤销）→ **2 次实例化 draw**（表面 + 内面 pass）；内面渲染（`depthTest=OFF + depthWrite const`）+ `IOverlay2D` 网格；笔刷锁层；**颜色模式 = 改 `Block.color`**，不动几何；"任意形状" = `IShapeTemplate`。**方块坐标 `int32 cell[3]`，贴齐 0,0,0 网格** | 叠叠乐 6 条（含任意形状扩展点） |
| F-ST 印章 | 底板轮廓 `FromOutline` / 模板；印面 = 颜色 + 深度双贴图（透视下靠深度遮挡凸显五官）；把手 = `PbrParams` 金属 + 圆柱/圆台 | 印章 3 条 |
| F-PN 捏捏乐 | 本轮不做（不建桩） | — |

## 5. 命令模式与离屏（第 6 条约束的落地）

### 5.1 三态

```
应用逻辑（touch/笔刷/scene 修改）
   │  只经 ISubmitQueue / IRenderGraph 提交
   ▼
IRecorder::BeginFrame(frame_index, 输入事件) ──► CommandStream（可序列化）
   │                                                   │
   ├─ Recording：正常渲染 + 录制                        │
   ├─ Replaying：直接回放 GPU 命令（不跑应用逻辑）◄──────┤ 验证「渲染正确性」
   └─ Reproduce：回放输入事件 + 从同一初始场景重建 ◄─────┘ 验证「操作可复现」
```

### 5.2 命令流内容

每条命令记录：`op`（bindPipeline/draw/drawInstanced/pushConstants/beginPass…）、**资源偏移**
（`page_id/offset/size`，而非指针）、参数快照（矩阵、色调、深度状态）、pass 边界。
`CommandStream` 头部带：格式版本、引擎版本、后端类型、`frame_index`、输入事件摘要、初始场景哈希。

### 5.3 三类校验

| 校验 | 方法 | 通过标准 |
|---|---|---|
| 录制 vs 回放 | 同一 `CommandStream` 重新回放到离屏 target | 命令哈希一致 + PNG 逐像素一致 |
| 复现 | 回放输入事件流，从相同初始场景重跑应用逻辑 | 命令哈希一致 + 最终 PNG 逐像素一致 |
| 跨版本 | 文本 dump 可 diff | 格式版本号不匹配时明确报错 |

### 5.4 离屏是一等公民

- `OffscreenTarget` 与 `WindowTarget` 在 RHI 层是同一 `IRenderTarget` 抽象；
- 所有 CI/golden 测试、录制回放、复现都跑在离屏上，**不需要窗口/显示服务**；
- CLI：`magicblocks-cli --scene sketch|blocks|voxel|stamp --frame N --output out.png`；
  `--record cap.bin`、`--replay cap.bin --output out.png --verify`。

### 5.5 上屏路径（方案一：两路独立输出）

**上屏 = 把 3D 场景画进 Surface，然后交给系统合成器。UI 完全不在这个链路上。**

```
每帧（SDK 内部）：
  acquire Surface image
    → 场景 pass（L2 render graph）
    → present
  结束。没有 UI pass，没有 UI 纹理采样，没有 CPU 回读。

系统合成器（SDK 之外）：
  layer[0] = SDK 的 SurfaceView（3D）
  layer[1] = Compose 的 Surface（控件，透明背景）
  → SurfaceFlinger / CoreAnimation 合成上屏
```

| 项 | 值 |
|---|---|
| SDk 每帧 submit 次数 | **1** |
| CPU 回读像素次数 | **0** |
| 依赖 UI 纹理 | **否** |
| 3D 与 UI 帧率 | **独立** |
| 离屏入口 | 仅 CLI / golden / 录制回放使用（§5.4），与上屏互不影响 |

> **与早期草案的区别**：早期写过「场景 pass + UI pass 都渲染到同一张 swapchain image，单次 submit」。
> **该条已作废** —— UI 归 Compose，SDK 不画 UI。
> SDK 的「单次 submit」含义变为：**SDK 自己那一帧（3D 场景）只提交一次**。

## 6. 内存分配设计（第 7 条约束的落地）

### 6.1 分层

```
IMemoryAllocator            ← L1 公共接口（SDK 不暴露 vkAllocateMemory）
   └─ VmaAllocator          ← 唯一实现，VMA v3.5.0
        ├─ DevicePage × N     64 MB 大页（DEVICE_LOCAL）
        └─ HostVisiblePage × N  8 MB 页（HOST_VISIBLE|HOST_COHERENT）
             └─ Bucket[用途] → 空闲链表 → 子分配 {offset,size}
```

### 6.2 规则

1. **大块优先**：进程启动时按 `DeviceCaps` 预算预留首组大页（而不是每个资源一次分配）。
2. **偏移复用**：资源从所在页的空闲链表切出 `offset`；释放时归还子块并合并相邻空闲（O(1) buddy/链表）。
3. **热路径零 VMA 调用**：稳态帧内不得触发任何 `vmaAllocateMemory` / `vmaFreeMemory`（单测断言计数）。
4. **页回收条件**：页内空闲率 100% 且持续 N 帧（默认 120）且无在途帧引用 → 退回 VMA 或保留给下一轮复用。
5. **帧内顺序**：`vmaSetCurrentFrameIndex` 每帧推进，让 VMA 自己的延迟释放与引擎帧边界一致。
6. **显存预算**：`DeviceCaps.memoryBudget` → 超限时明确报错（不静默 OOM 到系统）。
7. **VMA 建议参数**：不创建 dedicated allocation（小资源一律子分配）；使用 `VMA_ALLOCATION_CREATE_MAPPED_BIT` 给 Host 页；设备支持时打开 `VMA_ALLOCATOR_CREATE_BUFFER_DEVICE_ADDRESS_BIT`（为后续 buffer reference 提速）。

### 6.3 与命令模式的耦合（关键）

资源句柄是 `{page_id, offset, size, id}`，**命令流记录偏移而不是指针**，所以：

- 回放时同一 `page_id/offset` 指向同一片 GPU 内存，命令语义完全一致；
- 复现时若资源重建（页重新分配），命令流里的偏移仍映射到"同一逻辑资源槽位"，靠 `id` 做一致性校验；
- 这避免了"命令流记录指针 → 回放 dangling"的经典陷阱。

## 7. 门控脚本（实现阶段交付）

| 脚本 | 断言 |
|---|---|
| `tools/check_no_geometry_shader` | `shaders/` 与 `src/` 无 GS stage、无邻接三角形用法 |
| `tools/check_layer_purity` | L2+ 无 `vulkan/vulkan.h` / `Vk*`；backend 无玩法名；features 不直写 GPU 命令 |
| `tools/check_no_raw_alloc` | `backend/` 外无 `vkAllocateMemory`；backend 内仅 VMA 注入点 |
| `tools/check_deps_tests` | 既有 `tools/tests` 仍全绿（本次改动不破坏 build-deps 行为） |

## 8. 移动端约束与预算

| 维度 | 决策 |
|---|---|
| 顶点处理 | 无 GS；所有批量用实例化 + 顶点着色器 `gl_InstanceIndex` 查参数 |
| MSAA | 默认 4x（TBR 便宜），能力不足按 `DeviceCaps` 报错或降级到可用级别（记录警告） |
| 深度 | D16 或 D24S8（按能力）；rev-Z 可选（为精度要求高的场景） |
| 纹理 | 256 色动态色表（叠叠乐/印章）；可选 ASTC/ETC2 |
| 面数 / drawcall | 单场景 ≤ 200k 三角面、≤ 300 drawcall（目标值，正确性优先） |
| 帧时间 | 60 fps 目标；**叠叠乐方块改色/撤销笔迹只改实例列表，不重编任何几何**（方块实例 10k 级 ≈ 480 KB） |
| 排序 | 统一在 L2 排序，透明按距离，tie-break 稳定 |
| 同步 | 单队列单线程录制 |
| **UI 合成** | **方案一**：3D 与 UI 两路独立输出，系统合成器合并；SDK 不算 UI 带宽 |
| **UI 控件预算** | Compose `graphicsLayer` 调用点 ≤ 20；禁止 `Modifier.blur`；禁止每帧改文本 |

## 9. 平台策略

| 平台 | 本轮 | 后续 |
|---|---|---|
| Android | Vulkan 后端 + `ANativeWindow` + 生命周期；**壳层用 `FrameLayout` + `SurfaceView`(3D) + `ComposeView`(UI) 兄弟节点** | Gradle 工程（`type: as` 模板） |
| 桌面（Windows/Linux） | Vulkan + GLFW（CI、CLI、golden） | — |
| iOS | **只做设计考虑**：surface 抽象不依赖 Android 专属概念；`BackendType::Vulkan` 走 MoltenVK 时`IRenderTarget`/`ISwapchain` 不出现平台宏。**UI 用 CMP，3D 层必须落在 Compose 之下**（CMP 的 `UIKitView` 模型），`Info.plist` 需 `CADisableMinimumFrameDurationOnPhone` | 搭 iOS 工程、验证 MoltenVK |

### 9.1 UI 框架选型（方案一的配套）

| 项 | 决定 |
|---|---|
| UI 框架 | **Compose**（Android 原生）/ **Compose Multiplatform**（iOS）。不自研 UI |
| UI 的内容 | 顶栏、底栏、调色板、工具面板、层数控制 —— **一律是环绕式控件** |
| 3D 之上的浮层 | 由 **SDK 的 `IOverlay2D`** 在 3D 层内画（虚线框、网格、gizmo），**不经过 Compose** |
| 为什么浮层走 SDK | (1) 必须跟手实时更新，不能等 Compose 重组；(2) **iOS 上 3D 层在 Compose 之下**，浮层必须由 3D 层自己画，否则 Z 序无解 |
| 状态更新 | 滑块/选色等高频状态必须用 `derivedStateOf` 收窄失效范围，禁止整树重组 |

## 10. 边界情况与明确不做

| 项 | 决策 |
|---|---|
| 阴影 / 动画 / 骨骼 | 不做 |
| GLES / Metal 后端 | 不做，接口留位（`BackendType` 枚举） |
| 多线程录制 / 多队列 / 异步计算 | 不做（单队列单线程） |
| 模型格式 | 只 OBJ；glTF 后续再议 |
| 捏捏乐 | 不做 |
| **运行时体素网格** | **不做**（2026-11-04 修正）—— 叠叠乐用 `IBlockSet` 方块实例集合；体素化只作为 OBJ→方块的**一次性预处理**（`IVoxelizer`），不进入运行时数据结构 |
| 每帧新建资源 | 允许但计入分配计数门槛（稳态帧必须 0） |
| 纹理图集 / bindless | 本轮不做（后续在 `IMaterial` 层扩展） |
| 动态分辨率 | 不做 |
| **UI 绘制 / UI 合成** | **不做**（方案一）—— UI 归 Compose；合成归系统合成器（SurfaceFlinger / CoreAnimation） |
| **UI 纹理导出** | **不做** —— 那是方案二的做法，已否决（§2.3） |
| **整体后处理（对 UI+3D 统一）** | **不做** —— 方案一无法对 UI 层做后处理；如将来需要，走 §2.3.5 的回滚点 |

## 11. 验收门（实现阶段，对齐 build-pipeline SKILL）

1. **接口纯度**：§7 四个门控脚本零违规。
2. **内存门槛**：稳态帧 VMA alloc/free = 0；页复用与子块合并单测通过；`vkAllocateMemory` 零裸调用。
3. **CLI + 离屏出图**：四个功能各 ≥ 1 张 PNG（无头、无显示服务）。
4. **命令模式**：录制 → 回放 → 复现，命令哈希与 PNG 一致（golden 固定）；序列化往返一致。
5. **单元测试**：图元生成、轮廓挤出（含孔）、OBJ 解析、DDA 拾取、**方块扫掠求交（含撤销整笔）**、`IShapeTemplate` 实例化、gizmo 屏幕恒定尺寸、命令序列化往返、内存分页分配。
6. **单一 golden**：Linux（lavapipe）+ Windows（SwiftShader）逐像素一致，不建分平台基线。
7. **UI 合成门控（方案一）**：UIG1–UIG4 四条零违规；SDK 每帧 submit 次数 = 1；CPU 回读 = 0；
   壳层 3D view 与 Compose view 为兄弟节点；无 `TextureView`。
8. **UI 性能门**：Compose `graphicsLayer` 调用点 ≤ 20；拖动滑块时不重组整棵控件树；
   iOS `Info.plist` 含 `CADisableMinimumFrameDurationOnPhone`。

## 12. 本次改动清单（本轮已落地的部分）

| 文件 | 改动 |
|---|---|
| `third_party/deps.yaml` | 新增 `vma: tag v3.5.0`（头文件库） |
| `tools/deps_lib/cmake_driver.py` | 新增 `HEADER_ONLY_LIBS`：纯头文件库跳过 configure/build/install，由 `_post_install_copy` 拷头进池前缀；新增 `_copy_header_only` |
| `tools/deps_lib/manifest.py` | `LibSpec.source_dir` 字段（纯头文件库拷头用） |
| `tools/build-deps.py` | 编译前用 `pool.src_dir` 填 `LibSpec.source_dir` |

共 4 个文件改动，全部为「三方库池支持纯头文件库」这一件事所需，不触碰任何既有库的构建产物。

已验证：`fetch-deps.py` 拉取 `vma-v3.5.0` 成功；`build-deps.py --variant release` 与 `--variant debug` 均产出
`third_party/_install/vma-v3.5.0/<variant>/include/vk_mem_alloc.h`（`VMA_VERSION == 3.5.0`）。

## 13. 后续实现路线（不在本轮）

按 SDK 依赖顺序落地，每阶段独立计划 + 审阅门 ≥ 80 + 测试门 0 失败：

1. **L0/L1 + Vulkan 后端 + VMA 分页分配器**（含 §7 门控脚本）
2. **L2 场景 / 渲染图 / 命令录制回放**
3. **L3 核心接口**（MeshFactory 图元、ICamera、IPicker、IOverlay2D、IBlockSet/IBlockBrush）
4. **CLI + 离屏 golden + 三态命令校验**
5. **依次组合玩法**：F-SK → F-VX → F-BL → F-ST

## 14. 待确认

| # | 问题 | 状态 |
|---|---|---|
| Q1 | 叠叠乐方块"与网格对齐"的确切含义 | ✅ **已裁定 2026-11-04：选 A** —— 必须贴齐 0,0,0 轴对齐网格，`Block.cell` 为 `int32[3]`，详见 §4.1 与 L3 设计 §6A |
| Q2 | UI 绘制路线 | ✅ **已裁定 2026-11-05：用现成框架（Compose），不自研** —— 见 §1.2、C8 |
| Q3 | UI 与 3D 的合成方式 | ✅ **已裁定 2026-11-05：方案一** —— 引擎输出结果 1、UI 输出结果 2，两者独立、由系统合成器合并；见 §2.3 |
| Q4 | 是否需要「UI 被 3D 遮挡」「UI 作为 3D 纹理」 | ⏸ **已按"不需要"设计**（四个玩法均为环绕式控件）。若将来需要，走 §2.3.5 回滚点 |

> **Q1 裁定（2026-11-04，用户最终定：方案 A = 格点吸附）**：叠叠乐方块**必须贴齐 0,0,0 轴对齐网格**，
> 尺寸统一，位置只能落在整格点上。`Block.cell` 用 **`std::int32_t[3]` 整格坐标**（不是 float）：
> 既省内存，又天然抗浮点漂移（同一次点击永远落到同一格），并且命令流里是整数 → 哈希稳定。
> 换算 `world = origin + cell * cell_size`，`origin`/`cell_size` 是场景常量、进快照。
> **不做** `FreeFloat`（自由浮点放置）与 `IPlacementPolicy` 策略注入 —— 那是多策略抽象，
> 本次按 A 收敛，保持接口最小。`IOverlay2D` 的网格指示直接用 `cell_size`，
> 保证"看到的格子"= "落的格子"。实现细节见
> [L3 primitives 设计](2026-11-04-magicblocks-l3-primitives-design.md) §6A。
>
> 注:`IBlockSet` / `IBlockBrush` / `IShapeTemplate` 属 **L4 玩法层**（主 spec §4 P7a/P7b/P7c），
> 不在 L3 primitives 范围内（用户 2026-11-04 明确：严格按 L3 需求，未列入 L3 的不做）。
