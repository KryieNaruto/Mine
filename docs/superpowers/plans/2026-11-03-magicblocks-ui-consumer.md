# MagicBlocks UI 消费层（L5）搭建计划 · 第 1 版

日期：2026-11-03 · 规格：`docs/superpowers/specs/2026-11-03-magicblocks-ui-consumer-design.md`
前置：L0 `device/` 已落地；L1 `resources/` 由并行会话推进（本轮**只预留桩**，不阻塞）

## 0. 本轮范围

### 0.1 一句话范围

只落 **UI 消费层**（spec 的 §2、§4~§8）：公共头 `magicblocks/ui/*`（9 个）、
`src/ui/` 实现（13 个文件）、Vulkan 侧帧渲染器桩 `vulkan_ui_overlay.{h,cpp}`、
CLI `magicblocks-ui-cli`、单测 `tests/ui_test.cpp`（40 例）、两条 UI 专用门控脚本。

SDK 的 L1/L2/L3 正在被并行会话改写。凡是"上层需要但下层还没给"的东西
（场景快照、离屏图元上屏、命令列表），一律按 **`ui::stub`** 的一等公民落位，
签名与将来的真接口保持一致；替换时 UI 层零改动。

### 0.2 本轮要闭合的硬约束

| 约束 | 落地方式 | 判定方式 |
|---|---|---|
| **§5.1 / §5.4** 上屏路径零 CPU 回读 | `IFrameRenderer` 故意**没有** readback/map 成员；回读只存在于 `offscreen` 命名的文件 | `check_ui_no_readback.py`（UIG1） |
| **§5.2** 单帧单次 submit | 提交只允许出现在文件名含 `present` 的适配器里 | `check_ui_single_submit.py`（UIG2） |
| **§5.5** 离屏仅用于 CI/golden | `TargetMode::Offscreen` 是显式配置项，不是降级动作 | `ui_UiApp.OffscreenModeRunsFrames` |
| **§7** 输入仲裁 ①HUD→②工具→③相机 | 归属在 `PointerDown` 时**一次判定**，移动中不改判 | `ui_Input.*` |
| **§8** 立即模式自绘 UI | HUD 每帧重排 + 重发图元，不持有前帧状态 | `ui_Hud.DrawIsDeterministic` |
| **G1** 无几何着色器 | UI 四边形用 `gl_InstanceIndex` 实例化 + SSBO | `check_no_geometry_shader.py` |
| **G3** SDK 无玩法分支 | `src/ui/**` 不出现 sketch/blocks/voxel/stamp | `check_layer_purity.py` |
| **G4** 零裸分配 | UI 实例缓冲走 L1 分页分配器（Staging 桶子块 + 按偏移绑定） | `check_no_raw_alloc.py` |
| **§6 G5** 稳态帧 VMA 调用 == 0 | 实例缓冲预分配到硬上限（2048），热路径只 `memcpy`；扩容时才重新 reserve | 代码结构 + `ui_Perf.*` |
| **不回退、不静默** | 桩明确返回 `Unsupported` / `RecordFailed`；`render_into_device_image()` 返回 false | `ui_Presenter.*` / `ui_UiApp.*` |

## 1. 关键事实核对

| 断言 | 实测 |
|---|---|
| `ui::stub` 里的实体必须能被公共头引用 | ✅ 在 `router.h`/`overlay_impl.h` 里加 alias 块（`using stub::IScene;` 等） |
| `device::Vec2f` 与 `ui::Vec2` 是两套类型 | ✅ `types.h` 提供 `from_device()` / `to_device()` |
| `UiApp` 能否 `dynamic_cast` | ❌ 故意非多态 → 用 `friend struct UiAppAccess` + `as_internal()` |
| `Presenter` 能否整体赋值 | ❌ 含引用/不可赋值成员 → placement-new + `set_config/reset` |
| `MemoryStateStore` 的 pimpl 析构在哪 | 只有在 `router.cpp` → `Impl` 里改存裸指针 + `delete` |
| 着色器符号名怎么来 | `embed_shaders.py` 按**文件名**生成 → `kUi_overlayVert`（非驼峰） |
| `windows.h` 的 `min`/`max` 宏 | ✅ 会打断 `std::min` → 在包含之后 `#undef` |
| SwiftShader 的内存类型 | 只有 host-visible；`VMA_MEMORY_USAGE_GPU_ONLY + DEVICE_LOCAL` 会 `FEATURE_NOT_PRESENT` |

## 2. 落地结果

### 2.1 新增/改动文件

| 层 | 文件 | 说明 |
|---|---|---|
| 公共头 | `include/magicblocks/ui/{types,sdk_stub,scene_stub,overlay,frame_renderer,input,hud,router,perf,presenter,ui_app}.h` | 11 个；桩集中在 `ui::stub` |
| UI 实现 | `src/ui/{router,input_router,overlay_impl,overlay_renderer,hud,perf_governor,presenter,presenter_host,presenter_offscreen,offscreen_target,ui_app_internal,ui_app}.{h,cpp}` | 13 个文件；**零 Vk* 类型** |
| 后端桩 | `src/backend/vulkan/vulkan_ui_overlay.{h,cpp}` | 因为 G3 才放 backend：实例化四边形、单 drawcall、LoadOp::Load |
| 着色器 | `src/backend/vulkan/shaders/ui_overlay.{vert,frag}` | SSBO 实例 + 单次 y 翻转 + alpha 混合 |
| CLI | `src/ui_cli/main.cpp` | `render` / `stats` / `present`；退出码 0/1/2 |
| 单测 | `tests/ui_test.cpp`、`tests/ui_offscreen_support.h` | 40 例 / 7 个套件 |
| 门控 | `tools/check_ui_no_readback.py`、`tools/check_ui_single_submit.py`、`tools/tests/test_ui_check_scripts.py` | UIG1 / UIG2 + 31 个自测 |
| 文档 | `MagicBlocks/docs/UI_CONSUMER.md` | 架构、两条路径、桩纪律、降级表、门控表、构建命令 |

### 2.2 两条路径（刻意的对称结构）

- **主路径**（§5.2）：`Presenter` 是纯状态机（不碰 GPU）→ `PresenterHost` 适配器
  → `VulkanFrameRenderer::render_into_device_image()`。今天**明确返回 false**，
  UI 层如实上报 `RecordFailed`。
- **离屏路径**（§5.5，仅 CI/golden）：`OffscreenTargetRenderer` → `render_to_memory()`
  → 渲染 + 读回 RGBA8。

两条路径共用**同一份**绘制代码（`internal->draw_ui_for_current_target()`），
所以 golden 锁住的布局就是真机布局。

## 3. 执行结果

### 3.1 测试

| 套件 | 例数 | 结果 |
|---|---|---|
| `ui_Router` | 6 | ✅ 全过 |
| `ui_Input` | 8 | ✅ 全过（含双指捏合修复） |
| `ui_Hud` | 5 | ✅ 全过（含布局越界修复） |
| `ui_Perf` | 6 | ✅ 全过 |
| `ui_Presenter` | 9 | ✅ 全过 |
| `ui_UiApp` | 5 | ✅ 全过（含离屏状态机） |
| `ui_Offscreen` | 3 | ⚠️ 3 例被**下层阻塞**（见 §3.3） |
| **合计** | **40** | **37 通过 / 3 阻塞** |

### 3.2 门控

| 门 | 结果 |
|---|---|
| G1 无几何着色器 | ✅ 90 文件 |
| G3 分层纯净 | ✅ backend 47 / 其余 37，零越界 |
| G4 零裸分配 | ✅ 84 文件 |
| UIG1 上屏零回读 | ✅ 12 个 UI 文件，豁免 3 个 offscreen 文件 |
| UIG2 单帧单提交 | ✅ 17 个 UI 文件，呈递路径内 1 处提交 |
| 门控脚本自测 | ✅ 191 例全过 |

构建：`magicblocks` / `magicblocks-ui-cli` / `magicblocks_test` 三目标 **0 error 0 warning**。

### 3.3 阻塞项（不在本轮所有权内）

`ui_Offscreen` 的 3 例需要真实的 SwiftShader 离屏目标，而 L1 的
`src/backend/vulkan/vulkan_target.cpp::create_image()` 目前会**崩溃**
（验证层：`vkBindImageMemory(): image Invalid VkImage Object`）。

这不是 UI 层的问题——同一缺陷在 L0 自己的用例上也复现：

```
[ RUN      ] Target.OffscreenReadbackIsRgba8
unknown file: error: SEH exception with code 0xc0000005 thrown in the test body.
```

`create_image()` 把 `vkCreateImage` 的句柄转成 `std::uintptr_t` 交给
`allocate_image_page()` / `bind_image_page()`，但分配器的图像页在
`VMA_MEMORY_USAGE_GPU_ONLY + requiredFlags = DEVICE_LOCAL` 下于 SwiftShader 上
拿不到内存类型。L1 文件由并行会话持有，本轮**不越界修改**。

`tests/data/golden_ui.png` 也因此尚未生成：它必须由
`magicblocks-ui-cli render --width 320 --height 240 --output ...` 产出，
而那条命令走的就是同一个阻塞点。

### 3.4 本轮修掉的真实缺陷

| 缺陷 | 现象 | 根因 | 修法 |
|---|---|---|---|
| 双指捏合比例恒为 0 | `ui_Input.PinchIsRecognized` 失败 | 抬起后 `pair_distance()` 归零 | 缓存 `pinch_last_distance`，抬手前先取值 |
| HUD 控件越出屏幕 | 控件右边 674 > 480 | 色板推进 `inner_x` 后再算滑条 | 滑条从面板固定右边缘算 |
| 底部面板控件溢出 | 竖屏下越界 | 控件顺序推进无回退 | 放不下就 `continue`，不越出面板 |
| 离屏模式 `RecordFailed` | 状态机读回失败 | Presenter 无条件走 `render_into_device_image` | 按 `mode` 分支到 `render_to_memory` |
| 实例缓冲崩溃 | SEH 0xC0000005 | 把 `VkBuffer` 传给 `vmaBindImageMemory`（按 VkImage 解引用） | 改走桶内子块 + `page_memory_handle` 按偏移绑定 |
| 描述符未绑内存 | 验证层 VUID-00329 | 同上 | 同上 |
| 桶页未预留 | `当前页无空位` | 桶不会自动长页 | 首次分配前 `reserve_pages()`（仅初始化/扩容时） |

## 4. 设计取舍记录

1. **两套「presenter」是故意的**：`Presenter`（公开、纯状态机、不碰 GPU）与
   `PresenterHost`/`OffscreenTargetRenderer`（私有适配器）。UIG2 只允许文件名含
   `present` 的文件提交——把适配器和状态机分开，门控才能精确表达"提交点唯一"。

2. **`IFrameRenderer` 没有回读成员**：不是忘了，是结构性禁止。§5.4 的"上屏路径
   零回读"如果靠"约定不调用"，随时会破；靠"接口里根本没有这个方法"，破不了。

3. **桩一律显式失败**：`render_into_device_image()` 返回 false 而不是假装成功。
   假装成功会让 Presenter 在真设备上静默空转——比直接失败糟得多。

4. **`UiApp` 非多态**：为了让 `dynamic_cast` 不可用，逼迫内部访问走
   `UiAppAccess` friend 这一条显式通道，测试与实现都能看见这条边界。

5. **golden 由 GPU 渲染产出**，不用 `overlay_renderer.cpp` 的软件光栅器——
   后者只服务确定性测试，不能代表真机像素。

## 5. 后续（等 L1/L2 落地）

1. `create_image()` 修好后：跑通 `ui_Offscreen` 3 例，生成 `tests/data/golden_ui.png`。
2. L1 `ICommandList` 落地后：把 `record_ui_instances()` 换成 `drawInstanced`，
   `render_into_device_image()` 改为在调用方 command buffer 内录制
   `[Scene pass] + [UI pass(LoadOp::Load)]` —— **签名不变，UI 层零改动**。
3. L3 `IOverlay2D` 落地后：删掉 `ui::stub::IOverlay2D` 与 `detail::Overlay2D`，
   把 `overlay_renderer.cpp` 的软件路径降级为纯测试辅助。
4. 字体图集（当前文本画占位条）：属于 L3 纹理，本轮按 spec §9 明确不做。
