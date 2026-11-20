#pragma once
// The Cora's I/O board (Prolific PL2303; Python: hardware.IOBoard, PROTOCOL.md "I/O board").
// ASCII commands, LF-terminated, sent one character at a time (pacing), answered with "...OK\n".
// 115200 8N1, DTR/RTS off. Line 09 is the 532 nm laser emission line (L = on, H = off).
//
// Thread safety: command() is serialised by an internal mutex, so the laser-off command of a Stop request
// (another thread) is never interleaved character by character with a running command.
#include <QByteArray>
#include <QString>
#include <map>
#include <memory>
#include <mutex>
#include <optional>

#include "seriallink.h"

namespace lr::hw {

inline const QString LASER_LINE = QStringLiteral("09");

class IOBoard {
public:
    // Takes an open link (openSerialPort(port, 115200) or a Pl2303Usb): DTR/RTS off, 0.1 s, flush input.
    explicit IOBoard(std::unique_ptr<SerialLink> link, double pacing_s = 0.03);
    // Opens the serial port at 115200 8N1.
    static std::unique_ptr<IOBoard> open(const QString& port, double pacing_s = 0.03);

    // Sends text + "\n" one character per pacing interval; waits up to timeout_s for "OK\n".
    // Returns the reply without "OK", stripped. Throws HardwareError if not confirmed.
    QString command(const QString& text, double timeout_s = 3.0);
    // "IO H nn" / "IO L nn". Line 09 (laser) only with allowLaser (Cora's laser guard, laser-off paths).
    void setLine(const QString& line, bool high, bool allowLaser = false);
    std::optional<double> vol();              // "VOL 03" -> "VOL:0.09" (harmless query; keep-alive, probe)
    void setPacing(double s);
    double pacing() const;
    std::map<QString, QChar> lines() const;   // last state we set: line -> 'H'/'L'
    QString portName() const;
    void close();

private:
    std::unique_ptr<SerialLink> s_;
    double pacing_;
    std::map<QString, QChar> lines_;
    mutable std::mutex m_;
};

}  // namespace lr::hw
