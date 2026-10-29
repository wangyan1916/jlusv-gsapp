#pragma once

#include <QObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkCookieJar>
#include <QTimer>
#include <QVector>
#include <QString>
#include <functional>

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

    // 船端任务与模式就绪状态
    bool taskReady = false;
    bool deviceOnline = false;
    int controlOwner = 0;      // 1=Main, 2=Ipc
    int controlState = 0;      // 1=Actuator, 2=HeadingSpeed, 3=Waypoints
    uint32_t controlRevision = 0;
    QString controlOwnerStr{"UNKNOWN"};
    QString controlStateStr{"UNKNOWN"};
    QString controlPhase{"UNKNOWN"};
    QString controlReason{"UNKNOWN"};
    QString loraState;
};

class GsClient : public QObject {
    Q_OBJECT
public:
    explicit GsClient(QObject *parent = nullptr);

    void setBaseUrl(const QString &url);
    QString baseUrl() const { return m_baseUrl; }

    void setPassword(const QString &pwd);
    QString password() const { return m_password; }
    bool isAuthenticated() const { return m_authenticated; }
    const UsvTelemetry& latestTelemetry() const { return m_latestTelemetry; }

    void startPolling(int intervalMs = 500);
    void stopPolling();

    // 网关安全认证
    void login(std::function<void(bool ok, const QString &err)> callback = nullptr);

    // 模式切换 (1=Main, 3=Waypoints)
    void switchControlMode(int owner, int state, std::function<void(bool ok, const QString &msg)> callback);

    // 任务发布指令
    void submitWaypoints(const QVector<Waypoint> &waypoints, int ttlSeconds = 300);
    void submitEmergencyStop();

signals:
    void telemetryUpdated(const UsvTelemetry &telemetry);
    void missionProgress(const QString &message);
    void missionResult(bool success, const QString &message);
    void connectionStatusChanged(bool connected);
    void authStatusChanged(bool authenticated, const QString &message);

private slots:
    void onPollTimer();
    void onOverviewReply(QNetworkReply *reply);

private:
    void doSubmitWaypoints(const QVector<Waypoint> &waypoints, int ttlSeconds, bool isRetry);
    void doSubmitEmergencyStop(bool isRetry);
    void trackOperation(const QString &operationId,
                        int timeoutMs,
                        std::function<void(const QJsonObject &obj, bool &done)> onUpdate,
                        std::function<void(const QString &err)> onTimeout);
    QString generateOperationId();

    QString m_baseUrl{"http://192.168.188.2"};
    QString m_password{"jlusv"};
    QString m_sessionCookie;
    bool m_authenticated = false;

    QNetworkAccessManager *m_nam;
    QTimer *m_pollTimer;
    UsvTelemetry m_latestTelemetry;
    bool m_connected = false;
};

} // namespace jlusv
