#pragma once
// The Measure area (Python gui/app.py MeasurePage): live spectrum with faded previous spectra, peak labels
// and the processing preview (baseline drawn), series progress, trend of one band (or waterfall), detector
// and last-spectrum panels, and at the right the settings panel with MEASURE / START SERIES / Stop.
//
// Fast series: addSpectrum() only does the bookkeeping (history, processed copy for the display cache, peaks,
// one trend point, detector labels); the plots are redrawn by a coalescing timer at most every
// kMinRedrawMs, and not at all while the page is hidden (redrawn when shown). Processed spectra are cached
// per history entry (recomputed only when the processing settings change), at most kFaded previous spectra
// are drawn faded, and the trend curve is updated in place.
#include <QDateTime>
#include <QElapsedTimer>
#include <QString>
#include <deque>
#include <utility>
#include <vector>

#include "controller.h"
#include "page.h"

class QCheckBox;
class QComboBox;
class QHBoxLayout;
class QDoubleSpinBox;
class QFormLayout;
class QLabel;
class QLineEdit;
class QMenu;
class QProgressBar;
class QPushButton;
class QRadioButton;
class QScrollArea;
class QSlider;
class QSpinBox;
class QTimeEdit;
class QTimer;
class QToolButton;

namespace lr::gui {

class Curve;
class ElidedLabel;
class RepeatDialog;
class SnrDialog;
class FocusWindow;
class PeakLabels;
class PlotWidget;

class MeasurePage : public Page {
    Q_OBJECT
public:
    explicit MeasurePage(Controller* ctl, QWidget* parent = nullptr);
    static constexpr int kMinRedrawMs = 150;          // plots redrawn at most ~7 times per second
    static constexpr int kFaded = 4;                  // previous spectra drawn faded behind the newest
    // Useful range of a spectrum for peak picking, the peaks menu and the automatic trend band: the
    // region below kUsefulMin (laser line, edge filter) and above kUsefulMax (end of the detector range;
    // calibration extrapolation produces junk there) is ignored.
    static constexpr double kUsefulMin = 100, kUsefulMax = 4000;
    static constexpr double kPeakMin = 150;           // peaks are listed from here (Python pick(xmin=150))
    // Peak positions (at most 12, ascending) of a processed spectrum inside the useful range.
    static lr::Vec pickPeaks(const lr::Vec& x, const lr::Vec& y);

    // ---- settings as entered
    MeasureSettings measureSettings() const;          // single spectrum ("once" background -> "each")
    lr::SeriesSettings seriesSettings() const;
    int channel() const;                              // 532 / 1064
    lr::processing::Settings processingSettings() const;   // Python proc_settings()
    double minInterval() const;                       // shortest possible interval with these settings (s)
    double startDelay() const;                        // from the Start choice (s)
    QString folder() const { return m_folder; }
    void setFolder(const QString& path);              // fixed folder (no automatic per-day folder)
    static QString defaultFolder(const QDate& day = QDate::currentDate());   // ~/Documents/LibreRaman/<date>

    // ---- widgets (tests, self-test, screenshots)
    QDoubleSpinBox* exposureBox() const { return m_exposure; }
    QSpinBox* averagesBox() const { return m_averages; }
    QDoubleSpinBox* acquisitionBox() const { return m_acq; }
    QLineEdit* sampleEdit() const { return m_sample; }
    QDoubleSpinBox* intervalBox() const { return m_interval; }
    QComboBox* intervalUnitBox() const { return m_intervalUnit; }
    QSpinBox* countBox() const { return m_count; }
    QCheckBox* fastestBox() const { return m_fastest; }
    QComboBox* startBox() const { return m_start; }
    QPushButton* measureButton() const { return m_btnOnce; }
    QPushButton* seriesButton() const { return m_btnSeries; }
    QPushButton* stopButton() const { return m_btnStop; }
    QPushButton* pauseButton() const { return m_btnPause; }
    PlotWidget* spectrumPlot() const { return m_plot; }
    PlotWidget* trendPlot() const { return m_tplot; }
    QSpinBox* bandBox() const { return m_band; }
    QToolButton* moreButton() const { return m_moreBtn; }   // "⋯" menu of the Last spectrum panel
    QPushButton* addToLibraryButton() const { return m_addLibBtn; }
    QSlider* strengthSlider() const { return m_strength; }
    QMenu* moreMenu() const { return m_moreMenu; }
    QLabel* signalLabel() const { return m_fillPct; }         // "37 %" next to the Signal bar
    QToolButton* processingToggle() const { return m_procToggle; }
    QWidget* processingBody() const { return m_procBody; }
    QComboBox* baselineBox() const { return m_baseline; }   // in the strip above the plot (always visible)
    QCheckBox* showBaselineBox() const { return m_showBaseline; }
    QCheckBox* showEarlierBox() const { return m_showEarlier; }
    QScrollArea* settingsScrollArea() const { return m_settingsScroll; }
    QComboBox* trendModeBox() const { return m_trendMode; }
    const std::deque<lr::Spectrum>& history() const { return m_history; }
    const std::vector<std::pair<double, double>>& trend() const { return m_trend; }
    const lr::Vec& lastPeaks() const { return m_lastPeaks; }
    QString seriesText() const;                       // "Spectrum" row of the Series box
    // Laser-damage note (#9) of the newest spectrum: a row of the Detector box shown only when the check fired.
    QLabel* damageLabel() const { return m_lblDamage; }
    bool damageNoteVisible() const;
    // Measure > Acquire until S/N… (#10) and Measure > Repeated measurements… (#11): own windows, made when first
    // opened (nullptr before). They take sample, folder and spectrum settings from this page.
    SnrDialog* snrDialog() const { return m_snrDialog; }
    RepeatDialog* repeatDialog() const { return m_repeatDialog; }
    // Before a measurement started elsewhere (the dialogs): asks for a sample name if needed and moves to today's
    // folder; false = cancelled.
    bool prepareMeasurement();

    void addSpectrum(const lr::Spectrum& sp) override;   // Python add_spectrum

signals:
    // "Add to library…" (Last spectrum ⋯ menu, plot menu): handled by MainWindow::addToLibrary.
    void addToLibraryRequested(const lr::Spectrum& sp);

public slots:
    void measureOnce();
    void startSeries();
    // Empty or the old default "sample": asks for a name (when dialogs are enabled); false = cancelled.
    // A real name is remembered for the next start of the program.
    bool confirmSampleName();
    void setBand(double value, bool automatic = false);
    void onSeriesProgress(const lr::SeriesProgress& p);
    void setBusy(bool busy);
    void setReady(bool ready);
    void redraw();
    void redrawTrend();
    void recomputeTrend();
    void flushRedraw();                               // a pending (throttled) redraw now
    // "Clear": plot, earlier traces, trend, Series and Last spectrum panels (files and other pages are not
    // touched). The next spectrum opens the default view again.
    void clearAll();
    void addLastToLibrary();                          // emits addToLibraryRequested(newest spectrum)
    void openSnrDialog();
    void openRepeatDialog();
    int redrawCount() const { return m_redraws; }     // tests: plot updates so far

private:
    QWidget* buildSettingsPanel();
    void updateMinInterval();
    void updateEnabled();
    void showFolder();
    void browse();
    void fillPeakMenu();
    void spectrumMenu(const QPointF& data, const QPoint& globalPos);
    void fillMoreMenu(QMenu* m);                      // Clear, Add to library… (⋯ and plot menus)
    double bandValue(const lr::Vec& x, const lr::Vec& y) const;
    double trendTime(const lr::Spectrum& sp) const;
    void resizeEvent(QResizeEvent* e) override;
    void showEvent(QShowEvent* e) override;
    void scheduleRedraw();
    void processingChanged();
    struct Processed {                                // a history entry processed with the current settings
        lr::Vec y, base;
        unsigned stamp = 0;
    };
    const Processed& processedAt(std::size_t i);     // cached (recomputed when the settings changed)

    Controller* m_ctl;
    std::deque<lr::Spectrum> m_history;               // recent spectra (faded display, trend), at most 50
    std::deque<Processed> m_processed;                // parallel to m_history
    unsigned m_procStamp = 1;                         // incremented when the processing settings change
    QTimer* m_redrawTimer = nullptr;
    QElapsedTimer m_sinceRedraw;
    bool m_redrawPending = false;
    int m_redraws = 0;
    Curve* m_trendCurve = nullptr;                    // the trend curve (updated in place)
    std::vector<std::pair<double, double>> m_trend;   // (elapsed s, band value)
    lr::Vec m_lastPeaks;
    bool m_bandAuto = true;
    bool m_viewPending = true;                        // next spectrum: default view 150-3500 cm-1, auto y
    bool m_ready = false, m_busy = false;
    bool m_sync = false;
    QString m_folder;
    bool m_folderAuto = true;                         // follows the date (new folder each day)

    PlotWidget* m_plot;
    Curve* m_curve;
    Curve* m_baseCurve;
    std::vector<Curve*> m_oldCurves;
    PeakLabels* m_peakLabels;

    QLabel *m_lblN, *m_lblNext, *m_lblElapsed, *m_lblEnd;
    QProgressBar* m_bar;
    QSpinBox* m_band;
    QToolButton* m_peakBtn;
    FocusWindow* m_focus = nullptr;           // Focus mode window (made when first opened)
public:
    void openFocus();                         // the Focus mode window (laser on continuously, distance knob)
private:
    QMenu* m_peakMenu;
    QComboBox* m_trendMode;
    PlotWidget* m_tplot;
    QProgressBar* m_fill;
    QLabel* m_fillPct;
    QToolButton* m_moreBtn;
    QPushButton* m_addLibBtn = nullptr;
    QMenu* m_moreMenu;
    QToolButton* m_procToggle;
    QWidget* m_procBody;
    QScrollArea* m_settingsScroll;
    QLabel *m_lblDark, *m_lblSat, *m_lblFluor, *m_lblTime, *m_lblSet, *m_lblPeaks;
    QLabel* m_lblDamage = nullptr;
    QFormLayout* m_detForm = nullptr;
    SnrDialog* m_snrDialog = nullptr;
    RepeatDialog* m_repeatDialog = nullptr;

    QPushButton *m_btnOnce, *m_btnSeries, *m_btnStop, *m_btnPause;
    QPushButton* m_btnClear = nullptr;          // clears plot, trend and panels (files are kept)
    QLineEdit* m_sample;
    ElidedLabel* m_folderLabel;
    QDoubleSpinBox *m_exposure, *m_acq, *m_prebleach, *m_interval, *m_duration;
    QSpinBox *m_averages, *m_count;
    QComboBox *m_background, *m_channel, *m_intervalUnit, *m_stopBy, *m_start;
    QTimeEdit* m_startAt;
    QCheckBox *m_seriesOn, *m_fastest;
    QLabel* m_minLabel;
    QComboBox *m_baseline, *m_normalise, *m_derivative;
    QSlider* m_strength;
    QLabel* m_strengthLabel = nullptr;
    QHBoxLayout* m_plotBar = nullptr;                // baseline and display above the plot
    QCheckBox *m_despike, *m_smoothing, *m_intensity;
    QCheckBox *m_showBaseline = nullptr, *m_showEarlier = nullptr;   // strip above the plot
    QRadioButton *m_showRaw, *m_showCor, *m_showBoth;
};

}  // namespace lr::gui
