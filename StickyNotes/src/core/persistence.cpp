#include "core/persistence.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

static QJsonObject taskToJson(const TaskItem& t) {
  QJsonObject o;
  o["text"] = t.text;
  o["done"] = t.done;
  return o;
}

static QJsonObject noteToJson(const Note& n) {
  QJsonObject o;
  o["id"] = n.id.toString(QUuid::WithoutBraces);
  o["title"] = n.title;
  o["titleColor"] = n.titleColor.name();
  o["bodyText"] = n.bodyText;
  o["pinned"] = n.pinned;
  QJsonArray tasks;
  for (const TaskItem& t : n.tasks)
    tasks.append(taskToJson(t));
  o["tasks"] = tasks;
  o["pos"] = QJsonArray{n.pos.x(), n.pos.y()};
  o["size"] = QJsonArray{n.size.width(), n.size.height()};
  return o;
}

bool saveStore(const NoteStore& store, const QString& path) {
  QJsonObject root;
  QJsonArray notes;
  for (const Note& n : store.notes())
    notes.append(noteToJson(n));
  root["notes"] = notes;
  QFile f(path);
  if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate))
    return false;
  f.write(QJsonDocument(root).toJson());
  return true;
}

static bool taskFromJson(const QJsonObject& o, TaskItem& t) {
  if (!o.contains("text") || !o.contains("done"))
    return false;
  t.text = o["text"].toString();
  t.done = o["done"].toBool();
  return true;
}

static bool noteFromJson(const QJsonObject& o, Note& n) {
  if (!o.contains("id"))
    return false;
  n.id = QUuid(o["id"].toString());
  if (n.id.isNull())
    return false;
  n.title = o["title"].toString();
  n.titleColor = QColor(o["titleColor"].toString());
  if (!n.titleColor.isValid())
    n.titleColor = QColor("#ffb74d");
  n.bodyText = o["bodyText"].toString();
  n.pinned = o["pinned"].toBool();
  n.tasks.clear();
  const QJsonArray tasks = o["tasks"].toArray();
  for (const QJsonValue& v : tasks) {
    TaskItem t;
    if (taskFromJson(v.toObject(), t))
      n.tasks.append(t);
  }
  if (o.contains("pos")) {
    QJsonArray a = o["pos"].toArray();
    if (a.size() == 2)
      n.pos = QPointF(a[0].toDouble(), a[1].toDouble());
  }
  if (o.contains("size")) {
    QJsonArray a = o["size"].toArray();
    if (a.size() == 2)
      n.size = QSizeF(a[0].toDouble(), a[1].toDouble());
  }
  return true;
}

bool loadStore(NoteStore& store, const QString& path) {
  QFile f(path);
  if (!f.open(QIODevice::ReadOnly))
    return false;
  QJsonParseError err;
  QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
  if (err.error != QJsonParseError::NoError)
    return false;
  if (!doc.isObject())
    return false;
  QJsonObject root = doc.object();
  if (!root.contains("notes"))
    return false;
  NoteStore tmp;
  const QJsonArray notes = root["notes"].toArray();
  for (const QJsonValue& v : notes) {
    Note n;
    if (noteFromJson(v.toObject(), n))
      tmp.add(n);
  }
  // 全部解析成功后才替换 store 内容（失败路径不改动原 store）。
  // NoteStore 为 QObject 不可拷贝/移动赋值，直接替换内部容器。
  store.notes().swap(tmp.notes());
  store.rebuildIndex();
  return true;
}
