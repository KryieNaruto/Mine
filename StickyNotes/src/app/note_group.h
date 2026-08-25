#pragma once
#include <QList>
#include <QObject>
#include <QPoint>
#include <QSet>

class NoteWindow;

// 多窗吸附组：组内成员随组整体平移，支持增删（重复/不存在返回 false）。
class NoteGroup : public QObject {
  Q_OBJECT
public:
  bool add(NoteWindow* w);
  bool remove(NoteWindow* w);
  QList<NoteWindow*> members() const;
  void moveBy(const QPoint& delta);
  void moveBy(int dx, int dy) { moveBy(QPoint(dx, dy)); }

private:
  QSet<NoteWindow*> members_;
};
