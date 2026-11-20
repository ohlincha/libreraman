// Widget test of the Calibration area with the simulator at high speed (offscreen, files in a temporary
// XDG_CONFIG_HOME): wizard with carbon tetrachloride + chloroform (measure, match, fit, accept -> history copy and
// active file, the instrument's calibration changes at once), NotArmed, Stop, one-point offset, quick check,
// loading a saved spectrum, history (make active, revert to factory), instrument log and reminder.
#include "gui/calibrationpage.h"
#include "gui/calibrationstore.h"
#include "gui/controller.h"
#include "gui/mainwindow.h"
#include "gui/page.h"
#include "gui/plotwidget.h"
#include "gui/theme.h"

#include "core/calibrate.h"
#include "core/simulator.h"
#include "core/standards.h"

#include <QApplication>
#include <QDir>
#include <QFileInfo>
#include <QDoubleSpinBox>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QListWidget>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QSettings>
#include <QSignalSpy>
#include <QTableWidget>
#include <QTemporaryDir>
#include <QTest>

#include <cmath>

using namespace lr::gui;

class GuiTestCalibrationPage : public QObject {
    Q_OBJECT
private slots:
    void initTestCase();
    void cleanupTestCase();
    void cleanup();
    void offline();
    void connectSimulator();
    void notArmed();
    void wizardPolynomial();
    void stopMeasurement();
    void quickCheck();
    void oneSampleOffset();
    void loadSavedSpectrum();
    void history();
    void reminder();

private:
    void arm();
    void measure(const QString& name);
    QString log() const { return m_win->logView()->toPlainText(); }
    lr::Instrument* inst() const { return m_ctl->instrument(); }

    QTemporaryDir m_cfg;
    QTemporaryDir m_settingsDir;
    Controller* m_ctl = nullptr;
    MainWindow* m_win = nullptr;
    CalibrationPage* m_page = nullptr;
};

void GuiTestCalibrationPage::initTestCase() {
    QVERIFY(m_cfg.isValid());
    qputenv("XDG_CONFIG_HOME", m_cfg.path().toLocal8Bit());       // calib::configDir() -> <tmp>/libreramen
    QCOMPARE(lr::calib::configDir(), QDir::cleanPath(m_cfg.path() + QStringLiteral("/libreramen")));
    QCoreApplication::setOrganizationName(QStringLiteral("LibreRamenTest"));
    QCoreApplication::setApplicationName(QStringLiteral("gui_test_calibrationpage"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_settingsDir.path());
    theme::apply(*qApp, theme::Mode::Light);
    setDialogsEnabled(false);
    m_ctl = new Controller;
    m_ctl->setSimulatorSpeed(200);
    m_ctl->setPollInterval(50);
    m_win = new MainWindow(m_ctl);
    m_win->resize(1440, 900);
    m_win->show();
    m_page = qobject_cast<CalibrationPage*>(m_win->page(QStringLiteral("Calibration")));
    QVERIFY(m_page);
    m_win->showArea(int(pageAreas().indexOf(QStringLiteral("Calibration"))));
}

void GuiTestCalibrationPage::cleanupTestCase() {
    delete m_win;
    delete m_ctl;
}

void GuiTestCalibrationPage::cleanup() {
    if (m_ctl->isBusy()) {
        m_ctl->stop();
        QTRY_VERIFY_WITH_TIMEOUT(!m_ctl->isBusy(), 10000);
    }
}

void GuiTestCalibrationPage::arm() {
    m_ctl->simControls()->closeCompartment();
    m_ctl->simControls()->pressArm();
    QTRY_VERIFY(m_ctl->lastStatus().armed);
}

void GuiTestCalibrationPage::measure(const QString& name) {
    m_ctl->simControls()->setSample(name);
    QSignalSpy ready(m_page, &CalibrationPage::sampleReady);
    QVERIFY(m_page->measureStandard(name));
    QVERIFY(m_ctl->isBusy());
    QVERIFY(!m_page->measureButton()->isEnabled());
    QVERIFY(ready.wait(20000));
    QCOMPARE(ready.at(0).at(0).toString(), name);
    QTRY_VERIFY(!m_ctl->isBusy());
}

void GuiTestCalibrationPage::offline() {
    // no instrument: offline key (built-in serial), factory calibration, nothing in the history
    QCOMPARE(m_page->serial(), lr::defaultCalibration().serial);
    QCOMPARE(m_page->channel(), 532);
    QVERIFY(m_page->historyEntries().empty());
    QVERIFY(m_page->logEntries().empty());
    QVERIFY(m_page->activeCalibrationText().startsWith(QStringLiteral("Factory")));
    QVERIFY(!m_page->measureButton()->isEnabled());
    QVERIFY(!m_page->measureStandard(QStringLiteral("chloroform")));      // refused, no crash
    // the standards list shows every standard; the default selection is CCl4 + chloroform
    QCOMPARE(m_page->standardsList()->count(), int(lr::standards::all().size()));
    QCOMPARE(m_page->selectedStandards(),
             QStringList({QStringLiteral("carbon tetrachloride"), QStringLiteral("chloroform")}));
    QCOMPARE(m_page->samplesTable()->rowCount(), 2);
}

void GuiTestCalibrationPage::connectSimulator() {
    QSignalSpy connected(m_ctl, &Controller::connected);
    m_win->connectInstrument(true);
    QVERIFY(connected.wait(10000));
    QCOMPARE(m_page->serial(), QStringLiteral("SIMULATOR"));    // never the real instrument's files
    QTRY_VERIFY(log().contains(QStringLiteral("Calibration reminder: The 532 nm channel has not been calibrated")));
    QVERIFY(m_page->measureButton()->isEnabled());
}

void GuiTestCalibrationPage::notArmed() {
    m_ctl->simControls()->openCompartment();                   // disarms
    QTRY_VERIFY(!m_ctl->lastStatus().armed);
    QSignalSpy errors(m_ctl, &Controller::error);
    QSignalSpy ready(m_page, &CalibrationPage::sampleReady);
    QVERIFY(m_page->measureStandard(QStringLiteral("carbon tetrachloride")));
    QTRY_COMPARE(errors.size(), 1);
    QVERIFY(errors.at(0).at(0).toString().contains(QStringLiteral("compartment is open")));   // NotArmed
    QTRY_VERIFY(!m_ctl->isBusy());
    QVERIFY(ready.isEmpty());
    // closed but not armed
    m_ctl->simControls()->closeCompartment();
    QVERIFY(m_page->measureStandard(QStringLiteral("carbon tetrachloride")));
    QTRY_COMPARE(errors.size(), 2);
    QVERIFY(errors.at(1).at(0).toString().contains(QStringLiteral("Not armed")));
    QTRY_VERIFY(!m_ctl->isBusy());
    QVERIFY(ready.isEmpty());
    QVERIFY(!m_page->sample(QStringLiteral("carbon tetrachloride")));
    QTRY_VERIFY(m_page->measureButton()->isEnabled());           // ready for the next attempt
}

void GuiTestCalibrationPage::wizardPolynomial() {
    arm();
    m_page->setSelectedStandards({QStringLiteral("ccl4"), QStringLiteral("chcl3")});   // aliases
    QCOMPARE(m_page->selectedStandards(),
             QStringList({QStringLiteral("carbon tetrachloride"), QStringLiteral("chloroform")}));
    measure(QStringLiteral("carbon tetrachloride"));
    measure(QStringLiteral("chloroform"));
    const auto* ccl4 = m_page->sample(QStringLiteral("carbon tetrachloride"));
    const auto* chcl3 = m_page->sample(QStringLiteral("chloroform"));
    QVERIFY(ccl4 && chcl3);
    QVERIFY(ccl4->matches.size() >= 3);
    QVERIFY(chcl3->matches.size() >= 3);
    QVERIFY(QFileInfo::exists(ccl4->file));                       // measured spectra are kept (provenance)
    QVERIFY(ccl4->file.startsWith(lr::calib::configDir()));
    QVERIFY(std::isfinite(ccl4->dark));
    QCOMPARE(m_page->samplesTable()->item(0, 2)->text(),
             QStringLiteral("%1 / %2").arg(ccl4->matches.size()).arg(lr::standards::bands(QStringLiteral("ccl4")).size()));
    QVERIFY(m_page->spectrumPlot()->items().size() > 3);          // spectrum, band markers, matched peaks

    const double before = inst()->shiftOf(532, 1000.0);
    m_page->setMode(CalibrationPage::Mode::Polynomial);
    m_page->setDegree(0);
    QVERIFY(!m_page->acceptButton()->isEnabled());
    QVERIFY(m_page->fit());
    QVERIFY(m_page->hasFit());
    const lr::calib::Result r = m_page->result();
    QCOMPARE(int(r.residuals.size()), int(ccl4->matches.size() + chcl3->matches.size()));
    QVERIFY(r.rms < 3.0);
    QCOMPARE(r.standards.size(), std::size_t(2));
    QCOMPARE(m_page->residualTable()->rowCount(), int(r.residuals.size()));
    QVERIFY(!m_page->residualTable()->item(0, 3)->text().isEmpty());    // "after" column
    QVERIFY(m_page->acceptButton()->isEnabled());
    // degree choice refits
    m_page->setDegree(1);
    QCOMPARE(m_page->result().degree, 1);
    m_page->setDegree(0);

    QSignalSpy changed(m_page, &CalibrationPage::calibrationChanged);
    QVERIFY(m_page->acceptCalibration());
    QCOMPARE(changed.size(), 1);
    const QString active = calstore::activePath(QStringLiteral("SIMULATOR"), 532);
    QVERIFY(QFileInfo::exists(active));
    QVERIFY(QFileInfo::exists(m_page->lastAccepted()));
    QVERIFY(QFileInfo(m_page->lastAccepted()).fileName().startsWith(QStringLiteral("calibration_SIMULATOR_532_")));
    // the active file is readable by calib::load (and Python load: same keys)
    const auto loaded = lr::calib::load(532, QStringLiteral("SIMULATOR"));
    QVERIFY(loaded);
    QCOMPARE(loaded->standards.size(), std::size_t(2));
    QFile f(active);
    QVERIFY(f.open(QIODevice::ReadOnly));
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    for (const char* key : {"poly", "residuals", "rms", "matches", "standards", "date", "degree", "channel", "serial"})
        QVERIFY2(o.contains(QLatin1String(key)), key);
    QCOMPARE(o.value(QLatin1String("kind")).toString(), QStringLiteral("polynomial"));
    QCOMPARE(o.value(QLatin1String("spectra")).toArray().size(), 2);
    // in effect at once
    QVERIFY(inst()->userCalibration().count(532));
    QVERIFY(std::abs(inst()->shiftOf(532, 1000.0) - before) > 1e-6);
    QCOMPARE(inst()->shiftOf(532, 1000.0), r.poly(1000.0));
    QVERIFY(inst()->calibrationLabel(532).startsWith(QStringLiteral("user ")));
    QVERIFY(m_page->activeCalibrationText().startsWith(QStringLiteral("User calibration")));
    QVERIFY(!m_page->hasFit());
    // history and log
    QCOMPARE(int(m_page->historyEntries().size()), 1);
    QVERIFY(m_page->historyEntries()[0].active);
    QCOMPARE(m_page->historyEntries()[0].bands, int(r.residuals.size()));
    QCOMPARE(m_page->historyTable()->rowCount(), 1);
    QCOMPARE(int(m_page->logEntries().size()), 1);
    QCOMPARE(m_page->logEntries()[0].event, QStringLiteral("calibration"));
    QVERIFY(std::isfinite(m_page->logEntries()[0].devMean));
    QVERIFY(std::isfinite(m_page->logEntries()[0].dark));
    QVERIFY(log().contains(QStringLiteral("Calibration accepted for 532 nm")));
    // a new spectrum is matched against the new calibration: deviations are now small
    QVERIFY(m_page->reminderText().isEmpty());
}

void GuiTestCalibrationPage::stopMeasurement() {
    arm();
    m_page->setSelectedStandards({QStringLiteral("chloroform")});
    QSignalSpy ready(m_page, &CalibrationPage::sampleReady);
    // long exposure (simulated: 120 s / 200), then Stop
    QDoubleSpinBox* exposure = m_page->exposureBox();
    const double old = exposure->value();
    exposure->setValue(120);
    QVERIFY(m_page->measureStandard(QStringLiteral("chloroform")));
    QTRY_VERIFY(m_ctl->lastStatus().laserOn || !m_ctl->isBusy());
    m_ctl->stop();
    QTRY_VERIFY_WITH_TIMEOUT(!m_ctl->isBusy(), 5000);
    QVERIFY(!inst()->laserOn());
    QVERIFY(ready.isEmpty());
    QVERIFY(!m_page->sample(QStringLiteral("chloroform")));
    exposure->setValue(old);
}

void GuiTestCalibrationPage::quickCheck() {
    arm();
    const std::vector<double> coeff = inst()->userCalibration().at(532).poly.coeff;
    m_ctl->simControls()->setSample(QStringLiteral("chloroform"));
    QSignalSpy done(m_page, &CalibrationPage::checkFinished);
    QVERIFY(m_page->quickCheck(QStringLiteral("chcl3")));
    QVERIFY(done.wait(20000));
    QTRY_VERIFY(!m_ctl->isBusy());
    QVERIFY(m_page->lastCheck());
    QVERIFY(m_page->lastCheck()->bands.size() >= 3);
    QVERIFY(m_page->lastCheck()->rms < 2.0);                      // just calibrated
    QCOMPARE(m_page->checkVerdict(), QStringLiteral("ok"));
    QCOMPARE(m_page->checkTable()->rowCount(), int(m_page->lastCheck()->bands.size()));
    QCOMPARE(inst()->userCalibration().at(532).poly.coeff, coeff); // unchanged
    QCOMPARE(int(m_page->historyEntries().size()), 1);
    QCOMPARE(m_page->logEntries().back().event, QStringLiteral("quick check"));
    QCOMPARE(m_page->logEntries().back().note, QStringLiteral("ok"));
    QVERIFY(log().contains(QStringLiteral("Calibration check with chloroform")));
}

void GuiTestCalibrationPage::oneSampleOffset() {
    arm();
    m_page->setSelectedStandards({QStringLiteral("chloroform")});
    measure(QStringLiteral("chloroform"));
    m_page->setMode(CalibrationPage::Mode::Offset);
    QVERIFY(m_page->fit());
    QVERIFY(std::isfinite(m_page->offset()));
    QVERIFY(std::abs(m_page->offset()) < 3.0);                   // already calibrated
    const lr::calib::ShiftFn cur = m_page->currentShift();
    // a constant shift of the calibration in use
    const double d0 = m_page->result().poly(200.0) - cur(200.0), d1 = m_page->result().poly(1800.0) - cur(1800.0);
    QVERIFY(std::abs(d0 - m_page->offset()) < 0.05);
    QVERIFY(std::abs(d1 - m_page->offset()) < 0.05);
    QVERIFY(m_page->acceptCalibration());
    QCOMPARE(int(m_page->historyEntries().size()), 2);
    QCOMPARE(m_page->historyEntries()[0].kind, QStringLiteral("offset"));
    QVERIFY(m_page->historyEntries()[0].active);
    QVERIFY(!m_page->historyEntries()[1].active);
    // silicon alone selects the one-point mode automatically
    m_page->setMode(CalibrationPage::Mode::Polynomial);
    m_page->setSelectedStandards({QStringLiteral("silicon")});
    QCOMPARE(m_page->mode(), CalibrationPage::Mode::Offset);
}

void GuiTestCalibrationPage::loadSavedSpectrum() {
    // the spectra measured above were saved; load one as if offline
    const QDir dir(lr::calib::configDir() + QStringLiteral("/calibration-spectra"));
    const QStringList files = dir.entryList({QStringLiteral("carbon_tetrachloride_*.csv")}, QDir::Files);
    QVERIFY(!files.isEmpty());
    m_page->setSelectedStandards({QStringLiteral("chloroform")});
    QVERIFY(m_page->loadStandardFile(QStringLiteral("CCl4"), dir.filePath(files.first())));
    QVERIFY(m_page->selectedStandards().contains(QStringLiteral("carbon tetrachloride")));   // added
    const auto* s = m_page->sample(QStringLiteral("carbon tetrachloride"));
    QVERIFY(s);
    QCOMPARE(int(s->y.size()), 2048);
    QVERIFY(s->matches.size() >= 3);
    QCOMPARE(s->origin, files.first());
    // quick check from the file
    QSignalSpy done(m_page, &CalibrationPage::checkFinished);
    QVERIFY(m_page->quickCheckFile(QStringLiteral("carbon tetrachloride"), dir.filePath(files.first())));
    QCOMPARE(done.size(), 1);
    QVERIFY(!m_page->lastCheck()->bands.empty());
    // not a pixel spectrum: refused with a message
    QFile bad(m_cfg.filePath(QStringLiteral("short.csv")));
    QVERIFY(bad.open(QIODevice::WriteOnly));
    bad.write("raman_shift_cm1,intensity\n100,1\n200,2\n300,3\n");
    bad.close();
    QVERIFY(!m_page->loadStandardFile(QStringLiteral("ethanol"), bad.fileName()));
    QVERIFY(log().contains(QStringLiteral("detector has 2048 pixels")));
}

void GuiTestCalibrationPage::history() {
    m_page->refreshHistory();
    QCOMPARE(int(m_page->historyEntries().size()), 2);
    const auto older = m_page->historyEntries()[1];
    QVERIFY(older.kind == QStringLiteral("polynomial"));
    // make the older (polynomial) calibration active again
    QVERIFY(m_page->makeActive(1));
    QVERIFY(m_page->historyEntries()[1].active);
    QVERIFY(!m_page->historyEntries()[0].active);
    const auto c = lr::calib::load(532, QStringLiteral("SIMULATOR"), older.path);
    QVERIFY(c);
    QCOMPARE(inst()->userCalibration().at(532).poly.coeff, c->poly.coeff);
    QCOMPARE(m_page->logEntries().back().event, QStringLiteral("activated"));
    // details of the selected entry
    m_page->historyTable()->selectRow(1);
    QVERIFY(m_page->findChild<QPlainTextEdit*>()->toPlainText().contains(QStringLiteral("reference")));
    // revert to factory: active file removed, history kept, factory shift in effect
    const double factory = lr::defaultCalibration().channels.at(532).shift(1000.0);
    QVERIFY(m_page->revertToFactory());
    QVERIFY(!QFileInfo::exists(calstore::activePath(QStringLiteral("SIMULATOR"), 532)));
    QVERIFY(inst()->userCalibration().empty());
    QCOMPARE(inst()->shiftOf(532, 1000.0), factory);
    QCOMPARE(int(m_page->historyEntries().size()), 2);
    for (const auto& e : m_page->historyEntries())
        QVERIFY(!e.active);
    QCOMPARE(m_page->logEntries().back().event, QStringLiteral("factory"));
    QVERIFY(m_page->activeCalibrationText().startsWith(QStringLiteral("Factory")));

    // an active file written elsewhere (e.g. the Python CLI) is listed and archived on revert
    lr::calib::Result r;
    r.poly = c->poly;
    r.residuals = {{218.0, 0.1, 100.0}, {459.0, -0.1, 300.0}};
    r.standards = {QStringLiteral("carbon tetrachloride")};
    r.date = QStringLiteral("2026-01-02 03:04");
    r.degree = 1;
    lr::calib::save(r, 532, QStringLiteral("SIMULATOR"));
    m_page->refreshHistory();
    QCOMPARE(int(m_page->historyEntries().size()), 3);
    int activeOnly = 0;
    for (const auto& e : m_page->historyEntries())
        activeOnly += e.activeOnly ? 1 : 0;
    QCOMPARE(activeOnly, 1);
    QVERIFY(m_page->revertToFactory());
    QCOMPARE(int(m_page->historyEntries().size()), 3);         // archived as a history copy
    for (const auto& e : m_page->historyEntries())
        QVERIFY(!e.activeOnly && !e.active);
    QVERIFY(QDir(lr::calib::configDir()).exists(QStringLiteral("calibration_SIMULATOR_532_20260102-030400.json")));
}

void GuiTestCalibrationPage::reminder() {
    // an old active calibration -> warning on the page and in the log at connect
    lr::calib::Result r;
    r.poly = lr::Poly{{0.0, 1.0}, 0.0, 1.0};
    r.residuals = {{218.0, 0.1, 100.0}, {459.0, -0.1, 300.0}};
    r.standards = {QStringLiteral("carbon tetrachloride")};
    r.date = QDateTime::currentDateTime().addDays(-40).toString(QStringLiteral("yyyy-MM-dd HH:mm"));
    r.degree = 1;
    calstore::accept(r, 532, QStringLiteral("SIMULATOR"), QStringLiteral("polynomial"));
    m_page->refreshHistory();
    QVERIFY(m_page->makeActive(0));
    m_page->setReminderDays(30);
    QCOMPARE(QSettings().value(QStringLiteral("calibration/reminderDays")).toInt(), 30);
    QVERIFY(m_page->reminderText().contains(QStringLiteral("40 days ago")));
    m_page->setReminderDays(60);
    QVERIFY(m_page->reminderText().isEmpty());
    m_page->setReminderDays(0);                                 // off
    QVERIFY(m_page->reminderText().isEmpty());
    m_page->setReminderDays(30);
    QSignalSpy connected(m_ctl, &Controller::connected);
    m_win->connectInstrument(true);                             // the page applies the simulator's active file
    QVERIFY(connected.wait(10000));
    QTRY_VERIFY(log().contains(QStringLiteral("Calibration reminder: Last calibration of the 532 nm channel was 40 days ago")));
    QVERIFY(inst()->userCalibration().count(532));
    QCOMPARE(inst()->shiftOf(532, 1000.0), 1000.0);             // the identity polynomial written above
    QVERIFY(m_page->revertToFactory());
}

QTEST_MAIN(GuiTestCalibrationPage)
#include "gui_test_calibrationpage.moc"
