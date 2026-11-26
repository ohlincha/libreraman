#include "core/legacyname.h"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QSettings>
#include <QStandardPaths>

namespace lr::legacy {

namespace {

const char* const kOld = "LibreRamen";

// Copies the folder `from` to `to` (which must not exist yet). Files only, no links.
void copyTree(const QString& from, const QString& to) {
    QDir().mkpath(to);
    QDirIterator it(from, QDir::Files | QDir::Hidden | QDir::NoDotAndDotDot, QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString src = it.next();
        const QString dst = QDir(to).filePath(QDir(from).relativeFilePath(src));
        QDir().mkpath(QFileInfo(dst).path());
        QFile::copy(src, dst);
    }
}

}  // namespace

void env() {
    for (const char* name : {"API_FILE", "API_URL", "API_TOKEN"}) {
        const QByteArray now = QByteArray("LIBRERAMAN_") + name;
        const QByteArray old = QByteArray("LIBRERAMEN_") + name;
        if (qEnvironmentVariableIsEmpty(now.constData()) && !qEnvironmentVariableIsEmpty(old.constData()))
            qputenv(now.constData(), qgetenv(old.constData()));
    }
}

void configDir() {
    const QString xdg = qEnvironmentVariable("XDG_CONFIG_HOME");
    const QString base = xdg.isEmpty() ? QDir::homePath() + "/.config" : xdg;
    const QString now = QDir::cleanPath(base + "/libreraman");
    const QString old = QDir::cleanPath(base + "/libreramen");
    if (!QFileInfo::exists(now) && QFileInfo(old).isDir())
        copyTree(old, now);
}

void appData() {
    const QString app = QCoreApplication::applicationName();
    const QString org = QCoreApplication::organizationName();
    QSettings now;
    if (now.allKeys().isEmpty()) {
        QSettings old(QString::fromLatin1(kOld), QString::fromLatin1(kOld));
        for (const QString& k : old.allKeys())
            now.setValue(k, old.value(k));
        now.sync();
    }
    const QString dataNow = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QCoreApplication::setApplicationName(QString::fromLatin1(kOld));
    QCoreApplication::setOrganizationName(QString::fromLatin1(kOld));
    const QString dataOld = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    QCoreApplication::setApplicationName(app);
    QCoreApplication::setOrganizationName(org);
    if (dataOld != dataNow && !QFileInfo::exists(dataNow) && QFileInfo(dataOld).isDir())
        copyTree(dataOld, dataNow);
}

}  // namespace lr::legacy
