#pragma once
#include "imgui.h"
namespace easypainter::app {
// 把含全量简体中文字形的字体加入 atlas。成功返回非空 ImFont*。
ImFont* LoadCjkFont(ImFontAtlas* atlas, const char* font_path, float size_px);
// 返回 CJK_FONT_PATH 编译宏指向的字体文件路径(随项目 bundle)。
const char* cjk_font_path();
}
