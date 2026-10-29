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
    m_nam->setCookieJar(new QNetworkCookieJar(this));
    connect(m_pollTimer, &QTimer::timeout, this, &GsClient::onPollTimer);
}

void GsClient::setBaseUrl(const QString &url) {
    m_baseUrl = url.trimmed();
    while (m_baseUrl.endsWith('/')) {
        m_baseUrl.chop(1);
    }
    if (!m_baseUrl.isEmpty() && !m_baseUrl.startsWith("http://") && !m_baseUrl.startsWith("https://")) {
        m_baseUrl = "http://" + m_baseUrl;
    }
}

void GsClient::setPassword(const QString &pwd) {
    m_password = pwd.trimmed();
}

void GsClient::login(std::function<void(bool ok, const QString &err)> callback) {
    if (m_baseUrl.isEmpty()) {
        if (callback) callback(false, "未配置网关地址");
        return;
    }

    QUrl url(m_baseUrl + "/api/v1/auth/login");
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setRawHeader("Origin", m_baseUrl.toUtf8());
    request.setRawHeader("X-GS-Write", "1");

    QJsonObject reqObj;
    reqObj.insert("password", m_password);

    QNetworkReply *reply = m_nam->post(request, QJsonDocument(reqObj).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply, callback]() {
        reply->deleteLater();
        const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray body = reply->readAll();

        if (code == 200) {
            m_authenticated = true;
            // 提取 session Cookie 备用
            for (const auto &hdr : reply->rawHeaderPairs()) {
                if (hdr.first.toLower() == "set-cookie") {
                    QString s = QString::fromUtf8(hdr.second);
                    int idx = s.indexOf("session=");
                    if (idx >= 0) {
                        int end = s.indexOf(';', idx);
                        if (end < 0) end = s.length();
                        m_sessionCookie = s.mid(idx, end - idx).trimmed();
                    }
                }
            }
            emit authStatusChanged(true, "安全认证成功");
            if (callback) callback(true, "");
        } else {
            m_authenticated = false;
            m_sessionCookie.clear();
            QString err = QString("HTTP %1 %2").arg(code).arg(QString::fromUtf8(body));
            emit authStatusChanged(false, err);
            if (callback) callback(false, err);
        }
    });
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

    if (!m_authenticated) {
        login([this, waypoints, ttlSeconds](bool ok, const QString &err) {
            if (!ok) {
                emit missionResult(false, "任务发布被拒绝: 网关安全认证失败 (" + err + ")");
                return;
            }
            doSubmitWaypoints(waypoints, ttlSeconds, false);
        });
        return;
    }

    doSubmitWaypoints(waypoints, ttlSeconds, false);
}

void GsClient::doSubmitWaypoints(const QVector<Waypoint> &waypoints, int ttlSeconds, bool isRetry) {
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
    request.setRawHeader("Origin", m_baseUrl.toUtf8());
    request.setRawHeader("X-GS-Write", "1");
    if (!m_sessionCookie.isEmpty()) {
        request.setRawHeader("Cookie", m_sessionCookie.toUtf8());
    }

    QNetworkReply *reply = m_nam->post(request, QJsonDocument(req).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply, waypoints, ttlSeconds, isRetry]() {
        reply->deleteLater();
        const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray body = reply->readAll();

        if (code == 200 || code == 202) {
            emit missionResult(true, "任务发布成功已接收 (202 Accepted)");
        } else if (code == 401 && !isRetry) {
            // 会话可能超时失效，尝试重新登录并自动重试一次
            m_authenticated = false;
            m_sessionCookie.clear();
            login([this, waypoints, ttlSeconds](bool ok, const QString &err) {
                if (ok) {
                    doSubmitWaypoints(waypoints, ttlSeconds, true);
                } else {
                    emit missionResult(false, QString("网关会话失效且重新认证失败: %1").arg(err));
                }
            });
        } else {
            emit missionResult(false, QString("任务发布被拒绝: HTTP %1 %2").arg(code).arg(QString::fromUtf8(body)));
        }
    });
}

void GsClient::submitEmergencyStop() {
    if (m_latestTelemetry.uid.isEmpty() || m_latestTelemetry.bootNonce.isEmpty()) {
        emit missionResult(false, "尚未检测到在线船只");
        return;
    }

    if (!m_authenticated) {
        login([this](bool ok, const QString &err) {
            if (!ok) {
                emit missionResult(false, "急停发送被拒绝: 网关安全认证失败 (" + err + ")");
                return;
            }
            doSubmitEmergencyStop(false);
        });
        return;
    }

    doSubmitEmergencyStop(false);
}

void GsClient::doSubmitEmergencyStop(bool isRetry) {
    QJsonObject req;
    req.insert("operation_id", generateOperationId());
    req.insert("uid", m_latestTelemetry.uid);
    req.insert("expected_rp_boot_nonce", m_latestTelemetry.bootNonce);
    req.insert("kind", "stop");

    QUrl url(m_baseUrl + "/api/v1/missions/submit");
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setRawHeader("Origin", m_baseUrl.toUtf8());
    request.setRawHeader("X-GS-Write", "1");
    if (!m_sessionCookie.isEmpty()) {
        request.setRawHeader("Cookie", m_sessionCookie.toUtf8());
    }

    QNetworkReply *reply = m_nam->post(request, QJsonDocument(req).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply, isRetry]() {
        reply->deleteLater();
        const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray body = reply->readAll();

        if (code == 200 || code == 202) {
            emit missionResult(true, "紧急停船命令已接收 (202 Accepted)");
        } else if (code == 401 && !isRetry) {
            m_authenticated = false;
            m_sessionCookie.clear();
            login([this](bool ok, const QString &err) {
                if (ok) {
                    doSubmitEmergencyStop(true);
                } else {
                    emit missionResult(false, QString("网关会话失效且重新认证失败: %1").arg(err));
                }
            });
        } else {
            emit missionResult(false, QString("紧急停船被拒绝: HTTP %1 %2").arg(code).arg(QString::fromUtf8(body)));
        }
    });
}

} // namespace jlusv
