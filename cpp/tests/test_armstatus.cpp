// MCP2200 ARM status over a fake HID transport (hardware.ArmStatus behaviour).
#include <QElapsedTimer>
#include <QThread>
#include <QtTest>
#include <atomic>
#include <deque>
#include <mutex>
#include <thread>

#include "core/errors.h"
#include "core/hw/armstatus.h"

using namespace lr;

struct FakeHidState {
    std::mutex m;
    struct Call {
        QByteArray data;
        bool reply;
        qint64 ms;
    };
    std::vector<Call> calls;
    std::deque<QByteArray> replies;           // consumed by READ_ALL; empty -> `fallback`
    QByteArray fallback;
    std::atomic<int> active{0}, overlaps{0};
    bool closed = false;
    QElapsedTimer t;
    FakeHidState() { t.start(); }
};

class FakeHid : public hw::HidTransport {
public:
    explicit FakeHid(std::shared_ptr<FakeHidState> s) : s_(std::move(s)) {}
    QByteArray transact(const QByteArray& data, bool reply, double) override {
        if (s_->active++ > 0) ++s_->overlaps;
        QThread::usleep(200);
        QByteArray r;
        {
            std::lock_guard<std::mutex> l(s_->m);
            s_->calls.push_back({data, reply, s_->t.elapsed()});
            if (reply) {
                if (!s_->replies.empty()) {
                    r = s_->replies.front();
                    s_->replies.pop_front();
                } else {
                    r = s_->fallback;
                }
            }
        }
        --s_->active;
        return r;
    }
    QString name() const override { return "fake"; }
    void close() override { s_->closed = true; }

private:
    std::shared_ptr<FakeHidState> s_;
};

static QByteArray readAll(unsigned char gpio) {
    // as captured on Linux: 80 00 00 00 8f 00 ff 00 04 e1 <gpio> ...
    QByteArray r = QByteArray::fromHex("800000008f00ff0004e1");
    r.append(char(gpio));
    r.append(QByteArray(5, '\0'));
    return r;
}

class TestArmStatus : public QObject {
    Q_OBJECT
private slots:
    void decodesGpio() {
        auto s = std::make_shared<FakeHidState>();
        hw::ArmStatus a(std::make_unique<FakeHid>(s));
        s->fallback = readAll(0xFF);                             // disarmed, compartment closed
        auto st = a.state();
        QVERIFY(!st.armed);
        QVERIFY(!st.compartmentOpen);
        QCOMPARE(st.gpio, 0xFF);
        s->fallback = readAll(0xFE);                             // bit 0 = 0: armed
        st = a.state();
        QVERIFY(st.armed);
        QVERIFY(!st.compartmentOpen);
        s->fallback = readAll(0x7F);                             // bit 7 = 0: compartment open
        st = a.state();
        QVERIFY(!st.armed);
        QVERIFY(st.compartmentOpen);
        QCOMPARE(int(s->calls.size()), 3);
        for (const auto& c : s->calls) {
            QCOMPARE(c.data, QByteArray(1, char(0x80)));         // READ_ALL
            QVERIFY(c.reply);
        }
    }

    void retriesThreeTimes() {
        auto s = std::make_shared<FakeHidState>();
        hw::ArmStatus a(std::make_unique<FakeHid>(s));
        s->replies = {QByteArray(), QByteArray::fromHex("0102030405060708090a0b"), readAll(0xFE)};
        QCOMPARE(a.gpio(), 0xFE);
        QCOMPARE(int(s->calls.size()), 3);
        s->calls.clear();
        s->replies.clear();
        s->fallback = QByteArray::fromHex("80000000");           // too short
        try {
            a.gpio();
            QFAIL("no exception");
        } catch (const HardwareError& e) {
            QCOMPARE(QString(e.what()), QString("no status from the MCP2200"));
        }
        QCOMPARE(int(s->calls.size()), 3);
    }

    void pulse() {
        auto s = std::make_shared<FakeHidState>();
        hw::ArmStatus a(std::make_unique<FakeHid>(s));
        a.pulse();
        QCOMPARE(int(s->calls.size()), 2);
        QCOMPARE(s->calls[0].data, QByteArray::fromHex("08" "00000000000000000000" "00ff"));
        QCOMPARE(s->calls[1].data, QByteArray::fromHex("08" "00000000000000000000" "ff00"));
        QVERIFY(!s->calls[0].reply);
        QVERIFY(!s->calls[1].reply);
        QVERIFY(s->calls[1].ms - s->calls[0].ms >= 99);
    }

    void serialisedAcrossThreads() {
        auto s = std::make_shared<FakeHidState>();
        s->fallback = readAll(0xFE);
        hw::ArmStatus a(std::make_unique<FakeHid>(s));
        std::vector<std::thread> ts;
        for (int i = 0; i < 4; ++i)
            ts.emplace_back([&] {
                for (int k = 0; k < 20; ++k) a.state();
            });
        for (auto& t : ts) t.join();
        QCOMPARE(s->overlaps.load(), 0);
        QCOMPARE(int(s->calls.size()), 80);
        a.close();
        QVERIFY(s->closed);
        QCOMPARE(a.transportName(), QString("fake"));
    }
};

QTEST_MAIN(TestArmStatus)
#include "test_armstatus.moc"
