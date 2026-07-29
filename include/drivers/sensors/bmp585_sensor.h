#pragma once

#include "drivers/sensors/sensor_interface.h"
#include <bmp5.h> // Bosch official API

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
    struct bmp5_dev bmp5_dev_;
    struct bmp5_osr_odr_press_config osr_odr_press_cfg_;
    
    core::DeviceState state_ {core::DeviceState::Unknown};
    core::ErrorCode lastError_ {core::ErrorCode::None};
    uint32_t lastSuccessMs_ {0};
    
    float currentPressureHpa_ {0.0f};
    bool hasValidData_ {false};

    // Bosch API用 I2C ラッパー関数群 (静的メソッド)
    static int8_t i2c_read(uint8_t reg_addr, uint8_t *reg_data, uint32_t length, void *intf_ptr);
    static int8_t i2c_write(uint8_t reg_addr, const uint8_t *reg_data, uint32_t length, void *intf_ptr);
    static void delay_us(uint32_t period, void *intf_ptr);
};

} // namespace sensors
} // namespace drivers
