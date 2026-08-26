# Mine Windows 工具链迁移到 MSVC 设计

日期:2026-08-25
状态:已与用户逐点确认(生成器 / MSVC 来源 / Qt6 预编译)

## 1. 背景与动机

SwiftShader(swiftshader-master/third_party/marl)在 MinGW-w64 下编译报
`scheduler.cpp:64: '__nop' was not declared` —— `__nop` 是 MSVC 内建指令,
MinGW 的 gcc/clang 不识别。这是 SwiftShader 上游对 MinGW 平台的一类已知缺口
(与 deps.yaml 里 abseil-cpp 注释提到的 MinGW 平台缺口同源)。

与其给 `__nop` 打补丁逐坑治标,不如把 Windows 工具链整个切到 **MSVC**:
MSVC 原生支持 `__nop`,SwiftShader/abseil 等上游本就 MSVC 优先。

**本设计修订 `2026-08-24-cross-platform-design.md` 中「Windows 工具链 =
MSYS2 + MinGW-w64 g++」的决策**,替换为「MSVC + MSYS2」。
`2026-08-25-cross-platform.md` 计划中所有引用 MinGW 的 Task 随之更新。

## 2. 核心决策(已与用户逐点确认)

| 决策点 | 结论 |
|---|---|
| C/C++ 编译器 | **MSVC cl.exe**(Ninja + cl) |
| CMake 生成器 | **`-G Ninja` + 注入 cl 的 Ninja**(保留现有 -G Ninja 架构;不切 VS 生成器) |
| MSVC 来源 | **检测已装 + 自动装 Build Tools**(vswhere / VSINSTALLDIR;`Microsoft.VisualStudio.Product.BuildTools`,免管理员) |
| MSYS2 角色 | **仅**:ninja / glslc / vulkan.h / python3;**不再装 MinGW 编译器** |
| Qt6 | **预编译**:aqtinstall 下载 Qt 官方 MSVC 预编译(`win64_msvc2019_64`),独立到 `.user-deps/`(不进池) |
| abseil / SwiftShader / fmt / glm / googletest / glfw / ink | **池内 MSVC 源码编译**(无官方 MSVC 预编译包) |
| deps.yaml | **去掉 `windows_package` 字段**(MSVC 下 pacman 的 MinGW 库不兼容) |
| pool.is_pacman_provided | MSVC 下**恒 False**(不再用 pacman 预编译) |

### 2.1 「预编译 vs 编译」矩阵(已核实现状)

| 库 | 结论 | 依据 |
|---|---|---|
| Qt6 | ✅ **预编译**(aqtinstall,官方 MSVC `msvc2019_64`) | Qt 官方发布 MSVC 预编译;`aqtinstall`(pip,3.3.0)可脚本化下载 |
| abseil-cpp | ⚠️ **池内 MSVC 编译** | NuGet `abseil-cpp`/`Abseil`/`absl` **0 命中**,无官方 MSVC 预编译 |
| SwiftShader | ⚠️ **池内 MSVC 编译** | 无官方 MSVC 预编译;但 MSVC 原生规避 `__nop` 等 MinGW 坑 |
| fmt / glm / googletest / glfw | ⚠️ **池内 MSVC 编译** | 上游原生支持 MSVC,CMake 编译成本低;GLFW NuGet(3.4.0)含 X11 依赖,不用 |
| ink-stroke-modeler | ⚠️ **池内 MSVC 编译** | 依赖池内 MSVC abseil |

## 3. 架构与组件

### 3.1 MSVC 工具链发现与自动安装(`tools/deps_lib/msvc.sh`)

1. **发现**:`vswhere.exe`(默认路径 `C:/Program Files (x86)/Microsoft Visual Studio/Installer/vswhere.exe`)查已装
   VS/Build Tools;或读 `VSINSTALLDIR`/`VCINSTALLDIR` 环境变量。取含 VC 工具链 + Windows SDK 的实例。
2. **自动安装**:未找到 → 下载 `vs_buildtools.exe` 并静默装
   `Microsoft.VisualStudio.Product.BuildTools`,组件含 VC 工具链 + Windows SDK + 可选 Ninja。
   免管理员(用户级安装)。
3. **导出 vcvars**:定位实例的 `VC/Auxiliary/Build/vcvars64.bat`,生成 `.user-deps/vcvars.sh`
   (记录 VS 安装根、vcvars64.bat 路径、ninja 路径)。

### 3.2 环境注入(`win-deps.sh` + `.user-deps/env.sh`)

- `win-deps.sh` 每次执行 `cmake`/`ninja` 前 **source vcvars64.bat 导出的环境**(PATH/INCLUDE/LIB/LIBPATH),
  使 `cl.exe` 与 Windows SDK 头/库可用。
- `env.sh` 生成:写入 `MINE_ROOT` / `USER_DEPS` / SwiftShader ICD 路径 / Qt6 MSVC 前缀 /
  MSVC 池前缀;不再写入 `/mingw64`。

### 3.3 池构建(`pool.py` / `cmake_driver.py`)

- `is_pacman_provided(root, name)` → 恒 `False`(MSVC 下 pacman 的 MinGW 库不兼容)。
- `configure_command`:
  - 保留 `-G Ninja`。
  - **不再注入 `/mingw64` 前缀**。
  - 注入 **MSVC 池前缀**(已 build 的 MSVC 版 abseil/GLFW 等)。
  - 加 `-DCMAKE_MSVC_RUNTIME_LIBRARY=MultiThreadedDLL`(动态 CRT,统一 ABI)。
- `build_lib` 的 `_stream()` 不变(已实时透传)。
- `_src_fingerprint` / `.built` 指纹机制不变。

### 3.4 依赖部署(`win-deps.sh`)

| 项 | MinGW(移除) | MSVC(新增) |
|---|---|---|
| 工具链 | `mingw-w64-x86_64-gcc` 等 | **不装 MinGW 编译器**;装/复用 ninja;msvc.sh 装 Build Tools |
| ①镜像源 | mirrorlist.mingw64 | 保留(仅 ninja 等 msys 包) |
| ③windows_package | `mingw-w64-x86_64-abseil-cpp` | **移除**;abseil 池内 MSVC 编译 |
| ④Vulkan | `mingw-w64-x86_64-vulkan-headers/shaderc/loader` | 改装 **MSVC 兼容** glslc/vulkan.h(MSYS2 或 Vulkan SDK);定位路径相应调整 |
| ⑥Qt6 | `mingw-w64-x86_64-qt6-base` | **aqtinstall 下载 MSVC 预编译** → `.user-deps/` |
| env.sh | `/mingw64` 前缀 | MSVC 前缀 + Qt6 前缀 + SwiftShader ICD |

### 3.5 探测(`setup-env.sh`)

- `probe()` Windows 分支:探测 **MSVC**(`cl` 可经 vcvars 定位 + `ninja` + `python3` + glslc/vulkan.h),
  不再探测 `g++`/`pkg-config` 为硬依赖。
- 缺失 → 走自动安装(msvc.sh)→ 重探 → 硬缺报错。与「不回退原则」一致:不做 MinGW 回退。

### 3.6 项目 CMakeLists(不改或微调)

- `EasyPainter/CMakeLists.txt` 的 X11 pkg-config 补链接已 `if(UNIX)` 限定,**Windows(MSVC)不受影响**。
- `CMAKE_PREFIX_PATH` 由 env.sh / configure_command 注入,项目侧不动。

## 4. 数据流

```
setup-env.sh(Windows)
  └─ install-user-deps.sh → win-deps.sh
       ├─ msvc.sh: vswhere 检测 / 自动装 Build Tools → .user-deps/vcvars.sh
       ├─ pacman(仅 ninja/glslc/vulkan.h/python3)
       ├─ aqtinstall: 下载 Qt6 MSVC 预编译 → .user-deps/qt/
       └─ 生成 .user-deps/env.sh(MSVC 前缀 + Qt6 前缀 + SwiftShader ICD)
  └─ fetch-deps.py --all(池)
  └─ build-deps.py --all: abseil/SwiftShader/fmt/glm/googletest/glfw/ink 池内 MSVC 编译
       └─ cmake_driver: source vcvars → -G Ninja + cl → 池前缀 + MSVC_RUNTIME_LIBRARY
```

## 5. 错误处理

- **无 VS/Build Tools 且自动安装失败**:明确报错,提示手动装 Build Tools 或设 `VSINSTALLDIR`。不做 MinGW 回退。
- **vswhere 定位不到 VC 工具链**:报错并列出可用实例。
- **aqtinstall 下载失败**:报错,提示手动用 aqtinstall 或 Qt 在线安装器。
- 池编译失败:沿用现有 `build_lib` 失败日志(尾部 60 行)。

## 6. 测试策略

- 新增单测:
  - `vcvars.sh` 生成(msvc.sh 纯函数:实例路径 → vcvars 路径 → env.sh 内容)。
  - `probe()` MSVC 分支。
  - `configure_command` MSVC 注入(池前缀 + `MSVC_RUNTIME_LIBRARY`,不注入 `/mingw64`)。
  - `is_pacman_provided` 在 MSVC 下恒 False。
- SwiftShader / abseil / GLFW / Qt6 的 MSVC 构建:本机 Windows 验证(不在单测里编译)。
- 门控:Windows 双项目无头构建 + 离屏渲染 PNG + 全测试绿,单一 golden 基线双平台一致。

## 7. 验收标准

- Windows(MSVC)下 `setup-env.sh` 一键:MSVC 就绪(自动装 Build Tools)→ 池内 MSVC 编译 SwiftShader/abseil →
  离屏渲染 PNG 与 Linux lavapipe 一致(golden 基线)。
- SwiftShader 在 MSVC 下编译通过(`__nop` 不再报错)。
- 双项目(EasyPainter + StickyNotes)无头构建 + 全测试绿。
- 现有 Linux 路径不回退、全测试保持绿。

## 8. 不做的事(YAGNI)

- **不切 `-G "Visual Studio"` 生成器**;保留 `-G Ninja`。
- **不做 MinGW 回退路径**。
- **不为 SwiftShader/abseil 造预编译包**(无官方渠道,池内编译成本更低)。
- **不建第二份 golden 基线**(沿用单一基线)。
- **CI 的 Windows job 本轮不做**(后续轮次;本轮到本机 Windows 验证通过为止)。
