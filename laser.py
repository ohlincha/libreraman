#!/usr/bin/env python3
"""
Step 3: first laser test (532 nm), copying the vendor software's sequence from the captures.

  1. MCP2200 pulse (outputs cleared, then set ~100 ms later), as the software does at "arm"; wait 5 s.
  2. IO L 09 (laser on), wait 3.6 s, one exposure, then IO H 09 (laser off) - ALWAYS, also on errors,
     Ctrl+C or SIGTERM/SIGHUP (e.g. a dropped ssh connection).
  3. One background exposure with the laser off, then the closing MCP2200 pulse.
The laser is on for (3.6 s + exposure + readout) only. The power is whatever the instrument is set to.

Requires the explicit flag --operator-present (someone at the instrument, lid closed, sample in).
Usage: python3 laser.py --operator-present [exposure_s]   -> laser_<time>.csv
"""
import os
import select
import signal
import struct
import sys
import time

import dark   # I/O board, spectrometer helpers and calibration from step 2

MCP_VID_PID = ("04d8", "00df")


def mcp_hidraw():
    import glob
    for dev in sorted(glob.glob("/dev/hidraw*")):
        p = os.path.realpath(f"/sys/class/hidraw/{os.path.basename(dev)}/device")
        while p not in ("/", ""):
            if dark.ids(p) == MCP_VID_PID:
                return dev
            if os.path.exists(os.path.join(p, "idVendor")):
                break
            p = os.path.dirname(p)
    return None


class MCP:
    def __init__(self, dev):
        self.fd = os.open(dev, os.O_RDWR)

    def _cmd(self, data):
        os.write(self.fd, bytes([0]) + bytes(data).ljust(16, b"\0"))
        r, _, _ = select.select([self.fd], [], [], 2.0)
        return os.read(self.fd, 64) if r else b""

    def gpio(self):
        rep = self._cmd([0x80])
        return rep[10] if len(rep) > 10 and rep[0] == 0x80 else None

    def pulse(self):
        clr = [0x08] + [0] * 10 + [0x00, 0xFF]
        st = [0x08] + [0] * 10 + [0xFF, 0x00]
        os.write(self.fd, bytes([0]) + bytes(clr).ljust(16, b"\0"))
        time.sleep(0.1)
        os.write(self.fd, bytes([0]) + bytes(st).ljust(16, b"\0"))


def expose(dev, seconds):
    dark.ctrl_out(dev, 0xAA, 0xFFFF, 0xFFFF)
    try:
        data = bytes(dev.read(0x82, 4608, timeout=int(seconds * 1000) + 5000))
    finally:
        dark.ctrl_out(dev, 0xAA, 0, 0)
    return struct.unpack(f">{len(data) // 2}H", data)[:dark.ACTIVE_PIXELS]


def main():
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    if "--operator-present" not in sys.argv:
        sys.exit("Refusing: run with --operator-present (someone at the instrument, lid closed).")
    exposure = float(args[0]) if args else 1.0
    if not 0.05 <= exposure <= 10:
        sys.exit("exposure must be 0.05–10 s for this test")

    def on_signal(signum, frame):
        raise KeyboardInterrupt(f"signal {signum}")
    for s in (signal.SIGTERM, signal.SIGHUP):
        signal.signal(s, on_signal)

    port = dark.io_port()
    if not port:
        sys.exit("I/O board not found")
    io = dark.IOBoard(port)
    io.cmd("VOL 03")
    devs = dark.find_spectrometer(timeout=1)
    if not devs:
        print("Spectrometer not powered; running the start-up sequence")
        for line in dark.INIT_LINES:
            io.cmd(line)
            time.sleep(0.5)
        devs = dark.find_spectrometer()
    if not devs:
        sys.exit("spectrometer not found")
    dev = devs[0]
    try:
        dev.get_active_configuration()      # already configured: don't reset it (that drops the first packet)
    except Exception:  # noqa: BLE001
        dev.set_configuration()
    dark.ctrl_out(dev, 0xA3)
    dark.ctrl_out(dev, 0xA4, 0, 0x03F0)
    raw = max(0, round(exposure / 16e-6) - 1)
    dark.ctrl_out(dev, 0xAE, 8, 0x30, struct.pack("<II", raw, max(1, round(20 / exposure))))

    hid = mcp_hidraw()
    mcp = MCP(hid) if hid else None
    if mcp:
        print(f"MCP2200 GPIO before: {mcp.gpio():08b}")
        mcp.pulse()
        print(">>> Press the ARM button on the instrument now (waiting up to 60 s)", flush=True)
        t_end = time.time() + 60
        while time.time() < t_end and (mcp.gpio() or 1) & 1:
            time.sleep(0.2)
        g = mcp.gpio()
        print(f"MCP2200 GPIO: {g:08b}")
        if g & 1:
            sys.exit("Not armed (GPIO bit 0 still 1); laser NOT switched on.")
        print("Armed.")
    else:
        sys.exit("MCP2200 not found; laser NOT switched on.")

    laser_frame = None
    t_on = None
    try:
        # Bypasses dark.IOBoard's line-09 guard on purpose: this is the one place the laser is switched.
        print(">>> LASER ON (IO L 09)")
        t_on = time.time()
        reply = _raw_io(io, "IO L 09")
        print("    ", reply)
        time.sleep(3.6)
        laser_frame = expose(dev, exposure)
    finally:
        reply = _raw_io(io, "IO H 09")
        dt = time.time() - t_on if t_on else 0
        print(f">>> LASER OFF (IO H 09) -> {reply}  (laser was on {dt:.1f} s)")
    background = expose(dev, exposure)
    if mcp:
        mcp.pulse()
        print(f"MCP2200 GPIO at end: {mcp.gpio():08b}")

    fn = time.strftime("laser_%Y%m%d_%H%M%S.csv")
    with open(fn, "w") as f:
        f.write(f"# Cora 5000, 532 nm, exposure {exposure} s; laser frame, background, difference\n")
        f.write("pixel,wavelength_nm,raman_shift_cm1,laser,background,difference\n")
        for i, (a, b) in enumerate(zip(laser_frame, background)):
            wl = sum(k * (dark.PIXEL0 - i) ** n for n, k in enumerate(dark.WL_COEFF))
            f.write(f"{i},{wl:.3f},{1e7 / dark.LASER_NM - 1e7 / wl:.2f},{a},{b},{a - b}\n")
    diff = [a - b for a, b in zip(laser_frame, background)]
    print(f"laser mean {sum(laser_frame) / len(laser_frame):.0f}, background mean "
          f"{sum(background) / len(background):.0f}, max difference {max(diff)}")
    print("Saved", fn)


def _raw_io(io, text):
    for ch in (text + "\n").encode():
        io.s.write(bytes([ch]))
        io.s.flush()
        time.sleep(0.08)
    buf, t_end = b"", time.time() + 3
    while time.time() < t_end and not buf.endswith(b"OK\n"):
        buf += io.s.read(64)
    return buf.decode("ascii", "replace").strip()


if __name__ == "__main__":
    main()
