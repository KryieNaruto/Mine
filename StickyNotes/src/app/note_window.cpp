#include "app/note_window.h"

#include <QPropertyAnimation>
#include <QVBoxLayout>
#include "app/note_widget.h"

NoteWindow::NoteWindow(NoteStore& store, QUuid noteId, QWidget* parent)
    : QWidget(parent), store_(store), id_(noteId) {
  setWindowFlags(Qt::Window | Qt::FramelessWindowHint);
  setAttribute(Qt::WA_DeleteOnClose, false);

  auto* lay = new QVBoxLayout(this);
  lay->setContentsMargins(0, 0, 0, 0);
  lay->setSpacing(0);
  widget_ = new NoteWidget(store_, id_, this);
  lay->addWidget(widget_);

  if (Note* n = store_.find(id_))
    setGeometry(QRect(n->pos.toPoint(), n->size.toSize()));

  connect(&store_, &NoteStore::changed, this, &NoteWindow::onStoreChanged);
  connect(widget_, &NoteWidget::titleEdited, this,
          [this](const QString& t) { store_.setTitle(id_, t); });
  connect(widget_, &NoteWidget::colorPicked, this,
          [this](const QColor& c) { store_.setTitleColor(id_, c); });
  connect(widget_, &NoteWidget::pinToggled, this, &NoteWindow::setPinned);
  connect(widget_, &NoteWidget::taskToggled, this,
          [this](int i, bool done) { store_.setTaskDone(id_, i, done); });
  connect(widget_, &NoteWidget::dragStart, this,
          [this](const QPoint& gp) { lastDragGlobal_ = gp; emit dragStarted(); });
  connect(widget_, &NoteWidget::dragMove, this, [this](const QPoint& gp) {
    QPoint delta = gp - lastDragGlobal_;
    move(pos() + delta);
    lastDragGlobal_ = gp;
  });
  connect(widget_, &NoteWidget::dragEnd, this, [this]() {
    emit dragEnded();
  });
}

void NoteWindow::setPinned(bool on) {
  if (pinned_ == on)
    return;
  pinned_ = on;
  setWindowFlag(Qt::WindowStaysOnTopHint, on);
  show();
}

void NoteWindow::setDocked(const DockState& d) {
  dockState_ = d;
  QRect target;
  if (d.docked) {
    shownRect_ = geometry();
    target = d.hiddenRect;
  } else {
    target = shownRect_.isValid() ? shownRect_ : geometry();
  }
  if (anim_) {
    anim_->stop();
    anim_->deleteLater();
    anim_ = nullptr;
  }
  anim_ = new QPropertyAnimation(this, "pos");
  anim_->setDuration(150);
  anim_->setStartValue(pos());
  anim_->setEndValue(target.topLeft());
  anim_->setEasingCurve(QEasingCurve::InOutQuad);
  anim_->start();
}

void NoteWindow::onStoreChanged() {
  widget_->refresh();
}
