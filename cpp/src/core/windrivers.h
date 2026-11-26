#pragma once
// Windows: (re)install the WinUSB drivers for the instrument via the installer's install-drivers.ps1
// (Python: windrivers.py; installer/windows/install-drivers.ps1 explains what it changes and why the PL2303
// I/O board is only bound when it is the instrument's). The script needs administrator rights, so it is
// started with ShellExecuteEx "runas" (one UAC prompt). Compiles everywhere; repair() throws elsewhere.
//
// Thread safety: ElevatedProcess is not shared between threads.
#include <QString>
#include <memory>
#include <optional>

namespace lr::windrivers {

extern const QString HINT;    // "Use Instrument > Repair USB drivers in LibreRaman ..."

// <app>/drivers/install-drivers.ps1 (with wdi-simple.exe next to it), checking the executable's directory
// and its parent (console build in <app>\cli); empty if not installed (running from a build or the zip).
QString scriptPath();

class ElevatedProcess {
public:
    virtual ~ElevatedProcess() = default;
    virtual std::optional<int> poll() = 0;   // nullopt while running, else the exit code
};

// Starts the script elevated (powershell -NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File ...
// [-Quiet]). Throws lr::Error: not Windows, script not installed, or the UAC prompt was declined.
std::unique_ptr<ElevatedProcess> repair(bool quiet = false);

}  // namespace lr::windrivers
