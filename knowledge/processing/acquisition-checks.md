---
type: Method
title: Acquisition checks (laser damage, S/N, repeats)
description: Laser-damage check, acquire-until-S/N stop rule and repeated-measurement statistics; default thresholds and why.
resource: cpp/src/core/acqcheck.cpp, cpp/src/core/repeats.cpp
tags: [processing, acquisition, defaults]
timestamp: 2026-11-25
---
# Acquisition checks

Issues #9 (laser damage), #10 (acquire until S/N), #11 (repeated measurements of one vial). Logic in
`core/acqcheck.*` and `core/repeats.*` (unit tests `test_acqcheck`, `test_repeats`); GUI: dashboard *Laser* line
(only when the check fires), **Measure** menu → *Acquire until S/N…* / *Repeated measurements…* (own windows; the
Measure page gets no new controls — user's requirement, these are niche).

**All thresholds are first guesses, tested only on synthetic spectra and the simulator, NOT on the real Cora 5000.**
Revisit them with real black samples (graphite, LiFePO₄, NMC, paper separators). They are constants (not in the UI).

## Instrument facts behind the design

- Laser power on the Cora cannot be varied at all (not even by hand in a useful way), so the advice is
  *shorten the exposure or defocus slightly (distance knob)*, never "lower the power".
- The anti-Stokes side is blocked by the edge filter: checked on the real ethanol/polystyrene spectra
  (`cpp/tests/golden/cora`): no anti-Stokes bands where ~250/330 counts were expected. So sample temperature
  cannot be measured from the Stokes/anti-Stokes ratio; heating can only be inferred from changes over time.

## Laser damage (#9)

Applies to every multi-scan acquisition (`AcquireOptions::frames` returns the individual signal frames) and to
*until S/N* runs. Mean of the first third of the frames vs the last third (block-aligned `FrameStore`, bounded
memory), range 150–3500 cm⁻¹, despiked, SNIP-4 background (one common background under the bands, the
background level separately). The strongest 3 bands with height ≥ 10 × noise are compared.

| Metric | Default | Reasoning |
|---|---|---|
| band shift (centroid over the upper half of the band) | > 2.5 cm⁻¹, and > 4 σ of the position, and > 3 % of the FWHM | ~1 detector pixel (≈2.4 cm⁻¹/px). Graphite G moves ≈ −0.02…−0.03 cm⁻¹/K, so ~100 K of heating; the σ and FWHM terms keep broad bands (water OH, amorphous carbon) and weak bands quiet |
| loss of summed band height | > 15 %, and > 4 σ | photo-/thermal degradation and LiFePO₄ → hematite reduce the original bands; 15 % is above packing/focus drift within one acquisition |
| background rise | > 20 % of max(early background, strongest band), and > 4 σ | heating glow / burning raises a broad background; a *falling* background (photobleaching of fluorescence) is not flagged |

Result in the file: `laser_damage: suspected | none detected`, `laser_damage_check: band 1582 → 1577.5 cm⁻¹ ·
bands −22 % · background +35 % (first 4 vs last 4 frames)`. Simulator check over ~200 runs (all recorded samples,
6–30 frames, 0.2–1 s): one false alarm (ZnS 350 cm⁻¹ band, 2 frames per third at 0.2 s). With few or very short
frames the check is weak; it needs ≥ 2 frames (≥ 6 in *until S/N* before it may stop the run).

## Acquire until S/N (#10)

S/N = band height above a SNIP-4 background / robust per-point noise (MAD of the second difference / √6); band =
strongest in 150–3500 cm⁻¹ or the maximum within ±15 cm⁻¹ of a chosen position. The background (laser off) is taken
once with up to 8 frames (≤ ¼ of the time limit); its noise is in every frame, so S/N eventually levels off and the
time limit ends the run. Stop rule after each frame: damage (≥ 6 frames) → target reached (≥ 3 frames) → time
limit (next frame would end after it). Defaults: target 100, limit 10 min, stop on damage on. Expected frames =
n·(target/S/N)², shown as a guide.

## Repeated measurements (#11)

Mean and point-wise sd over the included spectra; bands = the 6 strongest of the mean (prominence ≥ 5 % of the
strongest), measured in each spectrum within ±12 cm⁻¹: position, height, ratio to the strongest band (same
spectrum, so packing cancels), each mean ± sample sd. Outliers by shape: unit-norm spectra vs their point-wise
median, distance = |s − median| / |median|; flagged when the modified z-score (0.6745·(d − med)/MAD) > 3.5
(Iglewicz & Hoaglin) **and** d > 0.10 (smaller differences are noise or packing). Needs ≥ 3 spectra. Flagged
spectra stay included until the user unticks them. Files: each spectrum (`repeat: k/N`, `repeat_group`),
`<sample>_mean_<time>.csv`, `<sample>_repeats_<time>.csv`.
