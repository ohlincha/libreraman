#pragma once
// Niche acquisition modes, opened from the Measure menu (they add no controls to the Measure page):
//
// * SnrDialog, "Acquire until S/N…" (#10): frames are accumulated until a target S/N of one band (or the
//   strongest), a time limit, or the laser-damage check (#9) stops them; the running S/N is shown.
//   Controller::measureUntilSnr does the work (core/acqcheck.h).
// * RepeatDialog, "Repeated measurements…" (#11): N spectra of one vial with the Measure page's settings, with a
//   prompt to shake or rotate the vial between them; then mean ± sd of the spectra and the band results, outliers
//   flagged, spectra can be excluded (the mean and the summary are saved again). core/repeats.h does the work.
//
// Both take the sample name, folder, background and laser from the Measure page; the spectra are saved and shown
// like single measurements.
#include <QString>
#include <QWidget>
#include <vector>

#include "controller.h"
#include "core/repeats.h"

class QCheckBox;
class QDoubleSpinBox;
class QFrame;
class QLabel;
class QProgressBar;
class QPushButton;
class QSpinBox;
class QTableWidget;

namespace lr::gui {

class MeasurePage;
class PlotWidget;

class SnrDialog : public QWidget {
    Q_OBJECT
public:
    SnrDialog(Controller* ctl, MeasurePage* page, QWidget* parent = nullptr);
    bool running() const { return m_running; }
    QDoubleSpinBox* targetBox() const { return m_target; }
    QDoubleSpinBox* bandBox() const { return m_band; }          // 0 = strongest
    QDoubleSpinBox* exposureBox() const { return m_exposure; }
    QDoubleSpinBox* minutesBox() const { return m_minutes; }
    QCheckBox* stopOnDamageBox() const { return m_stopOnDamage; }
    QPushButton* startButton() const { return m_start; }
    QPushButton* stopButton() const { return m_stop; }
    QLabel* statusLabel() const { return m_status; }
    QLabel* resultLabel() const { return m_result; }

public slots:
    void start();

private:
    void onProgress(const lr::acq::SnrProgress& p);
    void onBusy(bool busy);

    Controller* m_ctl;
    MeasurePage* m_page;
    bool m_running = false;
    QDoubleSpinBox *m_target, *m_band, *m_exposure, *m_minutes;
    QCheckBox* m_stopOnDamage;
    QPushButton *m_start, *m_stop;
    QProgressBar* m_bar;
    QLabel *m_status, *m_result;
};

class RepeatDialog : public QWidget {
    Q_OBJECT
public:
    RepeatDialog(Controller* ctl, MeasurePage* page, QWidget* parent = nullptr);
    bool running() const { return m_running; }
    bool waitingForVial() const { return m_waiting; }       // the "shake or rotate the vial" prompt is up
    QSpinBox* countBox() const { return m_count; }
    QPushButton* startButton() const { return m_start; }
    QPushButton* continueButton() const { return m_continue; }
    QPushButton* finishButton() const { return m_finish; }
    QTableWidget* spectraTable() const { return m_table; }  // column kUse: check box (included)
    QTableWidget* bandsTable() const { return m_bands; }
    QLabel* statusLabel() const { return m_status; }
    const std::vector<lr::Spectrum>& spectra() const { return m_spectra; }
    const lr::repeats::Result& result() const { return m_result; }
    QString meanPath() const { return m_meanPath; }         // saved files (empty before the end)
    QString summaryPath() const { return m_summaryPath; }
    static constexpr int kUse = 0;

public slots:
    void start();
    void measureNext();                                     // Continue
    void finish();                                          // Finish now / after the last measurement
    void setExcluded(int index, bool excluded);

private:
    void onSpectrum(const lr::Spectrum& sp);
    void onBusy(bool busy);
    void analyseAndSave();
    void fillTables();
    void drawPlot();

    Controller* m_ctl;
    MeasurePage* m_page;
    bool m_running = false, m_waiting = false, m_gotOne = false, m_filling = false;
    int m_total = 0;
    QString m_group;
    MeasureSettings m_settings;
    std::vector<lr::Spectrum> m_spectra;
    std::vector<bool> m_exclude;
    lr::repeats::Result m_result;
    QString m_meanPath, m_summaryPath;

    QSpinBox* m_count;
    QPushButton *m_start, *m_continue, *m_finish, *m_stop;
    QFrame* m_prompt;
    QLabel *m_promptText, *m_status, *m_files;
    QTableWidget *m_table, *m_bands;
    PlotWidget* m_plot;
};

}  // namespace lr::gui
