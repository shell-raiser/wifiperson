#include "wifi_scanner_backend.h"

#include <linux/nl80211.h>

#include <netlink/attr.h>
#include <netlink/genl/genl.h>
#include <netlink/genl/ctrl.h>
#include <netlink/msg.h>
#include <netlink/socket.h>

#include <QSet>
#include <QThread>

#include <cstdint>

namespace {

struct NlContext
{
    nl_sock *socket = nullptr;
    int familyId = -1;
};

struct InterfaceDumpContext
{
    QVector<int> interfaceIndexes;
};

struct WiphyDumpContext
{
    BandSupport support;
};

struct ScanDumpContext
{
    QVector<WiFiNetwork> networks;
    QSet<QString> seenBssids;
};

int handleNetlinkError(sockaddr_nl *, nlmsgerr *err, void *arg)
{
    *static_cast<int *>(arg) = err->error;
    return NL_STOP;
}

int handleNetlinkAck(nl_msg *, void *arg)
{
    *static_cast<int *>(arg) = 0;
    return NL_STOP;
}

int handleNetlinkFinish(nl_msg *, void *arg)
{
    *static_cast<int *>(arg) = 0;
    return NL_SKIP;
}

int ignoreValidMessage(nl_msg *, void *)
{
    return NL_SKIP;
}

int executeNetlinkRequest(
    nl_sock *socket,
    nl_msg *message,
    nl_recvmsg_msg_cb_t validHandler,
    void *validHandlerData,
    QString *errorMessage)
{
    nl_cb *callbacks = nl_cb_alloc(NL_CB_DEFAULT);
    if (callbacks == nullptr) {
        nlmsg_free(message);
        if (errorMessage != nullptr) {
            *errorMessage = "Failed to allocate libnl callbacks.";
        }
        return -NLE_NOMEM;
    }

    int status = 1;
    nl_cb_set(callbacks, NL_CB_VALID, NL_CB_CUSTOM, validHandler, validHandlerData);
    nl_cb_err(callbacks, NL_CB_CUSTOM, handleNetlinkError, &status);
    nl_cb_set(callbacks, NL_CB_FINISH, NL_CB_CUSTOM, handleNetlinkFinish, &status);
    nl_cb_set(callbacks, NL_CB_ACK, NL_CB_CUSTOM, handleNetlinkAck, &status);

    const int sendStatus = nl_send_auto(socket, message);
    if (sendStatus < 0) {
        if (errorMessage != nullptr) {
            *errorMessage = QString("Failed to send nl80211 request: %1").arg(nl_geterror(sendStatus));
        }
        nl_cb_put(callbacks);
        nlmsg_free(message);
        return sendStatus;
    }

    while (status > 0) {
        const int receiveStatus = nl_recvmsgs(socket, callbacks);
        if (receiveStatus < 0) {
            status = receiveStatus;
            break;
        }
    }

    if (status < 0 && errorMessage != nullptr) {
        *errorMessage = QString("nl80211 request failed: %1").arg(nl_geterror(status));
    }

    nl_cb_put(callbacks);
    nlmsg_free(message);
    return status;
}

bool openNetlinkContext(NlContext *context, QString *errorMessage)
{
    context->socket = nl_socket_alloc();
    if (context->socket == nullptr) {
        if (errorMessage != nullptr) {
            *errorMessage = "Failed to allocate libnl socket.";
        }
        return false;
    }

    const int connectStatus = genl_connect(context->socket);
    if (connectStatus < 0) {
        if (errorMessage != nullptr) {
            *errorMessage = QString("Failed to connect to generic netlink: %1").arg(nl_geterror(connectStatus));
        }
        nl_socket_free(context->socket);
        context->socket = nullptr;
        return false;
    }

    context->familyId = genl_ctrl_resolve(context->socket, NL80211_GENL_NAME);
    if (context->familyId < 0) {
        if (errorMessage != nullptr) {
            *errorMessage = QString("Failed to resolve nl80211 family: %1").arg(nl_geterror(context->familyId));
        }
        nl_socket_free(context->socket);
        context->socket = nullptr;
        return false;
    }

    return true;
}

void closeNetlinkContext(NlContext *context)
{
    if (context->socket != nullptr) {
        nl_socket_free(context->socket);
        context->socket = nullptr;
    }
}

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

QString parseSsidFromInformationElements(const uint8_t *data, int length)
{
    int offset = 0;
    while (offset + 2 <= length) {
        const uint8_t elementId = data[offset];
        const int elementLength = data[offset + 1];
        offset += 2;
        if (offset + elementLength > length) {
            break;
        }

        if (elementId == 0) {
            return QString::fromUtf8(reinterpret_cast<const char *>(data + offset), elementLength);
        }

        offset += elementLength;
    }

    return QString();
}

int signalDbmFromAttributes(const nlattr *signalMbmAttr, const nlattr *signalUnspecAttr)
{
    if (signalMbmAttr != nullptr) {
        const auto milliBelMillwatt = static_cast<int32_t>(nla_get_u32(signalMbmAttr));
        return milliBelMillwatt / 100;
    }

    if (signalUnspecAttr != nullptr) {
        const int signalPercent = nla_get_u8(signalUnspecAttr);
        return -100 + qBound(0, signalPercent, 100) / 2;
    }

    return -100;
}

int interfaceDumpCallback(nl_msg *message, void *arg)
{
    auto *context = static_cast<InterfaceDumpContext *>(arg);
    nlmsghdr *header = nlmsg_hdr(message);
    genlmsghdr *genlHeader = static_cast<genlmsghdr *>(nlmsg_data(header));

    nlattr *attributes[NL80211_ATTR_MAX + 1] = {};
    nla_parse(attributes, NL80211_ATTR_MAX, genlmsg_attrdata(genlHeader, 0), genlmsg_attrlen(genlHeader, 0), nullptr);

    if (attributes[NL80211_ATTR_IFINDEX] == nullptr) {
        return NL_SKIP;
    }

    const int interfaceIndex = nla_get_u32(attributes[NL80211_ATTR_IFINDEX]);
    if (!context->interfaceIndexes.contains(interfaceIndex)) {
        context->interfaceIndexes.append(interfaceIndex);
    }

    return NL_SKIP;
}

int wiphyDumpCallback(nl_msg *message, void *arg)
{
    auto *context = static_cast<WiphyDumpContext *>(arg);
    nlmsghdr *header = nlmsg_hdr(message);
    genlmsghdr *genlHeader = static_cast<genlmsghdr *>(nlmsg_data(header));

    nlattr *attributes[NL80211_ATTR_MAX + 1] = {};
    nla_parse(attributes, NL80211_ATTR_MAX, genlmsg_attrdata(genlHeader, 0), genlmsg_attrlen(genlHeader, 0), nullptr);

    if (attributes[NL80211_ATTR_WIPHY_BANDS] == nullptr) {
        return NL_SKIP;
    }

    nlattr *bandAttr = nullptr;
    int bandRemaining = 0;
    nla_for_each_nested(bandAttr, attributes[NL80211_ATTR_WIPHY_BANDS], bandRemaining) {
        nlattr *bandInfo[NL80211_BAND_ATTR_MAX + 1] = {};
        nla_parse(bandInfo, NL80211_BAND_ATTR_MAX, static_cast<nlattr *>(nla_data(bandAttr)), nla_len(bandAttr), nullptr);

        if (bandInfo[NL80211_BAND_ATTR_FREQS] == nullptr) {
            continue;
        }

        nlattr *frequencyAttr = nullptr;
        int frequencyRemaining = 0;
        nla_for_each_nested(frequencyAttr, bandInfo[NL80211_BAND_ATTR_FREQS], frequencyRemaining) {
            nlattr *frequencyInfo[NL80211_FREQUENCY_ATTR_MAX + 1] = {};
            nla_parse(
                frequencyInfo,
                NL80211_FREQUENCY_ATTR_MAX,
                static_cast<nlattr *>(nla_data(frequencyAttr)),
                nla_len(frequencyAttr),
                nullptr);

            if (frequencyInfo[NL80211_FREQUENCY_ATTR_FREQ] == nullptr) {
                continue;
            }

            const int frequencyMHz = nla_get_u32(frequencyInfo[NL80211_FREQUENCY_ATTR_FREQ]);
            if (frequencyMHz >= 2400 && frequencyMHz <= 2500) {
                context->support.has24GHz = true;
            } else if (frequencyMHz >= 5000 && frequencyMHz < 5925) {
                context->support.has5GHz = true;
            } else if (frequencyMHz >= 5925) {
                context->support.has6GHz = true;
            }
        }
    }

    return NL_SKIP;
}

int scanDumpCallback(nl_msg *message, void *arg)
{
    auto *context = static_cast<ScanDumpContext *>(arg);
    nlmsghdr *header = nlmsg_hdr(message);
    genlmsghdr *genlHeader = static_cast<genlmsghdr *>(nlmsg_data(header));

    nlattr *attributes[NL80211_ATTR_MAX + 1] = {};
    nla_parse(attributes, NL80211_ATTR_MAX, genlmsg_attrdata(genlHeader, 0), genlmsg_attrlen(genlHeader, 0), nullptr);
    if (attributes[NL80211_ATTR_BSS] == nullptr) {
        return NL_SKIP;
    }

    nlattr *bss[NL80211_BSS_MAX + 1] = {};
    nla_parse_nested(bss, NL80211_BSS_MAX, attributes[NL80211_ATTR_BSS], nullptr);

    if (bss[NL80211_BSS_BSSID] == nullptr || bss[NL80211_BSS_FREQUENCY] == nullptr) {
        return NL_SKIP;
    }

    const auto *bssid = static_cast<const uint8_t *>(nla_data(bss[NL80211_BSS_BSSID]));
    const QString bssidText = QString("%1:%2:%3:%4:%5:%6")
                                  .arg(bssid[0], 2, 16, QChar('0'))
                                  .arg(bssid[1], 2, 16, QChar('0'))
                                  .arg(bssid[2], 2, 16, QChar('0'))
                                  .arg(bssid[3], 2, 16, QChar('0'))
                                  .arg(bssid[4], 2, 16, QChar('0'))
                                  .arg(bssid[5], 2, 16, QChar('0'))
                                  .toUpper();

    if (context->seenBssids.contains(bssidText)) {
        return NL_SKIP;
    }

    const int frequencyMHz = nla_get_u32(bss[NL80211_BSS_FREQUENCY]);
    const int channel = channelFromFrequency(frequencyMHz);
    if (channel <= 0) {
        return NL_SKIP;
    }

    QString ssid;
    if (bss[NL80211_BSS_INFORMATION_ELEMENTS] != nullptr) {
        ssid = parseSsidFromInformationElements(
            static_cast<const uint8_t *>(nla_data(bss[NL80211_BSS_INFORMATION_ELEMENTS])),
            nla_len(bss[NL80211_BSS_INFORMATION_ELEMENTS]));
    }
    if (ssid.isEmpty() && bss[NL80211_BSS_BEACON_IES] != nullptr) {
        ssid = parseSsidFromInformationElements(
            static_cast<const uint8_t *>(nla_data(bss[NL80211_BSS_BEACON_IES])),
            nla_len(bss[NL80211_BSS_BEACON_IES]));
    }
    if (ssid.isEmpty()) {
        ssid = QStringLiteral("<hidden>");
    }

    WiFiNetwork network;
    network.ssid = ssid;
    network.bssid = bssidText;
    network.frequencyMHz = frequencyMHz;
    network.channel = channel;
    network.band = bandFromFrequency(frequencyMHz);
    network.signalDbm = signalDbmFromAttributes(bss[NL80211_BSS_SIGNAL_MBM], bss[NL80211_BSS_SIGNAL_UNSPEC]);

    context->seenBssids.insert(bssidText);
    context->networks.append(network);
    return NL_SKIP;
}

BandSupport queryBandSupport(NlContext *context, QString *errorMessage)
{
    WiphyDumpContext dumpContext;

    nl_msg *message = nlmsg_alloc();
    if (message == nullptr) {
        if (errorMessage != nullptr) {
            *errorMessage = "Failed to allocate wiphy query message.";
        }
        return dumpContext.support;
    }

    genlmsg_put(message, 0, 0, context->familyId, 0, NLM_F_DUMP, NL80211_CMD_GET_WIPHY, 0);
    nla_put_flag(message, NL80211_ATTR_SPLIT_WIPHY_DUMP);
    executeNetlinkRequest(context->socket, message, wiphyDumpCallback, &dumpContext, errorMessage);
    return dumpContext.support;
}

QVector<int> queryInterfaceIndexes(NlContext *context, QString *errorMessage)
{
    InterfaceDumpContext dumpContext;

    nl_msg *message = nlmsg_alloc();
    if (message == nullptr) {
        if (errorMessage != nullptr) {
            *errorMessage = "Failed to allocate interface query message.";
        }
        return {};
    }

    genlmsg_put(message, 0, 0, context->familyId, 0, NLM_F_DUMP, NL80211_CMD_GET_INTERFACE, 0);
    if (executeNetlinkRequest(context->socket, message, interfaceDumpCallback, &dumpContext, errorMessage) < 0) {
        return {};
    }

    return dumpContext.interfaceIndexes;
}

QVector<WiFiNetwork> queryScanResults(NlContext *context, const QVector<int> &interfaceIndexes, QString *errorMessage)
{
    ScanDumpContext dumpContext;

    for (int interfaceIndex : interfaceIndexes) {
        nl_msg *message = nlmsg_alloc();
        if (message == nullptr) {
            if (errorMessage != nullptr) {
                *errorMessage = "Failed to allocate scan query message.";
            }
            return {};
        }

        genlmsg_put(message, 0, 0, context->familyId, 0, NLM_F_DUMP, NL80211_CMD_GET_SCAN, 0);
        nla_put_u32(message, NL80211_ATTR_IFINDEX, interfaceIndex);

        QString requestError;
        if (executeNetlinkRequest(context->socket, message, scanDumpCallback, &dumpContext, &requestError) < 0) {
            if (errorMessage != nullptr && errorMessage->isEmpty()) {
                *errorMessage = requestError;
            }
        }
    }

    return dumpContext.networks;
}

bool triggerActiveScan(NlContext *context, int interfaceIndex, QString *errorMessage)
{
    nl_msg *message = nlmsg_alloc();
    if (message == nullptr) {
        if (errorMessage != nullptr) {
            *errorMessage = "Failed to allocate active scan message.";
        }
        return false;
    }

    genlmsg_put(message, 0, 0, context->familyId, 0, 0, NL80211_CMD_TRIGGER_SCAN, 0);
    nla_put_u32(message, NL80211_ATTR_IFINDEX, interfaceIndex);

    nlattr *ssidNest = nla_nest_start(message, NL80211_ATTR_SCAN_SSIDS);
    if (ssidNest == nullptr) {
        nlmsg_free(message);
        if (errorMessage != nullptr) {
            *errorMessage = "Failed to build scan SSID list.";
        }
        return false;
    }

    static const char wildcardSsid[] = "";
    nla_put(message, 1, 0, wildcardSsid);
    nla_nest_end(message, ssidNest);

    return executeNetlinkRequest(context->socket, message, ignoreValidMessage, nullptr, errorMessage) >= 0;
}

QString normalizeScanFailureMessage(const QString &message)
{
    if (message.contains("Network is down", Qt::CaseInsensitive)
        || message.contains("RF-kill", Qt::CaseInsensitive)
        || message.contains("No wireless interfaces", Qt::CaseInsensitive)) {
        return "Wi-Fi appears to be turned off. Turn on Wi-Fi and try again.";
    }

    return message;
}

bool isWifiOffFailure(const QString &message)
{
    return message.contains("Turn on Wi-Fi", Qt::CaseInsensitive)
        || message.contains("Network is down", Qt::CaseInsensitive)
        || message.contains("RF-kill", Qt::CaseInsensitive)
        || message.contains("No wireless interfaces", Qt::CaseInsensitive);
}

bool isRecoverableTriggerFailure(const QString &message)
{
    return message.contains("busy", Qt::CaseInsensitive)
        || message.contains("Operation not permitted", Qt::CaseInsensitive)
        || message.contains("Permission denied", Qt::CaseInsensitive)
        || message.contains("Device or resource busy", Qt::CaseInsensitive);
}

WiFiScanOutcome performLinuxWifiScan()
{
    WiFiScanOutcome outcome;
    QString errorMessage;
    NlContext context;
    if (!openNetlinkContext(&context, &errorMessage)) {
        outcome.errorMessage = errorMessage;
        return outcome;
    }

    const BandSupport support = queryBandSupport(&context, nullptr);
    outcome.support = support;
    const QVector<int> interfaceIndexes = queryInterfaceIndexes(&context, &errorMessage);
    if (interfaceIndexes.isEmpty()) {
        closeNetlinkContext(&context);
        if (errorMessage.isEmpty()) {
            errorMessage = "No wireless interfaces were exposed by nl80211.";
        }
        outcome.errorMessage = normalizeScanFailureMessage(errorMessage);
        return outcome;
    }

    QString triggerError;
    const bool triggerWorked = triggerActiveScan(&context, interfaceIndexes.first(), &triggerError);
    if (!triggerWorked) {
        triggerError = normalizeScanFailureMessage(triggerError);
        if (isWifiOffFailure(triggerError)) {
            closeNetlinkContext(&context);
            outcome.errorMessage = triggerError;
            return outcome;
        }
    }

    if (triggerWorked) {
        QThread::msleep(2500);
    }

    const QVector<WiFiNetwork> networks = queryScanResults(&context, interfaceIndexes, &errorMessage);
    closeNetlinkContext(&context);

    if (networks.isEmpty()) {
        if (!triggerWorked && isRecoverableTriggerFailure(triggerError)) {
            outcome.errorMessage =
                "No cached Wi-Fi scan results are available yet. "
                "Try scanning once from the system Wi-Fi UI or wait a moment and refresh again.";
            return outcome;
        }

        if (errorMessage.isEmpty()) {
            errorMessage =
                "nl80211 did not return any scan results. The interface may need a recent scan, or access may be restricted.";
        }
        outcome.errorMessage = normalizeScanFailureMessage(errorMessage);
        return outcome;
    }

    outcome.networks = networks;
    return outcome;
}

} // namespace

WiFiScanOutcome performWifiScan()
{
    return performLinuxWifiScan();
}
