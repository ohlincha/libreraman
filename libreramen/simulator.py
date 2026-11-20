"""
Simulated Cora 5000, built from spectra recorded on the real instrument (water, ZnS, CCl4, CHCl3,
ethanol at 532 nm) and its measured dark current. Same interface as instrument.Cora.

Extra controls for demos and tests: press_arm(), open_compartment(), close_compartment(), sample.
"""

import contextlib
import os
import threading
import time

import numpy as np

from . import calibration as calmod
from .instrument import Cora, CoraError, NotArmed

DATA = os.path.join(os.path.dirname(__file__), "data", "simulator.npz")


class _SimSpec:
    def __init__(self, channel):
        self.channel = channel
        self.exposure_s = None

    def init(self):
        pass

    def set_exposure(self, s):
        self.exposure_s = s

    def pixel_numbers(self):
        return list(range(2048)) if self.channel == 532 else list(range(256))

    def serial(self):
        return "SIMULATOR"


class SimCora(Cora):
    simulated = True

    def __init__(self, calibration=None, log=print, speed=1.0, seed=None):
        super().__init__(calibration or calmod.DEFAULT, log)
        d = np.load(DATA)
        self.dark_offset = d["dark_offset"].astype(float)
        self.dark_rate = d["dark_rate"].astype(float)
        self.samples = {str(n): s.astype(float) for n, s in zip(d["names"], d["spectra"])}
        self.sample = "ethanol"
        self.speed = speed
        self.rng = np.random.default_rng(seed)
        self.armed = False
        self.compartment_open = False
        self.initialized = False
        self.laser_time = {}           # sample -> accumulated laser exposure (for bleaching)
        self._stop = threading.Event()

    # --- demo controls
    def press_arm(self):
        if not self.compartment_open:
            self.armed = True

    def open_compartment(self):
        self.compartment_open = True
        self.armed = False

    def close_compartment(self):
        self.compartment_open = False

    io_port = None

    def keepalive(self):
        pass

    def laser_settle_s(self):
        return 1.0 / self.speed

    # --- interface
    def connect(self):
        time.sleep(0.3 / self.speed)
        self.log("Connected to the simulator (spectra recorded on a real Cora 5000)")
        return self

    def status(self):
        return {"armed": self.armed, "compartment_open": self.compartment_open, "gpio": None,
                "channel": self.channel, "laser_on": self.laser_on,
                "spectrometers": [532, 1064] if self.initialized else []}

    def initialize(self, progress=None):
        steps = 10
        for k in range(steps):
            time.sleep(0.6 / self.speed)
            if progress:
                progress((k + 1) / steps)
        self.spec = {532: _SimSpec(532), 1064: _SimSpec(1064)}
        self.initialized = True
        self.channel = 532
        self.log("Initialized (simulator, 532 nm path)")

    def select_channel(self, ch, wait_cooling=True):
        if ch == 1064:
            raise CoraError("The simulator has no 1064 nm data (the development unit's 1064 nm channel is faulty).")
        self.channel = ch

    def _laser_off(self):
        if self.laser_on:
            self.log("Laser OFF")
        self.laser_on = False

    @contextlib.contextmanager
    def _laser(self, power_mw=None):
        if self.compartment_open:
            raise NotArmed("The sample compartment is open.")
        if not self.armed:
            raise NotArmed("Not armed: press ARM on the instrument (needed after every opening).")
        try:
            self.laser_on = True
            self.log("Laser ON (532 nm, simulated)")
            time.sleep(1.0 / self.speed)
            yield
        finally:
            self._laser_off()

    def frames(self, exposure_s, n):
        out = []
        for _ in range(n):
            t0 = time.monotonic()
            y = self.dark_offset + self.dark_rate * exposure_s
            if self.laser_on:
                s = self.samples[self.sample]
                lt = self.laser_time.get(self.sample, 0.0)
                bleach = 0.6 + 0.4 * np.exp(-lt / 180.0) if self.sample == "zinc sulfide" else 1.0
                y = y + s * exposure_s * bleach
                self.laser_time[self.sample] = lt + exposure_s
            y = y + self.rng.normal(0, 1, y.shape) * np.sqrt(np.maximum(y, 1) + 64)
            if self.rng.random() < 0.08 * exposure_s:          # occasional cosmic ray
                i = self.rng.integers(5, len(y) - 5)
                y[i:i + 2] += self.rng.uniform(2000, 15000)
            out.append(list(np.clip(y, 0, 65535)))
            dt = exposure_s / self.speed - (time.monotonic() - t0)
            if dt > 0:
                time.sleep(dt)
        return out

    def _bleach(self, seconds, stop_event=None):
        self.laser_time[self.sample] = self.laser_time.get(self.sample, 0.0) + seconds
        time.sleep(seconds / self.speed)

    def close(self, power_down=False):
        self._laser_off()
