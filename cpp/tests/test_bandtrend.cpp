// Tests for core/bandtrend: band heights, areas, fitted areas, positions and widths along a synthetic
// series (a Lorentzian band decaying exponentially with τ = 300 s, a Gaussian band growing with τ = 150 s, a
// constant reference band, a sloping background and noise), ratios, relative trends and kinetic fits.
#include <QtTest>

#include <cmath>
#include <random>

#include "core/bandtrend.h"

using namespace lr;
using namespace lr::bandtrend;

namespace {

constexpr double kTauA = 300, kTauB = 150;
constexpr double kHA = 1000, kWA = 10;          // Lorentzian at 1000 cm⁻¹: height, FWHM
constexpr double kHB = 800, kWB = 14;           // Gaussian at 1450 cm⁻¹
constexpr double kHR = 500, kWR = 12;           // constant Lorentzian at 600 cm⁻¹

double lor(double x, double c, double w) { return 1.0 / (1.0 + std::pow(2 * (x - c) / w, 2)); }
double gau(double x, double c, double w) { return std::exp(-4 * std::log(2.0) * std::pow((x - c) / w, 2)); }

double hA(double t) { return kHA * std::exp(-t / kTauA); }
double hB(double t) { return kHB * (1 - std::exp(-t / kTauB)); }
double areaA(double t) { return M_PI / 2 * hA(t) * kWA; }
double areaB(double t) { return std::sqrt(M_PI / (4 * std::log(2.0))) * hB(t) * kWB; }

struct Data {
    Vec x, t;
    std::vector<Vec> rows;
};

Data series(double noise = 2.0) {
    Data d;
    for (int i = 0; i <= 1400; ++i) d.x.push_back(400 + i);
    std::mt19937 rng(7);
    std::normal_distribution<double> nd(0, noise);
    for (int k = 0; k <= 60; ++k) {
        const double t = 20.0 * k;
        d.t.push_back(t);
        Vec y;
        for (double x : d.x)
            y.push_back(hA(t) * lor(x, 1000, kWA) + hB(t) * gau(x, 1450, kWB) + kHR * lor(x, 600, kWR) + 200 +
                        0.05 * (x - 400) + nd(rng));
        d.rows.push_back(y);
    }
    return d;
}

Band band(const QString& name, double lo, double hi, Quantity q, const QString& profile = "lorentz") {
    Band b;
    b.name = name;
    b.lo = lo;
    b.hi = hi;
    b.quantity = q;
    b.profile = profile;
    return b;
}

Vec mean(const Data& d) {
    Vec m(d.x.size(), 0.0);
    for (const Vec& r : d.rows)
        for (size_t i = 0; i < m.size(); ++i) m[i] += r[i] / double(d.rows.size());
    return m;
}

}  // namespace

class TestBandTrend : public QObject {
    Q_OBJECT
private slots:
    void heightAreaPositionWidth() {
        const Data d = series();
        const std::vector<Band> bands{band("A", 940, 1060, Quantity::Height), band("B", 1390, 1510, Quantity::Area)};
        const auto m = measureSeries(d.x, d.rows, bands);
        QCOMPARE(int(m.size()), 2);
        QCOMPARE(int(m[0].size()), 61);
        for (size_t k = 0; k < d.t.size(); ++k) {
            const double t = d.t[k];
            const Measure& a = m[0][k];
            const Measure& b = m[1][k];
            // Lorentzian tails at ±60 cm⁻¹ are 0.7 % of the height: the local baseline takes them away
            if (hA(t) > 100) {
                QVERIFY2(std::abs(a.height / hA(t) - 1) < 0.02, qPrintable(QString("t %1: %2 vs %3").arg(t).arg(a.height).arg(hA(t))));
                QVERIFY2(std::abs(a.peak - 1000) < 0.3, qPrintable(QString::number(a.peak)));
                // the centroid weighs the noise of the whole region: tight only for strong bands
                QVERIFY2(std::abs(a.centroid - 1000) < (hA(t) > 300 ? 0.3 : 1.0), qPrintable(QString("t %1: %2").arg(t).arg(a.centroid)));
                QVERIFY2(std::abs(a.fwhm - kWA) < (hA(t) > 300 ? 0.3 : 0.6), qPrintable(QString("t %1: %2").arg(t).arg(a.fwhm)));
            }
            // Gaussian: the whole area is inside the region
            if (hB(t) > 100) {
                QVERIFY2(std::abs(b.area / areaB(t) - 1) < 0.02, qPrintable(QString("t %1: %2 vs %3").arg(t).arg(b.area).arg(areaB(t))));
                QVERIFY2(std::abs(b.height / hB(t) - 1) < 0.02, qPrintable(QString::number(b.height)));
                QVERIFY2(std::abs(b.fwhm - kWB) < 0.4, qPrintable(QString::number(b.fwhm)));
            }
            QVERIFY(!a.fitted && std::isnan(a.fitArea));
        }
        // the Lorentzian's area in a ±6·FWHM window with the tails as baseline is a constant fraction
        Band whole = band("A", 940, 1060, Quantity::Area);
        double lo = 1e9, hi = 0;
        for (size_t k = 0; k < d.t.size(); ++k) {
            if (hA(d.t[k]) < 300) continue;           // weaker: the noise of the edge anchors dominates
            const double f = measure(d.x, d.rows[k], whole, false).area / areaA(d.t[k]);
            lo = std::min(lo, f);
            hi = std::max(hi, f);
        }
        QVERIFY2(hi / lo - 1 < 0.08 && lo > 0.8, qPrintable(QString("%1 %2").arg(lo).arg(hi)));
        Vec areas;
        for (const Vec& r : d.rows) areas.push_back(measure(d.x, r, whole, false).area);
        const KineticFit fw = fitExponential(d.t, areas);
        QVERIFY2(fw.ok && std::abs(fw.tau / kTauA - 1) < 0.03, qPrintable(fw.summary()));
        // no local baseline: the background (200 + 0.05 (x - 400) = 230 at 1000) is included
        Band raw = band("A", 940, 1060, Quantity::Height);
        raw.localBaseline = false;
        QVERIFY(std::abs(measure(d.x, d.rows[0], raw, false).height - (kHA + 230 + 0.012 * kHR)) < 10);
        // outside the data / too narrow: NaN, no exception
        QVERIFY(std::isnan(measure(d.x, d.rows[0], band("out", 3000, 3100, Quantity::Height), false).height));
        QVERIFY(std::isnan(measure(d.x, d.rows[0], band("one", 1000.2, 1000.6, Quantity::Height), true).fitArea));
    }

    void fittedArea() {
        const Data d = series();
        Band a = band("A", 940, 1060, Quantity::FitArea, "lorentz");
        Band b = band("B", 1390, 1510, Quantity::FitArea, "gauss");
        const Vec m = mean(d);
        setGuesses(a, d.x, m);
        setGuesses(b, d.x, m);
        QCOMPARE(int(a.guesses.size()), 1);
        QVERIFY(std::abs(a.guesses[0].position - 1000) < 1);
        const auto r = measureSeries(d.x, d.rows, {a, b});
        for (size_t k = 0; k < d.t.size(); ++k) {
            const double t = d.t[k];
            const Measure& ma = r[0][k];
            const Measure& mb = r[1][k];
            QVERIFY(ma.fitted);
            if (hA(t) > 50)                            // within 4 standard errors
                QVERIFY2(std::abs(ma.fitArea - areaA(t)) < 4 * ma.fitAreaErr,
                         qPrintable(QString("t %1: %2 ± %3 vs %4").arg(t).arg(ma.fitArea).arg(ma.fitAreaErr).arg(areaA(t))));
            if (hA(t) > 150) {
                QVERIFY2(std::abs(ma.fitArea / areaA(t) - 1) < 0.02,
                         qPrintable(QString("t %1: %2 vs %3").arg(t).arg(ma.fitArea).arg(areaA(t))));
                QVERIFY(std::abs(ma.fitPosition - 1000) < 0.2);
                QVERIFY(std::abs(ma.fitFwhm - kWA) < 0.3);
                QVERIFY(ma.fitAreaErr > 0 && ma.fitAreaErr < 0.02 * ma.fitArea);
                QCOMPARE(ma.value(Quantity::FitArea), ma.fitArea);
                QCOMPARE(ma.error(Quantity::FitArea), ma.fitAreaErr);
            }
            if (hB(t) > 150)
                QVERIFY2(std::abs(mb.fitArea / areaB(t) - 1) < 0.02,
                         qPrintable(QString("t %1: %2 vs %3").arg(t).arg(mb.fitArea).arg(areaB(t))));
        }
        // exponential fit of the fitted areas (weighted by their errors) recovers τ
        const Series s = extract(r[0], Quantity::FitArea);
        const KineticFit f = fitExponential(d.t, s.value, s.error);
        QVERIFY2(f.ok, qPrintable(f.message));
        QVERIFY2(std::abs(f.tau / kTauA - 1) < 0.02, qPrintable(f.summary()));
        QVERIFY(f.r2 > 0.999);
        QVERIFY(f.tauErr > 0 && f.tauErr < 0.02 * f.tau);
    }

    void kinetics() {
        const Data d = series();
        const std::vector<Band> bands{band("A", 940, 1060, Quantity::Height), band("B", 1390, 1510, Quantity::Area),
                                      band("R", 480, 720, Quantity::Height)};
        const auto m = measureSeries(d.x, d.rows, bands);
        // decaying height
        const KineticFit fa = fitExponential(d.t, extract(m[0], Quantity::Height).value);
        QVERIFY2(fa.ok && std::abs(fa.tau / kTauA - 1) < 0.02, qPrintable(fa.summary()));
        QVERIFY(fa.b > 0);
        QCOMPARE(fa.t0, 0.0);
        QVERIFY(std::abs(fa(0) - fa.a - fa.b) < 1e-9);
        // growing area: B < 0
        const KineticFit fb = fitKinetic(Kinetic::Exponential, d.t, extract(m[1], Quantity::Area).value);
        QVERIFY2(fb.ok && std::abs(fb.tau / kTauB - 1) < 0.02, qPrintable(fb.summary()));
        QVERIFY(fb.b < 0 && fb.a > 0);
        QVERIFY(std::abs(fb.rate * fb.tau - 1) < 1e-12);
        // ratio to the constant reference band follows exp(-t/τ) · kHA/kHR
        const Series ra = ratio(extract(m[0], Quantity::Height), extract(m[2], Quantity::Height));
        for (size_t k = 0; k < d.t.size(); ++k)
            if (hA(d.t[k]) > 200)
                QVERIFY2(std::abs(ra.value[k] / (hA(d.t[k]) / kHR) - 1) < 0.02, qPrintable(QString::number(ra.value[k])));
        const KineticFit fr = fitExponential(d.t, ra.value);
        QVERIFY2(fr.ok && std::abs(fr.tau / kTauA - 1) < 0.02, qPrintable(fr.summary()));
        // relative to the first point
        const Series rel = relativeToFirst(extract(m[0], Quantity::Height));
        QCOMPARE(rel.value[0], 1.0);
        QVERIFY(std::abs(rel.value[15] - std::exp(-300.0 / kTauA)) < 0.02);
        // starting later than 0: t0 is the first time; time offsets do not change τ
        Vec t2 = d.t;
        for (double& v : t2) v += 5000;
        const KineticFit f2 = fitExponential(t2, extract(m[0], Quantity::Height).value);
        QVERIFY(f2.ok && std::abs(f2.tau - fa.tau) < 1e-3 * fa.tau && f2.t0 == 5000);
    }

    void exactFitsAndEdgeCases() {
        Vec t, y, yl;
        for (int i = 0; i < 30; ++i) {
            t.push_back(10.0 * i);
            y.push_back(5 + 20 * std::exp(-t.back() / 77.0));
            yl.push_back(3 - 0.25 * t.back());
        }
        const KineticFit e = fitExponential(t, y);
        QVERIFY(e.ok && std::abs(e.tau - 77) < 1e-6 && std::abs(e.a - 5) < 1e-8 && std::abs(e.b - 20) < 1e-8);
        QVERIFY(std::abs(e.r2 - 1) < 1e-12);
        const KineticFit l = fitLinear(t, yl);
        QVERIFY(l.ok && std::abs(l.b + 0.25) < 1e-12 && std::abs(l.a - 3) < 1e-10 && std::abs(l.r2 - 1) < 1e-12);
        QCOMPARE(l.rate, l.b);
        QVERIFY(l.summary().contains("R²"));
        // NaN points are skipped
        y[3] = std::nan("");
        QVERIFY(fitExponential(t, y).ok);
        QCOMPARE(fitExponential(t, y).points, 29);
        // too few points, constant times, no decay
        QVERIFY(!fitExponential({1, 2, 3}, {1, 2, 3}).ok);
        QVERIFY(!fitLinear({1}, {1}).ok);
        QVERIFY(!fitLinear({2, 2, 2}, {1, 2, 3}).ok);
        QVERIFY(!fitExponential(t, yl).ok || fitExponential(t, yl).r2 > 0.99);   // a line: no meaningful τ
        QCOMPARE(fitKinetic(Kinetic::None, t, y).ok, false);
        // ratio error propagation
        Series a{{10, 20}, {1, std::nan("")}}, b{{5, 0}, {0.5, 1}};
        const Series r = ratio(a, b);
        QCOMPARE(r.value[0], 2.0);
        QVERIFY(std::abs(r.error[0] - 2 * std::sqrt(0.01 + 0.01)) < 1e-12);
        QVERIFY(std::isnan(r.value[1]));
        // names, units
        QCOMPARE(quantityNames().size(), 8);
        QCOMPARE(quantityKey(Quantity::FitArea), QString("fit_area"));
        QVERIFY(needsFit(Quantity::FitFwhm) && !needsFit(Quantity::Fwhm));
        QCOMPARE(quantityUnit(Quantity::Centroid), QString("cm⁻¹"));
        QCOMPARE(timeUnit(100).first, QString("s"));
        QCOMPARE(timeUnit(600).first, QString("min"));
        QCOMPARE(timeUnit(4 * 3600).second, 3600.0);
        QCOMPARE(defaultName(1051.6), QString("1052 cm⁻¹"));
        peaks::Peak p;
        p.position = 1052;
        p.fwhm = 8;
        const Band bb = bandAround(p, Vec{1000, 1001, 1002});
        QCOMPARE(bb.lo, 1036.0);
        QCOMPARE(bb.hi, 1068.0);
        QCOMPARE(bb.name, QString("1052 cm⁻¹"));
    }
};

QTEST_GUILESS_MAIN(TestBandTrend)
#include "test_bandtrend.moc"
