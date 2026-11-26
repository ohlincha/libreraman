#include "watchdog.h"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QProcess>
#include <QThread>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <thread>

#if defined(Q_OS_WIN)
#include <windows.h>
#else
#include <cerrno>
#include <csignal>
#include <sys/types.h>
#include <unistd.h>
#endif

#include "errors.h"
#include "hw/ioboard.h"
#include "hw/laserdriver.h"

namespace lr::watchdog {

namespace {

void appendLog(const QString& msg) {
    QFile f(QDir::homePath() + "/.libreraman-watchdog.log");
    if (!f.open(QIODevice::Append | QIODevice::Text)) return;
    f.write((QDateTime::currentDateTime().toString("yyyy-MM-dd HH:mm:ss ") + msg + "\n").toUtf8());
}

bool parentAlive(qint64 pid) {
    if (pid <= 0) return true;
#if defined(Q_OS_WIN)
    HANDLE h = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, DWORD(pid));
    if (!h) return GetLastError() == ERROR_ACCESS_DENIED;
    const bool alive = WaitForSingleObject(h, 0) == WAIT_TIMEOUT;
    CloseHandle(h);
    return alive;
#else
    return ::kill(pid_t(pid), 0) == 0 || errno == EPERM;
#endif
}

void killProcess(qint64 pid) {                // Python _kill: taskkill /F / SIGKILL, then 1 s
#if defined(Q_OS_WIN)
    if (HANDLE h = OpenProcess(PROCESS_TERMINATE, FALSE, DWORD(pid))) {
        TerminateProcess(h, 1);
        CloseHandle(h);
    }
#else
    ::kill(pid_t(pid), SIGKILL);
#endif
    QThread::msleep(1000);
}

// Python _laser_off: retry the I/O board for 15 s (the port is free once the dead process has released it).
void laserOff(const QString& ioPort, const QString& driverPort) {
    QElapsedTimer t;
    t.start();
    QString last;
    bool done = false;
    while (t.elapsed() < 15000) {
        try {
            auto io = hw::IOBoard::open(ioPort);
            io->setLine(hw::LASER_LINE, true, /*allowLaser=*/true);
            io->close();
            appendLog("watchdog: laser line 09 switched off");
            done = true;
            break;
        } catch (const std::exception& e) {
            last = QString::fromUtf8(e.what());
            QThread::msleep(500);
        }
    }
    if (!done) appendLog("watchdog: could not switch the laser off: " + last);
    if (!driverPort.isEmpty()) {
        try {
            auto d = hw::LaserDriver::open(driverPort);
            d->off();
            d->close();
            appendLog("watchdog: 1064 nm driver off");
        } catch (const std::exception& e) {
            appendLog(QString("watchdog: 1064 nm driver: ") + e.what());
        }
    }
}

// stdin lines, read by a detached thread (it may stay blocked in getline until the process exits).
struct LineQueue {
    std::mutex m;
    std::condition_variable cv;
    std::deque<std::optional<std::string>> q;     // nullopt = EOF
};

}  // namespace

int run(const QString& ioPort, const QString& driverPortIn, qint64 parentPid, double hang_s) {
    QString driverPort = driverPortIn;
    auto lines = std::make_shared<LineQueue>();
    std::thread([lines] {
        std::string line;
        while (std::getline(std::cin, line)) {
            std::lock_guard<std::mutex> l(lines->m);
            lines->q.emplace_back(line);
            lines->cv.notify_all();
        }
        std::lock_guard<std::mutex> l(lines->m);
        lines->q.emplace_back(std::nullopt);
        lines->cv.notify_all();
    }).detach();

    bool laserOn = false;
    QElapsedTimer last;
    last.start();
    for (;;) {
        std::optional<std::optional<std::string>> item;
        {
            std::unique_lock<std::mutex> l(lines->m);
            if (lines->cv.wait_for(l, std::chrono::seconds(1), [&] { return !lines->q.empty(); })) {
                item = std::move(lines->q.front());
                lines->q.pop_front();
            }
        }
        if (!item && !parentAlive(parentPid)) item.emplace(std::nullopt);   // parent gone without closing stdin
        if (!item) {                                                         // 1 s without a message
            if (laserOn && last.elapsed() > hang_s * 1000) {
                appendLog(QString("watchdog: no heartbeat for %1 s with the laser on: ending the program")
                              .arg(QString::number(hang_s, 'f', 0)));
                if (parentPid > 0) killProcess(parentPid);
                laserOff(ioPort, driverPort);
                return 2;
            }
            continue;
        }
        if (!*item) {                                                        // EOF: parent gone
            if (laserOn) {
                appendLog("watchdog: program ended unexpectedly with the laser on");
                laserOff(ioPort, driverPort);
            }
            return 0;
        }
        const QString msg = QString::fromStdString(**item).trimmed();
        if (msg.startsWith("ping")) {
            last.restart();
            laserOn = msg.endsWith("on");
        } else if (msg.startsWith("driver ")) {
            driverPort = msg.mid(7);
        } else if (msg == "quit") {
            return 0;
        }
    }
}

bool isWatchdogCall(const QStringList& args) { return !args.isEmpty() && args[0] == "--watchdog"; }

int runFromArgs(const QStringList& args) {
    QString port, driver;
    qint64 parent = 0;
    for (int i = 0; i + 1 < args.size(); ++i) {
        if (args[i] == "--port") port = args[++i];
        else if (args[i] == "--parent") parent = args[++i].toLongLong();
        else if (args[i] == "--driver") driver = args[++i];
    }
    return run(port, driver, parent);
}

// ----------------------------------------------------------------------------- client
std::unique_ptr<WatchdogClient> WatchdogClient::start(const QString& ioPort) {
    std::unique_ptr<WatchdogClient> c(new WatchdogClient);
    c->p_ = std::make_unique<QProcess>();
    c->p_->setProcessChannelMode(QProcess::ForwardedChannels);   // stdin piped; output as the parent's
#if defined(Q_OS_WIN)
    c->p_->setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* a) {
        a->flags |= CREATE_NO_WINDOW;          // 0x08000000
    });
#elif defined(Q_OS_UNIX)
    c->p_->setChildProcessModifier([] { ::setsid(); });   // survives the parent's terminal/process group
#endif
    c->p_->start(QCoreApplication::applicationFilePath(),
                 {"--watchdog", "--port", ioPort, "--parent", QString::number(QCoreApplication::applicationPid())});
    if (!c->p_->waitForStarted(5000))
        throw Error(("cannot start the laser watchdog: " + c->p_->errorString()).toStdString());
    return c;
}

WatchdogClient::~WatchdogClient() { quit(); }

bool WatchdogClient::running() const { return p_ && p_->state() == QProcess::Running; }

bool WatchdogClient::send(const QByteArray& line) {
    if (!running()) {
        p_.reset();
        return false;
    }
    if (p_->write(line) != line.size()) {
        p_.reset();
        return false;
    }
    p_->waitForBytesWritten(1000);
    return true;
}

bool WatchdogClient::ping(bool laserOn, const QString& driverPort) {
    if (!send(laserOn ? "ping on\n" : "ping off\n")) return false;
    if (!driverPort.isEmpty()) return send(("driver " + driverPort + "\n").toUtf8());
    return true;
}

void WatchdogClient::quit() {
    if (!p_) return;
    if (p_->state() == QProcess::Running) {
        p_->write("quit\n");
        p_->waitForBytesWritten(1000);
        p_->closeWriteChannel();
        p_->waitForFinished(2000);
    }
    p_.reset();
}

}  // namespace lr::watchdog
