#!/usr/bin/env python3
"""
Read-only probe for the Cora 5000 on Linux.

It sends only two queries:
  * "VOL 03" to the I/O board (Prolific, 115200 baud 8N1, DTR/RTS off, characters ~80 ms apart). It's a reading the vendor software
    polls every 2 s; it switches nothing.
  * READ_ALL (0x80) to the MCP2200 over HID (status and configuration read).
It never sends IO H/L commands (power lines or the laser), never arms anything and never
talks to the laser driver.

Usage:  python3 probe.py            (needs pyserial; the HID part needs access to /dev/hidrawN)
"""
import glob
import os
import sys
import time

CORA_HUB = ("0424", "2517")


def sysfs_usb_dev(tty_or_hid):
    """sysfs directory of the USB device that owns /dev/<name>."""
    path = os.path.realpath(f"/sys/class/{tty_or_hid}/device")
    while path != "/" and not os.path.exists(os.path.join(path, "idVendor")):
        path = os.path.dirname(path)
    return path if path != "/" else None


def ids(d):
    try:
        return (open(os.path.join(d, "idVendor")).read().strip(),
                open(os.path.join(d, "idProduct")).read().strip())
    except OSError:
        return None


def behind_cora_hub(d):
    p = os.path.dirname(d)
    while p != "/" and p:
        if ids(p) == CORA_HUB:
            return True
        p = os.path.dirname(p)
    return False


def find():
    found = {"io": None, "mcp_hid": None, "others": []}
    for dev in sorted(glob.glob("/dev/ttyUSB*") + glob.glob("/dev/ttyACM*")):
        name = os.path.basename(dev)
        d = sysfs_usb_dev(f"tty/{name}")
        vp = ids(d) if d else None
        cora = bool(d and behind_cora_hub(d))
        print(f"{dev:14} USB {':'.join(vp) if vp else '?':10} {'(inside the Cora)' if cora else ''}")
        if vp == ("067b", "2303") and cora:
            found["io"] = dev
    for dev in sorted(glob.glob("/dev/hidraw*")):
        name = os.path.basename(dev)
        d = sysfs_usb_dev(f"hidraw/{name}")
        vp = ids(d) if d else None
        if vp == ("04d8", "00df"):
            found["mcp_hid"] = dev
            print(f"{dev:14} USB 04d8:00df (MCP2200 HID){' (inside the Cora)' if behind_cora_hub(d) else ''}")
    return found


def probe_io(dev):
    import serial
    print(f"\nI/O board on {dev}: sending 'VOL 03'")
    with serial.Serial(dev, 115200, bytesize=8, parity="N", stopbits=1, timeout=0.2) as s:
        s.dtr = False
        s.rts = False
        time.sleep(0.2)
        s.reset_input_buffer()
        for ch in b"VOL 03\n":          # the vendor software paces characters ~80 ms apart
            s.write(bytes([ch]))
            s.flush()
            time.sleep(0.08)
        buf = b""
        t_end = time.time() + 3
        while time.time() < t_end and not buf.endswith(b"OK\n"):
            buf += s.read(64)
        print("  reply:", repr(buf.decode("ascii", "replace")) if buf else "(nothing)")
        return bool(buf)


def probe_mcp(dev):
    print(f"\nMCP2200 on {dev}: READ_ALL")
    try:
        fd = os.open(dev, os.O_RDWR)
    except PermissionError:
        print(f"  permission denied. Install the udev rule (70-cora.rules) or run: sudo chmod a+rw {dev}")
        return False
    try:
        os.write(fd, bytes([0x00, 0x80] + [0] * 15))   # report id 0 + 16-byte command
        import select
        r, _, _ = select.select([fd], [], [], 2.0)
        if not r:
            print("  no reply")
            return False
        rep = os.read(fd, 64)
    finally:
        os.close(fd)
    print("  raw:", rep.hex(" "))
    if len(rep) >= 11 and rep[0] == 0x80:
        print(f"  IO direction bitmap 0x{rep[4]:02x}, default outputs 0x{rep[6]:02x}, "
              f"baud divisor {rep[8] << 8 | rep[9]}, GPIO port = {rep[10]:08b} "
              f"(bit 0 = 0 means armed in the vendor software's captures)")
    return True


def main():
    f = find()
    if not f["io"]:
        print("\nI/O board (Prolific behind the Cora hub) not found. Is the Cora on (On button pressed)?")
    else:
        try:
            probe_io(f["io"])
        except Exception as e:  # noqa: BLE001
            print("  error:", e)
    if not f["mcp_hid"]:
        print("\nMCP2200 HID device not found.")
    else:
        probe_mcp(f["mcp_hid"])


if __name__ == "__main__":
    sys.exit(main())
