// LibreRamanMCP: Model Context Protocol server (stdio, JSON-RPC 2.0, one message per line) for AI agents.
// It is only a client of the running LibreRaman application's local API (core/apiclient.h,
// gui/apiserver.h), so the same rules apply as for every remote client: read-only unless "Allow remote
// measurements" is switched on at the instrument, ARM still needed on the instrument, stop always allowed,
// every action logged in the application as "API (mcp)".
//
//   LibreRamanMCP [--api-file FILE]          (Claude Code: claude mcp add libreraman -- /path/LibreRamanMCP)
//
// Methods: initialize, notifications/initialized, ping, tools/list, tools/call. Tools: status, acquire,
// run_series, series_progress, get_job, stop, list_spectra, get_spectrum, find_peaks, process,
// list_standards, get_log.
#include "core/legacyname.h"
#include "core/apiclient.h"
#include "core/errors.h"

#include <QCoreApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <iostream>
#include <string>

namespace {

QString g_apiFile;
const QStringList kProtocols = {QStringLiteral("2025-06-18"), QStringLiteral("2025-03-26"), QStringLiteral("2024-11-05")};

void send(const QJsonObject& msg) {
    std::fputs(QJsonDocument(msg).toJson(QJsonDocument::Compact).constData(), stdout);
    std::fputc('\n', stdout);
    std::fflush(stdout);
}

void sendResult(const QJsonValue& id, const QJsonObject& result) {
    send({{"jsonrpc", "2.0"}, {"id", id}, {"result", result}});
}

void sendError(const QJsonValue& id, int code, const QString& message) {
    send({{"jsonrpc", "2.0"}, {"id", id}, {"error", QJsonObject{{"code", code}, {"message", message}}}});
}

// ---- tool schemas
QJsonObject prop(const char* type, const char* description) {
    return {{"type", type}, {"description", description}};
}

QJsonObject schema(const QJsonObject& props = {}, const QStringList& required = {}) {
    QJsonObject s{{"type", "object"}, {"properties", props}, {"additionalProperties", false}};
    if (!required.isEmpty())
        s.insert("required", QJsonArray::fromStringList(required));
    return s;
}

QJsonObject enumProp(const char* description, const QStringList& values) {
    QJsonObject p = prop("string", description);
    p.insert("enum", QJsonArray::fromStringList(values));
    return p;
}

QJsonObject processingSchema() {
    QStringList baselines{"none", "als", "arpls", "airpls", "polynomial", "rolling_ball", "snip", "snip4"};
    return {{"type", "object"},
            {"description", "Processing before the analysis (default: the settings of the Measure page in the GUI)."},
            {"properties", QJsonObject{{"baseline", enumProp("Baseline method", baselines)},
                                       {"strength", prop("number", "Baseline stiffness 0-10 (default 5)")},
                                       {"fit_min", prop("number", "Fit the baseline above this shift only, cm-1 "
                                                                  "(default 150, the edge filter; 0 = from 0 cm-1)")},
                                       {"despike", prop("boolean", "Remove cosmic-ray spikes")},
                                       {"smooth", prop("boolean", "Savitzky-Golay smoothing")},
                                       {"smooth_window", prop("integer", "Smoothing window (points, odd)")},
                                       {"derivative", prop("integer", "0, 1 or 2")},
                                       {"normalize", enumProp("Normalisation", {"none", "max", "area", "band"})},
                                       {"band", prop("number", "Band position (cm-1) for normalize=band")}}},
            {"additionalProperties", false}};
}

QJsonObject tool(const char* name, const char* title, const char* description, const QJsonObject& input,
                 bool readOnly, bool destructive = false) {
    return {{"name", name},
            {"title", title},
            {"description", description},
            {"inputSchema", input},
            {"annotations", QJsonObject{{"title", title},
                                        {"readOnlyHint", readOnly},
                                        {"destructiveHint", destructive},
                                        {"idempotentHint", readOnly},
                                        {"openWorldHint", false}}}};
}

QJsonArray tools() {
    const QJsonObject measureProps{
        {"sample", prop("string", "Sample name (used in the file name)")},
        {"exposure_s", prop("number", "Exposure per frame in seconds (0.01-600, default 1)")},
        {"averages", prop("integer", "Number of frames averaged (1-10000, default 1)")},
        {"background", enumProp("Dark background: 'each' (default) or 'none'", {"each", "none"})},
        {"prebleach_s", prop("number", "Laser on before measuring, seconds (0-3600, default 0)")},
        {"channel", prop("integer", "532 or 1064 nm (default: keep the current channel)")}};
    QJsonObject seriesProps{
        {"sample", prop("string", "Sample name")},
        {"exposure_s", prop("number", "Exposure per frame in seconds (default 1)")},
        {"averages", prop("integer", "Frames per spectrum (default 1)")},
        {"background", enumProp("'each' (default), 'once' or 'none'", {"each", "once", "none"})},
        {"interval_s", prop("number", "Time between spectra in seconds (default 10)")},
        {"count", prop("integer", "Number of spectra (default 10; or give duration_s)")},
        {"duration_s", prop("number", "Duration of the series in seconds (instead of count)")},
        {"start_delay_s", prop("number", "Wait before the first spectrum, seconds")},
        {"laser_stays_on", prop("boolean", "Fastest mode: laser on throughout, background once")}};
    const QJsonObject idProp{{"id", prop("integer", "Spectrum id (from list_spectra or acquire)")}};
    QJsonObject peakProps = idProp;
    peakProps.insert("snr", prop("number", "Minimum signal-to-noise ratio (default 5)"));
    peakProps.insert("min_fwhm", prop("number", "Minimum FWHM in cm-1 (default 2)"));
    peakProps.insert("xmin", prop("number", "Lower limit of the region, cm-1"));
    peakProps.insert("xmax", prop("number", "Upper limit of the region, cm-1"));
    peakProps.insert("settings", processingSchema());
    QJsonObject procProps = idProp;
    procProps.insert("settings", processingSchema());
    procProps.insert("max_points", prop("integer", "Return at most this many points (decimated; default all)"));
    QJsonObject getProps = idProp;
    getProps.insert("max_points", prop("integer", "Return at most this many points (decimated; default all, ~2000)"));
    getProps.insert("include_raw", prop("boolean", "Also return the raw signal and background frames"));

    return {
        tool("status", "Instrument status",
             "Current state of the Raman spectrometer: connection, ARM, compartment, laser, channel, whether a "
             "measurement is running, and whether remote measurements are allowed at the instrument.",
             schema(), true),
        tool("acquire", "Measure one spectrum",
             "Measure one Raman spectrum and wait for it (switches the laser on). Needs 'Allow remote "
             "measurements' in the LibreRaman GUI and ARM pressed on the instrument; refused while another "
             "measurement runs. Returns the job with the new spectrum id.",
             schema(measureProps), false),
        tool("run_series", "Start a time series",
             "Start a time series of spectra (returns at once with a job id; use series_progress, get_job and "
             "list_spectra to follow it, stop to end it). Same permissions as acquire.",
             schema(seriesProps), false),
        tool("series_progress", "Series progress", "Progress of the running or last time series.", schema(), true),
        tool("get_job", "Job state", "State of a measurement or series started through the API.",
             schema({{"id", prop("integer", "Job id")}}, {"id"}), true),
        tool("stop", "Stop and laser off",
             "Stop the running measurement or series and switch the laser off immediately. Always allowed.",
             schema(), false, true),
        tool("list_spectra", "Recent spectra", "Recent spectra (metadata, newest last).",
             schema({{"limit", prop("integer", "At most this many (default 20)")}}), true),
        tool("get_spectrum", "Spectrum data", "Data of one spectrum: Raman shift (cm-1) and intensity, metadata.",
             schema(getProps, {"id"}), true),
        tool("find_peaks", "Find peaks",
             "Peaks of a spectrum (position cm-1, height, FWHM, area, S/N) after baseline processing.",
             schema(peakProps, {"id"}), true),
        tool("process", "Process spectrum",
             "Apply processing (baseline, despike, smoothing, derivative, normalisation) to a spectrum; returns "
             "the processed data and the baseline.",
             schema(procProps, {"id"}), true),
        tool("list_standards", "Reference standards",
             "Raman shift reference standards (calibration) with their band positions.", schema(), true),
        tool("get_log", "Application log", "Log lines of the LibreRaman application (with origin of each action).",
             schema({{"since", prop("integer", "Only lines after this sequence number")}}), true),
    };
}

// ---- tool calls
QJsonObject textResult(const QJsonValue& v, bool isError) {
    const QByteArray text = v.isString() ? v.toString().toUtf8()
                            : v.isArray() ? QJsonDocument(v.toArray()).toJson(QJsonDocument::Indented)
                                          : QJsonDocument(v.toObject()).toJson(QJsonDocument::Indented);
    QJsonObject r{{"content", QJsonArray{QJsonObject{{"type", "text"}, {"text", QString::fromUtf8(text)}}}},
                  {"isError", isError}};
    return r;
}

QJsonArray decimate(const QJsonArray& a, int maxPoints) {
    if (maxPoints <= 0 || a.size() <= maxPoints)
        return a;
    QJsonArray out;
    const double step = double(a.size() - 1) / double(maxPoints - 1);
    for (int i = 0; i < maxPoints; ++i)
        out.append(a.at(qsizetype(std::lround(i * step))));
    return out;
}

QJsonObject callTool(const QString& name, const QJsonObject& args) {
    lr::api::Endpoint ep;
    try {
        ep = lr::api::findServer(g_apiFile);
    } catch (const lr::Error& e) {
        return textResult(QString::fromUtf8(e.what()), true);
    }
    auto call = [&](const QByteArray& method, const QString& path, const QJsonValue& body = QJsonValue(QJsonValue::Undefined),
                    int timeoutMs = 30000) -> lr::api::Reply {
        return lr::api::request(ep, method, path, body, timeoutMs, QStringLiteral("mcp"));
    };
    auto result = [](const lr::api::Reply& r) {
        if (!r.ok()) {
            QString msg = r.error;
            if (r.status == 403)
                msg += QStringLiteral(" (a person at the instrument must allow remote measurements)");
            return textResult(QJsonObject{{"error", msg}, {"status", r.status}}, true);
        }
        return textResult(r.json, false);
    };
    auto pick = [&](const QStringList& keys) {
        QJsonObject o;
        for (const QString& k : keys)
            if (args.contains(k))
                o.insert(k, args.value(k));
        return o;
    };

    if (name == "status")
        return result(call("GET", "/status"));
    if (name == "series_progress")
        return result(call("GET", "/series/progress"));
    if (name == "list_standards")
        return result(call("GET", "/standards"));
    if (name == "stop")
        return result(call("POST", "/stop", QJsonObject{}));
    if (name == "get_job")
        return result(call("GET", QStringLiteral("/jobs/%1").arg(args.value("id").toInt())));
    if (name == "get_log")
        return result(call("GET", QStringLiteral("/log?since=%1").arg(args.value("since").toInteger())));
    if (name == "list_spectra")
        return result(call("GET", QStringLiteral("/spectra?limit=%1").arg(args.value("limit").toInt(20))));
    if (name == "acquire") {
        QJsonObject body = pick({"sample", "exposure_s", "averages", "background", "prebleach_s", "channel"});
        body.insert("wait", true);
        const double expected = args.value("exposure_s").toDouble(1) * args.value("averages").toDouble(1) * 2 +
                                args.value("prebleach_s").toDouble(0) + 30;
        lr::api::Reply r = call("POST", "/measure", body, int(std::min(24 * 3600.0, expected * 2 + 120) * 1000));
        if (r.ok()) {
            const QJsonObject job = r.json.toObject();
            const QString st = job.value("state").toString();
            if (st != QLatin1String("done"))
                return textResult(job, true);
            QJsonObject out = job;                       // add the spectrum's metadata
            const QJsonArray ids = job.value("spectra").toArray();
            if (!ids.isEmpty()) {
                const lr::api::Reply list = call("GET", "/spectra");
                for (const QJsonValue& v : list.json.toObject().value("spectra").toArray())
                    if (v.toObject().value("id").toInt() == ids.last().toInt())
                        out.insert("spectrum", v);
            }
            return textResult(out, false);
        }
        return result(r);
    }
    if (name == "run_series")
        return result(call("POST", "/series", pick({"sample", "exposure_s", "averages", "background", "interval_s",
                                                    "count", "duration_s", "start_delay_s", "laser_stays_on"})));
    if (name == "get_spectrum") {
        lr::api::Reply r = call("GET", QStringLiteral("/spectra/%1").arg(args.value("id").toInt()));
        if (!r.ok())
            return result(r);
        QJsonObject s = r.json.toObject();
        const int maxPoints = args.value("max_points").toInt(0);
        if (!args.value("include_raw").toBool()) {
            s.remove("signal");
            s.remove("background");
        }
        for (const char* k : {"shift", "intensity", "signal", "background"})
            if (s.contains(k))
                s.insert(k, decimate(s.value(k).toArray(), maxPoints));
        return textResult(s, false);
    }
    if (name == "find_peaks")
        return result(call("POST", "/peaks", pick({"id", "snr", "min_fwhm", "xmin", "xmax", "settings"})));
    if (name == "process") {
        lr::api::Reply r = call("POST", "/process", pick({"id", "settings"}));
        if (!r.ok())
            return result(r);
        QJsonObject o = r.json.toObject();
        const int maxPoints = args.value("max_points").toInt(0);
        for (const char* k : {"x", "y", "baseline"})
            o.insert(k, decimate(o.value(k).toArray(), maxPoints));
        return textResult(o, false);
    }
    return {};                                           // unknown tool: caller answers with an error
}

}  // namespace

int main(int argc, char** argv) {
    QCoreApplication app(argc, argv);
    lr::legacy::env();                                  // carried over from LibreRamen (<= 0.3.9)
    lr::legacy::configDir();
    const QStringList args = app.arguments().mid(1);
    for (int i = 0; i < args.size(); ++i) {
        if (args[i] == "--api-file" && i + 1 < args.size()) {
            g_apiFile = args[++i];
        } else if (args[i] == "-h" || args[i] == "--help") {
            std::fputs("usage: LibreRamanMCP [--api-file FILE]\n"
                       "MCP server (stdio) for AI agents; talks to the running LibreRaman application.\n"
                       "Claude Code: claude mcp add libreraman -- LibreRamanMCP\n",
                       stdout);
            return 0;
        } else if (args[i] == "--version") {
            std::fputs("LibreRamanMCP " LIBRERAMAN_VERSION "\n", stdout);
            return 0;
        } else {
            std::fprintf(stderr, "LibreRamanMCP: unknown argument %s\n", qPrintable(args[i]));
            return 2;
        }
    }
    const QJsonArray toolList = tools();
    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty() || line == "\r")
            continue;
        QJsonParseError pe{};
        const QJsonDocument doc = QJsonDocument::fromJson(QByteArray::fromStdString(line), &pe);
        if (pe.error != QJsonParseError::NoError || !doc.isObject()) {
            sendError(QJsonValue(), -32700, QStringLiteral("parse error"));
            continue;
        }
        const QJsonObject msg = doc.object();
        const QString method = msg.value("method").toString();
        const bool isRequest = msg.contains("id") && !method.isEmpty();
        const QJsonValue id = msg.value("id");
        if (!isRequest)
            continue;                                    // notifications (initialized, cancelled) and responses
        const QJsonObject params = msg.value("params").toObject();
        if (method == "initialize") {
            const QString wanted = params.value("protocolVersion").toString();
            sendResult(id, {{"protocolVersion", kProtocols.contains(wanted) ? wanted : kProtocols.first()},
                            {"capabilities", QJsonObject{{"tools", QJsonObject{{"listChanged", false}}}}},
                            {"serverInfo", QJsonObject{{"name", "libreraman"}, {"title", "LibreRaman"},
                                                       {"version", LIBRERAMAN_VERSION}}},
                            {"instructions",
                             "Controls a Raman spectrometer through the running LibreRaman application. Call status "
                             "first. Measurements (acquire, run_series) switch a laser on: they only work when a "
                             "person has allowed remote measurements in the LibreRaman GUI and pressed ARM on the "
                             "instrument; otherwise report this to the user instead of retrying. stop is always "
                             "allowed and switches the laser off."}});
        } else if (method == "ping") {
            sendResult(id, {});
        } else if (method == "tools/list") {
            sendResult(id, {{"tools", toolList}});
        } else if (method == "tools/call") {
            const QString name = params.value("name").toString();
            const QJsonObject result = callTool(name, params.value("arguments").toObject());
            if (result.isEmpty())
                sendError(id, -32602, QStringLiteral("unknown tool: ") + name);
            else
                sendResult(id, result);
        } else {
            sendError(id, -32601, QStringLiteral("method not found: ") + method);
        }
    }
    return 0;
}
