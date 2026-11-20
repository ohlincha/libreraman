"""
Command line:
  python3 -m cora status
  python3 -m cora init
  python3 -m cora dark    [-e 1]                         (laser off)
  python3 -m cora acquire [-e 1] [-n 1] [--channel 532|1064] [--power 50] [-o file.csv]
  python3 -m cora series  [-e 1] [-n 1] --interval 10 --count 12 [-o file.csv]
  python3 -m cora shutdown                               (vendor Shutdown: USB, OFF)
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
    ap = argparse.ArgumentParser(prog="cora")
    ap.add_argument("cmd", choices=["status", "init", "dark", "acquire", "series", "shutdown"])
    ap.add_argument("-e", "--exposure", type=float, default=1.0)
    ap.add_argument("-n", "--averages", type=int, default=1)
    ap.add_argument("--channel", type=int, choices=[532, 1064], default=532)
    ap.add_argument("--power", type=float, help="mW (1064 nm only)")
    ap.add_argument("--interval", type=float, default=10)
    ap.add_argument("--count", type=int, default=10)
    ap.add_argument("--background", choices=["each", "once"], default="each")
    ap.add_argument("--ini")
    ap.add_argument("-o", "--output")
    a = ap.parse_args(argv)

    def on_signal(signum, frame):
        raise KeyboardInterrupt
    for s in (signal.SIGTERM, signal.SIGHUP):
        signal.signal(s, on_signal)

    cal = calibration.load_ini(a.ini) if a.ini else None
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
        if a.channel == 1064:
            c.select_channel(532)
        return 0
    except CoraError as e:
        print("error:", e)
        return 1
    finally:
        c.close()


if __name__ == "__main__":
    sys.exit(main())
