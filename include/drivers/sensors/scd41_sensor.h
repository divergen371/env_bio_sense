#pragma once

#include "drivers/sensors/sensor_interface.h"
#include <SensirionI2CScd4x.h>
#include <atomic>

namespace drivers {
namespace sensors {

struct Scd41FrcResult {
    bool success {false};
    bool restartSuccess {false};
    uint16_t referencePpm {0};
    uint16_t preCalibrationCo2Ppm {0};
    int16_t correctionPpm {0};
    uint16_t rawWord {0};
    uint16_t ambientPressureHpa {0};
    uint32_t pressureAgeMs {UINT32_MAX};
    uint32_t measurementUptimeMs {0};
    const char* errorMessage {nullptr};
};

enum class Scd41Condition : uint8_t {
    AwaitingFirstSample = 0,
    Healthy,
    LockTimeout,
    DataReadyError,
    DataNotReadyTimeout,
    ReadError,
    DriverError,
    RecoveryStopping,
    RecoveryWaiting,
    RecoveryFailed,
    Stabilizing
};

struct Scd41Health {
    Scd41Condition condition {Scd41Condition::AwaitingFirstSample};
    core::DeviceState state {core::DeviceState::Unknown};
    uint32_t lastSuccessMs {0};
    uint32_t ageMs {UINT32_MAX};
    uint16_t rawError {0};
    uint32_t consecutiveErrors {0};
};

class Scd41Sensor : public IEnvironmentSensor {
public:
    Scd41Sensor();

    // ISensor 実装
    core::SensorId id() const override { return core::SensorId::Scd41; }
    bool begin() override;

    // Perform Forced Recalibration (FRC) using an external reference CO2 value.
    // The sensor must have been operating in periodic measurement mode for >3 mins.
    bool performForcedRecalibration(uint16_t referenceCo2Ppm, Scd41FrcResult& result);
    
    // Perform factory reset and reapply necessary configurations
    bool factoryResetAndReconfigure();

    void update(uint32_t nowMs) override;
    core::DeviceState state() const override { return state_; }
    core::ErrorCode lastError() const override { return lastError_; }
    uint32_t lastSuccessMs() const override { return lastSuccessMs_; }
    Scd41Health health(uint32_t nowMs) const;

    // IEnvironmentSensor 実装
    bool readEnvironment(core::EnvironmentData& out) const override;

    // 気圧補正用（CO2濃度計算の高精度化）
    // 引数は hPa (SCD41の内部的には Pa/100) であり、海面更正気圧ではなく現地気圧を渡すこと
    void setAmbientPressure(uint16_t ambientPressureHpa);

private:
    enum class RecoveryPhase : uint8_t {
        Idle,
        WaitAfterStop
    };

    static constexpr uint32_t DATA_STALE_MS = 15000;
    static constexpr uint32_t STOP_TO_START_DELAY_MS = 500;

    bool beginRecovery(uint32_t nowMs);
    void continueRecovery(uint32_t nowMs);
    bool reinitializeAfterStop(uint16_t& rawError);
    void enterRecoveryQuarantine(uint32_t nowMs);
    void markRecoveryFailure(uint32_t nowMs, Scd41Condition condition,
                             core::ErrorCode errorCode, uint16_t rawError);

    SensirionI2CScd4x scd4x_;
    
    core::DeviceState state_ {core::DeviceState::Unknown};
    core::ErrorCode lastError_ {core::ErrorCode::None};
    uint32_t lastSuccessMs_ {0};
    
    uint8_t postFrcLogCount_ {0};
    
    uint16_t currentCo2Ppm_ {0};
    float currentTemperature_ {0.0f};
    float currentHumidity_ {0.0f};
    
    bool hasValidData_ {false};
    
    // 統計・診断用カウンタ
    uint32_t successCount_ {0};
    uint32_t readErrorCount_ {0};
    uint32_t notReadyCount_ {0};
    uint32_t consecutiveErrors_ {0};
    Scd41Condition condition_ {Scd41Condition::AwaitingFirstSample};
    uint16_t lastRawError_ {0};

    RecoveryPhase recoveryPhase_ {RecoveryPhase::Idle};
    uint32_t recoveryDeadlineMs_ {0};
    uint32_t nextRecoveryAttemptMs_ {0};
    uint8_t recoveryAttemptsSinceStable_ {0};
    bool recoveryUsesReinit_ {false};
    bool recoveryQuarantine_ {false};
    uint8_t recoveryGoodSamples_ {0};
    uint32_t stableRunStartMs_ {0};
    
    // FRC および 状態管理用
    std::atomic<bool> maintenanceInProgress_ {false};
    uint32_t measurementStartMs_ {0};
    uint16_t lastAmbientPressureHpa_ {0};
    uint32_t lastAmbientPressureInputMs_ {0};
    uint32_t lastAmbientPressureCommandMs_ {0};
    bool hasAmbientPressure_ {false};
};

} // namespace sensors
} // namespace drivers
