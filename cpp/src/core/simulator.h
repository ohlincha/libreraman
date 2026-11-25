#pragma once
// Simulated Cora 5000 (Python: simulator.SimCora), built from spectra recorded on the real instrument
// (CCl4, CHCl3, ethanol, water, ZnS at 532 nm) and its measured dark current (resource :/core/simulator.bin).
// Same noise, cosmic-ray and ZnS-bleaching model as the Python version; all waits are divided by speed.
//
// Thread safety: as Instrument (measurements serialised by an internal lock; status() and the SimControls
// may be called from any thread, e.g. the GUI's demo buttons while a measurement runs).
#include <QString>
#include <QStringList>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <random>

#include "instrument.h"

namespace lr {

// Demo controls of the simulator: obtain with dynamic_cast<SimControls*>(instrument).
class SimControls {
public:
    virtual void pressArm() = 0;              // arms only with the compartment closed
    virtual void openCompartment() = 0;       // also disarms
    virtual void closeCompartment() = 0;
    // The accessory that lets the laser run with the lid open (focusing): attached, ARM works and stays on with
    // the compartment open. (How the real accessory behaves is not known yet; this is the assumption.)
    virtual void setLidAccessory(bool attached) = 0;
    virtual void setSample(const QString& name) = 0;   // one of sampleNames(); throws Error otherwise
    virtual QString sample() const = 0;
    virtual QStringList sampleNames() const = 0;
    virtual double speed() const = 0;
    virtual void setSpeed(double speed) = 0;

protected:
    ~SimControls() = default;
};

// Contents of simulator.bin.
struct SimData {
    Vec darkOffset, darkRate;                 // per pixel (data order = calibration pixel 0..2047)
    std::vector<std::pair<QString, Vec>> samples;   // counts per second
};
const SimData& simulatorData();               // loaded once from the Qt resource; throws Error
// Samples whose spectra change with time (in addition to the recorded ones in simulatorData()).
QStringList simulatorReactionNames();
// Demo sample that changes under the laser (issues #9, #10): ethanol whose bands move down (0.5 cm-1 per second
// of laser light, at most 20 cm-1) and fade (e^(-t/30 s)) while a broad background grows (1 % of the strongest
// band per second, at most 2x). t = laser time on this sample since it was (re)chosen.
QString simulatorDamageName();

class SimCora : public Instrument, public SimControls {
public:
    SimCora(const Calibration& cal, LogFn log, double speed = 1.0, std::optional<uint64_t> seed = std::nullopt);

    // Instrument
    void connect() override;
    void initialize(const ProgressFn& progress = {}) override;
    bool needsInitialize() const override { return !initialized_; }
    void resume() override;
    Status status() override;
    void keepalive() override {}
    void selectChannel(int ch, bool waitCooling = true) override;
    Vec dark(double exposure_s, int averages = 1) override;
    Spectrum acquire(const AcquireOptions& o) override;
    void acquireLaserOn(const AcquireOptions& o, const Vec& background, const std::function<bool()>& more,
                        const std::function<void(Spectrum&)>& each) override;
    void laserOffNow() override { laserOff(); }
    Vec pixelNumbers(int ch) const override;
    void close(bool powerDown = false) override;
    QString serial() override { return QStringLiteral("SIMULATOR"); }
    bool simulated() const override { return true; }
    double laserSettleS() const override { return 1.0 / speed_; }

    // SimControls
    void pressArm() override;
    void openCompartment() override;
    void setLidAccessory(bool attached) override { accessory_ = attached; }
    void closeCompartment() override;
    void setSample(const QString& name) override;
    QString sample() const override;
    QStringList sampleNames() const override;
    double speed() const override { return speed_; }
    void setSpeed(double s) override;

private:
    class LaserGuard;
    std::vector<Vec> frames(double exposure_s, int n, const std::atomic<bool>* stop);
    void laserOff();
    void bleach(double seconds, const std::atomic<bool>* stop);
    void sleepScaled(double s, const std::atomic<bool>* stop = nullptr) const;

    const SimData& data_;
    std::atomic<double> speed_;
    std::atomic<bool> armed_{false}, open_{false}, initialized_{false}, accessory_{false};
    mutable std::mutex m_;                    // sample_, laserTime_, rng_
    QString sample_ = QStringLiteral("ethanol");
    std::map<QString, double> laserTime_;     // sample -> accumulated laser exposure (s), for bleaching
    // Reaction samples: concentrations evolve in simulated time (wall time × speed) since the sample was
    // chosen; their spectra are mixtures of the recorded ones.
    double reactionWall0_ = 0.0;              // monotonic seconds when the reaction sample was chosen
    double reactionSimT_ = 0.0;               // simulated seconds accumulated before the last speed change
    Vec sampleSpectrum(const QString& name) const;   // call with m_ held
    Vec damagedSpectrum(double laserTime) const;     // simulatorDamageName() after this laser time (s)
    std::mt19937_64 rng_;
    std::recursive_mutex lock_;               // measurements
};

}  // namespace lr
