#include "peaks.h"

#include <Eigen/Dense>
#include <algorithm>
#include <cmath>
#include <limits>

#include "errors.h"
#include "processing.h"

namespace lr::peaks {

namespace {

const double kLn2 = std::log(2.0);
const double kSqrtPi = std::sqrt(M_PI);

double median(Vec v) {                              // numpy.median
    if (v.empty()) return std::numeric_limits<double>::quiet_NaN();
    const size_t m = v.size() / 2;
    std::nth_element(v.begin(), v.begin() + long(m), v.end());
    const double hi = v[m];
    if (v.size() % 2) return hi;
    return (*std::max_element(v.begin(), v.begin() + long(m)) + hi) / 2.0;
}

double medianStep(const Vec& x) {
    Vec d;
    for (size_t i = 0; i + 1 < x.size(); ++i) d.push_back(x[i + 1] - x[i]);
    return std::abs(median(d));
}

double subpixel(const Vec& x, const Vec& y, size_t i) {      // _subpixel
    if (0 < i && i + 1 < y.size()) {
        const double a = y[i - 1], b = y[i], c = y[i + 1];
        const double den = a - 2 * b + c;
        double off = den != 0.0 ? 0.5 * (a - c) / den : 0.0;
        off = std::max(-0.5, std::min(0.5, off));
        return x[i] + off * (x[i + 1] - x[i - 1]) / 2;
    }
    return x[i];
}

// w'(z) = -2 z w(z) + 2i/sqrt(pi); for large |z| the asymptotic series avoids the cancellation.
std::complex<double> faddeevaDeriv(std::complex<double> z, std::complex<double> w) {
    if (std::abs(z) > 50.0) {
        const std::complex<double> z2 = z * z, iz2 = 1.0 / z2;
        std::complex<double> s = 0.0, t = iz2;
        double c = 1.0;                                       // (2k+1)!! / 2^k
        for (int k = 0; k < 6; ++k) {
            s += c * t;
            t *= iz2;
            c *= (2.0 * k + 3.0) / 2.0;
        }
        return -std::complex<double>(0.0, 1.0) / kSqrtPi * s;
    }
    return -2.0 * z * w + std::complex<double>(0.0, 2.0 / kSqrtPi);
}

// Profile value and its gradient with respect to the parameters (a, x0, w[, eta | wl]).
double profileGrad(int kind, double x, const double* p, double* g) {
    switch (kind) {
    case 0: {                                                 // lorentz
        const double a = p[0], w = p[2], q = (x - p[1]) / (w / 2), d = 1 + q * q;
        if (g) {
            g[0] = 1 / d;
            g[1] = 4 * a * q / (w * d * d);
            g[2] = 2 * a * q * q / (w * d * d);
        }
        return a / d;
    }
    case 1: {                                                 // gauss
        const double a = p[0], w = p[2], q = (x - p[1]) / w, e = std::exp(-4 * kLn2 * q * q), f = a * e;
        if (g) {
            g[0] = e;
            g[1] = 8 * kLn2 * q * f / w;
            g[2] = 8 * kLn2 * q * q * f / w;
        }
        return f;
    }
    case 2: {                                                 // pvoigt
        double gl[3], gg[3];
        const double eta = p[3];
        const double l = profileGrad(0, x, p, g ? gl : nullptr), gs = profileGrad(1, x, p, g ? gg : nullptr);
        if (g) {
            for (int k = 0; k < 3; ++k) g[k] = eta * gl[k] + (1 - eta) * gg[k];
            g[3] = l - gs;
        }
        return eta * l + (1 - eta) * gs;
    }
    default: {                                                // voigt: a * Re w(z) / Re w(z0)
        const double a = p[0], wg = std::max(p[2], 1e-9), wl = std::max(p[3], 1e-9);
        const double sg = wg / (2 * std::sqrt(2 * kLn2)), gl = wl / 2, s = sg * std::sqrt(2.0);
        const std::complex<double> z((x - p[1]) / s, gl / s), z0(0.0, gl / s);
        const std::complex<double> w = faddeeva(z), w0 = faddeeva(z0);
        const double R = w.real(), R0 = w0.real();
        if (g) {
            const std::complex<double> dw = faddeevaDeriv(z, w), dw0 = faddeevaDeriv(z0, w0);
            const std::complex<double> I(0.0, 1.0);
            g[0] = R / R0;
            g[1] = a / R0 * (dw * (-1.0 / s)).real();
            const double dsdwg = p[2] > 1e-9 ? std::sqrt(2.0) / (2 * std::sqrt(2 * kLn2)) : 0.0;
            const double dgldwl = p[3] > 1e-9 ? 0.5 : 0.0;
            const double dRs = (dw * (-z / s)).real(), dR0s = (dw0 * (-z0 / s)).real();
            const double dRg = (dw * (I / s)).real(), dR0g = (dw0 * (I / s)).real();
            g[2] = a * (dRs * R0 - R * dR0s) / (R0 * R0) * dsdwg;
            g[3] = a * (dRg * R0 - R * dR0g) / (R0 * R0) * dgldwl;
        }
        return a * R / R0;
    }
    }
}

int kindOf(const QString& shape) {
    const int k = int(profiles().indexOf(shape));
    if (k < 0) throw Error(("unknown profile '" + shape + "'").toStdString());
    return k;
}

Vec linspace(double a, double b, int n) {                     // numpy.linspace
    Vec v(size_t(std::max(n, 0)));
    if (n == 1) v[0] = a;
    if (n < 2) return v;
    const double step = (b - a) / (n - 1);
    for (int i = 0; i < n; ++i) v[size_t(i)] = i * step + a;
    v[size_t(n - 1)] = b;
    return v;
}

// ---- bounded least squares (stands in for scipy.optimize.least_squares(method="trf", x_scale="jac")):
// Levenberg-Marquardt with Jacobian column scaling; variables at a bound whose gradient points outwards are
// held fixed, steps are projected onto the box. Iterates to the constrained minimum.
struct Model {
    const Vec& x;
    const Vec& y;
    double xc;
    int kind, npar, npk;

    double value(const Eigen::VectorXd& p, double xx, double* grad) const {
        double v = p(0) + p(1) * (xx - xc);
        if (grad) { grad[0] = 1.0; grad[1] = xx - xc; }
        for (int k = 0; k < npk; ++k) {
            const int o = 2 + k * npar;
            v += profileGrad(kind, xx, p.data() + o, grad ? grad + o : nullptr);
        }
        return v;
    }
    void eval(const Eigen::VectorXd& p, Eigen::VectorXd& r, Eigen::MatrixXd* J) const {
        const Eigen::Index n = Eigen::Index(x.size()), m = p.size();
        r.resize(n);
        if (J) J->resize(n, m);
        std::vector<double> g(static_cast<size_t>(m));
        for (Eigen::Index i = 0; i < n; ++i) {
            r(i) = value(p, x[size_t(i)], J ? g.data() : nullptr) - y[size_t(i)];
            if (J)
                for (Eigen::Index j = 0; j < m; ++j) (*J)(i, j) = g[size_t(j)];
        }
    }
};

Eigen::VectorXd boundedLeastSquares(const Model& f, Eigen::VectorXd p, const Eigen::VectorXd& lo,
                                    const Eigen::VectorXd& hi) {
    const Eigen::Index m = p.size();
    p = p.cwiseMax(lo).cwiseMin(hi);
    Eigen::VectorXd r, rn;
    Eigen::MatrixXd J;
    f.eval(p, r, &J);
    double cost = 0.5 * r.squaredNorm();
    Eigen::VectorXd D = J.colwise().norm().transpose();
    for (Eigen::Index j = 0; j < m; ++j)
        if (D(j) == 0) D(j) = 1;
    double mu = 1e-3, nu = 2;
    int nfev = 1;
    while (nfev < 20000) {
        const Eigen::VectorXd g = J.transpose() * r;
        std::vector<Eigen::Index> free;
        for (Eigen::Index j = 0; j < m; ++j)
            if (!((p(j) <= lo(j) && g(j) > 0) || (p(j) >= hi(j) && g(j) < 0))) free.push_back(j);
        if (free.empty()) break;
        const Eigen::Index nf = Eigen::Index(free.size());
        Eigen::MatrixXd A(nf, nf);
        Eigen::VectorXd b(nf);
        const Eigen::MatrixXd JtJ = J.transpose() * J;
        for (Eigen::Index a = 0; a < nf; ++a) {
            b(a) = -g(free[size_t(a)]);
            for (Eigen::Index c = 0; c < nf; ++c) A(a, c) = JtJ(free[size_t(a)], free[size_t(c)]);
            A(a, a) += mu * D(free[size_t(a)]) * D(free[size_t(a)]);
        }
        const Eigen::VectorXd dxf = A.ldlt().solve(b);
        Eigen::VectorXd pn = p;
        for (Eigen::Index a = 0; a < nf; ++a) pn(free[size_t(a)]) += dxf(a);
        pn = pn.cwiseMax(lo).cwiseMin(hi);
        const Eigen::VectorXd s = pn - p;
        if ((s.array().abs() <= 1e-15 * (p.array().abs() + 1e-300)).all()) break;     // no representable step
        f.eval(pn, rn, nullptr);
        ++nfev;
        const double costn = 0.5 * rn.squaredNorm();
        const double pred = -g.dot(s) - 0.5 * (J * s).squaredNorm();
        if (costn < cost) {
            const double rho = pred > 0 ? (cost - costn) / pred : 1.0;
            const bool tiny = cost - costn <= 1e-15 * cost;
            p = pn;
            r = rn;
            cost = costn;
            f.eval(p, r, &J);
            D = D.cwiseMax(J.colwise().norm().transpose());
            mu *= std::max(1.0 / 3.0, 1.0 - std::pow(2 * rho - 1, 3));
            nu = 2;
            if (tiny) break;
        } else {
            mu *= nu;
            nu *= 2;
            if (mu > 1e30) break;
        }
    }
    return p;
}

}  // namespace

// ----------------------------------------------------------------------------- Faddeeva function
// w(z) = exp(-z^2) erfc(-iz), algorithm of G.P.M. Poppe and C.M.J. Wijers (ACM TOMS 680), about 14
// significant digits.
std::complex<double> faddeeva(std::complex<double> z) {
    const double xi = z.real(), yi = z.imag();
    if (yi < 0) {                                             // w(z) = 2 exp(-z^2) - w(-z)
        return 2.0 * std::exp(-z * z) - faddeeva(-z);
    }
    const double factor = 1.12837916709551257388;             // 2 / sqrt(pi)
    const double xa = std::abs(xi), ya = yi;
    const double x = xa / 6.3, y = ya / 4.4;
    double qrho = x * x + y * y;
    const double xq = xa * xa - ya * ya, yq = 2 * xa * ya;
    double u, v;
    if (qrho < 0.085264) {                                    // power series
        qrho = (1 - 0.85 * y) * std::sqrt(qrho);
        const int n = int(std::floor(6 + 72 * qrho + 0.5));
        int j = 2 * n + 1;
        double xs = 1.0 / j, ys = 0.0;
        for (int i = n; i >= 1; --i) {
            j -= 2;
            const double xaux = (xs * xq - ys * yq) / i;
            ys = (xs * yq + ys * xq) / i;
            xs = xaux + 1.0 / j;
        }
        const double u1 = -factor * (xs * ya + ys * xa) + 1.0, v1 = factor * (xs * xa - ys * ya);
        const double d = std::exp(-xq), u2 = d * std::cos(yq), v2 = -d * std::sin(yq);
        u = u1 * u2 - v1 * v2;
        v = u1 * v2 + v1 * u2;
    } else {                                                  // continued fraction / Laplace
        double h = 0.0, h2 = 0.0;
        int kapn = 0, nu;
        if (qrho > 1.0) {
            qrho = std::sqrt(qrho);
            nu = int(3 + (1442 / (26 * qrho + 77)));
        } else {
            qrho = (1 - y) * std::sqrt(1 - qrho);
            h = 1.88 * qrho;
            h2 = 2 * h;
            kapn = int(std::floor(7 + 34 * qrho + 0.5));
            nu = int(std::floor(16 + 26 * qrho + 0.5));
        }
        const bool b = h > 0;
        double ql = b ? std::pow(h2, kapn) : 0.0;
        double rx = 0, ry = 0, sx = 0, sy = 0;
        for (int n = nu; n >= 0; --n) {
            const int np1 = n + 1;
            double tx = ya + h + np1 * rx;
            const double ty = xa - np1 * ry;
            const double c = 0.5 / (tx * tx + ty * ty);
            rx = c * tx;
            ry = c * ty;
            if (b && n <= kapn) {
                tx = ql + sx;
                sx = rx * tx - ry * sy;
                sy = ry * tx + rx * sy;
                ql = ql / h2;
            }
        }
        u = factor * (h == 0 ? rx : sx);
        v = factor * (h == 0 ? ry : sy);
        if (ya == 0) u = std::exp(-xa * xa);
    }
    if (xi < 0) v = -v;
    return {u, v};
}

// ----------------------------------------------------------------------------- picking
double noiseLevel(const Vec& y) {
    Vec d;
    for (size_t i = 0; i + 1 < y.size(); ++i) d.push_back(y[i + 1] - y[i]);
    const double md = median(d);
    for (double& v : d) v = std::abs(v - md);
    const double s = 1.4826 * median(d) / std::sqrt(2.0);
    return s != 0.0 ? s : 1e-12;
}

std::vector<Peak> pick(const Vec& x0, const Vec& y0, double snr, double minFwhm, bool shoulders,
                       std::optional<double> xmin, std::optional<double> xmax) {
    Vec x, y;
    for (size_t i = 0; i < x0.size(); ++i) {
        if ((xmin && !(x0[i] >= *xmin)) || (xmax && !(x0[i] <= *xmax))) continue;
        x.push_back(x0[i]);
        y.push_back(y0[i]);
    }
    if (y.size() < 7) return {};
    const double sigma = noiseLevel(y);
    const double step = medianStep(x);
    const Vec ys = sig::savgolFilter(y, 7, 3);
    const sig::PeakSet ps = sig::findPeaks(ys, snr * sigma, std::max(1.0, minFwhm / step / 2));
    const Vec w = sig::peakWidths(ys, ps.peaks, 0.5);
    std::vector<Peak> out;
    for (size_t k = 0; k < ps.peaks.size(); ++k) {
        const size_t i = size_t(ps.peaks[k]);
        Peak p;
        p.position = subpixel(x, ys, i);
        p.height = ys[i];
        p.fwhm = w[k] * step;
        p.area = ys[i] * p.fwhm * 1.064;
        p.snr = ps.prominences[k] / sigma;
        out.push_back(p);
    }
    if (shoulders) {
        const Vec d2 = sig::savgolFilter(y, 11, 3, 2);
        const double s2 = noiseLevel(d2);
        Vec nd2(d2.size());
        for (size_t i = 0; i < d2.size(); ++i) nd2[i] = -d2[i];
        const sig::PeakSet cand = sig::findPeaks(nd2, snr * s2 * 2);
        const long hw = std::max(3L, long(std::nearbyint(minFwhm * 2 / step)));
        for (int ci : cand.peaks) {
            const long i = ci;
            const size_t lo = size_t(std::max(0L, i - hw)), hi = size_t(std::min(long(ys.size()), i + hw + 1));
            const double yi = ys[size_t(i)];
            const bool onFlank = yi > *std::min_element(ys.begin() + long(lo), ys.begin() + long(hi)) + 3 * snr * sigma;
            const bool monotone = (ys[lo] - yi) * (yi - ys[hi - 1]) > 0;    // rising or falling through i
            bool apart = true;
            for (const Peak& p : out)
                if (!(std::abs(x[size_t(i)] - p.position) > 1.5 * std::max(p.fwhm, 4 * step))) apart = false;
            if (apart && yi > snr * sigma && onFlank && monotone) {
                Peak p;
                p.position = x[size_t(i)];
                p.height = yi;
                p.fwhm = 4 * step;
                p.area = yi * 4 * step;
                p.snr = yi / sigma;
                p.shoulder = true;
                out.push_back(p);
            }
        }
    }
    std::stable_sort(out.begin(), out.end(), [](const Peak& a, const Peak& b) { return a.position < b.position; });
    return out;
}

// ----------------------------------------------------------------------------- profiles
double lorentz(double x, double a, double x0, double w) {
    const double p[3] = {a, x0, w};
    return profileGrad(0, x, p, nullptr);
}
double gauss(double x, double a, double x0, double w) {
    const double p[3] = {a, x0, w};
    return profileGrad(1, x, p, nullptr);
}
double pvoigt(double x, double a, double x0, double w, double eta) {
    const double p[4] = {a, x0, w, eta};
    return profileGrad(2, x, p, nullptr);
}
double voigt(double x, double a, double x0, double wg, double wl) {
    const double p[4] = {a, x0, wg, wl};
    return profileGrad(3, x, p, nullptr);
}

const QStringList& profiles() {
    static const QStringList p = {"lorentz", "gauss", "pvoigt", "voigt"};
    return p;
}

int profileParams(const QString& shape) { return kindOf(shape) < 2 ? 3 : 4; }

double profile(const QString& shape, double x, const double* p) { return profileGrad(kindOf(shape), x, p, nullptr); }

std::optional<double> area(const QString& shape, const double* p) {
    const double g = p[0] * p[2] * std::sqrt(M_PI / (4 * kLn2));
    switch (kindOf(shape)) {
    case 0: return M_PI * p[0] * p[2] / 2;
    case 1: return g;
    case 2: return p[3] * M_PI * p[0] * p[2] / 2 + (1 - p[3]) * g;
    default: return std::nullopt;
    }
}

// ----------------------------------------------------------------------------- fitting
QString FitResult::table() const {
    QStringList rows;
    rows << QString("%1 %2 %3 %4 %5 %6").arg("position", 10).arg(QString("±"), 6).arg("height", 10).arg("fwhm", 8)
                .arg(QString("±"), 6).arg("area", 12);
    for (const FitPeak& p : peaks)
        rows << QString("%1 %2 %3 %4 %5 %6").arg(p.position, 10, 'f', 2).arg(p.positionErr, 6, 'f', 2)
                    .arg(p.height, 10, 'f', 1).arg(p.fwhm, 8, 'f', 2).arg(p.fwhmErr, 6, 'f', 2).arg(p.area, 12, 'f', 1);
    rows << QString("R² = %1, RMS residual = %2").arg(r2, 0, 'f', 5).arg(rms, 0, 'f', 2);
    return rows.join("\n");
}

namespace {

FitResult fitImpl(const Vec& x, const Vec& y, std::pair<double, double> region, const Vec& pos,
                  const std::vector<std::optional<double>>& fwhm, const QString& shape, bool fixPositions) {
    if (pos.empty()) throw Error("no peaks to fit in this region");
    const int kind = kindOf(shape), npar = profileParams(shape), npk = int(pos.size());
    Vec xr, yr;
    for (size_t i = 0; i < x.size(); ++i)
        if (x[i] >= region.first && x[i] <= region.second) { xr.push_back(x[i]); yr.push_back(y[i]); }
    if (xr.empty()) throw Error("no data in this region");
    double xc = 0.0;
    for (double v : xr) xc += v;
    xc /= double(xr.size());
    const double step = medianStep(xr);
    const double ymin = *std::min_element(yr.begin(), yr.end());
    const double inf = std::numeric_limits<double>::infinity();
    std::vector<double> p0 = {ymin, 0.0}, lo = {-inf, -inf}, hi = {inf, inf};
    for (int k = 0; k < npk; ++k) {
        const double x0 = pos[size_t(k)];
        size_t i = 0;
        for (size_t j = 1; j < xr.size(); ++j)
            if (std::abs(xr[j] - x0) < std::abs(xr[i] - x0)) i = j;
        const double h = std::max(yr[i] - ymin, 1e-6);
        const double w = std::max(fwhm[size_t(k)].value_or(10.0), 3 * step);
        const double tol = fixPositions ? 1e-6 : std::max(3 * w, 10 * step);
        if (kind < 2) {
            p0.insert(p0.end(), {h, x0, w});
            lo.insert(lo.end(), {0, x0 - tol, step});
            hi.insert(hi.end(), {inf, x0 + tol, 20 * w + 50});
        } else if (kind == 2) {
            p0.insert(p0.end(), {h, x0, w, 0.5});
            lo.insert(lo.end(), {0, x0 - tol, step, 0});
            hi.insert(hi.end(), {inf, x0 + tol, 20 * w + 50, 1});
        } else {
            p0.insert(p0.end(), {h, x0, w / 2, w / 2});
            lo.insert(lo.end(), {0, x0 - tol, 1e-3, 1e-3});
            hi.insert(hi.end(), {inf, x0 + tol, 20 * w + 50, 20 * w + 50});
        }
    }
    const Eigen::Index m = Eigen::Index(p0.size());
    const Model model{xr, yr, xc, kind, npar, npk};
    const Eigen::VectorXd p = boundedLeastSquares(model, Eigen::Map<Eigen::VectorXd>(p0.data(), m),
                                                  Eigen::Map<Eigen::VectorXd>(lo.data(), m),
                                                  Eigen::Map<Eigen::VectorXd>(hi.data(), m));
    Eigen::VectorXd resid;
    Eigen::MatrixXd J;
    model.eval(p, resid, &J);
    const double dof = double(std::max<Eigen::Index>(1, Eigen::Index(yr.size()) - m));
    const double s2 = resid.squaredNorm() / dof;
    // covariance = pinv(J'J) * s2 (numpy.linalg.pinv, rcond 1e-15)
    const Eigen::MatrixXd JtJ = J.transpose() * J;
    Eigen::JacobiSVD<Eigen::MatrixXd> svd(JtJ, Eigen::ComputeFullU | Eigen::ComputeFullV);
    const Eigen::VectorXd sv = svd.singularValues();
    Eigen::VectorXd inv = Eigen::VectorXd::Zero(sv.size());
    for (Eigen::Index k = 0; k < sv.size(); ++k)
        if (sv(k) > 1e-15 * sv(0)) inv(k) = 1.0 / sv(k);
    const Eigen::MatrixXd cov = svd.matrixV() * inv.asDiagonal() * svd.matrixU().transpose() * s2;
    Eigen::VectorXd err(m);
    for (Eigen::Index k = 0; k < m; ++k) err(k) = std::sqrt(std::max(cov(k, k), 0.0));

    FitResult res;
    res.shape = shape;
    res.baseline = {p(0), p(1)};
    res.x = xr;
    res.y = yr;
    for (int k = 0; k < npk; ++k) {
        const int o = 2 + k * npar;
        const double* q = p.data() + o;
        const double* e = err.data() + o;
        Vec comp(xr.size());
        for (size_t i = 0; i < xr.size(); ++i) comp[i] = profileGrad(kind, xr[i], q, nullptr);
        res.components.push_back(comp);
        FitPeak fp;
        fp.position = q[1];
        fp.positionErr = e[1];
        fp.height = q[0];
        fp.heightErr = e[0];
        if (kind == 3) {
            const double fg = q[2], fl = q[3];
            fp.fwhm = 0.5346 * fl + std::sqrt(0.2166 * fl * fl + fg * fg);
            fp.fwhmErr = std::hypot(e[2], e[3]);
            const Vec dense = linspace(q[1] - 50 * fp.fwhm, q[1] + 50 * fp.fwhm, 20001);
            double a = 0.0, prev = profileGrad(kind, dense[0], q, nullptr);
            for (size_t i = 1; i < dense.size(); ++i) {
                const double cur = profileGrad(kind, dense[i], q, nullptr);
                a += (dense[i] - dense[i - 1]) * (cur + prev) / 2.0;
                prev = cur;
            }
            fp.area = a;
        } else {
            fp.fwhm = q[2];
            fp.fwhmErr = e[2];
            fp.area = *area(shape, q);
        }
        if (kind == 2) fp.eta = q[3];
        res.peaks.push_back(fp);
    }
    res.model.resize(xr.size());
    double ssRes = 0.0, mean = 0.0, ssTot = 0.0;
    for (size_t i = 0; i < xr.size(); ++i) {
        res.model[i] = model.value(p, xr[i], nullptr);
        ssRes += (yr[i] - res.model[i]) * (yr[i] - res.model[i]);
        mean += yr[i];
    }
    mean /= double(yr.size());
    for (double v : yr) ssTot += (v - mean) * (v - mean);
    if (ssTot == 0.0) ssTot = 1.0;
    res.r2 = 1 - ssRes / ssTot;
    res.rms = std::sqrt(ssRes / double(yr.size()));
    return res;
}

}  // namespace

FitResult fit(const Vec& x, const Vec& y, std::pair<double, double> region,
              const std::optional<std::vector<Peak>>& guesses, const QString& shape, bool fixPositions) {
    std::vector<Peak> g;
    if (guesses) {
        g = *guesses;
    } else {             // pick on the whole spectrum (a noise estimate from a zoomed band is far too high)
        for (const Peak& p : pick(x, y, 4.0))
            if (region.first <= p.position && p.position <= region.second) g.push_back(p);
    }
    Vec pos;
    std::vector<std::optional<double>> w;
    for (const Peak& p : g) {
        pos.push_back(p.position);
        std::optional<double> f;
        for (const Peak& q : g)                               // the first guess at this position
            if (q.position == p.position) { f = q.fwhm; break; }
        w.push_back(f);
    }
    return fitImpl(x, y, region, pos, w, shape, fixPositions);
}

FitResult fit(const Vec& x, const Vec& y, std::pair<double, double> region, const Vec& positions,
              const QString& shape, bool fixPositions) {
    return fitImpl(x, y, region, positions, std::vector<std::optional<double>>(positions.size()), shape, fixPositions);
}

}  // namespace lr::peaks
