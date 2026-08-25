#pragma once
#include <QColor>

// 标题栏底 = 标题色（可直接返回标题色）
QColor titleBarColor(const QColor& titleColor);

// 正文背景 = 标题色 alpha 降至 ~0.35 混合在白底上
QColor fadedBodyColor(const QColor& titleColor);

// hover 时正文背景 = 标题色不透明
QColor bodyColorHover(const QColor& titleColor);

// 按亮度选黑/白前景，保证可读
QColor titleBarTextColor(const QColor& bg);
