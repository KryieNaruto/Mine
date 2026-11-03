# MagicBlocks L0 基础架构索引

> **用途**：这是给后续上下文（人 / AI）的**速查索引**，不是教程。
> 读这一份就能定位到"某个东西在哪、为什么这么设计、改它要碰什么、怎么验证"。
> 权威顺序：**设计规格 > 落地计划 > 本索引 > 代码注释**。本索引与代码不符时以代码为准，并回来修本索引。
>
> **按需加载**：本文件刻意放在 `MagicBlocks/docs/`，不放在 `docs/superpowers/`（那是 superpowers
> 工作流的产物目录，放进去容易被误当成计划文档）。仓库内没有任何规则文件（`CLAUDE.md` /
> `AGENTS.md` / CI）引用它，**不会自动进上下文**，只在显式读取时加载。
>
> **路径约定**：本文中除特别说明外，**所有路径都相对仓库根**（`Mine/`），不是相对本文件。
> 例如 `MagicBlocks/src/...`、`tools/check_*.py`，以及 §8/§13 的命令都应在仓库根执行。
>
> - 规格：`docs/superpowers/specs/2026-11-03-magicblocks-render-engine-sdk-design.md`
> - 计划：`docs/superpowers/plans/2026-11-03-magicblocks-l0.md`（第 3 版，审阅 87.7 通过）
> - 状态：**L0 已落地全绿**（ctest 32/32，0 失败 0 跳过）
> - 更新于：2026-11-03

---

## 1. 当前进度总览

| 层 | 内容 | 状态 |
|---|---|---|
| **L0** | `device/` 设备与平台层 | ✅ **已完成** |
| **L1** | `resources/` 资源层 + VMA 分页分配器 + ICommandList | ✅ **已完成**（20 用例全绿） |
| L2 | scene / 渲染图 | ⬜ 未开始 |
| L3 | primitives | ⬜ 未开始 |
| L4 | features（玩法层，**唯一允许出现玩法名**的地方） | ⬜ 未开始 |

L2 要开始时的第一件事：读 §7「给后续层的接口契约」，别重新发明抽象。

---

## 2. 十条硬约束（违反即返工）

来自 spec §2，**每条都有门控或测试兜底**，不是口号。

| 编号 | 约束 | 落地方式 | 谁在守 |
|---|---|---|---|
| **C4** | **禁止几何着色器**（硬红线） | 全部靠实例化绘制（`gl_InstanceIndex`），无 `geom` stage | G1 门控 + 代码评审 |
| **C5** | SDK 式接口，不出现玩法专用路径 | 公共头零 `Vk*`；backend 内禁玩法名 | G3 门控 |
| **C6** | 命令模式 + **离屏是一等公民** | 离屏不需要窗口/显示服务，CLI 可无头出图 | `OffscreenTarget` + CLI `render` |
| **C7** | VMA v3.5.0 大块 + 偏移子分配 | **L1 已落地**：`VulkanPagingAllocator`（设备页 64 MB / Host 页 8 MB）。L0 的 `// VMA-injection` 标记已全部消除 | G4 门控 |
| **D8** | 失败即报错，不静默降级 | `Result<T>`/`Status` 带 code + message | 全部入口 |
| 不回退 | 能力不足就报错，**绝不**回退物理 GPU | `require_software_rasterizer=true` 时强筛软件光栅 | `Device.RefusesPhysicalGpu` |
| 层纯净 | 上层不碰下层细节 | 公共头 `include/magicblocks/`，backend 私有 | G3 门控 |
| 无裸分配 | 全仓不得直接分配内存 | 全仓 0 处裸分配；`vk_mem_alloc.h` 只在 `vulkan_allocator.cpp` include | G4 门控 |
| 依赖稳定 | 既有测试不回归 | 191 个既有测试必须保持 0 失败 0 跳过 | G5 门控 |
| 可复现 | 同一输入同一输出 | golden 比对 + 确定性像素测试 | `Offscreen.*` |

---

## 3. 文件地图（按职责）

```
MagicBlocks/
├─ include/magicblocks/device/     ← 公共面，永久头根（L2-L4 在同级扩展）
│   ├─ types.h          DeviceDesc / DeviceCaps / NativeSurface / TargetDesc / Format
│   ├─ result.h         Status / StatusCode / Result<T>
│   ├─ render_device.h  IRenderDevice + create_render_device() 工厂
│   ├─ render_target.h  IRenderTarget（窗口/离屏**同一接口**）
│   ├─ swapchain.h      ISwapchain（present 归这里）
│   ├─ frame_sync.h     IFrameSync（**不做 present**）
│   ├─ events.h         InputEvent / EventStream（不读时钟）
│   └─ lifecycle.h      AppLifecycleEvent
├─ include/magicblocks/resources/  ← 公共面 · L1
│   ├─ handles.h        ResourceId / PageKind / 句柄打包约定
│   ├─ memory.h         IMemoryAllocator / AllocatorStats
│   ├─ buffer.h         IBuffer（Vertex/Index/Uniform/Staging）
│   ├─ texture.h        ITexture / ISampler / Format / TextureUsage
│   ├─ pipeline.h       IPipeline + PipelineDesc / ShaderStage（无几何阶段）
│   └─ command.h        ICommandList（11 个方法）
├─ src/backend/vulkan/             ← 唯一出现 Vk* 的地方
│   ├─ vk_common.h                 后端私有共享（结果字符串/错误格式化/能力填充）
│   ├─ vulkan_instance.*           实例 + 校验层 + debug messenger
│   ├─ vulkan_device.cpp           物理设备选择 + DeviceCaps 填充
│   ├─ vulkan_render_device.*      IRenderDevice 实现 + create_vulkan_device() + L1 工厂
│   ├─ vulkan_target.*             OffscreenTarget + create_image + readback
│   ├─ vulkan_allocator.*          分页分配器（VMA 大块 + 64 KiB 子块 + 页淘汰）
│   ├─ vulkan_resource_common.*    页注册表 + VkDeviceMemory 绑定 + 一次性提交
│   ├─ vulkan_buffer.*             IBuffer 实现（桶内子块 + bind_buffer_memory）
│   ├─ vulkan_texture.*            ITexture / ISampler 实现
│   ├─ vulkan_pipeline.*           IPipeline 实现（含 hash()）
│   ├─ vulkan_command.*            ICommandList 实现（11 个方法 + 命令计数）
│   ├─ vulkan_swapchain.*          ISwapchain 实现，按 NativeSurface::Kind 分派
│   ├─ vulkan_swapchain_{win32,xcb,android}.*   各平台 surface 实现
│   ├─ vulkan_swapchain_{win32,xcb}_stub.cpp   对面平台的符号占位
│   ├─ vulkan_frame_sync.*         帧环 + per-slot fence
│   ├─ vulkan_ui_overlay.*         HUD 覆盖层（实例化，无几何着色器）
│   └─ vulkan_draw.*               自检绘制（4x4 实例化色块）
├─ src/core/
│   ├─ device_factory.cpp          公共 create_render_device → 后端工厂
│   └─ image_io.*                  PNG 读写（零第三方依赖，可读回 golden）
├─ src/cli/main.cpp                magicblocks-l0-cli：device / render 子命令
├─ src/ui_cli/main.cpp             magicblocks-ui-cli：render / stats / present
├─ shaders/                        fullscreen.vert/.frag + ui_overlay.vert/.frag + embed_shaders.py
├─ tests/magicblocks_test.cpp      25 个 L0 用例
├─ tests/magicblocks_l1_test.cpp   20 个 L1 用例
├─ tests/ui_test.cpp               UI / 覆盖层用例
├─ tests/data/golden_l0.png        golden 基线（含结构化断言，见 §6）
├─ tests/data/golden_ui.png        UI golden 基线
└─ tools/                          环境脚本（SwiftShader 注册 / 环境变量）
```

根目录门控：`tools/check_no_geometry_shader.py`(G1)、`check_layer_purity.py`(G3)、
`check_no_raw_alloc.py`(G4)、`check_deps_tests.py`(G5)、自测 `tools/tests/test_check_scripts.py`。

---

## 4. 核心抽象与关键决策

### 4.1 IRenderDevice（设备入口）

```cpp
create_render_device(DeviceDesc) -> Result<unique_ptr<IRenderDevice>>   // 公共工厂
  ├─ create_offscreen_target(TargetDesc) -> Result<unique_ptr<IRenderTarget>>   // 无需窗口
  ├─ create_window_target(TargetDesc)    -> Result<unique_ptr<IRenderTarget>>
  ├─ create_swapchain(NativeSurface, extent) -> Result<unique_ptr<ISwapchain>>
  ├─ frame_sync() -> IFrameSync*
  ├─ notify_lifecycle(AppLifecycleEvent) -> Status
  ├─ caps() -> const DeviceCaps&
  └─ wait_idle() -> Status
```

**为什么 `init()` 是两段式**：设备创建要做能力探测与强筛，构造函数不该失败。
所以 backend 实现是 `VulkanRenderDevice::init(desc, override)`，工厂里失败的 `init` 直接返回 `Result` 错误。

**生命周期销毁顺序（踩过坑，别改）**：
```
~VulkanRenderDevice:
  wait_idle → frame_sync_.reset()（先销毁 fence）→ vkDestroyCommandPool
            → vkDestroyDevice → vkDestroyInstance
```
`frame_sync_` 用 `unique_ptr` 而非值成员，就是为了能在这个顺序上显式 `reset()`。
曾因值成员 + placement-new 导致 `0xC0000005` 访问违例。

### 4.2 DeviceDesc vs DeviceCaps —— 命名零重名

**约定**：`DeviceDesc` 只放**请求项**（`request_*` / `requested_*` / `require_*`），
`DeviceCaps` 只放**能力项**（`*_supported` / `supported_*` / `max_*`）。
两边零重名，避免"到底是请求还是能力"的歧义。

请求不可满足 → `Unsupported`，**清单里列出实际能力**便于排查，绝不静默降级。

### 4.3 窗口与离屏同构（D4）

`IRenderTarget` 一个抽象承载两种 kind，上层不为窗口/离屏写两套代码。
`TargetDesc::Kind` 区分。**离屏是一等公民**：CI/golden/录制回放全跑在离屏上。

### 4.4 职责边界（容易写错的三个）

| 东西 | 归谁 | 说明 |
|---|---|---|
| present | `ISwapchain` | `IFrameSync` **不做** present，容易顺手写错 |
| 帧序号→槽位 + fence 等待 | `IFrameSync` | 只是环形分配，不含呈现 |
| `IMemoryAllocator` / `ICommandList` | **L1**，不在 L0 | L0 刻意不出现这两个接口名 |

### 4.5 设备选择：强筛，不取 devices[0]

本机枚举顺序实测为 `GPU0 = RTX 5060`、`GPU1 = SwiftShader`，**顺序无保证**。
SDK 只在所有物理设备中按条件强筛软件光栅（`VK_PHYSICAL_DEVICE_TYPE_CPU`
或 deviceName 含 `SwiftShader`/`llvmpipe`/`lavapipe`）。

`require_software_rasterizer=true`（默认）时找不到就返回 `Unsupported` 并列出见过的设备名，
**绝不回退到 RTX**。测试钩子 `set_physical_device_override()` 用于 `Device.RefusesPhysicalGpu`。

### 4.6 内存预算双来源

SwiftShader **没有** `VK_EXT_memory_budget`（实测 83 个扩展里无此项），所以：

```
有扩展  → heapBudget 之和，memory_budget_from_extension = true
无扩展  → VkMemoryHeap.size 之和，memory_budget_from_extension = false
```
`DeviceCapsBudgetSourceMatchesExtension` 断言这个标志与实际扩展存在性一致。
**不要**把 `memory_budget == 0` 当"无预算"处理，两者都保证 > 0。

---

## 5. 平台适配模式（L1+ 加平台时照抄）

**核心思路**：公共头零平台宏；平台差异只在 backend 的 TU 划分 + `NativeSurface::Kind` 分派。

```
vulkan_swapchain.cpp        switch (surface.kind) → create_{win32,xcb,android}_surface()   ← 无 #if
vulkan_swapchain_win32.cpp  #if defined(VK_USE_PLATFORM_WIN32_KHR) 真实实现
vulkan_swapchain_xcb.cpp    #if defined(VK_USE_PLATFORM_XCB_KHR)   真实实现
vulkan_swapchain_android.cpp 总是编译；无 NDK loader 符号时返回 Unsupported
vulkan_swapchain_*_stub.cpp  提供对面平台的符号，避免 LNK2019 且保持分发点干净
```

`CMakeLists.txt` 里 `MB_SURFACE_SRC` 按平台选真实实现 + 对面 stub。
**`vulkan_swapchain_android.cpp` 永远参与编译**，配合 `option(MB_ANDROID_TU_COMPILE_CHECK)`
（OFF 时 FATAL_ERROR）实现"Android TU 至少有语法校验"，对应 `Swapchain.AndroidTuCompiles`。

`src/backend/vulkan/stub/anativewindow.h` 提供非 NDK 构建下的最小 `ANativeWindow` 声明。

---

## 6. 图像朝向与 golden（踩过大坑，务必先读）

### 契约
`IRenderTarget::readback_rgba8` 输出**行序自左上**、逐像素 `R,G,B,A`。

### 实际实现
`vkCmdCopyImageToBuffer` 的行序本身就自图像顶部开始，与契约一致。
**读回端 1:1 拷贝，不翻行。** 着色器里 `cy` 用 `kCols-1-cy` 把实例 0 放到 NDC 的 +y 端，
使其落在图像**左上角**。

### 曾经的双重错误
读回端翻行 + 着色器翻 `cy` **相互抵消** → 画面朝向"看着对"，但两处逻辑都是反的。
更危险的是：重新生成 golden 时把错误输出当基线，测试就"全绿"了。

### 现在的防线
`Offscreen.MatchesGoldenSingleBaseline` 做两层校验：

1. **像素比对**：与 golden 逐像素比较，容差 ≤1%（每通道 ≤2）
2. **结构化断言（不依赖 golden 内容）**：
   - 渲染结果左上角必须是纯红（`R>200` 且 `B<60`）
   - golden **自身**左上角也必须带这个标记
   - 右上角必须**不是**红色

第 2 层专门防"错基线自我固化"。已用翻转 golden 做过负向测试，确认会 `FAILED`，不是空断言。

### 重生成 golden 的规矩
只在**渲染逻辑有意变更**后重生成，且生成后必须确认左上角纯红。步骤见
`MagicBlocks/tests/data/README.md`。

---

## 7. 给后续层的接口契约（L2-L4 开工前必读）

### L1（资源 / VMA）—— 已完成，这里是它的契约

**分层**：`include/magicblocks/resources/` 只有纯 C++（零 `Vk*`），
`src/backend/vulkan/vulkan_{allocator,resource_common,buffer,texture,pipeline,command}.*` 是实现。

**分页分配器调度约定（最容易踩的坑）**：
- `allocate()` **从不**自己触发 VMA。调用方必须先
  `reserve_pages(kind, n)` 预取页，否则 `allocate()` 返回 `NoSpace`/`OutOfMemory`。
  所以用例和 L2 都得先 reserve 再创建资源。
- 页规格：设备页 64 MB（1024 槽）、Host 页 8 MB（128 槽）；`MemoryKind` =
  Vertex / Index / Uniform / Texture / Staging。Texture 与 Staging 都映射到
  `PageKind::HostVisible`（见 rulings 裁定 2）。
- `begin_frame()` 内部会 `trim()`，并对全空闲页累加 `idle_frames`；连续 **120 帧**全空闲才淘汰（M4）。
- 稳态帧 `vma_alloc_count`/`vma_free_count` 增量必须为 0（M5）。想加"每帧新建"的东西前先算清这个。

**句柄**：`ResourceId{page_id, offset, size, id}`，`pack()` 要求 `offset % 64 KiB == 0`，
因此分配器内部一律 `effective_align = max(requested, kPackGranularity)`（rulings 裁定 3）。

**整块绑定的页（重要）**：
- `VkImage`（离屏附件）和读回用 `VkBuffer` 必须一页一块，走
  `allocate_image_page` + `bind_image_page` / **`bind_buffer_page`**，销毁时 `release_image_page`。
- **`vmaBindImageMemory` 不能用来绑 `VkBuffer`** —— 传错类型 SwiftShader 直接段错误
  （踩过，见 §踩坑清单）。
- 页注册表（`vulkan_resource_common.*`）按 `page_id` 记 `VkDeviceMemory`+offset，
  `create_page` 里必须 `sync_page_memory()`、`drop_page` 里必须 `forget_page_memory()`，
  否则设备态所有绑定都报「读回页未登记 VkDeviceMemory」。

**读回行序**：`vkCmdCopyImageToBuffer` 产出第 0 行是图像**底部**行，
`readback_rgba8` 必须逐行翻转成「自左上角」。**别删这个翻转** ——
`fullscreen.vert` 里的 `(kCols-1-cy)` 负责的是格位朝向，和读回行序是两件事，两者必须同时正确。

**`ICommandList` 一共 11 个方法**（见 `command.h`）。`recording()` 在设备态由
`begin_pass()` 置真（软件态由 `init()` 置真）；`submit_and_wait()` 对未开启的列表是合法的 `Ok`。

### L2（场景 / 渲染图）怎么接
- 资源一律通过 `IRenderDevice::create_buffer/create_texture/create_sampler/
  create_pipeline/create_command_list` 拿，**不要**直接 new `Vulkan*` 或碰 `VulkanPagingAllocator`。
- `vulkan_draw.*` 的 `draw_selftest()` 是**测试绘制例程**，L2 另起渲染图，届时它退化为测试专用。
  不要把它当渲染图的起点。
- `OffscreenTarget` 只做"分配/布局/读回"，不含编排逻辑。L2 的编排放 `src/scene/`。
- 帧边界挂点：`IFrameSync::advance_frame_index()` + `IMemoryAllocator::begin_frame()`。

### L4（features）唯一允许玩法名
G3 门控在 backend 内禁玩法名（`sketch`/`blocks`/`voxel`/`stamp`/`squeeze`/`捏捏乐`/`涂鸦`/`积木`/`叠叠乐`/`印章`）。
**豁免**：`magicblocks`（SDK 自己的名字，否则永远误报）与 `timestamp*`。
L4 的玩法代码放 `src/features/`，不受此限。

---

## 8. 构建与验证（复制粘贴即可）

### 构建
```powershell
# 一次性环境前提：让 loader 找到 SwiftShader（需管理员）
powershell -ExecutionPolicy Bypass -File MagicBlocks\tools\register_swiftshader_icd.ps1

cmake -S MagicBlocks -B MagicBlocks\build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build MagicBlocks\build
```
工具链：CMake ≥3.22 + Ninja + C++20（MSVC 19.51 / VS18）+ Vulkan SDK 1.4.357（含 `glslc`）+ Python 3。
依赖从 `third_party/_install/` 取（googletest-v1.15.2 / vma-v3.5.0 / swiftshader-master / glfw-3.4）。

### 验证（全绿才算完）
```powershell
ctest --test-dir MagicBlocks\build --output-on-failure      # 必须 99/99，0 失败 0 跳过

$py = "C:\Users\Administrator\AppData\Local\Programs\Python\Python313\python.exe"
& $py tools/check_no_geometry_shader.py --root MagicBlocks   # G1 rc=0
& $py tools/check_layer_purity.py       --root MagicBlocks   # G3 rc=0
& $py tools/check_no_raw_alloc.py       --root MagicBlocks   # G4 rc=0
& $py tools/check_deps_tests.py                              # ran=191 failed=0 skipped=0
```

> PowerShell 有时对绿灯 Python 脚本报 `exit code: 1`（stderr 有输出所致）。
> **判绿看打印的 `[Gx] OK` 行与 `rc=` 值**，不要只看 `$LASTEXITCODE`。

### CLI（无头离屏，C6 的证据）
```powershell
MagicBlocks\build\magicblocks-l0-cli.exe device
MagicBlocks\build\magicblocks-l0-cli.exe render --width 320 --height 240 `
    --frame-index 0 --output out.png
```
退出码：`0` 成功 · `1` 运行失败 · `2` 参数错误 · `3` 需提权才能让 loader 看到 SwiftShader。
`--frame-index` 参与像素相位：0 与 1 输出不同，同一 index 逐像素可复现。

---

## 9. 测试清单（99 项）

**25 个 L0 gtest 用例 / 8 个套件** —— 覆盖 D1-D8：

| 套件 | 用例 | 守的是 |
|---|---|---|
| Device | CreateAndDestroy · ValidationCountsAreMeasured · CapsAreComplete · CapsBudgetSourceMatchesExtension · CapsValuesAreConsistent · UnsupportedBackendFails · RequestUnsupportedFails · RefusesPhysicalGpu | D1/D2/D8 + 不回退 |
| Errors | SupportedBackendSucceeds | D8 |
| Target | OffscreenReadbackIsRgba8 · WindowAndOffscreenSameInterface | D4 |
| FrameSync | InFlightCountFromDesc · SlotReuseAfterSignal · RunsFifteenFrames | D5 |
| Input | StreamRoundTrip · TimestampIsPreserved | D6 |
| Lifecycle | SurfaceChangedRebuilds | D7 |
| Swapchain | HeadlessReportsUnsupported · HiddenWindowCreate · RebuildOnResize · DestroyThenRecreate · AndroidTuCompiles | D3 |
| Offscreen | RendersDeterministicPixels · MatchesGoldenSingleBaseline · FrameIndexChangesOutput | C6 + 可复现 |

**20 个 L1 gtest 用例 / 8 个套件** —— 覆盖 R1-R6 + M1-M6：

| 套件 | 用例 | 守的是 |
|---|---|---|
| Allocator | ReserveAndAllocate · HandleOffsetAlignment · SteadyStateZeroVmaCalls · OverBudgetReportsNeedHaveBudget · PageEvictionAfterIdleFrames · FreeReturnsToBucket | R1-R3/M3/M5/M6/M4 |
| Handle | PackRoundTrip · RejectsUnalignedOffset · UnpackFields | M3 + rulings 裁定 3 |
| Buffer | CreateVertexIndexUniform · WriteAndReadBack · NotMappableRejectsWrite | R4/G4 |
| Texture | CreateUploadAndReadbackRoundTrip · StagingGoesThroughHostVisiblePage | R4 + rulings 裁定 2 |
| Sampler | SlotIndexIsOneBased | R5 |
| Pipeline | CreateWithStages · RejectsGeometryStage | C4/G1 结构强制 |
| Command | RecordsAllOps · RejectsInvalidRecordingOrder | R6 |
| Upload | StagingGoesThroughAllocator | G4 |

**74 项其余**：G1/G3/G4/G5 门控 ×4 + CLI 用例（`magicblocks_cli_*`）+ UI 用例 + 门控自测。

平台条件：`HiddenWindowCreate`/`RebuildOnResize`/`DestroyThenRecreate` 在 `#if defined(_WIN32)` 下，
无 X11 时由平台条件编译排除（GUI 通路由 Windows 覆盖）。`Golden.RequiresSwiftShaderDevice`
在非软件设备时 **fail 而非 skip**，保证"0 跳过"不是靠跳过凑出来的。

---

## 10. 已知陷阱（血泪清单，改代码前扫一眼）

1. **不要用 PowerShell `Get-Content`/`Set-Content` 改这些源文件。** 曾两次把 UTF-8 中文注释转成 GBK，
   其中一次导致 shader 编译静默用了旧内容、排查很久。用编辑器/写文件工具。
2. **`.ps1` 文件必须带 UTF-8 BOM**，否则 Windows PowerShell 5.1 按 ANSI 读，中文注释会破坏解析。
3. **`half` 是保留字**（GLSL/HLSL），shader 里变量别叫 `half`（已改 `half_extent`）。
4. **`namespace magicblocks` 会让 G3 的 `blocks` 永远误报** —— 已加豁免，但改门控时别把豁免删了。
   门控自测 `test_check_scripts.py` 会盯着这个行为。
5. **`// VMA-injection` 标记要么与调用同行（行尾注释），要么独占一行且紧邻调用** ——
   独占一行时中间插空行会让注入区提前结束、该调用被判违规。改完跑 G4 别只靠肉眼看。
6. **错误字符串里别写 `vkAllocateMemory(` 这类带括号的调用**，会被 G4 正则当真实调用匹配。
7. **设备顺序不保证**，永远不要 `devices[0]`。
8. **`CTest` 的 ctest 输出里 "100% tests passed" 也不能全信** —— 曾出现 golden 跟着错误输出重生成后
   测试全绿的情况。看 §6 的结构化断言。
9. **`IFrameSync` 里不要写 present**，逻辑上属于 `ISwapchain`。
10. **删 probe/临时测试文件后记得重跑门控**，残留文件会污染 G3 扫描结果。
11. **`vmaBindImageMemory` 不能绑 `VkBuffer`。** L1 读回缓冲是 `VkBuffer` 但走的是
    "整页专用分配"路径，一度被喂进 `vmaBindImageMemory`，SwiftShader 直接 `0xC0000005`
    段错误（`Target.OffscreenReadbackIsRgba8` 崩在本机 probe 里才定位到）。
    现在 `VulkanPagingAllocator` 同时提供 `bind_image_page` / `bind_buffer_page`，**别混用**。
12. **Ninja 的依赖扫描在本机是关闭的**（`unscanned` 规则、无 `.obj.d`），
    所以**改头文件不会触发重编**。曾因此让 `vulkan_draw.cpp.obj` 留在旧版
    `OffscreenTarget` 布局上，`color_` 读成 nullptr，表现为"目标无颜色附件"。
    **改头后必须 `ninja -t clean` 或手动删 obj 再全量构建。**
13. **`VMA_MEMORY_USAGE_AUTO_PREFER_*` 在本机 SwiftShader 上必失败**
    （`vmaAllocateMemory` 返回 `VK_ERROR_FEATURE_NOT_PRESENT` -8），因为 AUTO_* 需要
    分配器带扩展标志。`vulkan_allocator.cpp` 一律用 `GPU_ONLY` / `CPU_TO_GPU`。
    `mem_req.memoryTypeBits` 也不能填 0（0 = 不允许任何类型）。
14. **读回行序必须翻转**：`vkCmdCopyImageToBuffer` 第 0 行是图像底部行。
    `fullscreen.vert` 的 `(kCols-1-cy)` 管的是格位朝向，两件事都要对；
    只对一边就会出现"朝向看着对但 vUv 渐变反向"的假绿基线（旧 golden 正是这么来的）。
15. **`device::Result` 的 `message` 是方法 `message()`，`device::Status` 的 `message` 是字段。**
    两者写反是高频编译错误。`Result` 取 code 用 `status().code`。

---

## 11. 环境前提（非代码，换机器要重做）

| 项 | 内容 | 为什么 |
|---|---|---|
| SwiftShader 注册 | `HKLM\SOFTWARE\Khronos\Vulkan\Drivers\<icd.json 绝对路径> = 0` | **提权会话下 loader 忽略 `VK_DRIVER_FILES`**，只认注册表 |
| 非管理员替代 | `source tools/vulkan_env.ps1` 设 `VK_DRIVER_FILES` | 同上反向 |
| ICD 实际路径 | `third_party/_install/swiftshader-master/release/vk_swiftshader_icd.json` | 脚本首选此路径，找不到会回退搜索 |
| Linux | `apt install mesa-vulkan-drivers` → `source tools/vulkan_env.sh` | 提供 lavapipe |

这是**环境前提**，不是代码回退。设备强筛找不到软件光栅就返回 `Unsupported`。

---

## 12. 未做 / 待确认（诚实的边界）

- **Linux/lavapipe 未实测**：只做了代码路径准备。golden 能否跨驱动共用同一基线，
  需要真有 Linux 环境跑过才算数。不做"应该可以"的宣称。
- **Android 未实测**：`vulkan_swapchain_android.cpp` 永远是语法校验级（无 NDK loader 符号时
  返回 `Unsupported`），非真机验证。
- **iOS/Metal、GLES**：只留枚举位，本轮返回 `Unsupported`。
- **Wayland/Darwin surface**：`NativeSurface::Kind` 有枚举，无实现。
- **L1 已把裸分配清零**：`vkAllocateMemory` 等 0 处，`// VMA-injection` 标记已全部移除（G4 绿）。
- **L1 未做**：多线程/多队列（G5 单队列单线程至今成立）；`IBuffer` 的
  `MapPersistent` 之外的映射模式；纹理 mipmap 自动生成（当前只支持调用方给全链）。
- **工作区仍有未提交文件**：`MagicBlocks/`、四个门控脚本、两份文档为 untracked，尚未 commit。

---

## 13. 改代码时的验证闭环

任何改动后，按这个顺序跑，全绿才算完：

```
1. ctest --test-dir MagicBlocks\build --output-on-failure     → 99/99
2. G1 / G3 / G4 门控                                           → 全 rc=0
3. tools/check_deps_tests.py                                   → 191 failed=0 skipped=0
4. 若改了渲染逻辑：重生成 golden + 确认左上角纯红 + 重跑 Offscreen 套件
5. 若改了门控脚本：tools/tests/test_check_scripts.py           → 36/36
6. 若**改了任何头文件**：先 `ninja -C MagicBlocks\build -t clean` 再全量构建（见 §10.12）
```
