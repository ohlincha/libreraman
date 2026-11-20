#pragma once
// Helpers for the golden tests: loading tests/golden/*.json (written by golden/make_golden.py from the
// Python reference) and comparing arrays with tolerances.
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <algorithm>
#include <cmath>

#include "core/spectrum.h"

namespace golden {

inline QString path(const QString& name) { return QStringLiteral(GOLDEN_DIR) + "/" + name; }

inline QJsonObject load(const QString& name) {
    QFile f(path(name));
    if (!f.open(QIODevice::ReadOnly)) qFatal("cannot open golden file %s", qPrintable(f.fileName()));
    return QJsonDocument::fromJson(f.readAll()).object();
}

inline lr::Vec vec(const QJsonValue& v) {
    lr::Vec out;
    for (const QJsonValue& e : v.toArray()) out.push_back(e.toDouble());
    return out;
}

inline lr::Vec every(const lr::Vec& v, int stride) {
    lr::Vec out;
    for (size_t i = 0; i < v.size(); i += size_t(stride)) out.push_back(v[i]);
    return out;
}

inline double maxAbs(const lr::Vec& v) {
    double m = 0.0;
    for (double x : v) m = std::max(m, std::abs(x));
    return m;
}

// Empty if |a - e| <= atol + rtol |e| everywhere, else a description of the worst point.
inline QString compare(const lr::Vec& a, const lr::Vec& e, double atol, double rtol = 0.0) {
    if (a.size() != e.size()) return QString("size %1 != expected %2").arg(a.size()).arg(e.size());
    double worst = 0.0;
    long wi = -1;
    for (size_t i = 0; i < a.size(); ++i) {
        if (std::isnan(a[i]) && std::isnan(e[i])) continue;
        const double excess = std::abs(a[i] - e[i]) - (atol + rtol * std::abs(e[i]));
        if (std::isnan(excess) || excess > worst) {
            worst = std::isnan(excess) ? INFINITY : excess;
            wi = long(i);
        }
    }
    if (wi < 0) return {};
    return QString("index %1: %2 vs expected %3 (atol %4, rtol %5)")
        .arg(wi).arg(a[size_t(wi)], 0, 'g', 17).arg(e[size_t(wi)], 0, 'g', 17).arg(atol).arg(rtol);
}

// True if f() throws an E (QVERIFY_THROWS_EXCEPTION needs Qt 6.3; the project supports 6.2).
template <class E, class F>
bool throws(F&& f) {
    try {
        f();
    } catch (const E&) {
        return true;
    } catch (...) {
        return false;
    }
    return false;
}

inline bool close(double a, double e, double atol, double rtol = 0.0) {
    return std::abs(a - e) <= atol + rtol * std::abs(e);
}

}  // namespace golden

#define COMPARE_VEC(a, e, atol, rtol, what)                                              \
    do {                                                                                 \
        const QString msg_ = golden::compare((a), (e), (atol), (rtol));                  \
        QVERIFY2(msg_.isEmpty(), qPrintable(QString("%1: %2").arg(what, msg_)));         \
    } while (0)

#define COMPARE_NUM(a, e, atol, rtol, what)                                                                     \
    QVERIFY2(golden::close((a), (e), (atol), (rtol)),                                                           \
             qPrintable(QString("%1: %2 vs expected %3").arg(what).arg((a), 0, 'g', 17).arg((e), 0, 'g', 17)))
