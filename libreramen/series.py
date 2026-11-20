"""Time series / scheduled scans: one spectrum every `interval_s`, written to CSV as it goes."""

import time

from .spectrum import iso


def run_series(cora, path, exposure_s, interval_s, count, averages=1, background="each", power_mw=None,
               stop_event=None, on_spectrum=None):
    """background: 'each' (a background with every spectrum) or 'once' (one at the start, laser-off)."""
    t0 = time.time()
    bg_once = cora.dark(exposure_s, averages) if background == "once" else None
    cols, shift = [], None
    with open(path, "w") as f:
        f.write(f"# Cora 5000 time series, started {iso(t0)}\n# channel: {cora.channel} nm\n"
                f"# exposure_s: {exposure_s}\n# averages: {averages}\n# interval_s: {interval_s}\n"
                f"# background: {background}\n")
        for k in range(count):
            target = t0 + k * interval_s
            while time.time() < target:
                if stop_event is not None and stop_event.wait(min(0.5, target - time.time())):
                    return cols
                elif stop_event is None:
                    time.sleep(min(0.5, max(0, target - time.time())))
            sp = cora.acquire(exposure_s, averages, background=(background == "each"), power_mw=power_mw)
            if bg_once is not None:
                px = cora.spec[cora.channel].pixel_numbers()
                cal = cora.cal.channels[cora.channel]
                bg = [b for _, b in sorted(zip((cal.shift(p) for p in px), bg_once))]
                sp.intensity = [s - b for s, b in zip(sp.signal, bg)]
            elapsed = sp.t - t0
            cols.append((elapsed, sp.intensity))
            shift = sp.shift
            if on_spectrum:
                on_spectrum(k, elapsed, sp)
            # rewrite the table each time so the file is always complete
            f.seek(0)
            f.truncate()
            f.write(f"# Cora 5000 time series, started {iso(t0)}\n# channel: {cora.channel} nm\n"
                    f"# exposure_s: {exposure_s}\n# averages: {averages}\n# interval_s: {interval_s}\n"
                    f"# background: {background}\n")
            f.write("raman_shift_cm1," + ",".join(f"t={e:.1f}s" for e, _ in cols) + "\n")
            for i, x in enumerate(shift):
                f.write(f"{x:.2f}," + ",".join(f"{c[i]:.1f}" for _, c in cols) + "\n")
            f.flush()
    return cols
