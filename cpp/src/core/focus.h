#pragma once
// Focus mode: how much Raman signal a spectrum has, so that the sample distance (the instrument's distance knob)
// can be turned to the maximum while spectra come in continuously. Background removed by SNIP (fluorescence and
// the dark level do not count), over 200-3200 cm-1 (not the laser line, not the detector's ends).
#include "spectrum.h"

namespace lr::focus {

inline constexpr double kMin = 200, kMax = 3200;   // cm-1

struct Signal {
    double band = 0;          // height of the strongest band above the background (counts per frame)
    double bandShift = 0;     // where it is (cm-1)
    double total = 0;         // Raman signal above the background summed over the range (counts·cm-1)
    double rawMax = 0;        // highest raw count (NaN if the spectrum has no raw signal)
    bool saturated = false;   // rawMax at the detector's limit: shorten the exposure
};

// follow > 0: the band nearest this shift (within 15 cm-1) instead of the strongest.
Signal measure(const Spectrum& sp, double follow = 0);

}  // namespace lr::focus
