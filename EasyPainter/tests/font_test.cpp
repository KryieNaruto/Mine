// font_test:ImGui 界面中文字形覆盖回归用例(先红后绿)。
// 走真实绘制路径 AddText→Render,CPU 栅格化 ImDrawData 落盘 PNG,
// 并用 FindGlyphNoFallback 统计缺失字形数(缺失会以 '?' 渲染)。
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
static std::vector<ImWchar> utf8_to_cps(const char* s) {
  std::vector<ImWchar> out;
  const unsigned char* p = reinterpret_cast<const unsigned char*>(s);
  while (*p) {
    unsigned int c = 0;
    if ((*p & 0x80) == 0) {
      c = *p++;
    } else if ((*p & 0xE0) == 0xC0) {
      c = (*p & 0x1F) << 6;
      c |= (p[1] & 0x3F);
      p += 2;
    } else if ((*p & 0xF0) == 0xE0) {
      c = (*p & 0x0F) << 12;
      c |= (p[1] & 0x3F) << 6;
      c |= (p[2] & 0x3F);
      p += 3;
    } else if ((*p & 0xF8) == 0xF0) {
      c = (*p & 0x07) << 18;
      c |= (p[1] & 0x3F) << 12;
      c |= (p[2] & 0x3F) << 6;
      c |= (p[3] & 0x3F);
      p += 4;
    } else {
      ++p;
      continue;
    }
    out.push_back(static_cast<ImWchar>(c));
  }
  return out;
}

// 边缘函数,返回符号化有向面积。
static float edge(const ImVec2& a, const ImVec2& b, const ImVec2& c) {
  return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

// CPU 栅格化一个纹理三角形(ImDrawData 文本三角形都是轴向对齐的字形四边形)
static void rasterize_tri(const ImDrawVert& v0, const ImDrawVert& v1,
                          const ImDrawVert& v2, const uint8_t* tex, int tw, int th,
                          int bpp, int W, int H, std::vector<uint8_t>& rgba,
                          int x0, int y0, int x1, int y1) {
  const float minx = std::floor(std::min({v0.pos.x, v1.pos.x, v2.pos.x}));
  const float miny = std::floor(std::min({v0.pos.y, v1.pos.y, v2.pos.y}));
  const float maxx = std::ceil(std::max({v0.pos.x, v1.pos.x, v2.pos.x}));
  const float maxy = std::ceil(std::max({v0.pos.y, v1.pos.y, v2.pos.y}));
  const int sx0 = std::max(x0, (int)minx), sy0 = std::max(y0, (int)miny);
  const int sx1 = std::min(x1, (int)maxx), sy1 = std::min(y1, (int)maxy);
  const float area = edge(v0.pos, v1.pos, v2.pos);
  if (area == 0.f) return;
  for (int py = sy0; py < sy1; ++py) {
    for (int px = sx0; px < sx1; ++px) {
      const ImVec2 p(px + 0.5f, py + 0.5f);
      float w0 = edge(v1.pos, v2.pos, p);
      float w1 = edge(v2.pos, v0.pos, p);
      float w2 = edge(v0.pos, v1.pos, p);
      if (w0 < 0 || w1 < 0 || w2 < 0) continue;
      w0 /= area;
      w1 /= area;
      w2 /= area;
      const float u = w0 * v0.uv.x + w1 * v1.uv.x + w2 * v2.uv.x;
      const float v = w0 * v0.uv.y + w1 * v1.uv.y + w2 * v2.uv.y;
      int tx = (int)(u * tw), ty = (int)(v * th);
      tx = std::clamp(tx, 0, tw - 1);
      ty = std::clamp(ty, 0, th - 1);
      const int ti = (ty * tw + tx) * bpp;
      const float a = tex[ti + 3] / 255.f;
      const int o = (py * W + px) * 4;
      rgba[o + 0] = 255;
      rgba[o + 1] = 255;
      rgba[o + 2] = 255;
      rgba[o + 3] = (uint8_t)(std::max((int)rgba[o + 3], (int)(a * 255)));
    }
  }
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
  unsigned char* tex = nullptr;
  int tw = 0, th = 0, bpp = 0;
  ImGui::GetIO().Fonts->GetTexDataAsRGBA32(&tex, &tw, &th, &bpp);
  ImDrawData* dd = ImGui::GetDrawData();
  for (int li = 0; li < dd->CmdListsCount; ++li) {
    ImDrawList* list = dd->CmdLists[li];
    for (const ImDrawCmd& cmd : list->CmdBuffer) {
      if (cmd.UserCallback) continue;
      const int x0 = std::max(0, (int)cmd.ClipRect.x);
      const int y0 = std::max(0, (int)cmd.ClipRect.y);
      const int x1 = std::min(W, (int)cmd.ClipRect.z);
      const int y1 = std::min(H, (int)cmd.ClipRect.w);
      for (unsigned int i = 0; i + 2 < cmd.ElemCount; i += 3) {
        const unsigned int ia = cmd.IdxOffset + i;
        const ImDrawVert& a = list->VtxBuffer[cmd.VtxOffset + list->IdxBuffer[ia]];
        const ImDrawVert& b = list->VtxBuffer[cmd.VtxOffset + list->IdxBuffer[ia + 1]];
        const ImDrawVert& c = list->VtxBuffer[cmd.VtxOffset + list->IdxBuffer[ia + 2]];
        rasterize_tri(a, b, c, tex, tw, th, bpp, W, H, rgba, x0, y0, x1, y1);
      }
    }
  }
  // 渲染后所有用到的字形已烘焙进 baked 表;缺失字形用 fallback('?')。
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
  EXPECT_GT(missing, 0);  // 中文全部缺失
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
