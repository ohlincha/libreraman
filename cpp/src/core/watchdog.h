#pragma once
// Laser watchdog (Python: watchdog.py): a separate process that switches the laser off if the main program
// dies or hangs.
//
// The main program starts it (WatchdogClient::start) with a pipe on stdin and writes one line per status
// poll: "ping on" / "ping off" (laser state), "driver <port>" (1064 nm driver port, if open), "quit".
//  * stdin closed (main program crashed or was killed) - or the parent process is gone: the watchdog opens
//    the I/O board and sends IO H 09 (and switches the 1064 nm driver off if its port is known), then exits;
//  * no ping for hang_s while the laser was on: the main program is considered hung; the watchdog kills it
//    and switches the laser off.
// Executables dispatch to run() when started as `<exe> --watchdog --port P --parent PID` (isWatchdogCall /
// runFromArgs). Log: ~/.libreraman-watchdog.log.
//
// Thread safety: WatchdogClient belongs to the thread that created it (QProcess).
#include <QString>
#include <QStringList>
#include <memory>

class QProcess;

namespace lr::watchdog {

// The watchdog loop on this process's stdin; returns the exit code (0 normal/EOF, 2 after a hang).
int run(const QString& ioPort, const QString& driverPort = {}, qint64 parentPid = 0, double hang_s = 20.0);

bool isWatchdogCall(const QStringList& args);   // args (without argv[0]) start with "--watchdog"
int runFromArgs(const QStringList& args);       // parses --port, --parent [, --driver] and calls run()

class WatchdogClient {
public:
    // Starts `<this executable> --watchdog --port ioPort --parent <pid>` in its own session (Unix) /
    // without a console window (Windows). Throws lr::Error.
    static std::unique_ptr<WatchdogClient> start(const QString& ioPort);
    ~WatchdogClient();                        // quit()

    bool ping(bool laserOn, const QString& driverPort = {});   // false (and stops using it) if the pipe broke
    void quit();                              // "quit", close stdin, wait briefly
    bool running() const;

private:
    WatchdogClient() = default;
    bool send(const QByteArray& line);
    std::unique_ptr<QProcess> p_;
};

}  // namespace lr::watchdog
