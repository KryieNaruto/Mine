#include <QTest>
#include <QTemporaryDir>
#include <QImage>
#include <QProcess>

class ImageGoldenTest : public QObject {
  Q_OBJECT
private slots:
  void cliRenderMatchesGolden() {
    // 自包含：子进程必须离屏，显式设置而非依赖 ctest 全局 env 前缀（单独运行不假红）
    qputenv("QT_QPA_PLATFORM", "offscreen");
    QTemporaryDir dir;
    QString out = dir.filePath("render.png");
    QString emptyStore = dir.filePath("empty.json");
    { QFile f(emptyStore); f.open(QIODevice::WriteOnly); f.write("{\"notes\":[]}"); }  // 空 store → 走 fixture
    QProcess p;
    p.start(QString(STICKYNOTES_CLI), {"--render", out, emptyStore});  // 绝对路径 + 显式空 store（审阅补）
    QVERIFY(p.waitForFinished(30000));
    QCOMPARE(p.exitStatus(), QProcess::NormalExit);
    QCOMPARE(p.exitCode(), 0);
    QImage got(out);
    QImage ref(QString(STICKYNOTES_GOLDEN_DIR) + "/stickynotes_golden.png");
    QVERIFY(!got.isNull() && !ref.isNull());
    QCOMPARE(got.size(), ref.size());
    int diff = 0;
    for (int y = 0; y < got.height(); ++y)
      for (int x = 0; x < got.width(); ++x)
        if (got.pixel(x, y) != ref.pixel(x, y)) ++diff;
    // 容差统一：按实测设定（golden 生成同环境 Qt6.4.2/offscreen 应为像素级一致；
    // 若实测有 1-2px 渲染差异，在 README 记录并按实测放宽此值——不预设）
    QVERIFY2(diff <= (int)(got.width()*got.height()*0.001),
             "pixel diff over tolerance");
  }
};
QTEST_MAIN(ImageGoldenTest)
#include "image_golden_test.moc"
