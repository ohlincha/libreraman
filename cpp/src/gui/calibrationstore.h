#pragma once
// Files of the Calibration page (no widgets): calibration history, instrument log, quick-check numbers.
//
// All files live in calib::configDir() (shared with the Python version):
//   calibration_<serial>_<ch>.json                   the ACTIVE calibration, read at connect by C++ and Python
//                                                    (calib::load / calibrate.load). Absent = factory.
//   calibration_<serial>_<ch>_<yyyyMMdd-HHmmss>.json history copy of every accepted calibration (same JSON
//                                                    as calib::save plus "kind", "spectra", "laser_nm_equiv").
//                                                    Python ignores these (it only opens the exact name above).
//   instrument-log_<serial>.csv                      instrument log: one row per calibration, quick check,
//                                                    activation or revert (dark level, calibration drift).
// Accept = write a history copy and copy it over the active file. Make active = copy a history file over the
// active file. Revert to factory = remove the active file (archived first as a history copy if no identical
// copy exists, e.g. when it was written by the Python CLI).
#include <QDateTime>
#include <QString>
#include <QStringList>
#include <QtNumeric>
#include <optional>
#include <vector>

#include "core/calibrate.h"

namespace lr::gui::calstore {

QString activePath(const QString& serial, int channel);
QString historyPath(const QString& serial, int channel, const QDateTime& t);   // unique (adds _2, _3 ...)

struct HistoryEntry {
    QString path;
    QDateTime time;               // from the file name (history copy) or the file's date
    QString date;                 // "date" of the JSON
    int channel = 0;
    QStringList standards;
    int bands = 0;
    double rms = 0.0;
    int degree = 0;
    QString kind;                 // "polynomial" / "offset"
    bool active = false;          // identical to the active file
    bool activeOnly = false;      // the active file itself (no history copy)
};

// History of one instrument and channel, newest first. If the active file has no identical history copy it
// is listed as well (activeOnly).
std::vector<HistoryEntry> history(const QString& serial, int channel);
std::optional<HistoryEntry> readEntry(const QString& path);

// Writes the result as a history copy and as the active file; returns the history path.
// kind: "polynomial" / "offset"; spectra: files the calibration was made from (provenance).
QString accept(const calib::Result& r, int channel, const QString& serial, const QString& kind,
               const QStringList& spectra = {}, double laserNmEquiv = 0.0);
void makeActive(const QString& historyFile, int channel, const QString& serial);
// Removes the active file (archiving it first if needed). Returns false if there was none.
bool revertToFactory(int channel, const QString& serial);

// ---- instrument log
struct LogEntry {
    QDateTime time = QDateTime::currentDateTime();
    QString serial;
    int channel = 0;
    QString event;                // "calibration" | "quick check" | "activated" | "factory"
    QStringList standards;
    int bands = 0;
    double devMean = qQNaN();     // mean (calibration in use - reference) over the matched bands, cm-1
    double devRms = qQNaN();      // RMS of the same
    double fitRms = qQNaN();      // RMS of the new fit (calibration)
    int degree = -1;
    double dark = qQNaN();        // mean background (dark) level, counts per frame
    double exposure = qQNaN();    // s (the dark level depends on it)
    QString note;
};
QString logPath(const QString& serial);
void appendLog(const LogEntry& e);
std::vector<LogEntry> readLog(const QString& serial);   // oldest first

// ---- deviations of the calibration in use (quick check, drift)
struct Deviation {
    QString standard;
    double reference = 0, measured = 0, pixel = 0;   // measured = current calibration at the peak pixel
    int strength = 0;
    double deviation() const { return measured - reference; }
};
struct CheckResult {
    std::vector<Deviation> bands;
    double mean = 0, rms = 0, maxAbs = 0;
};
CheckResult check(const std::vector<std::pair<QString, std::vector<calib::Match>>>& matches,
                  const calib::ShiftFn& current);

// Days since a calibration date ("yyyy-MM-dd HH:mm" or ISO); nullopt if unreadable.
std::optional<qint64> daysSince(const QString& date, const QDateTime& now = QDateTime::currentDateTime());

// Laser wavelength (nm) that turns a constant shift offset into the same axis: 1e7/l' = 1e7/l + offset.
double equivalentLaserNm(double laserNm, double offset);

}  // namespace lr::gui::calstore
