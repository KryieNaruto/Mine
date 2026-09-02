#pragma once

#include <QWidget>
#include <QColor>
#include <QTimer>
#include <QPointF>
#include <QString>

// 表盘刻度样式参数。所有「*比」均相对窗口短边 min(w,h)，改这里即可整体调样式。
// 调节建议：widthRatio 别超过 heightRatio 的一半，否则会糊成圆环；
// cornerRatio 约取 widthRatio 一半呈胶囊形；frontFade 越小进度前沿越锐利。
struct TickStyle {
    int    count       = 36;     // 刻度数量（一圈均分）
    double widthRatio  = 0.026;  // 刻度宽度比（0.04 偏粗，0.026 细且清晰）
    double heightRatio = 0.08;   // 刻度高度比（径向长度）
    double cornerRatio = 0.013;  // 圆角比（约等于宽度一半呈胶囊形）
    double ringRadius  = 0.28;   // 环半径比（刻度中心到表盘中心）
    double frontFade   = 0.03;   // 进度前沿渐变过渡带宽（0..1，占单个刻度宽度比例）
};

class FocusTimerWidget : public QWidget
{
    Q_OBJECT

public:
    explicit FocusTimerWidget(QWidget *parent = nullptr);

    // 确定性地将计时状态设置为指定秒数（按工作/休息循环取模），并停止内部定时器
    void setTime(double seconds);

    // 供无边框窗口使用：命中右上角关闭按钮 / 播放按钮 / 「...」按钮 / 圆心热区
    bool closeButtonHit(const QPoint &pos) const;
    bool playButtonHit(const QPoint &pos) const;
    bool dotsButtonHit(const QPoint &pos) const;
    bool centerHit(const QPoint &pos) const;

    // 关闭按钮中心的全局坐标（吸入式关闭动画的收缩目标点）
    QPoint closeButtonCenterGlobal() const;

    // 应用新的工作/休息时长（秒）。当前阶段：新时长 > 已用则续跑，
    // 已用 >= 新时长则立即切到下一阶段（信号规则与 updateTimer 一致）
    void setDurations(double workSec, double restSec);
    double workSeconds() const { return m_workDuration; }
    double restSeconds() const { return m_restDuration; }

    // 圆环中心内容模式
    enum class CenterMode { TimeText, Plant };
    void setCenterMode(CenterMode mode);
    CenterMode centerMode() const { return m_centerMode; }
    void setSeedId(const QString &id);
    QString seedId() const { return m_seedId; }
    // 截图 -c plant：必须走 Vulkan，失败则 lastPlantVulkanOk() 为 false
    void setPlantVulkanRequired(bool required);
    bool lastPlantVulkanOk() const { return m_lastPlantVulkanOk; }
    QString lastPlantVulkanError() const { return m_lastPlantVulkanError; }

    // 当前阶段剩余秒数（供全屏休息提醒等外部组件显示倒计时）
    double remainingSeconds() const;

signals:
    // 阶段切换：0 = 工作，1 = 休息（setTime 截图模式不触发）
    void phaseChanged(int state);
    // 「...」按钮点击
    void dotsClicked();

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;
    QSize sizeHint() const override;

private slots:
    void updateTimer();

private:
    void drawRing(QPainter &p);
    void drawButton(QPainter &p);
    void drawDotsButton(QPainter &p);
    void drawCloseButton(QPainter &p);
    void drawCenterContent(QPainter &p);
    void drawCenterTimeText(QPainter &p, double alpha = 1.0);
    void persistAppearance() const;
    void showSeedMenu();
    double workProgress() const;
    QRect buttonRect() const;
    QRect dotsButtonRect() const;
    QRect closeButtonRect() const;
    void showDotsMenu();
    void openSettingsDialog();
    void drawPauseIcon(QPainter &p, const QPointF &c, double r, double alpha);
    void drawPlayIcon(QPainter &p, const QPointF &c, double r, double alpha);
    static double approach(double value, double target, double step);

    QTimer *m_timer;
    QColor m_activeColor;
    QColor m_inactiveColor;
    QColor m_bgColor;
    bool m_isRunning;
    int m_state; // 0: 工作, 1: 休息
    double m_elapsed;
    double m_workDuration;
    double m_restDuration;

    // 交互动画状态（0..1 插值系数，由现有 60fps QTimer 驱动）
    double m_hoverT = 0.0;  // 播放按钮悬停提亮
    double m_pressT = 0.0;  // 播放按钮按压（1 = 完全按下）
    double m_iconT  = 1.0;  // 图标过渡：1 = 暂停图标，0 = 播放图标
    double m_closeHoverT = 0.0; // 关闭按钮悬停提亮
    double m_dotsHoverT = 0.0;  // 「...」按钮悬停提亮
    bool m_buttonHovered = false;
    bool m_buttonPressed = false;
    bool m_closeHovered = false;
    bool m_dotsHovered = false;
    bool m_dotsPressed = false;
    bool m_timerStopped = false; // setTime 停止定时器后置位，不再触发动画插值
    CenterMode m_centerMode = CenterMode::TimeText;
    QString m_seedId = QStringLiteral("small_tree");
    bool m_centerPressed = false;
    bool m_plantVulkanRequired = false;
    bool m_lastPlantVulkanOk = false;
    QString m_lastPlantVulkanError;
};
