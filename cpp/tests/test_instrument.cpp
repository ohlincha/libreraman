// Instrument helpers (makeSpectrum, shiftOf, user calibration), calib::load of the Python JSON, Python-style
// formatting and the hardware-independent parts of the spectrometer (exposure registers, pixel order).
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>

#include "core/calibrate.h"
#include "core/errors.h"
#include "core/hw/spectrometer.h"
#include "core/instrument.h"
#include "core/pyformat.h"

using namespace lr;

// Written by calibrate.save() of the Python version (json.dump(..., indent=1)).
static const char* PY_JSON = R"({
 "poly": {
  "coeff": [
   1000.5,
   1234.25,
   -3.5,
   0.125
  ],
  "x0": 1023.5,
  "scale": 1000.0
 },
 "residuals": [
  [
   1001.4,
   0.12,
   514.9
  ],
  [
   3061.0,
   -0.3,
   1800.2
  ]
 ],
 "rms": 0.6234,
 "matches": [
  [
   "benzonitrile",
   [
    [
     514.9,
     1001.4,
     10,
     500.0
    ]
   ]
  ]
 ],
 "standards": [
  "benzonitrile",
  "chloroform"
 ],
 "date": "2026-11-20 14:03",
 "degree": 3,
 "channel": 532,
 "serial": "99002169"
})";

class TestInst : public Instrument {
public:
    explicit TestInst(Calibration c = defaultCalibration()) : Instrument(std::move(c), [this](const QString& m) { logs << m; }) {}
    QStringList logs;
    void connect() override {}
    void initialize(const ProgressFn&) override {}
    bool needsInitialize() const override { return false; }
    void resume() override {}
    Status status() override { return {}; }
    void keepalive() override {}
    void selectChannel(int ch, bool) override { channel_ = ch; }
    Vec dark(double, int) override { return {}; }
    Spectrum acquire(const AcquireOptions&) override { return {}; }
    void acquireLaserOn(const AcquireOptions&, const Vec&, const std::function<bool()>&,
                        const std::function<void(Spectrum&)>&) override {}
    void laserOffNow() override {}
    Vec pixelNumbers(int) const override { return {}; }
    void close(bool) override {}
    QString serial() override { return "TEST"; }
    using Instrument::loadUserCalibration;
    using Instrument::makeSpectrum;
};

class TestInstrument : public QObject {
    Q_OBJECT
private slots:
    void pythonFormatting() {
        QCOMPARE(py::str(532.1), QString("532.1"));
        QCOMPARE(py::str(50.0), QString("50.0"));
        QCOMPARE(py::str(1064.01), QString("1064.01"));
        QCOMPARE(py::str(0.1 + 0.2), QString("0.30000000000000004"));
        QCOMPARE(py::str(1e16), QString("1e+16"));
        QCOMPARE(py::str(1234567890123456.0), QString("1234567890123456.0"));
        QCOMPARE(py::str(1.5e-5), QString("1.5e-05"));
        QCOMPARE(py::str(0.0001), QString("0.0001"));
        QCOMPARE(py::str(-2.5), QString("-2.5"));
        QCOMPARE(py::str(100.0), QString("100.0"));
        QCOMPARE(py::str(970.1781676344818), QString("970.1781676344818"));
        QCOMPARE(py::fixed(12.25, 1), QString("12.2"));           // ties to even on the exact binary value
        QCOMPARE(py::fixed(2.5, 0), QString("2"));
        QCOMPARE(py::fixed(0.125, 2), QString("0.12"));
        QCOMPARE(py::fixed(-0.04, 1), QString("-0.0"));
        QCOMPARE(py::g(10.0), QString("10"));
        QCOMPARE(py::g(2.5), QString("2.5"));
        QCOMPARE(py::str(true), QString("True"));
        QCOMPARE(py::str(std::vector<int>{532, 1064}), QString("[532, 1064]"));
        QCOMPARE(py::str(std::vector<int>{}), QString("[]"));
        QCOMPARE(py::repr("TECE"), QString("'TECE'"));
        QCOMPARE(py::repr("it's"), QString("\"it's\""));
        QCOMPARE(py::repr("a\nb\x01"), QString("'a\\nb\\x01'"));
    }

    void factoryShift() {
        TestInst t;
        QCOMPARE(t.shiftOf(532, 2047), 4190.267581659709);       // values from calibration.py
        QCOMPARE(t.shiftOf(532, 0), -661.5563739259451);
        QCOMPARE(t.shiftOf(532, 1000), 2241.343783087261);
        QCOMPARE(t.shiftOf(1064, 255), 2374.724739202128);
        QCOMPARE(t.calibrationLabel(532), QString("factory (built-in)"));
    }

    void userCalibration() {
        TestInst t;
        UserCalibration c;
        c.poly.coeff = {1000.5, 1234.25, -3.5, 0.125};
        c.poly.x0 = 1023.5;
        c.poly.scale = 1000.0;
        c.date = "2026-11-20 14:03";
        c.standards = {"benzonitrile", "chloroform"};
        t.setUserCalibration(532, c);
        QCOMPARE(t.shiftOf(532, 1500.0), 1587.8389659246407);    // calibrate.polyval
        QCOMPARE(t.shiftOf(532, 12.0), -251.65420015885954);
        QCOMPARE(t.shiftOf(1064, 255), 2374.724739202128);       // other channel: factory
        QCOMPARE(t.calibrationLabel(532), QString("user 2026-11-20 14:03 (benzonitrile, chloroform)"));
    }

    void makeSpectrum532() {
        TestInst t;
        const Vec px = hw::Spectrometer::pixelNumbers(532);      // data order: 2047 ... 0
        QCOMPARE(int(px.size()), 2048);
        QCOMPARE(px.front(), 2047.0);
        QCOMPARE(px.back(), 0.0);
        Vec sig(px.size()), bg(px.size());
        for (size_t i = 0; i < px.size(); ++i) {
            sig[i] = 1000.0 + double(i);
            bg[i] = 10.0 + double(i % 7);
        }
        Spectrum sp = t.makeSpectrum(532, 1.5, 3, px, sig, bg, 0.0);
        QCOMPARE(sp.channel, 532);
        QCOMPARE(sp.exposure_s, 1.5);
        QCOMPARE(sp.averages, 3);
        QCOMPARE(int(sp.shift.size()), 2048);
        for (size_t i = 1; i < sp.shift.size(); ++i) QVERIFY(sp.shift[i] > sp.shift[i - 1]);   // ascending
        // reversed data order: the first point is pixel 0 = data index 2047
        QCOMPARE(sp.pixels.front(), 0.0);
        QCOMPARE(sp.signal.front(), 1000.0 + 2047);
        QCOMPARE(sp.background.front(), 10.0 + double(2047 % 7));
        QCOMPARE(sp.intensity.front(), sp.signal.front() - sp.background.front());
        QCOMPARE(sp.shift.front(), -661.5563739259451);
        QCOMPARE(sp.pixels.back(), 2047.0);
        QCOMPARE(sp.meta.size(), size_t(3));
        QCOMPARE(sp.meta[0].first, QString("laser_nm"));
        QCOMPARE(sp.meta[0].second, QString("532.1"));
        QCOMPARE(sp.meta[1], qMakePair(QString("calibration"), QString("factory (built-in)")));
        QCOMPARE(sp.meta[2], qMakePair(QString("power"), QString("set on the instrument (not computer-controlled)")));
    }

    void makeSpectrum1064() {
        TestInst t;
        const Vec px = hw::Spectrometer::pixelNumbers(1064);
        QCOMPARE(int(px.size()), 256);
        QCOMPARE(px.front(), 0.0);
        const Vec sig(256, 5.0), bg(256, 2.0);
        Spectrum sp = t.makeSpectrum(1064, 1.0, 1, px, sig, bg, 0.0);
        QCOMPARE(sp.metaValue("laser_nm"), QString("1064.01"));
        QCOMPARE(sp.metaValue("power_mw"), QString("50.0"));
        QCOMPARE(sp.metaValue("note"), QString("1064 nm pixel orientation not yet verified"));
        QCOMPARE(sp.intensity[10], 3.0);
        sp = t.makeSpectrum(1064, 1.0, 1, px, sig, bg, 200.0);
        QCOMPARE(sp.metaValue("power_mw"), QString("200.0"));
    }

    void meanFramesInOrder() {
        QCOMPARE(meanFrames({{1, 2, 3}, {3, 4, 5}}), Vec({2, 3, 4}));
        QCOMPARE(meanFrames({{1, 2, 3}}), Vec({1, 2, 3}));
        QVERIFY(meanFrames({}).empty());
    }

    void loadPythonJson() {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        qputenv("XDG_CONFIG_HOME", dir.path().toUtf8());
        QCOMPARE(calib::configDir(), QDir::cleanPath(dir.path() + "/libreramen"));
        QVERIFY(!calib::load(532, "99002169").has_value());       // no file
        QVERIFY(QDir().mkpath(calib::configDir()));
        QFile f(calib::configDir() + "/calibration_99002169_532.json");
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(PY_JSON);
        f.close();
        const auto c = calib::load(532, "99002169");
        QVERIFY(c.has_value());
        QCOMPARE(c->poly.coeff, std::vector<double>({1000.5, 1234.25, -3.5, 0.125}));
        QCOMPARE(c->poly.x0, 1023.5);
        QCOMPARE(c->poly.scale, 1000.0);
        QCOMPARE(c->date, QString("2026-11-20 14:03"));
        QCOMPARE(c->standards, std::vector<QString>({"benzonitrile", "chloroform"}));
        QCOMPARE(c->rms, 0.6234);
        QCOMPARE(c->poly(1500.0), 1587.8389659246407);
        QVERIFY(!calib::load(1064, "99002169").has_value());
        // explicit path
        const auto d = calib::load(1064, "x", f.fileName());
        QVERIFY(d.has_value());
        QCOMPARE(d->rms, 0.6234);
        // used by the instrument (calibration serial of the default calibration = 99002169)
        TestInst t;
        t.loadUserCalibration();
        QCOMPARE(int(t.userCalibration().size()), 1);
        QCOMPARE(t.logs.value(0),
                 QString("Using calibration for 532 nm from 2026-11-20 14:03 (benzonitrile, chloroform, RMS 0.62 cm-1)"));
        QCOMPARE(t.shiftOf(532, 12.0), -251.65420015885954);
        // broken file
        QFile g(dir.path() + "/bad.json");
        QVERIFY(g.open(QIODevice::WriteOnly));
        g.write("{nope");
        g.close();
        bool threw = false;
        try {
            calib::load(532, "x", g.fileName());
        } catch (const Error&) {
            threw = true;
        }
        QVERIFY(threw);
        qunsetenv("XDG_CONFIG_HOME");
    }

    void exposureRegisters() {
        using hw::Spectrometer;
        using P = std::pair<quint32, quint32>;                  // values computed with hardware.py
        QCOMPARE(Spectrometer::exposureRaw(532, 1.0), P(62499, 20));
        QCOMPARE(Spectrometer::exposureRaw(532, 1.2), P(74999, 17));
        QCOMPARE(Spectrometer::exposureRaw(532, 6.0), P(374999, 3));
        QCOMPARE(Spectrometer::exposureRaw(532, 20.0), P(1249999, 1));
        QCOMPARE(Spectrometer::exposureRaw(532, 0.1), P(6249, 200));
        QCOMPARE(Spectrometer::exposureRaw(1064, 1.0), P(1249975, 1));
        QCOMPARE(Spectrometer::exposureRaw(1064, 20.0), P(24999975, 1));
        QCOMPARE(Spectrometer::productName(532), QString("BS2ACCD"));
        QCOMPARE(Spectrometer::productName(1064), QString("BS2BASE"));
    }
};

QTEST_MAIN(TestInstrument)
#include "test_instrument.moc"
