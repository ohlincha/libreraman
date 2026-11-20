#pragma once
// Local HTTP/JSON API of the running application (docs/SPECIFICATION.md sections 2, 4, 8). The GUI owns the
// instrument (Controller); the API is one more client of the same Controller, so the GUI, the command line
// (`LibreRamenCLI --remote`), Python (libreramen/client.py) and AI agents (LibreRamenMCP) all see the same
// state and the same safety rules.
//
// Security and laser safety:
//  * listens on 127.0.0.1 only; every request needs "Authorization: Bearer <token>" (random token per
//    start, written to the API file with user-only permissions, see core/apiclient.h); Host must be a
//    loopback name (DNS rebinding);
//  * remote clients are read-only unless "Allow remote measurements" is switched on at the instrument
//    (GUI: Instrument menu; persisted). Without it, POST /measure and /series answer 403. The laser itself
//    still needs ARM on the instrument and a closed compartment (Instrument::acquire());
//  * POST /stop is always allowed (stop + laser off at once);
//  * limits (exposure, scans, pre-bleach, series) are checked here for remote clients, as the GUI's fields do;
//  * every remote action is logged with its origin "API (<client>)" (the X-LibreRamen-Client header; the
//    command line sends "cli", the MCP server "mcp", Python "python").
//
// Endpoints (all JSON; errors: {"error": "...", "status": N}):
//   GET  /status              connection, ARM, compartment, laser, channel, busy, remote control, job
//   GET  /info                software/instrument/API information, limits, endpoint list
//   GET  /spectra[?limit=N]   recent spectra (newest last; metadata only)
//   GET  /spectra/{id}        one spectrum with data (shift, intensity, signal, background, meta)
//   POST /measure             {exposure_s, averages, background: each|none, prebleach_s, sample, channel,
//                              wait} -> 202 job (or, with "wait": true, 200 when finished)
//   POST /series              {sample, exposure_s, averages, background: each|once|none, prebleach_s,
//                              interval_s, count | duration_s, start_delay_s, laser_stays_on} -> 202 job
//   GET  /series/progress     progress of the running (or last) series
//   GET  /jobs/{id}           state of a job started through the API: running|done|failed|stopped
//   POST /stop                stop + laser off (always allowed)
//   POST /process             {id | x, y, settings} -> processed spectrum and baseline
//   POST /peaks               {id | x, y, settings, snr, min_fwhm, shoulders, xmin, xmax} -> peaks
//   GET  /standards           reference standards (calibration)
//   POST /match               library search: 501 until the library registers a handler (addRoute)
//   GET  /log[?since=N]       log lines with sequence numbers
//
// Thread safety: GUI thread only (handlers run in the GUI thread, like the GUI's own controls).
#include <QDateTime>
#include <QJsonObject>
#include <QObject>
#include <QString>
#include <QStringList>
#include <deque>
#include <functional>
#include <map>
#include <vector>

#include "core/httpserver.h"
#include "core/processing.h"
#include "core/series.h"
#include "core/spectrum.h"

namespace lr::gui {

class Controller;

class ApiServer : public QObject {
    Q_OBJECT
public:
    // Handler of a route: args are the {placeholders} of the pattern in order; origin is "API (<client>)".
    struct Call {
        const lr::http::Request& request;
        QStringList args;
        QString origin;
    };
    using Route = std::function<void(const Call&, const lr::http::Reply&)>;

    explicit ApiServer(Controller* ctl, QObject* parent = nullptr);
    ~ApiServer() override;                    // stop()

    // Listens on 127.0.0.1:port (0 = any free port) and writes the API file. false and *error on failure
    // (e.g. the port is used by another LibreRamen); the API file is then left alone.
    bool start(quint16 port, const QString& apiFile, QString* error = nullptr);
    void stop();                              // stops listening and removes the API file
    bool isRunning() const;
    quint16 port() const;
    QByteArray token() const { return m_token; }
    QString apiFile() const { return m_apiFile; }

    bool remoteControl() const { return m_remoteControl; }
    void setRemoteControl(bool allowed);      // logs the change ("GUI: remote measurements allowed")

    // Where measurements started through the API are saved (the Measure page's folder), and the processing
    // used by /process and /peaks when the request gives no settings (the Measure page's).
    void setDataFolder(std::function<QString()> f) { m_folder = std::move(f); }
    void setProcessing(std::function<lr::processing::Settings()> f) { m_processing = std::move(f); }

    // Extension point: adds or replaces a route, e.g. addRoute("POST", "/match", ...) by the library.
    // Patterns may contain "{name}" segments ("/spectra/{id}").
    void addRoute(const QByteArray& method, const QString& pattern, Route route);

    // Log lines for GET /log (the main window passes every line of its log view).
    void recordLog(const QString& line);

    // Spectra kept for /spectra (most recent; older ones are on disk only).
    static constexpr int kKeepSpectra = 100;

signals:
    void logMessage(const QString& msg);
    void remoteControlChanged(bool allowed);
    void runningChanged(bool running);

private:
    struct RouteEntry {
        QByteArray method;
        QStringList pattern;                  // path segments
        Route route;
    };
    struct Job {
        int id = 0;
        QString kind;                         // "measure" | "series"
        QString origin;
        QString state = QStringLiteral("running");
        QString error;
        std::vector<int> spectra;
        QDateTime started, finished;
        bool stopRequested = false;
        std::vector<lr::http::Reply> waiters;
        QJsonObject toJson() const;
    };
    struct Stored {
        int id;
        lr::Spectrum sp;
    };

    void handle(const lr::http::Request& req, const lr::http::Reply& reply);
    void registerRoutes();
    void log(const QString& origin, const QString& msg);
    const Stored* findSpectrum(int id) const;
    QJsonObject spectrumSummary(const Stored& s) const;
    QJsonObject statusJson() const;
    QJsonObject progressJson() const;
    void onSpectrum(const lr::Spectrum& sp);
    void onBusyChanged(bool busy);
    Job& newJob(const QString& kind, const QString& origin);

    // route handlers
    void getStatus(const Call&, const lr::http::Reply&);
    void getInfo(const Call&, const lr::http::Reply&);
    void getSpectra(const Call&, const lr::http::Reply&);
    void getSpectrum(const Call&, const lr::http::Reply&);
    void postMeasure(const Call&, const lr::http::Reply&);
    void postSeries(const Call&, const lr::http::Reply&);
    void getProgress(const Call&, const lr::http::Reply&);
    void getJob(const Call&, const lr::http::Reply&);
    void postStop(const Call&, const lr::http::Reply&);
    void postProcess(const Call&, const lr::http::Reply&);
    void postPeaks(const Call&, const lr::http::Reply&);
    void getStandards(const Call&, const lr::http::Reply&);
    void getLog(const Call&, const lr::http::Reply&);
    // common checks of /measure and /series; false if a reply was sent
    bool mayControl(const Call& c, const lr::http::Reply& reply);
    // spectrum data of a /process or /peaks request (by id or x/y); false if a reply was sent
    bool requestData(const QJsonObject& body, lr::Vec* x, lr::Vec* y, const lr::http::Reply& reply) const;
    bool requestSettings(const QJsonObject& body, lr::processing::Settings* s, const lr::http::Reply& reply) const;

    Controller* m_ctl;
    lr::http::Server m_http;
    QByteArray m_token;
    QString m_apiFile;
    bool m_remoteControl = false;
    std::function<QString()> m_folder;
    std::function<lr::processing::Settings()> m_processing;
    std::vector<RouteEntry> m_routes;

    std::deque<Stored> m_spectra;
    int m_nextSpectrum = 1;
    std::map<int, Job> m_jobs;
    int m_nextJob = 1;
    int m_activeJob = 0;                      // API job in progress (0: none / started by the GUI)
    lr::SeriesProgress m_progress;
    bool m_haveProgress = false;
    bool m_seriesRunning = false;

    struct LogLine {
        qint64 seq;
        QDateTime t;
        QString text;
    };
    std::deque<LogLine> m_log;
    qint64 m_nextLog = 1;
};

}  // namespace lr::gui
