#pragma once

#include "plant_scene.h"

#include <QPainter>
#include <QPointF>
#include <QVector>

// 交互模式 Vulkan 失败时的 CPU 回退：同一套网格，Y 轴翻转到 Qt 坐标系。
void drawPlantMesh(QPainter &p, const QVector<PlantVertex> &verts,
                   const QPointF &center, double radius);
