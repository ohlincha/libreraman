---
type: Method
title: Baseline correction
description: Methods, the 150 cm-1 fit range, and why arPLS (strength 6) is the default.
resource: cpp/src/core/processing.cpp
tags: [processing]
timestamp: 2026-11-23
---
# Baselines

Methods (`processing::baselines()`): none, als, arpls, airpls, polynomial, rolling_ball, snip, snip4. Strength
0–10 maps to each method's main parameter (λ = 10^(2+0.6 s) for the penalised ones, 10 + 15 s iterations for
SNIP).

- **Fit range:** only x ≥ fitMin is fitted (default `kBaselineFitMin` = 150 cm⁻¹, the 532 nm edge filter; Data
  page *fit from*, API `fit_min`; 0 for imported spectra with bands lower down); below it the baseline equals
  the data. Including the edge-filter
  cliff dragged every method down beside low-wavenumber bands (ZnS 256/348 cm⁻¹), 10–30× worse (issue #1).
- **snip4** (SNIP, 4th-order clipping filter, decreasing window, Morháč) follows dome-shaped fluorescence;
  classic SNIP cuts into it (~15000 counts residual on the simulator ZnS spectrum). snip4 is the Measure
  default ([decision](../decisions/2026-11-23-snip4-default.md)).
- Data page: own method drop-down, default *as Measure page*.
- Fluorescence indicator: median snip4 baseline / strongest band over 150–3500 cm⁻¹ (ZnS 5.7, ethanol 0.02);
  noted on the Measure dashboard at ≥ 1 with background > 1000 counts.
- Library matching (full-spectrum HQI and peak tables) uses snip4 too (issue #4). Simulator test: with a
  fluorescence dome as high as the strongest band, classic SNIP matched every sample as ZnS; snip4 found all.
  On clean noisy spectra snip4 scores are lower (water ~0.93, ZnS ~0.85 vs ~0.99) but still far ahead.
  Libraries record `peak_baseline` in their info table; older automatic peak tables are re-picked once.

## 2026-11-23: default changed to arPLS 6

snip4 inflates bands on fluorescent samples and cuts into low bands of clean ones (CCl4 218 cm-1: 27 %).
arPLS 6 keeps bands within a few % on both and matches the library correctly; see
[decision](../decisions/2026-11-23-arpls-default.md). The statements above about snip4 being the default are
superseded.
