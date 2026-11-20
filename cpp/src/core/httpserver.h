#pragma once
// A small HTTP/1.1 server for the local JSON API (api.h, gui/apiserver.h).
//
// Why not QtHttpServer: it is not available with the distribution Qt of Ubuntu 24.04 (Qt 6.4) and is an
// extra module elsewhere; the API only needs request/response JSON on 127.0.0.1, so a few hundred lines on
// QTcpServer are simpler and portable. One request per connection ("Connection: close"); bodies with
// Content-Length only (no chunked uploads); "Expect: 100-continue" is answered.
//
// Thread safety: lives in the thread that created it (the GUI thread); the handler is called there. A reply
// may be sent later (e.g. when a measurement has finished) by keeping the Reply callable; if the client has
// gone away in the meantime, the reply is dropped.
#include <QByteArray>
#include <QHostAddress>
#include <QJsonObject>
#include <QJsonValue>
#include <QMap>
#include <QObject>
#include <QString>
#include <QUrlQuery>
#include <functional>

class QTcpServer;
class QTcpSocket;

namespace lr::http {

struct Request {
    QByteArray method;                        // "GET", "POST", ...
    QString path;                             // decoded, without the query ("/spectra/3")
    QUrlQuery query;
    QMap<QByteArray, QByteArray> headers;     // keys in lower case
    QByteArray body;
    QHostAddress peer;

    QByteArray header(const QByteArray& name) const { return headers.value(name.toLower()); }
    // The body as a JSON object; an empty body is {}. false (and *error set) if it is not a JSON object.
    bool jsonBody(QJsonObject* out, QString* error) const;
};

struct Response {
    int status = 200;
    QByteArray body;
    QByteArray contentType = "application/json";
    QMap<QByteArray, QByteArray> headers;     // extra headers

    static Response json(int status, const QJsonValue& v);
    // {"error": message, "status": status}
    static Response error(int status, const QString& message);
};

using Reply = std::function<void(const Response&)>;           // call once; may be called later
using Handler = std::function<void(const Request&, const Reply&)>;

QByteArray reasonPhrase(int status);

class Server : public QObject {
    Q_OBJECT
public:
    explicit Server(QObject* parent = nullptr);
    ~Server() override;

    void setHandler(Handler h) { m_handler = std::move(h); }
    void setMaxBodySize(qint64 bytes) { m_maxBody = bytes; }
    // Listens on address:port (port 0: any free port). false and *error on failure (e.g. port in use).
    bool listen(const QHostAddress& address, quint16 port, QString* error = nullptr);
    void close();
    bool isListening() const;
    quint16 port() const;

private:
    void onConnection();
    void onReadyRead(QTcpSocket* s);

    QTcpServer* m_server;
    Handler m_handler;
    qint64 m_maxBody = 32 * 1024 * 1024;
};

}  // namespace lr::http
