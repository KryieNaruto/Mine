# 魔法功能预览 —— UI 消费层设计（MagicBlocks UI Consumer）

日期：2026-11-03
状态：设计第 1 版（用户已确认方向：UI 消费层是后续所有功能模块的**唯一入口**；demo 但**性能优先**）

## 1. 背景与定位

上一轮已定稿 SDK 渲染核心：[渲染引擎 SDK 设计](2026-11-03-magicblocks-render-engine-sdk-design.md)。

本轮设计 **UI 消费层（UI Consumer Layer）**。定位明确：

> UI 消费层是**之后的几个功能模块（涂鸦 / 积木 / 叠叠乐 / 印章）的唯一入口**。
> 它是 SDK 的**第一个使用者**，展示"SDK 该怎么被消费"，本身不实现任何玩法渲染，
> 只做：布局、输入、功能路由、性能治理。

### 1.1 用户已确认的约束

| # | 约束 | 本文档落地章节 |
|---|---|---|
| U1 | UI 消费层是后续几个功能模块的**统一入口** | §2、§4 |
| U2 | demo 但**性能优先**，SDK 不是瓶颈，**瓶颈在于"上屏"** | §3、§5、§6 |
| U3 | **一旦拷贝到 CPU 就会大大降低帧率** → 上屏链路必须零 CPU 回读 | §5 |
| U4 | 沿用已有平台/后端约束：Android + iOS（本轮 Android）、RHI + Vulkan、无 GS | §7 |

## 2. 分层与职责

```
┌─────────────────────────────────────────────────────────────┐
│  App Shell（平台薄壳：Activity / NativeActivity / 桌面 GLFW）    │
│  职责：窗口创建、surface 生命周期、系统返回键、低内存回调          │
├─────────────────────────────────────────────────────────────┤
│  UI Consumer Layer（本文档）                                   │
│   ├─ Router        功能路由：当前玩法、进入/退出、状态保存          │
│   ├─ HUD           界面控件（按钮/面板/调色/层数/工具切换）          │
│   ├─ InputRouter   触摸事件 → 相机 / 工具 / UI 命中，优先级仲裁      │
│   ├─ Presenter     上屏链路（本文档核心，§5）                     │
│   └─ PerfGovernor  帧预算、降级、统计（§6）                       │
├─────────────────────────────────────────────────────────────┤
│  MagicBlocks SDK（渲染核心，上一轮定稿）                          │
│   L4 features · L3 primitives · L2 scene · L1 resources · L0 device │
│   backend/vulkan + VMA 分页分配                                 │
└─────────────────────────────────────────────────────────────┘
```

**铁律**：UI 消费层只调用 SDK **公共头**（`include/magicblocks/...`），与 SDK 内部共享
"同一帧、同一 CommandList、同一次 submit"，**绝不另开第二条上屏路径**。

## 3. 上屏瓶颈分析（为什么性能点在这里）

"上屏"= 一帧图像从 GPU 合成完成 → 呈现到屏幕。移动端链路：

```
[场景渲染] ─► [UI 合成] ─► [上屏 present]
   场景 pass     HUD 绘制      swapchain image
```

三种可能的实现方式与代价：

| 方案 | 链路 | 代价 | 结论 |
|---|---|---|---|
| **A. CPU 回读再贴**（典型错误做法） | RenderTarget → `vkCmdCopyImageToBuffer` → map → CPU 修改像素 / CPU 画 UI → 再上传 / 再画 | **每帧一次 GPU→CPU 同步 + 带宽 + 迟滞一到两帧**，帧率断崖 | ❌ 禁止 |
| **B. 离屏 target + blit 上屏** | 场景 → 离屏 image → blit/copy → swapchain image | 一次全屏 copy，无 CPU 往返；带宽可控 | ⚠️ 仅在需要"离屏语义"（golden/录制）时使用 |
| **C. 直接渲染进 swapchain image**（推荐） | 场景 pass + UI pass 都渲染到同一张 swapchain image | 零 copy、零 CPU 往返、延迟最低 | ✅ 主路径 |

**关键结论**：主路径必须是 **C —— 场景与 UI 同帧直接渲染进 swapchain image**。
离屏（方案 B）只用于 CI/golden/命令录制回放这类"不需要上屏"的场景。

> 这直接推翻"先离屏、再贴图"作为运行时主路径的思路：那个范式在桌面 CI 里是正确的，
> 但作为**上屏主路径**会多一次全屏 copy 和一次额外同步。运行时要区分
> `TargetMode::Swapchain`（上屏）与 `TargetMode::Offscreen`（测试/录制）。

## 4. 功能模块入口（Router）

UI 消费层提供**唯一入口**——功能切换只经过 Router，功能模块不互相感知。

```
HomeScreen ──(选择玩法)──► FeatureHost(Sketch | Blocks | Voxel | Stamp)
     ▲                              │
     └──────────(返回/退出)──────────┘
```

| 接口 | 职责 |
|---|---|
| `IRouter` | `Push(feature_id)` / `Pop()` / `Current()`；进入/退出时调用特性层的 `OnEnter/OnExit`（挂载/卸载到 `IRenderFeature` 注册表） |
| `IFeatureHost` | 特性生命周期：`OnEnter / OnExit / OnUpdate(dt) / OnInput(InputEvent)`；只依赖 SDK 的 `IScene`、`IInputEvent`、`IRenderFeature` |
| `IStateStore` | 退出时保存场景状态（节点树、工具参数、相机位姿），重进恢复；**存内存为主，落盘异步** |

本轮只建立 **HomeScreen 入口 + 空 FeatureHost 骨架**（四个玩法仍是 SDK 层之后的组合阶段），
但**入口结构、输入路由、上屏链路必须先定型**，否则后面每个玩法都要各自复制一套上屏逻辑。

## 5. Presenter —— 上屏链路设计（本文档核心）

### 5.1 目标

| # | 目标 |
|---|---|
| P1 | **零 CPU 回读**：每帧不得出现任何 `vkCmdCopyImageToBuffer` / `map` 读回 swapchain 或离屏图像 |
| P2 | **零额外全屏 copy**：主路径场景 + UI 直接渲染进 swapchain image |
| P3 | **单次 submit**：场景 pass、UI pass、上屏都在同一 CommandBuffer 内 |
| P4 | **低延迟**：present mode 可切 MAILBOX（无垂直同步等待），默认 FIFO 保稳 |
| P5 | **不卡 UI**：UI 线程不做 GPU 同步等待，帧栅栏最多滞后 N=2 帧 |

### 5.2 帧结构（主路径）

```
Frame n:
 1. Wait Fence(n)               ← 复用两帧前资源，不阻塞当前
 2. Acquire swapchain image
 3. Record into cmd(n):
      [Pass 1  Scene]  渲染各玩法内容（深度测试开，清深度）
      [Pass 2  UI]     HUD + 覆盖层，深度测试关，加载/清除后叠加
      （End rendering → image layout PRESENT）
 4. Submit(cmd) + signal semaphore
 5. Present(image, semaphore)
 6. Advance 帧索引（N=2 ring）
```

**UI 与场景同帧同 image** 意味着：UI 不会因为"离屏再合成"而额外滞后一帧，
点击 → 反馈的延迟 = 1 次 submit + 1 次 present。

### 5.3 渲染图融合

SDK 的 `IRenderGraph` 支持把 **UI pass 作为图的最后一个 pass**，附件 = swapchain image（`LoadOp::Load`），
这样：

- 图裁剪可自动去掉被遮挡的 pass；
- 命令录制（§上一轮的 `CommandStream`）天然把"场景 + UI"一起录进同一帧，复现时 UI 也在；
- 不新增 SDK 层的东西，UI pass 只是 `IRenderFeature` 的又一个注册项。

### 5.4 禁止事项（渲染层硬约束）

| 禁止 | 原因 |
|---|---|
| CPU 读回任何渲染结果用于 UI | 用户明确指出的帧率杀手 |
| `vkQueueWaitIdle` / `vkDeviceWaitIdle` 每帧调用 | 直接串行化 GPU/CPU |
| `map()` 非 coherent 内存每帧读写 | 带宽与同步开销 |
| staging buffer 参与上屏路径 | 只在资源上传时使用 |
| 每帧创建/销毁 swapchain 或 render pass | 走缓存，旋转时才重建 |
| UI 单独一次 submit | 额外同步点 |

### 5.5 离屏路径（测试/录制专用）

`TargetMode::Offscreen`：场景 + UI 渲染到离屏 image → `save_png`。
仅用于 CLI/CI/golden/命令回放，**上屏路径不经过它**。
两者共用同一 `IRenderTarget` 抽象，只是目标不同；`Presenter` 内部按 mode 分流。

## 6. 性能治理（PerfGovernor）

| # | 机制 | 说明 |
|---|---|---|
| G1 | **帧预算** | 每帧预算 16.6ms（60fps），分桶：场景渲染 / UI / present；超预算记录 |
| G2 | **降级策略**（明确档位，不静默） | L0 全质量 → L1 关 MSAA → L2 UI 简化（去阴影/去动画）→ L3 降低场景分辨率（仅离屏路径可做） |
| G3 | **UI 合批** | HUD 图元按纹理/管线合批；每帧 UI drawcall 上限（如 ≤ 50）与顶点上限 |
| G4 | **字体/图集** | 字体图集一次上传、静态；不每帧重传 |
| G5 | **立即模式 UI 的零分配** | 每帧 UI 顶点/索引写入预分配的大 buffer（VMA 分页偏移），**稳态帧 VMA alloc/free = 0**（与 SDK §6 M5 门槛一致） |
| G6 | **输入到显示延迟** | 统计并暴露 `input→present` 帧数（目标 ≤ 2 帧） |
| G7 | **切后台** | surface 销毁时暂停 present，保留 CPU 侧状态，避免唤醒时黑屏 |
| G8 | **统计面板** | 帧时间、GPU 时间（能力支持时）、drawcall、VMA 分配计数、present 队列长度 |

## 7. 输入路由（InputRouter）

同一批触摸事件要分给三处，必须显式仲裁顺序：

```
TouchEvent
   │
   ├─ ① HUD 命中？（按钮/面板）── 是 ─► 消费，不再下发
   ├─ ② 工具激活？（笔刷画/盖章）── 是 ─► 交给当前特性
   └─ ③ 相机/场景交互（旋转/缩放/拾取）── 交给 ICamera / IPicker
```

- 手势识别（单指拖 / 双指缩 / 长按拾取）在 UI 层统一做，**不在每个玩法里重复**；
- 所有事件同时喂给 `InputEvent` 队列，供命令复现（上一轮 §5）回放；
- **输入到命令的一致性**：记录的是"已解析的手势结果 + 原始事件"，复现时优先用手势结果。

## 8. 平台与 UI 框架选型

| 项 | 决策 | 理由 |
|---|---|---|
| UI 绘制方式 | **即时模式 UI 直接画进 Vulkan**（不走平台原生控件） | 原生控件会引入一次系统合成层，等于额外一次上屏合成；且 Android/iOS 两套 |
| UI 框架 | 自绘最小控件集（按钮/面板/滑条/色板/文字）或引入轻量即时模式库 | demo 范围小，先自绘最小集；渲染接口统一为 `IOverlay2D` + 文本渲染，未来可替换 |
| 文字 | 字体图集（SDF 或位图），一次上传 | 避免每帧字形上传 |
| Android 壳 | NativeActivity 或自定义 SurfaceView + JNI | 桌面 GLFW 壳复用同一 UI 层 |
| iOS | 只做设计考虑：UI 层不依赖平台控件，surface 抽象预留 | 后续 |

## 9. 边界与明确不做（UI 层）

| 项 | 决策 |
|---|---|
| 动画/转场 | 不做（demo） |
| 多窗口/多 surface | 不做 |
| 系统键盘输入 | 不做（只做触摸） |
| 无障碍 | 不做（demo），但控件命中区域保留 |
| UI 主题/换肤 | 不做 |
| CPU 回读 | **硬禁止**，见 §5.4 |

## 10. 验收门（实现阶段）

1. **上屏零回读**：门控脚本扫描 UI 层与 Presenter，出现 `CopyImageToBuffer` / `map()` 于呈递路径 → 失败。
2. **单 submit 每帧**：统计断言每帧 submit 次数 = 1（主路径）。
3. **延迟**：`input→present` ≤ 2 帧（统计桩断言）。
4. **稳态帧零分配**：UI 顶点缓冲复用，VMA alloc/free = 0（与 SDK M5 一致）。
5. **离屏 golden**：HomeScreen + 空 FeatureHost 的离屏截图进 golden；命令录制 → 回放 → 复现哈希一致。
6. 既有 SDK 门控（无 GS、分层纯度、内存路径）继续全绿。

## 11. 后续实现路线（本轮不做）

1. HomeScreen + Router + FeatureHost 骨架（空特性）
2. HUD 最小控件集 + 文本图集 + `IOverlay2D` 合批
3. Presenter 主路径（C 方案：直接渲进 swapchain）+ 离屏分流
4. PerfGovernor 统计与降级档
5. 再按玩法进入：涂鸦 → 叠叠乐 → 积木 → 印章（各自只填 FeatureHost 内容）
