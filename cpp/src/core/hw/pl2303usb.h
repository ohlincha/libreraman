#pragma once
// Minimal user-space driver for the I/O board's Prolific PL2303 (HXA) over libusb (Python: hardware.Pl2303Usb).
//
// Windows' current Prolific driver refuses this chip ("PL2303HXA PHASED OUT SINCE 2012"). With WinUSB bound
// to it (LibreRaman-Setup / Zadig) we talk to it directly, using the requests the vendor's driver sent in the
// captures: vendor set-up sequence, CDC line coding (115200 8N1) and control lines (DTR/RTS off),
// bulk OUT 0x02 / IN 0x83. Compiles on every platform; Cora::connect() uses it on Windows only.
//
// Thread safety: as SerialLink (serialised by IOBoard).
#include <memory>
#include <vector>

#include "seriallink.h"
#include "usb.h"

namespace lr::hw {

class Pl2303Usb : public SerialLink {
public:
    explicit Pl2303Usb(std::unique_ptr<UsbHandle> dev, int baud = 115200);   // throws HardwareError
    // Every 067b:2303 that can be opened and set up (others, e.g. still bound to Prolific's driver, skipped).
    static std::vector<std::unique_ptr<Pl2303Usb>> findAll();

    QString name() const override { return port_; }       // "usb:<bus>-<address>"
    void write(const QByteArray& data, int timeoutMs) override;   // bulk OUT 0x02, 2 s
    QByteArray read(int n, int timeoutMs) override;               // bulk IN 0x83, 64-byte reads, buffered
    void resetInput() override;
    void close() override;

private:
    std::unique_ptr<UsbHandle> dev_;
    QString port_;
    QByteArray buf_;
};

}  // namespace lr::hw
