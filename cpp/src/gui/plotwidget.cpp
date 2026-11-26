#include "plotwidget.h"

#include <QBuffer>
#include <QFile>
#include <QFontMetricsF>
#include <QMouseEvent>
#include <QPaintEvent>
#include <QPainter>
#include <QPainterPath>
#include <QSvgGenerator>
#include <QWheelEvent>

#include <algorithm>
#include <cmath>

namespace lr::gui {

namespace {

constexpr double kTickLen = 5;
constexpr double kMinorLen = 3;
constexpr double kGrab = 5;               // pixels around draggable lines/edges

bool finite(double v) { return std::isfinite(v); }

double clampPx(double v) { return std::clamp(v, -1e6, 1e6); }   // raster engines dislike huge coordinates

QColor withAlpha(QColor c, int a) {
    c.setAlpha(a);
    return c;
}

void drawMarker(QPainter& p, const QPointF& c, Marker m, double s) {
    const double r = s / 2;
    switch (m) {
    case Marker::None: break;
    case Marker::Circle: p.drawEllipse(c, r, r); break;
    case Marker::Square: p.drawRect(QRectF(c.x() - r, c.y() - r, s, s)); break;
    case Marker::Diamond:
        p.drawPolygon(QPolygonF({{c.x(), c.y() - r}, {c.x() + r, c.y()}, {c.x(), c.y() + r}, {c.x() - r, c.y()}}));
        break;
    case Marker::Triangle:
        p.drawPolygon(QPolygonF({{c.x(), c.y() - r}, {c.x() + r, c.y() + r * 0.8}, {c.x() - r, c.y() + r * 0.8}}));
        break;
    case Marker::Cross:
        p.drawLine(QPointF(c.x() - r, c.y() - r), QPointF(c.x() + r, c.y() + r));
        p.drawLine(QPointF(c.x() - r, c.y() + r), QPointF(c.x() + r, c.y() - r));
        break;
    case Marker::Plus:
        p.drawLine(QPointF(c.x() - r, c.y()), QPointF(c.x() + r, c.y()));
        p.drawLine(QPointF(c.x(), c.y() - r), QPointF(c.x(), c.y() + r));
        break;
    }
}

// colour-map anchors (matplotlib, sampled at 9 points)
const QColor* mapTable(ColorMap m) {
    static const QColor viridis[9] = {QColor("#440154"), QColor("#472d7b"), QColor("#3b528b"),
                                      QColor("#2c728e"), QColor("#21918c"), QColor("#28ae80"),
                                      QColor("#5ec962"), QColor("#addc30"), QColor("#fde725")};
    static const QColor inferno[9] = {QColor("#000004"), QColor("#1b0c41"), QColor("#4a0c6b"),
                                      QColor("#781c6d"), QColor("#a52c60"), QColor("#cf4446"),
                                      QColor("#ed6925"), QColor("#fb9b06"), QColor("#fcffa4")};
    static const QColor grey[9] = {QColor("#000000"), QColor("#202020"), QColor("#404040"),
                                   QColor("#606060"), QColor("#808080"), QColor("#9f9f9f"),
                                   QColor("#bfbfbf"), QColor("#dfdfdf"), QColor("#ffffff")};
    switch (m) {
    case ColorMap::Inferno: return inferno;
    case ColorMap::Grey: return grey;
    case ColorMap::Viridis: break;
    }
    return viridis;
}

const std::vector<QRgb>& lut(ColorMap m) {
    static std::vector<QRgb> tables[3];
    auto& t = tables[static_cast<int>(m)];
    if (t.empty()) {
        t.resize(256);
        for (int i = 0; i < 256; ++i)
            t[std::size_t(i)] = colorMapColor(m, i / 255.0).rgba();
    }
    return t;
}

}  // namespace

// ============================================================================================ palette
QColor PlotPalette::seriesColor(int k) const {
    if (series.empty())
        return main;
    const int n = int(series.size());
    return series[std::size_t(((k % n) + n) % n)];
}

bool PlotPalette::isDark() const { return plotArea.lightnessF() < 0.5; }

PlotPalette PlotPalette::light() {
    PlotPalette p;
    p.background = QColor("#ffffff");
    p.plotArea = QColor("#ffffff");
    p.axis = QColor("#9aa1ab");
    p.text = QColor("#5b6270");
    p.grid = QColor(40, 50, 65, 34);
    p.label = QColor("#3a3f47");
    p.main = QColor("#1f4e79");
    p.baseline = QColor("#c8892a");
    p.previous = QColor(31, 78, 121);
    p.reference = QColor("#999999");
    p.region = QColor(31, 111, 104, 56);
    p.accent = QColor("#1f6f68");
    p.legendBackground = QColor(255, 255, 255, 225);
    p.series = {QColor("#1f4e79"), QColor("#b5543c"), QColor("#3c8d6e"), QColor("#c8892a"), QColor("#6a4c93"),
                QColor("#2a7f9e"), QColor("#9c3d5a"), QColor("#7a7a2a"), QColor("#555b63")};
    return p;
}

PlotPalette PlotPalette::dark() {
    PlotPalette p;
    p.background = QColor("#141a24");
    p.plotArea = QColor("#182130");
    p.axis = QColor("#5d6779");
    p.text = QColor("#a9b3c3");
    p.grid = QColor(255, 255, 255, 22);
    p.label = QColor("#dde3ec");
    p.main = QColor("#7fb2e5");
    p.baseline = QColor("#e2a447");
    p.previous = QColor(127, 178, 229);
    p.reference = QColor("#7d8798");
    p.region = QColor(47, 179, 162, 60);
    p.accent = QColor("#3cc0ad");
    p.legendBackground = QColor(20, 26, 36, 225);
    p.series = {QColor("#7fb2e5"), QColor("#e9805f"), QColor("#5fc495"), QColor("#e2a447"), QColor("#ad92de"),
                QColor("#4fbadf"), QColor("#de6f93"), QColor("#c9c95f"), QColor("#a3abb8")};
    return p;
}

static PlotPalette& defaultPaletteRef() {
    static PlotPalette p = PlotPalette::light();
    return p;
}
const PlotPalette& PlotPalette::defaultPalette() { return defaultPaletteRef(); }
void PlotPalette::setDefaultPalette(const PlotPalette& p) { defaultPaletteRef() = p; }

// ============================================================================================ helpers
void Bounds::add(double v) {
    if (!finite(v))
        return;
    lo = std::min(lo, v);
    hi = std::max(hi, v);
}

void Bounds::add(const Bounds& b) {
    if (b.valid()) {
        lo = std::min(lo, b.lo);
        hi = std::max(hi, b.hi);
    }
}

namespace plot {

double niceStep(double span, int maxTicks) {
    span = std::abs(span);
    if (!(span > 0) || !finite(span))
        return 1;
    const double raw = span / std::max(1, maxTicks);
    const double mag = std::pow(10.0, std::floor(std::log10(raw)));
    for (double m : {1.0, 2.0, 2.5, 5.0, 10.0})
        if (m * mag >= raw * (1 - 1e-9))
            return m * mag;
    return 10 * mag;
}

Ticks niceTicks(double lo, double hi, int maxTicks) {
    Ticks t;
    if (lo > hi)
        std::swap(lo, hi);
    if (!finite(lo) || !finite(hi) || !(hi > lo))
        return t;
    t.step = niceStep(hi - lo, std::max(1, maxTicks));
    const double l10 = std::log10(t.step);
    const double mant = t.step / std::pow(10.0, std::floor(l10 + 1e-9));
    t.decimals = std::max(0, -int(std::floor(l10 + 1e-9)) + (std::abs(mant - 2.5) < 1e-6 ? 1 : 0));
    const double eps = t.step * 1e-9;
    const double first = std::ceil(lo / t.step - 1e-9) * t.step;
    for (int i = 0; i < 1000; ++i) {
        double v = first + i * t.step;
        if (v > hi + eps)
            break;
        if (std::abs(v) < eps)
            v = 0;
        t.major.push_back(v);
    }
    const int sub = std::abs(mant - 2.0) < 1e-6 ? 4 : 5;
    const double ms = t.step / sub;
    const double mfirst = std::ceil(lo / ms - 1e-9) * ms;
    for (int i = 0; i < 5000; ++i) {
        const double v = mfirst + i * ms;
        if (v > hi + eps)
            break;
        const double k = v / t.step;
        if (std::abs(k - std::round(k)) > 1e-6)
            t.minor.push_back(v);
    }
    return t;
}

QString superscript(int n) {
    static const QString digits = QStringLiteral("⁰¹²³⁴⁵⁶⁷⁸⁹");
    QString s = n < 0 ? QStringLiteral("⁻") : QString();
    const QString num = QString::number(std::abs(n));
    for (QChar c : num)
        s += digits[c.digitValue()];
    return s;
}

QString formatTick(double v, const Ticks& t) {
    if (t.step > 0 && std::abs(v) < t.step * 1e-9)
        v = 0;
    double maxAbs = 0;
    for (double m : t.major)
        maxAbs = std::max(maxAbs, std::abs(m));
    QString s;
    if (maxAbs >= 1e6 || (t.step > 0 && t.step < 1e-4)) {          // scientific: mantissa × 10^e
        if (v == 0)
            return QStringLiteral("0");
        const int e = int(std::floor(std::log10(maxAbs > 0 ? maxAbs : t.step)));
        const double scale = std::pow(10.0, e);
        const int dec = std::max(0, -int(std::floor(std::log10(t.step / scale) + 1e-9)) + 1);
        s = QString::number(v / scale, 'f', std::min(dec, 6));
        if (s.contains('.')) {                                       // drop trailing zeros
            while (s.endsWith('0'))
                s.chop(1);
            if (s.endsWith('.'))
                s.chop(1);
        }
        s += QStringLiteral("×10") + superscript(e);
    } else {
        s = QString::number(v, 'f', t.decimals);
    }
    if (s.startsWith('-'))
        s.replace(0, 1, QChar(0x2212));
    return s;
}

Range autoYRange(double lo, double hi, double headroom, double padding) {
    // a single value (one point, or a flat line): a range around it of 5 % of its size, not ±1e-9
    if (!(hi - lo > 1e-9 * std::max(std::abs(lo), std::abs(hi)))) {
        const double h = std::max(std::abs(hi) * 0.05, 1.0);
        lo -= h / 2;
        hi += h / 2;
    }
    const double top = hi + headroom * (hi - lo);
    double span = top - lo;
    if (!(span > 0))
        span = 1;
    return {lo - padding * span, top + padding * span};
}

std::vector<int> staggerLevels(const std::vector<double>& positions, double minGap) {
    std::vector<int> out;
    out.reserve(positions.size());
    int level = 0;
    for (std::size_t i = 0; i < positions.size(); ++i) {
        level = (i > 0 && positions[i] - positions[i - 1] < minGap) ? level + 1 : 0;
        out.push_back(level);
    }
    return out;
}

std::vector<QPointF> decimateMinMax(const double* x, const double* y, std::size_t i0, std::size_t i1,
                                    double x0, double x1, int columns) {
    std::vector<QPointF> out;
    if (i1 <= i0 || columns <= 0 || x1 == x0)
        return out;
    out.reserve(std::size_t(columns) * 4);
    const double scale = columns / (x1 - x0);
    long cur = std::numeric_limits<long>::min();
    std::size_t first = 0, last = 0, mn = 0, mx = 0;
    auto flush = [&] {
        if (cur == std::numeric_limits<long>::min())
            return;
        std::size_t idx[4] = {first, mn, mx, last};
        std::sort(idx, idx + 4);
        for (int k = 0; k < 4; ++k)
            if (k == 0 || idx[k] != idx[k - 1])
                out.emplace_back(x[idx[k]], y[idx[k]]);
    };
    for (std::size_t i = i0; i < i1; ++i) {
        if (!finite(y[i]) || !finite(x[i]))
            continue;
        const long c = std::clamp(long(std::floor((x[i] - x0) * scale)), 0L, long(columns - 1));
        if (c != cur) {
            flush();
            cur = c;
            first = last = mn = mx = i;
        } else {
            last = i;
            if (y[i] < y[mn])
                mn = i;
            if (y[i] > y[mx])
                mx = i;
        }
    }
    flush();
    return out;
}

}  // namespace plot

// ============================================================================================ transform
double PlotTransform::px(double vx) const {
    double f = (vx - x.lo) / x.span();
    if (invertX)
        f = 1 - f;
    return area.left() + f * area.width();
}

double PlotTransform::py(double vy) const { return area.bottom() - (vy - y.lo) / y.span() * area.height(); }

double PlotTransform::dataX(double pxv) const {
    double f = (pxv - area.left()) / area.width();
    if (invertX)
        f = 1 - f;
    return x.lo + f * x.span();
}

double PlotTransform::dataY(double pyv) const { return y.lo + (area.bottom() - pyv) / area.height() * y.span(); }

// ============================================================================================ PlotItem
PlotItem::PlotItem(PlotWidget* plot) : QObject(plot), m_plot(plot) {
    if (m_plot)
        m_plot->registerItem(this);
}

PlotItem::~PlotItem() {
    if (m_plot)
        m_plot->unregisterItem(this);
}

PlotWidget* PlotItem::plot() const { return m_plot; }

void PlotItem::setVisible(bool on) {
    if (on != m_visible) {
        m_visible = on;
        changed(true);
    }
}

void PlotItem::setZValue(double z) {
    m_z = z;
    changed();
}

void PlotItem::setName(const QString& name) {
    m_name = name;
    changed();
}

void PlotItem::setAutoScale(bool on) {
    m_autoScale = on;
    changed(true);
}

Bounds PlotItem::yBounds(double, double) const { return {}; }

void PlotItem::changed(bool boundsChanged) {
    if (m_plot)
        m_plot->itemChanged(boundsChanged);
}

// ============================================================================================ Curve
void Curve::setData(Vec x, Vec y) {
    m_x = std::move(x);
    m_y = std::move(y);
    const std::size_t n = size();
    bool asc = true, desc = true;
    for (std::size_t i = 1; i < n && (asc || desc); ++i) {
        if (m_x[i] < m_x[i - 1])
            asc = false;
        if (m_x[i] > m_x[i - 1])
            desc = false;
    }
    m_order = n < 2 ? 1 : (asc ? 1 : (desc ? -1 : 0));
    changed(true);
}

void Curve::clearData() { setData({}, {}); }

void Curve::setPen(const QPen& pen) {
    m_pen = pen;
    changed();
}

void Curve::setColor(const QColor& c) {
    m_pen.setColor(c);
    changed();
}

void Curve::setWidth(double w) {
    m_pen.setWidthF(w);
    changed();
}

void Curve::setStyle(Qt::PenStyle s) {
    m_pen.setStyle(s);
    changed();
}

void Curve::setOpacity(double o) {
    m_opacity = std::clamp(o, 0.0, 1.0);
    changed();
}

void Curve::setLineVisible(bool on) {
    m_line = on;
    changed();
}

void Curve::setMarker(Marker m, double size, const QBrush& brush, const QPen& pen) {
    m_marker = m;
    m_markerSize = size;
    m_markerBrush = brush.style() == Qt::NoBrush ? QBrush(m_pen.color()) : brush;
    m_markerPen = pen;
    if ((m == Marker::Cross || m == Marker::Plus) && pen.style() == Qt::NoPen)
        m_markerPen = QPen(m_markerBrush.color(), 1.4);
    changed();
}

void Curve::setFill(const QBrush& brush, double level) {
    m_fill = true;
    m_fillBrush = brush;
    m_fillLevel = level;
    changed(true);
}

void Curve::clearFill() {
    m_fill = false;
    changed(true);
}

void Curve::setDecimation(bool on) {
    m_decimate = on;
    changed();
}

std::pair<std::size_t, std::size_t> Curve::visibleIndices(double x0, double x1) const {
    const std::size_t n = size();
    if (m_order == 0)
        return {0, n};
    auto b = m_x.begin(), e = m_x.begin() + std::ptrdiff_t(n);
    if (m_order > 0)
        return {std::size_t(std::lower_bound(b, e, x0) - b), std::size_t(std::upper_bound(b, e, x1) - b)};
    return {std::size_t(std::lower_bound(b, e, x1, std::greater<double>()) - b),
            std::size_t(std::upper_bound(b, e, x0, std::greater<double>()) - b)};
}

Bounds Curve::xBounds() const {
    Bounds b;
    const std::size_t n = size();
    if (n == 0)
        return b;
    if (m_order != 0) {
        b.add(m_x.front());
        b.add(m_x[n - 1]);
        if (b.valid())
            return b;
    }
    for (std::size_t i = 0; i < n; ++i)
        b.add(m_x[i]);
    return b;
}

Bounds Curve::yBounds(double x0, double x1) const {
    Bounds b;
    auto [a, e] = visibleIndices(x0, x1);
    for (std::size_t i = a; i < e; ++i)
        if (m_x[i] >= x0 && m_x[i] <= x1)
            b.add(m_y[i]);
    if (m_fill && b.valid())
        b.add(m_fillLevel);
    return b;
}

void Curve::paint(QPainter& p, const PlotTransform& t) {
    const std::size_t n = size();
    if (n == 0)
        return;
    p.setOpacity(p.opacity() * m_opacity);
    auto [a, b] = visibleIndices(t.x.lo, t.x.hi);
    if (m_order != 0) {                       // one neighbour on each side so the line reaches the edge
        if (a > 0)
            --a;
        if (b < n)
            ++b;
    }
    if (a >= b)
        return;
    const int columns = std::max(1, int(t.area.width()));
    const bool decimate = m_decimate && m_order != 0 && (b - a) > std::size_t(3 * columns);
    auto mapPt = [&](double x, double y) { return QPointF(clampPx(t.px(x)), clampPx(t.py(y))); };

    std::vector<QPolygonF> segs;
    if (decimate) {
        QPolygonF poly;
        std::size_t c0 = a, c1 = b;               // keep the off-screen neighbours as they are
        if (c0 < c1 && (t.x.lo > m_x[c0] || t.x.hi < m_x[c0])) {
            if (finite(m_y[c0]))
                poly << mapPt(m_x[c0], m_y[c0]);
            ++c0;
        }
        const bool lastOutside = c1 > c0 && (m_x[c1 - 1] < t.x.lo || m_x[c1 - 1] > t.x.hi);
        if (lastOutside)
            --c1;
        for (const QPointF& d : plot::decimateMinMax(m_x.data(), m_y.data(), c0, c1, t.x.lo, t.x.hi, columns))
            poly << mapPt(d.x(), d.y());
        if (lastOutside && finite(m_y[c1]))
            poly << mapPt(m_x[c1], m_y[c1]);
        segs.push_back(std::move(poly));
    } else {
        QPolygonF poly;
        poly.reserve(qsizetype(b - a));
        for (std::size_t i = a; i < b; ++i) {
            if (!finite(m_x[i]) || !finite(m_y[i])) {
                if (!poly.isEmpty())
                    segs.push_back(std::move(poly));
                poly = QPolygonF();
                continue;
            }
            poly << mapPt(m_x[i], m_y[i]);
        }
        if (!poly.isEmpty())
            segs.push_back(std::move(poly));
    }

    if (m_fill) {
        const double base = clampPx(t.py(m_fillLevel));
        p.setPen(Qt::NoPen);
        p.setBrush(m_fillBrush);
        for (const QPolygonF& s : segs) {
            if (s.size() < 2)
                continue;
            QPolygonF poly = s;
            poly << QPointF(s.last().x(), base) << QPointF(s.first().x(), base);
            p.drawPolygon(poly);
        }
    }
    if (m_line && m_pen.style() != Qt::NoPen) {
        QPen pen = m_pen;
        pen.setJoinStyle(Qt::RoundJoin);
        pen.setCapStyle(Qt::FlatCap);
        p.setPen(pen);
        p.setBrush(Qt::NoBrush);
        for (const QPolygonF& s : segs)
            p.drawPolyline(s);
    }
    if (m_marker != Marker::None && !decimate) {
        p.setPen(m_markerPen);
        p.setBrush(m_markerBrush);
        for (std::size_t i = a; i < b; ++i)
            if (finite(m_x[i]) && finite(m_y[i]))
                drawMarker(p, mapPt(m_x[i], m_y[i]), m_marker, m_markerSize);
    }
}

void Curve::paintLegendSymbol(QPainter& p, const QRectF& r) const {
    p.setOpacity(p.opacity() * m_opacity);
    if (m_fill) {
        p.fillRect(QRectF(r.left(), r.center().y() - 3, r.width(), 6), m_fillBrush);
    }
    if (m_line && m_pen.style() != Qt::NoPen) {
        p.setPen(m_pen);
        p.drawLine(QPointF(r.left(), r.center().y()), QPointF(r.right(), r.center().y()));
    }
    if (m_marker != Marker::None) {
        p.setPen(m_markerPen);
        p.setBrush(m_markerBrush);
        drawMarker(p, r.center(), m_marker, std::min(m_markerSize, r.height() - 2));
    }
}

// ============================================================================================ ErrorBars
void ErrorBars::setData(Vec x, Vec y, Vec err) {
    m_x = std::move(x);
    m_y = std::move(y);
    m_err = std::move(err);
    changed(true);
}

void ErrorBars::setPen(const QPen& pen) {
    m_pen = pen;
    m_hasPen = true;
    changed();
}

void ErrorBars::setCapWidth(double px) {
    m_cap = std::max(0.0, px);
    changed();
}

Bounds ErrorBars::xBounds() const {
    Bounds b;
    for (std::size_t i = 0; i < size(); ++i)
        if (finite(m_y[i]) && finite(m_err[i]) && m_err[i] > 0)
            b.add(m_x[i]);
    return b;
}

Bounds ErrorBars::yBounds(double x0, double x1) const {
    Bounds b;
    for (std::size_t i = 0; i < size(); ++i)
        if (m_x[i] >= x0 && m_x[i] <= x1 && finite(m_y[i]) && finite(m_err[i]) && m_err[i] > 0) {
            b.add(m_y[i] - m_err[i]);
            b.add(m_y[i] + m_err[i]);
        }
    return b;
}

void ErrorBars::paint(QPainter& p, const PlotTransform& t) {
    const PlotWidget* pw = plot();
    QPen pen = m_hasPen ? m_pen : QPen(pw ? pw->plotPalette().main : QColor(Qt::black), 1.0);
    pen.setCapStyle(Qt::FlatCap);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    const double half = m_cap / 2;
    for (std::size_t i = 0; i < size(); ++i) {
        const double x = m_x[i], y = m_y[i], e = m_err[i];
        if (!finite(x) || !finite(y) || !finite(e) || e <= 0 || x < t.x.lo || x > t.x.hi)
            continue;
        const double px = t.px(x), lo = clampPx(t.py(y - e)), hi = clampPx(t.py(y + e));
        p.drawLine(QPointF(px, lo), QPointF(px, hi));
        if (half > 0) {
            p.drawLine(QPointF(px - half, lo), QPointF(px + half, lo));
            p.drawLine(QPointF(px - half, hi), QPointF(px + half, hi));
        }
    }
}

// ============================================================================================ TextItem
void TextItem::setText(const QString& text) {
    m_text = text;
    changed();
}

void TextItem::setPos(double x, double y) {
    m_pos = {x, y};
    changed();
}

void TextItem::setAnchor(const QPointF& anchor) {
    m_anchor = anchor;
    changed();
}

void TextItem::setColor(const QColor& c) {
    m_color = c;
    changed();
}

void TextItem::setFont(const QFont& f) {
    m_font = f;
    m_hasFont = true;
    changed();
}

void TextItem::setBackground(const QBrush& b) {
    m_bg = b;
    changed();
}

QRectF TextItem::boundingRect(const PlotTransform& t, const QFont& base) const {
    const QFontMetricsF fm(m_hasFont ? m_font : base);
    const QRectF br = fm.boundingRect(QRectF(0, 0, 1e6, 1e6), Qt::AlignLeft | Qt::AlignTop, m_text);
    const QSizeF sz(br.width() + 4, br.height());
    const QPointF pt = t.map(m_pos);
    return {QPointF(pt.x() - m_anchor.x() * sz.width(), pt.y() - m_anchor.y() * sz.height()), sz};
}

void TextItem::paint(QPainter& p, const PlotTransform& t) {
    if (m_text.isEmpty() || !finite(m_pos.x()) || !finite(m_pos.y()))
        return;
    if (m_hasFont)
        p.setFont(m_font);
    const QFontMetricsF fm = p.fontMetrics();
    const QRectF br = fm.boundingRect(QRectF(0, 0, 1e6, 1e6), Qt::AlignLeft | Qt::AlignTop, m_text);
    const QSizeF sz(br.width() + 4, br.height());
    const QPointF pt = t.map(m_pos);
    const QRectF r(QPointF(pt.x() - m_anchor.x() * sz.width(), pt.y() - m_anchor.y() * sz.height()), sz);
    if (m_bg.style() != Qt::NoBrush) {
        p.setPen(Qt::NoPen);
        p.setBrush(m_bg);
        p.drawRoundedRect(r.adjusted(-2, -1, 2, 1), 3, 3);
    }
    const PlotWidget* pw = plot();
    p.setPen(m_color.isValid() ? m_color : (pw ? pw->plotPalette().label : QColor(Qt::black)));
    p.drawText(r, Qt::AlignCenter, m_text);
}

// ============================================================================================ PeakLabels
void PeakLabels::setLabels(std::vector<PeakLabel> labels) {
    m_labels = std::move(labels);
    std::stable_sort(m_labels.begin(), m_labels.end(),
                     [](const PeakLabel& a, const PeakLabel& b) { return a.x < b.x; });
    changed();
}

void PeakLabels::setPeaks(const Vec& positions, const Vec& x, const Vec& y, int decimals) {
    std::vector<PeakLabel> out;
    const std::size_t n = std::min(x.size(), y.size());
    if (n == 0) {
        setLabels({});
        return;
    }
    const bool asc = std::is_sorted(x.begin(), x.begin() + std::ptrdiff_t(n));
    for (double pos : positions) {
        std::size_t best = 0;
        if (asc) {
            const auto it = std::lower_bound(x.begin(), x.begin() + std::ptrdiff_t(n), pos);
            best = std::min(n - 1, std::size_t(it - x.begin()));
            if (best > 0 && std::abs(x[best - 1] - pos) <= std::abs(x[best] - pos))
                --best;
        } else {
            for (std::size_t i = 1; i < n; ++i)
                if (std::abs(x[i] - pos) < std::abs(x[best] - pos))
                    best = i;
        }
        out.push_back({pos, y[best], QString::number(pos, 'f', decimals)});
    }
    setLabels(std::move(out));
}

void PeakLabels::setMinGapFraction(double f) {
    m_gap = f;
    changed();
}

void PeakLabels::setColor(const QColor& c) {
    m_color = c;
    changed();
}

void PeakLabels::setFont(const QFont& f) {
    m_font = f;
    m_hasFont = true;
    changed();
}

std::vector<int> PeakLabels::levels(double xSpan) const {
    std::vector<double> xs;
    xs.reserve(m_labels.size());
    for (const PeakLabel& l : m_labels)
        xs.push_back(l.x);
    return plot::staggerLevels(xs, std::abs(xSpan) * m_gap);
}

std::vector<QRectF> PeakLabels::layoutLabels(const PlotTransform& t, const QFontMetricsF& fm) const {
    // Python: label k of a run of close peaks is raised by 1.1 text heights per level (level % 3).
    // Generalised so that a raised label always clears the previous one, also when its own peak is
    // lower: its bottom is at most 0.1 h above the previous label's top. Equal peaks: as Python.
    std::vector<QRectF> out(m_labels.size());
    const std::vector<int> lv = levels(t.x.span());
    const double h = fm.height();
    QRectF prev;
    for (std::size_t i = 0; i < m_labels.size(); ++i) {
        const PeakLabel& l = m_labels[i];
        if (!finite(l.y) || !finite(l.x))
            continue;
        const QPointF pt = t.map(l.x, l.y);
        const double w = fm.horizontalAdvance(l.text) + 4;
        double bottom = pt.y() - 0.2 * h;
        if (lv[i] % 3 != 0 && !prev.isNull())
            bottom = std::min(bottom, prev.top() - 0.1 * h);
        out[i] = QRectF(pt.x() - 0.5 * w, bottom - h, w, h);
        prev = out[i];
    }
    return out;
}

void PeakLabels::paint(QPainter& p, const PlotTransform& t) {
    if (m_labels.empty())
        return;
    if (m_hasFont)
        p.setFont(m_font);
    const PlotWidget* pw = plot();
    p.setPen(m_color.isValid() ? m_color : (pw ? pw->plotPalette().label : QColor(Qt::black)));
    const std::vector<QRectF> rects = layoutLabels(t, p.fontMetrics());
    for (std::size_t i = 0; i < m_labels.size(); ++i)
        if (!rects[i].isNull() && m_labels[i].x >= t.x.lo && m_labels[i].x <= t.x.hi)
            p.drawText(rects[i], Qt::AlignCenter, m_labels[i].text);
}

// ============================================================================================ InfiniteLine
InfiniteLine::InfiniteLine(PlotWidget* plot, Orientation o) : PlotItem(plot), m_orient(o) { setZValue(5); }

void InfiniteLine::setValue(double v) {
    v = std::clamp(v, m_lo, m_hi);
    if (v == m_value)
        return;
    m_value = v;
    changed();
    emit moved(m_value);
}

void InfiniteLine::setPen(const QPen& pen) {
    m_pen = pen;
    m_hasPen = true;
    changed();
}

void InfiniteLine::setMovable(bool on) {
    m_movable = on;
    changed();
}

void InfiniteLine::setBounds(double lo, double hi) {
    m_lo = std::min(lo, hi);
    m_hi = std::max(lo, hi);
    setValue(m_value);
}

void InfiniteLine::setLabel(const QString& text) {
    m_label = text;
    changed();
}

void InfiniteLine::paint(QPainter& p, const PlotTransform& t) {
    const PlotWidget* pw = plot();
    QPen pen = m_pen;
    if (!m_hasPen && pw)
        pen = QPen(m_movable ? pw->plotPalette().accent : pw->plotPalette().reference, m_movable ? 1.5 : 1.0,
                   m_movable ? Qt::SolidLine : Qt::DashLine);
    p.setPen(pen);
    const QRectF& a = t.area;
    QRectF textRect;
    if (m_orient == Vertical) {
        const double x = t.px(m_value);
        if (x < a.left() - 1 || x > a.right() + 1)
            return;
        p.drawLine(QPointF(x, a.top()), QPointF(x, a.bottom()));
        if (m_movable) {                               // small grip at the top
            p.setBrush(pen.color());
            p.drawPolygon(QPolygonF({{x - 5, a.top()}, {x + 5, a.top()}, {x, a.top() + 6}}));
        }
        if (!m_label.isEmpty()) {
            const QFontMetricsF fm = p.fontMetrics();
            textRect = QRectF(x + 4, a.top() + 4, fm.horizontalAdvance(m_label) + 6, fm.height());
            if (textRect.right() > a.right())
                textRect.moveRight(x - 4);
        }
    } else {
        const double y = t.py(m_value);
        if (y < a.top() - 1 || y > a.bottom() + 1)
            return;
        p.drawLine(QPointF(a.left(), y), QPointF(a.right(), y));
        if (!m_label.isEmpty()) {
            const QFontMetricsF fm = p.fontMetrics();
            textRect = QRectF(a.right() - fm.horizontalAdvance(m_label) - 10, y - fm.height() - 2,
                              fm.horizontalAdvance(m_label) + 6, fm.height());
        }
    }
    if (!textRect.isNull() && pw) {
        p.setPen(Qt::NoPen);
        p.setBrush(pw->plotPalette().legendBackground);
        p.drawRoundedRect(textRect, 3, 3);
        p.setPen(pen.color());
        p.drawText(textRect, Qt::AlignCenter, m_label);
    }
}

int InfiniteLine::hitTest(const QPointF& pos, const PlotTransform& t) const {
    if (!m_movable || !t.area.adjusted(-kGrab, -kGrab, kGrab, kGrab).contains(pos))
        return -1;
    const double d = m_orient == Vertical ? std::abs(pos.x() - t.px(m_value)) : std::abs(pos.y() - t.py(m_value));
    return d <= kGrab ? 0 : -1;
}

Qt::CursorShape InfiniteLine::cursorFor(int) const {
    return m_orient == Vertical ? Qt::SizeHorCursor : Qt::SizeVerCursor;
}

void InfiniteLine::dragTo(int, const QPointF& data) { setValue(m_orient == Vertical ? data.x() : data.y()); }

void InfiniteLine::dragEnd(int) { emit moveFinished(m_value); }

// ============================================================================================ RegionItem
void RegionItem::setRegion(double lo, double hi) {
    if (lo > hi)
        std::swap(lo, hi);
    if (lo == m_lo && hi == m_hi)
        return;
    m_lo = lo;
    m_hi = hi;
    changed();
    emit regionChanged(m_lo, m_hi);
}

void RegionItem::setBrush(const QBrush& b) {
    m_brush = b;
    m_hasBrush = true;
    changed();
}

void RegionItem::setEdgePen(const QPen& pen) {
    m_pen = pen;
    m_hasPen = true;
    changed();
}

void RegionItem::setMovable(bool on) {
    m_movable = on;
    changed();
}

void RegionItem::paint(QPainter& p, const PlotTransform& t) {
    const PlotWidget* pw = plot();
    double a = t.px(m_lo), b = t.px(m_hi);
    if (a > b)
        std::swap(a, b);
    a = std::max(a, t.area.left() - 2);
    b = std::min(b, t.area.right() + 2);
    if (b < a)
        return;
    const QRectF r(QPointF(a, t.area.top()), QPointF(b, t.area.bottom()));
    p.fillRect(r, m_hasBrush ? m_brush : QBrush(pw ? pw->plotPalette().region : QColor(0, 0, 0, 30)));
    QPen pen = m_hasPen ? m_pen : QPen(pw ? pw->plotPalette().accent : QColor(Qt::darkCyan), 1.2);
    p.setPen(pen);
    for (double v : {m_lo, m_hi}) {
        const double x = t.px(v);
        if (x >= t.area.left() - 1 && x <= t.area.right() + 1)
            p.drawLine(QPointF(x, t.area.top()), QPointF(x, t.area.bottom()));
    }
}

int RegionItem::hitTest(const QPointF& pos, const PlotTransform& t) const {
    if (!m_movable || !t.area.contains(pos))
        return -1;
    const double dl = std::abs(pos.x() - t.px(m_lo)), dh = std::abs(pos.x() - t.px(m_hi));
    if (std::min(dl, dh) <= kGrab)
        return dl <= dh ? 0 : 1;
    const double x = t.dataX(pos.x());
    return (x > m_lo && x < m_hi) ? 2 : -1;
}

Qt::CursorShape RegionItem::cursorFor(int part) const {
    return part == 2 ? Qt::SizeAllCursor : Qt::SizeHorCursor;
}

void RegionItem::dragStart(int, const QPointF& data) {
    m_grabX = data.x();
    m_grabLo = m_lo;
    m_grabHi = m_hi;
}

void RegionItem::dragTo(int part, const QPointF& data) {
    if (part == 0)
        setRegion(std::min(data.x(), m_hi), m_hi);
    else if (part == 1)
        setRegion(m_lo, std::max(data.x(), m_lo));
    else
        setRegion(m_grabLo + data.x() - m_grabX, m_grabHi + data.x() - m_grabX);
}

void RegionItem::dragEnd(int) { emit regionChangeFinished(m_lo, m_hi); }

// ============================================================================================ heat map
QColor colorMapColor(ColorMap map, double t) {
    if (!finite(t))
        return QColor(0, 0, 0, 0);
    t = std::clamp(t, 0.0, 1.0) * 8;
    const int i = std::min(7, int(t));
    const double f = t - i;
    const QColor* tab = mapTable(map);
    const QColor &a = tab[i], &b = tab[i + 1];
    return QColor::fromRgbF(float(a.redF() + f * (b.redF() - a.redF())),
                            float(a.greenF() + f * (b.greenF() - a.greenF())),
                            float(a.blueF() + f * (b.blueF() - a.blueF())));
}

void HeatMapItem::setData(int rows, int cols, Vec values) {
    if (rows < 0 || cols < 0 || values.size() < std::size_t(rows) * std::size_t(cols))
        rows = cols = 0;
    m_rows = rows;
    m_cols = cols;
    m_values = std::move(values);
    m_dirty = true;
    changed(true);
}

void HeatMapItem::setExtent(double x0, double x1, double y0, double y1) {
    m_x0 = x0;
    m_x1 = x1;
    m_y0 = y0;
    m_y1 = y1;
    changed(true);
}

void HeatMapItem::setColorMap(ColorMap m) {
    m_map = m;
    m_dirty = true;
    changed();
}

void HeatMapItem::setLevels(double lo, double hi) {
    m_autoLevels = false;
    m_levels = {std::min(lo, hi), std::max(lo, hi)};
    m_dirty = true;
    changed();
}

void HeatMapItem::setAutoLevels() {
    m_autoLevels = true;
    m_dirty = true;
    changed();
}

void HeatMapItem::setColorBarVisible(bool on) {
    m_colorBar = on;
    changed();
}

void HeatMapItem::setColorBarLabel(const QString& s) {
    m_colorBarLabel = s;
    changed();
}

const QImage& HeatMapItem::image() const {
    if (m_dirty)
        rebuild();
    return m_image;
}

void HeatMapItem::rebuild() const {
    m_dirty = false;
    if (m_rows == 0 || m_cols == 0) {
        m_image = QImage();
        return;
    }
    if (m_autoLevels) {
        Bounds b;
        for (std::size_t i = 0; i < std::size_t(m_rows) * std::size_t(m_cols); ++i)
            b.add(m_values[i]);
        const_cast<HeatMapItem*>(this)->m_levels = b.valid() ? Range{b.lo, b.hi} : Range{0, 1};
    }
    const double lo = m_levels.lo, span = m_levels.hi - m_levels.lo > 0 ? m_levels.hi - m_levels.lo : 1;
    const std::vector<QRgb>& tab = lut(m_map);
    m_image = QImage(m_cols, m_rows, QImage::Format_ARGB32);
    for (int r = 0; r < m_rows; ++r) {
        auto* line = reinterpret_cast<QRgb*>(m_image.scanLine(m_rows - 1 - r));
        const double* v = m_values.data() + std::size_t(r) * std::size_t(m_cols);
        for (int c = 0; c < m_cols; ++c)
            line[c] = finite(v[c]) ? tab[std::size_t(std::clamp(int((v[c] - lo) / span * 255 + 0.5), 0, 255))]
                                   : qRgba(0, 0, 0, 0);
    }
}

Bounds HeatMapItem::xBounds() const {
    Bounds b;
    if (m_rows && m_cols) {
        b.add(m_x0);
        b.add(m_x1);
    }
    return b;
}

Bounds HeatMapItem::yBounds(double, double) const {
    Bounds b;
    if (m_rows && m_cols) {
        b.add(m_y0);
        b.add(m_y1);
    }
    return b;
}

void HeatMapItem::paint(QPainter& p, const PlotTransform& t) {
    const QImage& img = image();
    if (img.isNull())
        return;
    // crop to the visible cells, then reduce (area-averaging) to the screen resolution: no aliasing of
    // narrow bands when 1000+ columns are shown in fewer pixels
    const double dx = (m_x1 - m_x0) / m_cols, dy = (m_y1 - m_y0) / m_rows;
    if (dx == 0 || dy == 0)
        return;
    auto cellRange = [](double v0, double d, double lo, double hi, int n) {
        const double a = (lo - v0) / d, b = (hi - v0) / d;
        int c0 = int(std::floor(std::min(a, b))), c1 = int(std::ceil(std::max(a, b)));
        return std::make_pair(std::clamp(c0, 0, n), std::clamp(c1, 0, n));
    };
    auto [c0, c1] = cellRange(m_x0, dx, t.x.lo, t.x.hi, m_cols);
    auto [r0, r1] = cellRange(m_y0, dy, t.y.lo, t.y.hi, m_rows);
    if (c1 <= c0 || r1 <= r0)
        return;
    QImage sub = img.copy(c0, m_rows - r1, c1 - c0, r1 - r0);
    const QPointF pa = t.map(m_x0 + c0 * dx, m_y0 + r1 * dy);   // image top-left
    const QPointF pb = t.map(m_x0 + c1 * dx, m_y0 + r0 * dy);   // image bottom-right
    const bool flipX = pb.x() < pa.x(), flipY = pb.y() < pa.y();
    if (flipX || flipY)
        sub = sub.mirrored(flipX, flipY);
    const QRectF target = QRectF(pa, pb).normalized();
    const qreal dpr = p.device() ? p.device()->devicePixelRatioF() : 1.0;
    const int tw = std::max(1, int(std::ceil(target.width() * dpr)));
    const int th = std::max(1, int(std::ceil(target.height() * dpr)));
    if (sub.width() > tw || sub.height() > th)
        sub = sub.scaled(std::min(sub.width(), tw), std::min(sub.height(), th), Qt::IgnoreAspectRatio,
                         Qt::SmoothTransformation);
    p.setRenderHint(QPainter::SmoothPixmapTransform, false);
    p.drawImage(target, sub);
}

// ============================================================================================ PlotWidget
struct PlotWidget::Layout {
    QRectF outer, area;
    Ticks xt, yt, ct;
    QRectF colorBar;
    double lineHeight = 12;
};

PlotWidget::PlotWidget(QWidget* parent) : QWidget(parent), m_pal(PlotPalette::defaultPalette()) {
    setMouseTracking(true);
    setAttribute(Qt::WA_OpaquePaintEvent);
    setToolTip(navigationHelp(m_wheelMode));
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
}

PlotWidget::~PlotWidget() {
    auto items = m_items;
    m_items.clear();
    for (PlotItem* it : items) {
        it->m_plot = nullptr;
        delete it;
    }
}

void PlotWidget::registerItem(PlotItem* it) {
    m_items.push_back(it);
    itemChanged(true);
}

void PlotWidget::unregisterItem(PlotItem* it) {
    m_items.erase(std::remove(m_items.begin(), m_items.end(), it), m_items.end());
    if (m_dragItem == it) {
        m_dragItem = nullptr;
        m_drag = Drag::None;
    }
    itemChanged(true);
}

void PlotWidget::itemChanged(bool boundsChanged) {
    if (boundsChanged && (m_autoX || m_autoY))
        m_viewDirty = true;
    update();
}

Curve* PlotWidget::addCurve(const QPen& pen, const QString& name) {
    auto* c = new Curve(this);
    c->setPen(pen == QPen() ? QPen(m_pal.main, 1.5) : pen);
    c->setName(name);
    return c;
}

Curve* PlotWidget::addCurve(Vec x, Vec y, const QPen& pen, const QString& name) {
    Curve* c = addCurve(pen, name);
    c->setData(std::move(x), std::move(y));
    return c;
}

Curve* PlotWidget::addScatter(Vec x, Vec y, const QColor& color, double size, Marker m) {
    Curve* c = addCurve(QPen(color, 1.0));
    c->setLineVisible(false);
    c->setMarker(m, size, QBrush(color));
    c->setData(std::move(x), std::move(y));
    return c;
}

ErrorBars* PlotWidget::addErrorBars(Vec x, Vec y, Vec err, const QPen& pen) {
    auto* e = new ErrorBars(this);
    if (pen.style() != Qt::NoPen && pen != QPen())
        e->setPen(pen);
    e->setData(std::move(x), std::move(y), std::move(err));
    return e;
}

TextItem* PlotWidget::addText(const QString& text, double x, double y, const QPointF& anchor) {
    auto* t = new TextItem(this);
    t->setText(text);
    t->setPos(x, y);
    t->setAnchor(anchor);
    return t;
}

PeakLabels* PlotWidget::addPeakLabels() { return new PeakLabels(this); }

InfiniteLine* PlotWidget::addVLine(double x, const QPen& pen, bool movable) {
    auto* l = new InfiniteLine(this, InfiniteLine::Vertical);
    if (pen != QPen())
        l->setPen(pen);
    l->setMovable(movable);
    l->setValue(x);
    return l;
}

InfiniteLine* PlotWidget::addHLine(double y, const QPen& pen, bool movable) {
    auto* l = new InfiniteLine(this, InfiniteLine::Horizontal);
    if (pen != QPen())
        l->setPen(pen);
    l->setMovable(movable);
    l->setValue(y);
    return l;
}

RegionItem* PlotWidget::addRegion(double lo, double hi) {
    auto* r = new RegionItem(this);
    r->setRegion(lo, hi);
    return r;
}

HeatMapItem* PlotWidget::addHeatMap() { return new HeatMapItem(this); }

void PlotWidget::removeItem(PlotItem* item) {
    if (item && std::find(m_items.begin(), m_items.end(), item) != m_items.end())
        delete item;
}

void PlotWidget::clear() {
    auto items = m_items;
    for (PlotItem* it : items)
        delete it;
}

void PlotWidget::setPlotPalette(const PlotPalette& p) {
    m_pal = p;
    m_customPalette = true;
    update();
}

void PlotWidget::setLabel(Axis axis, const QString& text) {
    (axis == Axis::Bottom ? m_xLabel : m_yLabel) = text;
    update();
}

QString PlotWidget::label(Axis axis) const { return axis == Axis::Bottom ? m_xLabel : m_yLabel; }

void PlotWidget::setGrid(bool x, bool y) {
    m_gridX = x;
    m_gridY = y;
    update();
}

void PlotWidget::setXInverted(bool on) {
    m_invertX = on;
    update();
}

void PlotWidget::setLegendVisible(bool on) {
    m_legend = on;
    update();
}

void PlotWidget::setHoverReadout(bool on) {
    m_readout = on;
    update();
}

void PlotWidget::setReadoutFormatter(std::function<QString(const QPointF&)> f) { m_readoutFmt = std::move(f); }

QString PlotWidget::navigationHelp(WheelMode mode) {
    if (mode == WheelMode::ZoomX)
        return QStringLiteral("<b>Wheel</b>: zoom along the spectrum<br><b>Ctrl+wheel</b>: zoom intensity "
                              "(baseline stays)<br><b>Drag</b>: pan<br><b>Shift+drag</b>: zoom to a rectangle<br>"
                              "<b>Double-click</b>: full view");
    return QStringLiteral("<b>Wheel</b>: zoom intensity (baseline stays)<br><b>Drag</b>: pan along the spectrum<br>"
                          "<b>Ctrl+wheel</b> or wheel on the x axis: zoom wavenumbers<br>"
                          "<b>Shift+drag</b>: zoom to a rectangle<br><b>Double-click</b>: full view");
}

// ---- view
static Range sane(double lo, double hi) {
    if (lo > hi)
        std::swap(lo, hi);
    const double minSpan = std::max(1e-12, 1e-9 * std::max(std::abs(lo), std::abs(hi)));
    if (hi - lo < minSpan) {
        const double c = (lo + hi) / 2;
        const double h = (lo == hi) ? std::max(0.5, std::abs(c) * 0.05) : minSpan / 2;
        lo = c - h;
        hi = c + h;
    }
    return {lo, hi};
}

void PlotWidget::applyX(double lo, double hi) {
    if (!finite(lo) || !finite(hi))
        return;
    const Range r = sane(lo, hi);
    if (r == m_x)
        return;
    m_x = r;
    update();
    emit xRangeChanged(m_x.lo, m_x.hi);
}

void PlotWidget::applyY(double lo, double hi) {
    if (!finite(lo) || !finite(hi))
        return;
    const Range r = sane(lo, hi);
    if (r == m_y)
        return;
    m_y = r;
    update();
    emit yRangeChanged(m_y.lo, m_y.hi);
}

Bounds PlotWidget::dataXBounds() const {
    Bounds b;
    for (PlotItem* it : m_items)
        if (it->isVisible() && it->autoScale())
            b.add(it->xBounds());
    return b;
}

void PlotWidget::ensureView() const {
    if (!m_viewDirty)
        return;
    m_viewDirty = false;
    auto* self = const_cast<PlotWidget*>(this);
    if (m_autoX) {
        const Bounds b = dataXBounds();
        if (b.valid())
            self->applyX(b.lo, b.hi);
    }
    if (m_autoY)
        self->rescaleY();
}

void PlotWidget::rescaleY() {
    if (!m_autoY)
        return;
    if (m_viewDirty && m_autoX) {
        m_viewDirty = false;
        const Bounds b = dataXBounds();
        if (b.valid())
            applyX(b.lo, b.hi);
    }
    m_viewDirty = false;
    Bounds curves, exact;
    for (PlotItem* it : m_items) {
        if (!it->isVisible() || !it->autoScale())
            continue;
        const Bounds b = it->yBounds(m_x.lo, m_x.hi);
        (it->yBoundsExact() ? exact : curves).add(b);
    }
    if (!curves.valid() && !exact.valid())
        return;
    Range r = curves.valid() ? plot::autoYRange(curves.lo, curves.hi, m_headroom) : Range{exact.lo, exact.hi};
    if (curves.valid() && exact.valid())
        r = {std::min(r.lo, exact.lo), std::max(r.hi, exact.hi)};
    applyY(r.lo, r.hi);
}

void PlotWidget::setXRange(double lo, double hi) {
    ensureView();
    m_autoX = false;
    applyX(lo, hi);
    if (m_autoY)
        rescaleY();
}

void PlotWidget::setYRange(double lo, double hi) {
    ensureView();
    applyY(lo, hi);
}

Range PlotWidget::xRange() const {
    ensureView();
    return m_x;
}

Range PlotWidget::yRange() const {
    ensureView();
    return m_y;
}

void PlotWidget::setHomeXRange(double lo, double hi) {
    m_home = sane(lo, hi);
    m_hasHome = true;
    if (m_autoX)
        setXRange(m_home.lo, m_home.hi);
}

void PlotWidget::clearHomeXRange() { m_hasHome = false; }

void PlotWidget::setAutoX(bool on) {
    m_autoX = on;
    if (on) {
        m_viewDirty = true;
        update();
    }
}

void PlotWidget::setAutoY(bool on) {
    m_autoY = on;
    if (on)
        rescaleY();
}

void PlotWidget::setHeadroom(double fraction) {
    m_headroom = fraction;
    rescaleY();
}

void PlotWidget::autoRange() {
    m_autoY = true;
    if (m_hasHome) {
        m_autoX = false;
        applyX(m_home.lo, m_home.hi);
    } else {
        m_autoX = true;
    }
    m_viewDirty = true;
    ensureView();
    rescaleY();
    update();
    emit autoRangeRequested();
}

Range PlotWidget::xLimit() const {
    Bounds b = dataXBounds();
    if (m_hasHome) {
        b.add(m_home.lo);
        b.add(m_home.hi);
    }
    return b.valid() ? Range{b.lo, b.hi} : Range{0, 0};
}

// Keeps [lo, hi] inside the data/home extent: never zoomed out beyond it, panned back inside, and not
// narrower than 1/2000 of it (so the spectrum can't be lost by zooming or panning).
static void clampToLimit(double& lo, double& hi, const Range& lim) {
    const double L = lim.hi - lim.lo;
    if (!(L > 0)) return;
    double w = std::max(hi - lo, L / 2000.0);
    if (w >= L) {
        lo = lim.lo;
        hi = lim.hi;
        return;
    }
    const double c = std::clamp(0.5 * (lo + hi), lim.lo + w / 2, lim.hi - w / 2);
    lo = c - w / 2;
    hi = c + w / 2;
}

void PlotWidget::zoomX(double factor, double centerX) {
    ensureView();
    if (!(factor > 0) || !finite(factor) || !finite(centerX))
        return;
    double lo = centerX - (centerX - m_x.lo) * factor, hi = centerX + (m_x.hi - centerX) * factor;
    clampToLimit(lo, hi, xLimit());
    m_autoY = true;                                    // intensity follows the visible x range
    setXRange(lo, hi);
}

void PlotWidget::scaleYTop(double factor) {
    ensureView();
    if (!(factor > 0) || !finite(factor))
        return;
    m_autoY = false;
    const double lo = m_y.lo;                          // the bottom stays; the top moves (magnifies small bands)
    double hi = lo + m_y.span() * factor;
    if (!(hi > lo)) hi = lo + 1e-9;
    applyY(lo, hi);
}

void PlotWidget::panPixels(const QPointF& delta) {
    const PlotTransform t = transform();
    if (t.area.width() <= 0 || t.area.height() <= 0)
        return;
    if (delta.x() != 0) {
        double dx = delta.x() / t.area.width() * t.x.span();
        if (t.invertX)
            dx = -dx;
        double lo = m_x.lo - dx, hi = m_x.hi - dx;
        clampToLimit(lo, hi, xLimit());
        setXRange(lo, hi);
    }
    if (m_panY && delta.y() != 0) {
        const double dy = delta.y() / t.area.height() * t.y.span();
        m_autoY = false;
        applyY(m_y.lo + dy, m_y.hi + dy);
    }
}

PlotTransform PlotWidget::transform() const {
    ensureView();
    const Layout L = layout(QRectF(rect()), font(), const_cast<PlotWidget*>(this));
    return {L.area, m_x, m_y, m_invertX};
}

QPointF PlotWidget::mapToData(const QPointF& widgetPos) const { return transform().unmap(widgetPos); }

QPointF PlotWidget::mapFromData(const QPointF& dataPos) const { return transform().map(dataPos); }

HeatMapItem* PlotWidget::colorBarItem() const {
    for (PlotItem* it : m_items)
        if (auto* h = dynamic_cast<HeatMapItem*>(it))
            if (h->isVisible() && h->colorBarVisible() && h->rows() && h->cols())
                return h;
    return nullptr;
}

std::vector<PlotItem*> PlotWidget::sortedItems() const {
    std::vector<PlotItem*> v = m_items;
    std::stable_sort(v.begin(), v.end(), [](PlotItem* a, PlotItem* b) { return a->zValue() < b->zValue(); });
    return v;
}

PlotWidget::Layout PlotWidget::layout(const QRectF& r, const QFont& font, QPaintDevice* dev) const {
    Layout L;
    L.outer = r;
    const QFontMetricsF fm(font, dev);
    const double lh = fm.height();
    L.lineHeight = lh;
    const double top = std::ceil(lh * 0.5 + 6);
    const double bottom = std::ceil(kTickLen + 3 + lh + (m_xLabel.isEmpty() ? 0 : lh + 4) + 4);
    const double plotH = std::max(10.0, r.height() - top - bottom);

    // y ticks decide the left margin
    L.yt = plot::niceTicks(m_y.lo, m_y.hi, std::max(2, int(plotH / (lh * 2.4))));
    double yw = 0;
    for (double v : L.yt.major)
        yw = std::max(yw, fm.horizontalAdvance(plot::formatTick(v, L.yt)));
    const double left = std::ceil(6 + (m_yLabel.isEmpty() ? 0 : lh + 6) + yw + kTickLen + 4);

    // colour bar at the right
    double right = 14;
    if (HeatMapItem* h = colorBarItem()) {
        h->image();                                          // levels are known after the image is built
        const Range lv = h->levels();
        L.ct = plot::niceTicks(lv.lo, lv.hi, std::max(2, int(plotH / (lh * 2.4))));
        double cw = 0;
        for (double v : L.ct.major)
            cw = std::max(cw, fm.horizontalAdvance(plot::formatTick(v, L.ct)));
        right = std::ceil(14 + 14 + kTickLen + 3 + cw + (h->colorBarLabel().isEmpty() ? 0 : lh + 6) + 8);
    }
    const double plotW = std::max(10.0, r.width() - left - right);
    L.area = QRectF(r.left() + left, r.top() + top, plotW, plotH);
    if (colorBarItem())
        L.colorBar = QRectF(L.area.right() + 14, L.area.top(), 14, L.area.height());

    // x ticks: as many as fit with a gap of ~2 label widths
    int nx = std::max(2, int(plotW / 90));
    for (int pass = 0; pass < 3; ++pass) {
        L.xt = plot::niceTicks(m_x.lo, m_x.hi, nx);
        double xw = 0;
        for (double v : L.xt.major)
            xw = std::max(xw, fm.horizontalAdvance(plot::formatTick(v, L.xt)));
        const int fit = std::max(2, int(plotW / (xw + 2.2 * lh)));
        if (int(L.xt.major.size()) <= fit + 1)
            break;
        nx = std::max(1, fit - 1);
    }
    return L;
}

// ---- painting
void PlotWidget::render(QPainter& p, const QRectF& target) {
    ensureView();
    p.save();
    p.setFont(font());
    const Layout L = layout(target, font(), p.device());
    const QRectF& A = L.area;
    const PlotTransform t{A, m_x, m_y, m_invertX};
    const QFontMetricsF fm(font(), p.device());

    p.setRenderHint(QPainter::Antialiasing, false);
    p.fillRect(target, m_pal.background);
    p.fillRect(A, m_pal.plotArea);
    // grid and axes: cosmetic pens of a whole number of device pixels (crisp, all equally wide)
    const qreal dpr = p.device() ? p.device()->devicePixelRatioF() : 1.0;
    const int hairW = std::max(1, qRound(dpr));
    auto hair = [hairW](const QColor& c) {
        QPen pen(c, hairW);
        pen.setCosmetic(true);
        return pen;
    };
    auto snap = [dpr, hairW](double v) {          // line centre on the device pixel grid
        const double d = v * dpr;
        return (hairW % 2 ? std::floor(d) + 0.5 : std::round(d)) / dpr;
    };
    const double aL = snap(A.left()), aB = snap(A.bottom());

    // grid
    p.setPen(hair(m_pal.grid));
    if (m_gridX)
        for (double v : L.xt.major) {
            const double x = t.px(v);
            if (x > A.left() + 1 && x < A.right() - 0.5)
                p.drawLine(QPointF(snap(x), A.top()), QPointF(snap(x), aB));
        }
    if (m_gridY)
        for (double v : L.yt.major) {
            const double y = t.py(v);
            if (y > A.top() + 0.5 && y < A.bottom() - 1)
                p.drawLine(QPointF(aL, snap(y)), QPointF(A.right(), snap(y)));
        }

    // items
    p.save();
    p.setClipRect(A);
    p.setRenderHint(QPainter::Antialiasing, true);
    p.setRenderHint(QPainter::TextAntialiasing, true);
    for (PlotItem* it : sortedItems()) {
        if (!it->isVisible())
            continue;
        p.save();
        it->paint(p, t);
        p.restore();
    }
    p.restore();

    // axes: lines, ticks, labels
    p.setPen(hair(m_pal.axis));
    p.drawLine(QPointF(aL, aB), QPointF(A.right(), aB));
    p.drawLine(QPointF(aL, aB), QPointF(aL, A.top()));
    for (double v : L.xt.major) {
        const double x = t.px(v);
        if (x >= A.left() - 0.5 && x <= A.right() + 0.5)
            p.drawLine(QPointF(snap(x), aB), QPointF(snap(x), aB + kTickLen));
    }
    for (double v : L.xt.minor) {
        const double x = t.px(v);
        if (x >= A.left() && x <= A.right())
            p.drawLine(QPointF(snap(x), aB), QPointF(snap(x), aB + kMinorLen));
    }
    for (double v : L.yt.major) {
        const double y = t.py(v);
        if (y >= A.top() - 0.5 && y <= A.bottom() + 0.5)
            p.drawLine(QPointF(aL - kTickLen, snap(y)), QPointF(aL, snap(y)));
    }
    p.setRenderHint(QPainter::TextAntialiasing, true);
    p.setPen(m_pal.text);
    const double lh = L.lineHeight;
    for (double v : L.xt.major) {
        const double x = t.px(v);
        if (x < A.left() - 0.5 || x > A.right() + 0.5)
            continue;
        const QString s = plot::formatTick(v, L.xt);
        const double w = fm.horizontalAdvance(s);
        p.drawText(QRectF(x - w / 2 - 2, A.bottom() + kTickLen + 2, w + 4, lh), Qt::AlignCenter, s);
    }
    for (double v : L.yt.major) {
        const double y = t.py(v);
        if (y < A.top() - 0.5 || y > A.bottom() + 0.5)
            continue;
        const QString s = plot::formatTick(v, L.yt);
        p.drawText(QRectF(target.left(), y - lh / 2, A.left() - kTickLen - 4 - target.left(), lh),
                   Qt::AlignRight | Qt::AlignVCenter, s);
    }
    // Axis labels: centred on the plot area but allowed to use the whole widget length (small plots on
    // small screens); elided rather than clipped if they are still too long.
    if (!m_xLabel.isEmpty()) {
        const double room = std::min(A.width(), 2 * std::min(A.center().x() - target.left(),
                                                             target.right() - A.center().x()));
        const double len = std::max(room, std::min(double(fm.horizontalAdvance(m_xLabel)) + 4, target.width()));
        p.drawText(QRectF(A.center().x() - len / 2, A.bottom() + kTickLen + 3 + lh + 3, len, lh), Qt::AlignCenter,
                   fm.elidedText(m_xLabel, Qt::ElideRight, int(target.width())));
    }
    if (!m_yLabel.isEmpty()) {
        const double full = target.height();
        const double need = fm.horizontalAdvance(m_yLabel) + 4;
        // centre on the plot area if the label fits there, otherwise on the widget
        const double cy = need <= A.height() ? A.center().y() : target.center().y();
        p.save();
        p.translate(target.left() + 5, cy);
        p.rotate(-90);
        p.drawText(QRectF(-full / 2, 0, full, lh), Qt::AlignCenter,
                   fm.elidedText(m_yLabel, Qt::ElideRight, int(full)));
        p.restore();
    }

    p.setRenderHint(QPainter::Antialiasing, true);
    if (m_legend)
        drawLegend(p, t);
    if (HeatMapItem* h = colorBarItem())
        drawColorBar(p, L, h);
    p.restore();
}

namespace {
bool segmentHitsRect(QPointF a, QPointF b, const QRectF& r) {   // Liang-Barsky
    double t0 = 0, t1 = 1;
    const double dx = b.x() - a.x(), dy = b.y() - a.y();
    const double pq[4][2] = {{-dx, a.x() - r.left()}, {dx, r.right() - a.x()},
                             {-dy, a.y() - r.top()}, {dy, r.bottom() - a.y()}};
    for (const auto& [pp, q] : pq) {
        if (pp == 0) {
            if (q < 0)
                return false;
        } else {
            const double u = q / pp;
            if (pp < 0)
                t0 = std::max(t0, u);
            else
                t1 = std::min(t1, u);
            if (t0 > t1)
                return false;
        }
    }
    return true;
}
}  // namespace

struct PlotWidget::LegendLayout {
    std::vector<PlotItem*> entries;
    int shown = 0;
    bool more = false;
    double lh = 0, sym = 22, pad = 6, tw = 0;
    QString moreText;
    QRectF box;
};

// How much a legend box at `box` would hide: curve segments crossing it, and (heavily weighted) peak labels.
double PlotWidget::legendOverlap(const QRectF& box, const PlotTransform& t, const QFontMetricsF& fm) const {
    const QRectF r = box.adjusted(-4, -4, 4, 4);
    double score = 0;
    for (PlotItem* it : m_items) {
        if (!it->isVisible())
            continue;
        if (auto* c = dynamic_cast<Curve*>(it)) {
            const std::size_t n = c->size();
            QPointF prev;
            bool havePrev = false;
            for (std::size_t i = 0; i < n; ++i) {
                const double x = c->m_x[i], y = c->m_y[i];
                if (!std::isfinite(x) || !std::isfinite(y)) {
                    havePrev = false;
                    continue;
                }
                const QPointF q = t.map(x, y);
                if (c->m_line && havePrev ? segmentHitsRect(prev, q, r) : r.contains(q))
                    score += 1;
                prev = q;
                havePrev = true;
            }
        } else if (auto* pl = dynamic_cast<PeakLabels*>(it)) {
            const std::vector<QRectF> rects = pl->layoutLabels(t, fm);
            for (std::size_t i = 0; i < rects.size(); ++i)
                if (!rects[i].isNull() && pl->labels()[i].x >= t.x.lo && pl->labels()[i].x <= t.x.hi &&
                    rects[i].intersects(r))
                    score += 200;
        }
    }
    return score;
}

PlotWidget::LegendLayout PlotWidget::legendLayout(const QFontMetricsF& fm, const PlotTransform& t) const {
    LegendLayout L;
    const QRectF& area = t.area;
    for (PlotItem* it : m_items)
        if (it->isVisible() && it->hasLegendEntry() && !it->name().isEmpty())
            L.entries.push_back(it);
    if (L.entries.empty())
        return L;
    L.lh = fm.height() + 2;
    const int maxRows = std::max(1, int((area.height() - 20) / L.lh) - 1);
    L.shown = std::min(int(L.entries.size()), maxRows);
    L.more = L.shown < int(L.entries.size());
    const double maxText = std::max(60.0, area.width() * 0.4);
    for (int i = 0; i < L.shown; ++i)
        L.tw = std::max(L.tw, std::min(maxText, fm.horizontalAdvance(L.entries[std::size_t(i)]->name())));
    L.moreText = QStringLiteral("… %1 more").arg(int(L.entries.size()) - L.shown);
    if (L.more)
        L.tw = std::max(L.tw, fm.horizontalAdvance(L.moreText));
    const double w = L.pad * 3 + L.sym + L.tw, h = L.pad * 2 + L.lh * (L.shown + (L.more ? 1 : 0));
    auto rectAt = [&](LegendPosition c) {
        const bool right = c == LegendPosition::TopRight || c == LegendPosition::BottomRight;
        const bool top = c == LegendPosition::TopRight || c == LegendPosition::TopLeft;
        return QRectF(right ? area.right() - 10 - w : area.left() + 10, top ? area.top() + 10 : area.bottom() - 10 - h,
                      w, h);
    };
    LegendPosition corner = m_legendPos;
    if (corner == LegendPosition::Auto) {
        // preference order on ties: top right, top left, bottom right, bottom left; the previous choice is
        // kept unless another corner is clearly emptier
        const LegendPosition order[] = {LegendPosition::TopRight, LegendPosition::TopLeft, LegendPosition::BottomRight,
                                        LegendPosition::BottomLeft};
        const double keep = legendOverlap(rectAt(m_autoCorner), t, fm);
        LegendPosition best = m_autoCorner;
        double bestScore = keep;
        for (LegendPosition c : order) {
            const double s = legendOverlap(rectAt(c), t, fm);
            if (s < bestScore - 0.5 && s < 0.8 * keep) {
                best = c;
                bestScore = s;
            }
        }
        m_autoCorner = best;
        corner = best;
    }
    L.box = rectAt(corner);
    return L;
}

void PlotWidget::setLegendPosition(LegendPosition pos) {
    m_legendPos = pos;
    m_autoCorner = LegendPosition::TopRight;
    update();
}

QRectF PlotWidget::legendRect() const {
    if (!m_legend)
        return {};
    ensureView();
    return legendLayout(QFontMetricsF(font(), this), transform()).box;
}

void PlotWidget::drawLegend(QPainter& p, const PlotTransform& t) {
    const QFontMetricsF fm(p.font(), p.device());
    const LegendLayout L = legendLayout(fm, t);
    if (L.entries.empty())
        return;
    const QRectF& box = L.box;
    const double lh = L.lh, sym = L.sym, pad = L.pad, tw = L.tw;
    p.setPen(QPen(withAlpha(m_pal.axis, 120), 1));
    p.setBrush(m_pal.legendBackground);
    p.drawRoundedRect(box, 4, 4);
    for (int i = 0; i < L.shown; ++i) {
        PlotItem* it = L.entries[std::size_t(i)];
        const QRectF row(box.left() + pad, box.top() + pad + i * lh, box.width() - 2 * pad, lh);
        p.save();
        it->paintLegendSymbol(p, QRectF(row.left(), row.top(), sym, row.height()));
        p.restore();
        p.setPen(m_pal.text);
        p.drawText(QRectF(row.left() + sym + pad, row.top(), tw, lh), Qt::AlignLeft | Qt::AlignVCenter,
                   fm.elidedText(it->name(), Qt::ElideMiddle, tw));
    }
    if (L.more) {
        p.setPen(m_pal.text);
        p.drawText(QRectF(box.left() + pad, box.top() + pad + L.shown * lh, box.width() - 2 * pad, lh),
                   Qt::AlignLeft | Qt::AlignVCenter, L.moreText);
    }
}

void PlotWidget::drawColorBar(QPainter& p, const Layout& L, HeatMapItem* hm) {
    const QRectF& r = L.colorBar;
    QImage grad(1, 256, QImage::Format_ARGB32);
    const std::vector<QRgb>& tab = lut(hm->colorMap());
    for (int i = 0; i < 256; ++i)
        grad.setPixel(0, 255 - i, tab[std::size_t(i)]);
    p.setRenderHint(QPainter::SmoothPixmapTransform, true);
    p.drawImage(r, grad);
    p.setRenderHint(QPainter::Antialiasing, false);
    p.setPen(QPen(m_pal.axis, 1));
    p.setBrush(Qt::NoBrush);
    p.drawRect(r);
    const Range lv = hm->levels();
    const QFontMetricsF fm(p.font(), p.device());
    const double lh = fm.height();
    double maxW = 0;
    for (double v : L.ct.major) {
        const double y = r.bottom() - (v - lv.lo) / (lv.span() > 0 ? lv.span() : 1) * r.height();
        if (y < r.top() - 0.5 || y > r.bottom() + 0.5)
            continue;
        p.setPen(QPen(m_pal.axis, 1));
        p.drawLine(QPointF(r.right(), y), QPointF(r.right() + kTickLen, y));
        const QString s = plot::formatTick(v, L.ct);
        maxW = std::max(maxW, fm.horizontalAdvance(s));
        p.setPen(m_pal.text);
        p.drawText(QRectF(r.right() + kTickLen + 3, y - lh / 2, 200, lh), Qt::AlignLeft | Qt::AlignVCenter, s);
    }
    if (!hm->colorBarLabel().isEmpty()) {
        double cw = 0;
        for (double v : L.ct.major)
            cw = std::max(cw, fm.horizontalAdvance(plot::formatTick(v, L.ct)));
        p.save();
        p.setPen(m_pal.text);
        p.translate(r.right() + kTickLen + 3 + cw + 6, r.center().y());
        p.rotate(-90);
        p.drawText(QRectF(-r.height() / 2, 0, r.height(), lh), Qt::AlignCenter, hm->colorBarLabel());
        p.restore();
    }
}

QString PlotWidget::readoutText(const QPointF& d) const {
    if (m_readoutFmt)
        return m_readoutFmt(d);
    auto dec = [](double span) { return std::clamp(3 - int(std::ceil(std::log10(std::max(span, 1e-12)))), 0, 8); };
    auto num = [](double v, int decimals) {
        QString s = QString::number(v, 'f', decimals);
        if (s.startsWith('-'))
            s.replace(0, 1, QChar(0x2212));
        return s;
    };
    return QStringLiteral("x  %1    y  %2").arg(num(d.x(), dec(m_x.span())), num(d.y(), dec(m_y.span())));
}

void PlotWidget::paintEvent(QPaintEvent*) {
    QPainter p(this);
    render(p, QRectF(rect()));
    const PlotTransform t = transform();
    p.setRenderHint(QPainter::Antialiasing, true);
    if (m_drag == Drag::Rubber && m_moved) {
        const QRectF r = QRectF(m_pressPos, m_lastPos).normalized().intersected(t.area);
        p.setPen(QPen(m_pal.accent, 1, Qt::DashLine));
        p.setBrush(withAlpha(m_pal.accent, 40));
        p.drawRect(r);
    }
    if (m_readout && m_hover && m_drag == Drag::None && t.area.contains(m_hoverPos)) {
        const QString s = readoutText(t.unmap(m_hoverPos));
        const QFontMetricsF fm(font(), this);
        QRectF box(t.area.left() + 8, t.area.top() + 6, fm.horizontalAdvance(s) + 12, fm.height() + 4);
        if (m_legend && box.intersects(legendRect()))   // the legend sits top-left: move the readout below the curves
            box.moveBottom(t.area.bottom() - 6);
        p.setPen(QPen(withAlpha(m_pal.axis, 110), 1));
        p.setBrush(m_pal.legendBackground);
        p.drawRoundedRect(box, 3, 3);
        p.setPen(m_pal.text);
        p.drawText(box, Qt::AlignCenter, s);
    }
}

// ---- interaction
void PlotWidget::mousePressEvent(QMouseEvent* e) {
    m_pressPos = m_lastPos = e->position();
    m_moved = false;
    m_pressButton = e->button();
    m_drag = Drag::None;
    if (e->button() != Qt::LeftButton)
        return;
    const PlotTransform t = transform();
    auto items = sortedItems();
    for (auto it = items.rbegin(); it != items.rend(); ++it) {
        if (!(*it)->isVisible())
            continue;
        const int part = (*it)->hitTest(m_pressPos, t);
        if (part >= 0) {
            m_drag = Drag::Item;
            m_dragItem = *it;
            m_dragPart = part;
            m_dragItem->dragStart(part, t.unmap(m_pressPos));
            return;
        }
    }
    if (e->modifiers() & Qt::ShiftModifier)
        m_drag = Drag::Rubber;
    else {
        m_drag = Drag::Pan;
        setCursor(Qt::ClosedHandCursor);
    }
}

void PlotWidget::mouseMoveEvent(QMouseEvent* e) {
    const QPointF pos = e->position();
    const PlotTransform t = transform();
    m_hoverPos = pos;
    m_hover = t.area.contains(pos);
    if (m_drag == Drag::None) {
        Qt::CursorShape cur = Qt::ArrowCursor;
        auto items = sortedItems();
        for (auto it = items.rbegin(); it != items.rend(); ++it) {
            if (!(*it)->isVisible())
                continue;
            const int part = (*it)->hitTest(pos, t);
            if (part >= 0) {
                cur = (*it)->cursorFor(part);
                break;
            }
        }
        setCursor(cur);
        if (m_hover)
            emit hovered(t.unmap(pos));
        update();
        return;
    }
    if ((pos - m_pressPos).manhattanLength() > 3)
        m_moved = true;
    switch (m_drag) {
    case Drag::Item:
        if (m_dragItem)
            m_dragItem->dragTo(m_dragPart, t.unmap(pos));
        break;
    case Drag::Pan:
        if (m_moved)
            panPixels(pos - m_lastPos);
        break;
    case Drag::Rubber: update(); break;
    case Drag::None: break;
    }
    m_lastPos = pos;
}

void PlotWidget::mouseReleaseEvent(QMouseEvent* e) {
    const QPointF pos = e->position();
    const Drag drag = m_drag;
    m_drag = Drag::None;
    unsetCursor();
    if (e->button() != m_pressButton)
        return;
    m_pressButton = Qt::NoButton;
    if (drag == Drag::Item) {
        if (m_dragItem)
            m_dragItem->dragEnd(m_dragPart);
        m_dragItem = nullptr;
        update();
        return;
    }
    const PlotTransform t = transform();
    if (drag == Drag::Rubber && m_moved) {
        const QRectF r = QRectF(m_pressPos, pos).normalized().intersected(t.area);
        if (r.width() > 4) {
            const double a = t.dataX(r.left()), b = t.dataX(r.right());
            if (r.height() > 4)
                m_autoY = false;
            setXRange(std::min(a, b), std::max(a, b));
            if (r.height() > 4)
                applyY(t.dataY(r.bottom()), t.dataY(r.top()));
        }
        update();
        return;
    }
    if (!m_moved) {
        const QPointF d = t.unmap(pos);
        emit clicked(d, e->button(), e->modifiers());
        if (e->button() == Qt::RightButton)
            emit rightClicked(d, e->globalPosition().toPoint());
    }
    update();
}

void PlotWidget::mouseDoubleClickEvent(QMouseEvent* e) {
    if (e->button() == Qt::LeftButton) {
        autoRange();
        m_pressButton = Qt::NoButton;          // the following release is not a click
        e->accept();
    }
}

void PlotWidget::wheelEvent(QWheelEvent* e) {
    const PlotTransform t = transform();
    const QPointF pos = e->position();
    const QPoint ang = e->angleDelta();
    if (ang.y() == 0 && ang.x() != 0) {        // horizontal scrolling pans along x
        panPixels(QPointF(ang.x() / 120.0 * t.area.width() * 0.1, 0));
        e->accept();
        return;
    }
    if (ang.y() == 0)
        return;
    const double factor = std::pow(0.85, ang.y() / 120.0);
    const bool overX = pos.y() > t.area.bottom() && pos.x() >= t.area.left();
    const bool overY = pos.x() < t.area.left();
    const bool mod = e->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier | Qt::MetaModifier);
    bool zoomXAxis;
    if (overX)
        zoomXAxis = true;
    else if (overY)
        zoomXAxis = false;
    else if (m_wheelMode == WheelMode::Spectrum)
        zoomXAxis = e->modifiers() & Qt::ControlModifier;
    else
        zoomXAxis = !mod;
    if (zoomXAxis)
        zoomX(factor, t.dataX(std::clamp(pos.x(), t.area.left(), t.area.right())));
    else
        scaleYTop(factor);
    e->accept();
}

void PlotWidget::leaveEvent(QEvent*) {
    if (m_hover) {
        m_hover = false;
        emit hoverLeft();
        update();
    }
}

void PlotWidget::changeEvent(QEvent* e) {
    if ((e->type() == QEvent::PaletteChange || e->type() == QEvent::StyleChange) && !m_customPalette) {
        m_pal = PlotPalette::defaultPalette();
        update();
    }
    if (e->type() == QEvent::FontChange)
        update();
    QWidget::changeEvent(e);
}

// ---- output
QImage PlotWidget::renderImage(QSize logicalSize, qreal scale) {
    if (logicalSize.isEmpty())
        logicalSize = size();
    if (logicalSize.isEmpty())
        logicalSize = QSize(640, 400);
    if (!(scale > 0))
        scale = 1;
    QImage img((QSizeF(logicalSize) * scale).toSize(), QImage::Format_ARGB32_Premultiplied);
    const int dpm = qRound(logicalDpiX() / 0.0254);
    img.setDotsPerMeterX(dpm);
    img.setDotsPerMeterY(dpm);
    img.setDevicePixelRatio(scale);
    img.fill(m_pal.background);
    QPainter p(&img);
    render(p, QRectF(QPointF(0, 0), QSizeF(logicalSize)));
    p.end();
    return img;
}

bool PlotWidget::exportPng(const QString& path, QSize logicalSize, qreal scale) {
    return renderImage(logicalSize, scale).save(path, "PNG");
}

QByteArray PlotWidget::toSvg(QSize sz, const QString& title) {
    if (sz.isEmpty())
        sz = size();
    if (sz.isEmpty())
        sz = QSize(640, 400);
    QBuffer buf;
    buf.open(QIODevice::WriteOnly);
    QSvgGenerator gen;
    gen.setOutputDevice(&buf);
    gen.setSize(sz);
    gen.setViewBox(QRect(QPoint(0, 0), sz));
    gen.setResolution(logicalDpiX());
    gen.setTitle(title.isEmpty() ? QStringLiteral("LibreRaman plot") : title);
    gen.setDescription(QStringLiteral("Created by LibreRaman " LIBRERAMAN_VERSION));
    QPainter p(&gen);
    render(p, QRectF(QPointF(0, 0), QSizeF(sz)));
    p.end();
    return buf.data();
}

bool PlotWidget::exportSvg(const QString& path, QSize size, const QString& title) {
    const QByteArray svg = toSvg(size, title);
    QFile f(path);
    if (svg.isEmpty() || !f.open(QIODevice::WriteOnly))
        return false;
    return f.write(svg) == svg.size();
}

}  // namespace lr::gui
