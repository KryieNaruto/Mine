#include "core/note_store.h"

QUuid NoteStore::add(const Note& note) {
  Note n = note;
  if (n.id.isNull())
    n.id = QUuid::createUuid();
  index_.insert(n.id, notes_.size());
  notes_.append(n);
  emit changed();
  return n.id;
}

void NoteStore::remove(QUuid id) {
  auto it = index_.find(id);
  if (it == index_.end())
    return;
  int idx = it.value();
  notes_.removeAt(idx);
  index_.remove(id);
  // 重排被移除位置之后的索引
  for (int i = idx; i < notes_.size(); ++i)
    index_.insert(notes_[i].id, i);
  emit changed();
}

Note* NoteStore::find(QUuid id) {
  auto it = index_.find(id);
  if (it == index_.end())
    return nullptr;
  return &notes_[it.value()];
}

void NoteStore::setTitle(QUuid id, const QString& v) {
  if (Note* n = find(id)) { n->title = v; emit changed(); }
}

void NoteStore::setBodyText(QUuid id, const QString& v) {
  if (Note* n = find(id)) { n->bodyText = v; emit changed(); }
}

void NoteStore::setTitleColor(QUuid id, const QColor& c) {
  if (Note* n = find(id)) { n->titleColor = c; emit changed(); }
}

void NoteStore::setPinned(QUuid id, bool v) {
  if (Note* n = find(id)) { n->pinned = v; emit changed(); }
}

void NoteStore::addTask(QUuid id, const QString& text) {
  if (Note* n = find(id)) {
    TaskItem t; t.text = text; t.done = false;
    n->tasks.append(t);
    emit changed();
  }
}

void NoteStore::setTaskDone(QUuid id, int index, bool done) {
  Note* n = find(id);
  if (n && index >= 0 && index < n->tasks.size()) {
    n->tasks[index].done = done;
    emit changed();
  }
}
