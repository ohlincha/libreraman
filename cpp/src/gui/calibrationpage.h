#pragma once
// The Calibration area (docs/SPECIFICATION.md 5.7). Header: instrument, active calibration, reminder. Tabs:
//  * Calibrate - wizard: 1 choose standard(s) (band lists shown) -> 2 measure each with the instrument
//    (Controller::acquire: ARM check, Stop) or load saved spectra -> automatic peak matching (calib::findPeaks /
//    match against the calibration in use) shown with reference band markers and matched peaks -> 3 fit:
//    polynomial (degree auto or chosen; several standards combined) or one-point offset (silicon; equivalent
//    to a laser-wavelength correction) -> residual per band (table and plot) -> 4 accept (history copy +
//    active file, Instrument::setUserCalibration: effective at once) or reject.
//  * Quick check - measure (or load) one standard and report the deviations of the calibration in use,
//    without changing it.
//  * History - calibration files of the instrument and channel (calibrationstore.h): view, make active,
//    revert to factory.
//  * Instrument log - dark level and calibration drift over time (instrument-log_<serial>.csv; one row per
//    calibration and quick check).
// Reminder: QSettings "calibration/reminderDays" (0 = off); warning on the page and in the log at connect.
// The simulator's calibration files use the serial "SIMULATOR" (never the real instrument's files); the page
// applies its active files when the simulator connects (Cora::connect loads the real instrument's own).
// Measured standards are saved to <configDir>/calibration-spectra/ (provenance; listed in the history file).
#include <QDateTime>
#include <QString>
#include <QStringList>
#include <optional>
#include <vector>

#include "calibrationstore.h"
#include "controller.h"
#include "page.h"

#include "core/calibrate.h"

class QButtonGroup;
class QComboBox;
class QCheckBox;
class QDoubleSpinBox;
class QLabel;
class QListWidget;
class QPlainTextEdit;
class QPushButton;
class QRadioButton;
class QSpinBox;
class QTabWidget;
class QTableWidget;

namespace lr::gui {

class PlotWidget;

class CalibrationPage : public Page {
    Q_OBJECT
public:
    enum class Mode { Polynomial, Offset };

    struct Sample {                                   // one measured or loaded standard
        QString standard;
        Vec y;                                        // intensities in calibration-pixel order
        double dark = qQNaN();                        // mean background (counts per frame)
        double exposure = qQNaN();
        QString origin;                               // "measured 14:03" / file name
        QString file;                                 // saved or loaded spectrum
        std::vector<calib::Match> matches;            // against the calibration in use
    };

    explicit CalibrationPage(const PageContext& ctx, QWidget* parent = nullptr);
    void setController(Controller* c) override;
    void activated() override;

    // ---- instrument context
    QString serial() const;                           // calibration file key ("SIMULATOR" for the simulator)
    int channel() const;                              // 532 / 1064
    calib::ShiftFn currentShift() const;              // calibration in use (user or factory)
    QString activeCalibrationText() const;

    // ---- wizard
    QStringList selectedStandards() const;            // in list order
    void setSelectedStandards(const QStringList& names);
    bool measureStandard(const QString& name);        // starts a measurement; -> sampleReady(name)
    bool loadStandardFile(const QString& name, const QString& path);
    const Sample* sample(const QString& name) const;
    void setMode(Mode m);
    Mode mode() const;
    void setDegree(int degree);                       // 0 = automatic
    bool fit();
    bool hasFit() const { return m_fit.has_value(); }
    const calib::Result& result() const { return *m_fit; }
    bool acceptCalibration();                         // -> history + active file + instrument
    void rejectCalibration();
    QString lastAccepted() const { return m_lastAccepted; }   // history path
    double offset() const { return m_offset; }        // one-point mode: shift correction (cm-1)

    // ---- quick check
    bool quickCheck(const QString& standard);         // measure; -> checkFinished()
    bool quickCheckFile(const QString& standard, const QString& path);
    const std::optional<calstore::CheckResult>& lastCheck() const { return m_check; }
    QString checkVerdict() const;

    // ---- history, log, reminder
    void refreshHistory();
    const std::vector<calstore::HistoryEntry>& historyEntries() const { return m_history; }
    bool makeActive(int row);
    bool revertToFactory();
    void refreshLog();
    const std::vector<calstore::LogEntry>& logEntries() const { return m_log; }
    static int reminderDays();                        // QSettings, default 90; 0 = off
    void setReminderDays(int days);
    QString reminderText() const;                     // empty: no warning

    // ---- widgets (tests, screenshots)
    QTabWidget* tabs() const { return m_tabs; }
    QListWidget* standardsList() const { return m_stdList; }
    QTableWidget* samplesTable() const { return m_samplesTable; }
    QTableWidget* residualTable() const { return m_resTable; }
    QTableWidget* checkTable() const { return m_checkTable; }
    QTableWidget* historyTable() const { return m_histTable; }
    QPushButton* acceptButton() const { return m_btnAccept; }
    QPushButton* measureButton() const { return m_btnMeasure; }
    QDoubleSpinBox* exposureBox() const { return m_exposure; }
    PlotWidget* spectrumPlot() const { return m_plot; }
    PlotWidget* residualPlot() const { return m_resPlot; }
    QComboBox* checkStandardBox() const { return m_checkStd; }

signals:
    void sampleReady(const QString& standard);
    void checkFinished();
    void calibrationChanged();                        // accepted, activated or reverted

private:
    QWidget* buildHeader();
    QWidget* buildWizard();
    QWidget* buildCheck();
    QWidget* buildHistory();
    QWidget* buildLog();
    void onAcquired(const lr::Spectrum& sp, const QString& tag);
    void onConnected();
    void updateHeader();
    void updateEnabled();
    void updateStandardInfo();
    void updateSamplesTable();
    void selectedStandardsChanged();
    void showSample(const QString& name);
    void drawSpectrum(PlotWidget* plot, const Sample& s, const calib::ShiftFn& shift, bool showBands);
    void updateResiduals();
    void showCheck(bool record);
    void showHistoryDetails();
    void invalidateFit();
    void addSample(Sample s);
    std::optional<Sample> readSampleFile(const QString& name, const QString& path);
    QString saveSpectrum(const lr::Spectrum& sp, const QString& standard) const;
    double laserNm() const;
    int pixelCount() const;
    void message(const QString& title, const QString& text);
    void logEvent(calstore::LogEntry e);
    MeasureSettings measureSettings(const QString& standard, double exposure, int scans) const;
    void prepareSimulator(const QString& standard);
    lr::Instrument* instrument() const;

    PageContext m_ctx;
    std::vector<Sample> m_samples;
    QString m_shown;                                  // standard shown in the wizard plot
    std::optional<calib::Result> m_fit;
    QString m_fitKind;
    double m_offset = 0.0;
    std::vector<calstore::Deviation> m_before;        // deviations of the calibration in use at fit time
    QString m_lastAccepted;
    QString m_pending;                                // tag of the running measurement
    std::optional<calstore::CheckResult> m_check;
    std::optional<Sample> m_checkSample;
    std::vector<calstore::HistoryEntry> m_history;
    std::vector<calstore::LogEntry> m_log;
    bool m_busy = false;

    // header
    QLabel* m_hdrInstrument;
    QLabel* m_hdrCal;
    QLabel* m_hdrReminder;
    QSpinBox* m_reminder;
    QTabWidget* m_tabs;
    // wizard
    QListWidget* m_stdList;
    QLabel* m_stdInfo;
    QLabel* m_instructions;
    QDoubleSpinBox* m_exposure;
    QSpinBox* m_scans;
    QTableWidget* m_samplesTable;
    QPushButton* m_btnMeasure;
    QPushButton* m_btnLoad;
    QPushButton* m_btnStop;
    QRadioButton* m_modePoly;
    QRadioButton* m_modeOffset;
    QComboBox* m_degree;
    QPushButton* m_btnFit;
    QLabel* m_fitLabel;
    QPushButton* m_btnAccept;
    QPushButton* m_btnReject;
    QCheckBox* m_newAxis;
    PlotWidget* m_plot;
    PlotWidget* m_resPlot;
    QTableWidget* m_resTable;
    // quick check
    QComboBox* m_checkStd;
    QLabel* m_checkInfo;
    QDoubleSpinBox* m_checkExposure;
    QSpinBox* m_checkScans;
    QDoubleSpinBox* m_tolerance;
    QPushButton* m_btnCheck;
    QPushButton* m_btnCheckFile;
    QLabel* m_verdict;
    PlotWidget* m_checkPlot;
    QTableWidget* m_checkTable;
    // history
    QTableWidget* m_histTable;
    QPlainTextEdit* m_histDetails;
    QPushButton* m_btnActivate;
    QPushButton* m_btnFactory;
    // log
    PlotWidget* m_driftPlot;
    PlotWidget* m_darkPlot;
    QTableWidget* m_logTable;
    QLabel* m_logInfo;
};

}  // namespace lr::gui
