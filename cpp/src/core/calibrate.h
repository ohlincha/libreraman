#pragma once
// Calibration against reference standards (Python: calibrate.py): peak finding, matching to the bands of
// standards.h, weighted polynomial fit, one-point (offset) calibration, storage as JSON compatible with
// the Python version (<config>/libreramen/calibration_<serial>_<channel>.json).
//
// 1. Find peaks in a background-subtracted spectrum (in calibration-pixel order).
// 2. Match them to the standard's reference bands, using the current calibration as the starting point.
// 3. Fit a polynomial Raman shift(pixel) through all matches (several standards can be combined).
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

struct FitOutput {                // fit(): polynomial, per-band residuals, RMS
    Poly poly;
    std::vector<Residual> residuals;
    double rms = 0.0;
};

struct Result {                   // calibrate() result dict
    Poly poly;
    std::vector<Residual> residuals;
    double rms = 0.0;
    std::vector<std::pair<QString, std::vector<Match>>> matches;   // per standard
    std::vector<QString> standards;
    QString date;                 // "yyyy-MM-dd HH:mm", local time
    int degree = 0;
    UserCalibration userCalibration() const;                      // what the instrument stores
};

// ---- small numerics (Python _smooth, _baseline, _solve, polyfit; polyval is Poly::operator())
Vec smooth(const Vec& y, int w);                 // centred moving average, shrinking window at the ends
Vec baseline(const Vec& y, int w = 151);         // rolling minimum, then smoothed
Vec solve(std::vector<Vec> a, Vec b);            // Gauss-Jordan with partial pivoting; throws "singular fit"
// Weighted least-squares polynomial in t = (x - x0) / scale, x0 = mean(x), scale = (max - min) / 2 or 1.
Poly polyfit(const Vec& x, const Vec& y, int deg, const Vec& w = {});

// ---- peaks, matching, fit
std::vector<PixelPeak> findPeaks(const Vec& y, double minRel = 0.02, int smooth = 3);
// Pair reference bands with measured peaks; shiftOf = current calibration.
std::vector<Match> match(const std::vector<PixelPeak>& peaks, const ShiftFn& shiftOf, const QString& standard,
                         double window = 12.0);
// Matches from one or more standards -> shift polynomial + residuals. With a single band (e.g. silicon) the
// current calibration `base` is shifted by a constant offset (one-point calibration; needs base).
FitOutput fit(const std::vector<Match>& matches, std::optional<int> deg = std::nullopt, const ShiftFn& base = {});
// samples: (intensities in calibration-pixel order, standard name).
Result calibrate(const std::vector<std::pair<Vec, QString>>& samples, const ShiftFn& shiftOf,
                 std::optional<int> deg = std::nullopt, double window = 12.0);
QString report(const Result& result, const ShiftFn& before = {});
// Writes the result as JSON (same structure as the Python save()); returns the path. Default path:
// configDir()/calibration_<serial>_<channel>.json.
QString save(const Result& result, int channel, const QString& serial, const QString& path = {});

}  // namespace lr::calib
