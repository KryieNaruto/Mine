# Mine 工作空间跨平台化(一套代码跨平台)设计

日期:2026-08-24

## 1. 背景与目标

用户将仓库拉到 Windows 本机(Git Bash / MSYS2)后,发现现有环境搭建是纯 Linux 的:
`install-user-deps.sh` 依赖 `dpkg-deb`/`apt`,`setup-env.sh` 按 apt 包名探测,
EasyPainter 窗口端经 `pkg-config` 链 X11,离屏渲染绑 lavapipe(X11/Xvfb)。

**根本问题是思路错误**:不应是「给现有 Linux 环境加 Windows 分支」,而是

> **一套代码跨平台** —— 不再有「Linux 环境 / Windows 环境」两套东西。

同时有一个被证实的正确技术判断:

> **离屏渲染是 Vulkan 核心自带能力**(渲染到 framebuffer/image 不依赖任何平台扩展);
> **显示只是贴图** —— 把那张离屏图贴到 swapchain(窗口)或存成 PNG(CLI)。

所以渲染核心代码必须是一套,lavapipe / SwiftShader / 真 GPU 只是运行时驱动,不进代码逻辑。

**验收标准**:同一份源码与同一套测试逻辑,在 Linux 与 Windows(MSYS2)都能无头构建、跑全量测试、离屏渲染输出 PNG,单一 golden 基线双平台一致校验。

## 2. 核心决策(已与用户逐节确认)

| 决策点 | 结论 |
|---|---|
| 目标 | 一套代码跨平台,不做「两套环境」 |
| 渲染核心 | Vulkan 离屏渲染(平台无关一套代码),显示只是贴图薄层 |
| 离屏软件光栅 | Linux=lavapipe / Windows=SwiftShader;两者都只是「能创建 Vulkan device 的驱动」 |
| 窗口 surface | 由 `VK_USE_PLATFORM_*` 自动选 Win32 / X11(编译器宏),不写死平台分支 |
| golden 基线 | **单一基线**,双平台一致校验;像素不一致 → 修渲染路径,不建第二份基线 |
| 工具链 | Linux 保留现有;Windows 走 MSYS2 + MinGW-w64 g++ |
| Qt6 | Linux=Debian 系统包;Windows=MSYS2 `mingw-w64-x86_64-qt6-base` |
| 三方库池 | 已跨平台(deps.yaml + `-G Ninja`),无需大改 |
| 门控文档 | SKILL 硬约束从「Linux 实现细节」抽象为「平台无关原则」 |
| 测试/CI | 同一套测试逻辑双平台跑,CI 双 job 都全绿才通过 |

## 3. 总体布局(现状 → 目标)

```
当前:
  easypainter-cli ──render_offscreen()──▶  PNG    ← 平台无关 ✅
  easypainter(窗口)──GLFW+X11──swapchain──▶ 窗口   ← 平台耦合集中在 main.cpp(440 行)

目标:
  core/render(离屏渲染)              ← 一套代码,平台无关(已是如此)
      ├─ VulkanContext   (init_instance / init_device,无 surface 依赖)
      ├─ Pipeline        (shader / render pass,离屏与窗口共用)
      └─ Offscreen       (渲染到 RGBA → 读回 host buffer)
  显示层(平台相关薄壳):
      ├─ windowed: GLFW(用 VK_USE_PLATFORM_* 自动 Win32/X11)+ swapchain
      │           = 离屏图 blit 到 swapchain image
      └─ cli:     离屏图 → stb 写 PNG
```

**关键洞察(已核验源码)**:`render_offscreen` 已经是「Vulkan 核心做离屏、显示只是贴图」的范本 ——
不创建 surface、无 swapchain、无 X11,全程渲染到 image → copy 到 host-visible buffer → 返回 RGBA。
我们要做的不是发明新架构,而是把它的平台无关性固化为原则,并让窗口层遵循「离屏 → 贴图」模式。

**平台差异被限定在四类位置**(见门控文档原则):
1. 依赖部署工具(脚本平台分支)
2. 窗口 surface 获取(`VK_USE_PLATFORM_*`)
3. 软件光栅驱动选型(lavapipe / SwiftShader,靠 `VK_DRIVER_FILES`)
4. 环境变量(`env.sh` 平台分支)

## 4. 渲染代码现状核验

| 文件 | 平台依赖 | 结论 |
|---|---|---|
| `EasyPainter/src/core/render/offscreen.cpp` | 无 surface/swapchain/X11,渲染到 image → host buffer → RGBA | **平台无关,不动** |
| `EasyPainter/src/core/render/vulkan_context.cpp` | `init_instance({})` / `init_device(VK_NULL_HANDLE)` 即无 surface 入口 | **平台无关,不动** |
| `EasyPainter/src/core/render/pipeline.cpp` | shader 由构建期 glslc 嵌入,离屏与窗口共用 | **平台无关,不动** |
| `EasyPainter/src/core/render/image_io.cpp` | `stb_image_write` 写 PNG | **跨平台,不动** |
| `EasyPainter/src/cli/main.cpp` | 不链接 GLFW/ImGui,纯 Vulkan 离屏路径 | **已跨平台,微调错误提示** |
| `EasyPainter/src/app/main.cpp` | GLFW + swapchain + ImGui,X11 经 pkg-config | **唯一平台耦合点,薄化** |
| `StickyNotes/src/core/*` | 纯数据模型 + geometry,不碰渲染 API | **平台无关,不动** |
| `StickyNotes` app/CLI | QtWidgets,离屏靠 `-platform offscreen` + `QWidget::render()` | **Qt6 双平台,不涉及平台 API** |

**窗口层薄化目标**:swapchain 部分保持通用(Vulkan swapchain 本就是跨平台 API),
surface 获取用 `glfwCreateWindowSurface`(GLFW 内部按 `VK_USE_PLATFORM_WIN32_KHR` / `VK_USE_PLATFORM_X11_KHR` 自动选),
X11 相关链接只在 Linux 生效(`if(WIN32)` / `if(UNIX)` 包 pkg-config X11 的 CMake 条件)。

## 5. 工具链与依赖部署跨平台

### 5.1 脚本平台分支(一套脚本,平台分支,不是两套)

| 脚本 | Linux 分支(现有,保留) | Windows 分支(新增) |
|---|---|---|
| `setup-env.sh` | 探测 cmake/ninja/g++/pkg-config/python3 + user-deps | 探测 MSYS2 环境 + mingw-w64 工具链(pacman 装) |
| `install-user-deps.sh` | Vulkan SDK tar + X11 头 + lavapipe + Xvfb(现有) | Vulkan SDK zip + SwiftShader;不装 X11/Xvfb;Qt6 走 pacman |
| `env.sh`(生成) | `LD_LIBRARY_PATH` + `VK_DRIVER_FILES`(lavapipe) | `PATH` 加 MSYS2/mingw bin + Vulkan bin;`VK_DRIVER_FILES`(SwiftShader) |

平台判定在脚本头部统一:`uname -s` → `MSYS*`/`MINGW*` 走 Windows 分支,其余走 Linux 分支。
`install-user-deps.sh` 的 `dpkg-deb` 前置检查仅 Linux 分支执行,Windows 分支不再要求。

### 5.2 离屏渲染驱动:平台无关选型

「离屏渲染是 Vulkan 自带」在驱动层落地 = **只需一个能创建 Vulkan device 的软件光栅**。

| | Linux | Windows |
|---|---|---|
| 离屏软件光栅 | lavapipe(Mesa) | SwiftShader(Google) |
| 窗口软件光栅 | lavapipe 出 swapchain | SwiftShader 出 swapchain(WGL) |
| Vulkan 头/SDK/glslc | Vulkan SDK tar | Vulkan SDK zip(同一套 glslc/头) |

**关键**:离屏渲染路径(Linux lavapipe / Windows SwiftShader)都只是「能创建 device 的驱动」——
`offscreen.cpp` 一行不改。`VK_DRIVER_FILES` 选驱动,`VK_USE_PLATFORM_*` 让窗口 surface 自动选平台。

### 5.3 三方库池(无需大改)

- `deps.yaml` 库已跨平台:glm/fmt/abseil/ink/googletest 纯头或跨平台 CMake。
- `glfw` 已 `GLFW_BUILD_WAYLAND=OFF`,Windows 下自动用 Win32 后端。
- `build-deps.py` / `fetch-deps.py` 已 `-G Ninja` + Python,MSYS2 的 Ninja 即可跑。

### 5.4 Qt6(StickyNotes 特例)

- **Linux**:保持 Debian 系统包(`qt6-base-dev` + `offscreen` 插件)。
- **Windows**:MSYS2 pacman 装 `mingw-w64-x86_64-qt6-base`,自带 `offscreen` platform 插件 → CLI `--render` 离屏 PNG 与 Linux 一致。

## 6. 测试 / CI / 门控文档跨平台

### 6.1 同一套测试逻辑,双平台跑

原则:**测试代码与断言逻辑平台无关,平台差异只出现在「环境准备」**。

| | 逻辑/断言 | 渲染(离屏) | golden |
|---|---|---|---|
| 代码 | 一套(gtest/QtTest) | 一套(Vulkan offscreen / Qt offscreen) | 同一基准 PNG |
| Linux | 直接跑 | lavapipe | 校验 |
| Windows | 直接跑 | SwiftShader | 校验 |

### 6.2 单一 golden 基线(不建分平台基线)

- 离屏渲染输出平台无关:同一 shader、同一 renderpass、同一确定性绘制,lavapipe 与 SwiftShader
  都是软件光栅,对同一点集应逐像素一致。
- golden PNG 只存一份,两平台跑同一 `MatchesBaselineGolden`。
- **若某平台逐像素有差异 → 定位渲染路径并修到一致,不引入第二份基线。**
  否则 CI 防不住「改一个平台、悄悄坏另一个」。

### 6.3 CI 矩阵

```
workflow:
  job-linux:   Ubuntu + 现有 user-deps → build → ctest(全绿)
  job-windows: windows-2022 + MSYS2 → pacman 装依赖 → build → ctest(全绿)
```

两个 job 跑**完全相同**的 `ctest`(同一测试逻辑、同一 golden)。任一平台不绿即失败。

### 6.4 门控文档改动(bugfix-pipeline / build-pipeline SKILL)

把硬约束从「Linux 实现细节」抽象为「平台无关原则」:

| 现有(SKILL 隐含 Linux) | 改为(平台无关) |
|---|---|
| 离屏渲染 = Xvfb / lavapipe | **离屏渲染 = Vulkan 离屏渲染(平台无关核心),不依赖窗口/显示服务** |
| CLI 输出 PNG 供 golden | **golden 单一基线,双平台一致校验**(不设分平台基线) |
| (未提及) | **渲染核心禁平台专用 API**(无平台 `#ifdef` 分叉渲染逻辑) |

同步更新设计文档中「GLFW 仅 X11 / 用 Xvfb 跑」等表述,立为「窗口 surface 由 `VK_USE_PLATFORM_*` 自动选」原则。

### 6.5 错误处理 / 风险

| 风险 | 处理 |
|---|---|
| SwiftShader 缺失(Windows) | `setup-env.sh` 明确报错;遵循 SKILL 不回退原则,不提供绕过 |
| 跨平台像素 diff | 按 6.2,定位渲染路径修复,不引入第二份基线 |
| MSYS2 Qt6 offscreen 插件缺失 | pacman 装齐,缺失则 CLI 明确报错 |
| MSYS2 环境识别误判 | 脚本统一 `uname -s` 判定,双平台 CI 验证 |

## 7. 影响面清单

| 文件/目录 | 改动 |
|---|---|
| `docs/superpowers/specs/*-easypainter-design.md` | 「GLFW 仅 X11 / Xvfb」表述 → 平台无关原则 |
| `docs/superpowers/specs/*-workspace-bootstrap-design.md` | 「初版 Linux/GCC 单一 toolchain」→ 跨平台说明 |
| `.claude/skills/bugfix-pipeline/SKILL.md` | 离屏/golden 硬约束平台无关化 |
| `.claude/skills/build-pipeline/SKILL.md` | 同上 |
| `tools/setup-env.sh` | 平台分支 |
| `tools/install-user-deps.sh` | 平台分支(Windows: Vulkan zip + SwiftShader + pacman Qt6) |
| `tools/install-user-deps.sh` 产物 `env.sh` | 平台分支 |
| `EasyPainter/src/app/main.cpp` | 窗口层薄化(X11 链接 Linux-only) |
| `EasyPainter/CMakeLists.txt` | pkg-config X11 仅 Linux 条件 |
| `EasyPainter/src/cli/main.cpp` | 错误提示去掉「lavapipe」字样(平台无关) |
| `StickyNotes/CMakeLists.txt` | Qt6 双平台 find_package(无需大改) |
| CI(新增) | linux + windows 双 job |
| `tools/deps_lib/cmake_driver.py` | 无需改(已 `-G Ninja` 平台无关) |

## 8. 明确不做什么(防范围蔓延)

- 不重建渲染核心 —— `offscreen.cpp` 等已是平台无关,只固化原则。
- 不做分平台 golden 基线 —— 单一基线是「一套代码」的验证,不是妥协。
- 不引入 Vulkan 内存分配器 / 多帧并行等性能优化 —— 超出本次跨平台范围。
- 不把两个 app 的渲染合并成一个统一引擎 —— 各自保持轻量,共享「平台无关离屏」原则。
