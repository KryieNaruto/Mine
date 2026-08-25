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

const char* cjk_font_path() {
#ifdef CJK_FONT_PATH
  return CJK_FONT_PATH;
#else
  return "";
#endif
}

}  // namespace easypainter::app
