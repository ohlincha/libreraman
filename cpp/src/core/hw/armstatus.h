#pragma once
// ARM state and compartment switch via the Microchip MCP2200's HID side (Python: hardware.ArmStatus and the
// platform HID transports _LinuxHid/_MacHid/_WinHid; PROTOCOL.md "MCP2200", "Arming").
//   READ_ALL (0x80) -> reply byte 10 = GPIO port: bit 0 = 0 armed, bit 7 = 0 compartment open.
//   pulse(): SET_CLEAR_OUTPUT (0x08) all low, 0.1 s, all high - as the vendor software does around measurements.
// Output reports are report ID 0 + 16 bytes; replies are read with a timeout, stale input reports are
// dropped before each command.
//
// Thread safety: all methods may be called from any thread (internal mutex; status is polled by the GUI
// while a measurement runs).
#include <QByteArray>
#include <QString>
#include <memory>
#include <mutex>

namespace lr::hw {

class HidTransport {
public:
    virtual ~HidTransport() = default;
    // Drops pending input reports, writes report ID 0 + data padded to 16 bytes, then (if reply) returns
    // the next input report without report ID, or an empty array after timeout_s. Throws HardwareError.
    virtual QByteArray transact(const QByteArray& data, bool reply = true, double timeout_s = 2.0) = 0;
    virtual QString name() const = 0;
    virtual void close() = 0;
};

// hidapi transport for the MCP2200 (04d8:00df); nullptr if not present. Throws HardwareError if present
// but it can't be opened (macOS: Input Monitoring permission).
std::unique_ptr<HidTransport> openMcp2200();

struct ArmState {
    bool armed = false;
    bool compartmentOpen = false;
    int gpio = 0;
};

class ArmStatus {
public:
    explicit ArmStatus(std::unique_ptr<HidTransport> t);
    int gpio();                               // 3 attempts, else HardwareError("no status from the MCP2200")
    ArmState state();
    void pulse();
    QString transportName() const;
    void close();

private:
    QByteArray cmd(const QByteArray& data, bool reply);
    std::unique_ptr<HidTransport> t_;
    std::mutex m_;
};

}  // namespace lr::hw
