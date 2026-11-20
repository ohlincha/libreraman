#include "laserdriver.h"

#include <QThread>

#include "../errors.h"
#include "../pyformat.h"

namespace lr::hw {

LaserDriver::LaserDriver(std::unique_ptr<SerialLink> link) : s_(std::move(link)) {
    s_->setDtr(false);
    s_->setRts(false);
    QThread::msleep(200);
    s_->resetInput();
}

std::unique_ptr<LaserDriver> LaserDriver::open(const QString& port) {
    return std::make_unique<LaserDriver>(openSerialPort(port, 19200));
}

QString LaserDriver::command(const QString& text) {
    std::lock_guard<std::mutex> l(m_);
    if (!s_) throw HardwareError("laser driver is closed");
    s_->write((text + "\r").toLatin1(), -1);  // pyserial without write_timeout: blocking
    QThread::msleep(120);
    return py::asciiReplace(s_->read(200, 300)).trimmed();
}

QString LaserDriver::init() {
    command("");
    const QString rev = command("rev?");
    for (const char* c : {"tecmax 0.6", "temperature 25.00", "laseroff", "current 0.00"}) command(c);
    return rev;
}

void LaserDriver::setCurrent(double amps) { command("current " + py::fixed(amps, 2)); }

void LaserDriver::on() { command("laseron"); }

void LaserDriver::off() {
    command("current 0.00");
    command("laseroff");
}

QString LaserDriver::portName() const { return s_ ? s_->name() : QString(); }

void LaserDriver::close() {
    std::lock_guard<std::mutex> l(m_);
    if (s_) s_->close();
}

}  // namespace lr::hw
