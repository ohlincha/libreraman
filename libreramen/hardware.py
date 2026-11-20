"""
Low-level access to the parts inside the Cora 5000:

* IOBoard      - Prolific PL2303 serial port: power lines, 532 nm laser line (09), VOL reading.
* ArmStatus    - Microchip MCP2200 (HID): ARM state (GPIO bit 0) and compartment switch (bit 7).
* LaserDriver  - Silicon Labs CP210x serial port: 1064 nm laser driver (current in A).
* Spectrometer - BaySpec USB spectrometers (ba8c:1688): "BS2ACCD" = 532 nm CCD, "BS2BASE" = 1064 nm.

See PROTOCOL.md for how all of this was worked out.
"""

import glob
import os
import select
import struct
import sys
import time

import serial

CORA_HUB = ("0424", "2517")
PROLIFIC = ("067b", "2303")
MCP2200 = ("04d8", "00df")
CP210X = ("10c4", "ea60")
BAYSPEC = (0xBA8C, 0x1688)
LASER_LINE = "09"


class HardwareError(Exception):
    pass


# ----------------------------------------------------------------------------- discovery (Linux)
def _ids(d):
    try:
        return (open(os.path.join(d, "idVendor")).read().strip(),
                open(os.path.join(d, "idProduct")).read().strip())
    except OSError:
        return None


def _usb_chain(sys_dev):
    """IDs of the owning USB device and all its parent hubs, nearest first."""
    out, p = [], os.path.realpath(sys_dev)
    while p not in ("/", ""):
        v = _ids(p)
        if v:
            out.append(v)
        p = os.path.dirname(p)
    return out


def find_tty(vid_pid, inside_cora=True):
    """/dev/tty* of a USB serial chip; with inside_cora, only one behind the Cora's hub."""
    if sys.platform.startswith("linux"):
        for dev in sorted(glob.glob("/dev/ttyUSB*") + glob.glob("/dev/ttyACM*")):
            chain = _usb_chain(f"/sys/class/tty/{os.path.basename(dev)}/device")
            if chain and chain[0] == vid_pid and (not inside_cora or CORA_HUB in chain[1:]):
                return dev
        return None
    from serial.tools import list_ports   # other systems: match by USB ID only
    for p in list_ports.comports():
        if p.vid is not None and (f"{p.vid:04x}", f"{p.pid:04x}") == vid_pid:
            return p.device
    return None


def find_hidraw(vid_pid):
    for dev in sorted(glob.glob("/dev/hidraw*")):
        chain = _usb_chain(f"/sys/class/hidraw/{os.path.basename(dev)}/device")
        if chain and chain[0] == vid_pid:
            return dev
    return None


# ----------------------------------------------------------------------------- I/O board
class IOBoard:
    """ASCII commands, LF-terminated, answered with 'OK'. 115200 8N1, DTR/RTS off.
    The vendor software sends one character at a time ~80 ms apart; we keep a (shorter) pacing."""

    def __init__(self, port, pacing=0.03):
        self.s = serial.Serial(port, 115200, timeout=0.1)
        self.s.dtr = False
        self.s.rts = False
        self.pacing = pacing
        time.sleep(0.1)
        self.s.reset_input_buffer()
        self.lines = {}      # last state we set: line -> "H"/"L"

    def command(self, text, timeout=3.0):
        for ch in (text + "\n").encode():
            self.s.write(bytes([ch]))
            self.s.flush()
            time.sleep(self.pacing)
        buf, t_end = b"", time.monotonic() + timeout
        while time.monotonic() < t_end and not buf.endswith(b"OK\n"):
            buf += self.s.read(64)
        reply = buf.decode("ascii", "replace").strip()
        if not reply.endswith("OK"):
            raise HardwareError(f"I/O board did not confirm {text!r} (got {reply!r})")
        return reply[:-2].strip()

    def set_line(self, line, high, _allow_laser=False):
        if line == LASER_LINE and not _allow_laser:
            raise HardwareError("the laser line is switched only through Cora.laser_532()")
        self.command(f"IO {'H' if high else 'L'} {line}")
        self.lines[line] = "H" if high else "L"

    def vol(self):
        r = self.command("VOL 03")
        return float(r.split(":")[1]) if ":" in r else None

    def close(self):
        self.s.close()


# ----------------------------------------------------------------------------- ARM status (MCP2200)
class ArmStatus:
    """MCP2200 over Linux hidraw. READ_ALL (0x80) -> byte 10 = GPIO port.
    bit 0 = 0: armed (ARM button pressed, cleared when the compartment opens); bit 7 = 0: compartment open."""

    def __init__(self, path):
        import threading
        self.fd = os.open(path, os.O_RDWR)
        self.lock = threading.Lock()      # status is polled from the GUI while a measurement runs

    def _cmd(self, data, reply=True):
        with self.lock:
            os.write(self.fd, bytes([0]) + bytes(data).ljust(16, b"\0"))
            if not reply:
                return b""
            r, _, _ = select.select([self.fd], [], [], 2.0)
            return os.read(self.fd, 64) if r else b""

    def gpio(self):
        for _ in range(3):
            rep = self._cmd([0x80])
            if len(rep) > 10 and rep[0] == 0x80:
                return rep[10]
        raise HardwareError("no status from the MCP2200")

    def state(self):
        g = self.gpio()
        return {"armed": not (g & 0x01), "compartment_open": not (g & 0x80), "gpio": g}

    def pulse(self):
        """Outputs low then high, as the vendor software does around measurements (purpose unknown)."""
        self._cmd([0x08] + [0] * 10 + [0x00, 0xFF], reply=False)
        time.sleep(0.1)
        self._cmd([0x08] + [0] * 10 + [0xFF, 0x00], reply=False)

    def close(self):
        os.close(self.fd)


# ----------------------------------------------------------------------------- 1064 nm laser driver
class LaserDriver:
    """CR-terminated ASCII at 19200 8N1. Commands seen: rev?, tecmax, temperature, laseron, laseroff, current."""

    def __init__(self, port):
        self.s = serial.Serial(port, 19200, timeout=0.3)
        self.s.dtr = False
        self.s.rts = False
        time.sleep(0.2)
        self.s.reset_input_buffer()

    def command(self, text):
        self.s.write((text + "\r").encode())
        self.s.flush()
        time.sleep(0.12)
        return self.s.read(200).decode("ascii", "replace").strip()

    def init(self):
        self.command("")
        rev = self.command("rev?")
        for c in ("tecmax 0.6", "temperature 25.00", "laseroff", "current 0.00"):
            self.command(c)
        return rev

    def set_current(self, amps):
        self.command(f"current {amps:.2f}")

    def on(self):
        self.command("laseron")

    def off(self):
        self.command("current 0.00")
        self.command("laseroff")

    def close(self):
        self.s.close()


# ----------------------------------------------------------------------------- spectrometers
class Spectrometer:
    """BaySpec USB spectrometer. Vendor requests (bmRequestType 0x40 out / 0xC0 in):
    A1 read EEPROM, A3 init, A4 flag, AD (1064 only), AE exposure, AA start/stop; frame on bulk 0x82."""

    PRODUCTS = {532: "BS2ACCD", 1064: "BS2BASE"}
    FRAME_BYTES = {532: 4608, 1064: 1024}
    ACTIVE = {532: (10, 2058), 1064: (0, 256)}          # data index range with real pixels

    def __init__(self, dev, channel):
        self.dev = dev
        self.channel = channel
        try:
            dev.get_active_configuration()      # never re-configure: that drops the next frame's first packet
        except Exception:  # noqa: BLE001
            dev.set_configuration()
        self.exposure_s = None

    @classmethod
    def find(cls, channel, timeout=0.0):
        import usb.core
        import usb.util
        t_end = time.monotonic() + timeout
        while True:
            for d in usb.core.find(find_all=True, idVendor=BAYSPEC[0], idProduct=BAYSPEC[1]):
                try:
                    if usb.util.get_string(d, d.iProduct) == cls.PRODUCTS[channel]:
                        return cls(d, channel)
                except Exception:  # noqa: BLE001
                    continue
            if time.monotonic() >= t_end:
                return None
            time.sleep(0.25)

    def _out(self, req, value=0, index=0, data=b""):
        self.dev.ctrl_transfer(0x40, req, value, index, data, timeout=1000)

    def _in(self, req, index, length):
        return bytes(self.dev.ctrl_transfer(0xC0, req, 0, index, length, timeout=1000))

    def eeprom(self):
        return b"".join(self._in(0xA1, off, 64) for off in (0x00, 0x40, 0x80, 0xC0))

    def serial(self):
        return self.eeprom()[8:16].decode("ascii", "replace")

    def init(self):
        self._out(0xA3)
        if self.channel == 532:
            self._out(0xA4, 0, 0x03F0)
        else:
            self._out(0xA4, 1, 0x03F0)
            self._out(0xAD, 0, 0x38)
            self._out(0xA4, 1, 0x03F0)

    def set_exposure(self, seconds):
        if self.channel == 532:
            raw, p2 = max(0, round(seconds / 16e-6) - 1), max(1, min(200, round(20 / seconds)))
        else:
            raw, p2 = max(0, round(seconds / 0.8e-6) - 25), 1
        self._out(0xAE, 8, 0x30, struct.pack("<II", raw, p2))
        self.exposure_s = seconds

    def frame(self):
        """One exposure; returns the active pixels in data order (raw counts)."""
        self._out(0xAA, 0xFFFF, 0xFFFF)
        try:
            data = bytes(self.dev.read(0x82, self.FRAME_BYTES[self.channel],
                                       timeout=int(self.exposure_s * 1000) + 5000))
        finally:
            self._out(0xAA, 0, 0)
        px = struct.unpack(f">{len(data) // 2}H", data)
        a, b = self.ACTIVE[self.channel]
        if len(px) < b:
            raise HardwareError(f"short frame: {len(data)} bytes")
        return list(px[a:b])

    def pixel_numbers(self):
        """Calibration pixel p for each returned value. 532: reversed (p = 2057 - i, verified with water).
        1064: assumed p = i (orientation not yet verified)."""
        a, b = self.ACTIVE[self.channel]
        if self.channel == 532:
            return [2057 - i for i in range(a, b)]
        return list(range(a, b))
