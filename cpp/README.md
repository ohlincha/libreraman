# LibreRamen – C++ implementation

The C++/Qt 6 implementation of LibreRamen. The Python package in `../libreramen` is the **reference**:
it was developed against the real instrument, and the C++ code must behave the same (same USB/serial
traffic, same safety rules, same file formats, same numbers within floating-point tolerance).
`../PROTOCOL.md` documents the instrument.

## Build

Dependencies: Qt ≥ 6.2 (Core, Gui, Widgets, Svg, SerialPort, Network, Sql with the SQLite driver, Test), libusb-1.0,
hidapi, Eigen 3.

```
# Debian/Ubuntu
sudo apt install cmake ninja-build qt6-base-dev libqt6sql6-sqlite qt6-serialport-dev qt6-svg-dev libusb-1.0-0-dev libhidapi-dev libeigen3-dev
cmake -S cpp -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build && ctest --test-dir build --output-on-failure
```
macOS: `brew install qt libusb hidapi eigen`. Windows: Qt from the online installer or aqtinstall,
`vcpkg install libusb hidapi eigen3`.

`LibreRamen --selftest` starts the GUI against the simulator (offscreen), takes and saves one spectrum and
exits 0/1 (for checking packaged apps). Widget tests are `tests/gui_test_*.cpp` (linked with the `libreramen_gui` object library = `src/gui`
without `main.cpp`; run offscreen). `-DLIBRERAMEN_GUI_PREVIEW=ON` builds `gui_preview`, which renders
plots, status strip, splash and icon to PNG files for visual checks:
`QT_QPA_PLATFORM=offscreen ./gui_preview <dir>`; `datapage_preview <dir>` renders the Data area
(overlay, stacked, difference, fit, heat map), `calibration_preview <dir> [--dark]` the Calibration area
with the simulator (temporary config dir), `computed_preview <dir>` the computed-spectra panel against the
simulator's measured CCl4/chloroform. `tests/golden/computed/` holds synthetic, truncated output excerpts
written after each program's layout (not real runs). `tests/test_cli.cpp` runs the command line program against
`tests/golden/cli.json` (output of the Python CLI).
The Analysis page renders itself to `analysis-*.png` when `LIBRERAMEN_PREVIEW_DIR=<dir>` is set for
`gui_test_analysispage`.

Development machine without root: `source ~/opt/libreramen-env.sh` and add `$LR_CMAKE_ARGS` to the
cmake configure line (Qt from aqtinstall, -dev packages extracted with `dpkg -x`).

## Layout and module map

| C++ | Python reference | Contents |
|---|---|---|
| `src/core/spectrum.*` | `spectrum.py` | `Spectrum`, CSV output |
| `src/core/calibration.*` | `calibration.py` | factory calibration, vendor ini |
| `src/core/instrument.*` | `instrument.py` (interface) | abstract `Instrument`, shared helpers |
| `src/core/cora.*` | `instrument.py` (`Cora`) | the real instrument |
| `src/core/hw/*` | `hardware.py` | I/O board, PL2303 over libusb, MCP2200 (hidapi), laser driver, BaySpec spectrometers, discovery |
| `src/core/simulator.*` | `simulator.py` | simulated instrument (`resources/simulator.bin`) |
| `src/core/series.*` | `series.py`, GUI controller | time series |
| `src/core/watchdog.*` | `watchdog.py` | laser watchdog process |
| `src/core/windrivers.*` | `windrivers.py` | Windows driver repair (elevated script) |
| `src/core/processing.*` | `processing.py` | baselines (ALS, arPLS, polynomial, rolling ball, SNIP), despike, smoothing, normalisation |
| `src/core/peaks.*` | `peaks.py` | peak picking, profile fitting |
| `src/core/calibrate.*` | `calibrate.py` | calibration against standards, JSON storage |
| `src/core/standards.*` | `standards.py` | reference standards |
| `src/core/fileio.*` | `fileio.py` | readers/writers (own CSV, vendor CSV/TXT/SPC, JCAMP-DX, columns) |
| `src/core/library.*` | – (C++ only) | spectral library: SQLite file (Qt Sql), schema versioning, import (own spectra, files, RRUFF, band lists), export, demo library |
| `src/core/match.*` | – (C++ only) | library matching: HQI full-spectrum search, peak matching, NNLS (Lawson–Hanson) mixtures |
| `src/core/chemometrics.*` | – (C++ only) | Analysis: data matrix, preprocessing, PCA, EFA, OPA, SIMPLISMA, MCR-ALS (constraints, rotational-ambiguity bands), NNLS |
| `src/core/computed*.*` | – (new in C++) | computed Raman spectra: readers for Gaussian, ORCA (.out/.hess), NWChem, CP2K, Quantum ESPRESSO, vasp_raman.py, Phonopy-Spectroscopy, CASTEP; activities → cross-sections, scaling fit, broadening, assignment, comparison |
| `src/cli/` | `__main__.py` | command line |
| `src/gui/controller.*` | `gui/controller.py` | instrument jobs in a worker thread, status poll, watchdog, saving |
| `src/gui/mainwindow.*`, `measurepage.*` | `gui/app.py` | main window, Measure area |
| `src/gui/page.h`, `pages.cpp` | – | `Page` interface and registry of the side-bar areas (Data, Library, …) |
| `src/gui/datapage.*` | `gui/data.py` | Data area: open/compare/process/fit/export spectra |
| `src/gui/calibrationpage.*`, `calibrationstore.*` | `calibrate.py` (CLI flow) | Calibration area: wizard, quick check, history, instrument log, reminder |
| `src/gui/main.cpp` | `gui/app.py` main, `selftest.py` | `LibreRamen [--sim] [--screenshot F]`, `--selftest`, `--watchdog` |
| `src/gui/` (other) | `gui/` | plot widget, theme, status strip, splash, icon |
| `src/gui/computedpanel.*` | – (new in C++) | computed spectra panel (for the Analysis page) |
| `tests/` | – | QtTest; `tests/golden/` holds outputs of the Python reference |

## Conventions

- Namespace `lr`; C++17; Qt types for strings/paths/JSON, `std::vector<double>` (`lr::Vec`) for data,
  Eigen inside numerical code.
- Errors: exceptions from `core/errors.h` (`lr::Error`, `NotArmed`, `HardwareError`, `Stopped`) with
  user-readable messages (copy the Python messages).
- Hardware is accessed from worker threads; `Instrument` implementations lock internally.
- Laser safety rules of the Python version are kept exactly (see the header of `instrument.h`).
- Files written by the C++ version must be readable by the Python version and vice versa.
- Plotting: own `QPainter` widget (no Qt Charts/QCustomPlot: licences), so the app stays MIT/LGPL-only.
