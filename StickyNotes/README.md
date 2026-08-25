# StickyNotes

Qt 6 桌面便签（PC only）：无边框扁平 UI、边缘自动收齐、固定置顶、任务条勾选删除线、多便签 dock/分开。

## 构建 / 测试 / 运行

```bash
source /home/qiansenwei/workspace/Mine/.user-deps/env.sh
cmake -B build -S . -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
QT_QPA_PLATFORM=offscreen ctest --test-dir build --output-on-failure
```

## CLI

```bash
./build/stickynotes-cli --list [store.json]
./build/stickynotes-cli --add "<title>" [store.json]
./build/stickynotes-cli --remove <id> [store.json]
./build/stickynotes-cli --pin <id> <on|off> [store.json]
QT_QPA_PLATFORM=offscreen ./build/stickynotes-cli --render out.png [store.json]
```

缺省 `store.json` 为 CWD 下 `stickynotes.json`。

## golden 生成环境

golden 基准由 `tools/gen-golden.sh` 在 Qt 6.4.2 / offscreen / bundled Noto CJK 下生成，人工确认后提交；`image_golden_test` 按实测容差逐像素比对。
