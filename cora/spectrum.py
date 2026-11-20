"""Spectrum container and CSV output."""

import datetime as dt
import time
from dataclasses import dataclass, field


@dataclass
class Spectrum:
    channel: int                    # 532 or 1064
    exposure_s: float
    averages: int
    shift: list                     # Raman shift (cm^-1), ascending
    intensity: list                 # signal - background (counts per frame)
    signal: list = field(default_factory=list)
    background: list = field(default_factory=list)
    t: float = field(default_factory=time.time)
    meta: dict = field(default_factory=dict)

    def save_csv(self, path):
        with open(path, "w", newline="") as f:
            f.write(f"# Cora 5000 spectrum, {iso(self.t)}\n")
            f.write(f"# channel: {self.channel} nm\n# exposure_s: {self.exposure_s}\n# averages: {self.averages}\n")
            for k, v in self.meta.items():
                f.write(f"# {k}: {v}\n")
            f.write("raman_shift_cm1,intensity,signal,background\n")
            for i, x in enumerate(self.shift):
                f.write(f"{x:.2f},{self.intensity[i]:.1f},"
                        f"{self.signal[i] if self.signal else ''},{self.background[i] if self.background else ''}\n")


def iso(t):
    return dt.datetime.fromtimestamp(t).astimezone().isoformat(timespec="seconds")
