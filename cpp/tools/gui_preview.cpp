// Renders the GUI building blocks to PNG files for visual checks (not built by default:
// cmake -DLIBRERAMEN_GUI_PREVIEW=ON). Usage: QT_QPA_PLATFORM=offscreen gui_preview [output dir]
#include "gui/icon.h"
#include "gui/plotwidget.h"
#include "gui/splash.h"
#include "gui/statusstrip.h"
#include "gui/theme.h"

#include <QApplication>
#include <QDir>
#include <QLayout>
#include <QMouseEvent>
#include <QPainter>
#include <QVBoxLayout>

#include <cmath>
#include <cstdio>
#include <random>

using namespace lr::gui;
using lr::Vec;

namespace {

struct Band {
    double pos, height, width;
};

// Raman-like spectrum on 2048 points: Lorentzian bands on a fluorescence background, shot noise.
void spectrum(const std::vector<Band>& bands, double bg, unsigned seed, Vec& x, Vec& y, Vec* base = nullptr) {
    std::mt19937 rng(seed);
    std::normal_distribution<double> noise(0, 1);
    x.clear();
    y.clear();
    if (base)
        base->clear();
    for (int i = 0; i < 2048; ++i) {
        const double s = 110 + i * (3550.0 - 110) / 2047;
        const double b = bg * (0.55 + 0.45 * std::exp(-std::pow((s - 1500) / 1500, 2)));
        double v = b;
        for (const Band& k : bands)
            v += k.height / (1 + std::pow((s - k.pos) / k.width, 2));
        x.push_back(s);
        y.push_back(v + noise(rng) * std::sqrt(std::max(v, 1.0)) * 0.6);
        if (base)
            base->push_back(b);
    }
}

const std::vector<Band> kEthanol = {{433, 1800, 9},  {882, 6500, 7},   {1052, 2600, 8}, {1096, 2300, 8},
                                    {1275, 1100, 8}, {1454, 3200, 10}, {2876, 5200, 14}, {2929, 7400, 13},
                                    {2973, 6100, 12}};
const std::vector<Band> kCCl4 = {{218, 6000, 5}, {314, 8200, 6}, {459, 21000, 3.5}, {762, 3400, 5}, {791, 3600, 5}};

Vec peaksOf(const std::vector<Band>& b) {
    Vec v;
    for (const Band& k : b)
        v.push_back(k.pos);
    return v;
}

bool save(const QImage& img, const QString& dir, const QString& name) {
    const QString path = dir + QLatin1Char('/') + name;
    const bool ok = img.save(path);
    std::printf("%s %s\n", ok ? "wrote" : "FAILED", qPrintable(path));
    return ok;
}

void measurePlot(PlotWidget& w, const std::vector<Band>& bands, double bg) {
    const PlotPalette& pal = w.plotPalette();
    w.setLabel(Axis::Bottom, QStringLiteral("Raman shift (cm⁻¹)"));
    w.setLabel(Axis::Left, QStringLiteral("Intensity (counts)"));
    Vec x, y, base;
    for (int i = 0; i < 4; ++i) {                        // faded previous spectra (Python: alpha 35 + 20 i)
        spectrum(bands, bg * (0.9 + 0.03 * i), 10 + unsigned(i), x, y);
        QColor c = pal.previous;
        c.setAlpha(35 + 20 * i);
        w.addCurve(x, y, QPen(c, 1.0));
    }
    spectrum(bands, bg, 1, x, y, &base);
    w.addCurve(x, base, QPen(pal.baseline, 1.2, Qt::DashLine));
    w.addCurve(x, y, QPen(pal.main, 1.6));
    w.addPeakLabels()->setPeaks(peaksOf(bands), x, y);
    w.setHomeXRange(150, 3500);
}

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    const QString dir = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QStringLiteral("gui-preview");
    QDir().mkpath(dir);
    theme::apply(app, theme::Mode::Light);
    const QSize size(900, 520);

    {   // Measure page: live spectrum, baseline, faded previous spectra, staggered peak labels
        PlotWidget w;
        w.resize(size);
        measurePlot(w, kEthanol, 3000);
        save(w.renderImage(size, 1.5), dir, QStringLiteral("spectrum-light.png"));
        w.setXRange(700, 1600);                           // zoomed: labels re-staggered, y rescaled
        save(w.renderImage(size, 1.5), dir, QStringLiteral("spectrum-zoom.png"));
        // interactive overlays (widget grab): hover readout, then a Shift+drag rubber band
        auto mouse = [&w](QEvent::Type type, QPointF pos, Qt::MouseButton b, Qt::MouseButtons bs,
                          Qt::KeyboardModifiers m) {
            QMouseEvent ev(type, pos, w.mapToGlobal(pos), b, bs, m);
            QApplication::sendEvent(&w, &ev);
        };
        const QPointF a = w.mapFromData(QPointF(1020, 6200)), b = w.mapFromData(QPointF(1130, 3600));
        mouse(QEvent::MouseMove, a, Qt::NoButton, Qt::NoButton, Qt::NoModifier);
        save(w.grab().toImage(), dir, QStringLiteral("hover.png"));
        mouse(QEvent::MouseButtonPress, a, Qt::LeftButton, Qt::LeftButton, Qt::ShiftModifier);
        mouse(QEvent::MouseMove, b, Qt::NoButton, Qt::LeftButton, Qt::ShiftModifier);
        save(w.grab().toImage(), dir, QStringLiteral("rubberband.png"));
        mouse(QEvent::MouseButtonRelease, b, Qt::LeftButton, Qt::NoButton, Qt::ShiftModifier);
        save(w.renderImage(size, 1.5), dir, QStringLiteral("rubberband-after.png"));
        w.exportSvg(dir + QStringLiteral("/spectrum-light.svg"), size);
    }
    {   // the same with the dark palette
        PlotWidget w;
        w.setPlotPalette(PlotPalette::dark());
        w.resize(size);
        measurePlot(w, kCCl4, 1500);
        save(w.renderImage(size, 1.5), dir, QStringLiteral("spectrum-dark.png"));
    }
    {   // Data area: overlay with legend, fit region and a draggable marker line
        PlotWidget w;
        w.resize(size);
        w.setLabel(Axis::Bottom, QStringLiteral("Raman shift (cm⁻¹)"));
        w.setLabel(Axis::Left, QStringLiteral("Intensity"));
        w.setHeadroom(0.08);
        w.setLegendVisible(true);
        const QStringList names = {QStringLiteral("ethanol 14:02:11"), QStringLiteral("ethanol 14:05:34 #2"),
                                   QStringLiteral("CCl4 reference"), QStringLiteral("mixture A"),
                                   QStringLiteral("mixture B (long name to be elided in the legend)")};
        for (int k = 0; k < names.size(); ++k) {
            std::vector<Band> b = k == 2 ? kCCl4 : kEthanol;
            if (k >= 3)
                for (const Band& c : kCCl4)
                    b.push_back({c.pos, c.height * 0.3 * (k - 2), c.width});
            Vec x, y;
            spectrum(b, 600 + 150 * k, 20 + unsigned(k), x, y);
            w.addCurve(x, y, QPen(w.plotPalette().seriesColor(k), 1.3), names[k]);
        }
        w.addRegion(820, 1150);
        auto* l = w.addVLine(1454, QPen(), true);
        l->setLabel(QStringLiteral("1454 cm⁻¹"));
        save(w.renderImage(size, 1.5), dir, QStringLiteral("overlay.png"));
    }
    {   // stacked
        PlotWidget w;
        w.resize(size);
        w.setLabel(Axis::Bottom, QStringLiteral("Raman shift (cm⁻¹)"));
        w.setLabel(Axis::Left, QStringLiteral("Intensity (offset)"));
        w.setHeadroom(0.08);
        for (int k = 0; k < 8; ++k) {
            Vec x, y;
            std::vector<Band> b = kEthanol;
            for (Band& c : b)
                c.height *= 1 - 0.08 * k;
            for (const Band& c : kCCl4)
                b.push_back({c.pos, c.height * 0.05 * k, c.width});
            spectrum(b, 500, 40 + unsigned(k), x, y);
            for (double& v : y)
                v += k * 3500;
            w.addCurve(x, y, QPen(w.plotPalette().seriesColor(k), 1.2));
        }
        save(w.renderImage(size, 1.5), dir, QStringLiteral("stacked.png"));
    }
    {   // heat map of a series with colour bar
        PlotWidget w;
        w.resize(size);
        w.setLabel(Axis::Bottom, QStringLiteral("Raman shift (cm⁻¹)"));
        w.setLabel(Axis::Left, QStringLiteral("spectrum #"));
        const int rows = 40, cols = 1200;
        Vec vals;
        for (int r = 0; r < rows; ++r) {
            std::vector<Band> b = kEthanol;
            for (Band& c : b)
                c.height *= 1 - r / 60.0;
            for (const Band& c : kCCl4)
                b.push_back({c.pos, c.height * r / 50.0, c.width});
            Vec x, y;
            spectrum(b, 300, 100 + unsigned(r), x, y);
            for (int c = 0; c < cols; ++c) {         // resample onto the common grid (nearest)
                const double s = 150 + c * (3500.0 - 150) / (cols - 1);
                const auto i = std::size_t((s - 110) / (3550.0 - 110) * 2047 + 0.5);
                vals.push_back(y[i]);
            }
        }
        HeatMapItem* hm = w.addHeatMap();
        hm->setData(rows, cols, vals);
        hm->setExtent(150, 3500, 0, rows);
        hm->setColorBarVisible(true);
        hm->setColorBarLabel(QStringLiteral("Intensity"));
        save(w.renderImage(size, 1.5), dir, QStringLiteral("heatmap.png"));
    }
    {   // trend with markers (Measure page, small)
        PlotWidget w;
        const QSize ts(700, 220);
        w.resize(ts);
        w.setLabel(Axis::Bottom, QStringLiteral("time (min)"));
        w.setLabel(Axis::Left, QStringLiteral("band 882 cm⁻¹"));
        w.setHeadroom(0.05);
        Vec t, v;
        std::mt19937 rng(5);
        std::normal_distribution<double> n(0, 40);
        for (int i = 0; i < 40; ++i) {
            t.push_back(i * 0.5);
            v.push_back(6500 - 30 * i + n(rng));
        }
        Curve* c = w.addCurve(t, v, QPen(w.plotPalette().main, 1.5));
        c->setMarker(Marker::Circle, 5);
        save(w.renderImage(ts, 1.5), dir, QStringLiteral("trend.png"));
    }
    {   // 200 000 points: per-column min/max decimation keeps spikes
        PlotWidget w;
        w.resize(size);
        w.setLabel(Axis::Bottom, QStringLiteral("time (s)"));
        w.setLabel(Axis::Left, QStringLiteral("signal"));
        Vec x, y;
        std::mt19937 rng(7);
        std::normal_distribution<double> n(0, 1);
        for (int i = 0; i < 200000; ++i) {
            x.push_back(i * 1e-3);
            y.push_back(std::sin(i * 2e-4) * 5 + n(rng) * 0.3 + (i % 37813 == 0 ? 9 : 0));
        }
        w.addCurve(x, y, QPen(w.plotPalette().main, 1.0));
        save(w.renderImage(size, 1.5), dir, QStringLiteral("decimation.png"));
    }
    {   // status strip in four states, light window background
        QWidget box;
        box.setAutoFillBackground(true);
        auto* v = new QVBoxLayout(&box);
        v->setContentsMargins(0, 0, 0, 8);
        StatusView s;
        auto add = [&](const StatusView& st) {
            auto* strip = new StatusStrip;
            strip->update(st);
            v->addWidget(strip);
        };
        s.connection = QStringLiteral("searching");
        add(s);
        s.connection = QStringLiteral("initialising");
        add(s);
        s = StatusView{QStringLiteral("connected"), false, true, false, false, 532, QStringLiteral("Cora 5000"),
                       QStringLiteral("99002169")};
        add(s);
        s.laserOn = true;
        add(s);
        s = StatusView{QStringLiteral("connected"), true, false, true, false, 1064, {}, {}};
        add(s);
        s.connection = QStringLiteral("lost");
        add(s);
        box.resize(1200, 10);
        box.adjustSize();
        box.resize(1200, box.sizeHint().height());
        box.layout()->activate();
        save(box.grab().toImage(), dir, QStringLiteral("statusstrip.png"));
        ElidedLabel el;
        el.setFullText(QStringLiteral("~/Documents/LibreRamen/2026-11-20/a/very/long/path/to/the/data/folder"));
        el.resize(220, 24);
        save(el.grab().toImage(), dir, QStringLiteral("elidedlabel.png"));
    }
    {   // splash and icon
        Splash sp;
        sp.message(QStringLiteral("Cora 5000 found (S/N 99002169) · initialising… 70 %"));
        sp.setProgress(0.7);
        save(sp.renderImage(), dir, QStringLiteral("splash.png"));
        QImage icons(16 + 32 + 64 + 128 + 256 + 6 * 12, 256 + 24, QImage::Format_ARGB32_Premultiplied);
        icons.fill(QColor("#d9dde3"));
        QPainter p(&icons);
        int xx = 12;
        for (int sz : {16, 32, 64, 128, 256}) {
            p.drawImage(xx, 12 + (256 - sz) / 2, drawIcon(sz));
            xx += sz + 12;
        }
        p.end();
        save(icons, dir, QStringLiteral("icons.png"));
    }
    theme::apply(app, theme::Mode::Dark);
    {   // dark theme: window with strip and plot following the default palette
        QWidget win;
        win.setAutoFillBackground(true);
        auto* v = new QVBoxLayout(&win);
        v->setContentsMargins(0, 0, 0, 0);
        auto* strip = new StatusStrip;
        strip->update(StatusView{QStringLiteral("connected"), true, true, false, false, 532, {}, {}});
        v->addWidget(strip);
        auto* plot = new PlotWidget;
        measurePlot(*plot, kEthanol, 3000);
        v->addWidget(plot, 1);
        win.resize(1200, 640);
        win.layout()->activate();
        save(win.grab().toImage(), dir, QStringLiteral("window-dark.png"));
    }
    return 0;
}
