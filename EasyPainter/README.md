# EasyPainter

集成测试 Google [ink-stroke-modeler](https://github.com/google/ink-stroke-modeler) 的
C++20 工程。三个目标：

- **`easypainter`**：窗口端（GLFW + Vulkan swapchain + Dear ImGui），拖拽鼠标画笔画、
  预测轨迹实时跟随、调参面板可交互。
- **`easypainter-cli`**：无头 CLI，离屏渲染（lavapipe 软件光栅）输出 PNG。
- **`easypainter_core`**：静态库（stroke 封装 `Predictor` + Vulkan 离屏渲染 + bench）。

## 环境准备

系统依赖（Vulkan SDK / X11 头 / lavapipe / Xvfb）为**无 sudo 用户级部署**：

```bash
tools/install-user-deps.sh   # 部署到 .user-deps/,生成 env.sh
source .user-deps/env.sh     # 每个构建/运行 shell 先 source
tools/setup-env.sh --check   # 校验工具链与 user-deps
```

三方库进全局池（`third_party/deps.yaml`），一次 fetch + 预编译（release+debug）：

```bash
tools/fetch-deps.py --all
tools/build-deps.py --all
```

## 构建

```bash
cd EasyPainter
cmake --preset debug && cmake --build --preset debug
```

## CLI 用法

```bash
./build/debug/easypainter-cli --output /tmp/out.png            # 内置示例点
./build/debug/easypainter-cli --input pts.txt --output a.png \
    --width 640 --height 480 --stroke 0.0003,72                # 自定义输入/调参
```

输入文件每行 `x,y`（自动归一化到 [0,1]）。

## 窗口用法

无物理显示器时用 Xvfb：

```bash
Xvfb :99 -screen 0 1280x800x24 &
DISPLAY=:99 ./build/debug/easypainter
```

在窗口内按住鼠标左键拖动画笔画；右侧面板滑条调参，`Run bench` 显示延迟/吞吐。

## 测试

```bash
ctest --test-dir build/debug --output-on-failure
```

共 10 用例，0 失败 0 跳过：

| 用例 | 断言 |
|---|---|
| InputSource / Predictor ×4 | 事件序列、空输入不崩、产出非空、reset |
| ImageIO.WritesPngFile | PNG magic 校验 |
| OffscreenRender.ProducesImageOnLavapipe | 离屏渲染冒烟（红像素存在） |
| OffscreenRender.MatchesBaselineGolden | 与 CLI 基准 PNG 逐像素比对（≤1% 容差） |
| Bench ×2 | 单次 update 延迟均值 <10ms、吞吐 >0 |
| NumericGolden.MatchesIndependentInkOracle | 与独立 oracle 数值 golden 逐点比对（1e-4） |

## Golden 说明

**数值 golden（`tests/data/golden_points.txt`）**：由独立 oracle 生成——
`tools/oracle_golden.cpp` 直接驱动 ink 原生 API（`StrokeModeler::Update`），
与本工程 `Predictor` 封装是完全独立的两条编译路径（禁止自产自比）。重新生成：

```bash
source .user-deps/env.sh
cmake -S EasyPainter/tools -B EasyPainter/tools/build -G Ninja
cmake --build EasyPainter/tools/build
./EasyPainter/tools/build/generate_golden > EasyPainter/tests/data/golden_points.txt
```

生成环境：Ubuntu 24.04, ink-stroke-modeler main (f2388813b0b2), abseil 20260817.0。

**图像 golden（`tests/data/golden_render.png`）**：由 `easypainter-cli` 在 lavapipe
软件光栅上生成（`--width 320 --height 240` 内置示例点），再经 `render_offscreen`
重渲逐像素比对。生成环境：lavapipe 25.2.8（`mesa-vulkan-drivers`，ICD
`lvp_icd.json`）。基准按**图像/窗口坐标**约定（原点左上，y 向下）；
2026-08-24 因修复 `shaders/stroke.vert` 的 Y 双重翻转而重基线。
