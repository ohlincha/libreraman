// Command line (Python: libreramen/__main__.py). Same commands, options and output lines:
//   LibreRamenCLI status | init | dark [-e 1] [-n 1]
//   LibreRamenCLI acquire [-e 1] [-n 1] [--channel 532|1064] [--power 50] [-o file.csv]
//   LibreRamenCLI series  [-e 1] [-n 1] --interval 10 --count 12 [--background each|once] [-o file.csv]
//   LibreRamenCLI shutdown                     (vendor Shutdown: USB, OFF)
//   LibreRamenCLI drivers                      (Windows: repair the USB drivers, elevated)
//   LibreRamenCLI gui [--sim]                  (starts the LibreRamen application)
//   LibreRamenCLI watchdog --port P --parent PID   (also: --watchdog ..., as started by WatchdogClient)
//   LibreRamenCLI calibrate --standard chloroform [--standard ethanol ...] [-e 1 -n 5] [--degree 3]
//         (measures each standard in turn, asking you to swap samples and press ARM)
//   LibreRamenCLI calibrate --standard chloroform --from chcl3.csv ...   (offline, from saved spectra)
//   LibreRamenCLI standards                    (list the reference standards)
// Options: --ini BS_Device_Settings.ini (calibration; default: built-in values of the development unit);
//          --sim: use the simulator (its ARM button is pressed for you), --speed: simulator time factor.
//          --json: machine-readable result on stdout (status, dark, acquire, series, standards; messages go to
//          stderr), exit code 0 ok, 1 error, 2 usage, 130 stopped.
//   LibreRamenCLI --remote COMMAND ...         talks to the running application (remote.h; --remote -h)
// selftest needs the GUI port and is not available yet.
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QThread>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <iostream>
#include <string>
#include <map>
#include <optional>
#include <thread>

#if defined(Q_OS_WIN)
#include <windows.h>
#else
#include <csignal>
#endif

#include "core/calibrate.h"
#include "core/calibration.h"
#include "core/errors.h"
#include "core/instrument.h"
#include "core/pyformat.h"
#include "core/series.h"
#include "core/simulator.h"
#include "core/standards.h"
#include "core/watchdog.h"
#include "core/windrivers.h"
#include "remote.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace {

namespace py = lr::py;
std::atomic<bool> g_stop{false};
bool g_json = false;                          // --json: results as JSON on stdout, messages on stderr

#if defined(Q_OS_WIN)
BOOL WINAPI onConsoleCtrl(DWORD) {
    g_stop = true;
    return TRUE;
}
#else
extern "C" void onSignal(int) { g_stop = true; }
#endif

void out(const QString& s) {
    std::FILE* f = g_json ? stderr : stdout;
    std::fputs((s + "\n").toUtf8().constData(), f);
    std::fflush(f);
}

void jsonOut(const QJsonObject& o) {
    std::fputs(QJsonDocument(o).toJson(QJsonDocument::Indented).constData(), stdout);
    std::fflush(stdout);
}

// "error: ..." line, or {"error": ...} with --json
void errorOut(const QString& msg) {
    if (g_json)
        jsonOut(QJsonObject{{"error", msg}});
    else
        out("error: " + msg);
}

const QStringList COMMANDS = {"gui",    "selftest",  "watchdog",  "status",   "init",   "dark",   "acquire",
                              "series", "calibrate", "standards", "shutdown", "drivers"};

const char* USAGE =
    "usage: libreramen [-h] [-e EXPOSURE] [-n AVERAGES] [--channel {532,1064}] [--power POWER]\n"
    "                  [--interval INTERVAL] [--count COUNT] [--background {each,once}] [--ini INI]\n"
    "                  [-o OUTPUT] [--standard STANDARD] [--from FROM_FILES] [--degree DEGREE] [--sim]\n"
    "                  [--speed SPEED] [--screenshot SCREENSHOT] [--port PORT] [--parent PARENT] [--json]\n"
    "                  {gui,selftest,watchdog,status,init,dark,acquire,series,calibrate,standards,shutdown,drivers}\n"
    "       libreramen --remote COMMAND ...   (the running application; --remote -h for help)\n";

[[noreturn]] void usageError(const QString& msg) {
    std::fputs(USAGE, stderr);
    std::fputs(("libreramen: error: " + msg + "\n").toUtf8().constData(), stderr);
    std::exit(2);
}

struct Args {
    QString cmd;
    double exposure = 1.0;
    int averages = 1;
    int channel = 532;
    double power = 0.0;                       // 0 = not given (Python None)
    double interval = 10;
    int count = 10;
    QString background = "each";
    QString ini, output, screenshot, port;
    QStringList standards, fromFiles;
    int degree = -1;
    bool sim = false;
    double speed = 1.0;
    qint64 parent = 0;
};

// argparse-like: "--opt value", "--opt=value", "-e value", "-e1.5"; errors exit with code 2.
Args parse(const QStringList& argv) {
    Args a;
    const std::map<QString, QString> alias = {{"-e", "--exposure"}, {"-n", "--averages"}, {"-o", "--output"}};
    const QStringList withValue = {"--exposure", "--averages",   "--channel", "--power",      "--interval",
                                   "--count",    "--background", "--ini",     "--output",     "--standard",
                                   "--from",     "--degree",     "--speed",   "--screenshot", "--port",
                                   "--parent"};
    auto num = [&](const QString& opt, const QString& v, bool integer) {
        bool ok = false;
        const double d = integer ? double(v.toLongLong(&ok)) : v.toDouble(&ok);
        if (!ok)
            usageError("argument " + opt + ": invalid " + (integer ? "int" : "float") + " value: " + py::repr(v));
        return d;
    };
    QStringList positional;
    for (int i = 0; i < argv.size(); ++i) {
        QString o = argv[i], v;
        bool hasValue = false;
        if (o == "-h" || o == "--help") {
            std::fputs(USAGE, stdout);
            std::exit(0);
        }
        if (!o.startsWith('-') || o == "-") {
            positional << o;
            continue;
        }
        if (o.startsWith("--") && o.contains('=')) {
            v = o.mid(o.indexOf('=') + 1);
            o = o.left(o.indexOf('='));
            hasValue = true;
        } else if (!o.startsWith("--") && o.size() > 2) {   // -e1.5
            v = o.mid(2);
            o = o.left(2);
            hasValue = true;
        }
        if (alias.count(o)) o = alias.at(o);
        if (o == "--sim") {
            a.sim = true;
            continue;
        }
        if (o == "--json") {
            g_json = true;
            continue;
        }
        if (!withValue.contains(o)) usageError("unrecognized arguments: " + argv[i]);
        if (!hasValue) {
            if (i + 1 >= argv.size()) usageError("argument " + o + ": expected one argument");
            v = argv[++i];
        }
        if (o == "--exposure") a.exposure = num(o, v, false);
        else if (o == "--averages") a.averages = int(num(o, v, true));
        else if (o == "--channel") {
            a.channel = int(num(o, v, true));
            if (a.channel != 532 && a.channel != 1064)
                usageError("argument --channel: invalid choice: " + v + " (choose from 532, 1064)");
        } else if (o == "--power") a.power = num(o, v, false);
        else if (o == "--interval") a.interval = num(o, v, false);
        else if (o == "--count") a.count = int(num(o, v, true));
        else if (o == "--background") {
            if (v != "each" && v != "once")
                usageError("argument --background: invalid choice: " + py::repr(v) + " (choose from 'each', 'once')");
            a.background = v;
        } else if (o == "--ini") a.ini = v;
        else if (o == "--output") a.output = v;
        else if (o == "--standard") a.standards << v;
        else if (o == "--from") a.fromFiles << v;
        else if (o == "--degree") a.degree = int(num(o, v, true));
        else if (o == "--speed") a.speed = num(o, v, false);
        else if (o == "--screenshot") a.screenshot = v;
        else if (o == "--port") a.port = v;
        else if (o == "--parent") a.parent = qint64(num(o, v, true));
    }
    if (positional.isEmpty()) usageError("the following arguments are required: cmd");
    if (positional.size() > 1) usageError("unrecognized arguments: " + positional.mid(1).join(' '));
    a.cmd = positional[0];
    if (!COMMANDS.contains(a.cmd)) {
        QStringList q;
        for (const auto& c : COMMANDS) q << py::repr(c);
        usageError("argument cmd: invalid choice: " + py::repr(a.cmd) + " (choose from " + q.join(", ") + ")");
    }
    return a;
}

QString guiExecutable() {
    const QString dir = QCoreApplication::applicationDirPath();
    for (const QString& p : {dir + "/LibreRamen.exe", dir + "/../LibreRamen.exe", dir + "/LibreRamen",
                             dir + "/LibreRamen.app/Contents/MacOS/LibreRamen"})
        if (QFileInfo(p).isFile() && QFileInfo(p).isExecutable()) return QDir::cleanPath(p);
    return {};
}

QString padLeft(const QString& s, int width) { return s.size() >= width ? s : QString(width - s.size(), ' ') + s; }

double mean(const lr::Vec& v) {
    double s = 0;                             // sum() in order, as Python
    for (double x : v) s += x;
    return s / double(v.size());
}

QString padRight(const QString& s, int width) { return s.size() >= width ? s : s + QString(width - s.size(), ' '); }

// Python input(prompt): prompt without newline, one line from stdin (EOF: empty answer).
QString input(const QString& prompt) {
    std::fputs(prompt.toUtf8().constData(), stdout);
    std::fflush(stdout);
    std::string line;
    if (!std::getline(std::cin, line)) return {};
    return QString::fromUtf8(line.c_str()).remove('\r');
}

// `standards`: name, kind, source and band positions, sorted by name (Python sorted(STANDARDS.items())).
int listStandards() {
    std::vector<const lr::standards::Standard*> all;
    for (const auto& s : lr::standards::all()) all.push_back(&s);
    std::sort(all.begin(), all.end(), [](auto* a, auto* b) { return a->name < b->name; });
    if (g_json) {
        QJsonArray list;
        for (const auto* s : all) {
            QJsonArray bands;
            for (const auto& b : s->bands) bands.append(b.position);
            list.append(QJsonObject{{"name", s->name}, {"kind", s->kind}, {"source", s->source}, {"bands", bands}});
        }
        jsonOut(QJsonObject{{"standards", list}});
        return 0;
    }
    for (const auto* s : all) {
        QStringList bands;
        for (const auto& b : s->bands) bands << py::g(b.position);
        out(padRight(s->name, 28) + " " + padRight(s->kind, 6) + " " + padRight(s->source, 4) + "  " +
            bands.join(", "));
    }
    return 0;
}

std::optional<int> degreeOf(const Args& a) { return a.degree < 0 ? std::nullopt : std::optional<int>(a.degree); }

// Spectrum intensities in calibration-pixel order (what calibrate works with; Python _pixel_order).
lr::Vec pixelOrder(const lr::Spectrum& sp) {
    if (sp.pixels.size() != sp.intensity.size()) return sp.intensity;
    std::vector<std::pair<double, double>> pv;
    for (std::size_t i = 0; i < sp.pixels.size(); ++i) pv.emplace_back(sp.pixels[i], sp.intensity[i]);
    std::sort(pv.begin(), pv.end());
    lr::Vec y;
    for (const auto& p : pv) y.push_back(p.second);
    return y;
}

// Column 1 of a saved spectrum CSV, one row per pixel (Python offline_calibration: '#' lines skipped,
// header row dropped).
lr::Vec readIntensities(const QString& path) {
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly | QIODevice::Text))
        throw lr::Error(QString("[Errno 2] No such file or directory: %1").arg(py::repr(path)).toStdString());
    lr::Vec y;
    bool header = true;
    for (const QString& line : QString::fromUtf8(f.readAll()).split('\n')) {
        if (line.startsWith('#') || line.trimmed().isEmpty()) continue;
        if (header) {
            header = false;
            continue;
        }
        const QStringList cells = line.split(',');
        bool ok = false;
        const double v = cells.size() > 1 ? cells[1].trimmed().toDouble(&ok) : 0.0;
        if (!ok)
            throw lr::Error(QString("%1: could not read the intensity in row %2")
                                .arg(QFileInfo(path).fileName(), py::repr(line.trimmed()))
                                .toStdString());
        y.push_back(v);
    }
    return y;
}

// `calibrate --from file ...` (Python offline_calibration).
int offlineCalibration(const Args& a) {
    const lr::Calibration cal = a.ini.isEmpty() ? lr::defaultCalibration() : lr::loadIni(a.ini);
    if (a.fromFiles.size() != a.standards.size()) {
        out("give one --from file per --standard");
        return 1;
    }
    if (!cal.channels.count(a.channel))
        throw lr::Error(QString("no calibration for the %1 nm channel").arg(a.channel).toStdString());
    const lr::ChannelCal ch = cal.channels.at(a.channel);
    const lr::calib::ShiftFn shift = [ch](double p) { return ch.shift(p); };
    std::vector<std::pair<lr::Vec, QString>> samples;
    for (int i = 0; i < a.standards.size(); ++i) samples.emplace_back(readIntensities(a.fromFiles[i]), a.standards[i]);
    const lr::calib::Result r = lr::calib::calibrate(samples, shift, degreeOf(a));
    out(lr::calib::report(r, shift));
    if (!a.output.isEmpty()) out("saved " + lr::calib::save(r, a.channel, cal.serial, a.output));
    return 0;
}

// `calibrate --standard ...` with the instrument (Python live_calibration).
void liveCalibration(lr::Instrument& c, const Args& a) {
    if (a.standards.isEmpty()) throw lr::Error("give at least one --standard");
    std::vector<std::pair<lr::Vec, QString>> samples;
    for (const QString& name : a.standards) {
        input("Put in " + name + ", close the compartment, press ARM, then press Enter here… ");
        if (auto* sim = dynamic_cast<lr::SimControls*>(&c)) {     // simulator: load the standard, press ARM
            QString sample = name;
            try {
                sample = lr::standards::find(name).name;
            } catch (const lr::Error&) {
            }
            if (sim->sampleNames().contains(sample)) sim->setSample(sample);
            sim->closeCompartment();
            sim->pressArm();
        }
        while (!c.status().armed) {
            if (g_stop) throw lr::Stopped();
            QThread::msleep(500);
        }
        lr::AcquireOptions o;
        o.exposure_s = a.exposure;
        o.averages = std::max(a.averages, 3);
        o.stop = &g_stop;
        samples.emplace_back(pixelOrder(c.acquire(o)), name);
    }
    const int channel = c.channel();
    const lr::ChannelCal ch = c.calibration().channels.at(channel);
    const lr::calib::ShiftFn before = [ch](double p) { return ch.shift(p); };
    const lr::calib::Result r = lr::calib::calibrate(samples, before, degreeOf(a));
    out(lr::calib::report(r, before));
    if (input("Save this calibration? [y/N] ").trimmed().toLower() == "y")
        out("saved " + lr::calib::save(r, channel, c.calibration().serial));
}

int runInstrument(const Args& a) {
    const lr::Calibration cal = a.ini.isEmpty() ? lr::defaultCalibration() : lr::loadIni(a.ini);
    std::unique_ptr<lr::Instrument> c = a.sim ? lr::makeSimulator(cal, out, a.speed) : lr::makeCora(cal, out);

    // Ctrl-C / SIGTERM / SIGHUP (Python: KeyboardInterrupt, then close() in finally): laser off right away;
    // the running command stops at its next check; if it doesn't end within 10 s, exit anyway.
    std::atomic<bool> done{false};
    std::thread watcher([&] {
        while (!done) {
            if (g_stop) {
                c->laserOffNow();
                for (int i = 0; i < 200 && !done; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(50));
                if (!done) {
                    out("stopped");
                    std::_Exit(130);
                }
                return;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    });
    struct Join {
        std::atomic<bool>& d;
        std::thread& t;
        ~Join() {
            d = true;
            t.join();
        }
    } join{done, watcher};

    int rc = 0;
    try {
        c->connect();
        if (a.sim)
            if (auto* s = dynamic_cast<lr::SimControls*>(c.get())) s->pressArm();   // nobody at the simulator
        const lr::Status st = c->status();
        if (g_json && a.cmd == "status") {
            QJsonArray specs;
            for (int sp : st.spectrometers) specs.append(sp);
            jsonOut(QJsonObject{{"armed", st.armed}, {"compartment_open", st.compartment_open},
                                {"laser_on", st.laser_on}, {"spectrometers", specs}, {"simulated", a.sim}});
        }
        out("armed: " + lr::py::str(st.armed) + ", compartment open: " + lr::py::str(st.compartment_open) +
            ", spectrometers: " + lr::py::str(st.spectrometers));
        if (a.cmd == "shutdown") {
            c->close(true);
            out("Shutdown sent");
            return 0;
        }
        if (a.cmd != "status") {
            if (c->needsInitialize() || a.cmd == "init") c->initialize();
            else c->resume();                 // instrument already initialized: assume the 532 nm path
            if (a.channel != c->channel()) c->selectChannel(a.channel);
            const QString stamp = QDateTime::currentDateTime().toString("yyyyMMdd_HHmmss");
            if (a.cmd == "dark") {
                const lr::Vec d = c->dark(a.exposure, a.averages);
                const auto [mn, mx] = std::minmax_element(d.begin(), d.end());
                out("dark: " + QString::number(d.size()) + " px, mean " + lr::py::fixed(mean(d), 0) + ", min " +
                    lr::py::fixed(*mn, 0) + ", max " + lr::py::fixed(*mx, 0));
                if (g_json)
                    jsonOut(QJsonObject{{"pixels", int(d.size())}, {"mean", mean(d)}, {"min", *mn}, {"max", *mx},
                                        {"exposure_s", a.exposure}, {"averages", a.averages}});
            } else if (a.cmd == "acquire") {
                lr::AcquireOptions o;
                o.exposure_s = a.exposure;
                o.averages = a.averages;
                o.power_mw = a.power;
                o.stop = &g_stop;
                lr::Spectrum sp = c->acquire(o);
                const QString path = a.output.isEmpty() ? "cora_" + QString::number(a.channel) + "_" + stamp + ".csv"
                                                        : a.output;
                sp.saveCsv(path);
                const auto i = size_t(std::max_element(sp.intensity.begin(), sp.intensity.end()) - sp.intensity.begin());
                out("saved " + path + ": " + QString::number(sp.shift.size()) + " points, max " +
                    lr::py::fixed(sp.intensity[i], 0) + " at " + lr::py::fixed(sp.shift[i], 0) + " cm-1");
                if (g_json)
                    jsonOut(QJsonObject{{"file", path}, {"points", int(sp.shift.size())}, {"max", sp.intensity[i]},
                                        {"max_at", sp.shift[i]}, {"channel", sp.channel},
                                        {"exposure_s", sp.exposure_s}, {"averages", sp.averages}});
            } else if (a.cmd == "series") {
                const QString path = a.output.isEmpty()
                                         ? "cora_series_" + QString::number(a.channel) + "_" + stamp + ".csv"
                                         : a.output;
                lr::runSeries(*c, path, a.exposure, a.interval, a.count, a.averages, a.background, a.power, &g_stop,
                              [&](int k, double e, const lr::Spectrum& sp) {
                                  out("  " + QString::number(k + 1) + "/" + QString::number(a.count) + " at " +
                                      padLeft(lr::py::fixed(e, 1), 6) + " s: mean " +
                                      lr::py::fixed(mean(sp.intensity), 0));
                              });
                out("saved " + path);
                if (g_json) jsonOut(QJsonObject{{"file", path}, {"count", a.count}});
            } else if (a.cmd == "calibrate") {
                liveCalibration(*c, a);
            }
            if (g_stop) throw lr::Stopped();
            if (a.channel == 1064) c->selectChannel(532);
        }
    } catch (const lr::Stopped&) {
        if (g_json) jsonOut(QJsonObject{{"error", "stopped"}});
        else out("stopped");
        rc = 130;
    } catch (const lr::Error& e) {
        errorOut(QString::fromUtf8(e.what()));
        rc = 1;
    }
    try {
        c->close();
    } catch (const lr::Error& e) {
        out(QString("error: ") + QString::fromUtf8(e.what()));
        if (rc == 0) rc = 1;
    }
    return rc;
}

}  // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    const QStringList args = app.arguments().mid(1);
    if (lr::watchdog::isWatchdogCall(args)) {           // started by WatchdogClient
        const int code = lr::watchdog::runFromArgs(args.mid(1));
        std::fflush(nullptr);
        std::_Exit(code);                     // (the stdin reader thread may still be blocked)
    }
    if (args.contains("--remote")) {                    // the running application, through its local API
        QStringList rest = args;
        rest.removeAll("--remote");
        return lr::cli::runRemote(rest);
    }
    const Args a = parse(args);
    if (a.cmd == "watchdog") {
        const int code = lr::watchdog::run(a.port, {}, a.parent);
        std::fflush(nullptr);
        std::_Exit(code);
    }
    if (a.cmd == "standards") return listStandards();
    if (a.cmd == "calibrate" && !a.fromFiles.isEmpty()) {
        try {
            return offlineCalibration(a);
        } catch (const lr::Error& e) {
            out(QString("error: ") + QString::fromUtf8(e.what()));
            return 1;
        }
    }
    if (a.cmd == "selftest") {
        out(a.cmd + ": not yet available in the C++ version");
        return 1;
    }
    if (a.cmd == "drivers") {
        try {
            auto proc = lr::windrivers::repair(true);
            std::optional<int> code;
            while (!(code = proc->poll())) QThread::msleep(500);
            out("driver set-up finished (code " + QString::number(*code) +
                "); details in %ProgramData%\\LibreRamen\\drivers.log");
            return *code;
        } catch (const lr::Error& e) {
            out(QString("error: ") + QString::fromUtf8(e.what()));
            return 1;
        }
    }
    if (a.cmd == "gui") {
        const QString exe = guiExecutable();
        if (exe.isEmpty()) {
            out("error: the LibreRamen application was not found next to " + QCoreApplication::applicationFilePath());
            return 1;
        }
        QStringList extra;
        if (a.sim) extra << "--sim";
        if (!a.screenshot.isEmpty()) extra << "--screenshot" << a.screenshot;
        return QProcess::execute(exe, extra);
    }
#if defined(Q_OS_WIN)
    SetConsoleCtrlHandler(onConsoleCtrl, TRUE);
#else
    for (int sig : {SIGINT, SIGTERM, SIGHUP}) std::signal(sig, onSignal);
#endif
    try {
        return runInstrument(a);
    } catch (const lr::Error& e) {            // e.g. --ini unreadable, simulator data missing
        errorOut(QString::fromUtf8(e.what()));
        return 1;
    }
}
