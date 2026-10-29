#include "map_bridge.h"
#include <QGeoCoordinate>

namespace jlusv {

MapBridge::MapBridge(QObject *parent)
    : QObject(parent)
{
}

void MapBridge::setUseGcj02(bool enable) {
    if (m_useGcj02 != enable) {
        m_useGcj02 = enable;
        updateDisplayPosition();
        emit useGcj02Changed();
        emit waypointsChanged();
    }
}

void MapBridge::updateDisplayPosition() {
    if (m_rawLat == 0.0 && m_rawLon == 0.0) {
        m_displayLat = 0.0;
        m_displayLon = 0.0;
    } else if (m_useGcj02) {
        auto [gcjLat, gcjLon] = CoordTransform::wgs84ToGcj02(m_rawLat, m_rawLon);
        m_displayLat = gcjLat;
        m_displayLon = gcjLon;
    } else {
        m_displayLat = m_rawLat;
        m_displayLon = m_rawLon;
    }
    emit boatPositionChanged();
}

void MapBridge::updateTelemetry(const UsvTelemetry &telemetry) {
    m_rawLat = telemetry.latDeg;
    m_rawLon = telemetry.lonDeg;
    m_heading = telemetry.headingDeg;
    m_speed = telemetry.sogMps;
    m_online = telemetry.fresh;

    switch (telemetry.rtkState) {
    case 4:  m_rtkText = "RTK FIXED (固定解)"; break;
    case 5:  m_rtkText = "RTK FLOAT (浮点解)"; break;
    case 1:  m_rtkText = "单点定位 (GNSS)"; break;
    default: m_rtkText = "无有效解"; break;
    }

    updateDisplayPosition();
    emit boatHeadingChanged();
    emit boatTelemetryChanged();
}

void MapBridge::addWaypointFromMap(double lat, double lon, double speed) {
    Waypoint wp;
    if (m_useGcj02) {
        // 地图上点点的是 GCJ-02 屏幕坐标，还原为真实 WGS-84 坐标
        auto [wgsLat, wgsLon] = CoordTransform::gcj02ToWgs84(lat, lon);
        wp.latDeg = wgsLat;
        wp.lonDeg = wgsLon;
    } else {
        wp.latDeg = lat;
        wp.lonDeg = lon;
    }
    wp.speedMps = speed;
    m_waypoints.append(wp);
    emit waypointsChanged();
}

void MapBridge::setRawWaypoints(const QVector<Waypoint> &waypoints) {
    m_waypoints = waypoints;
    emit waypointsChanged();
}

void MapBridge::addRawWaypoint(double wgsLat, double wgsLon, double speed) {
    Waypoint wp;
    wp.latDeg = wgsLat;
    wp.lonDeg = wgsLon;
    wp.speedMps = speed;
    m_waypoints.append(wp);
    emit waypointsChanged();
}

void MapBridge::clearWaypoints() {
    m_waypoints.clear();
    emit waypointsChanged();
}

void MapBridge::removeLastWaypoint() {
    if (!m_waypoints.isEmpty()) {
        m_waypoints.removeLast();
        emit waypointsChanged();
    }
}

QVariantList MapBridge::waypointsQml() const {
    QVariantList list;
    for (const auto &wp : m_waypoints) {
        double dLat = wp.latDeg;
        double dLon = wp.lonDeg;
        if (m_useGcj02) {
            auto [gcjLat, gcjLon] = CoordTransform::wgs84ToGcj02(wp.latDeg, wp.lonDeg);
            dLat = gcjLat;
            dLon = gcjLon;
        }
        list.append(QVariant::fromValue(QGeoCoordinate(dLat, dLon)));
    }
    return list;
}

} // namespace jlusv
