#pragma once
// Focus mode: the laser stays on and short spectra come one after another, so the sample distance (the
// instrument's distance knob) can be turned while watching the Raman signal: the strongest band's height and the
// total Raman signal, each with the best value so far, a trend of the last minute and the live spectrum.
// Nothing is saved unless "Keep this spectrum" is pressed. The lid-open accessory most likely makes the lid read
// closed, so the lid may be open without LibreRamen knowing: a red warning stays on whenever the laser is on.
// The laser goes off on Stop, Esc, closing the window, or after the set time.
#include <QDateTime>
#include <QElapsedTimer>
#include <QWidget>
#include <functional>

#include "controller.h"
#include "core/focus.h"

class QDoubleSpinBox;
class QLabel;
class QProgressBar;
class QPushButton;
class QSpinBox;

namespace lr::gui {

class Curve;
class PlotWidget;

class FocusWindow : public QWidget {
    Q_OBJECT
public:
    // folder: where "Keep this spectrum" saves (the Measure page's folder)
    FocusWindow(Controller* ctl, std::function<QString()> folder, std::function<int()> channel, QWidget* parent = nullptr);
    bool running() const { return m_running; }
    QPushButton* startButton() const { return m_start; }   // tests, screenshots

protected:
    void closeEvent(QCloseEvent* e) override;
    void keyPressEvent(QKeyEvent* e) override;

private:
    void startStop();
    void onSpectrum(const lr::Spectrum& sp, const QString& tag);
    void onStatus(const StatusView& st);
    void keep();
    void resetBest();

    Controller* m_ctl;
    std::function<QString()> m_folder;
    std::function<int()> m_channel;
    QLabel* m_banner;
    QDoubleSpinBox* m_exposure;
    QDoubleSpinBox* m_follow;
    QSpinBox* m_minutes;
    QPushButton* m_start;
    QPushButton* m_keep;
    QLabel *m_bandValue, *m_bandBest, *m_totalValue, *m_totalBest, *m_warn;
    QProgressBar *m_bandBar, *m_totalBar;
    PlotWidget* m_plot;
    Curve* m_curve;
    PlotWidget* m_trend;
    Curve *m_trendBand, *m_trendTotal;
    bool m_running = false;
    StatusView m_status;
    lr::Spectrum m_last;
    bool m_haveLast = false;
    double m_bestBand = 0, m_bestTotal = 0;
    QElapsedTimer m_clock;
    lr::Vec m_t, m_band, m_total;
};

}  // namespace lr::gui
