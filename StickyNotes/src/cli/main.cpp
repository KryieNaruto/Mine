#include <QApplication>
#include <QFont>
#include <QFontDatabase>
#include <QImage>
#include <QPainter>
#include <QRectF>
#include <cstdio>
#include <cstring>

#include "app/note_widget.h"
#include "core/model.h"
#include "core/note_store.h"
#include "core/persistence.h"

// STICKYNOTES_FONT_DIR 由 stickynotes_assets INTERFACE 目标注入
#ifndef STICKYNOTES_FONT_DIR
#define STICKYNOTES_FONT_DIR "assets/fonts"
#endif

static void loadCjkFont() {
  int id = QFontDatabase::addApplicationFont(QStringLiteral(STICKYNOTES_FONT_DIR) +
                                             QStringLiteral("/NotoSansCJK-Regular.ttc"));
  if (id < 0) {
    std::fprintf(stderr, "warning: failed to load CJK font\n");
    return;
  }
  const QStringList fams = QFontDatabase::applicationFontFamilies(id);
  if (!fams.isEmpty())
    QApplication::setFont(QFont(fams.first(), 10));
}

static Note makeFixtureNote1() {
  Note n;
  n.title = QStringLiteral("学习计划");
  n.titleColor = QColor("#ffb74d");
  n.pinned = true;
  n.bodyText = QStringLiteral("复习 Qt Widgets\n完成单元测试");
  n.pos = QPointF(100, 100);
  n.size = QSizeF(260, 320);
  n.tasks = {TaskItem{QStringLiteral("阅读设计文档"), true},
             TaskItem{QStringLiteral("编写单元测试"), false}};
  return n;
}

static Note makeFixtureNote2() {
  Note n;
  n.title = QStringLiteral("买菜清单");
  n.titleColor = QColor("#4fc3f7");
  n.pinned = false;
  n.bodyText = QStringLiteral("周末采购");
  n.pos = QPointF(400, 100);
  n.size = QSizeF(260, 320);
  n.tasks = {TaskItem{QStringLiteral("西红柿"), false},
             TaskItem{QStringLiteral("鸡蛋"), true},
             TaskItem{QStringLiteral("牛奶"), false}};
  return n;
}

static QString defaultStorePath() { return QStringLiteral("stickynotes.json"); }

static int cmdList(const QString& storePath) {
  NoteStore s;
  loadStore(s, storePath);
  for (const Note& n : s.notes()) {
    int done = 0;
    for (const TaskItem& t : n.tasks)
      if (t.done) ++done;
    std::printf("%s %s [%s] tasks(%d/%d)\n",
                n.id.toString(QUuid::WithoutBraces).toUtf8().constData(),
                n.title.toUtf8().constData(),
                n.pinned ? "pinned" : "",
                done, n.tasks.size());
  }
  return 0;
}

static int cmdAdd(const QString& title, const QString& storePath) {
  NoteStore s;
  loadStore(s, storePath);
  Note n;
  n.title = title;
  QUuid id = s.add(n);
  if (!saveStore(s, storePath)) {
    std::fprintf(stderr, "error: save failed\n");
    return 1;
  }
  std::printf("%s\n", id.toString(QUuid::WithoutBraces).toUtf8().constData());
  return 0;
}

static int cmdRemove(const QString& idStr, const QString& storePath) {
  NoteStore s;
  loadStore(s, storePath);
  QUuid id(idStr);
  s.remove(id);
  if (!saveStore(s, storePath)) {
    std::fprintf(stderr, "error: save failed\n");
    return 1;
  }
  return 0;
}

static int cmdPin(const QString& idStr, const QString& onOff, const QString& storePath) {
  NoteStore s;
  loadStore(s, storePath);
  QUuid id(idStr);
  if (!s.find(id)) {
    std::fprintf(stderr, "error: no such note\n");
    return 1;
  }
  s.setPinned(id, onOff == QStringLiteral("on"));
  if (!saveStore(s, storePath)) {
    std::fprintf(stderr, "error: save failed\n");
    return 1;
  }
  return 0;
}

static int cmdRender(const QString& outPath, const QString& storePath) {
  NoteStore store;
  bool loaded = loadStore(store, storePath);
  // 空 store（或文件不存在）→ 内置 fixture（golden 基准）
  if (!loaded || store.notes().isEmpty()) {
    store.add(makeFixtureNote1());
    store.add(makeFixtureNote2());
  }

  // 画布 = 所有便签 pos+size 并集 + 边距
  QRectF bounds;
  bool first = true;
  for (const Note& n : store.notes()) {
    QRectF r(n.pos, n.size);
    bounds = first ? r : bounds.united(r);
    first = false;
  }
  bounds = bounds.adjusted(-20, -20, 20, 20);
  QImage canvas(bounds.size().toSize(), QImage::Format_ARGB32_Premultiplied);
  canvas.fill(Qt::white);

  QPainter p(&canvas);
  p.setRenderHint(QPainter::Antialiasing, true);
  for (const Note& n : store.notes()) {
    NoteWidget w(store, n.id);
    w.resize(n.size.toSize());
    w.render(&p, n.pos.toPoint() - bounds.topLeft().toPoint());
  }
  p.end();

  if (!canvas.save(outPath)) {
    std::fprintf(stderr, "error: save png failed\n");
    return 1;
  }
  std::printf("rendered %dx%d -> %s\n", canvas.width(), canvas.height(),
              outPath.toUtf8().constData());
  return 0;
}

int main(int argc, char** argv) {
  QApplication app(argc, argv);
  loadCjkFont();

  if (argc < 2) {
    std::fprintf(stderr,
                 "usage:\n"
                 "  stickynotes-cli --list [store.json]\n"
                 "  stickynotes-cli --add \"<title>\" [store.json]\n"
                 "  stickynotes-cli --remove <id> [store.json]\n"
                 "  stickynotes-cli --pin <id> <on|off> [store.json]\n"
                 "  stickynotes-cli --render <out.png> [store.json]\n");
    return 2;
  }
  const QString cmd = QString::fromLocal8Bit(argv[1]);
  auto storeAt = [&](int base) -> QString {
    return base < argc ? QString::fromLocal8Bit(argv[base]) : defaultStorePath();
  };

  if (cmd == QStringLiteral("--list")) {
    return cmdList(storeAt(2));
  } else if (cmd == QStringLiteral("--add")) {
    if (argc < 3) return 2;
    return cmdAdd(QString::fromLocal8Bit(argv[2]), storeAt(3));
  } else if (cmd == QStringLiteral("--remove")) {
    if (argc < 3) return 2;
    return cmdRemove(QString::fromLocal8Bit(argv[2]), storeAt(3));
  } else if (cmd == QStringLiteral("--pin")) {
    if (argc < 4) return 2;
    return cmdPin(QString::fromLocal8Bit(argv[2]), QString::fromLocal8Bit(argv[3]), storeAt(4));
  } else if (cmd == QStringLiteral("--render")) {
    if (argc < 3) return 2;
    return cmdRender(QString::fromLocal8Bit(argv[2]), storeAt(3));
  }
  std::fprintf(stderr, "error: unknown command %s\n", cmd.toUtf8().constData());
  return 2;
}
