"""
Data area: open, compare, process, fit and export spectra.

Spectra measured in this session are added automatically; files can be opened or dropped onto the list.
"""

import os

import numpy as np
import pyqtgraph as pg
from PySide6.QtCore import Qt
from PySide6.QtWidgets import (QAbstractItemView, QCheckBox, QComboBox, QFileDialog, QHBoxLayout, QHeaderView,
                               QLabel, QListWidget, QListWidgetItem, QMenu, QMessageBox, QPushButton, QSlider,
                               QSplitter, QStackedWidget, QTableWidget, QTableWidgetItem, QToolButton, QVBoxLayout,
                               QWidget)

from .. import fileio, peaks as peakmod, processing

COLORS = ["#1f4e79", "#b5543c", "#3c8d6e", "#c8892a", "#6a4c93", "#2a7f9e", "#9c3d5a", "#7a7a2a", "#555b63"]
OPEN_FILTER = ("Spectra (*.csv *.txt *.spc *.jdx *.dx *.dat);;LibreRamen / vendor CSV (*.csv);;"
               "Vendor text (*.txt);;Galactic SPC (*.spc);;JCAMP-DX (*.jdx *.dx);;All files (*)")


class SpectrumList(QListWidget):
    def __init__(self, page):
        super().__init__()
        self.page = page
        self.setAcceptDrops(True)
        self.setSelectionMode(QAbstractItemView.ExtendedSelection)

    def dragEnterEvent(self, e):
        if e.mimeData().hasUrls():
            e.acceptProposedAction()

    dragMoveEvent = dragEnterEvent

    def dropEvent(self, e):
        self.page.open_paths([u.toLocalFile() for u in e.mimeData().urls()])


class DataPage(QWidget):
    def __init__(self, plot_factory, proc_settings):
        super().__init__()
        self.proc_settings = proc_settings      # callable: the Measure page's processing settings
        self.spectra = []
        self.fit_items = []
        outer = QHBoxLayout(self)
        outer.setContentsMargins(8, 4, 8, 8)
        split = QSplitter(Qt.Horizontal)
        outer.addWidget(split)

        # --- left: list
        left = QWidget()
        lv = QVBoxLayout(left)
        lv.setContentsMargins(0, 0, 0, 0)
        lv.addWidget(QLabel("<b>Spectra</b> (tick to show; drop files here)"))
        self.list = SpectrumList(self)
        self.list.itemChanged.connect(self.redraw)
        self.list.currentRowChanged.connect(self.update_peaks)
        lv.addWidget(self.list, 1)
        row = QHBoxLayout()
        for text, fn in (("Open…", self.open_files), ("Remove", self.remove_selected), ("Clear", self.clear)):
            b = QPushButton(text)
            b.clicked.connect(fn)
            row.addWidget(b)
        lv.addLayout(row)
        row = QHBoxLayout()
        b = QPushButton("All")
        b.clicked.connect(lambda: self.check_all(True))
        row.addWidget(b)
        b = QPushButton("None")
        b.clicked.connect(lambda: self.check_all(False))
        row.addWidget(b)
        lv.addLayout(row)
        split.addWidget(left)

        # --- right: controls, plot, peaks
        right = QWidget()
        rv = QVBoxLayout(right)
        rv.setContentsMargins(0, 0, 0, 0)
        bar = QHBoxLayout()
        bar.addWidget(QLabel("Show"))
        self.mode = QComboBox()
        self.mode.addItems(["Overlay", "Stacked", "Heat map"])
        self.mode.currentIndexChanged.connect(self.redraw)
        bar.addWidget(self.mode)
        bar.addWidget(QLabel("Normalise"))
        self.norm = QComboBox()
        self.norm.addItems(["none", "max", "area"])
        self.norm.currentIndexChanged.connect(self.redraw)
        bar.addWidget(self.norm)
        self.baseline = QCheckBox("baseline correction")
        self.baseline.setChecked(True)
        self.baseline.toggled.connect(self.redraw)
        bar.addWidget(self.baseline)
        self.diff = QCheckBox("difference A − B")
        self.diff.setToolTip("<p>First ticked spectrum minus the second, both processed as shown</p>")
        self.diff.toggled.connect(self.redraw)
        bar.addWidget(self.diff)
        bar.addWidget(QLabel("offset"))
        self.offset = QSlider(Qt.Horizontal, minimum=0, maximum=100, value=40)
        self.offset.setMaximumWidth(100)
        self.offset.valueChanged.connect(self.redraw)
        bar.addWidget(self.offset)
        bar.addStretch()
        exp = QToolButton(text="Export ▾")
        exp.setPopupMode(QToolButton.InstantPopup)
        m = QMenu(exp)
        m.addAction("Ticked spectra as CSV…", lambda: self.export("csv"))
        m.addAction("Ticked spectra as JCAMP-DX…", lambda: self.export("jdx"))
        m.addAction("Ticked spectra as one table (common axis)…", lambda: self.export("table"))
        m.addSeparator()
        m.addAction("Plot as PNG…", lambda: self.export_plot("png"))
        m.addAction("Plot as SVG…", lambda: self.export_plot("svg"))
        exp.setMenu(m)
        bar.addWidget(exp)
        rv.addLayout(bar)

        self.stack = QStackedWidget()
        self.plot = plot_factory()
        self.plot.setLabel("bottom", "Raman shift (cm⁻¹)")
        self.plot.setLabel("left", "Intensity")
        self.plot.showGrid(x=True, y=True, alpha=0.2)
        self.plot.addLegend(offset=(-10, 10))
        self.stack.addWidget(self.plot)
        self.heat = pg.PlotWidget(background="w")
        self.heat.setLabel("bottom", "Raman shift (cm⁻¹)")
        self.heat.setLabel("left", "spectrum #")
        self.heat_img = pg.ImageItem()
        self.heat.addItem(self.heat_img)
        self.stack.addWidget(self.heat)
        rv.addWidget(self.stack, 3)

        prow = QHBoxLayout()
        prow.addWidget(QLabel("<b>Peaks</b> of the current spectrum"))
        prow.addStretch()
        prow.addWidget(QLabel("fit profile"))
        self.profile = QComboBox()
        self.profile.addItems(["pvoigt", "voigt", "lorentz", "gauss"])
        prow.addWidget(self.profile)
        b = QPushButton("Fit peaks in view")
        b.setToolTip("<p>Fits the peaks of the current spectrum inside the visible x range (zoom in first)</p>")
        b.clicked.connect(self.fit_view)
        prow.addWidget(b)
        b = QPushButton("Clear fit")
        b.clicked.connect(self.clear_fit)
        prow.addWidget(b)
        rv.addLayout(prow)
        self.table = QTableWidget(0, 6)
        self.table.setHorizontalHeaderLabels(["position (cm⁻¹)", "±", "height", "FWHM (cm⁻¹)", "area", "S/N or η"])
        self.table.horizontalHeader().setSectionResizeMode(QHeaderView.Stretch)
        self.table.setMaximumHeight(170)
        rv.addWidget(self.table, 1)
        self.fit_label = QLabel("")
        rv.addWidget(self.fit_label)
        split.addWidget(right)
        split.setSizes([260, 900])

    # ------------------------------------------------------------------ list management
    def add(self, sp, checked=True):
        self.spectra.append(sp)
        it = QListWidgetItem(sp.name)
        it.setFlags(it.flags() | Qt.ItemIsUserCheckable)
        it.setCheckState(Qt.Checked if checked else Qt.Unchecked)
        it.setToolTip(sp.source or "measured in this session")
        self.list.blockSignals(True)
        self.list.addItem(it)
        self.list.blockSignals(False)
        self.list.setCurrentRow(self.list.count() - 1)
        self.redraw()

    def open_files(self):
        paths, _ = QFileDialog.getOpenFileNames(self, "Open spectra", os.path.expanduser("~"), OPEN_FILTER)
        self.open_paths(paths)

    def open_paths(self, paths):
        errors = []
        for p in paths:
            if os.path.isdir(p):
                for root, _, files in os.walk(p):
                    self.open_paths([os.path.join(root, f) for f in sorted(files)
                                     if f.lower().endswith((".csv", ".txt", ".spc", ".jdx", ".dx"))])
                continue
            try:
                for sp in fileio.read(p):
                    self.add(sp, checked=len(self.spectra) < 12)
            except Exception as e:  # noqa: BLE001
                errors.append(f"{os.path.basename(p)}: {e}")
        if errors:
            QMessageBox.warning(self, "Open spectra", "Some files could not be read:\n\n" + "\n".join(errors[:15]))

    def remove_selected(self):
        rows = sorted({i.row() for i in self.list.selectedIndexes()}, reverse=True)
        for r in rows:
            self.list.takeItem(r)
            del self.spectra[r]
        self.redraw()

    def clear(self):
        self.list.clear()
        self.spectra.clear()
        self.redraw()

    def check_all(self, on):
        self.list.blockSignals(True)
        for i in range(self.list.count()):
            self.list.item(i).setCheckState(Qt.Checked if on else Qt.Unchecked)
        self.list.blockSignals(False)
        self.redraw()

    def ticked(self):
        return [s for i, s in enumerate(self.spectra) if self.list.item(i).checkState() == Qt.Checked]

    # ------------------------------------------------------------------ processing and drawing
    def processed(self, sp):
        x, y = sp.x, sp.y
        ps = dict(self.proc_settings())
        if not self.baseline.isChecked():
            ps["baseline"] = "none"
        ps["normalize"] = self.norm.currentText()
        y2, _ = processing.process(x, y, ps)
        return x, y2

    def redraw(self, *_):
        self.clear_fit(redraw=False)
        self.plot.clear()
        sel = self.ticked()
        mode = self.mode.currentText()
        self.stack.setCurrentIndex(1 if mode == "Heat map" else 0)
        if not sel:
            return
        data = [self.processed(s) for s in sel]
        if self.diff.isChecked() and len(data) >= 2:
            (xa, ya), (xb, yb) = data[0], data[1]
            yd = ya - np.interp(xa, xb, yb)
            self.plot.plot(xa, yd, pen=pg.mkPen(COLORS[0], width=1.4), name=f"{sel[0].name} − {sel[1].name}")
            self.plot.addLine(y=0, pen=pg.mkPen("#999", style=Qt.DashLine))
            self.plot.getViewBox().rescale_y(0.08)
            return
        if mode == "Heat map":
            lo = max(x[0] for x, _ in data)
            hi = min(x[-1] for x, _ in data)
            grid = np.linspace(lo, hi, 1200)
            img = np.array([np.interp(grid, x, y) for x, y in data])
            self.heat_img.setImage(img.T, autoLevels=True)
            self.heat_img.setRect(lo, 0, hi - lo, len(data))
            self.heat_img.setColorMap(pg.colormap.get("viridis"))
            return
        span = max((np.percentile(y, 99.5) - np.percentile(y, 1)) for _, y in data) or 1.0
        step = span * self.offset.value() / 100.0 if mode == "Stacked" else 0.0
        for k, ((x, y), s) in enumerate(zip(data, sel)):
            col = COLORS[k % len(COLORS)]
            self.plot.plot(x, y + k * step, pen=pg.mkPen(col, width=1.3), name=s.name)
        self.plot.getViewBox().rescale_y(0.08)

    # ------------------------------------------------------------------ peaks and fitting
    def current(self):
        r = self.list.currentRow()
        return self.spectra[r] if 0 <= r < len(self.spectra) else None

    def update_peaks(self, *_):
        sp = self.current()
        self.table.setRowCount(0)
        if sp is None:
            return
        x, y = self.processed(sp)
        for p in peakmod.pick(x, y, snr=8, xmin=150):
            self._row([p.position, None, p.height, p.fwhm, p.area, p.snr])

    def _row(self, vals):
        r = self.table.rowCount()
        self.table.insertRow(r)
        fmts = ["{:.1f}", "{:.2f}", "{:.0f}", "{:.1f}", "{:.0f}", "{:.2f}"]
        for c, (v, f) in enumerate(zip(vals, fmts)):
            self.table.setItem(r, c, QTableWidgetItem("" if v is None else f.format(v)))

    def fit_view(self):
        sp = self.current()
        if sp is None:
            return
        x, y = self.processed(sp)
        lo, hi = self.plot.getViewBox().viewRange()[0]
        if hi - lo > 1200:
            QMessageBox.information(self, "Fit", "Zoom in on the band(s) to fit first (visible range < 1200 cm⁻¹).")
            return
        try:
            res = peakmod.fit(x, y, (lo, hi), shape=self.profile.currentText())
        except Exception as e:  # noqa: BLE001
            QMessageBox.warning(self, "Fit", str(e))
            return
        self.clear_fit(redraw=False)
        self.fit_items.append(self.plot.plot(res.x, res.model, pen=pg.mkPen("#000", width=1.2, style=Qt.DashLine)))
        base = res.baseline[0] + res.baseline[1] * (res.x - res.x.mean())
        for k, comp in enumerate(res.components):
            self.fit_items.append(self.plot.plot(res.x, comp + base, pen=pg.mkPen(COLORS[(k + 3) % len(COLORS)],
                                                                                   width=1)))
        self.table.setRowCount(0)
        for p in res.peaks:
            self._row([p["position"], p["position_err"], p["height"], p["fwhm"], p["area"], p.get("eta")])
        self.fit_label.setText(f"{res.shape} fit of {len(res.peaks)} band(s) in {lo:.0f}–{hi:.0f} cm⁻¹: "
                               f"R² = {res.r2:.5f}, RMS residual {res.rms:.1f}")

    def clear_fit(self, redraw=True):
        for it in self.fit_items:
            self.plot.removeItem(it)
        self.fit_items = []
        self.fit_label.setText("")

    # ------------------------------------------------------------------ export
    def export(self, kind):
        sel = self.ticked()
        if not sel:
            return
        if kind == "table":
            p, _ = QFileDialog.getSaveFileName(self, "Save table", "spectra.csv", "CSV (*.csv)")
            if p:
                fileio.write_table(p, [fileio.DataSpectrum(s.name, *self.processed(s)) for s in sel])
            return
        d = QFileDialog.getExistingDirectory(self, "Folder for the exported spectra")
        if not d:
            return
        for s in sel:
            x, y = self.processed(s)
            out = fileio.DataSpectrum(s.name, x, y, dict(s.meta, processing=self._describe()))
            safe = "".join(c if c.isalnum() or c in "-_." else "_" for c in s.name)
            if kind == "csv":
                fileio.write_csv(os.path.join(d, safe + ".csv"), out)
            else:
                fileio.write_jcamp(os.path.join(d, safe + ".jdx"), out)

    def _describe(self):
        ps = self.proc_settings()
        return (f"baseline={ps['baseline'] if self.baseline.isChecked() else 'none'} "
                f"strength={ps['strength']:.1f} despike={ps['despike']} smooth={ps['smooth']} "
                f"normalize={self.norm.currentText()}")

    def export_plot(self, kind):
        p, _ = QFileDialog.getSaveFileName(self, "Save plot", f"spectra.{kind}", f"{kind.upper()} (*.{kind})")
        if not p:
            return
        import pyqtgraph.exporters as ex
        item = self.plot.getPlotItem() if self.stack.currentIndex() == 0 else self.heat.getPlotItem()
        e = ex.SVGExporter(item) if kind == "svg" else ex.ImageExporter(item)
        if kind == "png":
            e.parameters()["width"] = 2000
        e.export(p)
