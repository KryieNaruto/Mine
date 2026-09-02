# 禅道项目交付结果

## 日期

2026-08-18

## 目标

1. 在 `D:\qsw\禅道` 搭建可多人共享的 CMake + Qt 桌面工程。
2. 解决方案名 `main`，可执行文件 `main.exe`，安装到 `_install/main.exe`。
3. 技术栈：C++20 + Qt6 Widgets；Vulkan 可选（`find_package(Vulkan QUIET)`）。
4. 初始化为 git 仓库，并推送远端，支持多人协作。
5. 同步沉淀软件总监 Agent + 交付流程 Skill。

## 已完成工作

### 1. 工程搭建

- 根 `CMakeLists.txt`：`project(main)` + `add_subdirectory(main)`，Visual Studio 可识别为 `main.sln`。
- `main/CMakeLists.txt`：C++20、AUTOMOC、Qt6 Widgets、可选 Vulkan、安装规则 + `windeployqt` 自动部署。
- 最小可运行源码 `main/src/main.cpp`：标题为「禅道」的 Qt 窗口，Vulkan 状态通过标签显示。
- 资源目录 `main/resource/` 已放置占位说明。

### 2. 构建与安装

```powershell
cmake -S . -B _build -G "Visual Studio 18 2026" -A x64 `
  -DCMAKE_INSTALL_PREFIX="_install" `
  -DCMAKE_PREFIX_PATH="D:\Qt\6.8.3\msvc2022_64"

cmake --build _build --config Release
cmake --install _build --config Release
```

- 编译成功：`_build\main\Release\main.exe`。
- 安装成功：`_install\main.exe` 已存在，并附带 Qt 运行依赖（`Qt6Core.dll`、`Qt6Widgets.dll`、`Qt6Gui.dll`、`platforms/`、`styles/` 等）。

### 3. Git 仓库与远端

- 本地仓库已初始化，分支 `main`。
- 提交 1：`Initialize 禅道 Qt desktop project with CMake.`（16 个文件）。
- 提交 2：`Update .gitignore to ignore temporary GitHub token file.`。
- 远端：`https://github.com/KryieNaruto/chandao.git`。
- 已推送 `main` 分支到 GitHub，工作区干净。

### 4. 软件总监 Agent + Skill

- Skill：`software-director-delivery`，位于 `.cursor/skills/software-director-delivery/SKILL.md`。
- Agent：`software-director`（需求解析与派工），以及 `delivery-implementer`、`delivery-tester`、`delivery-triage`。
- 启用说明：`plan/director-workflow.md`。
- 聊天输入 `/software-director-delivery` 即可触发四步交付流程：计划 → 执行 → 测试 → 失败排查/再执行。

## 测试结果

| 检查项 | 结果 |
| --- | --- |
| 目录结构符合约定 | 通过 |
| 根 CMake 可识别为 `main` 解决方案 | 通过 |
| Qt6 Widgets 最小窗口标题含「禅道」 | 通过 |
| Vulkan 可选，无 SDK 时仍编译通过 | 通过 |
| Release 编译成功 | 通过 |
| `_install/main.exe` 存在且带 Qt 依赖 | 通过 |
| `.gitignore` 包含 `_build/`、`_install/`、`.github_token` | 通过 |
| git 仓库已初始化并推送到 GitHub | 通过 |
| clone 地址已写入计划 | 见下方 |

## 验收结论

**通过。** `_install\main.exe` 已生成，可直接运行（依赖 Qt DLL 已部署）。

## 克隆地址

```bash
git clone https://github.com/KryieNaruto/chandao.git
```

## 安全提醒

本次推送曾使用一次性的 GitHub Personal Access Token，且该 Token 在聊天记录中出现过。建议：

1. 立即前往 GitHub Settings → Developer settings → Personal access tokens → 删除/撤销该 Token。
2. 重新生成新的 Token（classic，勾选 `repo` 权限）。
3. 在本地使用 Git Credential Manager 或 HTTPS 登录，避免把 Token 留在 remote URL 中。

本地 remote 已经改回普通 URL：

```bash
git remote -v
# origin  https://github.com/KryieNaruto/chandao.git (fetch)
# origin  https://github.com/KryieNaruto/chandao.git (push)
```

## 专注计时器 Demo 迭代（2026-08-18）

### 本次交付内容

- 在 `main/src/` 新增 `focus_timer_widget.h` / `focus_timer_widget.cpp`：自定义 100×100 专注计时控件。
- 重写 `main/src/main.cpp`：使用 `FocusTimerWidget` 作为中央控件，窗口标题为「专注」，固定尺寸 100×100，保留系统标题栏（含关闭按钮）。
- 更新 `main/CMakeLists.txt`：将新源文件加入 `main` 可执行目标。

### 界面与行为

- 仅保留圆形环绕刻度条与蓝色主按钮；标题文字、中间装饰图案、右侧省略号按钮均已去除。
- 背景色 `#2B2B2B`，激活刻度 `#55B2E8`，未激活刻度 `#3D3D3D`，暂停图标黑色。
- 36 个圆角矩形刻度自 3 点钟方向起，顺时针随进度变蓝。
- 默认工作时长 10 秒，工作结束后圆环全部变灰并进入 3 秒休息；休息结束后自动开始下一轮工作。
- 蓝色主按钮可暂停 / 继续计时；暂停时按钮显示黑色播放三角形，运行时显示黑色暂停双竖线。

### 构建与验证

```powershell
cmake --build _build --config Release
cmake --install _build --config Release
```

- 编译成功，生成 `_build/main/Release/main.exe`。
- 安装成功，`_install/main.exe` 已更新，Qt 运行依赖已自动部署。
- 运行验证：程序正常启动并持续运行，无崩溃，仅 Qt 样式相关 libpng 警告（不影响功能）。

### 尺寸调整（2026-08-18 迭代）

- 将窗口大小从固定 100×100 改为占主屏幕面积的 30%，以正方形窗口呈现。
- 窗口尺寸计算：`sqrt(screen_width * screen_height * 0.3)`。
- `FocusTimerWidget` 改为自适应绘制：所有刻度、按钮、图标均按当前控件宽高比例缩放，不再依赖固定 100×100 坐标。
- 构建与运行验证通过，`_install/main.exe` 已更新。

### 视觉与按钮优化（2026-08-18 迭代）

- 按钮半径从 `base * 0.14` 调整为 `base * 0.126`，使按钮面积约占窗口面积的 5%。
- 圆环进度由离散点亮改为平滑渐变：每个刻度内部根据覆盖比例从 `#55B2E8` 渐变到 `#3D3D3D`，进度前沿在每个方块中可见颜色走动。
- 按钮背景增加径向渐变：中心 `#77C8F8` → 边缘 `#55B2E8`。
- 暂停 / 播放图标增加线性渐变，减弱状态切换的僵硬感。
- 使用 Qt `QPainter` 的渐变能力实现，无需引入 Vulkan；后续如需在圆环中心做 Vulkan 特效时再扩展。
- 构建与运行验证通过，`_install/main.exe` 已更新。

### 圆环方向与卡顿修复（2026-08-18 迭代）

- 修正刻度渐变方向：保持方块朝向不变（长轴仍指向圆心），但将渐变从沿局部 y 轴改为沿局部 x 轴（即圆环顺时针切线方向），使颜色在方块内部沿圆形进度条路径从后侧向前侧推进。
- 提升刷新率：定时器从 100ms 缩短到 16ms（约 60fps），`m_elapsed` 增量同步改为 0.016s，解决小方格内颜色移动卡顿问题。
- 构建与运行验证通过，`_install/main.exe` 已更新。

## 当前整体状态

### 功能

- 窗口占主屏幕面积 30%，正方形，保留系统标题栏（含关闭按钮）。
- 仅保留圆环刻度条与蓝色暂停/继续按钮；标题文字、中间图案、省略号按钮已去除。
- 默认工作 10 秒，圆环从 3 点钟方向起顺时针随进度变蓝；工作结束全灰并休息 3 秒，随后自动开始下一轮。
- 按钮点击可暂停 / 继续。

### 视觉

- 背景 `#2B2B2B`；激活刻度 `#55B2E8`；未激活刻度 `#3D3D3D`。
- 按钮背景为径向渐变（中心 `#77C8F8` → 边缘 `#55B2E8`），图标为线性渐变。
- 圆环每个刻度内部沿顺时针切线方向平滑渐变，颜色前沿可见移动。
- 按钮面积约占窗口面积 5%。
- 刷新率约 60fps，动画流畅。

### 验证

- Release 编译成功。
- `_install/main.exe` 已更新，Qt 运行依赖已部署。
- 启动运行 3 秒无崩溃，无异常退出。

## 后续建议

1. 下载安装 Qt 6.8.3 MSVC 2022_64 到其他机器时，可调整 `-DCMAKE_PREFIX_PATH` 指向实际路径。
2. 业务功能开发在 `main/src/` 中展开；运行时资源放入 `main/resource/`。
3. 需要 Vulkan 时安装 Vulkan SDK，CMake 会自动启用 `HAS_VULKAN`。
4. 使用 `/software-director-delivery` Skill 管理后续新需求。

## 命令行图像输出接口迭代（2026-08-18）

### 需求

增强当前架构，软件添加图像输出接口，能够通过命令行输出对应时间的软件图像，方便快速知晓结果。例如 `main -t 1` 输出第 1 秒的图像，再与 `main -t 1.5` 的第 1.5 秒图像比较，判断是否符合要求。

### 实际改动摘要

- `main/src/focus_timer_widget.h/.cpp`：新增公共接口 `setTime(double seconds)`。停止内部 QTimer，置运行态，按「工作 10s + 休息 3s」循环用 `fmod(seconds, 13.0)` 确定性算出 `m_state` / `m_elapsed`，无需真实等待。
- `main/src/main.cpp`：新增命令行参数 `-t <秒数>`（支持小数）与 `-s <边长>`（默认 400）。提供 `-t` 时不弹窗：创建控件 → `resize(size,size)` → `setTime(t)` → `grab()` → 保存 `shot_<t参数原文>.png` 到当前工作目录，退出码 0。无 `-t` 时交互窗口行为完全不变。
- 高 DPI 修复：`grab()` 结果按 `devicePixelRatio` 归一化（尺寸不符时 `scaled` 回 `-s` 指定值），保证任意缩放机器上输出尺寸严格等于 `-s`。

### 用法示例

```powershell
cd D:\qsw\禅道\_shottest
D:\qsw\禅道\_install\main.exe -t 1      # 生成 shot_1.png（400×400，第 1 秒画面）
D:\qsw\禅道\_install\main.exe -t 1.5    # 生成 shot_1.5.png（第 1.5 秒画面）
D:\qsw\禅道\_install\main.exe -t 1 -s 200  # 自定义尺寸 200×200
```

肉眼对比 `shot_1.png` 与 `shot_1.5.png` 即可确认进度推进是否符合要求。

### 结果

- 验收：**通过**（测试轮次 2 / 5）。
- round 1 FAIL：高 DPI（DPR=2）下 PNG 尺寸翻倍（800×800 而非 400×400）；triage 定位为代码缺陷（`grab()` 未按 DPR 归一化），最小修复后 round 2 PASS。
- 产物：`_install/main.exe` 已更新，Qt 依赖已部署。

### 测试结论（round 2，全部 PASS）

| 验收项 | 结果 |
| --- | --- |
| 构建 / 安装 Release 成功，`_install/main.exe` 更新 | 通过 |
| `-t 1` 生成 `shot_1.png`，严格 400×400，退出码 0，不弹窗 | 通过 |
| `-t 1.5` 生成 `shot_1.5.png`，严格 400×400 | 通过 |
| 蓝色像素对比：1.5s（9832）> 1s（9061），进度趋势正确 | 通过 |
| `-t 1 -s 200` 输出严格 200×200 | 通过 |
| 无参数运行弹窗正常，不回归 | 通过 |

### 证据

- 计划：`plan/delivery-plan.md`
- 执行记录：`plan/execution-log.md`
- 测试报告：`plan/test-report.md`（round 1 FAIL → round 2 PASS）
- 排查记录：`plan/triage-log.md`

### 残留风险

- 输出文件名仅含 `-t` 参数原文，同 `-t` 不同 `-s` 会互相覆盖，使用时注意顺序。
- 安装时 windeployqt 有无害警告（translations、dxcompiler.dll 缺失），不影响运行。

## 界面迭代：按钮、刻度、自适应与无边框（2026-08-18）

### 需求

1. 暂停/继续按钮缩小为约占软件的 10%（直径 ≈ 窗口边长 10%）。
2. 按钮增加交互动画与图案渐变（悬停提亮、按压缩放、图标过渡）。
3. 表刻度样式参数抽出，默认调细至舒适观感。
4. 布局自适应：缩放窗口时表盘/按钮/边距等比不变，窗口最小 200×200。
5. 去掉系统标题栏，整体与表盘融为一体，保留左上角关闭按钮，简约扁平风格。

### 实际改动摘要

- `main/src/focus_timer_widget.h/.cpp`：
  - 按钮半径 `base*0.126` → `base*0.05`（直径实测恰为边长 10.00%），图标按按钮半径等比缩放。
  - 复用 60fps QTimer 驱动插值动画：悬停提亮（约 120ms）、按压 0.92 倍缩放回弹、暂停↔播放图标 150ms 交叉淡化；图标保留线性渐变；新增 `setMouseTracking`。
  - 新增 `TickStyle` 结构体集中管理刻度参数（数量 36、宽度比 0.026、高度比、圆角比、环半径比 0.28、前沿渐变带宽 0.03），带中文注释与调节建议；默认刻度由 0.04 调细为 0.026 呈细胶囊形。
- `main/src/frameless_window.h/.cpp`（新增）：`Qt::FramelessWindowHint` 无边框窗口，背景 `#2B2B2B` 与表盘一致；左上角扁平 × 关闭按钮（悬停提亮）；空白区域拖拽移动；6px 热区 8 方向缩放（纯 Qt 实现）；最小 200×200；拖拽与程序性 resize 均按短边回正保持正方形；启动居中，初始面积为屏幕 30%。
- `main/src/main.cpp`：改用 `FramelessWindow`；`-t` / `-s` 截图模式行为不变。

### 结果

- 验收：**通过**（测试轮次 1 / 5，一次通过）。
- 产物：`_install/main.exe` 已更新（Release，Qt 依赖已部署）。

### 测试结论（round 1，全部 PASS）

| 验收项 | 结果 |
| --- | --- |
| 构建 / 安装 Release 成功，`_install/main.exe` 更新 | 通过 |
| 按钮直径实测 40px/400px = 10.00%（8%~12% 区间，远小于旧版 25%） | 通过 |
| `TickStyle` 集中头文件、宽度比 0.026，截图刻度明显变细 | 通过 |
| `-t 1` / `-t 1.5` 严格 400×400，退出码 0，不弹窗；1.5s 蓝色像素 1711 > 1s 的 1146 | 通过 |
| `-t 1 -s 200` 输出严格 200×200 | 通过 |
| 最小 200×200、正方形回正、无边框、左上角关闭按钮、边缘缩放热区均实现 | 通过 |
| 无参启动 5 秒不崩溃 | 通过 |

### 证据

- 计划：`plan/delivery-plan.md`
- 执行记录：`plan/execution-log.md`
- 测试报告：`plan/test-report.md`（round 1 PASS）

### 残留风险

- 边缘缩放手感为纯 Qt 实现，未走 Windows 原生 NCHITTEST，极端快速拖拽可能有轻微迟滞；如介意可后续换原生方案。
- 悬停/按压动画在截图模式（`-t`）下不可见，仅交互模式生效，属预期。

## 扁平化、托盘化、中心数字与休息提醒迭代（2026-08-18）

### 需求

1. 按钮样式颜色扁平化，去掉中心白色（径向）渐变。
2. 关闭按钮移到右上角；点击不退出，而是最小化到系统托盘；真正退出需右键托盘图标点「退出」。
3. 圆环中心加内容：本期实现数字剩余时间；植物生长模式先不做，但留模式切换扩展余地。
4. 计时结束后进入 10s 休息计时，刻度颜色表达反转（工作 灰→蓝，休息 蓝→灰）；计时到后弹出窗口。

### 实际改动摘要

- `main/src/focus_timer_widget.h/.cpp`：
  - 按钮扁平化：纯色 `#55B2E8`，图标纯色 `#101010`；悬停提亮、按压缩放、图标交叉淡化动画保留。
  - 关闭按钮从左上角移到右上角。
  - 新增 `enum class CenterMode { TimeText, Plant }` 与 `setCenterMode`/`centerMode`/`drawCenterContent` 分发；`Plant` 分支留空注释预留。`TimeText`：圆环中心显示向上取整剩余秒数（10→1），工作阶段 `#E8E8E8`、休息阶段 `#55B2E8`，字号 0.12×短边。
  - 休息计时 10s：从全蓝开始顺时针逐刻度变灰，刻度内切向前沿渐变方向反转；休息结束自动进入下一轮工作；周期常量集中，`setTime` 按 `fmod(seconds, 20.0)` 确定性渲染。
  - 新增 `phaseChanged` 信号供窗口层感知阶段切换。
- `main/src/frameless_window.h/.cpp`：
  - 系统托盘（`QSystemTrayIcon`）：点击右上角 × `hide()` 到托盘；右键菜单「退出」→ `QApplication::quit()`；左键/双击恢复主窗口；截图模式不建托盘。
  - 全屏休息提醒 `RestOverlay`（内部小类）：`FramelessWindowHint | WindowStaysOnTopHint | Tool`，半透明 `rgba(0,0,0,180)` 覆盖主屏，白色「休息一下」+ 剩余秒数倒计时，休息结束自动关闭；截图模式不触发。
- `main/CMakeLists.txt`：无改动（未新增源文件）。

### 结果

- 验收：**通过**（测试轮次 1 / 5，一次通过）。
- 产物：`_install/main.exe` 已更新（Release，Qt 依赖已部署）。

### 测试结论（round 1，全部 PASS）

| 验收项 | 结果 |
| --- | --- |
| 构建 / 安装 Release 成功，`_install/main.exe` 为最新 | 通过 |
| `-t 3`：400×400、退出码 0；中心亮色「7」（#E8E8E8）；按钮纯色无渐变 | 通过 |
| `-t 13`：休息阶段中心蓝色「7」；3 点钟起前段刻度已变灰；蓝色像素 6748 > 工作 3s 的 2935 | 通过 |
| `-t 1 -s 200` 严格 200×200 不回归 | 通过 |
| 无参运行 15 秒跨工作→休息边界不崩溃 | 通过 |
| 12 秒处全屏截屏实测「休息一下」遮罩自动弹出 | 通过 |
| 代码审查：休息反向、hide() 托盘、「退出」菜单、20s 周期、截图模式无托盘/遮罩 | 通过 |

### 人工验证项（自动测试未覆盖，代码路径已确认）

- 点击右上角 × → 隐藏到托盘。
- 托盘右键「退出」真正关闭；左键/双击恢复主窗口。
- 休息结束遮罩自动关闭。

### 证据

- 计划：`plan/delivery-plan.md`
- 执行记录：`plan/execution-log.md`
- 测试报告：`plan/test-report.md`（round 1 PASS）

### 残留风险

- 全屏提醒仅覆盖主屏，多显示器不覆盖副屏。
- 托盘图标使用系统标准图标回退，未引入自定义 ico。
- 植物生长模式仅留扩展位（`CenterMode::Plant`），后续实现时无需改动模式切换架构。

### round 2 修复：事件传播断点（2026-08-18）

**用户实测反馈**：窗口拖动不了；点右上角 × 不能最小化到托盘，窗口仍显示在前面。

**根因**（见 `plan/triage-log.md` round 2）：`FocusTimerWidget` 作为中央控件铺满整个客户区，其鼠标事件函数对未命中播放按钮的点击既不处理也不 `event->ignore()`，事件不向父窗口传播，`FramelessWindow` 的拖拽 / 关闭 / 边缘缩放处理器收不到事件（同根失效）。`hide()` 代码本身存在但永远收不到点击。

**最小修复**（仅 `main/src/focus_timer_widget.cpp` 三个函数）：

- `mousePressEvent`：未命中播放按钮（含右上角 ×）时 `event->ignore()`。
- `mouseReleaseEvent`：`m_buttonPressed` 为假时 `event->ignore()`。
- `mouseMoveEvent`：不在播放按钮/关闭按钮上时 `event->ignore()`（同时救活边缘缩放光标）。

**测试结论（round 2，PASS）**：构建/安装退出码 0、产物哈希确认更新；截图回归 8/8 与 round 1 数据一致；PostMessage 直投消息自动化实测：拖动后 `GetWindowRect` 位置显著变化、点 × 后 `IsWindowVisible=False` 且进程托盘驻留；代码审查确认 ignore() 不影响播放按钮路径。

**人工验证项**：真实鼠标拖拽手感（自动化桩在 DPR=2 下存在坐标换算伪差，不影响结论）。

### round 3 变更：去掉全屏蒙版（2026-08-19）

**需求**：用户反馈「不需要整个屏幕的蒙版」，确认改为：不新增任何窗口；工作→休息切换时，若主窗口在托盘中则恢复显示，并闪烁任务栏提醒。

**改动**（`main/src/frameless_window.h/.cpp`）：

- `RestOverlay` 全屏半透明蒙版类整体删除（创建、显示、倒计时、关闭全部代码）。
- `setupRestAlert()` 挂在 `phaseChanged` 信号上：工作→休息时，若窗口隐藏则 `showNormal()` + `activateWindow()` 恢复显示，并无条件 `QApplication::alert(this)` 触发任务栏闪烁。

**测试结论（round 3，PASS）**：构建/安装退出码 0；隐藏状态下跨过工作→休息边界，主窗口自动恢复可见且圆环已切换为休息蓝色；7 张全屏截图逐帧统计确认全程无黑色蒙版；截图回归 8/8 与前两轮一致。

**人工验证项**：任务栏闪烁视觉效果（瞬态无法自动捕获）；托盘右键「退出」。

## 按钮下移、白色图标、「...」菜单与设置弹窗迭代（2026-08-19）

### 需求

1. 继续/暂停按钮下移一定距离（原来贴着圆盘），图案改成白色。
2. 按钮旁边增加「...」按钮，点击弹出下拉框，内含「设置」功能按钮；设置弹出无边框窗口，可设置专注时间与休息时间，UI 沿用当前风格，右上角关闭按钮，中间偏下确定按钮。

### 对齐结论

- 时长最小单位秒，设置窗口以 HH:MM:SS 显示与输入（QTimeEdit）；圆环中心剩余时间同步改为 HH:MM:SS。
- 设置经 QSettings("Chandao","FocusTimer") 持久化，交互模式启动时加载；截图模式（-t）不加载，始终默认 10s/10s 保证回归确定性。
- 确定应用规则：新时长 > 已用 → 续跑当前阶段；已用 ≥ 新时长（时间缩短）→ 立即进入下一阶段。
- 下拉框用 QMenu + 深色样式表。

### 实际改动摘要

- `main/src/focus_timer_widget.h/.cpp`：按钮中心 y 0.78h → 0.86h；暂停/播放图标 #101010 → #FFFFFF；新增「...」按钮（x ≈ w*0.5+base*0.11、半径 base*0.04、三个白色圆点、悬停提亮、未命中事件 ignore() 不回归）；深色 QMenu 含「设置」；setDurations 按对齐规则应用时长（截图模式不发 phaseChanged）；中心时间 HH:MM:SS（字号 base*0.075）。
- `main/src/settings_dialog.h/.cpp`（新增）：无边框深色 QDialog，右上角自绘 ×（取消不保存），两行 HH:mm:ss QTimeEdit，中间偏下扁平蓝色「确定」，标题区可拖拽，弹出居中于父窗口。
- `main/src/frameless_window.h/.cpp`：新增 timerWidget() 访问器；拖拽判定排除 dots 热区。
- `main/src/main.cpp`：交互模式启动加载 QSettings 并 setDurations；截图模式跳过。
- `main/CMakeLists.txt`：加入新源文件。

### 结果

- 验收：**通过**（测试轮次 2 / 5）。
- round 1 FAIL：「...」圆点为灰色 (122,122,122)——实现用 inactiveColor.lighter(200)，上限约 (195,195,195) 到不了白色；triage 定位后最小修复（圆点改纯白 #FFFFFF），round 2 PASS。
- 产物：`_install/main.exe` 已更新（Release，Qt 依赖已部署）。

### 测试结论（round 2，全部 PASS）

| 验收项 | 结果 |
| --- | --- |
| 构建 / 安装 Release 退出码 0，`_install/main.exe` 最新 | 通过 |
| 截图回归 `-t 1` / `-t 1.5` / `-t 13` 严格 400×400、退出码 0；蓝色像素 1.5s(1436) > 1s(970)；`-t 13` 休息阶段 | 通过 |
| 按钮下移：圆心 y/边长 = 0.839（≥ 0.83） | 通过 |
| 暂停图标白色（近白像素 108 个） | 通过 |
| 「...」三个圆点纯白 (255,255,255) | 通过（round 2 修复后） |
| 中心时间 HH:MM:SS（`-t 3` 显示 00:00:07） | 通过 |
| setDurations 分支 / 截图模式不加载 QSettings / 事件链 ignore() 代码审查 | 通过 |
| 无参启动 5 秒不崩溃 | 通过 |

### 人工验证项（自动测试未覆盖，代码路径已确认）

- 点击「...」→ 下拉菜单 →「设置」→ 弹窗改时长 → 确定生效（续跑/进下一阶段规则）。
- 弹窗右上角 × 取消不保存；重启程序后时长保持（QSettings 持久化）。
- 设置弹窗标题区拖拽移动。

### 证据

- 计划：`plan/delivery-plan.md`
- 执行记录：`plan/execution-log.md`
- 测试报告：`plan/test-report.md`（round 1 FAIL → round 2 PASS）
- 排查记录：`plan/triage-log.md`（2026-08-19 节）

### 残留风险

- 中心 HH:MM:SS 文本变长，旧轮次蓝色像素基线不可直接复用，后续截图断言需以新版本基线为准。
- QMenu 与设置弹窗的视觉效果（深色样式、居中、拖拽手感）需人工最终确认。

## 视觉打磨迭代：按钮同尺寸、圆角浮层、悬停动画（2026-08-19）

### 需求（改进意见）

1. 暂停/继续按钮与「...」按钮一样大小。
2. 「...」下拉浮窗丑：需圆角浮窗；hover 高亮有内 margin，蓝色外一圈灰。
3. hover 颜色加动画过渡，显得丝滑。

### 实际改动摘要（仅 main/src/focus_timer_widget.cpp）

- 新增统一常量 `kButtonRadiusRatio = 0.04`，播放按钮半径 0.05 → 0.04，与「...」按钮同尺寸（`drawButton`/`buttonRect`/`dotsButtonRect` 共用）。
- 删除 QMenu 实现，新增内部类 `DotsMenuPopup`（匿名命名空间，无 Q_OBJECT，std::function 回调）：`Qt::Popup | FramelessWindowHint | NoDropShadowWindowHint` + `WA_TranslucentBackground` 真圆角（半径 8）；高亮满宽绘制并用圆角 clip 裁剪，消除「蓝外一圈灰」；悬停高亮透明度与文字颜色按 60fps `approach` 插值渐变（120ms），与主控件动画同源。
- 弹出位置：「...」按钮正下方水平居中；点击项先 close() 再回调（回调内可能开嵌套事件循环，避免访问已删对象）。

### 验证

- 构建/安装退出码 0，`_install/main.exe` 已更新。
- `-t 3` 截图人工核验：两按钮同尺寸、同水平线，图标白色，中心 HH:MM:SS 正常。
- `-t 1` / `-t 1.5` / `-t 3` 均严格 400×400、退出码 0；无参启动 5 秒存活无崩溃。

### 人工验证项

- 点「...」看圆角浮层实际观感；hover 蓝色渐变是否丝滑；点击「设置」弹窗链路不回退。

## 视觉打磨迭代 2：按钮组居中、灰底圆点、弹出动画、滚筒设置窗（2026-08-19）

### 需求（改进意见）

1. 播放与「...」两个按钮作为整体水平居中，而非播放按钮单独居中。
2. 「...」按钮添加淡灰色背景。
3. 「...」浮层与设置窗口增加丝滑弹出动画。
4. 设置窗口：圆角、放大、时间改为竖向滚筒（可点击拖动滚动）。

### 实际改动摘要

- `main/src/focus_timer_widget.cpp`：
  - 新增 `kButtonHalfGap = 0.055`，两按钮中心对称分布于窗口中线两侧（间距不变，组中心对齐中线）。
  - `drawDotsButton` 改为常驻淡灰圆底（`#454545`，悬停渐变至 `#585858`），圆点纯白。
  - `DotsMenuPopup` 增加弹出动画：淡入 + 上滑 8px（160ms OutCubic），构造时先置全透明防首帧闪现。
- `main/src/settings_dialog.h/.cpp`：
  - 圆角：`WA_TranslucentBackground` + 12px 圆角绘制；尺寸 320×220 → 400×400。
  - 新增内部类 `WheelPicker` 竖向滚筒：按住拖动跟手滚动、松手指数趋近吸附（60fps）、支持鼠标滚轮；选中行加深底色带，文字随距中心距离渐变暗/缩小，选中项加粗纯白；时滚筒 0-99，分/秒 0-59。
  - 每行「专注/休息时间」= 时/分/秒三个滚筒 + 单位小字；`workSeconds()/restSeconds()` 由滚筒值换算。
  - 弹出动画：淡入 + 上滑 14px（200ms OutCubic）；OK 按钮圆角化。

### 验证

- 构建/安装退出码 0，`_install/main.exe` 已更新。
- `-t 3` 截图人工核验：按钮组整体居中、「...」淡灰圆底、两按钮同尺寸。
- 无参启动 5 秒存活无崩溃。

### 人工验证项

- 浮层/设置窗弹出动画丝滑度；滚筒拖动手感与吸附；滚筒设置后确定生效与持久化。

## 滚筒渐隐与设置窗文字放大迭代（2026-08-19，软件总监工作流）

### 需求（改进意见）

1. 时间设置滚轮顶部和底部没有渐隐，数字与「时/分/秒」标签视觉上重叠拥挤。
2. 设置窗口文字太小，布局中部空旷。

### 实际改动摘要（仅 main/src/settings_dialog.cpp）

- `WheelPicker::paintEvent` 绘制完数字后，顶/底各叠加一条 #2B2B2B 不透明→透明的线性渐隐色带（各 51px ≈ 1.5 项高），数字接近边缘渐隐融入背景。
- 滚筒尺寸放大：项高 30→34、宽 52→56、字号 17/14→19/16。
- 设置窗：标题 18px 加粗、行标签 15px、单位标签 12px；列间距 2→6、行间距加大；窗口 400×400 → 420×460 填充中部空白；「确定」仍中间偏下。

### 结果

- 验收：**通过**（测试轮次 1 / 5，一次通过）。
- 产物：`_install/main.exe` 已更新（Release，Qt 依赖已部署）。

### 测试结论（round 1，全部 PASS）

| 验收项 | 结果 |
| --- | --- |
| 构建 / 安装 Release 退出码 0，`_install/main.exe` 最新 | 通过 |
| 截图回归 `-t 1` / `-t 1.5` / `-t 3` 严格 400×400、退出码 0；`-t 3` 与 round6 基线像素 diff = 0（主窗口零改动） | 通过 |
| 代码审查：渐隐色带绘制顺序/颜色/高度、全部字号与尺寸常量、底部 stretch 保留 | 通过 |
| 无参启动 5 秒不崩溃 | 通过 |

### 人工验证项

- 打开设置窗：滚筒顶/底数字渐隐、与「时/分/秒」标签无重叠；整体文字大小与布局观感。

### 证据

- 计划：`plan/delivery-plan.md`
- 执行记录：`plan/execution-log.md`
- 测试报告：`plan/test-report.md`（round 1 PASS）

### 残留风险

- 渐隐色带颜色硬编码 #2B2B2B，若未来弹窗背景可变需同步抽出。

## 滚筒单位内嵌与吸入式关闭动画迭代（2026-08-19）

### 需求（改进意见）

1. 滚筒的「时/分/秒」单位放在合适位置（如数字左边），不突兀不遮挡。
2. 两个关闭按钮（主窗口 ×、设置窗 ×）点击后加小动画，最好是类似苹果「被吸进去」的效果。

### 实际改动摘要

- `main/src/settings_dialog.cpp/.h`：
  - `WheelPicker` 新增 `setUnit`：单位文字（13px 灰 #7A7A7A）画在选中数字左侧（按数字实际宽度左移 6px 定位），与数字同行居中；布局中独立的单位标签行整体移除。
  - `SettingsDialog` 重写 `reject()`：× 点击 / Esc 先播吸入式动画——窗口向自身右上角 × 收缩成 20×20 方块并加速淡出（200ms `InCubic`），结束后才真正关闭；动画期间 `setEnabled(false)` 屏蔽交互、防重入。
- `main/src/focus_timer_widget.h/.cpp`：新增 `closeButtonCenterGlobal()`（关闭按钮中心全局坐标，作为收缩目标点）。
- `main/src/frameless_window.h/.cpp`：
  - 新增 `hideToTray()`：主窗口点 × 后向右上角 × 收缩 + 加速淡出（220ms `InCubic`），结束 `hide()` 到托盘并复位几何/透明度供下次恢复；动画期间屏蔽鼠标交互。
  - 新增 `showEvent` 弹出动画：自中心 96% 放大 + 淡入（160ms `OutCubic`），首次启动与托盘恢复均生效。
  - 几何动画两端均为正方形，中间帧等比插值，不与 `resizeEvent` 的正方形回正逻辑冲突。

### 验证

- 构建/安装退出码 0，`_install/main.exe` 已更新。
- `-t 3` 截图与上一版一致（主窗口静态画面零回归）；无参启动 5 秒存活无崩溃。

### 人工验证项

- 设置窗：单位文字在选中数字左侧的观感；滚筒拖动时单位只跟随选中项。
- 主窗口 × 与设置窗 × 的吸入式动画手感；托盘恢复的弹出动画；动画完成后再操作的稳定性。

## 修复：托盘恢复位置漂移 + 滚筒单位固定覆盖层（2026-08-19）

### 需求（改进意见）

1. 主窗口关闭后从托盘显示，窗口位置改变。
2. 设置窗口「时/分/秒」字体调亮一点，且不要嵌入滚轮（滚动时单位字会跟着滚）。

### 根因

- 位置漂移：吸入动画直接操作真窗口几何，隐藏态下 Qt 的位置 bookkeeping 与动画收尾相互污染（实测恢复后左上角偏移 +501/+30，即动画收尾位置）；此外测试一度误认为「恢复失效」，实为注册表中用户真实设置（45min/5min）使 10s 阶段边界不触发。
- 单位字滚动：单位文字画在选中项的绘制分支内，随滚动偏移移动。

### 实际改动摘要

- `main/src/frameless_window.cpp/.h`：
  - 吸入动画改为快照替身方案：新增内部类 `GenieGhost`（无边框置顶 Tool 窗，绘制 `grab()` 快照，按 DPR 归一化源区）。点 × 时真窗口立即 `hide()`、几何不动；替身做 220ms `InCubic` 收缩 + 200ms 淡出后自毁。最小尺寸钳制也不再影响动画（替身无 minimumSize，可真正收到 20×20）。
  - 状态守卫：`m_hiding`/`m_popping` 互斥，托盘恢复与休息提醒恢复在动画进行中跳过；移除 `m_savedGeometry`（真窗口不再动，无需保存）。
- `main/src/settings_dialog.cpp`：`WheelPicker` 单位文字改为固定覆盖层——绘制在渐隐色带之后、滚筒中轴当前数字左侧（两位数字宽度恒定，定位不受滚动中间态影响），颜色 #7A7A7A → #C8C8C8。

### 验证

- 构建/安装退出码 0，`_install/main.exe` 已更新。
- 自动化位置回归（`_shottest/round9/pos_test.ps1`）：点 × 隐藏 → 跨过工作→休息边界自动恢复 → `GetWindowRect` 与隐藏前逐像素一致（diff 全 0），PASS。
- `-t 3` 截图与上版 diff-bbox = None（零回归）；无参启动 5 秒存活无崩溃。
- 用户持久化设置（2700/300）已备份并在测试后恢复。

### 人工验证项

- 吸入动画观感（替身快照方案动画终点可真正收到 20×20，视觉效果应与此前一致或更好）。
- 设置窗单位字固定不滚动、亮度合适。

## 当前整体状态（2026-08-19 截至本轮）

### 验收结论

**通过。** `_install/main.exe` 已更新（Release，Qt 运行依赖已部署）。

本轮（关闭吸入动画 + 滚筒单位覆盖层 + 托盘恢复位置修复）构建/安装退出码 0；隐藏→工作/休息边界自动恢复后窗口矩形与隐藏前逐像素一致（diff 全 0）；`-t 3` 截图与上版 diff-bbox = None；无参启动 5 秒无崩溃。

### 功能

- 无边框正方形主窗口（屏幕面积 30%，最小 200×200），右上角 × 隐藏到系统托盘；托盘左键/双击恢复，右键「退出」真正关闭。
- 圆环刻度 36 格，工作灰→蓝、休息蓝→灰，中心显示剩余时间 HH:MM:SS。
- 默认时长工作 10s / 休息 10s（截图模式 `-t` 始终用此默认值）；交互模式从 QSettings("Chandao","FocusTimer") 加载持久化时长。
- 播放/暂停与「...」按钮作为一组水平居中；「...」弹出圆角深色菜单，内含「设置」。
- 设置弹窗：无边框圆角 420×460，右上角 × 取消，竖向滚筒设置专注/休息时长（时 0–99、分/秒 0–59），确定后按「新时长 > 已用则续跑，否则立即进下一阶段」应用并持久化。
- 工作→休息切换：若窗口在托盘中则恢复显示，并 QApplication::alert 闪烁任务栏；无全屏蒙版。

### 视觉与动画

- 背景 `#2B2B2B`，激活蓝 `#55B2E8`，未激活灰 `#3D3D3D`。
- 播放按钮蓝色圆底、白色暂停/播放图标；「...」按钮淡灰圆底、白色三点，悬停提亮有 60fps 插值。
- 「...」浮层：圆角、满宽 hover 高亮渐变、淡入+上滑弹出。
- 设置窗：滚筒顶/底渐隐；「时/分/秒」为固定覆盖层画在当前数字左侧（不随滚动移动），颜色 `#C8C8C8`；弹出淡入+上滑。
- 关闭动画（主窗口 × 与设置窗 ×）：快照替身向右上角 × 收缩成 20×20 并加速淡出（约 220ms InCubic）；主窗口从托盘恢复时自中心 96% 放大 + 淡入（160ms OutCubic）。真窗口几何在关闭动画中不动，恢复后位置不漂移。

### 命令行截图接口

```
main.exe -t <秒数> [-s <边长>]
```

无弹窗，输出 `shot_<t>.png`，尺寸严格等于 `-s`（默认 400）。

### 证据

- 计划：`plan/delivery-plan.md`
- 执行记录：`plan/execution-log.md`
- 测试报告：`plan/test-report.md`
- 排查记录：`plan/triage-log.md`
- 位置回归脚本：`_shottest/round9/pos_test.ps1`（PASS，diff 全 0）

### 残留风险 / 人工验证项

- 吸入动画、设置窗单位字亮度与滚筒拖动手感需人工最终确认。
- 渐隐色带颜色硬编码 `#2B2B2B`，若弹窗背景可变需同步抽出。
- 全屏提醒仅覆盖主屏（上一轮已去掉蒙版，仅任务栏闪烁）。


## 修复：关闭动画布局闪帧（2026-08-19）

### 需求

点击关闭按钮后，执行动画瞬间布局全部错误，窗口先闪一下再缩小，而不是直接缩小或关闭。

### 根因

- 设置窗 `reject()` 对**真窗口**做 geometry 收缩（420×460 → 20×20），Qt 布局随尺寸重排，滚筒固定尺寸无法等比缩放。
- 主窗口虽已有快照替身，但 `hide()` 会触发 Windows DWM 默认隐藏过渡，真窗口矩形被系统缩放，表盘/按钮按比例重绘，闪一帧错乱布局；且原先 `hide()` 在 ghost `show()` 之前。

### 实际改动摘要

- 新建 header-only `main/src/genie_ghost.h`：共用快照替身（吸入 20×20、InCubic、几何 220ms / 淡出 200ms）。
- `frameless_window.cpp` `hideToTray()`：先 grab 并 show/raise/repaint 替身盖住真窗 → setWindowOpacity(0)（几何不动）→ Windows 下禁用 DWM 过渡 → 再 hide() → 替身吸入。
- `settings_dialog.cpp` `reject()`：不再对 this 做 geometry 动画；setWindowOpacity(0) 藏真窗（模态期间不 hide()）；替身结束后才 QDialog::reject()。

### 结果

- 验收：**通过**（测试轮次 1 / 5，一次通过）。
- 产物：`_install/main.exe` 已更新（Release，14:55:15）。
- 截图回归 `-t 1/1.5/3` 严格 400×400、退出码 0，与 round7 基线像素差 0。
- 无参启动 5 秒无崩溃。

### 人工验证项

- 主窗口点右上角 ×：只看到快照缩小吸入，表盘/按钮不重排，无「先闪一下再缩小」。
- 设置窗点 ×：滚筒/按钮不重排；动画结束后才真正关闭。

### 证据

- 计划：`plan/delivery-plan.md`
- 执行记录：`plan/execution-log.md`
- 测试报告：`plan/test-report.md`（round 1 PASS）

## 修复 round 2：关闭动画仍闪错乱布局（2026-08-19）

### 需求

用户复测：点 × 一瞬间仍闪，随后布局错误。

### 根因（round 1 方案未覆盖）

- 替身 show、开始缩小、真窗口 hide 挤在同一帧，DWM 仍可能把真窗口缩成非正方形。
- `resizeEvent` 在隐藏过程中仍按短边回正。
- DPR=2 下三参数 `drawPixmap` 只画出快照左上 1/4，看起来像布局全错。
- 主窗口首次 `setWindowOpacity(0)` 会重建 layered HWND。

### 实际改动摘要

- `GenieGhost` 分步 `appearAt`（完整上屏并刷新合成器）再 `suckInto`；绘制改两参数 `drawPixmap(rect(), snapshot)`。
- 主窗口：构造时禁用 DWM 过渡；appearAt → hide → suckInto；hiding 期间不做短边回正；不再改 opacity。
- 设置窗：同样先 appearAt 再透明再吸入。

### 结果

- 自动验收：**通过**（round 2 / 5）。
- 产物：`_install/main.exe` 已更新（Release，15:08:27）。
- 截图回归 `-t 1/1.5/3` 与基线像素差 0。
- **点 × 观感仍需用户确认。**

### 证据

- 计划：`plan/delivery-plan.md`
- 执行记录：`plan/execution-log.md`
- 测试报告：`plan/test-report.md`（round 2 PASS）
- 排查：`plan/triage-log.md`（round 2）

## 本轮交付总结（2026-08-19）

### 需求

1. 点击关闭按钮后，动画瞬间布局全部错误：窗口先闪一下再缩小。修复。
2. 用户复测：点 × 一瞬间仍闪，随后布局错误。继续修。
3. 「时 / 分 / 秒」和滚筒叠在一起，字有一部分被遮挡。修复。

### 做了什么

**关闭吸入动画（主窗口 ×、设置窗 ×）**

- 新建 header-only `main/src/genie_ghost.h`：快照替身分步 `appearAt`（完整尺寸上屏并刷新合成器）再 `suckInto`（向 × 收缩成 20×20 并淡出，InCubic，约 220ms）。
- 绘制使用两参数 `drawPixmap(rect(), snapshot)`，避免 200% 缩放（DPR=2）下三参数源矩形只画出左上 1/4。
- 主窗口 `hideToTray()`：appearAt → `hide()` → suckInto；构造时禁用 DWM 隐藏过渡；`m_hiding` 期间 `resizeEvent` 不做短边回正；不再 `setWindowOpacity`（避免 HWND 被加成 layered 后闪帧）。真窗口几何在动画中不动。
- 设置窗 `reject()`：不对真 Dialog 做 geometry 动画；appearAt 后仅改透明度，动画结束才 `QDialog::reject()`。

**设置窗单位文字**

- 「时 / 分 / 秒」从滚筒内部绘制改为每个滚筒**右侧外侧**独立标签（14px，`#C8C8C8`），与选中行垂直居中。滚筒内只显示两位数字，不再互相裁切/遮挡。

### 结果是什么

- 验收：自动项**通过**（关闭动画测试 round 1 FAIL 观感 → round 2 自动 PASS；单位文字为后续小改，Release 编译/安装成功）。
- 产物：`_install/main.exe`（Release，Qt 运行依赖已部署）。
- Git：已提交并推送 `main`。

| 项 | 值 |
| --- | --- |
| 提交 | `5811ab5` |
| 说明 | Fix close-button suck-in flash and keep time unit labels off the wheels. |
| 远程 | `https://github.com/KryieNaruto/chandao.git` |
| 范围 | `e342900..5811ab5` |

### 测试结论

| 检查项 | 结果 |
| --- | --- |
| Release 构建 / 安装退出码 0 | 通过 |
| `-t 1` / `-t 1.5` / `-t 3` 严格 400×400、退出码 0 | 通过（与 round7/round10 像素差 0） |
| 源码：关闭动画只缩放快照替身；hiding 不做短边回正 | 通过 |
| 无参启动 5 秒不崩溃 | 通过 |
| 点 × 无错乱闪帧 | 人工项（round 2 后请用户再确认） |
| 「时 / 分 / 秒」在滚筒外、数字不被挡 | 人工项（请打开设置确认） |

### 证据

- 计划：`plan/delivery-plan.md`
- 执行记录：`plan/execution-log.md`
- 测试报告：`plan/test-report.md`（关闭动画 round 2 PASS）
- 排查记录：`plan/triage-log.md`（round 2）
- 本文件：`plan/results/final-result.md`

### 残留风险 / 人工验收项

- 主窗口 / 设置窗点 × 的吸入观感需人工最终确认。
- 设置窗「时 / 分 / 秒」与数字是否完全无遮挡需打开设置确认。
- 渐隐色带颜色仍硬编码 `#2B2B2B`。
- `_shottest/` 未纳入本次提交。

## 中心植物生长（Vulkan）交付总结（2026-08-20）

### 需求

在已有 `CenterMode::Plant` 扩展位上新增可点击切换的扁平植物生长中心样式；本期只做小树种子「种子-发芽」（5 个时间阶段 / 6 个视觉态），使用离屏 Vulkan 渲染。

### 做了什么

- 种子注册表 + `PlantScene` 六段扁平几何（盆土 / 芽 / 叶 / 持续生长 / 树冠 / 树干）。
- CMake 条件化 `HAS_VULKAN`：无 SDK 仍能编译；有 SDK 且有 `glslangValidator` 时编离屏 Vulkan 渲染器，SPIR-V 输出到构建目录。
- `VulkanPlantRenderer`：OPTIMAL 附件 + layout barrier + staging 回读。
- 点击表盘内圆在数字时间与植物模式间切换；「...」→「选择种子」；CLI `-t -c plant`。
- 截图模式 Vulkan 失败：退出码 2，写 `plant-vulkan-failed.txt`，不保存 PNG。

### 结果是什么

- 验收：自动项 **通过**（测试轮次 2 / 5）。
- 产物：`_install/main.exe` 已更新（Release，143360 字节，2026-08-20 10:45:27），链接 `vulkan-1.dll`。
- 本机已安装 LunarG Vulkan SDK 1.4.357.0（`C:\VulkanSDK\1.4.357.0`）。

### 测试结论（round 1，无 SDK）

| 检查项 | 结果 |
| --- | --- |
| Release 构建 / 安装退出码 0 | 通过 |
| `-t 1` / `1.5` / `3` 严格 400×400，蓝像素 1.5>1 | 通过 |
| 无 SDK：`-c plant` 退出码 2 + `plant-vulkan-failed.txt` + 无充数 PNG | 通过 |
| 圆心 accept/ignore、frameless 排除中心热区 | 通过 |
| 无参启动 5 秒不崩溃 | 通过 |
| 植物 6 态 Vulkan 出图趋势 | 当时环境阻塞 |

### 测试结论（round 2，已装 SDK，PASS）

| 检查项 | 结果 |
| --- | --- |
| CMake `Vulkan found` + 编入 `vulkan_plant_renderer.cpp` | 通过 |
| TimeText `-t 1/1.5/3` 400×400，蓝像素与 round 1 一致 | 通过 |
| `-c plant` 六张图退出码 0、400×400、无失败日志 | 通过 |
| 绿像素 0.5→2→4→7.5：0 → 130 → 610 → 3812 | 通过 |
| 7.5s 中心无时间字；9.5s 树干 + `00:00:01`；13s 树干 + 休息 `00:00:07` | 通过 |
| 无参启动 5 秒不崩溃 | 通过 |
| 点击圆心来回切、选种子菜单 | 人工项 |

### 证据

- 计划：`plan/delivery-plan.md`
- 执行记录：`plan/execution-log.md`
- 测试报告：`plan/test-report.md`（round 1 PASS 无 SDK；round 2 PASS 有 Vulkan）
- 本文件：`plan/results/final-result.md`

### 残留风险 / 人工验收项

- 点击圆心切换、选种子菜单手感需你本地确认。
- 阶段 6 时间字淡入色约为 (175,175,175)，不是满不透明 `#E8E8E8`，属计划内透明度。
