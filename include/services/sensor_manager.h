#pragma once

#include "core/sensor_snapshot.h"
#include <cstdint>

namespace services {

class SensorManager {
public:
    bool begin();
    void update(uint32_t nowMs);

    const core::SensorSnapshot& snapshot() const { return snapshot_; }
    const core::SystemStatus& status() const { return status_; }

private:
    core::SensorSnapshot snapshot_ {};
    core::SystemStatus status_ {};
};

} // namespace services
