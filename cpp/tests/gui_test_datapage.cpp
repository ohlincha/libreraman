// Widget tests for the Data area (gui/datapage): opening the golden sample files, views (overlay,
// stacked, heat map), normalisation, difference, calculator, peak table, fitting, list management and
// export (files re-readable with fileio::read). Runs offscreen.
#include "gui/datapage.h"
#include "gui/page.h"
#include "gui/datapagehost.h"
#include "gui/plotwidget.h"
#include "gui/theme.h"

#include "core/fileio.h"
#include "core/carbon.h"
#include "core/peaks.h"

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDialog>
#include <QDir>
#include <QFileInfo>
#include <QListWidget>
#include <QMenu>
#include <QPushButton>
#include <QSettings>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QStackedWidget>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTest>

#include <array>
#include <cmath>
#include <random>

using namespace lr::gui;
using lr::Vec;
using lr::fileio::DataSpectrum;

namespace {

const QString FILES = QStringLiteral(GOLDEN_DIR "/files");

// Baseline-free synthetic spectrum with Lorentzian bands (and a little noise).
DataSpectrum synth(const QString& name, std::vector<std::array<double, 3>> bands, double offset = 0, unsigned seed = 1,
                   double x0 = 150, double x1 = 1850) {
    std::mt19937 rng(seed);
    std::normal_distribution<double> noise(0, 2);
    DataSpectrum d;
    d.name = name;
    for (int i = 0; i < 1200; ++i) {
        const double x = x0 + i * (x1 - x0) / 1199;
        double y = offset;
        for (const auto& b : bands) y += b[1] / (1 + std::pow((x - b[0]) / (b[2] / 2), 2));
        d.x.push_back(x);
        d.y.push_back(y + noise(rng));
    }
    return d;
}

std::vector<Curve*> curves(PlotWidget* w) {
    std::vector<Curve*> out;
    for (PlotItem* it : w->items())
        if (auto* c = qobject_cast<Curve*>(it); c && c->lineVisible()) out.push_back(c);   // not peak markers
    return out;
}

template <class T> T* firstItem(PlotWidget* w) {
    for (PlotItem* it : w->items())
        if (auto* c = qobject_cast<T*>(it)) return c;
    return nullptr;
}

double maxOf(const Vec& v) {
    double m = -1e300;
    for (double a : v) m = std::max(m, a);
    return m;
}

// The page without baseline correction so that values can be checked against the raw data.
void plain(DataPage& p) {
    p.setMessagesEnabled(false);
    p.setBaselineCorrection(false);
}

}  // namespace

class GuiTestDataPage : public QObject {
    Q_OBJECT
private slots:
    void initTestCase();
    void init();
    void openGoldenFiles();
    void openFolderAndErrors();
    void listManagement();
    void measuredSpectrum();
    void overlayAndStacked();
    void heatMap();
    void normalisation();
    void difference();
    void calculator();
    void peakTable();
    void fitting();
    void fitComponents();
    void carbonDG();
    void peakPicking();
    void peakClicks();
    void addToLibrary();
    void preview();
    void exportFiles();
    void processingDescription();
    void baselineSettingsRemembered();
    void addToLibraryChoice();
};

void GuiTestDataPage::initTestCase() {
    QStandardPaths::setTestModeEnabled(true);          // QSettings away from the user's configuration
    QCoreApplication::setOrganizationName(QStringLiteral("LibreRamanTest"));   // Windows/macOS: QSettings
    QCoreApplication::setApplicationName(QStringLiteral("gui_test_datapage")); // needs one to store anything
    theme::apply(*qApp, theme::Mode::Light);
    QVERIFY(QDir(FILES).exists());
}

void GuiTestDataPage::init() { QSettings().remove(QStringLiteral("data")); }   // each test starts at defaults

void GuiTestDataPage::openGoldenFiles() {
    DataPage p;
    p.setMessagesEnabled(false);
    p.resize(1200, 760);
    int expected = 0;
    QStringList good;
    for (const QString& f : QDir(FILES).entryList(QDir::Files, QDir::Name)) {
        try {
            expected += int(lr::fileio::read(FILES + "/" + f).size());
            good << FILES + "/" + f;
        } catch (const std::exception&) {
        }
    }
    QVERIFY(expected >= 15);
    QSignalSpy changed(&p, &DataPage::spectraChanged);
    const QStringList errors = p.openFiles(good);
    QVERIFY2(errors.isEmpty(), qPrintable(errors.join('\n')));
    QCOMPARE(p.count(), expected);
    QVERIFY(changed.count() >= expected);
    // Python: the first 12 are ticked, later ones not
    QCOMPARE(int(p.tickedRows().size()), std::min(expected, 12));
    QCOMPARE(p.currentRow(), expected - 1);
    QCOMPARE(int(curves(p.plot()).size()), std::min(expected, 12));
    // names and tooltips from the files
    QCOMPARE(p.list()->item(0)->text(), p.spectrum(0).name);
    QVERIFY(p.list()->item(0)->toolTip().endsWith(QFileInfo(good[0]).fileName()));
    // every view draws without throwing
    for (auto v : {DataPage::View::Stacked, DataPage::View::HeatMap, DataPage::View::Overlay}) {
        p.setView(v);
        QVERIFY(!p.grab().isNull());
    }
}

void GuiTestDataPage::openFolderAndErrors() {
    DataPage p;
    p.setMessagesEnabled(false);
    QSignalSpy msg(&p, &DataPage::message);
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    QDir(tmp.path()).mkpath("sub/deeper");
    QFile::copy(FILES + "/own.csv", tmp.path() + "/b.csv");
    QFile::copy(FILES + "/vendor.txt", tmp.path() + "/a.TXT");
    QFile::copy(FILES + "/points.jdx", tmp.path() + "/sub/deeper/c.jdx");
    QFile::copy(FILES + "/even.spc", tmp.path() + "/sub/ignored.dat");     // not a folder suffix
    QFile::copy(FILES + "/bad_own.csv", tmp.path() + "/sub/bad.csv");
    const QStringList errors = p.openFiles({tmp.path()});
    QCOMPARE(errors.size(), 1);
    QVERIFY(errors[0].startsWith("bad.csv: "));
    QCOMPARE(msg.count(), 1);
    QCOMPARE(msg[0][0].toString(), QStringLiteral("Open spectra"));
    QCOMPARE(p.count(), 3);
    // files of a folder (sorted) before its subfolders
    QVERIFY(p.spectrum(0).source.endsWith("a.TXT"));
    QVERIFY(p.spectrum(1).source.endsWith("b.csv"));
    QVERIFY(p.spectrum(2).source.endsWith("c.jdx"));
    QVERIFY(!p.openFiles({FILES + "/does-not-exist.csv"}).isEmpty());
}

void GuiTestDataPage::listManagement() {
    DataPage p;
    plain(p);
    for (int k = 0; k < 4; ++k) p.addSpectrum(synth(QString("s%1").arg(k), {{{500.0 + 100 * k, 1000, 10}}}));
    QCOMPARE(p.count(), 4);
    QCOMPARE(int(curves(p.plot()).size()), 4);
    // automatic colours follow the ticked position; custom colours stick
    const PlotPalette& pal = p.plot()->plotPalette();
    QCOMPARE(p.colorOf(2), pal.seriesColor(2));
    p.setTicked(0, false);
    QCOMPARE(p.colorOf(0), QColor());
    QCOMPARE(p.colorOf(2), pal.seriesColor(1));
    QCOMPARE(int(curves(p.plot()).size()), 3);
    p.setColor(3, QColor("#ff0000"));
    QCOMPARE(p.colorOf(3), QColor("#ff0000"));
    QCOMPARE(curves(p.plot())[2]->pen().color(), QColor("#ff0000"));
    // rename via the list item (as an edit would do) -> legend name
    p.rename(1, "renamed");
    QCOMPARE(p.spectrum(1).name, QStringLiteral("renamed"));
    QCOMPARE(curves(p.plot())[0]->name(), QStringLiteral("renamed"));
    p.rename(1, "   ");                            // empty: keeps the old name
    QCOMPARE(p.spectrum(1).name, QStringLiteral("renamed"));
    QCOMPARE(p.list()->item(1)->text(), QStringLiteral("renamed"));
    // check all / none
    p.checkAll(false);
    QVERIFY(p.tickedRows().empty());
    QVERIFY(curves(p.plot()).empty());
    p.checkAll(true);
    QCOMPARE(int(p.tickedRows().size()), 4);
    // remove selected
    p.list()->clearSelection();
    p.list()->item(0)->setSelected(true);
    p.list()->item(2)->setSelected(true);
    p.removeSelected();
    QCOMPARE(p.count(), 2);
    QCOMPARE(p.spectrum(0).name, QStringLiteral("renamed"));
    QCOMPARE(p.spectrum(1).name, QStringLiteral("s3"));
    QCOMPARE(p.list()->count(), 2);
    p.clear();
    QCOMPARE(p.count(), 0);
    QVERIFY(p.plot()->items().empty());
    QCOMPARE(p.peakTable()->rowCount(), 0);
}

void GuiTestDataPage::measuredSpectrum() {
    DataPage p;
    plain(p);
    lr::Spectrum sp;
    const DataSpectrum s = synth("x", {{{800, 500, 12}}});
    sp.shift = s.x;
    sp.intensity = s.y;
    sp.t = QDateTime(QDate(2026, 11, 20), QTime(14, 5, 9));
    sp.meta = {{"sample", "ethanol"}, {"series_index", "3"}};
    p.addSpectrum(sp);
    QCOMPARE(p.count(), 1);
    QCOMPARE(p.spectrum(0).name, QStringLiteral("ethanol 14:05:09 #3"));
    QVERIFY(p.isTicked(0));
    QCOMPARE(p.list()->item(0)->toolTip(), QStringLiteral("measured in this session"));
    QCOMPARE(p.spectrum(0).metaValue("channel"), QStringLiteral("532 nm"));
    sp.meta.clear();
    p.addSpectrum(sp);
    QCOMPARE(p.spectrum(1).name, QStringLiteral("spectrum 14:05:09"));
}

void GuiTestDataPage::overlayAndStacked() {
    DataPage p;
    plain(p);
    p.addSpectrum(synth("a", {{{500, 1000, 10}}}, 0, 1));
    p.addSpectrum(synth("b", {{{900, 400, 10}}}, 0, 2));
    p.addSpectrum(synth("c", {{{1300, 700, 10}}}, 0, 3));
    QCOMPARE(p.plot()->headroom(), 0.08);
    QVERIFY(p.plot()->autoY());
    auto cs = curves(p.plot());
    QCOMPARE(int(cs.size()), 3);
    QCOMPARE(cs[0]->yData(), p.spectrum(0).y);           // overlay: no offset, no processing here
    QCOMPARE(cs[1]->name(), QStringLiteral("b"));

    p.setView(DataPage::View::Stacked);
    p.setOffset(50);
    cs = curves(p.plot());
    QCOMPARE(int(cs.size()), 3);
    // step = largest (99.5th - 1st percentile) span * 50 %
    double step = cs[1]->yData()[0] - p.spectrum(1).y[0];
    QVERIFY(step > 0);
    QVERIFY(std::abs(cs[2]->yData()[0] - p.spectrum(2).y[0] - 2 * step) < 1e-9);
    QVERIFY(step > 0.5 * 0.5 * 1000 && step < 0.5 * 1000);
    p.setOffset(0);
    QCOMPARE(curves(p.plot())[2]->yData(), p.spectrum(2).y);
    // intensity range follows the visible x range
    p.setView(DataPage::View::Overlay);
    p.resize(1000, 700);
    p.plot()->setXRange(1200, 1400);
    const lr::gui::Range y = p.plot()->yRange();
    QVERIFY(y.hi > 700 && y.hi < 900);
    p.plot()->setXRange(400, 600);
    QVERIFY(p.plot()->yRange().hi > 1000);
}

void GuiTestDataPage::heatMap() {
    DataPage p;
    plain(p);
    for (int k = 0; k < 5; ++k) p.addSpectrum(synth(QString("t%1").arg(k), {{{700, 100.0 * (k + 1), 10}}}, 0, k + 1u,
                                                    150 + k, 1850 - k));
    p.setView(DataPage::View::HeatMap);
    auto* stack = p.findChild<QStackedWidget*>();
    QVERIFY(stack);
    QCOMPARE(stack->currentWidget(), static_cast<QWidget*>(p.heatPlot()));
    auto* hm = firstItem<HeatMapItem>(p.heatPlot());
    QVERIFY(hm);
    QVERIFY(!hm->image().isNull());
    QCOMPARE(hm->rows(), 5);
    QCOMPARE(hm->cols(), 1200);
    QVERIFY(hm->colorBarVisible());
    // common x range (overlap of all spectra)
    QCOMPARE(hm->xBounds().lo, 154.0);
    QCOMPARE(hm->xBounds().hi, 1846.0);
    QCOMPARE(hm->yBounds(0, 1).hi, 5.0);
    QVERIFY(std::abs(hm->levels().hi - 500) < 30);
    QCOMPARE(p.heatPlot()->label(Axis::Left), QStringLiteral("spectrum #"));
    QVERIFY(!p.heatPlot()->renderImage(QSize(600, 400)).isNull());
    // difference overrides the heat map and shows the plot
    p.setDifference(true);
    QCOMPARE(stack->currentWidget(), static_cast<QWidget*>(p.plot()));
    p.setDifference(false);
    p.checkAll(false);
    QVERIFY(!firstItem<HeatMapItem>(p.heatPlot()));
}

void GuiTestDataPage::normalisation() {
    DataPage p;
    plain(p);
    p.addSpectrum(synth("a", {{{500, 1000, 10}}}, 0, 1));
    p.addSpectrum(synth("b", {{{900, 300, 10}}}, 0, 2));
    p.setNormalisation("max");
    for (Curve* c : curves(p.plot())) QVERIFY(std::abs(maxOf(c->yData()) - 1.0) < 1e-12);
    QCOMPARE(p.settings().normalize, QStringLiteral("max"));
    p.setNormalisation("area");
    for (Curve* c : curves(p.plot())) {
        const Vec& x = c->xData();
        const Vec& y = c->yData();
        double a = 0;
        for (std::size_t i = 1; i < x.size(); ++i) a += 0.5 * (std::abs(y[i]) + std::abs(y[i - 1])) * (x[i] - x[i - 1]);
        QVERIFY2(std::abs(a - 1.0) < 1e-9, qPrintable(QString::number(a)));
    }
    // baseline correction uses the Measure page's method
    p.setNormalisation("none");
    p.setProcessingSettings([] {
        lr::processing::Settings s;
        s.baseline = "polynomial";
        s.strength = 3;
        return s;
    });
    QCOMPARE(p.settings().baseline, QStringLiteral("none"));       // switched off on the page
    p.setBaselineCorrection(true);
    QCOMPARE(p.settings().baseline, QStringLiteral("polynomial"));
    QCOMPARE(p.settings().strength, 3.0);
    // a method chosen on the page overrides the Measure page's, with the page's strength
    p.setBaselineMethod("als");
    p.setBaselineStrength(7.5);
    QCOMPARE(p.settings().baseline, QStringLiteral("als"));
    QCOMPARE(p.settings().strength, 7.5);
    QVERIFY(p.baselineCorrection());
    p.setBaselineCorrection(false);
    QCOMPARE(p.settings().baseline, QStringLiteral("none"));
    p.setBaselineCorrection(true);                                 // back to the last method
    QCOMPARE(p.baselineMethod(), QStringLiteral("als"));
    p.setBaselineMethod("measure");
    QCOMPARE(p.settings().baseline, QStringLiteral("polynomial"));
}

void GuiTestDataPage::difference() {
    DataPage p;
    plain(p);
    DataSpectrum a = synth("A", {{{500, 1000, 10}}}, 0, 1);
    DataSpectrum b = synth("B", {{{500, 600, 10}}}, 0, 2, 140, 1860);
    p.addSpectrum(a);
    p.addSpectrum(b);
    p.addSpectrum(synth("C", {{{800, 100, 10}}}, 0, 3));
    p.setDifference(true);
    QVERIFY(p.difference());
    auto cs = curves(p.plot());
    QCOMPARE(int(cs.size()), 1);
    QCOMPARE(cs[0]->name(), QStringLiteral("A − B"));
    QVERIFY(firstItem<InfiniteLine>(p.plot()));
    // y = A - interp(B) at A's x
    const Vec& yd = cs[0]->yData();
    QCOMPARE(yd.size(), a.x.size());
    for (std::size_t i : {std::size_t(0), std::size_t(300), std::size_t(1199)}) {
        const double xq = a.x[i];
        const auto j = std::size_t(std::upper_bound(b.x.begin(), b.x.end(), xq) - b.x.begin());
        const double yb = b.y[j - 1] + (b.y[j] - b.y[j - 1]) * (xq - b.x[j - 1]) / (b.x[j] - b.x[j - 1]);
        QVERIFY(std::abs(yd[i] - (a.y[i] - yb)) < 1e-9);
    }
    const auto peak = std::max_element(yd.begin(), yd.end());
    QVERIFY(std::abs(*peak - 400) < 20);
    // fewer than two ticked: normal overlay
    p.setTicked(1, false);
    p.setTicked(2, false);
    QCOMPARE(curves(p.plot())[0]->name(), QStringLiteral("A"));
}

void GuiTestDataPage::calculator() {
    DataPage p;
    plain(p);
    DataSpectrum a = synth("A", {{{500, 1000, 10}}}, 0, 1);
    DataSpectrum b = synth("B", {{{500, 600, 10}}}, 0, 2);
    p.addSpectrum(a);
    p.addSpectrum(b);
    const int r = p.calculate(0, 2.0, 1, -0.5, 3.0);
    QCOMPARE(r, 2);
    QCOMPARE(p.count(), 3);
    const DataSpectrum& c = p.spectrum(2);
    QCOMPARE(c.name, QStringLiteral("2·[A] − 0.5·[B] + 3"));
    QCOMPARE(c.metaValue("calculated"), c.name);
    QVERIFY(c.metaValue("processing").startsWith("baseline=none "));
    for (std::size_t i = 0; i < a.x.size(); i += 97) QVERIFY(std::abs(c.y[i] - (2 * a.y[i] - 0.5 * b.y[i] + 3)) < 1e-9);
    QVERIFY(p.isTicked(2));
    QCOMPARE(p.currentRow(), 2);
    // without B, negative offset, no offset
    p.calculate(1, 1.5, -1, 0, -2.25);
    QCOMPARE(p.spectrum(3).name, QStringLiteral("1.5·[B] − 2.25"));
    p.calculate(0, 1, 1, 1, 0);
    QCOMPARE(p.spectrum(4).name, QStringLiteral("1·[A] + 1·[B]"));
    QCOMPARE(p.calculate(9, 1, -1, 0, 0), -1);
}

void GuiTestDataPage::peakTable() {
    DataPage p;
    plain(p);
    const DataSpectrum s = synth("bands", {{{400, 800, 12}}, {{1000, 1500, 10}}, {{1600, 500, 14}}}, 0, 5);
    p.addSpectrum(s);
    p.addSpectrum(synth("other", {{{700, 300, 10}}}, 0, 6));
    QTableWidget* t = p.peakTable();
    QCOMPARE(t->columnCount(), 7);   // + possible assignment
    QCOMPARE(t->horizontalHeaderItem(0)->text(), QStringLiteral("position (cm⁻¹)"));
    QCOMPARE(t->rowCount(), 1);                        // current = last added
    p.setCurrentRow(0);
    const auto expected = lr::peaks::pick(s.x, s.y, 8, 2.0, false, 150.0);
    QCOMPARE(t->rowCount(), int(expected.size()));
    QCOMPARE(t->rowCount(), 3);
    for (int r = 0; r < 3; ++r) {
        QCOMPARE(t->item(r, 0)->text(), QString::number(expected[size_t(r)].position, 'f', 1));
        QCOMPARE(t->item(r, 1)->text(), QString());
        QCOMPARE(t->item(r, 2)->text(), QString::number(expected[size_t(r)].height, 'f', 0));
    }
    QVERIFY(std::abs(t->item(1, 0)->text().toDouble() - 1000) < 0.5);
    p.setNormalisation("max");                         // table follows the processing
    const QString h = t->item(1, 2)->text();         // heights above the local baseline, 0..1
    QVERIFY2(h.size() == 5 && h.toDouble() > 0.9 && h.toDouble() <= 1.0, qPrintable(h));
}

void GuiTestDataPage::fitting() {
    DataPage p;
    plain(p);
    p.resize(1000, 700);
    p.addSpectrum(synth("bands", {{{1000, 1500, 10}}, {{1030, 700, 12}}, {{1600, 500, 14}}}, 0, 5));
    QString err;
    // the full (wide) view fits too: no width limit, no dialog
    QVERIFY2(p.fitView(&err), qPrintable(err));
    QVERIFY2(p.fitText().contains(QStringLiteral("fit of 3 band(s)")), qPrintable(p.fitText()));
    QVERIFY(p.note().isEmpty());
    // at most maxComponents() components: the strongest, said in the note line
    p.setMaxComponents(2);                             // refits
    QVERIFY2(p.fitText().contains(QStringLiteral("fit of 2 band(s)")), qPrintable(p.fitText()));
    QCOMPARE(p.fitComponents(), 2);
    QVERIFY2(p.note().contains(QStringLiteral("2 strongest of 3")), qPrintable(p.note()));
    QVERIFY(std::abs(p.peakTable()->item(1, 0)->text().toDouble() - 1030) < 1);   // 1600 (weakest) left out
    p.setMaxComponents(12);
    p.clearFit();
    p.plot()->setXRange(950, 1100);
    p.setFitProfile("lorentz");
    QVERIFY2(p.fitView(&err), qPrintable(err));
    QVERIFY(p.hasFit());
    QVERIFY2(p.fitText().startsWith(QStringLiteral("lorentz fit of 2 band(s) in 950–1100 cm⁻¹: R² = 0.99")),
             qPrintable(p.fitText()));
    QCOMPARE(p.peakTable()->rowCount(), 2);
    QVERIFY(std::abs(p.peakTable()->item(0, 0)->text().toDouble() - 1000) < 0.3);
    QVERIFY(!p.peakTable()->item(0, 1)->text().isEmpty());          // ± from the fit
    QVERIFY(std::abs(p.peakTable()->item(0, 3)->text().toDouble() - 10) < 0.5);
    QCOMPARE(p.peakTable()->item(0, 5)->text(), QString());         // η only for pvoigt
    // fit curves: model + 2 components, not taking part in the automatic y range; the fit window
    int fitCurves = 0;
    for (Curve* c : curves(p.plot()))
        if (!c->autoScale() && c->lineVisible()) ++fitCurves;
    QCOMPARE(fitCurves, 1 + 2);
    auto* region = firstItem<RegionItem>(p.plot());
    QVERIFY(region);
    QCOMPARE(region->region().lo, 950.0);
    // refit a range (as dragging the window does)
    p.setFitProfile("pvoigt");
    QVERIFY(p.fitRange(1550, 1650, &err));
    QVERIFY(p.fitText().startsWith(QStringLiteral("pvoigt fit of 1 band(s) in 1550–1650")));
    QVERIFY(!p.peakTable()->item(0, 5)->text().isEmpty());          // η
    QCOMPARE(firstItem<RegionItem>(p.plot())->region().hi, 1650.0);
    // a range without bands: the reason in the note line
    QVERIFY(!p.fitRange(1300, 1400, &err));
    QVERIFY2(err.contains(QStringLiteral("no peaks")), qPrintable(err));
    QCOMPARE(p.note(), err);
    // redraw / clear removes the fit
    p.clearFit();
    QVERIFY(!p.hasFit());
    QVERIFY(p.fitText().isEmpty());
    QVERIFY(!firstItem<RegionItem>(p.plot()));
    QVERIFY(p.fitRange(950, 1100));
    p.setNormalisation("max");
    QVERIFY(!p.hasFit());
}

void GuiTestDataPage::fitComponents() {
    DataPage p;
    plain(p);
    p.resize(1000, 700);
    p.addSpectrum(synth("bands", {{{1000, 1500, 10}}, {{1030, 700, 12}}}, 0, 5));
    QVERIFY(p.fitRange(950, 1100));
    QCOMPARE(p.fitComponents(), 2);
    // right-click near a component removes it: refit with one component
    QVERIFY(p.removePeakNear(1029, 5));
    QCOMPARE(p.fitComponents(), 1);
    QCOMPARE(p.peakTable()->rowCount(), 1);
    QVERIFY(!p.removePeakNear(1300, 5));               // nothing there
    // adding it back (the removed pick returns), and a component where nothing was picked
    QVERIFY(p.addPeakAt(1031));
    QCOMPARE(p.fitComponents(), 2);
    QVERIFY(p.addPeakAt(1060));
    QCOMPARE(p.fitComponents(), 3);
    QVERIFY(p.fitText().contains(QStringLiteral("fit of 3 band(s)")));
    QVERIFY(!p.addPeakAt(1060.5));                     // already there
    bool found = false;
    for (const auto& pk : p.peaksOf(0)) found = found || std::abs(pk.position - 1060) <= 3;
    QVERIFY(found);
    QVERIFY(p.removePeakNear(1060, 5));                // an added peak is forgotten
    QCOMPARE(p.fitComponents(), 2);
    // the plot's context menu offers both
    QMenu menu;
    p.fillPlotMenu(menu, 1000.4);
    QStringList texts;
    for (QAction* a : menu.actions()) texts << a->text();
    QVERIFY2(texts.filter(QStringLiteral("Add peak here")).size() == 1, qPrintable(texts.join('|')));
    QVERIFY2(texts.filter(QStringLiteral("Remove peak at 1000")).size() == 1, qPrintable(texts.join('|')));
    QVERIFY(texts.contains(QStringLiteral("Refit")));
    for (QAction* a : menu.actions())
        if (a->text().startsWith(QStringLiteral("Remove peak"))) a->trigger();
    QCOMPARE(p.fitComponents(), 1);
}

void GuiTestDataPage::carbonDG() {
    // a carbon spectrum: D Lorentzian, G BWF (I0 1000, ω0 1595, Γ 60, q -8), 2D band; no baseline correction
    DataPage p;
    plain(p);
    p.resize(1000, 700);
    DataSpectrum d;
    d.name = QStringLiteral("hard carbon");
    std::mt19937 rng(4);
    std::normal_distribution<double> noise(0, 2);
    for (double x = 150; x <= 3200; x += 1.5) {
        d.x.push_back(x);
        d.y.push_back(600 / (1 + std::pow((x - 1350) / 60, 2)) + lr::peaks::bwf(x, 1000, 1595, 60, -8) +
                      300 / (1 + std::pow((x - 2700) / 40, 2)) + 20 + noise(rng));
    }
    p.addSpectrum(d);
    // no permanent button on the page (menu Tools and the plot's context menu)
    for (QPushButton* b : p.findChildren<QPushButton*>()) QVERIFY(!b->text().contains(QStringLiteral("Carbon")));
    {
        QMenu menu;
        p.fillPlotMenu(menu, 1500);
        bool found = false;
        for (QAction* a : menu.actions()) found |= a->text() == QStringLiteral("Carbon D/G…");
        QVERIFY(found);
    }
    QString err;
    QVERIFY2(p.carbonAnalysis(lr::carbon::Model::LorentzBwf, &err), qPrintable(err));
    QVERIFY(p.hasFit() && p.carbonResult());
    const lr::carbon::Result& r = *p.carbonResult();
    const double gArea = M_PI * 1000 * 60 * (1 - 1 / 64.0) / 2, dArea = M_PI * 600 * 120 / 2;
    QVERIFY2(std::abs(r.idigArea / (dArea / gArea) - 1) < 0.03, qPrintable(QString::number(r.idigArea)));
    QVERIFY2(p.fitText().startsWith(QStringLiteral("Carbon D/G (D Lorentzian + G Breit–Wigner–Fano): I_D/I_G = ")),
             qPrintable(p.fitText()));
    QVERIFY(p.fitText().contains(QStringLiteral("L_a = ")));
    // peak table: D, G (BWF maximum), 2D
    QCOMPARE(p.peakTable()->rowCount(), 3);
    QVERIFY(std::abs(p.peakTable()->item(1, 0)->text().toDouble() - (1595 - 60 / 16.0)) < 0.5);
    QCOMPARE(p.peakTable()->item(1, 6)->text(), QStringLiteral("G (carbon)"));
    QVERIFY(std::abs(p.peakTable()->item(1, 5)->text().toDouble() + 8) < 1);       // q
    // exports: the carbon results and the peak table
    QTemporaryDir tmp;
    const QString cpath = tmp.filePath("carbon.csv");
    QVERIFY(p.exportCarbon(cpath));
    QFile f(cpath);
    QVERIFY(f.open(QIODevice::ReadOnly));
    const QString csv = QString::fromUtf8(f.readAll());
    QVERIFY(csv.startsWith(QStringLiteral("spectrum,quantity,value,error,unit\nhard carbon,model,")));
    QVERIFY(csv.contains(QStringLiteral("I_D/I_G (area)")));
    QVERIFY(p.exportPeaks(tmp.filePath("peaks.csv")));
    QFile pf(tmp.filePath("peaks.csv"));
    QVERIFY(pf.open(QIODevice::ReadOnly));
    QVERIFY(QString::fromUtf8(pf.readAll()).contains(QStringLiteral("carbon lorentz-bwf")));
    // the window: results table, model switch, Copy
    p.carbonDialog();
    QVERIFY(p.carbonWindow() && p.carbonWindow()->isVisible());
    QVERIFY(p.carbonTable()->rowCount() > 10);
    QCOMPARE(p.carbonTable()->item(0, 1)->text(), lr::carbon::modelName(lr::carbon::Model::LorentzBwf));
    auto* model = p.carbonWindow()->findChild<QComboBox*>();
    QVERIFY(model);
    model->setCurrentIndex(1);                                          // Sadezky: reanalyses
    QCOMPARE(p.carbonResult()->model, lr::carbon::Model::Sadezky);
    QCOMPARE(p.carbonTable()->item(0, 1)->text(), lr::carbon::modelName(lr::carbon::Model::Sadezky));
    for (QPushButton* b : p.carbonWindow()->findChildren<QPushButton*>())
        if (b->text() == QStringLiteral("Copy")) b->click();
    QVERIFY(QApplication::clipboard()->text().startsWith(QStringLiteral("hard carbon\nquantity\tvalue")));
    p.carbonWindow()->close();
    // a normal fit replaces it; clearing removes it
    QVERIFY(p.fitRange(1300, 1400));
    QVERIFY(!p.carbonResult());
    QVERIFY(p.carbonAnalysis());
    p.clearFit();
    QVERIFY(!p.carbonResult() && !p.hasFit());
    // a spectrum without the D/G region: the reason in the note line
    p.addSpectrum(synth("low", {{{1000, 1500, 10}}}, 0, 5, 150, 1200));
    p.setCurrentRow(p.count() - 1);
    QVERIFY(!p.carbonAnalysis(lr::carbon::Model::LorentzBwf, &err));
    QVERIFY2(err.contains(QStringLiteral("1300–1620")), qPrintable(err));
    QCOMPARE(p.note(), err);
}

void GuiTestDataPage::peakPicking() {
    DataPage p;
    plain(p);
    p.resize(1000, 700);
    // a band below 100 cm⁻¹ (laser line region) is not picked
    p.addSpectrum(synth("A", {{{80, 900, 8}}, {{400, 800, 12}}, {{1000, 1500, 10}}}, 0, 5, 50, 1850));
    p.addSpectrum(synth("B", {{{700, 300, 10}}}, 0, 6));
    p.setCurrentRow(0);
    auto peaksA = p.peaksOf(0);
    QCOMPARE(int(peaksA.size()), 2);
    QVERIFY(peaksA.front().position > 100);
    QVERIFY(!p.addPeakAt(80));
    QVERIFY(p.note().contains(QStringLiteral("100")));
    // labels of the current spectrum (default), of all ticked ones, or none
    QCOMPARE(p.peakLabels(), DataPage::Labels::Current);
    QVERIFY(p.peakLabelItem(0));
    QCOMPARE(int(p.peakLabelItem(0)->labels().size()), 2);
    QVERIFY(!p.peakLabelItem(1));
    p.setPeakLabels(DataPage::Labels::AllTicked);
    QVERIFY(p.peakLabelItem(0) && p.peakLabelItem(1));
    QCOMPARE(int(p.peakLabelItem(1)->labels().size()), 1);
    p.setPeakLabels(DataPage::Labels::Off);
    QVERIFY(!p.peakLabelItem(0));
    p.setPeakLabels(DataPage::Labels::Current);
    // sensitivity: a high S/N threshold keeps only the strong band
    QVERIFY(peaksA[0].snr < peaksA[1].snr);            // 400 (800 high) weaker than 1000 (1500 high)
    p.setPeakSnr((peaksA[0].snr + peaksA[1].snr) / 2);
    QCOMPARE(int(p.peaksOf(0).size()), 1);
    QCOMPARE(p.peakTable()->rowCount(), 1);
    p.setPeakSnr(8);
    // an added peak shows in the table and the labels; it survives a redraw
    QVERIFY(p.addPeakAt(1500));
    QCOMPARE(p.peakTable()->rowCount(), 3);
    p.setNormalisation("max");
    QCOMPARE(int(p.peaksOf(0).size()), 3);
    QCOMPARE(int(p.peakLabelItem(0)->labels().size()), 3);
    // pick on the raw data: a sloping background is not removed there
    DataPage q;
    q.setMessagesEnabled(false);
    DataSpectrum s = synth("slope", {{{600, 800, 10}}, {{1200, 900, 12}}}, 0, 3);
    for (std::size_t i = 0; i < s.x.size(); ++i) s.y[i] += 2000 + 2 * s.x[i];
    q.addSpectrum(s);
    QCOMPARE(int(q.peaksOf(0).size()), 2);
    q.setPickOnRaw(true);
    QVERIFY(q.pickOnRaw());
    const auto raw = q.peaksOf(0);
    QVERIFY(!raw.empty());
    QVERIFY(raw.front().height > 2000);                // heights on the background
    // the peak table export: the added peak flagged
    QTemporaryDir tmp;
    QVERIFY(p.exportPeaks(tmp.path() + "/peaks.csv"));
    QFile f(tmp.path() + "/peaks.csv");
    QVERIFY(f.open(QIODevice::ReadOnly));
    const QStringList lines = QString::fromUtf8(f.readAll()).trimmed().split('\n');
    QCOMPARE(lines.size(), 1 + 3);
    QVERIFY(lines[0].startsWith(QStringLiteral("spectrum,position_cm-1")));
    QVERIFY2(lines[3].startsWith(QStringLiteral("A,")) && lines[3].section(',', 6, 6) == QStringLiteral("yes") &&
                 std::abs(lines[3].section(',', 1, 1).toDouble() - 1500) <= 3,
             qPrintable(lines[3]));
    p.setPeakLabels(DataPage::Labels::AllTicked);      // all ticked spectra
    QVERIFY(p.exportPeaks(tmp.path() + "/all.csv"));
    QFile g(tmp.path() + "/all.csv");
    QVERIFY(g.open(QIODevice::ReadOnly));
    QCOMPARE(QString::fromUtf8(g.readAll()).trimmed().split('\n').size(), 1 + 3 + 1);
}

void GuiTestDataPage::peakClicks() {
    DataPage p;
    plain(p);
    p.resize(1200, 800);
    p.show();
    QVERIFY(QTest::qWaitForWindowExposed(&p));
    p.addSpectrum(synth("bands", {{{1000, 1500, 10}}}, 0, 5));
    p.plot()->setXRange(900, 1400);
    const int before = int(p.peaksOf(0).size());
    // a click in the plot adds a peak (after the double-click interval) ...
    const QPoint at = p.plot()->mapFromData(QPointF(1200, 300)).toPoint();
    QTest::mouseClick(p.plot(), Qt::LeftButton, {}, at);
    QTRY_COMPARE(int(p.peaksOf(0).size()), before + 1);
    // ... a double-click (zoom out) does not
    const QPoint at2 = p.plot()->mapFromData(QPointF(1300, 300)).toPoint();
    QTest::mouseDClick(p.plot(), Qt::LeftButton, {}, at2);
    QTest::qWait(QApplication::doubleClickInterval() + 100);
    QCOMPARE(int(p.peaksOf(0).size()), before + 1);
}

namespace {
class FakeWindow : public QObject {
    Q_OBJECT
public:
    int calls = 0;
    QString sample;
    bool accept = true;
    Q_INVOKABLE bool addToLibrary(const lr::Spectrum& sp) {
        ++calls;
        sample = sp.metaValue(QStringLiteral("sample"));
        return accept && !sp.shift.empty() && sp.shift.size() == sp.intensity.size();
    }
};
}  // namespace

void GuiTestDataPage::addToLibrary() {
    DataPage p;
    plain(p);
    p.addSpectrum(synth("cyclohexane", {{{801, 900, 8}}}, 0, 5));
    QSignalSpy spy(&p, &DataPage::addToLibraryRequested);
    QMenu menu;
    p.fillListMenu(menu, 0);
    QAction* add = nullptr;
    for (QAction* a : menu.actions())
        if (a->text() == QStringLiteral("Add to library…")) add = a;
    QVERIFY(add);
    add->trigger();
    QCOMPARE(spy.count(), 1);
    const auto d = spy.at(0).at(0).value<DataSpectrum>();
    QCOMPARE(d.name, QStringLiteral("cyclohexane"));
    // the host routes it to the main window's addToLibrary(const lr::Spectrum&)
    const lr::Spectrum sp = DataPageHost::toSpectrum(d);
    QCOMPARE(sp.shift.size(), d.x.size());
    QCOMPARE(sp.metaValue(QStringLiteral("sample")), QStringLiteral("cyclohexane"));
    FakeWindow w;
    QString err;
    QVERIFY2(DataPageHost::routeToLibrary(&w, sp, &err), qPrintable(err));
    QCOMPARE(w.calls, 1);
    QCOMPARE(w.sample, QStringLiteral("cyclohexane"));
    w.accept = false;
    QVERIFY(!DataPageHost::routeToLibrary(&w, sp, &err));
    QVERIFY(err.startsWith(QStringLiteral("not added")));
    QObject plainObject;                               // no such method
    QVERIFY(!DataPageHost::routeToLibrary(&plainObject, sp, &err));
    QVERIFY(!err.isEmpty());
}

// LIBRERAMAN_PREVIEW_DIR=dir: pictures of the peak/fit features (LIBRERAMAN_PREVIEW_SIZE=WxH, default 1300x780)
void GuiTestDataPage::preview() {
    const QString dir = qEnvironmentVariable("LIBRERAMAN_PREVIEW_DIR");
    if (dir.isEmpty()) QSKIP("preview only");
    QDir().mkpath(dir);
    const QStringList wh = qEnvironmentVariable("LIBRERAMAN_PREVIEW_SIZE", "1300x780").split('x');
    const QString tag = qEnvironmentVariable("LIBRERAMAN_PREVIEW_TAG", "1440");
    DataPage p;
    plain(p);
    p.resize(wh[0].toInt(), wh[1].toInt());
    p.show();
    QVERIFY(QTest::qWaitForWindowExposed(&p));
    std::vector<std::array<double, 3>> bands = {{{314, 600, 12}}, {{459, 1500, 8}}, {{762, 300, 10}},
                                                {{790, 350, 10}}, {{1001, 900, 9}}, {{1030, 400, 12}},
                                                {{1157, 250, 14}}, {{1442, 500, 18}}, {{1600, 450, 14}},
                                                {{2850, 700, 20}}, {{2905, 1100, 22}}, {{2960, 600, 18}},
                                                {{3060, 400, 16}}};
    p.addSpectrum(synth("sample A", bands, 0, 5, 100, 3400));
    p.addSpectrum(synth("sample B", {{{459, 900, 8}}, {{1001, 1200, 9}}, {{1600, 300, 14}}}, 0, 6, 100, 3400));
    p.setCurrentRow(0);
    p.plot()->setXRange(150, 3300);
    QVERIFY(p.addPeakAt(1100));
    QVERIFY(p.fitView());                              // wide range: the 12 strongest
    QApplication::processEvents();
    QVERIFY(p.grab().save(dir + "/datafix-fit-wide-" + tag + ".png"));
    p.clearFit();
    p.setPeakLabels(DataPage::Labels::AllTicked);
    p.setView(DataPage::View::Stacked);
    QApplication::processEvents();
    QVERIFY(p.grab().save(dir + "/datafix-labels-stacked-" + tag + ".png"));
    p.setView(DataPage::View::Overlay);
    p.setPeakLabels(DataPage::Labels::Current);
    p.setCurrentRow(0);
    p.plot()->setXRange(700, 1250);
    QVERIFY(p.fitView());
    QApplication::processEvents();
    QVERIFY(p.grab().save(dir + "/datafix-fit-zoom-" + tag + ".png"));
    p.clearFit();
    p.setBandHelp(true, true);
    p.plot()->setXRange(150, 3300);
    QApplication::processEvents();
    QVERIFY(p.grab().save(dir + "/bandhelp-wide-" + tag + ".png"));
    p.plot()->setXRange(900, 1700);
    QApplication::processEvents();
    QVERIFY(p.grab().save(dir + "/bandhelp-zoom-" + tag + ".png"));
}

void GuiTestDataPage::exportFiles() {
    DataPage p;
    p.setMessagesEnabled(false);
    p.openFiles({FILES + "/own.csv", FILES + "/vendor.csv", FILES + "/points.jdx"});
    p.addSpectrum(synth("with / odd: name", {{{800, 900, 12}}}, 50, 9));
    p.setTicked(1, false);
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QStringList csv = p.exportSpectra(tmp.path(), "csv");
    QCOMPARE(csv.size(), 3);
    QVERIFY(csv.last().endsWith("/with___odd__name.csv"));
    const QStringList jdx = p.exportSpectra(tmp.path(), "jdx");
    QCOMPARE(jdx.size(), 3);
    const std::vector<int> ticked = p.tickedRows();
    for (const QStringList& list : {csv, jdx}) {
        for (int k = 0; k < list.size(); ++k) {
            QVERIFY(QFileInfo(list[k]).size() > 0);
            const auto back = lr::fileio::read(list[k]);
            QCOMPARE(int(back.size()), 1);
            const auto [x, y] = p.processed(p.spectrum(ticked[size_t(k)]));
            QCOMPARE(back[0].x.size(), x.size());
            QVERIFY(std::abs(back[0].y[x.size() / 2] - y[x.size() / 2]) < 1e-3 * (1 + std::abs(y[x.size() / 2])));
            const QString key = list[k].endsWith(".jdx") ? QStringLiteral("$PROCESSING") : QStringLiteral("processing");
            QCOMPARE(back[0].metaValue(key), p.describeProcessing());
        }
    }
    const QString table = tmp.path() + "/table.csv";
    QVERIFY(p.exportTable(table));
    QVERIFY(QFileInfo(table).size() > 0);
    {   // common-axis table (not a spectrum file: fileio::read rejects it, as the Python version does)
        QFile f(table);
        QVERIFY(f.open(QIODevice::ReadOnly | QIODevice::Text));
        const QStringList lines = QString::fromUtf8(f.readAll()).split('\n', Qt::SkipEmptyParts);
        QVERIFY(lines.size() > 10);
        QVERIFY(lines[0].startsWith("raman_shift_cm1,"));
        QVERIFY(lines[0].endsWith(",with / odd: name"));
        QCOMPARE(lines[1].split(',').size(), 4);
    }
    for (const QString& kind : {QStringLiteral("png"), QStringLiteral("svg")}) {
        const QString path = tmp.path() + "/plot." + kind;
        QVERIFY(p.exportPlot(path));
        QVERIFY(QFileInfo(path).size() > 1000);
    }
    QImage img(tmp.path() + "/plot.png");
    QCOMPARE(img.width(), 2000);
    // all ticked spectra in one file (JCAMP-DX LINK file, multi-file SPC); opened again: one entry each
    for (const QString& fn : {QStringLiteral("one.jdx"), QStringLiteral("one.spc")}) {
        const QString path = tmp.path() + "/" + fn;
        QVERIFY(p.exportOneFile(path));
        const auto back = lr::fileio::read(path);
        QCOMPARE(back.size(), ticked.size());
        for (size_t k = 0; k < ticked.size(); ++k) {
            const auto [x, y] = p.processed(p.spectrum(ticked[k]));
            QCOMPARE(back[k].x.size(), x.size());
            QVERIFY(std::abs(back[k].y[x.size() / 2] - y[x.size() / 2]) < 1e-3 * (1 + std::abs(y[x.size() / 2])));
        }
        QCOMPARE(back[0].name, fn.endsWith(".jdx") ? p.spectrum(ticked[0]).name : QStringLiteral("one #1"));
        const int before = p.count();
        QVERIFY(p.openFiles({path}).isEmpty());
        QCOMPARE(p.count(), before + int(ticked.size()));
        p.removeRows({p.count() - 3, p.count() - 2, p.count() - 1});
    }
    p.setView(DataPage::View::HeatMap);
    QVERIFY(p.exportPlot(tmp.path() + "/heat.png"));
    QVERIFY(QFile(tmp.path() + "/plot.svg").open(QIODevice::ReadOnly));
}

void GuiTestDataPage::processingDescription() {
    DataPage p;
    p.setMessagesEnabled(false);
    QCOMPARE(p.describeProcessing(),
             QStringLiteral("baseline=arpls strength=6.0 despike=False smooth=False normalize=none"));
    p.setBaselineCorrection(false);
    p.setNormalisation("area");
    QCOMPARE(p.describeProcessing(),
             QStringLiteral("baseline=none strength=6.0 despike=False smooth=False normalize=area"));
}

// #6: the page's baseline method, strength and fit start survive a restart; #5: fit start reaches the settings.
void GuiTestDataPage::baselineSettingsRemembered() {
    {
        DataPage p;
        QCOMPARE(p.baselineMethod(), QStringLiteral("measure"));
        QCOMPARE(p.baselineFitMin(), 150.0);
        p.setBaselineMethod("snip");
        p.setBaselineStrength(7.5);
        p.setBaselineFitMin(0);
        QCOMPARE(p.settings().fitMin, 0.0);
        QVERIFY(p.describeProcessing().contains("fit_min=0"));
    }
    DataPage q;
    QCOMPARE(q.baselineMethod(), QStringLiteral("snip"));
    QCOMPARE(q.baselineStrength(), 7.5);
    QCOMPARE(q.baselineFitMin(), 0.0);
    QCOMPARE(q.settings().baseline, QStringLiteral("snip"));
}

// Add to library…: a button next to Calculator…; one ticked spectrum is added directly, with several the
// current one is preselected in a list (here without dialogs: the current ticked one).
void GuiTestDataPage::addToLibraryChoice() {
    DataPage p;
    p.setMessagesEnabled(false);
    QVERIFY(!p.addToLibraryButton()->isEnabled());
    QCOMPARE(p.libraryCandidate(false), -1);
    for (const char* n : {"A", "B"}) {
        lr::fileio::DataSpectrum d;
        d.name = QString::fromLatin1(n);
        for (int i = 0; i < 200; ++i) d.x.push_back(200 + 10 * i), d.y.push_back(100 + i % 7);
        p.addSpectrum(d);
    }
    QVERIFY(p.addToLibraryButton()->isEnabled());
    QSignalSpy spy(&p, &DataPage::addToLibraryRequested);
    p.setTicked(0, true);
    p.setTicked(1, false);
    QCOMPARE(p.libraryCandidate(false), 0);                 // one ticked: that one
    p.setTicked(1, true);
    p.setCurrentRow(1);
    QCOMPARE(p.libraryCandidate(false), 1);                 // several: the current one is preselected
    lr::gui::setDialogsEnabled(false);                      // no list to choose from in the test
    p.addToLibraryDialog();
    lr::gui::setDialogsEnabled(true);
    QCOMPARE(spy.count(), 1);
    QCOMPARE(spy[0][0].value<lr::fileio::DataSpectrum>().name, QStringLiteral("B"));
}

QTEST_MAIN(GuiTestDataPage)
#include "gui_test_datapage.moc"
