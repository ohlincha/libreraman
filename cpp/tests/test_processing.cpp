// Golden tests for core/processing (Python: processing.py), data from golden/processing.json.
//
// Tolerances (relative to the largest |value| of the expected array):
//   kExact  1e-9  closed-form / order-statistics code (SNIP, rolling ball, despike, smoothing, normalisation)
//   kSolve  1e-6  methods that solve a linear system (ALS, arPLS: banded LDL' here vs SuperLU in scipy, with
//                 condition numbers up to ~1e12 for the stiffest settings; modified polynomial: QR here vs SVD)
// The golden outputs themselves are rounded to 12 significant digits.
#include <QJsonArray>
#include <algorithm>
#include <QJsonObject>
#include <QtTest>

#include "core/errors.h"
#include "core/processing.h"
#include "golden_util.h"

using namespace lr;

namespace {
const double kExact = 1e-9;
const double kSolve = 1e-6;
}  // namespace

class TestProcessing : public QObject {
    Q_OBJECT
    QJsonObject g;
    Vec x;
    int stride = 1;
    Vec spectrum(const QString& n) const { return golden::vec(g["spectra"].toObject()[n]); }

private slots:
    void initTestCase() {
        g = golden::load("processing.json");
        x = golden::vec(g["x"]);
        stride = g["stride"].toInt();
        QCOMPARE(x.size(), size_t(2048));
    }

    void baselinesList() {
        const auto& b = processing::baselines();
        QCOMPARE(b.size(), size_t(8));
        QCOMPARE(b[0].first, QString("none"));
        QCOMPARE(b[5].first, QString("snip"));
        QCOMPARE(b[6].first, QString("snip4"));
        QCOMPARE(b[7].first, QString("airpls"));
    }

    void baselineDispatcher() {
        for (const QJsonValue& v : g["baseline"].toArray()) {
            const QJsonObject c = v.toObject();
            const QString m = c["method"].toString();
            const Vec y = spectrum(c["spectrum"].toString());
            const Vec e = golden::vec(c["out"]);
            const Vec a = golden::every(processing::baseline(x, y, m, c["strength"].toDouble()), stride);
            const bool solve = m == "als" || m == "arpls" || m == "airpls" || m == "polynomial";
            const double tol = (solve ? kSolve : kExact) * std::max(golden::maxAbs(e), 1.0);
            COMPARE_VEC(a, e, tol, 0.0, m + " " + c["spectrum"].toString());
        }
        QVERIFY(golden::throws<Error>([&] { processing::baseline(x, x, "bogus"); }));
    }

    void baselineDirect() {
        const Vec y = golden::vec(g["short"]), xs = golden::vec(g["short_x"]);
        const QJsonObject d = g["direct"].toObject();
        const double s = golden::maxAbs(y);
        COMPARE_VEC(processing::baselineAls(y, 1e4, 0.05, 5), golden::vec(d["als"]), kSolve * s, 0, "als");
        COMPARE_VEC(processing::baselineArpls(y, 1e3, 1e-2, 20), golden::vec(d["arpls"]), kSolve * s, 0, "arpls");
        COMPARE_VEC(processing::baselinePolynomial(xs, y, 3, 30), golden::vec(d["polynomial"]), kSolve * s, 0, "poly");
        COMPARE_VEC(processing::baselineRollingBall(y, 11.5), golden::vec(d["rolling_ball"]), kExact * s, 0, "ball");
        COMPARE_VEC(processing::baselineRollingBall(Vec(y.begin(), y.begin() + 40), 40),
                    golden::vec(d["rolling_ball_big"]), kExact * s, 0, "ball (window > data)");
        COMPARE_VEC(processing::baselineSnip(y, 25), golden::vec(d["snip"]), kExact * s, 0, "snip");
        COMPARE_VEC(processing::baselineSnip4(y, 25), golden::vec(d["snip4"]), kExact * s, 0, "snip4");
        COMPARE_VEC(processing::baselineAirpls(y, 1e3, 15), golden::vec(d["airpls"]), kSolve * s, 0, "airpls");
    }

    // Baselines are fitted on x >= 150 cm-1 only: below it the baseline is the data (corrected = 0), and the
    // edge-filter cliff does not drag the fit down beside low-wavenumber bands.
    void baselineFitRange() {
        const Vec y = spectrum("zinc sulfide");
        for (const auto& [m, desc] : processing::baselines()) {
            if (m == "none") continue;
            const Vec b = processing::baseline(x, y, m, 5.0);
            Vec fx, fy;
            for (size_t i = 0; i < x.size(); ++i) {
                if (x[i] < processing::kBaselineFitMin) QCOMPARE(b[i], y[i]);
                else { fx.push_back(x[i]); fy.push_back(y[i]); }
            }
            const Vec fb = processing::baseline(fx, fy, m, 5.0);      // the same as fitting the range alone
            size_t k = 0;
            for (size_t i = 0; i < x.size(); ++i)
                if (x[i] >= processing::kBaselineFitMin) QCOMPARE(b[i], fb[k++]);
        }
    }

    // #5: the fit start is a parameter (Data page "fit from", API fit_min): 0 fits everything, so imported
    // spectra keep their bands below 150 cm-1; process() passes Settings::fitMin through.
    void baselineFitMinParameter() {
        const Vec y = spectrum("zinc sulfide");
        const Vec all = processing::baseline(x, y, "snip4", 5.0, 0.0);
        bool differs = false;                          // 0-150 cm-1 is now fitted (negative shifts are not)
        for (size_t i = 0; i < x.size(); ++i)
            if (x[i] >= 0 && x[i] < processing::kBaselineFitMin) differs = differs || all[i] != y[i];
        QVERIFY(differs);
        processing::Settings st;
        st.baseline = "snip4";
        st.fitMin = 0.0;
        QCOMPARE(processing::process(x, y, st).second, all);
        QCOMPARE(processing::Settings::fromMap({{"fit_min", 40.0}}).fitMin, 40.0);
        QCOMPARE(processing::Settings::fromMap({}).fitMin, processing::kBaselineFitMin);
    }

    // ZnS (simulator spectrum, strong dome-shaped fluorescence): SNIP 4th order and the penalised methods leave
    // the region between the bands near zero; classic SNIP cuts into the dome (documents why snip4 is default).
    void fluorescenceIndicator() {
        double bg = 0;
        const double zns = processing::fluorescenceRatio(x, spectrum("zinc sulfide"), &bg);
        QVERIFY2(zns > 3 && bg > processing::kFluorescenceCounts, qPrintable(QString::number(zns)));
        QVERIFY(processing::fluorescenceRatio(x, spectrum("ethanol")) < 0.1);
        QVERIFY(processing::fluorescenceRatio(x, spectrum("carbon tetrachloride")) < 0.1);
        QVERIFY(std::isnan(processing::fluorescenceRatio(Vec{100, 200}, Vec{1, 2})));
    }

    void znsFluorescence() {
        const Vec y = spectrum("zinc sulfide");
        auto residual = [&](const QString& m) {               // median corrected value, 1200-2200 cm-1
            const Vec b = processing::baseline(x, y, m, 5.0);
            Vec r;
            for (size_t i = 0; i < x.size(); ++i)
                if (x[i] > 1200 && x[i] < 2200) r.push_back(y[i] - b[i]);
            std::nth_element(r.begin(), r.begin() + long(r.size() / 2), r.end());
            return r[r.size() / 2];
        };
        for (const char* m : {"snip4", "als", "arpls", "airpls"})
            QVERIFY2(residual(m) < 1500, qPrintable(QString("%1: %2").arg(m).arg(residual(m))));
        QVERIFY(residual("snip") > 5000);
    }

    void despike() {
        const Vec y = golden::vec(g["spiky"]);
        const QJsonObject d = g["despike"].toObject();
        const double s = golden::maxAbs(y);
        COMPARE_VEC(processing::despike(y), golden::vec(d["default"]), kExact * s, 0, "despike");
        COMPARE_VEC(processing::despike(y, 4.0), golden::vec(d["t4"]), kExact * s, 0, "despike t=4");
        const Vec eth = spectrum("ethanol");
        COMPARE_VEC(golden::every(processing::despike(eth), stride), golden::vec(d["ethanol"]),
                    kExact * golden::maxAbs(eth), 0, "despike ethanol");
    }

    void despikeFrames() {
        std::vector<Vec> f;
        for (const QJsonValue& v : g["frames"].toArray()) f.push_back(golden::vec(v));
        const QJsonObject d = g["despike_frames"].toObject();
        COMPARE_VEC(processing::despikeFrames(f), golden::vec(d["5"]), 1e-9, 0, "5 frames");
        COMPARE_VEC(processing::despikeFrames({f.begin(), f.begin() + 4}, 3.0), golden::vec(d["4k3"]), 1e-9, 0, "4 frames");
        COMPARE_VEC(processing::despikeFrames({f.begin(), f.begin() + 2}), golden::vec(d["2"]), 1e-9, 0, "2 frames");
    }

    void smooth() {
        const Vec y = golden::vec(g["short"]);
        const QJsonObject d = g["smooth"].toObject();
        for (const QString& w : d.keys())
            COMPARE_VEC(processing::smooth(y, w.toInt()), golden::vec(d[w]), kExact * golden::maxAbs(y), 0, "window " + w);
        COMPARE_VEC(processing::smooth(y, 7, 2), golden::vec(g["smooth_o2_w7"]), kExact * golden::maxAbs(y), 0, "order 2");
        QVERIFY(golden::throws<Error>([&] { sig::savgolFilter(Vec(5, 1.0), 7, 3); }));       // window longer than data
        QVERIFY(golden::throws<Error>([&] { sig::savgolFilter(Vec(50, 1.0), 5, 5); }));      // order >= window
    }

    void normalize() {
        const Vec y = golden::vec(g["short"]), xs = golden::vec(g["short_x"]);
        const QJsonObject d = g["normalize"].toObject();
        for (const QString& m : d.keys()) {
            const Vec e = golden::vec(d[m]);
            COMPARE_VEC(processing::normalize(xs, y, m, 700.0), e, kExact * golden::maxAbs(e), 0, m);
        }
    }

    void process() {
        for (const QJsonValue& v : g["process"].toArray()) {
            const QJsonObject c = v.toObject();
            const processing::Settings st = processing::Settings::fromMap(c["settings"].toObject().toVariantMap());
            const auto [out, base] = processing::process(x, spectrum(c["spectrum"].toString()), st);
            const Vec eo = golden::vec(c["out"]), eb = golden::vec(c["base"]);
            const bool solve = st.baseline == "als" || st.baseline == "arpls" || st.baseline == "polynomial";
            const double tol = solve ? kSolve : kExact;
            // the processed output is (y - baseline) normalised: a baseline error relative to the raw data is
            // amplified by (baseline height / band height), up to ~100 here
            const QString what = c["spectrum"].toString() + " " + st.baseline;
            COMPARE_VEC(golden::every(out, stride), eo, 100 * tol * golden::maxAbs(eo), 0, what + " out");
        }
    }

    void findPeaks() {
        for (const QJsonValue& v : g["find_peaks"].toArray()) {
            const QJsonObject c = v.toObject();
            const QString n = c["spectrum"].toString();
            Vec y;
            if (n == "negative") {
                for (double xv : x) y.push_back(-std::abs(xv));
            } else {
                processing::Settings st;
                st.baseline = "snip";
                y = processing::process(x, spectrum(n), st).first;
            }
            const Vec a = processing::findPeaks(x, y, c["prominence"].toDouble(), c["max_peaks"].toInt());
            COMPARE_VEC(a, golden::vec(c["peaks"]), 0.0, 0.0, n);
        }
        QVERIFY(processing::findPeaks({}, {}).empty());
    }

    void scipyFindPeaks() {
        // plateaus (midpoint), distance (highest wins) and prominence, as scipy.signal.find_peaks
        const Vec y = {0, 1, 3, 3, 3, 1, 0, 2, 0, 5, 4, 6, 0, 1, 1};
        const sig::PeakSet all = sig::findPeaks(y, 0.0);
        QCOMPARE(all.peaks, (std::vector<int>{3, 7, 9, 11}));
        QCOMPARE(all.prominences, (Vec{3, 2, 1, 6}));
        QCOMPARE(sig::findPeaks(y, 0.0, 0.0, 3).peaks, (std::vector<int>{3, 7, 11}));
        QCOMPARE(sig::findPeaks(y, 2.5).peaks, (std::vector<int>{3, 11}));
        const Vec w = sig::peakWidths(y, {3}, 0.5);
        QCOMPARE(w[0], 3.5);                // half height 1.5 crossed at 1.25 and 4.75
    }
};

QTEST_GUILESS_MAIN(TestProcessing)
#include "test_processing.moc"
