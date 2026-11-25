#include "simulator.h"

#include <QElapsedTimer>
#include <QFile>
#include <QThread>
#include <QtEndian>
#include <algorithm>
#include <cmath>
#include <cstring>

#include "errors.h"
#include "pyformat.h"

// The core library is static: its resources must be registered explicitly (outside any namespace).
static void initCoreResources() { Q_INIT_RESOURCE(core); }

namespace lr {

// ----------------------------------------------------------------------------- data
const SimData& simulatorData() {
    static const SimData data = [] {
        initCoreResources();
        QFile f(":/core/simulator.bin");
        if (!f.open(QIODevice::ReadOnly)) throw Error("simulator data not found (:/core/simulator.bin)");
        const QByteArray b = f.readAll();
        size_t pos = 0;
        auto need = [&](size_t n) {
            if (pos + n > size_t(b.size())) throw Error("simulator data truncated");
        };
        auto u32 = [&] {
            need(4);
            const quint32 v = qFromLittleEndian<quint32>(b.constData() + pos);
            pos += 4;
            return v;
        };
        auto u16 = [&] {
            need(2);
            const quint16 v = qFromLittleEndian<quint16>(b.constData() + pos);
            pos += 2;
            return v;
        };
        auto f32 = [&](quint32 n) {
            need(size_t(n) * 4);
            Vec v(n);
            for (quint32 i = 0; i < n; ++i) {
                const quint32 raw = qFromLittleEndian<quint32>(b.constData() + pos + 4 * i);
                float x;
                std::memcpy(&x, &raw, 4);
                v[i] = x;
            }
            pos += size_t(n) * 4;
            return v;
        };
        need(8);
        if (std::memcmp(b.constData(), "LRSIM1\0\0", 8) != 0) throw Error("simulator data: bad magic");
        pos = 8;
        const quint32 nSamples = u32(), nPx = u32();
        SimData d;
        d.darkOffset = f32(nPx);
        d.darkRate = f32(nPx);
        for (quint32 k = 0; k < nSamples; ++k) {
            const quint16 len = u16();
            need(len);
            const QString name = QString::fromUtf8(b.constData() + pos, len);
            pos += len;
            d.samples.emplace_back(name, f32(nPx));
        }
        return d;
    }();
    return data;
}

// ----------------------------------------------------------------------------- SimCora
SimCora::SimCora(const Calibration& cal, LogFn log, double speed, std::optional<uint64_t> seed)
    : Instrument(cal, std::move(log)), data_(simulatorData()), speed_(speed > 0 ? speed : 1.0),
      rng_(seed ? *seed : std::random_device{}()) {}

std::unique_ptr<Instrument> makeSimulator(const Calibration& cal, LogFn log, double speed) {
    return std::make_unique<SimCora>(cal, std::move(log), speed);
}

void SimCora::sleepScaled(double s, const std::atomic<bool>* stop) const {
    const double total = s / speed_;
    if (total <= 0) return;
    QElapsedTimer t;
    t.start();
    for (;;) {
        if (stop && *stop) throw Stopped();
        const double left = total - t.nsecsElapsed() / 1e9;
        if (left <= 0) return;
        QThread::usleep(static_cast<unsigned long>(std::min(left, 0.05) * 1e6));
    }
}

void SimCora::pressArm() {
    if (!open_ || accessory_) armed_ = true;
}

void SimCora::openCompartment() {
    open_ = true;
    if (!accessory_) armed_ = false;
}

void SimCora::closeCompartment() { open_ = false; }

namespace {

const QString kEvaporation = QStringLiteral("reaction: ethanol evaporating from water");
const QString kConsecutive = QStringLiteral("reaction: A → B → C (ethanol → chloroform → CCl4, demo)");

}  // namespace

QStringList simulatorReactionNames() { return {kEvaporation, kConsecutive}; }

namespace {
const QString kDamage = QStringLiteral("demo: sample changing under the laser (ethanol)");
}  // namespace

QString simulatorDamageName() { return kDamage; }

namespace {

double monoNow() {
    static QElapsedTimer t = [] {
        QElapsedTimer e;
        e.start();
        return e;
    }();
    return t.nsecsElapsed() / 1e9;
}

}  // namespace

void SimCora::setSpeed(double s) {
    std::lock_guard<std::mutex> l(m_);
    const double now = monoNow();                           // keep the reaction clock continuous
    reactionSimT_ += (now - reactionWall0_) * speed_;
    reactionWall0_ = now;
    speed_ = s > 0 ? s : 1.0;
}

void SimCora::setSample(const QString& name) {
    bool known = name == kEvaporation || name == kConsecutive || name == kDamage;
    for (const auto& s : data_.samples)
        if (s.first == name) known = true;
    if (!known) throw Error(("unknown sample: " + name).toStdString());
    std::lock_guard<std::mutex> l(m_);
    if (name == kDamage) laserTime_[name] = 0.0;           // (re)choosing it: a fresh sample
    if (name != sample_ || name.startsWith(QStringLiteral("reaction:"))) {   // (re)choosing a reaction restarts it
        reactionWall0_ = monoNow();
        reactionSimT_ = 0.0;
    }
    sample_ = name;
}

// Spectrum (counts/s) of a sample. Reactions mix recorded spectra:
//  * ethanol evaporating from water: ethanol volume fraction x(t) = 0.4·exp(−t/τ), τ = 15 min (rest water);
//  * A → B → C, k1 = 1/(4 min), k2 = 1/(12 min) (consecutive first order, [A]0 = 1):
//      A = e^(−k1 t), B = k1/(k2−k1)·(e^(−k1 t) − e^(−k2 t)), C = 1 − A − B.
Vec SimCora::sampleSpectrum(const QString& name) const {
    auto rec = [&](const QString& n) -> const Vec* {
        for (const auto& kv : data_.samples)
            if (kv.first == n) return &kv.second;
        return nullptr;
    };
    if (const Vec* s = rec(name)) return *s;
    const double t = reactionSimT_ + (monoNow() - reactionWall0_) * speed_;
    std::vector<std::pair<const Vec*, double>> mix;
    if (name == kEvaporation) {
        const double x = 0.4 * std::exp(-t / 900.0);
        mix = {{rec("ethanol"), x}, {rec("water"), 1.0 - x}};
    } else if (name == kConsecutive) {
        const double k1 = 1.0 / 240.0, k2 = 1.0 / 720.0;
        const double a = std::exp(-k1 * t), b = k1 / (k2 - k1) * (std::exp(-k1 * t) - std::exp(-k2 * t));
        mix = {{rec("ethanol"), a}, {rec("chloroform"), b}, {rec("carbon tetrachloride"), std::max(0.0, 1.0 - a - b)}};
    }
    Vec out(data_.darkOffset.size(), 0.0);
    for (const auto& [v, w] : mix)
        if (v)
            for (size_t i = 0; i < out.size() && i < v->size(); ++i) out[i] += w * (*v)[i];
    return out;
}

Vec SimCora::damagedSpectrum(double lt) const {
    const Vec* base = nullptr;
    for (const auto& kv : data_.samples)
        if (kv.first == QLatin1String("ethanol")) base = &kv.second;
    Vec out(data_.darkOffset.size(), 0.0);
    if (!base || base->size() < 2) return out;
    const Vec& s = *base;
    const int n = int(std::min(out.size(), s.size()));
    // bands move to lower Raman shift: shift of 0.5 cm-1/s, converted to pixels with the local dispersion
    const double s0 = shiftOf(532, 0), s1 = shiftOf(532, n - 1);
    const double disp = std::abs(s1 - s0) / double(n - 1);
    const double dir = s1 > s0 ? 1.0 : -1.0;
    const double dpx = dir * std::min(20.0, 0.5 * lt) / (disp > 0 ? disp : 2.4);
    const double fade = std::exp(-lt / 30.0);
    const double smax = *std::max_element(s.begin(), s.begin() + n);
    const double bg = std::min(2.0, 0.01 * lt) * smax;
    for (int i = 0; i < n; ++i) {
        const double p = i + dpx;                         // content now at i came from pixel p
        const int k = int(std::floor(p));
        double v = 0.0;
        if (k >= 0 && k + 1 < n) v = s[k] + (p - k) * (s[k + 1] - s[k]);
        const double frac = dir > 0 ? double(i) / (n - 1) : 1.0 - double(i) / (n - 1);   // toward high shift
        out[i] = v * fade + bg * (0.5 + 0.5 * frac);
    }
    return out;
}

QString SimCora::sample() const {
    std::lock_guard<std::mutex> l(m_);
    return sample_;
}

QStringList SimCora::sampleNames() const {
    QStringList l;
    for (const auto& s : data_.samples) l << s.first;
    l << kDamage << simulatorReactionNames();
    return l;
}

void SimCora::connect() {
    sleepScaled(0.3);
    log("Connected to the simulator (spectra recorded on a real Cora 5000)");
}

Status SimCora::status() {
    Status s;
    s.armed = armed_;
    s.compartment_open = open_;
    s.gpio = 0;                               // (Python: None)
    s.channel = channel_;
    s.laser_on = laserOn_;
    if (initialized_) s.spectrometers = {532, 1064};
    return s;
}

void SimCora::initialize(const ProgressFn& progress) {
    std::lock_guard<std::recursive_mutex> lk(lock_);
    const int steps = 10;
    for (int k = 0; k < steps; ++k) {
        sleepScaled(0.6);
        if (progress) progress(double(k + 1) / steps);
    }
    initialized_ = true;
    channel_ = 532;
    log("Initialized (simulator, 532 nm path)");
}

void SimCora::resume() {
    std::lock_guard<std::recursive_mutex> lk(lock_);
    initialized_ = true;
    channel_ = 532;
}

void SimCora::selectChannel(int ch, bool /*waitCooling*/) {
    if (ch == 1064)
        throw Error("The simulator has no 1064 nm data (the development unit's 1064 nm channel is faulty).");
    std::lock_guard<std::recursive_mutex> lk(lock_);
    channel_ = ch;
}

Vec SimCora::pixelNumbers(int ch) const {
    Vec p(ch == 1064 ? 256 : 2048);
    for (size_t i = 0; i < p.size(); ++i) p[i] = double(i);
    return p;
}

void SimCora::laserOff() {
    if (laserOn_) log("Laser OFF");
    laserOn_ = false;
}

// Python SimCora._laser(): no MCP2200 pulse, 1 s (scaled) settle.
class SimCora::LaserGuard {
public:
    explicit LaserGuard(SimCora& c, bool allowOpen = false) : c_(c) {
        if (c_.open_ && !allowOpen) throw NotArmed("The sample compartment is open.");
        if (!c_.armed_) throw NotArmed("Not armed: press ARM on the instrument (needed after every opening).");
        c_.laserOn_ = true;
        c_.log("Laser ON (532 nm, simulated)");
        try {
            c_.sleepScaled(1.0);
        } catch (...) {
            c_.laserOff();
            throw;
        }
    }
    ~LaserGuard() { c_.laserOff(); }
    LaserGuard(const LaserGuard&) = delete;
    LaserGuard& operator=(const LaserGuard&) = delete;

private:
    SimCora& c_;
};

std::vector<Vec> SimCora::frames(double exposure_s, int n, const std::atomic<bool>* stop) {
    std::vector<Vec> out;
    std::normal_distribution<double> normal(0.0, 1.0);
    std::uniform_real_distribution<double> uni(0.0, 1.0), ray(2000.0, 15000.0);
    for (int f = 0; f < n; ++f) {
        if (stop && *stop) throw Stopped();
        QElapsedTimer t0;
        t0.start();
        Vec y(data_.darkOffset.size());
        {
            std::lock_guard<std::mutex> l(m_);
            for (size_t i = 0; i < y.size(); ++i) y[i] = data_.darkOffset[i] + data_.darkRate[i] * exposure_s;
            if (laserOn_) {
                const double lt = laserTime_.count(sample_) ? laserTime_[sample_] : 0.0;
                const Vec s = sample_ == kDamage ? damagedSpectrum(lt) : sampleSpectrum(sample_);
                const double bleach = sample_ == "zinc sulfide" ? 0.6 + 0.4 * std::exp(-lt / 180.0) : 1.0;
                for (size_t i = 0; i < y.size() && i < s.size(); ++i) y[i] += s[i] * exposure_s * bleach;
                laserTime_[sample_] = lt + exposure_s;
            }
            for (double& v : y) v += normal(rng_) * std::sqrt(std::max(v, 1.0) + 64.0);   // shot + read noise
            if (uni(rng_) < 0.08 * exposure_s && y.size() > 11) {                       // occasional cosmic ray
                std::uniform_int_distribution<size_t> at(5, y.size() - 6);
                const size_t i = at(rng_);
                const double a = ray(rng_);
                y[i] += a;
                y[i + 1] += a;
            }
        }
        for (double& v : y) v = std::clamp(v, 0.0, 65535.0);
        out.push_back(std::move(y));
        const double dt = exposure_s / speed_ - t0.nsecsElapsed() / 1e9;
        if (dt > 0) QThread::usleep(static_cast<unsigned long>(dt * 1e6));
    }
    return out;
}

void SimCora::bleach(double seconds, const std::atomic<bool>* stop) {
    {
        std::lock_guard<std::mutex> l(m_);
        laserTime_[sample_] += seconds;
    }
    sleepScaled(seconds, stop);
}

Vec SimCora::dark(double exposure_s, int averages) {
    std::lock_guard<std::recursive_mutex> lk(lock_);
    return meanFrames(frames(exposure_s, averages, nullptr));
}

Spectrum SimCora::acquire(const AcquireOptions& o) {
    std::lock_guard<std::recursive_mutex> lk(lock_);
    const int ch = channel_;
    if (ch == 0) throw Error("not initialized");
    Vec sig;
    {
        LaserGuard laser(*this);
        if (o.prebleach_s > 0) {
            log("Pre-bleaching for " + py::g(o.prebleach_s) + " s");
            bleach(o.prebleach_s, o.stop);
        }
        std::vector<Vec> f = frames(o.exposure_s, o.averages, o.stop);
        sig = meanFrames(f);
        if (o.frames) *o.frames = std::move(f);
    }
    const Vec bg = o.background ? meanFrames(frames(o.exposure_s, o.averages, o.stop)) : Vec(sig.size(), 0.0);
    Spectrum sp = makeSpectrum(ch, o.exposure_s, o.averages, pixelNumbers(ch), sig, bg, o.power_mw);
    if (o.prebleach_s > 0) sp.setMeta("prebleach_s", py::str(o.prebleach_s));
    return sp;
}

void SimCora::acquireLaserOn(const AcquireOptions& o, const Vec& background, const std::function<bool()>& more,
                             const std::function<void(Spectrum&)>& each) {
    std::lock_guard<std::recursive_mutex> lk(lock_);
    const int ch = channel_;
    if (ch == 0) throw Error("not initialized");
    LaserGuard laser(*this, o.allowOpenCompartment);
    try {
        while (!(o.stop && *o.stop) && (!more || more())) {
            const Vec sig = meanFrames(frames(o.exposure_s, o.averages, o.stop));
            Spectrum sp = makeSpectrum(ch, o.exposure_s, o.averages, pixelNumbers(ch), sig, background, o.power_mw);
            if (each) each(sp);
        }
    } catch (const Stopped&) {
    }
}

void SimCora::close(bool /*powerDown*/) { laserOff(); }

}  // namespace lr
