// Golden tests for core/fileio (Python: fileio.py). golden/files/ holds sample files of every format read
// by the Python reader (own CSV, series table, early CSV, vendor CSV/TXT, plain columns, JCAMP-DX
// XYPOINTS/XYDATA, SPC evenly spaced/with x array) and files written by the Python writers; golden/fileio.json
// holds what the Python read() returns for each. Values are parsed from the same text, so they are compared
// exactly (SPC: float32/int32 data, exact as well); written files are compared byte for byte.
#include <QDir>
#include <QJsonArray>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QtEndian>
#include <QtTest>
#include <cmath>
#include <cstring>

#include "core/errors.h"
#include "core/fileio.h"
#include "golden_util.h"

using namespace lr;

namespace {

QString filesDir() { return golden::path("files"); }

void writeFile(const QString& path, const QByteArray& b) {
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
    f.write(b);
}

double f32(const QByteArray& b, qsizetype off) {
    const quint32 w = qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(b.constData()) + off);
    float f;
    std::memcpy(&f, &w, 4);
    return double(f);
}
quint32 u32(const QByteArray& b, qsizetype off) {
    return qFromLittleEndian<quint32>(reinterpret_cast<const uchar*>(b.constData()) + off);
}
double f64(const QByteArray& b, qsizetype off) {
    const quint64 w = qFromLittleEndian<quint64>(reinterpret_cast<const uchar*>(b.constData()) + off);
    double d;
    std::memcpy(&d, &w, 8);
    return d;
}
double asFloat(double v) { return double(float(v)); }
double as4(double v) { return QString::number(v, 'f', 4).toDouble(); }

// A small time series: uneven x (like a calibrated Raman shift axis), 3 spectra.
std::vector<fileio::DataSpectrum> series3(int npts = 40) {
    std::vector<fileio::DataSpectrum> out;
    for (int k = 0; k < 3; ++k) {
        fileio::DataSpectrum d;
        d.name = QString("CCl4 t=%1 s").arg(k * 2.5);
        for (int i = 0; i < npts; ++i) {
            d.x.push_back(100.123456 + 20.5 * i + 0.01 * i * i);
            d.y.push_back(1000.0 * std::exp(-0.01 * (i - 15 - k) * (i - 15 - k)) + 12.345678 * k - 3.21);
        }
        d.meta = {{"sample", "CCl4"},
                  {"series_index", QString::number(k + 1)},
                  {"series_elapsed_s", QString::number(k * 2.5)},
                  {"exposure_s", "0.5"},
                  {"calibration", "532 nm: 2026-11-20"},
                  {"acquired", QString("2026-11-20T18:21:0%1+01:00").arg(k * 2)}};
        out.push_back(d);
    }
    return out;
}

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

    void jcampMultiRoundTrip() {
        QTemporaryDir dir;
        const auto in = series3();
        const QString path = dir.filePath("series.jdx");
        fileio::writeJcampMulti(path, in, "CCl4 time series", {{"sample", "CCl4"}, {"started", "2026-11-20T18:21:00+01:00"}});
        const QString text = QString::fromUtf8(contents(path));
        const QStringList lines = text.split('\n', Qt::SkipEmptyParts);
        // LINK block, then one block per spectrum, then the LINK block's END
        QCOMPARE(lines.mid(0, 6), QStringList({"##TITLE=CCl4 time series", "##JCAMP-DX=5.01", "##DATA TYPE=LINK",
                                               "##BLOCKS=3", "##ORIGIN=LibreRamen", "##OWNER="}));
        QCOMPARE(lines[6], QString("##LONGDATE=2026/11/20 18:21:00 +0100"));
        QCOMPARE(lines[7], QString("##$SAMPLE=CCl4"));
        QCOMPARE(lines[9], QString("##TITLE=CCl4 t=0 s"));
        QCOMPARE(lines.count("##END="), 4);
        QCOMPARE(lines.last(), QString("##END="));
        for (int k = 1; k <= 3; ++k) QVERIFY(lines.contains(QString("##BLOCK_ID=%1").arg(k)));
        QCOMPARE(lines.count("##DATA TYPE=RAMAN SPECTRUM"), 3);
        QCOMPARE(lines.count("##XYPOINTS=(XY..XY)"), 3);
        QVERIFY(lines.contains("##LONGDATE=2026/11/20 18:21:04 +0100"));
        QVERIFY(lines.contains("##$SERIES_ELAPSED_S=2.5"));
        QVERIFY(lines.contains("##YUNITS=COUNTS"));
        QVERIFY(lines.contains("##NPOINTS=40"));

        const auto r = fileio::read(path);
        QCOMPARE(r.size(), size_t(3));
        for (size_t k = 0; k < 3; ++k) {
            QCOMPARE(r[k].name, in[k].name);
            QCOMPARE(r[k].x.size(), in[k].x.size());
            for (size_t i = 0; i < in[k].x.size(); ++i) {
                QCOMPARE(r[k].x[i], as4(in[k].x[i]));        // exactly what was written (4 decimals)
                QCOMPARE(r[k].y[i], as4(in[k].y[i]));
            }
            for (const auto& kv : in[k].meta) QCOMPARE(r[k].metaValue(kv.first), kv.second);
            QCOMPARE(r[k].metaValue("started"), QString("2026-11-20T18:21:00+01:00"));   // from the LINK block
            QCOMPARE(r[k].metaValue("BLOCK_ID"), QString::number(k + 1));
            QCOMPARE(r[k].metaValue("DATA TYPE"), QString("RAMAN SPECTRUM"));
        }
        // the elapsed times reach the Analysis page (chemometrics reads series_elapsed_s)
        QCOMPARE(r[2].metaValue("series_elapsed_s"), QString("5"));
    }

    void jcampMultiBlockParse() {
        // a compound file as other programs write it: XYDATA (X++(Y..Y)) and XYPOINTS blocks, a block without
        // data (ignored), labels in mixed case
        QTemporaryDir dir;
        const QString path = dir.filePath("other.jdx");
        writeFile(path, "##TITLE= kinetics\r\n##JCAMP-DX= 5.01\r\n##DATA TYPE= LINK\r\n##BLOCKS= 3\r\n"
                        "##$Operator= someone\r\n"
                        "##TITLE= first\r\n##JCAMP-DX= 5.01\r\n##BLOCK_ID= 1\r\n##XUNITS= 1/CM\r\n##FIRSTX= 100\r\n"
                        "##LASTX= 400\r\n##NPOINTS= 4\r\n##YFACTOR= 2\r\n##XYDATA= (X++(Y..Y))\r\n100 1 2\r\n300 3 4\r\n"
                        "##END=\r\n"
                        "##TITLE= notes only\r\n##BLOCK_ID= 2\r\n##END=\r\n"
                        "##TITLE= second\r\n##BLOCK_ID= 3\r\n##$Time= t=12.5s\r\n##XYPOINTS= (XY..XY)\r\n"
                        "500, 7; 450, 6\r\n400, 5\r\n##END=\r\n##END=\r\n");
        const auto r = fileio::read(path);
        QCOMPARE(r.size(), size_t(2));
        QCOMPARE(r[0].name, QString("first"));
        QCOMPARE(r[0].x, (Vec{100, 200, 300, 400}));
        QCOMPARE(r[0].y, (Vec{2, 4, 6, 8}));
        QCOMPARE(r[0].metaValue("operator"), QString("someone"));
        QCOMPARE(r[1].name, QString("second"));
        QCOMPARE(r[1].x, (Vec{400, 450, 500}));
        QCOMPARE(r[1].y, (Vec{5, 6, 7}));
        QCOMPARE(r[1].metaValue("time"), QString("t=12.5s"));
        QCOMPARE(r[1].metaValue("BLOCK_ID"), QString("3"));
        // a LINK file without spectra
        writeFile(path, "##TITLE=x\n##DATA TYPE=LINK\n##BLOCKS=0\n##END=\n");
        QVERIFY(golden::throws<Error>([&] { fileio::read(path); }));
        // single-block files are read as before (golden files), also with ##BLOCK_ID but no ##BLOCKS
        const auto one = fileio::read(QDir(filesDir()).filePath("points.jdx"));
        QCOMPARE(one.size(), size_t(1));
    }

    void spcMultiLayout() {
        QTemporaryDir dir;
        const auto in = series3(50);
        const QString path = dir.filePath("series.spc");
        fileio::writeSpcMulti(path, in, "CCl4 time series", {{"sample", "CCl4"}, {"exposure_s", "0.5"}});
        const QByteArray b = contents(path);
        const int npts = 50, n = 3;
        const qsizetype data = 512 + 4 * npts + n * (32 + 4 * npts);       // header, x array, sub-files
        const QByteArray logText = "sample=CCl4\r\nexposure_s=0.5\r\n";
        QCOMPARE(b.size(), data + 64 + logText.size() + 1);
        // 512-byte header (Galactic SPC new format, field offsets of the format description)
        QCOMPARE(uchar(b[0]), uchar(0x80 | 0x10 | 0x04));   // ftflgs: TXVALS | TORDRD | TMULTI
        QCOMPARE(uchar(b[1]), uchar(0x4B));                 // fversn
        QCOMPARE(uchar(b[2]), uchar(11));                   // fexper: Raman
        QCOMPARE(uchar(b[3]), uchar(0x80));                 // fexp: float32
        QCOMPARE(u32(b, 4), quint32(npts));                 // fnpts
        QCOMPARE(f64(b, 8), in[0].x.front());               // ffirst
        QCOMPARE(f64(b, 16), in[0].x.back());               // flast
        QCOMPARE(u32(b, 24), quint32(n));                   // fnsub
        QCOMPARE(uchar(b[28]), uchar(13));                  // fxtype: Raman shift
        QCOMPARE(uchar(b[29]), uchar(4));                   // fytype: counts
        QCOMPARE(uchar(b[30]), uchar(4));                   // fztype: seconds
        QCOMPARE(u32(b, 32), quint32(2026u << 20 | 11u << 16 | 20u << 11 | 18u << 6 | 21u));   // fdate
        QCOMPARE(b.mid(88, 30), QByteArray("CCl4 time series, LibreRamen") + QByteArray(2, '\0'));   // fcmnt
        QCOMPARE(u32(b, 248), quint32(data));               // flogoff
        QCOMPARE(f32(b, 312), 2.5);                         // fzinc
        for (int i = 0; i < npts; ++i) QCOMPARE(f32(b, 512 + 4 * i), asFloat(in[0].x[size_t(i)]));
        for (int k = 0; k < n; ++k) {                       // 32-byte sub-file headers, then y
            const qsizetype o = 512 + 4 * npts + k * (32 + 4 * npts);
            QCOMPARE(uchar(b[o]), uchar(0));                // subflgs
            QCOMPARE(uchar(b[o + 1]), uchar(0x80));         // subexp
            QCOMPARE(qFromLittleEndian<quint16>(reinterpret_cast<const uchar*>(b.constData()) + o + 2), quint16(k));
            QCOMPARE(f32(b, o + 4), 2.5 * k);               // subtime = z
            QCOMPARE(f32(b, o + 8), 2.5 * std::min(k + 1, n - 1));   // subnext
            QCOMPARE(u32(b, o + 16), quint32(0));           // subnpts (only for XYXYS)
            for (int i = 0; i < npts; ++i) QCOMPARE(f32(b, o + 32 + 4 * i), asFloat(in[size_t(k)].y[size_t(i)]));
        }
        QCOMPARE(u32(b, data), quint32(64 + logText.size() + 1));   // log block: logsizd
        QCOMPARE(u32(b, data + 4), quint32(4096));          // logsizm
        QCOMPARE(u32(b, data + 8), quint32(64));            // logtxto
        QCOMPARE(b.mid(data + 64), logText + QByteArray(1, '\0'));

        const auto r = fileio::read(path);
        QCOMPARE(r.size(), size_t(3));
        for (size_t k = 0; k < 3; ++k) {
            QCOMPARE(r[k].name, QString("series t=%1s").arg(2.5 * double(k)));
            QCOMPARE(r[k].metaValue("series_elapsed_s"), QString::number(2.5 * double(k)));
            QCOMPARE(r[k].metaValue("z"), QString::number(2.5 * double(k)));
            QCOMPARE(r[k].metaValue("sample"), QString("CCl4"));            // from the log block
            QCOMPARE(r[k].metaValue("exposure_s"), QString("0.5"));
            for (size_t i = 0; i < size_t(npts); ++i) {
                QCOMPARE(r[k].x[i], asFloat(in[k].x[i]));
                QCOMPARE(r[k].y[i], asFloat(in[k].y[i]));
            }
        }
    }

    void spcMultiXyxys() {
        // different x axes: TXYXYS (x array in every sub-file); no elapsed times: z = index
        QTemporaryDir dir;
        auto in = series3(30);
        in[1].x.resize(20);
        in[1].y.resize(20);
        for (auto& d : in) {
            d.meta = {{"processing", "baseline snip"}};
        }
        const QString path = dir.filePath("mixed.spc");
        fileio::writeSpcMulti(path, in);
        const QByteArray b = contents(path);
        QCOMPARE(uchar(b[0]), uchar(0x80 | 0x40 | 0x04));   // TXVALS | TXYXYS | TMULTI
        QCOMPARE(u32(b, 4), quint32(0));                    // fnpts: no directory
        QCOMPARE(uchar(b[29]), uchar(0));                   // fytype: arbitrary (processed)
        QCOMPARE(uchar(b[30]), uchar(0));                   // fztype: arbitrary
        QCOMPARE(u32(b, 248), quint32(0));                  // no log block
        QCOMPARE(b.size(), qsizetype(512 + 3 * 32 + 8 * (30 + 20 + 30)));
        QCOMPARE(u32(b, 512 + 16), quint32(30));            // subnpts
        QCOMPARE(u32(b, 512 + 32 + 8 * 30 + 16), quint32(20));
        const auto r = fileio::read(path);
        QCOMPARE(r.size(), size_t(3));
        QCOMPARE(r[1].x.size(), size_t(20));
        for (size_t i = 0; i < 20; ++i) QCOMPARE(r[1].y[i], asFloat(in[1].y[i]));
        QCOMPARE(r[2].metaValue("z"), QString("2"));
        QCOMPARE(r[2].metaValue("series_elapsed_s"), QString());
        QCOMPARE(r[2].name, QString("mixed #3"));
        // JCAMP: processed data are ARBITRARY UNITS
        fileio::writeJcampMulti(dir.filePath("mixed.jdx"), in);
        QVERIFY(contents(dir.filePath("mixed.jdx")).contains("##YUNITS=ARBITRARY UNITS\n"));
        QCOMPARE(fileio::read(dir.filePath("mixed.jdx")).size(), size_t(3));
        // nothing to write
        QVERIFY(golden::throws<Error>([&] { fileio::writeSpcMulti(dir.filePath("none.spc"), {}); }));
        QVERIFY(golden::throws<Error>([&] { fileio::writeJcampMulti(dir.filePath("none.jdx"), {fileio::DataSpectrum{}}); }));
    }

    void spcZValues() {
        // the golden multi-file SPC has no z values (meta as in Python); the same file with an even z axis in
        // minutes (fzinc, first sub-file's subtime) gives elapsed seconds
        QByteArray b = contents(QDir(filesDir()).filePath("multi.spc"));
        QVERIFY(!b.isEmpty());
        const auto plain = fileio::read(QDir(filesDir()).filePath("multi.spc"));
        QCOMPARE(plain[1].metaValue("z"), QString());
        auto* u = reinterpret_cast<uchar*>(b.data());
        u[30] = 5;                                          // fztype: minutes
        const float inc = 0.5f, z0 = 2.0f;
        quint32 w;
        std::memcpy(&w, &inc, 4);
        qToLittleEndian<quint32>(w, u + 312);
        std::memcpy(&w, &z0, 4);
        qToLittleEndian<quint32>(w, u + 512 + 4 * 50 + 4);  // subtime of the first sub-file (after the x array)
        QTemporaryDir dir;
        writeFile(dir.filePath("z.spc"), b);
        const auto r = fileio::read(dir.filePath("z.spc"));
        QCOMPARE(r.size(), size_t(2));
        QCOMPARE(r[0].metaValue("z"), QString("2"));
        QCOMPARE(r[1].metaValue("z"), QString("2.5"));
        QCOMPARE(r[1].metaValue("series_elapsed_s"), QString("150"));
        QCOMPARE(r[1].name, QString("z t=150s"));
        QCOMPARE(r[1].y, plain[1].y);
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
