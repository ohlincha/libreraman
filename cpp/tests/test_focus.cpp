// Focus mode: the focus signal (strongest band, total Raman signal, saturation) and the laser rule: the
// compartment may read open only in focus mode (lid-open accessory), and arming is still required.
#include <QtTest>
#include <cmath>

#include "core/errors.h"
#include "core/focus.h"
#include "core/simulator.h"

using namespace lr;

namespace {
std::unique_ptr<SimCora> ready() {
    auto s = std::make_unique<SimCora>(defaultCalibration(), [](const QString&) {}, 500.0, 1);
    s->connect();
    s->initialize();
    return s;
}
Spectrum synthetic(double bandHeight, double rawMax) {
    Spectrum sp;
    for (int i = 0; i < 1500; ++i) {
        const double x = 100 + 2.0 * i;   // 100..3098 cm-1
        sp.shift.push_back(x);
        // sloping fluorescence + a band at 1001 cm-1 (sigma 5) + a smaller one at 1600
        sp.intensity.push_back(500 + 0.3 * x + bandHeight * std::exp(-0.5 * std::pow((x - 1001) / 5, 2)) +
                               0.4 * bandHeight * std::exp(-0.5 * std::pow((x - 1600) / 8, 2)));
    }
    sp.signal.assign(sp.intensity.size(), 1000);
    sp.signal[10] = rawMax;
    return sp;
}
}  // namespace

class TestFocus : public QObject {
    Q_OBJECT
private slots:
    void signal() {
        const focus::Signal a = focus::measure(synthetic(1000, 2000)), b = focus::measure(synthetic(2000, 2000));
        QVERIFY2(std::abs(a.bandShift - 1001) <= 2, qPrintable(QString::number(a.bandShift)));
        QVERIFY2(std::abs(a.band - 1000) < 60, qPrintable(QString::number(a.band)));
        QVERIFY2(b.band > 1.8 * a.band && b.total > 1.8 * a.total, qPrintable(QString("%1 %2 %3 %4").arg(a.band).arg(b.band).arg(a.total).arg(b.total)));
        QVERIFY(!a.saturated);
        const focus::Signal f = focus::measure(synthetic(1000, 2000), 1600);           // follow another band
        QVERIFY(std::abs(f.bandShift - 1600) <= 2 && f.band < 0.6 * a.band);
        QVERIFY(focus::measure(synthetic(1000, 65535)).saturated);
    }

    void laserRule() {
        auto s = ready();
        s->pressArm();
        s->setLidAccessory(true);
        s->openCompartment();                 // with the accessory: still armed
        AcquireOptions o;
        o.exposure_s = 0.1;
        QVERIFY_THROWS_EXCEPTION(NotArmed, s->acquire(o));   // normal measurements: never with the lid open
        int n = 0;
        AcquireOptions f = o;
        f.allowOpenCompartment = true;        // focus mode
        s->acquireLaserOn(f, s->dark(0.1), [&n] { return n < 3; }, [&n](Spectrum&) { ++n; });
        QCOMPARE(n, 3);
        QVERIFY(!s->laserOn());               // off afterwards
        // without the accessory, opening disarms: focus mode refuses too (not armed)
        auto t = ready();
        t->pressArm();
        t->openCompartment();
        QVERIFY_THROWS_EXCEPTION(NotArmed, t->acquireLaserOn(f, t->dark(0.1), [] { return false; }, {}));
    }
};

QTEST_GUILESS_MAIN(TestFocus)
#include "test_focus.moc"
