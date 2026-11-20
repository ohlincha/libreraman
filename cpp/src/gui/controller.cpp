#include "controller.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFileInfo>
#include <QStringList>

#include "core/calibration.h"
#include "core/errors.h"
#include "core/simulator.h"
#include "core/watchdog.h"

namespace lr::gui {

namespace {
QString what(const std::exception& e) { return QString::fromUtf8(e.what()); }
}  // namespace

Controller::Controller(QObject* parent) : QObject(parent) {
    qRegisterMetaType<lr::Spectrum>();
    qRegisterMetaType<lr::SeriesProgress>();
    qRegisterMetaType<lr::SeriesSettings>();
    qRegisterMetaType<lr::gui::InstrumentInfo>();
    qRegisterMetaType<lr::gui::MeasureSettings>();
    qRegisterMetaType<lr::gui::StatusView>();
    connect(&m_timer, &QTimer::timeout, this, &Controller::poll);
    m_timer.start(500);
}

Controller::~Controller() { shutdown(false); }

QString Controller::connectionName(Connection c) {
    switch (c) {
    case Connection::Searching: return QStringLiteral("searching");
    case Connection::Initialising: return QStringLiteral("initialising");
    case Connection::Connected: return QStringLiteral("connected");
    case Connection::Lost: return QStringLiteral("lost");
    case Connection::NotFound: return QStringLiteral("not found");
    }
    return QStringLiteral("searching");
}

lr::SimControls* Controller::simControls() const {
    return m_simulated ? dynamic_cast<lr::SimControls*>(m_inst.get()) : nullptr;
}

double Controller::laserSettleS() const { return m_inst ? m_inst->laserSettleS() : 3.6; }

bool Controller::watchdogRunning() const { return m_watchdog && m_watchdog->running(); }

void Controller::setSimulatorSpeed(double speed) {
    m_simSpeed = speed > 0 ? speed : 1.0;
    if (auto* s = simControls())
        s->setSpeed(m_simSpeed);
}

void Controller::setPollInterval(int ms) { m_timer.start(ms); }

QString Controller::savePath(const QString& folder, const QString& sample, const QDateTime& t) {
    const QString base = lr::safeName(sample) + t.toString(QStringLiteral("_yyyyMMdd_HHmmss"));
    QString path = QDir(folder).filePath(base + QStringLiteral(".csv"));
    for (int k = 2; QFileInfo::exists(path); ++k)       // two spectra in the same second: don't overwrite
        path = QDir(folder).filePath(base + QStringLiteral("_%1.csv").arg(k));
    return path;
}

// ------------------------------------------------------------------------------------------------ jobs
bool Controller::run(Job job, bool series) {
    if (m_busy) {
        emit error(QStringLiteral("Busy: stop the current measurement first."));
        return false;
    }
    if (m_worker.joinable())
        m_worker.join();
    m_busy = true;
    m_series = series;
    m_stop = false;
    m_pause = false;
    emit busyChanged(true);
    m_worker = std::thread([this, job = std::move(job)] {
        try {
            job();
        } catch (const lr::Stopped&) {
            emit log(QStringLiteral("Stopped"));
        } catch (const lr::Error& e) {                  // NotArmed, HardwareError, CoraError: user-readable
            emit error(what(e));
        } catch (const std::exception& e) {
            emit error(QStringLiteral("Unexpected error: ") + what(e));
        } catch (...) {
            emit error(QStringLiteral("Unexpected error"));
        }
        QMetaObject::invokeMethod(this, [this] { jobFinished(); }, Qt::QueuedConnection);
    });
    return true;
}

void Controller::jobFinished() {
    if (deliveryPending()) {                           // results still on their way: finish after them
        QMetaObject::invokeMethod(this, [this] { jobFinished(); }, Qt::QueuedConnection);
        return;
    }
    if (m_worker.joinable())
        m_worker.join();
    if (!m_busy)
        return;                                        // shutdown() was first
    m_busy = false;
    m_series = false;
    m_pause = false;
    emit busyChanged(false);
}

void Controller::selectChannel(int ch) {
    if (ch && m_inst && ch != m_inst->channel()) {
        emit log(QStringLiteral("Switching to %1 nm").arg(ch));
        m_inst->selectChannel(ch);
    }
}

// ------------------------------------------------------------------------------------------------ connection
InstrumentInfo Controller::makeInfo(const QString& serial) const {
    InstrumentInfo i;
    i.simulated = m_simulated;
    i.serial = serial;
    QStringList cal;
    for (const auto& kv : m_inst->userCalibration())
        cal << QStringLiteral("%1 nm: %2").arg(kv.first).arg(kv.second.date);
    i.calibration = cal.isEmpty() ? QStringLiteral("factory") : cal.join(QStringLiteral(", "));
    return i;
}

void Controller::connectInstrument(bool simulate) {
    if (m_busy) {
        emit error(QStringLiteral("Busy: stop the current measurement first."));
        return;
    }
    stopWatchdog();
    auto old = std::make_shared<std::unique_ptr<lr::Instrument>>(std::move(m_inst));
    m_simulated = false;
    m_info = {};
    m_conn = Connection::Searching;
    const double speed = m_simSpeed;
    run([this, simulate, speed, old] {
        if (*old) {                                    // reconnect: release the old instrument (laser off)
            try {
                (*old)->close();
            } catch (const std::exception& e) {
                emit log(QStringLiteral("Closing the previous connection: ") + what(e));
            }
            old->reset();
        }
        m_conn = Connection::Searching;
        const lr::LogFn logFn = [this](const QString& m) { emit log(m); };
        auto inst = std::make_shared<std::unique_ptr<lr::Instrument>>();
        try {
            *inst = simulate ? lr::makeSimulator(lr::defaultCalibration(), logFn, speed)
                             : lr::makeCora(lr::defaultCalibration(), logFn);
            (*inst)->connect();
        } catch (const std::exception& e) {
            m_conn = Connection::NotFound;
            emit connectFailed(what(e));
            return;
        }
        m_conn = Connection::Initialising;
        try {
            if ((*inst)->needsInitialize())
                (*inst)->initialize([this](double f) { emit initProgress(f); });
            else
                (*inst)->resume();             // already powered up: assume the 532 nm path
        } catch (const std::exception& e) {
            m_conn = Connection::NotFound;
            try {
                (*inst)->close();
            } catch (...) {
            }
            emit connectFailed(QStringLiteral("Initialisation failed: ") + what(e));
            return;
        }
        emit initProgress(1.0);
        QString serial = QStringLiteral("SIMULATOR");
        if (!simulate) {
            try {
                serial = (*inst)->serial();            // spectrometer EEPROM (USB): not in the GUI thread
            } catch (const std::exception& e) {
                serial = QStringLiteral("?");
                emit log(QStringLiteral("Could not read the serial number: ") + what(e));
            }
        }
        QMetaObject::invokeMethod(
            this,
            [this, inst, simulate, serial] {
                m_inst = std::move(*inst);
                m_closed = false;
                m_simulated = simulate;
                m_conn = Connection::Connected;
                m_info = makeInfo(serial);
                startWatchdog();
                poll();
                emit connected(m_info);
            },
            Qt::QueuedConnection);
    });
}

void Controller::disconnectInstrument() {
    if (m_busy) {
        emit error(QStringLiteral("Busy: stop the current measurement first."));
        return;
    }
    stopWatchdog();
    if (m_inst) {
        try {
            m_inst->close();                           // release the USB devices
        } catch (const std::exception& e) {
            emit log(QStringLiteral("Closing the connection: ") + what(e));
        }
    }
    m_inst.reset();
    m_simulated = false;
    m_conn = Connection::NotFound;
    poll();
}

// ------------------------------------------------------------------------------------------------ status
void Controller::poll() {
    StatusView st;
    st.connection = connectionName(m_conn);
    st.simulated = m_simulated;
    if (m_inst && m_conn == Connection::Connected) {
        try {
            const lr::Status s = m_inst->status();
            st.armed = s.armed;
            st.compartmentOpen = s.compartment_open;
            st.laserOn = s.laser_on;
            st.channel = s.channel;
            ping(s.laser_on);
            if (++m_ka % 4 == 0)
                m_inst->keepalive();
        } catch (const std::exception& e) {
            m_conn = Connection::Lost;
            st.connection = connectionName(m_conn);
            emit log(QStringLiteral("Connection lost: ") + what(e));
        }
        st.model = m_info.model;
        st.serial = m_info.serial;
    }
    m_last = st;
    emit status(st);
}

void Controller::ping(bool laserOn) {
    if (!m_watchdog)
        return;
    if (!m_watchdog->ping(laserOn, m_inst ? m_inst->driverPort() : QString()))
        m_watchdog.reset();
}

void Controller::startWatchdog() {
    if (m_simulated || !m_inst || m_inst->ioPort().isEmpty())
        return;
    try {
        m_watchdog = lr::watchdog::WatchdogClient::start(m_inst->ioPort());
        emit log(QStringLiteral("Laser watchdog running"));
    } catch (const std::exception& e) {
        emit log(QStringLiteral("Could not start the laser watchdog: ") + what(e));
    }
}

void Controller::stopWatchdog() {
    if (m_watchdog) {
        m_watchdog->quit();
        m_watchdog.reset();
    }
}

// ------------------------------------------------------------------------------------------------ measuring
namespace {
QString num(double d) { return QString::number(d, 'g', 6); }
}  // namespace

bool Controller::measureOnce(const MeasureSettings& s, const QString& origin) {
    if (!m_inst || m_conn != Connection::Connected) {
        emit error(QStringLiteral("No instrument connected."));
        return false;
    }
    if (m_busy) {
        emit error(QStringLiteral("Busy: stop the current measurement first."));
        return false;
    }
    emit log(QStringLiteral("%1: measure \"%2\": %3 s × %4, background %5%6")
                 .arg(origin, s.sample, num(s.exposure_s))
                 .arg(s.averages)
                 .arg(s.background, s.prebleach_s > 0 ? QStringLiteral(", pre-bleach %1 s").arg(num(s.prebleach_s)) : QString()));
    return run([this, s] {
        selectChannel(s.channel);
        lr::AcquireOptions o;
        o.exposure_s = s.exposure_s;
        o.averages = s.averages;
        o.background = s.background != QLatin1String("none");
        o.prebleach_s = s.prebleach_s;
        o.stop = &m_stop;
        lr::Spectrum sp = m_inst->acquire(o);
        sp.setMeta(QStringLiteral("sample"), s.sample);
        if (!QDir().mkpath(s.folder))
            throw lr::Error(("Cannot create the folder " + s.folder).toStdString());
        const QString path = savePath(s.folder, s.sample, sp.t);
        sp.saveCsv(path);
        sp.setMeta(QStringLiteral("file"), path);
        emit log(QStringLiteral("Saved ") + QDir::toNativeSeparators(path));
        emit spectrum(sp);
    });
}

bool Controller::acquire(const MeasureSettings& s, const QString& tag) {
    if (!m_inst || m_conn != Connection::Connected) {
        emit error(QStringLiteral("No instrument connected."));
        return false;
    }
    return run([this, s, tag] {
        selectChannel(s.channel);
        lr::AcquireOptions o;
        o.exposure_s = s.exposure_s;
        o.averages = s.averages;
        o.background = s.background != QLatin1String("none");
        o.prebleach_s = s.prebleach_s;
        o.stop = &m_stop;
        lr::Spectrum sp = m_inst->acquire(o);
        sp.setMeta(QStringLiteral("sample"), s.sample);
        emit acquired(sp, tag);
    });
}

bool Controller::startSeries(const lr::SeriesSettings& s, int channel, const QString& origin) {
    if (!m_inst || m_conn != Connection::Connected) {
        emit error(QStringLiteral("No instrument connected."));
        return false;
    }
    if (m_busy) {
        emit error(QStringLiteral("Busy: stop the current measurement first."));
        return false;
    }
    emit log(QStringLiteral("%1: series \"%2\": %3 s × %4 every %5 s, %6%7")
                 .arg(origin, s.sample, num(s.exposure_s))
                 .arg(s.averages)
                 .arg(num(s.interval_s),
                      s.stopByCount ? QStringLiteral("%1 spectra").arg(s.count)
                                    : QStringLiteral("for %1 h").arg(num(s.duration_s / 3600)),
                      s.laserStaysOn ? QStringLiteral(", laser stays on") : QString()));
    return run(
        [this, s, channel] {
            selectChannel(channel);
            lr::SeriesCallbacks cb;
            cb.spectrum = [this](const lr::Spectrum& sp) { postSpectrum(sp); };
            cb.progress = [this](const lr::SeriesProgress& p) { postProgress(p); };
            cb.log = [this](const QString& m) { emit log(m); };
            lr::runTimeSeries(*m_inst, s, m_stop, cb, &m_pause);
        },
        true);
}

void Controller::postSpectrum(const lr::Spectrum& sp) {
    const std::lock_guard<std::mutex> lock(m_pendingMutex);
    m_pendingSpectra.push_back(sp);
    postDelivery();
}

void Controller::postProgress(const lr::SeriesProgress& p) {
    const std::lock_guard<std::mutex> lock(m_pendingMutex);
    m_pendingProgress = p;                             // only the newest counts
    postDelivery();
}

void Controller::postDelivery() {
    if (m_deliveryPosted)
        return;
    m_deliveryPosted = true;
    QMetaObject::invokeMethod(this, [this] { deliver(); }, Qt::QueuedConnection);
}

void Controller::deliver() {
    // Emits pending spectra for at most kDeliverBudgetMs (the pages' work per spectrum adds up when a fast
    // series outpaces the GUI), then yields to the event loop and continues in a new pass. The progress is
    // emitted once its spectra are out.
    QElapsedTimer t;
    t.start();
    for (;;) {
        lr::Spectrum sp;
        std::optional<lr::SeriesProgress> progress;
        bool last = false;
        {
            const std::lock_guard<std::mutex> lock(m_pendingMutex);
            if (m_pendingSpectra.empty()) {
                progress.swap(m_pendingProgress);
                m_deliveryPosted = false;
                last = true;
            } else if (t.elapsed() >= kDeliverBudgetMs) {
                QMetaObject::invokeMethod(this, [this] { deliver(); }, Qt::QueuedConnection);   // still posted
                return;
            } else {
                sp = std::move(m_pendingSpectra.front());
                m_pendingSpectra.pop_front();
            }
        }
        if (last) {
            if (progress)
                emit seriesProgress(*progress);
            return;
        }
        emit spectrum(sp);
    }
}

bool Controller::deliveryPending() {
    const std::lock_guard<std::mutex> lock(m_pendingMutex);
    return m_deliveryPosted;
}

void Controller::stop() {
    m_stop = true;
    m_pause = false;
    if (m_inst) {
        try {
            m_inst->laserOffNow();
        } catch (...) {
        }
    }
}

void Controller::setPaused(bool paused) {
    if (m_pause.exchange(paused) != paused && isSeriesRunning())
        emit log(paused ? QStringLiteral("Series: pausing before the next spectrum")
                        : QStringLiteral("Series: resuming"));
}

void Controller::shutdown(bool powerDown) {
    m_timer.stop();
    stop();
    if (m_worker.joinable())
        m_worker.join();                               // the job ends at its next stop check
    if (m_busy) {
        m_busy = false;
        m_series = false;
        emit busyChanged(false);
    }
    stopWatchdog();
    if (m_inst && !m_closed) {
        m_closed = true;                               // (closeEvent and the destructor both call this)
        try {
            m_inst->close(powerDown);
        } catch (...) {
        }
    }
}

// ------------------------------------------------------------------------------------------------ simulator
void Controller::simPressArm() {
    if (auto* s = simControls())
        s->pressArm();
}

void Controller::simOpenCompartment() {
    if (auto* s = simControls())
        s->openCompartment();
}

void Controller::simCloseCompartment() {
    if (auto* s = simControls())
        s->closeCompartment();
}

void Controller::simSetSample(const QString& name) {
    if (auto* s = simControls()) {
        try {
            s->setSample(name);
        } catch (const std::exception& e) {
            emit error(what(e));
        }
    }
}

}  // namespace lr::gui
