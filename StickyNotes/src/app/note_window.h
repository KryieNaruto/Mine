#pragma once
#include <QWidget>
#include <QUuid>
#include "app/note_widget.h"
#include "core/geometry_util.h"
#include "core/note_store.h"

class QPropertyAnimation;

// 无边框便签窗口：内部装配 NoteWidget；管理置顶 flag 与收齐动画。
class NoteWindow : public QWidget {
  Q_OBJECT
public:
  NoteWindow(NoteStore& store, QUuid noteId, QWidget* parent = nullptr);

  bool pinned() const { return pinned_; }
  void setPinned(bool on);                    // Qt::WindowStaysOnTopHint
  DockState dockState() const { return dockState_; }
  void setDocked(const DockState& d);         // QPropertyAnimation 到 hiddenRect / 回 shownRect
  NoteWidget* noteWidget() const { return widget_; }

signals:
  void dragStarted();
  void dragEnded();

private:
  NoteStore& store_;
  QUuid id_;
  NoteWidget* widget_ = nullptr;
  bool pinned_ = false;
  DockState dockState_;
  QRect shownRect_;
  QPropertyAnimation* anim_ = nullptr;
  QPoint lastDragGlobal_;

  void onStoreChanged();
};
