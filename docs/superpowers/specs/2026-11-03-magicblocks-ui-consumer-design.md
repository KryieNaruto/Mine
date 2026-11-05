# 魔法功能预览 —— UI 消费层设计（MagicBlocks UI Consumer）

日期：2026-11-03（2026-11-05 第 2 版：全面对齐**方案一**）
状态：设计第 2 版（用户已确认：UI 用现成框架不自研；合成采用方案一）

> **第 2 版改动摘要**：第 1 版的「自绘即时模式 UI 画进 Vulkan、场景与 UI 同帧同 swapchain image」
> **整条作废**。现版本：**SDK 只渲染 3D，Compose 渲染控件，两路独立输出由系统合成器合成**。
> 依据：[SDK 设计大纲 §2.3](2026-11-03-magicblocks-render-engine-sdk-design.md) §2.3、
> [UI 绘制路线调研](2026-11-05-magicblocks-ui-rendering-route-research.md)。

## 1. 背景与定位

上一轮已定稿 SDK 渲染核心：[渲染引擎 SDK 设计](2026-11-03-magicblocks-render-engine-sdk-design.md)。

本轮设计 **UI 消费层（UI Consumer Layer）**。定位：

> UI 消费层是**之后的几个功能模块（涂鸦 / 积木 / 叠叠乐 / 印章）的唯一入口**。
> 它是 SDK 的**第一个使用者**，展示"SDK 该怎么被消费"，本身不实现任何玩法渲染。
> 它做四件事：**壳层组装、功能路由、输入仲裁、性能治理**。
> 它**不做** UI 控件绘制（归 Compose），**不做**最终合成（归系统合成器）。

### 1.1 用户已确认的约束

| # | 约束 | 本文档落地章节 |
|---|---|---|
| U1 | UI 消费层是后续几个功能模块的**统一入口** | §4 |
| U2 | demo 但**性能优先**，SDK 不是瓶颈，**瓶颈在于"上屏"** | §3、§6 |
| U3 | **一旦拷贝到 CPU 就会大大降低帧率** → 上屏链路必须零 CPU 回读 | §5 |
| U4 | 沿用已有平台/后端约束：Android + iOS（本轮 Android）、RHI + Vulkan、无 GS | §8 |
| **U5** | **UI 不自研，必须用现成框架**（Compose） | §3.2、§8 |
| **U6** | **合成采用方案一**：引擎输出结果 1、UI 输出结果 2，两者独立由系统合成器合并 | §3.2、§5 |

## 2. 分层与职责（第 2 版）

```
┌─────────────────────────────────────────────────────────────┐
│  App Shell（平台原生薄壳）                                     │
│   Android: Activity + FrameLayout                            │
│     ├─ [0] SurfaceView   ← SDK 渲染 3D（输出 1）              │
│     └─ [1] ComposeView   ← Compose 渲染控件（输出 2）          │
│   iOS:     UIViewController + root UIView（本轮不实现）         │
│   职责：窗口创建、surface 生命周期、系统返回键、低内存回调        │
├─────────────────────────────────────────────────────────────┤
│  UI Consumer Layer（本文档）                                   │
│   ├─ Router        功能路由：当前玩法、进入/退出、状态保存        │
│   ├─ InputArbiter  触摸事件仲裁：Compose 命中 还是 3D 场景       │
│   ├─ FeatureBridge SDK 桥接：Surface 生命周期、帧驱动、JNI 输入   │
│   └─ PerfGovernor  帧预算、统计、降级（§6）                     │
│   ※ 没有 HUD 层：控件归 Compose                               │
│   ※ 没有 Presenter 合成层：合成归系统合成器                     │
├─────────────────────────────────────────────────────────────┤
│  MagicBlocks SDK（渲染核心，上一轮定稿）                        │
│   L4 features · L3 primitives · L2 scene · L1 resources · L0 device │
│   backend/vulkan + VMA 分页分配                                │
│   边界：把 3D 场景画进一个 Surface，然后 present               │
└─────────────────────────────────────────────────────────────┘
```

**铁律（第 2 版）**：

1. UI 控件**只由 Compose 绘制**，SDK 与 UI 消费层都不画控件。
2. 3D 视图与 Compose 视图是**兄弟节点**，3D 视图**不得**被 `AndroidView` 包进 Compose 树（UIG3）。
3. **合成发生在 SDK 之外**（SurfaceFlinger），SDK 每帧只提交自己的 3D 场景（UIG2）。
4. UI 消费层只调用 SDK **公共头**（`include/magicblocks/...`），不碰后端类型。

## 3. 上屏瓶颈分析（为什么性能点在这里）

### 3.1 移动端上屏链路（方案一）

```
App（app 进程）
  ├─ [3D 线程] SDK: acquire Surface → 场景 pass → present ─┐
  └─ [UI 线程] Compose: 测量/布局/绘制 → RenderNode → 自绘 ─┤
                                                            ▼
系统合成器（SurfaceFlinger / CoreAnimation）              ← 合成点
  layer[0] = 3D Surface
  layer[1] = Compose Surface（透明背景）
       │
       ▼
    屏幕
```

**关键的认知转变**：第 1 版认为"性能瓶颈在 app 内的合成（场景 pass + UI pass 同一张 image）"。
方案一下，**app 内根本没有合成这一步**——合成在 SurfaceFlinger，由**显示控制器硬件（DPU）**完成，
几乎不占 GPU。所以：

> **方案一把"合成"这个瓶颈从 GPU 上拿走了。** 剩下的瓶颈只有两个：
> ① SDK 的 3D 渲染本身；② Compose 的重组与 GPU 层数。

### 3.2 各条"错误路径"的代价（第 2 版重写）

| 路径 | 链路 | 代价 | 结论 |
|---|---|---|---|
| **A. CPU 回读** | RenderTarget → `vkCmdCopyImageToBuffer` → map → CPU 处理 → 再上传 | 每帧 GPU→CPU 同步 + 带宽 + 迟滞 1–2 帧 | ❌ **硬禁止**（U3） |
| **B. 离屏 + blit 上屏** | 场景 → 离屏 image → copy → swapchain | 一次全屏 copy | ⚠️ 仅 CLI/golden/录制回放使用 |
| **C. 场景直接渲进 swapchain** | 一次 submit，present 给 SurfaceFlinger | 零 copy、零回读 | ✅ **SDK 主路径**（注意：**只有 3D，没有 UI pass**） |
| **D. UI 纹理进引擎 pass**（方案二） | Compose → `AHardwareBuffer` → 引擎采样混合 | 每帧多 2 次全屏带宽 + 帧率强耦合 + 数百行导入代码 | ❌ **已否决**（§2.3） |
| **E. `TextureView`** | 3D 渲染 → 纹理回传 → View 系统绘制 | 多一次 GPU→GPU 拷贝 + 延迟 | ❌ **禁止**（UIG4） |

**关键结论**：SDK 主路径是 **C**，但 **C 的范围缩小了——只画 3D 场景**。
UI 完全不在这张 image 上，也不在 SDK 的任何 pass 上。

### 3.3 延迟账（方案一）

| 操作 | 路径 | 延迟 |
|---|---|---|
| 拖动模型 / 画笔 | 触摸 → SDK InputRouter → 下一帧 3D present | **1 帧**（不受 Compose 影响） |
| 点按钮 | 触摸 → Compose 重组 → Compose present | **1 帧**（不受 SDK 影响） |
| 3D 与 UI 互相等待 | — | **无**（两条独立管线） |

> 这是方案一相对第 1 版设计最大的收益：**两条管线彻底解耦**，
> 任意一方慢都不会拖累另一方。

## 4. 功能模块入口（Router）

功能切换只经过 Router，功能模块不互相感知。

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

**UI 与 Router 的关系（第 2 版）**：Router 的状态（当前玩法、是否可撤销、工具参数）
通过 **Compose 的状态对象**暴露给控件层。这意味着一份 `FeatureUiState`：

- `FeatureHost` 写入（场景侧）；
- Compose 读取并渲染控件；
- **跨线程边界只传不可变快照**，不允许把 SDK 对象直接塞进 Compose 状态。

## 5. 输入仲裁（InputArbiter，取代第 1 版的 InputRouter）

### 5.1 仲裁模型

触摸事件必须**在平台层就分流**，因为 Compose 和 SurfaceView 是两个独立窗口。

```
MotionEvent 到 FrameLayout.dispatchTouchEvent
   │
   ├─ ① 命中 Compose 交互区？（按钮 / 滑块 / 色板 / 面板）
   │      是 ──► 交给 ComposeView，事件结束
   │
   └─ ② 否则 ──► 交给 SurfaceView
                  └─ 触摸坐标（减去 SurfaceView 偏移）→ JNI → SDK InputEvent
```

**为什么必须在这里分流**：Compose 有自己完整的命中测试与手势系统（触控 slop、嵌套滚动、长按）。
如果让 SDK 抢在前面处理，Compose 的按钮就点不动；如果让 Compose 处理全部，
SDK 就拿不到绘制手势。**两个系统各有各的手势语义，只能在边界上做一个明确的裁决。**

### 5.2 分流规则

| 规则 | 说明 |
|---|---|
| **R-1 显式区域优先** | Compose 侧控件的**实际布局矩形**是唯一的判定依据，不用"猜" |
| **R-2 默认归 3D** | 空白区域一律归 3D 场景，保证"所见即所画" |
| **R-3 一次手势不跨界** | `DOWN` 落在哪边，整个手势就归哪边直到 `UP`（避免画到一半被面板抢走） |
| **R-4 手势识别留在 SDK** | 单指拖 / 双指缩 / 长按拾取由 SDK 的 `InputEvent` 统一识别，**不在每个玩法里重复** |
| **R-5 事件双写命令流** | 原始事件同时喂 `InputEvent` 队列，供命令复现（SDK §5）回放 |

### 5.3 禁止事项（交互层硬约束）

| 禁止 | 原因 |
|---|---|
| 让 SDK 处理本该 Compose 消费的事件 | 按钮点不动 |
| 用"屏幕上半屏归 UI"这类区域猜测 | 面板布局会变，必然错位 |
| 让 Compose 的长按/双击跨到 3D 区 | 手势语义冲突 |
| 每帧轮询 UI 状态决定事件归属 | 状态滞后一帧 |

> **注意**：`R-1` 的"实际布局矩形"由 Compose 通过回调上报（`onGloballyPositioned`）
> 或由壳层直接 `View.getGlobalVisibleRect` 取，**不缓存跨帧**。

## 6. 性能治理（PerfGovernor）

方案一下，性能治理分成**两条互不干扰的轨道**：

### 6.1 3D 轨道（SDK 侧）

| # | 机制 | 说明 |
|---|---|---|
| G1 | **帧预算** | 3D 帧预算独立计算；超预算记录并触发降级 |
| G2 | **降级档位**（明确，不静默） | L0 全质量 → L1 关 MSAA → L2 降低特效 → L3 降低离屏分辨率 |
| G3 | **drawcall 上限** | 单场景 ≤ 300 drawcall（对齐 SDK §8） |
| G4 | **稳态零分配** | 稳态帧 VMA alloc/free = 0（SDK M5 门槛） |
| G5 | **切后台** | surface 销毁时暂停 present，保留 CPU 侧状态，避免唤醒时黑屏 |
| G6 | **3D 浮层走 SDK** | 虚线选框 / 网格 / gizmo 由 `IOverlay2D` 在 3D 层内画，**不经过 Compose**（§8.2） |

### 6.2 UI 轨道（Compose 侧）

| # | 机制 | 说明 |
|---|---|---|
| U1 | **状态收窄** | 高频状态（笔刷粗细、选色、层数）必须用 `derivedStateOf` 收窄失效范围，**禁止整树重组** |
| U2 | **GPU 层数上限** | `graphicsLayer` 调用点 **≤ 20**（`RenderNode` 过多会打断 GPU 合批） |
| U3 | **禁止 `Modifier.blur`** | 移动端开销极高 |
| U4 | **禁止每帧改文本** | 帧率显示等高频数字必须限频到 1Hz |
| U5 | **图标用位图** | `ImageVector` 每帧重放路径，改用 `painterResource` |
| U6 | **不做大面积阴影** | `Modifier.shadow` 产生额外层 + 模糊 |

### 6.3 统计面板（调试用）

帧时间、GPU 时间（能力支持时）、drawcall、VMA 分配计数、present 队列长度、
**以及 Compose 侧的重组次数与层数**。两条轨道的数据**分开显示**，便于定位是哪边慢。

## 7. 帧驱动模型（第 2 版新增，第 1 版没有）

方案一下有**两条独立的帧循环**，必须说清楚谁驱动谁：

```
[3D 帧循环]  SurfaceView 的回调 / Choreographer 驱动
   onFrame → SDK::RenderFrame() → present
   ※ 自由运行，不受 Compose 影响

[Compose 帧循环]  Compose 自己的 recomposer + Choreographer
   state 变 → 重组 → 测量布局 → 绘制
   ※ 自由运行，不受 SDK 影响

二者仅通过 FeatureUiState（不可变快照）通信，单向：
   FeatureHost --写--> FeatureUiState --读--> Compose
```

| 问题 | 答案 |
|---|---|
| 3D 帧率由谁决定？ | SDK 自己（配合 `Choreographer` 或 surface 帧回调） |
| Compose 帧率由谁决定？ | Compose 的 recomposer，**只在状态变时重绘** |
| 二者需要同帧吗？ | **不需要**。这正是方案一的意义 |
| 3D 需要读 Compose 状态吗？ | 需要（如当前工具），但**只读不可变快照**，且不阻塞 3D 帧 |
| 会不会撕裂？ | 不会。两个独立 Surface 由系统合成器同步到同一个 vsync |

> **第 1 版的做法是"UI 与场景同帧同 image"**（§5.2 的帧结构里显式列了 `[Pass 2 UI]`）。
> 方案一下这段**整段删除**：帧结构只剩 `[Pass 1 Scene]` → `Present`。

## 8. 平台与 UI 框架选型（第 2 版重写）

### 8.1 选型表

| 项 | 第 1 版（作废） | **第 2 版（采纳）** | 理由 |
|---|---|---|---|
| UI 绘制方式 | 自绘即时模式 UI 画进 Vulkan | **Compose 原生控件** | 用户明确：不自研 UI |
| UI 框架 | 自绘最小控件集 | **Jetpack Compose**（Android）/ **CMP**（iOS） | 平台官方、生态成熟 |
| 3D 承载 | UI pass 渲进 swapchain image | **SurfaceView**（独立 Surface） | 系统合成器直接合成，零额外拷贝 |
| 图片文字 | 字体图集 SDF | **Compose 自带的文本/图标系统** | 不用自己管字形 |
| 合成 | app 内合成 | **SurfaceFlinger / CoreAnimation** | 硬件合成，几乎不占 GPU |
| Android 壳 | NativeActivity / 自定义 SurfaceView + JNI | **Activity + FrameLayout + SurfaceView + ComposeView** | 两个兄弟节点 |
| iOS | UI 层不依赖平台控件 | **CMP + 原生 3D 视图**（3D 在 Compose 之下） | 见 §8.2 |

### 8.2 3D 之上的浮层必须由 SDK 画（关键设计结论）

| 浮层 | 归属 | 为什么 |
|---|---|---|
| 选中包围盒虚线 / gizmo | **SDK `IOverlay2D`** | 跟手实时更新，不能等 Compose 重组 |
| 叠叠乐网格指示 | **SDK `IOverlay2D`** | 必须与方块落点严格对齐，同一坐标系 |
| 顶栏 / 底栏 / 调色板 / 面板 | **Compose** | 静态控件，不需要跟手 |
| 弹窗 / 提示 / Toast | **Compose** | 平台原生交互 |

**iOS 的特殊约束**：CMP 的 `UIKitView` 把原生视图**插到 Compose 渲染视图之下**
（`root.insertSubview(container, 0)` + `BlendMode.DstAtop` 挖洞）。所以 **iOS 上 3D 层永远在 Compose 之下**。
如果虚线选框用 Compose 画，它会被自己的 3D 场景盖住——**必须由 3D 层自己画**。
这也是 §8.2 表格中浮层归属在双端统一的原因。

`Info.plist` 必须包含 `CADisableMinimumFrameDurationOnPhone`，否则 CMP 在 iOS 上**运行时会崩溃**。

## 9. 边界与明确不做（UI 层）

| 项 | 决策 |
|---|---|
| **UI 控件绘制** | **不做** —— 归 Compose |
| **UI 最终合成** | **不做** —— 归系统合成器 |
| **UI 纹理导出 / 进引擎 pass** | **不做** —— 方案二，已否决 |
| 动画/转场 | 用 Compose 内置动画（不自己写） |
| 多窗口/多 surface | 不做 |
| 系统键盘输入 | 不做（只做触摸） |
| 无障碍 | 用 Compose 自带无障碍（不自建） |
| UI 主题/换肤 | 不做 |
| CPU 回读 | **硬禁止**，见 §3.2 |

## 10. 验收门（实现阶段）

| # | 验收项 | 判据 |
|---|---|---|
| 1 | **上屏零回读** | `check_ui_no_readback.py` 通过，运行时计数 = 0 |
| 2 | **SDK 单 submit** | `check_ui_single_submit.py` 通过；SDK 每帧 submit = 1 |
| 3 | **3D 视图不在 Compose 树内** | 壳层评审 + 布局 dump：`SurfaceView` 是 `ComposeView` 的兄弟 |
| 4 | **无 `TextureView`** | 壳层静态扫描为 0 |
| 5 | **Compose 层数** | `graphicsLayer` 调用点 ≤ 20 |
| 6 | **重组收窄** | 拖动滑块时不重组整棵控件树（Layout Inspector 验证） |
| 7 | **延迟** | 3D 交互 `input→present` ≤ 2 帧；UI 点击 ≤ 2 帧（两条轨道分别测） |
| 8 | **输入仲裁** | `DOWN` 落在 Compose 区时 3D 收不到事件；落在 3D 区时 Compose 收不到事件 |
| 9 | **稳态零分配** | SDK VMA alloc/free = 0 |
| 10 | **离屏 golden** | HomeScreen + 空 FeatureHost 的 3D 离屏截图进 golden；录制→回放→复现哈希一致 |
| 11 | **iOS 配置** | `Info.plist` 含 `CADisableMinimumFrameDurationOnPhone` |
| 12 | **既有门控** | 无 GS、分层纯度、内存路径、R1 全部继续绿 |

## 11. 后续实现路线（本轮不做）

1. **Android 壳骨架**：`Activity` + `FrameLayout` + `SurfaceView` + `ComposeView` 兄弟节点，
   验证 SDK 能渲进 Surface、Compose 能叠在上面、触摸仲裁正确
2. **Router + FeatureHost 骨架**（空特性）+ `FeatureUiState` 单向数据流
3. **输入仲裁**（InputArbiter）+ JNI 事件桥
4. **PerfGovernor 双轨统计**（3D / Compose 分开）
5. 再按玩法进入：涂鸦 → 叠叠乐 → 积木 → 印章（各自只填 FeatureHost 内容 + Compose 控件）

## 12. 与第 1 版的差异清单（便于审阅）

| 第 1 版内容 | 第 2 版处理 |
|---|---|
| §2 `HUD` 层 "界面控件（按钮/面板/调色/层数/工具切换）" | ❌ 删除 —— 归 Compose |
| §2 `Presenter` "上屏链路（本文档核心）" | ❌ 删除 —— 合成归系统合成器；保留 `FeatureBridge` 做 Surface 生命周期 |
| §2 铁律 "同一帧、同一 CommandList、同一次 submit" | ❌ 作废 —— 改为"3D 与 UI 两条独立管线" |
| §3 方案 C "场景 pass + UI pass 都渲染到同一张 swapchain image" | ❌ 作废 —— 只剩场景 pass |
| §5 Presenter 整章（帧结构含 `[Pass 2 UI]`、UI pass 作为 render graph 最后一项） | ❌ 整章删除 —— 见 §7 帧驱动模型 |
| §6 G3 "UI 合批"、G4 "字体/图集"、G5 "立即模式 UI 零分配" | ❌ 删除 —— 都是自绘 UI 的产物 |
| §7 InputRouter（HUD 命中 → 工具 → 相机三级） | 🔄 重写为 §5 InputArbiter（Compose 区 / 3D 区二分） |
| §8 "UI 绘制方式：即时模式画进 Vulkan；自绘最小控件集" | 🔄 重写为 §8（Compose + SurfaceView） |
| §10 验收门 5 条 | 🔄 扩展为 §10 的 12 条 |
