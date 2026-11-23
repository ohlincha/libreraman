#include "apiserver.h"

#include "controller.h"

#include "core/apiclient.h"
#include "core/errors.h"
#include "core/peaks.h"
#include "core/standards.h"

#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QStandardPaths>

#include <cmath>

namespace lr::gui {

using lr::http::Reply;
using lr::http::Request;
using lr::http::Response;

namespace {

// Limits for remote clients (the same ranges as the Measure page's fields).
struct Limits {
    double exposureMin = 0.01, exposureMax = 600;
    int averagesMax = 10000;
    double prebleachMax = 3600;
    double intervalMin = 0.1, intervalMax = 100000;
    int countMax = 100000;
    double durationMax = 1000 * 3600.0;
    double startDelayMax = 7 * 24 * 3600.0;
} const kLimits;

QJsonArray toArray(const lr::Vec& v) {
    QJsonArray a;
    for (double d : v)
        a.append(std::isfinite(d) ? QJsonValue(d) : QJsonValue());
    return a;
}

bool sameToken(const QByteArray& a, const QByteArray& b) {     // constant time for equal lengths
    if (a.size() != b.size())
        return false;
    unsigned char d = 0;
    for (qsizetype i = 0; i < a.size(); ++i)
        d |= static_cast<unsigned char>(a[i] ^ b[i]);
    return d == 0;
}

QString clientName(const Request& r) {
    QString c = QString::fromUtf8(r.header("x-libreramen-client")).simplified();
    QString out;
    for (const QChar ch : c)
        if (ch.isLetterOrNumber() || QStringLiteral(" -_.@/").contains(ch))
            out += ch;
    out.truncate(40);
    return out.isEmpty() ? QStringLiteral("client") : out;
}

// Typed, range-checked access to the fields of a request body; unknown fields are errors (typos must not
// silently fall back to defaults when the laser is involved).
class Fields {
public:
    Fields(const QJsonObject& o, QStringList allowed) : m_o(o) {
        for (auto it = o.begin(); it != o.end(); ++it)
            if (!allowed.contains(it.key()) && m_err.isEmpty())
                m_err = QStringLiteral("unknown field \"%1\" (allowed: %2)").arg(it.key(), allowed.join(QStringLiteral(", ")));
    }
    bool has(const QString& k) const { return m_o.contains(k) && !m_o.value(k).isNull(); }
    double num(const QString& k, double def, double lo, double hi) {
        if (!has(k))
            return def;
        const QJsonValue v = m_o.value(k);
        if (!v.isDouble() || !std::isfinite(v.toDouble()))
            return fail(k + QStringLiteral(" must be a number")), def;
        const double d = v.toDouble();
        if (d < lo || d > hi)
            return fail(QStringLiteral("%1 must be between %2 and %3").arg(k).arg(lo).arg(hi)), def;
        return d;
    }
    int integer(const QString& k, int def, int lo, int hi) {
        const double d = num(k, def, lo, hi);
        if (has(k) && d != std::floor(d))
            fail(k + QStringLiteral(" must be an integer"));
        return int(d);
    }
    bool boolean(const QString& k, bool def) {
        if (!has(k))
            return def;
        if (!m_o.value(k).isBool())
            fail(k + QStringLiteral(" must be true or false"));
        return m_o.value(k).toBool();
    }
    QString choice(const QString& k, const QString& def, const QStringList& allowed) {
        if (!has(k))
            return def;
        const QString s = m_o.value(k).toString();
        if (!allowed.contains(s))
            fail(QStringLiteral("%1 must be one of: %2").arg(k, allowed.join(QStringLiteral(", "))));
        return s;
    }
    QString text(const QString& k, const QString& def, int maxLen = 200) {
        if (!has(k))
            return def;
        if (!m_o.value(k).isString())
            return fail(k + QStringLiteral(" must be a string")), def;
        const QString s = m_o.value(k).toString().trimmed();
        if (s.size() > maxLen)
            fail(QStringLiteral("%1 is longer than %2 characters").arg(k).arg(maxLen));
        return s.isEmpty() ? def : s;
    }
    void fail(const QString& m) {
        if (m_err.isEmpty())
            m_err = m;
    }
    const QString& error() const { return m_err; }

private:
    const QJsonObject& m_o;
    QString m_err;
};


}  // namespace

// ============================================================================================ Job
QJsonObject ApiServer::Job::toJson() const {
    QJsonArray ids;
    for (int s : spectra)
        ids.append(s);
    QJsonObject o{{QStringLiteral("id"), id},
                  {QStringLiteral("kind"), kind},
                  {QStringLiteral("origin"), origin},
                  {QStringLiteral("state"), state},
                  {QStringLiteral("spectra"), ids},
                  {QStringLiteral("started"), lr::iso(started)}};
    if (!error.isEmpty())
        o.insert(QStringLiteral("error"), error);
    if (finished.isValid())
        o.insert(QStringLiteral("finished"), lr::iso(finished));
    return o;
}

// ============================================================================================ set-up
ApiServer::ApiServer(Controller* ctl, QObject* parent) : QObject(parent), m_ctl(ctl) {
    m_http.setHandler([this](const Request& r, const Reply& reply) { handle(r, reply); });
    connect(ctl, &Controller::spectrum, this, &ApiServer::onSpectrum);
    connect(ctl, &Controller::busyChanged, this, &ApiServer::onBusyChanged);
    connect(ctl, &Controller::error, this, [this](const QString& msg) {
        if (m_activeJob && m_jobs.count(m_activeJob) && m_jobs[m_activeJob].error.isEmpty())
            m_jobs[m_activeJob].error = msg.section(QLatin1Char('\n'), 0, 0);
    });
    connect(ctl, &Controller::seriesProgress, this, [this](const lr::SeriesProgress& p) {
        m_progress = p;
        m_haveProgress = true;
    });
    registerRoutes();
}

ApiServer::~ApiServer() { stop(); }

bool ApiServer::start(quint16 port, const QString& apiFile, QString* error) {
    stop();
    if (!m_http.listen(QHostAddress::LocalHost, port, error))
        return false;
    m_token = lr::api::newToken();
    QString err;
    if (!lr::api::writeApiFile(apiFile, m_http.port(), m_token, &err)) {
        m_http.close();
        if (error)
            *error = QStringLiteral("cannot write %1: %2").arg(QDir::toNativeSeparators(apiFile), err);
        return false;
    }
    m_apiFile = apiFile;
    emit logMessage(QStringLiteral("Local API on http://127.0.0.1:%1 (%2)")
                        .arg(m_http.port())
                        .arg(m_remoteControl ? QStringLiteral("remote measurements allowed")
                                             : QStringLiteral("read-only")));
    emit runningChanged(true);
    return true;
}

void ApiServer::stop() {
    if (!m_http.isListening())
        return;
    m_http.close();
    if (!m_apiFile.isEmpty()) {                       // only our own file (another instance may have its own)
        QFile f(m_apiFile);
        if (f.open(QIODevice::ReadOnly) &&
            QJsonDocument::fromJson(f.readAll()).object().value(QStringLiteral("token")).toString().toLatin1() == m_token) {
            f.close();
            QFile::remove(m_apiFile);
        }
    }
    m_token.clear();
    emit runningChanged(false);
}

bool ApiServer::isRunning() const { return m_http.isListening(); }

quint16 ApiServer::port() const { return m_http.port(); }

void ApiServer::setRemoteControl(bool allowed) {
    if (allowed == m_remoteControl)
        return;
    m_remoteControl = allowed;
    emit logMessage(allowed ? QStringLiteral("GUI: remote measurements allowed (API, command line, AI agents)")
                            : QStringLiteral("GUI: remote measurements not allowed (remote clients read-only)"));
    emit remoteControlChanged(allowed);
}

void ApiServer::recordLog(const QString& line) {
    m_log.push_back({m_nextLog++, QDateTime::currentDateTime(), line});
    while (m_log.size() > 2000)
        m_log.pop_front();
}

void ApiServer::log(const QString& origin, const QString& msg) { emit logMessage(origin + QStringLiteral(": ") + msg); }

void ApiServer::addRoute(const QByteArray& method, const QString& pattern, Route route) {
    const QStringList segs = pattern.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    for (RouteEntry& r : m_routes)
        if (r.method == method && r.pattern == segs) {
            r.route = std::move(route);
            return;
        }
    m_routes.push_back({method, segs, std::move(route)});
}

void ApiServer::registerRoutes() {
    using M = void (ApiServer::*)(const Call&, const Reply&);
    const std::tuple<const char*, const char*, M> routes[] = {
        {"GET", "/status", &ApiServer::getStatus},          {"GET", "/info", &ApiServer::getInfo},
        {"GET", "/spectra", &ApiServer::getSpectra},        {"GET", "/spectra/{id}", &ApiServer::getSpectrum},
        {"POST", "/measure", &ApiServer::postMeasure},      {"POST", "/series", &ApiServer::postSeries},
        {"GET", "/series/progress", &ApiServer::getProgress}, {"GET", "/jobs/{id}", &ApiServer::getJob},
        {"POST", "/stop", &ApiServer::postStop},            {"POST", "/process", &ApiServer::postProcess},
        {"POST", "/peaks", &ApiServer::postPeaks},          {"GET", "/standards", &ApiServer::getStandards},
        {"GET", "/log", &ApiServer::getLog}};
    for (const auto& [method, path, fn] : routes) {
        const M f = fn;
        addRoute(method, QString::fromLatin1(path), [this, f](const Call& c, const Reply& r) { (this->*f)(c, r); });
    }
    addRoute("POST", QStringLiteral("/match"), [](const Call&, const Reply& r) {
        r(Response::error(501, QStringLiteral("library matching is not available yet")));
    });
}

// ============================================================================================ dispatch
void ApiServer::handle(const Request& req, const Reply& reply) {
    QByteArray host = req.header("host");
    if (!host.isEmpty()) {                            // DNS rebinding: only loopback names
        if (host.startsWith('['))
            host = host.left(host.indexOf(']') + 1);
        else if (host.contains(':'))
            host = host.left(host.indexOf(':'));
        if (host != "127.0.0.1" && host != "localhost" && host != "[::1]")
            return reply(Response::error(403, QStringLiteral("only local clients (127.0.0.1) are served")));
    }
    const QByteArray auth = req.header("authorization");
    if (m_token.isEmpty() || !auth.startsWith("Bearer ") || !sameToken(auth.mid(7).trimmed(), m_token)) {
        Response r = Response::error(401, QStringLiteral("missing or wrong API token (see the API file %1)")
                                              .arg(QDir::toNativeSeparators(m_apiFile)));
        r.headers.insert("WWW-Authenticate", "Bearer");
        return reply(r);
    }
    const QStringList segs = req.path.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    bool pathMatched = false;
    for (const RouteEntry& r : m_routes) {
        if (r.pattern.size() != segs.size())
            continue;
        QStringList args;
        bool match = true;
        for (int i = 0; i < segs.size() && match; ++i) {
            if (r.pattern[i].startsWith(QLatin1Char('{')))
                args << segs[i];
            else
                match = r.pattern[i] == segs[i];
        }
        if (!match)
            continue;
        pathMatched = true;
        if (r.method != req.method)
            continue;
        const Call call{req, args, QStringLiteral("API (%1)").arg(clientName(req))};
        try {
            r.route(call, reply);
        } catch (const lr::Error& e) {
            reply(Response::error(400, QString::fromUtf8(e.what())));
        } catch (const std::exception& e) {
            reply(Response::error(500, QStringLiteral("internal error: ") + QString::fromUtf8(e.what())));
        }
        return;
    }
    reply(pathMatched ? Response::error(405, QStringLiteral("method %1 not allowed for %2")
                                                 .arg(QString::fromLatin1(req.method), req.path))
                      : Response::error(404, QStringLiteral("no such endpoint: ") + req.path));
}

// ============================================================================================ state
void ApiServer::onSpectrum(const lr::Spectrum& sp) {
    const int id = m_nextSpectrum++;
    m_spectra.push_back({id, sp});
    while (int(m_spectra.size()) > kKeepSpectra)
        m_spectra.pop_front();
    if (m_activeJob && m_jobs.count(m_activeJob))
        m_jobs[m_activeJob].spectra.push_back(id);
}

void ApiServer::onBusyChanged(bool busy) {
    if (busy || !m_activeJob || !m_jobs.count(m_activeJob))
        return;
    Job& j = m_jobs[m_activeJob];
    m_activeJob = 0;
    j.finished = QDateTime::currentDateTime();
    if (!j.error.isEmpty())
        j.state = QStringLiteral("failed");
    else if (j.stopRequested || (j.kind == QLatin1String("measure") && j.spectra.empty()) ||
             (j.kind == QLatin1String("series") && !(m_haveProgress && m_progress.done)))
        j.state = QStringLiteral("stopped");
    else
        j.state = QStringLiteral("done");
    const QJsonObject o = j.toJson();
    for (const Reply& w : j.waiters)
        w(Response::json(200, o));
    j.waiters.clear();
}

ApiServer::Job& ApiServer::newJob(const QString& kind, const QString& origin) {
    Job j;
    j.id = m_nextJob++;
    j.kind = kind;
    j.origin = origin;
    j.started = QDateTime::currentDateTime();
    while (m_jobs.size() >= 100)
        m_jobs.erase(m_jobs.begin());
    return m_jobs[j.id] = j;
}

const ApiServer::Stored* ApiServer::findSpectrum(int id) const {
    for (const Stored& s : m_spectra)
        if (s.id == id)
            return &s;
    return nullptr;
}

QJsonObject ApiServer::spectrumSummary(const Stored& s) const {
    const lr::Spectrum& sp = s.sp;
    QJsonObject o{{QStringLiteral("id"), s.id},
                  {QStringLiteral("sample"), sp.metaValue(QStringLiteral("sample"))},
                  {QStringLiteral("time"), lr::iso(sp.t)},
                  {QStringLiteral("channel"), sp.channel},
                  {QStringLiteral("exposure_s"), sp.exposure_s},
                  {QStringLiteral("averages"), sp.averages},
                  {QStringLiteral("points"), int(sp.shift.size())}};
    const QString file = sp.metaValue(QStringLiteral("file"));
    o.insert(QStringLiteral("file"), file.isEmpty() ? QJsonValue() : QJsonValue(file));
    return o;
}

QJsonObject ApiServer::statusJson() const {
    const StatusView st = m_ctl->lastStatus();
    const bool connected = m_ctl->connection() == Controller::Connection::Connected && m_ctl->instrument();
    QJsonObject o{{QStringLiteral("connection"), Controller::connectionName(m_ctl->connection())},
                  {QStringLiteral("simulated"), m_ctl->simulated()},
                  {QStringLiteral("armed"), connected && st.armed},
                  {QStringLiteral("compartment_open"), connected && st.compartmentOpen},
                  {QStringLiteral("laser_on"), connected && (m_ctl->instrument()->laserOn() || st.laserOn)},
                  {QStringLiteral("channel"), connected ? m_ctl->instrument()->channel() : 0},
                  {QStringLiteral("busy"), m_ctl->isBusy()},
                  {QStringLiteral("series_running"), m_ctl->isSeriesRunning()},
                  {QStringLiteral("paused"), m_ctl->isPaused()},
                  {QStringLiteral("remote_control"), m_remoteControl},
                  {QStringLiteral("model"), m_ctl->info().model},
                  {QStringLiteral("serial"), m_ctl->info().serial},
                  {QStringLiteral("time"), lr::iso(QDateTime::currentDateTime())}};
    o.insert(QStringLiteral("job"), m_activeJob && m_jobs.count(m_activeJob) ? QJsonValue(m_jobs.at(m_activeJob).toJson())
                                                                             : QJsonValue());
    return o;
}

QJsonObject ApiServer::progressJson() const {
    QJsonObject o{{QStringLiteral("running"), m_ctl->isSeriesRunning()},
                  {QStringLiteral("paused"), m_ctl->isSeriesRunning() && (m_ctl->isPaused() || m_progress.paused)}};
    if (m_haveProgress) {
        o.insert(QStringLiteral("n"), m_progress.n);
        o.insert(QStringLiteral("total"), m_progress.total);
        o.insert(QStringLiteral("elapsed_s"), m_progress.elapsed_s);
        o.insert(QStringLiteral("next_in_s"), m_progress.next_in_s >= 0 ? QJsonValue(m_progress.next_in_s) : QJsonValue());
        o.insert(QStringLiteral("end"), m_progress.end.isValid() ? QJsonValue(lr::iso(m_progress.end)) : QJsonValue());
        o.insert(QStringLiteral("done"), m_progress.done);
    }
    return o;
}

// ============================================================================================ handlers
void ApiServer::getStatus(const Call&, const Reply& r) { r(Response::json(200, statusJson())); }

void ApiServer::getInfo(const Call&, const Reply& r) {
    const InstrumentInfo& i = m_ctl->info();
    QJsonArray endpoints;
    for (const RouteEntry& e : m_routes)
        endpoints.append(QString::fromLatin1(e.method) + QStringLiteral(" /") + e.pattern.join(QLatin1Char('/')));
    const QJsonObject limits{{QStringLiteral("exposure_s"), QJsonArray{kLimits.exposureMin, kLimits.exposureMax}},
                             {QStringLiteral("averages"), QJsonArray{1, kLimits.averagesMax}},
                             {QStringLiteral("prebleach_s"), QJsonArray{0, kLimits.prebleachMax}},
                             {QStringLiteral("interval_s"), QJsonArray{kLimits.intervalMin, kLimits.intervalMax}},
                             {QStringLiteral("count"), QJsonArray{1, kLimits.countMax}},
                             {QStringLiteral("duration_s"), QJsonArray{1, kLimits.durationMax}},
                             {QStringLiteral("start_delay_s"), QJsonArray{0, kLimits.startDelayMax}}};
    r(Response::json(200, QJsonObject{
                              {QStringLiteral("name"), QStringLiteral("LibreRamen")},
                              {QStringLiteral("version"), QStringLiteral(LIBRERAMEN_VERSION)},
                              {QStringLiteral("api_version"), lr::api::kApiVersion},
                              {QStringLiteral("instrument"),
                               QJsonObject{{QStringLiteral("model"), i.model},
                                           {QStringLiteral("serial"), i.serial},
                                           {QStringLiteral("simulated"), i.simulated},
                                           {QStringLiteral("calibration"), i.calibration},
                                           {QStringLiteral("connection"), Controller::connectionName(m_ctl->connection())}}},
                              {QStringLiteral("remote_control"), m_remoteControl},
                              {QStringLiteral("data_folder"), m_folder ? m_folder() : QString()},
                              {QStringLiteral("limits"), limits},
                              {QStringLiteral("endpoints"), endpoints}}));
}

void ApiServer::getSpectra(const Call& c, const Reply& r) {
    bool ok = true;
    const QString lim = c.request.query.queryItemValue(QStringLiteral("limit"));
    const int limit = lim.isEmpty() ? kKeepSpectra : lim.toInt(&ok);
    if (!ok || limit < 0)
        return r(Response::error(400, QStringLiteral("limit must be a non-negative integer")));
    QJsonArray a;
    const int n = int(m_spectra.size());
    for (int k = std::max(0, n - limit); k < n; ++k)
        a.append(spectrumSummary(m_spectra[std::size_t(k)]));
    r(Response::json(200, QJsonObject{{QStringLiteral("spectra"), a}}));
}

void ApiServer::getSpectrum(const Call& c, const Reply& r) {
    bool ok = false;
    const int id = c.args.value(0).toInt(&ok);
    const Stored* s = ok ? findSpectrum(id) : nullptr;
    if (!s)
        return r(Response::error(404, QStringLiteral("no spectrum %1 (only the last %2 are kept)").arg(c.args.value(0)).arg(kKeepSpectra)));
    QJsonObject o = spectrumSummary(*s);
    QJsonObject meta;
    for (const auto& kv : s->sp.meta)
        meta.insert(kv.first, kv.second);
    o.insert(QStringLiteral("meta"), meta);
    o.insert(QStringLiteral("shift"), toArray(s->sp.shift));
    o.insert(QStringLiteral("intensity"), toArray(s->sp.intensity));
    if (!s->sp.signal.empty())
        o.insert(QStringLiteral("signal"), toArray(s->sp.signal));
    if (!s->sp.background.empty())
        o.insert(QStringLiteral("background"), toArray(s->sp.background));
    r(Response::json(200, o));
}

bool ApiServer::mayControl(const Call& c, const Reply& reply) {
    if (!m_remoteControl) {
        log(c.origin, QStringLiteral("measurement refused (remote measurements are not allowed at the instrument)"));
        reply(Response::error(403, QStringLiteral("Remote measurements are not allowed. Switch on \"Allow remote "
                                                  "measurements\" in the Instrument menu of LibreRamen at the instrument.")));
        return false;
    }
    return true;
}

void ApiServer::postMeasure(const Call& c, const Reply& reply) {
    if (!mayControl(c, reply))
        return;
    QJsonObject body;
    QString err;
    if (!c.request.jsonBody(&body, &err))
        return reply(Response::error(400, err));
    Fields f(body, {QStringLiteral("exposure_s"), QStringLiteral("averages"), QStringLiteral("background"),
                    QStringLiteral("prebleach_s"), QStringLiteral("sample"), QStringLiteral("channel"),
                    QStringLiteral("wait")});
    MeasureSettings s;
    s.exposure_s = f.num(QStringLiteral("exposure_s"), 1.0, kLimits.exposureMin, kLimits.exposureMax);
    s.averages = f.integer(QStringLiteral("averages"), 1, 1, kLimits.averagesMax);
    s.background = f.choice(QStringLiteral("background"), QStringLiteral("each"), {QStringLiteral("each"), QStringLiteral("none")});
    s.prebleach_s = f.num(QStringLiteral("prebleach_s"), 0.0, 0.0, kLimits.prebleachMax);
    s.sample = f.text(QStringLiteral("sample"), QStringLiteral("sample"));
    s.channel = f.integer(QStringLiteral("channel"), 0, 0, 1064);
    if (s.channel != 0 && s.channel != 532 && s.channel != 1064)
        f.fail(QStringLiteral("channel must be 532 or 1064 (or 0: keep the current channel)"));
    const bool wait = f.boolean(QStringLiteral("wait"), false);
    if (!f.error().isEmpty())
        return reply(Response::error(400, f.error()));
    if (m_ctl->connection() != Controller::Connection::Connected || !m_ctl->instrument())
        return reply(Response::error(503, QStringLiteral("No instrument connected.")));
    if (m_ctl->isBusy())
        return reply(Response::error(409, QStringLiteral("Busy: a measurement or series is running.")));
    s.folder = m_folder ? m_folder() : QDir(QDir::homePath()).filePath(QStringLiteral("Documents/LibreRamen"));
    if (!m_ctl->measureOnce(s, c.origin))
        return reply(Response::error(409, QStringLiteral("the measurement could not be started")));
    Job& j = newJob(QStringLiteral("measure"), c.origin);
    m_activeJob = j.id;
    if (wait)
        j.waiters.push_back(reply);
    else
        reply(Response::json(202, j.toJson()));
}

void ApiServer::postSeries(const Call& c, const Reply& reply) {
    if (!mayControl(c, reply))
        return;
    QJsonObject body;
    QString err;
    if (!c.request.jsonBody(&body, &err))
        return reply(Response::error(400, err));
    Fields f(body, {QStringLiteral("sample"), QStringLiteral("exposure_s"), QStringLiteral("averages"),
                    QStringLiteral("background"), QStringLiteral("prebleach_s"), QStringLiteral("interval_s"),
                    QStringLiteral("count"), QStringLiteral("duration_s"), QStringLiteral("start_delay_s"),
                    QStringLiteral("laser_stays_on"), QStringLiteral("channel")});
    lr::SeriesSettings s;
    s.sample = f.text(QStringLiteral("sample"), QStringLiteral("sample"));
    s.exposure_s = f.num(QStringLiteral("exposure_s"), 1.0, kLimits.exposureMin, kLimits.exposureMax);
    s.averages = f.integer(QStringLiteral("averages"), 1, 1, kLimits.averagesMax);
    s.background = f.choice(QStringLiteral("background"), QStringLiteral("each"),
                            {QStringLiteral("each"), QStringLiteral("once"), QStringLiteral("none")});
    s.prebleach_s = f.num(QStringLiteral("prebleach_s"), 0.0, 0.0, kLimits.prebleachMax);
    s.interval_s = f.num(QStringLiteral("interval_s"), 10.0, kLimits.intervalMin, kLimits.intervalMax);
    if (f.has(QStringLiteral("count")) && f.has(QStringLiteral("duration_s")))
        f.fail(QStringLiteral("give either count or duration_s, not both"));
    s.stopByCount = !f.has(QStringLiteral("duration_s"));
    s.count = f.integer(QStringLiteral("count"), 10, 1, kLimits.countMax);
    s.duration_s = f.num(QStringLiteral("duration_s"), 60.0, 1.0, kLimits.durationMax);
    s.start_delay_s = f.num(QStringLiteral("start_delay_s"), 0.0, 0.0, kLimits.startDelayMax);
    s.laserStaysOn = f.boolean(QStringLiteral("laser_stays_on"), false);
    const int channel = f.integer(QStringLiteral("channel"), 0, 0, 1064);
    if (channel != 0 && channel != 532 && channel != 1064)
        f.fail(QStringLiteral("channel must be 532 or 1064 (or 0: keep the current channel)"));
    if (!f.error().isEmpty())
        return reply(Response::error(400, f.error()));
    if (m_ctl->connection() != Controller::Connection::Connected || !m_ctl->instrument())
        return reply(Response::error(503, QStringLiteral("No instrument connected.")));
    if (m_ctl->isBusy())
        return reply(Response::error(409, QStringLiteral("Busy: a measurement or series is running.")));
    s.folder = m_folder ? m_folder() : QDir(QDir::homePath()).filePath(QStringLiteral("Documents/LibreRamen"));
    m_haveProgress = false;
    m_progress = {};
    if (!m_ctl->startSeries(s, channel, c.origin))
        return reply(Response::error(409, QStringLiteral("the series could not be started")));
    Job& j = newJob(QStringLiteral("series"), c.origin);
    m_activeJob = j.id;
    reply(Response::json(202, j.toJson()));
}

void ApiServer::getProgress(const Call&, const Reply& r) { r(Response::json(200, progressJson())); }

void ApiServer::getJob(const Call& c, const Reply& r) {
    bool ok = false;
    const int id = c.args.value(0).toInt(&ok);
    if (!ok || !m_jobs.count(id))
        return r(Response::error(404, QStringLiteral("no job ") + c.args.value(0)));
    r(Response::json(200, m_jobs.at(id).toJson()));
}

void ApiServer::postStop(const Call& c, const Reply& r) {
    const bool wasBusy = m_ctl->isBusy();
    if (m_activeJob && m_jobs.count(m_activeJob))
        m_jobs[m_activeJob].stopRequested = true;
    m_ctl->stop();                                    // stop flag + laser(s) off right away
    log(c.origin, QStringLiteral("STOP (laser off)"));
    lr::Instrument* inst = m_ctl->instrument();
    r(Response::json(200, QJsonObject{{QStringLiteral("stopped"), true},
                                      {QStringLiteral("was_busy"), wasBusy},
                                      {QStringLiteral("laser_on"), inst ? inst->laserOn() : false}}));
}

bool ApiServer::requestData(const QJsonObject& body, lr::Vec* x, lr::Vec* y, const Reply& reply) const {
    if (body.contains(QStringLiteral("id"))) {
        const Stored* s = findSpectrum(body.value(QStringLiteral("id")).toInt(-1));
        if (!s) {
            reply(Response::error(404, QStringLiteral("no spectrum %1").arg(body.value(QStringLiteral("id")).toVariant().toString())));
            return false;
        }
        *x = s->sp.shift;
        *y = s->sp.intensity;
        return true;
    }
    const QJsonArray xa = body.value(QStringLiteral("x")).toArray(), ya = body.value(QStringLiteral("y")).toArray();
    if (xa.size() != ya.size() || xa.size() < 5) {
        reply(Response::error(400, QStringLiteral("give \"id\" of a spectrum, or \"x\" and \"y\" arrays of the same length (at least 5 points)")));
        return false;
    }
    for (qsizetype i = 0; i < xa.size(); ++i) {
        if (!xa[i].isDouble() || !ya[i].isDouble()) {
            reply(Response::error(400, QStringLiteral("x and y must contain numbers only")));
            return false;
        }
        x->push_back(xa[i].toDouble());
        y->push_back(ya[i].toDouble());
    }
    return true;
}

bool ApiServer::requestSettings(const QJsonObject& body, lr::processing::Settings* s, const Reply& reply) const {
    if (!body.contains(QStringLiteral("settings"))) {
        *s = m_processing ? m_processing() : lr::processing::Settings{};
        return true;
    }
    if (!body.value(QStringLiteral("settings")).isObject()) {
        reply(Response::error(400, QStringLiteral("settings must be an object")));
        return false;
    }
    const QJsonObject o = body.value(QStringLiteral("settings")).toObject();
    Fields f(o, {QStringLiteral("baseline"), QStringLiteral("strength"), QStringLiteral("fit_min"),
                 QStringLiteral("despike"), QStringLiteral("smooth"),
                 QStringLiteral("smooth_window"), QStringLiteral("derivative"), QStringLiteral("deriv_window"),
                 QStringLiteral("normalize"), QStringLiteral("band")});
    QStringList methods{QStringLiteral("none")};
    for (const auto& b : lr::processing::baselines())
        methods << b.first;
    f.choice(QStringLiteral("baseline"), {}, methods);
    f.num(QStringLiteral("strength"), 5, 0, 10);
    f.num(QStringLiteral("fit_min"), lr::processing::kBaselineFitMin, 0, 4000);
    f.integer(QStringLiteral("smooth_window"), 9, 3, 101);
    f.integer(QStringLiteral("derivative"), 0, 0, 2);
    f.integer(QStringLiteral("deriv_window"), 11, 5, 101);
    f.choice(QStringLiteral("normalize"), {}, {QStringLiteral("none"), QStringLiteral("max"), QStringLiteral("area"), QStringLiteral("band")});
    if (!f.error().isEmpty()) {
        reply(Response::error(400, QStringLiteral("settings: ") + f.error()));
        return false;
    }
    *s = lr::processing::Settings::fromMap(o.toVariantMap());
    return true;
}

namespace {
QJsonObject settingsJson(const lr::processing::Settings& s) {
    QJsonObject o{{QStringLiteral("baseline"), s.baseline},       {QStringLiteral("strength"), s.strength},
                  {QStringLiteral("fit_min"), s.fitMin},
                  {QStringLiteral("despike"), s.despike},         {QStringLiteral("smooth"), s.smooth},
                  {QStringLiteral("smooth_window"), s.smoothWindow}, {QStringLiteral("derivative"), s.derivative},
                  {QStringLiteral("deriv_window"), s.derivWindow}, {QStringLiteral("normalize"), s.normalize}};
    o.insert(QStringLiteral("band"), s.band ? QJsonValue(*s.band) : QJsonValue());
    return o;
}
}  // namespace

void ApiServer::postProcess(const Call& c, const Reply& reply) {
    QJsonObject body;
    QString err;
    if (!c.request.jsonBody(&body, &err))
        return reply(Response::error(400, err));
    Fields f(body, {QStringLiteral("id"), QStringLiteral("x"), QStringLiteral("y"), QStringLiteral("settings")});
    if (!f.error().isEmpty())
        return reply(Response::error(400, f.error()));
    lr::Vec x, y;
    lr::processing::Settings s;
    if (!requestData(body, &x, &y, reply) || !requestSettings(body, &s, reply))
        return;
    const auto [py, base] = lr::processing::process(x, y, s);
    reply(Response::json(200, QJsonObject{{QStringLiteral("x"), toArray(x)},
                                          {QStringLiteral("y"), toArray(py)},
                                          {QStringLiteral("baseline"), toArray(base)},
                                          {QStringLiteral("settings"), settingsJson(s)}}));
}

void ApiServer::postPeaks(const Call& c, const Reply& reply) {
    QJsonObject body;
    QString err;
    if (!c.request.jsonBody(&body, &err))
        return reply(Response::error(400, err));
    Fields f(body, {QStringLiteral("id"), QStringLiteral("x"), QStringLiteral("y"), QStringLiteral("settings"),
                    QStringLiteral("snr"), QStringLiteral("min_fwhm"), QStringLiteral("shoulders"),
                    QStringLiteral("xmin"), QStringLiteral("xmax")});
    const double snr = f.num(QStringLiteral("snr"), 5.0, 0.5, 1e6);
    const double minFwhm = f.num(QStringLiteral("min_fwhm"), 2.0, 0.0, 1e4);
    const bool shoulders = f.boolean(QStringLiteral("shoulders"), false);
    std::optional<double> xmin, xmax;
    if (f.has(QStringLiteral("xmin")))
        xmin = f.num(QStringLiteral("xmin"), 0, -1e6, 1e6);
    if (f.has(QStringLiteral("xmax")))
        xmax = f.num(QStringLiteral("xmax"), 0, -1e6, 1e6);
    if (!f.error().isEmpty())
        return reply(Response::error(400, f.error()));
    lr::Vec x, y;
    lr::processing::Settings s;
    if (!requestData(body, &x, &y, reply) || !requestSettings(body, &s, reply))
        return;
    const lr::Vec py = lr::processing::process(x, y, s).first;
    QJsonArray peaks;
    for (const lr::peaks::Peak& p : lr::peaks::pick(x, py, snr, minFwhm, shoulders, xmin, xmax))
        peaks.append(QJsonObject{{QStringLiteral("position"), p.position}, {QStringLiteral("height"), p.height},
                                 {QStringLiteral("fwhm"), p.fwhm},         {QStringLiteral("area"), p.area},
                                 {QStringLiteral("snr"), p.snr},           {QStringLiteral("shoulder"), p.shoulder}});
    reply(Response::json(200, QJsonObject{{QStringLiteral("peaks"), peaks},
                                          {QStringLiteral("noise"), lr::peaks::noiseLevel(py)},
                                          {QStringLiteral("settings"), settingsJson(s)}}));
}

void ApiServer::getStandards(const Call&, const Reply& r) {
    QJsonArray a;
    for (const lr::standards::Standard& s : lr::standards::all()) {
        QJsonArray bands;
        for (const lr::standards::Band& b : lr::standards::bands(s.name))
            bands.append(QJsonObject{{QStringLiteral("position"), b.position},
                                     {QStringLiteral("strength"), b.strength},
                                     {QStringLiteral("tolerance"), b.tolerance}});
        a.append(QJsonObject{{QStringLiteral("name"), s.name},
                             {QStringLiteral("kind"), s.kind},
                             {QStringLiteral("source"), s.source},
                             {QStringLiteral("note"), s.note},
                             {QStringLiteral("aka"), QJsonArray::fromStringList(s.aka)},
                             {QStringLiteral("bands"), bands}});
    }
    r(Response::json(200, QJsonObject{{QStringLiteral("standards"), a}}));
}

void ApiServer::getLog(const Call& c, const Reply& r) {
    const qint64 since = c.request.query.queryItemValue(QStringLiteral("since")).toLongLong();
    QJsonArray lines;
    for (const LogLine& l : m_log)
        if (l.seq > since)
            lines.append(QJsonObject{{QStringLiteral("seq"), l.seq}, {QStringLiteral("time"), lr::iso(l.t)},
                                     {QStringLiteral("text"), l.text}});
    r(Response::json(200, QJsonObject{{QStringLiteral("lines"), lines}, {QStringLiteral("next"), m_nextLog - 1}}));
}

}  // namespace lr::gui
