"""
Low-level access to the parts inside the Cora 5000:

* IOBoard      - Prolific PL2303 serial port: power lines, 532 nm laser line (09), VOL reading.
* ArmStatus    - Microchip MCP2200 (HID): ARM state (GPIO bit 0) and compartment switch (bit 7).
* LaserDriver  - Silicon Labs CP210x serial port: 1064 nm laser driver (current in A).
* Spectrometer - BaySpec USB spectrometers (ba8c:1688): "BS2ACCD" = 532 nm CCD, "BS2BASE" = 1064 nm.

See PROTOCOL.md for how all of this was worked out.
"""

import glob
import os
import select
import struct
import sys
import time

import serial

CORA_HUB = ("0424", "2517")
PROLIFIC = ("067b", "2303")
MCP2200 = ("04d8", "00df")
CP210X = ("10c4", "ea60")
BAYSPEC = (0xBA8C, 0x1688)
LASER_LINE = "09"


class HardwareError(Exception):
    pass


# ----------------------------------------------------------------------------- discovery (Linux)
def _ids(d):
    try:
        return (open(os.path.join(d, "idVendor")).read().strip(),
                open(os.path.join(d, "idProduct")).read().strip())
    except OSError:
        return None


def _usb_chain(sys_dev):
    """IDs of the owning USB device and all its parent hubs, nearest first."""
    out, p = [], os.path.realpath(sys_dev)
    while p not in ("/", ""):
        v = _ids(p)
        if v:
            out.append(v)
        p = os.path.dirname(p)
    return out


def find_tty(vid_pid, inside_cora=True):
    """/dev/tty* of a USB serial chip; with inside_cora, only one behind the Cora's hub."""
    if sys.platform.startswith("linux"):
        for dev in sorted(glob.glob("/dev/ttyUSB*") + glob.glob("/dev/ttyACM*")):
            chain = _usb_chain(f"/sys/class/tty/{os.path.basename(dev)}/device")
            if chain and chain[0] == vid_pid and (not inside_cora or CORA_HUB in chain[1:]):
                return dev
        return None
    c = tty_candidates(vid_pid)
    return c[0] if c else None


def tty_candidates(vid_pid):
    """All serial ports with this USB ID (macOS/Windows: no hub information, so callers may need to probe)."""
    if sys.platform.startswith("linux"):
        d = find_tty(vid_pid, inside_cora=False)
        return [d] if d else []
    from serial.tools import list_ports
    out = []
    for p in list_ports.comports():
        if p.vid is not None and (f"{p.vid:04x}", f"{p.pid:04x}") == vid_pid:
            dev = p.device
            if sys.platform == "darwin" and dev.startswith("/dev/tty."):
                dev = "/dev/cu." + dev[len("/dev/tty."):]      # cu.* doesn't wait for carrier detect
            out.append(dev)
    return sorted(set(out))


class _LinuxHid:
    def __init__(self, path):
        self.fd = os.open(path, os.O_RDWR)

    def transact(self, data, reply=True, timeout=2.0):
        os.write(self.fd, bytes([0]) + bytes(data).ljust(16, b"\0"))
        if not reply:
            return b""
        r, _, _ = select.select([self.fd], [], [], timeout)
        return os.read(self.fd, 64) if r else b""

    def close(self):
        os.close(self.fd)


class _MacHid:
    """IOKit HID for the MCP2200: output reports via IOHIDDeviceSetReport; input reports arrive through a
    callback serviced by a dedicated thread running a CFRunLoop."""

    def __init__(self, vid_pid):
        import ctypes
        import queue
        import threading
        from ctypes import c_bool, c_char_p, c_double, c_int32, c_long, c_uint8, c_uint32, c_void_p
        self.ct = ctypes
        iokit = ctypes.cdll.LoadLibrary("/System/Library/Frameworks/IOKit.framework/IOKit")
        cf = ctypes.cdll.LoadLibrary("/System/Library/Frameworks/CoreFoundation.framework/CoreFoundation")
        sig = {(cf, "CFStringCreateWithCString"): (c_void_p, [c_void_p, c_char_p, c_uint32]),
               (cf, "CFNumberGetValue"): (c_bool, [c_void_p, c_long, c_void_p]),
               (cf, "CFSetGetCount"): (c_long, [c_void_p]), (cf, "CFSetGetValues"): (None, [c_void_p, c_void_p]),
               (cf, "CFRelease"): (None, [c_void_p]), (cf, "CFRetain"): (c_void_p, [c_void_p]),
               (cf, "CFRunLoopGetCurrent"): (c_void_p, []), (cf, "CFRunLoopRunInMode"): (c_int32, [c_void_p, c_double, c_bool]),
               (iokit, "IOHIDManagerCreate"): (c_void_p, [c_void_p, c_uint32]),
               (iokit, "IOHIDManagerSetDeviceMatching"): (None, [c_void_p, c_void_p]),
               (iokit, "IOHIDManagerCopyDevices"): (c_void_p, [c_void_p]),
               (iokit, "IOHIDDeviceGetProperty"): (c_void_p, [c_void_p, c_void_p]),
               (iokit, "IOHIDDeviceOpen"): (c_int32, [c_void_p, c_uint32]),
               (iokit, "IOHIDDeviceClose"): (c_int32, [c_void_p, c_uint32]),
               (iokit, "IOHIDDeviceSetReport"): (c_int32, [c_void_p, c_int32, c_long, c_void_p, c_long]),
               (iokit, "IOHIDDeviceScheduleWithRunLoop"): (None, [c_void_p, c_void_p, c_void_p]),
               (iokit, "IOHIDDeviceRegisterInputReportCallback"): (None, [c_void_p, c_void_p, c_long, c_void_p, c_void_p])}
        for (lib, name), (res, args) in sig.items():
            fn = getattr(lib, name)
            fn.restype, fn.argtypes = res, args
        self.iokit, self.cf = iokit, cf

        def cfstr(t):
            return cf.CFStringCreateWithCString(None, t.encode(), 0x08000100)

        def prop(dev, key):
            k = cfstr(key)
            ref = iokit.IOHIDDeviceGetProperty(dev, k)
            cf.CFRelease(k)
            if not ref:
                return None
            v = ctypes.c_int32()
            cf.CFNumberGetValue(ref, 3, ctypes.byref(v))
            return v.value
        self.mgr = iokit.IOHIDManagerCreate(None, 0)
        iokit.IOHIDManagerSetDeviceMatching(self.mgr, None)
        devset = iokit.IOHIDManagerCopyDevices(self.mgr)
        self.dev = None
        if devset:
            n = cf.CFSetGetCount(devset)
            arr = (c_void_p * n)()
            cf.CFSetGetValues(devset, arr)
            for d in arr:
                if (prop(d, "VendorID"), prop(d, "ProductID")) == (int(vid_pid[0], 16), int(vid_pid[1], 16)):
                    self.dev = cf.CFRetain(d)
                    break
        if not self.dev:
            raise HardwareError("MCP2200 HID device not found")
        if iokit.IOHIDDeviceOpen(self.dev, 0) != 0:
            raise HardwareError("Cannot open the MCP2200 (macOS: allow LibreRamen under System Settings > "
                                "Privacy & Security > Input Monitoring)")
        self.q = queue.Queue()
        self.buf = (c_uint8 * 64)()
        CB = ctypes.CFUNCTYPE(None, c_void_p, c_int32, c_void_p, c_uint32, c_uint32, ctypes.POINTER(c_uint8), c_long)
        self.cb = CB(lambda ctx, res, snd, typ, rid, rep, ln: self.q.put(bytes(rep[:ln])))
        self.running = True
        ready = threading.Event()

        def loop():
            mode = c_void_p.in_dll(cf, "kCFRunLoopDefaultMode")
            iokit.IOHIDDeviceRegisterInputReportCallback(self.dev, self.buf, 64, self.cb, None)
            iokit.IOHIDDeviceScheduleWithRunLoop(self.dev, cf.CFRunLoopGetCurrent(), mode)
            ready.set()
            while self.running:
                cf.CFRunLoopRunInMode(mode, 0.2, False)
        self.thread = threading.Thread(target=loop, daemon=True)
        self.thread.start()
        ready.wait(2)

    def transact(self, data, reply=True, timeout=2.0):
        import queue
        while not self.q.empty():
            self.q.get_nowait()
        rep = (self.ct.c_uint8 * 16)(*bytes(data).ljust(16, b"\0"))
        r = self.iokit.IOHIDDeviceSetReport(self.dev, 1, 0, rep, 16)      # kIOHIDReportTypeOutput
        if r != 0:
            raise HardwareError(f"MCP2200 write failed ({r})")
        if not reply:
            return b""
        try:
            return self.q.get(timeout=timeout)
        except queue.Empty:
            return b""

    def close(self):
        self.running = False
        self.iokit.IOHIDDeviceClose(self.dev, 0)


class _WinHid:
    """hid.dll/setupapi via ctypes (no driver needed). Output reports with WriteFile; a reader thread
    collects input reports (ReadFile blocks; closing the handle ends it)."""

    def __init__(self, vid_pid):
        import ctypes
        import queue
        import threading
        from ctypes import wintypes
        self.ct, self.wt = ctypes, wintypes
        path = self._find(int(vid_pid[0], 16), int(vid_pid[1], 16))
        if not path:
            raise HardwareError("MCP2200 HID device not found")
        k32 = ctypes.WinDLL("kernel32", use_last_error=True)
        k32.CreateFileW.restype = wintypes.HANDLE
        k32.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p,
                                    wintypes.DWORD, wintypes.DWORD, wintypes.HANDLE]
        k32.ReadFile.argtypes = [wintypes.HANDLE, ctypes.c_void_p, wintypes.DWORD, ctypes.POINTER(wintypes.DWORD),
                                 ctypes.c_void_p]
        k32.WriteFile.argtypes = [wintypes.HANDLE, ctypes.c_void_p, wintypes.DWORD, ctypes.POINTER(wintypes.DWORD),
                                  ctypes.c_void_p]
        k32.CloseHandle.argtypes = [wintypes.HANDLE]
        self.k32 = k32
        h = k32.CreateFileW(path, 0xC0000000, 3, None, 3, 0, None)          # read/write, shared, existing
        if h in (None, wintypes.HANDLE(-1).value):
            raise HardwareError(f"cannot open the MCP2200 (error {ctypes.get_last_error()})")
        self.h = h
        self.in_len, self.out_len = self._caps()
        self.q = queue.Queue()
        self.running = True

        def reader():
            buf = ctypes.create_string_buffer(self.in_len)
            n = wintypes.DWORD()
            while self.running:
                if not k32.ReadFile(self.h, buf, self.in_len, ctypes.byref(n), None):
                    break
                self.q.put(buf.raw[1:n.value])        # strip the report-ID byte
        self.thread = threading.Thread(target=reader, daemon=True)
        self.thread.start()

    def _caps(self):
        ct = self.ct
        hid = ct.WinDLL("hid")
        pp = ct.c_void_p()
        hid.HidD_GetPreparsedData.argtypes = [ct.c_void_p, ct.POINTER(ct.c_void_p)]
        if not hid.HidD_GetPreparsedData(self.h, ct.byref(pp)):
            return 17, 17
        caps = (ct.c_ushort * 64)()
        hid.HidP_GetCaps.argtypes = [ct.c_void_p, ct.c_void_p]
        hid.HidP_GetCaps(pp, caps)
        hid.HidD_FreePreparsedData.argtypes = [ct.c_void_p]
        hid.HidD_FreePreparsedData(pp)
        return caps[2] or 17, caps[3] or 17       # InputReportByteLength, OutputReportByteLength

    @staticmethod
    def _find(vid, pid):
        import ctypes
        from ctypes import wintypes

        class GUID(ctypes.Structure):
            _fields_ = [("d1", wintypes.DWORD), ("d2", wintypes.WORD), ("d3", wintypes.WORD), ("d4", ctypes.c_ubyte * 8)]

        class IFD(ctypes.Structure):
            _fields_ = [("cbSize", wintypes.DWORD), ("guid", GUID), ("flags", wintypes.DWORD), ("reserved", ctypes.c_void_p)]

        class ATTR(ctypes.Structure):
            _fields_ = [("Size", wintypes.ULONG), ("VendorID", wintypes.USHORT), ("ProductID", wintypes.USHORT),
                        ("VersionNumber", wintypes.USHORT)]
        hid, sa, k32 = ctypes.WinDLL("hid"), ctypes.WinDLL("setupapi"), ctypes.WinDLL("kernel32")
        sa.SetupDiGetClassDevsW.restype = ctypes.c_void_p
        sa.SetupDiGetClassDevsW.argtypes = [ctypes.POINTER(GUID), wintypes.LPCWSTR, wintypes.HWND, wintypes.DWORD]
        sa.SetupDiEnumDeviceInterfaces.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.POINTER(GUID), wintypes.DWORD,
                                                   ctypes.POINTER(IFD)]
        sa.SetupDiGetDeviceInterfaceDetailW.argtypes = [ctypes.c_void_p, ctypes.POINTER(IFD), ctypes.c_void_p,
                                                        wintypes.DWORD, ctypes.POINTER(wintypes.DWORD), ctypes.c_void_p]
        sa.SetupDiDestroyDeviceInfoList.argtypes = [ctypes.c_void_p]
        k32.CreateFileW.restype = wintypes.HANDLE
        k32.CreateFileW.argtypes = [wintypes.LPCWSTR, wintypes.DWORD, wintypes.DWORD, ctypes.c_void_p, wintypes.DWORD,
                                    wintypes.DWORD, wintypes.HANDLE]
        k32.CloseHandle.argtypes = [wintypes.HANDLE]
        hid.HidD_GetAttributes.argtypes = [wintypes.HANDLE, ctypes.POINTER(ATTR)]
        guid = GUID()
        hid.HidD_GetHidGuid(ctypes.byref(guid))
        info = sa.SetupDiGetClassDevsW(ctypes.byref(guid), None, None, 0x12)
        found, idx = None, 0
        while found is None:
            ifd = IFD()
            ifd.cbSize = ctypes.sizeof(IFD)
            if not sa.SetupDiEnumDeviceInterfaces(info, None, ctypes.byref(guid), idx, ctypes.byref(ifd)):
                break
            idx += 1
            need = wintypes.DWORD()
            sa.SetupDiGetDeviceInterfaceDetailW(info, ctypes.byref(ifd), None, 0, ctypes.byref(need), None)
            buf = ctypes.create_string_buffer(need.value + 4)
            ctypes.c_uint32.from_buffer(buf).value = 8 if ctypes.sizeof(ctypes.c_void_p) == 8 else 6
            if not sa.SetupDiGetDeviceInterfaceDetailW(info, ctypes.byref(ifd), buf, need, None, None):
                continue
            path = ctypes.wstring_at(ctypes.addressof(buf) + 4)
            h = k32.CreateFileW(path, 0, 3, None, 3, 0, None)
            if h in (None, wintypes.HANDLE(-1).value):
                continue
            a = ATTR()
            a.Size = ctypes.sizeof(ATTR)
            if hid.HidD_GetAttributes(h, ctypes.byref(a)) and (a.VendorID, a.ProductID) == (vid, pid):
                found = path
            k32.CloseHandle(h)
        sa.SetupDiDestroyDeviceInfoList(info)
        return found

    def transact(self, data, reply=True, timeout=2.0):
        import queue
        while not self.q.empty():
            self.q.get_nowait()
        buf = self.ct.create_string_buffer(bytes([0]) + bytes(data).ljust(self.out_len - 1, b"\0"), self.out_len)
        n = self.wt.DWORD()
        if not self.k32.WriteFile(self.h, buf, self.out_len, self.ct.byref(n), None):
            raise HardwareError(f"MCP2200 write failed (error {self.ct.get_last_error()})")
        if not reply:
            return b""
        try:
            return self.q.get(timeout=timeout)
        except queue.Empty:
            return b""

    def close(self):
        self.running = False
        self.k32.CloseHandle(self.h)


def open_mcp2200():
    """Platform HID transport for the MCP2200, or None if not found."""
    if sys.platform.startswith("linux"):
        p = find_hidraw(MCP2200)
        return _LinuxHid(p) if p else None
    if sys.platform == "darwin":
        try:
            return _MacHid(MCP2200)
        except HardwareError as e:
            if "not found" in str(e):
                return None
            raise
    if sys.platform == "win32":
        try:
            return _WinHid(MCP2200)
        except HardwareError as e:
            if "not found" in str(e):
                return None
            raise
    raise HardwareError("MCP2200 access on this system is not implemented yet")


def find_hidraw(vid_pid):
    for dev in sorted(glob.glob("/dev/hidraw*")):
        chain = _usb_chain(f"/sys/class/hidraw/{os.path.basename(dev)}/device")
        if chain and chain[0] == vid_pid:
            return dev
    return None


# ----------------------------------------------------------------------------- I/O board
class IOBoard:
    """ASCII commands, LF-terminated, answered with 'OK'. 115200 8N1, DTR/RTS off.
    The vendor software sends one character at a time ~80 ms apart; we keep a (shorter) pacing."""

    def __init__(self, port, pacing=0.03):
        self.s = serial.Serial(port, 115200, timeout=0.1)
        self.s.dtr = False
        self.s.rts = False
        self.pacing = pacing
        time.sleep(0.1)
        self.s.reset_input_buffer()
        self.lines = {}      # last state we set: line -> "H"/"L"

    def command(self, text, timeout=3.0):
        for ch in (text + "\n").encode():
            self.s.write(bytes([ch]))
            self.s.flush()
            time.sleep(self.pacing)
        buf, t_end = b"", time.monotonic() + timeout
        while time.monotonic() < t_end and not buf.endswith(b"OK\n"):
            buf += self.s.read(64)
        reply = buf.decode("ascii", "replace").strip()
        if not reply.endswith("OK"):
            raise HardwareError(f"I/O board did not confirm {text!r} (got {reply!r})")
        return reply[:-2].strip()

    def set_line(self, line, high, _allow_laser=False):
        if line == LASER_LINE and not _allow_laser:
            raise HardwareError("the laser line is switched only through Cora.laser_532()")
        self.command(f"IO {'H' if high else 'L'} {line}")
        self.lines[line] = "H" if high else "L"

    def vol(self):
        r = self.command("VOL 03")
        return float(r.split(":")[1]) if ":" in r else None

    def close(self):
        self.s.close()


# ----------------------------------------------------------------------------- ARM status (MCP2200)
class ArmStatus:
    """MCP2200 over Linux hidraw. READ_ALL (0x80) -> byte 10 = GPIO port.
    bit 0 = 0: armed (ARM button pressed, cleared when the compartment opens); bit 7 = 0: compartment open."""

    def __init__(self, transport):
        import threading
        self.t = transport
        self.lock = threading.Lock()      # status is polled from the GUI while a measurement runs

    def _cmd(self, data, reply=True):
        with self.lock:
            return self.t.transact(data, reply)

    def gpio(self):
        for _ in range(3):
            rep = self._cmd([0x80])
            if len(rep) > 10 and rep[0] == 0x80:
                return rep[10]
        raise HardwareError("no status from the MCP2200")

    def state(self):
        g = self.gpio()
        return {"armed": not (g & 0x01), "compartment_open": not (g & 0x80), "gpio": g}

    def pulse(self):
        """Outputs low then high, as the vendor software does around measurements (purpose unknown)."""
        self._cmd([0x08] + [0] * 10 + [0x00, 0xFF], reply=False)
        time.sleep(0.1)
        self._cmd([0x08] + [0] * 10 + [0xFF, 0x00], reply=False)

    def close(self):
        self.t.close()


# ----------------------------------------------------------------------------- 1064 nm laser driver
class LaserDriver:
    """CR-terminated ASCII at 19200 8N1. Commands seen: rev?, tecmax, temperature, laseron, laseroff, current."""

    def __init__(self, port):
        self.s = serial.Serial(port, 19200, timeout=0.3)
        self.s.dtr = False
        self.s.rts = False
        time.sleep(0.2)
        self.s.reset_input_buffer()

    def command(self, text):
        self.s.write((text + "\r").encode())
        self.s.flush()
        time.sleep(0.12)
        return self.s.read(200).decode("ascii", "replace").strip()

    def init(self):
        self.command("")
        rev = self.command("rev?")
        for c in ("tecmax 0.6", "temperature 25.00", "laseroff", "current 0.00"):
            self.command(c)
        return rev

    def set_current(self, amps):
        self.command(f"current {amps:.2f}")

    def on(self):
        self.command("laseron")

    def off(self):
        self.command("current 0.00")
        self.command("laseroff")

    def close(self):
        self.s.close()


# ----------------------------------------------------------------------------- spectrometers
class Spectrometer:
    """BaySpec USB spectrometer. Vendor requests (bmRequestType 0x40 out / 0xC0 in):
    A1 read EEPROM, A3 init, A4 flag, AD (1064 only), AE exposure, AA start/stop; frame on bulk 0x82."""

    PRODUCTS = {532: "BS2ACCD", 1064: "BS2BASE"}
    FRAME_BYTES = {532: 4608, 1064: 1024}
    ACTIVE = {532: (10, 2058), 1064: (0, 256)}          # data index range with real pixels

    def __init__(self, dev, channel):
        self.dev = dev
        self.channel = channel
        try:
            dev.get_active_configuration()      # never re-configure: that drops the next frame's first packet
        except Exception:  # noqa: BLE001
            dev.set_configuration()
        self.exposure_s = None

    @classmethod
    def find(cls, channel, timeout=0.0):
        import usb.core
        import usb.util
        backend = None
        try:
            import libusb_package               # bundled libusb (macOS/Windows wheels)
            backend = libusb_package.get_libusb1_backend()
        except ImportError:
            pass
        t_end = time.monotonic() + timeout
        while True:
            for d in usb.core.find(find_all=True, idVendor=BAYSPEC[0], idProduct=BAYSPEC[1], backend=backend):
                try:
                    if usb.util.get_string(d, d.iProduct) == cls.PRODUCTS[channel]:
                        return cls(d, channel)
                except Exception:  # noqa: BLE001
                    continue
            if time.monotonic() >= t_end:
                return None
            time.sleep(0.25)

    def _out(self, req, value=0, index=0, data=b""):
        self.dev.ctrl_transfer(0x40, req, value, index, data, timeout=1000)

    def _in(self, req, index, length):
        return bytes(self.dev.ctrl_transfer(0xC0, req, 0, index, length, timeout=1000))

    def eeprom(self):
        return b"".join(self._in(0xA1, off, 64) for off in (0x00, 0x40, 0x80, 0xC0))

    def serial(self):
        return self.eeprom()[8:16].decode("ascii", "replace")

    def init(self):
        self._out(0xA3)
        if self.channel == 532:
            self._out(0xA4, 0, 0x03F0)
        else:
            self._out(0xA4, 1, 0x03F0)
            self._out(0xAD, 0, 0x38)
            self._out(0xA4, 1, 0x03F0)

    def set_exposure(self, seconds):
        if self.channel == 532:
            raw, p2 = max(0, round(seconds / 16e-6) - 1), max(1, min(200, round(20 / seconds)))
        else:
            raw, p2 = max(0, round(seconds / 0.8e-6) - 25), 1
        self._out(0xAE, 8, 0x30, struct.pack("<II", raw, p2))
        self.exposure_s = seconds

    def frame(self):
        """One exposure; returns the active pixels in data order (raw counts)."""
        self._out(0xAA, 0xFFFF, 0xFFFF)
        try:
            data = bytes(self.dev.read(0x82, self.FRAME_BYTES[self.channel],
                                       timeout=int(self.exposure_s * 1000) + 5000))
        finally:
            self._out(0xAA, 0, 0)
        px = struct.unpack(f">{len(data) // 2}H", data)
        a, b = self.ACTIVE[self.channel]
        if len(px) < b:
            raise HardwareError(f"short frame: {len(data)} bytes")
        return list(px[a:b])

    def pixel_numbers(self):
        """Calibration pixel p for each returned value. 532: reversed (p = 2057 - i, verified with water).
        1064: assumed p = i (orientation not yet verified)."""
        a, b = self.ACTIVE[self.channel]
        if self.channel == 532:
            return [2057 - i for i in range(a, b)]
        return list(range(a, b))
