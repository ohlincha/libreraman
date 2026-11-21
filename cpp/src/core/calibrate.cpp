#include "calibrate.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>
#include <algorithm>
#include <cmath>
#include <tuple>

#include "errors.h"
#include "standards.h"

namespace lr::calib {

// ----------------------------------------------------------------------------- small numerics
Vec smooth(const Vec& y, int w) {                                 // _smooth
    const long h = w / 2, n = long(y.size());
    Vec pre(y.size() + 1, 0.0), out(y.size());
    double s = 0.0;
    for (size_t i = 0; i < y.size(); ++i) { s += y[i]; pre[i + 1] = s; }
    for (long i = 0; i < n; ++i) {
        const long a = std::max(0L, i - h), b = std::min(n, i + h + 1);
        out[size_t(i)] = (pre[size_t(b)] - pre[size_t(a)]) / double(b - a);
    }
    return out;
}

Vec baseline(const Vec& y, int w) {                               // _baseline
    const long h = w / 2, n = long(y.size());
    Vec mins(y.size());
    for (long i = 0; i < n; ++i) {
        const long a = std::max(0L, i - h), b = std::min(n, i + h + 1);
        mins[size_t(i)] = *std::min_element(y.begin() + a, y.begin() + b);
    }
    return smooth(mins, w);
}

Vec solve(std::vector<Vec> m, Vec b) {                            // _solve
    const size_t n = b.size();
    for (size_t i = 0; i < n; ++i) m[i].push_back(b[i]);
    for (size_t c = 0; c < n; ++c) {
        size_t piv = c;
        for (size_t r = c + 1; r < n; ++r)
            if (std::abs(m[r][c]) > std::abs(m[piv][c])) piv = r;
        std::swap(m[c], m[piv]);
        if (std::abs(m[c][c]) < 1e-300) throw Error("singular fit");
        for (size_t r = 0; r < n; ++r) {
            if (r == c) continue;
            const double f = m[r][c] / m[c][c];
            for (size_t k = c; k <= n; ++k) m[r][k] -= f * m[c][k];
        }
    }
    Vec out(n);
    for (size_t i = 0; i < n; ++i) out[i] = m[i][n] / m[i][i];
    return out;
}

Poly polyfit(const Vec& x, const Vec& y, int deg, const Vec& w0) {
    if (x.empty()) throw Error("polyfit: no points");
    const Vec w = w0.empty() ? Vec(x.size(), 1.0) : w0;
    double x0 = 0.0;
    for (double v : x) x0 += v;
    x0 /= double(x.size());
    const auto [mn, mx] = std::minmax_element(x.begin(), x.end());
    double sc = (*mx - *mn) / 2;
    if (sc == 0.0) sc = 1.0;
    Vec t(x.size());
    for (size_t i = 0; i < x.size(); ++i) t[i] = (x[i] - x0) / sc;
    std::vector<Vec> a(size_t(deg + 1), Vec(size_t(deg + 1), 0.0));
    Vec b(size_t(deg + 1), 0.0);
    for (int i = 0; i <= deg; ++i) {
        for (int j = 0; j <= deg; ++j) {
            double s = 0.0;
            for (size_t k = 0; k < t.size(); ++k) s += w[k] * std::pow(t[k], i + j);
            a[size_t(i)][size_t(j)] = s;
        }
        double s = 0.0;
        for (size_t k = 0; k < t.size(); ++k) s += w[k] * y[k] * std::pow(t[k], i);
        b[size_t(i)] = s;
    }
    Poly p;
    p.coeff = solve(a, b);
    p.x0 = x0;
    p.scale = sc;
    return p;
}

// ----------------------------------------------------------------------------- peaks
std::vector<PixelPeak> findPeaks(const Vec& y, double minRel, int smoothW) {
    const Vec ys = smooth(y, smoothW);
    const Vec base = baseline(ys);
    Vec z(ys.size());
    for (size_t i = 0; i < ys.size(); ++i) z[i] = ys[i] - base[i];
    if (z.empty()) return {};
    double top = *std::max_element(z.begin(), z.end());
    if (top == 0.0) top = 1.0;
    std::vector<PixelPeak> peaks;
    for (size_t i = 2; i + 2 < z.size(); ++i) {
        if (z[i] >= z[i - 1] && z[i] > z[i + 1] && z[i] >= z[i - 2] && z[i] > z[i + 2] && z[i] > minRel * top) {
            const double a = z[i - 1], b = z[i], c = z[i + 1];
            const double den = a - 2 * b + c;
            const double off = den != 0.0 ? 0.5 * (a - c) / den : 0.0;
            peaks.push_back({double(i) + std::max(-0.5, std::min(0.5, off)), b});
        }
    }
    return peaks;
}

// ----------------------------------------------------------------------------- matching and fit
std::vector<Match> match(const std::vector<PixelPeak>& peaks, const ShiftFn& shiftOf, const QString& standard,
                         double window) {
    std::vector<Match> out;
    std::vector<bool> used(peaks.size(), false);
    for (const standards::Band& band : standards::bands(standard)) {
        bool found = false;
        double bd = 0, bh = 0;
        size_t bk = 0;
        for (size_t k = 0; k < peaks.size(); ++k) {
            if (used[k]) continue;
            const double d = std::abs(shiftOf(peaks[k].pixel) - band.position), h = peaks[k].height;
            // (d, -h) < (best_d, -best_h): closest, then tallest
            if (d <= window && (!found || std::make_tuple(d, -h) < std::make_tuple(bd, -bh))) {
                found = true;
                bd = d;
                bk = k;
                bh = h;
            }
        }
        if (found) {
            used[bk] = true;
            out.push_back({peaks[bk].pixel, band.position, band.strength, bh});
        }
    }
    return out;
}

// ----------------------------------------------------------------------------- correction model
QString degreeName(int degree) {
    switch (degree) {
    case 0: return "offset";
    case 1: return "linear";
    case 2: return "quadratic";
    case 3: return "cubic";
    default: return QString("degree %1").arg(degree);
    }
}

int autoDegree(const Vec& pixels, int npix) {
    const size_t n = pixels.size();
    Vec px = pixels;
    std::sort(px.begin(), px.end());
    const double span = n ? px.back() - px.front() : 0.0;
    if (n < 2 || span < 0.25 * npix) return 0;
    double gap = 0.0;
    for (size_t i = 1; i < n; ++i) gap = std::max(gap, px[i] - px[i - 1]);
    if (n >= 6 && span >= 0.6 * npix && gap <= 0.3 * span) return 3;
    return n >= 4 ? 2 : 1;
}

ShiftFn correction(const std::vector<Match>& matches, int deg, const ShiftFn& base) {
    Vec px, d, w;
    for (const Match& m : matches) {
        px.push_back(m.pixel);
        d.push_back(m.reference - base(m.pixel));
        w.push_back(double(m.strength));
    }
    const double lo = *std::min_element(px.begin(), px.end()), hi = *std::max_element(px.begin(), px.end());
    const Poly c = polyfit(px, d, deg, w);
    double slope = 0.0;                          // linear continuation outside [lo, hi]; degree 0: constant
    if (deg >= 1) {
        const Poly lin = polyfit(px, d, 1, w);
        slope = lin.coeff[1] / lin.scale;
    }
    const double cLo = c(lo), cHi = c(hi);
    return [c, lo, hi, cLo, cHi, slope](double p) {
        if (p < lo) return cLo + slope * (p - lo);
        if (p > hi) return cHi + slope * (p - hi);
        return c(p);
    };
}

// ----------------------------------------------------------------------------- fit
FitOutput fit(const std::vector<Match>& matches, std::optional<int> deg, const ShiftFn& base, int npix) {
    FitOutput r;
    if (matches.empty()) throw Error("no reference band matched");
    Vec px;
    for (const Match& m : matches) px.push_back(m.pixel);
    Vec uniq = px;
    std::sort(uniq.begin(), uniq.end());
    const int distinct = int(std::unique(uniq.begin(), uniq.end()) - uniq.begin());
    if (!base) {
        if (matches.size() == 1) throw Error("one-point calibration needs the current calibration");
        int d = deg ? *deg : std::max(1, autoDegree(px, npix));
        d = std::min(d, distinct - 1);
        Vec ref, w;
        for (const Match& m : matches) {
            ref.push_back(m.reference);
            w.push_back(double(m.strength));
        }
        r.poly = polyfit(px, ref, d, w);
        r.degree = d;
    } else {
        int d = deg ? *deg : autoDegree(px, npix);
        d = std::max(0, std::min(d, distinct - 1));
        const ShiftFn delta = correction(matches, d, base);
        Vec grid, val;
        for (int g = 0; g < npix; g += kStoreStep) grid.push_back(g);
        if (grid.back() != npix - 1) grid.push_back(npix - 1);
        for (double g : grid) val.push_back(base(g) + delta(g));
        r.poly = polyfit(grid, val, kStoreDegree);
        r.degree = d;
    }
    double ss = 0.0;
    for (const Match& m : matches) {
        r.residuals.push_back({m.reference, r.poly(m.pixel) - m.reference, m.pixel});
        ss += r.residuals.back().residual * r.residuals.back().residual;
    }
    r.rms = std::sqrt(ss / double(r.residuals.size()));
    return r;
}

Result calibrate(const std::vector<std::pair<Vec, QString>>& samples, const ShiftFn& shiftOf, std::optional<int> deg,
                 double window, const ShiftFn& base) {
    Result res;
    std::vector<Match> all;
    for (const auto& [y, name] : samples) {
        const std::vector<Match> m = match(findPeaks(y), shiftOf, name, window);
        res.matches.emplace_back(name, m);
        all.insert(all.end(), m.begin(), m.end());
    }
    const FitOutput f = fit(all, deg, base ? base : shiftOf, samples.empty() ? 2048 : int(samples.front().first.size()));
    res.poly = f.poly;
    res.residuals = f.residuals;
    res.rms = f.rms;
    for (const auto& s : samples) res.standards.push_back(s.second);
    res.date = QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm");
    res.degree = f.degree;
    return res;
}

UserCalibration Result::userCalibration() const {
    UserCalibration u;
    u.poly = poly;
    u.date = date;
    u.standards = standards;
    u.rms = rms;
    return u;
}

QString report(const Result& result, const ShiftFn& before) {
    QStringList names;
    for (const QString& s : result.standards) names << s;
    QStringList lines;
    lines << QString("Calibration with %1: %2 bands, %3 correction to the factory calibration, RMS %4 cm-1")
                 .arg(names.join(", ")).arg(result.residuals.size()).arg(degreeName(result.degree)).arg(result.rms, 0, 'f', 2);
    if (!result.residuals.empty()) {
        double rlo = result.residuals[0].reference, rhi = rlo, plo = result.residuals[0].pixel, phi = plo;
        for (const Residual& r : result.residuals) {
            rlo = std::min(rlo, r.reference);
            rhi = std::max(rhi, r.reference);
            plo = std::min(plo, r.pixel);
            phi = std::max(phi, r.pixel);
        }
        lines << QString::asprintf("  bands cover %.0f-%.0f cm-1 (pixels %.0f-%.0f); ", rlo, rhi, plo, phi) +
                     (result.degree == 0 ? "the offset applies to the whole detector"
                                         : "outside this range the correction is extrapolated linearly");
    }
    for (const auto& [name, m] : result.matches)
        lines << QString("  %1: %2 of %3 reference bands matched").arg(name).arg(m.size()).arg(standards::bands(name).size());
    lines << QString("  reference   residual   pixel") + (before ? "   before" : "");
    std::vector<Residual> res = result.residuals;
    std::sort(res.begin(), res.end(), [](const Residual& a, const Residual& b) {
        return std::tie(a.reference, a.residual, a.pixel) < std::tie(b.reference, b.residual, b.pixel);
    });
    for (const Residual& r : res) {
        const QString extra = before ? QString::asprintf("   %+6.2f", before(r.pixel) - r.reference) : QString();
        lines << QString::asprintf("  %9.1f   %+7.2f   %7.1f", r.reference, r.residual, r.pixel) + extra;
    }
    return lines.join("\n");
}

// ----------------------------------------------------------------------------- storage
QString save(const Result& result, int channel, const QString& serial, const QString& path0) {
    const QString path = path0.isEmpty()
        ? QDir(configDir()).filePath(QString("calibration_%1_%2.json").arg(serial).arg(channel)) : path0;
    QDir().mkpath(QFileInfo(path).absolutePath());
    QJsonArray coeff;
    for (double c : result.poly.coeff) coeff.append(c);
    QJsonArray residuals;
    for (const Residual& r : result.residuals) residuals.append(QJsonArray{r.reference, r.residual, r.pixel});
    QJsonArray matches;
    for (const auto& [name, ms] : result.matches) {
        QJsonArray list;
        for (const Match& m : ms) list.append(QJsonArray{m.pixel, m.reference, m.strength, m.height});
        matches.append(QJsonArray{name, list});
    }
    QJsonArray standards;
    for (const QString& s : result.standards) standards.append(s);
    QJsonObject o;
    o["poly"] = QJsonObject{{"coeff", coeff}, {"x0", result.poly.x0}, {"scale", result.poly.scale}};
    o["residuals"] = residuals;
    o["rms"] = result.rms;
    o["matches"] = matches;
    o["standards"] = standards;
    o["date"] = result.date;
    o["degree"] = result.degree;
    o["channel"] = channel;
    o["serial"] = serial;
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) throw Error(("cannot write " + path).toStdString());
    f.write(QJsonDocument(o).toJson(QJsonDocument::Indented));
    return path;
}

}  // namespace lr::calib
