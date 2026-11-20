#pragma once
// Reading and writing spectra (Python: fileio.py).
//
// read(path) -> list of DataSpectrum. Recognised:
// * LibreRamen CSV (single spectrum: raman_shift_cm1,intensity,signal,background; series: raman_shift_cm1,t=...)
// * Vendor (Cora/BaySpec) CSV and TXT ("Key:,value" header, then shift and intensity columns)
// * Galactic SPC (single or multi-file, evenly spaced or with an x array)
// * JCAMP-DX (XYDATA=(X++(Y..Y)) and XYPOINTS)
// * Any text file with two numeric columns
// writeCsv(), writeJcamp() for one spectrum; writeTable() for several on a common axis.
// Errors (unreadable or unrecognised files) are thrown as lr::Error with the Python messages.
#include <QString>
#include <utility>
#include <vector>

#include "spectrum.h"

namespace lr::fileio {

struct DataSpectrum {
    QString name;
    Vec x;                        // Raman shift, cm^-1 (ascending after sorted())
    Vec y;                        // intensity (background-subtracted where known)
    Meta meta;                    // ordered key/value pairs
    QString source;               // file it was read from
    Vec signal;                   // raw laser frames (LibreRamen files); empty = not available
    Vec background;

    DataSpectrum& sorted();       // sort all arrays by x
    QString metaValue(const QString& key, const QString& fallback = {}) const;
    void setMeta(const QString& key, const QString& value);      // replaces in place or appends
};

std::vector<DataSpectrum> read(const QString& path);

void writeCsv(const QString& path, const DataSpectrum& sp);
void writeJcamp(const QString& path, const DataSpectrum& sp);     // JCAMP-DX 5.01, XYPOINTS
// Several spectra resampled (linear interpolation) onto a common axis: overlap range, `step` cm^-1.
void writeTable(const QString& path, const std::vector<DataSpectrum>& spectra, double step = 1.0);
// An acquisition Spectrum as a DataSpectrum (name: given, else meta "sample", else "spectrum").
DataSpectrum fromSpectrum(const Spectrum& sp, const QString& name = {});

// Python _selftest: read every .csv/.txt/.spc/.jdx/.dx file under folder; returns (number read,
// [(path, error)]).
std::pair<int, std::vector<std::pair<QString, QString>>> selftest(const QString& folder);

}  // namespace lr::fileio
