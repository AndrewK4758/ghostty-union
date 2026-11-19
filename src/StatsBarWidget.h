#pragma once

#include <QHash>
#include <QStringList>
#include <QVariant>
#include <QWidget>

#include <systemstats/SensorInfo.h>

class QLabel;
class QPaintEvent;
class QTimer;
class StatsCardWidget;

namespace KSysGuard::SystemStats {
class DBusInterface;
}

/**
 * Visual single-row telemetry bar (`40px`) mounted directly above the terminal tabs.
 *
 * Renders Union/Solar-Forge rounded telemetry cards with 1Hz scrolling sparklines,
 * progress gauges, thermal/load color thresholds, glowing VPN status badge, click-to-copy
 * IPs, and a 1-second local clock fed by KDE `org.kde.ksystemstats1` + QNetworkInterface.
 */
class StatsBarWidget final : public QWidget {
    Q_OBJECT

public:
    explicit StatsBarWidget(QWidget *parent = nullptr);
    ~StatsBarWidget() override;

    /// Subscribe or unsubscribe from ksystemstats D-Bus updates (e.g. when hidden/minimized).
    void setSensorsEnabled(bool enabled);

    /// Current primary display strings of the 5 telemetry cards (CPU, RAM, RTX, NET, VPN).
    QStringList summaryTexts() const;

    /// Current number of recorded 1Hz sparkline history samples.
    int historySampleCount() const;

protected:
    void paintEvent(QPaintEvent *event) override;

private Q_SLOTS:
    void onNewSensorData(const KSysGuard::SensorDataList &updates);
    void refreshDisplay();
    void refreshVpnAndClock();

private:
    void buildUi();
    void pushHistorySample();
    QString formatPercentValue(const QString &sensorId) const;
    QString formatTempValue(const QString &sensorId) const;
    QString formatRateCompact(const QString &sensorId) const;
    static QString queryIpv4ForInterface(const QString &ifName);

    KSysGuard::SystemStats::DBusInterface *m_dbus = nullptr;
    QStringList m_sensorIds;
    QHash<QString, QVariant> m_values;
    bool m_subscribed = false;

    StatsCardWidget *m_cpuCard = nullptr;
    StatsCardWidget *m_ramCard = nullptr;
    StatsCardWidget *m_gpuCard = nullptr;
    StatsCardWidget *m_netCard = nullptr;
    StatsCardWidget *m_vpnCard = nullptr;
    QWidget *m_clockPill = nullptr;
    QLabel *m_dateLabel = nullptr;
    QLabel *m_timeLabel = nullptr;
    QTimer *m_clockTimer = nullptr;

    QString m_lastWlanIp;
    QString m_lastVpnIp;
};
