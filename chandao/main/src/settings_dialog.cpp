#include "settings_dialog.h"

#include "genie_ghost.h"

#include <QLabel>
#include <QPushButton>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QPainter>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QPropertyAnimation>
#include <QShowEvent>
#include <QTimer>
#include <cmath>

// 竖向滚筒选择器：按住上下拖动滚动，松手指数趋近吸附到最近项，支持鼠标滚轮。
// 数值以「项」为单位保存为浮点偏移，居中项为当前值。
class WheelPicker : public QWidget
{
public:
    WheelPicker(int minValue, int maxValue, QWidget *parent = nullptr)
        : QWidget(parent)
        , m_min(minValue)
        , m_max(maxValue)
    {
        setFixedSize(56, kItemH * kVisible);
        setCursor(Qt::OpenHandCursor);
        m_timer = new QTimer(this);
        connect(m_timer, &QTimer::timeout, this, [this] { tick(); });
        m_timer->start(16); // 约 60fps 吸附动画
    }

    int value() const
    {
        return qBound(m_min, static_cast<int>(std::lround(m_offset)) + m_min, m_max);
    }

    void setValue(int v)
    {
        m_offset = qBound(0, v - m_min, m_max - m_min);
        m_target = m_offset;
        update();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        p.setRenderHint(QPainter::Antialiasing);

        const double centerY = height() * 0.5;
        // 选中行底色带，提供视觉锚点
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0x3A, 0x3A, 0x3A));
        p.drawRoundedRect(QRectF(2.0, centerY - kItemH * 0.5, width() - 4.0, kItemH), 6.0, 6.0);

        const int count = m_max - m_min + 1;
        for (int i = 0; i < count; ++i) {
            const double y = centerY + (i - m_offset) * kItemH;
            if (y < -kItemH || y > height() + kItemH) {
                continue;
            }
            // 距中心越远越暗、越小；选中项加粗纯白
            const double dist = std::abs(i - m_offset);
            const double t = qBound(0.0, 1.0 - dist / (kVisible * 0.5), 1.0);
            const bool selected = (std::lround(m_offset) == i);
            const int g = static_cast<int>(0x66 + (0xFF - 0x66) * t);
            QFont font = p.font();
            font.setPixelSize(selected ? 19 : 16);
            font.setBold(selected);
            p.setFont(font);
            p.setPen(QColor(g, g, g));
            p.drawText(QRectF(0.0, y - kItemH * 0.5, width(), kItemH),
                       Qt::AlignCenter,
                       QString::number(m_min + i).rightJustified(2, QLatin1Char('0')));
        }

        // 顶/底渐隐色带：数字接近滚筒边缘时融入背景
        const double fadeH = kItemH * 1.5;
        const QColor bg(0x2B, 0x2B, 0x2B);
        QLinearGradient topFade(0.0, 0.0, 0.0, fadeH);
        topFade.setColorAt(0.0, bg);
        topFade.setColorAt(1.0, QColor(bg.red(), bg.green(), bg.blue(), 0));
        p.fillRect(QRectF(0.0, 0.0, width(), fadeH), topFade);
        QLinearGradient bottomFade(0.0, height() - fadeH, 0.0, height());
        bottomFade.setColorAt(0.0, QColor(bg.red(), bg.green(), bg.blue(), 0));
        bottomFade.setColorAt(1.0, bg);
        p.fillRect(QRectF(0.0, height() - fadeH, width(), fadeH), bottomFade);
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton) {
            m_dragging = true;
            m_dragStartY = event->pos().y();
            m_dragStartOffset = m_offset;
            setCursor(Qt::ClosedHandCursor);
        }
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        if (m_dragging) {
            // 向上拖 → 数值增大；拖动中不吸附，直接跟手
            m_offset = qBound(0.0,
                              m_dragStartOffset + (m_dragStartY - event->pos().y()) / double(kItemH),
                              double(m_max - m_min));
            m_target = m_offset;
            update();
        }
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() == Qt::LeftButton && m_dragging) {
            m_dragging = false;
            setCursor(Qt::OpenHandCursor);
            m_target = qBound(0.0, std::round(m_offset), double(m_max - m_min)); // 松手吸附
        }
    }

    void wheelEvent(QWheelEvent *event) override
    {
        const int steps = event->angleDelta().y() / 120;
        m_target = qBound(0.0, std::round(m_target) - steps, double(m_max - m_min));
    }

private:
    void tick()
    {
        if (m_dragging) {
            return;
        }
        const double diff = m_target - m_offset;
        if (std::abs(diff) < 0.01) {
            if (m_offset != m_target) {
                m_offset = m_target;
                update();
            }
            return;
        }
        m_offset += diff * 0.25; // 指数趋近，吸附丝滑
        update();
    }

    static constexpr int kItemH = 34;
    static constexpr int kVisible = 5;

    int m_min;
    int m_max;
    double m_offset = 0.0; // 滚动位置（单位：项）
    double m_target = 0.0;
    bool m_dragging = false;
    int m_dragStartY = 0;
    double m_dragStartOffset = 0.0;
    QTimer *m_timer;
};

SettingsDialog::SettingsDialog(double workSec, double restSec, QWidget *parent)
    : QDialog(parent, Qt::Dialog | Qt::FramelessWindowHint)
{
    setAttribute(Qt::WA_TranslucentBackground); // 圆角之外真正透明
    setFixedSize(420, 460);
    setWindowOpacity(0.0); // 弹出动画从全透明开始，避免首帧闪现
    setStyleSheet(QStringLiteral(
        "QLabel { color: #E8E8E8; background: transparent; }"
        "QPushButton { background: #55B2E8; color: #FFFFFF; border: none;"
        "              border-radius: 6px; padding: 10px 40px; }"
        "QPushButton:hover { background: #6CC0F0; }"
        "QPushButton:pressed { background: #4A9FD4; }"));

    QLabel *titleLabel = new QLabel(QStringLiteral("设置"), this);
    titleLabel->setAlignment(Qt::AlignCenter);
    QFont titleFont = titleLabel->font();
    titleFont.setPixelSize(18);
    titleFont.setBold(true);
    titleLabel->setFont(titleFont);

    const auto secsToHms = [](double secs, int &h, int &m, int &s) {
        const int total = qBound(0, static_cast<int>(secs), 359999); // 滚筒上限 99:59:59
        h = total / 3600;
        m = (total % 3600) / 60;
        s = total % 60;
    };
    int h = 0, m = 0, s = 0;

    const auto makeTimeRow = [this](const QString &name, WheelPicker *&wh, WheelPicker *&wm, WheelPicker *&ws) {
        wh = new WheelPicker(0, 99, this);
        wm = new WheelPicker(0, 59, this);
        ws = new WheelPicker(0, 59, this);
        // 单位放在滚筒外侧右侧，与选中行垂直居中，避免和数字抢同一格被裁切/遮挡
        const auto makeUnit = [this](const QString &text) {
            QLabel *lab = new QLabel(text, this);
            QFont f = lab->font();
            f.setPixelSize(14);
            lab->setFont(f);
            lab->setStyleSheet(QStringLiteral("color: #C8C8C8;"));
            lab->setAlignment(Qt::AlignVCenter | Qt::AlignLeft);
            return lab;
        };
        const auto addWheel = [&](QHBoxLayout *into, WheelPicker *wheel, const QString &unit) {
            QHBoxLayout *group = new QHBoxLayout;
            group->setSpacing(6);
            group->setContentsMargins(0, 0, 0, 0);
            group->addWidget(wheel);
            group->addWidget(makeUnit(unit));
            into->addLayout(group);
        };
        QHBoxLayout *row = new QHBoxLayout;
        row->setSpacing(12);
        QLabel *nameLabel = new QLabel(name, this);
        QFont nameFont = nameLabel->font();
        nameFont.setPixelSize(15);
        nameLabel->setFont(nameFont);
        row->addWidget(nameLabel);
        row->addStretch();
        addWheel(row, wh, QStringLiteral("时"));
        addWheel(row, wm, QStringLiteral("分"));
        addWheel(row, ws, QStringLiteral("秒"));
        return row;
    };

    QHBoxLayout *workRow = makeTimeRow(QStringLiteral("专注时间"), m_workH, m_workM, m_workS);
    secsToHms(workSec, h, m, s);
    m_workH->setValue(h);
    m_workM->setValue(m);
    m_workS->setValue(s);

    QHBoxLayout *restRow = makeTimeRow(QStringLiteral("休息时间"), m_restH, m_restM, m_restS);
    secsToHms(restSec, h, m, s);
    m_restH->setValue(h);
    m_restM->setValue(m);
    m_restS->setValue(s);

    QPushButton *okButton = new QPushButton(QStringLiteral("确定"), this);
    connect(okButton, &QPushButton::clicked, this, &QDialog::accept);

    QVBoxLayout *layout = new QVBoxLayout(this);
    layout->setContentsMargins(24, 14, 24, 24);
    layout->addWidget(titleLabel);
    layout->addSpacing(12);
    layout->addLayout(workRow);
    layout->addSpacing(20);
    layout->addLayout(restRow);
    layout->addStretch(); // 「确定」落在中间偏下
    layout->addWidget(okButton, 0, Qt::AlignHCenter);

    if (parent) {
        // 弹出时居中于父窗口
        const QRect pg = parent->window()->geometry();
        move(pg.center() - rect().center());
    }
}

double SettingsDialog::workSeconds() const
{
    return m_workH->value() * 3600 + m_workM->value() * 60 + m_workS->value();
}

double SettingsDialog::restSeconds() const
{
    return m_restH->value() * 3600 + m_restM->value() * 60 + m_restS->value();
}

QRect SettingsDialog::closeButtonRect() const
{
    const int d = 24;
    const int m = 10;
    return QRect(width() - m - d, m, d, d);
}

QRect SettingsDialog::titleRect() const
{
    // 顶部标题区（不含右上角 × 热区）
    return QRect(0, 0, closeButtonRect().left(), 44);
}

void SettingsDialog::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);

    // 圆角背景（WA_TranslucentBackground 保证四角透明）
    p.setPen(QPen(QColor(0x3D, 0x3D, 0x3D), 1));
    p.setBrush(QColor(0x2B, 0x2B, 0x2B));
    p.drawRoundedRect(QRectF(rect()).adjusted(0.5, 0.5, -0.5, -0.5), 12.0, 12.0);

    // 右上角自绘 ×
    const QPointF c = closeButtonRect().center();
    const double d = 6.0;
    p.setPen(QPen(QColor(0xE8, 0xE8, 0xE8), 1.6, Qt::SolidLine, Qt::RoundCap));
    p.drawLine(QPointF(c.x() - d, c.y() - d), QPointF(c.x() + d, c.y() + d));
    p.drawLine(QPointF(c.x() - d, c.y() + d), QPointF(c.x() + d, c.y() - d));
}

void SettingsDialog::showEvent(QShowEvent *event)
{
    QDialog::showEvent(event);
    // 丝滑弹出：淡入 + 从上方 14px 处滑入
    auto *fade = new QPropertyAnimation(this, "windowOpacity", this);
    fade->setDuration(200);
    fade->setStartValue(0.0);
    fade->setEndValue(1.0);
    fade->setEasingCurve(QEasingCurve::OutCubic);
    fade->start(QAbstractAnimation::DeleteWhenStopped);

    auto *slide = new QPropertyAnimation(this, "pos", this);
    slide->setDuration(200);
    slide->setStartValue(pos() - QPoint(0, 14));
    slide->setEndValue(pos());
    slide->setEasingCurve(QEasingCurve::OutCubic);
    slide->start(QAbstractAnimation::DeleteWhenStopped);
}

void SettingsDialog::reject()
{
    if (m_closing) {
        QDialog::reject(); // 动画结束后的真正关闭
        return;
    }
    m_closing = true;

    const QRect geo = geometry();
    const QPoint target = mapToGlobal(closeButtonRect().center());
    QPixmap shot = grab();

    auto *ghost = new GenieGhost(shot);
    connect(ghost, &QWidget::destroyed, this, [this] {
        QDialog::reject();
    });

    // 快照先完整上屏；模态 Dialog 不能 hide()（会提前 done），只能改透明度。
    // 本窗已是 WA_TranslucentBackground，opacity 不会重建 HWND。
    ghost->appearAt(geo);
    setEnabled(false);
    setUpdatesEnabled(false);
    setWindowOpacity(0);
    ghost->suckInto(target);
}

void SettingsDialog::mousePressEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        if (closeButtonRect().contains(event->pos())) {
            reject(); // × 关闭：取消不保存
            return;
        }
        if (titleRect().contains(event->pos())) {
            m_moving = true;
            m_moveOffset = event->globalPosition().toPoint() - frameGeometry().topLeft();
            return;
        }
    }
    QDialog::mousePressEvent(event);
}

void SettingsDialog::mouseMoveEvent(QMouseEvent *event)
{
    if (m_moving) {
        move(event->globalPosition().toPoint() - m_moveOffset);
        return;
    }
    QDialog::mouseMoveEvent(event);
}

void SettingsDialog::mouseReleaseEvent(QMouseEvent *event)
{
    if (event->button() == Qt::LeftButton) {
        m_moving = false;
    }
    QDialog::mouseReleaseEvent(event);
}
