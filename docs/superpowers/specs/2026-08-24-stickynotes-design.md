# StickyNotes（桌面便签）设计文档

日期：2026-08-24
类型：快速验证项目（一项目一文件夹）
技术栈：**Qt 6.4.2 Widgets / C++20 / CMake ≥3.22**

## 1. 背景与目标

在 Mine 工作空间新建一个仅面向 PC 桌面的便签应用 `StickyNotes`。目标是「快速验证」以下能力组合：无边框扁平便签 UI、多便签 dock/分开、边缘自动收齐、固定置顶、任务条勾选删除线。与 EasyPainter 同构：core（纯逻辑）/ app（GUI）/ cli（无头入口）三层，CLI + 离屏渲染作为全局硬约束。

## 2. 需求（回溯自用户第一版要求）

| # | 需求 | 验收标准（可度量） |
|---|---|---|
| R1 | 基础便签记录：标题、标题背景色、添加、删除、正文背景淡化 | 能新建/编辑标题、选标题色、删除便签；正文背景 = 标题色降低透明度，鼠标 hover 恢复不透明 |
| R2 | 固定/取消固定 | 点击 pin 按钮切换置顶（`Qt::WindowStaysOnTopHint`）；**仅置顶，靠近边缘仍收齐** |
| R3 | 无标题栏扁平 UI | `Qt::FramelessWindowHint`，自绘标题栏，无系统边框 |
| R4 | 靠近屏幕边缘自动收齐，鼠标靠近自动拉出（四边） | 窗口距屏幕任一上/下/左/右边缘 < 阈值且鼠标不在其上 → 收成细条标签（露出 ~8px）；鼠标进入标签区 → 展开回原几何 |
| R5 | 任务条：任务前按钮点击完成任务，划删除线 | 每行=勾选框+文本；勾选 → `QFont::StrikeOut` + 置灰；状态持久化 |
| R6 | 多便签可 dock 也可分开 | 每便签独立无边框窗口；拖动标题栏靠近另一便签 → 吸附成 `NoteGroup` 随组移动；可拆分回独立窗口 |

## 3. 全局硬约束（缺失即打回）

1. **CLI 模式**：必须有无头入口 `stickynotes-cli`，可脱离 GUI 交互运行（CRUD/序列化/渲染）。
2. **离屏渲染输出图像**：必须能离屏渲染并落盘 PNG（`-platform offscreen` + `QWidget::grab()`），供无头验收与 golden 对比。

## 4. 环境与依赖可行性（已探测）

| 项 | 结论 |
|---|---|
| Qt6 运行时 | `libqt6core6t64 / gui6t64 / widgets6t64 / opengl6t64` **已系统安装**（6.4.2+dfsg-21.1build5），含 `offscreen` platform plugin → 离屏渲染零额外运行期 |
| Qt6 dev 包 | `qt6-base-dev`、`qt6-base-dev-tools`（moc/uic/rcc）、`libqt6test6t64` 在 apt 镜像（aliyun）有 candidate → 走 `tools/install-user-deps.sh` 无 sudo `.deb` 递归解包链路（EasyPainter 已验证） |
| sudo | 需要密码，不可用 → 一律 `.user-deps` 部署 |
| 显示/输入 | `.user-deps` 已含 Xvfb 与 XTEST → 窗口化冒烟/鼠标临近验证可用 |
| CMake | 系统已有 `/usr/bin/cmake` |

## 5. 架构

### 5.1 模块分层

```
StickyNotes/
├── CMakeLists.txt / CMakePresets.json / deps.yaml
├── src/
│   ├── core/       # 仅 QtCore 依赖，纯逻辑，可独立单测
│   │   ├── model.h/.cpp        # Note / TaskItem 数据结构
│   │   ├── note_store.h/.cpp   # 增删改、pin/任务切换、变更信号
│   │   ├── persistence.h/.cpp  # JSON 序列化/反序列化（QJsonDocument）
│   │   ├── palette.h/.cpp      # 标题色 / 正文淡化色（透明度混合）
│   │   └── geometry_util.h/.cpp# 四边收齐判定 + 收齐/展开目标几何纯函数
│   ├── app/        # QtWidgets+Gui
│   │   ├── note_widget.h/.cpp  # 便签内容控件（标题行/正文/任务列表）
│   │   ├── note_window.h/.cpp  # 无边框便签窗（自绘标题栏 + 按钮）
│   │   ├── note_group.h/.cpp   # 多窗吸附组
│   │   ├── edge_dock_controller.h/.cpp # 边缘收齐/拉出
│   │   └── palette_dialog.h/.cpp       # 标题色选择
│   └── cli/
│       └── main.cpp            # stickynotes-cli（--render/--list/--add/--remove）
├── tests/
│   ├── model_test.cpp / store_test.cpp / persistence_test.cpp
│   ├── palette_test.cpp / geometry_test.cpp
│   ├── widget_test.cpp / image_golden_test.cpp
└── assets/ fonts/（Noto CJK，供中文渲染与 golden 稳定）
```

### 5.2 数据模型

```cpp
struct TaskItem { QString text; bool done = false; };
struct Note {
  QUuid id;
  QString title;
  QColor  titleColor;      // 标题背景色
  QString bodyText;        // 正文
  bool    pinned = false;
  QVector<TaskItem> tasks;
  QPointF pos;  QSizeF size;  // 窗口几何
};
```

- `NoteStore`：`QVector<Note>` + `QHash<QUuid,int>` 索引；增删改/pin/任务切换均发信号；`NoteStore` 变更 → app 防抖（~500ms QTimer）自动落盘 JSON。
- `persistence`：路径注入（测试用临时文件）；反序列化对损坏 JSON 返回错误而非崩溃。

### 5.3 需求 → 实现映射

- **R1**：标题行 `QLineEdit` + 标题色（`palette_dialog` 选色）；正文 `QTextEdit`；正文背景 = `titleColor` 降低透明度（alpha 降到 ~0.35，铺在默认底色上）；**hover 时恢复不透明**（`enterEvent`/`leaveEvent` 重绘）。新增/删除按钮在标题栏。
- **R2**：pin 按钮切换 `pinned` → `setWindowFlag(Qt::WindowStaysOnTopHint)`。仅置顶，收齐行为不受影响。
- **R3**：`FramelessWindowHint`；`mousePress/Move` 在标题区实现窗口拖动（`windowHandle()->startSystemMove()` 优先，失败回退 `move()`）。
- **R4**：`EdgeDockController`：定时器（~100ms）读 `QCursor::pos()` + 窗口几何；判定与目标几何由 `geometry_util` 纯函数给出；切换用 `QPropertyAnimation` 平滑。固定便签**仍收齐**。收齐成细条标签（仅标题色条，露出 ~8px），hover 展开。
- **R5**：任务行 = `QCheckBox` + `QLabel`；勾选 → 模型 `done=true` → 文本 `QFont::StrikeOut` + 置灰；持久化。
- **R6**：每便签独立顶层无边框窗；拖动标题栏，其边缘与另一便签边缘距离 < 阈值 → 吸附成 `NoteGroup`（组内随组整体移动，含 edge-dock 联动）；标题栏「拆合」按钮断开还原。

## 6. CLI + 离屏（硬约束实现）

- `stickynotes-cli --render <out.png>`：`-platform offscreen`，构造固定 fixture（含中文标题）→ 逐便签 `widget->resize(); widget->render(&img)` 或 `grab()` → 合并画到 `QImage` → 落盘 PNG。确定性输出。
- `--list`：打印便签数/标题/pin/任务完成数。`--add "<title>"` / `--remove <id>` / `--pin <id>`：无头操作 `NoteStore` 并持久化，供逻辑验收。
- CLI 二进制链接 core + widgets（render 需要 widget），但**无窗口依赖**（offscreen 平台）。

## 7. 测试策略（QtTest，`-platform offscreen`，全确定性）

| 用例 | 覆盖 | 判定 |
|---|---|---|
| model_test | Note/TaskItem 增删改 | 断言通过 |
| store_test | 增删、pin 切换、任务勾选信号、索引一致性 | 断言通过 |
| persistence_test | JSON 往返一致；损坏文件返回错误不崩溃；空 store 序列化 | 断言通过 |
| palette_test | 淡化色 alpha 计算、hover 不透明色 | 数值断言 |
| geometry_test | 四边收齐判定、收齐/展开目标几何（上/下/左/右） | 数值断言 |
| widget_test | 任务勾选→StrikeOut 属性；pin→窗口 flag；淡化→hover 色变化 | 断言通过 |
| image_golden_test | CLI 渲染 fixture → 与基准 PNG 逐像素比对（容差） | 像素一致 |

**图像 golden 约定**：基准 PNG 由 `stickynotes-cli --render` 在确定环境（Qt 6.4.2、offscreen、Noto CJK 字体）生成，人工确认外观后提交；测试比对该 PNG，容差按实测设定（不预设）。测试红若因字体缺字形 → 按 EasyPainter 的 bundled Noto CJK 方案补齐字体资源（非回退路径，属环境补齐）。

**窗口化冒烟**（Xvfb + XTEST 鼠标临近验证 edge-dock 拉出）放 `tools/` 脚本，**不进测试门**（避免 flaky），仅作人工验收辅助。

## 8. 风险与健壮性

| 风险 | 处理 |
|---|---|
| Qt dev 包下载 ~200MB | 无 sudo 链路已验证；失败按阻塞如实报告，不降级 |
| 中文/字体缺字形 | bundled Noto CJK（EasyPainter 同款），fixture 含中文标题验证 |
| golden 跨 Qt 版本漂移 | 锁定 6.4.2 + 容差；README 记录 golden 生成环境 |
| 损坏 JSON | persistence 返回错误 + 默认空 store，不崩溃 |
| offscreen 平台缺失 | 运行时已装，若仍缺按阻塞处理 |

## 9. 成功标准

1. core/序列化/收齐几何/调色板全单测通过。
2. CLI 离屏渲染 PNG 与 golden 一致。
3. 无边框窗口 flags 正确；pin 置顶；任务勾选显示删除线。
4. 四边收齐/拉出逻辑可测且通过。
5. 多便签吸附组/拆分逻辑可测且通过。

## 10. 明确不做（YAGNI）
- 不做回退/兜底路径（替代库/降级跳过）。
- 不做云同步、换肤系统、多显示器增强收齐、通知。
- 不做窗口级持久化的多显示器归属（单屏几何即可）。
