#include <QtTest>
#include <QTcpServer>
#include <QTcpSocket>
#include <QJsonDocument>
#include <QPointer>
#include "network/gs_client.h"
#include "map/map_bridge.h"
#include <cmath>
#include <limits>

using namespace jlusv;

// An ephemeral loopback HTTP server: no device address, serial port or real login.
class Gateway : public QObject {
public:
    QTcpServer server;
    QString uid{"001122334455"}, boot{"0000000000001234"}, gsUid{"AABBCCDDEEFF"};
    int owner = 1, work = 3, revision = 1, modeId = 1, taskId = 0;
    int index = 0, count = 0, queries = 0, overviewCode = 200, postCode = 202, queryCode = 200, delayPost = 0;
    bool navFresh = true, outputsOn = false, hideTarget = false, wrongReceipt = false;
    bool execFresh = false, headersOk = true;
    QString phase{"UNKNOWN"}, reason{"NONE"}, stage{"RP_RECEIVED"};
    QString transportReason;
    QJsonObject navigationOverrides;
    QJsonObject receiverSnapshot{{"available", true}, {"gnss", QJsonObject{
        {"position_available", true}, {"position_valid", true}, {"position_source", "RTCM1005"},
        {"latitude", 43.82530}, {"longitude", 125.28800}, {"base_position_age_ms", 0}, {"status_age_ms", 0}}}};
    int receiverCode = 200, receiverQueries = 0, delayReceiver = 0;
    bool receiverReadOnly = true;
    QVector<QJsonObject> posts;
    QHash<QString, QJsonObject> receipts;

    Gateway() {
        if (!server.listen(QHostAddress::LocalHost, 0)) qFatal("loopback listen failed");
        connect(&server, &QTcpServer::newConnection, this, [this]() {
            while (server.hasPendingConnections()) {
                auto *socket = server.nextPendingConnection();
                connect(socket, &QTcpSocket::disconnected, socket, &QObject::deleteLater);
                connect(socket, &QTcpSocket::readyRead, this, [this, socket]() {
                    auto bytes = socket->property("input").toByteArray() + socket->readAll();
                    socket->setProperty("input", bytes);
                    const int end = bytes.indexOf("\r\n\r\n");
                    if (end < 0 || socket->property("handled").toBool()) return;
                    const auto lines = bytes.left(end).split('\n');
                    int length = 0;
                    for (const auto &line : lines) if (line.toLower().startsWith("content-length:"))
                        length = line.mid(15).trimmed().toInt();
                    if (bytes.size() < end + 4 + length) return;
                    socket->setProperty("handled", true);
                    const auto parts = lines.first().trimmed().split(' ');
                    const auto body = QJsonDocument::fromJson(bytes.mid(end + 4, length)).object();
                    handle(socket, parts[0], parts[1], bytes.left(end), body);
                });
            }
        });
    }
    QString url() const { return "http://127.0.0.1:" + QString::number(server.serverPort()); }
    QJsonObject member(const QString &id) const {
        QJsonObject mode{{"available", true}, {"fresh", navFresh}, {"owner_code", owner}, {"state_code", work},
                         {"revision", revision}, {"gs_boot", 99}, {"command_id", modeId},
                         {"owner", owner == 1 ? "MAIN" : "IPC"}, {"work_state", "WAYPOINTS"},
                         {"phase", phase}, {"reason", reason}, {"waypoint_index", index}, {"waypoint_count", count}};
        QJsonObject task{{"fresh", navFresh}, {"gs_boot", 99}, {"command_id", taskId}, {"state", phase}, {"reason", reason}};
        QJsonObject power{{"valid", true}, {"armed", outputsOn}, {"servo_enabled", outputsOn ? 3 : 0},
                          {"motor", QJsonArray{outputsOn ? 100 : 0, outputsOn ? 100 : 0}}};
        QJsonObject nav{{"available", true}, {"fresh", navFresh}, {"age_ms", 10}, {"position_valid", true},
                       {"lat_deg", 43.82544}, {"lon_deg", 125.28832}, {"sog_mps", 1.0},
                       {"heading_valid", true}, {"heading_deg", 10.0}, {"control_mode", mode},
                       {"ipc_task", task}, {"power", power}};
        for (auto it = navigationOverrides.begin(); it != navigationOverrides.end(); ++it) nav.insert(it.key(), it.value());
        return {{"uid", id}, {"name", id}, {"registered", true}, {"state", "JOINED"},
                {"device_online", true}, {"task_ready", true}, {"task_rp_boot_nonce", boot}, {"navigation", nav}};
    }
    void send(QTcpSocket *socket, int code, const QJsonObject &obj, bool cookie = false) {
        const QByteArray body = QJsonDocument(obj).toJson(QJsonDocument::Compact);
        QByteArray header = "HTTP/1.1 " + QByteArray::number(code) + " Test\r\nContent-Type: application/json\r\n";
        if (cookie) header += "Set-Cookie: session=loopback-test; Path=/; HttpOnly\r\n";
        header += "Content-Length: " + QByteArray::number(body.size()) + "\r\nConnection: close\r\n\r\n";
        socket->write(header + body);
        socket->disconnectFromHost();
    }
    void handle(QTcpSocket *socket, const QByteArray &method, const QByteArray &path,
                const QByteArray &headers, const QJsonObject &body) {
        if (path == "/api/v1/auth/login") { send(socket, 200, {{"authenticated", true}}, true); return; }
        if (path == "/api/v1/fleet/overview") {
            QJsonArray members;
            if (!hideTarget) members.append(member(uid));
            members.append(member("112233445566"));
            send(socket, overviewCode, {{"gs_uid", gsUid}, {"members", members}}); return;
        }
        if (path == "/api/v1/gs/gnss/receiver") {
            ++receiverQueries;
            receiverReadOnly = receiverReadOnly && method == "GET" && !headers.contains("X-GS-Write:");
            if (delayReceiver) {
                QPointer<QTcpSocket> guarded(socket);
                const auto snapshot = receiverSnapshot;
                const int code = receiverCode;
                QTimer::singleShot(delayReceiver, this, [this, guarded, snapshot, code]() {
                    if (guarded) send(guarded, code, snapshot);
                });
            } else send(socket, receiverCode, receiverSnapshot);
            return;
        }
        if (method == "POST" && path == "/api/v1/missions/submit") {
            headersOk = headersOk && headers.contains("X-GS-Write: 1") &&
                        headers.contains("Origin: " + url().toUtf8()) && headers.contains("session=loopback-test");
            posts.append(body);
            const QString op = body.value("operation_id").toString();
            QJsonObject r{{"operation_id", op}, {"uid", body.value("uid")}, {"kind", body.value("kind")},
                          {"command_id", posts.size() + 10}, {"gs_boot", 99}, {"expected_rp_boot", 0x1234}, {"actual_rp_boot", 0x1234}};
            receipts.insert(op, r);
            if (delayPost) {
                QPointer<QTcpSocket> guarded(socket);
                QTimer::singleShot(delayPost, this, [this, guarded, r]() { if (guarded) send(guarded, postCode, decorate(r)); });
            } else send(socket, postCode, decorate(r));
            return;
        }
        if (method == "GET" && path.startsWith("/api/v1/missions/")) {
            ++queries;
            if (queryCode != 200) { send(socket, queryCode, {{"error", "query_unavailable"}}); return; }
            const QString op = QString::fromUtf8(path.mid(int(QByteArray("/api/v1/missions/").size())));
            if (!receipts.contains(op)) { send(socket, 404, {{"error", "not_found"}}); return; }
            auto r = decorate(receipts.value(op));
            if (wrongReceipt) r.insert("uid", "112233445566");
            send(socket, 200, r); return;
        }
        send(socket, 404, {{"error", "not_found"}});
    }
    QJsonObject decorate(QJsonObject r) const {
        r.insert("stage", stage); r.insert("status", "OK");
        if (!transportReason.isEmpty()) r.insert("reason", transportReason);
        r.insert("execution_state", phase); r.insert("execution_reason", reason);
        r.insert("execution_fresh", execFresh);
        r.insert("execution_age_ms", execFresh ? QJsonValue(10) : QJsonValue());
        return r;
    }
    void execution(const QString &state, const QString &why = "NONE") {
        phase = state; reason = why; execFresh = true; taskId = posts.size() + 10;
    }
};

class GsClientTest : public QObject {
    Q_OBJECT
private:
    void connectClient(GsClient &client, Gateway &gateway) {
        client.setBaseUrl(gateway.url()); client.startPolling(100); client.login();
        QTRY_VERIFY_WITH_TIMEOUT(client.canControl(), 3000);
    }
private slots:
    void groundStationReceiverContract_data() {
        QTest::addColumn<QJsonObject>("snapshot");
        QTest::addColumn<bool>("valid");
        Gateway g;
        const auto base = g.receiverSnapshot;
        auto row = [&](const char *name, const char *key, QJsonValue value, bool valid) {
            auto snapshot = base, gnss = base.value("gnss").toObject();
            gnss.insert(key, value); snapshot.insert("gnss", gnss);
            QTest::newRow(name) << snapshot << valid;
        };
        QTest::newRow("rtcm-origin") << base << true;
        row("gga-origin", "position_source", "NMEA_GGA", true);
        row("rtcm-source-expired", "base_position_age_ms", 3000, false);
        auto survey = base, gnss = base.value("gnss").toObject();
        gnss.insert("position_source", "SVIN_RESULT"); gnss.insert("base_position_age_ms", 3500);
        survey.insert("gnss", gnss);
        QTest::newRow("survey-five-second-window") << survey << true;
        gnss.insert("base_position_age_ms", 5000); survey.insert("gnss", gnss);
        QTest::newRow("survey-source-expired") << survey << false;
        row("rp-invalidates-origin", "position_valid", false, false);
        row("status-expired", "status_age_ms", 5000, false);
        row("missing-source-age", "base_position_age_ms", QJsonValue(), false);
        row("null-latitude", "latitude", QJsonValue(), false);
        row("text-longitude", "longitude", "125.288", false);
        row("latitude-out-of-range", "latitude", 91, false);
        auto unavailable = base; unavailable.insert("available", false);
        QTest::newRow("receiver-unavailable") << unavailable << false;
    }
    void groundStationReceiverContract() {
        QFETCH(QJsonObject, snapshot); QFETCH(bool, valid);
        Gateway g; g.receiverSnapshot = snapshot;
        GsClient c; MapBridge map;
        connect(&c, &GsClient::telemetryUpdated, &map, &MapBridge::updateTelemetry);
        connect(&c, &GsClient::groundStationUpdated, &map, &MapBridge::updateGroundStation);
        c.setBaseUrl(g.url()); c.startPolling(100);
        QTRY_VERIFY(!c.groundStationPosition().source.isEmpty());
        QCOMPARE(c.groundStationPosition().valid, valid);
        QCOMPARE(map.groundStationValid(), valid);
        QCOMPARE(map.relativePosition().valid, valid);
        QVERIFY(map.isOnline()); // GS origin validity never changes vessel position validity.
        QVERIFY(g.receiverReadOnly); QVERIFY(g.posts.isEmpty());
    }
    void groundStationFailureDoesNotBlockVesselControl() {
        Gateway g; g.receiverCode = 404;
        GsClient c; MapBridge map;
        connect(&c, &GsClient::telemetryUpdated, &map, &MapBridge::updateTelemetry);
        connect(&c, &GsClient::groundStationUpdated, &map, &MapBridge::updateGroundStation);
        connectClient(c, g);
        QTRY_VERIFY(g.receiverQueries > 0);
        QVERIFY(c.canPublish(1, 3)); QVERIFY(map.isOnline());
        QVERIFY(!map.groundStationValid()); QVERIFY(!map.relativePosition().valid);
        QVERIFY(g.posts.isEmpty());
    }
    void groundStationExpiresWhileReceiverRequestStalls() {
        Gateway g; GsClient c; MapBridge map;
        connect(&c, &GsClient::telemetryUpdated, &map, &MapBridge::updateTelemetry);
        connect(&c, &GsClient::groundStationUpdated, &map, &MapBridge::updateGroundStation);
        c.setBaseUrl(g.url()); c.startPolling(100);
        QTRY_VERIFY(map.relativePosition().valid);
        g.delayReceiver = 5000;
        QTRY_VERIFY_WITH_TIMEOUT(!map.groundStationValid(), 3500);
        QVERIFY(map.isOnline()); QVERIFY(!map.relativePosition().valid);
        QVERIFY(c.groundStationPosition().old);
        c.stopPolling();
        QVERIFY(!map.groundStationValid()); QVERIFY(!map.isOnline());
    }
    void groundStationTransitTimeDoesNotRenewSourceAge() {
        Gateway g; GsClient c;
        auto gnss = g.receiverSnapshot.value("gnss").toObject();
        gnss.insert("base_position_age_ms", 2700); g.receiverSnapshot.insert("gnss", gnss);
        g.delayReceiver = 450;
        c.setBaseUrl(g.url()); c.startPolling(100);
        QTRY_VERIFY(!c.groundStationPosition().source.isEmpty());
        QVERIFY(c.groundStationPosition().ageMs >= 3000);
        QVERIFY(!c.groundStationPosition().valid);
    }
    void lateGroundStationReplyCannotCrossGateways() {
        Gateway oldGateway, newGateway;
        oldGateway.delayReceiver = 900; newGateway.gsUid = "FFEEDDCCBBAA";
        auto gnss = newGateway.receiverSnapshot.value("gnss").toObject();
        gnss.insert("latitude", 10); gnss.insert("longitude", 20);
        newGateway.receiverSnapshot.insert("gnss", gnss);
        GsClient c; MapBridge map;
        connect(&c, &GsClient::groundStationUpdated, &map, &MapBridge::updateGroundStation);
        c.setBaseUrl(oldGateway.url()); c.startPolling(100);
        QTRY_VERIFY(oldGateway.receiverQueries > 0);
        c.setBaseUrl(newGateway.url());
        QVERIFY(!map.groundStationValid());
        QTRY_VERIFY(map.groundStationValid());
        QTest::qWait(1000);
        QCOMPARE(c.groundStationPosition().latDeg, 10.0);
        QCOMPARE(c.groundStationPosition().lonDeg, 20.0);
        c.stopPolling(); QVERIFY(!map.groundStationValid());
    }
    void relativePositionUsesTrueNorthAndSignedMetres_data() {
        QTest::addColumn<double>("baseLon"); QTest::addColumn<double>("lat");
        QTest::addColumn<double>("lon"); QTest::addColumn<double>("distance");
        QTest::addColumn<double>("bearing");
        QTest::newRow("north") << 0.0 << 1.0 << 0.0 << 111194.92664455874 << 0.0;
        QTest::newRow("east") << 0.0 << 0.0 << 1.0 << 111194.92664455874 << 90.0;
        QTest::newRow("south") << 0.0 << -1.0 << 0.0 << 111194.92664455874 << 180.0;
        QTest::newRow("west") << 0.0 << 0.0 << -1.0 << 111194.92664455874 << 270.0;
        QTest::newRow("same-position") << 0.0 << 0.0 << 0.0 << 0.0 << 0.0;
        QTest::newRow("date-line") << 179.999 << 0.0 << -179.999 << 222.389853289 << 90.0;
    }
    void relativePositionUsesTrueNorthAndSignedMetres() {
        QFETCH(double, baseLon); QFETCH(double, lat); QFETCH(double, lon);
        QFETCH(double, distance); QFETCH(double, bearing);
        GroundStationPosition gs; gs.available = gs.valid = true; gs.lonDeg = baseLon;
        UsvTelemetry vessel; vessel.fresh = vessel.positionValid = true;
        vessel.latDeg = lat; vessel.lonDeg = lon;
        MapBridge map; map.setUseGcj02(false);
        map.updateGroundStation(gs); map.updateTelemetry(vessel);
        const auto r = map.relativePosition();
        QVERIFY(map.groundStationValid()); QVERIFY(r.valid);
        QVERIFY(std::abs(r.distanceM - distance) < .00001);
        QVERIFY(std::abs(r.bearingDeg - bearing) < .00001);
        constexpr double rad = 3.14159265358979323846 / 180.0;
        QVERIFY(std::abs(r.eastM - distance * std::sin(bearing * rad)) < .00001);
        QVERIFY(std::abs(r.northM - distance * std::cos(bearing * rad)) < .00001);
        gs.valid = false; map.updateGroundStation(gs);
        QVERIFY(!map.relativePosition().valid); QVERIFY(map.isOnline());
    }
    void mapTransformsBothMarkersWithoutChangingMetres() {
        GroundStationPosition gs; gs.available = gs.valid = true;
        gs.latDeg = 43.8253; gs.lonDeg = 125.2880;
        UsvTelemetry vessel; vessel.fresh = vessel.positionValid = true;
        vessel.latDeg = 43.82544; vessel.lonDeg = 125.28832;
        MapBridge map; map.updateGroundStation(gs); map.updateTelemetry(vessel);
        const auto r = map.relativePosition();
        const auto display = CoordTransform::wgs84ToGcj02(gs.latDeg, gs.lonDeg);
        QCOMPARE(map.groundStationLat(), display.first); QCOMPARE(map.groundStationLon(), display.second);
        QVERIFY(std::abs(map.groundStationLat() - gs.latDeg) > 1e-6);
        map.setUseGcj02(false);
        QCOMPARE(map.groundStationLat(), gs.latDeg); QCOMPARE(map.groundStationLon(), gs.lonDeg);
        QCOMPARE(map.boatLat(), vessel.latDeg); QCOMPARE(map.boatLon(), vessel.lonDeg);
        QCOMPARE(map.relativePosition().distanceM, r.distanceM);
        QCOMPARE(map.relativePosition().eastM, r.eastM); QCOMPARE(map.relativePosition().northM, r.northM);
        vessel.positionValid = false; map.updateTelemetry(vessel);
        QVERIFY(map.groundStationValid()); QVERIFY(!map.relativePosition().valid);
    }

    void receiverSolutionUsesGatewayEnum_data() {
        QTest::addColumn<int>("code");
        QTest::addColumn<bool>("fresh");
        QTest::addColumn<QString>("text");
        QTest::newRow("fixed") << 3 << true << QString("RTK FIXED (固定解)");
        QTest::newRow("float") << 2 << true << QString("RTK FLOAT (浮点解)");
        QTest::newRow("non-rtk") << 1 << true << QString("非 RTK 解");
        QTest::newRow("unknown") << 0 << true << QString("未知 / 未提供");
        QTest::newRow("nmea-4-is-not-gateway-fixed") << 4 << true << QString("未知 / 未提供");
        QTest::newRow("nmea-5-is-not-gateway-float") << 5 << true << QString("未知 / 未提供");
        QTest::newRow("stale-fixed") << 3 << false << QString("解状态过期");
    }
    void receiverSolutionUsesGatewayEnum() {
        QFETCH(int, code); QFETCH(bool, fresh); QFETCH(QString, text);
        Gateway g; GsClient c; MapBridge map;
        g.navFresh = fresh; g.navigationOverrides.insert("rtk_state", code);
        connect(&c, &GsClient::telemetryUpdated, &map, &MapBridge::updateTelemetry);
        c.setBaseUrl(g.url()); c.startPolling(100);
        QTRY_VERIFY(c.latestTelemetry().available);
        QCOMPARE(map.rtkStateText(), text);
        QVERIFY(g.posts.isEmpty());
    }
    void malformedTelemetryDoesNotInventNorthOrCoordinates() {
        Gateway g; GsClient c; MapBridge map;
        g.navigationOverrides = {{"lat_deg", QJsonValue()}, {"lon_deg", "125.2"},
            {"heading_deg", QJsonValue()}, {"cog_deg", QJsonValue()}, {"sog_mps", -1}, {"rtk_state", 2.5}};
        connect(&c, &GsClient::telemetryUpdated, &map, &MapBridge::updateTelemetry);
        connectClient(c, g);
        const auto &t = c.latestTelemetry();
        QVERIFY(t.fresh); QVERIFY(!t.positionValid); QVERIFY(!t.headingValid);
        QVERIFY(!t.cogValid); QVERIFY(!t.speedValid); QCOMPARE(t.rtkState, 0);
        QVERIFY(!map.isOnline()); QVERIFY(!map.boatHeadingValid());
        QCOMPARE(map.positionSourceText(), QString("无有效位置 · 来源未提供"));
        QVERIFY(g.posts.isEmpty());
    }
    void positionAndHeadingExpireIndependently() {
        Gateway g; GsClient c; MapBridge map;
        g.navigationOverrides = {{"nav_age_ms", 6000}, {"heading_age_ms", 10}, {"cog_deg", 90},
            {"heading_source", "CALIBRATED_MAGNETOMETER"}, {"heading_reference", "TRUE"}};
        connect(&c, &GsClient::telemetryUpdated, &map, &MapBridge::updateTelemetry);
        connectClient(c, g);
        QVERIFY(!c.latestTelemetry().positionValid); QVERIFY(!c.latestTelemetry().cogValid);
        QVERIFY(c.latestTelemetry().headingValid); QVERIFY(c.latestTelemetry().controlFresh);
        QVERIFY(map.boatHeadingValid()); QVERIFY(map.headingSourceText().contains("磁罗盘艏向"));
        QVERIFY(c.canPublish(1, 3));
        g.navigationOverrides.insert("nav_age_ms", 10);
        g.navigationOverrides.insert("heading_age_ms", 4000);
        QTRY_VERIFY(c.latestTelemetry().positionValid && !c.latestTelemetry().headingValid);
        QVERIFY(c.latestTelemetry().cogValid); QVERIFY(c.latestTelemetry().controlFresh);
        QVERIFY(map.isOnline()); QVERIFY(!map.boatHeadingValid());
        QVERIFY(c.canPublish(1, 3)); QVERIFY(g.posts.isEmpty());
    }
    void cogSourceZeroDegreesAndOptionalPositionSource() {
        Gateway g; GsClient c; MapBridge map;
        g.navigationOverrides = {{"heading_deg", 0}, {"cog_deg", 0}, {"sog_mps", 0},
            {"heading_source", "GNSS"}, {"heading_reference", "TRUE"}, {"rtk_state", 2},
            {"diff_age_ms", 0}, {"diff_age_state", "VALID"}};
        connect(&c, &GsClient::telemetryUpdated, &map, &MapBridge::updateTelemetry);
        connectClient(c, g);
        QVERIFY(c.latestTelemetry().headingValid); QVERIFY(c.latestTelemetry().cogValid);
        QVERIFY(c.latestTelemetry().speedValid); QCOMPARE(c.latestTelemetry().diffAgeMs, qint64(0));
        QVERIFY(map.boatHeadingValid()); QCOMPARE(map.boatHeading(), 0.0);
        QCOMPARE(map.headingSourceText(), QString("GPS 对地航迹向 COG（真北）"));
        QCOMPARE(map.positionSourceText(), QString("来源未提供"));
        g.navigationOverrides.insert("position_source", "INERTIAL");
        g.navigationOverrides.insert("drift_ms", 1200);
        QTRY_VERIFY(map.positionSourceText().startsWith("惯性推算（非 FIXED）"));
        QCOMPARE(map.rtkStateText(), QString("RTK FLOAT (浮点解)"));
        QVERIFY(map.positionSourceText().contains("1.2 s"));
        g.navigationOverrides.insert("heading_deg", QJsonValue());
        g.navigationOverrides.insert("diff_age_ms", QJsonValue());
        QTRY_VERIFY(!c.latestTelemetry().headingValid && c.latestTelemetry().diffAgeMs < 0);
        QVERIFY(c.latestTelemetry().cogValid); QVERIFY(map.isOnline());
        QVERIFY(!map.boatHeadingValid()); QVERIFY(g.posts.isEmpty());
    }
    void idleDisconnectDoesNotInventConnectionFailure() {
        GsClient c;
        QSignalSpy connection(&c, &GsClient::connectionStatusChanged);
        QSignalSpy auth(&c, &GsClient::authStatusChanged);
        c.stopPolling();
        QCOMPARE(connection.size(), 0); QCOMPARE(auth.size(), 0);
    }

    void timedOutReceiptIsStableAndTrackingStops() {
        Gateway g; g.stage = "TIMED_OUT"; g.transportReason = "gs_radio_delivery_unconfirmed";
        GsClient c; connectClient(c, g);
        QSignalSpy progress(&c, &GsClient::missionProgress), result(&c, &GsClient::missionResult);
        c.switchControlMode(2, 3); QTRY_COMPARE(g.posts.size(), 1);
        QTRY_VERIFY(g.queries >= 2);
        QTest::qWait(5200); // The original bug starts alternating at the 5-second source-age branch.
        int timeoutLines = 0;
        for (const auto &row : progress) {
            const QString text = row[0].toString();
            if (text.contains("stage=TIMED_OUT")) {
                ++timeoutLines;
                QVERIFY(text.contains("gs_radio_delivery_unconfirmed"));
                QVERIFY(text.contains("operation_id="));
            }
            QVERIFY(!text.contains("执行回报过期/不可用"));
        }
        QCOMPARE(timeoutLines, 1); QCOMPARE(result.size(), 0);
        QTRY_COMPARE_WITH_TIMEOUT(result.size(), 1, 17000);
        QVERIFY(!result[0][0].toBool()); QVERIFY(!c.isBusy());
        const int automaticQueries = g.queries;
        QTest::qWait(1100); QCOMPARE(g.queries, automaticQueries);
        c.queryOperation(); QTRY_COMPARE(g.queries, automaticQueries + 1);
        QTest::qWait(1100); QCOMPARE(g.queries, automaticQueries + 1);
        QCOMPARE(g.posts.size(), 1); // Manual refresh also never republishes the command.
    }

    void noExecutionReportIsNotCalledExpired() {
        Gateway g; GsClient c; connectClient(c, g);
        QSignalSpy progress(&c, &GsClient::missionProgress);
        c.submitWaypoints({{43, 125, 1}}); QTRY_VERIFY(g.queries >= 2);
        for (const auto &row : progress) QVERIFY(!row[0].toString().contains("过期"));
    }

    void queryErrorsDoNotAlternateWithOldReceipt() {
        Gateway g; GsClient c; connectClient(c, g);
        QSignalSpy progress(&c, &GsClient::missionProgress);
        c.submitWaypoints({{43, 125, 1}}); QTRY_COMPARE(g.posts.size(), 1);
        g.queryCode = 500;
        QTRY_VERIFY(g.queries >= 2);
        const int lines = progress.size();
        QTest::qWait(1200); QCOMPARE(progress.size(), lines);
        QVERIFY(progress.last()[0].toString().contains("HTTP 500"));
        QCOMPARE(g.posts.size(), 1);
    }

    void lostAckCanStillConfirmActualMode() {
        Gateway g; g.stage = "TIMED_OUT"; g.transportReason = "gs_radio_delivery_unconfirmed";
        GsClient c; connectClient(c, g);
        QSignalSpy result(&c, &GsClient::missionResult);
        c.switchControlMode(2, 3); QTRY_COMPARE(g.posts.size(), 1);
        g.execution("MODE_APPLIED"); g.owner = 2; g.work = 3; g.revision = 2; g.modeId = 11;
        QTRY_COMPARE(result.size(), 1); QVERIFY(result[0][0].toBool());
        QCOMPARE(g.posts.size(), 1);
    }

    void modeRequiresActualApplication_data() {
        QTest::addColumn<int>("owner");
        QTest::newRow("main") << 1; QTest::newRow("ipc") << 2;
    }
    void modeRequiresActualApplication() {
        QFETCH(int, owner);
        Gateway g; g.owner = owner == 1 ? 2 : 1; g.work = 1;
        GsClient c; connectClient(c, g);
        QSignalSpy result(&c, &GsClient::missionResult);
        bool called = false, applied = false;
        c.switchControlMode(owner, 3, [&](bool ok, const QString &) { called = true; applied = ok; });
        QTRY_COMPARE(g.posts.size(), 1);
        QTest::qWait(600);
        QVERIFY(!called); QVERIFY(c.isBusy()); QCOMPARE(result.size(), 0);
        g.execution("MODE_APPLIED"); // An execution string alone cannot confirm the wrong revision/owner.
        c.queryOperation(); QTest::qWait(600); QVERIFY(!called);
        g.owner = owner; g.work = 3; g.revision = 2; g.modeId = 11;
        QTRY_VERIFY_WITH_TIMEOUT(called, 3000);
        QVERIFY(applied); QVERIFY(!c.isBusy()); QCOMPARE(g.posts.size(), 1); QVERIFY(g.headersOk);
        QCOMPARE(g.posts[0].value("expected_control_revision").toInt(), 1);
    }

    void waypointsBothOwners_data() {
        QTest::addColumn<int>("owner");
        QTest::newRow("main") << 1; QTest::newRow("ipc") << 2;
    }
    void waypointsBothOwners() {
        QFETCH(int, owner);
        Gateway g; g.owner = owner;
        GsClient c; connectClient(c, g);
        QSignalSpy result(&c, &GsClient::missionResult);
        QJsonObject last;
        connect(&c, &GsClient::operationUpdated, this, [&](const QJsonObject &op) { last = op; });
        QVector<Waypoint> points;
        for (int i = 0; i < 16; ++i) points.append({43.8254401 + i * .00001, 125.2883202, i * .2});
        c.submitWaypoints(points, 600, owner);
        QTRY_COMPARE(g.posts.size(), 1);
        const auto req = g.posts.first();
        const auto sent = req.value("points").toArray();
        QCOMPARE(req.value("kind").toString(), QString("waypoints"));
        QCOMPARE(req.value("uid").toString(), g.uid);
        QCOMPARE(req.value("expected_rp_boot_nonce").toString(), g.boot);
        QCOMPARE(req.value("ttl_s").toInt(), 600); QCOMPARE(sent.size(), 16);
        for (int i = 0; i < 16; ++i) {
            QCOMPARE(sent[i].toObject().value("lat_deg").toDouble(), points[i].latDeg);
            QCOMPARE(sent[i].toObject().value("speed_mps").toDouble(), points[i].speedMps);
        }
        g.execution("WAITING_IPC", "SENSOR_INVALID"); c.queryOperation();
        QTRY_COMPARE(last.value("execution_state").toString(), QString("WAITING_IPC"));
        QCOMPARE(result.size(), 0);
        g.execution("RUNNING"); g.index = 3; g.count = 16; c.queryOperation();
        QTRY_COMPARE(last.value("execution_state").toString(), QString("RUNNING"));
        QTRY_COMPARE(last.value("waypoint_index").toInt(), 3);
        QCOMPARE(result.size(), 0); QCOMPARE(g.posts.size(), 1);
        g.execution("FAILED", "SENSOR_INVALID"); c.queryOperation();
        QTRY_COMPARE(result.size(), 1); QVERIFY(!result[0][0].toBool());
    }

    void headingTasks_data() {
        QTest::addColumn<int>("owner"); QTest::addColumn<bool>("fixed");
        QTest::newRow("main-speed") << 1 << false; QTest::newRow("main-fixed") << 1 << true;
        QTest::newRow("ipc-speed") << 2 << false; QTest::newRow("ipc-fixed") << 2 << true;
    }
    void headingTasks() {
        QFETCH(int, owner); QFETCH(bool, fixed);
        Gateway g; g.owner = owner; g.work = 2;
        GsClient c; connectClient(c, g);
        if (fixed) c.submitHeadingOnly(owner, 359.99, 0);
        else c.submitHeadingSpeed(owner, 123.45, 1.23);
        QTRY_COMPARE(g.posts.size(), 1);
        const auto req = g.posts.first();
        QCOMPARE(req.value("kind").toString(), fixed ? QString("heading_only") : QString("ipc_heading_speed"));
        QCOMPARE(req.contains("speed_mps"), !fixed);
        QCOMPARE(req.contains("throttle_percent"), fixed);
        QCOMPARE(req.value(fixed ? "throttle_percent" : "speed_mps").toDouble(), fixed ? 0.0 : 1.23);
    }

    void stopNeedsOutputProof_data() {
        QTest::addColumn<int>("owner");
        QTest::newRow("main") << 1; QTest::newRow("ipc") << 2;
    }
    void stopNeedsOutputProof() {
        QFETCH(int, owner);
        Gateway g; g.owner = owner;
        GsClient c; connectClient(c, g);
        QSignalSpy result(&c, &GsClient::missionResult);
        c.submitEmergencyStop(); QTRY_COMPARE(g.posts.size(), 1);
        QTest::qWait(550); QCOMPARE(result.size(), 0); // ACK is never stop proof.
        g.execution("STOPPING", "USER_STOP"); c.queryOperation();
        QTest::qWait(550); QCOMPARE(result.size(), 0);
        g.outputsOn = true; g.execution(owner == 1 ? "STOPPED" : "WAITING_IPC", "USER_STOP");
        c.queryOperation(); QTest::qWait(550); QCOMPARE(result.size(), 0);
        g.outputsOn = false;
        QTRY_COMPARE_WITH_TIMEOUT(result.size(), 1, 3000);
        QVERIFY(result[0][0].toBool()); QCOMPARE(g.posts.size(), 1);
    }

    void invalidAndStaleCannotPublish() {
        Gateway g; GsClient c; connectClient(c, g);
        QSignalSpy result(&c, &GsClient::missionResult);
        c.submitWaypoints({{91, 125, 1}}); c.submitWaypoints({{43, 125, 3.1}});
        c.submitWaypoints({{std::numeric_limits<double>::quiet_NaN(), 125, 1}});
        c.submitWaypoints(QVector<Waypoint>(17)); c.submitWaypoints({{43, 125, 1}}, 0);
        QCOMPARE(result.size(), 5); QCOMPARE(g.posts.size(), 0);
        g.navFresh = false; QTRY_VERIFY(!c.latestTelemetry().controlFresh);
        c.submitWaypoints({{43, 125, 1}}); QCOMPARE(result.size(), 6); QCOMPARE(g.posts.size(), 0);
    }

    void pinsTargetAndInvalidatesReboot() {
        Gateway g; GsClient c; connectClient(c, g);
        g.hideTarget = true;
        QTRY_VERIFY(!c.latestTelemetry().taskReady);
        QCOMPARE(c.latestTelemetry().uid, g.uid); // Never falls through to the other boat.
        g.hideTarget = false; QTRY_VERIFY(c.canControl());
        QSignalSpy result(&c, &GsClient::missionResult);
        c.switchControlMode(2, 3); QTRY_COMPARE(g.posts.size(), 1);
        g.boot = "0000000000005678";
        QTRY_COMPARE(result.size(), 1); QVERIFY(!result[0][0].toBool());
        QTest::qWait(600); QCOMPARE(g.posts.size(), 1);
    }

    void ambiguousPostOnlyQueries() {
        Gateway g; g.postCode = 500;
        GsClient c; connectClient(c, g);
        QSignalSpy result(&c, &GsClient::missionResult);
        c.submitWaypoints({{43, 125, 1}});
        QTRY_COMPARE(g.posts.size(), 1); QTRY_VERIFY(g.queries >= 2);
        QCOMPARE(g.posts.size(), 1); QCOMPARE(result.size(), 0);
        g.wrongReceipt = true; g.execution("RUNNING");
        QTest::qWait(600); QCOMPARE(result.size(), 0); QCOMPARE(g.posts.size(), 1);
    }

    void unauthorizedNeverRelogsOrResends() {
        Gateway g; g.postCode = 401;
        GsClient c; connectClient(c, g);
        QSignalSpy result(&c, &GsClient::missionResult);
        c.submitWaypoints({{43, 125, 1}});
        QTRY_COMPARE(result.size(), 1); QVERIFY(!c.isAuthenticated());
        QTest::qWait(600); QCOMPARE(g.posts.size(), 1); QVERIFY(!result[0][0].toBool());
    }

    void latePostCannotFollowNewTarget() {
        Gateway g; g.delayPost = 1000;
        GsClient c; connectClient(c, g);
        QSignalSpy result(&c, &GsClient::missionResult);
        bool applied = false;
        c.switchControlMode(2, 3, [&](bool ok, const QString &) { applied = ok; });
        QTRY_COMPARE(g.posts.size(), 1);
        c.setTargetUid("112233445566");
        g.execution("MODE_APPLIED"); g.owner = 2; g.revision = 2; g.modeId = 11;
        QTest::qWait(1300);
        QVERIFY(!applied); QCOMPARE(result.size(), 1); QCOMPARE(g.posts.size(), 1);
        QCOMPARE(c.latestTelemetry().uid, QString("112233445566"));
    }

    void stopPreemptsModeOperation() {
        Gateway g; GsClient c; connectClient(c, g);
        QSignalSpy result(&c, &GsClient::missionResult);
        c.switchControlMode(2, 3); QTRY_COMPARE(g.posts.size(), 1);
        QVERIFY(c.isBusy()); c.submitEmergencyStop(); QTRY_COMPARE(g.posts.size(), 2);
        QCOMPARE(g.posts[1].value("kind").toString(), QString("stop"));
        g.execution("STOPPED", "USER_STOP");
        QTRY_COMPARE(result.size(), 1); QVERIFY(result[0][0].toBool());
    }

    void mapPreservesWgsAndPerPointSpeed() {
        MapBridge map;
        map.setRawWaypoints({{43.82544, 125.28832, .4}, {43.8255, 125.2884, 2.1}});
        map.setDefaultSpeed(.7);
        QCOMPARE(map.rawWaypoints()[1].speedMps, 2.1);
        const auto display = CoordTransform::wgs84ToGcj02(43.82544, 125.28832);
        map.addWaypointFromMap(display.first, display.second, map.defaultSpeed());
        const auto wp = map.rawWaypoints().last();
        QVERIFY(std::abs(wp.latDeg - 43.82544) < 1e-7);
        QVERIFY(std::abs(wp.lonDeg - 125.28832) < 1e-7);
        QCOMPARE(wp.speedMps, .7);
        map.setUseGcj02(false);
        QCOMPARE(map.rawWaypoints()[0].latDeg, 43.82544);
        const auto firstDisplay = map.waypointsQml()[0].value<QGeoCoordinate>();
        QCOMPARE(firstDisplay.latitude(), 43.82544);
        map.addWaypointFromMap(43.8256, 125.2885, .9);
        QCOMPARE(map.rawWaypoints().last().lonDeg, 125.2885);
        QCOMPARE(map.rawWaypoints().last().speedMps, .9);
        map.setUseGcj02(true);
        for (int i = 0; i < 20; ++i) map.addWaypointFromMap(display.first, display.second, .7);
        QCOMPARE(map.rawWaypoints().size(), 16);
    }
};

QTEST_GUILESS_MAIN(GsClientTest)
#include "test_gs_client.moc"
