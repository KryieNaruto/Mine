#include <QTest>
#include "core/note_store.h"

class StoreTest : public QObject {
  Q_OBJECT
private slots:
  void addSetsIdAndIndex() {
    NoteStore s; Note n; n.title = "t";
    QUuid id = s.add(n);
    QCOMPARE(s.notes().size(), 1);
    QVERIFY(!id.isNull());
    QVERIFY(s.find(id));
    QCOMPARE(s.find(id)->title, QString("t"));
  }
  void removeUpdatesIndex() {
    NoteStore s;
    QUuid a = s.add(Note{});
    QUuid b = s.add(Note{});       // 不再保留元素引用（add 返回 id，扩容无悬垂风险）
    s.remove(a);
    QCOMPARE(s.notes().size(), 1);
    QVERIFY(s.find(a) == nullptr);
    QVERIFY(s.find(b));
    QCOMPARE(s.find(b)->id, b);
  }
  void pinToggleEmitsChanged() {
    NoteStore s; QUuid a = s.add(Note{});
    int n = 0; connect(&s, &NoteStore::changed, [&]{ ++n; });
    s.setPinned(a, true);
    QVERIFY(s.find(a)->pinned);
    QCOMPARE(n, 1);
  }
  void taskDone() {
    NoteStore s; QUuid a = s.add(Note{});
    s.addTask(a, "买牛奶");
    QCOMPARE(s.find(a)->tasks.size(), 1);
    s.setTaskDone(a, 0, true);
    QVERIFY(s.find(a)->tasks[0].done);
    QVERIFY(s.find(a)->tasks[0].text == "买牛奶");
  }
};
QTEST_MAIN(StoreTest)
#include "store_test.moc"
