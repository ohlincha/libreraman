# LibreRamen – Specification

Version 0.1 (draft), 2026-11-20. This document describes what LibreRamen should do. It is the
reference for development and for anyone evaluating or contributing to the software.

## 1. Purpose and scope

LibreRamen is open-source software for Raman spectrometers whose vendor software is no longer
usable. The first supported instrument is the Anton Paar Cora 5000 (BaySpec hardware inside;
532 nm and 1064 nm lasers). The vendor software runs only on an old, never-updated Windows and
lacks features such as scheduled measurements.

Goals:
- Control the instrument directly over USB on **Windows 10/11, macOS (12 Monterey and later) and
  Linux (Debian 12/13, Ubuntu 22.04/24.04)**, with native installers (`.exe`, `.dmg`, `.deb`).
- Make everyday measurements fast and safe, and add time series, calibration against common
  standards, a spectral library, comparison, plotting and basic chemometrics.
- Be scriptable (command line, Python, local API, AI agents via MCP).
- Keep raw data and full provenance for every measurement.

Non-goals (for now): Raman mapping/imaging (the Cora has no stage), regulated (21 CFR Part 11)
compliance, cloud services.

*LibreRamen is independent and not affiliated with Anton Paar or BaySpec. Product names are used
only to describe compatibility.*

## 2. Architecture

```
GUI (Qt) ──┐
CLI ───────┤
Python ────┼──►  LibreRamen server  ──►  instrument driver  ──USB──►  instrument
local API ─┤        (owns the hardware,        (Cora 5000; simulator;
AI (MCP) ──┘         enforces safety)           more drivers later)
```

- **Engine** (`libreramen` package): drivers, acquisition, processing, calibration, library,
  analysis. Pure Python with numpy/scipy.
- **Server**: a background process that owns the instrument. All clients see the same state.
  Safety limits are enforced here, independent of the client.
- **Clients**: the Qt GUI (PySide6 + pyqtgraph), the command line, a Python client library, a local
  HTTP/JSON API, and an MCP server for AI agents.
- **Simulator**: a driver that produces realistic spectra (from real recorded data) for
  development, demos and training. Always clearly marked as such.

## 3. Instrument support (Cora 5000)

Decoded and tested on the real instrument (see `PROTOCOL.md`):

| Part | Interface | Function |
|---|---|---|
| BaySpec spectrometer "BS2ACCD" | USB vendor class (libusb / WinUSB) | 532 nm channel, 2048 px |
| BaySpec spectrometer "BS2BASE" | same | 1064 nm channel, 256 px |
| I/O board (Prolific PL2303) | serial 115200 8N1 | power switching, 532 nm laser on/off, keep-alive |
| Laser driver (CP210x) | serial 19200 8N1 | 1064 nm laser, power via current |
| MCP2200 | HID | ARM state (bit 0), compartment switch (bit 7) |

- Start-up like the vendor's "Initialize system" (about 9 s instead of 50 s).
- Channel switching 532 ↔ 1064 nm with detector cooling wait.
- 532 nm laser power is set on the instrument (not computer-controlled); 1064 nm power is
  computer-controlled.
- Shutdown equivalent to the vendor's (to be refined: onboard computer behaviour).
- Known issue: the 1064 nm channel of the development unit shows no Raman signal (probably a
  hardware fault).

## 4. Safety

- The laser can only emit when a person has pressed **ARM** on the instrument; opening the
  compartment disarms it. The software never works around this.
- The laser is switched on only for the duration of a measurement (plus settling time) and is
  switched off in every case: normal end, error, stop, disconnect, program exit.
- **Watchdog**: a separate process switches the laser off if the main program stops responding.
  At start-up the software checks the laser line and switches it off if needed.
- **Keep-alive**: regular status polling (like the vendor software) so the instrument doesn't sleep
  during long series; the PC is kept awake during a series.
- Limits (maximum power, exposure, laser-on time) are enforced by the server for all clients.
- Remote control (CLI/API/AI) of the laser requires explicit permission given at the instrument
  (GUI switch or configuration); without it, remote clients are read-only.
- Every action is logged with its origin (GUI, CLI, script, AI agent).

## 5. User interface

### 5.1 Start
- **Splash screen** with logo, version, citation note; shows start-up progress (searching →
  instrument found → initialising). Click to skip.
- Instrument not found: a checklist (cable, power, On pressed, vendor software closed, drivers or
  permissions) and the options *Simulator* or *Offline (analysis only)*.
- Automatic initialisation after connecting (configurable).
- On exit: ask whether to shut the instrument down or leave it on ("remember my choice").

### 5.2 Main window
- **Status strip, always visible**: connection (not found / initialising / connected / lost /
  simulator), laser (not armed / armed / **emitting**), compartment (open/closed), channel,
  remote-control state, large **STOP** button. Colour + symbol + text. Window title and taskbar
  icon show the laser state. Status is read from the hardware about twice per second.
- **Side bar** with five areas: Measure, Data, Library, Analysis, Calibration. Panels within an area
  can be moved, hidden or detached to a second screen. A data browser is shared by all areas.

### 5.3 Measure (dashboard: everything at a glance)
- Large **live spectrum** (previous spectra of a series faded behind; peak labels; zoom/pan;
  optional overlay of a reference or library hit).
- **Settings** editable in place: sample name, folder, exposure, scans per spectrum (averages),
  background mode, channel/power.
- **Time series**: interval, duration or number of spectra, start (now / delay / at a time);
  segments (e.g. every 30 s for 10 min, then every 5 min for 3 h). Laser off between spectra.
- **Processing**: background mode, baseline method and strength (slider, live preview), cosmic-ray
  removal, smoothing; show raw / corrected / both; the baseline is drawn.
- **Series progress**: n of N, countdown to next spectrum, elapsed, expected end time.
- **Trend / waterfall**: band intensity/area vs time, or waterfall of the series so far.
- **Detector**: fill level (warns before saturation), dark level, cooling.
- **Last spectrum**: time, settings, main peaks, best library hit.
- **Log**: saved, paused, compartment opened, continued, errors.
- Buttons: Measure once, Start series, Pause, Stop. **Methods**: load/save the full setup.
- Interruptions (compartment opened during a series): pause, mark the gap, continue after ARM.

### 5.4 Data
- Overlay, stacked (offset), waterfall, heat map (time or sample vs shift), trend plots.
- Normalisation (none, max, area, band), difference spectra with scaling, similarity score,
  side-by-side peak tables with shifts marked.
- Peak labelling; x axis in cm⁻¹ or nm; publication-quality export (PNG, SVG, PDF) with styles.

### 5.5 Library
- Sources: open databases where the licence allows (e.g. RRUFF, Raman Open Database — licences
  checked before import), literature band lists, computed spectra (clearly marked), and the user's
  own spectra (one click "Add to library", with name, formula, CAS, notes, conditions, operator).
- Storage: one SQLite file, can be shared on a network drive. Every entry keeps its source.
- Matching: full-spectrum hit quality index (baseline-corrected, normalised), peak-list matching,
  later mixture analysis; filters by class, source, laser.
- Import of the vendor's factory libraries for the owner's own use if technically possible
  (never redistributed).

### 5.6 Analysis
- One preprocessing chain (region, baseline, normalisation, derivative) shared with display.
- **PCA** (scores, loadings, explained variance), **EFA** (forward/backward), **OPA**, **SIMPLISMA**,
  **MCR-ALS** (non-negativity, closure, unimodality, fixed spectra; show rotational ambiguity),
  **NNLS** fitting with library spectra; later **PLS**.
- **Computed spectra**: read Gaussian, ORCA, NWChem, CP2K, Quantum ESPRESSO, phonopy/VASP, CASTEP;
  convert Raman activities to intensities for the laser wavelength and temperature; frequency
  scaling (manual or fitted); broadening; overlay; band assignment table; compare several
  structures; optional direct access to ECCE projects.

### 5.7 Calibration
- Library of reference standards: ASTM E1840 (cyclohexane, naphthalene, polystyrene, sulfur,
  paracetamol, benzonitrile, toluene/acetonitrile, indene), common solvents and solids.
- Wizard: choose standard(s) → measure → automatic peak matching → polynomial fit → residual per
  band → accept/reject. Several standards can be combined. Stored per instrument with history.
- Quick check with one standard; reminder after N days; laser-wavelength one-point correction.
- Instrument log: dark level, detector temperature, calibration drift, laser use.

## 6. Processing

Non-destructive: raw signal and background frames are always stored; corrections are applied on
top and can be changed later for a whole series.
- Background: per spectrum, once, every N minutes, stored file (with scaling, e.g. solvent), none.
- Baseline: asymmetric least squares (ALS, arPLS, airPLS), modified polynomial, rolling ball,
  SNIP, manual anchor points, vendor-style segments.
- Cosmic-ray removal (multi-scan comparison and single-spectrum spike detection), Savitzky–Golay
  smoothing, intensity-response correction (instrument NIST coefficients), normalisation,
  resampling to an even grid.
- Saturation detection; optional auto-exposure.

## 7. Data and files

- Every spectrum is written immediately; long series are crash-safe.
- Provenance in every file: instrument, serial, channel, settings, calibration (source and date),
  processing, software version, operator, sample, timestamps, checksum of raw data.
- **Write**: CSV/TXT, JCAMP-DX, SPC, HDF5 (series and projects), Excel, project files, PNG/SVG/PDF.
- **Read**: own formats, the vendor's SPC/CSV/TXT (including old data and `.mtd` methods),
  JCAMP-DX, RRUFF, plain columns, other instruments' formats where feasible (e.g. Renishaw WDF).
- Export to electronic lab notebooks (simple formats first).

## 8. Integration and automation

- **CLI** with JSON output and exit codes; **Python client**; **local HTTP/JSON API**; **MCP server**
  with tools such as status, acquire, run_series, get_spectrum, find_peaks, match, calibrate, pca.
- Logging other devices alongside a series (e.g. temperature from a VICTOR multimeter via
  victor-dmm) and merging by timestamp; external triggers.
- Notifications (series finished, error, compartment opened) by e-mail or messenger.
- Read-only remote viewing of a running series in a browser.

## 9. Platforms and distribution

- Windows 10 (1809+)/11: installer including the WinUSB driver for the spectrometers (warns that
  the vendor software then can't use them on that PC; offers undo). Note: the Prolific chip may
  need an older driver version.
- macOS 12+ (Intel and Apple Silicon): `.dmg` (universal) with libusb and hidapi bundled.
- Debian 12/13, Ubuntu 22.04/24.04: `.deb` with udev rule and desktop entry (Ubuntu 24.04+/Debian 13+;
  built against the distribution's Qt), AppImage for older and other distributions.
- Built automatically on GitHub for each release. Signing optional (later).
- Test machines: Linux PC at the instrument (primary), Windows 11 laptop, macOS 12 laptop.
  The old Windows 10 1511 PC keeps the vendor software (Qt 6 doesn't support 1511).

## 10. Quality and project

- Simulator-based automated tests; hardware tests on the instrument for each release.
- Documentation: user manual with tutorials, developer notes, protocol notes.
- Licence MIT; citable release (Zenodo DOI), possibly a short software paper.
- English and Swedish; dark mode; large-font friendly. No telemetry; update check only.

## 11. Roadmap

1. Engine (done for 532 nm: start-up, dark, acquisition, averaging, time series, calibration fit).
2. Simulator, processing, Qt GUI: splash, status strip, Measure dashboard.
3. Data area (comparison, plots, export formats), safety watchdog, keep-alive.
4. Calibration wizard, library and matching.
5. Analysis (PCA/EFA/OPA/MCR-ALS, computed spectra).
6. Server, CLI/JSON, API, MCP.
7. Installers for all platforms, documentation, first release.
