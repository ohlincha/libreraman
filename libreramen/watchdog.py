"""
Laser watchdog: a separate process that switches the laser off if the main program dies or hangs.

The main program starts it with a pipe on stdin and writes one line per tick ("ping on" / "ping off").
* Pipe closed (main program crashed or was killed): the watchdog opens the I/O board and sends IO H 09
  (and switches the 1064 nm driver off if its port is known), then exits.
* No ping for `hang_s` while the laser was on: the main program is considered hung; the watchdog ends it
  and switches the laser off.
"""

import os
import queue
import sys
import threading
import time


def _laser_off(io_port, driver_port, log):
    from .hardware import IOBoard, LaserDriver
    t_end = time.monotonic() + 15
    while time.monotonic() < t_end:          # the port is free once the dead process has released it
        try:
            io = IOBoard(io_port)
            io.set_line("09", True, _allow_laser=True)
            io.close()
            log("watchdog: laser line 09 switched off")
            break
        except Exception as e:  # noqa: BLE001
            last = e
            time.sleep(0.5)
    else:
        log(f"watchdog: could not switch the laser off: {last}")
    if driver_port:
        try:
            d = LaserDriver(driver_port)
            d.off()
            d.close()
            log("watchdog: 1064 nm driver off")
        except Exception as e:  # noqa: BLE001
            log(f"watchdog: 1064 nm driver: {e}")


def run(io_port, driver_port=None, parent_pid=None, hang_s=20.0):
    log_path = os.path.join(os.path.expanduser("~"), ".libreramen-watchdog.log")

    def log(msg):
        try:
            with open(log_path, "a") as f:
                f.write(time.strftime("%Y-%m-%d %H:%M:%S ") + msg + "\n")
        except OSError:
            pass
    q = queue.Queue()

    def reader():
        for line in sys.stdin:
            q.put(line.strip())
        q.put(None)                           # EOF: parent gone
    threading.Thread(target=reader, daemon=True).start()
    laser_on, last = False, time.monotonic()
    while True:
        try:
            msg = q.get(timeout=1.0)
        except queue.Empty:
            if laser_on and time.monotonic() - last > hang_s:
                log(f"watchdog: no heartbeat for {hang_s:.0f} s with the laser on: ending the program")
                if parent_pid:
                    _kill(parent_pid)
                _laser_off(io_port, driver_port, log)
                return 2
            continue
        if msg is None:
            if laser_on:
                log("watchdog: program ended unexpectedly with the laser on")
            _laser_off(io_port, driver_port, log) if laser_on else None
            return 0
        if msg.startswith("ping"):
            last = time.monotonic()
            laser_on = msg.endswith("on")
        elif msg.startswith("driver "):
            driver_port = msg.split(" ", 1)[1] or None
        elif msg == "quit":
            return 0


def _kill(pid):
    try:
        if sys.platform == "win32":
            import subprocess
            subprocess.run(["taskkill", "/F", "/PID", str(pid)], capture_output=True)
        else:
            import signal
            os.kill(pid, signal.SIGKILL)
    except Exception:  # noqa: BLE001
        pass
    time.sleep(1.0)


def start(io_port):
    """Start the watchdog for this process; returns a Popen whose stdin takes 'ping on/off' lines."""
    import subprocess
    if getattr(sys, "frozen", False):
        cmd = [sys.executable, "watchdog"]
    else:
        cmd = [sys.executable, "-m", "libreramen", "watchdog"]
    cmd += ["--port", io_port, "--parent", str(os.getpid())]
    kw = {}
    if sys.platform == "win32":
        kw["creationflags"] = 0x08000000      # CREATE_NO_WINDOW
    else:
        kw["start_new_session"] = True        # survives the parent's terminal/process group
    return subprocess.Popen(cmd, stdin=subprocess.PIPE, text=True, bufsize=1, **kw)
