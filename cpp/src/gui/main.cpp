// LibreRaman graphical program (Python: gui/app.py main, selftest.py, the gui/selftest/watchdog parts of
// __main__.py).
//   LibreRaman [--sim] [--dark | --light] [--screenshot FILE [--demo single|series|focus] [--area NAME]]
//              [--api-port N] [--no-api]   local API (default: settings, port 8765; see apiserver.h)
//   LibreRaman --selftest        GUI + simulator self-test (offscreen unless QT_QPA_PLATFORM is set); exit 0 = OK
//   LibreRaman --watchdog --port P --parent PID     laser watchdog process (started by the program itself)
// --screenshot saves the window after 15 s and quits (with --sim a spectrum of CCl4 is measured first and
// saved in <temp>/libreraman-screenshot; --demo series: a series instead; --window-size WxH: window size;
// --area Data|Library|Analysis|Calibration: that area is shown when the picture is taken).
//
// Single instance: a second LibreRaman of the same user asks the running one to come to the front and exits
// (per-user QLocalServer "libreraman-gui-<user>"). Not for --sim, --screenshot and --selftest, which may run
// next to the real program (they do not get the local API port then; the REMOTE tile says why).
#include "core/legacyname.h"
#include "gui/desktopentry.h"
#include "controller.h"
#include "decimalpoint.h"
#include "icon.h"
#include <QPushButton>

#include "focuswindow.h"
#include "mainwindow.h"
#include "measurepage.h"
#include "page.h"
#include "splash.h"
#include "theme.h"

#include "core/watchdog.h"

#include <QApplication>
#include <QComboBox>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFileInfo>
#include <QLineEdit>
#include <QLocalServer>
#include <QLocalSocket>
#include <QSettings>
#include <QSpinBox>
#include <QRegularExpression>
#include <QTimer>

#include <cmath>
#include <cstdio>
#include <cstdlib>

using namespace lr::gui;

namespace {

QString argValue(const QStringList& args, const QString& name) {
    const int i = int(args.indexOf(name));
    return i >= 0 && i + 1 < args.size() ? args.at(i + 1) : QString();
}

void printLine(const QString& s) {
    std::printf("%s\n", s.toLocal8Bit().constData());
    std::fflush(stdout);
}

// Python selftest.py: start the GUI against the simulator, take one spectrum, quit. 0 = OK.
int selftest(QApplication& app) {
    setDialogsEnabled(false);
    Controller ctl;
    ctl.setSimulatorSpeed(20);
    MainWindow win(&ctl);
    win.show();
    int spectra = 0;
    QStringList errors;
    QString file;
    QObject::connect(&ctl, &Controller::error, &app, [&](const QString& e) { errors << e; });
    QObject::connect(&ctl, &Controller::connectFailed, &app, [&](const QString& e) { errors << e; });
    QObject::connect(&ctl, &Controller::spectrum, &app, [&](const lr::Spectrum& sp) {
        ++spectra;
        file = sp.metaValue(QStringLiteral("file"));
    });
    QObject::connect(&ctl, &Controller::connected, &app, [&] {
        QTimer::singleShot(200, &app, [&] {
            ctl.simPressArm();
            MeasurePage* m = win.measurePage();
            m->exposureBox()->setValue(0.2);
            m->averagesBox()->setValue(1);
            m->setFolder(QDir(QDir::homePath()).filePath(QStringLiteral("libreraman-smoke")));
            m->measureOnce();
        });
    });
    ctl.connectInstrument(true);
    QTimer::singleShot(25000, &app, &QApplication::quit);
    QTimer t;
    QObject::connect(&t, &QTimer::timeout, &app, [&] {
        if (spectra || !errors.isEmpty())
            QApplication::quit();
    });
    t.start(200);
    app.exec();
    win.close();
    const bool saved = !file.isEmpty() && QFileInfo::exists(file);
    if (spectra && !saved)
        errors << QStringLiteral("spectrum not saved");
    printLine(QStringLiteral("selftest: spectra %1, file %2, errors [%3]")
                  .arg(spectra)
                  .arg(file.isEmpty() ? QStringLiteral("-") : file, errors.join(QStringLiteral("; "))));
    const bool ok = spectra && saved && errors.isEmpty();
    printLine(ok ? QStringLiteral("selftest: OK") : QStringLiteral("selftest: FAILED"));
    return ok ? 0 : 1;
}

QString instanceKey() {
    QString user = qEnvironmentVariable("USER");
    if (user.isEmpty())
        user = qEnvironmentVariable("USERNAME");
    user.replace(QRegularExpression(QStringLiteral("[^A-Za-z0-9_.-]")), QStringLiteral("_"));
    return QStringLiteral("libreraman-gui-") + (user.isEmpty() ? QStringLiteral("user") : user);
}

// True if another instance was found (and asked to show its window).
bool raiseRunningInstance(const QString& key) {
    QLocalSocket s;
    s.connectToServer(key);
    if (!s.waitForConnected(1000))
        return false;
    s.write("raise\n");
    s.waitForBytesWritten(1000);
    s.waitForReadyRead(1000);                               // "ok" (the first instance answers)
    s.disconnectFromServer();
    return true;
}

}  // namespace

int main(int argc, char** argv) {
    QStringList rawArgs;
    for (int i = 1; i < argc; ++i)
        rawArgs << QString::fromLocal8Bit(argv[i]);
    if (lr::watchdog::isWatchdogCall(rawArgs)) {          // started by WatchdogClient: no GUI at all
        QCoreApplication core(argc, argv);
        const int code = lr::watchdog::runFromArgs(rawArgs.mid(1));
        std::fflush(nullptr);
        std::_Exit(code);                                   // (the stdin reader thread may still be blocked)
    }
    const bool selftestMode =
        rawArgs.contains(QStringLiteral("--selftest")) || rawArgs.contains(QStringLiteral("selftest"));
    if (selftestMode && qEnvironmentVariableIsEmpty("QT_QPA_PLATFORM"))
        qputenv("QT_QPA_PLATFORM", "offscreen");

    QApplication app(argc, argv);
    lr::gui::useDecimalPoint(app);                     // "1.5 s", not "1,5 s" on European systems
    QApplication::setApplicationName(QStringLiteral("LibreRaman"));
    QApplication::setOrganizationName(QStringLiteral("LibreRaman"));
    QApplication::setApplicationVersion(QStringLiteral(LIBRERAMAN_VERSION));
    QGuiApplication::setDesktopFileName(QStringLiteral("io.github.ohlincha.libreraman"));   // Wayland app id
    QApplication::setWindowIcon(appIcon());
    lr::legacy::env();                                  // carried over from LibreRamen (<= 0.3.9)
    lr::legacy::configDir();
    lr::legacy::appData();
    lr::gui::desktop::migrateOldLauncher();
    const QStringList args = app.arguments().mid(1);
    theme::apply(app, args.contains(QStringLiteral("--dark"))    ? theme::Mode::Dark
                      : args.contains(QStringLiteral("--light")) ? theme::Mode::Light
                                                                 : theme::Mode::Auto);
    if (selftestMode)
        return selftest(app);

    const bool simulate = args.contains(QStringLiteral("--sim"));
    const QString shot = argValue(args, QStringLiteral("--screenshot"));
    const QString demo = argValue(args, QStringLiteral("--demo"));
    if (!shot.isEmpty())
        setDialogsEnabled(false);

    QLocalServer instanceServer;
    if (!simulate && shot.isEmpty()) {
        const QString key = instanceKey();
        if (raiseRunningInstance(key)) {
            printLine(QStringLiteral("LibreRaman is already running; its window was brought to the front."));
            return 0;
        }
        QLocalServer::removeServer(key);                    // stale (Unix: after a crash)
        instanceServer.setSocketOptions(QLocalServer::UserAccessOption);
        instanceServer.listen(key);
    }

    Splash splash;
    splash.show();
    splash.message(QStringLiteral("Starting…"));
    QApplication::processEvents();
    Controller ctl;
    MainWindow win(&ctl);
    QObject::connect(&instanceServer, &QLocalServer::newConnection, &win, [&] {
        while (QLocalSocket* c = instanceServer.nextPendingConnection()) {
            QObject::connect(c, &QLocalSocket::readyRead, c, [c, &win] {
                if (c->readAll().contains("raise")) {
                    win.raiseWindow();
                    c->write("ok\n");
                    c->flush();
                }
            });
            QObject::connect(c, &QLocalSocket::disconnected, c, &QObject::deleteLater);
        }
    });
    win.setOfferDesktopIcon(shot.isEmpty());
    const QString winSize = argValue(args, QStringLiteral("--window-size"));
    if (!winSize.isEmpty()) {
        const QStringList wh = winSize.split(QLatin1Char('x'));
        if (wh.size() == 2 && wh[0].toInt() > 0 && wh[1].toInt() > 0)
            win.resize(wh[0].toInt(), wh[1].toInt());
    }

    QObject::connect(&ctl, &Controller::initProgress, &splash, [&](double f) {
        splash.setProgress(f);
        splash.message(QStringLiteral("Initialising… %1 %").arg(int(f * 100)));
    });
    QObject::connect(&ctl, &Controller::connected, &splash, [&](const InstrumentInfo& info) {
        splash.message(QStringLiteral("%1 found (S/N %2)").arg(info.model, info.serial));
        QTimer::singleShot(600, &win, [&] {
            if (!win.isVisible()) {
                win.show();
                splash.finish(&win);
            }
        });
    });
    QObject::connect(&ctl, &Controller::connectFailed, &win, [&] {
        if (!win.isVisible()) {
            win.show();
            splash.finish(&win);
        }
    });
    QObject::connect(&splash, &Splash::skipped, &win, [&] { win.show(); });

    if (!shot.isEmpty()) {                                  // documentation/testing: save the window as an image
        if (simulate) {
            QObject::connect(&ctl, &Controller::connected, &win, [&] {
                QTimer::singleShot(800, &win, [&] {
                    ctl.simPressArm();
                    win.simulatorSample()->setCurrentText(QStringLiteral("carbon tetrachloride"));
                    MeasurePage* m = win.measurePage();
                    m->setFolder(QDir(QDir::tempPath()).filePath(QStringLiteral("libreraman-screenshot")));
                    m->sampleEdit()->setText(QStringLiteral("CCl4"));
                    m->exposureBox()->setValue(0.5);
                    m->averagesBox()->setValue(1);
                    if (demo == QLatin1String("focus")) {   // focus mode running (the accessory: the lid reads closed)
                        ctl.simSetLidAccessory(true);
                        m->openFocus();
                        if (auto* f = m->findChild<FocusWindow*>())
                            QTimer::singleShot(300, f, [f] { f->startButton()->click(); });
                    } else if (demo == QLatin1String("series")) {
                        m->intervalUnitBox()->setCurrentText(QStringLiteral("s"));
                        m->intervalBox()->setValue(std::max(2.5, std::ceil(m->minInterval() * 10) / 10));
                        m->countBox()->setValue(40);
                        m->startSeries();
                    } else {
                        m->measureOnce();
                    }
                });
            });
        }
        QTimer::singleShot(15000, &win, [&] {
            if (!win.isVisible())
                win.show();
            if (const int area = int(pageAreas().indexOf(argValue(args, QStringLiteral("--area")))); area >= 0) {
                win.showArea(area);
                QApplication::processEvents();
            }
            QWidget* target = &win;
            if (auto* f = win.findChild<FocusWindow*>(); f && f->isVisible()) target = f;
            target->grab().save(shot);
            win.close();
            QApplication::quit();
        });
    }
    // local API for the command line, scripts and AI agents (not for screenshots)
    const QSettings settings;
    if (shot.isEmpty() && !args.contains(QStringLiteral("--no-api")) &&
        settings.value(QStringLiteral("api/enabled"), true).toBool()) {
        bool ok = false;
        const int port = argValue(args, QStringLiteral("--api-port")).toInt(&ok);
        win.startApi(ok ? port : -1);
    }
    splash.message(QStringLiteral("Searching for instrument…"));
    QTimer::singleShot(200, &ctl, [&] { win.connectInstrument(simulate); });
    const int rc = app.exec();
    ctl.shutdown(false);                                    // (already done by closeEvent unless quit otherwise)
    return rc;
}
