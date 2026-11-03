# MagicBlocks L0+L1 —— 设备平台层与资源层

MagicBlocks 渲染引擎 SDK 的 **L0（device/）**：设备抽象、能力探测、交换链、渲染目标
（窗口与离屏**同一种对象**）、帧同步、输入事件流、生命周期与失败即报错。

以及 **L1（resources/）**：分页内存分配器（VMA v3.5.0 大块 + 偏移子分配）与
Buffer / Texture / Sampler / Pipeline / CommandList 资源对象。

本层**不包含**任何场景/渲染图（L2）、primitives（L3）、features（L4）。

> **架构速查**：[docs/ARCHITECTURE.md](docs/ARCHITECTURE.md) —— 文件地图、核心抽象与设计决策、
> L2-L4 接口契约、踩坑清单、验证闭环。展开后续层或动内部实现前先读这份。

## 硬约束（来自设计规格）

| 编号 | 约束 | 在本层的落地 |
|---|---|---|
| C4 | **禁止几何着色器** | 自检画面全部靠实例化绘制（`gl_InstanceIndex`）。门控 G1 扫描全部 shader/源码；`ShaderStage` 枚举只有 Vertex/Fragment/Compute，结构上无法表达 GS |
| C5 | 不出现玩法专用路径 | 门控 G3：backend 内禁玩法名，backend 外禁 `Vk*`/`vk*`/VMA |
| C6 | 命令模式 / 离屏一等公民 | 离屏渲染不需要窗口或显示服务：`OffscreenTarget` + CLI `render` |
| C7 | VMA v3.5.0 大块 + 偏移子分配 | **L1 已落地**：`VulkanPagingAllocator` 设备页 64 MB / Host 页 8 MB，句柄 = `{page_id, offset, size, id}` |
| G4 | 全仓零裸分配 | 门控 G4 扫描全部源码：无 `malloc/new` 裸分配；`vkAllocateMemory` 等 0 处；`vk_mem_alloc.h` 只在 `vulkan_allocator.cpp` include |
| M5 | 稳态零 VMA 调用 | `vma_alloc_count`/`vma_free_count` 增量在稳态帧必须为 0 |
| M6 | 超预算即报错 | over-budget 返回 `OutOfMemory`，消息含「需要/已有/预算」 |
| 不回退 | 能力不足就报错 | 设备强筛：`require_software_rasterizer=true` 时**绝不**回退物理 GPU |

## 目录

```
include/magicblocks/device/     SDK 公共面 · L0
  types.h        DeviceDesc / DeviceCaps / NativeSurface / TargetDesc
  result.h       Status / StatusCode / Result<T>
  events.h       InputEvent + EventStream（不读时钟）
  lifecycle.h    AppLifecycleEvent
  frame_sync.h   IFrameSync（不做 present）
  swapchain.h    ISwapchain（present 归这里）
  render_target.h IRenderTarget
  render_device.h IRenderDevice + create_render_device()
include/magicblocks/resources/  SDK 公共面 · L1
  handles.h      ResourceId / PageKind / 打包约定（page_id|offset|id）
  memory.h       IMemoryAllocator / AllocatorStats / BudgetDesc
  buffer.h       IBuffer（Vertex/Index/Uniform/Staging）
  texture.h      ITexture / ISampler / Format / TextureUsage
  pipeline.h     IPipeline + PipelineDesc / ShaderStage（无几何阶段）
  command.h      ICommandList（11 个方法，渲染通道内录制）
src/backend/vulkan/             Vulkan 后端（唯一出现 Vk* 的地方）
  vulkan_allocator.*            分页分配器（VMA 大块 + 64 KiB 子块）
  vulkan_resource_common.*      页注册表 + 内存绑定 + 一次性提交
  vulkan_buffer/texture/pipeline/command.*  L1 资源对象
src/core/image_io.*             PNG 读写（零第三方依赖）
src/cli/main.cpp                magicblocks-l0-cli
src/ui_cli/main.cpp             magicblocks-ui-cli
shaders/                        fullscreen.vert/.frag + ui_overlay.vert/.frag + embed_shaders.py
tests/magicblocks_test.cpp      25 个 L0 用例
tests/magicblocks_l1_test.cpp   20 个 L1 用例
tests/data/golden_l0.png        golden 基线（SwiftShader 产出）
tests/data/golden_ui.png        UI golden 基线
tools/                          环境脚本
```

## 构建

需要 CMake ≥ 3.22、Ninja、C++20 编译器、Vulkan SDK（含 `glslc`）、Python 3。
第三方依赖从 `third_party/_install/` 取（`googletest`、`vma`、`swiftshader`）。

```powershell
# 让 loader 能找到 SwiftShader（需要管理员；只写注册表指向 third_party 里的 ICD，不改动其内容）
powershell -ExecutionPolicy Bypass -File tools\register_swiftshader_icd.ps1

cmake -S MagicBlocks -B MagicBlocks\build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build MagicBlocks\build
ctest --test-dir MagicBlocks\build --output-on-failure
```

## CLI（无头离屏）

```bash
# 打印 DeviceCaps（JSON），不需要窗口
magicblocks-l0-cli device

# 离屏渲染一帧到 PNG
magicblocks-l0-cli render --width 320 --height 240 --frame-index 0 --output out.png
```

`--frame-index` 参与像素相位：`0` 与 `1` 的输出**不同**，同一 index 逐像素可复现。

退出码：`0` 成功 · `1` 运行失败 · `2` 参数错误 · `3` 需要提权才能让 loader 看到 SwiftShader。

## 测试

```bash
MagicBlocks\build\magicblocks_test.exe          # 85 个用例（25 L0 + 20 L1 + 40 UI/其它）
ctest --test-dir MagicBlocks\build              # 99 项：85 用例 + 4 门控 + 10 CLI + 自测
```

门控（spec §7）：

```bash
python tools/check_no_geometry_shader.py --root MagicBlocks   # G1
python tools/check_layer_purity.py       --root MagicBlocks   # G3
python tools/check_no_raw_alloc.py       --root MagicBlocks   # G4
python tools/check_deps_tests.py                              # G5 既有 191 用例回归
```

## L1 资源层要点

- **分页分配器**：`allocate()` **从不**自己触发 VMA —— 调用方必须先
  `reserve_pages(kind, n)` 预取页。设备页 64 MB（1024 个 64 KiB 子块）、Host 页 8 MB（128 个）。
- **句柄即偏移（M3）**：`ResourceId{page_id, offset, size, id}`，且 `offset` 必为 64 KiB 整数倍
  （分配器内部 `effective_align = max(requested, kPackGranularity)`）。
- **稳态零 VMA 调用（M5）**：`begin_frame()` 内部会 `trim()` 并对全空闲页累加 `idle_frames`，
  连续 120 帧全空闲的页才淘汰（M4）。稳态帧的 `vma_alloc_count`/`vma_free_count` 增量必须为 0。
- **超预算即报错（M6）**：返回 `OutOfMemory`，消息里给出「需要/已有/预算」三个数字。
- **整块绑定的页**：`VkImage` / 读回用 `VkBuffer` 只能一页一块，走
  `allocate_image_page` + `bind_image_page` / `bind_buffer_page`。**两者不可混用**：
  `vmaBindImageMemory` 拿 VkBuffer 会直接崩（曾如此，见 ARCHITECTURE 踩坑清单）。
- **读回行序**：`vkCmdCopyImageToBuffer` 产出的第 0 行是图像**底部**行，
  `readback_rgba8` 必须逐行翻转成「自左上角」，否则整幅图上下颠倒。

## 设备选择与 SwiftShader

本机枚举顺序为 `GPU0 = RTX 5060`、`GPU1 = SwiftShader`，且顺序不受保证。
SDK **不做** `devices[0]`，而是强筛软件光栅（`VK_PHYSICAL_DEVICE_TYPE_CPU` 或
`deviceName` 含 `SwiftShader`/`llvmpipe`/`lavapipe`）；`require_software_rasterizer=true`
时找不到就返回 `Unsupported`，**绝不**回退到物理 GPU。

`VK_EXT_memory_budget` 在 SwiftShader 上**不存在**（实测扩展共 83 个，无此项），
因此 `DeviceCaps` 的内存预算双来源：有扩展时取 `heapBudget` 之和，
否则取 `VkMemoryHeap.size` 之和，并由 `memory_budget_from_extension` 标明来源。

### 提权下的 loader 行为

以管理员运行时，Vulkan loader **忽略** `VK_DRIVER_FILES` / `VK_ICD_FILENAMES`，
只认注册表 `HKLM\SOFTWARE\Khronos\Vulkan\Drivers`。`tools/register_swiftshader_icd.ps1`
就是按这条主路径注册（只写注册表，不碰 `third_party/` 内容）。非管理员环境改用
`tools/vulkan_env.ps1` 设置 `VK_DRIVER_FILES`。

## 图像朝向契约

`vkCmdCopyImageToBuffer` 的行序自图像顶部开始，与 `IRenderTarget::readback_rgba8`
的契约（左上为第 0 行）一致，**读回端 1:1 拷贝、不翻行**。着色器里 `cy` 用
`kCols-1-cy` 把实例 0 放到 NDC 的 +y 端，使其落在图像左上角。

> 这里曾经有坑：读回端翻行 + 着色器翻 `cy` 相互抵消，画面朝向"看起来对"但两处逻辑
> 都是反的，且 golden 一旦跟着错基线生成，测试就会"通过"。现在
> `Offscreen.MatchesGoldenSingleBaseline` 额外做**结构化断言**：左上角必须是纯红
> (R>200, B<60)、golden 自身也必须带这个标记、右上角必须**不是**红色。这三条不依赖
> golden 内容，错朝向会立刻失败（已用翻转 golden 的负向测试验证）。

## Linux

`apt install mesa-vulkan-drivers`（提供 `lavapipe`，ICD 在
`/usr/share/vulkan/icd.d/lvp_icd.*.json`），然后 `source tools/vulkan_env.sh`。
golden 跨驱动共享同一份基线：lavapipe 与 SwiftShader 都必须比对通过
（≤1% 像素差 + 上述结构化断言）。无 X11 时窗口用例由平台条件编译排除（GUI 通路由 Windows 覆盖）。

## 与 L2 的衔接

- 裸分配已全部清零（G4 绿）：L0 的 `// VMA-injection` 标记块已由 L1 分页分配器取代，
  `VkImage` / 读回缓冲走 `allocate_image_page`，资源对象走桶内 64 KiB 子块。
- L2（scene/）通过 `IRenderDevice::create_buffer/create_texture/create_sampler/
  create_pipeline/create_command_list` 拿资源，**不直接碰** `VulkanPagingAllocator`。
- 帧边界挂点：`IFrameSync::advance_frame_index()` + `IMemoryAllocator::begin_frame()`。
- L2 需要新资源类型时，在 `resources/` 加接口 + 在 `vulkan_` 后端加实现，
  公共头不出现任何 `Vk*`（门控 G3 强制）。
