"""
The Cora 5000 as one object: start-up, channel selection, status, laser control and acquisition.

Laser safety:
* The instrument can only emit when a person has pressed ARM (GPIO bit 0); opening the compartment
  disarms it. acquire() refuses to switch a laser on when the instrument isn't armed.
* Lasers are switched on only inside the _laser() context manager, which always switches them off,
  also on errors and KeyboardInterrupt. close() switches both off as well.
"""

import contextlib
import threading
import time

from . import calibrate as calib
from . import calibration as calmod
from .hardware import (CP210X, PROLIFIC, ArmStatus, HardwareError, IOBoard, LaserDriver, Pl2303Usb, Spectrometer,
                       find_tty, open_mcp2200, tty_candidates)
from .spectrum import Spectrum

# Line states copied from the vendor software (see PROTOCOL.md).
INIT_532 = [("01", True), ("00", True), ("0a", False), ("0e", True), ("0c", False),
            ("0d", False), ("0b", True), ("08", True), ("07", True), ("0b", False)]
TO_1064_POWER = [("0b", True), ("07", False), ("0b", False)]      # powers the laser driver
TO_1064_PATH = [("0e", False), ("0f", True)]
TO_532 = [("07", True), ("0b", True), ("0b", False), ("0f", False), ("0e", True)]
LASER_SETTLE_532 = 3.6          # s between laser on and the first frame (vendor: ~3.7 s)
LASER_SETTLE_1064 = 1.5


class CoraError(Exception):
    pass


class NotArmed(CoraError):
    pass


class Cora:
    def __init__(self, calibration=None, log=print):
        self.cal = calibration or calmod.DEFAULT
        self.log = log
        self.lock = threading.RLock()
        self.io = self.arm = self.driver = None
        self.spec = {}                 # channel -> Spectrometer
        self.channel = None
        self.laser_on = False
        self.power_mw = {532: None, 1064: 50.0}
        self.user_cal = {}             # channel -> fitted shift polynomial (from calibrate.py)

    def load_user_calibration(self):
        for ch in (532, 1064):
            c = calib.load(ch, self.cal.serial)
            if c:
                self.user_cal[ch] = c
                self.log(f"Using calibration for {ch} nm from {c['date']} "
                         f"({', '.join(c['standards'])}, RMS {c['rms']:.2f} cm-1)")

    def shift_of(self, ch):
        if ch in self.user_cal:
            p = self.user_cal[ch]["poly"]
            return lambda px: calib.polyval(p, px)
        return self.cal.channels[ch].shift

    # ------------------------------------------------------------------ connection
    def connect(self):
        port = find_tty(PROLIFIC, inside_cora=True)
        candidates = [port] if port else tty_candidates(PROLIFIC)
        import sys
        if sys.platform == "win32":          # prefer the chip driven directly (WinUSB); the COM port may be refused
            candidates = Pl2303Usb.find_all() + candidates
        for cand in candidates:            # several Prolific adapters (e.g. a Titrino cable): ask each politely
            try:
                io = IOBoard(cand)
                io.vol()                   # harmless query that only the Cora's I/O board answers
                self.io, port = io, cand
                break
            except Exception:  # noqa: BLE001
                try:
                    io.close()
                except Exception:  # noqa: BLE001
                    pass
        if not self.io:
            import sys
            hint = ""
            if sys.platform == "win32":
                from .windrivers import HINT
                hint = (" On Windows the I/O board's old Prolific chip is refused by Prolific's current driver "
                        "('PL2303HXA PHASED OUT') and needs the WinUSB driver instead. " + HINT)
            raise CoraError("Cora not found (I/O board). Is the USB cable connected and the instrument on "
                            "(On pressed, screen ready)?" + hint)
        self.io_port = port if isinstance(port, str) else None     # (watchdog needs a serial port name)
        try:                               # safety: whatever happened before, start with the 532 nm laser off
            self.io.set_line("09", True, _allow_laser=True)
        except Exception:  # noqa: BLE001
            pass
        t = open_mcp2200()
        if not t:
            raise CoraError("MCP2200 (ARM status) not found")
        self.arm = ArmStatus(t)
        hid = type(t).__name__
        for ch in (532, 1064):
            s = Spectrometer.find(ch)
            if s:
                self.spec[ch] = s
        self.load_user_calibration()
        self.log(f"Connected: I/O board {getattr(port, 'port', port)}, ARM status {hid}, spectrometers {sorted(self.spec)}")
        return self

    def keepalive(self):
        """Like the vendor software's regular VOL query; skipped while a measurement holds the instrument."""
        if self.io and self.lock.acquire(blocking=False):
            try:
                self.io.vol()
            finally:
                self.lock.release()

    def status(self):
        st = self.arm.state()
        st.update(channel=self.channel, laser_on=self.laser_on, spectrometers=sorted(self.spec))
        return st

    # ------------------------------------------------------------------ start-up / channels
    def initialize(self):
        """Power the spectrometers and set the lines like the vendor's 'Initialize system' (532 nm path)."""
        with self.lock:
            self.io.command("TECE")
            for line, high in INIT_532:
                self.io.set_line(line, high)
                if line == "0c" and not high:
                    self._wait_spec(532)
                if line == "0d" and not high:
                    self._wait_spec(1064)
            self.spec[532].init()
            self.spec[1064].init()
            self.channel = 532
            self.log("Initialized (532 nm path)")

    def _wait_spec(self, ch, timeout=10):
        s = Spectrometer.find(ch, timeout)
        if not s:
            hint = ""
            import sys
            if sys.platform == "win32":
                from .windrivers import HINT
                hint = (" On Windows the spectrometers need the WinUSB driver (installed by LibreRaman-Setup; "
                        "the vendor software then can't use them on this PC). " + HINT)
            raise CoraError(f"spectrometer for {ch} nm did not appear on USB.{hint}")
        self.spec[ch] = s

    def select_channel(self, ch, wait_cooling=True):
        with self.lock:
            if ch == self.channel:
                return
            self._laser_off()
            if ch == 1064:
                for line, high in TO_1064_POWER:
                    self.io.set_line(line, high)
                port = None
                t_end = time.monotonic() + 15
                while not port and time.monotonic() < t_end:
                    port = find_tty(CP210X)
                    time.sleep(0.3)
                if not port:
                    raise CoraError("1064 nm laser driver did not appear")
                self.driver = LaserDriver(port)
                self.log("1064 nm laser driver: " + self.driver.init())
                for line, high in TO_1064_PATH:
                    self.io.set_line(line, high)
                self.spec[1064].init()
                self.channel = 1064
                if wait_cooling:
                    t = self.cal.channels[1064].tec_wait_s
                    self.log(f"Waiting {t:.0f} s for the 1064 nm detector to cool")
                    time.sleep(t)
            else:
                if self.driver:
                    self.driver.off()
                    self.driver.close()
                    self.driver = None
                for line, high in TO_532:
                    self.io.set_line(line, high)
                self.spec[532].init()
                self.channel = 532
            self.log(f"Channel: {ch} nm")

    # ------------------------------------------------------------------ lasers
    def _laser_off(self):
        errors = []
        try:
            self.io.set_line("09", True, _allow_laser=True)
        except Exception as e:  # noqa: BLE001
            errors.append(e)
        if self.driver:
            try:
                self.driver.off()
            except Exception as e:  # noqa: BLE001
                errors.append(e)
        if self.laser_on:
            self.log("Laser OFF")
        self.laser_on = False
        if errors:
            raise HardwareError(f"could not confirm laser off: {errors}")

    @contextlib.contextmanager
    def _laser(self, power_mw=None):
        st = self.arm.state()
        if st["compartment_open"]:
            raise NotArmed("The sample compartment is open.")
        if not st["armed"]:
            raise NotArmed("Not armed: press ARM on the instrument (needed after every opening).")
        self.arm.pulse()
        try:
            if self.channel == 532:
                self.io.set_line("09", False, _allow_laser=True)
                self.laser_on = True
                self.log("Laser ON (532 nm)")
                time.sleep(LASER_SETTLE_532)
            else:
                c = self.cal.channels[1064]
                p = power_mw or self.power_mw[1064]
                if not 0 < p <= c.pwr_max_mw:
                    raise CoraError(f"power must be 0-{c.pwr_max_mw:.0f} mW")
                self.driver.on()
                self.driver.set_current(c.pwr_coeff[0] + c.pwr_coeff[1] * p)
                self.laser_on = True
                self.log(f"Laser ON (1064 nm, {p:.0f} mW)")
                time.sleep(LASER_SETTLE_1064)
            yield
        finally:
            self._laser_off()
            self.arm.pulse()

    # ------------------------------------------------------------------ acquisition
    READOUT_S = 0.02            # per frame (measured: a 1 s frame takes 1.01 s)
    OVERHEAD_S = 0.5            # arming check, laser commands, saving

    def laser_settle_s(self):
        return LASER_SETTLE_532 if self.channel != 1064 else LASER_SETTLE_1064

    def cycle_time(self, exposure_s, averages, background=True, laser_stays_on=False):
        """Minimum time for one spectrum with these settings (s)."""
        frames = averages * (2 if background else 1)
        settle = 0.0 if laser_stays_on else self.laser_settle_s()
        return settle + frames * (exposure_s + self.READOUT_S) + self.OVERHEAD_S

    def frames(self, exposure_s, n):
        sp = self.spec[self.channel]
        if sp.exposure_s != exposure_s:
            sp.set_exposure(exposure_s)
        return [sp.frame() for _ in range(n)]

    def dark(self, exposure_s, averages=1):
        with self.lock:
            return _mean(self.frames(exposure_s, averages))

    def acquire(self, exposure_s, averages=1, background=True, power_mw=None, stop_event=None, prebleach_s=0.0):
        """Laser frames (averaged), then background frames with the laser off. Returns a Spectrum.
        prebleach_s: keep the laser on this long before measuring (photobleaching reduces fluorescence)."""
        with self.lock:
            ch = self.channel
            if ch is None:
                raise CoraError("not initialized")
            with self._laser(power_mw):
                if prebleach_s > 0:
                    self.log(f"Pre-bleaching for {prebleach_s:g} s")
                    self._bleach(prebleach_s, stop_event)
                sig = _mean(self.frames(exposure_s, averages))
            bg = _mean(self.frames(exposure_s, averages)) if background else [0] * len(sig)
            sp = self._spectrum(ch, exposure_s, averages, sig, bg, power_mw)
            if prebleach_s > 0:
                sp.meta["prebleach_s"] = prebleach_s
            return sp

    def _bleach(self, seconds, stop_event=None):
        t_end = time.monotonic() + seconds
        while time.monotonic() < t_end:
            if stop_event is not None and stop_event.is_set():
                raise CoraError("stopped during pre-bleaching")
            time.sleep(min(0.2, t_end - time.monotonic()))

    def _spectrum(self, ch, exposure_s, averages, sig, bg, power_mw):
        cal = self.cal.channels[ch]
        px = self.spec[ch].pixel_numbers()
        f = self.shift_of(ch)
        rows = sorted(zip((f(p) for p in px), sig, bg, px))
        meta = {"laser_nm": cal.laser_nm,
                "calibration": (f"user {self.user_cal[ch]['date']} ({', '.join(self.user_cal[ch]['standards'])})"
                                if ch in self.user_cal else f"factory ({self.cal.source})")}
        if ch == 1064:
            meta["power_mw"] = power_mw or self.power_mw[1064]
            meta["note"] = "1064 nm pixel orientation not yet verified"
        else:
            meta["power"] = "set on the instrument (not computer-controlled)"
        sp = Spectrum(ch, exposure_s, averages, [r[0] for r in rows], [r[1] - r[2] for r in rows],
                      [r[1] for r in rows], [r[2] for r in rows], meta=meta)
        sp.pixels = [r[3] for r in rows]
        return sp

    # ------------------------------------------------------------------ shutdown
    def close(self, power_down=False):
        """Laser off and release the ports. power_down=True also sends the vendor Shutdown (USB, OFF)."""
        with self.lock:
            try:
                if self.io:
                    self._laser_off()
            finally:
                if power_down and self.io:
                    # Exact vendor timing (80 ms per character, ~0.4 s between USB and OFF). With faster
                    # sending the instrument's onboard computer shut down slowly ("killing" a program).
                    self.io.pacing = 0.08
                    self.io.vol()
                    time.sleep(1.0)
                    self.io.command("USB")
                    time.sleep(0.4)
                    self.io.command("OFF")
                for x in (self.driver, self.arm, self.io):
                    if x:
                        try:
                            x.close()
                        except Exception:  # noqa: BLE001
                            pass
                self.driver = self.arm = self.io = None


def _mean(frames):
    n = len(frames)
    return [sum(v) / n for v in zip(*frames)]
