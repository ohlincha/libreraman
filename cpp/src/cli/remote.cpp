#include "remote.h"

#include "core/apiclient.h"
#include "core/errors.h"
#include "core/fileio.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QSet>
#include <cstdio>
#include <map>

namespace lr::cli {

const char* const REMOTE_USAGE =
    "usage: LibreRamanCLI --remote COMMAND [options] [--json] [--api-file FILE]\n"
    "Talks to the running LibreRaman application (local API on 127.0.0.1).\n"
    "commands:\n"
    "  status                    instrument state (connection, ARM, laser, busy, remote control)\n"
    "  info                      software, instrument and API information\n"
    "  measure [-e S] [-n N] [--sample NAME] [--background each|none] [--prebleach S]\n"
    "          [--channel 532|1064] [--no-wait] [--timeout S]\n"
    "                            one spectrum (needs \"Allow remote measurements\" at the instrument)\n"
    "  series [-e S] [-n N] [--sample NAME] [--background each|once|none] --interval S\n"
    "         (--count N | --duration S) [--delay S] [--fast]   start a time series\n"
    "  progress                  progress of the running series\n"
    "  job ID                    state of a job started through the API\n"
    "  stop                      stop and switch the laser off (always allowed)\n"
    "  spectra [--limit N]       recent spectra\n"
    "  spectrum ID [-o FILE.csv] one spectrum (data), optionally saved as CSV\n"
    "  peaks ID [--snr X]        peaks of a spectrum (with the GUI's processing settings)\n"
    "  standards                 reference standards\n"
    "  log [--since N]           log lines of the application\n"
    "--json prints the API's JSON reply. Exit codes: 0 ok, 1 failed, 2 usage, 3 LibreRaman not running,\n"
    "4 not allowed (token / remote measurements off), 5 busy, 130 stopped.\n";

namespace {

bool g_json = false;

void print(const QString& s) {
    std::fputs((s + QLatin1Char('\n')).toUtf8().constData(), stdout);
    std::fflush(stdout);
}

void printJson(const QJsonValue& v) {
    const QByteArray b = v.isArray() ? QJsonDocument(v.toArray()).toJson(QJsonDocument::Indented)
                                     : QJsonDocument(v.toObject()).toJson(QJsonDocument::Indented);
    std::fputs(b.constData(), stdout);
    std::fflush(stdout);
}

[[noreturn]] void usage(const QString& msg) {
    std::fputs(REMOTE_USAGE, stderr);
    std::fputs(("LibreRamanCLI: error: " + msg + "\n").toUtf8().constData(), stderr);
    std::exit(kUsage);
}

QString yesNo(bool b) { return b ? QStringLiteral("yes") : QStringLiteral("no"); }

// Exit code and message for a failed request.
int failure(const lr::api::Reply& r) {
    if (g_json)
        printJson(r.json.isObject() ? r.json : QJsonObject{{QStringLiteral("error"), r.error}, {QStringLiteral("status"), r.status}});
    else
        std::fputs(("error: " + r.error + "\n").toUtf8().constData(), stderr);
    if (r.status == 0)
        return kNotRunning;
    if (r.status == 401 || r.status == 403)
        return kDenied;
    if (r.status == 409)
        return kBusy;
    return kFailed;
}

int jobExit(const QJsonObject& job) {
    const QString st = job.value(QStringLiteral("state")).toString();
    if (st == QLatin1String("done") || st == QLatin1String("running"))
        return kOk;
    return st == QLatin1String("stopped") ? kStopped : kFailed;
}

QString describeJob(const QJsonObject& j) {
    QStringList ids;
    for (const QJsonValue& v : j.value(QStringLiteral("spectra")).toArray())
        ids << QString::number(v.toInt());
    QString s = QStringLiteral("job %1 (%2): %3").arg(j.value(QStringLiteral("id")).toInt())
                    .arg(j.value(QStringLiteral("kind")).toString(), j.value(QStringLiteral("state")).toString());
    if (!ids.isEmpty())
        s += QStringLiteral(", spectra ") + ids.join(QStringLiteral(", "));
    if (j.contains(QStringLiteral("error")))
        s += QStringLiteral(", error: ") + j.value(QStringLiteral("error")).toString();
    return s;
}

}  // namespace

int runRemote(const QStringList& argv) {
    // ---- options
    const std::map<QString, QString> alias = {{"-e", "--exposure"}, {"-n", "--averages"}, {"-o", "--output"}};
    const QSet<QString> withValue = {"--exposure", "--averages", "--sample",   "--background", "--prebleach",
                                     "--channel",  "--interval", "--count",    "--duration",   "--delay",
                                     "--timeout",  "--limit",    "--since",    "--snr",        "--output",
                                     "--api-file", "--client"};
    const QSet<QString> flags = {"--json", "--no-wait", "--fast"};
    QStringList pos;
    std::map<QString, QString> opt;
    for (int i = 0; i < argv.size(); ++i) {
        QString o = argv[i], v;
        bool hasValue = false;
        if (o == "-h" || o == "--help") {
            std::fputs(REMOTE_USAGE, stdout);
            return kOk;
        }
        if (!o.startsWith('-')) {
            pos << o;
            continue;
        }
        if (o.startsWith("--") && o.contains('=')) {
            v = o.mid(o.indexOf('=') + 1);
            o = o.left(o.indexOf('='));
            hasValue = true;
        } else if (!o.startsWith("--") && o.size() > 2) {
            v = o.mid(2);
            o = o.left(2);
            hasValue = true;
        }
        if (alias.count(o))
            o = alias.at(o);
        if (flags.contains(o)) {
            opt[o] = "1";
            continue;
        }
        if (!withValue.contains(o))
            usage("unrecognized argument: " + argv[i]);
        if (!hasValue) {
            if (i + 1 >= argv.size())
                usage("argument " + o + ": expected one argument");
            v = argv[++i];
        }
        opt[o] = v;
    }
    g_json = opt.count("--json") > 0;
    if (pos.isEmpty())
        usage("give a command (status, measure, stop, ...)");
    const QString cmd = pos[0];
    auto has = [&](const char* k) { return opt.count(QString::fromLatin1(k)) > 0; };
    auto num = [&](const char* k, bool integer) -> QJsonValue {
        const QString key = QString::fromLatin1(k);
        bool ok = false;
        const double d = opt.at(key).toDouble(&ok);
        if (!ok || (integer && d != double(qint64(d))))
            usage("argument " + key + ": invalid " + (integer ? "int" : "float") + " value: " + opt.at(key));
        return integer ? QJsonValue(qint64(d)) : QJsonValue(d);
    };
    auto idArg = [&]() {
        bool ok = false;
        const int id = pos.value(1).toInt(&ok);
        if (pos.size() != 2 || !ok)
            usage(cmd + ": give one numeric ID");
        return id;
    };
    if (pos.size() > 1 && cmd != "job" && cmd != "spectrum" && cmd != "peaks")
        usage("unrecognized arguments: " + pos.mid(1).join(' '));

    lr::api::Endpoint ep;
    try {
        ep = lr::api::findServer(has("--api-file") ? opt.at("--api-file") : QString());
    } catch (const lr::Error& e) {
        const QString m = QString::fromUtf8(e.what());
        if (g_json)
            printJson(QJsonObject{{QStringLiteral("error"), m}, {QStringLiteral("status"), 0}});
        else
            std::fputs(("error: " + m + "\n").toUtf8().constData(), stderr);
        return kNotRunning;
    }
    const QString client = has("--client") ? opt.at("--client") : QStringLiteral("cli");
    auto call = [&](const QByteArray& method, const QString& path, const QJsonValue& body = QJsonValue(QJsonValue::Undefined),
                    int timeoutMs = 30000) { return lr::api::request(ep, method, path, body, timeoutMs, client); };

    // ---- commands
    if (cmd == "status") {
        const auto r = call("GET", "/status");
        if (!r.ok())
            return failure(r);
        const QJsonObject s = r.json.toObject();
        if (g_json) {
            printJson(s);
            return kOk;
        }
        const bool connected = s.value("connection").toString() == QLatin1String("connected");
        print("connection: " + s.value("connection").toString() + (s.value("simulated").toBool() ? " (simulator)" : ""));
        if (connected)
            print("armed: " + yesNo(s.value("armed").toBool()) + ", compartment open: " +
                  yesNo(s.value("compartment_open").toBool()) + ", laser on: " + yesNo(s.value("laser_on").toBool()) +
                  ", channel: " + QString::number(s.value("channel").toInt()) + " nm");
        print("busy: " + yesNo(s.value("busy").toBool()) + (s.value("series_running").toBool() ? " (series)" : "") +
              ", remote measurements: " + (s.value("remote_control").toBool() ? "allowed" : "not allowed (read-only)"));
        return kOk;
    }
    if (cmd == "info" || cmd == "progress" || cmd == "standards" || cmd == "job") {
        const QString path = cmd == "progress" ? QStringLiteral("/series/progress")
                             : cmd == "job"    ? QStringLiteral("/jobs/%1").arg(idArg())
                                               : "/" + cmd;
        const auto r = call("GET", path);
        if (!r.ok())
            return failure(r);
        if (g_json || cmd == "info") {
            printJson(r.json);
            return cmd == "job" ? jobExit(r.json.toObject()) : kOk;
        }
        const QJsonObject o = r.json.toObject();
        if (cmd == "job") {
            print(describeJob(o));
            return jobExit(o);
        }
        if (cmd == "progress") {
            if (!o.contains("n"))
                print(QStringLiteral("no series yet"));
            else
                print(QStringLiteral("%1: %2/%3 spectra, %4 s elapsed%5")
                          .arg(o.value("running").toBool() ? (o.value("paused").toBool() ? "paused" : "running")
                                                           : (o.value("done").toBool() ? "done" : "ended"))
                          .arg(o.value("n").toInt())
                          .arg(o.value("total").toInt())
                          .arg(o.value("elapsed_s").toDouble(), 0, 'f', 1)
                          .arg(o.value("next_in_s").isDouble()
                                   ? QStringLiteral(", next in %1 s").arg(o.value("next_in_s").toDouble(), 0, 'f', 1)
                                   : QString()));
            return kOk;
        }
        for (const QJsonValue& v : o.value("standards").toArray()) {
            QStringList bands;
            for (const QJsonValue& b : v.toObject().value("bands").toArray())
                bands << QString::number(b.toObject().value("position").toDouble(), 'g', 6);
            print(v.toObject().value("name").toString().leftJustified(28) + " " + bands.join(", "));
        }
        return kOk;
    }
    if (cmd == "stop") {
        const auto r = call("POST", "/stop", QJsonObject{});
        if (!r.ok())
            return failure(r);
        if (g_json)
            printJson(r.json);
        else
            print(r.json.toObject().value("laser_on").toBool() ? "stop sent; the laser still reports ON"
                                                               : "stopped; laser off");
        return kOk;
    }
    if (cmd == "spectra") {
        const auto r = call("GET", "/spectra" + (has("--limit") ? "?limit=" + QString::number(num("--limit", true).toInt()) : QString()));
        if (!r.ok())
            return failure(r);
        if (g_json) {
            printJson(r.json);
            return kOk;
        }
        for (const QJsonValue& v : r.json.toObject().value("spectra").toArray()) {
            const QJsonObject s = v.toObject();
            print(QStringLiteral("%1  %2  %3  %4 s × %5  %6")
                      .arg(s.value("id").toInt(), 4)
                      .arg(s.value("time").toString(), s.value("sample").toString())
                      .arg(s.value("exposure_s").toDouble())
                      .arg(s.value("averages").toInt())
                      .arg(s.value("file").toString()));
        }
        return kOk;
    }
    if (cmd == "spectrum") {
        const int id = idArg();
        const auto r = call("GET", QStringLiteral("/spectra/%1").arg(id));
        if (!r.ok())
            return failure(r);
        const QJsonObject s = r.json.toObject();
        if (has("--output")) {
            lr::fileio::DataSpectrum d;
            d.name = s.value("sample").toString();
            for (const QJsonValue& v : s.value("shift").toArray())
                d.x.push_back(v.toDouble());
            for (const QJsonValue& v : s.value("intensity").toArray())
                d.y.push_back(v.toDouble());
            const QJsonObject meta = s.value("meta").toObject();
            for (auto it = meta.begin(); it != meta.end(); ++it)
                d.meta.emplace_back(it.key(), it.value().toString());
            try {
                lr::fileio::writeCsv(opt.at("--output"), d);
            } catch (const lr::Error& e) {
                std::fputs((QStringLiteral("error: ") + QString::fromUtf8(e.what()) + "\n").toUtf8().constData(), stderr);
                return kFailed;
            }
        }
        if (g_json)
            printJson(s);
        else
            print(QStringLiteral("spectrum %1: %2, %3, %4 points%5")
                      .arg(id)
                      .arg(s.value("sample").toString(), s.value("time").toString())
                      .arg(s.value("points").toInt())
                      .arg(has("--output") ? ", saved " + opt.at("--output") : QString()));
        return kOk;
    }
    if (cmd == "peaks") {
        QJsonObject body{{"id", idArg()}};
        if (has("--snr"))
            body.insert("snr", num("--snr", false));
        const auto r = call("POST", "/peaks", body);
        if (!r.ok())
            return failure(r);
        if (g_json) {
            printJson(r.json);
            return kOk;
        }
        print(QStringLiteral("position (cm-1)    height      FWHM     S/N"));
        for (const QJsonValue& v : r.json.toObject().value("peaks").toArray()) {
            const QJsonObject p = v.toObject();
            print(QStringLiteral("%1 %2 %3 %4%5")
                      .arg(p.value("position").toDouble(), 15, 'f', 1)
                      .arg(p.value("height").toDouble(), 9, 'f', 0)
                      .arg(p.value("fwhm").toDouble(), 9, 'f', 1)
                      .arg(p.value("snr").toDouble(), 7, 'f', 0)
                      .arg(p.value("shoulder").toBool() ? "  shoulder" : ""));
        }
        return kOk;
    }
    if (cmd == "log") {
        const auto r = call("GET", "/log" + (has("--since") ? "?since=" + QString::number(num("--since", true).toInteger()) : QString()));
        if (!r.ok())
            return failure(r);
        if (g_json) {
            printJson(r.json);
            return kOk;
        }
        for (const QJsonValue& v : r.json.toObject().value("lines").toArray())
            print(QStringLiteral("%1  %2  %3").arg(v.toObject().value("seq").toInteger()).arg(
                v.toObject().value("time").toString(), v.toObject().value("text").toString()));
        return kOk;
    }
    if (cmd == "measure" || cmd == "series") {
        QJsonObject body;
        if (has("--exposure")) body.insert("exposure_s", num("--exposure", false));
        if (has("--averages")) body.insert("averages", num("--averages", true));
        if (has("--sample")) body.insert("sample", opt.at("--sample"));
        if (has("--background")) body.insert("background", opt.at("--background"));
        if (has("--prebleach")) body.insert("prebleach_s", num("--prebleach", false));
        if (has("--channel")) body.insert("channel", num("--channel", true));
        int timeoutMs = 30000;
        if (cmd == "measure") {
            const bool wait = !has("--no-wait");
            body.insert("wait", wait);
            if (wait) {
                const double e = body.value("exposure_s").toDouble(1.0), n = body.value("averages").toDouble(1.0);
                const double expected = e * n * 2 + body.value("prebleach_s").toDouble(0) + 30;
                timeoutMs = int(std::min(24 * 3600.0, expected * 2 + 120) * 1000);   // incl. a channel switch
            }
        } else {
            if (has("--interval")) body.insert("interval_s", num("--interval", false));
            if (has("--count")) body.insert("count", num("--count", true));
            if (has("--duration")) body.insert("duration_s", num("--duration", false));
            if (has("--delay")) body.insert("start_delay_s", num("--delay", false));
            if (has("--fast")) body.insert("laser_stays_on", true);
        }
        if (has("--timeout")) timeoutMs = int(num("--timeout", false).toDouble() * 1000);
        const auto r = call("POST", "/" + cmd, body, timeoutMs);
        if (!r.ok())
            return failure(r);
        const QJsonObject job = r.json.toObject();
        if (g_json)
            printJson(job);
        else
            print(describeJob(job));
        return jobExit(job);
    }
    usage("unknown command: " + cmd);
}

}  // namespace lr::cli
