#ifndef MAINWINDOW_H
#define MAINWINDOW_H

#include "wifi_types.h"

#include <QColor>
#include <QMainWindow>
#include <QPoint>
#include <QPointF>
#include <QTimer>
#include <QVector>

QT_BEGIN_NAMESPACE
class QComboBox;
class QLabel;
class QPushButton;
class QStackedWidget;
class QTableWidget;
class QToolButton;
class QWidget;
class QChart;
class QChartView;
class QCategoryAxis;
class QValueAxis;
QT_END_NAMESPACE

class WiFiScanThread;

class MainWindow : public QMainWindow
{
    Q_OBJECT

public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;
    void changeEvent(QEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;

private slots:
    void refreshScan();
    void handleScanFinished(const QVector<WiFiNetwork> &networks, const BandSupport &support);
    void handleScanError(const QString &message, const BandSupport &support);
    void showNetworkDetails(int row, int column);
    void showNetworkList();
    void updateScanInterval(int index);
    void updateScanSpinner();

private:
    QVector<WiFiNetwork> filteredNetworks() const;
    QVector<QPointF> buildSpectrumEnvelope(const WiFiNetwork &network) const;
    QVector<WiFiNetwork> interferingNetworks(const WiFiNetwork &selected) const;
    QString bandLabel(WiFiBand band) const;
    QString channelLabel(const WiFiNetwork &network) const;
    QColor colorForBand(WiFiBand band, int index) const;
    qreal horizontalDistanceToSegment(const QPointF &point, const QPointF &a, const QPointF &b) const;
    void applyFilters();
    void finishScanWithNetworks(const QVector<WiFiNetwork> &networks);
    void showInvalidState(const QString &message);
    void setupUi();
    void restoreRefreshButton();
    void setRefreshButtonLoading(bool loading);
    void syncBandButtons();
    void updateBandButtonVisibility();
    void updateCombinedBandAvailability();
    void updateChart(const QVector<WiFiNetwork> &networks);
    void updateChartTheme();
    void updateDetailTable(const WiFiNetwork &network);
    void updateHoverTooltip(const QPoint &mousePos);
    void updateTable(const QVector<WiFiNetwork> &networks);
    void setStatusMessage(const QString &message) const;

    QStackedWidget *pageStack = nullptr;
    QWidget *listPage = nullptr;
    QWidget *detailPage = nullptr;
    QChartView *chartView = nullptr;
    QChart *chart = nullptr;
    QCategoryAxis *axisX = nullptr;
    QValueAxis *axisY = nullptr;
    QTableWidget *networkTable = nullptr;
    QTableWidget *detailTable = nullptr;
    QTableWidget *interferenceTable = nullptr;
    QLabel *invalidStateLabel = nullptr;
    QLabel *interferenceLabel = nullptr;
    QLabel *statusLabel = nullptr;
    QPushButton *refreshButton = nullptr;
    QPushButton *backButton = nullptr;
    QComboBox *scanIntervalCombo = nullptr;
    QToolButton *band24Button = nullptr;
    QToolButton *band5Button = nullptr;
    QToolButton *band6Button = nullptr;
    QToolButton *combinedBandButton = nullptr;
    QTimer *scanTimer = nullptr;
    QTimer *scanSpinnerTimer = nullptr;
    WiFiScanThread *scanThread = nullptr;
    QVector<WiFiNetwork> allNetworks;
    QVector<WiFiNetwork> visibleNetworks;
    BandSupport bandSupport;
    bool show24GHz = true;
    bool show5GHz = false;
    bool show6GHz = false;
    bool scanInProgress = false;
    bool refreshQueued = false;
    int scanSpinnerFrame = 0;
};

#endif // MAINWINDOW_H
