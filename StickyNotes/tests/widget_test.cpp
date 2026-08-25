#include <QTest>
#include <QApplication>
#include <QCheckBox>
#include <QLabel>
#include "app/note_window.h"
#include "core/note_store.h"
#include "core/palette.h"

class WidgetTest : public QObject {
  Q_OBJECT
private slots:
  void framelessAndPinFlags() {
    NoteStore s; QUuid id = s.add(Note{});
    NoteWindow w(s, id);                       // 构造签名 (NoteStore&, QUuid)
    w.show();
    QVERIFY(w.windowFlags() & Qt::FramelessWindowHint);          // R3 无边框（审阅补）
    QVERIFY(!(w.windowFlags() & Qt::WindowStaysOnTopHint));
    w.setPinned(true);
    QVERIFY(w.windowFlags() & Qt::WindowStaysOnTopHint);
    w.setPinned(false);
    QVERIFY(!(w.windowFlags() & Qt::WindowStaysOnTopHint));
  }
  void taskDoneShowsStrikeout() {
    NoteStore s; QUuid id = s.add(Note{});
    s.addTask(id, "任务");
    NoteWindow w(s, id);                       // 构造签名 (NoteStore&, QUuid)
    w.show();
    auto* ck = w.findChild<QCheckBox*>("taskCheck0");            // 精确 objectName
    QVERIFY(ck);
    ck->setChecked(true);                                        // 触发模型 done
    QVERIFY(s.find(id)->tasks[0].done);
    auto* lbl = w.findChild<QLabel*>("taskLabel0");
    QVERIFY(lbl);
    QVERIFY(lbl->font().strikeOut());                            // R5 删除线
  }
  void bodyColorAccessorFadesOnLeave() {
    NoteStore s; QUuid id = s.add(Note{}); s.find(id)->titleColor = QColor("#ffb74d");
    NoteWindow w(s, id);                       // 构造签名 (NoteStore&, QUuid)
    // currentBodyColor() 由内部 hover 状态驱动；离屏下初始为离开态 → 淡化色
    QVERIFY(w.noteWidget()->currentBodyColor().alpha() <= 100);  // 解耦实现方式
    w.noteWidget()->setHoveredForTest(true);                     // 测试辅助：模拟 hover
    QCOMPARE(w.noteWidget()->currentBodyColor(), QColor("#ffb74d"));
  }
  void pinnedStillDocks() {
    NoteStore s; QUuid id = s.add(Note{});
    NoteWindow w(s, id);                       // 构造签名 (NoteStore&, QUuid)
    w.show();
    w.setPinned(true);                                           // R2：置顶仍可收齐
    DockState d; d.docked = true; d.hiddenRect = QRect(-252, 300, 260, 320);
    w.setDocked(d);
    QVERIFY(w.dockState().docked);                               // 收齐路径不受 pin 影响
  }
};
QTEST_MAIN(WidgetTest)
#include "widget_test.moc"
