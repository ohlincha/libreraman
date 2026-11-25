#pragma once
// Checks on a multi-frame acquisition (no GUI; used by gui/controller and the tests):
//
// * Laser damage (issue #9): the mean of the first third of the frames is compared with the mean of the last
//   third: shift of the strongest bands, loss of band height, rise of the background. Black samples (carbon,
//   LiFePO4, layered oxides) heat under the 532 nm beam and can change while they are measured; the averaged
//   spectrum then looks clean and is wrong. The Cora's laser power cannot be changed at all, so the result is
//   advice for the user (shorter exposure, slight defocus with the distance knob) plus a flag in the file
//   ("laser_damage" metadata).
// * Signal-to-noise (issue #10): S/N of one band (or the strongest) in a spectrum, and the rule that ends an
//   "acquire until S/N" run (target reached, time limit, sample changing).
//
// Default thresholds (DamageThresholds, SnrTarget) and their reasoning: knowledge/processing/acquisition-checks.md.
// They are NOT yet tested on the real Cora 5000 (only on the simulator and synthetic spectra).
//
// Spectra here are intensity (signal - background) on an ascending Raman-shift axis; the analysis uses
// kBandMin..kBandMax only (edge filter below 150 cm-1, junk at the end of the detector range).
#include <QString>
#include <cmath>
#include <limits>
#include <optional>
#include <vector>

#include "spectrum.h"

namespace lr::acq {

inline constexpr double kBandMin = 150.0, kBandMax = 3500.0;   // cm-1

// Robust noise (standard deviation per point) of y on x in kBandMin..kBandMax: median absolute second
// difference, scaled for white noise (1.4826 MAD / sqrt(6)). Bands and smooth backgrounds barely affect it.
// NaN with fewer than 16 points in the range.
double noise(const Vec& x, const Vec& y);

// ------------------------------------------------------------------------------------------- frames
// Frame store with bounded memory: frames are summed into at most kMaxBlocks blocks of equal size (merged in
// pairs when needed), enough to form the early and late thirds of thousands of frames.
class FrameStore {
public:
    static constexpr int kMaxBlocks = 240;
    void add(const Vec& frame);              // all frames must have the same length
    int count() const { return m_count; }
    bool empty() const { return m_count == 0; }
    Vec mean() const;                        // of all frames
    // Means of the first and last ~third of the frames (block-aligned; at least one frame each, and the two
    // never overlap). Empty with fewer than 2 frames. nEarly/nLate: frames in each.
    std::pair<Vec, Vec> thirds(int* nEarly = nullptr, int* nLate = nullptr) const;
    void clear();

private:
    struct Block {
        Vec sum;
        int n = 0;
    };
    std::vector<Block> m_blocks;
    int m_blockSize = 1, m_count = 0;
    Vec m_total;
};

// Raw signal frames of acquire() (AcquireOptions::frames, data order) as intensity frames in the order of the
// spectrum (sorted by shift): frame - sp.background, matched by calibration pixel (sp.pixels vs dataPixels =
// Instrument::pixelNumbers(channel)). Points without a match are 0.
std::vector<Vec> alignFrames(const Spectrum& sp, const std::vector<Vec>& frames, const Vec& dataPixels);

// ------------------------------------------------------------------------------------------- damage (#9)
struct DamageThresholds {
    double shift_cm = 2.5;         // a strong band moved by more than this (cm-1; ~1 detector pixel)
    double intensityLoss = 0.15;   // the strongest bands lost more than 15 % of their summed height
    double backgroundRise = 0.20;  // the background rose by more than 20 % of max(early background, strongest band)
    int bands = 3;                 // number of strongest bands compared
    double minBandSnr = 10.0;      // bands below this height/noise are not used (their positions are too noisy)
    double window_cm = 15.0;       // a band is looked for within ± this of its early position
    double noiseSigmas = 4.0;      // a change must also exceed this many noise standard deviations
    double widthFraction = 0.03;   // ...and a shift this fraction of the band's FWHM (broad bands wander)
};

struct BandChange {
    double early = 0, late = 0;              // positions (cm-1)
    double heightEarly = 0, heightLate = 0;  // above the background (counts per frame)
};

struct DamageReport {
    bool valid = false;            // two spectra with at least one usable band were compared
    bool changing = false;         // shifted || faded || background
    bool shifted = false, faded = false, background = false;
    int framesEarly = 0, framesLate = 0;
    std::vector<BandChange> bands; // strongest first
    double maxShift = 0;           // signed shift of the band that moved most (cm-1)
    double maxShiftAt = std::numeric_limits<double>::quiet_NaN();   // its early position
    double intensityChange = 0;    // summed band height late/early - 1 (-0.2 = lost 20 %)
    double backgroundChange = 0;   // (late - early background) / max(early background, strongest band height)
    // "band 1582 → 1577 cm⁻¹ · bands −22 % · background +35 %" (the metrics that crossed a threshold are listed
    // first; all three are given). Empty if !valid.
    QString summary() const;
};

// The advice shown on the Measure dashboard when DamageReport::changing.
QString damageAdvice();

// Compares two spectra on the same ascending axis (early, late: means of frame groups). noiseEarly/noiseLate:
// noise per point of each (NaN = estimate with noise()).
DamageReport compareSpectra(const Vec& x, const Vec& early, const Vec& late, const DamageThresholds& t = {},
                            double noiseEarly = std::numeric_limits<double>::quiet_NaN(),
                            double noiseLate = std::numeric_limits<double>::quiet_NaN());
// First third vs last third of the frames (intensity frames on axis x). Invalid with fewer than 2 frames.
DamageReport checkFrames(const Vec& x, const FrameStore& frames, const DamageThresholds& t = {});
DamageReport checkFrames(const Vec& x, const std::vector<Vec>& frames, const DamageThresholds& t = {});
// File metadata: "laser_damage" = "suspected" | "none detected" and "laser_damage_check" = the summary with the
// frames compared. Nothing for an invalid report.
void setDamageMeta(Spectrum& sp, const DamageReport& r);

// ------------------------------------------------------------------------------------------- S/N (#10)
struct Snr {
    double snr = std::numeric_limits<double>::quiet_NaN();
    double band = std::numeric_limits<double>::quiet_NaN();     // position of the band used (cm-1)
    double height = std::numeric_limits<double>::quiet_NaN();   // above the local background
    double noise = std::numeric_limits<double>::quiet_NaN();
    bool valid() const { return std::isfinite(snr); }
};
// S/N = band height above a SNIP background / noise(). band: the maximum within ± window of this position;
// none: the strongest band in kBandMin..kBandMax.
Snr estimateSnr(const Vec& x, const Vec& y, std::optional<double> band = {}, double window = 15.0);

struct SnrTarget {
    double target = 100.0;         // stop when S/N >= target
    std::optional<double> band;    // none = strongest band
    double maxSeconds = 600.0;     // time limit (laser-on accumulation)
    int minFrames = 3;             // never stop on S/N before this many frames (a single noisy frame can lie)
    bool stopOnDamage = true;      // stop when checkFrames() reports a changing sample...
    int damageMinFrames = 6;       // ...once there are at least this many frames (2 per third)
};

enum class Stop { Continue, Reached, TimeLimit, Damage };
QString stopText(Stop s);          // "target S/N reached", "time limit", "sample changing under the laser", ""

// The rule after each frame: Damage first, then Reached, then TimeLimit (the next frame would end after
// maxSeconds: elapsed + frameSeconds > maxSeconds).
Stop snrStop(const SnrTarget& t, double snr, int frames, double elapsedSeconds, double frameSeconds, bool damage);
// Frames expected for the target (S/N grows as the square root of the number of frames); -1 if unknown.
int framesNeeded(double snr, int frames, double target);

// One "acquire until S/N" run: add the frames (intensity on a fixed axis) as they come; each add() returns the
// decision. The damage check runs after every frame once damageMinFrames are in (also with stopOnDamage off:
// the report then only goes into the file).
class SnrRun {
public:
    explicit SnrRun(SnrTarget target, DamageThresholds damage = {});
    Stop add(const Vec& x, const Vec& intensity, double elapsedSeconds, double frameSeconds);
    int frames() const { return m_frames.count(); }
    Vec mean() const { return m_frames.mean(); }
    const Snr& snr() const { return m_snr; }
    const DamageReport& damage() const { return m_damage; }
    const SnrTarget& target() const { return m_target; }
    Stop stop() const { return m_stop; }
    const FrameStore& store() const { return m_frames; }

private:
    SnrTarget m_target;
    DamageThresholds m_thresholds;
    FrameStore m_frames;
    Vec m_x;
    Snr m_snr;
    DamageReport m_damage;
    Stop m_stop = Stop::Continue;
};

// Progress of an "acquire until S/N" run (Controller::snrProgress).
struct SnrProgress {
    int frames = 0;
    double snr = std::numeric_limits<double>::quiet_NaN();
    double band = std::numeric_limits<double>::quiet_NaN();
    double target = 0;
    double elapsed = 0, maxSeconds = 0;
    int framesNeeded = -1;
    bool damage = false;
    bool finished = false;
    QString stopText;              // when finished
};

}  // namespace lr::acq
