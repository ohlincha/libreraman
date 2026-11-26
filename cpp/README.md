# LibreRaman – C++ implementation

The C++/Qt 6 implementation of LibreRaman, and the only maintained one. The Python package in `../libreraman`
was the first version, developed against the real instrument; it is **frozen** (not maintained, not shipped).
The golden files in `tests/golden/` were generated from it and are kept as they are; new or changed behaviour
is tested in C++ only (issue #8).
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

`LibreRaman --selftest` starts the GUI against the simulator (offscreen), takes and saves one spectrum and
exits 0/1 (for checking packaged apps). Widget tests are `tests/gui_test_*.cpp` (linked with the `libreraman_gui` object library = `src/gui`
without `main.cpp`; run offscreen). `gui_test_seriesperf` runs a fast 150-spectrum series against the simulator
with the Measure, Data and Analysis areas visible and fails if the event loop is blocked for 1 s or more (it
prints the ping latencies; a Debug build on a desktop: ~100-300 ms, before the throttling ~130 s). Fast series
stay responsive because the controller delivers series results in time-boxed passes (30 ms, newest progress
only), the Measure page redraws at most every 150 ms (cached processing, 4 faded spectra, trend curve updated
in place), the Data/Analysis areas get measured spectra in batches (`SpectrumBatcher`: at most every 1 s / 2 s
while visible and at most 1/5 of the GUI thread's time, on activation when hidden), the Data area caches
processed spectra and draws at most 24 curves in the overlay, and the Analysis area runs PCA/EFA of large data
sets in a background thread and measures peak trends incrementally (only new spectra; peak fits of many
spectra in a background thread). `-DLIBRERAMAN_GUI_PREVIEW=ON` builds `gui_preview`, which renders
plots, status strip, splash and icon to PNG files for visual checks:
`QT_QPA_PLATFORM=offscreen ./gui_preview <dir>`; `datapage_preview <dir>` renders the Data area
(overlay, stacked, difference, fit, heat map), `calibration_preview <dir> [--dark]` the Calibration area
with the simulator (temporary config dir), `computed_preview <dir>` the computed-spectra panel against the
simulator's measured CCl4/chloroform. `tests/golden/computed/` holds synthetic, truncated output excerpts
written after each program's layout (not real runs). `tests/test_cli.cpp` runs the command line program against
`tests/golden/cli.json` (output of the Python CLI).
The Analysis page renders itself to `analysis-*.png` when `LIBRERAMAN_PREVIEW_DIR=<dir>` is set for
`gui_test_analysispage`.

Development machine without root: `source ~/opt/libreraman-env.sh` and add `$LR_CMAKE_ARGS` to the
cmake configure line (Qt from aqtinstall, -dev packages extracted with `dpkg -x`).

## Packaging

`cmake --install` produces an installable layout on every platform (rules in `cmake/Install.cmake`, files in
`packaging/`); CI (`.github/workflows/build.yml`, jobs `cpp-*`) turns it into installers. The C++ version is
taken from `project(... VERSION)` in `CMakeLists.txt` (`packaging/version.sh`; a tag `v*` that differs gives a
warning). Icons are made at build time from `resources/icon.png` by `tools/mkicon.cpp` (`.ico`, `.icns`, hicolor
PNGs), so no generated icons are kept in the repository.

| | install layout | CI job → artifact |
|---|---|---|
| Linux | `bin/LibreRaman`, `bin/LibreRamanCLI`, desktop entry `io.github.ohlincha.libreraman`, hicolor icons, AppStream metainfo, udev rule in `lib/udev/rules.d` | `cpp-linux-deb`: `cpack` → `libreraman_<v>_amd64.deb` (Ubuntu 24.04 build, Depends via dpkg-shlibdeps + Qt plugin packages, postinst reloads udev), tested by `apt install` in clean Ubuntu 24.04 and Debian 13 containers; `cpp-appimage`: linuxdeploy + Qt plugin on Ubuntu 22.04 → `LibreRaman-<v>-Linux-x86_64.AppImage`, tested in Debian 12 |
| macOS | `LibreRaman.app` with `Info.plist` (`packaging/macos`), `LibreRaman.icns`, CLI in `Contents/MacOS` | `cpp-macos`: universal (arm64 + x86_64), deployment target 12.0, libusb/hidapi built from source for it, `macdeployqt`, ad-hoc signature, `packaging/macos/check-bundle.sh` (architectures, minimum macOS, no outside libraries), dmg with Applications link → `LibreRaman-<v>-macOS-universal.dmg`; `cpp-macos-intel` runs it on Intel |
| Windows | `LibreRaman.exe`, `LibreRamanCLI.exe` (icon + version resource), `libusb-1.0.dll`, `hidapi.dll`, MSVC runtime DLLs (app-local) | `cpp-windows`: MSVC Release, `windeployqt` for both programs, Inno Setup (`installer/windows/libreraman.iss /DCpp`, with the WinUSB driver set-up from `wdi.yml`) → `LibreRaman-<v>-Windows-x64-Setup.exe`; installed silently and tested |

Each package is tested with `LibreRamanCLI standards` and `LibreRaman selftest` (GUI + simulator; offscreen,
and xcb/cocoa where available) without the build machine's Qt in the environment. Only Qt's SQLite driver is
shipped (the other SQL drivers are removed from the CI Qt before deploying). `LibreRaman` and `LibreRamanCLI`
both handle `--watchdog` (the program starts itself as its laser watchdog), so both must stay real executables
of the package (no wrapper scripts). Local `.deb` with a Qt not from the distribution:
`-DLIBRERAMAN_DEB_SHLIBDEPS=OFF` (fixed Depends list), then `cpack` in the build directory.

## Layout and module map

| C++ | Python reference | Contents |
|---|---|---|
| `src/core/spectrum.*` | `spectrum.py` | `Spectrum`, CSV output |
| `src/core/calibration.*` | `calibration.py` | factory calibration, vendor ini |
| `src/core/instrument.*` | `instrument.py` (interface) | abstract `Instrument`, shared helpers |
| `src/core/cora.*` | `instrument.py` (`Cora`) | the real instrument |
| `src/core/hw/*` | `hardware.py` | I/O board, PL2303 over libusb, MCP2200 (hidapi), laser driver, BaySpec spectrometers, discovery |
| `src/core/simulator.*` | `simulator.py` | simulated instrument (`resources/simulator.bin`) |
| `src/core/series.*` | `series.py`, GUI controller | time series; series.csv/.jdx/.spc rewritten by a writer thread (see `series.h`) |
| `src/core/watchdog.*` | `watchdog.py` | laser watchdog process |
| `src/core/windrivers.*` | `windrivers.py` | Windows driver repair (elevated script) |
| `src/core/processing.*` | `processing.py` | baselines (ALS, arPLS, polynomial, rolling ball, SNIP), despike, smoothing, normalisation |
| `src/core/peaks.*` | `peaks.py` | peak picking, profile fitting |
| `src/core/calibrate.*` | `calibrate.py` | calibration against standards, JSON storage |
| `src/core/standards.*` | `standards.py` | reference standards |
| `src/core/fileio.*` | `fileio.py` | readers/writers (own CSV, vendor CSV/TXT/SPC, JCAMP-DX, columns); C++ only: multi-block JCAMP-DX and multi-file SPC (layouts in `fileio.h`) |
| `src/core/library.*` | – (C++ only) | spectral library: SQLite file (Qt Sql), schema versioning, import (own spectra, files, RRUFF, band lists), export, demo library |
| `src/core/match.*` | – (C++ only) | library matching: HQI full-spectrum search, peak matching, NNLS (Lawson–Hanson) mixtures |
| `src/core/chemometrics.*` | – (C++ only) | Analysis: data matrix, preprocessing, PCA, EFA, OPA, SIMPLISMA, MCR-ALS (constraints, rotational-ambiguity bands), NNLS |
| `src/core/bandtrend.*` | – (C++ only) | Analysis: peak trends of a series (band height/area/centroid/FWHM, fitted area with errors, ratio, exponential and linear kinetic fits; formulas in the header) |
| `src/core/computed*.*` | – (new in C++) | computed Raman spectra: readers for Gaussian, ORCA (.out/.hess), NWChem, CP2K, Quantum ESPRESSO, vasp_raman.py, Phonopy-Spectroscopy, CASTEP; activities → cross-sections, scaling fit, broadening, assignment, comparison |
| `src/cli/` | `__main__.py` | command line (`remote.*`: `--remote` client of the local API) |
| `src/core/httpserver.*`, `apiclient.*` | – | small HTTP/1.1 server; API file, blocking API client |
| `src/gui/apiserver.*` | – | local HTTP/JSON API of the running application (see below) |
| `src/mcp/` | – | `LibreRamanMCP`, MCP server for AI agents (client of the local API) |
| `src/gui/controller.*` | `gui/controller.py` | instrument jobs in a worker thread, status poll, watchdog, saving |
| `src/gui/mainwindow.*`, `measurepage.*` | `gui/app.py` | main window, Measure area |
| `src/gui/page.h`, `pages.cpp` | – | `Page` interface and registry of the side-bar areas (Data, Library, …) |
| `src/gui/datapage.*` | `gui/data.py` | Data area: open/compare/process/fit/export spectra |
| `src/gui/calibrationpage.*`, `calibrationstore.*` | `calibrate.py` (CLI flow) | Calibration area: wizard, quick check, history, instrument log, reminder |
| `src/gui/main.cpp` | `gui/app.py` main, `selftest.py` | `LibreRaman [--sim] [--screenshot F]`, `--selftest`, `--watchdog` |
| `src/gui/` (other) | `gui/` | plot widget, theme, status strip, splash, icon |
| `src/gui/computedpanel.*` | – (new in C++) | computed spectra panel (for the Analysis page) |
| `tests/` | – | QtTest; `tests/golden/` holds outputs of the Python reference |

## Automation: local API, `--remote`, Python, MCP

The running application owns the instrument; scripts and AI agents talk to it through a **local HTTP/JSON API**
(`src/gui/apiserver.h`), so all clients share one state and one set of safety rules
(docs/SPECIFICATION.md sections 2, 4, 8).

- **Where**: `http://127.0.0.1:8765` (loopback only; port: QSettings `api/port` or `LibreRaman --api-port N`;
  off: Instrument > *Local API*, or `--no-api`). Every request needs `Authorization: Bearer <token>`; the token
  is new at every start and is written with the port to the **API file** (user-only permissions):
  `~/.config/LibreRaman/api.json` (Linux), `~/Library/Preferences/LibreRaman/api.json` (macOS),
  `%LOCALAPPDATA%\LibreRaman\api.json` (Windows), or `$LIBRERAMAN_API_FILE`. Clients read it.
- **Permissions**: remote clients are **read-only** until someone at the instrument checks Instrument >
  *Allow remote measurements* (remembered; the REMOTE tile of the status strip shows OFF / READ-ONLY / CONTROL).
  Without it `POST /measure` and `/series` answer 403. Even with it the laser needs ARM on the instrument
  and a closed compartment. `POST /stop` (stop + laser off) is **always** allowed. Settings are range-checked
  (same limits as the GUI fields, see `GET /info`); unknown fields are rejected. Every action is logged with
  its origin: `GUI: measure …`, `API (cli): measure …`, `API (mcp): STOP (laser off)`.
- **Why no QtHttpServer**: not packaged for the distribution Qt of Ubuntu 24.04 (6.4) used in CI, and an
  extra module elsewhere; `src/core/httpserver.*` is ~250 lines on QTcpServer (one request per connection,
  Content-Length bodies, JSON only).

| Endpoint | Body / query | Answer |
|---|---|---|
| `GET /status` | | `{"connection":"connected","simulated":false,"armed":true,"compartment_open":false,"laser_on":false,"channel":532,"busy":false,"series_running":false,"paused":false,"remote_control":true,"job":null,…}` |
| `GET /info` | | version, `api_version`, instrument (model, serial, calibration), `data_folder`, `limits`, `endpoints` |
| `GET /spectra?limit=N` | | `{"spectra":[{"id":7,"sample":"CCl4","time":"…","file":"…/CCl4_20261120_183901.csv","channel":532,"exposure_s":1,"averages":3,"points":2048}]}` (last 100 kept) |
| `GET /spectra/{id}` | | the above plus `meta`, `shift`, `intensity`, `signal`, `background` |
| `POST /measure` | `{"exposure_s":1,"averages":3,"background":"each","prebleach_s":0,"sample":"CCl4","channel":532,"wait":true}` | 202 `{"id":4,"kind":"measure","state":"running",…}`; with `"wait":true` 200 when finished: `{"state":"done","spectra":[7],…}` (`failed` + `error`, or `stopped`). 403 not allowed, 409 busy, 503 no instrument, 400 invalid |
| `POST /series` | `{"sample":"x","exposure_s":1,"averages":1,"background":"each\|once\|none","interval_s":10,"count":12}` (or `duration_s`; `start_delay_s`, `laser_stays_on`) | 202 job |
| `GET /series/progress` | | `{"running":true,"paused":false,"n":3,"total":12,"elapsed_s":31.2,"next_in_s":8.8,"end":"…","done":false}` |
| `GET /jobs/{id}` | | job (`running`, `done`, `failed`, `stopped`) |
| `POST /stop` | | `{"stopped":true,"was_busy":true,"laser_on":false}` |
| `POST /process` | `{"id":7}` or `{"x":[…],"y":[…]}`, optional `"settings":{"baseline":"snip","strength":5,"despike":true,…}` (default: the Measure page's) | `{"x","y","baseline","settings"}` |
| `POST /peaks` | as `/process`, plus `snr`, `min_fwhm`, `shoulders`, `xmin`, `xmax` | `{"peaks":[{"position":459.1,"height":…,"fwhm":…,"area":…,"snr":…,"shoulder":false}],"noise":…}` |
| `GET /standards` | | reference standards with bands |
| `POST /match` | | 501 until the library registers a handler (`ApiServer::addRoute`) |
| `GET /log?since=N` | | `{"lines":[{"seq":12,"time":"…","text":"API (cli): measure …"}],"next":12}` |

Errors are `{"error":"…","status":N}` (401 token, 403 not allowed, 404, 405, 409 busy, 400 invalid).

```
curl -H "Authorization: Bearer $(jq -r .token ~/.config/LibreRaman/api.json)" http://127.0.0.1:8765/status
```

**Command line** (`LibreRamanCLI --remote -h`): `status`, `info`, `measure [-e S] [-n N] [--sample X]
[--background each|none] [--prebleach S] [--channel 532|1064] [--no-wait]`, `series --interval S (--count N |
--duration S) [--fast]`, `progress`, `job ID`, `stop`, `spectra`, `spectrum ID [-o file.csv]`, `peaks ID`,
`standards`, `log [--since N]`; `--json` prints the API's JSON. Exit codes: 0 ok, 1 failed, 2 usage,
3 LibreRaman not running, 4 not allowed (token / remote measurements off), 5 busy, 130 stopped. The local
commands (`status`, `dark`, `acquire`, `series`, `standards`) also take `--json` (result on stdout, messages
on stderr).

**Python** (standard library only): `libreraman/client.py`

```python
from libreraman.client import Client
c = Client()                                   # reads the API file
job = c.measure(exposure_s=1, averages=3, sample="CCl4")
print(c.peaks(job["spectra"][-1]))
c.stop()
```

**MCP server for AI agents**: `LibreRamanMCP` (stdio, JSON-RPC 2.0; protocol versions 2024-11-05 to
2025-06-18) is a client of the same API (origin `API (mcp)`), so the permissions above apply. Tools: `status`,
`acquire`, `run_series`, `series_progress`, `get_job`, `stop`, `list_spectra`, `get_spectrum` (`max_points`),
`find_peaks`, `process`, `list_standards`, `get_log`. Register it with Claude Code:

```
claude mcp add libreraman -- /path/to/LibreRamanMCP
```

or in any MCP client's JSON configuration (Claude Desktop `claude_desktop_config.json`, etc.):

```json
{ "mcpServers": { "libreraman": { "command": "/path/to/LibreRamanMCP" } } }
```

(`--api-file FILE` if the application uses a non-default API file.) LibreRaman must be running; without
*Allow remote measurements* the agent can read status and spectra but not switch the laser on.

## Conventions

- Namespace `lr`; C++17; Qt types for strings/paths/JSON, `std::vector<double>` (`lr::Vec`) for data,
  Eigen inside numerical code.
- Errors: exceptions from `core/errors.h` (`lr::Error`, `NotArmed`, `HardwareError`, `Stopped`) with
  user-readable messages (copy the Python messages).
- Hardware is accessed from worker threads; `Instrument` implementations lock internally.
- Laser safety rules of the Python version are kept exactly (see the header of `instrument.h`).
- Files written by the C++ version must be readable by the Python version and vice versa.
- Plotting: own `QPainter` widget (no Qt Charts/QCustomPlot: licences), so the app stays MIT/LGPL-only.
