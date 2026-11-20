#pragma once
// Exceptions of the engine (Python: instrument.CoraError, NotArmed, hardware.HardwareError).
#include <stdexcept>
#include <string>

namespace lr {

struct Error : std::runtime_error {                // CoraError
    using std::runtime_error::runtime_error;
};
struct NotArmed : Error {                          // laser refused: not armed / compartment open
    using Error::Error;
};
struct HardwareError : Error {                     // a device did not answer as expected
    using Error::Error;
};
struct Stopped : Error {                           // the user pressed Stop
    Stopped() : Error("stopped") {}
};

}  // namespace lr
