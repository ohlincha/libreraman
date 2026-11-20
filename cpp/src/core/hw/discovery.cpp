#include "discovery.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QSerialPortInfo>
#include <algorithm>

namespace lr::hw {

namespace {
QString hex4(uint16_t v) { return QString("%1").arg(v, 4, 16, QChar('0')); }

bool readId(const QString& dir, QPair<QString, QString>& out) {
    QFile v(dir + "/idVendor"), p(dir + "/idProduct");
    if (!v.open(QIODevice::ReadOnly) || !p.open(QIODevice::ReadOnly)) return false;
    out = {QString::fromLatin1(v.readAll()).trimmed(), QString::fromLatin1(p.readAll()).trimmed()};
    return true;
}
}  // namespace

QList<QPair<QString, QString>> usbChain(const QString& sysDev) {
    QList<QPair<QString, QString>> out;
    QString p = QFileInfo(sysDev).canonicalFilePath();     // os.path.realpath
    while (!p.isEmpty() && p != "/") {
        QPair<QString, QString> id;
        if (readId(p, id)) out.append(id);
        const int slash = p.lastIndexOf('/');
        p = slash <= 0 ? QString() : p.left(slash);
    }
    return out;
}

QString findTty(UsbId id, bool insideCora) {
#if defined(Q_OS_LINUX)
    const QPair<QString, QString> want{hex4(id.vid), hex4(id.pid)}, hub{hex4(CORA_HUB.vid), hex4(CORA_HUB.pid)};
    QStringList devs;
    for (const QString& n : QDir("/dev").entryList({"ttyUSB*", "ttyACM*"}, QDir::System | QDir::Files | QDir::NoDotAndDotDot))
        devs << "/dev/" + n;
    std::sort(devs.begin(), devs.end());      // Python: sorted(glob + glob)
    for (const QString& dev : devs) {
        const auto chain = usbChain("/sys/class/tty/" + QFileInfo(dev).fileName() + "/device");
        if (!chain.isEmpty() && chain[0] == want && (!insideCora || chain.mid(1).contains(hub))) return dev;
    }
    return {};
#else
    Q_UNUSED(insideCora);
    const QStringList c = ttyCandidates(id);
    return c.isEmpty() ? QString() : c[0];
#endif
}

QStringList ttyCandidates(UsbId id) {
#if defined(Q_OS_LINUX)
    const QString d = findTty(id, false);
    return d.isEmpty() ? QStringList() : QStringList{d};
#else
    QStringList out;
    for (const QSerialPortInfo& p : QSerialPortInfo::availablePorts()) {
        if (!p.hasVendorIdentifier() || !p.hasProductIdentifier()) continue;
        if (p.vendorIdentifier() != id.vid || p.productIdentifier() != id.pid) continue;
#if defined(Q_OS_WIN)
        QString dev = p.portName();            // "COM3" (pyserial's device name)
#else
        QString dev = p.systemLocation();
#endif
#if defined(Q_OS_MACOS)
        if (dev.startsWith("/dev/tty.")) dev = "/dev/cu." + dev.mid(9);   // cu.* doesn't wait for carrier detect
#endif
        if (!out.contains(dev)) out << dev;
    }
    std::sort(out.begin(), out.end());
    return out;
#endif
}

}  // namespace lr::hw
