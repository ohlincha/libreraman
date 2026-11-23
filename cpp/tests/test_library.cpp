// Spectral library (library.h) and matching (match.h): schema and versioning, CRUD, import/export formats
// (incl. a synthetic RRUFF file and band lists), full-spectrum HQI search, peak search, NNLS and mixtures.
#include <QSqlDatabase>
#include <QSqlError>
#include <QSqlQuery>
#include <QTemporaryDir>
#include <QtTest>

#include <cmath>
#include <random>

#include "core/errors.h"
#include "core/fileio.h"
#include "core/library.h"
#include "core/match.h"
#include "core/peaks.h"
#include "core/processing.h"
#include "core/simulator.h"

using namespace lr;
using namespace lr::library;

namespace {

void writeText(const QString& path, const QString& text) {
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly | QIODevice::Text));
    f.write(text.toUtf8());
}

// Raw SQL on a library file through a separate connection: the first value of a query, 0 for statements
// without result, -1 on errors.
int sqlInt(const QString& path, const QString& sql) {
    int v = -1;
    {
        QSqlDatabase db = QSqlDatabase::addDatabase("QSQLITE", "test-raw");
        db.setDatabaseName(path);
        if (db.open()) {
            QSqlQuery q(db);
            if (q.exec(sql)) v = q.next() ? q.value(0).toInt() : 0;
            db.close();
        }
    }
    QSqlDatabase::removeDatabase("test-raw");
    return v;
}

// Synthetic Raman-like spectrum: Lorentzian bands on a sloping background.
void bandsSpectrum(const std::vector<std::pair<double, double>>& bands, Vec& x, Vec& y, double noise = 0,
                   unsigned seed = 1) {
    std::mt19937 rng(seed);
    std::normal_distribution<double> nd(0, 1);
    x.clear();
    y.clear();
    for (double s = 100; s <= 3200; s += 1.5) {
        double v = 50 + 0.02 * s;
        for (const auto& b : bands) v += b.second / (1 + std::pow((s - b.first) / 6.0, 2));
        x.push_back(s);
        y.push_back(v + noise * nd(rng));
    }
}

std::unique_ptr<SimCora> simulator() {
    auto s = std::make_unique<SimCora>(defaultCalibration(), [](const QString&) {}, 1000.0, 7);
    s->connect();
    s->initialize();
    s->pressArm();
    return s;
}

const char* kRruff =
    "##NAMES=Quartz\n"
    "##RRUFFID=R040031\n"
    "##IDEAL CHEMISTRY=SiO_2_\n"
    "##LOCALITY=Hot Springs, Arkansas, USA\n"
    "##OWNER=RRUFF\n"
    "##SOURCE=Synthetic test file\n"
    "##DESCRIPTION=Colourless crystal\n"
    "##STATUS=The identification of this mineral has been confirmed by X-ray diffraction.\n"
    "##URL=rruff.info/quartz/R040031\n"
    "##PIN_ID=\n"
    "##ORIENTATION=unoriented\n";

QString rruffText(double band, double extra = 0) {
    QString t = QString::fromLatin1(kRruff);
    for (double s = 100; s <= 1400; s += 1.0) {
        const double v = 1000.0 / (1 + std::pow((s - band) / 4.0, 2)) + 300.0 / (1 + std::pow((s - 206) / 6.0, 2)) +
                         (extra > 0 ? 400.0 / (1 + std::pow((s - extra) / 5.0, 2)) : 0.0) + 10;
        t += QString("%1, %2\n").arg(s, 0, 'f', 2).arg(v, 0, 'f', 4);
    }
    t += "##END=\n";
    return t;
}

}  // namespace

class TestLibrary : public QObject {
    Q_OBJECT
    QTemporaryDir tmp;

    QString file(const QString& name) const { return tmp.filePath(name); }

private slots:
    void initTestCase() { QVERIFY(tmp.isValid()); }

    void sources() {
        for (Source s : allSources()) {
            QCOMPARE(sourceFromKey(sourceKey(s)), s);
            QCOMPARE(sourceFromKey(sourceLabel(s)), s);
        }
        QCOMPARE(sourceFromKey("RRUFF"), Source::Rruff);
        QVERIFY_THROWS_EXCEPTION(Error, sourceFromKey("internet"));
    }

    void blobs() {
        const Vec v = {0.0, -1.5, 3.14159265358979, 1e-300, 1e300, std::nan("")};
        const QByteArray b = toBlob(v);
        QCOMPARE(b.size(), 48);
        QCOMPARE(quint8(b[8 + 7]), quint8(0xBF));      // -1.5 little-endian: sign/exponent in the last byte
        const Vec w = fromBlob(b);
        QCOMPARE(w.size(), v.size());
        for (size_t i = 0; i + 1 < v.size(); ++i) QCOMPARE(w[i], v[i]);
        QVERIFY(std::isnan(w.back()));
    }

    void schema() {
        const QString p = file("schema/lib.sqlite");
        {
            Library lib;
            QVERIFY(!lib.isOpen());
            QVERIFY_THROWS_EXCEPTION(Error, lib.count());
            lib.open(p);
            QVERIFY(lib.isOpen());
            QVERIFY(!lib.readOnly());
            QCOMPARE(lib.schemaVersion(), Library::kSchemaVersion);
            QCOMPARE(lib.info("format"), QString("LibreRamen spectral library"));
            QVERIFY(lib.info("schema_history").startsWith("0->1"));
            QVERIFY(!lib.onNetwork());
            QCOMPARE(lib.journalMode(), QString("wal"));
            QCOMPARE(lib.count(), 0);
            lib.setInfo("owner", "lab 3");
        }
        QCOMPARE(sqlInt(p, "PRAGMA user_version"), 1);
        {
            Library lib;
            lib.open(p, OpenMode::ReadOnly);       // close() left the file in rollback-journal mode
            QVERIFY(lib.readOnly());
            QCOMPARE(lib.journalMode(), QString("delete"));
            QCOMPARE(lib.info("owner"), QString("lab 3"));
            Entry e;
            e.name = "x";
            QVERIFY_THROWS_EXCEPTION(Error, lib.add(e));
            QVERIFY_THROWS_EXCEPTION(Error, lib.setInfo("a", "b"));
        }
        // missing files
        Library lib;
        QVERIFY_THROWS_EXCEPTION(Error, lib.open(file("missing.sqlite"), OpenMode::ReadOnly));
        QVERIFY_THROWS_EXCEPTION(Error, lib.open(file("missing.sqlite"), OpenMode::ReadWrite, false));
        QVERIFY(!QFile::exists(file("missing.sqlite")));
        // newer schema
        const QString newer = file("newer.sqlite");
        QVERIFY(QFile::copy(p, newer));
        QCOMPARE(sqlInt(newer, "PRAGMA user_version = 99"), 0);
        try {
            lib.open(newer);
            QFAIL("opened a newer library");
        } catch (const Error& e) {
            QVERIFY2(QString(e.what()).contains("newer LibreRamen"), e.what());
        }
        QVERIFY(!lib.isOpen());
        // another SQLite database and a text file
        const QString other = file("other.sqlite");
        QCOMPARE(sqlInt(other, "CREATE TABLE t (a INTEGER)"), 0);
        try {
            lib.open(other);
            QFAIL("opened a foreign database");
        } catch (const Error& e) {
            QVERIFY2(QString(e.what()).contains("not a LibreRamen library"), e.what());
        }
        writeText(file("text.sqlite"), "this is not a database, just some text that is long enough............\n");
        QVERIFY_THROWS_EXCEPTION(Error, lib.open(file("text.sqlite")));
        // an empty (version 0) file is migrated, but not when opened read-only
        writeText(file("empty.sqlite"), "");
        QVERIFY_THROWS_EXCEPTION(Error, lib.open(file("empty.sqlite"), OpenMode::ReadOnly));
        lib.open(file("empty.sqlite"));
        QCOMPARE(lib.schemaVersion(), 1);
        lib.close();
        QVERIFY(!lib.isOpen());
        // network detection
        QVERIFY(Library::isNetworkPath("//server/share/lib.sqlite"));
        QVERIFY(!Library::isNetworkPath(p));
    }

    void crud() {
        Library lib;
        lib.open(file("crud.sqlite"));
        Entry e;
        e.name = "Polystyrene film";
        e.formula = "(C8H8)n";
        e.cas = "9003-53-6";
        e.cls = "polymer";
        e.tags = QStringList{"reference", "film"};
        e.source = Source::Own;
        e.licence = "own data";
        e.laserNm = 532.1;
        e.conditions = "1 s x 10";
        e.op = "AO";
        e.notes = "petri dish lid";
        e.provenance.insert("instrument", "Cora 5000");
        Vec x, y;
        bandsSpectrum({{620, 300}, {1001, 2000}, {1031, 600}, {1602, 900}, {3054, 1200}}, x, y);
        e.spectra.push_back({0, "raw", false, x, y});
        Vec xr(x.rbegin(), x.rend()), yr(y.rbegin(), y.rend());      // stored ascending
        e.spectra.push_back({0, "processed", true, xr, yr});
        e.peaks = {{1001.4, 1.0, 6, "ring breathing"}, {1602, 0.45, 8, {}}};
        const qint64 id = lib.add(e);
        QVERIFY(id > 0);
        QCOMPARE(lib.count(), 1);

        auto g = lib.get(id);
        QVERIFY(g.has_value());
        QCOMPARE(g->name, e.name);
        QCOMPARE(g->formula, e.formula);
        QCOMPARE(g->cas, e.cas);
        QCOMPARE(g->cls, e.cls);
        QCOMPARE(g->tags, e.tags);
        QCOMPARE(g->source, Source::Own);
        QCOMPARE(g->laserNm, 532.1);
        QCOMPARE(g->op, QString("AO"));
        QCOMPARE(g->provenance.value("instrument").toString(), QString("Cora 5000"));
        QVERIFY(g->created.isValid());
        QCOMPARE(g->spectrumCount, 2);
        QCOMPARE(g->peakCount, 2);
        QCOMPARE(int(g->spectra.size()), 2);
        QCOMPARE(g->spectra[0].x, x);
        QCOMPARE(g->spectra[0].y, y);
        QCOMPARE(g->spectra[1].x, x);               // sorted on storing
        QVERIFY(g->spectra[1].processed);
        QCOMPARE(g->preferredSpectrum()->label, QString("processed"));
        QCOMPARE(g->peaks[0].label, QString("ring breathing"));
        QVERIFY(!lib.get(id + 100).has_value());
        QVERIFY(lib.get(id, false)->spectra.empty());

        // update metadata only
        g->name = "Polystyrene (PS)";
        g->spectra.clear();
        lib.update(*g, true);
        auto g2 = lib.get(id);
        QCOMPARE(g2->name, QString("Polystyrene (PS)"));
        QCOMPARE(int(g2->spectra.size()), 2);
        QVERIFY(g2->modified >= g2->created);
        // update replacing data
        g2->spectra.resize(1);
        g2->peaks.clear();
        lib.update(*g2);
        QCOMPARE(lib.get(id)->spectrumCount, 1);
        QCOMPARE(lib.get(id)->peakCount, 0);
        Entry ghost = *g2;
        ghost.id = 9999;
        QVERIFY_THROWS_EXCEPTION(Error, lib.update(ghost));

        // more entries for filters
        auto mk = [&](const QString& name, const QString& cls, Source s, double laser) {
            Entry n;
            n.name = name;
            n.cls = cls;
            n.source = s;
            n.laserNm = laser;
            n.peaks = {{500, 1, 0, {}}};
            return lib.add(n);
        };
        mk("Calcite", "mineral", Source::Rruff, 532);
        mk("Calcite 785", "mineral", Source::Rruff, 785);
        mk("Aspirin (computed)", "drug", Source::Computed, 0);
        mk("Toluene", "solvent", Source::Literature, 0);
        QCOMPARE(lib.count(), 5);
        QCOMPARE(lib.classes(), QStringList({"drug", "mineral", "polymer", "solvent"}));
        Filter f;
        f.text = "calc";
        QCOMPARE(int(lib.find(f).size()), 2);
        f = {};
        f.cls = "MINERAL";
        QCOMPARE(int(lib.find(f).size()), 2);
        f = {};
        f.source = Source::Computed;
        QCOMPARE(lib.find(f).at(0).name, QString("Aspirin (computed)"));
        f = {};
        f.laserNm = 532;
        QCOMPARE(int(lib.find(f).size()), 4);       // 532.1, 532 and two without laser
        f.includeUnknownLaser = false;
        QCOMPARE(int(lib.find(f).size()), 2);
        f = {};
        f.spectraOnly = true;
        QCOMPARE(int(lib.find(f).size()), 1);
        f = {};
        f.text = "50%_";                            // LIKE wildcards are escaped
        QCOMPARE(int(lib.find(f).size()), 0);
        f = {};
        f.limit = 2;
        const auto two = lib.find(f, true);
        QCOMPARE(int(two.size()), 2);
        QCOMPARE(two[0].name, QString("Aspirin (computed)"));   // sorted by name
        QCOMPARE(int(two[0].peaks.size()), 1);
        const auto all = lib.find({}, true);
        QCOMPARE(all.back().name, QString("Toluene"));
        for (const auto& a : all)
            if (a.id == id) QCOMPARE(int(a.spectra.size()), 1);

        // delete cascades to spectra and peaks
        lib.remove(id);
        QCOMPARE(lib.count(), 4);
        const QString path = lib.path();
        lib.close();
        QCOMPARE(sqlInt(path, "SELECT COUNT(*) FROM spectra"), 0);
        QCOMPARE(sqlInt(path, "SELECT COUNT(*) FROM peaks WHERE entry_id = " + QString::number(id)), 0);
    }

    void addSpectrum() {
        Library lib;
        lib.open(file("own.sqlite"));
        auto sim = simulator();
        AcquireOptions o;
        o.exposure_s = 0.5;
        Spectrum sp = sim->acquire(o);
        sp.setMeta("sample", "ethanol 96%");
        sp.setMeta("operator", "Ann");
        const qint64 id = lib.addSpectrum(sp);
        auto e = lib.get(id);
        QCOMPARE(e->name, QString("ethanol 96%"));
        QCOMPARE(e->source, Source::Own);
        QCOMPARE(e->licence, QString("own data"));
        QVERIFY(std::abs(e->laserNm - 532.1) < 1);
        QCOMPARE(e->op, QString("Ann"));
        QVERIFY2(e->conditions.contains("exposure 0.5 s"), qPrintable(e->conditions));
        QVERIFY(e->provenance.contains("metadata"));
        QVERIFY(e->provenance.contains("measured"));
        QCOMPARE(int(e->spectra.size()), 1);
        QCOMPARE(e->spectra[0].x, sp.shift);
        QVERIFY(!e->spectra[0].processed);
        QVERIFY(e->peaks.size() >= 5);                     // ethanol: 433, 882, 1052, 1096, 1454, 2876, 2929 ...
        bool c882 = false;
        for (const auto& p : e->peaks) c882 = c882 || std::abs(p.position - 882) < 6;
        QVERIFY(c882);
        // given metadata wins over the spectrum's
        Entry meta;
        meta.name = "EtOH";
        meta.formula = "C2H6O";
        meta.op = "Bo";
        const qint64 id2 = lib.addSpectrum(sp, meta);
        QCOMPARE(lib.get(id2)->name, QString("EtOH"));
        QCOMPARE(lib.get(id2)->op, QString("Bo"));
        fileio::DataSpectrum empty;
        QVERIFY_THROWS_EXCEPTION(Error, lib.addSpectrum(empty));
    }

    void rruff() {
        const QString dir = file("rruff");
        QDir().mkpath(dir);
        const QString p1 = dir + "/Quartz__R040031-3__Raman__532__0__unoriented__Raman_Data_Processed__1234.txt";
        writeText(p1, rruffText(464));
        writeText(dir + "/Calcite__R040070-1__Raman__780__0__unoriented__Raman_Data_RAW__5678.txt",
                  QString(rruffText(1086, 712)).replace("Quartz", "Calcite").replace("SiO_2_", "CaCO_3_")
                      .replace("R040031", "R040070"));
        writeText(dir + "/broken.txt", "##NAMES=Nothing\n##END=\n");
        writeText(dir + "/notes.txt", "some notes\n");
        QVERIFY(isRruffFile(p1));
        QVERIFY(!isRruffFile(dir + "/notes.txt"));
        const RruffSpectrum r = readRruff(p1);
        QCOMPARE(r.name, QString("Quartz"));
        QCOMPARE(r.rruffId, QString("R040031"));
        QCOMPARE(r.formula, QString("SiO2"));
        QCOMPARE(r.locality, QString("Hot Springs, Arkansas, USA"));
        QCOMPARE(r.laserNm, 532.0);
        QVERIFY(r.processed);
        QCOMPARE(int(r.x.size()), 1301);
        QCOMPARE(r.x.front(), 100.0);
        QVERIFY(std::abs(r.y[364] - (1000.0 + 300.0 / (1 + std::pow((464 - 206) / 6.0, 2)) + 10)) < 1e-4);
        QCOMPARE(r.header.front().first, QString("NAMES"));
        QVERIFY_THROWS_EXCEPTION(Error, readRruff(dir + "/broken.txt"));

        const Entry e = entryFromRruff(r);
        QCOMPARE(e.source, Source::Rruff);
        QCOMPARE(e.sourceId, QString("R040031"));
        QVERIFY(e.licence.contains("RRUFF"));
        QCOMPARE(e.cls, QString("mineral"));
        QVERIFY(e.notes.contains("Hot Springs"));
        QCOMPARE(e.provenance.value("url").toString(), QString("rruff.info/quartz/R040031"));
        QVERIFY(e.spectra.at(0).processed);
        bool q464 = false;
        for (const auto& p : e.peaks) q464 = q464 || std::abs(p.position - 464) < 1;
        QVERIFY(q464);

        Library lib;
        lib.open(file("rruff.sqlite"));
        QStringList errors;
        const auto ids = lib.importRruffFolder(dir, &errors);
        QCOMPARE(int(ids.size()), 2);
        QCOMPARE(errors.size(), 2);
        const auto calcite = lib.get(ids[0]);   // sorted by file name: Calcite first
        QCOMPARE(calcite->name, QString("Calcite"));
        QCOMPARE(calcite->formula, QString("CaCO3"));
        QCOMPARE(calcite->laserNm, 780.0);
        QVERIFY(!calcite->spectra[0].processed);
        // importFile recognises RRUFF too
        const auto one = lib.importFile(p1);
        QCOMPARE(lib.get(one.at(0))->source, Source::Rruff);
        QVERIFY_THROWS_EXCEPTION(Error, lib.importRruffFolder(dir + "/nope"));
    }

    void bandLists() {
        const QString p = file("bands.csv");
        writeText(p, "# class: solvent\n"
                     "# source: Smith, J. Raman Spectrosc. (2001)\n"
                     "# licence: factual data\n"
                     "name,bands\n"
                     "Toluene,521;786:s;1003:vs;1030:m;1210:0.2;3057\n"
                     "\"Acetone, dry\",\"787, 1710, 2925\"\n"
                     "# class: mineral\n"
                     "Calcite,156 282 712 1086:s\n");
        const auto lists = readBandList(p);
        QCOMPARE(int(lists.size()), 3);
        QCOMPARE(lists[0].name, QString("Toluene"));
        QCOMPARE(lists[0].cls, QString("solvent"));
        QCOMPARE(lists[0].reference, QString("Smith, J. Raman Spectrosc. (2001)"));
        QCOMPARE(int(lists[0].bands.size()), 6);
        QCOMPARE(lists[0].bands[1].intensity, 1.0);
        QCOMPARE(lists[0].bands[3].intensity, 0.5);
        QCOMPARE(lists[0].bands[4].intensity, 0.2);
        QCOMPARE(lists[0].bands[0].intensity, 0.0);
        QCOMPARE(lists[1].name, QString("Acetone, dry"));
        QCOMPARE(int(lists[1].bands.size()), 3);
        QCOMPARE(lists[2].cls, QString("mineral"));
        QCOMPARE(lists[2].bands.back().position, 1086.0);

        const QString p2 = file("bands2.csv");
        writeBandList(p2, lists);
        const auto back = readBandList(p2);
        QCOMPARE(int(back.size()), 3);
        QCOMPARE(back[1].name, lists[1].name);
        QCOMPARE(back[0].bands[4].intensity, 0.2);
        QCOMPARE(back[2].cls, QString("mineral"));
        QCOMPARE(back[0].licence, QString("factual data"));

        writeText(file("bad.csv"), "Toluene,521;abc\n");
        QVERIFY_THROWS_EXCEPTION(Error, readBandList(file("bad.csv")));

        Library lib;
        lib.open(file("bands.sqlite"));
        const auto ids = lib.importBandList(p);
        QCOMPARE(int(ids.size()), 3);
        const auto t = lib.get(ids[0]);
        QCOMPARE(t->source, Source::Literature);
        QCOMPARE(t->peakCount, 6);
        QCOMPARE(t->spectrumCount, 0);
        QCOMPARE(t->licence, QString("factual data"));
        QVERIFY(t->preferredSpectrum() == nullptr);
        Entry comp;
        comp.source = Source::Computed;
        comp.notes = "B3LYP/6-31G*";
        const auto ids2 = lib.importBandList(p, comp);
        QCOMPARE(lib.get(ids2[0])->source, Source::Computed);
        QCOMPARE(lib.get(ids2[0])->notes, QString("B3LYP/6-31G*"));
        // export of an entry without spectrum: band list
        lib.exportEntry(ids[0], file("toluene-out.csv"));
        QCOMPARE(readBandList(file("toluene-out.csv")).at(0).bands.size(), size_t(6));
    }

    void importExport() {
        Library lib;
        lib.open(file("io.sqlite"));
        Vec x, y;
        bandsSpectrum({{1001, 2000}, {1602, 900}}, x, y, 2.0);
        fileio::DataSpectrum ds;
        ds.name = "PS film";
        ds.x = x;
        ds.y = y;
        ds.setMeta("sample", "PS film");
        ds.setMeta("laser_nm", "785");
        const QString csv = file("ps.csv");
        fileio::writeCsv(csv, ds);
        Entry defaults;
        defaults.cls = "polymer";
        defaults.op = "Cy";
        const auto ids = lib.importFile(csv, defaults);
        QCOMPARE(int(ids.size()), 1);
        auto e = lib.get(ids[0]);
        QCOMPARE(e->name, QString("PS film"));
        QCOMPARE(e->cls, QString("polymer"));
        QCOMPARE(e->op, QString("Cy"));
        QCOMPARE(e->laserNm, 785.0);
        QCOMPARE(e->sourceId, QString("ps.csv"));
        QVERIFY(e->provenance.contains("imported"));
        QCOMPARE(int(e->spectra[0].x.size()), int(x.size()));
        QVERIFY_THROWS_EXCEPTION(Error, lib.importFile(file("nothing.csv")));

        // export JCAMP-DX and CSV, read back with fileio
        e->licence = "CC BY 4.0";
        e->formula = "(C8H8)n";
        lib.update(*e, true);
        lib.exportEntry(ids[0], file("out.jdx"));
        lib.exportEntry(ids[0], file("out.csv"));
        for (const QString& f : {file("out.jdx"), file("out.csv")}) {
            const auto back = fileio::read(f);
            QCOMPARE(int(back.size()), 1);
            QCOMPARE(int(back[0].x.size()), int(x.size()));
            QVERIFY(std::abs(back[0].y[100] - y[100]) < 0.1);
        }
        const auto back = fileio::read(file("out.csv"));
        QCOMPARE(back[0].metaValue("licence"), QString("CC BY 4.0"));
        QCOMPARE(back[0].metaValue("library_source"), QString("Own"));
        QVERIFY_THROWS_EXCEPTION(Error, lib.exportEntry(12345, file("x.csv")));

        // copy to another library
        const QString other = file("copy/other.sqlite");
        QCOMPARE(lib.exportTo(ids, other), 1);
        QVERIFY_THROWS_EXCEPTION(Error, lib.exportTo(ids, lib.path()));
        Library lib2;
        lib2.open(other, OpenMode::ReadOnly);
        const auto copied = lib2.find({}, true);
        QCOMPARE(int(copied.size()), 1);
        QCOMPARE(copied[0].licence, QString("CC BY 4.0"));
        QCOMPARE(copied[0].spectra[0].y, e->spectra[0].y);
        QCOMPARE(copied[0].created, e->created);
        QVERIFY(copied[0].provenance.contains("copied_from"));
    }

    void demo() {
        Library lib;
        lib.open(file("demo.sqlite"));
        createDemo(lib);
        const auto own = lib.find([] {
            Filter f;
            f.source = Source::Own;
            return f;
        }());
        QCOMPARE(int(own.size()), 5);
        for (const auto& e : own) {
            QCOMPARE(e.spectrumCount, 1);
            QVERIFY(e.peakCount > 0);
            QVERIFY(e.licence.contains("MIT"));
        }
        QVERIFY(lib.count() > 20);                 // + standards as literature band lists
        const auto [x, y] = simulatorSpectrum("water");
        QCOMPARE(int(x.size()), 2048);
        QVERIFY(std::is_sorted(x.begin(), x.end()));
        QVERIFY_THROWS_EXCEPTION(Error, simulatorSpectrum("gold"));
    }

    // ---------------------------------------------------------------------------------------- matching
    void hqiBasics() {
        Vec x, y, y2;
        bandsSpectrum({{1001, 2000}, {1602, 900}}, x, y);
        const match::Prepared a = match::prepare(x, y);
        QVERIFY(!a.empty());
        QCOMPARE(a.xFirst(), 150.0);
        QCOMPARE(a.xLast(), 3198.0);                                          // data end at 3199
        QCOMPARE(a.y.size(), size_t(1525));
        QVERIFY(std::abs(match::hqi(a, a) - 1.0) < 1e-12);
        Vec ys = y;
        for (double& v : ys) v = 3 * v + 500;                                  // scale and offset
        QVERIFY(match::hqi(a, match::prepare(x, ys)) > 0.999);
        bandsSpectrum({{700, 2000}, {2200, 900}}, x, y2);
        QVERIFY(match::hqi(a, match::prepare(x, y2)) < 0.1);
        // overlap reported, partial ranges, no overlap
        Vec xa(x.begin(), x.begin() + 600), ya(y.begin(), y.begin() + 600);   // 100..998.5
        double lo = 0, hi = 0;
        match::hqi(a, match::prepare(xa, ya), &lo, &hi);
        QCOMPARE(lo, 150.0);
        QCOMPARE(hi, 998.0);
        match::Preprocess pp;
        pp.xmin = 2000;
        QCOMPARE(match::hqi(a, match::prepare(xa, ya, pp)), 0.0);
        QCOMPARE(match::hqi(Vec{1, 2, 3}, Vec{3, 2, 1}), 0.0);                // anticorrelated
        QCOMPARE(match::hqi(Vec{1, 2, 3}, Vec{2, 4, 6}), 1.0);
        // derivative preprocessing still matches itself
        pp = {};
        pp.derivative = 1;
        pp.smoothWindow = 7;
        const match::Prepared d = match::prepare(x, y, pp);
        QVERIFY(std::abs(match::hqi(d, d) - 1.0) < 1e-12);
    }

    // Issue #4: a library whose peak tables were picked automatically with an older default (classic SNIP up to
    // 0.2.4, nothing recorded; SNIP 4th order in 0.3.0/0.3.1, "snip4") is re-picked once with the current one
    // (arPLS 6); an edited table is kept.
    void upgradePeakTables_data() {
        QTest::addColumn<QString>("method");
        QTest::addColumn<QString>("marker");
        QTest::newRow("<= 0.2.4 (classic SNIP)") << "snip" << "";
        QTest::newRow("0.3.0/0.3.1 (snip4)") << "snip4" << "snip4";
    }
    void upgradePeakTables() {
        QFETCH(QString, method);
        QFETCH(QString, marker);
        auto older = [&method](const Vec& x, const Vec& y) {     // the old library::autoPeaks
            const Vec b = processing::baseline(x, y, method, 5.0);
            Vec yc(y.size());
            for (size_t i = 0; i < y.size(); ++i) yc[i] = y[i] - b[i];
            auto pk = peaks::pick(x, yc, 5.0, 2.0, false, 50.0);
            std::sort(pk.begin(), pk.end(), [](const auto& a, const auto& c) { return a.height > c.height; });
            if (pk.size() > 40) pk.resize(40);
            const double hmax = pk.empty() ? 1.0 : pk.front().height;
            std::vector<PeakEntry> out;
            for (const auto& q : pk) out.push_back({q.position, q.height / hmax, q.fwhm, {}});
            std::sort(out.begin(), out.end(), [](const auto& a, const auto& c) { return a.position < c.position; });
            return out;
        };
        const QString path = file("upgrade-" + method + ".sqlite");
        auto [x, y] = simulatorSpectrum("zinc sulfide");       // fluorescent: the two methods differ
        qint64 autoId = 0, editedId = 0;
        {
            Library lib;
            lib.open(path);
            QCOMPARE(lib.info("peak_baseline"), QStringLiteral("arpls:6"));   // new files are marked
            Entry e;
            e.name = "ZnS old";
            e.spectra.push_back({0, "raw", false, x, y});
            e.peaks = older(x, y);
            QVERIFY(e.peaks.size() > 1);
            autoId = lib.add(e);
            e.name = "ZnS edited";
            e.peaks = {{348.0, 1.0, 10.0, "ZnS LO"}};
            editedId = lib.add(e);
            lib.setInfo("peak_baseline", marker);               // as written by the older version
        }
        Library lib;
        lib.open(path);
        QCOMPARE(lib.info("peak_baseline"), QStringLiteral("arpls:6"));
        const auto now = lib.get(autoId)->peaks;
        const auto want = match::pickPeaks(x, y);
        QCOMPARE(now.size(), want.size());
        for (size_t i = 0; i < now.size(); ++i) QVERIFY(std::abs(now[i].position - want[i].position) < 1e-6);
        const auto edited = lib.get(editedId)->peaks;
        QCOMPARE(int(edited.size()), 1);
        QCOMPARE(edited[0].label, QStringLiteral("ZnS LO"));
    }

    // The shipped reference set: literature band lists with citations; added once (idempotent), recorded in
    // info, and found by peak search.
    void referenceEntries() {
        Library lib;
        lib.open(file("reference.sqlite"));
        const int n = addReferenceEntries(lib);
        QCOMPARE(n, referenceCount());
        QVERIFY(n >= 30);
        QCOMPARE(addReferenceEntries(lib), 0);
        QCOMPARE(lib.info("reference_version"), QString::number(referenceVersion()));
        Filter f;
        f.text = "LiFePO4";
        const auto found = lib.find(f, true);
        const auto it = std::find_if(found.begin(), found.end(), [](const Entry& e) { return e.formula == "LiFePO4"; });
        QVERIFY(it != found.end());
        const Entry& lfp = *it;
        QCOMPARE(lfp.source, Source::Literature);
        QVERIFY(lfp.tags.contains("reference"));
        QVERIFY(lfp.notes.contains("doi.org"));
        bool v1 = false;
        for (const auto& p : lfp.peaks) v1 = v1 || std::abs(p.position - 949) < 5;
        QVERIFY(v1);
        // a measured LFP peak list (main PO4 bands) finds LFP first
        match::Matcher m;
        m.load(lib);
        const std::vector<PeakEntry> q{{949, 1, 0, {}}, {995, 0.3, 0, {}}, {1068, 0.3, 0, {}}, {440, 0.1, 0, {}}};
        const auto hits = m.searchPeaks(q, {}, 3);
        QVERIFY(!hits.empty());
        QVERIFY2(hits[0].name.contains("LFP"), qPrintable(hits[0].name));
        // solvents: cyclohexane (ASTM E1840 bands) is found from its strongest lines
        const std::vector<PeakEntry> cy{{801.3, 0.95, 0, {}}, {2852.9, 1, 0, {}}, {2938.3, 0.67, 0, {}},
                                        {1028.3, 0.15, 0, {}}, {1266.4, 0.14, 0, {}}, {1444.4, 0.1, 0, {}}};
        const auto hc = m.searchPeaks(cy, {}, 3);
        QVERIFY(!hc.empty());
        QVERIFY2(hc[0].name.contains("Cyclohexane"), qPrintable(hc[0].name));
        // a library from an older version gets only the entries it lacks
        Library old;
        old.open(file("reference-old.sqlite"));
        addReferenceEntries(old);
        const auto all = old.find(Filter{}, false);
        old.remove(std::find_if(all.begin(), all.end(), [](const Entry& e) { return e.name == "Benzene"; })->id);
        old.setInfo("reference_version", "1");
        QCOMPARE(addReferenceEntries(old), 1);
    }

    void searchFluorescent() {
        Library lib;
        lib.open(file("fluor.sqlite"));
        createDemo(lib);
        match::Matcher m;
        QCOMPARE(m.preprocess().baseline, QStringLiteral("arpls"));
        m.load(lib);
        auto sim = simulator();
        for (const QString& sample : {QStringLiteral("carbon tetrachloride"), QStringLiteral("chloroform"),
                                      QStringLiteral("ethanol"), QStringLiteral("water")}) {
            sim->setSample(sample);
            AcquireOptions o;
            o.exposure_s = 0.5;
            const Spectrum sp = sim->acquire(o);
            Vec y = sp.intensity;
            const double amp = *std::max_element(y.begin(), y.end());
            for (size_t i = 0; i < y.size(); ++i) {
                const double u = (sp.shift[i] - 1800) / 1300;
                y[i] += amp * std::exp(-u * u);
            }
            const auto hits = m.search(sp.shift, y, 5);
            QVERIFY2(hits[0].name.toLower() == sample,
                     qPrintable(QString("%1 matched as %2 (%3)").arg(sample, hits[0].name).arg(hits[0].score)));
            QVERIFY2(hits[0].score > 0.8, qPrintable(QString("%1: HQI %2").arg(sample).arg(hits[0].score)));
        }
    }

    void searchSimulator() {
        Library lib;
        lib.open(file("match.sqlite"));
        createDemo(lib);
        match::Matcher m;
        m.load(lib);
        QVERIFY(m.size() > 20);
        auto sim = simulator();
        for (const QString& sample : sim->sampleNames()) {
            if (sample.startsWith(QStringLiteral("reaction:"))) continue;     // mixtures, not library entries
            // the stored spectrum itself
            const auto [x, y] = simulatorSpectrum(sample);
            auto hits = m.search(x, y, 5);
            QVERIFY(!hits.empty());
            QCOMPARE(hits[0].name.toLower(), sample);
            QVERIFY(hits[0].score > 0.9999);
            QCOMPARE(int(hits.size()), 5);           // only entries with spectra (5 demo spectra)
            // a fresh noisy measurement of the same sample
            sim->setSample(sample);
            AcquireOptions o;
            o.exposure_s = 0.5;
            const Spectrum sp = sim->acquire(o);
            hits = m.search(sp.shift, sp.intensity, 5);
            QCOMPARE(hits[0].name.toLower(), sample);
            // arPLS 6 (default) follows broad bands a little, so noisy water/ZnS score ~0.9, still far ahead
            QVERIFY2(hits[0].score > 0.8, qPrintable(QString("%1: HQI %2").arg(sample).arg(hits[0].score)));
            QVERIFY2(hits[1].score < hits[0].score - 0.1,
                     qPrintable(QString("%1: runner-up %2 %3").arg(sample, hits[1].name).arg(hits[1].score)));
            QVERIFY(hits[0].licence.contains("MIT"));
            QVERIFY(hits[0].hi - hits[0].lo > 2500);
        }
        // in-memory filters
        const auto [x, y] = simulatorSpectrum("ethanol");
        Filter f;
        f.cls = "inorganic";
        auto hits = m.search(x, y, 5, f);
        QCOMPARE(int(hits.size()), 1);
        QCOMPARE(hits[0].name, QString("Zinc sulfide"));
        f = {};
        f.laserNm = 785;
        f.includeUnknownLaser = false;
        QVERIFY(m.search(x, y, 5, f).empty());
        f = {};
        f.source = Source::Rruff;
        QVERIFY(m.search(x, y, 5, f).empty());
        // derivative preprocessing ranks the same
        match::Preprocess pp;
        pp.derivative = 1;
        pp.smoothWindow = 9;
        m.setPreprocess(pp);
        sim->setSample("chloroform");
        const Spectrum sp = sim->acquire(AcquireOptions{});
        hits = m.search(sp.shift, sp.intensity, 3);
        QCOMPARE(hits[0].name, QString("Chloroform"));
        QVERIFY(m.prepared(hits[0].id) != nullptr);
        QVERIFY(m.prepared(-1) == nullptr);
    }

    void peakMatching() {
        using library::PeakEntry;
        const std::vector<PeakEntry> ref = {{459, 1, 0, {}}, {314, 0.4, 0, {}}, {218, 0.3, 0, {}}, {790, 0.1, 0, {}}};
        auto s = match::peakScore(ref, ref, 8);
        QCOMPARE(s.score, 1.0);
        QCOMPARE(s.matched, 4);
        std::vector<PeakEntry> shifted = ref;
        for (auto& p : shifted) p.position += 4;
        QVERIFY(std::abs(match::peakScore(shifted, ref, 8).score - 0.75) < 1e-12);
        for (auto& p : shifted) p.position += 10;
        QCOMPARE(match::peakScore(shifted, ref, 8).score, 0.0);
        QCOMPARE(match::peakScore({}, ref, 8).score, 0.0);
        // one-to-one: two query peaks close to one library peak count once
        s = match::peakScore({{459, 1, 0, {}}, {461, 1, 0, {}}}, {{460, 1, 0, {}}}, 8);
        QCOMPARE(s.matched, 1);
        QVERIFY(std::abs(s.score - std::sqrt(0.5 * (1 - 0.5 / 8)) * std::sqrt(1 - 0.5 / 8)) < 1e-12);
        // library peaks outside the query range do not count
        s = match::peakScore({{459, 1, 0, {}}}, ref, 8, 400, 600);
        QCOMPARE(s.libraryPeaks, 1);
        QCOMPARE(s.score, 1.0);

        Library lib;
        lib.open(file("peaks.sqlite"));
        createDemo(lib);
        match::Matcher m;
        m.load(lib);
        auto sim = simulator();
        for (const QString& sample : {QString("carbon tetrachloride"), QString("ethanol"), QString("chloroform")}) {
            sim->setSample(sample);
            AcquireOptions o;
            o.exposure_s = 1;
            const Spectrum sp = sim->acquire(o);
            auto hits = m.searchPeaks(sp.shift, sp.intensity, {}, 5);
            QVERIFY(!hits.empty());
            QVERIFY2(hits[0].name.toLower() == sample, qPrintable(sample + " -> " + hits[0].name));
            QVERIFY(hits[0].matched >= 3);
            QVERIFY(hits[0].score > 0.6);
            // the literature band lists alone also find it
            Filter lit;
            lit.source = Source::Literature;
            hits = m.searchPeaks(sp.shift, sp.intensity, {}, 3, lit);
            QVERIFY2(hits[0].name.toLower() == sample, qPrintable(sample + " (literature) -> " + hits[0].name));
        }
    }

    void nnls() {
        std::mt19937 rng(3);
        std::uniform_real_distribution<double> u(0, 1);
        Eigen::MatrixXd A(50, 4);
        for (int i = 0; i < 50; ++i)
            for (int j = 0; j < 4; ++j) A(i, j) = u(rng);
        Eigen::VectorXd c(4);
        c << 0.5, 0.0, 2.0, 1.25;
        Eigen::VectorXd x = match::nnls(A, A * c);
        QVERIFY((x - c).norm() < 1e-9);
        // unconstrained solution has a negative coefficient: NNLS satisfies the KKT conditions
        Eigen::VectorXd c2(4);
        c2 << 1.0, -0.8, 0.5, 0.3;
        Eigen::VectorXd b = A * c2;
        x = match::nnls(A, b);
        QVERIFY(x.minCoeff() >= 0);
        const Eigen::VectorXd w = A.transpose() * (b - A * x);
        for (int j = 0; j < 4; ++j) {
            if (x(j) > 0) QVERIFY(std::abs(w(j)) < 1e-9);
            else QVERIFY(w(j) <= 1e-9);
        }
        // compare with brute force over all active sets
        double best = 1e300;
        for (int mask = 1; mask < 16; ++mask) {
            std::vector<int> cols;
            for (int j = 0; j < 4; ++j)
                if (mask & (1 << j)) cols.push_back(j);
            Eigen::MatrixXd As(50, Eigen::Index(cols.size()));
            for (size_t k = 0; k < cols.size(); ++k) As.col(Eigen::Index(k)) = A.col(cols[k]);
            const Eigen::VectorXd s = As.colPivHouseholderQr().solve(b);
            if (s.minCoeff() < 0) continue;
            best = std::min(best, (As * s - b).norm());
        }
        QVERIFY(std::abs((A * x - b).norm() - best) < 1e-9);
        QCOMPARE(match::nnls(A, -A * c).norm(), 0.0);      // all-negative target: zero
        QCOMPARE(match::nnls(Eigen::MatrixXd(0, 0), Eigen::VectorXd()).size(), Eigen::Index(0));
    }

    void mixture() {
        Library lib;
        lib.open(file("mix.sqlite"));
        createDemo(lib, false);
        match::Matcher m;
        m.load(lib);
        // 60 % ethanol + 40 % carbon tetrachloride, in units of the unit-norm prepared references
        const auto [x, ye] = simulatorSpectrum("ethanol");
        const auto [x2, yc] = simulatorSpectrum("carbon tetrachloride");
        QCOMPARE(x, x2);
        auto norm = [](const Vec& v) {
            double s = 0;
            for (double a : v) s += a * a;
            return std::sqrt(s);
        };
        const double ne = norm(match::prepare(x, ye).y), nc = norm(match::prepare(x, yc).y);
        std::mt19937 rng(11);
        std::normal_distribution<double> nd(0, 1);
        Vec y(x.size());
        double ymax = 0;
        for (size_t i = 0; i < x.size(); ++i) {
            y[i] = 0.6 * ye[i] / ne + 0.4 * yc[i] / nc;
            ymax = std::max(ymax, y[i]);
        }
        for (double& v : y) v += 0.01 * ymax * nd(rng);

        qint64 idE = 0, idC = 0;
        for (const auto& e : lib.find()) {
            if (e.name == "Ethanol") idE = e.id;
            if (e.name == "Carbon tetrachloride") idC = e.id;
        }
        QVERIFY(idE > 0 && idC > 0);
        const match::Mixture fit = m.fitMixture(x, y, {idC, idE});
        QCOMPARE(fit.components[0].id, idE);
        QVERIFY2(std::abs(fit.components[0].fraction - 0.6) < 0.03, qPrintable(QString::number(fit.components[0].fraction)));
        QVERIFY(std::abs(fit.components[1].fraction - 0.4) < 0.03);
        QVERIFY(fit.hqi > 0.98);
        QCOMPARE(fit.parts.size(), size_t(2));
        QCOMPARE(fit.x.size(), fit.fit.size());

        const auto mixes = m.searchMixtures(x, y, 5, 3, 5);
        QVERIFY(!mixes.empty());
        const match::Mixture& best = mixes[0];
        QCOMPARE(int(best.components.size()), 2);
        QCOMPARE(best.components[0].name, QString("Ethanol"));
        QCOMPARE(best.components[1].name, QString("Carbon tetrachloride"));
        QVERIFY(std::abs(best.components[0].fraction - 0.6) < 0.03);
        QVERIFY(best.hqi > mixes.back().hqi - 1e-12);
        // a pure spectrum stays a single component
        const auto pure = m.searchMixtures(x, ye, 5, 3, 3);
        QCOMPARE(int(pure[0].components.size()), 1);
        QCOMPARE(pure[0].components[0].name, QString("Ethanol"));
        QVERIFY_THROWS_EXCEPTION(Error, m.fitMixture(x, y, {424242}));
    }
};

QTEST_GUILESS_MAIN(TestLibrary)
#include "test_library.moc"
