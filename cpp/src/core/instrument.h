#pragma once
// The instrument as seen by the GUI, CLI and series runner (Python: instrument.Cora; simulator.SimCora
// implements the same interface). All methods may block (USB/serial I/O) and are called from a worker
// thread; status() is also called from the GUI's poll timer, so implementations lock internally.
//
// Laser safety (see ../../../libreramen/instrument.py and PROTOCOL.md):
//  * acquire() refuses to switch a laser on when the instrument isn't armed or the compartment is open;
//  * a laser is on only inside acquire() (RAII guard), and is switched off on every exit path;
//  * close() switches both lasers off.
#include <QString>
#include <atomic>
#include <functional>
#include <map>
#include <memory>
#include <vector>

#include "calibration.h"
#include "spectrum.h"

namespace lr {

using LogFn = std::function<void(const QString&)>;
using ProgressFn = std::function<void(double)>;        // 0..1

struct Status {
    bool armed = false;
    bool compartment_open = false;
    int gpio = 0;
    int channel = 0;              // 0 = not initialised
    bool laser_on = false;
    std::vector<int> spectrometers;
};

struct Poly {                     // calibrate.py polyfit: y = sum coeff[k] * ((x - x0) / scale)^k
    std::vector<double> coeff;
    double x0 = 0.0, scale = 1.0;
    double operator()(double x) const;
};

struct UserCalibration {          // fitted Raman-shift polynomial (calibrate.py), per channel
    Poly poly;                    // shift(calibration pixel)
    QString date;
    std::vector<QString> standards;
    double rms = 0.0;
};

struct AcquireOptions {
    double exposure_s = 1.0;
    int averages = 1;
    bool background = true;
    double power_mw = 0.0;                    // 1064 nm only; 0 = default
    double prebleach_s = 0.0;
    const std::atomic<bool>* stop = nullptr;  // set -> throw Stopped (checked between frames / while bleaching)
};

class Instrument {
public:
    virtual ~Instrument() = default;

    virtual void connect() = 0;                               // throws Error with a user-readable hint
    virtual void initialize(const ProgressFn& progress = {}) = 0;
    virtual bool needsInitialize() const = 0;                 // false if the instrument was already powered up
    virtual void resume() = 0;                                // already initialised: assume the 532 nm path
    virtual Status status() = 0;
    virtual void keepalive() = 0;
    virtual void selectChannel(int ch, bool waitCooling = true) = 0;
    virtual Vec dark(double exposure_s, int averages = 1) = 0;
    virtual Spectrum acquire(const AcquireOptions& o) = 0;
    // "Laser stays on" series (GUI controller _series_fastest): laser on once (same checks as acquire()),
    // then signal-only spectra with this background (data order, e.g. from dark()) until more() returns
    // false or o.stop is set; each spectrum is passed to each(). Returns normally when stopped.
    virtual void acquireLaserOn(const AcquireOptions& o, const Vec& background, const std::function<bool()>& more,
                                const std::function<void(Spectrum&)>& each) = 0;
    // Stop button (GUI controller stop()): laser(s) off right away, from any thread, without waiting for the
    // running measurement. Errors are ignored.
    virtual void laserOffNow() = 0;
    // Calibration pixel numbers of channel ch in data order (the order of dark()/frames).
    virtual Vec pixelNumbers(int ch) const = 0;
    virtual void close(bool powerDown = false) = 0;

    virtual QString serial() = 0;                             // spectrometer serial (EEPROM)
    virtual QString ioPort() const { return {}; }             // serial port name for the watchdog (if any)
    virtual QString driverPort() const { return {}; }         // 1064 nm driver port while open (watchdog "driver")
    virtual bool simulated() const { return false; }

    int channel() const { return channel_; }
    bool laserOn() const { return laserOn_; }
    const Calibration& calibration() const { return cal_; }
    const std::map<int, UserCalibration>& userCalibration() const { return userCal_; }
    void setUserCalibration(int ch, const UserCalibration& c) { userCal_[ch] = c; }
    void clearUserCalibration(int ch) { userCal_.erase(ch); }  // back to the factory calibration
    // (set/clear: not while a measurement runs - makeSpectrum() reads the user calibration)
    virtual double laserSettleS() const { return channel_ == 1064 ? 1.5 : 3.6; }
    // Minimum time for one spectrum with these settings (s).
    double cycleTime(double exposure_s, int averages, bool background = true, bool laserStaysOn = false) const {
        const int frames = averages * (background ? 2 : 1);
        return (laserStaysOn ? 0.0 : laserSettleS()) + frames * (exposure_s + 0.02) + 0.5;
    }
    // Raman shift of calibration pixel p on channel ch (user calibration if present, else factory).
    double shiftOf(int ch, double p) const;
    // Calibration description for file metadata: "factory (built-in)" or "user 2026-11-20 (CCl4, ...)".
    QString calibrationLabel(int ch) const;

protected:
    explicit Instrument(Calibration cal, LogFn log) : cal_(std::move(cal)), log_(std::move(log)) {}
    void log(const QString& m) const { if (log_) log_(m); }
    void loadUserCalibration();                               // calib::load(ch, serial) for 532 and 1064
    // On connect, with the serial read from the spectrometer: use the stored factory calibration of this
    // instrument (factory_<serial>.ini); without one, keep the built-in values but file user calibrations under
    // this serial, and say so. Then loads the user calibrations.
    void selectFactoryCalibration(const QString& serial);
    // Builds the Spectrum (sorted by shift) from averaged signal/background frames in data order.
    Spectrum makeSpectrum(int ch, double exposure_s, int averages, const Vec& pixels, const Vec& sig,
                          const Vec& bg, double power_mw) const;

    Calibration cal_;
    LogFn log_;
    std::atomic<int> channel_{0};                             // read by status() from other threads
    std::atomic<bool> laserOn_{false};
    std::map<int, UserCalibration> userCal_;
};

std::unique_ptr<Instrument> makeCora(const Calibration& cal, LogFn log);       // real hardware (cora.cpp)
// Simulator; extra controls via the SimControls interface (simulator.h).
std::unique_ptr<Instrument> makeSimulator(const Calibration& cal, LogFn log, double speed = 1.0);

Vec meanFrames(const std::vector<Vec>& frames);

}  // namespace lr
