// Widget test of the main window with the simulator at high speed (offscreen): window and pages, connect,
// ARM, measure once (spectrum shown and saved), a series of 3 spectra, pause/resume, Stop (laser off at
// once), closing without dialogs.
#include "gui/calibrationpage.h"
#include "gui/controller.h"
#include "gui/datapagehost.h"
#include "gui/mainwindow.h"
#include "gui/measurepage.h"
#include "gui/page.h"
#include "gui/plotwidget.h"
#include "gui/statusstrip.h"
#include "gui/theme.h"

#include "core/simulator.h"

#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileInfo>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QSignalSpy>
#include <QSpinBox>
#include <QTemporaryDir>
#include <QTest>

using namespace lr::gui;

namespace {

// Stands in for a page of another developer: records the spectra passed by the window (add_to_data hook).
class RecordingPage : public Page {
public:
    explicit RecordingPage(const PageContext& c) : ctx(c) {}
    void addSpectrum(const lr::Spectrum& sp) override { names << spectrumDisplayName(sp); }
    void activated() override { ++activations; }
    PageContext ctx;
    QStringList names;
    int activations = 0;
};

RecordingPage* g_recorder = nullptr;

}  // namespace

class GuiTestMainWindow : public QObject {
    Q_OBJECT
private slots:
    void initTestCase();
    void cleanupTestCase();
    void cleanup();
    void windowAndPages();
    void connectSimulator();
    void measureOnce();
    void busyRefusesSecondJob();
    void series();
    void pauseAndResume();
    void stopSwitchesLaserOff();
    void closePolicySetting();
    void closeWhileSeriesRunning();

private:
    QTemporaryDir m_tmp;
    QTemporaryDir m_settingsDir;
    Controller* m_ctl = nullptr;
    MainWindow* m_win = nullptr;
    QString log() const { return m_win->logView()->toPlainText(); }
};

void GuiTestMainWindow::initTestCase() {
    QVERIFY(m_tmp.isValid());
    QCoreApplication::setOrganizationName(QStringLiteral("LibreRamenTest"));
    QCoreApplication::setApplicationName(QStringLiteral("gui_test_mainwindow"));
    QSettings::setDefaultFormat(QSettings::IniFormat);              // keep the user's settings untouched
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_settingsDir.path());
    theme::apply(*qApp, theme::Mode::Light);
    setDialogsEnabled(false);
    registerPage(QStringLiteral("Library"), [](const PageContext& c) -> Page* {
        g_recorder = new RecordingPage(c);
        return g_recorder;
    });
    m_ctl = new Controller;
    m_ctl->setSimulatorSpeed(50);
    m_ctl->setPollInterval(50);
    m_win = new MainWindow(m_ctl);
    m_win->resize(1440, 900);
    m_win->show();
}

void GuiTestMainWindow::cleanupTestCase() {
    delete m_win;
    delete m_ctl;
}

void GuiTestMainWindow::cleanup() {             // a failed test must not leave a job running
    if (m_ctl->isBusy()) {
        m_ctl->stop();
        QTRY_VERIFY_WITH_TIMEOUT(!m_ctl->isBusy(), 10000);
    }
}

void GuiTestMainWindow::windowAndPages() {
    QVERIFY(m_win->measurePage());
    QCOMPARE(m_win->page(QStringLiteral("Measure")), static_cast<Page*>(m_win->measurePage()));
    QVERIFY(dynamic_cast<DataPageHost*>(m_win->page(QStringLiteral("Data"))));      // the real Data area
    QVERIFY(qobject_cast<CalibrationPage*>(m_win->page(QStringLiteral("Calibration"))));   // the real area
    QVERIFY(g_recorder);
    QCOMPARE(m_win->page(QStringLiteral("Library")), static_cast<Page*>(g_recorder));
    QCOMPARE(g_recorder->ctx.controller, m_ctl);
    QCOMPARE(g_recorder->controller(), m_ctl);
    QVERIFY(g_recorder->ctx.processing);
    QCOMPARE(g_recorder->ctx.processing().baseline, QStringLiteral("snip"));   // Measure page default
    m_win->showArea(2);
    QCOMPARE(m_win->currentArea(), 2);
    QCOMPARE(g_recorder->activations, 1);
    m_win->showArea(0);
    QVERIFY(m_win->windowTitle().startsWith(QStringLiteral("LibreRamen")));
    QVERIFY(!m_win->measurePage()->measureButton()->isEnabled());   // nothing connected yet
    QVERIFY(m_win->measurePage()->folder().contains(QStringLiteral("LibreRamen")));
    // linked exposure / scans / acquisition time
    MeasurePage* m = m_win->measurePage();
    m->exposureBox()->setValue(2.0);
    m->averagesBox()->setValue(4);
    QCOMPARE(m->acquisitionBox()->value(), 8.0);
    m->acquisitionBox()->setValue(20.0);
    QCOMPARE(m->averagesBox()->value(), 10);
}

void GuiTestMainWindow::connectSimulator() {
    QSignalSpy connected(m_ctl, &Controller::connected);
    QSignalSpy progress(m_ctl, &Controller::initProgress);
    m_win->connectInstrument(true);
    QVERIFY(connected.wait(10000));
    const auto info = connected.at(0).at(0).value<InstrumentInfo>();
    QVERIFY(info.simulated);
    QCOMPARE(info.serial, QStringLiteral("SIMULATOR"));
    QCOMPARE(info.calibration, QStringLiteral("factory"));
    QVERIFY(!progress.isEmpty());
    QCOMPARE(progress.last().at(0).toDouble(), 1.0);
    QCOMPARE(m_ctl->connection(), Controller::Connection::Connected);
    QVERIFY(m_ctl->simControls());
    QVERIFY(!m_ctl->watchdogRunning());                              // no watchdog for the simulator
    QTRY_VERIFY(m_win->simulatorBar()->isVisible());
    QTRY_COMPARE(m_win->statusStrip()->instrumentTile()->mainText(), QStringLiteral("● SIMULATOR"));
    QCOMPARE(m_win->statusStrip()->channelTile()->mainText(), QStringLiteral("532 nm"));
    QTRY_VERIFY(m_win->measurePage()->measureButton()->isEnabled());
    QVERIFY(log().contains(QStringLiteral("Instrument ready: Cora 5000 S/N SIMULATOR")));
    // the simulator bar's sample is applied to the simulator
    m_win->simulatorSample()->setCurrentText(QStringLiteral("carbon tetrachloride"));
    QCOMPARE(m_ctl->simControls()->sample(), QStringLiteral("carbon tetrachloride"));
}

void GuiTestMainWindow::measureOnce() {
    MeasurePage* m = m_win->measurePage();
    m->setFolder(m_tmp.filePath(QStringLiteral("day")));
    m->sampleEdit()->setText(QStringLiteral("CCl4 test"));
    m->exposureBox()->setValue(0.1);
    m->averagesBox()->setValue(1);

    // not armed: refused with a user-readable message, no spectrum
    QSignalSpy errors(m_ctl, &Controller::error);
    QSignalSpy spectra(m_ctl, &Controller::spectrum);
    QSignalSpy busy(m_ctl, &Controller::busyChanged);
    QTest::mouseClick(m->measureButton(), Qt::LeftButton);
    QTRY_COMPARE(errors.size(), 1);
    QVERIFY(errors.at(0).at(0).toString().contains(QStringLiteral("Not armed")));
    QTRY_VERIFY(!m_ctl->isBusy());
    QVERIFY(spectra.isEmpty());
    QVERIFY(log().contains(QStringLiteral("ERROR: Not armed")));

    m_ctl->simControls()->pressArm();
    QTRY_COMPARE(m_win->statusStrip()->laserTile()->mainText(), QStringLiteral("▲ ARMED"));
    QTest::mouseClick(m->measureButton(), Qt::LeftButton);
    QVERIFY(m_ctl->isBusy());
    QVERIFY(!m->measureButton()->isEnabled());
    QVERIFY(spectra.wait(10000));
    QTRY_VERIFY(!m_ctl->isBusy());
    QCOMPARE(errors.size(), 1);
    QVERIFY(m->measureButton()->isEnabled());
    const auto sp = spectra.at(0).at(0).value<lr::Spectrum>();
    const QString file = sp.metaValue(QStringLiteral("file"));
    QVERIFY(QFileInfo::exists(file));
    QCOMPARE(QFileInfo(file).absolutePath(), QDir(m_tmp.filePath(QStringLiteral("day"))).absolutePath());
    QVERIFY(QFileInfo(file).fileName().startsWith(QStringLiteral("CCl4_test_")));
    QVERIFY(QFileInfo(file).fileName().endsWith(QStringLiteral(".csv")));
    QCOMPARE(sp.metaValue(QStringLiteral("sample")), QStringLiteral("CCl4 test"));
    QVERIFY(log().contains(QStringLiteral("Saved ")));
    // shown: spectrum, peaks (CCl4: 459 cm-1 is the tallest band, followed by the trend)
    QCOMPARE(int(m->history().size()), 1);
    QVERIFY(!m->lastPeaks().empty());
    QTRY_VERIFY(std::abs(m->bandBox()->value() - 459) <= 3);
    QCOMPARE(int(m->trend().size()), 1);
    // ... and passed to the other pages (Data page hook)
    QCOMPARE(g_recorder->names.size(), 1);
    QVERIFY(g_recorder->names.at(0).startsWith(QStringLiteral("CCl4 test ")));
    // a second spectrum in the same second must not overwrite the first file
    QCOMPARE(Controller::savePath(QFileInfo(file).absolutePath(), QStringLiteral("CCl4 test"), sp.t),
             QFileInfo(file).absolutePath() + QStringLiteral("/") + QFileInfo(file).completeBaseName() +
                 QStringLiteral("_2.csv"));
}

void GuiTestMainWindow::busyRefusesSecondJob() {
    MeasurePage* m = m_win->measurePage();
    QSignalSpy errors(m_ctl, &Controller::error);
    QSignalSpy spectra(m_ctl, &Controller::spectrum);
    QVERIFY(m_ctl->measureOnce(m->measureSettings()));
    QVERIFY(!m_ctl->measureOnce(m->measureSettings()));
    QCOMPARE(errors.size(), 1);
    QVERIFY(errors.at(0).at(0).toString().startsWith(QStringLiteral("Busy")));
    QVERIFY(spectra.wait(10000));
    QTRY_VERIFY(!m_ctl->isBusy());
    QCOMPARE(spectra.size(), 1);
}

void GuiTestMainWindow::series() {
    MeasurePage* m = m_win->measurePage();
    m->setFolder(m_tmp.filePath(QStringLiteral("series")));
    m->sampleEdit()->setText(QStringLiteral("ser"));
    m->intervalUnitBox()->setCurrentText(QStringLiteral("s"));
    m->intervalBox()->setValue(0.3);                 // shorter than possible: adjusted (no dialog in tests)
    m->countBox()->setValue(3);
    QSignalSpy spectra(m_ctl, &Controller::spectrum);
    QSignalSpy progress(m_ctl, &Controller::seriesProgress);
    QTest::mouseClick(m->seriesButton(), Qt::LeftButton);
    QVERIFY(m_ctl->isSeriesRunning());
    QVERIFY(m->pauseButton()->isEnabled());
    QVERIFY(m->intervalBox()->value() >= m->minInterval());
    QTRY_VERIFY_WITH_TIMEOUT(!m_ctl->isBusy(), 20000);
    QCOMPARE(spectra.size(), 3);
    QVERIFY(!progress.isEmpty());
    const auto last = progress.last().at(0).value<lr::SeriesProgress>();
    QVERIFY(last.done);
    QCOMPARE(last.n, 3);
    QVERIFY(m->seriesText().startsWith(QStringLiteral("3 / 3")));
    QVERIFY(m->seriesText().contains(QStringLiteral("done")));
    QCOMPARE(int(m->history().size()), 3);
    QCOMPARE(int(m->trend().size()), 3);
    QVERIFY(!m->pauseButton()->isEnabled());
    // files: <folder>/ser_series_<date>_<time>/{0001_..csv, 0002, 0003, series.csv}
    const QStringList dirs = QDir(m_tmp.filePath(QStringLiteral("series"))).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    QCOMPARE(dirs.size(), 1);
    QVERIFY(dirs.at(0).startsWith(QStringLiteral("ser_series_")));
    const QDir sd(m_tmp.filePath(QStringLiteral("series/")) + dirs.at(0));
    QVERIFY(sd.exists(QStringLiteral("series.csv")));
    QCOMPARE(sd.entryList({QStringLiteral("000*.csv")}, QDir::Files).size(), 3);
    QVERIFY(log().contains(QStringLiteral("Series finished: 3 spectra")));
    // waterfall view draws one curve per spectrum
    QCOMPARE(int(m->trendPlot()->items().size()), 1);
    m->trendModeBox()->setCurrentText(QStringLiteral("waterfall"));
    QCOMPARE(int(m->trendPlot()->items().size()), 3);
    m->trendModeBox()->setCurrentText(QStringLiteral("trend"));
}

void GuiTestMainWindow::pauseAndResume() {
    MeasurePage* m = m_win->measurePage();
    m->countBox()->setValue(3);
    m->intervalBox()->setValue(std::ceil(m->minInterval() * 10) / 10 + 0.5);
    QSignalSpy spectra(m_ctl, &Controller::spectrum);
    m->startSeries();
    QVERIFY(m_ctl->isSeriesRunning());
    QVERIFY(spectra.wait(5000));                    // first spectrum, then pause while waiting for the next
    m->pauseButton()->click();
    QVERIFY(m_ctl->isPaused());
    QCOMPARE(m->pauseButton()->text(), QStringLiteral("▶  Resume"));
    QTRY_VERIFY_WITH_TIMEOUT(m->seriesText().contains(QStringLiteral("(paused)")), 5000);
    QTest::qWait(int(m->intervalBox()->value() * 1000));
    QCOMPARE(spectra.size(), 1);
    m->pauseButton()->click();                      // resume
    QVERIFY(!m_ctl->isPaused());
    QTRY_VERIFY_WITH_TIMEOUT(!m_ctl->isBusy(), 20000);
    QCOMPARE(spectra.size(), 3);
    QVERIFY(log().contains(QStringLiteral("Continuing")));
    QCOMPARE(m->pauseButton()->text(), QStringLiteral("❚❚  Pause"));
}

void GuiTestMainWindow::stopSwitchesLaserOff() {
    MeasurePage* m = m_win->measurePage();
    m_ctl->setSimulatorSpeed(1);                    // a real-time measurement of 5 s
    m->exposureBox()->setValue(5.0);
    m->averagesBox()->setValue(1);
    QSignalSpy spectra(m_ctl, &Controller::spectrum);
    QSignalSpy errors(m_ctl, &Controller::error);
    QTest::mouseClick(m->measureButton(), Qt::LeftButton);
    QTRY_VERIFY_WITH_TIMEOUT(m_ctl->instrument()->laserOn(), 5000);
    QTRY_VERIFY(m_win->windowTitle().startsWith(QStringLiteral("● LASER ON")));
    QTRY_COMPARE(m_win->statusStrip()->laserTile()->mainText(), QStringLiteral("✹ LASER ON"));
    QTest::mouseClick(m_win->statusStrip()->stopButton(), Qt::LeftButton);
    QVERIFY(!m_ctl->instrument()->laserOn());       // at once, not after the frame
    QVERIFY(!m_ctl->instrument()->status().laser_on);
    QTRY_VERIFY_WITH_TIMEOUT(!m_ctl->isBusy(), 3000);
    QCOMPARE(spectra.size(), 0);
    QCOMPARE(errors.size(), 0);
    QVERIFY(log().contains(QStringLiteral("Stopped")));
    QTRY_VERIFY(!m_win->windowTitle().contains(QStringLiteral("LASER ON")));
    m_ctl->setSimulatorSpeed(50);
    m->exposureBox()->setValue(0.1);
}

void GuiTestMainWindow::closePolicySetting() {
    QCOMPARE(MainWindow::onClosePolicy(), MainWindow::OnClose::Ask);
    MainWindow::setOnClosePolicy(MainWindow::OnClose::LeaveOn);
    QCOMPARE(MainWindow::onClosePolicy(), MainWindow::OnClose::LeaveOn);
    QCOMPARE(QSettings().value(QStringLiteral("instrument/onClose")).toString(), QStringLiteral("leave"));
    MainWindow::setOnClosePolicy(MainWindow::OnClose::Ask);
    QCOMPARE(MainWindow::onClosePolicy(), MainWindow::OnClose::Ask);
}

void GuiTestMainWindow::closeWhileSeriesRunning() {
    MeasurePage* m = m_win->measurePage();
    m_ctl->setSimulatorSpeed(1);
    m->exposureBox()->setValue(2.0);
    m->countBox()->setValue(100);
    m->intervalBox()->setValue(10);
    m->startSeries();
    QVERIFY(m_ctl->isSeriesRunning());
    QTRY_VERIFY_WITH_TIMEOUT(m_ctl->instrument()->laserOn(), 5000);
    QVERIFY(m_win->close());                        // no question (dialogs off): stops and quits
    QVERIFY(!m_win->isVisible());
    QVERIFY(!m_ctl->isBusy());
    QVERIFY(!m_ctl->instrument()->laserOn());
}

QTEST_MAIN(GuiTestMainWindow)
#include "gui_test_mainwindow.moc"
