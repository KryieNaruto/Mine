# 修复计划:StickyNotes 打开无界面、只有黑色控制台

日期:2026-09-02 · 规格:`specs/2026-09-02-stickynotes-firstrun-console-design.md`

## 根因(简)

- **R1**:`stickynotes` GUI 目标缺 `WIN32` → MSVC `/SUBSYSTEM:CONSOLE` → 黑控制台(chandao `main` 已标 WIN32)。
- **R2**:首次/空 store 启动 `main()` 建 0 个 `NoteWindow`(loadStore 文件缺失→空 store、NoteStore 默认不播种),且「+」新建信号无消费者 → 无界面且无自建入口。

## 修复方案

1. **R1 — `StickyNotes/CMakeLists.txt`**
   - `add_executable(stickynotes src/app/main.cpp)` → `add_executable(stickynotes WIN32 src/app/main.cpp)`(GUI 子系统,与 chandao 一致)。
   - WIN32 自守卫:文件末尾 `if(WIN32)` 段内对 `stickynotes` 目标 `get_target_property(_w stickynotes WIN32_EXECUTABLE)`,若非真则 `message(FATAL_ERROR ...)`,防误删 WIN32 无声回归。

2. **R2 — 首启播种(app 层启动策略)**
   - 新增 `src/app/startup.h` / `src/app/startup.cpp`:
     `bool seedFirstRunNoteIfMissing(NoteStore& store, const QString& storePath);`
     - `QFile::exists(storePath)` 为真(已有数据文件,含空数组/损坏)→ 返回 false,不干预。
     - 不存在(真首启)→ `store.add(Note{})`(空白便签,默认橙黄/pos(100,100)/260×320)+ `saveStore` 落盘;返回 true。
   - `src/app/main.cpp`:在 `loadStore(store, kStorePath)` 之后、初始建窗循环之前调用 `seedFirstRunNoteIfMissing(store, kStorePath)`,使开箱即有 ≥1 便签窗口。
   - `CMakeLists.txt`:把 `startup.cpp/.h` 加入 `stickynotes_app` 源列表。

3. **测试注册**:新增 `tests/startup_test.cpp`,注册 `startup_test`(link `stickynotes_app Qt6::Test Qt6::Widgets`,镜像 `widget_test`)。

## 变更清单

| 文件 | 动作 |
|---|---|
| `StickyNotes/CMakeLists.txt` | `stickynotes` 加 WIN32 + 自守卫;`stickynotes_app` 加 startup.cpp/.h |
| `StickyNotes/src/app/startup.h/.cpp` | 新增:首启播种策略 |
| `StickyNotes/src/app/main.cpp` | loadStore 后调用播种 |
| `StickyNotes/tests/startup_test.cpp` | 新增回归用例 |
| `StickyNotes/tests/CMakeLists.txt` | 注册 startup_test |
| docs spec/plan | 本文档与规格 |

## 回归用例设计(先红后绿,无头/offscreen)

`tests/startup_test.cpp`(QTEST_MAIN + QTemporaryDir,offscreen 平台;镜像 widget_test):

1. `firstRun_noStoreFile_seedsBlankNote`(核心红):
   临时目录无 store 文件 → `loadStore` false、store 空 → `seedFirstRunNoteIfMissing` 为真 →
   **断言 `notes().size()==1`**、文件已落盘、note id 非空;
   冒烟:`NoteWindow w(store,id); w.show();` 断言 `w.noteWidget()!=nullptr`、`w.isWindow()` 且在 `QApplication::topLevelWidgets()` 中(即「有界面」)。
   - 红:未实现播种(空/空 store)→ size 0 ≠ 1。
2. `existingFile_withNotes_unchanged`:预置 1 便签 → 播种返回 false,仍 1。
3. `existingFile_emptyArray_unchanged`:`{"notes":[]}` → 播种返回 false,仍 0(尊重用户删光)。

先红后绿步骤(implement AGENT 执行):
1. 加 `startup.h/.cpp` **stub**(仅 return false)+ 加 `startup_test.cpp` 并注册 → 构建 → 跑 case1 红(size 0)。
2. 实现真实播种体 → case1 绿;case2/3 全绿。
3. R1 的 WIN32 声明 + `WIN32` 自守卫为平台声明/Windows configure 期检查,Linux 无红绿;其与 chandao `main` 的差分 + Windows 构建为准。

## 验证方式(无头)

```bash
source /home/qiansenwei/workspace/Mine/.user-deps/env.sh
cmake -B build -S . -DCMAKE_BUILD_TYPE=Debug && cmake --build build -j
QT_QPA_PLATFORM=offscreen ctest --test-dir build --output-on-failure
```
- 期望:新增 `startup_test` 1-3 全绿 + 既有 7 用例无回归(0 失败 0 跳过)。
- 离屏渲染回归:`image_golden_test`(offscreen PNG 逐像素比对)保持全绿,证明 GUI/渲染未被改动破坏。

## 影响面核对

- 共享出错代码路径:`main()` 启动装配(changed 处理器在播种之后才 connect,播种 add 不触发重复建窗)。其它调用方:CLI 不使用播种(app 层函数),不受影响。
- 不引入新回归:`loadStore`/`NoteStore`/渲染未改;仅新增启动播种与窗口目标子系统声明。
- 边界:store 文件损坏 → `QFile::exists` 真 → 不播种(维持现状:0 窗口、不覆盖用户文件)。
