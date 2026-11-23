#include "match.h"

#include <algorithm>
#include <cmath>
#include <functional>
#include <map>
#include <numeric>
#include <set>

#include "errors.h"
#include "peaks.h"
#include "processing.h"

namespace lr::match {

using library::Entry;
using library::Filter;
using library::PeakEntry;

namespace {

[[noreturn]] void fail(const QString& msg) { throw Error(msg.toStdString()); }

void sortedFinite(const Vec& x0, const Vec& y0, Vec& x, Vec& y) {
    const size_t n = std::min(x0.size(), y0.size());
    std::vector<size_t> idx;
    idx.reserve(n);
    for (size_t i = 0; i < n; ++i)
        if (std::isfinite(x0[i]) && std::isfinite(y0[i])) idx.push_back(i);
    std::stable_sort(idx.begin(), idx.end(), [&](size_t a, size_t b) { return x0[a] < x0[b]; });
    x.resize(idx.size());
    y.resize(idx.size());
    for (size_t i = 0; i < idx.size(); ++i) {
        x[i] = x0[idx[i]];
        y[i] = y0[idx[i]];
    }
}

double weightOf(const PeakEntry& p) {
    return p.intensity > 0 ? 0.25 + 0.75 * std::min(p.intensity, 1.0) : 1.0;
}

bool containsCi(const QString& hay, const QString& needle) { return hay.contains(needle, Qt::CaseInsensitive); }

}  // namespace

Vec Prepared::xs() const {
    Vec v(y.size());
    for (size_t i = 0; i < v.size(); ++i) v[i] = x(i);
    return v;
}

Prepared prepare(const Vec& x0, const Vec& y0, const Preprocess& p) {
    Prepared out;
    out.step = p.step > 0 ? p.step : 2.0;
    Vec x, y;
    sortedFinite(x0, y0, x, y);
    if (x.size() < 2) return out;
    const double lo = std::max(p.xmin, x.front()), hi = std::min(p.xmax, x.back());
    if (!(hi > lo)) return out;
    const long k0 = long(std::ceil(lo / out.step - 1e-9)), k1 = long(std::floor(hi / out.step + 1e-9));
    if (k1 - k0 + 1 < 16) return out;
    const size_t n = size_t(k1 - k0 + 1);
    Vec xg(n), yg(n);
    size_t j = 0;
    for (size_t i = 0; i < n; ++i) {
        const double xv = double(k0 + long(i)) * out.step;
        xg[i] = xv;
        while (j + 2 < x.size() && x[j + 1] < xv) ++j;
        const double dx = x[j + 1] - x[j];
        const double t = dx > 0 ? std::clamp((xv - x[j]) / dx, 0.0, 1.0) : 0.0;
        yg[i] = y[j] + t * (y[j + 1] - y[j]);
    }
    if (!p.baseline.isEmpty() && p.baseline != "none") {
        const Vec b = processing::baseline(xg, yg, p.baseline, p.strength);
        for (size_t i = 0; i < n; ++i) yg[i] -= b[i];
    }
    if (p.smoothWindow >= 5 && size_t(p.smoothWindow) < n) {
        const int w = p.smoothWindow | 1;
        yg = sig::savgolFilter(yg, w, 3);
    }
    if (p.derivative > 0) {
        const int w = std::max(p.derivWindow | 1, 5);
        if (size_t(w) < n) yg = sig::savgolFilter(yg, w, 3, std::min(p.derivative, 2));
    }
    out.k0 = k0;
    out.y = std::move(yg);
    return out;
}

double hqi(const Vec& a, const Vec& b) {
    const size_t n = std::min(a.size(), b.size());
    if (n < 3) return 0.0;
    double ma = 0, mb = 0;
    for (size_t i = 0; i < n; ++i) {
        ma += a[i];
        mb += b[i];
    }
    ma /= double(n);
    mb /= double(n);
    double sab = 0, saa = 0, sbb = 0;
    for (size_t i = 0; i < n; ++i) {
        const double da = a[i] - ma, db = b[i] - mb;
        sab += da * db;
        saa += da * da;
        sbb += db * db;
    }
    if (saa <= 0 || sbb <= 0) return 0.0;
    const double r = sab / std::sqrt(saa * sbb);
    return r > 0 ? r * r : 0.0;
}

double hqi(const Prepared& a, const Prepared& b, double* lo, double* hi) {
    if (lo) *lo = 0;
    if (hi) *hi = 0;
    if (a.empty() || b.empty() || std::abs(a.step - b.step) > 1e-12) return 0.0;
    const long s = std::max(a.k0, b.k0);
    const long e = std::min(a.k0 + long(a.y.size()), b.k0 + long(b.y.size()));
    if (e - s < 3) return 0.0;
    if (lo) *lo = double(s) * a.step;
    if (hi) *hi = double(e - 1) * a.step;
    const Vec va(a.y.begin() + (s - a.k0), a.y.begin() + (e - a.k0));
    const Vec vb(b.y.begin() + (s - b.k0), b.y.begin() + (e - b.k0));
    return hqi(va, vb);
}

std::vector<PeakEntry> pickPeaks(const Vec& x0, const Vec& y0, double snr, int maxPeaks) {
    Vec x, y;
    sortedFinite(x0, y0, x, y);
    if (x.size() < 16) return {};
    const Vec b = processing::baseline(x, y, "snip4", 5.0);
    for (size_t i = 0; i < y.size(); ++i) y[i] -= b[i];
    std::vector<peaks::Peak> pk = peaks::pick(x, y, snr, 2.0, false, 50.0);   // not the laser line/notch edge
    std::sort(pk.begin(), pk.end(), [](const peaks::Peak& a, const peaks::Peak& c) { return a.height > c.height; });
    if (maxPeaks > 0 && int(pk.size()) > maxPeaks) pk.resize(size_t(maxPeaks));
    const double hmax = pk.empty() ? 1.0 : std::max(pk.front().height, 1e-300);
    std::vector<PeakEntry> out;
    for (const auto& p : pk) out.push_back({p.position, p.height / hmax, p.fwhm, {}});
    std::sort(out.begin(), out.end(), [](const PeakEntry& a, const PeakEntry& c) { return a.position < c.position; });
    return out;
}

PeakScore peakScore(const std::vector<PeakEntry>& query0, const std::vector<PeakEntry>& ref0, double tol, double qlo,
                    double qhi, double rlo, double rhi) {
    PeakScore s;
    std::vector<PeakEntry> q, r;
    for (const auto& p : query0)
        if (p.position >= rlo - tol && p.position <= rhi + tol) q.push_back(p);
    for (const auto& p : ref0)
        if (p.position >= qlo - tol && p.position <= qhi + tol) r.push_back(p);
    s.queryPeaks = int(q.size());
    s.libraryPeaks = int(r.size());
    if (q.empty() || r.empty() || !(tol > 0)) return s;
    struct Pair {
        double d;
        size_t i, j;
    };
    std::vector<Pair> pairs;
    for (size_t i = 0; i < q.size(); ++i)
        for (size_t j = 0; j < r.size(); ++j) {
            const double d = std::abs(q[i].position - r[j].position);
            if (d <= tol) pairs.push_back({d, i, j});
        }
    std::sort(pairs.begin(), pairs.end(), [](const Pair& a, const Pair& b) {
        if (a.d != b.d) return a.d < b.d;
        return a.i != b.i ? a.i < b.i : a.j < b.j;
    });
    std::vector<bool> usedQ(q.size()), usedR(r.size());
    double wq = 0, wr = 0, mq = 0, mr = 0;
    for (const auto& p : q) wq += weightOf(p);
    for (const auto& p : r) wr += weightOf(p);
    for (const Pair& p : pairs) {
        if (usedQ[p.i] || usedR[p.j]) continue;
        usedQ[p.i] = usedR[p.j] = true;
        const double c = 1.0 - 0.5 * p.d / tol;
        mq += c * weightOf(q[p.i]);
        mr += c * weightOf(r[p.j]);
        ++s.matched;
    }
    s.score = (wq > 0 && wr > 0) ? std::sqrt((mq / wq) * (mr / wr)) : 0.0;
    return s;
}

// Lawson & Hanson (1974), "Solving Least Squares Problems", algorithm NNLS.
Eigen::VectorXd nnls(const Eigen::MatrixXd& A, const Eigen::VectorXd& b, int maxIter) {
    const Eigen::Index n = A.cols();
    Eigen::VectorXd x = Eigen::VectorXd::Zero(n);
    if (n == 0 || A.rows() == 0) return x;
    if (maxIter <= 0) maxIter = int(3 * n + 30);
    const double tol = 10 * std::numeric_limits<double>::epsilon() * A.cwiseAbs().colwise().sum().maxCoeff() *
                       double(std::max(A.rows(), n));
    std::vector<bool> passive(size_t(n), false);
    Eigen::VectorXd w = A.transpose() * (b - A * x);
    int iter = 0;
    auto solvePassive = [&](Eigen::VectorXd& z) {
        std::vector<Eigen::Index> P;
        for (Eigen::Index j = 0; j < n; ++j)
            if (passive[size_t(j)]) P.push_back(j);
        Eigen::MatrixXd Ap(A.rows(), Eigen::Index(P.size()));
        for (size_t k = 0; k < P.size(); ++k) Ap.col(Eigen::Index(k)) = A.col(P[k]);
        const Eigen::VectorXd zp = Ap.colPivHouseholderQr().solve(b);
        z.setZero(n);
        for (size_t k = 0; k < P.size(); ++k) z(P[k]) = zp(Eigen::Index(k));
    };
    while (iter < maxIter) {
        Eigen::Index jmax = -1;
        double wmax = tol;
        for (Eigen::Index j = 0; j < n; ++j)
            if (!passive[size_t(j)] && w(j) > wmax) {
                wmax = w(j);
                jmax = j;
            }
        if (jmax < 0) break;
        passive[size_t(jmax)] = true;
        Eigen::VectorXd z;
        solvePassive(z);
        while (iter++ < maxIter) {
            bool allPositive = true;
            for (Eigen::Index j = 0; j < n; ++j)
                if (passive[size_t(j)] && z(j) <= 0) allPositive = false;
            if (allPositive) break;
            double alpha = std::numeric_limits<double>::infinity();
            for (Eigen::Index j = 0; j < n; ++j)
                if (passive[size_t(j)] && z(j) <= 0) alpha = std::min(alpha, x(j) / (x(j) - z(j)));
            x += alpha * (z - x);
            for (Eigen::Index j = 0; j < n; ++j)
                if (passive[size_t(j)] && std::abs(x(j)) <= tol) {
                    passive[size_t(j)] = false;
                    x(j) = 0;
                }
            solvePassive(z);
        }
        x = z;
        w = A.transpose() * (b - A * x);
    }
    for (Eigen::Index j = 0; j < n; ++j)
        if (x(j) < 0) x(j) = 0;
    return x;
}

// ------------------------------------------------------------------------------------------------ Matcher
void Matcher::setPreprocess(const Preprocess& p) {
    pre_ = p;
    for (Ref& r : refs_) r.prep = r.x.empty() ? Prepared{} : prepare(r.x, r.y, pre_);
}

void Matcher::load(const library::Library& lib, const Filter& f) {
    refs_.clear();
    for (const Entry& e : lib.find(f, true)) add(e);
}

void Matcher::add(const Entry& e) {
    Ref r;
    r.meta = e;
    r.meta.spectra.clear();
    r.meta.peaks.clear();
    if (const library::SpectrumData* s = e.preferredSpectrum()) {
        sortedFinite(s->x, s->y, r.x, r.y);
        if (!r.x.empty()) {
            r.prep = prepare(r.x, r.y, pre_);
            r.lo = r.x.front();
            r.hi = r.x.back();
        }
    }
    r.peaks = !e.peaks.empty() ? e.peaks : (r.x.empty() ? std::vector<PeakEntry>{} : pickPeaks(r.x, r.y));
    for (Ref& old : refs_)
        if (e.id != 0 && old.meta.id == e.id) {
            old = std::move(r);
            return;
        }
    refs_.push_back(std::move(r));
}

const Matcher::Ref* Matcher::find(qint64 id) const {
    for (const Ref& r : refs_)
        if (r.meta.id == id) return &r;
    return nullptr;
}

const Prepared* Matcher::prepared(qint64 id) const {
    const Ref* r = find(id);
    return r && !r->prep.empty() ? &r->prep : nullptr;
}

bool Matcher::accepts(const Filter& f, const Entry& e) {
    const QString t = f.text.trimmed();
    if (!t.isEmpty() && !containsCi(e.name, t) && !containsCi(e.formula, t) && !containsCi(e.cas, t) &&
        !containsCi(e.cls, t) && !containsCi(e.tags.join(','), t) && !containsCi(e.notes, t) &&
        !containsCi(e.sourceId, t))
        return false;
    if (!f.cls.trimmed().isEmpty() && e.cls.compare(f.cls.trimmed(), Qt::CaseInsensitive) != 0) return false;
    if (f.source && e.source != *f.source) return false;
    if (f.laserNm > 0) {
        if (e.laserNm <= 0) return f.includeUnknownLaser;
        if (std::abs(e.laserNm - f.laserNm) > f.laserTol) return false;
    }
    return true;
}

Hit Matcher::hitOf(const Ref& r) const {
    Hit h;
    h.id = r.meta.id;
    h.name = r.meta.name;
    h.formula = r.meta.formula;
    h.cls = r.meta.cls;
    h.licence = r.meta.licence;
    h.source = r.meta.source;
    h.laserNm = r.meta.laserNm;
    return h;
}

std::vector<Hit> Matcher::search(const Vec& x, const Vec& y, int topN, const Filter& f) const {
    return search(prepare(x, y, pre_), topN, f);
}

std::vector<Hit> Matcher::search(const Prepared& q, int topN, const Filter& f) const {
    std::vector<Hit> hits;
    if (q.empty()) return hits;
    for (const Ref& r : refs_) {
        if (r.prep.empty() || !accepts(f, r.meta)) continue;
        double lo = 0, hi = 0;
        const double s = hqi(q, r.prep, &lo, &hi);
        if (hi - lo < minOverlap) continue;
        Hit h = hitOf(r);
        h.score = s;
        h.lo = lo;
        h.hi = hi;
        hits.push_back(std::move(h));
    }
    std::stable_sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) { return a.score > b.score; });
    if (topN > 0 && int(hits.size()) > topN) hits.resize(size_t(topN));
    return hits;
}

std::vector<Hit> Matcher::searchPeaks(const Vec& x0, const Vec& y0, const PeakOptions& o, int topN,
                                      const Filter& f) const {
    Vec x, y;
    sortedFinite(x0, y0, x, y);
    if (x.size() < 16) return {};
    return searchPeaks(pickPeaks(x, y, o.snr, o.maxPeaks), o, topN, f, x.front(), x.back());
}

std::vector<Hit> Matcher::searchPeaks(const std::vector<PeakEntry>& query, const PeakOptions& o, int topN,
                                      const Filter& f, double qlo, double qhi) const {
    std::vector<Hit> hits;
    if (query.empty()) return hits;
    for (const Ref& r : refs_) {
        if (r.peaks.empty() || !accepts(f, r.meta)) continue;
        const PeakScore ps = peakScore(query, r.peaks, o.tolerance, qlo, qhi, r.lo, r.hi);
        Hit h = hitOf(r);
        h.score = ps.score;
        h.matched = ps.matched;
        h.libraryPeaks = ps.libraryPeaks;
        h.queryPeaks = ps.queryPeaks;
        h.lo = std::max(qlo, r.lo);
        h.hi = std::min(qhi, r.hi);
        if (!std::isfinite(h.lo) || !std::isfinite(h.hi)) {
            double a = std::numeric_limits<double>::infinity(), b = -a;
            for (const auto& p : query) {
                a = std::min(a, p.position);
                b = std::max(b, p.position);
            }
            if (!std::isfinite(h.lo)) h.lo = a;
            if (!std::isfinite(h.hi)) h.hi = b;
        }
        hits.push_back(std::move(h));
    }
    std::stable_sort(hits.begin(), hits.end(), [](const Hit& a, const Hit& b) {
        if (a.score != b.score) return a.score > b.score;
        return a.matched > b.matched;
    });
    if (topN > 0 && int(hits.size()) > topN) hits.resize(size_t(topN));
    return hits;
}

Mixture Matcher::fit(const Prepared& q, const std::vector<const Ref*>& refs) const {
    if (q.empty()) fail(QStringLiteral("the query spectrum has no usable range"));
    long s = q.k0, e = q.k0 + long(q.y.size());
    for (const Ref* r : refs) {
        if (r->prep.empty()) fail(QStringLiteral("library entry '%1' has no spectrum").arg(r->meta.name));
        s = std::max(s, r->prep.k0);
        e = std::min(e, r->prep.k0 + long(r->prep.y.size()));
    }
    if (e - s < 16 || double(e - 1 - s) * q.step < minOverlap)
        fail(QStringLiteral("the spectra have no common range of at least %1 cm-1").arg(minOverlap));
    const Eigen::Index n = e - s, k = Eigen::Index(refs.size());
    Eigen::VectorXd b(n);
    for (Eigen::Index i = 0; i < n; ++i) b(i) = q.y[size_t(s - q.k0 + i)];
    const double bn = b.norm();
    if (bn > 0) b /= bn;
    Eigen::MatrixXd A(n, k);
    for (Eigen::Index j = 0; j < k; ++j) {
        const Prepared& p = refs[size_t(j)]->prep;
        for (Eigen::Index i = 0; i < n; ++i) A(i, j) = p.y[size_t(s - p.k0 + i)];
        const double cn = A.col(j).norm();
        if (cn > 0) A.col(j) /= cn;
    }
    const Eigen::VectorXd c = nnls(A, b);
    const Eigen::VectorXd fitv = A * c;
    Mixture m;
    const double sum = c.sum();
    for (Eigen::Index j = 0; j < k; ++j)
        m.components.push_back({refs[size_t(j)]->meta.id, refs[size_t(j)]->meta.name, c(j), sum > 0 ? c(j) / sum : 0.0});
    m.x.resize(size_t(n));
    m.query.resize(size_t(n));
    m.fit.resize(size_t(n));
    for (Eigen::Index i = 0; i < n; ++i) {
        m.x[size_t(i)] = double(s + i) * q.step;
        m.query[size_t(i)] = b(i);
        m.fit[size_t(i)] = fitv(i);
    }
    for (Eigen::Index j = 0; j < k; ++j) {
        Vec part(static_cast<size_t>(n));
        for (Eigen::Index i = 0; i < n; ++i) part[size_t(i)] = c(j) * A(i, j);
        m.parts.push_back(std::move(part));
    }
    m.hqi = hqi(m.query, m.fit);
    m.residual = (b - fitv).norm();
    // sort components (and parts) by fraction
    std::vector<size_t> idx(m.components.size());
    std::iota(idx.begin(), idx.end(), size_t(0));
    std::stable_sort(idx.begin(), idx.end(),
                     [&](size_t a, size_t bb) { return m.components[a].fraction > m.components[bb].fraction; });
    std::vector<MixtureComponent> comps;
    std::vector<Vec> parts;
    for (size_t i : idx) {
        comps.push_back(m.components[i]);
        parts.push_back(std::move(m.parts[i]));
    }
    m.components = std::move(comps);
    m.parts = std::move(parts);
    m.score = m.hqi - 0.005 * double(std::max<size_t>(m.components.size(), 1) - 1);
    return m;
}

Mixture Matcher::fitMixture(const Vec& x, const Vec& y, const std::vector<qint64>& ids) const {
    std::vector<const Ref*> refs;
    for (qint64 id : ids) {
        const Ref* r = find(id);
        if (!r) fail(QStringLiteral("library entry %1 is not loaded").arg(id));
        refs.push_back(r);
    }
    if (refs.empty()) fail(QStringLiteral("no components given"));
    return fit(prepare(x, y, pre_), refs);
}

std::vector<Mixture> Matcher::searchMixtures(const Vec& x, const Vec& y, int candidates, int maxComponents, int topN,
                                             const Filter& f) const {
    const Prepared q = prepare(x, y, pre_);
    const std::vector<Hit> hits = search(q, std::max(candidates, 1), f);
    std::vector<const Ref*> cand;
    for (const Hit& h : hits)
        if (const Ref* r = find(h.id)) cand.push_back(r);
    std::vector<Mixture> out;
    std::set<std::vector<qint64>> seen;
    const int kmax = std::clamp(maxComponents, 1, 3);
    std::vector<size_t> sel;
    std::function<void(size_t, int)> rec = [&](size_t start, int k) {
        if (int(sel.size()) == k) {
            std::vector<const Ref*> refs;
            for (size_t i : sel) refs.push_back(cand[i]);
            try {
                Mixture m = fit(q, refs);
                // drop components that contribute (almost) nothing and refit with the rest
                std::vector<const Ref*> keep;
                for (const auto& c : m.components)
                    if (c.fraction >= 0.01)
                        for (const Ref* r : refs)
                            if (r->meta.id == c.id) keep.push_back(r);
                if (keep.empty()) return;
                if (keep.size() != refs.size()) m = fit(q, keep);
                std::vector<qint64> key;
                for (const auto& c : m.components) key.push_back(c.id);
                std::sort(key.begin(), key.end());
                if (!seen.insert(key).second) return;
                out.push_back(std::move(m));
            } catch (const Error&) {
            }
            return;
        }
        for (size_t i = start; i < cand.size(); ++i) {
            sel.push_back(i);
            rec(i + 1, k);
            sel.pop_back();
        }
    };
    for (int k = 1; k <= kmax; ++k) rec(0, k);
    std::stable_sort(out.begin(), out.end(), [](const Mixture& a, const Mixture& b) { return a.score > b.score; });
    if (topN > 0 && int(out.size()) > topN) out.resize(size_t(topN));
    return out;
}

}  // namespace lr::match
