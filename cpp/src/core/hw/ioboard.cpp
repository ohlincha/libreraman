#include "ioboard.h"

#include <QElapsedTimer>
#include <QThread>

#include "../errors.h"
#include "../pyformat.h"

namespace lr::hw {

IOBoard::IOBoard(std::unique_ptr<SerialLink> link, double pacing_s) : s_(std::move(link)), pacing_(pacing_s) {
    s_->setDtr(false);
    s_->setRts(false);
    QThread::msleep(100);
    s_->resetInput();
}

std::unique_ptr<IOBoard> IOBoard::open(const QString& port, double pacing_s) {
    return std::make_unique<IOBoard>(openSerialPort(port, 115200), pacing_s);
}

QString IOBoard::command(const QString& text, double timeout_s) {
    std::lock_guard<std::mutex> l(m_);
    if (!s_) throw HardwareError("I/O board is closed");
    const QByteArray bytes = (text + "\n").toLatin1();
    for (char ch : bytes) {                   // one character at a time (the board drops fast input)
        s_->write(QByteArray(1, ch), 2000);   // pyserial write_timeout=2
        QThread::usleep(static_cast<unsigned long>(pacing_ * 1e6));
    }
    QByteArray buf;
    QElapsedTimer t;
    t.start();
    while (t.elapsed() < qint64(timeout_s * 1000) && !buf.endsWith("OK\n"))
        buf += s_->read(64, 100);             // port timeout 0.1 s
    const QString reply = py::asciiReplace(buf).trimmed();
    if (!reply.endsWith("OK"))
        throw HardwareError(QString("I/O board did not confirm %1 (got %2)")
                                .arg(py::repr(text), py::repr(reply)).toStdString());
    return reply.left(reply.size() - 2).trimmed();
}

void IOBoard::setLine(const QString& line, bool high, bool allowLaser) {
    if (line == LASER_LINE && !allowLaser)
        throw HardwareError("the laser line is switched only through Cora.laser_532()");
    command(QString("IO %1 %2").arg(high ? 'H' : 'L').arg(line));
    std::lock_guard<std::mutex> l(m_);
    lines_[line] = high ? 'H' : 'L';
}

std::optional<double> IOBoard::vol() {
    const QString r = command("VOL 03");
    if (!r.contains(':')) return std::nullopt;
    bool ok = false;
    const double v = r.split(':').value(1).trimmed().toDouble(&ok);
    if (!ok) throw HardwareError(("unexpected VOL reply " + py::repr(r)).toStdString());
    return v;
}

void IOBoard::setPacing(double s) {
    std::lock_guard<std::mutex> l(m_);
    pacing_ = s;
}

double IOBoard::pacing() const {
    std::lock_guard<std::mutex> l(m_);
    return pacing_;
}

std::map<QString, QChar> IOBoard::lines() const {
    std::lock_guard<std::mutex> l(m_);
    return lines_;
}

QString IOBoard::portName() const { return s_ ? s_->name() : QString(); }

void IOBoard::close() {
    std::lock_guard<std::mutex> l(m_);
    if (s_) s_->close();
}

}  // namespace lr::hw
