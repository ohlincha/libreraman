// Local API (gui/apiserver.h) with the main window and the simulator at high speed (offscreen, 127.0.0.1 only):
// token required, status/info, remote measurements refused unless allowed at the instrument, busy (409),
// stop always allowed (laser off), series progress, processing/peaks, the command line (--remote) and the
// MCP server (LibreRamenMCP) as clients.
#include "gui/apiserver.h"
#include "gui/controller.h"
#include "gui/mainwindow.h"
#include "gui/measurepage.h"
#include "gui/page.h"
#include "gui/statusstrip.h"
#include "gui/theme.h"

#include "core/apiclient.h"
#include "core/simulator.h"

#include <QAction>
#include <QDir>
#include <QElapsedTimer>
#include <QApplication>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPlainTextEdit>
#include <QProcess>
#include <QSettings>
#include <QSignalSpy>
#include <QTcpSocket>
#include <QTemporaryDir>
#include <QTest>

#include <chrono>
#include <future>

using namespace lr::gui;

namespace {

const QJsonValue kNoBody(QJsonValue::Undefined);

struct Proc {
    int code = -1;
    QByteArray out;
};

}  // namespace

class GuiTestApi : public QObject {
    Q_OBJECT
private slots:
    void initTestCase();
    void cleanupTestCase();
    void cleanup();
    void apiFileAndToken();
    void statusAndInfo();
    void measureRefusedWithoutPermission();
    void measureAllowed();
    void validation();
    void busyAndStop();
    void stopAlwaysAllowed();
    void seriesProgress();
    void processingAndPeaks();
    void standardsAndLog();
    void matchExtensionPoint();
    void cliRemote();
    void mcpServer();
    void stopRemovesApiFile();

private:
    // A request from another thread (the server runs in this one): the event loop keeps running meanwhile.
    lr::api::Reply call(const QByteArray& method, const QString& path, const QJsonValue& body = kNoBody,
                        const QByteArray& token = QByteArray("<server>"), int timeoutMs = 30000);
    QJsonObject waitJob(int id, int timeoutMs = 20000);
    Proc run(const QString& exe, const QStringList& args, int timeoutMs = 30000);
    void setControl(bool on) { m_win->remoteControlAction()->setChecked(on); }
    QString log() const { return m_win->logView()->toPlainText(); }

    QTemporaryDir m_tmp;
    QTemporaryDir m_settingsDir;
    QString m_apiFile;
    Controller* m_ctl = nullptr;
    MainWindow* m_win = nullptr;
};

lr::api::Reply GuiTestApi::call(const QByteArray& method, const QString& path, const QJsonValue& body,
                                const QByteArray& token, int timeoutMs) {
    lr::api::Endpoint ep;
    ep.port = m_win->apiServer()->port();
    ep.token = token == "<server>" ? m_win->apiServer()->token() : token;
    auto f = std::async(std::launch::async, [=] { return lr::api::request(ep, method, path, body, timeoutMs, "test"); });
    while (f.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    return f.get();
}

QJsonObject GuiTestApi::waitJob(int id, int timeoutMs) {
    QJsonObject job;
    QElapsedTimer t;
    t.start();
    do {
        job = call("GET", QStringLiteral("/jobs/%1").arg(id)).json.toObject();
        if (job.value("state").toString() != QLatin1String("running"))
            break;
        QTest::qWait(50);
    } while (t.elapsed() < timeoutMs);
    return job;
}

Proc GuiTestApi::run(const QString& exe, const QStringList& args, int timeoutMs) {
    QProcess p;
    p.start(exe, args);
    if (!p.waitForStarted())
        return {};
    p.closeWriteChannel();
    QElapsedTimer t;
    t.start();
    while (p.state() != QProcess::NotRunning && t.elapsed() < timeoutMs)
        QTest::qWait(20);                               // (the server needs this thread's event loop)
    if (p.state() != QProcess::NotRunning) {
        p.kill();
        p.waitForFinished();
        return {};
    }
    return {p.exitStatus() == QProcess::NormalExit ? p.exitCode() : -1, p.readAllStandardOutput()};
}

void GuiTestApi::initTestCase() {
    QVERIFY(m_tmp.isValid());
    QCoreApplication::setOrganizationName(QStringLiteral("LibreRamenTest"));
    QCoreApplication::setApplicationName(QStringLiteral("gui_test_api"));
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, m_settingsDir.path());
    m_apiFile = m_tmp.filePath(QStringLiteral("config/api.json"));
    qputenv("LIBRERAMEN_API_FILE", m_apiFile.toUtf8());      // also for the CLI and MCP child processes
    theme::apply(*qApp, theme::Mode::Light);
    setDialogsEnabled(false);
    m_ctl = new Controller;
    m_ctl->setSimulatorSpeed(50);
    m_ctl->setPollInterval(50);
    m_win = new MainWindow(m_ctl);
    m_win->show();
    m_win->measurePage()->setFolder(m_tmp.filePath(QStringLiteral("data")));
    QCOMPARE(m_win->statusStrip()->remoteTile()->mainText(), QStringLiteral("○ OFF"));
    QVERIFY(m_win->startApi(0));                              // any free port; API file from the environment
    QVERIFY(m_win->apiServer()->isRunning());
    QVERIFY(m_win->apiServer()->port() > 0);
    QCOMPARE(m_win->statusStrip()->remoteTile()->mainText(), QStringLiteral("READ-ONLY"));
    QSignalSpy connected(m_ctl, &Controller::connected);
    m_win->connectInstrument(true);
    QVERIFY(connected.wait(10000));
    m_ctl->simSetSample(QStringLiteral("carbon tetrachloride"));
}

void GuiTestApi::cleanupTestCase() {
    delete m_win;
    delete m_ctl;
}

void GuiTestApi::cleanup() {
    if (m_ctl->isBusy()) {
        m_ctl->stop();
        QTRY_VERIFY_WITH_TIMEOUT(!m_ctl->isBusy(), 10000);
    }
}

void GuiTestApi::apiFileAndToken() {
    QFile f(m_apiFile);
    QVERIFY(f.open(QIODevice::ReadOnly));
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    QCOMPARE(o.value("port").toInt(), int(m_win->apiServer()->port()));
    QCOMPARE(o.value("token").toString().toLatin1(), m_win->apiServer()->token());
    QCOMPARE(o.value("token").toString().size(), 64);
    QCOMPARE(o.value("url").toString(), QStringLiteral("http://127.0.0.1:%1").arg(m_win->apiServer()->port()));
#if !defined(Q_OS_WIN)
    QCOMPARE(f.permissions() & (QFileDevice::ReadGroup | QFileDevice::ReadOther | QFileDevice::WriteGroup |
                                QFileDevice::WriteOther),
             QFileDevice::Permissions());
#endif
    const lr::api::Endpoint ep = lr::api::findServer();
    QCOMPARE(ep.port, m_win->apiServer()->port());
    QCOMPARE(ep.token, m_win->apiServer()->token());

    // without or with a wrong token: 401
    lr::api::Reply r = call("GET", "/status", kNoBody, QByteArray());
    QCOMPARE(r.status, 401);
    r = call("GET", "/status", kNoBody, QByteArray(64, 'a'));
    QCOMPARE(r.status, 401);
    QVERIFY(r.json.toObject().value("error").toString().contains("token"));
    r = call("POST", "/stop", QJsonObject{}, QByteArray("x"));   // stop needs the token too
    QCOMPARE(r.status, 401);
    QCOMPARE(call("GET", "/status").status, 200);

    // foreign Host header (DNS rebinding) is refused
    auto raw = std::async(std::launch::async, [port = m_win->apiServer()->port(), token = m_win->apiServer()->token()] {
        QTcpSocket s;
        s.connectToHost(QStringLiteral("127.0.0.1"), port);
        if (!s.waitForConnected(3000))
            return QByteArray();
        s.write("GET /status HTTP/1.1\r\nHost: evil.example:80\r\nAuthorization: Bearer " + token + "\r\n\r\n");
        QByteArray in;
        while (s.waitForReadyRead(3000))
            in += s.readAll();
        return in;
    });
    while (raw.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    QVERIFY(raw.get().startsWith("HTTP/1.1 403"));
}

void GuiTestApi::statusAndInfo() {
    lr::api::Reply r = call("GET", "/status");
    QCOMPARE(r.status, 200);
    QJsonObject s = r.json.toObject();
    for (const char* k : {"connection", "simulated", "armed", "compartment_open", "laser_on", "channel", "busy",
                          "series_running", "paused", "remote_control", "job"})
        QVERIFY2(s.contains(k), k);
    QCOMPARE(s.value("connection").toString(), QStringLiteral("connected"));
    QCOMPARE(s.value("simulated").toBool(), true);
    QCOMPARE(s.value("laser_on").toBool(), false);
    QCOMPARE(s.value("channel").toInt(), 532);
    QCOMPARE(s.value("busy").toBool(), false);
    QCOMPARE(s.value("remote_control").toBool(), false);
    QVERIFY(s.value("job").isNull());

    r = call("GET", "/info");
    QCOMPARE(r.status, 200);
    const QJsonObject i = r.json.toObject();
    QCOMPARE(i.value("api_version").toInt(), lr::api::kApiVersion);
    QCOMPARE(i.value("instrument").toObject().value("serial").toString(), QStringLiteral("SIMULATOR"));
    QCOMPARE(i.value("data_folder").toString(), m_tmp.filePath(QStringLiteral("data")));
    const QJsonArray eps = i.value("endpoints").toArray();
    for (const char* e : {"GET /status", "POST /measure", "POST /series", "POST /stop", "GET /spectra/{id}", "POST /match"})
        QVERIFY2(eps.contains(QString::fromLatin1(e)), e);
    QVERIFY(i.value("limits").toObject().contains("exposure_s"));

    QCOMPARE(call("GET", "/nope").status, 404);
    QCOMPARE(call("GET", "/measure").status, 405);
    QCOMPARE(call("GET", "/spectra/999").status, 404);
}

void GuiTestApi::measureRefusedWithoutPermission() {
    QVERIFY(!m_win->apiServer()->remoteControl());
    m_ctl->simPressArm();
    QSignalSpy busy(m_ctl, &Controller::busyChanged);
    const lr::api::Reply r = call("POST", "/measure", QJsonObject{{"exposure_s", 0.05}});
    QCOMPARE(r.status, 403);
    QVERIFY(r.json.toObject().value("error").toString().contains("Allow remote measurements"));
    QCOMPARE(call("POST", "/series", QJsonObject{{"count", 2}}).status, 403);
    QVERIFY(busy.isEmpty());
    QVERIFY(!m_ctl->isBusy());
    QVERIFY(log().contains("API (test): measurement refused"));
}

void GuiTestApi::measureAllowed() {
    setControl(true);
    QVERIFY(m_win->apiServer()->remoteControl());
    QVERIFY(QSettings().value("api/allowRemoteControl").toBool());            // persisted
    QCOMPARE(m_win->statusStrip()->remoteTile()->mainText(), QStringLiteral("▲ CONTROL"));
    QVERIFY(log().contains("GUI: remote measurements allowed"));

    // not armed: the job fails with the instrument's message (the laser is never switched on)
    m_ctl->simOpenCompartment();
    m_ctl->simCloseCompartment();
    QTRY_VERIFY(!m_ctl->lastStatus().armed);
    lr::api::Reply r = call("POST", "/measure", QJsonObject{{"exposure_s", 0.05}, {"wait", true}});
    QCOMPARE(r.status, 200);
    QCOMPARE(r.json.toObject().value("state").toString(), QStringLiteral("failed"));
    QVERIFY(r.json.toObject().value("error").toString().contains("Not armed"));

    // armed: one spectrum, waited for
    m_ctl->simPressArm();
    r = call("POST", "/measure",
             QJsonObject{{"exposure_s", 0.05}, {"averages", 2}, {"sample", "CCl4 api"}, {"wait", true}});
    QCOMPARE(r.status, 200);
    const QJsonObject job = r.json.toObject();
    QCOMPARE(job.value("state").toString(), QStringLiteral("done"));
    QCOMPARE(job.value("kind").toString(), QStringLiteral("measure"));
    QCOMPARE(job.value("origin").toString(), QStringLiteral("API (test)"));
    QCOMPARE(job.value("spectra").toArray().size(), 1);
    const int id = job.value("spectra").toArray().at(0).toInt();
    QVERIFY(log().contains("API (test): measure \"CCl4 api\": 0.05 s × 2"));

    r = call("GET", QStringLiteral("/spectra/%1").arg(id));
    QCOMPARE(r.status, 200);
    const QJsonObject sp = r.json.toObject();
    QCOMPARE(sp.value("sample").toString(), QStringLiteral("CCl4 api"));
    QCOMPARE(sp.value("averages").toInt(), 2);
    QVERIFY(sp.value("points").toInt() > 1000);
    QCOMPARE(sp.value("shift").toArray().size(), sp.value("points").toInt());
    QCOMPARE(sp.value("intensity").toArray().size(), sp.value("points").toInt());
    QVERIFY(QFileInfo::exists(sp.value("file").toString()));
    QCOMPARE(QFileInfo(sp.value("file").toString()).absolutePath(), QDir(m_tmp.filePath("data")).absolutePath());

    r = call("GET", "/spectra?limit=1");
    QCOMPARE(r.json.toObject().value("spectra").toArray().size(), 1);
    QCOMPARE(r.json.toObject().value("spectra").toArray().at(0).toObject().value("id").toInt(), id);

    // without wait: 202 and a job to poll
    r = call("POST", "/measure", QJsonObject{{"exposure_s", 0.05}});
    QCOMPARE(r.status, 202);
    QCOMPARE(r.json.toObject().value("state").toString(), QStringLiteral("running"));
    const QJsonObject done = waitJob(r.json.toObject().value("id").toInt());
    QCOMPARE(done.value("state").toString(), QStringLiteral("done"));
    QTRY_VERIFY(!m_ctl->isBusy());
}

void GuiTestApi::validation() {
    setControl(true);
    struct Case {
        QJsonObject body;
        const char* error;
    };
    const Case cases[] = {{{{"exposure_s", 1000}}, "exposure_s must be between"},
                          {{{"exposure_s", "1"}}, "exposure_s must be a number"},
                          {{{"averages", 1.5}}, "averages must be an integer"},
                          {{{"background", "once"}}, "background must be one of"},
                          {{{"channel", 785}}, "channel must be"},
                          {{{"exposure", 1}}, "unknown field \"exposure\""}};
    for (const Case& c : cases) {
        const lr::api::Reply r = call("POST", "/measure", c.body);
        QCOMPARE(r.status, 400);
        QVERIFY2(r.error.contains(QLatin1String(c.error)), qPrintable(r.error));
    }
    QCOMPARE(call("POST", "/series", QJsonObject{{"count", 3}, {"duration_s", 10}}).status, 400);
    // invalid JSON
    auto raw = std::async(std::launch::async, [port = m_win->apiServer()->port(), token = m_win->apiServer()->token()] {
        QTcpSocket s;
        s.connectToHost(QStringLiteral("127.0.0.1"), port);
        if (!s.waitForConnected(3000))
            return QByteArray();
        s.write("POST /measure HTTP/1.1\r\nHost: 127.0.0.1\r\nAuthorization: Bearer " + token +
                "\r\nContent-Length: 5\r\n\r\n{nope");
        QByteArray in;
        while (s.waitForReadyRead(3000))
            in += s.readAll();
        return in;
    });
    while (raw.wait_for(std::chrono::milliseconds(0)) != std::future_status::ready)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    QVERIFY(raw.get().startsWith("HTTP/1.1 400"));
    QVERIFY(!m_ctl->isBusy());
}

void GuiTestApi::busyAndStop() {
    setControl(true);
    m_ctl->simPressArm();
    lr::api::Reply r = call("POST", "/series",
                            QJsonObject{{"sample", "long"}, {"exposure_s", 0.05}, {"interval_s", 1.0}, {"count", 1000}});
    QCOMPARE(r.status, 202);
    const int job = r.json.toObject().value("id").toInt();
    QCOMPARE(r.json.toObject().value("kind").toString(), QStringLiteral("series"));
    QTRY_VERIFY(m_ctl->isSeriesRunning());
    // busy: a second measurement or series is refused
    QCOMPARE(call("POST", "/measure", QJsonObject{{"exposure_s", 0.05}}).status, 409);
    QCOMPARE(call("POST", "/series", QJsonObject{{"count", 2}}).status, 409);
    QVERIFY(call("GET", "/status").json.toObject().value("busy").toBool());
    QCOMPARE(call("GET", "/status").json.toObject().value("job").toObject().value("id").toInt(), job);
    // progress
    QTRY_VERIFY_WITH_TIMEOUT(call("GET", "/series/progress").json.toObject().value("n").toInt() >= 2, 10000);
    const QJsonObject p = call("GET", "/series/progress").json.toObject();
    QCOMPARE(p.value("running").toBool(), true);
    QCOMPARE(p.value("total").toInt(), 1000);
    QCOMPARE(p.value("done").toBool(), false);
    // stop: laser off at once, job "stopped"
    r = call("POST", "/stop", QJsonObject{});
    QCOMPARE(r.status, 200);
    QCOMPARE(r.json.toObject().value("was_busy").toBool(), true);
    QCOMPARE(r.json.toObject().value("laser_on").toBool(), false);
    QVERIFY(!m_ctl->instrument()->laserOn());
    QTRY_VERIFY_WITH_TIMEOUT(!m_ctl->isBusy(), 10000);
    const QJsonObject j = waitJob(job);
    QCOMPARE(j.value("state").toString(), QStringLiteral("stopped"));
    QVERIFY(j.value("spectra").toArray().size() >= 2);
    QCOMPARE(call("GET", "/series/progress").json.toObject().value("running").toBool(), false);
    QVERIFY(log().contains("API (test): STOP (laser off)"));
}

void GuiTestApi::stopAlwaysAllowed() {
    setControl(false);
    QCOMPARE(m_win->statusStrip()->remoteTile()->mainText(), QStringLiteral("READ-ONLY"));
    m_ctl->simPressArm();
    MeasureSettings s;                                          // started at the instrument (GUI)
    s.folder = m_tmp.filePath(QStringLiteral("data"));
    s.exposure_s = 100;                                         // 2 s at speed 50, twice
    QVERIFY(m_ctl->measureOnce(s));
    QVERIFY(log().contains("GUI: measure"));
    QTRY_VERIFY_WITH_TIMEOUT(m_ctl->instrument()->laserOn(), 10000);
    QVERIFY(call("GET", "/status").json.toObject().value("laser_on").toBool());
    QCOMPARE(call("POST", "/measure", QJsonObject{}).status, 403);   // read-only ...
    const lr::api::Reply r = call("POST", "/stop", QJsonObject{});   // ... but stop is always allowed
    QCOMPARE(r.status, 200);
    QCOMPARE(r.json.toObject().value("was_busy").toBool(), true);
    QCOMPARE(r.json.toObject().value("laser_on").toBool(), false);
    QVERIFY(!m_ctl->instrument()->laserOn());
    QTRY_VERIFY_WITH_TIMEOUT(!m_ctl->isBusy(), 5000);
    QTRY_VERIFY(!call("GET", "/status").json.toObject().value("laser_on").toBool());
}

void GuiTestApi::seriesProgress() {
    setControl(true);
    m_ctl->simPressArm();
    const lr::api::Reply r = call("POST", "/series", QJsonObject{{"sample", "short"}, {"exposure_s", 0.05},
                                                                {"interval_s", 0.1}, {"count", 3}, {"background", "once"}});
    QCOMPARE(r.status, 202);
    const QJsonObject j = waitJob(r.json.toObject().value("id").toInt());
    QCOMPARE(j.value("state").toString(), QStringLiteral("done"));
    QCOMPARE(j.value("spectra").toArray().size(), 3);
    const QJsonObject p = call("GET", "/series/progress").json.toObject();
    QCOMPARE(p.value("running").toBool(), false);
    QCOMPARE(p.value("n").toInt(), 3);
    QCOMPARE(p.value("done").toBool(), true);
}

void GuiTestApi::processingAndPeaks() {
    const QJsonArray list = call("GET", "/spectra").json.toObject().value("spectra").toArray();
    QVERIFY(!list.isEmpty());
    const int id = list.last().toObject().value("id").toInt();
    lr::api::Reply r = call("POST", "/process", QJsonObject{{"id", id}, {"settings", QJsonObject{{"baseline", "snip"}}}});
    QCOMPARE(r.status, 200);
    QJsonObject o = r.json.toObject();
    QCOMPARE(o.value("y").toArray().size(), o.value("x").toArray().size());
    QCOMPARE(o.value("baseline").toArray().size(), o.value("x").toArray().size());
    QCOMPARE(o.value("settings").toObject().value("baseline").toString(), QStringLiteral("snip"));
    QCOMPARE(call("POST", "/process", QJsonObject{{"id", id}, {"settings", QJsonObject{{"baseline", "magic"}}}}).status, 400);

    // peaks of CCl4 (459 cm-1 strongest)
    r = call("POST", "/peaks", QJsonObject{{"id", id}, {"settings", QJsonObject{{"baseline", "snip"}}}});
    QCOMPARE(r.status, 200);
    const QJsonArray peaks = r.json.toObject().value("peaks").toArray();
    QVERIFY(!peaks.isEmpty());
    bool found459 = false;
    for (const QJsonValue& v : peaks) {
        const QJsonObject p = v.toObject();
        QVERIFY(p.contains("height") && p.contains("fwhm") && p.contains("snr"));
        found459 |= std::abs(p.value("position").toDouble() - 459) < 4;
    }
    QVERIFY(found459);

    // uploaded data: a Lorentzian at 1000 cm-1
    QJsonArray x, y;
    for (int i = 0; i < 400; ++i) {
        const double xi = 800 + i;
        x.append(xi);
        y.append(1000.0 / (1 + std::pow((xi - 1000) / 5, 2)) + (i % 2 ? 1 : -1));
    }
    r = call("POST", "/peaks", QJsonObject{{"x", x}, {"y", y}, {"settings", QJsonObject{{"baseline", "none"}}}});
    QCOMPARE(r.status, 200);
    const QJsonArray up = r.json.toObject().value("peaks").toArray();
    QCOMPARE(up.size(), 1);
    QVERIFY(std::abs(up.at(0).toObject().value("position").toDouble() - 1000) < 0.5);
    QVERIFY(std::abs(up.at(0).toObject().value("fwhm").toDouble() - 10) < 1.5);
    QCOMPARE(call("POST", "/peaks", QJsonObject{{"x", QJsonArray{1, 2}}, {"y", QJsonArray{1, 2}}}).status, 400);
}

void GuiTestApi::standardsAndLog() {
    lr::api::Reply r = call("GET", "/standards");
    QCOMPARE(r.status, 200);
    const QJsonArray st = r.json.toObject().value("standards").toArray();
    QVERIFY(st.size() >= 5);
    QVERIFY(!st.at(0).toObject().value("bands").toArray().isEmpty());

    r = call("GET", "/log");
    QCOMPARE(r.status, 200);
    const QJsonArray lines = r.json.toObject().value("lines").toArray();
    QVERIFY(!lines.isEmpty());
    const qint64 next = r.json.toObject().value("next").toInteger();
    QCOMPARE(lines.last().toObject().value("seq").toInteger(), next);
    m_win->addLog(QStringLiteral("marker line"));
    r = call("GET", QStringLiteral("/log?since=%1").arg(next));
    const QJsonArray newer = r.json.toObject().value("lines").toArray();
    QCOMPARE(newer.size(), 1);
    QCOMPARE(newer.at(0).toObject().value("text").toString(), QStringLiteral("marker line"));
}

void GuiTestApi::matchExtensionPoint() {
    QCOMPARE(call("POST", "/match", QJsonObject{{"id", 1}}).status, 501);
    m_win->apiServer()->addRoute("POST", "/match", [](const ApiServer::Call& c, const lr::http::Reply& reply) {
        reply(lr::http::Response::json(200, QJsonObject{{"hits", QJsonArray{}}, {"origin", c.origin}}));
    });
    const lr::api::Reply r = call("POST", "/match", QJsonObject{{"id", 1}});
    QCOMPARE(r.status, 200);
    QCOMPARE(r.json.toObject().value("origin").toString(), QStringLiteral("API (test)"));
}

void GuiTestApi::cliRemote() {
    const QString cli = QStringLiteral(CLI_PATH);
    Proc p = run(cli, {"--remote", "status", "--json"});
    QCOMPARE(p.code, 0);
    const QJsonObject s = QJsonDocument::fromJson(p.out).object();
    QCOMPARE(s.value("connection").toString(), QStringLiteral("connected"));

    p = run(cli, {"--remote", "status"});
    QCOMPARE(p.code, 0);
    QVERIFY(p.out.contains("connection: connected (simulator)"));

    setControl(false);
    p = run(cli, {"--remote", "measure", "-e", "0.05", "--json"});
    QCOMPARE(p.code, 4);                                       // not allowed
    QCOMPARE(QJsonDocument::fromJson(p.out).object().value("status").toInt(), 403);
    QVERIFY(log().contains("API (cli): measurement refused"));

    setControl(true);
    m_ctl->simPressArm();
    p = run(cli, {"--remote", "measure", "-e", "0.05", "--sample", "from cli"});
    QCOMPARE(p.code, 0);
    QVERIFY2(p.out.contains("(measure): done, spectra"), p.out.constData());
    QVERIFY(log().contains("API (cli): measure \"from cli\""));
    const int id = call("GET", "/spectra?limit=1").json.toObject().value("spectra").toArray().at(0).toObject().value("id").toInt();
    const QString csv = m_tmp.filePath(QStringLiteral("cli.csv"));
    p = run(cli, {"--remote", "spectrum", QString::number(id), "-o", csv});
    QCOMPARE(p.code, 0);
    QVERIFY(QFileInfo(csv).size() > 1000);

    p = run(cli, {"--remote", "stop"});
    QCOMPARE(p.code, 0);
    QVERIFY(p.out.contains("laser off"));

    p = run(cli, {"--remote", "status", "--api-file", m_tmp.filePath("missing.json")});
    QCOMPARE(p.code, 3);                                       // not running
    p = run(cli, {"--remote", "bogus"});
    QCOMPARE(p.code, 2);

    // --json for the local commands
    p = run(cli, {"standards", "--json"});
    QCOMPARE(p.code, 0);
    QVERIFY(QJsonDocument::fromJson(p.out).object().value("standards").toArray().size() >= 5);
}

void GuiTestApi::mcpServer() {
    setControl(true);
    m_ctl->simPressArm();
    QProcess mcp;
    mcp.start(QStringLiteral(MCP_PATH), {});
    QVERIFY(mcp.waitForStarted());
    int nextId = 1;
    auto rpc = [&](const QString& method, const QJsonObject& params) -> QJsonObject {
        const int id = nextId++;
        mcp.write(QJsonDocument(QJsonObject{{"jsonrpc", "2.0"}, {"id", id}, {"method", method}, {"params", params}})
                      .toJson(QJsonDocument::Compact) + "\n");
        QElapsedTimer t;
        t.start();
        while (!mcp.canReadLine() && t.elapsed() < 30000)
            QTest::qWait(10);
        const QJsonObject reply = QJsonDocument::fromJson(mcp.readLine()).object();
        if (reply.value("id").toInt() != id)
            return {};
        return reply;
    };
    auto toolJson = [](const QJsonObject& reply) {
        const QJsonObject res = reply.value("result").toObject();
        return QJsonDocument::fromJson(res.value("content").toArray().at(0).toObject().value("text").toString().toUtf8()).object();
    };

    QJsonObject r = rpc("initialize", {{"protocolVersion", "2025-06-18"},
                                       {"capabilities", QJsonObject{}},
                                       {"clientInfo", QJsonObject{{"name", "test"}, {"version", "1"}}}});
    QCOMPARE(r.value("result").toObject().value("protocolVersion").toString(), QStringLiteral("2025-06-18"));
    QCOMPARE(r.value("result").toObject().value("serverInfo").toObject().value("name").toString(), QStringLiteral("libreramen"));
    QVERIFY(r.value("result").toObject().value("capabilities").toObject().contains("tools"));
    mcp.write(R"({"jsonrpc":"2.0","method":"notifications/initialized"})" "\n");   // no reply

    r = rpc("tools/list", {});
    QStringList names;
    for (const QJsonValue& t : r.value("result").toObject().value("tools").toArray()) {
        names << t.toObject().value("name").toString();
        QCOMPARE(t.toObject().value("inputSchema").toObject().value("type").toString(), QStringLiteral("object"));
        QVERIFY(!t.toObject().value("description").toString().isEmpty());
    }
    for (const char* n : {"status", "acquire", "run_series", "stop", "get_spectrum", "list_spectra", "find_peaks", "process"})
        QVERIFY2(names.contains(QLatin1String(n)), n);

    r = rpc("tools/call", {{"name", "status"}, {"arguments", QJsonObject{}}});
    QCOMPARE(r.value("result").toObject().value("isError").toBool(), false);
    QCOMPARE(toolJson(r).value("connection").toString(), QStringLiteral("connected"));

    r = rpc("tools/call", {{"name", "acquire"}, {"arguments", QJsonObject{{"exposure_s", 0.05}, {"sample", "from mcp"}}}});
    QCOMPARE(r.value("result").toObject().value("isError").toBool(), false);
    const QJsonObject acq = toolJson(r);
    QCOMPARE(acq.value("state").toString(), QStringLiteral("done"));
    QCOMPARE(acq.value("spectrum").toObject().value("sample").toString(), QStringLiteral("from mcp"));
    QVERIFY(log().contains("API (mcp): measure \"from mcp\""));
    const int id = acq.value("spectrum").toObject().value("id").toInt();

    r = rpc("tools/call", {{"name", "get_spectrum"}, {"arguments", QJsonObject{{"id", id}, {"max_points", 100}}}});
    QCOMPARE(toolJson(r).value("shift").toArray().size(), 100);
    QVERIFY(!toolJson(r).contains("signal"));
    r = rpc("tools/call", {{"name", "find_peaks"}, {"arguments", QJsonObject{{"id", id}}}});
    QCOMPARE(r.value("result").toObject().value("isError").toBool(), false);
    QVERIFY(!toolJson(r).value("peaks").toArray().isEmpty());

    setControl(false);                                         // read-only: acquire refused, as an error result
    r = rpc("tools/call", {{"name", "acquire"}, {"arguments", QJsonObject{{"exposure_s", 0.05}}}});
    QCOMPARE(r.value("result").toObject().value("isError").toBool(), true);
    QCOMPARE(toolJson(r).value("status").toInt(), 403);
    r = rpc("tools/call", {{"name", "stop"}, {"arguments", QJsonObject{}}});
    QCOMPARE(r.value("result").toObject().value("isError").toBool(), false);

    r = rpc("tools/call", {{"name", "nope"}, {"arguments", QJsonObject{}}});
    QCOMPARE(r.value("error").toObject().value("code").toInt(), -32602);
    r = rpc("resources/list", {});
    QCOMPARE(r.value("error").toObject().value("code").toInt(), -32601);
    r = rpc("ping", {});
    QVERIFY(r.contains("result"));

    mcp.closeWriteChannel();                                   // EOF: the server ends
    QTRY_COMPARE_WITH_TIMEOUT(mcp.state(), QProcess::NotRunning, 5000);
    QCOMPARE(mcp.exitCode(), 0);
}

void GuiTestApi::stopRemovesApiFile() {
    QVERIFY(QFileInfo::exists(m_apiFile));
    m_win->stopApi();
    QVERIFY(!m_win->apiServer()->isRunning());
    QVERIFY(!QFileInfo::exists(m_apiFile));
    QCOMPARE(m_win->statusStrip()->remoteTile()->mainText(), QStringLiteral("○ OFF"));
    QVERIFY(!m_win->apiAction()->isChecked());
}

QTEST_MAIN(GuiTestApi)
#include "gui_test_api.moc"
