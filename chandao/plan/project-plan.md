# 禅道桌面项目落地计划

## 1. 目标

在仓库根目录 `D:\qsw\禅道` 搭建可多人共享的 Qt 桌面工程：

- 产品名：**禅道**
- 解决方案 / 根 CMake 工程名：`main`
- 可执行文件：`main.exe`
- 技术栈：C++20 + Qt6 Widgets；Vulkan 为可选依赖（有 SDK 则启用，没有也不失败）
- Visual Studio 可通过根 `CMakeLists.txt` 或生成的 `main.sln` 打开
- 编译后把 `main.exe` 及 Qt 运行依赖安装到 `_install/main.exe`
- 初始化 git 并推送到远端，方便协作

本仓库检查时工作区为空（无既有源码、无 `.cursor` 文件）。后续若并行 Agent 写入 `.cursor/skills`、`.cursor/agents` 等，将一并纳入版本库，**不覆盖、不整体忽略 `.cursor`**。

## 2. 目标目录结构

```
D:\qsw\禅道\                 # 仓库根
  main\                      # 子项目（解决方案工程）
    src\                     # C++ 源码
    resource\                # 图片、spv 等占位资源
    CMakeLists.txt
  plan\                      # 计划文档
    project-plan.md
  CMakeLists.txt             # 根 CMake：project(main) + add_subdirectory(main)
  README.md                  # 构建说明与 clone 地址
  .gitignore
  _build\                    # CMake 构建目录（gitignore）
  _install\                  # 安装输出（gitignore）
```

可执行目标名为 `main`，安装规则使用 `RUNTIME DESTINATION .`，使测试能在 `_install/main.exe` 找到文件（而不是 `_install/bin/main.exe`）。

## 3. CMake 方案

### 3.1 根 `CMakeLists.txt`

- `cmake_minimum_required(VERSION 3.21)`
- `project(main VERSION 0.1.0 LANGUAGES CXX)` —— 使用 Visual Studio 生成器时产出 `_build/main.sln`
- `add_subdirectory(main)`

### 3.2 `main/CMakeLists.txt`

- C++20，`CMAKE_AUTOMOC` / `CMAKE_AUTORCC` 开启
- `find_package(Qt6 REQUIRED COMPONENTS Widgets)`
- `add_executable(main WIN32 ...)`
- `find_package(Vulkan QUIET)`：找到则 `HAS_VULKAN` + 链接 `Vulkan::Vulkan`
- `install(TARGETS main RUNTIME DESTINATION .)`
- Windows 下在 install 阶段自动调用 `windeployqt`，把 Qt DLL 部署到 `_install`

### 3.3 生成器

本机未装 Visual Studio 2022，已装 **Visual Studio Community 2026 Insiders**（MSVC 14.51）+ CMake 4.4。默认生成器：

```
-G "Visual Studio 18 2026" -A x64
```

这与「优先 MSVC」一致；ABI 使用 Qt 的 `msvc2022_64` 套件即可。

## 4. Qt / Vulkan

### 4.1 Qt

检查结果：`C:\Qt` 不存在，需自动安装，不能停在“请用户自行安装”。

计划：

1. `python -m pip install aqtinstall`
2. 安装 Qt 6（MSVC 64 位桌面套件）到仓库外路径，例如 `D:\Qt`（避免提交超大 SDK）
3. 配置时传入 `-DCMAKE_PREFIX_PATH=<Qt msvc*_64 目录>`
4. 安装后对 `_install/main.exe` 运行 `windeployqt`

探测顺序：`C:\Qt\6.*\msvc*_64` → `D:\Qt\6.*\msvc*_64` → aqtinstall 安装。

### 4.2 Vulkan

检查结果：未设置 `VULKAN_SDK`，无 `C:\VulkanSDK`。CMake 使用 `find_package(Vulkan QUIET)`，找不到则跳过，源码用 `#ifdef HAS_VULKAN` 分支，保证编译通过。

## 5. 应用范围

最小可运行 Qt 窗口：标题含「禅道」，启动即可。不做禅道业务功能。

## 6. 构建 / 安装命令（Windows PowerShell）

在仓库根执行（`<Qt路径>` 为实际探测或安装后的 `msvc*_64` 目录）：

```powershell
cmake -S . -B _build -G "Visual Studio 18 2026" -A x64 `
  -DCMAKE_INSTALL_PREFIX="_install" `
  -DCMAKE_PREFIX_PATH="<Qt路径>"

cmake --build _build --config Release
cmake --install _build --config Release
```

`cmake --install` 会把 `main.exe` 装到 `_install/main.exe`，并调用 `windeployqt` 拷贝 Qt 依赖。

如需手动部署：

```powershell
& "<Qt路径>\bin\windeployqt.exe" --release "_install\main.exe"
```

验收：`Test-Path _install\main.exe` 为 true，且同目录存在 Qt DLL（如 `Qt6Core.dll` / `Qt6Widgets.dll`）。

## 7. Git 与远端

用户已明确要求把当前目录变成 git 仓库并推送，视为允许 `git init` + 首次 commit + push。

约束：

- **禁止**修改 `git config user.*`
- **禁止** force push、`--no-verify`、破坏性 reset
- 不提交密钥、`.env`、Qt/Vulkan SDK、`_build/`、`_install/`
- `.gitignore` 忽略构建产物与常见 IDE 用户文件；**不**忽略整个 `.cursor`（仅可忽略 `.cursor/*.log`）
- 已存在的 `.cursor/skills`、`.cursor/agents` 一并 `git add`，不覆盖

远端策略：

1. 按 Cursor `new-repo` / `origin` 技能：优先 Cursor 托管仓库（`origin.cursor.com`）。
2. **限制**：origin CLI **不支持原生 Windows**（非 WSL）。本机无 WSL、无 origin。不能在 Windows 上安装 origin CLI。
3. 回退：使用 GitHub（本机附近仓库已使用 `https://github.com/KryieNaruto/krita.git`）。若 `gh` 可用则 `gh repo create`；否则尝试安装 GitHub CLI 或使用已有 git credential。
4. 推送成功后把 clone URL 写入本文件与 `README.md`。

首次提交信息示例：`Initialize 禅道 Qt desktop project with CMake.`

## 8. 风险

| 风险 | 应对 |
|------|------|
| Origin CLI 在原生 Windows 不可用 | 改用 GitHub 远端；计划中记录原因与 clone 地址 |
| 未装 VS 2022，只有 VS 2026 Insiders | 使用 CMake 生成器 `Visual Studio 18 2026` + Qt `msvc2022_64` |
| VS 安装 `isComplete=false` | 已确认存在 `cl.exe`（MSVC 14.51）；若链接失败再补组件 |
| 本机无 Qt | `aqtinstall` 自动安装到 `D:\Qt` |
| 无 Vulkan SDK | 可选依赖，不启用 `HAS_VULKAN` |
| 全局未配置 git user.name / email | **不改 git config**；提交时仅用进程环境变量 `GIT_AUTHOR_*` / `GIT_COMMITTER_*` |
| 并行 Agent 写 `.cursor` | 不覆盖；提交前再 `git add` |
| `windeployqt` 漏 DLL | 安装规则自动调用；验收检查 `_install` 下 Qt DLL |

## 9. 验收清单

- [ ] `plan/project-plan.md` 已保存
- [ ] 目录结构符合约定
- [ ] 根 CMake `project(main)` + `add_subdirectory(main)`，VS 可识别（`_build/main.sln`）
- [ ] Qt6 Widgets 工程，最小窗口标题含「禅道」
- [ ] Vulkan 为 QUIET 可选
- [ ] Release 编译成功
- [ ] `_install/main.exe` 存在，且带 Qt 运行依赖（非占位假 exe）
- [ ] `.gitignore` 含 `_build/`、`_install/`
- [ ] git 仓库已初始化并推送到远端
- [ ] clone 地址已写入计划或 README

## 10. 执行顺序

1. 写入本计划（当前步骤）
2. 添加 `.gitignore`、CMake、源码、资源占位、README
3. 探测 / 安装 Qt6 MSVC 套件
4. CMake 配置 → Release 编译 → 安装 + windeployqt
5. 确认 `_install/main.exe` 及 Qt DLL
6. `git init -b main`、提交、创建远端并 push
7. 回写 clone 地址，汇总验收结果
