#include "windrivers.h"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>

#if defined(Q_OS_WIN)
#include <windows.h>
#include <shellapi.h>
#endif

#include "errors.h"

namespace lr::windrivers {

const QString HINT = QStringLiteral(
    "Use Instrument > Repair USB drivers in LibreRamen (or Start menu > LibreRamen > Repair USB drivers) "
    "with the instrument connected and switched on.");

QString scriptPath() {
    const QString exe = QCoreApplication::instance() ? QCoreApplication::applicationFilePath() : QString();
    if (exe.isEmpty()) return {};
    const QString base = QFileInfo(exe).absolutePath();
    for (const QString& d : {base, QFileInfo(base).absolutePath()}) {   // GUI in <app>, console build in <app>\cli
        const QString p = QDir(d).filePath("drivers/install-drivers.ps1");
        if (QFileInfo::exists(p) && QFileInfo::exists(QDir(d).filePath("drivers/wdi-simple.exe")))
            return QDir::toNativeSeparators(p);
    }
    return {};
}

#if defined(Q_OS_WIN)
namespace {
class WinElevated : public ElevatedProcess {
public:
    explicit WinElevated(HANDLE h) : h_(h) {}
    ~WinElevated() override {
        if (h_) CloseHandle(h_);
    }
    std::optional<int> poll() override {
        if (!h_) return code_;
        DWORD code = 0;
        GetExitCodeProcess(h_, &code);
        if (code == STILL_ACTIVE) return std::nullopt;   // 259
        CloseHandle(h_);
        h_ = nullptr;
        code_ = int(code);
        return code_;
    }

private:
    HANDLE h_;
    std::optional<int> code_;
};
}  // namespace
#endif

std::unique_ptr<ElevatedProcess> repair(bool quiet) {
#if defined(Q_OS_WIN)
    const QString script = scriptPath();
    if (script.isEmpty())
        throw Error("The driver installer was not found. Install LibreRamen with LibreRamen-Setup.exe "
                    "(the zip version has no driver installer).");
    const QString params = QString("-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File \"%1\"").arg(script) +
                           (quiet ? QStringLiteral(" -Quiet") : QString());
    const std::wstring verb = L"runas", file = L"powershell.exe", par = params.toStdWString();
    SHELLEXECUTEINFOW info{};
    info.cbSize = sizeof(info);
    info.fMask = SEE_MASK_NOCLOSEPROCESS;     // 0x40
    info.lpVerb = verb.c_str();
    info.lpFile = file.c_str();
    info.lpParameters = par.c_str();
    info.nShow = 0;                           // SW_HIDE
    if (!ShellExecuteExW(&info) || !info.hProcess) {
        const DWORD err = GetLastError();
        throw Error(err == ERROR_CANCELLED ? std::string("Driver installation was cancelled.")   // 1223
                                           : "Could not start the driver installer (error " + std::to_string(err) + ").");
    }
    return std::make_unique<WinElevated>(info.hProcess);
#else
    Q_UNUSED(quiet);
    throw Error("USB drivers only need installing on Windows.");
#endif
}

}  // namespace lr::windrivers
