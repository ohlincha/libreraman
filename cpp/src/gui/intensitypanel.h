#pragma once
// "Intensity" tab of the Calibration area (issue #16): measure or load a reference with a known emission curve
// (NIST SRM 2242 glass, a tungsten-halogen lamp as a blackbody, or a table), compute the instrument-response
// correction (core/response.h), inspect it (measured vs true curve, correction factor next to the one in use),
// accept it (history copy + active file) or remove the active one. Measure and Data apply the active correction
// when "intensity correction" is ticked there.
#include <QWidget>
#include <functional>
#include <optional>

#include "core/response.h"

class QComboBox;
class QDoubleSpinBox;
class QLabel;
class QPushButton;
class QSpinBox;

namespace lr {
struct ChannelCal;
}

namespace lr::gui {

class Controller;
class PlotWidget;

class IntensityPanel : public QWidget {
    Q_OBJECT
public:
    struct Host {                                      // supplied by the Calibration page
        std::function<QString()> serial;               // file key ("SIMULATOR" for the simulator)
        std::function<int()> channel;
        std::function<double()> laserNm;
        std::function<const ChannelCal*()> channelCal; // for the SRM coefficients and range (may be nullptr)
        std::function<Controller*()> controller;       // nullptr or not connected: measuring disabled
        std::function<bool()> ready;                   // instrument connected and idle
        std::function<void(const QString&)> log;
    };

    explicit IntensityPanel(Host host, QWidget* parent = nullptr);
    void setController(Controller* c);                 // listens for its own acquisitions
    void refresh();                                    // instrument or channel changed

    response::Reference reference() const;             // as chosen in the form
    void setReferenceKind(response::Reference::Kind k);
    void setTemperature(double kelvin);
    bool setTableFile(const QString& path);
    bool measure();                                    // -> result shown when the spectrum arrives
    bool loadFile(const QString& path);                // a saved spectrum of the reference
    const std::optional<response::Response>& pending() const { return m_pending; }
    bool acceptPending();
    bool removeActive();
    QString inUseText() const;

    QPushButton* measureButton() const { return m_btnMeasure; }
    QPushButton* acceptButton() const { return m_btnAccept; }
    QLabel* resultLabel() const { return m_result; }
    PlotWidget* curvePlot() const { return m_curvePlot; }
    PlotWidget* factorPlot() const { return m_factorPlot; }

signals:
    void responseChanged();                            // accepted or removed

private:
    void onAcquired(const lr::Spectrum& sp, const QString& tag);
    void evaluate(const Vec& x, const Vec& y, double rawMax, double exposure, int averages, const QString& file);
    void updateForm();
    void updateEnabled();
    void redraw();

    Host m_host;
    Controller* m_ctl = nullptr;
    bool m_busy = false;
    response::Reference m_table;                       // last table read
    std::optional<response::Response> m_pending;
    Vec m_x, m_y;                                      // reference spectrum shown

    QComboBox* m_kind;
    QDoubleSpinBox* m_temp;
    QPushButton* m_btnTable;
    QLabel* m_tableLabel;
    QDoubleSpinBox* m_exposure;
    QSpinBox* m_scans;
    QPushButton* m_btnMeasure;
    QPushButton* m_btnLoad;
    QLabel* m_result;
    QPushButton* m_btnAccept;
    QPushButton* m_btnDiscard;
    QLabel* m_inUse;
    QPushButton* m_btnRemove;
    PlotWidget* m_curvePlot;
    PlotWidget* m_factorPlot;
};

}  // namespace lr::gui
