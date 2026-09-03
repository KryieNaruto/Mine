#include <QTest>
#include <QFile>
#include <QTemporaryDir>
#include <QApplication>

#include "app/note_window.h"
#include "app/startup.h"
#include "core/note_store.h"
#include "core/persistence.h"

// 首启播种回归:开箱即有 ≥1 便签窗口;已有数据/删光文件一律不干预。
class StartupTest : public QObject {
  Q_OBJECT
private slots:
  void firstRun_noStoreFile_seedsBlankNote() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath("stickynotes.json");
    NoteStore store;
    QVERIFY(!loadStore(store, path));               // 无文件 → load false,store 空
    QVERIFY(store.notes().isEmpty());
    QVERIFY(seedFirstRunNoteIfMissing(store, path)); // 真首启 → 播种
    QCOMPARE(store.notes().size(), 1);
    QVERIFY(QFile::exists(path));                   // 已落盘
    QVERIFY(!store.notes().first().id.isNull());    // id 已生成
    // 冒烟「有界面」:播种便签能建出顶层 NoteWindow(offscreen 下不断言 isVisible)
    NoteWindow w(store, store.notes().first().id);
    w.show();
    QVERIFY(w.noteWidget() != nullptr);
    QVERIFY(w.isWindow());
    QVERIFY(QApplication::topLevelWidgets().contains(&w));
  }
  void existingFile_withNotes_unchanged() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath("stickynotes.json");
    NoteStore seed;
    seed.add(Note{});
    QVERIFY(saveStore(seed, path));                 // 预置 1 便签
    NoteStore store;
    QVERIFY(loadStore(store, path));
    QCOMPARE(store.notes().size(), 1);
    QVERIFY(!seedFirstRunNoteIfMissing(store, path)); // 已有文件 → 不播种
    QCOMPARE(store.notes().size(), 1);
  }
  void existingFile_emptyArray_unchanged() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    const QString path = dir.filePath("stickynotes.json");
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Truncate));
    f.write(QByteArrayLiteral("{\"notes\":[]}"));
    f.close();
    NoteStore store;
    QVERIFY(loadStore(store, path));
    QCOMPARE(store.notes().size(), 0);
    QVERIFY(!seedFirstRunNoteIfMissing(store, path)); // 尊重用户删光 → 不播种
    QCOMPARE(store.notes().size(), 0);
  }
};
QTEST_MAIN(StartupTest)
#include "startup_test.moc"
