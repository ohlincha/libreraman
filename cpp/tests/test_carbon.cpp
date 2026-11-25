// Tests for core/carbon (issue #12) and the Breit-Wigner-Fano profile of core/peaks: synthetic carbon spectra
// of known parameters (D Lorentzian + G BWF with and without D′, Sadezky five bands, 2D band), recovered
// I_D/I_G, positions and widths; BWF maximum, FWHM, area and fit recovery; the Cançado L_a formula.
#include <QtTest>

#include <cmath>
#include <random>

#include "core/bandtrend.h"
#include "core/carbon.h"
#include "core/errors.h"
#include "core/peaks.h"
#include "golden_util.h"

using namespace lr;

namespace {

double lor(double x, double h, double c, double w) { return h / (1.0 + std::pow(2 * (x - c) / w, 2)); }
double gau(double x, double h, double c, double w) { return h * std::exp(-4 * std::log(2.0) * std::pow((x - c) / w, 2)); }

// Box-Muller on mt19937's raw output: the same noise on every platform.
struct Noise {
    std::mt19937 rng;
    double sigma;
    Noise(unsigned seed, double s) : rng(seed), sigma(s) {}
    double operator()() {
        const double u1 = (double(rng()) + 0.5) / 4294967296.0, u2 = (double(rng()) + 0.5) / 4294967296.0;
        return sigma * std::sqrt(-2.0 * std::log(u1)) * std::cos(6.283185307179586 * u2);
    }
};

Vec axis(double lo = 100, double hi = 3200, double step = 1.0) {
    Vec x;
    for (double v = lo; v <= hi + 1e-9; v += step) x.push_back(v);
    return x;
}

// Disordered carbon: D Lorentzian, G BWF (I0, ω0, Γ, q), optional D′, a 2D band, offset and noise.
struct Spec {
    double dH = 600, dPos = 1350, dW = 120;
    double gI0 = 1000, gPos = 1595, gW = 60, gQ = -8;
    double dpH = 0, dpPos = 1620, dpW = 25;
    double h2d = 300, pos2d = 2700, w2d = 80;
    double noise = 2.0;
};

Vec make(const Vec& x, const Spec& s, unsigned seed = 3) {
    Noise n(seed, s.noise);
    Vec y;
    for (double v : x)
        y.push_back(lor(v, s.dH, s.dPos, s.dW) + peaks::bwf(v, s.gI0, s.gPos, s.gW, s.gQ) + lor(v, s.dpH, s.dpPos, s.dpW) +
                    lor(v, s.h2d, s.pos2d, s.w2d) + 20 + n());
    return y;
}

}  // namespace

class TestCarbon : public QObject {
    Q_OBJECT
private slots:
    void bwfProfile() {
        const double i0 = 1000, x0 = 1595, w = 60, q = -8;
        // maximum at x0 + Γ/(2q), height I0 (1 + 1/q²)
        const double xm = x0 + w / (2 * q), hm = i0 * (1 + 1 / (q * q));
        COMPARE_NUM(peaks::bwf(xm, i0, x0, w, q), hm, 0.0, 1e-12, "BWF maximum");
        QVERIFY(peaks::bwf(xm + 0.01, i0, x0, w, q) < hm && peaks::bwf(xm - 0.01, i0, x0, w, q) < hm);
        QVERIFY(xm < x0);                                      // q < 0: maximum below ω0, tail to low shift
        QVERIFY(peaks::bwf(x0 - 200, i0, x0, w, q) > peaks::bwf(x0 + 200, i0, x0, w, q));
        // zero at x0 - qΓ/2 (the Fano antiresonance), Lorentzian for q -> inf
        QVERIFY(std::abs(peaks::bwf(x0 - q * w / 2, i0, x0, w, q)) < 1e-9);
        COMPARE_NUM(peaks::bwf(1610, i0, x0, w, -INFINITY), lor(1610, i0, x0, w), 0.0, 1e-12, "Lorentzian limit");
        // FWHM = Γ (q² + 1)/(q² - 1): the half-maximum crossings
        const double fwhm = w * (q * q + 1) / (q * q - 1);
        const double s = 1 / q, k = (1 - s * s) / 2;
        const double u1 = (2 * s - std::sqrt(4 * s * s + 4 * k * k)) / (2 * k), u2 = (2 * s + std::sqrt(4 * s * s + 4 * k * k)) / (2 * k);
        COMPARE_NUM(peaks::bwf(x0 + u1 * w / 2, i0, x0, w, q), hm / 2, 0.0, 1e-12, "half maximum (left)");
        COMPARE_NUM(peaks::bwf(x0 + u2 * w / 2, i0, x0, w, q), hm / 2, 0.0, 1e-12, "half maximum (right)");
        COMPARE_NUM((u2 - u1) * w / 2, fwhm, 0.0, 1e-12, "FWHM formula");
        // area above the asymptote I0/q²: π I0 Γ (1 - 1/q²)/2 (symmetric integration)
        double a = 0;
        for (double v = x0 - 2e5; v < x0 + 2e5; v += 0.05) a += 0.05 * (peaks::bwf(v, i0, x0, w, q) - i0 / (q * q));
        const double p[4] = {i0, x0, w, 1 / q};
        COMPARE_NUM(*peaks::area("bwf", p), M_PI * i0 * w * (1 - 1 / (q * q)) / 2, 0.0, 1e-12, "BWF area");
        COMPARE_NUM(a, *peaks::area("bwf", p), 0.0, 2e-3, "BWF area (numerical)");
        QCOMPARE(peaks::profileParams("bwf"), 4);
        COMPARE_NUM(peaks::profile("bwf", 1580.0, p), peaks::bwf(1580, i0, x0, w, q), 0.0, 1e-15, "profile()");
    }

    void bwfFitRecovery() {
        const Vec x = axis(1300, 1850, 1.0);
        Noise n(11, 1.0);
        Vec y;
        for (double v : x) y.push_back(peaks::bwf(v, 1000, 1590, 50, -6) + 30 + 0.02 * (v - 1500) + n());
        const peaks::FitResult r = peaks::fit(x, y, {1350, 1850}, Vec{1585}, "bwf");
        QCOMPARE(r.peaks.size(), size_t(1));
        const peaks::FitPeak& p = r.peaks[0];
        QCOMPARE(p.shape, QString("bwf"));
        QVERIFY(p.center && p.q);
        COMPARE_NUM(*p.center, 1590.0, 0.3, 0.0, "ω0");
        COMPARE_NUM(*p.q, -6.0, 0.0, 0.05, "q");
        COMPARE_NUM(p.position, 1590 + 50 / (2 * -6.0), 0.3, 0.0, "maximum position");
        COMPARE_NUM(p.height, 1000 * (1 + 1 / 36.0), 0.0, 0.01, "maximum height");
        COMPARE_NUM(p.fwhm, 50 * 37.0 / 35.0, 0.0, 0.01, "FWHM");
        QVERIFY(p.positionErr > 0 && p.positionErr < 0.5);
        QVERIFY(r.r2 > 0.999);
    }

    void lorentzBwf() {
        const Spec s;
        const Vec x = axis();
        const carbon::Result r = carbon::analyse(x, make(x, s));
        QCOMPARE(r.model, carbon::Model::LorentzBwf);
        QCOMPARE(r.dPrime, -1);                                 // no D′ in this spectrum
        QCOMPARE(r.bands.size(), size_t(2));
        const carbon::Band& D = r.bands[size_t(r.d)];
        const carbon::Band& G = r.bands[size_t(r.g)];
        QCOMPARE(D.name, QString("D"));
        QCOMPARE(G.shape, QString("bwf"));
        COMPARE_NUM(D.position, s.dPos, 0.5, 0.0, "D position");
        COMPARE_NUM(D.fwhm, s.dW, 0.0, 0.02, "D FWHM");
        COMPARE_NUM(G.position, s.gPos + s.gW / (2 * s.gQ), 0.5, 0.0, "G position (BWF maximum)");
        COMPARE_NUM(*G.center, s.gPos, 0.5, 0.0, "G ω0");
        COMPARE_NUM(*G.q, s.gQ, 0.0, 0.1, "G q");
        COMPARE_NUM(G.fwhm, s.gW * (s.gQ * s.gQ + 1) / (s.gQ * s.gQ - 1), 0.0, 0.02, "G FWHM");
        const double gHeight = s.gI0 * (1 + 1 / (s.gQ * s.gQ));
        const double gArea = M_PI * s.gI0 * s.gW * (1 - 1 / (s.gQ * s.gQ)) / 2, dArea = M_PI * s.dH * s.dW / 2;
        COMPARE_NUM(r.idigHeight, s.dH / gHeight, 0.0, 0.02, "I_D/I_G (height)");
        COMPARE_NUM(r.idigArea, dArea / gArea, 0.0, 0.03, "I_D/I_G (area)");
        QVERIFY(r.idigAreaErr > 0 && r.idigAreaErr < 0.05 * r.idigArea);
        COMPARE_NUM(r.la, carbon::crystalliteSize(r.idigArea), 0.0, 1e-12, "L_a from the area ratio");
        // 2D band
        QVERIFY(r.band2d);
        COMPARE_NUM(r.band2d->position, s.pos2d, 1.0, 0.0, "2D position");
        COMPARE_NUM(r.band2d->fwhm, s.w2d, 0.0, 0.03, "2D FWHM");
        COMPARE_NUM(r.i2dIg, s.h2d / gHeight, 0.0, 0.03, "I_2D/I_G");
        // named results and table
        COMPARE_NUM(r.value("id_ig_area"), r.idigArea, 0.0, 0.0, "value(id_ig_area)");
        COMPARE_NUM(r.value("g_position"), G.position, 0.0, 0.0, "value(g_position)");
        QVERIFY(std::isnan(r.value("d_prime_position")));
        QVERIFY(std::isnan(r.value("no such key")));
        for (const QString& k : carbon::Result::keys()) QVERIFY2(k == "d_prime_position" || std::isfinite(r.value(k)), qPrintable(k));
        const QString t = r.table();
        QVERIFY(t.contains("L_a (Cançado, from the area ratio, λ = 532 nm)"));
        QVERIFY(t.contains("G q (BWF)"));
        QVERIFY(t.contains("D′ not resolved"));
        QVERIFY(r.csv("sample").startsWith("spectrum,quantity,value,error,unit\nsample,model,"));
    }

    void dPrimeResolved() {
        Spec s;                                                 // more graphitic: narrow bands, a clear D′
        s.dH = 400, s.dW = 45, s.gI0 = 1000, s.gPos = 1582, s.gW = 22, s.gQ = -20, s.dpH = 250, s.dpW = 22;
        const Vec x = axis();
        const carbon::Result r = carbon::analyse(x, make(x, s));
        QVERIFY(r.dPrime >= 0);
        const carbon::Band& Dp = r.bands[size_t(r.dPrime)];
        QCOMPARE(Dp.name, QString("D′"));
        COMPARE_NUM(Dp.position, s.dpPos, 1.0, 0.0, "D′ position");
        COMPARE_NUM(r.bands[size_t(r.g)].position, s.gPos + s.gW / (2 * s.gQ), 0.5, 0.0, "G position");
        COMPARE_NUM(r.idigHeight, s.dH / (s.gI0 * (1 + 1 / (s.gQ * s.gQ))), 0.0, 0.03, "I_D/I_G (height)");
        COMPARE_NUM(r.value("d_prime_position"), Dp.position, 0.0, 0.0, "value(d_prime_position)");
    }

    void sadezky() {
        const Vec x = axis(100, 2000);                          // no 2D region
        Noise n(5, 2.0);
        Vec y;
        for (double v : x)
            y.push_back(lor(v, 1000, 1585, 55) + lor(v, 800, 1350, 150) + lor(v, 250, 1620, 35) + gau(v, 150, 1510, 150) +
                        lor(v, 120, 1200, 160) + 10 + n());
        carbon::Options o;
        o.model = carbon::Model::Sadezky;
        const carbon::Result r = carbon::analyse(x, y, o);
        QCOMPARE(r.bands.size(), size_t(5));
        const QStringList names{"G", "D1", "D2", "D3", "D4"};
        for (int k = 0; k < 5; ++k) QCOMPARE(r.bands[size_t(k)].name, names[k]);
        QCOMPARE(r.bands[3].shape, QString("gauss"));
        COMPARE_NUM(r.bands[0].position, 1585.0, 1.0, 0.0, "G position");
        COMPARE_NUM(r.bands[1].position, 1350.0, 1.0, 0.0, "D1 position");
        COMPARE_NUM(r.bands[2].position, 1620.0, 3.0, 0.0, "D2 position");
        COMPARE_NUM(r.bands[3].position, 1510.0, 10.0, 0.0, "D3 position");
        COMPARE_NUM(r.bands[4].position, 1200.0, 10.0, 0.0, "D4 position");
        COMPARE_NUM(r.idigHeight, 0.8, 0.0, 0.03, "I_D1/I_G (height)");
        COMPARE_NUM(r.idigArea, 800 * 150.0 / (1000 * 55.0), 0.0, 0.05, "I_D1/I_G (area)");
        QVERIFY(!r.band2d);
        QVERIFY(r.notes.join(' ').contains("2D region"));
        QVERIFY(r.table().contains("D1/G"));
    }

    void crystalliteSize() {
        // Cançado: L_a = 2.4e-10 λ⁴ / (I_D/I_G); 19.2 nm at 532 nm for a ratio of 1
        COMPARE_NUM(carbon::crystalliteSize(1.0), 2.4e-10 * std::pow(532.0, 4), 0.0, 1e-15, "L_a(1)");
        COMPARE_NUM(carbon::crystalliteSize(1.0), 19.22, 0.01, 0.0, "19.2 nm");
        COMPARE_NUM(carbon::crystalliteSize(0.5), 38.45, 0.01, 0.0, "L_a(0.5)");
        COMPARE_NUM(carbon::crystalliteSize(2.0, 514.5), 2.4e-10 * std::pow(514.5, 4) / 2, 0.0, 1e-15, "other λ");
        QVERIFY(std::isnan(carbon::crystalliteSize(0.0)));
        QVERIFY(std::isnan(carbon::crystalliteSize(-1.0)));
    }

    void errors() {
        const Vec x = axis(100, 1200);
        QVERIFY(golden::throws<Error>([&] { carbon::analyse(x, Vec(x.size(), 1.0)); }));
        const Vec x2 = axis();
        carbon::Options o;
        o.lo = 1400;                                            // region misses the D band
        QVERIFY(golden::throws<Error>([&] { carbon::analyse(x2, make(x2, Spec{}), o); }));
    }

    void bandTrend() {
        // carbon quantities of a band: the band's region if it holds 1300-1620 cm⁻¹, else 950-1800
        const Vec x = axis();
        Spec s;
        const Vec y = make(x, s);
        bandtrend::Band b;
        b.lo = 1500;
        b.hi = 1700;
        b.quantity = bandtrend::Quantity::CarbonIdIgArea;
        QVERIFY(bandtrend::needsFit(b.quantity));
        const bandtrend::Measure m = bandtrend::measure(x, y, b, true);
        const carbon::Result r = carbon::analyse(x, y);
        COMPARE_NUM(m.value(bandtrend::Quantity::CarbonIdIgArea), r.idigArea, 0.0, 1e-9, "trend I_D/I_G (area)");
        COMPARE_NUM(m.error(bandtrend::Quantity::CarbonIdIgArea), r.idigAreaErr, 0.0, 1e-9, "trend error");
        COMPARE_NUM(m.value(bandtrend::Quantity::CarbonIdIgHeight), r.idigHeight, 0.0, 1e-9, "trend I_D/I_G (height)");
        COMPARE_NUM(m.value(bandtrend::Quantity::CarbonLa), r.la, 0.0, 1e-9, "trend L_a");
        COMPARE_NUM(m.value(bandtrend::Quantity::CarbonGPosition), r.value("g_position"), 0.0, 1e-9, "trend G position");
        QCOMPARE(bandtrend::quantityUnit(bandtrend::Quantity::CarbonLa), QString("nm"));
        QVERIFY(bandtrend::isPosition(bandtrend::Quantity::CarbonGPosition));
        // without fitting: NaN
        QVERIFY(std::isnan(bandtrend::measure(x, y, b, false).value(bandtrend::Quantity::CarbonIdIgArea)));
    }
};

QTEST_GUILESS_MAIN(TestCarbon)
#include "test_carbon.moc"
