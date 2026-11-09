# MagicBlocks —— UI 层重建（上屏改壳层持有）

日期：2026-11-05
状态：实施中
前置裁定：[UI 绘制路线调研 v2](2026-11-05-magicblocks-ui-rendering-route-research.md) §7 —— 旧 UI 消费层设计 §5 **作废**

---

## 1. 为什么要重建

旧 UI 消费层（`docs/superpowers/specs/2026-11-03-magicblocks-ui-consumer-design.md` §5）
规定的主路径是：

```
Scene pass + UI pass 录进**同一个 command list** → 每帧**恰好 1 次** submit → present
```

调研 v2 §7 明确判定这条路线**作废**，理由是物理层面的：

- Android 上 UI 由 Compose 画在**自己的 `RenderNode`/`Surface`** 上，3D 场景是
  独立的 `SurfaceView`。两者由 **SurfaceFlinger 合成**，不需要、也不允许游戏引擎
  替系统做合成。
- 如果 SDK 硬要在同一张 swapchain image 上"场景 pass + UI pass 叠加"，就等于
  自研了一套合成器 —— 既拿不到 Compose 的控件，也和壳层的 Z 序/裁剪打架。

所以新边界是：**SDK 只负责"渲染一帧 3D 场景（含浮层）到壳层给的 surface"，
UI 合成完全交给壳层。**

---

## 2. 新架构

```
┌──────────────────────────────────────────────────────────────┐
│ Android:  FrameLayout                                        │
│   ├─ [0] SurfaceView        ← SDK 渲染进 ANativeWindow*       │
│   └─ [1] ComposeView        ← 控件层（Compose 自绘，SDK 不管） │
│        由 SurfaceFlinger 合成，零 CPU 回读、零额外拷贝         │
├──────────────────────────────────────────────────────────────┤
│ iOS:  root UIView                                            │
│   ├─ [0] UIView(CAMetalLayer) ← SDK 渲染进 CAMetalLayer*      │
│   └─ [1] ComposeUIViewController 视图 ← 控件层（CMP）         │
└──────────────────────────────────────────────────────────────┘
```

SDK 侧只暴露一件事：**把场景渲染到外部 surface**。

### 2.1 新增：`ISceneFrameSink`

壳层实现它，把"一次 submit + present"的能力交给 SDK：

| 方法 | 语义 |
|---|---|
| `extent()` / `format()` | 壳层当前 surface 的尺寸与格式 |
| `acquire_image()` | 取一张可写图像；返回 `NeedRebuild` 让 SDK 重建目标 |
| `submit_frame(list)` | 提交**这一帧场景**的命令流（`ICommandList`），**每帧恰好 1 次** |
| `present_image()` | 呈现刚提交的那张 |
| `surface_lost()` | 壳层报告 surface 失效 |

与旧 `IPresentSurface` 的关键差别：旧接口的 `submit(cmd, semaphore)` 是让 SDK
**自己发命令**、壳层只做 submit 转发；新接口的 `submit_frame(ICommandList&)`
让 SDK 交出**已录好的命令流**，壳层负责 acquire/present 语义。
**两者都不读回 CPU 内存。**

### 2.2 保留并强化：`IOverlay2D`

调研 §4 指出 iOS 上 Compose 控件**永远在 3D 之上**，因此"必须跟手的浮层"
（笔刷轨迹、网格、gizmo、HUD 数字）**不能靠 Compose 画在场景上**，
必须由 3D 层自己画。`IOverlay2D` 就是那个唯一的通道。

### 2.3 删除

- `Presenter`（同帧多 pass 合成状态机）—— 路线作废，整块删除
- `IPresentSurface`（要求 UI 与场景共用一次 submit）—— 由 `ISceneFrameSink` 取代
- `check_ui_single_submit.py` 的"场景+UI 同一 command list"断言 —— 随路线一起作废，
  改为"每帧场景恰好一次 submit"

---

## 3. 验收口径（沿用调研 §8）

| # | 项 | 阈值 | 测量方式 |
|---|---|---|---|
| UI-P1 | SDK 侧 CPU 回读次数 | **0** | `check_ui_no_readback.py` |
| UI-P2 | Compose `graphicsLayer` 调用点 | ≤ 20 | 静态计数 + 评审 |
| UI-P3 | 拖滑块时不重组整树 | — | Layout Inspector |
| UI-P4 | 3D 层不在 Compose 树内 | **必须兄弟节点** | 代码评审 + 布局 dump |
| UI-P5 | `TextureView` 使用 | **0** | 静态扫描 |
| UI-P6 | iOS `CADisableMinimumFrameDurationOnPhone` | 必须存在 | Info.plist |
| UI-P7 | 稳态帧 VMA alloc/free | **0** | M5 断言 |

新增（本层自身）：

| # | 项 | 阈值 | 测量方式 |
|---|---|---|---|
| UI-P8 | 每帧 `submit_frame` 调用次数 | **恰好 1** | 运行时计数 + 单测 |
| UI-P9 | 场景层对 `ui/` 的依赖方向 | UI 单向依赖 device/scene，不被反向依赖 | `check_layer_purity.py` |
| UI-P10 | 浮层必须走 `IOverlay2D`，不得绕过 | 静态扫描 | 新增门禁 |

---

## 4. 实施边界（本轮）

- **做**：接口重定义 + Windows 上的可运行验证（渲染一帧进外部 surface，并把浮层真正画出来，数值校验）。
- **不做**：Android/iOS 工程搭建（本机无 NDK 工具链），只做接口对齐与文档。
