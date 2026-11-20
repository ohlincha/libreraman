// Command line (src/cli) against the Python reference: `standards` and offline `calibrate --from` must
// print exactly what `python3 -m libreramen` prints (tests/golden/cli.json, made by make_golden.py
// gold_cli()); live `calibrate` is run against the simulator.
#include "core/pyformat.h"

#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QDir>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>

namespace {

QJsonObject load(const QString& name) {
    QFile f(QStringLiteral(GOLDEN_DIR "/") + name);
    if (!f.open(QIODevice::ReadOnly)) qFatal("cannot open %s", qPrintable(f.fileName()));
    return QJsonDocument::fromJson(f.readAll()).object();
}

struct Run {
    int code = -1;
    QString out;
};

Run cli(const QStringList& args, const QByteArray& input = {}, int timeoutMs = 60000) {
    QProcess p;
    p.setProcessChannelMode(QProcess::SeparateChannels);
    p.start(QStringLiteral(CLI_PATH), args);
    if (!p.waitForStarted()) return {};
    if (!input.isEmpty()) p.write(input);
    p.closeWriteChannel();
    if (!p.waitForFinished(timeoutMs)) {
        p.kill();
        p.waitForFinished();
        return {};
    }
    // Windows consoles write "\r\n" (like Python's print there); the golden output is from Linux.
    QString out = QString::fromUtf8(p.readAllStandardOutput());
    out.replace(QStringLiteral("\r\n"), QStringLiteral("\n"));
    return {p.exitStatus() == QProcess::NormalExit ? p.exitCode() : -1, out};
}

}  // namespace

class TestCli : public QObject {
    Q_OBJECT
private slots:
    void goldenRuns();
    void savedCalibration();
    void liveCalibrationSimulator();
};

void TestCli::goldenRuns() {
    const QJsonObject gold = load("cli.json");
    const QJsonObject spectra = load("calibrate.json")["spectra"].toObject();
    QTemporaryDir tmp;
    QVERIFY(tmp.isValid());
    const QString dir = tmp.path() + "/";
    const QJsonObject files = gold["files"].toObject();
    for (auto it = files.begin(); it != files.end(); ++it) {      // same text as gold_cli() wrote
        QFile f(dir + it.key());
        QVERIFY(f.open(QIODevice::WriteOnly));
        QString s = "# test spectrum (calibration pixel order)\npixel,intensity\n";
        const QJsonArray y = spectra[it.value().toString()].toArray();
        for (int i = 0; i < y.size(); ++i) s += QString::number(i) + "," + lr::py::str(y[i].toDouble()) + "\n";
        f.write(s.toUtf8());
    }
    const QJsonArray runs = gold["runs"].toArray();
    QVERIFY(runs.size() >= 5);
    for (const QJsonValue& v : runs) {
        const QJsonObject r = v.toObject();
        QStringList args;
        for (const QJsonValue& a : r["args"].toArray()) {
            const QString s = a.toString();
            args << ((files.contains(s) || s == "OUT.json") ? dir + s : s);
        }
        const Run run = cli(args);
        QString out = QString(run.out).replace(dir, "DIR/");
        out.replace(QDir::toNativeSeparators(dir), "DIR/");   // paths printed with native separators (Windows)
        QCOMPARE(out, r["stdout"].toString());
        QCOMPARE(run.code, r["rc"].toInt());
    }
}

void TestCli::savedCalibration() {
    // -o: JSON readable as a calibration (same structure as the Python save())
    const QJsonObject spectra = load("calibrate.json")["spectra"].toObject();
    QTemporaryDir tmp;
    QFile f(tmp.path() + "/etoh.csv");
    QVERIFY(f.open(QIODevice::WriteOnly));
    QString s = "pixel,intensity\n";
    const QJsonArray y = spectra["ethanol"].toArray();
    for (int i = 0; i < y.size(); ++i) s += QString::number(i) + "," + lr::py::str(y[i].toDouble()) + "\n";
    f.write(s.toUtf8());
    f.close();
    const QString outPath = tmp.path() + "/cal.json";
    const Run run = cli({"calibrate", "--standard", "etoh", "--from", f.fileName(), "-o", outPath});
    QCOMPARE(run.code, 0);
    QVERIFY(run.out.endsWith("saved " + outPath + "\n"));
    QFile j(outPath);
    QVERIFY(j.open(QIODevice::ReadOnly));
    const QJsonObject o = QJsonDocument::fromJson(j.readAll()).object();
    QCOMPARE(o["channel"].toInt(), 532);
    QCOMPARE(o["serial"].toString(), QStringLiteral("99002169"));
    QCOMPARE(o["poly"].toObject()["coeff"].toArray().size(), 4);
    // errors: unknown standard, missing file
    Run bad = cli({"calibrate", "--standard", "unobtainium", "--from", f.fileName()});
    QCOMPARE(bad.code, 1);
    QVERIFY(bad.out.startsWith("error: "));
    bad = cli({"calibrate", "--standard", "ethanol", "--from", tmp.path() + "/missing.csv"});
    QCOMPARE(bad.code, 1);
    QVERIFY(bad.out.startsWith("error: "));
}

void TestCli::liveCalibrationSimulator() {
    QTemporaryDir cfg;                                   // never touch the user's calibration
    qputenv("XDG_CONFIG_HOME", cfg.path().toUtf8());
    const Run run = cli({"calibrate", "--sim", "--speed", "200", "--standard", "ccl4", "--standard", "ethanol", "-e",
                         "0.2"},
                        "\n\nn\n", 180000);
    qunsetenv("XDG_CONFIG_HOME");
    QVERIFY2(run.code == 0, qPrintable(run.out));
    QVERIFY(run.out.contains("Put in ccl4, close the compartment, press ARM, then press Enter here… "));
    QVERIFY(run.out.contains("Put in ethanol, close the compartment"));
    QVERIFY2(run.out.contains("Calibration with ccl4, ethanol: "), qPrintable(run.out));
    QVERIFY(run.out.contains("  ccl4: 5 of 5 reference bands matched"));
    QVERIFY(run.out.contains("  reference   residual   pixel   before\n"));
    QVERIFY(run.out.contains("Save this calibration? [y/N] "));
    QVERIFY(!run.out.contains("saved "));
    // without a standard: error after connecting (as Python)
    const Run none = cli({"calibrate", "--sim", "--speed", "200"}, {}, 120000);
    QCOMPARE(none.code, 1);
    QVERIFY2(none.out.contains("error: give at least one --standard"), qPrintable(none.out));
}

QTEST_GUILESS_MAIN(TestCli)
#include "test_cli.moc"
