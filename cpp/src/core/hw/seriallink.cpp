#include "seriallink.h"

#include <QDeadlineTimer>
#include <QSerialPort>
#include <QThread>
#include <condition_variable>
#include <deque>
#include <functional>
#include <future>
#include <mutex>

#if defined(Q_OS_UNIX)
#include <termios.h>
#endif

#include "../errors.h"

namespace lr::hw {
namespace {

// Runs closures in one QThread that owns the QSerialPort. QSerialPort is a QObject with thread affinity
// (socket notifiers); the instrument calls it from worker threads and the GUI thread, so every call is
// executed in the port's own thread, using only QSerialPort's blocking waitFor* API (no event loop needed).
class PortThread : public QThread {
public:
    PortThread() { start(); }
    ~PortThread() override {
        {
            std::lock_guard<std::mutex> l(m_);
            quit_ = true;
        }
        cv_.notify_all();
        wait();
    }
    template <class F>
    auto call(F f) -> decltype(f()) {
        using R = decltype(f());
        auto task = std::make_shared<std::packaged_task<R()>>(std::move(f));
        auto fut = task->get_future();
        {
            std::lock_guard<std::mutex> l(m_);
            q_.emplace_back([task] { (*task)(); });
        }
        cv_.notify_all();
        return fut.get();                     // rethrows exceptions from the port thread
    }

protected:
    void run() override {
        for (;;) {
            std::function<void()> job;
            {
                std::unique_lock<std::mutex> l(m_);
                cv_.wait(l, [this] { return quit_ || !q_.empty(); });
                if (q_.empty()) return;       // quit
                job = std::move(q_.front());
                q_.pop_front();
            }
            job();
        }
    }

private:
    std::mutex m_;
    std::condition_variable cv_;
    std::deque<std::function<void()>> q_;
    bool quit_ = false;
};

class QtSerialLink : public SerialLink {
public:
    QtSerialLink(const QString& port, int baud) : name_(port) {
        t_.call([&] {
            auto p = std::make_unique<QSerialPort>();
            p->setPortName(port);
            p->setBaudRate(baud);
            p->setDataBits(QSerialPort::Data8);
            p->setParity(QSerialPort::NoParity);
            p->setStopBits(QSerialPort::OneStop);
            p->setFlowControl(QSerialPort::NoFlowControl);
            if (!p->open(QIODevice::ReadWrite))
                throw HardwareError(("cannot open " + port + ": " + p->errorString()).toStdString());
            port_ = std::move(p);
        });
    }
    ~QtSerialLink() override {
        try {
            close();
        } catch (...) {
        }
    }
    QString name() const override { return name_; }

    void write(const QByteArray& data, int timeoutMs) override {
        t_.call([&] {
            QSerialPort* p = port();
            if (p->write(data) != data.size())
                throw HardwareError(("write to " + name_ + " failed: " + p->errorString()).toStdString());
            const QDeadlineTimer dl = timeoutMs < 0 ? QDeadlineTimer(QDeadlineTimer::Forever)
                                                    : QDeadlineTimer(qint64(timeoutMs));
            while (p->bytesToWrite() > 0) {           // pyserial write_timeout
                const int ms = timeoutMs < 0 ? -1 : int(dl.remainingTime());
                if (timeoutMs >= 0 && ms <= 0)
                    throw HardwareError(("write to " + name_ + " timed out").toStdString());
                if (!p->waitForBytesWritten(ms) && p->error() != QSerialPort::NoError &&
                    p->error() != QSerialPort::TimeoutError)
                    throw HardwareError(("write to " + name_ + " failed: " + p->errorString()).toStdString());
            }
#if defined(Q_OS_UNIX)
            tcdrain(int(p->handle()));        // pyserial flush(): wait until transmitted
#endif
        });
    }

    QByteArray read(int n, int timeoutMs) override {
        return t_.call([&] {
            QSerialPort* p = port();
            const QDeadlineTimer dl(qint64(timeoutMs > 0 ? timeoutMs : 0));
            while (p->bytesAvailable() < n) {
                const qint64 rem = dl.remainingTime();
                if (rem <= 0) break;
                if (!p->waitForReadyRead(int(rem)) && p->error() != QSerialPort::TimeoutError &&
                    p->error() != QSerialPort::NoError)
                    throw HardwareError(("read from " + name_ + " failed: " + p->errorString()).toStdString());
            }
            return p->read(n);
        });
    }

    void resetInput() override {
        t_.call([&] { port()->clear(QSerialPort::Input); });
    }
    void setDtr(bool on) override {
        t_.call([&] { port()->setDataTerminalReady(on); });
    }
    void setRts(bool on) override {
        t_.call([&] { port()->setRequestToSend(on); });
    }
    void close() override {
        t_.call([&] {
            if (port_) port_->close();
            port_.reset();
        });
    }

private:
    QSerialPort* port() {
        if (!port_) throw HardwareError(("port " + name_ + " is closed").toStdString());
        return port_.get();
    }
    QString name_;
    PortThread t_;                            // declared before port_: port_ is destroyed in close() first
    std::unique_ptr<QSerialPort> port_;       // lives in (and is only touched by) t_
};

}  // namespace

std::unique_ptr<SerialLink> openSerialPort(const QString& port, int baud) {
    return std::make_unique<QtSerialLink>(port, baud);
}

}  // namespace lr::hw
