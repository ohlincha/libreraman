"""Entry point for the packaged app: no arguments = GUI, otherwise the command line (e.g. 'status')."""
import sys

from libreramen.__main__ import main

if __name__ == "__main__":
    args = sys.argv[1:]
    if args and sys.platform == "win32":          # windowed exe used from a console: attach so output shows
        import ctypes
        if ctypes.windll.kernel32.AttachConsole(-1):
            sys.stdout = open("CONOUT$", "w")
            sys.stderr = open("CONOUT$", "w")
    args = [a for a in args if not a.startswith("-psn_")]      # macOS Finder adds a process serial number
    sys.exit(main(args if args else ["gui"]))
