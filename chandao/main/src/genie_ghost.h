#pragma once

#include <QWidget>
#include <QPixmap>
#include <QPainter>
#include <QPropertyAnimation>
#include <QEasingCurve>
#include <QWindow>
#include <QCoreApplication>
#include <QEventLoop>

#ifdef Q_OS_WIN
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <dwmapi.h>
#  pragma comment(lib, "dwmapi.lib")
#endif

// 吸入式关闭的替身窗口：显示窗口快照并做收缩淡出动画。
// 真窗口几何与子控件布局在动画期间必须保持不变。
class GenieGhost : public QWidget
{
public:
    explicit GenieGhost(const QPixmap &snapshot)
        : QWidget(nullptr,
                  Qt::FramelessWindowHint | Qt::WindowStaysOnTopHint | Qt::Tool
                      | Qt::WindowDoesNotAcceptFocus)
        , m_snapshot(snapshot)
    {
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_DeleteOnClose);
        setAttribute(Qt::WA_ShowWithoutActivating);
        setAttribute(Qt::WA_NoSystemBackground);
    }

    // 先以完整尺寸上屏并刷新合成器，再开始吸入。不可与 suckInto 同一帧完成。
    void appearAt(const QRect &globalGeo)
    {
        m_startGeo = globalGeo;
        setGeometry(globalGeo);
        createWinId();
        if (QWindow *wh = windowHandle()) {
            wh->setPosition(globalGeo.topLeft());
            wh->resize(globalGeo.size());
        }
        show();
        raise();
        repaint();
        QCoreApplication::processEvents(QEventLoop::ExcludeUserInputEvents);
#ifdef Q_OS_WIN
        if (HWND hwnd = reinterpret_cast<HWND>(winId())) {
            UpdateWindow(hwnd);
        }
        DwmFlush();
#endif
    }

    // 向目标点收缩成小方块并加速淡出。调用前必须已经 appearAt。
    void suckInto(const QPoint &targetGlobal)
    {
        const QRect from = m_startGeo.isValid() ? m_startGeo : geometry();
        setGeometry(from);
        const QRect to(targetGlobal.x() - 10, targetGlobal.y() - 10, 20, 20);

        auto *geom = new QPropertyAnimation(this, "geometry", this);
        geom->setDuration(220);
        geom->setStartValue(from);
        geom->setEndValue(to);
        geom->setEasingCurve(QEasingCurve::InCubic);
        geom->start(QAbstractAnimation::DeleteWhenStopped);

        auto *fade = new QPropertyAnimation(this, "windowOpacity", this);
        fade->setDuration(200);
        fade->setStartValue(1.0);
        fade->setEndValue(0.0);
        fade->setEasingCurve(QEasingCurve::InCubic);
        fade->start(QAbstractAnimation::DeleteWhenStopped);

        connect(geom, &QPropertyAnimation::finished, this, [this] { close(); });
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::SmoothPixmapTransform);
        // 使用两参数重载，让 Qt 按 pixmap.devicePixelRatio 铺满目标矩形。
        // 若把 deviceIndependentSize 当作源矩形传入三参数重载，DPR=2 时会
        // 把源区当成物理像素，只画出左上 1/4，视觉上就是「布局全错」。
        p.drawPixmap(rect(), m_snapshot);
    }

private:
    QPixmap m_snapshot;
    QRect m_startGeo;
};
