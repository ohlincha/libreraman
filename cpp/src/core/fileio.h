#pragma once
// Reading and writing spectra (Python: fileio.py).
//
// read(path) -> list of DataSpectrum. Recognised:
// * LibreRaman CSV (single spectrum: raman_shift_cm1,intensity,signal,background; series: raman_shift_cm1,t=...)
// * Vendor (Cora/BaySpec) CSV and TXT ("Key:,value" header, then shift and intensity columns)
// * Galactic SPC (single or multi-file, evenly spaced or with an x array)
// * JCAMP-DX (XYDATA=(X++(Y..Y)) and XYPOINTS)
// * Any text file with two numeric columns
// writeCsv(), writeJcamp() for one spectrum; writeTable() for several on a common axis; writeJcampMulti(),
// writeSpcMulti() and MultiEncoder for several spectra (a time series) in one file.
// Errors (unreadable or unrecognised files) are thrown as lr::Error with the Python messages.
//
// Several spectra in one file (C++ only; docs/SPECIFICATION.md section 7):
// * JCAMP-DX 5.01 compound file: a LINK block (##DATA TYPE=LINK, ##BLOCKS=n, file metadata as ##$ labels)
//   enclosing one RAMAN SPECTRUM block per spectrum (##BLOCK_ID=k, ##TITLE=name, ##LONGDATE= from meta
//   "acquired" when it is an ISO time, every meta key as ##$KEY (upper case, spaces -> "_"), ##XUNITS=1/CM,
//   ##YUNITS=COUNTS (ARBITRARY UNITS for processed data, i.e. meta "processing"), ##FIRSTX/LASTX/NPOINTS,
//   AFFN ##XYPOINTS=(XY..XY) with x and y to 4 decimals (the Raman shift axis is not evenly spaced, so not
//   X++(Y..Y)), ##END=), then the LINK block's ##END=.
//   Reading such a file (recognised by ##BLOCKS=) gives one DataSpectrum per block with data; ##$KEY labels
//   become meta "key" (lower case), the LINK block's ##$ labels are inherited by every block. Files with
//   one block are read as before (all labels as upper-case meta keys).
// * Galactic SPC "new format" (0x4B), multi-file: 512-byte header with TMULTI and TXVALS (one float32 x array
//   shared by all sub-files, when all spectra have the same x) or TXVALS|TXYXYS (an x array in every
//   sub-file, no directory: fnpts = 0), fexper 11 (Raman), fexp 0x80 (float32 y), fxtype 13 (Raman shift,
//   cm-1), fytype 4 (counts; 0 = arbitrary for processed data), fztype 4 (seconds) with TORDRD when every
//   spectrum has an elapsed time (meta "series_elapsed_s" or "time" = "t=..s"), else z = index (fztype 0,
//   evenly spaced); fzinc = mean z step; fdate from the first spectrum; fcmnt "<title>, LibreRaman"; a log
//   block (flogoff; 64-byte LOGSTC, then text) with the file metadata as "key=value" lines. Every sub-file:
//   32-byte header (subexp 0x80, subindx, subtime = z, subnext = next z, subnpts for TXYXYS), then the x
//   array (TXYXYS only) and y, float32.
//   Reading: when the file has TORDRD/TRANDM, fzinc != 0 or non-zero sub-file times, each sub-file's z is
//   surfaced as meta "z" and, for time axes (fztype seconds, minutes, hours, ms), "series_elapsed_s"
//   (seconds; name "<file> t=<z>s"); the log text's key=value lines become meta.
#include <QByteArray>
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
    Vec signal;                   // raw laser frames (LibreRaman files); empty = not available
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

// ---- several spectra in one file (layout: see the top of this file)
// Builds a multi-spectrum file incrementally: add() encodes each spectrum once; bytes() assembles the
// complete file (the headers depend on the number of spectra). The series runner keeps one per format and
// rewrites the file as the series grows.
class MultiEncoder {
public:
    enum class Format { Jcamp, Spc };
    MultiEncoder(Format format, const QString& title, const Meta& fileMeta = {});
    void add(const DataSpectrum& sp);         // throws Error for an empty spectrum
    QByteArray bytes() const;                 // throws Error when there are no spectra
    void save(const QString& path) const;     // bytes() written atomically (QSaveFile); throws Error
    int count() const { return int(m_items.size()); }
    qint64 size() const { return m_size; }    // sum of the encoded spectra (bytes)
    Format format() const { return m_format; }

private:
    struct Item {
        QByteArray data;                      // JCAMP: the whole block; SPC: y as float32
        QByteArray xs;                        // SPC: x as float32
        quint32 npts = 0;
        double first = 0, last = 0;
        double z = 0;
        bool hasTime = false;
        bool counts = true;                   // not processed (meta "processing" absent)
        QDateTime acquired;                   // invalid = unknown
    };
    Format m_format;
    QString m_title;
    Meta m_meta;
    std::vector<Item> m_items;
    bool m_commonX = true;                    // SPC: every spectrum has the first one's x (TXVALS)
    qint64 m_size = 0;
};

void writeJcampMulti(const QString& path, const std::vector<DataSpectrum>& spectra, const QString& title = {},
                     const Meta& fileMeta = {});
void writeSpcMulti(const QString& path, const std::vector<DataSpectrum>& spectra, const QString& title = {},
                   const Meta& fileMeta = {});

// Python _selftest: read every .csv/.txt/.spc/.jdx/.dx file under folder; returns (number read,
// [(path, error)]).
std::pair<int, std::vector<std::pair<QString, QString>>> selftest(const QString& folder);

}  // namespace lr::fileio
