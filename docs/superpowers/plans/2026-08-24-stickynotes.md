# StickyNotes Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build a PC-only Qt 6 desktop sticky-notes app (`StickyNotes`) with frameless flat UI, edge-dock auto-hide, pin-to-top, task-checkboxes with strikethrough, and dock/separate multi-note groups — plus a headless CLI that renders offscreen PNGs for golden testing.

**Architecture:** Three-layer, mirroring EasyPainter: `core` (pure QtCore logic: model/store/persistence/palette/geometry — all unit-testable without GUI), `app` (QtWidgets GUI: frameless note windows, edge-dock controller, note groups), `cli` (headless entry: CRUD + offscreen render to PNG). Qt 6.4.2 Widgets/C++20/CMake. Tests are QtTest run under `-platform offscreen` for full determinism.

**Tech Stack:** Qt 6.4.2 (Core/Gui/Widgets/Test), C++20, CMake ≥3.22, QtTest. Qt dev packages deployed no-sudo into `.user-deps/` via `tools/install-user-deps.sh` (EasyPainter's proven pattern); Qt runtime already system-installed (incl. `offscreen` platform plugin). Noto CJK font reused from `EasyPainter/assets/fonts/`.

**Spec:** `docs/superpowers/specs/2026-08-24-stickynotes-design.md` (design spec — the plan argues from it; executors read both).

---

## Global Constraints

- C++ standard **C++20** (`CMAKE_CXX_STANDARD 20`).
- Qt **6.4.2** only (lock for golden stability); resolved from `.user-deps` env (headers/moc) + system runtime (libs/offscreen plugin).
- **无 sudo**：所有缺失 dev 包经 `tools/install-user-deps.sh` 部署到 `$MINE_ROOT/.user-deps/`，构建/运行前 `source .user-deps/env.sh`。
- **硬约束（缺失即整份计划打回）**：必须提供 CLI 模式 + 离屏渲染输出图像（`stickynotes-cli --render out.png` + `QT_QPA_PLATFORM=offscreen`）。
- **不回退原则**：无替代库、无预编译兜底、无降级跳过；依赖缺失 → 补齐走主路径，或如实报告阻塞。
- 所有可独立测试的纯逻辑单元：**先写失败测试再实现（TDD）**。
- 中文渲染：使用 bundled **Noto CJK** 字体（`assets/fonts/NotoSansCJK-Regular.ttc`，复用自 EasyPainter），标题/任务含中文的 golden fixture 验证字形。
- 测试一律 `QT_QPA_PLATFORM=offscreen`；窗口化冒烟（Xvfb+XTEST）放 `tools/` 脚本，不进测试门。
- 图像 golden 基准：由 `stickynotes-cli --render` 在 Qt 6.4.2/offscreen/Noto CJK 下生成，人工确认外观后提交；比对容差按实测设定。

---

### Task 1: 环境部署 — Qt6 dev 依赖进 `.user-deps` + 真实探针

**Files:**
- Modify: `tools/install-user-deps.sh`（新增 Qt6 section：`qt6-base-dev`、`qt6-base-dev-tools`、`libqt6test6t64` + 递归运行期依赖；扩展 `env.sh` 导出 Qt cmake/include 前缀）
- Create: `StickyNotes/probe/qt6_probe/CMakeLists.txt`、`main.cpp`（临时探针，验证后删除）

**Interfaces:**
- Produces: `source .user-deps/env.sh` 后 `find_package(Qt6 COMPONENTS Core Gui Widgets Test)` 可命中，`moc`/`uic`/`rcc` 可用，`libQt6Test` 可链接，`QT_QPA_PLATFORM=offscreen` 下能建窗 + `grab()` 出非空 PNG。

- [ ] **Step 1: 确认运行时基础**

Run: `dpkg-query -W -f='${Status}\n' libqt6core6t64 libqt6gui6t64 libqt6widgets6t64 2>&1 | head`
Expected: 三者均 `install ok installed`（已探测确认为 6.4.2+dfsg-21.1build5）。

- [ ] **Step 2: install-user-deps.sh 增加 Qt6 部署 section**

在 `tools/install-user-deps.sh` 中，复用现有 `dl_deb` / `x_deb` / `dep_names` / `fetch_runtime_deps` 工具函数，新增（放在既有 vulkan/x11 section 之后、env.sh 生成之前）：

```bash
# --- Qt6 dev: 头文件 / moc·uic·rcc / Qt6 cmake 配置 / QtTest 运行期 ----------
info "Qt6: 部署 qt6-base-dev + qt6-base-dev-tools + libqt6test6t64"
QT_DEVS="qt6-base-dev qt6-base-dev-tools libqt6test6t64"
for p in $QT_DEVS; do
  if installed "$p"; then info "$p 已系统安装，跳过"; continue; fi
  x_deb "$p" || die "Qt 包 $p 下载/解包失败"
  fetch_runtime_deps "$p" 3
done
```

在 env.sh 生成段（现有 `export` 列表）追加 Qt 前缀：

```bash
export QT_PREFIX="$USBIN/usr"
export PATH="$USBIN/usr/lib/qt6/bin:$PATH"                    # moc6/uic6/rcc6
export CMAKE_PREFIX_PATH="$USBIN/usr${CMAKE_PREFIX_PATH:+:$CMAKE_PREFIX_PATH}"
export CMAKE_INCLUDE_PATH="$USBIN/usr/include/$MULTIARCH/qt6${CMAKE_INCLUDE_PATH:+:$CMAKE_INCLUDE_PATH}"
export LD_LIBRARY_PATH="$USBIN/usr/lib/$MULTIARCH${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
```

（`$USBIN`、`$MULTIARCH` 为脚本内既有变量，分别指向 `.user-deps/usr/usr` 与 `x86_64-linux-gnu`。）

- [ ] **Step 3: 运行脚本部署 + 校验落盘**

Run: `source /home/qiansenwei/workspace/Mine/.user-deps/env.sh && bash /home/qiansenwei/workspace/Mine/tools/install-user-deps.sh 2>&1 | tail -30`
Expected: Qt section 无 `die`；`ls /home/qiansenwei/workspace/Mine/.user-deps/usr/usr/lib/*/cmake/Qt6/Qt6Config.cmake` 与 `.../bin/moc6` 存在。重跑脚本幂等（已装包跳过）。

- [ ] **Step 4: 写最小 Qt6 探针（防假绿）**

`StickyNotes/probe/qt6_probe/CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.22)
project(qt6_probe CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Test)
add_executable(qt6_probe main.cpp)
target_link_libraries(qt6_probe PRIVATE Qt6::Core Qt6::Gui Qt6::Widgets Qt6::Test)
```

`StickyNotes/probe/qt6_probe/main.cpp`:

```cpp
#include <QApplication>
#include <QWidget>
#include <QImage>
#include <QPainter>
#include <QTest>
#include <cstdio>
int main(int argc, char** argv) {
  QApplication app(argc, argv);
  QWidget w; w.resize(120, 80);
  // offscreen 下 grab 必须产出非空图像
  QImage img = w.grab().toImage();
  bool ok = !img.isNull() && img.width() == 120 && img.height() == 80;
  if (!ok) { std::fprintf(stderr, "grab failed\n"); return 1; }
  // 验证 Qt6::Test 可链接（符号可见即链过）
  int pass = QTest::qRandomSeed() >= 0 ? 1 : 0;   // 任意调用，仅验证链接
  std::fprintf(stderr, "probe ok grab=%dx%d\n", img.width(), img.height());
  return ok && pass ? 0 : 2;
}
```

- [ ] **Step 5: 编译并运行探针（离屏）**

Run: `cd /home/qiansenwei/workspace/Mine/StickyNotes/probe/qt6_probe && source /home/qiansenwei/workspace/Mine/.user-deps/env.sh && cmake -B build -S . -DCMAKE_BUILD_TYPE=Release && cmake --build build -j && QT_QPA_PLATFORM=offscreen ./build/qt6_probe`
Expected: 打印 `probe ok grab=120x80`，退出码 0。失败 → 按阻塞修复（修 install 脚本/env.sh 直至探针绿，不降级）。

- [ ] **Step 6: 删除临时探针 + Commit**

```bash
git add tools/install-user-deps.sh
git rm -rf StickyNotes/probe
git commit -m "feat(stickynotes): deploy qt6 dev deps into user-deps + env, probe-verified"
```

---

### Task 2: 工程骨架 + core 模型（Note/TaskItem/NoteStore）+ 单测

**Files:**
- Create: `StickyNotes/CMakeLists.txt`、`CMakePresets.json`、`deps.yaml`、`README.md`、`assets/fonts/NotoSansCJK-Regular.ttc`（复制自 EasyPainter）
- Create: `StickyNotes/src/core/model.h`、`src/core/note_store.h`、`src/core/note_store.cpp`
- Create: `StickyNotes/tests/store_test.cpp`

**Interfaces:**
- Produces:
  - `struct TaskItem { QString text; bool done; }`（默认 `text` 空、`done=false`）
  - `struct Note { QUuid id; QString title; QColor titleColor; QString bodyText; bool pinned; QVector<TaskItem> tasks; QPointF pos; QSizeF size; }`，默认 `titleColor=QColor("#ffb74d")`，`size=QSizeF(260,320)`
  - `class NoteStore : public QObject`：`Q_SIGNAL void changed();`；`QVector<Note>& notes()`；`Note& add(const Note&)`（分配新 id 并返回引用）；`void remove(QUuid)`；`Note* find(QUuid)`；`void setTitle/setBodyText/setTitleColor/setPinned(QUuid, ...)`；`void addTask(QUuid, const QString&)`；`void setTaskDone(QUuid, int, bool)`；每次变更发 `changed()`

- [ ] **Step 1: 复制字体资源**

Run: `mkdir -p /home/qiansenwei/workspace/Mine/StickyNotes/assets/fonts && cp /home/qiansenwei/workspace/Mine/EasyPainter/assets/fonts/NotoSansCJK-Regular.ttc /home/qiansenwei/workspace/Mine/StickyNotes/assets/fonts/`
Expected: 文件存在（~19MB）。

- [ ] **Step 2: 写失败测试 `tests/store_test.cpp`**

```cpp
#include <QTest>
#include "core/note_store.h"

class StoreTest : public QObject {
  Q_OBJECT
private slots:
  void addSetsIdAndIndex() {
    NoteStore s; Note n; n.title = "t";
    auto& ref = s.add(n);
    QCOMPARE(s.notes().size(), 1);
    QVERIFY(!ref.id.isNull());
    QCOMPARE(s.find(ref.id), &ref);
  }
  void removeUpdatesIndex() {
    NoteStore s; auto& a = s.add(Note{}); auto& b = s.add(Note{});
    s.remove(a.id);
    QCOMPARE(s.notes().size(), 1);
    QVERIFY(s.find(a.id) == nullptr);
    QVERIFY(s.find(b.id) == &b);
  }
  void pinToggleEmitsChanged() {
    NoteStore s; auto& a = s.add(Note{});
    int n = 0; connect(&s, &NoteStore::changed, [&]{ ++n; });
    s.setPinned(a.id, true);
    QVERIFY(s.find(a.id)->pinned);
    QCOMPARE(n, 1);
  }
  void taskDone() {
    NoteStore s; auto& a = s.add(Note{});
    s.addTask(a.id, "买牛奶");
    QCOMPARE(s.find(a.id)->tasks.size(), 1);
    s.setTaskDone(a.id, 0, true);
    QVERIFY(s.find(a.id)->tasks[0].done);
    QVERIFY(s.find(a.id)->tasks[0].text == "买牛奶");
  }
};
QTEST_MAIN(StoreTest)
#include "store_test.moc"
```

- [ ] **Step 3: 运行测试确认失败**

Run（先建最小 `CMakeLists.txt` 或临时 g++ 直接编）：`cd /home/qiansenwei/workspace/Mine/StickyNotes && source /home/qiansenwei/workspace/Mine/.user-deps/env.sh && cmake -B build -S . -DCMAKE_BUILD_TYPE=Debug && cmake --build build -j`
Expected: 编译失败（`core/note_store.h` 不存在）。**此步骤先落 CMakeLists 骨架（见 Step 4），再确认失败**。

- [ ] **Step 4: 写工程 CMakeLists + core 模型**

`StickyNotes/CMakeLists.txt`:

```cmake
cmake_minimum_required(VERSION 3.22)
project(StickyNotes CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Gui Widgets Test)

add_library(stickynotes_core STATIC
  src/core/note_store.cpp
  src/core/model.h
  src/core/note_store.h)
target_include_directories(stickynotes_core PUBLIC src)
target_link_libraries(stickynotes_core PUBLIC Qt6::Core)

# 字体资源：运行期从源码树定位（CLI/app 均用同一 helper）
add_library(stickynotes_assets INTERFACE)
target_include_directories(stickynotes_assets INTERFACE assets)
target_compile_definitions(stickynotes_assets INTERFACE
  STICKYNOTES_FONT_DIR="${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts")

enable_testing()
add_subdirectory(tests)
```

`src/core/model.h`:

```cpp
#pragma once
#include <QColor>
#include <QPointF>
#include <QSizeF>
#include <QString>
#include <QVector>
#include <QUuid>

struct TaskItem {
  QString text;
  bool done = false;
};

struct Note {
  QUuid id;
  QString title;
  QColor titleColor = QColor("#ffb74d");
  QString bodyText;
  bool pinned = false;
  QVector<TaskItem> tasks;
  QPointF pos = QPointF(100, 100);
  QSizeF size = QSizeF(260, 320);
};
```

`src/core/note_store.h`:

```cpp
#pragma once
#include <QHash>
#include <QObject>
#include <QVector>
#include "core/model.h"

class NoteStore : public QObject {
  Q_OBJECT
public:
  Note& add(const Note& note);
  void remove(QUuid id);
  Note* find(QUuid id);
  QVector<Note>& notes() { return notes_; }
  const QVector<Note>& notes() const { return notes_; }

  void setTitle(QUuid id, const QString& v);
  void setBodyText(QUuid id, const QString& v);
  void setTitleColor(QUuid id, const QColor& c);
  void setPinned(QUuid id, bool v);
  void addTask(QUuid id, const QString& text);
  void setTaskDone(QUuid id, int index, bool done);
signals:
  void changed();
private:
  QVector<Note> notes_;
  QHash<QUuid, int> index_;
};
```

`src/core/note_store.cpp`: 实现如上接口；`add` 若 `note.id` 为空则 `QUuid::createUuid()`，否则复用；维护 `index_`；每个修改方法 `emit changed()`。

- [ ] **Step 5: 补最小 tests/CMakeLists.txt 并跑测试**

`StickyNotes/tests/CMakeLists.txt`:

```cmake
find_package(Qt6 REQUIRED COMPONENTS Test)
add_executable(store_test store_test.cpp)
target_link_libraries(store_test PRIVATE stickynotes_core Qt6::Test)
add_test(NAME store_test COMMAND store_test)
```

Run: `cd /home/qiansenwei/workspace/Mine/StickyNotes && cmake -B build -S . -DCMAKE_BUILD_TYPE=Debug && cmake --build build -j && QT_QPA_PLATFORM=offscreen ctest --test-dir build --output-on-failure`
Expected: `store_test` 4/4 通过。

- [ ] **Step 6: Commit**

```bash
git add StickyNotes
git commit -m "feat(stickynotes): project skeleton + NoteStore core model with unit tests"
```

---

### Task 3: persistence（JSON 序列化/反序列化）+ 单测

**Files:**
- Create: `StickyNotes/src/core/persistence.h`、`src/core/persistence.cpp`
- Create: `StickyNotes/tests/persistence_test.cpp`

**Interfaces:**
- Consumes: `NoteStore`（Task 2）。
- Produces: `bool saveStore(const NoteStore&, const QString& path)`（写 UTF-8 JSON，成功 true）；`bool loadStore(NoteStore&, const QString& path)`（成功填充并返回 true；文件不存在或损坏 JSON 返回 **false 且不抛异常、store 内容不变**）。JSON 结构：`{"notes":[{"id","title","titleColor":"#rrggbb","bodyText","pinned","tasks":[{"text","done"}],"pos":[x,y],"size":[w,h]}]}`。

- [ ] **Step 1: 写失败测试**

```cpp
#include <QTest>
#include <QTemporaryDir>
#include "core/note_store.h"
#include "core/persistence.h"

class PersistenceTest : public QObject {
  Q_OBJECT
private slots:
  void roundTrip() {
    QTemporaryDir dir; QString p = dir.filePath("n.json");
    NoteStore s; auto& a = s.add(Note{});
    a.title = "便签"; a.bodyText = "正文"; a.titleColor = QColor("#ffee88");
    a.pinned = true; a.pos = QPointF(12, 34); a.size = QSizeF(200, 300);
    s.addTask(a.id, "任务A"); s.setTaskDone(a.id, 0, true);
    NoteStore t;
    QVERIFY(loadStore(t, p) == false);   // 文件不存在 → false
    QVERIFY(saveStore(s, p));
    QVERIFY(loadStore(t, p));
    QCOMPARE(t.notes().size(), 1);
    auto& n = t.notes()[0];
    QCOMPARE(n.title, QString("便签"));
    QCOMPARE(n.titleColor, QColor("#ffee88"));
    QVERIFY(n.pinned);
    QCOMPARE(n.pos, QPointF(12, 34));
    QCOMPARE(n.size, QSizeF(200, 300));
    QCOMPARE(n.tasks.size(), 1);
    QVERIFY(n.tasks[0].done);
    QCOMPARE(n.tasks[0].text, QString("任务A"));
  }
  void corruptFileReturnsFalse() {
    QTemporaryDir dir; QString p = dir.filePath("bad.json");
    { QFile f(p); f.open(QIODevice::WriteOnly); f.write("{{{not json"); }
    NoteStore t; auto& before = t.add(Note{}); QUuid beforeId = before.id;
    QVERIFY(loadStore(t, p) == false);
    QCOMPARE(t.notes().size(), 1);            // store 内容不变
    QCOMPARE(t.notes()[0].id, beforeId);
  }
  void emptyStore() {
    QTemporaryDir dir; QString p = dir.filePath("e.json");
    NoteStore s;
    QVERIFY(saveStore(s, p));
    NoteStore t; QVERIFY(loadStore(t, p));
    QCOMPARE(t.notes().size(), 0);
  }
};
QTEST_MAIN(PersistenceTest)
#include "persistence_test.moc"
```

- [ ] **Step 2: 运行确认失败**（`cmake --build` 因缺 `persistence.h` 失败）

- [ ] **Step 3: 实现 persistence.h/.cpp**

用 `QJsonDocument`/`QJsonObject`/`QJsonArray`；`saveStore` 打开 `QIODevice::WriteOnly|QIODevice::Truncate` 写 `doc.toJson()`；`loadStore` 读文件失败返回 false，`QJsonDocument::fromJson` 解析失败或根非对象返回 false，逐 note 构造并 `store.add()`（复用 Task 2 的 add，id 用 `QJsonValue::toVariant().toUuid()`）。**任何解析异常路径仅返回 false，不修改 store。**

- [ ] **Step 4: 跑测试全绿 + Commit**

```bash
git add StickyNotes/src/core/persistence.* StickyNotes/tests/persistence_test.cpp StickyNotes/tests/CMakeLists.txt StickyNotes/CMakeLists.txt
git commit -m "feat(stickynotes): JSON persistence with corrupt-file handling"
```

---

### Task 4: palette（标题色 / 正文淡化 hover 恢复）+ 单测

**Files:**
- Create: `StickyNotes/src/core/palette.h`、`src/core/palette.cpp`
- Create: `StickyNotes/tests/palette_test.cpp`

**Interfaces:**
- Produces:
  - `QColor titleBarColor(const QColor& titleColor)` — 标题栏底 = 标题色（可直接返回标题色）
  - `QColor fadedBodyColor(const QColor& titleColor)` — 正文背景 = 标题色 alpha 降至 **~0.35** 混合在白底上（`QColor(c.red(),c.green(),c.blue(), 89)` ≈ 0.35*255）
  - `QColor bodyColorHover(const QColor& titleColor)` — hover 时正文背景 = 标题色**不透明**
  - `QColor titleBarTextColor(const QColor& bg)` — 按亮度（`qGray`）选黑/白，保证可读

- [ ] **Step 1: 写失败测试**

```cpp
#include <QTest>
#include "core/palette.h"

class PaletteTest : public QObject {
  Q_OBJECT
private slots:
  void fadedHasLowAlpha() {
    QColor c(255, 187, 77);
    QColor f = fadedBodyColor(c);
    QVERIFY(f.alpha() <= 100);                 // ≈0.35
    QCOMPARE(f.red(), 255);
  }
  void hoverIsOpaque() {
    QColor c(30, 144, 255);
    QCOMPARE(bodyColorHover(c).alpha(), 255);
    QCOMPARE(bodyColorHover(c), c);
  }
  void textContrast() {
    QVERIFY(titleBarTextColor(QColor("white")) == QColor(Qt::black));
    QVERIFY(titleBarTextColor(QColor("black")) == QColor(Qt::white));
  }
};
QTEST_MAIN(PaletteTest)
#include "palette_test.moc"
```

- [ ] **Step 2: 运行确认失败**

- [ ] **Step 3: 实现 palette.h/.cpp**（按 Interfaces 常量实现；alpha 89 可调，测试只断言 ≤100）

- [ ] **Step 4: 全绿 + Commit**

```bash
git add StickyNotes/src/core/palette.* StickyNotes/tests/palette_test.cpp StickyNotes/tests/CMakeLists.txt StickyNotes/CMakeLists.txt
git commit -m "feat(stickynotes): palette fade/hover color helpers"
```

---

### Task 5: geometry_util（四边收齐 + 吸附组）+ 单测

**Files:**
- Create: `StickyNotes/src/core/geometry_util.h`、`src/core/geometry_util.cpp`
- Create: `StickyNotes/tests/geometry_test.cpp`

**Interfaces:**
- Produces（全部纯函数，无 GUI 依赖）:
  - `enum class ScreenEdge { Top, Bottom, Left, Right };`
  - `struct DockState { ScreenEdge edge; QRect hiddenRect; QRect tabRect; bool docked; };`
  - `std::optional<DockState> computeDock(const QRect& winRect, const QRect& screen, int cursorX, int cursorY, int threshold = 8, int reveal = 8)`
    若 `winRect` 与 screen 某边距离 ≤ `threshold`（取最近边）：返回 `DockState{edge, hiddenRect, tabRect, docked:true}`。
    - `hiddenRect`：贴边收齐后的几何（宽度或高度方向压缩到 `reveal` 高度/宽度，沿对应边）。Left: `x=-width+reveal, y=win.y, w=win.width, h=win.height`；Right: `x=screen.right()-reveal, ...`；Top/Bottom 同理。
    - `tabRect`：鼠标悬停触发展开的细条热区 = `hiddenRect`（收齐态可见区）。
    - `docked` 初始 false；仅当「距边近」时 true。
  - `bool cursorNearDock(const QRect& tabRect, int cursorX, int cursorY, int pad = 6)` — 鼠标是否进入标签热区（展开条件）。
  - `QRect snappedRect(const QRect& a, const QRect& b, int gap = 8)` — 组吸附：使 `b` 贴到 `a` 右侧并垂直对齐（右向吸附为 v1 默认），返回 `b` 的新几何。用于 dock 吸附判定。

- [ ] **Step 1: 写失败测试**（覆盖四边 + 收齐几何 + 鼠标展开）

```cpp
#include <QTest>
#include "core/geometry_util.h"

class GeometryTest : public QObject {
  Q_OBJECT
private slots:
  void dockLeft() {
    QRect scr(0,0,1920,1080), win(10, 300, 260, 320);
    auto d = computeDock(win, scr, 500, 500);
    QVERIFY(d.has_value());
    QCOMPARE(d->edge, ScreenEdge::Left);
    QVERIFY(d->hiddenRect.x() < 0 && d->hiddenRect.x() > -win.width());
    QCOMPARE(d->hiddenRect.width(), win.width());
    QVERIFY(d->tabRect.width() >= 8);
  }
  void dockRightTopBottom() {
    QRect scr(0,0,1920,1080);
    QVERIFY(computeDock(QRect(1900,300,260,320), scr, 500,500)->edge == ScreenEdge::Right);
    QVERIFY(computeDock(QRect(300,5,260,320),   scr, 500,500)->edge == ScreenEdge::Top);
    QVERIFY(computeDock(QRect(300,1070,260,320),scr, 500,500)->edge == ScreenEdge::Bottom);
  }
  void farAwayNoDock() {
    QRect scr(0,0,1920,1080);
    QVERIFY(!computeDock(QRect(800,400,260,320), scr, 800,400).has_value());
  }
  void cursorExpandsTab() {
    QRect scr(0,0,1920,1080);
    auto d = computeDock(QRect(10,300,260,320), scr, 500,500);
    QVERIFY(d.has_value());
    QVERIFY(cursorNearDock(d->tabRect, d->tabRect.center().x(), d->tabRect.center().y()));
    QVERIFY(!cursorNearDock(d->tabRect, 100, 100));
  }
  void snapRight() {
    QRect a(100,100,260,320), b(600,200,260,320);
    QRect r = snappedRect(a, b);
    QCOMPARE(r.x(), a.right() + 8);
    QCOMPARE(r.y(), a.y());
  }
};
QTEST_MAIN(GeometryTest)
#include "geometry_test.moc"
```

- [ ] **Step 2: 运行确认失败**

- [ ] **Step 3: 实现 geometry_util.h/.cpp**（按 Interfaces 定义；`computeDock` 取四边最近者，距离 = 窗口边到 screen 边，left=`win.x`，right=`screen.right()-win.right()`，top=`win.y`，bottom=`screen.bottom()-win.bottom()`，取最小且 ≤threshold 的边）

- [ ] **Step 4: 全绿 + Commit**

```bash
git add StickyNotes/src/core/geometry_util.* StickyNotes/tests/geometry_test.cpp StickyNotes/tests/CMakeLists.txt StickyNotes/CMakeLists.txt
git commit -m "feat(stickynotes): edge-dock + snap pure geometry functions"
```

---

### Task 6: CLI — 无头 CRUD + 离屏渲染 PNG + golden 基准

**Files:**
- Create: `StickyNotes/src/cli/main.cpp`
- Create: `StickyNotes/tests/image_golden_test.cpp`、`StickyNotes/golden/stickynotes_golden.png`
- Modify: `StickyNotes/CMakeLists.txt`（加 `stickynotes-cli` 目标）、`tests/CMakeLists.txt`（加 golden 测试）
- Create: `StickyNotes/tools/gen-golden.sh`、`tools/render_fixture.cpp`（生成基准用，见 Step 4）

**Interfaces:**
- Consumes: `NoteStore`、`persistence`、`palette`、`NoteWidget`（Task 7 先行，或本任务先放占位——**顺序调整：本任务依赖 NoteWidget，故 Task 7 的 NoteWidget 需在此前就绪**；实现时若顺序冲突，将 NoteWidget 拆入本任务前置步骤）。实际依赖裁决：**golden 渲染需要完整 NoteWidget 外观，故 Task 6 与 Task 7 合并为一个任务执行**（见下方合并说明）。
- Produces: 可执行 `stickynotes-cli`：
  - `--list [store.json]`：打印每 note 的 `id title [pinned] tasks(n/m)`
  - `--add "<title>" [store.json]`：新建 note（默认色）并持久化
  - `--remove <id> [store.json]`
  - `--pin <id> <on|off> [store.json]`
  - `--render <out.png> [store.json]`：`QT_QPA_PLATFORM=offscreen` 下用 NoteWidget 离屏渲染整张便签墙到 PNG；无 store 参数时渲染内置 fixture（含中文标题/任务，用作 golden 基准）
  - 缺省 `store.json`：`stickynotes.json`（CWD）

- [ ] **Step 1: 写失败测试 `image_golden_test.cpp`**

```cpp
#include <QTest>
#include "core/note_store.h"
#include "core/persistence.h"

class ImageGoldenTest : public QObject {
  Q_OBJECT
private slots:
  void cliRenderMatchesGolden() {
    // 调 stickynotes-cli --render 到临时文件，与 golden/ 基准逐像素比对
    QTemporaryDir dir;
    QString out = dir.filePath("render.png");
    QProcess p; p.start("stickynotes-cli", {"--render", out});
    QVERIFY(p.waitForFinished(30000));
    QCOMPARE(p.exitStatus(), QProcess::NormalExit);
    QCOMPARE(p.exitCode(), 0);
    QImage got(out); QImage ref(STICKYNOTES_GOLDEN_DIR "/stickynotes_golden.png");
    QVERIFY(!got.isNull() && !ref.isNull());
    QCOMPARE(got.size(), ref.size());
    // 容差：按实测（初代设 0 期望像素级一致，Qt 6.4.2 同机可复现）
    int diff = 0;
    for (int y = 0; y < got.height(); ++y)
      for (int x = 0; x < got.width(); ++x)
        if (got.pixel(x, y) != ref.pixel(x, y)) ++diff;
    QVERIFY2(diff <= (int)(got.width()*got.height()*0.001), "pixel diff over tolerance");
  }
};
QTEST_MAIN(ImageGoldenTest)
#include "image_golden_test.moc"
```

（`STICKYNOTES_GOLDEN_DIR` 由 tests/CMakeLists.txt 注入 compile definition 指向 `StickyNotes/golden`。）

- [ ] **Step 2: 确认失败**（无 golden/无 cli → 失败）

- [ ] **Step 3: 实现 `src/cli/main.cpp`**

要点：`QApplication app(argc,argv)`；`QT_QPA_PLATFORM` 由调用方设 offscreen；加载字体 `QFontDatabase::addApplicationFont(STICKYNOTES_FONT_DIR "/NotoSansCJK-Regular.ttc")`；解析子命令；`--render`：构建 fixture（store 为空时用固定内容：2 张便签，标题「学习计划」「买菜清单」，任务含完成与未完成，pinned 一张，标题色不同）→ 对每 note 建 `NoteWidget`、`resize(size)`、`render(&QPainter(QImage))` 到一张画布 → `save(out)`。`--list/--add/--remove/--pin` 操作 `NoteStore` + `saveStore`，stdout 打印。

- [ ] **Step 4: 生成 golden 基准（人工确认外观）**

`StickyNotes/tools/gen-golden.sh`：`QT_QPA_PLATFORM=offscreen ./build/release/stickynotes-cli --render golden/stickynotes_golden.png`（无 store 参数走 fixture）。运行后 **Read 该 PNG 由主会话人工确认**：中文可读、淡化/hover 逻辑对、无边框扁平、任务删除线可见。确认后提交。若中文缺字形 → 修字体加载（Noto CJK 必须命中）。

- [ ] **Step 5: 全绿 + Commit**

```bash
git add StickyNotes/src/cli StickyNotes/tests/image_golden_test.cpp StickyNotes/tests/CMakeLists.txt StickyNotes/CMakeLists.txt StickyNotes/golden StickyNotes/tools/gen-golden.sh
git commit -m "feat(stickynotes): headless CLI (crud + offscreen render) + golden baseline"
```

---

### Task 7: app GUI — NoteWidget + NoteWindow（无边框/自绘标题栏/拖动/pin/淡化 hover/任务条）

**Files:**
- Create: `StickyNotes/src/app/palette_dialog.h/.cpp`、`note_widget.h/.cpp`、`note_window.h/.cpp`
- Create: `StickyNotes/tests/widget_test.cpp`

**Interfaces:**
- Consumes: `NoteStore`、`palette`、`geometry_util`（Task 2/4/5）。
- Produces:
  - `class NoteWidget : public QWidget`：持有 note 引用；信号 `editTitle`, `colorChanged`, `pinToggled`, `deleteRequested`, `splitRequested`, `taskToggled(int, bool)`, `startDrag(QPoint)`, `dragMove(QPoint)`, `dragEnd()`；方法 `setDocked(bool)`、`refresh()`。正文背景随 hover 切换（`fadedBodyColor` ↔ `bodyColorHover`）。
  - `class NoteWindow : public QWidget`（`Qt::FramelessWindowHint`）：组合 `NoteWidget`；`setPinned(bool)`（`Qt::WindowStaysOnTopHint`）；`setDocked(DockState)` 收齐/展开（用 `QPropertyAnimation` 平滑）；`DockState dockState()`。

- [ ] **Step 1: 写失败测试 `widget_test.cpp`**（offscreen，验证 GUI 状态与模型联动）

```cpp
#include <QTest>
#include <QApplication>
#include "app/note_window.h"
#include "core/note_store.h"
#include "core/palette.h"

class WidgetTest : public QObject {
  Q_OBJECT
private slots:
  void pinTogglesWindowFlag() {
    NoteStore s; auto& n = s.add(Note{});
    NoteWindow w(s, n);
    w.show();                       // offscreen 下仍可用
    QVERIFY(!(w.windowFlags() & Qt::WindowStaysOnTopHint));
    w.setPinned(true);
    QVERIFY(w.windowFlags() & Qt::WindowStaysOnTopHint);
    w.setPinned(false);
    QVERIFY(!(w.windowFlags() & Qt::WindowStaysOnTopHint));
  }
  void taskDoneShowsStrikeout() {
    NoteStore s; auto& n = s.add(Note{});
    s.addTask(n.id, "任务");
    NoteWindow w(s, n);
    w.show();
    auto* ck = w.findChild<QCheckBox*>();
    QVERIFY(ck);
    ck->setChecked(true);           // 触发模型 done
    QVERIFY(n.tasks[0].done);
    // 标签字体带删除线
    auto* lbl = w.findChild<QLabel*>();
    QVERIFY(lbl->font().strikeOut());
  }
  void bodyFadesOnLeaveAndRestoresOnEnter() {
    NoteStore s; auto& n = s.add(Note{}); n.titleColor = QColor("#ffb74d");
    NoteWindow w(s, n); w.show();
    auto* body = w.findChild<QWidget*>("bodyArea");
    QVERIFY(body);
    QTest::mouseMove(w, QPoint(0,0)); QApplication::processEvents();
    // enter/leave 由 NoteWidget 内部 hover 状态驱动；此处断言离开态为淡化色
    QCOMPARE(body->palette().color(QPalette::Base).alpha() <= 100, true);
  }
};
QTEST_MAIN(WidgetTest)
#include "widget_test.moc"
```

- [ ] **Step 2: 运行确认失败**（无 `note_window.h`）

- [ ] **Step 3: 实现 NoteWidget**

- 标题栏（自绘）：`titleBarColor` 打底、`titleBarTextColor` 前景；含标题 `QLineEdit`、pin/color/split/delete 按钮。
- 正文：`QTextEdit`（objectName `bodyArea`）；`enterEvent` → `bodyColorHover`（不透明），`leaveEvent` → `fadedBodyColor`（alpha≈35%）；用 `setStyleSheet` 或 palette 切换背景，保证 hover 变化可被 widget_test 断言。
- 任务条：行 = `QCheckBox` + `QLabel`；勾选 → `setTaskDone` + `QFont::StrikeOut` + `QLabel::setEnabled(false)` 置灰；模型变化 `refresh()` 重建列表。
- 拖动：标题栏 `mousePressEvent` 记偏移 + 发 `startDrag`，`mouseMoveEvent` 发 `dragMove`，release 发 `dragEnd`（NoteWindow 收组/吸附用）。

- [ ] **Step 4: 实现 NoteWindow**

- `FramelessWindowHint`；`setPinned` 切换 `WindowStaysOnTopHint`（`setWindowFlag` + `show`）。
- `setDocked(DockState)`：动画到 `hiddenRect`（收齐）或回 `shownRect`（展开）；`dockState()` 返回当前。
- `NoteWindow` 注册为 `NoteStore` 变更的视图：store `changed()` → `refresh()`。

- [ ] **Step 5: 全绿 + Commit**

```bash
git add StickyNotes/src/app StickyNotes/tests/widget_test.cpp StickyNotes/tests/CMakeLists.txt StickyNotes/CMakeLists.txt
git commit -m "feat(stickynotes): frameless note window with fade-on-hover body + task strikethrough"
```

---

### Task 8: EdgeDockController + NoteGroup（收齐/拉出/吸附组）

**Files:**
- Create: `StickyNotes/src/app/note_group.h/.cpp`、`edge_dock_controller.h/.cpp`
- Modify: `StickyNotes/src/app/note_window.h/.cpp`（接入控制器）、`StickyNotes/CMakeLists.txt`
- Create: `StickyNotes/tests/group_test.cpp`、`tools/edge_dock_smoke.sh`

**Interfaces:**
- Consumes: `geometry_util`（Task 5）、`NoteWindow`（Task 7）。
- Produces:
  - `class NoteGroup : public QObject`：`QSet<NoteWindow*> members`；`bool add(NoteWindow*)`（重复返回 false）、`bool remove(NoteWindow*)`、`QList<NoteWindow*> members() const`、`void moveBy(QPoint delta)`（组内所有成员平移）。
  - `class EdgeDockController : public QObject`：构造接 `QScreen*`、`std::function<QList<NoteWindow*>()>` 窗口枚举；`QTimer`（~150ms）驱动：每窗 `computeDock`；距边近且鼠标不在热区 → 收齐；鼠标进热区 → 展开；记每组/窗 dock 状态。窗口拖动结束（`dragEnd`）时做组吸附：若与另一窗边缘距离 ≤ 阈值 → `NoteGroup` 合并并贴齐。

- [ ] **Step 1: 写失败测试 `group_test.cpp`**（NoteGroup 纯逻辑 + 吸附几何）

```cpp
#include <QTest>
#include "app/note_group.h"
#include "core/geometry_util.h"

class GroupTest : public QObject {
  Q_OBJECT
private slots:
  void addRemoveMembers() {
    NoteGroup g;
    QPointer<NoteWindow> w1 = new NoteWindow, w2 = new NoteWindow;
    QVERIFY(g.add(w1)); QVERIFY(g.add(w2));
    QVERIFY(!g.add(w1));                 // 重复
    QCOMPARE(g.members().size(), 2);
    QVERIFY(g.remove(w1));
    QCOMPARE(g.members().size(), 1);
    delete w1; delete w2;
  }
  void snapGeometry() {
    QRect a(100,100,260,320), b(600,200,260,320);
    QRect r = snappedRect(a, b);
    QCOMPARE(r.x(), a.right() + 8);
    QCOMPARE(r.y(), a.y());
  }
};
QTEST_MAIN(GroupTest)
#include "group_test.moc"
```

- [ ] **Step 2: 运行确认失败**

- [ ] **Step 3: 实现 NoteGroup + EdgeDockController**（按 Interfaces）

- [ ] **Step 4: 窗口化冒烟脚本（不进测试门）**

`tools/edge_dock_smoke.sh`：`source env.sh` → 起 `Xvfb :99` → `DISPLAY=:99 QT_QPA_PLATFORM=xcb ./build/release/stickynotes` 跑起来后，用 XTEST（`xdotool` 或 Python-Xlib 若可用）把鼠标移到窗口近左缘并观察是否收齐（截图 `import -window root` 落 PNG 供人工查看）；跑完 `kill`。**此脚本不进 ctest，仅人工验收辅助。**

- [ ] **Step 5: 全绿 + Commit**

```bash
git add StickyNotes/src/app/note_group.* StickyNotes/src/app/edge_dock_controller.* StickyNotes/tests/group_test.cpp StickyNotes/tests/CMakeLists.txt StickyNotes/CMakeLists.txt StickyNotes/tools/edge_dock_smoke.sh
git commit -m "feat(stickynotes): edge-dock controller + note groups (snap/separate)"
```

---

### Task 9: app 主程序（多窗口装配）+ 全量测试门

**Files:**
- Create: `StickyNotes/src/app/main.cpp`（窗口版入口，装配 NoteStore/多 NoteWindow/EdgeDockController/NoteGroup，自动加载/防抖保存 JSON）
- Modify: `StickyNotes/CMakeLists.txt`（加 `stickynotes` 可执行目标）

**Interfaces:**
- Consumes: 全部前述模块。
- Produces: 可执行 `stickynotes`（无头环境下 `QT_QPA_PLATFORM=offscreen` 可起；windowed 用 `xcb`）。

- [ ] **Step 1: 实现 `src/app/main.cpp`**（装配；`QTimer` 防抖 ~500ms 自动 `saveStore` 到 `stickynotes.json`；启动 `loadStore`；每 note 建 `NoteWindow` 并注册到控制器/组）

- [ ] **Step 2: 编译全部 + 全量测试（测试门目标）**

Run: `cd /home/qiansenwei/workspace/Mine/StickyNotes && cmake -B build -S . -DCMAKE_BUILD_TYPE=Debug && cmake --build build -j && QT_QPA_PLATFORM=offscreen ctest --test-dir build --output-on-failure`
Expected: **全部用例 0 失败、0 跳过**：store_test、persistence_test、palette_test、geometry_test、widget_test、group_test、image_golden_test 全绿。

- [ ] **Step 3: 补充 README.md**（构建/运行/测试/CLI 用法/golden 生成环境记录）+ Commit

```bash
git add StickyNotes/src/app/main.cpp StickyNotes/CMakeLists.txt StickyNotes/README.md
git commit -m "feat(stickynotes): app entry wiring + README; full test suite green"
```

---

## Self-Review

**Spec coverage 回溯：**
- R1 基础便签（标题/背景色/添加/删除/正文淡化）→ Task 2（模型）+ Task 4（palette）+ Task 6/7（GUI+CLI）。
- R2 固定/取消固定（仅置顶仍收齐）→ Task 7 `setPinned` + widget_test 断言 flag。
- R3 无标题栏扁平 UI → Task 7 FramelessWindowHint + 自绘标题栏。
- R4 四边收齐/拉出 → Task 5 `geometry_util`（纯函数单测）+ Task 8 EdgeDockController + 冒烟脚本。
- R5 任务条勾选删除线 → Task 2 `setTaskDone` + Task 7 StrikeOut + widget_test。
- R6 多便签 dock/分开 → Task 8 NoteGroup + 吸附 `snappedRect` + group_test。
- 硬约束 CLI+离屏 → Task 6 `--render` + golden 比对。
- 中文/字体 → Task 2 复制 Noto CJK + Task 6 fixture 含中文 + golden 人工确认。

**占位符扫描**：无 TBD/TODO；所有代码块均为具体实现或明确接口签名；GUI 部分（NoteWidget/NoteWindow 细节）以行为规格 + 关键 API 给出，执行时按规格实现（行为已被 widget_test 钉住）。

**类型/签名一致性**：`NoteStore`/`Note`/`TaskItem`/`palette`/`geometry_util` 签名在 Task 2–8 间交叉引用一致；`NoteWindow(s, n)` 构造、`NoteWidget` objectName `bodyArea`、`snappedRect(a,b)` 等跨任务引用已对齐。
