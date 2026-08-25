# StickyNotes

Qt 6 桌面便签（PC only）：无边框扁平 UI、边缘自动收齐、固定置顶、任务条勾选删除线、多便签 dock/分开。

技术栈：Qt 6.4.2（Core/Gui/Widgets/Test）、C++20、CMake ≥ 3.22。dev 依赖经
`tools/install-user-deps.sh` 无 sudo 部署到 `.user-deps/`；运行时（含 offscreen 平台插件）
系统已装。

## 构建 / 测试 / 运行

```bash
source /home/qiansenwei/workspace/Mine/.user-deps/env.sh
cmake -B build -S . -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
QT_QPA_PLATFORM=offscreen ctest --test-dir build --output-on-failure
```

窗口版（无头环境用 offscreen；有显示器用 xcb）：

```bash
QT_QPA_PLATFORM=offscreen ./build/stickynotes          # 无头可起
QT_QPA_PLATFORM=xcb ./build/stickynotes                 # windowed
```

## 架构

- `src/core/`：纯逻辑（`Note`/`TaskItem` 模型、`NoteStore`、`persistence` JSON、`palette`
  调色、`geometry_util` 收齐/吸附纯函数）。
- `src/app/`：QtWidgets GUI（`NoteWidget` 便签控件、`NoteWindow` 无边框窗、`PaletteDialog`
  选色、`NoteGroup` 吸附组、`EdgeDockController` 边缘收齐/拉出）。
- `src/cli/`：无头入口 `stickynotes-cli`（CRUD + offscreen 渲染 PNG）。

**防悬垂约定**：`NoteStore::add` 返回 `QUuid`（不返回容器元素引用）；`NoteWidget`/`NoteWindow`
持 `QUuid`，`refresh()` 经 `find(id_)` 解析当前 `Note`——运行期增删便签不产生悬垂引用。

## CLI 用法

```bash
./build/stickynotes-cli --list [store.json]
./build/stickynotes-cli --add "<title>" [store.json]
./build/stickynotes-cli --remove <id> [store.json]
./build/stickynotes-cli --pin <id> <on|off> [store.json]
QT_QPA_PLATFORM=offscreen ./build/stickynotes-cli --render out.png [store.json]
```

缺省 `store.json` 为 CWD 下 `stickynotes.json`。`--render` 当传入的 store 为空（0 便签）
时渲染内置 fixture（含中文标题/任务），用作 golden 基准。

## 测试

| 用例 | 覆盖 |
|---|---|
| store_test | 增删、pin 切换、任务勾选、索引一致性 |
| persistence_test | JSON 往返一致、损坏文件返回错误不崩溃、空 store |
| palette_test | 淡化色 alpha、hover 不透明色、前景对比 |
| geometry_test | 四边收齐判定、收齐/展开目标几何、吸附贴合 |
| widget_test | FramelessWindowHint、pin 置顶、任务删除线、淡化→hover 色、pinned 仍收齐 |
| group_test | NoteGroup 成员增删/重复、组几何平移、吸附几何 |
| image_golden_test | CLI 渲染 fixture 与 golden 逐像素比对（容差 0.1%） |

## golden 生成环境

golden 基准 `golden/stickynotes_golden.png` 由 `tools/gen-golden.sh` 生成：

```bash
source /home/qiansenwei/workspace/Mine/.user-deps/env.sh
bash tools/gen-golden.sh build
```

生成环境：Qt 6.4.2（+dfsg-21.1build5）、`QT_QPA_PLATFORM=offscreen`、bundled Noto CJK
（`assets/fonts/NotoSansCJK-Regular.ttc`）。同环境重新渲染应与基准像素级一致；
`image_golden_test` 容差 0.1% 按实测设定。

## 窗口化冒烟（不进测试门）

`tools/edge_dock_smoke.sh`：起 Xvfb :99 → xcb 跑 `stickynotes` → xdotool 移鼠标至窗口近左缘
观察收齐 → 截图 `/tmp/edge_dock_smoke.png` 供人工验收（仅人工辅助，避免 flaky）。
