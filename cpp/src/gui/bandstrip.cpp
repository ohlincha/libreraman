#include "bandstrip.h"

#include <QFontMetricsF>
#include <QPainter>

#include <algorithm>

namespace lr::gui {

namespace {
constexpr int kMaxLanes = 5;
}

BandStripItem::BandStripItem(PlotWidget* plot) : PlotItem(plot) {
    setAutoScale(false);
    setZValue(-5);                                 // under the curves
}

void BandStripItem::setCategories(bool organic, bool inorganic) {
    m_organic = organic;
    m_inorganic = inorganic;
    changed();
}

QString BandStripItem::toolTipAt(const QPointF& pos) const {
    QStringList lines;
    for (const Hit& h : m_hits)
        if (h.rect.adjusted(-2, -1, 2, 1).contains(pos)) {
            QString s = h.band->label().toHtmlEscaped();
            if (!h.band->note.isEmpty())
                s += QStringLiteral("<br><i>%1</i>").arg(h.band->note.toHtmlEscaped());
            lines << s;
        }
    return lines.isEmpty() ? QString() : QStringLiteral("<p>%1</p>").arg(lines.join(QStringLiteral("<br>")));
}

void BandStripItem::paint(QPainter& p, const PlotTransform& t) {
    m_hits.clear();
    if (!m_organic && !m_inorganic)
        return;
    const PlotPalette& pal = plot()->plotPalette();
    QFont f = p.font();
    f.setPointSizeF(f.pointSizeF() * 0.85);
    p.save();
    p.setFont(f);
    const QFontMetricsF fm(f);
    const double lh = fm.height() + 3;
    const double x0 = std::min(t.x.lo, t.x.hi), x1 = std::max(t.x.lo, t.x.hi);

    // visible bands, narrow (specific) ones first so they get the lanes
    std::vector<const bands::Band*> vis;
    for (const bands::Band& b : bands::all())
        if (((m_organic && b.organic()) || (m_inorganic && b.inorganic())) && b.hi >= x0 && b.lo <= x1)
            vis.push_back(&b);
    std::stable_sort(vis.begin(), vis.end(),
                     [](const bands::Band* a, const bands::Band* b) { return a->width() < b->width(); });

    std::vector<std::vector<std::pair<double, double>>> lanes(kMaxLanes);
    auto free = [&](int lane, double a, double b) {
        for (const auto& [u, v] : lanes[size_t(lane)])
            if (a < v + 3 && b > u - 3)
                return false;
        return true;
    };
    auto colour = [&](const bands::Band& b) {
        if (b.category == QLatin1String("inorganic"))
            return pal.seriesColor(1);
        if (b.category == QLatin1String("both"))
            return pal.seriesColor(2);
        return pal.seriesColor(0);
    };

    for (const bands::Band* b : vis) {
        double a = t.px(b->lo), e = t.px(b->hi);
        if (a > e)
            std::swap(a, e);
        a = std::max(a, t.area.left());
        e = std::min(e, t.area.right());
        // short label: ranges by group ("C=O"), sharp characteristic bands by compound/mineral ("gypsum")
        const QString qual = b->group.section(QLatin1Char('('), 1).section(QLatin1Char(')'), 0, 0).trimmed();
        const QString base = b->group.section(QStringLiteral(" ("), 0, 0);
        QString name = base;
        if (b->sharp() && !qual.isEmpty()) {
            if (qual == QLatin1String("aqueous"))
                name = base + QStringLiteral("(aq)");
            else if (qual.startsWith(QLatin1String("gas")))
                name = base + QStringLiteral("(g)");
            else
                name = qual;
        }
        const double tw = fm.horizontalAdvance(name) + 6;
        double ba = a, bb = std::max(e, a + 3);            // bar extent in pixels
        double la = ba, lb = std::max(bb, ba + tw);       // occupied extent incl. the label
        if (b->sharp()) {                                  // label centred on the line
            la = a - tw / 2;
            lb = a + tw / 2;
        }
        int lane = -1;
        for (int k = 0; k < kMaxLanes && lane < 0; ++k)
            if (free(k, la, lb))
                lane = k;
        if (lane < 0)
            continue;                                      // no room at this zoom: zoom in for more
        lanes[size_t(lane)].push_back({la, lb});
        const double top = t.area.top() + 2 + lane * lh;
        QColor c = colour(*b);
        if (b->sharp()) {
            QColor lc = c;
            lc.setAlpha(90);
            p.setPen(QPen(lc, 1, Qt::DotLine));
            p.drawLine(QPointF(a, top + lh), QPointF(a, t.area.bottom()));
            QRectF r(la, top, lb - la, lh - 1);
            QColor fill = pal.legendBackground;
            p.fillRect(r, fill);
            p.setPen(c);
            p.drawText(r, Qt::AlignCenter, name);
            m_hits.push_back({r, b});
        } else {
            QRectF bar(ba, top, bb - ba, lh - 1);
            QColor fill = c;
            fill.setAlpha(45);
            QColor edge = c;
            edge.setAlpha(150);
            p.fillRect(bar, fill);
            p.setPen(QPen(edge, 1));
            p.drawRect(bar);
            p.setPen(pal.text);
            const QRectF textRect(ba + 3, top, std::max(bb - ba, tw) - 3, lh - 1);
            p.drawText(textRect, Qt::AlignLeft | Qt::AlignVCenter, name);
            m_hits.push_back({QRectF(la, top, lb - la, lh - 1), b});
        }
    }
    p.restore();
}

}  // namespace lr::gui
