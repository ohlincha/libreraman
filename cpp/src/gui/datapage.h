#pragma once
// Data area (Python: gui/data.py): open, compare, process, fit and export spectra.
//
// Spectra measured in this session are added with addSpectrum(); files can be opened or dropped onto the
// list. The ticked spectra are shown as an overlay, stacked with an offset, or as a heat map (one row per
// spectrum), processed with the Measure page's settings (setProcessingSettings) plus the page's own
// normalisation and baseline method. Other features: difference A − B of the first two ticked spectra,
// a calculator a·A + b·B + c, the peak table of the current spectrum, profile fits of the bands inside the
// visible x range (the fit window can then be dragged to refit), and export (CSV, JCAMP-DX, common-axis
// table, all ticked spectra in one JCAMP-DX or SPC file, plot as PNG/SVG). Multi-spectrum files (JCAMP-DX
// LINK files, multi-file SPC, series.csv) open as one entry per spectrum. Long series: processed spectra are
// cached per entry, at most kMaxCurves ticked spectra are drawn in the overlay/stacked views (evenly thinned),
// and a redraw processes new spectra for at most kProcessBudgetMs (the rest follow in later passes). The intensity axis follows the visible x range (PlotWidget autoY).
//
// Peaks: picked (S/N threshold, on the processed or the raw spectrum, ≥ 100 cm⁻¹) and labelled on the plot
// for the current spectrum (or all ticked ones). Clicking the plot adds a peak to the current spectrum,
// right-click offers adding/removing a peak near the cursor; the edits are kept per spectrum. The peaks
// in the fit window are the start values (components) of the fit, at most maxComponents() (the strongest);
// editing the peaks refits. Fit problems are shown in the note line (no dialogs).
//
// The page is self-contained (no dependency on the main window): "Add to library…" in the list's context
// menu only emits addToLibraryRequested().
#include <QColor>
#include <QStringList>
#include <QWidget>

#include <functional>
#include <optional>
#include <utility>
#include <vector>

#include "core/carbon.h"
#include "core/fileio.h"
#include "core/peaks.h"
#include "core/processing.h"
#include "core/spectrum.h"

class QCheckBox;
class QDialog;
class QComboBox;
class QPushButton;
class QLabel;
class QListWidget;
class QListWidgetItem;
class QMenu;
class QSlider;
class QSpinBox;
class QDoubleSpinBox;
class QTimer;
class QStackedWidget;
class QTableWidget;

namespace lr::gui {

class PlotWidget;
class PlotItem;
class RegionItem;
class HeatMapItem;
class PeakLabels;
class BandStripItem;
class Curve;

class DataPage : public QWidget {
    Q_OBJECT
public:
    enum class View { Overlay = 0, Stacked = 1, HeatMap = 2 };
    using SettingsFn = std::function<processing::Settings()>;

    explicit DataPage(QWidget* parent = nullptr);
    ~DataPage() override;

    // Processing settings of the Measure page (Python proc_settings callable). Baseline and normalisation
    // are overridden by the page's own controls. Default: SNIP, strength 5, no despike/smoothing.
    void setProcessingSettings(SettingsFn fn);

    // ---- spectra
    // A spectrum measured in this session (Python MainWindow.add_to_data): named "<sample> HH:MM:SS"
    // (+ " #<series_index>"), ticked.
    void addSpectrum(const Spectrum& sp);
    void addSpectrum(const fileio::DataSpectrum& sp, bool checked = true);   // Python add()
    void addSpectra(const std::vector<Spectrum>& spectra);                   // several, one redraw
    // Files or folders (recursively: .csv .txt .spc .jdx .dx). Unreadable files are reported in one
    // warning box (or only via the message() signal when messages are disabled). Returns the errors.
    QStringList openFiles(const QStringList& paths);
    int count() const { return int(m_entries.size()); }
    const fileio::DataSpectrum& spectrum(int row) const { return m_entries.at(size_t(row)).sp; }
    bool isTicked(int row) const;
    void setTicked(int row, bool on);
    void checkAll(bool on);
    void rename(int row, const QString& name);
    void setColor(int row, const QColor& c);     // invalid: automatic (palette colour by ticked position)
    QColor colorOf(int row) const;               // as drawn (invalid if not ticked)
    void removeRows(std::vector<int> rows);
    void removeSelected();
    void clear();
    int currentRow() const;
    QPushButton* addToLibraryButton() const { return m_addLibBtn; }
    int libraryCandidate(bool ask, const QString& title = {});   // row "Add to library…"/"Match" uses (-1: none / cancelled)
    void addToLibraryDialog();
    void matchDialog();                    // "Match": the candidate spectrum (asks which if several are ticked)
    QPushButton* matchButton() const { return m_matchBtn; }
    void setCurrentRow(int row);
    std::vector<int> tickedRows() const;

    // ---- view and processing
    void setView(View v);
    View view() const;
    void setNormalisation(const QString& mode);  // "none", "max", "area"
    QString normalisation() const;
    // Baseline: "measure" (the Measure page's method and strength, the default), "none", or a
    // processing::baselines() key with this page's strength (0-10).
    void setBaselineMethod(const QString& key);
    QString baselineMethod() const;
    void setBaselineStrength(double s);
    double baselineStrength() const;
    // Baseline fit lower end (cm-1): 150 for this instrument (edge filter); 0 fits imported spectra everywhere.
    void setBaselineFitMin(double cm);
    double baselineFitMin() const;
    void setIntensityCorrection(bool on);        // per spectrum, by the serial in its metadata
    bool intensityCorrection() const;
    void setBaselineCorrection(bool on);         // off = "none"; on = the last method other than "none"
    bool baselineCorrection() const;
    void setDifference(bool on);                 // first ticked − second ticked
    bool difference() const;
    void setOffset(int percent);
    void setAngle(int degrees);                 // stacked view: -60..60 (0 = plain stack)
    int angle() const;                 // stacked offset, 0..100 % of the largest span
    int offset() const;
    processing::Settings settings() const;       // effective settings (Measure page + page controls)
    // x, processed y. If processing fails (error set), y is returned unprocessed.
    std::pair<Vec, Vec> processed(const fileio::DataSpectrum& sp, QString* error = nullptr) const;
    QString describeProcessing() const;          // Python _describe() (file metadata)

    // ---- peaks
    enum class Labels { Off = 0, Current = 1, AllTicked = 2 };
    static constexpr double kMinPeakShift = 100.0;   // no peaks below (laser line, filter edge)
    void setPeakSnr(double snr);                 // pick threshold (default 8)
    double peakSnr() const;
    void setPickOnRaw(bool raw);                 // pick on the unprocessed spectrum (default: as shown)
    bool pickOnRaw() const;
    void setPeakLabels(Labels l);
    Labels peakLabels() const;
    // Picked peaks of a spectrum (minus the removed ones, plus the added ones), sorted by position.
    std::vector<peaks::Peak> peaksOf(int row) const;
    // Adds a peak to the current spectrum at x (snapped to the local maximum within ±3 cm⁻¹); refits if
    // there is a fit. False (and a note) outside the data or below kMinPeakShift.
    bool addPeakAt(double x);
    // Removes the peak (or fit component) of the current spectrum nearest to x within `tolerance` cm⁻¹
    // (< 0: 12 pixels of the plot); refits if there is a fit.
    bool removePeakNear(double x, double tolerance = -1);
    void resetPeaks(int row);                    // forget the added/removed peaks of a spectrum
    PeakLabels* peakLabelItem(int row) const;    // the labels of a spectrum on the plot (nullptr: none)
    QString note() const;                        // the note line (processing and fit problems)
    void showNote(const QString& text) { setNote(text); }   // until the next redraw/fit (empty: clear)
    // Context menu of the plot at shift x (tests: inspect the actions without exec()).
    void fillPlotMenu(QMenu& menu, double x);
    void fillListMenu(QMenu& menu, int row);     // context menu of a list entry

    // ---- fitting
    void setFitProfile(const QString& shape);    // pvoigt, voigt, lorentz, gauss
    void setMaxComponents(int n);                // at most n components (the strongest peaks), default 12
    int maxComponents() const;
    // Fit the peaks of the current spectrum in [lo, hi] (any width); on failure returns false and the
    // message (also shown in the note line).
    bool fitRange(double lo, double hi, QString* error = nullptr);
    bool fitView(QString* error = nullptr);      // the visible x range
    bool refit(QString* error = nullptr);        // the current fit window again (after editing the peaks)
    void clearFit();
    bool hasFit() const { return !m_fitItems.empty(); }
    int fitComponents() const { return int(m_fitPositions.size()); }

    // ---- carbon D/G analysis (core/carbon.h) of the current spectrum as shown: draws the fit (components and,
    // if resolved, the 2D band) like a fit without a fit window, fills the peak table with the bands and the fit
    // line with I_D/I_G and L_a. On failure returns false and the message (also in the note line). Cleared
    // like a fit (clearFit, redraw); a normal fit replaces it.
    bool carbonAnalysis(carbon::Model model = carbon::Model::LorentzBwf, QString* error = nullptr);
    const std::optional<carbon::Result>& carbonResult() const { return m_carbon; }
    bool exportCarbon(const QString& path);      // the carbon results as CSV (spectrum, quantity, value, error, unit)
    QDialog* carbonWindow() const { return m_carbonDlg; }         // nullptr until carbonDialog()
    QTableWidget* carbonTable() const { return m_carbonTable; }   // the window's results table

    // ---- calculator: a·A + b·B + c (rows; bRow < 0: no B); adds the result, returns its row (-1: none)
    int calculate(int aRow, double a, int bRow, double b, double c);

    // ---- export (ticked spectra, processed as shown); return the written paths / success
    QStringList exportSpectra(const QString& dir, const QString& kind);   // kind "csv" or "jdx"
    // All ticked spectra in one file: JCAMP-DX LINK file (.jdx/.dx) or multi-file SPC (.spc), by suffix.
    bool exportOneFile(const QString& path);
    bool exportTable(const QString& path);
    // Peak table as CSV: the fit result if there is one, else the peaks of the current spectrum (labels
    // "all ticked": of every ticked spectrum), with the spectrum name and whether a peak was added by hand.
    bool exportPeaks(const QString& path);
    bool exportPlot(const QString& path);       // .png (2000 px wide) or .svg, the plot shown

    // ---- widgets (tests, main window)
    PlotWidget* plot() const { return m_plot; }
    PlotWidget* heatPlot() const { return m_heat; }
    QTableWidget* peakTable() const { return m_table; }
    void setBandHelp(bool organic, bool inorganic);   // band-assignment strip and column
    QListWidget* list() const { return m_list; }
    QString fitText() const;
    void setMessagesEnabled(bool on) { m_messages = on; }   // false: no message boxes (tests)

public slots:
    void openFilesDialog();
    void calculatorDialog();
    void exportDialog(const QString& kind);      // "csv", "jdx", "table", "peaks", "onejdx", "onespc", "png", "svg"
    // Carbon D/G window (menu Tools ▸ Carbon D/G analysis…, or the plot's context menu; no button on the page):
    // the model choice, the results table (Copy, Save CSV…) and Analyse again; analyses the current spectrum
    // when opened.
    void carbonDialog();
    void redraw();

signals:
    void spectraChanged();                       // added, removed, renamed, ticked
    void message(const QString& title, const QString& text);   // warnings and notes (also shown as boxes)
    // "Add to library…" in the list's context menu (the host routes it to the Library page).
    void addToLibraryRequested(const lr::fileio::DataSpectrum& sp);
    // "Match" (next to Add to library…): the host sends it to the Library page's matching.
    void matchRequested(const lr::fileio::DataSpectrum& sp);

protected:
    void changeEvent(QEvent* e) override;
    bool eventFilter(QObject* obj, QEvent* e) override;   // band-strip tooltips on the plot

private:
    struct Entry {
        fileio::DataSpectrum sp;
        QColor color;                            // custom colour (invalid = automatic)
        // processed(sp) for the settings `key` (redraw cache: a long series is not reprocessed per spectrum)
        QString key;
        Vec procY;
        QString procError;
        Vec addedPeaks, removedPeaks;            // peak edits (positions, cm⁻¹)
    };
    struct Drawn {                               // a spectrum as drawn (stacked: with offset and shift)
        int row;
        double dy, dx;
    };
    QString settingsKey() const;
    static constexpr int kProcessBudgetMs = 50;
    static constexpr int kMaxCurves = 24;        // overlay / stacked: more ticked spectra are thinned out
    bool m_followUp = false;                     // a redraw for spectra still to be processed is posted
    void build();
    void addEntry(const fileio::DataSpectrum& sp, bool checked);
    void openPaths(const QStringList& paths, QStringList& errors);
    void onItemChanged(QListWidgetItem* it);
    void listContextMenu(const QPoint& pos);
    void updatePeaks();
    void updatePeakOverlay();
    void plotClicked(const QPointF& data, Qt::MouseButton button);
    void plotMenu(const QPointF& data, const QPoint& global);
    std::pair<Vec, Vec> pickData(int row) const;  // the data peaks are picked on
    Vec shownY(int row) const;                    // processed y (cached when possible)
    void peaksChanged();                          // table, labels, refit
    void setNote(const QString& text);            // empty: back to the processing note
    double pixelTolerance(double px) const;       // px plot pixels in cm⁻¹
    // The peak (or fit component: its start position) of the current spectrum nearest to x within tol.
    bool findPeakNear(double x, double tol, double* pos) const;
    double drawnShift(int row) const;             // stacked waterfall: sideways shift of a drawn row
    void updateSwatches();
    int heightDecimals() const;
    void fillPeakRow(const std::vector<std::pair<double, int>>& values);   // (value, decimals), NaN = blank
    void warn(const QString& title, const QString& text);
    void info(const QString& title, const QString& text);

    std::vector<Entry> m_entries;
    SettingsFn m_settingsFn;
    bool m_messages = true;
    int m_batch = 0;                             // > 0: redraw deferred (opening many files)
    bool m_dirty = false;

    QListWidget* m_list = nullptr;
    QComboBox* m_mode = nullptr;
    QComboBox* m_norm = nullptr;
    QComboBox* m_baseline = nullptr;           // "measure" = the Measure page's method, else a method key
    QDoubleSpinBox* m_strength = nullptr;
    QPushButton* m_addLibBtn = nullptr;
    QPushButton* m_matchBtn = nullptr;      // used when a method is chosen on this page
    QSpinBox* m_fitMin = nullptr;              // baseline fit lower end (cm-1)
    QString m_lastBaseline = QStringLiteral("measure");   // restored by setBaselineCorrection(true)
    void saveBaselineSettings() const;     // method, strength and fit start persist (QSettings)
    void strengthMode(bool reset);         // polynomial: the control is the order
    QCheckBox* m_diff = nullptr;
    QSlider* m_offset = nullptr;
    QSlider* m_angle = nullptr;                 // stacked: sideways shift per trace (pseudo-3D waterfall)
    QCheckBox* m_intensity = nullptr;           // intensity (instrument-response) correction
    QCheckBox* m_mono = nullptr;                // stacked: one colour
    QCheckBox* m_bandOrganic = nullptr;         // band-assignment help (resources/band_assignments.csv)
    QCheckBox* m_bandInorganic = nullptr;
    BandStripItem* m_bandStrip = nullptr;       // owned by m_plot (deleted by clear())
    void updateBandStrip();
    void updateAssignments();                   // assignment column of the peak table
    QString assignmentText(double position, int max = 3) const;
    bool m_inAngleRedraw = false;
    QStackedWidget* m_stack = nullptr;
    PlotWidget* m_plot = nullptr;
    PlotWidget* m_heat = nullptr;
    HeatMapItem* m_heatItem = nullptr;
    QComboBox* m_profile = nullptr;
    QTableWidget* m_table = nullptr;
    QLabel* m_fitLabel = nullptr;
    QLabel* m_note = nullptr;                    // processing and fit problems
    QString m_baseNote;                          // processing problems / thinning (set by redraw)
    QDoubleSpinBox* m_snr = nullptr;
    QComboBox* m_pickOn = nullptr;               // processed / raw
    QComboBox* m_labels = nullptr;
    QSpinBox* m_maxComp = nullptr;
    QTimer* m_clickTimer = nullptr;              // a click adds a peak unless it was a double-click
    double m_clickX = 0;
    std::vector<Drawn> m_drawn;
    std::vector<PeakLabels*> m_labelItems;       // parallel to m_labelRows
    std::vector<int> m_labelRows;
    Curve* m_peakMarks = nullptr;
    std::vector<PlotItem*> m_fitItems;
    RegionItem* m_fitRegion = nullptr;
    Vec m_fitPositions;                          // start positions of the fit components
    Vec m_fitResultPositions;                    // fitted positions (same order)
    std::vector<peaks::FitPeak> m_fitPeaks;
    QString m_fitShape;                          // profile (or "carbon <model>") of m_fitPeaks, for the export
    void drawFit(const peaks::FitResult& res, int row);   // model and component curves where `row` is drawn
    // carbon D/G
    std::optional<carbon::Result> m_carbon;
    QString m_carbonSpectrum;                    // name of the analysed spectrum
    QDialog* m_carbonDlg = nullptr;
    QComboBox* m_carbonModel = nullptr;
    QTableWidget* m_carbonTable = nullptr;
    QLabel* m_carbonNote = nullptr;
    void fillCarbonTable();
    void runCarbon();                            // the window's model on the current spectrum
};

}  // namespace lr::gui
