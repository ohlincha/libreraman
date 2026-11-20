import sys, struct, time
sys.path.insert(0, ".")
import usbp
path, want = sys.argv[1], set(sys.argv[2].split(","))
maxlen = int(sys.argv[3]) if len(sys.argv) > 3 else 64
pk = usbp.read(path)
t0 = pk[0].t
print("start", time.strftime("%Y-%m-%d %H:%M:%S", time.gmtime(t0)), "UTC")
devs = {}
for p in pk:
    if p.xfer == 2 and p.inbound and len(p.data) >= 18 and p.data[1] == 1 and p.data[0] == 18:
        vid, pid = struct.unpack_from("<HH", p.data, 8); devs[p.dev] = f"{vid:04x}:{pid:04x}"
def show(d):
    h = d[:maxlen].hex(" ")
    a = "".join(chr(c) if 32 <= c < 127 else "." for c in d[:maxlen])
    return f"{h}{' …' if len(d) > maxlen else ''}  |{a}|"
pending = {}
for p in pk:
    name = devs.get(p.dev, "?")
    if name not in want and str(p.dev) not in want:
        continue
    t = p.t - t0
    tag = f"{t:8.3f} d{p.dev:<2} {name} ep{p.ep:02x}"
    if p.xfer == 2:
        if p.stage == 0 and not p.inbound:
            bm, br, wv, wi, wl = struct.unpack("<BBHHH", p.setup)
            if bm == 0x80 and br == 6:   # GET_DESCRIPTOR: skip noise
                pending[p.irp] = None; continue
            pending[p.irp] = f"CTRL bm={bm:02x} req={br:02x} wValue={wv:04x} wIndex={wi:04x} wLen={wl}"
            if p.data:
                print(tag, pending[p.irp], "OUT", show(p.data))
                pending[p.irp] = None
        elif p.inbound and p.irp in pending:
            s = pending.pop(p.irp)
            if s:
                print(tag, s, "->", show(p.data) if p.data else f"(status {p.status:x})")
        continue
    if p.xfer in (1, 3):
        if not p.inbound and p.data:
            print(tag, "BULK/INT OUT", len(p.data), show(p.data))
        elif p.inbound and p.data:
            print(tag, "BULK/INT IN ", len(p.data), show(p.data))
