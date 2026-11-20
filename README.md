# LibreRamen

Open-source, cross-platform software for Raman spectrometers that deserve a second life.
The first supported instrument is the **Anton Paar Cora 5000** (BaySpec hardware inside, 532 nm
and 1064 nm lasers), whose vendor software needs an old, never-updated Windows. LibreRamen talks
to the instrument directly over USB from Linux (Windows and macOS to follow) and adds what the
vendor software lacks: scheduled scans, time series and calibration against common standards.

*LibreRamen is an independent project, not affiliated with or endorsed by Anton Paar or BaySpec.
Product names are used only to describe compatibility.*

**Status: early.** The USB protocol is decoded and the engine (`libreramen` package plus command line)
measures spectra and time series from Linux at 532 nm; 1064 nm works but its axis isn't verified yet. See [PROTOCOL.md](PROTOCOL.md) for everything known so far. The scripts here are test
tools, not the finished program.

![LibreRamen measuring a time series (simulator, ZnS)](docs/screenshot-measure.png)

```
pip install PySide6 pyqtgraph numpy scipy pyserial pyusb
python3 -m libreramen gui          # with the instrument
python3 -m libreramen gui --sim    # simulator: spectra recorded on a real Cora 5000
```

The full feature plan is in [docs/SPECIFICATION.md](docs/SPECIFICATION.md).

## What's inside the instrument

A USB hub with: two BaySpec spectrometers (`ba8c:1688`, "BS2ACCD" for 532 nm and "BS2BASE" for
1064 nm), an I/O board on a Prolific serial port (power switching, 532 nm laser on/off), a CP210x
laser driver (1064 nm), and an MCP2200 that reports the state of the instrument's ARM button.
No vendor drivers are needed on Linux.

## Engine (Python package `libreramen`)

```
python3 -m libreramen status                       # armed? compartment? spectrometers?
python3 -m libreramen init                         # vendor-style start-up (no laser)
python3 -m libreramen dark -e 1                    # dark frame, laser off
python3 -m libreramen acquire -e 1 -n 3            # 532 nm: 3 x 1 s, background-subtracted, CSV
python3 -m libreramen acquire --channel 1064 --power 200 -e 5
python3 -m libreramen series -e 1 --interval 10 --count 60 --background once
python3 -m libreramen shutdown                     # like the vendor's Shutdown
```

`--ini BS_Device_Settings.ini` loads the calibration of your own instrument (copy the file
from the vendor software's `BS_System` folder); without it, the development unit's values are used.

```python
from libreramen import Cora
c = Cora().connect(); c.initialize()
sp = c.acquire(exposure_s=1, averages=3)      # Spectrum: .shift, .intensity, .save_csv()
c.close()
```

## Test scripts (Linux, in `tools/`)

| Script | What it does | Laser |
|---|---|---|
| `probe.py` | Finds the devices and reads status | never |
| `dark.py` | Powers the spectrometers and takes one dark frame | never |
| `laser.py` | One 532 nm frame plus background (needs ARM pressed) | yes |
| `series.py` | Exposure/timing series at 532 nm | yes |
| `test1064.py` | One 1064 nm frame plus background | yes |

Requirements: `python3-serial`, `python3-usb`, membership in the `dialout` group, and the udev rule:

```
sudo cp 70-cora.rules /etc/udev/rules.d/ && sudo udevadm control --reload && sudo udevadm trigger
```

## Laser safety

The scripts only switch the laser on when the instrument is **armed** (the ARM button on the
instrument) and refuse to run without `--operator-present`. The instrument's own interlock cuts
the laser when the sample compartment is open. Always use your laboratory's laser precautions.

## Calibration

The program ships with a library of **reference standards**: the ASTM E1840 set (cyclohexane,
naphthalene, polystyrene, sulfur, paracetamol, benzonitrile, toluene/acetonitrile, indene), common
solvents (CCl₄, CHCl₃, CH₂Cl₂, toluene, benzene, acetonitrile, acetone, ethanol, methanol, isopropanol,
DMSO, ethylene glycol) and solids (silicon, diamond, calcite, anatase, rutile, PTFE, BaSO₄).
`python3 -m libreramen standards` lists them. Measure one or more of whatever your lab has and fit:

```
python3 -m libreramen calibrate --standard chloroform --standard ethanol     # guided, live
```

The program finds the peaks, matches them to the reference bands, fits a new pixel → Raman shift
polynomial, prints the residual of every band and, if you accept, stores it for that instrument.
With CCl₄, CHCl₃ and ethanol on the development unit: 19 of 19 bands matched, RMS 1.0 cm⁻¹
(factory calibration: about +2 cm⁻¹ off).

### Factory calibration

The wavelength calibration is instrument-specific. It is read from the vendor software's
`BS_System/BS_Device_Settings.ini` (copy it from the instrument PC); the values for the
development instrument are listed in PROTOCOL.md.

## License

MIT. The protocol was worked out from USB traffic for interoperability with an instrument we own.
