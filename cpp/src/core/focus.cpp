#include "focus.h"

#include <algorithm>
#include <cmath>

#include "processing.h"
#include "response.h"

namespace lr::focus {

Signal measure(const Spectrum& sp, double follow) {
    Signal s;
    const Vec& x = sp.shift;
    const Vec& y = sp.intensity;
    const std::size_t n = std::min(x.size(), y.size());
    if (n < 10) return s;
    const Vec base = processing::baseline(x, y, QStringLiteral("snip"), 5.0);
    // what is left after the background, inside the range; its median is the true zero (most points are
    // background; SNIP may sit a little off under a slope), its spread the noise
    std::vector<std::size_t> idx;
    Vec r;
    for (std::size_t i = 0; i < n; ++i)
        if (x[i] >= kMin && x[i] <= kMax) idx.push_back(i), r.push_back(y[i] - base[i]);
    if (r.size() < 10) return s;
    Vec tmp = r;
    std::nth_element(tmp.begin(), tmp.begin() + long(tmp.size() / 2), tmp.end());
    const double zero = tmp[tmp.size() / 2];
    for (double& v : tmp) v = std::abs(v - zero);
    std::nth_element(tmp.begin(), tmp.begin() + long(tmp.size() / 2), tmp.end());
    const double sigma = 1.4826 * tmp[tmp.size() / 2];
    double best = -1e300;
    for (std::size_t k = 0; k < idx.size(); ++k) {
        const std::size_t i = idx[k];
        const double v = r[k] - zero;
        if (k > 0 && v > 3 * sigma) s.total += v * std::abs(x[i] - x[idx[k - 1]]);   // the bands, not the noise
        const bool near = follow <= 0 || std::abs(x[i] - follow) <= 15;
        if (near && v > best) best = v, s.bandShift = x[i];
    }
    s.band = best > -1e300 ? best : 0;
    s.rawMax = sp.signal.empty() ? std::nan("") : *std::max_element(sp.signal.begin(), sp.signal.end());
    s.saturated = std::isfinite(s.rawMax) && s.rawMax >= response::kSaturated;
    return s;
}

}  // namespace lr::focus
