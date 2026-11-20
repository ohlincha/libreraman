"""
LibreRamen main window: status strip, side bar with the main areas, and the Measure dashboard.

Run:  python3 -m libreramen gui [--sim]
"""

import os
import sys
import time

import numpy as np
import pyqtgraph as pg
from PySide6.QtCore import Qt, QTimer
from PySide6.QtGui import QAction, QFont
from PySide6.QtWidgets import (QApplication, QButtonGroup, QCheckBox, QComboBox, QDoubleSpinBox, QFileDialog, QMenu,
                               QFormLayout, QFrame, QGridLayout, QGroupBox, QHBoxLayout, QLabel, QLineEdit,
                               QMainWindow, QMessageBox, QPlainTextEdit, QProgressBar, QPushButton, QRadioButton,
                               QSlider, QSpinBox, QStackedWidget, QToolButton, QVBoxLayout, QWidget)

from .. import __version__, peaks as peakmod, processing
from .controller import Controller
from .splash import Splash

# Calm palette: dark slate tiles with an accent bar; only "laser on" fills a tile (alarm red).
TILE_BG = "#27303b"
COLORS = {"grey": "#7b8594", "green": "#2f9e8f", "yellow": "#c9922b", "red": "#c0392b",
          "orange": "#d4892b", "blue": "#7f93b8"}
ACCENT = {"measure": "#1f6f68", "series": "#34477a", "danger": "#b42318"}
PLOT = {"main": "#1f4e79", "baseline": "#c8892a", "old": (31, 78, 121), "label": "#3a3f47"}


# ----------------------------------------------------------------------------- status strip
class Tile(QFrame):
    def __init__(self, title):
        super().__init__()
        self.setObjectName("tile")
        lay = QVBoxLayout(self)
        lay.setContentsMargins(12, 6, 12, 6)
        lay.setSpacing(0)
        self.head = QLabel(title)
        self.head.setStyleSheet("color: rgba(255,255,255,0.85); font-size: 9pt;")
        self.main = QLabel("—")
        f = QFont()
        f.setPointSize(13)
        f.setBold(True)
        self.main.setFont(f)
        self.sub = QLabel("")
        self.sub.setStyleSheet("color: rgba(255,255,255,0.85); font-size: 9pt;")
        for w in (self.head, self.main, self.sub):
            w.setStyleSheet(w.styleSheet() + "color: white;" if w is self.main else w.styleSheet())
            lay.addWidget(w)
        self.set("grey", "—", "")

    def set(self, color, main, sub="", alarm=False):
        c = COLORS[color]
        if alarm:            # the one state that must be impossible to miss
            self.setStyleSheet(f"#tile {{ background: {c}; border-radius: 6px; }}")
            self.main.setStyleSheet("color: white;")
        else:
            self.setStyleSheet(f"#tile {{ background: {TILE_BG}; border-radius: 6px; border-left: 6px solid {c}; }}")
            self.main.setStyleSheet(f"color: {c};")
        self.main.setText(main)
        self.sub.setText(sub)


class StatusStrip(QWidget):
    def __init__(self, on_stop):
        super().__init__()
        lay = QHBoxLayout(self)
        lay.setContentsMargins(8, 8, 8, 4)
        self.conn = Tile("INSTRUMENT")
        self.laser = Tile("LASER")
        self.comp = Tile("COMPARTMENT")
        self.chan = Tile("CHANNEL")
        for t, s in ((self.conn, 3), (self.laser, 3), (self.comp, 2), (self.chan, 1)):
            lay.addWidget(t, s)
        self.stop = QPushButton("■  STOP")
        self.stop.setMinimumSize(120, 48)
        self.stop.setStyleSheet("QPushButton { background: #b42318; color: white; font-weight: bold; font-size: 14pt;"
                                " border-radius: 6px; } QPushButton:pressed { background: #7f1a15; }")
        self.stop.clicked.connect(on_stop)
        lay.addWidget(self.stop)

    def update_status(self, st, info):
        c = st.get("connection")
        name = f"{info.get('model', 'Cora 5000')} · S/N {info.get('serial', '?')}" if info else ""
        if st.get("simulated") and c == "connected":
            self.conn.set("blue", "● SIMULATOR", "spectra recorded on a real Cora 5000")
        elif c == "connected":
            self.conn.set("green", "● CONNECTED", name)
        elif c == "initialising":
            self.conn.set("yellow", "◐ INITIALISING…", "switching on spectrometers and cooling")
        elif c == "lost":
            self.conn.set("red", "✖ CONNECTION LOST", "check the USB cable", alarm=True)
        elif c == "not found":
            self.conn.set("grey", "○ NOT FOUND", "no instrument")
        else:
            self.conn.set("grey", "○ SEARCHING…", "")
        if c != "connected":
            self.laser.set("grey", "—", "")
            self.comp.set("grey", "—", "")
            self.chan.set("grey", "—", "")
            return
        if st.get("laser_on"):
            self.laser.set("red", "✹ LASER ON", f"{st.get('channel')} nm emitting", alarm=True)
        elif st.get("armed"):
            self.laser.set("orange", "▲ ARMED", "laser can be switched on")
        else:
            self.laser.set("grey", "○ NOT ARMED", "press ARM on the instrument")
        if st.get("compartment_open"):
            self.comp.set("yellow", "▢ OPEN", "laser disabled")
        else:
            self.comp.set("green", "■ CLOSED", "")
        self.chan.set("green", f"{st.get('channel') or '—'} nm", "")


class ElidedLabel(QLabel):
    """Shows a long text (e.g. a path) shortened at the front to fit the current width."""

    def __init__(self, text=""):
        super().__init__()
        self.full = text
        self.setMinimumWidth(40)

    def setFullText(self, text):
        self.full = text
        self.setToolTip(text)
        self._update()

    def resizeEvent(self, ev):
        super().resizeEvent(ev)
        self._update()

    def _update(self):
        super().setText(self.fontMetrics().elidedText(self.full, Qt.ElideLeft, max(20, self.width() - 2)))


# ----------------------------------------------------------------------------- spectrum plot
class SpectrumViewBox(pg.ViewBox):
    """Spectroscopy-style navigation:
    wheel = change the top of the y axis (the bottom/baseline stays put); drag = pan along x;
    wheel over the x axis or Ctrl+wheel = zoom x around the cursor; double-click = full view."""

    def __init__(self):
        super().__init__()
        self.setMouseEnabled(x=True, y=False)
        self.home_x = None          # x range restored by double-click (None: full data range)
        self.auto_y = True          # our own automatic intensity scaling (with headroom for labels)
        self.on_auto = None         # callback to rescale after double-click
        self.on_right_click = None  # callback(x in data units, global screen pos) replaces pyqtgraph's menu
        self.sigXRangeChanged.connect(lambda *_: self.rescale_y())

    def mouseClickEvent(self, ev):
        if ev.button() == Qt.RightButton and self.on_right_click:
            ev.accept()
            self.on_right_click(self.mapSceneToView(ev.scenePos()).x(), ev.screenPos().toPoint())
            return
        super().mouseClickEvent(ev)

    def rescale_y(self, headroom=0.18):
        """Fit the intensity axis to the data visible in the current x range (only in automatic mode)."""
        if not self.auto_y:
            return
        x0, x1 = self.viewRange()[0]
        lo, hi = None, None
        for item in self.addedItems:
            if not isinstance(item, pg.PlotDataItem) or not item.isVisible():
                continue
            xd, yd = item.getData()
            if xd is None or len(xd) == 0:
                continue
            m = (xd >= x0) & (xd <= x1) & np.isfinite(yd)
            if m.any():
                a, b = float(np.min(yd[m])), float(np.max(yd[m]))
                lo = a if lo is None else min(lo, a)
                hi = b if hi is None else max(hi, b)
        if lo is not None:
            self.enableAutoRange(y=False)
            self.setYRange(lo, hi + headroom * (hi - lo or 1), padding=0.02)

    def wheelEvent(self, ev, axis=None):
        mods = ev.modifiers() if hasattr(ev, "modifiers") else Qt.NoModifier
        if axis == 0 or mods & Qt.ControlModifier:
            self.setMouseEnabled(x=True, y=False)
            return super().wheelEvent(ev, axis=0)
        delta = ev.delta() if hasattr(ev, "delta") else ev.angleDelta().y()
        factor = 0.85 ** (delta / 120.0)
        (y0, y1) = self.viewRange()[1]
        self.auto_y = False
        self.enableAutoRange(y=False)
        self.setYRange(y0, y0 + (y1 - y0) * factor, padding=0)
        ev.accept()

    def mouseDoubleClickEvent(self, ev):
        self.auto_y = True
        if self.home_x:
            self.setXRange(*self.home_x, padding=0)
        else:
            self.enableAutoRange(x=True)
        if self.on_auto:
            self.on_auto()
        else:
            self.enableAutoRange(y=True)
        ev.accept()


def spectrum_plot(**kw):
    w = pg.PlotWidget(background="w", viewBox=SpectrumViewBox(), **kw)
    w.setToolTip("<b>Wheel</b>: zoom intensity (baseline stays)<br><b>Drag</b>: pan along the spectrum<br>"
                 "<b>Ctrl+wheel</b> or wheel on the x axis: zoom wavenumbers<br><b>Double-click</b>: full view")
    return w


# ----------------------------------------------------------------------------- Measure page
def form(parent):
    f = QFormLayout(parent)
    f.setLabelAlignment(Qt.AlignLeft | Qt.AlignVCenter)
    f.setFieldGrowthPolicy(QFormLayout.AllNonFixedFieldsGrow)
    f.setRowWrapPolicy(QFormLayout.DontWrapRows)
    f.setHorizontalSpacing(8)
    return f


def box(title):
    g = QGroupBox(title)
    g.setStyleSheet("QGroupBox { font-weight: bold; margin-top: 10px; } "
                    "QGroupBox::title { subcontrol-origin: margin; left: 8px; padding: 0 3px; }")
    return g


class MeasurePage(QWidget):
    def __init__(self, ctl, win):
        super().__init__()
        self.ctl, self.win = ctl, win
        self.history = []              # recent spectra (for faded display, trend)
        self.trend = []                # (elapsed s, value)
        self.series_t0 = None
        grid = QGridLayout(self)
        grid.setContentsMargins(8, 4, 8, 8)

        # live spectrum
        self.plot = spectrum_plot()
        self.plot.setLabel("bottom", "Raman shift (cm⁻¹)")
        self.plot.setLabel("left", "Intensity (counts)")
        self.plot.showGrid(x=True, y=True, alpha=0.2)
        self.plot.setXRange(150, 3500)
        self.plot.getViewBox().home_x = (150, 3500)
        self.plot.getViewBox().on_auto = self.redraw
        self.plot.getViewBox().on_right_click = self.spectrum_menu
        self.plot.setMenuEnabled(False)
        self.last_peaks = []
        self.curve = self.plot.plot(pen=pg.mkPen(PLOT["main"], width=1.6))
        self.base_curve = self.plot.plot(pen=pg.mkPen(PLOT["baseline"], width=1.2, style=Qt.DashLine))
        self.old_curves = [self.plot.plot(pen=pg.mkPen((*PLOT["old"], 35 + 20 * i), width=1)) for i in range(4)]
        self.peak_labels = []
        grid.addWidget(self.plot, 0, 0, 1, 3)

        # series box
        sb = box("Series")
        f = form(sb)
        self.lbl_n = QLabel("—")
        self.lbl_next = QLabel("—")
        self.lbl_elapsed = QLabel("—")
        self.lbl_end = QLabel("—")
        self.bar = QProgressBar()
        for k, w in (("Spectrum", self.lbl_n), ("Next in", self.lbl_next), ("Elapsed", self.lbl_elapsed),
                     ("Ends", self.lbl_end)):
            f.addRow(k, w)
        f.addRow(self.bar)
        grid.addWidget(sb, 1, 0)

        # trend
        tb = box("Trend")
        tl = QVBoxLayout(tb)
        row = QHBoxLayout()
        row.addWidget(QLabel("band at"))
        self.band = QSpinBox()
        self.band.setRange(100, 4000)
        self.band.setValue(1000)
        self.band.setSuffix(" cm⁻¹")
        self.band.setToolTip("<p>Band followed in the trend. Set automatically to the tallest peak of the first "
                             "spectrum; change it here, pick a peak, or right-click in the spectrum.</p>")
        self.band_auto = True
        self.band.valueChanged.connect(self.recompute_trend)
        self.band.editingFinished.connect(lambda: setattr(self, "band_auto", False))
        row.addWidget(self.band)
        self.peak_btn = QToolButton(text="peaks ▾")
        self.peak_btn.setPopupMode(QToolButton.InstantPopup)
        self.peak_menu = QMenu(self.peak_btn)
        self.peak_btn.setMenu(self.peak_menu)
        self.peak_menu.aboutToShow.connect(self._fill_peak_menu)
        row.addWidget(self.peak_btn)
        self.trend_mode = QComboBox()
        self.trend_mode.addItems(["trend", "waterfall"])
        self.trend_mode.currentTextChanged.connect(self.redraw_trend)
        row.addWidget(self.trend_mode)
        row.addStretch()
        tl.addLayout(row)
        self.tplot = spectrum_plot()
        self.tplot.setMinimumHeight(90)
        self.tplot.showGrid(x=True, y=True, alpha=0.2)
        tl.addWidget(self.tplot)
        grid.addWidget(tb, 1, 1, 1, 2)

        # detector + last spectrum
        db = box("Detector")
        f = form(db)
        self.fill = QProgressBar()
        self.fill.setFormat("%p % of maximum")
        self.lbl_dark = QLabel("—")
        self.lbl_sat = QLabel("—")
        f.addRow("Signal", self.fill)
        f.addRow("Dark level", self.lbl_dark)
        f.addRow("Saturation", self.lbl_sat)
        grid.addWidget(db, 2, 0)
        lb = box("Last spectrum")
        f = form(lb)
        self.lbl_time = QLabel("—")
        self.lbl_set = QLabel("—")
        self.lbl_peaks = QLabel("—")
        self.lbl_peaks.setWordWrap(True)
        f.addRow("Time", self.lbl_time)
        f.addRow("Settings", self.lbl_set)
        f.addRow("Peaks (cm⁻¹)", self.lbl_peaks)
        grid.addWidget(lb, 2, 1, 1, 2)

        grid.addWidget(self._settings_panel(), 0, 3, 3, 1)
        grid.setColumnStretch(0, 2)
        grid.setColumnStretch(1, 2)
        grid.setColumnStretch(2, 1)
        grid.setColumnStretch(3, 0)
        grid.setRowStretch(0, 5)
        grid.setRowStretch(1, 2)
        grid.setRowStretch(2, 1)

    # --- settings panel (right)
    def _settings_panel(self):
        outer = QWidget()
        outer.setFixedWidth(380)
        ov = QVBoxLayout(outer)
        ov.setContentsMargins(0, 0, 0, 0)
        big = ("QPushButton {{ background: {c}; color: white; font-size: 15pt; font-weight: bold; border-radius: 8px;"
               " padding: 6px; }} QPushButton:disabled {{ background: #b9bfca; color: #eef0f4; }}"
               " QPushButton:pressed {{ background: {d}; }}")
        self.btn_once = QPushButton("▶  MEASURE")
        self.btn_series = QPushButton("⟳  START SERIES")
        self.btn_stop = QPushButton("■  Stop")
        self.btn_once.setStyleSheet(big.format(c=ACCENT["measure"], d="#15524d"))
        self.btn_series.setStyleSheet(big.format(c=ACCENT["series"], d="#25335a"))
        for b in (self.btn_once, self.btn_series):
            b.setMinimumHeight(54)
        self.btn_stop.setMinimumHeight(30)
        self.btn_once.setToolTip("<p>Take one spectrum with the settings below (laser on only while measuring)</p>")
        self.btn_series.setToolTip("Start the time series set up below")
        self.btn_once.clicked.connect(self.measure_once)
        self.btn_series.clicked.connect(self.start_series)
        self.btn_stop.clicked.connect(self.ctl.stop)
        ov.addWidget(self.btn_once)
        ov.addWidget(self.btn_series)
        ov.addWidget(self.btn_stop)
        self.btn_once.setEnabled(False)
        self.btn_series.setEnabled(False)

        from PySide6.QtWidgets import QScrollArea
        scroll = QScrollArea()
        scroll.setWidgetResizable(True)
        scroll.setFrameShape(QFrame.NoFrame)
        scroll.setHorizontalScrollBarPolicy(Qt.ScrollBarAlwaysOff)
        w = QWidget()
        scroll.setWidget(w)
        ov.addWidget(scroll, 1)
        v = QVBoxLayout(w)
        v.setContentsMargins(0, 0, 8, 0)

        s = box("Sample")
        g = QGridLayout(s)
        g.setHorizontalSpacing(6)
        g.setVerticalSpacing(4)
        self.sample = QLineEdit("sample")
        self.sample.setPlaceholderText("sample name (used in file names)")
        self.folder = QLineEdit(os.path.join(os.path.expanduser("~"), "Documents", "LibreRamen",
                                             time.strftime("%Y-%m-%d")))
        self.folder.setVisible(False)                  # holds the full path
        self.folder_label = ElidedLabel()
        self.folder_label.setStyleSheet("color: #555;")
        self.folder.textChanged.connect(self._show_folder)
        b = QToolButton(text="Change…")
        b.clicked.connect(self.browse)
        g.addWidget(QLabel("Name"), 0, 0)
        g.addWidget(self.sample, 0, 1, 1, 2)
        g.addWidget(QLabel("Saved in"), 1, 0)
        g.addWidget(self.folder_label, 1, 1)
        g.addWidget(b, 1, 2)
        g.setColumnStretch(1, 1)
        v.addWidget(s)

        s = box("Spectrum")
        g = QGridLayout(s)                             # two compact columns
        g.setHorizontalSpacing(6)
        g.setVerticalSpacing(4)
        self.exposure = QDoubleSpinBox(decimals=2, minimum=0.01, maximum=600, value=1.0, suffix=" s")
        self.averages = QSpinBox(minimum=1, maximum=1000, value=3)
        self.averages.setToolTip("Number of scans averaged into one spectrum")
        self.background = QComboBox()
        self.background.addItems(["each spectrum", "once (series)", "none"])
        self.channel = QComboBox()
        self.channel.addItems(["532 nm", "1064 nm"])
        g.addWidget(QLabel("Exposure"), 0, 0)
        g.addWidget(self.exposure, 0, 1)
        g.addWidget(QLabel("Scans"), 0, 2)
        g.addWidget(self.averages, 0, 3)
        g.addWidget(QLabel("Background"), 1, 0)
        g.addWidget(self.background, 1, 1, 1, 3)
        g.addWidget(QLabel("Laser"), 2, 0)
        g.addWidget(self.channel, 2, 1, 1, 3)
        g.setColumnStretch(1, 1)
        g.setColumnStretch(3, 1)
        v.addWidget(s)

        s = box("Time series")
        f = form(s)
        self.series_on = QCheckBox("measure repeatedly")
        self.interval = QDoubleSpinBox(decimals=1, minimum=0.1, maximum=100000, value=5)
        self.interval_unit = QComboBox()
        self.interval_unit.addItems(["s", "min", "h"])
        self.interval_unit.setCurrentText("min")
        row = QHBoxLayout()
        row.addWidget(self.interval)
        row.addWidget(self.interval_unit)
        self.stop_by = QComboBox()
        self.stop_by.addItems(["number of spectra", "duration (h)"])
        self.count = QSpinBox(minimum=1, maximum=100000, value=60)
        self.duration = QDoubleSpinBox(decimals=1, minimum=0.01, maximum=1000, value=5, suffix=" h")
        self.start = QComboBox()
        self.start.addItems(["now", "in 1 min", "in 10 min", "in 1 h"])
        self.fastest = QCheckBox("as fast as possible (laser stays on)")
        self.min_label = QLabel("")
        self.min_label.setStyleSheet("color: #666; font-size: 9pt;")
        f.addRow(self.series_on)
        f.addRow(self.fastest)
        f.addRow("Every", row)
        f.addRow("", self.min_label)
        for wdg in (self.exposure, self.averages, self.interval):
            wdg.valueChanged.connect(self.update_min_interval)
        for wdg in (self.background, self.interval_unit):
            wdg.currentIndexChanged.connect(self.update_min_interval)
        self.fastest.toggled.connect(self.update_min_interval)
        f.addRow("Stop after", self.stop_by)
        f.addRow("Spectra", self.count)
        f.addRow("Duration", self.duration)
        f.addRow("Start", self.start)
        v.addWidget(s)

        s = box("Processing")
        f = form(s)
        self.baseline = QComboBox()
        for k, d in processing.BASELINES.items():
            self.baseline.addItem(d, k)
        self.baseline.setCurrentIndex(list(processing.BASELINES).index("arpls"))
        self.strength = QSlider(Qt.Horizontal, minimum=0, maximum=100, value=50)
        self.despike = QCheckBox("remove cosmic rays")
        self.despike.setChecked(True)
        self.smoothing = QCheckBox("smooth (Savitzky–Golay)")
        self.show_raw = QRadioButton("raw")
        self.show_cor = QRadioButton("corrected")
        self.show_both = QRadioButton("both")
        self.show_both.setChecked(True)
        grp = QButtonGroup(self)
        row = QHBoxLayout()
        for r in (self.show_raw, self.show_cor, self.show_both):
            grp.addButton(r)
            row.addWidget(r)
            r.toggled.connect(self.redraw)
        for wdg in (self.baseline, self.strength, self.despike, self.smoothing):
            sig = getattr(wdg, "currentIndexChanged", None) or getattr(wdg, "valueChanged", None) \
                or getattr(wdg, "toggled")
            sig.connect(self.redraw)
        f.addRow("Baseline", self.baseline)
        f.addRow("Strength", self.strength)
        f.addRow(self.despike)
        f.addRow(self.smoothing)
        f.addRow("Show", row)
        v.addWidget(s)

        v.addStretch()
        for cb in w.findChildren(QComboBox):          # long entries must not force the panel wider
            cb.setSizeAdjustPolicy(QComboBox.AdjustToMinimumContentsLengthWithIcon)
            cb.setMinimumContentsLength(8)
        for le in w.findChildren(QLineEdit):
            le.setMinimumWidth(60)
        return outer

    def min_interval(self):
        inst = self.ctl.inst
        bg = self.background.currentIndex() == 0 and not self.fastest.isChecked()
        settle = 3.6 if inst is None else inst.laser_settle_s()
        frames = self.averages.value() * (2 if bg else 1)
        return (0 if self.fastest.isChecked() else settle) + frames * (self.exposure.value() + 0.02) + 0.5

    def update_min_interval(self):
        mi = self.min_interval()
        mult = {"s": 1, "min": 60, "h": 3600}[self.interval_unit.currentText()]
        self.interval.setEnabled(not self.fastest.isChecked())
        self.interval_unit.setEnabled(not self.fastest.isChecked())
        if self.fastest.isChecked():
            self.min_label.setText(f"one spectrum about every {mi:.1f} s")
            self.min_label.setStyleSheet("color: #666; font-size: 9pt;")
            self.interval.setStyleSheet("")
            return
        too_short = self.interval.value() * mult < mi
        self.min_label.setText(f"minimum ≈ {mi:.1f} s per spectrum" + ("  — interval too short!" if too_short else ""))
        self.min_label.setStyleSheet(f"color: {'#d43c3c' if too_short else '#666'}; font-size: 9pt;")
        self.interval.setStyleSheet("background: #fbe3e3;" if too_short else "")

    def _show_folder(self, path=None):
        path = path or self.folder.text()
        home = os.path.expanduser("~")
        shown = "~" + path[len(home):] if path.startswith(home) else path
        self.folder_label.setFullText(shown)
        self.folder_label.setToolTip(path)

    def resizeEvent(self, ev):
        super().resizeEvent(ev)
        self._show_folder()

    def browse(self):
        d = QFileDialog.getExistingDirectory(self, "Data folder", self.folder.text())
        if d:
            self.folder.setText(d)

    def settings(self):
        mult = {"s": 1, "min": 60, "h": 3600}[self.interval_unit.currentText()]
        delays = {"now": 0, "in 1 min": 60, "in 10 min": 600, "in 1 h": 3600}
        return {"sample": self.sample.text().strip() or "sample", "folder": self.folder.text(),
                "exposure": self.exposure.value(), "averages": self.averages.value(),
                "background": ["each", "once", "none"][self.background.currentIndex()],
                "interval": self.interval.value() * mult,
                "stop_by": "count" if self.stop_by.currentIndex() == 0 else "duration",
                "count": self.count.value(), "duration": self.duration.value() * 3600,
                "start_delay": delays[self.start.currentText()],
                "fastest": self.fastest.isChecked()}

    def proc_settings(self):
        return {"baseline": self.baseline.currentData(), "strength": self.strength.value() / 10,
                "despike": self.despike.isChecked(), "smooth": self.smoothing.isChecked()}

    def measure_once(self):
        s = self.settings()
        if s["background"] == "once":
            s["background"] = "each"
        self.ctl.measure_once(s)

    def start_series(self):
        self.band_auto = True                                  # new series: pick the tallest band again
        s = self.settings()
        mi = self.min_interval()
        if not self.fastest.isChecked() and s["interval"] < mi:
            r = QMessageBox.question(self, "Interval too short",
                                     f"With these settings one spectrum takes at least {mi:.1f} s, so an interval of "
                                     f"{s['interval']:g} s is not possible.\n\nUse {mi:.1f} s instead?")
            if r != QMessageBox.Yes:
                return
            self.interval_unit.setCurrentText("s")
            self.interval.setValue(round(mi + 0.05, 1))
        self.history.clear()
        self.trend.clear()
        self.series_t0 = time.time()
        self.ctl.start_series(self.settings())

    # --- results
    def add_spectrum(self, sp):
        first = not self.history
        self.history.append(sp)
        self.history = self.history[-50:]
        x = np.array(sp.shift)
        y, _ = processing.process(x, np.array(sp.intensity), self.proc_settings())
        self.last_peaks = [p.position for p in peakmod.pick(x, y, snr=8, xmin=150)][:12]
        if first and self.band_auto and self.last_peaks:      # follow the tallest band by default
            tall = max(self.last_peaks, key=lambda p: y[int(np.argmin(np.abs(x - p)))])
            self.set_band(tall, auto=True)
        t = sp.meta.get("series_elapsed_s", (sp.t - (self.series_t0 or sp.t)))
        self.trend.append((t, self._band_value(x, y)))
        raw_max = max(sp.signal) if sp.signal else max(sp.intensity)
        self.fill.setValue(int(100 * raw_max / 65535))
        bg = np.array(sp.background) if sp.background else None
        self.lbl_dark.setText(f"{bg.mean():.0f} counts" if bg is not None and bg.any() else "—")
        sat = raw_max >= 65000
        self.lbl_sat.setText("<b style='color:#d43c3c'>SATURATED: shorten the exposure</b>" if sat
                             else ("close to maximum" if raw_max > 55000 else "none"))
        self.lbl_time.setText(time.strftime("%H:%M:%S", time.localtime(sp.t)))
        self.lbl_set.setText(f"{sp.averages} × {sp.exposure_s:g} s · {sp.channel} nm")
        self.redraw()
        self.redraw_trend()

    def set_band(self, value, auto=False):
        self.band_auto = auto
        self.band.blockSignals(True)
        self.band.setValue(int(round(value)))
        self.band.blockSignals(False)
        self.recompute_trend()

    def _fill_peak_menu(self):
        self.peak_menu.clear()
        if not self.last_peaks:
            self.peak_menu.addAction("(no peaks yet)").setEnabled(False)
        for p in self.last_peaks:
            self.peak_menu.addAction(f"{p:.0f} cm⁻¹", lambda v=p: self.set_band(v))

    def spectrum_menu(self, x, screen_pos):
        m = QMenu(self)
        near = min(self.last_peaks, key=lambda p: abs(p - x)) if self.last_peaks else None
        if near is not None and abs(near - x) < 25:
            m.addAction(f"Use peak {near:.0f} cm⁻¹ for the trend", lambda: self.set_band(near))
        m.addAction(f"Use {x:.0f} cm⁻¹ for the trend", lambda: self.set_band(x))
        m.addSeparator()
        m.addAction("Full view", lambda: self.plot.getViewBox().mouseDoubleClickEvent(_Accept()))
        m.exec(screen_pos)

    def _band_value(self, x, y):
        i = int(np.argmin(np.abs(x - self.band.value())))
        return float(np.max(y[max(0, i - 4):i + 5]))

    def recompute_trend(self):
        if not self.history:
            return
        t0 = self.history[0].t
        self.trend = []
        for sp in self.history:
            x = np.array(sp.shift)
            y, _ = processing.process(x, np.array(sp.intensity), self.proc_settings())
            self.trend.append((sp.meta.get("series_elapsed_s", sp.t - t0), self._band_value(x, y)))
        self.redraw_trend()

    def redraw(self):
        for lab in self.peak_labels:
            self.plot.removeItem(lab)
        self.peak_labels = []
        if not self.history:
            return
        ps = self.proc_settings()
        sp = self.history[-1]
        x = np.array(sp.shift)
        raw = np.array(sp.intensity)
        y, base = processing.process(x, raw, ps)
        mode = "raw" if self.show_raw.isChecked() else ("cor" if self.show_cor.isChecked() else "both")
        if mode == "raw":
            shown = raw
            self.base_curve.setData([], [])
        elif mode == "cor":
            shown = y
            self.base_curve.setData([], [])
        else:
            shown = processing.smooth(processing.despike(raw)) if ps["smooth"] else (
                processing.despike(raw) if ps["despike"] else raw)
            self.base_curve.setData(x, base)
        self.curve.setData(x, shown)
        olds = self.history[-5:-1]
        for c in self.old_curves:
            c.setData([], [])
        for c, o in zip(self.old_curves[-len(olds):] if olds else [], olds):
            yo = np.array(o.intensity) if mode != "cor" else processing.process(np.array(o.shift),
                                                                                np.array(o.intensity), ps)[0]
            c.setData(np.array(o.shift), yo)
        peaks = [p.position for p in peakmod.pick(x, y, snr=8, xmin=150)][:12]
        self.lbl_peaks.setText("  ".join(f"{p:.0f}" for p in peaks) or "—")
        ref = shown
        vb = self.plot.getViewBox()
        if vb.auto_y:                                # automatic y: leave room above the tallest peak for labels
            xr0 = vb.viewRange()[0]
            vis = (x >= xr0[0]) & (x <= xr0[1])
            if vis.any():
                lo_, hi_ = float(np.min(ref[vis])), float(np.max(ref[vis]))
                vb.enableAutoRange(y=False)
                vb.setYRange(lo_, hi_ + 0.18 * (hi_ - lo_ or 1), padding=0.02)
        xr = self.plot.getViewBox().viewRange()[0]
        min_gap = (xr[1] - xr[0]) * 0.035          # labels closer than this (in cm-1) are staggered
        last, level = None, 0
        for p in peaks:
            i = int(np.argmin(np.abs(x - p)))
            level = level + 1 if last is not None and p - last < min_gap else 0
            last = p
            lab = pg.TextItem(f"{p:.0f}", color=PLOT["label"], anchor=(0.5, 1.2 + 1.1 * (level % 3)))
            lab.setPos(p, ref[i])
            self.plot.addItem(lab)
            self.peak_labels.append(lab)

    def redraw_trend(self):
        self.tplot.clear()
        if self.trend_mode.currentText() == "trend":
            span = self.trend[-1][0] if self.trend else 0
            unit, div = ("s", 1) if span < 180 else (("min", 60) if span < 3 * 3600 else ("h", 3600))
            self.tplot.getAxis("bottom").enableAutoSIPrefix(False)
            self.tplot.getAxis("left").enableAutoSIPrefix(False)
            self.tplot.setLabel("bottom", f"time ({unit})")
            self.tplot.setLabel("left", f"band {self.band.value()} cm⁻¹")
            if self.trend:
                t = np.array([a for a, _ in self.trend]) / div
                v = np.array([b for _, b in self.trend])
                self.tplot.plot(t, v, pen=pg.mkPen(PLOT["main"], width=1.5), symbol="o", symbolSize=5,
                                symbolBrush=PLOT["main"])
        else:
            self.tplot.setLabel("bottom", "Raman shift (cm⁻¹)")
            self.tplot.setLabel("left", "spectrum (offset)")
            ps = self.proc_settings()
            hist = self.history[-15:]
            if hist:
                ys = [processing.process(np.array(s.shift), np.array(s.intensity), ps)[0] for s in hist]
                off = max(float(np.percentile(y[np.array(s.shift) > 150], 99.5)) for s, y in zip(hist, ys)) * 0.6 or 1
                n = len(hist)
                for k, (s, y) in enumerate(zip(hist, ys)):
                    col = pg.intColor(k, hues=max(n, 2), values=1, maxValue=200)
                    self.tplot.plot(np.array(s.shift), y + k * off, pen=pg.mkPen(col, width=1))

    def series_progress(self, p):
        total = p.get("total", 0)
        n = p.get("n", 0)
        self.lbl_n.setText(f"{n} / {total}" + ("  (paused: press ARM)" if p.get("paused") else "")
                           + ("  done" if p.get("done") else ""))
        if "next_in" in p and not p.get("done"):
            m, s = divmod(int(max(0, p["next_in"])), 60)
            self.lbl_next.setText(f"{m:02d}:{s:02d}")
        else:
            self.lbl_next.setText("—")
        e = int(p.get("elapsed", 0))
        self.lbl_elapsed.setText(f"{e // 3600}:{e % 3600 // 60:02d}:{e % 60:02d}")
        if "end" in p:
            self.lbl_end.setText(time.strftime("~%H:%M", time.localtime(p["end"])))
        self.bar.setValue(int(100 * n / total) if total else 0)

    ready = False

    def set_busy(self, busy):
        self.busy_now = busy
        self.btn_once.setEnabled(self.ready and not busy)
        self.btn_series.setEnabled(self.ready and not busy)

    def set_ready(self, ready):
        self.ready = ready
        self.set_busy(getattr(self, "busy_now", False))


# ----------------------------------------------------------------------------- main window
class _Accept:
    def accept(self):
        pass


class Placeholder(QWidget):
    def __init__(self, title, text):
        super().__init__()
        v = QVBoxLayout(self)
        lab = QLabel(f"<h2>{title}</h2><p>{text}</p><p style='color:#888'>Coming in a later version; "
                     "see docs/SPECIFICATION.md.</p>")
        lab.setWordWrap(True)
        lab.setAlignment(Qt.AlignTop | Qt.AlignLeft)
        v.addWidget(lab)


class MainWindow(QMainWindow):
    AREAS = ["Measure", "Data", "Library", "Analysis", "Calibration"]

    def __init__(self, ctl):
        super().__init__()
        self.ctl = ctl
        self.info = {}
        self.setWindowTitle(f"LibreRamen {__version__}")
        self.setMinimumSize(1000, 640)
        self.fit_to_screen()
        central = QWidget()
        outer = QVBoxLayout(central)
        outer.setContentsMargins(0, 0, 0, 0)
        outer.setSpacing(0)
        self.strip = StatusStrip(ctl.stop)
        outer.addWidget(self.strip)

        self.sim_bar = QWidget()
        sl = QHBoxLayout(self.sim_bar)
        sl.setContentsMargins(10, 0, 10, 0)
        sl.addWidget(QLabel("<b>Simulator:</b> sample"))
        self.sim_sample = QComboBox()
        self.sim_sample.addItems(["ethanol", "chloroform", "carbon tetrachloride", "water", "zinc sulfide"])
        self.sim_sample.currentTextChanged.connect(lambda t: ctl.sim("sample", t))
        sl.addWidget(self.sim_sample)
        for label, act in (("Press ARM", "arm"), ("Open compartment", "open"), ("Close compartment", "close")):
            b = QPushButton(label)
            b.clicked.connect(lambda _=False, a=act: ctl.sim(a))
            sl.addWidget(b)
        sl.addStretch()
        self.sim_bar.hide()
        outer.addWidget(self.sim_bar)

        body = QHBoxLayout()
        body.setContentsMargins(0, 0, 0, 0)
        side = QVBoxLayout()
        side.setContentsMargins(6, 8, 0, 8)
        self.stack = QStackedWidget()
        self.side_buttons = []
        self.measure = MeasurePage(ctl, self)
        from .data import DataPage
        self.data = DataPage(spectrum_plot, self.measure.proc_settings)
        pages = [self.measure,
                 self.data,
                 Placeholder("Library", "Spectral library (open databases, literature, your own spectra) and matching."),
                 Placeholder("Analysis", "PCA, EFA, OPA, SIMPLISMA, MCR-ALS, NNLS; comparison with computed spectra."),
                 Placeholder("Calibration", "Calibration against reference standards (wizard), quick checks, history. "
                                            "Already available on the command line: python3 -m libreramen calibrate")]
        side_css = ("QToolButton { border: none; border-left: 4px solid transparent; padding: 8px 6px; "
                    "text-align: left; color: #3a3f47; font-size: 11pt; } "
                    "QToolButton:hover { background: rgba(0,0,0,0.05); } "
                    f"QToolButton:checked {{ border-left: 4px solid {ACCENT['measure']}; background: rgba(31,111,104,0.10);"
                    " font-weight: bold; color: #1b1f24; }")
        for i, (name, page) in enumerate(zip(self.AREAS, pages)):
            b = QToolButton(text=name)
            b.setCheckable(True)
            b.setToolButtonStyle(Qt.ToolButtonTextOnly)
            b.setFixedSize(104, 44)
            b.setStyleSheet(side_css)
            b.clicked.connect(lambda _=False, k=i: self.show_area(k))
            side.addWidget(b)
            self.side_buttons.append(b)
            self.stack.addWidget(page)
        side.addStretch()
        body.addLayout(side)
        body.addWidget(self.stack, 1)
        outer.addLayout(body, 1)

        self.logview = QPlainTextEdit(readOnly=True)
        self.logview.setMaximumHeight(70)
        from PySide6.QtGui import QFontDatabase
        mono = QFontDatabase.systemFont(QFontDatabase.FixedFont)
        mono.setPointSize(9)
        self.logview.setFont(mono)
        outer.addWidget(self.logview)
        self.setCentralWidget(central)
        self.show_area(0)

        m = self.menuBar().addMenu("&File")
        a = QAction("Quit", self)
        a.triggered.connect(self.close)
        m.addAction(a)
        m = self.menuBar().addMenu("&Instrument")
        a = QAction("Use simulator", self)
        a.triggered.connect(lambda: self.connect_instrument(True))
        m.addAction(a)
        a = QAction("Reconnect", self)
        a.triggered.connect(lambda: self.connect_instrument(False))
        m.addAction(a)

        ctl.status.connect(self.on_status)
        ctl.log.connect(self.add_log)
        ctl.error.connect(self.on_error)
        ctl.spectrum.connect(self.measure.add_spectrum)
        ctl.spectrum.connect(self.add_to_data)
        ctl.series_progress.connect(self.measure.series_progress)
        ctl.busy.connect(self.measure.set_busy)
        ctl.connected.connect(self.on_connected)
        ctl.connect_failed.connect(self.on_connect_failed)

    def fit_to_screen(self):
        """Use at most the available screen area (macOS: without menu bar and Dock)."""
        scr = QApplication.primaryScreen()
        if scr is None:
            self.resize(1440, 900)
            return
        a = scr.availableGeometry()
        w, h = min(1440, int(a.width() * 0.92)), min(900, int(a.height() * 0.92))
        self.resize(w, h)
        self.move(a.x() + (a.width() - w) // 2, a.y() + (a.height() - h) // 2)

    def add_to_data(self, sp):
        from .. import fileio
        name = f"{sp.meta.get('sample', 'spectrum')} {time.strftime('%H:%M:%S', time.localtime(sp.t))}"
        if "series_index" in sp.meta:
            name += f" #{sp.meta['series_index']}"
        self.data.add(fileio.from_spectrum(sp, name), checked=True)

    def show_area(self, k):
        self.stack.setCurrentIndex(k)
        for i, b in enumerate(self.side_buttons):
            b.setChecked(i == k)

    def connect_instrument(self, simulate):
        self.ctl.connect_instrument(simulate)

    def add_log(self, msg):
        self.logview.appendPlainText(time.strftime("%H:%M:%S  ") + msg)

    def on_error(self, msg):
        self.add_log("ERROR: " + msg.splitlines()[0])
        QMessageBox.warning(self, "LibreRamen", msg.split("\nTraceback")[0])

    def on_status(self, st):
        self.strip.update_status(st, self.info)
        if st.get("connection") != "connected" and self.measure.ready:
            self.measure.set_ready(False)
        laser = st.get("laser_on")
        self.setWindowTitle(("● LASER ON – " if laser else "") + f"LibreRamen {__version__}")
        self.sim_bar.setVisible(bool(st.get("simulated")))

    def on_connected(self, info):
        self.info = info
        self.measure.set_ready(True)
        self.add_log(f"Instrument ready: {info['model']} S/N {info['serial']}, calibration {info['calibration']}")

    def on_connect_failed(self, msg):
        r = QMessageBox(self)
        r.setWindowTitle("No Raman spectrometer found")
        r.setText("<b>No Raman spectrometer found.</b>")
        r.setInformativeText("Check: USB cable connected · instrument switched on and <i>On</i> pressed "
                             "(wait until its screen is ready) · vendor software closed.\n\n" + msg)
        retry = r.addButton("Try again", QMessageBox.AcceptRole)
        sim = r.addButton("Use simulator", QMessageBox.ActionRole)
        r.addButton("Offline (analysis only)", QMessageBox.RejectRole)
        r.exec()
        if r.clickedButton() is retry:
            self.connect_instrument(False)
        elif r.clickedButton() is sim:
            self.connect_instrument(True)

    def closeEvent(self, ev):
        if self.ctl.is_busy and QMessageBox.question(self, "Quit", "A measurement is running. Stop it and quit?") \
                != QMessageBox.Yes:
            ev.ignore()
            return
        self.ctl.shutdown()
        ev.accept()


def main(argv=None):
    argv = sys.argv[1:] if argv is None else argv
    pg.setConfigOptions(antialias=True)
    app = QApplication.instance() or QApplication([sys.argv[0]])
    app.setApplicationName("LibreRamen")
    app.setStyleSheet("QToolTip { max-width: 340px; padding: 4px; }")     # long tooltips wrap
    from .icon import app_icon
    app.setWindowIcon(app_icon())
    splash = Splash()
    splash.show()
    splash.message("Starting…")
    app.processEvents()
    ctl = Controller()
    win = MainWindow(ctl)
    simulate = "--sim" in argv

    def progress(f):
        splash.message(f"Initialising… {int(f * 100)} %")
    ctl.init_progress.connect(progress)

    def ready(info):
        splash.message(f"{info['model']} found (S/N {info['serial']})")
        QTimer.singleShot(600, lambda: (win.show(), splash.finish(win)))

    def failed(_msg):
        win.show()
        splash.finish(win)
    ctl.connected.connect(ready)
    ctl.connect_failed.connect(failed)
    if "--screenshot" in argv:                      # documentation/testing: save the window as an image
        shot = argv[argv.index("--screenshot") + 1]

        def demo():
            if simulate:
                ctl.sim("arm")
                ctl.sim("sample", "carbon tetrachloride")
                win.measure.exposure.setValue(0.5)
                win.measure.averages.setValue(1)
                win.measure.measure_once()
        ctl.connected.connect(lambda i: QTimer.singleShot(800, demo))
        QTimer.singleShot(15000, lambda: win.grab().save(shot))
    splash.message("Searching for instrument…")
    QTimer.singleShot(200, lambda: ctl.connect_instrument(simulate))
    return app.exec()


if __name__ == "__main__":
    sys.exit(main())
