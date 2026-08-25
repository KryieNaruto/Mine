#include <QTest>
#include <QTemporaryDir>
#include "core/note_store.h"
#include "core/persistence.h"

class PersistenceTest : public QObject {
  Q_OBJECT
private slots:
  void roundTrip() {
    QTemporaryDir dir; QString p = dir.filePath("n.json");
    NoteStore s; QUuid id = s.add(Note{});
    s.find(id)->title = "便签"; s.find(id)->bodyText = "正文"; s.find(id)->titleColor = QColor("#ffee88");
    s.find(id)->pinned = true; s.find(id)->pos = QPointF(12, 34); s.find(id)->size = QSizeF(200, 300);
    s.addTask(id, "任务A"); s.setTaskDone(id, 0, true);
    NoteStore t;
    QVERIFY(loadStore(t, p) == false);   // 文件不存在 → false
    QVERIFY(saveStore(s, p));
    QVERIFY(loadStore(t, p));
    QCOMPARE(t.notes().size(), 1);
    auto& n = t.notes()[0];
    QCOMPARE(n.title, QString("便签"));
    QCOMPARE(n.titleColor, QColor("#ffee88"));
    QVERIFY(n.pinned);
    QCOMPARE(n.pos, QPointF(12, 34));
    QCOMPARE(n.size, QSizeF(200, 300));
    QCOMPARE(n.tasks.size(), 1);
    QVERIFY(n.tasks[0].done);
    QCOMPARE(n.tasks[0].text, QString("任务A"));
  }
  void corruptFileReturnsFalse() {
    QTemporaryDir dir; QString p = dir.filePath("bad.json");
    { QFile f(p); f.open(QIODevice::WriteOnly); f.write("{{{not json"); }
    NoteStore t; QUuid beforeId = t.add(Note{});
    QVERIFY(loadStore(t, p) == false);
    QCOMPARE(t.notes().size(), 1);            // store 内容不变
    QCOMPARE(t.notes()[0].id, beforeId);
  }
  void emptyStore() {
    QTemporaryDir dir; QString p = dir.filePath("e.json");
    NoteStore s;
    QVERIFY(saveStore(s, p));
    NoteStore t; QVERIFY(loadStore(t, p));
    QCOMPARE(t.notes().size(), 0);
  }
};
QTEST_MAIN(PersistenceTest)
#include "persistence_test.moc"
