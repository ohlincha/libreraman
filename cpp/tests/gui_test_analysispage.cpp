// Widget test for the Analysis page: data in, every method run, results and plots exported. Runs offscreen.
//
// With LIBRERAMAN_PREVIEW_DIR set, preview() also renders the page to analysis-*.png in that folder, using a
// mixture time series made from the simulator's recorded spectra (for looking at the layout).
#include "gui/analysispage.h"
#include "gui/plotwidget.h"
#include "gui/theme.h"

#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>
#include <QTextStream>

#include <cmath>
#include <random>

#include "core/calibration.h"
#include "core/simulator.h"

using namespace lr;
using namespace lr::gui;

namespace {

double lorentz(double x, double c, double w) { return 1.0 / (1.0 + std::pow((x - c) / w, 2)); }

Vec axis(int n = 400) {
    Vec x;
    for (int i = 0; i < n; ++i) x.push_back(200.0 + i * 1800.0 / (n - 1));
    return x;
}

Vec component(int j, const Vec& x) {
    Vec y;
    for (double v : x) {
        switch (j) {
        case 0: y.push_back(1000 * lorentz(v, 460, 8) + 400 * lorentz(v, 1000, 15)); break;
        case 1: y.push_back(900 * lorentz(v, 1200, 10) + 300 * lorentz(v, 1020, 20)); break;
        default: y.push_back(1100 * lorentz(v, 1700, 9) + 300 * lorentz(v, 980, 20)); break;
        }
    }
    return y;
}

// A -> B -> C series with times "t=..s" (as a series.csv read by fileio).
std::vector<fileio::DataSpectrum> series(int n = 30) {
    const Vec x = axis();
    std::vector<Vec> S{component(0, x), component(1, x), component(2, x)};
    std::mt19937 rng(3);
    std::normal_distribution<double> noise(0, 4);
    std::vector<fileio::DataSpectrum> out;
    for (int i = 0; i < n; ++i) {
        const double t = 10.0 * i, k1 = 0.012, k2 = 0.005;
        const double a = std::exp(-k1 * t), b = k1 / (k2 - k1) * (std::exp(-k1 * t) - std::exp(-k2 * t));
        const double c[3] = {a, b, 1 - a - b};
        fileio::DataSpectrum d;
        d.name = QStringLiteral("series t=%1s").arg(t, 0, 'f', 1);
        d.x = x;
        for (size_t p = 0; p < x.size(); ++p) d.y.push_back(c[0] * S[0][p] + c[1] * S[1][p] + c[2] * S[2][p] + 50 + noise(rng));
        d.setMeta(QStringLiteral("time"), QStringLiteral("t=%1s").arg(t, 0, 'f', 1));
        out.push_back(d);
    }
    return out;
}

fileio::DataSpectrum pure(int j) {
    fileio::DataSpectrum d;
    d.name = QStringLiteral("pure %1").arg(QChar('A' + j));
    d.x = axis();
    d.y = component(j, d.x);
    for (double& v : d.y) v += 50;
    return d;
}

}  // namespace

class GuiTestAnalysisPage : public QObject {
    Q_OBJECT
private slots:
    void dataAndPreprocessing() {
        AnalysisPage page;
        QCOMPARE(page.spectrumCount(), 0);
        QVERIFY(page.data().empty());
        page.setSpectra(series());
        QCOMPARE(page.spectrumCount(), 30);
        QCOMPARE(page.data().rows(), 30);
        QVERIFY(page.data().hasTimes);
        QCOMPARE(page.data().times[3], 30.0);
        chemo::Preprocess p;
        p.xmin = 300;
        p.xmax = 1900;
        p.baseline = "snip";
        p.normalize = "max";
        page.setPreprocess(p);
        QVERIFY(page.data().x.front() >= 300 && page.data().x.back() <= 1900);
        QVERIFY(std::abs(page.data().D.row(0).maxCoeff() - 1.0) < 1e-9);
        const chemo::Preprocess q = page.preprocessSettings();
        QCOMPARE(q.baseline, QString("snip"));
        QVERIFY(q.xmin && std::abs(*q.xmin - 300) < 1e-9);
        // acquisitions: names and times from Spectrum
        Spectrum sp;
        sp.shift = axis();
        sp.intensity = component(0, sp.shift);
        sp.setMeta("sample", "live");
        page.addSpectrum(sp);
        QCOMPARE(page.spectrumCount(), 31);
        page.clearSpectra();
        QVERIFY(page.data().empty());
        // a series file
        QTemporaryDir dir;
        const QString path = dir.filePath("series.csv");
        {
            QFile f(path);
            QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Text));
            QTextStream o(&f);
            o << "# LibreRaman time series\nraman_shift_cm1,t=0.0s,t=5.0s,t=10.0s\n";
            for (int i = 0; i < 50; ++i) o << 100 + i << "," << i << "," << 2 * i << "," << i * i << "\n";
        }
        page.setPreprocess({});                           // the region 300-1900 would exclude this file
        QVERIFY(page.loadSeriesFile(path));
        QCOMPARE(page.data().rows(), 3);
        QCOMPARE(page.data().times[2], 10.0);
        // series.jdx (JCAMP-DX LINK file) and series.spc (multi-file SPC): times from the metadata
        {
            std::vector<fileio::DataSpectrum> sp3;
            for (int k = 0; k < 3; ++k) {
                fileio::DataSpectrum d;
                d.name = QString("t%1").arg(k);
                for (int i = 0; i < 50; ++i) {
                    d.x.push_back(100 + i * 1.5);
                    d.y.push_back(i * (k + 1));
                }
                d.meta = {{"series_elapsed_s", QString::number(k * 7.5)}};
                sp3.push_back(d);
            }
            fileio::writeJcampMulti(dir.filePath("series.jdx"), sp3, "series");
            fileio::writeSpcMulti(dir.filePath("series.spc"), sp3, "series");
        }
        for (const char* fn : {"series.jdx", "series.spc"}) {
            QVERIFY(page.loadSeriesFile(dir.filePath(fn)));
            QCOMPARE(page.data().rows(), 3);
            QVERIFY(page.data().hasTimes);
            QCOMPARE(page.data().times[2], 15.0);
        }
        QVERIFY(!page.loadSeriesFile(dir.filePath("missing.csv")));
        QVERIFY(!page.lastError().isEmpty());
    }

    void methodsAndExport() {
        AnalysisPage page;
        page.resize(1300, 850);
        page.show();
        page.setSpectra(series());
        chemo::Preprocess p;
        p.baseline = "snip";
        page.setPreprocess(p);
        page.setComponents(3);

        page.showMethod(AnalysisPage::Method::Pca);
        QVERIFY(page.pcaResult());
        QCOMPARE(page.pcaResult()->components, 3);
        page.showMethod(AnalysisPage::Method::Efa);
        QVERIFY(page.efaResult());
        QCOMPARE(int(page.efaResult()->windows.size()), 3);
        page.showMethod(AnalysisPage::Method::PureVariables);
        page.runPureVariables();

        page.showMethod(AnalysisPage::Method::Mcr);
        page.runMcr(true);
        QVERIFY(page.mcrResult());
        QVERIFY2(page.mcrResult()->lof < 10.0, qPrintable(QString::number(page.mcrResult()->lof)));
        page.computeAmbiguity(true);
        QCOMPARE(int(page.ambiguity().size()), 3);

        // in the background, with progress and a result signal
        QSignalSpy done(&page, &AnalysisPage::mcrFinished);
        page.setMcrInitialEstimate(AnalysisPage::InitialEstimate::OpaSpectra);
        page.runMcr(false);
        QVERIFY(done.wait(30000));
        QVERIFY(!page.mcrRunning());
        QVERIFY(page.mcrResult()->lof < 10.0);
        // stopped run
        chemo::McrOptions o = page.mcrOptions();
        o.tolerancePercent = 0;
        o.maxIterations = 100000;
        page.setMcrOptions(o);
        page.runMcr(false);
        page.stopMcr();
        QVERIFY(done.wait(30000));
        QVERIFY(!page.mcrRunning());

        // NNLS with the true spectra as references, every spectrum
        page.setReferenceSpectra({pure(0), pure(1), pure(2)});
        page.showMethod(AnalysisPage::Method::Nnls);
        page.runNnls();

        QTemporaryDir dir;
        for (const QString kind : {"scores", "loadings", "variance", "efa", "pure", "spectra", "concentrations"}) {
            const QString f = dir.filePath(kind + ".csv");
            QVERIFY2(page.exportCsv(kind, f), qPrintable(kind + ": " + page.lastError()));
            QFile in(f);
            QVERIFY(in.open(QIODevice::ReadOnly));
            QVERIFY(in.readAll().count('\n') > 3);
        }
        QVERIFY(!page.exportCsv("nonsense", dir.filePath("x.csv")));
        for (const QString& name : page.plotNames()) {
            QVERIFY2(page.exportPlot(name, dir.filePath(name + ".png")), qPrintable(name));
            QVERIFY(QFileInfo(dir.filePath(name + ".png")).size() > 1000);
        }
        QVERIFY(page.exportPlot("mcr-spectra", dir.filePath("s.svg")));
        QFile svg(dir.filePath("s.svg"));
        QVERIFY(svg.open(QIODevice::ReadOnly));
        QVERIFY(svg.readAll().contains("<svg"));
    }

    void nnlsAmounts() {
        AnalysisPage page;
        auto s = series(6);
        page.setSpectra(s);
        page.setReferenceSpectra({pure(0), pure(1), pure(2)});
        auto combos = page.findChildren<QComboBox*>();
        QComboBox* src = nullptr;
        QComboBox* target = nullptr;
        for (QComboBox* c : combos) {
            if (c->findText("Reference spectra") >= 0) src = c;
            if (c->findText("All spectra (amounts vs time)") >= 0) target = c;
        }
        QVERIFY(src && target);
        src->setCurrentIndex(1);
        target->setCurrentIndex(1);                       // first spectrum: pure A (t = 0)
        page.runNnls();
        QVERIFY(page.nnlsResult());
        const auto& c = page.nnlsResult()->coefficients;
        QVERIFY2(std::abs(c[0] - 1.0) < 0.02 && c[1] < 0.02 && c[2] < 0.02,
                 qPrintable(QString("%1 %2 %3").arg(c[0]).arg(c[1]).arg(c[2])));
    }

    void peakTrends() {
        using namespace lr::bandtrend;
        AnalysisPage page;
        page.resize(1400, 900);
        page.show();
        auto all = series(36);
        page.setSpectra(std::vector<fileio::DataSpectrum>(all.begin(), all.begin() + 30));
        chemo::Preprocess p;
        p.baseline = "snip";
        page.setPreprocess(p);
        page.showMethod(AnalysisPage::Method::Trends);
        QCOMPARE(page.currentMethod(), AnalysisPage::Method::Trends);
        QVERIFY(!page.trendPeaks().empty());
        // A (460 cm⁻¹) decays as exp(-0.012 t); C (1700 cm⁻¹) grows
        const int a = page.addTrendBandAtPeak(462);
        QCOMPARE(a, 0);
        QVERIFY(std::abs((page.trendBand(a).lo + page.trendBand(a).hi) / 2 - 460) < 3);
        QCOMPARE(page.trendBand(a).name, QString("460 cm⁻¹"));
        QCOMPARE(page.addTrendBandAtPeak(5000), -1);
        const int c = page.addTrendBand(1670, 1730);
        QCOMPARE(page.trendBand(c).name, QString("1700 cm⁻¹"));
        QCOMPARE(page.trendBandCount(), 2);
        page.runTrends(true);
        Series sa = page.trendSeries(a), sc = page.trendSeries(c);
        QCOMPARE(int(sa.value.size()), 30);
        QVERIFY(sa.value[0] > 10 * sa.value[29] && sa.value[0] > 500);
        QVERIFY(sc.value[29] > 5 * std::max(sc.value[0], 1.0));
        // exponential fit of the decay: τ = 1/0.012 s
        page.setTrendKinetics(Kinetic::Exponential);
        KineticFit f = page.trendFit(a);
        QVERIFY2(f.ok && std::abs(f.tau / (1 / 0.012) - 1) < 0.05, qPrintable(f.summary()));
        // fitted areas: 30 fits > kAsyncFits, in the background
        page.setTrendQuantity(a, Quantity::FitArea);
        QSignalSpy done(&page, &AnalysisPage::trendsFinished);
        page.runTrends(false);
        QVERIFY(page.trendsRunning());
        QVERIFY(done.wait(60000));
        QVERIFY(!page.trendsRunning());
        sa = page.trendSeries(a);
        QCOMPARE(int(sa.value.size()), 30);
        for (size_t i = 0; i < 10; ++i) QVERIFY(std::isfinite(sa.value[i]) && sa.error[i] > 0 && sa.error[i] < 0.1 * sa.value[i]);
        f = page.trendFit(a);
        QVERIFY2(f.ok && std::abs(f.tau / (1 / 0.012) - 1) < 0.05, qPrintable(f.summary()));
        // a growing series: the new spectra are added, the old values are kept
        const Vec before = sa.value;
        page.addSpectra(std::vector<fileio::DataSpectrum>(all.begin() + 30, all.end()));
        page.runTrends(true);
        sa = page.trendSeries(a);
        QCOMPARE(int(sa.value.size()), 36);
        for (size_t i = 0; i < 30; ++i) QCOMPARE(sa.value[i], before[i]);
        // ratio to C, relative to the first spectrum
        page.setTrendQuantity(a, Quantity::Height);
        page.runTrends(true);
        page.setTrendNormalisation(AnalysisPage::TrendNormalisation::Ratio, c);
        QVERIFY(page.trendSeries(c).value.empty());
        const Series r = page.trendSeries(a);
        QCOMPARE(int(r.value.size()), 36);
        QVERIFY(r.value[0] > 100 * r.value[35]);
        page.setTrendNormalisation(AnalysisPage::TrendNormalisation::First);
        QCOMPARE(page.trendSeries(a).value[0], 1.0);
        page.setTrendNormalisation(AnalysisPage::TrendNormalisation::None);
        page.setTrendKinetics(Kinetic::Linear);
        QVERIFY(page.trendFit(c).ok);
        // export
        QTemporaryDir dir;
        QVERIFY2(page.exportCsv("trends", dir.filePath("trends.csv")), qPrintable(page.lastError()));
        QFile in(dir.filePath("trends.csv"));
        QVERIFY(in.open(QIODevice::ReadOnly | QIODevice::Text));
        const QString csv = QString::fromUtf8(in.readAll());
        QVERIFY(csv.contains("band 460 cm⁻¹: ") && csv.contains("1700 cm⁻¹ height") && csv.contains("fit 1700 cm⁻¹"));
        int data = 0;
        for (const QString& line : csv.split('\n', Qt::SkipEmptyParts)) data += line.startsWith('#') ? 0 : 1;
        QCOMPARE(data, 1 + 36);                       // column names + one line per spectrum
        for (const QString name : {"trends", "trends-spectra"}) {
            QVERIFY(page.exportPlot(name, dir.filePath(name + ".png")));
            QVERIFY(QFileInfo(dir.filePath(name + ".png")).size() > 1000);
        }
        QVERIFY(page.exportPlot("trends", dir.filePath("trends.svg")));
        page.removeTrendBand(c);
        QCOMPARE(page.trendBandCount(), 1);
        page.clearSpectra();
        page.runTrends(true);
        QVERIFY(page.trendSeries(a).value.empty());
    }

    void preview() {
        const QString out = qEnvironmentVariable("LIBRERAMAN_PREVIEW_DIR");
        if (out.isEmpty()) QSKIP("set LIBRERAMAN_PREVIEW_DIR to render analysis-*.png");
        QDir().mkpath(out);
        // mixture time series from the simulator's recorded spectra: ethanol -> chloroform -> CCl4
        const SimData& sim = simulatorData();
        const ChannelCal& cal = defaultCalibration().channels.at(532);
        auto sample = [&](const QString& name) {
            for (const auto& s : sim.samples)
                if (s.first.compare(name, Qt::CaseInsensitive) == 0) return s.second;
            return Vec();
        };
        const Vec A = sample("ethanol"), B = sample("chloroform"), C = sample("carbon tetrachloride");
        QVERIFY2(!A.empty() && !B.empty() && !C.empty(), "simulator samples");
        Vec x;
        for (size_t p = 0; p < A.size(); ++p) x.push_back(cal.shift(double(p)));
        std::mt19937 rng(11);
        std::vector<fileio::DataSpectrum> ds;
        const int n = 36;
        for (int i = 0; i < n; ++i) {
            const double t = 20.0 * i, k1 = 0.008, k2 = 0.004;
            const double a = std::exp(-k1 * t), b = k1 / (k2 - k1) * (std::exp(-k1 * t) - std::exp(-k2 * t));
            const double c = 1 - a - b;
            fileio::DataSpectrum d;
            d.name = QStringLiteral("reaction t=%1s").arg(t, 0, 'f', 0);
            d.x = x;
            for (size_t p = 0; p < x.size(); ++p) {
                const double fl = 300.0 * std::exp(-std::pow((x[p] - 1800) / 1500, 2)) * (1.0 + 0.002 * i);
                const double v = (a * A[p] + b * B[p] + c * C[p]) * 2.0 + fl;
                std::normal_distribution<double> nd(0, std::sqrt(std::max(v, 1.0)) * 0.7);
                d.y.push_back(v + nd(rng));
            }
            d.setMeta("time", QStringLiteral("t=%1s").arg(t, 0, 'f', 1));
            ds.push_back(d.sorted());
        }
        for (const bool dark : {false, true}) {
            theme::apply(*qApp, dark ? theme::Mode::Dark : theme::Mode::Light);
            AnalysisPage page;
            page.resize(1500, 920);
            page.show();
            page.setSpectra(ds);
            chemo::Preprocess p;
            p.xmin = 200;
            p.xmax = 3200;
            p.baseline = "als";
            p.strength = 6;
            page.setPreprocess(p);
            page.setComponents(3);
            const QString suffix = dark ? QStringLiteral("-dark") : QString();
            auto shot = [&](const QString& name) {
                QApplication::processEvents();
                const QString f = out + "/analysis-" + name + suffix + ".png";
                QVERIFY(page.grab().save(f));
                qInfo("wrote %s", qPrintable(f));
            };
            page.showMethod(AnalysisPage::Method::Mcr);
            page.runMcr(true);
            page.computeAmbiguity(true);
            shot("mcr");
            // peak trends: ethanol (884, decays; fitted area), chloroform (667, intermediate), CCl4 (459, grows)
            page.showMethod(AnalysisPage::Method::Trends);
            for (double pos : {884.0, 667.0, 459.0})
                QVERIFY2(page.addTrendBandAtPeak(pos) >= 0, qPrintable(QString::number(pos)));
            for (int b = 0; b < 3; ++b) page.setTrendQuantity(b, bandtrend::Quantity::FitArea);
            page.setTrendKinetics(bandtrend::Kinetic::Exponential);
            page.runTrends(true);
            shot("trends");
            if (dark) continue;
            page.showMethod(AnalysisPage::Method::Pca);
            shot("pca");
            page.showMethod(AnalysisPage::Method::Efa);
            shot("efa");
            page.showMethod(AnalysisPage::Method::PureVariables);
            shot("pure");
            page.setReferenceSpectra({});
            page.showMethod(AnalysisPage::Method::Nnls);
            shot("nnls");
        }
        theme::apply(*qApp, theme::Mode::Light);
    }
};

QTEST_MAIN(GuiTestAnalysisPage)
#include "gui_test_analysispage.moc"
