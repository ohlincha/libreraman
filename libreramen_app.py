"""Entry point for the packaged app: no arguments = GUI, otherwise the command line (e.g. 'status')."""
import sys

from libreramen.__main__ import main

if __name__ == "__main__":
    args = sys.argv[1:]
    if args and sys.platform == "win32" and sys.stdout is None:   # windowed exe used from a console
        import ctypes
        import os
        import tempfile
        if ctypes.windll.kernel32.AttachConsole(-1):
            sys.stdout = open("CONOUT$", "w")
            sys.stderr = open("CONOUT$", "w")
        else:                                     # no console (e.g. remote/redirected): write a log file
            log = open(os.path.join(tempfile.gettempdir(), "libreramen-cli.log"), "w", buffering=1)
            sys.stdout = sys.stderr = log
    if args and args[0] not in ("gui", "watchdog"):
        import faulthandler                       # if a command hangs, show where (stack dump after 45 s)
        faulthandler.dump_traceback_later(45, exit=False, file=sys.stderr)
    args = [a for a in args if not a.startswith("-psn_")]      # macOS Finder adds a process serial number
    sys.exit(main(args if args else ["gui"]))
