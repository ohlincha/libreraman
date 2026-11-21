#pragma once
// Main window (Python gui/app.py MainWindow): status strip with STOP, simulator bar, side bar with the
// areas (Measure, Data, Library, Analysis, Calibration; see page.h for plugging in pages), log view at the
// bottom, File and Instrument menus.
//
// Closing: asks whether to stop a running measurement, and (real instrument) whether to shut the instrument
// down or leave it on, with "Remember my choice" (QSettings "instrument/onClose": ask | shutdown | leave;
// also selectable in Instrument > When closing). With dialogsEnabled() == false (tests, self-test) nothing
// is asked: a running measurement is stopped and the remembered choice is used ("ask" = leave on).
//
// Local API (apiserver.h): the window owns the ApiServer; main() starts it (startApi()) unless switched off
// (Instrument > Local API; QSettings "api/enabled", default on, "api/port", default 8765). Remote clients are
// read-only unless Instrument > Allow remote measurements is checked (QSettings "api/allowRemoteControl",
// default off). The REMOTE tile of the status strip shows off / read-only / control.
#include <QMainWindow>
#include <QString>
#include <vector>

#include "controller.h"

class QAction;
class QComboBox;
class QPlainTextEdit;
class QStackedWidget;
class QToolButton;

namespace lr::gui {

class ApiServer;
class MeasurePage;
class Page;
class StatusStrip;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    enum class OnClose { Ask, ShutDown, LeaveOn };

    explicit MainWindow(Controller* ctl, QWidget* parent = nullptr);

    Controller* controller() const { return m_ctl; }
    MeasurePage* measurePage() const { return m_measure; }
    Page* page(const QString& area) const;            // nullptr for unknown areas
    StatusStrip* statusStrip() const { return m_strip; }
    QWidget* simulatorBar() const { return m_simBar; }
    QComboBox* simulatorSample() const { return m_simSample; }
    QPlainTextEdit* logView() const { return m_log; }
    int currentArea() const;
    InstrumentInfo instrumentInfo() const { return m_info; }

    ApiServer* apiServer() const { return m_api; }
    QAction* apiAction() const { return m_apiAction; }
    QAction* remoteControlAction() const { return m_remoteAction; }
    QAction* shutDownAction() const { return m_shutDownAction; }
    QString apiError() const { return m_apiError; }   // why the local API is not running (empty: running/off)
    // Starts the local API on 127.0.0.1 (port < 0: QSettings "api/port"; apiFile empty: the default API file).
    bool startApi(int port = -1, const QString& apiFile = {});
    void stopApi();

    static OnClose onClosePolicy();
    static void setOnClosePolicy(OnClose p);

public slots:
    void showArea(int k);
    void addLog(const QString& msg);
    void connectInstrument(bool simulate);
    void repairDrivers();
    void loadFactoryCalibration();   // Instrument > Load factory calibration… (BS_Device_Settings.ini)                             // Windows: re-install the USB drivers, then reconnect
    void fitToScreen();
    // "Add to library…" from another page (Measure): the Library area stores it (Page::addToLibrary).
    Q_INVOKABLE bool addToLibrary(const lr::Spectrum& sp);   // invokable: DataPageHost calls it by name
    // Instrument > Shut down instrument…: confirms (dialogsEnabled), stops a running measurement, then the
    // vendor power-down (real instrument) or a plain disconnect (simulator). Logs "Instrument shut down".
    void shutDownInstrument();
    void raiseWindow();                               // brought to the front (a second instance was started)

private slots:
    void onStatus(const lr::gui::StatusView& st);
    void onError(const QString& msg);
    void onConnected(const lr::gui::InstrumentInfo& info);
    void onConnectFailed(const QString& msg);
    void onSpectrum(const lr::Spectrum& sp);

protected:
    void closeEvent(QCloseEvent* e) override;

private:
    void buildMenus();
    QWidget* buildSimulatorBar();
    void updateTitle(bool laserOn);
    void updateRemoteTile();

    Controller* m_ctl;
    InstrumentInfo m_info;
    StatusStrip* m_strip;
    QWidget* m_simBar;
    QComboBox* m_simSample = nullptr;
    QStackedWidget* m_stack;
    MeasurePage* m_measure;
    std::vector<Page*> m_pages;                       // in pageAreas() order (m_pages[0] == m_measure)
    std::vector<QToolButton*> m_sideButtons;
    QPlainTextEdit* m_log;
    bool m_laserTitle = false;
    bool m_shutDown = false;
    ApiServer* m_api = nullptr;
    QAction* m_apiAction = nullptr;
    QAction* m_remoteAction = nullptr;
    QAction* m_shutDownAction = nullptr;
    QString m_apiError;
    void finishShutDown();
};

}  // namespace lr::gui
