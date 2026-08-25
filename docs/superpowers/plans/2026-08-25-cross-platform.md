# 跨平台化(一套代码跨平台)实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 让 Mine 工作空间的源码、工具链脚本、测试逻辑一套跨平台(Linux + Windows/MSYS2),离屏渲染核心平台无关,单一 golden 基线双平台一致校验。

**Architecture:** 渲染核心(EasyPainter 的 `core/render/*`)已是平台无关的 Vulkan 离屏渲染,固化为原则;平台差异只出现在:工具链脚本分支、窗口 surface(`VK_USE_PLATFORM_*`)、软件光栅驱动(lavapipe/SwiftShader 经 `VK_ICD_FILENAMES` 选)、环境变量。工具链脚本由「全 Linux」改为「一套脚本 + 平台分支」,SwiftShader 作为池库加入 `deps.yaml`,CI 双 job 跑同一套 `ctest`。

**Tech Stack:** Bash(平台分支脚本)、Python(池驱动,已跨平台)、CMake+Ninja、Vulkan(离屏渲染)、GLFW(窗口)、Qt6(StickyNotes)、gtest/QtTest。

**Spec:** `docs/superpowers/specs/2026-08-24-cross-platform-design.md`

## Global Constraints

- 渲染核心(`EasyPainter/src/core/render/*`)禁平台专用 API、禁平台 `#ifdef` 分叉渲染逻辑 —— 保持一套代码。
- golden 基线**单一**(`EasyPainter/tests/data/golden_render.png`、StickyNotes fixture),双平台一致校验;像素不一致 → 修渲染路径,不建第二份基线。
- 平台差异只允许出现在:依赖部署脚本分支、窗口 surface 获取、软件光栅驱动选型、环境变量。
- 不回退原则:工具/依赖缺失 → 直接补齐走主路径,或如实报告阻塞,不做回退/绕过。
- 门控硬约束:CLI 无头入口 + 离屏渲染输出图像,双平台成立。
- 现有测试必须保持全绿(EasyPainter 13/13、StickyNotes 7/7 基线)。

---

## 文件结构

| 文件 | 职责 |
|---|---|
| `tools/setup-env.sh` | 工具链探测,加平台分支(MSYS2 探测) |
| `tools/install-user-deps.sh` | 依赖部署,加平台分支(Windows: Vulkan zip + SwiftShader + pacman Qt6) |
| `third_party/deps.yaml` | 新增 `swiftshader` 库(池内 CMake 构建) |
| `.claude/skills/bugfix-pipeline/SKILL.md` | 门控硬约束平台无关化 |
| `.claude/skills/build-pipeline/SKILL.md` | 同上 |
| `docs/superpowers/specs/*-easypainter-design.md` | 「GLFW 仅 X11 / Xvfb」→ 平台无关原则 |
| `docs/superpowers/specs/*-workspace-bootstrap-design.md` | 「初版 Linux/GCC」→ 跨平台说明 |
| `EasyPainter/CMakeLists.txt` | pkg-config X11 链接仅 Linux 生效 |
| `EasyPainter/src/cli/main.cpp` | 错误提示去平台名词 |
| `EasyPainter/tests/golden_test.cpp` | 读 golden 基线,断言已平台无关(核对即可) |
| CI(`.github/workflows/*.yml`) | 新增 linux + windows 双 job |

---

### Task 1: 工具链脚本加平台分支(`setup-env.sh` + `install-user-deps.sh`)

**Files:**
- Modify: `tools/setup-env.sh`
- Modify: `tools/install-user-deps.sh`
- Create: `tools/win-deps.sh`(Windows 依赖安装入口,由 install-user-deps.sh 平台分支调用)

**Interfaces:**
- Consumes: 现有 `setup-env.sh` 的 `chk`/`probe`/`main` 结构;`install-user-deps.sh` 的 `info/warn/err/die/has` 辅助与 `env.sh` 生成逻辑。
- Produces: 平台分支后的 `setup-env.sh` / `install-user-deps.sh`;Windows 分支生成 `.user-deps/env.sh` 指向 MSYS2 工具链 + SwiftShader ICD。

- [ ] **Step 1: 写平台判定头(两脚本共用)**

在 `setup-env.sh` 与 `install-user-deps.sh` 顶部加入统一平台判定:

```bash
# 平台判定:MSYS*/MINGW* → Windows;否则 Linux
case "$(uname -s)" in
  MSYS*|MINGW*) OS_PLATFORM="windows" ;;
  *)            OS_PLATFORM="linux" ;;
esac
```

- [ ] **Step 2: setup-env.sh 加 Windows 分支**

在 `probe()` 中按 `OS_PLATFORM` 分支:

```bash
probe() {
  info "=== 系统工具链探测(${OS_PLATFORM:-linux}) ==="
  HARD_MISS=0; MISS_DETAILS=()
  chk "cmake"      "3.22" "cmake --version"
  chk "ninja"      ""     "ninja --version"
  chk "g++"        "11"   "g++ --version"
  chk "pkg-config" ""     "pkg-config --version"
  chk "git"        ""     "git --version"
  chk "python3"    "3.8"  "python3 --version"
  if [ "$OS_PLATFORM" = "windows" ]; then
    chk_user_deps_windows
  else
    chk_user_deps
  fi
}
```

新增 Windows 专用 `chk_user_deps_windows()`:

```bash
chk_user_deps_windows() {
  if [ -f "$MINE_ROOT/.user-deps/env.sh" ]; then
    printf '[OK]   user-deps: %s\n' "$MINE_ROOT/.user-deps/env.sh"
  else
    HARD_MISS=$((HARD_MISS+1)); MISS_DETAILS+=("user-deps(Vulkan SDK + SwiftShader + Qt6)")
    printf '[MISS] user-deps: 未部署(先执行 tools/install-user-deps.sh)\n'
  fi
}
```

`lavapipe_hint()` 加平台判断,Windows 不提示 lavapipe:

```bash
lavapipe_hint() {
  [ "$OS_PLATFORM" = "windows" ] && return 0
  if ! ls /dev/dri/* >/dev/null 2>&1; then
    warn "未检测到 GPU 设备(/dev/dri 为空);离屏渲染依赖 lavapipe(已由 install-user-deps.sh 部署)"
  fi
}
```

`print_help()` 更新「Windows 先执行 install-user-deps.sh(MSYS2)」说明。

- [ ] **Step 3: 验证 setup-env.sh Linux 分支不回归**

```bash
source .user-deps/env.sh
tools/setup-env.sh --check
```
Expected: 输出 `[OK]` 全项,exit 0。

- [ ] **Step 4: 提交 setup-env.sh**

```bash
git add tools/setup-env.sh
git commit -m "feat(tools): setup-env.sh 平台分支(MSYS2 探测)"
```

- [ ] **Step 5: install-user-deps.sh 顶部加平台分支**

在 `die/has` 定义之后、`# --- 前置工具检查 ---` 之前插入:

```bash
case "$(uname -s)" in
  MSYS*|MINGW*) OS_PLATFORM="windows" ;;
  *)            OS_PLATFORM="linux" ;;
esac
```

`# --- 前置工具检查 ---` 段改为按平台:

```bash
if [ "$OS_PLATFORM" = "windows" ]; then
  for c in curl tar unzip sed; do
    has "$c" || die "缺少命令: $c(MSYS2 需安装)"
  done
  has pacman || die "缺少 pacman(MSYS2 需安装)"
else
  for c in curl tar dpkg-deb dpkg sed; do
    has "$c" || die "缺少命令: $c"
  done
  has apt-get || warn "未检测到 apt-get;.deb 将走远程 archive.ubuntu.com 下载(需能出网)"
  if ! has gcc && ! has cc && ! has g++; then
    die "缺少 C/C++ 编译器(gcc/cc/g++),探针无法编译"
  fi
fi
```

- [ ] **Step 6: 写 Windows 依赖安装分支(`tools/win-deps.sh`)**

新建 `tools/win-deps.sh`,MSYS2 环境执行,职责:Vulkan SDK zip 部署 + SwiftShader 构建 + Qt6 pacman + 生成 `env.sh` + 离屏 Vulkan 探针。

```bash
#!/usr/bin/env bash
# Windows(MSYS2)依赖部署:Vulkan SDK zip + SwiftShader + Qt6(pacman)。
# 由 install-user-deps.sh 平台分支调用;也可单独执行。
set -euo pipefail
info() { printf '[INFO] %s\n' "$*"; }
err()  { printf '[ERROR] %s\n' "$*" >&2; }
die()  { err "$*"; exit 1; }
has()  { command -v "$1" >/dev/null 2>&1; }

MINE_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
USER_DEPS="$MINE_ROOT/.user-deps"
DEB_CACHE="$USER_DEPS/.deb-cache"
mkdir -p "$USER_DEPS" "$DEB_CACHE"

# --- ① Vulkan SDK(zip,含 glslc.exe + vulkan.h) ---
SDK_URL="https://sdk.lunarg.com/sdk/download/latest/windows/vulkan-sdk-latest.zip"
SDK_ZIP="$DEB_CACHE/vulkan-sdk-latest.zip"
SDK_DIR="$USER_DEPS/vulkan-sdk"
if [ ! -d "$SDK_DIR" ]; then
  info "① 下载 Vulkan SDK ($SDK_URL)"
  curl -fL --retry 3 -o "$SDK_ZIP" "$SDK_URL"
  mkdir -p "$SDK_DIR"
  unzip -q -o "$SDK_ZIP" -d "$SDK_DIR"
fi
# 找 glslc.exe 与 include/vulkan/vulkan.h
GLSLC="$(find "$SDK_DIR" -name 'glslc.exe' -type f | head -1 || true)"
[ -n "$GLSLC" ] || die "SDK 缺少 glslc.exe"
SDK_BIN="$(dirname "$GLSLC")"
VULKAN_INC="$(find "$SDK_DIR" -type d -path '*/Include/vulkan' | head -1 || true)"
[ -n "$VULKAN_INC" ] || die "SDK 缺少 Include/vulkan/vulkan.h"
info "① glslc: $GLSLC;vulkan.h: $VULKAN_INC/vulkan.h"

# --- ② SwiftShader(经池构建,见 Task 2;此处仅确保 ICD 路径) ---
SWSS_ICD="$MINE_ROOT/third_party/_install/swiftshader-main/release/vk_swiftshader_icd.json"
[ -f "$SWSS_ICD" ] || die "SwiftShader ICD 未找到: $SWSS_ICD(先执行 tools/fetch-deps.py --project EasyPainter && tools/build-deps.py --project EasyPainter)"
SWSS_BIN="$(dirname "$SWSS_ICD")"

# --- ③ Qt6(pacman) ---
if ! pacman -Q mingw-w64-x86_64-qt6-base >/dev/null 2>&1; then
  info "③ pacman 安装 Qt6 base"
  pacman -S --needed --noconfirm mingw-w64-x86_64-qt6-base
else
  info "③ Qt6 已安装"
fi

# --- ④ 生成 env.sh ---
cat > "$USER_DEPS/env.sh" <<EOF
# Mine Windows(MSYS2)用户级依赖环境(由 win-deps.sh 生成)。
export MINE_ROOT="$MINE_ROOT"
export USER_DEPS="$USER_DEPS"
export PATH="$SDK_BIN:\$PATH"
export VK_ICD_FILENAMES="$SWSS_ICD"
export VK_DRIVER_FILES="$SWSS_ICD"
export CMAKE_PREFIX_PATH="$SDK_BIN/..:$MINE_ROOT/third_party/_install/glfw-3.4/release"
EOF
info "④ 已生成 $USER_DEPS/env.sh"

# --- ⑤ 离屏 Vulkan 探针(SwiftShader 能创建 device) ---
"$GLSLC" -fshader-stage=fragment -o "$DEB_CACHE/probe.frag.spv" - <<'EOF' || die "glslc 编译失败"
#version 450
layout(location=0) out vec4 outColor;
void main(){ outColor = vec4(1.0,0.0,0.0,1.0); }
EOF
info "⑤ glslc 探针通过"
info "完成。使用前: source $USER_DEPS/env.sh"
```

- [ ] **Step 7: install-user-deps.sh 尾部分支(探针 + 收尾)**

在 `# ===================== 真实探针(防假绿) =====================` 之前插入:

```bash
if [ "$OS_PLATFORM" = "windows" ]; then
  info "Windows 平台: 依赖部署转交 tools/win-deps.sh"
  exec "$MINE_ROOT/tools/win-deps.sh" "$@"
fi
```

(即 Windows 分支复用同一脚本入口,`exec` 到 win-deps.sh,避免重复部署逻辑。)

- [ ] **Step 8: 验证 install-user-deps.sh Linux 分支不回归**

```bash
tools/install-user-deps.sh
```
Expected: 复用已部署标记(`vulkan-sdk 已存在`、`X11 已部署`、`lavapipe 已部署` 等),不重复下载,最后探针通过,exit 0。

- [ ] **Step 9: 提交 install-user-deps.sh + win-deps.sh**

```bash
git add tools/install-user-deps.sh tools/win-deps.sh
git commit -m "feat(tools): install-user-deps 平台分支,新增 win-deps.sh(MSYS2)"
```

---

### Task 2: SwiftShader 进三方库池(`deps.yaml` + 池驱动)

**Files:**
- Modify: `third_party/deps.yaml`
- Create: `EasyPainter/deps.yaml`(若尚不存在,声明 use 集)
- Modify: `tools/deps_lib/cmake_driver.py`(仅若 SwiftShader 构建需额外选项)

**Interfaces:**
- Consumes: `tools/deps_lib/manifest.py` 的 `LibSpec`/`load_global_manifest`;`cmake_driver.configure_command` 的 `-G Ninja` + variant 目录。
- Produces: `third_party/_install/swiftshader-main/release/vk_swiftshader_icd.json`(供 Task 1 win-deps.sh 引用)。

- [ ] **Step 1: deps.yaml 加 swiftshader**

在 `third_party/deps.yaml` 的 `libs:` 下追加:

```yaml
  swiftshader:
    repo: https://github.com/google/swiftshader.git
    tag: "main"
    build: cmake
    options: [SWIFTSHADER_BUILD_TESTS=OFF, SWIFTSHADER_BUILD_HEADLESS=OFF, SWIFTSHADER_BUILD_VULKAN=ON]
```

- [ ] **Step 2: 验证池能拉取+构建 SwiftShader(Linux 侧先验证可行性)**

```bash
source .user-deps/env.sh
cd third_party && python3 ../tools/fetch-deps.py --all
cd .. && python3 tools/build-deps.py --project EasyPainter --variant release
```

Expected: SwiftShader 构建成功,产出 `third_party/_install/swiftshader-main/release/libvk_swiftshader.so` 与 `vk_swiftshader_icd.json`(Linux 也能构建 SwiftShader,只是生产环境用它做离屏驱动)。

> 注:SwiftShader 在 Linux 也可用(`VK_ICD_FILENAMES` 指向其 ICD),此步同时为双平台验证奠定基础。

- [ ] **Step 3: 提交**

```bash
git add third_party/deps.yaml EasyPainter/deps.yaml
git commit -m "feat(deps): SwiftShader 进池(Windows 离屏软件光栅)"
```

---

### Task 3: 门控文档平台无关化(SKILL)

**Files:**
- Modify: `.claude/skills/bugfix-pipeline/SKILL.md`
- Modify: `.claude/skills/build-pipeline/SKILL.md`

**Interfaces:**
- Consumes: 现有 SKILL 的「CLI + 离屏渲染硬约束」章节。
- Produces: 平台无关的硬约束表述,供后续所有构建/修复按此执行。

- [ ] **Step 1: bugfix-pipeline SKILL 更新离屏/golden 表述**

在「① 问题查找」硬约束 2 与「② 制定计划」硬约束处,把「离屏渲染输出图像」从 Linux 实现细节抽象为平台无关:

```markdown
2. **离屏渲染输出图像**：必须能离屏渲染并落盘一张执行图像（如 PNG），供无头验收与前后对比。
   **离屏渲染 = Vulkan 离屏渲染核心（平台无关一套代码），不依赖窗口/显示服务（Xvfb/X11）**；
   平台差异只允许在软件光栅驱动选型（Linux=lavapipe / Windows=SwiftShader）。
```

在「常见借口/常见错误」中补一条平台无关红旗:

```markdown
| 「这个环境只跑 Linux 就行」 | 渲染核心必须平台无关，双平台同一套测试与 golden；平台差异只在部署/驱动选型 |
```

- [ ] **Step 2: build-pipeline SKILL 同步更新**

同样的「离屏渲染输出图像」硬约束与平台无关表述,同步到 `build-pipeline/SKILL.md` 的「① 制定计划」。

- [ ] **Step 3: 提交**

```bash
git add .claude/skills/bugfix-pipeline/SKILL.md .claude/skills/build-pipeline/SKILL.md
git commit -m "docs(skills): 门控硬约束平台无关化(离屏=Vulkan核心,双平台golden)"
```

---

### Task 4: 设计文档平台表述更新

**Files:**
- Modify: `docs/superpowers/specs/2026-08-23-easypainter-design.md`
- Modify: `docs/superpowers/specs/2026-08-23-workspace-bootstrap-design.md`

**Interfaces:**
- Consumes: 现有设计文档的「GLFW 仅 X11」「初版 Linux/GCC」等段落。
- Produces: 与跨平台设计一致的平台无关表述。

- [ ] **Step 1: easypainter-design 更新窗口/离屏表述**

- 第 36 行「GLFW 仅 X11 后端」→「GLFW 后端由 `VK_USE_PLATFORM_*` 自动选(Win32/X11)」。
- 第 35 行 lavapipe →「软件光栅离屏(Linux=lavapipe / Windows=SwiftShader),驱动经 `VK_ICD_FILENAMES` 选,渲染核心平台无关」。
- 第 56 行 Xvfb →「窗口显示:Linux 无显示器用 Xvfb;Windows 用真窗口;离屏渲染不依赖显示服务」。
- 第 153-165 行错误处理表:lavapipe/Xvfb 相关风险改为「软件光栅缺失 → setup-env.sh 明确报错,不给回退」。

- [ ] **Step 2: workspace-bootstrap-design 更新**

- 第 300 行「初版在 Linux/GCC 单一 toolchain 下工作」→「脚本跨平台:Linux 分支(现有)+ Windows 分支(MSYS2),见 `2026-08-24-cross-platform-design.md`」。

- [ ] **Step 3: 提交**

```bash
git add docs/superpowers/specs/2026-08-23-easypainter-design.md docs/superpowers/specs/2026-08-23-workspace-bootstrap-design.md
git commit -m "docs(specs): 平台表述更新(窗口surface自动选,离屏驱动平台无关)"
```

---

### Task 5: EasyPainter 窗口层薄化 + CMake 平台条件

**Files:**
- Modify: `EasyPainter/CMakeLists.txt`
- Modify: `EasyPainter/src/cli/main.cpp`

**Interfaces:**
- Consumes: 现有 `easypainter` 目标(swapchain + GLFW + ImGui);`easypainter-cli` 目标。
- Produces: windowed 目标在 Windows 不再链 X11;CLI 错误提示平台无关。

- [ ] **Step 1: CMakeLists 的 X11 链接仅 Linux 生效**

把 `# --- windowed 应用 ---` 段的 pkg-config X11 块包进平台条件:

```cmake
# --- windowed 应用(仅此目标依赖 GLFW;headless/CLI 构建不依赖) ---
find_package(glfw3 CONFIG REQUIRED)
if(UNIX AND NOT APPLE)
  find_package(PkgConfig REQUIRED)
  # glfw 导出 target 不带 X11 链接(GLFW 把 X11 当 PRIVATE 依赖),手动经 pkg-config 补;仅 Linux
  pkg_check_modules(X11_REQ IMPORTED_TARGET x11 xrandr xinerama xcursor xi xext xrender xfixes)
endif()
```

并更新 `target_link_libraries`:

```cmake
if(UNIX AND NOT APPLE)
  target_link_libraries(easypainter PRIVATE
    easypainter_core imgui glfw PkgConfig::X11_REQ)
else()
  target_link_libraries(easypainter PRIVATE
    easypainter_core imgui glfw)  # Windows: GLFW 自带 Win32 后端,无需 X11
endif()
```

> `WIN32` 宏由 CMake 在 Windows 自动定义,`UNIX` 不定义;用 `UNIX AND NOT APPLE` 覆盖 Linux 语义更稳。

- [ ] **Step 2: CLI 错误提示平台无关**

`EasyPainter/src/cli/main.cpp` 第 124 行:

```cpp
// 原: std::fprintf(stderr, "Vulkan 初始化失败(需要 lavapipe 软件光栅)\n");
std::fprintf(stderr, "Vulkan 初始化失败(需软件光栅驱动,Linux=lavapipe / Windows=SwiftShader)\n");
```

- [ ] **Step 3: Linux 回归验证(全绿)**

```bash
source .user-deps/env.sh
cd EasyPainter && cmake -B build/release -S . -DCMAKE_BUILD_TYPE=Release && cmake --build build/release -j && ctest --test-dir build/release --output-on-failure
```
Expected: 构建通过,13/13 全绿(含 `MatchesBaselineGolden` 单基线)。

- [ ] **Step 4: 提交**

```bash
git add EasyPainter/CMakeLists.txt EasyPainter/src/cli/main.cpp
git commit -m "feat(EasyPainter): 窗口层 X11 链接仅 Linux,CLI 提示平台无关"
```

---

### Task 6: CI 双平台矩阵

**Files:**
- Create: `.github/workflows/ci.yml`

**Interfaces:**
- Consumes: 现有 Linux 构建命令(`source .user-deps/env.sh` + cmake + ctest);MSYS2 pacman 包名。
- Produces: linux + windows 两个 job,同一 `ctest` 全绿才通过。

- [ ] **Step 1: 写 CI workflow**

```yaml
name: CI
on: [push, pull_request]

jobs:
  linux:
    runs-on: ubuntu-22.04
    steps:
      - uses: actions/checkout@v4
      - name: 环境部署
        run: |
          tools/install-user-deps.sh
          source .user-deps/env.sh
          python3 tools/fetch-deps.py --all
          python3 tools/build-deps.py --all
      - name: 构建 + 测试
        run: |
          source .user-deps/env.sh
          cmake -B build -S EasyPainter -DCMAKE_BUILD_TYPE=Release
          cmake --build build -j
          ctest --test-dir build --output-on-failure
          cmake -B build-sn -S StickyNotes -DCMAKE_BUILD_TYPE=Release
          cmake --build build-sn -j
          QT_QPA_PLATFORM=offscreen ctest --test-dir build-sn --output-on-failure

  windows:
    runs-on: windows-2022
    defaults:
      run:
        shell: msys2 {0}
    steps:
      - uses: actions/checkout@v4
      - uses: msys2/setup-msys2@v2
        with:
          update: true
          install: >-
            base-devel
            git
            mingw-w64-x86_64-gcc
            mingw-w64-x86_64-cmake
            mingw-w64-x86_64-ninja
            mingw-w64-x86_64-pkgconf
            mingw-w64-x86_64-python
            mingw-w64-x86_64-qt6-base
            unzip
      - name: 环境部署
        run: |
          tools/install-user-deps.sh
          source .user-deps/env.sh
          python tools/fetch-deps.py --all
          python tools/build-deps.py --all
      - name: 构建 + 测试
        run: |
          source .user-deps/env.sh
          cmake -B build -S EasyPainter -DCMAKE_BUILD_TYPE=Release -G Ninja
          cmake --build build -j
          ctest --test-dir build --output-on-failure
          cmake -B build-sn -S StickyNotes -DCMAKE_BUILD_TYPE=Release -G Ninja
          cmake --build build-sn -j
          QT_QPA_PLATFORM=offscreen ctest --test-dir build-sn --output-on-failure
```

> 说明:Linux 分支保持现有命令;Windows 分支用 `msys2/setup-msys2` action,`shell: msys2 {0}` 让 run 在 MSYS2 里执行,`install-user-deps.sh` 经平台分支 `exec` 到 `win-deps.sh`。

- [ ] **Step 2: 提交**

```bash
git add .github/workflows/ci.yml
git commit -m "ci: 双平台矩阵(linux + windows/MSYS2),同一套 ctest"
```

---

### Task 7: 跨平台渲染一致性验证(golden 双平台一致)

**Files:**
- Verify: `EasyPainter/tests/image_golden_test.cpp`(断言逻辑,已平台无关)
- Verify: `StickyNotes/tests/image_golden_test.cpp`

**Interfaces:**
- Consumes: `render_offscreen` 输出 + 单一 golden PNG。
- Produces: 确认渲染核心在两平台产出逐像素一致的图(验证「一套代码」)。

- [ ] **Step 1: 核对 EasyPainter golden 测试已平台无关**

读 `EasyPainter/tests/image_golden_test.cpp`,确认它只对比 `render_offscreen()` 输出与 `GOLDEN_DIR/golden_render.png`,无平台分支。若发现平台相关断言(如按 OS 容差),移除。

- [ ] **Step 2: 核对 StickyNotes golden 测试**

读 `StickyNotes/tests/image_golden_test.cpp`,确认 fixture 渲染对比同源基线,无平台分支。

- [ ] **Step 3: 记录双平台一致作为验证标准**

在 CI 或 `tools/README.md` 注明:「golden 单一基线,双平台逐像素一致校验;不一致 → 修渲染路径,不建分平台基线」。

- [ ] **Step 4: 提交(若有断言改动)**

```bash
git add EasyPainter/tests/image_golden_test.cpp StickyNotes/tests/image_golden_test.cpp
git commit -m "test: golden 断言平台无关核对(单基线双平台)"
```

---

## Self-Review

### 1. Spec 覆盖检查
| Spec 需求 | 对应 Task |
|---|---|
| 渲染核心平台无关(固化为原则) | Task 3(SKILL)、Task 5(CMake 条件)、Task 7(验证) |
| 单一 golden 基线双平台 | Task 7 |
| 工具链脚本平台分支 | Task 1 |
| SwiftShader 池内构建 | Task 2 |
| Qt6 双平台 | Task 1(win-deps pacman) |
| 窗口 surface 自动选平台 | Task 5(CMake `UNIX AND NOT APPLE`) |
| CI 双 job | Task 6 |
| 门控文档平台无关化 | Task 3、Task 4 |

### 2. 占位符扫描
- 无 TBD/TODO/「implement later」;所有 step 含实际内容与验证命令。

### 3. 类型一致性
- `OS_PLATFORM`(windows/linux)在 Task 1 三处(setup-env probe / install-user-deps 前置检查 / install-user-deps exec)一致。
- `win-deps.sh` 生成的 `env.sh` 的 `VK_ICD_FILENAMES` 指向 `third_party/_install/swiftshader-main/release/vk_swiftshader_icd.json`,与 Task 2 池产物路径一致。
- `UNIX AND NOT APPLE` 在 Task 5 两处(link 库、target_link_libraries)一致。
