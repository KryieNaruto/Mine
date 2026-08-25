# EasyPainter Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 在 `Mine/EasyPainter/` 建一个 ImGui+Vulkan 渲染的 C++ 工程，集成测试 Google ink-stroke-modeler，并同时支持窗口交互、CLI 离屏渲染输出 PNG。

**Architecture:** 三个目标：`easypainter_core`（静态库，stroke 封装 + Vulkan 渲染 + 离屏 + bench）、`easypainter`（窗口端 GLFW+ImGui）、`easypainter-cli`（无头离屏）。stroke 预测逻辑通过 `Predictor` 封装与 ink 隔离，渲染通过 `Pipeline` + `Offscreen` 抽象出 windowed/headless 两态。

**Tech Stack:** C++20，CMake + Ninja，ink-stroke-modeler（main）、abseil-cpp（20260817.0）、Dear ImGui（v1.92.8，vendor）、GLFW（3.4）、glm（1.0.1）、googletest（v1.15.2）、stb_image_write（vendor）、Vulkan + X11（系统级，无 sudo 用户级部署）。

**Spec:** `docs/superpowers/specs/2026-08-23-easypainter-design.md`

## Global Constraints

- C++ 标准 **C++20**（项目与 abseil 池编译均设 `CMAKE_CXX_STANDARD=20`）。
- 进池库：ink-stroke-modeler、abseil-cpp、glfw、glm、googletest；**imgui 项目内 vendor（不走池）**；Vulkan + X11 头经**无 sudo 用户级部署**（`tools/install-user-deps.sh` → `$MINE_ROOT/.user-deps/` + `env.sh`，构建/运行前 `source`），lavapipe 提供无 GPU 软件光栅；windowed 用 Xvfb/Xvnc 虚拟 display。
- 跨库依赖由工具改造支撑：`cmake_driver` 注入 `CMAKE_PREFIX_PATH`（池内已建前缀）+ `build-deps` 按 `depends_on` 拓扑排序 + 清单 abseil-cpp 排 ink-stroke-modeler 前。
- 版本锁定：ink-stroke-modeler=`main`（lock 记 commit）、abseil=`20260817.0`、imgui=`v1.92.8`(vendor)、glfw=`3.4`、glm=`1.0.1`、googletest=`v1.15.2`。
- **硬约束（缺失即整份计划打回）**：必须提供 CLI 模式 + 离屏渲染输出图像。
- 所有可独立测试的纯逻辑单元，先写失败测试再实现（TDD）。

---

### Task 1: 三方库接入 + 工具改造 + find_package 验证

**Files:**
- Modify: `third_party/deps.yaml`
- Modify: `tools/deps_lib/manifest.py`（`LibSpec` 加 `depends_on` 字段）
- Modify: `tools/deps_lib/cmake_driver.py`（`configure_command` 注入 `CMAKE_PREFIX_PATH`）
- Modify: `tools/build-deps.py`（`depends_on` 拓扑先序）
- Modify: `tools/setup-env.sh`（探测用户级部署 + 系统缺项指引，不再依赖 sudo/apt 安装）
- Create: `tools/install-user-deps.sh`（无 sudo 下载解压 Vulkan/X11/lavapipe/Xvfb 到 `.user-deps/` + sed 重写绝对路径 + 生成 `env.sh`）
- Modify: `.gitignore`（根：加 `.user-deps/`，与 `_src/_install/` 同级）
- Modify: `tools/tests/test_cmake_driver.py`；Create: `tools/tests/test_build_deps.py`
- Create: `EasyPainter/deps.yaml`

**Interfaces:**
- Produces: 池内 5 库（ink-stroke-modeler/abseil-cpp/glfw/glm/googletest）`fetch`+`build` 完成；`build-deps.py` 支持 `depends_on` 拓扑；`cmake_driver.configure_command` 自动注入 `-DCMAKE_PREFIX_PATH=<池内已建前缀>`；`setup-env.sh` 可探测 `.user-deps` 部署与系统缺项（Vulkan/X11）。

- [ ] **Step 1: 在全局清单登记 5 个池库（abseil 排在 ink 之前）**

`third_party/deps.yaml` 的 `libs` 下新增（并入现有 `fmt`/`glm`，勿删；`depends_on` 为新字段）：

```yaml
  abseil-cpp:
    repo: https://github.com/abseil/abseil-cpp.git
    tag: "20260817.0"
    build: cmake
    options: [ABSL_BUILD_TESTING=OFF, CMAKE_CXX_STANDARD=20]
  ink-stroke-modeler:
    repo: https://github.com/google/ink-stroke-modeler.git
    tag: "main"
    build: cmake
    options: [INK_STROKE_MODELER_FIND_DEPENDENCIES=ON]
    depends_on: [abseil-cpp]
  glfw:
    repo: https://github.com/glfw/glfw.git
    tag: "3.4"
    build: cmake
    options: [GLFW_BUILD_EXAMPLES=OFF, GLFW_BUILD_TESTS=OFF, GLFW_BUILD_DOCS=OFF, GLFW_BUILD_WAYLAND=OFF]
  googletest:
    repo: https://github.com/google/googletest.git
    tag: "v1.15.2"
    build: cmake
```

（imgui 不走池，见 Task 2 vendor。）
（abseil-cpp tag `20260817.0` 已核验：2026-08-24 查 GitHub API matching-refs 确认存在，且为最新 tag；实际拉取仍以 `.pool.lock.json` 记录 commit 为准。）

- [ ] **Step 2: `manifest.py` 支持 `depends_on`**

`LibSpec` 加字段 `depends_on: tuple = ()`；`resolve_libs` 解析 `d.get("depends_on", [])` 并冻结为 tuple。

- [ ] **Step 3: `cmake_driver` 注入 `CMAKE_PREFIX_PATH`**

`configure_command`：对指定 variant，扫描 `_install/*/<variant>/` 下已存在 `.built` 的前缀，用 `;` 拼接成 `-DCMAKE_PREFIX_PATH=<p1>;<p2>;...` 追加进 cmd。这样 ink configure 时能 `find_package(absl)` 命中池内 abseil。

- [ ] **Step 4: `build-deps.py` 按 `depends_on` 拓扑先序**

处理每个 lib 前，先确保其 `depends_on` 列表里的库已 build（未 build 先 build 依赖；递归处理并做环检测，环时报错退出）。已 build 的依赖不重复编。

- [ ] **Step 5: 补 tools 测试**

`tools/tests/test_cmake_driver.py` 增加断言：`configure_command` 对已 `.built` 变体含 `-DCMAKE_PREFIX_PATH`，且不含未 build 前缀。新增 `tools/tests/test_build_deps.py`：构造两个 lib（B `depends_on` A），断言 build 顺序 A 先于 B。

- [ ] **Step 6: 无 sudo 用户级部署系统依赖（`tools/install-user-deps.sh`）**

服务器无 root，改用下载包解压到 `$MINE_ROOT/.user-deps/`（根 `.gitignore` 覆盖）。新建 `tools/install-user-deps.sh`：
- ① Vulkan 工具链：下载 `https://sdk.lunarg.com/sdk/download/latest/linux/vulkan-sdk-latest.tar.xz` 解压到 `.user-deps/vulkan-sdk/`（含 `vulkan.h` 与 `glslc`）。
- ② X11 开发头（GLFW 仅 X11 后端）：`apt-get download` 以下**全部**并 `dpkg -x` 到 `.user-deps/`——**含传递依赖**：`libx11-dev libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libxext-dev libxcb1-dev libx11-xcb-dev x11proto-dev libxau-dev`（`apt-get download` 不拉依赖，必须显式列全；若服务器无 apt 包列表，改从 `archive.ubuntu.com` 直接 curl 指定 .deb）。
- ③ 重写绝对路径：`dpkg -x` 后对所有 `.user-deps/**/*.pc` 执行 `sed -i` 把 `prefix=/usr`（及 includedir/libdir）改写为 `.user-deps/` 前缀。
- ④ lavapipe（无 GPU 软件光栅）：`apt-get download libvulkan-lavapipe` + `dpkg -x`；`sed -i` 把其 ICD json 的 `library_path` 从 `/usr/...` 改写为 `.user-deps/` 实际路径。**运行期传递依赖**：`dpkg-deb -f libvulkan-lavapipe_*.deb Depends` 解析（libllvm libgbm1 libdrm2 libexpat1 libzstd1 libz1），`apt-get download` 递归拉取 + `dpkg -x`，由 `LD_LIBRARY_PATH` 覆盖。
- ⑤ Xvfb（windowed 虚拟 display）：`apt-get download xvfb` + `dpkg -x`（或探测 `xvfb-run` 可用性）；同样 `dpkg-deb -f xvfb_*.deb Depends` 递归拉取运行期 .so（libxfont2 libpixman-1-0 libgl1 libxshmfence1 等）。
- ⑥ 生成 `.user-deps/env.sh` 导出：`PATH`（glslc bin）、`PKG_CONFIG_PATH`（X11 头 pkgconfig）、`CMAKE_PREFIX_PATH`（Vulkan 前缀）、`CMAKE_INCLUDE_PATH`（X11 头目录，供 glfw 的 FindX11）、`LD_LIBRARY_PATH`（SDK lib + lavapipe 驱动目录，否则运行期 `vkCreateInstance` 失败）、`VK_DRIVER_FILES`（lavapipe ICD json）。
- `setup-env.sh` 探测调整为：检测 `.user-deps/env.sh` 是否存在 + 系统缺项，缺失时打印「先跑 `tools/install-user-deps.sh`」指引。
- **验证（真实探针，防假绿）**：① 编译最小 X11+GLFW 探针，**先 `Xvfb :99 &` + `DISPLAY=:99` 再 `glfwInit()`**（成功即过），跑完 `kill` Xvfb——必须先起 X display 再探针，顺序不可反；② 用 lavapipe 实际 `vkCreateInstance` + `vkEnumeratePhysicalDevices`（**断言 ≥1 物理设备**）成功；③ `which glslc` 且能编一个最小 `.frag→.spv`。

- [ ] **Step 7: 拉取 + 编译进池**

Run（先 `source .user-deps/env.sh`）: `tools/fetch-deps.py --all` 然后 `tools/build-deps.py --all`
Expected: abseil-cpp 先于 ink-stroke-modeler 编译成功；5 库（含 glfw-3.4，X11 后端）`_install/<name>-<ver>/{release,debug}/.built` 存在；二次运行全部 `SKIP`。

- [ ] **Step 8: 验证 find_package 目标可达**

临时 `probe/CMakeLists.txt`，`CMAKE_PREFIX_PATH` 指向 `third_party/_install/*/release`，逐一 `find_package(abs CONFIG REQUIRED)`、`find_package(glfw3 CONFIG REQUIRED)`、`find_package(GTest CONFIG REQUIRED)` 并打印目标存在性。**重点验证 ink-stroke-modeler**：确认 ink 的 CONFIG 可链接（目标 `InkStrokeModeler::stroke_modeler/types/params`），并确认其导出 targets 的 include 目录——实测为空则项目侧在 Task 2 显式 `target_include_directories(INTERFACE <ink前缀>/include)` 补齐（必要接线，非回退）。不做 add_subdirectory 等替代方案。

- [ ] **Step 9: 写项目依赖声明 + Commit**

`EasyPainter/deps.yaml`：

```yaml
use: [abseil-cpp, ink-stroke-modeler, glfw, glm, googletest]
```

```bash
git add third_party/deps.yaml tools/ EasyPainter/deps.yaml
git commit -m "feat(easypainter): pool libs + tooling(CMAKE_PREFIX_PATH/depends_on) + vulkan/x11 system deps"
```

---

### Task 2: 项目骨架 + core 静态库可编译

**Files:**
- Create: `EasyPainter/CMakeLists.txt`
- Create: `EasyPainter/CMakePresets.json`
- Create: `EasyPainter/.gitignore`
- Create: `EasyPainter/src/core/stroke/types.h`
- Create: `EasyPainter/src/core/stroke/predictor.h`（接口先置，实现留 Task 3）

**Interfaces:**
- Produces: 目标 `easypainter_core`（STATIC，`src/core/` 全部编译进）；`CMAKE_CXX_STANDARD 20`；`find_package` 链接池产物（含 Task 1 确认的 ink 链接方式）。

- [ ] **Step 1: 写 CMakeLists（含变体解析 + 池前缀）**

`EasyPainter/CMakeLists.txt`（变体解析沿用 workspace §8 的 `file(GLOB _install/*/${_variant})` 逻辑）：

```cmake
cmake_minimum_required(VERSION 3.22)
project(EasyPainter CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(MINE_ROOT "$ENV{MINE_ROOT}" CACHE PATH "Mine workspace root")
if(NOT MINE_ROOT)
  set(MINE_ROOT "${CMAKE_CURRENT_LIST_DIR}/..")
endif()
string(TOLOWER "${CMAKE_BUILD_TYPE}" _variant)
file(GLOB _pool_dirs LIST_DIRECTORIES true "${MINE_ROOT}/third_party/_install/*/${_variant}")
list(APPEND CMAKE_PREFIX_PATH ${_pool_dirs})

find_package(absl CONFIG REQUIRED)
find_package(glm CONFIG REQUIRED)
find_package(GTest CONFIG REQUIRED)
# ink-stroke-modeler：Task 1 已实测 CONFIG 链接 + 手动补 include 目录（必要接线，非回退）
# glfw：仅 windowed 目标(easypainter)需要，在 Task 6 目标作用域 find_package；headless/CLI 构建不依赖 GLFW

# imgui：项目内 vendor，不走池（Task 1 无此库）
add_library(imgui STATIC
  vendor/imgui/imgui.cpp
  vendor/imgui/imgui_draw.cpp
  vendor/imgui/imgui_tables.cpp
  vendor/imgui/imgui_widgets.cpp)
target_include_directories(imgui PUBLIC vendor/imgui)

add_library(easypainter_core STATIC
  src/core/stroke/types.h
  src/core/stroke/predictor.h)
target_include_directories(easypainter_core PUBLIC src)
```

（find_package 目标名与 ink 链接方式以 Task 1 实测为准，此处为骨架。）

- [ ] **Step 2: 写 CMakePresets.json**

沿用 workspace §8 的 debug/release 预设（`CMAKE_BUILD_TYPE` 分别 `Debug`/`Release`）。

- [ ] **Step 3: 写 .gitignore**

忽略 `build/`、`*.o`、`*.spv`（编译产物 SPIR-V）、`*.png`（离屏测试产物，除非作为 golden 提交）。`.user-deps/` 已由根 `.gitignore` 覆盖（Task 1），项目内不重复。

- [ ] **Step 4: 验证配置 + 编译通过**

Run: `cmake --preset debug && cmake --build --preset debug`
Expected: `easypainter_core` 静态库编译成功（`types.h` 仅 `#pragma once` + `#include <vector>`，无实现也可编）。

- [ ] **Step 5: Commit**

```bash
git add EasyPainter/CMakeLists.txt EasyPainter/CMakePresets.json EasyPainter/.gitignore EasyPainter/src/core/stroke/types.h
git commit -m "feat(easypainter): project skeleton + core static lib compiles"
```

---

### Task 3: stroke 核心（types + input_source + predictor）+ 单测

**Files:**
- Create: `EasyPainter/src/core/stroke/types.h`（补全）
- Create: `EasyPainter/src/core/stroke/input_source.h/.cpp`
- Create: `EasyPainter/src/core/stroke/predictor.h/.cpp`
- Create: `EasyPainter/tests/stroke_test.cpp`

**Interfaces:**
- Consumes: `easypainter_core`（Task 2）
- Produces:
  - `stroke::Vec2{float x,y}`、`stroke::InputType{kDown,kMove,kUp}`、`stroke::InputEvent{InputType type; Vec2 pos; float time_s;}`
  - `std::vector<InputEvent> input_source::build_events(const std::vector<Vec2>& pts)` — 首点 kDown、末点 kUp、其余 kMove，`time_s` 递增。
  - `class Predictor`：`Predictor(PredictorConfig cfg={})`、`void update(const InputEvent&, std::vector<Vec2>& out)`、`std::vector<Vec2> predict(const std::vector<InputEvent>&)`、`void reset()`。

- [ ] **Step 1: 写失败测试 `tests/stroke_test.cpp`**

```cpp
#include <gtest/gtest.h>
#include "core/stroke/input_source.h"
#include "core/stroke/predictor.h"
using namespace easypainter::stroke;

TEST(InputSource, BuildsDownMoveUpSequence) {
  auto ev = build_events({{0,0},{1,0},{2,0},{3,0}});
  ASSERT_EQ(ev.size(), 4u);
  EXPECT_EQ(ev.front().type, InputType::kDown);
  EXPECT_EQ(ev.back().type, InputType::kUp);
  EXPECT_EQ(ev[1].type, InputType::kMove);
  EXPECT_GT(ev[1].time_s, ev[0].time_s);
}

TEST(Predictor, EmptyInputNoCrash) {
  Predictor p;
  auto pts = p.predict({});
  EXPECT_TRUE(pts.empty());
}

TEST(Predictor, ProducesPointsForMove) {
  Predictor p;
  auto pts = p.predict({{InputType::kDown, {0,0}, 0.0f},
                        {InputType::kMove, {1,1}, 0.05f},
                        {InputType::kUp,   {2,1}, 0.10f}});
  EXPECT_FALSE(pts.empty());
}

TEST(Predictor, ResetClearsState) {
  Predictor p;
  p.predict({{InputType::kDown,{0,0},0.0f},{InputType::kMove,{1,1},0.05f}});
  p.reset();
  auto pts = p.predict({{InputType::kDown,{0,0},0.0f},{InputType::kMove,{1,1},0.05f}});
  EXPECT_FALSE(pts.empty());
}
```

在 `EasyPainter/CMakeLists.txt` 加 `enable_testing()` + `find_package(GTest)` + `add_executable(stroke_test tests/stroke_test.cpp)` + `target_link_libraries(stroke_test PRIVATE easypainter_core GTest::gtest GTest::gtest_main)` + `gtest_discover_tests(stroke_test)`。

- [ ] **Step 2: 跑测试确认失败**

Run: `ctest --test-dir build/debug --output-on-failure`
Expected: 编译失败（`input_source.h`/`predictor.h` 未定义函数体）或链接失败。

- [ ] **Step 3: 实现 input_source + predictor**

`input_source.cpp`：`build_events` 按首/末/中间分配 kDown/kUp/kMove，`time_s` 从 0 按固定步长递增。
`predictor.cpp`：持 `ink::stroke_model::StrokeModeler` 实例（读实际头文件对齐构造与 `Update`/`Result` 签名），把 `InputEvent` 映射为 ink 的 `Input`，`update` 把新预测点追加到 `out`；`reset` 重建 modeler。`PredictorConfig` 映射 ink 的 `StrokeModelerParams` 可调字段。

- [ ] **Step 4: 跑测试确认通过**

Run: `ctest --test-dir build/debug --output-on-failure`
Expected: 4 用例全过。

- [ ] **Step 5: Commit**

```bash
git add EasyPainter/src/core/stroke/ EasyPainter/tests/stroke_test.cpp EasyPainter/CMakeLists.txt
git commit -m "feat(easypainter): stroke predictor + input source with unit tests"
```

---

### Task 4: 渲染核心（image_io + vulkan_context + pipeline + offscreen）+ 图像 golden

**Files:**
- Modify: `EasyPainter/CMakeLists.txt`（render/bench 源并入 core + 链接 Vulkan）
- Create: `EasyPainter/vendor/stb_image_write.h`
- Create: `EasyPainter/src/core/render/image_io.h/.cpp`
- Create: `EasyPainter/src/core/render/vulkan_context.h/.cpp`
- Create: `EasyPainter/src/core/render/pipeline.h/.cpp`
- Create: `EasyPainter/src/core/render/offscreen.h/.cpp`
- Create: `EasyPainter/shaders/stroke.vert`、`EasyPainter/shaders/stroke.frag`
- Create: `EasyPainter/tests/image_golden_test.cpp`

**Interfaces:**
- Consumes: `stroke::Vec2`（Task 3）
- Produces:
  - `bool image_io::write_png(const std::string& path, uint32_t w, uint32_t h, const std::vector<uint8_t>& rgba)`
  - `class VulkanContext`（RAII：instance/device/queue，无 surface；`init()`/`~`）
  - `class Pipeline`（持有渲染管线 + shader，`void draw(VkCommandBuffer, const std::vector<Vec2>&)`）
  - `OffscreenResult render::render_offscreen(const VulkanContext&, const Pipeline&, const std::vector<Vec2>&, uint32_t w, uint32_t h)`，`OffscreenResult{uint32_t width,height; std::vector<uint8_t> rgba;}`

- [ ] **Step 1: vendor stb_image_write.h**

从 `https://github.com/nothings/stb` 单头 `stb_image_write.h` 复制进 `EasyPainter/vendor/`（保留版权头）。

- [ ] **Step 2: 写失败测试（image_io 纯逻辑）**

`tests/image_golden_test.cpp` 先测 `image_io`（不依赖 Vulkan）：

```cpp
#include <gtest/gtest.h>
#include "core/render/image_io.h"
TEST(ImageIO, WritesPngFile) {
  std::vector<uint8_t> rgba(4*2*2, 255);
  ASSERT_TRUE(easypainter::render::write_png("/tmp/t.png", 2, 2, rgba));
  // 读回文件头校验 PNG magic（8 字节 89 50 4E 47 ...）
}
```

- [ ] **Step 3: 实现 image_io**

`image_io.cpp`：`#define STB_IMAGE_WRITE_IMPLEMENTATION` 后 `#include "stb_image_write.h"`，封装 `stbi_write_png`。

- [ ] **Step 4: 写 shader + CMake 自定义命令编 SPIR-V**

`stroke.vert`/`stroke.frag`（GLSL：MVP + 单色 stroke）。CMake 用 `add_custom_command` 调 `glslc` 把 `.vert/.frag` 编成 `.spv` 输出到 `build/`（glslc 由 Task 1 部署提供）；`Pipeline` 运行时加载 `.spv`。不做预编译 `.spv` 提交回退。

- [ ] **Step 5: 实现 vulkan_context / pipeline / offscreen**

`vulkan_context`：`vkCreateInstance`（无 surface 扩展要求）+ 选物理设备 + 建逻辑设备/queue。
`pipeline`：建 render pass（单附件）、图形管线、从 `.spv` 建 shader module；`draw` 绑定顶点 buffer 后 `vkCmdDraw`。
`offscreen`：建离屏 `VkImage`（`COLOR_ATTACHMENT|TRANSFER_SRC`）→ 渲染 → `vkCmdCopyImageToBuffer` 到 host-visible buffer → `vkMapMemory` 填 `rgba`。
接线：把 `src/core/render/*.cpp` 并入 `easypainter_core`（CMake 源列表），并 `find_package(Vulkan REQUIRED)` + `target_link_libraries(easypainter_core PUBLIC Vulkan::Vulkan)`。render 子系统自此归 core 所有，Task 5/6 的 target 经 core 的 PUBLIC 传递获得 Vulkan，无需单独链接。bench 源到 Task 7 才创建，届时再追加进 core（见 Task 7）。

- [ ] **Step 6: 跑 image_io 测试**

Run: `ctest --test-dir build/debug -R ImageIO --output-on-failure`
Expected: 通过。图像 golden 无 GPU 时经 lavapipe 软件光栅运行，不跳过（与验收 100 分一致）；README 记录 golden 生成所用 lavapipe 版本。

- [ ] **Step 7: 生成基准图像并写图像 golden 断言**

`easypainter-cli` 就绪后（Task 5），用内置示例点产一张基准 PNG 提交为 golden；`image_golden_test.cpp` 用 `render_offscreen` 重渲并逐像素比对（容差阈值）。

- [ ] **Step 8: Commit**

```bash
git add EasyPainter/vendor EasyPainter/src/core/render EasyPainter/shaders EasyPainter/tests/image_golden_test.cpp
git commit -m "feat(easypainter): vulkan offscreen render core + image_io + golden"
```

---

### Task 5: CLI 入口（离屏渲染输出图像）

**Files:**
- Create: `EasyPainter/src/cli/main.cpp`
- Modify: `EasyPainter/CMakeLists.txt`

**Interfaces:**
- Consumes: `predictor`（Task 3）、`render_offscreen`（Task 4）
- Produces: 可执行 `easypainter-cli`；CLI 契约 `--input <x,y文件> --output <png> [--width N --height M] [--stroke <预测器参数>]`；零参数时用内置示例点输出确定性 PNG。

- [ ] **Step 1: 写 CLI 参数解析 + 主流程**

`src/cli/main.cpp`：解析参数（缺省 `--output out.png`、`--width 640 --height 480`；`--stroke <预测器参数>` 如 `--stroke 5.0,0.9` 映射 `PredictorConfig` 对应字段，与 spec §6 契约一致）→ 读点文件（无则内置示例点）→ `build_events` → `predict` → `render_offscreen` → `write_png` → 退出码。

- [ ] **Step 2: CMake 增加 `easypainter-cli` 目标**

```cmake
add_executable(easypainter-cli src/cli/main.cpp)
target_link_libraries(easypainter-cli PRIVATE easypainter_core)  # Vulkan 经 core 的 PUBLIC 传递,无需单独链接
```

- [ ] **Step 3: 验证离屏输出**

Run: `./build/debug/easypainter-cli --output /tmp/out.png`
Expected: 退出码 0，`/tmp/out.png` 为合法 PNG（`file /tmp/out.png` 报 PNG，非零大小）。

- [ ] **Step 4: Commit**

```bash
git add EasyPainter/src/cli EasyPainter/CMakeLists.txt
git commit -m "feat(easypainter): CLI headless offscreen render to PNG"
```

---

### Task 6: windowed 入口 + ImGui GUI

**Files:**
- Create: `EasyPainter/src/app/main.cpp`
- Create: `EasyPainter/src/app/gui.h/.cpp`
- Modify: `EasyPainter/CMakeLists.txt`

**Interfaces:**
- Consumes: `predictor`、`pipeline`、`vulkan_context`（前三任务）
- Produces: 可执行 `easypainter`（GLFW 窗口 + ImGui + Vulkan swapchain + 轨迹渲染 + 调参面板）。

- [ ] **Step 1: GLFW+ImGui+Vulkan 初始化骨架**

`src/app/main.cpp`：`glfwInit` → `glfwCreateWindowSurface` → swapchain → 初始化 `imgui_impl_glfw` + `imgui_impl_vulkan` → 主循环（poll 事件 → 采集鼠标轨迹 → `predict` → `pipeline.draw` → ImGui 渲染 → present）。

- [ ] **Step 2: ImGui 面板**

`gui.cpp`：调参面板（`PredictorConfig` 各字段滑条）、轨迹显示、benchmark 曲线（Task 7 数据）。

- [ ] **Step 3: CMake 增加 `easypainter` 目标并链接 GLFW+ImGui+Vulkan**

在 `easypainter` 目标作用域内 `find_package(glfw3 CONFIG REQUIRED)`（不污染顶层）与 `find_package(Vulkan)`；链接 `glfw`、vendor 的 `imgui` 目标，以及 `vendor/imgui/backends/imgui_impl_glfw.cpp`、`imgui_impl_vulkan.cpp` 后端源。注释标明 headless/CLI 构建不依赖 GLFW。

- [ ] **Step 4: 验证窗口可运行（Xvfb/Xvnc 虚拟 display）**

Run（无物理显示器，Xvfb 已由 Task 1 Step 6 部署）: `source .user-deps/env.sh && (Xvfb :99 &) && DISPLAY=:99 ./build/debug/easypainter`（或 `xvfb-run -a`）
Expected: 在虚拟 X display 上弹出窗口，拖拽鼠标显示预测轨迹，调参面板可交互，退出正常。Xvfb 部署失败且系统无 Xvfb 时，作为阻塞按 Task 1 Step 6 指引修复（不静默降级）。

- [ ] **Step 5: Commit**

```bash
git add EasyPainter/src/app EasyPainter/CMakeLists.txt
git commit -m "feat(easypainter): windowed ImGui+Vulkan app with tuning panel"
```

---

### Task 7: bench + 性能断言

**Files:**
- Modify: `EasyPainter/CMakeLists.txt`（把 `src/core/bench/bench.cpp` 追加进 `easypainter_core` 源列表）
- Create: `EasyPainter/src/core/bench/bench.h/.cpp`
- Create: `EasyPainter/tests/bench_test.cpp`

**Interfaces:**
- Consumes: `Predictor`（Task 3）
- Produces: `LatencyStats{double p50_ms,p99_ms,mean_ms;}`；`LatencyStats measure_update_latency(Predictor&, const InputEvent&, int iters)`；`double measure_throughput_pts_per_s(Predictor&, const std::vector<InputEvent>&)`。

- [ ] **Step 1: 写失败测试**

`tests/bench_test.cpp`：`EXPECT_LT(stats.mean_ms, 10.0)`（单次 update 均值 <10ms，宽松上限，避免抖动误报）、`EXPECT_GT(throughput, 0.0)`。

- [ ] **Step 2: 实现 bench**

`bench.cpp`：`std::chrono::steady_clock` 循环测单次 `update` 延迟，排序取 p50/p99/mean；吞吐 = 总预测点数 / 总耗时。接线：在 `easypainter_core` 源列表追加 `src/core/bench/bench.cpp`（Task 4 未引用 bench，此刻文件已存在，configure 不再缺源）。

- [ ] **Step 3: 跑测试确认通过**

Run: `ctest --test-dir build/debug -R Bench --output-on-failure`
Expected: 通过。

- [ ] **Step 4: 把 bench 数据接进 GUI**

`gui.cpp` 调 `measure_*` 并绘曲线（Task 6 面板预留位）。

- [ ] **Step 5: Commit**

```bash
git add EasyPainter/src/core/bench EasyPainter/tests/bench_test.cpp EasyPainter/src/app/gui.cpp
git commit -m "feat(easypainter): prediction latency/throughput benchmark + assertions"
```

---

### Task 8: 数值 golden 对比 + 收尾

**Files:**
- Create: `EasyPainter/tests/golden_test.cpp`
- Create: `EasyPainter/tests/data/golden_points.txt`（官方示例基准点）
- Modify: `EasyPainter/README.md`（新建项目说明）

**Interfaces:**
- Consumes: `predictor`（Task 3）
- Produces: 数值 golden 用例 + 项目 README + 干净 git。

- [ ] **Step 1: 采集独立 oracle 基准点**

优先用 ink-stroke-modeler 仓库自带测试数据/期望输出（`third_party/_src/ink-stroke-modeler-*/` 下 testdata 或 upstream 测试用例）作为 golden 源，拷贝固定输入与期望输出到 `tests/data/golden_input.txt`/`golden_points.txt`（`x,y` 每行），并注明坐标归一化。
若仓库无现成期望输出：单独编译 ink 官方 example（独立于本工程 predictor 的另一条编译路径）对固定输入生成 `golden_points.txt` 并提交，生成器与生成环境写进 README。
**禁止用本工程 predictor 自产自比。**

- [ ] **Step 2: 写 golden 对比测试**

`tests/golden_test.cpp`：读 `golden_points.txt` 与 `predictor` 对同输入的重算结果，逐点比对（`EXPECT_NEAR` 容差 `1e-4`）。

- [ ] **Step 3: 全量跑 ctest 确认 100 分**

Run: `ctest --test-dir build/debug --output-on-failure`
Expected: 全部用例 0 失败 0 跳过（对应 SKILL 测试门 100 分）。有显示/无显示环境均通过（离屏与逻辑测试无 surface 依赖）。

- [ ] **Step 4: 写 README + 清理**

`EasyPainter/README.md`：项目简介、构建三步（fetch/build/cmake）、CLI 用法、窗口用法、测试命令、图像 golden 环境（lavapipe）。清理临时 `probe/`、`/tmp` 产物。

- [ ] **Step 5: 最终提交**

```bash
git add EasyPainter/tests/golden_test.cpp EasyPainter/tests/data/golden_points.txt EasyPainter/README.md
git commit -m "feat(easypainter): numeric golden test + docs + cleanup"
```

---

## 自检记录

- **Spec 覆盖**：三目标（端到端 Task 6、正确性 Task 8 + Task 4 图像、性能 Task 7）、CLI+离屏（Task 5 + Task 4）、三方库（Task 1）、C++20（Task 2）、验收标准逐条对应 Task 3/5/6/8。
- **占位扫描**：无 TBD/TODO；ink 真实 API 由 Task 3 读实际头文件对齐（以稳定包装接口 `Predictor` 隔离），非占位。
- **类型一致**：`Vec2/InputEvent/Predictor/render_offscreen/OffscreenResult/LatencyStats` 跨任务签名一致。
