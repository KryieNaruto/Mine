#include "app/note_widget.h"

#include <QCheckBox>
#include <QEnterEvent>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMouseEvent>
#include <QTextEdit>
#include <QToolButton>
#include <QVBoxLayout>
#include "app/palette_dialog.h"
#include "core/palette.h"

NoteWidget::NoteWidget(NoteStore& store, QUuid id, QWidget* parent)
    : QWidget(parent), store_(store), id_(id) {
  auto* root = new QVBoxLayout(this);
  root->setContentsMargins(0, 0, 0, 0);
  root->setSpacing(0);

  // 标题栏（自绘底色，扁平无边框）
  titleBar_ = new QWidget(this);
  titleBar_->setFixedHeight(32);
  titleBar_->installEventFilter(this);
  auto* titleLay = new QHBoxLayout(titleBar_);
  titleLay->setContentsMargins(6, 0, 4, 0);
  titleLay->setSpacing(2);

  titleEdit_ = new QLineEdit(titleBar_);
  titleEdit_->setObjectName(QStringLiteral("titleEdit"));
  titleEdit_->setFrame(false);
  titleEdit_->setPlaceholderText(QStringLiteral("标题"));
  connect(titleEdit_, &QLineEdit::textEdited, this, &NoteWidget::titleEdited);
  titleLay->addWidget(titleEdit_, 1);

  pinBtn_ = new QToolButton(titleBar_);
  pinBtn_->setText(QStringLiteral("置"));
  pinBtn_->setCheckable(true);
  pinBtn_->setToolTip(QStringLiteral("固定置顶"));
  connect(pinBtn_, &QToolButton::toggled, this, &NoteWidget::pinToggled);
  titleLay->addWidget(pinBtn_);

  colorBtn_ = new QToolButton(titleBar_);
  colorBtn_->setText(QStringLiteral("色"));
  colorBtn_->setToolTip(QStringLiteral("标题颜色"));
  connect(colorBtn_, &QToolButton::clicked, this, [this]() {
    QColor c = PaletteDialog::getColor(this);
    if (c.isValid())
      emit colorPicked(c);
  });
  titleLay->addWidget(colorBtn_);

  splitBtn_ = new QToolButton(titleBar_);
  splitBtn_->setText(QStringLiteral("拆"));
  splitBtn_->setToolTip(QStringLiteral("从组中拆分"));
  connect(splitBtn_, &QToolButton::clicked, this, &NoteWidget::splitRequested);
  titleLay->addWidget(splitBtn_);

  addBtn_ = new QToolButton(titleBar_);
  addBtn_->setText(QStringLiteral("+"));
  addBtn_->setToolTip(QStringLiteral("新建便签"));
  connect(addBtn_, &QToolButton::clicked, this, &NoteWidget::addRequested);
  titleLay->addWidget(addBtn_);

  deleteBtn_ = new QToolButton(titleBar_);
  deleteBtn_->setText(QStringLiteral("✕"));
  deleteBtn_->setToolTip(QStringLiteral("删除便签"));
  connect(deleteBtn_, &QToolButton::clicked, this, &NoteWidget::deleteRequested);
  titleLay->addWidget(deleteBtn_);

  root->addWidget(titleBar_);

  // 正文
  bodyArea_ = new QTextEdit(this);
  bodyArea_->setObjectName(QStringLiteral("bodyArea"));
  bodyArea_->setFrameShape(QFrame::NoFrame);
  connect(bodyArea_, &QTextEdit::textChanged, this, [this]() {
    store_.setBodyText(id_, bodyArea_->toPlainText());
  });
  root->addWidget(bodyArea_, 1);

  // 任务列表容器
  tasksWidget_ = new QWidget(this);
  tasksLayout_ = new QVBoxLayout(tasksWidget_);
  tasksLayout_->setContentsMargins(6, 2, 6, 4);
  tasksLayout_->setSpacing(2);
  root->addWidget(tasksWidget_);

  refresh();
}

void NoteWidget::refresh() {
  Note* note = store_.find(id_);
  if (!note)
    return;
  // 标题
  {
    const QSignalBlocker b(titleEdit_);
    if (titleEdit_->text() != note->title)
      titleEdit_->setText(note->title);
  }
  // 正文：textChanged 已写回 store，此处仅在外部变更时同步
  {
    const QSignalBlocker b(bodyArea_);
    if (bodyArea_->toPlainText() != note->bodyText)
      bodyArea_->setPlainText(note->bodyText);
  }
  pinBtn_->setChecked(note->pinned);
  rebuildTasks(note);
  applyColors();
}

QColor NoteWidget::currentBodyColor() const {
  Note* note = store_.find(id_);
  if (!note)
    return QColor(Qt::white);
  return hovered_ ? bodyColorHover(note->titleColor) : fadedBodyColor(note->titleColor);
}

void NoteWidget::setDocked(bool docked) {
  docked_ = docked;
  update();
}

void NoteWidget::setHoveredForTest(bool h) {
  hovered_ = h;
  refresh();
}

void NoteWidget::enterEvent(QEnterEvent* e) {
  hovered_ = true;
  refresh();
  QWidget::enterEvent(e);
}

void NoteWidget::leaveEvent(QEvent* e) {
  hovered_ = false;
  refresh();
  QWidget::leaveEvent(e);
}

bool NoteWidget::eventFilter(QObject* obj, QEvent* ev) {
  if (obj == titleBar_) {
    if (ev->type() == QEvent::MouseButtonPress) {
      auto* me = static_cast<QMouseEvent*>(ev);
      if (me->button() == Qt::LeftButton) {
        emit dragStart(me->globalPosition().toPoint());
        return true;
      }
    } else if (ev->type() == QEvent::MouseMove) {
      auto* me = static_cast<QMouseEvent*>(ev);
      if (me->buttons() & Qt::LeftButton) {
        emit dragMove(me->globalPosition().toPoint());
        return true;
      }
    } else if (ev->type() == QEvent::MouseButtonRelease) {
      emit dragEnd();
      return true;
    }
  }
  return QWidget::eventFilter(obj, ev);
}

void NoteWidget::rebuildTasks(const Note* note) {
  // 任务只在末尾追加（无删除/重排），行号即索引，逐行 reconcile 避免销毁信号发送者
  while (taskRows_.size() > note->tasks.size()) {
    QWidget* row = taskRows_.takeLast();
    tasksLayout_->removeWidget(row);
    row->deleteLater();
  }
  for (int i = 0; i < note->tasks.size(); ++i) {
    QWidget* row = nullptr;
    if (i < taskRows_.size()) {
      row = taskRows_[i];
    } else {
      row = new QWidget(tasksWidget_);
      auto* lay = new QHBoxLayout(row);
      lay->setContentsMargins(0, 0, 0, 0);
      lay->setSpacing(6);
      auto* cb = new QCheckBox(row);
      cb->setObjectName(QStringLiteral("taskCheck%1").arg(i));
      auto* lbl = new QLabel(row);
      lbl->setObjectName(QStringLiteral("taskLabel%1").arg(i));
      lay->addWidget(cb);
      lay->addWidget(lbl, 1);
      const int idx = i;
      connect(cb, &QCheckBox::toggled, this, [this, idx](bool done) {
        emit taskToggled(idx, done);
      });
      tasksLayout_->addWidget(row);
      taskRows_.append(row);
    }
    QCheckBox* cb = row->findChild<QCheckBox*>(QStringLiteral("taskCheck%1").arg(i));
    QLabel* lbl = row->findChild<QLabel*>(QStringLiteral("taskLabel%1").arg(i));
    const TaskItem& t = note->tasks[i];
    {
      const QSignalBlocker b(cb);
      cb->setChecked(t.done);
    }
    lbl->setText(t.text);
    QFont f = lbl->font();
    f.setStrikeOut(t.done);
    lbl->setFont(f);
    lbl->setEnabled(!t.done);
  }
}

void NoteWidget::applyColors() {
  Note* note = store_.find(id_);
  if (!note)
    return;
  QColor titleCol = titleBarColor(note->titleColor);
  QColor textCol = titleBarTextColor(titleCol);
  bodyColor_ = currentBodyColor();

  titleBar_->setStyleSheet(QStringLiteral("background-color: %1;").arg(titleCol.name()));
  const QString btnStyle =
      QStringLiteral("QToolButton { background: transparent; border: none; color: %1; }"
                     "QToolButton:hover { background: rgba(0,0,0,40); }")
          .arg(textCol.name());
  for (QToolButton* b : {pinBtn_, colorBtn_, splitBtn_, deleteBtn_, addBtn_}) {
    b->setStyleSheet(btnStyle);
  }
  titleEdit_->setStyleSheet(
      QStringLiteral("QLineEdit { background: transparent; border: none; color: %1;"
                     "font-weight: bold; }")
          .arg(textCol.name()));
  bodyArea_->setStyleSheet(
      QStringLiteral("QTextEdit { background-color: rgba(%1,%2,%3,%4); border: none; }")
          .arg(bodyColor_.red())
          .arg(bodyColor_.green())
          .arg(bodyColor_.blue())
          .arg(bodyColor_.alpha()));
}
