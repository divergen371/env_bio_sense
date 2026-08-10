#pragma once

#include "core/sensor_types.h"

namespace core {

struct SensorSnapshot {
    EnvironmentData environment {};
    PpgData ppg {};
};

struct SystemStatus {
    DeviceState sht45State {DeviceState::Unknown};
    DeviceState bmp581State {DeviceState::Unknown};
    DeviceState scd41State {DeviceState::Unknown};
    DeviceState max30102State {DeviceState::Unknown};
    DeviceState displayState {DeviceState::Unknown};
    
    uint32_t uptimeMs {};
    uint32_t i2cErrorCount {};
};

} // namespace core
