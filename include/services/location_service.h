#pragma once

#include "core/sensor_types.h"
#include "utils/location_selector.h"
#include <freertos/FreeRTOS.h>

namespace services {

class LocationService {
public:
    void begin();
    void update(const core::GnssData& gnss, uint32_t nowMs);
    core::DeviceLocation current(uint32_t nowMs) const;

    static double distanceMeters(double lat1Deg, double lon1Deg,
                                 double lat2Deg, double lon2Deg);

private:
    mutable portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;
    utils::LocationSelector selector_ {};
    core::LocationSource lastLoggedSource_ {core::LocationSource::Unavailable};
};

} // namespace services
