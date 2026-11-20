#pragma once
// The Measure area (Python gui/app.py MeasurePage): live spectrum with faded previous spectra, peak labels
// and the processing preview (baseline drawn), series progress, trend of one band (or waterfall), detector
// and last-spectrum panels, and at the right the settings panel with MEASURE / START SERIES / Stop.
#include <QDateTime>
#include <QString>
#include <deque>
#include <utility>
#include <vector>

#include "controller.h"
#include "page.h"

class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QLineEdit;
class QMenu;
class QProgressBar;
class QPushButton;
class QRadioButton;
class QSlider;
class QSpinBox;
class QTimeEdit;
class QToolButton;

namespace lr::gui {

class Curve;
class ElidedLabel;
class PeakLabels;
class PlotWidget;

class MeasurePage : public Page {
    Q_OBJECT
public:
    explicit MeasurePage(Controller* ctl, QWidget* parent = nullptr);

    // ---- settings as entered
    MeasureSettings measureSettings() const;          // single spectrum ("once" background -> "each")
    lr::SeriesSettings seriesSettings() const;
    int channel() const;                              // 532 / 1064
    lr::processing::Settings processingSettings() const;   // Python proc_settings()
    double minInterval() const;                       // shortest possible interval with these settings (s)
    double startDelay() const;                        // from the Start choice (s)
    QString folder() const { return m_folder; }
    void setFolder(const QString& path);              // fixed folder (no automatic per-day folder)
    static QString defaultFolder(const QDate& day = QDate::currentDate());   // ~/Documents/LibreRamen/<date>

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
    QComboBox* trendModeBox() const { return m_trendMode; }
    const std::deque<lr::Spectrum>& history() const { return m_history; }
    const std::vector<std::pair<double, double>>& trend() const { return m_trend; }
    const lr::Vec& lastPeaks() const { return m_lastPeaks; }
    QString seriesText() const;                       // "Spectrum" row of the Series box

    void addSpectrum(const lr::Spectrum& sp) override;   // Python add_spectrum

public slots:
    void measureOnce();
    void startSeries();
    void setBand(double value, bool automatic = false);
    void onSeriesProgress(const lr::SeriesProgress& p);
    void setBusy(bool busy);
    void setReady(bool ready);
    void redraw();
    void redrawTrend();
    void recomputeTrend();

private:
    QWidget* buildSettingsPanel();
    void updateMinInterval();
    void updateEnabled();
    void showFolder();
    void browse();
    void fillPeakMenu();
    void spectrumMenu(const QPointF& data, const QPoint& globalPos);
    double bandValue(const lr::Vec& x, const lr::Vec& y) const;
    double trendTime(const lr::Spectrum& sp) const;
    void resizeEvent(QResizeEvent* e) override;

    Controller* m_ctl;
    std::deque<lr::Spectrum> m_history;               // recent spectra (faded display, trend), at most 50
    std::vector<std::pair<double, double>> m_trend;   // (elapsed s, band value)
    lr::Vec m_lastPeaks;
    bool m_bandAuto = true;
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
    QMenu* m_peakMenu;
    QComboBox* m_trendMode;
    PlotWidget* m_tplot;
    QProgressBar* m_fill;
    QLabel *m_lblDark, *m_lblSat, *m_lblTime, *m_lblSet, *m_lblPeaks;

    QPushButton *m_btnOnce, *m_btnSeries, *m_btnStop, *m_btnPause;
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
    QCheckBox *m_despike, *m_smoothing;
    QRadioButton *m_showRaw, *m_showCor, *m_showBoth;
};

}  // namespace lr::gui
