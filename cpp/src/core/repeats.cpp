#include "repeats.h"

#include <QFile>
#include <QFileInfo>
#include <QStringList>
#include <QTextStream>
#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>

#include "acqcheck.h"
#include "errors.h"
#include "processing.h"
#include "pyformat.h"

namespace lr::repeats {

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

double median(Vec v) {
    if (v.empty()) return kNaN;
    std::sort(v.begin(), v.end());
    const size_t m = v.size() / 2;
    return v.size() % 2 ? v[m] : 0.5 * (v[m - 1] + v[m]);
}

MeanSd meanSd(const std::vector<double>& v) {
    MeanSd r;
    double s = 0;
    for (double d : v)
        if (std::isfinite(d)) s += d, ++r.n;
    if (r.n == 0) return r;
    r.mean = s / r.n;
    if (r.n > 1) {
        double q = 0;
        for (double d : v)
            if (std::isfinite(d)) q += (d - r.mean) * (d - r.mean);
        r.sd = std::sqrt(q / (r.n - 1));
    }
    return r;
}

Vec interpolate(const Vec& x, const Vec& y, const Vec& at) {
    Vec out(at.size(), 0.0);
    if (x.size() < 2) return out;
    for (size_t i = 0; i < at.size(); ++i) {
        const auto it = std::lower_bound(x.begin(), x.end(), at[i]);
        if (it == x.begin()) out[i] = y.front();
        else if (it == x.end()) out[i] = y[x.size() - 1];
        else {
            const size_t k = size_t(it - x.begin());
            const double f = (at[i] - x[k - 1]) / (x[k] - x[k - 1]);
            out[i] = y[k - 1] + f * (y[k] - y[k - 1]);
        }
    }
    return out;
}

bool sameAxis(const Vec& a, const Vec& b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (std::abs(a[i] - b[i]) > 1e-6) return false;
    return true;
}

struct Corrected {                            // a spectrum in the band range, background removed, smoothed
    Vec x, y;
};

Corrected corrected(const Vec& x, const Vec& y) {
    Corrected c;
    for (size_t i = 0; i < x.size() && i < y.size(); ++i)
        if (x[i] >= acq::kBandMin && x[i] <= acq::kBandMax) c.x.push_back(x[i]), c.y.push_back(y[i]);
    if (c.y.size() < 32) return {};
    Vec d = processing::despike(c.y);
    const Vec b = processing::baselineSnip4(d);
    for (size_t i = 0; i < d.size(); ++i) d[i] -= b[i];
    c.y = processing::smooth(d, 7, 2);
    return c;
}

// Position and height of the maximum within ± w of `at` (parabolic vertex).
std::pair<double, double> bandAt(const Corrected& c, double at, double w) {
    size_t best = size_t(-1);
    for (size_t i = 0; i < c.x.size(); ++i)
        if (std::abs(c.x[i] - at) <= w && (best == size_t(-1) || c.y[i] > c.y[best])) best = i;
    if (best == size_t(-1)) return {kNaN, kNaN};
    if (best == 0 || best + 1 >= c.y.size()) return {c.x[best], c.y[best]};
    const double a = c.y[best - 1], b = c.y[best], d = c.y[best + 1];
    const double den = a - 2 * b + d;
    if (den >= 0) return {c.x[best], b};
    const double off = std::clamp(0.5 * (a - d) / den, -0.5, 0.5);
    return {c.x[best] + off * 0.5 * (c.x[best + 1] - c.x[best - 1]), b - 0.25 * (a - d) * off};
}

}  // namespace

QString MeanSd::text(int decimals) const {
    if (n < 2) return QString::number(mean, 'f', decimals);
    return QStringLiteral("%1 ± %2").arg(mean, 0, 'f', decimals).arg(sd, 0, 'f', decimals);
}

Result analyse(const std::vector<Spectrum>& spectra, const std::vector<bool>& exclude, const Options& o) {
    Result r;
    const size_t m = spectra.size();
    if (m == 0) return r;
    r.x = spectra[0].shift;
    const size_t n = r.x.size();
    std::vector<Vec> ys(m);
    for (size_t k = 0; k < m; ++k)
        ys[k] = sameAxis(spectra[k].shift, r.x) ? spectra[k].intensity
                                                : interpolate(spectra[k].shift, spectra[k].intensity, r.x);
    r.included.assign(m, true);
    for (size_t k = 0; k < m && k < exclude.size(); ++k) r.included[k] = !exclude[k];
    r.nIncluded = int(std::count(r.included.begin(), r.included.end(), true));

    // ---- outliers (all spectra, whether included or not)
    std::vector<size_t> idx;
    for (size_t i = 0; i < n; ++i)
        if (r.x[i] >= acq::kBandMin && r.x[i] <= acq::kBandMax) idx.push_back(i);
    r.distance.assign(m, kNaN);
    r.outlier.assign(m, false);
    if (m >= 3 && idx.size() >= 16) {
        std::vector<Vec> unit(m, Vec(idx.size()));
        for (size_t k = 0; k < m; ++k) {
            double q = 0;
            for (size_t j = 0; j < idx.size(); ++j) q += ys[k][idx[j]] * ys[k][idx[j]];
            const double norm = q > 0 ? std::sqrt(q) : 1.0;
            for (size_t j = 0; j < idx.size(); ++j) unit[k][j] = ys[k][idx[j]] / norm;
        }
        Vec med(idx.size()), col(m);
        for (size_t j = 0; j < idx.size(); ++j) {
            for (size_t k = 0; k < m; ++k) col[k] = unit[k][j];
            med[j] = median(col);
        }
        double mq = 0;
        for (double v : med) mq += v * v;
        const double mnorm = mq > 0 ? std::sqrt(mq) : 1.0;
        for (size_t k = 0; k < m; ++k) {
            double q = 0;
            for (size_t j = 0; j < idx.size(); ++j) q += (unit[k][j] - med[j]) * (unit[k][j] - med[j]);
            r.distance[k] = std::sqrt(q) / mnorm;
        }
        const double md = median(r.distance);
        Vec dev(m);
        for (size_t k = 0; k < m; ++k) dev[k] = std::abs(r.distance[k] - md);
        const double mad = median(dev);
        for (size_t k = 0; k < m; ++k) {
            const double z = mad > 0 ? 0.6745 * (r.distance[k] - md) / mad
                                     : (r.distance[k] > md ? std::numeric_limits<double>::infinity() : 0.0);
            r.outlier[k] = z > o.outlierZ && r.distance[k] > o.minDistance;
        }
    }

    // ---- intensity of each spectrum (packing, focus)
    {
        std::vector<double> tot;
        for (size_t k = 0; k < m; ++k) {
            if (!r.included[k]) continue;
            double s = 0;
            for (size_t i : idx) s += ys[k][i];
            tot.push_back(s);
        }
        r.totalIntensity = meanSd(tot);
    }
    if (r.nIncluded == 0) return r;

    // ---- mean and spread
    r.mean.assign(n, 0.0);
    r.sd.assign(n, 0.0);
    for (size_t k = 0; k < m; ++k)
        if (r.included[k])
            for (size_t i = 0; i < n; ++i) r.mean[i] += ys[k][i];
    for (double& v : r.mean) v /= r.nIncluded;
    if (r.nIncluded > 1) {
        for (size_t k = 0; k < m; ++k)
            if (r.included[k])
                for (size_t i = 0; i < n; ++i) r.sd[i] += (ys[k][i] - r.mean[i]) * (ys[k][i] - r.mean[i]);
        for (double& v : r.sd) v = std::sqrt(v / (r.nIncluded - 1));
    }

    // ---- bands: picked on the mean, measured in each spectrum
    const Corrected cm = corrected(r.x, r.mean);
    if (cm.y.empty()) return r;
    const double ymax = *std::max_element(cm.y.begin(), cm.y.end());
    if (ymax <= 0) return r;
    const sig::PeakSet ps = sig::findPeaks(cm.y, o.minProminence * ymax);
    std::vector<size_t> order(ps.peaks.size());
    std::iota(order.begin(), order.end(), size_t(0));
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return ps.prominences[a] > ps.prominences[b]; });
    if (int(order.size()) > o.maxBands) order.resize(size_t(o.maxBands));
    std::vector<double> nominal;
    for (size_t k : order) nominal.push_back(bandAt(cm, cm.x[size_t(ps.peaks[k])], 0.1).first);
    if (nominal.empty()) return r;
    const double strongest = nominal.front();             // order is by prominence
    std::sort(nominal.begin(), nominal.end());
    std::vector<Corrected> cs(m);
    for (size_t k = 0; k < m; ++k) cs[k] = corrected(r.x, ys[k]);
    for (double nom : nominal) {
        BandStat b;
        b.nominal = nom;
        for (size_t k = 0; k < m; ++k) {
            const auto [p, h] = cs[k].y.empty() ? std::pair<double, double>{kNaN, kNaN} : bandAt(cs[k], nom, o.window_cm);
            b.positions.push_back(p);
            b.heights.push_back(h);
        }
        r.bands.push_back(std::move(b));
        if (nom == strongest) r.reference = int(r.bands.size()) - 1;
    }
    for (BandStat& b : r.bands) {
        std::vector<double> pos, h, ratio;
        for (size_t k = 0; k < m; ++k) {
            if (!r.included[k]) continue;
            pos.push_back(b.positions[k]);
            h.push_back(b.heights[k]);
            const double ref = r.bands[size_t(r.reference)].heights[k];
            ratio.push_back(std::isfinite(ref) && ref > 0 ? b.heights[k] / ref : kNaN);
        }
        b.position = meanSd(pos);
        b.height = meanSd(h);
        b.ratio = meanSd(ratio);
    }
    return r;
}

namespace {

QString excludedList(const Result& r) {
    QStringList l;
    for (size_t k = 0; k < r.included.size(); ++k)
        if (!r.included[k]) l << QString::number(k + 1);
    return l.join(QStringLiteral(", "));
}

QString outlierList(const Result& r) {
    QStringList l;
    for (size_t k = 0; k < r.outlier.size(); ++k)
        if (r.outlier[k]) l << QStringLiteral("%1 (distance %2)").arg(k + 1).arg(r.distance[k], 0, 'f', 2);
    return l.isEmpty() ? QStringLiteral("none") : l.join(QStringLiteral(", "));
}

QString repeatsText(const Result& r) {
    QString t = QStringLiteral("%1 of %2").arg(r.nIncluded).arg(r.included.size());
    const QString ex = excludedList(r);
    if (!ex.isEmpty()) t += QStringLiteral(" (excluded: %1)").arg(ex);
    return t;
}

QString bandKey(const BandStat& b) { return QStringLiteral("band_%1").arg(b.nominal, 0, 'f', 0); }

QString bandText(const Result& r, const BandStat& b) {
    const double ref = r.reference >= 0 ? r.bands[size_t(r.reference)].nominal : kNaN;
    return QStringLiteral("position %1 cm-1; height %2; ratio to %3 cm-1 %4")
        .arg(b.position.text(1), b.height.text(0))
        .arg(ref, 0, 'f', 0)
        .arg(b.ratio.text(3));
}

}  // namespace

Spectrum meanSpectrum(const std::vector<Spectrum>& spectra, const Result& r) {
    Spectrum sp;
    if (spectra.empty() || r.mean.empty()) return sp;
    const Spectrum& first = spectra.front();
    sp.channel = first.channel;
    sp.exposure_s = first.exposure_s;
    sp.averages = first.averages;
    sp.shift = r.x;
    sp.intensity = r.mean;
    bool raw = true;
    for (size_t k = 0; k < spectra.size(); ++k)
        if (r.included[k] && (spectra[k].signal.size() != r.x.size() || spectra[k].background.size() != r.x.size() ||
                              !sameAxis(spectra[k].shift, r.x)))
            raw = false;
    if (raw) {
        sp.signal.assign(r.x.size(), 0.0);
        sp.background.assign(r.x.size(), 0.0);
        for (size_t k = 0; k < spectra.size(); ++k)
            if (r.included[k])
                for (size_t i = 0; i < r.x.size(); ++i) {
                    sp.signal[i] += spectra[k].signal[i] / r.nIncluded;
                    sp.background[i] += spectra[k].background[i] / r.nIncluded;
                }
    }
    sp.t = spectra.back().t;
    for (const auto& kv : first.meta)
        if (kv.first != QLatin1String("file") && !kv.first.startsWith(QLatin1String("repeat")) &&
            !kv.first.startsWith(QLatin1String("laser_damage")) && kv.first != QLatin1String("snr"))
            sp.meta.push_back(kv);
    sp.setMeta(QStringLiteral("mode"), QStringLiteral("mean of repeated measurements (vial shaken/rotated between)"));
    sp.setMeta(QStringLiteral("repeats"), repeatsText(r));
    QStringList files;
    for (const Spectrum& s : spectra) files << QFileInfo(s.metaValue(QStringLiteral("file"))).fileName();
    sp.setMeta(QStringLiteral("repeat_files"), files.join(QStringLiteral("; ")));
    sp.setMeta(QStringLiteral("outliers"), outlierList(r));
    for (const BandStat& b : r.bands) sp.setMeta(bandKey(b), bandText(r, b));
    return sp;
}

void writeSummary(const QString& path, const std::vector<Spectrum>& spectra, const Result& r, const QString& sample) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate))
        throw Error(("cannot write " + path).toStdString());
    QTextStream o(&f);
    o << "# LibreRaman repeated measurements of one vial, "
      << (spectra.empty() ? QString() : iso(spectra.back().t)) << "\n";
    o << "# sample: " << sample << "\n";
    o << "# repeats: " << repeatsText(r) << "\n";
    o << "# outliers: " << outlierList(r) << "\n";
    o << "# total_intensity: " << r.totalIntensity.text(0) << "\n";
    for (const BandStat& b : r.bands) o << "# " << bandKey(b) << ": " << bandText(r, b) << "\n";
    for (size_t k = 0; k < spectra.size(); ++k)
        o << "# r" << (k + 1) << ": " << QFileInfo(spectra[k].metaValue(QStringLiteral("file"))).fileName()
          << (r.included[k] ? QString() : QStringLiteral(" (excluded)"))
          << (r.outlier[k] ? QStringLiteral(" (outlier)") : QString()) << "\n";
    o << "raman_shift_cm1,mean,sd";
    for (size_t k = 0; k < spectra.size(); ++k) o << ",r" << (k + 1);
    o << "\n";
    for (size_t i = 0; i < r.x.size(); ++i) {
        o << py::fixed(r.x[i], 2) << "," << (i < r.mean.size() ? py::fixed(r.mean[i], 1) : QString()) << ","
          << (i < r.sd.size() ? py::fixed(r.sd[i], 1) : QString());
        for (const Spectrum& s : spectra) {
            o << ",";
            if (sameAxis(s.shift, r.x) && i < s.intensity.size()) o << py::fixed(s.intensity[i], 1);
        }
        o << "\n";
    }
}

}  // namespace lr::repeats
