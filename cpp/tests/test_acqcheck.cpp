// Laser-damage check (#9) and acquire-until-S/N (#10): metrics on synthetic frame series with and without
// change, the frame store, the S/N estimate and the stop rule, and the simulator's changing demo sample.
#include <QtTest>
#include <cmath>
#include <random>

#include "core/acqcheck.h"
#include "core/simulator.h"

using namespace lr;
using namespace lr::acq;

namespace {

Vec axis() {
    Vec x;
    for (double v = 100; v <= 3600; v += 2.4) x.push_back(v);
    return x;
}

struct Change {                       // at the last frame (linear in between)
    double shift = 0;                 // cm-1
    double fade = 0;                  // fraction of band height lost
    double background = 0;            // counts added to the background
};

double gauss(double x, double c, double s) { return std::exp(-0.5 * (x - c) * (x - c) / (s * s)); }

// Frames of a sample with bands at 1001, 1600 and 2930 cm-1 (heights h, 0.5 h, 0.75 h) on a flat background of
// 300 counts, with shot + read noise.
std::vector<Vec> series(const Vec& x, int n, const Change& c = {}, double h = 2000, unsigned seed = 1) {
    std::mt19937 rng(seed);
    std::normal_distribution<double> nd(0, 1);
    std::vector<Vec> frames;
    for (int k = 0; k < n; ++k) {
        const double f = n > 1 ? double(k) / (n - 1) : 0.0;
        const double d = -c.shift * f, a = 1 - c.fade * f, bg = 300 + c.background * f;
        Vec y(x.size());
        for (size_t i = 0; i < x.size(); ++i) {
            const double v = bg + a * h * (gauss(x[i], 1001 + d, 4) + 0.5 * gauss(x[i], 1600 + d, 6) +
                                           0.75 * gauss(x[i], 2930 + d, 8));
            y[i] = v + nd(rng) * std::sqrt(v + 64);
        }
        frames.push_back(std::move(y));
    }
    return frames;
}

}  // namespace

class TestAcqCheck : public QObject {
    Q_OBJECT
private slots:
    void noiseEstimate() {
        const Vec x = axis();
        const auto f = series(x, 1, {}, 2000, 7);
        const double n = noise(x, f[0]);
        // per-point noise sqrt(300 + 64) ~ 19 on the background (bands are a small part of the axis)
        QVERIFY2(n > 15 && n < 25, qPrintable(QString::number(n)));
    }

    void frameStoreThirds() {
        FrameStore s;
        QVERIFY(s.thirds().first.empty());
        for (int k = 0; k < 1000; ++k) s.add(Vec(4, double(k)));   // value = frame index
        QCOMPARE(s.count(), 1000);
        QCOMPARE(s.mean()[0], 499.5);
        int ne = 0, nl = 0;
        const auto [early, late] = s.thirds(&ne, &nl);
        QVERIFY(ne >= 333 && ne < 400);                             // block-aligned: about a third
        QVERIFY(nl >= 333 && nl < 400);
        QVERIFY(std::abs(early[0] - (ne - 1) / 2.0) < 1e-9);       // frames 0..ne-1
        QVERIFY(std::abs(late[0] - (999 - (nl - 1) / 2.0)) < 1e-9);
        FrameStore two;
        two.add(Vec(2, 1.0));
        two.add(Vec(2, 3.0));
        const auto [e2, l2] = two.thirds(&ne, &nl);
        QCOMPARE(ne, 1);
        QCOMPARE(nl, 1);
        QCOMPARE(e2[0], 1.0);
        QCOMPARE(l2[0], 3.0);
    }

    void alignFramesBySpectrumPixels() {
        Spectrum sp;                                                // sorted by shift: pixels 2, 1, 0
        sp.shift = {10, 20, 30};
        sp.pixels = {2, 1, 0};
        sp.background = {1, 1, 1};
        const auto out = alignFrames(sp, {{100, 200, 300}}, {0, 1, 2});
        QCOMPARE(out.size(), size_t(1));
        QCOMPARE(out[0], (Vec{299, 199, 99}));
    }

    void stableSampleNotFlagged() {
        const Vec x = axis();
        for (unsigned seed = 1; seed <= 5; ++seed) {
            const DamageReport r = checkFrames(x, series(x, 12, {}, 2000, seed));
            QVERIFY(r.valid);
            QVERIFY2(!r.changing, qPrintable(r.summary()));
            QCOMPARE(r.framesEarly, 4);
            QCOMPARE(r.framesLate, 4);
            QCOMPARE(int(r.bands.size()), 3);
            QVERIFY(std::abs(r.bands[0].early - 1001) < 1);         // strongest first
        }
        // two frames (the minimum) and a weak sample: no false alarm either
        QVERIFY(!checkFrames(x, series(x, 2)).changing);
        QVERIFY(!checkFrames(x, series(x, 12, {}, 150)).changing);
        QVERIFY(!checkFrames(x, series(x, 1)).valid);              // one frame: nothing to compare
    }

    void cosmicRayNotFlagged() {
        const Vec x = axis();
        auto f = series(x, 6);
        f[0][700] += 12000;                                         // two-pixel cosmic ray in the first frame
        f[0][701] += 12000;
        const DamageReport r = checkFrames(x, f);
        QVERIFY2(!r.changing, qPrintable(r.summary()));
    }

    void shiftDetected() {
        const Vec x = axis();
        const DamageReport r = checkFrames(x, series(x, 12, {6.0, 0, 0}));
        QVERIFY(r.changing);
        QVERIFY(r.shifted);
        QVERIFY(!r.faded);
        QVERIFY(!r.background);
        QVERIFY2(r.maxShift < -2.5 && r.maxShift > -6.5, qPrintable(QString::number(r.maxShift)));
        QVERIFY(r.summary().startsWith(QStringLiteral("band ")));
        // a small drift stays below the threshold
        QVERIFY(!checkFrames(x, series(x, 12, {1.0, 0, 0})).shifted);
    }

    void fadeDetected() {
        const Vec x = axis();
        const DamageReport r = checkFrames(x, series(x, 12, {0, 0.4, 0}));
        QVERIFY(r.faded);
        QVERIFY(!r.shifted);
        QVERIFY(r.changing);
        QVERIFY(r.intensityChange < -0.15);
        QVERIFY(r.summary().startsWith(QStringLiteral("bands −")));
        QVERIFY(!checkFrames(x, series(x, 12, {0, 0.1, 0})).faded);
    }

    void backgroundRiseDetected() {
        const Vec x = axis();
        const DamageReport r = checkFrames(x, series(x, 12, {0, 0, 1500}));   // +75 % of the strongest band
        QVERIFY(r.background);
        QVERIFY(r.changing);
        QVERIFY(r.backgroundChange > 0.2);
        QVERIFY(!checkFrames(x, series(x, 12, {0, 0, 200})).background);    // +10 %
        // a falling background (photobleaching of fluorescence) is not damage
        QVERIFY(!checkFrames(x, series(x, 12, {0, 0, -250})).changing);
    }

    void damageMeta() {
        const Vec x = axis();
        Spectrum sp;
        setDamageMeta(sp, DamageReport{});                          // invalid: nothing
        QVERIFY(sp.meta.empty());
        setDamageMeta(sp, checkFrames(x, series(x, 12, {0, 0.4, 0})));
        QCOMPARE(sp.metaValue(QStringLiteral("laser_damage")), QStringLiteral("suspected"));
        QVERIFY(sp.metaValue(QStringLiteral("laser_damage_check")).contains(QStringLiteral("first 4 vs last 4 frames")));
        setDamageMeta(sp, checkFrames(x, series(x, 12)));
        QCOMPARE(sp.metaValue(QStringLiteral("laser_damage")), QStringLiteral("none detected"));
        QVERIFY(damageAdvice().contains(QStringLiteral("shorten the exposure")));
        QVERIFY(!damageAdvice().contains(QStringLiteral("power")));   // the Cora's laser power cannot be changed
    }

    void snrEstimate() {
        const Vec x = axis();
        const auto f = series(x, 16);
        const Snr one = estimateSnr(x, f[0]);
        QVERIFY(one.valid());
        QVERIFY(std::abs(one.band - 1001) < 1.5);
        // height 2000 over per-point noise ~19 (background) -> ~100; the estimate is within a factor ~1.3
        QVERIFY2(one.snr > 75 && one.snr < 140, qPrintable(QString::number(one.snr)));
        const Snr band = estimateSnr(x, f[0], 1600.0);
        QVERIFY(std::abs(band.band - 1600) < 1.5);
        QVERIFY(band.snr < 0.7 * one.snr);
        QVERIFY(!estimateSnr(x, f[0], 5000.0).valid());             // outside the axis
        // S/N grows with the square root of the number of frames
        FrameStore s;
        for (const Vec& v : f) s.add(v);
        const double r = estimateSnr(x, s.mean()).snr / one.snr;
        QVERIFY2(r > 3.0 && r < 5.3, qPrintable(QString::number(r)));   // sqrt(16) = 4
    }

    void stopRule() {
        SnrTarget t;
        t.target = 100;
        t.maxSeconds = 60;
        QCOMPARE(snrStop(t, 150, 2, 2, 1, false), Stop::Continue);     // below minFrames
        QCOMPARE(snrStop(t, 150, 3, 3, 1, false), Stop::Reached);
        QCOMPARE(snrStop(t, 50, 10, 10, 1, false), Stop::Continue);
        QCOMPARE(snrStop(t, 50, 59, 59.5, 1, false), Stop::TimeLimit);  // next frame would end after 60 s
        QCOMPARE(snrStop(t, 50, 5, 5, 1, true), Stop::Continue);       // too few frames to trust the check
        QCOMPARE(snrStop(t, 150, 6, 6, 1, true), Stop::Damage);        // damage wins
        t.stopOnDamage = false;
        QCOMPARE(snrStop(t, 50, 6, 6, 1, true), Stop::Continue);
        QCOMPARE(snrStop(t, std::nan(""), 6, 6, 1, false), Stop::Continue);
        QCOMPARE(framesNeeded(50, 4, 100), 16);
        QCOMPARE(framesNeeded(200, 4, 100), 4);
        QCOMPARE(framesNeeded(std::nan(""), 4, 100), -1);
        QCOMPARE(stopText(Stop::Reached), QStringLiteral("target S/N reached"));
    }

    void snrRunReachesTarget() {
        const Vec x = axis();
        SnrTarget t;
        t.target = 300;                                             // ~100 per frame -> about 9 frames
        t.maxSeconds = 100;
        SnrRun run(t);
        const auto f = series(x, 40);
        Stop s = Stop::Continue;
        int k = 0;
        while (s == Stop::Continue && k < 40) s = run.add(x, f[size_t(k)], k + 1.0, 1.0), ++k;
        QCOMPARE(s, Stop::Reached);
        QVERIFY2(run.frames() >= 5 && run.frames() <= 20, qPrintable(QString::number(run.frames())));
        QVERIFY(run.snr().snr >= 300);
        QVERIFY(run.damage().valid);
        QVERIFY(!run.damage().changing);
    }

    void snrRunStopsOnDamageAndTime() {
        const Vec x = axis();
        SnrTarget t;
        t.target = 1e6;                                             // never reached
        t.maxSeconds = 1e6;
        SnrRun run(t);
        const auto f = series(x, 30, {0, 0.6, 0});
        Stop s = Stop::Continue;
        for (int k = 0; k < 30 && s == Stop::Continue; ++k) s = run.add(x, f[size_t(k)], k + 1.0, 1.0);
        QCOMPARE(s, Stop::Damage);
        QVERIFY(run.frames() < 30);
        QVERIFY(run.damage().faded);
        t.maxSeconds = 5;
        SnrRun timed(t);
        s = Stop::Continue;
        int k = 0;
        while (s == Stop::Continue) s = timed.add(x, f[0], k + 1.0, 1.0), ++k;
        QCOMPARE(s, Stop::TimeLimit);
        QCOMPARE(timed.frames(), 5);
    }

    // The simulator's demo sample changes under the laser; the recorded samples do not (ZnS bleaches slowly).
    void simulatorDemo() {
      for (uint64_t seed = 1; seed <= 3; ++seed) {
        SimCora s(defaultCalibration(), [](const QString&) {}, 500.0, seed);
        s.connect();
        s.initialize();
        s.closeCompartment();
        s.pressArm();
        QVERIFY(s.sampleNames().contains(simulatorDamageName()));
        for (const QString& sample : s.sampleNames()) {
            if (sample.startsWith(QStringLiteral("reaction:"))) continue;
            s.setSample(sample);
            std::vector<Vec> raw;
            AcquireOptions o;
            o.exposure_s = 1.0;
            o.averages = 12;
            o.frames = &raw;
            const Spectrum sp = s.acquire(o);
            QCOMPARE(int(raw.size()), o.averages);
            const auto frames = alignFrames(sp, raw, s.pixelNumbers(532));
            const DamageReport r = checkFrames(sp.shift, frames);
            QVERIFY2(r.valid, qPrintable(sample));
            QVERIFY2(r.changing == (sample == simulatorDamageName()), qPrintable(sample + ": " + r.summary()));
        }
      }
    }
};

QTEST_MAIN(TestAcqCheck)
#include "test_acqcheck.moc"
