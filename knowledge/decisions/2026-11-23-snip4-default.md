---
type: Decision
title: SNIP 4th order is the default baseline
timestamp: 2026-11-23
tags: [decision, processing]
---
# SNIP 4th order default

Classic SNIP failed on fluorescent samples (ZnS); snip4 fixes that and leaves ethanol/CCl₄ unchanged, keeping
SNIP's robustness for series. ALS is equally clean but needs strength tuning more often. See
[baselines](../processing/baselines.md), issue #1.
