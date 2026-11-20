"""
Golden data for the C++ port of the numerical modules (processing, peaks, calibrate, standards, fileio).

Run from the repository root with the Python reference package on the path:
    PYTHONPATH=. path/to/python cpp/tests/golden/make_golden.py

Inputs are rounded to a few decimals so that C++ and Python start from identical doubles. Long outputs are
stored subsampled (every STRIDE-th point) to keep the files small. Everything is deterministic (fixed seeds).
"""

import json
import os
import struct

import numpy as np

from libreramen import calibrate, fileio, peaks, processing, standards
from libreramen.calibration import DEFAULT
from libreramen.spectrum import Spectrum

HERE = os.path.dirname(os.path.abspath(__file__))
DATA = os.path.join(HERE, "..", "..", "..", "libreramen", "data", "simulator.npz")
STRIDE = 8


def arr(a, stride=1):
    """Floats for JSON; subsampled outputs are rounded to 12 significant digits (well below the test tolerances)."""
    a = np.asarray(a, float)
    if stride == 1:
        return [float(v) for v in a]
    return [float(f"{v:.12g}") for v in a[::stride]]


def res(a, stride=1):
    """Computed output: rounded to 12 significant digits."""
    return [float(f"{v:.12g}") for v in np.asarray(a, float)[::stride]]


def dump(name, obj):
    with open(os.path.join(HERE, name), "w") as f:
        json.dump(obj, f, separators=(",", ":"))
    print("wrote", name, os.path.getsize(os.path.join(HERE, name)), "bytes")


# ----------------------------------------------------------------------------- inputs
sim = np.load(DATA)
names = [str(n) for n in sim["names"]]
spectra = {n: np.round(s.astype(float), 1) for n, s in zip(names, sim["spectra"])}
ch532 = DEFAULT.channels[532]
X = np.round(np.array([ch532.shift(p) for p in range(2048)]), 3)    # ascending, = pixel order


def synth(seed=7):
    """Synthetic baseline-corrected spectrum: Lorentz/Gauss bands, a shoulder, a doublet and noise."""
    rng = np.random.default_rng(seed)
    x = np.round(np.linspace(200.0, 1800.0, 1001), 4)
    y = (peaks.lorentz(x, 1000, 450, 12) + peaks.gauss(x, 600, 800, 15) + peaks.lorentz(x, 400, 822, 14)
         + peaks.pvoigt(x, 800, 1200, 10, 0.4) + peaks.lorentz(x, 300, 1225, 18) + peaks.gauss(x, 150, 1500, 8)
         + 5 * rng.normal(size=len(x)))
    return x, np.round(y, 3)


SX, SY = synth()
# a band with a shoulder on its flank (pick(shoulders=True))
SHOULDER = np.round(peaks.lorentz(SX, 3000, 1000, 40) + peaks.gauss(SX, 200, 1060, 25)
                    + np.random.default_rng(7).normal(size=len(SX)), 3)


def spiky(seed=3):
    rng = np.random.default_rng(seed)
    x = np.arange(500.0)
    y = 200 + 0.1 * x + peaks.lorentz(x, 800, 150, 10) + peaks.gauss(x, 400, 330, 12) + 6 * rng.normal(size=500)
    for i, h in ((40, 900), (151, 700), (260, 1500), (261, 1200), (420, 600), (499, 800)):
        y[i] += h
    return np.round(y, 2)


# ----------------------------------------------------------------------------- processing
def gold_processing():
    out = {"x": arr(X), "spectra": {n: arr(spectra[n]) for n in ("ethanol", "zinc sulfide", "carbon tetrachloride")},
           "stride": STRIDE}
    base = []
    for n, s in (("ethanol", 5.0), ("zinc sulfide", 2.35), ("carbon tetrachloride", 8.0)):
        for m in processing.BASELINES:
            base.append({"spectrum": n, "method": m, "strength": s,
                         "out": res(processing.baseline(X, spectra[n], m, s), STRIDE)})
    out["baseline"] = base
    # direct calls with explicit parameters on a short signal
    t = np.linspace(0, 1, 300)
    short = np.round(100 * np.exp(-t) + 50 * np.exp(-((t - 0.4) / 0.02) ** 2) + 30 * np.exp(-((t - 0.7) / 0.01) ** 2)
                     + np.random.default_rng(5).normal(size=300), 3)
    sx = np.round(t * 1000, 3)
    out["short"] = arr(short)
    out["short_x"] = arr(sx)
    out["direct"] = {
        "als": res(processing.baseline_als(short, lam=1e4, p=0.05, niter=5)),
        "arpls": res(processing.baseline_arpls(short, lam=1e3, ratio=1e-2, niter=20)),
        "polynomial": res(processing.baseline_polynomial(sx, short, order=3, niter=30)),
        "rolling_ball": res(processing.baseline_rolling_ball(short, radius=11.5)),
        "rolling_ball_big": res(processing.baseline_rolling_ball(short[:40], radius=40)),
        "snip": res(processing.baseline_snip(short, iterations=25)),
    }
    sp = spiky()
    frames = []
    rng = np.random.default_rng(11)
    for k in range(5):
        f = 300 + 20 * np.sin(np.arange(200) / 15) + 10 * rng.normal(size=200)
        f[rng.integers(0, 200, 3)] += 2000
        frames.append(np.round(f, 2))
    out["spiky"] = arr(sp)
    out["despike"] = {"default": res(processing.despike(sp)), "t4": res(processing.despike(sp, 4.0)),
                      "ethanol": res(processing.despike(spectra["ethanol"]), STRIDE)}
    out["frames"] = [arr(f) for f in frames]
    out["despike_frames"] = {"5": res(processing.despike_frames(frames)),
                             "4k3": res(processing.despike_frames(frames[:4], k=3.0)),
                             "2": res(processing.despike_frames(frames[:2]))}
    out["smooth"] = {f"{w}": res(processing.smooth(short, w)) for w in (9, 5, 4, 3, 21)}
    out["smooth_o2_w7"] = res(processing.smooth(short, 7, 2))
    out["normalize"] = {m: res(processing.normalize(sx, short, m, 700.0)) for m in ("none", "max", "area", "band")}
    proc = []
    for n, st in (("ethanol", {"baseline": "snip", "strength": 5.0, "despike": True, "smooth": True,
                               "normalize": "max", "derivative": 0}),
                  ("ethanol", {"baseline": "als", "strength": 4.0, "despike": False, "smooth": False,
                               "normalize": "area"}),
                  ("carbon tetrachloride", {"baseline": "arpls", "strength": 6.0, "smooth": True, "smooth_window": 13,
                                            "normalize": "band", "band": 459.0, "derivative": 1}),
                  ("zinc sulfide", {"baseline": "rolling_ball", "strength": 3.0, "derivative": 2, "deriv_window": 15}),
                  ("zinc sulfide", {})):
        y, b = processing.process(X, spectra[n], st)
        proc.append({"spectrum": n, "settings": st, "out": arr(y, STRIDE), "base": arr(b, STRIDE)})
    out["process"] = proc
    fp = []
    for n in ("ethanol", "carbon tetrachloride", "zinc sulfide"):
        y, _ = processing.process(X, spectra[n], {"baseline": "snip", "strength": 5.0})
        fp.append({"spectrum": n, "prominence": 0.05, "max_peaks": 8, "peaks": processing.find_peaks(X, y)})
        fp.append({"spectrum": n, "prominence": 0.01, "max_peaks": 20, "peaks": processing.find_peaks(X, y, 0.01, 20)})
    fp.append({"spectrum": "negative", "prominence": 0.05, "max_peaks": 8, "peaks": processing.find_peaks(X, -np.abs(X))})
    out["find_peaks"] = fp
    dump("processing.json", out)


# ----------------------------------------------------------------------------- peaks
def peak_dict(p):
    return {"position": p.position, "height": p.height, "fwhm": p.fwhm, "area": p.area, "snr": p.snr,
            "shoulder": p.shoulder}


def fit_dict(r):
    return {"shape": r.shape, "peaks": r.peaks, "baseline": list(r.baseline), "x": arr(r.x), "model": arr(r.model),
            "components": [arr(c) for c in r.components], "r2": r.r2, "rms": r.rms, "table": r.table()}


def gold_peaks():
    from scipy.special import wofz
    out = {"x": arr(SX), "y": arr(SY)}
    out["noise_level"] = [{"y": arr(SY[:50]), "out": peaks.noise_level(SY[:50])},
                          {"y": [1.0, 1.0, 1.0, 1.0], "out": peaks.noise_level([1.0, 1.0, 1.0, 1.0])},
                          {"y": "synth", "out": peaks.noise_level(SY)}]
    out["pick"] = [
        {"args": {}, "peaks": [peak_dict(p) for p in peaks.pick(SX, SY)]},
        {"args": {"snr": 3.0, "min_fwhm": 4.0, "shoulders": True},
         "peaks": [peak_dict(p) for p in peaks.pick(SX, SY, snr=3.0, min_fwhm=4.0, shoulders=True)]},
        {"args": {"snr": 5.0, "shoulders": True, "xmin": 700.0, "xmax": 1300.0},
         "peaks": [peak_dict(p) for p in peaks.pick(SX, SY, shoulders=True, xmin=700.0, xmax=1300.0)]},
        {"args": {"xmin": 1799.0}, "peaks": [peak_dict(p) for p in peaks.pick(SX, SY, xmin=1799.0)]},
        {"args": {"shoulders": True}, "input": "shoulder",
         "peaks": [peak_dict(p) for p in peaks.pick(SX, SHOULDER, shoulders=True)]},
    ]
    out["shoulder"] = arr(SHOULDER)
    sims = []
    for n in ("ethanol", "carbon tetrachloride", "zinc sulfide"):
        y, _ = processing.process(X, spectra[n], {"baseline": "snip", "strength": 5.0, "despike": True, "smooth": True})
        sims.append({"spectrum": n, "peaks": [peak_dict(p) for p in peaks.pick(X, y, snr=8, xmin=150)]})
    out["pick_sim"] = sims
    xs = np.linspace(-30, 30, 61)
    out["profiles"] = {"x": arr(xs),
                       "lorentz": arr(peaks.lorentz(xs, 2.0, 1.5, 7.0)),
                       "gauss": arr(peaks.gauss(xs, 2.0, 1.5, 7.0)),
                       "pvoigt": arr(peaks.pvoigt(xs, 2.0, 1.5, 7.0, 0.3)),
                       "voigt": arr(peaks.voigt(xs, 2.0, 1.5, 5.0, 3.0)),
                       "voigt_narrow_l": arr(peaks.voigt(xs, 1.0, 0.0, 8.0, 1e-4)),
                       "voigt_narrow_g": arr(peaks.voigt(xs, 1.0, 0.0, 0.0, 6.0))}
    rng = np.random.default_rng(1)
    zs = [complex(rng.uniform(-60, 60), 10 ** rng.uniform(-5, 2)) for _ in range(200)]
    out["wofz"] = [[z.real, z.imag, wofz(z).real, wofz(z).imag] for z in zs]
    fits = []
    picked = peaks.pick(SX, SY)
    cases = [("lorentz", (400, 500), None, False), ("gauss", (760, 860), [800.0, 822.0], False),
             ("pvoigt", (760, 860), None, False), ("pvoigt", (1150, 1280), [1200.0, 1225.0], False),
             ("voigt", (1150, 1280), [1200.0, 1225.0], False), ("lorentz", (1150, 1280), "picked", False),
             ("gauss", (1450, 1550), [1500.0], True), ("voigt", (400, 500), None, False)]
    for shape, region, guesses, fixp in cases:
        g = [p for p in picked if region[0] <= p.position <= region[1]] if guesses == "picked" else guesses
        r = peaks.fit(SX, SY, region, g, shape, fixp)
        fits.append({"shape": shape, "region": list(region),
                     "guesses": guesses if guesses != "picked" else [peak_dict(p) for p in g],
                     "fix_positions": fixp, "result": fit_dict(r)})
    out["fit"] = fits
    try:
        peaks.fit(SX, SY, (1650, 1700))
    except ValueError as e:
        out["fit_error"] = str(e)
    dump("peaks.json", out)


# ----------------------------------------------------------------------------- calibrate
def gold_calibrate():
    out = {"spectra": {n: arr(spectra[n]) for n in ("carbon tetrachloride", "chloroform", "ethanol", "zinc sulfide")}}
    y = list(spectra["chloroform"])
    out["smooth"] = {"w": 7, "in": arr(y[:300]), "out": calibrate._smooth(y[:300], 7)}
    out["baseline"] = {"w": 51, "out": calibrate._baseline(y[:300], 51)}
    xp = [3.0, 10.0, 50.5, 77.0, 120.0, 200.0]
    yp = [1.0, 4.0, -2.0, 7.5, 3.0, 11.0]
    p = calibrate.polyfit(xp, yp, 3, [1.0, 2.0, 1.0, 3.0, 1.0, 2.0])
    out["polyfit"] = {"x": xp, "y": yp, "w": [1.0, 2.0, 1.0, 3.0, 1.0, 2.0], "deg": 3, "poly": p,
                      "eval": [[v, calibrate.polyval(p, v)] for v in (0.0, 42.0, 199.0)],
                      "unweighted": calibrate.polyfit(xp, yp, 2)}
    out["shift_of"] = [[px, ch532.shift(px)] for px in (0.0, 100.5, 1023.0, 2047.0)]
    fp = {}
    for n in ("carbon tetrachloride", "chloroform", "ethanol", "zinc sulfide"):
        fp[n] = [list(t) for t in calibrate.find_peaks(list(spectra[n]))]
    out["find_peaks"] = fp
    out["find_peaks_args"] = [list(t) for t in calibrate.find_peaks(list(spectra["ethanol"]), 0.1, 5)]
    m = {}
    for n, std in (("carbon tetrachloride", "ccl4"), ("chloroform", "chloroform"), ("ethanol", "ethanol")):
        m[n] = {"standard": std, "matches": [list(t) for t in calibrate.match(calibrate.find_peaks(list(spectra[n])),
                                                                                ch532.shift, std)]}
    out["match"] = m
    out["match_window"] = [list(t) for t in calibrate.match(calibrate.find_peaks(list(spectra["ethanol"])),
                                                             ch532.shift, "ethanol", 4.0)]
    samples = [(list(spectra["carbon tetrachloride"]), "carbon tetrachloride"), (list(spectra["chloroform"]), "chloroform"),
               (list(spectra["ethanol"]), "ethanol")]
    res = calibrate.calibrate(samples, ch532.shift)
    out["calibrate"] = {"poly": res["poly"], "residuals": [list(r) for r in res["residuals"]], "rms": res["rms"],
                        "matches": [[n, [list(x) for x in mm]] for n, mm in res["matches"]],
                        "standards": res["standards"], "degree": res["degree"],
                        "report": calibrate.report(res), "report_before": calibrate.report(res, ch532.shift)}
    res2 = calibrate.calibrate(samples[:1], ch532.shift, deg=2)
    out["calibrate_deg2"] = {"poly": res2["poly"], "rms": res2["rms"], "report": calibrate.report(res2)}
    # one-point calibration (silicon-like): a single matched band shifts the base calibration
    one = [(1000.3, 520.5, 3, 5000.0)]
    p1, r1, rms1 = calibrate.fit(one, base=ch532.shift)
    out["fit_one"] = {"matches": [list(one[0])], "poly": p1, "residuals": [list(r) for r in r1], "rms": rms1}
    two = [list(t) for t in res["residuals"][:2]]
    p2, r2, rms2 = calibrate.fit([(t[2], t[0], 2, 1.0) for t in two])
    out["fit_two"] = {"matches": [[t[2], t[0], 2, 1.0] for t in two], "poly": p2, "residuals": [list(r) for r in r2],
                      "rms": rms2}
    for args, key in ((([], None), "fit_error_none"), ((one, None), "fit_error_one")):
        try:
            calibrate.fit(args[0], base=args[1])
        except ValueError as e:
            out[key] = str(e)
    try:
        calibrate._solve([[1.0, 2.0], [2.0, 4.0]], [1.0, 2.0])
    except ValueError as e:
        out["solve_error"] = str(e)
    path = calibrate.save(res, 532, "99002169", os.path.join(HERE, "calibration_99002169_532.json"))
    out["saved"] = os.path.basename(path)
    dump("calibrate.json", out)


# ----------------------------------------------------------------------------- standards
def gold_standards():
    out = []
    for k, v in standards.STANDARDS.items():
        out.append({"name": k, "kind": v["kind"], "source": v["source"], "note": v.get("note", ""),
                    "aka": v.get("aka", []), "bands": [list(b) for b in standards.bands(k)]})
    lookups = {q: standards.find(q)[0] for q in ("CCl4", " Teflon ", "si wafer", "cyclohexane", "Toluene-Acetonitrile")}
    try:
        standards.find("unobtainium")
    except KeyError as e:
        err = e.args[0]
    dump("standards.json", {"standards": out, "find": lookups, "error": err})


# ----------------------------------------------------------------------------- fileio
def write_spc(path, x, ys, txvals, fexp, comment):
    """Galactic SPC (new format 0x4B) in the layout fileio._read_spc expects."""
    npts = len(x)
    flags = (0x80 if txvals else 0) | (0x04 if len(ys) > 1 else 0)
    hdr = bytearray(512)
    hdr[0], hdr[1], hdr[2], hdr[3] = flags, 0x4B, 0, fexp
    struct.pack_into("<I", hdr, 4, npts)
    struct.pack_into("<dd", hdr, 8, float(x[0]), float(x[-1]))
    struct.pack_into("<I", hdr, 24, len(ys))
    c = comment.encode("latin-1")[:129]
    hdr[88:88 + len(c)] = c
    body = bytearray()
    if txvals:
        body += np.asarray(x, "<f4").tobytes()
    for k, y in enumerate(ys):
        sub = bytearray(32)
        sub[1] = fexp if fexp != 0x80 else 0x80
        struct.pack_into("<H", sub, 2, k)
        body += sub
        if fexp == 0x80:
            body += np.asarray(y, "<f4").tobytes()
        else:
            body += np.round(np.asarray(y) * 2.0 ** (32 - fexp)).astype("<i4").tobytes()
    with open(path, "wb") as f:
        f.write(bytes(hdr) + bytes(body))


def ds_dict(d):
    return {"name": d.name, "x": arr(d.x), "y": arr(d.y), "meta": [[k, str(v)] for k, v in d.meta.items()],
            "source": os.path.basename(d.source),
            "signal": None if d.signal is None else arr(d.signal),
            "background": None if d.background is None else arr(d.background)}


def gold_fileio():
    d = os.path.join(HERE, "files")
    os.makedirs(d, exist_ok=True)
    for fn in os.listdir(d):
        os.remove(os.path.join(d, fn))
    rng = np.random.default_rng(21)
    n = 60
    shift = np.round(np.linspace(150, 1800, n) + rng.normal(0, 0.2, n), 2)
    inten = np.round(1000 * np.exp(-((shift - 1000) / 40) ** 2) + rng.normal(0, 5, n), 1)
    sig = np.round(inten + 300 + rng.normal(0, 5, n), 1)
    bg = np.round(sig - inten, 1)
    files = {}
    # own CSV (acquisition Spectrum), with and without sample name; Spectrum keeps pixel order -> reversed rows
    sp = Spectrum(532, 2.5, 3, list(shift[::-1]), list(inten[::-1]), list(sig[::-1]), list(bg[::-1]),
                  t=1763659267.0, meta={"sample": "ethanol, abs.", "laser_power_mw": "120", "note": "a: b"})
    sp.save_csv(os.path.join(d, "own.csv"))
    sp2 = Spectrum(1064, 1.0, 1, list(shift), list(inten), t=1763659267.0, meta={})
    sp2.save_csv(os.path.join(d, "own_nosignal.csv"))
    files["from_spectrum"] = ds_dict(fileio.from_spectrum(sp))
    files["from_spectrum_named"] = ds_dict(fileio.from_spectrum(sp2, "named"))
    # early test file with a "difference" column
    with open(os.path.join(d, "early.csv"), "w") as f:
        f.write("# Cora 5000 spectrum\nraman_shift_cm1,difference,laser,background\n")
        for a, b, c, e in zip(shift, inten, sig, bg):
            f.write(f"{a},{b},{c},{e}\n")
    # series table
    with open(os.path.join(d, "series.csv"), "w") as f:
        f.write("# Cora 5000 time series, started 2026-11-20T10:00:00+01:00\n# channel: 532 nm\n# exposure_s: 1.0\n"
                "# averages: 1\n# interval_s: 10\n# background: each\n")
        f.write("raman_shift_cm1,t=0.0s,t=10.2s,t=20.1s\n")
        for i, x in enumerate(shift[:30]):
            f.write(f"{x:.2f},{inten[i]:.1f},{inten[i] * 0.9:.1f},{'' if i == 3 else f'{inten[i] * 0.8:.1f}'}\n")
    # vendor CSV and TXT
    with open(os.path.join(d, "vendor.csv"), "w") as f:
        f.write("Sample_Name:,CCl4 test\nIntegration_Time:,1000,ms\nLaser_Wavelength:,532.1\nDate:,2026/11/20\n"
                "Spectrum_Raman_Shift(cm-1),Intensity\n")
        for a, b in zip(shift[::-1], inten[::-1]):
            f.write(f"{a},{b}\n")
        f.write("\nEnd of data,\n1,2\n")
    with open(os.path.join(d, "vendor.txt"), "w") as f:
        f.write("Device:\tCora5001\nAverages:\t3\nRaman Shift\tCounts\n")
        for a, b in zip(shift, inten):
            f.write(f"{a}\t{b}\n")
    # plain columns
    with open(os.path.join(d, "columns.txt"), "w") as f:
        f.write("some header line\nshift intensity\n")
        for a, b in zip(shift[::-1], inten[::-1]):
            f.write(f"  {a}   {b}\n")
    with open(os.path.join(d, "columns_semicolon.csv"), "w") as f:
        for a, b in zip(shift[:20], inten[:20]):
            f.write(f"{a};{b};9\n")
    # JCAMP-DX: XYPOINTS via write_jcamp, and an XYDATA=(X++(Y..Y)) file
    ds = fileio.DataSpectrum("jcamp test", shift, inten, {"laser": "532 nm", "sample name": "x"})
    fileio.write_jcamp(os.path.join(d, "points.jdx"), ds)
    with open(os.path.join(d, "xydata.jdx"), "w") as f:
        f.write("##TITLE=\n##JCAMP-DX=4.24\n##XUNITS=1/CM\n##FIRSTX=100\n##LASTX=1000\n##NPOINTS=10\n"
                "##XFACTOR=1.0\n##YFACTOR=0.5\n##XYDATA=(X++(Y..Y))\n100 10 20 30 40\n500 50 60 70\n"
                "800 80 90 1.0e2\n##END=\n")
    # SPC: evenly spaced, integer y with exponent; multi-file with x array, float y
    xs = np.linspace(100.0, 3000.0, 50)
    write_spc(os.path.join(d, "even.spc"), xs, [np.round(1000 * np.sin(xs / 300) ** 2, 3)], False, 20,
              "sample=quartz, laser=532 nm,operator=me")
    write_spc(os.path.join(d, "multi.spc"), xs[::-1], [np.round(np.cos(xs / 200) * 100, 2), np.round(xs / 7, 2)], True,
              0x80, "series")
    for fn in sorted(os.listdir(d)):
        files[fn] = [ds_dict(s) for s in fileio.read(os.path.join(d, fn))]
    errors = {}
    with open(os.path.join(d, "bad_own.csv"), "w") as f:
        f.write("# Cora 5000 spectrum\nraman_shift_cm1,foo\n1,2\n")
    with open(os.path.join(d, "bad_columns.txt"), "w") as f:
        f.write("nothing\nnumeric here\n")
    for fn in ("bad_own.csv", "bad_columns.txt"):
        try:
            fileio.read(os.path.join(d, fn))
        except ValueError as e:
            errors[fn] = str(e).replace(os.path.join(d, ""), "")
    files["errors"] = errors
    # writers (the C++ test writes the same data and compares the text)
    w = fileio.DataSpectrum("written, \"quoted\"", shift, inten - 0.00004, {"sample": "x", "exposure s": 1.5})
    fileio.write_csv(os.path.join(d, "written.csv"), w)
    fileio.write_jcamp(os.path.join(d, "written.jdx"), w)
    w2 = fileio.DataSpectrum("second", shift[5:] + 0.3, inten[5:] * 2)
    fileio.write_table(os.path.join(d, "table.csv"), [w, w2], step=7.5)
    files["writers"] = {"x": arr(shift), "y": arr(inten - 0.00004), "x2": arr(shift[5:] + 0.3), "y2": arr(inten[5:] * 2)}
    ok, bad = fileio._selftest(d)
    files["selftest"] = {"ok": ok, "bad": sorted(os.path.basename(p) for p, _ in bad)}
    dump("fileio.json", files)


if __name__ == "__main__":
    gold_processing()
    gold_peaks()
    gold_calibrate()
    gold_standards()
    gold_fileio()
