#pragma once

#include "drivers/sensors/sensor_interface.h"
#include <Adafruit_BMP5xx.h>

namespace drivers {
namespace sensors {

class Bmp585Sensor : public IEnvironmentSensor {
public:
    Bmp585Sensor();
    
    // ISensor 実装
    core::SensorId id() const override { return core::SensorId::Bmp585; }
    bool begin() override;
    void update(uint32_t nowMs) override;
    core::DeviceState state() const override { return state_; }
    core::ErrorCode lastError() const override { return lastError_; }
    uint32_t lastSuccessMs() const override { return lastSuccessMs_; }

    // IEnvironmentSensor 実装
    bool readEnvironment(core::EnvironmentData& out) const override;

private:
    Adafruit_BMP5xx bmp_;
    core::DeviceState state_ {core::DeviceState::Unknown};
    core::ErrorCode lastError_ {core::ErrorCode::None};
    uint32_t lastSuccessMs_ {0};
    
    float currentPressureHpa_ {0.0f};
    bool hasValidData_ {false};
};

} // namespace sensors
} // namespace drivers
