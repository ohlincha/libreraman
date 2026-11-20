import sys, struct, time
sys.path.insert(0, ".")
import usbp
path = sys.argv[1]
pk = usbp.read(path); t0 = pk[0].t
devs = {}
for p in pk:
    if p.xfer == 2 and p.inbound and len(p.data) >= 18 and p.data[1] == 1 and p.data[0] == 18:
        vid, pid = struct.unpack_from("<HH", p.data, 8); devs[p.dev] = f"{vid:04x}:{pid:04x}"
def lt(t): return time.strftime("%H:%M:%S", time.localtime(t))
ev = []
setups = {}
plbuf = b""; cpbuf = b""
frames = []
for p in pk:
    n = devs.get(p.dev)
    if n == "ba8c:1688":
        if p.xfer == 2 and p.stage == 0 and not p.inbound:
            bm, br, wv, wi, wl = struct.unpack("<BBHHH", p.setup)
            if br == 0xae:
                us_raw = struct.unpack_from("<II", p.data)
                ev.append((p.t, f"BaySpec d{p.dev}: set exposure raw={us_raw[0]} (= {(us_raw[0]+1)*16e-6:.3f} s), param2={us_raw[1]}"))
            elif br == 0xaa:
                ev.append((p.t, f"BaySpec d{p.dev}: {'START' if wv == 0xffff else 'end'} acquisition"))
            elif br in (0xa3, 0xad) or (br == 0xa4 and bm == 0x40):
                ev.append((p.t, f"BaySpec d{p.dev}: req {br:02x} wValue={wv:04x} wIndex={wi:04x}"))
        if p.xfer == 3 and p.inbound and p.data:
            px = struct.unpack(f">{len(p.data)//2}H", p.data)
            frames.append((p.t, px))
            ev.append((p.t, f"BaySpec d{p.dev}: FRAME {len(px)} px, min {min(px)} max {max(px)} mean {sum(px)/len(px):.0f}"))
    elif n == "067b:2303" and p.xfer == 3 and p.data and not p.inbound:
        plbuf += p.data
        if plbuf.endswith(b"\n"):
            s = plbuf.decode(errors="replace").strip(); plbuf = b""
            if s != "VOL 03": ev.append((p.t, f"IO board (Prolific): {s}"))
    elif n == "10c4:ea60" and p.xfer == 3 and p.data and not p.inbound:
        cpbuf += p.data
        if cpbuf.endswith(b"\r"):
            s = cpbuf.decode(errors="replace").strip(); cpbuf = b""
            if s: ev.append((p.t, f"Laser driver (CP210x): {s}"))
    elif n == "04d8:00df" and p.xfer == 1 and p.data and not p.inbound and p.data[0] == 0x08:
        ev.append((p.t, f"MCP2200 GPIO: set={p.data[11]:02x} clear={p.data[12]:02x}"))
last = None
for p in pk:
    if devs.get(p.dev) == "04d8:00df" and p.xfer == 1 and p.inbound and len(p.data) == 16 and p.data[0] == 0x80 and p.data[2] == 1:
        if p.data[10] != last:
            ev.append((p.t, f"MCP2200 inputs = {p.data[10]:08b}")); last = p.data[10]
for t, s in sorted(ev):
    print(lt(t), f"{t-t0:7.1f}", s)
import pickle; pickle.dump(frames, open(sys.argv[2], "wb"))
