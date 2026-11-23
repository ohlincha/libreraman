#pragma once
// Spectral processing (Python: processing.py). Everything works on copies; raw data is never modified.
// Baseline methods: als, arpls, airpls, polynomial (modified polynomial), rolling_ball, snip, snip4, none.
// baseline() fits only on x >= fitMin (default kBaselineFitMin, 150 cm-1): below it the edge filter cuts the signal to ~0 and
// that cliff drags every method down beside low-wavenumber bands (ZnS on fluorescence). There the baseline
// equals the data.
//
// lr::sig holds the few scipy.signal / scipy.ndimage equivalents the processing and peak code relies on
// (savgol_filter, find_peaks, peak_prominences, peak_widths), with scipy's semantics.
#include <QString>
#include <QVariantMap>
#include <optional>
#include <utility>
#include <vector>

#include "spectrum.h"

namespace lr::sig {

// scipy.signal.savgol_filter(y, window, order, deriv, delta=1.0, mode="interp") for odd windows. Throws lr::Error if
// order >= window or window > y.size() (as scipy does).
Vec savgolFilter(const Vec& y, int window, int order, int deriv = 0);

struct PeakSet {                       // scipy.signal.find_peaks result (indices + properties used here)
    std::vector<int> peaks;
    Vec prominences;
    std::vector<int> leftBases, rightBases;
    Vec widths;                        // only if minWidth was given
};
// scipy.signal.find_peaks(y, prominence=minProminence, width=minWidth, distance=distance); minWidth <= 0
// and distance <= 0 mean "not given".
PeakSet findPeaks(const Vec& y, double minProminence, double minWidth = 0.0, double distance = 0.0);
// scipy.signal.peak_widths(y, peaks, rel_height)[0] (prominences computed with wlen=None).
Vec peakWidths(const Vec& y, const std::vector<int>& peaks, double relHeight = 0.5);

}  // namespace lr::sig

namespace lr::processing {

// BASELINES: method key -> description, in the order of the Python dict.
const std::vector<std::pair<QString, QString>>& baselines();

Vec baselineAls(const Vec& y, double lam = 1e5, double p = 0.01, int niter = 10);
Vec baselineArpls(const Vec& y, double lam = 1e5, double ratio = 1e-3, int niter = 50);
Vec baselinePolynomial(const Vec& x, const Vec& y, int order = 5, int niter = 100);
Vec baselineRollingBall(const Vec& y, double radius = 100);
Vec baselineSnip(const Vec& y, int iterations = 60);
// SNIP with the 4th-order clipping filter and a decreasing window: follows concave (fluorescence) backgrounds.
Vec baselineSnip4(const Vec& y, int iterations = 60);
// airPLS (Zhang 2010): weight 0 above the fit, exp(t|d|/|sum d-|) below.
Vec baselineAirpls(const Vec& y, double lam = 1e5, int niter = 20);
// The 532 nm channel's edge filter cuts the signal below this shift: the default lower end of the baseline fit,
// and the low end of the Measure plot's autoscale, the fluorescence check and the intensity correction.
inline constexpr double kBaselineFitMin = 150.0;
// strength 0-10 maps to the method's main parameter (larger = smoother/stiffer baseline). "none" or an
// empty method gives zeros; an unknown method throws lr::Error. Only x >= fitMin is fitted; below it the
// baseline equals the data (fitMin <= the smallest x fits everything, e.g. 0 for imported spectra).
Vec baseline(const Vec& x, const Vec& y, const QString& method = "als", double strength = 5.0,
             double fitMin = kBaselineFitMin);

// Fluorescence indicator: median SNIP-4th-order baseline over 150-3500 cm-1 divided by the strongest band
// above it (raw counts). ~0.01-0.05 for clear liquids, > 1 when fluorescence dominates (ZnS ~6). NaN if
// there are fewer than 64 points in the range or no band above the baseline.
double fluorescenceRatio(const Vec& x, const Vec& y, double* background = nullptr);
inline constexpr double kFluorescenceRatio = 1.0;          // mention fluorescence at or above this ratio...
inline constexpr double kFluorescenceCounts = 1000.0;      // ...when the background is also above this level

Vec despike(const Vec& y, double threshold = 8.0);                  // single-spectrum cosmic-ray removal
Vec despikeFrames(const std::vector<Vec>& frames, double k = 5.0);  // multi-scan removal, returns the mean
Vec smooth(const Vec& y, int window = 9, int order = 3);            // Savitzky-Golay
// mode: "none", "max", "area", "band" (band: position in x units)
Vec normalize(const Vec& x, const Vec& y, const QString& mode = "none", std::optional<double> band = {});

struct Settings {                       // the settings dict of process()
    QString baseline = "none";
    double strength = 5.0;
    double fitMin = kBaselineFitMin;    // lower end of the baseline fit (cm-1), see baseline()
    bool despike = false;
    bool smooth = false;
    int smoothWindow = 9;
    int derivative = 0;
    int derivWindow = 11;
    QString normalize = "none";
    std::optional<double> band;
    // Intensity (instrument-response) correction: applied by the caller BEFORE process(), with the active
    // correction of the spectrum's instrument (response::corrected); process() itself ignores it.
    bool intensity = false;
    // From a dict with the Python keys (baseline, strength, fit_min, despike, smooth, smooth_window, derivative,
    // deriv_window, normalize, band, intensity); missing keys keep the defaults.
    static Settings fromMap(const QVariantMap& m);
};
// Apply the processing chain; returns (processed, baseline).
std::pair<Vec, Vec> process(const Vec& x, const Vec& y, const Settings& settings);

// Positions (x) of the most prominent peaks (prominence relative to max(y)), sorted by position.
Vec findPeaks(const Vec& x, const Vec& y, double prominence = 0.05, int maxPeaks = 8);

}  // namespace lr::processing
