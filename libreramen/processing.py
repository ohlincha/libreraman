"""
Spectral processing. Everything here works on copies; raw data is never modified.

Baseline methods: als, arpls, polynomial (modified polynomial), rolling_ball, snip, none.
"""

import numpy as np
from scipy import sparse
from scipy.ndimage import grey_opening, uniform_filter1d
from scipy.signal import savgol_filter
from scipy.sparse.linalg import spsolve

BASELINES = {
    "none": "No baseline correction",
    "als": "Asymmetric least squares (Eilers)",
    "arpls": "Asymmetrically reweighted penalized least squares",
    "polynomial": "Modified polynomial (iterative)",
    "rolling_ball": "Rolling ball",
    "snip": "SNIP (statistics-sensitive non-linear iterative peak clipping)",
}


def _diff_matrix(n):
    d = sparse.diags([1.0, -2.0, 1.0], [0, 1, 2], shape=(n - 2, n))
    return d.T @ d


def baseline_als(y, lam=1e5, p=0.01, niter=10):
    y = np.asarray(y, float)
    n = len(y)
    D = lam * _diff_matrix(n)
    w = np.ones(n)
    for _ in range(niter):
        W = sparse.diags(w)
        z = spsolve((W + D).tocsc(), w * y)
        w = p * (y > z) + (1 - p) * (y <= z)
    return z


def baseline_arpls(y, lam=1e5, ratio=1e-3, niter=50):
    y = np.asarray(y, float)
    n = len(y)
    D = lam * _diff_matrix(n)
    w = np.ones(n)
    z = y
    for _ in range(niter):
        W = sparse.diags(w)
        z = spsolve((W + D).tocsc(), w * y)
        d = y - z
        dn = d[d < 0]
        if len(dn) < 2:
            break
        m, s = dn.mean(), dn.std() or 1.0
        wt = 1.0 / (1.0 + np.exp(np.clip(2.0 * (d - (2 * s - m)) / s, -50, 50)))
        if np.linalg.norm(w - wt) / (np.linalg.norm(w) or 1) < ratio:
            break
        w = wt
    return z


def baseline_polynomial(x, y, order=5, niter=100):
    x = np.asarray(x, float)
    y = np.asarray(y, float)
    t = (x - x.mean()) / (np.ptp(x) / 2 or 1)
    yy = y.copy()
    for _ in range(niter):
        c = np.polynomial.polynomial.polyfit(t, yy, order)
        z = np.polynomial.polynomial.polyval(t, c)
        new = np.minimum(yy, z)
        if np.allclose(new, yy):
            break
        yy = new
    return z


def baseline_rolling_ball(y, radius=100):
    y = np.asarray(y, float)
    size = max(3, int(2 * radius + 1))
    opened = grey_opening(y, size=size)
    return uniform_filter1d(opened, size=max(3, size // 2))


def baseline_snip(y, iterations=60):
    y = np.asarray(y, float)
    v = np.log(np.log(np.sqrt(np.maximum(y - y.min(), 0) + 1) + 1) + 1)
    n = len(v)
    for k in range(1, iterations + 1):
        a = v[k:n - k]
        b = (v[:n - 2 * k] + v[2 * k:]) / 2
        v[k:n - k] = np.minimum(a, b)
    z = (np.exp(np.exp(v) - 1) - 1) ** 2 - 1 + y.min()
    return z


def baseline(x, y, method="als", strength=5.0, **kw):
    """strength 0-10 maps to the method's main parameter (larger = smoother/stiffer baseline)."""
    if method in (None, "none"):
        return np.zeros_like(np.asarray(y, float))
    s = float(np.clip(strength, 0, 10))
    if method == "als":
        return baseline_als(y, lam=10 ** (2 + s * 0.6), **kw)
    if method == "arpls":
        return baseline_arpls(y, lam=10 ** (2 + s * 0.6), **kw)
    if method == "polynomial":
        return baseline_polynomial(x, y, order=int(round(1 + s)), **kw)
    if method == "rolling_ball":
        return baseline_rolling_ball(y, radius=10 + s * 30)
    if method == "snip":
        return baseline_snip(y, iterations=int(10 + s * 15))
    raise ValueError(f"unknown baseline method {method!r}")


def despike(y, threshold=8.0):
    """Remove cosmic-ray spikes (1-2 pixels wide) from a single spectrum without touching Raman bands.

    A point is a spike if it rises far above a 5-point median (more than `threshold` x noise) AND that
    excess is most of its height above the local background. The tip of a real band (about 10 pixels wide
    here) only exceeds the 5-point median by a few per cent of its height, so it is kept."""
    from scipy.ndimage import median_filter
    y = np.asarray(y, float)
    med = median_filter(y, size=5, mode="nearest")
    res = y - med
    noise = 1.4826 * np.median(np.abs(res - np.median(res))) or 1.0
    base = median_filter(y, size=41, mode="nearest")
    spike = (res > threshold * noise) & (res > 0.5 * (y - base))
    spike = spike | np.roll(spike, 1) & (res > 3 * noise) | np.roll(spike, -1) & (res > 3 * noise)
    out = y.copy()
    out[spike] = med[spike]
    return out


def despike_frames(frames, k=5.0):
    """Multi-scan cosmic-ray removal: per pixel, values far above the median of the scans are replaced."""
    f = np.asarray(frames, float)
    if f.shape[0] < 3:
        return f.mean(axis=0)
    med = np.median(f, axis=0)
    mad = np.median(np.abs(f - med), axis=0) + np.sqrt(np.maximum(med, 1))
    f = np.where(f - med > k * mad, med, f)
    return f.mean(axis=0)


def smooth(y, window=9, order=3):
    window = int(window) | 1
    if window < order + 2:
        return np.asarray(y, float)
    return savgol_filter(np.asarray(y, float), window, order)


def normalize(x, y, mode="none", band=None):
    y = np.asarray(y, float)
    if mode == "max":
        m = y.max()
        return y / m if m else y
    if mode == "area":
        a = np.trapezoid(np.abs(y), x) if hasattr(np, "trapezoid") else np.trapz(np.abs(y), x)
        return y / a if a else y
    if mode == "band" and band is not None:
        i = int(np.argmin(np.abs(np.asarray(x) - band)))
        v = y[max(0, i - 3):i + 4].max()
        return y / v if v else y
    return y


def process(x, y, settings):
    """Apply the processing chain. settings: dict with baseline, strength, despike, smooth, normalize."""
    y = np.asarray(y, float)
    if settings.get("despike"):
        y = despike(y)
    if settings.get("smooth"):
        y = smooth(y, settings.get("smooth_window", 9))
    base = baseline(x, y, settings.get("baseline", "none"), settings.get("strength", 5.0))
    out = y - base
    d = int(settings.get("derivative", 0) or 0)
    if d:
        out = savgol_filter(out, int(settings.get("deriv_window", 11)) | 1, 3, deriv=d)
    out = normalize(x, out, settings.get("normalize", "none"), settings.get("band"))
    return out, base


def find_peaks(x, y, prominence=0.05, max_peaks=8):
    from scipy.signal import find_peaks as fp
    y = np.asarray(y, float)
    if not len(y) or y.max() <= 0:
        return []
    idx, props = fp(y, prominence=prominence * y.max(), distance=8)
    order = np.argsort(props["prominences"])[::-1][:max_peaks]
    return sorted(float(x[i]) for i in idx[order])
