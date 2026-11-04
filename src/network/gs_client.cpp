#include "gs_client.h"
#include <QJsonDocument>
#include <QNetworkCookieJar>
#include <QUuid>
#include <cmath>
#include <cstdint>

namespace jlusv {
namespace {
quint32 number(const QJsonObject &obj, const char *key) {
    const double n = obj.value(key).toDouble(-1);
    return std::isfinite(n) && n >= 0 && n <= UINT32_MAX && std::floor(n) == n ? quint32(n) : 0;
}
qint64 telemetryAge(const QJsonObject &obj, const char *key) {
    const auto value = obj.value(key);
    const double n = value.toDouble(-1);
    return value.isDouble() && std::isfinite(n) && n >= 0 && n < UINT32_MAX && std::floor(n) == n
        ? qint64(n) : -1;
}
bool inRange(const QJsonObject &obj, const char *key, double low, double high) {
    const auto value = obj.value(key);
    return value.isDouble() && std::isfinite(value.toDouble()) && value.toDouble() >= low && value.toDouble() <= high;
}
quint32 boot32(const QString &text) {
    const quint64 boot = text.toULongLong(nullptr, 16);
    return quint32(boot) ? quint32(boot) : quint32(boot >> 32);
}
bool validNonce(const QString &text) {
    bool ok = false;
    const quint64 boot = text.toULongLong(&ok, 16);
    return text.size() == 16 && ok && boot != 0;
}
bool freshReceipt(const QJsonObject &r) {
    const auto age = r.value("execution_age_ms");
    return r.value("execution_fresh").toBool() && age.isDouble() &&
           age.toDouble() >= 0 && age.toDouble() <= 5000;
}
bool zeroOutputs(const QJsonObject &p) {
    const auto motor = p.value("motor").toArray();
    return p.value("valid").toBool() && p.value("armed").isBool() && !p.value("armed").toBool() &&
           p.value("servo_enabled").isDouble() && p.value("servo_enabled").toInt(-1) == 0 &&
           motor.size() == 2 && motor[0].isDouble() && motor[1].isDouble() &&
           motor[0].toDouble() == 0 && motor[1].toDouble() == 0;
}
}

QString executionText(const QString &state) {
    if (state == "MODE_APPLIED") return "模式已生效";
    if (state == "WAITING_IPC") return "等待执行条件";
    if (state == "STARTING") return "零输出使能中";
    if (state == "RUNNING") return "执行中";
    if (state == "STOPPING") return "停止中";
    if (state == "STOPPED") return "已停止";
    if (state == "FAILED") return "执行中止";
    if (state == "SUPERSEDED") return "已被新任务替代";
    if (state == "IDLE") return "待机";
    return "等待新鲜执行回报";
}

QString executionReasonText(const QString &reason) {
    if (reason == "NONE" || reason.isEmpty()) return "无";
    if (reason == "SENSOR_INVALID") return "所需方向、位置或航速无效/过期";
    if (reason == "IPC_LOST") return "工控连接中断";
    if (reason == "GS_PERMISSION_LOST") return "地面站执行许可失效";
    if (reason == "CONTROLLER_NOT_CONFIGURED") return "控制器或舵向配置未就绪";
    if (reason == "WAITING_TOPIC") return "等待工控对应输入/任务";
    if (reason == "ACTUATOR_ERROR") return "执行器未完成应用或输出已关闭";
    if (reason == "AUTHORITY_CHANGED") return "控制权已切换";
    if (reason == "MODE_CHANGED") return "控制模式已切换";
    if (reason == "USER_STOP") return "用户停止";
    if (reason == "ROUTE_COMPLETE") return "航路点已完成";
    if (reason == "EXPERIMENT_COMPLETE") return "实验已完成";
    if (reason == "EXPERIMENT_ABORTED") return "实验中止：条件失效、达到时限或配置不满足，详见船端日志";
    if (reason == "NEWER_TASK") return "已有更新任务";
    if (reason == "WAITING_FRESH_EXECUTION_REPORT") return "等待船端新鲜执行回报";
    return reason;
}

GsClient::GsClient(QObject *parent) : QObject(parent), m_nam(this) {
    m_clock.start();
    m_nam.setCookieJar(new QNetworkCookieJar(&m_nam));
    m_operationTimer.setInterval(500);
    connect(&m_pollTimer, &QTimer::timeout, this, &GsClient::pollOverview);
    connect(&m_operationTimer, &QTimer::timeout, this, &GsClient::queryOperation);
}

void GsClient::setBusy(bool busy) {
    if (m_busy == busy) return;
    m_busy = busy;
    emit busyChanged(busy);
}

void GsClient::setAuthenticated(bool authenticated, const QString &message) {
    m_authenticated = authenticated;
    emit authStatusChanged(authenticated, message);
}

void GsClient::clearOperation() {
    ++m_operationToken;
    ++m_queryToken;
    m_operationTimer.stop();
    m_queryBusy = false;
    m_operation = {};
    setBusy(false);
}

void GsClient::invalidateTelemetry() {
    m_latestTelemetry = {};
    m_latestTelemetry.uid = m_selectedUid;
    m_overviewAt = -10000;
    emit telemetryUpdated(m_latestTelemetry);
}

void GsClient::resetContext(const QString &reason) {
    ++m_context;
    m_overviewBusy = false;
    m_groundBusy = false;
    m_groundAt = m_groundRequestedAt = -10000;
    m_groundSnapshot = {};
    updateGroundStationPosition();
    if (!m_operation.request.isEmpty()) emit missionResult(false, reason + "；此前操作结果须核对船端实际状态");
    clearOperation();
    invalidateTelemetry();
    emit operationUpdated({});
}

void GsClient::setBaseUrl(const QString &url) {
    QString normalized = url.trimmed();
    while (normalized.endsWith('/')) normalized.chop(1);
    if (!normalized.isEmpty() && !normalized.startsWith("http://") && !normalized.startsWith("https://"))
        normalized.prepend("http://");
    if (normalized == m_baseUrl) return;
    resetContext("网关地址已改变");
    m_baseUrl = normalized;
    m_gsUid.clear();
    m_members = {};
    m_nam.setCookieJar(new QNetworkCookieJar(&m_nam));
    setAuthenticated(false, "请连接并认证新网关");
    emit fleetUpdated(m_members);
}

void GsClient::setPassword(const QString &password) {
    if (m_password == password) return;
    m_password = password;
    setAuthenticated(false, "控制密码已改变，请重新连接认证");
}

void GsClient::setTargetUid(const QString &uid) {
    if (uid == m_selectedUid) return;
    resetContext("目标船只已改变");
    m_selectedUid = uid;
    updateTelemetry();
}

bool GsClient::canControl(bool requireMode) const {
    const auto &t = m_latestTelemetry;
    return m_authenticated && m_connected && m_clock.elapsed() - m_overviewAt <= 2500 &&
           !m_gsUid.isEmpty() && !t.uid.isEmpty() && t.taskReady && validNonce(t.bootNonce) &&
           (!requireMode || (t.fresh && t.controlFresh && t.controlRevision > 0 &&
                            m_navigationAge + m_clock.elapsed() - m_overviewAt <= 5000));
}

bool GsClient::canPublish(int owner, int work) const {
    return !m_busy && canControl() && m_latestTelemetry.controlOwner == owner &&
           m_latestTelemetry.controlState == work;
}

QNetworkRequest GsClient::request(const QString &path, bool write) const {
    QNetworkRequest req(QUrl(m_baseUrl + path));
    req.setHeader(QNetworkRequest::ContentTypeHeader, "application/json");
    req.setTransferTimeout(2500);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute, QNetworkRequest::ManualRedirectPolicy);
    if (write) {
        const QUrl url(m_baseUrl);
        req.setRawHeader("Origin", (url.scheme() + "://" + url.authority()).toUtf8());
        req.setRawHeader("X-GS-Write", "1");
    }
    return req;
}

void GsClient::login(std::function<void(bool, const QString &)> callback) {
    const quint64 context = m_context;
    const QString password = m_password;
    auto *reply = m_nam.post(request("/api/v1/auth/login", true),
                            QJsonDocument(QJsonObject{{"password", password}}).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply, context, password, callback]() {
        reply->deleteLater();
        if (context != m_context || password != m_password) return;
        const bool ok = reply->error() == QNetworkReply::NoError &&
                        reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt() == 200;
        const QString message = ok ? "安全认证成功" : "认证失败，请检查地址与密码：" + reply->errorString();
        setAuthenticated(ok, message);
        if (callback) callback(ok, message);
    });
}

void GsClient::startPolling(int intervalMs) {
    m_pollTimer.start(qMax(100, intervalMs));
    pollOverview();
}

void GsClient::stopPolling() {
    const bool wasConnected = m_connected, wasAuthenticated = m_authenticated;
    m_pollTimer.stop();
    resetContext("已断开网关");
    m_connected = false;
    m_authenticated = false;
    if (wasAuthenticated) emit authStatusChanged(false, "已断开");
    if (wasConnected) emit connectionStatusChanged(false);
}

void GsClient::pollOverview() {
    // Expire locally even when a stalled HTTP request has not finished yet.
    if (m_clock.elapsed() - m_overviewAt > 2500 && m_latestTelemetry.fresh) invalidateTelemetry();
    updateGroundStationPosition();
    pollGroundStation();
    if (m_overviewBusy) return;
    m_overviewBusy = true;
    const quint64 context = m_context;
    auto *reply = m_nam.get(request("/api/v1/fleet/overview"));
    connect(reply, &QNetworkReply::finished, this, [this, reply, context]() {
        reply->deleteLater();
        if (context != m_context) return;
        m_overviewBusy = false;
        QJsonParseError error;
        const auto doc = QJsonDocument::fromJson(reply->readAll(), &error);
        const auto root = doc.object();
        if (reply->error() != QNetworkReply::NoError || error.error != QJsonParseError::NoError ||
            !root.value("members").isArray() || root.value("gs_uid").toString().isEmpty()) {
            invalidateTelemetry();
            if (m_connected) { m_connected = false; emit connectionStatusChanged(false); }
            return;
        }
        const QString gsUid = root.value("gs_uid").toString();
        if (!m_gsUid.isEmpty() && gsUid != m_gsUid) {
            resetContext("网关身份已改变");
            setAuthenticated(false, "网关身份已改变，请重新连接认证");
            m_nam.setCookieJar(new QNetworkCookieJar(&m_nam));
        }
        m_gsUid = gsUid;
        m_members = root.value("members").toArray();
        m_overviewAt = m_clock.elapsed();
        if (!m_connected) { m_connected = true; emit connectionStatusChanged(true); }
        // Pick once; disappearance of a selected vessel must never retarget a command.
        if (m_selectedUid.isEmpty() && !m_members.isEmpty()) {
            m_selectedUid = m_members.first().toObject().value("uid").toString();
            for (const auto &member : m_members) if (member.toObject().value("task_ready").toBool()) {
                m_selectedUid = member.toObject().value("uid").toString(); break;
            }
        }
        updateTelemetry();
        emit fleetUpdated(m_members);
        pollGroundStation();
    });
}

void GsClient::pollGroundStation() {
    if (m_gsUid.isEmpty() || m_groundBusy || m_clock.elapsed() - m_groundRequestedAt < 1000) return;
    m_groundBusy = true;
    const quint64 context = m_context;
    const qint64 requestedAt = m_groundRequestedAt = m_clock.elapsed();
    auto *reply = m_nam.get(request("/api/v1/gs/gnss/receiver"));
    connect(reply, &QNetworkReply::finished, this, [this, reply, context, requestedAt]() {
        reply->deleteLater();
        if (context != m_context) return;
        m_groundBusy = false;
        QJsonParseError error;
        const auto doc = QJsonDocument::fromJson(reply->readAll(), &error);
        m_groundSnapshot = reply->error() == QNetworkReply::NoError && error.error == QJsonParseError::NoError
            ? doc.object() : QJsonObject{};
        // Include HTTP transit time instead of renewing a cached source's age.
        m_groundAt = requestedAt;
        updateGroundStationPosition();
    });
}

void GsClient::updateGroundStationPosition() {
    const auto gnss = m_groundSnapshot.value("gnss").toObject();
    const qint64 elapsed = qMax<qint64>(0, m_clock.elapsed() - m_groundAt);
    GroundStationPosition p;
    p.available = gnss.value("position_available").toBool() &&
                  inRange(gnss, "latitude", -90, 90) && inRange(gnss, "longitude", -180, 180);
    p.latDeg = gnss.value("latitude").toDouble(); p.lonDeg = gnss.value("longitude").toDouble();
    p.source = gnss.value("position_source").toString();
    p.ageMs = telemetryAge(gnss, "base_position_age_ms");
    if (p.ageMs >= 0) p.ageMs += elapsed;
    const qint64 statusAge = telemetryAge(gnss, "status_age_ms");
    const qint64 ttl = p.source == "RTCM1005" || p.source == "NMEA_GGA" ? 3000 : 5000;
    // position_valid already includes the GS RP runtime freshness check in the
    // receiver endpoint. Match the GS card's additional source/status age limits.
    p.valid = p.available && m_groundSnapshot.value("available").toBool() &&
              gnss.value("position_valid").toBool() && elapsed <= 2500 &&
              statusAge >= 0 && statusAge + elapsed < 5000 && p.ageMs >= 0 && p.ageMs < ttl;
    p.estimated = gnss.value("position_estimated").toBool();
    p.old = p.available && (!p.valid || gnss.value("position_old").toBool());
    m_groundPosition = p;
    emit groundStationUpdated(m_groundPosition);
}

void GsClient::updateTelemetry() {
    QJsonObject member;
    for (const auto &value : m_members) if (value.toObject().value("uid").toString() == m_selectedUid) {
        member = value.toObject(); break;
    }
    const auto nav = member.value("navigation").toObject();
    const auto mode = nav.value("control_mode").toObject();
    const auto task = nav.value("ipc_task").toObject();
    const QString boot = member.value("task_rp_boot_nonce").toString();
    if (!m_operation.request.isEmpty() && validNonce(boot) &&
        boot != m_operation.request.value("expected_rp_boot_nonce").toString())
        finishOperation(false, "船端已重启，旧操作失效；请核对模式后重新发布新任务");
    UsvTelemetry t;
    const auto age = nav.value("age_ms");
    m_navigationAge = age.isDouble() && age.toDouble() >= 0 && age.toDouble() <= 5000 ? qint64(age.toDouble()) : 5001;
    t.uid = m_selectedUid; t.bootNonce = boot;
    t.taskReady = member.value("task_ready").toBool() && member.value("state").toString() == "JOINED";
    t.deviceOnline = member.value("device_online").toBool();
    t.loraState = member.value("lora_state").toString();
    t.available = nav.value("available").toBool();
    t.fresh = nav.value("fresh").toBool() && m_clock.elapsed() - m_overviewAt <= 2500 &&
              m_navigationAge + m_clock.elapsed() - m_overviewAt <= 5000;
    // Position and direction have separate source ages; loss of position must
    // not invalidate a fresh compass/control direction or the task receipt.
    const qint64 cachedMs = qMax<qint64>(0, m_clock.elapsed() - m_overviewAt);
    t.navAgeMs = telemetryAge(nav, "nav_age_ms");
    t.headingAgeMs = telemetryAge(nav, "heading_age_ms");
    if (t.navAgeMs >= 0) t.navAgeMs += cachedMs;
    if (t.headingAgeMs >= 0) t.headingAgeMs += cachedMs;
    const bool navFresh = t.fresh && (!nav.contains("nav_age_ms") || (t.navAgeMs >= 0 && t.navAgeMs <= 5000));
    t.positionValid = navFresh && nav.value("position_valid").toBool() &&
                      inRange(nav, "lat_deg", -90, 90) && inRange(nav, "lon_deg", -180, 180);
    t.latDeg = nav.value("lat_deg").toDouble(); t.lonDeg = nav.value("lon_deg").toDouble();
    t.headingValid = t.fresh && nav.value("heading_valid").toBool() &&
                     inRange(nav, "heading_deg", 0, 360) && nav.value("heading_deg").toDouble() < 360 &&
                     (!nav.contains("heading_age_ms") || (t.headingAgeMs >= 0 && t.headingAgeMs <= 3000));
    t.headingDeg = nav.value("heading_deg").toDouble();
    t.headingSource = nav.value("heading_source").toString();
    t.headingValid = t.headingValid && t.headingSource == "CALIBRATED_MAGNETOMETER";
    t.headingReference = nav.value("heading_reference").toString();
    t.headingReason = nav.value("heading_reason").toString();
    t.speedValid = navFresh && inRange(nav, "sog_mps", 0, 655.34);
    t.cogValid = navFresh && inRange(nav, "cog_deg", 0, 360) && nav.value("cog_deg").toDouble() < 360;
    t.sogMps = nav.value("sog_mps").toDouble(); t.cogDeg = nav.value("cog_deg").toDouble();
    const auto rtk = nav.value("rtk_state");
    const double rtkCode = rtk.toDouble(-1);
    t.rtkState = t.fresh && rtk.isDouble() && rtkCode >= 0 && rtkCode <= 3 && std::floor(rtkCode) == rtkCode
        ? int(rtkCode) : 0;
    t.satellites = nav.value("satellites").toInt();
    t.batteryValid = t.fresh && nav.value("battery_v").isDouble();
    t.batteryV = nav.value("battery_v").toDouble();
    t.positionSource = nav.value("position_source").toString();
    t.driftMs = telemetryAge(nav, "drift_ms");
    t.diffAgeState = nav.value("diff_age_state").toString();
    t.diffAgeMs = telemetryAge(nav, "diff_age_ms");
    if (!t.fresh || t.diffAgeState != "VALID" || t.diffAgeMs > 3000) t.diffAgeMs = -1;
    t.controlFresh = t.fresh && mode.value("fresh").toBool();
    t.controlOwner = mode.value("owner_code").toInt(); t.controlState = mode.value("state_code").toInt();
    t.controlRevision = number(mode, "revision");
    t.modeGsBoot = number(mode, "gs_boot"); t.modeCommandId = number(mode, "command_id");
    t.controlOwnerStr = mode.value("owner").toString("UNKNOWN");
    t.controlStateStr = mode.value("work_state").toString("UNKNOWN");
    t.controlPhase = mode.value("phase").toString("UNKNOWN");
    t.controlReason = mode.value("reason").toString("UNKNOWN");
    t.waypointIndex = mode.value("waypoint_index").toInt(); t.waypointCount = mode.value("waypoint_count").toInt();
    t.taskFresh = t.fresh && task.value("fresh").toBool();
    t.taskGsBoot = number(task, "gs_boot"); t.taskCommandId = number(task, "command_id");
    t.taskPhase = task.value("state").toString("UNKNOWN");
    t.taskReason = task.value("reason").toString("UNKNOWN");
    t.outputsOff = t.fresh && zeroOutputs(nav.value("power").toObject());
    m_latestTelemetry = t;
    emit telemetryUpdated(t);
    if (m_operation.posted && m_operationTimer.isActive()) evaluateOperation();
}

QJsonObject GsClient::taskRequest(const QString &kind, int owner, int work) {
    if (!canPublish(owner, work)) {
        emit missionResult(false, "请先连接认证，并应用所选主控/工控工作模式，等待船端新鲜确认");
        return {};
    }
    return {{"kind", kind}};
}

void GsClient::switchControlMode(int owner, int work, std::function<void(bool, const QString &)> callback) {
    if (m_busy || !canControl() || owner < 1 || owner > 2 || work < 1 || work > 3 ||
        m_latestTelemetry.controlRevision == UINT32_MAX) {
        const QString message = "模式切换条件未满足，请等待认证、新鲜模式或上次操作结束";
        emit missionResult(false, message);
        if (callback) callback(false, message);
        return;
    }
    beginOperation({{"kind", "control_mode"}, {"mode_owner", owner}, {"mode_state", work},
                    {"expected_control_revision", double(m_latestTelemetry.controlRevision)}}, callback);
}

void GsClient::submitWaypoints(const QVector<Waypoint> &points, int ttlSeconds, int owner) {
    if (points.isEmpty() || points.size() > 16 || ttlSeconds < 1 || ttlSeconds > 3600) {
        emit missionResult(false, "需要1～16个航点，有效期1～3600秒"); return;
    }
    QJsonArray array;
    for (const auto &wp : points) {
        if (!std::isfinite(wp.latDeg) || !std::isfinite(wp.lonDeg) || !std::isfinite(wp.speedMps) ||
            std::abs(wp.latDeg) > 90 || std::abs(wp.lonDeg) > 180 || wp.speedMps < 0 || wp.speedMps > 3) {
            emit missionResult(false, "航点坐标或速度无效，速度必须为0～3 m/s"); return;
        }
        array.append(QJsonObject{{"lat_deg", wp.latDeg}, {"lon_deg", wp.lonDeg}, {"speed_mps", wp.speedMps}});
    }
    auto req = taskRequest("waypoints", owner, 3);
    if (req.isEmpty()) return;
    req.insert("points", array); req.insert("ttl_s", ttlSeconds);
    beginOperation(req);
}

void GsClient::submitHeadingSpeed(int owner, double headingDeg, double speedMps) {
    if (!std::isfinite(headingDeg) || headingDeg < 0 || headingDeg >= 360 ||
        !std::isfinite(speedMps) || speedMps < 0 || speedMps > 3) {
        emit missionResult(false, "航向须在[0,360)°，航速须在0～3 m/s"); return;
    }
    auto req = taskRequest("ipc_heading_speed", owner, 2);
    if (req.isEmpty()) return;
    req.insert("heading_deg", headingDeg); req.insert("speed_mps", speedMps);
    beginOperation(req);
}

void GsClient::submitHeadingOnly(int owner, double headingDeg, double throttlePercent) {
    if (!std::isfinite(headingDeg) || headingDeg < 0 || headingDeg >= 360 ||
        !std::isfinite(throttlePercent) || throttlePercent < 0 || throttlePercent > 100) {
        emit missionResult(false, "航向须在[0,360)°，固定推进须在0～100%（0%仍控舵）"); return;
    }
    auto req = taskRequest("heading_only", owner, 2);
    if (req.isEmpty()) return;
    req.insert("heading_deg", headingDeg); req.insert("throttle_percent", throttlePercent);
    beginOperation(req);
}

void GsClient::submitManeuver(bool zigzag, double rudderDeg, bool speedMode, double drive,
                              double thresholdDeg, int count, int maxSeconds) {
    if (!std::isfinite(rudderDeg) || std::abs(rudderDeg) < 1 || std::abs(rudderDeg) > 30 ||
        !std::isfinite(drive) || drive < 0 || drive > (speedMode ? 3 : 100) || (speedMode && drive == 0) ||
        !std::isfinite(thresholdDeg) || (zigzag ? thresholdDeg < 1 || thresholdDeg > 90 : thresholdDeg != 0) ||
        count < (zigzag ? 1 : 4) || count > 16 || maxSeconds < 10 || maxSeconds > 600) {
        emit missionResult(false, "实验参数无效：舵角±1～30°，定速须大于0且不超过3m/s，固定推进0～100%，时限10～600秒"); return;
    }
    if (m_latestTelemetry.taskFresh && (m_latestTelemetry.taskPhase == "RUNNING" ||
        m_latestTelemetry.taskPhase == "STARTING" || m_latestTelemetry.taskPhase == "STOPPING")) {
        emit missionResult(false, "请先停船并确认输出关闭，再发布新实验"); return;
    }
    auto req = taskRequest(zigzag ? "zigzag" : "turning", 2, 2);
    if (req.isEmpty()) return;
    req.insert("rudder_deg", rudderDeg); req.insert("drive_mode", speedMode ? "speed" : "thrust");
    req.insert(speedMode ? "speed_mps" : "throttle_percent", drive);
    req.insert("threshold_deg", zigzag ? thresholdDeg : 0); req.insert("count", count); req.insert("max_seconds", maxSeconds);
    beginOperation(req);
}

void GsClient::submitIdentification(int profile,double thrustPercent,double rudderDeg,int count,
                                   int settleSeconds,int holdSeconds,int coastSeconds,int maxSeconds) {
    if(profile<1||profile>5||!std::isfinite(thrustPercent)||thrustPercent<0||thrustPercent>100||
       !std::isfinite(rudderDeg)||count<1||count>16||(profile!=5&&count!=1)||settleSeconds<1||settleSeconds>60||
       holdSeconds<1||holdSeconds>300||coastSeconds<0||coastSeconds>300||maxSeconds<10||maxSeconds>600||
       settleSeconds+holdSeconds*count+coastSeconds+5>maxSeconds||
       (profile==3?std::abs(rudderDeg)!=90:rudderDeg!=0)||
       (profile==4?thrustPercent!=0:thrustPercent<=0)||
       ((profile==2||profile==3)?coastSeconds<=0:coastSeconds!=0)) {
        emit missionResult(false,"实验参数无效：总时限须覆盖准备、各档保持、滑行及5秒握手余量；横漂只允许±90°，横摇推进必须为0。");return;
    }
    if(m_latestTelemetry.taskFresh&&(m_latestTelemetry.taskPhase=="RUNNING"||m_latestTelemetry.taskPhase=="STARTING"||m_latestTelemetry.taskPhase=="STOPPING")){
        emit missionResult(false,"请先停船并确认输出关闭，再发布实验");return;
    }
    auto req=taskRequest("identification",2,2);if(req.isEmpty())return;
    const bool origin=m_groundPosition.valid;
    const qint32 lat=origin?qint32(std::llround(m_groundPosition.latDeg*1e7)):0;
    const qint32 lon=origin?qint32(std::llround(m_groundPosition.lonDeg*1e7)):0;
    const quint32 revision=origin?qMax(1u,qHash(QString::number(lat)+","+QString::number(lon),0)):0;
    req.insert("profile",profile);req.insert("count",count);req.insert("settle_seconds",settleSeconds);
    req.insert("hold_seconds",holdSeconds);req.insert("coast_seconds",coastSeconds);req.insert("max_seconds",maxSeconds);
    req.insert("rudder_cdeg",int(std::lround(rudderDeg*100)));req.insert("thrust_permille",int(std::lround(thrustPercent*10)));
    req.insert("origin_valid",origin?1:0);req.insert("origin_latitude_e7",lat);req.insert("origin_longitude_e7",lon);
    req.insert("origin_revision",double(revision));beginOperation(req);
}

void GsClient::submitEmergencyStop() {
    // STOP preempts local submission/mode tracking and does not require a fresh mode.
    if (!canControl(false)) { emit missionResult(false, "无法发送停船：等待目标船LoRa身份和网关认证"); return; }
    beginOperation({{"kind", "stop"}});
}

void GsClient::beginOperation(QJsonObject req, std::function<void(bool, const QString &)> callback) {
    clearOperation();
    req.insert("operation_id", QUuid::createUuid().toString(QUuid::WithoutBraces).remove('-').toLower());
    req.insert("uid", m_latestTelemetry.uid);
    req.insert("expected_rp_boot_nonce", m_latestTelemetry.bootNonce);
    m_operation.request = req; m_operation.gsUid = m_gsUid;
    m_operation.startedAt = m_operation.lastFreshAt = m_clock.elapsed();
    m_operation.modeCallback = callback;
    setBusy(true);
    const quint64 context = m_context, token = m_operationToken;
    operationNotice("正在提交；等待网关受理及船端实际回报");
    auto *reply = m_nam.post(request("/api/v1/missions/submit", true), QJsonDocument(req).toJson(QJsonDocument::Compact));
    connect(reply, &QNetworkReply::finished, this, [this, reply, context, token]() {
        reply->deleteLater();
        if (context != m_context || token != m_operationToken) return;
        const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
        const auto body = reply->readAll();
        m_operation.posted = true;
        if (code >= 400 && code < 500) {
            if (code == 401) setAuthenticated(false, "控制会话失效，请重新连接认证");
            finishOperation(false, QString("网关拒绝：HTTP %1 %2").arg(code).arg(QString::fromUtf8(body))); return;
        }
        if ((code == 200 || code == 202) && reply->error() == QNetworkReply::NoError &&
            acceptReceipt(QJsonDocument::fromJson(body).object())) evaluateOperation();
        else {
            if (m_operation.queryError.isEmpty()) m_operation.queryError = "提交结果未确认，正在查询原操作编号；不会自动重发";
            evaluateOperation();
        }
        if (token == m_operationToken && !m_operation.request.isEmpty()) m_operationTimer.start();
    });
}

bool GsClient::acceptReceipt(const QJsonObject &r) {
    const auto &req = m_operation.request;
    if (req.isEmpty() || m_operation.gsUid != m_gsUid || r.value("operation_id") != req.value("operation_id") ||
        r.value("uid") != req.value("uid") || r.value("kind") != req.value("kind") ||
        number(r, "expected_rp_boot") != boot32(req.value("expected_rp_boot_nonce").toString()) ||
        (r.value("stage").toString() == "RP_RECEIVED" && number(r, "actual_rp_boot") != number(r, "expected_rp_boot")) ||
        !number(r, "command_id") || !number(r, "gs_boot") ||
        (m_operation.commandId && m_operation.commandId != number(r, "command_id")) ||
        (m_operation.gsBoot && m_operation.gsBoot != number(r, "gs_boot"))) {
        m_operation.queryError = "回执身份不匹配或格式无效，执行结果未确认"; return false;
    }
    m_operation.commandId = number(r, "command_id"); m_operation.gsBoot = number(r, "gs_boot");
    m_operation.receipt = r;
    m_operation.queryError.clear();
    if (freshReceipt(r)) {
        m_operation.lastFreshAt = m_clock.elapsed();
        m_operation.receivedExecution = true;
    }
    return true;
}

void GsClient::queryOperation() {
    if (m_operation.request.isEmpty() || !m_operation.posted) return;
    const qint64 now = m_clock.elapsed();
    const QString kind = m_operation.request.value("kind").toString();
    const bool needsApplication = kind == "control_mode" || kind == "stop";
    if (m_operationTimer.isActive() && !m_operation.notifiedUnknown &&
        ((needsApplication && now - m_operation.startedAt >= 20000) || now - m_operation.lastFreshAt >= 20000)) {
        m_operation.notifiedUnknown = true;
        m_operationTimer.stop();
        ++m_queryToken;
        m_queryBusy = false;
        setBusy(false);
        const QString message = "确认超时，执行结果未知；自动跟踪已停止，可手动刷新原操作回执。不会重新发布";
        operationNotice(message);
        emit missionResult(false, message);
        if (m_operation.modeCallback) {
            auto callback = std::move(m_operation.modeCallback); m_operation.modeCallback = {};
            callback(false, message);
        }
        return;
    }
    if (m_queryBusy) return;
    m_queryBusy = true;
    const quint64 context = m_context, token = m_operationToken, queryToken = ++m_queryToken;
    auto *reply = m_nam.get(request("/api/v1/missions/" + m_operation.request.value("operation_id").toString()));
    connect(reply, &QNetworkReply::finished, this, [this, reply, context, token, queryToken]() {
        reply->deleteLater();
        if (context != m_context || token != m_operationToken || queryToken != m_queryToken) return;
        m_queryBusy = false;
        if (reply->error() == QNetworkReply::NoError)
            acceptReceipt(QJsonDocument::fromJson(reply->readAll()).object());
        else {
            const int code = reply->attribute(QNetworkRequest::HttpStatusCodeAttribute).toInt();
            m_operation.queryError = QString("回执查询未成功：HTTP %1 / %2；不会重新发布").arg(code).arg(reply->errorString());
        }
        evaluateOperation();
    });
}

void GsClient::operationNotice(const QString &message) {
    m_operation.lastMessage = message;
    auto display = m_operation.receipt;
    display.insert("execution_fresh", executionFresh());
    display.insert("operation_id", m_operation.request.value("operation_id"));
    display.insert("uid", m_operation.request.value("uid"));
    display.insert("kind", m_operation.request.value("kind"));
    display.insert("message", message);
    const auto &t = m_latestTelemetry;
    if (t.controlFresh && t.taskFresh && t.taskCommandId == m_operation.commandId &&
        t.taskGsBoot == m_operation.gsBoot && t.uid == m_operation.request.value("uid").toString() &&
        t.bootNonce == m_operation.request.value("expected_rp_boot_nonce").toString()) {
        display.insert("waypoint_index", t.waypointIndex);
        display.insert("waypoint_count", t.waypointCount);
    }
    // Do not use source age or poll count as a log key: only state changes matter.
    QJsonObject logState{{"message", message}, {"command_id", double(m_operation.commandId)},
                         {"stage", display.value("stage")}, {"status", display.value("status")},
                         {"reason", display.value("reason")}, {"execution_fresh", display.value("execution_fresh")},
                         {"execution_state", display.value("execution_state")}, {"execution_reason", display.value("execution_reason")},
                         {"waypoint_index", display.value("waypoint_index")}};
    const QString key = QString::fromUtf8(QJsonDocument(logState).toJson(QJsonDocument::Compact));
    if (key != m_operation.lastLogKey) {
        m_operation.lastLogKey = key;
        const auto &req = m_operation.request;
        QString details = QString("operation_id=%1 uid=%2 kind=%3 command_id=%4 boot=%5")
            .arg(req.value("operation_id").toString(), req.value("uid").toString(), req.value("kind").toString(),
                 QString::number(m_operation.commandId), req.value("expected_rp_boot_nonce").toString());
        if (req.value("kind").toString() == "control_mode")
            details += QString(" owner=%1 work=%2 expected_revision=%3").arg(req.value("mode_owner").toInt())
                .arg(req.value("mode_state").toInt()).arg(number(req, "expected_control_revision"));
        if (!m_operation.receipt.isEmpty())
            details += QString(" stage=%1 status=%2 reason=%3 execution=%4/%5")
                .arg(display.value("stage").toString(), display.value("status").toString(), display.value("reason").toString("NONE"),
                     display.value("execution_state").toString(), display.value("execution_reason").toString());
        emit missionProgress(message + " [" + details + "]");
    }
    emit operationUpdated(display);
}

bool GsClient::executionFresh() const {
    return freshReceipt(m_operation.receipt) && m_clock.elapsed() - m_operation.lastFreshAt +
           m_operation.receipt.value("execution_age_ms").toDouble() <= 5000;
}

void GsClient::evaluateOperation() {
    if (m_operation.request.isEmpty()) return;
    const auto r = m_operation.receipt, req = m_operation.request;
    const QString stage = r.value("stage").toString(), state = r.value("execution_state").toString();
    const QString reason = r.value("execution_reason").toString(), kind = req.value("kind").toString();
    const auto &t = m_latestTelemetry;
    if (!m_operation.queryError.isEmpty()) { operationNotice(m_operation.queryError); return; }
    if (stage == "REJECTED" || stage == "CANCELLED") {
        finishOperation(false, "任务未接受：" + r.value("status").toString() + " / " + r.value("reason").toString()); return;
    }
    if (executionFresh()) {
        if (kind == "control_mode") {
            if (state == "MODE_APPLIED" && t.controlFresh && t.uid == req.value("uid").toString() &&
                t.bootNonce == req.value("expected_rp_boot_nonce").toString() &&
                t.controlRevision == number(req, "expected_control_revision") + 1 &&
                t.controlOwner == req.value("mode_owner").toInt() && t.controlState == req.value("mode_state").toInt() &&
                t.modeGsBoot == m_operation.gsBoot && t.modeCommandId == m_operation.commandId) {
                finishOperation(true, "模式已在船端生效，可以发布所选任务"); return;
            }
        } else {
            const bool stopped = t.taskFresh && t.taskPhase == state && t.outputsOff && t.taskCommandId == m_operation.commandId &&
                                 t.taskGsBoot == m_operation.gsBoot && t.uid == req.value("uid").toString() &&
                                 t.bootNonce == req.value("expected_rp_boot_nonce").toString();
            if (kind == "stop" && stopped && (state == "STOPPED" || state == "WAITING_IPC" || state == "IDLE")) {
                finishOperation(true, "船端已停止：执行回报匹配，推进为0且动力/舵输出关闭"); return;
            }
            if (state == "FAILED" || state == "SUPERSEDED") {
                finishOperation(false, executionText(state) + "：" + executionReasonText(reason)); return;
            }
            if (kind != "stop" && reason == "ROUTE_COMPLETE" && stopped && state != "STOPPING") {
                finishOperation(true, "航路点已完成，输出已关闭"); return;
            }
            if ((kind == "turning" || kind == "zigzag" || kind == "identification") && stopped && state != "STOPPING" &&
                (reason == "EXPERIMENT_COMPLETE" || reason == "EXPERIMENT_ABORTED")) {
                finishOperation(reason == "EXPERIMENT_COMPLETE", executionReasonText(reason) + "；船端已确认输出关闭"); return;
            }
            if (kind != "stop" && state == "STOPPED") {
                finishOperation(false, "任务已停止：" + executionReasonText(reason)); return;
            }
            if (stage == "RP_RECEIVED" && kind != "stop") setBusy(false);
            operationNotice(executionText(state) + " · " + executionReasonText(reason));
            return;
        }
    }
    if (stage == "RP_RECEIVED" && kind != "control_mode" && kind != "stop") setBusy(false);
    if (stage == "TIMED_OUT") {
        const QString transportReason = r.value("reason").toString("UNSPECIFIED");
        const QString explanation = transportReason == "gs_radio_delivery_unconfirmed" ? "地面站无线任务未获得匹配的船端回执"
            : transportReason == "local_uart_send_failed" ? "网关向地面站RP发送失败"
            : transportReason == "receipt_deadline" ? "网关等待匹配回执超过时限" : "后端未确认任务交付";
        operationNotice(explanation + "；执行结果未知（" + transportReason + "），将在确认期限内查询原操作");
    }
    else if (kind == "control_mode" && stage == "RP_RECEIVED") operationNotice("船端已收件，等待停止输出及实际模式生效确认");
    else if (stage == "RP_RECEIVED") operationNotice(m_operation.receivedExecution
        ? "执行回报已过期，当前执行结果未知；仅查询原操作" : "船端已收件，尚未获得新鲜执行回报");
    else operationNotice("收件阶段：" + (stage.isEmpty() ? QString("结果待确认") : stage));
}

void GsClient::finishOperation(bool success, const QString &message) {
    m_operationTimer.stop();
    operationNotice(message);
    auto callback = std::move(m_operation.modeCallback);
    clearOperation();
    emit missionResult(success, message);
    if (callback) callback(success, message);
}

} // namespace jlusv
