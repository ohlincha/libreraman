import sys, struct
sys.path.insert(0, ".")
import usbp
path, want = sys.argv[1], sys.argv[2]
gap = float(sys.argv[3]) if len(sys.argv) > 3 else 0.3
pk = usbp.read(path); t0 = pk[0].t
devs = {}
for p in pk:
    if p.xfer == 2 and p.inbound and len(p.data) >= 18 and p.data[1] == 1 and p.data[0] == 18:
        vid, pid = struct.unpack_from("<HH", p.data, 8); devs[p.dev] = f"{vid:04x}:{pid:04x}"
out = []; cur = None; buf = b""; tl = None; ts = None
for p in pk:
    if devs.get(p.dev) != want or p.xfer != 3 or not p.data:
        continue
    d = "RX" if p.inbound else "TX"
    if d != cur or p.t - tl > gap:
        if cur: out.append((ts, cur, buf))
        cur, buf, ts = d, b"", p.t - t0
    buf += p.data; tl = p.t
if cur: out.append((ts, cur, buf))
for ts, d, b in out:
    s = "".join(chr(c) if 32 <= c < 127 else f"<{c:02x}>" for c in b)
    print(f"{ts:8.2f} {d} {s}")
