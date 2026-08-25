#include "core/geometry_util.h"

#include <climits>

std::optional<DockState> computeDock(const QRect& winRect, const QRect& screen,
                                     int /*cursorX*/, int /*cursorY*/,
                                     int threshold, int reveal) {
  int left = winRect.x();
  int right = screen.right() - winRect.right();
  int top = winRect.y();
  int bottom = screen.bottom() - winRect.bottom();

  ScreenEdge edge;
  int dist;
  if (left <= right && left <= top && left <= bottom) { edge = ScreenEdge::Left; dist = left; }
  else if (right <= top && right <= bottom)           { edge = ScreenEdge::Right; dist = right; }
  else if (top <= bottom)                              { edge = ScreenEdge::Top; dist = top; }
  else                                                 { edge = ScreenEdge::Bottom; dist = bottom; }

  if (dist > threshold)
    return std::nullopt;

  QRect hidden = winRect;
  switch (edge) {
    case ScreenEdge::Left:
      hidden.moveLeft(screen.left() - winRect.width() + reveal);
      break;
    case ScreenEdge::Right:
      hidden.moveLeft(screen.right() - reveal);
      break;
    case ScreenEdge::Top:
      hidden.moveTop(screen.top() - winRect.height() + reveal);
      break;
    case ScreenEdge::Bottom:
      hidden.moveTop(screen.bottom() - reveal);
      break;
  }
  DockState d;
  d.edge = edge;
  d.hiddenRect = hidden;
  d.tabRect = hidden;          // 收齐态可见区即鼠标悬停热区
  d.docked = true;
  return d;
}

bool cursorNearDock(const QRect& tabRect, int cursorX, int cursorY, int pad) {
  return tabRect.adjusted(-pad, -pad, pad, pad).contains(cursorX, cursorY);
}

QRect snappedRect(const QRect& a, const QRect& b, int gap) {
  return QRect(a.right() + gap, a.y(), b.width(), b.height());
}
