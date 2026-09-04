#pragma once

#include <cstdint>

namespace core {

enum class SensorId : uint8_t {
    Sht45,
    Bmp581,
    Scd41,
    Max30102,
    Sgp41,
    Gnss,
    Bme690
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

enum class PressureReferenceSource : uint8_t {
    Unset,
    Amedas,
    Gnss,
    Manual,
    Stored
};

struct Bme690Data {
    float temperatureC {};
    float humidityRh {};
    float pressureHpa {};
    float gasResistanceOhm {};
    uint32_t timestampMs {};
    uint8_t gasIndex {};
    uint8_t status {};
    bool tphValid {false};
    bool gasValid {false};
    bool heaterStable {false};
};

struct EnvironmentData {
    float temperatureC {};
    float humidityRh {};
    float pressureHpa {};
    uint16_t co2Ppm {};
    bool co2Valid {false};
    uint32_t co2AgeMs {UINT32_MAX};
    DeviceState scd41State {DeviceState::Unknown};
    float scd41TemperatureC {};
    float scd41HumidityRh {};
    int32_t vocIndex {};
    int32_t noxIndex {};
    float altitudeM {}; // 追加: 高度 (m)
    EnclosureWarning enclosureWarning {EnclosureWarning::Normal};
    uint32_t timestampMs {};
    bool valid {};
    bool temperatureValid {false};
    bool humidityValid {false};
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

struct TimeSnapshot {
    int64_t monotonicUs {};
    int64_t utcEpochUs {};
    uint32_t ppsAgeMs {UINT32_MAX};
    TimeSource source {TimeSource::Unset};
    bool utcValid {false};
    bool disciplined {false};
};

inline bool isDisciplinedTimeSource(TimeSource source) {
    return source == TimeSource::Gnss || source == TimeSource::Ntp;
}

inline TimeSource timeSourceAfterGnssLoss(TimeSource source, bool utcValid) {
    return utcValid && source == TimeSource::Gnss
        ? TimeSource::Holdover : source;
}

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
    uint32_t ppsAgeMs {UINT32_MAX};
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
