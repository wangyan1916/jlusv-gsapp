#include "map_bridge.h"
#include <QGeoCoordinate>
#include <cmath>

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

void MapBridge::setDefaultSpeed(double speed) {
    if (!std::isfinite(speed) || speed < 0 || speed > 3 || speed == m_defaultSpeed) return;
    m_defaultSpeed = speed;
    emit defaultSpeedChanged();
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
    m_groundDisplayLat = m_groundPosition.latDeg;
    m_groundDisplayLon = m_groundPosition.lonDeg;
    if (m_useGcj02 && m_groundPosition.available) {
        const auto display = CoordTransform::wgs84ToGcj02(m_groundPosition.latDeg, m_groundPosition.lonDeg);
        m_groundDisplayLat = display.first;
        m_groundDisplayLon = display.second;
    }
    emit groundStationPositionChanged();
}

void MapBridge::updateGroundStation(const GroundStationPosition &position) {
    m_groundPosition = position;
    updateDisplayPosition();
    updateRelativePosition();
}

void MapBridge::updateRelativePosition() {
    m_relativePosition = {};
    if (m_online && m_groundPosition.valid) {
        // Same WGS-84 spherical distance and true-north bearing as the GS card.
        // GCJ-02 conversion is confined to display coordinates.
        constexpr double rad = 3.14159265358979323846 / 180.0;
        const double a = m_groundPosition.latDeg * rad, b = m_rawLat * rad;
        const double dl = (m_rawLon - m_groundPosition.lonDeg) * rad, dp = b - a;
        const double h = std::pow(std::sin(dp / 2), 2) +
                         std::cos(a) * std::cos(b) * std::pow(std::sin(dl / 2), 2);
        const double distance = 12742000.0 * std::asin(std::sqrt(qBound(0.0, h, 1.0)));
        const double bearing = std::fmod(std::atan2(std::sin(dl) * std::cos(b),
            std::cos(a) * std::sin(b) - std::sin(a) * std::cos(b) * std::cos(dl)) / rad + 360.0, 360.0);
        m_relativePosition = {true, distance, bearing, distance * std::sin(bearing * rad),
                             distance * std::cos(bearing * rad)};
    }
    emit relativePositionChanged();
}

void MapBridge::updateTelemetry(const UsvTelemetry &telemetry) {
    m_rawLat = telemetry.latDeg;
    m_rawLon = telemetry.lonDeg;
    m_heading = telemetry.headingDeg;
    m_headingValid = telemetry.fresh && telemetry.headingValid;
    m_speed = telemetry.sogMps;
    m_online = telemetry.fresh && telemetry.positionValid;

    // The gateway enum is not the NMEA GGA quality code (4/5).
    switch (telemetry.rtkState) {
    case 3:  m_rtkText = "RTK FIXED (固定解)"; break;
    case 2:  m_rtkText = "RTK FLOAT (浮点解)"; break;
    case 1:  m_rtkText = "非 RTK 解"; break;
    default: m_rtkText = "未知 / 未提供"; break;
    }
    if (!telemetry.available) m_rtkText = "未收到导航数据";
    else if (!telemetry.fresh) m_rtkText = "解状态过期";

    m_positionSourceText = telemetry.positionSource.isEmpty() ? "来源未提供" : "未知来源";
    if (telemetry.positionSource == "GNSS") m_positionSourceText = "GNSS";
    else if (telemetry.positionSource == "TRUSTED_RTK") m_positionSourceText = "可信 RTK 位置";
    else if (telemetry.positionSource == "INERTIAL") {
        m_positionSourceText = "惯性推算（非 FIXED）";
        if (telemetry.driftMs >= 0) m_positionSourceText += QString(" · %1 s").arg(telemetry.driftMs / 1000.0, 0, 'f', 1);
    }
    if (!m_online) m_positionSourceText = "无有效位置 · " + m_positionSourceText;

    m_headingSourceText = "来源未提供";
    if (telemetry.headingSource == "GNSS") m_headingSourceText = "GPS 对地航迹向 COG";
    else if (telemetry.headingSource == "CALIBRATED_MAGNETOMETER") m_headingSourceText = "磁罗盘艏向";
    if (telemetry.headingReference == "TRUE") m_headingSourceText += "（真北）";
    else if (telemetry.headingReference == "MAGNETIC") m_headingSourceText += "（磁北）";

    updateDisplayPosition();
    updateRelativePosition();
    emit boatHeadingChanged();
    emit boatTelemetryChanged();
}

void MapBridge::addWaypointFromMap(double lat, double lon, double speed) {
    if (m_waypoints.size() >= 16) return;
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
