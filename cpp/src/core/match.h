#pragma once
// Library matching (SPECIFICATION 5.5).
//
// Full-spectrum search: query and library spectra get the same preprocessing - linear resampling onto a
// common grid (multiples of `step` cm^-1, so any two prepared spectra share grid points), baseline removal
// (processing::baseline, arPLS strength 6 by default), optional smoothing and Savitzky-Golay derivative. They are
// compared over their overlapping range with the hit quality index
//     HQI = max(r, 0)^2,  r = Pearson correlation of the two mean-centred vectors        (0..1)
// which is independent of scale and offset (normalisation is implicit). Entries whose overlap is shorter
// than minOverlap are skipped.
//
// Peak search: query peaks (picked on the SNIP-corrected spectrum, or given) against the entries' peak lists
// (literature band lists, or peaks picked when the spectrum was stored). Peaks are paired one-to-one,
// closest first, within `tolerance`; each pair counts with closeness 1 - 0.5 d/tolerance and the peak weights
// (0.25 + 0.75 x relative intensity; 1 when unknown). score = sqrt(recall x precision), where recall is the
// matched weight fraction of library peaks inside the query range and precision that of query peaks inside
// the library entry's range.
//
// Mixtures: non-negative least squares (Lawson-Hanson) of the prepared query on 1..3 prepared library spectra
// (each scaled to unit norm over the common range); candidates are the top full-spectrum hits; every
// combination is fitted and ranked by HQI(query, fit) minus 0.005 per extra component (a component is
// only kept if it explains something). fraction_i = c_i / sum(c): the share of each unit-norm reference.
#include <Eigen/Dense>
#include <QString>
#include <limits>
#include <optional>
#include <vector>

#include "library.h"

namespace lr::match {

struct Preprocess {
    double step = 2.0;                 // grid spacing (cm^-1)
    double xmin = 150, xmax = 3500;    // analysed range (intersected with the data)
    QString baseline = "arpls";        // processing::baseline method ("none" = off)
    double strength = 6.0;             // 0..10
    int smoothWindow = 0;              // Savitzky-Golay smoothing on the grid (odd points; 0 = off)
    int derivative = 0;                // 0, 1 or 2 (Savitzky-Golay, order 3)
    int derivWindow = 11;              // points
};

// A spectrum on the common grid: x_i = (k0 + i) * step.
struct Prepared {
    long k0 = 0;
    double step = 2.0;
    Vec y;
    bool empty() const { return y.empty(); }
    double x(size_t i) const { return double(k0 + long(i)) * step; }
    double xFirst() const { return x(0); }
    double xLast() const { return y.empty() ? x(0) : x(y.size() - 1); }
    Vec xs() const;
};

// Preprocess one spectrum (x ascending or descending). Empty if fewer than 16 grid points remain.
Prepared prepare(const Vec& x, const Vec& y, const Preprocess& p = {});
// HQI of two prepared spectra over their overlap; overlap (cm^-1) returned in lo/hi. 0 if no overlap.
double hqi(const Prepared& a, const Prepared& b, double* lo = nullptr, double* hi = nullptr);
double hqi(const Vec& a, const Vec& b);                 // equal-length vectors

// Peaks of a spectrum for peak search: arPLS (strength 6) baseline, peaks::pick(snr) above 50 cm^-1, the maxPeaks highest,
// relative heights.
std::vector<library::PeakEntry> pickPeaks(const Vec& x, const Vec& y, double snr = 5.0, int maxPeaks = 40);
struct PeakScore {
    double score = 0;
    int matched = 0, libraryPeaks = 0, queryPeaks = 0;  // counted inside the common range
};
PeakScore peakScore(const std::vector<library::PeakEntry>& query, const std::vector<library::PeakEntry>& reference,
                    double tolerance, double qlo = -std::numeric_limits<double>::infinity(),
                    double qhi = std::numeric_limits<double>::infinity(),
                    double rlo = -std::numeric_limits<double>::infinity(),
                    double rhi = std::numeric_limits<double>::infinity());

// Lawson-Hanson NNLS: argmin ||A c - b||, c >= 0.
Eigen::VectorXd nnls(const Eigen::MatrixXd& A, const Eigen::VectorXd& b, int maxIter = 0);

struct Hit {
    qint64 id = 0;
    QString name, formula, cls, licence;
    library::Source source = library::Source::Own;
    double laserNm = 0;
    double score = 0;                  // HQI or peak score, 0..1
    double lo = 0, hi = 0;             // compared range (cm^-1)
    int matched = -1, libraryPeaks = -1, queryPeaks = -1;   // peak search only
};

struct PeakOptions {
    double tolerance = 8.0;            // cm^-1
    double snr = 5.0;                  // for picking query peaks
    int maxPeaks = 40;
};

struct MixtureComponent {
    qint64 id = 0;
    QString name;
    double coefficient = 0;            // on the unit-norm reference
    double fraction = 0;               // coefficient / sum
};

struct Mixture {
    std::vector<MixtureComponent> components;   // sorted by fraction, descending
    double hqi = 0;                    // HQI(query, fit)
    double score = 0;                  // hqi - 0.005 (components - 1), used for ranking
    double residual = 0;               // ||query - fit|| with the query scaled to unit norm
    // for plotting (on the common grid; query scaled to unit norm)
    Vec x, query, fit;
    std::vector<Vec> parts;            // coefficient x reference, per component
};

class Matcher {
public:
    explicit Matcher(Preprocess p = {}) : pre_(p) {}

    const Preprocess& preprocess() const { return pre_; }
    // Changing the preprocessing re-prepares the loaded references.
    void setPreprocess(const Preprocess& p);

    // Loads (and prepares) the entries of `lib` passing `f`; replaces what was loaded.
    void load(const library::Library& lib, const library::Filter& f = {});
    void add(const library::Entry& e);           // one entry (with data)
    void clear() { refs_.clear(); }
    size_t size() const { return refs_.size(); }
    double minOverlap = 400.0;                   // cm^-1 for full-spectrum search and mixtures

    // Full-spectrum search; `f` is applied in memory (class, source, laser, text in name/formula/class/tags).
    std::vector<Hit> search(const Vec& x, const Vec& y, int topN = 10, const library::Filter& f = {}) const;
    std::vector<Hit> search(const Prepared& q, int topN = 10, const library::Filter& f = {}) const;
    // Peak search with peaks picked from the spectrum, or with a given peak list (range: all).
    std::vector<Hit> searchPeaks(const Vec& x, const Vec& y, const PeakOptions& o = {}, int topN = 10,
                                 const library::Filter& f = {}) const;
    std::vector<Hit> searchPeaks(const std::vector<library::PeakEntry>& query, const PeakOptions& o = {},
                                 int topN = 10, const library::Filter& f = {},
                                 double qlo = -std::numeric_limits<double>::infinity(),
                                 double qhi = std::numeric_limits<double>::infinity()) const;
    // NNLS fit of the query with the given entries. Throws lr::Error if an id is unknown or there is no
    // common range.
    Mixture fitMixture(const Vec& x, const Vec& y, const std::vector<qint64>& ids) const;
    // Best mixtures of 1..maxComponents among the top `candidates` full-spectrum hits.
    std::vector<Mixture> searchMixtures(const Vec& x, const Vec& y, int candidates = 8, int maxComponents = 3,
                                        int topN = 5, const library::Filter& f = {}) const;

    const Prepared* prepared(qint64 id) const;   // nullptr if unknown or without spectrum

private:
    struct Ref {
        library::Entry meta;                     // without spectra (kept small)
        Vec x, y;                                // original preferred spectrum (for re-preparing)
        Prepared prep;
        std::vector<library::PeakEntry> peaks;
        double lo = -std::numeric_limits<double>::infinity(), hi = std::numeric_limits<double>::infinity();
    };
    static bool accepts(const library::Filter& f, const library::Entry& e);
    Hit hitOf(const Ref& r) const;
    Mixture fit(const Prepared& q, const std::vector<const Ref*>& refs) const;
    const Ref* find(qint64 id) const;

    Preprocess pre_;
    std::vector<Ref> refs_;
};

}  // namespace lr::match
