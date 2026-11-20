#include "pl2303usb.h"

#include <QElapsedTimer>
#include <QtEndian>
#include <algorithm>

#include "../errors.h"

namespace lr::hw {

Pl2303Usb::Pl2303Usb(std::unique_ptr<UsbHandle> dev, int baud) : dev_(std::move(dev)) {
    port_ = QString("usb:%1-%2").arg(dev_->bus()).arg(dev_->address());
    dev_->ensureConfigured();
    // Vendor set-up sequence of the PL2303 driver (as in the captures; same order as hardware.py).
    auto r = [&](uint16_t v) { dev_->ctrlIn(0xC0, 0x01, v, 0, 1, 1000); };
    auto w = [&](uint16_t v, uint16_t i) { dev_->ctrlOut(0x40, 0x01, v, i, {}, 1000); };
    r(0x8484); w(0x0404, 0); r(0x8484); r(0x8383); r(0x8484); w(0x0404, 1); r(0x8484); r(0x8383);
    w(0x0000, 1); w(0x0001, 0); w(0x0002, 0x44);
    // pyusb claims the interface before the first class request.
    dev_->claimInterface(0);
    // CDC SET_LINE_CODING: <u32 baud><u8 stop bits 0 = 1><u8 parity 0 = none><u8 data bits 8>
    QByteArray lc(7, '\0');
    qToLittleEndian<quint32>(quint32(baud), lc.data());
    lc[4] = 0; lc[5] = 0; lc[6] = 8;
    dev_->ctrlOut(0x21, 0x20, 0, 0, lc, 1000);
    // SET_CONTROL_LINE_STATE 0: DTR and RTS off
    dev_->ctrlOut(0x21, 0x22, 0, 0, {}, 1000);
}

std::vector<std::unique_ptr<Pl2303Usb>> Pl2303Usb::findAll() {
    std::vector<std::unique_ptr<Pl2303Usb>> out;
    std::vector<libusb_device*> devs;
    try {
        devs = UsbHandle::list(0x067B, 0x2303);
    } catch (const Error&) {
        return out;                           // no libusb
    }
    for (auto* d : devs) {
        try {
            out.push_back(std::make_unique<Pl2303Usb>(std::make_unique<UsbHandle>(d)));
        } catch (const std::exception&) {     // e.g. still bound to the Prolific driver
        }
    }
    UsbHandle::unref(devs);
    return out;
}

void Pl2303Usb::write(const QByteArray& data, int /*timeoutMs*/) {
    if (!dev_) throw HardwareError("PL2303 is closed");
    dev_->bulkWrite(0x02, data, 2000);
}

QByteArray Pl2303Usb::read(int n, int timeoutMs) {
    if (!dev_) throw HardwareError("PL2303 is closed");
    QElapsedTimer t;
    t.start();
    while (buf_.size() < n) {
        const unsigned remaining = unsigned(std::max<qint64>(1, timeoutMs - t.elapsed()));
        try {
            buf_ += dev_->bulkRead(0x83, 64, remaining, /*timeoutIsError=*/false);
        } catch (const HardwareError&) {      // as hardware.py: errors are treated like a timeout
        }
        if (t.elapsed() >= timeoutMs) break;
    }
    const QByteArray out = buf_.left(n);
    buf_.remove(0, out.size());
    return out;
}

void Pl2303Usb::resetInput() {
    buf_.clear();
    while (!read(64, 50).isEmpty()) {
    }
}

void Pl2303Usb::close() { dev_.reset(); }      // pyusb dispose_resources: release interface, close

}  // namespace lr::hw
