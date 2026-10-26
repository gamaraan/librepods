#include "blemanager.h"
#include "enums.h"
#include <QDebug>
#include <QDBusArgument>
#include <QDBusConnection>
#include <QDBusMessage>
#include <QDBusObjectPath>
#include <QTimer>
#include "logger.h"
#include <QMap>

namespace
{

typedef QMap<QDBusObjectPath, QMap<QString, QVariantMap>> ManagedObjectList;

// bluetoothd answers in milliseconds; never let a wedged bus hold the window guard.
constexpr int stopDiscoveryTimeoutMs = 1000;

// BlueZ keeps one discovery session per D-Bus client, so this only drops
// librepods' own session - a scan owned by another client keeps the adapter
// discovering, which is that client's business. Needed because Qt's discovery
// manager can skip StopDiscovery when it registered while somebody else was
// already scanning, and the session then outlives the window.
void releaseDiscoverySessions()
{
    QDBusConnection bus = QDBusConnection::systemBus();
    QDBusMessage request = QDBusMessage::createMethodCall(
        QStringLiteral("org.bluez"), QStringLiteral("/"),
        QStringLiteral("org.freedesktop.DBus.ObjectManager"),
        QStringLiteral("GetManagedObjects"));
    QDBusMessage reply = bus.call(request, QDBus::Block, stopDiscoveryTimeoutMs);
    if (reply.type() == QDBusMessage::ErrorMessage || reply.arguments().isEmpty())
    {
        return;
    }

    ManagedObjectList objects;
    reply.arguments().constFirst().value<QDBusArgument>() >> objects;

    for (auto it = objects.constBegin(); it != objects.constEnd(); ++it)
    {
        if (!it.value().contains(QStringLiteral("org.bluez.Adapter1")))
        {
            continue;
        }

        QDBusMessage stop = QDBusMessage::createMethodCall(
            QStringLiteral("org.bluez"), it.key().path(),
            QStringLiteral("org.bluez.Adapter1"), QStringLiteral("StopDiscovery"));
        // "No discovery started" just means this client had no session; ignore it.
        bus.call(stop, QDBus::Block, stopDiscoveryTimeoutMs);
    }
}

} // namespace

// Fixed header data[0] through data[10], then a 16-byte encrypted payload.
static constexpr int proximityPairingBytes = 11 + 16;

AirpodsTrayApp::Enums::AirPodsModel getModelName(quint16 modelId)
{
    using namespace AirpodsTrayApp::Enums;
    static const QMap<quint16, AirPodsModel> modelMap = {
        {0x0220, AirPodsModel::AirPods1},
        {0x0F20, AirPodsModel::AirPods2},
        {0x1320, AirPodsModel::AirPods3},
        {0x1920, AirPodsModel::AirPods4},
        {0x1B20, AirPodsModel::AirPods4ANC},
        {0x0A20, AirPodsModel::AirPodsMaxLightning},
        {0x1F20, AirPodsModel::AirPodsMaxUSBC},
        {0x0E20, AirPodsModel::AirPodsPro},
        {0x1420, AirPodsModel::AirPodsPro2Lightning},
        {0x2420, AirPodsModel::AirPodsPro2USBC},
        // Apple product 0x2027 arrives byte-swapped, so data[3] is 0x27 and this key is 0x2720.
        {0x2720, AirPodsModel::AirPodsPro3}
        // No AirPods Max 2 id yet: AAP names it instead, and an unrecognized id is ignored.
    };

    return modelMap.value(modelId, AirPodsModel::Unknown);
}

QString getColorName(quint8 colorId)
{
    switch (colorId)
    {
    case 0x00:
        return "White";
    case 0x01:
        return "Black";
    case 0x02:
        return "Red";
    case 0x03:
        return "Blue";
    case 0x04:
        return "Pink";
    case 0x05:
        return "Gray";
    case 0x06:
        return "Silver";
    case 0x07:
        return "Gold";
    case 0x08:
        return "Rose Gold";
    case 0x09:
        return "Space Gray";
    case 0x0A:
        return "Dark Blue";
    case 0x0B:
        return "Light Blue";
    case 0x0C:
        return "Yellow";
    default:
        return "Unknown";
    }
}

QString getConnectionStateName(BleInfo::ConnectionState state)
{
    using ConnectionState = BleInfo::ConnectionState;
    switch (state)
    {
    case ConnectionState::DISCONNECTED:
        return QString("Disconnected");
    case ConnectionState::IDLE:
        return QString("Idle");
    case ConnectionState::MUSIC:
        return QString("Playing Music");
    case ConnectionState::CALL:
        return QString("On Call");
    case ConnectionState::RINGING:
        return QString("Ringing");
    case ConnectionState::HANGING_UP:
        return QString("Hanging Up");
    case ConnectionState::UNKNOWN:
    default:
        return QString("Unknown");
    }
}

BleManager::BleManager(QObject *parent) : QObject(parent)
{
    discoveryAgent = new QBluetoothDeviceDiscoveryAgent(this);
    discoveryAgent->setLowEnergyDiscoveryTimeout(ScanDuty::scanWindowMs);

    idleTimer = new QTimer(this);
    idleTimer->setSingleShot(true);
    connect(idleTimer, &QTimer::timeout, this, &BleManager::onIdleFinished);

    windowGuard = new QTimer(this);
    windowGuard->setSingleShot(true);
    connect(windowGuard, &QTimer::timeout, this, &BleManager::onWindowOverrun);

    connect(discoveryAgent, &QBluetoothDeviceDiscoveryAgent::deviceDiscovered,
            this, &BleManager::onDeviceDiscovered);
    connect(discoveryAgent, &QBluetoothDeviceDiscoveryAgent::finished,
            this, &BleManager::onScanFinished);
    connect(discoveryAgent, &QBluetoothDeviceDiscoveryAgent::errorOccurred,
            this, &BleManager::onErrorOccurred);
}

BleManager::~BleManager()
{
    // discoveryAgent is QObject-parented to `this` (see ctor:
    // `new QBluetoothDeviceDiscoveryAgent(this)`). Qt's parent-child
    // ownership deletes it for us when this dtor returns — the
    // explicit `delete discoveryAgent` that used to live here was
    // redundant and ran before Qt's own child cleanup pass. Now empty.
}

void BleManager::startScan()
{
    LOG_DEBUG("Starting BLE scan...");
    duty.request();
    // A start inside the gap has to disarm the pending resume, or the next tick starts a second window.
    idleTimer->stop();
    discoveryAgent->start(QBluetoothDeviceDiscoveryAgent::LowEnergyMethod);
    windowGuard->start(ScanDuty::scanWindowMs + ScanDuty::guardSlackMs);
}

void BleManager::stopScan()
{
    LOG_DEBUG("Stopping BLE scan...");
    duty.cancel();
    idleTimer->stop();
    windowGuard->stop();
    discoveryAgent->stop();
}

bool BleManager::isScanning() const
{
    return duty.isRequested();
}

void BleManager::onDeviceDiscovered(const QBluetoothDeviceInfo &info)
{
    // Check for Apple's manufacturer ID (0x004C)
    if (info.manufacturerData().contains(0x004C))
    {
        QByteArray data = info.manufacturerData().value(0x004C);
        // Sample input: 07 19 01 27 20 21 88 8F 11 00 04 <16 encrypted bytes>
        // A shorter frame reads past data[10] and slices a wrong payload below, and any BLE device in range can send one.
        if (data.size() >= proximityPairingBytes && data[0] == 0x07)
        {
            QString address = info.address().toString();
            BleInfo deviceInfo;
            deviceInfo.name = info.name().isEmpty() ? "AirPods" : info.name();
            deviceInfo.address = address;
            deviceInfo.rawData = data.left(data.size() - 16);
            deviceInfo.encryptedPayload = data.mid(data.size() - 16);

            // data[1] is the length of the data, so we can skip it

            // Check if pairing mode is paired (0x01) or pairing (0x00)
            if (data[2] == 0x00)
            {
                return; // Skip pairing mode devices (the values are differently structured)
            }

            
            // Parse device model (big-endian: high byte at data[3], low byte at data[4])
            deviceInfo.modelName = getModelName(static_cast<quint16>(data[4]) | (static_cast<quint8>(data[3]) << 8));

            // Status byte for primary pod and other flags
            quint8 status = static_cast<quint8>(data[5]);
            deviceInfo.status = status;

            // Pods battery byte (upper nibble: one pod, lower nibble: other pod)
            quint8 podsBatteryByte = static_cast<quint8>(data[6]);

            // Flags and case battery byte (upper nibble: case battery, lower nibble: flags)
            quint8 flagsAndCaseBattery = static_cast<quint8>(data[7]);

            // Lid open counter and device color
            quint8 lidIndicator = static_cast<quint8>(data[8]);
            deviceInfo.color = getColorName((quint8)(data[9]));

            deviceInfo.connectionState = static_cast<BleInfo::ConnectionState>(data[10]);

            // Next: Encrypted Payload: 16 bytes

            // Determine primary pod (bit 5 of status) and value flipping
            bool primaryLeft = (status & 0x20) != 0; // Bit 5: 1 = left primary, 0 = right primary
            bool areValuesFlipped = !primaryLeft;    // Flipped when right pod is primary

            deviceInfo.primaryLeft = primaryLeft; // Store primary pod information

            // Parse battery levels
            int leftNibble = areValuesFlipped ? (podsBatteryByte >> 4) & 0x0F : podsBatteryByte & 0x0F;
            int rightNibble = areValuesFlipped ? podsBatteryByte & 0x0F : (podsBatteryByte >> 4) & 0x0F;
            deviceInfo.leftPodBattery = (leftNibble == 15) ? -1 : leftNibble * 10;
            deviceInfo.rightPodBattery = (rightNibble == 15) ? -1 : rightNibble * 10;
            int caseNibble = flagsAndCaseBattery & 0x0F; // Extracts lower nibble
            deviceInfo.caseBattery = (caseNibble == 15) ? -1 : caseNibble * 10;

            // Parse charging statuses from flags (uper 4 bits of data[7])
            quint8 flags = (flagsAndCaseBattery >> 4) & 0x0F;                                        // Extracts lower nibble
            deviceInfo.rightCharging = areValuesFlipped ? (flags & 0x01) != 0 : (flags & 0x02) != 0; // Depending on primary, bit 0 or 1
            deviceInfo.leftCharging = areValuesFlipped ? (flags & 0x02) != 0 : (flags & 0x01) != 0;  // Depending on primary, bit 1 or 0
            deviceInfo.caseCharging = (flags & 0x04) != 0;                                           // bit 2

            // Additional status flags from status byte (data[5])
            deviceInfo.isThisPodInTheCase = (status & 0x40) != 0; // Bit 6
            deviceInfo.isOnePodInCase = (status & 0x10) != 0;     // Bit 4
            deviceInfo.areBothPodsInCase = (status & 0x04) != 0;  // Bit 2

            // In-ear detection with XOR logic
            bool xorFactor = areValuesFlipped ^ deviceInfo.isThisPodInTheCase;
            deviceInfo.isLeftPodInEar = xorFactor ? (status & 0x08) != 0 : (status & 0x02) != 0;  // Bit 3 or 1
            deviceInfo.isRightPodInEar = xorFactor ? (status & 0x02) != 0 : (status & 0x08) != 0; // Bit 1 or 3

            // Determine primary and secondary in-ear status
            deviceInfo.isPrimaryInEar = primaryLeft ? deviceInfo.isLeftPodInEar : deviceInfo.isRightPodInEar;
            deviceInfo.isSecondaryInEar = primaryLeft ? deviceInfo.isRightPodInEar : deviceInfo.isLeftPodInEar;

            // Microphone status
            deviceInfo.isLeftPodMicrophone = primaryLeft ^ deviceInfo.isThisPodInTheCase;
            deviceInfo.isRightPodMicrophone = !primaryLeft ^ deviceInfo.isThisPodInTheCase;

            deviceInfo.lidOpenCounter = lidIndicator & 0x07; // Extract bits 0-2 (count)
            quint8 lidState = static_cast<quint8>((lidIndicator >> 3) & 0x01); // Extract bit 3 (lid state)
            if (deviceInfo.isThisPodInTheCase) {
                deviceInfo.lidState = static_cast<BleInfo::LidState>(lidState);
            }

            // Update timestamp
            deviceInfo.lastSeen = QDateTime::currentDateTime();

            emit deviceFound(deviceInfo); // Emit signal for device found
        }
    }
}

void BleManager::onScanFinished()
{
    LOG_DEBUG("BLE scan window finished");
    windowGuard->stop();
    releaseDiscoverySessions();
    if (!duty.windowFinished())
    {
        return;
    }
    idleTimer->start(ScanDuty::idleWindowMs);
}

void BleManager::onIdleFinished()
{
    if (!duty.idleFinished())
    {
        return;
    }
    discoveryAgent->start(QBluetoothDeviceDiscoveryAgent::LowEnergyMethod);
    windowGuard->start(ScanDuty::scanWindowMs + ScanDuty::guardSlackMs);
}

void BleManager::onWindowOverrun()
{
    // The window's own timeout did not arrive, which happens when another process
    // changes the adapter's discovery state while the agent is starting: Qt's
    // start() bails out early and never arms its LE timer. Stop the agent so the
    // session is released, then make sure the cycle still advances even if no
    // finished() comes back.
    LOG_WARN("BLE scan window overran, forcing stop");
    discoveryAgent->stop();
    releaseDiscoverySessions();

    if (duty.phase() == ScanDuty::Phase::Scanning && duty.windowFinished())
    {
        idleTimer->start(ScanDuty::idleWindowMs);
    }
}

void BleManager::onErrorOccurred(QBluetoothDeviceDiscoveryAgent::Error error)
{
    LOG_ERROR("BLE scan error occurred:" << error);
    stopScan();
}
