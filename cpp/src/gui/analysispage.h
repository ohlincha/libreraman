#pragma once
// Analysis area (specification 5.6): a data set of spectra, one preprocessing chain with live preview, and
// the chemometric methods of core/chemometrics: PCA, EFA, pure variables (OPA, SIMPLISMA), MCR-ALS with
// constraints and rotational-ambiguity bands, NNLS fitting with known spectra. Results can be exported as
// CSV and every plot as PNG or SVG.
//
// Self-contained page: the main window feeds it with setSpectra()/addSpectrum()/loadSeriesFile(). MCR-ALS
// and the ambiguity bands run in a worker thread (Stop button); everything else is fast enough to run in
// the GUI thread and follows changes of the data and the preprocessing automatically.
#include <QString>
#include <QStringList>
#include <QWidget>

#include <atomic>
#include <memory>
#include <thread>
#include <utility>
#include <vector>

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
class QTabWidget;
class QTimer;
class QToolButton;

namespace lr::gui {

class PlotWidget;
class RegionItem;

class AnalysisPage : public QWidget {
    Q_OBJECT
public:
    enum class Method { Pca, Efa, PureVariables, Mcr, Nnls };
    enum class InitialEstimate { Simplisma, OpaSpectra, Efa };

    explicit AnalysisPage(QWidget* parent = nullptr);
    ~AnalysisPage() override;

    // ---- data set (replaces / appends; all spectra are checked = used)
    void setSpectra(const std::vector<fileio::DataSpectrum>& spectra);
    void setSpectra(const std::vector<Spectrum>& spectra);
    void addSpectrum(const Spectrum& sp);
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
    QString lastError() const { return m_error; }

    // ---- export
    // CSV files with a "#" comment header. kind: "scores", "loadings", "variance" (PCA), "efa",
    // "pure" (pure-variable spectra), "spectra", "concentrations" (MCR), "nnls". False + lastError() on failure.
    bool exportCsv(const QString& kind, const QString& path);
    // A plot by name ("pca-scree", "pca-scores", "pca-loadings", "efa", "pure-purity", "pure-spectra",
    // "mcr-spectra", "mcr-concentrations", "mcr-lof", "nnls-fit", "nnls-residual", "preview") as PNG or SVG
    // (by suffix).
    bool exportPlot(const QString& name, const QString& path);
    PlotWidget* plot(const QString& name) const;
    QStringList plotNames() const;

signals:
    void statusMessage(const QString& text);
    void dataChanged();                         // raw data set or preprocessing changed
    void mcrFinished();
    void ambiguityFinished();

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
    QWidget* titled(const QString& name, const QString& title, PlotWidget* plot);
    QToolButton* exportButton(Method m);
    PlotWidget* newPlot(const QString& xLabel, const QString& yLabel);

    void refreshList();
    void dataSetChanged();                      // rebuild raw matrix, then preprocess
    void preprocessChanged();
    void schedule();                            // debounced re-run of the visible method
    void runVisible();
    void updatePreview();
    void invalidateResults();
    void setInfo(QLabel* label, const QString& text, bool warning = false);

    void drawPca();
    void drawEfa();
    void drawPure();
    void drawMcr();
    void drawMcrLof();
    void drawNnls();
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
    bool m_dirty[5] = {true, true, true, true, true};

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
};

}  // namespace lr::gui
