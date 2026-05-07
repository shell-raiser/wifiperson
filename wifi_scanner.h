#ifndef WIFI_SCANNER_H
#define WIFI_SCANNER_H

#include "wifi_types.h"

#include <QThread>

class WiFiScanThread : public QThread
{
    Q_OBJECT

public:
    explicit WiFiScanThread(QObject *parent = nullptr);

signals:
    void scanFinished(const QVector<WiFiNetwork> &networks, const BandSupport &support);
    void scanError(const QString &message, const BandSupport &support);

protected:
    void run() override;
};

#endif // WIFI_SCANNER_H
