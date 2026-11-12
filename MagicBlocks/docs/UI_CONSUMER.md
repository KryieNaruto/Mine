# MagicBlocks UI 消费层 —— 实现说明

> 对应设计:`docs/superpowers/specs/2026-11-03-magicblocks-ui-consumer-design.md`
> 本文件只讲**代码长什么样、为什么这样切**,不复述 spec。

## 1. 目录与分层

```
include/magicblocks/ui/     ← 公开面(壳层可见)
  types.h           Vec2 / Rect / Rgba8 / palette / TargetMode / DisplayInfo / FeatureId
  sdk_stub.h        桩策略说明(唯一解释"sdk_stub"这个名字的文件)
  scene_stub.h      S1 IScene + S9 IRenderFeature 的桩形状(命名空间 ui::stub)
  overlay.h         L3 IOverlay2D 桩 —— UI 层唯一的绘制出口
  frame_renderer.h  ISceneFrameSink:壳层提供的 surface 抽象(SDK 只往里画)
  scene_frame.h     SceneFrameDriver:一帧的状态机 + SceneFrameResult
  input.h           InputOwner / Gesture / InputRouter
  hud.h             Control / Hud / build_minimal_hud
  router.h          IFeatureHost / Router / IStateStore / MemoryStateStore
  perf.h            QualityTier / FrameSample / PerfGovernor
  ui_app.h          装配体:init / on_input / on_lifecycle / run_frame

src/ui/                     ← UI 层本体(零 Vk*)
  overlay_impl.h            Overlay2D 图元容器(私有不导出)
  overlay_renderer.cpp      图元 → 顶点/合批/base 栅格化(用于确定性测试)
  hud.cpp                   布局 + 命中 + 绘制
  input_router.cpp          仲裁 + 手势
  router.cpp                路由 + 状态存储
  perf_governor.cpp         档位与违规记账
  scene_frame.cpp           一帧状态机(纯 CPU,不碰 GPU)
  scene_sink_host.{h,cpp}   上屏适配器(ISceneFrameSink ← device::ISwapchain)
  scene_offscreen.cpp       离屏适配器(→ VulkanFrameRenderer::render_to_memory)
  offscreen_target.h        离屏适配器声明
  ui_app_internal.h         内部扩展接口(as_internal)
  ui_app.cpp                装配与帧循环

src/backend/vulkan/
  vulkan_ui_overlay.{h,cpp} UI 叠加层 pass(Vk* 只允许在这里)
  shaders/ui_overlay.vert   实例化四边形 + SSBO
  shaders/ui_overlay.frag   flat 颜色 + alpha 混合

src/ui_cli/main.cpp         magblocks-ui-cli:render / stats / present
tests/ui_test.cpp           UI 层测试套件(文件名含 ui → 被 UIG1/UIG2 扫描)
```

**为什么后端实现在 `src/backend/`**:门控 G3(`check_layer_purity.py`)规定
`Vk*` 只能出现在 `src/backend/**`。UI 层的逻辑代码因此一行 Vulkan 都不认识,
只在 `vulkan_ui_overlay.cpp` 里把"图元列表"翻译成 GPU 实例。

**为什么提交点被物理收拢**:门控 UIG2(`check_ui_single_submit.py`)把全仓的
submit 调用限制在被明确允许的位置。上屏路径现在**一个提交点都没有**
(`submits=0` 是常态,因为提交由壳层做),离屏路径(CI / golden)的提交集中在
`scene_offscreen.cpp` 与后端。想在别处偷偷多提交一次,门控脚本会直接失败。

## 2. 上屏路线:SDK 只画进壳层给的 surface

**这是 2026-11 定下的路线变更。** 早期实现是"SDK 驱动同一帧上屏":
SDK 自己拿 swapchain image、自己录、自己提交、自己 present。这个方案已删除。

现在的分工:

```
壳层(shell)          ← 拥有窗口 / swapchain / Compose 层 / SurfaceView
   │  提供一块 surface
   ▼
ISceneFrameSink       ← SDK 唯一的对外触点(include/magicblocks/ui/frame_renderer.h)
   │
   ▼
SDK 把"一帧场景"渲染进去
```

`ISceneFrameSink` 只有五个动作:

| 方法 | 谁实现 | 契约 |
|---|---|---|
| `extent()` / `format()` | 壳层 | 告诉 SDK 目标尺寸与格式 |
| `ready()` | 壳层 | 现在能不能出图 |
| `acquire_image()` | 壳层 | 给出一块可写的 image 句柄 |
| `submit_frame(list)` | 壳层 | SDK 把录制好的命令列表交出去,**由壳层决定何时提交/上屏** |
| `present_image(img)` | 壳层 | 告知壳层:这一帧录完了 |

SDK 内部由 `SceneFrameDriver`(`scene_frame.cpp`)驱动,是一个**纯状态机**,
不持有任何 GPU 对象:

```
Idle → Acquire → Record → Submit → Presented → 推进帧号
```

失败语义:**不重试、不静默**。

| 情况 | `FrameStatus` | 说明 |
|---|---|---|
| 没有 sink(CLI / 后台) | `NoSurface` | 正常跳过,**不消耗帧号**,`submits=0` |
| acquire 失败 | `NeedRebuild` / `SurfaceLost` | 壳层给的原因原样返回 |
| 录制失败 | `RecordFailed` | 一次 submit 都不发 |
| 提交失败 | `SubmitFailed` | 不继续 present |

`SceneFrameResult::ok()` 对 `Ok` 与 `NoSurface` 都返回真 —— 因为"这帧不该上屏"
不是错误。

### 为什么删掉了旧路线

旧路线里 SDK 和壳层都想管同一帧的生命周期,结果是:

- 壳层的 Compose 合成与 SDK 的第二条上屏路径互相打架;
- 要在 SDK 里模拟壳层的 frame pacing,做不对;
- 平台差异(Windows 的 `vkQueuePresentKHR` 有,vulkan-swiftshader 没有)漏进 SDK。

现在 SDK 只管"把这帧场景画到这块内存上",别的一概不管。

## 3. SDK 桩的处理方式

SDK 的 L1/L2/L3 还在别处同步落地,UI 层运行时只有 L0。桩的纪律是:

1. **桩是"同名的接口形状,不是假实现"**。`scene_stub.h` 里的 `IScene` 与将来
   L2 的 `IScene` 同签名;L2 落地时 UI 层只需改 include,不改逻辑。
2. **桩必须明确失败,不能假装成功**。`ShellSurfaceSink::submit_frame()` 在
   L1 command list 缺席时返回 `false`,于是上层如实报 `SubmitFailed`。
   绝不返回 `true` 让调用方以为上屏了。同理,`UiOverlayPass::append_*()` 在
   实例容量不足时返回 `Unsupported` —— 调用方**必须检查**,否则图元会被静默吞掉
   (这正是本轮修掉的那个缺陷)。
3. **桩不许被内部绕过**。`ui::stub` 命名空间里的类型只能在 `src/ui/` 内部出现,
   公开面用 `using` 别名(`router.h`),SDK 落地后改别名一行即可。

## 4. 输入仲裁(spec §7)

一根手指的归属在 **PointerDown 那一刻**定死,后续 Move/Up 沿用:

```
① HUD 命中?   → Hud(直接消费,不再下发)
② 工具激活且消费? → Tool
③ 相机兜底     → Camera
都不认领       → Unrouted(计入 stats.dropped)
```

`ui_Input.OwnerDoesNotJumpMidGesture` 专门盯着"画到一半跳槽到相机"这个经典 bug。

手势在**抬手时**统一成型(`Tap` / `Drag` / `LongPress` / `Pinch` / `TwoFingerPan`),
双指手势只喂相机层。pinch 的缩放比必须用**两指都在场时最后一次观察到的间距**
(`pinch_last_distance`)—— 抬手瞬间再算 `pair_distance()` 会得到近乎 0 的值,
于是每个 pinch 都被误判成"没缩放"(已修,见 `input_router.cpp`)。

## 5. 档位与降级(spec §6)

`PerfGovernor` 只做记账与决策,**不读时钟**(耗时由调用方注入):

- 超预算连续 `degrade_after_frames` 帧 → 降一档:`Full → NoMsaa → SimpleUi → LowRes`
- 低负载连续 `recover_after_frames` 帧且低于 `budget * recover_margin_ratio` → 升一档
- 每次切换都进 `tier_changes()` 并带原因字符串(**不静默**)
- 违规计数:`submit_count > 1`(离屏/后台帧合法地提交 0 次)、稳态 `vma_alloc_count != 0`、drawcall > 50、
  顶点 > 8000、input→present > 2 帧

## 6. 门控

| 门控 | 脚本 | 守什么 |
|---|---|---|
| UIG1 | `tools/check_ui_no_readback.py` | `src/ui/**` 与 `tests/*ui*` 里不得出现读回;唯一豁免是路径含 `offscreen` |
| UIG2 | `tools/check_ui_single_submit.py` | 每帧至多一次提交;submit 只能出现在被明确允许的位置 |

两个脚本各自有自测(`tools/tests/test_ui_check_scripts.py`,31 个用例)。

## 7. 已知边界(本轮不做)

- **上屏归壳层**:SDK 不调用 `vkQueuePresentKHR`。`ShellSurfaceSink::present_image()`
  对 L0 只是"记下这一帧录完了"并返回成功 —— 真正的呈现由壳层做。这一点在代码里
  有明确注释,不假装 SDK 自己上屏了。
- **壳层实现**:Windows 侧用 `magicblocks-sink-verify` 验证了接口可用(见 §8);
  Android/Compose 侧的壳层**已落地**(见 §10),不再属于"本轮不做"。
- **无文本图集**:标签用占位短横线绘制(`hud.cpp`),spec §9 记的待办。
- **玩法内容**:`UiApp` 只提供 `IFeatureHost` 骨架与 `FeatureId` 路由,
  不含任何玩法名或玩法逻辑(spec §2 命名纪律)。

## 8. 构建与验证

```powershell
# 本机每个新 shell 都没有 MSVC 的 INCLUDE/LIB,用包装脚本
powershell -File build\mb.ps1                  # 全量
powershell -File build\mb.ps1 magicblocks      # 只建库

build\magicblocks_test.exe --gtest_filter="ui_*"
build\magicblocks-sink-verify.exe build\tmp\sink   # 真实 surface 上屏验证
build\magicblocks-ui-cli.exe render --width 320 --height 240 --output out.png
build\magicblocks-ui-cli.exe stats  --width 320 --height 240 --frames 3
build\magicblocks-ui-cli.exe present --width 320 --height 240 --frames 5

python tools\check_ui_no_readback.py   --root MagicBlocks
python tools\check_ui_single_submit.py --root MagicBlocks
python -m unittest discover -s tools\tests -p "test_ui_check_scripts.py"
```

测试纪律(与 L0 一致):**缺 SwiftShader 是 FAIL,不是 SKIP**;绝不回退物理 GPU。

## 9. 本轮验证结果

`magicblocks-sink-verify` 用**真实 Vulkan(SwiftShader)** 把一帧场景渲染到
一块外部 surface 上,断言:

```
run_frame -> status=Ok phase=Presented submits=0 acquired=1 presented=1
  [OK] frame 整体成功
  [OK] 恰好 1 次 acquire_image
  [OK] 恰好 1 次 present_image
  [OK] acquire 返回了有效 image
  [OK] image extent 与外部 surface 一致(320x240)
  [OK] present 的正是 acquire 到的那一帧
  [OK] 每帧至多 1 次提交（UIG2）
  像素：非零=76800/76800，不透明=76800/76800
  [OK] 场景帧有实际像素输出
  [OK] HUD 图元真的光栅化到了外部 surface 上
```

出图 `build/tmp/sink/sink_verify_frame.png` 共 14 种颜色:深色底(64.3%)、
顶部面板(20.4%)、蓝色条形(5.1%)、以及 6 种彩色按钮块 —— 就是 HUD 的结构。

### 本轮修掉的缺陷

1. **`UiOverlayPass::init()` 从未被调用**(既有缺陷,与本次重构无关)。
   后果:`capacity_` 恒为 0,每帧 `append_*` 全部返回 `Unsupported` 被静默丢弃,
   离屏出图是纯透明空图。这是 `ui_Offscreen.*` 两个测试长期失败的真因。
   修法:在 `VulkanFrameRenderer` 构造里调 `init()`,并让调用方检查 `append_*` 返回值。
2. **`PerfGovernor` 的提交规则过严**:`submit_count != 1` 会把合法的 0 次提交
   (离屏/后台帧)记成违规。改为 `submit_count > 1`。

## 10. Android 壳层(MDP1221 平板)

### 10.1 视图结构

`MainActivity` 的布局是 `FrameLayout` 里的**兄弟节点**,不是嵌套:

```
FrameLayout
  ├── EngineSurfaceView   (SDK 的 Vulkan 输出,独立合成层,不参与 Compose 重绘)
  └── ComposeView         (壳层 UI:左右两个侧栏 + 居中 HUD 文本)
```

`SurfaceView` 走 `setZOrderMediaOverlay` 之外的默认层级,Compose 只画在两侧
**不透明面板**上(各 232dp),中间整块区域保持透明,触摸直接落到 `EngineSurfaceView`。
**不要把 SurfaceView 塞进 Compose 的 `AndroidView`** —— 那会把它变成 Compose
子节点,合成层被吞进 Compose,输入与帧率都会退化。

### 10.2 左右分栏

| 区域 | 内容 |
|---|---|
| 左栏「预设模型」 | `LazyColumn`:内置立方体 + `assets/res/models/*.obj` 扫描结果;空/扫描中/失败三态 + 「重新扫描」 |
| 右栏「功能」 | 缩放 −/+、复位视角、隐藏/显示信息、按设备能力枚举的 MSAA 档位 |
| 中间 | 一行 HUD 文本(旋转角、当前模型、帧率) |

模型扫描在**后台线程**做(`ModelCatalog.scan`),结果回到主线程;失败返回
`ModelScan.Failed(reason)` 而不是抛异常。

### 10.3 资源管线

`assets/res/` 与 `*.obj` 被 `assets/.gitignore` 排除。`tools/fetch-assets.ps1`
按 `assets.lock.json`(sha256 校验)从本地 RayTracing 安装目录填充;
`android/app/build.gradle` 增加了 `checkPresetModels` 任务并在 `preBuild` 前跑,
缺 `.obj` 时**直接让构建失败**,不静默出一个没有模型的 APK。

`sdk` 侧加载路径:`ModelCatalog.materialize()` 把 asset 解压到 `cacheDir/models`,
用 APK 的 `lastUpdateTime` 做 `.stamp`,按长度比对逐文件校验,并清理旧版本的残留
文件;之后 `nativeLoadModel(path, name)` 走 `prim::load_and_upload_model`。

### 10.4 手势轴向(重要)

平板面板物理竖屏、Activity 锁横屏,所以 `Display.getRotation() == ROTATION_90`,
逻辑坐标窗口 2160x1440、物理屏 1440x2160。**`MotionEvent` 的 x/y 是逻辑坐标**,
必须映射到视觉(物理)坐标之后才能驱动旋转,否则"左右滑"会变成"上下转"。

映射只存在于 `EngineSurfaceView.applyRotationDelta` 一处:

```
rot=90/270 时先交换 dx/dy,然后
  q ← normalize( q * Δpitch(局部 X) * Δyaw(局部 Y) )
```

姿态是**单位四元数** `(qx,qy,qz,qw)`,Kotlin → JNI → C++ 全链路只传四元数,
不分解欧拉角、不做 pitch 夹紧、没有万向锁。两个增量都是**右乘**(局部轴),
所以旋转轴永远是摄像机自己的轴:左右拖 = 绕相机 up 转,
上下拖 = 绕相机 right 转,与相机已经转了多少度无关。

C++ 侧(`scene_renderer.cpp`)从四元数取三根轴:`col0=right、col1=up、col2=back`,
`eye = center - dist * back`,`view = make_look_at(eye, center, up)`;物体世界矩阵恒为
**单位阵**,转的永远只有相机。`rot90` 在 C++ 里只保留给宽高比补偿
(那是像素密度问题,与轴向无关)。

### 10.5 渲染线程与 UI 线程(重要)

`session.render()` **不在主线程上跑**。早期它挂在 `Choreographer` 上,等于在 UI 线程里
调 Vulkan 提交;而 SDK 的 `render_frame()` 内部有同步等待(acquire fence、
`vkWaitForFences`、模型上传…)。SDK 一慢,**整条链就拖死主线程**,
输入/动画/Compose 重绘全停 —— 表现成"整个 App 奇卡"。

注意这不是合成层的问题:`SurfaceView` 的 buffer 一直由 SurfaceFlinger 独立
合成,和 Compose 本来就没共享;共享的是**调用的线程**。

现在的分工:

| 线程 | 职责 |
|---|---|
| `mb-render`(专用线程) | 只做 `session.render(qx,qy,qz,qw,scale,panX,panY)` |
| 主线程 | 手势 → 纯数学改四元数/缩放/平移;Compose 重绘 |

两者只通过一个 `@Volatile` 的 7-float 姿态快照(`pendingPose`)通信,没有任何等待。
主线程侧统一调 `requestRender()`(非阻塞),不再有同步出帧的点。

两条硬约束:

1. `stopRenderLoop()` 必须 `join` 到渲染线程真正退出,**再** `detachSurface()`,
   否则渲染线程会对着已销毁的 swapchain 取图(use-after-free)。
2. 渲染线程空闲时按 `FRAME_BUDGET_MS = 8` 节拍休眠,不是忙等 —— 两个
   线程共享 CPU/带宽,渲染线程狂转同样会把 UI 挤卡。

效果:SDK 重构期间再慢,也只有画面掉帧(SurfaceView 那层的
`queueBuffer fps` 下降),UI 不再跟着卡。

### 10.6 两条顶点布局 → 两条管线(重要)

场景里可能画两种物体,它们的顶点布局**不同**,所以管线也必须不同:

| 画什么 | 顶点结构 | 每顶点字节 | 颜色属性 |
|---|---|---|---|
| 内置立方体 | `android::Vertex`(`scene_renderer.h`) | 36 | `location 2` = `Float3` |
| 预设 OBJ 模型 | `primitives::Vertex`(`primitives/mesh.h`) | 48 | `location 2` = `UByte4Norm` + `uv` |

曾经的写法是 `record_into` 里"两者布局一致,共用同一条管线",直接复用
`cube_pipeline_desc()`。GPU 于是按 36 字节的步长去读 48 字节的数据:第一个顶点
碰巧还对,从第二个起每个字段都落在错位的字节上 —— 兔子的每个三角形三个角各自
飞到毫不相干的位置,就是用户说的**"三角面片全炸"**。

现在的约定(`scene_renderer.cpp`):

* `cube_pipeline_desc(samples)` —— 内置立方体的 36 字节布局;
* `model_pipeline_desc(samples)` —— 先复制立方体那份,再**整组换掉**
  `vertexLayout`(stride 48 + `offsetof`),着色器仍是同一个 `cube.vert/cube.frag`;
* 两条管线在 `rebuild_pipeline_for_swapchain()` 里**一起**重建。采样数一变,
  旧管线就与 render pass 不兼容(vkCmdDraw 被静默丢弃,屏幕只剩清屏色),
  所以模型管线不能只在 1x 时建一次;
* `record_into()` 按当前画的是模型还是立方体选管线;画模型但模型管线尚未就绪时
  **退回立方体管线**,宁可画错一帧也不 bind 一条与 pass 不兼容的管线。

回归证据:`magicblocks_test` 里 `SceneRendererModelPipeline` 用例组把
"模型管线的 stride/属性格式必须等于 `mesh_vertex_layout()` 且**不等于**立方体那份"
钉在 CI 上;真机上 5 个预设逐个点选、互相切换、再切回内置立方体,全部正确。
