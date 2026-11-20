#pragma once
// Chemometrics for the Analysis area (specification 5.6): data matrix, preprocessing, PCA, EFA, OPA,
// SIMPLISMA, MCR-ALS with constraints and rotational-ambiguity bands, NNLS fitting with known spectra.
// There is no Python reference for this module; the algorithms follow the literature cited below.
//
// Conventions: a data matrix D has one spectrum per row (n rows) and one variable (Raman shift) per
// column (p columns). The bilinear model of MCR is D = C S + E with C (n × k) concentration profiles
// and S (k × p) pure spectra (rows). Everything is double precision (Eigen).
//
// Thread safety: all functions are re-entrant; mcrAls() may run in a worker thread and be stopped through
// the stop flag.
#include <Eigen/Dense>
#include <QString>
#include <QStringList>
#include <atomic>
#include <functional>
#include <optional>
#include <vector>

#include "fileio.h"
#include "spectrum.h"

namespace lr::chemo {

using Matrix = Eigen::MatrixXd;
using Vector = Eigen::VectorXd;

// ============================================================================================ data matrix
// Spectra on one common x grid.
struct DataMatrix {
    Vec x;                           // common axis (ascending), p values
    Matrix D;                        // n × p
    QStringList names;               // n names
    Vec times;                       // n values: elapsed seconds since the first spectrum, or 0..n-1
    bool hasTimes = false;           // false: times are the spectrum index

    int rows() const { return int(D.rows()); }
    int cols() const { return int(D.cols()); }
    bool empty() const { return D.rows() == 0 || D.cols() == 0; }
    Vec row(int i) const;            // spectrum i as a Vec
    DataMatrix subset(const std::vector<int>& rows) const;   // selected spectra, in the given order
};

// Linear interpolation of (x, y) at the points of grid (x ascending); outside the data: the end values.
Vec resample(const Vec& x, const Vec& y, const Vec& grid);

// Spectra -> data matrix. If every spectrum has the same x axis it is used as it is; otherwise all are
// resampled (linear interpolation) onto an even grid over the range they all cover, with spacing `step`
// (0: the median point spacing of the spectra). Throws lr::Error if the spectra have no common range.
// Times are taken from the metadata (series column "t=12.5s", "series_elapsed_s", ISO time stamps of
// LibreRamen spectrum files), relative to the earliest spectrum; without them, times are the indices.
DataMatrix buildMatrix(const std::vector<fileio::DataSpectrum>& spectra, double step = 0.0);
DataMatrix buildMatrix(const std::vector<Spectrum>& spectra, double step = 0.0);   // times from Spectrum::t
// A time-series table as written by series.cpp / series.py ("raman_shift_cm1,t=0.0s,t=10.1s,..."), or any
// file fileio::read() understands (one or several spectra).
DataMatrix readSeries(const QString& path);

// ============================================================================================ preprocessing
enum class Scaling { None, Center, Autoscale };    // column-wise, used by PCA (MCR works on unscaled data)

struct Preprocess {
    std::optional<double> xmin, xmax;  // region kept (inclusive); empty = from the start / to the end
    bool smooth = false;               // Savitzky-Golay (processing::smooth)
    int smoothWindow = 9;
    QString baseline = "none";         // processing::baselines() key
    double strength = 5.0;             // 0..10, processing::baseline()
    int derivative = 0;                // 0, 1 or 2: Savitzky-Golay derivative (order 3, as processing::process)
    int derivWindow = 11;
    QString normalize = "none";        // processing::normalize() mode, applied within the region
    std::optional<double> band;        // for normalize == "band"
};

// Per spectrum, in this order (as processing::process, so display and analysis agree): smoothing, baseline
// subtraction, derivative (all on the full spectrum, avoiding edge effects at the region limits), then the
// region cut, then normalisation within the region. Throws lr::Error for invalid settings.
DataMatrix preprocess(const DataMatrix& data, const Preprocess& p);

// Column means and scales (standard deviations, ddof 1; 1 where a column is constant or Scaling != Autoscale)
struct ColumnScaling {
    Vector mean, scale;
};
ColumnScaling columnScaling(const Matrix& D, Scaling s);
Matrix applyScaling(const Matrix& D, const ColumnScaling& cs);

// ============================================================================================ PCA
struct PcaResult {
    int components = 0;
    Matrix scores;                     // n × k   (T = U Σ)
    Matrix loadings;                   // p × k   (P = V, unit columns; sign: largest |value| positive)
    Vec singularValues;                // all min(n, p) singular values of the scaled matrix
    Vec explained;                     // fraction of the total sum of squares per component (all of them)
    Vec cumulative;                    // cumulative fraction
    ColumnScaling scaling;             // applied before the SVD
    Vec q;                             // per sample: residual sum of squares after k components (Q, SPE)
    Vec t2;                            // per sample: Hotelling T² with k components
    double qLimit = 0.0;               // approximate 95 % limits: Q by Box's g·χ²(h) approximation, T² from
    double t2Limit = 0.0;              // the F distribution (quantiles by Wilson-Hilferty / Paulson)
};
// PCA by SVD of the (centred/scaled) matrix. components is clipped to [1, min(n, p)].
PcaResult pca(const Matrix& D, int components, Scaling scaling = Scaling::Center);

// Number of significant components: smallest k whose cumulative explained variance reaches `fraction`.
int rankForVariance(const PcaResult& r, double fraction = 0.99);

// ============================================================================================ EFA
// Evolving factor analysis (Maeder 1987; Keller & Massart 1991). Row i of forward holds the first k
// singular values of the sub-matrix of spectra 0..i, row i of backward those of spectra i..n-1 (NaN where
// the window has fewer than j+1 spectra). The noise level is the (k+1)-th singular value of the whole
// matrix: the largest singular value due to noise alone (smaller windows have smaller noise values).
struct EfaResult {
    Matrix forward, backward;          // n × k
    double noise = 0.0;
    // Appearance/disappearance window of each component (first-in-first-out assumption: component j
    // appears where the j-th forward value rises above threshold and disappears where the (k-1-j)-th
    // backward value falls below it). Indices into the rows; window [first, last].
    std::vector<std::pair<int, int>> windows;
    double threshold = 0.0;            // used for windows
};
// threshold <= 0: 2 × noise. D is used as given (no centring, as usual for EFA).
EfaResult efa(const Matrix& D, int components, double threshold = 0.0);
// Initial concentration estimates from EFA windows: 1 inside a component's window, 0 outside (n × k).
Matrix efaConcentrations(const EfaResult& e, int n);

// ============================================================================================ OPA
// Orthogonal projection approach (Sánchez, Toft, van den Bogaert, Massart, Anal. Chem. 68 (1996) 79).
// On rows (spectra): the k most mutually dissimilar spectra, a common initial S estimate. On columns
// (variables): the k purest variables, columns of D as initial C estimate. The dissimilarities (determinants
// of the Gram matrix of the unit-length references and candidate) are weighted by the candidate's norm
// relative to the largest one, so that weak, noise-dominated spectra (whose normalised shape is random)
// are not picked - the same purpose as the offset of SIMPLISMA.
struct OpaResult {
    std::vector<int> selected;         // row (spectra) or column (variables) indices, in selection order
    std::vector<Vec> dissimilarity;    // the dissimilarity curve of each step (n or p values)
    Matrix estimate;                   // spectra: k × p (S, rows of D); variables: n × k (C, columns of D)
};
OpaResult opa(const Matrix& D, int components, bool variables = false);

// ============================================================================================ SIMPLISMA
// SIMPLe-to-use Interactive Self-modelling Mixture Analysis (Windig & Guilment, Anal. Chem. 63 (1991)
// 1425). Purity p_j = σ_j / (μ_j + α), α = offsetPercent/100 · max μ; weights are determinants of the
// correlation-around-origin matrix of the already selected pure variables with the candidate.
struct SimplismaResult {
    std::vector<int> pureVariables;    // column indices, in selection order
    std::vector<Vec> purity;           // purity spectrum of each step (p values)
    Vec stdSpectrum;                   // σ_j (the "standard deviation spectrum")
    Matrix C;                          // n × k: columns of D at the pure variables
    Matrix S;                          // k × p: least-squares spectra for C (pinv(C) D), rows scaled to max 1
};
SimplismaResult simplisma(const Matrix& D, int components, double offsetPercent = 5.0);

// ============================================================================================ NNLS
// min ||A x - b|| subject to x >= 0, from the normal equations AtA = AᵀA (k × k) and Atb = Aᵀb
// (fast combinatorial active set, Bro & de Jong, J. Chemom. 11 (1997) 393 = Lawson-Hanson on AᵀA).
Vector nnlsNormal(const Matrix& AtA, const Vector& Atb, double tol = 0.0);
Vector nnls(const Matrix& A, const Vector& b);

// Fit of a spectrum y by non-negative amounts of component spectra (rows of `components`, same grid as y),
// optionally plus a polynomial background of order baselineOrder (coefficients of either sign).
struct NnlsFit {
    Vec coefficients;                  // one per component (>= 0)
    Vec background;                    // polynomial background (zeros if baselineOrder < 0)
    Vec fitted;                        // components·coefficients + background
    Vec residual;                      // y - fitted
    double residualNorm = 0.0;         // ||residual||
    double r2 = 0.0;                   // 1 - SS_res / SS_tot (around the mean)
};
NnlsFit nnlsFit(const Matrix& components, const Vec& x, const Vec& y, int baselineOrder = -1);

// ============================================================================================ MCR-ALS
// Multivariate curve resolution by alternating least squares (Tauler, Chemom. Intell. Lab. Syst. 30 (1995)
// 133; de Juan & Tauler). Each iteration solves C for fixed S, then S for fixed C (or the other way round
// when starting from C), each subject to the constraints:
//  * non-negativity (C and/or S): exact NNLS per row of C / per column of S (nnlsNormal), otherwise
//    unconstrained least squares;
//  * closure: every row of C is scaled to sum to closureTotal after the C step (fixes the scale);
//  * unimodality (C): each profile is made non-increasing away from its maximum ("vertical" correction;
//    values above unimodalTolerance × the previous value are clipped to it);
//  * fixed spectra: the S rows listed in fixedSpectra keep their initial value (known components).
// Without closure the scale ambiguity is removed by normalising the free S rows to unit Euclidean length
// after each S step (C carries the intensity).
// Lack of fit LOF = 100 · sqrt(Σ e² / Σ d²) (% of the data), explained variance R² = 100 (1 − Σ e² / Σ d²).
// Convergence: the relative change of the residual standard deviation between iterations falls below
// tolerancePercent (default 0.1 %, as the MCR-ALS toolbox). The best (lowest LOF) iterate is returned.
struct McrOptions {
    int maxIterations = 100;
    double tolerancePercent = 0.1;
    bool nonNegC = true;
    bool nonNegS = true;
    bool closure = false;
    double closureTotal = 1.0;
    bool unimodalC = false;
    double unimodalTolerance = 1.0;    // 1.0 = strict; 1.05 allows 5 % rises
    std::vector<int> fixedSpectra;     // component indices whose S row stays as given in the start S
};

struct McrStart {                      // give either C (n × k) or S (k × p)
    Matrix C;
    Matrix S;
};

struct McrResult {
    Matrix C;                          // n × k
    Matrix S;                          // k × p
    double lof = 0.0;                  // lack of fit, %
    double r2 = 0.0;                   // explained variance, %
    int iterations = 0;                // iterations performed
    int bestIteration = 0;             // iteration of the returned solution
    bool converged = false;
    bool stopped = false;              // by the stop flag
    Vec lofHistory;                    // LOF after each iteration
    QString message;                   // "converged after 23 iterations" etc.
};

using McrProgress = std::function<void(int iteration, double lof)>;
// Throws lr::Error for inconsistent sizes or options.
McrResult mcrAls(const Matrix& D, const McrStart& start, const McrOptions& opt = {},
                 const std::atomic<bool>* stop = nullptr, const McrProgress& progress = {});

// Unimodal (vertical) correction of one profile (exposed for tests).
void makeUnimodal(Eigen::Ref<Vector> c, double tolerance = 1.0);

// ============================================================================================ ambiguity
// Rotational ambiguity, MCR-BANDS (Jaumot & Tauler, Chemom. Intell. Lab. Syst. 103 (2010) 96): every
// C' = C T, S' = T⁻¹ S reproduces the fit exactly. For each component n the relative signal contribution
//     f_n(T) = ||c'_n s'_n||_F / ||C S||_F
// is minimised and maximised over invertible T subject to the non-negativity constraints that were used
// (C' >= 0, S' >= 0, within a tolerance of tolerance × the column/row maximum, as soft penalties) and
// with fixed spectra kept (their S rows may only be rescaled). T has unit diagonal (f_n does not depend
// on the scale of each component); the k(k-1) off-diagonal elements are optimised by Nelder-Mead from
// the MCR solution (T = I) and a few perturbed starts. A narrow [fmin, fmax] means a unique resolution
// of that component; a wide one means that the shapes shown are one of many equally good solutions.
// The extreme solutions are returned (S rows scaled to unit length, C scaled accordingly) to draw the
// band. Closure and unimodality are not imposed on the rotations (the band is then an upper bound).
struct AmbiguityBand {
    double fOptimal = 0.0;             // f_n of the MCR solution
    double fMin = 0.0, fMax = 0.0;
    Vec cMin, cMax;                    // concentration profile of component n at fMin / fMax (n values)
    Vec sMin, sMax;                    // spectrum of component n at fMin / fMax (p values, unit length)
};
std::vector<AmbiguityBand> ambiguityBands(const McrResult& r, const McrOptions& opt, double tolerance = 0.01,
                                          const std::atomic<bool>* stop = nullptr);

// ============================================================================================ helpers
// Absolute Pearson correlation between two vectors (0 if either is constant).
double correlation(const Vector& a, const Vector& b);
// Best one-to-one matching of the rows of `found` to the rows of `truth` by |correlation| (exhaustive for
// k <= 8, greedy beyond); returns perm with found row perm[i] matched to truth row i, and the correlations.
std::pair<std::vector<int>, Vec> matchComponents(const Matrix& truth, const Matrix& found);

Matrix toMatrix(const std::vector<Vec>& rows);     // rows must have equal length (throws lr::Error)
Vec toVec(const Vector& v);

}  // namespace lr::chemo
