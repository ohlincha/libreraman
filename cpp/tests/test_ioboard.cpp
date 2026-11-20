// I/O board and laser driver over a fake serial link (hardware.IOBoard / LaserDriver behaviour).
#include <QElapsedTimer>
#include <QThread>
#include <QtTest>
#include <atomic>
#include <map>
#include <mutex>
#include <thread>

#include "core/errors.h"
#include "core/hw/ioboard.h"
#include "core/hw/laserdriver.h"

using namespace lr;

template <class E, class F>
static bool throws(F f) {
    try {
        f();
    } catch (const E&) {
        return true;
    } catch (...) {
        return false;
    }
    return false;
}

// A device at the end of a serial link: records every write (with time) and answers each complete command
// (terminated by `term`) from a script; unknown commands get `defaultReply`.
struct FakeDevice {
    std::mutex m;
    struct Write {
        QByteArray data;
        qint64 ns;
    };
    std::vector<Write> writes;
    QByteArray pending, rx;
    std::map<QByteArray, QByteArray> replies;
    QByteArray defaultReply = "OK\n";
    char term = '\n';
    bool dtr = true, rts = true, closed = false;
    int resets = 0;
    std::atomic<int> inCommand{0}, overlap{0};
    QElapsedTimer t;
    FakeDevice() { t.start(); }
    QByteArray sent() {
        QByteArray all;
        for (const auto& w : writes) all += w.data;
        return all;
    }
};

class FakeLink : public hw::SerialLink {
public:
    explicit FakeLink(std::shared_ptr<FakeDevice> d) : d_(std::move(d)) {}
    QString name() const override { return "fake0"; }
    void write(const QByteArray& data, int) override {
        std::lock_guard<std::mutex> l(d_->m);
        d_->writes.push_back({data, d_->t.nsecsElapsed()});
        d_->pending += data;
        if (d_->pending.endsWith(d_->term)) {
            const QByteArray cmd = d_->pending.chopped(1);
            auto it = d_->replies.find(cmd);
            d_->rx += it != d_->replies.end() ? it->second : d_->defaultReply;
            d_->pending.clear();
        }
    }
    // pyserial semantics: returns when n bytes have arrived or after the timeout. Replies arrive in chunks.
    QByteArray read(int n, int timeoutMs) override {
        QElapsedTimer t;
        t.start();
        QByteArray got;
        while (got.size() < n && t.elapsed() < timeoutMs) {
            {
                std::lock_guard<std::mutex> l(d_->m);
                const QByteArray part = d_->rx.left(std::min(n - int(got.size()), 3));
                d_->rx.remove(0, part.size());
                got += part;
            }
            if (got.size() < n) QThread::msleep(1);
        }
        return got;
    }
    void resetInput() override {
        std::lock_guard<std::mutex> l(d_->m);
        d_->rx.clear();
        ++d_->resets;
    }
    void setDtr(bool on) override { d_->dtr = on; }
    void setRts(bool on) override { d_->rts = on; }
    void close() override { d_->closed = true; }

private:
    std::shared_ptr<FakeDevice> d_;
};

class TestIOBoard : public QObject {
    Q_OBJECT
private slots:
    void openSetsLinesAndFlushes() {
        auto d = std::make_shared<FakeDevice>();
        d->rx = "garbage";
        hw::IOBoard io(std::make_unique<FakeLink>(d), 0.001);
        QVERIFY(!d->dtr);
        QVERIFY(!d->rts);
        QCOMPARE(d->resets, 1);
        QVERIFY(d->rx.isEmpty());
        QVERIFY(d->writes.empty());
        QCOMPARE(io.portName(), QString("fake0"));
    }

    void commandIsPacedCharByChar() {
        auto d = std::make_shared<FakeDevice>();
        hw::IOBoard io(std::make_unique<FakeLink>(d), 0.02);
        QCOMPARE(io.command("TECE"), QString());
        QCOMPARE(d->sent(), QByteArray("TECE\n"));
        QCOMPARE(int(d->writes.size()), 5);                       // one write per character
        for (size_t i = 1; i < d->writes.size(); ++i) {
            QCOMPARE(d->writes[i].data.size(), 1);
            QVERIFY2(d->writes[i].ns - d->writes[i - 1].ns >= 19'000'000, "pacing between characters");
        }
        QCOMPARE(io.pacing(), 0.02);
        io.setPacing(0.08);                                       // power-down timing
        QCOMPARE(io.pacing(), 0.08);
    }

    void replyParsing() {
        auto d = std::make_shared<FakeDevice>();
        d->replies["VOL 03"] = "VOL:0.09\nOK\n";
        d->replies["USB"] = "USB:00\r\nOK\n";
        hw::IOBoard io(std::make_unique<FakeLink>(d), 0.0);
        const auto v = io.vol();
        QVERIFY(v.has_value());
        QCOMPARE(*v, 0.09);
        QCOMPARE(io.command("USB"), QString("USB:00"));
        d->replies["VOL 03"] = "OK\n";
        QVERIFY(!io.vol().has_value());
        d->replies["VOL 03"] = "VOL:x\nOK\n";
        QVERIFY(throws<HardwareError>([&] { io.vol(); }));
    }

    void missingOkIsAnError() {
        auto d = std::make_shared<FakeDevice>();
        d->replies["TECE"] = "ERR\n";
        hw::IOBoard io(std::make_unique<FakeLink>(d), 0.0);
        QElapsedTimer t;
        t.start();
        try {
            io.command("TECE", 0.3);
            QFAIL("no exception");
        } catch (const HardwareError& e) {
            QCOMPARE(QString(e.what()), QString("I/O board did not confirm 'TECE' (got 'ERR')"));
        }
        QVERIFY(t.elapsed() >= 290);                              // waited for the timeout
        d->defaultReply = "";
        try {
            io.command("IO H 01", 0.2);
            QFAIL("no exception");
        } catch (const HardwareError& e) {
            QCOMPARE(QString(e.what()), QString("I/O board did not confirm 'IO H 01' (got '')"));
        }
    }

    void laserLineGuard() {
        auto d = std::make_shared<FakeDevice>();
        hw::IOBoard io(std::make_unique<FakeLink>(d), 0.0);
        QVERIFY(throws<HardwareError>([&] { io.setLine("09", false); }));
        QVERIFY(throws<HardwareError>([&] { io.setLine("09", true); }));
        QVERIFY(d->writes.empty());                               // nothing sent
        io.setLine("09", false, true);
        io.setLine("0a", false);
        io.setLine("01", true);
        QCOMPARE(d->sent(), QByteArray("IO L 09\nIO L 0a\nIO H 01\n"));
        const auto l = io.lines();
        QCOMPARE(l.at("09"), QChar('L'));
        QCOMPARE(l.at("0a"), QChar('L'));
        QCOMPARE(l.at("01"), QChar('H'));
    }

    void commandsAreNotInterleaved() {                            // Stop from another thread
        auto d = std::make_shared<FakeDevice>();
        hw::IOBoard io(std::make_unique<FakeLink>(d), 0.002);
        std::thread t([&] {
            for (int i = 0; i < 5; ++i) io.command("VOL 03");
        });
        for (int i = 0; i < 5; ++i) io.setLine("09", true, true);
        t.join();
        QByteArray all = d->sent();
        QCOMPARE(all.count("VOL 03\n"), 5);
        QCOMPARE(all.count("IO H 09\n"), 5);
        QCOMPARE(all.size(), 5 * 7 + 5 * 8);
    }

    void closeClosesLink() {
        auto d = std::make_shared<FakeDevice>();
        hw::IOBoard io(std::make_unique<FakeLink>(d), 0.0);
        io.close();
        QVERIFY(d->closed);
    }

    void laserDriverCommands() {
        auto d = std::make_shared<FakeDevice>();
        d->term = '\r';
        d->defaultReply = "";
        d->replies["rev?"] = "Software Rev=Rev6  Aug 18 2011 17:44:22\r\n";
        hw::LaserDriver ld(std::make_unique<FakeLink>(d));
        QVERIFY(!d->dtr && !d->rts);
        QCOMPARE(d->resets, 1);
        QCOMPARE(ld.init(), QString("Software Rev=Rev6  Aug 18 2011 17:44:22"));
        QCOMPARE(d->sent(), QByteArray("\rrev?\rtecmax 0.6\rtemperature 25.00\rlaseroff\rcurrent 0.00\r"));
        d->writes.clear();
        ld.on();
        ld.setCurrent(0.275808 + 0.002209 * 50);                 // 0.386258 A -> "0.39"
        ld.setCurrent(0.125);                                     // Python rounds ties to even: "0.12"
        ld.off();
        QCOMPARE(d->sent(), QByteArray("laseron\rcurrent 0.39\rcurrent 0.12\rcurrent 0.00\rlaseroff\r"));
    }
};

QTEST_MAIN(TestIOBoard)
#include "test_ioboard.moc"
