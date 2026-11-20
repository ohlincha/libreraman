# LibreRamen – Cora 5000 USB protocol notes (from captures 2026-11-20)

Captures are in `captures/`. They contain keyboard traffic, including the vendor-software logon at 09:40
(vendor default password). Keep them out of the public repo anyway.

The instrument contains a USB hub (SMSC 0424:2517, plus a 0424:2514 hub inside it) and these
devices:

| Device | USB ID | Role (inferred) | Link |
|---|---|---|---|
| BaySpec spectrometer #1 | ba8c:1688 | Spectrometer that is used for scans | Vendor control requests + bulk IN 0x82 |
| BaySpec spectrometer #2 | ba8c:1688 | Second channel, initialised but never scanned (IR? 20 s default exposure) | Same |
| Prolific PL2303 | 067b:2303 | I/O board: interlocks/relays, **laser emission line** | Serial 115200 8N1, DTR/RTS off, LF-terminated ASCII, characters sent ~80 ms apart |
| Silicon Labs CP210x | 10c4:ea60 | Laser driver (TEC + current) | Serial 19200 8N1, CR-terminated ASCII |
| Microchip MCP2200 | 04d8:00df | GPIO via HID: **laser arming** pulse + armed status input | HID 16-byte reports (MCP2200 command set) |

## BaySpec (vendor requests, bmRequestType 0xC0 = IN / 0x40 = OUT)

- `0xA1` IN, wIndex = 0x00/0x40/0x80/0xC0, len 64: read the 256-byte EEPROM/config.
  Serial number "99002169" at offset 8. The calibration may be in here (still to check).
- `0xA3` OUT: reset/initialise.
- `0xA4` OUT wValue=0/1 wIndex=0x03F0: set a flag (0 on #1, 1 on #2). `0xA4` IN wIndex=0x03FF reads it back.
- `0xA8` IN wIndex=0x80FF: stalls on #1 and returns 0 on #2 (perhaps a temperature/cooler read).
- `0xAD` OUT wIndex=0x0038: #2 only.
- `0xAE` OUT wValue=8 wIndex=0x30, 8 bytes = `<u32 exposure><u32 param2>`.
  Exposure is in 16 µs units minus 1: 62499 → 1.000 s, 380806 → 6.093 s (matches the measured frame times).
  param2: 200 at reset, 20 with 1 s, 3 with 6 s, 1 with 20 s (meaning unknown, maybe gain or mode).
- `0xAA` OUT wValue=0xFFFF wIndex=0xFFFF: start an exposure. After the exposure time,
  bulk IN ep 0x82 returns **4608 bytes = 2304 big-endian u16**. Then `0xAA` 0/0 ends it.
  About 2068 active pixels; the rest is padding (constant 47288); a few zeros near pixel 2068–2077.
- Each frame is one exposure; averaging and background subtraction are done on the PC.

## I/O board (Prolific, 115200 8N1, DTR/RTS off, "\n" terminated, replies "OK\n")

(The driver opens the port at 1200 7E1 by default; the software switches to 115200 8N1 before its first command.)

- `TECE` at init.
- `IO H nn` / `IO L nn`: set line nn high/low. `IO L 09` = **laser on** (frames taken with 09 low
  are bright; frames with 09 high are the background); `IO H 09` = laser off.
  The init sequence also sets lines 00, 01, 07, 08, 0a–0e. Several of them are power switches:
  each device appears on USB right after its line goes low:
  `IO L 0a` → the inner hub (0.1 s later), `IO L 0c` → spectrometer #1 (0.4 s later), `IO L 0d` → spectrometer #2 (0.5 s later),
  `IO L 07` → laser driver CP210x (0.2 s later).
  After "On" and before "Initialize system", only the hub, the Prolific and the MCP2200 are present.
- `VOL 03` → `VOL:0.09`: polled about every 2 s (some analogue reading).

## Laser driver (CP210x, 19200 8N1, "\r" terminated)

- At init only: `rev?` → `Software Rev=Rev6  Aug 18 2011 17:44:22`, `tecmax 0.6`,
  `temperature 25.00`, `laseroff`, `current 0.00`. This session never sent any other command.
  This may belong to the second (unused) channel.

## MCP2200 (HID)

- READ_ALL (0x80) is polled continuously; byte 10 is the GPIO input port.
- The arm/disarm pulse is SET_CLEAR_OUTPUT (0x08): clear all, then set all about 100 ms later.
  About 5 s after arming, input bit 0 goes 0 (= armed); it goes back to 1 when disarmed.
- The MCP2200's serial side is only probed (`TEC`, `rev?`) and isn't used.

## Session timeline (local time)

09:38 Initialise: I/O board lines, both spectrometers, laser driver off.
09:41:04 Arm pulse → 09:41:09 armed.
09:42:03 IO L 09 (laser on) → 1 s preview + 6 s frame → IO H 09 → 6 s background frame.
09:43:55–09:45:04 Run: laser on, frame, laser off, background frame, laser on, then repeated 6 s frames; IO H 09 at stop.

## Kitchensink capture (2026-11-20, 09:55–10:01)

- **Lasers: 532 nm and 1064 nm.** The `.spc` export comments say `LS WL=532`.
  532 nm uses spectrometer #1 (d15); its laser is switched by `IO L 09` / `IO H 09`.
- **Switch to 1064 nm:** `IO H 0b`, `IO L 07` (powers the CP210x laser driver), `IO L 0b`, then to the
  driver: `rev?`, `tecmax 0.6`, `temperature 25.00`, `laseron`, `current 0.00`; then `IO L 0e`, `IO H 0f`;
  spectrometer #2 (d16, the 1064 channel) is initialised with a 20 s exposure.
  **Back to 532:** `laseroff`, `current 0.00`, `IO H 07`, `IO H 0b`, `IO L 0b`, `IO L 0f`, `IO H 0e`, then d15 is re-initialised.
  No 1064 scan was made, so the real `current` value for emission hasn't been seen yet.
- **Laser power 50 → 51 mW (532):** no command at all on any USB device. The value only appears
  in the `.spc` comment. Either the 532 nm power isn't computer-controlled, or it's set on the instrument itself.
- **Exposure 1.0 → 1.2 s:** 0xAE raw 74999 (confirms 16 µs units, raw = t/16 µs − 1). param2 went 20 → 17;
  param2 ≈ round(20 s / exposure) in every case so far (20, 17, 3), maybe a scans-per-20 s setting.
- **Spectrometer EEPROM** (0xA1): no calibration in it. Offset 0x30 holds the last exposure raw value and param2
  (`f7 24 01 00 11 00 00 00` = 74999, 17). So the calibration must be in the vendor software files.
- **Logout/exit:** `USB` → `USB:00`, then `OFF` to the I/O board.
- **.spc export:** Galactic SPC, 3301 points from 200 to 3500 cm⁻¹ (1 cm⁻¹ steps), float32 Y,
  baseline-corrected (comment `Baseline=1`), comment holds Tint, laser wavelength, power, averaging.

## Calibration (from the vendor's BS_System/BS_Device_Settings.ini)

Wavelength (nm) = c0 + c1·p + c2·p² + c3·p³ for pixel p; Raman shift = 1e7/λ_laser − 1e7/λ.
- 532 nm channel (spectrometer #1): λ_laser 532.100; c = 514.006252760493, 0.096682105209,
  −6.593998e−6, 5.8e−11. This reproduces the stored benzonitrile peaks within 0.6 cm⁻¹
  (pixel 514.9 → 1001.3 cm⁻¹). The range is about −660 to 4220 cm⁻¹ over pixels 0–2067.
- 1064 nm channel (spectrometer #2): λ_laser 1064.010; c = 1067.709823464664, 1.417819808954,
  4.218663e−6, −3.48208e−7 (about 256 pixels).
- Laser power coefficients: 532: 0.289022 + 0.008822·P (max 490 mW); 1064: 0.275808 + 0.002209·P
  (max 450 mW). For 1064 this is probably the driver's `current` (A). For 532 the mechanism is unknown:
  nothing was sent when the power changed.
- Intensity response correction: `NIST_Coeff_00..05` (5th-order polynomial, 195–3500 cm⁻¹).
- Other settings: TEC wait 3 s (532) / 60 s (1064), FWHM 7 / 17, integration cap 200 (= param2 at reset?),
  sleep time 600 s.
- The vendor logs in BS_Record are encrypted.

**Pixel order (verified with water on 2026-11-20):** the frame is in reverse order. Data index i
corresponds to calibration pixel p = 2057 − i. The first ~10 values are dark reference pixels (p > 2047).
With that, water gives the O–H stretch at 3234/3360, the bend at 1646 and the combination band at ~2110 cm⁻¹.

## Linux, first contact (2026-11-20)

The probe found everything, identified by sitting behind the 0424:2517 hub. MCP2200 READ_ALL works over hidraw:
`80 00 00 00 8f 00 ff 00 04 e1 ff …` (GPIO port 0xff, disarmed; baud divisor 1249 = 9600).

## Linux: laser and first spectrum (2026-11-20, 10:39)

- **Arming (verified 2026-11-20 ~11:45):** MCP2200 GPIO (READ_ALL byte 10):
  - **bit 7 = compartment switch** (0 while open);
  - **bit 0 = armed** (0 = armed). Pressing the instrument's **ARM button** sets it to 0 (only with the compartment
    closed). **Opening the compartment clears it** (→ 1), and it stays 1 until ARM is pressed again.
  - The MCP2200 pulse (outputs low then high) does NOT arm (no change in bit 0, no light); its purpose is unknown.
    Keep sending it as the vendor software does.
  - The **red light = laser emitting** (on with `IO L 09` while armed), not "armed".
  - Without arming, `IO L 09` produces no light (first test, 10:29).
  So the software can't arm by itself: a person at the instrument must press ARM after each sample change.
  Software should read bits 0 and 7 and show armed / compartment state.
- The physical interlock cuts the laser when the compartment is open (per the user).
- The vendor initialisation must be completed before the 532 nm laser emits. The full line state after
  vendor init is: TECE; IO H 01, H 00, L 0a, H 0e, L 0c, L 0d, H 0b, H 08, H 07, L 0b
  (line 08 is the prime suspect for the 532 laser supply). `IO L 0d` brings up spectrometer #2.
- With two spectrometers present, pick them by product string: "BS2ACCD" = 532 nm CCD, "BS2BASE" = 1064 nm detector.
- Don't call set_configuration on an already-configured device: it resets the data toggle and the
  first 512-byte packet of the next frame is lost.
- Result: 1 s of water from Linux, correlation 1.000 with the vendor software's frame.

- Shutdown attempt from Linux: `IO H 09`, `USB` (→ `USB:00`), `OFF` (→ `OK`). Both spectrometers disappear from USB;
  the hub, the I/O board and the MCP2200 stay. **But the instrument's onboard computer (Win CE) then showed
  "This program is not responding"** instead of going dark. Don't send OFF until the vendor Shutdown sequence has been captured.
  Recovery: physical power switch off/on, then On. Timing: 1 s frames take 1.01 s; signal is linear
  (1070/1060/1045 counts/s at 0.1/1/5 s); 5 back-to-back frames drift 0.3%.
- 1064 nm channel, decoded from the vendor capture `2026-11-20c/1064.pcapng`:
  - Switch to 1064: `IO H 0b`, `IO L 07` (driver power), `IO L 0b`; driver: `rev?`, `tecmax 0.6`, `temperature 25.00`,
    `laseroff`, `current 0.00`; then `IO L 0e`, `IO H 0f` (beam path); init detector #2 (A3, A4=1, AD 0x38, A4=1);
    the software waits about 60 s (TEC wait).
  - Detector #2 frame = **512 px** (1024 bytes, big-endian u16) on ep 0x82. **Exposure units 0.8 µs**:
    raw 1249975 = 1.0 s (frames arrive 1.0 s after START). So raw ≈ t/0.8 µs − 25.
  - Laser: `laseron`, then `current 0.39` just before each signal frame and `current 0.00` for the background frame;
    at the end `current 0.00`, `laseroff`. 0.39 A = 0.275808 + 0.002209·50 mW (the ini's LS_Pwr_Coeff for 1064), so
    **1064 power is set by the driver current**, I = c0 + c1·P.
  - Back to 532: `IO H 07`, `IO H 0b`, `IO L 0b`, `IO L 0f`, `IO H 0e`, re-init detector #1.
  - Vendor Shutdown = `USB` → `USB:00`, then `OFF`, exactly what we sent from Linux.
  - Export: 2201 points, 100–2300 cm⁻¹, `LS WL=1064, LS Pwr=50mW`.

## 532 nm power (vendor capture `2026-11-20d/power.pcapng`, 12:02–12:05)

Scans at 50, 300, then 50 mW (ZnS): **no power-related command on any device**, and the signal went
10 800 → 6 500 → 5 200 counts (ZnS luminescence fading, not following the setting). So the 532 nm power
is **not computer-controlled**; the vendor software only writes the value into the file. Our software
records it as metadata. (Expert view, András: such instruments typically run the laser at full power and set
lower powers with an aperture/stop; without a computer-controlled aperture only exposure time and pre-bleaching
remain as software controls.)

## Standards measured with the engine (2026-11-20, 532 nm, 3 × 1 s)

| Sample | Measured (cm⁻¹) | Literature |
|---|---|---|
| CCl₄ | 221, 316, 461, 787 | 218, 314, 459, 762/790 |
| CHCl₃ | 262, 367, 668, 761, 1217, 3021 | 261, 364, 667, 760, 1216, 3019 |
| Water | 1646, 3234/3360 | ~1640, ~3230/3400 |

The factory calibration (2017) holds within 1–3 cm⁻¹ from 260 to 3020 cm⁻¹, with a roughly uniform +2 cm⁻¹ offset
(could be corrected by adjusting the laser wavelength slightly).

## 1064 nm channel: probably faulty hardware

CCl₄ at 200 mW, 3 × 5 s showed **no** Raman bands (not even 459 cm⁻¹), only a uniform rise of ~290 counts.
The vendor software's 1064 nm export of water is also flat noise, and the user reports no 1064 signal in the
vendor software either. Suspect the laser, fibre/beam path or driver. Decisive test: vendor software, CCl₄,
1064 nm, maximum power, 10 s. Our switching sequence matches the vendor's exactly.

## Shutdown behaviour

Sending `USB`, `OFF` from Linux (engine, 30 ms pacing, no gap) shut the instrument down, but its onboard
computer first spent a while "killing" a program, which isn't normal (with the vendor software it goes dark
promptly). The vendor capture shows the same two commands, so the difference is timing or state. The engine
now uses the vendor timing (80 ms/char, VOL query, 0.4 s gap); to be verified.

## macOS (2026-11-20, Intel Mac, macOS 12.7.6)

The packaged app (CI build, Qt 6.9, `LibreRamen-macOS-Intel.dmg`) works with the instrument: macOS's own drivers
provide `/dev/cu.usbserial-*` (Prolific I/O board) and `/dev/cu.usbmodem*` (MCP2200 CDC); the MCP2200 HID
side is accessed via IOKit (output reports with IOHIDDeviceSetReport, input reports via a CFRunLoop thread);
the spectrometers via the bundled libusb (pip `libusb-package`). Status, initialisation, dark frames and a
532 nm CCl₄ measurement gave the same results as on Linux.

## Windows (caesium, Windows 11, 2026-11-20)

- MCP2200: CDC COM port + HID, works with Windows' own drivers.
- Prolific PL2303HXA: Prolific's current driver refuses the chip ("PL2303HXA PHASED OUT SINCE 2012"); writes to
  the COM port block. LibreRamen drives the chip itself over libusb (`hardware.Pl2303Usb`) once WinUSB is bound
  to it (Zadig), the same driver the spectrometers need.
- BaySpec spectrometers: no driver ("Error") until WinUSB is installed (Zadig).
- Bug found: `signal.SIGHUP` doesn't exist on Windows (every CLI command crashed; fixed).

## Still unknown

- Wavelength calibration: not in the EEPROM; look in the vendor software folders ("Anton Paar", "Anton PaariNST").
- (Solved: 1064 power = driver current; 532 power is not settable from the PC.)
- param2 of 0xAE; the roles of the other I/O lines and of the `VOL` reading.
