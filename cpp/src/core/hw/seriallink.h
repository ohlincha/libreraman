#pragma once
// Byte transport for the ASCII devices (I/O board, 1064 nm laser driver). Implementations:
//   * QtSerialLink - a serial port (QSerialPort) - Linux/macOS, and COM ports on Windows;
//   * Pl2303Usb    - the I/O board's PL2303 driven over libusb (Windows, pl2303usb.h);
//   * fakes in the unit tests.
// The calls mirror the small part of pyserial that hardware.py uses.
//
// Thread safety: a SerialLink is not internally synchronised; its owner (IOBoard, LaserDriver) serialises
// all calls. QtSerialLink may be called from any thread (the port lives in a private thread).
#include <QByteArray>
#include <QString>
#include <memory>

namespace lr::hw {

class SerialLink {
public:
    virtual ~SerialLink() = default;
    virtual QString name() const = 0;                     // port name ("/dev/ttyUSB0", "COM3", "usb:1-4")
    // write() + flush(): returns when the data has been handed to the device. timeoutMs < 0: no limit.
    virtual void write(const QByteArray& data, int timeoutMs) = 0;
    // pyserial read(n) with a timeout: up to n bytes; returns early only when n bytes have arrived.
    virtual QByteArray read(int n, int timeoutMs) = 0;
    virtual void resetInput() = 0;                        // reset_input_buffer()
    virtual void setDtr(bool) {}
    virtual void setRts(bool) {}
    virtual void close() = 0;
};

// Opens a serial port 8N1 without flow control. Throws HardwareError.
std::unique_ptr<SerialLink> openSerialPort(const QString& port, int baud);

}  // namespace lr::hw
