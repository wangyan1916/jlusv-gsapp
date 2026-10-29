#pragma once

#include <QObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QTimer>
#include <QVector>
#include <QString>

namespace jlusv {

struct Waypoint {
    double latDeg = 0.0;
    double lonDeg = 0.0;
    double speedMps = 1.2;
};

struct UsvTelemetry {
    QString uid;
    QString bootNonce;
    bool available = false;
    bool fresh = false;
    bool positionValid = false;
    double latDeg = 0.0;
    double lonDeg = 0.0;
    double headingDeg = 0.0;
    bool headingValid = false;
    double sogMps = 0.0;
    double cogDeg = 0.0;
    int rtkState = 0; // 0=none, 1=single, 4=fixed, 5=float
    int satellites = 0;
    double batteryV = 0.0;
    QString positionSource;
};

class GsClient : public QObject {
    Q_OBJECT
public:
    explicit GsClient(QObject *parent = nullptr);

    void setBaseUrl(const QString &url);
    QString baseUrl() const { return m_baseUrl; }

    void startPolling(int intervalMs = 500);
    void stopPolling();

    // 任务发布指令
    void submitWaypoints(const QVector<Waypoint> &waypoints, int ttlSeconds = 300);
    void submitEmergencyStop();

signals:
    void telemetryUpdated(const UsvTelemetry &telemetry);
    void missionResult(bool success, const QString &message);
    void connectionStatusChanged(bool connected);

private slots:
    void onPollTimer();
    void onOverviewReply(QNetworkReply *reply);
    void onSubmitReply(QNetworkReply *reply);

private:
    QString m_baseUrl{"http://192.168.188.2"};
    QNetworkAccessManager *m_nam;
    QTimer *m_pollTimer;
    UsvTelemetry m_latestTelemetry;
    bool m_connected = false;

    QString generateOperationId();
};

} // namespace jlusv
