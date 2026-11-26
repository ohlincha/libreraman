#pragma once
// Client side of the local HTTP/JSON API of the running LibreRaman application (server: gui/apiserver.h).
// Used by `LibreRamanCLI --remote ...` and `LibreRamanMCP`; the Python client libreraman/client.py does the
// same.
//
// Finding the server: the application writes the API file (JSON {"url", "port", "token", "pid", "version"},
// readable by the user only) when its API starts and removes it when it stops:
//   $LIBRERAMAN_API_FILE, else <generic config dir>/LibreRaman/api.json
//   (Linux ~/.config/LibreRaman/api.json, macOS ~/Library/Preferences/LibreRaman/api.json,
//    Windows %LOCALAPPDATA%\LibreRaman\api.json).
// $LIBRERAMAN_API_URL and $LIBRERAMAN_API_TOKEN, if both set, take precedence over the file.
//
// Requests are blocking (QTcpSocket wait functions): call them from a thread without a running event loop
// that the server depends on (never from the GUI thread of the application that hosts the server).
#include <QByteArray>
#include <QJsonValue>
#include <QString>

namespace lr::api {

constexpr quint16 kDefaultPort = 8765;
constexpr int kApiVersion = 1;

QString defaultApiFile();

struct Endpoint {
    QString host = QStringLiteral("127.0.0.1");
    quint16 port = kDefaultPort;
    QByteArray token;
    QString source;                           // where it came from (for messages)
};

// Reads the API file / environment. Throws lr::Error ("LibreRaman is not running ...") if not found.
Endpoint findServer(const QString& apiFile = {});

// Writes the API file (user-only permissions; the directory is created). false and *error on failure.
bool writeApiFile(const QString& path, quint16 port, const QByteArray& token, QString* error = nullptr);
// A new random token (64 hex digits).
QByteArray newToken();

struct Reply {
    int status = 0;                           // HTTP status; 0 = no connection / no answer
    QJsonValue json;                          // parsed body (Undefined if not JSON)
    QByteArray body;
    QString error;                            // transport error, or the "error" field of an error reply

    bool ok() const { return status >= 200 && status < 300; }
};

// One request. `client` identifies the caller in the instrument's log (header X-LibreRaman-Client).
Reply request(const Endpoint& ep, const QByteArray& method, const QString& pathAndQuery,
              const QJsonValue& body = QJsonValue(QJsonValue::Undefined), int timeoutMs = 30000,
              const QString& client = QStringLiteral("cli"));

}  // namespace lr::api
