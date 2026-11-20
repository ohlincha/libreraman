#include "spectrometer.h"

#include <QElapsedTimer>
#include <QThread>
#include <QtEndian>
#include <algorithm>
#include <cmath>

#include "../errors.h"

namespace lr::hw {

namespace {
constexpr uint16_t BAYSPEC_VID = 0xBA8C, BAYSPEC_PID = 0x1688;
int frameBytes(int ch) { return ch == 532 ? 4608 : 1024; }
std::pair<int, int> active(int ch) { return ch == 532 ? std::make_pair(10, 2058) : std::make_pair(0, 256); }
// Python's round(): ties to even (nearbyint in the default rounding mode).
long long pyRound(double x) { return static_cast<long long>(std::nearbyint(x)); }
}  // namespace

Spectrometer::Spectrometer(std::unique_ptr<UsbHandle> dev, int channel) : dev_(std::move(dev)), channel_(channel) {
    dev_->ensureConfigured();                 // never re-configure an active device
}

QString Spectrometer::productName(int channel) { return channel == 532 ? "BS2ACCD" : "BS2BASE"; }

std::unique_ptr<Spectrometer> Spectrometer::find(int channel, double timeout_s) {
    QElapsedTimer t;
    t.start();
    for (;;) {
        std::vector<libusb_device*> devs;
        try {
            devs = UsbHandle::list(BAYSPEC_VID, BAYSPEC_PID);
        } catch (const Error&) {
        }
        std::unique_ptr<Spectrometer> found;
        for (auto* d : devs) {
            if (found) break;
            try {
                auto h = std::make_unique<UsbHandle>(d);
                if (h->productString() == productName(channel)) found = std::make_unique<Spectrometer>(std::move(h), channel);
            } catch (const std::exception&) {
            }
        }
        UsbHandle::unref(devs);
        if (found) return found;
        if (t.elapsed() >= timeout_s * 1000) return nullptr;
        QThread::msleep(250);
    }
}

void Spectrometer::out(uint8_t req, uint16_t value, uint16_t index, const QByteArray& data) {
    dev_->ctrlOut(0x40, req, value, index, data, 1000);
}

QByteArray Spectrometer::in(uint8_t req, uint16_t index, uint16_t length) {
    return dev_->ctrlIn(0xC0, req, 0, index, length, 1000);
}

QByteArray Spectrometer::eeprom() {
    QByteArray e;
    for (uint16_t off : {0x00, 0x40, 0x80, 0xC0}) e += in(0xA1, off, 64);
    return e;
}

QString Spectrometer::serial() {
    const QByteArray e = eeprom().mid(8, 8);
    QString s;
    for (char c : e) s += (static_cast<unsigned char>(c) < 0x80) ? QChar(c) : QChar(0xFFFD);  // ascii, replace
    return s;
}

void Spectrometer::init() {
    out(0xA3);
    if (channel_ == 532) {
        out(0xA4, 0, 0x03F0);
    } else {
        out(0xA4, 1, 0x03F0);
        out(0xAD, 0, 0x38);
        out(0xA4, 1, 0x03F0);
    }
}

std::pair<quint32, quint32> Spectrometer::exposureRaw(int channel, double seconds) {
    if (channel == 532)   // 16 us units minus 1; param2 ~ 20 s / exposure, 1..200
        return {quint32(std::max(0LL, pyRound(seconds / 16e-6) - 1)),
                quint32(std::max(1LL, std::min(200LL, pyRound(20 / seconds))))};
    return {quint32(std::max(0LL, pyRound(seconds / 0.8e-6) - 25)), 1u};    // 0.8 us units minus 25
}

void Spectrometer::setExposure(double seconds) {
    const auto [raw, p2] = exposureRaw(channel_, seconds);
    QByteArray d(8, '\0');
    qToLittleEndian<quint32>(raw, d.data());
    qToLittleEndian<quint32>(p2, d.data() + 4);
    out(0xAE, 8, 0x30, d);
    exposure_ = seconds;
}

std::vector<double> Spectrometer::frame() {
    if (!exposure_) throw Error("spectrometer: exposure not set");
    out(0xAA, 0xFFFF, 0xFFFF);                // start the exposure
    QByteArray data;
    try {
        data = dev_->bulkRead(0x82, frameBytes(channel_), unsigned(int(*exposure_ * 1000) + 5000));
    } catch (...) {
        out(0xAA, 0, 0);                      // always end it (Python: finally)
        throw;
    }
    out(0xAA, 0, 0);
    const int n = int(data.size() / 2);
    const auto [a, b] = active(channel_);
    if (n < b) throw HardwareError(QString("short frame: %1 bytes").arg(data.size()).toStdString());
    std::vector<double> px;
    px.reserve(b - a);
    const auto* p = reinterpret_cast<const uchar*>(data.constData());
    for (int i = a; i < b; ++i) px.push_back(qFromBigEndian<quint16>(p + 2 * i));
    return px;
}

std::vector<double> Spectrometer::pixelNumbers(int channel) {
    // 532: reversed (p = 2057 - i, verified with water). 1064: assumed p = i (not yet verified).
    const auto [a, b] = active(channel);
    std::vector<double> out;
    for (int i = a; i < b; ++i) out.push_back(channel == 532 ? 2057 - i : i);
    return out;
}

std::vector<double> Spectrometer::pixelNumbers() const { return pixelNumbers(channel_); }

}  // namespace lr::hw
