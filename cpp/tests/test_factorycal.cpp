// Factory calibration of other instruments: BS_Device_Settings.ini stored per serial number.
#include <QTemporaryDir>
#include <QtTest>

#include "core/calibration.h"
#include "core/errors.h"

using namespace lr;

namespace {
QString writeIni(const QString& dir, const QString& serial, double laser532) {
    const QString p = dir + "/BS_Device_Settings.ini";
    QFile f(p);
    f.open(QIODevice::WriteOnly);
    QString t = "[SYSTEM]\r\n";
    if (!serial.isEmpty()) t += "Sys_SN=" + serial + "\r\n";
    t += "Sys_Laser_Count=1\r\nSys_TEC_Wait_Time_0=3\r\n[LASER_0]\r\n";
    t += QString("LS_WL=%1\r\nLS_WL_Coeff_0=514.1\r\nLS_WL_Coeff_1=0.0967\r\nLS_WL_Coeff_2=-0.0000066\r\n"
                 "LS_WL_Coeff_3=0.00000000006\r\nLS_Pwr_Coeff_0=0.29\r\nLS_Pwr_Coeff_1=0.0088\r\nLS_Pwr_Max=490\r\n")
             .arg(laser532, 0, 'f', 3);
    f.write(t.toLatin1());
    return p;
}
}  // namespace

class TestFactoryCal : public QObject {
    Q_OBJECT
    QTemporaryDir cfg;
private slots:
    void initTestCase() { qputenv("XDG_CONFIG_HOME", cfg.path().toUtf8()); }
    void importStoresPerSerial() {
        QTemporaryDir src;
        QVERIFY(!hasStoredFactory("12345678"));
        const Calibration c = importFactoryIni(writeIni(src.path(), "12345678", 532.25));
        QCOMPARE(c.serial, QString("12345678"));
        QVERIFY(hasStoredFactory("12345678"));
        QVERIFY(factoryIniPath("12345678").endsWith("libreraman/factory_12345678.ini"));
        QCOMPARE(c.source, factoryIniPath("12345678"));
        const Calibration back = loadIni(factoryIniPath("12345678"));   // the stored copy reads the same
        QVERIFY(!back.channels.empty());
        QCOMPARE(back.channels.begin()->second.laser_nm, 532.25);
        QVERIFY(!hasStoredFactory("99999999"));
    }
    void needsSerial() {
        QTemporaryDir src;
        QVERIFY_THROWS_EXCEPTION(lr::Error, importFactoryIni(writeIni(src.path(), "", 532.1)));
    }
    void unsafeSerialCharacters() {
        QVERIFY(!factoryIniPath("../x").contains(".."));
    }
};

QTEST_MAIN(TestFactoryCal)
#include "test_factorycal.moc"
