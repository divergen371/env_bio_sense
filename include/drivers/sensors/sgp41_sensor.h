#pragma once

#include "drivers/sensors/sensor_interface.h"
#include <SensirionI2CSgp41.h>
#include <VOCGasIndexAlgorithm.h>
#include <NOxGasIndexAlgorithm.h>

namespace drivers {
namespace sensors {

class Sgp41Sensor : public IEnvironmentSensor {
public:
    Sgp41Sensor();

    // ISensor 実装
    core::SensorId id() const override { return core::SensorId::Sgp41; }
    bool begin() override;
    void update(uint32_t nowMs) override;
    core::DeviceState state() const override { return state_; }
    core::ErrorCode lastError() const override { return lastError_; }
    uint32_t lastSuccessMs() const override { return lastSuccessMs_; }

    // IEnvironmentSensor 実装
    bool readEnvironment(core::EnvironmentData& out) const override;

    // 補正用データの注入
    void setCompensation(float temperatureC, float humidityRh);

private:
    SensirionI2CSgp41 sgp41_;
    VOCGasIndexAlgorithm vocAlgorithm_;
    NOxGasIndexAlgorithm noxAlgorithm_;
    
    core::DeviceState state_ {core::DeviceState::Unknown};
    core::ErrorCode lastError_ {core::ErrorCode::None};
    uint32_t lastSuccessMs_ {0};
    
    // Compensation defaults
    uint16_t compensationRh_ {0x8000}; // Default 50%
    uint16_t compensationT_ {0x6666};  // Default 25C

    // Raw signals
    uint16_t srawVoc_ {0};
    uint16_t srawNox_ {0};

    // Processed indexes
    int32_t vocIndex_ {0};
    int32_t noxIndex_ {0};

    bool hasValidData_ {false};
    uint32_t startMs_ {0};
    
    // Diagnostic tracking
    uint32_t successCount_ {0};
    uint32_t readErrorCount_ {0};
    uint32_t consecutiveErrors_ {0};
};

} // namespace sensors
} // namespace drivers
