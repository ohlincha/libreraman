#pragma once
// Thin libusb-1.0 wrapper with the semantics of the pyusb calls used by hardware.py
// (ctrl_transfer, read/write on endpoints, get_active_configuration / set_configuration, get_string).
//
// Thread safety: libusb itself is thread-safe; one UsbHandle must be used by one thread at a time
// (the owners - Spectrometer, Pl2303Usb - are serialised by the instrument's lock).
#include <QByteArray>
#include <QString>
#include <cstdint>
#include <memory>
#include <vector>

struct libusb_context;
struct libusb_device;
struct libusb_device_handle;

namespace lr::hw {

libusb_context* usbContext();                 // shared context, created on first use; throws HardwareError
QString usbErrorName(int rc);

class UsbHandle {
public:
    // Opens dev (takes a reference). Throws HardwareError if it can't be opened (e.g. no WinUSB driver).
    explicit UsbHandle(libusb_device* dev);
    ~UsbHandle();
    UsbHandle(const UsbHandle&) = delete;
    UsbHandle& operator=(const UsbHandle&) = delete;

    // All devices with this VID/PID (each a new reference; release with libusb_unref_device or adopt).
    static std::vector<libusb_device*> list(uint16_t vid, uint16_t pid);
    static void unref(const std::vector<libusb_device*>& devs);

    int bus() const;
    int address() const;
    QString productString();                  // usb.util.get_string(d, d.iProduct) (first language ID)

    // pyusb: try get_active_configuration(); on failure set_configuration() (the first configuration).
    // An active configuration is never set again: that resets the data toggle (lost first packet).
    void ensureConfigured();
    // pyusb claims the interface lazily before the first transfer that needs it (bulk, class requests).
    void claimInterfaceOf(uint8_t endpoint);
    void claimInterface(int iface);

    // bmRequestType 0x40/0x21 (OUT) with optional data; returns bytes transferred. Throws HardwareError.
    int ctrlOut(uint8_t reqType, uint8_t req, uint16_t value, uint16_t index, const QByteArray& data,
                unsigned timeoutMs);
    QByteArray ctrlIn(uint8_t reqType, uint8_t req, uint16_t value, uint16_t index, uint16_t length,
                      unsigned timeoutMs);
    // Bulk transfers. read(): returns the data; on timeout returns what arrived if timeoutIsError is false,
    // else throws HardwareError ("timed out"). write(): throws on error.
    QByteArray bulkRead(uint8_t ep, int length, unsigned timeoutMs, bool timeoutIsError = true);
    int bulkWrite(uint8_t ep, const QByteArray& data, unsigned timeoutMs);

    libusb_device_handle* raw() const { return h_; }

private:
    libusb_device* dev_ = nullptr;
    libusb_device_handle* h_ = nullptr;
    std::vector<int> claimed_;
};

}  // namespace lr::hw
