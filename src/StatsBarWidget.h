#pragma once

#include <QHash>
#include <QPointer>
#include <QStringList>
#include <QVariant>
#include <QWidget>

#include <systemstats/SensorInfo.h>

class ClockPillWidget;
class QNetworkAccessManager;
class QNetworkReply;
class QPaintEvent;
class QTimer;
class StatsCardWidget;

namespace KSysGuard::SystemStats {
class DBusInterface;
}

/**
 * Visual single-row telemetry bar (`40px`) mounted directly above the terminal tabs.
 *
 * Renders full-width Union/Solar-Forge rounded telemetry cards across all 6 bar items
 * (`CPU`, `RAM`, `RTX`, `NET`, `VPN`, `ClockPill`) with equal `±Δpx` growth/shrinkage,
 * 1Hz scrolling sparklines, progress gauges, thermal/load color thresholds, private &
 * public IPv4 readout, glowing VPN status badge, click-to-copy IPs, and a 1-second
 * local clock fed by KDE `org.kde.ksystemstats1` + QNetworkInterface + async public IPv4 lookup.
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

    QString privateIp() const { return m_lastPrivateIp; }
    QString publicIp() const { return m_lastPublicIp; }
    void setPublicIpForTesting(const QString &ip);

    static QString parsePublicIpv4Response(const QByteArray &body);
    static QString queryPrimaryPrivateIpv4();
    static bool isVirtualOrTunnelInterface(const QString &ifName);

protected:
    void paintEvent(QPaintEvent *event) override;

private Q_SLOTS:
    void onNewSensorData(const KSysGuard::SensorDataList &updates);
    void refreshDisplay();
    void refreshVpnAndClock();
    void refreshPublicIp();

private:
    void buildUi();
    void pushHistorySample();
    void requestPublicIpEndpoint(int endpointIndex);
    void schedulePublicIpRefresh();
    void updateNetToolTip();
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
    ClockPillWidget *m_clockPill = nullptr;
    QTimer *m_clockTimer = nullptr;

    QNetworkAccessManager *m_netAccess = nullptr;
    QPointer<QNetworkReply> m_publicIpReply;
    QTimer *m_publicIpTimer = nullptr;
    QTimer *m_publicIpDebounceTimer = nullptr;

    QString m_lastPrivateIp;
    QString m_lastPublicIp;
    QString m_lastVpnIp;
    QString m_prevPublicIpTriggerPrivate;
    QString m_prevPublicIpTriggerVpn;
};
