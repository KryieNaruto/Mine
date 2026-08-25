# EasyPainter 设计（集成测试 Google ink-stroke-modeler）

日期：2026-08-23

## 1. 背景与目标

在 Mine 工作空间内新建 `EasyPainter` 项目，用于**集成测试** Google 的笔迹预测库 [ink-stroke-modeler](https://github.com/google/ink-stroke-modeler)（原名 stroke-modeler，已更名）。渲染端采用 **Dear ImGui + Vulkan**，窗口由 GLFW 承载。

三个并列目标（已确认「都要」）：

1. **端到端交互**：输入笔迹采样点 → Stroke Modeler 实时预测 → ImGui+Vulkan 渲染预测轨迹，可交互调参观察。
2. **正确性对比**：以官方示例输出为 golden 基准，校验本工程预测结果数值一致。
3. **性能基准**：测量单次预测延迟 / 吞吐，ImGui 面板绘制 benchmark 曲线。

外加一条全局硬约束（SKILL 通用规则，对 EasyPainter 同样强制）：

4. **CLI 模式 + 离屏渲染输出图像**：提供无头 CLI 入口，离屏渲染预测结果并落盘一张执行图像（PNG），供脚本化验收与图像 golden 对比。

## 2. 核心决策（已确认）

| 决策点 | 结论 |
|---|---|
| 项目目录 | `Mine/EasyPainter/`，遵循 workspace 一项目一文件夹约定 |
| 三方库接入 | 全部进全局池 `third_party/deps.yaml`，项目 `deps.yaml` 只声明 `use` |
| 渲染端 | Dear ImGui（Vulkan + GLFW 后端）+ Vulkan，GLFW 建窗口 |
| Vulkan 本身 | **系统级依赖（无 sudo 部署）**，`tools/install-user-deps.sh` 把 loader/headers/glslc 解压到 `.user-deps/`，不进池；`setup-env.sh` 只做探测+指引 |
| C++ 标准 | **C++20**（ink-stroke-modeler 硬性要求） |
| CLI / 离屏 | 单仓库两个可执行目标：`easypainter`（窗口）+ `easypainter-cli`（无头离屏） |
| 图像编码 | PNG 用 `stb_image_write.h`（单头 vendor 进项目，零编译） |
| 正确性金标 | 双金标：**数值 golden**（官方示例预测点）+ **图像 golden**（离屏渲染 PNG 逐像素比对） |
| 骨架来源 | **手写骨架**（模板 hello-world 太简陋，需自定义 CMakeLists + 模块） |
| 跨库依赖 | 工具层改造：`build-deps.py` 拓扑排序（`depends_on` 先序）+ `cmake_driver` 注入 `CMAKE_PREFIX_PATH` 指向池内已建前缀；清单中 abseil-cpp 排 ink-stroke-modeler 之前 |
| imgui 接入 | **项目内 vendor**（不走池）：imgui 官方无稳定 install config，vendor `imgui/*.cpp` + `backends/imgui_impl_vulkan.cpp` + `imgui_impl_glfw.cpp` 进 `EasyPainter/vendor/imgui/` |
| 系统依赖部署 | **无 sudo 用户级部署**：服务器无 root，`tools/install-user-deps.sh` 下载包解压到 `$MINE_ROOT/.user-deps/`（根 `.gitignore` 覆盖）+ 生成 `env.sh`（PATH/PKG_CONFIG_PATH/CMAKE_PREFIX_PATH/CMAKE_INCLUDE_PATH/LD_LIBRARY_PATH/VK_DRIVER_FILES，并 sed 重写解压包内 `/usr` 绝对路径），构建/运行前 `source` |
| 无 GPU 环境 | 软件光栅 **lavapipe**（`.user-deps` 内部署 + `VK_DRIVER_FILES`），使离屏渲染与图像 golden 在无 GPU 环境也能跑 |
| windowed 显示 | 无物理显示器，用 **Xvfb/Xvnc 虚拟 X display**（`DISPLAY=:N`）跑 `easypainter`（GLFW 仅 X11 后端） |

## 3. 三方库清单（进池新增库；imgui/stb 为项目内 vendor；Vulkan/X11 为系统级）

| 库 | repo | tag / branch | build | options | 说明 |
|---|---|---|---|---|---|
| ink-stroke-modeler | `github.com/google/ink-stroke-modeler` | `main`（lock 记 commit） | cmake | `INK_STROKE_MODELER_FIND_DEPENDENCIES=ON` | 核心库，C++20，目标 `InkStrokeModeler::stroke_modeler/types/params` |
| abseil-cpp | `github.com/abseil/abseil-cpp` | `20260817.0` | cmake | `ABSL_BUILD_TESTING=OFF`, `CMAKE_CXX_STANDARD=20` | ink 硬依赖（absl::Status/StatusOr） |
| glfw | `github.com/glfw/glfw` | `3.4` | cmake | `GLFW_BUILD_EXAMPLES=OFF`, `GLFW_BUILD_TESTS=OFF`, `GLFW_BUILD_DOCS=OFF`, `GLFW_BUILD_WAYLAND=OFF` | 窗口 + Vulkan surface；编译仅需 X11 系统头 |
| glm | （已在池） | `1.0.1` | — | — | 数学 |
| googletest | `github.com/google/googletest` | `v1.15.2` | cmake | — | 测试框架，目标 `GTest::gtest`/`GTest::gtest_main` |
| stb | `github.com/nothings/stb` | 单头 `stb_image_write.h` | header-only | — | vendor 进 `EasyPainter/vendor/`，不进池 |

**imgui（项目内 vendor，不进池）**：imgui 官方无稳定 CMake install config，故不走池。vendor 进 `EasyPainter/vendor/imgui/`：`imgui.h/.cpp`、`imgui_draw.cpp`、`imgui_tables.cpp`、`imgui_widgets.cpp`、`imconfig.h`、`imstb_*.h`，以及 `backends/imgui_impl_glfw.cpp/.h`、`backends/imgui_impl_vulkan.cpp/.h`。项目 CMake 直接编译这些源，不 `find_package(imgui)`。

**Vulkan 与窗口系统（无 sudo 用户级部署，`tools/install-user-deps.sh`）**：服务器无 root，全部下载包解压到 `$MINE_ROOT/.user-deps/`（根 `.gitignore` 覆盖），并生成 `env.sh`（导出 `PATH`/`PKG_CONFIG_PATH`/`CMAKE_PREFIX_PATH`/`CMAKE_INCLUDE_PATH`/`LD_LIBRARY_PATH`/`VK_DRIVER_FILES`），构建/运行前 `source .user-deps/env.sh`：
- **Vulkan 工具链**：Vulkan SDK 预编译 tar（`https://sdk.lunarg.com/sdk/download/latest/linux/vulkan-sdk-latest.tar.xz`，已核验可下载，含 `vulkan.h` 与 `glslc`）解压到 `.user-deps/vulkan-sdk/`。
- **X11 开发头**（GLFW 仅 X11 后端）：`apt-get download` 以下全部 + `dpkg -x` 到部署根——**含传递依赖**：`libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libxext-dev libxcb1-dev libx11-xcb-dev x11proto-dev libxau-dev`（`apt-get download` 不拉依赖，必须显式列全；无 apt 列表时改从 archive.ubuntu.com 直接 curl 指定 .deb）。
- **重写绝对路径**：`dpkg -x` 后 `sed` 重写所有 `.user-deps/**/*.pc` 的 `prefix/includedir/libdir`（原 `/usr`）为 `.user-deps/` 前缀；lavapipe ICD json 的 `library_path` 同样改写为实际路径。
- **lavapipe**（无 GPU 软件光栅）：`apt-get download libvulkan-lavapipe` + `dpkg -x`，`VK_DRIVER_FILES` 指向其 ICD json，使离屏渲染（headless 无 surface）与图像 golden 在无 GPU 环境可跑。**运行期传递依赖**：lavapipe 驱动 dlopen 后仍需 `libllvm libgbm1 libdrm2 libexpat1 libzstd1 libz1`，用 `dpkg-deb -f <deb> Depends` 解析并递归下载解压到 `.user-deps/`，由 `LD_LIBRARY_PATH` 覆盖。
- **windowed 显示**：无物理显示器，用 **Xvfb/Xvnc 虚拟 X display**（`apt-get download xvfb` + `dpkg -x` 部署，或探测 `xvfb-run` 可用性）跑 `easypainter`（GLFW X11 后端），`DISPLAY=:99`。xvfb 运行期仍需 `libxfont2 libpixman-1-0 libgl1 libxshmfence1` 等，同样 `dpkg-deb -f Depends` 递归下载。
- **验证**：真实探针——用解压头 configure+编译最小 X11+GLFW 探针（`glfwInit()` 成功）、用 lavapipe 实际 `vkCreateInstance` 成功（非零物理设备）、`glslc` 编一个最小 `.frag→.spv`。

**版本固定说明**：ink-stroke-modeler 无 tag，池按 `main` 拉取、`.pool.lock.json` 记录实际 commit（`requested_tag: main`，`commit: f2388813b0b2`），与 workspace §6 约定一致。

## 4. 总体布局

```
EasyPainter/
├── deps.yaml                     # use: [abseil-cpp, ink-stroke-modeler, glfw, glm, googletest]
├── CMakeLists.txt                # C++20, find_package → 池 install, 3 个目标
├── CMakePresets.json             # debug/release → 池产物变体
├── .gitignore
├── vendor/
│   ├── imgui/                    # ★ vendor 的 Dear ImGui(源 + vulkan/glfw 后端)
│   └── stb_image_write.h         # 单头 PNG 编码
├── shaders/
│   ├── stroke.vert               # 笔画顶点着色器
│   └── stroke.frag               # 笔画片元着色器
├── src/
│   ├── core/
│   │   ├── stroke/
│   │   │   ├── types.h           # 平台无关别名（Vec2/InputEvent/Result）
│   │   │   ├── input_source.h/.cpp   # 采样点流 → ink InputEvent
│   │   │   └── predictor.h/.cpp      # StrokeModeler 封装（配置→建模→更新→取点）
│   │   ├── render/
│   │   │   ├── vulkan_context.h/.cpp # instance/device（windowed+headless 共用）
│   │   │   ├── pipeline.h/.cpp       # 笔画绘制管线（含 shader 加载）
│   │   │   ├── offscreen.h/.cpp      # 离屏渲染 → host buffer
│   │   │   └── image_io.h/.cpp       # buffer → PNG（stb）
│   │   └── bench/
│   │       └── bench.h/.cpp          # 延迟/吞吐测量
│   ├── app/
│   │   ├── main.cpp              # windowed 入口（GLFW + ImGui）
│   │   └── gui.h/.cpp            # ImGui 面板（调参/轨迹/benchmark 曲线）
│   └── cli/
│       └── main.cpp              # CLI 入口（--cli --input --output）
└── tests/
    ├── stroke_test.cpp           # predictor 单测（输入→预测非空/形状/单调）
    ├── golden_test.cpp           # 数值 golden 对比
    ├── image_golden_test.cpp     # 离屏图像 golden（逐像素）
    └── bench_test.cpp            # 性能基准断言（延迟上限）
```

**目标划分**：`easypainter_core`（静态库：`src/core/` 全部）→ 被 `easypainter`（窗口）、`easypainter-cli`（无头）、`tests/` 共同链接。

## 5. 渲染架构（windowed + headless 两态）

`vulkan_context` 提供与窗口解耦的 instance/device/queue；两态只在「呈现表面」上分叉：

- **windowed**：GLFW 建窗口 → `glfwCreateWindowSurface` → swapchain → `vkQueuePresent`；ImGui 经 `imgui_impl_vulkan` + `imgui_impl_glfw` 渲染。
- **headless**：不建窗口、无 swapchain，渲染到离屏 `VkImage`（`COLOR_ATTACHMENT | TRANSFER_SRC`）→ `vkCmdCopyImageToBuffer` 到 host-visible buffer → `vkMapMemory` → 交给 `image_io` 落盘 PNG。

共用同一套 `pipeline`（stroke 顶点/片元 shader、同一预测结果几何数据），保证两态输出一致——这是图像 golden 可比对的前提。

## 6. CLI 模式 + 离屏渲染

```
easypainter-cli --input <采样点文件> --output <out.png> [--width N --height M] [--stroke <参数...>]
```

行为：读采样点（纯文本，每行一个 `x,y`）→ `predictor` 得预测点集 → 离屏渲染到 `out.png` → 退出（非零码表示失败）。**CLI 二进制不链接 GLFW/ImGui**（运行层无窗口依赖），可在 CI / 无显示环境跑；但池构建阶段 glfw 仍需 X11 头（见 §3 部署）。采样点文件缺省用内置示例点，保证 `easypainter-cli --output x.png` 零参数即可产出确定性图像。无 GPU 环境依赖 **lavapipe** 软件光栅离屏渲染（headless 不建 surface，lavapipe 可跑）。

## 7. Stroke Modeler 集成（数据流）

```
原始采样点(Vec2) → input_source 归一化/降采样 → ink::InputEvent 序列
        → predictor: StrokeModeler(params).Update(event*) → Result(points)
        → 几何数据(顶点 buffer) → pipeline 渲染（windowed 或 headless）
```

- `input_source`：把屏幕/文件坐标转为 ink 期望的输入坐标，处理 down/move/up 事件。
- `predictor`：持有 `ink::stroke_model::StrokeModeler`，封装 `ModelParams`（可调）、逐事件 `Update`、导出 `Result.points`。这是正确性测试与性能测试的核心单元，接口与 ink 隔离，方便 mock 与 golden 注入。

## 8. 测试策略

| 目标 | 用例 | 位置 | 判定 |
|---|---|---|---|
| 正确性（数值） | 固定采样点输入 → 预测点与官方 golden 逐值对比（容差 1e-4） | `golden_test.cpp` | 数值一致 |
| 正确性（图像） | `easypainter-cli` 离屏渲染 → 与基准 PNG 逐像素比对 | `image_golden_test.cpp` | 像素一致（容差阈值） |
| 单元 | predictor/input_source/bench 的独立行为（非空、形状、单调、空输入不崩） | `stroke_test.cpp` | 断言全过 |
| 性能 | 单次 `Update` 延迟 P50/P99、吞吐（points/s），上限断言 | `bench_test.cpp` | 延迟 < 阈值 |
| 端到端 | windowed 人工交互：拖拽画笔画 → 观察预测轨迹 + 调参生效 | 人工 | 验收清单 |

测试框架用 **GoogleTest**（单独进池 `googletest`，目标 `GTest::gtest`），与 ink-stroke-modeler 自带的 gtest 测试解耦。

**数值 golden 独立 oracle（关键约束）**：golden 基准**不得由本工程 predictor 自产自比**。来源二选一（实现时按实际确定）：① 优先采用 ink-stroke-modeler 仓库自带测试数据/期望输出（`_src/ink-stroke-modeler-*/` 下 testdata 或 upstream 测试用例）；② 若无现成数据，单独编译 ink 官方 example（与本工程 predictor 完全独立的两条路径）生成基准点并提交，注明生成器与坐标归一化。

**图像 golden 环境**：在 lavapipe（软件光栅）上生成并比对基准 PNG，容差按 lavapipe 实测值设定（不预设值）；README 记录 golden 的生成环境（lavapipe 版本）以支撑可复现性。有真 GPU 环境跑同一用例，若与 lavapipe 基准像素差异超出容差，以 lavapipe 基准为准并记录。

## 9. 风险与对策

| 风险 | 对策 |
|---|---|
| C++20：ink + abseil 需 C++20 | 项目 CMake 显式 `CMAKE_CXX_STANDARD 20`；abseil 池编译加 `CMAKE_CXX_STANDARD=20` |
| ink 的 find_package config 与 include 目录 | Task 1 实测 CONFIG 可用并直接链接；ink 导出 targets 的 include 目录为空，项目侧显式 `target_include_directories(INTERFACE <ink前缀>/include)` 补齐（必要接线，非回退） |
| abseil 与 ink 的 find_package 衔接 | `INK_STROKE_MODELER_FIND_DEPENDENCIES=ON` + `CMAKE_PREFIX_PATH` 指向池内 abseil install 前缀 |
| Vulkan 系统依赖缺失 | `tools/install-user-deps.sh` 部署 Vulkan SDK tar（headers+glslc）到 `.user-deps/`；`setup-env.sh` 探测 env.sh 存在性并指引 |
| Shader 编译 | `glslc` 编译期把 `.vert/.frag` 编成 SPIR-V（glslc 由 install-user-deps.sh 部署提供）；不做预编译回退 |
| headless 无显示环境 | 离屏路径不创建 surface，CI 可跑；无物理设备时依赖 lavapipe 软件光栅，缺失则明确报错退出 |
| 跨库依赖（abseil→ink） | 工具层改造：cmake_driver 注入 CMAKE_PREFIX_PATH + build-deps depends_on 拓扑 + 清单 abseil 排前；Task 1 落地并补 tools 测试 |
| imgui 无 install config | 项目内 vendor（Task 2），不 find_package(imgui) |
| GLFW 需 X11 头 | 部署脚本下载并解压全部 X11 传递依赖（见 §3 映射表）；glfw 仅 X11 后端 |
| glslc | 由 install-user-deps.sh 部署（Vulkan SDK 内含）；缺失即补齐，不做预编译 SPIR-V 回退 |
| 无 sudo 部署：X11 传递依赖缺失 | 下载清单含 libxext-dev/libxcb1-dev/libx11-xcb-dev/x11proto-dev/libxau-dev 等；`apt-get download` 不拉依赖，需显式列全 |
| 无 sudo 部署：解压包内 `/usr` 绝对路径 | `dpkg -x` 后 `sed` 重写 `*.pc` 与 lavapipe ICD json 的路径为 `.user-deps/` 前缀 |
| 无 sudo 部署：运行期库加载 | `env.sh` 导出 `LD_LIBRARY_PATH`（SDK lib + lavapipe/xvfb 运行期 .so 目录），否则 `vkCreateInstance`/Xvfb 启动失败 |
| 无 sudo 部署：lavapipe/xvfb 运行期 .so 传递依赖 | 用 `dpkg-deb -f <deb> Depends` 解析并递归下载（lavapipe：libllvm/libgbm1/libdrm2/libexpat1/libzstd1/libz1；xvfb：libxfont2/libpixman-1-0/libgl1/libxshmfence1 等） |
| 无 sudo 部署：apt-get download 前提 | 无 root 服务器需已有 apt 包列表；无列表时改从 archive.ubuntu.com 直接 curl 指定 .deb |
| Xvfb 不可用 | 部署脚本含 `xvfb` 下载解压；仍缺则按阻塞修复（与 Task 6 Step 4、§10.2 一致，不降级） |

## 10. 验收标准

1. `easypainter-cli --output out.png` 在无显示环境零参数跑通（无 GPU 时经 lavapipe），产出确定性 PNG。
2. windowed `easypainter` 在 Xvfb/Xvnc 虚拟 display（无物理显示器）下可拖拽绘制、预测轨迹实时跟随、ImGui 调参生效。
3. `ctest` 全绿：单测、数值 golden、图像 golden、性能断言全部通过（对应 SKILL 测试门 100 分）。图像 golden 在 lavapipe 软件光栅上生成/比对，无 GPU 环境不豁免。
4. 池内新增 4 库（ink/abseil/glfw/glm）`fetch` + `build` 一次成功、二次全部 `SKIP`。
5. 仓库 `git status` 干净：`_src/_build/_install`、构建产物、`.user-deps/`（根 `.gitignore`）均被忽略，`source env.sh` 后仍干净。
