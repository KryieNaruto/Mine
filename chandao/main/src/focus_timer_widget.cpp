#include "focus_timer_widget.h"
#include "settings_dialog.h"
#include "plant/seed.h"
#include "plant/plant_scene.h"
#include "plant/painter_plant_renderer.h"
#ifdef HAS_VULKAN
#include "plant/vulkan_plant_renderer.h"
#endif

#include <QPainter>
#include <QPainterPath>
#include <QMouseEvent>
#include <QPropertyAnimation>
#include <QSettings>
#include <QStringList>
#include <QVector>
#include <QtMath>
#include <cmath>
#include <functional>
#include <QDebug>

// 各动画在 60fps 下的单帧步长：图标切换约 150ms，悬停/按压取相近量级的快响应
static constexpr double kIconStep  = 0.016 / 0.150;
static constexpr double kHoverStep = 0.016 / 0.120;
static constexpr double kPressStep = 0.016 / 0.060;

// 播放按钮与「...」按钮统一半径比（直径 ≈ 窗口边长 8%）
static constexpr double kButtonRadiusRatio = 0.04;
// 两按钮作为整体水平居中：各自中心距窗口中线的偏移比（间距与旧版一致）
static constexpr double kButtonHalfGap = 0.055;

namespace {

// 「...」圆角浮层菜单：QMenu 样式表无法做圆角与悬停过渡动画，故自绘。
// 高亮满宽无内边距（避免蓝色外一圈灰），悬停颜色按 60fps 插值渐变。
class DotsMenuPopup : public QWidget
{
public:
    explicit DotsMenuPopup(const QStringList &items, QWidget *parent = nullptr, int width = 140)
        : QWidget(parent, Qt::Popup | Qt::FramelessWindowHint | Qt::NoDropShadowWindowHint)
        , m_items(items)
        , m_hoverT(items.size(), 0.0)
        , m_width(width)
    {
        // WA_TranslucentBackground 让圆角之外真正透明，否则四角露黑/灰
        setAttribute(Qt::WA_TranslucentBackground);
        setAttribute(Qt::WA_DeleteOnClose);
        setWindowOpacity(0.0); // 弹出动画从全透明开始，避免首帧闪现
        setMouseTracking(true);
        setFixedSize(m_width, kVPad * 2 + kItemH * m_items.size());
        m_timer = new QTimer(this);
        connect(m_timer, &QTimer::timeout, this, [this] { tick(); });
        m_timer->start(16); // 与主控件同为约 60fps 插值
    }

    // 点击某项后回调（在 close() 之后调用，回调内不得再访问本对象）
    std::function<void(int)> onTriggered;

protected:
    void showEvent(QShowEvent *event) override
    {
        QWidget::showEvent(event);
        // 丝滑弹出：淡入 + 从上方 8px 处滑入
        auto *fade = new QPropertyAnimation(this, "windowOpacity", this);
        fade->setDuration(160);
        fade->setStartValue(0.0);
        fade->setEndValue(1.0);
        fade->setEasingCurve(QEasingCurve::OutCubic);
        fade->start(QAbstractAnimation::DeleteWhenStopped);

        auto *slide = new QPropertyAnimation(this, "pos", this);
        slide->setDuration(160);
        slide->setStartValue(pos() - QPoint(0, 8));
        slide->setEndValue(pos());
        slide->setEasingCurve(QEasingCurve::OutCubic);
        slide->start(QAbstractAnimation::DeleteWhenStopped);
    }

    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);

        // 圆角背景
        QPainterPath clip;
        clip.addRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 8.0, 8.0);
        p.setClipPath(clip);
        p.fillPath(clip, QColor(0x2B, 0x2B, 0x2B));

        // 菜单项：高亮满宽（由 clip 裁出圆角），蓝色透明度随悬停渐变
        QFont font = p.font();
        font.setPixelSize(13);
        p.setFont(font);
        for (int i = 0; i < m_items.size(); ++i) {
            const QRectF itemRc(0.0, kVPad + i * kItemH, m_width, kItemH);
            const double t = m_hoverT[i];
            if (t > 0.0) {
                QColor hl(0x55, 0xB2, 0xE8);
                hl.setAlphaF(t);
                p.setPen(Qt::NoPen);
                p.setBrush(hl);
                p.drawRect(itemRc);
            }
            // 文字颜色随悬停在 #E8E8E8 与纯白间过渡
            const int v = static_cast<int>(0xE8 + (0xFF - 0xE8) * t);
            p.setPen(QColor(v, v, v));
            p.drawText(itemRc.adjusted(14, 0, 0, 0),
                       Qt::AlignVCenter | Qt::AlignLeft, m_items[i]);
        }

        // 1px 描边（在 clip 之内绘制，避免外沿灰边）
        p.setClipping(false);
        p.setPen(QPen(QColor(0x3D, 0x3D, 0x3D), 1));
        p.setBrush(Qt::NoBrush);
        p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 8.0, 8.0);
    }

    void mouseMoveEvent(QMouseEvent *event) override { setHovered(indexAt(event->pos())); }
    void leaveEvent(QEvent *) override { setHovered(-1); }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        const int idx = indexAt(event->pos());
        auto cb = onTriggered;
        close(); // WA_DeleteOnClose 为延迟删除，本帧内对象仍有效
        if (idx >= 0 && cb) {
            cb(idx); // 回调可能开启嵌套事件循环，此后不得再访问本对象成员
        }
    }

private:
    int indexAt(const QPoint &pos) const
    {
        const int rel = pos.y() - kVPad;
        if (rel < 0) {
            return -1;
        }
        const int idx = rel / kItemH;
        return (idx >= 0 && idx < m_items.size()) ? idx : -1;
    }

    void setHovered(int idx)
    {
        if (idx != m_hovered) {
            m_hovered = idx;
        }
    }

    void tick()
    {
        for (int i = 0; i < m_hoverT.size(); ++i) {
            const double target = (i == m_hovered) ? 1.0 : 0.0;
            double &v = m_hoverT[i];
            v = (v < target) ? qMin(v + kHoverStep, target) : qMax(v - kHoverStep, target);
        }
        update();
    }

    static constexpr int kItemH = 30;
    static constexpr int kVPad = 6;

    QStringList m_items;
    QVector<double> m_hoverT;
    int m_hovered = -1;
    int m_width = 140;
    QTimer *m_timer;
};

} // namespace

#ifdef HAS_VULKAN
static VulkanPlantRenderer &vkPlant()
{
    static VulkanPlantRenderer renderer;
    return renderer;
}
#endif

FocusTimerWidget::FocusTimerWidget(QWidget *parent)
    : QWidget(parent)
    , m_activeColor(0x55, 0xB2, 0xE8)
    , m_inactiveColor(0x3D, 0x3D, 0x3D)
    , m_bgColor(0x2B, 0x2B, 0x2B)
    , m_isRunning(true)
    , m_state(0)
    , m_elapsed(0.0)
    , m_workDuration(10.0)
    , m_restDuration(10.0)
{
    setMouseTracking(true); // 悬停提亮需要不按下也能收到移动事件
    m_timer = new QTimer(this);
    connect(m_timer, &QTimer::timeout, this, &FocusTimerWidget::updateTimer);
    m_timer->start(16); // 约 60fps
    connect(this, &FocusTimerWidget::dotsClicked, this, &FocusTimerWidget::showDotsMenu);
}

void FocusTimerWidget::setTime(double seconds)
{
    m_timer->stop();
    m_timerStopped = true;
    m_isRunning = true;

    const double cycle = m_workDuration + m_restDuration;
    const double rem = std::fmod(seconds, cycle);
    if (rem < m_workDuration) {
        m_state = 0;
        m_elapsed = rem;
    } else {
        m_state = 1;
        m_elapsed = rem - m_workDuration;
    }
    update();
}

bool FocusTimerWidget::closeButtonHit(const QPoint &pos) const
{
    return closeButtonRect().contains(pos);
}

bool FocusTimerWidget::playButtonHit(const QPoint &pos) const
{
    return buttonRect().contains(pos);
}

bool FocusTimerWidget::dotsButtonHit(const QPoint &pos) const
{
    return dotsButtonRect().contains(pos);
}

QPoint FocusTimerWidget::closeButtonCenterGlobal() const
{
    return mapToGlobal(closeButtonRect().center());
}

void FocusTimerWidget::setDurations(double workSec, double restSec)
{
    if (workSec <= 0.0 || restSec <= 0.0) {
        return;
    }
    m_workDuration = workSec;
    m_restDuration = restSec;

    const double duration = (m_state == 0) ? m_workDuration : m_restDuration;
    if (m_elapsed >= duration) {
        // 新时长已小于等于已用时长：当前阶段立即结束，进入下一阶段
        m_state = 1 - m_state;
        m_elapsed = 0.0;
        if (!m_timerStopped) {
            emit phaseChanged(m_state);
        }
    }
    // 否则续跑：剩余 = 新时长 - 已用（m_elapsed 不动即天然满足）
    update();
}

void FocusTimerWidget::setCenterMode(CenterMode mode)
{
    if (m_centerMode != mode) {
        m_centerMode = mode;
        persistAppearance();
        update();
    }
}

void FocusTimerWidget::setSeedId(const QString &id)
{
    const SeedDescriptor *seed = SeedRegistry::find(id);
    const QString next = seed ? seed->id : QStringLiteral("small_tree");
    if (m_seedId != next) {
        m_seedId = next;
        persistAppearance();
        update();
    }
}

void FocusTimerWidget::setPlantVulkanRequired(bool required)
{
    m_plantVulkanRequired = required;
}

void FocusTimerWidget::persistAppearance() const
{
    if (m_timerStopped) {
        return;
    }
    QSettings settings(QStringLiteral("Chandao"), QStringLiteral("FocusTimer"));
    settings.setValue(QStringLiteral("centerMode"),
                      m_centerMode == CenterMode::Plant
                          ? QStringLiteral("plant")
                          : QStringLiteral("time"));
    settings.setValue(QStringLiteral("seedId"), m_seedId);
}

double FocusTimerWidget::workProgress() const
{
    if (m_state != 0) {
        return 1.0;
    }
    if (m_workDuration <= 0.0) {
        return 0.0;
    }
    return qBound(0.0, m_elapsed / m_workDuration, 1.0);
}

bool FocusTimerWidget::centerHit(const QPoint &pos) const
{
    if (playButtonHit(pos) || dotsButtonHit(pos) || closeButtonHit(pos)) {
        return false;
    }
    const double base = qMin(width(), height());
    const TickStyle ts;
    const QPointF c(width() * 0.5, height() * 0.4);
    const double radius = base * (ts.ringRadius - ts.heightRatio * 0.5);
    const double dx = pos.x() - c.x();
    const double dy = pos.y() - c.y();
    return dx * dx + dy * dy <= radius * radius;
}

double FocusTimerWidget::remainingSeconds() const
{
    const double duration = (m_state == 0) ? m_workDuration : m_restDuration;
    return qMax(0.0, duration - m_elapsed);
}

QSize FocusTimerWidget::sizeHint() const
{
    return QSize(100, 100);
}

void FocusTimerWidget::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(rect(), m_bgColor);

    drawRing(p);
    drawCenterContent(p);
    drawButton(p);
    drawDotsButton(p);
    drawCloseButton(p);
}

void FocusTimerWidget::mousePressEvent(QMouseEvent *event)
{
    if (buttonRect().contains(event->pos())) {
        m_buttonPressed = true;
        update();
    } else if (dotsButtonRect().contains(event->pos())) {
        m_dotsPressed = true;
        update();
    } else if (centerHit(event->pos())) {
        m_centerPressed = true;
    } else {
        // 未命中按钮（含右上角 ×）：交还父窗口处理拖拽/关闭/边缘缩放
        event->ignore();
    }
}

void FocusTimerWidget::mouseReleaseEvent(QMouseEvent *event)
{
    if (m_buttonPressed) {
        m_buttonPressed = false;
        if (buttonRect().contains(event->pos())) {
            m_isRunning = !m_isRunning;
        }
        update();
    } else if (m_dotsPressed) {
        m_dotsPressed = false;
        if (dotsButtonRect().contains(event->pos())) {
            emit dotsClicked();
        }
        update();
    } else if (m_centerPressed) {
        m_centerPressed = false;
        if (centerHit(event->pos())) {
            setCenterMode(m_centerMode == CenterMode::Plant
                              ? CenterMode::TimeText
                              : CenterMode::Plant);
        }
        update();
    } else {
        event->ignore();
    }
}

void FocusTimerWidget::mouseMoveEvent(QMouseEvent *event)
{
    const bool overButton = buttonRect().contains(event->pos());
    const bool overClose = closeButtonRect().contains(event->pos());
    const bool overDots = dotsButtonRect().contains(event->pos());
    if (overButton != m_buttonHovered || overClose != m_closeHovered
        || overDots != m_dotsHovered) {
        m_buttonHovered = overButton;
        m_closeHovered = overClose;
        m_dotsHovered = overDots;
        update();
    }
    // 按下内圆后必须由子控件收 move/release，不要 ignore 把拖拽抢回去
    if (m_centerPressed || m_buttonPressed || m_dotsPressed) {
        return;
    }
    if (!overButton && !overClose && !overDots) {
        // 交还父窗口：边缘缩放光标依赖父级收到移动事件
        event->ignore();
    }
}

void FocusTimerWidget::leaveEvent(QEvent *)
{
    if (m_buttonHovered || m_closeHovered || m_dotsHovered) {
        m_buttonHovered = false;
        m_closeHovered = false;
        m_dotsHovered = false;
        update();
    }
}

void FocusTimerWidget::drawRing(QPainter &p)
{
    const TickStyle ts;
    constexpr double startAngle = 0.0; // 3 点钟方向

    const double w = width();
    const double h = height();
    const double base = qMin(w, h);
    const QPointF center(w * 0.5, h * 0.4);
    const double radius = base * ts.ringRadius;
    const double tickW = base * ts.widthRatio;
    const double tickH = base * ts.heightRatio;
    const double tickR = base * ts.cornerRatio;

    double progress = 0.0;
    if (m_state == 0) { // 工作阶段：灰 -> 蓝
        progress = qMin(1.0, m_elapsed / m_workDuration);
    } else { // 休息阶段：蓝 -> 灰
        progress = qMin(1.0, m_elapsed / m_restDuration);
    }

    for (int i = 0; i < ts.count; ++i) {
        double angle = startAngle + (360.0 / ts.count) * i;
        double rad = qDegreesToRadians(angle);
        double x = center.x() + radius * std::cos(rad);
        double y = center.y() + radius * std::sin(rad);

        // 该刻度被进度覆盖的比例（0..1），超出范围自动钳制
        const double cover = qBound(0.0, progress * ts.count - i, 1.0);
        // t 统一表示该刻度的蓝色占比：工作时随进度增大，休息时随进度减小
        const double t = (m_state == 0) ? cover : 1.0 - cover;

        p.save();
        p.translate(x, y);
        // 保持方块朝向不变：长轴仍然指向圆心（径向）
        p.rotate(angle + 90);
        p.setPen(Qt::NoPen);

        // 渐变沿局部 x 轴（即圆环顺时针切线方向），从后侧 (-tickW/2) 到前侧 (+tickW/2)
        QLinearGradient gradient(-tickW * 0.5, 0, tickW * 0.5, 0);
        if (t <= 0.0) {
            gradient.setColorAt(0.0, m_inactiveColor);
            gradient.setColorAt(1.0, m_inactiveColor);
        } else if (t >= 1.0) {
            gradient.setColorAt(0.0, m_activeColor);
            gradient.setColorAt(1.0, m_activeColor);
        } else if (m_state == 0) {
            // 工作：颜色前沿朝顺时针推进，蓝在后侧、灰在前侧
            gradient.setColorAt(0.0, m_activeColor);
            gradient.setColorAt(qMax(0.0, t - ts.frontFade), m_activeColor);
            gradient.setColorAt(qMin(1.0, t + ts.frontFade), m_inactiveColor);
            gradient.setColorAt(1.0, m_inactiveColor);
        } else {
            // 休息：灰的前沿朝顺时针推进，蓝在前侧，前沿渐变方向相应反转
            const double edge = 1.0 - t;
            gradient.setColorAt(0.0, m_inactiveColor);
            gradient.setColorAt(qMax(0.0, edge - ts.frontFade), m_inactiveColor);
            gradient.setColorAt(qMin(1.0, edge + ts.frontFade), m_activeColor);
            gradient.setColorAt(1.0, m_activeColor);
        }

        p.setBrush(gradient);
        p.drawRoundedRect(-tickW * 0.5, -tickH * 0.5, tickW, tickH, tickR, tickR);
        p.restore();
    }
}

void FocusTimerWidget::drawButton(QPainter &p)
{
    const double w = width();
    const double h = height();
    const double base = qMin(w, h);
    const QPointF center(w * 0.5 - base * kButtonHalfGap, h * 0.86);
    // 与「...」按钮同尺寸（直径 ≈ 窗口边长 8%），两按钮整体水平居中
    const double radius = base * kButtonRadiusRatio;
    // 按压瞬时缩小到约 0.92 倍并随 m_pressT 回弹
    const double scale = 1.0 - 0.08 * m_pressT;
    // 悬停提亮系数
    const double lift = m_hoverT * 0.16;

    // 扁平化纯色背景，仅保留悬停提亮
    const QColor bg = m_activeColor.lighter(static_cast<int>(100 + lift * 100));

    p.save();
    p.translate(center);
    p.scale(scale, scale);
    p.translate(-center);

    p.setPen(Qt::NoPen);
    p.setBrush(bg);
    p.drawEllipse(center, radius, radius);

    // 图标平滑过渡：m_iconT 为 1 时是暂停、0 时是播放，中间按透明度交叉淡化
    if (m_iconT >= 1.0) {
        drawPauseIcon(p, center, radius, 1.0);
    } else if (m_iconT <= 0.0) {
        drawPlayIcon(p, center, radius, 1.0);
    } else {
        drawPauseIcon(p, center, radius, m_iconT);
        drawPlayIcon(p, center, radius, 1.0 - m_iconT);
    }

    p.restore();
}

void FocusTimerWidget::drawPauseIcon(QPainter &p, const QPointF &c, double r, double alpha)
{
    // 图标尺寸按按钮半径等比
    const double gap = r * 0.30;
    const double half = r * 0.42;
    const double lineWidth = qMax(1.0, r * 0.22);

    QColor iconColor(0xFF, 0xFF, 0xFF);
    iconColor.setAlphaF(alpha);

    p.setPen(QPen(iconColor, lineWidth, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
    p.setBrush(Qt::NoBrush);
    p.drawLine(c.x() - gap, c.y() - half, c.x() - gap, c.y() + half);
    p.drawLine(c.x() + gap, c.y() - half, c.x() + gap, c.y() + half);
}

void FocusTimerWidget::drawPlayIcon(QPainter &p, const QPointF &c, double r, double alpha)
{
    const double gap = r * 0.30;
    const double half = r * 0.42;

    QColor iconColor(0xFF, 0xFF, 0xFF);
    iconColor.setAlphaF(alpha);

    // 三角形顶点略右移，视觉重心居中
    QPointF points[3] = {
        QPointF(c.x() + gap * 1.3, c.y()),
        QPointF(c.x() - gap, c.y() - half),
        QPointF(c.x() - gap, c.y() + half)
    };
    p.setPen(Qt::NoPen);
    p.setBrush(iconColor);
    p.drawPolygon(points, 3);
}

void FocusTimerWidget::drawDotsButton(QPainter &p)
{
    const double base = qMin(width(), height());
    const QRectF rc = dotsButtonRect();
    const QPointF c = rc.center();
    const double r = rc.width() * 0.5;

    // 常驻淡灰圆底，悬停时提亮（m_dotsHoverT 插值，丝滑过渡）
    const int g = static_cast<int>(0x45 + (0x58 - 0x45) * m_dotsHoverT);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(g, g, g));
    p.drawEllipse(c, r, r);

    // 圆点纯白，与播放按钮白色图标风格统一
    const QColor fg(0xFF, 0xFF, 0xFF);
    const double dotR = qMax(1.0, base * 0.008);
    const double gap = r * 0.55;
    p.setPen(Qt::NoPen);
    p.setBrush(fg);
    for (int i = -1; i <= 1; ++i) {
        p.drawEllipse(QPointF(c.x() + i * gap, c.y()), dotR, dotR);
    }
}

void FocusTimerWidget::showDotsMenu()
{
    auto *popup = new DotsMenuPopup(
        { QStringLiteral("设置"), QStringLiteral("选择种子") }, this, 140);

    // 弹出在「...」按钮正下方，水平居中对齐按钮
    const QRect rc = dotsButtonRect();
    const QPoint anchor = mapToGlobal(QPoint(rc.center().x(), rc.bottom() + 6));
    popup->move(anchor.x() - popup->width() / 2, anchor.y());

    popup->onTriggered = [this](int index) {
        if (index == 0) {
            openSettingsDialog();
        } else if (index == 1) {
            showSeedMenu();
        }
    };
    popup->show();
}

void FocusTimerWidget::showSeedMenu()
{
    const QVector<SeedDescriptor> seeds = SeedRegistry::all();
    QStringList names;
    names.reserve(seeds.size());
    for (const SeedDescriptor &s : seeds) {
        names.push_back(s.displayName);
    }
    auto *popup = new DotsMenuPopup(names, this, 140);
    const QRect rc = dotsButtonRect();
    const QPoint anchor = mapToGlobal(QPoint(rc.center().x(), rc.bottom() + 6));
    popup->move(anchor.x() - popup->width() / 2, anchor.y());
    popup->onTriggered = [this, seeds](int index) {
        if (index >= 0 && index < seeds.size()) {
            setSeedId(seeds.at(index).id);
        }
    };
    popup->show();
}

void FocusTimerWidget::openSettingsDialog()
{
    SettingsDialog dlg(m_workDuration, m_restDuration, this);
    if (dlg.exec() == QDialog::Accepted) {
        setDurations(dlg.workSeconds(), dlg.restSeconds());
        QSettings settings(QStringLiteral("Chandao"), QStringLiteral("FocusTimer"));
        settings.setValue(QStringLiteral("workSeconds"), m_workDuration);
        settings.setValue(QStringLiteral("restSeconds"), m_restDuration);
    }
}

void FocusTimerWidget::drawCloseButton(QPainter &p)
{
    const double base = qMin(width(), height());
    const QRectF rc = closeButtonRect();
    const QPointF c = rc.center();
    const double r = rc.width() * 0.5;

    // 悬停提亮：浮现浅灰圆底 + × 变亮
    if (m_closeHoverT > 0.0) {
        QColor bg = QColor(0xFF, 0xFF, 0xFF);
        bg.setAlphaF(0.10 * m_closeHoverT);
        p.setPen(Qt::NoPen);
        p.setBrush(bg);
        p.drawEllipse(c, r, r);
    }

    QColor fg = m_inactiveColor.lighter(static_cast<int>(160 + 120 * m_closeHoverT));
    const double d = r * 0.32;
    const double lineWidth = qMax(1.0, base * 0.008);
    p.setPen(QPen(fg, lineWidth, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(c.x() - d, c.y() - d, c.x() + d, c.y() + d);
    p.drawLine(c.x() - d, c.y() + d, c.x() + d, c.y() - d);
}

QRect FocusTimerWidget::buttonRect() const
{
    const double w = width();
    const double h = height();
    const double base = qMin(w, h);
    const double radius = base * kButtonRadiusRatio;
    const QPointF center(w * 0.5 - base * kButtonHalfGap, h * 0.86);
    return QRect(static_cast<int>(center.x() - radius),
                 static_cast<int>(center.y() - radius),
                 static_cast<int>(radius * 2),
                 static_cast<int>(radius * 2));
}

QRect FocusTimerWidget::dotsButtonRect() const
{
    const double w = width();
    const double h = height();
    const double base = qMin(w, h);
    const double radius = base * kButtonRadiusRatio;
    // 与播放按钮同一水平线、整体水平居中：位于中线右侧对称位
    const QPointF center(w * 0.5 + base * kButtonHalfGap, h * 0.86);
    return QRect(static_cast<int>(center.x() - radius),
                 static_cast<int>(center.y() - radius),
                 static_cast<int>(radius * 2),
                 static_cast<int>(radius * 2));
}

QRect FocusTimerWidget::closeButtonRect() const
{
    const double base = qMin(width(), height());
    const double d = base * 0.07;
    const double m = base * 0.04;
    // 右上角
    return QRect(static_cast<int>(width() - m - d), static_cast<int>(m),
                 static_cast<int>(d), static_cast<int>(d));
}

void FocusTimerWidget::drawCenterContent(QPainter &p)
{
    switch (m_centerMode) {
    case CenterMode::TimeText:
        drawCenterTimeText(p);
        break;
    case CenterMode::Plant: {
        const SeedDescriptor *seed = SeedRegistry::find(m_seedId);
        const SeedDescriptor desc = seed ? *seed : SeedRegistry::smallTree();
        const double pProg = workProgress();
        const QVector<PlantVertex> mesh = PlantScene::build(desc, pProg);

        const double base = qMin(width(), height());
        const TickStyle ts;
        const QPointF center(width() * 0.5, height() * 0.4);
        const double radius = base * (ts.ringRadius - ts.heightRatio * 0.5);
        const int logical = qMax(1, int(std::lround(radius * 2.0)));
        const qreal dpr = qMax(1.0, devicePixelRatioF());
        const int physical = qBound(1, int(std::ceil(logical * dpr)), 512);

        m_lastPlantVulkanOk = false;
        m_lastPlantVulkanError.clear();
        bool drew = false;
#ifdef HAS_VULKAN
        VulkanPlantRenderer &vk = vkPlant();
        if (!vk.isReady()) {
            vk.init();
        }
        if (vk.isReady()) {
            QImage img = vk.render(mesh, physical);
            if (!img.isNull()) {
                img.setDevicePixelRatio(double(physical) / double(logical));
                p.save();
                QPainterPath clip;
                clip.addEllipse(center, radius, radius);
                p.setClipPath(clip);
                const QRectF dest(center.x() - logical * 0.5,
                                  center.y() - logical * 0.5,
                                  logical, logical);
                p.setRenderHint(QPainter::SmoothPixmapTransform, true);
                p.drawImage(dest, img);
                p.restore();
                m_lastPlantVulkanOk = true;
                drew = true;
            } else {
                m_lastPlantVulkanError = vk.lastError();
            }
        } else {
            m_lastPlantVulkanError = vk.lastError();
        }
#else
        m_lastPlantVulkanError = QStringLiteral("HAS_VULKAN not enabled");
#endif
        if (!drew) {
            if (m_plantVulkanRequired) {
                // 截图模式禁止用回退图充数
            } else {
                qWarning("plant vulkan fallback: %s",
                         qPrintable(m_lastPlantVulkanError));
                drawPlantMesh(p, mesh, center, radius);
            }
        }

        const PlantPhase ph = PlantScene::phaseOf(desc, pProg);
        if (ph.index == ph.visualCount - 1) {
            const double alpha = (m_state == 1) ? 1.0 : ph.localT;
            drawCenterTimeText(p, alpha);
        }
        break;
    }
    default:
        break;
    }
}

void FocusTimerWidget::drawCenterTimeText(QPainter &p, double alpha)
{
    if (alpha <= 0.0) {
        return;
    }
    const double base = qMin(width(), height());
    // 与 drawRing 同一圆心，视觉居中于圆环内
    const QPointF center(width() * 0.5, height() * 0.4);

    // 剩余秒数向上取整（减去微小量避免浮点误差多跳一格）
    const int secs = qMax(0, static_cast<int>(std::ceil(remainingSeconds() - 1e-9)));
    const int hh = secs / 3600;
    const int mm = (secs % 3600) / 60;
    const int ss = secs % 60;
    const QString text = QStringLiteral("%1:%2:%3")
        .arg(hh, 2, 10, QLatin1Char('0'))
        .arg(mm, 2, 10, QLatin1Char('0'))
        .arg(ss, 2, 10, QLatin1Char('0'));

    QFont font = p.font();
    // HH:MM:SS 共 8 字符，字号比纯秒数显示缩小以适配圆环内宽
    font.setPixelSize(static_cast<int>(base * 0.075));
    font.setBold(true);
    p.setFont(font);
    QColor pen = (m_state == 0) ? QColor(0xE8, 0xE8, 0xE8) : m_activeColor;
    pen.setAlphaF(qBound(0.0, alpha, 1.0));
    p.setPen(pen);

    const double half = base * 0.22;
    p.drawText(QRectF(center.x() - half, center.y() - half, half * 2, half * 2),
               Qt::AlignCenter, text);
}

double FocusTimerWidget::approach(double value, double target, double step)
{
    if (value < target) {
        return qMin(value + step, target);
    }
    return qMax(value - step, target);
}

void FocusTimerWidget::updateTimer()
{
    if (m_isRunning) {
        m_elapsed += 0.016;

        if (m_state == 0) {
            if (m_elapsed >= m_workDuration) {
                m_state = 1;
                m_elapsed = 0.0;
                emit phaseChanged(m_state);
            }
        } else {
            if (m_elapsed >= m_restDuration) {
                m_state = 0;
                m_elapsed = 0.0;
                emit phaseChanged(m_state);
            }
        }
    }

    // 截图模式（setTime 停表后）不再推进动画，保证画面确定
    if (!m_timerStopped) {
        m_hoverT = approach(m_hoverT, m_buttonHovered ? 1.0 : 0.0, kHoverStep);
        m_pressT = approach(m_pressT, m_buttonPressed ? 1.0 : 0.0, kPressStep);
        m_iconT = approach(m_iconT, m_isRunning ? 1.0 : 0.0, kIconStep);
        m_closeHoverT = approach(m_closeHoverT, m_closeHovered ? 1.0 : 0.0, kHoverStep);
        m_dotsHoverT = approach(m_dotsHoverT, m_dotsHovered ? 1.0 : 0.0, kHoverStep);
    }

    update();
}
