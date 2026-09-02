#include "frameless_window.h"

#include "focus_timer_widget.h"
#include "genie_ghost.h"

#include <QMouseEvent>
#include <QResizeEvent>
#include <QShowEvent>
#include <QApplication>
#include <QGuiApplication>
#include <QScreen>
#include <QCursor>
#include <QSystemTrayIcon>
#include <QMenu>
#include <QAction>
#include <QStyle>
#include <QPropertyAnimation>
#include <cmath>

#ifdef Q_OS_WIN
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  ifndef NOMINMAX
#    define NOMINMAX
#  endif
#  include <windows.h>
#  include <dwmapi.h>
#  ifndef DWMWA_TRANSITIONS_FORCEDISABLED
#    define DWMWA_TRANSITIONS_FORCEDISABLED 3
#  endif
#  pragma comment(lib, "dwmapi.lib")
#endif

FramelessWindow::FramelessWindow(QWidget *parent)
    : QMainWindow(parent, Qt::FramelessWindowHint)
{
    setMinimumSize(200, 200);
    setMouseTracking(true); // 边缘命中需要常态追踪鼠标形状
    setStyleSheet(QStringLiteral("QMainWindow { background: #2B2B2B; }"));

    m_timerWidget = new FocusTimerWidget(this);
    m_timerWidget->setMouseTracking(true);
    setCentralWidget(m_timerWidget);

    setupTray();
    setupRestAlert();

#ifdef Q_OS_WIN
    // 在首次显示前关掉 DWM 最小化/隐藏过渡，否则 hide() 会把真窗口缩成非正方形，
    // FocusTimerWidget 按宽高重绘，点 × 瞬间闪出「布局错乱」。
    createWinId();
    {
        BOOL disable = TRUE;
        DwmSetWindowAttribute(reinterpret_cast<HWND>(winId()),
                              DWMWA_TRANSITIONS_FORCEDISABLED,
                              &disable, sizeof(disable));
    }
#endif

    // 初始尺寸取主屏幕面积 30% 的正方形，并居中
    QScreen *screen = QGuiApplication::primaryScreen();
    if (screen) {
        const QRect sg = screen->geometry();
        const int side = static_cast<int>(std::sqrt(sg.width() * sg.height() * 0.3));
        setGeometry(QRect(QPoint(0, 0), QSize(side, side)));
        move(sg.center() - QPoint(side / 2, side / 2));
    }
}

void FramelessWindow::setupTray()
{
    if (!QSystemTrayIcon::isSystemTrayAvailable()) {
        return;
    }

    m_trayIcon = new QSystemTrayIcon(this);
    QIcon icon = windowIcon();
    if (icon.isNull()) {
        icon = QApplication::style()->standardIcon(QStyle::SP_TitleBarNormalButton);
    }
    m_trayIcon->setIcon(icon);
    m_trayIcon->setToolTip(windowTitle().isEmpty() ? QStringLiteral("专注") : windowTitle());

    // 右键菜单「退出」才真正关闭程序
    QMenu *menu = new QMenu(this);
    QAction *quitAction = menu->addAction(QStringLiteral("退出"));
    connect(quitAction, &QAction::triggered, qApp, &QApplication::quit);
    m_trayIcon->setContextMenu(menu);

    // 左键/双击托盘图标恢复主窗口
    connect(m_trayIcon, &QSystemTrayIcon::activated, this,
            [this](QSystemTrayIcon::ActivationReason reason) {
                if (m_hiding || m_popping) {
                    return; // 动画进行中不恢复，避免几何/透明度状态竞争
                }
                if (reason == QSystemTrayIcon::Trigger
                    || reason == QSystemTrayIcon::DoubleClick) {
                    showNormal();
                    activateWindow();
                }
            });

    m_trayIcon->show();
}

void FramelessWindow::setupRestAlert()
{
    // 工作→休息切换：不弹窗。窗口处于托盘隐藏状态时恢复显示，
    // 并通过 QApplication::alert 触发任务栏闪烁提醒（窗口可见时仅闪烁）。
    connect(m_timerWidget, &FocusTimerWidget::phaseChanged, this,
            [this](int state) {
                if (state != 1 || m_hiding || m_popping) {
                    return; // 动画进行中不恢复，避免几何/透明度状态竞争
                }
                if (!isVisible()) {
                    showNormal();
                    activateWindow();
                }
                QApplication::alert(this);
            });
}

void FramelessWindow::hideToTray()
{
    if (m_hiding || m_popping) {
        return; // 弹出动画进行中点 × 会捕捉到中间帧几何，导致恢复后位置漂移
    }
    m_hiding = true;

    const QRect geo = geometry();
    const QPoint target = m_timerWidget->closeButtonCenterGlobal();
    QPixmap shot = grab();

    auto *ghost = new GenieGhost(shot);
    connect(ghost, &QWidget::destroyed, this, [this] { m_hiding = false; });

    // 1) 快照以完整尺寸先盖住真窗口并刷新合成器
    ghost->appearAt(geo);
    // 2) 再藏真窗口：冻结绘制、禁止 resize 回正；不改 opacity（首次 setWindowOpacity
    //    会给 HWND 加 WS_EX_LAYERED，窗口重建瞬间也会闪错乱布局）
    setUpdatesEnabled(false);
    hide();
    setUpdatesEnabled(true); // 下次从托盘恢复时需要绘制；几何在 hide 期间未改
    // 3) 真窗口已不可见后，才开始吸入，避免与 DWM 隐藏动画叠在一起
    ghost->suckInto(target);
}

void FramelessWindow::showEvent(QShowEvent *event)
{
    QMainWindow::showEvent(event);
    if (m_hiding || m_popping) {
        return; // 关闭动画收尾 / 上一次弹出未结束，不重复触发
    }
    m_popping = true;
    // 弹出：自中心 96% 放大 + 淡入
    const QRect to = geometry();
    const QPoint c = to.center();
    const int side = static_cast<int>(to.width() * 0.96);
    const QRect from(c.x() - side / 2, c.y() - side / 2, side, side);

    auto *geom = new QPropertyAnimation(this, "geometry", this);
    geom->setDuration(160);
    geom->setStartValue(from);
    geom->setEndValue(to);
    geom->setEasingCurve(QEasingCurve::OutCubic);
    geom->start(QAbstractAnimation::DeleteWhenStopped);

    auto *fade = new QPropertyAnimation(this, "windowOpacity", this);
    fade->setDuration(160);
    fade->setStartValue(0.0);
    fade->setEndValue(1.0);
    fade->setEasingCurve(QEasingCurve::OutCubic);
    fade->start(QAbstractAnimation::DeleteWhenStopped);

    connect(geom, &QPropertyAnimation::finished, this, [this] {
        m_popping = false;
    });
}

int FramelessWindow::edgeHit(const QPoint &pos) const
{
    int edges = EdgeNone;
    if (pos.x() < kEdgeMargin) {
        edges |= EdgeLeft;
    } else if (pos.x() >= width() - kEdgeMargin) {
        edges |= EdgeRight;
    }
    if (pos.y() < kEdgeMargin) {
        edges |= EdgeTop;
    } else if (pos.y() >= height() - kEdgeMargin) {
        edges |= EdgeBottom;
    }
    return edges;
}

void FramelessWindow::updateCursor(const QPoint &pos)
{
    const int edges = edgeHit(pos);
    Qt::CursorShape shape = Qt::ArrowCursor;
    switch (edges) {
    case EdgeLeft:
    case EdgeRight:
        shape = Qt::SizeHorCursor;
        break;
    case EdgeTop:
    case EdgeBottom:
        shape = Qt::SizeVerCursor;
        break;
    case EdgeLeft | EdgeTop:
    case EdgeRight | EdgeBottom:
        shape = Qt::SizeFDiagCursor;
        break;
    case EdgeRight | EdgeTop:
    case EdgeLeft | EdgeBottom:
        shape = Qt::SizeBDiagCursor;
        break;
    default:
        break;
    }
    setCursor(shape);
}

void FramelessWindow::mousePressEvent(QMouseEvent *event)
{
    if (m_hiding) {
        return; // 关闭动画进行中，屏蔽一切交互
    }
    if (event->button() != Qt::LeftButton) {
        QMainWindow::mousePressEvent(event);
        return;
    }

    const QPoint pos = event->pos();

    // 关闭按钮优先于移动/缩放判定：吸入式动画后隐藏到系统托盘
    if (m_timerWidget->closeButtonHit(pos)) {
        hideToTray();
        return;
    }
    // 播放按钮 / 「...」按钮 / 圆心热区交给控件自己处理，不触发窗口拖拽
    if (m_timerWidget->playButtonHit(pos) || m_timerWidget->dotsButtonHit(pos)
        || m_timerWidget->centerHit(pos)) {
        QMainWindow::mousePressEvent(event);
        return;
    }

    const int edges = edgeHit(pos);
    if (edges != EdgeNone) {
        m_resizing = edges;
        m_startGeometry = geometry();
        m_startGlobal = event->globalPosition().toPoint();
    } else {
        m_moving = true;
        m_moveOffset = event->globalPosition().toPoint() - frameGeometry().topLeft();
    }
}

void FramelessWindow::mouseMoveEvent(QMouseEvent *event)
{
    if (m_resizing != EdgeNone) {
        applyResize(event->globalPosition().toPoint());
        return;
    }
    if (m_moving) {
        move(event->globalPosition().toPoint() - m_moveOffset);
        return;
    }
    updateCursor(event->pos());
    QMainWindow::mouseMoveEvent(event);
}

void FramelessWindow::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        m_moving = false;
        m_resizing = EdgeNone;
        updateCursor(event->pos());
    }
    QMainWindow::mouseReleaseEvent(event);
}

void FramelessWindow::applyResize(const QPoint &globalPos)
{
    const QPoint delta = globalPos - m_startGlobal;
    QRect geo = m_startGeometry;

    if (m_resizing & EdgeLeft) {
        geo.setLeft(m_startGeometry.left() + delta.x());
    }
    if (m_resizing & EdgeRight) {
        geo.setRight(m_startGeometry.right() + delta.x());
    }
    if (m_resizing & EdgeTop) {
        geo.setTop(m_startGeometry.top() + delta.y());
    }
    if (m_resizing & EdgeBottom) {
        geo.setBottom(m_startGeometry.bottom() + delta.y());
    }

    // 以短边为准回正成正方形；拖左边/上边时保持对侧边不动
    const int side = qMax(qMin(geo.width(), geo.height()), minimumWidth());
    if (m_resizing & EdgeLeft) {
        geo.setLeft(geo.right() - side + 1);
    } else {
        geo.setRight(geo.left() + side - 1);
    }
    if (m_resizing & EdgeTop) {
        geo.setTop(geo.bottom() - side + 1);
    } else {
        geo.setBottom(geo.top() + side - 1);
    }

    setGeometry(geo);
}

void FramelessWindow::resizeEvent(QResizeEvent *event)
{
    // 关闭过程中若 DWM 仍送来非正方形 WM_SIZE，绝不能按短边回正，
    // 否则表盘/按钮会按错误宽高重绘，点 × 瞬间闪错乱布局。
    if (m_hiding) {
        QMainWindow::resizeEvent(event);
        return;
    }
    // 程序性（非拖拽）resize 也保持正方形：以短边回正。
    // resize 内再 resize 会递归触发本函数，用宽高差判断收敛。
    const int w = event->size().width();
    const int h = event->size().height();
    if (m_resizing == EdgeNone && w != h) {
        const int side = qMin(w, h);
        resize(side, side);
    }
    QMainWindow::resizeEvent(event);
}

bool FramelessWindow::event(QEvent *event)
{
    if (event->type() == QEvent::HoverLeave) {
        unsetCursor();
    }
    return QMainWindow::event(event);
}
