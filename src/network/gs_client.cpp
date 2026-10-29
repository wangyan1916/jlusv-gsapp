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

    // 筛选目标船只：严谨筛选 task_ready 为真且 bootNonce 有效且非全零的船只
    QJsonObject targetMember;
    for (const auto &val : members) {
        const QJsonObject m = val.toObject();
        const QString nonce = m.value("task_rp_boot_nonce").toString();
        const bool taskReady = m.value("task_ready").toBool();
        const bool online = m.value("device_online").toBool();
        if (taskReady && !nonce.isEmpty() && nonce != "0000000000000000") {
            targetMember = m;
            if (online) {
                break; // 优先选取在线且就绪的船只
            }
        }
    }
    if (targetMember.isEmpty()) {
        targetMember = members.first().toObject();
    }

    m_latestTelemetry.uid = targetMember.value("uid").toString();
    m_latestTelemetry.bootNonce = targetMember.value("task_rp_boot_nonce").toString();
    m_latestTelemetry.taskReady = targetMember.value("task_ready").toBool();
    m_latestTelemetry.deviceOnline = targetMember.value("device_online").toBool();
    m_latestTelemetry.loraState = targetMember.value("lora_state").toString();

    const QJsonObject nav = targetMember.value("navigation").toObject();
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

    const QJsonObject ctrl = nav.value("control_mode").toObject();
    m_latestTelemetry.controlOwner = ctrl.value("owner_code").toInt();
    m_latestTelemetry.controlState = ctrl.value("state_code").toInt();
    m_latestTelemetry.controlRevision = ctrl.value("revision").toVariant().toUInt();
    m_latestTelemetry.controlOwnerStr = ctrl.value("owner").toString("UNKNOWN");
    m_latestTelemetry.controlStateStr = ctrl.value("work_state").toString("UNKNOWN");
    m_latestTelemetry.controlPhase = ctrl.value("phase").toString("UNKNOWN");
    m_latestTelemetry.controlReason = ctrl.value("reason").toString("UNKNOWN");

    emit telemetryUpdated(m_latestTelemetry);
}

void GsClient::trackOperation(const QString &operationId,
                              int timeoutMs,
                              std::function<void(const QJsonObject &obj, bool &done)> onUpdate,
                              std::function<void(const QString &err)> onTimeout)
{
    auto timer = new QTimer(this);
    auto elapsed = std::make_shared<int>(0);

    connect(timer, &QTimer::timeout, this, [this, timer, elapsed, operationId, timeoutMs, onUpdate, onTimeout]() {
        *elapsed += 300;
        if (*elapsed >= timeoutMs) {
            timer->stop();
            timer->deleteLater();
            if (onTimeout) onTimeout("等待船端响应超时");
            return;
        }

        QUrl url(m_baseUrl + "/api/v1/missions/" + operationId);
        QNetworkRequest req(url);
        req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");

        QNetworkReply *reply = m_nam->get(req);
        connect(reply, &QNetworkReply::finished, this, [reply, timer, onUpdate]() {
            reply->deleteLater();
            if (reply->error() != QNetworkReply::NoError) {
                return;
            }
            QJsonDocument doc = QJsonDocument::fromJson(reply->readAll());
            if (!doc.isObject()) return;

            bool done = false;
            onUpdate(doc.object(), done);
            if (done) {
                timer->stop();
                timer->deleteLater();
            }
        });
    });

    timer->start(300);
}

void GsClient::switchControlMode(int owner, int state, std::function<void(bool ok, const QString &msg)> callback) {
    const QString opId = generateOperationId();
    QJsonObject req;
    req.insert("operation_id", opId);
    req.insert("uid", m_latestTelemetry.uid);
    req.insert("expected_rp_boot_nonce", m_latestTelemetry.bootNonce);
    req.insert("kind", "control_mode");
    req.insert("mode_owner", owner);
    req.insert("mode_state", state);
    req.insert("expected_control_revision", double(m_latestTelemetry.controlRevision));

    QUrl url(m_baseUrl + "/api/v1/missions/submit");
    QNetworkRequest request(url);
    request.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    request.setRawHeader("Origin", m_baseUrl.toUtf8());
    request.setRawHeader("X-GS-Write", "1");
    if (!m_sessionCookie.isEmpty()) {
        request.setRawHeader("Cookie", m_sessionCookie.toUtf8());
    }

    QNetworkReply *reply = m_nam->post(request, QJsonDocument(req).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply, opId, callback]() {
        reply->deleteLater();
        const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray body = reply->readAll();

        if (code != 200 && code != 202) {
            if (callback) callback(false, QString("网关拒绝模式切换请求: HTTP %1 %2").arg(code).arg(QString::fromUtf8(body)));
            return;
        }

        emit missionProgress("网关已受理模式切换请求 (202 Accepted)，正在等待船端执行确认 (MODE_APPLIED)...");

        trackOperation(opId, 8000, [this, callback](const QJsonObject &obj, bool &done) {
            const QString stage = obj.value("stage").toString();
            const QString status = obj.value("status").toString();
            const QString reason = obj.value("reason").toString();
            const QString execState = obj.value("execution_state").toString();

            if (execState == "MODE_APPLIED" || (stage == "RP_RECEIVED" && status == "OK")) {
                done = true;
                if (callback) callback(true, "MODE_APPLIED");
                return;
            }
            if (stage == "REJECTED" || status == "BUSY" || status == "CONFLICT") {
                done = true;
                if (callback) callback(false, QString("船端拒绝模式切换 [status: %1, reason: %2]").arg(status, reason));
                return;
            }
            if (stage == "TIMED_OUT") {
                done = true;
                if (callback) callback(false, "模式切换请求在无线链路超时 (未收到船端 ACK)");
                return;
            }
        }, [callback](const QString &err) {
            if (callback) callback(false, err);
        });
    });
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
    if (m_latestTelemetry.uid.isEmpty() || !m_latestTelemetry.taskReady ||
        m_latestTelemetry.bootNonce.isEmpty() || m_latestTelemetry.bootNonce == "0000000000000000") {
        emit missionResult(false, "目标船只尚未就绪 (task_ready 为假或 boot_nonce 全零，船端未接入 LoRa 任务链路)");
        return;
    }

    if (!m_authenticated) {
        login([this, waypoints, ttlSeconds](bool ok, const QString &err) {
            if (!ok) {
                emit missionResult(false, "网关安全认证失败: " + err);
                return;
            }
            submitWaypoints(waypoints, ttlSeconds);
        });
        return;
    }

    // 检查船端控制模式是否为 MAIN/WAYPOINTS (owner=1, state=3)
    if (m_latestTelemetry.controlOwner == 1 && m_latestTelemetry.controlState == 3) {
        emit missionProgress("船端当前已处于 [MAIN/WAYPOINTS] 循迹模式，开始发布航点...");
        doSubmitWaypoints(waypoints, ttlSeconds, false);
    } else {
        emit missionProgress(QString("船端当前模式为 [%1/%2]，正在申请切换为 [MAIN/WAYPOINTS 循迹模式]...")
            .arg(m_latestTelemetry.controlOwnerStr, m_latestTelemetry.controlStateStr));

        switchControlMode(1, 3, [this, waypoints, ttlSeconds](bool ok, const QString &msg) {
            if (!ok) {
                emit missionResult(false, "模式切换被拒绝: " + msg);
                return;
            }
            emit missionProgress("模式切换已在船端生效 (MODE_APPLIED)！正在下发航点...");
            doSubmitWaypoints(waypoints, ttlSeconds, false);
        });
    }
}

void GsClient::doSubmitWaypoints(const QVector<Waypoint> &waypoints, int ttlSeconds, bool isRetry) {
    const QString opId = generateOperationId();
    QJsonObject req;
    req.insert("operation_id", opId);
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
    connect(reply, &QNetworkReply::finished, this, [this, reply, waypoints, ttlSeconds, isRetry, opId]() {
        reply->deleteLater();
        const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray body = reply->readAll();

        if (code == 401 && !isRetry) {
            m_authenticated = false;
            m_sessionCookie.clear();
            login([this, waypoints, ttlSeconds](bool ok, const QString &err) {
                if (ok) {
                    doSubmitWaypoints(waypoints, ttlSeconds, true);
                } else {
                    emit missionResult(false, QString("网关会话失效且重新认证失败: %1").arg(err));
                }
            });
            return;
        }

        if (code != 200 && code != 202) {
            emit missionResult(false, QString("任务发布被网关拒绝: HTTP %1 %2").arg(code).arg(QString::fromUtf8(body)));
            return;
        }

        emit missionProgress("网关已受理航点 (202 Accepted)，正在等待船端 LoRa 确认接收...");

        // 持续轮询 /api/v1/missions/{operation_id} 跟踪真实船端执行回执
        trackOperation(opId, 12000, [this](const QJsonObject &obj, bool &done) {
            const QString stage = obj.value("stage").toString();
            const QString status = obj.value("status").toString();
            const QString reason = obj.value("reason").toString();
            const QString execState = obj.value("execution_state").toString();
            const QString execReason = obj.value("execution_reason").toString();

            if (stage == "RP_RECEIVED") {
                if (status == "OK") {
                    const QString sUpper = execState.toUpper();
                    const QString rUpper = execReason.toUpper();

                    if (sUpper == "RUNNING" || sUpper == "RUN") {
                        done = true;
                        emit missionResult(true, "航点已在船端生效，动力系统已启动循迹！");
                        return;
                    }
                    if (sUpper == "STARTING") {
                        done = true;
                        emit missionResult(true, "航点已在船端生效，零输出使能中 (STARTING)...");
                        return;
                    }
                    if (sUpper == "WAITING_IPC" || sUpper == "WAITING" || sUpper == "IDLE" || sUpper.isEmpty()) {
                        done = true;
                        QString msg = "航点已成功被船端接收 (LoRa ACK确认)！当前状态: 等待执行条件 (WAITING_IPC)。";
                        if (qFuzzyIsNull(m_latestTelemetry.latDeg) && qFuzzyIsNull(m_latestTelemetry.lonDeg)) {
                            msg += "\n提示: 船端当前经纬度为 0.000000, 0.000000 (GNSS 尚未完成有效水面定位锁定)。船端主控保护机制已启动，一旦获取有效 RTK/GNSS 位置与航向，即可自动起航！";
                        } else {
                            msg += "\n提示: 船端正在等待传感器反馈就绪 (需要有效航向、航速及 RTK 定位)，满足条件后自动起航。";
                        }
                        emit missionResult(true, msg);
                        return;
                    }
                    if (sUpper == "SENSOR_INVALID" || sUpper == "SENSORINVALID" || rUpper == "SENSOR_INVALID") {
                        done = true;
                        emit missionResult(false, "船端接收航点但无法执行: 传感器数据无效或过期 (缺少新鲜航向/RTK定位/航速)");
                        return;
                    }
                    if (sUpper == "STOPPING" || sUpper == "STOPPED" || rUpper == "USER_STOP" || rUpper == "USERSTOP") {
                        done = true;
                        emit missionResult(false, "船端任务已被急停打断");
                        return;
                    }
                    done = true;
                    emit missionResult(true, QString("航点已成功被船端接收 (LoRa ACK确认)！当前执行状态: %1").arg(execState));
                    return;
                } else {
                    done = true;
                    emit missionResult(false, QString("船端拒绝执行航点: status=%1, reason=%2").arg(status, reason));
                    return;
                }
            } else if (stage == "REJECTED") {
                done = true;
                emit missionResult(false, QString("任务被船端拒绝: status=%1, reason=%2").arg(status, reason));
                return;
            } else if (stage == "TIMED_OUT") {
                done = true;
                emit missionResult(false, "任务发布超时: 未在时限内收到船端 LoRa ACK 回执");
                return;
            } else if (stage == "CANCELLED") {
                done = true;
                emit missionResult(false, "任务已被取消或被覆盖");
                return;
            } else {
                emit missionProgress(QString("任务正在传输中 (网关阶段: %1)...").arg(stage));
            }
        }, [this](const QString &err) {
            emit missionResult(false, "任务执行跟踪超时: " + err);
        });
    });
}

void GsClient::submitEmergencyStop() {
    if (m_latestTelemetry.uid.isEmpty() || !m_latestTelemetry.taskReady ||
        m_latestTelemetry.bootNonce.isEmpty() || m_latestTelemetry.bootNonce == "0000000000000000") {
        emit missionResult(false, "尚未检测到在线就绪船只");
        return;
    }

    if (!m_authenticated) {
        login([this](bool ok, const QString &err) {
            if (!ok) {
                emit missionResult(false, "急停发送被拒绝: 网关安全认证失败 (" + err + ")");
                return;
            }
            submitEmergencyStop();
        });
        return;
    }

    doSubmitEmergencyStop(false);
}

void GsClient::doSubmitEmergencyStop(bool isRetry) {
    const QString opId = generateOperationId();
    QJsonObject req;
    req.insert("operation_id", opId);
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
    connect(reply, &QNetworkReply::finished, this, [this, reply, isRetry, opId]() {
        reply->deleteLater();
        const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const QByteArray body = reply->readAll();

        if (code == 401 && !isRetry) {
            m_authenticated = false;
            m_sessionCookie.clear();
            login([this](bool ok, const QString &err) {
                if (ok) {
                    doSubmitEmergencyStop(true);
                } else {
                    emit missionResult(false, QString("网关会话失效且重新认证失败: %1").arg(err));
                }
            });
            return;
        }

        if (code != 200 && code != 202) {
            emit missionResult(false, QString("紧急停船被网关拒绝: HTTP %1 %2").arg(code).arg(QString::fromUtf8(body)));
            return;
        }

        emit missionProgress("网关已受理急停指令 (202 Accepted)，正在等待船端停机确认...");

        trackOperation(opId, 8000, [this](const QJsonObject &obj, bool &done) {
            const QString stage = obj.value("stage").toString();
            const QString status = obj.value("status").toString();
            const QString execState = obj.value("execution_state").toString();

            const QString sUpper = execState.toUpper();
            if (stage == "RP_RECEIVED" || sUpper == "STOPPING" || sUpper == "STOPPED" || sUpper == "USERSTOP" || sUpper == "USER_STOP") {
                done = true;
                emit missionResult(true, "紧急停船已在物理船端生效停机！");
                return;
            }
            if (stage == "REJECTED") {
                done = true;
                emit missionResult(false, "急停指令被船端拒绝: " + status);
                return;
            }
            if (stage == "TIMED_OUT") {
                done = true;
                emit missionResult(false, "急停指令超时: 未收到船端 LoRa ACK");
                return;
            }
        }, [this](const QString &err) {
            emit missionResult(false, "急停跟踪超时: " + err);
        });
    });
}

} // namespace jlusv
