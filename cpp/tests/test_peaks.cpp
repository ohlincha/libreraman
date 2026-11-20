// Golden tests for core/peaks (Python: peaks.py), data from golden/peaks.json.
//
// Tolerances:
//  * noise level, pick(), profiles: 1e-9 relative (same arithmetic; Savitzky-Golay coefficients from a
//    different least-squares solver).
//  * Faddeeva function: 1e-13 relative to |w| against scipy.special.wofz (TOMS 680, ~14 digits).
//  * fit(): scipy's least_squares (TRF) stops at ftol = xtol = 1e-8, the C++ fit iterates to the
//    constrained minimum, so parameters agree to a small fraction of their standard error:
//    |delta| <= 0.01 * stderr + 1e-8 |value| (observed: < 2e-4 * stderr); standard errors 1 %, eta 0.01,
//    areas 1e-4 relative + 0.02 * error-derived spread, R^2 1e-8, RMS 1e-6 relative, model and
//    component curves 1e-4 of the model's maximum.
#include <QJsonArray>
#include <QJsonObject>
#include <QtTest>

#include "core/errors.h"
#include "core/peaks.h"
#include "core/processing.h"
#include "golden_util.h"

using namespace lr;

namespace {

peaks::Peak peakFrom(const QJsonObject& o) {
    peaks::Peak p;
    p.position = o["position"].toDouble();
    p.height = o["height"].toDouble();
    p.fwhm = o["fwhm"].toDouble();
    p.area = o["area"].toDouble();
    p.snr = o["snr"].toDouble();
    p.shoulder = o["shoulder"].toBool();
    return p;
}

QString comparePeaks(const std::vector<peaks::Peak>& a, const QJsonArray& e, double rtol) {
    if (a.size() != size_t(e.size())) return QString("%1 peaks, expected %2").arg(a.size()).arg(e.size());
    for (size_t i = 0; i < a.size(); ++i) {
        const peaks::Peak p = peakFrom(e[int(i)].toObject());
        const double av[] = {a[i].position, a[i].height, a[i].fwhm, a[i].area, a[i].snr};
        const double ev[] = {p.position, p.height, p.fwhm, p.area, p.snr};
        for (int k = 0; k < 5; ++k)
            if (!golden::close(av[k], ev[k], 1e-12, rtol))
                return QString("peak %1 field %2: %3 vs %4").arg(i).arg(k).arg(av[k], 0, 'g', 17).arg(ev[k], 0, 'g', 17);
        if (a[i].shoulder != p.shoulder) return QString("peak %1: shoulder flag").arg(i);
    }
    return {};
}

}  // namespace

class TestPeaks : public QObject {
    Q_OBJECT
    QJsonObject g;
    Vec x, y;

private slots:
    void initTestCase() {
        g = golden::load("peaks.json");
        x = golden::vec(g["x"]);
        y = golden::vec(g["y"]);
    }

    void noiseLevel() {
        for (const QJsonValue& v : g["noise_level"].toArray()) {
            const QJsonObject c = v.toObject();
            const Vec in = c["y"].isString() ? y : golden::vec(c["y"]);
            COMPARE_NUM(peaks::noiseLevel(in), c["out"].toDouble(), 0.0, 1e-12, "noise level");
        }
    }

    void pick() {
        for (const QJsonValue& v : g["pick"].toArray()) {
            const QJsonObject c = v.toObject(), a = c["args"].toObject();
            const Vec in = c["input"].toString() == "shoulder" ? golden::vec(g["shoulder"]) : y;
            std::optional<double> xmin, xmax;
            if (a.contains("xmin")) xmin = a["xmin"].toDouble();
            if (a.contains("xmax")) xmax = a["xmax"].toDouble();
            const auto r = peaks::pick(x, in, a["snr"].toDouble(5.0), a["min_fwhm"].toDouble(2.0), a["shoulders"].toBool(),
                                       xmin, xmax);
            const QString msg = comparePeaks(r, c["peaks"].toArray(), 1e-9);
            QVERIFY2(msg.isEmpty(), qPrintable(msg));
        }
        bool sawShoulder = false;
        for (const QJsonValue& v : g["pick"].toArray().last().toObject()["peaks"].toArray())
            sawShoulder |= v.toObject()["shoulder"].toBool();
        QVERIFY(sawShoulder);
        QVERIFY(peaks::pick({1, 2, 3}, {1, 5, 1}).empty());
    }

    void pickSimulator() {
        const QJsonObject pg = golden::load("processing.json");
        const Vec sx = golden::vec(pg["x"]);
        for (const QJsonValue& v : g["pick_sim"].toArray()) {
            const QJsonObject c = v.toObject();
            processing::Settings st;
            st.baseline = "snip";
            st.despike = st.smooth = true;
            const Vec sy = processing::process(sx, golden::vec(pg["spectra"].toObject()[c["spectrum"].toString()]), st).first;
            const QString msg = comparePeaks(peaks::pick(sx, sy, 8, 2.0, false, 150.0), c["peaks"].toArray(), 1e-8);
            QVERIFY2(msg.isEmpty(), qPrintable(c["spectrum"].toString() + ": " + msg));
        }
    }

    void profiles() {
        const QJsonObject p = g["profiles"].toObject();
        const Vec xs = golden::vec(p["x"]);
        auto map = [&](auto f) {
            Vec out;
            for (double xv : xs) out.push_back(f(xv));
            return out;
        };
        COMPARE_VEC(map([](double t) { return peaks::lorentz(t, 2.0, 1.5, 7.0); }), golden::vec(p["lorentz"]), 1e-15, 1e-12, "lorentz");
        COMPARE_VEC(map([](double t) { return peaks::gauss(t, 2.0, 1.5, 7.0); }), golden::vec(p["gauss"]), 1e-15, 1e-12, "gauss");
        COMPARE_VEC(map([](double t) { return peaks::pvoigt(t, 2.0, 1.5, 7.0, 0.3); }), golden::vec(p["pvoigt"]), 1e-15, 1e-12, "pvoigt");
        COMPARE_VEC(map([](double t) { return peaks::voigt(t, 2.0, 1.5, 5.0, 3.0); }), golden::vec(p["voigt"]), 1e-13, 1e-11, "voigt");
        COMPARE_VEC(map([](double t) { return peaks::voigt(t, 1.0, 0.0, 8.0, 1e-4); }), golden::vec(p["voigt_narrow_l"]), 1e-13, 1e-9,
                    "voigt (gaussian limit)");
        COMPARE_VEC(map([](double t) { return peaks::voigt(t, 1.0, 0.0, 0.0, 6.0); }), golden::vec(p["voigt_narrow_g"]), 1e-13, 1e-9,
                    "voigt (lorentzian limit)");
        QCOMPARE(peaks::profiles(), (QStringList{"lorentz", "gauss", "pvoigt", "voigt"}));
        QCOMPARE(peaks::profileParams("gauss"), 3);
        QCOMPARE(peaks::profileParams("voigt"), 4);
        QVERIFY(golden::throws<Error>([&] { peaks::profileParams("cauchy"); }));
        const double q[4] = {2.0, 0.0, 3.0, 0.25};
        COMPARE_NUM(*peaks::area("lorentz", q), M_PI * 3.0, 0.0, 1e-15, "lorentz area");
        QVERIFY(!peaks::area("voigt", q));
        COMPARE_NUM(peaks::profile("pvoigt", 1.0, q), peaks::pvoigt(1.0, 2.0, 0.0, 3.0, 0.25), 0.0, 1e-15, "profile()");
    }

    void faddeeva() {
        for (const QJsonValue& v : g["wofz"].toArray()) {
            const QJsonArray a = v.toArray();
            const std::complex<double> w = peaks::faddeeva({a[0].toDouble(), a[1].toDouble()});
            const std::complex<double> e(a[2].toDouble(), a[3].toDouble());
            QVERIFY2(std::abs(w - e) <= 1e-13 * std::abs(e),
                     qPrintable(QString("w(%1+%2i)").arg(a[0].toDouble()).arg(a[1].toDouble())));
        }
        // lower half plane and symmetry: w(-z) = 2 exp(-z^2) - w(z), w(-conj z) = conj w(z)
        const std::complex<double> z(1.3, 0.7);
        const auto a = peaks::faddeeva(-z), b = 2.0 * std::exp(-z * z) - peaks::faddeeva(z);
        QVERIFY(std::abs(a - b) < 1e-14);
        QVERIFY(std::abs(peaks::faddeeva(-std::conj(z)) - std::conj(peaks::faddeeva(z))) < 1e-15);
    }

    void fit() {
        int n = 0;
        for (const QJsonValue& v : g["fit"].toArray()) {
            const QJsonObject c = v.toObject(), e = c["result"].toObject();
            const QString shape = c["shape"].toString();
            const QJsonArray reg = c["region"].toArray();
            const std::pair<double, double> region{reg[0].toDouble(), reg[1].toDouble()};
            const bool fix = c["fix_positions"].toBool();
            peaks::FitResult r;
            const QJsonValue gs = c["guesses"];
            if (gs.isNull()) {
                r = peaks::fit(x, y, region, std::nullopt, shape, fix);
            } else if (gs.toArray().first().isObject()) {
                std::vector<peaks::Peak> pk;
                for (const QJsonValue& p : gs.toArray()) pk.push_back(peakFrom(p.toObject()));
                r = peaks::fit(x, y, region, pk, shape, fix);
            } else {
                r = peaks::fit(x, y, region, golden::vec(gs), shape, fix);
            }
            const QString what = QString("%1 fit #%2").arg(shape).arg(n++);
            QCOMPARE(r.shape, shape);
            const QJsonArray ep = e["peaks"].toArray();
            QCOMPARE(r.peaks.size(), size_t(ep.size()));
            for (size_t k = 0; k < r.peaks.size(); ++k) {
                const QJsonObject q = ep[int(k)].toObject();
                const peaks::FitPeak& p = r.peaks[k];
                auto param = [&](double a, const char* key, const char* errKey) {
                    const double ev = q[key].toDouble(), ee = q[errKey].toDouble();
                    COMPARE_NUM(a, ev, 0.01 * ee, 1e-8, what + " " + key);
                };
                param(p.position, "position", "position_err");
                param(p.height, "height", "height_err");
                param(p.fwhm, "fwhm", "fwhm_err");
                COMPARE_NUM(p.positionErr, q["position_err"].toDouble(), 0.0, 0.01, what + " position_err");
                COMPARE_NUM(p.heightErr, q["height_err"].toDouble(), 0.0, 0.01, what + " height_err");
                COMPARE_NUM(p.fwhmErr, q["fwhm_err"].toDouble(), 0.0, 0.01, what + " fwhm_err");
                const double areaSpread = q["area"].toDouble() * (q["height_err"].toDouble() / q["height"].toDouble() +
                                                                  q["fwhm_err"].toDouble() / q["fwhm"].toDouble());
                COMPARE_NUM(p.area, q["area"].toDouble(), 0.02 * areaSpread, 1e-4, what + " area");
                QCOMPARE(p.eta.has_value(), q.contains("eta"));
                if (p.eta) COMPARE_NUM(*p.eta, q["eta"].toDouble(), 0.01, 0.0, what + " eta");
            }
            COMPARE_VEC(r.x, golden::vec(e["x"]), 0.0, 0.0, what + " x");
            const Vec em = golden::vec(e["model"]);
            COMPARE_VEC(r.model, em, 1e-4 * golden::maxAbs(em), 0.0, what + " model");
            QCOMPARE(r.components.size(), r.peaks.size());
            for (size_t k = 0; k < r.components.size(); ++k)
                COMPARE_VEC(r.components[k], golden::vec(e["components"].toArray()[int(k)]), 1e-4 * golden::maxAbs(em), 0.0,
                            what + " component");
            COMPARE_NUM(r.r2, e["r2"].toDouble(), 1e-8, 0.0, what + " r2");
            COMPARE_NUM(r.rms, e["rms"].toDouble(), 0.0, 1e-6, what + " rms");
            const QJsonArray b = e["baseline"].toArray();
            COMPARE_NUM(r.baseline.first, b[0].toDouble(), 1e-3 * r.rms, 1e-6, what + " baseline offset");
            COMPARE_NUM(r.baseline.second, b[1].toDouble(), 1e-3 * r.rms, 1e-6, what + " baseline slope");
        }
        try {
            peaks::fit(x, y, {1650, 1700});
            QFAIL("no exception");
        } catch (const Error& ex) {
            QCOMPARE(QString(ex.what()), g["fit_error"].toString());
        }
    }

    void table() {          // formatting, with the Python numbers
        for (const QJsonValue& v : g["fit"].toArray()) {
            const QJsonObject e = v.toObject()["result"].toObject();
            peaks::FitResult r;
            r.r2 = e["r2"].toDouble();
            r.rms = e["rms"].toDouble();
            for (const QJsonValue& q : e["peaks"].toArray()) {
                const QJsonObject o = q.toObject();
                peaks::FitPeak p;
                p.position = o["position"].toDouble();
                p.positionErr = o["position_err"].toDouble();
                p.height = o["height"].toDouble();
                p.fwhm = o["fwhm"].toDouble();
                p.fwhmErr = o["fwhm_err"].toDouble();
                p.area = o["area"].toDouble();
                r.peaks.push_back(p);
            }
            QCOMPARE(r.table(), e["table"].toString());
        }
    }
};

QTEST_GUILESS_MAIN(TestPeaks)
#include "test_peaks.moc"
