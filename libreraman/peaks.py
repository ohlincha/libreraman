"""
Peak picking and peak fitting (deconvolution of overlapping bands).

pick()  - noise-based detection: peaks above `snr` times the noise, with sub-pixel position, height,
          FWHM, area and S/N; optional shoulder detection from the second derivative.
fit()   - least-squares fit of a sum of profiles (lorentz, gauss, pvoigt, voigt) plus a linear baseline
          in a region, with standard errors from the Jacobian.
"""

from dataclasses import dataclass, field

import numpy as np
from scipy.optimize import least_squares
from scipy.signal import find_peaks, peak_widths, savgol_filter
from scipy.special import voigt_profile


@dataclass
class Peak:
    position: float
    height: float
    fwhm: float
    area: float
    snr: float
    shoulder: bool = False


def noise_level(y):
    """Robust noise estimate (sigma) from the first difference (insensitive to peaks and baseline)."""
    d = np.diff(np.asarray(y, float))
    return 1.4826 * np.median(np.abs(d - np.median(d))) / np.sqrt(2) or 1e-12


def pick(x, y, snr=5.0, min_fwhm=2.0, shoulders=False, xmin=None, xmax=None):
    """Find peaks in a baseline-corrected spectrum. Returns a list of Peak, sorted by position."""
    x = np.asarray(x, float)
    y = np.asarray(y, float)
    m = np.ones_like(x, bool)
    if xmin is not None:
        m &= x >= xmin
    if xmax is not None:
        m &= x <= xmax
    x, y = x[m], y[m]
    if len(y) < 7:
        return []
    sigma = noise_level(y)
    step = abs(np.median(np.diff(x)))
    ys = savgol_filter(y, 7, 3)
    idx, props = find_peaks(ys, prominence=snr * sigma, width=max(1.0, min_fwhm / step / 2))
    w = peak_widths(ys, idx, rel_height=0.5)[0]
    out = []
    for k, i in enumerate(idx):
        pos = _subpixel(x, ys, i)
        fwhm = float(w[k] * step)
        out.append(Peak(pos, float(ys[i]), fwhm, float(ys[i] * fwhm * 1.064), float(props["prominences"][k] / sigma)))
    if shoulders:
        d2 = savgol_filter(y, 11, 3, deriv=2)
        s2 = noise_level(d2)
        cand, _ = find_peaks(-d2, prominence=snr * s2 * 2)
        hw = max(3, int(round(min_fwhm * 2 / step)))
        for i in cand:
            lo_, hi_ = max(0, i - hw), min(len(ys), i + hw + 1)
            on_flank = ys[i] > np.min(ys[lo_:hi_]) + 3 * snr * sigma      # not in a valley between peaks
            monotone = (ys[lo_] - ys[i]) * (ys[i] - ys[hi_ - 1]) > 0       # rising or falling through i
            if (all(abs(x[i] - p.position) > 1.5 * max(p.fwhm, 4 * step) for p in out) and ys[i] > snr * sigma
                    and on_flank and monotone):
                out.append(Peak(float(x[i]), float(ys[i]), float(4 * step), float(ys[i] * 4 * step), float(ys[i] / sigma),
                                shoulder=True))
    return sorted(out, key=lambda p: p.position)


def _subpixel(x, y, i):
    if 0 < i < len(y) - 1:
        a, b, c = y[i - 1], y[i], y[i + 1]
        den = a - 2 * b + c
        off = 0.5 * (a - c) / den if den else 0.0
        off = max(-0.5, min(0.5, off))
        return float(x[i] + off * (x[i + 1] - x[i - 1]) / 2)
    return float(x[i])


# ----------------------------------------------------------------------------- profiles
def lorentz(x, a, x0, w):
    """a = height, w = FWHM."""
    return a / (1 + ((x - x0) / (w / 2)) ** 2)


def gauss(x, a, x0, w):
    return a * np.exp(-4 * np.log(2) * ((x - x0) / w) ** 2)


def pvoigt(x, a, x0, w, eta):
    return eta * lorentz(x, a, x0, w) + (1 - eta) * gauss(x, a, x0, w)


def voigt(x, a, x0, wg, wl):
    sg = max(wg, 1e-9) / (2 * np.sqrt(2 * np.log(2)))
    gl = max(wl, 1e-9) / 2
    v = voigt_profile(x - x0, sg, gl)
    return a * v / voigt_profile(0.0, sg, gl)


PROFILES = {"lorentz": (lorentz, 3), "gauss": (gauss, 3), "pvoigt": (pvoigt, 4), "voigt": (voigt, 4)}


def _area(shape, p):
    if shape == "lorentz":
        return np.pi * p[0] * p[2] / 2
    if shape == "gauss":
        return p[0] * p[2] * np.sqrt(np.pi / (4 * np.log(2)))
    if shape == "pvoigt":
        return p[3] * np.pi * p[0] * p[2] / 2 + (1 - p[3]) * p[0] * p[2] * np.sqrt(np.pi / (4 * np.log(2)))
    return None   # voigt: integrated numerically


@dataclass
class FitResult:
    shape: str
    peaks: list                      # dicts: position, height, fwhm, area (+ _err)
    baseline: tuple                  # (offset, slope) at the region centre
    x: np.ndarray = field(repr=False)
    y: np.ndarray = field(repr=False)
    model: np.ndarray = field(repr=False)
    components: list = field(repr=False)
    r2: float = 0.0
    rms: float = 0.0

    def table(self):
        rows = [f"{'position':>10} {'±':>6} {'height':>10} {'fwhm':>8} {'±':>6} {'area':>12}"]
        for p in self.peaks:
            rows.append(f"{p['position']:10.2f} {p['position_err']:6.2f} {p['height']:10.1f} "
                        f"{p['fwhm']:8.2f} {p['fwhm_err']:6.2f} {p['area']:12.1f}")
        rows.append(f"R² = {self.r2:.5f}, RMS residual = {self.rms:.2f}")
        return "\n".join(rows)


def fit(x, y, region, guesses=None, shape="pvoigt", fix_positions=False):
    """Fit `shape` profiles in region (xmin, xmax). guesses: positions (or Peak list); default: pick()."""
    x = np.asarray(x, float)
    y = np.asarray(y, float)
    m = (x >= region[0]) & (x <= region[1])
    xr, yr = x[m], y[m]
    if guesses is None:          # pick on the whole spectrum (a noise estimate from a zoomed band is far too high)
        guesses = [p for p in pick(x, y, snr=4) if region[0] <= p.position <= region[1]]
    pos = [g.position if isinstance(g, Peak) else float(g) for g in guesses]
    if not pos:
        raise ValueError("no peaks to fit in this region")
    fn, npar = PROFILES[shape]
    xc = xr.mean()
    step = abs(np.median(np.diff(xr)))
    p0, lo, hi = [float(np.min(yr)), 0.0], [-np.inf, -np.inf], [np.inf, np.inf]
    for x0 in pos:
        i = int(np.argmin(np.abs(xr - x0)))
        h = max(float(yr[i] - np.min(yr)), 1e-6)
        w = next((g.fwhm for g in guesses if isinstance(g, Peak) and g.position == x0), 10.0)
        w = max(w, 3 * step)
        tol = 1e-6 if fix_positions else max(3 * w, 10 * step)
        if shape in ("lorentz", "gauss"):
            p0 += [h, x0, w]
            lo += [0, x0 - tol, step]
            hi += [np.inf, x0 + tol, 20 * w + 50]
        elif shape == "pvoigt":
            p0 += [h, x0, w, 0.5]
            lo += [0, x0 - tol, step, 0]
            hi += [np.inf, x0 + tol, 20 * w + 50, 1]
        else:
            p0 += [h, x0, w / 2, w / 2]
            lo += [0, x0 - tol, 1e-3, 1e-3]
            hi += [np.inf, x0 + tol, 20 * w + 50, 20 * w + 50]

    def model(p, xx):
        out = p[0] + p[1] * (xx - xc)
        for k in range(len(pos)):
            out = out + fn(xx, *p[2 + k * npar: 2 + (k + 1) * npar])
        return out

    res = least_squares(lambda p: model(p, xr) - yr, p0, bounds=(lo, hi), x_scale="jac", max_nfev=20000)
    p = res.x
    resid = res.fun
    dof = max(1, len(yr) - len(p))
    s2 = float(resid @ resid) / dof
    try:
        cov = np.linalg.pinv(res.jac.T @ res.jac) * s2
        err = np.sqrt(np.clip(np.diag(cov), 0, None))
    except np.linalg.LinAlgError:
        err = np.full(len(p), np.nan)
    peaks, comps = [], []
    for k in range(len(pos)):
        q = p[2 + k * npar: 2 + (k + 1) * npar]
        e = err[2 + k * npar: 2 + (k + 1) * npar]
        comp = fn(xr, *q)
        comps.append(comp)
        if shape == "voigt":
            fg, fl = q[2], q[3]
            fwhm = 0.5346 * fl + np.sqrt(0.2166 * fl ** 2 + fg ** 2)
            fwhm_err = float(np.hypot(e[2], e[3]))
            dense = np.linspace(q[1] - 50 * fwhm, q[1] + 50 * fwhm, 20001)
            area = float(np.trapezoid(fn(dense, *q), dense) if hasattr(np, "trapezoid") else np.trapz(fn(dense, *q), dense))
        else:
            fwhm, fwhm_err = float(q[2]), float(e[2])
            area = float(_area(shape, q))
        d = {"position": float(q[1]), "position_err": float(e[1]), "height": float(q[0]), "height_err": float(e[0]),
             "fwhm": float(fwhm), "fwhm_err": fwhm_err, "area": area}
        if shape == "pvoigt":
            d["eta"] = float(q[3])
        peaks.append(d)
    mod = model(p, xr)
    ss_res = float(np.sum((yr - mod) ** 2))
    ss_tot = float(np.sum((yr - yr.mean()) ** 2)) or 1.0
    return FitResult(shape, peaks, (float(p[0]), float(p[1])), xr, yr, mod, comps,
                     1 - ss_res / ss_tot, float(np.sqrt(ss_res / len(yr))))
