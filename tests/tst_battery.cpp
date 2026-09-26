// Battery::parsePacket bounds + protocol regression.
//
// Pre-fix, parsePacket called packet[6] after only verifying the
// 6-byte BATTERY_STATUS header via startsWith. A 6-byte buffer
// (== header length, no count byte) would UB on packet[6].
// New code rejects size < 7 up front.

#include <QTest>
#include <QByteArray>
#include <QSignalSpy>
#include <QLoggingCategory>

Q_LOGGING_CATEGORY(openpods, "openpods.test", QtWarningMsg)

#include "../battery.hpp"

class TestBattery : public QObject
{
    Q_OBJECT

private slots:
    void parsePacket_rejectsEmpty()
    {
        Battery b;
        QVERIFY(!b.parsePacket(QByteArray()));
    }

    void parsePacket_rejectsShortBelowHeader()
    {
        Battery b;
        QByteArray buf = AirPodsPackets::Parse::BATTERY_STATUS.left(4);
        QVERIFY(!b.parsePacket(buf));
    }

    void parsePacket_rejectsHeaderOnly()
    {
        // Exactly the header (6 bytes) — startsWith passes; pre-fix this
        // UB'd on packet[6].
        Battery b;
        QByteArray buf = AirPodsPackets::Parse::BATTERY_STATUS;
        QCOMPARE(buf.size(), 6);
        QVERIFY(!b.parsePacket(buf));
    }

    void parsePacket_rejectsBatteryCountTooHigh()
    {
        Battery b;
        QByteArray buf = AirPodsPackets::Parse::BATTERY_STATUS;
        buf.append(static_cast<char>(4)); // count > 3 → reject
        QVERIFY(!b.parsePacket(buf));
    }

    void parsePacket_rejectsCountSizeMismatch()
    {
        Battery b;
        QByteArray buf = AirPodsPackets::Parse::BATTERY_STATUS;
        buf.append(static_cast<char>(1)); // 1 battery → expect 7 + 5 = 12 bytes
        buf.append(QByteArray(3, '\0')); // only 3 more = 10 total
        QVERIFY(!b.parsePacket(buf));
    }

    void parsePacket_acceptsSingleHeadset()
    {
        Battery b;
        QSignalSpy spy(&b, &Battery::batteryStatusChanged);

        // Build a valid 1-battery packet: header + count(1) + entry(5).
        // Entry layout: type, spacer(0x01), level, status, end(0x01)
        QByteArray buf = AirPodsPackets::Parse::BATTERY_STATUS;
        buf.append(static_cast<char>(1));                       // count
        buf.append(static_cast<char>(Battery::Component::Headset)); // type
        buf.append(static_cast<char>(0x01));                    // spacer
        buf.append(static_cast<char>(85));                      // level
        buf.append(static_cast<char>(Battery::BatteryStatus::Discharging));
        buf.append(static_cast<char>(0x01));                    // end

        QVERIFY(b.parsePacket(buf));
        QVERIFY(spy.count() >= 1);
        QCOMPARE(b.getHeadsetLevel(), quint8(85));
        QVERIFY(b.isHeadsetAvailable());
    }

    void parsePacket_rejectsBrokenSpacer()
    {
        Battery b;
        QByteArray buf = AirPodsPackets::Parse::BATTERY_STATUS;
        buf.append(static_cast<char>(1));
        buf.append(static_cast<char>(Battery::Component::Left));
        buf.append(static_cast<char>(0x42)); // spacer should be 0x01
        buf.append(static_cast<char>(50));
        buf.append(static_cast<char>(Battery::BatteryStatus::Discharging));
        buf.append(static_cast<char>(0x01));
        QVERIFY(!b.parsePacket(buf));
    }

    void resetClearsState()
    {
        Battery b;
        // Synthesize a packet so the indicator reports a level, then reset.
        QByteArray buf = AirPodsPackets::Parse::BATTERY_STATUS;
        buf.append(static_cast<char>(1));
        buf.append(static_cast<char>(Battery::Component::Headset));
        buf.append(static_cast<char>(0x01));
        buf.append(static_cast<char>(75));
        buf.append(static_cast<char>(Battery::BatteryStatus::Discharging));
        buf.append(static_cast<char>(0x01));
        QVERIFY(b.parsePacket(buf));
        QCOMPARE(b.getHeadsetLevel(), quint8(75));

        b.reset();
        QCOMPARE(b.getHeadsetLevel(), quint8(0));
        QVERIFY(!b.isHeadsetAvailable());
    }
};

QTEST_GUILESS_MAIN(TestBattery)
#include "tst_battery.moc"
