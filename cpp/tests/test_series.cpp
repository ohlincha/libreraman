// Time series with the simulator: series.py (command line) and the GUI controller's series.
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QtTest>
#include <atomic>
#include <thread>

#include "core/errors.h"
#include "core/fileio.h"
#include "core/series.h"
#include "core/simulator.h"

using namespace lr;

namespace {
std::unique_ptr<SimCora> sim(double speed = 200.0, bool arm = true) {
    auto s = std::make_unique<SimCora>(defaultCalibration(), LogFn{}, speed, 7);
    s->connect();
    s->initialize();
    if (arm) s->pressArm();
    return s;
}

QStringList readLines(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text)) return {};
    return QString::fromUtf8(f.readAll()).split('\n', Qt::SkipEmptyParts);
}
}  // namespace

class TestSeries : public QObject {
    Q_OBJECT
private slots:
    void cliSeriesEach() {
        QTemporaryDir dir;
        auto s = sim();
        const QString path = dir.filePath("series.csv");
        std::vector<int> ks;
        const auto cols = runSeries(*s, path, 0.5, 0.05, 3, 2, "each", 0.0, nullptr,
                                    [&](int k, double e, const Spectrum& sp) {
                                        ks.push_back(k);
                                        QVERIFY(e >= 0);
                                        QCOMPARE(sp.averages, 2);
                                    });
        QCOMPARE(int(cols.size()), 3);
        QCOMPARE(ks, std::vector<int>({0, 1, 2}));
        QVERIFY(cols[1].elapsed_s > cols[0].elapsed_s);
        const QStringList l = readLines(path);
        QCOMPARE(l.size(), 7 + 2048);
        QVERIFY(l[0].startsWith("# Cora 5000 time series, started 20"));
        QCOMPARE(l.mid(1, 5), QStringList({"# channel: 532 nm", "# exposure_s: 0.5", "# averages: 2",
                                           "# interval_s: 0.05", "# background: each"}));
        QVERIFY(l[6].startsWith("raman_shift_cm1,t="));
        QCOMPARE(l[6].count(",t="), 3);
        QCOMPARE(l[7].split(',').size(), 4);
        QCOMPARE(l[7].split(',')[0], QString("-661.56"));
        QVERIFY(!s->laserOn());
    }

    void cliSeriesOnceAndStop() {
        QTemporaryDir dir;
        auto s = sim();
        const QString path = dir.filePath("once.csv");
        std::atomic<bool> stop{false};
        // interval longer than the test: the second spectrum is never reached
        const auto cols = runSeries(*s, path, 0.2, 100.0, 5, 1, "once", 0.0, &stop,
                                    [&](int, double, const Spectrum& sp) {
                                        for (size_t i = 0; i < sp.signal.size(); ++i)
                                            QCOMPARE(sp.intensity[i], sp.signal[i] - sp.background[i]);
                                        QVERIFY(sp.background[100] > 500);   // the dark measured once
                                        stop = true;
                                    });
        QCOMPARE(int(cols.size()), 1);
        const QStringList l = readLines(path);
        QCOMPARE(l[5], QString("# background: once"));
        QCOMPARE(l[6].count(",t="), 1);
    }

    void guiSeries() {
        QTemporaryDir dir;
        auto s = sim();
        s->setSample("water");
        SeriesSettings set;
        set.sample = "water / test";
        set.folder = dir.path();
        set.exposure_s = 0.5;
        set.interval_s = 0.1;
        set.count = 3;
        std::atomic<bool> stop{false};
        QStringList logs;
        std::vector<SeriesProgress> prog;
        int spectra = 0;
        SeriesCallbacks cb;
        cb.log = [&](const QString& m) { logs << m; };
        cb.progress = [&](const SeriesProgress& p) { prog.push_back(p); };
        cb.spectrum = [&](const Spectrum& sp) {
            ++spectra;
            QCOMPARE(sp.metaValue("sample"), QString("water / test"));
            QCOMPARE(sp.metaValue("series_index"), QString::number(spectra));
        };
        const QString folder = runTimeSeries(*s, set, stop, cb);
        QCOMPARE(spectra, 3);
        QVERIFY(QFileInfo(folder).fileName().startsWith("water___test_series_"));
        const QStringList files = QDir(folder).entryList({"*.csv"}, QDir::Files, QDir::Name);
        QCOMPARE(files.size(), 4);
        QVERIFY(files[0].startsWith("0001_"));
        QVERIFY(files[2].startsWith("0003_"));
        QCOMPARE(files[3], QString("series.csv"));
        const QStringList l = readLines(QDir(folder).filePath("series.csv"));
        QVERIFY(l[0].startsWith("# LibreRamen time series, started "));
        QCOMPARE(l.mid(1, 5), QStringList({"# sample: water / test", "# exposure_s: 0.5", "# averages: 1",
                                           "# interval_s: 0.1", "# background: each"}));
        QCOMPARE(l[6].count(",t="), 3);
        QCOMPARE(l.size(), 7 + 2048);
        const QStringList one = readLines(QDir(folder).filePath(files[0]));
        QVERIFY(one.contains("# sample: water / test"));
        QVERIFY(one.contains("# series_index: 1"));
        QVERIFY(prog.back().done);
        QCOMPARE(prog.back().n, 3);
        QCOMPARE(logs.last(), "Series finished: 3 spectra in " + folder);
    }

    void guiSeriesByDurationWithBackgroundOnce() {
        QTemporaryDir dir;
        auto s = sim();
        SeriesSettings set;
        set.folder = dir.path();
        set.exposure_s = 0.2;
        set.interval_s = 0.25;
        set.stopByCount = false;
        set.duration_s = 0.5;                                     // -> int(0.5 // 0.25) + 1 = 3 spectra
        set.background = "once";
        std::atomic<bool> stop{false};
        int n = 0, total = 0;
        SeriesCallbacks cb;
        cb.spectrum = [&](const Spectrum& sp) {
            ++n;
            for (size_t i = 0; i < sp.signal.size(); ++i) QCOMPARE(sp.intensity[i], sp.signal[i] - sp.background[i]);
        };
        cb.progress = [&](const SeriesProgress& p) { total = p.total; };
        runTimeSeries(*s, set, stop, cb);
        QCOMPARE(n, 3);
        QCOMPARE(total, 3);
    }

    void pausesWhileDisarmed() {
        QTemporaryDir dir;
        auto s = sim(200.0, /*arm=*/false);
        SeriesSettings set;
        set.folder = dir.path();
        set.exposure_s = 0.2;
        set.count = 1;
        std::atomic<bool> stop{false};
        QStringList logs;
        bool sawPaused = false;
        SeriesCallbacks cb;
        cb.log = [&](const QString& m) { logs << m; };
        cb.progress = [&](const SeriesProgress& p) { sawPaused = sawPaused || p.paused; };
        std::thread arm([&] {
            QThread::msleep(700);
            s->pressArm();
        });
        runTimeSeries(*s, set, stop, cb);
        arm.join();
        QVERIFY(sawPaused);
        QVERIFY(logs.contains("Paused: not armed (compartment opened?). Press ARM on the instrument to continue."));
        QVERIFY(logs.contains("Armed again: continuing"));
        QVERIFY(logs.last().startsWith("Series finished: 1 spectra in "));
    }

    void userPauseAndStop() {
        QTemporaryDir dir;
        auto s = sim();
        SeriesSettings set;
        set.folder = dir.path();
        set.exposure_s = 0.2;
        set.interval_s = 0.05;
        set.count = 100;
        std::atomic<bool> stop{false}, pause{true};
        QStringList logs;
        int n = 0;
        SeriesCallbacks cb;
        cb.log = [&](const QString& m) { logs << m; };
        cb.spectrum = [&](const Spectrum&) {
            if (++n == 2) stop = true;
        };
        std::thread resume([&] {
            QThread::msleep(600);
            pause = false;
        });
        runTimeSeries(*s, set, stop, cb, &pause);
        resume.join();
        QVERIFY(logs.contains("Paused"));
        QVERIFY(logs.contains("Continuing"));
        QCOMPARE(n, 2);
        QVERIFY(logs.last().startsWith("Series stopped: 2 spectra in "));
        QVERIFY(!s->laserOn());
    }

    void laserStaysOn() {
        QTemporaryDir dir;
        QStringList instLogs;
        auto s = std::make_unique<SimCora>(defaultCalibration(), [&](const QString& m) { instLogs << m; }, 200.0, 3);
        s->connect();
        s->initialize();
        s->pressArm();
        SeriesSettings set;
        set.folder = dir.path();
        set.exposure_s = 0.5;
        set.count = 3;
        set.laserStaysOn = true;
        set.start_delay_s = 0.3;
        std::atomic<bool> stop{false};
        QStringList logs;
        int n = 0;
        SeriesCallbacks cb;
        cb.log = [&](const QString& m) { logs << m; };
        cb.spectrum = [&](const Spectrum& sp) {
            ++n;
            QVERIFY(s->laserOn());
            QCOMPARE(sp.metaValue("mode"), QString("as fast as possible, laser on, background once"));
        };
        QElapsedTimer t;
        t.start();
        const QString folder = runTimeSeries(*s, set, stop, cb);
        QVERIFY(t.elapsed() >= 290);                              // start delay honoured
        QCOMPARE(n, 3);
        QCOMPARE(instLogs.count("Laser ON (532 nm, simulated)"), 1);
        QVERIFY(!s->laserOn());
        QCOMPARE(logs.first(), QString("Series starts in 0 s"));
        QCOMPARE(QDir(folder).entryList({"*.csv"}, QDir::Files).size(), 4);
    }

    void laserStaysOnRequiresArm() {
        QTemporaryDir dir;
        auto s = sim(200.0, false);
        SeriesSettings set;
        set.folder = dir.path();
        set.exposure_s = 0.2;
        set.laserStaysOn = true;
        std::atomic<bool> stop{false};
        bool threw = false;
        try {
            runTimeSeries(*s, set, stop);
        } catch (const NotArmed&) {
            threw = true;
        }
        QVERIFY(threw);
    }

    void seriesFilesAllFormats() {
        // fast mode, 25 spectra: series.csv, series.jdx and series.spc hold every spectrum, equal to the
        // per-spectrum CSVs (the ground truth) within the precision of each format
        QTemporaryDir dir;
        auto s = sim(400.0);
        SeriesSettings set;
        set.sample = "CCl4";
        set.folder = dir.path();
        set.exposure_s = 0.1;
        set.count = 25;
        set.laserStaysOn = true;
        std::atomic<bool> stop{false};
        QStringList logs;
        SeriesCallbacks cb;
        cb.log = [&](const QString& m) { logs << m; };
        const QString folder = runTimeSeries(*s, set, stop, cb);
        QVERIFY2(!logs.join('\n').contains("Series files:"), qPrintable(logs.join('\n')));
        const QDir d(folder);
        const QStringList own = d.entryList({"0*.csv"}, QDir::Files, QDir::Name);
        QCOMPARE(own.size(), 25);
        const auto table = fileio::read(d.filePath("series.csv"));
        const auto jdx = fileio::read(d.filePath("series.jdx"));
        const auto spc = fileio::read(d.filePath("series.spc"));
        QCOMPARE(table.size(), size_t(25));
        QCOMPARE(jdx.size(), size_t(25));
        QCOMPARE(spc.size(), size_t(25));
        double lastT = -1;
        for (int k = 0; k < 25; ++k) {
            const auto one = fileio::read(d.filePath(own[k]));
            QCOMPARE(one.size(), size_t(1));
            const fileio::DataSpectrum& g = one[0];
            const QString el = g.metaValue("series_elapsed_s");
            QCOMPARE(jdx[size_t(k)].metaValue("series_elapsed_s"), el);
            QCOMPARE(jdx[size_t(k)].metaValue("series_index"), QString::number(k + 1));
            QCOMPARE(jdx[size_t(k)].name, "CCl4 t=" + el + " s");
            QVERIFY(std::abs(spc[size_t(k)].metaValue("series_elapsed_s").toDouble() - el.toDouble()) < 1e-5);
            QVERIFY(el.toDouble() >= lastT);
            lastT = el.toDouble();
            QCOMPARE(jdx[size_t(k)].x.size(), g.x.size());
            for (size_t i = 0; i < g.x.size(); ++i) {
                QVERIFY(std::abs(jdx[size_t(k)].x[i] - g.x[i]) <= 0.0051);       // CSV: 2 decimals, JCAMP: 4
                QVERIFY(std::abs(jdx[size_t(k)].y[i] - g.y[i]) <= 0.051);
                QVERIFY(std::abs(spc[size_t(k)].y[i] - g.y[i]) <= 0.051 + 1e-6 * std::abs(g.y[i]));
                QVERIFY(std::abs(table[size_t(k)].y[i] - g.y[i]) <= 1e-9);         // both 1 decimal
            }
        }
        QCOMPARE(jdx[0].metaValue("sample"), QString("CCl4"));
        QCOMPARE(spc[0].metaValue("mode"), QString("as fast as possible, laser on, background once"));

        // only series.csv
        set.files.jdx = set.files.spc = false;
        set.count = 3;
        const QString f2 = runTimeSeries(*s, set, stop, cb);
        QVERIFY(QFileInfo::exists(QDir(f2).filePath("series.csv")));
        QVERIFY(!QFileInfo::exists(QDir(f2).filePath("series.jdx")));
        QVERIFY(!QFileInfo::exists(QDir(f2).filePath("series.spc")));
    }

    void seriesFilesWhenStopped() {
        // stopped after 4 spectra: the files are complete when runTimeSeries returns
        QTemporaryDir dir;
        auto s = sim(400.0);
        SeriesSettings set;
        set.folder = dir.path();
        set.exposure_s = 0.1;
        set.interval_s = 0.01;
        set.count = 100;
        std::atomic<bool> stop{false};
        int n = 0;
        SeriesCallbacks cb;
        cb.spectrum = [&](const Spectrum&) {
            if (++n == 4) stop = true;
        };
        const QString folder = runTimeSeries(*s, set, stop, cb);
        QCOMPARE(fileio::read(QDir(folder).filePath("series.jdx")).size(), size_t(4));
        QCOMPARE(fileio::read(QDir(folder).filePath("series.spc")).size(), size_t(4));
        QCOMPARE(fileio::read(QDir(folder).filePath("series.csv")).size(), size_t(4));
    }

    void safeNames() {
        QCOMPARE(safeName(""), QString("sample"));
        QCOMPARE(safeName("CCl4 run#2"), QString("CCl4_run_2"));
        QCOMPARE(safeName("a.b-c_d"), QString("a.b-c_d"));
        QCOMPARE(safeName(QString(80, 'x')).size(), 60);
    }
};

QTEST_MAIN(TestSeries)
#include "test_series.moc"
