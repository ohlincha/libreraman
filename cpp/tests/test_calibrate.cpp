// Golden tests for core/calibrate and core/standards (Python: calibrate.py, standards.py), data from
// golden/calibrate.json, golden/standards.json and golden/calibration_99002169_532.json (Python save()).
//
// The calibration code is plain arithmetic in the same order as the Python version: values are compared at
// 1e-12 relative (1e-9 where they pass through the factory calibration, whose polynomial is evaluated with
// repeated multiplication in C++ and with pow() in Python). The text report is compared exactly.
#include <QJsonArray>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

#include "core/calibrate.h"
#include "core/calibration.h"
#include "core/errors.h"
#include "core/standards.h"
#include "golden_util.h"

using namespace lr;

namespace {

Poly polyFrom(const QJsonObject& o) {
    Poly p;
    p.coeff = golden::vec(o["coeff"]);
    p.x0 = o["x0"].toDouble();
    p.scale = o["scale"].toDouble();
    return p;
}

QString comparePoly(const Poly& a, const QJsonObject& e, double rtol) {
    const QString m = golden::compare(a.coeff, golden::vec(e["coeff"]), 1e-12, rtol);
    if (!m.isEmpty()) return "coeff " + m;
    if (!golden::close(a.x0, e["x0"].toDouble(), 0, 1e-15) || !golden::close(a.scale, e["scale"].toDouble(), 0, 1e-15))
        return "x0/scale";
    return {};
}

std::vector<calib::Match> matchesFrom(const QJsonArray& a) {
    std::vector<calib::Match> out;
    for (const QJsonValue& v : a) {
        const QJsonArray m = v.toArray();
        out.push_back({m[0].toDouble(), m[1].toDouble(), m[2].toInt(), m[3].toDouble()});
    }
    return out;
}

QString compareMatches(const std::vector<calib::Match>& a, const QJsonArray& e, double rtol) {
    if (a.size() != size_t(e.size())) return QString("%1 matches, expected %2").arg(a.size()).arg(e.size());
    const auto em = matchesFrom(e);
    for (size_t i = 0; i < a.size(); ++i)
        if (!golden::close(a[i].pixel, em[i].pixel, 0, rtol) || a[i].reference != em[i].reference ||
            a[i].strength != em[i].strength || !golden::close(a[i].height, em[i].height, 1e-12, rtol))
            return QString("match %1").arg(i);
    return {};
}

QString compareResiduals(const std::vector<calib::Residual>& a, const QJsonArray& e, double atol) {
    if (a.size() != size_t(e.size())) return "count";
    for (size_t i = 0; i < a.size(); ++i) {
        const QJsonArray r = e[int(i)].toArray();
        if (a[i].reference != r[0].toDouble() || !golden::close(a[i].residual, r[1].toDouble(), atol) ||
            !golden::close(a[i].pixel, r[2].toDouble(), atol))
            return QString("residual %1: %2 vs %3").arg(i).arg(a[i].residual, 0, 'g', 17).arg(r[1].toDouble(), 0, 'g', 17);
    }
    return {};
}

calib::ShiftFn factory() {
    const ChannelCal ch = defaultCalibration().channels.at(532);
    return [ch](double p) { return ch.shift(p); };
}

#define CHECK_MSG(expr)                                \
    do {                                               \
        const QString m_ = (expr);                     \
        QVERIFY2(m_.isEmpty(), qPrintable(m_));        \
    } while (0)

}  // namespace

class TestCalibrate : public QObject {
    Q_OBJECT
    QJsonObject g, spectra;
    Vec spectrum(const QString& n) const { return golden::vec(spectra[n]); }
    std::vector<std::pair<Vec, QString>> samples() const {
        return {{spectrum("carbon tetrachloride"), "carbon tetrachloride"}, {spectrum("chloroform"), "chloroform"},
                {spectrum("ethanol"), "ethanol"}};
    }

private slots:
    void initTestCase() {
        g = golden::load("calibrate.json");
        spectra = g["spectra"].toObject();
    }

    void smoothAndBaseline() {
        const QJsonObject s = g["smooth"].toObject();
        const Vec in = golden::vec(s["in"]);
        COMPARE_VEC(calib::smooth(in, s["w"].toInt()), golden::vec(s["out"]), 1e-12, 1e-13, "smooth");
        const QJsonObject b = g["baseline"].toObject();
        COMPARE_VEC(calib::baseline(in, b["w"].toInt()), golden::vec(b["out"]), 1e-12, 1e-13, "baseline");
    }

    void polyfit() {
        const QJsonObject p = g["polyfit"].toObject();
        const Vec x = golden::vec(p["x"]), y = golden::vec(p["y"]);
        const Poly a = calib::polyfit(x, y, p["deg"].toInt(), golden::vec(p["w"]));
        CHECK_MSG(comparePoly(a, p["poly"].toObject(), 1e-12));
        for (const QJsonValue& v : p["eval"].toArray()) {
            const QJsonArray e = v.toArray();
            COMPARE_NUM(a(e[0].toDouble()), e[1].toDouble(), 1e-12, 1e-12, "polyval");
        }
        CHECK_MSG(comparePoly(calib::polyfit(x, y, 2), p["unweighted"].toObject(), 1e-12));
        try {
            calib::solve({{1.0, 2.0}, {2.0, 4.0}}, {1.0, 2.0});
            QFAIL("no exception");
        } catch (const Error& e) {
            QCOMPARE(QString(e.what()), g["solve_error"].toString());
        }
    }

    void factoryCalibration() {         // the shift function used as the starting point below
        for (const QJsonValue& v : g["shift_of"].toArray())
            COMPARE_NUM(factory()(v.toArray()[0].toDouble()), v.toArray()[1].toDouble(), 1e-9, 0, "factory shift");
    }

    void findPeaks() {
        const QJsonObject fp = g["find_peaks"].toObject();
        for (const QString& n : fp.keys()) {
            const auto a = calib::findPeaks(spectrum(n));
            const QJsonArray e = fp[n].toArray();
            QCOMPARE(a.size(), size_t(e.size()));
            for (size_t i = 0; i < a.size(); ++i) {
                COMPARE_NUM(a[i].pixel, e[int(i)].toArray()[0].toDouble(), 0, 1e-12, n);
                COMPARE_NUM(a[i].height, e[int(i)].toArray()[1].toDouble(), 1e-9, 1e-12, n);
            }
        }
        const auto a = calib::findPeaks(spectrum("ethanol"), 0.1, 5);
        QCOMPARE(a.size(), size_t(g["find_peaks_args"].toArray().size()));
    }

    void match() {
        const QJsonObject m = g["match"].toObject();
        for (const QString& n : m.keys()) {
            const QJsonObject c = m[n].toObject();
            CHECK_MSG(compareMatches(calib::match(calib::findPeaks(spectrum(n)), factory(), c["standard"].toString()),
                                     c["matches"].toArray(), 1e-12));
        }
        CHECK_MSG(compareMatches(calib::match(calib::findPeaks(spectrum("ethanol")), factory(), "ethanol", 4.0),
                                 g["match_window"].toArray(), 1e-12));
    }

    void calibrate() {
        const QJsonObject e = g["calibrate"].toObject();
        const calib::Result r = calib::calibrate(samples(), factory());
        CHECK_MSG(comparePoly(r.poly, e["poly"].toObject(), 1e-9));
        CHECK_MSG(compareResiduals(r.residuals, e["residuals"].toArray(), 1e-9));
        COMPARE_NUM(r.rms, e["rms"].toDouble(), 0, 1e-9, "rms");
        QCOMPARE(r.degree, e["degree"].toInt());
        QCOMPARE(r.matches.size(), size_t(3));
        for (size_t i = 0; i < r.matches.size(); ++i) {
            const QJsonArray em = e["matches"].toArray()[int(i)].toArray();
            QCOMPARE(r.matches[i].first, em[0].toString());
            CHECK_MSG(compareMatches(r.matches[i].second, em[1].toArray(), 1e-12));
        }
        QCOMPARE(r.standards, (std::vector<QString>{"carbon tetrachloride", "chloroform", "ethanol"}));
        QCOMPARE(r.date.size(), 16);
        QCOMPARE(calib::report(r), e["report"].toString());
        QCOMPARE(calib::report(r, factory()), e["report_before"].toString());
        const UserCalibration u = r.userCalibration();
        QCOMPARE(u.rms, r.rms);
        QCOMPARE(u.poly.coeff, r.poly.coeff);

        const QJsonObject e2 = g["calibrate_deg2"].toObject();
        const calib::Result r2 = calib::calibrate({samples()[0]}, factory(), 2);
        CHECK_MSG(comparePoly(r2.poly, e2["poly"].toObject(), 1e-9));
        QCOMPARE(calib::report(r2), e2["report"].toString());
    }

    void fitSpecialCases() {
        const QJsonObject one = g["fit_one"].toObject();
        const calib::FitOutput f1 = calib::fit(matchesFrom(one["matches"].toArray()), std::nullopt, factory());
        // degree-6 fit of the factory curve: the highest coefficients amplify the last-digit differences of the
        // factory evaluation (repeated multiplication vs pow); compare them loosely and the curve tightly
        CHECK_MSG(comparePoly(f1.poly, one["poly"].toObject(), 1e-7));
        for (double px = 0; px < 2048; px += 97)
            COMPARE_NUM(f1.poly(px), polyFrom(one["poly"].toObject())(px), 1e-8, 0, "one-point curve");
        QCOMPARE(f1.degree, 0);
        CHECK_MSG(compareResiduals(f1.residuals, one["residuals"].toArray(), 1e-9));
        COMPARE_NUM(f1.rms, one["rms"].toDouble(), 1e-8, 0, "one-point rms");   // approximation error only
        QVERIFY(f1.rms < 0.05);
        const QJsonObject two = g["fit_two"].toObject();
        const calib::FitOutput f2 = calib::fit(matchesFrom(two["matches"].toArray()));
        CHECK_MSG(comparePoly(f2.poly, two["poly"].toObject(), 1e-12));
        CHECK_MSG(compareResiduals(f2.residuals, two["residuals"].toArray(), 1e-9));
        try {
            calib::fit({});
            QFAIL("no exception");
        } catch (const Error& e) {
            QCOMPARE(QString(e.what()), g["fit_error_none"].toString());
        }
        try {
            calib::fit(matchesFrom(one["matches"].toArray()));
            QFAIL("no exception");
        } catch (const Error& e) {
            QCOMPARE(QString(e.what()), g["fit_error_one"].toString());
        }
    }

    // ---- correction model (hardware test round: CCl4 alone extrapolated to 27000 cm-1 at the last pixel)
    void autoDegree() {
        QCOMPARE(calib::autoDegree({300.0}), 0);
        QCOMPARE(calib::autoDegree({257, 287, 333, 433, 441}), 0);              // CCl4: 9 % of the detector
        QCOMPARE(calib::autoDegree({300, 900}), 1);
        QCOMPARE(calib::autoDegree({300, 600, 900}), 1);
        QCOMPARE(calib::autoDegree({325, 474, 533, 548, 680, 1297, 1321, 1343}), 2);   // ethanol: 50 %
        QCOMPARE(calib::autoDegree({100, 400, 700, 1000, 1300, 1600}), 3);
        QCOMPARE(calib::autoDegree({100, 120, 140, 160, 180, 1600}), 2);       // not spread out
        QCOMPARE(calib::autoDegree({10, 30, 50, 70}, 256), 0);                 // 1064 nm detector: 23 %
        QCOMPARE(calib::autoDegree({10, 50, 90}, 256), 1);                     // 31 %
    }

    void correctionExtrapolatesLinearly() {
        const std::vector<calib::Match> m = {{300, 0, 3, 1}, {500, 0, 2, 1}, {700, 0, 3, 1}, {900, 0, 1, 1}};
        const calib::ShiftFn zero = [](double) { return 0.0; };
        std::vector<calib::Match> q = m;
        for (auto& x : q) x.reference = 1e-4 * (x.pixel - 600) * (x.pixel - 600);   // a parabola
        const calib::ShiftFn d2 = calib::correction(q, 2, zero);
        QVERIFY(std::abs(d2(500) - 1.0) < 1e-9);
        // outside 300..900: straight lines (second difference 0), continuous at the edges
        QVERIFY(std::abs(d2(100) - 2 * d2(200) + d2(300)) < 1e-9);
        QVERIFY(std::abs(d2(2047) - 2 * d2(1500) + d2(953)) < 1e-9);
        QVERIFY(std::abs(d2(900.0001) - d2(900)) < 1e-3);
        const calib::ShiftFn d0 = calib::correction(q, 0, zero);
        QCOMPARE(d0(0), d0(2047));                                               // offset: constant
    }

    void ccl4AloneDoesNotExtrapolate() {
        const calib::ShiftFn f = factory();
        const calib::Result r = calib::calibrate({samples()[0]}, f);
        QCOMPARE(r.degree, 0);                                                   // narrow coverage: offset
        double worst = 0;
        for (int p = 1000; p < 2048; ++p) worst = std::max(worst, std::abs(r.poly(p) - f(p)));
        QVERIFY2(worst < 15.0, qPrintable(QString::number(worst)));
        QVERIFY(r.rms < 2.0);
        QVERIFY(calib::report(r).contains("the offset applies to the whole detector"));
        // the old model (free polynomial, auto cubic) is what went wrong on the instrument
        std::vector<calib::Match> all = r.matches[0].second;
        const calib::FitOutput old = calib::fit(all, 3);
        QVERIFY(std::abs(old.poly(2047.0) - f(2047.0)) > 1000.0);
        // even a linear correction through these bands runs away by ~20 cm-1 (the GUI refuses that: rule c)
        const calib::Result r1 = calib::calibrate({samples()[0]}, f, 1);
        QVERIFY(std::abs(r1.poly(2047.0) - f(2047.0)) > 15.0);
    }

    void wideCoverageCalibratesWell() {
        const calib::ShiftFn f = factory();
        const calib::Result r = calib::calibrate({samples()[0], samples()[1]}, f);     // CCl4 + chloroform
        QCOMPARE(r.degree, 2);
        QVERIFY(r.rms < 1.5);
        for (int p = 0; p < 2048; p += 7) QVERIFY(std::abs(r.poly(p) - f(p)) < 5.0);
        const calib::Result e = calib::calibrate({samples()[2]}, f);                   // ethanol alone
        QCOMPARE(e.degree, 2);
        QVERIFY(e.rms < 1.5);
        // the stored polynomial reproduces factory + correction (largest error, ~0.3 cm-1, at the kink where the
        // quadratic correction turns into its linear continuation)
        std::vector<calib::Match> all;
        for (const auto& [n, m] : r.matches) all.insert(all.end(), m.begin(), m.end());
        const calib::ShiftFn d = calib::correction(all, 2, f);
        for (int p = 0; p < 2048; p += 3) QVERIFY(std::abs(r.poly(p) - f(p) - d(p)) < 0.5);
    }

    void matchingFromAWrongCalibration() {
        // the calibration in use = the old CCl4-only cubic: only 2 of 8 ethanol bands are found; from the factory
        // calibration all 8
        const calib::ShiftFn f = factory();
        const calib::Result r = calib::calibrate({samples()[0]}, f);
        const calib::FitOutput bad = calib::fit(r.matches[0].second, 3);
        const Poly bp = bad.poly;
        const calib::ShiftFn wrong = [bp](double p) { return bp(p); };
        const auto peaks = calib::findPeaks(spectrum("ethanol"));
        QCOMPARE(calib::match(peaks, wrong, "ethanol").size(), size_t(2));
        QCOMPARE(calib::match(peaks, f, "ethanol").size(), size_t(8));
        // matched against the wrong calibration but fitted to the factory one: still a sane curve
        const calib::Result e = calib::calibrate({samples()[2]}, f, std::nullopt, 12.0, f);
        QCOMPARE(e.degree, 2);
    }

    void saveAndLoad() {
        const calib::Result r = calib::calibrate(samples(), factory());
        QTemporaryDir dir;
        const QString path = calib::save(r, 532, "99002169", dir.filePath("sub/cal.json"));
        QFile f(path);
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QJsonObject a = QJsonDocument::fromJson(f.readAll()).object();
        QFile pf(golden::path(g["saved"].toString()));
        QVERIFY(pf.open(QIODevice::ReadOnly));
        const QJsonObject e = QJsonDocument::fromJson(pf.readAll()).object();
        QCOMPARE(a.keys(), e.keys());                                   // same structure as Python's save()
        CHECK_MSG(comparePoly(polyFrom(a["poly"].toObject()), e["poly"].toObject(), 1e-9));
        QCOMPARE(a["poly"].toObject().keys(), e["poly"].toObject().keys());
        QCOMPARE(a["channel"].toInt(), 532);
        QCOMPARE(a["serial"].toString(), QString("99002169"));
        QCOMPARE(a["standards"], e["standards"]);
        QCOMPARE(a["degree"], e["degree"]);
        QCOMPARE(a["residuals"].toArray().size(), e["residuals"].toArray().size());
        QCOMPARE(a["residuals"].toArray()[0].toArray().size(), 3);
        const QJsonArray am = a["matches"].toArray(), em = e["matches"].toArray();
        QCOMPARE(am.size(), em.size());
        for (int i = 0; i < am.size(); ++i) {
            QCOMPARE(am[i].toArray()[0], em[i].toArray()[0]);
            CHECK_MSG(compareMatches(matchesFrom(am[i].toArray()[1].toArray()), em[i].toArray()[1].toArray(), 1e-12));
        }
        // the (temporary) loader reads both the C++ and the Python file
        for (const QString& p : {path, golden::path(g["saved"].toString())}) {
            const auto u = calib::load(532, "99002169", p);
            QVERIFY(u.has_value());
            COMPARE_NUM(u->rms, r.rms, 0, 1e-9, "loaded rms");
            QCOMPARE(u->standards, r.standards);
            COMPARE_NUM((u->poly)(1000.0), r.poly(1000.0), 1e-9, 0, "loaded poly");
        }
        QVERIFY(!calib::load(532, "none", dir.filePath("missing.json")));
        // default location: $XDG_CONFIG_HOME/libreramen/calibration_<serial>_<channel>.json
        const QByteArray old = qgetenv("XDG_CONFIG_HOME");
        qputenv("XDG_CONFIG_HOME", dir.path().toUtf8());
        QCOMPARE(calib::configDir(), dir.path() + "/libreramen");
        const QString def = calib::save(r, 1064, "SN1");
        QCOMPARE(QFileInfo(def).fileName(), QString("calibration_SN1_1064.json"));
        QVERIFY(calib::load(1064, "SN1").has_value());
        if (old.isEmpty()) qunsetenv("XDG_CONFIG_HOME");
        else qputenv("XDG_CONFIG_HOME", old);
    }

    void standards() {
        const QJsonObject s = golden::load("standards.json");
        const QJsonArray all = s["standards"].toArray();
        QCOMPARE(standards::all().size(), size_t(all.size()));
        for (int i = 0; i < all.size(); ++i) {
            const QJsonObject e = all[i].toObject();
            const standards::Standard& a = standards::all()[size_t(i)];
            QCOMPARE(a.name, e["name"].toString());
            QCOMPARE(a.kind, e["kind"].toString());
            QCOMPARE(a.source, e["source"].toString());
            QCOMPARE(a.note, e["note"].toString());
            QCOMPARE(QJsonArray::fromStringList(a.aka), e["aka"].toArray());
            const auto b = standards::bands(a.name);
            const QJsonArray eb = e["bands"].toArray();
            QCOMPARE(b.size(), size_t(eb.size()));
            for (size_t k = 0; k < b.size(); ++k) {
                const QJsonArray t = eb[int(k)].toArray();
                QCOMPARE(b[k].position, t[0].toDouble());
                QCOMPARE(b[k].strength, t[1].toInt());
                QCOMPARE(b[k].tolerance, t[2].toDouble());
            }
        }
        const QJsonObject f = s["find"].toObject();
        for (const QString& q : f.keys()) QCOMPARE(standards::find(q).name, f[q].toString());
        try {
            standards::find("unobtainium");
            QFAIL("no exception");
        } catch (const Error& e) {
            QCOMPARE(QString(e.what()), s["error"].toString());
        }
    }
};

QTEST_GUILESS_MAIN(TestCalibrate)
#include "test_calibrate.moc"
