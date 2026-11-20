#pragma once
// Data area (Python: gui/data.py): open, compare, process, fit and export spectra.
//
// Spectra measured in this session are added with addSpectrum(); files can be opened or dropped onto the
// list. The ticked spectra are shown as an overlay, stacked with an offset, or as a heat map (one row per
// spectrum), processed with the Measure page's settings (setProcessingSettings) plus the page's own
// normalisation and baseline switch. Other features: difference A − B of the first two ticked spectra,
// a calculator a·A + b·B + c, the peak table of the current spectrum, profile fits of the bands inside the
// visible x range (the fit window can then be dragged to refit), and export (CSV, JCAMP-DX, common-axis
// table, all ticked spectra in one JCAMP-DX or SPC file, plot as PNG/SVG). Multi-spectrum files (JCAMP-DX
// LINK files, multi-file SPC, series.csv) open as one entry per spectrum. Long series: processed spectra are
// cached per entry, at most kMaxCurves ticked spectra are drawn in the overlay/stacked views (evenly thinned),
// and a redraw processes new spectra for at most kProcessBudgetMs (the rest follow in later passes). The intensity axis follows the visible x range (PlotWidget autoY).
//
// The page is self-contained (no dependency on the main window).
#include <QColor>
#include <QStringList>
#include <QWidget>

#include <functional>
#include <utility>
#include <vector>

#include "core/fileio.h"
#include "core/peaks.h"
#include "core/processing.h"
#include "core/spectrum.h"

class QCheckBox;
class QComboBox;
class QLabel;
class QListWidget;
class QListWidgetItem;
class QSlider;
class QStackedWidget;
class QTableWidget;

namespace lr::gui {

class PlotWidget;
class PlotItem;
class RegionItem;
class HeatMapItem;

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
    void setCurrentRow(int row);
    std::vector<int> tickedRows() const;

    // ---- view and processing
    void setView(View v);
    View view() const;
    void setNormalisation(const QString& mode);  // "none", "max", "area"
    QString normalisation() const;
    void setBaselineCorrection(bool on);
    bool baselineCorrection() const;
    void setDifference(bool on);                 // first ticked − second ticked
    bool difference() const;
    void setOffset(int percent);                 // stacked offset, 0..100 % of the largest span
    int offset() const;
    processing::Settings settings() const;       // effective settings (Measure page + page controls)
    // x, processed y. If processing fails (error set), y is returned unprocessed.
    std::pair<Vec, Vec> processed(const fileio::DataSpectrum& sp, QString* error = nullptr) const;
    QString describeProcessing() const;          // Python _describe() (file metadata)

    // ---- peaks and fitting
    void setFitProfile(const QString& shape);    // pvoigt, voigt, lorentz, gauss
    // Fit the bands of the current spectrum in [lo, hi]; on failure returns false and the message.
    bool fitRange(double lo, double hi, QString* error = nullptr);
    bool fitView(QString* error = nullptr);      // the visible x range (< 1200 cm⁻¹)
    void clearFit();
    bool hasFit() const { return !m_fitItems.empty(); }

    // ---- calculator: a·A + b·B + c (rows; bRow < 0: no B); adds the result, returns its row (-1: none)
    int calculate(int aRow, double a, int bRow, double b, double c);

    // ---- export (ticked spectra, processed as shown); return the written paths / success
    QStringList exportSpectra(const QString& dir, const QString& kind);   // kind "csv" or "jdx"
    // All ticked spectra in one file: JCAMP-DX LINK file (.jdx/.dx) or multi-file SPC (.spc), by suffix.
    bool exportOneFile(const QString& path);
    bool exportTable(const QString& path);
    bool exportPlot(const QString& path);       // .png (2000 px wide) or .svg, the plot shown

    // ---- widgets (tests, main window)
    PlotWidget* plot() const { return m_plot; }
    PlotWidget* heatPlot() const { return m_heat; }
    QTableWidget* peakTable() const { return m_table; }
    QListWidget* list() const { return m_list; }
    QString fitText() const;
    void setMessagesEnabled(bool on) { m_messages = on; }   // false: no message boxes (tests)

public slots:
    void openFilesDialog();
    void calculatorDialog();
    void exportDialog(const QString& kind);      // "csv", "jdx", "table", "onejdx", "onespc", "png", "svg"
    void redraw();

signals:
    void spectraChanged();                       // added, removed, renamed, ticked
    void message(const QString& title, const QString& text);   // warnings and notes (also shown as boxes)

protected:
    void changeEvent(QEvent* e) override;

private:
    struct Entry {
        fileio::DataSpectrum sp;
        QColor color;                            // custom colour (invalid = automatic)
        // processed(sp) for the settings `key` (redraw cache: a long series is not reprocessed per spectrum)
        QString key;
        Vec procY;
        QString procError;
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
    QCheckBox* m_baseline = nullptr;
    QCheckBox* m_diff = nullptr;
    QSlider* m_offset = nullptr;
    QStackedWidget* m_stack = nullptr;
    PlotWidget* m_plot = nullptr;
    PlotWidget* m_heat = nullptr;
    HeatMapItem* m_heatItem = nullptr;
    QComboBox* m_profile = nullptr;
    QTableWidget* m_table = nullptr;
    QLabel* m_fitLabel = nullptr;
    QLabel* m_note = nullptr;                    // processing problems
    std::vector<PlotItem*> m_fitItems;
    RegionItem* m_fitRegion = nullptr;
};

}  // namespace lr::gui
