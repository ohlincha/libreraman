#pragma once
// Peak picking and peak fitting (Python: peaks.py).
//
// pick()  noise-based detection: peaks above `snr` times the noise, with sub-pixel position, height, FWHM,
//         area and S/N; optional shoulder detection from the second derivative.
// fit()   bounded least-squares fit of a sum of profiles (lorentz, gauss, pvoigt, voigt, bwf) plus a linear
//         baseline in a region, with standard errors from the Jacobian.
// fitComponents()  the same with a profile, start values and bounds per component (mixed models such as the
//         carbon D/G fits in carbon.h).
//
// Breit-Wigner-Fano (bwf): I(x) = I0 (1 + 2(x - x0)/(q w))² / (1 + (2(x - x0)/w)²), with w = Γ. Convention:
// q < 0 gives the tail on the low-shift side (graphitic carbon G band, Ferrari & Robertson 2000); |q| -> inf
// is a Lorentzian. The fit parameter is 1/q (smooth through the Lorentzian limit), bounded to |1/q| <= 0.5.
// Derived values, with u = 2(x - x0)/w and s = 1/q: the maximum is at u = s, i.e. x_max = x0 + w/(2q),
// height I0 (1 + 1/q²); FWHM = w (q² + 1)/(q² - 1) (|q| > 1); the profile tends to I0/q² far from x0, so its
// area is taken above that asymptote: π I0 w (1 - 1/q²)/2 (principal value; the Lorentzian area for q -> inf).
// fit() reports position = x_max, height = maximum, the FWHM and area above; FitPeak::center = x0, q = q.
#include <QString>
#include <QStringList>
#include <complex>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

#include "spectrum.h"

namespace lr::peaks {

struct Peak {
    double position = 0, height = 0, fwhm = 0, area = 0, snr = 0;
    bool shoulder = false;
};

// Robust noise estimate (sigma) from the first difference (insensitive to peaks and baseline).
double noiseLevel(const Vec& y);
// Peaks in a baseline-corrected spectrum, sorted by position.
std::vector<Peak> pick(const Vec& x, const Vec& y, double snr = 5.0, double minFwhm = 2.0, bool shoulders = false,
                       std::optional<double> xmin = {}, std::optional<double> xmax = {});

// ---- profiles (a = height, w = FWHM)
double lorentz(double x, double a, double x0, double w);
double gauss(double x, double a, double x0, double w);
double pvoigt(double x, double a, double x0, double w, double eta);
double voigt(double x, double a, double x0, double wg, double wl);     // Gaussian and Lorentzian FWHM
double bwf(double x, double i0, double x0, double w, double q);       // Breit-Wigner-Fano (see above)
std::complex<double> faddeeva(std::complex<double> z);                 // scipy.special.wofz (Poppe-Wijers)

// PROFILES: names in the Python order, and the number of parameters per peak.
const QStringList& profiles();
int profileParams(const QString& shape);                               // throws lr::Error for unknown shapes
// Profile `shape` with parameters p (profileParams(shape) values) at x. Parameters: (height, position, FWHM)
// plus eta (pvoigt), the Lorentzian FWHM (voigt; FWHM is then the Gaussian one) or 1/q (bwf: (I0, x0, Γ, 1/q)).
double profile(const QString& shape, double x, const double* p);
// Analytic area of a profile (_area); nullopt for voigt (integrated numerically by fit()).
std::optional<double> area(const QString& shape, const double* p);

struct FitPeak {
    double position = 0, positionErr = 0, height = 0, heightErr = 0, fwhm = 0, fwhmErr = 0, area = 0;
    std::optional<double> eta;                                         // pvoigt only
    QString shape;                                                     // profile of this component
    std::optional<double> center, centerErr, q, qErr;                  // bwf only: x0 and q (position = maximum)
};

struct FitResult {
    QString shape;
    std::vector<FitPeak> peaks;
    std::pair<double, double> baseline;                                // (offset, slope) at the region centre
    Vec x, y, model;
    std::vector<Vec> components;
    double r2 = 0.0, rms = 0.0;
    QString table() const;
};

// Fit `shape` profiles in region (xmin, xmax). Without guesses, peaks are picked on the whole spectrum
// (pick(snr=4)) and those inside the region are used. Throws lr::Error("no peaks to fit in this region").
FitResult fit(const Vec& x, const Vec& y, std::pair<double, double> region,
              const std::optional<std::vector<Peak>>& guesses = std::nullopt, const QString& shape = "pvoigt",
              bool fixPositions = false);
// One component of fitComponents(). NaN means the default of fit(): start height = data at the position
// minus the region's minimum, position within ±max(3·FWHM, 10 points), FWHM from one point to 20·FWHM + 50.
struct Component {
    QString shape = QStringLiteral("lorentz");
    double position = 0, fwhm = 10;                                    // start values
    double height = std::numeric_limits<double>::quiet_NaN();
    double posLo = std::numeric_limits<double>::quiet_NaN(), posHi = std::numeric_limits<double>::quiet_NaN();
    double fwhmLo = std::numeric_limits<double>::quiet_NaN(), fwhmHi = std::numeric_limits<double>::quiet_NaN();
    double invQ = -0.1;                                                // bwf: start 1/q
};
// Mixed-profile fit with a linear baseline in region; FitResult::shape is the shapes joined by '+'.
FitResult fitComponents(const Vec& x, const Vec& y, std::pair<double, double> region,
                        const std::vector<Component>& components);

// Guesses given as positions only (start FWHM 10).
FitResult fit(const Vec& x, const Vec& y, std::pair<double, double> region, const Vec& positions,
              const QString& shape = "pvoigt", bool fixPositions = false);

}  // namespace lr::peaks
