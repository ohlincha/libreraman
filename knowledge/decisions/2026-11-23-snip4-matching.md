---
type: Decision
title: Library matching uses SNIP 4th order
timestamp: 2026-11-23
tags: [decision, processing, library]
---
# snip4 for library matching

Issue #4. Matching (Library page default, `match::Preprocess`, `match::pickPeaks`) and the automatic peak
tables of library entries use snip4, like the Measure page. No separate setting: one baseline everywhere.
Evidence and caveats in [baselines](../processing/baselines.md). Old libraries: automatic peak tables (those
identical to the old classic-SNIP picking, no labels) are re-picked once when opened read-write; the info key
`peak_baseline` records the method, so the schema version stays 1 and older LibreRamen still opens the file.
