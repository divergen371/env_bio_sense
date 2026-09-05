#include "services/location_service.h"
#include "services/logger.h"
#include "config/secrets.h"
#include "utils/location_policy.h"
#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstdlib>

#ifndef LOCATION_FALLBACK_ENABLED
// An old coordinate must never become active merely because an older private
// secrets.h predates the explicit opt-in flag.
#define LOCATION_FALLBACK_ENABLED 0
#endif

namespace services {
namespace {

bool parseCoordinate(const char* text, double& value) {
    if (text == nullptr || *text == '\0') return false;
    errno = 0;
    char* end = nullptr;
    const double parsed = std::strtod(text, &end);
    if (errno != 0 || end == text || !std::isfinite(parsed)) return false;
    while (*end != '\0' && std::isspace(static_cast<unsigned char>(*end))) ++end;
    if (*end != '\0') return false;
    value = parsed;
    return true;
}

} // namespace

void LocationService::begin() {
    core::DeviceLocation configured;
#if LOCATION_FALLBACK_ENABLED
    double latitude = 0.0;
    double longitude = 0.0;
    if (parseCoordinate(LOCATION_LAT, latitude) &&
        parseCoordinate(LOCATION_LON, longitude) &&
        utils::location_policy::coordinateValid(latitude, longitude)) {
        configured.latitudeDeg = latitude;
        configured.longitudeDeg = longitude;
        configured.source = core::LocationSource::ConfiguredFallback;
        configured.ageMs = UINT32_MAX;
        configured.valid = true;
    }
#endif

    portENTER_CRITICAL(&mux_);
    selector_.reset(configured);
    lastLoggedSource_ = configured.valid
        ? core::LocationSource::ConfiguredFallback
        : core::LocationSource::Unavailable;
    portEXIT_CRITICAL(&mux_);

    if (configured.valid) {
        Logger::info("Location", "Location source: CONFIGURED_FALLBACK");
    } else {
        Logger::warn("Location", "Location source: UNAVAILABLE");
    }
}

void LocationService::update(const core::GnssData& gnss, uint32_t nowMs) {
    portENTER_CRITICAL(&mux_);
    const core::LocationSource before = selector_.current(nowMs).source;
    selector_.update(gnss, nowMs);
    const core::DeviceLocation selected = selector_.current(nowMs);
    portEXIT_CRITICAL(&mux_);

    bool sourceChanged = false;
    portENTER_CRITICAL(&mux_);
    if (selected.source != lastLoggedSource_) {
        lastLoggedSource_ = selected.source;
        sourceChanged = true;
    }
    portEXIT_CRITICAL(&mux_);
    if (sourceChanged || selected.source != before) {
        Logger::info("Location", "Location source changed: %s -> %s",
            core::locationSourceName(before),
            core::locationSourceName(selected.source));
    }
}

core::DeviceLocation LocationService::current(uint32_t nowMs) const {
    portENTER_CRITICAL(&mux_);
    const core::DeviceLocation selected = selector_.current(nowMs);
    portEXIT_CRITICAL(&mux_);
    return selected;
}

double LocationService::distanceMeters(double lat1Deg, double lon1Deg,
                                       double lat2Deg, double lon2Deg) {
    return utils::location_policy::distanceMeters(
        lat1Deg, lon1Deg, lat2Deg, lon2Deg);
}

} // namespace services
