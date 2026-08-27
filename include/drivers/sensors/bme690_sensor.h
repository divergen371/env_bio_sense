#pragma once

#include "drivers/sensors/sensor_interface.h"
#include "core/sensor_types.h"
#include <bme69x.h>
#include <cstdint>

namespace drivers {
namespace sensors {

class Bme690Sensor : public ISensor {
public:
    Bme690Sensor();
    
    // ISensor 実装
    core::SensorId id() const override { return core::SensorId::Bme690; }
    bool begin() override;
    void update(uint32_t nowMs) override;
    
    core::DeviceState state() const override { return state_; }
    core::ErrorCode lastError() const override { return lastError_; }
    uint32_t lastSuccessMs() const override { return lastSuccessMs_; }
    
    // BME690固有のデータ取得
    bool readData(core::Bme690Data& out) const;

private:
    struct bme69x_dev bmeDev_;
    struct bme69x_conf bmeConf_;
    struct bme69x_heatr_conf heatrConf_;

    core::DeviceState state_ {core::DeviceState::Unknown};
    core::ErrorCode lastError_ {core::ErrorCode::None};
    uint32_t lastSuccessMs_ {0};
    uint32_t errorCount_ {0};
    uint32_t lastReinitMs_ {0};

    core::Bme690Data lastData_ {};
    
    enum class State {
        Idle,
        Waiting
    };
    State machineState_ {State::Idle};
    
    uint32_t measureDelayUs_ {0};
    uint32_t lastMeasureTriggerMs_ {0};
    
    bool initDevice();
    void setError(core::ErrorCode err);
    bool triggerMeasurement();
    void readMeasurement();
};

} // namespace sensors
} // namespace drivers
