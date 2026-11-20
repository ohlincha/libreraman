#pragma once
// Runs the instrument for the GUI (Python gui/controller.py). Hardware jobs (connect, measure, series) run
// one at a time in a worker thread; results come back to the GUI thread as queued signals. The status is
// polled every 500 ms from the GUI thread (also during measurements: Instrument::status() is thread-safe),
// with a keep-alive every 4th poll and a heartbeat to the laser watchdog process.
//
// Thread safety: the Controller lives in the GUI thread; call its methods from there only. Signals may be
// emitted from the worker thread (connect receivers normally: Qt queues them to the GUI thread).
//
// Series results (spectrum, seriesProgress) do not go through one queued event each: the worker appends them
// to a pending list and posts at most one delivery at a time. A delivery emits pending spectra in order for
// at most kDeliverBudgetMs, then yields to the event loop (and continues in the next pass); the newest
// progress is emitted after the last pending spectrum (stale progress is dropped). A GUI that falls behind a
// fast series stays responsive and catches up; busyChanged(false) comes after every result.
#include <QMetaType>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <atomic>
#include <functional>
#include <memory>
#include <deque>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

#include "core/instrument.h"
#include "core/series.h"
#include "core/spectrum.h"
#include "statusstrip.h"

namespace lr {
class SimControls;
namespace watchdog {
class WatchdogClient;
}
}  // namespace lr

namespace lr::gui {

struct InstrumentInfo {                       // Python connected(dict)
    QString model = QStringLiteral("Cora 5000");
    QString serial;
    bool simulated = false;
    QString calibration;                      // "532 nm: 2026-11-20, ..." or "factory"
};

struct MeasureSettings {                      // Python MeasurePage.settings() for a single spectrum
    QString sample = QStringLiteral("sample");
    QString folder;                           // files are saved here (created if needed)
    double exposure_s = 1.0;
    int averages = 1;
    QString background = QStringLiteral("each");   // "each" | "none"
    double prebleach_s = 0.0;
    int channel = 0;                          // 532 / 1064; 0 = keep the current channel
};

class Controller : public QObject {
    Q_OBJECT
public:
    enum class Connection { Searching, Initialising, Connected, Lost, NotFound };

    explicit Controller(QObject* parent = nullptr);
    ~Controller() override;                   // stop(), waits for the worker, closes the instrument

    // ---- state (GUI thread)
    lr::Instrument* instrument() const { return m_inst.get(); }
    lr::SimControls* simControls() const;     // nullptr unless connected to the simulator
    bool simulated() const { return m_simulated; }
    Connection connection() const { return m_conn.load(); }
    static QString connectionName(Connection c);   // "searching", "initialising", "connected", "lost", "not found"
    bool isBusy() const { return m_busy; }
    bool isSeriesRunning() const { return m_busy && m_series; }
    bool isPaused() const { return m_pause.load(); }
    const InstrumentInfo& info() const { return m_info; }
    StatusView lastStatus() const { return m_last; }
    double laserSettleS() const;              // of the connected instrument (3.6 s if none)
    bool watchdogRunning() const;

    // Simulator speed (all waits divided by it) for simulators created from now on, and the current one.
    void setSimulatorSpeed(double speed);
    double simulatorSpeed() const { return m_simSpeed; }
    void setPollInterval(int ms);             // default 500

    // Python _save: <folder>/<safe sample>_yyyyMMdd_HHmmss.csv (time of the spectrum); sets meta "file".
    static QString savePath(const QString& folder, const QString& sample, const QDateTime& t);

public slots:
    // Searches, connects and initialises (or resumes) in the worker thread; the old instrument is closed.
    // -> connected(info) or connectFailed(message)
    void connectInstrument(bool simulate);
    // Closes the instrument (laser off) and forgets it (Windows driver repair). Refused while busy.
    void disconnectInstrument();
    // origin: who asked ("GUI", "API (cli)", ...); logged with the settings (spec: every action with origin).
    bool measureOnce(const lr::gui::MeasureSettings& s, const QString& origin = QStringLiteral("GUI"));
    // One spectrum for the Calibration page: like measureOnce (channel, ARM check -> error(), Stop) but not
    // saved and not passed to the pages; delivered by acquired(sp, tag) instead of spectrum().
    bool acquire(const lr::gui::MeasureSettings& s, const QString& tag);
    // Time series (runTimeSeries); channel 0 keeps the current channel.
    bool startSeries(const lr::SeriesSettings& s, int channel = 0, const QString& origin = QStringLiteral("GUI"));
    // Stop button: stop flag + laser(s) off right away, without waiting for the running job.
    void stop();
    void setPaused(bool paused);              // series waits before its next spectrum while paused
    // Before quitting: stop, wait for the worker, watchdog "quit", close the instrument (powerDown: vendor
    // shutdown, the instrument switches off).
    void shutdown(bool powerDown = false);

    // Simulator controls (ignored unless connected to the simulator)
    void simPressArm();
    void simOpenCompartment();
    void simCloseCompartment();
    void simSetSample(const QString& name);

    void poll();                              // status poll (timer; public for tests)

signals:
    void status(const lr::gui::StatusView& st);
    void log(const QString& msg);
    void connected(const lr::gui::InstrumentInfo& info);
    void connectFailed(const QString& msg);
    void initProgress(double fraction);
    void spectrum(const lr::Spectrum& sp);
    void acquired(const lr::Spectrum& sp, const QString& tag);   // result of acquire()
    void seriesProgress(const lr::SeriesProgress& p);
    void busyChanged(bool busy);
    void error(const QString& msg);           // user-readable

private:
    using Job = std::function<void()>;
    bool run(Job job, bool series = false);   // false (and error()) if a job is running
    void jobFinished();
    void startWatchdog();
    void stopWatchdog();
    void ping(bool laserOn);
    void selectChannel(int ch);               // worker thread
    InstrumentInfo makeInfo(const QString& serial) const;
    // worker thread: queue a series result for the GUI thread (see the top of this file)
    void postSpectrum(const lr::Spectrum& sp);
    void postProgress(const lr::SeriesProgress& p);
    void postDelivery();                      // with m_pendingMutex held
    void deliver();                           // GUI thread
    bool deliveryPending();
    static constexpr int kDeliverBudgetMs = 30;

    std::unique_ptr<lr::Instrument> m_inst;
    bool m_closed = false;                    // shutdown() closed m_inst
    std::atomic<Connection> m_conn{Connection::Searching};
    bool m_simulated = false;
    InstrumentInfo m_info;
    StatusView m_last;
    double m_simSpeed = 1.0;

    std::thread m_worker;
    bool m_busy = false;                      // GUI thread
    bool m_series = false;
    std::atomic<bool> m_stop{false}, m_pause{false};

    std::mutex m_pendingMutex;                // worker -> GUI results of a series
    std::deque<lr::Spectrum> m_pendingSpectra;
    std::optional<lr::SeriesProgress> m_pendingProgress;
    bool m_deliveryPosted = false;

    QTimer m_timer;
    int m_ka = 0;
    std::unique_ptr<lr::watchdog::WatchdogClient> m_watchdog;
};

}  // namespace lr::gui

Q_DECLARE_METATYPE(lr::Spectrum)
Q_DECLARE_METATYPE(lr::SeriesProgress)
Q_DECLARE_METATYPE(lr::SeriesSettings)
Q_DECLARE_METATYPE(lr::gui::InstrumentInfo)
Q_DECLARE_METATYPE(lr::gui::MeasureSettings)
Q_DECLARE_METATYPE(lr::gui::StatusView)
