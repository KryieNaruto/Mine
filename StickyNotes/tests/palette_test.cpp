#include <QTest>
#include "core/palette.h"

class PaletteTest : public QObject {
  Q_OBJECT
private slots:
  void fadedHasLowAlpha() {
    QColor c(255, 187, 77);
    QColor f = fadedBodyColor(c);
    QVERIFY(f.alpha() <= 100);                 // ≈0.35
    QCOMPARE(f.red(), 255);
  }
  void hoverIsOpaque() {
    QColor c(30, 144, 255);
    QCOMPARE(bodyColorHover(c).alpha(), 255);
    QCOMPARE(bodyColorHover(c), c);
  }
  void textContrast() {
    QVERIFY(titleBarTextColor(QColor("white")) == QColor(Qt::black));
    QVERIFY(titleBarTextColor(QColor("black")) == QColor(Qt::white));
  }
};
QTEST_MAIN(PaletteTest)
#include "palette_test.moc"
