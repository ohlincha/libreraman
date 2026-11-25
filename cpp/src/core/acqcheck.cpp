#include "acqcheck.h"

#include <QStringList>
#include <algorithm>
#include <map>
#include <numeric>

#include "processing.h"

namespace lr::acq {

namespace {

double median(Vec v) {
    if (v.empty()) return std::numeric_limits<double>::quiet_NaN();
    const size_t m = v.size() / 2;
    std::nth_element(v.begin(), v.begin() + long(m), v.end());
    const double hi = v[m];
    if (v.size() % 2) return hi;
    const double lo = *std::max_element(v.begin(), v.begin() + long(m));
    return 0.5 * (lo + hi);
}

struct Range {
    size_t lo = 0, hi = 0;                    // [lo, hi) of the points with kBandMin <= x <= kBandMax
    size_t size() const { return hi - lo; }
};

Range bandRange(const Vec& x, size_t n) {
    Range r;
    n = std::min(n, x.size());
    r.lo = size_t(std::lower_bound(x.begin(), x.begin() + long(n), kBandMin) - x.begin());
    r.hi = size_t(std::upper_bound(x.begin(), x.begin() + long(n), kBandMax) - x.begin());
    if (r.hi < r.lo) r.hi = r.lo;
    return r;
}

Vec slice(const Vec& v, const Range& r) { return Vec(v.begin() + long(r.lo), v.begin() + long(r.hi)); }

Vec minus(const Vec& a, const Vec& b) {
    Vec o(a.size());
    for (size_t i = 0; i < a.size(); ++i) o[i] = a[i] - b[i];
    return o;
}

Vec smoothed(const Vec& y) { return y.size() >= 7 ? processing::smooth(y, 7, 2) : y; }

// Parabolic vertex through (i-1, i, i+1): position and height.
std::pair<double, double> vertex(const Vec& x, const Vec& y, size_t i) {
    if (i == 0 || i + 1 >= y.size()) return {x[i], y[i]};
    const double a = y[i - 1], b = y[i], c = y[i + 1];
    const double den = a - 2 * b + c;
    if (den >= 0) return {x[i], b};
    const double off = std::clamp(0.5 * (a - c) / den, -0.5, 0.5);
    const double step = 0.5 * (x[i + 1] - x[i - 1]);
    return {x[i] + off * step, b - 0.25 * (a - c) * off};
}

// Index of the maximum of y with |x - at| <= w; npos if none.
size_t argmaxNear(const Vec& x, const Vec& y, double at, double w) {
    size_t best = size_t(-1);
    for (size_t i = 0; i < x.size(); ++i)
        if (std::abs(x[i] - at) <= w && (best == size_t(-1) || y[i] > y[best])) best = i;
    return best;
}

// Iterated centroid of max(y, 0) within ± hw of the current estimate (robust for broad, flat-topped bands, whose
// maximum wanders with the noise), and its standard error for per-point noise n.
std::pair<double, double> centroid(const Vec& x, const Vec& y, double c, double hw, double n) {
    double sigma = std::numeric_limits<double>::infinity();
    for (int it = 0; it < 6; ++it) {
        double sw = 0, swx = 0;
        for (size_t i = 0; i < x.size(); ++i)
            if (std::abs(x[i] - c) <= hw && y[i] > 0) sw += y[i], swx += y[i] * x[i];
        if (sw <= 0) return {c, sigma};
        const double next = swx / sw;
        double q = 0;
        for (size_t i = 0; i < x.size(); ++i)
            if (std::abs(x[i] - next) <= hw) q += (x[i] - next) * (x[i] - next);
        sigma = n * std::sqrt(q) / sw;
        const bool done = std::abs(next - c) < 0.01;
        c = next;
        if (done) break;
    }
    return {c, sigma};
}

// noise() of y over the points with |x - at| <= w (shot noise varies along a fluorescent spectrum).
double localNoise(const Vec& x, const Vec& y, double at, double w) {
    Vec d;
    for (size_t i = 1; i + 1 < x.size(); ++i)
        if (std::abs(x[i] - at) <= w) d.push_back(y[i - 1] - 2 * y[i] + y[i + 1]);
    if (d.size() < 16) return std::numeric_limits<double>::quiet_NaN();
    const double m = median(d);
    for (double& v : d) v = std::abs(v - m);
    return 1.4826 * median(d) / std::sqrt(6.0);
}

QString pct(double f) {
    const int p = int(std::lround(100 * f));
    return (p > 0 ? QStringLiteral("+") : p < 0 ? QStringLiteral("−") : QString()) + QString::number(std::abs(p)) +
           QStringLiteral(" %");
}

}  // namespace

double noise(const Vec& x, const Vec& y) {
    const Range r = bandRange(x, y.size());
    if (r.size() < 16) return std::numeric_limits<double>::quiet_NaN();
    Vec d;
    d.reserve(r.size());
    for (size_t i = r.lo + 1; i + 1 < r.hi; ++i) d.push_back(y[i - 1] - 2 * y[i] + y[i + 1]);
    const double m = median(d);
    for (double& v : d) v = std::abs(v - m);
    return 1.4826 * median(d) / std::sqrt(6.0);
}

// ------------------------------------------------------------------------------------------- FrameStore
void FrameStore::add(const Vec& f) {
    if (m_count == 0) m_total.assign(f.size(), 0.0);
    const size_t n = std::min(f.size(), m_total.size());
    for (size_t i = 0; i < n; ++i) m_total[i] += f[i];
    if (m_blocks.empty() || m_blocks.back().n >= m_blockSize) {
        m_blocks.push_back({Vec(f.begin(), f.begin() + long(n)), 1});
        m_blocks.back().sum.resize(m_total.size(), 0.0);
    } else {
        Block& b = m_blocks.back();
        for (size_t i = 0; i < n; ++i) b.sum[i] += f[i];
        ++b.n;
    }
    ++m_count;
    if (int(m_blocks.size()) > kMaxBlocks) {             // merge neighbours: half as many, twice as large
        std::vector<Block> merged;
        for (size_t k = 0; k < m_blocks.size(); k += 2) {
            Block b = std::move(m_blocks[k]);
            if (k + 1 < m_blocks.size()) {
                for (size_t i = 0; i < b.sum.size(); ++i) b.sum[i] += m_blocks[k + 1].sum[i];
                b.n += m_blocks[k + 1].n;
            }
            merged.push_back(std::move(b));
        }
        m_blocks = std::move(merged);
        m_blockSize *= 2;
    }
}

Vec FrameStore::mean() const {
    Vec m = m_total;
    if (m_count > 0)
        for (double& v : m) v /= double(m_count);
    return m;
}

std::pair<Vec, Vec> FrameStore::thirds(int* nEarly, int* nLate) const {
    if (nEarly) *nEarly = 0;
    if (nLate) *nLate = 0;
    if (m_count < 2 || m_blocks.size() < 2) return {};
    const int k = std::max(1, m_count / 3);
    size_t ie = 0;
    int ne = 0;
    Vec early(m_total.size(), 0.0), late(m_total.size(), 0.0);
    while (ie < m_blocks.size() && ne < k) {
        for (size_t i = 0; i < early.size(); ++i) early[i] += m_blocks[ie].sum[i];
        ne += m_blocks[ie].n;
        ++ie;
    }
    size_t il = m_blocks.size();
    int nl = 0;
    while (il > ie && nl < k) {
        --il;
        for (size_t i = 0; i < late.size(); ++i) late[i] += m_blocks[il].sum[i];
        nl += m_blocks[il].n;
    }
    if (ne == 0 || nl == 0) return {};
    for (double& v : early) v /= ne;
    for (double& v : late) v /= nl;
    if (nEarly) *nEarly = ne;
    if (nLate) *nLate = nl;
    return {std::move(early), std::move(late)};
}

void FrameStore::clear() { *this = FrameStore(); }

std::vector<Vec> alignFrames(const Spectrum& sp, const std::vector<Vec>& frames, const Vec& dataPixels) {
    std::map<double, size_t> at;
    for (size_t j = 0; j < dataPixels.size(); ++j) at.emplace(dataPixels[j], j);
    std::vector<Vec> out;
    out.reserve(frames.size());
    const size_t n = sp.shift.size();
    for (const Vec& f : frames) {
        Vec v(n, 0.0);
        for (size_t i = 0; i < n && i < sp.pixels.size(); ++i) {
            const auto it = at.find(sp.pixels[i]);
            if (it == at.end() || it->second >= f.size()) continue;
            v[i] = f[it->second] - (i < sp.background.size() ? sp.background[i] : 0.0);
        }
        out.push_back(std::move(v));
    }
    return out;
}

// ------------------------------------------------------------------------------------------- damage
QString damageAdvice() {
    return QStringLiteral("sample changing under the laser: shorten the exposure or defocus slightly (distance knob)");
}

QString DamageReport::summary() const {
    if (!valid) return {};
    QStringList parts, quiet;
    if (std::isfinite(maxShiftAt))
        (shifted ? parts : quiet)
            << QStringLiteral("band %1 → %2 cm⁻¹").arg(maxShiftAt, 0, 'f', 0).arg(maxShiftAt + maxShift, 0, 'f', 1);
    if (!bands.empty()) (faded ? parts : quiet) << QStringLiteral("bands %1").arg(pct(intensityChange));
    (background ? parts : quiet) << QStringLiteral("background %1").arg(pct(backgroundChange));
    return (parts + quiet).join(QStringLiteral(" · "));
}

DamageReport compareSpectra(const Vec& x, const Vec& early, const Vec& late, const DamageThresholds& t,
                            double noiseEarly, double noiseLate) {
    DamageReport r;
    const Range rg = bandRange(x, std::min(early.size(), late.size()));
    if (rg.size() < 32) return r;
    const Vec xs = slice(x, rg);
    const Vec ye = processing::despike(slice(early, rg)), yl = processing::despike(slice(late, rg));
    const double ne = std::isfinite(noiseEarly) ? noiseEarly : noise(x, early);
    const double nl = std::isfinite(noiseLate) ? noiseLate : noise(x, late);
    if (!std::isfinite(ne) || !std::isfinite(nl)) return r;
    const double nboth = std::max(std::sqrt(ne * ne + nl * nl), 1e-12);
    // background level of each (rising background), but one common background under the bands: SNIP under a
    // broad band varies with the noise, and separate backgrounds would tilt it differently in the two spectra
    const Vec be = processing::baselineSnip4(ye), bl = processing::baselineSnip4(yl);
    Vec avg(ye.size());
    for (size_t i = 0; i < avg.size(); ++i) avg[i] = 0.5 * (ye[i] + yl[i]);
    const Vec bc = processing::baselineSnip4(avg);
    const double lift = 0.5 * (median(bl) - median(be));     // a background rise is not a band change
    Vec bce = bc, bcl = bc;
    for (size_t i = 0; i < bc.size(); ++i) bce[i] -= lift, bcl[i] += lift;
    const Vec se = smoothed(minus(ye, bce)), sl = smoothed(minus(yl, bcl));
    const double bgE = median(be), bgL = median(bl);

    // strongest bands of the early spectrum
    const double smax = *std::max_element(se.begin(), se.end());
    const sig::PeakSet ps = sig::findPeaks(se, std::max(t.minBandSnr * ne, 0.03 * smax));
    const Vec widths = sig::peakWidths(se, ps.peaks, 0.5);       // FWHM in points
    const double spacing = (xs.back() - xs.front()) / double(xs.size() - 1);
    std::vector<size_t> order(ps.peaks.size());
    std::iota(order.begin(), order.end(), size_t(0));
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return ps.prominences[a] > ps.prominences[b]; });
    double sumE = 0, sumL = 0, maxHeight = 0;
    for (size_t k : order) {
        if (int(r.bands.size()) >= t.bands) break;
        const size_t i = size_t(ps.peaks[k]);
        if (se[i] < t.minBandSnr * ne) continue;
        BandChange b;
        std::tie(b.early, b.heightEarly) = vertex(xs, se, i);
        const size_t j = argmaxNear(xs, sl, b.early, t.window_cm);
        if (j == size_t(-1)) continue;
        std::tie(b.late, b.heightLate) = vertex(xs, sl, j);
        r.bands.push_back(b);
        sumE += b.heightEarly;
        sumL += b.heightLate;
        maxHeight = std::max(maxHeight, b.heightEarly);
        // position: centroid over the band's upper half (± FWHM/2); a band that faded into the noise has no
        // meaningful position, and a shift must exceed the position uncertainty
        if (b.heightLate >= 0.5 * t.minBandSnr * nl) {
            const double fwhm = widths[k] * spacing;
            const double hw = std::clamp(0.5 * fwhm, 2 * spacing, 100.0);
            const double w = std::max(3 * hw, 60 * spacing);
            const double lne = std::max(ne, localNoise(xs, ye, b.early, w));   // (max: NaN -> ne)
            const double lnl = std::max(nl, localNoise(xs, yl, b.early, w));
            const auto [ce, ue] = centroid(xs, se, b.early, hw, lne);
            const auto [cl, ul] = centroid(xs, sl, ce, hw, lnl);
            const double d = cl - ce;
            // broad bands (water's OH stretch, amorphous carbon) have a poorly defined position: the shift must
            // also exceed widthFraction of the FWHM
            const double need = std::max(t.noiseSigmas * std::hypot(ue, ul), t.widthFraction * fwhm);
            if (std::abs(d) > need && std::abs(d) > std::abs(r.maxShift)) {
                r.maxShift = d;
                r.maxShiftAt = b.early;
            }
        }
    }
    const double ref = std::max(bgE, maxHeight);
    r.valid = !r.bands.empty() || bgE > t.minBandSnr * ne;
    if (!r.valid) return r;
    r.shifted = std::abs(r.maxShift) > t.shift_cm;
    if (sumE > 0) {
        r.intensityChange = sumL / sumE - 1.0;
        r.faded = r.intensityChange < -t.intensityLoss &&
                  sumE - sumL > t.noiseSigmas * nboth * std::sqrt(double(r.bands.size()));
    }
    if (ref > 0) {
        r.backgroundChange = (bgL - bgE) / ref;
        r.background = r.backgroundChange > t.backgroundRise && bgL - bgE > t.noiseSigmas * nboth;
    }
    r.changing = r.shifted || r.faded || r.background;
    return r;
}

DamageReport checkFrames(const Vec& x, const FrameStore& frames, const DamageThresholds& t) {
    int ne = 0, nl = 0;
    const auto [early, late] = frames.thirds(&ne, &nl);
    if (early.empty()) return {};
    DamageReport r = compareSpectra(x, early, late, t);
    r.framesEarly = ne;
    r.framesLate = nl;
    return r;
}

DamageReport checkFrames(const Vec& x, const std::vector<Vec>& frames, const DamageThresholds& t) {
    FrameStore s;
    for (const Vec& f : frames) s.add(f);
    return checkFrames(x, s, t);
}

void setDamageMeta(Spectrum& sp, const DamageReport& r) {
    if (!r.valid) return;
    sp.setMeta(QStringLiteral("laser_damage"), r.changing ? QStringLiteral("suspected") : QStringLiteral("none detected"));
    sp.setMeta(QStringLiteral("laser_damage_check"),
               QStringLiteral("%1 (first %2 vs last %3 frames)").arg(r.summary()).arg(r.framesEarly).arg(r.framesLate));
}

// ------------------------------------------------------------------------------------------- S/N
Snr estimateSnr(const Vec& x, const Vec& y, std::optional<double> band, double window) {
    Snr s;
    const Range rg = bandRange(x, y.size());
    if (rg.size() < 32) return s;
    const Vec xs = slice(x, rg);
    const Vec ys = processing::despike(slice(y, rg));
    const double n = noise(x, y);
    if (!std::isfinite(n) || n <= 0) return s;
    const Vec c = minus(ys, processing::baselineSnip4(ys));
    size_t i = 0;
    if (band) {
        i = argmaxNear(xs, c, *band, window);
        if (i == size_t(-1)) return s;
    } else {
        i = size_t(std::max_element(c.begin(), c.end()) - c.begin());
    }
    std::tie(s.band, s.height) = vertex(xs, c, i);
    s.noise = n;
    s.snr = s.height / n;
    return s;
}

QString stopText(Stop s) {
    switch (s) {
    case Stop::Reached: return QStringLiteral("target S/N reached");
    case Stop::TimeLimit: return QStringLiteral("time limit");
    case Stop::Damage: return QStringLiteral("sample changing under the laser");
    case Stop::Continue: break;
    }
    return {};
}

Stop snrStop(const SnrTarget& t, double snr, int frames, double elapsed, double frameSeconds, bool damage) {
    if (t.stopOnDamage && damage && frames >= t.damageMinFrames) return Stop::Damage;
    if (frames >= t.minFrames && std::isfinite(snr) && snr >= t.target) return Stop::Reached;
    if (elapsed + frameSeconds > t.maxSeconds) return Stop::TimeLimit;
    return Stop::Continue;
}

int framesNeeded(double snr, int frames, double target) {
    if (!std::isfinite(snr) || snr <= 0 || frames <= 0) return -1;
    const double r = target / snr;
    return std::max(frames, int(std::ceil(frames * r * r)));
}

SnrRun::SnrRun(SnrTarget target, DamageThresholds damage) : m_target(std::move(target)), m_thresholds(damage) {}

Stop SnrRun::add(const Vec& x, const Vec& intensity, double elapsed, double frameSeconds) {
    if (m_x.empty()) m_x = x;
    m_frames.add(intensity);
    m_snr = estimateSnr(m_x, m_frames.mean(), m_target.band);
    if (m_frames.count() >= m_target.damageMinFrames)
        m_damage = checkFrames(m_x, m_frames, m_thresholds);
    m_stop = snrStop(m_target, m_snr.snr, m_frames.count(), elapsed, frameSeconds, m_damage.changing);
    return m_stop;
}

}  // namespace lr::acq
