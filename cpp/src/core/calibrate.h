#pragma once
// Calibration against reference standards (Python: calibrate.py): peak finding, matching to the bands of
// standards.h, weighted polynomial fit, one-point (offset) calibration, storage as JSON compatible with
// the Python version (<config>/libreramen/calibration_<serial>_<channel>.json).
//
// Only the part used by the instrument is fixed here; the rest of the API is defined by the port.
#include <QString>
#include <optional>

#include "instrument.h"

namespace lr::calib {

QString configDir();              // $XDG_CONFIG_HOME/libreramen or ~/.config/libreramen (Windows/macOS: same
                                  // relative location as the Python version uses, for shared files)
std::optional<UserCalibration> load(int channel, const QString& serial, const QString& path = {});

}  // namespace lr::calib
