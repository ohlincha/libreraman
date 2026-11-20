// Widget tests for the computed-spectra panel (gui/computedpanel): loading output files and folders,
// scaling fit against an experimental spectrum, plot items per layout, assignment table, comparison and
// export (files re-readable with fileio::read). Runs offscreen.
#include "gui/computedpanel.h"
#include "gui/plotwidget.h"

#include "core/computed.h"
#include "core/fileio.h"

#include <QApplication>
#include <QCheckBox>
#include <QFileInfo>
#include <QListWidget>
#include <QSignalSpy>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTest>

#include <cmath>
#include <random>

using namespace lr::gui;
using lr::fileio::DataSpectrum;

namespace {

const QString DIR = QStringLiteral(GOLDEN_DIR "/computed/");

// Experimental-like CCl4: the Gaussian bands scaled by f, Lorentzians on a sloping background, noise.
DataSpectrum experimental(double f) {
    const auto calc = lr::computed::read(DIR + "gaussian_ccl4.log").front();
    lr::computed::Options o;
    o.scaling.low = f;
    const auto b = lr::computed::bands(calc, o);
    const double m = lr::computed::maxValue(b);
    std::mt19937 rng(7);
    std::normal_distribution<double> noise(0, 0.003);
    DataSpectrum d;
    d.name = "CCl4 measured";
    for (double x = 150; x <= 1200; x += 0.5) {
        double y = 0.2 + 0.0001 * x;
        for (const auto& band : b) y += band.value / m / (1 + std::pow((x - band.position) / 4.0, 2));
        d.x.push_back(x);
        d.y.push_back(y + noise(rng));
    }
    return d;
}

int curveCount(PlotWidget* w) {
    int n = 0;
    for (PlotItem* it : w->items())
        if (qobject_cast<Curve*>(it)) ++n;
    return n;
}

template <class T> int itemCount(PlotWidget* w) {
    int n = 0;
    for (PlotItem* it : w->items())
        if (qobject_cast<T*>(it)) ++n;
    return n;
}

}  // namespace

class GuiTestComputedPanel : public QObject {
    Q_OBJECT
private slots:
    void loadAndShow() {
        ComputedPanel p;
        p.setMessagesEnabled(false);
        p.resize(1200, 800);
        p.show();
        QVERIFY(p.infoText().contains("Open the output"));
        QVERIFY(p.loadFile(DIR + "gaussian_ccl4.log"));
        QCOMPARE(p.count(), 1);
        QCOMPARE(p.currentRow(), 0);
        QVERIFY(p.isChecked(0));
        QVERIFY(p.list()->item(0)->text().contains("CCl4 B3LYP/6-31G(d)"));
        QVERIFY(p.infoText().contains("Gaussian 16"));
        QVERIFY(p.infoText().contains("9 modes, 4 bands"));
        // computed curve + sticks, no experimental yet
        QCOMPARE(curveCount(p.plot()), 2);
        QCOMPARE(p.assignmentTable()->rowCount(), 4);         // computed bands only

        // two jobs in one file: two structures, the first of the new file becomes current
        QVERIFY(p.loadFile(DIR + "orca4_ccl4_twojobs.out"));
        QCOMPARE(p.count(), 3);
        QCOMPARE(p.currentRow(), 1);
        QCOMPARE(curveCount(p.plot()), 4);                   // three computed + sticks
        p.setChecked(2, false);
        QCOMPARE(curveCount(p.plot()), 3);

        // errors
        QSignalSpy spy(&p, &ComputedPanel::message);
        QString err;
        QVERIFY(!p.loadFile(DIR + "not_an_output.txt", &err));
        QVERIFY(err.contains("not a recognised"));
        QCOMPARE(spy.count(), 1);
        QCOMPARE(p.count(), 3);

        // warnings shown for the current structure
        QVERIFY(p.loadFile(DIR + "cp2k_chcl3_vib.out"));
        QVERIFY(p.infoText().contains("imaginary"));
        QVERIFY(p.list()->item(3)->text().startsWith(QStringLiteral("⚠")));

        p.removeRow(3);
        QCOMPARE(p.count(), 3);
        p.clear();
        QCOMPARE(p.count(), 0);
        QCOMPARE(curveCount(p.plot()), 0);
    }

    void fitAndAssign() {
        ComputedPanel p;
        p.setMessagesEnabled(false);
        p.resize(1200, 800);
        p.show();
        QVERIFY(p.loadFile(DIR + "gaussian_ccl4.log"));
        QString err;
        QVERIFY(!p.fitScaling(&err));                         // no experimental spectrum yet
        QVERIFY(err.contains("experimental"));
        p.setExperimental(experimental(0.975));
        QVERIFY(p.hasExperimental());
        QCOMPARE(int(p.experimentalPeaks().size()), 4);
        QVERIFY2(p.fitScaling(&err), qPrintable(err));
        QVERIFY2(std::abs(p.scaling(0).low - 0.975) < 1e-3, qPrintable(QString::number(p.scaling(0).low)));
        QCOMPARE(p.options().scaling.low, p.scaling(0).low);
        // assignment: four matched rows
        const auto rows = p.assignment();
        QCOMPARE(int(rows.size()), 4);
        for (const auto& r : rows) {
            QVERIFY(r.band >= 0 && r.peak >= 0);
            QVERIFY(std::abs(r.delta) < 1.0);
        }
        QCOMPARE(p.assignmentTable()->rowCount(), 4);
        QCOMPARE(p.assignmentTable()->item(2, 2)->text(), QString("A1"));
        // stacked: computed, sticks, experimental, connectors + peak labels
        QCOMPARE(curveCount(p.plot()), 4);
        QCOMPARE(itemCount<PeakLabels>(p.plot()), 1);
        p.setPlotLayout(ComputedPanel::Layout::Mirror);
        QCOMPARE(curveCount(p.plot()), 3);                   // no connectors
        QCOMPARE(itemCount<InfiniteLine>(p.plot()), 1);      // zero line
        bool negative = false;
        for (PlotItem* it : p.plot()->items())
            if (auto* c = qobject_cast<Curve*>(it))
                for (double v : c->yData())
                    if (v < -0.5) negative = true;
        QVERIFY(negative);
        p.setPlotLayout(ComputedPanel::Layout::Overlay);
        p.setSticksVisible(false);
        QCOMPARE(curveCount(p.plot()), 2);

        // manual scaling, two regions
        lr::computed::Scaling s;
        s.low = 0.99;
        s.high = 0.96;
        s.split = 2000;
        p.setScaling(0, s);
        QVERIFY(p.findChild<QCheckBox*>() != nullptr);
        QCOMPARE(p.scaling(0).high, 0.96);
        QVERIFY(p.scaling(0).twoRegion());
        p.setTwoRegion(false);
        QVERIFY(!p.scaling(0).twoRegion());

        // settings reach the options
        p.setLaser(785);
        p.setTemperature(77);
        p.setQuantity(lr::computed::Quantity::Activity);
        QCOMPARE(p.options().laserNm, 785.0);
        QCOMPARE(p.options().temperatureK, 77.0);
        QVERIFY(p.options().quantity == lr::computed::Quantity::Activity);
        lr::computed::Broadening br;
        br.shape = lr::computed::Shape::Gaussian;
        br.fwhm = 12;
        p.setBroadening(br);
        QVERIFY(p.broadening().shape == lr::computed::Shape::Gaussian);
        QCOMPARE(p.broadening().fwhm, 12.0);
        // grid follows the experimental range
        const auto g = p.grid();
        QCOMPARE(g.front(), 150.0);
        QCOMPARE(g.back(), 1200.0);
        p.clearExperimental();
        QVERIFY(!p.hasExperimental());
        QVERIFY(p.grid().back() > 900);
    }

    void folderCompareExport() {
        ComputedPanel p;
        p.setMessagesEnabled(false);
        p.resize(1200, 800);
        p.show();
        QSignalSpy spy(&p, &ComputedPanel::message);
        const int n = p.loadFolder(DIR);
        QCOMPARE(n, 15);                                      // 15 files, one without frequencies, one with 2 jobs
        QCOMPARE(spy.count(), 1);                             // gaussian_opt_only.log reported
        QVERIFY(spy.first().at(1).toString().contains("gaussian_opt_only.log"));

        p.setExperimental(experimental(1.0));
        QString err;
        const auto s = p.compareStructures(&err);
        QCOMPARE(int(s.size()), 15);
        // a CCl4 calculation wins (vasp_raman.py output has no atoms, hence the file name)
        const QString best = QFileInfo(p.calculation(s.front().index).source).fileName();
        QVERIFY2(best.contains("ccl4"), qPrintable(best));
        QVERIFY(s.front().correlation > 0.9);
        QVERIFY(p.list()->item(s.front().index)->text().startsWith("r 0.9"));

        p.setCurrentRow(s.front().index);
        QTemporaryDir tmp;
        QVERIFY(p.exportSpectrum(tmp.filePath("c.csv"), &err));
        QVERIFY(p.exportSpectrum(tmp.filePath("c.jdx"), &err));
        const auto csv = lr::fileio::read(tmp.filePath("c.csv"));
        QCOMPARE(int(csv.size()), 1);
        QCOMPARE(csv[0].x.front(), 150.0);
        QCOMPARE(int(lr::fileio::read(tmp.filePath("c.jdx")).front().x.size()), int(csv[0].x.size()));
        QVERIFY(p.exportAssignment(tmp.filePath("a.csv"), &err));
        QVERIFY(QFileInfo(tmp.filePath("a.csv")).size() > 50);
        QVERIFY(p.exportPlot(tmp.filePath("plot.png")));
        QVERIFY(p.exportPlot(tmp.filePath("plot.svg")));
        QVERIFY(QFileInfo(tmp.filePath("plot.png")).size() > 1000);
        QVERIFY(!p.exportAssignment(tmp.filePath("missing/dir/a.csv"), &err));
    }
};

QTEST_MAIN(GuiTestComputedPanel)
#include "gui_test_computedpanel.moc"
