"""
Command line:
  python3 -m libreramen gui [--sim]                      (graphical program)
  python3 -m libreramen status
  python3 -m libreramen init
  python3 -m libreramen dark    [-e 1]                         (laser off)
  python3 -m libreramen acquire [-e 1] [-n 1] [--channel 532|1064] [--power 50] [-o file.csv]
  python3 -m libreramen series  [-e 1] [-n 1] --interval 10 --count 12 [-o file.csv]
  python3 -m libreramen calibrate --standard chloroform [--standard ethanol ...] [-e 1 -n 5]
        (measures each standard in turn, asking you to swap samples and press ARM)
  python3 -m libreramen calibrate --standard chloroform --from chcl3.csv ...   (offline, from saved spectra)
  python3 -m libreramen standards                              (list the reference standards)
  python3 -m libreramen shutdown                               (vendor Shutdown: USB, OFF)
Options: --ini BS_Device_Settings.ini (calibration; default: built-in values of the development unit)
"""

import argparse
import signal
import sys
import time

from . import calibration
from .instrument import Cora, CoraError
from .series import run_series


def main(argv=None):
    ap = argparse.ArgumentParser(prog="libreramen")
    ap.add_argument("cmd", choices=["gui", "selftest", "watchdog", "status", "init", "dark", "acquire", "series", "calibrate", "standards", "shutdown"])
    ap.add_argument("-e", "--exposure", type=float, default=1.0)
    ap.add_argument("-n", "--averages", type=int, default=1)
    ap.add_argument("--channel", type=int, choices=[532, 1064], default=532)
    ap.add_argument("--power", type=float, help="mW (1064 nm only)")
    ap.add_argument("--interval", type=float, default=10)
    ap.add_argument("--count", type=int, default=10)
    ap.add_argument("--background", choices=["each", "once"], default="each")
    ap.add_argument("--ini")
    ap.add_argument("-o", "--output")
    ap.add_argument("--standard", action="append", default=[])
    ap.add_argument("--from", dest="from_files", action="append", default=[],
                    help="saved spectrum CSV (one per --standard, same order)")
    ap.add_argument("--degree", type=int)
    ap.add_argument("--sim", action="store_true", help="use the simulator (gui)")
    ap.add_argument("--screenshot", help="gui: save the window as an image after 15 s")
    ap.add_argument("--port", help="watchdog: I/O board serial port")
    ap.add_argument("--parent", type=int, help="watchdog: pid of the main program")
    a = ap.parse_args(argv)
    if a.cmd == "selftest":
        from .selftest import main as selftest
        return selftest()
    if a.cmd == "watchdog":
        from .watchdog import run
        return run(a.port, parent_pid=a.parent)
    if a.cmd == "gui":
        from .gui.app import main as gui_main
        extra = ["--screenshot", a.screenshot] if a.screenshot else []
        return gui_main((["--sim"] if a.sim else []) + extra)

    def on_signal(signum, frame):
        raise KeyboardInterrupt
    for s in (signal.SIGTERM, signal.SIGHUP):
        signal.signal(s, on_signal)

    cal = calibration.load_ini(a.ini) if a.ini else None
    if a.cmd == "standards":
        from . import standards
        for k, v in sorted(standards.STANDARDS.items()):
            print(f"{k:28} {v['kind']:6} {v['source']:4}  " + ", ".join(f"{b[0]:g}" for b in v["bands"]))
        return 0
    if a.cmd == "calibrate" and a.from_files:
        return offline_calibration(a, cal or calibration.DEFAULT)
    c = Cora(cal)
    try:
        c.connect()
        st = c.status()
        print(f"armed: {st['armed']}, compartment open: {st['compartment_open']}, spectrometers: {st['spectrometers']}")
        if a.cmd == "status":
            return 0
        if a.cmd == "shutdown":
            c.close(power_down=True)
            print("Shutdown sent")
            return 0
        if len(c.spec) < 2 or a.cmd == "init":
            c.initialize()
        else:
            c.channel = 532           # instrument already initialized: assume the 532 nm path
            c.spec[532].init()
        if a.channel != c.channel:
            c.select_channel(a.channel)
        stamp = time.strftime("%Y%m%d_%H%M%S")
        if a.cmd == "dark":
            d = c.dark(a.exposure, a.averages)
            print(f"dark: {len(d)} px, mean {sum(d) / len(d):.0f}, min {min(d):.0f}, max {max(d):.0f}")
        elif a.cmd == "acquire":
            sp = c.acquire(a.exposure, a.averages, power_mw=a.power)
            out = a.output or f"cora_{a.channel}_{stamp}.csv"
            sp.save_csv(out)
            i = max(range(len(sp.intensity)), key=lambda k: sp.intensity[k])
            print(f"saved {out}: {len(sp.shift)} points, max {sp.intensity[i]:.0f} at {sp.shift[i]:.0f} cm-1")
        elif a.cmd == "series":
            out = a.output or f"cora_series_{a.channel}_{stamp}.csv"
            run_series(c, out, a.exposure, a.interval, a.count, a.averages, a.background, a.power,
                       on_spectrum=lambda k, e, sp: print(f"  {k + 1}/{a.count} at {e:6.1f} s: "
                                                          f"mean {sum(sp.intensity) / len(sp.intensity):.0f}"))
            print("saved", out)
        elif a.cmd == "calibrate":
            live_calibration(c, a)
        if a.channel == 1064:
            c.select_channel(532)
        return 0
    except CoraError as e:
        print("error:", e)
        return 1
    finally:
        c.close()


def _pixel_order(sp):
    """Spectrum intensities in calibration-pixel order (what calibrate.py works with)."""
    return [y for _, y in sorted(zip(sp.pixels, sp.intensity))]


def live_calibration(c, a):
    from . import calibrate as calib
    if not a.standard:
        raise CoraError("give at least one --standard")
    samples = []
    for name in a.standard:
        input(f"Put in {name}, close the compartment, press ARM, then press Enter here… ")
        while not c.status()["armed"]:
            time.sleep(0.5)
        sp = c.acquire(a.exposure, max(a.averages, 3))
        samples.append((_pixel_order(sp), name))
    before = c.cal.channels[c.channel].shift
    r = calib.calibrate(samples, before, a.degree)
    print(calib.report(r, before))
    if input("Save this calibration? [y/N] ").strip().lower() == "y":
        print("saved", calib.save(r, c.channel, c.cal.serial))


def offline_calibration(a, cal):
    import csv
    from . import calibrate as calib
    if len(a.from_files) != len(a.standard):
        print("give one --from file per --standard")
        return 1
    ch = cal.channels[a.channel]
    samples = []
    for name, path in zip(a.standard, a.from_files):
        rows = [r for r in csv.reader(line for line in open(path) if not line.startswith("#"))][1:]
        samples.append(([float(r[1]) for r in rows], name))   # files from this program: one row per pixel
    r = calib.calibrate(samples, ch.shift, a.degree)
    print(calib.report(r, ch.shift))
    if a.output:
        print("saved", calib.save(r, a.channel, cal.serial, a.output))
    return 0


if __name__ == "__main__":
    sys.exit(main())
