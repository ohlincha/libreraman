#include "splash.h"

#include <QFontMetricsF>
#include <QGuiApplication>
#include <QLinearGradient>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QScreen>

#include <algorithm>
#include <cmath>

namespace lr::gui {

QImage drawLogo(qreal dpr, const QString& version) {
    const int w = kLogoWidth, h = kLogoHeight;
    QImage img(QSize(int(w * dpr), int(h * dpr)), QImage::Format_ARGB32_Premultiplied);
    img.setDevicePixelRatio(dpr);
    img.fill(Qt::transparent);
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::TextAntialiasing);
    QLinearGradient g(0, 0, 0, h);
    g.setColorAt(0, QColor("#1d2433"));
    g.setColorAt(1, QColor("#10141d"));
    p.setBrush(g);
    p.setPen(Qt::NoPen);
    p.drawRoundedRect(QRectF(0, 0, w, h), 18, 18);

    const double cx = 140, top = 120;
    // steam = a Raman spectrum (water/ethanol-like bands)
    const double bands[5][3] = {{0.18, 0.35, 0.03}, {0.33, 0.55, 0.02}, {0.47, 0.95, 0.018},
                                {0.62, 0.45, 0.025}, {0.8, 0.7, 0.03}};
    QPainterPath path;
    for (int i = 0; i <= 120; ++i) {
        const double x = i / 120.0;
        double y = 0.05;
        for (const auto& b : bands)
            y += b[1] * std::exp(-std::pow((x - b[0]) / b[2], 2));
        const QPointF pt(cx - 75 + x * 150, top - 8 - y * 85);
        if (i == 0)
            path.moveTo(pt);
        else
            path.lineTo(pt);
    }
    QPen steam(QColor("#ff8f5a"), 3);
    steam.setJoinStyle(Qt::RoundJoin);
    steam.setCapStyle(Qt::RoundCap);
    p.setPen(steam);
    p.setBrush(Qt::NoBrush);
    p.drawPath(path);
    // bowl
    QPainterPath bowl;
    bowl.moveTo(cx - 90, top + 10);
    bowl.cubicTo(cx - 85, top + 95, cx + 85, top + 95, cx + 90, top + 10);
    bowl.closeSubpath();
    p.setPen(Qt::NoPen);
    p.setBrush(QColor("#d6493b"));
    p.drawPath(bowl);
    p.setBrush(QColor("#f2c94c"));
    p.drawRoundedRect(QRectF(cx - 90, top + 2, 180, 14), 7, 7);
    p.setPen(QPen(QColor("#ffffff"), 2));
    for (int k = 0; k < 4; ++k)                              // noodles
        p.drawArc(QRectF(cx - 60 + k * 30, top - 2, 26, 14), 0, 180 * 16);
    QPen sticks(QColor("#c9b28a"), 4);                       // chopsticks
    sticks.setCapStyle(Qt::RoundCap);
    p.setPen(sticks);
    p.drawLine(QPointF(cx + 40, top - 70), QPointF(cx + 110, top + 30));
    p.drawLine(QPointF(cx + 55, top - 75), QPointF(cx + 118, top + 22));

    QFont f = QGuiApplication::font();
    f.setPixelSize(40);
    f.setBold(true);
    p.setFont(f);
    p.setPen(QColor("#ffffff"));
    p.drawText(QRectF(255, 66, 300, 54), Qt::AlignLeft | Qt::AlignVCenter, QStringLiteral("LibreRaman"));
    f.setPixelSize(15);
    f.setBold(false);
    p.setFont(f);
    p.setPen(QColor("#b8c2d6"));
    p.drawText(QRectF(258, 122, 300, 60), Qt::AlignLeft | Qt::TextWordWrap,
               QStringLiteral("Open-source software for Raman\nspectrometers that deserve a second life"));
    f.setPixelSize(12);
    p.setFont(f);
    p.setPen(QColor("#7d879b"));
    p.drawText(QRectF(258, 180, 300, 20), Qt::AlignLeft, QStringLiteral("Version %1 · MIT licence").arg(version));
    p.end();
    return img;
}

static qreal screenDpr() {
    const QScreen* s = QGuiApplication::primaryScreen();
    return s ? s->devicePixelRatio() : 1.0;
}

Splash::Splash(const QString& version)
    : QSplashScreen(QPixmap::fromImage(drawLogo(screenDpr(), version))), m_version(version) {
    setAttribute(Qt::WA_TranslucentBackground);
    setCursor(Qt::PointingHandCursor);
    setToolTip(QStringLiteral("Click to continue"));
}

void Splash::message(const QString& text) {
    m_text = text;
    repaint();
}

void Splash::setProgress(double fraction) {
    m_progress = fraction < 0 ? -1 : std::min(1.0, fraction);
    repaint();
}

void Splash::paintOverlay(QPainter& p, const QSizeF& size) const {
    p.setRenderHint(QPainter::Antialiasing);
    p.setRenderHint(QPainter::TextAntialiasing);
    QFont f = QGuiApplication::font();
    f.setPixelSize(13);
    p.setFont(f);
    const QFontMetricsF fm(f);
    const double textTop = size.height() - 12 - fm.height();
    if (m_progress >= 0) {
        const QRectF track(18, textTop - 12, size.width() - 36, 4);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(255, 255, 255, 22));
        p.drawRoundedRect(track, 2, 2);
        p.setBrush(QColor("#ff8f5a"));
        p.drawRoundedRect(QRectF(track.topLeft(), QSizeF(std::max(4.0, track.width() * m_progress), 4)), 2, 2);
    }
    if (!m_text.isEmpty()) {
        p.setPen(QColor("#d7deea"));
        p.drawText(QRectF(18, textTop, size.width() - 36, fm.height()), Qt::AlignLeft | Qt::AlignVCenter,
                   fm.elidedText(m_text, Qt::ElideRight, size.width() - 36));
    }
}

void Splash::drawContents(QPainter* p) { paintOverlay(*p, QSizeF(kLogoWidth, kLogoHeight)); }

QImage Splash::renderImage() const {
    QImage img = drawLogo(2.0, m_version);
    QPainter p(&img);
    paintOverlay(p, QSizeF(kLogoWidth, kLogoHeight));
    p.end();
    return img;
}

void Splash::mousePressEvent(QMouseEvent* e) {
    emit skipped();
    QSplashScreen::mousePressEvent(e);
}

}  // namespace lr::gui
