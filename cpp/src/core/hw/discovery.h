#pragma once
// Finding the Cora's serial ports by USB ID (Python: hardware.find_tty, tty_candidates, _usb_chain).
//  Linux: /dev/ttyUSB* and /dev/ttyACM* whose owning USB device has the ID; with insideCora only one that
//         sits behind the Cora's hub 0424:2517 (sysfs parent chain), so other adapters are never touched.
//  macOS/Windows: no hub information; all ports with the ID (macOS: /dev/cu.* instead of /dev/tty.*);
//         the caller probes them (Cora::connect: VOL query).
#include <QList>
#include <QPair>
#include <QString>
#include <QStringList>
#include <cstdint>

namespace lr::hw {

struct UsbId {
    uint16_t vid, pid;
};
constexpr UsbId CORA_HUB{0x0424, 0x2517};
constexpr UsbId PROLIFIC{0x067B, 0x2303};
constexpr UsbId MCP2200{0x04D8, 0x00DF};
constexpr UsbId CP210X{0x10C4, 0xEA60};

// First matching port, or an empty string.
QString findTty(UsbId id, bool insideCora = true);
// All ports with this USB ID (Linux: at most one, without the hub condition).
QStringList ttyCandidates(UsbId id);

// Linux helper (exposed for tests): IDs ("067b","2303") of the device owning a sysfs path and its parent
// hubs, nearest first.
QList<QPair<QString, QString>> usbChain(const QString& sysDev);

}  // namespace lr::hw
