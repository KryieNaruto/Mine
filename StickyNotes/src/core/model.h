#pragma once
#include <QColor>
#include <QPointF>
#include <QSizeF>
#include <QString>
#include <QVector>
#include <QUuid>

struct TaskItem {
  QString text;
  bool done = false;
};

struct Note {
  QUuid id;
  QString title;
  QColor titleColor = QColor("#ffb74d");
  QString bodyText;
  bool pinned = false;
  QVector<TaskItem> tasks;
  QPointF pos = QPointF(100, 100);
  QSizeF size = QSizeF(260, 320);
};
