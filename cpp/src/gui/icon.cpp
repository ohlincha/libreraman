#include "icon.h"

#include <QFile>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>

#include <cmath>

namespace lr::gui {

QImage drawIcon(int size) {
    const double s = size / 1024.0;
    QImage img(size, size, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing);
    // background: dark rounded square (macOS-style margins); small sizes use the space for the motif
    const double m = (size >= 64 ? 100 : 24) * s;
    QLinearGradient g(0, m, 0, size - m);
    g.setColorAt(0, QColor("#26304a"));
    g.setColorAt(1, QColor("#121722"));
    p.setPen(QPen(QColor(255, 255, 255, 60), std::max(1.0, 8 * s)));    // faint edge for dark backgrounds
    p.setBrush(g);
    p.drawRoundedRect(QRectF(m, m, size - 2 * m, size - 2 * m), 185 * s, 185 * s);
    // steam = Raman spectrum (few, tall bands so that it reads at small sizes)
    const double bands[5][3] = {{0.16, 0.42, 0.035}, {0.36, 0.62, 0.03}, {0.54, 1.0, 0.03},
                                {0.72, 0.55, 0.035}, {0.88, 0.3, 0.035}};
    QPainterPath path;
    const double x0 = 270 * s, x1 = 754 * s, base = 560 * s, amp = 330 * s;
    for (int i = 0; i <= 240; ++i) {
        const double t = i / 240.0;
        double y = 0.03;
        for (const auto& b : bands)
            y += b[1] * std::exp(-std::pow((t - b[0]) / b[2], 2));
        const QPointF pt(x0 + t * (x1 - x0), base - y * amp);
        if (i == 0)
            path.moveTo(pt);
        else
            path.lineTo(pt);
    }
    QPen pen(QColor("#ff9a5c"), std::max(1.5, 34 * s));
    pen.setCapStyle(Qt::RoundCap);
    pen.setJoinStyle(Qt::RoundJoin);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    p.drawPath(path);
    // bowl
    QPainterPath bowl;
    bowl.moveTo(225 * s, 600 * s);
    bowl.cubicTo(235 * s, 860 * s, 789 * s, 860 * s, 799 * s, 600 * s);
    bowl.closeSubpath();
    p.setPen(Qt::NoPen);
    p.setBrush(QColor("#e04a3c"));
    p.drawPath(bowl);
    p.setBrush(QColor("#f2c94c"));                                       // rim / broth
    p.drawRoundedRect(QRectF(205 * s, 570 * s, 614 * s, 60 * s), 30 * s, 30 * s);
    if (size >= 48) {                                                    // details only where visible
        p.setPen(QPen(QColor("#ffffff"), 14 * s));
        for (int k = 0; k < 4; ++k)
            p.drawArc(QRectF((300 + k * 110) * s, 560 * s, 90 * s, 50 * s), 0, 180 * 16);
        p.setPen(QPen(QColor("#b03428"), 16 * s));
        p.drawLine(QPointF(330 * s, 740 * s), QPointF(694 * s, 740 * s));
    }
    p.end();
    return img;
}

QIcon appIcon() {
    QIcon icon;
    for (int sz : {16, 24, 32, 48, 64, 128, 256, 512})
        icon.addPixmap(QPixmap::fromImage(drawIcon(sz)));
    if (QFile::exists(QStringLiteral(":/gui/icon.png")))
        icon.addFile(QStringLiteral(":/gui/icon.png"), QSize(1024, 1024));
    return icon;
}

}  // namespace lr::gui
