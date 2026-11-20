#pragma once
// 1064 nm laser driver (Silicon Labs CP210x; Python: hardware.LaserDriver; PROTOCOL.md "Laser driver").
// CR-terminated ASCII at 19200 8N1. Commands: rev?, tecmax, temperature, laseron, laseroff, current <A>.
//
// Thread safety: internal mutex (the Stop request and the watchdog may switch it off from another thread).
#include <QString>
#include <memory>
#include <mutex>

#include "seriallink.h"

namespace lr::hw {

class LaserDriver {
public:
    explicit LaserDriver(std::unique_ptr<SerialLink> link);   // DTR/RTS off, 0.2 s, flush input
    static std::unique_ptr<LaserDriver> open(const QString& port);   // 19200 8N1

    QString command(const QString& text);     // text + "\r", 0.12 s, read up to 200 bytes (0.3 s)
    QString init();                           // "", rev?, tecmax 0.6, temperature 25.00, laseroff, current 0.00
    void setCurrent(double amps);             // "current %.2f"
    void on();                                // laseron
    void off();                               // current 0.00, laseroff
    QString portName() const;
    void close();

private:
    std::unique_ptr<SerialLink> s_;
    std::mutex m_;
};

}  // namespace lr::hw
