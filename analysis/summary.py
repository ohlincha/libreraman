import sys, collections, struct
sys.path.insert(0, ".")
import usbp
for path in sys.argv[1:]:
    pk = usbp.read(path)
    print("=====", path.split("/")[-1], len(pk), "packets", f"{pk[-1].t-pk[0].t:.1f} s")
    # device descriptors
    devs = {}
    for p in pk:
        if p.xfer == 2 and p.inbound and len(p.data) >= 18 and p.data[1] == 1 and p.data[0] == 18:
            vid, pid = struct.unpack_from("<HH", p.data, 8)
            devs[p.dev] = f"{vid:04x}:{pid:04x}"
    print("devices:", devs)
    c = collections.Counter(); b = collections.Counter()
    for p in pk:
        k = (p.dev, devs.get(p.dev, "?"), hex(p.ep), ["iso","int","ctrl","bulk"][p.xfer] if p.xfer < 4 else p.xfer, "IN" if p.inbound else "OUT")
        c[k] += 1; b[k] += len(p.data)
    for k in sorted(c, key=str):
        print("  ", k, c[k], "pkts", b[k], "bytes")
