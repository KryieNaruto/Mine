# MagicBlocks 定位裁定 R-1 —— SDK 降级为"计算/几何库 + 离屏渲染器"

日期：2026-11-03
状态：**已按推荐执行**（用户指令"继续"，本文件记录裁定 + 落地边界 + 可回滚点）
关联：[SDK 设计](2026-11-03-magicblocks-render-engine-sdk-design.md)、
[UI 消费层](2026-11-03-magicblocks-ui-consumer-design.md)、
[技术路线对比 §12](2026-11-03-magicblocks-tech-route-comparison.md)、
[计算类依赖选型](2026-11-03-magicblocks-compute-deps-selection.md)

---

## 1. 裁定

用户两条前置结论共同推出本裁定：

1. "**现在只做渲染引擎**" —— 玩法/UI 不是本期研发目标。
2. "**安卓消费层必须使用现成的，不然 UI 全都得自研！**" —— 上屏与 UI 交给生态
   （首选 SurfaceView + Jetpack Compose；渲染后端保持自研 Vulkan/MoltenVK）。

于是：**MagicBlocks 的定位从"带窗口上屏的引擎"收敛为"计算/几何库 + 离屏渲染器"**。

| 关注点 | 归属 |
|---|---|
| 上屏（present / swapchain 配对 / 帧节拍） | **平台壳层**（Android `SurfaceView` + `ANativeWindow`；iOS `CAMetalLayer`） |
| 控件 UI | **现成方案**（Compose / SwiftUI） |
| 几何与渲染核心（RHI / L1 / L2 / L3 / 命令流 / 离屏 golden） | **MagicBlocks SDK** |

一句话：**SDK 只负责"算出并画出一张正确的图"，不负责"把图贴到屏幕上"。**

## 2. 为什么不是 R-2（保留自研上屏 + 现成控件）

R-2 要求 `Compose ↔ SurfaceView` 互操作且 SDK 自己持有 `VkSurfaceKHR`/Present 队列。
代价是 SDK 必须为每个平台维护"壳层互操作层"，而这层恰恰是最容易出平台坑、最没有复用价值的
部分（也是 `ISwapchain` 至今 `acquire/present` 都没落地、`vulkan_ui_overlay` 主路径只能返回
`Unsupported` 的原因）。

R-1 让 SDK 的对外能力**在平台之间完全一致**（一张 RGBA8 离屏图 / 一个可被任何 GPU 纹理消费的
设备图像），换平台只换壳层。这与 `EasyPainter` 已经验证过的"**离屏渲染是核心，显示只是贴图**"
是同一条经验。

## 3. 落地边界（**已执行**）

### 3.1 分级为"一档"的（SDK 永久保留）

- `device/`：`IRenderDevice`、`IRenderTarget`（**窗口与离屏同一接口**，离屏是一等公民）、
  `Format`/`Extent2D`、`Status`/`Result<T>`。
- `resources/`：VMA 分页分配器（设备页 64 MB / Host 页 8 MB）、`IBuffer`/`ITexture`/`IPipeline`/`ICommandList`。
- `scene/`：`IScene`、`IRenderGraph`、命令流录制/回放、`ICommandStream`。
- `backend/vulkan/`：唯一出现 `Vk*` 的地方；离屏 target、draw、pipeline、texture、buffer、command。
- `ui/`：**仅保留离屏/CLI 消费所需**的 `IFrameRenderer::render_to_memory`、HUD/覆盖层图元、
  输入路由（纯 CPU 逻辑，可被壳层事件驱动）、`PerfGovernor`。

### 3.2 分级为"移交壳层"的（SDK 不再作为主路径）

| 组件 | 处置 |
|---|---|
| `device/swapchain.h` `ISwapchain` | **保留声明**（Windows/XCB 桌面调试仍用），但不再是"消费层唯一上屏口" |
| `ui/presenter.h` `Presenter` + `IPresentSurface` | **保留为 Windows 桌面预览 / 录制入口**；Android/iOS 壳层不使用 |
| `src/backend/vulkan/vulkan_swapchain_*.cpp` | 保留（桌面调试、CI 截图窗口用） |
| `vulkan_ui_overlay` 主路径（`render_into_device_image`） | **维持返回 `Unsupported`**——它本来就是为 R-2/自研上屏准备的；R-1 下不需要它 |

> 关键：**不删任何已落地代码**。R-1 是把它们从"主路径"降级为"调试/测试路径"，
> 这样 R-2 随时可回滚，且已有 32+20 用例与三张 golden 全部保持有效。

### 3.3 新增的第 4 张 golden（R-1 的验收面）

R-1 下 SDK 的"产品级出口"是 **离屏 RGBA8**。因此新增一条端到端验收（**已实测通过**）：

```
magicblocks-ui-cli render --width 320 --height 240 --output r1_320x240.png
```

- **不需要窗口、不需要显示服务**（门控 R1 兜底：CLI 里不得出现 `HWND`/`xcb_`/`ANativeWindow`/wayland）。
- 实测产出与既有基线 `tests/data/golden_ui.png` **逐字节一致**（SHA256
  `DD9C57B8…426E`，122,943 字节 → 307,528 字节 PNG），说明"离屏出图"本身就是一条
  **可跨平台复现的交付通路**，不是测试专用的旁路。
- CLI **没有 `--mode` 开关**：离屏就是默认且唯一路径 —— 门控 R1-3 会拒绝把模式开关加回来。

> 回归命令已固化为 `tools/check_offscreen_first.py`（退出码 0/1，接入 CI 与本地预检）。

## 4. 对既有设计文档的修订

| 文档 | 修订 |
|---|---|
| UI 消费层设计 | §5 上屏链路改为"**壳层负责**"；SDK 提供 `ITexture`/离屏图两种交付 |
| SDK 设计 | §10 验收门新增 R1-1「离屏出图不依赖窗口」；上屏门（single-submit 等）降级为桌面调试路径的约束 |
| 技术路线对比 §12 | 填入本裁定（选 R-1） |

## 5. 可回滚点

若后续要求"SDK 自持上屏"，只需：
1. 把壳层 present 代码从平台工程移回 `src/backend/vulkan/vulkan_swapchain_*.cpp`；
2. 让 `vulkan_ui_overlay::render_into_device_image()` 真正实现（签名已定型，见其文件头注释）；
3. `Presenter` 直接接 `ISwapchain` —— **所有类型和状态机都在，无需重写**。
