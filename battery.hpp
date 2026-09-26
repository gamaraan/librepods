#pragma once

#include <QByteArray>
#include <QMap>
#include <QString>
#include <QObject>
#include <climits>

#include "airpods_packets.h"
#include "logger.h"

class Battery : public QObject
{
    Q_OBJECT

    Q_PROPERTY(quint8 leftPodLevel READ getLeftPodLevel NOTIFY batteryStatusChanged)
    Q_PROPERTY(bool leftPodCharging READ isLeftPodCharging NOTIFY batteryStatusChanged)
    Q_PROPERTY(bool leftPodAvailable READ isLeftPodAvailable NOTIFY batteryStatusChanged)
    Q_PROPERTY(quint8 rightPodLevel READ getRightPodLevel NOTIFY batteryStatusChanged)
    Q_PROPERTY(bool rightPodCharging READ isRightPodCharging NOTIFY batteryStatusChanged)
    Q_PROPERTY(bool rightPodAvailable READ isRightPodAvailable NOTIFY batteryStatusChanged)
    Q_PROPERTY(quint8 headsetLevel READ getHeadsetLevel NOTIFY batteryStatusChanged)
    Q_PROPERTY(bool headsetCharging READ isHeadsetCharging NOTIFY batteryStatusChanged)
    Q_PROPERTY(bool headsetAvailable READ isHeadsetAvailable NOTIFY batteryStatusChanged)
    Q_PROPERTY(quint8 caseLevel READ getCaseLevel NOTIFY batteryStatusChanged)
    Q_PROPERTY(bool caseCharging READ isCaseCharging NOTIFY batteryStatusChanged)
    Q_PROPERTY(bool caseAvailable READ isCaseAvailable NOTIFY batteryStatusChanged)

public:
    explicit Battery(QObject *parent = nullptr) : QObject(parent)
    {
        reset();
    }

    void reset()
    {
        // Initialize all components to unknown state
        states[Component::Headset] = {};
        states[Component::Left] = {};
        states[Component::Right] = {};
        states[Component::Case] = {};
        emit batteryStatusChanged();
    }

    // Enum for AirPods components
    enum class Component
    {
        Headset = 0x01, // AirPods Max
        Right = 0x02,
        Left = 0x04,
        Case = 0x08,
    };
    Q_ENUM(Component)

    enum class BatteryStatus
    {
        Charging = 0x01,
        Discharging = 0x02,
        Disconnected = 0x04,
    };
    Q_ENUM(BatteryStatus)

    // Struct to hold battery level and status
    struct BatteryState
    {
        quint8 level = 0; // Battery level (0-100), 0 if unknown
        BatteryStatus status = BatteryStatus::Disconnected;
    };

    // Parse the battery status packet and detect primary/secondary pods
    bool parsePacket(const QByteArray &packet)
    {
        // BATTERY_STATUS header is 6 bytes; startsWith alone doesn't
        // guarantee a 7th byte for the battery-count field. Require it
        // up front so packet[6] is in bounds.
        if (packet.size() < 7 || !packet.startsWith(AirPodsPackets::Parse::BATTERY_STATUS))
        {
            return false;
        }

        // Get battery count (number of components)
        quint8 batteryCount = static_cast<quint8>(packet[6]);
        if (batteryCount > 3 || packet.size() != 7 + 5 * batteryCount)
        {
            return false; // Invalid count or size mismatch
        }

        QMap<Component, BatteryState> newStates = states;

        // Track pods to determine primary and secondary based on order
        QList<Component> podsInPacket;
        podsInPacket.reserve(2);

        for (quint8 i = 0; i < batteryCount; ++i)
        {
            int offset = 7 + (5 * i);
            quint8 type = static_cast<quint8>(packet[offset]);

            // Verify spacer and end bytes
            if (static_cast<quint8>(packet[offset + 1]) != 0x01 ||
                static_cast<quint8>(packet[offset + 4]) != 0x01)
            {
                return false;
            }

            Component comp = static_cast<Component>(type);
            auto level = static_cast<quint8>(packet[offset + 2]);
            auto status = static_cast<BatteryStatus>(packet[offset + 3]);

            if (status != BatteryStatus::Disconnected)
            {
                newStates[comp] = {level, status};
            }

            // If this is a pod (Left or Right), add it to the list
            if (comp == Component::Left || comp == Component::Right || comp == Component::Headset)
            {
                podsInPacket.append(comp);
            }
        }

        // Update states
        states = newStates;

        // Set primary and secondary pods based on order
        if (!podsInPacket.isEmpty())
        {
            if (podsInPacket.count() == 1 && podsInPacket[0] == Component::Headset) {
                // AirPods Max
                primaryPod = podsInPacket[0];
                emit primaryChanged();
            } else {
                Component newPrimaryPod = podsInPacket[0]; // First pod is primary
                if (newPrimaryPod != primaryPod)
                {
                    primaryPod = newPrimaryPod;
                    emit primaryChanged();
                }
            }
        }
        if (podsInPacket.size() >= 2)
        {
            secondaryPod = podsInPacket[1]; // Second pod is secondary
        }

        // Emit signal to notify about battery status change
        emit batteryStatusChanged();

        if (primaryPod == Component::Headset) {
            LOG_INFO("Primary Pod:" << primaryPod);
        } else {
            // Log which is left and right pod
            LOG_INFO("Primary Pod:" << primaryPod);
            LOG_INFO("Secondary Pod:" << secondaryPod);
        }

        return true;
    }

    // Get the raw state for a component
    BatteryState getState(Component comp) const
    {
        return states.value(comp, {});
    }

    // Get a formatted status string including charging state
    QString getComponentStatus(Component comp) const
    {
        BatteryState state = getState(comp);
        if (state.level == 0)
        {
            return "Unknown";
        }

        QString statusStr;
        switch (state.status)
        {
        case BatteryStatus::Charging:
            statusStr = "Charging";
            break;
        case BatteryStatus::Discharging:
            statusStr = "Discharging";
            break;
        case BatteryStatus::Disconnected:
            statusStr = "Disconnected";
            break;
        default:
            statusStr = "Invalid";
            break;
        }
        return QString("%1% (%2)").arg(state.level).arg(statusStr);
    }

    Component getPrimaryPod() const { return primaryPod; }
    Component getSecondaryPod() const { return secondaryPod; }

    quint8 getLeftPodLevel() const { return states.value(Component::Left).level; }
    bool isLeftPodCharging() const { return isStatus(Component::Left, BatteryStatus::Charging); }
    bool isLeftPodAvailable() const { return !isStatus(Component::Left, BatteryStatus::Disconnected); }
    quint8 getRightPodLevel() const { return states.value(Component::Right).level; }
    bool isRightPodCharging() const { return isStatus(Component::Right, BatteryStatus::Charging); }
    bool isRightPodAvailable() const { return !isStatus(Component::Right, BatteryStatus::Disconnected); }
    quint8 getCaseLevel() const { return states.value(Component::Case).level; }
    bool isCaseCharging() const { return isStatus(Component::Case, BatteryStatus::Charging); }
    bool isCaseAvailable() const { return !isStatus(Component::Case, BatteryStatus::Disconnected); }

    quint8 getHeadsetLevel() const { return states.value(Component::Headset).level; }
    bool isHeadsetCharging() const { return isStatus(Component::Headset, BatteryStatus::Charging); }
    bool isHeadsetAvailable() const { return !isStatus(Component::Headset, BatteryStatus::Disconnected); }

signals:
    void batteryStatusChanged();
    void primaryChanged();

private:
    bool isStatus(Component component, BatteryStatus status) const
    {
        return states.value(component).status == status;
    }

    QMap<Component, BatteryState> states;
    Component primaryPod;
    Component secondaryPod;
};
