#include "response.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QMutex>
#include <QMutexLocker>
#include <QRegularExpression>
#include <QTextStream>
#include <algorithm>
#include <cmath>
#include <map>

#include "calibrate.h"
#include "errors.h"
#include "processing.h"

namespace lr::response {

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

double medianOf(Vec v) {
    if (v.empty()) return kNaN;
    const size_t m = v.size() / 2;
    std::nth_element(v.begin(), v.begin() + long(m), v.end());
    const double hi = v[m];
    if (v.size() % 2) return hi;
    return (*std::max_element(v.begin(), v.begin() + long(m)) + hi) / 2.0;
}

double interp(const Vec& xs, const Vec& ys, double x) {        // xs ascending, clamps at the ends
    if (xs.empty()) return kNaN;
    if (x <= xs.front()) return ys.front();
    if (x >= xs.back()) return ys.back();
    const auto it = std::upper_bound(xs.begin(), xs.end(), x);
    const size_t i = size_t(it - xs.begin());
    const double t = (x - xs[i - 1]) / (xs[i] - xs[i - 1]);
    return ys[i - 1] + t * (ys[i] - ys[i - 1]);
}

QJsonArray toArray(const Vec& v) {
    QJsonArray a;
    for (double d : v) a.append(d);
    return a;
}

Vec toVec(const QJsonValue& v) {
    Vec out;
    for (const QJsonValue& d : v.toArray()) out.push_back(d.toDouble());
    return out;
}

QString stem(const QString& serial, int channel) {
    return QDir(calib::configDir()).filePath(QStringLiteral("response_%1_%2").arg(serial).arg(channel));
}

}  // namespace

// ============================================================================================ reference
const std::vector<double>& Reference::srm2242() {
    // NIST SRM 2242 certificate, relative spectral intensity for 532 nm excitation, x = shift / 1000 cm-1
    // (A0..A5 = 9.71937e-2, 2.28325e-4, -5.86762e-8, 2.16023e-10, -9.77171e-14, 1.15596e-17 with x in cm-1)
    static const std::vector<double> c = {0.0971937, 0.228325, -0.0586762, 0.216023, -0.0977171, 0.0115596};
    return c;
}

QString Reference::label() const {
    switch (kind) {
    case Kind::Srm2242: return QStringLiteral("NIST SRM 2242");
    case Kind::Blackbody: return QStringLiteral("blackbody %1 K").arg(temperatureK, 0, 'f', 0);
    case Kind::Table: return QStringLiteral("table %1").arg(tableName);
    }
    return {};
}

Vec Reference::curve(const Vec& shift, double laserNm) const {
    Vec out(shift.size(), kNaN);
    switch (kind) {
    case Kind::Srm2242: {
        const std::vector<double>& c = coeff.empty() ? srm2242() : coeff;
        for (size_t i = 0; i < shift.size(); ++i) {
            if (shift[i] < range[0] || shift[i] > range[1]) continue;
            const double x = shift[i] / 1000.0;
            double s = 0.0;
            for (size_t k = c.size(); k-- > 0;) s = s * x + c[k];
            out[i] = s;
        }
        break;
    }
    case Kind::Blackbody: {
        constexpr double c2 = 1.438777;                       // second radiation constant, cm K
        for (size_t i = 0; i < shift.size(); ++i) {
            const double v = 1e7 / laserNm - shift[i];         // absolute wavenumber of the scattered light
            if (v <= 0 || temperatureK <= 0) continue;
            out[i] = v * v / std::expm1(c2 * v / temperatureK);
        }
        break;
    }
    case Kind::Table:
        if (tableShift.size() < 2) break;
        for (size_t i = 0; i < shift.size(); ++i)
            if (shift[i] >= tableShift.front() && shift[i] <= tableShift.back())
                out[i] = interp(tableShift, tableValue, shift[i]);
        break;
    }
    return out;
}

Reference readTable(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) throw Error(("cannot read " + path).toStdString());
    std::vector<std::pair<double, double>> rows;
    QTextStream in(&f);
    static const QRegularExpression sep(QStringLiteral("[,;\\t ]+"));
    while (!in.atEnd()) {
        const QString line = in.readLine().trimmed();
        if (line.isEmpty() || line.startsWith(QLatin1Char('#'))) continue;
        const QStringList t = line.split(sep, Qt::SkipEmptyParts);
        if (t.size() < 2) continue;
        bool ok1 = false, ok2 = false;
        const double a = t[0].toDouble(&ok1), b = t[1].toDouble(&ok2);
        if (ok1 && ok2 && std::isfinite(a) && std::isfinite(b)) rows.emplace_back(a, b);   // header lines skipped
    }
    if (rows.size() < 2) throw Error(("no (shift, intensity) rows in " + path).toStdString());
    std::sort(rows.begin(), rows.end());
    Reference r;
    r.kind = Reference::Kind::Table;
    r.tableName = QFileInfo(path).fileName();
    for (const auto& [a, b] : rows) {
        if (!r.tableShift.empty() && a == r.tableShift.back()) continue;
        r.tableShift.push_back(a);
        r.tableValue.push_back(b);
    }
    if (r.tableShift.size() < 2) throw Error(("no (shift, intensity) rows in " + path).toStdString());
    return r;
}

// ============================================================================================ response
Vec Response::factorAt(const Vec& x) const {
    Vec out(x.size(), 1.0);
    if (shift.size() < 2) return out;
    for (size_t i = 0; i < x.size(); ++i) out[i] = interp(shift, factor, std::clamp(x[i], range[0], range[1]));
    return out;
}

Vec Response::apply(const Vec& x, const Vec& y) const {
    const Vec f = factorAt(x);
    Vec out(y.size());
    for (size_t i = 0; i < y.size(); ++i) out[i] = y[i] * (i < f.size() ? f[i] : 1.0);
    return out;
}

QString Response::summary() const {
    return QStringLiteral("%1, %2, %3-%4 cm-1").arg(reference, date).arg(range[0], 0, 'f', 0).arg(range[1], 0, 'f', 0);
}

Response compute(const Vec& shift, const Vec& measured, const Reference& ref, double laserNm, double rawMax) {
    const size_t n = shift.size();
    if (n < 64 || measured.size() != n) throw Error("the reference spectrum is too short");
    for (size_t i = 1; i < n; ++i)
        if (!(shift[i] > shift[i - 1])) throw Error("the reference spectrum must have ascending shifts");
    if (std::isfinite(rawMax) && rawMax >= kSaturated)
        throw Error("the reference spectrum is saturated: shorten the exposure");

    // smooth the (broad) reference: remove spikes, then a wide Savitzky-Golay filter
    Vec sm = processing::despike(measured);
    const int window = std::min<int>(int(n) - (int(n) % 2 == 0 ? 1 : 0), 41);
    if (window >= 5) sm = sig::savgolFilter(sm, window, 2);
    const Vec truth = ref.curve(shift, laserNm);

    double smMax = 0.0;
    for (size_t i = 0; i < n; ++i)
        if (shift[i] >= kMinShift) smMax = std::max(smMax, sm[i]);
    if (smMax <= 0) throw Error("the reference spectrum has no signal above 150 cm-1");

    // valid points: inside the reference's range, above the filter edge, reference well above zero;
    // the longest contiguous run of them is the valid range
    std::vector<bool> ok(n);
    for (size_t i = 0; i < n; ++i)
        ok[i] = shift[i] >= kMinShift && std::isfinite(truth[i]) && truth[i] > 0 && sm[i] > kMinLevel * smMax;
    size_t best = 0, bestLen = 0;
    for (size_t i = 0; i < n;) {
        if (!ok[i]) { ++i; continue; }
        size_t j = i;
        while (j < n && ok[j]) ++j;
        if (j - i > bestLen) { best = i; bestLen = j - i; }
        i = j;
    }
    // the smoothing window reaches across the ends of the run: drop half a window on each side
    const size_t trim = size_t(std::max(window, 1) / 2);
    if (bestLen < 32 + 2 * trim) throw Error("the reference and the detector overlap too little");
    const size_t lo = best + trim, hi = best + bestLen - 1 - trim;

    Vec resid, level;
    for (size_t i = lo; i <= hi; ++i) {
        resid.push_back(measured[i] - sm[i]);
        level.push_back(sm[i]);
    }
    const double mr = medianOf(resid);
    for (double& r : resid) r = std::abs(r - mr);
    const double noise = std::max(1.4826 * medianOf(resid), 1e-9);
    Response r;
    r.snr = medianOf(level) / noise;
    if (r.snr < kMinSnr)
        throw Error(QStringLiteral("the reference is too weak (S/N %1, needs %2): measure longer")
                        .arg(r.snr, 0, 'f', 0).arg(kMinSnr, 0, 'f', 0).toStdString());

    Vec f(n, kNaN), inRange;
    for (size_t i = lo; i <= hi; ++i) {
        f[i] = truth[i] / sm[i];
        inRange.push_back(f[i]);
    }
    const double norm = medianOf(inRange);
    for (size_t i = lo; i <= hi; ++i) f[i] /= norm;
    for (size_t i = 0; i < lo; ++i) f[i] = f[lo];
    for (size_t i = hi + 1; i < n; ++i) f[i] = f[hi];
    const auto [mn, mx] = std::minmax_element(f.begin() + long(lo), f.begin() + long(hi) + 1);
    r.dynamic = *mn > 0 ? *mx / *mn : kNaN;
    if (!(r.dynamic <= kMaxDynamic))
        r.warnings << QStringLiteral("the correction varies %1-fold over the range: check the reference and the "
                                     "exposure").arg(r.dynamic, 0, 'f', 0);
    r.reference = ref.label();
    r.laserNm = laserNm;
    r.range[0] = shift[lo];
    r.range[1] = shift[hi];
    if (r.range[1] - r.range[0] < 1500)
        r.warnings << QStringLiteral("the correction covers only %1-%2 cm-1").arg(r.range[0], 0, 'f', 0).arg(r.range[1], 0, 'f', 0);
    r.shift = shift;
    r.factor = f;
    r.date = QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm"));
    return r;
}

// ============================================================================================ storage
QString activePath(const QString& serial, int channel) { return stem(serial, channel) + QStringLiteral(".json"); }

QString historyPath(const QString& serial, int channel) {
    const QString base = stem(serial, channel) + QLatin1Char('_') +
                         QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"));
    QString p = base + QStringLiteral(".json");
    for (int k = 2; QFile::exists(p); ++k) p = base + QStringLiteral("_%1.json").arg(k);
    return p;
}

void save(const Response& r, const QString& path) {
    QJsonObject o;
    o["kind"] = QStringLiteral("intensity response");
    o["serial"] = r.serial;
    o["channel"] = r.channel;
    o["date"] = r.date;
    o["reference"] = r.reference;
    o["laser_nm"] = r.laserNm;
    o["range"] = QJsonArray{r.range[0], r.range[1]};
    o["exposure_s"] = r.exposure_s;
    o["averages"] = r.averages;
    o["source"] = r.sourceFile;
    o["snr"] = r.snr;
    o["dynamic"] = r.dynamic;
    o["warnings"] = QJsonArray::fromStringList(r.warnings);
    o["shift"] = toArray(r.shift);
    o["factor"] = toArray(r.factor);
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) throw Error(("cannot write " + path).toStdString());
    f.write(QJsonDocument(o).toJson(QJsonDocument::Indented));
}

std::optional<Response> loadFile(const QString& path) {
    QFile f(path);
    if (!f.exists()) return std::nullopt;
    if (!f.open(QIODevice::ReadOnly)) throw Error(("cannot read " + path).toStdString());
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject())
        throw Error(("invalid response file " + path + ": " + err.errorString()).toStdString());
    const QJsonObject o = doc.object();
    Response r;
    r.serial = o["serial"].toString();
    r.channel = o["channel"].toInt(532);
    r.date = o["date"].toString();
    r.reference = o["reference"].toString();
    r.laserNm = o["laser_nm"].toDouble(532.1);
    const QJsonArray rg = o["range"].toArray();
    r.range[0] = rg.at(0).toDouble();
    r.range[1] = rg.at(1).toDouble();
    r.exposure_s = o["exposure_s"].toDouble();
    r.averages = o["averages"].toInt();
    r.sourceFile = o["source"].toString();
    r.snr = o["snr"].toDouble();
    r.dynamic = o["dynamic"].toDouble();
    for (const QJsonValue& w : o["warnings"].toArray()) r.warnings << w.toString();
    r.shift = toVec(o["shift"]);
    r.factor = toVec(o["factor"]);
    if (r.shift.size() < 2 || r.shift.size() != r.factor.size() || !(r.range[1] > r.range[0]))
        throw Error(("invalid response file " + path + ": no correction curve").toStdString());
    return r;
}

std::optional<Response> load(const QString& serial, int channel) { return loadFile(activePath(serial, channel)); }

QString accept(const Response& r) {
    const QString hist = historyPath(r.serial, r.channel);
    save(r, hist);
    save(r, activePath(r.serial, r.channel));
    return hist;
}

bool remove(const QString& serial, int channel) {
    const QString p = activePath(serial, channel);
    return QFile::exists(p) && QFile::remove(p);
}

Vec corrected(const Vec& x, const Vec& y, const QString& serial, int channel, bool* applied) {
    const auto r = active(serial, channel);
    if (applied) *applied = r != nullptr;
    return r ? r->apply(x, y) : y;
}

int channelOf(const QString& meta) {
    static const QRegularExpression num(QStringLiteral("^\\s*(\\d+)"));
    const auto m = num.match(meta);
    return m.hasMatch() ? m.captured(1).toInt() : 0;
}

std::shared_ptr<const Response> active(const QString& serial, int channel) {
    struct Entry {
        QDateTime mtime;
        qint64 size = -1;
        std::shared_ptr<const Response> r;
    };
    static QMutex mutex;
    static std::map<QString, Entry> cache;
    if (serial.isEmpty()) return nullptr;
    const QString p = activePath(serial, channel);
    const QFileInfo fi(p);
    QMutexLocker lock(&mutex);
    Entry& e = cache[p];
    if (!fi.exists()) {
        e = Entry{};
        return nullptr;
    }
    if (e.r && e.mtime == fi.lastModified() && e.size == fi.size()) return e.r;
    e.mtime = fi.lastModified();
    e.size = fi.size();
    try {
        auto r = loadFile(p);
        e.r = r ? std::make_shared<const Response>(std::move(*r)) : nullptr;
    } catch (const std::exception&) {
        e.r = nullptr;
    }
    return e.r;
}

}  // namespace lr::response
