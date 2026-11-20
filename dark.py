#!/usr/bin/env python3
"""
Step 2: power up spectrometer #1 (532 nm channel) and take ONE DARK frame. The laser stays off.

What it sends, copied from the vendor software's own initialisation:
  I/O board:  TECE, IO H 01, IO H 00, IO L 0a (inner hub), IO H 0e, IO L 0c (spectrometer #1)
  Spectrometer: read EEPROM (0xA1), init (0xA3, 0xA4), exposure 1 s (0xAE), start (0xAA), read frame.
It refuses to send anything for I/O line 09 (the 532 nm laser), never arms the laser (MCP2200)
and never powers or talks to the laser driver (line 07, CP210x).

Needs: pyserial, pyusb (sudo apt install python3-usb), the 70-cora.rules udev rule,
and membership in 'dialout'.
Usage:  python3 dark.py [exposure_s]        -> writes dark_<time>.csv
"""
import glob
import os
import struct
import sys
import time

import serial
import usb.core
import usb.util

CORA_HUB = ("0424", "2517")
WL_COEFF = (514.006252760493, 0.096682105209, -0.000006593998, 0.000000000058)   # 532 nm channel
LASER_NM = 532.100
ACTIVE_PIXELS = 2068
PIXEL0 = 2057          # data index i is CCD pixel (2057 - i): reversed order, ~10 dark reference pixels first
INIT_LINES = ["TECE", "IO H 01", "IO H 00", "IO L 0a", "IO H 0e", "IO L 0c", "IO L 0d", "IO H 0b", "IO H 08", "IO H 07", "IO L 0b"]


def ids(d):
    try:
        return (open(os.path.join(d, "idVendor")).read().strip(),
                open(os.path.join(d, "idProduct")).read().strip())
    except OSError:
        return None


def io_port():
    for dev in sorted(glob.glob("/dev/ttyUSB*")):
        p = os.path.realpath(f"/sys/class/tty/{os.path.basename(dev)}/device")
        own = None
        while p not in ("/", ""):
            v = ids(p)
            if v and own is None:
                own = v
            elif v == CORA_HUB and own == ("067b", "2303"):
                return dev
            p = os.path.dirname(p)
    return None


class IOBoard:
    def __init__(self, dev):
        self.s = serial.Serial(dev, 115200, timeout=0.2)
        self.s.dtr = False
        self.s.rts = False
        time.sleep(0.2)
        self.s.reset_input_buffer()

    def cmd(self, text):
        if "09" in text.split():
            raise RuntimeError("refusing to touch line 09 (laser) in this script")
        for ch in (text + "\n").encode():
            self.s.write(bytes([ch]))
            self.s.flush()
            time.sleep(0.08)
        buf, t_end = b"", time.time() + 3
        while time.time() < t_end and not buf.endswith(b"OK\n"):
            buf += self.s.read(64)
        reply = buf.decode("ascii", "replace").strip()
        print(f"  I/O  {text:10} -> {reply!r}")
        if not reply.endswith("OK"):
            raise RuntimeError(f"I/O board did not confirm {text!r}")
        return reply


def _product(d):
    try:
        return usb.util.get_string(d, d.iProduct) or ""
    except Exception:  # noqa: BLE001
        return ""


def find_spectrometer(timeout=10, product="BS2ACCD"):
    """The 532 nm CCD is 'BS2ACCD'; the 1064 nm detector is 'BS2BASE'."""
    t_end = time.time() + timeout
    while time.time() < t_end:
        devs = [d for d in usb.core.find(find_all=True, idVendor=0xBA8C, idProduct=0x1688)
                if _product(d) == product]
        if devs:
            return devs
        time.sleep(0.25)
    return []


def ctrl_in(dev, req, index, length):
    return bytes(dev.ctrl_transfer(0xC0, req, 0, index, length, timeout=1000))


def ctrl_out(dev, req, value=0, index=0, data=None):
    dev.ctrl_transfer(0x40, req, value, index, data or b"", timeout=1000)


def main():
    exposure = float(sys.argv[1]) if len(sys.argv) > 1 else 1.0
    port = io_port()
    if not port:
        sys.exit("I/O board not found. Is the Cora on (On pressed) and the vendor software closed?")
    print(f"I/O board: {port}")
    io = IOBoard(port)
    print("I/O board:", io.cmd("VOL 03"))
    for line in INIT_LINES:
        io.cmd(line)
        time.sleep(0.5)

    print("Waiting for spectrometer #1 on USB…")
    devs = find_spectrometer()
    if not devs:
        sys.exit("No spectrometer (ba8c:1688) appeared. Send me the output and `lsusb`.")
    dev = devs[0]
    print(f"  found ba8c:1688 at bus {dev.bus} address {dev.address}")
    try:
        dev.get_active_configuration()      # don't re-configure a configured device (drops the first packet)
    except usb.core.USBError:
        dev.set_configuration()

    eeprom = b"".join(ctrl_in(dev, 0xA1, off, 64) for off in (0x00, 0x40, 0x80, 0xC0))
    print("  EEPROM serial:", eeprom[8:16].decode("ascii", "replace"),
          "| type bytes", eeprom[:8].hex(" "))
    ctrl_out(dev, 0xA3)
    ctrl_out(dev, 0xA4, 0, 0x03F0)
    raw = max(0, round(exposure / 16e-6) - 1)
    param2 = max(1, round(20 / exposure))
    ctrl_out(dev, 0xAE, 8, 0x30, struct.pack("<II", raw, param2))
    print(f"  exposure {exposure} s (raw {raw}, param2 {param2}); starting (laser OFF)…")
    ctrl_out(dev, 0xAA, 0xFFFF, 0xFFFF)
    try:
        data = bytes(dev.read(0x82, 4608, timeout=int(exposure * 1000) + 5000))
    finally:
        ctrl_out(dev, 0xAA, 0, 0)
    px = struct.unpack(f">{len(data) // 2}H", data)
    act = px[:ACTIVE_PIXELS]
    print(f"  frame: {len(px)} pixels; active min {min(act)} max {max(act)} mean {sum(act) / len(act):.0f}")

    fn = time.strftime("dark_%Y%m%d_%H%M%S.csv")
    with open(fn, "w") as f:
        f.write(f"# Cora 5000 dark frame, exposure {exposure} s, spectrometer #1 (532 nm channel), laser off\n")
        f.write("pixel,wavelength_nm,raman_shift_cm1,counts\n")
        for i, c in enumerate(act):
            wl = sum(k * (PIXEL0 - i) ** n for n, k in enumerate(WL_COEFF))
            f.write(f"{i},{wl:.3f},{1e7 / LASER_NM - 1e7 / wl:.2f},{c}\n")
    print(f"Saved {fn}. The spectrometer stays powered; the vendor software or 'On' resets everything.")


if __name__ == "__main__":
    main()
