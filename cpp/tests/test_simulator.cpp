// Simulated instrument (simulator.py behaviour), run fast (speed factor).
#include <QThread>
#include <QtTest>
#include <atomic>
#include <cmath>
#include <numeric>
#include <thread>

#include "core/errors.h"
#include "core/simulator.h"

using namespace lr;

namespace {
double meanOf(const Vec& v) { return std::accumulate(v.begin(), v.end(), 0.0) / double(v.size()); }

std::unique_ptr<SimCora> ready(QStringList* logs = nullptr, double speed = 500.0, uint64_t seed = 1) {
    auto s = std::make_unique<SimCora>(defaultCalibration(), [logs](const QString& m) {
        if (logs) *logs << m;
    }, speed, seed);
    s->connect();
    s->initialize();
    return s;
}
}  // namespace

class TestSimulator : public QObject {
    Q_OBJECT
private slots:
    void data() {
        const SimData& d = simulatorData();
        QCOMPARE(int(d.darkOffset.size()), 2048);
        QCOMPARE(int(d.darkRate.size()), 2048);
        QCOMPARE(int(d.samples.size()), 5);
        QStringList names;
        for (const auto& s : d.samples) {
            names << s.first;
            QCOMPARE(int(s.second.size()), 2048);
        }
        QCOMPARE(names, QStringList({"carbon tetrachloride", "chloroform", "ethanol", "water", "zinc sulfide"}));
    }

    void startUp() {
        QStringList logs;
        SimCora s(defaultCalibration(), [&](const QString& m) { logs << m; }, 500.0, 1);
        QVERIFY(s.simulated());
        QCOMPARE(s.serial(), QString("SIMULATOR"));
        QVERIFY(s.ioPort().isEmpty());
        s.connect();
        QVERIFY(s.needsInitialize());
        QVERIFY(s.status().spectrometers.empty());
        std::vector<double> prog;
        s.initialize([&](double p) { prog.push_back(p); });
        QCOMPARE(int(prog.size()), 10);
        QCOMPARE(prog.back(), 1.0);
        QVERIFY(!s.needsInitialize());
        const Status st = s.status();
        QCOMPARE(st.channel, 532);
        QCOMPARE(st.spectrometers, std::vector<int>({532, 1064}));
        QVERIFY(!st.laser_on);
        QCOMPARE(logs, QStringList({"Connected to the simulator (spectra recorded on a real Cora 5000)",
                                    "Initialized (simulator, 532 nm path)"}));
        QCOMPARE(s.laserSettleS(), 1.0 / 500.0);
        bool threw = false;
        try {
            s.selectChannel(1064);
        } catch (const Error& e) {
            threw = QString(e.what()).startsWith("The simulator has no 1064 nm data");
        }
        QVERIFY(threw);
        QCOMPARE(s.channel(), 532);
    }

    void armingRules() {
        auto s = ready();
        AcquireOptions o;
        o.exposure_s = 0.1;
        try {
            s->acquire(o);
            QFAIL("acquired without ARM");
        } catch (const NotArmed& e) {
            QCOMPARE(QString(e.what()), QString("Not armed: press ARM on the instrument (needed after every opening)."));
        }
        s->openCompartment();
        s->pressArm();                                            // no effect while open
        QVERIFY(!s->status().armed);
        QVERIFY(s->status().compartment_open);
        try {
            s->acquire(o);
            QFAIL("acquired with the compartment open");
        } catch (const NotArmed& e) {
            QCOMPARE(QString(e.what()), QString("The sample compartment is open."));
        }
        s->closeCompartment();
        s->pressArm();
        QVERIFY(s->status().armed);
        s->acquire(o);
        s->openCompartment();                                     // opening disarms
        QVERIFY(!s->status().armed);
        QVERIFY(!s->laserOn());
    }

    void spectrum() {
        QStringList logs;
        auto s = ready(&logs);
        s->pressArm();
        QCOMPARE(s->sample(), QString("ethanol"));
        AcquireOptions o;
        o.exposure_s = 1.0;
        o.averages = 4;
        const Spectrum sp = s->acquire(o);
        QVERIFY(!s->laserOn());
        QCOMPARE(int(sp.shift.size()), 2048);
        for (size_t i = 1; i < sp.shift.size(); ++i) QVERIFY(sp.shift[i] > sp.shift[i - 1]);
        QCOMPARE(sp.averages, 4);
        QCOMPARE(sp.metaValue("calibration"), QString("factory (built-in)"));
        // strongest ethanol band: C-H stretch near 2930 cm-1
        const size_t imax = size_t(std::max_element(sp.intensity.begin(), sp.intensity.end()) - sp.intensity.begin());
        QVERIFY2(sp.shift[imax] > 2900 && sp.shift[imax] < 2960, qPrintable(QString::number(sp.shift[imax])));
        // intensity ~ recorded sample (counts/s) x exposure
        const Vec& ref = simulatorData().samples[2].second;      // ethanol
        double a = 0, b = 0;
        for (size_t i = 0; i < sp.pixels.size(); ++i) {
            a += sp.intensity[i];
            b += ref[size_t(sp.pixels[i])];
        }
        QVERIFY2(std::abs(a / b - 1.0) < 0.05, qPrintable(QString::number(a / b)));
        for (size_t i = 0; i < sp.intensity.size(); ++i) QCOMPARE(sp.intensity[i], sp.signal[i] - sp.background[i]);
        QVERIFY(logs.contains("Laser ON (532 nm, simulated)"));
        QCOMPARE(logs.last(), QString("Laser OFF"));
    }

    void noBackgroundAndPrebleach() {
        QStringList logs;
        auto s = ready(&logs);
        s->pressArm();
        AcquireOptions o;
        o.exposure_s = 0.2;
        o.background = false;
        o.prebleach_s = 2.0;
        const Spectrum sp = s->acquire(o);
        for (size_t i = 0; i < sp.intensity.size(); ++i) {
            QCOMPARE(sp.background[i], 0.0);
            QCOMPARE(sp.intensity[i], sp.signal[i]);
        }
        QCOMPARE(sp.metaValue("prebleach_s"), QString("2.0"));
        QVERIFY(logs.contains("Pre-bleaching for 2 s"));
    }

    void zincSulfideBleaches() {
        auto s = ready();
        s->pressArm();
        s->setSample("zinc sulfide");
        AcquireOptions o;
        o.exposure_s = 1.0;
        o.averages = 2;
        const double fresh = meanOf(s->acquire(o).intensity);
        o.prebleach_s = 400;                                      // 0.6 + 0.4 exp(-402/180) ~ 0.64
        const double bleached = meanOf(s->acquire(o).intensity);
        QVERIFY2(bleached / fresh < 0.75, qPrintable(QString::number(bleached / fresh)));
        bool threw = false;
        try {
            s->setSample("unobtainium");
        } catch (const Error&) {
            threw = true;
        }
        QVERIFY(threw);
    }

    void darkAndSeed() {
        auto a = ready(nullptr, 500.0, 42), b = ready(nullptr, 500.0, 42);
        const Vec da = a->dark(0.5, 2), db = b->dark(0.5, 2);
        QCOMPARE(int(da.size()), 2048);
        QCOMPARE(da, db);                                         // same seed, same noise
        QVERIFY(!a->laserOn());
        const double m = meanOf(da);                              // offset 692.8 + rate 373.7 /s (data means; a cosmic ray adds a few counts)
        QVERIFY2(std::abs(m - (692.785 + 0.5 * 373.683)) < 10, qPrintable(QString::number(m)));
    }

    void stopFlag() {
        auto s = ready(nullptr, 20.0);
        s->pressArm();
        std::atomic<bool> stop{false};
        AcquireOptions o;
        o.exposure_s = 1.0;
        o.averages = 20;
        o.stop = &stop;
        std::thread t([&] {
            QThread::msleep(150);
            stop = true;
        });
        bool stopped = false;
        try {
            s->acquire(o);
        } catch (const Stopped&) {
            stopped = true;
        }
        t.join();
        QVERIFY(stopped);
        QVERIFY(!s->laserOn());
    }

    void laserOnSeries() {
        QStringList logs;
        auto s = ready(&logs);
        s->pressArm();
        const Vec bg = s->dark(0.2, 1);
        AcquireOptions o;
        o.exposure_s = 0.2;
        int n = 0;
        bool onDuring = true;
        s->acquireLaserOn(o, bg, [&] { return n < 3; }, [&](Spectrum& sp) {
            ++n;
            onDuring = onDuring && s->laserOn();
            QCOMPARE(sp.background.size(), size_t(2048));
        });
        QCOMPARE(n, 3);
        QVERIFY(onDuring);
        QVERIFY(!s->laserOn());
        QCOMPARE(logs.count("Laser ON (532 nm, simulated)"), 1);
    }

    void laserOffNowFromAnotherThread() {
        auto s = ready(nullptr, 5.0);
        s->pressArm();
        std::atomic<bool> stop{false};
        AcquireOptions o;
        o.exposure_s = 1.0;
        o.stop = &stop;
        std::thread t([&] {
            s->acquireLaserOn(o, Vec(2048, 0.0), {}, {});
        });
        QTRY_VERIFY_WITH_TIMEOUT(s->laserOn(), 3000);
        stop = true;
        s->laserOffNow();                                         // Stop button
        QVERIFY(!s->laserOn());
        t.join();
        QVERIFY(!s->laserOn());
    }

    void controls() {
        auto s = ready();
        SimControls* c = dynamic_cast<SimControls*>(static_cast<Instrument*>(s.get()));
        QVERIFY(c);
        QCOMPARE(c->sampleNames().size(), 8);                 // 5 recorded + damage demo + 2 reactions
        c->setSpeed(3.0);
        QCOMPARE(c->speed(), 3.0);
        auto inst = makeSimulator(defaultCalibration(), {}, 2.0);
        QVERIFY(inst->simulated());
        QCOMPARE(dynamic_cast<SimControls*>(inst.get())->speed(), 2.0);
    }

    // A -> B -> C (ethanol -> chloroform -> CCl4): ethanol's 882 band at the start, CCl4's 459 band later.
    void reaction() {
        auto s = ready(nullptr, 1000.0);                      // 1 s wall = 1000 s simulated
        SimControls* c = dynamic_cast<SimControls*>(static_cast<Instrument*>(s.get()));
        const QString name = c->sampleNames().last();
        QVERIFY(name.startsWith("reaction: A"));
        c->closeCompartment();
        c->pressArm();
        c->setSample(name);
        AcquireOptions o;
        o.exposure_s = 0.5;
        auto band = [](const Spectrum& sp, double at) {
            double m = 0;
            for (size_t i = 0; i < sp.shift.size(); ++i)
                if (std::abs(sp.shift[i] - at) < 8) m = std::max(m, sp.intensity[i]);
            return m;
        };
        const Spectrum a = s->acquire(o);                     // t ~ 0: mostly ethanol
        QVERIFY(band(a, 882) > 2 * band(a, 459));
        QThread::msleep(3000);                                // ~3000 s = 50 min: mostly CCl4
        const Spectrum b = s->acquire(o);
        QVERIFY(band(b, 459) > 2 * band(b, 882));
    }
};

QTEST_MAIN(TestSimulator)
#include "test_simulator.moc"
