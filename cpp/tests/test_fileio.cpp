// Golden tests for core/fileio (Python: fileio.py). golden/files/ holds sample files of every format read
// by the Python reader (own CSV, series table, early CSV, vendor CSV/TXT, plain columns, JCAMP-DX
// XYPOINTS/XYDATA, SPC evenly spaced/with x array) and files written by the Python writers; golden/fileio.json
// holds what the Python read() returns for each. Values are parsed from the same text, so they are compared
// exactly (SPC: float32/int32 data, exact as well); written files are compared byte for byte.
#include <QDir>
#include <QJsonArray>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtTest>

#include "core/errors.h"
#include "core/fileio.h"
#include "golden_util.h"

using namespace lr;

namespace {

QString filesDir() { return golden::path("files"); }

QByteArray contents(const QString& path) {
    QFile f(path);
    return f.open(QIODevice::ReadOnly) ? f.readAll() : QByteArray();
}

QString compareSpectrum(const fileio::DataSpectrum& a, const QJsonObject& e) {
    if (a.name != e["name"].toString()) return "name " + a.name + " vs " + e["name"].toString();
    QString m = golden::compare(a.x, golden::vec(e["x"]), 0.0, 1e-15);
    if (!m.isEmpty()) return "x " + m;
    m = golden::compare(a.y, golden::vec(e["y"]), 0.0, 1e-15);
    if (!m.isEmpty()) return "y " + m;
    const QJsonArray meta = e["meta"].toArray();
    if (a.meta.size() != size_t(meta.size())) return QString("meta size %1 vs %2").arg(a.meta.size()).arg(meta.size());
    for (size_t i = 0; i < a.meta.size(); ++i) {
        const QJsonArray kv = meta[int(i)].toArray();
        if (a.meta[i].first != kv[0].toString() || a.meta[i].second != kv[1].toString())
            return "meta " + a.meta[i].first + "=" + a.meta[i].second + " vs " + kv[0].toString() + "=" + kv[1].toString();
    }
    if (QFileInfo(a.source).fileName() != e["source"].toString()) return "source " + a.source;
    for (const auto& [arr, key] : {std::pair<const Vec*, const char*>{&a.signal, "signal"}, {&a.background, "background"}}) {
        if (e[key].isNull() != arr->empty()) return QString("%1 presence").arg(key);
        if (!arr->empty()) {
            m = golden::compare(*arr, golden::vec(e[key]), 0.0, 1e-15);
            if (!m.isEmpty()) return QString("%1 ").arg(key) + m;
        }
    }
    return {};
}

#define CHECK_MSG(expr, what)                                                       \
    do {                                                                            \
        const QString m_ = (expr);                                                  \
        QVERIFY2(m_.isEmpty(), qPrintable(QString("%1: %2").arg(what, m_)));        \
    } while (0)

}  // namespace

class TestFileio : public QObject {
    Q_OBJECT
    QJsonObject g;

private slots:
    void initTestCase() { g = golden::load("fileio.json"); }

    void readAllFormats() {
        int n = 0;
        for (const QString& key : g.keys()) {
            if (!g[key].isArray()) continue;
            const QJsonArray e = g[key].toArray();
            const auto a = fileio::read(QDir(filesDir()).filePath(key));
            QCOMPARE(a.size(), size_t(e.size()));
            for (size_t i = 0; i < a.size(); ++i) CHECK_MSG(compareSpectrum(a[i], e[int(i)].toObject()), key);
            ++n;
        }
        QCOMPARE(n, 12);       // every sample file was checked
    }

    void readErrors() {
        const QJsonObject errors = g["errors"].toObject();
        for (const QString& fn : errors.keys()) {
            try {
                fileio::read(QDir(filesDir()).filePath(fn));
                QFAIL("no exception");
            } catch (const Error& e) {
                QCOMPARE(QString(e.what()).remove(filesDir() + "/"), errors[fn].toString());
            }
        }
        QVERIFY(golden::throws<Error>([&] { fileio::read(QDir(filesDir()).filePath("does_not_exist.csv")); }));
    }

    void writers() {
        const QJsonObject w = g["writers"].toObject();
        fileio::DataSpectrum a{"written, \"quoted\"", golden::vec(w["x"]), golden::vec(w["y"]),
                               {{"sample", "x"}, {"exposure s", "1.5"}}, {}, {}, {}};
        fileio::DataSpectrum b{"second", golden::vec(w["x2"]), golden::vec(w["y2"]), {}, {}, {}, {}};
        QTemporaryDir dir;
        fileio::writeCsv(dir.filePath("written.csv"), a);
        fileio::writeJcamp(dir.filePath("written.jdx"), a);
        fileio::writeTable(dir.filePath("table.csv"), {a, b}, 7.5);
        for (const char* fn : {"written.csv", "written.jdx", "table.csv"}) {
            const QByteArray mine = contents(dir.filePath(fn)), ref = contents(QDir(filesDir()).filePath(fn));
            QVERIFY2(!ref.isEmpty(), qPrintable(fn));
            QCOMPARE(mine, ref);
        }
        // round trip through the readers
        const auto r = fileio::read(dir.filePath("written.jdx"));
        QCOMPARE(r.size(), size_t(1));
        QCOMPARE(r[0].name, a.name);
        QCOMPARE(r[0].x.size(), a.x.size());
    }

    void fromSpectrum() {
        // the acquisition Spectrum the Python test wrote to own.csv (rows in reverse order)
        const QJsonObject e = g["from_spectrum"].toObject();
        Spectrum sp;
        sp.channel = 532;
        sp.exposure_s = 2.5;
        sp.averages = 3;
        auto rev = [](Vec v) { std::reverse(v.begin(), v.end()); return v; };
        sp.shift = rev(golden::vec(e["x"]));
        sp.intensity = rev(golden::vec(e["y"]));
        sp.signal = rev(golden::vec(e["signal"]));
        sp.background = rev(golden::vec(e["background"]));
        sp.meta = {{"sample", "ethanol, abs."}, {"laser_power_mw", "120"}, {"note", "a: b"}};
        CHECK_MSG(compareSpectrum(fileio::fromSpectrum(sp), e), "from_spectrum");

        const QJsonObject e2 = g["from_spectrum_named"].toObject();
        Spectrum s2;
        s2.channel = 1064;
        s2.exposure_s = 1.0;
        s2.averages = 1;
        s2.shift = golden::vec(e2["x"]);
        s2.intensity = golden::vec(e2["y"]);
        CHECK_MSG(compareSpectrum(fileio::fromSpectrum(s2, "named"), e2), "from_spectrum named");

        // a Spectrum written by Spectrum::saveCsv reads back like the Python file
        QTemporaryDir dir;
        sp.saveCsv(dir.filePath("own.csv"));
        const auto r = fileio::read(dir.filePath("own.csv"));
        QCOMPARE(r.size(), size_t(1));
        QCOMPARE(r[0].name, QString("ethanol, abs."));
        QCOMPARE(r[0].x, fileio::fromSpectrum(sp).x);
        QCOMPARE(r[0].signal, fileio::fromSpectrum(sp).signal);
    }

    void sorted() {
        fileio::DataSpectrum d{"s", {3, 1, 2}, {30, 10, 20}, {}, {}, {3.5, 1.5, 2.5}, {1, 2}};
        d.sorted();
        QCOMPARE(d.x, (Vec{1, 2, 3}));
        QCOMPARE(d.y, (Vec{10, 20, 30}));
        QCOMPARE(d.signal, (Vec{1.5, 2.5, 3.5}));
        QCOMPARE(d.background, (Vec{1, 2}));            // length differs: left alone, as in Python
        d.setMeta("a", "1");
        d.setMeta("a", "2");
        QCOMPARE(d.metaValue("a"), QString("2"));
        QCOMPARE(d.meta.size(), size_t(1));
    }

    void selftest() {
        const QJsonObject e = g["selftest"].toObject();
        const auto [ok, bad] = fileio::selftest(filesDir());
        QCOMPARE(ok, e["ok"].toInt());
        QStringList names;
        for (const auto& b : bad) names << QFileInfo(b.first).fileName();
        names.sort();
        QStringList expected;
        for (const QJsonValue& v : e["bad"].toArray()) expected << v.toString();
        QCOMPARE(names, expected);
    }
};

QTEST_GUILESS_MAIN(TestFileio)
#include "test_fileio.moc"
