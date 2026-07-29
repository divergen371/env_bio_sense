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
    void update(uint32_t nowMs) override;
    core::DeviceState state() const override { return state_; }
    core::ErrorCode lastError() const override { return lastError_; }
    uint32_t lastSuccessMs() const override { return lastSuccessMs_; }

    // IEnvironmentSensor 実装
    bool readEnvironment(core::EnvironmentData& out) const override;

private:
    SensirionI2CScd4x scd4x_;
    
    core::DeviceState state_ {core::DeviceState::Unknown};
    core::ErrorCode lastError_ {core::ErrorCode::None};
    uint32_t lastSuccessMs_ {0};
    
    uint16_t currentCo2Ppm_ {0};
    float currentTemperature_ {0.0f};
    float currentHumidity_ {0.0f};
    
    bool hasValidData_ {false};
};

} // namespace sensors
} // namespace drivers
