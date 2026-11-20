// Chemometrics (see chemometrics.h for the methods and references).
#include "chemometrics.h"

#include <QDateTime>
#include <QRegularExpression>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numeric>
#include <random>

#include "errors.h"
#include "processing.h"

namespace lr::chemo {

namespace {

const double kNaN = std::numeric_limits<double>::quiet_NaN();
const double kInf = std::numeric_limits<double>::infinity();

[[noreturn]] void fail(const QString& msg) { throw Error(msg.toStdString()); }

// ---- SVD (Eigen 3.4.90+ takes the options as a template argument; older versions at run time)
struct Svd {
    Matrix U, V;
    Vector s;
};

Svd thinSvd(const Matrix& X) {
#if EIGEN_VERSION_AT_LEAST(3, 4, 90)
    Eigen::BDCSVD<Matrix, Eigen::ComputeThinU | Eigen::ComputeThinV> svd(X);
#else
    Eigen::BDCSVD<Matrix> svd(X, Eigen::ComputeThinU | Eigen::ComputeThinV);
#endif
    return {svd.matrixU(), svd.matrixV(), svd.singularValues()};
}

// Singular values of a matrix with few rows from its Gram matrix block (EFA windows).
Vector gramSingularValues(const Matrix& G) {
    Eigen::SelfAdjointEigenSolver<Matrix> es(G, Eigen::EigenvaluesOnly);
    Vector ev = es.eigenvalues().reverse();       // descending
    for (Eigen::Index i = 0; i < ev.size(); ++i) ev(i) = std::sqrt(std::max(0.0, ev(i)));
    return ev;
}

// Least-squares solution of G X = B for a small symmetric (possibly singular) G: minimum-norm solution.
Matrix solveSmall(const Matrix& G, const Matrix& B) {
    Eigen::CompleteOrthogonalDecomposition<Matrix> cod(G);
    return cod.solve(B);
}

double median(Vec v) {
    if (v.empty()) return kNaN;
    std::sort(v.begin(), v.end());
    const size_t m = v.size() / 2;
    return v.size() % 2 ? v[m] : 0.5 * (v[m - 1] + v[m]);
}

// ---- quantiles (approximations good to ~1 % for the 95 % limits)
const double kZ95 = 1.6448536269514722;

double chi2Quantile95(double h) {                 // Wilson-Hilferty
    if (h <= 0) return 0.0;
    const double a = 2.0 / (9.0 * h);
    const double c = 1.0 - a + kZ95 * std::sqrt(a);
    return h * c * c * c;
}

double fQuantile95(double d1, double d2) {        // Paulson's normal approximation, solved for F
    const double A = 2.0 / (9.0 * d1), B = 2.0 / (9.0 * d2);
    const double a = 1.0 - B, b = 1.0 - A, z2 = kZ95 * kZ95;
    const double qa = a * a - z2 * B, qb = -2.0 * a * b, qc = b * b - z2 * A;
    if (qa <= 0) return kInf;
    const double disc = qb * qb - 4 * qa * qc;
    if (disc < 0) return kInf;
    const double u = (-qb + std::sqrt(disc)) / (2 * qa);
    return u * u * u;
}

// ---- time stamps of spectra
struct TimeOf {
    double value = kNaN;
    bool absolute = false;                        // epoch seconds (else elapsed seconds)
};

std::optional<double> parseElapsed(const QString& s) {     // "t=12.5s", "12.5"
    static const QRegularExpression re(QStringLiteral("^\\s*(?:t\\s*=\\s*)?([-+]?[0-9]*\\.?[0-9]+(?:[eE][-+]?[0-9]+)?)\\s*s?\\s*$"));
    const auto m = re.match(s);
    if (!m.hasMatch()) return std::nullopt;
    bool ok = false;
    const double v = m.captured(1).toDouble(&ok);
    return ok ? std::optional<double>(v) : std::nullopt;
}

std::optional<double> parseIso(const QString& s) {
    const QDateTime t = QDateTime::fromString(s.trimmed(), Qt::ISODateWithMs);
    if (!t.isValid()) return std::nullopt;
    return double(t.toMSecsSinceEpoch()) / 1000.0;
}

TimeOf timeOf(const fileio::DataSpectrum& d) {
    const QString t = d.metaValue(QStringLiteral("time"));
    if (!t.isEmpty()) {
        if (const auto v = parseElapsed(t)) return {*v, false};
        if (const auto v = parseIso(t)) return {*v, true};
    }
    const QString el = d.metaValue(QStringLiteral("series_elapsed_s"));
    if (const auto v = parseElapsed(el); v && !el.isEmpty()) return {*v, false};
    for (const auto& kv : d.meta) {
        // the first line of a LibreRamen/Cora spectrum file, "# Cora 5000 spectrum, 2026-11-20T18:21:07+01:00",
        // is read as key "Cora 5000 spectrum, 2026-11-20T18" and value "21:07+01:00"
        const int c = int(kv.first.indexOf(QStringLiteral("spectrum, ")));
        if (c >= 0) {
            if (const auto v = parseIso(kv.first.mid(c + 10) + ':' + kv.second)) return {*v, true};
        }
        if (kv.first == QLatin1String("timestamp") || kv.first == QLatin1String("acquired")) {
            if (const auto v = parseIso(kv.second)) return {*v, true};
        }
    }
    return {};
}

void setTimes(DataMatrix& m, const std::vector<TimeOf>& t) {
    const int n = int(t.size());
    m.times.assign(size_t(n), 0.0);
    bool all = n > 0, abs = n > 0 && t[0].absolute;
    for (const auto& v : t) all = all && std::isfinite(v.value) && v.absolute == abs;
    if (all) {
        double t0 = 0;
        if (abs) {
            t0 = kInf;
            for (const auto& v : t) t0 = std::min(t0, v.value);
        }
        for (int i = 0; i < n; ++i) m.times[size_t(i)] = t[size_t(i)].value - t0;
        m.hasTimes = true;
    } else {
        for (int i = 0; i < n; ++i) m.times[size_t(i)] = i;
        m.hasTimes = false;
    }
}

// ---- Nelder-Mead minimiser (ambiguity bands)
Vector nelderMead(const std::function<double(const Vector&)>& f, Vector x0, double step, int maxEval,
                  const std::atomic<bool>* stop) {
    const int d = int(x0.size());
    if (d == 0) return x0;
    std::vector<Vector> pts(size_t(d + 1), x0);
    std::vector<double> val(size_t(d + 1));
    for (int i = 0; i < d; ++i) pts[size_t(i + 1)](i) += step;
    int evals = 0;
    for (int i = 0; i <= d; ++i, ++evals) val[size_t(i)] = f(pts[size_t(i)]);
    std::vector<int> idx(size_t(d + 1));
    while (evals < maxEval) {
        if (stop && stop->load()) break;
        std::iota(idx.begin(), idx.end(), 0);
        std::sort(idx.begin(), idx.end(), [&](int a, int b) { return val[size_t(a)] < val[size_t(b)]; });
        const int best = idx[0], worst = idx[size_t(d)], second = idx[size_t(d - 1)];
        const double fb = val[size_t(best)], fw = val[size_t(worst)];
        if (std::abs(fw - fb) <= 1e-10 * (std::abs(fb) + 1e-10)) {
            double spread = 0;
            for (int i = 0; i <= d; ++i) spread = std::max(spread, (pts[size_t(i)] - pts[size_t(best)]).cwiseAbs().maxCoeff());
            if (spread < 1e-7) break;
        }
        Vector centroid = Vector::Zero(d);
        for (int i = 0; i <= d; ++i)
            if (i != worst) centroid += pts[size_t(i)];
        centroid /= d;
        const Vector xr = centroid + (centroid - pts[size_t(worst)]);
        const double fr = f(xr);
        ++evals;
        if (fr < fb) {
            const Vector xe = centroid + 2.0 * (centroid - pts[size_t(worst)]);
            const double fe = f(xe);
            ++evals;
            if (fe < fr) {
                pts[size_t(worst)] = xe;
                val[size_t(worst)] = fe;
            } else {
                pts[size_t(worst)] = xr;
                val[size_t(worst)] = fr;
            }
        } else if (fr < val[size_t(second)]) {
            pts[size_t(worst)] = xr;
            val[size_t(worst)] = fr;
        } else {
            const bool outside = fr < fw;
            const Vector xc = outside ? Vector(centroid + 0.5 * (xr - centroid))
                                      : Vector(centroid + 0.5 * (pts[size_t(worst)] - centroid));
            const double fc = f(xc);
            ++evals;
            if (fc < (outside ? fr : fw)) {
                pts[size_t(worst)] = xc;
                val[size_t(worst)] = fc;
            } else {                                   // shrink towards the best point
                for (int i = 0; i <= d; ++i) {
                    if (i == best) continue;
                    pts[size_t(i)] = pts[size_t(best)] + 0.5 * (pts[size_t(i)] - pts[size_t(best)]);
                    val[size_t(i)] = f(pts[size_t(i)]);
                    ++evals;
                }
            }
        }
    }
    int best = 0;
    for (int i = 1; i <= d; ++i)
        if (val[size_t(i)] < val[size_t(best)]) best = i;
    return pts[size_t(best)];
}

}  // namespace

// ============================================================================================ data matrix
Vec DataMatrix::row(int i) const {
    Vec v(size_t(D.cols()));
    for (Eigen::Index j = 0; j < D.cols(); ++j) v[size_t(j)] = D(i, j);
    return v;
}

DataMatrix DataMatrix::subset(const std::vector<int>& rowsIn) const {
    DataMatrix m;
    m.x = x;
    m.D.resize(Eigen::Index(rowsIn.size()), D.cols());
    m.hasTimes = hasTimes;
    for (size_t r = 0; r < rowsIn.size(); ++r) {
        const int i = rowsIn[r];
        if (i < 0 || i >= rows()) fail(QStringLiteral("spectrum index out of range"));
        m.D.row(Eigen::Index(r)) = D.row(i);
        m.names << (i < names.size() ? names[i] : QString());
        m.times.push_back(size_t(i) < times.size() ? times[size_t(i)] : double(i));
    }
    return m;
}

Vec resample(const Vec& x, const Vec& y, const Vec& grid) {
    Vec out(grid.size(), kNaN);
    const size_t n = std::min(x.size(), y.size());
    if (n == 0) return out;
    for (size_t g = 0; g < grid.size(); ++g) {
        const double v = grid[g];
        if (v <= x[0]) {
            out[g] = y[0];
        } else if (v >= x[n - 1]) {
            out[g] = y[n - 1];
        } else {
            const size_t hi = size_t(std::upper_bound(x.begin(), x.begin() + long(n), v) - x.begin());
            const size_t lo = hi - 1;
            const double dx = x[hi] - x[lo];
            out[g] = dx > 0 ? y[lo] + (y[hi] - y[lo]) * (v - x[lo]) / dx : y[lo];
        }
    }
    return out;
}

DataMatrix buildMatrix(const std::vector<fileio::DataSpectrum>& in, double step) {
    DataMatrix m;
    if (in.empty()) return m;
    std::vector<fileio::DataSpectrum> sp = in;
    for (auto& d : sp) {
        const size_t n = std::min(d.x.size(), d.y.size());
        d.x.resize(n);
        d.y.resize(n);
        d.signal.clear();
        d.background.clear();
        d.sorted();
        if (n < 2) fail(QStringLiteral("%1: fewer than two points").arg(d.name));
    }
    bool same = true;
    for (const auto& d : sp) {
        if (d.x.size() != sp[0].x.size()) {
            same = false;
            break;
        }
        for (size_t i = 0; i < d.x.size() && same; ++i)
            same = std::abs(d.x[i] - sp[0].x[i]) <= 1e-9 * std::max(1.0, std::abs(d.x[i]));
        if (!same) break;
    }
    if (same) {
        m.x = sp[0].x;
    } else {
        double lo = -kInf, hi = kInf;
        Vec spacings;
        for (const auto& d : sp) {
            lo = std::max(lo, d.x.front());
            hi = std::min(hi, d.x.back());
            Vec dx;
            for (size_t i = 1; i < d.x.size(); ++i) dx.push_back(d.x[i] - d.x[i - 1]);
            spacings.push_back(median(dx));
        }
        if (!(hi > lo)) fail(QStringLiteral("the spectra have no common x range"));
        const double h = step > 0 ? step : median(spacings);
        if (!(h > 0)) fail(QStringLiteral("invalid x spacing"));
        const auto npts = size_t(std::floor((hi - lo) / h + 1e-9)) + 1;
        for (size_t i = 0; i < npts; ++i) m.x.push_back(lo + double(i) * h);
        if (m.x.size() < 2) fail(QStringLiteral("the common x range of the spectra is too short"));
    }
    m.D.resize(Eigen::Index(sp.size()), Eigen::Index(m.x.size()));
    std::vector<TimeOf> times;
    for (size_t r = 0; r < sp.size(); ++r) {
        const Vec y = same ? sp[r].y : resample(sp[r].x, sp[r].y, m.x);
        for (size_t j = 0; j < y.size(); ++j) m.D(Eigen::Index(r), Eigen::Index(j)) = y[j];
        m.names << (sp[r].name.isEmpty() ? QStringLiteral("spectrum %1").arg(r + 1) : sp[r].name);
        times.push_back(timeOf(sp[r]));
    }
    setTimes(m, times);
    return m;
}

DataMatrix buildMatrix(const std::vector<Spectrum>& spectra, double step) {
    std::vector<fileio::DataSpectrum> ds;
    std::vector<TimeOf> times;
    for (size_t i = 0; i < spectra.size(); ++i) {
        ds.push_back(fileio::fromSpectrum(spectra[i]));
        if (ds.back().name == QLatin1String("spectrum")) ds.back().name = QStringLiteral("spectrum %1").arg(i + 1);
        const QDateTime& t = spectra[i].t;
        times.push_back(t.isValid() ? TimeOf{double(t.toMSecsSinceEpoch()) / 1000.0, true} : TimeOf{});
    }
    DataMatrix m = buildMatrix(ds, step);
    setTimes(m, times);
    return m;
}

DataMatrix readSeries(const QString& path) { return buildMatrix(fileio::read(path)); }

// ============================================================================================ preprocessing
DataMatrix preprocess(const DataMatrix& data, const Preprocess& p) {
    DataMatrix out;
    out.names = data.names;
    out.times = data.times;
    out.hasTimes = data.hasTimes;
    if (data.empty()) {
        out.x = data.x;
        out.D = data.D;
        return out;
    }
    if (p.derivative < 0 || p.derivative > 2) fail(QStringLiteral("derivative must be 0, 1 or 2"));
    // region
    size_t i0 = 0, i1 = data.x.size();
    if (p.xmin)
        while (i0 < i1 && data.x[i0] < *p.xmin) ++i0;
    if (p.xmax)
        while (i1 > i0 && data.x[i1 - 1] > *p.xmax) --i1;
    if (i1 < i0 + 2) fail(QStringLiteral("the selected region contains fewer than two points"));
    out.x.assign(data.x.begin() + long(i0), data.x.begin() + long(i1));
    out.D.resize(data.D.rows(), Eigen::Index(i1 - i0));
    for (Eigen::Index r = 0; r < data.D.rows(); ++r) {
        Vec y = data.row(int(r));
        if (p.smooth) y = processing::smooth(y, p.smoothWindow | 1);
        if (!p.baseline.isEmpty() && p.baseline != QLatin1String("none")) {
            const Vec b = processing::baseline(data.x, y, p.baseline, p.strength);
            for (size_t i = 0; i < y.size(); ++i) y[i] -= b[i];
        }
        if (p.derivative > 0) y = sig::savgolFilter(y, std::max(5, p.derivWindow | 1), 3, p.derivative);
        Vec yr(y.begin() + long(i0), y.begin() + long(i1));
        yr = processing::normalize(out.x, yr, p.normalize, p.band);
        for (size_t i = 0; i < yr.size(); ++i) out.D(r, Eigen::Index(i)) = yr[i];
    }
    return out;
}

ColumnScaling columnScaling(const Matrix& D, Scaling s) {
    ColumnScaling cs;
    const Eigen::Index n = D.rows(), p = D.cols();
    cs.mean = Vector::Zero(p);
    cs.scale = Vector::Ones(p);
    if (s == Scaling::None || n == 0) return cs;
    cs.mean = D.colwise().mean().transpose();
    if (s == Scaling::Autoscale && n > 1) {
        for (Eigen::Index j = 0; j < p; ++j) {
            const double sd = std::sqrt((D.col(j).array() - cs.mean(j)).square().sum() / double(n - 1));
            cs.scale(j) = sd > 1e-300 ? sd : 1.0;
        }
    }
    return cs;
}

Matrix applyScaling(const Matrix& D, const ColumnScaling& cs) {
    Matrix X = D.rowwise() - cs.mean.transpose();
    return X.array().rowwise() / cs.scale.transpose().array();
}

// ============================================================================================ PCA
PcaResult pca(const Matrix& D, int components, Scaling scaling) {
    PcaResult r;
    const Eigen::Index n = D.rows(), p = D.cols();
    if (n < 2 || p < 1) fail(QStringLiteral("PCA needs at least two spectra"));
    r.scaling = columnScaling(D, scaling);
    const Matrix X = applyScaling(D, r.scaling);
    const Svd svd = thinSvd(X);
    const int m = int(svd.s.size());
    const int k = std::clamp(components, 1, m);
    r.components = k;
    const double total = svd.s.squaredNorm();
    double cum = 0;
    for (int i = 0; i < m; ++i) {
        r.singularValues.push_back(svd.s(i));
        const double e = total > 0 ? svd.s(i) * svd.s(i) / total : 0.0;
        cum += e;
        r.explained.push_back(e);
        r.cumulative.push_back(cum);
    }
    r.loadings = svd.V.leftCols(k);
    r.scores = svd.U.leftCols(k) * svd.s.head(k).asDiagonal();
    for (int j = 0; j < k; ++j) {                 // sign convention: largest |loading| positive
        Eigen::Index imax = 0;
        r.loadings.col(j).cwiseAbs().maxCoeff(&imax);
        if (r.loadings(imax, j) < 0) {
            r.loadings.col(j) *= -1;
            r.scores.col(j) *= -1;
        }
    }
    const Matrix E = X - r.scores * r.loadings.transpose();
    r.q.resize(size_t(n));
    r.t2.resize(size_t(n));
    for (Eigen::Index i = 0; i < n; ++i) {
        r.q[size_t(i)] = E.row(i).squaredNorm();
        double t2 = 0;
        for (int j = 0; j < k; ++j) {
            const double lam = svd.s(j) * svd.s(j) / double(n - 1);
            if (lam > 0) t2 += r.scores(i, j) * r.scores(i, j) / lam;
        }
        r.t2[size_t(i)] = t2;
    }
    // 95 % limits
    double th1 = 0, th2 = 0;
    for (int i = k; i < m; ++i) {
        const double lam = svd.s(i) * svd.s(i) / double(n - 1);
        th1 += lam;
        th2 += lam * lam;
    }
    r.qLimit = th2 > 0 ? (th2 / th1) * chi2Quantile95(th1 * th1 / th2) : 0.0;
    r.t2Limit = n > k ? double(k) * double(n - 1) / double(n - k) * fQuantile95(k, double(n - k)) : kInf;
    return r;
}

int rankForVariance(const PcaResult& r, double fraction) {
    for (size_t i = 0; i < r.cumulative.size(); ++i)
        if (r.cumulative[i] >= fraction) return int(i) + 1;
    return int(r.cumulative.size());
}

// ============================================================================================ EFA
EfaResult efa(const Matrix& D, int components, double threshold) {
    EfaResult e;
    const Eigen::Index n = D.rows();
    if (n < 1 || D.cols() < 1) fail(QStringLiteral("EFA needs data"));
    const int k = std::clamp(components, 1, int(std::min(n, D.cols())));
    const Matrix G = D * D.transpose();
    e.forward = Matrix::Constant(n, k, kNaN);
    e.backward = Matrix::Constant(n, k, kNaN);
    for (Eigen::Index i = 0; i < n; ++i) {
        const Vector f = gramSingularValues(G.topLeftCorner(i + 1, i + 1));
        for (Eigen::Index j = 0; j < std::min<Eigen::Index>(k, f.size()); ++j) e.forward(i, j) = f(j);
        const Vector b = gramSingularValues(G.bottomRightCorner(n - i, n - i));
        for (Eigen::Index j = 0; j < std::min<Eigen::Index>(k, b.size()); ++j) e.backward(i, j) = b(j);
    }
    const Vector all = gramSingularValues(G);
    e.noise = k < all.size() ? all(k) : 0.0;
    e.threshold = threshold > 0 ? threshold : 2.0 * e.noise;
    for (int j = 0; j < k; ++j) {
        int first = -1, last = -1;
        for (Eigen::Index i = 0; i < n; ++i)
            if (std::isfinite(e.forward(i, j)) && e.forward(i, j) > e.threshold) {
                first = int(i);
                break;
            }
        const int jb = k - 1 - j;
        for (Eigen::Index i = n - 1; i >= 0; --i)
            if (std::isfinite(e.backward(i, jb)) && e.backward(i, jb) > e.threshold) {
                last = int(i);
                break;
            }
        if (first < 0 || last < 0 || first > last) {
            first = 0;
            last = int(n) - 1;
        }
        e.windows.emplace_back(first, last);
    }
    return e;
}

Matrix efaConcentrations(const EfaResult& e, int n) {
    Matrix C = Matrix::Zero(n, Eigen::Index(e.windows.size()));
    for (size_t j = 0; j < e.windows.size(); ++j)
        for (int i = std::max(0, e.windows[j].first); i <= std::min(n - 1, e.windows[j].second); ++i)
            C(i, Eigen::Index(j)) = 1.0;
    return C;
}

// ============================================================================================ OPA
OpaResult opa(const Matrix& Din, int components, bool variables) {
    OpaResult r;
    const Matrix X = variables ? Matrix(Din.transpose()) : Din;   // objects = rows of X
    const Eigen::Index m = X.rows();
    if (m < 1 || X.cols() < 1) fail(QStringLiteral("OPA needs data"));
    const int k = std::clamp(components, 1, int(std::min(m, X.cols())));
    Matrix Y = X;
    Vec weight(static_cast<size_t>(m), 0.0);
    const double maxNorm = X.rowwise().norm().maxCoeff();
    for (Eigen::Index i = 0; i < m; ++i) {
        const double nrm = Y.row(i).norm();
        if (nrm > 0) Y.row(i) /= nrm;
        weight[size_t(i)] = maxNorm > 0 ? nrm / maxNorm : 0.0;
    }
    Vector mean = Y.colwise().mean().transpose();
    if (mean.norm() > 0) mean.normalize();
    std::vector<Vector> refs;
    for (int step = 0; step < k; ++step) {
        std::vector<Vector> R = step == 0 ? std::vector<Vector>{mean} : refs;
        const int q = int(R.size()) + 1;
        Vec dis(static_cast<size_t>(m));
        Matrix M(X.cols(), q);
        for (int c = 0; c < q - 1; ++c) M.col(c) = R[size_t(c)];
        for (Eigen::Index i = 0; i < m; ++i) {
            M.col(q - 1) = Y.row(i).transpose();
            dis[size_t(i)] = weight[size_t(i)] * std::max(0.0, (M.transpose() * M).determinant());
        }
        for (int s : r.selected) dis[size_t(s)] = 0.0;
        const int best = int(std::max_element(dis.begin(), dis.end()) - dis.begin());
        r.dissimilarity.push_back(dis);
        r.selected.push_back(best);
        refs.push_back(Y.row(best).transpose());
    }
    if (variables) {
        r.estimate.resize(Din.rows(), k);
        for (int j = 0; j < k; ++j) r.estimate.col(j) = Din.col(r.selected[size_t(j)]);
    } else {
        r.estimate.resize(k, Din.cols());
        for (int j = 0; j < k; ++j) r.estimate.row(j) = Din.row(r.selected[size_t(j)]);
    }
    return r;
}

// ============================================================================================ SIMPLISMA
SimplismaResult simplisma(const Matrix& D, int components, double offsetPercent) {
    SimplismaResult r;
    const Eigen::Index n = D.rows(), p = D.cols();
    if (n < 2 || p < 1) fail(QStringLiteral("SIMPLISMA needs at least two spectra"));
    const int k = std::clamp(components, 1, int(std::min(n, p)));
    const Vector mu = D.colwise().mean().transpose();
    Vector sd(p);
    for (Eigen::Index j = 0; j < p; ++j) sd(j) = std::sqrt((D.col(j).array() - mu(j)).square().mean());
    const double alpha = offsetPercent / 100.0 * mu.maxCoeff();
    r.stdSpectrum.assign(sd.data(), sd.data() + p);
    Vector purity0(p), lambda(p);
    for (Eigen::Index j = 0; j < p; ++j) {
        const double den = mu(j) + alpha;
        purity0(j) = den > 1e-300 ? sd(j) / den : 0.0;
        lambda(j) = std::sqrt(mu(j) * mu(j) + (sd(j) + alpha) * (sd(j) + alpha));
    }
    Matrix Y = D;                                 // scaled data, columns divided by lambda
    for (Eigen::Index j = 0; j < p; ++j) Y.col(j) /= (lambda(j) > 0 ? lambda(j) : 1.0);
    for (int step = 0; step < k; ++step) {
        const int q = step + 1;
        Matrix M(n, q);
        for (int c = 0; c < step; ++c) M.col(c + 1) = Y.col(r.pureVariables[size_t(c)]);
        Vec pur(static_cast<size_t>(p));
        for (Eigen::Index j = 0; j < p; ++j) {
            M.col(0) = Y.col(j);
            const double w = (M.transpose() * M / double(n)).determinant();
            pur[size_t(j)] = w * purity0(j);
        }
        for (int s : r.pureVariables) pur[size_t(s)] = 0.0;
        const int best = int(std::max_element(pur.begin(), pur.end()) - pur.begin());
        r.purity.push_back(pur);
        r.pureVariables.push_back(best);
    }
    r.C.resize(n, k);
    for (int j = 0; j < k; ++j) r.C.col(j) = D.col(r.pureVariables[size_t(j)]);
    r.S = solveSmall(r.C.transpose() * r.C, r.C.transpose() * D);
    for (int j = 0; j < k; ++j) {
        Eigen::Index imax = 0;
        const double a = r.S.row(j).cwiseAbs().maxCoeff(&imax);
        if (a <= 0) continue;
        const double s = r.S(j, imax) < 0 ? -a : a;
        r.S.row(j) /= s;
        r.C.col(j) *= s;
    }
    return r;
}

// ============================================================================================ NNLS
Vector nnlsNormal(const Matrix& AtA, const Vector& Atb, double tol) {
    const Eigen::Index k = AtA.rows();
    Vector x = Vector::Zero(k);
    if (k == 0) return x;
    if (tol <= 0) tol = 1e-12 * std::max(1e-300, Atb.cwiseAbs().maxCoeff()) * double(k);
    std::vector<bool> passive(size_t(k), false);
    auto solvePassive = [&](Vector& s) {
        std::vector<Eigen::Index> P;
        for (Eigen::Index i = 0; i < k; ++i)
            if (passive[size_t(i)]) P.push_back(i);
        s = Vector::Zero(k);
        if (P.empty()) return;
        const auto q = Eigen::Index(P.size());
        Matrix G(q, q);
        Vector b(q);
        for (Eigen::Index a = 0; a < q; ++a) {
            b(a) = Atb(P[size_t(a)]);
            for (Eigen::Index c = 0; c < q; ++c) G(a, c) = AtA(P[size_t(a)], P[size_t(c)]);
        }
        Vector z;
        Eigen::LDLT<Matrix> ldlt(G);
        if (ldlt.info() == Eigen::Success && ldlt.isPositive() && ldlt.vectorD().minCoeff() > 1e-14 * ldlt.vectorD().maxCoeff())
            z = ldlt.solve(b);
        else
            z = solveSmall(G, b);
        for (Eigen::Index a = 0; a < q; ++a) s(P[size_t(a)]) = z(a);
    };
    Vector w = Atb - AtA * x;
    const int maxOuter = int(3 * k + 10);
    for (int outer = 0; outer < maxOuter; ++outer) {
        Eigen::Index jmax = -1;
        double wmax = tol;
        for (Eigen::Index i = 0; i < k; ++i)
            if (!passive[size_t(i)] && w(i) > wmax) {
                wmax = w(i);
                jmax = i;
            }
        if (jmax < 0) break;
        passive[size_t(jmax)] = true;
        Vector s;
        solvePassive(s);
        for (int inner = 0; inner < int(3 * k + 10); ++inner) {
            double minS = kInf;
            for (Eigen::Index i = 0; i < k; ++i)
                if (passive[size_t(i)]) minS = std::min(minS, s(i));
            if (minS > 0) break;
            double alpha = kInf;
            Eigen::Index jmin = -1;
            for (Eigen::Index i = 0; i < k; ++i)
                if (passive[size_t(i)] && s(i) <= 0) {
                    const double a = x(i) / (x(i) - s(i));
                    if (a < alpha) {
                        alpha = a;
                        jmin = i;
                    }
                }
            if (!std::isfinite(alpha)) alpha = 0;
            x += alpha * (s - x);
            const double xtol = 1e-12 * x.cwiseAbs().maxCoeff();
            for (Eigen::Index i = 0; i < k; ++i)
                if (passive[size_t(i)] && (i == jmin || x(i) <= xtol)) {
                    passive[size_t(i)] = false;
                    x(i) = 0;
                }
            solvePassive(s);
        }
        x = s.cwiseMax(0.0);
        w = Atb - AtA * x;
    }
    return x;
}

Vector nnls(const Matrix& A, const Vector& b) { return nnlsNormal(A.transpose() * A, A.transpose() * b); }

NnlsFit nnlsFit(const Matrix& components, const Vec& x, const Vec& y, int baselineOrder) {
    const Eigen::Index k = components.rows(), p = components.cols();
    if (k < 1) fail(QStringLiteral("no component spectra"));
    if (Eigen::Index(y.size()) != p) fail(QStringLiteral("the spectrum and the components have different lengths"));
    const Vector yv = Eigen::Map<const Vector>(y.data(), p);
    const Matrix K = components.transpose();      // p × k
    NnlsFit f;
    Vector c;
    Vector bg = Vector::Zero(p);
    if (baselineOrder >= 0) {
        if (Eigen::Index(x.size()) != p) fail(QStringLiteral("x and y have different lengths"));
        const double lo = x.front(), hi = x.back();
        const double mid = 0.5 * (lo + hi), half = hi > lo ? 0.5 * (hi - lo) : 1.0;
        Matrix P(p, baselineOrder + 1);
        for (Eigen::Index i = 0; i < p; ++i) {
            const double t = (x[size_t(i)] - mid) / half;
            double v = 1;
            for (int o = 0; o <= baselineOrder; ++o, v *= t) P(i, o) = v;
        }
        // eliminate the free polynomial: project onto its orthogonal complement, NNLS there
        Eigen::HouseholderQR<Matrix> qr(P);
        const Matrix Q = qr.householderQ() * Matrix::Identity(p, P.cols());
        const Matrix Kp = K - Q * (Q.transpose() * K);
        const Vector yp = yv - Q * (Q.transpose() * yv);
        c = nnls(Kp, yp);
        bg = Q * (Q.transpose() * (yv - K * c));
    } else {
        c = nnls(K, yv);
    }
    const Vector fit = K * c + bg;
    const Vector res = yv - fit;
    f.coefficients = toVec(c);
    f.background = toVec(bg);
    f.fitted = toVec(fit);
    f.residual = toVec(res);
    f.residualNorm = res.norm();
    const double sst = (yv.array() - yv.mean()).square().sum();
    f.r2 = sst > 0 ? 1.0 - res.squaredNorm() / sst : 0.0;
    return f;
}

// ============================================================================================ MCR-ALS
void makeUnimodal(Eigen::Ref<Vector> c, double tolerance) {
    const Eigen::Index n = c.size();
    if (n < 3) return;
    Eigen::Index m = 0;
    c.maxCoeff(&m);
    for (Eigen::Index i = m + 1; i < n; ++i)
        if (c(i - 1) >= 0 && c(i) > tolerance * c(i - 1)) c(i) = tolerance * c(i - 1);
    for (Eigen::Index i = m - 1; i >= 0; --i)
        if (c(i + 1) >= 0 && c(i) > tolerance * c(i + 1)) c(i) = tolerance * c(i + 1);
}

McrResult mcrAls(const Matrix& D, const McrStart& start, const McrOptions& opt, const std::atomic<bool>* stop,
                 const McrProgress& progress) {
    const Eigen::Index n = D.rows(), p = D.cols();
    if (n < 1 || p < 1) fail(QStringLiteral("MCR-ALS needs data"));
    const bool fromC = start.S.size() == 0;
    Matrix C, S;
    Eigen::Index k = 0;
    if (fromC) {
        if (start.C.rows() != n || start.C.cols() < 1)
            fail(QStringLiteral("initial concentrations: %1 rows needed").arg(n));
        C = start.C;
        k = C.cols();
        S = Matrix::Zero(k, p);
    } else {
        if (start.S.cols() != p || start.S.rows() < 1)
            fail(QStringLiteral("initial spectra: %1 points needed").arg(p));
        S = start.S;
        k = S.rows();
        C = Matrix::Zero(n, k);
    }
    if (!D.allFinite() || !C.allFinite() || !S.allFinite()) fail(QStringLiteral("the data contain invalid values"));
    std::vector<bool> fixed(size_t(k), false);
    for (int f : opt.fixedSpectra) {
        if (f < 0 || f >= k) fail(QStringLiteral("fixed spectrum index out of range"));
        if (fromC) fail(QStringLiteral("fixed spectra need initial spectra"));
        fixed[size_t(f)] = true;
    }
    std::vector<Eigen::Index> freeIdx, fixedIdx;
    for (Eigen::Index j = 0; j < k; ++j) (fixed[size_t(j)] ? fixedIdx : freeIdx).push_back(j);
    if (opt.closure && !(opt.closureTotal > 0)) fail(QStringLiteral("the closure total must be positive"));

    auto stepC = [&] {
        const Matrix G = S * S.transpose();
        const Matrix B = D * S.transpose();       // n × k
        if (opt.nonNegC) {
            for (Eigen::Index i = 0; i < n; ++i) C.row(i) = nnlsNormal(G, B.row(i).transpose()).transpose();
        } else {
            C = solveSmall(G, B.transpose()).transpose();
        }
        if (opt.unimodalC)
            for (Eigen::Index j = 0; j < k; ++j) makeUnimodal(C.col(j), std::max(1.0, opt.unimodalTolerance));
        if (opt.closure) {
            for (Eigen::Index i = 0; i < n; ++i) {
                const double s = C.row(i).sum();
                if (std::abs(s) > 1e-300) C.row(i) *= opt.closureTotal / s;
            }
        }
    };
    auto stepS = [&] {
        if (freeIdx.empty()) return;
        const auto q = Eigen::Index(freeIdx.size());
        Matrix Cf(n, q);
        for (Eigen::Index a = 0; a < q; ++a) Cf.col(a) = C.col(freeIdx[size_t(a)]);
        Matrix Dr = D;
        for (Eigen::Index j : fixedIdx) Dr.noalias() -= C.col(j) * S.row(j);
        const Matrix G = Cf.transpose() * Cf;
        const Matrix B = Cf.transpose() * Dr;     // q × p
        Matrix Sf(q, p);
        if (opt.nonNegS) {
            for (Eigen::Index j = 0; j < p; ++j) Sf.col(j) = nnlsNormal(G, B.col(j));
        } else {
            Sf = solveSmall(G, B);
        }
        for (Eigen::Index a = 0; a < q; ++a) {
            const Eigen::Index j = freeIdx[size_t(a)];
            S.row(j) = Sf.row(a);
            if (!opt.closure) {                   // scale to unit length; C carries the intensity
                const double nrm = S.row(j).norm();
                if (nrm > 0) {
                    S.row(j) /= nrm;
                    C.col(j) *= nrm;
                }
            }
        }
    };

    McrResult r;
    const double d2 = D.squaredNorm();
    if (!(d2 > 0)) fail(QStringLiteral("the data are all zero"));
    double prevSigma = kNaN, bestLof = kInf;
    int rising = 0;
    for (int it = 1; it <= std::max(1, opt.maxIterations); ++it) {
        if (stop && stop->load()) {
            r.stopped = true;
            break;
        }
        if (fromC) {
            stepS();
            stepC();
        } else {
            stepC();
            stepS();
        }
        const double sse = (D - C * S).squaredNorm();
        const double lof = 100.0 * std::sqrt(sse / d2);
        r.lofHistory.push_back(lof);
        r.iterations = it;
        if (lof < bestLof) {
            bestLof = lof;
            r.C = C;
            r.S = S;
            r.bestIteration = it;
        }
        if (progress) progress(it, lof);
        const double sigma = std::sqrt(sse / double(n * p));
        if (std::isfinite(prevSigma) && prevSigma > 0) {
            const double change = (prevSigma - sigma) / prevSigma;
            if (std::abs(change) * 100.0 < opt.tolerancePercent) {
                r.converged = true;
                break;
            }
            rising = change < 0 ? rising + 1 : 0;
            if (rising >= 20) break;              // diverging
        }
        prevSigma = sigma;
    }
    if (r.C.size() == 0) {                         // stopped before the first iteration
        r.C = C;
        r.S = S;
    }
    const double sse = (D - r.C * r.S).squaredNorm();
    r.lof = 100.0 * std::sqrt(sse / d2);
    r.r2 = 100.0 * (1.0 - sse / d2);
    if (r.stopped)
        r.message = QStringLiteral("stopped after %1 iterations").arg(r.iterations);
    else if (r.converged)
        r.message = QStringLiteral("converged after %1 iterations").arg(r.iterations);
    else if (r.iterations < opt.maxIterations)
        r.message = QStringLiteral("diverging after %1 iterations; best solution kept").arg(r.iterations);
    else
        r.message = QStringLiteral("not converged after %1 iterations").arg(r.iterations);
    return r;
}

// ============================================================================================ ambiguity
std::vector<AmbiguityBand> ambiguityBands(const McrResult& res, const McrOptions& opt, double tolerance,
                                          const std::atomic<bool>* stop) {
    std::vector<AmbiguityBand> bands;
    const Eigen::Index k = res.C.cols(), n = res.C.rows(), p = res.S.cols();
    if (k < 1 || res.S.rows() != k) return bands;
    // balanced factors: ||c0_j|| = ||s0_j||
    Matrix C0 = res.C, S0 = res.S;
    for (Eigen::Index j = 0; j < k; ++j) {
        const double cn = C0.col(j).norm(), sn = S0.row(j).norm();
        if (cn > 0 && sn > 0) {
            const double a = std::sqrt(sn / cn);
            C0.col(j) *= a;
            S0.row(j) /= a;
        }
    }
    const double total = (C0 * S0).norm();
    std::vector<bool> fixed(size_t(k), false);
    for (int f : opt.fixedSpectra)
        if (f >= 0 && f < k) fixed[size_t(f)] = true;
    std::vector<Vector> fixedShape(static_cast<size_t>(k));
    for (Eigen::Index j = 0; j < k; ++j)
        if (fixed[size_t(j)] && S0.row(j).norm() > 0) fixedShape[size_t(j)] = S0.row(j).transpose().normalized();

    auto contribution = [&](const Matrix& C, const Matrix& S, Eigen::Index j) {
        return total > 0 ? C.col(j).norm() * S.row(j).norm() / total : 0.0;
    };
    auto makeT = [&](const Vector& v) {
        Matrix T = Matrix::Identity(k, k);
        Eigen::Index t = 0;
        for (Eigen::Index a = 0; a < k; ++a)
            for (Eigen::Index b = 0; b < k; ++b)
                if (a != b) T(a, b) = v(t++);
        return T;
    };
    // relative squared violation of non-negativity (beyond tolerance × the largest |value|)
    auto violation = [&](const Matrix& C, const Matrix& S) {
        double v = 0;
        if (opt.nonNegC)
            for (Eigen::Index j = 0; j < k; ++j) {
                const double m = C.col(j).cwiseAbs().maxCoeff();
                if (m <= 0) continue;
                for (Eigen::Index i = 0; i < n; ++i) {
                    const double e = C(i, j) / m + tolerance;
                    if (e < 0) v += e * e;
                }
            }
        if (opt.nonNegS)
            for (Eigen::Index j = 0; j < k; ++j) {
                const double m = S.row(j).cwiseAbs().maxCoeff();
                if (m <= 0) continue;
                for (Eigen::Index i = 0; i < p; ++i) {
                    const double e = S(j, i) / m + tolerance;
                    if (e < 0) v += e * e;
                }
            }
        for (Eigen::Index j = 0; j < k; ++j)
            if (fixed[size_t(j)] && fixedShape[size_t(j)].size()) {
                const double nrm = S.row(j).norm();
                if (nrm <= 0) return kInf;
                const double d = (S.row(j).transpose() / nrm - fixedShape[size_t(j)]).squaredNorm();
                v += 1e3 * d;
            }
        return v;
    };

    const bool constrained = opt.nonNegC || opt.nonNegS || !opt.fixedSpectra.empty();
    const int dim = int(k * (k - 1));
    std::mt19937 rng(12345);
    std::normal_distribution<double> nd(0.0, 0.05);
    for (Eigen::Index comp = 0; comp < k; ++comp) {
        AmbiguityBand b;
        b.fOptimal = contribution(C0, S0, comp);
        auto profiles = [&](const Matrix& T, Vec& c, Vec& s) {
            const Matrix Ti = T.inverse();
            Vector cc = C0 * T.col(comp);
            Vector ss = (Ti.row(comp) * S0).transpose();
            const double nrm = ss.norm();
            if (nrm > 0) {
                ss /= nrm;
                cc *= nrm;
            }
            c = toVec(cc);
            s = toVec(ss);
        };
        if (!constrained || k == 1) {
            b.fMin = k == 1 ? b.fOptimal : 0.0;
            b.fMax = k == 1 ? b.fOptimal : kInf;
            if (k == 1) {
                profiles(Matrix::Identity(1, 1), b.cMin, b.sMin);
                b.cMax = b.cMin;
                b.sMax = b.sMin;
            }
            bands.push_back(b);
            continue;
        }
        for (int sense = 0; sense < 2; ++sense) {      // 0: minimise f, 1: maximise f
            const double sign = sense == 0 ? 1.0 : -1.0;
            Vector best = Vector::Zero(dim);
            double bestF = b.fOptimal;
            std::vector<Vector> starts{Vector::Zero(dim)};
            for (int s = 0; s < 2; ++s) {
                Vector v(dim);
                for (int i = 0; i < dim; ++i) v(i) = nd(rng);
                starts.push_back(v);
            }
            for (const Vector& x0 : starts) {
                Vector x = x0;
                for (double mu : {1e1, 1e3, 1e5}) {
                    auto obj = [&](const Vector& v) {
                        const Matrix T = makeT(v);
                        const double det = T.determinant();
                        if (!std::isfinite(det) || std::abs(det) < 1e-6) return 1e30;
                        const Matrix C = C0 * T;
                        const Matrix S = T.inverse() * S0;
                        const double viol = violation(C, S);
                        if (!std::isfinite(viol)) return 1e30;
                        return sign * contribution(C, S, comp) + mu * viol;
                    };
                    x = nelderMead(obj, x, mu < 1e2 ? 0.1 : 0.02, 150 * std::max(2, dim), stop);
                    if (stop && stop->load()) break;
                }
                const Matrix T = makeT(x);
                if (std::abs(T.determinant()) < 1e-6) continue;
                const Matrix C = C0 * T, S = T.inverse() * S0;
                // accept only (nearly) feasible solutions: violation of the order of the tolerance itself
                if (violation(C, S) > 1e-4) continue;
                const double f = contribution(C, S, comp);
                if (sign * f < sign * bestF) {
                    bestF = f;
                    best = x;
                }
            }
            if (sense == 0) {
                b.fMin = std::min(bestF, b.fOptimal);
                profiles(makeT(best), b.cMin, b.sMin);
            } else {
                b.fMax = std::max(bestF, b.fOptimal);
                profiles(makeT(best), b.cMax, b.sMax);
            }
        }
        bands.push_back(b);
        if (stop && stop->load()) break;
    }
    return bands;
}

// ============================================================================================ helpers
double correlation(const Vector& a, const Vector& b) {
    if (a.size() != b.size() || a.size() < 2) return 0.0;
    const Vector ac = a.array() - a.mean(), bc = b.array() - b.mean();
    const double den = ac.norm() * bc.norm();
    return den > 0 ? std::abs(ac.dot(bc)) / den : 0.0;
}

std::pair<std::vector<int>, Vec> matchComponents(const Matrix& truth, const Matrix& found) {
    const int k = int(truth.rows()), m = int(found.rows());
    Matrix corr(k, m);
    for (int i = 0; i < k; ++i)
        for (int j = 0; j < m; ++j) corr(i, j) = correlation(truth.row(i).transpose(), found.row(j).transpose());
    std::vector<int> perm(size_t(k), -1);
    if (k <= 8 && m >= k) {
        std::vector<int> idx(static_cast<size_t>(m));
        std::iota(idx.begin(), idx.end(), 0);
        double bestScore = -1;
        do {
            double s = 0;
            for (int i = 0; i < k; ++i) s += corr(i, idx[size_t(i)]);
            if (s > bestScore) {
                bestScore = s;
                perm.assign(idx.begin(), idx.begin() + k);
            }
        } while (std::next_permutation(idx.begin(), idx.end()));
    } else {
        std::vector<bool> used(size_t(m), false);
        for (int i = 0; i < k; ++i) {
            int bj = -1;
            for (int j = 0; j < m; ++j)
                if (!used[size_t(j)] && (bj < 0 || corr(i, j) > corr(i, bj))) bj = j;
            if (bj >= 0) used[size_t(bj)] = true;
            perm[size_t(i)] = bj;
        }
    }
    Vec c(size_t(k), 0.0);
    for (int i = 0; i < k; ++i)
        if (perm[size_t(i)] >= 0) c[size_t(i)] = corr(i, perm[size_t(i)]);
    return {perm, c};
}

Matrix toMatrix(const std::vector<Vec>& rows) {
    if (rows.empty()) return {};
    Matrix M(Eigen::Index(rows.size()), Eigen::Index(rows[0].size()));
    for (size_t i = 0; i < rows.size(); ++i) {
        if (rows[i].size() != rows[0].size()) fail(QStringLiteral("rows of different length"));
        for (size_t j = 0; j < rows[i].size(); ++j) M(Eigen::Index(i), Eigen::Index(j)) = rows[i][j];
    }
    return M;
}

Vec toVec(const Vector& v) { return Vec(v.data(), v.data() + v.size()); }

}  // namespace lr::chemo
