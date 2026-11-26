"""Self-test: start the GUI against the simulator, take one spectrum, quit. Exit code 0 = OK.

Run: python3 -m libreraman selftest   (the build runs this on the packaged app)
"""
import os
import sys

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
from PySide6.QtCore import QTimer  # noqa: E402
from PySide6.QtWidgets import QApplication  # noqa: E402

from libreraman.gui import app as A  # noqa: E402
from libreraman.gui.controller import Controller  # noqa: E402


def main():
    qa = QApplication.instance() or QApplication([])
    ctl = Controller()
    win = A.MainWindow(ctl)
    win.show()
    result = {"spectra": 0, "errors": []}
    ctl.error.connect(lambda e: result["errors"].append(e))
    ctl.spectrum.connect(lambda sp: result.__setitem__("spectra", result["spectra"] + 1))


    def go(info):
        ctl.inst.speed = 20
        ctl.sim("arm")
        win.measure.exposure.setValue(0.2)
        win.measure.averages.setValue(1)
        win.measure.folder.setText(os.path.join(os.path.expanduser("~"), "libreraman-smoke"))
        win.measure.measure_once()


    ctl.connected.connect(lambda i: QTimer.singleShot(200, lambda: go(i)))
    ctl.connect_instrument(True)
    QTimer.singleShot(25000, qa.quit)
    t = QTimer()
    t.timeout.connect(lambda: qa.quit() if result["spectra"] or result["errors"] else None)
    t.start(200)
    qa.exec()
    print(result)
    return 0 if result["spectra"] and not result["errors"] else 1


if __name__ == "__main__":
    sys.exit(main())
