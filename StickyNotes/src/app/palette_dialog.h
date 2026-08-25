#pragma once
#include <QDialog>

class QColor;

// 标题色选择对话框：预设色板，点选即返回
class PaletteDialog : public QDialog {
  Q_OBJECT
public:
  explicit PaletteDialog(QWidget* parent = nullptr);
  QColor selectedColor() const { return selected_; }

  // 便捷入口：弹窗选择颜色，取消返回无效 QColor
  static QColor getColor(QWidget* parent = nullptr);
private:
  QColor selected_;
};
