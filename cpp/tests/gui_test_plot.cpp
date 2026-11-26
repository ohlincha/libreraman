// Widget tests for src/gui: plot widget (ticks, automatic y range, zoom/pan, labels, export, heat map,
// decimation, interaction), status strip, splash, icon and theme. Runs offscreen.
#include "gui/icon.h"
#include "gui/plotwidget.h"
#include "gui/splash.h"
#include "gui/statusstrip.h"
#include "gui/theme.h"

#include <QFont>
#include <QApplication>
#include <QElapsedTimer>
#include <QPushButton>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QWheelEvent>

#include <cmath>
#include <random>

using namespace lr::gui;
using lr::Vec;

namespace {

void gaussSpectrum(Vec& x, Vec& y, int n = 2048) {
    x.clear();
    y.clear();
    for (int i = 0; i < n; ++i) {
        const double s = 100 + i * 3400.0 / (n - 1);
        x.push_back(s);
        y.push_back(100 + 1000 * std::exp(-std::pow((s - 1000) / 20, 2)) + 400 * std::exp(-std::pow((s - 2900) / 30, 2)));
    }
}

void sendWheel(QWidget* w, QPointF pos, int delta, Qt::KeyboardModifiers mods = Qt::NoModifier) {
    QWheelEvent ev(pos, w->mapToGlobal(pos), QPoint(), QPoint(0, delta), Qt::NoButton, mods, Qt::NoScrollPhase,
                   false);
    QApplication::sendEvent(w, &ev);
}

}  // namespace

class GuiTestPlot : public QObject {
    Q_OBJECT
private slots:
    void ticks();
    void tickFormat();
    void autoYRangeFunction();
    void autoYOverVisibleRange();
    void zoomPanTransforms();
    void wheelAndDoubleClick();
    void invertedX();
    void clickSignals();
    void rubberBand();
    void draggableLineAndRegion();
    void labelStaggering();
    void legendPlacement();
    void exportPngSvg();
    void heatMap();
    void decimationKeepsMinMax();
    void curveFeatures();
    void statusStrip();
    void elidedLabel();
    void splashAndIcon();
    void themePalette();
};

void GuiTestPlot::ticks() {
    QCOMPARE(plot::niceStep(3350, 7), 500.0);
    QCOMPARE(plot::niceStep(1, 4), 0.25);
    QCOMPARE(plot::niceStep(100, 5), 20.0);
    Ticks t = plot::niceTicks(150, 3500, 7);
    QCOMPARE(t.step, 500.0);
    QCOMPARE(t.major.front(), 500.0);
    QCOMPARE(t.major.back(), 3500.0);
    QCOMPARE(int(t.major.size()), 7);
    QCOMPARE(t.decimals, 0);
    QVERIFY(!t.minor.empty());
    for (double m : t.minor)
        QVERIFY(std::abs(std::remainder(m, 500.0)) > 1e-6);
    t = plot::niceTicks(0, 1, 4);
    QCOMPARE(t.step, 0.25);
    QCOMPARE(t.decimals, 2);
    QCOMPARE(int(t.major.size()), 5);
    t = plot::niceTicks(0, 25000, 10);
    QCOMPARE(t.step, 2500.0);
    QCOMPARE(t.decimals, 0);                             // not "2500.0"
    t = plot::niceTicks(-0.3, 0.3, 6);
    QVERIFY(std::find(t.major.begin(), t.major.end(), 0.0) != t.major.end());   // exact zero, no -1e-17
    QVERIFY(plot::niceTicks(1, 1, 5).major.empty());
    QVERIFY(plot::niceTicks(0, qInf(), 5).major.empty());
}

void GuiTestPlot::tickFormat() {
    Ticks t = plot::niceTicks(-1000, 1000, 4);
    QCOMPARE(plot::formatTick(-500, t), QString(QChar(0x2212)) + QStringLiteral("500"));
    QCOMPARE(plot::formatTick(0, t), QStringLiteral("0"));
    t = plot::niceTicks(0, 1, 4);
    QCOMPARE(plot::formatTick(0.25, t), QStringLiteral("0.25"));
    t = plot::niceTicks(0, 5e6, 5);
    QCOMPARE(plot::formatTick(2e6, t), QStringLiteral("2×10⁶"));
    QCOMPARE(plot::superscript(-1), QStringLiteral("⁻¹"));
}

void GuiTestPlot::autoYRangeFunction() {
    // Python: setYRange(lo, hi + headroom*(hi-lo or 1), padding=0.02)
    Range r = plot::autoYRange(0, 100, 0.18);
    QVERIFY(qFuzzyCompare(r.lo, -0.02 * 118));
    QVERIFY(qFuzzyCompare(r.hi, 118 + 0.02 * 118));
    // flat (one point): ±2.5 % of the value, at least ±0.5 (not a range of 1e-9, which gave unreadable ticks)
    r = plot::autoYRange(5, 5, 0.18);
    QVERIFY(qFuzzyCompare(r.lo, 4.5 - 0.02 * 1.18));
    QVERIFY(qFuzzyCompare(r.hi, 5.5 + 0.18 + 0.02 * 1.18));
    r = plot::autoYRange(47853.9, 47853.9, 0.08);      // trend with one spectrum
    QVERIFY(r.lo < 47853.9 - 1000 && r.hi > 47853.9 + 1000);
}

void GuiTestPlot::autoYOverVisibleRange() {
    PlotWidget w;
    w.resize(800, 500);
    Vec x, y;
    gaussSpectrum(x, y);
    w.addCurve(x, y);
    QVERIFY(w.autoY());
    QCOMPARE(w.xRange().lo, 100.0);                     // automatic x: data range
    QCOMPARE(w.xRange().hi, 3500.0);
    Range expect = plot::autoYRange(100, 1100, 0.18);
    QVERIFY(std::abs(w.yRange().hi - expect.hi) < 1);
    QVERIFY(std::abs(w.yRange().lo - expect.lo) < 1);
    QSignalSpy xs(&w, &PlotWidget::xRangeChanged);
    w.setXRange(2500, 3500);                            // only the 2900 band visible
    QCOMPARE(xs.count(), 1);
    expect = plot::autoYRange(100, 500, 0.18);
    QVERIFY(std::abs(w.yRange().hi - expect.hi) < 2);
    w.setHeadroom(0.08);
    expect = plot::autoYRange(100, 500, 0.08);
    QVERIFY(std::abs(w.yRange().hi - expect.hi) < 2);
    // manual y stays
    w.setAutoY(false);
    w.setYRange(0, 50);
    w.setXRange(500, 1500);
    QCOMPARE(w.yRange().hi, 50.0);
    // invisible/non-autoscale curves are ignored
    w.setAutoY(true);
    Curve* big = w.addCurve(Vec{500, 1500}, Vec{1e6, 1e6});
    QVERIFY(w.yRange().hi > 1e6);
    big->setAutoScale(false);
    QVERIFY(w.yRange().hi < 2000);
    big->setAutoScale(true);
    big->setVisible(false);
    QVERIFY(w.yRange().hi < 2000);
    // setData on an autoscaled view updates the range lazily
    big->setVisible(true);
    big->setData(Vec{500, 1500}, Vec{0, 5000});
    QVERIFY(w.yRange().hi > 5000 && w.yRange().hi < 7000);
}

void GuiTestPlot::zoomPanTransforms() {
    PlotWidget w;
    w.resize(800, 500);
    w.addCurve(Vec{0, 1000}, Vec{0, 10});
    w.setXRange(0, 1000);
    const PlotTransform t = w.transform();
    QVERIFY(t.area.width() > 600 && t.area.height() > 350);
    QVERIFY(w.rect().contains(t.area.toRect()));
    // data <-> pixel round trip
    const QPointF d(250, 3);
    QVERIFY((w.mapToData(w.mapFromData(d)) - d).manhattanLength() < 1e-9);
    QCOMPARE(t.px(0), t.area.left());
    QCOMPARE(t.px(1000), t.area.right());
    // zoom around a point keeps that point fixed
    w.zoomX(0.5, 500);
    QCOMPARE(w.xRange().lo, 250.0);
    QCOMPARE(w.xRange().hi, 750.0);
    w.zoomX(0.5, 250);
    QCOMPARE(w.xRange().lo, 250.0);
    QCOMPARE(w.xRange().hi, 500.0);
    // drag to the right by half the plot width: view moves left by half the span
    const double half = w.plotArea().width() / 2;
    w.panPixels(QPointF(half, 0));
    QVERIFY(std::abs(w.xRange().lo - 125) < 1e-9);
    QVERIFY(std::abs(w.xRange().hi - 375) < 1e-9);
    QVERIFY(!w.autoX());
    // y: baseline stays, top scales; autoY off
    const Range y0 = w.yRange();
    w.scaleYTop(0.5);
    QVERIFY(!w.autoY());
    QCOMPARE(w.yRange().lo, y0.lo);
    QVERIFY(std::abs(w.yRange().span() - y0.span() / 2) < 1e-9);
    // y panning only when enabled
    const Range y1 = w.yRange();
    w.panPixels(QPointF(0, 50));
    QCOMPARE(w.yRange().lo, y1.lo);
    w.setPanY(true);
    w.panPixels(QPointF(0, 50));
    QVERIFY(w.yRange().lo > y1.lo);
}

void GuiTestPlot::wheelAndDoubleClick() {
    PlotWidget w;
    w.resize(800, 500);
    Vec x, y;
    gaussSpectrum(x, y);
    w.addCurve(x, y);
    w.setHomeXRange(150, 3500);
    QCOMPARE(w.xRange().lo, 150.0);
    const QPointF c = w.plotArea().center();
    QCOMPARE(w.wheelMode(), WheelMode::ZoomX);         // default: wheel zooms x, y follows
    // Spectrum mode (Python): wheel = top of the y axis, baseline stays
    w.setWheelMode(WheelMode::Spectrum);
    const Range y0 = w.yRange();
    sendWheel(&w, c, 120);
    QVERIFY(!w.autoY());
    QCOMPARE(w.yRange().lo, y0.lo);
    QVERIFY(std::abs(w.yRange().span() - 0.85 * y0.span()) < 1e-6);
    QCOMPARE(w.xRange().lo, 150.0);
    // Ctrl+wheel: x zoom around the cursor
    const double cx = w.mapToData(c).x();
    sendWheel(&w, c, 120, Qt::ControlModifier);
    QVERIFY(std::abs(w.xRange().span() - 0.85 * 3350) < 1e-6);
    QVERIFY(std::abs(w.mapToData(c).x() - cx) < 1e-6);
    // wheel over the x axis zooms x as well
    const double sp = w.xRange().span();
    sendWheel(&w, QPointF(c.x(), w.plotArea().bottom() + 10), -120);
    QVERIFY(std::abs(w.xRange().span() - sp / 0.85) < 1e-6);
    // ZoomX mode: plain wheel zooms x, Ctrl scales y
    w.setWheelMode(WheelMode::ZoomX);
    const Range ys = w.yRange();
    sendWheel(&w, c, 240);
    QVERIFY(std::abs(w.xRange().span() - sp / 0.85 * 0.85 * 0.85) < 1e-6);
    QVERIFY(w.autoY());                                 // intensity follows the visible x range
    // zooming out stops at the data/home extent (the spectrum can't be lost)
    for (int k = 0; k < 20; ++k) sendWheel(&w, c, -120);
    QCOMPARE(w.xRange().lo, w.xLimit().lo);             // data and home range united
    QCOMPARE(w.xRange().hi, w.xLimit().hi);
    sendWheel(&w, c, 120, Qt::ControlModifier);
    QVERIFY(w.yRange().span() < ys.span());
    // double-click: home x range, automatic y, autoRangeRequested
    QSignalSpy autoSpy(&w, &PlotWidget::autoRangeRequested);
    QTest::mouseDClick(&w, Qt::LeftButton, Qt::NoModifier, c.toPoint());
    QCOMPARE(autoSpy.count(), 1);
    QCOMPARE(w.xRange().lo, 150.0);                     // the home range
    QCOMPARE(w.xRange().hi, 3500.0);
    QVERIFY(w.autoY());
    // without home: data range
    w.clearHomeXRange();
    w.autoRange();
    QCOMPARE(w.xRange().lo, 100.0);
    QCOMPARE(w.xRange().hi, 3500.0);
}

void GuiTestPlot::invertedX() {
    PlotWidget w;
    w.resize(600, 400);
    w.addCurve(Vec{0, 100}, Vec{0, 1});
    w.setXInverted(true);
    const PlotTransform t = w.transform();
    QCOMPARE(t.px(100), t.area.left());
    QCOMPARE(t.px(0), t.area.right());
    QVERIFY(std::abs(t.dataX(t.px(30)) - 30) < 1e-9);
    w.zoomX(0.5, 50);                                  // room to pan (panning stays inside the data)
    const double lo0 = w.xRange().lo;
    w.panPixels(QPointF(t.area.width() / 10, 0));     // drag right on an inverted axis: higher values
    QVERIFY(w.xRange().lo > lo0);
}

void GuiTestPlot::clickSignals() {
    PlotWidget w;
    w.resize(800, 500);
    w.addCurve(Vec{0, 1000}, Vec{0, 10});
    w.setXRange(200, 800);                             // zoomed in: room to pan
    QSignalSpy clicks(&w, &PlotWidget::clicked);
    QSignalSpy right(&w, &PlotWidget::rightClicked);
    const QPointF p = w.mapFromData(QPointF(400, 5));
    QTest::mouseClick(&w, Qt::LeftButton, Qt::NoModifier, p.toPoint());
    QCOMPARE(clicks.count(), 1);
    const QPointF d = clicks.at(0).at(0).toPointF();
    QVERIFY(std::abs(d.x() - 400) < 2);
    QCOMPARE(clicks.at(0).at(1).value<Qt::MouseButton>(), Qt::LeftButton);
    QTest::mouseClick(&w, Qt::RightButton, Qt::NoModifier, p.toPoint());
    QCOMPARE(right.count(), 1);
    QVERIFY(std::abs(right.at(0).at(0).toPointF().x() - 400) < 2);
    // a drag is a pan, not a click
    QTest::mousePress(&w, Qt::LeftButton, Qt::NoModifier, p.toPoint());
    QTest::mouseMove(&w, (p + QPointF(80, 0)).toPoint());
    QTest::mouseRelease(&w, Qt::LeftButton, Qt::NoModifier, (p + QPointF(80, 0)).toPoint());
    QCOMPARE(clicks.count(), 2);                        // only the right click was added
    QVERIFY(w.xRange().lo < 200);
}

void GuiTestPlot::rubberBand() {
    PlotWidget w;
    w.resize(800, 500);
    w.addCurve(Vec{0, 1000}, Vec{0, 10});
    w.setXRange(0, 1000);
    const QPoint a = w.mapFromData(QPointF(200, 8)).toPoint(), b = w.mapFromData(QPointF(400, 2)).toPoint();
    QTest::mousePress(&w, Qt::LeftButton, Qt::ShiftModifier, a);
    QTest::mouseMove(&w, b);
    QTest::mouseRelease(&w, Qt::LeftButton, Qt::ShiftModifier, b);
    QVERIFY(std::abs(w.xRange().lo - 200) < 3);
    QVERIFY(std::abs(w.xRange().hi - 400) < 3);
    QVERIFY(std::abs(w.yRange().lo - 2) < 0.1);
    QVERIFY(std::abs(w.yRange().hi - 8) < 0.1);
    QVERIFY(!w.autoY());
}

void GuiTestPlot::draggableLineAndRegion() {
    PlotWidget w;
    w.resize(800, 500);
    w.addCurve(Vec{0, 1000}, Vec{0, 10});
    w.setXRange(0, 1000);
    InfiniteLine* l = w.addVLine(300, QPen(), true);
    QSignalSpy moved(l, &InfiniteLine::moved);
    QSignalSpy done(l, &InfiniteLine::moveFinished);
    const QPoint p = w.mapFromData(QPointF(300, 5)).toPoint();
    const QPoint q = w.mapFromData(QPointF(600, 5)).toPoint();
    QTest::mousePress(&w, Qt::LeftButton, Qt::NoModifier, p);
    QTest::mouseMove(&w, q);
    QTest::mouseRelease(&w, Qt::LeftButton, Qt::NoModifier, q);
    QVERIFY(std::abs(l->value() - 600) < 3);
    QVERIFY(moved.count() >= 1);
    QCOMPARE(done.count(), 1);
    QCOMPARE(w.xRange().lo, 0.0);                       // dragging the line did not pan
    l->setBounds(0, 500);
    QCOMPARE(l->value(), 500.0);

    InfiniteLine* h = w.addHLine(5);
    QCOMPARE(h->orientation(), InfiniteLine::Horizontal);
    QVERIFY(!h->isMovable());

    RegionItem* r = w.addRegion(100, 200);
    QCOMPARE(r->zValue(), -10.0);                       // below curves (0), lines 5, labels 10
    QCOMPARE(l->zValue(), 5.0);
    QSignalSpy rc(r, &RegionItem::regionChangeFinished);
    const QPoint e0 = w.mapFromData(QPointF(200, 5)).toPoint(), e1 = w.mapFromData(QPointF(250, 5)).toPoint();
    QTest::mousePress(&w, Qt::LeftButton, Qt::NoModifier, e0);       // right edge
    QTest::mouseMove(&w, e1);
    QTest::mouseRelease(&w, Qt::LeftButton, Qt::NoModifier, e1);
    QVERIFY(std::abs(r->region().hi - 250) < 3);
    QCOMPARE(r->region().lo, 100.0);
    QCOMPARE(rc.count(), 1);
    const QPoint m0 = w.mapFromData(QPointF(150, 5)).toPoint(), m1 = w.mapFromData(QPointF(170, 5)).toPoint();
    QTest::mousePress(&w, Qt::LeftButton, Qt::NoModifier, m0);       // inside: moves the region
    QTest::mouseMove(&w, m1);
    QTest::mouseRelease(&w, Qt::LeftButton, Qt::NoModifier, m1);
    QVERIFY(std::abs(r->region().lo - 120) < 3);
    QVERIFY(std::abs(r->region().span() - 150) < 3);
}

// Automatic legend corner: where it hides the fewest curve segments and peak labels.
void GuiTestPlot::legendPlacement() {
    auto make = [](double peakAt, PlotWidget& w) {
        Vec x, y;
        for (int i = 0; i < 2048; ++i) {
            const double s = 100 + i * 3400.0 / 2047;
            x.push_back(s);
            y.push_back(50 + 10000 * std::exp(-std::pow((s - peakAt) / 15, 2)) + 3000 * std::exp(-std::pow((s - 1800) / 20, 2)));
        }
        w.resize(640, 400);
        QFont f = w.font();                    // fixed pixel size: the CI runners render fonts at 72 dpi, and a
        f.setPixelSize(16);                    // smaller legend fit beside the peak in the top right
        w.setFont(f);
        w.setHeadroom(0.18);
        w.addCurve(x, y, QPen(Qt::blue), QStringLiteral("a rather long sample name  12:34:56"));
        w.addPeakLabels()->setPeaks({peakAt, 1800}, x, y);
        w.setLegendVisible(true);
    };
    {   // tall peak at the right: the legend moves to the top left
        PlotWidget w;
        make(3300, w);
        const QRectF r = w.legendRect();
        QVERIFY(!r.isNull());
        QVERIFY(r.center().x() < w.plotArea().center().x());
        QVERIFY(r.center().y() < w.plotArea().center().y());
        // and does not cover the peak's label
        const QPointF peak = w.mapFromData({3300, 10050});
        QVERIFY(!r.contains(QPointF(peak.x(), peak.y() - 8)));
    }
    {   // tall peak at the left: top right (the default corner)
        PlotWidget w;
        make(300, w);
        const QRectF r = w.legendRect();
        QVERIFY(r.center().x() > w.plotArea().center().x());
        QVERIFY(r.center().y() < w.plotArea().center().y());
        // a fixed corner is respected
        w.setLegendPosition(LegendPosition::BottomLeft);
        QCOMPARE(w.legendPosition(), LegendPosition::BottomLeft);
        const QRectF b = w.legendRect();
        QVERIFY(b.center().x() < w.plotArea().center().x());
        QVERIFY(b.center().y() > w.plotArea().center().y());
        w.setLegendVisible(false);
        QVERIFY(w.legendRect().isNull());
    }
}

void GuiTestPlot::labelStaggering() {
    // Python: level = level + 1 if p - last < min_gap else 0; drawn at level % 3
    QCOMPARE(plot::staggerLevels({100, 110, 120, 130, 500, 505, 900}, 20),
             (std::vector<int>{0, 1, 2, 3, 0, 1, 0}));
    QCOMPARE(plot::staggerLevels({}, 20), std::vector<int>{});

    PlotWidget w;
    w.resize(900, 500);
    Vec x, y;
    for (int i = 0; i < 3400; ++i) {
        x.push_back(100 + i);
        y.push_back(100 + 1000 * std::exp(-std::pow((x.back() - 2930) / 8, 2)) +
                    700 * std::exp(-std::pow((x.back() - 2975) / 8, 2)) +
                    800 * std::exp(-std::pow((x.back() - 2880) / 8, 2)));
    }
    w.addCurve(x, y);
    PeakLabels* pl = w.addPeakLabels();
    pl->setPeaks({2975, 2880, 2930}, x, y);            // sorted by x internally
    QCOMPARE(pl->labels().front().text, QStringLiteral("2880"));
    QVERIFY(std::abs(pl->labels()[1].y - 1100) < 5);   // y at the nearest x
    QCOMPARE(pl->levels(3400), (std::vector<int>{0, 1, 2}));   // 0.035*3400 = 119 > gaps
    QCOMPARE(pl->levels(500), (std::vector<int>{0, 0, 0}));    // zoomed in: 17.5 < gaps
    // drawn rectangles never overlap, also when a raised label belongs to a lower peak
    const PlotTransform t = w.transform();
    const QFontMetricsF fm(w.font());
    const auto rects = pl->layoutLabels(t, fm);
    for (std::size_t i = 0; i < rects.size(); ++i)
        for (std::size_t j = i + 1; j < rects.size(); ++j)
            QVERIFY(!rects[i].intersects(rects[j]));
    QVERIFY(rects[0].bottom() < t.py(900));             // above its peak
    // headroom keeps level-0 labels inside the plot
    QVERIFY(rects[1].top() > t.area.top());
}

void GuiTestPlot::exportPngSvg() {
    QTemporaryDir dir;
    QVERIFY(dir.isValid());
    PlotWidget w;
    w.resize(640, 400);
    Vec x, y;
    gaussSpectrum(x, y);
    w.addCurve(x, y, QPen(Qt::blue, 1.5), QStringLiteral("test"));
    w.setLegendVisible(true);
    w.setLabel(Axis::Bottom, QStringLiteral("Raman shift (cm⁻¹)"));
    w.setLabel(Axis::Left, QStringLiteral("Intensity"));
    const QString png = dir.filePath(QStringLiteral("plot.png"));
    QVERIFY(w.exportPng(png, QSize(500, 300), 2.0));
    QImage img(png);
    QCOMPARE(img.size(), QSize(1000, 600));
    // not blank: background, curve and text colours present
    QSet<QRgb> colours;
    for (int yy = 0; yy < img.height(); yy += 3)
        for (int xx = 0; xx < img.width(); xx += 3)
            colours.insert(img.pixel(xx, yy));
    QVERIFY(colours.size() > 20);
    const QString svg = dir.filePath(QStringLiteral("plot.svg"));
    QVERIFY(w.exportSvg(svg, QSize(500, 300), QStringLiteral("Test")));
    QFile f(svg);
    QVERIFY(f.open(QIODevice::ReadOnly));
    const QByteArray data = f.readAll();
    QVERIFY(data.size() > 2000);
    QVERIFY(data.contains("<svg"));
    QVERIFY(data.contains("polyline") || data.contains("<path"));
    QVERIFY(data.contains("Intensity"));
    QImage scr = w.renderImage();                       // --screenshot use: widget size
    QCOMPARE(scr.size(), QSize(640, 400));
}

void GuiTestPlot::heatMap() {
    PlotWidget w;
    w.resize(600, 400);
    HeatMapItem* hm = w.addHeatMap();
    // 3 rows × 4 cols, 0..11
    Vec v(12);
    for (int i = 0; i < 12; ++i)
        v[std::size_t(i)] = i;
    v[5] = qQNaN();
    hm->setData(3, 4, v);
    hm->setExtent(100, 500, 0, 3);
    QCOMPARE(hm->levels().lo, 0.0);                     // auto levels after the image is built
    const QImage& img = hm->image();
    QCOMPARE(img.size(), QSize(4, 3));
    QCOMPARE(hm->levels().hi, 11.0);
    QCOMPARE(QColor(img.pixel(0, 2)), colorMapColor(ColorMap::Viridis, 0));    // row 0 at the bottom
    QCOMPARE(QColor(img.pixel(3, 0)), colorMapColor(ColorMap::Viridis, 1));
    QCOMPARE(qAlpha(img.pixel(1, 1)), 0);                                       // NaN transparent
    // auto range = extent exactly (no headroom)
    QCOMPARE(w.xRange().lo, 100.0);
    QCOMPARE(w.xRange().hi, 500.0);
    QCOMPARE(w.yRange().lo, 0.0);
    QCOMPARE(w.yRange().hi, 3.0);
    // rendered cells have the mapped colours
    const QImage out = w.renderImage();
    const PlotTransform t = w.transform();
    const QPoint cellTopRight = t.map(450, 2.5).toPoint();
    const QPoint cellBottomLeft = t.map(150, 0.5).toPoint();
    QCOMPARE(QColor(out.pixel(cellTopRight)), colorMapColor(ColorMap::Viridis, 1));
    QCOMPARE(QColor(out.pixel(cellBottomLeft)), colorMapColor(ColorMap::Viridis, 0));
    // fixed levels and other maps
    hm->setLevels(0, 22);
    hm->setColorMap(ColorMap::Grey);
    QCOMPARE(QColor(hm->image().pixel(3, 0)).red(), colorMapColor(ColorMap::Grey, 0.5).red());
    // colour bar takes room at the right
    const double before = w.plotArea().width();
    hm->setColorBarVisible(true);
    hm->setColorBarLabel(QStringLiteral("Intensity"));
    QVERIFY(w.plotArea().width() < before - 30);
    QVERIFY(!w.renderImage().isNull());
    // many columns are reduced without crashing; zoomed-in crop
    Vec big(20 * 5000);
    for (std::size_t i = 0; i < big.size(); ++i)
        big[i] = std::sin(double(i));
    hm->setData(20, 5000, big);
    w.setXRange(200, 210);
    QVERIFY(!w.renderImage().isNull());
}

void GuiTestPlot::decimationKeepsMinMax() {
    std::mt19937 rng(3);
    std::normal_distribution<double> n(0, 1);
    const std::size_t N = 100000;
    Vec x(N), y(N);
    for (std::size_t i = 0; i < N; ++i) {
        x[i] = double(i);
        y[i] = n(rng);
    }
    y[12345] = 50;
    y[67890] = -40;
    y[500] = qQNaN();
    const int cols = 700;
    auto pts = plot::decimateMinMax(x.data(), y.data(), 0, N, 0, double(N - 1), cols);
    QVERIFY(pts.size() <= std::size_t(cols) * 4);
    QVERIFY(pts.size() >= std::size_t(cols) * 2);
    double mx = -1e9, mn = 1e9;
    for (const QPointF& p : pts) {
        mx = std::max(mx, p.y());
        mn = std::min(mn, p.y());
        QVERIFY(std::isfinite(p.y()));
    }
    QCOMPARE(mx, 50.0);
    QCOMPARE(mn, -40.0);
    // every column keeps its own min and max
    for (int c = 0; c < cols; c += 37) {
        const double c0 = c * (N - 1.0) / cols, c1 = (c + 1) * (N - 1.0) / cols;
        double cmin = 1e9, cmax = -1e9, dmin = 1e9, dmax = -1e9;
        for (std::size_t i = 0; i < N; ++i)
            if (x[i] >= c0 && x[i] < c1 && std::isfinite(y[i])) {
                cmin = std::min(cmin, y[i]);
                cmax = std::max(cmax, y[i]);
            }
        for (const QPointF& p : pts)
            if (p.x() >= c0 && p.x() < c1) {
                dmin = std::min(dmin, p.y());
                dmax = std::max(dmax, p.y());
            }
        QCOMPARE(dmin, cmin);
        QCOMPARE(dmax, cmax);
    }
    // order of x is preserved (a line through the points does not go back)
    for (std::size_t i = 1; i < pts.size(); ++i)
        QVERIFY(pts[i].x() >= pts[i - 1].x());
    // descending x works too
    Vec xr(x.rbegin(), x.rend()), yr(y.rbegin(), y.rend());
    auto pr = plot::decimateMinMax(xr.data(), yr.data(), 0, N, 0, double(N - 1), cols);
    QCOMPARE(pr.size(), pts.size());
    // the widget uses it when zoomed out and renders quickly
    PlotWidget w;
    w.resize(800, 500);
    for (int k = 0; k < 40; ++k) {
        Vec xs, ys;
        gaussSpectrum(xs, ys);
        for (double& v : ys)
            v += k * 50;
        w.addCurve(xs, ys);
    }
    w.addCurve(x, y);
    QElapsedTimer timer;
    timer.start();
    for (int i = 0; i < 5; ++i)
        w.renderImage();
    qInfo("5 renders of 40 × 2048 + 100000 points: %lld ms", static_cast<long long>(timer.elapsed()));
}

void GuiTestPlot::curveFeatures() {
    PlotWidget w;
    w.resize(600, 400);
    Curve* c = w.addCurve(Vec{0, 1, 2, 3}, Vec{1, qQNaN(), 3, 4});
    QCOMPARE(c->pen().color(), w.plotPalette().main);  // default pen
    QCOMPARE(c->size(), std::size_t(4));
    QCOMPARE(c->yBounds(0, 3).lo, 1.0);
    QCOMPARE(c->yBounds(0, 3).hi, 4.0);
    QCOMPARE(c->yBounds(1.5, 2.5).hi, 3.0);
    c->setFill(QBrush(QColor(0, 0, 255, 40)), -2);
    QCOMPARE(c->yBounds(0, 3).lo, -2.0);               // fill level is part of the data range
    c->setMarker(Marker::Diamond, 7);
    c->setOpacity(0.5);
    c->setStyle(Qt::DashLine);
    c->setName(QStringLiteral("c"));
    Curve* s = w.addScatter(Vec{3, 1, 2}, Vec{1, 2, 3}, Qt::red);   // unordered x
    QVERIFY(!s->lineVisible());
    QCOMPARE(s->xBounds().lo, 1.0);
    QCOMPARE(s->xBounds().hi, 3.0);
    TextItem* t = w.addText(QStringLiteral("label"), 1, 2);
    t->setBackground(QColor(255, 255, 255, 200));
    const QRectF br = t->boundingRect(w.transform(), w.font());
    QVERIFY(std::abs(br.center().x() - w.mapFromData(QPointF(1, 2)).x()) < 1);   // anchor (0.5, 1)
    QVERIFY(br.bottom() <= w.mapFromData(QPointF(1, 2)).y() + 0.5);
    w.setLegendVisible(true);
    QVERIFY(!w.renderImage().isNull());
    QCOMPARE(int(w.items().size()), 3);
    w.removeItem(s);
    QCOMPARE(int(w.items().size()), 2);
    delete t;                                           // deleting an item directly also unregisters it
    QCOMPARE(int(w.items().size()), 1);
    w.clear();
    QVERIFY(w.items().empty());
    QVERIFY(!w.renderImage().isNull());                // empty plot renders
}

void GuiTestPlot::statusStrip() {
    StatusStrip s;
    QSignalSpy stop(&s, &StatusStrip::stopClicked);
    QCOMPARE(s.instrumentTile()->mainText(), QStringLiteral("○ SEARCHING…"));
    StatusView v;
    v.connection = QStringLiteral("connected");
    v.armed = true;
    v.channel = 532;
    v.model = QStringLiteral("Cora 5000");
    v.serial = QStringLiteral("99002169");
    s.update(v);
    QCOMPARE(s.instrumentTile()->mainText(), QStringLiteral("● CONNECTED"));
    QCOMPARE(s.instrumentTile()->subText(), QStringLiteral("Cora 5000 · S/N 99002169"));
    QCOMPARE(s.laserTile()->mainText(), QStringLiteral("▲ ARMED"));
    QCOMPARE(s.laserTile()->tone(), theme::Tone::Orange);
    QVERIFY(!s.laserTile()->isAlarm());
    QCOMPARE(s.channelTile()->mainText(), QStringLiteral("532 nm"));
    v.laserOn = true;
    s.update(v);
    QCOMPARE(s.laserTile()->mainText(), QStringLiteral("✹ LASER ON"));
    QVERIFY(s.laserTile()->isAlarm());
    QCOMPARE(s.laserTile()->subText(), QStringLiteral("532 nm emitting"));
    v.laserOn = false;
    v.compartmentOpen = true;
    v.simulated = true;
    s.update(v);
    QCOMPARE(s.instrumentTile()->mainText(), QStringLiteral("● SIMULATOR"));
    QCOMPARE(s.compartmentTile()->mainText(), QStringLiteral("▢ OPEN"));
    v.connection = QStringLiteral("lost");
    s.update(v);
    QVERIFY(s.instrumentTile()->isAlarm());
    QCOMPARE(s.laserTile()->mainText(), QStringLiteral("—"));
    QCOMPARE(s.status().connection, QStringLiteral("lost"));
    QTest::mouseClick(s.stopButton(), Qt::LeftButton);
    QCOMPARE(stop.count(), 1);
    s.resize(1200, 80);
    QVERIFY(!s.grab().isNull());
}

void GuiTestPlot::elidedLabel() {
    ElidedLabel l;
    l.setFullText(QStringLiteral("/home/user/Documents/LibreRaman/2026-11-20/a/long/path"));
    l.resize(120, 20);
    l.show();                                           // hidden widgets get their resize event on show
    QVERIFY(l.text().startsWith(QChar(0x2026)));       // elided at the front
    QVERIFY(l.text().endsWith(QStringLiteral("path")));
    QCOMPARE(l.toolTip(), l.fullText());
    l.resize(2000, 20);
    QCOMPARE(l.text(), l.fullText());
}

void GuiTestPlot::splashAndIcon() {
    Splash s;
    QSignalSpy skipped(&s, &Splash::skipped);
    s.message(QStringLiteral("Searching for instrument…"));
    s.setProgress(0.4);
    QCOMPARE(s.text(), QStringLiteral("Searching for instrument…"));
    QCOMPARE(s.progress(), 0.4);
    const QImage img = s.renderImage();
    QCOMPARE(img.size(), QSize(2 * kLogoWidth, 2 * kLogoHeight));
    QCOMPARE(qAlpha(img.pixel(0, 0)), 0);               // rounded corner
    QVERIFY(qAlpha(img.pixel(img.width() / 2, img.height() / 2)) == 255);
    s.show();
    QTest::mouseClick(&s, Qt::LeftButton);
    QCOMPARE(skipped.count(), 1);
    QVERIFY(!s.isVisible());
    for (int sz : {16, 48, 256}) {
        const QImage ic = drawIcon(sz);
        QCOMPARE(ic.size(), QSize(sz, sz));
        QCOMPARE(qAlpha(ic.pixel(sz / 2, sz / 2)), 255);
    }
    QCOMPARE(qAlpha(drawIcon(256).pixel(1, 1)), 0);
    const QIcon icon = appIcon();
    QVERIFY(!icon.isNull());
    QVERIFY(icon.availableSizes().contains(QSize(1024, 1024)));    // :/gui/icon.png from gui.qrc
}

void GuiTestPlot::themePalette() {
    auto* app = qobject_cast<QApplication*>(QCoreApplication::instance());
    QVERIFY(app);
    PlotWidget follows;
    PlotWidget fixed;
    fixed.setPlotPalette(PlotPalette::light());
    theme::apply(*app, theme::Mode::Dark);
    QVERIFY(theme::isDark());
    QVERIFY(PlotPalette::defaultPalette().isDark());
    QVERIFY(theme::plotPalette().isDark());
    QVERIFY(app->palette().color(QPalette::Window).lightness() < 100);
    QVERIFY(follows.plotPalette().isDark());           // existing plots follow (palette change event)
    QVERIFY(!fixed.plotPalette().isDark());
    QVERIFY(PlotWidget().plotPalette().isDark());
    QVERIFY(theme::bigButtonStyle(theme::Action::Measure).contains(QStringLiteral("#1f6f68")));
    QVERIFY(theme::bigButtonStyle(theme::Action::Series).contains(QStringLiteral("#34477a")));
    QVERIFY(theme::stopButtonStyle().contains(QStringLiteral("#b42318")));
    theme::apply(*app, theme::Mode::Light);
    QVERIFY(!theme::isDark());
    QVERIFY(!follows.plotPalette().isDark());
    QCOMPARE(theme::toneColor(theme::Tone::Red), QColor("#c0392b"));
    QCOMPARE(theme::tileBackground(), QColor("#27303b"));
    QCOMPARE(PlotPalette::light().main, QColor("#1f4e79"));
    QCOMPARE(int(PlotPalette::light().series.size()), 9);
    QCOMPARE(PlotPalette::light().seriesColor(9), PlotPalette::light().seriesColor(0));
}

QTEST_MAIN(GuiTestPlot)
#include "gui_test_plot.moc"
