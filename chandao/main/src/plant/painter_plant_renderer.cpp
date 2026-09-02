#include "painter_plant_renderer.h"

#include <QPainterPath>

void drawPlantMesh(QPainter &p, const QVector<PlantVertex> &verts,
                   const QPointF &center, double radius)
{
    if (verts.size() < 3 || radius <= 0.0) {
        return;
    }

    p.save();
    QPainterPath clip;
    clip.addEllipse(center, radius, radius);
    p.setClipPath(clip);
    p.setPen(Qt::NoPen);
    p.setRenderHint(QPainter::Antialiasing, true);

    for (int i = 0; i + 2 < verts.size(); i += 3) {
        const PlantVertex &a = verts[i];
        const PlantVertex &b = verts[i + 1];
        const PlantVertex &c = verts[i + 2];
        QColor col;
        col.setRgbF(a.r, a.g, a.b, a.a);
        p.setBrush(col);
        const QPointF pa(center.x() + a.x * radius, center.y() - a.y * radius);
        const QPointF pb(center.x() + b.x * radius, center.y() - b.y * radius);
        const QPointF pc(center.x() + c.x * radius, center.y() - c.y * radius);
        p.drawPolygon(QPolygonF({ pa, pb, pc }));
    }
    p.restore();
}
