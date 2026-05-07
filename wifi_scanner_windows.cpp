#include "wifi_scanner_backend.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <windows.h>
#include <wlanapi.h>

#include <QScopeGuard>
#include <QSet>

#include <memory>

namespace {

struct WlanMemoryDeleter
{
    void operator()(void *pointer) const
    {
        if (pointer != nullptr) {
            WlanFreeMemory(pointer);
        }
    }
};

template <typename T>
using WlanMemory = std::unique_ptr<T, WlanMemoryDeleter>;

WiFiBand bandFromFrequency(int frequencyMHz)
{
    if (frequencyMHz >= 5925) {
        return WiFiBand::Band6;
    }
    if (frequencyMHz >= 5000) {
        return WiFiBand::Band5;
    }
    return WiFiBand::Band24;
}

int channelFromFrequency(int frequencyMHz)
{
    if (frequencyMHz == 2484) {
        return 14;
    }
    if (frequencyMHz >= 2412 && frequencyMHz <= 2472) {
        return (frequencyMHz - 2407) / 5;
    }
    if (frequencyMHz >= 5000 && frequencyMHz <= 5895) {
        return (frequencyMHz - 5000) / 5;
    }
    if (frequencyMHz >= 5955) {
        return (frequencyMHz - 5950) / 5;
    }
    return 0;
}

QString windowsWlanError(const QString &operation, DWORD errorCode)
{
    return QString("%1 failed with Windows error code %2.").arg(operation).arg(errorCode);
}

QString ssidText(const DOT11_SSID &ssid)
{
    if (ssid.uSSIDLength == 0) {
        return QStringLiteral("<hidden>");
    }

    return QString::fromUtf8(reinterpret_cast<const char *>(ssid.ucSSID), static_cast<int>(ssid.uSSIDLength));
}

QString bssidText(const UCHAR bssid[6])
{
    return QString("%1:%2:%3:%4:%5:%6")
        .arg(bssid[0], 2, 16, QChar('0'))
        .arg(bssid[1], 2, 16, QChar('0'))
        .arg(bssid[2], 2, 16, QChar('0'))
        .arg(bssid[3], 2, 16, QChar('0'))
        .arg(bssid[4], 2, 16, QChar('0'))
        .arg(bssid[5], 2, 16, QChar('0'))
        .toUpper();
}

void addBandSupport(BandSupport *support, WiFiBand band)
{
    if (band == WiFiBand::Band24) {
        support->has24GHz = true;
    } else if (band == WiFiBand::Band5) {
        support->has5GHz = true;
    } else if (band == WiFiBand::Band6) {
        support->has6GHz = true;
    }
}

} // namespace

WiFiScanOutcome performWifiScan()
{
    WiFiScanOutcome outcome;
    outcome.support = BandSupport{false, false, false};

    HANDLE clientHandle = nullptr;
    DWORD negotiatedVersion = 0;
    DWORD status = WlanOpenHandle(2, nullptr, &negotiatedVersion, &clientHandle);
    if (status != ERROR_SUCCESS) {
        outcome.errorMessage = windowsWlanError("WlanOpenHandle", status);
        return outcome;
    }

    auto closeClientHandle = qScopeGuard([clientHandle]() {
        WlanCloseHandle(clientHandle, nullptr);
    });

    PWLAN_INTERFACE_INFO_LIST rawInterfaces = nullptr;
    status = WlanEnumInterfaces(clientHandle, nullptr, &rawInterfaces);
    if (status != ERROR_SUCCESS) {
        outcome.errorMessage = windowsWlanError("WlanEnumInterfaces", status);
        return outcome;
    }

    WlanMemory<WLAN_INTERFACE_INFO_LIST> interfaces(rawInterfaces);
    if (interfaces->dwNumberOfItems == 0) {
        outcome.errorMessage = "No wireless interfaces were exposed by the Windows WLAN API.";
        return outcome;
    }

    QString firstScanError;
    QSet<QString> seenBssids;
    for (DWORD interfaceIndex = 0; interfaceIndex < interfaces->dwNumberOfItems; ++interfaceIndex) {
        const WLAN_INTERFACE_INFO &interfaceInfo = interfaces->InterfaceInfo[interfaceIndex];

        const DWORD scanStatus = WlanScan(clientHandle, &interfaceInfo.InterfaceGuid, nullptr, nullptr, nullptr);
        if (scanStatus != ERROR_SUCCESS && firstScanError.isEmpty()) {
            firstScanError = windowsWlanError("WlanScan", scanStatus);
        }

        Sleep(2500);

        PWLAN_BSS_LIST rawBssList = nullptr;
        status = WlanGetNetworkBssList(
            clientHandle,
            &interfaceInfo.InterfaceGuid,
            nullptr,
            dot11_BSS_type_any,
            FALSE,
            nullptr,
            &rawBssList);
        if (status != ERROR_SUCCESS) {
            if (firstScanError.isEmpty()) {
                firstScanError = windowsWlanError("WlanGetNetworkBssList", status);
            }
            continue;
        }

        WlanMemory<WLAN_BSS_LIST> bssList(rawBssList);
        for (DWORD itemIndex = 0; itemIndex < bssList->dwNumberOfItems; ++itemIndex) {
            const WLAN_BSS_ENTRY &entry = bssList->wlanBssEntries[itemIndex];
            const QString bssid = bssidText(entry.dot11Bssid);
            if (seenBssids.contains(bssid)) {
                continue;
            }

            const int frequencyMHz = static_cast<int>(entry.ulChCenterFrequency / 1000);
            const int channel = channelFromFrequency(frequencyMHz);
            if (channel <= 0) {
                continue;
            }

            WiFiNetwork network;
            network.ssid = ssidText(entry.dot11Ssid);
            network.bssid = bssid;
            network.frequencyMHz = frequencyMHz;
            network.channel = channel;
            network.band = bandFromFrequency(frequencyMHz);
            network.signalDbm = static_cast<int>(entry.lRssi);

            addBandSupport(&outcome.support, network.band);
            seenBssids.insert(bssid);
            outcome.networks.append(network);
        }
    }

    if (outcome.networks.isEmpty()) {
        outcome.errorMessage = firstScanError.isEmpty()
            ? "The Windows WLAN API did not return any scan results."
            : firstScanError;
    }

    return outcome;
}
