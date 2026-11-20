#include "apiclient.h"

#include "errors.h"

#include <QDeadlineTimer>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QRandomGenerator>
#include <QStandardPaths>
#include <QTcpSocket>
#include <QUrl>
#include <QCoreApplication>

#include <algorithm>

namespace lr::api {

QString defaultApiFile() {
    const QString env = qEnvironmentVariable("LIBRERAMEN_API_FILE");
    if (!env.isEmpty())
        return env;
    return QDir(QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation))
        .filePath(QStringLiteral("LibreRamen/api.json"));
}

Endpoint findServer(const QString& apiFile) {
    Endpoint ep;
    const QString envUrl = qEnvironmentVariable("LIBRERAMEN_API_URL");
    const QString envToken = qEnvironmentVariable("LIBRERAMEN_API_TOKEN");
    if (apiFile.isEmpty() && !envUrl.isEmpty() && !envToken.isEmpty()) {
        const QUrl u(envUrl);
        ep.host = u.host().isEmpty() ? QStringLiteral("127.0.0.1") : u.host();
        ep.port = quint16(u.port(kDefaultPort));
        ep.token = envToken.toUtf8();
        ep.source = QStringLiteral("LIBRERAMEN_API_URL");
        return ep;
    }
    const QString path = apiFile.isEmpty() ? defaultApiFile() : apiFile;
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        throw Error(("LibreRamen is not running, or its API is switched off (no API file " +
                     QDir::toNativeSeparators(path) + "). Start the LibreRamen application.")
                        .toStdString());
    const QJsonObject o = QJsonDocument::fromJson(f.readAll()).object();
    if (o.value(QStringLiteral("token")).toString().isEmpty() || o.value(QStringLiteral("port")).toInt() <= 0)
        throw Error(("Invalid API file " + QDir::toNativeSeparators(path)).toStdString());
    ep.port = quint16(o.value(QStringLiteral("port")).toInt());
    ep.token = o.value(QStringLiteral("token")).toString().toUtf8();
    ep.source = path;
    return ep;
}

QByteArray newToken() {
    QByteArray raw(32, Qt::Uninitialized);
    QRandomGenerator* g = QRandomGenerator::system();
    for (char& c : raw)
        c = char(g->bounded(256));
    return raw.toHex();
}

bool writeApiFile(const QString& path, quint16 port, const QByteArray& token, QString* error) {
    auto fail = [&](const QString& m) {
        if (error)
            *error = m;
        return false;
    };
    if (!QDir().mkpath(QFileInfo(path).absolutePath()))
        return fail(QStringLiteral("cannot create ") + QFileInfo(path).absolutePath());
    QFile::remove(path);                          // re-created below with user-only permissions
    QFile f(path);
    const auto perms = QFileDevice::ReadOwner | QFileDevice::WriteOwner;
#if QT_VERSION >= QT_VERSION_CHECK(6, 3, 0)
    if (!f.open(QIODevice::WriteOnly | QIODevice::NewOnly, perms))
        return fail(f.errorString());
#else
    if (!f.open(QIODevice::WriteOnly | QIODevice::NewOnly))
        return fail(f.errorString());
    f.setPermissions(perms);
#endif
    const QJsonObject o{{QStringLiteral("url"), QStringLiteral("http://127.0.0.1:%1").arg(port)},
                        {QStringLiteral("port"), int(port)},
                        {QStringLiteral("token"), QString::fromLatin1(token)},
                        {QStringLiteral("pid"), QCoreApplication::applicationPid()},
                        {QStringLiteral("api_version"), kApiVersion},
                        {QStringLiteral("version"), QStringLiteral(LIBRERAMEN_VERSION)}};
    if (f.write(QJsonDocument(o).toJson()) < 0)
        return fail(f.errorString());
    return true;
}

Reply request(const Endpoint& ep, const QByteArray& method, const QString& pathAndQuery, const QJsonValue& body,
              int timeoutMs, const QString& client) {
    Reply r;
    QDeadlineTimer deadline(timeoutMs);
    QTcpSocket s;
    s.connectToHost(ep.host, ep.port);
    if (!s.waitForConnected(int(std::min<qint64>(deadline.remainingTime(), 5000)))) {
        r.error = QStringLiteral("cannot connect to LibreRamen at %1:%2 (%3)").arg(ep.host).arg(ep.port).arg(s.errorString());
        return r;
    }
    QByteArray payload;
    if (body.isObject())
        payload = QJsonDocument(body.toObject()).toJson(QJsonDocument::Compact);
    else if (body.isArray())
        payload = QJsonDocument(body.toArray()).toJson(QJsonDocument::Compact);
    QByteArray req = method + ' ' + QUrl(pathAndQuery).toEncoded() + " HTTP/1.1\r\n";
    req += "Host: " + ep.host.toUtf8() + ':' + QByteArray::number(ep.port) + "\r\n";
    req += "Authorization: Bearer " + ep.token + "\r\n";
    req += "X-LibreRamen-Client: " + client.toUtf8() + "\r\n";
    req += "Accept: application/json\r\nConnection: close\r\n";
    if (!payload.isEmpty() || method == "POST")
        req += "Content-Type: application/json\r\nContent-Length: " + QByteArray::number(payload.size()) + "\r\n";
    req += "\r\n" + payload;
    s.write(req);
    QByteArray in;
    while (s.state() == QAbstractSocket::ConnectedState) {
        if (deadline.hasExpired()) {
            r.error = QStringLiteral("no answer from LibreRamen within %1 s").arg(timeoutMs / 1000.0);
            return r;
        }
        if (s.bytesToWrite() > 0)
            s.waitForBytesWritten(int(std::min<qint64>(deadline.remainingTime(), 200)));
        if (s.waitForReadyRead(int(std::min<qint64>(deadline.remainingTime(), 200))))
            in += s.readAll();
    }
    in += s.readAll();
    const qsizetype end = in.indexOf("\r\n\r\n");
    if (end < 0) {
        r.error = QStringLiteral("incomplete answer from LibreRamen");
        return r;
    }
    const QList<QByteArray> first = in.left(in.indexOf("\r\n")).split(' ');
    r.status = first.value(1).toInt();
    r.body = in.mid(end + 4);
    QJsonParseError pe{};
    const QJsonDocument d = QJsonDocument::fromJson(r.body, &pe);
    if (pe.error == QJsonParseError::NoError)
        r.json = d.isObject() ? QJsonValue(d.object()) : QJsonValue(d.array());
    if (!r.ok())
        r.error = r.json.toObject().value(QStringLiteral("error")).toString(
            QStringLiteral("HTTP %1").arg(r.status));
    return r;
}

}  // namespace lr::api
