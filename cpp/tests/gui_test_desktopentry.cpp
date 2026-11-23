// Linux desktop icon (desktopentry.h): the launcher's text, and creating it in a scratch home folder.
#include <QApplication>
#include <QFile>
#include <QImage>
#include <QStandardPaths>
#include <QTemporaryDir>
#include <QtTest>

#include "gui/desktopentry.h"

namespace desktop = lr::gui::desktop;

class GuiTestDesktopEntry : public QObject {
    Q_OBJECT
    QTemporaryDir m_home;
private slots:
    void initTestCase() {
        if (!desktop::supported()) QSKIP("desktop launchers are for Linux");
        QVERIFY(m_home.isValid());
        qputenv("HOME", m_home.path().toUtf8());       // never the real desktop
        qunsetenv("XDG_CONFIG_HOME");
        qunsetenv("XDG_DATA_HOME");
        qunsetenv("APPIMAGE");
    }
    void entryText() {
        const QString t = desktop::entryText(QStringLiteral("/usr/bin/LibreRamen"), QStringLiteral("/x/icon.png"));
        QVERIFY(t.startsWith(QStringLiteral("[Desktop Entry]\n")));
        QVERIFY(t.contains(QStringLiteral("\nExec=/usr/bin/LibreRamen\n")));
        QVERIFY(t.contains(QStringLiteral("\nIcon=/x/icon.png\n")));
        QVERIFY(t.contains(QStringLiteral("\nStartupWMClass=LibreRamen\n")));
        // a path with a space is quoted (AppImage in "My Programs")
        const QString q = desktop::entryText(QStringLiteral("/home/a/My Programs/LibreRamen.AppImage"), {});
        QVERIFY(q.contains(QStringLiteral("\nExec=\"/home/a/My Programs/LibreRamen.AppImage\"\n")));
    }
    void createLauncher() {
        QDir(m_home.path()).mkpath(QStringLiteral("Desktop"));
        QVERIFY(desktop::launcherPath().startsWith(m_home.path()));
        QVERIFY(!desktop::launcherExists());
        QCOMPARE(desktop::createLauncher(), QString());
        QVERIFY(desktop::launcherExists());
        QFile f(desktop::launcherPath());
        QVERIFY(f.open(QIODevice::ReadOnly));
        const QString t = QString::fromUtf8(f.readAll());
        QVERIFY(t.contains(QStringLiteral("Exec=") + QCoreApplication::applicationFilePath()));
        QVERIFY(f.permissions() & QFileDevice::ExeOwner);
        const QString icon = t.section(QStringLiteral("\nIcon="), 1).section(u'\n', 0, 0);
        QCOMPARE(QImage(icon).width(), 256);
    }
};

QTEST_MAIN(GuiTestDesktopEntry)
#include "gui_test_desktopentry.moc"
