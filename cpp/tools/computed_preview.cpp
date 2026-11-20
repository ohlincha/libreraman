// Renders the computed-spectra panel (gui/computedpanel) to PNG files for visual checks (not built by
// default: cmake -DLIBRERAMEN_GUI_PREVIEW=ON). Usage: QT_QPA_PLATFORM=offscreen computed_preview [output dir]
// Computed spectra: the synthetic output excerpts in tests/golden/computed; experimental: the spectra the
// simulator was built from (CCl4 and chloroform measured on a real Cora 5000 at 532 nm).
#include "gui/computedpanel.h"
#include "gui/plotwidget.h"
#include "gui/theme.h"

#include "core/calibration.h"
#include "core/fileio.h"
#include "core/simulator.h"

#include <QApplication>
#include <QDir>

#include <cstdio>

using namespace lr::gui;

namespace {

const QString DIR = QStringLiteral(GOLDEN_DIR "/computed/");

lr::fileio::DataSpectrum measured(const QString& sample) {
    const lr::ChannelCal& cal = lr::defaultCalibration().channels.at(532);
    for (const auto& [name, counts] : lr::simulatorData().samples) {
        if (name != sample) continue;
        lr::fileio::DataSpectrum d;
        d.name = name + " (measured, simulator)";
        for (std::size_t p = 0; p < counts.size(); ++p) {
            const double s = cal.shift(double(p));
            if (s < 150 || s > 3300) continue;
            d.x.push_back(s);
            d.y.push_back(counts[p]);
        }
        d.sorted();
        return d;
    }
    qFatal("no simulator sample %s", qPrintable(sample));
}

bool save(const QImage& img, const QString& dir, const QString& name) {
    const QString path = dir + QLatin1Char('/') + name;
    const bool ok = img.save(path);
    std::printf("%s %s\n", ok ? "wrote" : "FAILED", qPrintable(path));
    return ok;
}

}  // namespace

int main(int argc, char** argv) {
    QApplication app(argc, argv);
    const QString dir = argc > 1 ? QString::fromLocal8Bit(argv[1]) : QStringLiteral("gui-preview");
    QDir().mkpath(dir);

    for (const bool dark : {false, true}) {
        theme::apply(app, dark ? theme::Mode::Dark : theme::Mode::Light);
        const QString sfx = dark ? QStringLiteral("-dark") : QString();
        ComputedPanel p;
        p.setMessagesEnabled(false);
        p.setAttribute(Qt::WA_DontShowOnScreen);
        p.resize(1400, 900);
        p.show();
        p.loadFile(DIR + "gaussian_ccl4.log");
        p.loadFile(DIR + "orca4_ccl4_twojobs.out");
        p.setChecked(1, false);
        p.setCurrentRow(0);
        p.setExperimental(measured("carbon tetrachloride"));
        QString err;
        if (!p.fitScaling(&err)) std::printf("fit failed: %s\n", qPrintable(err));
        p.plot()->setXRange(150, 900);
        QApplication::processEvents();
        save(p.grab().toImage(), dir, "computed-ccl4" + sfx + ".png");
        if (dark) continue;

        p.setPlotLayout(ComputedPanel::Layout::Mirror);
        QApplication::processEvents();
        save(p.grab().toImage(), dir, "computed-ccl4-mirror.png");

        ComputedPanel q;
        q.setMessagesEnabled(false);
        q.setAttribute(Qt::WA_DontShowOnScreen);
        q.resize(1400, 900);
        q.show();
        q.loadFile(DIR + "gaussian_chcl3_hpmodes.out");
        q.loadFile(DIR + "orca5_chcl3.out");
        q.loadFile(DIR + "cp2k_chcl3_vib.out");
        q.loadFile(DIR + "gaussian_ccl4.log");
        q.setExperimental(measured("chloroform"));
        q.setTwoRegion(true, 2000);                  // C-H stretch scaled separately
        const auto scores = q.compareStructures(&err);
        for (const auto& s : scores)
            std::printf("  r = %.3f  %s  scale %s\n", s.correlation, qPrintable(q.calculation(s.index).name()),
                        qPrintable(s.scaling.text()));
        q.setCurrentRow(scores.empty() ? 0 : scores.front().index);
        q.setChecked(2, false);
        QApplication::processEvents();
        save(q.grab().toImage(), dir, "computed-chcl3-compare.png");
    }
    return 0;
}
