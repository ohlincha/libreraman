---
type: File format
title: LibreRamen spectrum CSV
description: The files written for every measured spectrum.
resource: cpp/src/core/spectrum.cpp
tags: [format]
timestamp: 2026-11-23
---
# Spectrum CSV

Saved to `~/Documents/LibreRamen/<yyyy-mm-dd>/<sample>_<yyyymmdd_hhmmss>.csv`; a series is a folder with one CSV
per spectrum plus `series.csv`.

Header lines `# key: value`, e.g. `channel: 532 nm`, `exposure_s`, `averages`, `laser_nm`, `calibration`,
`serial` (instrument, or `SIMULATOR`), `power`, `sample`. Columns: `raman_shift_cm1,intensity,signal,background`
(intensity = signal − background, per frame), ascending shift, full detector including anti-Stokes.

Known gap: the sample name defaults to "sample" (issue #7).
