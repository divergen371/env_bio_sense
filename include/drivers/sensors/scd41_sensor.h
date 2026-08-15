#pragma once

#include "drivers/sensors/sensor_interface.h"
#include <SensirionI2CScd4x.h>

namespace drivers {
namespace sensors {

class Scd41Sensor : public IEnvironmentSensor {
public:
    Scd41Sensor();

    // ISensor 実装
    core::SensorId id() const override { return core::SensorId::Scd41; }
    bool begin() override;

    // Perform manual calibration (FRC). Note: This is a blocking call (~500ms).
    // The sensor must have been operating in periodic measurement mode for >3 mins.
    bool performManualCalibration(uint16_t targetCo2Ppm, uint16_t& frcCorrection);

    void update(uint32_t nowMs) override;
    core::DeviceState state() const override { return state_; }
    core::ErrorCode lastError() const override { return lastError_; }
    uint32_t lastSuccessMs() const override { return lastSuccessMs_; }

    // IEnvironmentSensor 実装
    bool readEnvironment(core::EnvironmentData& out) const override;

    // 気圧補正用（CO2濃度計算の高精度化）
    void setAmbientPressure(uint16_t pressureHpa);

private:
    SensirionI2CScd4x scd4x_;
    
    core::DeviceState state_ {core::DeviceState::Unknown};
    core::ErrorCode lastError_ {core::ErrorCode::None};
    uint32_t lastSuccessMs_ {0};
    
    uint16_t currentCo2Ppm_ {0};
    float currentTemperature_ {0.0f};
    float currentHumidity_ {0.0f};
    
    bool hasValidData_ {false};
    
    // 統計・診断用カウンタ
    uint32_t successCount_ {0};
    uint32_t readErrorCount_ {0};
    uint32_t notReadyCount_ {0};
    uint32_t consecutiveErrors_ {0};
};

} // namespace sensors
} // namespace drivers
