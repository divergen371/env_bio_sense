#pragma once

#include "drivers/sensors/sensor_interface.h"
#include <SensirionI2CScd4x.h>

namespace drivers {
namespace sensors {

struct Scd41FrcResult {
    bool success;
    uint16_t referencePpm;
    uint16_t preCalibrationCo2Ppm;
    int16_t correctionPpm;
    uint16_t rawWord;
    uint16_t ambientPressureHpa;
    uint32_t measurementUptimeMs;
    const char* errorMessage;
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

    // IEnvironmentSensor 実装
    bool readEnvironment(core::EnvironmentData& out) const override;

    // 気圧補正用（CO2濃度計算の高精度化）
    // 引数は hPa (SCD41の内部的には Pa/100) であり、海面更正気圧ではなく現地気圧を渡すこと
    void setAmbientPressure(uint16_t ambientPressureHpa);

private:
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
    
    // FRC および 状態管理用
    bool calibrationInProgress_ {false};
    uint32_t measurementStartMs_ {0};
    uint32_t lastAmbientPressureHpa_ {0};
    uint32_t lastAmbientPressureSetMs_ {0};
    bool hasAmbientPressure_ {false};
};

} // namespace sensors
} // namespace drivers
