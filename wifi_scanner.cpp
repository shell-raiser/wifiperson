#include "wifi_scanner.h"

#include "wifi_scanner_backend.h"

WiFiScanThread::WiFiScanThread(QObject *parent)
    : QThread(parent)
{
}

void WiFiScanThread::run()
{
    const WiFiScanOutcome outcome = performWifiScan();
    if (!outcome.errorMessage.isEmpty()) {
        emit scanError(outcome.errorMessage, outcome.support);
        return;
    }

    emit scanFinished(outcome.networks, outcome.support);
}
