---
type: Decision
title: The Python package is no longer maintained
timestamp: 2026-11-23
tags: [decision]
---
# Python package unmaintained

The product is the C++ app. `libreramen/` (Python) stays only as the generator of the C++ golden files; whether
to freeze those and test new behaviour in C++ only is open (issue #8). On 2026-11-23 `processing.py` was still
extended (snip4, airpls, fit range, fluorescence ratio) to regenerate the goldens.
