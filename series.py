#!/usr/bin/env python3
"""
Exposure/timing test (532 nm): laser on once, frames with several exposures back to back, laser off,
then one background per exposure. Requires the instrument to be ARMED already (it won't wait).
Usage: python3 series.py --operator-present  -> series_<time>.csv (one column per frame)
"""
import signal
import struct
import sys
import time

import dark
import laser

PLAN = [0.1, 1, 1, 1, 1, 1, 5]


def main():
    if "--operator-present" not in sys.argv:
        sys.exit("Refusing: run with --operator-present")
    for s in (signal.SIGTERM, signal.SIGHUP):
        signal.signal(s, lambda n, f: (_ for _ in ()).throw(KeyboardInterrupt()))
    io = dark.IOBoard(dark.io_port())
    dev = dark.find_spectrometer(2)[0]
    dark.ctrl_out(dev, 0xA3)
    dark.ctrl_out(dev, 0xA4, 0, 0x03F0)
    mcp = laser.MCP(laser.mcp_hidraw())
    g = mcp.gpio()
    print(f"GPIO {g:08b}")
    if g & 1:
        sys.exit("Not armed: press ARM (laser NOT switched on)")

    def set_exp(t):
        raw = max(0, round(t / 16e-6) - 1)
        dark.ctrl_out(dev, 0xAE, 8, 0x30, struct.pack("<II", raw, max(1, round(20 / t))))

    frames, t_on = [], time.time()
    try:
        print(">>> LASER ON", laser._raw_io(io, "IO L 09"))
        time.sleep(3.6)
        for t in PLAN:
            set_exp(t)
            t0 = time.time()
            px = laser.expose(dev, t)
            dt = time.time() - t0
            frames.append((f"laser_{t}s", px))
            print(f"  laser {t:5.2f} s: took {dt:5.2f} s, mean {sum(px) / len(px):7.0f}, max {max(px)}")
    finally:
        print(f">>> LASER OFF {laser._raw_io(io, 'IO H 09')} (on {time.time() - t_on:.1f} s)")
    for t in sorted(set(PLAN)):
        set_exp(t)
        px = laser.expose(dev, t)
        frames.append((f"bg_{t}s", px))
        print(f"  bg    {t:5.2f} s: mean {sum(px) / len(px):7.0f}")
    fn = time.strftime("series_%Y%m%d_%H%M%S.csv")
    with open(fn, "w") as f:
        f.write("pixel,raman_shift_cm1," + ",".join(n for n, _ in frames) + "\n")
        for i in range(len(frames[0][1])):
            wl = sum(k * (dark.PIXEL0 - i) ** n for n, k in enumerate(dark.WL_COEFF))
            f.write(f"{i},{1e7 / dark.LASER_NM - 1e7 / wl:.2f}," + ",".join(str(p[i]) for _, p in frames) + "\n")
    print("Saved", fn)


if __name__ == "__main__":
    main()
