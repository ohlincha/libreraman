// Renders the Data area (DataPage) to PNG files for visual checks (not built by default:
// cmake -DLIBRERAMEN_GUI_PREVIEW=ON). Usage: QT_QPA_PLATFORM=offscreen datapage_preview [output dir]
// Writes synthetic spectra as LibreRamen CSV files into a temporary folder and opens them like a user.
#include "gui/datapage.h"
#include "gui/plotwidget.h"
#include "gui/theme.h"

#include "core/fileio.h"

#include <QApplication>
#include <QCheckBox>
#include <QEventLoop>
#include <QTimer>
#include <QDir>
#include <QTableWidget>
#include <QTemporaryDir>

#include <cmath>
#include <cstdio>
#include <random>

using namespace lr::gui;
using lr::Vec;

namespace {

struct Band {
    double pos, height, width;
};

// Raman-like raw spectrum (fluorescence background, Lorentzian bands, shot noise) on 1800 points.
lr::fileio::DataSpectrum spectrum(const QString& name, const std::vector<Band>& bands, double bg, unsigned seed,
                                  double scale = 1.0) {
    std::mt19937 rng(seed);
    std::normal_distribution<double> noise(0, 1);
    lr::fileio::DataSpectrum d;
    d.name = name;
    for (int i = 0; i < 1800; ++i) {
        const double s = 120 + i * (3300.0 - 120) / 1799;
        const double b = bg * (0.5 + 0.5 * std::exp(-std::pow((s - 1400) / 1600, 2)));
        double v = b;
        for (const Band& k : bands) v += scale * k.height / (1 + std::pow((s - k.pos) / k.width, 2));
        d.x.push_back(s);
        d.y.push_back(v + noise(rng) * std::sqrt(std::max(v, 1.0)) * 0.5);
    }
    d.meta = {{"sample", name}, {"channel", "532 nm"}};
    return d;
}

const std::vector<Band> kEthanol = {{433, 1800, 9},  {882, 6500, 7},   {1052, 2600, 8}, {1096, 2300, 8},
                                    {1275, 1100, 8}, {1454, 3200, 10}, {2876, 5200, 14}, {2929, 7400, 13},
                                    {2973, 6100, 12}};
const std::vector<Band> kCCl4 = {{218, 6000, 5}, {314, 8200, 6}, {459, 21000, 3.5}, {762, 3400, 5}, {791, 3600, 5}};
const std::vector<Band> kChloroform = {{262, 5200, 5}, {366, 4800, 5}, {668, 9800, 4}, {761, 3300, 6},
                                       {1216, 1500, 6}, {3019, 4200, 8}};
const std::vector<Band> kCyclohexane = {{384, 1400, 6},  {426, 1900, 6},  {801, 9000, 4},  {1028, 3600, 5},
                                        {1157, 1200, 6}, {1266, 3000, 7}, {1444, 2600, 8}, {2853, 7200, 10},
                                        {2923, 5800, 12}, {2938, 6500, 10}};

bool save(const QImage& img, const QString& dir, const QString& name) {
    const QString path = dir + QLatin1Char('/') + name;
    const bool ok = img.save(path);
    std::printf("%s %s\n", ok ? "wrote" : "FAILED", qPrintable(path));
    return ok;
}

QStringList writeFiles(const QString& dir) {
    QStringList paths;
    auto put = [&](const lr::fileio::DataSpectrum& d) {
        const QString p = dir + "/" + d.name + ".csv";
        lr::fileio::writeCsv(p, d);
        paths << p;
    };
    put(spectrum("ethanol", kEthanol, 2600, 1));
    put(spectrum("cyclohexane", kCyclohexane, 1800, 2));
    put(spectrum("chloroform", kChloroform, 2200, 3));
    put(spectrum("carbon tetrachloride", kCCl4, 1500, 4));
    return paths;
}

void addSeries(DataPage& page) {             // reaction: ethanol band decays, cyclohexane grows
    for (int k = 0; k < 14; ++k) {
        const double f = k / 13.0;
        auto a = spectrum("", kEthanol, 2400 + 60 * k, 100 + unsigned(k), 1 - 0.8 * f);
        auto b = spectrum("", kCyclohexane, 0, 200 + unsigned(k), 0.15 + 0.85 * f);
        lr::fileio::DataSpectrum d = a;
        for (std::size_t i = 0; i < d.y.size(); ++i) d.y[i] += b.y[i];
        d.name = QStringLiteral("reaction t=%1 min").arg(k * 5);
        page.addSpectrum(d, true);
    }
}

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    const QString dir = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QStringLiteral("gui-preview");
    QDir().mkpath(dir);
    QTemporaryDir tmp;
    const QStringList files = writeFiles(tmp.path());

    for (const bool dark : {false, true}) {
        theme::apply(app, dark ? theme::Mode::Dark : theme::Mode::Light);
        const QString sfx = dark ? QStringLiteral("-dark") : QString();
        DataPage page;
        page.setAttribute(Qt::WA_DontShowOnScreen);
        page.resize(1330, 800);
        page.show();
        page.openFiles(files);
        page.setCurrentRow(0);
        QApplication::processEvents();
        save(page.grab().toImage(), dir, "datapage-overlay" + sfx + ".png");
        if (dark) continue;

        page.setView(DataPage::View::Stacked);
        QApplication::processEvents();
        save(page.grab().toImage(), dir, "datapage-stacked.png");

        page.setView(DataPage::View::Overlay);
        page.setTicked(2, false);
        page.setTicked(3, false);
        page.setNormalisation("max");
        page.setDifference(true);
        QApplication::processEvents();
        save(page.grab().toImage(), dir, "datapage-difference.png");
        page.setDifference(false);
        page.setNormalisation("none");

        page.setCurrentRow(0);
        page.setTicked(1, false);
        page.plot()->setXRange(780, 1180);
        QString err;
        if (!page.fitView(&err)) std::printf("fit failed: %s\n", qPrintable(err));
        QApplication::processEvents();
        save(page.grab().toImage(), dir, "datapage-fit.png");

        page.clear();
        addSeries(page);
        page.setView(DataPage::View::HeatMap);
        QApplication::processEvents();
        page.heatPlot()->setXRange(650, 1550);   // narrow bands: zoom in to see them change
        QApplication::processEvents();
        save(page.grab().toImage(), dir, "datapage-heatmap.png");

        // waterfall: stacked + angle, one colour; full range and zoomed into a narrow region
        page.setView(DataPage::View::Stacked);
        page.setOffset(25);
        page.setAngle(35);
        for (QCheckBox* c : page.findChildren<QCheckBox*>())
            if (c->text() == QStringLiteral("one colour")) c->setChecked(true);
        page.plot()->autoRange();
        QApplication::processEvents();
        save(page.grab().toImage(), dir, "datapage-waterfall.png");
        page.plot()->setXRange(780, 1150);
        {   // the shift follows the zoom after a short delay
            QEventLoop loop;
            QTimer::singleShot(300, &loop, &QEventLoop::quit);
            loop.exec();
        }
        QApplication::processEvents();
        save(page.grab().toImage(), dir, "datapage-waterfall-zoom.png");
    }
    return 0;
}
