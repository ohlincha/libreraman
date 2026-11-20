#pragma once
// Computed Raman spectra from quantum-chemistry output (specification 5.6 "Computed spectra").
//
// Readers (computed_readers.cpp; the format is detected from the content, see detectFormat()):
//   gaussian    Gaussian 09/16 .log/.out, freq=raman: every "Harmonic frequencies" section is one
//               Calculation (Link1 jobs, several freq jobs); with freq=HPModes the high-precision
//               block is skipped in favour of the normal block of the same job. Symmetry labels,
//               IR intensities, depolarisation ratios, normal-mode displacements, standard
//               orientation geometry, method/basis (route line, else SCF Done / Standard basis).
//   orca        ORCA 4/5/6 output: VIBRATIONAL FREQUENCIES, RAMAN SPECTRUM, IR SPECTRUM (ORCA 4
//               T**2 or ORCA 5/6 km/mol column), CARTESIAN COORDINATES (ANGSTROEM); one Calculation
//               per frequency section ($new_job). No symmetry labels (ORCA does not print them).
//   orca-hess   ORCA .hess: $vibrational_frequencies, $raman_spectrum, $ir_spectrum, $atoms
//               (normal modes are not read).
//   nwchem      NWChem frequency/Raman output: the "Normal Eigenvalue ||" tables (projected IR
//               intensities in km/mol, Raman table: column labelled "Activity", else its last
//               column), "Output coordinates in angstroms", XC functional, "ao basis" summary.
//   cp2k        CP2K VIBRATIONAL_ANALYSIS "VIB|" blocks: frequencies, IR intensities; a "VIB|Raman"
//               row is used if present. CP2K normally prints no Raman activities: then the
//               calculation is read with a warning and unit activities (positions only).
//   qe          Quantum ESPRESSO dynmat.x output ("# mode [cm-1] [THz] IR Raman depol.fact",
//               Raman activities in A^4/amu need ph.x with lraman=.true.); symmetry labels from
//               "freq (i - j) = ... --> T_2g ..." lines of ph.x/dynmat.x when present. A ph.x output
//               alone gives frequencies only (warning).
//   vasp-raman  vasp_raman.py output (vasp_raman.dat: "# mode freq(cm-1) alpha beta2 activity").
//   phonopy     Phonopy-Spectroscopy peak tables (columns Frequency [THz|inv. cm|meV], Irrep.,
//               Intensity, Linewidth; whitespace or comma separated).
//   castep      CASTEP .castep with calculate_raman: the "+ N Frequency irrep. IR intensity active
//               Raman activity active +" table of each q-point that has Raman activities.
// Modes with |frequency| < 1 cm^-1 (translations, rotations, acoustic modes) are dropped; negative
// frequencies are imaginary modes (warning, never plotted). read() throws lr::Error when a file has no
// recognisable frequencies; a file without Raman activities gives a warning (unit activities).
//
// Intensities (ramanCrossSection): the differential Raman cross-section of a mode with activity S
// (Å^4/amu), frequency ν_i and excitation ν_0 (wavenumbers) at temperature T (Polavarapu 1990,
// J. Phys. Chem. 94, 8106; Krishnakumar et al., Spectrochim. Acta A 60 (2004) 1427; Long, The Raman
// Effect, 2002):
//     dσ/dΩ = (2π)^4/45 · h/(8π² c ν_i) · (ν_0 − ν_i)^4 · S / (1 − exp(−h c ν_i / k T))
// i.e. I ∝ (ν_0 − ν_i)^4 · S / (ν_i · (1 − exp(−h c ν_i/kT))). Returned in SI (m²/sr); the plots
// normalise to the strongest band.
//
// Scaling: ν' = f·ν (single factor) or f_low below / f_high above a split frequency (two regions).
// fitScaling() matches the computed bands to experimental peaks (peaks::pick) with nearest-neighbour or
// Hungarian (minimum total |Δν|) assignment within a tolerance, solves the least-squares factor
// f = Σ ν_c ν_e / Σ ν_c² over the pairs, re-assigns and repeats until stable; the start factor comes
// from a coarse scan (0.85..1.10) maximising the number of matches.
//
// Broadening: every band becomes an area-normalised Lorentzian, Gaussian or pseudo-Voigt profile of
// given FWHM whose area equals the band value. Structures are compared by the Pearson correlation of
// the broadened spectra with the experimental one on the overlapping range.
#include <QString>
#include <QStringList>
#include <array>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

#include "fileio.h"
#include "peaks.h"
#include "spectrum.h"

namespace lr::computed {

inline constexpr double kNaN = std::numeric_limits<double>::quiet_NaN();

// ---------------------------------------------------------------------------------------------- data
struct Mode {
    double frequency = 0;                         // cm^-1 as computed (unscaled); < 0: imaginary
    QString symmetry;                             // irreducible representation, if printed
    double activity = kNaN;                       // Raman activity S (Å^4/amu); NaN = not available
    double depolarization = kNaN;                 // depolarisation ratio (plane-polarised light)
    double ir = kNaN;                             // IR intensity (program units, usually km/mol)
    std::vector<std::array<double, 3>> displacement;   // per atom (Cartesian), may be empty
};

struct Atom {
    QString element;
    std::array<double, 3> position{0, 0, 0};      // Å (Cartesian), or fractional for CASTEP cells
};

struct Calculation {
    QString program;                              // "Gaussian", "ORCA", "NWChem", "CP2K", ...
    QString version;
    QString format;                               // detectFormat() name
    QString method, basis;                        // as printed (e.g. "B3LYP", "6-31G(d)"); may be empty
    QString formula;                              // e.g. "CCl4" (from the file or the atoms)
    QString source;                               // file path
    int block = 0;                                // number of the frequency section in the file (0..)
    int blocks = 1;                               // frequency sections read from the file
    std::vector<Atom> atoms;
    std::vector<Mode> modes;                      // ascending frequency
    QStringList warnings;                         // imaginary modes, missing Raman activities, ...

    QString name() const;                         // "CCl4 B3LYP/6-31G(d) (file.log)" (+ " #2" for blocks)
    QString label() const;                        // program + version + method/basis
    bool hasActivities() const;                   // any mode with a Raman activity
    bool hasIr() const;
    int imaginaryModes() const;
};

// Format names (detectFormat) and the file dialog filter.
const QStringList& formats();
QString fileFilter();
// Format of a text (first part of a file is enough); empty if unknown. fileName helps for .hess.
QString detectFormat(const QString& text, const QString& fileName = {});
// Reads all frequency sections of a file. format empty = detect. Throws lr::Error.
std::vector<Calculation> read(const QString& path, const QString& format = {});
std::vector<Calculation> readText(const QString& text, const QString& source = {}, const QString& format = {});
// ECCE / project hook: supported output files under dir (recursively), sorted. Only files with a
// typical extension (.log .out .output .hess .castep .dat .txt .csv) are sniffed (first 256 kB).
QStringList findOutputs(const QString& dir, bool recursive = true);
// Chemical formula in Hill order ("CHCl3", "CCl4") from element symbols.
QString hillFormula(const QStringList& elements);

// ----------------------------------------------------------------------------------- intensities
enum class Quantity { Activity, Intensity };     // plot activities as-is, or cross-sections

struct Scaling {
    double low = 1.0;                             // factor (single) or below `split`
    double high = 1.0;                            // above `split` (two regions)
    double split = 0;                             // <= 0: single factor (`low`)
    bool twoRegion() const { return split > 0; }
    double apply(double nu) const;
    QString text() const;                         // "0.967" or "0.983 < 2000 cm⁻¹ < 0.958"
};

struct Options {
    Quantity quantity = Quantity::Intensity;
    double laserNm = 532.0;
    double temperatureK = 298.15;
    Scaling scaling;
    double minFrequency = 10.0;                   // modes below (cm^-1, unscaled) are not plotted
    double mergeTolerance = 0.5;                  // degenerate modes within this (cm^-1) form one band
};

// Differential Raman cross-section dσ/dΩ (m²/sr) for a mode (formula above). 0 for nu <= 0 or
// nu >= laser wavenumber.
double ramanCrossSection(double nuCm, double activity, double laserNm, double temperatureK);

// A band: degenerate modes merged, scaled position, value per Options::quantity. The cross-section uses
// the scaled position. A calculation without Raman activities gets activity 1 and intensity 1 per mode
// (positions only: equal heights per mode).
struct Band {
    double position = 0;                          // scaled (cm^-1)
    double unscaled = 0;
    double activity = 0;                          // summed (unit activity if the file has none)
    double intensity = 0;                         // summed cross-section (m²/sr); 1/mode without activities
    double value = 0;                             // activity or intensity (Options::quantity)
    double ir = kNaN;
    QString symmetry;
    std::vector<int> modes;                       // indices into Calculation::modes
};
std::vector<Band> bands(const Calculation& calc, const Options& opt);
double maxValue(const std::vector<Band>& b);

// ------------------------------------------------------------------------------------ broadening
enum class Shape { Lorentzian, Gaussian, PseudoVoigt };
QString shapeName(Shape s);

struct Broadening {
    Shape shape = Shape::Lorentzian;
    double fwhm = 8.0;                            // cm^-1
    double eta = 0.5;                             // Lorentzian fraction (pseudo-Voigt)
};
// Area-normalised profile at x (integral 1).
double unitProfile(double x, double x0, const Broadening& b);
Vec makeGrid(double lo, double hi, double step);
// Sum of profiles; each band's area equals its value.
Vec broaden(const std::vector<Band>& b, const Vec& grid, const Broadening& br);
// The broadened spectrum as a DataSpectrum (normalised to a maximum of 1 if normalise) with metadata
// (program, method, basis, scaling, laser, temperature, profile).
fileio::DataSpectrum spectrum(const Calculation& calc, const Options& opt, const Broadening& br, const Vec& grid,
                              bool normalise = true);
// Stick spectrum (position, value) normalised to 1 if normalise.
std::pair<Vec, Vec> sticks(const std::vector<Band>& b, bool normalise = true);

// ------------------------------------------------------------------------------ matching / scaling
enum class Matching { Nearest, Hungarian };

// Pairs (i, j) of a[i] with b[j], |a[i] − b[j]| <= tolerance, each index used at most once.
// Nearest: greedy by distance (closest pair first); Hungarian: minimum total |Δ| where an unmatched
// item of `a` costs 2·tolerance, so more pairs are preferred (e.g. a = {100, 106}, b = {104, 112},
// tolerance 7: Nearest pairs only 106–104, Hungarian pairs 100–104 and 106–112).
std::vector<std::pair<int, int>> assign(const Vec& a, const Vec& b, double tolerance, Matching m = Matching::Hungarian);
// Minimum-cost assignment of rows to columns of an n×m cost matrix (row-major), n <= m; returns the
// column of every row.
std::vector<int> hungarian(const std::vector<double>& cost, int n, int m);

// Experimental peaks used for matching: ALS baseline (if subtractBaseline), then peaks::pick(snr); peaks
// lower than minRelative × the highest are dropped (noise, weak overtones).
std::vector<peaks::Peak> experimentalPeaks(const fileio::DataSpectrum& exp, double snr = 8.0,
                                           bool subtractBaseline = true, double minRelative = 0.02);

struct FitOptions {
    bool twoRegion = false;
    double split = 2000.0;                        // cm^-1 (unscaled computed frequency)
    double tolerance = 30.0;                      // cm^-1 between scaled computed band and peak
    Matching matching = Matching::Hungarian;
    double minRelative = 0.02;                    // computed bands weaker than this × max are ignored
    double lo = 0.80, hi = 1.10;                  // allowed factor range
    std::optional<std::pair<double, double>> range;   // experimental x range used (default all)
};

struct ScaleFit {
    Scaling scaling;
    double rms = kNaN;                            // cm^-1 of the matched pairs after scaling
    int matched = 0;
    std::vector<std::pair<int, int>> pairs;       // (band index, peak index)
};
// Fit to experimental peak positions; bands must be computed with unit scaling (Options::scaling).
ScaleFit fitScaling(const std::vector<Band>& unscaled, const Vec& expPositions, const FitOptions& o = {});
ScaleFit fitScaling(const Calculation& calc, const Options& opt, const fileio::DataSpectrum& exp,
                    const FitOptions& o = {}, double snr = 8.0);

// --------------------------------------------------------------------------------- assignment table
struct AssignmentRow {
    int band = -1;                                // index into the (scaled) bands, -1: experimental only
    int peak = -1;                                // index into the experimental peaks, -1: computed only
    double computed = kNaN;                       // scaled position
    double unscaled = kNaN;
    double experimental = kNaN;
    double delta = kNaN;                          // computed − experimental
    double computedRel = kNaN;                    // band value / strongest band
    double experimentalRel = kNaN;                // peak height / highest peak
    QString symmetry;
};
// All bands above minRelative and all peaks, matched within tolerance, sorted by position.
std::vector<AssignmentRow> assignmentTable(const std::vector<Band>& scaled, const std::vector<peaks::Peak>& exp,
                                           double tolerance = 30.0, Matching m = Matching::Hungarian,
                                           double minRelative = 0.01);
void writeAssignmentCsv(const QString& path, const std::vector<AssignmentRow>& rows);
QString assignmentText(const std::vector<AssignmentRow>& rows);   // aligned plain-text table

// ------------------------------------------------------------------------------------- comparison
// Pearson correlation of two spectra on the overlap (optionally limited to range), after linear
// interpolation of b onto the points of a.
double correlation(const Vec& ax, const Vec& ay, const Vec& bx, const Vec& by,
                   std::optional<std::pair<double, double>> range = {});

struct StructureScore {
    int index = 0;                                // into the calculations given
    double correlation = kNaN;
    Scaling scaling;
    double rms = kNaN;
    int matched = 0;
};
// Scores every calculation against the experimental spectrum (baseline-corrected internally), each
// with its own fitted scaling if fitScale, else opt.scaling. Sorted best first.
std::vector<StructureScore> compare(const std::vector<Calculation>& calcs, const Options& opt, const Broadening& br,
                                    const fileio::DataSpectrum& exp, bool fitScale = true, const FitOptions& fo = {});

}  // namespace lr::computed
