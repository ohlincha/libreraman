---
type: Method
title: Intensity (instrument-response) correction
description: Correcting the spectral sensitivity with a reference of known emission curve.
resource: cpp/src/core/response.cpp
tags: [processing, calibration]
timestamp: 2026-11-23
---
# Intensity response

Correction factor F = true / measured (smoothed, median 1 over the valid range, range trimmed by half the
smoothing window); corrected = raw × F. Raw data are never changed; Measure and Data apply it when *intensity
correction* is ticked (issue #16).

- **References:** NIST SRM 2242 glass (certified polynomial in shift/1000 cm⁻¹; the factory file's
  `NIST_Coeff_00..05` are exactly these certificate values, identical for both channels, i.e. the reference's
  true curve and *not* this instrument's response); tungsten-halogen lamp as a blackbody (photon flux per unit
  wavenumber); a table file.
- **Checks:** refuse saturated or S/N < 50 references; warn if F varies > 30× or covers < 1500 cm⁻¹.
- **Files:** `~/.config/libreraman/response_<serial>_<ch>.json` (active) plus dated history copies; the
  reference spectrum in `calibration-spectra/`. Spectra carry `# serial:` so files are matched to their instrument.
- Unknown (2026-11-23): whether the lab has an SRM 2242 vial. Follow-up: anti-Stokes/Stokes temperature.
