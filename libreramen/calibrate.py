"""
Calibration against reference standards.

1. Find peaks in a background-subtracted spectrum (in calibration-pixel order).
2. Match them to the standard's reference bands, using the current calibration as the starting point.
3. Fit a CORRECTION to the factory calibration through all matches (several standards can be combined):
   delta(p) = reference shift - factory shift(p). Its degree is limited by how much of the detector the
   matched bands cover (a free cubic through five bands on 9 % of the detector extrapolated to 27000 cm-1 at
   the far end), and outside the matched pixel range the correction is only extrapolated linearly.

The result is stored as a plain polynomial shift(p) = factory(p) + delta(p) (fitted over the whole detector),
so readers of the file need nothing new; it replaces the factory calibration for that channel. The result
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


# Degree of the stored polynomial shift(p) = factory(p) + delta(p): the factory calibration (cubic in wavelength)
# is reproduced to < 0.01 cm-1 by degree 6 over the whole 532 nm detector (degree 4: 0.55 cm-1).
STORE_DEG = 6
STORE_STEP = 8          # pixel grid of that fit (plus the last pixel)
DEGREE_NAMES = {0: "offset", 1: "linear", 2: "quadratic", 3: "cubic"}


def auto_degree(pixels, npix=2048):
    """Degree of the correction to the factory calibration, limited by the coverage of the matched bands:
    span < 25 % of the detector -> offset only (even a straight line through bands on 9 % of the detector
    extrapolates by ~20 cm-1 at the far end with +-2 cm-1 scatter); >= 25 %: linear (2-3 bands), quadratic
    (4+ bands); cubic needs 6+ bands spread over >= 60 % (no gap wider than 30 % of their span). A correction on
    top of the factory calibration is smooth: quadratic is nearly always enough."""
    n = len(pixels)
    px = sorted(pixels)
    span = px[-1] - px[0]
    if n < 2 or span < 0.25 * npix:
        return 0
    gap = max(b - a for a, b in zip(px, px[1:]))
    if n >= 6 and span >= 0.6 * npix and gap <= 0.3 * span:
        return 3
    return 2 if n >= 4 else 1


def correction(matches, deg, base):
    """delta(p) = reference - base(p) through the matches: weighted polynomial of degree deg inside the matched
    pixel range; outside it continues linearly from the edge value with the slope of a weighted straight-line
    fit (constant for degree 0), so it never runs away like a polynomial would. Returns delta(p)."""
    px = [m[0] for m in matches]
    d = [m[1] - base(m[0]) for m in matches]
    w = [float(m[2]) for m in matches]
    lo, hi = min(px), max(px)
    c = polyfit(px, d, deg, w)
    slope = 0.0
    if deg >= 1:
        lin = polyfit(px, d, 1, w)
        slope = lin["coeff"][1] / lin["scale"]
    c_lo, c_hi = polyval(c, lo), polyval(c, hi)

    def delta(p):
        if p < lo:
            return c_lo + slope * (p - lo)
        if p > hi:
            return c_hi + slope * (p - hi)
        return polyval(c, p)
    return delta


def fit(matches, deg=None, base=None, npix=2048):
    """matches from one or more standards -> (shift polynomial, per-band residuals, RMS, correction degree).
    With base (the factory calibration) a correction delta(p) = reference - base(p) is fitted (correction()):
    degree deg, or auto_degree() from the coverage; one band = a constant offset (silicon; the same as a
    corrected laser wavelength). shift(p) = base(p) + delta(p) is then stored as a degree-STORE_DEG polynomial
    over the whole detector. Without base: a free polynomial through the bands (no extrapolation guard)."""
    if not matches:
        raise ValueError("no reference band matched")
    px = [m[0] for m in matches]
    distinct = len(set(px))
    if base is None:
        if len(matches) == 1:
            raise ValueError("one-point calibration needs the current calibration")
        deg = deg if deg is not None else max(1, auto_degree(px, npix))
        deg = min(deg, distinct - 1)
        p = polyfit(px, [m[1] for m in matches], deg, [float(m[2]) for m in matches])
    else:
        deg = deg if deg is not None else auto_degree(px, npix)
        deg = max(0, min(deg, distinct - 1))
        delta = correction(matches, deg, base)
        grid = list(range(0, npix, STORE_STEP))
        if grid[-1] != npix - 1:
            grid.append(npix - 1)
        p = polyfit(grid, [base(g) + delta(g) for g in grid], STORE_DEG)
    res = [(m[1], polyval(p, m[0]) - m[1], m[0]) for m in matches]
    rms = (sum(r[1] ** 2 for r in res) / len(res)) ** 0.5
    return p, res, rms, deg


def calibrate(samples, shift_of, deg=None, window=12.0, base=None):
    """samples: [(intensities in calibration-pixel order, standard name)]. shift_of: calibration the peaks are
    matched against; base: calibration the correction is fitted to (default shift_of; normally the factory
    calibration). Returns a result dict ("degree" = degree of the correction)."""
    all_m, per = [], []
    for y, name in samples:
        m = match(find_peaks(y), shift_of, name, window)
        per.append((name, m))
        all_m += m
    p, res, rms, d = fit(all_m, deg, base=base or shift_of, npix=len(samples[0][0]) if samples else 2048)
    return {"poly": p, "residuals": res, "rms": rms, "matches": per,
            "standards": [n for _, n in samples], "date": time.strftime("%Y-%m-%d %H:%M"),
            "degree": d}


def report(result, before=None):
    d = result["degree"]
    kind = DEGREE_NAMES.get(d, "degree %d" % d)
    lines = [f"Calibration with {', '.join(result['standards'])}: {len(result['residuals'])} bands, "
             f"{kind} correction to the factory calibration, RMS {result['rms']:.2f} cm-1"]
    refs = [r[0] for r in result["residuals"]]
    pix = [r[2] for r in result["residuals"]]
    if refs:
        rest = ("the offset applies to the whole detector" if d == 0
                else "outside this range the correction is extrapolated linearly")
        lines.append(f"  bands cover {min(refs):.0f}-{max(refs):.0f} cm-1 (pixels {min(pix):.0f}-{max(pix):.0f}); {rest}")
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
    return os.path.join(base, "libreramen")


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
