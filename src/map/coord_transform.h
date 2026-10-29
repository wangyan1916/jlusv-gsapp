#pragma once

#include <cmath>
#include <utility>

namespace jlusv {

/**
 * @brief WGS-84 与 GCJ-02 (火星坐标系) 互相转换算法
 * 用于消除吉林一号/高德卫星瓦片在中国境内的非线性偏移
 */
class CoordTransform {
public:
    static bool isOutOfChina(double lat, double lon);

    // WGS-84 经纬度转 GCJ-02 经纬度 (用于在加密卫星底图上准确定位船只)
    static std::pair<double, double> wgs84ToGcj02(double lat, double lon);

    // GCJ-02 经纬度转 WGS-84 经纬度 (用于在加密地图上点点后，还原真实地理坐标下发给船只)
    static std::pair<double, double> gcj02ToWgs84(double lat, double lon);

private:
    static double transformLat(double x, double y);
    static double transformLon(double x, double y);

    static constexpr double kPi = 3.1415926535897932384626;
    static constexpr double kA  = 6378245.0;
    static constexpr double kEe = 0.00669342162296594323;
};

} // namespace jlusv
