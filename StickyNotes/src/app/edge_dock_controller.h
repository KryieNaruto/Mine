#pragma once
#include <QList>
#include <QObject>
#include <QScreen>
#include <QTimer>
#include <functional>

class NoteWindow;
class NoteGroup;

// 边缘收齐/拉出控制器：定时器驱动每窗 computeDock；拖动结束做组吸附。
class EdgeDockController : public QObject {
  Q_OBJECT
public:
  EdgeDockController(QScreen* screen,
                     std::function<QList<NoteWindow*>()> windowEnum,
                     QObject* parent = nullptr);

  void start();
  void registerWindow(NoteWindow* w);   // 运行时新增便签时注册（连接 drag 信号）
  void unregisterWindow(NoteWindow* w);
  void resetAllDocks();                 // 主程序装配完成后对既有窗做一次状态同步

private:
  QScreen* screen_;
  std::function<QList<NoteWindow*>()> windowEnum_;
  QTimer timer_;
  QList<NoteWindow*> dockedWindows_;
  QList<NoteGroup*> groups_;

  void onTick();
  void onDragStarted(NoteWindow* w);    // 分离所在组
  void onDragEnded(NoteWindow* w);      // 组吸附
  NoteGroup* groupOf(NoteWindow* w);
  void maybeSnap(NoteWindow* w);
  void mergeIntoGroup(NoteWindow* a, NoteWindow* b);
};
