#include <QApplication>
#include <QFont>
#include <QFontDatabase>
#include <QScreen>
#include <QSet>
#include <QTimer>
#include <QHash>

#include "app/edge_dock_controller.h"
#include "app/note_group.h"
#include "app/note_window.h"
#include "core/note_store.h"
#include "core/persistence.h"

#ifndef STICKYNOTES_FONT_DIR
#define STICKYNOTES_FONT_DIR "assets/fonts"
#endif

static const QString kStorePath = QStringLiteral("stickynotes.json");

int main(int argc, char** argv) {
  QApplication app(argc, argv);

  int fid = QFontDatabase::addApplicationFont(
      QStringLiteral(STICKYNOTES_FONT_DIR) + QStringLiteral("/NotoSansCJK-Regular.ttc"));
  if (fid >= 0) {
    const QStringList fams = QFontDatabase::applicationFontFamilies(fid);
    if (!fams.isEmpty())
      app.setFont(QFont(fams.first(), 10));
  }

  NoteStore store;
  loadStore(store, kStorePath);

  QHash<QUuid, NoteWindow*> windows;
  auto createWindow = [&](QUuid id) {
    auto* w = new NoteWindow(store, id);
    w->show();
    windows.insert(id, w);
    return w;
  };
  auto removeWindow = [&](QUuid id) {
    if (NoteWindow* w = windows.take(id)) {
      w->close();
      w->deleteLater();
    }
  };

  // 先装配已有便签
  for (const Note& n : store.notes())
    createWindow(n.id);

  // 控制器：屏幕 + 窗口枚举
  QScreen* screen = QGuiApplication::primaryScreen();
  EdgeDockController* controller = nullptr;
  if (screen) {
    controller = new EdgeDockController(
        screen, [&]() { return QList<NoteWindow*>(windows.values().begin(), windows.values().end()); },
        &app);
    for (NoteWindow* w : windows)
      controller->registerWindow(w);
    controller->start();
  }

  // 运行时增删便签装配（闭环 §5.2 防悬垂）：store 变更 → 对比窗口集合
  QObject::connect(&store, &NoteStore::changed, &app, [&]() {
    QSet<QUuid> live;
    for (const Note& n : store.notes())
      live.insert(n.id);
    // 关闭已删除
    QList<QUuid> toRemove;
    for (auto it = windows.constBegin(); it != windows.constEnd(); ++it)
      if (!live.contains(it.key()))
        toRemove.append(it.key());
    for (const QUuid& id : toRemove) {
      if (controller)
        controller->unregisterWindow(windows.value(id));
      removeWindow(id);
    }
    // 新建新增
    for (const Note& n : store.notes()) {
      if (!windows.contains(n.id)) {
        NoteWindow* w = createWindow(n.id);
        if (controller)
          controller->registerWindow(w);
      }
    }
  });

  // 防抖自动保存 ~500ms
  QTimer* saveTimer = new QTimer(&app);
  saveTimer->setInterval(500);
  saveTimer->setSingleShot(true);
  QObject::connect(&store, &NoteStore::changed, &app, [&, saveTimer]() {
    saveTimer->start();
  });
  QObject::connect(saveTimer, &QTimer::timeout, &app, [&]() {
    saveStore(store, kStorePath);
  });

  return app.exec();
}
