"""Minimal USBPcap (linktype 249) pcapng parser."""
import struct, sys, collections
from scapy.utils import RawPcapNgReader

class Pkt:
    __slots__ = ("t","irp","status","func","info","bus","dev","ep","xfer","dlen","stage","setup","data","inbound")

def read(path):
    out = []
    r = RawPcapNgReader(path)
    for raw, meta in r:
        t = meta.tshigh << 32 | meta.tslow
        t = t / (meta.tsresol if hasattr(meta, "tsresol") and meta.tsresol else 1e6)
        hl, irp, st, fn, info, bus, dev, ep, xf, dl = struct.unpack_from("<HQIHBHHBBI", raw, 0)
        p = Pkt(); p.t=t; p.irp=irp; p.status=st; p.func=fn; p.info=info; p.bus=bus; p.dev=dev
        p.ep=ep; p.xfer=xf; p.dlen=dl; p.inbound = bool(info & 1)
        p.stage = raw[27] if xf == 2 and hl >= 28 else None
        p.setup = None
        body = raw[hl:]
        if xf == 2 and p.stage == 0:   # setup stage
            p.setup = body[:8]; body = body[8:]
        p.data = body
        out.append(p)
    return out
