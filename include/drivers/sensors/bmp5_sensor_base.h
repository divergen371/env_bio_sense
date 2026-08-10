#pragma once

#include "drivers/sensors/sensor_interface.h"
#include "bmp5.h"

namespace drivers {
namespace sensors {

class Bmp5SensorBase : public IEnvironmentSensor {
public:
    Bmp5SensorBase();
    virtual ~Bmp5SensorBase() = default;

    bool begin() override;
    void update(uint32_t nowMs) override;
    bool readEnvironment(core::EnvironmentData& out) const override;
    
    core::DeviceState state() const override { return state_; }
    core::ErrorCode lastError() const override { return lastError_; }
    uint32_t lastSuccessMs() const override { return lastSuccessMs_; }
    
    // 診断用
    void runDiagnostics();

protected:
    // 派生クラスで実装する固有パラメータ
    virtual uint8_t getI2cAddress() const = 0;
    virtual uint8_t getExpectedChipId() const = 0;
    virtual const char* getSensorName() const = 0;

private:
    struct bmp5_dev bmp5_dev_;
    struct bmp5_osr_odr_press_config osr_odr_press_cfg_;

    uint8_t dev_addr_;
    uint32_t lastSuccessMs_ = 0;
    float currentPressureHpa_ = 0.0f;
    float currentTemperatureC_ = 0.0f;
    bool isStale_ = true;
    bool hasValidData_ = false;

    core::DeviceState state_ = core::DeviceState::Unknown;
    core::ErrorCode lastError_ = core::ErrorCode::None;

    uint32_t consecutiveErrors_ = 0;
    uint32_t resetCount_ = 0;
    uint32_t nextReinitMs_ = 0;

    bool reinitSensor(uint32_t nowMs);
    void changeState(core::DeviceState newState);

    static int8_t i2c_read(uint8_t reg_addr, uint8_t *reg_data, uint32_t length, void *intf_ptr);
    static int8_t i2c_write(uint8_t reg_addr, const uint8_t *reg_data, uint32_t length, void *intf_ptr);
    static void delay_us(uint32_t period, void *intf_ptr);
};

} // namespace sensors
} // namespace drivers
