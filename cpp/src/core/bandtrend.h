#pragma once
// Peak trends (kinetics) of a series of spectra: per band (a Raman-shift region) and spectrum the height,
// integrated area, centroid, FWHM and, optionally, the fitted area, height, position and FWHM of the band's
// main peak; normalisation (ratio to a reference band, relative to the first point) with error propagation;
// kinetic fits of a trend (single exponential, straight line). No widgets: used by the Analysis area and
// usable from the API/CLI.
//
// Formulas (spectrum y(x) on the points x_i inside the region [lo, hi]):
//   local baseline (option, default on): straight line through the two edge anchors; each anchor is the mean
//     (x, y) of the outermost m points at that edge, m = clamp(n/10, 1, 3) for n points in the region (less
//     sensitive to noise than a single point). Off: zero. s_i = y_i - b(x_i).
//   height:   maximum of s, refined by the parabola through the maximum and its two neighbours; its vertex
//             also gives `peak` (position of the maximum).
//   area:     trapezoid rule, sum (x_{i+1} - x_i)(s_i + s_{i+1})/2 over the region's points.
//   centroid: sum x_i w_i / sum w_i, w_i = s_i - height/2, over the contiguous points around the maximum with
//             s_i >= height/2 (the part above half maximum: far less sensitive to noise and baseline than the
//             whole region; the weights vanish at the edge, so points entering or leaving do not cause jumps).
//   fwhm:     width between the linear interpolations of s = height/2 left and right of the maximum
//             (NaN if s does not fall below half the maximum inside the region).
//   fit:      peaks::fit() in the region (its own linear baseline) with the band's profile and start values
//             `guesses` (normally peaks::pick() on the mean spectrum, so every spectrum is fitted with the same
//             number of peaks); the main peak is guesses[main]. Area error by propagation of the height and
//             FWHM errors (area ∝ height·FWHM for every profile at fixed shape; correlations ignored). A main
//             peak outside the region or wider than it (a weak or vanished band) gives NaN.
//   ratio:    r = a/b, σ_r = |r| sqrt((σ_a/a)² + (σ_b/b)²).
//   relative: y_i / y_first (first finite value), errors scaled alike.
//   exponential: y = A + B exp(-(t - t0)/τ), t0 = first time. Start values: τ on a logarithmic grid with A, B
//             by linear least squares (variable projection), then Levenberg-Marquardt on (A, B, k = 1/τ);
//             standard errors from s²(JᵀJ)⁻¹, s² = SSR/(n - 3); σ_τ = σ_k/k². Weighted by 1/σ² when all errors
//             are given and positive.
//   linear:   y = a + b·t by (weighted) least squares; rate = b.
//   R² = 1 - SSR/SST (unweighted).
#include <QString>
#include <QStringList>

#include <atomic>
#include <limits>
#include <vector>

#include "peaks.h"
#include "spectrum.h"

namespace lr::bandtrend {

constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

enum class Quantity {
    Height,          // maximum in the region
    Area,            // integrated area (trapezoid)
    Centroid,        // position: centre of mass
    Fwhm,            // full width at half maximum (interpolated)
    FitArea,         // fitted area of the main peak (with error)
    FitHeight,       // fitted height of the main peak (with error)
    FitPosition,     // fitted position of the main peak (with error)
    FitFwhm,         // fitted FWHM of the main peak (with error)
    // carbon D/G analysis (carbon.h, D Lorentzian + G BWF) of the spectrum, with errors; the fit region is the
    // band's region if it holds 1300-1620 cm⁻¹, else 950-1800 cm⁻¹
    CarbonIdIgArea,  // I_D/I_G by area
    CarbonIdIgHeight,  // I_D/I_G by height
    CarbonLa,        // crystallite size L_a (nm, Cançado, from the area ratio)
    CarbonGPosition, // G position (BWF maximum)
};
// Display names ("Height", "Area", ..., "Fitted area", ...) in enum order, and short keys for files/APIs.
const QStringList& quantityNames();
QString quantityKey(Quantity q);                       // "height", "area", "centroid", "fwhm", "fit_area", ...
bool needsFit(Quantity q);
bool isPosition(Quantity q);                           // Centroid, FitPosition (unit cm⁻¹, not ratio-able)
QString quantityUnit(Quantity q);                      // "cm⁻¹" for positions and widths, "nm" for L_a, "" otherwise
bool isCarbon(Quantity q);                             // a carbon D/G quantity (not of the band's main peak)

struct Band {
    QString name;                  // e.g. "1052 cm⁻¹"
    double lo = 0, hi = 0;         // region (cm⁻¹); lo < hi
    Quantity quantity = Quantity::Height;
    bool localBaseline = true;
    QString profile = QStringLiteral("lorentz");       // peaks::profiles() name for the fitted quantities
    std::vector<peaks::Peak> guesses;                  // start values of the fit (empty: picked per spectrum)
    int main = 0;                                      // index of the main peak in guesses
};

// Measurements of one band in one spectrum (NaN where undefined).
struct Measure {
    double height = kNaN, peak = kNaN, area = kNaN, centroid = kNaN, fwhm = kNaN;
    bool fitted = false;                               // fit attempted
    double fitArea = kNaN, fitAreaErr = kNaN, fitHeight = kNaN, fitHeightErr = kNaN;
    double fitPosition = kNaN, fitPositionErr = kNaN, fitFwhm = kNaN, fitFwhmErr = kNaN, fitR2 = kNaN;
    double idigArea = kNaN, idigAreaErr = kNaN, idigHeight = kNaN, idigHeightErr = kNaN;   // carbon (fit)
    double la = kNaN, laErr = kNaN, gPosition = kNaN, gPositionErr = kNaN;
    double value(Quantity q) const;
    double error(Quantity q) const;                    // NaN for the quantities without an error estimate
};

// One band in one spectrum (x ascending). fit: also the fitted quantities (slower); for a carbon quantity the
// carbon D/G analysis instead of the band's profile fit.
Measure measure(const Vec& x, const Vec& y, const Band& band, bool fit);

// Start values for fitting `band` in every spectrum: peaks picked on `y` (normally the mean spectrum) in the
// region (pick(snr 4) on the whole spectrum, as peaks::fit); without any, one peak at the region's maximum.
// Sets band.guesses and band.main (the tallest).
void setGuesses(Band& band, const Vec& x, const Vec& y);
// A band of ±2·FWHM (at least ±3 points, ±5 cm⁻¹) around a picked peak, named "1052 cm⁻¹".
Band bandAround(const peaks::Peak& p, const Vec& x);
QString defaultName(double position);                  // "1052 cm⁻¹"

// A series: spectra are the rows (all on the axis x). Measures rows [from, rows.size()) of every band;
// result[b][i - from]. stop: checked between spectra (the result is then incomplete).
std::vector<std::vector<Measure>> measureSeries(const Vec& x, const std::vector<Vec>& rows,
                                                const std::vector<Band>& bands, size_t from = 0,
                                                const std::atomic<bool>* stop = nullptr);

// ---- trends
struct Series {
    Vec value, error;              // error: NaN (or empty) without estimate
};
Series extract(const std::vector<Measure>& m, Quantity q);
Series ratio(const Series& a, const Series& b);
Series relativeToFirst(const Series& a);

// ---- kinetic fits
enum class Kinetic { None, Exponential, Linear };
struct KineticFit {
    Kinetic model = Kinetic::None;
    bool ok = false;
    QString message;               // why it failed
    double t0 = 0;                 // exponential: time origin (first time)
    double a = kNaN, b = kNaN;     // exponential: A, B; linear: intercept a (at t = 0), slope b
    double aErr = kNaN, bErr = kNaN;
    double tau = kNaN, tauErr = kNaN;                  // exponential
    double rate = kNaN, rateErr = kNaN;                // exponential: k = 1/τ; linear: slope b
    double r2 = kNaN;
    int points = 0;
    double operator()(double t) const;                 // model value
    QString summary(const QString& timeUnit = QStringLiteral("s"), double timeScale = 1.0) const;   // "τ = 120 ± 3 s, R² = 0.998"
};
// Points with a non-finite t or y (or error) are skipped. err: optional standard errors (weights).
KineticFit fitExponential(const Vec& t, const Vec& y, const Vec& err = {});
KineticFit fitLinear(const Vec& t, const Vec& y, const Vec& err = {});
KineticFit fitKinetic(Kinetic model, const Vec& t, const Vec& y, const Vec& err = {});

// Time axis unit for a span in seconds: ("s", 1), ("min", 60) from 180 s, ("h", 3600) from 3 h (as the
// Measure page's trend).
std::pair<QString, double> timeUnit(double spanSeconds);

}  // namespace lr::bandtrend
