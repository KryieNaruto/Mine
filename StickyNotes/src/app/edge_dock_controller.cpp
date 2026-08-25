#include "app/edge_dock_controller.h"

#include <QCursor>
#include "app/note_group.h"
#include "app/note_window.h"
#include "core/geometry_util.h"

namespace {
const int kSnapThreshold = 25;
}

EdgeDockController::EdgeDockController(QScreen* screen,
                                       std::function<QList<NoteWindow*>()> windowEnum,
                                       QObject* parent)
    : QObject(parent), screen_(screen), windowEnum_(std::move(windowEnum)) {
  connect(&timer_, &QTimer::timeout, this, &EdgeDockController::onTick);
  timer_.setInterval(150);
}

void EdgeDockController::start() {
  timer_.start();
}

void EdgeDockController::registerWindow(NoteWindow* w) {
  connect(w, &NoteWindow::dragStarted, this, [this, w]() { onDragStarted(w); });
  connect(w, &NoteWindow::dragEnded, this, [this, w]() { onDragEnded(w); });
}

void EdgeDockController::unregisterWindow(NoteWindow* w) {
  disconnect(w, nullptr, this, nullptr);
  if (NoteGroup* g = groupOf(w))
    g->remove(w);
}

void EdgeDockController::resetAllDocks() {
  onTick();
}

void EdgeDockController::onTick() {
  if (!screen_)
    return;
  const QList<NoteWindow*> windows = windowEnum_();
  const QPoint cur = QCursor::pos();
  QList<NoteWindow*> keepDocked;

  for (NoteWindow* w : windows) {
    if (!w->isVisible())
      continue;
    auto d = computeDock(w->geometry(), screen_->availableGeometry(), cur.x(), cur.y());
    bool shouldDock = false;
    if (d.has_value()) {
      bool cursorInTab = cursorNearDock(d->tabRect, cur.x(), cur.y());
      shouldDock = !cursorInTab;
      if (cursorInTab) {
        DockState expand;
        expand.docked = false;
        w->setDocked(expand);
      }
    }
    if (shouldDock) {
      w->setDocked(*d);
      keepDocked.append(w);
    }
  }
  dockedWindows_ = keepDocked;
}

void EdgeDockController::onDragStarted(NoteWindow* w) {
  // 拖出即脱离所在组（拆合），便于独立移动后再吸附
  if (NoteGroup* g = groupOf(w)) {
    g->remove(w);
    if (g->members().isEmpty()) {
      groups_.removeOne(g);
      g->deleteLater();
    }
  }
}

void EdgeDockController::onDragEnded(NoteWindow* w) {
  maybeSnap(w);
}

NoteGroup* EdgeDockController::groupOf(NoteWindow* w) {
  for (NoteGroup* g : groups_)
    if (g->members().contains(w))
      return g;
  return nullptr;
}

void EdgeDockController::maybeSnap(NoteWindow* w) {
  const QList<NoteWindow*> windows = windowEnum_();
  for (NoteWindow* other : windows) {
    if (other == w)
      continue;
    const QRect a = w->geometry();
    const QRect b = other->geometry();
    // w 在 other 右侧：把 w 贴到 other 右侧（同高对齐）
    if (qAbs(b.right() - a.left()) <= kSnapThreshold) {
      QRect snapped = snappedRect(b, a);
      w->move(snapped.topLeft());
      mergeIntoGroup(w, other);
      return;
    }
    // w 在 other 左侧：把 other 贴到 w 右侧
    if (qAbs(a.right() - b.left()) <= kSnapThreshold) {
      QRect snapped = snappedRect(a, b);
      other->move(snapped.topLeft());
      mergeIntoGroup(w, other);
      return;
    }
  }
}

void EdgeDockController::mergeIntoGroup(NoteWindow* a, NoteWindow* b) {
  NoteGroup* ga = groupOf(a);
  NoteGroup* gb = groupOf(b);
  if (ga && ga == gb)
    return;
  NoteGroup* target = ga ? ga : (gb ? gb : new NoteGroup());
  if (gb && gb != target) {
    for (NoteWindow* m : gb->members())
      target->add(m);
    groups_.removeOne(gb);
    gb->deleteLater();
  }
  target->add(a);
  target->add(b);
  if (!groups_.contains(target))
    groups_.append(target);
}
