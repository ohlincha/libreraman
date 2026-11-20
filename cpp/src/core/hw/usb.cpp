#include "usb.h"

#if __has_include(<libusb.h>)
#include <libusb.h>
#else
#include <libusb-1.0/libusb.h>
#endif

#include <algorithm>
#include <mutex>

#include "../errors.h"

namespace lr::hw {

libusb_context* usbContext() {
    static std::once_flag once;
    static libusb_context* ctx = nullptr;
    static int rc = 0;
    std::call_once(once, [] { rc = libusb_init(&ctx); });
    if (rc != 0 || !ctx) throw HardwareError("libusb could not be initialised: " + usbErrorName(rc).toStdString());
    return ctx;
}

QString usbErrorName(int rc) { return QString::fromLatin1(libusb_error_name(rc)); }

UsbHandle::UsbHandle(libusb_device* dev) : dev_(libusb_ref_device(dev)) {
    const int rc = libusb_open(dev_, &h_);
    if (rc != 0) {
        libusb_unref_device(dev_);
        dev_ = nullptr;
        h_ = nullptr;
        throw HardwareError("cannot open USB device: " + usbErrorName(rc).toStdString());
    }
}

UsbHandle::~UsbHandle() {
    if (h_) {
        for (int i : claimed_) libusb_release_interface(h_, i);
        libusb_close(h_);
    }
    if (dev_) libusb_unref_device(dev_);
}

std::vector<libusb_device*> UsbHandle::list(uint16_t vid, uint16_t pid) {
    std::vector<libusb_device*> out;
    libusb_device** devs = nullptr;
    const auto n = libusb_get_device_list(usbContext(), &devs);
    for (decltype(+n) i = 0; i < n; ++i) {
        libusb_device_descriptor d{};
        if (libusb_get_device_descriptor(devs[i], &d) == 0 && d.idVendor == vid && d.idProduct == pid)
            out.push_back(libusb_ref_device(devs[i]));
    }
    if (devs) libusb_free_device_list(devs, 1);
    return out;
}

void UsbHandle::unref(const std::vector<libusb_device*>& devs) {
    for (auto* d : devs) libusb_unref_device(d);
}

int UsbHandle::bus() const { return libusb_get_bus_number(dev_); }
int UsbHandle::address() const { return libusb_get_device_address(dev_); }

QString UsbHandle::productString() {
    libusb_device_descriptor d{};
    if (libusb_get_device_descriptor(dev_, &d) != 0 || d.iProduct == 0)
        throw HardwareError("no product string");
    unsigned char buf[256];
    const int n = libusb_get_string_descriptor_ascii(h_, d.iProduct, buf, sizeof buf);
    if (n < 0) throw HardwareError("cannot read the product string: " + usbErrorName(n).toStdString());
    return QString::fromLatin1(reinterpret_cast<const char*>(buf), n);
}

void UsbHandle::ensureConfigured() {
    int cfg = 0;
    if (libusb_get_configuration(h_, &cfg) == 0 && cfg != 0) return;     // active: leave it alone
    libusb_config_descriptor* c = nullptr;
    int value = 1;
    if (libusb_get_config_descriptor(dev_, 0, &c) == 0 && c) {
        value = c->bConfigurationValue;
        libusb_free_config_descriptor(c);
    }
    const int rc = libusb_set_configuration(h_, value);
    if (rc != 0) throw HardwareError("cannot configure the USB device: " + usbErrorName(rc).toStdString());
}

void UsbHandle::claimInterface(int iface) {
    if (std::find(claimed_.begin(), claimed_.end(), iface) != claimed_.end()) return;
    const int rc = libusb_claim_interface(h_, iface);
    if (rc != 0) throw HardwareError("cannot claim the USB interface: " + usbErrorName(rc).toStdString());
    claimed_.push_back(iface);
}

void UsbHandle::claimInterfaceOf(uint8_t endpoint) {
    int iface = 0;
    libusb_config_descriptor* c = nullptr;
    if (libusb_get_active_config_descriptor(dev_, &c) == 0 && c) {
        for (int i = 0; i < c->bNumInterfaces; ++i)
            for (int a = 0; a < c->interface[i].num_altsetting; ++a) {
                const auto& alt = c->interface[i].altsetting[a];
                for (int e = 0; e < alt.bNumEndpoints; ++e)
                    if (alt.endpoint[e].bEndpointAddress == endpoint) iface = alt.bInterfaceNumber;
            }
        libusb_free_config_descriptor(c);
    }
    claimInterface(iface);
}

int UsbHandle::ctrlOut(uint8_t reqType, uint8_t req, uint16_t value, uint16_t index, const QByteArray& data,
                       unsigned timeoutMs) {
    QByteArray d = data;
    const int rc = libusb_control_transfer(h_, reqType, req, value, index,
                                           reinterpret_cast<unsigned char*>(d.data()),
                                           static_cast<uint16_t>(d.size()), timeoutMs);
    if (rc < 0)
        throw HardwareError(QString("USB request 0x%1 failed: %2").arg(req, 2, 16, QChar('0'))
                                .arg(usbErrorName(rc)).toStdString());
    return rc;
}

QByteArray UsbHandle::ctrlIn(uint8_t reqType, uint8_t req, uint16_t value, uint16_t index, uint16_t length,
                             unsigned timeoutMs) {
    QByteArray buf(length, '\0');
    const int rc = libusb_control_transfer(h_, reqType, req, value, index,
                                           reinterpret_cast<unsigned char*>(buf.data()), length, timeoutMs);
    if (rc < 0)
        throw HardwareError(QString("USB request 0x%1 failed: %2").arg(req, 2, 16, QChar('0'))
                                .arg(usbErrorName(rc)).toStdString());
    buf.truncate(rc);
    return buf;
}

QByteArray UsbHandle::bulkRead(uint8_t ep, int length, unsigned timeoutMs, bool timeoutIsError) {
    claimInterfaceOf(ep);
    QByteArray buf(length, '\0');
    int n = 0;
    const int rc = libusb_bulk_transfer(h_, ep, reinterpret_cast<unsigned char*>(buf.data()), length, &n, timeoutMs);
    if (rc == LIBUSB_ERROR_TIMEOUT && !timeoutIsError) {
        buf.truncate(n);
        return buf;
    }
    if (rc != 0)
        throw HardwareError(QString("USB read from endpoint 0x%1 failed: %2").arg(ep, 2, 16, QChar('0'))
                                .arg(usbErrorName(rc)).toStdString());
    buf.truncate(n);
    return buf;
}

int UsbHandle::bulkWrite(uint8_t ep, const QByteArray& data, unsigned timeoutMs) {
    claimInterfaceOf(ep);
    QByteArray d = data;
    int n = 0;
    const int rc = libusb_bulk_transfer(h_, ep, reinterpret_cast<unsigned char*>(d.data()), int(d.size()), &n, timeoutMs);
    if (rc != 0)
        throw HardwareError(QString("USB write to endpoint 0x%1 failed: %2").arg(ep, 2, 16, QChar('0'))
                                .arg(usbErrorName(rc)).toStdString());
    return n;
}

}  // namespace lr::hw
