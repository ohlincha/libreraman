#pragma once
// Peak picking and peak fitting (Python: peaks.py).
//
// pick()  noise-based detection: peaks above `snr` times the noise, with sub-pixel position, height, FWHM,
//         area and S/N; optional shoulder detection from the second derivative.
// fit()   bounded least-squares fit of a sum of profiles (lorentz, gauss, pvoigt, voigt) plus a linear
//         baseline in a region, with standard errors from the Jacobian.
#include <QString>
#include <QStringList>
#include <complex>
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
std::complex<double> faddeeva(std::complex<double> z);                 // scipy.special.wofz (Poppe-Wijers)

// PROFILES: names in the Python order, and the number of parameters per peak.
const QStringList& profiles();
int profileParams(const QString& shape);                               // throws lr::Error for unknown shapes
// Profile `shape` with parameters p (profileParams(shape) values) at x.
double profile(const QString& shape, double x, const double* p);
// Analytic area of a profile (_area); nullopt for voigt (integrated numerically by fit()).
std::optional<double> area(const QString& shape, const double* p);

struct FitPeak {
    double position = 0, positionErr = 0, height = 0, heightErr = 0, fwhm = 0, fwhmErr = 0, area = 0;
    std::optional<double> eta;                                         // pvoigt only
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
// Guesses given as positions only (start FWHM 10).
FitResult fit(const Vec& x, const Vec& y, std::pair<double, double> region, const Vec& positions,
              const QString& shape = "pvoigt", bool fixPositions = false);

}  // namespace lr::peaks
