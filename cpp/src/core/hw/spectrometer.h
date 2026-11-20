#pragma once
// BaySpec USB spectrometers ba8c:1688 (Python: hardware.Spectrometer; PROTOCOL.md "BaySpec").
// "BS2ACCD" = 532 nm CCD (frame 4608 bytes = 2304 big-endian u16), "BS2BASE" = 1064 nm detector (1024 bytes).
// Vendor requests (bmRequestType 0x40 OUT / 0xC0 IN): A1 EEPROM, A3 init, A4 flag, AD (1064 only),
// AE exposure, AA start/stop; frame on bulk IN 0x82.
// An already active USB configuration is never set again (that drops the next frame's first packet).
//
// Thread safety: none; the instrument serialises access with its lock.
#include <QByteArray>
#include <QString>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "usb.h"

namespace lr::hw {

class Spectrometer {
public:
    Spectrometer(std::unique_ptr<UsbHandle> dev, int channel);
    // The spectrometer for channel 532/1064 (by product string), polling every 0.25 s up to timeout_s;
    // nullptr if not found.
    static std::unique_ptr<Spectrometer> find(int channel, double timeout_s = 0.0);
    static QString productName(int channel);  // "BS2ACCD" / "BS2BASE"

    int channel() const { return channel_; }
    QByteArray eeprom();                      // 256 bytes (A1 at 0x00/0x40/0x80/0xC0)
    QString serial();                         // EEPROM[8:16]
    void init();
    void setExposure(double seconds);
    std::optional<double> exposure() const { return exposure_; }
    // Exposure register values (AE request) for a channel: <u32 raw><u32 param2>.
    static std::pair<quint32, quint32> exposureRaw(int channel, double seconds);
    std::vector<double> frame();              // one exposure: active pixels in data order (raw counts)
    std::vector<double> pixelNumbers() const; // calibration pixel for each value of frame()
    static std::vector<double> pixelNumbers(int channel);

private:
    void out(uint8_t req, uint16_t value = 0, uint16_t index = 0, const QByteArray& data = {});
    QByteArray in(uint8_t req, uint16_t index, uint16_t length);
    std::unique_ptr<UsbHandle> dev_;
    int channel_;
    std::optional<double> exposure_;
};

}  // namespace lr::hw
