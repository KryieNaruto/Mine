# MagicBlocks L0（device/ 设备与平台层）落地计划 · 第 3 版

日期：2026-11-03 · 规格：`docs/superpowers/specs/2026-11-03-magicblocks-render-engine-sdk-design.md`
（§2、§4 L0、§7、§9、§11、§13 第 1 阶段的前半）

> **审阅历史**
> - 第 1 版：加权 **63.9/100 → 打回**（A：架构 70 / 需求 66；B：可行性 62 / 可测性 66 / 健壮性 58）。
> - 第 2 版：A 维 74/71 打回、B 维 66/68/62 打回。
> - **第 3 版：加权 87.7/100 → 通过**（架构 87 / 需求 91 / 可行性 87 / 可测性 87 / 健壮性 85）。
>   本版在第 3 轮基础上再吸收 7 条非阻断建议（见 §11）。

> **执行结果（2026-11-03）**
> 已按第 3 版落地并全绿：`ctest` **32/32 通过（0 失败 0 跳过）**；门控 G1/G3/G4 rc=0；
> 依赖回归 `ran=141 failed=0 skipped=0`；门控自测 17/17。
> 执行中修正了 3 处计划未预见的问题（图像朝向双翻行、G3 玩法名门控自相矛盾、
> 读回缺 draw 调用），详见 README「图像朝向契约」与 `tools/check_layer_purity.py` 注释。


## 0. 本轮范围

### 0.1 一句话范围

只落 **L0 `device/`**：SDK 公共头 `magicblocks/device/*`、`namespace magicblocks::device` 抽象接口、
Vulkan 后端实现、L0 的 **CLI + 离屏渲染 → PNG** 硬约束通路、以及 spec §7 的 4 个门控脚本。
L1 resources/memory/command、L2 scene、L3 primitives、L4 features 全部**不做，连接口位都不在本轮提交**
（第 1 版把它们塞进 L0 公共头，属越层扩范围，本版已删）。

**工程定位（消化审阅 A⑦）**：`MagicBlocks/` **就是 SDK 根**，`include/magicblocks/` 是将来 L1–L4 的公共头根目录，
命名空间 `magicblocks::` 与头路径 `magicblocks/<layer>/` 从此固定；**本轮只填 `device/`**，L1 接入时**不搬家、不改名**。

### 0.2 实测更正：三条审阅结论不成立（附实验证据）

复用自老代码库的审阅意见里有三条技术判断与实测相反，若不更正会把计划推向错误设计：

| # | 审阅意见 | 实测结论 | 证据 |
|---|---|---|---|
| E1 | 「SwiftShader ICD 自报 `api_version: 1.0.5`，不满足 push constant≥128，`Caps` 用例在 SwiftShader 上必然红」 | **部分不成立。** ICD json 的 `api_version` 是 **loader 接口版本**（`MAKE_API_VERSION(0,0,major,minor)`），与设备 API 版本无关；SwiftShader 设备实测 `apiVersion = 1.3.0`、`maxPushConstantsSize = 128`、含 `VK_KHR_swapchain`(rev 70)。**但**：见 E1b | `vulkaninfo` GPU1 块实测 |
| **E1b** | （本节初稿曾写「SwiftShader 含 `VK_EXT_memory_budget`」） | **该句已撤回，实测为假。** SwiftShader 设备扩展共 **83** 个，**不含 `VK_EXT_memory_budget`**（NVIDIA 设备含；`vk_swiftshader.dll` 二进制内亦无此字符串）。SwiftShader 的 memory heap 实测为**单个 2 GiB heap**。**影响**：`DeviceCaps.memory_budget` 在 SwiftShader 上无法来自 `heapBudget`，改按 §3-D2 的**双来源**取值，用例断言相应改成分支断言 | `vulkaninfo` GPU1 扩展表 + DLL 字符串检索 |
| **E4** | 设备选择顺序 | 本机 `vulkaninfo` 枚举顺序为 **GPU0=RTX 5060、GPU1=SwiftShader**，且顺序不保证。因此**必须强筛软件光栅**，不能取 `devices[0]`（审阅 B③） | `vulkaninfo` GPU0/GPU1 块 |
| E2 | 「提权下可走 `vk_loader_settings.json`（`driver.disable_environment` + `staging_dirs`）」 | **不成立。** 该 settings 文件 schema 无 `driver`/`staging_dirs` 键，loader 直接报 `No valid vk_loader_settings.json file found` | 在 SwiftShader 目录放置该文件后 loader debug 日志原文 |
| E3 | （第 1 版计划 §1.1）「提权下 SwiftShader 不可达 → 以非提权 shell 复跑为唯一出口」 | **不成立，已有可行的主路径。** 提权下 loader 忽略 `VK_DRIVER_FILES`/`VK_ICD_FILENAMES`/`VK_LOADER_DRIVERS_SELECT`，但**读 HKLM 注册表**：注册 `HKLM\SOFTWARE\Khronos\Vulkan\Drivers\<icd.json 绝对路径> = 0` 后，`vulkaninfo` 枚举到 `SwiftShader Device (LLVM 10.0.0)` | 见 §1.2，本轮已实测通过 |

> 结论：**不需要任何降级/遗留项**。SwiftShader 在提权与非提权下均可达。`DeviceCaps` 除 `memory_budget`
> 走的来源按设备分支不同（E1b）外，其余探测项在 SwiftShader 上均能拿到实值；两个预算来源都已定义取值与断言。

#### 0.3 环境阻塞项的最终裁定

- 软件光栅驱动选择入口统一收敛到 `MagicBlocks/tools/vulkan_env.ps1|sh`：**双路径**——
  非提权 → 导出 `VK_DRIVER_FILES`（与 `tools/win-deps.sh:397-398` 同组变量）；
  提权 → loader 忽略环境变量，改走 **HKLM 注册表**：`HKLM\SOFTWARE\Khronos\Vulkan\Drivers\<icd json 绝对路径> = 0`
  （由 `register_swiftshader_icd.ps1` 幂等写入，`-Remove` 幂等移除）。**本轮已实测该路径可枚举到 SwiftShader**。
- **不修改 `third_party/` 的内容**：注册表项**只读引用**其绝对路径，不写入、不覆盖任何池产物（审阅 B①）。
  脚本以 `--icd <path>` 显式传入，默认 `${MINE_ROOT}/third_party/_install/swiftshader-master/release/vk_swiftshader_icd.json`。
- 非管理员且需要提权路径时 → **明确报错退出码 3**，不静默降级（不回退原则）。
- **设备强筛硬约束**（审阅 B③ / E4）：物理设备选择必须按 `is_software_rasterizer || deviceName 含 "SwiftShader"`
  强筛；候选含非软件光栅设备时**直接 `Status::Unsupported` 失败**，绝不 fallback 到 RTX 5060。
- **禁止**任何「无 SwiftShader 就用物理 GPU」的降级分支。用例层面见 §6 的 `Golden.RequiresSwiftShaderDevice`。

## 1. 关键事实核对（全部实测，非记忆）

| 断言 | 实测 |
|---|---|
| `MagicBlocks/` 已由 `tools/new-project.py` 生成（模板态） | ✅ 已存在，10:27 生成；`deps.yaml` = `use: [googletest]` |
| cpp 模板产出可直接用 | ❌ **模板有坑**：默认 **C++17**、`find_package(googletest CONFIG REQUIRED)` + `googletest::googletest`，而**池里是 `GTest` 包**、target 为 `GTest::gtest`；模板也无 Vulkan/glslc/CTest/`_pool_only`/`/utf-8` |
| 池内 `googletest-v1.15.2/{release,debug}/lib/cmake/GTest/` | ✅ 存在，target `GTest::gtest` / `GTest::gtest_main` |
| 池内 `vma-v3.5.0/{release,debug}/include/vk_mem_alloc.h` | ✅ 存在（本轮不 include，留给 L1） |
| 池内 `glfw-3.4` | ✅ 存在（本轮**不用**：L0 只需 surface/swapchain，Windows 用隐藏 HWND 直连 `VK_KHR_win32_surface`，少一个依赖） |
| Vulkan SDK | ✅ `D:\env\VulkanSDK\1.4.357.0`（`VK_HEADER_VERSION 357`，`Vulkan_GLSLC_EXECUTABLE` 可用） |
| CMake / Ninja / Python / MSVC | ✅ cmake 3.31.12、ninja、Python 3.13、VS18（vcvars64.bat 存在） |
| loader 1.4.341 提权行为 | ✅ 忽略 `VK_DRIVER_FILES`/`VK_ICD_FILENAMES`/`VK_LOADER_DRIVERS_SELECT`；读 HKLM |
| `KHRONOS_validation` 层可用于泄漏判定 | ✅ SDK 1.4.357 自带（`VK_LAYER_KHRONOS_validation`） |
| EasyPainter 有 `gl_InstanceIndex` 用例可抄 | ❌ 没有。L0 的实例化着色器**新写**，不写"复用范式" |

## 2. 产物布局

```
MagicBlocks/
  CMakeLists.txt              # 手写(覆盖模板)：C++20 + _pool_only + /utf-8 + find_package(Vulkan) + glslc 绝对路径 + CTest
  CMakePresets.json           # debug / release（沿用模板结构）
  deps.yaml                   # use: [googletest]
  include/magicblocks/device/ # ← L0 公共头（唯一对外接口）
    types.h                   # BackendType / DeviceDesc / DeviceCaps / Extent2D / Format / PresentMode / NativeSurface
    result.h                  # Status / Result<T> / StatusOr<T>（code + message，无静默降级）
    events.h                  # InputEvent + EventStream（确定性时间戳，不读实时时钟）
    lifecycle.h               # AppLifecycleEvent
    render_device.h           # IRenderDevice + CreateRenderDevice（D1/D2/D8）
    swapchain.h               # ISwapchain（D3）
    render_target.h           # IRenderTarget：Window/Offscreen 同构（D4）
    frame_sync.h              # IFrameSync + InFlightCount（D5）
  src/backend/vulkan/         # ← 唯一允许出现 Vk*/vulkan.h 的目录
    vk_common.h               # pragma once + VK_USE_PLATFORM_* + 实例/设备创建公共件
    vulkan_instance.cpp        # 实例、surface 扩展按平台追加、VK_KHR_portability 枚举
    vulkan_device.cpp          # 物理设备选择、单队列、DeviceCaps 探测、validation 设置
    vulkan_swapchain_{win32,xcb,wayland}.cpp  # 各平台独立编译单元，CMake 选编
    vulkan_target.cpp          # WindowTarget / OffscreenTarget 同一 IRenderTarget
    vulkan_frame_sync.cpp
    vulkan_readback.cpp        # 离屏 RGBA8 读回（L0 的读回通路，不是 L2 渲染核心）
    device_factory.cpp         # 后端入口，把 VkDevice 包成不透明句柄
  src/cli/main.cpp            # magicblocks-l0-cli（device / render 两个子命令）
  shaders/                    # ← 着色器唯一位置
    fullscreen.vert / .frag   # 实例化 + gl_InstanceIndex（G2 范式），无 GS
    embed_shaders.py          # 沿用 EasyPainter 范式
  tests/                      # gtest 用例（§6）
  tests/data/golden_l0.png    # 单一 baseline + tests/data/README.md（注明生成环境）
  tools/
    vulkan_env.ps1 / .sh      # 驱动选择（双路径，无降级）
    register_swiftshader_icd.ps1  # 提权路径：HKLM 注册表（幂等/可卸载）
  README.md
tools/                        # ← 工作空间级门控（spec §7）
  check_no_geometry_shader.py
  check_layer_purity.py
  check_no_raw_alloc.py
  check_deps_tests.py
  tests/test_check_scripts.py # ← 门控脚本自测（违规样本红 / 合规样本绿）
```

**目录归属澄清**（回应 A①⑥ / B②③）：`src/`、`tests/`、`shaders/` 全部在 `MagicBlocks/` 内，不散落到工作空间根；
工作空间根只新增 `tools/` 下 4 个门控脚本 + 1 个自测，属 spec §7 明文要求的位置。

## 3. L0 功能点 → 接口 → 用例（唯一事实来源，§6 与本节同名）

| 功能点 | 接口（`include/magicblocks/device/`） | 实现 | 用例 |
|---|---|---|---|
| **D1** `IRenderDevice`/`DeviceDesc` | `DeviceDesc{backend, app_name, enable_validation, **request_rev_z**, **requested_msaa**, **in_flight_frames=2**, require_software_rasterizer=true}`；`BackendType{Vulkan, OpenGLES, Metal}`；`CreateRenderDevice(desc) -> Result<unique_ptr<IRenderDevice>>` | `device_factory.cpp`、`vulkan_device.cpp` | `Device.CreateAndDestroy` / `Device.RequestUnsupportedFails` |
| **D2** `DeviceCaps` 探测（**只放能力项，命名与 Desc 请求项不重名**） | `DeviceCaps{depth_formats, **supported_msaa_levels**, texture_formats, **rev_z_supported**, max_push_constant_size, max_uniform_buffer_range, timestamp_queries, compressed_formats, **memory_budget, memory_heap_total**, api_version, driver_version, device_name, is_software_rasterizer}` | 见下方「请求 vs 能力解析规则」 | `Device.CapsAreComplete` / `Device.CapsBudgetSourceMatchesExtension` |
| **D3** `ISwapchain` | `swapchain.h`：`Create(NativeSurface, Extent2D)`、`Extent/Format/PresentModes`、`Rebuild(extent)`；`NativeSurface{Kind{Android, Win32, Xcb, Wayland, Darwin, Headless}, a, b}` —— **公共头零平台宏** | `vulkan_swapchain_*.cpp`（各平台独立 TU，CMake 按 OS 选编）；实例层按平台追加 `VK_KHR_{win32,xcb,wayland}_surface` 并在 CMake 里 `target_compile_definitions(VK_USE_PLATFORM_*)`；设备层启用 `VK_KHR_swapchain` | `Swapchain.HiddenWindowCreate` / `Swapchain.PresentModeDefaultsToFifo` / `Swapchain.RebuildOnResize` / `Swapchain.DestroyThenRecreate` / `Swapchain.AndroidTuCompiles` / `Swapchain.HeadlessReportsUnsupported` |
| **D4** `IRenderTarget` 同构 | `render_target.h`：单一 `IRenderTarget`，`TargetDesc{kind=Window/Offscreen, extent, samples, with_depth, with_stencil, format}`、`ColorAttachment`、`DepthAttachment`、`ReadbackRgba8()` | `vulkan_target.cpp` | `Target.WindowAndOffscreenSameInterface` / `Target.OffscreenReadbackIsRgba8` |
| **D5** 帧同步 | `frame_sync.h`：`IFrameSync`，`InFlightCount()`（2 或 3）、`SlotForFrame(frame_index)`（= `frame_index % InFlightCount()`）、`Acquire/SubmitAndPresent/WaitIdle/AdvanceFrameIndex` | `vulkan_frame_sync.cpp`：N 个 fence + semaphore，**帧环形资源复用** | `FrameSync.InFlightCountIsTwoOrThree` / `FrameSync.RingSlotCycles` / `FrameSync.RunsFifteenFrames` |
| **D6** `InputEvent` | `events.h`：`InputEvent{kind, pointer_id, position, button, delta, timestamp_us, handled}`；`EventStream{append/rewind/next/pending}` —— 时间戳由录制方给定，**头文件内不出现任何时钟调用** | 纯数据 | `Input.StreamRoundTrip` / `Input.TimestampIsPreserved` |
| **D7** 生命周期 | `lifecycle.h`：`AppLifecycleEvent{SurfaceCreated, SurfaceDestroyed, SurfaceChanged(extent), Paused, Resumed}`；`IRenderDevice::NotifyLifecycle(ev)` | `vulkan_device.cpp` 转发 swapchain 重建 | `Lifecycle.SurfaceChangedRebuilds` / `Lifecycle.PauseResumeState` |
| **D8** 失败即报错 | `result.h`：`Status{Ok, InvalidArgument, Unsupported, OutOfMemory, DeviceLost, InitializationFailed, ShaderCompileFailed, IoError}`；`Result<T>` 携带 `Status{code, message}` | 全链路 | `Errors.UnsupportedBackend` / `Errors.CarriesCodeAndMessage` / `Errors.SupportedBackendSucceeds` |

**请求 vs 能力解析规则**（消化审阅 A③，消除 Desc/Caps 语义悬空）：
- `DeviceDesc` 只放**请求项**（`request_rev_z` / `requested_msaa` / `in_flight_frames`），`DeviceCaps` 只放**能力项**
  （`rev_z_supported` / `supported_msaa_levels`），**两边零重名**。
- 请求不可满足（如 `request_rev_z=true` 但 `rev_z_supported=false`、`requested_msaa=8` 不在
  `supported_msaa_levels`、`in_flight_frames ∉ {2,3}`）→ `CreateRenderDevice` 返回 **`Status::Unsupported`**，
  **不静默降级**（不回退原则）。由 `Device.RequestUnsupportedFails` 断言。
- `memory_budget` 双来源：设备支持 `VK_EXT_memory_budget` → `heapBudget` 之和；不支持（SwiftShader 实测）→
  各 `VkMemoryHeap.size` 之和，写进独立字段 `memory_heap_total`，`memory_budget` 恒等于「当前可用来判超预算的值」。
  **不依赖 VMA**（预算权威来源不设在 VMA）。

**NativeSurface 值表**（消化审阅 A④ / A6，公共头零平台宏，平台值只在 `vulkan_swapchain_*.cpp` 内部解释）：

| `NativeSurface::Kind` | `a` | `b` | 本轮 |
|---|---|---|---|
| `Android` | `ANativeWindow*` | 保留（0） | 只留 TU + ABI 编译期校验，不建 Gradle 工程（spec C1/C3） |
| `Win32` | `HWND` | `HINSTANCE` | **本轮主路径**（隐藏窗口） |
| `Xcb` | `xcb_connection_t*` | `xcb_window_t` | TU 在 Linux 编，本轮 Windows 不编 |
| `Wayland` | `wl_display*` | `wl_surface*` | 同上 |
| `Darwin`（iOS 抽象位，spec D7） | `CAMetalLayer*` | 保留（0） | **只留枚举 + 值表，不实现**（spec §9「iOS 只做设计考虑」） |
| `Headless` | 0 | 0 | 返回 `Unsupported` |

**L1 接口位不在本轮**：`IMemoryAllocator` / `ICommandList` 的接口在 L1 才提交（A② 指出其违反 §2.1 单向下沉，
本版已从 L0 删除；L0 只保留 `IFrameSync::AdvanceFrameIndex`，为 L1 的 VMA 帧边界预留，并在注释里写明）。

**`IFrameSync` 与 `ISwapchain` 的 present 归属**（消化审阅 A3）：`IFrameSync` **不碰 present**——
它只有 `Acquire(fence) / WaitSlot(slot) / AdvanceFrameIndex()`；present 由 `ISwapchain` 独占。
`FrameSync.*` 用例只在**离屏态**验证环形槽复用，不依赖窗口。

**离屏渲染归属**（回应 A① / B⑤）：`vulkan_readback.cpp` 只做 **RGBA8 读回**，不承载场景/渲染图/排序，
不属 L2；CLI 的实例化自检画面由 `src/cli/` 直接驱动 `IRenderTarget` + 少量离屏命令，**不落成引擎层 API**，
L2 落地时整体迁到 `src/scene/`（计划内已注明迁移路径）。

## 4. 硬约束落地（CLI + 离屏 → PNG）

```
magicblocks-l0-cli device  [--json out.json]          # 打印 DeviceCaps（无窗口）
magicblocks-l0-cli render  --width 320 --height 240 --frame-index 0 --output out.png
```

- `render` 全走 **`OffscreenTarget`**（Vulkan 离屏渲染核心，平台无关一套代码），不依赖窗口/显示服务；
- 输出 RGBA8 PNG（自写 `image_io`，不引新依赖），PNG magic + 尺寸校验；
- 画面：实例化绘制的确定性自检图（固定色块 + 左上角红色标定点 + 深度排序验证），
  顶点着色器 `gl_InstanceIndex` 取参数（G2 无 GS 范式，**新写**）；
- **`--frame-index N`（默认 0）替代被删的 `--frame N`**（消化审阅 A§4 缩水）：它只是**确定性输入参数**
  （驱动自检图的实例相位 + 命令哈希），不引入 L2 的场景帧语义；
- golden 单一 baseline，生成环境写入 `tests/data/README.md`：**SwiftShader 设备 `deviceName` 含 `SwiftShader` 时才允许生成**；
- **比对侧同样强筛**（审阅 B②）：读取/比对 golden 时若设备不是软件光栅 → **失败**，错误信息里打印实际 `deviceName`；
- 跨驱动一致性（spec §11.6）落地为**可判定条件**：`≤1% 像素差异` + 三条结构性断言（尺寸、左上角标定点颜色、
  主色块中心像素），不要求像素完美；`tests/data/README.md` 记录生成时的 `deviceName / apiVersion / heap / CMake / glslc` 版本。

## 5. 门控脚本（spec §7，本轮交付 + 自测）

| 脚本 | 断言 | 关键更正 |
|---|---|---|
| `check_no_geometry_shader.py` | 扫描 **`MagicBlocks/shaders/**`（必须存在且非空）** 与 `MagicBlocks/src/**`：GS stage 名、`gl_PrimitiveIDIn`、`EmitVertex`/`EndPrimitive`、`gl_in[`、`triangles with adjacency`、`GL_*_ADJACENCY`、`-fshader-stage[ =]+(geom|geometry)` | 补「shaders/ 缺失即失败」；正则覆盖 `=` 与 ` = ` 两种写法 |
| `check_layer_purity.py` | ① `src/backend/**` **之外**不得出现 `#include <vulkan/`、`Vk*`、`vkXxx(`/`vmaXxx(`；② `src/backend/**` **内**不得出现玩法名（sketch/blocks/voxel/stamp/捏捏乐/涂鸦/积木/叠叠乐/印章） | **重写**：上一版整个跳过 backend，与 §5 条文相反 |
| `check_no_raw_alloc.py` | backend 外不得出现 `vkAllocateMemory`/`vkFreeMemory`/**`vkMapMemory`/`vkUnmapMemory`**；backend 内允许但须落在 `// VMA-injection` 标记块内（L1 用）；本轮断言「backend 外零裸分配」 | 口径与已实现脚本一致（含 map/unmap，B⑨） |
| `check_deps_tests.py` | `python -m unittest discover -s tools/tests -p "test_*.py"`，断言 **用例文件数 == 11**（既有 10 + 本轮新增 `test_check_scripts.py`）且 **0 失败 0 跳过**，任一失败即门控失败 | 显式跑既有用例集，清单见 §8 |

五个脚本（含自测）全部注册进 CTest（`magicblocks_gate_*` / `magicblocks_gate_scripts_selftest`），**不设 SKIP**。

## 6. 测试设计（测试门：0 失败 0 跳过）

| 用例 | 断言要点 |
|---|---|
| `Device.CreateAndDestroy` | 创建成功；`caps` 可读；**校验层可计量判据**：`enable_validation=true` 时以 `VK_LAYER_KHRONOS_validation` 经 `VK_INSTANCE_LAYERS` 显式启用 + `VkValidationFeaturesEXT` 打开 **`best_practices`** + `vkCreateDebugUtilsMessengerEXT` 回调收集；断言 **error 条目数 == 0** 且 **回调总数 > 0**（后者防「层根本没挂上」造成的假绿）；两次创建互不影响 |
| `Device.CapsAreComplete` | 深度格式含 `D16_UNORM` 或 `D24_UNORM_S8_UINT`；`supported_msaa_levels` 非空；纹理格式含 `RGBA8_UNORM`；`max_push_constant_size >= 128`；`memory_budget > 0`；`timestamp_queries` 布尔可读；`compressed_formats` 可空但字段存在；`max_uniform_buffer_range > 0` |
| `Device.CapsBudgetSourceMatchesExtension` | 分支断言：设备支持 `VK_EXT_memory_budget` → `memory_budget` = `heapBudget` 之和且 `memory_budget_from_extension==true`；不支持（SwiftShader 实测）→ `memory_heap_total` = `VkMemoryHeap.size` 之和，`memory_budget == memory_heap_total` 且 `from_extension==false`。两支都断言 `memory_budget > 0` |
| `Device.CapsValuesAreConsistent` | `api_version >= 1.0`；`device_name` 非空；`is_software_rasterizer == (device_name 含 "SwiftShader")` |
| `Device.UnsupportedBackendFails` | `BackendType::OpenGLES` / `Metal` → `Status::Unsupported`，message 非空 |
| `Device.RequestUnsupportedFails` | `request_rev_z=true` 而 `rev_z_supported=false`、或 `requested_msaa` 不在 `supported_msaa_levels`、或 `in_flight_frames=4` → `Status::Unsupported`（**不静默降级**，A③） |
| `Device.RefusesPhysicalGpu` | **可注入枚举器**（`DeviceDesc` 之外的测试钩子 `testing::set_physical_device_override("非软件光栅名")`）：`require_software_rasterizer=true` 且候选为非软件光栅设备 → `Status::Unsupported`，消息含实际 `deviceName`（**防静默跑在 RTX 上**，B③/E4） |
| `Golden.RequiresSwiftShaderDevice` | **golden 用例的前置**：读取/比对时设备不是软件光栅 → **失败**（不是 SKIP），错误码 `Unsupported`，消息打印实际 `deviceName`（B①） |
| `Offscreen.MatchesGoldenSingleBaseline` | 与 `tests/data/golden_l0.png` 比对：`≤1%` 像素差异 + 三条结构性断言（尺寸相等、左上角标定点 R>200 且 B<60、主色块中心像素命中阈值） |
| `Offscreen.FrameIndexChangesOutput` | `--frame-index 0` 与 `--frame-index 1` 的 PNG **不同**；同一 index 连跑两次**逐像素完全相同**（把「驱动实例相位」变成可判定断言，建议③） |
| `Target.OffscreenReadbackIsRgba8` | 长度 = w\*h\*4；**通道序断言**：左上角标定点 R>200 且 B<60（防 RGBA/BGRA 互换） |
| `Target.WindowAndOffscreenSameInterface` | 同一 `IRenderTarget*` 承载两种 kind；无窗口时 `Unsupported` |
| `Swapchain.HiddenWindowCreate` | Windows：隐藏 HWND → surface → swapchain，断言 extent/format/FIFO 在 `PresentModes` 内 |
| `Swapchain.PresentModeDefaultsToFifo` | 默认 present mode 为 FIFO；MAILBOX 若在 `PresentModes` 内则可探测到（spec D3） |
| `Swapchain.RebuildOnResize` | `Rebuild(新 extent)` 后 `Extent()` 一致（对应旋转 / `SurfaceChanged`） |
| `Swapchain.DestroyThenRecreate` | `SurfaceDestroyed` → swapchain 失效；`SurfaceCreated` → 可重建并出图（对应 `APP_CMD_DESTROY_WINDOW` → `INIT_WINDOW`，A7） |
| `Swapchain.AndroidTuCompiles` | CMake 开关 `MB_ANDROID_TU_COMPILE_CHECK`(默认 **ON**)：以 `src/backend/vulkan/stub/anativewindow.h` 提供的 `ANativeWindow` 前置声明编译 `vulkan_swapchain_android.cpp`（**始终参与 `magicblocks` 编译**，只用 `vkCreateAndroidSurfaceKHR` 的 ABI 形状,不链接）；NDK 存在时 `MB_ANDROID_USE_NDK=ON` 改走真实 `<android/native_window.h>`（建议⑤） |
| `Swapchain.HeadlessReportsUnsupported` | `NativeSurface{Headless}` → `Unsupported` + message |
| `FrameSync.InFlightCountFromDesc` | 传入 `in_flight_frames=2→2`、`3→3`、`4→InvalidArgument`（A5） |
| `FrameSync.SlotReuseAfterSignal` | 连续跑 2N 帧，第 i 帧与第 i+N 帧拿到**同一槽**且该槽 fence 在 `Acquire` 时已 signal（**资源真复用**，不是槽号算术，A5） |
| `FrameSync.RunsFifteenFrames` | 连续 15 帧 `Acquire/WaitSlot` 全 OK；**泄漏判据可计量**：校验层 error 计数 == 0 且回调总数 > 0（同 `Device.CreateAndDestroy` 口径，建议⑥） |
| `Input.StreamRoundTrip` | 写入 N 条 → rewind → 逐条取回，字段逐一相等 |
| `Input.TimestampIsPreserved` | 时间戳原样（1000000us 递增），不因引擎耗时改动 |
| `Lifecycle.SurfaceChangedRebuilds` | `SurfaceChanged{640×480}` → swapchain extent 更新；`Paused/Resumed` 状态可查 |
| `Errors.CarriesCodeAndMessage` | 三条失败路径均 code 正确、message 非空 |
| `Errors.SupportedBackendSucceeds` | `BackendType::Vulkan` → `Ok`（正例，防"什么都失败"的假绿） |

**CLI 级 CTest**（直接跑 exe，端到端，不进 L0 单元表）：`cli_device_json`（JSON 可解析且含 `device_name`）、
`cli_render_png`（`render --width 320 --height 240 --frame-index 0 --output out.png` → PNG magic 正确、尺寸正确、无窗口）。

## 7. 执行顺序（先红后绿）

**步 0（先行，必须先绿才进步 1）：修复既有 `tools/tests` 的 3 个坏用例**
实测：`python -m unittest discover -s tools/tests -p "test_*.py"` 在**动手前**是
`Ran 140, FAILED (failures=3)`，三条均为**与本轮无关的既有环境相关缺陷**（如实记录，不掩盖）：

| 坏用例 | 根因（实测） | 修复 | 状态 |
|---|---|---|---|
| `test_android.test_home_android_sdk_fallback` | 本机 `LOCALAPPDATA\Android\Sdk` 真实存在且探测顺序在其之前，`~/Android/Sdk` 分支测不到 | 用例里 `os.environ.pop("LOCALAPPDATA")` 隔离本机环境 | ✅ 已修 |
| `test_android.test_user_deps_android_sdk_fallback` | 同上 | 同上 | ✅ 已修 |
| `test_cmake_driver.test_gbk_console_does_not_crash_on_unencodable` | 子进程 stdout 是 cp936，`print('\ufffd')` 在**子进程内**先抛 `UnicodeEncodeError`（rc=1），测到的不是父进程打印路径 | 子进程注入 `PYTHONIOENCODING=utf-8`；控制台替换形态随平台而异，去掉写死的 `"?\n"` 断言，保留「不崩 + 有输出」 | ✅ 已修 |

**实测结果：`Ran 140 tests ... OK`（0 失败 0 跳过）**。

1. 手写 `MagicBlocks/CMakeLists.txt` 覆盖模板（C++20 + `_pool_only` + `/utf-8` + `find_package(Vulkan)` +
   `find_program(Vulkan_GLSLC_EXECUTABLE)` + `VK_USE_PLATFORM_*` 平台定义 + **`enable_testing()` + `include(GoogleTest)` +
   `gtest_discover_tests(...)`** + `CMAKE_BUILD_TYPE` 空时按 `CMAKE_CONFIGURATION_TYPES` 回退 variant（照抄 EasyPainter L22-32）），
   链接目标用池实测的 **`GTest::gtest GTest::gtest_main`**；`deps.yaml` 保持 `use: [googletest]`。
   **不重跑** `new-project.py`（目录已存在必失败，B©）。
2. 落公共头 `include/magicblocks/device/*`（纯接口）→ 落 `src/cli` 与 `tests/` 骨架 → 编译期红。
3. 门控脚本：改 G1（shaders 存在性 + 根 CMakeLists）+ 重写 G3（真扫 backend）+ 新增 G4、G5 + 自测
   → **运行实测**：G1 现为 `rc=2 shaders/ 缺失`（正确报错，非静默绿）、G3 `rc=0`、G4 `rc=0`、G5 `rc=0 ran=140 failed=0 skipped=0`、自测 16 例全绿。
4. 逐个落实现：`vulkan_instance` → `vulkan_device`（**软件光栅强筛**）→ `vulkan_target`(+`vulkan_readback`) →
   `vulkan_frame_sync` → `vulkan_swapchain_win32` → `device_factory` → CLI。
5. 生成 golden（确认 `deviceName` 含 SwiftShader）→ golden 用例绿 → 门控全绿 → 既有 `tools/tests` 全绿。

## 8. 影响面核对

- **新增**：`MagicBlocks/`（模板已存在，本轮回填内容）、`tools/check_*.py`（4 个）、`tools/tests/test_check_scripts.py`（1 个）。
- **修改**：`tools/tests/test_android.py`、`tools/tests/test_cmake_driver.py`（步 0 的既有坏用例修复，见 §7）。
- **不触碰**：`EasyPainter/`、`third_party/*`（注册表只读引用其路径，不写内容）、`tools/deps_lib/*`、`tools/build-deps.py`、`tools/new-project.py`。
- **既有回归**：`tools/tests/` 现有 **11 个 `test_*.py`**（android / build_deps / check_scripts / cmake_driver / fetch / manifest / mirror /
  msvc_env / new_project / pool / project_gen），**实测 140 用例 0 失败 0 跳过**，由 `check_deps_tests.py` 全量重跑。
- **共享路径**：无跨项目共享源文件；`MagicBlocks/` 独立目录，不影响其它项目构建。

## 9. 验收（spec §11 的 L0 子集）

1. 门控：4 个脚本 + 1 个自测零违规，CTest 全绿。
2. 无 GS：G1 通过；着色器目录存在且只含 vert/frag。
3. CLI + 离屏出图：`magicblocks-l0-cli render --width 320 --height 240 --frame-index 0 --output out.png` 无窗口出 PNG，golden 通过。
4. 失败即报错：所有失败路径带 code/message，无静默降级；**非软件光栅设备一律 `Unsupported`**。
5. 测试门：MagicBlocks 全用例 + 既有 `tools/tests`（140 例）全绿，0 失败 0 跳过。
6. 单一 golden：不建分平台基线。
7. **设备可达性**：提权（HKLM）与非提权（`VK_DRIVER_FILES`）双路径均能枚举到 SwiftShader 并跑完 golden（§0.2-E3 已实测）。
8. **跨驱动一致性（spec §11.6）**：Linux(lavapipe) 与 Windows(SwiftShader) 产出**同一份** `golden_l0.png` 的比对通过
   （≤1% + 三条结构性断言，见 §4）；不建分平台基线。**Linux runner 落地条件**（建议⑦）：
   `apt install mesa-vulkan-drivers`（提供 `/usr/share/vulkan/icd.d/lvp_icd.*.json`），`MagicBlocks/tools/vulkan_env.sh`
   导出 `VK_DRIVER_FILES` 指向该 ICD；无 X11 时 `Swapchain.HiddenWindowCreate` 在 Linux 上**明确按平台跳过**（该用例的
   Windows 断言由 Windows runner 覆盖，不是"跳过就算过"——Linux 侧对应断言是 `Swapchain.HeadlessReportsUnsupported`）。

## 10. 明确不做（本轮）

L1 资源/内存/VMA 分页及其接口位、L2 场景/渲染图/命令录制回放、L3 primitives、L4 features、
GLES/Metal 后端实现（仅 `BackendType` 枚举留位）、iOS 工程、GLFW 窗口端、多队列/多线程。

## 11. 第 3 轮审阅的 7 条非阻断建议 · 落实状态

| # | 建议 | 落实 |
|---|---|---|
| 1 | 标题改「第 3 版」+ 补修订史 | ✅ 标题与「审阅历史」段 |
| 2 | §3-D3 回填 3 个 swapchain 用例 | ✅ D3 行现列 6 个用例 |
| 3 | 新增 `Offscreen.FrameIndexChangesOutput` | ✅ §6 |
| 4 | `RefusesPhysicalGpu` 补可注入枚举器 | ✅ §6（`testing::set_physical_device_override`） |
| 5 | 统一 `AndroidTuCompiles` 的 stub 头与 CMake 开关 | ✅ §6 + `src/backend/vulkan/stub/anativewindow.h`、`MB_ANDROID_TU_COMPILE_CHECK=ON` |
| 6 | 「零泄漏」改可计量 | ✅ §6（`VkValidationFeaturesEXT` + `best_practices`，error==0 且 回调总数>0） |
| 7 | §9.8 补 lavapipe 落地与 CI 条件 | ✅ §9.8 |
