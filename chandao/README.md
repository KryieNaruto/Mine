# 禅道

最小可运行的 Qt 桌面窗口项目（CMake + MSVC）。根工程名为 `chandao`，可执行目标为 `main`。

本仓库自 2026-08 起作为 Mine 工作空间的子项目维护（来源：独立仓库 KryieNaruto/chandao，已归档）。

## 目录

- `main/`：源码与子工程
- `plan/`：计划文档
- `_build/`：CMake 构建目录（不入库）
- `_install/`：安装输出，验收目标为 `_install/main.exe`（不入库）

## 构建（Windows PowerShell）

将 `<Qt路径>` 换成本机 Qt 套件目录（例如 `D:\Qt\6.8.3\msvc2022_64`）：

```powershell
cmake -S . -B _build -G "Visual Studio 18 2026" -A x64 `
  -DCMAKE_INSTALL_PREFIX="_install" `
  -DCMAKE_PREFIX_PATH="<Qt路径>"

cmake --build _build --config Release
cmake --install _build --config Release
```

安装阶段会调用 `windeployqt`，把 Qt DLL 拷到 `_install`。

用 Visual Studio 打开 `_build/chandao.sln`，或直接打开根目录 `CMakeLists.txt`。
