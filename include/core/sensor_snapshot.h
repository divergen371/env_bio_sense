#pragma once

#include "core/sensor_types.h"

namespace core {

struct SensorHealthSnapshot {
    uint32_t ageMs {UINT32_MAX};
    uint32_t consecutiveErrors {};
    DeviceState state {DeviceState::Unknown};
    ErrorCode error {ErrorCode::None};
};

struct Sgp41Telemetry {
    uint16_t srawVoc {};
    uint16_t srawNox {};
    uint16_t compensationRhTicks {};
    uint16_t compensationTemperatureTicks {};
};

struct AltitudeTelemetry {
    float rawAltitudeM {};
    float displayAltitudeM {};
    float seaLevelPressureHpa {};
    float pressureOffsetHpa {};
    uint32_t seaLevelPressureAgeMs {UINT32_MAX};
    PressureFieldState pressureState {PressureFieldState::Invalid};
    PressureReferenceSource pressureSource {PressureReferenceSource::Unset};
};

struct I2cTelemetry {
    uint32_t lockTimeouts {};
    uint32_t communicationErrors {};
};

struct SensorTelemetry {
    SensorHealthSnapshot sht45 {};
    SensorHealthSnapshot bmp581 {};
    SensorHealthSnapshot scd41 {};
    SensorHealthSnapshot sgp41 {};
    SensorHealthSnapshot bme690 {};
    Sgp41Telemetry sgp41Raw {};
    AltitudeTelemetry altitude {};
    I2cTelemetry i2c {};
    uint16_t scd41RawError {};
};

struct SensorSnapshot {
    EnvironmentData environment {};
    PpgData ppg {};
    GnssData gnss {};
    Bme690Data bme690 {};
    SensorTelemetry telemetry {};
};

struct SystemStatus {
    DeviceState sht45State {DeviceState::Unknown};
    DeviceState bmp581State {DeviceState::Unknown};
    DeviceState scd41State {DeviceState::Unknown};
    DeviceState max30102State {DeviceState::Unknown};
    DeviceState displayState {DeviceState::Unknown};
    DeviceState bme690State {DeviceState::Unknown};
    
    uint32_t uptimeMs {};
    uint32_t i2cErrorCount {};

    GnssStatus gnss {};
    TimeSource timeSource {TimeSource::Unset};
};

} // namespace core
