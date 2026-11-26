#include "armstatus.h"

#include <QThread>
#include <mutex>

#if __has_include(<hidapi.h>)
#include <hidapi.h>
#else
#include <hidapi/hidapi.h>
#endif

#include "../errors.h"

namespace lr::hw {

// ----------------------------------------------------------------------------- hidapi transport
namespace {

constexpr unsigned short MCP2200_VID = 0x04D8, MCP2200_PID = 0x00DF;

QString hidErr(hid_device* d) {
    const wchar_t* e = hid_error(d);
    return e ? QString::fromWCharArray(e) : QStringLiteral("unknown error");
}

class HidapiTransport : public HidTransport {
public:
    explicit HidapiTransport(hid_device* d) : d_(d) {}
    ~HidapiTransport() override { close(); }

    QByteArray transact(const QByteArray& data, bool reply, double timeout_s) override {
        if (!d_) throw HardwareError("MCP2200 is closed");
        unsigned char in[65];
        while (hid_read_timeout(d_, in, 64, 0) > 0) {   // drop stale input reports (_WinHid: HidD_FlushQueue)
        }
        unsigned char out[17] = {0};                     // report ID 0 + 16 bytes
        for (int i = 0; i < data.size() && i < 16; ++i) out[1 + i] = static_cast<unsigned char>(data[i]);
        if (hid_write(d_, out, sizeof out) < 0)
            throw HardwareError(("MCP2200 write failed (" + hidErr(d_) + ")").toStdString());
        if (!reply) return {};
        const int n = hid_read_timeout(d_, in, 64, int(timeout_s * 1000));
        if (n < 0) throw HardwareError(("MCP2200 read failed (" + hidErr(d_) + ")").toStdString());
        return QByteArray(reinterpret_cast<const char*>(in), n);
    }
    QString name() const override { return QStringLiteral("hidapi"); }
    void close() override {
        if (d_) hid_close(d_);
        d_ = nullptr;
    }

private:
    hid_device* d_;
};

}  // namespace

std::unique_ptr<HidTransport> openMcp2200() {
    static std::once_flag once;
    static int rc = 0;
    std::call_once(once, [] { rc = hid_init(); });
    if (rc != 0) throw HardwareError("hidapi could not be initialised");
    hid_device_info* list = hid_enumerate(MCP2200_VID, MCP2200_PID);
    if (!list) return nullptr;                           // not found
    const QByteArray path(list->path);
    hid_free_enumeration(list);
    hid_device* d = hid_open_path(path.constData());
    if (!d) {
#if defined(Q_OS_MACOS)
        throw HardwareError("Cannot open the MCP2200 (macOS: allow LibreRaman under System Settings > "
                            "Privacy & Security > Input Monitoring)");
#else
        throw HardwareError(("cannot open the MCP2200 (" + hidErr(nullptr) + ")").toStdString());
#endif
    }
    return std::make_unique<HidapiTransport>(d);
}

// ----------------------------------------------------------------------------- ArmStatus
ArmStatus::ArmStatus(std::unique_ptr<HidTransport> t) : t_(std::move(t)) {}

QByteArray ArmStatus::cmd(const QByteArray& data, bool reply) {
    std::lock_guard<std::mutex> l(m_);
    if (!t_) throw HardwareError("MCP2200 is closed");
    return t_->transact(data, reply);
}

int ArmStatus::gpio() {
    for (int i = 0; i < 3; ++i) {
        const QByteArray rep = cmd(QByteArray(1, char(0x80)), true);           // READ_ALL
        if (rep.size() > 10 && static_cast<unsigned char>(rep[0]) == 0x80) return static_cast<unsigned char>(rep[10]);
    }
    throw HardwareError("no status from the MCP2200");
}

ArmState ArmStatus::state() {
    const int g = gpio();
    return {!(g & 0x01), !(g & 0x80), g};
}

void ArmStatus::pulse() {
    QByteArray low(13, '\0'), high(13, '\0');   // SET_CLEAR_OUTPUT: 0x08, 10 zero bytes, set mask, clear mask
    low[0] = high[0] = 0x08;
    low[11] = 0x00; low[12] = char(0xFF);
    high[11] = char(0xFF); high[12] = 0x00;
    cmd(low, false);
    QThread::msleep(100);
    cmd(high, false);
}

QString ArmStatus::transportName() const { return t_ ? t_->name() : QString(); }

void ArmStatus::close() {
    std::lock_guard<std::mutex> l(m_);
    if (t_) t_->close();
}

}  // namespace lr::hw
