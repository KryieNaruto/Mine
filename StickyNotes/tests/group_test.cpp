#include <QTest>
#include "app/note_group.h"
#include "app/note_window.h"
#include "core/note_store.h"
#include "core/geometry_util.h"

class GroupTest : public QObject {
  Q_OBJECT
private slots:
  void addRemoveMembers() {
    NoteStore s;
    QUuid a = s.add(Note{}); NoteWindow w1(s, a);                // 与 Task 6 构造签名 (NoteStore&, QUuid) 一致
    QUuid b = s.add(Note{}); NoteWindow w2(s, b);
    NoteGroup g;
    QVERIFY(g.add(&w1)); QVERIFY(g.add(&w2));
    QVERIFY(!g.add(&w1));                 // 重复
    QCOMPARE(g.members().size(), 2);
    QVERIFY(g.remove(&w1));
    QCOMPARE(g.members().size(), 1);
  }
  void moveByMovesMembers() {            // 闭环 spec §5.3 R6「组几何平移单测」
    NoteStore s;
    QUuid a = s.add(Note{}); NoteWindow w1(s, a); w1.show();
    QUuid b = s.add(Note{}); NoteWindow w2(s, b); w2.show();
    w1.move(100, 100); w2.move(400, 100);
    NoteGroup g; g.add(&w1); g.add(&w2);
    QPoint p1 = w1.pos(), p2 = w2.pos();
    g.moveBy(50, 30);
    QCOMPARE(w1.pos(), p1 + QPoint(50, 30));     // 组内成员随组整体平移
    QCOMPARE(w2.pos(), p2 + QPoint(50, 30));
  }
  void snapGeometry() {
    QRect a(100,100,260,320), b(600,200,260,320);
    QRect r = snappedRect(a, b);
    QCOMPARE(r.x(), a.right() + 8);
    QCOMPARE(r.y(), a.y());
  }
};
QTEST_MAIN(GroupTest)
#include "group_test.moc"
