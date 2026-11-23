#pragma once
// Intensity (instrument-response) correction (issue #16).
//
// The detector, grating and filters weight every wavelength differently, so band ratios across the spectrum
// (2D/G, band areas far apart, anti-Stokes/Stokes) depend on the instrument. A reference with a known relative
// emission curve T(shift) is measured: M(shift) = R(shift) T(shift). The correction factor F = T / M (smoothed,
// normalised to median 1 over its valid range) turns any spectrum S measured the same way into R-free
// relative intensities: S_corrected = S * F. Pixel widths cancel because reference and sample share pixels.
//
// References:
//   * NIST SRM 2242 (luminescent glass for 532 nm): certified 5th-order polynomial in x = shift / 1000 cm-1.
//     The vendor's BS_Device_Settings.ini carries the same coefficients (NIST_Coeff_00..05,
//     ChannelCal::nist_coeff), which are therefore the reference's true curve, not this instrument's response.
//   * A tungsten-halogen lamp of known colour temperature: Planck photon flux per unit wavenumber,
//     N ~ v^2 / (exp(c2 v / T) - 1), v the absolute wavenumber (approximation: tungsten is not a perfect
//     blackbody; good to a few % over the Stokes range).
//   * A table (shift cm-1, relative intensity) from a file, e.g. a certificate.
//
// Files (calib::configDir(), like the wavelength calibration): response_<serial>_<ch>.json is the active
// correction; every accepted one is also kept as response_<serial>_<ch>_<yyyyMMdd-HHmmss>.json.
// Raw data are never changed: the correction is a processing option.
#include <QString>
#include <QStringList>
#include <limits>
#include <memory>
#include <optional>
#include <vector>

#include "processing.h"
#include "spectrum.h"

namespace lr::response {

inline constexpr double kMinShift = processing::kBaselineFitMin;   // the edge filter cuts the signal below this
inline constexpr double kMinLevel = 0.05;       // valid range: smoothed reference above this fraction of its max
inline constexpr double kMinSnr = 50.0;         // reference must be at least this far above its noise
inline constexpr double kMaxDynamic = 30.0;     // larger max/min factor ratios get a warning
inline constexpr double kSaturated = 65000.0;   // raw counts

struct Reference {
    enum class Kind { Srm2242, Blackbody, Table };
    Kind kind = Kind::Srm2242;
    std::vector<double> coeff;                   // SRM: polynomial in shift/1000 (empty = certified values)
    double range[2] = {195.0, 3500.0};           // SRM: certified range (cm-1)
    double temperatureK = 2856.0;                // blackbody (CIE illuminant A)
    Vec tableShift, tableValue;                  // table, ascending shift
    QString tableName;

    QString label() const;                       // "NIST SRM 2242", "blackbody 2856 K", "table <name>"
    // True relative curve at the shifts (NaN outside the reference's own range).
    Vec curve(const Vec& shift, double laserNm) const;
    static const std::vector<double>& srm2242();   // certified coefficients
};

// Reads a table reference: two columns (shift cm-1, relative intensity), comma/semicolon/tab/space separated,
// '#' comments and a header line allowed. Throws lr::Error.
Reference readTable(const QString& path);

struct Response {
    QString serial;
    int channel = 532;
    QString date;                                // "yyyy-MM-dd HH:mm"
    QString reference;                           // Reference::label()
    double laserNm = 532.1;
    double range[2] = {0.0, 0.0};                // valid shift range (cm-1); constant extrapolation outside
    Vec shift, factor;                           // ascending shift
    double exposure_s = 0.0;
    int averages = 0;
    QString sourceFile;                          // the measured reference spectrum
    double snr = 0.0;
    double dynamic = 0.0;                        // max/min factor over the valid range
    QStringList warnings;

    // Factor at the shifts (linear interpolation; constant outside the valid range).
    Vec factorAt(const Vec& x) const;
    Vec apply(const Vec& x, const Vec& y) const;  // y * factorAt(x)
    QString summary() const;                     // one line for headers and file metadata
};

// Correction from a measured reference (dark-subtracted counts at ascending shifts). Throws lr::Error if
// the reference is saturated, too weak (S/N < kMinSnr) or leaves no usable range. rawMax: highest raw
// signal (saturation check; NaN = unknown).
Response compute(const Vec& shift, const Vec& measured, const Reference& ref, double laserNm,
                 double rawMax = std::numeric_limits<double>::quiet_NaN());

// ---- storage
QString activePath(const QString& serial, int channel);
QString historyPath(const QString& serial, int channel);   // new, unique, for now
std::optional<Response> load(const QString& serial, int channel);   // active file; throws on a corrupt file
std::optional<Response> loadFile(const QString& path);
void save(const Response& r, const QString& path);
QString accept(const Response& r);                // history copy + active file; returns the history path
bool remove(const QString& serial, int channel);  // removes the active file (history kept)

// Active correction of an instrument and channel, cached and re-read when the file changes; nullptr if there
// is none or it is unreadable. Thread-safe.
std::shared_ptr<const Response> active(const QString& serial, int channel);

// y corrected with the active correction of (serial, channel); y unchanged if there is none. *applied says
// which. Channel from file metadata: channelOf("532 nm") = 532 (0 if unreadable).
Vec corrected(const Vec& x, const Vec& y, const QString& serial, int channel, bool* applied = nullptr);
int channelOf(const QString& meta);

// Spectrum metadata key of the instrument (written by Instrument::makeSpectrum): the serial, or "SIMULATOR"
// for the simulator (whose files never mix with the real instrument's).
inline constexpr const char* kSerialKey = "serial";

}  // namespace lr::response
