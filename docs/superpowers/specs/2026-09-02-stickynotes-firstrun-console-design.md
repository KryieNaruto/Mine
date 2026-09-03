# StickyNotes「打开无界面、只有黑色控制台」根因规格

日期:2026-09-02 · 关联计划:`plans/2026-09-02-stickynotes-firstrun-console.md`

## Bug 报告

Windows + VS 打开 StickyNotes 生成的 .sln,运行 `stickynotes`,**没有便签界面,只有一个黑色控制台**。
(用户确认复现平台 = Windows + VS;首启期望 = 开箱即有界面。)

## 复现(实证)

| 场景 | 结果 |
|---|---|
| 全新目录(无 `stickynotes.json`),Xvfb+xcb 跑真实 `build/stickynotes` | 进程存活,**0 个便签窗口**,画面全空(唯一 root child 是 Qt 内部 3×3 "Selection Owner" 剪贴板助手,非用户界面) |
| 预置 1 条便签的 store,同条件跑 | 出现便签窗口(证明非渲染回归) |
| Linux 无头基线 `QT_QPA_PLATFORM=offscreen ctest` | 7/7 全绿 → GUI 核心/渲染正常 |

## 根因(两条叠加,命中「没有界面 + 只有黑色控制台」)

### R1 — `stickynotes` 目标是 console 子系统(黑控制台来源)

- `StickyNotes/CMakeLists.txt`:`add_executable(stickynotes src/app/main.cpp)` **未标 `WIN32`**。
- 同仓库 chandao 的 GUI 入口:`add_executable(main WIN32 ...)`,明确 GUI 子系统。
- 后果:MSVC 下 `stickynotes.exe` 编为 `/SUBSYSTEM:CONSOLE`,Explorer 双击 / VS F5 必然先弹一个黑色控制台窗口。
- 影响面:仅 `stickynotes` 一个 GUI 入口;`stickynotes-cli` 是真正的 CLI,**保持 console 正确**。

### R2 — 首次/空 store 启动零便签(「没有界面」来源)

- `loadStore(store, path)`:文件不存在 → `QFile::open` 失败 → 返回 `false`,**store 保持为空**(不播种)。
- `NoteStore` 默认构造不播种任何便签;`main()` 只 `for (notes) createWindow` → 空 store = **0 个 NoteWindow = 无界面**。
- 无其他自建入口:标题栏「+」按钮发 `addRequested()`,但全工程**无任何 connect 消费该信号**(`splitRequested`/`deleteRequested` 同样悬空)→ 开箱后无任何办法新建便签。
- 影响面:首次启动路径(全新 CWD 无 `stickynotes.json`);有数据文件用户不受影响。

## 修复方向(不回退、不掩盖)

1. **R1**:`stickynotes` 目标加 `WIN32` → GUI 子系统,消灭黑色控制台;另在 `WIN32` 下用
   `get_target_property(WIN32_EXECUTABLE)` + `FATAL_ERROR` 作自守卫,防止将来误删 WIN32 而无人察觉。
2. **R2**:新增 app 层启动策略 `seedFirstRunNoteIfMissing(NoteStore&, storePath)`:
   **store 文件不存在(真首启)** → `store.add(Note{})` 播种一张空白便签 + `saveStore` 落盘,
   保证开箱 ≥1 个便签窗口。文件**已存在**(含 `{"notes":[]}` 空数组或损坏)一律**不干预**,尊重用户已有数据/删除,避免覆盖。

## 验证口径

- 无头:全量 `QT_QPA_PLATFORM=offscreen ctest`(含新增 `startup_test`,镜像现有 offscreen QTest 用法)。
- 离屏像素:既有 `image_golden_test`(CLI offscreen 渲染 PNG 逐像素比对 golden)保持全绿,证明渲染未被改动破坏。
- R1 的 console 子系统在 Linux 无 console 概念,红绿仅能静态/Windows 自守卫覆盖;「开箱有界面」这一跨平台可见症状由 R2 的 `startup_test` 承载先红后绿。
