#include <QTest>
#include "core/geometry_util.h"

class GeometryTest : public QObject {
  Q_OBJECT
private slots:
  void dockLeft() {
    QRect scr(0,0,1920,1080), win(4, 300, 260, 320);       // 距左边 4 ≤ 25
    auto d = computeDock(win, scr, 500, 500);
    QVERIFY(d.has_value());
    QCOMPARE(d->edge, ScreenEdge::Left);
    QVERIFY(d->hiddenRect.x() < 0 && d->hiddenRect.x() > -win.width());
    QCOMPARE(d->hiddenRect.width(), win.width());
    QVERIFY(d->tabRect.width() >= 8);
  }
  void dockRightTopBottom() {
    QRect scr(0,0,1920,1080);
    QVERIFY(computeDock(QRect(1920-260-4,300,260,320), scr, 500,500)->edge == ScreenEdge::Right);   // 距右 4
    QVERIFY(computeDock(QRect(300,4,260,320),           scr, 500,500)->edge == ScreenEdge::Top);      // 距顶 4
    QVERIFY(computeDock(QRect(300,1080-320-4,260,320),  scr, 500,500)->edge == ScreenEdge::Bottom);   // 距底 4
  }
  void farAwayNoDock() {
    QRect scr(0,0,1920,1080);
    QVERIFY(!computeDock(QRect(800,400,260,320), scr, 800,400).has_value());
  }
  void cursorExpandsTab() {
    QRect scr(0,0,1920,1080);
    auto d = computeDock(QRect(4,300,260,320), scr, 500,500);
    QVERIFY(d.has_value());
    QVERIFY(cursorNearDock(d->tabRect, d->tabRect.center().x(), d->tabRect.center().y()));
    QVERIFY(!cursorNearDock(d->tabRect, 100, 100));
  }
  void snapRight() {
    QRect a(100,100,260,320), b(600,200,260,320);
    QRect r = snappedRect(a, b);
    QCOMPARE(r.x(), a.right() + 8);
    QCOMPARE(r.y(), a.y());
  }
};
QTEST_MAIN(GeometryTest)
#include "geometry_test.moc"
