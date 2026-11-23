#pragma once
// Spectral library (SPECIFICATION 5.5): one SQLite file (Qt Sql, QSQLITE driver) that may be shared on a
// network drive. Every entry keeps its source and licence. Matching is in match.h.
//
// Schema (PRAGMA user_version = schema version; migrations run on open, see library.cpp):
//   info(key PRIMARY KEY, value)                        creator, created, schema history
//   entries(id, name, formula, cas, class, tags, source, source_id, licence, laser_nm, conditions,
//           operator, notes, created, modified, provenance JSON)
//   spectra(id, entry_id -> entries ON DELETE CASCADE, label, processed, n, xmin, xmax, x BLOB, y BLOB)
//           x/y: n float64 values, little-endian
//   peaks(id, entry_id -> entries ON DELETE CASCADE, position, intensity, width, label)
//
// Sources: own (measured by the user), rruff, literature (band lists), computed (clearly marked), vendor
// (vendor factory libraries for the owner's own use; never redistributed). LibreRamen ships no third-party
// spectra; the demo library (createDemo) is built from the simulator's own recordings.
//
// Files on a network drive: busy timeout, rollback journal instead of WAL (WAL needs shared memory that
// network file systems do not provide), and a read-only open mode for shared reference libraries.
// Errors are thrown as lr::Error with user-readable messages. A Library is used from one thread.
#include <QDateTime>
#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <memory>
#include <optional>
#include <vector>

#include "fileio.h"
#include "spectrum.h"

namespace lr::library {

enum class Source { Own, Rruff, Literature, Computed, Vendor };
const std::vector<Source>& allSources();
QString sourceKey(Source s);                     // "own", "rruff", "literature", "computed", "vendor"
QString sourceLabel(Source s);                   // "Own", "RRUFF", "Literature", "Computed", "Vendor"
Source sourceFromKey(const QString& key);        // key or label, case-insensitive; throws lr::Error

struct SpectrumData {
    qint64 id = 0;                               // 0 until stored
    QString label;                               // e.g. "raw", "processed", "RRUFF raw"
    bool processed = false;                      // baseline-corrected/normalised by the source
    Vec x, y;                                    // Raman shift (cm^-1, ascending), intensity
};

struct PeakEntry {
    double position = 0;                         // cm^-1
    double intensity = 0;                        // relative (0..1) or 0 = unknown
    double width = 0;                            // FWHM cm^-1 or 0 = unknown
    QString label;                               // assignment (optional)
};

struct Entry {
    qint64 id = 0;                               // 0 until stored
    QString name, formula, cas;
    QString cls;                                 // class (e.g. "solvent", "mineral", "polymer")
    QStringList tags;
    Source source = Source::Own;
    QString sourceId;                            // e.g. RRUFF ID, DOI, file name
    QString licence;
    double laserNm = 0;                          // 0 = unknown / not applicable
    QString conditions, op, notes;               // op: operator
    QDateTime created, modified;                 // set by the library
    QJsonObject provenance;                      // free-form (file, instrument, processing, import, ...)
    std::vector<SpectrumData> spectra;           // loaded only with withData = true
    std::vector<PeakEntry> peaks;
    int spectrumCount = 0, peakCount = 0;        // filled by get()/find() also without data

    // Spectrum used for matching/plotting: the first processed one, else the first one; nullptr if none.
    const SpectrumData* preferredSpectrum() const;
};

struct Filter {
    QString text;                                // substring of name, formula, CAS, class, tags, notes, source ID
    QString cls;                                 // exact class (case-insensitive); empty = all
    std::optional<Source> source;
    double laserNm = 0;                          // > 0: entries within laserTol of it
    double laserTol = 10;
    bool includeUnknownLaser = true;             // with laserNm > 0: also entries without a laser
    bool spectraOnly = false;                    // only entries with at least one spectrum
    int limit = 0;                               // 0 = all
};

enum class OpenMode { ReadWrite, ReadOnly };

class Library {
public:
    static constexpr int kSchemaVersion = 1;

    Library();
    ~Library();
    Library(const Library&) = delete;
    Library& operator=(const Library&) = delete;

    // Opens (and with ReadWrite and create, creates) a library file and migrates it to kSchemaVersion.
    // Throws lr::Error: missing file, not a library, newer schema, read-only file needing a migration.
    void open(const QString& path, OpenMode mode = OpenMode::ReadWrite, bool create = true);
    void close();
    bool isOpen() const;
    bool readOnly() const;
    QString path() const;
    bool onNetwork() const;                      // detected at open (UNC path, nfs/cifs/smb/afp/... mount)
    QString journalMode() const;                 // "wal" locally, "delete" on a network drive
    int schemaVersion() const;                   // of the open file (== kSchemaVersion after open)
    static bool isNetworkPath(const QString& path);

    // ---- entries
    qint64 add(Entry e);                         // stores spectra and peaks too; returns the new id
    void update(const Entry& e);                 // metadata; spectra/peaks replaced unless keepData
    void update(const Entry& e, bool keepData);
    void remove(qint64 id);
    void remove(const std::vector<qint64>& ids);
    std::optional<Entry> get(qint64 id, bool withData = true) const;
    std::vector<Entry> find(const Filter& f = {}, bool withData = false) const;   // sorted by name
    int count() const;
    QStringList classes() const;                 // distinct non-empty classes, sorted

    // ---- import (each returns the ids of the new entries; one transaction per call)
    // "Add to library": the spectrum (raw intensity, x = shift) with metadata from `meta` (name, formula,
    // CAS, notes, conditions, operator ...). Empty fields are filled from the spectrum's metadata (sample,
    // laser_nm, operator, settings) and its peaks are picked (peaks::pick on the SNIP-corrected spectrum).
    qint64 addSpectrum(const Spectrum& sp, Entry meta = {});
    qint64 addSpectrum(const fileio::DataSpectrum& sp, Entry meta = {});
    // Any file fileio::read() understands, or a RRUFF text file; `meta` supplies defaults (source, licence...).
    std::vector<qint64> importFile(const QString& path, const Entry& meta = {});
    // All RRUFF *.txt files in a folder (not recursive); unreadable files are reported in errors.
    std::vector<qint64> importRruffFolder(const QString& dir, QStringList* errors = nullptr);
    // Literature band list CSV (see readBandList); entries get source Literature unless meta says otherwise.
    std::vector<qint64> importBandList(const QString& path, const Entry& meta = {});

    // ---- export
    // One entry to a file: .jdx/.dx JCAMP-DX, .txt/.csv LibreRamen CSV (spectrum + metadata incl. source and
    // licence); entries without spectra are written as a band list CSV.
    void exportEntry(qint64 id, const QString& path) const;
    // Copies entries into another library file (created if needed); returns the number copied.
    int exportTo(const std::vector<qint64>& ids, const QString& otherPath) const;

    // ---- info table
    QString info(const QString& key) const;
    void setInfo(const QString& key, const QString& value);

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

// ---- formats
struct RruffSpectrum {
    QString name, rruffId, formula, locality, owner, sourceName, description, status, url;
    double laserNm = 0;
    bool processed = false;                      // "Processed" in the file name or header
    Vec x, y;
    Meta header;                                 // all ##KEY=value lines in order
};
bool isRruffFile(const QString& path);           // ##NAMES= / ##RRUFFID= header
RruffSpectrum readRruff(const QString& path);    // throws lr::Error
Entry entryFromRruff(const RruffSpectrum& r);    // source Rruff, licence/citation note, provenance

// Band list CSV: one entry per line, "name,band;band;..." or "name,band,band,..." (bands in cm^-1, an optional
// strength after a colon: 1001:s, 1031:0.3; w/m/s/vs map to 0.25/0.5/1/1). Lines starting with # are
// comments; a header line starting with "name" is skipped. Optional "# class: ..." / "# source: ..." /
// "# licence: ..." comment lines set defaults for the following entries.
struct BandListEntry {
    QString name, cls, reference, licence;
    std::vector<PeakEntry> bands;
};
std::vector<BandListEntry> readBandList(const QString& path);
void writeBandList(const QString& path, const std::vector<BandListEntry>& entries);

// The simulator's five recorded spectra (532 nm, x from the factory calibration) as own entries, plus the
// reference standards of standards.h as literature band lists. For tests and the demo.
void createDemo(Library& lib, bool withStandards = true);

// Reference entries shipped with LibreRamen (resources/reference/*.json): literature band positions with
// citations, as peak lists (source Literature, tag "reference", provenance "reference_key"). Adds the ones not
// yet in `lib` and records info "reference_version"; returns how many were added. Throws lr::Error if `lib` is
// read-only.
int addReferenceEntries(Library& lib);
int referenceVersion();                         // of the shipped set (info "reference_version" in a library)
int referenceCount();                           // entries in the shipped set
// The simulator spectrum of `sample` (simulatorData() name) on the factory shift axis, counts per second.
std::pair<Vec, Vec> simulatorSpectrum(const QString& sample);

// float64 little-endian blob <-> vector
QByteArray toBlob(const Vec& v);
Vec fromBlob(const QByteArray& b);

}  // namespace lr::library
