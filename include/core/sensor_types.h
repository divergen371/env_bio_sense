#pragma once

#include <cstdint>

namespace core {

enum class SensorId : uint8_t {
    Sht45,
    Bmp581,
    Scd41,
    Max30102,
    Sgp41
};

enum class DeviceState : uint8_t {
    Unknown,
    Initializing,
    Ready,
    Degraded,
    Warning,
    Offline,
    RetryWait,
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

enum class EnclosureWarning : uint8_t {
    Normal,
    HeatTrapped
};

struct EnvironmentData {
    float temperatureC {};
    float humidityRh {};
    float pressureHpa {};
    uint16_t co2Ppm {};
    float scd41TemperatureC {};
    float scd41HumidityRh {};
    int32_t vocIndex {};
    int32_t noxIndex {};
    EnclosureWarning enclosureWarning {EnclosureWarning::Normal};
    uint32_t timestampMs {};
    bool valid {};
    bool pressureValid {false};
    bool pressureStale {false};
    bool sgp41Valid {false};
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
    float dptHeartRateBpm {};   // DPT（周波数領域）による心拍推定値
    float dptSpo2Percent {};    // DPTによるSpO2推定値
    float perfusionIndex {};    // 灌流指数 PI (%)
    uint32_t timestampMs {};
    bool rawValid {};
    bool calculatedValid {};
    bool signalPoor {false}; // アプローチA: 波形品質フラグ
    uint32_t signalAmplitude {0};
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
