#pragma once
// Desktop icon on Linux. The Windows installer has its own tick box, and macOS has no desktop launchers.
//
// The first time the window is shown, LibreRamen asks once whether to put an icon on the desktop
// (QSettings "desktop/iconOffered"). File > Put an icon on the desktop does the same thing at any time.
// The launcher is a .desktop file in the desktop folder (XDG_DESKTOP_DIR, usually ~/Desktop). It starts this
// executable, or the AppImage file when running from an AppImage. For an AppImage it is also copied to
// ~/.local/share/applications, because an AppImage has no entry in the applications menu.
// The icon is written to the application data folder.
#include <QString>

class QWidget;

namespace lr::gui::desktop {

bool supported();                       // Linux (and other XDG desktops)
QString launcherPath();                 // <desktop folder>/io.github.ohlincha.libreramen.desktop
bool launcherExists();
// The .desktop file's text; exec is quoted as needed.
QString entryText(const QString& exec, const QString& iconPath);
// Writes the icon and the launcher (executable, and marked trusted on GNOME). Empty on success, otherwise why not.
QString createLauncher();
// Asks once (dialogsEnabled() only) if supported, the desktop folder exists and there is no launcher yet.
void offerOnce(QWidget* parent);

}  // namespace lr::gui::desktop
