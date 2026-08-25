#include "app/palette_dialog.h"

#include <QColor>
#include <QGridLayout>
#include <QPushButton>

static const QColor kPresets[] = {
  QColor("#ffb74d"), QColor("#f06292"), QColor("#ba68c8"), QColor("#9575cd"),
  QColor("#4fc3f7"), QColor("#4db6ac"), QColor("#81c784"), QColor("#dce775"),
  QColor("#fff176"), QColor("#ff8a65"), QColor("#a1887f"), QColor("#90a4ae"),
};

PaletteDialog::PaletteDialog(QWidget* parent) : QDialog(parent) {
  setWindowTitle(QStringLiteral("选择标题色"));
  auto* grid = new QGridLayout(this);
  grid->setSpacing(6);
  int n = int(sizeof(kPresets) / sizeof(kPresets[0]));
  for (int i = 0; i < n; ++i) {
    auto* b = new QPushButton(this);
    b->setFixedSize(36, 36);
    b->setStyleSheet(QStringLiteral("background-color: %1; border: 1px solid #999;")
                       .arg(kPresets[i].name()));
    connect(b, &QPushButton::clicked, this, [this, i]() {
      selected_ = kPresets[i];
      accept();
    });
    grid->addWidget(b, i / 6, i % 6);
  }
}

QColor PaletteDialog::getColor(QWidget* parent) {
  PaletteDialog dlg(parent);
  return dlg.exec() == QDialog::Accepted ? dlg.selectedColor() : QColor();
}
