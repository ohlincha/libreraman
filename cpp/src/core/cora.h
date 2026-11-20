#pragma once
// The real Cora 5000 (Python: instrument.Cora): connection, start-up, channel selection, status,
// laser control and acquisition. PROTOCOL.md documents every command used here.
//
// Laser safety (identical to the Python version):
//  * a laser is switched on only by LaserGuard (inside acquire()/acquireLaserOn()), and only when the
//    instrument is armed and the compartment is closed (MCP2200 GPIO bits 0 and 7);
//  * LaserGuard switches it off on every exit path (normal, exception, Stop); close() switches both off;
//  * IOBoard refuses line 09 unless the caller is one of these paths (allowLaser).
//
// Thread safety: all public methods may be called from any thread. Measurements, start-up and channel
// changes hold a recursive lock (Python: self.lock = RLock); status() only uses the MCP2200 (own lock);
// keepalive() is skipped while the lock is busy; laserOffNow() bypasses the lock (I/O board has its own).
#include <QString>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

#include "hw/armstatus.h"
#include "hw/ioboard.h"
#include "hw/laserdriver.h"
#include "hw/spectrometer.h"
#include "instrument.h"

namespace lr {

// Line states copied from the vendor software (PROTOCOL.md).
struct LineState {
    const char* line;
    bool high;
};
extern const std::vector<LineState> INIT_532, TO_1064_POWER, TO_1064_PATH, TO_532;

class Cora : public Instrument {
public:
    Cora(const Calibration& cal, LogFn log);
    ~Cora() override;                         // close() (laser off), errors ignored

    void connect() override;
    void initialize(const ProgressFn& progress = {}) override;
    bool needsInitialize() const override;    // fewer than two spectrometers on USB
    void resume() override;
    Status status() override;
    void keepalive() override;                // VOL query, skipped while a measurement holds the instrument
    void selectChannel(int ch, bool waitCooling = true) override;
    Vec dark(double exposure_s, int averages = 1) override;
    Spectrum acquire(const AcquireOptions& o) override;
    void acquireLaserOn(const AcquireOptions& o, const Vec& background, const std::function<bool()>& more,
                        const std::function<void(Spectrum&)>& each) override;
    void laserOffNow() override;
    Vec pixelNumbers(int ch) const override;
    void close(bool powerDown = false) override;

    QString serial() override;
    QString ioPort() const override;
    QString driverPort() const override;

private:
    class LaserGuard;
    std::vector<Vec> frames(double exposure_s, int n, const std::atomic<bool>* stop);
    void laserOff();                          // Python _laser_off: throws HardwareError if not confirmed
    void bleach(double seconds, const std::atomic<bool>* stop);
    void waitSpec(int ch, double timeout_s = 10.0);
    hw::Spectrometer& spec(int ch);
    std::shared_ptr<hw::IOBoard> io() const;
    std::shared_ptr<hw::ArmStatus> armStatus() const;
    std::shared_ptr<hw::LaserDriver> driver() const;

    mutable std::recursive_mutex lock_;       // the instrument (Python Cora.lock)
    mutable std::mutex devMutex_;             // guards the pointers/map below for cross-thread readers
    std::shared_ptr<hw::IOBoard> io_;
    std::shared_ptr<hw::ArmStatus> arm_;
    std::shared_ptr<hw::LaserDriver> driver_;
    std::map<int, std::unique_ptr<hw::Spectrometer>> spec_;
    QString ioPort_;                          // serial port name of the I/O board (empty for Pl2303Usb)
    double defaultPower1064_ = 50.0;          // Python power_mw[1064]
};

}  // namespace lr
