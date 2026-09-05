#pragma once

#include "core/sensor_types.h"
#include <cmath>

namespace utils {
namespace location_policy {

constexpr uint32_t GNSS_LIVE_MAX_AGE_MS = 5000u;
constexpr uint16_t GNSS_MIN_SATELLITES = 4u;
constexpr float GNSS_MAX_HDOP = 5.0f;
constexpr uint8_t CANDIDATE_CONFIRMATIONS = 3u;
constexpr double CANDIDATE_MAX_SPREAD_M = 100.0;
constexpr uint32_t LAST_KNOWN_MAX_AGE_MS = 24u * 60u * 60u * 1000u;
constexpr double STATION_RESELECT_DISTANCE_M = 1000.0;

inline bool coordinateValid(double latitudeDeg, double longitudeDeg) {
    return std::isfinite(latitudeDeg) && std::isfinite(longitudeDeg) &&
           latitudeDeg >= -90.0 && latitudeDeg <= 90.0 &&
           longitudeDeg >= -180.0 && longitudeDeg <= 180.0;
}

inline bool gnssFixAcceptable(const core::GnssData& gnss) {
    return gnss.fixValid &&
           gnss.ageMs <= GNSS_LIVE_MAX_AGE_MS &&
           coordinateValid(gnss.latitudeDeg, gnss.longitudeDeg) &&
           gnss.satellites >= GNSS_MIN_SATELLITES &&
           gnss.hdopValid && std::isfinite(gnss.hdop) &&
           gnss.hdop <= GNSS_MAX_HDOP;
}

inline double distanceMeters(double lat1Deg, double lon1Deg,
                             double lat2Deg, double lon2Deg) {
    if (!coordinateValid(lat1Deg, lon1Deg) ||
        !coordinateValid(lat2Deg, lon2Deg)) {
        return NAN;
    }
    constexpr double DEGREES_TO_RADIANS = 0.017453292519943295;
    constexpr double EARTH_RADIUS_M = 6371000.0;
    const double lat1 = lat1Deg * DEGREES_TO_RADIANS;
    const double lat2 = lat2Deg * DEGREES_TO_RADIANS;
    const double dLat = (lat2Deg - lat1Deg) * DEGREES_TO_RADIANS;
    const double dLon = (lon2Deg - lon1Deg) * DEGREES_TO_RADIANS;
    const double x = dLon * std::cos((lat1 + lat2) * 0.5);
    return EARTH_RADIUS_M * std::sqrt(x * x + dLat * dLat);
}

inline bool shouldReselectStations(double distanceM) {
    return std::isfinite(distanceM) &&
           distanceM >= STATION_RESELECT_DISTANCE_M;
}

} // namespace location_policy
} // namespace utils
