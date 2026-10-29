#pragma once

#include <QObject>
#include <QVector>
#include <QGeoCoordinate>
#include <QVariantList>
#include "coord_transform.h"
#include "../network/gs_client.h"

namespace jlusv {

class MapBridge : public QObject {
    Q_OBJECT
    Q_PROPERTY(double boatLat READ boatLat NOTIFY boatPositionChanged)
    Q_PROPERTY(double boatLon READ boatLon NOTIFY boatPositionChanged)
    Q_PROPERTY(double boatHeading READ boatHeading NOTIFY boatHeadingChanged)
    Q_PROPERTY(double boatSpeed READ boatSpeed NOTIFY boatTelemetryChanged)
    Q_PROPERTY(QString rtkStateText READ rtkStateText NOTIFY boatTelemetryChanged)
    Q_PROPERTY(bool isOnline READ isOnline NOTIFY boatTelemetryChanged)
    Q_PROPERTY(bool useGcj02 READ useGcj02 WRITE setUseGcj02 NOTIFY useGcj02Changed)
    Q_PROPERTY(QVariantList waypointsQml READ waypointsQml NOTIFY waypointsChanged)

public:
    explicit MapBridge(QObject *parent = nullptr);

    double boatLat() const { return m_displayLat; }
    double boatLon() const { return m_displayLon; }
    double boatHeading() const { return m_heading; }
    double boatSpeed() const { return m_speed; }
    QString rtkStateText() const { return m_rtkText; }
    bool isOnline() const { return m_online; }
    bool useGcj02() const { return m_useGcj02; }

    void setUseGcj02(bool enable);

    QVariantList waypointsQml() const;
    const QVector<Waypoint>& rawWaypoints() const { return m_waypoints; }

    Q_INVOKABLE void addWaypointFromMap(double lat, double lon, double speed = 1.2);
    Q_INVOKABLE void clearWaypoints();
    Q_INVOKABLE void removeLastWaypoint();

public slots:
    void updateTelemetry(const UsvTelemetry &telemetry);

signals:
    void boatPositionChanged();
    void boatHeadingChanged();
    void boatTelemetryChanged();
    void useGcj02Changed();
    void waypointsChanged();

private:
    double m_rawLat = 0.0;
    double m_rawLon = 0.0;
    double m_displayLat = 0.0;
    double m_displayLon = 0.0;
    double m_heading = 0.0;
    double m_speed = 0.0;
    QString m_rtkText{"未连接"};
    bool m_online = false;
    bool m_useGcj02 = true; // 默认开启 GCJ-02 卫星底图坐标纠偏

    QVector<Waypoint> m_waypoints; // 存储真实 WGS-84 经纬度
    void updateDisplayPosition();
};

} // namespace jlusv
