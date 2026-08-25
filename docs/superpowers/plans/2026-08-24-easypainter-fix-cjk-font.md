# EasyPainter 修复计划：ImGui 界面中文显示为 "???"

> **For agentic workers:** 修复步骤用 checkbox（`- [x]`）跟踪。执行前先确认审阅通过（≥80）。

**Goal:** 修复 `easypainter` 窗口端 ImGui 调参面板中文全部显示为 `?` 的问题，并加先红后绿回归用例防复发。

**Root cause（已实证确认）:** `EasyPainter/src/app/main.cpp:245` `ImGui::CreateContext()` 后从未加载任何字体（全工程无 `AddFontFromFileTTF` 调用）。ImGui 默认内嵌字体 ProggyClean 只覆盖 ASCII/Latin。`gui.cpp` 里的简体中文（"EasyPainter 调参" / "最近预测延迟: %.2f ms" / "提示: 在窗口内按住鼠标左键拖动画笔画。"）码位集中在 0x4E00–0x9FFF（另有全角句号 。=0x3002）不在默认字形表内，`ImFontBaked::FindGlyphNoFallback()` 返回 NULL → ImGui 用 fallback 字符（`?`）渲染 → 界面全部中文变成 `??`。

- 实证（无头 CPU 探针，走真实 `AddText→Render` 路径）：默认字体 → 26 个中文字形缺失（全部）→ PNG 显示 `?`；加载 Noto Sans CJK 后 → **0 缺失** → 正常中文。`FindGlyphNoFallback` 判定 + 像素级对比（默认 1999 有墨像素 vs CJK 2975）双重确认。
- 实证（真实窗口端）：Xvfb 下运行 `easypainter` + `ffmpeg x11grab` 截图，调参面板中文确为 `?`（`/tmp/easypainter_before.png`）。
- 影响面：仅窗口端 `easypainter` 的 ImGui 层受影响；CLI/离屏笔画渲染不经 ImGui，不受影响。修复点集中在字体初始化，不触碰其他路径。

**Spec 约束 / 不回退原则:** 不回退、不兜底。单一主路径 = **项目内置（bundle）一个含 Latin + 全量简体中文字形的字体**，`AddFontFromFileTTF` + `GetGlyphRangesChineseFull()`（该版本 ImGui 1.92+ 的 legacy 路径仍按 `src->GlyphRanges` 烘焙，范围不可省）。字体缺失/加载失败 → 如实报错退出，不静默降级到默认字体。

**硬约束（全局，维持满足）:**
1. **CLI 复现入口**：`./build/debug/font_test --gtest_filter='FontCoverage.*'`（无头单命令）；另有用探针 `fonts.cpp` 的离屏渲染路径。
2. **离屏渲染输出图像**：`font_test` 与探针都落盘 before/after PNG（默认字体 → `?`；CJK 字体 → 中文），供无头验收与前后对比。真实窗口端再以 Xvfb + x11grab 截图复核。

**关键设计决定（已实证）:**
- 字体：**`NotoSansCJK-Regular.ttc`（FontNo=2 = SC 简体中文变体）**。实测覆盖 GUI 全部字符串且 **0 缺失**；Latin（ASCII）也齐全（Droid Sans Fallback 缺 Latin，实测 19 个 ASCII 缺失，不可作唯一字体）。bundle 到 `EasyPainter/assets/fonts/`（~19.5MB，含许可说明）。
- ImGui 1.92+ 字形为**懒加载/流式烘焙**：必须在真实绘制路径（`AddText`→`Render`）中渲染后才入 atlas 纹理。故回归用例与探针都走真实 `ImGui::NewFrame/AddText/Render` + CPU 栅格化 `ImDrawData`，而非直接查 atlas。

---

## Task 1: 加回归用例（先红后绿，TDD 序列）

**Files:**
- Add: `EasyPainter/tests/font_test.cpp`
- Add: `EasyPainter/src/app/fonts.h`（仅头文件占位，`LoadCjkFont` 声明）
- Modify: `EasyPainter/CMakeLists.txt`（新增 `font_test` target）

- [x] **Step 1: 新增 `tests/font_test.cpp`**（含 CPU 栅格化工具 + 两个用例）

```cpp
#include <gtest/gtest.h>
#include "app/fonts.h"
#include "core/render/image_io.h"
#include "imgui.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <vector>

namespace easypainter::app {

#ifndef CJK_FONT_PATH
#define CJK_FONT_PATH ""
#endif

// UTF-8 → 码位
static std::vector<ImWchar> utf8_to_cps(const char* s) { /* 见实现:标准 UTF-8 解码 */ }

// CPU 栅格化一个纹理三角形(ImDrawData 文本三角形都是轴向对齐的字形四边形)
static void rasterize_tri(const ImDrawVert& v0, const ImDrawVert& v1, const ImDrawVert& v2,
                          const uint8_t* tex, int tw, int th, int bpp, int W, int H,
                          std::vector<uint8_t>& rgba, int x0, int y0, int x1, int y1) {
  // 边界框 + 重心坐标插值 UV,采样 tex 的 alpha 通道混合到画布
}

// 走真实 ImGui 绘制路径渲染一帧,CPU 栅格化 ImDrawData;返回缺失字形数(会用 '?' 兜底)。
static int RenderGuiTextAndCountMissing(ImFont* font, float size, const char* utf8,
                                        int W, int H, std::vector<uint8_t>& rgba) {
  ImGuiIO& io = ImGui::GetIO();
  io.DisplaySize = ImVec2((float)W, (float)H);
  io.DeltaTime = 1.f / 60.f;
  io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;
  ImGui::NewFrame();
  ImGui::GetBackgroundDrawList()->AddText(font, size, ImVec2(8, 8),
                                          IM_COL32(255, 255, 255, 255), utf8);
  ImGui::Render();
  rgba.assign((size_t)W * H * 4, 0);
  unsigned char* tex = nullptr; int tw = 0, th = 0, bpp = 0;
  ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&tex, &tw, &th, &bpp);
  ImDrawData* dd = ImGui::GetDrawData();
  /* 遍历 dd->CmdLists 的每个 ImDrawCmd,按 ClipRect 裁剪,三角栅格化(同探针实现) */
  ImFontBaked* baked = font->GetFontBaked(size);
  int missing = 0;
  for (ImWchar c : utf8_to_cps(utf8))
    if (c != '\n' && c != ' ' && baked->FindGlyphNoFallback(c) == nullptr) ++missing;
  return missing;
}

static const char* kGuiText =
    "EasyPainter 调参\n最近预测延迟: 0.00 ms\n"
    "提示: 在窗口内按住鼠标左键拖动画笔画。";

// 钉死 bug 前置条件:默认字体无中文字形 → 渲染为 '?'。修复前后都应成立。
TEST(FontCoverage, DefaultFontLacksCjkGlyphs) {
  ImGui::CreateContext();
  ImFont* font = ImGui::GetIO().Fonts->AddFontDefault();
  std::vector<uint8_t> rgba;
  const int missing = RenderGuiTextAndCountMissing(font, 18.f, kGuiText, 640, 200, rgba);
  EXPECT_GT(missing, 0);                       // 中文全部缺失
  EXPECT_TRUE(render::write_png("font_before_default.png", 640, 200, rgba));
  ImGui::DestroyContext();
}

// ★ 回归用例(先红后绿):加载 CJK 字体后 GUI 字符串全部字形覆盖,0 缺失。
TEST(FontCoverage, CjkFontLoadsGuiStrings) {
  ImGui::CreateContext();
  ImFont* font = app::LoadCjkFont(ImGui::GetIO().Fonts, CJK_FONT_PATH, 18.f);
  ASSERT_NE(font, nullptr) << "CJK 字体加载失败:" << CJK_FONT_PATH;
  std::vector<uint8_t> rgba;
  const int missing = RenderGuiTextAndCountMissing(font, 18.f, kGuiText, 640, 200, rgba);
  EXPECT_EQ(missing, 0) << "仍有中文字形缺失,界面会显示 '?'";
  EXPECT_TRUE(render::write_png("font_after_cjk.png", 640, 200, rgba));
  ImGui::DestroyContext();
}

}  // namespace easypainter::app
```

- [x] **Step 2: 加 `src/app/fonts.h` 占位（仅声明，未实现）**

```cpp
#pragma once
#include "imgui.h"
namespace easypainter::app {
// 把含全量简体中文字形的字体加入 atlas。成功返回非空 ImFont*。
ImFont* LoadCjkFont(ImFontAtlas* atlas, const char* font_path, float size_px);
}
```

- [x] **Step 3: CMake 新增 `font_test` target**（Task 1 阶段 `tests/font_test.cpp` 已引用但 `src/app/fonts.cpp` 尚不存在 → 编译期缺源文件/链接期缺 `LoadCjkFont` 实现 → **红**；Task 2 补上 `src/app/fonts.cpp`）

```cmake
add_executable(font_test tests/font_test.cpp)
# Task 2 起追加 src/app/fonts.cpp 到本 target 源列表
target_link_libraries(font_test PRIVATE easypainter_core imgui GTest::gtest GTest::gtest_main)
target_include_directories(font_test PRIVATE src vendor/imgui vendor)
target_compile_definitions(font_test PRIVATE
  CJK_FONT_PATH="${CMAKE_CURRENT_SOURCE_DIR}/assets/fonts/NotoSansCJK-Regular.ttc")
gtest_discover_tests(font_test)
```

- [x] **Step 4: 确认红**：`cmake --build --preset debug --target font_test` → 链接错误 `undefined reference to easypainter::app::LoadCjkFont`（红态；同时 `CjkFontLoadsGuiStrings` 无法运行）。

## Task 2: 实现修复（fonts.cpp + bundle 字体 + main.cpp 接线）→ 转绿

**Files:**
- Add: `EasyPainter/src/app/fonts.cpp`
- Add: `EasyPainter/assets/fonts/NotoSansCJK-Regular.ttc`（从系统 `/usr/share/fonts/opentype/noto/` 拷贝）+ `EasyPainter/assets/fonts/README.md`（来源与 SIL OFL 1.1 许可说明）
- Modify: `EasyPainter/CMakeLists.txt`（`easypainter` target 加 fonts.cpp + `CJK_FONT_PATH` 编译宏）
- Modify: `EasyPainter/src/app/main.cpp`（CreateContext 后加载字体）

- [x] **Step 1: 实现 `src/app/fonts.cpp`**

```cpp
#include "app/fonts.h"

namespace easypainter::app {

ImFont* LoadCjkFont(ImFontAtlas* atlas, const char* font_path, float size_px) {
  if (atlas == nullptr || font_path == nullptr || *font_path == '\0') return nullptr;
  ImFontConfig cfg;
  cfg.FontNo = 2;  // Noto Sans CJK SC(简体中文变体);ttc 中 JP=0,KR=1,SC=2,TC=3,HK=4
  // ChineseFull = Default(ASCII/Latin) + Half-width + Hiragana/Katakana + ~21000 CJK
  return atlas->AddFontFromFileTTF(font_path, size_px, &cfg,
                                   atlas->GetGlyphRangesChineseFull());
}

}  // namespace easypainter::app
```

- [x] **Step 2: bundle 字体**：拷贝系统 `NotoSansCJK-Regular.ttc` 到 `assets/fonts/`，并写 `README.md` 注明来源路径、版本、SIL OFL 1.1 许可（系统许可文件 `/usr/share/doc/fonts-noto-cjk/copyright`）。
- [x] **Step 3: CMake**：
  - `easypainter` target 的 `add_executable` 加 `src/app/fonts.cpp src/app/fonts.h`，并加 `target_compile_definitions(easypainter PRIVATE CJK_FONT_PATH="<abs>/assets/fonts/NotoSansCJK-Regular.ttc")`。
  - `font_test` target 源列表追加 `src/app/fonts.cpp`（见 Task 1 Step 3）。
- [x] **Step 4: `main.cpp` 接线**（在 `ImGui::CreateContext();` 后、`ImGui_ImplGlfw_InitForVulkan` 前）：

```cpp
ImGui::CreateContext();
if (app::LoadCjkFont(ImGui::GetIO().Fonts, app::cjk_font_path(), 18.f) == nullptr) {
  std::fprintf(stderr, "[app] 无法加载 CJK 字体:%s,界面中文将显示为 '?'。\n",
               app::cjk_font_path());
  return 1;  // 依赖缺失:如实报告阻塞,不静默降级
}
```
（需在 `fonts.h` 补 `const char* cjk_font_path();`，返回 `#ifdef CJK_FONT_PATH` 的值；`#include "app/fonts.h"`。）

- [x] **Step 5: 确认转绿**：重建 → `./build/debug/font_test` 全绿；`CjkFontLoadsGuiStrings` 从无法编译 → 通过（0 缺失）；`DefaultFontLacksCjkGlyphs` 仍过（钉死 bug 前置）。

## Task 3: 离屏图像 + 真实窗口端复核

- [x] `font_test` 落盘 `font_before_default.png`（`?`）与 `font_after_cjk.png`（中文）→ 与探针基线对比。
- [x] 重建 `easypainter`；Xvfb 下重跑 + `ffmpeg x11grab` 截图 `/tmp/easypainter_after.png`，确认调参面板中文正常（与 `before` 图对照）。

## Task 4: 全量回归

- [x] `ctest --test-dir build/debug --output-on-failure` 全绿（0 失败 0 跳过）。

## 影响面核对

- `main.cpp` 字体初始化为新增调用点；`stroke_vb` 生命周期改动（工作区既有未提交 WIP，另一 bug 的修复）**保持不动**，仅报告，不纳入本修复提交范围。
- 不改 CLI/离屏/管线/shader/预测器；不引入新依赖（字体随仓库 bundle）。
- 字体仅在 atlas 构建期一次性烘焙（懒加载模型下只烘焙界面实际用到的字形），运行期无额外开销。

## 遗留项 / 备注

- 工作区 `EasyPainter/src/app/main.cpp` 存在**本次任务前**的未提交改动（stroke 顶点 buffer 生命周期修复），与本 bug 无关；本计划只改动字体相关区域，finish 阶段如实报告 git status。
- 字体大小选 18px（探针实证）。

## 执行记录

- 审阅得分：**95/100**（review AGENT 通过 ≥80 门槛）。
- 测试：**13/13 全绿**（`ctest --test-dir build/debug --output-on-failure`，0 失败 0 跳过）。
- 回归：**先红后绿已确认**——Task 1 Step 4 链接期红态（`undefined reference to LoadCjkFont`）→ Task 2 实现 `fonts.cpp` 后转绿。
