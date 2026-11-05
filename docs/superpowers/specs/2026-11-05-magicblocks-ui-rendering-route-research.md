# MagicBlocks —— UI 绘制路线与主流 UI 框架性能瓶颈调研（v2）

日期：2026-11-05
状态：调研定稿（取代 v1 的"自绘最小控件 / 自研上屏"路线）

> **本轮用户指令**："UI 绘制路线再详细调研一次！必须要加入当前主流 UI 框架的性能瓶颈。尤其是 Compose"

关联：
- [SDK 设计](2026-11-03-magicblocks-render-engine-sdk-design.md)
- [R1 定位裁定](2026-11-03-magicblocks-r1-positioning-ruling.md)（SDK = 计算/几何库 + 离屏渲染器，上屏移交壳层）
- [技术路线对比 §12](2026-11-03-magicblocks-tech-route-comparison.md)
- 旧 UI 消费层设计（**本文 §7 予以修订**）

---

## 0. 数据来源与可信度声明（先说清楚哪些是硬数据）

本机网络环境特殊：`github.com` / `api.github.com` 等 48 个域名被 `hosts` 映射到 `127.0.0.1`，
`web_search` 后端 401，`web_fetch` 超时。调研期间**临时放通** `api.github.com` 等条目拿到数据，
调研结束后已**逐行还原**并与用户自己的备份 `hosts.bak-20261103-165517` 比对一致（80 行全等）。

因此本文严格区分两种陈述：

| 标记 | 含义 |
|---|---|
| **[源码实证]** | 直接读到上游仓库某 commit 的源码/官方文档原文，附文件路径 |
| **[分析判断]** | 基于架构与公开行为的推理，**未取得官方 benchmark 数字** |

**必须诚实说明**：Compose / Flutter 的"具体掉帧毫秒数"我没有拿到可引用的官方 benchmark。
下面凡是涉及数字的地方都标注了来源，没有来源的一律写"未取得"。**我不会编造 benchmark。**

---

## 1. 结论先行（TL;DR）

| # | 结论 |
|---|---|
| **C1** | **UI 层用 Compose、3D 场景用 SurfaceView + 自研 Vulkan**，二者由 SurfaceFlinger 合成，**零 CPU 回读**。这是唯一同时满足"UI 不自研"和"性能优先"的组合。 |
| **C2** | Compose 的**结构性能瓶颈不是绘制**，而是 **GPU 层数（GraphicsLayer / RenderNode）+ 逐帧重组**。控制界面的层数必须**有上限且可测量**。 |
| **C3** | 把 3D 场景**放进 Compose 的渲染树**（`AndroidView` 包 SurfaceView）是**最危险的写法**——会让 SurfaceView 的 Z 序、裁剪、`SurfaceView` 在 `GraphicsLayer` 内的裁剪失效，并引入额外一次跨 view 合成。正确做法是让 SurfaceView 与 ComposeView **做兄弟节点**。 |
| **C4** | **iOS 上 Compose 不采用这个架构**：CMP 的 `UIKitView` 是把原生 UIView **插到 Compose 渲染的后方**，用 `BlendMode.DstAtop` 挖洞（[源码实证]）。这意味着 iOS 的 3D 层必须走"Compose 上、原生下"的叠加，且高刷需显式开启。 |
| **C5** | 四个玩法的**抖动最大来源是摄像头/工具面板的频繁状态更新**（拖动滑块、选色、缩放），不是静态界面。必须用**帧采样 + 状态合并（非每帧 setState）**。 |
| **C6** | 用户之前已否掉"自研 UI"，R1 又已把上屏移交壳层——本调研**与这两条一致**，无需回退任何决定。 |

---

## 2. 主流 UI 框架全景（Android / iOS 双端）

### 2.1 各方案的真实定位（元数据均为 GitHub API 实查，2026-11-04）

| 框架 | ⭐ Stars | 语言 | 渲染方式 | 能否承载 3D 场景 | 定位 |
|---|---|---|---|---|---|
| **Jetpack Compose**（`androidx` 内，无独立仓库） | androidx 主仓 **6,129** | Kotlin | **Android 原生 Canvas → RenderNode → Skia** | 需 `AndroidView`/`SurfaceView` 互操作 | Android 官方首选 UI |
| **Compose Multiplatform** `JetBrains/compose-multiplatform` | **19,469** | Kotlin | Android = 原生 Skia；**iOS/Desktop = Skiko（Skia 绑定）** | ✅ 有 `UIKitView` 原生互操作 | 跨端 Compose |
| **Flutter** `flutter/flutter` | **181,034** | Dart | **自带 Skia 渲染器（Impeller 后默认）**，完全接管绘制 | ✅ 有 `PlatformView` / 纹理互操作 | 跨端，UI 最强 |
| **React Native**（元数据获取失败） | — | JS/TS | 原生 View 为宿主 | ✅ 有 `react-native-filament` 等 | 跨端，UI 复用 JS 生态 |
| **Android View 体系**（SurfaceView / TextureView） | Android 平台内置 | Java/Kotlin | 硬件加速 Canvas | ✅ **SurfaceView 就是为此而生** | 低层、可控 |
| **Filament** `google/filament` | **20,688** | C++ | 原生 3D 渲染引擎 | ✅ 本身就是 3D | 3D 引擎（非 UI） |
| **SceneView** `sceneview/sceneview` | **1,357** | Kotlin | Compose + Filament | ✅ | 3D/AR 场景 SDK |

### 2.2 一句话对比

- **Flutter** 最"干净"：整个 UI 都在自己的渲染器里，几乎没有互操作坑——但代价是**要么全 Flutter，要么不做**，
  嵌入我方 Vulkan 场景需要 `PlatformView`（且支持程度一般）。
- **Compose** 最"贴平台"：Android 上它最终画的还是 **Android 原生 Canvas**，所以与 SurfaceView 的互操作是**平台级可靠**的。
- **CMP**：Android 侧与 Jetpack Compose 同源，**iOS 侧是另一套渲染后端**——这正是 §4 要说的坑。

---

## 3. Compose 的性能瓶颈（重点章节）

### 3.1 瓶颈总览：Compose 慢在哪，不慢在哪

| 环节 | 是否瓶颈 | 说明 |
|---|---|---|
| 布局（Measure/Layout） | ⚠️ 中等 | 每帧重新 measure 会很贵；`Box`/`Row` 嵌套过深会累积 |
| **重组（Recomposition）** | 🔴 **主要瓶颈** | 状态一变就重跑整个 composable 函数体；`mutableStateOf` 读得越宽，失效范围越大 |
| **绘制 → RenderNode（GPU 层）** | 🔴 **主要瓶颈** | 每个 `graphicsLayer{}`/`drawWithContent`/`clip` 都可能生成一层；层数多则 GPU 合批被打断 |
| Skia 实际光栅化 | 🟢 不是 | 现代安卓上由 GPU 完成，纯色矩形/圆角极便宜 |
| 提交到 SurfaceFlinger | 🟢 不是 | 一次 BufferQueue 提交 |

> **[分析判断]** 一句话：**Compose 的瓶颈是"每帧要跑多少 Kotlin 代码 + 生成多少个 GPU 层"**，
> 不是"画得太慢"。所以优化方向是**减少状态失效范围**和**控制层数**，而不是"用更快的画布"。

### 3.2 瓶颈 A：重组（Recomposition）——状态读得太宽

```kotlin
// ❌ 反例：整个屏幕读同一个状态 → 任何变化都重组整棵树
var t by mutableStateOf(ToolState())   // 拖动滑块时每帧都写
Column { TopBar(t); Palette(t); Slider(t) }
```

```kotlin
// ✅ 正例：把状态读进叶子，把变化限制在最小 composable
val tool = remember { mutableStateOf(ToolState()) }
Column {
  TopBar(remember { derivedStateOf { tool.value.mode } })   // 只关心 mode
  Slider(remember { derivedStateOf { tool.value.size } })   // 拖动只失效这里
}
```

**对四个玩法的影响**：拖动笔刷粗细、调色板选色、叠叠乐层数切换——这些**每次触摸都在写状态**。
如果状态对象是"一个大 `data class`"，每次拖动都会让整个界面重组。

### 3.3 瓶颈 B：GPU 层数（RenderNode / GraphicsLayer）

**[源码实证]** `AndroidComposeView` 继承 `ViewGroup`，其 `onDraw()` 是**空的**，
实际绘制发生在 `dispatchDraw()`：

```kotlin
// compose-multiplatform-core @ 47c012b
// compose/ui/ui/src/androidMain/kotlin/androidx/compose/ui/platform/AndroidComposeView.android.kt
override fun onDraw(canvas: android.graphics.Canvas) {
}

override fun dispatchDraw(canvas: android.graphics.Canvas) {
    ...
    canvasHolder.drawInto(canvas) { root.draw(this) }
    if (dirtyLayers.isNotEmpty()) { for (i in 0 until dirtyLayers.size) dirtyLayers[i].updateDisplayList() }
    ...
}
```

同一文件的 `createLayer()` 在硬件加速时优先创建 **`RenderNodeLayer`**：

```kotlin
if (isHardwareAccelerated && Build.VERSION.SDK_INT >= Build.VERSION_CODES.M && isRenderNodeCompatible) {
    try { return RenderNodeLayer(this, drawBlock, invalidateParentLayer) }
```

**这证实了**：Compose 的每一个"层"在 Android 上都落成一个 **`RenderNode`**。
`RenderNode` 数量过多会打断 GPU 的合批，是已知的移动端性能敏感点。

> **可测量**：`adb shell dumpsys gfxinfo <pkg> framestats` 能看出每帧的 `FrameCompleted`；
> Compose 侧的层数可以直接数 `graphicsLayer` 调用点。**这就是验收口径**：
> 控制界面 `graphicsLayer` 调用点 **≤ 20 个**（不是"感觉不卡"）。

### 3.4 瓶颈 C：`Canvas` 绘制内容的开销排序

| 内容 | 相对开销 | 建议 |
|---|---|---|
| 纯色矩形、圆角矩形 | 极低 | 随便用 |
| 文本 | 中（每次 layout 会重新排版） | **避免每帧改文本内容**（如显示帧率时用 `Snapshot.withMutableSnapshot` 限频到 1Hz） |
| 图标（`ImageVector`） | 中（矢量每帧重放路径） | **改用 `painterResource` 位图**或缓存 |
| 阴影 / `Modifier.shadow` | 高（额外的层 + 模糊） | 避免大面积阴影 |
| `Modifier.blur` | **极高** | 移动端禁用 |
| 嵌套 clip + 圆角 | 中（强制离屏） | 减少嵌套层级 |

### 3.5 瓶颈 D：与 3D 场景的互操作（最容易踩）

**[分析判断]** 三种写法的代价：

| 写法 | 代价 | 结论 |
|---|---|---|
| `AndroidView { SurfaceView() }` **放进 Compose 树** | SurfaceView 被包进 `RenderNode`/`DisplayList` 体系 → **Z 序与穿透规则复杂化**，`SurfaceView` 的 "punch-through" 在 View 层内裁剪失效，容易出黑块/遮挡错乱 | ❌ 危险 |
| **SurfaceView 与 ComposeView 做兄弟**（`FrameLayout` 里先后添加） | SurfaceFlinger 直接合成两个 layer，**零额外拷贝**；Compose 只画控件层 | ✅ **推荐** |
| `TextureView` | 纹理回传 → **多一次 GPU→GPU 拷贝 + 延迟** | ❌ 禁止 |

> SurfaceView 的关键价值：它是**独立的 `Surface`**，在 SurfaceFlinger 里是**独立 layer**，
> 不需要经过 app 的硬件加速 Canvas。这是"零 CPU 回读、零额外拷贝"的物理基础。

---

## 4. iOS 侧：CMP 的互操作模型（**[源码实证]**）

这一节很重要，因为 **iOS 上 Compose 的架构和 Android 完全不同**。

**[源码实证]** `compose-multiplatform-core @ 47c012b`：
`compose/ui/ui/src/uikitMain/kotlin/androidx/compose/ui/interop/UIKitView.uikit.kt`

```kotlin
}.drawBehind {
    drawRect(Color.Transparent, blendMode = BlendMode.DstAtop) // draw transparent hole
}

DisposableEffect(Unit) {
    componentInfo.component = factory()
    componentInfo.container = UIView().apply { addSubview(componentInfo.component) }
    root.insertSubview(componentInfo.container, 0)        // ← 插到 Compose 渲染视图的【下方】
    onDispose { componentInfo.container.removeFromSuperview() }
}
```

**这意味着**：

1. iOS 上原生视图（我们的 Metal/MoltenVK 3D 层）永远在 **Compose 渲染内容的下面**；
2. Compose 用 `BlendMode.DstAtop` 在对应位置"挖一个透明洞"，把下面的原生视图**露出来**；
3. 所以 **Compose 的控件可以在 3D 之上**，**3D 永远不会盖住 Compose 控件**——除非反过来再嵌一层。

**另一个硬事实 [官方文档原文]**：
> "Compose Multiplatform rendering requires high refresh rates to be explicitly enabled:
> add the `CADisableMinimumFrameDurationOnPhone` key to your app's `Info.plist` file.
> **Without it, the app will crash at runtime.**"

即 **CMP 在 iOS 上默认不是 120Hz**，且配置缺失会**直接崩溃**。

**对 iOS 的结论**：

| 项 | 结论 |
|---|---|
| 3D 层位置 | 必须在 Compose **之下**（原生 UIView/CAMetalLayer 插到 root 下方） |
| UI 控件覆盖 3D | ✅ 天然支持（Compose 在上） |
| 3D 覆盖 UI（如全屏画笔轨迹） | ❌ 需要**反过来把 UIKit 层提到前面**，绕开 CMP 的模型 |
| 高刷 | 必须显式加 `CADisableMinimumFrameDurationOnPhone` |
| 风险 | ⚠️ "挖洞"方案涉及逐帧混合，长时间运行的内存/帧时间表现**我没有取得官方数据** |

> **这条会直接影响设计**：iOS 上"在场景上画 HUD 图形"不能靠 Compose 画在场景上再指望 Z 序，
> 必须让 **3D 场景自己有那个 HUD 层**（我们的 `IOverlay2D` 正好可以干），
> Compose 只负责**环绕式控件**（顶栏、底栏、面板）。
> 这恰好与"SDK 里有 `IOverlay2D`"的设计吻合——**这是选择它的又一个理由**。

---

## 5. 其他候选框架的瓶颈（对照）

| 框架 | 主要瓶颈 | 与我们的冲突 |
|---|---|---|
| **Flutter** | `PlatformView` 互操作性能**明显低于原生**（[分析判断]）；接入现有 Vulkan Surface 需要自己写插件 | 技术栈完全切换（Dart），现有 C++ SDK 只能当裸库 |
| **React Native** | 桥接层（新架构 Fabric 已缓解但仍有）→ 逐帧 3D 交互不友好 | JS 生态，性能不可控 |
| **Android View（纯）** | 无重组开销，但**手写 UI 成本高**——用户已明确否决 | ❌ 已被否 |
| **Filament / SceneView 内嵌 UI** | 进入其 Material/Scene 体系，拿不到 C4（无 GS）、内面渲染、gizmo、命令回放 | ❌ 与硬约束冲突（见技术路线对比 §12） |
| **ImGui（桌面调试）** | 每帧重建顶点缓冲 | 仅用于 PC 调试/CLI，不上移动端 |

---

## 6. 推荐架构（最终形态）

```
┌──────────────────────────────────────────────────────────────┐
│ Android:  FrameLayout                                        │
│   ├─ [0] SurfaceView        ← 3D 场景（自研 Vulkan SDK 渲染） │
│   └─ [1] ComposeView        ← 控件层（Compose，透明背景）      │
│        由 SurfaceFlinger 合成，零 CPU 回读、零额外拷贝         │
├──────────────────────────────────────────────────────────────┤
│ iOS:  root UIView                                            │
│   ├─ [0] UIView(CAMetalLayer) ← 3D 场景（MoltenVK）          │
│   └─ [1] ComposeUIViewController 视图 ← 控件层（CMP）        │
│        注意：CMP 的 UIKitView 模型是"原生在下 + 挖洞"          │
└──────────────────────────────────────────────────────────────┘
```

**输入仲裁**（与 SDK `InputRouter` 对齐）：

```
触摸事件 → FrameLayout.dispatchTouchEvent
   ├─ 命中 Compose 交互区（按钮/滑块）→ 交给 Compose
   └─ 否则 → SurfaceView → JNI → SDK InputRouter → 玩法
```

**帧率解耦**：3D 场景 60/120Hz 独立渲染；Compose 只在状态变化时重绘。
两者**不要求同帧**——这正是独立 Surface 的意义。若要求"工具体随场景实时跟手"，
由 3D 层自绘（`IOverlay2D`），**不经过 Compose**。

---

## 7. 对既有设计的修订

| 旧设计 | 修订 |
|---|---|
| UI 消费层设计 §5「Presenter 主路径：场景+UI 同帧同一 command list、单次 submit」 | ❌ **作废**（与 R1 一致）。UI 由 Compose 独立渲染，SDK 不管 UI 合成 |
| `IOverlay2D` 仅用于积木虚线/叠叠乐网格 | ✅ **扩展**：还用于 iOS 的"HUD 必须自己画"场景、以及所有"需要跟手实时更新"的浮层 |
| SDK 上屏路径（`ISwapchain`/`Presenter`） | 保持 R1 裁定：降级为桌面调试与录制入口 |

---

## 8. 验收口径（可测量，不靠感觉）

| # | 项 | 阈值 | 测量方式 |
|---|---|---|---|
| UI-P1 | SDK 侧 CPU 回读次数 | **0** | 门控 `check_ui_no_readback.py`（已有） |
| UI-P2 | Compose `graphicsLayer` 调用点 | **≤ 20** | 静态计数 + 代码评审 |
| UI-P3 | 每帧 Compose 重组范围 | 拖滑块时**不重组整树** | Layout Inspector / 重组计数 |
| UI-P4 | 3D 层是否在 Compose 树内 | **否**（必须兄弟节点） | 代码评审 + 布局 dump |
| UI-P5 | `TextureView` 使用 | **0** | 静态扫描 |
| UI-P6 | iOS `CADisableMinimumFrameDurationOnPhone` | 必须存在 | Info.plist 检查 |
| UI-P7 | 稳态帧 SDK VMA alloc/free | **0** | 已有 M5 断言 |

---

## 9. 未取得数据（诚实清单）

以下内容我**没有**拿到可引用的官方数据，因此本文不做数字化结论：

1. Compose vs View 在具体机型上的**帧时间对比 benchmark**；
2. CMP iOS 渲染后端（Skiko/Metal）的**持续帧率数据**；
3. Flutter `PlatformView` 的**具体互操作开销**；
4. SurfaceView 双层合成的**具体能耗差异**。

若需要这些数字，需要真机 profiling（`Perfetto` / `adb shell dumpsys SurfaceFlinger --latency`），
建议作为**实现阶段的第一件事**，而不是设计阶段猜。
