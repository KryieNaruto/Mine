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
  frame_renderer.h  IFrameRenderer:主路径录进设备 image,离屏路径读回
  input.h           InputOwner / Gesture / InputRouter
  hud.h             Control / Hud / build_minimal_hud
  router.h          IFeatureHost / Router / IStateStore / MemoryStateStore
  perf.h            QualityTier / FrameSample / PerfGovernor
  presenter.h       阶段状态机 + IPresentSurface
  ui_app.h          装配体:init / on_input / on_lifecycle / run_frame

src/ui/                     ← UI 层本体(零 Vk*)
  overlay_impl.h            Overlay2D 图元容器(私有不导出)
  overlay_renderer.cpp      图元 → 顶点/合批/base 栅格化(用于确定性测试)
  hud.cpp                   布局 + 命中 + 绘制
  input_router.cpp          仲裁 + 手势
  router.cpp                路由 + 状态存储
  perf_governor.cpp         档位与违规记账
  presenter.cpp             阶段状态机(纯 CPU,不碰 GPU)
  presenter_host.{h,cpp}    主路径适配器(IPresentSurface ← device::ISwapchain)
  presenter_offscreen.cpp   离屏适配器(→ IFrameRenderer::render_to_memory)
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

**为什么文件名必须含 `present`**:门控 UIG2(`check_ui_single_submit.py`)只允许
文件名含 `present` 的文件里出现 submit 调用。于是全仓的提交点被物理收拢成两个
文件:`presenter_host.cpp`(主路径)、`presenter_offscreen.cpp`(离屏)。想在别处
偷偷多提交一次,门控脚本会直接失败。

## 2. 两条路径,一套状态机

`Presenter` 是一个**纯状态机**,不持有任何 GPU 对象:

```
WaitFence → Acquire → Record → Submit(恰好一次)→ Present → 推进帧号
```

它只依赖两个接口:

- `IPresentSurface` —— 取图 / 提交 / 呈现。主路径由 `PresenterHost` 实现。
- `IFrameRenderer` —— 录制。两个方法,对应两条互斥通路:
  - `render_into_device_image()`:主路径,录进 swapchain image。**没有读回入口**
    —— 接口层就不给,所以 spec §5.4 的"禁止 CPU 读回"是编译期约束,不是纪律。
  - `render_to_memory()`:离屏路径(spec §5.5),golden / CLI 专用。

`Presenter` 的 `run_frame()` 按 `cfg.mode` 分流(见 `presenter.cpp` 的 Record 段):
早期实现不分 mode 就调主路径,导致离屏模式下每帧都是 `RecordFailed` —— 已修。

失败语义:**不重试、不静默**。

| 情况 | 返回 |
|---|---|
| 后台 / surface 未就绪 | `NoSurface` + 阶段 `Skipped`(**不消耗帧号**) |
| acquire 失败 | surface 给出的原因(`NeedRebuild` / `SurfaceLost`),绝不内部重试 |
| 录制失败 | `RecordFailed`,且一次 submit 都不发 |
| 提交失败 | `SubmitFailed`,不继续 present |

## 3. SDK 桩的处理方式

SDK 的 L1/L2/L3 还在别处同步落地,UI 层运行时只有 L0。桩的纪律是:

1. **桩是"同名的接口形状,不是假实现"**。`scene_stub.h` 里的 `IScene` 与将来
   L2 的 `IScene` 同签名;L2 落地时 UI 层只需改 include,不改逻辑。
2. **桩必须明确失败,不能假装成功**。`VulkanFrameRenderer::render_into_device_image()`
   在 L1 command list 缺席时返回 `false`,于是主路径如实报 `RecordFailed`。
   绝不返回 `true` 让上层以为上屏了。
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
- 违规计数:`submit_count != 1`、稳态 `vma_alloc_count != 0`、drawcall > 50、
  顶点 > 8000、input→present > 2 帧

## 6. 门控

| 门控 | 脚本 | 守什么 |
|---|---|---|
| UIG1 | `tools/check_ui_no_readback.py` | `src/ui/**` 与 `tests/*ui*` 里不得出现读回;唯一豁免是路径含 `offscreen` |
| UIG2 | `tools/check_ui_single_submit.py` | submit 只能出现在文件名含 `present` 的文件里;离屏豁免**不适用** |

两个脚本各自有自测(`tools/tests/test_ui_check_scripts.py`,31 个用例)。

## 7. 已知边界(本轮不做)

- **主路径暂不可用**:`render_into_device_image()` 等 SDK L1 的 command list。
  在此之前主路径如实报失败,离屏路径(CLI / golden)可用。
- **无文本图集**:标签用占位短横线绘制(`hud.cpp`),spec §9 记的待办。
- **玩法内容**:`UiApp` 只提供 `IFeatureHost` 骨架与 `FeatureId` 路由,
  不含任何玩法名或玩法逻辑(spec §2 命名纪律)。

## 8. 构建与验证

```powershell
# 本机每个新 shell 都没有 MSVC 的 INCLUDE/LIB,用包装脚本
powershell -File build\mb.ps1                  # 全量
powershell -File build\mb.ps1 magicblocks      # 只建库

build\magicblocks_test.exe --gtest_filter="ui_*"
build\magicblocks-ui-cli.exe render --width 320 --height 240 --output out.png
build\magicblocks-ui-cli.exe stats  --width 320 --height 240 --frames 3
build\magicblocks-ui-cli.exe present --width 320 --height 240 --frames 5

python tools\check_ui_no_readback.py   --root MagicBlocks
python tools\check_ui_single_submit.py --root MagicBlocks
python -m unittest discover -s tools\tests -p "test_ui_check_scripts.py"
```

测试纪律(与 L0 一致):**缺 SwiftShader 是 FAIL,不是 SKIP**;绝不回退物理 GPU。
