#include "httpserver.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QPointer>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QUrl>
#include <memory>

namespace lr::http {

namespace {

constexpr int kMaxHeader = 64 * 1024;
constexpr int kRequestTimeoutMs = 30000;     // to receive the complete request

struct ConnState {
    QByteArray buf;
    bool dispatched = false;
    bool continued = false;                  // "100 Continue" sent
};

QByteArray serialize(const Response& r) {
    QByteArray out = "HTTP/1.1 " + QByteArray::number(r.status) + ' ' + reasonPhrase(r.status) + "\r\n";
    out += "Content-Type: " + r.contentType + "\r\n";
    out += "Content-Length: " + QByteArray::number(r.body.size()) + "\r\n";
    out += "Connection: close\r\nCache-Control: no-store\r\n";
    for (auto it = r.headers.begin(); it != r.headers.end(); ++it)
        out += it.key() + ": " + it.value() + "\r\n";
    out += "\r\n";
    out += r.body;
    return out;
}

void sendNow(QTcpSocket* s, const Response& r) {
    s->write(serialize(r));
    s->disconnectFromHost();
}

}  // namespace

bool Request::jsonBody(QJsonObject* out, QString* error) const {
    if (body.trimmed().isEmpty()) {
        *out = {};
        return true;
    }
    QJsonParseError pe{};
    const QJsonDocument d = QJsonDocument::fromJson(body, &pe);
    if (pe.error != QJsonParseError::NoError) {
        *error = QStringLiteral("invalid JSON body: ") + pe.errorString();
        return false;
    }
    if (!d.isObject()) {
        *error = QStringLiteral("the JSON body must be an object");
        return false;
    }
    *out = d.object();
    return true;
}

Response Response::json(int status, const QJsonValue& v) {
    Response r;
    r.status = status;
    if (v.isObject())
        r.body = QJsonDocument(v.toObject()).toJson(QJsonDocument::Compact);
    else if (v.isArray())
        r.body = QJsonDocument(v.toArray()).toJson(QJsonDocument::Compact);
    else
        r.body = QJsonDocument(QJsonObject{{QStringLiteral("value"), v}}).toJson(QJsonDocument::Compact);
    r.body += '\n';
    return r;
}

Response Response::error(int status, const QString& message) {
    return json(status, QJsonObject{{QStringLiteral("error"), message}, {QStringLiteral("status"), status}});
}

QByteArray reasonPhrase(int status) {
    switch (status) {
    case 100: return "Continue";
    case 200: return "OK";
    case 201: return "Created";
    case 202: return "Accepted";
    case 204: return "No Content";
    case 400: return "Bad Request";
    case 401: return "Unauthorized";
    case 403: return "Forbidden";
    case 404: return "Not Found";
    case 405: return "Method Not Allowed";
    case 408: return "Request Timeout";
    case 409: return "Conflict";
    case 413: return "Payload Too Large";
    case 422: return "Unprocessable Entity";
    case 431: return "Request Header Fields Too Large";
    case 500: return "Internal Server Error";
    case 501: return "Not Implemented";
    case 503: return "Service Unavailable";
    default: return status < 400 ? "OK" : "Error";
    }
}

Server::Server(QObject* parent) : QObject(parent), m_server(new QTcpServer(this)) {
    connect(m_server, &QTcpServer::newConnection, this, &Server::onConnection);
}

Server::~Server() = default;

bool Server::listen(const QHostAddress& address, quint16 port, QString* error) {
    if (m_server->isListening())
        m_server->close();
    if (m_server->listen(address, port))
        return true;
    if (error)
        *error = m_server->errorString();
    return false;
}

void Server::close() { m_server->close(); }

bool Server::isListening() const { return m_server->isListening(); }

quint16 Server::port() const { return m_server->serverPort(); }

void Server::onConnection() {
    while (QTcpSocket* s = m_server->nextPendingConnection()) {
        auto st = std::make_shared<ConnState>();
        connect(s, &QTcpSocket::disconnected, s, &QObject::deleteLater);
        connect(s, &QTcpSocket::readyRead, this, [this, s, st] {
            if (st->dispatched)
                return;
            st->buf += s->readAll();
            const qsizetype end = st->buf.indexOf("\r\n\r\n");
            if (end < 0) {
                if (st->buf.size() > kMaxHeader) {
                    st->dispatched = true;
                    sendNow(s, Response::error(431, QStringLiteral("request header too large")));
                }
                return;
            }
            const QList<QByteArray> lines = st->buf.left(end).split('\n');
            const QList<QByteArray> first = lines.value(0).trimmed().split(' ');
            if (first.size() != 3 || !first[2].startsWith("HTTP/1.")) {
                st->dispatched = true;
                sendNow(s, Response::error(400, QStringLiteral("malformed request line")));
                return;
            }
            Request req;
            req.method = first[0];
            req.peer = s->peerAddress();
            const QByteArray target = first[1];
            const qsizetype q = target.indexOf('?');
            req.path = QUrl::fromPercentEncoding(q < 0 ? target : target.left(q));
            if (q >= 0)
                req.query = QUrlQuery(QString::fromUtf8(target.mid(q + 1)));
            for (int i = 1; i < lines.size(); ++i) {
                const QByteArray& l = lines[i];
                const qsizetype c = l.indexOf(':');
                if (c > 0)
                    req.headers.insert(l.left(c).trimmed().toLower(), l.mid(c + 1).trimmed());
            }
            if (req.headers.contains("transfer-encoding")) {
                st->dispatched = true;
                sendNow(s, Response::error(501, QStringLiteral("chunked request bodies are not supported")));
                return;
            }
            bool ok = true;
            const qint64 len = req.headers.contains("content-length") ? req.headers.value("content-length").toLongLong(&ok) : 0;
            if (!ok || len < 0) {
                st->dispatched = true;
                sendNow(s, Response::error(400, QStringLiteral("invalid Content-Length")));
                return;
            }
            if (len > m_maxBody) {
                st->dispatched = true;
                sendNow(s, Response::error(413, QStringLiteral("request body too large")));
                return;
            }
            const qint64 have = st->buf.size() - (end + 4);
            if (have < len) {
                if (!st->continued && req.headers.value("expect").toLower() == "100-continue") {
                    st->continued = true;
                    s->write("HTTP/1.1 100 Continue\r\n\r\n");
                }
                return;
            }
            const QByteArray body = st->buf.mid(end + 4);
            req.body = body.left(qsizetype(len));
            st->dispatched = true;
            st->buf.clear();
            QPointer<QTcpSocket> ps(s);
            auto sent = std::make_shared<bool>(false);
            const Reply reply = [ps, sent](const Response& r) {
                if (*sent)
                    return;
                *sent = true;
                if (ps && ps->state() == QAbstractSocket::ConnectedState)
                    sendNow(ps, r);
            };
            if (!m_handler) {
                reply(Response::error(503, QStringLiteral("no handler")));
                return;
            }
            m_handler(req, reply);
        });
        QTimer::singleShot(kRequestTimeoutMs, s, [s, st] {
            if (!st->dispatched)
                s->abort();
        });
    }
}

}  // namespace lr::http
