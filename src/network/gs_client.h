#pragma once

#include <QObject>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QTimer>
#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonObject>
#include <QVector>
#include <functional>

namespace jlusv {

struct Waypoint {
    double latDeg = 0.0;
    double lonDeg = 0.0;
    double speedMps = 1.2;
};

struct UsvTelemetry {
    QString uid, bootNonce;
    bool available = false, fresh = false, positionValid = false;
    double latDeg = 0.0, lonDeg = 0.0, headingDeg = 0.0;
    bool headingValid = false, cogValid = false, speedValid = false, batteryValid = false;
    double sogMps = 0.0, cogDeg = 0.0, batteryV = 0.0;
    int rtkState = 0, satellites = 0;
    QString positionSource, headingSource, headingReference, headingReason, diffAgeState;
    qint64 navAgeMs = -1, headingAgeMs = -1, diffAgeMs = -1, driftMs = -1;
    bool taskReady = false, deviceOnline = false, controlFresh = false;
    int controlOwner = 0, controlState = 0;
    quint32 controlRevision = 0, modeGsBoot = 0, modeCommandId = 0;
    QString controlOwnerStr{"UNKNOWN"}, controlStateStr{"UNKNOWN"};
    QString controlPhase{"UNKNOWN"}, controlReason{"UNKNOWN"}, loraState;
    quint32 taskGsBoot = 0, taskCommandId = 0;
    bool taskFresh = false, outputsOff = false;
    QString taskPhase{"UNKNOWN"}, taskReason{"UNKNOWN"};
    int waypointIndex = 0, waypointCount = 0;
};

class GsClient : public QObject {
    Q_OBJECT
public:
    explicit GsClient(QObject *parent = nullptr);
    void setBaseUrl(const QString &url);
    QString baseUrl() const { return m_baseUrl; }
    void setPassword(const QString &password);
    bool isAuthenticated() const { return m_authenticated; }
    bool isBusy() const { return m_busy; }
    bool canControl(bool requireMode = true) const;
    bool canPublish(int owner, int work) const;
    const UsvTelemetry &latestTelemetry() const { return m_latestTelemetry; }
    void setTargetUid(const QString &uid);
    void startPolling(int intervalMs = 500);
    void stopPolling();
    void login(std::function<void(bool, const QString &)> callback = nullptr);
    void switchControlMode(int owner, int work,
                           std::function<void(bool, const QString &)> callback = nullptr);
    void submitWaypoints(const QVector<Waypoint> &points, int ttlSeconds = 300, int owner = 1);
    void submitHeadingSpeed(int owner, double headingDeg, double speedMps);
    void submitHeadingOnly(int owner, double headingDeg, double throttlePercent);
    void submitEmergencyStop();
    void queryOperation();

signals:
    void telemetryUpdated(const UsvTelemetry &telemetry);
    void fleetUpdated(const QJsonArray &members);
    void operationUpdated(const QJsonObject &operation);
    void missionProgress(const QString &message);
    void missionResult(bool success, const QString &message);
    void connectionStatusChanged(bool connected);
    void authStatusChanged(bool authenticated, const QString &message);
    void busyChanged(bool busy);

private:
    struct Operation {
        QJsonObject request, receipt;
        QString gsUid, lastMessage, lastLogKey, queryError;
        quint32 commandId = 0, gsBoot = 0;
        qint64 startedAt = 0, lastFreshAt = 0;
        bool posted = false, notifiedUnknown = false, receivedExecution = false;
        std::function<void(bool, const QString &)> modeCallback;
    };
    void pollOverview();
    void updateTelemetry();
    void invalidateTelemetry();
    void resetContext(const QString &reason);
    void setBusy(bool busy);
    void setAuthenticated(bool authenticated, const QString &message);
    void clearOperation();
    void beginOperation(QJsonObject request, std::function<void(bool, const QString &)> callback = nullptr);
    bool acceptReceipt(const QJsonObject &receipt);
    void evaluateOperation();
    void finishOperation(bool success, const QString &message);
    void operationNotice(const QString &message);
    bool executionFresh() const;
    QJsonObject taskRequest(const QString &kind, int owner, int work);
    QNetworkRequest request(const QString &path, bool write = false) const;

    QString m_baseUrl{"http://192.168.188.2"}, m_password{"jlusv"};
    QString m_selectedUid, m_gsUid;
    QNetworkAccessManager m_nam;
    QTimer m_pollTimer, m_operationTimer;
    QElapsedTimer m_clock;
    QJsonArray m_members;
    UsvTelemetry m_latestTelemetry;
    Operation m_operation;
    quint64 m_context = 0, m_operationToken = 0, m_queryToken = 0;
    qint64 m_overviewAt = -10000;
    qint64 m_navigationAge = 5001;
    bool m_authenticated = false, m_connected = false, m_busy = false;
    bool m_overviewBusy = false, m_queryBusy = false;
};

QString executionText(const QString &state);
QString executionReasonText(const QString &reason);

} // namespace jlusv
