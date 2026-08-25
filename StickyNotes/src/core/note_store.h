#pragma once
#include <QHash>
#include <QObject>
#include <QVector>
#include "core/model.h"

class NoteStore : public QObject {
  Q_OBJECT
public:
  QUuid add(const Note& note);   // 返回新 id，不返回容器引用（防 QVector 扩容悬垂）
  void remove(QUuid id);
  Note* find(QUuid id);
  QVector<Note>& notes() { return notes_; }
  const QVector<Note>& notes() const { return notes_; }

  void setTitle(QUuid id, const QString& v);
  void setBodyText(QUuid id, const QString& v);
  void setTitleColor(QUuid id, const QColor& c);
  void setPinned(QUuid id, bool v);
  void addTask(QUuid id, const QString& text);
  void setTaskDone(QUuid id, int index, bool done);
signals:
  void changed();
private:
  QVector<Note> notes_;
  QHash<QUuid, int> index_;
};
