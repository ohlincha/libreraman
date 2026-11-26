// GUI responsiveness during a long, fast series (offscreen, simulator at high speed, "as fast as possible"
// mode): the event loop must keep serving a 10 ms ping timer while spectra arrive faster than the pages could
// redraw them one by one, and every spectrum must be saved (per-spectrum CSVs, series.csv/.jdx/.spc).
// Scenarios: Measure, Data and Analysis area visible. The ping latencies are printed (qInfo) for the record.
#include "gui/analysispage.h"
#include "gui/analysispagehost.h"
#include "gui/controller.h"
#include "gui/datapage.h"
#include "gui/datapagehost.h"
#include "gui/mainwindow.h"
#include "gui/measurepage.h"
#include "gui/page.h"
#include "gui/theme.h"

#include "core/fileio.h"
#include "core/simulator.h"

#include <QApplication>
#include <QCheckBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QSignalSpy>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QTest>
#include <QTimer>

#include <algorithm>
#include <vector>

using namespace lr::gui;

namespace {

constexpr int kSpectra = 150;
constexpr int kPingMs = 10;
// Largest allowed gap between two 10 ms pings (offscreen; generous for slow CI machines: a Debug build on a
// desktop measures ~100-300 ms). Before the throttling the same series blocked the event loop for ~130 s.
constexpr qint64 kMaxGapMs = 1000;

struct PingStats {
    qint64 max = 0;
    qint64 p95 = 0;
    double mean = 0;
    int n = 0;
};

// Pings the event loop every kPingMs while `running` is true; returns the gaps beyond the interval.
class Pinger : public QObject {
public:
    Pinger() {
        m_timer.setInterval(kPingMs);
        m_timer.setTimerType(Qt::PreciseTimer);
        connect(&m_timer, &QTimer::timeout, this, [this] {
            const qint64 now = m_clock.nsecsElapsed();
            if (m_last >= 0) m_gaps.push_back((now - m_last) / 1000000 - kPingMs);
            m_last = now;
        });
    }
    void start() {
        m_gaps.clear();
        m_last = -1;
        m_clock.start();
        m_timer.start();
    }
    PingStats stop() {
        m_timer.stop();
        PingStats s;
        if (m_gaps.empty()) return s;
        std::vector<qint64> g = m_gaps;
        std::sort(g.begin(), g.end());
        s.n = int(g.size());
        s.max = g.back();
        s.p95 = g[size_t(double(g.size() - 1) * 0.95)];
        for (qint64 v : g) s.mean += double(v);
        s.mean /= double(g.size());
        return s;
    }

private:
    QTimer m_timer;
    QElapsedTimer m_clock;
    qint64 m_last = -1;
    std::vector<qint64> m_gaps;
};

}  // namespace

class GuiTestSeriesPerf : public QObject {
    Q_OBJECT
private slots:
    void initTestCase();
    void cleanupTestCase();
    void cleanup();
    void fastSeries_data();
    void fastSeries();

private:
    QTemporaryDir m_tmp;
    QTemporaryDir m_settingsDir;
    Controller* m_ctl = nullptr;
    MainWindow* m_win = nullptr;
};

void GuiTestSeriesPerf::initTestCase() {
    QVERIFY(m_tmp.isValid());
    QCoreApplication::setOrganizationName(QStringLiteral("LibreRamanTest"));
    QCoreApplication::setApplicationName(QStringLiteral("gui_test_seriesperf"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_settingsDir.path());
    theme::apply(*qApp, theme::Mode::Light);
    setDialogsEnabled(false);
    m_ctl = new Controller;
    m_ctl->setSimulatorSpeed(100);
    m_win = new MainWindow(m_ctl);
    m_win->resize(1440, 900);
    m_win->show();
    QSignalSpy connected(m_ctl, &Controller::connected);
    m_win->connectInstrument(true);
    QVERIFY(connected.wait(10000));
    m_ctl->simControls()->pressArm();
    QTRY_VERIFY(m_win->measurePage()->measureButton()->isEnabled());
}

void GuiTestSeriesPerf::cleanupTestCase() {
    delete m_win;
    delete m_ctl;
}

void GuiTestSeriesPerf::cleanup() {
    if (m_ctl->isBusy()) {
        m_ctl->stop();
        QTRY_VERIFY_WITH_TIMEOUT(!m_ctl->isBusy(), 10000);
    }
}

void GuiTestSeriesPerf::fastSeries_data() {
    QTest::addColumn<int>("area");
    QTest::newRow("Measure visible") << 0;
    QTest::newRow("Data visible") << 1;
    QTest::newRow("Analysis visible") << 3;
}

void GuiTestSeriesPerf::fastSeries() {
    QFETCH(int, area);
    MeasurePage* m = m_win->measurePage();
    auto* data = dynamic_cast<DataPageHost*>(m_win->page(QStringLiteral("Data")));
    auto* analysis = dynamic_cast<AnalysisPageHost*>(m_win->page(QStringLiteral("Analysis")));
    QVERIFY(data && analysis);
    data->dataPage()->clear();
    analysis->analysisPage()->clearSpectra();
    const QString folder = m_tmp.filePath(QStringLiteral("perf%1").arg(area));
    m->setFolder(folder);
    m->sampleEdit()->setText(QStringLiteral("perf"));
    m->exposureBox()->setValue(0.1);
    m->averagesBox()->setValue(1);
    m->fastestBox()->setChecked(true);
    m->countBox()->setValue(kSpectra);
    m_win->showArea(area);

    QSignalSpy spectra(m_ctl, &Controller::spectrum);
    Pinger ping;
    QElapsedTimer t;
    t.start();
    ping.start();
    m->startSeries();
    QVERIFY(m_ctl->isSeriesRunning());
    QTRY_VERIFY_WITH_TIMEOUT(!m_ctl->isBusy(), 120000);
    const qint64 seriesMs = t.elapsed();
    const PingStats s = ping.stop();
    QCoreApplication::processEvents();
    qInfo("%s: %d spectra in %lld ms (%.1f /s); ping gap beyond %d ms: max %lld ms, p95 %lld ms, mean %.1f ms "
          "(%d pings)",
          QTest::currentDataTag(), kSpectra, seriesMs, kSpectra * 1000.0 / double(std::max<qint64>(1, seriesMs)), kPingMs,
          s.max, s.p95, s.mean, s.n);

    // every spectrum delivered and saved
    QCOMPARE(spectra.size(), kSpectra);
    const QStringList dirs = QDir(folder).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    QCOMPARE(dirs.size(), 1);
    const QDir sd(QDir(folder).filePath(dirs.at(0)));
    QCOMPARE(sd.entryList({QStringLiteral("0*.csv")}, QDir::Files).size(), kSpectra);
    QCOMPARE(int(lr::fileio::read(sd.filePath(QStringLiteral("series.csv"))).size()), kSpectra);
    QCOMPARE(int(lr::fileio::read(sd.filePath(QStringLiteral("series.jdx"))).size()), kSpectra);
    QCOMPARE(int(lr::fileio::read(sd.filePath(QStringLiteral("series.spc"))).size()), kSpectra);

    // the pages caught up (at once when the series ended or when they are shown)
    QCOMPARE(int(m->trend().size()), kSpectra);
    QCOMPARE(int(m->history().size()), 50);
    ping.start();
    t.restart();
    m_win->showArea(1);
    QTRY_COMPARE(data->dataPage()->count(), kSpectra);
    const qint64 dataMs = t.elapsed();
    t.restart();
    m_win->showArea(3);
    QTRY_COMPARE(analysis->analysisPage()->spectrumCount(), kSpectra);
    QTRY_COMPARE(analysis->analysisPage()->rawData().rows(), kSpectra);
    const qint64 analysisMs = t.elapsed();
    // PCA of the visible tab: in a background thread for this many spectra (Debug build: ~2.5 s)
    QTRY_VERIFY_WITH_TIMEOUT(analysis->analysisPage()->pcaResult() &&
                                 analysis->analysisPage()->pcaResult()->scores.rows() == kSpectra, 30000);
    const PingStats c = ping.stop();
    m_win->showArea(0);
    qInfo("%s: catching up when shown: Data %lld ms, Analysis %lld ms (+ PCA in the background %lld ms); ping gap "
          "max %lld ms",
          QTest::currentDataTag(), dataMs, analysisMs, analysis->analysisPage()->lastAsyncMs(), c.max);

    QVERIFY2(s.max < kMaxGapMs, qPrintable(QStringLiteral("event loop blocked for %1 ms").arg(s.max)));
    QVERIFY2(c.max < kMaxGapMs, qPrintable(QStringLiteral("catching up blocked for %1 ms").arg(c.max)));
}

QTEST_MAIN(GuiTestSeriesPerf)
#include "gui_test_seriesperf.moc"
