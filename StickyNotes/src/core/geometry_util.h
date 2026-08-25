#pragma once
#include <QRect>
#include <optional>

enum class ScreenEdge { Top, Bottom, Left, Right };

struct DockState {
  ScreenEdge edge = ScreenEdge::Left;
  QRect hiddenRect;
  QRect tabRect;
  bool docked = false;
};

// 判定「距边足够近」的距离上限（默认 25）；reveal = 收齐后标签露出的厚度（默认 8）。
std::optional<DockState> computeDock(const QRect& winRect, const QRect& screen,
                                     int cursorX, int cursorY,
                                     int threshold = 25, int reveal = 8);

// 鼠标是否进入标签热区（展开条件）
bool cursorNearDock(const QRect& tabRect, int cursorX, int cursorY, int pad = 6);

// 组吸附：使 b 贴到 a 右侧并垂直对齐，返回 b 的新几何
QRect snappedRect(const QRect& a, const QRect& b, int gap = 8);
