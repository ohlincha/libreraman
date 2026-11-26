#include "desktopentry.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMessageBox>
#include <QProcess>
#include <QSettings>
#include <QStandardPaths>

#include "icon.h"
#include "page.h"

namespace lr::gui::desktop {

namespace {
constexpr char kOfferedKey[] = "desktop/iconOffered";
const QString kFileName = QStringLiteral("io.github.ohlincha.libreraman.desktop");

// Desktop Entry spec: an argument with reserved characters goes in double quotes, with " ` $ \ escaped;
// then, as a string value, every backslash is doubled once more.
QString quoteExec(const QString& path) {
    static const QString reserved = QStringLiteral(" \t\n\"'\\><~|&;$*?#()`");
    bool needs = false;
    for (QChar c : path)
        if (reserved.contains(c)) needs = true;
    if (!needs) return path;
    QString q;
    for (QChar c : path) {
        if (c == u'"' || c == u'`' || c == u'$' || c == u'\\') q += u'\\';
        q += c;
    }
    q = u'"' + q + u'"';
    q.replace(u'\\', QStringLiteral("\\\\"));
    return q;
}

QString executable() {
    const QString appImage = qEnvironmentVariable("APPIMAGE");
    return appImage.isEmpty() ? QCoreApplication::applicationFilePath() : appImage;
}

QString writeFile(const QString& path, const QByteArray& data, bool exec) {
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate) || f.write(data) != data.size())
        return QStringLiteral("cannot write %1: %2").arg(QDir::toNativeSeparators(path), f.errorString());
    f.close();
    if (exec)
        f.setPermissions(f.permissions() | QFileDevice::ExeOwner | QFileDevice::ExeUser);
    return {};
}
}  // namespace

bool supported() {
#if defined(Q_OS_LINUX) || defined(Q_OS_FREEBSD)
    return true;
#else
    return false;
#endif
}

QString launcherPath() {
    return QDir(QStandardPaths::writableLocation(QStandardPaths::DesktopLocation)).filePath(kFileName);
}

bool launcherExists() { return QFileInfo::exists(launcherPath()); }

QString entryText(const QString& exec, const QString& iconPath) {
    return QStringLiteral("[Desktop Entry]\n"
                          "Type=Application\n"
                          "Name=LibreRaman\n"
                          "GenericName=Raman Spectrometer\n"
                          "Comment=Measure, calibrate and compare Raman spectra\n"
                          "Exec=%1\n"
                          "Icon=%2\n"
                          "Terminal=false\n"
                          "Categories=Science;Chemistry;Physics;\n"
                          "StartupWMClass=LibreRaman\n")
        .arg(quoteExec(exec), iconPath);
}

QString createLauncher() {
    const QString dataDir = QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
    const QString iconPath = QDir(dataDir).filePath(QStringLiteral("libreraman.png"));
    QDir().mkpath(dataDir);
    if (!drawIcon(256).save(iconPath, "PNG"))
        return QStringLiteral("cannot write %1").arg(QDir::toNativeSeparators(iconPath));
    const QByteArray text = entryText(executable(), iconPath).toUtf8();
    const QString path = launcherPath();
    if (QString err = writeFile(path, text, true); !err.isEmpty())
        return err;
    // GNOME (Desktop Icons NG) runs a launcher on the desktop only when it is marked trusted
    QProcess gio;
    gio.setProgram(QStringLiteral("gio"));
    gio.setArguments({QStringLiteral("set"), path, QStringLiteral("metadata::trusted"), QStringLiteral("true")});
    gio.setStandardOutputFile(QProcess::nullDevice());
    gio.setStandardErrorFile(QProcess::nullDevice());
    gio.startDetached();                                // not installed elsewhere: nothing happens
    if (!qEnvironmentVariableIsEmpty("APPIMAGE")) {     // an AppImage has no entry in the applications menu
        const QString apps = QStandardPaths::writableLocation(QStandardPaths::ApplicationsLocation);
        writeFile(QDir(apps).filePath(kFileName), text, false);
    }
    return {};
}

void offerOnce(QWidget* parent) {
    if (!supported() || !dialogsEnabled()) return;
    QSettings st;
    if (st.value(QLatin1String(kOfferedKey), false).toBool()) return;
    if (!QFileInfo(QStandardPaths::writableLocation(QStandardPaths::DesktopLocation)).isDir() || launcherExists())
        return;
    st.setValue(QLatin1String(kOfferedKey), true);
    const auto answer = QMessageBox::question(parent, QStringLiteral("Desktop icon"),
                                              QStringLiteral("Put a LibreRaman icon on the desktop?\n\n"
                                                             "You can also do this later with File > Put an icon "
                                                             "on the desktop."));
    if (answer != QMessageBox::Yes) return;
    if (const QString err = createLauncher(); !err.isEmpty())
        QMessageBox::warning(parent, QStringLiteral("Desktop icon"), QStringLiteral("No desktop icon: %1").arg(err));
}

void migrateOldLauncher() {
    if (!supported()) return;
    const QString name = QStringLiteral("io.github.ohlincha.libreramen.desktop");
    const QString old = QDir(QStandardPaths::writableLocation(QStandardPaths::DesktopLocation)).filePath(name);
    if (QFileInfo::exists(old) && (launcherExists() || createLauncher().isEmpty()))
        QFile::remove(old);
    QFile::remove(QDir(QStandardPaths::writableLocation(QStandardPaths::ApplicationsLocation)).filePath(name));
}

}  // namespace lr::gui::desktop
