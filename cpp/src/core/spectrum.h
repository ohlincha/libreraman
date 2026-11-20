#pragma once
// One measured spectrum (Python: spectrum.Spectrum).
#include <QDateTime>
#include <QString>
#include <utility>
#include <vector>

namespace lr {

using Vec = std::vector<double>;
using Meta = std::vector<std::pair<QString, QString>>;   // ordered key/value pairs (written as "# key: value")

struct Spectrum {
    int channel = 532;                 // 532 or 1064
    double exposure_s = 1.0;
    int averages = 1;
    Vec shift;                         // Raman shift (cm^-1), ascending
    Vec intensity;                     // signal - background (counts per frame)
    Vec signal;                        // may be empty (e.g. read from a file)
    Vec background;
    Vec pixels;                        // calibration pixel for each point (may be empty)
    QDateTime t = QDateTime::currentDateTime();
    Meta meta;

    QString metaValue(const QString& key, const QString& fallback = {}) const;
    void setMeta(const QString& key, const QString& value);     // replaces or appends
    void saveCsv(const QString& path) const;                    // same format as the Python version
};

QString iso(const QDateTime& t);      // 2026-11-20T18:21:07+01:00

}  // namespace lr
