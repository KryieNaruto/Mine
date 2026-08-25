#include "core/palette.h"

#include <QColor>

QColor titleBarColor(const QColor& titleColor) {
  return titleColor;
}

QColor fadedBodyColor(const QColor& titleColor) {
  // alpha 89 ≈ 0.35 * 255；铺在白底上等价于低透明度淡化
  return QColor(titleColor.red(), titleColor.green(), titleColor.blue(), 89);
}

QColor bodyColorHover(const QColor& titleColor) {
  return QColor(titleColor.red(), titleColor.green(), titleColor.blue(), 255);
}

QColor titleBarTextColor(const QColor& bg) {
  // 亮度足够亮 → 黑字，否则白字
  return qGray(bg.red(), bg.green(), bg.blue()) > 128 ? QColor(Qt::black) : QColor(Qt::white);
}
