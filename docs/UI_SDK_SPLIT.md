# MagicBlocks UI ⇄ SDK 拆分 —— 耦合面审计 + C ABI 契约

> 状态：**§1–§4 审计（只读，不改源码）；§5.1–§5.6 头文件草案；§5.7 已落盘实现并实测验证**
> 落盘产物：**`MagicBlocks/include/magicblocks/mb_capi.h`**（444 行 / 26 个 `MB_API` 入口 / 无内部头依赖）
> 审计对象：git 分支 `feat/ui-sdk-split`，提交 `98cc9d9`（fix(android): 恢复加速度手感 + 三轴独立…）
> 审计范围：`MagicBlocks/android/app/src/main/java/com/magicblocks/pad/*.kt`、`MagicBlocks/src/android/*`、`MagicBlocks/include/magicblocks/**`、`MagicBlocks/CMakeLists.txt`、`MagicBlocks/android/app/build.gradle`
> 读者：负责实现 C ABI 的工程师（§5 是已落盘的契约 + 验证记录）、负责 Android 壳改接的工程师、负责 CMake 拆分的工程师。
>
> **注意**：本文件第 5 节的代码块是**起草时**的草案（`mb_engine` 命名、`include/mb_capi.h` 路径）。
> 实际落盘以 `MagicBlocks/include/magicblocks/mb_capi.h` 为准，差异与理由见 **§5.7**。

---

## 0. 结论摘要（先看这个）

| # | 结论 |
|---|---|
| 1 | 目前 UI↔SDK 的**唯一**接触面是 `MbRenderer`（`object`，`System.loadLibrary("magicblocks")` + 12 个 `external fun`）以及一个**由 C++ 主动回调**的静态方法 `MbRenderer.logNative(String)`。 |
| 2 | 12 个 JNI 入口里有 3 个是**纯 Kotlin ↔ C++ 便利函数**（`nativeWindowFromSurface` / `nativeListModels` / `nativeDestroy` 之类不碰渲染对象的那几支），它们和渲染世界的耦合其实比看上去更弱；真正需要 C ABI 化的是「句柄 + 姿态 + 状态」这条线。 |
| 3 | **最严重的越界不是 JNI 签名，而是编译期的类型脐带**：`src/android/scene_renderer.h` 直接 include `magicblocks/device/render_device.h`、`swapchain.h`、`resources/{buffer,command,pipeline}.h`、`primitives/{types,model_catalog}.h`，并在成员里持 `std::unique_ptr<dev::IRenderDevice>` / `ISwapchain` / `res::IBuffer` / `res::IPipeline` / `prim::ILoadedModel`。**SDK 内部任何一个虚函数加减，这个类的内存布局就变，UI 侧的 .so 必须跟着重编。** |
| 4 | 第二层越界在 `scene_renderer.cpp`：它 `#include "backend/vulkan/vulkan_render_device.h"` 和 `vulkan_swapchain.h`，用 `dynamic_cast<backend::VulkanRenderDevice*>` / `dynamic_cast<backend::VulkanSwapchain*>` 去够 `create_swapchain_with_msaa()`、`msaa_samples()`、`pre_transform()`。**backend 私有类型泄漏到了 `src/android/`**（G3 门控只管「Vk\* 字样」，管不到 `backend::` 命名空间，所以现在是**门控下的暗漏洞**）。 |
| 5 | 第三层越界在 `jni_bridge.cpp`：它不直接碰 C++ 类型，但**把 JNI 层和 SceneRenderer 绑成一对**，并且反向依赖 Kotlin 类名 `com/magicblocks/pad/MbRenderer` 与方法签名 `logNative(Ljava/lang/String;)V`。改类名/改包名 = 运行期静默丢日志。 |
| 6 | 第四处隐患在 `CMakeLists.txt`：`target_link_libraries(magicblocks PUBLIC Vulkan::Vulkan)`（Android 分支是 `PUBLIC vulkan`）。`PUBLIC` 意味着任何 link 了 `magicblocks` 的目标都会**继承 Vulkan 头路径**；今天侥幸没炸，是因为 `src/android/` 恰好不 include `<vulkan/vulkan.h>` —— 这是「靠自觉」而不是「靠构建系统」。 |
| 7 | 本文件第 5 节给出 `mb_capi.h` 契约：纯 C99、只 include `<stdint.h>`/`<stddef.h>`、不透明句柄族、按值传的定长结构体、调用方缓冲区字符串、错误码枚举、版本三元组 + 逐结构体 `struct_size` 兼容位。第 4 节给出「UI 允许知道的面 / 仅 SDK 内部」的硬边界。第 6 节给出 CMake 拆分方案（`magicblocks_capi` → `magicblocks.dll` / `libmagicblocks.so`，PUBLIC include 只指向那一个公开头）。 |
| 8 | **头文件已落盘并实测**：`MagicBlocks/include/magicblocks/mb_capi.h`，27 个导出入口。7 个编译单元（x64/x86 × C11/C17/C++14/17/20，`/W4 /WX`）全通过；真实 DLL 的导出表是 27 个**未修饰 C 名**；三个消费者程序（纯 C / C++ / 模拟 JNI shim）共 **141 项运行期断言全过**。落盘过程中抓到并修掉了草案看不出的一个致命缺陷（缺 `extern "C"` 代码块 → C++ 侧链接必然失败），详见 **§5.7.1 ⑤**。 |
| 9 | **头文件本身不带任何调用方前提**：已加 UTF-8 BOM，C11/C17/C++17/C++20 在 `-W4 /WX` 下**不传 `/utf-8` 也编得过**（见 §9.4 的 B3c 门）。改契约前先看 **§9「新增一个 ABI 入口的操作清单」** —— 漏改必被门控抓住，不靠记性。 |

---

## 1. Kotlin 侧现状：全部 native 交互点

### 1.1 库加载与反向回调（隐式的 native 表面）

`MbRenderer.kt:16-27`

```kotlin
object MbRenderer {
    const val TAG = "MagicBlocks"
    init { System.loadLibrary("magicblocks") }      // ← 加载产物名，CMake 里 OUTPUT_NAME "magicblocks"
    @JvmStatic fun logNative(msg: String) { Log.e(TAG, "[native] $msg") }   // ← C++ 反过来调它
}
```

* **`System.loadLibrary("magicblocks")`**：唯一一处对产物文件名硬编码。改成 C ABI 后这里仍然是 `loadLibrary("magicblocks")`，但它加载的东西从「带 JNI 的 C++ 库」变成「纯 C ABI 库 + 薄 JNI 适配层（可选）」。
* **`logNative`**：`jni_bridge.cpp:28-46` 通过 `FindClass("com/magicblocks/pad/MbRenderer")` + `GetStaticMethodID("logNative","(Ljava/lang/String;)V")` 调用。**这是一条 Kotlin → C++ 的隐式契约**：改类名、改方法名、改签名为 `logNative(String, Int)`、或者把 `object` 改成 `class`，C++ 侧都会走进「找不到就静默返回」的分支（`jni_bridge.cpp:31-39` 明确 `ExceptionClear()` 后 return），**错误日志整条消失但程序照常跑**。C ABI 版本必须改成**注册回调**（见 5.2 的 `mb_log_cb`），让连接失败在**调用时**就暴露。

### 1.2 `EngineSession` 覆盖的 12 个 `external fun`

逐个列出（文件/行号、JNI 实现、参数与返回值语义、线程与生命周期约束）。「触发方」列说明 UI 是什么时候调的。

| # | Kotlin 声明（`MbRenderer.kt`） | JNI 实现（`jni_bridge.cpp`） | 参数 / 返回值语义 | 调用方 / 线程 / 约束 |
|---|---|---|---|---|
| 1 | `nativeCreate(): Long` `:29` | `Java_..._nativeCreate` `:52-61` | 返回 `new SceneRenderer*` 被 `reinterpret_cast<jlong>` 包成句柄。**init 失败也返回非 0**（注释明说：让 `status()` 能读到失败原因）。**没有错误码**：调用方只能看 `status()` 字符串猜。 | `EngineSession.create()` `:87-92`；`SurfaceView.surfaceCreated → ensureEngine()` `EngineSurfaceView.kt:197-210`；**UI 线程**。 |
| 2 | `nativeSurfaceCreated(handle, window, width, height): Boolean` `:30` | `:63-75` | `window` 是 `nativeWindowFromSurface()` 给出的 `ANativeWindow*`（`jlong`）。建 swapchain + 按交换链真实格式重建管线。失败 → `logNative("attach_surface 失败: …")` + `false`。 | `EngineSession.attachSurface()` `:98-109` ← `EngineSurfaceView.attach()` `:228-242`（`surfaceCreated`/`surfaceChanged`/`applyMsaa`）；**UI 线程**。**内部会同步等 GPU**（建交换链、建管线、可能 `vkDeviceWaitIdle`），今天靠「启动时只做一次」勉强没 ANR。 |
| 3 | `nativeSurfaceDestroyed(handle)` `:31` | `:77-80` | `swapchain_.reset()` + `ANativeWindow_release()`。**必须保证没有渲染线程还在里面取图**（`EngineSurfaceView.kt:849-860` 的 `join(2000)` 就是为了这个）。 | `EngineSession.detachSurface()` `:111-115`；UI 线程。 |
| 4 | `nativeRenderFrame(handle, qx,qy,qz,qw, scale, panX,panY): Boolean` `:32-41` | `:82-94` | 7 个 `float`：**相机单位四元数**（相机系→世界系）+ 缩放 + **世界空间**平移。成功 = 取图、录制、提交、present 全部完成。失败 → 回调 log + `false`，原因不返回（只进 `status_text`）。 | `EngineSession.render()` `:123-126` ← `renderOnce()` `EngineSurfaceView.kt:244-298` ← **专用渲染线程 `mb-render`**（`:841-889`）。**热路径：每帧一次**。 |
| 5 | `nativeStatus(handle): String` `:42` | `:96-101` | `"frames=N size=WxH surface=ok|none rot=DEG model=NAME [last_error=…]"`（`scene_renderer.cpp:596-603`）。**一整条可被 UI 解析的字符串**——这是当前「状态」面的事实形态，但格式从未成为契约。 | `EngineSession.status()` `:128`；渲染线程每 500ms（`:289-292`）+ 失败时（`:295`）。 |
| 6 | `nativeDestroy(handle)` `:43` | `:103-106` | `delete` 句柄。**不校验句柄是否有效**。`EngineSession.destroy()` 之后把 `handle` 置 0，但 JNI 侧没有「句柄已释放」的标记，重复 destroy 是 UB。 | `EngineSession.destroy()` `:162-169` ← `EngineSurfaceView.dispose()` `:937-941` ← `MainActivity.onDestroy()` `MainActivity.kt:284-288`；UI 线程。 |
| 7 | `nativeSetMsaa(handle, samples: Int)` `:46` | `:114-118` | 只写 `msaa_samples_`，**必须在 attach 之前**；`<=0` 归一化成 1。**无返回值、无法失败**。 | `EngineSession.setMsaa()` `:146-149` ← `EngineSurfaceView.attach()` `:233`；UI 线程。 |
| 8 | `nativeMsaaSamples(handle): Int` `:49` | `:121-125` | **实际生效**采样数（设备不支持会低于请求值）。null 句柄 → 1。 | `EngineSession.msaaSamples()` `:151`；渲染线程每帧（`EngineSurfaceView.kt:280`）+ UI 线程（`:236`）。**跨线程读一个 uint32**。 |
| 9 | `nativeSupportedMsaa(handle): String` `:52` | `:129-142` | 逗号分隔的设备支持档位 `"1,4,8"`。空 → `"1"`。 | `EngineSession.supportedMsaa()` `:154-160` 自己 `split(',')`；UI 线程（`MainActivity.kt:143`，经 `runOnUiThread`）。 |
| 10 | `nativeListModels(directory: String): String` `:61` | `:153-181` | **不依赖句柄**的纯函数。返回 `"ok\n"` + 每行 `name\tpath\tbytes\tok(0/1)\tmessage`，或首行 `"err\t<原因>"`。**"目录不存在" 与 "空目录" 必须可区分**（`scene_renderer.cpp:161-179` + `tests/magicblocks_l3_model_catalog_test.cpp:116-131` 钉死）。 | `ModelCatalog.scan()` `MbRenderer.kt:201-231` ← `MainActivity.loadModelList()` `:263-282`，**后台线程 `model-scan`**（不是 UI 线程、不是渲染线程）。 |
| 11 | `nativeLoadModel(handle, path: String, name: String): Boolean` `:64` | `:185-212` | 切到 OBJ。失败 → `false` 且**渲染器保持原状**（`scene_renderer.cpp:569-579`），原因只进 `status_text`。 | `EngineSession.loadModel()` `:131-132` ← `EngineSurfaceView.selectModel()` `:926-935`；**UI 线程**。**内部会建 GPU 缓冲 → 同步等 GPU**。 |
| 12 | `nativeClearModel(handle)` `:67` | `:215-218` | 释放模型，回内置立方体。 | `EngineSurfaceView.selectModel(null)` `:927-929`；UI 线程。 |
| 13 | `nativeWindowFromSurface(surface: Surface): Long` `:70` | `:223-234` | `ANativeWindow_fromSurface()`，**引用计数 +1 由引擎持有**，`detach_surface` 时 release。非 NDK 构建返回 0。 | `EngineSession.attachSurface()` `:100`；UI 线程。**这是一次「Surface 句柄搬运」，不是渲染调用**，语义上先天适合做成 C ABI 的 `mb_surface_wrap`。 |

> 说明：表里 13 行 = `MbRenderer` 里 12 个 `external fun` + `logNative` 反向回调；其中 `nativeWindowFromSurface` 与 `nativeListModels` 都不是「对某个渲染对象做事」，是**纯工具函数**（见 1.3）。

### 1.3 谁在调、什么时候调（线程与顺序 —— 这是拆分时必须保持的契约）

```
UI 线程 (View 线程)                                渲染线程 "mb-render"
────────────────────────────────────────────────  ────────────────────────────────
surfaceCreated   → create()                        loop:
surfaceChanged   → stopRenderLoop()                 renderOnce() {
  → detachSurface()                                   render(qx..panY)   ← #4 每帧
  → setMsaa(requested) (#7)                           msaaSamples()      ← #8 每 1s
  → attachSurface():                                  status()           ← #5 每 500ms
       windowFromSurface (#13)                        }
       nativeSurfaceCreated (#1 attach, #2)
  → msaaSamples() (#8)
  → startRenderLoop()
[MSAA 按钮] applyMsaa → detach → attach（同上）
[模型列表] selectModel → loadModel (#11) / clearModel (#12)
  → requestRender()（只置一个 volatile 标志，不阻塞）
[上屏/状态] onStatus → status() (#5)、supportedMsaa (#9)
surfaceDestroyed → stopRenderLoop()（join 2s）→ detachSurface() (#3)
onDestroy        → dispose() → destroy() (#6)
后台线程 "model-scan"：ModelCatalog.scan → nativeListModels (#10)（与渲染无关）
```

**必须保住的 5 条现有语义**（拆分后的 C ABI 不能丢）：

1. **句柄可空**：`nativeCreate` 在 init 失败时也返回非 0（注释明说，为了让 `status()` 能报原因）。新 ABI 改成 `mb_err` 返回值 + out 参数句柄 = 0，调用方必须能表达「创建失败但仍有原因可读」——见 5.2 `mb_engine_create` 的 `err` 语义。
2. **请求值 ≠ 生效值**：MSAA 有「请求」与「实际生效」两个数，UI 必须显示后者（`EngineSurfaceView.kt:133-142` 的注释写得很直白：不许出现「点了 8x 实际 4x」还显示 8x）。
3. **detach 前必须等渲染线程退出**：否则 `swapchain_.reset()` 与 `acquire_next_image()` 之间是 use-after-free。C ABI 里这条要变成**库自己保证**（内部引用计数 / 每句柄一把锁），不能只靠 Kotlin 侧的 `join(2000)`。
4. **`loadModel` 失败保持原状**：不能偷偷切回立方体。
5. **目录扫描的「不存在 ≠ 空」**：这条已经有 gtest 钉着，必须原样保留到新的错误码 + 结果遍历 API 上。

### 1.4 现状里已经存在的「UI 依赖 SDK 内部」的隐式耦合

| # | 位置 | 耦合形态 | 风险 |
|---|---|---|---|
| C1 | `MbRenderer.kt:19` `System.loadLibrary("magicblocks")` | 产物文件名硬编码 | 改名/改产物形态即崩（`UnsatisfiedLinkError`） |
| C2 | `MbRenderer.kt:25-27` ↔ `jni_bridge.cpp:30-42` | 类名 `com/magicblocks/pad/MbRenderer` + `logNative(Ljava/lang/String;)V` 是字符串契约 | 改名字静默丢日志 |
| C3 | `MbRenderer.kt:208` `nativeListModels` 的 `\t` `\n` 分隔格式 | 文本协议没版本号、没转义；模型名里带 tab 就散架 | 解析歧义 |
| C4 | `MbRenderer.kt:156-160` `"1,4,8".split(',')` | 文本协议 | 同上 |
| C5 | `EngineSurfaceView.kt:289-292` `status()` 的中文/英文混合格式 | UI 只是**原样显示**（`MainActivity.kt` 的 HUD），没有解析 —— 所以今天不炸；但一旦有人想解析 `frames=` 就立刻绑死 | 潜在 |
| C6 | `EngineSurfaceView.kt:251` 一次传 7 个 float | 参数顺序即契约，加字段就得改 JNI 签名 | 演进成本 |
| C7 | `EngineSurfaceView.kt:280` **在渲染线程里**调 `msaaSamples()` | 每帧一次跨 JNI；返回的 `Int` 与交换链状态之间有竞态窗口 | 轻微 |
| C8 | `MainActivity.kt:143` 在 `onStatus` 回调里（`runOnUiThread` 后）调 `supportedMsaaLevels()` | 依赖「attach 之后才有真实档位」这一时序约定，但接口本身没说 | 时序耦合 |
| C9 | `EngineSurfaceView.kt:954-958` `applyMsaa` 自己拼「detach → set → attach」 | **SDK 的交换链重建顺序被 UI 实现了**。SDK 内部若改（比如支持 attach 后热切 MSAA），UI 得跟着改 | 结构性 |
| C10 | `app/build.gradle:29-33` `externalNativeBuild { path '../../CMakeLists.txt' }` | Android App 直接编**整个引擎源码树**（含所有 CLI target 的定义、GTest 探测、tess2 池路径推导） | 构建耦合；非 Android 平台完全没有动态库产物 |
| C11 | `build.gradle:22` `-DMB_BUILD_TESTS=OFF` 靠手传 | 同上，是 C10 的补丁 | 同上 |

---

## 2. C++ 侧现状：实现依赖与越界点

### 2.1 `src/android/scene_renderer.h`（161 行）的 include 与成员

**直接 include（15-29 行）**

| include | 属于哪一层 | 为什么它是越界 |
|---|---|---|
| `magicblocks/device/render_device.h` | L0 | 拉进 `IRenderDevice`（纯虚 18 个函数）、`DeviceCaps`、`create_render_device()`；**任何 L0 接口改动都会让 `SceneRenderer` 这个类重编** |
| `magicblocks/device/swapchain.h` | L0 | 拉进 `ISwapchain`、`ImageStatus`、`ImageResult` |
| `magicblocks/resources/buffer.h` / `command.h` / `pipeline.h` | L1 | 拉进 `IBuffer` / `ICommandList` / `IPipeline`（+ 经它们间接拉进 `handles.h`→`type_traits`、`pipeline.h`→`<vector>`） |
| `magicblocks/primitives/model_catalog.h` | L3 | 拉进 `ILoadedModel`、`ModelEntry`、`load_and_upload_model()` |
| `magicblocks/primitives/types.h` | L3 | 拉进 `Vec3` / `Mat4`（被 `record_into` 用） |
| `<atomic> <memory> <mutex> <string> <vector>` | STL | 头文件本身就是**纯 C++ 抽象**，任何 C 消费者都无法 include |

**成员里的 SDK 类型（123-159 行）**

```cpp
std::unique_ptr<dev::IRenderDevice>  device_;       // L0 接口
std::unique_ptr<dev::ISwapchain>     swapchain_;    // L0 接口
std::unique_ptr<res::IBuffer>        vertex_buffer_, index_buffer_;   // L1
std::unique_ptr<res::IPipeline>      pipeline_;                       // L1
std::unique_ptr<prim::ILoadedModel>  model_;                          // L3
```

> **这才是「SDK 内部重构影响 UI 编译」的物理原因**：`SceneRenderer` 的大小与对齐由这 6 个 `unique_ptr` + 若干标量决定。**只要 L0/L1/L3 的任何虚类布局变化（哪怕只是加一个虚函数），`SceneRenderer` 的 vtable/尺寸/析构顺序就变，`jni_bridge.cpp` 必须重编；而 `jni_bridge.cpp` 和 `SceneRenderer` 是编在同一个 `magicblocks_jni` target 里的 —— 于是「SDK 内部重构」= 「Android 全部重编」**。今天没暴露是因为都在一棵树里编；一旦按模块分发，这就是 ABI 断裂。

### 2.2 `src/android/scene_renderer.cpp`（606 行）的越界

| 行 | 内容 | 越界类型 |
|---|---|---|
| `:16-17` | `#include "backend/vulkan/vulkan_render_device.h"` / `vulkan_swapchain.h` | **backend 私有头泄漏到 `src/android/`** |
| `:248-252` | `dynamic_cast<backend::VulkanRenderDevice*>(device_.get())->create_swapchain_with_msaa(ns, extent, msaa)` | **为了带 MSAA 建交换链而 RTTI 下钻到 backend**。G3 门控（`tools/check_layer_purity.py`）只扫 `Vk\*` 字样与 `<vulkan/...>` include，**扫不到 `backend::` 命名空间** → 现行门控下是**合法但违背分层意图**的写法 |
| `:261-263` | `dynamic_cast<backend::VulkanSwapchain*>(swapchain_.get())->msaa_samples()` | 同上；用 backend 私有 getter 拿「实际生效采样数」 |
| `:273-279` | `dynamic_cast<backend::VulkanSwapchain*>(...)->pre_transform()` + 判断 `VK_SURFACE_TRANSFORM_ROTATE_*` | **最刺眼的一处**：`VK_SURFACE_TRANSFORM_ROTATE_90_BIT_KHR` 是 Vulkan 枚举，出现在 `src/backend/` 之外，**G3 应该报错却没报错**（因为它的模式只匹配 `Vk` 前缀/`vk` 函数名/`vulkan.h`，不匹配 `VK_` 宏）。建议顺带修门控（见 §7 M6）。 |
| `:299-317` | `vkdev->create_pipeline_for_shape(cube_pipeline_desc(samples), shape)`，`shape.from_swapchain = true` | 管线必须按交换链真实颜色格式+采样数派生 —— 这是「SDK 应当自己知道的事」，被提出到了 Android 壳层 |
| `:341-497` `record_into()` | 全部 `res::` / `prim::` 调用；`prim::make_look_at` / `make_perspective` / `Mat4::identity` | **相机数学、清屏颜色、视口/scissor、push constants 布局**全在 Android 壳层。128 字节 `PushConstants` 与 `shaders/cube.vert` 一一对应（`:25` 有 `static_assert`）→ **UI 侧的 .cpp 依赖着色器 ABI** |
| `:570-571` | `prim::load_and_upload_model(*device_, path, name, ModelNormalize{})` | 模型加载/归一化口径（包围盒对角线→1.0）写在壳层 |
| `:150-168` | `dev::DeviceDesc{app_name="MagicBlocksPad", enable_validation=false, require_software_rasterizer=false}` | 设备创建参数在壳层 |

**结论**：`src/android/` 今天实际上是**「UI 壳 + 一个私有样例场景」的混合物**。它既承担「把 Surface 接到 SDK」的壳职责，又实现了一整个「旋转立方体/加载 OBJ」的演示场景。**拆分时必须把后者搬进 SDK**（例如 `src/scenes/cube_demo/` 或 `src/apps/pad_scene/`），壳层只留 Surface 生命周期与姿态透传。

### 2.3 `src/android/jni_bridge.cpp`（236 行）的越界

| 行 | 内容 | 问题 |
|---|---|---|
| `:21` `using magicblocks::android::SceneRenderer;` | JNI 层**直接**依赖 C++ 类 | 壳层与 SDK 的 C++ 类型是同一个 TU 集合 |
| `:23-24` `pack/unpack` = `reinterpret_cast<jlong>` | 句柄 = 裸指针 | 跨语言句柄没有类型标签、没有世代号；重复 destroy / 用错句柄类型都无法检测 |
| `:28-46` `log_native` | 反射式 `FindClass`/`GetStaticMethodID` | 见 C2；而且**每次错误都做一次类查找**（热路径上浪费） |
| `:56-59` | init 失败仍返回句柄 | 调用方无法区分「成功」与「失败但仍可用」 |
| `:161` | `magicblocks::primitives::scan_model_dir(dir)` | JNI 层直接调 L3 的 `primitives`（`model_catalog.h`），并把返回的 `std::vector<ModelEntry>` 手工序列化成 `\t` 文本 |
| `:227-233` | `ANativeWindow_fromSurface` 在非 NDK 构建下返回 0 | 平台宏 `MB_ANDROID_USE_NDK` 同时出现在 `magicblocks` 和 `magicblocks_jni` 两个 target 的编译定义里（`CMakeLists.txt:318-320`、`:471-473`），**必须手工保持同步**，否则一边真路径一边 stub → 运行时 0 指针 |

### 2.4 构建侧现状（`CMakeLists.txt`）

| 行 | 内容 | 问题 |
|---|---|---|
| `:130` | `add_library(magicblocks STATIC …)` | **只有静态库**。非 Android 平台**根本没有 `magicblocks.dll`**；今天桌面端的「UI」是 8 个独立 exe，全部静态链进引擎。 |
| `:268` | `target_include_directories(magicblocks PUBLIC include PRIVATE src …)` | `PUBLIC include` = **整个 `include/magicblocks/**`（58 个头）都对消费者可见**。消费者只要 `#include "magicblocks/device/render_device.h"` 就能碰到 L0 接口 —— 没有任何东西阻止 UI 越界，**只是「自觉」**。 |
| `:306-310` | `target_link_libraries(magicblocks PUBLIC vulkan) / (PUBLIC Vulkan::Vulkan)` | **Vulkan 头路径以 PUBLIC 传播**。今天 `src/android/` 不 include `<vulkan/vulkan.h>` 纯属幸运。 |
| `:459-475` | `add_library(magicblocks_jni SHARED … scene_renderer.cpp jni_bridge.cpp)` + `OUTPUT_NAME "magicblocks"` | JNI 层和「Android 壳」编在**一个 .so** 里；`magicblocks`（静态）被 `PRIVATE` 链进去 → 引擎代码被复制进 `libmagicblocks.so`，**无法单独替换 SDK**，也无法给另一个 UI（未来的 Windows UWP/WinUI）复用。 |
| `:130-267` 源码列表 | 把 L0/L1/L2/L3/L4 全部塞进**同一个 target** | 拆分以后必须区分「公开的 C ABI 面」与「内部实现」 |
| `android/app/build.gradle:29-34` | `externalNativeBuild { cmake { path '../../CMakeLists.txt' } }` | App 直接编引擎全树（见 C10）。 |

---

## 3. 耦合面全景图（现状 → 目标）

```text
现状（编译期强耦合，运行期 JNI）

  Kotlin (EngineSurfaceView / MbRenderer / MainActivity)
        │  12 × external fun + 1 × 反向 logNative + loadLibrary("magicblocks")
        ▼
  libmagicblocks.so = [ jni_bridge.cpp + scene_renderer.{h,cpp} ]
        │  #include "magicblocks/device/render_device.h" "swapchain.h"
        │  #include "magicblocks/resources/{buffer,command,pipeline}.h"
        │  #include "magicblocks/primitives/{types,model_catalog}.h"
        │  #include "backend/vulkan/vulkan_render_device.h"   ← ★ backend 私有
        │  dynamic_cast<backend::VulkanRenderDevice/VulkanSwapchain>  ← ★ RTTI 下钻
        ▼
  [ magicblocks STATIC: L0 device + L1 resources + L2 scene + L3 primitives + L4 features + Vulkan backend ]


目标（编译期只认一个纯 C 头，运行期只认一个动态库）

  Kotlin (EngineSurfaceView / MbRenderer / MainActivity)          Windows/WinUI/其他 UI
        │  System.loadLibrary("magicblocks")                            │ LoadLibrary("magicblocks.dll")
        │  ↓（可选）薄 JNI 适配层 mb_jni.cpp：只做 jstring↔const char*、C 回调→Kotlin 回调
        ▼                                                               ▼
  ╔══════════════════════════════════════════════════════════════════════════╗
  ║  magicblocks.dll / libmagicblocks.so                                        ║
  ║  公开面：include/mb_capi.h（唯一 PUBLIC include；纯 C99；不透明句柄）        ║
  ║  ├─ mb_engine_*      引擎对象（等价今天的 SceneRenderer 句柄）               ║
  ║  ├─ mb_surface_*     Surface/窗口句柄的包装与引用计数                        ║
  ║  ├─ mb_pose_*        姿态快照（定长 struct，按值传）                         ║
  ║  ├─ mb_model_*       模型目录扫描/加载/清除                                  ║
  ║  ├─ mb_status_*      状态串、fps 快照、错误码 + 消息                         ║
  ║  └─ mb_log_cb        日志回调（替代 logNative 反射）                         ║
  ║  内部面：L0 device / L1 resources / L2 scene / L3 primitives / backend      ║
  ║          （SHARED 导出符号只允许 mb_*；其余全部 hidden）                      ║
  ╚══════════════════════════════════════════════════════════════════════════╝
MB_STATIC_ASSERT_SIZE(mb_engine_desc, 40);
        │  PRIVATE 链接（Vulkan / VMA / tess2 全部不在 PUBLIC 接口里）
        └── Vulkan::Vulkan / vulkan（NDK）
```

---

## 4. 「UI 允许知道的面」与「仅 SDK 内部」的硬边界

### 4.1 UI 允许知道（= `mb_capi.h` 的全部内容，**不多不少**）

| 类别 | 允许的形态 | 说明 |
|---|---|---|
| **Surface 句柄** | `mb_surface`（不透明指针），由 `mb_surface_wrap_android(ANativeWindow*)` / `mb_surface_wrap_win32(HWND)` 产生 | UI 只知道「我有一个 surface」，**不知道它是 `ANativeWindow*`、`HWND`、`HANDLE` 还是 `VkSurfaceKHR`** |
| **引擎句柄** | `mb_engine`（不透明指针） | UI 只知道「我有一个引擎」 |
| **姿态参数** | `mb_pose`（按值传的定长结构体：`float q[4]; float scale; float pan[2];`） | 语义固定为「相机单位四元数(qx,qy,qz,qw) + 缩放 + 世界空间平移」，与 `scene_renderer.h:67-79` 一致 |
| **状态字符串** | `char* buf + size_t buf_size`（调用方缓冲区） | 只保证「UTF-8、NUL 结尾、可能被截断」，UI **不得解析**（要解析的数据走 fps 快照/错误码/枚举） |
| **fps 快照** | `mb_perf_snapshot`（定长结构体，按值返回） | `fps / frame_ms_avg / frame_ms_max / render_fail_ratio / msaa_effective / frames_total` |
| **模型列表** | `mb_model_entry`（定长结构体）+ 迭代器 `mb_model_list` | `name / path / bytes / ok / message[128]`。**不再有 `\t` 文本协议** |
| **错误** | `mb_err`（枚举）+ `mb_err_message(engine, buf, size)` | 成功 = `MB_OK`；每个失败入口都必须能拿到原因 |
| **版本** | `MB_CAPI_VERSION_MAJOR/MINOR/PATCH` + `mb_capi_version_*()` + 每函数 `struct_size` | 见 §5.3 |
| **日志** | `mb_log_cb` 函数指针 + `mb_engine_set_log_cb` | 替代 `logNative` 反射 |

**允许 UI 知道的平台类型：只有一种** —— `mb_surface_wrap_android` 的 `void* native_window` / `mb_surface_wrap_win32` 的 `void* hwnd`。这两个入口**本来就只能被对应平台的 UI 调用**，且参数是 `void*`（不是 `ANativeWindow*`），所以头文件仍然只 include `<stdint.h>`/`<stddef.h>`。**如果希望头文件里连 `void* native_window` 都不出现**，可以进一步改成 `mb_surface_wrap(void* native_handle, mb_surface_kind kind)`（见 5.4 的备选方案）。

### 4.2 仅 SDK 内部（**UI 绝不能拿到**）

| 禁止项 | 出处（现状） | 替代路径 |
|---|---|---|
| `VkDevice` / `VkQueue` / `VkInstance` / `VkPhysicalDevice` / `VkSurfaceKHR` / `VkSwapchainKHR` / 任何 `Vk*` | `backend/vulkan/*` | UI 完全不需要；`pre_transform` 这类信息由 SDK 内部消化（见 4.3） |
| `backend::*` 命名空间（`VulkanRenderDevice`、`VulkanSwapchain`、`AttachmentShape`） | `scene_renderer.cpp:16-17, 248, 261, 273, 303` | `dynamic_cast` 全部下沉进 SDK：给 `ISwapchain` 补 `msaa_samples()` / `pre_transform()`，或新增 `create_swapchain_ex(extent, SwapchainOptions{msaa, …})` |
| `device::IRenderDevice*` / `ISwapchain*` / `ICommandList*` / `IBuffer*` / `IPipeline*` / `ITexture*` / `IRenderTarget*` / `IMemoryAllocator*` / `IFrameSync*` | `include/magicblocks/**`（58 个头） | 只有 `mb_engine` 一个不透明指针 |
| `primitives::ILoadedModel` / `IMesh` / `Vec3` / `Mat4` / `ModelEntry` | `scene_renderer.h:135`、`model_catalog.h` | `mb_model_list` / `mb_model_entry` |
| `device::Status` / `Result<T>`（模板 + `std::string`） | 全部公开头 | `mb_err` 枚举（纯整数） |
| `std::string` / `std::vector` / `std::unique_ptr` / 任何模板 | 全部公开头 | 调用方缓冲区 + 迭代器 |
| `ShaderModule` / `ShaderStage` / `PipelineDesc` / `VertexAttribute` / `RenderPassDesc` / `PushConstants` | `resources/pipeline.h`、`scene_renderer.h:38-48` | 全在 SDK 内；管线按「目标形状」自派生 |
| `ANativeWindow*` 的**所有权语义** | `jni_bridge.cpp:220-234` + `scene_renderer.h:138-140` | UI 只做 `mb_surface_wrap` / `mb_surface_release`，**引用计数由 SDK 管** |
| `magicblocks/primitives/model_catalog.h` 的 `ModelScanOptions` | `jni_bridge.cpp:161` | 选项走固定默认值 + 版本化结构体 |

### 4.3 「旋转补偿」这条特殊边界

今天 `scene_renderer.cpp:271-279` 从 `VkSurfaceTransformFlagBitsKHR` 推出 `rotation_deg_`，用它（a）修正投影宽高比（`:396-408`）；（b）**间接提醒 UI 手势该怎么换算** —— 但 UI 那边用的是 **`Display.getRotation()`**（`EngineSurfaceView.kt:116,455-460`、`MainActivity.kt:243-254`），**两个旋转量来源不同、算法不同**（一个是 surface transform，一个是 display rotation），今天一致纯属巧合。

拆分后的边界：

* **SDK 内部**：surface transform → 投影修正，对 UI 不可见。
* **UI**：手势坐标换算用自己的 `Display.getRotation()`（这是 Android View 的职责）。
* **不通过 C ABI 传递旋转量**。若将来需要，用 `mb_status_get_rotation(mb_engine, int32_t* out_degrees)` 显式给出，而不是让 UI 猜。

---

## 5. `mb_capi.h` DRAFT

设计约束（全部来自任务要求，逐条对应）：

| 要求 | 在草案里怎么落实 |
|---|---|
| 纯 C（无 `std::`/模板/引用） | 只用 `struct`/`enum`/`typedef`/函数指针；**没有 `namespace`、没有 `template`、没有 `&`、没有 `class`** |
| 只 include `<stdint.h>` / `<stddef.h>` | 头文件第 1-3 行，无其它 include（**没有 `<stdbool.h>`**：用 `uint8_t` 当布尔，避免 C++/C 的 `bool` 大小分歧） |
| 结构体按值传、大小固定 | 所有 `struct` 只用定长标量与定长数组；**没有指针成员**（除日志回调的 `struct_size` 与 user 指针，见 5.3）；`MB_STATIC_ASSERT_SIZE` 宏钉住每个结构体尺寸 |
| 字符串 = `const char*` + 调用方缓冲区 | `mb_status_get_text(engine, char* buf, size_t buf_size)` 返回**实际长度**（不含 NUL），保证可截断重试；`mb_model_entry.name` 是定长 `char[N]` |
| 版本号 | `MB_CAPI_VERSION_MAJOR/MINOR/PATCH` + `mb_capi_version()` 返回的 `mb_version` 结构体（按值） |
| 错误码枚举 | `mb_err`（含「错误码 → 稳定字符串」的 `mb_err_str`）；**每个入口的返回值都是 `mb_err`**（`mb_handle` 类入口用 out 参数） |
| 不透明句柄 | `mb_engine`、`mb_surface`、`mb_model_list` 三个 `struct mb_*_s*` 前向声明 + `typedef ... * mb_x;` |

### 5.1 头文件草案（可直接落盘为 `MagicBlocks/include/mb_capi.h`）

```c
/*
 * mb_capi.h —— MagicBlocks 稳定的 C ABI（UI ⇄ SDK 的唯一契约）
 *
 * 规则（违反即返工）：
 *   1. 本文件是**纯 C99**：不得出现 std::、模板、引用、class、异常、RTTI。
 *   2. 本文件只 include <stdint.h> 与 <stddef.h>。加任何其它 include 都是破坏。
 *   3. 所有 struct 按值传递、大小固定、**不含指针成员**（除 §5.3 说明的回调与 user 指针）。
 *   4. 所有“可能很长”的字符串一律走 “调用方缓冲区 + 返回所需长度” 模式。
 *   5. 所有对不透明句柄的操作都返回 mb_err；mb_err 的字符串形式由 mb_err_str 给出（永不返回 NULL）。
 *   6. 句柄是**不透明**的：调用方不得解引用、不得做指针算术、不得跨库 free。
 *
 * 版本策略：Major 变 = 破坏性变更（删/改函数或结构体字段）；
 *           Minor 变 = 只增不改（新增函数、在结构体**尾部**追加字段并同时提升 struct_size）；
 *           Patch 变 = 纯修复，ABI 不变。
 */
#ifndef MB_CAPI_H
#define MB_CAPI_H

#include <stddef.h>
#include <stdint.h>

/* ======================================================================
 * 0. 版本
 * ====================================================================== */

#define MB_CAPI_VERSION_MAJOR 1
#define MB_CAPI_VERSION_MINOR 0
#define MB_CAPI_VERSION_PATCH 0

/* 编译期“这个头文件版本支持不低于 X”的断言（调用方可选写） */
#define MB_CAPI_AT_LEAST(maj, min) \
  (MB_CAPI_VERSION_MAJOR > (maj) || (MB_CAPI_VERSION_MAJOR == (maj) && MB_CAPI_VERSION_MINOR >= (min)))

typedef struct mb_version {
  uint32_t major;         /* 破坏性变更时 +1 */
  uint32_t minor;         /* 只增不改时 +1 */
  uint32_t patch;         /* 纯修复 */
  uint32_t capi_struct_size; /* = sizeof(mb_version)，供未来在尾部追加字段 */
} mb_version;

/* 返回加载到的**运行时**版本。UI 应当在 loadLibrary 之后第一件事调它，
 * 与 MB_CAPI_VERSION_* 比对；major 不一致直接报错，不要继续调用其它函数。 */
mb_version mb_capi_version(void);

/* ======================================================================
 * 1. 导出/调用约定 + 结构体尺寸钉子
 *
 * ⚠️ 这一节必须放在**所有函数声明之前**：函数返回类型要用 mb_version / mb_err，
 *    而它们在 C 里是 typedef。宏名刻意用 MB_API / MB_CALL 而不是 MB_API ——
 *    后者会和「C 库名 MagicBlocks ⇒ magicblocks_capi」的直觉撞在一起，
 *    而且一旦以后有人写 `MB_API mb_err foo(void);` 就会在 C 模式下炸
 *    （typedef 名当类型、宏名当修饰符，两者不可混用）。
 * ====================================================================== */

#if defined(_WIN32)
#  define MB_API_EXPORT __declspec(dllexport)
#  define MB_API_IMPORT __declspec(dllimport)
#  define MB_CALL __cdecl
#else
#  define MB_API_EXPORT __attribute__((visibility("default")))
#  define MB_API_IMPORT __attribute__((visibility("default")))
#  define MB_CALL
#endif

/* 构建本库时由 CMake 定义 MB_CAPI_BUILDING_LIBRARY=1 */
#if defined(MB_CAPI_BUILDING_LIBRARY)
#  define MB_API MB_API_EXPORT
#else
#  define MB_API MB_API_IMPORT
#endif

/* 结构体尺寸的编译期钉子：换编译器/换 ABI 立刻炸，而不是运行期悄悄错位。
 *
 * 注意 C 分支：**不能**用 `##__LINE__`，那在标准 C 里是未定义行为（MSVC 也会报
 * 「宏参数列表中出现预处理器指令 / 无法终止」）。改成「两个同名 typedef 重定义」
 * —— 条件成立时两处 typedef 完全一致（合法），不成立时一处是 char、一处是
 * char[-1]（必然报错）。这是纯标准 C 的写法。
 *
 * 调用形式：`MB_STATIC_ASSERT_SIZE(type, expected)`（第一个参数是**类型名**，不是
 * sizeof 表达式 —— 因为要拼 typedef 名）。含指针的结构体尺寸随位宽变，用三目即可。 */
#if defined(__cplusplus)
#  define MB_STATIC_ASSERT_SIZE(type, expected) static_assert(sizeof(type) == (expected), #type " 尺寸不符")
#else
#  define MB_STATIC_ASSERT_SIZE(type, expected) \
    typedef char mb_size_check_##type[(sizeof(type) == (expected)) ? 1 : -1]; \
    typedef char mb_size_check_##type[(sizeof(type) == (expected)) ? 1 : -1]
#endif

/* ======================================================================
 * 2. 错误码
 * ====================================================================== */

typedef enum mb_err {
  MB_OK = 0,                /* 成功 */
  MB_E_INVALID_ARG = 1,     /* 参数非法（空指针/越界/尺寸 0） */
  MB_E_NOT_INITIALIZED = 2, /* 引擎尚未初始化完成 */
  MB_E_NO_SURFACE = 3,      /* 还没有 attach surface，或已被 detach */
  MB_E_SURFACE_LOST = 4,    /* surface 已失效（surfaceDestroyed），需重新 attach */
  MB_E_OUT_OF_DATE = 5,     /* 交换链与 surface 不匹配，SDK 内部会重建；UI 可忽略本帧 */
  MB_E_DEVICE_LOST = 6,     /* GPU 设备丢失（不可恢复，需重建引擎） */
  MB_E_UNSUPPORTED = 7,     /* 当前平台/后端不支持该能力 */
  MB_E_OUT_OF_MEMORY = 8,   /* 显存/内存预算不足 */
  MB_E_IO = 9,              /* 文件读写失败（模型/资产） */
  MB_E_NOT_FOUND = 10,      /* 目录/文件不存在（与“空目录”严格区分） */
  MB_E_BUSY = 11,           /* 该句柄正被渲染线程使用，调用方应稍后重试 */
  MB_E_VERSION_MISMATCH = 12, /* 头文件与库的 major 不一致 */
  MB_E_INTERNAL = 13,       /* SDK 内部缺陷（消息里带具体断点位置） */
  MB_E__COUNT = 14          /* 仅供 mb_err_str 做边界检查，不是真实错误码 */
} mb_err;

/* 错误码 → 稳定英文标识（"MB_E_NO_SURFACE" 等），**永不返回 NULL**。
 * 刻意用稳定英文而不是本地化文案：UI 负责翻译，SDK 不产出面向用户的措辞。 */
MB_API const char* MB_CALL mb_err_str(mb_err e);

/* ======================================================================
 * 3. 不透明句柄
 * ====================================================================== */

/* 三个句柄族互不通用：把 mb_surface 传给 mb_engine_* 必须被拒绝（MB_E_INVALID_ARG）。 */
typedef struct mb_engine_s*     mb_engine;      /* 引擎（等价今天的 SceneRenderer 句柄） */
typedef struct mb_surface_s*    mb_surface;     /* 平台 Surface/窗口的 SDK 侧包装 */
typedef struct mb_model_list_s* mb_model_list;  /* 目录扫描结果（只读，用完 mb_model_list_free） */

/* 句柄有效性检查：非 NULL 且类型标签正确才算有效。
 * 用它代替 C++ 的例外情况判空 —— 永不崩溃，永不 UB。 */
MB_API uint8_t MB_CALL mb_engine_is_valid(mb_engine e);
MB_API uint8_t MB_CALL mb_surface_is_valid(mb_surface s);

/* ======================================================================
 * 4. 日志回调（替代 JNI 反射调 logNative）
 * ====================================================================== */

typedef enum mb_log_level {
  MB_LOG_DEBUG = 0,
  MB_LOG_INFO = 1,
  MB_LOG_WARN = 2,
  MB_LOG_ERROR = 3
} mb_log_level;

/* level / msg 只在回调期间有效；msg 永不 NULL，UTF-8、NUL 结尾。
 * user 是注册时透传的指针，SDK 不解引用、不释放。 */
typedef void (MB_CALL * mb_log_cb)(void* user, mb_log_level level, const char* msg);

typedef struct mb_log_cb_desc {
  uint32_t   struct_size; /* = sizeof(mb_log_cb_desc)；尾部追加字段时用来判可用范围 */
  uint32_t   reserved0;
  mb_log_cb  cb;          /* NULL = 注销 */
  void*      user;
} mb_log_cb_desc;

/* 含指针 → 尺寸随位宽变：x64 = 24（2×u32 + 函数指针 + void*，指针 8 字节对齐）
 *                         x86 = 16（全部 4 字节对齐） */
MB_STATIC_ASSERT_SIZE(mb_log_cb_desc, sizeof(void*) == 8 ? 24 : 16);

MB_API mb_err MB_CALL mb_engine_set_log_cb(mb_engine e, const mb_log_cb_desc* desc);

/* ======================================================================
 * 5. Surface 包装
 * ====================================================================== */

typedef enum mb_surface_kind {
  MB_SURFACE_ANDROID = 1, /* native_handle = ANativeWindow*（Kotlin: Surface → nativeWindowFromSurface） */
  MB_SURFACE_WIN32 = 2,   /* native_handle = HWND */
  MB_SURFACE_XCB = 3,     /* native_handle = xcb_window_t 的 (uintptr_t) */
  MB_SURFACE_HEADLESS = 4 /* 离屏/自测；native_handle 忽略 */
} mb_surface_kind;

typedef struct mb_surface_desc {
  uint32_t       struct_size;   /* = sizeof(mb_surface_desc) */
  uint32_t       kind;          /* mb_surface_kind */
  void*          native_handle; /* 见 mb_surface_kind 的注释；SDK 会自己 +1 引用 */
  uint32_t       width;         /* 像素宽，>0 */
  uint32_t       height;        /* 像素高，>0 */
  uint32_t       reserved0;
} mb_surface_desc;

/* 含指针 → x64 = 32，x86 = 24（指针 8→4 字节，并少一处尾部 padding）。
 * **这是唯一一个尺寸随位宽变化的结构体**，调用方必须用 struct_size 而不是常量 sizeof。 */
MB_STATIC_ASSERT_SIZE(mb_surface_desc, sizeof(void*) == 8 ? 32 : 24);

/* 把平台 surface 包成 mb_surface：**SDK 取得自己的引用**（Android 上会调 ANativeWindow_acquire）。
 * 成功时 *out 非 NULL；失败时 *out == NULL 且返回对应错误码。
 * 调用方**不需要**自己 release 平台句柄 —— 若调用方之前自己 fromSurface 拿了引用，
 * 由调用方自己 release，SDK 只持有它自己那一份。 */
MB_API mb_err MB_CALL mb_surface_wrap(const mb_surface_desc* desc, mb_surface* out);

/* 释放 SDK 持有的那份表面引用。对 NULL 是 no-op；重复调用返回 MB_OK（幂等）。 */
MB_API mb_err MB_CALL mb_surface_release(mb_surface s);

/* 查询 surface 尺寸（像素）。尺寸变化时 SDK 会收到 SurfaceChanged 事件。
 * 返回的尺寸是**交换链 image 空间**的尺寸，不是屏幕上旋转后的尺寸。 */
MB_API mb_err MB_CALL mb_surface_size(mb_surface s, uint32_t* out_w, uint32_t* out_h);

/* ======================================================================
 * 6. 引擎生命周期
 * ====================================================================== */

typedef enum mb_msaa_policy {
  MB_MSAA_AUTO = 0,   /* SDK 自选（当前实现：请求 4x，不支持则降级） */
  MB_MSAA_OFF = 1,    /* 1x */
  MB_MSAA_4X = 2,
  MB_MSAA_8X = 3
} mb_msaa_policy;

typedef struct mb_engine_desc {
  uint32_t         struct_size;      /* = sizeof(mb_engine_desc) */
  uint32_t         msaa_policy;      /* mb_msaa_policy */
  const char*      app_name;         /* 可 NULL → "MagicBlocks" */
  uint32_t         requested_scene;  /* 预留：场景 id；0 = 默认场景 */
  uint32_t         reserved0;
  uint64_t         reserved1[2];
} mb_engine_desc;

/* 含指针 → x64 = 40、x86 = 40（**恰好相同**：末尾 2×u64 让 32 位下的尾部 padding
 * 也被算进 size，32 位下的 4 个 u32 正好把指针让出的 4 字节吃掉）。
 * 这不是巧合，是刻意：**位宽无关**的结构体才算真正“大小固定”。 */
MB_STATIC_ASSERT_SIZE(mb_engine_desc, 40);

/* 创建引擎（建设备 + 默认场景资源）。
 *
 * 与今天的 nativeCreate 不同：**失败时 *out == NULL 且 err 非 MB_OK**，
 * 失败原因可用 mb_engine_last_error_message(e) 读到 —— 所以失败时仍然会返回一个
 * “只读错误句柄”，调用方必须 mb_engine_destroy 它（见 *out 规则）。
 *
 * *out 规则：
 *   MB_OK                  → 完整可用引擎；
 *   MB_E_NOT_INITIALIZED   → 只读错误句柄（status / last_error 可用，其余入口返回 MB_E_NOT_INITIALIZED）；
 *   其它错误码              → *out == NULL。
 */
MB_API mb_err MB_CALL mb_engine_create(const mb_engine_desc* desc, mb_engine* out);

/* 销毁引擎。内部保证：先与正在进行的 mb_engine_render 串行化（最多等一帧），
 * 再释放交换链、资源、设备、表面引用。对 NULL 是 no-op，重复调用幂等。
 * 调用之后句柄立即失效，任何其它入口对它都返回 MB_E_INVALID_ARG（不 UB）。 */
MB_API mb_err MB_CALL mb_engine_destroy(mb_engine e);

/* ======================================================================
 * 7. 姿态（按值传的定长结构体 —— 这是 UI 唯一需要构造的“数据”）
 * ====================================================================== */

typedef struct mb_pose {
  float q[4];   /* 相机单位四元数 (qx, qy, qz, qw)：相机系 → 世界系。SDK 内部会归一化 */
  float scale;  /* 缩放（双指捏合）> 0；<=0 视为 1 */
  float pan[2]; /* 世界空间平移 (x, y) —— 由 UI 按“屏幕像素 / 当前可视宽度”换算好 */
} mb_pose;

MB_STATIC_ASSERT_SIZE(mb_pose, 28); /* 7 × float，位宽无关 */

/* 姿态的默认值（q = 单位四元数，scale = 1，pan = 0）。 */
MB_API mb_pose MB_CALL mb_pose_default(void);

/* ======================================================================
 * 8. 渲染
 * ====================================================================== */

/* 绑定 surface 并建交换链。可重复调用（等价今天的 detach + attach），
 * 调用时 SDK 会先停下内部正在进行的渲染（同步，最多等一帧）。
 * 交换链按引擎当前的 MSAA 设置创建；实际生效值用 mb_engine_msaa_effective 查询。 */
MB_API mb_err MB_CALL mb_engine_attach_surface(mb_engine e, mb_surface s);

/* 解绑 surface。返回后 SDK 保证不会再碰该 surface（无需 UI 侧等线程）。 */
MB_API mb_err MB_CALL mb_engine_detach_surface(mb_engine e);

/* 渲染并呈现一帧。**线程安全**：可在 UI 线程或任意单一渲染线程调用。
 *
 * 返回值语义（与原 nativeRenderFrame:Boolean 对齐并细化）：
 *   MB_OK            → 已呈现；
 *   MB_E_NO_SURFACE  → 未 attach，未做任何事；
 *   MB_E_OUT_OF_DATE → 交换链已内部重建，本帧丢弃（UI 忽略，下一帧自然好）；
 *   MB_E_SURFACE_LOST→ surface 已死，UI 应当停止循环并等待新的 surfaceCreated；
 *   其它             → 真错误，原因见 mb_engine_last_error_message。
 *
 * **不超时黑洞**：内部对每个等待都有上界，超时返回 MB_E_BUSY 而不是永久阻塞。 */
MB_API mb_err MB_CALL mb_engine_render(mb_engine e, mb_pose pose);

/* 请求渲染线程“下一拍重画一次”（不阻塞）。给“点按钮改状态但没有姿态变化”的场景用。 */
MB_API mb_err MB_CALL mb_engine_request_frame(mb_engine e);

/* 引擎可用性：MB_OK = 完整可用；其它 = 不可用（消息给出原因）。 */
MB_API mb_err MB_CALL mb_engine_ready(mb_engine e);

/* ======================================================================
 * 9. MSAA（请求值 / 生效值 / 设备支持的档位）
 * ====================================================================== */

/* 设置**请求**的 MSAA 档位。回退规则同今天：设备不支持时降级，不失败。
 * 生效时机：下一次 attach_surface（与今天一致）。 */
MB_API mb_err MB_CALL mb_engine_set_msaa(mb_engine e, mb_msaa_policy policy);

/* 实际生效的采样数（1/2/4/8/16）。UI 必须显示这个值，而不是请求值。 */
MB_API mb_err MB_CALL mb_engine_msaa_effective(mb_engine e, uint32_t* out_samples);

/* 设备报告的受支持档位（升序，至少含 1）。无设备时只给 {1}。
 * 调用两次模式：buf 可为 NULL 得到所需个数。 */
MB_API mb_err MB_CALL mb_engine_msaa_supported(mb_engine e,
                                                    uint32_t* out_buf, size_t buf_capacity,
                                                    size_t* out_count);

/* ======================================================================
 * 10. 状态字符串 / 性能快照
 * ====================================================================== */

/* 写 status 文本到调用方缓冲区。
 *   buf == NULL 或 buf_size == 0 → 只返回所需长度（含结尾 NUL），*out_written 可为 NULL；
 *   buf 不够长                    → 写入 buf_size-1 字节 + NUL，返回所需长度，
 *                                    调用方可按返回值重新分配再调一次。
 * 返回的文本**仅供显示**：SDK 不保证其格式；需要结构化数据请用 mb_perf_snapshot / mb_err。 */
MB_API mb_err MB_CALL mb_engine_status_text(mb_engine e, char* buf, size_t buf_size,
                                                 size_t* out_required);

/* 最近一个统计窗口的性能快照。**按值返回**，无分配、无锁（内部是原子快照）。 */
typedef struct mb_perf_snapshot {
  uint32_t struct_size;      /* = sizeof(mb_perf_snapshot) */
  uint32_t msaa_effective;   /* 本窗口内的生效采样数 */
  float    fps;              /* 成功呈现帧数 / 窗口秒数 */
  float    frame_ms_avg;     /* 每帧 render 占用（SDK 侧计时） */
  float    frame_ms_max;
  float    render_fail_ratio;/* 0.0 ~ 1.0 */
  uint64_t frames_total;     /* 累计成功呈现帧数 */
  uint64_t reserved0;
} mb_perf_snapshot;

MB_STATIC_ASSERT_SIZE(mb_perf_snapshot, 40); /* 5×4B + 4×4B + 2×u64 = 40，8 字节对齐，位宽无关 */

MB_API mb_perf_snapshot MB_CALL mb_engine_perf(mb_engine e);

/* 最近一次失败的原因（人类可读、UTF-8）。永不清空历史成功记录之外的东西。 */
MB_API mb_err MB_CALL mb_engine_last_error_message(mb_engine e, char* buf, size_t buf_size,
                                                       size_t* out_required);

/* ======================================================================
 * 11. 预设模型
 * ====================================================================== */

#define MB_MODEL_NAME_MAX 64   /* 定长字段；超出则截断并保证 NUL */
#define MB_MODEL_MSG_MAX  128
#define MB_MODEL_PATH_MAX 252  /* 252 = 464 - (64+128+8+8) 且是 4 的倍数；避开 Win32 MAX_PATH 的 260 边界 */

typedef struct mb_model_entry {
  char     name[MB_MODEL_NAME_MAX];      /* UI 只显示它 */
  char     message[MB_MODEL_MSG_MAX];    /* ok==0 时的原因 */
  char     path[MB_MODEL_PATH_MAX];      /* 便于日志/排错；UI 不应依赖其格式 */
  uint64_t bytes;
  uint32_t ok;                           /* 0 = 扫描阶段就不可用 */
  uint32_t reserved0;
} mb_model_entry;

MB_STATIC_ASSERT_SIZE(mb_model_entry, 464); /* 64+128+252+8(字节数)+8(两个 u32)，位宽无关 */

/* 扫描目录（**不依赖引擎**，无句柄必需；传 NULL 引擎也行，但推荐传以共享日志设置）。
 * “目录不存在” → MB_E_NOT_FOUND，“空目录” → MB_OK + count==0（两者的区分必须在错误码上）。 */
MB_API mb_err MB_CALL mb_models_scan(mb_engine e_or_null, const char* directory_utf8,
                                          mb_model_list* out);

MB_API uint32_t MB_CALL mb_model_list_count(mb_model_list list);

/* 取第 index 个条目（**按值写进调用方结构体**，不返回内部指针）。 */
MB_API mb_err MB_CALL mb_model_list_get(mb_model_list list, uint32_t index,
                                             mb_model_entry* out);

MB_API mb_err MB_CALL mb_model_list_free(mb_model_list list);

/* 切到 path 指定的 OBJ。name 只用于日志/状态（可 NULL）。
 * 失败时**渲染保持原状**（不回落立方体、不留半成品），原因在 last_error。 */
MB_API mb_err MB_CALL mb_engine_load_model(mb_engine e, const char* path_utf8,
                                                const char* name_utf8);

/* 切回内置立方体并释放模型资源。 */
MB_API mb_err MB_CALL mb_engine_clear_model(mb_engine e);

/* ======================================================================
 * 12. 自检：头文件与库版本是否匹配
 * ====================================================================== */

/* 调用方可用它一条语句校验：编译期头版本 vs 运行期库版本。
 * 不匹配返回 MB_E_VERSION_MISMATCH，并把双方版本写进 out。 */
MB_API mb_err MB_CALL mb_capi_check_version(mb_version* out_runtime);

#endif /* MB_CAPI_H */
```

### 5.2 为什么这样设计（逐条说明取舍）

| 决策 | 理由 / 备选被否的原因 |
|---|---|
| **只有 `mb_*` 前缀的导出符号** | 避免与 UI 侧同名符号冲突；Linux `.so` 默认导出全部符号，必须靠 `-fvisibility=hidden` + 显式 `__attribute__((visibility("default")))` 把导出面**收窄到 40 个左右**（用 `tools/check_capi_exports.*` 钉住，见 §7） |
| **句柄 = 不透明 `struct X_s*` 而不是 `uintptr_t`** | 今天 `reinterpret_cast<jlong>` 的裸指针无法区分 `mb_engine` 与 `mb_surface`。用不同 `struct` 类型让编译器在 C/C++ 侧帮忙挡错；运行期再加 `magic` 字段校验（`mb_engine_is_valid`）。**不采用「句柄 = 索引 + 世代号」**：那需要一张全局表 + 锁，热路径（每帧）不划算。 |
| **`mb_engine_create` 失败仍可能返回错误句柄** | 为了不丢今天「init 失败也能 `status()` 看原因」的语义；同时**明确区分**三种失败情况（完整可用 / 只读错误句柄 / NULL），比今天的「永远返回非 0 + 猜」严格 |
| **所有入口返回 `mb_err`** | 今天 `nativeSetMsaa` / `nativeSurfaceDestroyed` / `nativeDestroy` / `nativeClearModel` 都是 `void`，UI 完全不知道失败了。统一 `mb_err` 让「失败即报错」成为**可执行的契约**（对齐 SDK 已有的 D8 原则：`result.h:3-4`「失败即报错、不静默降级」） |
| **`mb_pose` 按值传** | 7 个 float = 28 字节，寄存器传；避免 UI 侧持有 SDK 分配的指针。**备选**：`const mb_pose*` —— 会引入「谁拥有这个结构」的问题，且与「按值传」硬要求冲突，弃用 |
| **字符串走「调用方缓冲区 + 返回所需长度」** | 避免 SDK 分配内存交给 C 侧 `free`（跨 CRT/跨分配器崩溃的经典写法），也避免 Java/Kotlin 侧额外一次 JNI 拷贝（`char[]` 直接读）。**不采用 `const char*` 返回内部指针**：调用方无法知道有效期，多线程下必踩 |
| **`mb_model_entry` 定长** | 替代今天的 `\t` 文本协议（C3/C4）。`path` 260 = Win32 `MAX_PATH`，Android 路径远短于它。**备选**：变长 + 双次调用 —— 会让「列表 200 个模型」变成 400 次 JNI 往返，否决 |
| **`mb_perf_snapshot` 按值返回** | 今天是 Kotlin 侧自己数帧（`EngineSurfaceView.kt:76-105`），把「fps 统计」放在 UI 里意味着 UI 承担了 SDK 的度量职责。移到 C ABI 后 UI 只显示 |
| **日志走注册回调而不是反射** | 消除 C2；回调在 `mb_engine_create` 之前注册也能用（`e == NULL` 走全局回调）。实现上 SDK 内部把 `mb_log_cb` 转成自己的日志接口，**不依赖任何 Kotlin 类名** |
| **`MB_STATIC_ASSERT_SIZE` 钉住每个结构体尺寸** | 换编译器（MSVC / Clang / NDK Clang）、换架构（arm64 / x64 / armv7）时，任何 padding 差异都会在**编译期**炸，而不是运行期错位。今天 `scene_renderer.cpp:25` 已经对 `PushConstants` 做过同样的事，这里只是推广到整个 ABI |
| **`struct_size` 字段** | 未来在结构体尾部追加字段时，旧 UI 传小的 `struct_size`，SDK 按 `struct_size` 判可用范围 → **Minor 版本无需重编 UI**。这是 ABI 稳定性的关键机制 |
| **没有 `<stdbool.h>`** | C 的 `_Bool` 是 1 字节、C++ 的 `bool` 也是 1 字节，但某些旧编译器/MSVC `/Zc` 选项会变。用 `uint8_t` / `uint32_t` 做布尔字段，**零歧义** |
| **`mb_surface_wrap` 需要 `void* native_handle`** | 这是唯一一处「平台相关」的入参。它被包在 `void*` 里，头文件本身仍然零平台依赖。**更激进的备选**（任务要求之外的 §5.4 讨论）是把这一步也移出 C ABI，由 JNI 适配层做 `ANativeWindow_fromSurface` + `mb_surface_from_native(void*)` |

### 5.3 结构体尺寸自检表（**已实测**，见 §5.6 的验证记录）

| 结构体 | 字段 | 实测 `sizeof`（MSVC） | 位宽无关？ |
|---|---|---|---|
| `mb_version` | 4 × `uint32_t` | 16 | ✔ |
| `mb_log_cb_desc` | `uint32_t × 2` + 函数指针 + `void*` | **24（x64）/ 16（x86）** | ✘ 随指针 |
| `mb_surface_desc` | `uint32_t × 2` + `void*` + `uint32_t × 3` + padding | **32（x64）/ 24（x86）** | ✘ 随指针 |
| `mb_engine_desc` | 2×`uint32` + `const char*` + 2×`uint32` + 2×`uint64` | **40（x64）/ 40（x86）** | ✔ *（巧合但稳定，见下） |
| `mb_pose` | 7 × `float` | 28 | ✔ |
| `mb_perf_snapshot` | `uint32 × 2` + `float × 4` + `float` + `uint64 × 2` | 40 | ✔ |
| `mb_model_entry` | `char[64]` + `char[128]` + `char[252]` + `uint64` + `uint32 × 2` | 464 | ✔ |

> **含指针的 2 个结构体尺寸随位宽变化**（32/64 位），所以断言写成
> `sizeof(void*) == 8 ? 32 : 24` 这类三目表达式。`mb_engine_desc` 在两宽度下**实测都是 40**
> （32 位下指针少 4 字节、尾部少 4 字节 padding，而 2×`uint64` 又强制 8 字节对齐补回来）——
> 数值稳定但**推理路径不同**，实现时不要依赖这个巧合，一律用 `struct_size`。
>
> **所有『热路径数据结构』（`mb_pose`、`mb_perf_snapshot`、`mb_model_entry`）都没有指针**，
> 布局位宽无关 —— 这是刻意的：它们要能按值跨语言传递，任何 padding 差异都会在真机上炸。

### 5.4 ABI 与现有实现的映射表（实现时的对照表）

| `mb_capi.h` 条目 | 今天对应物 | 迁移要点 |
|---|---|---|
| `mb_engine_create` | `nativeCreate` (`jni_bridge.cpp:52`) | 内部把 `SceneRenderer` 拆成「SDK 内部 `EngineImpl`」+「可选的 Android 场景插件」 |
| `mb_surface_wrap` | `nativeWindowFromSurface` (`:223`) + `nativeSurfaceCreated` (`:63`) | 拆成两步：**包装**（拿引用）与**绑定**（`mb_engine_attach_surface`）。UI 侧先 wrap 再 attach |
| `mb_engine_attach_surface` | `nativeSurfaceCreated` 后半 | 建交换链 + 按形状派生管线；**MSAA 参数来自引擎设置**，不是 attach 的参数（去掉了 C9 的「UI 实现 SDK 顺序」） |
| `mb_engine_detach_surface` + `mb_surface_release` | `nativeSurfaceDestroyed` (`:77`) | 今天引用计数由 JNI 层 + SceneRenderer 共同持有（互相耦合）；迁移后**只由 `mb_surface` 持有**，detach 只解绑不释放 |
| `mb_engine_render` | `nativeRenderFrame` (`:82`) | 7 个 `float` → `mb_pose`（按值）。返回值从 `Boolean` 变 `mb_err`（**有 `MB_E_OUT_OF_DATE` 这个新语义**） |
| `mb_engine_status_text` | `nativeStatus` (`:96`) | 字符串协议不变，但**位置不同**：不在契约里 |
| `mb_engine_destroy` | `nativeDestroy` (`:103`) | 加句柄失效标记（防 UB） |
| `mb_engine_set_msaa` / `msaa_effective` | `nativeSetMsaa` / `nativeMsaaSamples` (`:114`, `:121`) | 统一走 `uint32_t` + 单值枚举 |
| `mb_engine_msaa_supported` | `nativeSupportedMsaa` (`:129`) | **`"1,4,8"` 文本 → `uint32_t` 数组**（双次调用模式） |
| `mb_models_scan` / `mb_model_list_get` | `nativeListModels` (`:153`) | `\t` 文本 → 定长结构体数组 |
| `mb_engine_load_model` / `mb_engine_clear_model` | `nativeLoadModel` / `nativeClearModel` (`:185`, `:215`) | 失败保持原状语义不变 |
| `mb_log_cb_desc` + `set_log_cb` | `logNative` 反射 (`:28-46`) | 反射 → 回调；**不再依赖 Kotlin 类名** |
| `mb_capi_version` / `mb_capi_check_version` | 无 | 新增；`loadLibrary` 后第一件事 |
| `mb_engine_perf` | `EngineSurfaceView.currentPerf()`（纯 Kotlin 统计，`:76-105`） | 统计搬进 SDK |

### 5.5 迁移期兼容层（可选，建议）

为了「UI 一行不改也能先跑起来」，可以在 `libmagicblocks.so` **同一个产物里**保留一份极薄的 JNI shim（`src/capi/mb_jni_shim.cpp`），它**只**做：

```
Java_..._nativeRenderFrame → 把 7 个 jfloat 打成 mb_pose → mb_engine_render → 映射回 jboolean
```

但**它必须是可选编译单元**（`MB_BUILD_JNI_SHIM=ON`），只依赖 `mb_capi.h`，不 include 任何 SDK 内部头。这样：
- Android 可以先切到新 `.so`（行为不变），**再**逐条把 `MbRenderer` 的 `external fun` 换成 `mb_*`（此时通过 JNI 适配层调 `mb_*`）；
- 最后 UI 侧只剩 `mb_capi` 绑定（可选：用 JNA/JNI 生成绑定，或者用 Kotlin/Native cinterop）。

---

### 5.6 草案验证记录（写这份文档时实测的，不是推测）

把 §5.1 的代码块**原样抽出**成 `mb_capi.h`，用本机 MSVC（14.52.36725 / 19.4x）编译：

```
header extracted: 379 lines
PASS  MSVC x64 C11        (/std:c11   /W4 /WX)
PASS  MSVC x64 C17        (/std:c17   /W4 /WX)
PASS  MSVC x86 C11        (/std:c11   /W4 /WX)     ← 32 位分支的尺寸断言也通过
PASS  MSVC x64 C++14      (/std:c++14 /W4 /WX)
PASS  MSVC x64 C++17      (/std:c++17 /W4 /WX)
PASS  MSVC x64 C++20      (/std:c++20 /W4 /WX)
PASS  MSVC x86 C++20      (/std:c++20 /W4 /WX)
--- purity ---
PASS  only <stdint.h>/<stddef.h>
token std::[a-z]     occurrences=0
token \btemplate\b   occurrences=0
token \bnamespace\b  occurrences=0
token \bthrow\b      occurrences=0
token \bvirtual\b    occurrences=0
（\bclass\b 命中 1 处 = 文件头注释里“不得出现 class”那句，不是代码）
```

**负向验证**（证明断言真的会咬人，而不是形同虚设）：

| 用例 | 结果 |
|---|---|
| 故意把 `mb_bad`（`uint64_t` 单字段）断言成 4 字节 | C 分支 → `error C2118: 负下标`；C++ 分支 → `static_assert failed: 'mb_bad 尺寸不符'` ✔ |
| 32 位下把 `mb_engine_desc` 断言成 32（本轮第一版的真实错误） | C++ 分支 → `static_assert failed` ✔（说明三目表达式确实被求值，不是被忽略） |

> 这些验证也让草案在**写它的这一轮**就修掉了 3 个真实缺陷（否则会原样带进实现）：
> ① `MB_CAPI mb_version mb_capi_version(void)` 里**宏名与 typedef 名同形**，C 模式下必然炸 →
> 改成 `MB_API`/`MB_CALL`；② `##__LINE__` 拼 typedef 名是未定义行为（MSVC 报「宏参数列表中出现
> 预处理器指令」）→ 改成标准 C 的「同名 typedef 两次」写法；③ 3 个结构体尺寸随位宽变，单一常量
> 断言在 32 位下必错 → 改成三目。

### 5.7 落盘实现记录（`mb_capi.h` 已成为唯一公开契约）

§5.1 的草案已**落盘**为实现，路径与命名与草案有差异，**以实现为准**：

| 项 | 草案（§5.1） | 落盘实现 | 原因 |
|---|---|---|---|
| 路径 | `include/mb_capi.h` | **`MagicBlocks/include/magicblocks/mb_capi.h`** | 与 SDK 现有 `magicblocks/` 命名空间一致；`#include "magicblocks/mb_capi.h"` 明确「这是公有面」，且便于 CMake 用 `BUILD_INTERFACE` 只导出这一棵子树 |
| 句柄名 | `mb_engine` | **`mb_session`** | 任务要求用 session 语义；一个 session = 一次引擎生命周期（create → … → destroy），比 "engine" 更准确 |
| 创建参数 | `mb_engine_desc`（40B） | **`mb_session_desc`（48B）** | 多了 `reserved0`/`reserved1` 两个 u32，保证与指针字段对齐后**在 x64 与 x86 上都是 48 字节** |
| 版本查询 | `mb_version mb_capi_version(void)` | **`uint32_t mb_abi_version(void)`** + `size_t mb_abi_version_string(char*, size_t)` | 打包成 `MAJOR*10000 + MINOR*100 + PATCH` 的单个 `uint32_t`：可比较、可断言、不引入新结构体 |
| 导出宏 | `MB_API` / `MB_CALL` | 同名，另加 `MB_API_EXPORT` / `MB_API_IMPORT` 两级 | 由 `MB_CAPI_BUILDING_LIBRARY` 二选一；库内定义它 = dllexport，UI 侧不定义 = dllimport |
| 错误码 | `mb_err` | `mb_err`，`MB_OK=0` … `MB_E_INTERNAL=13`，`MB_E__COUNT=14` | 按 UI 真正需要区分的失败面梳理（surface 丢失 / 过期 / 设备丢失 / 缺目录 / 忙 / 版本不符） |
| 日志 | 无 | **`mb_set_log_callback(mb_log_cb_desc*)`** | 取代旧 JNI `logNative` 反射（见 §1.1、§1.4 C4）：库不再 `FindClass`/`GetStaticMethodID`，改为注册回调 |

**落盘版新增、草案没有的入口**（都是为了消掉 §1.4 里那些隐式耦合）：

| 入口 | 消掉的耦合 |
|---|---|
| `mb_abi_version` / `mb_abi_version_string` | C9「Kotlin 无法确认 native 版本」 |
| `mb_set_log_callback` | C4「`logNative` 字符串契约 + `ExceptionClear()` 静默吞错」 |
| `mb_session_is_valid` / `mb_model_list_is_valid` | C++ 异常/判空惯例 → `uint8_t`，UI 侧不必猜 |
| `mb_session_ready` | C6「create 成功不代表设备就绪」 |
| `mb_session_msaa_supported` | C7「UI 只能盲试 MSAA 档位」 |
| `mb_session_surface_resized` | C8「改尺寸必须卸载重挂」 |
| `mb_session_perf` | C10「fps/帧时只能靠日志文本扒」 |

#### 5.7.1 落盘版的验证（全部真实编译 + 真实链接 + 真实运行）

头文件：444 行，`MB_API` 入口 **26 个**，只 include `<stddef.h>` / `<stdint.h>`。

**① 编译矩阵**（MSVC，`/W4 /WX`，warnings-as-errors）：

```
PASS  x64 C11        PASS  x64 C17        PASS  x86 C11
PASS  x64 C++14      PASS  x64 C++17      PASS  x64 C++20     PASS  x86 C++20
```

**② 纯净度**（对落盘文件的实际扫描）：

```
# include 行：只 <stddef.h> / <stdint.h>，无 magicblocks/ 内部头
# 非注释代码里 std:: / template / class / namespace / throw / virtual / new / delete / try：0 处
# MB_API 声明：26 条，与导出表一致
```

**③ 导出表**（真的编译成 DLL 后 `dumpbin /exports`）：

```
26 exports，全部是未修饰的 C 名：
  mb_abi_version  mb_abi_version_string  mb_err_str  mb_model_list_count
  mb_model_list_free  mb_model_list_get  mb_model_list_is_valid  mb_pose_default
  mb_session_attach_surface  mb_session_clear_model  mb_session_create
  mb_session_desc_default  mb_session_destroy  mb_session_detach_surface
  mb_session_is_valid  mb_session_load_model  mb_session_msaa_effective
  mb_session_msaa_supported  mb_session_perf  mb_session_ready  mb_session_render
  mb_session_scan_models  mb_session_set_msaa  mb_session_status_text
  mb_session_surface_resized  mb_set_log_callback
（无 ?xxx@@YA 之类的 C++ 修饰名，无内部符号泄漏）
```

**④ 消费者程序**（证明「UI 侧只拿这个头就能干活」，不是只声明不调用）：

| 消费者 | 语言 | 结果 |
|---|---|---|
| `consumer.c` | 纯 C11，真实调用全部 26 个入口 + 断言契约语义 | **72 checks, 0 failed** |
| `cpp_consumer.cpp` | C++17/20，对**每个**入口取函数指针填表（强制签名逐个核对）+ `static_assert` 平凡可复制/标准布局 | **47 checks, 0 failed** |
| `jni_probe.cpp` | C++17，模拟 M5 的 JNI shim：ABI guard → 日志回调 → create → attach → 逐帧 render → MSAA 回读 → 模型列表 → detach → destroy | **22 checks, 0 failed** |
| `purity.c` | C11，契约层静态断言（版本打包自洽、错误码边界、热路径结构体无指针、模型条目布局） | 编译通过 |

契约语义被逐条断言（不是靠注释保证）：`mb_err_str` 永不 NULL（含越界值）；两段式查询返回**所需长度**且缓冲区过短时仍 NUL 结尾；「目录不存在」= `MB_E_NOT_FOUND` 而「空目录」= `MB_OK` + count 0；全 0 四元数 → `MB_E_INVALID_ARG`；卸载后 render → `MB_E_NO_SURFACE`；MSAA 越界值不被静默改写；已销毁句柄再传进来只报错不崩；`destroy`/`list_free` 对 NULL 幂等。

**⑤ 本轮落盘时抓到的真实缺陷（草案阶段看不出来的）**

1. **`extern "C"` 只有注释、没有代码块** —— 文件头第 23 行写着「用 C++ 编译时由 `extern "C"` 包裹」，
   但实际没有那段 `#if defined(__cplusplus) extern "C" { … }`。后果是 C++ 侧所有 26 个符号都拿到
   C++ 修饰名（`?mb_session_create@@YA…`），与 DLL 导出表里的未修饰 C 名对不上，**链接期直接 LNK2019**。
   是 `cpp_consumer.cpp` 第一次真实链接时暴露的（编译期完全看不出来）。
   已修复：`mb_capi.h:62-67` 加入 `extern "C"` 开块、`:441-443` 收块。
2. **`mb_session_desc` 尺寸断言写成 40** —— 从 `mb_engine_desc` 外推而非实测；
   实测 `0x030H` = **48**（x64 与 x86 同值）。7 个编译单元同时报
   `error C2118: 负下标` / `static assertion failed`。已改为 `MB_STATIC_ASSERT_SIZE(mb_session_desc, 48)`。

> 教训（写给实现队友）：结构体尺寸**必须实测**，不能从相似结构体推导；`extern "C"` 必须在
> **C++ 消费者真实链接**时才暴露问题，所以 M3 的验收里必须包含「C++ 程序链接 DLL 并运行」这一项，
> 只做「头文件能编译」是不够的。

#### 5.7.2 给 UI 侧的两条硬约束（写进 M5 的检查清单）

1. **取函数指针的写法**：MSVC 在 C 里不允许取 `dllimport` 函数的地址（`C4232`）。
   我们的 C 消费者因此静态链接 stub；C++ 消费者用 `MB_CAPI_BUILDING_LIBRARY` 让 `MB_API` 退化成普通声明。
   若 UI 需要保存函数指针表，同样处理；只是直接调用则无需任何改动。
2. **落地目录必须只有这一个头**：`include/` 下除 `mb_capi.h` 之外的 60 个内部头（`device/`、`resources/`、
   `primitives/`、`backend/`、`scene/`、`features/`、`ui/`）**必须**随着 M2/M3 移出 UI 可见范围，
   否则「UI 只依赖一个头文件」只是约定、不是事实。验证手段见 §6.3 的 `BUILD_INTERFACE` + G3 门禁加固。

## 6. 构建拆分方案（CMake）

### 6.1 目标产物

| 平台 | 产物 | 导出面 |
|---|---|---|
| Windows（MSVC / MinGW） | `magicblocks.dll` + `magicblocks.lib`（导入库） | 只有 `mb_*` |
| Android | `libmagicblocks.so`（arm64-v8a），打包进 APK 的 `jniLibs` | 只有 `mb_*` |
| Linux | `libmagicblocks.so` | 只有 `mb_*` |
| 所有平台 | `mb_capi.h`（**唯一** PUBLIC include） | — |

### 6.2 目标/目录结构

```
MagicBlocks/
├── include/
│   ├── mb_capi.h                       ← ★ 唯一的 PUBLIC 头（纯 C）
│   └── magicblocks/                    ← 仅内部使用（PRIVATE）
├── src/
│   ├── capi/                           ← ★ 新增：C ABI 实现层
│   │   ├── mb_capi_impl.cpp            ← 句柄校验、错误码映射、导出函数
│   │   ├── mb_handle.{h,cpp}           ← 句柄类型标签/世代号（内部）
│   │   ├── mb_jni_shim.cpp             ← 可选：旧 JNI 签名 → mb_*（仅 Android）
│   │   └── mb_export.cmake             ← 导出符号清单生成
│   ├── android/                        ← 只剩平台适配（→ 最终可空）
│   │   └── android_surface.cpp         ← ANativeWindow 包装（被 capi 调用）
│   ├── app/                            ← ★ 新增：把“旋转立方体/OBJ 场景”从 src/android 搬来
│   │   └── pad_scene.cpp/.h
│   ├── backend/  core/  features/  primitives/  scene/  ui/  cli/ …（原地不动）
│   └── CMakeLists.txt（或根 CMakeLists 内联）
├── CMakeLists.txt
└── android/app/build.gradle             ← 改为依赖预编好的 .so / 或仍用 CMake 但 target 拉新
```

### 6.3 CMake 改法（草案，**本轮不落盘**）

```cmake
# ─────────────────────────────────────────────────────────────
# 1) 内部核心库：改成 OBJECT 库，供 SHARED/DLL 与各 CLI 复用
#    关键改动：include / Vulkan / VMA / tess2 全部 PRIVATE
# ─────────────────────────────────────────────────────────────
add_library(magicblocks_core OBJECT
  # ……… 原有的 src/backend src/core src/scene src/ui src/primitives src/features 列表 ………
)
target_include_directories(magicblocks_core PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/include          # 内部头，**不进 PUBLIC**
  ${CMAKE_CURRENT_SOURCE_DIR}/src
  ${CMAKE_CURRENT_BINARY_DIR}
  ${MB_VMA_INCLUDE}
  ${_tess2_inc})
target_link_libraries(magicblocks_core PRIVATE
  $<IF:$<PLATFORM_ID:Android>,vulkan,Vulkan::Vulkan>
  ${_tess2_lib})
target_compile_definitions(magicblocks_core PRIVATE
  $<IF:$<PLATFORM_ID:Android>,VK_USE_PLATFORM_ANDROID_KHR;MB_ANDROID_USE_NDK,
     $<IF:$<PLATFORM_ID:Windows>,VK_USE_PLATFORM_WIN32_KHR,VK_USE_PLATFORM_XCB_KHR>>)

# ─────────────────────────────────────────────────────────────
# 2) 公开动态库：magicblocks.dll / libmagicblocks.so
#    ★ 唯一的对外产物；PUBLIC include 只有 include/（里面只有 mb_capi.h）
# ─────────────────────────────────────────────────────────────
add_library(magicblocks SHARED
  src/app/pad_scene.cpp
  src/capi/mb_capi_impl.cpp
  src/capi/mb_handle.cpp
  src/android/android_surface.cpp)           # 非 Android 时为空实现或 #if 掉
target_link_libraries(magicblocks PRIVATE magicblocks_core)
if(ANDROID)
  target_link_libraries(magicblocks PRIVATE android log)
endif()

# ★ 只暴露 include/（= 只有 mb_capi.h），且必须是 BUILD_INTERFACE 限定，
#   防止安装后把内部头带出去
target_include_directories(magicblocks PUBLIC
  $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
  $<INSTALL_INTERFACE:include>)
target_compile_definitions(magicblocks PRIVATE MB_CAPI_BUILDING_LIBRARY=1)
set_target_properties(magicblocks PROPERTIES
  OUTPUT_NAME magicblocks
  C_VISIBILITY_PRESET hidden          # ★ 只导出 mb_*
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON
  POSITION_INDEPENDENT_CODE ON
  WINDOWS_EXPORT_ALL_SYMBOLS OFF)
if(WIN32)
  # Windows 下必须用 .def 或 __declspec(dllexport) 精确导出；
  # 推荐同时生成 .def 并做“导出面只有 mb_*”的 CI 断言（见 §7）
  target_sources(magicblocks PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src/capi/mb_capi.def)
endif()

# ─────────────────────────────────────────────────────────────
# 3) 可选：旧 JNI 签名 shim（迁移期用；只依赖 mb_capi.h）
# ─────────────────────────────────────────────────────────────
option(MB_BUILD_JNI_SHIM "构建旧 JNI 签名的兼容 shim（只依赖 mb_capi.h）" OFF)
if(ANDROID AND MB_BUILD_JNI_SHIM)
  add_library(magicblocks_jni SHARED src/capi/mb_jni_shim.cpp)
  target_link_libraries(magicblocks_jni PRIVATE magicblocks android log)
  set_target_properties(magicblocks_jni PROPERTIES OUTPUT_NAME magicblocks_shim)
endif()

# ─────────────────────────────────────────────────────────────
# 4) 桌面 CLI / demo / 测试：直接链静态 core（不动它们，保住 145 项测试）
#    ⚠️ 给它们继续用静态库 magicblocks_core 的别名，避免今天所有 exe 全改
# ─────────────────────────────────────────────────────────────
add_library(magicblocks_static STATIC $<TARGET_OBJECTS:magicblocks_core>)
target_include_directories(magicblocks_static PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(magicblocks_static PUBLIC
  $<IF:$<PLATFORM_ID:Android>,vulkan,Vulkan::Vulkan> ${_tess2_lib})
```

**关键点逐条**：

1. **`include/` 目录里只放 `mb_capi.h`**（内部 `magicblocks/` 挪到 `src/include/` 或保留在 `include/` 但**只用 PRIVATE 暴露**）。这样「UI 越界」在**构建期**就不可能（`#include "magicblocks/..."` 直接 no-such-file）。
2. **Vulkan 从 `PUBLIC` 降为 `PRIVATE`**（`magicblocks_core` 与 `magicblocks` 都不再 PUBLIC 传播）。这同时修掉现状 §2.4 的隐患。
3. **`C_VISIBILITY_PRESET hidden` + 显式 `MB_API_EXPORT`**：Linux/Android 的导出面从「整个引擎（几千个符号）」收窄到「`mb_*`（约 40 个）」。
4. **`WINDOWS_EXPORT_ALL_SYMBOLS OFF`**：不开这玩意，否则 MSVC 会把**所有** C++ 符号都导出（含 `magicblocks::*`），正好是我们要避免的。
5. **`MB_BUILD_JNI_SHIM` 默认 OFF**：迁移期内 UI 还需要旧 JNI 签名时打开；最终删掉 `src/android/jni_bridge.cpp` 后整个选项消失。
6. **CLI / demo / 测试继续静态链接**：本任务的边界是「UI ⇄ SDK」，不该顺手把 8 个 CLI 改成动态链接（那会引入 `PATH` 里找 dll 的问题，破坏 145 项 gtest + 一堆 CLI CTest）。
7. **Android App 的 `build.gradle` 两种接法**（选一）：
   - **A（推荐，本轮之后做）**：`externalNativeBuild { cmake { path '../../CMakeLists.txt'; targets 'magicblocks' } }` + `android.defaultConfig.externalNativeBuild.cmake.targets` —— 只编 `magicblocks` 这一个 target，App 里拿到的就是 `libmagicblocks.so`；
   - **B（更彻底）**：SDK 作为独立产物先行构建（CI 产出 `libmagicblocks.so` 的 arm64 版本），App `jniLibs.srcDirs` 指向它，`externalNativeBuild` 只编**可选 shim**。这样 UI 侧构建不再需要 SDK 源码 —— 这正是「彻底拆分」的终点。

### 6.4 产物名与 Kotlin `loadLibrary`

`System.loadLibrary("magicblocks")` 对应 `libmagicblocks.so`（Android）/ `magicblocks.dll`（Windows，通过 `LoadLibrary("magicblocks.dll")` 或 JNA）。**产物名不变**，所以 `MbRenderer.kt:19` 那一行不需要改。这是刻意保留的：改名会让所有已安装的 APK 失效且无收益。

---

## 7. 落地路线与验收（给实现队友的检查清单）

| # | 里程碑 | 内容 | 验收（可执行） |
|---|---|---|---|
| **M1** | 头文件落盘 | 把 §5.1 写到 `MagicBlocks/include/mb_capi.h`（**逐字照抄即可 —— 已在本轮实测编译通过，见 §5.6**） | 7 格编译矩阵全绿：MSVC `{C11, C17, C++14, C++17, C++20}` × `{x64}` + `{C11, C++20}` × `{x86}`，全部 `/W4 /WX`；NDK clang `-x c -std=c99 -Wall -Wextra -Werror -fsyntax-only`；**负向**用例 `MB_STATIC_ASSERT_SIZE` 反例必须报错（不许静默通过） |
| **M2** | 场景下沉 | 把 `src/android/scene_renderer.{h,cpp}` 里的「立方体 + 相机 + OBJ 场景」搬到 `src/app/pad_scene.{h,cpp}`，改用 SDK 公开的 `IRenderDevice` / `ISwapchain`（不再是 `backend::`） | 给 `ISwapchain` 补 `msaa_samples()` / `pre_transform()`（或改用 `create_swapchain_ex`），`grep -rn "backend::" src/app` 为 0；`grep -rn "VK_SURFACE_TRANSFORM" src/ --include=*.cpp` 只剩 `src/backend/` |
| **M3** | C ABI 实现 | `src/capi/mb_capi_impl.cpp`：句柄表、错误映射、导出函数 | `dumpbin /exports magicblocks.dll` / `nm -D --defined-only libmagicblocks.so` 输出里**只有** `mb_*`；共约 40 个 |
| **M4** | 新门控 | 新增 `tools/check_capi_purity.py`：① `include/mb_capi.h` 只 include `<stdint.h>`/`<stddef.h>`；② 头文件里无 `std::`、`template`、`&`(引用)、`class`；③ `src/capi/` 不 include `magicblocks/**`（除经 `magicblocks_core` 的内部接口）；④ `src/backend/` 之外无 `VK_` 宏（修 §2.2 的 G3 漏洞） | `ctest -R magicblocks_gate_capi` 全绿；**故意**在头里加一行 `#include <string>` 后门控变红 |
| **M5** | Android 接入 | 先开 `MB_BUILD_JNI_SHIM`（行为不变），再逐条替换 `MbRenderer.external fun` 为 `mb_*` 绑定，最后删 shim | pad 上跑：拖动/捏合/双指/惯性/切模型/切 MSAA 全部与今天一致（逐项手测表）；`logcat` 里 `[native]` 日志仍在（走 `mb_log_cb`） |
| **M6** | 门控补丁 | 修 `tools/check_layer_purity.py`：加 `VK_[A-Z_]+` 与 `backend::` 两种模式（**只在 `src/backend/` 之外**报错） | 用今天的代码跑一次**必须变红**（`scene_renderer.cpp:276` 等）→ 修完变绿。这是「先证明门控有效，再让它通过」 |

**本轮不做的（诚实边界）**：

* 不落盘 `mb_capi.h`、不改任何 `.cpp/.h/.kt/CMakeLists.txt`（任务明令）。
* 不改 `MagicBlocks/android/**`（C1-C11 的修复都在 M5）。
* 不改 8 个 CLI / demo（M4 之后它们仍静态链接 `magicblocks_static`）。
* 不承诺「旧数据结构直接映射」：`mb_model_entry` 的定长字段会**截断**超长名字（>64 字节）；若名字会被截断，需要在 UI 侧显示 `message` 或由 SDK 保证 asset 名不超限（当前资产名都远短于此）。

---

## 8. 证据索引（本文每条断言的出处）

| 断言 | 位置 |
|---|---|
| 12 个 `external fun` 签名 | `MagicBlocks/android/app/src/main/java/com/magicblocks/pad/MbRenderer.kt:29-70` |
| `loadLibrary("magicblocks")` | `MbRenderer.kt:19-21` |
| `logNative` 反向回调 | `MbRenderer.kt:24-27` ↔ `src/android/jni_bridge.cpp:28-46` |
| init 失败仍返回句柄 | `jni_bridge.cpp:52-61`（注释 `:57`） |
| 句柄 = `reinterpret_cast<jlong>` | `jni_bridge.cpp:23-24` |
| `ANativeWindow_fromSurface` 引用计数归引擎 | `jni_bridge.cpp:220-234` + `scene_renderer.h:138-140` + `scene_renderer.cpp:324-339` |
| 模型列表 `\t` 文本协议 | `jni_bridge.cpp:144-181`、解析侧 `MbRenderer.kt:207-230` |
| MSAA 文本协议 | `jni_bridge.cpp:127-142`、`MbRenderer.kt:154-160` |
| 「请求值 ≠ 生效值」必须显示生效值 | `EngineSurfaceView.kt:133-142, 235-237, 945-951` |
| 渲染线程 + `join(2000)` | `EngineSurfaceView.kt:810-916, 849-860` |
| 姿态 7 个 float | `EngineSurfaceView.kt:250-251, 833-839`、`MbRenderer.kt:32-41` |
| `status_text()` 格式 | `scene_renderer.cpp:596-603` |
| `SceneRenderer` 的 SDK 类型成员 | `src/android/scene_renderer.h:17-29, 116-159` |
| backend 私有头泄漏 | `scene_renderer.cpp:13-17` |
| `dynamic_cast<backend::VulkanRenderDevice/Swapchain>` | `scene_renderer.cpp:248, 261, 273, 299` |
| `VK_SURFACE_TRANSFORM_*` 出现在 `src/backend/` 之外 | `scene_renderer.cpp:276-278` |
| G3 门控不匹配 `VK_` / `backend::` | `tools/check_layer_purity.py:97-129` |
| 只有 STATIC 库、`PUBLIC include` 全树 | `CMakeLists.txt:130, 268` |
| Vulkan `PUBLIC` 链接 | `CMakeLists.txt:306-311` |
| JNI 层与壳编在同一个 `.so` | `CMakeLists.txt:458-475` |
| App 直接编引擎全树 | `android/app/build.gradle:22, 27-34` |
| 「不存在 ≠ 空目录」已被测试钉住 | `tests/magicblocks_l3_model_catalog_test.cpp:116-131` |

---

## 9. 维护契约：新增一个 ABI 入口的操作清单

「UI 只依赖一个头文件」这句话，只有在**改头文件的代价可控、且漏改会被门控抓住**时才长期成立。
这一节把「加一个入口要动哪些文件」写成可照做的清单，不留给下一个人去翻代码猜。

### 9.1 必改 2 处（**只做这两处 = 只对 C/C++ 调用方生效**）

| # | 文件 | 改什么 | 漏改的后果 |
|---|---|---|---|
| 1 | `MagicBlocks/include/magicblocks/mb_capi.h` | `MB_API <ret> MB_CALL mb_xxx(...)` 声明；结构体新增字段只能追加在**尾部**并抬高 `MB_ABI_VERSION_MINOR` | 调用方拿不到声明；写进实现也是**契约外私货**，`GATE_SYMBOLS` 会以「导出表里多出未登记的 `mb_*`」报红 |
| 2 | `MagicBlocks/src/capi/mb_capi.cpp` | 实现 + 补一份 `MB_ABI_VERSION` 是否匹配的守卫（版本号变了就要动这里的那行） | 头里有声明、库里没符号 → 消费者**链接期 LNK2019**。编译期完全看不出来（t2 事故就是这样） |

**做完这两步，C / C++ 消费者立刻可用。** Android 的 Kotlin 壳还看不到它 —— 那是下面 3 处的事。

### 9.2 要暴露给 Kotlin，再加 3 处（合计 5 处）

| # | 文件 | 改什么 | 备注 |
|---|---|---|---|
| 3 | `MagicBlocks/tools/capi_protocol/protocol.json` | 添加/修改该入口的签名条目 | **单一事实来源**：C++ 与 Kotlin 的 ABI 表都由它生成/校验，先改这里再改两侧 |
| 4 | `MagicBlocks/src/android/jni_bridge.cpp` | 加 `Java_…_nativeXxx` 薄转发（只做 `jlong`↔句柄、`jstring`↔`const char*` 转换，业务一律下沉到 `mb_capi.cpp`） | 只做转发。**不许**在这里直接 include SDK 内部头 —— `BOUNDARY_IMPL_TU_PREFIXES` 只放行 `src/capi/` 与 `src/android/`，而 `src/android/` 的 include 面也是收紧的 |
| 5 | Android 壳 `.../MbRenderer.kt`（+ `EngineSession.kt` 的 `external fun`） | `@JvmStatic external fun nativeXxx(...)` 声明与调用 | 名字与描述符必须与第 4 步**双向吻合**，由 `GATE_JNI` 校验（函数名、`(…)V` 描述符逐条比对） |

> 为什么 `jni_bridge.cpp` 放在 `src/android/` 之后才轮到 `MbRenderer.kt`：
> `GATE_JNI` 是拿 Kotlin 的 `external fun` 与 C++ 的 `Java_*` 导出**双向**比对的。
> 只改一侧，门控直接红；两侧都改而 `protocol.json` 没跟上，`protocol` 自测红。
> 三处任缺一处都不会静默通过 —— 这是清单里最容易漏、也最不该漏的一环。

### 9.3 版本号与 `struct_size` 的规则（**改动前先对号入座**）

| 改动 | `MAJOR` | `MINOR` | `PATCH` | `struct_size` |
|---|---|---|---|---|
| 纯实现修复，头文件一个字节都没动 | — | — | +1 | — |
| 新增函数入口 | — | **+1** | 归 0 | — |
| 在结构体**尾部**追加字段 | — | **+1** | 归 0 | 改大，并同步 `MB_STATIC_ASSERT_SIZE` |
| 删除/改签名/改字段顺序或类型/改语义 | **+1** | 归 0 | 归 0 | 按新布局定，`MB_STATIC_ASSERT_SIZE` 同步 |

三条硬规则：

1. **`struct_size` 由调用方填、库侧校验**。每个带 `struct_size` 的结构体都配一个
   `mb_xxx_default()`（如 `mb_session_desc_default()`）；新增字段时改 `default()` 的
   初始化值 + `MB_STATIC_ASSERT_SIZE(struct, N)`，**N 必须实测**（`sizeof` 打印出来看），
   不能从相似结构体推导 —— 落盘时就栽过一次（§5.7.1 缺陷 2：写成 40，实际 48）。
2. **只增不改，缺口留在尾部**。删字段、换字段类型、调换顺序都是 MAJOR；
   想「反正没人用」就删掉，是这套规则里唯一的破约方式。
3. **旧的 `PATCH` 号不往回带**。MINOR 抬高时 PATCH 归 0；
   `MB_ABI_VERSION` 是 `MAJOR*10000 + MINOR*100 + PATCH` 打包的这个口径不能绕开
   （`verify_split.py` 的 A 段与 `GATE_VERSION` 都按它解析）。

### 9.4 谁在把关：漏改必红的三道门

| 门 | 位置 | 抓什么 |
|---|---|---|
| `GATE_SYMBOLS` | `MagicBlocks/tools/check_ui_isolation.py` | 解析 `mb_capi.h` 里的 `MB_API` 声明集合，与**实际导出表**逐条比对：**缺一个即红**（「没导出/被 mangle」），**多出未登记的 `mb_*` 也红**（「私货」）。导出表读法：先 `llvm-nm --extern-only <导入库 .lib>`，读不出就退到 `dumpbin /nologo /exports <dll>`（`llvm-nm` 对 PE `.dll` 恒输出 `no symbols`，那是工具限制，不是「没导出」） |
| `MB_API_SYMBOLS` 自测 | 同上，`test_symbol_list_matches_header` | 头文件加了入口而那份可读快照没跟上 → 自测红 |
| `GATE_JNI` | 同上 | Kotlin `external fun` ↔ C++ `Java_*` 名字与描述符双向吻合 |
| A 段 / B3c | `MagicBlocks/tools/uisplit/verify_split.py` | 头文件是否仍**只有 1 个**、是否**带 UTF-8 BOM**、以及「不传 `/utf-8` 的 `/W4 /WX` 编译」是否仍通过（BOM 掉了这条就红） |

> 关于 `GATE_SYMBOLS` 的一个使用前提（别绕过去）：它需要**先构建出 DLL 与导入库**。
> 什么都没构建时它报的是 `找不到 --lib`，那是「没有证据」，不是「契约正确」。
> 判定顺序：先 `build.ps1`，再看这道门。

---

## 附：变更记录

* 2026-11-11 — 初版（DRAFT）。基于 `feat/ui-sdk-split@98cc9d9` 的只读审计；含 `mb_capi.h` 草案（§5.1）与 CMake 拆分方案（§6）。**未修改任何源码文件。**
* 2026-11-11 — **落盘实现**：新增 `MagicBlocks/include/magicblocks/mb_capi.h`（444 行 / 26 个导出入口，唯一新增源码文件；未改 CMakeLists.txt、未改任何 `.cpp`/`.kt`）。同步新增 §5.7（路径与命名差异、落盘版新增入口、4 类验证记录、落盘时抓到的 2 个缺陷、给 UI 侧的 2 条硬约束）；§0 增加第 8 条结论；文件头状态行更新。§1–§4 审计内容未变。
* 2026-11-11 — 新增 **§9「新增一个 ABI 入口的操作清单」**（必改 2 处 / 暴露给 Kotlin 再加 3 处 / 版本号与 `struct_size` 规则 / 漏改必红的三道门）。给 `mb_capi.h` 加 **UTF-8 BOM**：不带 BOM 时 MSVC 按代码页 936 读中文注释 → `warning C4819` → 调用方一开 `/W4 /WX` 就是 `error C2220`，等于给每个 UI 加了条隐性编译选项前提。门控侧补了 A9（静态查前 3 字节）与 B3c（不传 `/utf-8` 的 C11/C17/C++17/C++20 编译证明）。
