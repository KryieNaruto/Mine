#include <QApplication>
#include <QPixmap>
#include <QImage>
#include <QStringList>
#include <QSettings>
#include <QFile>
#include <QTextStream>
#include "focus_timer_widget.h"
#include "frameless_window.h"

static int writePlantVulkanFailed(const QString &detail)
{
    QFile f(QStringLiteral("plant-vulkan-failed.txt"));
    if (f.open(QIODevice::WriteOnly | QIODevice::Truncate | QIODevice::Text)) {
        QTextStream out(&f);
        out << QStringLiteral("plant-vulkan-failed\n");
        if (!detail.isEmpty()) {
            out << detail << '\n';
        }
    }
    return 2;
}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    // 命令行：-t <秒数> 输出该时刻截图；-s <边长>；-c plant 植物中心；--seed <id>
    const QStringList args = app.arguments();
    QString timeArg;
    QString centerArg;
    QString seedArg;
    int shotSize = 400;
    for (int i = 1; i < args.size(); ++i) {
        if (args.at(i) == QStringLiteral("-t") && i + 1 < args.size()) {
            timeArg = args.at(i + 1);
            ++i;
        } else if (args.at(i) == QStringLiteral("-s") && i + 1 < args.size()) {
            shotSize = args.at(i + 1).toInt();
            ++i;
        } else if (args.at(i) == QStringLiteral("-c") && i + 1 < args.size()) {
            centerArg = args.at(i + 1);
            ++i;
        } else if (args.at(i) == QStringLiteral("--seed") && i + 1 < args.size()) {
            seedArg = args.at(i + 1);
            ++i;
        }
    }

    const bool plantShot = (centerArg == QStringLiteral("plant"));

    if (!timeArg.isEmpty()) {
        bool ok = false;
        const double t = timeArg.toDouble(&ok);
        if (!ok || shotSize <= 0) {
            return 1;
        }

        FocusTimerWidget widget;
        widget.resize(shotSize, shotSize);
        // 先停表，避免 setCenterMode/setSeedId 把截图参数写入 QSettings
        widget.setTime(t);
        if (!seedArg.isEmpty()) {
            widget.setSeedId(seedArg);
        }
        if (plantShot) {
            widget.setPlantVulkanRequired(true);
            widget.setCenterMode(FocusTimerWidget::CenterMode::Plant);
        }

        const QString fileName = QStringLiteral("shot_%1.png").arg(timeArg);
        QPixmap pixmap = widget.grab();
        if (plantShot && !widget.lastPlantVulkanOk()) {
            return writePlantVulkanFailed(widget.lastPlantVulkanError());
        }
        QImage image = pixmap.toImage();
        // 高 DPI 下 grab() 返回物理像素，需归一化到 -s 指定的逻辑尺寸
        if (image.size() != QSize(shotSize, shotSize)) {
            image = image.scaled(shotSize, shotSize,
                                 Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
        }
        if (!image.save(fileName, "PNG")) {
            return 1;
        }
        return 0;
    }

    // 交互模式：无边框窗口（尺寸/居中在 FramelessWindow 构造中完成）
    FramelessWindow window;
    window.setWindowTitle(QStringLiteral("专注"));

    // 加载持久化的专注/休息时长与中心样式（截图模式不加载）
    QSettings settings(QStringLiteral("Chandao"), QStringLiteral("FocusTimer"));
    const double workSec = settings.value(QStringLiteral("workSeconds"), 10.0).toDouble();
    const double restSec = settings.value(QStringLiteral("restSeconds"), 10.0).toDouble();
    window.timerWidget()->setDurations(workSec, restSec);
    window.timerWidget()->setSeedId(
        settings.value(QStringLiteral("seedId"), QStringLiteral("small_tree")).toString());
    if (settings.value(QStringLiteral("centerMode")).toString() == QStringLiteral("plant")) {
        window.timerWidget()->setCenterMode(FocusTimerWidget::CenterMode::Plant);
    }

    window.show();
    return app.exec();
}
