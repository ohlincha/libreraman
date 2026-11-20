"""
Calibration against reference standards.

1. Find peaks in a background-subtracted spectrum (in calibration-pixel order).
2. Match them to the standard's reference bands, using the current calibration as the starting point.
3. Fit a polynomial Raman shift(pixel) through all matches (several standards can be combined).

The fitted polynomial replaces the factory wavelength calibration for that channel; the result
lists the residual of every band so the user can judge the quality.
"""

import json
import os
import time

from . import standards as std_lib


# ----------------------------------------------------------------------------- small numerics
def _smooth(y, w):
    h = w // 2
    out, s = [], 0.0
    pre = [0.0]
    for v in y:
        s += v
        pre.append(s)
    for i in range(len(y)):
        a, b = max(0, i - h), min(len(y), i + h + 1)
        out.append((pre[b] - pre[a]) / (b - a))
    return out


def _baseline(y, w=151):
    """Rolling minimum, then smoothed: removes fluorescence/luminescence backgrounds."""
    h = w // 2
    mins = [min(y[max(0, i - h):i + h + 1]) for i in range(len(y))]
    return _smooth(mins, w)


def _solve(a, b):
    n = len(b)
    m = [row[:] + [b[i]] for i, row in enumerate(a)]
    for c in range(n):
        piv = max(range(c, n), key=lambda r: abs(m[r][c]))
        m[c], m[piv] = m[piv], m[c]
        if abs(m[c][c]) < 1e-300:
            raise ValueError("singular fit")
        for r in range(n):
            if r != c:
                f = m[r][c] / m[c][c]
                for k in range(c, n + 1):
                    m[r][k] -= f * m[c][k]
    return [m[i][n] / m[i][i] for i in range(n)]


def polyfit(x, y, deg, w=None):
    w = w or [1.0] * len(x)
    x0 = sum(x) / len(x)
    sc = (max(x) - min(x)) / 2 or 1.0
    t = [(xi - x0) / sc for xi in x]
    a = [[sum(wi * ti ** (i + j) for wi, ti in zip(w, t)) for j in range(deg + 1)] for i in range(deg + 1)]
    b = [sum(wi * yi * ti ** i for wi, ti, yi in zip(w, t, y)) for i in range(deg + 1)]
    c = _solve(a, b)
    return {"coeff": c, "x0": x0, "scale": sc}


def polyval(p, x):
    t = (x - p["x0"]) / p["scale"]
    return sum(c * t ** i for i, c in enumerate(p["coeff"]))


# ----------------------------------------------------------------------------- peaks
def find_peaks(y, min_rel=0.02, smooth=3):
    """Peak positions (fractional pixel index) and heights above a rolling-minimum baseline."""
    ys = _smooth(y, smooth)
    base = _baseline(ys)
    z = [a - b for a, b in zip(ys, base)]
    top = max(z) or 1.0
    peaks = []
    for i in range(2, len(z) - 2):
        if z[i] >= z[i - 1] and z[i] > z[i + 1] and z[i] >= z[i - 2] and z[i] > z[i + 2] and z[i] > min_rel * top:
            a, b, c = z[i - 1], z[i], z[i + 1]
            den = a - 2 * b + c
            off = 0.5 * (a - c) / den if den else 0.0
            peaks.append((i + max(-0.5, min(0.5, off)), b))
    return peaks


# ----------------------------------------------------------------------------- matching and fit
def match(peaks, shift_of, standard, window=12.0):
    """Pair reference bands with measured peaks. shift_of(pixel) = current calibration.
    Returns [(pixel, reference_shift, strength, height)]."""
    out, used = [], set()
    for ref, strength, _tol in std_lib.bands(standard):
        best = None
        for k, (px, h) in enumerate(peaks):
            if k in used:
                continue
            d = abs(shift_of(px) - ref)
            if d <= window and (best is None or (d, -h) < (best[0], -best[2])):
                best = (d, k, h)
        if best:
            used.add(best[1])
            out.append((peaks[best[1]][0], ref, strength, best[2]))
    return out


def fit(matches, deg=None):
    """matches from one or more standards -> shift polynomial + per-band residuals."""
    if len(matches) < 2:
        raise ValueError("need at least 2 matched bands")
    px = [m[0] for m in matches]
    deg = deg if deg is not None else min(3, len(matches) - 2 if len(matches) > 3 else 1)
    p = polyfit(px, [m[1] for m in matches], deg, [float(m[2]) for m in matches])
    res = [(m[1], polyval(p, m[0]) - m[1], m[0]) for m in matches]
    rms = (sum(r[1] ** 2 for r in res) / len(res)) ** 0.5
    return p, res, rms


def calibrate(samples, shift_of, deg=None, window=12.0):
    """samples: [(intensities in calibration-pixel order, standard name)]. Returns a result dict."""
    all_m, per = [], []
    for y, name in samples:
        m = match(find_peaks(y), shift_of, name, window)
        per.append((name, m))
        all_m += m
    p, res, rms = fit(all_m, deg)
    return {"poly": p, "residuals": res, "rms": rms, "matches": per,
            "standards": [n for _, n in samples], "date": time.strftime("%Y-%m-%d %H:%M"),
            "degree": len(p["coeff"]) - 1}


def report(result, before=None):
    lines = [f"Calibration with {', '.join(result['standards'])}: {len(result['residuals'])} bands, "
             f"polynomial degree {result['degree']}, RMS {result['rms']:.2f} cm-1"]
    for name, m in result["matches"]:
        lines.append(f"  {name}: {len(m)} of {len(std_lib.bands(name))} reference bands matched")
    lines.append("  reference   residual   pixel" + ("   before" if before else ""))
    for ref, r, px in sorted(result["residuals"]):
        extra = f"   {before(px) - ref:+6.2f}" if before else ""
        lines.append(f"  {ref:9.1f}   {r:+7.2f}   {px:7.1f}{extra}")
    return "\n".join(lines)


# ----------------------------------------------------------------------------- storage
def config_dir():
    base = os.environ.get("XDG_CONFIG_HOME") or os.path.expanduser("~/.config")
    return os.path.join(base, "cora")


def save(result, channel, serial, path=None):
    path = path or os.path.join(config_dir(), f"calibration_{serial}_{channel}.json")
    os.makedirs(os.path.dirname(path), exist_ok=True)
    data = dict(result)
    data["matches"] = [(n, [list(x) for x in m]) for n, m in result["matches"]]
    data.update(channel=channel, serial=serial)
    with open(path, "w") as f:
        json.dump(data, f, indent=1)
    return path


def load(channel, serial, path=None):
    path = path or os.path.join(config_dir(), f"calibration_{serial}_{channel}.json")
    if not os.path.exists(path):
        return None
    with open(path) as f:
        return json.load(f)
