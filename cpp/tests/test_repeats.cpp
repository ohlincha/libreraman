// Repeated measurements of one vial (#11): mean ± sd of the spectra and band results, outlier flagging,
// exclusion, the mean spectrum's metadata and the summary file.
#include <QtTest>
#include <cmath>
#include <random>

#include "core/repeats.h"

using namespace lr;
using namespace lr::repeats;

namespace {

double gauss(double x, double c, double s) { return std::exp(-0.5 * (x - c) * (x - c) / (s * s)); }

// A powder spectrum: bands at 1001 (h) and 1600 (0.4 h) with a position offset `dx`, scale `a` (packing),
// optionally a "burnt spot" (carbon D/G bands and a broad background).
Spectrum powder(double a, double dx, bool burnt, unsigned seed) {
    std::mt19937 rng(seed);
    std::normal_distribution<double> nd(0, 1);
    Spectrum sp;
    for (double x = 100; x <= 3600; x += 2.4) {
        double v = 200 + a * 3000 * (gauss(x, 1001 + dx, 4) + 0.4 * gauss(x, 1600 + dx, 6));
        if (burnt) v += 2500 * gauss(x, 1350, 40) + 3000 * gauss(x, 1590, 25) + 1500 * gauss(x, 2000, 900);
        sp.shift.push_back(x);
        sp.intensity.push_back(v + nd(rng) * std::sqrt(v + 64));
        sp.signal.push_back(v + 1000);
        sp.background.push_back(1000);
    }
    sp.setMeta(QStringLiteral("sample"), QStringLiteral("powder"));
    sp.setMeta(QStringLiteral("file"), QStringLiteral("/tmp/powder_%1.csv").arg(seed));
    return sp;
}

}  // namespace

class TestRepeats : public QObject {
    Q_OBJECT
private slots:
    void meanSdText() {
        MeanSd m;
        m.mean = 1582.34;
        m.sd = 0.42;
        m.n = 3;
        QCOMPARE(m.text(1), QStringLiteral("1582.3 ± 0.4"));
        m.n = 1;
        QCOMPARE(m.text(1), QStringLiteral("1582.3"));
    }

    void bandsMeanSd() {
        const double dx[] = {-0.6, 0.0, 0.6, -0.3, 0.3};
        const double a[] = {1.0, 0.8, 1.2, 0.9, 1.1};               // packing changes the overall intensity
        std::vector<Spectrum> s;
        for (int k = 0; k < 5; ++k) s.push_back(powder(a[k], dx[k], false, unsigned(k + 1)));
        const Result r = analyse(s);
        QCOMPARE(r.nIncluded, 5);
        QCOMPARE(int(r.bands.size()), 2);
        for (bool o : r.outlier) QVERIFY(!o);                       // scale differences are not outliers
        const BandStat& b = r.bands[0];
        QVERIFY(std::abs(b.nominal - 1001) < 1);
        QCOMPARE(b.position.n, 5);
        QVERIFY2(std::abs(b.position.mean - 1001) < 0.3, qPrintable(b.position.text(2)));
        // sd of the offsets = 0.47; the fitted positions follow them (plus some noise)
        QVERIFY2(b.position.sd > 0.25 && b.position.sd < 0.8, qPrintable(b.position.text(2)));
        QCOMPARE(r.reference, 0);                                   // 1001 is the strongest band
        QVERIFY(std::abs(b.ratio.mean - 1.0) < 1e-12);
        QVERIFY(std::abs(r.bands[1].ratio.mean - 0.4 * 4.0 / 6.0 * 1.5) < 0.15);   // smoothed heights: ~0.4
        QVERIFY(r.bands[1].ratio.sd < 0.05);                        // the ratio does not depend on packing
        QVERIFY(b.height.sd / b.height.mean > 0.1);                 // ...the heights do
        QVERIFY(r.totalIntensity.sd > 0);
        // mean and spread spectra
        QCOMPARE(r.mean.size(), r.x.size());
        QCOMPARE(r.sd.size(), r.x.size());
        size_t i = 0;
        while (r.x[i] < 1001) ++i;
        QVERIFY(r.sd[i] > 200);                                     // the band varies with packing
    }

    void outlierFlaggedAndExcluded() {
        std::vector<Spectrum> s;
        for (int k = 0; k < 6; ++k) s.push_back(powder(1.0 + 0.05 * k, 0, k == 3, unsigned(10 + k)));
        Result r = analyse(s);
        for (int k = 0; k < 6; ++k) QCOMPARE(bool(r.outlier[size_t(k)]), k == 3);
        QVERIFY(r.distance[3] > 0.1);
        QVERIFY(r.mean[0] != 0);
        const double before = r.sd[size_t(std::lower_bound(r.x.begin(), r.x.end(), 1590.0) - r.x.begin())];
        std::vector<bool> ex(6, false);
        ex[3] = true;
        r = analyse(s, ex);
        QCOMPARE(r.nIncluded, 5);
        QVERIFY(!r.included[3]);
        QVERIFY(r.outlier[3]);                                      // still flagged
        const double after = r.sd[size_t(std::lower_bound(r.x.begin(), r.x.end(), 1590.0) - r.x.begin())];
        QVERIFY(after < 0.3 * before);
        // two spectra: no outlier test (needs 3)
        const Result two = analyse({s[0], s[3]});
        QVERIFY(!two.outlier[0] && !two.outlier[1]);
        // everything excluded: no mean
        QVERIFY(analyse(s, std::vector<bool>(6, true)).mean.empty());
    }

    void meanSpectrumAndSummary() {
        std::vector<Spectrum> s;
        for (int k = 0; k < 4; ++k) s.push_back(powder(1.0, 0, k == 2, unsigned(20 + k)));
        std::vector<bool> ex(4, false);
        ex[2] = true;
        const Result r = analyse(s, ex);
        const Spectrum m = meanSpectrum(s, r);
        QCOMPARE(m.intensity, r.mean);
        QCOMPARE(m.signal.size(), m.shift.size());
        QCOMPARE(m.metaValue(QStringLiteral("sample")), QStringLiteral("powder"));
        QVERIFY(m.metaValue(QStringLiteral("file")).isEmpty());
        QCOMPARE(m.metaValue(QStringLiteral("repeats")), QStringLiteral("3 of 4 (excluded: 3)"));
        QVERIFY(m.metaValue(QStringLiteral("repeat_files")).contains(QStringLiteral("powder_22.csv")));
        QVERIFY(m.metaValue(QStringLiteral("band_1001")).contains(QStringLiteral("±")));
        QTemporaryDir dir;
        const QString path = dir.filePath(QStringLiteral("rep.csv"));
        writeSummary(path, s, r, QStringLiteral("powder"));
        QFile f(path);
        QVERIFY(f.open(QIODevice::ReadOnly | QIODevice::Text));
        const QString text = QString::fromUtf8(f.readAll());
        QVERIFY(text.contains(QStringLiteral("# repeats: 3 of 4 (excluded: 3)")));
        QVERIFY(text.contains(QStringLiteral("# band_1001: position ")));
        QVERIFY(text.contains(QStringLiteral("# r3: powder_22.csv (excluded)")));
        QVERIFY(text.contains(QStringLiteral("raman_shift_cm1,mean,sd,r1,r2,r3,r4\n")));
    }
};

QTEST_MAIN(TestRepeats)
#include "test_repeats.moc"
