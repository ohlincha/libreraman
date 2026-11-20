"""Windows: (re)install the WinUSB drivers for the instrument, via the installer's install-drivers.ps1.

The script (installed in <app>\\drivers next to libwdi's wdi-simple.exe) needs administrator rights, so it
is started with the "runas" verb (one UAC prompt). See installer/windows/install-drivers.ps1 for what it
changes and why the PL2303 I/O board is only bound when it is the instrument's (refused by Prolific)."""

import os
import sys

HINT = ("Use Instrument > Repair USB drivers in LibreRamen (or Start menu > LibreRamen > Repair USB drivers) "
        "with the instrument connected and switched on.")


def script_path():
    """install-drivers.ps1 of the installed app, or None (e.g. running from source or the zip)."""
    base = os.path.dirname(os.path.abspath(sys.executable))
    for d in (base, os.path.dirname(base)):          # GUI in <app>, console build in <app>\cli
        p = os.path.join(d, "drivers", "install-drivers.ps1")
        if os.path.exists(p) and os.path.exists(os.path.join(d, "drivers", "wdi-simple.exe")):
            return p
    return None


class _Elevated:
    """A process started with ShellExecuteEx("runas"); poll() -> None while running, else the exit code."""

    def __init__(self, handle):
        self.handle = handle

    def poll(self):
        import ctypes
        from ctypes import wintypes
        k32 = ctypes.WinDLL("kernel32")
        code = wintypes.DWORD()
        k32.GetExitCodeProcess(wintypes.HANDLE(self.handle), ctypes.byref(code))
        if code.value == 259:                         # STILL_ACTIVE
            return None
        k32.CloseHandle(wintypes.HANDLE(self.handle))
        return code.value


def repair(quiet=False):
    """Start the driver script elevated. Returns an _Elevated handle; raises RuntimeError if impossible
    (script not installed, or the user declined the UAC prompt)."""
    if sys.platform != "win32":
        raise RuntimeError("USB drivers only need installing on Windows.")
    script = script_path()
    if not script:
        raise RuntimeError("The driver installer was not found. Install LibreRamen with LibreRamen-Setup.exe "
                           "(the zip version has no driver installer).")
    import ctypes
    from ctypes import wintypes

    class SHELLEXECUTEINFOW(ctypes.Structure):
        _fields_ = [("cbSize", wintypes.DWORD), ("fMask", wintypes.ULONG), ("hwnd", wintypes.HWND),
                    ("lpVerb", wintypes.LPCWSTR), ("lpFile", wintypes.LPCWSTR), ("lpParameters", wintypes.LPCWSTR),
                    ("lpDirectory", wintypes.LPCWSTR), ("nShow", ctypes.c_int), ("hInstApp", wintypes.HINSTANCE),
                    ("lpIDList", ctypes.c_void_p), ("lpClass", wintypes.LPCWSTR), ("hkeyClass", wintypes.HKEY),
                    ("dwHotKey", wintypes.DWORD), ("hIconOrMonitor", wintypes.HANDLE), ("hProcess", wintypes.HANDLE)]
    params = f'-NoProfile -ExecutionPolicy Bypass -WindowStyle Hidden -File "{script}"' + (" -Quiet" if quiet else "")
    info = SHELLEXECUTEINFOW()
    info.cbSize = ctypes.sizeof(info)
    info.fMask = 0x40                                 # SEE_MASK_NOCLOSEPROCESS
    info.lpVerb, info.lpFile, info.lpParameters, info.nShow = "runas", "powershell.exe", params, 0
    shell32 = ctypes.WinDLL("shell32", use_last_error=True)
    shell32.ShellExecuteExW.argtypes = [ctypes.POINTER(SHELLEXECUTEINFOW)]
    if not shell32.ShellExecuteExW(ctypes.byref(info)) or not info.hProcess:
        err = ctypes.get_last_error()
        raise RuntimeError("Driver installation was cancelled." if err == 1223 else
                           f"Could not start the driver installer (error {err}).")
    return _Elevated(info.hProcess)
