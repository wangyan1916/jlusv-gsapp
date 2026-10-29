#include "coord_transform.h"

namespace jlusv {

bool CoordTransform::isOutOfChina(double lat, double lon) {
    if (lon < 72.004 || lon > 137.8347) return true;
    if (lat < 0.8293 || lat > 55.8271) return true;
    return false;
}

double CoordTransform::transformLat(double x, double y) {
    double ret = -100.0 + 2.0 * x + 3.0 * y + 0.2 * y * y + 0.1 * x * y + 0.2 * std::sqrt(std::abs(x));
    ret += (20.0 * std::sin(6.0 * x * kPi) + 20.0 * std::sin(2.0 * x * kPi)) * (2.0 / 3.0);
    ret += (20.0 * std::sin(y * kPi) + 40.0 * std::sin(y / 3.0 * kPi)) * (2.0 / 3.0);
    ret += (160.0 * std::sin(y / 12.0 * kPi) + 320.0 * std::sin(y * kPi / 30.0)) * (2.0 / 3.0);
    return ret;
}

double CoordTransform::transformLon(double x, double y) {
    double ret = 300.0 + x + 2.0 * y + 0.1 * x * x + 0.1 * x * y + 0.1 * std::sqrt(std::abs(x));
    ret += (20.0 * std::sin(6.0 * x * kPi) + 20.0 * std::sin(2.0 * x * kPi)) * (2.0 / 3.0);
    ret += (20.0 * std::sin(x * kPi) + 40.0 * std::sin(x / 3.0 * kPi)) * (2.0 / 3.0);
    ret += (150.0 * std::sin(x / 12.0 * kPi) + 300.0 * std::sin(x / 30.0 * kPi)) * (2.0 / 3.0);
    return ret;
}

std::pair<double, double> CoordTransform::wgs84ToGcj02(double lat, double lon) {
    if (isOutOfChina(lat, lon)) {
        return {lat, lon};
    }
    double dLat = transformLat(lon - 105.0, lat - 35.0);
    double dLon = transformLon(lon - 105.0, lat - 35.0);
    double radLat = lat / 180.0 * kPi;
    double magic = std::sin(radLat);
    magic = 1.0 - kEe * magic * magic;
    double sqrtMagic = std::sqrt(magic);
    dLat = (dLat * 180.0) / ((kA * (1.0 - kEe)) / (magic * sqrtMagic) * kPi);
    dLon = (dLon * 180.0) / (kA / sqrtMagic * std::cos(radLat) * kPi);
    return {lat + dLat, lon + dLon};
}

std::pair<double, double> CoordTransform::gcj02ToWgs84(double lat, double lon) {
    if (isOutOfChina(lat, lon)) {
        return {lat, lon};
    }
    // 高精度迭代逆转换算法 (毫毫米级精度)
    double wgsLat = lat;
    double wgsLon = lon;
    for (int i = 0; i < 3; ++i) {
        auto [curGcjLat, curGcjLon] = wgs84ToGcj02(wgsLat, wgsLon);
        wgsLat += (lat - curGcjLat);
        wgsLon += (lon - curGcjLon);
    }
    return {wgsLat, wgsLon};
}

} // namespace jlusv
