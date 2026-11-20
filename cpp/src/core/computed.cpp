// Computed Raman spectra: intensities, scaling, broadening, matching, comparison (see computed.h).
// The file readers are in computed_readers.cpp.
#include "computed.h"

#include <QFile>
#include <QFileInfo>
#include <QTextStream>

#include <algorithm>
#include <cmath>
#include <tuple>
#include <map>
#include <numeric>

#include "errors.h"
#include "processing.h"

namespace lr::computed {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kH = 6.62607015e-34;          // J s
constexpr double kC = 2.99792458e8;            // m/s
constexpr double kKb = 1.380649e-23;           // J/K
constexpr double kAmu = 1.66053906660e-27;     // kg

QString fmt(double v, int decimals) {
    if (!std::isfinite(v)) return {};
    QString t = QString::number(v, 'f', decimals);
    if (t.startsWith('-') && t.mid(1).toDouble() == 0.0) t.remove(0, 1);     // "-0.0"
    return t;
}

}  // namespace

// ------------------------------------------------------------------------------------ Calculation
QString Calculation::label() const {
    QStringList parts;
    parts << (version.isEmpty() ? program : program + " " + version);
    QString mb = method;
    if (!basis.isEmpty()) mb += (mb.isEmpty() ? "" : "/") + basis;
    if (!mb.isEmpty()) parts << mb;
    return parts.join(", ");
}

QString Calculation::name() const {
    QString n = formula.isEmpty() ? QFileInfo(source).completeBaseName() : formula;
    QString mb = method;
    if (!basis.isEmpty()) mb += (mb.isEmpty() ? "" : "/") + basis;
    if (!mb.isEmpty()) n += " " + mb;
    if (!source.isEmpty() && !formula.isEmpty()) n += " (" + QFileInfo(source).fileName() + ")";
    if (blocks > 1) n += QString(" #%1").arg(block + 1);
    return n.isEmpty() ? QStringLiteral("calculation") : n;
}

bool Calculation::hasActivities() const {
    return std::any_of(modes.begin(), modes.end(), [](const Mode& m) { return std::isfinite(m.activity); });
}

bool Calculation::hasIr() const {
    return std::any_of(modes.begin(), modes.end(), [](const Mode& m) { return std::isfinite(m.ir); });
}

int Calculation::imaginaryModes() const {
    return int(std::count_if(modes.begin(), modes.end(), [](const Mode& m) { return m.frequency < 0; }));
}

QString hillFormula(const QStringList& elements) {
    std::map<QString, int> count;
    for (const QString& e : elements)
        if (!e.isEmpty()) ++count[e];
    if (count.empty()) return {};
    QString out;
    auto put = [&](const QString& e) {
        auto it = count.find(e);
        if (it == count.end()) return;
        out += e + (it->second > 1 ? QString::number(it->second) : QString());
        count.erase(it);
    };
    if (count.count("C")) {
        put("C");
        put("H");
    }
    while (!count.empty()) put(count.begin()->first);
    return out;
}

// -------------------------------------------------------------------------------------- intensities
double Scaling::apply(double nu) const {
    if (!twoRegion()) return low * nu;
    return (nu < split ? low : high) * nu;
}

QString Scaling::text() const {
    if (!twoRegion()) return QString::number(low, 'f', 4);
    return QString("%1 (< %2 cm⁻¹), %3 (≥ %2 cm⁻¹)")
        .arg(QString::number(low, 'f', 4), QString::number(split, 'g', 6), QString::number(high, 'f', 4));
}

double ramanCrossSection(double nuCm, double activity, double laserNm, double temperatureK) {
    const double nu0Cm = 1e7 / laserNm;
    if (!(nuCm > 0) || !(nuCm < nu0Cm) || !std::isfinite(activity)) return 0.0;
    const double nu = nuCm * 100.0, nu0 = nu0Cm * 100.0;                    // m^-1
    const double s = activity * 1e-40 / kAmu;                                // Å^4/amu -> m^4/kg
    const double boltz = temperatureK > 0 ? 1.0 - std::exp(-kH * kC * nu / (kKb * temperatureK)) : 1.0;
    const double pre = std::pow(2 * kPi, 4) / 45.0 * kH / (8 * kPi * kPi * kC * nu);
    return pre * std::pow(nu0 - nu, 4) * s / boltz;                          // m²/sr
}

std::vector<Band> bands(const Calculation& calc, const Options& opt) {
    const bool unit = !calc.hasActivities();
    std::vector<int> idx;
    for (int i = 0; i < int(calc.modes.size()); ++i)
        if (calc.modes[i].frequency >= opt.minFrequency && calc.modes[i].frequency > 0) idx.push_back(i);
    std::sort(idx.begin(), idx.end(),
              [&](int a, int b) { return calc.modes[a].frequency < calc.modes[b].frequency; });
    std::vector<Band> out;
    for (int i : idx) {
        const Mode& m = calc.modes[i];
        const double act = unit ? 1.0 : (std::isfinite(m.activity) ? m.activity : 0.0);
        const double pos = opt.scaling.apply(m.frequency);
        // without Raman activities (positions only) every mode gets the same height
        const double inten = unit ? 1.0 : ramanCrossSection(pos, act, opt.laserNm, opt.temperatureK);
        if (!out.empty() && m.frequency - out.back().unscaled <= opt.mergeTolerance) {
            Band& b = out.back();                 // degenerate partner: position of the first mode
            b.activity += act;
            b.intensity += inten;
            if (std::isfinite(m.ir)) b.ir = (std::isfinite(b.ir) ? b.ir : 0.0) + m.ir;
            if (!m.symmetry.isEmpty() && !b.symmetry.split('+').contains(m.symmetry))
                b.symmetry += (b.symmetry.isEmpty() ? "" : "+") + m.symmetry;
            b.modes.push_back(i);
            continue;
        }
        Band b;
        b.position = pos;
        b.unscaled = m.frequency;
        b.activity = act;
        b.intensity = inten;
        b.ir = m.ir;
        b.symmetry = m.symmetry;
        b.modes = {i};
        out.push_back(b);
    }
    for (Band& b : out) b.value = opt.quantity == Quantity::Activity ? b.activity : b.intensity;
    return out;
}

double maxValue(const std::vector<Band>& b) {
    double m = 0;
    for (const Band& x : b) m = std::max(m, x.value);
    return m;
}

// ---------------------------------------------------------------------------------------- broadening
QString shapeName(Shape s) {
    switch (s) {
    case Shape::Lorentzian: return "Lorentzian";
    case Shape::Gaussian: return "Gaussian";
    case Shape::PseudoVoigt: return "pseudo-Voigt";
    }
    return {};
}

double unitProfile(double x, double x0, const Broadening& b) {
    const double w = b.fwhm > 0 ? b.fwhm : 1e-6;
    const double d = x - x0;
    const double lor = (w / 2) / (kPi * (d * d + w * w / 4));
    const double sigma = w / (2 * std::sqrt(2 * std::log(2.0)));
    const double gau = std::exp(-d * d / (2 * sigma * sigma)) / (sigma * std::sqrt(2 * kPi));
    switch (b.shape) {
    case Shape::Lorentzian: return lor;
    case Shape::Gaussian: return gau;
    case Shape::PseudoVoigt: return b.eta * lor + (1 - b.eta) * gau;
    }
    return lor;
}

Vec makeGrid(double lo, double hi, double step) {
    if (!(step > 0) || !(hi > lo)) throw Error("invalid grid (need lo < hi and step > 0)");
    const auto n = static_cast<std::size_t>(std::floor((hi - lo) / step + 1e-9)) + 1;
    if (n > 10000000) throw Error("grid too large");
    Vec g(n);
    for (std::size_t i = 0; i < n; ++i) g[i] = lo + double(i) * step;
    return g;
}

Vec broaden(const std::vector<Band>& b, const Vec& grid, const Broadening& br) {
    Vec y(grid.size(), 0.0);
    for (const Band& band : b) {
        if (!(band.value > 0)) continue;
        for (std::size_t i = 0; i < grid.size(); ++i) y[i] += band.value * unitProfile(grid[i], band.position, br);
    }
    return y;
}

std::pair<Vec, Vec> sticks(const std::vector<Band>& b, bool normalise) {
    const double m = normalise ? maxValue(b) : 1.0;
    Vec x, y;
    for (const Band& band : b) {
        x.push_back(band.position);
        y.push_back(m > 0 ? band.value / m : band.value);
    }
    return {x, y};
}

fileio::DataSpectrum spectrum(const Calculation& calc, const Options& opt, const Broadening& br, const Vec& grid,
                              bool normalise) {
    fileio::DataSpectrum d;
    d.name = calc.name() + " (computed)";
    d.source = calc.source;
    d.x = grid;
    d.y = broaden(bands(calc, opt), grid, br);
    if (normalise) {
        const double m = d.y.empty() ? 0.0 : *std::max_element(d.y.begin(), d.y.end());
        if (m > 0)
            for (double& v : d.y) v /= m;
    }
    d.meta = {{"sample", calc.name()},
              {"computed", "yes"},
              {"program", calc.version.isEmpty() ? calc.program : calc.program + " " + calc.version},
              {"method", calc.method},
              {"basis", calc.basis},
              {"formula", calc.formula},
              {"source", calc.source},
              {"quantity", opt.quantity == Quantity::Activity ? "Raman activity" : "Raman cross-section"},
              {"laser_nm", QString::number(opt.laserNm)},
              {"temperature_K", QString::number(opt.temperatureK)},
              {"scaling", opt.scaling.text()},
              {"profile", shapeName(br.shape)},
              {"fwhm_cm-1", QString::number(br.fwhm)},
              {"normalised", normalise ? "max = 1" : "no"}};
    return d;
}

// ------------------------------------------------------------------------------------------ matching
std::vector<int> hungarian(const std::vector<double>& cost, int n, int m) {
    if (n > m) throw Error("hungarian: more rows than columns");
    const double inf = std::numeric_limits<double>::infinity();
    std::vector<double> u(n + 1, 0.0), v(m + 1, 0.0);
    std::vector<int> p(m + 1, 0), way(m + 1, 0);
    for (int i = 1; i <= n; ++i) {
        p[0] = i;
        int j0 = 0;
        std::vector<double> minv(m + 1, inf);
        std::vector<char> used(m + 1, 0);
        do {
            used[j0] = 1;
            const int i0 = p[j0];
            double delta = inf;
            int j1 = 0;
            for (int j = 1; j <= m; ++j) {
                if (used[j]) continue;
                const double cur = cost[std::size_t(i0 - 1) * m + (j - 1)] - u[i0] - v[j];
                if (cur < minv[j]) {
                    minv[j] = cur;
                    way[j] = j0;
                }
                if (minv[j] < delta) {
                    delta = minv[j];
                    j1 = j;
                }
            }
            for (int j = 0; j <= m; ++j) {
                if (used[j]) {
                    u[p[j]] += delta;
                    v[j] -= delta;
                } else {
                    minv[j] -= delta;
                }
            }
            j0 = j1;
        } while (p[j0] != 0);
        do {
            const int j1 = way[j0];
            p[j0] = p[j1];
            j0 = j1;
        } while (j0);
    }
    std::vector<int> col(n, -1);
    for (int j = 1; j <= m; ++j)
        if (p[j]) col[p[j] - 1] = j - 1;
    return col;
}

std::vector<std::pair<int, int>> assign(const Vec& a, const Vec& b, double tolerance, Matching mode) {
    std::vector<std::pair<int, int>> out;
    const int n = int(a.size()), m = int(b.size());
    if (!n || !m) return out;
    if (mode == Matching::Nearest) {
        std::vector<std::tuple<double, int, int>> cand;
        for (int i = 0; i < n; ++i)
            for (int j = 0; j < m; ++j) {
                const double d = std::abs(a[i] - b[j]);
                if (d <= tolerance) cand.emplace_back(d, i, j);
            }
        std::sort(cand.begin(), cand.end());
        std::vector<char> ua(n, 0), ub(m, 0);
        for (const auto& [d, i, j] : cand) {
            if (ua[i] || ub[j]) continue;
            ua[i] = ub[j] = 1;
            out.emplace_back(i, j);
        }
    } else {
        // n rows; m real columns + n "unmatched" columns (row i may only use its own). Leaving a row
        // unmatched costs 2·tolerance (a band and a peak left over), so any pair within the tolerance is
        // preferred to no pair, and among the assignments the total |Δ| is minimal.
        const double big = 1e6 * (tolerance + 1.0);
        const int cols = m + n;
        std::vector<double> cost(std::size_t(n) * cols, big);
        for (int i = 0; i < n; ++i) {
            for (int j = 0; j < m; ++j) {
                const double d = std::abs(a[i] - b[j]);
                if (d <= tolerance) cost[std::size_t(i) * cols + j] = d;
            }
            cost[std::size_t(i) * cols + m + i] = 2 * tolerance;
        }
        const std::vector<int> col = hungarian(cost, n, cols);
        for (int i = 0; i < n; ++i)
            if (col[i] >= 0 && col[i] < m && std::abs(a[i] - b[col[i]]) <= tolerance) out.emplace_back(i, col[i]);
    }
    std::sort(out.begin(), out.end());
    return out;
}

std::vector<peaks::Peak> experimentalPeaks(const fileio::DataSpectrum& exp, double snr, bool subtractBaseline,
                                           double minRelative) {
    fileio::DataSpectrum d = exp;
    d.sorted();
    if (d.x.size() < 10) throw Error("experimental spectrum has too few points");
    Vec y = d.y;
    if (subtractBaseline) {
        const Vec base = processing::baseline(d.x, d.y, "als", 5.0);
        for (std::size_t i = 0; i < y.size(); ++i) y[i] -= base[i];
    }
    auto pk = peaks::pick(d.x, y, snr);
    double hmax = 0;
    for (const auto& p : pk) hmax = std::max(hmax, p.height);
    pk.erase(std::remove_if(pk.begin(), pk.end(), [&](const peaks::Peak& p) { return !(p.height >= minRelative * hmax); }),
             pk.end());
    return pk;
}

namespace {

// Factor for the band positions c matched to e (scan + least-squares refinement).
ScaleFit fitRegion(const Vec& c, const Vec& e, const FitOptions& o) {
    ScaleFit best;
    best.scaling.low = 1.0;
    if (c.empty() || e.empty()) return best;
    // coarse scan: most matches, then smallest total distance (cheap nearest matching)
    double f0 = std::clamp(1.0, o.lo, o.hi);
    int bestN = -1;
    double bestD = 0;
    for (double f = o.lo; f <= o.hi + 1e-12; f += 0.0025) {
        Vec cs(c.size());
        for (std::size_t i = 0; i < c.size(); ++i) cs[i] = f * c[i];
        const auto pr = assign(cs, e, o.tolerance, Matching::Nearest);
        double d = 0;
        for (const auto& [i, j] : pr) d += std::abs(cs[i] - e[j]);
        if (int(pr.size()) > bestN || (int(pr.size()) == bestN && d < bestD)) {
            bestN = int(pr.size());
            bestD = d;
            f0 = f;
        }
    }
    double f = f0;
    std::vector<std::pair<int, int>> pairs;
    for (int it = 0; it < 30; ++it) {
        Vec cs(c.size());
        for (std::size_t i = 0; i < c.size(); ++i) cs[i] = f * c[i];
        auto pr = assign(cs, e, o.tolerance, o.matching);
        if (pr.empty()) break;
        double num = 0, den = 0;
        for (const auto& [i, j] : pr) {
            num += c[i] * e[j];
            den += c[i] * c[i];
        }
        const double fn = std::clamp(num / den, o.lo, o.hi);
        const bool same = pr == pairs && std::abs(fn - f) < 1e-10;
        pairs = pr;
        f = fn;
        if (same) break;
    }
    best.scaling.low = f;
    best.pairs = pairs;
    best.matched = int(pairs.size());
    if (!pairs.empty()) {
        double ss = 0;
        for (const auto& [i, j] : pairs) ss += std::pow(f * c[i] - e[j], 2);
        best.rms = std::sqrt(ss / double(pairs.size()));
    }
    return best;
}

}  // namespace

ScaleFit fitScaling(const std::vector<Band>& unscaled, const Vec& expPositions, const FitOptions& o) {
    const double vmax = maxValue(unscaled);
    std::vector<int> use;
    for (int i = 0; i < int(unscaled.size()); ++i)
        if (unscaled[i].value >= o.minRelative * vmax && unscaled[i].unscaled > 0) use.push_back(i);
    std::vector<int> eIdx;
    for (int j = 0; j < int(expPositions.size()); ++j) {
        const double p = expPositions[j];
        if (std::isfinite(p) && (!o.range || (p >= o.range->first && p <= o.range->second))) eIdx.push_back(j);
    }
    std::sort(eIdx.begin(), eIdx.end(), [&](int a, int b) { return expPositions[a] < expPositions[b]; });
    Vec e;
    for (int j : eIdx) e.push_back(expPositions[j]);
    if (use.empty()) throw Error("no computed bands to fit");
    if (e.empty()) throw Error("no experimental peaks to fit the scaling factor to");

    auto run = [&](const std::vector<int>& sel) {
        Vec c;
        for (int i : sel) c.push_back(unscaled[i].unscaled);
        ScaleFit r = fitRegion(c, e, o);
        for (auto& pr : r.pairs) {
            pr.first = sel[pr.first];
            pr.second = eIdx[pr.second];
        }
        return r;
    };

    ScaleFit all = run(use);
    if (all.matched == 0) throw Error("no computed band lies within the tolerance of an experimental peak");
    if (!o.twoRegion) return all;

    std::vector<int> lo, hi;
    for (int i : use) (unscaled[i].unscaled < o.split ? lo : hi).push_back(i);
    ScaleFit a = lo.empty() ? all : run(lo), b = hi.empty() ? all : run(hi);
    if (a.matched == 0) a = all;
    if (b.matched == 0) b = all;
    ScaleFit out;
    out.scaling.low = a.scaling.low;
    out.scaling.high = b.scaling.low;
    out.scaling.split = o.split;
    if (!lo.empty()) out.pairs.insert(out.pairs.end(), a.pairs.begin(), a.pairs.end());
    if (!hi.empty()) out.pairs.insert(out.pairs.end(), b.pairs.begin(), b.pairs.end());
    // de-duplicate peaks claimed by both regions (keep the closer)
    std::sort(out.pairs.begin(), out.pairs.end());
    std::map<int, std::pair<int, double>> byPeak;
    for (const auto& [i, j] : out.pairs) {
        const double d = std::abs(out.scaling.apply(unscaled[i].unscaled) - expPositions[j]);
        auto it = byPeak.find(j);
        if (it == byPeak.end() || d < it->second.second) byPeak[j] = {i, d};
    }
    out.pairs.clear();
    double ss = 0;
    for (const auto& [j, v] : byPeak) {
        out.pairs.emplace_back(v.first, j);
        ss += v.second * v.second;
    }
    std::sort(out.pairs.begin(), out.pairs.end());
    out.matched = int(out.pairs.size());
    out.rms = out.matched ? std::sqrt(ss / out.matched) : kNaN;
    return out;
}

ScaleFit fitScaling(const Calculation& calc, const Options& opt, const fileio::DataSpectrum& exp, const FitOptions& o,
                    double snr) {
    Options unit = opt;
    unit.scaling = Scaling{};
    const auto pk = experimentalPeaks(exp, snr);
    Vec pos;
    for (const auto& p : pk) pos.push_back(p.position);
    return fitScaling(bands(calc, unit), pos, o);
}

// ---------------------------------------------------------------------------------------- assignment
std::vector<AssignmentRow> assignmentTable(const std::vector<Band>& scaled, const std::vector<peaks::Peak>& exp,
                                           double tolerance, Matching m, double minRelative) {
    const double vmax = maxValue(scaled);
    double hmax = 0;
    for (const auto& p : exp) hmax = std::max(hmax, p.height);
    std::vector<int> use;
    Vec c, e;
    for (int i = 0; i < int(scaled.size()); ++i)
        if (scaled[i].value >= minRelative * vmax) {
            use.push_back(i);
            c.push_back(scaled[i].position);
        }
    for (const auto& p : exp) e.push_back(p.position);
    const auto pairs = assign(c, e, tolerance, m);

    std::vector<AssignmentRow> rows;
    std::vector<char> usedB(use.size(), 0), usedP(exp.size(), 0);
    auto bandRow = [&](AssignmentRow& r, int k) {
        const Band& b = scaled[use[k]];
        r.band = use[k];
        r.computed = b.position;
        r.unscaled = b.unscaled;
        r.computedRel = vmax > 0 ? b.value / vmax : kNaN;
        r.symmetry = b.symmetry;
    };
    auto peakRow = [&](AssignmentRow& r, int j) {
        r.peak = j;
        r.experimental = exp[j].position;
        r.experimentalRel = hmax > 0 ? exp[j].height / hmax : kNaN;
    };
    for (const auto& [k, j] : pairs) {
        AssignmentRow r;
        bandRow(r, k);
        peakRow(r, j);
        r.delta = r.computed - r.experimental;
        usedB[k] = usedP[j] = 1;
        rows.push_back(r);
    }
    for (int k = 0; k < int(use.size()); ++k)
        if (!usedB[k]) {
            AssignmentRow r;
            bandRow(r, k);
            rows.push_back(r);
        }
    for (int j = 0; j < int(exp.size()); ++j)
        if (!usedP[j]) {
            AssignmentRow r;
            peakRow(r, j);
            rows.push_back(r);
        }
    auto key = [](const AssignmentRow& r) { return std::isfinite(r.experimental) ? r.experimental : r.computed; };
    std::stable_sort(rows.begin(), rows.end(), [&](const AssignmentRow& a, const AssignmentRow& b) { return key(a) < key(b); });
    return rows;
}

namespace {
const QStringList kAssignHeader = {"computed_cm-1", "unscaled_cm-1", "symmetry", "computed_rel",
                                   "experimental_cm-1", "experimental_rel", "delta_cm-1"};

QStringList rowCells(const AssignmentRow& r) {
    return {fmt(r.computed, 1),     fmt(r.unscaled, 1),        r.symmetry, fmt(r.computedRel, 3),
            fmt(r.experimental, 1), fmt(r.experimentalRel, 3), fmt(r.delta, 1)};
}
}  // namespace

void writeAssignmentCsv(const QString& path, const std::vector<AssignmentRow>& rows) {
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) throw Error(("cannot write " + path).toStdString());
    QTextStream out(&f);
    out << kAssignHeader.join(',') << '\n';
    for (const auto& r : rows) out << rowCells(r).join(',') << '\n';
}

QString assignmentText(const std::vector<AssignmentRow>& rows) {
    std::vector<QStringList> cells{kAssignHeader};
    for (const auto& r : rows) cells.push_back(rowCells(r));
    std::vector<int> w(kAssignHeader.size(), 0);
    for (const auto& c : cells)
        for (int i = 0; i < c.size(); ++i) w[i] = std::max(w[i], int(c[i].size()));
    QString out;
    for (const auto& c : cells) {
        QStringList line;
        for (int i = 0; i < c.size(); ++i) line << c[i].rightJustified(w[i]);
        out += line.join("  ") + '\n';
    }
    return out;
}

// ---------------------------------------------------------------------------------------- comparison
double correlation(const Vec& ax, const Vec& ay, const Vec& bx, const Vec& by, std::optional<std::pair<double, double>> range) {
    // b sorted by x for interpolation
    std::vector<std::size_t> ord(std::min(bx.size(), by.size()));
    std::iota(ord.begin(), ord.end(), std::size_t(0));
    std::sort(ord.begin(), ord.end(), [&](std::size_t i, std::size_t j) { return bx[i] < bx[j]; });
    Vec sx, sy;
    for (std::size_t i : ord) {
        sx.push_back(bx[i]);
        sy.push_back(by[i]);
    }
    if (sx.size() < 2) return kNaN;
    Vec u, v;
    for (std::size_t i = 0; i < std::min(ax.size(), ay.size()); ++i) {
        const double x = ax[i];
        if (x < sx.front() || x > sx.back()) continue;
        if (range && (x < range->first || x > range->second)) continue;
        const auto it = std::lower_bound(sx.begin(), sx.end(), x);
        std::size_t k = std::size_t(it - sx.begin());
        double yb;
        if (k == 0) yb = sy[0];
        else {
            const double t = (x - sx[k - 1]) / (sx[k] - sx[k - 1]);
            yb = sy[k - 1] + t * (sy[k] - sy[k - 1]);
        }
        u.push_back(ay[i]);
        v.push_back(yb);
    }
    if (u.size() < 3) return kNaN;
    const double mu = std::accumulate(u.begin(), u.end(), 0.0) / double(u.size());
    const double mv = std::accumulate(v.begin(), v.end(), 0.0) / double(v.size());
    double suv = 0, suu = 0, svv = 0;
    for (std::size_t i = 0; i < u.size(); ++i) {
        suv += (u[i] - mu) * (v[i] - mv);
        suu += (u[i] - mu) * (u[i] - mu);
        svv += (v[i] - mv) * (v[i] - mv);
    }
    return suu > 0 && svv > 0 ? suv / std::sqrt(suu * svv) : kNaN;
}

std::vector<StructureScore> compare(const std::vector<Calculation>& calcs, const Options& opt, const Broadening& br,
                                    const fileio::DataSpectrum& exp, bool fitScale, const FitOptions& fo) {
    fileio::DataSpectrum e = exp;
    e.sorted();
    if (e.x.size() < 10) throw Error("experimental spectrum has too few points");
    const Vec base = processing::baseline(e.x, e.y, "als", 5.0);
    Vec ey = e.y;
    for (std::size_t i = 0; i < ey.size(); ++i) ey[i] -= base[i];
    Vec pos;
    for (const auto& p : experimentalPeaks(e, 8.0, true)) pos.push_back(p.position);

    std::vector<StructureScore> out;
    for (int k = 0; k < int(calcs.size()); ++k) {
        StructureScore s;
        s.index = k;
        Options o = opt;
        if (fitScale) {
            try {
                Options unit = opt;
                unit.scaling = Scaling{};
                const ScaleFit f = fitScaling(bands(calcs[k], unit), pos, fo);
                o.scaling = f.scaling;
                s.rms = f.rms;
                s.matched = f.matched;
            } catch (const Error&) {
                // no match: keep the given scaling
            }
        }
        s.scaling = o.scaling;
        const Vec y = broaden(bands(calcs[k], o), e.x, br);
        s.correlation = correlation(e.x, ey, e.x, y, fo.range);
        out.push_back(s);
    }
    std::stable_sort(out.begin(), out.end(), [](const StructureScore& a, const StructureScore& b) {
        const double x = std::isfinite(a.correlation) ? a.correlation : -2, y = std::isfinite(b.correlation) ? b.correlation : -2;
        return x > y;
    });
    return out;
}

}  // namespace lr::computed
