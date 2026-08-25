#include "app/note_group.h"
#include "app/note_window.h"

bool NoteGroup::add(NoteWindow* w) {
  if (!w || members_.contains(w))
    return false;
  members_.insert(w);
  return true;
}

bool NoteGroup::remove(NoteWindow* w) {
  return members_.remove(w) > 0;
}

QList<NoteWindow*> NoteGroup::members() const {
  return QList<NoteWindow*>(members_.begin(), members_.end());
}

void NoteGroup::moveBy(const QPoint& delta) {
  for (NoteWindow* w : members_)
    w->move(w->pos() + delta);
}
