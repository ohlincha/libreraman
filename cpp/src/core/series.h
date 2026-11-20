#pragma once
// Time series.
//  * runSeries():     series.py run_series (command line): one spectrum every interval, one table file
//                     rewritten after each spectrum.
//  * runTimeSeries(): what the Python GUI controller does (gui/controller.py _series, _series_fastest):
//                     start delay, count or duration, pause while disarmed (continues after ARM) or while
//                     the caller's pause flag is set, "laser stays on" fast mode, one CSV per spectrum plus
//                     series.csv, series.jdx and series.spc (SeriesSettings::files) in a new folder.
//
// Files of runTimeSeries: every spectrum is saved at once as its own CSV (full data: signal, background,
// metadata) in the worker thread; these files are the crash-safe ground truth. The whole-series files
// (series.csv: table, series.jdx: JCAMP-DX LINK file, series.spc: multi-file SPC; layouts in fileio.h) are
// rewritten atomically (QSaveFile: write a temporary file, then rename) by a writer thread, so slow disks
// (fsync of a QSaveFile commit: ~0.1-0.3 s on a hard disk) never delay the acquisition. Each spectrum is
// formatted once (cached rows/blocks); a rewrite only concatenates. Rewrites are coalesced: spectra arriving
// during a rewrite go into the next one, and the next rewrite starts no earlier than twice the duration of
// the previous one after it (the writer is busy at most ~1/3 of the time; long fast series have files that
// lag by a few spectra). When the series ends (finished, stopped or failed) the files are written once more
// with every spectrum before runTimeSeries returns.
//
// Thread safety: run in a worker thread; the callbacks are called from that thread. stop/pause may be set
// from any thread.
#include <QDateTime>
#include <QString>
#include <atomic>
#include <functional>
#include <vector>

#include "instrument.h"

namespace lr {

struct SeriesColumn {
    double elapsed_s;                         // since the start of the series
    Vec intensity;
};

// background: "each" (a background with every spectrum) or "once" (one dark at the start, laser off).
// Stops between spectra when *stop is set (returns the columns so far). on_spectrum(k, elapsed_s, sp).
std::vector<SeriesColumn> runSeries(Instrument& inst, const QString& path, double exposure_s, double interval_s,
                                    int count, int averages = 1, const QString& background = "each",
                                    double power_mw = 0.0, const std::atomic<bool>* stop = nullptr,
                                    const std::function<void(int, double, const Spectrum&)>& onSpectrum = {});

struct SeriesSettings {
    QString sample = "sample";
    QString folder;                           // parent folder; the series gets <sample>_series_<date>_<time>/
    double exposure_s = 1.0;
    int averages = 1;
    QString background = "each";              // "each" | "once" | "none"
    double prebleach_s = 0.0;
    double interval_s = 10.0;
    bool stopByCount = true;                  // false: stop by duration
    int count = 10;
    double duration_s = 60.0;
    double start_delay_s = 0.0;
    bool laserStaysOn = false;                // "as fast as possible": laser on throughout, background once
    double power_mw = 0.0;                    // 1064 nm only; 0 = default
    struct Files {                            // whole-series files (the per-spectrum CSVs are always written)
        bool csv = true;                      // series.csv (table: shift + one intensity column per spectrum)
        bool jdx = true;                      // series.jdx (JCAMP-DX 5.01 LINK file, one block per spectrum)
        bool spc = true;                      // series.spc (Galactic SPC multi-file, z = elapsed s)
    } files;
};

struct SeriesProgress {
    int n = 0;                                // spectra taken
    int total = 0;                            // planned (fast mode by duration: n)
    double elapsed_s = 0.0;                   // since the (delayed) start; negative during the delay
    double next_in_s = -1.0;                  // until the next spectrum; -1 = n/a
    QDateTime end;                            // planned time of the last spectrum (invalid if n/a)
    bool paused = false;                      // waiting for ARM / closed compartment / pause flag
    bool done = false;
};

struct SeriesCallbacks {
    std::function<void(const Spectrum&)> spectrum;   // each saved spectrum (meta "file" not set)
    std::function<void(const SeriesProgress&)> progress;
    LogFn log;
};

// Returns the series folder. Throws Error (e.g. NotArmed in fast mode, hardware errors); Stop is a normal end.
QString runTimeSeries(Instrument& inst, const SeriesSettings& s, const std::atomic<bool>& stop,
                      const SeriesCallbacks& cb = {}, const std::atomic<bool>* pause = nullptr);

// File-name-safe sample name (controller _safe): alphanumerics and "-_." kept, others "_", at most 60.
QString safeName(const QString& name);

}  // namespace lr
