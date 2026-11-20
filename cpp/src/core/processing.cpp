#include "processing.h"

#include <Eigen/Dense>
#include <algorithm>
#include <cmath>
#include <numeric>

#include "errors.h"

namespace lr::sig {

namespace {

double factorial(int n) {
    double f = 1.0;
    for (int k = 2; k <= n; ++k) f *= k;
    return f;
}

// Value (or deriv-th derivative) at t of the polynomial basis t^m, m = 0..order.
Eigen::RowVectorXd basisRow(double t, int order, int deriv) {
    Eigen::RowVectorXd r = Eigen::RowVectorXd::Zero(order + 1);
    for (int m = deriv; m <= order; ++m) {
        double c = 1.0;
        for (int k = 0; k < deriv; ++k) c *= m - k;
        r(m) = c * std::pow(t, m - deriv);
    }
    return r;
}

// Pseudo-inverse of the Vandermonde matrix of t (least-squares polynomial fit operator).
Eigen::MatrixXd vanderPinv(const Eigen::VectorXd& t, int order) {
    Eigen::MatrixXd V(t.size(), order + 1);
    for (Eigen::Index i = 0; i < t.size(); ++i) V.row(i) = basisRow(t(i), order, 0);
    return V.completeOrthogonalDecomposition().pseudoInverse();
}

}  // namespace

Vec savgolFilter(const Vec& y, int window, int order, int deriv) {
    if (order >= window) throw Error("polyorder must be less than window_length.");
    if (window % 2 == 0) throw Error("savgolFilter: window_length must be odd");   // all callers use odd windows
    const int n = int(y.size());
    if (window > n) throw Error("If mode is 'interp', window_length must be less than or equal to the size of x.");
    Vec out(size_t(n), 0.0);
    if (deriv > order) return out;
    const int h = window / 2;                       // scipy: pos = window // 2 (centre of the window)
    // interior: the fitted polynomial's deriv-th derivative at the window centre
    Eigen::VectorXd tc(window);
    for (int j = 0; j < window; ++j) tc(j) = j - h;
    const Eigen::MatrixXd pc = vanderPinv(tc, order);
    const Eigen::RowVectorXd coeff = factorial(deriv) * pc.row(deriv);
    for (int i = 0; i < n; ++i) {
        double s = 0.0;
        for (int j = 0; j < window; ++j) {
            const int k = i + j - h;
            if (k >= 0 && k < n) s += coeff(j) * y[size_t(k)];
        }
        out[size_t(i)] = s;
    }
    // edges (mode "interp"): polynomial fitted to the first / last window points, evaluated there
    Eigen::VectorXd te(window);
    for (int j = 0; j < window; ++j) te(j) = j;
    const Eigen::MatrixXd pe = vanderPinv(te, order);
    Eigen::VectorXd yl(window), yr(window);
    for (int j = 0; j < window; ++j) {
        yl(j) = y[size_t(j)];
        yr(j) = y[size_t(n - window + j)];
    }
    const Eigen::VectorXd cl = pe * yl, cr = pe * yr;
    const int halflen = window / 2;
    for (int i = 0; i < halflen; ++i) {
        out[size_t(i)] = basisRow(i, order, deriv).dot(cl);
        out[size_t(n - halflen + i)] = basisRow(window - halflen + i, order, deriv).dot(cr);
    }
    return out;
}

namespace {

// scipy.signal._peak_finding_utils._local_maxima_1d (midpoints of flat peaks)
std::vector<int> localMaxima(const Vec& x) {
    std::vector<int> mids;
    const int imax = int(x.size()) - 1;
    int i = 1;
    while (i < imax) {
        if (x[size_t(i - 1)] < x[size_t(i)]) {
            int ahead = i + 1;
            while (ahead < imax && x[size_t(ahead)] == x[size_t(i)]) ++ahead;
            if (x[size_t(ahead)] < x[size_t(i)]) {
                mids.push_back((i + ahead - 1) / 2);
                i = ahead;
            }
        }
        ++i;
    }
    return mids;
}

// _peak_prominences with wlen=None
void prominences(const Vec& x, const std::vector<int>& peaks, Vec& prom, std::vector<int>& lb, std::vector<int>& rb) {
    const size_t m = peaks.size();
    prom.assign(m, 0.0);
    lb.assign(m, 0);
    rb.assign(m, 0);
    const int imax = int(x.size()) - 1;
    for (size_t k = 0; k < m; ++k) {
        const int peak = peaks[k];
        const double xp = x[size_t(peak)];
        int i = lb[k] = peak;
        double leftMin = xp;
        while (0 <= i && x[size_t(i)] <= xp) {
            if (x[size_t(i)] < leftMin) { leftMin = x[size_t(i)]; lb[k] = i; }
            --i;
        }
        i = rb[k] = peak;
        double rightMin = xp;
        while (i <= imax && x[size_t(i)] <= xp) {
            if (x[size_t(i)] < rightMin) { rightMin = x[size_t(i)]; rb[k] = i; }
            ++i;
        }
        prom[k] = xp - std::max(leftMin, rightMin);
    }
}

// _peak_widths
Vec widths(const Vec& x, const std::vector<int>& peaks, double relHeight, const Vec& prom,
           const std::vector<int>& lb, const std::vector<int>& rb) {
    Vec w(peaks.size(), 0.0);
    for (size_t p = 0; p < peaks.size(); ++p) {
        const int imin = lb[p], imax = rb[p], peak = peaks[p];
        const double height = x[size_t(peak)] - prom[p] * relHeight;
        int i = peak;
        while (imin < i && height < x[size_t(i)]) --i;
        double left = i;
        if (x[size_t(i)] < height) left += (height - x[size_t(i)]) / (x[size_t(i + 1)] - x[size_t(i)]);
        i = peak;
        while (i < imax && height < x[size_t(i)]) ++i;
        double right = i;
        if (x[size_t(i)] < height) right -= (height - x[size_t(i)]) / (x[size_t(i - 1)] - x[size_t(i)]);
        w[p] = right - left;
    }
    return w;
}

template <class T>
std::vector<T> pick(const std::vector<T>& v, const std::vector<bool>& keep) {
    std::vector<T> out;
    for (size_t i = 0; i < v.size(); ++i)
        if (keep[i]) out.push_back(v[i]);
    return out;
}

}  // namespace

PeakSet findPeaks(const Vec& y, double minProminence, double minWidth, double distance) {
    PeakSet r;
    r.peaks = localMaxima(y);
    if (distance > 0 && !r.peaks.empty()) {          // _select_by_peak_distance: keep the highest first
        const int dist = int(std::ceil(distance));
        const size_t m = r.peaks.size();
        std::vector<bool> keep(m, true);
        std::vector<size_t> order(m);
        std::iota(order.begin(), order.end(), 0);
        std::stable_sort(order.begin(), order.end(),
                         [&](size_t a, size_t b) { return y[size_t(r.peaks[a])] < y[size_t(r.peaks[b])]; });
        for (size_t ii = m; ii-- > 0;) {
            const size_t j = order[ii];
            if (!keep[j]) continue;
            for (size_t k = j; k-- > 0 && r.peaks[j] - r.peaks[k] < dist;) keep[k] = false;
            for (size_t k = j + 1; k < m && r.peaks[k] - r.peaks[j] < dist; ++k) keep[k] = false;
        }
        r.peaks = pick(r.peaks, keep);
    }
    prominences(y, r.peaks, r.prominences, r.leftBases, r.rightBases);
    {
        std::vector<bool> keep(r.peaks.size());
        for (size_t i = 0; i < keep.size(); ++i) keep[i] = minProminence <= r.prominences[i];
        r.peaks = pick(r.peaks, keep);
        r.prominences = pick(r.prominences, keep);
        r.leftBases = pick(r.leftBases, keep);
        r.rightBases = pick(r.rightBases, keep);
    }
    if (minWidth > 0) {
        r.widths = widths(y, r.peaks, 0.5, r.prominences, r.leftBases, r.rightBases);
        std::vector<bool> keep(r.peaks.size());
        for (size_t i = 0; i < keep.size(); ++i) keep[i] = minWidth <= r.widths[i];
        r.peaks = pick(r.peaks, keep);
        r.prominences = pick(r.prominences, keep);
        r.leftBases = pick(r.leftBases, keep);
        r.rightBases = pick(r.rightBases, keep);
        r.widths = pick(r.widths, keep);
    }
    return r;
}

Vec peakWidths(const Vec& y, const std::vector<int>& peaks, double relHeight) {
    Vec prom;
    std::vector<int> lb, rb;
    prominences(y, peaks, prom, lb, rb);
    return widths(y, peaks, relHeight, prom, lb, rb);
}

}  // namespace lr::sig

namespace lr::processing {

namespace {

// Solve (W + lam D'D) z = w*y, D = second difference: symmetric pentadiagonal, banded LDL^T.
Vec solvePenalized(const Vec& w, const Vec& y, double lam) {
    const size_t n = y.size();
    Vec d(n, 0.0), e(n, 0.0), f(n, 0.0);           // A(i,i), A(i,i+1), A(i,i+2)
    for (size_t k = 0; k + 2 < n; ++k) {             // add lam * [1 -2 1]' [1 -2 1] at rows/cols k..k+2
        const double c[3] = {1.0, -2.0, 1.0};
        for (int a = 0; a < 3; ++a) {
            d[k + size_t(a)] += lam * c[a] * c[a];
            if (a < 2) e[k + size_t(a)] += lam * c[a] * c[a + 1];
        }
        f[k] += lam * c[0] * c[2];
    }
    for (size_t i = 0; i < n; ++i) d[i] += w[i];
    Vec D(n), l1(n, 0.0), l2(n, 0.0);
    for (size_t i = 0; i < n; ++i) {
        if (i >= 2) l2[i] = f[i - 2] / D[i - 2];
        if (i >= 1) l1[i] = (e[i - 1] - (i >= 2 ? l2[i] * D[i - 2] * l1[i - 1] : 0.0)) / D[i - 1];
        D[i] = d[i] - (i >= 1 ? l1[i] * l1[i] * D[i - 1] : 0.0) - (i >= 2 ? l2[i] * l2[i] * D[i - 2] : 0.0);
    }
    Vec z(n);
    for (size_t i = 0; i < n; ++i) {
        double s = w[i] * y[i];
        if (i >= 1) s -= l1[i] * z[i - 1];
        if (i >= 2) s -= l2[i] * z[i - 2];
        z[i] = s;
    }
    for (size_t i = 0; i < n; ++i) z[i] /= D[i];
    for (size_t i = n; i-- > 0;) {
        if (i + 1 < n) z[i] -= l1[i + 1] * z[i + 1];
        if (i + 2 < n) z[i] -= l2[i + 2] * z[i + 2];
    }
    return z;
}

double median(Vec v) {                              // numpy.median (mean of the two middle values)
    if (v.empty()) return std::nan("");
    const size_t m = v.size() / 2;
    std::nth_element(v.begin(), v.begin() + long(m), v.end());
    const double hi = v[m];
    if (v.size() % 2) return hi;
    return (*std::max_element(v.begin(), v.begin() + long(m)) + hi) / 2.0;
}

// scipy.ndimage boundary handling: "reflect" (d c b a | a b c d | d c b a), repeated for long windows
size_t reflectIndex(long i, long n) {
    while (i < 0 || i >= n) {
        if (i < 0) i = -i - 1;
        if (i >= n) i = 2 * n - i - 1;
    }
    return size_t(i);
}

// Sliding window over [i + off, i + off + size) with reflect boundaries; op = 0 min, 1 max, 2 mean.
Vec windowFilter(const Vec& y, int size, long off, int op) {
    const long n = long(y.size());
    Vec out(y.size());
    for (long i = 0; i < n; ++i) {
        double acc = op == 0 ? INFINITY : op == 1 ? -INFINITY : 0.0;
        for (long k = 0; k < size; ++k) {
            const double v = y[reflectIndex(i + off + k, n)];
            if (op == 0) acc = std::min(acc, v);
            else if (op == 1) acc = std::max(acc, v);
            else acc += v;
        }
        out[size_t(i)] = op == 2 ? acc / size : acc;
    }
    return out;
}

// scipy.ndimage.median_filter(y, size, mode="nearest") for odd sizes
Vec medianFilterNearest(const Vec& y, int size) {
    const long n = long(y.size()), h = size / 2;
    Vec out(y.size()), w(static_cast<size_t>(size));
    for (long i = 0; i < n; ++i) {
        for (long k = 0; k < size; ++k) w[size_t(k)] = y[size_t(std::clamp(i - h + k, 0L, n - 1))];
        std::nth_element(w.begin(), w.begin() + h, w.end());
        out[size_t(i)] = w[size_t(h)];
    }
    return out;
}

double polyvalHorner(const Eigen::VectorXd& c, double t) {
    double s = 0.0;
    for (Eigen::Index k = c.size(); k-- > 0;) s = s * t + c(k);
    return s;
}

}  // namespace

const std::vector<std::pair<QString, QString>>& baselines() {
    static const std::vector<std::pair<QString, QString>> b = {
        {"none", "No baseline correction"},
        {"als", "Asymmetric least squares (Eilers)"},
        {"arpls", "Asymmetrically reweighted penalized least squares"},
        {"polynomial", "Modified polynomial (iterative)"},
        {"rolling_ball", "Rolling ball"},
        {"snip", "SNIP (statistics-sensitive non-linear iterative peak clipping)"},
    };
    return b;
}

// baseline_als
Vec baselineAls(const Vec& y, double lam, double p, int niter) {
    const size_t n = y.size();
    Vec w(n, 1.0), z = y;
    for (int it = 0; it < niter; ++it) {
        z = solvePenalized(w, y, lam);
        for (size_t i = 0; i < n; ++i) w[i] = y[i] > z[i] ? p : 1 - p;
    }
    return z;
}

// baseline_arpls
Vec baselineArpls(const Vec& y, double lam, double ratio, int niter) {
    const size_t n = y.size();
    Vec w(n, 1.0), z = y, wt(n);
    for (int it = 0; it < niter; ++it) {
        z = solvePenalized(w, y, lam);
        Vec dn;
        for (size_t i = 0; i < n; ++i)
            if (y[i] - z[i] < 0) dn.push_back(y[i] - z[i]);
        if (dn.size() < 2) break;
        double m = 0.0;
        for (double v : dn) m += v;
        m /= double(dn.size());
        double s = 0.0;
        for (double v : dn) s += (v - m) * (v - m);
        s = std::sqrt(s / double(dn.size()));
        if (s == 0.0) s = 1.0;
        double nd = 0.0, nw = 0.0;
        for (size_t i = 0; i < n; ++i) {
            const double d = y[i] - z[i];
            wt[i] = 1.0 / (1.0 + std::exp(std::clamp(2.0 * (d - (2 * s - m)) / s, -50.0, 50.0)));
            nd += (w[i] - wt[i]) * (w[i] - wt[i]);
            nw += w[i] * w[i];
        }
        nw = std::sqrt(nw);
        if (std::sqrt(nd) / (nw != 0.0 ? nw : 1.0) < ratio) break;
        w = wt;
    }
    return z;
}

// baseline_polynomial: fit, clip the data to the fit, repeat
Vec baselinePolynomial(const Vec& x, const Vec& y, int order, int niter) {
    const Eigen::Index n = Eigen::Index(y.size());
    if (n == 0) return {};
    double mean = 0.0;
    for (double v : x) mean += v;
    mean /= double(n);
    const auto [mn, mx] = std::minmax_element(x.begin(), x.end());
    double half = (*mx - *mn) / 2;
    if (half == 0.0) half = 1.0;
    Eigen::VectorXd t(n);
    for (Eigen::Index i = 0; i < n; ++i) t(i) = (x[size_t(i)] - mean) / half;
    // numpy.polynomial.polynomial.polyfit: least squares on the column-scaled Vandermonde matrix
    Eigen::MatrixXd V(n, order + 1);
    for (Eigen::Index i = 0; i < n; ++i) {
        double p = 1.0;
        for (int k = 0; k <= order; ++k) { V(i, k) = p; p *= t(i); }
    }
    const Eigen::VectorXd scl = V.colwise().norm().transpose().unaryExpr([](double v) { return v == 0 ? 1.0 : v; });
    const Eigen::MatrixXd Vs = V * scl.cwiseInverse().asDiagonal();
    Eigen::CompleteOrthogonalDecomposition<Eigen::MatrixXd> cod(Vs);
    Eigen::VectorXd yy = Eigen::Map<const Eigen::VectorXd>(y.data(), n);
    Vec z(size_t(n), 0.0);
    for (int it = 0; it < niter; ++it) {
        const Eigen::VectorXd c = cod.solve(yy).cwiseQuotient(scl);
        bool close = true;
        Eigen::VectorXd nw(n);
        for (Eigen::Index i = 0; i < n; ++i) {
            z[size_t(i)] = polyvalHorner(c, t(i));
            nw(i) = std::min(yy(i), z[size_t(i)]);
            if (!(std::abs(nw(i) - yy(i)) <= 1e-8 + 1e-5 * std::abs(yy(i)))) close = false;   // np.allclose
        }
        if (close) break;
        yy = nw;
    }
    return z;
}

// baseline_rolling_ball: grey opening (erosion then dilation), then a moving average
Vec baselineRollingBall(const Vec& y, double radius) {
    if (y.empty()) return {};
    const int size = std::max(3, int(2 * radius + 1));
    const Vec eroded = windowFilter(y, size, -(size / 2), 0);
    // grey_dilation mirrors the origin; for an even size the window moves one point to the right
    const Vec opened = windowFilter(eroded, size, -(size / 2) + (size % 2 ? 0 : 1), 1);
    const int u = std::max(3, size / 2);
    return windowFilter(opened, u, -(u / 2), 2);
}

// baseline_snip
Vec baselineSnip(const Vec& y, int iterations) {
    const size_t n = y.size();
    if (n == 0) return {};
    const double ymin = *std::min_element(y.begin(), y.end());
    Vec v(n);
    for (size_t i = 0; i < n; ++i) v[i] = std::log(std::log(std::sqrt(std::max(y[i] - ymin, 0.0) + 1) + 1) + 1);
    Vec nv;
    for (int k = 1; k <= iterations; ++k) {
        if (n < size_t(2 * k) + 1) continue;
        nv.assign(v.begin(), v.end());
        for (size_t i = size_t(k); i < n - size_t(k); ++i) nv[i] = std::min(v[i], (v[i - size_t(k)] + v[i + size_t(k)]) / 2);
        v.swap(nv);
    }
    Vec z(n);
    for (size_t i = 0; i < n; ++i) {
        const double a = std::exp(std::exp(v[i]) - 1) - 1;
        z[i] = a * a - 1 + ymin;
    }
    return z;
}

Vec baseline(const Vec& x, const Vec& y, const QString& method, double strength) {
    if (method.isEmpty() || method == "none") return Vec(y.size(), 0.0);
    const double s = std::clamp(strength, 0.0, 10.0);
    if (method == "als") return baselineAls(y, std::pow(10.0, 2 + s * 0.6));
    if (method == "arpls") return baselineArpls(y, std::pow(10.0, 2 + s * 0.6));
    if (method == "polynomial") return baselinePolynomial(x, y, int(std::nearbyint(1 + s)));
    if (method == "rolling_ball") return baselineRollingBall(y, 10 + s * 30);
    if (method == "snip") return baselineSnip(y, int(10 + s * 15));
    throw Error(("unknown baseline method '" + method + "'").toStdString());
}

// despike: a point is a spike if it rises far above a 5-point median (more than threshold x noise) AND
// that excess is most of its height above the local background (41-point median).
Vec despike(const Vec& y, double threshold) {
    const size_t n = y.size();
    if (n == 0) return {};
    const Vec med = medianFilterNearest(y, 5);
    Vec res(n);
    for (size_t i = 0; i < n; ++i) res[i] = y[i] - med[i];
    const double mr = median(res);
    Vec dev(n);
    for (size_t i = 0; i < n; ++i) dev[i] = std::abs(res[i] - mr);
    double noise = 1.4826 * median(dev);
    if (noise == 0.0) noise = 1.0;
    const Vec base = medianFilterNearest(y, 41);
    std::vector<bool> spike(n);
    for (size_t i = 0; i < n; ++i) spike[i] = res[i] > threshold * noise && res[i] > 0.5 * (y[i] - base[i]);
    Vec out = y;
    for (size_t i = 0; i < n; ++i) {
        const bool prev = spike[(i + n - 1) % n], next = spike[(i + 1) % n];       // np.roll wraps around
        if (spike[i] || ((prev || next) && res[i] > 3 * noise)) out[i] = med[i];
    }
    return out;
}

// despike_frames: per pixel, values far above the median of the scans are replaced by the median
Vec despikeFrames(const std::vector<Vec>& frames, double k) {
    if (frames.empty()) return {};
    const size_t m = frames.size(), n = frames[0].size();
    Vec out(size_t(n), 0.0);
    if (m < 3) {
        for (const Vec& f : frames)
            for (size_t i = 0; i < n; ++i) out[i] += f[i];
        for (double& v : out) v /= double(m);
        return out;
    }
    Vec col(m), dev(m);
    for (size_t i = 0; i < n; ++i) {
        for (size_t j = 0; j < m; ++j) col[j] = frames[j][i];
        const double med = median(col);
        for (size_t j = 0; j < m; ++j) dev[j] = std::abs(col[j] - med);
        const double mad = median(dev) + std::sqrt(std::max(med, 1.0));
        double s = 0.0;
        for (size_t j = 0; j < m; ++j) s += col[j] - med > k * mad ? med : col[j];
        out[i] = s / double(m);
    }
    return out;
}

Vec smooth(const Vec& y, int window, int order) {
    window |= 1;
    if (window < order + 2) return y;
    return sig::savgolFilter(y, window, order);
}

Vec normalize(const Vec& x, const Vec& y, const QString& mode, std::optional<double> band) {
    auto scaled = [&](double v) {
        if (v == 0.0) return y;
        Vec o(y.size());
        for (size_t i = 0; i < y.size(); ++i) o[i] = y[i] / v;
        return o;
    };
    if (y.empty()) return y;
    if (mode == "max") return scaled(*std::max_element(y.begin(), y.end()));
    if (mode == "area") {
        double a = 0.0;
        for (size_t i = 0; i + 1 < y.size(); ++i) a += (x[i + 1] - x[i]) * (std::abs(y[i + 1]) + std::abs(y[i])) / 2.0;
        return scaled(a);
    }
    if (mode == "band" && band) {
        size_t i = 0;
        for (size_t k = 1; k < x.size(); ++k)
            if (std::abs(x[k] - *band) < std::abs(x[i] - *band)) i = k;
        const size_t lo = i >= 3 ? i - 3 : 0, hi = std::min(y.size(), i + 4);
        return scaled(*std::max_element(y.begin() + long(lo), y.begin() + long(hi)));
    }
    return y;
}

Settings Settings::fromMap(const QVariantMap& m) {
    Settings s;
    if (m.contains("baseline")) s.baseline = m.value("baseline").isNull() ? QString("none") : m.value("baseline").toString();
    if (m.contains("strength")) s.strength = m.value("strength").toDouble();
    if (m.contains("despike")) s.despike = m.value("despike").toBool();
    if (m.contains("smooth")) s.smooth = m.value("smooth").toBool();
    if (m.contains("smooth_window")) s.smoothWindow = m.value("smooth_window").toInt();
    if (m.contains("derivative")) s.derivative = m.value("derivative").toInt();
    if (m.contains("deriv_window")) s.derivWindow = m.value("deriv_window").toInt();
    if (m.contains("normalize")) s.normalize = m.value("normalize").toString();
    if (m.contains("band") && !m.value("band").isNull()) s.band = m.value("band").toDouble();
    return s;
}

std::pair<Vec, Vec> process(const Vec& x, const Vec& y0, const Settings& st) {
    Vec y = y0;
    if (st.despike) y = despike(y);
    if (st.smooth) y = smooth(y, st.smoothWindow);
    Vec base = baseline(x, y, st.baseline, st.strength);
    Vec out(y.size());
    for (size_t i = 0; i < y.size(); ++i) out[i] = y[i] - base[i];
    if (st.derivative) out = sig::savgolFilter(out, st.derivWindow | 1, 3, st.derivative);
    out = normalize(x, out, st.normalize, st.band);
    return {out, base};
}

Vec findPeaks(const Vec& x, const Vec& y, double prominence, int maxPeaks) {
    if (y.empty()) return {};
    const double ymax = *std::max_element(y.begin(), y.end());
    if (ymax <= 0) return {};
    const sig::PeakSet ps = sig::findPeaks(y, prominence * ymax, 0.0, 8);
    std::vector<size_t> order(ps.peaks.size());
    std::iota(order.begin(), order.end(), 0);
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return ps.prominences[a] < ps.prominences[b]; });
    std::reverse(order.begin(), order.end());                 // np.argsort(prominences)[::-1]
    Vec out;
    for (size_t k = 0; k < order.size() && int(k) < maxPeaks; ++k) out.push_back(x[size_t(ps.peaks[order[k]])]);
    std::sort(out.begin(), out.end());
    return out;
}

}  // namespace lr::processing
