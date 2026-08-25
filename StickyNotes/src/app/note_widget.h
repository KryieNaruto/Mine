#pragma once
#include <QWidget>
#include <QUuid>
#include <QVector>
#include "core/note_store.h"

class QLineEdit;
class QTextEdit;
class QToolButton;
class QCheckBox;
class QLabel;
class QVBoxLayout;

// 便签内容控件：标题行 + 正文 + 任务列表。
// 持 QUuid + NoteStore&（不持 Note& 容器引用），refresh() 经 find(id_) 解析当前 Note。
class NoteWidget : public QWidget {
  Q_OBJECT
public:
  NoteWidget(NoteStore& store, QUuid id, QWidget* parent = nullptr);

  void refresh();
  QColor currentBodyColor() const;   // 断言用状态访问器：当前正文背景色
  void setDocked(bool docked);
  void setHoveredForTest(bool h);    // 测试辅助：模拟 hover，仅设内部标志并刷新

signals:
  void titleEdited(const QString& text);
  void colorPicked(const QColor& c);
  void pinToggled(bool on);
  void deleteRequested();
  void splitRequested();
  void addRequested();
  void taskToggled(int index, bool done);
  void dragStart(const QPoint& globalPos);
  void dragMove(const QPoint& globalPos);
  void dragEnd();

protected:
  void enterEvent(QEnterEvent* e) override;
  void leaveEvent(QEvent* e) override;
  bool eventFilter(QObject* obj, QEvent* ev) override;

private:
  NoteStore& store_;
  QUuid id_;
  bool hovered_ = false;
  bool docked_ = false;
  QColor bodyColor_;

  QWidget* titleBar_ = nullptr;
  QLineEdit* titleEdit_ = nullptr;
  QToolButton* pinBtn_ = nullptr;
  QToolButton* colorBtn_ = nullptr;
  QToolButton* splitBtn_ = nullptr;
  QToolButton* deleteBtn_ = nullptr;
  QToolButton* addBtn_ = nullptr;
  QTextEdit* bodyArea_ = nullptr;
  QWidget* tasksWidget_ = nullptr;
  QVBoxLayout* tasksLayout_ = nullptr;
  QVector<QWidget*> taskRows_;

  void rebuildTasks(const Note* note);
  void applyColors();
};
