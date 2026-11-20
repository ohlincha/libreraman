// Widget tests for the Data area (gui/datapage): opening the golden sample files, views (overlay,
// stacked, heat map), normalisation, difference, calculator, peak table, fitting, list management and
// export (files re-readable with fileio::read). Runs offscreen.
#include "gui/datapage.h"
#include "gui/plotwidget.h"
#include "gui/theme.h"

#include "core/fileio.h"
#include "core/peaks.h"

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QListWidget>
#include <QSignalSpy>
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
        if (auto* c = qobject_cast<Curve*>(it)) out.push_back(c);
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
    void exportFiles();
    void processingDescription();
};

void GuiTestDataPage::initTestCase() {
    theme::apply(*qApp, theme::Mode::Light);
    QVERIFY(QDir(FILES).exists());
}

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
    QCOMPARE(t->columnCount(), 6);
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
    QVERIFY(!p.fitView(&err));                         // full range: zoom in first
    QVERIFY(err.startsWith("Zoom in"));
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
    QCOMPARE(int(curves(p.plot()).size()), 1 + 3);
    auto* region = firstItem<RegionItem>(p.plot());
    QVERIFY(region);
    QCOMPARE(region->region().lo, 950.0);
    // refit a range (as dragging the window does)
    p.setFitProfile("pvoigt");
    QVERIFY(p.fitRange(1550, 1650, &err));
    QVERIFY(p.fitText().startsWith(QStringLiteral("pvoigt fit of 1 band(s) in 1550–1650")));
    QVERIFY(!p.peakTable()->item(0, 5)->text().isEmpty());          // η
    QCOMPARE(firstItem<RegionItem>(p.plot())->region().hi, 1650.0);
    QCOMPARE(int(curves(p.plot()).size()), 1 + 2);
    // a range without bands
    QVERIFY(!p.fitRange(1300, 1400, &err));
    QCOMPARE(err, QStringLiteral("no peaks to fit in this region"));
    // redraw / clear removes the fit
    p.clearFit();
    QVERIFY(!p.hasFit());
    QVERIFY(p.fitText().isEmpty());
    QVERIFY(!firstItem<RegionItem>(p.plot()));
    QVERIFY(p.fitRange(950, 1100));
    p.setNormalisation("max");
    QVERIFY(!p.hasFit());
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
             QStringLiteral("baseline=snip strength=5.0 despike=False smooth=False normalize=none"));
    p.setBaselineCorrection(false);
    p.setNormalisation("area");
    QCOMPARE(p.describeProcessing(),
             QStringLiteral("baseline=none strength=5.0 despike=False smooth=False normalize=area"));
}

QTEST_MAIN(GuiTestDataPage)
#include "gui_test_datapage.moc"
