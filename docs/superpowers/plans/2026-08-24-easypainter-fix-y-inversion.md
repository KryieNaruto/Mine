# EasyPainter 修复计划：鼠标与绘制位置上下颠倒

> **For agentic workers:** 修复步骤用 checkbox（`- [ ]`）跟踪。执行前先确认审阅通过（≥80）。

**Goal:** 修复 `easypainter` 窗口端「鼠标位置与绘制位置 Y 轴上下颠倒」的 bug，并为渲染 Y 方向加回归测试防复发。

**Root cause（已实证确认）:** `shaders/stroke.vert` 第 12 行 `p.y = 1.0 - p.y;` 本意是「图像/窗口坐标原点在左上（y 向下，0=顶）」，但作者按 OpenGL 的 NDC 心智做了翻转。Vulkan 视口变换把 **NDC y=-1 映到 framebuffer 顶部行（y_f=0）、y=+1 映到底部行**，与 OpenGL 相反。于是「翻转 + Vulkan 映射」构成**双重翻转 → 净镜像颠倒**。鼠标输入（`main.cpp:286-287` 用 `ny=my/h`，0=顶）喂给该 shader 后被倒置。

- 实证：离屏渲染示例 S 曲线（中点 `(x=0.5, y=0.0)`），中点落在图像**底部**（行 219~229 / H=240）；按 Y-down 约定应落在**顶部**。
- 影响面：windowed（报障）与 CLI/golden（离屏）共用同一 `Pipeline`/shader，均受影响；`MatchesBaselineGolden` 因「golden 由同款（错误的）shader 生成」而通过——golden 无法暴露该语义错误，需重生成。

**Spec 约束:** 坐标约定以 shader 注释为准——**输入为图像/窗口坐标，原点左上，y 向下**。修复后窗口端鼠标（Y-down）与 CLI 输入点（Y-down）语义一致。

**硬约束（build-pipeline 全局，维持满足）:** CLI 模式 + 离屏渲染输出图像——本修复不动 CLI/离屏架构，仅纠正 shader 映射；回归测试仍走离屏渲染路径。

**不回退原则:** 无回退/兜底设计；根因修复为 shader 去翻转 + golden 重基线。

---

## Task 1: 加 Y 方向回归测试（先红后绿，TDD 序列）

**Files:**
- Modify: `EasyPainter/tests/image_golden_test.cpp`

- [x] **Step 1: 新增用例 `OffscreenRender.YDownTopIsImageTop`**

在 `image_golden_test.cpp` 加：

```cpp
// Y 方向回归:归一化 y=0(图像/窗口坐标原点左上)必须渲染到图像顶部。
// 此前 stroke.vert 的 p.y=1.0-p.y 在 Vulkan 下构成双重翻转(上下颠倒)。
TEST(OffscreenRender, YDownTopIsImageTop) {
  VulkanContext ctx;
  ASSERT_TRUE(ctx.init());
  Pipeline pipeline;
  ASSERT_TRUE(pipeline.init(ctx));

  const std::vector<stroke::Vec2> pts = {{0.2f, 0.1f}, {0.8f, 0.1f}};  // y=0.1 水平线
  constexpr uint32_t kSize = 64;
  auto res = render_offscreen(ctx, pipeline, pts, kSize, kSize);

  int min_row = -1, max_row = -1;
  for (size_t i = 0; i < res.rgba.size(); i += 4) {
    if (res.rgba[i] > 150 && res.rgba[i + 1] < 100 && res.rgba[i + 2] < 100) {
      const int row = static_cast<int>(i / 4 / kSize);
      if (min_row < 0) min_row = row;
      max_row = row;
    }
  }
  ASSERT_GE(min_row, 0) << "未找到笔画像素";
  // y=0.1 的水平线应落在图像顶部 30% 内;颠倒时落在底部(约 0.9H)。
  EXPECT_LT(max_row, static_cast<int>(kSize) * 3 / 10);
}
```

- [x] **Step 2: 确认先红（未改 shader 时）**

跑 `ctest -R YDownTopIsImageTop` → 失败（线落在行 ~57，非 <19）。红态另外已由离屏探针实证（示例 S 曲线中点 y=0.0 落在底行）充当独立证据。

## Task 2: 修正 shader（根因修复）→ 确认后绿

**Files:**
- Modify: `EasyPainter/shaders/stroke.vert`

- [x] **Step 1: 移除双重翻转，更新注释**

`stroke.vert` 的 `main()` 改为（删除 `p.y = 1.0 - p.y;`，注释讲清 Vulkan 视口 Y 约定）：

```glsl
void main() {
  vec2 p = inPos * pc.uScale + pc.uOffset;
  // 输入按图像/窗口坐标(原点左上,y 向下):p.y=0 在图像顶部。
  // Vulkan 视口变换把 NDC y=-1 映到 framebuffer 顶部行,与 OpenGL 相反;
  // 故此处不再翻转,直接 p*2-1 即可让 y=0 → 顶部。
  gl_Position = vec4(p * 2.0 - 1.0, 0.0, 1.0);
}
```

映射验证：修复后 `y_f = inPos.y * H`（inPos.y=0 → 顶行，=1 → 底行），与输入 Y-down 约定一致。

- [x] **Step 2: 重编后跑回归用例确认后绿**

`cmake --build --preset release`（glslc 自定义命令 DEPENDS 命中 shader，级联重编 SPV/嵌入头/core/CLI）→ `ctest -R YDownTopIsImageTop` → 通过（线落在行 ~6 < 19）。

## Task 3: 重生成图像 golden

**Files:**
- Modify: `EasyPainter/tests/data/golden_render.png`（重生成，提交二进制）

- [x] **Step 1: 用修复后 shader 重生成基准**

```bash
cd /home/qiansenwei/workspace/Mine && source .user-deps/env.sh
cd EasyPainter && cmake --build --preset release   # 重编 shader(glslc 自定义命令)与 easypainter-cli
./build/release/easypainter-cli --output tests/data/golden_render.png --width 320 --height 240
```

与 `image_golden_test.cpp::MatchesBaselineGolden` 的复算路径完全一致（内置示例点、默认 Predictor 参数、320×240），Golden 注释同步说明 Y 约定。

- [x] **Step 2: 确认 golden 重基线**

`MatchesBaselineGolden` 现在与修复后渲染比对 → 通过（修复前后该用例均自洽，本步确保不因重基线而红）。

## Task 4: 测试门（100 分放行）

- [x] **Step 1: 全量跑 ctest**

```bash
ctest --test-dir build/release --output-on-failure
```

Expected: 全部用例 **0 失败 0 跳过**（含新增 Y 回归、图像 golden、数值 golden、bench、单测）。

## Task 5: 收尾

**Files:**
- Modify: `EasyPainter/README.md`（golden 生成说明补 Y 约定/重基线说明）
- Modify: `docs/superpowers/plans/2026-08-24-easypainter-fix-y-inversion.md`（自检记录）
- Cleanup: 既有未提交诊断（`tools/repro_bench.cpp` + `CMakeLists.txt` 的 repro_bench 目标，注释「定位后移除」）——本次收尾一并移除；`/tmp/yprobe` 删除。

- [x] **Step 1: 更新文档**

`README.md` golden 说明补 Y 约定与重基线说明。

- [x] **Step 2: 清理临时诊断与探针**

移除 `tools/repro_bench.cpp` + `CMakeLists.txt` 的 repro_bench 目标（注释「定位后移除」），删除 `/tmp/yprobe`。

- [x] **Step 3: 处置既有未提交 main.cpp 变更**

`src/app/main.cpp` 已有未提交的 buffer 生命周期/acquire 健壮性修复（与本 bug 无关，是前次崩溃诊断的收尾）。**处置：保留为独立 WIP，不并入本修复；** 在最终汇报中向用户说明，由其决定独立提交或回退。变更清单需与其区分，保证「git status 干净」目标与实际变更可核对。

---

## 自检记录

- **Spec 覆盖**：坐标约定（Y-down 左上原点）与 shader 注释一致；CLI+离屏硬约束保持满足；不回退。
- **回归覆盖**：新增 `YDownTopIsImageTop` 精确定位该 bug（y=0.1 → 顶部 30%），修复前红、修复后绿。
- **影响面核对**：`ProducesImageOnLavapipe`（位置无关冒烟）不受影响；`MatchesBaselineGolden` 随 golden 重基线保持一致；数值 golden/单测/bench 不涉渲染，不受影响。
- **遗留项**：`easypainter` 窗口端真机人工验收（Xvfb 下拖拽目视）仍按 README 进行；本修复不改变其交互。
- **审阅得分**：91/100 通过（≥80 门槛）
- **测试门**：11/11 通过（0 失败 0 跳过）
