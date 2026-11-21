// Renders the Calibration area with the simulator to PNG files (visual checks; built with
// -DLIBRERAMEN_GUI_PREVIEW=ON). Files go to a temporary XDG_CONFIG_HOME, never the user's own.
// Usage: QT_QPA_PLATFORM=offscreen calibration_preview [output dir] [--dark]
#include "gui/calibrationpage.h"
#include "gui/calibrationstore.h"
#include "gui/controller.h"
#include "gui/mainwindow.h"
#include "gui/page.h"
#include "gui/theme.h"

#include "core/simulator.h"

#include <QApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QSettings>
#include <QTabWidget>
#include <QTableWidget>
#include <QTemporaryDir>

#include <cmath>
#include <cstdio>
#include <functional>

using namespace lr::gui;

namespace {

bool waitFor(const std::function<bool()>& cond, int ms = 20000) {
    QElapsedTimer t;
    t.start();
    while (!cond()) {
        if (t.elapsed() > ms)
            return false;
        QApplication::processEvents(QEventLoop::AllEvents, 20);
    }
    return true;
}

void settle(int ms = 300) { waitFor([] { return false; }, ms); }

bool shot(QWidget* w, const QString& path) {
    settle();
    const bool ok = w->grab().save(path);
    std::printf("%s %s\n", ok ? "wrote" : "FAILED", qPrintable(path));
    return ok;
}

}  // namespace

int main(int argc, char** argv) {
    QTemporaryDir cfg;
    qputenv("XDG_CONFIG_HOME", cfg.path().toLocal8Bit());
    QApplication app(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("LibreRamenPreview"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, cfg.path());
    const QStringList args = app.arguments();
    const bool dark = args.contains(QStringLiteral("--dark"));
    QString out = QDir::homePath() + QStringLiteral("/opt/gui-preview");
    for (int i = 1; i < args.size(); ++i)
        if (!args[i].startsWith(QLatin1String("--")))
            out = args[i];
    QDir().mkpath(out);
    const QString sfx = dark ? QStringLiteral("-dark") : QString();
    theme::apply(app, dark ? theme::Mode::Dark : theme::Mode::Light);
    setDialogsEnabled(false);

    Controller ctl;
    ctl.setSimulatorSpeed(100);
    MainWindow win(&ctl);
    win.resize(1440, 900);
    win.show();
    auto* page = qobject_cast<CalibrationPage*>(win.page(QStringLiteral("Calibration")));
    if (!page)
        return 1;
    win.showArea(int(pageAreas().indexOf(QStringLiteral("Calibration"))));

    // some earlier history in the instrument log (drift plot)
    const QDateTime now = QDateTime::currentDateTime();
    const double drift[] = {0.2, 0.5, 0.9, 1.3, -0.1, 0.3, 0.6};
    const double darkLevel[] = {1052, 1058, 1049, 1061, 1055, 1051, 1057};
    for (int i = 0; i < 7; ++i) {
        calstore::LogEntry e;
        e.time = now.addDays(-70 + 10 * i);
        e.serial = QStringLiteral("SIMULATOR");
        e.channel = 532;
        e.event = i == 4 ? QStringLiteral("calibration") : QStringLiteral("quick check");
        e.standards = {i == 4 ? QStringLiteral("carbon tetrachloride; chloroform") : QStringLiteral("chloroform")};
        e.bands = 6;
        e.devMean = drift[i];
        e.devRms = std::abs(drift[i]) + 0.3;
        e.dark = darkLevel[i];
        e.exposure = 1.0;
        e.note = i == 4 ? QStringLiteral("polynomial") : QStringLiteral("ok");
        calstore::appendLog(e);
    }

    ctl.connectInstrument(true);
    if (!waitFor([&] { return ctl.connection() == Controller::Connection::Connected && !ctl.isBusy(); }))
        return 2;
    shot(&win, out + QStringLiteral("/calibration-start") + sfx + QStringLiteral(".png"));
    ctl.simControls()->closeCompartment();
    ctl.simControls()->pressArm();
    waitFor([&] { return ctl.lastStatus().armed; });

    page->setSelectedStandards({QStringLiteral("carbon tetrachloride"), QStringLiteral("chloroform")});
    for (const QString& s : {QStringLiteral("carbon tetrachloride"), QStringLiteral("chloroform")}) {
        page->measureStandard(s);
        if (!waitFor([&] { return page->sample(s) && !ctl.isBusy(); }))
            return 3;
    }
    page->fit();
    page->samplesTable()->setCurrentCell(1, 0);
    shot(&win, out + QStringLiteral("/calibration-wizard") + sfx + QStringLiteral(".png"));
    page->acceptCalibration();

    page->tabs()->setCurrentIndex(1);
    page->quickCheck(QStringLiteral("chloroform"));
    waitFor([&] { return page->lastCheck().has_value() && !ctl.isBusy(); });
    shot(&win, out + QStringLiteral("/calibration-check") + sfx + QStringLiteral(".png"));

    // a one-point (offset) calibration for the history
    page->tabs()->setCurrentIndex(0);
    page->setSelectedStandards({QStringLiteral("chloroform")});
    page->measureStandard(QStringLiteral("chloroform"));
    waitFor([&] { return page->sample(QStringLiteral("chloroform")) && !ctl.isBusy(); });
    page->setMode(CalibrationPage::Mode::Offset);
    page->fit();
    shot(&win, out + QStringLiteral("/calibration-offset") + sfx + QStringLiteral(".png"));
    page->acceptCalibration();

    page->tabs()->setCurrentIndex(2);
    page->historyTable()->selectRow(1);
    shot(&win, out + QStringLiteral("/calibration-history") + sfx + QStringLiteral(".png"));
    page->tabs()->setCurrentIndex(3);
    shot(&win, out + QStringLiteral("/calibration-log") + sfx + QStringLiteral(".png"));

    // calibration fixes: carbon tetrachloride alone (offset; a forced linear correction is refused), then a wrong
    // calibration in use (the old CCl4-only cubic): ethanol finds 2 of 8 bands -> "Match against the factory
    // calibration"
    auto measureOne = [&](const QString& s) {
        page->setSelectedStandards({s});
        page->measureStandard(s);
        return waitFor([&] { return page->sample(s) && !ctl.isBusy(); });
    };
    page->tabs()->setCurrentIndex(0);
    page->revertToFactory();
    page->setMode(CalibrationPage::Mode::Polynomial);
    page->setDegree(0);
    if (!measureOne(QStringLiteral("carbon tetrachloride")))
        return 4;
    page->fit();
    shot(&win, out + QStringLiteral("/calfix-ccl4-offset") + sfx + QStringLiteral(".png"));
    page->setDegree(1);
    shot(&win, out + QStringLiteral("/calfix-ccl4-linear-refused") + sfx + QStringLiteral(".png"));
    page->setDegree(0);
    lr::calib::Result bad;
    const lr::calib::FitOutput cubic = lr::calib::fit(page->sample(QStringLiteral("carbon tetrachloride"))->matches, 3);
    bad.poly = cubic.poly;
    bad.residuals = cubic.residuals;
    bad.rms = cubic.rms;
    bad.standards = {QStringLiteral("carbon tetrachloride")};
    bad.date = QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm"));
    bad.degree = 3;
    page->rejectCalibration();
    calstore::accept(bad, 532, QStringLiteral("SIMULATOR"), QStringLiteral("polynomial"));
    page->refreshHistory();
    page->makeActive(0);
    page->setAutoFactoryRetry(false);
    if (!measureOne(QStringLiteral("ethanol")))
        return 5;
    page->fit();
    shot(&win, out + QStringLiteral("/calfix-wrong-start") + sfx + QStringLiteral(".png"));
    page->matchAgainstFactory();
    shot(&win, out + QStringLiteral("/calfix-factory-match") + sfx + QStringLiteral(".png"));
    ctl.shutdown();
    return 0;
}
