#pragma once

#include <cstdint>

namespace core {

enum class SensorId : uint8_t {
    Sht45,
    Bmp585,
    Scd41,
    Max30102
};

enum class DeviceState : uint8_t {
    Unknown,
    Initializing,
    Ready,
    Degraded,
    Offline,
    Error
};

enum class ErrorCode : uint8_t {
    None,
    NotFound,
    InitFailed,
    ReadFailed,
    Timeout,
    InvalidData,
    BusError,
    Unsupported
};

struct EnvironmentData {
    float temperatureC {};
    float humidityRh {};
    float pressureHpa {};
    uint16_t co2Ppm {};
    uint32_t timestampMs {};
    bool valid {};
};

enum class PpgState : uint8_t {
    NoFinger,
    Calibrating,
    Measuring
};

struct PpgData {
    PpgState state {PpgState::NoFinger};
    uint32_t red {};
    uint32_t ir {};
    float heartRateBpm {};
    float spo2Percent {};
    uint32_t timestampMs {};
    bool rawValid {};
    bool calculatedValid {};
};

template <typename T>
struct Result {
    T value {};
    ErrorCode error {ErrorCode::None};

    bool ok() const {
        return error == ErrorCode::None;
    }
};

} // namespace core
