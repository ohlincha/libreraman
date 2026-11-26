"""
Reading and writing spectra.

read(path) -> list of DataSpectrum. Recognised:
* LibreRaman CSV (single spectrum: raman_shift_cm1,intensity,signal,background; series: raman_shift_cm1,t=…)
* Vendor (Cora/BaySpec) CSV and TXT ("Key:,value" header, then shift and intensity columns)
* Galactic SPC (single or multi-file, evenly spaced or with an x array)
* JCAMP-DX (XYDATA=(X++(Y..Y)) and XYPOINTS)
* Any text file with two numeric columns
write_csv(), write_jcamp() for one spectrum; write_table() for several on a common axis.
"""

import csv
import io
import os
import re
import struct
from dataclasses import dataclass, field

import numpy as np


@dataclass
class DataSpectrum:
    name: str
    x: np.ndarray                 # Raman shift, cm^-1 (ascending)
    y: np.ndarray                 # intensity (background-subtracted where known)
    meta: dict = field(default_factory=dict)
    source: str = ""
    signal: np.ndarray = None     # raw laser frames (LibreRaman files)
    background: np.ndarray = None

    def sorted(self):
        o = np.argsort(self.x)
        self.x, self.y = self.x[o], self.y[o]
        if self.signal is not None and len(self.signal) == len(o):
            self.signal = self.signal[o]
        if self.background is not None and len(self.background) == len(o):
            self.background = self.background[o]
        return self


def _num(s):
    try:
        return float(s)
    except (TypeError, ValueError):
        return None


def read(path):
    base = os.path.splitext(os.path.basename(path))[0]
    with open(path, "rb") as f:
        head = f.read(4)
    if path.lower().endswith(".spc") or (len(head) > 1 and head[1] in (0x4B, 0x4C, 0x4D) and head[0] < 0x80):
        return _read_spc(path, base)
    text = open(path, encoding="utf-8", errors="replace").read()
    if text.lstrip().startswith("##"):
        return _read_jcamp(text, base, path)
    lines = text.splitlines()
    if lines and lines[0].startswith("# Cora 5000") or any(l.startswith("raman_shift_cm1") for l in lines[:40]):
        return _read_own(lines, base, path)
    if any(re.match(r"^[A-Za-z_]+:\s*[,\t]", l) for l in lines[:5]):
        return _read_vendor(lines, base, path)
    return _read_columns(lines, base, path)


def _read_own(lines, base, path):
    meta = {}
    body = []
    for line in lines:
        if line.startswith("#"):
            k, _, v = line[1:].strip().partition(":")
            if v:
                meta[k.strip()] = v.strip()
        elif line.strip():
            body.append(line)
    rows = list(csv.reader(body))
    hdr = [h.strip() for h in rows[0]]
    data = [r for r in rows[1:] if r and _num(r[0]) is not None]
    xi = hdr.index("raman_shift_cm1") if "raman_shift_cm1" in hdr else 0
    x = np.array([float(r[xi]) for r in data])
    if len(hdr) > 2 and hdr[1].startswith("t="):                  # series table
        out = []
        for j, h in enumerate(hdr[1:], start=1):
            y = np.array([_num(r[j]) or 0.0 for r in data])
            out.append(DataSpectrum(f"{base} {h}", x, y, dict(meta, time=h), path).sorted())
        return out
    col = {h: i for i, h in enumerate(hdr)}

    def column(name):
        if name not in col:
            return None
        vals = [_num(r[col[name]]) for r in data]
        return None if any(v is None for v in vals) else np.array(vals)
    y = column("intensity")
    if y is None:
        y = column("difference")                                   # early test files
    if y is None:
        raise ValueError(f"{os.path.basename(path)}: no intensity column")
    sig = column("signal")
    sig = sig if sig is not None else column("laser")
    sp = DataSpectrum(meta.get("sample", base) or base, x, y, meta, path, sig, column("background"))
    return [sp.sorted()]


def _read_vendor(lines, base, path):
    meta, data, in_data = {}, [], False
    for line in lines:
        parts = [p.strip() for p in re.split(r"[,\t]", line)]
        if not in_data:
            if parts[0].startswith("Spectrum_Raman_Shift") or parts[0].lower().startswith("raman"):
                in_data = True
            elif parts[0].endswith(":"):
                meta[parts[0][:-1]] = " ".join(p for p in parts[1:] if p)
            continue
        if len(parts) >= 2 and _num(parts[0]) is not None and _num(parts[1]) is not None:
            data.append((float(parts[0]), float(parts[1])))
        elif data:
            break
    if not data:
        raise ValueError(f"{os.path.basename(path)}: the file contains no spectrum data")
    a = np.array(data)
    meta["format"] = "vendor text"
    return [DataSpectrum(base, a[:, 0], a[:, 1], meta, path).sorted()]


def _read_columns(lines, base, path):
    data = []
    for line in lines:
        parts = [p for p in re.split(r"[,;\t ]+", line.strip()) if p]
        if len(parts) >= 2 and _num(parts[0]) is not None and _num(parts[1]) is not None:
            data.append((float(parts[0]), float(parts[1])))
    if not data:
        raise ValueError(f"{path}: no numeric columns found")
    a = np.array(data)
    return [DataSpectrum(base, a[:, 0], a[:, 1], {"format": "columns"}, path).sorted()]


def _read_spc(path, base):
    b = open(path, "rb").read()
    flags, ver = b[0], b[1]
    if ver != 0x4B:
        raise ValueError(f"{path}: only the new Galactic SPC format (0x4B) is supported")
    npts, = struct.unpack_from("<I", b, 4)
    first, last = struct.unpack_from("<dd", b, 8)
    nsub, = struct.unpack_from("<I", b, 24)
    fexp = b[3]
    comment = b[88:218].split(b"\0")[0].decode("latin-1", "replace")
    meta = {"format": "SPC", "comment": comment}
    for item in comment.split(","):
        k, _, v = item.partition("=")
        if v:
            meta[k.strip()] = v.strip()
    off = 512
    xs = None
    if flags & 0x80:                                     # TXVALS: one x array for all sub-files
        xs = np.frombuffer(b, "<f4", npts, off).astype(float)
        off += 4 * npts
    out = []
    for k in range(max(1, nsub)):
        sub_exp = b[off + 1]
        off += 32                                        # sub-file header
        e = sub_exp if fexp == 0x80 or sub_exp == 0x80 else fexp
        if e == 0x80:
            y = np.frombuffer(b, "<f4", npts, off).astype(float)
        else:
            y = np.frombuffer(b, "<i4", npts, off).astype(float) * 2.0 ** (e - 32)
        off += 4 * npts
        x = xs if xs is not None else np.linspace(first, last, npts)
        out.append(DataSpectrum(base if nsub <= 1 else f"{base} #{k + 1}", x, y, dict(meta), path).sorted())
    return out


def _read_jcamp(text, base, path):
    meta, x, y = {}, [], []
    mode, firstx, lastx, npts, xf, yf = None, None, None, None, 1.0, 1.0
    for line in text.splitlines():
        if line.startswith("##"):
            k, _, v = line[2:].partition("=")
            k = k.strip().upper()
            v = v.strip()
            meta[k] = v
            if k == "FIRSTX":
                firstx = float(v)
            elif k == "LASTX":
                lastx = float(v)
            elif k == "NPOINTS":
                npts = int(float(v))
            elif k == "XFACTOR":
                xf = float(v)
            elif k == "YFACTOR":
                yf = float(v)
            elif k == "XYDATA":
                mode = "xydata"
            elif k in ("XYPOINTS", "PEAK TABLE"):
                mode = "points"
            elif k == "END":
                mode = None
            continue
        if mode == "xydata":
            vals = [float(t) for t in re.findall(r"[-+]?\d*\.?\d+(?:[eE][-+]?\d+)?", line)]
            if len(vals) > 1:
                y.extend(v * yf for v in vals[1:])
        elif mode == "points":
            for pair in re.findall(r"([-+]?[\d.eE+-]+)\s*,\s*([-+]?[\d.eE+-]+)", line):
                x.append(float(pair[0]) * xf)
                y.append(float(pair[1]) * yf)
    if not x and y:
        n = npts or len(y)
        x = list(np.linspace(firstx, lastx, n))[:len(y)]
    name = meta.get("TITLE", base) or base
    return [DataSpectrum(name, np.array(x), np.array(y), meta, path).sorted()]


# ----------------------------------------------------------------------------- writing
def write_csv(path, sp):
    with open(path, "w", newline="") as f:
        f.write(f"# LibreRaman spectrum: {sp.name}\n")
        for k, v in sp.meta.items():
            f.write(f"# {k}: {v}\n")
        f.write("raman_shift_cm1,intensity\n")
        for a, b in zip(sp.x, sp.y):
            f.write(f"{a:.3f},{b:.4f}\n")


def write_jcamp(path, sp):
    """JCAMP-DX 5.01, XYPOINTS (works for unevenly spaced data, readable everywhere)."""
    with open(path, "w") as f:
        f.write(f"##TITLE={sp.name}\n##JCAMP-DX=5.01\n##DATA TYPE=RAMAN SPECTRUM\n##ORIGIN=LibreRaman\n")
        for k, v in sp.meta.items():
            f.write(f"##${k.upper().replace(' ', '_')}={v}\n")
        f.write(f"##XUNITS=1/CM\n##YUNITS=ARBITRARY UNITS\n##FIRSTX={sp.x[0]:.4f}\n##LASTX={sp.x[-1]:.4f}\n"
                f"##NPOINTS={len(sp.x)}\n##XFACTOR=1\n##YFACTOR=1\n##XYPOINTS=(XY..XY)\n")
        for a, b in zip(sp.x, sp.y):
            f.write(f"{a:.4f}, {b:.4f}\n")
        f.write("##END=\n")


def write_table(path, spectra, step=1.0):
    """Several spectra resampled onto a common axis (overlap range, `step` cm^-1)."""
    lo = max(s.x[0] for s in spectra)
    hi = min(s.x[-1] for s in spectra)
    grid = np.arange(np.ceil(lo), hi, step)
    with open(path, "w", newline="") as f:
        w = csv.writer(f)
        w.writerow(["raman_shift_cm1"] + [s.name for s in spectra])
        cols = [np.interp(grid, s.x, s.y) for s in spectra]
        for i, g in enumerate(grid):
            w.writerow([f"{g:.1f}"] + [f"{c[i]:.4f}" for c in cols])


def from_spectrum(sp, name=None):
    """Convert an acquisition Spectrum (spectrum.Spectrum) to a DataSpectrum."""
    meta = dict(sp.meta, channel=f"{sp.channel} nm", exposure_s=sp.exposure_s, averages=sp.averages)
    d = DataSpectrum(name or sp.meta.get("sample", "spectrum"), np.array(sp.shift), np.array(sp.intensity), meta,
                     sp.meta.get("file", ""),
                     np.array(sp.signal) if sp.signal else None,
                     np.array(sp.background) if sp.background else None)
    return d.sorted()


def _selftest(folder):
    """Read every file under folder; returns (ok, failed list)."""
    ok, bad = 0, []
    for root, _, files in os.walk(folder):
        for fn in files:
            if fn.lower().endswith((".csv", ".txt", ".spc", ".jdx", ".dx")):
                p = os.path.join(root, fn)
                try:
                    s = read(p)
                    assert s and len(s[0].x) > 10
                    ok += 1
                except Exception as e:  # noqa: BLE001
                    bad.append((p, str(e)[:80]))
    return ok, bad


if __name__ == "__main__":   # quick check on a folder of files
    import sys
    n, bad = _selftest(sys.argv[1])
    print(n, "read,", len(bad), "failed")
    for p, e in bad[:20]:
        print(" ", p, e)
    _ = io
