#ifndef WIFI_TYPES_H
#define WIFI_TYPES_H

#include <QMetaType>
#include <QString>
#include <QVector>

enum class WiFiBand
{
    Band24,
    Band5,
    Band6
};

struct WiFiNetwork
{
    QString ssid;
    QString bssid;
    int channel = 0;
    int signalDbm = -100;
    int frequencyMHz = 0;
    WiFiBand band = WiFiBand::Band24;
};

struct BandSupport
{
    bool has24GHz = true;
    bool has5GHz = false;
    bool has6GHz = false;
};

Q_DECLARE_METATYPE(WiFiNetwork)
Q_DECLARE_METATYPE(BandSupport)
Q_DECLARE_METATYPE(QVector<WiFiNetwork>)

#endif // WIFI_TYPES_H
