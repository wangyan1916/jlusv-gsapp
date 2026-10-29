#include "gs_client.h"
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonArray>
#include <QUrl>
#include <QUuid>
#include <QDebug>

namespace jlusv {

GsClient::GsClient(QObject *parent)
    : QObject(parent)
    , m_nam(new QNetworkAccessManager(this))
    , m_pollTimer(new QTimer(this))
{
    connect(m_pollTimer, &QTimer::timeout, this, &GsClient::onPollTimer);
}

void GsClient::setBaseUrl(const QString &url) {
    m_baseUrl = url.trimmed();
    if (m_baseUrl.endsWith('/')) {
        m_baseUrl.chop(1);
    }
}

void GsClient::startPolling(int intervalMs) {
    m_pollTimer->start(intervalMs);
    onPollTimer(); // 立即触发一次
}

void GsClient::stopPolling() {
    m_pollTimer->stop();
}

QString GsClient::generateOperationId() {
    return QUuid::createUuid().toString(QUuid::WithoutBraces).remove('-').toLower();
}

void GsClient::onPollTimer() {
    QUrl url(m_baseUrl + "/api/v1/fleet/overview");
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

    QNetworkReply *reply = m_nam->get(request);
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        onOverviewReply(reply);
    });
}

void GsClient::onOverviewReply(QNetworkReply *reply) {
    reply->deleteLater();
    if (reply->error() != QNetworkReply::NoError) {
        if (m_connected) {
            m_connected = false;
            emit connectionStatusChanged(false);
        }
        return;
    }

    if (!m_connected) {
        m_connected = true;
        emit connectionStatusChanged(true);
    }

    const QByteArray data = reply->readAll();
    QJsonParseError err;
    const QJsonDocument doc = QJsonDocument::fromJson(data, &err);
    if (err.error != QJsonParseError::NoError || !doc.isObject()) {
        return;
    }

    const QJsonObject root = doc.object();
    const QJsonArray members = root.value("members").toArray();
    if (members.isEmpty()) {
        return;
    }

    // 获取首台在线或活跃的船只数据
    const QJsonObject member = members.first().toObject();
    m_latestTelemetry.uid = member.value("uid").toString();
    m_latestTelemetry.bootNonce = member.value("task_rp_boot_nonce").toString();

    const QJsonObject nav = member.value("navigation").toObject();
    m_latestTelemetry.available = nav.value("available").toBool();
    m_latestTelemetry.fresh = nav.value("fresh").toBool();
    m_latestTelemetry.positionValid = nav.value("position_valid").toBool();
    m_latestTelemetry.latDeg = nav.value("lat_deg").toDouble();
    m_latestTelemetry.lonDeg = nav.value("lon_deg").toDouble();
    m_latestTelemetry.headingDeg = nav.value("heading_deg").toDouble();
    m_latestTelemetry.headingValid = nav.value("heading_valid").toBool();
    m_latestTelemetry.sogMps = nav.value("sog_mps").toDouble();
    m_latestTelemetry.cogDeg = nav.value("cog_deg").toDouble();
    m_latestTelemetry.rtkState = nav.value("rtk_state").toInt();
    m_latestTelemetry.satellites = nav.value("satellites").toInt();
    m_latestTelemetry.batteryV = nav.value("battery_v").toDouble();
    m_latestTelemetry.positionSource = nav.value("position_source").toString();

    emit telemetryUpdated(m_latestTelemetry);
}

void GsClient::submitWaypoints(const QVector<Waypoint> &waypoints, int ttlSeconds) {
    if (waypoints.isEmpty()) {
        emit missionResult(false, "航点列表为空");
        return;
    }
    if (waypoints.size() > 16) {
        emit missionResult(false, "单次任务最多支持 16 个航点");
        return;
    }
    if (m_latestTelemetry.uid.isEmpty() || m_latestTelemetry.bootNonce.isEmpty()) {
        emit missionResult(false, "尚未检测到在线船只或未获取到启动随机数 (boot_nonce)");
        return;
    }

    QJsonObject req;
    req.insert("operation_id", generateOperationId());
    req.insert("uid", m_latestTelemetry.uid);
    req.insert("expected_rp_boot_nonce", m_latestTelemetry.bootNonce);
    req.insert("kind", "waypoints");
    req.insert("ttl_s", ttlSeconds);

    QJsonArray pointsArray;
    for (const auto &wp : waypoints) {
        QJsonObject pt;
        pt.insert("lat_deg", wp.latDeg);
        pt.insert("lon_deg", wp.lonDeg);
        pt.insert("speed_mps", wp.speedMps);
        pointsArray.append(pt);
    }
    req.insert("points", pointsArray);

    QUrl url(m_baseUrl + "/api/v1/missions/submit");
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

    QNetworkReply *reply = m_nam->post(request, QJsonDocument(req).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        onSubmitReply(reply);
    });
}

void GsClient::submitEmergencyStop() {
    if (m_latestTelemetry.uid.isEmpty() || m_latestTelemetry.bootNonce.isEmpty()) {
        emit missionResult(false, "尚未检测到在线船只");
        return;
    }

    QJsonObject req;
    req.insert("operation_id", generateOperationId());
    req.insert("uid", m_latestTelemetry.uid);
    req.insert("expected_rp_boot_nonce", m_latestTelemetry.bootNonce);
    req.insert("kind", "stop");

    QUrl url(m_baseUrl + "/api/v1/missions/submit");
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

    QNetworkReply *reply = m_nam->post(request, QJsonDocument(req).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply]() {
        onSubmitReply(reply);
    });
}

void GsClient::onSubmitReply(QNetworkReply *reply) {
    reply->deleteLater();
    const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
    const QByteArray body = reply->readAll();

    if (code == 200 || code == 202) {
        emit missionResult(true, "任务发布成功已接收 (202 Accepted)");
    } else {
        emit missionResult(false, QString("任务发布被拒绝: HTTP %1 %2").arg(code).arg(QString::fromUtf8(body)));
    }
}

} // namespace jlusv
