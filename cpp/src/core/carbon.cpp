#include "carbon.h"

#include <algorithm>
#include <cmath>
#include <limits>

#include "errors.h"

namespace lr::carbon {

namespace {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

// Position and height (above the window minimum) of the largest point of y in [lo, hi] (x ascending).
std::pair<double, double> localMax(const Vec& x, const Vec& y, double lo, double hi, double floor) {
    double bx = (lo + hi) / 2, by = -std::numeric_limits<double>::infinity();
    for (size_t i = 0; i < x.size() && i < y.size(); ++i)
        if (x[i] >= lo && x[i] <= hi && y[i] > by) { bx = x[i]; by = y[i]; }
    if (!std::isfinite(by)) return {bx, 1.0};
    return {bx, std::max(by - floor, 1e-6)};
}

double windowMin(const Vec& x, const Vec& y, double lo, double hi) {
    double m = std::numeric_limits<double>::infinity();
    for (size_t i = 0; i < x.size() && i < y.size(); ++i)
        if (x[i] >= lo && x[i] <= hi) m = std::min(m, y[i]);
    return std::isfinite(m) ? m : 0.0;
}

double relErr(double v, double e) { return v != 0 ? e / v : 0.0; }

double ratioErr(double r, double a, double ea, double b, double eb) {
    return std::abs(r) * std::hypot(relErr(a, ea), relErr(b, eb));
}

Band bandFrom(const QString& name, const peaks::FitPeak& p) {
    Band b;
    b.name = name;
    b.shape = p.shape;
    b.position = p.position;
    b.positionErr = p.positionErr;
    b.height = p.height;
    b.heightErr = p.heightErr;
    b.fwhm = p.fwhm;
    b.fwhmErr = p.fwhmErr;
    b.area = p.area;
    b.areaErr = std::abs(p.area) * std::hypot(relErr(p.height, p.heightErr), relErr(p.fwhm, p.fwhmErr));
    b.center = p.center;
    b.q = p.q;
    b.qErr = p.qErr;
    return b;
}

peaks::Component comp(const QString& shape, double pos, double fwhm, double posLo, double posHi, double fwhmLo,
                      double fwhmHi, double height = kNaN) {
    peaks::Component c;
    c.shape = shape;
    c.position = pos;
    c.fwhm = fwhm;
    c.posLo = posLo;
    c.posHi = posHi;
    c.fwhmLo = fwhmLo;
    c.fwhmHi = fwhmHi;
    c.height = height;
    return c;
}

QString num(double v, int prec) { return std::isfinite(v) ? QString::number(v, 'f', prec) : QString(); }
QString sig(double v) { return std::isfinite(v) ? QString::number(v, 'g', 5) : QString(); }

}  // namespace

QString modelName(Model m) {
    return m == Model::Sadezky ? QStringLiteral("Sadezky five-band (G, D1–D4)")
                               : QStringLiteral("D Lorentzian + G Breit–Wigner–Fano");
}

QString modelKey(Model m) { return m == Model::Sadezky ? QStringLiteral("sadezky") : QStringLiteral("lorentz-bwf"); }

double crystalliteSize(double idigArea, double laserNm) {
    if (!(idigArea > 0)) return kNaN;
    return 2.4e-10 * std::pow(laserNm, 4) / idigArea;
}

Result analyse(const Vec& x, const Vec& y, const Options& opt) {
    if (x.size() < 10 || x.size() != y.size()) throw Error("carbon D/G: no spectrum");
    const double lo = std::min(opt.lo, opt.hi), hi = std::max(opt.lo, opt.hi);
    if (lo > 1300 || hi < 1620 || x.front() > 1300 || x.back() < 1620)
        throw Error("carbon D/G: the spectrum (or the region) does not cover the D and G bands (1300–1620 cm⁻¹)");
    const double floor = windowMin(x, y, lo, hi);
    const auto [dPos, dH] = localMax(x, y, 1300, 1420, floor);
    const auto [gPos, gH] = localMax(x, y, 1540, 1620, floor);
    Result r;
    r.model = opt.model;
    if (opt.model == Model::LorentzBwf) {
        std::vector<peaks::Component> c2{comp("lorentz", dPos, 60, 1300, 1420, 10, 400, dH),
                                         comp("bwf", std::clamp(gPos, 1520.0, 1640.0), 50, 1520, 1640, 8, 300, gH)};
        const peaks::FitResult f2 = peaks::fitComponents(x, y, {lo, hi}, c2);
        std::vector<peaks::Component> c3 = c2;
        c3.push_back(comp("lorentz", 1620, 25, 1605, 1640, 5, 60, 0.2 * gH));
        bool useD3 = false;
        peaks::FitResult f3;
        try {
            f3 = peaks::fitComponents(x, y, {lo, hi}, c3);
            const peaks::FitPeak& p = f3.peaks[2];
            useD3 = p.height > 3 * p.heightErr && f3.rms < 0.95 * f2.rms && p.position > 1605.5 && p.position < 1639.5;
        } catch (const std::exception&) {
        }
        r.fit = useD3 ? f3 : f2;
        r.bands = {bandFrom("D", r.fit.peaks[0]), bandFrom("G", r.fit.peaks[1])};
        r.d = 0;
        r.g = 1;
        if (useD3) {
            r.bands.push_back(bandFrom(QStringLiteral("D′"), r.fit.peaks[2]));
            r.dPrime = 2;
        } else {
            r.notes << QStringLiteral("D′ not resolved (included in the BWF G band)");
        }
    } else {
        const double h3 = 0.5 * localMax(x, y, 1490, 1510, floor).second;
        const double h4 = 0.5 * localMax(x, y, 1190, 1210, floor).second;
        std::vector<peaks::Component> c{comp("lorentz", std::clamp(gPos, 1565.0, 1610.0), 50, 1565, 1610, 10, 150, gH),
                                        comp("lorentz", std::clamp(dPos, 1320.0, 1380.0), 120, 1320, 1380, 20, 350, dH),
                                        comp("lorentz", 1620, 30, 1605, 1635, 5, 80, 0.3 * gH),
                                        comp("gauss", 1500, 120, 1480, 1530, 40, 250, h3),
                                        comp("lorentz", 1200, 150, 1150, 1250, 40, 300, h4)};
        r.fit = peaks::fitComponents(x, y, {lo, hi}, c);
        const char* names[] = {"G", "D1", "D2", "D3", "D4"};
        for (size_t k = 0; k < r.fit.peaks.size(); ++k) r.bands.push_back(bandFrom(names[k], r.fit.peaks[k]));
        r.g = 0;
        r.d = 1;
        r.dPrime = 2;
    }
    const Band& D = r.bands[size_t(r.d)];
    const Band& G = r.bands[size_t(r.g)];
    r.idigHeight = G.height != 0 ? D.height / G.height : kNaN;
    r.idigHeightErr = ratioErr(r.idigHeight, D.height, D.heightErr, G.height, G.heightErr);
    r.idigArea = G.area != 0 ? D.area / G.area : kNaN;
    r.idigAreaErr = ratioErr(r.idigArea, D.area, D.areaErr, G.area, G.areaErr);
    r.la = crystalliteSize(r.idigArea);
    r.laErr = std::isfinite(r.la) ? r.la * relErr(r.idigArea, r.idigAreaErr) : kNaN;

    if (opt.secondOrder) {
        if (x.front() <= 2550 && x.back() >= 2800) {
            try {
                const double top = std::min(2850.0, x.back());
                const auto [p2, h2] = localMax(x, y, 2620, 2780, windowMin(x, y, 2500, top));
                const peaks::FitResult f =
                    peaks::fitComponents(x, y, {2500, top}, {comp("lorentz", p2, 60, 2620, 2780, 10, 300, h2)});
                r.fit2d = f;
                const peaks::FitPeak& p = f.peaks[0];
                const double noise = peaks::noiseLevel(y);
                if (p.height > 3 * p.heightErr && p.height > 3 * noise) {
                    r.band2d = bandFrom("2D", p);
                    r.i2dIg = G.height != 0 ? p.height / G.height : kNaN;
                } else {
                    r.notes << QStringLiteral("2D band not resolved");
                }
            } catch (const std::exception&) {
                r.notes << QStringLiteral("2D band not resolved");
            }
        } else {
            r.notes << QStringLiteral("2D region (2550–2800 cm⁻¹) not in the spectrum");
        }
    }
    return r;
}

const QStringList& Result::keys() {
    static const QStringList k{"id_ig_area", "id_ig_height", "la_nm",           "g_position",
                               "g_fwhm",     "d_position",   "d_fwhm",          "d_prime_position",
                               "2d_position", "2d_fwhm",     "i2d_ig"};
    return k;
}

double Result::value(const QString& key) const {
    auto band = [&](int i, bool pos) {
        if (i < 0 || size_t(i) >= bands.size()) return kNaN;
        return pos ? bands[size_t(i)].position : bands[size_t(i)].fwhm;
    };
    if (key == "id_ig_area") return idigArea;
    if (key == "id_ig_height") return idigHeight;
    if (key == "la_nm") return la;
    if (key == "g_position") return band(g, true);
    if (key == "g_fwhm") return band(g, false);
    if (key == "d_position") return band(d, true);
    if (key == "d_fwhm") return band(d, false);
    if (key == "d_prime_position") return band(dPrime, true);
    if (key == "2d_position") return band2d ? band2d->position : kNaN;
    if (key == "2d_fwhm") return band2d ? band2d->fwhm : kNaN;
    if (key == "i2d_ig") return band2d ? i2dIg : kNaN;
    return kNaN;
}

double Result::error(const QString& key) const {
    auto band = [&](int i, bool pos) {
        if (i < 0 || size_t(i) >= bands.size()) return kNaN;
        return pos ? bands[size_t(i)].positionErr : bands[size_t(i)].fwhmErr;
    };
    if (key == "id_ig_area") return idigAreaErr;
    if (key == "id_ig_height") return idigHeightErr;
    if (key == "la_nm") return laErr;
    if (key == "g_position") return band(g, true);
    if (key == "g_fwhm") return band(g, false);
    if (key == "d_position") return band(d, true);
    if (key == "d_fwhm") return band(d, false);
    if (key == "d_prime_position") return band(dPrime, true);
    if (key == "2d_position") return band2d ? band2d->positionErr : kNaN;
    if (key == "2d_fwhm") return band2d ? band2d->fwhmErr : kNaN;
    return kNaN;
}

std::vector<QStringList> Result::rows() const {
    const QString cm = QStringLiteral("cm⁻¹");
    std::vector<QStringList> out;
    out.push_back({QStringLiteral("model"), modelName(model), {}, {}});
    out.push_back({QStringLiteral("I_D/I_G (area)"), num(idigArea, 3), num(idigAreaErr, 3),
                   model == Model::Sadezky ? QStringLiteral("D1/G") : QString()});
    out.push_back({QStringLiteral("I_D/I_G (height)"), num(idigHeight, 3), num(idigHeightErr, 3),
                   model == Model::Sadezky ? QStringLiteral("D1/G") : QString()});
    out.push_back({QStringLiteral("L_a (Cançado, from the area ratio, λ = 532 nm)"), num(la, 2), num(laErr, 2),
                   QStringLiteral("nm")});
    auto addBand = [&](const Band& b) {
        out.push_back({b.name + QStringLiteral(" position"), num(b.position, 1), num(b.positionErr, 2), cm});
        if (b.center) out.push_back({b.name + QStringLiteral(" ω0 (BWF)"), num(*b.center, 1), {}, cm});
        if (b.q) out.push_back({b.name + QStringLiteral(" q (BWF)"), num(*b.q, 2), b.qErr ? num(*b.qErr, 2) : QString(), {}});
        out.push_back({b.name + QStringLiteral(" FWHM"), num(b.fwhm, 1), num(b.fwhmErr, 2), cm});
        out.push_back({b.name + QStringLiteral(" height"), sig(b.height), sig(b.heightErr), {}});
        out.push_back({b.name + QStringLiteral(" area"), sig(b.area), sig(b.areaErr), {}});
    };
    for (const Band& b : bands) addBand(b);
    if (band2d) {
        addBand(*band2d);
        out.push_back({QStringLiteral("I_2D/I_G (height)"), num(i2dIg, 3), {}, {}});
    }
    out.push_back({QStringLiteral("R² (first order)"), num(fit.r2, 5), {}, {}});
    for (const QString& n : notes) out.push_back({QStringLiteral("note"), n, {}, {}});
    return out;
}

QString Result::table() const {
    QStringList lines{QStringLiteral("quantity\tvalue\t±\tunit")};
    for (const QStringList& r : rows()) lines << r.join('\t');
    return lines.join('\n');
}

QString Result::csv(const QString& spectrum) const {
    auto quoted = [](QString t) {
        if (t.contains(',') || t.contains('"')) t = '"' + t.replace('"', QStringLiteral("\"\"")) + '"';
        return t;
    };
    QStringList lines{QStringLiteral("spectrum,quantity,value,error,unit")};
    for (const QStringList& r : rows()) {
        QStringList f{quoted(spectrum)};
        for (const QString& c : r) f << quoted(c);
        lines << f.join(',');
    }
    return lines.join('\n') + '\n';
}

}  // namespace lr::carbon
