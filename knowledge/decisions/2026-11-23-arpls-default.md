---
type: Decision
title: arPLS (strength 6) is the default baseline
timestamp: 2026-11-23
tags: [decision, processing]
supersedes: [2026-11-23-snip4-default.md, 2026-11-23-snip4-matching.md]
---
# arPLS default (Measure, Data, library matching and peak tables)

The snip4 decision was based on the residual *between* bands (fluorescence removed), not on what happens
*under* the bands. Measured that way (user reports, 2026-11-23: ZnS and CCl4 looked wrong):

- Synthetic ZnS (bands 275/350/670 on a dome 4.5x the strongest band, known background; 14 variants of dome
  height/width, band width, noise): arPLS 5–6 band areas within ~±10 % (670: up to −25 %); snip4 +11 to +248 %.
  arPLS 7.5 fails on narrow strong domes (+800 to +2000 %).
- Recorded CCl4 (simulator, no fluorescence): baseline under the 218 cm-1 band, % of band height: snip4 26.8,
  ALS 6.8, SNIP/airPLS 3.6, polynomial(6) 3.0, arPLS 0.9, rolling ball 0.1.
- Polynomial: order 1 (straight line) is as good as arPLS on CCl4; every order fails on ZnS.
- Library matching (demo library, noisy simulator spectra ± fluorescence dome): arPLS 5 drops water to HQI 0.63
  (broad bands); arPLS 6 all correct, water 0.91, ZnS 0.92 (snip4: 0.93/0.85).

Strength 6 everywhere. Polynomial's control is its order (order = strength + 1; 1 = straight line). Peak-table
marker in libraries: "arpls:6" (re-picks tables from snip/snip4). Regression test: test_processing
baselineKeepsBands.
