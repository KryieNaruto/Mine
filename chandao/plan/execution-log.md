# 执行记录

## 2026-08-18 交付执行（任务 1、2）

### 做了什么

- 任务 1：`FocusTimerWidget` 增加公共方法 `setTime(double seconds)`。实现逻辑：停止内部 `QTimer`，置 `m_isRunning = true`，按「工作 10s + 休息 3s」循环用 `fmod(seconds, 13.0)` 取模；余数 < `m_workDuration` 时 `m_state = 0`、`m_elapsed = 余数`，否则 `m_state = 1`、`m_elapsed = 余数 - m_workDuration`。未改动现有 `paintEvent`/鼠标/定时器逻辑。
- 任务 2：`main.cpp` 增加命令行解析。支持 `-t <秒数>`（double，支持小数）与 `-s <边长>`（int，默认 400）。提供 `-t` 时：不显示窗口，创建 `FocusTimerWidget`，`resize(size, size)`，调用 `setTime(t)`，用 `grab()` 转 `QImage` 保存 PNG，文件名为 `shot_<t参数原文>.png`，保存到进程当前工作目录，成功返回 0。未提供 `-t` 时保持原有交互窗口行为（屏幕面积 30% 正方形窗口，标题「专注」）完全不变。

### 改了哪些文件

- `main/src/focus_timer_widget.h`：声明 `setTime(double)`。
- `main/src/focus_timer_widget.cpp`：实现 `setTime(double)`，新增 `<cmath>` 头文件。
- `main/src/main.cpp`：新增 `-t` / `-s` 解析与离屏截图保存逻辑。

### 编译自检

- `cmake --build _build --config Release` 通过，`main.exe` 生成成功，无编译错误。

### 未做事项

- 未执行 `cmake --install` 与出图对比验收（按计划由测试 Agent 负责）。
- 未做自动图像对比功能（计划明确不做）。

## 2026-08-18 round 2 修复（高 DPI 截图尺寸翻倍）

### 修复内容

- 现象：DPR=2 机器上 `QWidget::grab()` 返回物理像素，PNG 尺寸为 `-s` 指定值的 2 倍（默认 400 出 800×800）。
- 按排查 Agent 方案 A（最小改动），在 `main/src/main.cpp` 截图保存段：`grab()` 转 `QImage` 后，若 `image.size() != QSize(shotSize, shotSize)`，则用 `image.scaled(shotSize, shotSize, Qt::IgnoreAspectRatio, Qt::SmoothTransformation)` 归一化，再保存 PNG；保存失败返回 1。100% 缩放机器走原路径，零开销无回归。
- 未改动 `focus_timer_widget.*`、CMake 及其他任何逻辑。

### 改动文件

- `main/src/main.cpp`：截图保存段新增 DPR 尺寸归一化（含 `QImage` 变量与条件缩放，含中文注释说明原因）。

### 编译自检

- `cmake --build _build --config Release` 通过，`main.exe` 重新生成成功，无编译错误。

### 未做事项 / 阻塞

- 未执行 `cmake --install` 与逐条验收（尺寸 400×400 / 200×200、蓝色像素对比、无参弹窗回归），由测试 Agent 在 round 2 复测。
- 无阻塞。

## 2026-08-18 交付执行（按钮缩小 / 按钮动画 / 刻度参数化 / 自适应 / 无边框窗口）

### 做了什么

1. **按钮缩小**：`drawButton` 与 `buttonRect` 半径由 `base * 0.126` 改为 `base * 0.05`（直径 ≈ 窗口边长 10%），内部图标（双竖线/三角）尺寸全部改为按按钮半径等比（间距 `r*0.30`、半高 `r*0.42`、线宽 `r*0.22`），小窗口下天然自适应。
2. **按钮动画**：复用现有 60fps `QTimer`（`updateTimer`）推进 4 个 0..1 插值系数——悬停提亮（按钮径向渐变两端按 `lighter()` 提亮，约 120ms）、按压瞬时缩放 0.92 倍并回弹（约 60ms 响应）、暂停↔播放图标约 150ms 交叉淡化过渡（`m_iconT` 中间态两图标按透明度叠加绘制）；图标保留线性渐变（顶端 `#101010` → 底端 `#3A3A3A`）。新增 `setMouseTracking` 支持悬停；点击语义改为按下缩放、松开在按钮内才切换状态（观感更自然）。
3. **刻度参数化**：`focus_timer_widget.h` 顶部新增 `TickStyle` 结构体（数量 36、宽度比 0.04→0.026 调细、高度比 0.08、圆角比 0.013≈宽度一半呈胶囊形、环半径比 0.28、前沿渐变带宽 0.03），每项带中文注释与调节建议；`drawRing` 全部改读结构体成员。
4. **自适应布局**：所有绘制继续按 `min(w,h)` 比例坐标，无固定像素；窗口最小 200×200、全程以短边回正保持正方形（拖拽缩放在 `applyResize` 内回正，程序性 resize 在 `resizeEvent` 内回正）；`-t` 截图路径未动（仅 `setTime` 内部增加 `m_timerStopped` 置位，停表后不再推进动画，画面保持确定）。
5. **无边框窗口**：新增 `main/src/frameless_window.h/.cpp`（`FramelessWindow : QMainWindow`，`Qt::FramelessWindowHint`），背景 `#2B2B2B` 与表盘一致；控件左上角绘制扁平风 × 关闭按钮（直径 `base*0.07`，悬停浮现浅灰圆底 + × 提亮），窗口层命中后 `close()`；空白区域按住拖拽移动；边缘 6px 热区 8 方向缩放（`edgeHit` + 对应缩放光标 + `applyResize` 手动 `setGeometry`，纯 Qt 实现）；构造时取主屏幕面积 30% 正方形并居中。关闭按钮/播放按钮命中优先于移动/缩放判定，互不冲突。

### 改了哪些文件

- `main/src/focus_timer_widget.h`：新增 `TickStyle` 结构体、动画状态成员、`closeButtonHit`/`playButtonHit` 公共接口、图标/关闭按钮绘制私有方法。
- `main/src/focus_timer_widget.cpp`：按钮缩小、动画插值、刻度参数化、关闭按钮绘制、悬停/按压/松开事件处理。
- `main/src/frameless_window.h` / `main/src/frameless_window.cpp`：新增无边框窗口类。
- `main/src/main.cpp`：交互模式改用 `FramelessWindow`（去掉 `setFixedSize`）；`-t`/`-s` 截图逻辑一行未改。
- `main/CMakeLists.txt`：`main` 目标加入两个新源文件。

### 编译自检

- `cmake --build _build --config Release` 通过，`main.exe` 生成成功，无编译错误/警告。
- 轻量验证（非完整验收）：`-t 1 -s 200` 退出码 0、输出严格 200×200，截图中按钮直径 ≈ 边长 10%、刻度明显变细。

### 未做事项 / 阻塞

- 未执行 `cmake --install` 与逐条交互验收（拖拽/缩放手感、动画观感、`-t 1.5` 回归），由测试 Agent 负责。
- 无阻塞。

## 2026-08-18 交付执行（按钮扁平化 / 托盘化 / 中心数字 / 休息反向 / 全屏提醒）

### 做了什么

1. **按钮扁平化**：`drawButton` 去掉径向渐变（原中心 `#77C8F8` → 边缘 `#55B2E8`），改为纯色 `#55B2E8`（`m_activeColor`），悬停提亮（`lighter(100 + lift*100)`）、按压缩放 0.92、图标交叉淡化动画全部保留；暂停/播放图标的线性渐变同步去除，改为纯色 `#101010`（保留 alpha 交叉淡化）。
2. **关闭按钮右上角 + 托盘化**：`closeButtonRect()` 从左上角移到右上角（`width() - m - d, m`）；`FramelessWindow::mousePressEvent` 命中关闭按钮后由 `close()` 改为 `hide()`；`setupTray()` 创建 `QSystemTrayIcon`（图标取窗口图标，为空则回退 `QStyle::SP_TitleBarNormalButton` 标准图标，未引入新资源文件），右键菜单含「退出」→ `QApplication::quit()`，左键/双击托盘图标恢复主窗口；托盘不可用时跳过创建。截图模式（`-t`）不创建 `FramelessWindow`，天然不建托盘。
3. **CenterMode 扩展架构**：`focus_timer_widget.h` 新增 `enum class CenterMode { TimeText, Plant }`、`setCenterMode`/`centerMode` 接口与 `drawCenterContent` 分发函数；`Plant` 分支留空并注释「植物模式预留」。本期实现 `TimeText`：圆环中心（与 `drawRing` 同圆心 `w*0.5, h*0.4`）绘制剩余秒数，向上取整为整数（减 1e-9 防浮点误差，下限钳到 1，范围 10→1），工作阶段 `#E8E8E8`、休息阶段 `#55B2E8`，字号 `min(w,h) * 0.12` 加粗居中。
4. **休息计时反向**：`m_restDuration` 3.0 → 10.0（周期 20s）；`drawRing` 休息阶段 `progress = elapsed / restDuration`，刻度的蓝色占比 `t = 1 - cover`，从全蓝开始顺时针逐刻度变灰（与工作灰→蓝正好相反）；每个刻度内部沿切线方向的前沿渐变保留，休息分支方向反转（灰在后侧、蓝在前侧）；休息结束自动回到工作阶段。
5. **全屏休息提醒**：`frameless_window.cpp` 内新增内部小类 `RestOverlay : QWidget`（`Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool`，`WA_TranslucentBackground`），覆盖主屏 `screen->geometry()`，背景 `rgba(0,0,0,180)`，居中绘制白色「休息一下」+ 剩余秒数倒计时（100ms 自刷新）；`FocusTimerWidget` 新增 `phaseChanged(int)` 信号在阶段切换时发射，`FramelessWindow::setupRestOverlay()` 持有遮罩并按信号弹出/关闭；截图模式 `setTime` 停表不发射信号、也不创建 `FramelessWindow`，不触发提醒。
6. **setTime 适配**：`setTime` 原有 `fmod(seconds, workDuration + restDuration)` 逻辑不变，随 `restDuration` 变 10s 自动适配 20s 周期；中心数字与休息反向配色均按确定性状态渲染。

### 改了哪些文件

- `main/src/focus_timer_widget.h`：`CenterMode` 枚举、`setCenterMode`/`centerMode`、`remainingSeconds()`、`phaseChanged` 信号、`drawCenterContent`/`drawCenterTimeText` 声明、关闭按钮注释更新。
- `main/src/focus_timer_widget.cpp`：按钮/图标扁平化、关闭按钮右上角、中心数字绘制、休息反向进度与渐变、休息时长 10s、阶段切换发信号。
- `main/src/frameless_window.h`：托盘与休息提醒成员、`setupTray`/`setupRestOverlay` 声明、类注释更新。
- `main/src/frameless_window.cpp`：`RestOverlay` 内部类、托盘创建与菜单、关闭按钮改 `hide()`、阶段信号联动。
- `main/CMakeLists.txt` 未改动（未新增文件）。

### 编译自检

- `cmake --build _build --config Release` 通过，`main.exe` 生成成功，无编译错误。

### 未做事项 / 阻塞

- 未执行 `cmake --install`，未做截图/交互验收（`-t 3`、`-t 13` 中心数字与反向配色、托盘隐藏/退出、全屏提醒弹出与自动关闭），由测试 Agent 负责。
- 植物生长模式仅留枚举与分发接口，未实现（计划明确不做）。
- 无阻塞。

## 2026-08-18 交付执行 round 2（修复鼠标事件不传播：拖拽 / 关闭托盘 / 边缘缩放失效）

### 做了什么

按排查 Agent 定位的根因（`FocusTimerWidget` 铺满客户区且不 `ignore()` 鼠标事件，父窗口 `FramelessWindow` 的拖拽/关闭/边缘缩放处理器收不到事件），仅对 `main/src/focus_timer_widget.cpp` 三个函数做最小修复：

1. `mousePressEvent`：未命中播放按钮 `buttonRect`（含命中右上角 ×）时调用 `event->ignore()`，事件传播给父窗口接管拖拽/关闭/边缘缩放。
2. `mouseReleaseEvent`：`m_buttonPressed` 为假时 `event->ignore()`。
3. `mouseMoveEvent`：悬停状态更新后，既不在播放按钮也不在关闭按钮上时 `event->ignore()`，救活父窗口边缘缩放光标。

坐标无需换算（子控件位于父窗口 (0,0)，Qt 传播时自动转换）。未动窗口 flags（无 `WindowStaysOnTopHint` 残留）。

同时将 round 2 排查段落（审核/根因/修复建议/是否建议停止）代写落盘到 `plan/triage-log.md`。

### 改了哪些文件

- `main/src/focus_timer_widget.cpp`：上述三个鼠标事件函数补 `event->ignore()`。
- `plan/triage-log.md`：追加 round 2 排查段落。
- `plan/execution-log.md`：追加本段。

### 编译自检

- `cmake --build _build --config Release` 通过，`main.exe` 生成成功，无编译错误（实现侧自查）。

### 未做事项 / 阻塞

- 未执行 `cmake --install`，未做截图回归与人工交互复测（拖动、× 隐藏到托盘、边缘缩放），完整验收由测试 Agent 负责。
- 无阻塞。

## 2026-08-19 移除全屏休息提醒，改为「恢复 + 闪烁提醒」

### 做了什么

按 `plan/delivery-plan.md` 末尾「变更（2026-08-19）」段落执行：

1. 整体删除 `RestOverlay` 全屏半透明蒙版小类（创建、显示、倒计时刷新、关闭全部代码），并清理因此不再使用的 `#include <QPainter>` / `<QTimer>` / `<QtMath>`（`<cmath>` 保留，构造器仍用 `std::sqrt`）。
2. `setupRestOverlay()` 改名为 `setupRestAlert()`：仍挂在现有 `FocusTimerWidget::phaseChanged(int)` 信号上，`state == 1`（工作→休息）时——若主窗口不可见（托盘隐藏态）则 `showNormal()` + `activateWindow()` 恢复显示；随后无条件调用 `QApplication::alert(this)`（Windows 上即任务栏闪烁提醒）。休息→工作方向不做任何动作。
3. 其余行为未动：托盘、中心数字、休息反向进度、拖拽、边缘缩放、`-t` 截图模式（信号不发，不触发提醒）均不回归。

### 改了哪些文件

- `main/src/frameless_window.h`：删除 `m_restOverlay` 成员与 `setupRestOverlay()` 声明，新增 `setupRestAlert()`，更新类注释。
- `main/src/frameless_window.cpp`：删除 `RestOverlay` 类与相关 include；实现 `setupRestAlert()`。
- `plan/execution-log.md`：追加本段。

### 编译自检

- `cmake --build _build --config Release` 通过，`main.exe` 生成成功，无编译错误/警告（实现侧自查）。

### 未做事项 / 阻塞

- 未执行 `cmake --install`，未做运行态验证（跨工作→休息边界的恢复显示与任务栏闪烁、托盘/截图回归），完整验收由测试 Agent 负责。
- 无阻塞。

## 2026-08-19 交付执行（按钮下移白图标 / 「...」按钮 / 设置弹窗 / 时长持久化 / HH:MM:SS）

### 做了什么

按 `plan/delivery-plan.md`「任务拆分」全部 6 项执行：

1. **播放/暂停按钮下移 + 图标白色**：`drawButton` 与 `buttonRect` 中心 y 由 `h*0.78` 下移至 `h*0.86`（半径不变 `base*0.05`）；`drawPauseIcon` / `drawPlayIcon` 图标颜色 `#101010` → `#FFFFFF`。
2. **「...」按钮**：`dotsButtonRect()` 中心 `(w*0.5 + base*0.11, h*0.86)`（与播放按钮同一水平线右侧），半径 `base*0.04`；`drawDotsButton` 自绘三个横向圆点（点径 `base*0.008`，间距 `r*0.55`），颜色由未激活灰提亮（`m_inactiveColor.lighter(200 + 120*m_dotsHoverT)`），悬停时浮现浅灰圆底（alpha 0.10*m_dotsHoverT），复用 60fps `updateTimer` 插值机制（新增独立 `m_dotsHoverT`/`m_dotsHovered`/`m_dotsPressed`）；点击（按下+松开在热区内）发 `dotsClicked` 信号；未命中热区的鼠标按下/松开/移动事件全部 `event->ignore()` 交还父窗口，`FramelessWindow::mousePressEvent` 同步将 dots 热区排除在拖拽判定外。
3. **下拉菜单**：归属 `FocusTimerWidget`（`showDotsMenu`），点击「...」弹出 `QMenu`（深色样式表：背景 `#2B2B2B`、文字 `#E8E8E8`、选中 `#55B2E8`、边框 `#3D3D3D`），含 action「设置」，经 `mapToGlobal` 弹出在按钮正下方 4px。
4. **设置弹窗**：新增 `main/src/settings_dialog.h/.cpp`，`SettingsDialog : QDialog`（`Qt::Dialog | Qt::FramelessWindowHint`，固定 320×220，背景 `#2B2B2B`）；右上角自绘 ×（点击 `reject()` 取消不保存）；两行「专注时间」「休息时间」各一个 `QTimeEdit`（`displayFormat "HH:mm:ss"`，深色样式 `#3D3D3D` 底 / `#E8E8E8` 字）；中间偏下「确定」按钮（扁平 `#55B2E8` 底白字，`accept()`）；顶部 40px 标题区（不含 × 热区）支持按住拖拽移动；弹出时居中于父窗口；时长输入上限钳到 23:59:59（QTimeEdit 上限）。已加入 `main/CMakeLists.txt` 的 `main` 目标。
5. **时长应用 + 持久化**：`FocusTimerWidget::setDurations(double workSec, double restSec)`——非正数直接拒绝；先存新时长，当前阶段若 `m_elapsed >= 新时长` 则立即切到下一阶段、`m_elapsed = 0`、发 `phaseChanged`（`m_timerStopped` 截图模式不发），否则续跑（剩余 = 新时长 - 已用，天然满足）；非当前阶段时长仅更新存储值。「确定」后在 `openSettingsDialog` 内用 `QSettings("Chandao","FocusTimer")` 写 `workSeconds`/`restSeconds`；`main.cpp` 交互模式启动时读取（默认 10.0/10.0）并经 `FramelessWindow::timerWidget()` 新访问器调用 `setDurations`；截图模式（`-t`）不加载，始终默认 10s/10s。
6. **中心时间 HH:MM:SS**：`drawCenterTimeText` 改为 `%1:%2:%3`（均补零两位，如 `00:00:07`），字号由 `base*0.12` 调小为 `base*0.075`，绘制框半宽由 `base*0.15` 放宽为 `base*0.22`，保证 8 字符在圆环内完整显示；取下限改为 `qMax(0, ...)`。

### 改了哪些文件

- `main/src/focus_timer_widget.h`：`dotsButtonHit`、`setDurations`、`workSeconds`/`restSeconds`、`dotsClicked` 信号、`drawDotsButton`/`dotsButtonRect`/`showDotsMenu`/`openSettingsDialog` 声明、dots 动画状态成员。
- `main/src/focus_timer_widget.cpp`：上述全部实现 + 按钮下移、图标白色、中心 HH:MM:SS、鼠标事件热区扩展（未命中仍 `ignore()`）、`updateTimer` 增加 `m_dotsHoverT` 插值。
- `main/src/settings_dialog.h` / `main/src/settings_dialog.cpp`：新增设置弹窗。
- `main/src/frameless_window.h`：新增 `timerWidget()` 访问器。
- `main/src/frameless_window.cpp`：拖拽判定排除 dots 热区。
- `main/src/main.cpp`：交互模式启动加载 QSettings 并调用 `setDurations`。
- `main/CMakeLists.txt`：`main` 目标加入 `settings_dialog.h/.cpp`。
- `plan/execution-log.md`：追加本段。

### 编译自检

- `cmake --build _build --config Release` 通过（CMake 因 CMakeLists 变更自动重配，Vulkan 仍未找到按预期跳过），`main.exe` 生成成功，无编译错误。

### 未做事项 / 阻塞

- 未执行 `cmake --install`，未做截图回归（`-t 1/1.5/13` 尺寸与退出码、`-t 3` 按钮位置/白图标/三点/HH:MM:SS 核验）与人工交互验收（「...」→「设置」→改时长→确定生效、× 不保存、重启保持、拖拽/托盘/边缘缩放回归），由测试 Agent 负责。
- 按钮最终参数：播放/暂停按钮中心 `(w*0.5, h*0.86)`、半径 `base*0.05`；「...」按钮中心 `(w*0.5 + base*0.11, h*0.86)`、半径 `base*0.04`（`base = min(w,h)`），均未再微调，若截图验收认为间距不理想可在此基础上调。
- 设置弹窗时长上限 23:59:59（`QTimeEdit`/`QTime` 限制），更长的跨天时长不支持（计划未要求）。
- 无阻塞。

## 2026-08-19 交付执行 round 2 修复（「...」圆点颜色非白色）

### 做了什么

- 测试 round 1 FAIL 唯一未达标项：「...」按钮三个圆点实测为灰色 (122,122,122)，计划要求白色。
- 按排查结论最小修复：`drawDotsButton` 中圆点颜色原用 `m_inactiveColor.lighter(200 + 120*m_dotsHoverT)`，基色 `#3D3D3D` 提亮上限约 (195,195,195)，永远到不了白色；改为直接纯白 `#FFFFFF`，悬停反馈仍由既有的浅灰圆底（alpha 0.10*m_dotsHoverT）承担，与播放按钮白色图标风格统一。
- 未动其他任何逻辑。

### 改了哪些文件

- `main/src/focus_timer_widget.cpp`：`drawDotsButton` 圆点颜色一行改动（含注释更新）。
- `plan/execution-log.md`：追加本段。

### 编译自检

- `cmake --build _build --config Release` 通过，`main.exe` 重新生成成功，无编译错误。

### 未做事项 / 阻塞

- 未执行 `cmake --install` 与完整复测（`-t 3` 圆点白色像素检测等），由测试 Agent 在 round 2 复测。
- 无阻塞。

## 2026-08-19 交付执行（滚筒渐隐与设置窗文字放大）

### 做了什么

按 `plan/delivery-plan.md`「任务拆分」全部 2 项执行，仅改 `main/src/settings_dialog.cpp`：

1. **WheelPicker 渐隐 + 尺寸放大**：
   - `paintEvent` 在绘制完所有数字项之后，于顶部和底部各叠加一条 `QLinearGradient` 渐变色带（`#2B2B2B` 不透明 → 全透明，各占 `kItemH * 1.5` = 51px），数字接近滚筒边缘时融入背景，不再与下方「时/分/秒」单位标签视觉重叠。色带绘制在选中行底色带与数字之后，直接覆盖边缘数字；选中行底色带位于中心（y≈68..102），与色带（y<51、y>119）无交叠，不受影响。
   - 尺寸放大：项高 `kItemH` 30→34（滚筒总高 150→170）、宽 52→56、选中字号 17→19、普通字号 14→16（均采用计划给定值，未再微调）。
   - 新增 `#include <QLinearGradient>`。
2. **设置窗文字放大 + 布局填充**：
   - 标题「设置」：`QFont` 18px 加粗；行标签「专注时间/休息时间」15px；单位标签（时/分/秒）12px。
   - 滚筒列与单位标签间距 2→6；标题下间距 10→12；两行间距 14→20；窗口尺寸 400×400 → 420×460。
   - 「确定」按钮位置语义不变（底部 stretch 保留，仍落中间偏下）。

### 改了哪些文件

- `main/src/settings_dialog.cpp`：上述全部改动（渐隐色带、尺寸/字号常量、字体设置、间距、窗口尺寸）。
- `plan/execution-log.md`：追加本段。

### 最终尺寸与字号常量

- WheelPicker：宽 56、项高 `kItemH = 34`、可见项 `kVisible = 5`（未变）、选中字号 19px、普通字号 16px、渐隐带高 51px（1.5 项高）、色带颜色 `#2B2B2B`（与弹窗背景一致）。
- SettingsDialog：窗口 420×460；标题 18px 加粗；行标签 15px；单位标签 12px；列内间距 6；标题下间距 12；行间间距 20。

### 编译自检

- `cmake --build _build --config Release` 通过，`main.exe` 生成成功，无编译错误。

### 未做事项 / 阻塞

- 未执行 `cmake --install`，未做截图回归（`-t 1/1.5/3`）与人工观感验收（渐隐效果、与单位标签间距、文字大小、中部空白填充），由测试 Agent 负责。
- 无阻塞。

## 2026-08-19 交付执行（关闭动画改快照替身，消除布局闪帧）

### 做了什么

按 `plan/delivery-plan.md` 已确认的 3 项任务执行，仅改 `main/src/`：

1. **抽出共用 GenieGhost**：将原先 `frameless_window.cpp` 匿名命名空间内的快照替身（`grab` 快照 + `suckInto` 吸入：20×20、InCubic、几何 220ms / 淡出 200ms、DPR 归一化绘制）抽到 header-only `main/src/genie_ghost.h`。`frameless_window.cpp` 与 `settings_dialog.cpp` 均 include 该头；已删除 cpp 内旧实现。未改 CMake。
2. **主窗口 `hideToTray()`**：`grab()` 后立刻 `show`/`raise`/`repaint` 替身盖住真窗口，再 `setWindowOpacity(0)`（几何不动），Windows 下调用 `DwmSetWindowAttribute(DWMWA_TRANSITIONS_FORCEDISABLED)` 后才 `hide()`，然后播替身吸入。`hide()` 发生在 ghost `show()` 之后。恢复显示透明度仍由现有 `showEvent` 弹出动画负责。
3. **设置窗 `reject()`**：改为同一套快照替身，不再对 `this` 做 `QPropertyAnimation(..., "geometry")`。动画期间只用 `setWindowOpacity(0)` 藏真窗、几何不动，不调用 `hide()`/`setVisible(false)`；替身 `destroyed` 后再 `QDialog::reject()`。`accept()` 未改。

### 改了哪些文件

- `main/src/genie_ghost.h`：新建，header-only 共用替身。
- `main/src/frameless_window.cpp`：删除匿名命名空间 GenieGhost；`hideToTray()` 改为先盖替身再透明/禁 DWM 过渡/`hide()`。
- `main/src/settings_dialog.cpp`：include 共用头；`reject()` 改替身吸入。
- `plan/execution-log.md`：追加本段。

未改：`frameless_window.h`、`settings_dialog.h`（接口无需变动）、`focus_timer_widget`、`main.cpp`、CMake。

### 未做事项 / 阻塞

- 未编译、未 `cmake --install`、未做截图回归（`-t 1/1.5/3`）与人工点 × 观感验收，由测试 Agent 负责。
- 无阻塞。

## 2026-08-19 交付执行 round 2 修复（关闭动画仍闪错乱布局）

### 做了什么

按用户复测 FAIL 做最小修复：

1. `genie_ghost.h`：拆成 `appearAt`（完整尺寸上屏并刷新合成器）与 `suckInto`（从记录的起点矩形吸入）；绘制改为 `drawPixmap(rect(), snapshot)`，避免 DPR=2 时三参数重载只画出左上 1/4。
2. `frameless_window.cpp`：构造时禁用 DWM 过渡；`hideToTray` 顺序改为 appearAt → hide → suckInto，去掉 `setWindowOpacity`（避免 HWND 被加成 layered 后闪帧）；`resizeEvent` 在 `m_hiding` 期间跳过短边回正。
3. `settings_dialog.cpp`：`reject()` 同样先 appearAt 再透明再吸入。

### 改了哪些文件

- `main/src/genie_ghost.h`
- `main/src/frameless_window.cpp`
- `main/src/settings_dialog.cpp`

### 未做事项 / 阻塞

- 点 × 观感需用户确认。截图回归交测试 Agent。
- 无阻塞。

## 2026-08-20 交付执行（中心植物生长 Vulkan）

### 做了什么

1. Seed 注册表 + PlantScene：`timeStageCount=5` → 6 视觉态；Y 向上 NDC；色板按计划（盆/土/芽/叶/干）。
2. CMake：`Vulkan QUIET`；仅 `Vulkan_FOUND` 且找到 `glslangValidator` 时定义 `HAS_VULKAN`、编 `vulkan_plant_renderer`、SPIR-V 输出到 `_build/main/shaders`、qrc `OBJECT_DEPENDS` 依赖 spv。无 SDK 仍能配置编译。
3. VulkanPlantRenderer：DEVICE_LOCAL OPTIMAL 附件、renderpass 后 barrier 到 TRANSFER_SRC、staging 回读、Y 翻转、预乘 ARGB32；进程内静态单例；resize 重建 FB。
4. FocusTimerWidget：圆心热区 accept（与 `drawRing` 同圆心/半径公式）；按下内圆后 move/release 不 ignore；阶段 6 QPainter 时间字；休息 `p=1`；交互 Vulkan 失败才 painter 回退。
5. frameless_window：`centerHit` 与播放/「...」一样排除拖拽。
6. 「...」菜单增加「选择种子」（先 close 再回调）；QSettings `centerMode`/`seedId`（截图模式先 `setTime` 再改模式，不写设置）。
7. CLI `-c plant` / `--seed`；Vulkan 失败退出码 2，写 `plant-vulkan-failed.txt`，不保存 PNG。

### 改了哪些文件

- `main/CMakeLists.txt`
- `main/src/main.cpp`
- `main/src/focus_timer_widget.h/.cpp`
- `main/src/frameless_window.cpp`（仅中心热区排除）
- 新增 `main/src/plant/seed.h`、`plant_scene.h/.cpp`、`painter_plant_renderer.h/.cpp`、`vulkan_plant_renderer.h/.cpp`
- 新增 `main/shaders/plant.vert`、`plant.frag`

### 编译自检

- `cmake -S . -B _build ...`：Vulkan SDK not found; building without HAS_VULKAN。
- `cmake --build _build --config Release` 通过。

### 未做事项 / 阻塞

- 本机无 Vulkan SDK：植物 Vulkan 截图视觉验收为环境前置，不由执行 Agent 安装 SDK。
- 完整验收（install、TimeText 回归、`-c plant` 失败码）交测试 Agent。

## 2026-08-20 安装 Vulkan SDK 后复测

### 做了什么

- 静默安装 LunarG Vulkan SDK **1.4.357.0** 到 `C:\VulkanSDK\1.4.357.0`（`VULKAN_SDK` 已写入系统环境）。
- 重新 configure：日志 `Vulkan found` + `glslangValidator`；编入 `vulkan_plant_renderer.cpp`；SPIR-V 编译成功。
- Release 构建 + 安装：`_install/main.exe` 更新为 143360 字节，依赖 `vulkan-1.dll`。

### 未做事项 / 阻塞

- 无代码改动。验收交测试 Agent round 2。
