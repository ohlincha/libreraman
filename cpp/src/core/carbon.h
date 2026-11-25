#pragma once
// Carbon D/G analysis (issue #12): first-order D and G bands of carbons (anodes, conductive carbons, chars),
// the I_D/I_G ratio, the in-plane crystallite size L_a and the 2D band. No widgets: used by the Data page,
// band trends (bandtrend.h carbon quantities) and usable from the API/CLI. 532 nm only (the Cora 5000).
//
// Models (peaks::fitComponents() with its linear baseline, on a baseline-corrected spectrum):
//   LorentzBwf: D Lorentzian (~1350) + G Breit-Wigner-Fano (~1580, see peaks.h for the profile and the q
//     convention: q < 0, tail on the low-shift side) + D' Lorentzian (~1620) when resolved: D' is kept if its
//     height exceeds 3 standard errors and it lowers the RMS residual by more than 5 %; otherwise the fit
//     without D' is reported. The G position is the BWF maximum x0 + Γ/(2q) (ω0 is reported as well).
//   Sadezky: five bands (Sadezky et al., Carbon 43 (2005) 1731): G ~1580, D1 ~1350, D2 (= D') ~1620,
//     D4 ~1200 Lorentzian, D3 ~1500 Gaussian; I_D/I_G is I_D1/I_G.
// Default fit region 950-1800 cm⁻¹.
// Results: I_D/I_G by height (maxima) and by area (integrated profiles; BWF: above its asymptote), with
//   σ_r = r sqrt((σ_a/a)² + (σ_b/b)²) (areas: from the height and FWHM errors, correlations ignored).
//   L_a (nm) = 2.4e-10 · λ⁴ · (I_D/I_G)⁻¹ with λ in nm and the AREA ratio (Cançado et al., Appl. Phys. Lett. 88
//   (2006) 163106): 19.2 nm / (I_D/I_G) at 532 nm. Valid for nanocrystalline graphite (L_a ≳ 2 nm, stage 1 of
//   Ferrari-Robertson); for amorphous carbon the ratio rises with L_a instead and the value is meaningless.
//   2D (~2700 cm⁻¹): one Lorentzian in 2500-2850 cm⁻¹ if the spectrum covers it; reported when its height
//   exceeds 3 standard errors and 3× the noise; I_2D/I_G by height (graphitisation).
#include <QString>
#include <QStringList>

#include <optional>
#include <utility>
#include <vector>

#include "peaks.h"
#include "spectrum.h"

namespace lr::carbon {

constexpr double kLaserNm = 532.0;

enum class Model { LorentzBwf, Sadezky };
QString modelName(Model m);                  // "D Lorentzian + G BWF", "Sadezky five-band"
QString modelKey(Model m);                   // "lorentz-bwf", "sadezky"

// L_a (nm) from the integrated-area ratio I_D/I_G (Cançado); NaN for a ratio <= 0.
double crystalliteSize(double idigArea, double laserNm = kLaserNm);

struct Options {
    Model model = Model::LorentzBwf;
    double lo = 950, hi = 1800;              // first-order fit region (cm⁻¹)
    bool secondOrder = true;                 // fit the 2D band if covered
};

struct Band {
    QString name;                            // "D", "G", "D′", "D1".."D4", "2D"
    QString shape;                           // peaks::profiles() name
    double position = 0, positionErr = 0;    // maximum (bwf: x0 + Γ/(2q))
    double height = 0, heightErr = 0, fwhm = 0, fwhmErr = 0, area = 0, areaErr = 0;
    std::optional<double> center, q, qErr;   // bwf
};

struct Result {
    Model model = Model::LorentzBwf;
    peaks::FitResult fit;                    // first-order fit (components in `bands` order)
    std::vector<Band> bands;                 // LorentzBwf: D, G[, D′]; Sadezky: G, D1, D2, D3, D4
    int d = -1, g = -1, dPrime = -1;         // indices into bands (dPrime -1: not resolved)
    double idigHeight = 0, idigHeightErr = 0, idigArea = 0, idigAreaErr = 0;
    double la = 0, laErr = 0;                // nm, from idigArea
    std::optional<Band> band2d;              // resolved 2D band
    std::optional<peaks::FitResult> fit2d;   // its fit (also when not resolved)
    double i2dIg = 0;                        // I_2D/I_G by height (when band2d)
    QStringList notes;                       // "D′ not resolved", "2D region not measured", ...

    // Named results (keys as bandtrend/API use them): id_ig_area, id_ig_height, la_nm, g_position, g_fwhm,
    // d_position, d_fwhm, d_prime_position, 2d_position, 2d_fwhm, i2d_ig. NaN where not available.
    static const QStringList& keys();
    double value(const QString& key) const;
    double error(const QString& key) const;  // NaN without estimate
    // Table rows: (quantity, value, ±, unit); the text forms are tab-separated (table) or CSV.
    std::vector<QStringList> rows() const;
    QString table() const;
    QString csv(const QString& spectrum = {}) const;
};

// Throws lr::Error when the spectrum does not cover the D and G bands (1300-1620 cm⁻¹) or the fit fails.
Result analyse(const Vec& x, const Vec& y, const Options& opt = {});

}  // namespace lr::carbon
