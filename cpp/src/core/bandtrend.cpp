#include "bandtrend.h"

#include <Eigen/Dense>
#include <algorithm>
#include <cmath>

namespace lr::bandtrend {

namespace {

bool finite(double v) { return std::isfinite(v); }

struct Region {
    size_t i0 = 0, i1 = 0;      // points [i0, i1)
    size_t size() const { return i1 - i0; }
};

// Points of x[0, n) (ascending) inside [lo, hi].
Region region(const Vec& x, size_t n, double lo, double hi) {
    Region r;
    const auto end = x.begin() + std::ptrdiff_t(std::min(n, x.size()));
    r.i0 = size_t(std::lower_bound(x.begin(), end, lo) - x.begin());
    r.i1 = size_t(std::upper_bound(x.begin(), end, hi) - x.begin());
    if (r.i1 < r.i0) r.i1 = r.i0;
    return r;
}

}  // namespace

// ============================================================================================ quantities
const QStringList& quantityNames() {
    static const QStringList n{QStringLiteral("Height"),         QStringLiteral("Area"),
                               QStringLiteral("Centroid"),       QStringLiteral("FWHM"),
                               QStringLiteral("Fitted area"),    QStringLiteral("Fitted height"),
                               QStringLiteral("Fitted position"), QStringLiteral("Fitted FWHM")};
    return n;
}

QString quantityKey(Quantity q) {
    static const char* k[] = {"height", "area", "centroid", "fwhm", "fit_area", "fit_height", "fit_position", "fit_fwhm"};
    return QString::fromLatin1(k[int(q)]);
}

bool needsFit(Quantity q) { return int(q) >= int(Quantity::FitArea); }

bool isPosition(Quantity q) { return q == Quantity::Centroid || q == Quantity::FitPosition; }

QString quantityUnit(Quantity q) {
    return isPosition(q) || q == Quantity::Fwhm || q == Quantity::FitFwhm ? QStringLiteral("cm⁻¹") : QString();
}

double Measure::value(Quantity q) const {
    switch (q) {
    case Quantity::Height: return height;
    case Quantity::Area: return area;
    case Quantity::Centroid: return centroid;
    case Quantity::Fwhm: return fwhm;
    case Quantity::FitArea: return fitArea;
    case Quantity::FitHeight: return fitHeight;
    case Quantity::FitPosition: return fitPosition;
    case Quantity::FitFwhm: return fitFwhm;
    }
    return kNaN;
}

double Measure::error(Quantity q) const {
    switch (q) {
    case Quantity::FitArea: return fitAreaErr;
    case Quantity::FitHeight: return fitHeightErr;
    case Quantity::FitPosition: return fitPositionErr;
    case Quantity::FitFwhm: return fitFwhmErr;
    default: return kNaN;
    }
}

// ============================================================================================ measuring
Measure measure(const Vec& x, const Vec& y, const Band& band, bool fit) {
    Measure m;
    const size_t np = std::min(x.size(), y.size());
    const Region r = region(x, np, std::min(band.lo, band.hi), std::max(band.lo, band.hi));
    const size_t n = r.size();
    if (n >= 2) {
        // local baseline through the edge anchors
        double ax = 0, ay = 0, bx = 0, by = 0;
        const size_t k = std::clamp<size_t>(n / 10, 1, 3);
        for (size_t j = 0; j < k; ++j) {
            ax += x[r.i0 + j];
            ay += y[r.i0 + j];
            bx += x[r.i1 - 1 - j];
            by += y[r.i1 - 1 - j];
        }
        ax /= double(k);
        ay /= double(k);
        bx /= double(k);
        by /= double(k);
        const double slope = band.localBaseline && bx != ax ? (by - ay) / (bx - ax) : 0.0;
        auto base = [&](double v) { return band.localBaseline ? ay + slope * (v - ax) : 0.0; };
        Vec s(n);
        bool ok = true;
        for (size_t j = 0; j < n; ++j) {
            s[j] = y[r.i0 + j] - base(x[r.i0 + j]);
            ok = ok && finite(s[j]);
        }
        if (ok) {
            // height and position of the maximum (parabolic refinement)
            const size_t im = size_t(std::max_element(s.begin(), s.end()) - s.begin());
            m.height = s[im];
            m.peak = x[r.i0 + im];
            if (im > 0 && im + 1 < n) {
                const double a = s[im - 1], b = s[im], c = s[im + 1];
                const double den = a - 2 * b + c;
                if (den < 0) {
                    const double off = std::clamp(0.5 * (a - c) / den, -0.5, 0.5);
                    const double h = 0.5 * (x[r.i0 + im + 1] - x[r.i0 + im - 1]);
                    m.peak = x[r.i0 + im] + off * h;
                    m.height = b - 0.25 * (a - c) * off;
                }
            }
            // area
            double area = 0;
            for (size_t j = 0; j + 1 < n; ++j) area += (x[r.i0 + j + 1] - x[r.i0 + j]) * (s[j] + s[j + 1]) / 2;
            m.area = area;
            // centroid of the contiguous part above half maximum around the maximum
            if (s[im] > 0) {
                size_t j0 = im, j1 = im;
                while (j0 > 0 && s[j0 - 1] >= s[im] / 2) --j0;
                while (j1 + 1 < n && s[j1 + 1] >= s[im] / 2) ++j1;
                double sw = 0, swx = 0;
                for (size_t j = j0; j <= j1; ++j) {    // weights fall to 0 at half maximum: no jumps
                    sw += s[j] - s[im] / 2;
                    swx += (s[j] - s[im] / 2) * x[r.i0 + j];
                }
                if (sw > 0) m.centroid = swx / sw;
            }
            // FWHM by linear interpolation of the half-maximum crossings
            const double half = std::max(m.height, s[im]) / 2;
            if (s[im] > 0) {
                double left = kNaN, right = kNaN;
                for (size_t j = im; j > 0; --j)
                    if (s[j - 1] < half) {
                        const double f = (half - s[j - 1]) / (s[j] - s[j - 1]);
                        left = x[r.i0 + j - 1] + f * (x[r.i0 + j] - x[r.i0 + j - 1]);
                        break;
                    }
                for (size_t j = im; j + 1 < n; ++j)
                    if (s[j + 1] < half) {
                        const double f = (s[j] - half) / (s[j] - s[j + 1]);
                        right = x[r.i0 + j] + f * (x[r.i0 + j + 1] - x[r.i0 + j]);
                        break;
                    }
                if (finite(left) && finite(right)) m.fwhm = right - left;
            }
        }
    }
    if (fit && n >= 5) {
        m.fitted = true;
        try {
            const Vec xs(x.begin(), x.begin() + std::ptrdiff_t(np)), ys(y.begin(), y.begin() + std::ptrdiff_t(np));
            const std::pair<double, double> reg{std::min(band.lo, band.hi), std::max(band.lo, band.hi)};
            const peaks::FitResult f =
                band.guesses.empty() ? peaks::fit(xs, ys, reg, std::nullopt, band.profile)
                                     : peaks::fit(xs, ys, reg, std::optional<std::vector<peaks::Peak>>(band.guesses), band.profile);
            if (!f.peaks.empty()) {
                size_t k = 0;
                if (!band.guesses.empty()) {
                    k = size_t(std::clamp(band.main, 0, int(f.peaks.size()) - 1));
                } else {                                   // picked per spectrum: the tallest
                    for (size_t j = 1; j < f.peaks.size(); ++j)
                        if (f.peaks[j].height > f.peaks[k].height) k = j;
                }
                const peaks::FitPeak& p = f.peaks[k];
                const double lo = std::min(band.lo, band.hi), hi = std::max(band.lo, band.hi);
                // a peak that left the band or is wider than it (a weak or vanished band): no result
                if (!(p.position >= lo && p.position <= hi && p.fwhm <= hi - lo && std::isfinite(p.area)))
                    return m;
                m.fitArea = p.area;
                m.fitHeight = p.height;
                m.fitHeightErr = p.heightErr;
                m.fitPosition = p.position;
                m.fitPositionErr = p.positionErr;
                m.fitFwhm = p.fwhm;
                m.fitFwhmErr = p.fwhmErr;
                m.fitR2 = f.r2;
                double rel = 0;
                if (p.height != 0) rel += std::pow(p.heightErr / p.height, 2);
                if (p.fwhm != 0) rel += std::pow(p.fwhmErr / p.fwhm, 2);
                m.fitAreaErr = std::abs(p.area) * std::sqrt(rel);
            }
        } catch (const std::exception&) {              // no peak, too few points: NaN
        }
    }
    return m;
}

QString defaultName(double position) { return QStringLiteral("%1 cm⁻¹").arg(std::lround(position)); }

void setGuesses(Band& band, const Vec& x, const Vec& y) {
    band.guesses.clear();
    band.main = 0;
    const double lo = std::min(band.lo, band.hi), hi = std::max(band.lo, band.hi);
    try {
        for (const peaks::Peak& p : peaks::pick(x, y, 4.0))
            if (lo <= p.position && p.position <= hi) band.guesses.push_back(p);
    } catch (const std::exception&) {
    }
    if (band.guesses.empty()) {                        // one peak at the maximum of the region
        const Region r = region(x, y.size(), lo, hi);
        if (r.size() == 0) return;
        size_t im = r.i0;
        for (size_t i = r.i0; i < r.i1; ++i)
            if (y[i] > y[im]) im = i;
        peaks::Peak p;
        p.position = x[im];
        p.height = y[im];
        p.fwhm = std::max((hi - lo) / 4, 1e-6);
        band.guesses.push_back(p);
        return;
    }
    for (size_t j = 1; j < band.guesses.size(); ++j)
        if (band.guesses[j].height > band.guesses[size_t(band.main)].height) band.main = int(j);
}

Band bandAround(const peaks::Peak& p, const Vec& x) {
    double step = 1.0;
    if (x.size() >= 2) step = std::abs(x.back() - x.front()) / double(x.size() - 1);
    const double half = std::max({2 * p.fwhm, 3 * step, 5.0});
    Band b;
    b.name = defaultName(p.position);
    b.lo = p.position - half;
    b.hi = p.position + half;
    return b;
}

std::vector<std::vector<Measure>> measureSeries(const Vec& x, const std::vector<Vec>& rows,
                                                const std::vector<Band>& bands, size_t from,
                                                const std::atomic<bool>* stop) {
    std::vector<std::vector<Measure>> out(bands.size());
    for (size_t i = from; i < rows.size(); ++i) {
        if (stop && *stop) break;
        for (size_t b = 0; b < bands.size(); ++b)
            out[b].push_back(measure(x, rows[i], bands[b], needsFit(bands[b].quantity)));
    }
    return out;
}

// ============================================================================================ trends
Series extract(const std::vector<Measure>& m, Quantity q) {
    Series s;
    s.value.reserve(m.size());
    s.error.reserve(m.size());
    for (const Measure& v : m) {
        s.value.push_back(v.value(q));
        s.error.push_back(v.error(q));
    }
    return s;
}

Series ratio(const Series& a, const Series& b) {
    Series r;
    const size_t n = std::min(a.value.size(), b.value.size());
    for (size_t i = 0; i < n; ++i) {
        const double va = a.value[i], vb = b.value[i];
        const double v = vb != 0 ? va / vb : kNaN;
        r.value.push_back(v);
        const double ea = i < a.error.size() ? a.error[i] : kNaN, eb = i < b.error.size() ? b.error[i] : kNaN;
        if (!finite(ea) && !finite(eb)) {
            r.error.push_back(kNaN);
            continue;
        }
        double rel = 0;
        if (finite(ea) && va != 0) rel += std::pow(ea / va, 2);
        if (finite(eb) && vb != 0) rel += std::pow(eb / vb, 2);
        r.error.push_back(std::abs(v) * std::sqrt(rel));
    }
    return r;
}

Series relativeToFirst(const Series& a) {
    Series r = a;
    double first = kNaN;
    for (double v : a.value)
        if (finite(v)) {
            first = v;
            break;
        }
    for (size_t i = 0; i < r.value.size(); ++i) {
        r.value[i] = first != 0 ? r.value[i] / first : kNaN;
        if (i < r.error.size()) r.error[i] = first != 0 ? std::abs(r.error[i] / first) : kNaN;
    }
    return r;
}

// ============================================================================================ kinetic fits
namespace {

struct Points {
    Vec t, y, w;                    // w: weights 1/σ² (all 1 without errors)
};

Points usable(const Vec& t, const Vec& y, const Vec& err) {
    Points p;
    bool weighted = !err.empty();
    const size_t n = std::min(t.size(), y.size());
    for (size_t i = 0; i < n && weighted; ++i)
        if (finite(t[i]) && finite(y[i]) && !(i < err.size() && finite(err[i]) && err[i] > 0)) weighted = false;
    for (size_t i = 0; i < n; ++i) {
        if (!finite(t[i]) || !finite(y[i])) continue;
        p.t.push_back(t[i]);
        p.y.push_back(y[i]);
        p.w.push_back(weighted ? 1.0 / (err[i] * err[i]) : 1.0);
    }
    // normalise the weights to a mean of 1 (keeps s² on the scale of the data)
    double mw = 0;
    for (double v : p.w) mw += v;
    if (!p.w.empty() && mw > 0)
        for (double& v : p.w) v *= double(p.w.size()) / mw;
    return p;
}

double rSquared(const Points& p, const KineticFit& f) {
    double mean = 0;
    for (double v : p.y) mean += v;
    mean /= double(p.y.size());
    double ssr = 0, sst = 0;
    for (size_t i = 0; i < p.t.size(); ++i) {
        ssr += std::pow(p.y[i] - f(p.t[i]), 2);
        sst += std::pow(p.y[i] - mean, 2);
    }
    return sst > 0 ? 1 - ssr / sst : (ssr == 0 ? 1.0 : 0.0);
}

// Weighted linear least squares for A, B of y = A + B·e (e fixed): returns the weighted SSR.
double linearAB(const Points& p, const Vec& e, double& A, double& B) {
    double sw = 0, se = 0, see = 0, sy = 0, sey = 0;
    for (size_t i = 0; i < p.t.size(); ++i) {
        const double w = p.w[i];
        sw += w;
        se += w * e[i];
        see += w * e[i] * e[i];
        sy += w * p.y[i];
        sey += w * e[i] * p.y[i];
    }
    const double det = sw * see - se * se;
    if (std::abs(det) <= 1e-300 * std::max(1.0, sw * see)) {
        A = sy / sw;
        B = 0;
    } else {
        B = (sw * sey - se * sy) / det;
        A = (sy - B * se) / sw;
    }
    double ssr = 0;
    for (size_t i = 0; i < p.t.size(); ++i) ssr += p.w[i] * std::pow(p.y[i] - A - B * e[i], 2);
    return ssr;
}

}  // namespace

double KineticFit::operator()(double t) const {
    switch (model) {
    case Kinetic::Exponential: return a + b * std::exp(-(t - t0) / tau);
    case Kinetic::Linear: return a + b * t;
    case Kinetic::None: break;
    }
    return kNaN;
}

QString KineticFit::summary(const QString& unit, double scale) const {
    if (!ok) return message.isEmpty() ? QStringLiteral("no fit") : message;
    auto fmt = [](double v, double e) {
        if (!finite(e) || e <= 0) return QString::number(v, 'g', 4);
        // as many decimals as the error's first significant digit needs
        const int dec = std::clamp(-int(std::floor(std::log10(e))), 0, 8);
        return QStringLiteral("%1 ± %2").arg(v, 0, 'f', dec).arg(e, 0, 'f', dec);
    };
    QString s;
    if (model == Kinetic::Exponential)
        s = QStringLiteral("τ = %1 %2").arg(fmt(tau / scale, tauErr / scale), unit);
    else
        s = QStringLiteral("slope = %1 /%2").arg(QString::number(b * scale, 'g', 4), unit);
    return s + QStringLiteral(", R² = %1").arg(r2, 0, 'f', 4);
}

KineticFit fitLinear(const Vec& t, const Vec& y, const Vec& err) {
    KineticFit f;
    f.model = Kinetic::Linear;
    const Points p = usable(t, y, err);
    f.points = int(p.t.size());
    if (p.t.size() < 2) {
        f.message = QStringLiteral("too few points for a line");
        return f;
    }
    double A = 0, B = 0;
    const double ssr = linearAB(p, p.t, A, B);                // y = A + B·t
    double sw = 0, st = 0, stt = 0;
    for (size_t i = 0; i < p.t.size(); ++i) {
        sw += p.w[i];
        st += p.w[i] * p.t[i];
        stt += p.w[i] * p.t[i] * p.t[i];
    }
    const double det = sw * stt - st * st;
    if (det <= 0) {
        f.message = QStringLiteral("all points at the same time");
        return f;
    }
    const double s2 = p.t.size() > 2 ? ssr / double(p.t.size() - 2) : 0.0;
    f.a = A;
    f.b = B;
    f.aErr = std::sqrt(s2 * stt / det);
    f.bErr = std::sqrt(s2 * sw / det);
    f.rate = B;
    f.rateErr = f.bErr;
    f.ok = true;
    f.r2 = rSquared(p, f);
    return f;
}

KineticFit fitExponential(const Vec& t, const Vec& y, const Vec& err) {
    KineticFit f;
    f.model = Kinetic::Exponential;
    const Points p = usable(t, y, err);
    const size_t n = p.t.size();
    f.points = int(n);
    if (n < 4) {
        f.message = QStringLiteral("too few points for an exponential");
        return f;
    }
    const double t0 = *std::min_element(p.t.begin(), p.t.end());
    const double span = *std::max_element(p.t.begin(), p.t.end()) - t0;
    if (span <= 0) {
        f.message = QStringLiteral("all points at the same time");
        return f;
    }
    Vec dt(n);
    for (size_t i = 0; i < n; ++i) dt[i] = p.t[i] - t0;
    // start: τ on a logarithmic grid (span/200 … 20·span), A and B linear
    double bestK = 1 / span, bestA = 0, bestB = 0, best = std::numeric_limits<double>::infinity();
    Vec e(n);
    for (int g = 0; g <= 120; ++g) {
        const double tau = span / 200 * std::pow(4000.0, g / 120.0);
        for (size_t i = 0; i < n; ++i) e[i] = std::exp(-dt[i] / tau);
        double A = 0, B = 0;
        const double ssr = linearAB(p, e, A, B);
        if (ssr < best) {
            best = ssr;
            bestK = 1 / tau;
            bestA = A;
            bestB = B;
        }
    }
    // Levenberg-Marquardt on (A, B, k), weighted residuals sqrt(w)·(model - y)
    Eigen::Vector3d q(bestA, bestB, bestK);
    auto eval = [&](const Eigen::Vector3d& v, Eigen::VectorXd& r, Eigen::MatrixXd* J) {
        r.resize(Eigen::Index(n));
        if (J) J->resize(Eigen::Index(n), 3);
        for (size_t i = 0; i < n; ++i) {
            const double ex = std::exp(-v(2) * dt[i]), sw = std::sqrt(p.w[i]);
            r(Eigen::Index(i)) = sw * (v(0) + v(1) * ex - p.y[i]);
            if (J) {
                (*J)(Eigen::Index(i), 0) = sw;
                (*J)(Eigen::Index(i), 1) = sw * ex;
                (*J)(Eigen::Index(i), 2) = -sw * v(1) * dt[i] * ex;
            }
        }
    };
    Eigen::VectorXd r, rn;
    Eigen::MatrixXd J;
    eval(q, r, &J);
    double cost = r.squaredNorm(), mu = 1e-3;
    for (int it = 0; it < 500; ++it) {
        const Eigen::Matrix3d JtJ = J.transpose() * J;
        const Eigen::Vector3d g = J.transpose() * r;
        Eigen::Matrix3d A = JtJ;
        for (int j = 0; j < 3; ++j) A(j, j) += mu * std::max(JtJ(j, j), 1e-300);
        const Eigen::Vector3d step = A.ldlt().solve(-g);
        if (!step.allFinite()) break;
        const Eigen::Vector3d qn = q + step;
        eval(qn, rn, nullptr);
        const double cn = rn.squaredNorm();
        if (std::isfinite(cn) && cn < cost) {
            const bool small = cost - cn <= 1e-14 * cost || (step.array().abs() <= 1e-12 * (q.array().abs() + 1e-300)).all();
            q = qn;
            cost = cn;
            eval(q, r, &J);
            mu = std::max(mu / 3, 1e-12);
            if (small) break;
        } else {
            mu *= 4;
            if (mu > 1e20) break;
        }
    }
    if (!(q(2) > 0) || !q.allFinite()) {
        f.message = QStringLiteral("no exponential decay or growth in this trend");
        return f;
    }
    const Eigen::Matrix3d JtJ = J.transpose() * J;
    const double s2 = cost / double(std::max<size_t>(1, n - 3));
    Eigen::Matrix3d cov = JtJ.completeOrthogonalDecomposition().pseudoInverse() * s2;
    f.t0 = t0;
    f.a = q(0);
    f.b = q(1);
    f.rate = q(2);
    f.tau = 1 / q(2);
    f.aErr = std::sqrt(std::max(cov(0, 0), 0.0));
    f.bErr = std::sqrt(std::max(cov(1, 1), 0.0));
    f.rateErr = std::sqrt(std::max(cov(2, 2), 0.0));
    f.tauErr = f.rateErr / (q(2) * q(2));
    f.ok = true;
    f.r2 = rSquared(p, f);
    if (f.r2 < 0) {                                // worse than a constant (weighted fit of a non-exponential)
        f.ok = false;
        f.message = QStringLiteral("not a single-exponential trend (R² < 0)");
    } else if (f.tau > 50 * span) {
        f.ok = false;
        f.message = QStringLiteral("τ much longer than the series: fit a line instead");
    }
    return f;
}

KineticFit fitKinetic(Kinetic model, const Vec& t, const Vec& y, const Vec& err) {
    switch (model) {
    case Kinetic::Exponential: return fitExponential(t, y, err);
    case Kinetic::Linear: return fitLinear(t, y, err);
    case Kinetic::None: break;
    }
    return {};
}

std::pair<QString, double> timeUnit(double span) {
    if (span >= 3 * 3600) return {QStringLiteral("h"), 3600.0};
    if (span >= 180) return {QStringLiteral("min"), 60.0};
    return {QStringLiteral("s"), 1.0};
}

}  // namespace lr::bandtrend
