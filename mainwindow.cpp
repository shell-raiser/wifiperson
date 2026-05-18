#include "mainwindow.h"
#include "wifi_scanner.h"

#include <QtCharts/QAreaSeries>
#include <QtCharts/QCategoryAxis>
#include <QtCharts/QChart>
#include <QtCharts/QChartView>
#include <QtCharts/QLineSeries>
#include <QtCharts/QValueAxis>

#include <QColor>
#include <QComboBox>
#include <QEvent>
#include <QHeaderView>
#include <QIcon>
#include <QHBoxLayout>
#include <QLabel>
#include <QMouseEvent>
#include <QPainter>
#include <QPen>
#include <QPixmap>
#include <QPushButton>
#include <QScrollBar>
#include <QSignalBlocker>
#include <QStackedWidget>
#include <QTableWidget>
#include <QTableWidgetItem>
#include <QToolButton>
#include <QToolTip>
#include <QVBoxLayout>
#include <QThread>
#include <QtMath>

#include <algorithm>
#include <utility>

namespace {

constexpr int DefaultScanIntervalMs = 30000;
constexpr int CombinedBandMinimumWidth = 980;

QColor blendWithWindow(const QColor &base, bool darkMode)
{
    return darkMode ? base.lighter(150) : base.darker(105);
}

QColor distinctColorForIndex(int index)
{
    // Spread hues around the wheel with a golden-angle step so nearby rows still look distinct.
    const int hue = (37 + index * 137) % 360;
    const int saturation = 185 + (index % 3) * 20;
    const int value = 210 + (index % 2) * 25;
    return QColor::fromHsv(hue, qMin(saturation, 255), qMin(value, 255));
}

class SortableTableWidgetItem : public QTableWidgetItem
{
public:
    using QTableWidgetItem::QTableWidgetItem;

    bool operator<(const QTableWidgetItem &other) const override
    {
        const QVariant leftValue = data(Qt::UserRole);
        const QVariant rightValue = other.data(Qt::UserRole);
        if (leftValue.isValid() && rightValue.isValid()) {
            return leftValue.toDouble() < rightValue.toDouble();
        }

        return QTableWidgetItem::operator<(other);
    }
};

QVector<int> standardChannelsForBand(WiFiBand band)
{
    QVector<int> channels;
    if (band == WiFiBand::Band24) {
        for (int channel = 1; channel <= 14; ++channel) {
            channels.append(channel);
        }
        return channels;
    }

    if (band == WiFiBand::Band5) {
        return {36, 40, 44, 48, 52, 56, 60, 64, 100, 104, 108, 112, 116, 120, 124, 128, 132, 136, 140, 144, 149, 153, 157, 161, 165, 169, 173};
    }

    for (int channel = 1; channel <= 233; channel += 4) {
        channels.append(channel);
    }
    return channels;
}

QVector<WiFiBand> activeBandsFromFlags(bool show24GHz, bool show5GHz, bool show6GHz)
{
    QVector<WiFiBand> bands;
    if (show24GHz) {
        bands.append(WiFiBand::Band24);
    }
    if (show5GHz) {
        bands.append(WiFiBand::Band5);
    }
    if (show6GHz) {
        bands.append(WiFiBand::Band6);
    }
    if (bands.isEmpty()) {
        bands.append(WiFiBand::Band24);
    }
    return bands;
}

qreal lowerChannelBoundary(int channel, WiFiBand band)
{
    return band == WiFiBand::Band24 ? channel - 0.5 : channel - 2.0;
}

qreal upperChannelBoundary(int channel, WiFiBand band)
{
    return band == WiFiBand::Band24 ? channel + 0.5 : channel + 2.0;
}

void clearCategoryAxis(QCategoryAxis *axis)
{
    const QStringList labels = axis->categoriesLabels();
    for (const QString &label : labels) {
        axis->remove(label);
    }
}

} // namespace

MainWindow::MainWindow(QWidget *parent)
    : QMainWindow(parent)
{
    setupUi();
    updateBandButtonVisibility();
    syncBandButtons();
    updateChartTheme();

    scanTimer = new QTimer(this);
    connect(scanTimer, &QTimer::timeout, this, &MainWindow::refreshScan);
    scanTimer->start(DefaultScanIntervalMs);

    scanSpinnerTimer = new QTimer(this);
    connect(scanSpinnerTimer, &QTimer::timeout, this, &MainWindow::updateScanSpinner);

    refreshScan();
}

MainWindow::~MainWindow()
{
    if (scanThread != nullptr) {
        scanThread->wait(2000);
    }
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event)
{
    if (watched == chartView->viewport()) {
        if (event->type() == QEvent::MouseMove) {
            const auto *mouseEvent = static_cast<QMouseEvent *>(event);
            updateHoverTooltip(mouseEvent->pos());
        } else if (event->type() == QEvent::Leave) {
            QToolTip::hideText();
        }
    }

    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::changeEvent(QEvent *event)
{
    if (event->type() == QEvent::PaletteChange) {
        updateChartTheme();
    }

    QMainWindow::changeEvent(event);
}

void MainWindow::resizeEvent(QResizeEvent *event)
{
    QMainWindow::resizeEvent(event);
    updateCombinedBandAvailability();
}

void MainWindow::setupUi()
{
    setWindowTitle("WiFiperson");
    resize(1180, 760);

    pageStack = new QStackedWidget(this);
    listPage = new QWidget(this);
    detailPage = new QWidget(this);

    chart = new QChart();
    chart->legend()->hide();
    chart->setAnimationOptions(QChart::SeriesAnimations);

    axisX = new QCategoryAxis();
    axisX->setTitleText("Wi-Fi Channel");
    axisX->setLabelsPosition(QCategoryAxis::AxisLabelsPositionCenter);

    axisY = new QValueAxis();
    axisY->setTitleText("Signal Strength (dBm)");
    axisY->setLabelFormat("%d");
    axisY->setRange(-100, -20);
    axisY->setTickCount(9);

    chart->addAxis(axisX, Qt::AlignBottom);
    chart->addAxis(axisY, Qt::AlignLeft);

    chartView = new QChartView(chart);
    chartView->setRenderHint(QPainter::Antialiasing);
    chartView->setMouseTracking(true);
    chartView->viewport()->setMouseTracking(true);
    chartView->viewport()->installEventFilter(this);

    invalidStateLabel = new QLabel(this);
    invalidStateLabel->setAlignment(Qt::AlignCenter);
    invalidStateLabel->setWordWrap(true);
    invalidStateLabel->hide();

    statusLabel = new QLabel(this);
    statusLabel->setWordWrap(true);
    statusLabel->hide();

    refreshButton = new QPushButton("Refresh Scan", this);
    refreshButton->setMinimumHeight(44);
    connect(refreshButton, &QPushButton::clicked, this, &MainWindow::refreshScan);

    band24Button = new QToolButton(this);
    band24Button->setMinimumSize(88, 44);
    band24Button->setText("2.4 GHz");
    band24Button->setCheckable(true);
    connect(band24Button, &QToolButton::toggled, this, [this](bool checked) {
        if (!checked) {
            if (show24GHz && !show5GHz && !show6GHz) {
                const QSignalBlocker blocker(band24Button);
                band24Button->setChecked(true);
            }
            return;
        }

        show24GHz = checked;
        show5GHz = false;
        show6GHz = false;
        syncBandButtons();
        applyFilters();
    });

    band5Button = new QToolButton(this);
    band5Button->setMinimumSize(88, 44);
    band5Button->setText("5 GHz");
    band5Button->setCheckable(true);
    connect(band5Button, &QToolButton::toggled, this, [this](bool checked) {
        if (!checked) {
            if (!show24GHz && show5GHz && !show6GHz) {
                const QSignalBlocker blocker(band5Button);
                band5Button->setChecked(true);
            }
            return;
        }

        show5GHz = checked;
        show24GHz = false;
        show6GHz = false;
        syncBandButtons();
        applyFilters();
    });

    band6Button = new QToolButton(this);
    band6Button->setMinimumSize(88, 44);
    band6Button->setText("6 GHz");
    band6Button->setCheckable(true);
    connect(band6Button, &QToolButton::toggled, this, [this](bool checked) {
        if (!checked) {
            if (!show24GHz && !show5GHz && show6GHz) {
                const QSignalBlocker blocker(band6Button);
                band6Button->setChecked(true);
            }
            return;
        }

        show6GHz = checked;
        show24GHz = false;
        show5GHz = false;
        syncBandButtons();
        applyFilters();
    });

    combinedBandButton = new QToolButton(this);
    combinedBandButton->setMinimumSize(112, 44);
    combinedBandButton->setText("2.4 + 5 GHz");
    combinedBandButton->setCheckable(true);
    connect(combinedBandButton, &QToolButton::toggled, this, [this](bool checked) {
        if (!checked) {
            if (show24GHz && show5GHz && !show6GHz) {
                const QSignalBlocker blocker(combinedBandButton);
                combinedBandButton->setChecked(true);
            }
            return;
        }

        show24GHz = true;
        show5GHz = true;
        show6GHz = false;
        syncBandButtons();
        applyFilters();
    });

    scanIntervalCombo = new QComboBox(this);
    scanIntervalCombo->setMinimumHeight(44);
    scanIntervalCombo->addItem("Poll: 30s", DefaultScanIntervalMs);
    scanIntervalCombo->addItem("Poll: 60s", 60000);
    scanIntervalCombo->addItem("Poll: 5 min", 300000);
    scanIntervalCombo->addItem("Poll: Off", 0);
    connect(scanIntervalCombo, qOverload<int>(&QComboBox::currentIndexChanged), this, &MainWindow::updateScanInterval);

    auto *controlsLayout = new QHBoxLayout();
    controlsLayout->addWidget(refreshButton);
    controlsLayout->addWidget(band24Button);
    controlsLayout->addWidget(band5Button);
    controlsLayout->addWidget(combinedBandButton);
    controlsLayout->addWidget(band6Button);
    controlsLayout->addWidget(scanIntervalCombo);
    controlsLayout->addStretch(1);

    networkTable = new QTableWidget(this);
    networkTable->setColumnCount(7);
    networkTable->setHorizontalHeaderLabels(
        {"Color", "SSID", "BSSID", "Band", "Channel", "Signal (dBm)", "Frequency (MHz)"});
    networkTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    networkTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    networkTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    networkTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    networkTable->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    networkTable->horizontalHeader()->setSectionResizeMode(5, QHeaderView::ResizeToContents);
    networkTable->horizontalHeader()->setSectionResizeMode(6, QHeaderView::ResizeToContents);
    networkTable->verticalHeader()->setVisible(false);
    networkTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    networkTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    networkTable->setAlternatingRowColors(true);
    networkTable->setSortingEnabled(true);
    networkTable->horizontalHeader()->setSortIndicatorShown(true);
    networkTable->verticalHeader()->setDefaultSectionSize(44);
    networkTable->setIconSize(QSize(36, 36));
    networkTable->setTextElideMode(Qt::ElideRight);
    networkTable->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    networkTable->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);
    connect(networkTable, &QTableWidget::cellClicked, this, &MainWindow::showNetworkDetails);

    auto *layout = new QVBoxLayout();
    layout->addWidget(chartView, 3);
    layout->addWidget(invalidStateLabel, 3);
    layout->addLayout(controlsLayout);
    layout->addWidget(statusLabel);
    layout->addWidget(networkTable, 2);

    listPage->setLayout(layout);

    backButton = new QPushButton("Back to networks", this);
    backButton->setMinimumHeight(44);
    connect(backButton, &QPushButton::clicked, this, &MainWindow::showNetworkList);

    detailTable = new QTableWidget(this);
    detailTable->setColumnCount(2);
    detailTable->setHorizontalHeaderLabels({"Property", "Value"});
    detailTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    detailTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    detailTable->verticalHeader()->setVisible(false);
    detailTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    detailTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    detailTable->setAlternatingRowColors(true);
    detailTable->verticalHeader()->setDefaultSectionSize(44);
    detailTable->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    detailTable->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);

    interferenceLabel = new QLabel("Interfering SSIDs", this);
    interferenceLabel->setWordWrap(true);

    interferenceTable = new QTableWidget(this);
    interferenceTable->setColumnCount(5);
    interferenceTable->setHorizontalHeaderLabels({"SSID", "BSSID", "Band", "Channel", "Signal (dBm)"});
    interferenceTable->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
    interferenceTable->horizontalHeader()->setSectionResizeMode(1, QHeaderView::Stretch);
    interferenceTable->horizontalHeader()->setSectionResizeMode(2, QHeaderView::ResizeToContents);
    interferenceTable->horizontalHeader()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    interferenceTable->horizontalHeader()->setSectionResizeMode(4, QHeaderView::ResizeToContents);
    interferenceTable->verticalHeader()->setVisible(false);
    interferenceTable->setEditTriggers(QAbstractItemView::NoEditTriggers);
    interferenceTable->setSelectionBehavior(QAbstractItemView::SelectRows);
    interferenceTable->setAlternatingRowColors(true);
    interferenceTable->verticalHeader()->setDefaultSectionSize(44);
    interferenceTable->setHorizontalScrollMode(QAbstractItemView::ScrollPerPixel);
    interferenceTable->setVerticalScrollMode(QAbstractItemView::ScrollPerPixel);

    auto *detailLayout = new QVBoxLayout();
    detailLayout->addWidget(backButton);
    detailLayout->addWidget(detailTable, 1);
    detailLayout->addWidget(interferenceLabel);
    detailLayout->addWidget(interferenceTable, 1);
    detailPage->setLayout(detailLayout);

    pageStack->addWidget(listPage);
    pageStack->addWidget(detailPage);
    pageStack->setCurrentWidget(listPage);

    setCentralWidget(pageStack);

    setStyleSheet(QStringLiteral(
        "QPushButton, QToolButton { padding: 10px 16px; font-size: 15px; }"
        "QTableView { font-size: 15px; }"
        "QHeaderView::section { padding: 8px; font-size: 14px; }"));
}

void MainWindow::refreshScan()
{
    if (scanInProgress) {
        refreshQueued = true;
        return;
    }

    scanInProgress = true;
    refreshQueued = false;
    setRefreshButtonLoading(true);
    setStatusMessage(QString());

    scanThread = new WiFiScanThread(this);
    connect(scanThread, &WiFiScanThread::scanFinished, this, &MainWindow::handleScanFinished);
    connect(scanThread, &WiFiScanThread::scanError, this, &MainWindow::handleScanError);
    connect(scanThread, &QThread::finished, scanThread, &QObject::deleteLater);
    connect(scanThread, &QThread::finished, this, [this]() { scanThread = nullptr; });
    scanThread->start();
}

void MainWindow::handleScanFinished(const QVector<WiFiNetwork> &networks, const BandSupport &support)
{
    scanInProgress = false;
    restoreRefreshButton();
    bandSupport = support;

    finishScanWithNetworks(networks);
    if (networks.isEmpty()) {
        setStatusMessage(
            "Scan completed, but no SSIDs were found. The channel axis is still shown for the selected band.");
    }

    if (scanThread != nullptr) {
        scanThread->quit();
    }

    if (refreshQueued) {
        refreshQueued = false;
        refreshScan();
    }
}

void MainWindow::handleScanError(const QString &message, const BandSupport &support)
{
    scanInProgress = false;
    restoreRefreshButton();
    bandSupport = support;
    updateBandButtonVisibility();
    syncBandButtons();

    if (message.contains("Turn on Wi-Fi", Qt::CaseInsensitive)) {
        showInvalidState(message);
    } else if (!allNetworks.isEmpty()) {
        invalidStateLabel->hide();
        chartView->show();
        networkTable->show();
        setStatusMessage(QString());
    } else {
        showInvalidState(QString("Wi-Fi scanning failed.\n\n%1").arg(message));
    }

    if (scanThread != nullptr) {
        scanThread->quit();
    }

    if (refreshQueued) {
        refreshQueued = false;
        refreshScan();
    }
}

QVector<WiFiNetwork> MainWindow::filteredNetworks() const
{
    QVector<WiFiNetwork> filtered;
    for (const WiFiNetwork &network : allNetworks) {
        if (network.band == WiFiBand::Band24 && show24GHz) {
            filtered.append(network);
        } else if (network.band == WiFiBand::Band5 && show5GHz) {
            filtered.append(network);
        } else if (network.band == WiFiBand::Band6 && show6GHz) {
            filtered.append(network);
        }
    }

    return filtered;
}

QVector<QPointF> MainWindow::buildSpectrumEnvelope(const WiFiNetwork &network) const
{
    const qreal centerChannel = network.channel;
    const qreal halfWidth = 2.0;
    const qreal baseLevel = -100.0;
    const qreal shoulderLevel = qMax(baseLevel, network.signalDbm - 12.0);

    return {
        QPointF(centerChannel - halfWidth, baseLevel),
        QPointF(centerChannel - halfWidth * 0.5, shoulderLevel),
        QPointF(centerChannel, network.signalDbm),
        QPointF(centerChannel + halfWidth * 0.5, shoulderLevel),
        QPointF(centerChannel + halfWidth, baseLevel)
    };
}

QVector<WiFiNetwork> MainWindow::interferingNetworks(const WiFiNetwork &selected) const
{
    QVector<WiFiNetwork> interferers;
    const qreal selectedLower = lowerChannelBoundary(selected.channel, selected.band);
    const qreal selectedUpper = upperChannelBoundary(selected.channel, selected.band);

    for (const WiFiNetwork &candidate : allNetworks) {
        if (candidate.bssid == selected.bssid || candidate.band != selected.band) {
            continue;
        }

        const qreal candidateLower = lowerChannelBoundary(candidate.channel, candidate.band);
        const qreal candidateUpper = upperChannelBoundary(candidate.channel, candidate.band);
        if (candidateLower <= selectedUpper && candidateUpper >= selectedLower) {
            interferers.append(candidate);
        }
    }

    std::sort(interferers.begin(), interferers.end(), [](const WiFiNetwork &left, const WiFiNetwork &right) {
        if (left.signalDbm == right.signalDbm) {
            return left.channel < right.channel;
        }
        return left.signalDbm > right.signalDbm;
    });

    return interferers;
}

QString MainWindow::bandLabel(WiFiBand band) const
{
    switch (band) {
    case WiFiBand::Band24:
        return "2.4 GHz";
    case WiFiBand::Band5:
        return "5 GHz";
    case WiFiBand::Band6:
        return "6 GHz";
    }

    return "Unknown";
}

QString MainWindow::channelLabel(const WiFiNetwork &network) const
{
    return QString::number(network.channel);
}

QColor MainWindow::colorForBand(WiFiBand band, int index) const
{
    Q_UNUSED(band);
    return distinctColorForIndex(index);
}

qreal MainWindow::horizontalDistanceToSegment(const QPointF &point, const QPointF &a, const QPointF &b) const
{
    const QPointF ab = b - a;
    const qreal abSquared = QPointF::dotProduct(ab, ab);
    if (qFuzzyIsNull(abSquared)) {
        return QLineF(point, a).length();
    }

    const qreal t = qBound<qreal>(0.0, QPointF::dotProduct(point - a, ab) / abSquared, 1.0);
    const QPointF projection = a + ab * t;
    return QLineF(point, projection).length();
}

void MainWindow::applyFilters()
{
    visibleNetworks = filteredNetworks();
    updateChart(visibleNetworks);
    updateTable(visibleNetworks);
    setStatusMessage(QString());
}

void MainWindow::finishScanWithNetworks(const QVector<WiFiNetwork> &networks)
{
    allNetworks = networks;
    invalidStateLabel->hide();
    chartView->show();
    networkTable->show();

    for (const WiFiNetwork &network : allNetworks) {
        if (network.band == WiFiBand::Band5) {
            bandSupport.has5GHz = true;
        } else if (network.band == WiFiBand::Band6) {
            bandSupport.has6GHz = true;
        }
    }

    updateBandButtonVisibility();
    if (!band24Button->isVisible() && !band5Button->isVisible() && !band6Button->isVisible()) {
        band24Button->setVisible(true);
        bandSupport.has24GHz = true;
    }

    if (!show24GHz && !show5GHz && !show6GHz) {
        show24GHz = bandSupport.has24GHz;
        show5GHz = false;
        show6GHz = false;
    }

    syncBandButtons();
    applyFilters();

    setStatusMessage(QString());
}

void MainWindow::showInvalidState(const QString &message)
{
    allNetworks.clear();
    visibleNetworks.clear();
    chartView->hide();
    networkTable->hide();
    band24Button->hide();
    band5Button->hide();
    band6Button->hide();
    combinedBandButton->hide();
    invalidStateLabel->setText(message);
    invalidStateLabel->show();
    updateChart({});
    updateTable({});
    setStatusMessage(QString());
}

void MainWindow::restoreRefreshButton()
{
    if (scanSpinnerTimer != nullptr) {
        scanSpinnerTimer->stop();
    }
    refreshButton->setEnabled(true);
    refreshButton->setText("Refresh Scan");
    refreshButton->setIcon(QIcon());
}

void MainWindow::setRefreshButtonLoading(bool loading)
{
    refreshButton->setEnabled(!loading);
    if (!loading) {
        restoreRefreshButton();
        return;
    }

    scanSpinnerFrame = 0;
    refreshButton->setText("Scanning");
    updateScanSpinner();
    scanSpinnerTimer->start(120);
}

void MainWindow::updateScanSpinner()
{
    QPixmap pixmap(24, 24);
    pixmap.fill(Qt::transparent);

    QPainter painter(&pixmap);
    painter.setRenderHint(QPainter::Antialiasing);
    QPen pen(palette().color(QPalette::ButtonText));
    pen.setWidth(3);
    pen.setCapStyle(Qt::RoundCap);
    painter.setPen(pen);
    painter.drawArc(QRect(4, 4, 16, 16), (scanSpinnerFrame * 30) * 16, 270 * 16);

    refreshButton->setIcon(QIcon(pixmap));
    scanSpinnerFrame = (scanSpinnerFrame + 1) % 12;
}

void MainWindow::syncBandButtons()
{
    const QSignalBlocker block24(band24Button);
    const QSignalBlocker block5(band5Button);
    const QSignalBlocker block6(band6Button);
    const QSignalBlocker blockCombined(combinedBandButton);
    band24Button->setChecked(show24GHz && !show5GHz);
    band5Button->setChecked(show5GHz && !show24GHz);
    band6Button->setChecked(show6GHz);
    combinedBandButton->setChecked(show24GHz && show5GHz && !show6GHz);
}

void MainWindow::updateBandButtonVisibility()
{
    band24Button->setVisible(bandSupport.has24GHz);
    band5Button->setVisible(bandSupport.has5GHz);
    band6Button->setVisible(bandSupport.has6GHz);
    updateCombinedBandAvailability();
}

void MainWindow::updateCombinedBandAvailability()
{
    if (combinedBandButton == nullptr) {
        return;
    }

    const bool combinedAllowed = width() >= CombinedBandMinimumWidth && bandSupport.has24GHz && bandSupport.has5GHz;
    combinedBandButton->setVisible(combinedAllowed);
    if (!combinedAllowed && show24GHz && show5GHz) {
        show5GHz = false;
        show6GHz = false;
        syncBandButtons();
        applyFilters();
    }
}

void MainWindow::updateChart(const QVector<WiFiNetwork> &networks)
{
    while (!chart->series().isEmpty()) {
        QAbstractSeries *series = chart->series().constFirst();
        chart->removeSeries(series);
        delete series;
    }

    const qreal baseLevel = -100.0;
    int colorIndex = 0;
    int minSignal = 0;
    int maxSignal = 0;
    bool haveSignals = false;

    for (const WiFiNetwork &network : networks) {
        const QColor color = colorForBand(network.band, colorIndex++);
        const QVector<QPointF> envelope = buildSpectrumEnvelope(network);
        if (!haveSignals) {
            minSignal = network.signalDbm;
            maxSignal = network.signalDbm;
            haveSignals = true;
        } else {
            minSignal = qMin(minSignal, network.signalDbm);
            maxSignal = qMax(maxSignal, network.signalDbm);
        }

        auto *upperSeries = new QLineSeries();
        upperSeries->append(envelope);
        QPen pen(color);
        pen.setWidth(3);
        upperSeries->setPen(pen);

        auto *lowerSeries = new QLineSeries();
        lowerSeries->append(QPointF(envelope.first().x(), baseLevel));
        lowerSeries->append(QPointF(envelope.last().x(), baseLevel));

        auto *areaSeries = new QAreaSeries(upperSeries, lowerSeries);
        areaSeries->setPen(pen);
        QColor fill = color;
        fill.setAlpha(55);
        areaSeries->setBrush(fill);

        chart->addSeries(areaSeries);
        areaSeries->attachAxis(axisX);
        areaSeries->attachAxis(axisY);
    }

    const QVector<WiFiBand> activeBands = activeBandsFromFlags(show24GHz, show5GHz, show6GHz);

    clearCategoryAxis(axisX);
    bool axisStarted = false;
    qreal axisStart = 0.5;
    qreal axisEnd = 14.5;

    for (WiFiBand activeBand : activeBands) {
        const QVector<int> standardChannels = standardChannelsForBand(activeBand);
        for (int channel : standardChannels) {
            const qreal lowerBoundary = lowerChannelBoundary(channel, activeBand);
            const qreal upperBoundary = upperChannelBoundary(channel, activeBand);
            if (!axisStarted) {
                axisX->setStartValue(lowerBoundary);
                axisStart = lowerBoundary;
                axisStarted = true;
            }
            axisEnd = upperBoundary;
            axisX->append(QString::number(channel), upperBoundary);
        }
    }

    axisX->setRange(axisStart, axisEnd);

    if (!haveSignals) {
        axisY->setRange(-100, -20);
    } else {
        const int paddedMin = qMax(-100, static_cast<int>(qFloor((minSignal - 10) / 5.0) * 5.0));
        const int paddedMax = qMin(-20, static_cast<int>(qCeil((maxSignal + 10) / 5.0) * 5.0));
        axisY->setRange(paddedMin, qMax(paddedMax, paddedMin + 20));
    }
    chart->setTitle(QStringLiteral("Wi-Fi Spectrum Utilization (%1 networks)").arg(networks.size()));
}

void MainWindow::updateChartTheme()
{
    const QPalette palette = this->palette();
    const bool darkMode = palette.color(QPalette::Window).lightness() < 128;
    const QColor windowColor = palette.color(QPalette::Window);
    const QColor panelColor = blendWithWindow(windowColor, darkMode);
    const QColor textColor = palette.color(QPalette::WindowText);
    const QColor gridColor = darkMode ? QColor(255, 255, 255, 55) : QColor(0, 0, 0, 45);

    chart->setBackgroundBrush(panelColor);
    chart->setPlotAreaBackgroundVisible(true);
    chart->setPlotAreaBackgroundBrush(darkMode ? QColor(18, 24, 33) : QColor(248, 250, 252));
    chart->setTitleBrush(textColor);

    axisX->setLabelsBrush(textColor);
    axisX->setTitleBrush(textColor);
    axisX->setGridLineColor(gridColor);

    axisY->setLabelsBrush(textColor);
    axisY->setTitleBrush(textColor);
    axisY->setGridLineColor(gridColor);
}

void MainWindow::updateDetailTable(const WiFiNetwork &network)
{
    const QVector<QPair<QString, QString>> rows = {
        {QStringLiteral("SSID"), network.ssid},
        {QStringLiteral("BSSID / access point MAC address"), network.bssid},
        {QStringLiteral("Band"), bandLabel(network.band)},
        {QStringLiteral("Channel"), channelLabel(network)},
        {QStringLiteral("Frequency"), QStringLiteral("%1 MHz").arg(network.frequencyMHz)},
        {QStringLiteral("Signal strength"), QStringLiteral("%1 dBm").arg(network.signalDbm)},
    };

    detailTable->setRowCount(rows.size());
    for (int row = 0; row < rows.size(); ++row) {
        detailTable->setItem(row, 0, new QTableWidgetItem(rows.at(row).first));
        detailTable->setItem(row, 1, new QTableWidgetItem(rows.at(row).second));
    }

    const QVector<WiFiNetwork> interferers = interferingNetworks(network);
    interferenceLabel->setText(
        interferers.isEmpty()
            ? QStringLiteral("No overlapping SSIDs were found for this channel.")
            : QStringLiteral("Interfering SSIDs (%1)").arg(interferers.size()));
    interferenceTable->setRowCount(interferers.size());
    for (int row = 0; row < interferers.size(); ++row) {
        const WiFiNetwork &interferer = interferers.at(row);
        interferenceTable->setItem(row, 0, new QTableWidgetItem(interferer.ssid));
        interferenceTable->setItem(row, 1, new QTableWidgetItem(interferer.bssid));
        interferenceTable->setItem(row, 2, new QTableWidgetItem(bandLabel(interferer.band)));

        auto *channelItem = new SortableTableWidgetItem(channelLabel(interferer));
        channelItem->setData(Qt::UserRole, interferer.channel);
        interferenceTable->setItem(row, 3, channelItem);

        auto *signalItem = new SortableTableWidgetItem(QString::number(interferer.signalDbm));
        signalItem->setData(Qt::UserRole, interferer.signalDbm);
        interferenceTable->setItem(row, 4, signalItem);
    }
}

void MainWindow::showNetworkDetails(int row, int column)
{
    Q_UNUSED(column);
    QTableWidgetItem *bssidItem = networkTable->item(row, 2);
    if (bssidItem == nullptr) {
        return;
    }

    const QString bssid = bssidItem->text();
    for (const WiFiNetwork &network : std::as_const(visibleNetworks)) {
        if (network.bssid == bssid) {
            updateDetailTable(network);
            pageStack->setCurrentWidget(detailPage);
            return;
        }
    }
}

void MainWindow::showNetworkList()
{
    pageStack->setCurrentWidget(listPage);
}

void MainWindow::updateHoverTooltip(const QPoint &mousePos)
{
    if (visibleNetworks.isEmpty()) {
        QToolTip::hideText();
        return;
    }

    const QRectF plotArea = chart->plotArea();
    if (!plotArea.contains(mousePos)) {
        QToolTip::hideText();
        return;
    }

    const QPointF chartValue = chart->mapToValue(mousePos);
    qreal bestDistance = 1e9;
    QString bestLabel;

    for (const WiFiNetwork &network : visibleNetworks) {
        const QVector<QPointF> envelope = buildSpectrumEnvelope(network);
        for (int index = 0; index + 1 < envelope.size(); ++index) {
            const qreal distance = horizontalDistanceToSegment(chartValue, envelope.at(index), envelope.at(index + 1));
            if (distance < bestDistance) {
                bestDistance = distance;
                bestLabel = QStringLiteral("%1\n%2").arg(network.ssid, network.bssid);
            }
        }
    }

    if (bestDistance < 1.2) {
        QToolTip::showText(chartView->viewport()->mapToGlobal(mousePos + QPoint(16, 16)), bestLabel, chartView);
    } else {
        QToolTip::hideText();
    }
}

void MainWindow::updateTable(const QVector<WiFiNetwork> &networks)
{
    const bool sortingEnabled = networkTable->isSortingEnabled();
    networkTable->setSortingEnabled(false);
    networkTable->setRowCount(networks.size());

    int colorIndex = 0;

    for (int row = 0; row < networks.size(); ++row) {
        const WiFiNetwork &network = networks.at(row);
        const QColor color = colorForBand(network.band, colorIndex++);
        auto *colorItem = new QTableWidgetItem();
        colorItem->setBackground(color);
        networkTable->setItem(row, 0, colorItem);
        networkTable->setItem(row, 1, new QTableWidgetItem(network.ssid));
        networkTable->setItem(row, 2, new QTableWidgetItem(network.bssid));
        networkTable->setItem(row, 3, new QTableWidgetItem(bandLabel(network.band)));

        auto *channelItem = new SortableTableWidgetItem(channelLabel(network));
        channelItem->setData(Qt::UserRole, network.channel);
        networkTable->setItem(row, 4, channelItem);

        auto *signalItem = new SortableTableWidgetItem(QString::number(network.signalDbm));
        signalItem->setData(Qt::UserRole, network.signalDbm);
        networkTable->setItem(row, 5, signalItem);

        auto *frequencyItem = new SortableTableWidgetItem(QString::number(network.frequencyMHz));
        frequencyItem->setData(Qt::UserRole, network.frequencyMHz);
        networkTable->setItem(row, 6, frequencyItem);
    }

    networkTable->setSortingEnabled(sortingEnabled);
}

void MainWindow::updateScanInterval(int index)
{
    if (scanTimer == nullptr) {
        return;
    }

    const int intervalMs = scanIntervalCombo->itemData(index).toInt();
    if (intervalMs <= 0) {
        scanTimer->stop();
        return;
    }

    scanTimer->start(intervalMs);
}

void MainWindow::setStatusMessage(const QString &message) const
{
    statusLabel->setText(message);
    statusLabel->setVisible(!message.isEmpty());
}
