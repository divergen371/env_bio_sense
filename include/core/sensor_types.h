#pragma once

#include <cstdint>

namespace core {

enum class SensorId : uint8_t {
    Sht45,
    Bmp581,
    Scd41,
    Max30102,
    Sgp41,
    Gnss
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

enum class PressureFieldState : uint8_t {
    Valid,
    LastKnown,
    StaticFallback,
    Invalid
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
    float altitudeM {}; // 追加: 高度 (m)
    EnclosureWarning enclosureWarning {EnclosureWarning::Normal};
    uint32_t timestampMs {};
    bool valid {};
    bool pressureValid {false};
    bool pressureStale {false};
    bool sgp41Valid {false};
    bool altitudeValid {false}; // 追加: 高度フラグ
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

enum class TimeSource : uint8_t {
    Unset,
    Manual,
    Ntp,
    Gnss,
    Holdover
};

struct GnssData {
    double latitudeDeg {};
    double longitudeDeg {};

    float altitudeMslM {};
    float speedMps {};
    float courseDeg {};
    float hdop {};
    uint16_t satellites {};

    bool fixValid {};
    bool altitudeValid {};
    bool speedValid {};
    bool courseValid {};
    bool hdopValid {};
    bool timeValid {};

    int64_t sampleMonotonicUs {};
    int64_t utcEpochMs {};
    int64_t lastPpsMonotonicUs {}; // NMEA到着時点での直近のPPS時刻
    uint32_t ageMs {UINT32_MAX};
};

struct GnssStatus {
    DeviceState transportState {DeviceState::Unknown};

    bool nmeaAlive {};
    bool ppsSeen {};
    bool ppsRecent {};
    bool timeDisciplined {};

    uint32_t nmeaAgeMs {UINT32_MAX};
    uint32_t fixAgeMs {UINT32_MAX};
    uint32_t ppsAgeMs {UINT32_MAX};

    uint32_t ppsCount {};
    int32_t lastPpsIntervalUs {};
    uint32_t uartBaud {};
    uint32_t checksumFailures {};
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
