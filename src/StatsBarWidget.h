#pragma once

#include <QFont>
#include <QHash>
#include <QList>
#include <QPointer>
#include <QStringList>
#include <QVariant>
#include <QWidget>

#include <systemstats/SensorInfo.h>

class ClockPillWidget;
class EvenDeltaRowLayout;
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
 * Renders full-width Union/Solar-Forge rounded telemetry cards across CPU, RAM,
 * 1..N dynamically enumerated GPU cards, NET, VPN, and ClockPill with equal `±Δpx` growth/shrinkage,
 * 1Hz scrolling sparklines, progress gauges, thermal/load color thresholds, private &
 * public IPv4 readout, glowing VPN status badge, click-to-copy IPs, and a 1-second
 * local clock fed by KDE `org.kde.ksystemstats1` + QNetworkInterface + async public IPv4 lookup.
 */
class StatsBarWidget final : public QWidget {
    Q_OBJECT

public:
    struct GpuDescriptor {
        QString prefix;
        QString name;
        QString tag;
        bool hasPower = true;
    };

    explicit StatsBarWidget(QWidget *parent = nullptr);
    ~StatsBarWidget() override;

    /// Subscribe or unsubscribe from ksystemstats D-Bus updates (e.g. when hidden/minimized).
    void setSensorsEnabled(bool enabled);

    /// Current primary display strings of the active telemetry cards (CPU, RAM, 1..N GPUs, NET, VPN).
    QStringList summaryTexts() const;

    /// Number of currently active GPU cards (`1..N`).
    int gpuCardCount() const { return static_cast<int>(m_gpuEntries.size()); }

    /// Short display tags of the currently active GPU cards in bar order.
    QStringList gpuTags() const;

    /// Override active GPU descriptors deterministically for testing.
    void setGpusForTesting(const QList<GpuDescriptor> &gpus);

    /// Current number of recorded 1Hz sparkline history samples.
    int historySampleCount() const;

    QString privateIp() const { return m_lastPrivateIp; }
    QString publicIp() const { return m_lastPublicIp; }
    void setPublicIpForTesting(const QString &ip);

    static bool isIntegratedGpuName(const QString &name);
    static QString formatGpuShortTag(const QString &name, int fallbackIndex = 0);
    static QStringList readConnectedNvidiaModelsFromProc(
        const QString &procRoot = QStringLiteral("/proc/driver/nvidia/gpus"));
    static QList<GpuDescriptor> selectActiveGpus(
        const QList<GpuDescriptor> &candidates,
        const QStringList &connectedNvidiaModels,
        bool enforceNvidiaProcFilter);

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
    struct GpuCardEntry {
        QString prefix;
        QString name;
        QString tag;
        QString usageId;
        QString tempId;
        QString powerId;
        QString nameId;
        bool hasPower = true;
        StatsCardWidget *card = nullptr;
    };

    void buildUi();
    void discoverGpusFromDbus();
    void applyGpuDescriptors(const QList<GpuDescriptor> &gpus);
    void syncSensorSubscriptions();
    void pushHistorySample();
    void requestPublicIpEndpoint(int endpointIndex);
    void schedulePublicIpRefresh();
    void updateNetToolTip();
    QString formatPercentValue(const QString &sensorId) const;
    QString formatTempValue(const QString &sensorId) const;
    QString formatRateCompact(const QString &sensorId) const;
    static QString queryIpv4ForInterface(const QString &ifName);

    KSysGuard::SystemStats::DBusInterface *m_dbus = nullptr;
    QStringList m_baseSensorIds;
    QStringList m_subscribedSensorIds;
    QHash<QString, QVariant> m_values;
    bool m_sensorsEnabled = false;
    bool m_testingGpusOverride = false;
    bool m_discoveryInFlight = false;
    bool m_discoveryPending = false;
    QStringList m_lastProcNvidiaModels;
    QList<GpuCardEntry> m_gpuEntries;

    EvenDeltaRowLayout *m_rowLayout = nullptr;
    QFont m_valueFont;
    QFont m_tagFont;
    StatsCardWidget *m_cpuCard = nullptr;
    StatsCardWidget *m_ramCard = nullptr;
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
