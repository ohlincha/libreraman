---
type: Runbook
title: Building and testing
description: Rootless C++ toolchain on the development desktop, tests, golden files, previews.
tags: [build, tests]
timestamp: 2026-11-23
---
# Build and test

The C++/Qt6 app (`cpp/`) builds without system Qt packages:

```
source ~/opt/libreramen-env.sh            # Qt 6.8.3 (aqtinstall) + ~/opt/sysroot
cmake -S cpp -B ~/opt/build-<topic> -G Ninja -DCMAKE_BUILD_TYPE=Release $LR_CMAKE_ARGS
nice cmake --build ~/opt/build-<topic> -j4
QT_QPA_PLATFORM=offscreen ctest --test-dir ~/opt/build-<topic> -j3
```

- Screenshots: configure with `-DLIBRERAMEN_GUI_PREVIEW=ON`, run `gui_preview`, `datapage_preview`,
  `calibration_preview` offscreen with an output folder (they use a temporary config dir).
- Golden files (`cpp/tests/golden/`) are generated from the Python package:
  `PYTHONPATH=<repo> python3 cpp/tests/golden/make_golden.py`; revert the date-only change in
  `calibration_99002169_532.json`. See [decision](../decisions/2026-11-23-python-unmaintained.md).
- CI: `.github/workflows/build.yml` (installers, Qt 6.8.3) and `cpp.yml`.
- Don't scan the whole home directory: `find` is `bfs` here and a stray one starved a dpkg install.
