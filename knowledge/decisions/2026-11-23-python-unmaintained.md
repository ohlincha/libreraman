---
type: Decision
title: The Python package is no longer maintained
timestamp: 2026-11-23
tags: [decision]
---
# Python package unmaintained

The product is the C++ app. `libreraman/` (Python) is frozen: not maintained, not shipped. The user does not
care about it (2026-11-23, issue #8 closed). The existing golden files stay as they are; new behaviour is tested
in C++ only. The package may be deleted later; nothing in the C++ app depends on it.
