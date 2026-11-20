#!/usr/bin/env python3
"""
1064 nm test, copying the vendor sequence from captures/2026-11-20c/1064.pcapng.
Switch to 1064 (driver power, beam path), init detector #2, wait for cooling, then (armed only):
laseron, current 0.39 A (= 50 mW), 1 s frame, current 0, background frame, laseroff.
Always ends with current 0.00 + laseroff and switches back to the 532 nm path.
Usage: python3 test1064.py --operator-present [cool_s] [exposure_s] [power_mW]
"""
import glob
import os
import struct
import sys
import time

import serial

import dark
import laser

CURRENT_50MW = 0.39
PWR_COEFF = (0.275808, 0.002209)   # current (A) = c0 + c1 * P(mW), from the vendor ini (1064 nm)
MAX_TEST_MW = 250
WL1064 = (1067.709823464664, 1.417819808954, 0.000004218663, -0.000000348208)


def driver_port(timeout=15):
    t_end = time.time() + timeout
    while time.time() < t_end:
        for dev in sorted(glob.glob("/dev/ttyUSB*")):
            p = os.path.realpath(f"/sys/class/tty/{os.path.basename(dev)}/device")
            while p not in ("/", ""):
                v = dark.ids(p)
                if v:
                    if v == ("10c4", "ea60"):
                        return dev
                    break
                p = os.path.dirname(p)
        time.sleep(0.3)
    return None


class Driver:
    def __init__(self, dev):
        self.s = serial.Serial(dev, 19200, timeout=0.3)
        self.s.dtr = False
        self.s.rts = False
        time.sleep(0.2)
        self.s.reset_input_buffer()

    def cmd(self, text):
        self.s.write((text + "\r").encode())
        self.s.flush()
        time.sleep(0.12)
        r = self.s.read(200).decode("ascii", "replace").strip()
        print(f"  LD   {text:18} -> {r!r}")
        return r


def main():
    if "--operator-present" not in sys.argv:
        sys.exit("Refusing: run with --operator-present")
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    cool = float(args[0]) if args else 60
    exposure = float(args[1]) if len(args) > 1 else 1.0
    power = float(args[2]) if len(args) > 2 else 50.0
    if not 0 < power <= MAX_TEST_MW or not 0.01 <= exposure <= 30:
        sys.exit(f"power must be 0-{MAX_TEST_MW} mW and exposure 0.01-30 s")
    current = PWR_COEFF[0] + PWR_COEFF[1] * power
    io = dark.IOBoard(dark.io_port())
    io.cmd("VOL 03")
    if not dark.find_spectrometer(1, "BS2BASE"):
        print("Instrument not initialised; running the start-up sequence")
        for line in dark.INIT_LINES:
            io.cmd(line)
            time.sleep(0.5)
    det = dark.find_spectrometer(10, "BS2BASE")
    if not det:
        sys.exit("1064 detector not found")
    dev = det[0]
    try:
        dev.get_active_configuration()
    except Exception:  # noqa: BLE001
        dev.set_configuration()

    print("Switching to 1064 nm")
    for c in ("IO H 0b", "IO L 07", "IO L 0b"):
        io.cmd(c)
        time.sleep(0.5)
    port = driver_port()
    if not port:
        sys.exit("laser driver (CP210x) did not appear")
    ld = Driver(port)
    ld.cmd("")
    ld.cmd("rev?")
    for c in ("tecmax 0.6", "temperature 25.00", "laseroff", "current 0.00"):
        ld.cmd(c)
    for c in ("IO L 0e", "IO H 0f"):
        io.cmd(c)
        time.sleep(0.5)
    dark.ctrl_out(dev, 0xA3)
    dark.ctrl_out(dev, 0xA4, 1, 0x03F0)
    dark.ctrl_out(dev, 0xAD, 0, 0x38)
    dark.ctrl_out(dev, 0xA4, 1, 0x03F0)
    dark.ctrl_out(dev, 0xAE, 8, 0x30, struct.pack("<II", round(exposure / 0.8e-6) - 25, 1))   # 0.8 us units
    print(f"Cooling {cool:.0f} s…", flush=True)
    time.sleep(cool)

    def expose():
        dark.ctrl_out(dev, 0xAA, 0xFFFF, 0xFFFF)
        try:
            d = bytes(dev.read(0x82, 1024, timeout=int(exposure * 1000) + 6000))
        finally:
            dark.ctrl_out(dev, 0xAA, 0, 0)
        return struct.unpack(f">{len(d) // 2}H", d)

    mcp = laser.MCP(laser.mcp_hidraw())
    g = mcp.gpio()
    print(f"GPIO {g:08b}")
    sig = bg = None
    t_on = None
    try:
        if g & 1:
            print("NOT ARMED: laser stays off; taking dark frames only")
        else:
            mcp.pulse()
            print(">>> 1064 LASER ON")
            t_on = time.time()
            ld.cmd("laseron")
            ld.cmd(f"current {current:.2f}")
            print(f"    {power:.0f} mW -> current {current:.2f} A, exposure {exposure} s")
            time.sleep(1.5)
            sig = expose()
            ld.cmd("current 0.00")
        bg = expose()
    finally:
        ld.cmd("current 0.00")
        ld.cmd("laseroff")
        if t_on:
            print(f">>> 1064 LASER OFF (on {time.time() - t_on:.1f} s)")
    print("Back to the 532 nm path")
    ld.s.close()
    for c in ("IO H 07", "IO H 0b", "IO L 0b", "IO L 0f", "IO H 0e"):
        io.cmd(c)
        time.sleep(0.5)
    mcp.pulse()
    sig = sig or [0] * len(bg)
    print(f"frames: {len(bg)} px; signal mean {sum(sig) / len(sig):.0f}, background mean {sum(bg) / len(bg):.0f}")
    fn = time.strftime("t1064_%Y%m%d_%H%M%S.csv")
    with open(fn, "w") as f:
        f.write("index,signal,background,difference\n")
        for i, (a, b) in enumerate(zip(sig, bg)):
            f.write(f"{i},{a},{b},{a - b}\n")
    print("Saved", fn)


if __name__ == "__main__":
    main()
