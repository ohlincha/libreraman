#pragma once
// Analysis area (specification 5.6): a data set of spectra, one preprocessing chain with live preview, and
// the chemometric methods of core/chemometrics: PCA, EFA, pure variables (OPA, SIMPLISMA), MCR-ALS with
// constraints and rotational-ambiguity bands, NNLS fitting with known spectra, and peak trends (kinetics of
// band heights, areas, fitted areas, positions and widths along a series; core/bandtrend). Results can be
// exported as CSV and every plot as PNG or SVG.
//
// Self-contained page: the main window feeds it with setSpectra()/addSpectrum()/loadSeriesFile(). MCR-ALS
// and the ambiguity bands run in a worker thread (Stop button); the other methods follow changes of the data
// and the preprocessing automatically (debounced): in the GUI thread, except PCA and EFA of data sets with
// kAsyncRows or more spectra (a long series), which run in a background thread (one at a time; a result for
// old data is dropped and the method runs again). Direct calls (runPca(), ...) are always synchronous.
// Peak trends are recomputed incrementally: measurements are cached per band and spectrum (keyed by the band
// settings and a hash of each preprocessed spectrum), so a growing series only measures its new spectra;
// peak fits of more than kAsyncFits spectra run in a background thread that delivers them in chunks.
#include <QString>
#include <QStringList>
#include <QWidget>

#include <atomic>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

#include "core/bandtrend.h"
#include "core/chemometrics.h"
#include "core/fileio.h"
#include "core/spectrum.h"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QListWidget;
class QPushButton;
class QSlider;
class QSpinBox;
class QTableView;
class QTableWidget;
class QTabWidget;
class QTimer;
class QToolButton;

namespace lr::gui {

class PlotWidget;
class TrendModel;
class RegionItem;

class AnalysisPage : public QWidget {
    Q_OBJECT
public:
    enum class Method { Pca, Efa, PureVariables, Mcr, Nnls, Trends };
    enum class TrendNormalisation { None, Ratio, First };
    enum class InitialEstimate { Simplisma, OpaSpectra, Efa };

    explicit AnalysisPage(QWidget* parent = nullptr);
    ~AnalysisPage() override;

    // ---- data set (replaces / appends; all spectra are checked = used)
    void setSpectra(const std::vector<fileio::DataSpectrum>& spectra);
    void setSpectra(const std::vector<Spectrum>& spectra);
    void addSpectrum(const Spectrum& sp);
    void addSpectra(const std::vector<Spectrum>& spectra);                    // one update for all
    void addSpectra(const std::vector<fileio::DataSpectrum>& spectra);
    // Reads a time-series table (series.csv) or any file fileio::read() understands and makes it the data
    // set. Returns false (and lastError()) if the file cannot be read.
    bool loadSeriesFile(const QString& path);
    bool addFile(const QString& path);
    void clearSpectra();
    int spectrumCount() const { return int(m_spectra.size()); }

    // ---- preprocessing
    void setPreprocess(const chemo::Preprocess& p);
    chemo::Preprocess preprocessSettings() const;
    const chemo::DataMatrix& rawData() const { return m_raw; }        // checked spectra on a common grid
    const chemo::DataMatrix& data() const { return m_data; }          // after preprocessing

    // ---- methods (used by the buttons; public for the main window, tests and previews)
    void showMethod(Method m);
    Method currentMethod() const;
    void setComponents(int k);                  // all methods
    void setMcrInitialEstimate(InitialEstimate e);
    void setMcrOptions(const chemo::McrOptions& o);
    chemo::McrOptions mcrOptions() const;
    void runPca();
    void runEfa();
    void runPureVariables();
    void runMcr(bool wait = false);             // wait: in the calling thread (tests, previews)
    void stopMcr();
    bool mcrRunning() const { return m_worker.joinable() && m_busy; }
    void computeAmbiguity(bool wait = false);
    void runNnls();
    // Known spectra for MCR-ALS (fixed components) and reference spectra for NNLS, from DataSpectra.
    void setKnownSpectra(const std::vector<fileio::DataSpectrum>& spectra);
    void setReferenceSpectra(const std::vector<fileio::DataSpectrum>& spectra);

    const chemo::PcaResult* pcaResult() const { return m_pca.get(); }
    const chemo::EfaResult* efaResult() const { return m_efa.get(); }
    const chemo::McrResult* mcrResult() const { return m_mcr.get(); }
    const std::vector<chemo::AmbiguityBand>& ambiguity() const { return m_bands; }
    const chemo::NnlsFit* nnlsResult() const { return m_nnls.get(); }

    // ---- peak trends (tab "Peak trends")
    // A band [lo, hi] (cm⁻¹); name default "1052 cm⁻¹" (centre). Returns its index.
    int addTrendBand(double lo, double hi, const QString& name = {});
    // Band of ±2·FWHM around the peak of the mean spectrum nearest to `position` (within 25 cm⁻¹); -1: none.
    int addTrendBandAtPeak(double position);
    void removeTrendBand(int i);
    int trendBandCount() const { return int(m_trendBands.size()); }
    const bandtrend::Band& trendBand(int i) const { return m_trendBands[size_t(i)].band; }
    void setTrendBand(int i, const bandtrend::Band& b);          // region, quantity, baseline, profile, name
    void setTrendQuantity(int i, bandtrend::Quantity q);
    void setTrendNormalisation(TrendNormalisation n, int reference = 0);
    void setTrendKinetics(bandtrend::Kinetic k);
    // Peaks picked on the mean (preprocessed) spectrum: the "Add band" menu.
    const std::vector<peaks::Peak>& trendPeaks();
    void runTrends(bool wait = false);          // wait: every fit in the calling thread
    bool trendsRunning() const { return m_trendBusy; }
    // Trend of band i as plotted (after normalisation; empty for the reference band of a ratio) and its fit.
    bandtrend::Series trendSeries(int i) const;
    bandtrend::KineticFit trendFit(int i) const;
    static constexpr int kAsyncFits = 24;       // more fits than this (spectra × fitted bands): background
    QString lastError() const { return m_error; }
    qint64 lastRunMs() const { return m_lastRunMs; }     // GUI-thread time of the last automatic method run
    qint64 lastAsyncMs() const { return m_lastAsyncMs; } // background time of the last PCA/EFA run (0: none)

    // ---- export
    // CSV files with a "#" comment header. kind: "scores", "loadings", "variance" (PCA), "efa",
    // "pure" (pure-variable spectra), "spectra", "concentrations" (MCR), "nnls", "trends" (peak trends: values,
    // errors and the raw measurements per band). False + lastError() on failure.
    bool exportCsv(const QString& kind, const QString& path);
    // A plot by name ("pca-scree", "pca-scores", "pca-loadings", "efa", "pure-purity", "pure-spectra",
    // "mcr-spectra", "mcr-concentrations", "mcr-lof", "nnls-fit", "nnls-residual", "trends-spectra", "trends",
    // "preview") as PNG or SVG
    // (by suffix).
    bool exportPlot(const QString& name, const QString& path);
    PlotWidget* plot(const QString& name) const;
    QStringList plotNames() const;

signals:
    void statusMessage(const QString& text);
    void dataChanged();                         // raw data set or preprocessing changed
    void mcrFinished();
    void ambiguityFinished();
    void trendsFinished();                      // background peak fits done

private:
    struct PlotEntry {
        QString name;
        QString title;
        PlotWidget* plot;
    };
    QWidget* buildDataPanel();
    QWidget* buildPcaTab();
    QWidget* buildEfaTab();
    QWidget* buildPureTab();
    QWidget* buildMcrTab();
    QWidget* buildNnlsTab();
    QWidget* buildTrendsTab();
    QWidget* titled(const QString& name, const QString& title, PlotWidget* plot);
    QToolButton* exportButton(Method m);
    PlotWidget* newPlot(const QString& xLabel, const QString& yLabel);

    void refreshList();
    void appendListItems(size_t from);          // list items for m_spectra[from..]
    void dataSetChanged();                      // rebuild raw matrix, then preprocess
    void preprocessChanged();
    void schedule();                            // debounced re-run of the visible method
    void runVisible();
    void runAsync(Method m);
    void asyncDone(Method m, unsigned gen, int k, int scaling, std::shared_ptr<chemo::PcaResult> p,
                   std::shared_ptr<chemo::EfaResult> e, const QString& err, qint64 ms);
    void updatePreview();
    void invalidateResults();
    void setInfo(QLabel* label, const QString& text, bool warning = false);

    void drawPca();
    void drawEfa();
    void drawPure();
    void drawMcr();
    void drawMcrLof();
    void drawNnls();
    // peak trends
    struct TrendBand {
        bandtrend::Band band;
        QColor color;
        int id = 0;
    };
    struct TrendCache {
        QString key;                            // band settings the rows were measured with
        std::vector<bandtrend::Measure> rows;   // spectra 0..rows.size()-1
    };
    struct TrendChunk {                         // background result
        std::vector<int> ids;
        std::vector<QString> keys;
        std::vector<size_t> starts;
        std::vector<std::vector<bandtrend::Measure>> rows;
        std::vector<quint64> hashes;            // of the measured spectra (from min(starts))
        size_t from = 0;
    };
    static QString trendKey(const bandtrend::Band& b);
    void trendBandsChanged(bool redrawSpectra = true);  // band list/settings changed: table, combos, run
    void refreshBandTable();
    void syncBandRow(int i);
    void updateTrendMean();
    void runTrendsAsync();
    void mergeTrendChunk(const TrendChunk& c);
    void drawTrendSpectra();
    void drawTrends();
    void trendSpectraMenu(const QPointF& data, const QPoint& globalPos);
    void fillAddBandMenu();
    int addBand(bandtrend::Band b);
    void mcrDone(std::shared_ptr<chemo::McrResult> r, unsigned generation);
    void bandsDone(std::vector<chemo::AmbiguityBand> b, unsigned generation);
    void joinWorker();
    bool mcrStart(chemo::McrStart& start, QString& error) const;
    chemo::Matrix onGrid(const std::vector<fileio::DataSpectrum>& spectra, QString& error) const;
    QString timeLabel() const;
    QStringList componentNames(int k) const;

    // data
    std::vector<fileio::DataSpectrum> m_spectra;
    std::vector<bool> m_checked;
    chemo::DataMatrix m_raw, m_data;
    std::vector<fileio::DataSpectrum> m_known, m_references;
    unsigned m_generation = 0;                  // incremented whenever m_data changes
    QString m_error;
    qint64 m_lastRunMs = 0, m_lastAsyncMs = 0;

    // results
    std::unique_ptr<chemo::PcaResult> m_pca;
    std::unique_ptr<chemo::EfaResult> m_efa;
    std::unique_ptr<chemo::OpaResult> m_opa;
    std::unique_ptr<chemo::SimplismaResult> m_simplisma;
    std::shared_ptr<chemo::McrResult> m_mcr;
    unsigned m_mcrGeneration = 0;
    chemo::McrOptions m_mcrUsed;
    std::vector<chemo::AmbiguityBand> m_bands;
    std::unique_ptr<chemo::NnlsFit> m_nnls;
    std::vector<chemo::NnlsFit> m_nnlsAll;
    QStringList m_nnlsNames;
    chemo::Matrix m_nnlsComponents;
    Vec m_liveLof;
    bool m_dirty[6] = {true, true, true, true, true, true};

    // automatic PCA/EFA of large data sets (runVisible) in the background
    static constexpr int kAsyncRows = 40;
    std::thread m_autoThread;
    bool m_autoBusy = false, m_autoAgain = false;

    // worker (MCR-ALS, bands)
    std::thread m_worker;
    std::atomic<bool> m_stop{false};
    std::atomic<bool> m_busy{false};

    // widgets: data panel
    QListWidget* m_list = nullptr;
    QLabel* m_dataInfo = nullptr;
    QCheckBox* m_regionOn = nullptr;
    QDoubleSpinBox* m_xmin = nullptr;
    QDoubleSpinBox* m_xmax = nullptr;
    QCheckBox* m_smooth = nullptr;
    QSpinBox* m_smoothWindow = nullptr;
    QComboBox* m_baseline = nullptr;
    QSlider* m_strength = nullptr;
    QComboBox* m_normalize = nullptr;
    QDoubleSpinBox* m_band = nullptr;
    QComboBox* m_derivative = nullptr;
    QSpinBox* m_derivWindow = nullptr;
    QComboBox* m_previewMode = nullptr;
    PlotWidget* m_preview = nullptr;
    RegionItem* m_regionItem = nullptr;
    QTimer* m_timer = nullptr;
    bool m_updating = false;

    QTabWidget* m_tabs = nullptr;
    std::vector<PlotEntry> m_plots;

    // PCA
    QSpinBox* m_pcaK = nullptr;
    QComboBox* m_pcaScaling = nullptr;
    QComboBox* m_pcaView = nullptr;
    QSpinBox* m_pcaX = nullptr;
    QSpinBox* m_pcaY = nullptr;
    QComboBox* m_pcaLabels = nullptr;
    QLabel* m_pcaInfo = nullptr;
    PlotWidget *m_scree = nullptr, *m_scores = nullptr, *m_loadings = nullptr;
    // EFA
    QSpinBox* m_efaK = nullptr;
    QLabel* m_efaInfo = nullptr;
    PlotWidget* m_efaPlot = nullptr;
    // pure variables
    QComboBox* m_pureMethod = nullptr;
    QSpinBox* m_pureK = nullptr;
    QDoubleSpinBox* m_offset = nullptr;
    QLabel* m_pureInfo = nullptr;
    PlotWidget *m_purity = nullptr, *m_pureSpectra = nullptr;
    // MCR
    QSpinBox* m_mcrK = nullptr;
    QComboBox* m_mcrInit = nullptr;
    QCheckBox *m_nnC = nullptr, *m_nnS = nullptr, *m_closure = nullptr, *m_unimodal = nullptr;
    QDoubleSpinBox* m_closureTotal = nullptr;
    QSpinBox* m_maxIter = nullptr;
    QDoubleSpinBox* m_tol = nullptr;
    QLabel* m_knownLabel = nullptr;
    QPushButton *m_runMcr = nullptr, *m_stopMcr = nullptr, *m_bandsButton = nullptr;
    QCheckBox* m_showBands = nullptr;
    QLabel* m_mcrInfo = nullptr;
    PlotWidget *m_mcrSpectra = nullptr, *m_mcrConc = nullptr, *m_mcrLof = nullptr;
    // NNLS
    QComboBox* m_nnlsTarget = nullptr;
    QComboBox* m_nnlsSource = nullptr;
    QSpinBox* m_nnlsOrder = nullptr;
    QLabel* m_refLabel = nullptr;
    QLabel* m_nnlsInfo = nullptr;
    PlotWidget *m_nnlsPlot = nullptr, *m_nnlsResidual = nullptr;
    // peak trends
    std::vector<TrendBand> m_trendBands;
    int m_nextBandId = 0;
    std::vector<TrendCache> m_trendCache;       // per band, same order as m_trendBands
    std::vector<quint64> m_trendHashes;         // hashes of the spectra the caches were measured on
    unsigned m_trendMeanGen = ~0u;
    Vec m_trendMean;
    std::vector<peaks::Peak> m_trendPeaks;
    std::vector<bandtrend::Series> m_trendSeries;   // as plotted, per band
    std::vector<bandtrend::KineticFit> m_trendFits;
    std::thread m_trendThread;
    bool m_trendBusy = false, m_trendAgain = false;
    unsigned m_trendRun = 0;                    // serial of the background run
    std::atomic<bool> m_trendStop{false};
    QComboBox* m_trendNorm = nullptr;
    QComboBox* m_trendRef = nullptr;
    QComboBox* m_trendKinetic = nullptr;
    QToolButton* m_addBand = nullptr;
    QTableWidget* m_bandTable = nullptr;
    QTableView* m_trendTable = nullptr;
    TrendModel* m_trendModel = nullptr;
    QLabel* m_trendInfo = nullptr;
    PlotWidget *m_trendSpectra = nullptr, *m_trendPlot = nullptr;
};

}  // namespace lr::gui
