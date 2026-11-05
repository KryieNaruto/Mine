# 魔法功能预览 —— 技术路线对比与三方库选型

日期：2026-11-03
状态：对比调研（供选型决策，不改变已定稿的 SDK / UI 消费层设计）

## 0. 本文档的定位

前两份文档已定稿：[渲染引擎 SDK 设计](2026-11-03-magicblocks-render-engine-sdk-design.md)、
[UI 消费层设计](2026-11-03-magicblocks-ui-consumer-design.md)。

本文档补上**技术路线对比**：同一个需求下，还有哪些别的实现路线、有哪些可用的三方库、
各自代价如何、为什么当前路线是最优解（或被哪些路线挑战）。

### 0.1 数据来源与可信度（第 2 版，本次补齐安卓侧调研）

| 数据 | 来源 | 可信度 |
|---|---|---|
| 库的 stars / 语言 / 许可 / 最近推送 / 是否归档 | **GitHub Search API + Repos API**（curl 实调，2026-11-03） | 高（一手数据，见各行标注） |
| 库的版本标签 | **GitHub Tags/Releases API + `git ls-remote`** 实探 | 高 |
| 库的功能自述 | **raw.githubusercontent.com 拉取的 README**（实读原文） | 高（以官方 README 为准） |
| 性能/集成难度评估 | 基于代码与文档的工程判断 | 中（无实测基准，需实现阶段验证） |

> 说明：本机环境里 `github.com` 一度被 hosts 解析到 `127.0.0.1`，导致 git clone 失败；
> 本次通过 hosts 临时放行 + 官方 API 完成了调研，**hosts 文件已原样还原**。
> 本会话的 `web_search` 工具因后端凭据失效不可用，故检索全部走 GitHub 官方 API。

## 1. 已定路线回顾

| 层 | 当前决策 |
|---|---|
| 图形后端 | 自研 RHI 抽象 + **Vulkan** 单后端（GLES/Metal 不做，留接口） |
| 内存 | **VMA v3.5.0**，大页 + 偏移子分配 |
| 几何着色器 | **硬禁**，用实例化 + VS 参数表替代 |
| 上屏 | **3D 场景直接渲染进 swapchain image**，零 CPU 回读；present 给系统合成器 |
| UI | **Compose（现成框架，不自研）**；UI 与 3D **两路独立输出，由系统合成器合并**（方案一，2026-11-05 裁定） |
| 命令模式 | 录制 / 回放 / 复现，命令流可序列化 |

## 2. 路线维度与候选方案总览

| 维度 | 路线 A（现选） | 路线 B | 路线 C | 路线 D | 路线 E（**本次新增：安卓侧现成方案**） |
|---|---|---|---|---|---|
| 渲染引擎 | 自研 RHI + Vulkan | 现成引擎（Filament / Ogre-Next / Diligent） | 跨后端封装（bgfx / sokol） | 引擎内嵌材质层（无） | **安卓现成 3D 库/框架（SceneView、AnKi、kool、korender、Magnum…）** |
| UI 绘制 | Compose（Android）/ CMP（iOS）—— **2026-11-05 裁定：不自研 UI** | 平台原生控件 | Dear ImGui / Nuklear / NanoVG | Qt/Cocos 等引擎内 UI | **Jetpack Compose / 原生 View + Filament** |
| 上屏 | 直接渲 swapchain（C 方案） | 离屏再 blit | 平台合成层叠加 | — | **由第三方库/系统 SurfaceFlinger 接管** |
| 内存 | VMA 分页偏移 | 每资源独立分配 | 引擎自带池 | 无显式管理 | **库自带分配策略（不可控）** |
| iOS | 设计考虑，本轮不做 | MoltenVK 直接支持 | 切 Metal 后端 | OpenGL ES | **SceneView 已支持 SwiftUI + RealityKit（苹果官方路线）** |

下面按维度展开。

## 3. 渲染引擎路线

### 3.1 路线 A：自研 RHI + Vulkan（现选）

- **优点**：完全可控；满足"命令模式三态 + 离屏一等公民 + 玩法通用接口"这类**非标准需求**；
  与工作区已有 `EasyPainter`/SwiftShader/lavapipe 的离屏范式一致；单后端，代码量最小。
- **代价**：Vulkan 生命周期/同步/描述符自己扛；Android 与桌面都要测。
- **与前三轮约束的吻合度**：最高（无 GS、VMA、命令流都是显式控制点）。

### 3.2 路线 B：现成引擎

| 库 | 最新版本（API/实探） | 特点 | 对本项目的适配 | 结论 |
|---|---|---|---|---|
| **Google Filament** `google/filament` ⭐20,684 · Apache-2.0 · C++ · 2026-11-03 活跃 | `v1.80.0` | README：*"real-time physically based rendering engine for **Android, iOS**, Windows, Linux, macOS, WebGPU, WebGL2"*；PBR 标杆；Android 有官方 Kotlin/Java 绑定与 `SurfaceView` 集成 | PBR/材质省事，但**深度内面渲染、体素、gizmo、命令复现都要绕进它的 Material/Scene 体系**；命令录制没有对等物 | ⚠️ 可作为 **印章 PBR** 的"参数参考"或后期替换，作全项目底座会失控 |
| **Ogre-Next** | `v3.0.0` | Hlms 可编程管线；重 | 移动端上屏链路由它接管，与 Presenter 冲突 | ❌ |
| **Diligent Engine** | `v2.5.6` | 跨后端 RHI（Vulkan/D3D12/Metal/GL） | 理念与 RHI 最接近，**可作设计参考**，带进来等于第二套资源/命令模型 | ⚠️ 参考，不落地 |
| **bgfx** | 无版本 tag（跟 master） | 跨后端封装（Vulkan/GL/Metal/D3D），C 风格 | 满足不了命令可复现；无 tag 与池约定冲突 | ❌ |
| **sokol_gfx** | 无版本 tag | 极简头文件跨后端 | 同上 | ❌ |

### 3.3 路线 C：GLES / 平台原生（Android GLES + iOS Metal）

- 移动端覆盖率最好，但用户已明确"**GLES 或 Metal 先不做，只做 Vulkan**"，且已否决。
- 保留理由：iOS 最终大概率要走 MoltenVK（Vulkan→Metal 转换层），而不是原生 Metal 重写。

### 3.4 路线 E（**本次新增**）：安卓侧现成 3D 库/框架调研

> 这一节是对上一版的补漏。上一版只列了"渲染引擎"和"UI"两个维度，**完全没有调研安卓侧
> 生态**。下面每个库的元数据来自 GitHub Repos/Search API（2026-11-03），功能自述来自其 README 原文。

#### 3.4.1 安卓 3D / AR 上层 SDK

| 库 | 元数据（API 实查） | 能力（README 原文要点） | 对本项目的适配 | 结论 |
|---|---|---|---|---|
| **SceneView** `sceneview/sceneview` | ⭐1,356 · Apache-2.0 · Kotlin · 2026-11-02 · tag `v4.65.0` | *"The AI-first 3D & AR SDK for **Jetpack Compose, SwiftUI** and the web… **Filament + ARCore on Android**, **RealityKit + ARKit on Apple**, Filament.js + WebXR… with Flutter, React Native and Compose Multiplatform bridges"* | **这是安卓侧最接近"现成轮子"的方案**：Compose 声明式、底层 Filament、iOS 侧已走苹果官方 RealityKit。但它是**模型浏览器/AR 场景**定位，我们需要的离屏录制、体素内面、gizmo、命令复现都没有对等物 | ⚠️ **值得作为 iOS 路线的参考**（它已证明"Android=Filament / iOS=RealityKit"是可行组合）；作本项目底座会拿不到需要的控制权 |
| **Sceneform Maintained** `sceneview/sceneform-android` | ⭐673 · Apache-2.0 · Java · **archived=True** · 最后推送 2024-06 | ARCore + Filament 的 Java 封装"Maintained 版" | **已归档**，上游 Google `google/Sceneform` 也已 404 | ❌ 归档，不许 |
| **ARKit/ARCore 原生** | — | 系统级 | 与玩法无关 | ❌ |

#### 3.4.2 安卓上能跑的 Vulkan 引擎

| 库 | 元数据（API 实查） | 能力（README 原文要点） | 对本项目的适配 | 结论 |
|---|---|---|---|---|
| **AnKi 3D Engine** `godlikepanos/anki-3d-engine` | ⭐1,629 · BSD · C++ · 2026-10-07 · 无版本 tag | *"Linux, Windows and **Android** open source game engine that runs on **Vulkan**"*；README 平台表明确列出 `Android / arm64 / Vulkan` | 完整游戏引擎（脚本、物理、渲染），**同样会接管上屏与场景体系**；无版本 tag 也不符合池约定 | ❌ 作参考（其移动端 Vulkan 接入可借鉴） |
| **kool** `kool-engine/kool` | ⭐594 · Apache-2.0 · Kotlin · 2026-10-25 | *"A **Vulkan / WebGPU / OpenGL** graphics engine written in **Kotlin**… Desktop JVM, **Android** and browsers"*；自带内嵌 UI 框架、物理、程序化几何 demo | Kotlin/JVM 技术栈，与我们的 C++ SDK 不同；Android 侧窗口/生命周期封装可参考 | ❌ 技术栈不符，但**其"程序化几何 + 内嵌 UI"思路与我们的 L3/L4 直接相关** |
| **Magnum** `mosra/magnum` | ⭐5,230 · License 见仓库（README 标 MIT）· C++ · 2026-10-30 · tag `v2020.06` | *"Lightweight and modular **C++11 graphics middleware**… OpenGL / WebGL family"*；支持 Android | **GL/GLES 路线**，与"只做 Vulkan"冲突；其场景图/材质拆分可参考 | ❌ 后端不符 |
| **Diligent / The-Forge / 其它** | — | — | 桌面导向 | ❌ |

#### 3.4.3 安卓 3D 查看器（学习参考，非底座）

| 库 | 元数据（API 实查） | 与本项目 | 结论 |
|---|---|---|---|
| **the3deer/android-3D-model-viewer** | ⭐1,558 · MIT · Java · 2026-09-04 · 版本 5.1.0 | 自研 **OpenGL ES 2.0/3.0** 引擎 + Android 端完整交互（旋转/缩放、线框、包围盒、**射线拾取 ray-cast selection**） | ✅ **拾取/线框/相机交互**的实现参考；后端是 GLES，不直接复用 |
| **dbrant/ModelViewer3D** | ⭐208 · 2026-11-01 | Android 上 STL/OBJ/PLY/3MF/FBX 查看器 | 参考 OBJ 加载与交互 |

#### 3.4.4 安卓 Vulkan 官方样例（**最值得抄的上屏代码**）

| 库 | 元数据（API 实查） | 用途 | 结论 |
|---|---|---|---|
| **googlesamples/android-vulkan-tutorials** | ⭐934 · **archived=True** · 最后推送 2023-11 | 官方教程：`ANativeWindow` + `VK_KHR_android_surface` + `vkCreateAndroidSurfaceKHR` 全流程 | ✅ **抄它的 surface / swapchain / 生命周期代码结构**，但归档不代表不能用（Vulkan 核心 API 稳定） |
| **KhronosGroup/Vulkan-Samples** | ⭐5,416 · Apache-2.0 · 2026-11-02 | 官方样例合集，含 **Android 平台页** | ✅ Android 上屏/旋转/重建的权威实现参考 |
| **SaschaWillems/Vulkan** | ⭐12,201 · MIT · 2026-10-25 | 桌面为主，样例极其完整 | ✅ shader/管线状态参考 |

#### 3.4.5 安卓侧补充三方库（本次新增候选）

| 库 | 元数据 | 用途 | 结论 |
|---|---|---|---|
| **ARM-software/astc-encoder** | ⭐1,312 · Apache-2.0 · tag `5.7.0` | ASTC 纹理压缩（安卓 tiler 首选格式） | ⚠️ 后期美术资源压缩时引入（离线工具链） |
| **KhronosGroup/KTX-Software** | ⭐1,396 · 2026-11-02 | KTX2 容器（配合 ASTC/BC 超压缩） | ⚠️ 后期 |
| **zeux/meshoptimizer** | ⭐8,777 · MIT · tag `v1.3` | 网格优化（体素/挤出网格顶点缓存优化） | ⚠️ 可选 |
| **google/gapid** | ⭐2,239 · 2024-05 | Android GPU 调试/抓帧（**验证上屏链路的利器**） | ✅ **强烈建议在性能调试期使用**，不做依赖 |
| **google/oboe** | ⭐4,129 · Apache-2.0 · 2026-11-01 | 低延迟音频 | 与本项目无关（列出以证已遍历） |

#### 3.4.6 关键发现（**直接挑战上一版结论**）

1. **"Android=Filament / iOS=RealityKit" 是安卓/iOS 生态的主流组合**（SceneView README 直接这么写）。
   → 我们的 iOS 路线可以**把 RealityKit 作为 MoltenVK 之外的备选**：iOS 侧不一定是"Vulkan through
   MoltenVK"，也可能是"iOS 换后端 = Metal/RealityKit"。这一点要在 iOS 阶段重新评估。
2. **安卓侧的"现成轮子"都停在"模型浏览/AR 场景"层次**，没有任何一个能同时满足：
   离屏录制回放、命令复现、VMA 分页、体素内面、无 GS。→ **自研 SDK 的判断依然成立**，
   但"上层 UI 用现成方案（SceneView/Compose）"和"底层 SDK 自研"可以拆开，
   上一版把二者捆在一起是错的。
3. **官方 Android Vulkan 样例已归档**，说明"NDK + `ANativeWindow` + 自建 surface"这条路
   不是安卓主流——主流是 **SurfaceView/Compose 里嵌 3D 渲染器**。所以我们的 Android 壳层
   （NativeActivity vs SurfaceView + JNI）必须重新权衡，不能默认 NativeActivity。
   > **2026-11-05 已据此裁定**：Android 壳层 = `Activity` + `FrameLayout` +
   > `SurfaceView`(3D) + `ComposeView`(UI)，见 §9.4 与
   > [SDK 设计大纲 §2.3](2026-11-03-magicblocks-render-engine-sdk-design.md)。

## 4. UI 绘制路线（重点对比：上屏代价）

> 用户明确：**SDK 不是瓶颈，瓶颈在上屏，一旦拷贝到 CPU 帧率断崖**。所以 UI 路线必须按"上屏代价"排序。
>
> **⚠️ 2026-11-05 重大修订**：用户裁定 **UI 必须用现成框架（Compose），不自研**，
> 且合成采用**方案一**（两路独立输出由系统合成器合并）。
> 本节原先的「A. 即时模式自绘进 Vulkan」**已作废**。
> 详细论证见 [UI 绘制路线调研](2026-11-05-magicblocks-ui-rendering-route-research.md)。

| 路线 | 上屏路径 | 额外合成次数 | CPU 往返 | 结论 |
|---|---|---|---|---|
| ~~A. 即时模式自绘进 Vulkan~~ | 场景 + UI → 同一 swapchain image | 0 | 无 | ❌ **作废**（用户要求不自研 UI） |
| **B. Compose + SurfaceView 兄弟节点**（**现选，方案一**） | 3D → SurfaceView；UI → ComposeView；两路独立，SurfaceFlinger 合成 | **0 次 app 内合成**（系统 DPU 硬件合成，几乎不占 GPU） | 无 | ✅ **主路径** |
| ~~B'. 自研控件进 Vulkan~~（原 A 的对立面） | — | — | — | ❌ 已否决 |
| C1. Dear ImGui | 第三方 Vulkan backend，画进当前 render pass | 0 | 无 | ⚠️ **降级为桌面调试/CLI 可选**，不上移动端 |
| C2. Nuklear | 纯 CPU 几何 + 自写 Vulkan 后端 | 0 | 无 | ❌ 移动端不适用 |
| C3. NanoVG | 矢量 2D 渲染器（无官方 Vulkan 后端） | 0 | 需移植 | ❌ 移动 Vulkan 不友好 |
| D. Qt / Cocos 等引擎内 UI | 随引擎走 | 0~1 | 视引擎 | ❌ 绑定过重 |

**对原「B. 平台原生控件吃合成开销」结论的更正**：

上一版把"系统合成一次"记作**代价**。这个判断**是错的**：

1. 现代 SoC 的显示控制器（DPU）合成多图层**几乎不占 GPU**，是专门的硬件通路；
2. 如果不走系统合成，就得把 UI 纹理采样进引擎 pass —— 那反而**每帧多 2 次全屏读写**；
3. 系统合成让 3D 与 UI 的**帧率完全解耦**，互不拖累。

**更正后结论**：**B（Compose + SurfaceView）是上屏代价最低的路线**，不是"省开发但吃开销"。

## 5. 上屏路线对比（核心瓶颈）

| 方案 | 链路 | 额外 copy | CPU 往返 | 延迟 | 结论 |
|---|---|---|---|---|---|
| **A. 3D 直接渲 swapchain**（现选） | 场景 pass → present（**不含 UI**） | 0 | 无 | 1 submit | ✅ |
| B. 离屏 → blit → swapchain | 场景 pass → 离屏 image → vkCmdBlitImage → swapchain | **1 次全屏 copy** | 无 | 1 submit | ⚠️ 仅离屏测试/录制 |
| C. CPU 回读再贴 | 回读 → map → CPU 合成 → 上传 | 0 | **有（致命）** | +1~2 帧 | ❌ 硬禁 |
| **D. 两路独立输出，系统合成**（**现选，方案一**） | 3D → SurfaceView；UI → ComposeView；SurfaceFlinger 合成 | **0** | 无 | 各 1 帧，互不等待 | ✅ **主路径** |
| E. UI 纹理进引擎 pass（方案二） | Compose → AHardwareBuffer → 引擎采样混合 | **+2 次全屏读写** | 无 | UI 需等引擎帧 | ❌ 已否决 |

**补充对比：present mode**（`IRenderTarget`/Presenter 的参数）

| present mode | 延迟 | 撕裂 | 功耗 | 适用 |
|---|---|---|---|---|
| FIFO（默认） | 1 帧 | 无 | 稳定 | 默认 |
| MAILBOX | 最低 | 无 | 略高 | 触控交互密集时 |
| IMMEDIATE | 最低 | **有** | 略高 | demo 调试 |

## 6. 内存分配路线

| 方案 | 说明 | 结论 |
|---|---|---|
| **A. VMA + 大页偏移子分配**（现选） | 64MB 设备页 / 8MB host 页；桶内空闲链表切偏移；稳态帧 0 次 VMA 调用 | ✅ |
| B. 每资源独立 VMA 分配 | 简单，但 drawable 多时分配/释放频繁 | ❌ 违背用户要求 |
| C. 自研分配器（不用 VMA） | 完全可控，但要自己处理内存类型、碎片、延迟释放 | ❌ 重复造轮子 |
| D. VMA + **`VmaVirtualBlock` 虚拟块** | VMA 官方的"一块大内存再切小块"封装；可与 A 叠加使用 | ✅ **可作为 A 的实现细节**：上层自研桶，底层虚拟块，减少自写链表代码 |

## 7. 几何生成 / 无 GS 路线

| 方案 | 说明 | 结论 |
|---|---|---|
| **A. 实例化 + VS 参数表**（现选） | 方块/圆柱/线框/gizmo 全走 `gl_InstanceIndex` 查 push constant / UBO | ✅ |
| B. 计算着色器生成几何 → 存储缓冲 | 体素面生成可放 GPU，但要 compute 能力与额外同步 | ⚠️ 后期优化项（体素量大时） |
| C. 几何着色器 | 用户硬禁 | ❌ |
| D. CPU 预生成网格 | 体素改色时 CPU 重建，可控但占 CPU | ⚠️ 首版可用，后期下放 GPU |

## 8. 三方库候选清单（按用途）

> 版本号：`git ls-remote` 实探；"无版本 tag"的库需要跟踪 commit（与池"tag 固定"约定冲突，需在池里按 commit 固定）。

### 8.1 渲染核心

| 库 | 最新 | 许可 | 用途 | 结论 |
|---|---|---|---|---|
| VulkanMemoryAllocator | `v3.5.0` | MIT | 内存（已入池） | ✅ 已用 |
| GLM | `1.0.1`（池内） | MIT | 数学 | ✅ 已用 |
| glslang / glslc (Vulkan SDK) | SDK 自带 | Apache-2.0 | 着色器编译 | ✅ 已用（泳池/CI） |
| meshoptimizer | `v1.3` | MIT | 网格简化/顶点缓存优化（体素/挤出网格用） | ⚠️ 可选，后期 |
| Diligent Engine | `v2.5.6` | Apache-2.0 | RHI 设计参考（不集成） | ⚠️ 只读参考 |

### 8.2 UI

| 库 | 最新 | 许可 | 用途 | 结论 |
|---|---|---|---|---|
| Dear ImGui | `v1.93.0` | MIT | 即时模式 UI（有 Vulkan backend） | ✅ 可选后端（需改造顶点缓冲复用） |
| Nuklear | 无 tag | MIT/PD | 极简即时 UI（需自写 Vulkan 后端） | ⚠️ 备选 |
| NanoVG | 无 tag | zlib | 矢量 2D（无 Vulkan 后端） | ❌ |
| stb_truetype / stb_image | 无 tag（单头） | MIT/PD | 字体光栅 / 图像解码 | ✅ 可作字体图集与 OBJ 纹理（若需要） |
| msdfgen | `v1.9.2` | MIT | MSDF 字体生成（离线） | ⚠️ 可选，高质量文字 |
| FreeType | `VER-2-15-0` | FreeType FTL/GPL | 字体（重，建议只用 stb_truetype） | ⚠️ 备选 |

### 8.3 资源 / 几何

| 库 | 最新 | 许可 | 用途 | 结论 |
|---|---|---|---|---|
| tinyobjloader | `v1.0.7` | MIT | OBJ 解析（**已定：采用**，已入池并编过） | ✅ |
| stb_image_write | 单头 | PD | PNG 输出（CLI/golden） | ✅ 建议用（EasyPainter 已在用） |
| Polygon triangulation（如 libtess2 / ears） | 视库 | — | 2D 轮廓带孔三角化（挤出） | 需选型，见下 §8.4 |

### 8.4 2D 轮廓三角化与布尔运算（**已定：引入三方库，不自研**）

> 用户已拍板：**计算类一律引入经过验证的高性能库**。详见
> [计算类依赖选型](2026-11-03-magicblocks-compute-deps-selection.md)。

| 库 | 版本（实探） | 用途 | 状态 |
|---|---|---|---|
| **libtess2** `memononen/libtess2` | `v1.0.2` | 2D 轮廓带孔三角化（挤出上下盖、印章底板填充） | ✅ 已入池并编过（release/debug） |
| **Clipper2** `AngusJohnson/Clipper2` | `Clipper2_2.0.1` | 2D 多边形布尔（笔画合并、外形裁切、轮廓简化） | ✅ 已入池并编过 |
| **meshoptimizer** `zeux/meshoptimizer` | `v1.3` | 网格简化 / 顶点缓存优化（挤出与导入网格） | ⏳ 待接入（L3 阶段） |

### 8.5 平台 / 构建

| 库 | 最新 | 用途 | 结论 |
|---|---|---|---|
| GLFW | 3.4（池内） | 桌面壳（CI/工具） | ✅ 已用 |
| MoltenVK | 跟随 Vulkan SDK | iOS 上跑 Vulkan | 后续 iOS 阶段 |
| SwiftShader / lavapipe | 池内 / 系统 | 离屏软件光栅 | ✅ 已用 |
| Android NDK r28 | 本机 `28.2.13676358` | Android 构建 | ✅ 环境就绪 |

## 9. 关键分歧点与建议

### 9.1 UI：Compose vs 自绘 vs Dear ImGui（**2026-11-05 重写**）

| | **Compose**（现选） | 自绘最小控件 | Dear ImGui |
|---|---|---|---|
| 用户裁定 | ✅ **必须用现成框架** | ❌ 已否决 | ❌ 不上移动端 |
| 开发量 | 小（布局/命中/动画/无障碍全自带） | 大（全自写） | 极小 |
| 上屏代价 | **0 次 app 内合成**（系统 DPU 硬件合成） | 0 | 0 |
| 与 SDK 的关系 | **完全解耦**：两路独立输出 | 需 SDK 内画 UI | 需 SDK 内画 UI |
| 帧率耦合 | **无** | 强耦合（同帧同 image） | 强耦合 |
| 与"稳态帧 0 分配"门槛 | **不影响 SDK**（各自独立） | 需自建 UI 零分配 | **必须改造**顶点缓冲 |
| 长线 | 平台官方，生态最优 | 完全可控但成本高 | 适合开发者面板 |
| 适用 | **移动端主路径** | — | **桌面调试 / CLI 性能面板** |

**建议**：移动端 **Compose**；桌面调试面板可用 Dear ImGui（不影响移动端架构）。

### 9.2 引擎底座：自研 vs Filament

自研的唯一损失是 PBR 与资源管线；本项目材质需求只有"纯色 + 金属把手"，
用 Filament 会为一个 PBR 拖进整套 Scene/Resource 体系并让出上屏控制权。**坚持自研**。

但 **§3.4 修订了这一点的一半**：**"底座自研"与"安卓壳层用现成方案"可以拆开**。
安卓侧的 SurfaceView/Compose 集成、生命周期、旋转/重建，可以参考 SceneView 与官方
`android-vulkan-tutorials` 的实现；底座（RHI/场景/命令流）仍然自研。

### 9.3 上屏：直接 swapchain vs 离屏

上屏必须是直接渲染进 swapchain（A 方案，**只画 3D**）。离屏只保留给 CLI/golden/录制回放。
**需要回写到 SDK 设计 §5.4**：`IRenderTarget` 区分 `TargetMode::Swapchain / Offscreen`。

### 9.4 安卓壳层：NativeActivity vs SurfaceView + Compose（**2026-11-05 更新**）

| | NativeActivity + `ANativeWindow` | **SurfaceView + Compose（现选）** |
|---|---|---|
| 参考实现 | `googlesamples/android-vulkan-tutorials`（**已归档**） | SceneView（主流）、Filament 官方 Android 集成 |
| 主流程度 | 低（官方样例已归档） | **高**（安卓生态实际做法） |
| UI 集成 | 需自绘全部控件（**已否决**） | **Compose 原生控件** |
| 性能 | 最直接 | SurfaceView 是独立 layer，**系统合成零 app 内开销**；Compose 独立刷新 |
| 帧率解耦 | 无 UI，谈不上解耦 | **3D 与 UI 两条独立管线** |
| 本项目结论 | ❌ | ✅ **采纳（方案一）** |

**建议**：**`Activity` + `FrameLayout` + `SurfaceView`(3D) + `ComposeView`(UI) 兄弟节点**。
3D 视图**不得**被 `AndroidView` 包进 Compose 树（UIG3）。
壳层与引擎通过 `IRenderTarget` 已抽象，SDK 不感知平台。

## 10. 风险与验证清单

| 风险 | 影响 | 验证方式 |
|---|---|---|
| ImGui 默认顶点缓冲每帧重建 | 违背零分配门槛 | 单测断言稳态帧 VMA alloc/free = 0 |
| Android 上 FIFO 在部分机型锁 30fps | 体感掉帧 | 切 MAILBOX 探测 + 统计 |
| 离屏与上屏 target 混用 | 多一次 copy | 代码扫描 + 帧结构单测（每帧 1 次 submit、0 次 blit） |
| 无版本 tag 的库（bgfx/sokol/stb/nanovg） | 池无法按 tag 固定 | 池内按 commit 固定，或在 deps.yaml 增加 `commit` 字段 |
| iOS MoltenVK 性能/特性差异 | 后期返工 | 后续阶段 POC，不进入本轮 |

## 11. 还需要你拍板的点

> 已拍板（2026-11-03）：UI 实现 = **安卓消费层必须使用现成方案**（详见 §12）；
> 2D 三角化 = **引入 libtess2**；OBJ 解析 = **tinyobjloader**；
> 计算类总原则 = **尽可能引入三方高性能库**（详见
> [计算类依赖选型](2026-11-03-magicblocks-compute-deps-selection.md)）。

1. **池内无 tag 库**：是否允许在 `deps.yaml` 增加 `commit` 字段（给 bgfx/sokol/stb 这类库用）？
2. **iOS 路线（§3.4.6 发现）**：iOS 阶段走 **MoltenVK（复用 Vulkan 后端）** 还是 **原生 Metal/RealityKit
   （跟随 SceneView 的主流组合）**？这决定 RHI 是否必须提前预留 Metal 后端。
3. ~~**安卓壳层具体形态**：Jetpack Compose 组合（推荐）还是 View 体系（§12）。~~
   → ✅ **2026-11-05 已裁定**：`FrameLayout` + `SurfaceView`(3D) + `ComposeView`(UI) 兄弟节点，见 §9.4。

## 12. 安卓消费层：必须使用现成方案（已拍板）

> 用户原话："**安卓消费层必须使用现成的，不然 UI 全都得自研！**"

### 12.1 含义

UI 消费层（Router / HUD / 输入仲裁 / 性能面板 / 状态存储）**不得自研**，直接建立在
安卓现成生态上。之前"自绘最小控件 / Dear ImGui"的讨论**作废**。

### 12.2 候选（元数据均为 GitHub API 实查）

| 方案 | 元数据 | UI 形态 | 底层渲染 | 结论 |
|---|---|---|---|---|
| **SceneView** `sceneview/sceneview` | ⭐1,356 · Apache-2.0 · Kotlin · `v4.65.0` · 2026-11-02 | **Jetpack Compose + SwiftUI** 声明式 | Android = **Filament + ARCore**；Apple = RealityKit + ARKit | ✅ **首选**：唯一同时覆盖 Android/iOS/Desktop 的 Compose 声明式 3D SDK |
| Sceneform Maintained | ⭐673 · **archived** | View 体系 | Filament | ❌ 归档 |
| 直接 Compose + SurfaceView + 自研桥 | — | Compose | 自研 Vulkan | ✅ **2026-11-05 采纳为方案一** —— UI 现成（Compose），3D 自研 Vulkan 接 SurfaceView，两路独立由系统合成器合并 |
| kool-engine | ⭐594 · Apache-2.0 · Kotlin | 引擎内嵌 UI | Vulkan/WebGPU/GL | ⚠️ 技术栈可参考，非 Compose 原生 |

### 12.3 对已定稿设计的影响（**这是本版最重要的结论**）

**与"自研 Vulkan 渲染核心（L0–L4）"存在直接冲突**，必须二选一：

| 路线 | 描述 | 代价 |
|---|---|---|
| **R-1：SDK 降级为"计算/几何库 + 离屏渲染器"**（推荐） | 上屏、UI、交互全部交给 SceneView/Filament；MagicBlocks 只保留 L2/L3 的**通用能力**（三角化、挤出、体素、拾取、命令录制）并作为 **离屏 golden/回放工具** | 现有 L0/L1 的 Vulkan 上屏代码（swapchain/NativeActivity/Presenter）**作废或退居测试**；但"命令复现 + 离屏 golden"的独特价值保留 |
| **R-2：保留自研渲染核心，消费层用"现成但不含渲染"的部分** | 用 Compose 做控件与路由，但场景仍自绘进自研 Vulkan surface（Compose ↔ SurfaceView 互操作） | UI 不自研，但**场景侧仍要自己接 Surface**；等于"现成 UI + 自研渲染" |
| R-3：全量改用 SceneView/Filament | 放弃自研渲染核心 | 离屏录制回放、体素内面、gizmo、无 GS 的定制渲染全部拿不到 → 与前面所有约束冲突 |

**R-1 已采纳**（用户指令"继续"，落地细节见
[R1 定位裁定](2026-11-03-magicblocks-r1-positioning-ruling.md)）：

> SDK 定位 = **计算/几何库 + 离屏渲染器**；上屏与控件 UI 交给平台壳层（SurfaceView + Compose）。
> 已落地的 `ISwapchain` / `Presenter` / `vulkan_swapchain_*` **一行不删**，降级为"桌面调试与录制入口"，
> 因此 R-2 可随时回滚。新增出口是**离屏 RGBA8**（无窗口、无显示服务），并加一张 golden 兜底。

**渲染后端仍自研 Vulkan/MoltenVK**：SceneView 的 Filament 后端只在"整体引入 3D 引擎"时才有意义，
那等于放弃 C4（无几何着色器）、体素内面、gizmo、命令回放这些硬约束 —— 与前面所有设计冲突。
故 Android 侧只复用其 **UI/组合层（Compose）**，不复用其渲染内核。

**仍待确认**：SceneView 是否仅在"UI/3D 控件"层面参考，还是必须整包依赖？若只是控件，
直接 `androidx.compose.*` + 自研 Vulkan SurfaceView 即可，不引入 Filament。
