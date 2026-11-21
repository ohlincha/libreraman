#pragma once
// Calibration against reference standards (Python: calibrate.py): peak finding, matching to the bands of
// standards.h, weighted polynomial fit, one-point (offset) calibration, storage as JSON compatible with
// the Python version (<config>/libreramen/calibration_<serial>_<channel>.json).
//
// 1. Find peaks in a background-subtracted spectrum (in calibration-pixel order).
// 2. Match them to the standard's reference bands, using the current (or the factory) calibration as the
//    starting point.
// 3. Fit a CORRECTION to the factory calibration through all matches (several standards can be combined):
//    delta(p) = reference shift - factory shift(p). Why: a free polynomial through bands on a small part of the
//    detector extrapolates wildly (carbon tetrachloride alone, pixels 257-441 of 2048, auto cubic: pixel 2047 ->
//    27453 cm-1 instead of 4190). So the degree of the correction is limited by the coverage of the matched
//    bands (autoDegree), and outside the matched pixel range the correction only continues linearly
//    (correction()). The factory calibration supplies the shape over the whole detector.
// 4. Store shift(p) = factory(p) + delta(p) as one ordinary polynomial {coeff, x0, scale}, fitted over a dense
//    pixel grid of the whole detector (degree kStoreDegree): the file format and all readers (C++ and Python)
//    stay unchanged.
#include <QString>
#include <functional>
#include <optional>
#include <utility>
#include <vector>

#include "instrument.h"

namespace lr::calib {

QString configDir();              // $XDG_CONFIG_HOME/libreramen or ~/.config/libreramen (Windows/macOS: same
                                  // relative location as the Python version uses, for shared files)
std::optional<UserCalibration> load(int channel, const QString& serial, const QString& path = {});

using ShiftFn = std::function<double(double)>;   // Raman shift of a (fractional) calibration pixel

struct PixelPeak {                // find_peaks: fractional pixel index and height above the baseline
    double pixel, height;
};

struct Match {                    // match: (pixel, reference_shift, strength, height)
    double pixel, reference;
    int strength;
    double height;
};

struct Residual {                 // (reference, fitted - reference, pixel)
    double reference, residual, pixel;
};

struct FitOutput {                // fit(): polynomial, per-band residuals, RMS, degree of the correction
    Poly poly;
    std::vector<Residual> residuals;
    double rms = 0.0;
    int degree = 0;
};

struct Result {                   // calibrate() result dict
    Poly poly;
    std::vector<Residual> residuals;
    double rms = 0.0;
    std::vector<std::pair<QString, std::vector<Match>>> matches;   // per standard
    std::vector<QString> standards;
    QString date;                 // "yyyy-MM-dd HH:mm", local time
    int degree = 0;               // degree of the correction to the factory calibration (0 = offset)
    UserCalibration userCalibration() const;                      // what the instrument stores
};

// ---- small numerics (Python _smooth, _baseline, _solve, polyfit; polyval is Poly::operator())
Vec smooth(const Vec& y, int w);                 // centred moving average, shrinking window at the ends
Vec baseline(const Vec& y, int w = 151);         // rolling minimum, then smoothed
Vec solve(std::vector<Vec> a, Vec b);            // Gauss-Jordan with partial pivoting; throws "singular fit"
// Weighted least-squares polynomial in t = (x - x0) / scale, x0 = mean(x), scale = (max - min) / 2 or 1.
Poly polyfit(const Vec& x, const Vec& y, int deg, const Vec& w = {});

// ---- correction model (Python STORE_DEG, STORE_STEP, auto_degree, correction)
constexpr int kStoreDegree = 6;   // stored shift polynomial: reproduces the factory calibration to < 0.01 cm-1
constexpr int kStoreStep = 8;     // pixel grid of that fit (plus the last pixel)
QString degreeName(int degree);   // "offset", "linear", "quadratic", "cubic", "degree n"
// Correction degree allowed by the coverage of the matched pixels (detector of npix pixels): span < 25 % ->
// 0 (offset only); otherwise 1 (2-3 bands) or 2 (4+ bands); 3 only with 6+ bands over >= 60 % and no gap
// wider than 30 % of their span.
int autoDegree(const Vec& pixels, int npix = 2048);
// delta(p) = reference - base(p) through the matches: weighted polynomial of degree deg inside the matched
// pixel range, outside it linear from the edge value with the slope of a weighted straight-line fit
// (constant for degree 0).
ShiftFn correction(const std::vector<Match>& matches, int deg, const ShiftFn& base);

// ---- peaks, matching, fit
std::vector<PixelPeak> findPeaks(const Vec& y, double minRel = 0.02, int smooth = 3);
// Pair reference bands with measured peaks; shiftOf = current calibration.
std::vector<Match> match(const std::vector<PixelPeak>& peaks, const ShiftFn& shiftOf, const QString& standard,
                         double window = 12.0);
// Matches from one or more standards -> shift polynomial + residuals. With base (the factory calibration): a
// correction of degree deg (default autoDegree) to base, stored as shift = base + delta (degree kStoreDegree
// over pixels 0..npix-1); a single band (e.g. silicon) is a constant offset, the same as a corrected laser
// wavelength. Without base: a free polynomial through the bands (needs 2+ bands).
FitOutput fit(const std::vector<Match>& matches, std::optional<int> deg = std::nullopt, const ShiftFn& base = {},
              int npix = 2048);
// samples: (intensities in calibration-pixel order, standard name). Peaks are matched against shiftOf, the
// correction is fitted to base (default: shiftOf; normally the factory calibration).
Result calibrate(const std::vector<std::pair<Vec, QString>>& samples, const ShiftFn& shiftOf,
                 std::optional<int> deg = std::nullopt, double window = 12.0, const ShiftFn& base = {});
QString report(const Result& result, const ShiftFn& before = {});
// Writes the result as JSON (same structure as the Python save()); returns the path. Default path:
// configDir()/calibration_<serial>_<channel>.json.
QString save(const Result& result, int channel, const QString& serial, const QString& path = {});

}  // namespace lr::calib
