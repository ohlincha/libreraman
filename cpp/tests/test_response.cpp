// Intensity (instrument-response) correction: reference curves, computing the correction, storage.
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>
#include <cmath>
#include <random>

#include "core/calibration.h"
#include "core/errors.h"
#include "core/response.h"

using namespace lr;

namespace {

Vec axis() {                                   // 2048 points, -660..4190 cm-1, ascending (like the 532 nm channel)
    Vec x(2048);
    for (size_t i = 0; i < x.size(); ++i) x[i] = -660.0 + 4850.0 * double(i) / 2047.0;
    return x;
}

// A plausible instrument response: filter edge at 135 cm-1, a broad maximum, falling sensitivity in the red.
double instrumentResponse(double s) {
    const double edge = 1.0 / (1.0 + std::exp(-(s - 135.0) / 8.0));
    return edge * (0.4 + std::exp(-0.5 * std::pow((s - 1200.0) / 1300.0, 2))) * (1.0 - 0.15 * std::sin(s / 700.0));
}

Vec measure(const Vec& x, const Vec& truth, double scale, unsigned seed, double noise = 20.0) {
    std::mt19937 g(seed);
    std::normal_distribution<double> n(0.0, noise);
    Vec y(x.size());
    for (size_t i = 0; i < x.size(); ++i)
        y[i] = (std::isfinite(truth[i]) ? truth[i] : 0.0) * instrumentResponse(x[i]) * scale + n(g);
    return y;
}

}  // namespace

class TestResponse : public QObject {
    Q_OBJECT
    QTemporaryDir m_dir;

private slots:
    void initTestCase() {
        QVERIFY(m_dir.isValid());
        qputenv("XDG_CONFIG_HOME", m_dir.path().toUtf8());
    }

    // The vendor's NIST_Coeff_00..05 are the SRM 2242 certificate values (the reference's true curve).
    void srmCoefficientsAreTheCertificate() {
        const ChannelCal& c = defaultCalibration().channels.at(532);
        QCOMPARE(c.nist_coeff.size(), response::Reference::srm2242().size());
        for (size_t k = 0; k < c.nist_coeff.size(); ++k) QCOMPARE(c.nist_coeff[k], response::Reference::srm2242()[k]);
        response::Reference r;
        const Vec v = r.curve({100.0, 1000.0, 3000.0, 3600.0}, 532.1);
        QVERIFY(std::isnan(v[0]) && std::isnan(v[3]));            // outside 195-3500
        double s = 0;                                              // polynomial in shift/1000
        for (size_t k = r.srm2242().size(); k-- > 0;) s = s * 1.0 + r.srm2242()[k];
        QCOMPARE(v[1], s);
        QVERIFY(v[2] > 0);
    }

    void blackbodyShape() {
        response::Reference r;
        r.kind = response::Reference::Kind::Blackbody;
        r.temperatureK = 2856;
        QCOMPARE(r.label(), QStringLiteral("blackbody 2856 K"));
        // 532 nm excitation, Stokes side = red: a 2856 K lamp gets brighter towards the red up to ~1 um
        const Vec v = r.curve({200.0, 1500.0, 3000.0}, 532.1);
        QVERIFY(v[0] < v[1] && v[1] < v[2]);
        r.temperatureK = 1e6;                                      // Rayleigh-Jeans: ~ v (photons per wavenumber)
        const Vec w = r.curve({0.0, 1000.0}, 500.0);               // v = 20000 and 19000 cm-1
        QVERIFY(std::abs(w[1] / w[0] - 0.95) < 0.01);
    }

    // With a known reference, the correction recovers the true curve and removes the instrument from a sample.
    void recoversTheTrueCurve() {
        const Vec x = axis();
        for (const auto kind : {response::Reference::Kind::Srm2242, response::Reference::Kind::Blackbody}) {
            response::Reference ref;
            ref.kind = kind;
            const Vec truth = ref.curve(x, 532.1);
            double tmax = 0;
            for (double t : truth) if (std::isfinite(t)) tmax = std::max(tmax, t);
            const Vec meas = measure(x, truth, 30000.0 / tmax, 7);
            const response::Response r = response::compute(x, meas, ref, 532.1, 40000);
            QVERIFY2(r.snr > 100, qPrintable(QString::number(r.snr)));
            QVERIFY(r.range[0] >= response::kMinShift);
            QVERIFY(r.range[1] > 3000);
            QVERIFY(r.warnings.isEmpty());
            // corrected reference / truth is constant over the valid range (within 2 %)
            const Vec corr = r.apply(x, meas);
            Vec q;
            for (size_t i = 0; i < x.size(); ++i)
                if (x[i] >= r.range[0] + 50 && x[i] <= r.range[1] - 50) q.push_back(corr[i] / truth[i]);
            std::sort(q.begin(), q.end());
            const double med = q[q.size() / 2];
            const double lo = q[q.size() / 50], hi = q[q.size() - 1 - q.size() / 50];   // 2nd/98th percentile
            QVERIFY2(lo > 0.97 * med && hi < 1.03 * med, qPrintable(ref.label()));
        }
        // a sample with two equal bands far apart: equal again after correction
        response::Reference ref;
        const Vec meas = measure(x, ref.curve(x, 532.1), 1e5, 3);
        const response::Response r = response::compute(x, meas, ref, 532.1);
        Vec band(x.size());
        for (size_t i = 0; i < x.size(); ++i)
            band[i] = 1000 * (std::exp(-0.5 * std::pow((x[i] - 500) / 8, 2)) + std::exp(-0.5 * std::pow((x[i] - 2900) / 8, 2)))
                      * instrumentResponse(x[i]);
        const Vec c = r.apply(x, band);
        auto peak = [&](const Vec& y, double at) {
            double m = 0;
            for (size_t i = 0; i < x.size(); ++i) if (std::abs(x[i] - at) < 20) m = std::max(m, y[i]);
            return m;
        };
        QVERIFY(std::abs(peak(band, 500) / peak(band, 2900) - 1) > 0.2);   // raw: unequal
        QVERIFY2(std::abs(peak(c, 500) / peak(c, 2900) - 1) < 0.03, qPrintable(QString::number(peak(c, 500) / peak(c, 2900))));
        // constant outside the valid range
        const Vec f = r.factorAt({-500.0, r.range[0], 4500.0, r.range[1]});
        QCOMPARE(f[0], f[1]);
        QCOMPARE(f[2], f[3]);
    }

    void refusesBadReferences() {
        const Vec x = axis();
        response::Reference ref;
        const Vec truth = ref.curve(x, 532.1);
        QVERIFY(golden_throws([&] { response::compute(x, measure(x, truth, 1e5, 1), ref, 532.1, 65535); }));  // saturated
        QVERIFY(golden_throws([&] { response::compute(x, measure(x, truth, 50, 1), ref, 532.1); }));          // weak
        QVERIFY(golden_throws([&] { response::compute(x, Vec(x.size(), 0.0), ref, 532.1); }));                // no signal
        QVERIFY(golden_throws([&] { response::compute({1, 2, 3}, {1, 2, 3}, ref, 532.1); }));                 // short
    }

    void tableReference() {
        const QString p = m_dir.filePath("cert.csv");
        QFile f(p);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("# certificate\nshift;intensity\n2000;3\n200;1\n1000;2\n");
        f.close();
        const response::Reference r = response::readTable(p);
        QCOMPARE(r.label(), QStringLiteral("table cert.csv"));
        const Vec v = r.curve({100, 200, 600, 1500, 2000, 2500}, 532.1);
        QVERIFY(std::isnan(v[0]) && std::isnan(v[5]));
        QCOMPARE(v[1], 1.0);
        QCOMPARE(v[2], 1.5);
        QCOMPARE(v[3], 2.5);
        QVERIFY(golden_throws([&] { response::readTable(m_dir.filePath("missing.csv")); }));
    }

    void storage() {
        const Vec x = axis();
        response::Reference ref;
        response::Response r = response::compute(x, measure(x, ref.curve(x, 532.1), 1e5, 5), ref, 532.1);
        r.serial = "TEST1";
        r.channel = 532;
        QVERIFY(!response::active("TEST1", 532));
        const QString hist = response::accept(r);
        QVERIFY(QFile::exists(hist));
        QVERIFY(QFile::exists(response::activePath("TEST1", 532)));
        const auto a = response::active("TEST1", 532);
        QVERIFY(a);
        QCOMPARE(a->reference, QStringLiteral("NIST SRM 2242"));
        QCOMPARE(a->factor.size(), r.factor.size());
        QCOMPARE(a->range[0], r.range[0]);
        QCOMPARE(a->factorAt({1000})[0], r.factorAt({1000})[0]);
        QVERIFY(response::active("TEST1", 532) == a);              // cached
        QVERIFY(response::remove("TEST1", 532));
        QVERIFY(!response::active("TEST1", 532));
        QVERIFY(QFile::exists(hist));                              // history kept
        QVERIFY(!response::remove("TEST1", 532));
    }

private:
    template <class F> static bool golden_throws(F f) {
        try {
            f();
        } catch (const Error&) {
            return true;
        }
        return false;
    }
};

QTEST_MAIN(TestResponse)
#include "test_response.moc"
