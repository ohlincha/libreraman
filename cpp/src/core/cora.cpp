#include "cora.h"

#include <QElapsedTimer>
#include <QThread>
#include <exception>

#include "errors.h"
#include "hw/discovery.h"
#include "hw/pl2303usb.h"
#include "pyformat.h"
#include "windrivers.h"

namespace lr {

const std::vector<LineState> INIT_532 = {{"01", true},  {"00", true}, {"0a", false}, {"0e", true}, {"0c", false},
                                         {"0d", false}, {"0b", true}, {"08", true},  {"07", true}, {"0b", false}};
const std::vector<LineState> TO_1064_POWER = {{"0b", true}, {"07", false}, {"0b", false}};   // powers the driver
const std::vector<LineState> TO_1064_PATH = {{"0e", false}, {"0f", true}};
const std::vector<LineState> TO_532 = {{"07", true}, {"0b", true}, {"0b", false}, {"0f", false}, {"0e", true}};

namespace {
constexpr double LASER_SETTLE_532 = 3.6;      // s between laser on and the first frame (vendor: ~3.7 s)
constexpr double LASER_SETTLE_1064 = 1.5;

void sleepS(double s) {
    if (s > 0) QThread::usleep(static_cast<unsigned long>(s * 1e6));
}
}  // namespace

// ----------------------------------------------------------------------------- laser guard
// Python Cora._laser() context manager. Constructor: refuse unless armed and closed, MCP2200 pulse, laser on,
// settle. release() (normal exit) and the destructor (exception/stop) switch it off and pulse again.
class Cora::LaserGuard {
public:
    LaserGuard(Cora& c, double power_mw) : c_(c) {
        auto arm = c_.armStatus();
        const hw::ArmState st = arm->state();
        if (st.compartmentOpen) throw NotArmed("The sample compartment is open.");
        if (!st.armed) throw NotArmed("Not armed: press ARM on the instrument (needed after every opening).");
        arm->pulse();
        try {
            if (c_.channel_ == 532) {
                c_.io()->setLine(hw::LASER_LINE, false, /*allowLaser=*/true);
                c_.laserOn_ = true;
                c_.log("Laser ON (532 nm)");
                sleepS(LASER_SETTLE_532);
            } else {
                const ChannelCal& cal = c_.cal_.channels.at(1064);
                const double p = power_mw > 0 ? power_mw : c_.defaultPower1064_;
                if (!(0 < p && p <= cal.pwr_max_mw))
                    throw Error(("power must be 0-" + py::fixed(cal.pwr_max_mw, 0) + " mW").toStdString());
                auto d = c_.driver();
                if (!d) throw Error("1064 nm laser driver is not open");
                d->on();
                d->setCurrent(cal.pwr_coeff[0] + cal.pwr_coeff[1] * p);
                c_.laserOn_ = true;
                c_.log("Laser ON (1064 nm, " + py::fixed(p, 0) + " mW)");
                sleepS(LASER_SETTLE_1064);
            }
        } catch (...) {
            c_.laserOff();                    // may throw HardwareError, which then replaces the error (as Python)
            arm->pulse();
            throw;
        }
    }
    void release() {                          // normal exit: errors propagate
        done_ = true;
        c_.laserOff();
        c_.armStatus()->pulse();
    }
    ~LaserGuard() {
        if (done_) return;
        try {
            c_.laserOff();
            c_.armStatus()->pulse();
        } catch (const std::exception& e) {
            c_.log(QString("Laser off failed: %1").arg(e.what()));
        }
    }
    LaserGuard(const LaserGuard&) = delete;
    LaserGuard& operator=(const LaserGuard&) = delete;

private:
    Cora& c_;
    bool done_ = false;
};

// ----------------------------------------------------------------------------- construction
Cora::Cora(const Calibration& cal, LogFn log) : Instrument(cal, std::move(log)) {}

Cora::~Cora() {
    try {
        close(false);
    } catch (...) {
    }
}

std::unique_ptr<Instrument> makeCora(const Calibration& cal, LogFn log) {
    return std::make_unique<Cora>(cal, std::move(log));
}

std::shared_ptr<hw::IOBoard> Cora::io() const {
    std::lock_guard<std::mutex> l(devMutex_);
    if (!io_) throw Error("not connected (I/O board)");
    return io_;
}

std::shared_ptr<hw::ArmStatus> Cora::armStatus() const {
    std::lock_guard<std::mutex> l(devMutex_);
    if (!arm_) throw Error("not connected (MCP2200)");
    return arm_;
}

std::shared_ptr<hw::LaserDriver> Cora::driver() const {
    std::lock_guard<std::mutex> l(devMutex_);
    return driver_;
}

hw::Spectrometer& Cora::spec(int ch) {
    std::lock_guard<std::mutex> l(devMutex_);
    auto it = spec_.find(ch);
    if (it == spec_.end()) throw Error(QString("spectrometer for %1 nm not found").arg(ch).toStdString());
    return *it->second;
}

// ----------------------------------------------------------------------------- connection
void Cora::connect() {
    std::lock_guard<std::recursive_mutex> lk(lock_);
    struct Candidate {
        QString port;
        std::unique_ptr<hw::SerialLink> link;   // already open (Pl2303Usb)
    };
    std::vector<Candidate> cands;
    const QString found = hw::findTty(hw::PROLIFIC, true);
    for (const QString& p : found.isEmpty() ? hw::ttyCandidates(hw::PROLIFIC) : QStringList{found})
        cands.push_back({p, nullptr});
#if defined(Q_OS_WIN)
    {   // prefer the chip driven directly (WinUSB); the COM port may be refused
        std::vector<Candidate> usb;
        for (auto& d : hw::Pl2303Usb::findAll()) {
            const QString n = d->name();
            usb.push_back({n, std::move(d)});
        }
        for (auto& c : cands) usb.push_back(std::move(c));
        cands = std::move(usb);
    }
#endif
    std::shared_ptr<hw::IOBoard> board;
    bool serialName = false;
    QString denied;                           // a port we found but may not open (Linux: missing udev rule)
    for (auto& c : cands) {                   // several Prolific adapters (e.g. a Titrino cable): ask each politely
        std::unique_ptr<hw::IOBoard> b;
        try {
            const bool isUsb = c.link != nullptr;
            b = std::make_unique<hw::IOBoard>(isUsb ? std::move(c.link) : hw::openSerialPort(c.port, 115200));
            b->vol();                         // harmless query that only the Cora's I/O board answers
            board = std::move(b);
            serialName = !isUsb;
            break;
        } catch (const std::exception& e) {
            const QString what = QString::fromUtf8(e.what());
            if (what.contains(QLatin1String("ermission")) || what.contains(QLatin1String("ccess denied")))
                denied = c.port;
            if (b) try {
                    b->close();
                } catch (...) {
                }
        }
    }
    if (!board && !denied.isEmpty())
        throw Error(("The instrument's I/O board was found at " + denied + ", but this user may not open it. "
                     "Reinstall LibreRamen (the package installs a udev rule granting access), or install "
                     "70-libreramen.rules to /etc/udev/rules.d and replug the instrument.").toStdString());
    if (!board) {
        QString hint;
#if defined(Q_OS_WIN)
        hint = " On Windows the I/O board's old Prolific chip is refused by Prolific's current driver "
               "('PL2303HXA PHASED OUT') and needs the WinUSB driver instead. " + windrivers::HINT;
#endif
        throw Error(("Cora not found (I/O board). Is the USB cable connected and the instrument on "
                     "(On pressed, screen ready)?" + hint).toStdString());
    }
    {
        std::lock_guard<std::mutex> l(devMutex_);
        io_ = board;
        ioPort_ = serialName ? board->portName() : QString();   // (the watchdog needs a serial port name)
    }
    try {                                     // safety: whatever happened before, start with the 532 nm laser off
        board->setLine(hw::LASER_LINE, true, true);
    } catch (const std::exception&) {
    }
    auto t = hw::openMcp2200();
    if (!t) throw Error("MCP2200 (ARM status) not found");
    const QString hid = t->name();
    {
        std::lock_guard<std::mutex> l(devMutex_);
        arm_ = std::make_shared<hw::ArmStatus>(std::move(t));
    }
    for (int ch : {532, 1064}) {
        auto s = hw::Spectrometer::find(ch);
        if (s) {
            std::lock_guard<std::mutex> l(devMutex_);
            spec_[ch] = std::move(s);
        }
    }
    loadUserCalibration();
    std::vector<int> chans;
    {
        std::lock_guard<std::mutex> l(devMutex_);
        for (const auto& kv : spec_) chans.push_back(kv.first);
    }
    log(QString("Connected: I/O board %1, ARM status %2, spectrometers %3").arg(board->portName(), hid, py::str(chans)));
}

QString Cora::ioPort() const {
    std::lock_guard<std::mutex> l(devMutex_);
    return ioPort_;
}

QString Cora::driverPort() const {
    auto d = driver();
    return d ? d->portName() : QString();
}

QString Cora::serial() {
    std::lock_guard<std::recursive_mutex> lk(lock_);
    return spec(532).serial();
}

void Cora::keepalive() {
    std::shared_ptr<hw::IOBoard> b;
    {
        std::lock_guard<std::mutex> l(devMutex_);
        b = io_;
    }
    if (!b) return;
    std::unique_lock<std::recursive_mutex> lk(lock_, std::try_to_lock);
    if (!lk.owns_lock()) return;              // a measurement holds the instrument
    b->vol();
}

Status Cora::status() {
    const hw::ArmState st = armStatus()->state();
    Status s;
    s.armed = st.armed;
    s.compartment_open = st.compartmentOpen;
    s.gpio = st.gpio;
    s.channel = channel_;
    s.laser_on = laserOn_;
    std::lock_guard<std::mutex> l(devMutex_);
    for (const auto& kv : spec_) s.spectrometers.push_back(kv.first);
    return s;
}

// ----------------------------------------------------------------------------- start-up / channels
bool Cora::needsInitialize() const {
    std::lock_guard<std::mutex> l(devMutex_);
    return spec_.size() < 2;
}

void Cora::initialize(const ProgressFn& progress) {
    std::lock_guard<std::recursive_mutex> lk(lock_);
    auto b = io();
    b->command("TECE");
    const double steps = double(INIT_532.size() + 1);
    int k = 0;
    for (const LineState& ls : INIT_532) {
        b->setLine(ls.line, ls.high);
        if (QString(ls.line) == "0c" && !ls.high) waitSpec(532);
        if (QString(ls.line) == "0d" && !ls.high) waitSpec(1064);
        if (progress) progress(++k / steps);
    }
    spec(532).init();
    spec(1064).init();
    channel_ = 532;
    log("Initialized (532 nm path)");
    if (progress) progress(1.0);
}

void Cora::resume() {                         // instrument already initialised: assume the 532 nm path
    std::lock_guard<std::recursive_mutex> lk(lock_);
    channel_ = 532;
    spec(532).init();
}

void Cora::waitSpec(int ch, double timeout_s) {
    auto s = hw::Spectrometer::find(ch, timeout_s);
    if (!s) {
        QString hint;
#if defined(Q_OS_WIN)
        hint = " On Windows the spectrometers need the WinUSB driver (installed by LibreRamen-Setup; "
               "the vendor software then can't use them on this PC). " + windrivers::HINT;
#endif
        throw Error(QString("spectrometer for %1 nm did not appear on USB.%2").arg(ch).arg(hint).toStdString());
    }
    std::lock_guard<std::mutex> l(devMutex_);
    spec_[ch] = std::move(s);
}

void Cora::selectChannel(int ch, bool waitCooling) {
    std::lock_guard<std::recursive_mutex> lk(lock_);
    if (ch != 532 && ch != 1064) throw Error(QString("unknown channel %1 nm").arg(ch).toStdString());
    if (ch == channel_) return;
    laserOff();
    auto b = io();
    if (ch == 1064) {
        for (const LineState& ls : TO_1064_POWER) b->setLine(ls.line, ls.high);
        QString port;
        QElapsedTimer t;
        t.start();
        while (port.isEmpty() && t.elapsed() < 15000) {
            port = hw::findTty(hw::CP210X);
            QThread::msleep(300);
        }
        if (port.isEmpty()) throw Error("1064 nm laser driver did not appear");
        auto d = std::shared_ptr<hw::LaserDriver>(hw::LaserDriver::open(port));
        {
            std::lock_guard<std::mutex> l(devMutex_);
            driver_ = d;
        }
        log("1064 nm laser driver: " + d->init());
        for (const LineState& ls : TO_1064_PATH) b->setLine(ls.line, ls.high);
        spec(1064).init();
        channel_ = 1064;
        if (waitCooling) {
            const double w = cal_.channels.at(1064).tec_wait_s;
            log("Waiting " + py::fixed(w, 0) + " s for the 1064 nm detector to cool");
            sleepS(w);
        }
    } else {
        if (auto d = driver()) {
            d->off();
            d->close();
            std::lock_guard<std::mutex> l(devMutex_);
            driver_.reset();
        }
        for (const LineState& ls : TO_532) b->setLine(ls.line, ls.high);
        spec(532).init();
        channel_ = 532;
    }
    log(QString("Channel: %1 nm").arg(ch));
}

// ----------------------------------------------------------------------------- lasers
void Cora::laserOff() {
    QStringList errors;
    try {
        io()->setLine(hw::LASER_LINE, true, true);
    } catch (const std::exception& e) {
        errors << e.what();
    }
    if (auto d = driver()) {
        try {
            d->off();
        } catch (const std::exception& e) {
            errors << e.what();
        }
    }
    if (laserOn_) log("Laser OFF");
    laserOn_ = false;
    if (!errors.isEmpty()) throw HardwareError(("could not confirm laser off: " + errors.join("; ")).toStdString());
}

void Cora::laserOffNow() {
    try {
        laserOff();
    } catch (const std::exception&) {
    }
}

// ----------------------------------------------------------------------------- acquisition
Vec Cora::pixelNumbers(int ch) const { return hw::Spectrometer::pixelNumbers(ch); }

std::vector<Vec> Cora::frames(double exposure_s, int n, const std::atomic<bool>* stop) {
    hw::Spectrometer& sp = spec(channel_);
    if (sp.exposure() != exposure_s) sp.setExposure(exposure_s);
    std::vector<Vec> out;
    for (int i = 0; i < n; ++i) {
        if (stop && *stop) throw Stopped();
        out.push_back(sp.frame());
    }
    return out;
}

Vec Cora::dark(double exposure_s, int averages) {
    std::lock_guard<std::recursive_mutex> lk(lock_);
    return meanFrames(frames(exposure_s, averages, nullptr));
}

void Cora::bleach(double seconds, const std::atomic<bool>* stop) {
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < seconds * 1000) {
        if (stop && *stop) throw Stopped();   // Python: CoraError("stopped during pre-bleaching")
        sleepS(std::min(0.2, seconds - t.elapsed() / 1000.0));
    }
}

Spectrum Cora::acquire(const AcquireOptions& o) {
    std::lock_guard<std::recursive_mutex> lk(lock_);
    const int ch = channel_;
    if (ch == 0) throw Error("not initialized");
    Vec sig;
    {
        LaserGuard laser(*this, o.power_mw);
        if (o.prebleach_s > 0) {
            log("Pre-bleaching for " + py::g(o.prebleach_s) + " s");
            bleach(o.prebleach_s, o.stop);
        }
        sig = meanFrames(frames(o.exposure_s, o.averages, o.stop));
        laser.release();
    }
    const Vec bg = o.background ? meanFrames(frames(o.exposure_s, o.averages, o.stop)) : Vec(sig.size(), 0.0);
    Spectrum sp = makeSpectrum(ch, o.exposure_s, o.averages, pixelNumbers(ch), sig, bg, o.power_mw);
    if (o.prebleach_s > 0) sp.setMeta("prebleach_s", py::str(o.prebleach_s));
    return sp;
}

void Cora::acquireLaserOn(const AcquireOptions& o, const Vec& background, const std::function<bool()>& more,
                          const std::function<void(Spectrum&)>& each) {
    std::lock_guard<std::recursive_mutex> lk(lock_);
    const int ch = channel_;
    if (ch == 0) throw Error("not initialized");
    LaserGuard laser(*this, o.power_mw);
    try {
        while (!(o.stop && *o.stop) && (!more || more())) {
            const Vec sig = meanFrames(frames(o.exposure_s, o.averages, o.stop));
            Spectrum sp = makeSpectrum(ch, o.exposure_s, o.averages, pixelNumbers(ch), sig, background, o.power_mw);
            if (each) each(sp);
        }
    } catch (const Stopped&) {
    }
    laser.release();
}

// ----------------------------------------------------------------------------- shutdown
void Cora::close(bool powerDown) {
    std::lock_guard<std::recursive_mutex> lk(lock_);
    std::shared_ptr<hw::IOBoard> b;
    {
        std::lock_guard<std::mutex> l(devMutex_);
        b = io_;
    }
    std::exception_ptr err;
    try {
        if (b) laserOff();
    } catch (...) {
        err = std::current_exception();
    }
    try {
        if (powerDown && b) {
            // Exact vendor timing (80 ms per character, ~0.4 s between USB and OFF). With faster sending the
            // instrument's onboard computer shut down slowly ("killing" a program).
            b->setPacing(0.08);
            b->vol();
            sleepS(1.0);
            b->command("USB");
            sleepS(0.4);
            b->command("OFF");
        }
    } catch (...) {
        err = std::current_exception();
    }
    std::shared_ptr<hw::LaserDriver> d;
    std::shared_ptr<hw::ArmStatus> a;
    {
        std::lock_guard<std::mutex> l(devMutex_);
        d = std::move(driver_);
        a = std::move(arm_);
        io_.reset();
        ioPort_.clear();
        spec_.clear();                        // releases the USB handles
        driver_.reset();
        arm_.reset();
    }
    for (auto f : std::initializer_list<std::function<void()>>{[&] { if (d) d->close(); },
                                                               [&] { if (a) a->close(); },
                                                               [&] { if (b) b->close(); }}) {
        try {
            f();
        } catch (...) {
        }
    }
    if (err) std::rethrow_exception(err);
}

}  // namespace lr
