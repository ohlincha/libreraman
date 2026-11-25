#pragma once
// Repeated measurements of one vial (issue #11). The Cora takes one GC vial and has no stage, so a powder is
// sampled by shaking or rotating the vial between N spectra. analyse() gives the mean spectrum and its spread,
// band results (position, height, ratio to the strongest band) as mean ± sd over the included spectra, and
// flags outliers (e.g. a burnt spot) by the shape of each spectrum; the user may exclude spectra.
//
// Outliers: each spectrum is scaled to unit norm over kBandMin..kBandMax (packing and focus change the overall
// intensity of a powder, so only the shape counts) and compared with the point-wise median of all spectra:
// distance = |s_i − median| / |median|. A spectrum is flagged when its modified z-score
// 0.6745 (d_i − median d) / MAD(d) exceeds Options::outlierZ (3.5, Iglewicz & Hoaglin) AND d_i exceeds
// Options::minDistance (0.10: differences under 10 % of the spectrum are noise or packing, not a different
// sample spot). Needs at least 3 spectra. Untested on the real Cora 5000; see
// knowledge/processing/acquisition-checks.md.
#include <QString>
#include <optional>
#include <vector>

#include "spectrum.h"

namespace lr::repeats {

struct Options {
    double outlierZ = 3.5;
    double minDistance = 0.10;
    int maxBands = 6;              // strongest bands of the mean spectrum reported
    double window_cm = 12.0;       // each spectrum's band is looked for within ± this of the mean's position
    double minProminence = 0.05;   // band picking on the mean: prominence relative to the strongest band
};

struct MeanSd {
    double mean = 0, sd = 0;       // sd: sample standard deviation (n − 1); 0 for n = 1
    int n = 0;
    QString text(int decimals) const;   // "1582.3 ± 0.4" (just the mean when n < 2)
};

struct BandStat {
    double nominal = 0;            // position in the mean spectrum (cm-1)
    MeanSd position, height, ratio;   // ratio: height / height of the reference band (same spectrum)
    std::vector<double> positions, heights;   // per input spectrum (NaN if not found)
};

struct Result {
    Vec x;                         // axis of the first spectrum
    Vec mean, sd;                  // point-wise over the included spectra (sd 0 with one)
    std::vector<double> distance;  // per input spectrum (see the top of this file); NaN if not computed
    std::vector<bool> outlier;     // flagged
    std::vector<bool> included;    // = not excluded
    int nIncluded = 0;
    std::vector<BandStat> bands;   // ascending position
    int reference = -1;            // index into bands of the strongest band (ratios are relative to it)
    MeanSd totalIntensity;         // mean ± sd of each spectrum's summed intensity in the band range (packing)
};

// spectra: intensity on (nearly) the same axis; others are interpolated onto the first one's axis.
// exclude: per spectrum (missing entries = included). With nothing included, mean/sd/bands are empty.
Result analyse(const std::vector<Spectrum>& spectra, const std::vector<bool>& exclude = {}, const Options& o = {});

// The mean as a spectrum for saving: intensity = mean, signal/background = means of the included spectra
// (when all have them), settings of the first; meta: sample, "repeats" ("4 of 5 (excluded: 3)"), "repeat_files",
// one "band_<position>" line per band and "outliers".
Spectrum meanSpectrum(const std::vector<Spectrum>& spectra, const Result& r);

// Table of the whole set: "# key: value" header with the band results (mean ± sd), then the columns
// raman_shift_cm1, mean, sd, and one intensity column per spectrum ("r1", "r2 (excluded)", ...). Throws
// lr::Error if the file cannot be written.
void writeSummary(const QString& path, const std::vector<Spectrum>& spectra, const Result& r, const QString& sample);

}  // namespace lr::repeats
