"""
Runs the instrument for the GUI. Hardware calls happen in worker threads; results come back to
the GUI thread as Qt signals. Status is polled about twice per second, also during measurements.
"""

import os
import threading
import time
import traceback

from PySide6.QtCore import QObject, QTimer, Signal

from ..instrument import Cora, CoraError, NotArmed
from ..spectrum import iso


class Controller(QObject):
    status = Signal(dict)          # armed, compartment_open, laser_on, channel, connection, ...
    log = Signal(str)
    connected = Signal(dict)       # info about the instrument
    connect_failed = Signal(str)
    init_progress = Signal(float)
    spectrum = Signal(object)      # Spectrum
    series_progress = Signal(dict)
    busy = Signal(bool)
    error = Signal(str)

    def __init__(self):
        super().__init__()
        self.inst = None
        self.simulated = False
        self.connection = "searching"
        self.stop_event = threading.Event()
        self.pause_event = threading.Event()
        self.worker = None
        self.timer = QTimer(self)
        self.timer.timeout.connect(self.poll)
        self.timer.start(500)
        self.series_settings = None

    # ------------------------------------------------------------------ helpers
    def _run(self, fn, *args):
        if self.worker and self.worker.is_alive():
            self.error.emit("Busy: stop the current measurement first.")
            return False

        def wrap():
            self.busy.emit(True)
            try:
                fn(*args)
            except NotArmed as e:
                self.error.emit(str(e))
            except CoraError as e:
                self.error.emit(str(e))
            except Exception as e:  # noqa: BLE001
                self.error.emit(f"{e}\n{traceback.format_exc()}")
            finally:
                self.busy.emit(False)
        self.stop_event.clear()
        self.worker = threading.Thread(target=wrap, daemon=True)
        self.worker.start()
        return True

    def _log(self, msg):
        self.log.emit(msg)

    def poll(self):
        st = {"connection": self.connection, "simulated": self.simulated}
        if self.inst is not None and self.connection == "connected":
            try:
                st.update(self.inst.status())
            except Exception as e:  # noqa: BLE001
                self.connection = "lost"
                st["connection"] = "lost"
                self._log(f"Connection lost: {e}")
        self.status.emit(st)

    @property
    def is_busy(self):
        return bool(self.worker and self.worker.is_alive())

    # ------------------------------------------------------------------ connection
    def connect_instrument(self, simulate=False):
        def job():
            self.connection = "searching"
            if simulate:
                from ..simulator import SimCora
                inst = SimCora(log=self._log)
            else:
                inst = Cora(log=self._log)
            try:
                inst.connect()
            except Exception as e:  # noqa: BLE001
                self.connection = "not found"
                self.connect_failed.emit(str(e))
                return
            self.inst, self.simulated = inst, simulate
            self.connection = "initialising"
            if simulate:
                inst.initialize(progress=self.init_progress.emit)
            else:
                if len(inst.spec) < 2:
                    inst.initialize()
                else:
                    inst.channel = 532
                    inst.spec[532].init()
            self.init_progress.emit(1.0)
            self.connection = "connected"
            serial = "SIMULATOR" if simulate else inst.spec[532].serial()
            self.connected.emit({"model": "Cora 5000", "serial": serial, "simulated": simulate,
                                 "calibration": ", ".join(f"{ch} nm: {c['date']}" for ch, c in inst.user_cal.items())
                                 or "factory"})
        self._run(job)

    # ------------------------------------------------------------------ measuring
    def measure_once(self, s):
        def job():
            sp = self.inst.acquire(s["exposure"], s["averages"], background=s["background"] != "none")
            sp.meta.update(sample=s["sample"])
            self._save(sp, s, single=True)
            self.spectrum.emit(sp)
        self._run(job)

    def start_series(self, s):
        self._run(self._series, s)

    def _series(self, s):
        """Time series: one spectrum every interval; pauses when disarmed, continues after ARM."""
        t0 = time.time()
        if s["start_delay"] > 0:
            self._log(f"Series starts in {s['start_delay']:.0f} s")
        if s.get("fastest"):
            return self._series_fastest(s)
        n_total = s["count"] if s["stop_by"] == "count" else max(1, int(s["duration"] // s["interval"]) + 1)
        folder = os.path.join(s["folder"], time.strftime(f"{_safe(s['sample'])}_series_%Y%m%d_%H%M%S"))
        os.makedirs(folder, exist_ok=True)
        table = os.path.join(folder, "series.csv")
        cols, start = [], t0 + s["start_delay"]
        bg_once = None
        k = 0
        while k < n_total and not self.stop_event.is_set():
            target = start + k * s["interval"]
            while time.time() < target and not self.stop_event.is_set():
                self.series_progress.emit({"n": k, "total": n_total, "next_in": target - time.time(),
                                           "elapsed": time.time() - start, "end": start + (n_total - 1) * s["interval"]})
                time.sleep(0.25)
            if self.stop_event.is_set():
                break
            if time.time() - target > 1.0:
                self._log(f"Behind schedule by {time.time() - target:.1f} s (spectrum {k + 1})")
            st = self.inst.status()
            if not st["armed"] or st["compartment_open"]:
                self._log("Paused: not armed (compartment opened?). Press ARM on the instrument to continue.")
                while not self.stop_event.is_set():
                    st = self.inst.status()
                    if st["armed"] and not st["compartment_open"]:
                        self._log("Armed again: continuing")
                        break
                    self.series_progress.emit({"n": k, "total": n_total, "paused": True,
                                               "elapsed": time.time() - start})
                    time.sleep(0.5)
                if self.stop_event.is_set():
                    break
            if s["background"] == "once" and bg_once is None:
                bg_once = self.inst.dark(s["exposure"], s["averages"])
            sp = self.inst.acquire(s["exposure"], s["averages"], background=s["background"] == "each")
            if bg_once is not None:
                px_bg = dict(zip(self.inst.spec[self.inst.channel].pixel_numbers(), bg_once))
                sp.background = [px_bg[p] for p in sp.pixels]
                sp.intensity = [a - b for a, b in zip(sp.signal, sp.background)]
            sp.meta.update(sample=s["sample"], series_index=k + 1, series_elapsed_s=round(sp.t - start, 1))
            sp.save_csv(os.path.join(folder, f"{k + 1:04d}_{time.strftime('%H%M%S', time.localtime(sp.t))}.csv"))
            cols.append((sp.t - start, sp.intensity))
            _write_table(table, s, cols, sp.shift, start)
            self.spectrum.emit(sp)
            k += 1
            self.series_progress.emit({"n": k, "total": n_total, "elapsed": time.time() - start,
                                       "end": start + (n_total - 1) * s["interval"], "next_in": s["interval"]})
        self._log(f"Series {'stopped' if self.stop_event.is_set() else 'finished'}: {k} spectra in {folder}")
        self.series_progress.emit({"n": k, "total": n_total, "done": True, "elapsed": time.time() - start})

    def _series_fastest(self, s):
        """Back-to-back spectra with the laser kept on; background once at the start (laser off)."""
        inst = self.inst
        start = time.time()
        folder = os.path.join(s["folder"], time.strftime(f"{_safe(s['sample'])}_series_%Y%m%d_%H%M%S"))
        os.makedirs(folder, exist_ok=True)
        table = os.path.join(folder, "series.csv")
        n_total = s["count"] if s["stop_by"] == "count" else None
        bg = inst.dark(s["exposure"], s["averages"])
        px_bg = dict(zip(inst.spec[inst.channel].pixel_numbers(), bg))
        cols, k = [], 0
        with inst.lock, inst._laser():
            while not self.stop_event.is_set():
                if n_total is not None and k >= n_total:
                    break
                if n_total is None and time.time() - start > s["duration"]:
                    break
                sig = inst.frames(s["exposure"], s["averages"])
                sig = [sum(v) / len(v) for v in zip(*sig)]
                sp = inst._spectrum(inst.channel, s["exposure"], s["averages"], sig,
                                    [px_bg[p] for p in inst.spec[inst.channel].pixel_numbers()], None)
                sp.meta.update(sample=s["sample"], series_index=k + 1, series_elapsed_s=round(sp.t - start, 1),
                               mode="as fast as possible, laser on, background once")
                sp.save_csv(os.path.join(folder, f"{k + 1:04d}_{time.strftime('%H%M%S', time.localtime(sp.t))}.csv"))
                cols.append((sp.t - start, sp.intensity))
                _write_table(table, s, cols, sp.shift, start)
                self.spectrum.emit(sp)
                k += 1
                self.series_progress.emit({"n": k, "total": n_total or k, "elapsed": time.time() - start})
        self._log(f"Series {'stopped' if self.stop_event.is_set() else 'finished'}: {k} spectra in {folder}")
        self.series_progress.emit({"n": k, "total": n_total or k, "done": True, "elapsed": time.time() - start})

    def _save(self, sp, s, single):
        os.makedirs(s["folder"], exist_ok=True)
        path = os.path.join(s["folder"], time.strftime(f"{_safe(s['sample'])}_%Y%m%d_%H%M%S.csv",
                                                      time.localtime(sp.t)))
        sp.save_csv(path)
        sp.meta["file"] = path
        self._log(f"Saved {path}")

    def stop(self):
        self.stop_event.set()
        if self.inst is not None:
            try:
                self.inst._laser_off()
            except Exception:  # noqa: BLE001
                pass

    # simulator controls
    def sim(self, action, value=None):
        if not self.simulated:
            return
        if action == "arm":
            self.inst.press_arm()
        elif action == "open":
            self.inst.open_compartment()
        elif action == "close":
            self.inst.close_compartment()
        elif action == "sample":
            self.inst.sample = value

    def shutdown(self, power_down=False):
        self.stop()
        if self.inst is not None:
            try:
                self.inst.close(power_down=power_down)
            except Exception:  # noqa: BLE001
                pass


def _safe(name):
    return "".join(c if c.isalnum() or c in "-_." else "_" for c in (name or "sample"))[:60]


def _write_table(path, s, cols, shift, start):
    tmp = path + ".tmp"
    with open(tmp, "w") as f:
        f.write(f"# LibreRamen time series, started {iso(start)}\n# sample: {s['sample']}\n"
                f"# exposure_s: {s['exposure']}\n# averages: {s['averages']}\n# interval_s: {s['interval']}\n"
                f"# background: {s['background']}\n")
        f.write("raman_shift_cm1," + ",".join(f"t={e:.1f}s" for e, _ in cols) + "\n")
        for i, x in enumerate(shift):
            f.write(f"{x:.2f}," + ",".join(f"{c[i]:.1f}" for _, c in cols) + "\n")
    os.replace(tmp, path)
