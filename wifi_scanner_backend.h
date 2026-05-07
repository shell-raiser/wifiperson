#ifndef WIFI_SCANNER_BACKEND_H
#define WIFI_SCANNER_BACKEND_H

#include "wifi_types.h"

#include <QString>

struct WiFiScanOutcome
{
    QVector<WiFiNetwork> networks;
    BandSupport support;
    QString errorMessage;
};

WiFiScanOutcome performWifiScan();

#endif // WIFI_SCANNER_BACKEND_H
